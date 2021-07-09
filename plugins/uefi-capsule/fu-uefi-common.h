/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2015 Peter Jones <pjones@redhat.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fwupdplugin.h>
#include <glib.h>

#define EFI_CAPSULE_HEADER_FLAGS_PERSIST_ACROSS_RESET	0x00010000
#define EFI_CAPSULE_HEADER_FLAGS_POPULATE_SYSTEM_TABLE	0x00020000
#define EFI_CAPSULE_HEADER_FLAGS_INITIATE_RESET		0x00040000

#define EFI_OS_INDICATIONS_FILE_CAPSULE_DELIVERY_SUPPORTED 0x0000000000000004ULL

typedef struct __attribute__((__packed__)) {
	guint16		 year;
	guint8		 month;
	guint8		 day;
	guint8		 hour;
	guint8		 minute;
	guint8		 second;
	guint8		 pad1;
	guint32		 nanosecond;
	guint16		 timezone;
	guint8		 daylight;
	guint8		 pad2;
} efi_time_t;

typedef struct __attribute__((__packed__)) {
	fwupd_guid_t	 guid;
	guint32		 header_size;
	guint32		 flags;
	guint32		 capsule_image_size;
} efi_capsule_header_t;

typedef struct __attribute__((__packed__)) {
	guint8		 version;
	guint8		 checksum;
	guint8		 image_type;
	guint8		 reserved;
	guint32		 mode;
	guint32		 x_offset;
	guint32		 y_offset;
} efi_ux_capsule_header_t;

typedef struct __attribute__((__packed__)) {
	guint32		 update_info_version;
	fwupd_guid_t	 guid;
	guint32		 capsule_flags;
	guint64		 hw_inst;
	efi_time_t	 time_attempted;
	guint32		 status;
} efi_update_info_t;

/* the biggest size SPI part currently seen */
#define FU_UEFI_COMMON_REQUIRED_ESP_FREE_SPACE		(32 * 1024 * 1024)

gchar		*fu_uefi_get_esp_app_path	(FuDevice       *device,
						 const gchar	*esp_path,
						 const gchar	*cmd,
						 GError		**error);
gchar		*fu_uefi_get_built_app_path	(GError		**error);
gboolean	 fu_uefi_get_bitmap_size	(const guint8	*buf,
						 gsize		 bufsz,
						 guint32	*width,
						 guint32	*height,
						 GError		**error);
gboolean	 fu_uefi_get_framebuffer_size	(guint32	*width,
						 guint32	*height,
						 GError		**error);
gchar		*fu_uefi_get_esp_path_for_os	(FuDevice 	*device,
						 const gchar	*esp_path);
guint64		 fu_uefi_read_file_as_uint64	(const gchar	*path,
						 const gchar	*attr_name);
