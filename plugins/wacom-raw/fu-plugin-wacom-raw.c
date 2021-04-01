/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fu-plugin-vfuncs.h"
#include "fu-wacom-aes-device.h"
#include "fu-wacom-emr-device.h"
#include "fu-wacom-common.h"

void
fu_plugin_init (FuPlugin *plugin)
{
	FuContext *ctx = fu_plugin_get_context (plugin);
	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
	fu_context_add_udev_subsystem (ctx, "hidraw");
	fu_context_add_quirk_key (ctx, "WacomI2cFlashBlockSize");
	fu_context_add_quirk_key (ctx, "WacomI2cFlashBaseAddr");
	fu_context_add_quirk_key (ctx, "WacomI2cFlashSize");

	/* register the custom types */
	g_type_ensure (FU_TYPE_WACOM_AES_DEVICE);
	g_type_ensure (FU_TYPE_WACOM_EMR_DEVICE);
}
