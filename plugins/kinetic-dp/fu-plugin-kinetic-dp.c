/*
 * Copyright (C) 2017 Mario Limonciello <mario.limonciello@dell.com>
 * Copyright (C) 2017 Peichen Huang <peichenhuang@tw.kinetic.com>
 * Copyright (C) 2017 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include <fwupdplugin.h>

#include "fu-kinetic-dp-common.h"
#include "fu-kinetic-dp-device.h"
#include "fu-kinetic-dp-firmware.h"

#define FU_KINETIC_DP_DRM_REPLUG_DELAY	5	/* Unit: sec */

struct FuPluginData {
	GPtrArray	*devices;
	guint		drm_changed_id;
};

static void
fu_plugin_kinetic_dp_device_rescan (FuPlugin *plugin, FuDevice *device)
{
	g_autoptr (FuDeviceLocker) locker = NULL;
	g_autoptr (GError) error_local = NULL;

	/* open fd */
	locker = fu_device_locker_new (device, &error_local);
	if (locker == NULL) {
		g_debug ("Failed to open device %s: %s", fu_device_get_logical_id (device), error_local->message);
		return;
	}

	if (!fu_device_rescan (device, &error_local)) {
		g_debug ("No device found on %s: %s", fu_device_get_logical_id (device), error_local->message);
		if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED))
			fu_plugin_device_remove (plugin, device);
	} else {
		fu_plugin_device_add (plugin, device);
	}
}

/* reprobe all existing devices added by this plugin */
static void
fu_plugin_kinetic_dp_rescan (FuPlugin *plugin)
{
	FuPluginData *priv = fu_plugin_get_data (plugin);
	for (guint i = 0; i < priv->devices->len; i++) {
		FuDevice *device = FU_DEVICE (g_ptr_array_index (priv->devices, i));
		fu_plugin_kinetic_dp_device_rescan (plugin, device);
	}
}

static gboolean
fu_plugin_kinetic_dp_rescan_cb (gpointer user_data)
{
	FuPlugin *plugin = FU_PLUGIN (user_data);
	FuPluginData *priv = fu_plugin_get_data (plugin);
	fu_plugin_kinetic_dp_rescan (plugin);
	priv->drm_changed_id = 0;

	return FALSE;
}

gboolean
fu_plugin_backend_device_changed (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuPluginData *priv = fu_plugin_get_data (plugin);

	/* interesting device? */
	if (!FU_IS_UDEV_DEVICE (device))
		return TRUE;
	if (g_strcmp0 (fu_udev_device_get_subsystem (FU_UDEV_DEVICE (device)), "drm") != 0)
		return TRUE;

	/* recoldplug all drm_dp_aux_dev devices after a *long* delay */
	if (priv->drm_changed_id != 0)
		g_source_remove (priv->drm_changed_id);
	priv->drm_changed_id = g_timeout_add_seconds(FU_KINETIC_DP_DRM_REPLUG_DELAY,
						     fu_plugin_kinetic_dp_rescan_cb,
						     plugin);

	return TRUE;
}

gboolean
fu_plugin_backend_device_added (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuContext *ctx = fu_plugin_get_context (plugin);
	FuPluginData *priv = fu_plugin_get_data (plugin);
	g_autoptr (FuDeviceLocker) locker = NULL;
	g_autoptr (FuKineticDpDevice) dev = NULL;

	/* interesting device? */
	if (!FU_IS_UDEV_DEVICE (device))
		return TRUE;

	dev = fu_kinetic_dp_device_new (FU_UDEV_DEVICE (device));
	locker = fu_device_locker_new (dev, error);
	if (locker == NULL)
		return FALSE;

	/* for DeviceKind=system devices */
	fu_kinetic_dp_device_set_system_type (FU_KINETIC_DP_DEVICE (dev),
					      fu_context_get_hwid_value (ctx, FU_HWIDS_KEY_PRODUCT_SKU));

	/* this might fail if there is nothing connected */
	fu_plugin_kinetic_dp_device_rescan (plugin, FU_DEVICE (dev));
	g_ptr_array_add (priv->devices, g_steal_pointer  (&dev));

	return TRUE;
}

gboolean
fu_plugin_update (FuPlugin *plugin,
		  FuDevice *device,
		  GBytes *blob_fw,
		  FwupdInstallFlags flags,
		  GError **error)
{
	g_autoptr (FuDeviceLocker) locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	if (!fu_device_write_firmware (device, blob_fw, flags, error))
		return FALSE;
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_SKIPS_RESTART))
		fu_plugin_device_remove (plugin, device);

	return TRUE;
}

void
fu_plugin_init (FuPlugin *plugin)
{
	FuContext *ctx = fu_plugin_get_context (plugin);
	FuPluginData *priv = fu_plugin_alloc_data (plugin, sizeof (FuPluginData));

	/* devices added by this plugin */
	priv->devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
	fu_context_add_udev_subsystem (ctx, "drm");	/* used for uevent only */
	fu_context_add_udev_subsystem (ctx, "drm_dp_aux_dev");
	fu_plugin_add_firmware_gtype (plugin, NULL, FU_TYPE_KINETIC_DP_FIRMWARE);
}

void
fu_plugin_destroy (FuPlugin *plugin)
{
	FuPluginData *priv = fu_plugin_get_data (plugin);
	if (priv->drm_changed_id != 0)
		g_source_remove (priv->drm_changed_id);
	g_ptr_array_unref (priv->devices);
}
