/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <pulsecore/device-port.h>
#include <pulsecore/card.h>
#include <pulsecore/core-util.h>
#include <pulsecore/thread.h>
#include <pulsecore/protocol-dbus.h>

#include <fcntl.h>
#include <unistd.h>

#include "pal-config-parser.h"
#include "pal-sink.h"
#include "pal-source.h"
#include "pal-utils.h"
#include "pal-loopback.h"

#define PAL_CARD_DEFAULT_CONF_NAME "default.conf"
#define PAL_CARD_DEFAULT_TARGET_NAME_LENGTH 7

#define PAL_CARD_PORT_PREFIX "Port "
#define PAL_CARD_PROFILE_PREFIX "Profile "
#define PAL_CARD_SINK_PREFIX "Sink "
#define PAL_CARD_SOURCE_PREFIX "Source "
#define PAL_CARD_LOOPBACK_PREFIX "Loopback "
#define PAL_CARD_SND_SUFFIX "snd-card"

#define MAX_RETRY 100
#define SNDCARD_PATH "/sys/kernel/snd_card/card_state"
#define RETRY_INTERVAL 1

#define MAX_BUF_SIZE 256

/** Sound card state */
typedef enum snd_card_status_t {
    SND_CARD_STATUS_OFFLINE = 0,
    SND_CARD_STATUS_ONLINE  = 1,
} snd_card_status_t;

static pa_pal_sink_config* pa_pal_config_get_sink(pa_hashmap *sinks, char *name);
static pa_pal_source_config *pa_pal_config_get_source(pa_hashmap *sources, char *name);
static pa_pal_card_profile_config* pa_pal_config_get_profile(pa_hashmap *profiles, char *name);
static pa_pal_card_port_config* pa_pal_config_get_port(pa_hashmap *ports, char *name);
static pa_pal_loopback_config* pa_pal_config_get_loopback(pa_hashmap *loopback_profiles, char *name);

static pa_pal_source_config* pa_pal_config_get_source(pa_hashmap *sources, char *name) {
    pa_pal_source_config *source = NULL;

    pa_assert(sources);
    pa_assert(name);

    if (!pa_startswith(name, PAL_CARD_SOURCE_PREFIX)) {
        goto exit;
    }
    /* point to Port name */
    name += strlen(PAL_CARD_SOURCE_PREFIX);

    source = pa_hashmap_get(sources, name);
    if (source) {
        goto exit;
    }

    source = pa_xnew0(pa_pal_source_config, 1);

    source->ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    source->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    source->formats = pa_idxset_new(NULL, NULL);

    source->name = pa_xstrdup(name);

    pa_log_debug("%s::source name is %s", __func__, source->name);

    pa_hashmap_put(sources, source->name, source);

    source->id = pa_hashmap_size(sources);

exit:
    return source;
}

static pa_pal_sink_config* pa_pal_config_get_sink(pa_hashmap *sinks, char *name) {
    pa_pal_sink_config *sink = NULL;

    pa_assert(sinks);
    pa_assert(name);

    if (!pa_startswith(name, PAL_CARD_SINK_PREFIX)) {
        goto exit;
    }
    /* point to Port name */
    name += strlen(PAL_CARD_SINK_PREFIX);

    sink = pa_hashmap_get(sinks, name);
    if (sink) {
        goto exit;
    }

    sink = pa_xnew0(pa_pal_sink_config, 1);


    sink->ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    sink->profiles = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    sink->formats = pa_idxset_new(NULL, NULL);

    sink->name = pa_xstrdup(name);

    pa_log_debug("%s::sink name is %s", __func__, sink->name);

    pa_hashmap_put(sinks, sink->name, sink);

    sink->id = pa_hashmap_size(sinks);

exit:
    return sink;
}

static int pa_pal_config_parse_encodings(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_encoding_t encoding;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;

    pa_format_info *format;
    pa_idxset *formats = NULL;

    int ret = -1;
    char **items = NULL;
    char *item;
    int i = 0;
    char *name = NULL;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        name = sink->name;
        formats = sink->formats;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        name = source->name;
        formats = source->formats;
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        name = port->name;
        formats = port->formats;
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

    items = pa_split_spaces_strv(state->rvalue);
    if (!items) {
        pa_log_error("%s: [%s:%u] encoding list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    /* add port profile port hashmap */
    while ((item = items[i++])) {
        encoding = pa_encoding_from_string(item);

        if (sink && !pa_pal_sink_is_supported_encoding(encoding)) {
            pa_log_error("%s: unsupported sink encoding %s sink %s", __func__, item, name);
            goto exit;
        } else if (source && !pa_pal_source_is_supported_encoding(encoding)) {
            pa_log_error("%s: unsupported source encoding %s source %s", __func__, item, name);
            goto exit;
        }

        format = pa_format_info_new();
        format->encoding = encoding;

        pa_idxset_put(formats, format, NULL);

        pa_log_debug("%s: adding encoding %s to usecase %s", __func__, item, name);
    }
    ret = 0;

exit:
    if (items)
        pa_xstrfreev(items);

    return ret;
}

static int pa_pal_config_parse_type(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;

    int ret = -1;
    int i = 0;
    char **items = NULL;
    char *item;
    char *name;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        name = sink->name;
        items = pa_split_spaces_strv(state->rvalue);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        name = source->name;
        items = pa_split_spaces_strv(state->rvalue);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

    if (!items) {
        pa_log_error("%s: [%s:%u] flag list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    /* add port profile port hashmap */
    while ((item = items[i++])) {
        if (sink) {
            sink->stream_type = pa_pal_sink_get_type_from_string(item);
            pa_log_debug("%s: adding flag %s to sink %s", __func__, item, name);
        } else {
            source->stream_type = pa_pal_source_get_type_from_string(item);
            pa_log_debug("%s: adding flag %s to source %s", __func__, item, name);
        }
    }

    ret = 0;

exit:
    if (items)
        pa_xstrfreev(items);
    return ret;
}

static int pa_pal_config_parse_avoid_processing(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = NULL;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    char **items = NULL;
    char *item = NULL;
    char *name = NULL;
    int i = 0;
    int ret = -1;

    pa_assert(state);
    pa_assert(state->rvalue);
    pa_assert(state->userdata);

    config_data = state->userdata;
    items = pa_split_spaces_strv(state->rvalue);

    if (!items) {
        pa_log_error("%s: [%s:%u] flag list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        name = sink->name;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        name = source->name;
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

    /* add list to sink/source */
    while ((item = items[i++])) {
        if (sink) {
            sink->avoid_config_processing |= pa_pal_utils_get_config_id_from_string(item);
            pa_log_debug("%s: Adding %s to the list of configs to avoid processing for sink %s", __func__, item, name);
        } else {
            source->avoid_config_processing |= pa_pal_utils_get_config_id_from_string(item);
            pa_log_debug("%s: Adding %s to the list of configs to avoid processing for source %s", __func__, item, name);
        }
    }

    ret = 0;

exit:
    if (items)
        pa_xstrfreev(items);

    return ret;
}

static int pa_pal_config_parse_default_encoding(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_encoding_t encoding;

    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if (!(encoding = pa_encoding_from_string(state->rvalue))) {
        pa_log_error("%s: [%s:%u] invalid encoding %s", __func__, state->filename, state->lineno, state->rvalue);
        goto exit;
    }

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        if (!pa_pal_sink_is_supported_encoding(encoding)) {
            pa_log_error("%s: unsupported sink encoding %s sink %s", __func__, state->rvalue, sink->name);
            goto exit;
        }
        sink->default_encoding = encoding;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        if (!pa_pal_source_is_supported_encoding(encoding)) {
            pa_log_error("%s: unsupported source encoding %s source %s", __func__, state->rvalue, source->name);
            goto exit;
        }
        source->default_encoding = encoding;
    } else {
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_default_sample_rate(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;

    int ret = -1;
    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        pa_atou(state->rvalue, &sink->default_spec.rate);
        if (!pa_pal_sink_is_supported_sample_rate(sink->default_spec.rate)) {
            pa_log_error("%s: unsupported  sample rate %d by sink %s", __func__, sink->default_spec.rate, sink->name);
            goto exit;
        }
        pa_log_debug("%s: default sample rate %d for sink %s", __func__, sink->default_spec.rate, sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        pa_atou(state->rvalue, &source->default_spec.rate);
        if (!pa_pal_source_is_supported_sample_rate(source->default_spec.rate)) {
            pa_log_error("%s: unsupported  sample rate %d by source %s", __func__, source->default_spec.rate, source->name);
            goto exit;
        }
        pa_log_debug("%s: default sample rate %d for source %s", __func__, source->default_spec.rate, source->name);
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        pa_atou(state->rvalue, &port->default_spec.rate);
        pa_log_debug("%s: default sample rate %d for port %s", __func__, port->default_spec.rate, port->name);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

   ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_default_sample_format(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;

    int ret = -1;
    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        sink->default_spec.format = pa_parse_sample_format(state->rvalue);
        pa_log_debug("%s: default sample format %s to usecase %s", __func__, state->rvalue, sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        source->default_spec.format = pa_parse_sample_format(state->rvalue);
        pa_log_debug("%s: default sample format %s to usecase %s", __func__, state->rvalue, source->name);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

   ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_default_channel_map(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;

    pa_channel_map map;
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];

    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if (!pa_channel_map_parse(&map, state->rvalue)) {
        pa_log_error("%s: [%s:%u] invalid channel map", __func__, state->filename, state->lineno);
        goto exit;
    }

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        sink->default_map = map;
        sink->default_spec.channels = map.channels;
        pa_log_debug("%s adding default channel map %s to sink %s", __func__, pa_channel_map_snprint(cm, sizeof(cm), &map), sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        source->default_map = map;
        source->default_spec.channels = map.channels;
        pa_log_debug("%s adding default channel map %s to source %s", __func__, pa_channel_map_snprint(cm, sizeof(cm), &map), source->name);
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        port->default_map = map;
        port->default_spec.channels = map.channels;
        pa_log_debug("%s adding default channel map %s to port %s", __func__, pa_channel_map_snprint(cm, sizeof(cm), &map), port->name);
    } else {
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_default_buffer_size(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;

    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        pa_atou(state->rvalue, &sink->buffer_size);
        pa_log_debug("%s adding default buffer size %d to sink %s", __func__, sink->buffer_size, sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        pa_atou(state->rvalue, &source->buffer_size);
        pa_log_debug("%s adding default buffer size %d to source %s", __func__, source->buffer_size, source->name);
    } else {
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_default_buffer_count(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;

    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        pa_atou(state->rvalue, &sink->buffer_count);
        pa_log_debug("%s adding default buffer count %d to sink %s", __func__, sink->buffer_count, sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        pa_atou(state->rvalue, &source->buffer_count);
        pa_log_debug("%s adding default buffer count %d to source %s", __func__, source->buffer_count, source->name);
    } else {
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_sample_rates(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;

    pa_format_info *format;
    pa_idxset *formats = NULL;

    int ret = -1;
    char **items = NULL;
    char *item;
    char *name;
    int32_t *sample_rates = NULL;
    int i = 0;
    uint32_t j = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        formats = sink->formats;
        name = sink->name;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        formats = source->formats;
        name = source->name;
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        formats = port->formats;
        name = port->name;
    } else {
        goto exit;
    }

    if (pa_idxset_isempty(formats)) {
        pa_log_error("%s: [%s:%u] encoding list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    if (!(items = pa_split_spaces_strv(state->rvalue))) {
        pa_log_error("%s: [%s:%u] sample rate list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    /* get number of entries */
    while ((item = items[i++]));

    pa_log_info("%s: number fo sample rate %d",__func__, i-1);

    if (i-1 < 0)
        goto exit;

    sample_rates = pa_xnew0(int32_t, i-1);

    i = 0;
    /* add port profile port hashmap */
    while ((item = items[i])) {
        if ((pa_atoi(item, &sample_rates[i]) < 0)) {
            pa_log_error("%s: [%s:%u] invalid sample rate", __func__, state->filename, state->lineno);
            goto exit;
        }

        if (sink && !pa_pal_sink_is_supported_sample_rate(sample_rates[i])) {
            pa_log_error("%s: unsupported  sample rate %d by sink %s", __func__, sample_rates[i], sink->name);
            goto exit;
        } else if (source && !pa_pal_source_is_supported_sample_rate(sample_rates[i])) {
            pa_log_error("%s: unsupported sample rate %d by source %s", __func__, sample_rates[i], source->name);
            goto exit;
        }

        pa_log_debug("%s: adding sample rate %d to %s", __func__, sample_rates[i], name);
        i++;
    }

    PA_IDXSET_FOREACH(format, formats, j)
        pa_format_info_set_prop_int_array(format, PA_PROP_FORMAT_RATE, sample_rates, i);

    ret = 0;

exit:
    if (sample_rates)
        pa_xfree(sample_rates);

    if (items)
        pa_xstrfreev(items);

    return ret;
}

static int pa_pal_config_parse_sample_formats(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;

    pa_format_info *format;
    pa_idxset *formats = NULL;

    int ret = -1;
    char **items = NULL;
    char *item;
    char *name;
    int32_t *sample_formats = NULL;
    int i = 0;
    uint32_t j = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        formats = sink->formats;
        name = sink->name;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        formats = source->formats;
        name = source->name;
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        formats = port->formats;
        name = port->name;
     } else {
        goto exit;
    }

    if (pa_idxset_isempty(formats)) {
        pa_log_error("%s: [%s:%u] encoding list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    if (!(items = pa_split_spaces_strv(state->rvalue))) {
        pa_log_error("%s: [%s:%u] sample format list missing", __func__, state->filename, state->lineno);
        goto exit;
    }

    /* get number of entries */
    while ((item = items[i++]));

    pa_log_info("%s: number fo sample format %d",__func__, i-1);

    if (i-1 < 0)
        goto exit;

    sample_formats = pa_xnew0(int32_t, i-1);

    i = 0;
    /* add port profile port hashmap */
    while ((item = items[i])) {
       sample_formats[i] = pa_parse_sample_format(item);

        pa_log_debug("%s: adding sample format %d to usecase %s", __func__, sample_formats[i], name);
        i++;
    }

    PA_IDXSET_FOREACH(format, formats, j)
        pa_format_info_set_prop_string_array(format, PA_PROP_FORMAT_SAMPLE_FORMAT, (const char **)items, i);

    ret = 0;

exit:
    if (sample_formats)
        pa_xfree(sample_formats);

    if (items)
        pa_xstrfreev(items);

    return ret;
}

static int pa_pal_config_parse_channel_maps(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;

    pa_format_info *format;
    pa_idxset *formats = NULL;
    pa_channel_map map;

    int ret = -1;
    uint32_t i = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        formats = sink->formats;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        formats = source->formats;
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        formats = port->formats;
    } else {
        goto exit;
    }

    if (!pa_channel_map_parse(&map, state->rvalue)) {
        pa_log_error("%s: [%s:%u] invalid channel map", __func__, state->filename, state->lineno);
        goto exit;
    }

    PA_IDXSET_FOREACH(format, formats, i) {
        pa_log_debug("%s: adding default channel map %s count %d to encoding %s", __func__, state->rvalue, map.channels, pa_encoding_to_string(format->encoding));
        pa_format_info_set_channel_map(format, &map);
        pa_format_info_set_channels(format, map.channels);
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_alternative_sample_rate(pa_config_parser_state *state) {

    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;

    int ret = -1;
    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        pa_atou(state->rvalue, &sink->alternate_sample_rate);
        pa_log_debug("%s: alternate sample rate %d for sink %s", __func__, sink->alternate_sample_rate, sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        pa_atou(state->rvalue, &source->alternate_sample_rate);
        pa_log_debug("%s: alternate sample rate %d for source %s", __func__, source->alternate_sample_rate, source->name);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

   ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_pal_devicepp_config(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = NULL;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_card_port_config *port = NULL;
    int ret = 0;

    pa_assert(state);
    pa_assert(state->rvalue);

    config_data = state->userdata;
    pa_assert(config_data);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        sink->pal_devicepp_config = pa_xstrdup(state->rvalue);
        pa_log_debug("%s: pal devicepp config is %s for sink %s", __func__, sink->pal_devicepp_config, sink->name);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        source->pal_devicepp_config = pa_xstrdup(state->rvalue);
        pa_log_debug("%s: pal devicepp config is %s for source %s", __func__, source->pal_devicepp_config, source->name);
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        port->pal_devicepp_config = pa_xstrdup(state->rvalue);
        pa_log_debug("%s: pal devicepp config is %s for port %s", __func__, port->pal_devicepp_config, port->name);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        ret = -1;
    }

    return ret;
}

static int pa_pal_config_parse_presence(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_port_config *port;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;

    pa_pal_card_usecase_type_t *usecase_type = NULL;

    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        if (pa_streq(state->rvalue, "always")) {
            port->available = PA_AVAILABLE_YES;
        } else if (pa_streq(state->rvalue, "dynamic")) {
            port->available = PA_AVAILABLE_NO;
        } else if (port && pa_streq(state->rvalue, "static")) {
            port->available = PA_AVAILABLE_UNKNOWN;
        } else {
            pa_log_error("%s: invalid port state %s(it should be always, dynamic or static)", __func__, state->rvalue);
            goto exit;
        }
    } else if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        usecase_type = &sink->usecase_type;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        usecase_type = &source->usecase_type;
   } else  {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

    if (sink || source) {
        if (pa_streq(state->rvalue, "always")) {
            *usecase_type = PA_PAL_CARD_USECASE_TYPE_STATIC;
        } else if (pa_streq(state->rvalue, "dynamic")) {
            *usecase_type = PA_PAL_CARD_USECASE_TYPE_DYNAMIC;
        } else {
            pa_log_error("%s: invalid sink state %s(it should be always or dynamic)", __func__, state->rvalue);
            goto exit;
        }
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_use_hw_volume(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        sink->use_hw_volume = pa_parse_boolean(state->rvalue);
        ret = 0;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        source->use_hw_volume = pa_parse_boolean(state->rvalue);
        ret = 0;
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
    }

    return ret;
}


static void pa_pal_config_free_sink(pa_pal_sink_config *sink) {
    pa_assert(sink);

    pa_log_info("%s: freeing sink %s", __func__, sink->name);

    pa_xfree(sink->name);

    pa_xfree(sink->description);

    pa_hashmap_free(sink->ports);

    pa_hashmap_free(sink->profiles);

    pa_idxset_free(sink->formats, (pa_free_cb_t) pa_format_info_free);

    if (sink->port_conf_string)
        pa_xstrfreev(sink->port_conf_string);

    if (sink->pal_devicepp_config)
        pa_xfree(sink->pal_devicepp_config);

    pa_xfree(sink);
} /* end sink parsing related functions */

static void pa_pal_config_free_source(pa_pal_source_config *source) {
    pa_assert(source);

    pa_log_info("%s: freeing source %s", __func__, source->name);

    pa_xfree(source->name);

    pa_xfree(source->description);

    pa_hashmap_free(source->ports);

    pa_hashmap_free(source->profiles);

    pa_idxset_free(source->formats, (pa_free_cb_t) pa_format_info_free);

    if (source->port_conf_string)
        pa_xstrfreev(source->port_conf_string);

    if (source->pal_devicepp_config)
        pa_xfree(source->pal_devicepp_config);

    pa_xfree(source);
} /* end source parsing related functions */

/* common between port, profile and sink*/
static int pa_pal_config_parse_description(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_profile_config *profile;
    pa_pal_card_port_config *port;
    pa_pal_sink_config *sink;
    pa_pal_source_config *source;
    pa_pal_loopback_config *loopback_config = NULL;

    int ret = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((profile = pa_pal_config_get_profile(config_data->profiles, state->section))) {
        profile->description = pa_xstrdup(state->rvalue);
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        port->description = pa_xstrdup(state->rvalue);
    } else if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        sink->description = pa_xstrdup(state->rvalue);
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        source->description = pa_xstrdup(state->rvalue);
    } else if ((loopback_config = pa_pal_config_get_loopback(config_data->loopbacks, state->section))) {
        loopback_config->description = pa_xstrdup(state->rvalue);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        ret = -1;
    }

    return ret;
}

/* common between port and profile */
static int pa_pal_config_parse_priority(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_profile_config *profile;
    pa_pal_card_port_config *port;

    int ret = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((profile = pa_pal_config_get_profile(config_data->profiles, state->section))) {
        if (pa_atou(state->rvalue, &profile->priority) < 0) {
            pa_log("%s: Invalid profile priority", __func__);
        }
    } else if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        if (pa_atou(state->rvalue, &port->priority) < 0) {
            pa_log("%s: Invalid port priority", __func__);
        }
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        ret = -1;
    }

    return ret;
}

static pa_pal_card_profile_config* pa_pal_config_get_profile(pa_hashmap *profiles, char *name) {
    pa_pal_card_profile_config *profile = NULL;

    pa_assert(profiles);
    pa_assert(name);

    if (!pa_startswith(name, PAL_CARD_PROFILE_PREFIX)) {
        goto exit;
    }

    /* point to Port name */
    name += strlen(PAL_CARD_PROFILE_PREFIX);

    profile = pa_hashmap_get(profiles, name);
    if (profile) {
        goto exit;
    }

    profile = pa_xnew0(pa_pal_card_profile_config, 1);

    profile->ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    profile->name = pa_xstrdup(name);

    pa_log_debug("%s::profile name is %s", __func__, profile->name);

    pa_hashmap_put(profiles, profile->name, profile);

exit:
    return profile;
}

static int pa_pal_config_parse_profile_max_sink_channels(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_profile_config *profile;

    int ret = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((profile = pa_pal_config_get_profile(config_data->profiles, state->section))) {
        if (pa_atou(state->rvalue, &profile->max_sink_channels) < 0) {
            pa_log("%s: Invalid profile sink channel count", __func__);
            ret = -1;
        }
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        ret = -1;
    }

    return ret;
}

static int pa_pal_config_parse_profile_max_source_channels(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_profile_config *profile;

    int ret = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((profile = pa_pal_config_get_profile(config_data->profiles, state->section))) {
        if (pa_atou(state->rvalue, &profile->max_source_channels) < 0) {
            pa_log("%s: Invalid profile source channel count", __func__);
            ret = -1;
        }
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        ret = -1;
    }

    return ret;
}

static int pa_pal_config_parse_port_names(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;

    pa_pal_card_profile_config *profile1;
    pa_pal_card_profile_config *profile;
    pa_pal_card_port_config *port;
    pa_pal_sink_config *sink = NULL;
    pa_pal_source_config *source = NULL;
    pa_pal_loopback_config *loopback_config = NULL;

    pa_hashmap *ports = NULL;
    pa_hashmap *profiles = NULL;

    int ret = 0;
    int i = 0;
    char **items = NULL;
    char *port_name;
    char *name;
    void *hashmap_state;

    pa_log_info("%s", __func__);

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((profile = pa_pal_config_get_profile(config_data->profiles, state->section))) {
        ports = profile->ports;
        profile->port_conf_string = pa_split_spaces_strv(state->rvalue);
        items = profile->port_conf_string;
        name = profile->name;
    } else if ((sink = pa_pal_config_get_sink(config_data->sinks, state->section))) {
        ports = sink->ports;
        sink->port_conf_string = pa_split_spaces_strv(state->rvalue);
        items = sink->port_conf_string;
        profiles = sink->profiles;
        name = sink->name;
    } else if ((source = pa_pal_config_get_source(config_data->sources, state->section))) {
        ports = source->ports;
        source->port_conf_string = pa_split_spaces_strv(state->rvalue);
        items = source->port_conf_string;
        profiles = source->profiles;
        name = source->name;
    } else if ((loopback_config = pa_pal_config_get_loopback(config_data->loopbacks, state->section))) {
        name = loopback_config->name;
        items = pa_split_spaces_strv(state->rvalue);
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        ret = -1;
        goto exit;
    }

    if (!items) {
        pa_log_error("%s: [%s:%u] port name missing", __func__, state->filename, state->lineno);
        ret = -1;
        goto exit;
    }

    /* add port profile port hashmap */
    while ((port_name = items[i++])) {
        /* check if its valid port */
        port = pa_hashmap_get(config_data->ports, port_name);
        if (!port) {
            pa_log_error("%s: invalid port %s", __func__, port_name);
            pa_xstrfreev(items);
            ret = -1;
            goto exit;
        }

	/* handle loopback separately as it has both in and out ports */
        if (loopback_config) {
            if (port->direction == PA_DIRECTION_INPUT) {
                ports = loopback_config->in_ports;
                if (!loopback_config->in_port_conf_string)
                    loopback_config->in_port_conf_string = items;
            } else {
                ports = loopback_config->out_ports;
                if (!loopback_config->out_port_conf_string)
                    loopback_config->out_port_conf_string = items;
            }
        }

        pa_log_debug("%s: adding port %s to %s", __func__, port->name, name);
        pa_hashmap_put(ports, port_name, port);

        /* port list is only for profile */
        if (!source && !sink)
            continue;

        /* add a profile to sink/source if this port belong to this sink/source */
        PA_HASHMAP_FOREACH(profile1, config_data->profiles, hashmap_state) {
            if (pa_hashmap_get(profile1->ports, port->name)) {
                if (!pa_hashmap_get(profiles, profile1->name)) {
                    pa_log_debug("%s: adding profile %s to usecase %s", __func__, profile1->name, name);
                    pa_hashmap_put(profiles, profile1->name, profile1);

                    /* update source and sink count */
                    if (sink)
                        profile1->n_sinks++;
                    else
                        profile1->n_sources++;
                }
            }
        }
    }

exit:
    return ret;
}

/* Add unique loopback profile to hashmap */
static pa_pal_loopback_config* pa_pal_config_get_loopback(pa_hashmap *loopback_profiles, char *name) {
    pa_pal_loopback_config *loopback_config = NULL;

    pa_assert(loopback_profiles);
    pa_assert(name);

    if (!pa_startswith(name, PAL_CARD_LOOPBACK_PREFIX)) {
        goto exit;
    }
    /* point to Port name */
    name += strlen(PAL_CARD_LOOPBACK_PREFIX);

    /* Do not add if already there */
    loopback_config = pa_hashmap_get(loopback_profiles, name);
    if (loopback_config) {
        goto exit;
    }

    loopback_config = pa_xnew0(pa_pal_loopback_config, 1);
    if (!loopback_config) {
        return NULL;
    }
    loopback_config->name = pa_xstrdup(name);
    loopback_config->in_ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
    loopback_config->out_ports = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    pa_log_debug("%s: loopback name is %s", __func__, loopback_config->name);

    pa_hashmap_put(loopback_profiles, loopback_config->name, loopback_config);

exit:
    return loopback_config;
}

static void pa_pal_config_free_loopback(pa_pal_loopback_config *loopback_config) {
    pa_assert(loopback_config);

    pa_log_info("%s: freeing loopback %s", __func__, loopback_config->name);

    pa_xfree(loopback_config->name);

    pa_xfree(loopback_config->description);

    pa_hashmap_free(loopback_config->in_ports);

    pa_hashmap_free(loopback_config->out_ports);

    pa_xfree(loopback_config);
}

static void pa_pal_config_free_profile(pa_pal_card_profile_config *profile) {
    pa_assert(profile);

    pa_log_info("%s: freeing profile %s", __func__, profile->name);

    pa_xfree(profile->name);

    pa_xfree(profile->description);

    pa_hashmap_free(profile->ports);

    if (profile->port_conf_string)
        pa_xstrfreev(profile->port_conf_string);


    pa_xfree(profile);
} /* end profile parsing related functions */

static pa_pal_card_port_config* pa_pal_config_get_port(pa_hashmap *ports, char *name) {
    pa_pal_card_port_config *port = NULL;

    pa_assert(ports);
    pa_assert(name);

    if (!pa_startswith(name, PAL_CARD_PORT_PREFIX))
        goto exit;

    /* point to Port name */
    name += strlen(PAL_CARD_PORT_PREFIX);

    port = pa_hashmap_get(ports, name);
    if (port) {
        goto exit;
    }

    port = pa_xnew0(pa_pal_card_port_config, 1);
    port->name = pa_xstrdup(name);
    port->formats = pa_idxset_new(NULL, NULL);

    pa_log_debug("%s::port name is %s", __func__, port->name);

    pa_hashmap_put(ports, port->name, port);

exit:
    return port;
}

static int pa_pal_config_parse_port_device(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_port_config *port;
    int ret = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    port = pa_pal_config_get_port(config_data->ports, state->section);
    if (!port) {
        ret = -1;
        goto exit;
    }

    port->device = pa_pal_util_device_name_to_enum((const char *)state->rvalue);
    if (port->device != PAL_DEVICE_NONE) {
        goto exit;
    } else {
        pa_log_error("%s: invalid port device %s ", __func__, state->rvalue);
        ret = -1;
    }

exit:
    return ret;
}

static int pa_pal_config_parse_port_direction(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_port_config *port;
    int ret = 0;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    port = pa_pal_config_get_port(config_data->ports, state->section);
    if (!port) {
        ret = -1;
        goto exit;
    }

    if (pa_streq(state->rvalue, "in")) {
        port->direction = PA_DIRECTION_INPUT;
    } else if (pa_streq(state->rvalue, "out")) {
        port->direction = PA_DIRECTION_OUTPUT;
    } else {
        ret = -1;
        pa_log_error("%s: invalid port state %s(it should be out or in)", __func__, state->rvalue);
    }

exit:
    return ret;
}

static void pa_pal_config_free_port(pa_pal_card_port_config *port) {
    pa_assert(port);

    pa_log_info("%s: freeing port %s", __func__, port->name);

    pa_xfree(port->name);

    pa_xfree(port->description);

    pa_idxset_free(port->formats, (pa_free_cb_t) pa_format_info_free);

    if (port->pal_devicepp_config)
        pa_xfree(port->pal_devicepp_config);

    pa_xfree(port);
}

static int pa_wait_for_snd_card_to_online()
{
    int ret = 0;
    uint32_t retries = MAX_RETRY;
    int fd = -1;
    char buf[2];
    snd_card_status_t card_status = SND_CARD_STATUS_OFFLINE;

    /* wait here till snd card is registered                               */
    /* maximum wait period = (MAX_RETRY * RETRY_INTERVAL_US) micro-seconds */
    do {
        if ((fd = open(SNDCARD_PATH, O_RDWR)) >= 0) {
            memset(buf , 0 ,sizeof(buf));
            lseek(fd,0L,SEEK_SET);
            read(fd, buf, 1);
            close(fd);
            fd = -1;

            buf[sizeof(buf) - 1] = '\0';
            card_status = SND_CARD_STATUS_OFFLINE;
            sscanf(buf , "%d", &card_status);

            if (card_status == SND_CARD_STATUS_ONLINE) {
                pa_log_info("snd sysfs node open successful");
                break;
            }
        }
        retries--;
        sleep(RETRY_INTERVAL);
    } while ( retries > 0);

    if (0 == retries) {
        pa_log_error("Failed to open snd sysfs node, exiting ... ");
        ret = -1;
    }

    return ret;
}

static char *pa_pal_config_get_conf_file_name() {
    const char *cards = "/proc/asound/cards";

    char **items = NULL;
    char *item = NULL;
    char card_string[MAX_BUF_SIZE];
    char *conf_file_name = NULL;
    FILE *pf;
    uint32_t i = 0;

#ifdef PAL_CARD_STATUS_SUPPORTED
    if (0 > pa_wait_for_snd_card_to_online()) {
        pa_log_error("Not found any SND card online\n");
        goto exit;
    }
#endif

    if (!(pf = pa_fopen_cloexec(cards, "rb"))) {
        pa_log_error("Open %s failed\n", cards);
        goto exit;
    }

    while (fgets(card_string, MAX_BUF_SIZE - 1, pf) != NULL) {
        pa_strip_nl(card_string);

        items = pa_split_spaces_strv(card_string);
        if (!items) {
            pa_log_error("%s: invalid sound card name %s", __func__, card_string);
            goto exit;
        }

        i = 0;
        while ((item = items[i++])) {
            if (strstr(item, PAL_CARD_SND_SUFFIX)) {
                conf_file_name = pa_xstrdup(item);
                break;
            }
        }

        if (conf_file_name)
            break;
    }

    if (!item) {
        pa_log_error("%s: can't open %s file to get list of sound cards", __func__, cards);
        goto exit;
    }

    pa_log_info("%s: confile file name is  %s", __func__, conf_file_name);

exit:
    if (items)
        pa_xstrfreev(items);
    if (pf)
        fclose(pf);

    return conf_file_name;
}

static char* pa_pal_config_parser_get_conf_file_name(char *dir, char *conf_name) {
    char *conf_path = NULL;
    char *conf_file_name = NULL;

    if (!dir)
        dir = (char *)PAL_CARD_DEFAULT_CONF_PATH;

    conf_file_name = pa_pal_config_get_conf_file_name();
    if (conf_file_name) {
        /* add .conf suffix conf_file_name */
        conf_name = pa_sprintf_malloc("%s%s", conf_file_name, ".conf");
        conf_path = pa_maybe_prefix_path(conf_name, dir);

        pa_xfree(conf_file_name);
        pa_xfree(conf_name);
    }

    if (access(conf_path, F_OK) < 0) {
        /* use default conf name */
        conf_name = (char *)PAL_CARD_DEFAULT_CONF_NAME;
        pa_log_debug("%s:: No config file name %s, Using default conf file", __func__, conf_path);
        conf_path = pa_maybe_prefix_path(conf_name, dir);
    }

    pa_log_debug("%s:: config file name  %s", __func__, conf_path);

    return conf_path;
}

static int pa_pal_config_parse_port_sys_path(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_port_config *port = NULL;
    int ret = -1;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    if ((port = pa_pal_config_get_port(config_data->ports, state->section))) {
        if (pa_streq(state->lvalue, "state-node-path")) {
            port->state_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding state node path %s to %s", __func__, port->state_node_path, port->name);
        } else if (pa_streq(state->lvalue, "sample-format-node-path")) {
            port->sample_format_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding sample format node path %s to %s", __func__, port->sample_format_node_path, port->name);
        } else if (pa_streq(state->lvalue, "sample-rate-node-path")) {
            port->sample_rate_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding sample rate node path %s to %s", __func__, port->sample_rate_node_path, port->name);
        } else if (pa_streq(state->lvalue, "sample-layout-node-path")) {
            port->sample_layout_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding sample layout node path %s to %s", __func__, port->sample_layout_node_path, port->name);
        } else if (pa_streq(state->lvalue, "sample-channel-node-path")) {
            port->sample_channel_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding sample channel node path %s to %s", __func__, port->sample_channel_node_path, port->name);
        } else if (pa_streq(state->lvalue, "sample-ch-alloc-node-path")) {
            port->sample_channel_alloc_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding sample channel alloc node path %s to %s", __func__, port->sample_channel_alloc_node_path, port->name);
        } else if (pa_streq(state->lvalue, "linkon0-node-path")) {
            port->linkon0_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding linkon0 node path %s to %s", __func__, port->linkon0_node_path, port->name);
        } else if (pa_streq(state->lvalue, "poweron-node-path")) {
            port->poweron_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding poweron node path %s to %s", __func__, port->poweron_node_path, port->name);
        } else if (pa_streq(state->lvalue, "audio-path-node-path")) {
            port->audio_path_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding audio path node path %s to %s", __func__, port->audio_path_node_path, port->name);
        } else if (pa_streq(state->lvalue, "arc-enable-node-path")) {
            port->arc_enable_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding arc enable node path %s to %s", __func__, port->arc_enable_node_path, port->name);
        } else if (pa_streq(state->lvalue, "earc-enable-node-path")) {
            port->earc_enable_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding earc enable node path %s to %s", __func__, port->earc_enable_node_path, port->name);
        } else if (pa_streq(state->lvalue, "arc-state-node-path")) {
            port->arc_state_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding arc state node path %s to %s", __func__, port->arc_state_node_path, port->name);
        } else if (pa_streq(state->lvalue, "arc-sample-format-node-path")) {
            port->arc_sample_format_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding arc sample format node path %s to %s", __func__, port->arc_sample_format_node_path, port->name);
        } else if (pa_streq(state->lvalue, "arc-sample-rate-node-path")) {
            port->arc_sample_rate_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding arc sample rate node path %s to %s", __func__, port->arc_sample_rate_node_path, port->name);
        } else if (pa_streq(state->lvalue, "audio-preemph-node-path")) {
            port->audio_preemph_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding audio preemph node path %s to %s", __func__, port->audio_preemph_node_path, port->name);
        } else if (pa_streq(state->lvalue, "arc-audio-preemph-node-path")) {
            port->arc_audio_preemph_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding arc audio preemph node path %s to %s", __func__, port->arc_audio_preemph_node_path, port->name);
        } else if (pa_streq(state->lvalue, "dsd-rate-node-path")) {
            port->dsd_rate_node_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding DSD rate node path %s to %s", __func__, port->dsd_rate_node_path, port->name);
        } else if (pa_streq(state->lvalue, "hdmi-tx-state")) {
            port->hdmi_tx_state_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding hdmi-tx-state node path %s to %s", __func__, port->hdmi_tx_state_path, port->name);
        } else if (pa_streq(state->lvalue, "channel-status-node-path")) {
            port->channel_status_path = pa_xstrdup(state->rvalue);
            pa_log_debug("%s: adding channel-status-node-path node path %s to %s", __func__, port->channel_status_path, port->name);
        } else {
            pa_log_error ("%s: invalid property %s", __func__, state->lvalue);
            goto exit;
        }
    } else {
        pa_log_error("%s: invalid section name %s", __func__, state->section);
        goto exit;
    }

    ret = 0;

exit:
    return ret;
}

static int pa_pal_config_parse_port_format_detection(pa_config_parser_state *state) {
    pa_pal_config_data* config_data = state->userdata;
    pa_pal_card_port_config *port;
    int ret = 0;
    int k;

    pa_assert(config_data);
    pa_assert(state);
    pa_assert(state->rvalue);

    port = pa_pal_config_get_port(config_data->ports, state->section);
    if (!port) {
        ret = -1;
        goto exit;
    }

    if ((k = pa_parse_boolean(state->rvalue)) < 0) {
        pa_log_error("%s: invalid port format detection type %s(it should be yes or no)", __func__, state->rvalue);
        ret = -1;
        goto exit;
    }

    port->format_detection = k;

exit:
    return ret;
}

/* function to parser conf file to get card related info */
pa_pal_config_data* pa_pal_config_parse_new(char *dir, char *conf_file_name) {
    pa_pal_config_data *config_data;

    int ret = 0;
    char *conf_full_path = NULL;

    pa_config_item items[] = {
        /* [Global] */
        { "default-profile",             pa_config_parse_string,                                   NULL, "Global" },

        /* [Port... ] */
        { "direction",                   pa_pal_config_parse_port_direction,                      NULL, NULL },
        { "device",                      pa_pal_config_parse_port_device,                         NULL, NULL },
        { "hdmi-tx-state",               pa_pal_config_parse_port_sys_path,                       NULL, NULL },
        { "format-detection",            pa_pal_config_parse_port_format_detection,               NULL, NULL },

        /* [Profile... ] */
        { "max-sink-channels",           pa_pal_config_parse_profile_max_sink_channels,           NULL, NULL },
        { "max-source-channels",         pa_pal_config_parse_profile_max_source_channels ,        NULL, NULL },

        { "use-hw-volume",               pa_pal_config_parse_use_hw_volume,                       NULL, NULL },

        /* common between sink and source*/
        { "type",                        pa_pal_config_parse_type,                                NULL, NULL },
        { "avoid-processing",            pa_pal_config_parse_avoid_processing,                    NULL, NULL },
        { "alternate-sample-rate",       pa_pal_config_parse_alternative_sample_rate,             NULL, NULL },

        /* common between profile, sink and source */
        { "port-names",                  pa_pal_config_parse_port_names,                          NULL, NULL },


        /* common between port and profile section */
        { "priority",                    pa_pal_config_parse_priority,                            NULL, NULL },

        /* common between port and profile, sink source and port section */
        { "description",                 pa_pal_config_parse_description,                         NULL, NULL },

        /* common between port, sink and source */
        { "presence",                    pa_pal_config_parse_presence,                            NULL, NULL },
        { "default-encoding",            pa_pal_config_parse_default_encoding,                    NULL, NULL },
        { "default-sample-rate",         pa_pal_config_parse_default_sample_rate,                 NULL, NULL },
        { "default-sample-format",       pa_pal_config_parse_default_sample_format,               NULL, NULL },
        { "default-channel-map",         pa_pal_config_parse_default_channel_map,                 NULL, NULL },
        { "default-buffer-size",         pa_pal_config_parse_default_buffer_size,                 NULL, NULL },
        { "default-buffer-count",        pa_pal_config_parse_default_buffer_count,                NULL, NULL },
        { "encodings",                   pa_pal_config_parse_encodings,                           NULL, NULL },
        { "sample-rates",                pa_pal_config_parse_sample_rates,                        NULL, NULL },
        { "sample-formats",              pa_pal_config_parse_sample_formats,                      NULL, NULL },
        { "channel-maps",                pa_pal_config_parse_channel_maps,                        NULL, NULL },
        { "pal-devicepp-config",         pa_pal_config_parse_pal_devicepp_config,                 NULL, NULL },

	/* [Loopback...] */
        { "in-port-names",               pa_pal_config_parse_port_names,                          NULL, NULL },
        { "out-port-names",              pa_pal_config_parse_port_names,                          NULL, NULL },

        {  NULL, NULL, NULL, NULL }
    };

    pa_log_info("%s", __func__);

    config_data = pa_xnew0(pa_pal_config_data, 1);

    items[0].data = &config_data->default_profile;

    config_data->ports = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) pa_pal_config_free_port);

    config_data->profiles = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) pa_pal_config_free_profile);

    config_data->sinks = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) pa_pal_config_free_sink);

    config_data->sources = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) pa_pal_config_free_source);

    config_data->loopbacks = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func, NULL, (pa_free_cb_t) pa_pal_config_free_loopback);

    conf_full_path = pa_pal_config_parser_get_conf_file_name(dir, conf_file_name);
    if (!conf_full_path) {
        pa_log_error("%s:: Could not find valid conf, exiting ", __func__);
        ret = -1;
        goto fail;
    }

    ret = pa_config_parse(conf_full_path, NULL, items, NULL, false, config_data);
    if (ret < 0) {
        pa_log_error("%s:: Parsing of conf %s failed, error %d exiting ", __func__, conf_full_path, ret);
        goto fail;
    } else {
        goto exit;
    }

fail:
    pa_pal_config_parse_free(config_data);
    config_data = NULL;

exit:
    if (conf_full_path)
        pa_xfree(conf_full_path);

    return config_data;
}

void pa_pal_config_parse_free(pa_pal_config_data *config_data) {
    pa_log_info("%s", __func__);

    pa_assert(config_data);

    if (config_data->sinks) {
        pa_hashmap_free(config_data->sinks);
        config_data->sinks = NULL;
    }

    if (config_data->sources) {
        pa_hashmap_free(config_data->sources);
        config_data->sources = NULL;
    }

    if (config_data->loopbacks) {
        pa_hashmap_free(config_data->loopbacks);
        config_data->loopbacks = NULL;
    }

    if (config_data->default_profile) {
        pa_xfree(config_data->default_profile);
        config_data->default_profile = NULL;
    }

    if (config_data->profiles) {
        pa_hashmap_free(config_data->profiles);
        config_data->profiles = NULL;
    }

    if (config_data->ports) {
        pa_hashmap_free(config_data->ports);
        config_data->ports = NULL;
    }

    pa_xfree(config_data);
    config_data = NULL;
}
