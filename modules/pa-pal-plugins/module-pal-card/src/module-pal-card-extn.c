/*
 * Copyright (c) 2021, 2023-2025 Qualcomm Innovation Center, Inc. All rights reserved.
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
#include <pulsecore/thread-mq.h>
#include <pulsecore/shared.h>
#include <stdio.h>

#ifdef PAL_USES_CUTILS
#include <cutils/str_parms.h>
#endif

#include <PalApi.h>
#include <PalDefs.h>

#include "pal-card.h"
#include "pal-source.h"
#include "pal-sink.h"
#include "pal-config-parser.h"

//to be updated in PalDefs.h
#define PAL_PARAM_SET_CUSTOM_VOLUME_INDEX 52
#define PAL_PARAM_SET_CUSTOM_VOIP_ENABLE 53
#define PAL_PARAM_SET_CUSTOM_VOICE_RECOGNITION_ENABLE 54
#define PAL_PARAM_SET_CUSTOM_BARGEIN_ENABLE 55

/*Param key strings to be validated against key received from client*/
#define PAL_PARAM_KEY_VOLUME_INDEX "l_volume_idx"
#define PAL_PARAM_KEY_VOIP "l_voip_enable"
#define PAL_PARAM_KEY_VOICE_RECOGNITION "l_voice_recognition_enable"
#define PAL_PARAM_KEY_BARGEIN "l_bargein_enable"

#define PAL_DBUS_OBJECT_PATH_PREFIX "/org/pulseaudio/ext/pal"
#define PAL_DBUS_MODULE_IFACE "org.PulseAudio.Ext.Pal.Module"

#define OK 0


#ifndef PAL_USES_CUTILS
struct str_parms *str_parms_create_str(const char *_string){return NULL;}
int str_parms_get_str(struct str_parms *str_parms, const char *key,
                      char *out_val, int len){return 0;}
char *str_parms_to_str(struct str_parms *str_parms){return NULL;}
int str_parms_add_str(struct str_parms *str_parms, const char *key,
                      const char *value){return 0;}
struct str_parms *str_parms_create(void){return NULL;}
void str_parms_del(struct str_parms *str_parms, const char *key){return;}
void str_parms_destroy(struct str_parms *str_parms){return;}
#endif
struct pal_module_extn_data {
	char *obj_path;
	pa_dbus_protocol *dbus_protocol;
	pa_card *card;
};

static struct pal_module_extn_data *pal_extn_mdata = NULL;

/* key,value based set params*/
static void pal_module_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);
static void pal_module_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata);

enum module_method_handler_index {
	METHOD_HANDLER_SET_PARAMETERS,
	METHOD_HANDLER_GET_PARAMETERS,
	METHOD_HANDLER_MODULE_LAST = METHOD_HANDLER_GET_PARAMETERS,
	METHOD_HANDLER_MODULE_MAX = METHOD_HANDLER_MODULE_LAST + 1,
};

static pa_dbus_arg_info set_parameters_args[] = {
	{"kv_pairs", "s", "in"},
};

static pa_dbus_arg_info get_parameters_args[] = {
	{"kv_pairs", "s", "in"},
	{"value", "s", "out"}
};

static pa_dbus_method_handler module_method_handlers[METHOD_HANDLER_MODULE_MAX] = {
	[METHOD_HANDLER_SET_PARAMETERS] = {
		.method_name = "SetParameters",
		.arguments = set_parameters_args,
		.n_arguments = sizeof(set_parameters_args)/sizeof(pa_dbus_arg_info),
		.receive_cb = pal_module_set_parameters},
	[METHOD_HANDLER_GET_PARAMETERS] = {
		.method_name = "GetParameters",
		.arguments = get_parameters_args,
		.n_arguments = sizeof(get_parameters_args)/sizeof(pa_dbus_arg_info),
		.receive_cb = pal_module_get_parameters},
};

static pa_dbus_interface_info module_interface_info = {
	.name = PAL_DBUS_MODULE_IFACE,
	.method_handlers = module_method_handlers,
	.n_method_handlers = METHOD_HANDLER_MODULE_MAX,
	.property_handlers = NULL,
	.n_property_handlers = 0,
	.get_all_properties_cb = NULL,
};

static void pal_module_set_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
	int status = 0, err;
	DBusError error;
	const char *kvpairs = NULL;
	pal_param_payload *param_payload = NULL;
	struct str_parms *parms = NULL;
	char c_value[32];

	pa_assert(conn);
	pa_assert(msg);
	pa_assert(userdata);
	dbus_error_init(&error);

	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &kvpairs, DBUS_TYPE_INVALID)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
		dbus_error_free(&error);
		return;
	}
	parms = str_parms_create_str(kvpairs);
	if (!parms) {
		pa_log_error("failed to create params\n");
		status = -1;
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_VOLUME_INDEX, c_value, sizeof(c_value));
	if (err >= 0) {
		int volume_idx;
		volume_idx = atoi(c_value);
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
					sizeof(volume_idx));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) +
						sizeof(volume_idx));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(volume_idx);
			memcpy(&param_payload->payload[0], &volume_idx, sizeof(volume_idx));
			status =  pal_set_param(PAL_PARAM_SET_CUSTOM_VOLUME_INDEX, &param_payload,
						sizeof(param_payload->payload_size));
			if (status)
				pa_log_error("Volume set failed with status %x",status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_VOIP, c_value, sizeof(c_value));
	if (err >= 0) {
		bool voip_enable;
		if (strcmp(c_value, "true") == 0)
			voip_enable = true;
		else if (strcmp(c_value, "false") == 0) {
			voip_enable = false;
		}else{
			str_parms_destroy(parms);
			pa_log_error("Invalid param value.");
			status = -1;
			goto done;
		}

		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) + sizeof(bool));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) + sizeof(bool));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(bool);
			memcpy(&param_payload->payload[0], &voip_enable, sizeof(voip_enable));
			status = pal_set_param(PAL_PARAM_SET_CUSTOM_VOIP_ENABLE, &param_payload,
						sizeof(param_payload->payload_size));

			if (status)
				pa_log_error("Voip enable set failed with status %x", status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_VOICE_RECOGNITION, c_value, sizeof(c_value));
	if (err >= 0) {
		bool voice_recog_enable;
		if (strcmp(c_value, "true") == 0)
			voice_recog_enable = true;
		else if (strcmp(c_value, "false") == 0) {
			voice_recog_enable = false;
		}else{
			str_parms_destroy(parms);
			pa_log_error("Invalid param value.");
			status = -1;
			goto done;
		}
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) + sizeof(bool));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) + sizeof(bool));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(bool);
			memcpy(&param_payload->payload[0], &voice_recog_enable, sizeof(voice_recog_enable));
			status = pal_set_param(PAL_PARAM_SET_CUSTOM_VOICE_RECOGNITION_ENABLE, &param_payload,
						sizeof(param_payload->payload_size));

			if (status)
				pa_log_error("Voice recognition set failed with status %x",status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}
	err = str_parms_get_str(parms, PAL_PARAM_KEY_BARGEIN, c_value, sizeof(c_value));
	if (err >= 0) {
		bool bargein_enable;
		if (strcmp(c_value, "true") == 0)
			bargein_enable = true;
		else if (strcmp(c_value, "false") == 0) {
			bargein_enable = false;
		}else{
			str_parms_destroy(parms);
			pa_log_error("Invalid param value");
			status = -1;
			goto done;
		}
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) + sizeof(bool));
		if (!param_payload) {
			pa_log_error("calloc failed for size %zu", sizeof(pal_param_payload) + sizeof(bool));
			status = -1;
		} else {
			param_payload->payload_size = sizeof(bool);
			memcpy(&param_payload->payload[0], &bargein_enable, sizeof(bargein_enable));
			status = pal_set_param(PAL_PARAM_SET_CUSTOM_BARGEIN_ENABLE, &param_payload,
						sizeof(param_payload->payload_size));
			if (status)
				pa_log_error("Bargein enable set failed with status %x", status);
			free(param_payload);
			param_payload = NULL;
		}
		goto done;
	}

done:
	if (OK != status) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "set_param failed");
		dbus_error_free(&error);
		return;
	}
	pa_dbus_send_empty_reply(conn, msg);
}

static void pal_module_get_parameters(DBusConnection *conn, DBusMessage *msg, void *userdata)
{
	int status = 0;
	DBusError error;
	const char *kvpairs = NULL;
	DBusMessage *reply = NULL;
	pal_param_payload *param_payload = NULL;

	pa_assert(conn);
	pa_assert(msg);
	pa_assert(userdata);
	dbus_error_init(&error);

	if (!dbus_message_get_args(msg, &error, DBUS_TYPE_STRING, &kvpairs,DBUS_TYPE_INVALID)) {
		pa_dbus_send_error(conn, msg, DBUS_ERROR_INVALID_ARGS, "%s", error.message);
		dbus_error_free(&error);
		return;
	}

	if (strcmp("device_mute", kvpairs) == 0) {
		pal_device_mute_t *pdev_mute = NULL;
		param_payload = (pal_param_payload *)calloc(1, sizeof(pal_param_payload) +
					sizeof(pal_device_mute_t));
		status = pal_get_param(PAL_PARAM_ID_DEVICE_MUTE, (void **)&param_payload,
					(size_t *)&param_payload->payload_size, NULL);
		if (status) {
			pa_dbus_send_error(conn, msg, DBUS_ERROR_FAILED, "get_param failed");
			dbus_error_free(&error);
			free(param_payload);
			return;
		}
		pdev_mute = (pal_device_mute_t*)(uintptr_t)param_payload->payload[0];
		pa_dbus_send_basic_value_reply(conn, msg, DBUS_TYPE_STRING, &pdev_mute->mute);
	}
}

int pa_pal_module_extn_init(pa_core *core, pa_card *card)
{
	pa_assert(core);
	pa_assert(card);

	if (pal_extn_mdata) {
		pa_log_info("%s: Module already intialized",__func__);
		return -1;
	}

	pa_log_info("%s", __func__);
	pal_extn_mdata = pa_xnew0(struct pal_module_extn_data, 1);
	pal_extn_mdata->obj_path = pa_sprintf_malloc("%s", PAL_DBUS_OBJECT_PATH_PREFIX);
	pal_extn_mdata->dbus_protocol = pa_dbus_protocol_get(core);
	pal_extn_mdata->card = card;

	pa_assert_se(pa_dbus_protocol_add_interface(pal_extn_mdata->dbus_protocol,
					pal_extn_mdata->obj_path, &module_interface_info, pal_extn_mdata) >= 0);
	return 0;
}

void pa_pal_module_extn_deinit(void)
{
	pa_assert(pal_extn_mdata);
	pa_assert(pal_extn_mdata->dbus_protocol);
	pa_assert(pal_extn_mdata->obj_path);
	pa_assert_se(pa_dbus_protocol_remove_interface(pal_extn_mdata->dbus_protocol,
				pal_extn_mdata->obj_path, module_interface_info.name) >= 0);
	pa_dbus_protocol_unref(pal_extn_mdata->dbus_protocol);
	pa_xfree(pal_extn_mdata->obj_path);
	pa_xfree(pal_extn_mdata);
	pal_extn_mdata = NULL;
}
