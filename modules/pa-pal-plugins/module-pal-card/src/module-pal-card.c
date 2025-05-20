/*
 * Copyright (c) 2019, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause-Clear
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/device-port.h>
#include <pulsecore/core-util.h>
#include <pulsecore/core-format.h>
#include <pulse/sample.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/protocol-dbus.h>

#include <string.h>

#include <PalApi.h>
#include <PalDefs.h>
#include <agm/agm_api.h>

#include "pal-source.h"
#include "pal-sink.h"
#include "pal-card.h"
#include "pal-config-parser.h"
#include "pal-loopback.h"

#include "pal-jack.h"
#include "pal-jack-common.h"
#include "pal-jack-format.h"

#include "pal-utils.h"

#ifdef ENABLE_PAL_SERVICE
void load_pal_service();
#endif

#define CONC(A,B) (A B)
#define PAL_MODULE_ID_PREFIX "audio."
#define PAL_MODULE_PRIMARY "primary"

#ifndef PAL_MODULE_ID_PRIMARY
#define PAL_MODULE_ID_PRIMARY CONC(PAL_MODULE_ID_PREFIX, PAL_MODULE_PRIMARY)
#endif

#define PAL_CARD_NAME_PREFIX "pal."
#define DEFAULT_PROFILE "default"
#define DEFAULT_SCO_SAMPLE_RATE 16000
#define SCO_SAMPLE_RATE_8K 8000

PA_MODULE_AUTHOR("QTI");
PA_MODULE_DESCRIPTION("pal card module");
PA_MODULE_VERSION(PA_PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

/* We don't have any module arguments */
PA_MODULE_USAGE(
        "module=audio.primary"
        "conf_dir_name= direct from pal conf is present"
        "conf_file_name= pal conf name is present in conf_dir_name"
);

static const char* const valid_modargs[] = {
    "module",
    "conf_dir_name",
    "conf_file_name",
    NULL
};

struct userdata {
    pa_core *core;
    pa_card *card;
    const char *driver;
    char *module_name;
    pa_module *module;
    pa_hashmap *profiles;
    pa_modargs *modargs;

    pa_sample_spec ss;
    pa_channel_map map;

    pa_hashmap *sinks;
    pa_hashmap *sources;
    pa_hashmap *jacks;

    pa_pal_config_data *config_data;
    char *conf_dir_name;
    char *conf_file_name;
};


typedef struct {
    pa_pal_jack_handle_t *handle;
    pa_pal_jack_type_t jack_type;
    pa_pal_jack_out_config jack_curr_config;
    pa_pal_jack_out_config jack_prev_config;
} pa_pal_card_jack_info;
/* internal functions */

static int pa_pal_card_add_source(pa_module *module, pa_card *card, const char *driver, char *module_name, pa_pal_source_config *source,
                                  pa_pal_source_handle_t **source_handle);
static int pa_pal_card_add_sink(pa_module *module, pa_card *card, const char *driver, char *module_name, pa_pal_sink_config *sink,
                                pa_pal_sink_handle_t **sink_handle);
void pa__done(pa_module *m);
int pa_pal_module_extn_init(pa_core *core, pa_card *card);
void pa_pal_module_extn_deinit(void);

static void pa_pal_card_profiles_free(struct userdata *u, pa_hashmap *profiles) {
    pa_card_profile *p;
    void *state;

    PA_HASHMAP_FOREACH(p, profiles, state) {
        pa_hashmap_remove_and_free(profiles, p->name);
    }
}

static void pa_pal_card_create_ports(struct userdata *u, pa_hashmap *ports, pa_hashmap *profiles) {
    pa_device_port *port;
    pa_pal_card_port_config *config_port;
    pa_device_port_new_data port_data;
    pa_pal_card_port_device_data *port_device_data = NULL;

    void *state;

    pa_log_debug("%s:\n", __func__);
    pa_assert(u);
    pa_assert(u->config_data);
    pa_assert(ports);
    pa_assert(profiles);

    PA_HASHMAP_FOREACH(config_port, u->config_data->ports, state) {
        pa_device_port_new_data_init(&port_data);

        pa_device_port_new_data_set_name(&port_data, config_port->name);

        pa_device_port_new_data_set_description(&port_data, config_port->description);
        pa_device_port_new_data_set_direction(&port_data, config_port->direction);
        pa_device_port_new_data_set_available(&port_data, config_port->available);

        port = pa_device_port_new(u->core, &port_data, sizeof(pa_pal_card_port_device_data));

        port_device_data = PA_DEVICE_PORT_DATA(port);

        port_device_data->device = config_port->device;
        port->priority = config_port->priority;
        port_device_data->default_map = config_port->default_map;
        port_device_data->default_spec.channels = config_port->default_map.channels;
        port_device_data->default_spec.rate = config_port->default_spec.rate;

        if (config_port->pal_devicepp_config)
            port_device_data->pal_devicepp_config = pa_xstrdup(config_port->pal_devicepp_config);

        /* Sanity check that we don't have duplicates */
        pa_assert_se(pa_hashmap_put(ports, port->name, port) >= 0);

        pa_device_port_new_data_done(&port_data);

    }
}

static void pa_pal_card_create_profiles_and_add_ports(struct userdata *u, pa_hashmap *profiles, pa_hashmap *ports) {
    pa_card_profile *profile = NULL;
    pa_pal_card_port_config *config_port;
    pa_device_port *card_port;
    pa_pal_card_profile_config *config_profile;

    void *state;
    void *state1;

    pa_log_debug("%s:\n", __func__);

    PA_HASHMAP_FOREACH(config_profile, u->config_data->profiles, state) {
        profile = pa_card_profile_new(config_profile->name, config_profile->description, 0);

        pa_log_debug("%s:profile %s created",__func__, profile->name);

        profile->priority = config_profile->priority;
        profile->n_sinks = config_profile->n_sinks;
        profile->n_sources = config_profile->n_sources;
        profile->available =  PA_AVAILABLE_YES;

        pa_hashmap_put(profiles, profile->name, profile);

        /* Add profile to port */
        /* get port list from config profile structure */
        PA_HASHMAP_FOREACH(config_port, config_profile->ports, state1) {
            card_port = pa_hashmap_get(ports, config_port->name);
            if (!card_port) {
                pa_log_error("%s, skipping port %s as doesn't belong to card", __func__, config_port->name);
                continue;
            }

            pa_log_debug("%s: adding profile %s for port %s", __func__, profile->name, config_port->name);
            pa_hashmap_put(card_port->profiles, profile->name, profile);
        }
    }
}


static int pa_pal_card_set_profile(pa_card *c, pa_card_profile *new_profile) {
    pa_log_error("profile change not supported yet");
    return 0;
}

static void pa_pal_card_free(struct userdata *u) {
    pa_assert(u);

    if (u->card)
        pa_card_free(u->card);
}

/* create port and profile and adds it card */
static int pa_pal_card_create(struct userdata *u) {
    pa_card_new_data data;
    pa_card_profile *profile;

    pa_assert(u);
    pa_log_debug("%s:\n", __func__);
    pa_card_new_data_init(&data);
    data.driver = __FILE__;
    data.module = u->module;
    data.name =  pa_sprintf_malloc("%s%s", PAL_CARD_NAME_PREFIX, u->module_name);
    data.namereg_fail = true;

    pa_proplist_setf(data.proplist, PA_PROP_DEVICE_DESCRIPTION, "Card for the %s HAL module", u->module_name);

    pa_pal_card_create_ports(u, data.ports, data.profiles);
    pa_pal_card_create_profiles_and_add_ports(u, data.profiles, data.ports);

    u->card = pa_card_new(u->core, &data);
    pa_card_new_data_done(&data);

    if (!u->card) {
        pa_log_error("Failed to allocate card.");
        pa_pal_card_profiles_free(u, data.profiles);
        return -1;
    }

    u->card->userdata = u;
    u->card->set_profile = pa_pal_card_set_profile;

    profile = pa_hashmap_get(u->card->profiles, DEFAULT_PROFILE);
    if (!profile) {
        pa_log("profile not found");
        pa_pal_card_free(u);
        return -1;
    }

    u->card->active_profile = profile;

    pa_card_set_profile(u->card, profile, false);

    pa_card_put(u->card);

    return 0;
}

static int pa_pal_card_add_source(pa_module *module, pa_card *card, const char *driver, char *module_name, pa_pal_source_config *source,
                                  pa_pal_source_handle_t **source_handle) {
    uint32_t rc = 0;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_name);
    pa_assert(source);

    rc = pa_pal_source_create(module, card, driver, module_name, source, source_handle);
    if (rc) {
        pa_log_error("%s: source %s create failed %d ", __func__, source->name, rc);
    }

    return rc;
}

static int pa_pal_card_create_sources(struct userdata *u, const char *profile_name, pa_pal_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    pa_pal_source_config *source;

    void *state;

    pa_pal_card_source_info *source_info;

    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if (!(pa_hashmap_get(source->profiles, profile_name)) || source->usecase_type != usecase_type)
            continue;

        source_info = pa_xnew0(pa_pal_card_source_info, 1);
        rc = pa_pal_card_add_source(u->module, u->card, u->driver, u->module_name, source, &(source_info->handle));
        if (rc) {
            pa_log_error("%s: source %s create failed for profile %s, error %d ", __func__, source->name, profile_name, rc);
            source_info->handle = NULL;
            continue;
        }

        pa_hashmap_put(u->sources, source->name, source_info);
    }

    return rc;
}

static void pa_pal_card_free_sources(struct userdata *u, const char *profile_name) {
    pa_pal_source_config *source;
    void *state;
    pa_pal_card_source_info *source_info;

    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if (!(pa_hashmap_get(source->profiles, profile_name)))
            continue;

        source_info = pa_hashmap_get(u->sources, source->name);

        if (source_info) {
            pa_pal_source_close(source_info->handle);

            pa_hashmap_remove(u->sources, source->name);
            pa_xfree(source_info);
        }
    }
}

static int pa_pal_card_add_sink(pa_module *module, pa_card *card, const char *driver, char *module_name,
                                 pa_pal_sink_config *sink, pa_pal_sink_handle_t **sink_handle) {
    uint32_t rc = 0;

    pa_assert(module);
    pa_assert(card);
    pa_assert(driver);
    pa_assert(module);
    pa_assert(module_name);
    pa_assert(sink);

    rc = pa_pal_sink_create(module, card, driver, module_name, sink, sink_handle);
    if (rc) {
        pa_log_error("%s: sink %s create failed %d ", __func__, sink->name, rc);
    }

    return rc;
}

static int pa_pal_card_create_sinks(struct userdata *u, const char *profile_name, pa_pal_card_usecase_type_t usecase_type) {
    uint32_t rc = 0;
    pa_pal_sink_config *sink;

    void *state;

    pa_pal_card_sink_info *sink_info;

    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if (!(pa_hashmap_get(sink->profiles, profile_name)) || sink->usecase_type != usecase_type)
            continue;

        sink_info = pa_xnew0(pa_pal_card_sink_info, 1);
        rc = pa_pal_card_add_sink(u->module, u->card, u->driver, u->module_name, sink, &(sink_info->handle));
        if (rc) {
            pa_log_error("%s: sink %s create failed for profile %s, error %d ", __func__, sink->name, profile_name, rc);
            sink_info->handle = NULL;
            continue;
        }

        pa_hashmap_put(u->sinks, sink->name, sink_info);

    }

    return rc;
}

static void pa_pal_card_free_sinks(struct userdata *u, const char *profile_name) {
    pa_pal_sink_config *sink;
    void *state;
    pa_pal_card_sink_info *sink_info;

    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if (!(pa_hashmap_get(sink->profiles, profile_name)))
            continue;

        sink_info = pa_hashmap_get(u->sinks, sink->name);

        if (sink_info) {
            pa_pal_sink_close(sink_info->handle);

            pa_hashmap_remove(u->sinks, sink->name);
            pa_xfree(sink_info);
        }
    }
}

static pa_pal_card_source_info *pa_pal_card_is_dynamic_source_present_for_port(const char *port_name,
                                                                            struct userdata *u) {
    pa_pal_card_source_info *source_info = NULL;
    pa_pal_source_config *source;
    void *state;

    pa_assert(port_name);
    pa_assert(u);

    /* check if any source is already created on same port */
    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if ((source->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(source->ports, port_name))) {
            source_info = pa_hashmap_get(u->sources, source->name);
            if (source_info) {
                pa_log_info("%s: Found an existing dynamic source %s for port %s", __func__, source->name, port_name);
                break;
            }
        }
    }

    return source_info;
}

static pa_pal_card_sink_info *pa_pal_card_is_dynamic_sink_present_for_port(const char *port_name, struct userdata *u) {
    pa_pal_card_sink_info *sink_info = NULL;
    pa_pal_sink_config *sink;
    void *state;

    pa_assert(port_name);
    pa_assert(u);

    /* check if any sink is already created on same port */
    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if ((sink->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(sink->ports, port_name))) {
            sink_info = pa_hashmap_get(u->sinks, sink->name);
            if (sink_info) {
                pa_log_info("%s: Found an existing dynamic sink %s for port %s", __func__, sink->name, port_name);
                break;
            }
        }
    }

    return sink_info;
}

static void pa_pal_card_remove_dynamic_source(pa_device_port *port, struct userdata *u) {
    pa_pal_source_config *source = NULL;
    pa_pal_card_source_info *source_info = NULL;
    void *state;

    pa_assert(port);

    pa_log_debug("%s:", __func__);

    /*find a dynamic source which supports give a port, currently assumption is that one dynamic source is supported for a port */
    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if ((source->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(source->ports, port->name))) {
            /* check if this source supports required encoding */
            pa_log_info("%s: Found a dynamic source %s for port %s", __func__, source->name, port->name);
            source_info = pa_hashmap_get(u->sources, source->name);
            if (!source_info)
                continue;

            break;
        }
    }

    if (!source_info) {
        pa_log_error("%s: no dynamic usecase present, skip removal of source ", __func__);
        goto exit;
    }

    pa_pal_source_close(source_info->handle);

    pa_hashmap_remove(u->sources, source->name);
    pa_xfree(source_info);

exit:
    return;
}

static void pa_pal_card_add_dynamic_source(pa_device_port *port, pa_pal_jack_out_config *config, struct userdata *u) {
    int rc;

    pa_pal_card_source_info *source_info = NULL;

    pa_pal_source_config *source = NULL;
    pa_pal_source_config new_source;

    pa_idxset *requested_formats = NULL;
    pa_format_info *requested_format = NULL;

    pa_format_info *current_format = NULL;
    pa_format_info *config_format = NULL;

    pa_idxset *current_formats = NULL;

    pa_sample_spec ss;
    pa_channel_map map;
    pa_encoding_t encoding;

    char fmt[PA_FORMAT_INFO_SNPRINT_MAX];
    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state = NULL;
    uint32_t i;

    pa_assert(port);
    pa_assert(config);
    pa_assert(u);

    pa_log_debug("%s:", __func__);

    requested_format = pa_format_info_new();
    requested_format->encoding = config->encoding;

#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
    if (config->encoding != PA_ENCODING_PCM) {
        pa_format_info_set_rate(requested_format, config->ss.rate);

        if (config->encoding == PA_ENCODING_DSD) {
            pa_format_info_set_channels(requested_format, config->ss.channels);
        }
    }
#endif

    pa_log_info("%s: requested source with ss %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &config->ss));

    /* check if any dynamic source is already created on same port */
    source_info = pa_pal_card_is_dynamic_source_present_for_port(port->name, u);

    /* check if reconfigure is needed if yes then close free existing source and recreate new source */
    if (source_info) {
        /* For pcm get the media config as pcm source doesn't only add encoding in format*/
#ifndef PAL_DISABLE_COMPRESS_AUDIO_SUPPORT
        if ((config->encoding == PA_ENCODING_PCM) || (config->encoding == PA_ENCODING_DSD)) {
#else
        if (config->encoding == PA_ENCODING_PCM) {
#endif
            rc = pa_pal_source_get_media_config(source_info->handle, &ss, &map, &encoding);
            if (rc) {
                pa_log_error("%s: pa_pal_source_get_media_config failed, error %d", __func__, rc);
                goto exit;
            }
        } else {
            current_formats = pa_pal_source_get_config(source_info->handle);
            if (!current_formats || (pa_idxset_size(current_formats) != 1)) {  /* dynamic source should have single format */
                pa_log_error("%s: pa_pal_source_get_config failed", __func__);
                goto exit;
            }

            current_format = pa_idxset_first(current_formats, NULL);
            encoding = current_format->encoding;

            pa_log_info("%s: existing source format = %s", __func__, pa_format_info_snprint(fmt, sizeof(fmt), current_format));

            pa_format_info_to_sample_spec(current_format, &ss, &map);

            pa_idxset_free(current_formats, (pa_free_cb_t) pa_format_info_free);
        }

        pa_log_info("%s: closing current source and createing new one", __func__);
        pa_pal_card_remove_dynamic_source(port, u);
    }

    /* find a dynamic source which supports requested port and encoding */
    PA_HASHMAP_FOREACH(source, u->config_data->sources, state) {
        if ((source->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(source->ports, port->name))) {
            PA_IDXSET_FOREACH(config_format, source->formats, i) {
                if (pa_format_info_is_compatible(config_format, requested_format)) {
                    break;
                }
            }
        }
        /* check if this source supports requested format */
        if (config_format) {
            pa_log_info("%s: found a dynamic source %s for port %s with requested capablity", __func__, source->name, port->name);
            break;
        }
    }

    if (!config_format) {
        pa_log_error("%s: dynamic source for requested format is not supported for port %s", __func__, port->name);
        goto exit;
    }

    requested_formats = pa_idxset_new(NULL, NULL);
    pa_idxset_put(requested_formats, requested_format, NULL);

    new_source = *source;
    new_source.default_spec = config->ss;
    new_source.default_map = pa_pal_map_remove_invalid_channels(&(config->map));
    new_source.formats = requested_formats;
    new_source.default_encoding = config->encoding;

    source_info = pa_xnew0(pa_pal_card_source_info, 1);
    rc = pa_pal_card_add_source(u->module, u->card, u->driver, u->module_name, &new_source, &(source_info->handle));
    if (rc) {
        pa_log_error("%s: source %s create failed for port %s, error %d ", __func__, source->name, port->name, rc);
        source_info->handle = NULL;
    } else {
        pa_hashmap_put(u->sources, new_source.name, source_info);
    }

    pa_idxset_free(requested_formats, (pa_free_cb_t) pa_format_info_free);
exit:
   return;
}

static void pa_pal_card_set_sink_param(pa_device_port *port, struct userdata *u, const char *jack_param) {
    int ret = 0;
    jack_prm_kvpair_t kvpair;
    bool connection_state = false;

    pa_assert(port);
    pa_assert(jack_param);

    pa_log_debug("%s:", __func__);

    ret = pa_pal_external_jack_parse_kvpair(jack_param, &kvpair);
    if (ret) {
        pa_log_error("Invalid jack param !!");
        return;
    }

    switch(kvpair.key) {
        case JACK_PARAM_KEY_DEVICE_CONNECTION:
            connection_state = (!strcmp(kvpair.value, "true")) ? true : false;
            ret = pa_pal_set_device_connection_state(pa_pal_util_port_name_to_enum(port->name), connection_state);
            if(ret)
                pa_log_error("Set sink device connection params for connection=%d failed ret =%d", connection_state, ret);
            break;
        case JACK_PARAM_KEY_A2DP_SUSPEND:
            ret = pa_pal_sink_set_a2dp_suspend(kvpair.value);
            if (ret)
                pa_log_error("Set sink param for a2dp suspend=%s failed", kvpair.value);
            break;
        default:
            break;
    }
}

static int pa_pal_set_sco_params(uint32_t sample_rate) {
    int ret = E_SUCCESS;

    pal_param_btsco_t param_btsco;
    pal_param_id_type_t param_id;

    memset(&param_btsco, 0, sizeof(param_btsco));
    param_id = PAL_PARAM_ID_BT_SCO;
    param_btsco.is_bt_hfp = false; //false for HFP-AG case
    param_btsco.bt_sco_on = true;

    ret =  pal_set_param(param_id, (void*)&param_btsco,
            sizeof(pal_param_btsco_t));
    if (ret != 0) {
        pa_log_error("Set param_id=%d failed", param_id);
    }

    param_id = PAL_PARAM_ID_BT_SCO_WB;
    if (sample_rate == DEFAULT_SCO_SAMPLE_RATE) {
        param_btsco.bt_wb_speech_enabled = true;
    }
    else
        param_btsco.bt_wb_speech_enabled = false;

    ret =  pal_set_param(param_id, (void*)&param_btsco,
            sizeof(pal_param_btsco_t));
    if (ret != 0) {
        pa_log_error("Set param_id=%d failed", param_id);
    }

    return ret;
}

static void pa_pal_card_set_source_param(pa_device_port *port, struct userdata *u, const char *jack_param) {
    int ret = 0;
    jack_prm_kvpair_t kvpair;
    bool connection_state = false;

    pa_assert(port);
    pa_assert(jack_param);

    pa_log_debug("%s:", __func__);

    ret = pa_pal_external_jack_parse_kvpair(jack_param, &kvpair);
    if (ret) {
        pa_log_error("Invalid jack param !!");
        return;
    }

    /* check if any dynamic source is already created on same port */
    switch(kvpair.key) {
        case JACK_PARAM_KEY_DEVICE_CONNECTION:
            connection_state = (!strcmp(kvpair.value, "true")) ? true : false;
            ret = pa_pal_set_device_connection_state(pa_pal_util_port_name_to_enum(port->name), connection_state);
            if(ret)
                pa_log_error("Set source device connection params failed ret=%d", ret);
            ret = pa_pal_set_sco_params(DEFAULT_SCO_SAMPLE_RATE);
            break;
        case JACK_PARAM_KEY_DEVICE_SAMPLERATE:
            if (!strcmp(port->name, "btsco-in")) {
                /* setting common params for SCO  mode */
                if (!strcmp(kvpair.value, "16000"))
                    ret = pa_pal_set_sco_params(DEFAULT_SCO_SAMPLE_RATE);
                else if (!strcmp(kvpair.value, "8000"))
                    ret = pa_pal_set_sco_params(SCO_SAMPLE_RATE_8K);
                else
                    pa_log_error("sample rate %s not supported", kvpair.value);
                if(ret)
                    pa_log_error("Set common sco params failed. ret=%d", ret);
            }
            break;
        default:
            break;
    }
}

static void pa_pal_card_remove_dynamic_sink(pa_device_port *port, struct userdata *u) {
    pa_pal_sink_config *sink = NULL;
    pa_pal_card_sink_info *sink_info = NULL;
    void *state;

    pa_assert(port);

    pa_log_debug("%s:", __func__);

    /*find a dynamic sink which supports give a port, currently assumption is that one dynamic sink is supported for a port */
    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if ((sink->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(sink->ports, port->name))) {
            /* check if this sink supports required encoding */
            pa_log_info("%s: Found a dynamic sink %s for port %s", __func__, sink->name, port->name);
            sink_info = pa_hashmap_get(u->sinks, sink->name);
            if (!sink_info)
                continue;

            break;
        }
    }

    if (!sink_info) {
        pa_log_error("%s: no dynamic usecase present, skip removal of sink ", __func__);
        goto exit;
    }

    pa_pal_sink_close(sink_info->handle);

    pa_hashmap_remove(u->sinks, sink->name);
    pa_xfree(sink_info);

exit:
    return;
}

static void pa_pal_card_add_dynamic_sink(pa_device_port *port, pa_pal_jack_out_config *config, struct userdata *u) {
    int rc;
    bool reconfigure = false;

    pa_pal_card_sink_info *sink_info = NULL;

    pa_pal_sink_config *sink = NULL;
    pa_pal_sink_config new_sink;

    pa_idxset *requested_formats = NULL;
    pa_format_info *requested_format = NULL;

    pa_format_info *current_format = NULL;
    pa_format_info *config_format = NULL;

    pa_idxset *current_formats = NULL;

    pa_sample_spec ss;
    pa_channel_map map;
    pa_encoding_t encoding;

    char fmt[PA_FORMAT_INFO_SNPRINT_MAX];
    char ss_buf[PA_SAMPLE_SPEC_SNPRINT_MAX];

    void *state = NULL;
    uint32_t i;

    pa_assert(port);
    pa_assert(config);
    pa_assert(u);

    pa_log_debug("%s:", __func__);

    requested_format = pa_format_info_new();
    requested_format->encoding = config->encoding;

    if (config->encoding != PA_ENCODING_PCM)
        pa_format_info_set_rate(requested_format, config->ss.rate);

    pa_log_info("%s: requested sink with ss %s", __func__, pa_sample_spec_snprint(ss_buf, sizeof(ss_buf), &config->ss));

    /* check if any dynamic sink is already created on same port */
    sink_info = pa_pal_card_is_dynamic_sink_present_for_port(port->name, u);

    /* check if reconfigure is needed if yes then close free existing sink and recreate new sink */
    if (sink_info) {
        /* For pcm get the media config as pcm sink doesn't only add encoding in format*/
        if (config->encoding == PA_ENCODING_PCM) {
            rc = pa_pal_sink_get_media_config(sink_info->handle, &ss, &map, &encoding);
            if (rc) {
                pa_log_error("%s: pa_pal_sink_get_media_config failed, error %d", __func__, rc);
                goto exit;
            }
        } else {
            current_formats = pa_pal_sink_get_config(sink_info->handle);
            if (!current_formats || (pa_idxset_size(current_formats) != 1)) {  /* dynamic sink should have single format */
                pa_log_error("%s: pa_pal_sink_get_config failed", __func__);
                goto exit;
            }

            current_format = pa_idxset_first(current_formats, NULL);
            encoding = current_format->encoding;

            pa_log_info("%s: existing sink format = %s", __func__, pa_format_info_snprint(fmt, sizeof(fmt), current_format));

            pa_format_info_to_sample_spec(current_format, &ss, &map);

            pa_idxset_free(current_formats, (pa_free_cb_t) pa_format_info_free);

        }

       if (requested_format->encoding != encoding)
            reconfigure = true;
        else if ((requested_format->encoding == PA_ENCODING_PCM) && (!pa_sample_spec_equal(&config->ss, &ss)) && (!pa_channel_map_equal(&config->map, &map)))
            reconfigure = true;

        if (reconfigure) {
            pa_log_info("%s: sink reconfiguraiton needed, closing current sink and createing new one", __func__);
            pa_pal_card_remove_dynamic_sink(port, u);
        } else {
            pa_log_info("%s: sink already exits", __func__);
            goto exit;
        }
    }

    /* find a dynamic sink which supports requested port and encoding */
    PA_HASHMAP_FOREACH(sink, u->config_data->sinks, state) {
        if ((sink->usecase_type == PA_PAL_CARD_USECASE_TYPE_DYNAMIC) && (pa_hashmap_get(sink->ports, port->name))) {
            PA_IDXSET_FOREACH(config_format, sink->formats, i) {
                if (pa_format_info_is_compatible(config_format, requested_format)) {
                    break;
                }
            }
        }
        /* check if this sink supports requested format */
        if (config_format) {
            pa_log_info("%s: found a dynamic sink %s for port %s with requested capablity", __func__, sink->name, port->name);
            break;
        }
    }

    if (!config_format) {
        pa_log_error("%s: dynamic sink for requested format is not supported for port %s", __func__, port->name);
        goto exit;
    }

    requested_formats = pa_idxset_new(NULL, NULL);
    pa_idxset_put(requested_formats, requested_format, NULL);

    new_sink = *sink;
    new_sink.default_spec = config->ss;
    new_sink.default_map = config->map;
    new_sink.formats = requested_formats;
    new_sink.default_encoding = config->encoding;

    sink_info = pa_xnew0(pa_pal_card_sink_info, 1);
    rc = pa_pal_card_add_sink(u->module, u->card, u->driver, u->module_name, &new_sink, &(sink_info->handle));

    if (rc) {
        pa_log_error("%s: sink %s create failed for port %s, error %d ", __func__, sink->name, port->name, rc);
        sink_info->handle = NULL;
    } else {
        pa_hashmap_put(u->sinks, new_sink.name, sink_info);
    }

    pa_idxset_free(requested_formats, (pa_free_cb_t) pa_format_info_free);
exit:
   return;
}

static pa_hook_result_t pa_pal_jack_callback(void *dummy __attribute__((unused)), pa_pal_jack_event_data_t *event_data, void *prv_data) {
    const char *port_name = NULL;
    pa_available_t status = PA_AVAILABLE_UNKNOWN;
    pa_device_port *port;
    struct userdata *u;
    pa_pal_jack_event_t event;
    pa_pal_card_jack_info *jack_info;
    const char *jack_param = NULL;

    pa_assert(event_data);
    pa_assert(prv_data);

    u  = (struct userdata *)prv_data;

    event = event_data->event;
    if ((event != PA_PAL_JACK_AVAILABLE) && (event != PA_PAL_JACK_UNAVAILABLE) && (event != PA_PAL_JACK_CONFIG_UPDATE) &&
        (event != PA_PAL_JACK_NO_VALID_STREAM) && (event != PA_PAL_JACK_SET_PARAM)) {
        pa_log_error("%s: unsupport pal jack event %d",__func__, event);
        return PA_HOOK_CANCEL;
    }

    if (event_data->jack_type == PA_PAL_JACK_TYPE_WIRED_HEADSET_BUTTONS) {
        pa_log_info("PA_PAL_JACK_TYPE_WIRED_HEADSET_BUTTONS not supported currently");
        return PA_HOOK_CANCEL;
    }

    if (event == PA_PAL_JACK_AVAILABLE)
        status = PA_AVAILABLE_YES;
    else if (event == PA_PAL_JACK_UNAVAILABLE)
        status = PA_AVAILABLE_NO;

    port_name = pa_pal_util_get_port_name_from_jack_type(event_data->jack_type);
    if (port_name != NULL) {
        pa_log_info("port %s satus %d event %x", port_name, status, event);
        port = pa_hashmap_get(u->card->ports, port_name);
        if (port) {
            if (event == PA_PAL_JACK_AVAILABLE) {
                pa_device_port_set_available(port, status);
            } else if (event == PA_PAL_JACK_UNAVAILABLE) {
                pa_device_port_set_available(port, status);

                if (port->direction == PA_DIRECTION_INPUT) {
                    pa_pal_card_remove_dynamic_source(port, u);
                } else if (port->direction == PA_DIRECTION_OUTPUT) {
                    pa_pal_card_remove_dynamic_sink(port, u);
                }

            } else if ((event == PA_PAL_JACK_CONFIG_UPDATE) && (port->available == PA_AVAILABLE_YES)) {
                if (port->direction == PA_DIRECTION_INPUT) {
                    jack_info = pa_hashmap_get(u->jacks, port_name);
                    if (jack_info)
                        jack_info->jack_curr_config = *((pa_pal_jack_out_config *)event_data->pa_pal_jack_info);

                    pa_pal_card_add_dynamic_source(port, (pa_pal_jack_out_config *)event_data->pa_pal_jack_info, u);
                } else if (port->direction == PA_DIRECTION_OUTPUT) {
                    jack_info = pa_hashmap_get(u->jacks, port_name);
                    if (jack_info)
                        jack_info->jack_curr_config = *((pa_pal_jack_out_config *)event_data->pa_pal_jack_info);

                    pa_pal_card_add_dynamic_sink(port, (pa_pal_jack_out_config *)event_data->pa_pal_jack_info, u);
                }
            } else if ((event == PA_PAL_JACK_NO_VALID_STREAM) && (port->available == PA_AVAILABLE_YES)) {
                if (port->direction == PA_DIRECTION_INPUT) {
                    pa_pal_card_remove_dynamic_source(port, u);
                } else if (port->direction == PA_DIRECTION_OUTPUT) {
                    pa_pal_card_remove_dynamic_sink(port, u);
                }
            } else if ((event == PA_PAL_JACK_SET_PARAM) &&
                    ((port->available == PA_AVAILABLE_YES) ||
                     (event_data->jack_type == PA_PAL_JACK_TYPE_BTA2DP_OUT) ||
                     (event_data->jack_type == PA_PAL_JACK_TYPE_BTSCO_OUT) ||
                     (event_data->jack_type == PA_PAL_JACK_TYPE_BTSCO_IN))) {
                jack_param = (const char *)event_data->pa_pal_jack_info;

                if (port->direction == PA_DIRECTION_INPUT)
                    pa_pal_card_set_source_param(port, u, jack_param);
                else if (port->direction == PA_DIRECTION_OUTPUT)
                    pa_pal_card_set_sink_param(port, u, jack_param);

            } else {
                pa_log_error("unsupported event %d", event);
            }
        } else {
            pa_log_error("unsupported port %s", port_name);
        }

        if (pa_streq(port_name, "headset")) {
            port = pa_hashmap_get(u->card->ports, "headset-mic");
            if (port)
                pa_device_port_set_available(port, status);
        }
    } else {
        pa_log_error("unsupport jack type %d", event_data->jack_type);
    }

    return PA_HOOK_OK;
}

static void pa_pal_card_enable_jack_detection(struct userdata *u) {
    pa_pal_jack_handle_t *jack_handle = NULL;
    pa_pal_jack_type_t jack_types = PA_PAL_JACK_TYPE_INVALID;
    pa_pal_card_jack_info *jack_info = NULL;
    pa_pal_card_jack_info *secondary_jack_info = NULL;
    pa_pal_card_port_config *config_port = NULL;
    pa_pal_card_port_config *secondary_config_port = NULL;

    pa_device_port *port;
    void *state;
    pa_pal_jack_in_config *jack_in_config = NULL;
    char *port_name = NULL;
    int i = 0;
    bool external_jack = false;

    u->jacks = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    /* register for jack detection for dynamic port, PA_AVAILABLE_NO means its dynamic port */
    PA_HASHMAP_FOREACH(port, u->card->ports, state) {
        external_jack = false;

        config_port = pa_hashmap_get(u->config_data->ports, port->name);
        if (!config_port)
            continue;

        if (config_port->detection) {
            if (pa_streq(config_port->detection, "external"))
                external_jack = true;
        }

        if ((port->available == PA_AVAILABLE_NO) || (config_port->format_detection))
            jack_types = pa_pal_util_get_jack_type_from_port_name(port->name);
        else
            continue;

        /* If external jack, no need to pass any input configs */
        if (config_port->format_detection && !external_jack) {
            jack_in_config = pa_xnew0(pa_pal_jack_in_config, 1);
            pa_pal_util_get_jack_sys_path(config_port, jack_in_config);
        }

        /* Allocate memory for jack */
        jack_info = pa_xnew0(pa_pal_card_jack_info, 1);
        jack_info->jack_type = jack_types;
        pa_hashmap_put(u->jacks, port->name, jack_info);

        jack_handle = pa_pal_jack_register_event_callback(jack_types, pa_pal_jack_callback,
                       u->module, jack_in_config, (void *)u, external_jack);
        if (!jack_handle) {
            pa_log_error("%s: Enable pal jack failed for port %s\n", __func__, port->name);

            /* Free memory associated with jack */
            pa_hashmap_remove(u->jacks, port->name);
            pa_xfree(jack_info);
        } else {
            jack_info->handle = jack_handle;
        }

        jack_info = NULL;
        secondary_jack_info = NULL;
    }
}

static void pa_pal_card_disable_jack_detection(struct userdata *u, pa_module *m) {
    pa_pal_card_jack_info *jack_info;
    pa_pal_card_port_config *config_port = NULL;
    const char *port_name = NULL;
    void *state;
    bool external_jack = false;

    pa_assert(u);
    pa_assert(u->jacks);

    PA_HASHMAP_FOREACH(jack_info, u->jacks, state) {
        external_jack = false;
        port_name = pa_pal_util_get_port_name_from_jack_type(jack_info->jack_type);
        config_port = pa_hashmap_get(u->config_data->ports, port_name);

        if (config_port && config_port->detection) {
            if (pa_streq(config_port->detection, "external"))
                external_jack = true;
        }

        if (config_port && config_port->port_type) {
            /* no need to deregister secondary port */
            if (pa_streq(config_port->port_type, "secondary") && !external_jack)
                continue;
        }

        if (pa_pal_jack_deregister_event_callback(jack_info->handle, m, external_jack))
            pa_log_info("Jack event callback deregister successful for jack %d\n", jack_info->jack_type);
        else
            pa_log_error("Jack event callback deregister failed for jack %d\n",  jack_info->jack_type);
    }

    pa_hashmap_free(u->jacks);
    u->jacks = NULL;;
}

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_modargs *ma;

    int ret = 0;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log_error("Failed to parse module arguments");
        ma = NULL;
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->modargs = ma;
    u->module = m;
    u->core = m->core;
    u->driver = __FILE__;

    u->module_name = pa_xstrdup(pa_modargs_get_value(ma, "module", PAL_MODULE_ID_PRIMARY));

    if (pa_streq(u->module_name, PAL_MODULE_ID_PRIMARY)) {
        pa_log_debug("Loading pal module %s ", u->module_name);
    } else {
        pa_log_error("Unsupported module_name %s", u->module_name);
        goto fail;
    }

    u->conf_dir_name = pa_xstrdup(pa_modargs_get_value(ma, "conf_dir_name", NULL));
    u->conf_file_name = pa_xstrdup(pa_modargs_get_value(ma, "conf_file_name", NULL));

    u->config_data = pa_pal_config_parse_new(u->conf_dir_name, u->conf_file_name);
    if (!u->config_data) {
        pa_log_error("%s: pa_pal_config_parse_new failed", __func__);
        goto fail;
    }

    ret = agm_init();
    if (ret) {
        pa_log_error("%s: agm init failed\n", __func__);
        goto fail;
    }

    ret = pal_init();
    if (ret) {
        pa_log_error("%s: pal init failed\n", __func__);
        goto fail;
    }

    pa_pal_card_create(u);

    if (!u->config_data->default_profile) {
        pa_log_info("%s: default profile not present in card conf", __func__);
        u->config_data->default_profile = (char *)DEFAULT_PROFILE;
    }

    pa_pal_sink_module_init();
    if (pa_hashmap_size(u->config_data->sinks)) {
        u->sinks = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

        if (PA_UNLIKELY(pa_pal_card_create_sinks(u, u->config_data->default_profile, PA_PAL_CARD_USECASE_TYPE_STATIC)))
            goto fail;

    }

    pa_log_info("%s: using default profile %s", __func__, u->config_data->default_profile);

    if (pa_hashmap_size(u->config_data->sources)) {
        u->sources = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);
        if (PA_UNLIKELY(pa_pal_card_create_sources(u, u->config_data->default_profile, PA_PAL_CARD_USECASE_TYPE_STATIC)))
            goto fail;
    }

    pa_log_debug("module %s loaded", u->module_name);

    ret = pa_pal_module_extn_init(u->core, u->card);
    if(ret)
        pa_log_error("pal extn init failed\n");
    pa_log_debug("Pal extn module loaded successfully\n", __func__);

    if (pa_hashmap_size(u->config_data->loopbacks)) {
        ret = pa_pal_loopback_init(u->core, u->card, u->config_data->loopbacks, (void *)u, m);
        if (ret)
            pa_log_error("Pal loopback init failed !!");
    }

    pa_pal_card_enable_jack_detection(u);

#ifdef ENABLE_PAL_SERVICE
    load_pal_service();
#endif

    return ret;

fail:
    ret = -1;
    pa__done(m);
    return ret;
}

void pa__done(pa_module *m) {
    struct userdata *u;
    pa_card_profile *profile;
    void *state;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    pa_pal_module_extn_deinit();
    pa_pal_loopback_deinit();

    if (u->sources) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_pal_card_free_sources(u, profile->name);

        pa_hashmap_free(u->sources);
    }

    if (u->sinks) {
        PA_HASHMAP_FOREACH(profile, u->card->profiles, state)
            pa_pal_card_free_sinks(u, profile->name);

        pa_hashmap_free(u->sinks);
    }

    pa_pal_sink_module_deinit();

    pa_pal_card_disable_jack_detection(u, m);

    pal_deinit();

    agm_deinit();

    pa_pal_card_free(u);

    if (u->config_data)
        pa_pal_config_parse_free(u->config_data);

    pa_log_debug("module %s unloaded", u->module_name);

    if (u->module_name)
        pa_xfree(u->module_name);

    if (u->conf_dir_name)
        pa_xfree(u->conf_dir_name);

    if (u->conf_file_name)
        pa_xfree(u->conf_file_name);

    if (u->modargs)
        pa_modargs_free(u->modargs);

    pa_xfree(u);
}
