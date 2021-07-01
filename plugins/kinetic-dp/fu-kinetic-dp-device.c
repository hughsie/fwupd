/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2016 Mario Limonciello <mario.limonciello@dell.com>
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fcntl.h>

#include "fu-kinetic-dp-common.h"
#include "fu-kinetic-dp-connection.h"
#include "fu-kinetic-dp-device.h"
#include "fu-kinetic-dp-firmware.h"
#include "fu-kinetic-dp-secure-aux-isp.h"

#define KINETIC_DP_CUSTOM_UPDATE_PROTOCOL	"com.kinet-ic.dp"
#define KINETIC_DP_FWUPD_PLUGIN_NAME		"kinetic_dp"

struct _FuKineticDpDevice {
	FuUdevDevice		parent_instance;
	gchar			*system_type;
	FuKineticDpFamily	family;
	FuKineticDpMode		mode;
	guint16			chip_id;
};

G_DEFINE_TYPE (FuKineticDpDevice, fu_kinetic_dp_device, FU_TYPE_UDEV_DEVICE)

static void
fu_kinetic_dp_device_finalize (GObject *object)
{
	FuKineticDpDevice *self = FU_KINETIC_DP_DEVICE (object);

	g_free (self->system_type);

	G_OBJECT_CLASS (fu_kinetic_dp_device_parent_class)->finalize (object);
}

static void
fu_kinetic_dp_device_init (FuKineticDpDevice *self)
{
	fu_kinetic_dp_aux_isp_init ();

	fu_device_add_protocol (FU_DEVICE (self), KINETIC_DP_CUSTOM_UPDATE_PROTOCOL);
	fu_device_set_vendor (FU_DEVICE (self), "Kinetic");
	fu_device_add_vendor_id (FU_DEVICE (self), "DRM_DP_AUX_DEV:0x[TBD]");		/* <TODO> How to determine the vendor ID? */
	fu_device_set_summary (FU_DEVICE (self), "Multi-Stream Transport Device");	/* <TODO> How to distinguish SST or MST device */
	fu_device_add_icon (FU_DEVICE (self), "video-display");
	fu_device_set_version_format (FU_DEVICE (self), FWUPD_VERSION_FORMAT_TRIPLET);
	fu_udev_device_set_flags (FU_UDEV_DEVICE (self),
				  FU_UDEV_DEVICE_FLAG_OPEN_READ |
				  FU_UDEV_DEVICE_FLAG_OPEN_WRITE |
				  FU_UDEV_DEVICE_FLAG_VENDOR_FROM_PARENT);
}

static gboolean
fu_kinetic_dp_device_probe (FuDevice *device, GError **error)
{
	/* FuUdevDevice->probe */
	if (!FU_DEVICE_CLASS (fu_kinetic_dp_device_parent_class)->probe (device, error))
		return FALSE;

	/* get from sysfs if not set from tests */
	if (fu_device_get_logical_id (device) == NULL) {
		g_autofree gchar *logical_id = NULL;
		logical_id = g_path_get_basename (fu_udev_device_get_sysfs_path (FU_UDEV_DEVICE (device)));
		fu_device_set_logical_id (device, logical_id);
	}

	return fu_udev_device_set_physical_id (FU_UDEV_DEVICE (device), "pci,drm_dp_aux_dev", error);
}

static FuFirmware *
fu_kinetic_dp_device_prepare_firmware (FuDevice *device,
				       GBytes *fw,
				       FwupdInstallFlags flags,
				       GError **error)
{
	g_autoptr (FuFirmware) firmware = fu_kinetic_dp_firmware_new ();

	/* parse input firmware file to two images */
	if (!fu_firmware_parse (firmware, fw, flags, error))
		return NULL;

	return g_steal_pointer (&firmware);
}

static gboolean
fu_kinetic_dp_device_write_firmware (FuDevice *device,
				     FuFirmware *firmware,
				     FwupdInstallFlags flags,
				     GError **error)
{
	FuKineticDpDevice *self = FU_KINETIC_DP_DEVICE (device);
	g_autoptr (FuDeviceLocker) locker = NULL;

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);

	/* update firmware */
	if (!fu_kinetic_dp_aux_isp_start (self, firmware, error)) {
		g_prefix_error (error, "Firmware update failed: ");
		return FALSE;
	}

	/* wait for flash clear to settle */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
	fu_device_sleep_with_progress (device, 2);

	return TRUE;
}

FuKineticDpDevice *
fu_kinetic_dp_device_new (FuUdevDevice *device)
{
	FuKineticDpDevice *self = g_object_new (FU_TYPE_KINETIC_DP_DEVICE, NULL);
	fu_device_incorporate (FU_DEVICE (self), FU_DEVICE (device));
	return self;
}

void
fu_kinetic_dp_device_set_system_type (FuKineticDpDevice *self, const gchar *system_type)
{
	g_return_if_fail (FU_IS_KINETIC_DP_DEVICE (self));
	self->system_type = g_strdup (system_type);
}

static gboolean
fu_kinetic_dp_device_rescan (FuDevice *device, GError **error)
{
	FuKineticDpDevice *self = FU_KINETIC_DP_DEVICE (device);
	g_autoptr (FuKineticDpConnection) connection = NULL;
	g_autofree gchar *name = NULL;
	g_autofree gchar *guid1 = NULL;
	g_autofree gchar *guid2 = NULL;
	const gchar *name_parent;
	const gchar *name_family;

	KtDpDevInfo *dp_dev_info = NULL;

	/* <TODO> Now only support to do ISP for Host chip */
	if (!fu_kinetic_dp_aux_isp_read_device_info (self, DEV_HOST, &dp_dev_info, error)) {
		g_prefix_error (error, "Failed to read device info: ");
		return FALSE;
	}

	g_debug ("branch_id_str = %s", dp_dev_info->branch_id_str);

	/* <TODO> read firmware version */

	self->family = fu_kinetic_dp_chip_id_to_family (dp_dev_info->chip_id);

	/* Convert Kinetic chip id to numeric representation */
	self->chip_id = fu_kinetic_dp_aux_isp_get_numeric_chip_id (dp_dev_info->chip_id);

	/* set up the device name */
	name_parent = fu_device_get_name (FU_DEVICE (self));
	if (name_parent != NULL) {
		name = g_strdup_printf ("KT%04x inside %s", self->chip_id, name_parent);
	} else {
		name = g_strdup_printf ("KT%04x", self->chip_id);
	}
	fu_device_set_name (FU_DEVICE (self), name);

	/* detect chip family */
	switch (self->family) {
	case FU_KINETIC_DP_FAMILY_JAGUAR:
		/* <TODO> Set max firmware size for Jaguar */
		fu_device_add_instance_id_full (device, "KTDP-KT50X0", FU_DEVICE_INSTANCE_FLAG_ONLY_QUIRKS);
		break;
	case FU_KINETIC_DP_FAMILY_MUSTANG:
		/* <TODO> Determine max firmware size for Mustang */
		fu_device_add_instance_id_full (device, "KTDP-KT52X0", FU_DEVICE_INSTANCE_FLAG_ONLY_QUIRKS);
		break;
	default:
		break;
	}

	/* add non-standard GUIDs */
	name_family = fu_kinetic_dp_family_to_string (self->family);
	guid1 = g_strdup_printf ("KT-DP-%s-KT%04x", name_family, self->chip_id);
	fu_device_add_instance_id (FU_DEVICE (self), guid1);
	guid2 = g_strdup_printf ("KT-DP-%s", name_family);
	fu_device_add_instance_id (FU_DEVICE (self), guid2);

	/* <TODO> check if a valid device to update */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);

	return TRUE;
}

static void
fu_kinetic_dp_device_class_init (FuKineticDpDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	FuDeviceClass *klass_device = FU_DEVICE_CLASS  (klass);

	object_class->finalize = fu_kinetic_dp_device_finalize;

	klass_device->rescan = fu_kinetic_dp_device_rescan;
	klass_device->write_firmware = fu_kinetic_dp_device_write_firmware;
	klass_device->prepare_firmware = fu_kinetic_dp_device_prepare_firmware;
	klass_device->probe = fu_kinetic_dp_device_probe;
}
