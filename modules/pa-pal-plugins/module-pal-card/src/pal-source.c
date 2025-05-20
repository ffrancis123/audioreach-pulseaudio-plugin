/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <pulse/rtclock.h>
#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/source.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/core-format.h>
#include <pulsecore/core-util.h>
#include <pulse/util.h>

#include "pal-source.h"
#include "pal-utils.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

#define PAL_MAX_GAIN 1
#define PA_ALTERNATE_SOURCE_RATE 44100
#define PA_FORMAT_DEFAULT_SAMPLE_RATE_INDEX 0
#define PA_FORMAT_DEFAULT_SAMPLE_FORMAT_INDEX 0
#define PA_DEFAULT_SOURCE_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SOURCE_RATE 48000
#define PA_DEFAULT_SOURCE_CHANNELS 2
#define PA_NUM_DEVICES 1
#define PA_BITS_PER_BYTE 8
#define PA_DEFAULT_BUFFER_DURATION_MS 25
#define PA_LOW_LATENCY_DURATION_MS 5
#define PA_DEEP_BUFFER_DURATION_MS 20

static int restart_pal_source(pa_pal_source_data *sdata, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map);
static int create_pal_source(pa_pal_source_config *source, pa_pal_card_port_device_data *port_device_data, pa_pal_source_data *sdata);
static int close_pal_source(pa_pal_source_data *sdata);
static int open_pal_source(pa_pal_source_data *sdata);

static const uint32_t supported_source_rates[] =
                          {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};

static const  pa_sample_format_t supported_source_formats[] =
                                     {PA_SAMPLE_S16LE, PA_SAMPLE_S32LE, PA_SAMPLE_S24LE,PA_SAMPLE_S24_32LE};

static size_t source_get_buffer_size (pa_sample_spec spec,pal_stream_type_t type) {
    uint32_t buffer_duration = PA_DEFAULT_BUFFER_DURATION_MS;
    size_t length = 0;

    switch (type) {
        case PAL_STREAM_DEEP_BUFFER:
            buffer_duration = PA_DEEP_BUFFER_DURATION_MS;
            break;
        case PAL_STREAM_LOW_LATENCY:
            buffer_duration = PA_LOW_LATENCY_DURATION_MS;
        default:
            break;
    }

    length = ((spec.rate * buffer_duration * spec.channels * pa_sample_size_of_format(spec.format)) / 1000);

    return pa_frame_align(length ,&spec);
}

static bool source_check_supported_format(pa_sample_format_t format) {
   uint32_t i;
   for (i = 0; i < ARRAY_SIZE(supported_source_formats) ; i++) {
        if (format == supported_source_formats[i]) {
           return true;
	}
   }
   return false;
}

static uint32_t pa_pal_source_find_nearest_supported_sample_rate(uint32_t sample_rate) {
    uint32_t i;
    uint32_t nearest_rate = PA_DEFAULT_SOURCE_RATE;

    for (i = 0; i < ARRAY_SIZE(supported_source_rates) ; i++) {
        if (sample_rate == supported_source_rates[i]) {
            nearest_rate = sample_rate;
            break;
        } else if (sample_rate > supported_source_rates[i]) {
            nearest_rate = supported_source_rates[i];
        }
    }

    return nearest_rate;
}

static const char *pa_pal_source_get_name_from_type(pal_stream_type_t type) {
    const char *name = NULL;

    if (type == PAL_STREAM_RAW)
        name = "regular";
    else if (type == PAL_STREAM_LOW_LATENCY)
        name = "low-latency";
    else if (type == PAL_STREAM_COMPRESSED)
        name = "compress";
    else if (type == PAL_STREAM_VOIP_TX)
        name = "voip_tx";
    else if (type == PAL_STREAM_VOIP_RX)
        name = "voip_Rx";
    else if (type == PAL_STREAM_DEEP_BUFFER)
        name = "deep-buffer";

    return name;
}

static void pa_pal_source_set_volume_cb(pa_source *s) {
    pa_pal_source_data *sdata = NULL;
    float gain;
    int rc;
    pa_volume_t volume;
    pal_source_data *pal_sdata = NULL;
    struct pal_volume_data *volume_data = NULL;
    uint32_t i,no_vol_pair;
    uint32_t channel_mask = 1;

    pa_assert(s);
    sdata = (pa_pal_source_data *)s->userdata;

    if (!PA_SOURCE_IS_RUNNING(s->state)) {
        pa_log_error("set volume is supported only when source is in RUNNING state\n");
        return;
    }
    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pal_sdata->stream_handle);

    pal_sdata = sdata->pal_sdata;
    no_vol_pair = pal_sdata->stream_attributes->in_media_config.ch_info.channels;

    gain = ((float) pa_cvolume_max(&s->real_volume) * (float)PAL_MAX_GAIN) / (float)PA_VOLUME_NORM;
    volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / PAL_MAX_GAIN);

    volume_data = (struct pal_volume_data *)malloc(sizeof(uint32_t) +
                                                 (sizeof(struct pal_channel_vol_kv) * (no_vol_pair)));
    if (!volume_data) {
        pa_log_error("malloc failed for size %zu", sizeof(uint32_t) +
                 (sizeof(struct pal_channel_vol_kv) * (no_vol_pair)));
        return;
    }

    volume_data->no_of_volpair = no_vol_pair;

    for (i = 0; i < no_vol_pair; i++) {
        channel_mask = (channel_mask | pal_sdata->stream_attributes->out_media_config.ch_info.ch_map[i]);
    }

    channel_mask = (channel_mask << 1);

    for (i = 0; i < no_vol_pair; i++) {
        volume_data->volume_pair[i].channel_mask = channel_mask;
        volume_data->volume_pair[i].vol = gain;
    }

    pal_sdata->source_event_id = PA_PAL_VOLUME_APPLY;
    pa_mutex_lock(pal_sdata->mutex);
    rc = pal_stream_set_volume(pal_sdata->stream_handle, volume_data);
    pal_sdata->source_event_id = PA_PAL_NO_EVENT;
    pa_mutex_unlock(pal_sdata->mutex);
    pa_cond_signal(pal_sdata->cond_ctrl_thread, 0);
    if (rc)
        pa_log_error("pal stream : unable to set volume error %d\n", rc);
    else
        pa_cvolume_set(&s->real_volume, s->real_volume.channels, volume); /* TODO: Is this correct?  */

    pa_xfree(volume_data);
    return;
}

static int pa_pal_source_fill_info(pa_pal_source_config *source, pal_source_data *pal_sdata, pa_pal_card_port_device_data *port_device_data) {
    pa_assert(pal_sdata);

    pal_sdata->stream_attributes = pa_xnew0(struct pal_stream_attributes, 1);

    pal_sdata->stream_attributes->type = source->stream_type;
    pal_sdata->stream_attributes->info.opt_stream_info.version = 1;
    pal_sdata->stream_attributes->info.opt_stream_info.duration_us = -1;
    pal_sdata->stream_attributes->info.opt_stream_info.has_video = false;
    pal_sdata->stream_attributes->info.opt_stream_info.is_streaming = false;

    pal_sdata->stream_attributes->flags = 0;
    pal_sdata->stream_attributes->direction = PAL_AUDIO_INPUT;

    pal_sdata->stream_attributes->in_media_config.sample_rate = source->default_spec.rate;
    pal_sdata->stream_attributes->in_media_config.bit_width = pa_sample_size_of_format(source->default_spec.format) * PA_BITS_PER_BYTE;

    switch (pal_sdata->stream_attributes->in_media_config.bit_width) {
        case 32:
            pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S32_LE;
            break;
        case 24:
            pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_3LE;
            break;
        default:
            pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
    }

    if (!pa_pal_channel_map_to_pal(&source->default_map, &pal_sdata->stream_attributes->in_media_config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(&pal_sdata->stream_attributes->in_media_config.ch_info);
        return -1;
    }

    pal_sdata->pal_device = pa_xnew0(struct pal_device, 1);
    memset(pal_sdata->pal_device, 0, sizeof(struct pal_device));
    pal_sdata->pal_device->id = port_device_data->device;
    pal_sdata->dynamic_usecase = (source->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) ? true : false;
    pal_sdata->pal_device->config.sample_rate = port_device_data->default_spec.rate;
    pal_sdata->pal_device->config.bit_width = 16;

    if (port_device_data->pal_devicepp_config){
        pa_strlcpy(pal_sdata->pal_device->custom_config.custom_key, port_device_data->pal_devicepp_config,
            sizeof(pal_sdata->pal_device->custom_config.custom_key));
    } else if (source->pal_devicepp_config){
        pa_strlcpy(pal_sdata->pal_device->custom_config.custom_key, source->pal_devicepp_config, sizeof(pal_sdata->pal_device->custom_config.custom_key));
    }
    if (!pa_pal_channel_map_to_pal(&port_device_data->default_map, &pal_sdata->pal_device->config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(&pal_sdata->pal_device->config.ch_info);
        return -1;
    }

    pal_sdata->device_url = NULL; /* TODO: useful for BT devices */
    pal_sdata->index = source->id;
    pal_sdata->buffer_size = (size_t)(source->buffer_size);
    pal_sdata->buffer_count = (size_t)(source->buffer_count);
    pal_sdata->source_event_id = PA_PAL_NO_EVENT;
    pal_sdata->cond_ctrl_thread = pa_cond_new();

    pal_sdata->standby = true;

    return 0;
}

static int pa_pal_source_start(pa_pal_source_data *sdata) {
    int rc = 0;
    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pal_source_data *pal_sdata = sdata->pal_sdata;
    pa_log_debug("%s", __func__);

    if (pal_sdata->standby) {
        if (!sdata->pal_source_opened) {
            rc = open_pal_source(sdata);
            if (rc) {
                pa_log_error("open_pal_source failed, error %d", rc);
                pa_xfree(sdata->pal_sdata);
                sdata->pal_sdata = NULL;
                return rc;
            }
        }
        rc = pal_stream_start(pal_sdata->stream_handle);
        pa_log_debug("pal_stream_start returned %d", rc);
        pal_sdata->standby = false;
    } else {
        pa_log_debug("pal_stream already started");
    }
    return rc;
}

static int pa_pal_source_standby(pa_pal_source_data *sdata) {
    int rc = 0;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);

    pa_log_debug("%s",__func__);

    if (sdata->pal_source_opened) {
        rc = close_pal_source(sdata);
        if (PA_UNLIKELY(rc)) {
            pa_log_error("Could not close source handle %p, error  %d", sdata->pal_sdata->stream_handle, rc);
        }
    } else {
        pa_log_debug("pal_stream already in standby");
    }

    return 0;
}

static int pa_pal_set_device(pal_stream_handle_t *stream_handle,
                          pa_pal_card_port_device_data *param_device_connection) {
    struct pal_device device_connect;
    int ret = 0;

    device_connect.id = param_device_connection->device;

    ret = pal_stream_set_device(stream_handle, PA_NUM_DEVICES, &device_connect);
    if(ret)
        pa_log_error("pal source switch device %d failed %d", device_connect.id, ret);

    return ret;
}

static int pa_pal_source_set_port_cb(pa_source *s, pa_device_port *p) {
    int ret = 0;
    pal_param_device_connection_t param_device_connection;
    pa_pal_card_port_device_data *active_port_device_data;
    bool port_changed = false;
    bool dp_port_changed = false;
    bool hdmi_port_changed = false;

    pa_assert(s);
    pa_assert(p);
    pa_pal_card_port_device_data *port_device_data = PA_DEVICE_PORT_DATA(p);
    pa_pal_source_data *sdata = (pa_pal_source_data *)s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pal_sdata->pal_device);
    pa_assert(port_device_data);
    active_port_device_data = PA_DEVICE_PORT_DATA(s->active_port);
    pa_assert(active_port_device_data);

    /* For Headset-in device, need set connect state */
    if (port_device_data->device == PAL_DEVICE_IN_WIRED_HEADSET || active_port_device_data->device == PAL_DEVICE_IN_WIRED_HEADSET) {
         param_device_connection.id = PAL_DEVICE_IN_WIRED_HEADSET;

         if (port_device_data->device == PAL_DEVICE_IN_WIRED_HEADSET) {
             param_device_connection.connection_state = true;
             if(port_device_data->is_connected != param_device_connection.connection_state)
                port_changed = true;
             port_device_data->is_connected = param_device_connection.connection_state;
         }
         else if (active_port_device_data->device == PAL_DEVICE_IN_WIRED_HEADSET) {
             param_device_connection.connection_state = false;
             if(active_port_device_data->is_connected != param_device_connection.connection_state)
                port_changed = true;
             active_port_device_data->is_connected = param_device_connection.connection_state;
         }

         if (port_changed) {
             pa_log_info("headset mic %s", param_device_connection.connection_state ? "connecting" : "disconnecting");
             ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
                             (void*)&param_device_connection,
                             sizeof(pal_param_device_connection_t));
             if (ret != 0)
                 pa_log_error("pal source set device %d connect status failed %d",
                                PAL_DEVICE_IN_WIRED_HEADSET, ret);
         }
    }

    /* For HDMI-in device, need set connect state */
    if (port_device_data->device == PAL_DEVICE_IN_AUX_DIGITAL ||
          active_port_device_data->device == PAL_DEVICE_IN_AUX_DIGITAL) {
        param_device_connection.id = PAL_DEVICE_IN_AUX_DIGITAL;

        if (port_device_data->device == PAL_DEVICE_IN_AUX_DIGITAL) {
            param_device_connection.connection_state = true;
            if(port_device_data->is_connected != param_device_connection.connection_state)
                dp_port_changed = true;
            port_device_data->is_connected = param_device_connection.connection_state;
        }
        else if (active_port_device_data->device == PAL_DEVICE_IN_AUX_DIGITAL) {
            param_device_connection.connection_state = false;
            if(active_port_device_data->is_connected != param_device_connection.connection_state)
                dp_port_changed = true;
            active_port_device_data->is_connected = param_device_connection.connection_state;
        }

        if (dp_port_changed) {
            ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
                (void*)&param_device_connection,
                sizeof(pal_param_device_connection_t));
            if (ret != 0)
                pa_log_error("pal source set device %d connect status failed %d",
                  PAL_DEVICE_IN_AUX_DIGITAL, ret);
        }
    }

    if (port_device_data->device == PAL_DEVICE_IN_HDMI ||
          active_port_device_data->device == PAL_DEVICE_IN_HDMI) {
        param_device_connection.id = PAL_DEVICE_IN_HDMI;

        if (port_device_data->device == PAL_DEVICE_IN_HDMI) {
            param_device_connection.connection_state = true;
            if(port_device_data->is_connected != param_device_connection.connection_state)
                hdmi_port_changed = true;
            port_device_data->is_connected = param_device_connection.connection_state;
        }
        else if (active_port_device_data->device == PAL_DEVICE_IN_HDMI) {
            param_device_connection.connection_state = false;
            if(active_port_device_data->is_connected != param_device_connection.connection_state)
               hdmi_port_changed = true;
            active_port_device_data->is_connected = param_device_connection.connection_state;
        }

        if (hdmi_port_changed) {
            ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
                (void*)&param_device_connection,
                sizeof(pal_param_device_connection_t));
            if (ret != 0)
                pa_log_error("pal source set device %d connect status failed %d",
                  PAL_DEVICE_IN_HDMI, ret);
        }
    }

    /* Update required port info as per PA active port for next run */
    sdata->pal_sdata->pal_device->id = port_device_data->device;
    if (port_device_data->pal_devicepp_config) {
        pa_strlcpy(sdata->pal_sdata->pal_device->custom_config.custom_key, port_device_data->pal_devicepp_config,
                sizeof(sdata->pal_sdata->pal_device->custom_config.custom_key));
    }
    else {
        pa_strlcpy(sdata->pal_sdata->pal_device->custom_config.custom_key, "",
                sizeof(sdata->pal_sdata->pal_device->custom_config.custom_key));
    }

    if (PA_SOURCE_IS_OPENED(s->state)) {
        pa_assert(sdata->pal_sdata->stream_handle);
    }
    else {
        return ret;
    }

    param_device_connection.id = port_device_data->device;

    sdata->pal_sdata->source_event_id = PA_PAL_DEVICE_SWITCH;
    pa_mutex_lock(sdata->pal_sdata->mutex);
    ret = pa_pal_set_device(sdata->pal_sdata->stream_handle, (pa_pal_card_port_device_data *)&param_device_connection);
    sdata->pal_sdata->source_event_id = PA_PAL_NO_EVENT;
    pa_mutex_unlock(sdata->pal_sdata->mutex);
    pa_cond_signal(sdata->pal_sdata->cond_ctrl_thread, 0);
    if (ret != 0) {
        pa_log_error("pal source switch device failed %d", ret);
        return ret;
    }

    return ret;
}

static int pa_pal_source_set_state_in_io_thread_cb(pa_source *s, pa_source_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_GCC_UNUSED)
{
    pa_pal_source_data *source_data = NULL;
    int r = 0;

    pa_assert(s);
    pa_assert(s->userdata);

    source_data = (pa_pal_source_data *)(s->userdata);

    pa_log_debug("New state is: %d", new_state);

    /* Transition from source init state to source idle/opened */
    if (s->thread_info.state == PA_SOURCE_INIT && PA_SOURCE_IS_OPENED(new_state) &&
       (source_data->pal_sdata->dynamic_usecase)) {
        /* do nothing */
        r = 0;
    }
    else if (PA_SOURCE_IS_OPENED(new_state) && !PA_SOURCE_IS_OPENED(s->thread_info.state))
        r = pa_pal_source_start(source_data);
    else if (new_state == PA_SOURCE_SUSPENDED || (new_state == PA_SINK_UNLINKED && source_data->pal_source_opened))
        r = pa_pal_source_standby(source_data);

    return r;
}

static int pa_pal_source_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    pa_pal_source_data *source_data = NULL;

    pa_assert(o);

    source_data = (pa_pal_source_data *)(PA_SOURCE(o)->userdata);

    pa_assert(source_data);
    pa_assert(source_data->pa_sdata->source);

    switch (code) {
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((pa_usec_t*) data) = 0;
            return 0;
        }

        default:
             break;
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static void pa_pal_source_reconfigure_cb(pa_source *s, pa_sample_spec *spec, bool passthrough) {
    pa_pal_source_data *sdata = NULL;
    pa_source_data *pa_sdata = NULL;
    pal_source_data *pal_sdata = NULL;
    pa_pal_card_port_device_data *port_device_data = NULL;
    pa_channel_map new_map;
    pa_sample_spec tmp_spec;
    pa_volume_t volume;
    float gain ;
    bool supported_format = false;
    bool supported = false;
    uint32_t i;
    int rc = 0;
    uint32_t old_rate;

    pa_assert(s);
    pa_assert(spec);

    sdata = (pa_pal_source_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pal_sdata);
    tmp_spec = *spec;

    pa_sdata = sdata->pa_sdata;
    pal_sdata = sdata->pal_sdata;
    pal_stream_type_t stream_type = pal_sdata->stream_attributes->type;

    gain = ((float) pa_cvolume_max(&s->reference_volume) * (float)PAL_MAX_GAIN) / (float)PA_VOLUME_NORM;
    volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / PAL_MAX_GAIN);
    for (i = 0; i < ARRAY_SIZE(supported_source_rates) ; i++) {
        if (spec->rate == supported_source_rates[i]) {
            supported = true;
            break;
        }
    }
    supported_format = source_check_supported_format(spec->format);

    if (!supported) {
        pa_log_info("Source does not support sample rate of %d Hz", spec->rate);
        return;
    }

    if (!supported_format) {
        pa_log_info("Source does not support sample format of %d ", spec->format);
        return;
    }

    if (!PA_SOURCE_IS_OPENED(s->state)) {
        pa_channel_map_init_auto(&new_map, spec->channels, PA_CHANNEL_MAP_DEFAULT);

        old_rate = pa_sdata->source->sample_spec.rate; /*take backup*/
        pa_sdata->source->sample_spec.rate = spec->rate;
        pa_sdata->source->sample_spec.format = spec->format;

        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_CHANNELS) {
            s->reference_volume.channels = tmp_spec.channels;
            pa_channel_map_init_auto(&new_map, tmp_spec.channels, PA_CHANNEL_MAP_DEFAULT);
        } else {
            new_map = pa_sdata->source->channel_map;
            tmp_spec.channels = pa_sdata->source->sample_spec.channels;
        }

        port_device_data = PA_DEVICE_PORT_DATA(pa_sdata->source->active_port);

        tmp_spec.format = spec->format;
        /* find nearest suitable rate */
        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE) {
            tmp_spec.rate = pa_pal_source_find_nearest_supported_sample_rate(spec->rate);
        }
        else
            tmp_spec.rate = pa_sdata->source->sample_spec.rate;

        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_ALL)
            pal_sdata->buffer_size = source_get_buffer_size(tmp_spec, stream_type);

        pa_cvolume_set(&s->reference_volume, s->reference_volume.channels, volume);
        rc = restart_pal_source(sdata, PA_ENCODING_PCM, &tmp_spec, &new_map);
        if (PA_UNLIKELY(rc)) {
            pa_sdata->source->sample_spec.rate = old_rate; /*restore old rate if failed*/
            pa_log_error("Could create reopen pal source, error %d", rc);
            return;
        }

        pa_sdata->source->sample_spec = tmp_spec;
        pa_sdata->source->channel_map = new_map;
        pa_source_set_max_rewind(pa_sdata->source, 0);
        pa_source_set_fixed_latency(pa_sdata->source, pa_bytes_to_usec(pal_sdata->buffer_size, &s->sample_spec));
    }

    return;
}

static pa_idxset* pa_pal_source_get_formats(pa_source *s) {
    pa_pal_source_data *sdata = NULL;

    pa_assert(s);

    sdata = (pa_pal_source_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

static void pa_pal_source_thread_func(void *userdata) {
    pa_pal_source_data *source_data = (pa_pal_source_data *)userdata;
    pa_source_data *pa_sdata = NULL;
    pal_source_data *pal_sdata = NULL;

    pa_assert(source_data);

    pa_sdata = source_data->pa_sdata;
    pal_sdata = source_data->pal_sdata;

    pa_assert(pa_sdata);
    pa_assert(pal_sdata);

    pa_log_debug("Source IO Thread starting up");

    pa_thread_mq_install(&pa_sdata->thread_mq);

    for (;;) {
        int ret;
        pa_rtpoll_set_timer_disabled(pa_sdata->rtpoll);

        if ((!pal_sdata->dynamic_usecase &&
            PA_SOURCE_IS_OPENED(pa_sdata->source->thread_info.state)) ||
            PA_SOURCE_IS_RUNNING(pa_sdata->source->thread_info.state)) {
            pa_memchunk chunk;
            void *data;
            struct pal_buffer in_buf;

            memset(&in_buf, 0, sizeof(struct pal_buffer));

            chunk.memblock = pa_memblock_new(pa_sdata->source->core->mempool, pal_sdata->buffer_size);
            data = pa_memblock_acquire(chunk.memblock);
            chunk.length = pa_memblock_get_length(chunk.memblock);
            chunk.index = 0;

            in_buf.buffer = data;
            in_buf.size = chunk.length;

            pa_mutex_lock(pal_sdata->mutex);
            if (pal_sdata->source_event_id != PA_PAL_NO_EVENT) {
                /* wait for response from ctrl thread */
                pa_cond_wait(pal_sdata->cond_ctrl_thread, pal_sdata->mutex);
            }
            if (pal_sdata->stream_handle) {
                if ((ret = pal_stream_read(pal_sdata->stream_handle, &in_buf)) <= 0) {
                     pa_log_error("pal_stream_read failed, ret = %d", ret);
                     pa_msleep(pa_bytes_to_usec(in_buf.size, &pa_sdata->source->sample_spec)/1000);
                     ret = in_buf.size;
                }
                chunk.length = ret;
            }
            pa_mutex_unlock(pal_sdata->mutex);

#ifdef SOURCE_DUMP_ENABLED
            pa_log_error(" chunk length %d chunk index %d in_buf.size %d ",chunk.length, chunk.index, ret);
            if ((ret = write(pal_sdata->write_fd, in_buf.buffer, ret)) < 0)
                    pa_log_error("write to fd failed %d", ret);
#endif
            /* FIXME: don't post if read fails */
            pa_memblock_release(chunk.memblock);
            pa_source_post(pa_sdata->source, &chunk);
            pa_memblock_unref(chunk.memblock);

            pa_rtpoll_set_timer_absolute(pa_sdata->rtpoll, pa_rtclock_now());
        }

        /* nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(pa_sdata->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(pa_sdata->thread_mq.outq, PA_MSGOBJECT(pa_sdata->source->core), PA_CORE_MESSAGE_UNLOAD_MODULE, pa_sdata->source->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(pa_sdata->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Source IO Thread shutting down");
}

static int open_pal_source(pa_pal_source_data *sdata) {
    int rc;
#ifdef SOURCE_DUMP_ENABLED
    char *file_name;
#endif

    pal_buffer_config_t out_buf_cfg, in_buf_cfg;
    pal_source_data *pal_sdata = NULL;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);

    pal_sdata = sdata->pal_sdata;
#ifdef SOURCE_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_source_%d", pal_sdata->index);

    pal_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(pal_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for source index %d", pal_sdata->write_fd, pal_sdata->index);

    pa_xfree(file_name);
#endif

    pa_log_debug("opening source with configuration flag = 0x%x, format %d, sample_rate %d",
                 pal_sdata->stream_attributes->type, pal_sdata->stream_attributes->in_media_config.aud_fmt_id,
                 pal_sdata->stream_attributes->in_media_config.sample_rate);

    rc = pal_stream_open(pal_sdata->stream_attributes, 1, pal_sdata->pal_device, 0, NULL, NULL, 0,
                             &pal_sdata->stream_handle);
    if (rc) {
        pal_sdata->stream_handle = NULL;
        pa_log_error("Could not open input stream %d", rc);
        goto fail;
    }

    pa_log_debug("pal source opened %p", pal_sdata->stream_handle);

    /* FIXME: Update it by calling pal_stream_get_buffer_size */
    pa_log_debug("buffer size is %zu, buffer count is %zu\n", pal_sdata->buffer_size, pal_sdata->buffer_count);

    out_buf_cfg.buf_size = 0;
    out_buf_cfg.buf_count = 0;
    in_buf_cfg.buf_size = pal_sdata->buffer_size;
    in_buf_cfg.buf_count = pal_sdata->buffer_count;

    rc = pal_stream_set_buffer_size(pal_sdata->stream_handle, &in_buf_cfg, &out_buf_cfg);
    if(rc) {
        pa_log_error("pal_stream_set_buffer_size failed\n");
    }

    sdata->pal_source_opened = true;

fail:
    return rc;
}

static int close_pal_source(pa_pal_source_data *sdata) {
    pal_source_data *pal_sdata;
    pa_source_data *pa_sdata;
    int rc = -1;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pa_sdata);

    pal_sdata = sdata->pal_sdata;
    pa_sdata = sdata->pa_sdata;

    pa_assert(pal_sdata->stream_handle);
    pa_mutex_lock(pal_sdata->mutex);

    pa_log_debug("closing pal source %p", pal_sdata->stream_handle);

    if (PA_UNLIKELY(pal_sdata->stream_handle == NULL)) {
        pa_log_error("Invalid source handle %p", pal_sdata->stream_handle);
    } else {
        rc = pal_stream_stop(pal_sdata->stream_handle);

        if (PA_UNLIKELY(rc))
            pa_log_error(" pal_stream_stop failed for %p error  %d", pal_sdata->stream_handle, rc);

        rc = pal_stream_close(pal_sdata->stream_handle);
        if (PA_UNLIKELY(rc)) {
            pa_log_error(" could not close source handle %p, error  %d", pal_sdata->stream_handle, rc);
        }

        pal_sdata->stream_handle = NULL;
        pal_sdata->standby = true;
        sdata->pal_source_opened = false;
    }

    pa_mutex_unlock(pal_sdata->mutex);
#ifdef SOURCE_DUMP_ENABLED
    close(pal_sdata->write_fd);
#endif

    return rc;
}

static int restart_pal_source(pa_pal_source_data *sdata, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map) {
    int rc;
    pal_source_data *pal_sdata = NULL;
    pa_source_data *pa_sdata = NULL;
    pal_audio_fmt_t pal_format;
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pa_sdata);

    pa_sdata = sdata->pa_sdata;
    pal_sdata = sdata->pal_sdata;
    if (!pal_sdata->standby) {
        rc = close_pal_source((pa_pal_source_data *)sdata->pal_sdata);
        if (rc) {
            pa_log_error("close_pal_source failed, error %d", rc);
            goto exit;
        }
    }

    pal_format = pa_pal_util_get_pal_format_from_pa_encoding(encoding, NULL);
    if (!pal_format) {
        pa_log_error("%s: unsupported format", __func__);
        return -1;
    }
    if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH){
       switch (ss->format) {
           case PA_SAMPLE_S32LE:
               sdata->pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S32_LE;
               sdata->pal_sdata->stream_attributes->in_media_config.bit_width = 32;
               break;
           case PA_SAMPLE_S24_32LE:
               sdata->pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_LE;
               sdata->pal_sdata->stream_attributes->in_media_config.bit_width = 24;
               break;
           case PA_SAMPLE_S24LE:
               sdata->pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_3LE;
               sdata->pal_sdata->stream_attributes->in_media_config.bit_width = 24;
               break;
           default:
               sdata->pal_sdata->stream_attributes->in_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
               sdata->pal_sdata->stream_attributes->in_media_config.bit_width = 16;
               break;
       }
    }
    else
        sdata->pal_sdata->stream_attributes->in_media_config.aud_fmt_id = pal_format;

    sdata->pal_sdata->stream_attributes->in_media_config.sample_rate = ss->rate;
    if (!pa_pal_channel_map_to_pal(map, &sdata->pal_sdata->stream_attributes->in_media_config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        return -1;
    }

    rc = open_pal_source(sdata);
    if (rc) {
        pa_log_error("open_pal_source failed during recreation, error %d", rc);
    }

exit:
    return rc;
}

static int free_pal_source(pal_source_data *pal_sdata) {
    int rc = 0;

    if (!pal_sdata->standby) {
        rc = close_pal_source((pa_pal_source_data *)pal_sdata);
        if (rc) {
            pa_log_error("close_pal_source failed, error %d", rc);
        }
    }

    pa_mutex_free(pal_sdata->mutex);
    pa_cond_free(pal_sdata->cond_ctrl_thread);
    pa_xfree(pal_sdata->stream_attributes);
    pa_xfree(pal_sdata->pal_device);
    pa_xfree(pal_sdata);
    pal_sdata = NULL;

    return rc;
}

static int create_pal_source(pa_pal_source_config *source, pa_pal_card_port_device_data *port_device_data, pa_pal_source_data *sdata) {
    int rc;

    sdata->pal_sdata = pa_xnew0(pal_source_data, 1);

    sdata->pal_sdata->mutex = pa_mutex_new(false /* recursive  */, false /* inherit_priority */);
    rc = pa_pal_source_fill_info(source, sdata->pal_sdata, port_device_data);
    if (rc) {
        pa_log_error("pal source init failed, error %d", rc);
        pa_xfree(sdata->pal_sdata);
        sdata->pal_sdata = NULL;
        return rc;
    }
    return rc;
}

static int create_pa_source(pa_module *m, char *source_name, char *description, pa_idxset *formats, pa_sample_spec *ss, pa_channel_map *map, bool use_hw_volume, uint32_t alternate_sample_rate, pa_card *card,
                            pa_pal_card_avoid_processing_config_id_t avoid_config_processing, pa_hashmap *ports, const char *driver, pa_pal_source_data *source_data) {
    pa_source_new_data new_data;
    pa_source_data *pa_sdata = NULL;
    pal_source_data *pal_sdata = NULL;

    pa_device_port *port;
    pa_format_info *format;
    pa_format_info *in_format;
    void *state;
    uint32_t i;

    bool port_source_mapping = false;

    pa_assert(source_data->pal_sdata);

    pal_sdata = source_data->pal_sdata;
    pa_sdata = pa_xnew0(pa_source_data, 1);
    pa_source_new_data_init(&new_data);
    new_data.driver = driver;
    new_data.module = m;
    new_data.card = card;

    source_data->pa_sdata = pa_sdata;

    pa_sdata->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&pa_sdata->thread_mq, m->core->mainloop, pa_sdata->rtpoll);

    pa_source_new_data_set_name(&new_data, source_name);

    pa_log_info("ss->rate %d ss->channels %d", ss->rate, ss->channels);
    pa_source_new_data_set_sample_spec(&new_data, ss);
    pa_source_new_data_set_channel_map(&new_data, map);

    if (avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_ALL) {
        new_data.avoid_resampling_is_set = true;
        new_data.avoid_resampling = true;
    }
    else {
        new_data.avoid_resampling_is_set = false;
        new_data.avoid_resampling = false;
    }
    if (alternate_sample_rate == PA_ALTERNATE_SOURCE_RATE)
        pa_source_new_data_set_alternate_sample_rate(&new_data, PA_ALTERNATE_SOURCE_RATE);
    else if (alternate_sample_rate > 0)
        pa_log_error("%s: unsupported alternate sample rate %d",__func__, alternate_sample_rate);

    /* associate port with source */
    PA_HASHMAP_FOREACH(port, ports, state) {
        pa_log_debug("adding port %s to source %s", port->name, source_name);
        pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) == 0);
        port_source_mapping = true;
        pa_device_port_ref(port);
    }

   if (!port_source_mapping) {
        pa_log_error("%s: source %s creation failed as no port mapped, ",__func__, source_name);
        goto fail;
    }

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_pal_source_get_name_from_type(pal_sdata->stream_attributes->type));
    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_DESCRIPTION, description);

    pa_sdata->source = pa_source_new(m->core, &new_data, PA_SOURCE_HARDWARE);
    if (!pa_sdata->source) {
        pa_log_error("Could not create source");
        goto fail;
    }

    pa_log_info("pa source opened %p", pa_sdata->source);
    pa_source_new_data_done(&new_data);

    pa_sdata->avoid_config_processing = avoid_config_processing;

    pa_sdata->source->userdata = (void *)source_data;
    pa_sdata->source->parent.process_msg = pa_pal_source_process_msg;
    pa_sdata->source->set_state_in_io_thread = pa_pal_source_set_state_in_io_thread_cb;
    pa_sdata->source->set_port = pa_pal_source_set_port_cb;

    /* FIXME: check reconfigure needed for non pcm */
    pa_sdata->source->reconfigure = pa_pal_source_reconfigure_cb;

    if (pa_idxset_size(formats) > 0 ) {
        pa_sdata->source->get_formats = pa_pal_source_get_formats;

        pa_sdata->formats = pa_idxset_new(NULL, NULL);

        PA_IDXSET_FOREACH(in_format, formats, i) {
            format = pa_format_info_copy(in_format);
            pa_idxset_put(pa_sdata->formats, format, NULL);
        }
    }

    pa_source_set_asyncmsgq(pa_sdata->source, pa_sdata->thread_mq.inq);
    pa_source_set_rtpoll(pa_sdata->source, pa_sdata->rtpoll);
    pa_source_set_max_rewind(pa_sdata->source, 0);
    pa_source_set_fixed_latency(pa_sdata->source, pa_bytes_to_usec(pal_sdata->buffer_size, ss));

    if (use_hw_volume) {
        pa_sdata->source->n_volume_steps = PA_VOLUME_NORM+1; /* FIXME: What should be value */
        pa_source_set_set_volume_callback(pa_sdata->source, pa_pal_source_set_volume_cb);
    }

    pa_sdata->thread = pa_thread_new(source_name, pa_pal_source_thread_func, source_data);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

    pa_source_put(pa_sdata->source);

    return 0;

fail :
    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    if (pa_sdata->source) {
        pa_source_new_data_done(&new_data);
        pa_source_unlink(pa_sdata->source);
        pa_source_unref(pa_sdata->source);
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);
    }

    pa_xfree(pa_sdata);
    source_data->pa_sdata = NULL;

    return -1;
}

static int free_pa_source(pa_source_data *pa_sdata) {
    pa_assert(pa_sdata);
    pa_assert(pa_sdata->source);
    pa_assert(pa_sdata->thread);
    pa_assert(pa_sdata->rtpoll);

    pa_log_debug("closing pa source %p", pa_sdata->source);

    pa_source_unlink(pa_sdata->source);

    pa_asyncmsgq_send(pa_sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
    pa_thread_free(pa_sdata->thread);

    pa_source_unref(pa_sdata->source);

    if (pa_sdata->formats)
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    pa_thread_mq_done(&pa_sdata->thread_mq);

    pa_rtpoll_free(pa_sdata->rtpoll);

    pa_xfree(pa_sdata);

    return 0;
}

bool pa_pal_source_is_supported_sample_rate(uint32_t sample_rate) {
    bool supported = false;
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(supported_source_rates) ; i++) {
        if (sample_rate == supported_source_rates[i]) {
            supported = true;
            break;
        }
    }

    return supported;
}

pa_idxset* pa_pal_source_get_config(pa_pal_source_handle_t *handle) {
    pa_pal_source_data *sdata = (pa_pal_source_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->source);

    return pa_pal_source_get_formats(sdata->pa_sdata->source);
}

int pa_pal_source_get_media_config(pa_pal_source_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding) {
    pa_pal_source_data *sdata = (pa_pal_source_data *)handle;
    pa_format_info *f;

    uint32_t i;
    int ret = -1;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->source);

    *ss = sdata->pa_sdata->source->sample_spec;
    *map = sdata->pa_sdata->source->channel_map;

    PA_IDXSET_FOREACH(f, sdata->pa_sdata->formats, i) {
        /* currently a source supports single format */
        *encoding = f->encoding;
        ret = 0;
        break;
    }

    return ret;
}

int pa_pal_source_create(pa_module *m, pa_card *card, const char *driver, const char *module_name, pa_pal_source_config *source,
                          pa_pal_source_handle_t **handle) {
    int rc = -1;
    pa_pal_source_data *sdata;
    pa_device_port *card_port;
    pa_pal_card_port_config *source_port;
    pa_hashmap *ports;
    pa_pal_card_port_device_data *port_device_data;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_name);
    pa_assert(source);
    pa_assert(source->name);
    pa_assert(source->description);
    pa_assert(source->formats);
    pa_assert(source->ports);

    if (pa_hashmap_isempty(source->ports)) {
        pa_log_error("%s: empty port list", __func__);
        goto exit;
    }

    /*convert config port to card port */
    ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    PA_HASHMAP_FOREACH(source_port, source->ports, state) {
        if ((card_port = pa_hashmap_get(card->ports, source_port->name)))
            pa_hashmap_put(ports, card_port->name, card_port);
    }

    /* first entry is default device */
    card_port = pa_hashmap_first(ports);
    port_device_data = PA_DEVICE_PORT_DATA(card_port);
    pa_assert(port_device_data);

    sdata = pa_xnew0(pa_pal_source_data, 1);

    pa_log_info("%s: creating source with ss %s buffer size %d buffer count %d", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &source->default_spec), source->buffer_size, source->buffer_count);

    rc = create_pal_source(source, port_device_data, sdata);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could not open pal source, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pa_source(m, source->name, source->description, source->formats, &source->default_spec, &source->default_map, source->use_hw_volume, source->alternate_sample_rate, card, source->avoid_config_processing, ports, driver, sdata);
    pa_hashmap_free(ports);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa source for source %s, error %d", source->name, rc);
        free_pal_source(sdata->pal_sdata);
        pa_xfree(sdata);
        sdata = NULL;
    }

    *handle = (pa_pal_source_handle_t *)sdata;

exit:
    return rc;
}

void pa_pal_source_close(pa_pal_source_handle_t *handle) {
    pa_pal_source_data *sdata = (pa_pal_source_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pa_sdata);

    free_pa_source(sdata->pa_sdata);
    free_pal_source(sdata->pal_sdata);
    pa_xfree(sdata);
}
