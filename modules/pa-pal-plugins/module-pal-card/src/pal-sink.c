/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <math.h>

#include <pulse/rtclock.h>
#include <pulse/util.h>
#include <pulsecore/core-rtclock.h>
#include <pulsecore/core-format.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/sink.h>
#include <pulsecore/memchunk.h>
#include <pulsecore/mutex.h>
#include <pulsecore/core-util.h>

#include <sys/time.h>
#include <time.h>

#include "pal-sink.h"
#include "pal-utils.h"

/* #define SINK_DEBUG */

/* #define SINK_DUMP_ENABLED */

#define PAL_MAX_GAIN 1

#define PA_ALTERNATE_SINK_RATE 44100
#define PA_FORMAT_DEFAULT_SAMPLE_RATE_INDEX 0
#define PA_FORMAT_DEFAULT_SAMPLE_FORMAT_INDEX 0
#define PA_DEFAULT_SINK_FORMAT PA_SAMPLE_S16LE
#define PA_DEFAULT_SINK_RATE 48000
#define PA_DEFAULT_SINK_CHANNELS 2
#define PA_BITS_PER_BYTE 8
#define PA_DEFAULT_BUFFER_DURATION_MS 25
#define PA_LOW_LATENCY_BUFFER_DURATION_MS 5
#define PA_DEEP_BUFFER_BUFFER_DURATION_MS 20


typedef struct {
    struct pa_idxset *sinks;
} pa_pal_sink_module_data;

static pa_pal_sink_module_data *mdata = NULL;

static int restart_pal_sink(pa_sink *s, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map,
                            pa_pal_card_port_device_data *port_device_data, pal_stream_type_t type, int sink_id,
                            pa_pal_sink_data *sdata, uint32_t buffer_size, uint32_t buffer_count);
static int create_pal_sink(pa_pal_sink_config *sink, pa_pal_card_port_device_data *port_device_data, pa_pal_sink_data *sdata);
static int close_pal_sink(pa_pal_sink_data *sdata);
static int free_pa_sink(pa_pal_sink_data *sdata);
static int open_pal_sink(pa_pal_sink_data *sdata);
static int pa_pal_set_param(pal_sink_data *pal_sdata, uint32_t param_id);
static void free_pal_sink_thread_resources(pal_sink_data *pal_sdata);

static const uint32_t supported_sink_rates[] =
                          {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};

static size_t sink_get_buffer_size(pa_sample_spec spec, pal_stream_type_t type) {
    uint32_t buffer_duration = PA_DEFAULT_BUFFER_DURATION_MS;
    size_t length = 0;

    switch (type) {
        case PAL_STREAM_DEEP_BUFFER:
            buffer_duration = PA_DEEP_BUFFER_BUFFER_DURATION_MS;
            break;
        case PAL_STREAM_LOW_LATENCY:
            buffer_duration = PA_LOW_LATENCY_BUFFER_DURATION_MS;
            break;
        default:
            break;
    }
    length = ((spec.rate * buffer_duration * spec.channels * pa_sample_size_of_format(spec.format)) / 1000);

    return pa_frame_align(length, &spec);
}

static pa_sample_format_t pa_pal_sink_find_nearest_supported_pa_format(pa_sample_format_t format) {
    pa_sample_format_t format1;

    switch(format) {
        case PA_SAMPLE_S16LE:
        case PA_SAMPLE_U8:
        case PA_SAMPLE_ALAW:
        case PA_SAMPLE_S16BE:
            format1 = PA_SAMPLE_S16LE;
            break;
        case PA_SAMPLE_S24LE:
        case PA_SAMPLE_S24BE:
        case PA_SAMPLE_S24_32LE:
        case PA_SAMPLE_S24_32BE:
            format1 = PA_SAMPLE_S24LE;
            break;
        case PA_SAMPLE_S32LE:
        case PA_SAMPLE_FLOAT32LE:
        case PA_SAMPLE_S32BE:
            format1 = PA_SAMPLE_S32LE;
            break;
        default:
            format1 = PA_SAMPLE_S16LE;
            pa_log_error(" unsupport format %d hence defaulting to %d",format, format1);
    }
    return format1;

}

static uint32_t pa_pal_sink_find_nearest_supported_sample_rate(uint32_t sample_rate) {
    uint32_t i;
    uint32_t nearest_rate = PA_DEFAULT_SINK_RATE;

    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (sample_rate == supported_sink_rates[i]) {
            nearest_rate = sample_rate;
            break;
        } else if (sample_rate > supported_sink_rates[i]) {
            nearest_rate = supported_sink_rates[i];
        }
    }

    return nearest_rate;
}

static const char *pa_pal_sink_get_name_from_type(pal_stream_type_t type) {
    const char *name = NULL;

    if (type == PAL_STREAM_LOW_LATENCY)
        name = "low_latency";
    else if (type == PAL_STREAM_DEEP_BUFFER)
        name = "deep_buffer";
    else if (type == PAL_STREAM_COMPRESSED)
        name = "offload";
    else if (type == PAL_STREAM_VOIP_TX)
        name = "voip_tx";
    else if (type == PAL_STREAM_VOIP_RX)
        name = "voip_rx";
    else if (type == PAL_STREAM_GENERIC)
        name = "direct_pcm";

    return name;
}

static void pa_pal_sink_set_volume_cb(pa_sink *s) {
    pa_pal_sink_data *sdata = NULL;
    float gain;
    int rc;
    pa_volume_t volume;
    pal_sink_data *pal_sdata = NULL;
    struct pal_volume_data *volume_data = NULL;
    uint32_t i,no_vol_pair;
    uint32_t channel_mask = 1;

    pa_assert(s);
    sdata = (pa_pal_sink_data *)s->userdata;

    if (!PA_SINK_IS_RUNNING(s->state)) {
        pa_log_error("set volume is supported only when sink is in RUNNING state\n");
        return;
    }
    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pal_sdata->stream_handle);

    pal_sdata = sdata->pal_sdata;
    no_vol_pair = pal_sdata->stream_attributes->out_media_config.ch_info.channels;

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

    pal_sdata->sink_event_id = PA_PAL_VOLUME_APPLY;
    pa_mutex_lock(pal_sdata->mutex);
    rc = pal_stream_set_volume(pal_sdata->stream_handle, volume_data);
    pal_sdata->sink_event_id = PA_PAL_NO_EVENT;
    pa_mutex_unlock(pal_sdata->mutex);
    pa_cond_signal(pal_sdata->cond_ctrl_thread, 0);
    if (rc)
        pa_log_error("pal stream : unable to set volume error %d\n", rc);
    else
        pa_cvolume_set(&s->real_volume, s->real_volume.channels, volume); /* TODO: Is this correct?  */

    pa_xfree(volume_data);
    return;
}

static int pa_pal_sink_fill_info(pa_pal_sink_config *sink, pal_sink_data *pal_sdata, pa_pal_card_port_device_data *port_device_data, pal_audio_fmt_t encoding) {
    pa_assert(pal_sdata);

    pal_sdata->stream_attributes = pa_xnew0(struct pal_stream_attributes, 1);
    pal_sdata->stream_attributes->type = sink->stream_type;
    pal_sdata->stream_attributes->info.opt_stream_info.version = 1;
    pal_sdata->stream_attributes->info.opt_stream_info.duration_us = -1;
    pal_sdata->stream_attributes->info.opt_stream_info.has_video = false;
    pal_sdata->stream_attributes->info.opt_stream_info.is_streaming = false;

    pal_sdata->stream_attributes->flags = 0;
    pal_sdata->stream_attributes->direction = PAL_AUDIO_OUTPUT;
    pal_sdata->stream_attributes->out_media_config.sample_rate = sink->default_spec.rate;
    pal_sdata->stream_attributes->out_media_config.bit_width = pa_sample_size_of_format(sink->default_spec.format) * PA_BITS_PER_BYTE;

    switch (pal_sdata->stream_attributes->out_media_config.bit_width) {
        case 32:
            pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S32_LE;
            break;
        case 24:
            pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_3LE;
            break;
        default:
            pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
            break;
    }

    pal_sdata->compressed = (encoding != PAL_AUDIO_FMT_PCM_S16_LE ? true : false);
    if (pal_sdata->stream_attributes->type == PAL_STREAM_COMPRESSED) {
        pal_sdata->stream_attributes->info.opt_stream_info.duration_us = 4000;
        pal_sdata->stream_attributes->flags = PAL_STREAM_FLAG_NON_BLOCKING_MASK;
        pal_sdata->compressed = true;
    }

    pal_sdata->pal_snd_dec = pa_xnew0(pal_snd_dec_t, 1);
    memset(pal_sdata->pal_snd_dec, 0, sizeof(pal_snd_dec_t));

    if (!pa_pal_channel_map_to_pal(&sink->default_map, &pal_sdata->stream_attributes->out_media_config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(&pal_sdata->stream_attributes->out_media_config.ch_info);
        return -1;
    }

    pal_sdata->pal_device = pa_xnew0(struct pal_device, 1);
    memset(pal_sdata->pal_device, 0, sizeof(struct pal_device));
    pal_sdata->pal_device->id = port_device_data->device;
    pal_sdata->dynamic_usecase = (sink->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) ? true : false;
    pal_sdata->pal_device->config.sample_rate = port_device_data->default_spec.rate;
    pal_sdata->pal_device->config.bit_width = 16;
    if (sink->pal_devicepp_config) {
        pa_strlcpy(pal_sdata->pal_device->custom_config.custom_key, sink->pal_devicepp_config, sizeof(pal_sdata->pal_device->custom_config.custom_key));
    }
    if (!pa_pal_channel_map_to_pal(&port_device_data->default_map, &pal_sdata->pal_device->config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        pa_xfree(&pal_sdata->pal_device->config.ch_info);
        return -1;
    }

    pal_sdata->device_url = NULL; /* TODO: useful for BT devices */
    pal_sdata->bytes_written = 0;
    pal_sdata->index = sink->id;
    pal_sdata->buffer_size = (size_t)(sink->buffer_size);
    pal_sdata->buffer_count = (size_t)(sink->buffer_count);
    /* FIXME: Add DSP latency */
    pal_sdata->sink_latency_us = pa_bytes_to_usec(pal_sdata->buffer_size, &sink->default_spec);
    pal_sdata->sink_event_id = PA_PAL_NO_EVENT;
    pal_sdata->cond_ctrl_thread = pa_cond_new();
    pa_log_debug("sink latency %dus", pal_sdata->sink_latency_us);

    pal_sdata->standby = true;

    return 0;
}

static uint64_t pa_pal_sink_get_latency(pa_pal_sink_data *sdata) {
    int rc;
    uint64_t bytes_rendered;
    int64_t delta, latency = 0, ticks = 0;
    uint64_t cur_qtimer, abs_qtimer_time_stamp, session_time_stamp;
    uint64_t cur_session_time = 0, time_in_future = 0, time_elapsed = 0;
    pal_sink_data *pal_sdata;
    pa_sink_data *pa_sdata;
    struct pal_session_time stime = {0};

#ifdef SINK_DEBUG
    pa_log_debug("%s", __func__);
#endif

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pal_sdata);

    pal_sdata = sdata->pal_sdata;
    pa_sdata = sdata->pa_sdata;

    pa_assert(pa_sdata->sink);
    pa_assert(sdata->pal_sdata->stream_handle);

    rc = pal_get_timestamp(sdata->pal_sdata->stream_handle, &stime);
    if (!rc) {
        abs_qtimer_time_stamp = (uint64_t)(((uint64_t)stime.absolute_time.value_msw << 32) | (uint64_t)stime.absolute_time.value_lsw);
        session_time_stamp = (uint64_t)(((uint64_t)stime.session_time.value_msw << 32) | (uint64_t)stime.session_time.value_lsw);
#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
        pa_sdata->sink->sess_time = session_time_stamp;
#endif

#ifdef SINK_DEBUG
        pa_log_debug("%s: abs_qtimer_time_stamp %" PRId64 " us, session_time_stamp %" PRId64 " us", __func__,
                     abs_qtimer_time_stamp, session_time_stamp);
#endif

#if defined __aarch64__
        asm volatile("mrs %0, cntvct_el0" : "=r"(ticks));
#else
        asm volatile("mrrc p15, 1, %Q0, %R0, c14" : "=r"(ticks));
#endif

        cur_qtimer = (uint64_t)(ticks * 10/192);

#ifdef SINK_DEBUG
        pa_log_debug("%s:: ticks %" PRId64 " us, qtimer %" PRId64 " us", __func__, ticks, (int64_t)cur_qtimer);
#endif

        if (abs_qtimer_time_stamp > cur_qtimer) {
            time_in_future = abs_qtimer_time_stamp - cur_qtimer;
            if (time_in_future < session_time_stamp) {
                cur_session_time = session_time_stamp - time_in_future;
                bytes_rendered = pa_usec_to_bytes(cur_session_time, &pa_sdata->sink->sample_spec);
            } else {
                bytes_rendered = 0;
            }
        } else {
            time_elapsed = cur_qtimer - abs_qtimer_time_stamp;
            cur_session_time = session_time_stamp + time_elapsed;
            bytes_rendered = pa_usec_to_bytes(cur_session_time, &pa_sdata->sink->sample_spec);
        }

        delta = pal_sdata->bytes_written - bytes_rendered;
        /* bytes written should never be less than bytes rendered */
        if (delta <= 0) {
#ifdef SINK_DEBUG
            pa_log_debug("latency is 0");
#endif
            return 0;
        }

        latency = pa_bytes_to_usec(delta, &pa_sdata->sink->sample_spec);
#ifdef SINK_DEBUG
        pa_log_debug("%s:: time_in_future %" PRId64 ", cur_session_time %" PRId64 " bytes_rendered %d, latency %" PRId64 "", __func__,
                     time_in_future, cur_session_time, bytes_rendered, (int64_t)latency);
#endif
    } else  {
        latency = (int64_t)(pa_bytes_to_usec(pal_sdata->bytes_written, &pa_sdata->sink->sample_spec));
#ifdef SINK_DEBUG
        pa_log_debug("pal_get_timestamp failed, using latency based on written bytes latency = %" PRId64 "", latency);
#endif
    }

    if (latency < 0) {
        pa_log_error("latency is invalid (-ve), resetting it zero");
        latency = 0;
    }

    return (uint64_t)latency;
}

static int pa_pal_sink_start(pa_pal_sink_data *sdata) {
    int rc = 0;
    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pal_sink_data *pal_sdata = sdata->pal_sdata;
    pa_log_debug("%s %d", __func__, pal_sdata->standby);

    if (pal_sdata->standby) {
        if (!sdata->pal_sink_opened) {
            rc = open_pal_sink(sdata);
            if (rc) {
                pa_log_error("pal sink open failed, error %d", rc);
                goto cleanup;
            }
        }

        if (pal_sdata->compressed) {
             rc = pa_pal_set_param(pal_sdata, PAL_PARAM_ID_CODEC_CONFIGURATION);
             if (rc) {
                pa_log_error("pa_pal_set_param failed, error %d\n", rc);
                goto cleanup;
            }
        }

        rc = pal_stream_start(pal_sdata->stream_handle);
        if (rc) {
            pa_log_error("pal_stream_start failed, error %d\n", rc);
            goto cleanup;
        }
        pa_atomic_store(&sdata->pal_sdata->restart_in_progress, 0);
    } else {
        pa_log_debug("pal_stream already started");
    }

    pal_sdata->standby = false;

    return 0;

cleanup:
    if (sdata->pal_sink_opened && close_pal_sink(sdata))
        pa_log_error("could not close sink handle %p", sdata->pal_sdata->stream_handle);
    return rc;
}

static int pa_pal_sink_standby(pa_pal_sink_data *sdata) {
    int rc = 0;

    pa_assert(sdata);

    pa_log_debug("%s",__func__);

    if (sdata->pal_sink_opened) {
        pa_assert(sdata->pal_sdata);
        rc = close_pal_sink(sdata);
        if (PA_UNLIKELY(rc))
            pa_log_error("could not close sink handle %p, error %d", sdata->pal_sdata->stream_handle, rc);
    } else {
        pa_log_debug("pal_stream already in standby");
    }

    return 0;
}

static int pa_pal_set_device(pal_stream_handle_t *stream_handle,
                          pa_pal_card_port_device_data *param_device_connection) {
    struct pal_device device_connect;
    int no_of_devices = 1;
    int ret = 0;

    device_connect.id = param_device_connection->device;

    ret = pal_stream_set_device(stream_handle, no_of_devices, &device_connect);
    if(ret)
        pa_log_error("pal sink switch device %d failed %d", device_connect.id, ret);
    return ret;
}

static int pa_pal_sink_set_port_cb(pa_sink *s, pa_device_port *p) {
    pa_pal_card_port_device_data *port_device_data;
    pa_pal_card_port_device_data *active_port_device_data;
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)s->userdata;
    pal_param_device_connection_t param_device_connection;
    int no_of_devices = 1;
    int ret = 0;
    bool port_changed = false;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pal_sdata->pal_device);
    if (PA_SINK_IS_OPENED(s->state))
        pa_assert(sdata->pal_sdata->stream_handle);

    port_device_data = PA_DEVICE_PORT_DATA(p);
    pa_assert(port_device_data);

    active_port_device_data = PA_DEVICE_PORT_DATA(s->active_port);
    pa_assert(active_port_device_data);

    /* For HDMI-out device, need set connect state */
    if (port_device_data->device == PAL_DEVICE_OUT_AUX_DIGITAL |
          active_port_device_data->device == PAL_DEVICE_OUT_AUX_DIGITAL) {
        param_device_connection.device_config.dp_config.controller = 0;
        param_device_connection.device_config.dp_config.stream = 0;
        param_device_connection.id = PAL_DEVICE_OUT_AUX_DIGITAL;

        if (port_device_data->device == PAL_DEVICE_OUT_AUX_DIGITAL) {
            param_device_connection.connection_state = true;
            if(port_device_data->is_connected != param_device_connection.connection_state)
                port_changed = true;
            port_device_data->is_connected = param_device_connection.connection_state;
        }
        else if (active_port_device_data->device == PAL_DEVICE_OUT_AUX_DIGITAL) {
            param_device_connection.connection_state = false;
            if(active_port_device_data->is_connected != param_device_connection.connection_state)
                port_changed = true;
            active_port_device_data->is_connected = param_device_connection.connection_state;
        }

        if (port_changed) {
            ret = pal_set_param(PAL_PARAM_ID_DEVICE_CONNECTION,
                (void*)&param_device_connection,
                sizeof(pal_param_device_connection_t));
            if (ret != 0)
                pa_log_error("pal sink set device %d connect status failed %d",
                  PAL_DEVICE_OUT_AUX_DIGITAL, ret);
        }
    }

    param_device_connection.id = port_device_data->device;
    sdata->pal_sdata->pal_device->id = port_device_data->device;
    if (port_device_data->pal_devicepp_config){
        pa_strlcpy(sdata->pal_sdata->pal_device->custom_config.custom_key, port_device_data->pal_devicepp_config,
                        sizeof(sdata->pal_sdata->pal_device->custom_config.custom_key));
    }
    else {
        pa_strlcpy(sdata->pal_sdata->pal_device->custom_config.custom_key, "",
                        sizeof(sdata->pal_sdata->pal_device->custom_config.custom_key));
    }

    if (PA_SINK_IS_OPENED(s->state)) {
        sdata->pal_sdata->sink_event_id = PA_PAL_DEVICE_SWITCH;
        pa_mutex_lock(sdata->pal_sdata->mutex);
        ret = pa_pal_set_device(sdata->pal_sdata->stream_handle, (pa_pal_card_port_device_data *)&param_device_connection);
        sdata->pal_sdata->sink_event_id = PA_PAL_NO_EVENT;
        pa_mutex_unlock(sdata->pal_sdata->mutex);
        pa_cond_signal(sdata->pal_sdata->cond_ctrl_thread, 0);

        if (ret != 0)
            pa_log_error("pal sink switch device failed %d", ret);
    }

    return ret;
}

static int pa_pal_sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause PA_GCC_UNUSED)
{
    pa_pal_sink_data *sdata = NULL;
    int r = 0;

    pa_assert(s);

    sdata = (pa_pal_sink_data *)(s->userdata);
    pa_assert(sdata);

    pa_log_debug("Sink new state is: %d", new_state);

    /* Transition from sink init state to sink idle/opened */
    if (((s->thread_info.state == PA_SINK_INIT) && PA_SINK_IS_OPENED(new_state)) &&
        (sdata->pal_sdata->dynamic_usecase)) {
        /* do nothing */
        r = 0;
    }
    else if (PA_SINK_IS_OPENED(new_state))
        r = pa_pal_sink_start(sdata);
    else if (new_state == PA_SINK_SUSPENDED || (new_state == PA_SINK_UNLINKED && sdata->pal_sink_opened))
        r = pa_pal_sink_standby(sdata);

    return r;
}

static int pa_pal_sink_io_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    pa_pal_sink_data *sdata = (pa_pal_sink_data *)(PA_SINK(o)->userdata);

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

#ifdef SINK_DEBUG
    pa_log_debug("Fcun:%s recevied msg %d\n", __func__, code);
#endif

/* FIXME: Add callback function once pal_stream_get_param is enabled */
    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
            *((int64_t*) data) = pa_pal_sink_get_latency(sdata);
            return 0;
#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
        case PA_PAL_SINK_MESSAGE_DRAIN_READY:
            pa_sink_drain_complete(sdata->pa_sdata->sink);
            return 0;
#endif
        default:
             break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void pa_pal_sink_reconfigure_cb(pa_sink *s, pa_sample_spec *spec, bool passthrough) {
    pa_pal_sink_data *sdata = NULL;
    pa_sink_data *pa_sdata = NULL;
    pal_sink_data *pal_sdata = NULL;
    pa_pal_card_port_device_data *port_device_data = NULL;
    pa_channel_map new_map;
    pa_sample_spec tmp_spec;
    pa_volume_t volume;
    float gain ;

    bool supported = false;
    uint32_t i;
    int rc = 0;
    uint32_t old_rate;

    pa_assert(s);
    pa_assert(spec);

    sdata = (pa_pal_sink_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pal_sdata);

    pa_log_debug("%s", __func__);

    pa_sdata = sdata->pa_sdata;
    pal_sdata = sdata->pal_sdata;
    tmp_spec = *spec;
    pal_stream_type_t stream_type = pal_sdata->stream_attributes->type;

    gain = ((float) pa_cvolume_max(&s->reference_volume) * (float)PAL_MAX_GAIN) / (float)PA_VOLUME_NORM;
    volume = (pa_volume_t) roundf((float) gain * PA_VOLUME_NORM / PAL_MAX_GAIN);
    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (spec->rate == supported_sink_rates[i]) {
            supported = true;
            break;
        }
    }

    if (!supported) {
        pa_log_info("Sink does not support sample rate of %d Hz", spec->rate);
        return;
    }

    if (!PA_SINK_IS_OPENED(s->state)) {
        pa_channel_map_init_auto(&new_map, spec->channels, PA_CHANNEL_MAP_DEFAULT);

        old_rate = pa_sdata->sink->sample_spec.rate; /* take backup */
        pa_sdata->sink->sample_spec.rate = spec->rate;

        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_CHANNELS) {
            s->reference_volume.channels = tmp_spec.channels;
            pa_channel_map_init_auto(&new_map, tmp_spec.channels, PA_CHANNEL_MAP_DEFAULT);
        } else {
            new_map = pa_sdata->sink->channel_map;
            tmp_spec.channels = pa_sdata->sink->sample_spec.channels;
        }

        /* find nearest suitable format */
        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH)
            tmp_spec.format = pa_pal_sink_find_nearest_supported_pa_format(spec->format);
        else
            tmp_spec.format = pa_sdata->sink->sample_spec.format;

        /* find nearest suitable rate */
        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_SAMPLE_RATE)
            tmp_spec.rate = pa_pal_sink_find_nearest_supported_sample_rate(spec->rate);
        else
            tmp_spec.rate = pa_sdata->sink->sample_spec.rate;

        if (pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_ALL)
            pal_sdata->buffer_size = sink_get_buffer_size(tmp_spec, stream_type);

        port_device_data = PA_DEVICE_PORT_DATA(pa_sdata->sink->active_port);
        pa_cvolume_set(&s->reference_volume, s->reference_volume.channels, volume);
        rc = restart_pal_sink(s, PA_ENCODING_PCM, &tmp_spec, &new_map, port_device_data,
                pal_sdata->stream_attributes->type, pal_sdata->index, sdata,
                (uint32_t)pal_sdata->buffer_size, pal_sdata->buffer_count);
        if (PA_UNLIKELY(rc)) {
            pa_sdata->sink->sample_spec.rate = old_rate; /* restore old rate if failed */
            pa_log_error("Could create reopen pal sink, error %d", rc);
            return;
        }

        pa_sdata->sink->sample_spec = tmp_spec;
        pa_sdata->sink->channel_map = new_map;
        pa_sink_set_max_request(pa_sdata->sink, pal_sdata->buffer_size);
        pa_sink_set_max_rewind(pa_sdata->sink, 0);
        pa_sink_set_fixed_latency(pa_sdata->sink, pal_sdata->sink_latency_us);
    }

    return;
}

static pa_idxset* pa_pal_sink_get_formats(pa_sink *s) {
    pa_pal_sink_data *sdata = NULL;

    pa_assert(s);

    sdata = (pa_pal_sink_data *) s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);

    return pa_idxset_copy(sdata->pa_sdata->formats, (pa_copy_func_t) pa_format_info_copy);
}

#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
static bool pa_pal_sink_set_format_cb(pa_sink *s, const pa_format_info *format) {
    pal_sink_data *pal_sdata;
    pa_sink_data *pa_sdata;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_encoding_t encoding;
    pa_pal_card_port_device_data *port_device_data;
    char ch_map_buf[PA_CHANNEL_MAP_SNPRINT_MAX];
    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];
    char fmt[PA_FORMAT_INFO_SNPRINT_MAX];
    bool ret = false;

    pa_pal_sink_data *sdata = (pa_pal_sink_data *)s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pal_sdata);

    pa_sdata = sdata->pa_sdata;
    pal_sdata = sdata->pal_sdata;

    if (format != NULL) {
        pa_log_debug("Negotiated format: %s", pa_format_info_snprint(fmt, sizeof(fmt), format));

        if (pa_format_info_is_compressed(format) == 0) {
            pa_log_error("%s: Format info structure is not compressed", __func__);
            goto exit;
        }

        if (pa_pal_util_set_pal_metadata_from_pa_format(format) < 0) {
            pa_log_error("%s: Failed to set metadata from format", __func__);
            goto exit;
        }

        encoding = format->encoding;

        if (pa_format_info_to_sample_spec2(format, &ss, &map,
                    &pa_sdata->sink->sample_spec, &pa_sdata->sink->channel_map)) {
            pa_log_error("%s: Failed to obtain sample spec from format", __func__);
            goto exit;
        }

        if (pa_format_info_get_rate(format, &ss.rate) < 0) {
            pa_log_error("%s: Failed to obtain rate from format", __func__);
            goto exit;
        }

        if (pa_format_info_get_channels(format, &ss.channels) < 0) {
            pa_log_info("%s: Failed to obtain channels from format, set it to stereo", __func__);
            ss.channels = 2;
        }

        pa_pal_util_channel_map_init(&map, ss.channels);

        pa_log_info("%s: sink spec %s channel map, %s sample spec %s channel map %s", __func__,
              pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &pa_sdata->sink->sample_spec),
              pa_channel_map_snprint(ch_map_buf, sizeof(ch_map_buf), &pa_sdata->sink->channel_map),
              pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &ss),
              pa_channel_map_snprint(ch_map_buf, sizeof(ch_map_buf), &map));

       port_device_data = PA_DEVICE_PORT_DATA(pa_sdata->sink->active_port);

       if (restart_pal_sink(s, encoding, &pa_sdata->sink->sample_spec, &map, port_device_data,
                                pal_sdata->stream_attributes->type, pal_sdata->index, sdata,
                                (uint32_t)pal_sdata->buffer_size, pal_sdata->buffer_count)) {
           pa_log_error("%s: Failed to restart pal_sink with %s encoding", __func__, format == NULL ? "default" : "requested");
           goto exit;
       } else {
           pa_log_info("%s: Started pal_sink with %s encoding", __func__, format == NULL ? "default" : "requested");
           ret = true;
       }
   } else {
       if (sdata->pal_sdata->stream_handle != NULL) {
           pa_atomic_store(&sdata->pal_sdata->close_output, 1);
           /* stream should be in paused state during flush */
           pal_stream_pause(sdata->pal_sdata->stream_handle);
           if (pal_stream_flush(sdata->pal_sdata->stream_handle) != 0) {
               pa_log_error("%s: stream flush failed", __func__);
           }
       } else {
           pa_log_error("%s: Invalid stream handle", __func__);
       }

       pa_log_debug("%s: Exit compress playback", __func__);
       ret = true;
   }

exit:
    return ret;
}

static int pa_pal_sink_drain_cb(pa_sink *s) {
    int rc = 0;
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pal_sdata->stream_handle);

    if (!PA_SINK_IS_OPENED(s->state))
        return rc;

    pa_log_info("Func:%s", __func__);

    return pal_stream_drain(sdata->pal_sdata->stream_handle, PAL_DRAIN_PARTIAL);
}

static int pa_pal_sink_flush_cb(pa_sink *s) {
    int rc = 0;
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)s->userdata;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pal_sdata->stream_handle);

    if (!PA_SINK_IS_OPENED(s->state))
        return rc;

    pa_log_info("Func:%s", __func__);

    /* stream should be in paused state during flush */
    pal_stream_pause(sdata->pal_sdata->stream_handle);

    return pal_stream_flush(sdata->pal_sdata->stream_handle);
}
#endif

static void write_chunk(pa_pal_sink_data *sdata, pa_memchunk *chunk) {
    int rc = 0;
    void *data = NULL;
    struct pal_buffer out_buf;
    pa_sink_data *pa_sdata =  sdata->pa_sdata;
    pal_sink_data *pal_sdata = sdata->pal_sdata;
    size_t sink_buffer_size = pal_sdata->buffer_size;

    memset(&out_buf, 0, sizeof(struct pal_buffer));
    data = pa_memblock_acquire(chunk->memblock);
    out_buf.buffer = (char*)data + chunk->index;
    out_buf.size = chunk->length;
    sink_buffer_size = chunk->length;

    while(out_buf.buffer && !pa_atomic_load(&sdata->pal_sdata->close_output)) {
        pa_mutex_lock(pal_sdata->mutex);
        if (pal_sdata->sink_event_id != PA_PAL_NO_EVENT) {
            /* wait for response from ctrl thread */
            pa_cond_wait(pal_sdata->cond_ctrl_thread, pal_sdata->mutex);
        }
        if (pal_sdata->stream_handle) {
            if ((rc = pal_stream_write(pal_sdata->stream_handle, &out_buf)) < 0) {
                pa_log_error("Could not write data: %d %d", rc, __LINE__);
                break;
            }
        }
        else
            rc = -1;
        pa_mutex_unlock(pal_sdata->mutex);

        /* Update buffer attributes and bytes written */
        if ((pal_sdata->compressed) && (rc >= 0) && (rc < (int)out_buf.size)) {
#ifdef SINK_DEBUG
            pa_log_debug("[%d]Func:%s Waiting for write done event, size %d written %d",
                    __LINE__, __func__, (int)out_buf.size, rc);
#endif
            pa_fdsem_wait(pal_sdata->pal_fdsem);
#ifdef SINK_DEBUG
            pa_log_debug("[%d]Func:%s Async wake", __LINE__, __func__);
#endif
            /* Store pending bytes to be written, write done event comes */
            out_buf.size = out_buf.size - rc;
            /* Update buffer offset and size based on last write size */
            out_buf.buffer = (char *)out_buf.buffer + sink_buffer_size - out_buf.size;
        } else {
            pal_sdata->bytes_written += rc;
#ifdef SINK_DEBUG
            pa_log_debug("[%d]Func:%s Write data: size %d total %d", __LINE__, __func__,
                    rc, pal_sdata->bytes_written);
#endif
#ifdef SINK_DUMP_ENABLED
            if ((rc = write(pal_sdata->write_fd, out_buf.buffer, out_buf.size)) < 0)
                pa_log_error("write to fd failed %d", rc);
#endif
            /* Mark buffer as NULL, to indicate buffer has been consumed */
            out_buf.size = out_buf.size - rc;
            out_buf.buffer = NULL;
        }
    }

    pa_memblock_release(chunk->memblock);
    pa_memblock_unref(chunk->memblock);
}

static int pal_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    int rc = -1;
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)(PAL_MSG_OBJ(o)->userdata);

    pa_assert(sdata);
    pa_assert(chunk);

    switch (code) {
        case PAL_SINK_MESSAGE_WRITE_READY:
            write_chunk(sdata, chunk);
            pa_atomic_store(&sdata->pal_sdata->write_done, 1);
            rc = 0;
            /* Wake up sink thread */
            pa_fdsem_post(sdata->fdsem);
            break;
        default:
            pa_log_info("%s: Unknown code", __func__);
            break;
    }

    return rc;
}

static void pal_sink_thread_func(void *userdata) {
    pa_pal_sink_data *sink_data = (pa_pal_sink_data *) userdata;
    pa_sink_data *pa_sdata = sink_data->pa_sdata;
    pal_sink_data *pal_sdata = sink_data->pal_sdata;

    if ((pa_sdata->sink->core->realtime_scheduling)) {
        pa_log_info("%s:: Making io thread for %s as realtime with prio %d", __func__,
                pa_pal_sink_get_name_from_type(pal_sdata->stream_attributes->type),
                pa_sdata->sink->core->realtime_priority);
        pa_thread_make_realtime(pa_sdata->sink->core->realtime_priority);
    }

    pa_log_debug("Sink Write Thread starting up");

    pa_thread_mq_install(&pal_sdata->pal_thread_mq);

    for (;;) {
        int ret = 0;

        /* nothing to do. Let's sleep */
        pa_rtpoll_set_timer_disabled(pal_sdata->pal_thread_rtpoll);
        if ((ret = pa_rtpoll_run(pal_sdata->pal_thread_rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN
     */
    pa_asyncmsgq_post(pal_sdata->pal_thread_mq.outq, PA_MSGOBJECT(pa_sdata->sink->core),
            PA_CORE_MESSAGE_UNLOAD_MODULE, pa_sdata->sink->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(pal_sdata->pal_thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Sink Write Thread shutting down");
}

static int create_pal_sink_thread(pa_pal_sink_data *sdata) {
    pa_sink_data *pa_sdata = sdata->pa_sdata;
    pal_sink_data *pal_sdata = sdata->pal_sdata;
    char *thread_name = NULL;
    int ret = 0;

    pa_assert(sdata);
    pa_assert(pa_sdata);
    pa_assert(pal_sdata);

    pa_atomic_store(&pal_sdata->write_done, 1);

    pal_sdata->pal_thread_rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&pal_sdata->pal_thread_mq, pa_sdata->sink->core->mainloop,
            pal_sdata->pal_thread_rtpoll);

    pal_sdata->pal_msg = pa_msgobject_new(pal_msg_obj);
    pal_sdata->pal_msg->parent.process_msg = pal_sink_process_msg;
    pal_sdata->pal_msg->userdata = (void *)sdata;
    pal_sdata->pal_fdsem = pa_fdsem_new();
    if (!pal_sdata->pal_fdsem) {
        pa_log_error("Could not create pal fdsem");
        ret = -1;
        goto fail;
    }

    pal_sdata->pal_rtpoll_item = pa_rtpoll_item_new_fdsem(pal_sdata->pal_thread_rtpoll,
            PA_RTPOLL_NORMAL, pal_sdata->pal_fdsem);
    if (!pal_sdata->pal_rtpoll_item) {
        pa_log_error("Could not create rpoll item");
        ret = -1;
        goto fail;
    }

    thread_name = pa_sprintf_malloc("%s_pal_thread", pa_sdata->sink->name);

    if (!(pal_sdata->pal_thread = pa_thread_new(thread_name, pal_sink_thread_func, sdata))) {
        pa_log_error("%s: pal_write_thread creation failed", __func__);
        pa_xfree(thread_name);
        ret = -1;
        goto fail;
    }
    pa_log_debug("%s %s created", __func__, thread_name);

fail:
    if (ret)
        free_pal_sink_thread_resources(pal_sdata);

    return ret;
}

static void pa_pal_sink_thread_func(void *userdata) {
    pa_pal_sink_data *sdata;
    pa_sink_data *pa_sdata;
    pal_sink_data *pal_sdata;
    uint32_t sink_buffer_size;
    bool render = false;
    pa_memchunk chunk;
    struct pal_buffer out_buf;
    memset(&chunk, 0, sizeof(pa_memchunk));

    void *data;
    int rc;

    pa_assert(userdata);

    sdata = (pa_pal_sink_data *)userdata;
    pa_sdata = sdata->pa_sdata;
    pal_sdata = sdata->pal_sdata;
    sink_buffer_size = (uint32_t)pal_sdata->buffer_size;

    pa_log_debug("%s:\n", __func__);

    if ((pa_sdata->sink->core->realtime_scheduling)) {
        pa_log_info("%s:: Making io thread for %s as realtime with prio %d", __func__,
                pa_pal_sink_get_name_from_type(pal_sdata->stream_attributes->type),
                pa_sdata->sink->core->realtime_priority);
        pa_thread_make_realtime(pa_sdata->sink->core->realtime_priority);
    }
    pa_thread_mq_install(&pa_sdata->thread_mq);

    memset(&out_buf, 0, sizeof(struct pal_buffer));

    while (true) {
        pa_rtpoll_set_timer_disabled(pa_sdata->rtpoll);

        if (pa_sdata->sink->thread_info.rewind_requested)
            pa_sink_process_rewind(pa_sdata->sink, 0);

        /* A compressed sink only renders in RUNNING, not in IDLE */
        render = (!pal_sdata->compressed && !pal_sdata->dynamic_usecase &&
                   PA_SINK_IS_OPENED(pa_sdata->sink->thread_info.state)) ||
                   PA_SINK_IS_RUNNING(pa_sdata->sink->thread_info.state);

        if (render && !pa_atomic_load(&pal_sdata->restart_in_progress)) {
            if (!pal_sdata->compressed) {
                pa_sink_render_full(pa_sdata->sink, pal_sdata->buffer_size, &chunk);
                pa_assert(chunk.length == pal_sdata->buffer_size);
                write_chunk(sdata, &chunk);
                pa_rtpoll_set_timer_absolute(pa_sdata->rtpoll, pa_rtclock_now());
            } else if (pa_atomic_load(&pal_sdata->write_done)) {
                pa_sink_render(pa_sdata->sink, pal_sdata->buffer_size, &chunk);
                pa_assert(chunk.length > 0);
                pa_atomic_store(&pal_sdata->write_done, 0);
                pa_asyncmsgq_post(pal_sdata->pal_thread_mq.inq, PA_MSGOBJECT(pal_sdata->pal_msg),
                        PAL_SINK_MESSAGE_WRITE_READY, NULL, 0, &chunk, NULL);
            }
        } else if (pa_sdata->sink->thread_info.state == PA_SINK_SUSPENDED) {
            /* if sink is suspended state then reset buffer otherwise
             * it might end up sending incorrect buffer to pal_write */
            pa_log_debug("%d sink in suspended state. sending empty buffer \n", __LINE__);
            memset(&out_buf, 0, sizeof(struct pal_buffer));
        }

        rc = pa_rtpoll_run(pa_sdata->rtpoll);

        if (rc < 0) {
            pa_log_error("pa_rtpoll_run() returned an error: %d", rc);
            goto fail;
        }

        if (rc == 0)
            goto done;
    }

fail:
    pa_asyncmsgq_post(pa_sdata->thread_mq.outq, PA_MSGOBJECT(pa_sdata->sink->core), PA_CORE_MESSAGE_UNLOAD_MODULE, pa_sdata->sink->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(pa_sdata->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

done:
    pa_log_debug("Closing I/O thread");
}

static int32_t pa_pal_out_cb(pal_stream_handle_t *stream_handle,
                            uint32_t event_id, uint32_t *event_data,
                            uint32_t event_size, uint64_t cookie) {
    pal_sink_data *pal_sdata;
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)cookie;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pa_sdata);

    pal_sdata = sdata->pal_sdata;

#ifdef SINK_DEBUG
    pa_log_debug("[%d]Func:%s Stream_handle (%p), event_id (%x), event_data (%p), cookie %" PRIu64
        "event_size (%d)", __LINE__, __func__, stream_handle, event_id, event_data, cookie, event_size);
#endif

    switch (event_id) {
        case PAL_STREAM_CBK_EVENT_WRITE_READY:
#ifdef SINK_DEBUG
            pa_log_debug("[%d]Func:%s Received event WRITE_READY for handle %p",
                    __LINE__, __func__, pal_sdata->stream_handle);
#endif
            /* Wake up PAL thread */
            if (pal_sdata->compressed)
                pa_fdsem_post(pal_sdata->pal_fdsem);

            break;

        case PAL_STREAM_CBK_EVENT_PARTIAL_DRAIN_READY:
#ifdef SINK_DEBUG
            pa_log_debug("[%d]Func:%s Received event DRAIN_READY for handle %p",
                    __LINE__, __func__, pal_sdata->stream_handle);
#endif
            /* post drain complete to i/o thread */
            pa_asyncmsgq_post(sdata->pa_sdata->thread_mq.inq, PA_MSGOBJECT(sdata->pa_sdata->sink),
                                                PA_PAL_SINK_MESSAGE_DRAIN_READY, NULL, 0, NULL, NULL);

            break;

        default:
            pa_log_error("Unsupported event %d handle %p", event_id, pal_sdata->stream_handle);

            break;
    }

    return 0;
}

static int pa_pal_set_param(pal_sink_data *pal_sdata, uint32_t param_id) {
    int rc = -1;
    pal_param_payload *param_payload;

    param_payload = (pal_param_payload *) calloc (1, sizeof(pal_param_payload) + sizeof(pal_snd_dec_t));
    if (!param_payload)
        return rc;
    param_payload->payload_size = sizeof(pal_snd_dec_t);
    memcpy(param_payload->payload, pal_sdata->pal_snd_dec, param_payload->payload_size);
    rc = pal_stream_set_param(pal_sdata->stream_handle,
                               param_id, param_payload);
    free(param_payload);

    return rc;
}

int pa_pal_sink_set_a2dp_suspend(const char *prm_value)
{
    int ret = 0;
    pal_param_bta2dp_t param_bt_a2dp;

    pa_assert(prm_value);

    memset(&param_bt_a2dp, 0, sizeof(pal_param_bta2dp_t));
    param_bt_a2dp.a2dp_suspended = (!strcmp(prm_value, "true")) ? true : false;
    param_bt_a2dp.is_suspend_setparam = false;
    param_bt_a2dp.dev_id = PAL_DEVICE_OUT_BLUETOOTH_A2DP;

    ret = pal_set_param(PAL_PARAM_ID_BT_A2DP_SUSPENDED, (void *)&param_bt_a2dp,
            sizeof(pal_param_bta2dp_t));
    if (ret)
        pa_log_error("BT set param for a2dp suspend failed");

    return ret;
}

int pa_pal_sink_get_media_config(pa_pal_sink_handle_t *handle, pa_sample_spec *ss, pa_channel_map *map, pa_encoding_t *encoding) {
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)handle;
    pa_format_info *f;

    uint32_t i;
    int ret = -1;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

    *ss = sdata->pa_sdata->sink->sample_spec;
    *map = sdata->pa_sdata->sink->channel_map;

    PA_IDXSET_FOREACH(f, sdata->pa_sdata->formats, i) {
        /* currently a sink supports single format */
        *encoding = f->encoding;
        ret = 0;
        break;
    }

    return ret;
}

pa_idxset* pa_pal_sink_get_config(pa_pal_sink_handle_t *handle) {
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)handle;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_assert(sdata->pa_sdata->sink);

    return pa_pal_sink_get_formats(sdata->pa_sdata->sink);
}

static int open_pal_sink(pa_pal_sink_data *sdata) {
    int rc = 0;
    pal_buffer_config_t out_buf_cfg, in_buf_cfg;
    pal_sink_data *pal_sdata;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);

    pal_sdata = sdata->pal_sdata;

#ifdef SINK_DUMP_ENABLED
    char *file_name;
#endif

    pa_assert(pal_sdata);

    pa_log_debug("opening sink with configuration type = 0x%x, format %d, sample_rate %d, channels: %d",
                 pal_sdata->stream_attributes->type, pal_sdata->stream_attributes->out_media_config.aud_fmt_id,
                 pal_sdata->stream_attributes->out_media_config.sample_rate,
                 pal_sdata->stream_attributes->out_media_config.ch_info.channels);

    rc = pal_stream_open(pal_sdata->stream_attributes, 1, pal_sdata->pal_device, 0, NULL, pa_pal_out_cb, (uint64_t)sdata,
                             &pal_sdata->stream_handle);

    if (rc) {
        pal_sdata->stream_handle = NULL;
        pa_log_error("Could not open output stream %d", rc);
        goto exit;
    }

    pa_log_debug("pal sink opened %p", pal_sdata->stream_handle);

    /* FIXME: Update it by calling pal_stream_get_buffer_size */
    in_buf_cfg.buf_size = 0;
    in_buf_cfg.buf_count = 0;
    out_buf_cfg.buf_size = pal_sdata->buffer_size;
    out_buf_cfg.buf_count = pal_sdata->buffer_count;
    rc = pal_stream_set_buffer_size(pal_sdata->stream_handle, &in_buf_cfg, &out_buf_cfg);
    if(rc) {
        pa_log_error("pal_stream_set_buffer_size failed\n");
        goto exit;
    }

    sdata->pal_sink_opened = true;
    pa_atomic_store(&pal_sdata->close_output, 0);

#ifdef SINK_DUMP_ENABLED
    file_name = pa_sprintf_malloc("/data/pcmdump_sink_%d", pal_sdata->index);

    pal_sdata->write_fd = open(file_name, O_RDWR | O_TRUNC | O_CREAT, S_IRWXU);
    if(pal_sdata->write_fd < 0)
        pa_log_error("Could not open write fd %d for sink index %d", pal_sdata->write_fd, pal_sdata->index);

    pa_xfree(file_name);
#endif

exit:
    return rc;
}

static int close_pal_sink(pa_pal_sink_data *sdata) {
    pal_sink_data *pal_sdata;
    pa_sink_data *pa_sdata;
    int rc = -1;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pa_sdata);

    pal_sdata = sdata->pal_sdata;
    pa_sdata = sdata->pa_sdata;

    pa_assert(pal_sdata->stream_handle);
    pa_atomic_store(&sdata->pal_sdata->close_output, 1);
    pa_mutex_lock(pal_sdata->mutex);

    pa_log_debug("closing pal sink %p", pal_sdata->stream_handle);

    if (PA_UNLIKELY(pal_sdata->stream_handle == NULL)) {
        pa_log_error("Invalid sink handle %p", pal_sdata->stream_handle);
    } else {
        rc = pal_stream_stop(pal_sdata->stream_handle);

        if (PA_UNLIKELY(rc))
            pa_log_error("pal_stream_stop failed for %p error %d", pal_sdata->stream_handle, rc);

        rc = pal_stream_close(pal_sdata->stream_handle);
        if (PA_UNLIKELY(rc))
            pa_log_error("could not close sink handle %p, error %d", pal_sdata->stream_handle, rc);

        pal_sdata->stream_handle = NULL;
        pal_sdata->bytes_written = 0;
        pal_sdata->standby = true;
#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
        pa_sdata->sink->sess_time = 0;
#endif
        sdata->pal_sink_opened = false;
    }

    pa_mutex_unlock(pal_sdata->mutex);
#ifdef SINK_DUMP_ENABLED
    close(pal_sdata->write_fd);
#endif

    return rc;
}

static int restart_pal_sink(pa_sink *s, pa_encoding_t encoding, pa_sample_spec *ss, pa_channel_map *map, pa_pal_card_port_device_data *port_device_data, pal_stream_type_t type,
                            int sink_id, pa_pal_sink_data *sdata,uint32_t buffer_size, uint32_t buffer_count) {
    int rc;
    pal_audio_fmt_t pal_format;

    pa_assert(s);
    pa_assert(sdata->pal_sdata);
    pa_assert(sdata->pa_sdata);

    pa_atomic_store(&sdata->pal_sdata->restart_in_progress, 1);
    if (sdata->pal_sink_opened && PA_SINK_IS_OPENED(s->thread_info.state)) {
        rc = close_pal_sink(sdata);
        if (rc) {
            pa_log_error("close_pal_sink failed, error %d", rc);
            goto exit;
        }
    }

    pal_format = pa_pal_util_get_pal_format_from_pa_encoding(encoding, sdata->pal_sdata->pal_snd_dec);
    if (!pal_format) {
        pa_log_error("%s: unsupported format", __func__);
        return -1;
    }

    if (!sdata->pal_sdata->compressed && (sdata->pa_sdata->avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_BIT_WIDTH)) {

        sdata->pal_sdata->stream_attributes->out_media_config.bit_width = pa_sample_size_of_format(ss->format) * PA_BITS_PER_BYTE;
        switch (sdata->pal_sdata->stream_attributes->out_media_config.bit_width) {
            case 32:
                if (ss->format == PA_SAMPLE_S24_32LE) {
                    /**<24bit in 32bit word (LSB aligned) little endian PCM*/
                    sdata->pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_LE;
                }
                else
                    sdata->pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S32_LE;
                break;
            case 24:
                sdata->pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_PCM_S24_3LE;
                break;
            default:
                sdata->pal_sdata->stream_attributes->out_media_config.aud_fmt_id = PAL_AUDIO_FMT_DEFAULT_PCM;
                break;
        }
    } else {
        sdata->pal_sdata->stream_attributes->out_media_config.aud_fmt_id = pal_format;
    }

    sdata->pal_sdata->stream_attributes->out_media_config.sample_rate = ss->rate;
    sdata->pal_sdata->pal_device->config.sample_rate = ss->rate;
    if (!pa_pal_channel_map_to_pal(map, &sdata->pal_sdata->stream_attributes->out_media_config.ch_info)) {
        pa_log_error("%s: unsupported channel map", __func__);
        return -1;
    }

    sdata->pal_sdata->compressed = (pal_format != PAL_AUDIO_FMT_PCM_S16_LE ? true : false);

    rc = open_pal_sink(sdata);
    if (rc) {
        pa_log_error("open_pal_sink failed during recreation, error %d", rc);
    }

exit:
    return rc;
}

static void free_pal_sink_thread_resources(pal_sink_data *pal_sdata){
    pa_assert(pal_sdata);

    pa_log_debug("%s Freeing pal sink thread resources", __func__);

    if (pal_sdata->pal_thread) {
        pa_asyncmsgq_send(pal_sdata->pal_thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(pal_sdata->pal_thread);
        pal_sdata->pal_thread = NULL;
    }

    if (pal_sdata->pal_rtpoll_item)
        pa_rtpoll_item_free(pal_sdata->pal_rtpoll_item);

    if (pal_sdata->pal_thread_rtpoll)
        pa_rtpoll_free(pal_sdata->pal_thread_rtpoll);

    if (pal_sdata->pal_fdsem)
        pa_fdsem_free(pal_sdata->pal_fdsem);

    pa_thread_mq_done(&pal_sdata->pal_thread_mq);

    if (pal_sdata->pal_msg)
        pa_xfree(pal_sdata->pal_msg);
}

static int free_pal_sink(pa_pal_sink_data *sdata) {
    int rc = 0;

    pa_assert(sdata);
    pa_assert(sdata->pal_sdata);

    if (!sdata->pal_sdata->standby) {
        rc = close_pal_sink(sdata);
        if (rc) {
            pa_log_error("close_pal_sink failed, error %d", rc);
        }
    }

    if (sdata->pal_sdata->compressed) {
        free_pal_sink_thread_resources(sdata->pal_sdata);
    }
    pa_mutex_free(sdata->pal_sdata->mutex);
    pa_cond_free(sdata->pal_sdata->cond_ctrl_thread);
    pa_xfree(sdata->pal_sdata->stream_attributes);
    pa_xfree(sdata->pal_sdata->pal_snd_dec);
    pa_xfree(sdata->pal_sdata->pal_device);
    pa_xfree(sdata->pal_sdata);
    sdata->pal_sdata = NULL;

    return rc;
}

static int create_pal_sink(pa_pal_sink_config *sink, pa_pal_card_port_device_data *port_device_data, pa_pal_sink_data *sdata) {
    int rc = 0;

    sdata->pal_sdata = pa_xnew0(pal_sink_data, 1);
    sdata->pal_sdata->mutex = pa_mutex_new(false /* recursive  */, false /* inherit_priority */);

    rc = pa_pal_sink_fill_info(sink, sdata->pal_sdata, port_device_data, PAL_AUDIO_FMT_DEFAULT_PCM);
    if (rc) {
        pa_log_error("pal sink init failed, error %d", rc);
        pa_xfree(sdata->pal_sdata);
        sdata->pal_sdata = NULL;
        return rc;
    }

    return rc;
}

static int pa_pal_sink_free_common_resources(pa_pal_sink_data *sdata) {
    pa_assert(sdata);

    if (sdata->fdsem)
        pa_fdsem_free(sdata->fdsem);

    return 0;
}

static int pa_pal_sink_alloc_common_resources(pa_pal_sink_data *sdata) {
    pa_assert(sdata);

   sdata->fdsem = pa_fdsem_new();
   if (!sdata->fdsem) {
       pa_log_error("Could not create fdsem");
       return -1;
   }

   return 0;
}

static int create_pa_sink(pa_module *m, char *sink_name, char *description, pa_idxset *formats, pa_sample_spec *ss, pa_channel_map *map, bool use_hw_volume, uint32_t alternate_sample_rate, pa_card *card,
                          pa_pal_card_avoid_processing_config_id_t avoid_config_processing, pa_hashmap *ports, const char *driver, pa_pal_sink_data *sdata) {
    pa_sink_new_data new_data;
    pa_sink_data *pa_sdata;
    pa_device_port *port;
    pa_format_info *format;
    pa_format_info *in_format;

    void *state;
    uint32_t i;

    bool port_sink_mapping = false;

    pa_assert(sdata->pal_sdata);

    pa_sdata = pa_xnew0(pa_sink_data, 1);
    pa_sink_new_data_init(&new_data);
    new_data.driver = driver;
    new_data.module = m;
    new_data.card = card;

    sdata->pa_sdata = pa_sdata;

    pa_sdata->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&pa_sdata->thread_mq, m->core->mainloop, pa_sdata->rtpoll);

    pa_sink_new_data_set_name(&new_data, sink_name);

    pa_log_info("ss->rate %d ss->channels %d", ss->rate, ss->channels);
    pa_sink_new_data_set_sample_spec(&new_data, ss);
    pa_sink_new_data_set_channel_map(&new_data, map);

    if (avoid_config_processing & PA_PAL_CARD_AVOID_PROCESSING_FOR_ALL){
        new_data.avoid_resampling_is_set = true;
        new_data.avoid_resampling = true;
    }
    else{
        new_data.avoid_resampling_is_set = false;
        new_data.avoid_resampling = false;

    }

    if (alternate_sample_rate == PA_ALTERNATE_SINK_RATE)
        pa_sink_new_data_set_alternate_sample_rate(&new_data, alternate_sample_rate);
    else if (alternate_sample_rate > 0)
        pa_log_error("%s: unsupported alternative sample rate %d",__func__, alternate_sample_rate);

    /* associate port with sink */
    PA_HASHMAP_FOREACH(port, ports, state) {
        pa_log_debug("adding port %s to sink %s", port->name, sink_name);
        pa_assert_se(pa_hashmap_put(new_data.ports, port->name, port) == 0);
        port_sink_mapping = true;
        pa_device_port_ref(port);
    }

    if (!port_sink_mapping) {
        pa_log_error("%s: sink_name %s creation failed as no port mapped, ",__func__, sink_name);
        goto fail;
    }

    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_STRING, pa_pal_sink_get_name_from_type(sdata->pal_sdata->stream_attributes->type));
    pa_proplist_sets(new_data.proplist, PA_PROP_DEVICE_DESCRIPTION, description);

    pa_sdata->sink = pa_sink_new(m->core, &new_data, PA_SINK_HARDWARE | PA_SINK_LATENCY );
    pa_sink_new_data_done(&new_data);

    if (!pa_sdata->sink) {
        pa_log_error("Could not create pa sink");
        goto fail;
    }

    pa_log_debug("pa sink opened %p", pa_sdata->sink);

    /* Creating PAL sink_write thread for compress offload stream type */
    if (sdata->pal_sdata->compressed) {
        pa_sdata->rtpoll_item = pa_rtpoll_item_new_fdsem(pa_sdata->rtpoll, PA_RTPOLL_NORMAL, sdata->fdsem);
        if (!pa_sdata->rtpoll_item) {
            pa_log_error("Could not create rpoll item");
            goto fail;
        }
        if (create_pal_sink_thread(sdata)) {
            pa_log_error("Failed to create pal sink thread");
            goto fail;
        }
    }

    pa_sdata->sink->userdata = (void *)sdata;
    pa_sdata->sink->parent.process_msg = pa_pal_sink_io_process_msg;
    pa_sdata->sink->set_state_in_io_thread = pa_pal_sink_set_state_in_io_thread_cb;
    pa_sdata->sink->set_port = pa_pal_sink_set_port_cb;
    pa_sdata->sink->reconfigure = pa_pal_sink_reconfigure_cb;
    pa_sdata->avoid_config_processing = avoid_config_processing;

    if (pa_idxset_size(formats) > 0 ) {
        pa_sdata->sink->get_formats = pa_pal_sink_get_formats;

        pa_sdata->formats = pa_idxset_new(NULL, NULL);

        PA_IDXSET_FOREACH(in_format, formats, i) {
            format = pa_format_info_copy(in_format);
            pa_idxset_put(pa_sdata->formats, format, NULL);
        }
    }

#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
    pa_sdata->sink->set_format = pa_pal_sink_set_format_cb;
    pa_sdata->sink->drain = pa_pal_sink_drain_cb;
    pa_sdata->sink->flush = pa_pal_sink_flush_cb;
    pa_sdata->sink->sess_time = 0;
#endif

    pa_sink_set_asyncmsgq(pa_sdata->sink, pa_sdata->thread_mq.inq);
    pa_sink_set_rtpoll(pa_sdata->sink, pa_sdata->rtpoll);
    pa_sink_set_max_request(pa_sdata->sink, sdata->pal_sdata->buffer_size);
    pa_sink_set_max_rewind(pa_sdata->sink, 0);
    pa_sink_set_fixed_latency(pa_sdata->sink, sdata->pal_sdata->sink_latency_us);

    if (use_hw_volume) {
        pa_sdata->sink->n_volume_steps = PA_VOLUME_NORM+1; /* FIXME: What should be value */
        pa_sink_set_set_volume_callback(pa_sdata->sink, pa_pal_sink_set_volume_cb);
    }

    pa_sdata->thread = pa_thread_new(sink_name, pa_pal_sink_thread_func, sdata);
    if (PA_UNLIKELY(pa_sdata->thread == NULL)) {
        pa_log_error("Could not spawn I/O thread");
        goto fail;
    }

   pa_sink_put(pa_sdata->sink);

   return 0;

fail :
    if (pa_sdata)
        free_pa_sink(sdata);

    return -1;
}

static int free_pa_sink(pa_pal_sink_data *sdata) {
    pa_sink_data *pa_sdata;

    pa_assert(sdata);
    pa_assert(sdata->pa_sdata);
    pa_sdata = sdata->pa_sdata;

    pa_log_debug("closing pa sink %p", sdata->pa_sdata->sink);

    if (pa_sdata->sink) {
        if (PA_SINK_IS_OPENED(pa_sdata->sink->thread_info.state))
            pa_sink_suspend(pa_sdata->sink, PA_SINK_SUSPENDED, PA_SUSPEND_USER);
        pa_sink_unlink(pa_sdata->sink);
    }

    if (pa_sdata->thread) {
        pa_asyncmsgq_send(pa_sdata->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(pa_sdata->thread);
    }

    pa_thread_mq_done(&pa_sdata->thread_mq);

    if (pa_sdata->sink)
        pa_sink_unref(pa_sdata->sink);

    if (pa_sdata->formats)
        pa_idxset_free(pa_sdata->formats, (pa_free_cb_t) pa_format_info_free);

    if (pa_sdata->rtpoll)
        pa_rtpoll_free(pa_sdata->rtpoll);

    if (pa_sdata->rtpoll_item)
        pa_rtpoll_item_free(pa_sdata->rtpoll_item);

    pa_xfree(pa_sdata);

    return 0;
}

bool pa_pal_sink_is_supported_sample_rate(uint32_t sample_rate) {
    bool supported = false;
    uint32_t i;

    for (i = 0; i < ARRAY_SIZE(supported_sink_rates) ; i++) {
        if (sample_rate == supported_sink_rates[i]) {
            supported = true;
            break;
        }
    }

    return supported;
}

int pa_pal_sink_create(pa_module *m, pa_card *card, const char *driver, const char *module_name, pa_pal_sink_config *sink, pa_pal_sink_handle_t **handle) {
    int rc = -1;
    pa_pal_sink_data *sdata;
    pa_device_port *card_port;
    pa_pal_card_port_config *sink_port;
    pa_hashmap *ports;
    pa_pal_card_port_device_data *port_device_data;

    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state;

    pa_assert(m);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module_name);
    pa_assert(sink);
    pa_assert(sink->name);
    pa_assert(sink->description);
    pa_assert(sink->formats);
    pa_assert(sink->ports);

    if (pa_hashmap_isempty(sink->ports)) {
        pa_log_error("%s: empty port list", __func__);
        goto exit;
    }

    /*convert config port to card port */
    ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    PA_HASHMAP_FOREACH(sink_port, sink->ports, state) {
        if ((card_port = pa_hashmap_get(card->ports, sink_port->name)))
            pa_hashmap_put(ports, card_port->name, card_port);
    }

    /* first entry is default device */
    card_port = pa_hashmap_first(ports);
    port_device_data = PA_DEVICE_PORT_DATA(card_port);
    pa_assert(port_device_data);

    pa_log_info("%s: creating sink with ss %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &sink->default_spec));

    sdata = pa_xnew0(pa_pal_sink_data, 1);

    rc = pa_pal_sink_alloc_common_resources(sdata);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could pa_pal_sink_alloc_common_resources, error %d", rc);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pal_sink(sink, port_device_data, sdata);
    if (PA_UNLIKELY(rc))  {
        pa_log_error("Could create open pal sink, error %d", rc);
        pa_pal_sink_free_common_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    rc = create_pa_sink(m, sink->name, sink->description, sink->formats, &sink->default_spec, &sink->default_map, sink->use_hw_volume, sink->alternate_sample_rate, card, sink->avoid_config_processing, ports, driver, sdata);
    pa_hashmap_free(ports);
    if (PA_UNLIKELY(rc)) {
        pa_log_error("Could not create pa sink for sink %s, error %d", sink->name, rc);
        free_pal_sink(sdata);
        pa_pal_sink_free_common_resources(sdata);
        pa_xfree(sdata);
        sdata = NULL;
        goto exit;
    }

    *handle = (pa_pal_sink_handle_t *)sdata;
    pa_idxset_put(mdata->sinks, sdata, NULL);

exit:
    return rc;
}

void pa_pal_sink_close(pa_pal_sink_handle_t *handle) {
    pa_pal_sink_data *sdata = (pa_pal_sink_data *)handle;

    pa_assert(sdata);

    free_pa_sink(sdata);
    free_pal_sink(sdata);
    pa_pal_sink_free_common_resources(sdata);

    pa_idxset_remove_by_data(mdata->sinks, sdata, NULL);

    pa_xfree(sdata);
}

void pa_pal_sink_module_deinit() {

    pa_assert(mdata);

    pa_idxset_free(mdata->sinks, NULL);

    pa_xfree(mdata);
    mdata = NULL;
}

void pa_pal_sink_module_init() {

    mdata = pa_xnew0(pa_pal_sink_module_data, sizeof(pa_pal_sink_module_data));

    mdata->sinks = pa_idxset_new(NULL, NULL);
}
