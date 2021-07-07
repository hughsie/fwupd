/*
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2015 Peter Jones <pjones@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>
#include <fcntl.h>
#include <glib/gi18n.h>

#include "fu-uefi-backend.h"
#include "fu-uefi-bgrt.h"
#include "fu-uefi-bootmgr.h"
#include "fu-uefi-common.h"
#include "fu-uefi-device.h"

struct FuPluginData {
	FuUefiBgrt		*bgrt;
	FuVolume		*esp;
	FuBackend		*backend;
};

void
fu_plugin_init (FuPlugin *plugin)
{
	FuContext *ctx = fu_plugin_get_context (plugin);
	FuPluginData *data = fu_plugin_alloc_data (plugin, sizeof (FuPluginData));
	data->backend = fu_uefi_backend_new (ctx);
	data->bgrt = fu_uefi_bgrt_new ();
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_RUN_AFTER, "upower");
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_METADATA_SOURCE, "tpm");
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_METADATA_SOURCE, "tpm_eventlog");
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_METADATA_SOURCE, "dell");
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_METADATA_SOURCE, "linux_lockdown");
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_METADATA_SOURCE, "acpi_phat");
	fu_plugin_add_rule (plugin, FU_PLUGIN_RULE_CONFLICTS, "uefi"); /* old name */
	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
}

void
fu_plugin_destroy (FuPlugin *plugin)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	if (data->esp != NULL)
		g_object_unref (data->esp);
	g_object_unref (data->backend);
	g_object_unref (data->bgrt);
}

gboolean
fu_plugin_clear_results (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuUefiDevice *device_uefi = FU_UEFI_DEVICE (device);
	return fu_uefi_device_clear_status (device_uefi, error);
}

gboolean
fu_plugin_get_results (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuUefiDevice *device_uefi = FU_UEFI_DEVICE (device);
	FuUefiDeviceStatus status = fu_uefi_device_get_status (device_uefi);
	const gchar *tmp;
	g_autofree gchar *err_msg = NULL;
	g_autofree gchar *version_str = NULL;
	g_autoptr(GError) error_local = NULL;

	/* trivial case */
	if (status == FU_UEFI_DEVICE_STATUS_SUCCESS) {
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_SUCCESS);
		return TRUE;
	}

	/* check if something rudely removed our BOOTXXXX entry */
	if (!fu_uefi_bootmgr_verify_fwupd (&error_local)) {
		if (fu_plugin_has_custom_flag (plugin, "boot-order-lock")) {
			g_prefix_error (&error_local,
					"boot entry missing; "
					"perhaps 'Boot Order Lock' enabled in the BIOS: ");
			fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED_TRANSIENT);
		} else {
			g_prefix_error (&error_local, "boot entry missing: ");
			fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
		}
		fu_device_set_update_error (device, error_local->message);
		return TRUE;
	}

	/* something went wrong */
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_PWR_EVT_AC ||
	    status == FU_UEFI_DEVICE_STATUS_ERROR_PWR_EVT_BATT) {
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED_TRANSIENT);
	} else {
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
	}
	version_str = g_strdup_printf ("%u", fu_uefi_device_get_version_error (device_uefi));
	tmp = fu_uefi_device_status_to_string (status);
	if (tmp == NULL) {
		err_msg = g_strdup_printf ("failed to update to %s",
					   version_str);
	} else {
		err_msg = g_strdup_printf ("failed to update to %s: %s",
					   version_str, tmp);
	}
	fu_device_set_update_error (device, err_msg);
	return TRUE;
}

void
fu_plugin_add_security_attrs (FuPlugin *plugin, FuSecurityAttrs *attrs)
{
	g_autoptr(FwupdSecurityAttr) attr = NULL;
	g_autoptr(GError) error = NULL;

	/* create attr */
	attr = fwupd_security_attr_new (FWUPD_SECURITY_ATTR_ID_UEFI_SECUREBOOT);
	fwupd_security_attr_set_plugin (attr, fu_plugin_get_name (plugin));
	fu_security_attrs_append (attrs, attr);

	/* SB not available or disabled */
	if (!fu_efivar_secure_boot_enabled_full (&error)) {
		if (g_error_matches (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED)) {
			fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_FOUND);
			return;
		}
		fwupd_security_attr_add_flag (attr, FWUPD_SECURITY_ATTR_FLAG_RUNTIME_ISSUE);
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_ENABLED);
		return;
	}

	/* success */
	fwupd_security_attr_add_flag (attr, FWUPD_SECURITY_ATTR_FLAG_SUCCESS);
	fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_ENABLED);
}

static GBytes *
fu_plugin_uefi_capsule_get_splash_data (guint width, guint height, GError **error)
{
	const gchar * const *langs = g_get_language_names ();
	g_autofree gchar *datadir_pkg = NULL;
	g_autofree gchar *filename_archive = NULL;
	g_autofree gchar *langs_str = NULL;
	g_autoptr(FuArchive) archive = NULL;
	g_autoptr(GBytes) blob_archive = NULL;

	/* load archive */
	datadir_pkg = fu_common_get_path (FU_PATH_KIND_DATADIR_PKG);
	filename_archive = g_build_filename (datadir_pkg, "uefi-capsule-ux.tar.xz", NULL);
	blob_archive = fu_common_get_contents_bytes (filename_archive, error);
	if (blob_archive == NULL)
		return NULL;
	archive = fu_archive_new (blob_archive, FU_ARCHIVE_FLAG_NONE, error);
	if (archive == NULL)
		return NULL;

	/* find the closest locale match, falling back to `en` and `C` */
	for (guint i = 0; langs[i] != NULL; i++) {
		GBytes *blob_tmp;
		g_autofree gchar *fn = NULL;
		if (g_str_has_suffix (langs[i], ".UTF-8"))
			continue;
		fn = g_strdup_printf ("fwupd-%s-%u-%u.bmp", langs[i], width, height);
		blob_tmp = fu_archive_lookup_by_fn (archive, fn, NULL);
		if (blob_tmp != NULL) {
			g_debug ("using UX image %s", fn);
			return g_bytes_ref (blob_tmp);
		}
		g_debug ("no %s found", fn);
	}

	/* we found nothing */
	langs_str = g_strjoinv (",", (gchar **) langs);
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "failed to get splash file for %s in %s",
		     langs_str, datadir_pkg);
	return NULL;
}

static guint8
fu_plugin_uefi_capsule_calc_checksum (const guint8 *buf, gsize sz)
{
	guint8 csum = 0;
	for (gsize i = 0; i < sz; i++)
		csum += buf[i];
	return csum;
}

static gboolean
fu_plugin_uefi_capsule_write_splash_data (FuPlugin *plugin,
					  FuDevice *device,
					  GBytes *blob,
					  GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	guint32 screen_x, screen_y;
	gsize buf_size = g_bytes_get_size (blob);
	gssize size;
	guint32 height, width;
	guint8 csum = 0;
	efi_ux_capsule_header_t header = { 0 };
	efi_capsule_header_t capsule_header = {
		.flags = EFI_CAPSULE_HEADER_FLAGS_PERSIST_ACROSS_RESET,
		.guid = { 0x0 },
		.header_size = sizeof(efi_capsule_header_t),
		.capsule_image_size = 0
	};
	g_autofree gchar *esp_path = NULL;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *basename = NULL;
	g_autoptr(GFile) ofile = NULL;
	g_autoptr(GOutputStream) ostream = NULL;

	/* get screen dimensions */
	if (!fu_uefi_get_framebuffer_size (&screen_x, &screen_y, error))
		return FALSE;
	if (!fu_uefi_get_bitmap_size ((const guint8 *) g_bytes_get_data (blob, NULL),
				      buf_size, &width, &height, error)) {
		g_prefix_error (error, "splash invalid: ");
		return FALSE;
	}

	/* save to a predicatable filename */
	esp_path = fu_volume_get_mount_point (data->esp);
	directory = fu_uefi_get_esp_path_for_os (device, esp_path);
	basename = g_strdup_printf ("fwupd-%s.cap", FU_EFIVAR_GUID_UX_CAPSULE);
	fn = g_build_filename (directory, "fw", basename, NULL);
	if (!fu_common_mkdir_parent (fn, error))
		return FALSE;
	ofile = g_file_new_for_path (fn);
	ostream = G_OUTPUT_STREAM (g_file_replace (ofile, NULL, FALSE,
				   G_FILE_CREATE_NONE, NULL, error));
	if (ostream == NULL)
		return FALSE;

	if (!fwupd_guid_from_string (FU_EFIVAR_GUID_UX_CAPSULE,
				     &capsule_header.guid,
				     FWUPD_GUID_FLAG_MIXED_ENDIAN,
				     error))
		return FALSE;
	capsule_header.capsule_image_size =
		g_bytes_get_size (blob) +
		sizeof(efi_capsule_header_t) +
		sizeof(efi_ux_capsule_header_t);

	header.version = 1;
	header.image_type = 0;
	header.reserved = 0;
	header.x_offset = (screen_x / 2) - (width / 2);
	header.y_offset = fu_uefi_bgrt_get_yoffset (data->bgrt) +
				fu_uefi_bgrt_get_height (data->bgrt);

	/* header, payload and image has to add to zero */
	csum += fu_plugin_uefi_capsule_calc_checksum ((guint8 *) &capsule_header,
						      sizeof(capsule_header));
	csum += fu_plugin_uefi_capsule_calc_checksum ((guint8 *) &header,
						      sizeof(header));
	csum += fu_plugin_uefi_capsule_calc_checksum (g_bytes_get_data (blob, NULL),
						      g_bytes_get_size (blob));
	header.checksum = 0x100 - csum;

	/* write capsule file */
	size = g_output_stream_write (ostream, &capsule_header,
				      capsule_header.header_size, NULL, error);
	if (size < 0)
		return FALSE;
	size = g_output_stream_write (ostream, &header, sizeof(header), NULL, error);
	if (size < 0)
		return FALSE;
	size = g_output_stream_write_bytes (ostream, blob, NULL, error);
	if (size < 0)
		return FALSE;

	/* write display capsule location as UPDATE_INFO */
	return fu_uefi_device_write_update_info (FU_UEFI_DEVICE (device), fn,
						 "fwupd-ux-capsule",
						 FU_EFIVAR_GUID_UX_CAPSULE,
						 error);
}

static gboolean
fu_plugin_uefi_capsule_update_splash (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	guint best_idx = G_MAXUINT;
	guint32 lowest_border_pixels = G_MAXUINT;
	guint32 screen_height = 768;
	guint32 screen_width = 1024;
	g_autoptr(GBytes) image_bmp = NULL;

	struct {
		guint32	 width;
		guint32	 height;
	} sizes[] = {
		{ 640, 480 },	/* matching the sizes in po/make-images */
		{ 800, 600 },
		{ 1024, 768 },
		{ 1920, 1080 },
		{ 3840, 2160 },
		{ 5120, 2880 },
		{ 5688, 3200 },
		{ 7680, 4320 },
		{ 0, 0 }
	};

	/* no UX capsule support, so deleting var if it exists */
	if (fu_device_has_private_flag (device, FU_UEFI_DEVICE_FLAG_NO_UX_CAPSULE)) {
		g_debug ("not providing UX capsule");
		return fu_efivar_delete (FU_EFIVAR_GUID_FWUPDATE,
					    "fwupd-ux-capsule", error);
	}

	/* get the boot graphics resource table data */
	if (!fu_uefi_bgrt_get_supported (data->bgrt)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "BGRT is not supported");
		return FALSE;
	}
	if (!fu_uefi_get_framebuffer_size (&screen_width, &screen_height, error))
		return FALSE;
	g_debug ("framebuffer size %" G_GUINT32_FORMAT " x%" G_GUINT32_FORMAT,
		 screen_width, screen_height);

	/* find the 'best sized' pre-generated image */
	for (guint i = 0; sizes[i].width != 0; i++) {
		guint32 border_pixels;

		/* disregard any images that are bigger than the screen */
		if (sizes[i].width > screen_width)
			continue;
		if (sizes[i].height > screen_height)
			continue;

		/* is this the best fit for the display */
		border_pixels = (screen_width * screen_height) -
				(sizes[i].width * sizes[i].height);
		if (border_pixels < lowest_border_pixels) {
			lowest_border_pixels = border_pixels;
			best_idx = i;
		}
	}
	if (best_idx == G_MAXUINT) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to find a suitable image to use");
		return FALSE;
	}

	/* get the raw data */
	image_bmp = fu_plugin_uefi_capsule_get_splash_data (sizes[best_idx].width,
							    sizes[best_idx].height,
							    error);
	if (image_bmp == NULL)
		return FALSE;

	/* perform the upload */
	return fu_plugin_uefi_capsule_write_splash_data (plugin, device, image_bmp, error);
}

gboolean
fu_plugin_update (FuPlugin *plugin,
		  FuDevice *device,
		  GBytes *blob_fw,
		  FwupdInstallFlags flags,
		  GError **error)
{
	const gchar *str;
	guint32 flashes_left;
	g_autoptr(GError) error_splash = NULL;

	/* test the flash counter */
	flashes_left = fu_device_get_flashes_left (device);
	if (flashes_left > 0) {
		g_debug ("%s has %" G_GUINT32_FORMAT " flashes left",
			 fu_device_get_name (device),
			 flashes_left);
		if ((flags & FWUPD_INSTALL_FLAG_FORCE) == 0 && flashes_left <= 2) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "%s only has %" G_GUINT32_FORMAT " flashes left -- "
				     "see https://github.com/fwupd/fwupd/wiki/Dell-TPM:-flashes-left for more information.",
				     fu_device_get_name (device), flashes_left);
			return FALSE;
		}
	}

	/* TRANSLATORS: this is shown when updating the firmware after the reboot */
	str = _("Installing firmware update…");
	g_assert (str != NULL);

	/* perform the update */
	fu_device_set_status (device, FWUPD_STATUS_SCHEDULING);
	if (!fu_plugin_uefi_capsule_update_splash (plugin, device, &error_splash)) {
		g_debug ("failed to upload UEFI UX capsule text: %s",
			 error_splash->message);
	}

	return fu_device_write_firmware (device, blob_fw, flags, error);
}

static void
fu_plugin_uefi_capsule_load_config (FuPlugin *plugin, FuDevice *device)
{
	gboolean disable_shim;
	gboolean fallback_removable_path;
	gboolean use_grub;
	guint64 sz_reqd = FU_UEFI_COMMON_REQUIRED_ESP_FREE_SPACE;
	g_autofree gchar *require_esp_free_space = NULL;

	/* parse free space needed for ESP */
	require_esp_free_space = fu_plugin_get_config_value (plugin, "RequireESPFreeSpace");
	if (require_esp_free_space != NULL)
		sz_reqd = fu_common_strtoull (require_esp_free_space);
	fu_device_set_metadata_integer (device, "RequireESPFreeSpace", sz_reqd);

	/* shim used for SB or not? */
	disable_shim = fu_plugin_get_config_value_boolean (plugin, "DisableShimForSecureBoot");
	fu_device_set_metadata_boolean (device,
					"RequireShimForSecureBoot",
					!disable_shim);

	/* Use GRUB to load updates */
	use_grub = fu_plugin_get_config_value_boolean (plugin, "EnableGrubChainLoad");
	fu_device_set_metadata_boolean (device,
					"GrubChainLoad",
					use_grub);

	/* check if using UEFI removable path */
	fallback_removable_path = fu_plugin_get_config_value_boolean (plugin, "FallbacktoRemovablePath");
	fu_device_set_metadata_boolean (device,
					"FallbacktoRemovablePath",
					fallback_removable_path);
}

static void
fu_plugin_uefi_capsule_register_proxy_device (FuPlugin *plugin, FuDevice *device)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	g_autoptr(FuUefiDevice) dev = fu_uefi_device_new_from_dev (device);
	g_autoptr(GError) error_local = NULL;

	/* load all configuration variables */
	fu_plugin_uefi_capsule_load_config (plugin, FU_DEVICE (dev));
	if (data->esp == NULL)
		data->esp = fu_common_get_esp_default (&error_local);
	if (data->esp == NULL) {
		fu_device_set_update_error (FU_DEVICE (dev), error_local->message);
		fu_device_remove_flag (FU_DEVICE (dev), FWUPD_DEVICE_FLAG_UPDATABLE);
	} else {
		fu_uefi_device_set_esp (dev, data->esp);
	}
	fu_plugin_device_add (plugin, FU_DEVICE (dev));
}

void
fu_plugin_device_registered (FuPlugin *plugin, FuDevice *device)
{
	if (fu_device_get_metadata (device, FU_DEVICE_METADATA_UEFI_DEVICE_KIND) != NULL) {
		if (fu_device_get_guid_default (device) == NULL) {
			g_autofree gchar *dbg = fu_device_to_string (device);
			g_warning ("cannot create proxy device as no GUID: %s", dbg);
			return;
		}
		fu_plugin_uefi_capsule_register_proxy_device (plugin, device);
	}
}

static const gchar *
fu_plugin_uefi_capsule_uefi_type_to_string (FuUefiDeviceKind device_kind)
{
	if (device_kind == FU_UEFI_DEVICE_KIND_UNKNOWN)
		return "Unknown Firmware";
	if (device_kind == FU_UEFI_DEVICE_KIND_SYSTEM_FIRMWARE)
		return "System Firmware";
	if (device_kind == FU_UEFI_DEVICE_KIND_DEVICE_FIRMWARE)
		return "Device Firmware";
	if (device_kind == FU_UEFI_DEVICE_KIND_UEFI_DRIVER)
		return "UEFI Driver";
	if (device_kind == FU_UEFI_DEVICE_KIND_FMP)
		return "Firmware Management Protocol";
	return NULL;
}

static gchar *
fu_plugin_uefi_capsule_get_name_for_type (FuPlugin *plugin, FuUefiDeviceKind device_kind)
{
	GString *display_name;

	/* set Display Name prefix for capsules that are not PCI cards */
	display_name = g_string_new (fu_plugin_uefi_capsule_uefi_type_to_string (device_kind));
	if (device_kind == FU_UEFI_DEVICE_KIND_DEVICE_FIRMWARE)
		g_string_prepend (display_name, "UEFI ");
	return g_string_free (display_name, FALSE);
}

static gboolean
fu_plugin_uefi_capsule_coldplug_device (FuPlugin *plugin, FuUefiDevice *dev, GError **error)
{
	FuContext *ctx = fu_plugin_get_context (plugin);
	FuUefiDeviceKind device_kind;

	/* probe to get add GUIDs (and hence any quirk fixups) */
	if (!fu_device_probe (FU_DEVICE (dev), error))
		return FALSE;
	if (!fu_device_setup (FU_DEVICE (dev), error))
		return FALSE;

	/* if not already set by quirks */
	if (fu_plugin_has_custom_flag (plugin, "use-legacy-bootmgr-desc")) {
		fu_device_add_private_flag (FU_DEVICE (dev),
					    FU_UEFI_DEVICE_FLAG_USE_LEGACY_BOOTMGR_DESC);
	}

	/* set fallback name if nothing else is set */
	device_kind = fu_uefi_device_get_kind (dev);
	if (fu_device_get_name (FU_DEVICE (dev)) == NULL) {
		g_autofree gchar *name = NULL;
		name = fu_plugin_uefi_capsule_get_name_for_type (plugin, device_kind);
		if (name != NULL)
			fu_device_set_name (FU_DEVICE (dev), name);
		if (device_kind != FU_UEFI_DEVICE_KIND_SYSTEM_FIRMWARE) {
			fu_device_add_internal_flag (FU_DEVICE (dev),
						     FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME_CATEGORY);
		}
	}
	/* set fallback vendor if nothing else is set */
	if (fu_device_get_vendor (FU_DEVICE (dev)) == NULL &&
	    device_kind == FU_UEFI_DEVICE_KIND_SYSTEM_FIRMWARE) {
		const gchar *vendor = fu_context_get_hwid_value (ctx, FU_HWIDS_KEY_MANUFACTURER);
		if (vendor != NULL)
			fu_device_set_vendor (FU_DEVICE (dev), vendor);
	}

	/* set vendor ID as the BIOS vendor */
	if (device_kind != FU_UEFI_DEVICE_KIND_FMP) {
		const gchar *dmi_vendor;
		dmi_vendor = fu_context_get_hwid_value (ctx, FU_HWIDS_KEY_BIOS_VENDOR);
		if (dmi_vendor != NULL) {
			g_autofree gchar *vendor_id = g_strdup_printf ("DMI:%s", dmi_vendor);
			fu_device_add_vendor_id (FU_DEVICE (dev), vendor_id);
		}
	}

	/* success */
	return TRUE;
}

static void
fu_plugin_uefi_capsule_test_secure_boot (FuPlugin *plugin)
{
	const gchar *result_str = "Disabled";
	if (fu_efivar_secure_boot_enabled ())
		result_str = "Enabled";
	fu_plugin_add_report_metadata (plugin, "SecureBoot", result_str);
}

gboolean
fu_plugin_startup (FuPlugin *plugin, GError **error)
{
	FuPluginData *data = fu_plugin_get_data (plugin);
	guint64 nvram_total;
	g_autofree gchar *esp_path = NULL;
	g_autofree gchar *nvram_total_str = NULL;
	g_autoptr(GError) error_local = NULL;

	/* don't let user's environment influence test suite failures */
	if (g_getenv ("FWUPD_UEFI_TEST") != NULL)
		return TRUE;

	/* for the uploaded report */
	if (fu_plugin_has_custom_flag (plugin, "use-legacy-bootmgr-desc"))
		fu_plugin_add_report_metadata (plugin, "BootMgrDesc", "legacy");

	/* some platforms have broken SMBIOS data */
	if (fu_plugin_has_custom_flag (plugin, "uefi-force-enable"))
		return TRUE;

	/* check we can use this backend */
	if (!fu_backend_setup (data->backend, &error_local)) {
		if (g_error_matches (error_local, FWUPD_ERROR, FWUPD_ERROR_WRITE)) {
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_EFIVAR_NOT_MOUNTED);
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE);
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_USER_WARNING);
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}

	/* are the EFI dirs set up so we can update each device */
	if (!fu_efivar_supported (error))
		return FALSE;
	nvram_total = fu_efivar_space_used (error);
	if (nvram_total == G_MAXUINT64)
		return FALSE;
	nvram_total_str = g_strdup_printf ("%" G_GUINT64_FORMAT, nvram_total);
	fu_plugin_add_report_metadata (plugin, "EfivarNvramUsed", nvram_total_str);

	/* override the default ESP path */
	esp_path = fu_plugin_get_config_value (plugin, "OverrideESPMountPoint");
	if (esp_path != NULL) {
		data->esp = fu_common_get_esp_for_path (esp_path, error);
		if (data->esp == NULL) {
			g_prefix_error (error, "invalid OverrideESPMountPoint=%s "
					"specified in config: ", esp_path);
			return FALSE;
		}
	}

	/* test for invalid ESP in coldplug, and set the update-error rather
	 * than showing no output if the plugin had self-disabled here */
	return TRUE;
}

gboolean
fu_plugin_unlock (FuPlugin *plugin, FuDevice *device, GError **error)
{
	FuUefiDevice *device_uefi = FU_UEFI_DEVICE (device);
	FuDevice *device_alt = NULL;
	FwupdDeviceFlags device_flags_alt = 0;
	guint flashes_left = 0;
	guint flashes_left_alt = 0;

	if (fu_uefi_device_get_kind (device_uefi) !=
	    FU_UEFI_DEVICE_KIND_DELL_TPM_FIRMWARE) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Unable to unlock %s",
			     fu_device_get_name (device));
		return FALSE;
	}

	/* for unlocking TPM1.2 <-> TPM2.0 switching */
	g_debug ("Unlocking upgrades for: %s (%s)", fu_device_get_name (device),
		 fu_device_get_id (device));
	device_alt = fu_device_get_alternate (device);
	if (device_alt == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "No alternate device for %s",
			     fu_device_get_name (device));
		return FALSE;
	}
	g_debug ("Preventing upgrades for: %s (%s)", fu_device_get_name (device_alt),
		 fu_device_get_id (device_alt));

	flashes_left = fu_device_get_flashes_left (device);
	flashes_left_alt = fu_device_get_flashes_left (device_alt);
	if (flashes_left == 0) {
		/* flashes left == 0 on both means no flashes left */
		if (flashes_left_alt == 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "ERROR: %s has no flashes left.",
				     fu_device_get_name (device));
		/* flashes left == 0 on just unlocking device is ownership */
		} else {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "ERROR: %s is currently OWNED. "
				     "Ownership must be removed to switch modes.",
				     fu_device_get_name (device_alt));
		}
		return FALSE;
	}

	/* clone the info from real device but prevent it from being flashed */
	device_flags_alt = fu_device_get_flags (device_alt);
	fu_device_set_flags (device, device_flags_alt);
	fu_device_remove_flag (device_alt, FWUPD_DEVICE_FLAG_UPDATABLE);

	/* make sure that this unlocked device can be updated */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_QUAD);
	fu_device_set_version (device, "0.0.0.0");
	return TRUE;
}

static void
fu_plugin_uefi_update_state_notify_cb (GObject *object,
				       GParamSpec *pspec,
				       FuPlugin *plugin)
{
	FuDevice *device = FU_DEVICE (object);
	GPtrArray *devices;
	g_autofree gchar *msg = NULL;

	/* device is not in needs-reboot state */
	if (fu_device_get_update_state (device) != FWUPD_UPDATE_STATE_NEEDS_REBOOT)
		return;

	/* only do this on hardware that cannot coalesce multiple capsules */
	if (!fu_plugin_has_custom_flag (plugin, "no-coalesce"))
		return;

	/* mark every other device for this plugin as non-updatable */
	msg = g_strdup_printf ("Cannot update as %s [%s] needs reboot",
			       fu_device_get_name (device),
			       fu_device_get_id (device));
	devices = fu_plugin_get_devices (plugin);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device_tmp = g_ptr_array_index (devices, i);
		if (device_tmp == device)
			continue;
		fu_device_inhibit (device_tmp, "no-coalesce", msg);
	}
}

gboolean
fu_plugin_coldplug (FuPlugin *plugin, GError **error)
{
	FuContext *ctx = fu_plugin_get_context (plugin);
	FuPluginData *data = fu_plugin_get_data (plugin);
	const gchar *str;
	g_autoptr(GError) error_udisks2 = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	if (data->esp == NULL) {
		data->esp = fu_common_get_esp_default (&error_udisks2);
		if (data->esp == NULL) {
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_ESP_NOT_FOUND);
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE);
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_USER_WARNING);
			g_warning ("cannot find default ESP: %s", error_udisks2->message);
		}
	}

	/* add each device */
	if (!fu_backend_coldplug (data->backend, error))
		return FALSE;
	devices = fu_backend_get_devices (data->backend);
	for (guint i = 0; i < devices->len; i++) {
		FuUefiDevice *dev = g_ptr_array_index (devices, i);
		fu_device_set_context (FU_DEVICE (dev), ctx);
		if (data->esp != NULL)
			fu_uefi_device_set_esp (dev, data->esp);
		if (!fu_plugin_uefi_capsule_coldplug_device (plugin, dev, error))
			return FALSE;
		fu_device_add_flag (FU_DEVICE (dev), FWUPD_DEVICE_FLAG_UPDATABLE);
		fu_device_add_flag (FU_DEVICE (dev), FWUPD_DEVICE_FLAG_USABLE_DURING_UPDATE);

		/* load all configuration variables */
		fu_plugin_uefi_capsule_load_config (plugin, FU_DEVICE (dev));

		/* watch in case we set needs-reboot in the engine */
		g_signal_connect (dev, "notify::update-state",
				  G_CALLBACK (fu_plugin_uefi_update_state_notify_cb),
				  plugin);

		fu_plugin_device_add (plugin, FU_DEVICE (dev));
	}

	/* for debugging problems later */
	fu_plugin_uefi_capsule_test_secure_boot (plugin);
	if (!fu_uefi_bgrt_setup (data->bgrt, &error_local))
		g_debug ("BGRT setup failed: %s", error_local->message);
	str = fu_uefi_bgrt_get_supported (data->bgrt) ? "Enabled" : "Disabled";
	g_debug ("UX Capsule support : %s", str);
	fu_plugin_add_report_metadata (plugin, "UEFIUXCapsule", str);

	return TRUE;
}
