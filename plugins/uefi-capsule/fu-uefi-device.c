/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2015 Peter Jones <pjones@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>
#include <efivar.h>
#include <efivar/efiboot.h>
#include <fwupdplugin.h>

#include "fu-uefi-common.h"
#include "fu-uefi-device.h"
#include "fu-uefi-devpath.h"
#include "fu-uefi-bootmgr.h"
#include "fu-uefi-pcrs.h"

struct _FuUefiDevice {
	FuDevice		 parent_instance;
	FuVolume		*esp;
	FuDeviceLocker		*esp_locker;
	gchar			*fw_class;
	FuUefiDeviceKind	 kind;
	guint32			 capsule_flags;
	guint32			 fw_version;
	guint32			 fw_version_lowest;
	FuUefiDeviceStatus	 last_attempt_status;
	guint32			 last_attempt_version;
	guint64			 fmp_hardware_instance;
	gboolean		 missing_header;
	gboolean		 automounted_esp;
	gboolean		 requires_header;
};

G_DEFINE_TYPE (FuUefiDevice, fu_uefi_device, FU_TYPE_DEVICE)

enum {
	PROP_0,
	PROP_FW_CLASS,
	PROP_KIND,
	PROP_CAPSULE_FLAGS,
	PROP_FW_VERSION,
	PROP_FW_VERSION_LOWEST,
	PROP_LAST_ATTEMPT_STATUS,
	PROP_LAST_ATTEMPT_VERSION,
	PROP_FMP_HARDWARE_INSTANCE,
	PROP_LAST
};

void
fu_uefi_device_set_esp (FuUefiDevice *self, FuVolume *esp)
{
	g_return_if_fail (FU_IS_UEFI_DEVICE (self));
	g_return_if_fail (FU_IS_VOLUME (esp));
	g_set_object (&self->esp, esp);
}

const gchar *
fu_uefi_device_kind_to_string (FuUefiDeviceKind kind)
{
	if (kind == FU_UEFI_DEVICE_KIND_UNKNOWN)
		return "unknown";
	if (kind == FU_UEFI_DEVICE_KIND_SYSTEM_FIRMWARE)
		return "system-firmware";
	if (kind == FU_UEFI_DEVICE_KIND_DEVICE_FIRMWARE)
		return "device-firmware";
	if (kind == FU_UEFI_DEVICE_KIND_UEFI_DRIVER)
		return "uefi-driver";
	if (kind == FU_UEFI_DEVICE_KIND_FMP)
		return "fmp";
	if (kind == FU_UEFI_DEVICE_KIND_DELL_TPM_FIRMWARE)
		return "dell-tpm-firmware";
	return NULL;
}

static FuUefiDeviceKind
fu_uefi_device_kind_from_string (const gchar *kind)
{
	if (g_strcmp0 (kind, "system-firmware") == 0)
		return FU_UEFI_DEVICE_KIND_SYSTEM_FIRMWARE;
	if (g_strcmp0 (kind, "device-firmware") == 0)
		return FU_UEFI_DEVICE_KIND_DEVICE_FIRMWARE;
	if (g_strcmp0 (kind, "uefi-driver") == 0)
		return FU_UEFI_DEVICE_KIND_UEFI_DRIVER;
	if (g_strcmp0 (kind, "fmp") == 0)
		return FU_UEFI_DEVICE_KIND_FMP;
	if (g_strcmp0 (kind, "dell-tpm-firmware") == 0)
		return FU_UEFI_DEVICE_KIND_DELL_TPM_FIRMWARE;
	return FU_UEFI_DEVICE_KIND_UNKNOWN;
}

const gchar *
fu_uefi_device_status_to_string (FuUefiDeviceStatus status)
{
	if (status == FU_UEFI_DEVICE_STATUS_SUCCESS)
		return "success";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_UNSUCCESSFUL)
		return "unsuccessful";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_INSUFFICIENT_RESOURCES)
		return "insufficient resources";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_INCORRECT_VERSION)
		return "incorrect version";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_INVALID_FORMAT)
		return "invalid firmware format";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_AUTH_ERROR)
		return "authentication signing error";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_PWR_EVT_AC)
		return "AC power required";
	if (status == FU_UEFI_DEVICE_STATUS_ERROR_PWR_EVT_BATT)
		return "battery level is too low";
	return NULL;
}

static void
fu_uefi_device_to_string (FuDevice *device, guint idt, GString *str)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);
	fu_common_string_append_kv (str, idt, "Kind", fu_uefi_device_kind_to_string (self->kind));
	fu_common_string_append_kv (str, idt, "FwClass", self->fw_class);
	fu_common_string_append_kx (str, idt, "CapsuleFlags", self->capsule_flags);
	fu_common_string_append_kx (str, idt, "FwVersion", self->fw_version);
	fu_common_string_append_kx (str, idt, "FwVersionLowest", self->fw_version_lowest);
	fu_common_string_append_kv (str, idt, "LastAttemptStatus",
				    fu_uefi_device_status_to_string (self->last_attempt_status));
	fu_common_string_append_kx (str, idt, "LastAttemptVersion", self->last_attempt_version);
	if (self->esp != NULL) {
		fu_common_string_append_kv (str, idt, "EspId",
					    fu_volume_get_id (self->esp));
	}
	fu_common_string_append_ku (str, idt, "RequireESPFreeSpace",
				    fu_device_get_metadata_integer (device, "RequireESPFreeSpace"));
	fu_common_string_append_kb (str, idt, "RequireShimForSecureBoot",
				    fu_device_get_metadata_boolean (device, "RequireShimForSecureBoot"));
	fu_common_string_append_kb (str, idt, "GrubChainLoad",
				    fu_device_get_metadata_boolean (device, "GrubChainLoad"));

}

static void
fu_uefi_device_report_metadata_pre (FuDevice *device, GHashTable *metadata)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);

	/* record if we had an invalid header during update */
	g_hash_table_insert (metadata,
			     g_strdup ("MissingCapsuleHeader"),
			     g_strdup (self->missing_header ? "True" : "False"));

	/* where the ESP was mounted during installation */
	g_hash_table_insert (metadata,
			     g_strdup ("EspPath"),
			     fu_volume_get_mount_point (self->esp));
}

static void
fu_uefi_device_report_metadata_post (FuDevice *device, GHashTable *metadata)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);

	/* the actual last_attempt values */
	g_hash_table_insert (metadata,
			     g_strdup ("LastAttemptStatus"),
			     g_strdup_printf ("0x%x", self->last_attempt_status));
	g_hash_table_insert (metadata,
			     g_strdup ("LastAttemptVersion"),
			     g_strdup_printf ("0x%x", self->last_attempt_version));
}

FuUefiDeviceKind
fu_uefi_device_get_kind (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0);
	return self->kind;
}

guint32
fu_uefi_device_get_version (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0x0);
	return self->fw_version;
}

guint32
fu_uefi_device_get_version_lowest (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0x0);
	return self->fw_version_lowest;
}

guint32
fu_uefi_device_get_version_error (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0x0);
	return self->last_attempt_version;
}

guint64
fu_uefi_device_get_hardware_instance (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0x0);
	return self->fmp_hardware_instance;
}

FuUefiDeviceStatus
fu_uefi_device_get_status (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0);
	return self->last_attempt_status;
}

guint32
fu_uefi_device_get_capsule_flags (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), 0x0);
	return self->capsule_flags;
}

const gchar *
fu_uefi_device_get_guid (FuUefiDevice *self)
{
	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), NULL);
	return self->fw_class;
}

static gchar *
fu_uefi_device_build_varname (FuUefiDevice *self)
{
	return g_strdup_printf ("fwupd-%s-%"G_GUINT64_FORMAT,
				self->fw_class,
				self->fmp_hardware_instance);
}

FuUefiUpdateInfo *
fu_uefi_device_load_update_info (FuUefiDevice *self, GError **error)
{
	gsize datasz = 0;
	g_autofree gchar *varname = fu_uefi_device_build_varname (self);
	g_autofree guint8 *data = NULL;
	g_autoptr(FuUefiUpdateInfo) info = fu_uefi_update_info_new ();

	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* get the existing status */
	if (!fu_efivar_get_data (FU_EFIVAR_GUID_FWUPDATE, varname,
				    &data, &datasz, NULL, error))
		return NULL;
	if (!fu_uefi_update_info_parse (info, data, datasz, error))
		return NULL;
	return g_steal_pointer (&info);
}

gboolean
fu_uefi_device_clear_status (FuUefiDevice *self, GError **error)
{
	efi_update_info_t info;
	gsize datasz = 0;
	g_autofree gchar *varname = fu_uefi_device_build_varname (self);
	g_autofree guint8 *data = NULL;

	g_return_val_if_fail (FU_IS_UEFI_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* get the existing status */
	if (!fu_efivar_get_data (FU_EFIVAR_GUID_FWUPDATE, varname,
				    &data, &datasz, NULL, error))
		return FALSE;
	if (datasz < sizeof(efi_update_info_t)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "EFI variable is corrupt");
		return FALSE;
	}

	/* just copy the efi_update_info_t, ignore devpath then save it back */
	memcpy (&info, data, sizeof(info));
	info.status = FU_UEFI_DEVICE_STATUS_SUCCESS;
	memcpy (data, &info, sizeof(info));
	return fu_efivar_set_data (FU_EFIVAR_GUID_FWUPDATE, varname,
				      data, datasz,
				      FU_EFIVAR_ATTR_NON_VOLATILE |
				      FU_EFIVAR_ATTR_BOOTSERVICE_ACCESS |
				      FU_EFIVAR_ATTR_RUNTIME_ACCESS,
				      error);
}

static guint8 *
fu_uefi_device_build_dp_buf (const gchar *path, gsize *bufsz, GError **error)
{
	gssize req;
	gssize sz;
	g_autofree guint8 *dp_buf = NULL;
	g_autoptr(GPtrArray) dps = NULL;

	/* get the size of the path first */
	req = efi_generate_file_device_path (NULL, 0, path,
					     EFIBOOT_OPTIONS_IGNORE_FS_ERROR |
					     EFIBOOT_ABBREV_HD);
	if (req < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to efi_generate_file_device_path(%s)",
			     path);
		return NULL;
	}

	/* if we just have an end device path, it's not going to work */
	if (req <= 4) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to get valid device_path for (%s)",
			     path);
		return NULL;
	}

	/* actually get the path this time */
	dp_buf = g_malloc0 (req);
	sz = efi_generate_file_device_path (dp_buf, req, path,
					    EFIBOOT_OPTIONS_IGNORE_FS_ERROR |
					    EFIBOOT_ABBREV_HD);
	if (sz < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to efi_generate_file_device_path(%s)",
			     path);
		return NULL;
	}

	/* parse what we got back from efivar */
	dps = fu_uefi_devpath_parse (dp_buf, (gsize) sz,
				     FU_UEFI_DEVPATH_PARSE_FLAG_NONE, error);
	if (dps == NULL) {
		fu_common_dump_raw (G_LOG_DOMAIN, "dp_buf", dp_buf, (gsize) sz);
		return NULL;
	}

	/* success */
	if (bufsz != NULL)
		*bufsz = sz;
	return g_steal_pointer (&dp_buf);
}

static GBytes *
fu_uefi_device_fixup_firmware (FuDevice *device, GBytes *fw, GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);
	gsize fw_length;
	const guint8 *data = g_bytes_get_data (fw, &fw_length);
	g_autofree gchar *guid_new = NULL;

	self->missing_header = FALSE;

	/* GUID is the first 16 bytes */
	if (fw_length < sizeof(fwupd_guid_t)) {
		g_set_error_literal (error, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE,
				     "Invalid payload");
		return NULL;
	}
	guid_new = fwupd_guid_to_string ((fwupd_guid_t *) data, FWUPD_GUID_FLAG_MIXED_ENDIAN);

	/* ESRT header matches payload */
	if (g_strcmp0 (fu_uefi_device_get_guid (self), guid_new) == 0) {
		g_debug ("ESRT matches payload GUID");
		return g_bytes_ref (fw);
	/* Type that doesn't require a header */
	} else if (!self->requires_header) {
		return g_bytes_ref (fw);
	/* Missing, add a header */
	} else {
		guint header_size = getpagesize();
		guint8 *new_data = g_malloc (fw_length + header_size);
		guint8 *capsule = new_data + header_size;
		fwupd_guid_t esrt_guid = { 0x0 };
		efi_capsule_header_t *header = (efi_capsule_header_t *) new_data;

		g_warning ("missing or invalid embedded capsule header");
		self->missing_header = TRUE;
		header->flags = self->capsule_flags;
		header->header_size = header_size;
		header->capsule_image_size = fw_length + header_size;
		if (!fwupd_guid_from_string (fu_uefi_device_get_guid (self), &esrt_guid,
					     FWUPD_GUID_FLAG_MIXED_ENDIAN, error)) {
			g_prefix_error (error, "Invalid ESRT GUID: ");
			return NULL;
		}
		memcpy (&header->guid, &esrt_guid, sizeof (fwupd_guid_t));
		memcpy (capsule, data, fw_length);

		return g_bytes_new_take (new_data, fw_length + header_size);
	}
}

gboolean
fu_uefi_device_write_update_info (FuUefiDevice *self,
				  const gchar *filename,
				  const gchar *varname,
				  const gchar *guid,
				  GError **error)
{
	gsize datasz = 0;
	gsize dp_bufsz = 0;
	g_autofree guint8 *data = NULL;
	g_autofree guint8 *dp_buf = NULL;
	efi_update_info_t info = {
		.update_info_version	= 0x7,
		.guid			= { 0x0 },
		.capsule_flags		= self->capsule_flags,
		.hw_inst		= self->fmp_hardware_instance,
		.time_attempted		= { 0x0 },
		.status			= FU_UEFI_UPDATE_INFO_STATUS_ATTEMPT_UPDATE,
	};

	/* set the body as the device path */
	if (g_getenv ("FWUPD_UEFI_TEST") != NULL) {
		g_debug ("not building device path, in tests....");
		return TRUE;
	}

	/* convert to EFI device path */
	dp_buf = fu_uefi_device_build_dp_buf (filename, &dp_bufsz, error);
	if (dp_buf == NULL)
		return FALSE;

	/* save this header and body to the hardware */
	if (!fwupd_guid_from_string (guid, &info.guid, FWUPD_GUID_FLAG_MIXED_ENDIAN, error))
		return FALSE;
	datasz = sizeof(info) + dp_bufsz;
	data = g_malloc0 (datasz);
	memcpy (data, &info, sizeof(info));
	memcpy (data + sizeof(info), dp_buf, dp_bufsz);
	return fu_efivar_set_data (FU_EFIVAR_GUID_FWUPDATE, varname,
				   data, datasz,
				   FU_EFIVAR_ATTR_NON_VOLATILE |
				   FU_EFIVAR_ATTR_BOOTSERVICE_ACCESS |
				   FU_EFIVAR_ATTR_RUNTIME_ACCESS,
				   error);
}

static gboolean
fu_uefi_device_check_esp_free (FuDevice *device, GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);
	guint64 sz_reqd = fu_device_get_metadata_integer (device, "RequireESPFreeSpace");
	if (sz_reqd == G_MAXUINT) {
		g_debug ("maximum size is not configured");
		return TRUE;
	}
	return fu_volume_check_free_space (self->esp, sz_reqd, error);
}

static gboolean
fu_uefi_check_asset (FuDevice *device, GError **error)
{
	g_autofree gchar *source_app = fu_uefi_get_built_app_path (error);
	if (source_app == NULL) {
		if (fu_efivar_secure_boot_enabled ())
			g_prefix_error (error, "missing signed bootloader for secure boot: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_uefi_device_cleanup_esp (FuDevice *device, GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);
	g_autofree gchar *esp_path = fu_volume_get_mount_point (self->esp);
	g_autofree gchar *pattern = NULL;
	g_autoptr(GPtrArray) files = NULL;

	/* in case we call capsule install twice before reboot */
	if (fu_efivar_exists (FU_EFIVAR_GUID_EFI_GLOBAL, "BootNext"))
		return TRUE;

	/* delete any files matching the glob in the ESP */
	files = fu_common_get_files_recursive (esp_path, error);
	if (files == NULL)
		return FALSE;
	pattern = g_build_filename (esp_path, "EFI/*/fw/fwupd*.cap", NULL);
	for (guint i = 0; i < files->len; i++) {
		const gchar *fn = g_ptr_array_index (files, i);
		if (fu_common_fnmatch (pattern, fn)) {
			g_autoptr(GFile) file = g_file_new_for_path (fn);
			g_debug ("deleting %s", fn);
			if (!g_file_delete (file, NULL, error))
				return FALSE;
		}
	}

	/* delete any old variables */
	if (!fu_efivar_delete_with_glob (FU_EFIVAR_GUID_FWUPDATE, "fwupd*-*", error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_uefi_device_prepare (FuDevice *device,
			FwupdInstallFlags flags,
			GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);

	/* mount if required */
	self->esp_locker = fu_volume_locker (self->esp, error);
	if (self->esp_locker == NULL)
		return FALSE;

	/* sanity checks */
	if (!fu_uefi_device_cleanup_esp (device, error))
		return FALSE;
	if (!fu_uefi_device_check_esp_free (device, error))
		return FALSE;
	if (!fu_uefi_check_asset (device, error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_uefi_device_cleanup (FuDevice *device,
			FwupdInstallFlags flags,
			GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);

	/* unmount ESP if we opened it */
	if (!fu_device_locker_close (self->esp_locker, error))
		return FALSE;
	g_clear_object (&self->esp_locker);

	return TRUE;
}

static gboolean
fu_uefi_device_write_firmware (FuDevice *device,
			       FuFirmware *firmware,
			       FwupdInstallFlags install_flags,
			       GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);
	FuUefiBootmgrFlags flags = FU_UEFI_BOOTMGR_FLAG_NONE;
	const gchar *bootmgr_desc = "Linux Firmware Updater";
	g_autofree gchar *esp_path = fu_volume_get_mount_point (self->esp);
	g_autoptr(GBytes) fixed_fw = NULL;
	g_autoptr(GBytes) fw = NULL;
	g_autofree gchar *basename = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *varname = fu_uefi_device_build_varname (self);

	/* ensure we have the existing state */
	if (self->fw_class == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "cannot update device info with no GUID");
		return FALSE;
	}

	/* get default image */
	fw = fu_firmware_get_bytes (firmware, error);
	if (fw == NULL)
		return FALSE;

	/* save the blob to the ESP */
	directory = fu_uefi_get_esp_path_for_os (device, esp_path);
	basename = g_strdup_printf ("fwupd-%s.cap", self->fw_class);
	fn = g_build_filename (directory, "fw", basename, NULL);
	if (!fu_common_mkdir_parent (fn, error))
		return FALSE;
	fixed_fw = fu_uefi_device_fixup_firmware (device, fw, error);
	if (fixed_fw == NULL)
		return FALSE;
	if (!fu_common_set_contents_bytes (fn, fixed_fw, error))
		return FALSE;

	/* delete the logs to save space; use fwupdate to debug the EFI binary */
	if (fu_efivar_exists (FU_EFIVAR_GUID_FWUPDATE, "FWUPDATE_VERBOSE")) {
		if (!fu_efivar_delete (FU_EFIVAR_GUID_FWUPDATE,
				       "FWUPDATE_VERBOSE", error))
			return FALSE;
	}
	if (fu_efivar_exists (FU_EFIVAR_GUID_FWUPDATE, "FWUPDATE_DEBUG_LOG")) {
		if (!fu_efivar_delete (FU_EFIVAR_GUID_FWUPDATE,
				       "FWUPDATE_DEBUG_LOG", error))
			return FALSE;
	}

	/* set the blob header shared with fwupd.efi */
	if (!fu_uefi_device_write_update_info (self, fn, varname, self->fw_class, error))
		return FALSE;

	/* update the firmware before the bootloader runs */
	if (fu_device_get_metadata_boolean (device, "RequireShimForSecureBoot"))
		flags |= FU_UEFI_BOOTMGR_FLAG_USE_SHIM_FOR_SB;
	if (fu_device_has_private_flag (device, FU_UEFI_DEVICE_FLAG_USE_SHIM_UNIQUE))
		flags |= FU_UEFI_BOOTMGR_FLAG_USE_SHIM_UNIQUE;
	if (fu_device_get_metadata_boolean (device, "GrubChainLoad")) {
		if (flags)
			g_debug ("Grub chainloading overrode all shim configuration");
		flags = FU_UEFI_BOOTMGR_FLAG_GRUB_CHAINLOAD;
	}

	/* some legacy devices use the old name to deduplicate boot entries */
	if (fu_device_has_private_flag (device, FU_UEFI_DEVICE_FLAG_USE_LEGACY_BOOTMGR_DESC))
		bootmgr_desc = "Linux-Firmware-Updater";
	if (!fu_uefi_bootmgr_bootnext (device, esp_path, bootmgr_desc, flags, error))
		return FALSE;

	/* success! */
	return TRUE;
}

static gboolean
fu_uefi_device_add_system_checksum (FuDevice *device, GError **error)
{
	g_autoptr(FuUefiPcrs) pcrs = fu_uefi_pcrs_new ();
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) pcr0s = NULL;

	/* get all the PCRs */
	if (!fu_uefi_pcrs_setup (pcrs, &error_local)) {
		if (g_error_matches (error_local,
				     G_IO_ERROR,
				     G_IO_ERROR_NOT_SUPPORTED) ||
		    g_error_matches (error_local,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_FOUND)) {
			g_debug ("%s", error_local->message);
			return TRUE;
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}

	/* get all the PCR0s */
	pcr0s = fu_uefi_pcrs_get_checksums (pcrs, 0);
	if (pcr0s->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "no PCR0s detected");
		return FALSE;
	}
	for (guint i = 0; i < pcr0s->len; i++) {
		const gchar *checksum = g_ptr_array_index (pcr0s, i);
		fu_device_add_checksum (device, checksum);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_uefi_device_probe (FuDevice *device, GError **error)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (device);
	FwupdVersionFormat version_format;
	g_autofree gchar *devid = NULL;
	g_autofree gchar *guid_strup = NULL;
	g_autofree gchar *version_lowest = NULL;
	g_autofree gchar *version = NULL;

	/* broken sysfs? */
	if (self->fw_class == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "failed to read fw_class");
		return FALSE;
	}

	/* this is invalid */
	if (!fwupd_guid_is_valid (self->fw_class)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "ESRT GUID '%s' was not valid", self->fw_class);
		return FALSE;
	}

	/* add GUID first, as quirks may set the version format */
	fu_device_add_guid (device, self->fw_class);

	/* set versions */
	version_format = fu_device_get_version_format (device);
	version = fu_common_version_from_uint32 (self->fw_version, version_format);
	fu_device_set_version_format (device, version_format);
	fu_device_set_version_raw (device, self->fw_version);
	fu_device_set_version (device, version);
	if (self->fw_version_lowest != 0) {
		version_lowest = fu_common_version_from_uint32 (self->fw_version_lowest,
							        version_format);
		fu_device_set_version_lowest_raw (device, self->fw_version_lowest);
		fu_device_set_version_lowest (device, version_lowest);
	}

	/* set flags */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_REQUIRE_AC);
	fu_device_add_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_VERFMT);
	fu_device_add_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON);

	/* add icons */
	if (self->kind == FU_UEFI_DEVICE_KIND_DEVICE_FIRMWARE) {
		/* nothing better in the icon naming spec */
		fu_device_add_icon (device, "audio-card");
	} else {
		/* this is probably system firmware */
		fu_device_add_icon (device, "computer");
		fu_device_add_instance_id (device, "main-system-firmware");
	}

	/* set the PCR0 as the device checksum */
	if (self->kind == FU_UEFI_DEVICE_KIND_SYSTEM_FIRMWARE) {
		g_autoptr(GError) error_local = NULL;
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_CAN_VERIFY);
		if (!fu_uefi_device_add_system_checksum (device, &error_local))
			g_warning ("Failed to get PCR0s: %s", error_local->message);
	}

	/* whether to create a missing header */
	if (self->kind == FU_UEFI_DEVICE_KIND_FMP ||
	    self->kind == FU_UEFI_DEVICE_KIND_DELL_TPM_FIRMWARE)
		self->requires_header = FALSE;
	else
		self->requires_header = TRUE;

	/* Windows seems to be case insensitive, but for convenience we'll
	 * match the upper case values typically specified in the .inf file */
	guid_strup = g_ascii_strup (self->fw_class, -1);
	devid = g_strdup_printf ("UEFI\\RES_{%s}", guid_strup);
	fu_device_add_instance_id (device, devid);
	return TRUE;
}

static void
fu_uefi_device_set_property (GObject *object, guint prop_id,
			     const GValue *value, GParamSpec *pspec)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (object);
	switch (prop_id) {
	case PROP_FW_CLASS:
		self->fw_class = g_value_dup_string (value);
		break;
	case PROP_KIND:
		self->kind = g_value_get_uint (value);
		break;
	case PROP_CAPSULE_FLAGS:
		self->capsule_flags = g_value_get_uint (value);
		break;
	case PROP_FW_VERSION:
		self->fw_version = g_value_get_uint (value);
		break;
	case PROP_FW_VERSION_LOWEST:
		self->fw_version_lowest = g_value_get_uint (value);
		break;
	case PROP_LAST_ATTEMPT_STATUS:
		self->last_attempt_status = g_value_get_uint (value);
		break;
	case PROP_LAST_ATTEMPT_VERSION:
		self->last_attempt_version = g_value_get_uint (value);
		break;
	case PROP_FMP_HARDWARE_INSTANCE:
		self->fmp_hardware_instance = g_value_get_uint64 (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fu_uefi_device_init (FuUefiDevice *self)
{
	fu_device_add_protocol (FU_DEVICE (self), "org.uefi.capsule");
	fu_device_register_private_flag (FU_DEVICE (self),
					 FU_UEFI_DEVICE_FLAG_NO_UX_CAPSULE,
					 "no-ux-capsule");
	fu_device_register_private_flag (FU_DEVICE (self),
					 FU_UEFI_DEVICE_FLAG_USE_SHIM_UNIQUE,
					 "use-shim-unique");
	fu_device_register_private_flag (FU_DEVICE (self),
					 FU_UEFI_DEVICE_FLAG_USE_LEGACY_BOOTMGR_DESC,
					 "use-legacy-bootmgr-desc");
}

static void
fu_uefi_device_finalize (GObject *object)
{
	FuUefiDevice *self = FU_UEFI_DEVICE (object);

	g_free (self->fw_class);
	if (self->esp != NULL)
		g_object_unref (self->esp);
	if (self->esp_locker != NULL)
		g_object_unref (self->esp_locker);

	G_OBJECT_CLASS (fu_uefi_device_parent_class)->finalize (object);
}

static void
fu_uefi_device_class_init (FuUefiDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GParamSpec *pspec;
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);

	object_class->set_property = fu_uefi_device_set_property;
	object_class->finalize = fu_uefi_device_finalize;
	klass_device->to_string = fu_uefi_device_to_string;
	klass_device->probe = fu_uefi_device_probe;
	klass_device->prepare = fu_uefi_device_prepare;
	klass_device->write_firmware = fu_uefi_device_write_firmware;
	klass_device->cleanup = fu_uefi_device_cleanup;
	klass_device->report_metadata_pre = fu_uefi_device_report_metadata_pre;
	klass_device->report_metadata_post = fu_uefi_device_report_metadata_post;

	pspec = g_param_spec_string ("fw-class", NULL, NULL, NULL,
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_WRITABLE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_FW_CLASS, pspec);
	pspec = g_param_spec_uint ("kind", NULL, NULL,
				   FU_UEFI_DEVICE_KIND_UNKNOWN,
				   FU_UEFI_DEVICE_KIND_LAST - 1,
				   FU_UEFI_DEVICE_KIND_UNKNOWN,
				   G_PARAM_CONSTRUCT_ONLY |
				   G_PARAM_WRITABLE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_KIND, pspec);
	pspec = g_param_spec_uint ("capsule-flags", NULL, NULL,
				   0, G_MAXUINT32, 0,
				   G_PARAM_CONSTRUCT_ONLY |
				   G_PARAM_WRITABLE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_CAPSULE_FLAGS, pspec);
	pspec = g_param_spec_uint ("fw-version", NULL, NULL,
				   0, G_MAXUINT32, 0,
				   G_PARAM_CONSTRUCT_ONLY |
				   G_PARAM_WRITABLE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_FW_VERSION, pspec);
	pspec = g_param_spec_uint ("fw-version-lowest", NULL, NULL,
				   0, G_MAXUINT32, 0,
				   G_PARAM_CONSTRUCT_ONLY |
				   G_PARAM_WRITABLE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_FW_VERSION_LOWEST, pspec);
	pspec = g_param_spec_uint ("last-attempt-status", NULL, NULL,
				   FU_UEFI_DEVICE_STATUS_SUCCESS,
				   FU_UEFI_DEVICE_STATUS_LAST - 1,
				   FU_UEFI_DEVICE_STATUS_SUCCESS,
				   G_PARAM_CONSTRUCT_ONLY |
				   G_PARAM_WRITABLE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_LAST_ATTEMPT_STATUS, pspec);
	pspec = g_param_spec_uint ("last-attempt-version", NULL, NULL,
				   0, G_MAXUINT32, 0,
				   G_PARAM_CONSTRUCT_ONLY |
				   G_PARAM_WRITABLE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_LAST_ATTEMPT_VERSION, pspec);
	pspec = g_param_spec_uint64 ("fmp-hardware-instance", NULL, NULL,
				     0, G_MAXUINT64, 0,
				     G_PARAM_CONSTRUCT_ONLY |
				     G_PARAM_WRITABLE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_FMP_HARDWARE_INSTANCE, pspec);
}

FuUefiDevice *
fu_uefi_device_new_from_dev (FuDevice *dev)
{
	const gchar *tmp;
	FuUefiDevice *self;

	g_return_val_if_fail (fu_device_get_guid_default (dev) != NULL, NULL);

	/* create virtual object not backed by an ESRT entry */
	self = g_object_new (FU_TYPE_UEFI_DEVICE, NULL);
	fu_device_incorporate (FU_DEVICE (self), dev);
	self->fw_class = g_strdup (fu_device_get_guid_default (dev));
	tmp = fu_device_get_metadata (dev, FU_DEVICE_METADATA_UEFI_DEVICE_KIND);
	self->kind = fu_uefi_device_kind_from_string (tmp);
	self->capsule_flags = fu_device_get_metadata_integer (dev, FU_DEVICE_METADATA_UEFI_CAPSULE_FLAGS);
	self->fw_version = fu_device_get_metadata_integer (dev, FU_DEVICE_METADATA_UEFI_FW_VERSION);
	g_assert (self->fw_class != NULL);
	return self;
}

FuUefiDevice *
fu_uefi_device_new_from_guid (const gchar *guid)
{
	FuUefiDevice *self;
	self = g_object_new (FU_TYPE_UEFI_DEVICE, NULL);
	self->fw_class = g_strdup (guid);
	fu_device_set_version_format (FU_DEVICE (self), FWUPD_VERSION_FORMAT_NUMBER);
	return self;
}
