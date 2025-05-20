/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core-util.h>
#include <pulsecore/dbus-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/protocol-dbus.h>
#include <pulsecore/thread.h>
#include <pulsecore/shared.h>

#include "SoundTriggerUtils.h"
#include "PalApi.h"
#include "PalDefs.h"
#include "pal-voiceui-utils.h"
#include "agm/agm_api.h"

#define OK 0
#define PAL_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/qsthw"
#define PAL_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Qsthw"
#define PAL_DBUS_SESSION_IFACE "org.PulseAudio.Ext.Qsthw.Session"
#define PA_DBUS_PAL_MODULE_IFACE_VERSION 0x101
#define MAX_ACD_NUMBER_OF_CONTEXT 10

PA_MODULE_AUTHOR("QTI");
PA_MODULE_DESCRIPTION("pal voiceui card module");
PA_MODULE_VERSION(PA_PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

static const uint8_t chmap[] = {PAL_CHMAP_CHANNEL_FL, PAL_CHMAP_CHANNEL_FR, PAL_CHMAP_CHANNEL_C, PAL_CHMAP_CHANNEL_LS,
             PAL_CHMAP_CHANNEL_RS, PAL_CHMAP_CHANNEL_LFE, PAL_CHMAP_CHANNEL_LB, PAL_CHMAP_CHANNEL_RB };

static const char* const valid_modargs[] = {
    "module",
    NULL,
};

enum {
    PAL_THREAD_IDLE,
    PAL_THREAD_READ_QUEUED,
    PAL_THREAD_EXIT,
    PAL_THREAD_STOP_BUFFERING
};

struct pal_voiceui_module_data {
    pa_module *module;
    pa_modargs *modargs;

    char *module_name;
    char *obj_path;
    pa_dbus_protocol *dbus_protocol;
    pa_pal_voiceui_hooks *pal;
    bool is_session_started;
    uint32_t session_id;
};

struct pal_voiceui_session_data {
    struct pal_voiceui_module_data *common;
    pal_stream_handle_t *ses_handle;
    char *obj_path;
    int thread_state;
    struct pal_buffer *read_buf;
    unsigned int read_bytes;
    bool recognition_started;
    pa_thread *async_thread;
    pa_mutex *mutex;
    pa_cond *cond;
    pal_stream_type_t type;
};

struct pal_doa {
    int target_angle_L16[2];
    int interf_angle_L16[2];
    int8_t polarActivityGUI[360];
};

static int unload_sm(DBusConnection *conn, struct pal_voiceui_session_data *ses_data);
static void load_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void unload_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void start_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void start_recognition_v2(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void stop_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_buffer_size(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void stop_buffering(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void request_read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_param_data(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void get_interface_version(DBusConnection *conn, DBusMessage *msg, void *userdata);
void pa__done(pa_module *m);

enum module_handler_index {
    MODULE_HANDLER_LOAD_SOUND_MODEL,
    MODULE_HANDLER_GET_INTERFACE_VERSION,
    MODULE_HANDLER_MAX
};

enum session_handler_index {
    SESSION_HANDLER_UNLOAD_SOUND_MODEL,
    SESSION_HANDLER_START_RECOGNITION,
    SESSION_HANDLER_START_RECOGNITION_V2,
    SESSION_HANDLER_STOP_RECOGNITION,
    SESSION_HANDLER_GET_BUFFER_SIZE,
    SESSION_HANDLER_READ_BUFFER,
    SESSION_HANDLER_STOP_BUFFERING,
    SESSION_HANDLER_REQUEST_READ_BUFFER,
    SESSION_HANDLER_GET_PARAM_DATA,
    SESSION_HANDLER_MAX
};

pa_dbus_arg_info load_sound_model_args[] = {
    {"sound_model", "((i(uuuu)(uqqqay)(uqqqay))a(uuauss))","in"},
    {"opaque_data", "ay", "in"},
    {"object_path", "o","out"}
};

pa_dbus_arg_info unload_sound_model_args[] = {
};

pa_dbus_arg_info start_recognition_args[] = {
    {"recognition_config", "(iuba(uuua(uu)))","in"},
    {"opaque_data", "ay", "in"},
};

pa_dbus_arg_info start_recognition_v2_args[] = {
    {"recognition_config", "(ia(uuu))","in"},
    {"opaque_data", "ay", "in"},
};
pa_dbus_arg_info stop_recognition_args[] = {
};

pa_dbus_arg_info get_buffer_size_args[] = {
    {"buffer_size", "i", "out"},
};

pa_dbus_arg_info read_buffer_args[] = {
    {"bytes", "u", "in"},
    {"buf", "ay", "out"},
};

pa_dbus_arg_info stop_buffering_args[] = {
};

pa_dbus_arg_info request_read_buffer_args[] = {
    {"bytes", "u", "in"},
};

pa_dbus_arg_info get_param_data_args[] = {
    {"param", "s", "in"},
    {"payload", "ay", "out"},
};

pa_dbus_arg_info get_interface_version_args[] = {
    {"version", "i", "out"},
};

/* recognition config event.
 * XXX Skipped unused offload_info in audio_config as it is
 * used for compressed offload playback streams.
 */
pa_dbus_arg_info detection_event_args[] = {
    {"recognition_event", "(iiibiiib(uuuu))a(uuua(uu))t", NULL},
    {"opaque_data", "ay", NULL}
};

pa_dbus_arg_info read_buffer_available_event_args[] = {
    {"read_buffer_sequence", "u", NULL},
    {"read_status", "i", NULL},
    {"read_buffer", "ay", NULL}
};

pa_dbus_arg_info stop_buffering_done_event_args[] = {
    {"status", "i", NULL},
};

static pa_dbus_method_handler pal_voiceui_module_handlers[MODULE_HANDLER_MAX] = {
    [MODULE_HANDLER_LOAD_SOUND_MODEL] = {
        .method_name = "LoadSoundModel",
        .arguments = load_sound_model_args,
        .n_arguments = sizeof(load_sound_model_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = load_sound_model},
    [MODULE_HANDLER_GET_INTERFACE_VERSION] = {
        .method_name = "GetInterfaceVersion",
        .arguments = get_interface_version_args,
        .n_arguments = sizeof(get_interface_version_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_interface_version},
};

static pa_dbus_method_handler pal_voiceui_session_handlers[SESSION_HANDLER_MAX] = {
    [SESSION_HANDLER_UNLOAD_SOUND_MODEL] = {
        .method_name = "UnloadSoundModel",
        .arguments = unload_sound_model_args,
        .n_arguments = sizeof(unload_sound_model_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = unload_sound_model},
    [SESSION_HANDLER_START_RECOGNITION] = {
        .method_name = "StartRecognition",
        .arguments = start_recognition_args,
        .n_arguments = sizeof(start_recognition_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = start_recognition},
    [SESSION_HANDLER_START_RECOGNITION_V2] = {
        .method_name = "StartRecognition_v2",
        .arguments = start_recognition_v2_args,
        .n_arguments = sizeof(start_recognition_v2_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = start_recognition_v2},
    [SESSION_HANDLER_STOP_RECOGNITION] = {
        .method_name = "StopRecognition",
        .arguments = stop_recognition_args,
        .n_arguments = sizeof(stop_recognition_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = stop_recognition},
    [SESSION_HANDLER_GET_BUFFER_SIZE] = {
        .method_name = "GetBufferSize",
        .arguments = get_buffer_size_args,
        .n_arguments = sizeof(get_buffer_size_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_buffer_size},
    [SESSION_HANDLER_READ_BUFFER] = {
        .method_name = "ReadBuffer",
        .arguments = read_buffer_args,
        .n_arguments = sizeof(read_buffer_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = read_buffer},
    [SESSION_HANDLER_STOP_BUFFERING] = {
        .method_name = "StopBuffering",
        .arguments = stop_buffering_args,
        .n_arguments = sizeof(stop_buffering_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = stop_buffering},
    [SESSION_HANDLER_REQUEST_READ_BUFFER] = {
        .method_name = "RequestReadBuffer",
        .arguments = request_read_buffer_args,
        .n_arguments = sizeof(request_read_buffer_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = request_read_buffer},
    [SESSION_HANDLER_GET_PARAM_DATA] = {
        .method_name = "GetParamData",
        .arguments = get_param_data_args,
        .n_arguments = sizeof(get_param_data_args)/sizeof(pa_dbus_arg_info),
        .receive_cb = get_param_data},
};

enum signal_index {
    SIGNAL_DETECTION_EVENT,
    SIGNAL_READ_BUFFER_AVAILABLE_EVENT,
    SIGNAL_STOP_BUFFERING_DONE_EVENT,
    SIGNAL_MAX
};

static pa_dbus_signal_info det_event_signals[SIGNAL_MAX] = {
    [SIGNAL_DETECTION_EVENT] = {
        .name = "DetectionEvent",
        .arguments = detection_event_args,
        .n_arguments = sizeof(detection_event_args)/sizeof(pa_dbus_arg_info)},
    [SIGNAL_READ_BUFFER_AVAILABLE_EVENT] = {
        .name = "ReadBufferAvailableEvent",
        .arguments = read_buffer_available_event_args,
        .n_arguments = sizeof(read_buffer_available_event_args)/sizeof(pa_dbus_arg_info)},
    [SIGNAL_STOP_BUFFERING_DONE_EVENT] = {
        .name = "StopBufferingDoneEvent",
        .arguments = stop_buffering_done_event_args,
        .n_arguments = sizeof(stop_buffering_done_event_args)/sizeof(pa_dbus_arg_info)},
};

static pa_dbus_interface_info module_interface_info = {
    .name = PAL_DBUS_MODULE_IFACE,
    .method_handlers = pal_voiceui_module_handlers,
    .n_method_handlers = MODULE_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = NULL,
    .n_signals = 0
};

static pa_dbus_interface_info session_interface_info = {
    .name = PAL_DBUS_SESSION_IFACE,
    .method_handlers = pal_voiceui_session_handlers,
    .n_method_handlers = SESSION_HANDLER_MAX,
    .property_handlers = NULL,
    .n_property_handlers = 0,
    .get_all_properties_cb = NULL,
    .signals = det_event_signals,
    .n_signals = SIGNAL_MAX
};

static void signal_read_buffer_available(struct pal_voiceui_session_data *ses_data,
                                         unsigned int read_buffer_sequence, int status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, array_i;

    pa_log_info("Posting read buffer available, seq %u, status %d",
                 read_buffer_sequence, status);

    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path,
            session_interface_info.name,
            det_event_signals[SIGNAL_READ_BUFFER_AVAILABLE_EVENT].name));
    dbus_message_iter_init_append(message, &arg_i);

    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT32, &read_buffer_sequence);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_INT32, &status);

    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE,
                                         &ses_data->read_buf->buffer, ses_data->read_bytes);
    dbus_message_iter_close_container(&arg_i, &array_i);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);
    dbus_message_unref(message);
}

static void pa_pal_fill_default_acd_stream_attributes(struct pal_stream_attributes *stream_attr,
                                                      uint32_t *no_of_devices,
                                                      struct pal_device *devices) {
    pa_assert(stream_attr);
    pa_assert(devices);

    stream_attr->type = PAL_STREAM_ACD;
    stream_attr->info.voice_rec_info.version = 1;
    stream_attr->info.opt_stream_info.duration_us = 4000;
    stream_attr->info.opt_stream_info.has_video = false;
    stream_attr->info.opt_stream_info.is_streaming = false;
    stream_attr->info.voice_rec_info.record_direction = PAL_AUDIO_INPUT;
    stream_attr->flags = 0;
    stream_attr->direction = PAL_AUDIO_INPUT;
    stream_attr->in_media_config.sample_rate = 16000;
    stream_attr->in_media_config.bit_width = 16;
    stream_attr->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
    stream_attr->in_media_config.ch_info.channels = 1;

    *no_of_devices = 1;

    devices->id = PAL_DEVICE_IN_HANDSET_VA_MIC;
    devices->config.sample_rate = 16000;
    devices->config.bit_width = 16;
    devices->config.ch_info.channels = 1;
    memcpy(&devices->config.ch_info.ch_map, chmap, sizeof(chmap));
}

static void pa_pal_fill_default_attributes(struct pal_stream_attributes *stream_attr, uint32_t *no_of_devices,
                                          struct pal_device *devices) {
    pa_assert(stream_attr);
    pa_assert(devices);

    stream_attr->type = PAL_STREAM_VOICE_UI;
    stream_attr->info.voice_rec_info.version = 1;
    stream_attr->info.opt_stream_info.duration_us = 4000;
    stream_attr->info.opt_stream_info.has_video = false;
    stream_attr->info.opt_stream_info.is_streaming = false;
    stream_attr->info.voice_rec_info.record_direction = PAL_AUDIO_INPUT;
    stream_attr->flags = 0;
    stream_attr->direction = PAL_AUDIO_INPUT;
    stream_attr->in_media_config.sample_rate = 16000;
    stream_attr->in_media_config.bit_width = 16;
    stream_attr->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
    stream_attr->in_media_config.ch_info.channels = 1;

    *no_of_devices = 1;

    devices->id = PAL_DEVICE_IN_HANDSET_VA_MIC;
    devices->config.sample_rate = 48000;
    devices->config.bit_width = 16;
    devices->config.ch_info.channels = 1;
    memcpy(&devices->config.ch_info.ch_map, chmap, sizeof(chmap));
}

static void signal_stop_buffering_done(struct pal_voiceui_session_data *ses_data,
                                       int status) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i;
    uint32_t sm_handle = ses_data->common->session_id;

    pa_log_info("Posting stop buffering done for handle %d with status %d", sm_handle, status);

    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path,
                                            session_interface_info.name,
                                            det_event_signals[SIGNAL_STOP_BUFFERING_DONE_EVENT].name));

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_INT32, &status);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);
    dbus_message_unref(message);
}

static void async_thread_func(void *userdata) {
    struct pal_voiceui_session_data *ses_data = (struct pal_voiceui_session_data *)userdata;
    uint32_t sm_handle = ses_data->common->session_id;
    unsigned int bytes = 0, read_buffer_sequence = 0;
    int ret = 0;

    pa_log_debug("[%d]Starting Async Thread", sm_handle);

    pa_mutex_lock(ses_data->mutex);
    while (ses_data->thread_state != PAL_THREAD_EXIT) {
        pa_log_debug("[%d]Async Thread wait", sm_handle);
        pa_cond_wait(ses_data->cond, ses_data->mutex);
        pa_log_debug("[%d]Async Thread wakeup", sm_handle);

        if (ses_data->thread_state == PAL_THREAD_STOP_BUFFERING) {
            pa_mutex_unlock(ses_data->mutex);
            pa_log_debug("[%d]Stop buffering", sm_handle);

            if (ses_data->recognition_started) {
                ret = pal_stream_stop(ses_data->ses_handle);
                ses_data->recognition_started = false;

                if (ret)
                    pa_log_debug("[%d]Stop buffering failed with error %d", sm_handle, ret);
            }

            pa_mutex_lock(ses_data->mutex);
            signal_stop_buffering_done(ses_data, ret);
        }

        if (ses_data->thread_state != PAL_THREAD_READ_QUEUED)
            continue;

        if (ses_data->read_buf == NULL || ses_data->read_bytes != bytes) {
            if (ses_data->read_buf)
                pa_xfree(ses_data->read_buf);
            ses_data->read_buf = (struct pal_buffer *)pa_xmalloc0(sizeof(struct pal_buffer));
            ses_data->read_buf->buffer = pa_xmalloc0(ses_data->read_bytes);
            ses_data->read_buf->size = ses_data->read_bytes;
            bytes = ses_data->read_bytes;
        }

        pa_mutex_unlock(ses_data->mutex);
        ret = pal_stream_read(ses_data->ses_handle, ses_data->read_buf);

        if (ret <= 0) {
            ret = -ENODATA;
            pa_log_debug("[%d]Read failed with error %d", sm_handle, ret);
        }

        pa_mutex_lock(ses_data->mutex);

        if (ses_data->thread_state == PAL_THREAD_READ_QUEUED) {
            signal_read_buffer_available(ses_data, ++read_buffer_sequence, ret);
            ses_data->thread_state = PAL_THREAD_IDLE;
        } else if (ses_data->thread_state == PAL_THREAD_STOP_BUFFERING) {
            pa_mutex_unlock(ses_data->mutex);

            if (ses_data->recognition_started) {
                ret = pal_stream_stop(ses_data->ses_handle);
                ses_data->recognition_started = false;

                if (ret)
                    pa_log_debug("[%d]Stop buffering failed with error %d", sm_handle, ret);
            }

            pa_mutex_lock(ses_data->mutex);
            signal_stop_buffering_done(ses_data, ret);
        }
    }
    pa_xfree(ses_data->read_buf);
    ses_data->read_buf = NULL;
    pa_mutex_unlock(ses_data->mutex);

    pa_log_debug("[%d]Exiting Async Thread", sm_handle);
}

/* As of now pal is not filling event and cookie data. Hence just a log */
static int32_t event_callback(pal_stream_handle_t *stream_handle, uint32_t event_id, uint32_t *event_data, uint32_t event_size, uint64_t cookie) {
    DBusMessage *message = NULL;
    DBusMessageIter arg_i, struct_i, struct_ii, array_i, array_ii;
    dbus_uint32_t i, j;
    uint32_t frame_count = 0;

    struct pal_voiceui_session_data *ses_data = (struct pal_voiceui_session_data *)cookie;
    pa_pal_st_phrase_recognition_event *pal_event;
    struct pal_st_recognition_event *event;
    struct pal_st_phrase_recognition_event *phrase_event;
    int n_elements = 0;
    char *value = NULL;
    dbus_bool_t capture_available;
    dbus_bool_t trigger_in_data;
    uint32_t channels;
    struct st_param_header* st_param_header_ptr = NULL;
    struct acd_context_event* acd_context_event_ptr = NULL;
    struct acd_per_context_event_info* event_info_ptr = NULL;

    pa_assert(event_data);
    pa_assert(ses_data);

    if (ses_data->type == PAL_STREAM_ACD) {
        event = (struct pal_st_recognition_event*) event_data;
        st_param_header_ptr = (struct st_param_header*)((uint8_t *)event +
                                                     sizeof(struct pal_st_recognition_event));
        acd_context_event_ptr = (struct acd_context_event*)((uint8_t *)st_param_header_ptr +
                                                     sizeof(struct st_param_header));
        event_info_ptr = (struct acd_per_context_event_info*)((uint8_t *)acd_context_event_ptr +
                                                     sizeof(struct acd_context_event));
        if(acd_context_event_ptr->num_contexts > MAX_ACD_NUMBER_OF_CONTEXT)
            acd_context_event_ptr->num_contexts = MAX_ACD_NUMBER_OF_CONTEXT;
    } else {
        pal_event = (pa_pal_st_phrase_recognition_event *)((void *)event_data);
        phrase_event = &pal_event->phrase_event;
        event = &phrase_event->common;
    }
    capture_available = event->capture_available;
    trigger_in_data = event->trigger_in_data;
    channels = event->media_config.ch_info.channels;

    pa_log_info("Callback event received: %d", event->status);

    ses_data->thread_state = PAL_THREAD_IDLE;
    pa_assert_se(message = dbus_message_new_signal(ses_data->obj_path,
            session_interface_info.name,
            det_event_signals[SIGNAL_DETECTION_EVENT].name));

    dbus_message_iter_init_append(message, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_STRUCT, NULL, &struct_i);

    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->status);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->type);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &ses_data->common->session_id);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &capture_available);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->capture_session);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->capture_delay_ms);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_INT32, &event->capture_preamble_ms);
    dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_BOOLEAN, &trigger_in_data);
    dbus_message_iter_open_container(&struct_i, DBUS_TYPE_STRUCT, NULL, &struct_ii);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &event->media_config.sample_rate);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &channels);
    dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &event->media_config.aud_fmt_id);
    if (ses_data->type == PAL_STREAM_ACD) {
        dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &acd_context_event_ptr->num_contexts);
    } else {
        dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32, &frame_count);
    }
    dbus_message_iter_close_container(&struct_i, &struct_ii);
    dbus_message_iter_close_container(&arg_i, &struct_i);

    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "(uuua(uu))", &array_i);
    if (ses_data->type == PAL_STREAM_ACD) {

        for (i = 0; i < acd_context_event_ptr->num_contexts; i++) {
            dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
            dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                 &event_info_ptr->context_id);
            dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                 &event_info_ptr->event_type);
            dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                 &event_info_ptr->confidence_score);
            dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "(uu)", &array_ii);
            int user_num = 1;
            for (j = 0; j < user_num; j++) {
                dbus_message_iter_open_container(&array_ii, DBUS_TYPE_STRUCT, NULL, &struct_ii);
                dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32,&event_info_ptr->context_id);
                dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32,&event_info_ptr->confidence_score);
                dbus_message_iter_close_container(&array_ii, &struct_ii);
            }
            event_info_ptr = (struct acd_per_context_event_info*)(event_info_ptr +
                 sizeof(struct acd_per_context_event_info));
            dbus_message_iter_close_container(&struct_i, &array_ii);
            dbus_message_iter_close_container(&array_i, &struct_i);
        }
    } else {
        for (i = 0; i < phrase_event->num_phrases; i++) {
            dbus_message_iter_open_container(&array_i, DBUS_TYPE_STRUCT, NULL, &struct_i);
            dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                &phrase_event->phrase_extras[i].id);
            dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                &phrase_event->phrase_extras[i].recognition_modes);
            dbus_message_iter_append_basic(&struct_i, DBUS_TYPE_UINT32,
                &phrase_event->phrase_extras[i].confidence_level);

            dbus_message_iter_open_container(&struct_i, DBUS_TYPE_ARRAY, "(uu)", &array_ii);
            for (j = 0; j < phrase_event->phrase_extras[i].num_levels; j++) {
                dbus_message_iter_open_container(&array_ii, DBUS_TYPE_STRUCT, NULL, &struct_ii);
                dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32,
                    &phrase_event->phrase_extras[i].levels[j].user_id);
                dbus_message_iter_append_basic(&struct_ii, DBUS_TYPE_UINT32,
                    &phrase_event->phrase_extras[i].levels[j].level);
                dbus_message_iter_close_container(&array_ii, &struct_ii);
            }
            dbus_message_iter_close_container(&struct_i, &array_ii);
            dbus_message_iter_close_container(&array_i, &struct_i);
        }
    }
    dbus_message_iter_close_container(&arg_i, &array_i);
    if (ses_data->type == PAL_STREAM_ACD) {
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT64, &event_info_ptr->detection_ts);
        n_elements = 1;
        value = (char*)event_info_ptr;
    } else {
        dbus_message_iter_append_basic(&arg_i, DBUS_TYPE_UINT64, &pal_event->timestamp);
        value = (char*)pal_event + event->data_offset;
        n_elements = event->data_size;
    }
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &value, n_elements);
    dbus_message_iter_close_container(&arg_i, &array_i);

    pa_dbus_protocol_send_signal(ses_data->common->dbus_protocol, message);

    if(event->capture_available) {
        ses_data->common->is_session_started = true;
        pa_hook_fire(&ses_data->common->pal->hooks[PA_HOOK_PAL_VOICEUI_START_DETECTION],NULL);
    }

    dbus_message_unref(message);
    return 0;
}
/* Add support fot this once it's available in PAL
static void get_version(DBusConnection *conn, DBusMessage *msg, void *userdata){
    int version;
    char *minor;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("get_version");
    version = (int) strtof(qsthw_get_version(), &minor);
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &version);
} */

static void get_interface_version(DBusConnection *conn, DBusMessage *msg, void *userdata){
    int version;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("get_interface_version(%d)", PA_DBUS_PAL_MODULE_IFACE_VERSION);
    version = PA_DBUS_PAL_MODULE_IFACE_VERSION;
    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &version);
}

static void get_param_data(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = userdata;
    int status = 0;
    DBusError error;
    const char *param;
    pal_param_id_type_t param_id;
    pal_param_payload *payload = NULL;
    size_t payload_size = 0;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;
    struct ffv_doa_tracking_monitor_t *doa = NULL;
    struct pal_doa *doa_final = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &param,
                               DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    if (strcmp("st_direction_of_arrival", param) == 0) {
        param_id = PAL_PARAM_ID_DIRECTION_OF_ARRIVAL;
        payload_size = sizeof(struct pal_doa);
        doa_final = (struct pal_doa *)malloc(sizeof(struct pal_doa));
        if (!doa_final) {
            pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data failed, failed to allocate memory");
            dbus_error_free(&error);
            return;
        }
    } else {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data failed, unsupported param");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("get param data");
    status = pal_stream_get_param(ses_data->ses_handle,
                                  (uint32_t)param_id, &payload);

    if (OK != status) {
        free(payload);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param_data failed");
        dbus_error_free(&error);
        return;
    }

    doa = (struct ffv_doa_tracking_monitor_t *)((void *)payload);
    doa_final->target_angle_L16[0] = doa->target_angle_L16[0];
    doa_final->target_angle_L16[1] = doa->target_angle_L16[1];
    doa_final->interf_angle_L16[0] = doa->interf_angle_L16[0];
    doa_final->interf_angle_L16[1] = doa->interf_angle_L16[1];
    memcpy(doa_final->polarActivityGUI, doa->polarActivityGUI, sizeof(int8_t)*360);

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, (void **)&doa_final,
                                         payload_size);
    dbus_message_iter_close_container(&arg_i, &array_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    dbus_message_unref(reply);
}

static DBusHandlerResult disconnection_filter_cb(DBusConnection *conn,
                              DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = userdata;
    int rc = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    if (dbus_message_is_signal(msg, "org.freedesktop.DBus.Local", "Disconnected")) {
        /* connection died, unload the session for which callback got triggered */
        pa_log_info("connection died for session\n");
        if (ses_data->recognition_started) {
            rc = pal_stream_stop(ses_data->ses_handle);
            ses_data->recognition_started = false;

            if (rc)
                pa_log_error("%s: pal_stream_stop failed %d\n", __func__, rc);
        }

        rc = unload_sm(conn, ses_data);

        if (rc)
            pa_log_error("%s: unload_sm failed %d\n", __func__, rc);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int unload_sm(DBusConnection *conn, struct pal_voiceui_session_data *ses_data) {
    int status = 0;

    dbus_connection_remove_filter(conn, disconnection_filter_cb, ses_data);
    status = pal_stream_close(ses_data->ses_handle);

    pa_assert_se(pa_dbus_protocol_remove_interface(ses_data->common->dbus_protocol,
            ses_data->obj_path, session_interface_info.name) >= 0);

    pa_xfree(ses_data->obj_path);
    pa_xfree(ses_data);

    return status;
}

static void request_read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = (struct pal_voiceui_session_data *)userdata;
    unsigned int bytes;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32,
                               &bytes, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    pa_mutex_lock(ses_data->mutex);
    if (bytes == 0 || ses_data->async_thread == NULL ||
        ses_data->thread_state != PAL_THREAD_IDLE) {
        pa_mutex_unlock(ses_data->mutex);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "request_read_buffer failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->thread_state = PAL_THREAD_READ_QUEUED;
    ses_data->read_bytes = bytes;
    pa_cond_signal(ses_data->cond, 0);
    pa_mutex_unlock(ses_data->mutex);

    pa_dbus_send_empty_reply(conn, msg);
}

static void stop_buffering(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = userdata;
    int status = 0;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("stop buffering");
    pa_mutex_lock(ses_data->mutex);

    if (ses_data->recognition_started) {
        status = pal_stream_stop(ses_data->ses_handle);
        ses_data->recognition_started = false;
    }

    if (status) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "stop_buffering failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->thread_state = PAL_THREAD_STOP_BUFFERING;
    pa_cond_signal(ses_data->cond, 0);
    pa_mutex_unlock(ses_data->mutex);

    pa_dbus_send_empty_reply(conn, msg);
}

static void read_buffer(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = userdata;
    int ret = 0;
    DBusError error;
    unsigned int bytes;
    struct pal_buffer in_buffer;
    DBusMessage *reply = NULL;
    DBusMessageIter arg_i, array_i;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    if (!dbus_message_get_args(msg, &error, DBUS_TYPE_UINT32,
                               &bytes, DBUS_TYPE_INVALID)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
        dbus_error_free(&error);
        return;
    }

    memset(&in_buffer, 0, sizeof(struct pal_buffer));
    in_buffer.size = bytes;
    in_buffer.buffer = pa_xmalloc0(bytes);
    ret = pal_stream_read(ses_data->ses_handle, &in_buffer);
    if (ret < 0) {
        pa_xfree(in_buffer.buffer);
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "read_buffer failed");
        dbus_error_free(&error);
        return;
    }

    pa_assert_se((reply = dbus_message_new_method_return(msg)));
    dbus_message_iter_init_append(reply, &arg_i);
    dbus_message_iter_open_container(&arg_i, DBUS_TYPE_ARRAY, "y", &array_i);
    dbus_message_iter_append_fixed_array(&array_i, DBUS_TYPE_BYTE, &in_buffer.buffer, ret);
    dbus_message_iter_close_container(&arg_i, &array_i);
    pa_assert_se(dbus_connection_send(conn, reply, NULL));

    pa_xfree(in_buffer.buffer);
    dbus_message_unref(reply);
}

static void get_buffer_size(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    int buffer_size;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("get buffer size");
    buffer_size = 3840; /* Fixme: Modify this once pal_stream_get_buffer_size is implemented */

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_INT32, &buffer_size);
}

static void stop_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = userdata;
    int status = 0;
    DBusError error;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    pa_log_debug("stop recognition");

    if (ses_data->recognition_started) {
        status = pal_stream_stop(ses_data->ses_handle);
        ses_data->recognition_started = false;
    }

    if (status != 0) {
        pa_log_error("pal stream stop failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "stop_recognition failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

/* start_recognition_v2 api is for ACD, since ACD requires diffrent arguments to be passed
    compared VOICEUI usecase.
*/
static void start_recognition_v2(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = (struct pal_voiceui_session_data *)userdata;
    DBusError error;
    DBusMessageIter arg_i, struct_i, struct_ii, array_i;
    dbus_int32_t i, j, status = 0;
    int  arg_type;
    int num_contexts = 1;
    struct pal_st_recognition_config  *rec_config = NULL;
    uint32_t rec_config_size = 0;
    pal_param_payload *rec_config_payload = NULL;
    struct st_param_header * st_param_header_instance = NULL;
    struct acd_recognition_cfg *acd_recognition_cfg_instance = NULL;
    struct acd_per_context_cfg *context_cfg_ptr = NULL;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    pa_log_debug("start recognition");
    dbus_error_init(&error);

    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "start_recognition has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(ia(uuu))ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "Invalid signature for start_recognition");
        dbus_error_free(&error);
        return;
    }

    if (ses_data->common->is_session_started) {
        pa_hook_fire(&ses_data->common->pal->hooks[PA_HOOK_PAL_VOICEUI_STOP_DETECTION],NULL);
        ses_data->common->is_session_started = false;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &num_contexts);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_recurse(&struct_i, &array_i);

    rec_config_size = sizeof(struct pal_st_recognition_config) +
        sizeof(struct st_param_header) + sizeof(struct acd_recognition_cfg) +
            num_contexts * sizeof(struct acd_per_context_cfg);
    rec_config_payload = (pal_param_payload *)pa_xmalloc0(sizeof(pal_param_payload) + rec_config_size);
    if (!rec_config_payload) {
        pa_log_error("Failed to alloc memory for recognition config\n");
        return;
    }
    rec_config_payload->payload_size = rec_config_size;
    rec_config = (struct pal_st_recognition_config *)rec_config_payload->payload;

    rec_config->data_size = rec_config_size - sizeof(struct pal_st_recognition_config);
    rec_config->data_offset = sizeof(struct pal_st_recognition_config);

    st_param_header_instance = (struct st_param_header *) ((uint8_t *)rec_config + rec_config->data_offset);
    st_param_header_instance->key_id =  ST_PARAM_KEY_CONTEXT_RECOGNITION_INFO;
    st_param_header_instance->payload_size = sizeof(struct acd_recognition_cfg) +
                                        num_contexts * sizeof(struct acd_per_context_cfg);

    // construct acd_recognition_cfg
    acd_recognition_cfg_instance = (struct acd_recognition_cfg *) ((uint8_t *)st_param_header_instance +
                                    sizeof(struct st_param_header));
    acd_recognition_cfg_instance->version = 0x1;
    acd_recognition_cfg_instance->num_contexts = num_contexts;
    i = 1;
    context_cfg_ptr = (struct acd_per_context_cfg *) ((uint8_t *)acd_recognition_cfg_instance +
                    sizeof(struct acd_recognition_cfg));
    while (((arg_type = dbus_message_iter_get_arg_type(&array_i)) !=
                        DBUS_TYPE_INVALID) &&(i <= num_contexts)) {
        dbus_message_iter_recurse(&array_i, &struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &context_cfg_ptr->context_id);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &context_cfg_ptr->step_size);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &context_cfg_ptr->threshold);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_next(&array_i);
        context_cfg_ptr = (struct acd_per_context_cfg *)((uint8_t *)context_cfg_ptr + sizeof(struct acd_per_context_cfg));
        i++;
    }
    status = pal_stream_set_param(ses_data->ses_handle, PAL_PARAM_ID_RECOGNITION_CONFIG, rec_config_payload);
    pa_xfree(rec_config_payload);

    if (status != 0) {
        pa_log_error("param PAL_PARAM_ID_START_RECOGNITION set failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "start_recognition failed");
        dbus_error_free(&error);
        return;
    }

    status = pal_stream_start(ses_data->ses_handle);

    if (status != 0) {
        pa_log_error("pal_stream_start failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "start_recognition failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->recognition_started = true;
    pa_dbus_send_empty_reply(conn, msg);
}

/* Call pal_stream_set_param followed by pal_stream_start */
static void start_recognition(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = (struct pal_voiceui_session_data *)userdata;
    struct pal_st_recognition_config config = {0, };
    struct pal_st_recognition_config *rc_config = NULL;
    pal_param_payload *prm_payload = NULL;
    dbus_int32_t rc_config_size;
    DBusError error;
    DBusMessageIter arg_i, struct_i, struct_ii, struct_iii, array_i, sub_array_i;
    dbus_int32_t i, j, status = 0;
    int n_elements = 0, arg_type;
    char *value = NULL;
    char **addr_value = &value;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "start_recognition has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg), "(iuba(uuua(uu)))ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "Invalid signature for start_recognition");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("start recognition");
    if (ses_data->common->is_session_started) {
        pa_hook_fire(&ses_data->common->pal->hooks[PA_HOOK_PAL_VOICEUI_STOP_DETECTION],NULL);
        ses_data->common->is_session_started = false;
    }

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_get_basic(&struct_i, &config.capture_handle);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &config.capture_device);
    dbus_message_iter_next(&struct_i);
    dbus_message_iter_get_basic(&struct_i, &config.capture_requested);
    dbus_message_iter_next(&struct_i);

    dbus_message_iter_recurse(&struct_i, &array_i);
    i = 0;
    while (((arg_type = dbus_message_iter_get_arg_type(&array_i)) !=
                        DBUS_TYPE_INVALID) &&
            (config.num_phrases < PAL_SOUND_TRIGGER_MAX_PHRASES)) {
        dbus_message_iter_recurse(&array_i, &struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &config.phrases[i].id);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &config.phrases[i].recognition_modes);
        dbus_message_iter_next(&struct_ii);
        dbus_message_iter_get_basic(&struct_ii, &config.phrases[i].confidence_level);
        dbus_message_iter_next(&struct_ii);

        dbus_message_iter_recurse(&struct_ii, &sub_array_i);
        j = 0;
        while (((arg_type = dbus_message_iter_get_arg_type(&sub_array_i)) !=
                            DBUS_TYPE_INVALID) &&
                (config.phrases[i].num_levels < PAL_SOUND_TRIGGER_MAX_USERS)) {
            dbus_message_iter_recurse(&sub_array_i, &struct_iii);
            dbus_message_iter_get_basic(&struct_iii,
                 &config.phrases[i].levels[j].user_id);
            dbus_message_iter_next(&struct_iii);
            dbus_message_iter_get_basic(&struct_iii,
                 &config.phrases[i].levels[j].level);
            config.phrases[i].num_levels++;
            j++;
            dbus_message_iter_next(&sub_array_i);
        }
        config.num_phrases++;
        i++;
        dbus_message_iter_next(&array_i);
    }
    /* read data size and data offset */
    dbus_message_iter_next(&arg_i);
    dbus_message_iter_recurse(&arg_i, &array_i);
    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    config.data_size = n_elements;
    config.data_offset = sizeof(config);

    rc_config_size = sizeof(struct pal_st_recognition_config) + config.data_size;
    prm_payload = (pal_param_payload *) pa_xmalloc0(sizeof(pal_param_payload) + rc_config_size);
    prm_payload->payload_size = sizeof(pal_param_payload) + rc_config_size;
    rc_config = (struct pal_st_recognition_config *) prm_payload->payload;
    memcpy(rc_config, &config, sizeof(struct pal_st_recognition_config));
    memcpy((char *)rc_config + rc_config->data_offset,
           value, n_elements);
    rc_config->callback = NULL;
    rc_config->cookie = (void *)ses_data;

    status = pal_stream_set_param(ses_data->ses_handle, PAL_PARAM_ID_RECOGNITION_CONFIG, prm_payload);
    pa_xfree(prm_payload);

    if (status != 0) {
        pa_log_error("param PAL_PARAM_ID_START_RECOGNITION set failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "start_recognition failed");
        dbus_error_free(&error);
        return;
    }

    status = pal_stream_start(ses_data->ses_handle);

    if (status != 0) {
        pa_log_error("pal_stream_start failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "start_recognition failed");
        dbus_error_free(&error);
        return;
    }

    ses_data->recognition_started = true;
    pa_dbus_send_empty_reply(conn, msg);
}

static void unload_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_session_data *ses_data = userdata;
    DBusError error;
    int status = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);

    ses_data->thread_state = PAL_THREAD_EXIT;
    pa_cond_signal(ses_data->cond, 0);
    pa_thread_free(ses_data->async_thread);
    pa_cond_free(ses_data->cond);
    pa_mutex_free(ses_data->mutex);

    status = unload_sm(conn, ses_data);
    if (status != 0) {
        pa_log_error("pal stream close failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "unload_sound_model failed");
        dbus_error_free(&error);
        return;
    }

    pa_dbus_send_empty_reply(conn, msg);
}

/* implementations exposed by module global object path */
/* Call pal_stream_open followed by pal_stream_set_param for loading sound model */
static void load_sound_model(DBusConnection *conn, DBusMessage *msg, void *userdata) {
    struct pal_voiceui_module_data *m_data = userdata;
    struct pal_stream_attributes *stream_attr = NULL;
    struct pal_voiceui_session_data *ses_data = NULL;
    pal_param_payload *prm_payload = NULL;
    struct pal_st_phrase_sound_model phrase_sound_model = {0, };
    struct pal_st_phrase_sound_model *p_sound_model = NULL;
    struct pal_st_sound_model *common_sound_model = &phrase_sound_model.common;
    pal_stream_handle_t *stream_handle = NULL;
    DBusError error;
    DBusMessageIter arg_i, struct_i, struct_ii, struct_iii, array_i, sub_array_i;
    dbus_int32_t sm_type, sm_data_size, i, j, status = 0;
    int n_elements = 0, arg_type;
    char *value = NULL, *thread_name = NULL;
    char **addr_value = &value;
    uint32_t no_of_devices = 0;
    struct pal_device *devices = NULL;
    struct pal_channel_info *stream_ch_info = NULL, *device_ch_info = NULL;
    uint32_t no_of_modifiers = 0;
    struct modifier_kv *modifiers = NULL;
    int rc = 0;

    pa_assert(conn);
    pa_assert(msg);
    pa_assert(userdata);

    dbus_error_init(&error);
    if (!dbus_message_iter_init(msg, &arg_i)) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "load_sound_model has no arguments");
        dbus_error_free(&error);
        return;
    }

    if (!pa_streq(dbus_message_get_signature(msg),
                 "((i(uuuu)(uqqqay)(uqqqay))a(uuauss))ay")) {
        pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS,
            "Invalid signature for load_sound_model");
        dbus_error_free(&error);
        return;
    }

    pa_log_debug("load sound model");

    stream_attr = pa_xnew0(struct pal_stream_attributes, 1);
    stream_ch_info = pa_xnew0(struct pal_channel_info, 1);
    memcpy(&stream_attr->in_media_config.ch_info, stream_ch_info, sizeof(struct pal_channel_info));
    devices = pa_xnew0(struct pal_device, 1);
    device_ch_info = pa_xnew0(struct pal_channel_info, 1);
    memcpy(&devices->config.ch_info, device_ch_info, sizeof(struct pal_channel_info));

    dbus_message_iter_recurse(&arg_i, &struct_i);
    dbus_message_iter_recurse(&struct_i, &struct_ii);
    dbus_message_iter_get_basic(&struct_ii, &sm_type);
    common_sound_model->type = sm_type;
    if (sm_type == PAL_SOUND_MODEL_TYPE_GENERIC) {
        pa_pal_fill_default_acd_stream_attributes(stream_attr, &no_of_devices, devices);
        stream_attr->type = PAL_STREAM_ACD;
    } else {
        pa_pal_fill_default_attributes(stream_attr, &no_of_devices, devices);
        stream_attr->type = PAL_STREAM_VOICE_UI;
    }

    /* read sampling rate and number of channels for pal stream & pal device */
    dbus_message_iter_next(&struct_ii);
    dbus_message_iter_recurse(&struct_ii, &struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &stream_attr->in_media_config.sample_rate);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &stream_attr->in_media_config.ch_info.channels);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &devices->config.sample_rate);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &devices->config.ch_info.channels);

    /* read sound_model uuid values */
    dbus_message_iter_next(&struct_ii);
    dbus_message_iter_recurse(&struct_ii, &struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->uuid.timeLow);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->uuid.timeMid);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->uuid.timeHiAndVersion);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->uuid.clockSeq);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_recurse(&struct_iii, &array_i);

    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&common_sound_model->uuid.node[0], value, n_elements);

    /* read sound_model vendor_uuid values */
    dbus_message_iter_next(&struct_ii);
    dbus_message_iter_recurse(&struct_ii, &struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->vendor_uuid.timeLow);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->vendor_uuid.timeMid);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->vendor_uuid.timeHiAndVersion);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_get_basic(&struct_iii, &common_sound_model->vendor_uuid.clockSeq);
    dbus_message_iter_next(&struct_iii);
    dbus_message_iter_recurse(&struct_iii, &array_i);

    dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
    memcpy(&common_sound_model->vendor_uuid.node[0], value, n_elements);

    ses_data = pa_xnew0(struct pal_voiceui_session_data, 1);
    ses_data->common = (struct pal_voiceui_module_data *)userdata;
    ses_data->type = stream_attr->type;

    rc = pal_stream_open(stream_attr, no_of_devices, devices, no_of_modifiers, modifiers, event_callback, (uint64_t)ses_data, &stream_handle);
    if (rc != 0) {
        free(ses_data);
        ses_data = NULL;
        pa_log_error("pal stream open failed\n");
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "load_sound_model failed");
        dbus_error_free(&error);
        return;
    }

    if (sm_type == PAL_SOUND_MODEL_TYPE_KEYPHRASE) {
        common_sound_model->data_offset = sizeof(phrase_sound_model);

        /* read phrases fields */
        dbus_message_iter_next(&struct_i);
        dbus_message_iter_recurse(&struct_i, &array_i);
        i = 0;
        while (((arg_type = dbus_message_iter_get_arg_type(&array_i)) !=
                            DBUS_TYPE_INVALID) &&
                (phrase_sound_model.num_phrases < PAL_SOUND_TRIGGER_MAX_PHRASES)) {
            dbus_message_iter_recurse(&array_i, &struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].id);
            dbus_message_iter_next(&struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].recognition_mode);
            dbus_message_iter_next(&struct_ii);

            dbus_message_iter_recurse(&struct_ii, &sub_array_i);
            j = 0;
            while (((arg_type = dbus_message_iter_get_arg_type(&sub_array_i)) !=
                                DBUS_TYPE_INVALID) &&
                    (phrase_sound_model.phrases[i].num_users < PAL_SOUND_TRIGGER_MAX_USERS)) {
                dbus_message_iter_get_basic(&sub_array_i,
                      &phrase_sound_model.phrases[i].users[j]);
                phrase_sound_model.phrases[i].num_users++;
                j++;
                dbus_message_iter_next(&sub_array_i);
            }
            dbus_message_iter_next(&struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].locale);
            dbus_message_iter_next(&struct_ii);
            dbus_message_iter_get_basic(&struct_ii,
                &phrase_sound_model.phrases[i].text);
            phrase_sound_model.num_phrases++;
            i++;
            dbus_message_iter_next(&array_i);
        }

        /* read opaque data into sound model structure */
        dbus_message_iter_next(&arg_i);
        dbus_message_iter_recurse(&arg_i, &array_i);
        dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
        common_sound_model->data_size = n_elements;
        sm_data_size = sizeof(phrase_sound_model) + common_sound_model->data_size;
        /* Fill parsed info into param payload */
        prm_payload = (pal_param_payload *)pa_xmalloc0(sizeof(pal_param_payload) + sm_data_size);
        prm_payload->payload_size = sizeof(pal_param_payload) + sm_data_size;
        p_sound_model = (struct pal_st_phrase_sound_model *) prm_payload->payload;
        memcpy(p_sound_model, &phrase_sound_model, sizeof(phrase_sound_model));
        memcpy((char*)p_sound_model + common_sound_model->data_offset, value,
               common_sound_model->data_size);
    } else if (sm_type == PAL_SOUND_MODEL_TYPE_GENERIC) {
        common_sound_model->data_offset = sizeof(struct pal_st_sound_model);
        /*skip phrase related fields */
        dbus_message_iter_next(&arg_i);
        dbus_message_iter_recurse(&arg_i, &array_i);
        dbus_message_iter_get_fixed_array(&array_i, addr_value, &n_elements);
        sm_data_size = sizeof(struct pal_st_sound_model) + n_elements;
        prm_payload = (pal_param_payload *)pa_xmalloc0(sizeof(pal_param_payload) + sm_data_size);
        prm_payload->payload_size = sizeof(pal_param_payload) + sm_data_size;
        common_sound_model = (struct pal_st_sound_model *) prm_payload->payload;
        common_sound_model->data_size = n_elements;
        memcpy((char*)common_sound_model + common_sound_model->data_offset,
               value, common_sound_model->data_size);
    }

    status = pal_stream_set_param(stream_handle, PAL_PARAM_ID_LOAD_SOUND_MODEL, prm_payload);

    pa_xfree(prm_payload);
    prm_payload = NULL;
    p_sound_model = NULL;
    common_sound_model = NULL;
    if (status != 0) {
        free(ses_data);
        ses_data = NULL;
        pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "load_sound_model failed");
        dbus_error_free(&error);
        return;
    }

   /* After successful load sound model, allocate session data */
    ses_data->common->session_id++;
    ses_data->ses_handle = stream_handle;
    ses_data->obj_path = pa_sprintf_malloc("%s/ses_%d", m_data->obj_path, ses_data->common->session_id);
    ses_data->mutex = pa_mutex_new(false /* recursive  */, false /* inherit_priority */);
    ses_data->cond = pa_cond_new();
    ses_data->thread_state = PAL_THREAD_IDLE;
    ses_data->read_buf = NULL;
    ses_data->recognition_started = false;

    thread_name = pa_sprintf_malloc("pal read thread%d", ses_data->common->session_id);

    if (!(ses_data->async_thread = pa_thread_new(thread_name, async_thread_func, ses_data)))
        pa_log_error("%s: pal read thread creation failed", __func__);
    pa_xfree(thread_name);

    pa_assert_se(pa_dbus_protocol_add_interface(ses_data->common->dbus_protocol,
            ses_data->obj_path, &session_interface_info, ses_data) >= 0);

    pa_assert_se(dbus_connection_add_filter(conn, disconnection_filter_cb, ses_data, NULL));

    pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_OBJECT_PATH, &ses_data->obj_path);
}

int pa__init(pa_module *m) {
    struct pal_voiceui_module_data *m_data;
    pa_modargs *ma;
    int i;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        ma = NULL;
        goto error;
    }

    m->userdata = m_data = pa_xnew0(struct pal_voiceui_module_data, 1);
    m_data->pal = pa_xnew0(pa_pal_voiceui_hooks, 1);
    m_data->modargs = ma;
    m_data->module = m;
    m_data->session_id = 0;

    m_data->obj_path = pa_sprintf_malloc("%s/%s", PAL_DBUS_OBJECT_PATH_PREFIX,
                         "primary");

    m_data->dbus_protocol = pa_dbus_protocol_get(m->core);
    pa_assert_se(pa_dbus_protocol_add_interface(m_data->dbus_protocol,
            m_data->obj_path, &module_interface_info, m_data) >= 0);

    for (i = 0; i < PA_HOOK_PAL_VOICEUI_MAX; i++)
        pa_hook_init(&m_data->pal->hooks[i], NULL);

    pa_shared_set(m->core, "voice-ui-session", m_data->pal);

    return 0;

error:
    pa__done(m);
    return -1;
}

void pa__done(pa_module *m) {
    struct pal_voiceui_module_data *m_data;

    pa_assert(m);

    if (!(m_data = m->userdata))
        return;

    if (m_data->obj_path && m_data->dbus_protocol)
        pa_assert_se(pa_dbus_protocol_remove_interface(m_data->dbus_protocol,
                m_data->obj_path, module_interface_info.name) >= 0);

    if (m_data->dbus_protocol)
        pa_dbus_protocol_unref(m_data->dbus_protocol);

    if (m_data->obj_path)
        pa_xfree(m_data->obj_path);

    if (m_data->module_name)
        pa_xfree(m_data->module_name);

    if (m_data->modargs)
        pa_modargs_free(m_data->modargs);

    pa_shared_remove(m->core, "voice-ui-session");
    pa_xfree(m_data->pal);
    pa_xfree(m_data);
    m->userdata = NULL;
}
