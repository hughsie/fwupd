/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib-object.h>

#include "fwupd-enums.h"
#include "fwupd-release.h"

G_BEGIN_DECLS

#define FWUPD_TYPE_DEVICE (fwupd_device_get_type ())
G_DECLARE_DERIVABLE_TYPE (FwupdDevice, fwupd_device, FWUPD, DEVICE, GObject)

struct _FwupdDeviceClass
{
	GObjectClass			 parent_class;
	/*< private >*/
	void (*_fwupd_reserved1)	(void);
	void (*_fwupd_reserved2)	(void);
	void (*_fwupd_reserved3)	(void);
	void (*_fwupd_reserved4)	(void);
	void (*_fwupd_reserved5)	(void);
	void (*_fwupd_reserved6)	(void);
	void (*_fwupd_reserved7)	(void);
};

/**
 * FwupdDeviceMessageKind:
 * @FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN:		Unknown kind
 * @FWUPD_DEVICE_MESSAGE_KIND_POST:		After the update
 * @FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE:	Immediately
 *
 * The kind of message we show the user.
 **/
typedef enum {
	FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN,		/* Since: 1.6.2 */
	FWUPD_DEVICE_MESSAGE_KIND_POST,			/* Since: 1.6.2 */
	FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE,		/* Since: 1.6.2 */
	/*< private >*/
	FWUPD_DEVICE_MESSAGE_KIND_LAST
} FwupdDeviceMessageKind;

const gchar	*fwupd_device_message_kind_to_string		(FwupdDeviceMessageKind	 update_message_kind);
FwupdDeviceMessageKind fwupd_device_message_kind_from_string	(const gchar	*update_message_kind);

FwupdDevice	*fwupd_device_new			(void);
gchar		*fwupd_device_to_string			(FwupdDevice	*self);

const gchar	*fwupd_device_get_id			(FwupdDevice	*self);
void		 fwupd_device_set_id			(FwupdDevice	*self,
							 const gchar	*id);
const gchar	*fwupd_device_get_parent_id		(FwupdDevice	*self);
void		 fwupd_device_set_parent_id		(FwupdDevice	*self,
							 const gchar	*parent_id);
const gchar	*fwupd_device_get_composite_id		(FwupdDevice	*self);
void		 fwupd_device_set_composite_id		(FwupdDevice	*self,
							 const gchar	*composite_id);
FwupdDevice	*fwupd_device_get_parent		(FwupdDevice	*self);
void		 fwupd_device_set_parent		(FwupdDevice	*self,
							 FwupdDevice	*parent);
void		 fwupd_device_add_child			(FwupdDevice	*self,
							 FwupdDevice	*child);
void		 fwupd_device_remove_child		(FwupdDevice	*self,
							 FwupdDevice	*child);
GPtrArray	*fwupd_device_get_children		(FwupdDevice	*self);
const gchar	*fwupd_device_get_name			(FwupdDevice	*self);
void		 fwupd_device_set_name			(FwupdDevice	*self,
							 const gchar	*name);
const gchar	*fwupd_device_get_serial		(FwupdDevice	*self);
void		 fwupd_device_set_serial		(FwupdDevice	*self,
							 const gchar	*serial);
const gchar	*fwupd_device_get_summary		(FwupdDevice	*self);
void		 fwupd_device_set_summary		(FwupdDevice	*self,
							 const gchar	*summary);
const gchar	*fwupd_device_get_branch		(FwupdDevice	*self);
void		 fwupd_device_set_branch		(FwupdDevice	*self,
							 const gchar	*branch);
const gchar	*fwupd_device_get_description		(FwupdDevice	*self);
void		 fwupd_device_set_description		(FwupdDevice	*self,
							 const gchar	*description);
const gchar	*fwupd_device_get_version		(FwupdDevice	*self);
void		 fwupd_device_set_version		(FwupdDevice	*self,
							 const gchar	*version);
const gchar	*fwupd_device_get_version_lowest	(FwupdDevice	*self);
void		 fwupd_device_set_version_lowest	(FwupdDevice	*self,
							 const gchar	*version_lowest);
guint64		 fwupd_device_get_version_lowest_raw	(FwupdDevice	*self);
void		 fwupd_device_set_version_lowest_raw	(FwupdDevice	*self,
							 guint64	version_lowest_raw);
const gchar	*fwupd_device_get_version_bootloader	(FwupdDevice	*self);
void		 fwupd_device_set_version_bootloader	(FwupdDevice	*self,
							 const gchar	*version_bootloader);
guint64		 fwupd_device_get_version_bootloader_raw (FwupdDevice	*self);
void		 fwupd_device_set_version_bootloader_raw (FwupdDevice	*self,
							 guint64	version_bootloader_raw);
guint64		 fwupd_device_get_version_raw		(FwupdDevice	*self);
void		 fwupd_device_set_version_raw		(FwupdDevice	*self,
							 guint64	version_raw);
guint64		 fwupd_device_get_version_build_date	(FwupdDevice	*self);
void		 fwupd_device_set_version_build_date	(FwupdDevice	*self,
							 guint64	 version_build_date);
FwupdVersionFormat fwupd_device_get_version_format	(FwupdDevice	*self);
void		 fwupd_device_set_version_format	(FwupdDevice	*self,
							 FwupdVersionFormat version_format);
guint32		 fwupd_device_get_flashes_left		(FwupdDevice	*self);
void		 fwupd_device_set_flashes_left		(FwupdDevice	*self,
							 guint32	flashes_left);
guint32		 fwupd_device_get_install_duration	(FwupdDevice	*self);
void		 fwupd_device_set_install_duration	(FwupdDevice	*self,
							 guint32	 duration);
guint64		 fwupd_device_get_flags			(FwupdDevice	*self);
void		 fwupd_device_set_flags			(FwupdDevice	*self,
							 guint64	 flags);
void		 fwupd_device_add_flag			(FwupdDevice	*self,
							 FwupdDeviceFlags flag);
void		 fwupd_device_remove_flag		(FwupdDevice	*self,
							 FwupdDeviceFlags flag);
gboolean	 fwupd_device_has_flag			(FwupdDevice	*self,
							 FwupdDeviceFlags flag);
guint64		 fwupd_device_get_created		(FwupdDevice	*self);
void		 fwupd_device_set_created		(FwupdDevice	*self,
							 guint64	 created);
guint64		 fwupd_device_get_modified		(FwupdDevice	*self);
void		 fwupd_device_set_modified		(FwupdDevice	*self,
							 guint64	 modified);
GPtrArray	*fwupd_device_get_checksums		(FwupdDevice	*self);
void		 fwupd_device_add_checksum		(FwupdDevice	*self,
							 const gchar	*checksum);
const gchar	*fwupd_device_get_plugin		(FwupdDevice	*self);
void		 fwupd_device_set_plugin		(FwupdDevice	*self,
							 const gchar	*plugin);
G_DEPRECATED_FOR(fwupd_device_get_protocols)
const gchar	*fwupd_device_get_protocol		(FwupdDevice	*self);
G_DEPRECATED_FOR(fwupd_device_add_protocol)
void		 fwupd_device_set_protocol		(FwupdDevice	*self,
							 const gchar	*protocol);
void		 fwupd_device_add_protocol		(FwupdDevice	*self,
							 const gchar	*protocol);
gboolean	 fwupd_device_has_protocol		(FwupdDevice	*self,
							 const gchar	*protocol);
GPtrArray	*fwupd_device_get_protocols		(FwupdDevice	*self);
const gchar	*fwupd_device_get_vendor		(FwupdDevice	*self);
void		 fwupd_device_set_vendor		(FwupdDevice	*self,
							 const gchar	*vendor);
G_DEPRECATED_FOR(fwupd_device_get_vendor_ids)
const gchar	*fwupd_device_get_vendor_id		(FwupdDevice	*self);
G_DEPRECATED_FOR(fwupd_device_add_vendor_id)
void		 fwupd_device_set_vendor_id		(FwupdDevice	*self,
							 const gchar	*vendor_id);
void		 fwupd_device_add_vendor_id		(FwupdDevice	*self,
							 const gchar	*vendor_id);
gboolean	 fwupd_device_has_vendor_id		(FwupdDevice	*self,
							 const gchar	*vendor_id);
GPtrArray	*fwupd_device_get_vendor_ids		(FwupdDevice	*self);
void		 fwupd_device_add_guid			(FwupdDevice	*self,
							 const gchar	*guid);
gboolean	 fwupd_device_has_guid			(FwupdDevice	*self,
							 const gchar	*guid);
GPtrArray	*fwupd_device_get_guids			(FwupdDevice	*self);
const gchar	*fwupd_device_get_guid_default		(FwupdDevice	*self);
void		 fwupd_device_add_instance_id		(FwupdDevice	*self,
							 const gchar	*instance_id);
gboolean	 fwupd_device_has_instance_id		(FwupdDevice	*self,
							 const gchar	*instance_id);
GPtrArray	*fwupd_device_get_instance_ids		(FwupdDevice	*self);
void		 fwupd_device_add_icon			(FwupdDevice	*self,
							 const gchar	*icon);
gboolean	 fwupd_device_has_icon			(FwupdDevice	*self,
							 const gchar	*icon);
GPtrArray	*fwupd_device_get_icons			(FwupdDevice	*self);

FwupdUpdateState fwupd_device_get_update_state		(FwupdDevice	*self);
void		 fwupd_device_set_update_state		(FwupdDevice	*self,
							 FwupdUpdateState update_state);
const gchar	*fwupd_device_get_update_error		(FwupdDevice	*self);
void		 fwupd_device_set_update_error		(FwupdDevice	*self,
							 const gchar	*update_error);
FwupdDeviceMessageKind fwupd_device_get_update_message_kind	(FwupdDevice	*self);
void		 fwupd_device_set_update_message_kind	(FwupdDevice	*self,
							 FwupdDeviceMessageKind	 update_message_kind);
const gchar	*fwupd_device_get_update_message	(FwupdDevice	*self);
void		 fwupd_device_set_update_message	(FwupdDevice	*self,
							 const gchar	*update_message);
const gchar	*fwupd_device_get_update_image		(FwupdDevice	*self);
void		 fwupd_device_set_update_image		(FwupdDevice	*self,
							 const gchar	*update_image);
FwupdStatus	 fwupd_device_get_status		(FwupdDevice	*self);
void		 fwupd_device_set_status		(FwupdDevice	*self,
							 FwupdStatus	 status);
void		 fwupd_device_add_release		(FwupdDevice	*self,
							 FwupdRelease	*release);
GPtrArray	*fwupd_device_get_releases		(FwupdDevice	*self);
FwupdRelease	*fwupd_device_get_release_default	(FwupdDevice	*self);
gint		 fwupd_device_compare			(FwupdDevice	*self1,
							 FwupdDevice	*self2);

FwupdDevice	*fwupd_device_from_variant		(GVariant	*value);
GPtrArray	*fwupd_device_array_from_variant	(GVariant	*value);
void		 fwupd_device_array_ensure_parents	(GPtrArray	*devices);

G_END_DECLS
