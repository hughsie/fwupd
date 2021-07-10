/*
 * Copyright (C) 2021 Daniel Campello <campello@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fu-flashrom-device.h"
#include "fu-flashrom-internal-device.h"

#include <libflashrom.h>

#define HSFS_FDOPSS (1 << 13)

struct _FuFlashromInternalDevice {
	FuFlashromDevice		 parent_instance;
};

typedef struct {
	gboolean	me_region_flashable;
} FuFlashromInternalDevicePrivate;

G_DEFINE_TYPE_WITH_PRIVATE (FuFlashromInternalDevice, fu_flashrom_internal_device,
	       FU_TYPE_FLASHROM_DEVICE)

#define GET_PRIVATE(o) (fu_flashrom_internal_device_get_instance_private (o))

static void
fu_flashrom_internal_device_init (FuFlashromInternalDevice *self)
{
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_instance_id (FU_DEVICE (self), "main-system-firmware");
	fu_device_add_internal_flag (FU_DEVICE (self), FU_DEVICE_INTERNAL_FLAG_ENSURE_SEMVER);
	fu_device_set_physical_id (FU_DEVICE (self), "flashrom");
	fu_device_set_logical_id (FU_DEVICE (self), "bios");
	fu_device_set_version_format (FU_DEVICE (self), FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_add_icon (FU_DEVICE (self), "computer");
}

static void
set_fdopss_lock_state (FuDevice *device, GError **error)
{
	const guint16 hsfs = flashrom_tuxedo_get_hsfs();
	FuFlashromInternalDevicePrivate *priv =
		GET_PRIVATE (FU_FLASHROM_INTERNAL_DEVICE (device));
	if (hsfs & HSFS_FDOPSS) {
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_LOCKED);
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN);
		fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	} else if (hsfs > 0) {
		priv->me_region_flashable = true;
	}
}

static gboolean
fu_flashrom_internal_device_set_quirk_kv (FuDevice *device,
				 const gchar *key,
				 const gchar *value,
				 GError **error)
{
	if (g_strcmp0 (key, "FlashromNeedsFdopssUnlock") == 0) {
		set_fdopss_lock_state (device);
		return TRUE;
	}
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "not supported");
	return FALSE;
}

static gboolean
fu_flashrom_internal_device_prepare (FuDevice *device,
				     FwupdInstallFlags flags,
				     GError **error)
{
	g_autofree gchar *firmware_orig = NULL;
	g_autofree gchar *localstatedir = NULL;
	g_autofree gchar *basename = NULL;

	/* if the original firmware doesn't exist, grab it now */
	basename = g_strdup_printf ("flashrom-%s.bin", fu_device_get_id (device));
	localstatedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	firmware_orig = g_build_filename (localstatedir, "builder", basename, NULL);
	if (!fu_common_mkdir_parent (firmware_orig, error))
		return FALSE;
	if (!g_file_test (firmware_orig, G_FILE_TEST_EXISTS)) {
		FuFlashromDevice *parent = FU_FLASHROM_DEVICE (device);
		struct flashrom_flashctx *flashctx = fu_flashrom_device_get_flashctx (parent);
		gsize flash_size = fu_flashrom_device_get_flash_size (parent);
		g_autofree guint8 *newcontents = g_malloc0 (flash_size);
		g_autoptr(GBytes) buf = NULL;

		fu_device_set_status (device, FWUPD_STATUS_DEVICE_READ);
		if (flashrom_image_read (flashctx, newcontents, flash_size)) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_READ,
					     "failed to back up original firmware");
			return FALSE;
		}
		buf = g_bytes_new_static (newcontents, flash_size);
		if (!fu_common_set_contents_bytes (firmware_orig, buf, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
fu_flashrom_internal_device_write_firmware (FuDevice *device,
					    FuFirmware *firmware,
					    FwupdInstallFlags flags,
					    GError **error)
{
	FuFlashromDevice *parent = FU_FLASHROM_DEVICE (device);
	FuFlashromInternalDevicePrivate *priv =
		GET_PRIVATE (FU_FLASHROM_INTERNAL_DEVICE (device));
	struct flashrom_flashctx *flashctx = fu_flashrom_device_get_flashctx (parent);
	gsize flash_size = fu_flashrom_device_get_flash_size (parent);
	struct flashrom_layout *layout;
	gsize sz = 0;
	gint rc;
	const guint8 *buf;
	g_autoptr(GBytes) blob_fw = fu_firmware_get_bytes (firmware, error);
	if (blob_fw == NULL)
		return FALSE;

	buf = g_bytes_get_data (blob_fw, &sz);

	if (flashrom_layout_read_from_ifd (&layout, flashctx, NULL, 0)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_READ,
				     "failed to read layout from Intel ICH descriptor");
		return FALSE;
	}

	/* include bios region for safety reasons */
	if (flashrom_layout_include_region (layout, "bios")) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "invalid region name");
		return FALSE;
	}

	/* include me region for devices with fdopss override functionality */
	if (priv->me_region_flashable && flashrom_layout_include_region (layout, "me")) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "invalid region name");
		return FALSE;
	}

	/* write region */
	flashrom_layout_set (flashctx, layout);
	if (sz != flash_size) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "invalid image size 0x%x, expected 0x%x",
			     (guint) sz, (guint) flash_size);
		return FALSE;
	}

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	fu_device_set_progress (device, 0); /* urgh */
	rc = flashrom_image_write (flashctx, (void *) buf, sz, NULL /* refbuffer */);
	if (rc != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "image write failed, err=%i", rc);
		return FALSE;
	}

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_VERIFY);
	if (flashrom_image_verify (flashctx, (void *) buf, sz)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "image verify failed");
		return FALSE;
	}
	flashrom_layout_release (layout);

	/* success */
	return TRUE;
}

static void
fu_flashrom_internal_device_class_init (FuFlashromInternalDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	klass_device->prepare = fu_flashrom_internal_device_prepare;
	klass_device->write_firmware = fu_flashrom_internal_device_write_firmware;
	klass_device->set_quirk_kv = fu_flashrom_internal_device_set_quirk_kv;
}

FuDevice *
fu_flashrom_internal_device_new (void)
{
	return FU_DEVICE (g_object_new (FU_TYPE_FLASHROM_INTERNAL_DEVICE, NULL));
}
