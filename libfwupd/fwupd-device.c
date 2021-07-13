/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <glib-object.h>
#include <gio/gio.h>
#include <string.h>

#include "fwupd-common-private.h"
#include "fwupd-enums-private.h"
#include "fwupd-error.h"
#include "fwupd-device-private.h"
#include "fwupd-release-private.h"

/**
 * FwupdDevice:
 *
 * A physical device on the host with optionally updatable firmware.
 *
 * See also: [class@FwupdRelease]
 */

static void fwupd_device_finalize	 (GObject *object);

typedef struct {
	gchar				*id;
	gchar				*parent_id;
	gchar				*composite_id;
	guint64				 created;
	guint64				 modified;
	guint64				 flags;
	GPtrArray			*guids;
	GPtrArray			*vendor_ids;
	GPtrArray			*protocols;
	GPtrArray			*instance_ids;
	GPtrArray			*icons;
	gchar				*name;
	gchar				*serial;
	gchar				*summary;
	gchar				*branch;
	gchar				*description;
	gchar				*vendor;
	gchar				*vendor_id;	/* for compat only */
	gchar				*homepage;
	gchar				*plugin;
	gchar				*protocol;
	gchar				*version;
	gchar				*version_lowest;
	gchar				*version_bootloader;
	FwupdVersionFormat		 version_format;
	guint64				 version_raw;
	guint64				 version_build_date;
	guint64				 version_lowest_raw;
	guint64				 version_bootloader_raw;
	GPtrArray			*checksums;
	GPtrArray			*children;
	guint32				 flashes_left;
	guint32				 install_duration;
	FwupdUpdateState		 update_state;
	gchar				*update_error;
	gchar				*update_message;
	gchar				*update_image;
	FwupdStatus			 status;
	FwupdDeviceMessageKind		 update_message_kind;
	GPtrArray			*releases;
	FwupdDevice			*parent;	/* noref */
} FwupdDevicePrivate;

enum {
	PROP_0,
	PROP_VERSION_FORMAT,
	PROP_FLAGS,
	PROP_PROTOCOL,
	PROP_STATUS,
	PROP_PARENT,
	PROP_UPDATE_STATE,
	PROP_UPDATE_MESSAGE_KIND,
	PROP_UPDATE_MESSAGE,
	PROP_UPDATE_IMAGE,
	PROP_LAST
};

G_DEFINE_TYPE_WITH_PRIVATE (FwupdDevice, fwupd_device, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (fwupd_device_get_instance_private (o))

/**
 * fwupd_device_message_kind_to_string:
 * @update_message_kind: a update message kind, e.g. %FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE
 *
 * Converts a enumerated update message kind to a string.
 *
 * Returns: identifier string
 *
 * Since: 1.6.2
 **/
const gchar *
fwupd_device_message_kind_to_string (FwupdDeviceMessageKind update_message_kind)
{
	if (update_message_kind == FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN)
		return "unknown";
	if (update_message_kind == FWUPD_DEVICE_MESSAGE_KIND_POST)
		return "post";
	if (update_message_kind == FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE)
		return "immediate";
	return NULL;
}

/**
 * fwupd_device_message_kind_from_string:
 * @update_message_kind: a string, e.g. `immediate`
 *
 * Converts a string to an enumerated update message kind.
 *
 * Returns: enumerated value
 *
 * Since: 1.6.2
 **/
FwupdDeviceMessageKind
fwupd_device_message_kind_from_string (const gchar *update_message_kind)
{
	if (g_strcmp0 (update_message_kind, "unknown") == 0)
		return FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN;
	if (g_strcmp0 (update_message_kind, "post") == 0)
		return FWUPD_DEVICE_MESSAGE_KIND_POST;
	if (g_strcmp0 (update_message_kind, "immediate") == 0)
		return FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE;
	return FWUPD_DEVICE_MESSAGE_KIND_LAST;
}

/**
 * fwupd_device_get_checksums:
 * @self: a #FwupdDevice
 *
 * Gets the device checksums.
 *
 * Returns: (element-type utf8) (transfer none): the checksums, which may be empty
 *
 * Since: 0.9.3
 **/
GPtrArray *
fwupd_device_get_checksums (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->checksums;
}

/**
 * fwupd_device_add_checksum:
 * @self: a #FwupdDevice
 * @checksum: the device checksum
 *
 * Sets the device checksum.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_add_checksum (FwupdDevice *self, const gchar *checksum)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (checksum != NULL);
	for (guint i = 0; i < priv->checksums->len; i++) {
		const gchar *checksum_tmp = g_ptr_array_index (priv->checksums, i);
		if (g_strcmp0 (checksum_tmp, checksum) == 0)
			return;
	}
	g_ptr_array_add (priv->checksums, g_strdup (checksum));
}

/**
 * fwupd_device_get_children:
 * @self: a #FwupdDevice
 *
 * Gets the device children. These can only be assigned using fwupd_device_set_parent().
 *
 * Returns: (element-type FwupdDevice) (transfer none): the children, which may be empty
 *
 * Since: 1.3.7
 **/
GPtrArray *
fwupd_device_get_children (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->children;
}

/**
 * fwupd_device_get_summary:
 * @self: a #FwupdDevice
 *
 * Gets the device summary.
 *
 * Returns: the device summary, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_summary (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->summary;
}

/**
 * fwupd_device_set_summary:
 * @self: a #FwupdDevice
 * @summary: (nullable): the device one line summary
 *
 * Sets the device summary.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_summary (FwupdDevice *self, const gchar *summary)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->summary, summary) == 0)
		return;

	g_free (priv->summary);
	priv->summary = g_strdup (summary);
}

/**
 * fwupd_device_get_branch:
 * @self: a #FwupdDevice
 *
 * Gets the current device branch.
 *
 * Returns: the device branch, or %NULL if unset
 *
 * Since: 1.5.0
 **/
const gchar *
fwupd_device_get_branch (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->branch;
}

/**
 * fwupd_device_set_branch:
 * @self: a #FwupdDevice
 * @branch: (nullable): the device one line branch
 *
 * Sets the current device branch.
 *
 * Since: 1.5.0
 **/
void
fwupd_device_set_branch (FwupdDevice *self, const gchar *branch)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->branch, branch) == 0)
		return;

	g_free (priv->branch);
	priv->branch = g_strdup (branch);
}

/**
 * fwupd_device_get_serial:
 * @self: a #FwupdDevice
 *
 * Gets the serial number for the device.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.2
 **/
const gchar *
fwupd_device_get_serial (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->serial;
}

/**
 * fwupd_device_set_serial:
 * @self: a #FwupdDevice
 * @serial: (nullable): the device serial number
 *
 * Sets the serial number for the device.
 *
 * Since: 1.1.2
 **/
void
fwupd_device_set_serial (FwupdDevice *self, const gchar *serial)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->serial, serial) == 0)
		return;

	g_free (priv->serial);
	priv->serial = g_strdup (serial);
}

/**
 * fwupd_device_get_id:
 * @self: a #FwupdDevice
 *
 * Gets the ID.
 *
 * Returns: the ID, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_id (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->id;
}

/**
 * fwupd_device_set_id:
 * @self: a #FwupdDevice
 * @id: (nullable): the device ID, e.g. `USB:foo`
 *
 * Sets the ID.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_id (FwupdDevice *self, const gchar *id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->id, id) == 0)
		return;

	g_free (priv->id);
	priv->id = g_strdup (id);
}

/**
 * fwupd_device_get_parent_id:
 * @self: a #FwupdDevice
 *
 * Gets the ID.
 *
 * Returns: the parent ID, or %NULL if unset
 *
 * Since: 1.0.8
 **/
const gchar *
fwupd_device_get_parent_id (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->parent_id;
}

/**
 * fwupd_device_set_parent_id:
 * @self: a #FwupdDevice
 * @parent_id: (nullable): the device ID, e.g. `USB:foo`
 *
 * Sets the parent ID.
 *
 * Since: 1.0.8
 **/
void
fwupd_device_set_parent_id (FwupdDevice *self, const gchar *parent_id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->parent_id, parent_id) == 0)
		return;

	g_free (priv->parent_id);
	priv->parent_id = g_strdup (parent_id);
}

/**
 * fwupd_device_get_composite_id:
 * @self: a #FwupdDevice
 *
 * Gets the composite ID, falling back to the device ID if unset.
 *
 * The composite ID will be the same value for all parent, child and sibling
 * devices.
 *
 * Returns: (nullable): the composite ID
 *
 * Since: 1.6.0
 **/
const gchar *
fwupd_device_get_composite_id (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	if (priv->composite_id != NULL)
		return priv->composite_id;
	return priv->id;
}

/**
 * fwupd_device_set_composite_id:
 * @self: a #FwupdDevice
 * @composite_id: (nullable): a device ID
 *
 * Sets the composite ID, which is usually a SHA1 hash of a grandparent or
 * parent device.
 *
 * Since: 1.6.0
 **/
void
fwupd_device_set_composite_id (FwupdDevice *self, const gchar *composite_id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->composite_id, composite_id) == 0)
		return;

	g_free (priv->composite_id);
	priv->composite_id = g_strdup (composite_id);
}

/**
 * fwupd_device_get_parent:
 * @self: a #FwupdDevice
 *
 * Gets the parent.
 *
 * Returns: (transfer none): the parent device, or %NULL if unset
 *
 * Since: 1.0.8
 **/
FwupdDevice *
fwupd_device_get_parent (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->parent;
}

/**
 * fwupd_device_set_parent:
 * @self: a #FwupdDevice
 * @parent: (nullable): another #FwupdDevice
 *
 * Sets the parent. Only used internally.
 *
 * Since: 1.0.8
 **/
void
fwupd_device_set_parent (FwupdDevice *self, FwupdDevice *parent)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	if (priv->parent != NULL)
		g_object_remove_weak_pointer (G_OBJECT (priv->parent), (gpointer *) &priv->parent);
	if (parent != NULL)
		g_object_add_weak_pointer (G_OBJECT (parent), (gpointer *) &priv->parent);
	priv->parent = parent;

	/* this is what goes over D-Bus */
	fwupd_device_set_parent_id (self, parent != NULL ? fwupd_device_get_id (parent) : NULL);
}

static void
fwupd_device_child_finalized_cb (gpointer data, GObject *where_the_object_was)
{
	FwupdDevice *self = FWUPD_DEVICE (data);
	g_critical ("FuDevice child %p was finalized while still having parent %s [%s]!",
		    where_the_object_was,
		    fwupd_device_get_name (self),
		    fwupd_device_get_id (self));
}

/**
 * fwupd_device_add_child:
 * @self: a #FwupdDevice
 * @child: Another #FwupdDevice
 *
 * Adds a child device. An child device is logically linked to the primary
 * device in some way.
 *
 * NOTE: You should never call this function from user code, it is for daemon
 * use only. Only use fwupd_device_set_parent() to set up a logical tree.
 *
 * Since: 1.5.1
 **/
void
fwupd_device_add_child (FwupdDevice *self, FwupdDevice *child)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	/* add if the child does not already exist */
	for (guint i = 0; i < priv->children->len; i++) {
		FwupdDevice *devtmp = g_ptr_array_index (priv->children, i);
		if (devtmp == child)
			return;
	}
	g_object_weak_ref (G_OBJECT (child),
			   fwupd_device_child_finalized_cb,
			   self);
	g_ptr_array_add (priv->children, g_object_ref (child));
}

/**
 * fwupd_device_remove_child:
 * @self: a #FwupdDevice
 * @child: Another #FwupdDevice
 *
 * Removes a child device.
 *
 * NOTE: You should never call this function from user code, it is for daemon
 * use only.
 *
 * Since: 1.6.2
 **/
void
fwupd_device_remove_child (FwupdDevice *self, FwupdDevice *child)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	/* remove if the child exists */
	for (guint i = 0; i < priv->children->len; i++) {
		FwupdDevice *child_tmp = g_ptr_array_index (priv->children, i);
		if (child_tmp == child) {
			g_object_weak_unref (G_OBJECT (child),
					     fwupd_device_child_finalized_cb,
					     self);
			g_ptr_array_remove_index (priv->children, i);
			return;
		}
	}
}

/**
 * fwupd_device_get_guids:
 * @self: a #FwupdDevice
 *
 * Gets the GUIDs.
 *
 * Returns: (element-type utf8) (transfer none): the GUIDs
 *
 * Since: 0.9.3
 **/
GPtrArray *
fwupd_device_get_guids (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->guids;
}

/**
 * fwupd_device_has_guid:
 * @self: a #FwupdDevice
 * @guid: the GUID, e.g. `2082b5e0-7a64-478a-b1b2-e3404fab6dad`
 *
 * Finds out if the device has this specific GUID.
 *
 * Returns: %TRUE if the GUID is found
 *
 * Since: 0.9.3
 **/
gboolean
fwupd_device_has_guid (FwupdDevice *self, const gchar *guid)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (guid != NULL, FALSE);

	for (guint i = 0; i < priv->guids->len; i++) {
		const gchar *guid_tmp = g_ptr_array_index (priv->guids, i);
		if (g_strcmp0 (guid, guid_tmp) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fwupd_device_add_guid:
 * @self: a #FwupdDevice
 * @guid: the GUID, e.g. `2082b5e0-7a64-478a-b1b2-e3404fab6dad`
 *
 * Adds the GUID if it does not already exist.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_add_guid (FwupdDevice *self, const gchar *guid)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (guid != NULL);
	if (fwupd_device_has_guid (self, guid))
		return;
	g_ptr_array_add (priv->guids, g_strdup (guid));
}

/**
 * fwupd_device_get_guid_default:
 * @self: a #FwupdDevice
 *
 * Gets the default GUID.
 *
 * Returns: the GUID, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_guid_default (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	if (priv->guids->len == 0)
		return NULL;
	return g_ptr_array_index (priv->guids, 0);
}

/**
 * fwupd_device_get_instance_ids:
 * @self: a #FwupdDevice
 *
 * Gets the InstanceIDs.
 *
 * Returns: (element-type utf8) (transfer none): the InstanceID
 *
 * Since: 1.2.5
 **/
GPtrArray *
fwupd_device_get_instance_ids (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->instance_ids;
}

/**
 * fwupd_device_has_instance_id:
 * @self: a #FwupdDevice
 * @instance_id: the InstanceID, e.g. `PCI\VEN_10EC&DEV_525A`
 *
 * Finds out if the device has this specific InstanceID.
 *
 * Returns: %TRUE if the InstanceID is found
 *
 * Since: 1.2.5
 **/
gboolean
fwupd_device_has_instance_id (FwupdDevice *self, const gchar *instance_id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (instance_id != NULL, FALSE);

	for (guint i = 0; i < priv->instance_ids->len; i++) {
		const gchar *instance_id_tmp = g_ptr_array_index (priv->instance_ids, i);
		if (g_strcmp0 (instance_id, instance_id_tmp) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fwupd_device_add_instance_id:
 * @self: a #FwupdDevice
 * @instance_id: the GUID, e.g. `PCI\VEN_10EC&DEV_525A`
 *
 * Adds the InstanceID if it does not already exist.
 *
 * Since: 1.2.5
 **/
void
fwupd_device_add_instance_id (FwupdDevice *self, const gchar *instance_id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (instance_id != NULL);
	if (fwupd_device_has_instance_id (self, instance_id))
		return;
	g_ptr_array_add (priv->instance_ids, g_strdup (instance_id));
}

/**
 * fwupd_device_get_icons:
 * @self: a #FwupdDevice
 *
 * Gets the icon names to use for the device.
 *
 * NOTE: Icons specified without a full path are stock icons and should
 * be loaded from the users icon theme.
 *
 * Returns: (element-type utf8) (transfer none): an array of icon names
 *
 * Since: 0.9.8
 **/
GPtrArray *
fwupd_device_get_icons (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->icons;
}


/**
 * fwupd_device_has_icon:
 * @self: a #FwupdDevice
 * @icon: the name, e.g. `input-mouse` or `/usr/share/icons/foo.png`
 *
 * Finds out if the device has this specific icon.
 *
 * Returns: %TRUE if the icon is found
 *
 * Since: 1.6.2
 **/
gboolean
fwupd_device_has_icon (FwupdDevice *self, const gchar *icon)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	for (guint i = 0; i < priv->icons->len; i++) {
		const gchar *icon_tmp = g_ptr_array_index (priv->icons, i);
		if (g_strcmp0 (icon, icon_tmp) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fwupd_device_add_icon:
 * @self: a #FwupdDevice
 * @icon: the name, e.g. `input-mouse` or `/usr/share/icons/foo.png`
 *
 * Adds the icon name if it does not already exist.
 *
 * Since: 0.9.8
 **/
void
fwupd_device_add_icon (FwupdDevice *self, const gchar *icon)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (icon != NULL);
	if (fwupd_device_has_icon (self, icon))
		return;
	g_ptr_array_add (priv->icons, g_strdup (icon));
}

/**
 * fwupd_device_get_name:
 * @self: a #FwupdDevice
 *
 * Gets the device name.
 *
 * Returns: the device name, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_name (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->name;
}

/**
 * fwupd_device_set_name:
 * @self: a #FwupdDevice
 * @name: (nullable): the device name, e.g. `ColorHug2`
 *
 * Sets the device name.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_name (FwupdDevice *self, const gchar *name)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->name, name) == 0)
		return;

	g_free (priv->name);
	priv->name = g_strdup (name);
}

/**
 * fwupd_device_get_vendor:
 * @self: a #FwupdDevice
 *
 * Gets the device vendor.
 *
 * Returns: the device vendor, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_vendor (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->vendor;
}

/**
 * fwupd_device_set_vendor:
 * @self: a #FwupdDevice
 * @vendor: (nullable): the description
 *
 * Sets the device vendor.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_vendor (FwupdDevice *self, const gchar *vendor)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->vendor, vendor) == 0)
		return;

	g_free (priv->vendor);
	priv->vendor = g_strdup (vendor);
}

/**
 * fwupd_device_get_vendor_id:
 * @self: a #FwupdDevice
 *
 * Gets the combined device vendor ID.
 *
 * Returns: the device vendor, e.g. 'USB:0x1234|PCI:0x5678', or %NULL if unset
 *
 * Since: 0.9.4
 *
 * Deprecated: 1.5.5: Use fwupd_device_get_vendor_ids() instead.
 **/
const gchar *
fwupd_device_get_vendor_id (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->vendor_id;
}

/**
 * fwupd_device_set_vendor_id:
 * @self: a #FwupdDevice
 * @vendor_id: the ID, e.g. 'USB:0x1234' or 'USB:0x1234|PCI:0x5678'
 *
 * Sets the device vendor ID.
 *
 * Since: 0.9.4
 *
 * Deprecated: 1.5.5: Use fwupd_device_add_vendor_id() instead.
 **/
void
fwupd_device_set_vendor_id (FwupdDevice *self, const gchar *vendor_id)
{
	g_auto(GStrv) vendor_ids = NULL;

	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (vendor_id != NULL);

	/* add all */
	vendor_ids = g_strsplit (vendor_id, "|", -1);
	for (guint i = 0; vendor_ids[i] != NULL; i++)
		fwupd_device_add_vendor_id (self, vendor_ids[i]);
}

/**
 * fwupd_device_get_vendor_ids:
 * @self: a #FwupdDevice
 *
 * Gets the device vendor ID.
 *
 * Returns: (element-type utf8) (transfer none): the device vendor ID
 *
 * Since: 1.5.5
 **/
GPtrArray *
fwupd_device_get_vendor_ids (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->vendor_ids;
}

/**
 * fwupd_device_has_vendor_id:
 * @self: a #FwupdDevice
 * @vendor_id: the ID, e.g. 'USB:0x1234'
 *
 * Finds out if the device has this specific vendor ID.
 *
 * Returns: %TRUE if the ID is found
 *
 * Since: 1.5.5
 **/
gboolean
fwupd_device_has_vendor_id (FwupdDevice *self, const gchar *vendor_id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (vendor_id != NULL, FALSE);

	for (guint i = 0; i < priv->vendor_ids->len; i++) {
		const gchar *vendor_id_tmp = g_ptr_array_index (priv->vendor_ids, i);
		if (g_strcmp0 (vendor_id, vendor_id_tmp) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fwupd_device_add_vendor_id:
 * @self: a #FwupdDevice
 * @vendor_id: the ID, e.g. 'USB:0x1234'
 *
 * Adds a device vendor ID.
 *
 * Since: 1.5.5
 **/
void
fwupd_device_add_vendor_id (FwupdDevice *self, const gchar *vendor_id)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_auto(GStrv) vendor_ids_tmp = NULL;

	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (vendor_id != NULL);

	if (fwupd_device_has_vendor_id (self, vendor_id))
		return;
	g_ptr_array_add (priv->vendor_ids, g_strdup (vendor_id));

	/* build for compatibility */
	vendor_ids_tmp = g_new0 (gchar *, priv->vendor_ids->len + 1);
	for (guint i = 0; i < priv->vendor_ids->len; i++) {
		const gchar *vendor_id_tmp = g_ptr_array_index (priv->vendor_ids, i);
		vendor_ids_tmp[i] = g_strdup (vendor_id_tmp);
	}
	g_free (priv->vendor_id);
	priv->vendor_id = g_strjoinv ("|", vendor_ids_tmp);
}

/**
 * fwupd_device_get_description:
 * @self: a #FwupdDevice
 *
 * Gets the device description in AppStream markup format.
 *
 * Returns: the device description, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_description (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->description;
}

/**
 * fwupd_device_set_description:
 * @self: a #FwupdDevice
 * @description: (nullable): the description in AppStream markup format
 *
 * Sets the device description.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_description (FwupdDevice *self, const gchar *description)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->description, description) == 0)
		return;

	g_free (priv->description);
	priv->description = g_strdup (description);
}

/**
 * fwupd_device_get_version:
 * @self: a #FwupdDevice
 *
 * Gets the device version.
 *
 * Returns: the device version, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_version (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->version;
}

/**
 * fwupd_device_set_version:
 * @self: a #FwupdDevice
 * @version: (nullable): the device version, e.g. `1.2.3`
 *
 * Sets the device version.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_version (FwupdDevice *self, const gchar *version)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->version, version) == 0)
		return;

	g_free (priv->version);
	priv->version = g_strdup (version);
}

/**
 * fwupd_device_get_version_lowest:
 * @self: a #FwupdDevice
 *
 * Gets the lowest version of firmware the device will accept.
 *
 * Returns: the device version_lowest, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_version_lowest (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->version_lowest;
}

/**
 * fwupd_device_set_version_lowest:
 * @self: a #FwupdDevice
 * @version_lowest: (nullable): the version
 *
 * Sets the lowest version of firmware the device will accept.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_version_lowest (FwupdDevice *self, const gchar *version_lowest)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->version_lowest, version_lowest) == 0)
		return;

	g_free (priv->version_lowest);
	priv->version_lowest = g_strdup (version_lowest);
}

/**
 * fwupd_device_get_version_lowest_raw:
 * @self: a #FwupdDevice
 *
 * Gets the lowest version of firmware the device will accept in raw format.
 *
 * Returns: integer version number, or %0 if unset
 *
 * Since: 1.4.0
 **/
guint64
fwupd_device_get_version_lowest_raw (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->version_lowest_raw;
}

/**
 * fwupd_device_set_version_lowest_raw:
 * @self: a #FwupdDevice
 * @version_lowest_raw: the raw hardware version
 *
 * Sets the raw lowest version number from the hardware before converted to a string.
 *
 * Since: 1.4.0
 **/
void
fwupd_device_set_version_lowest_raw (FwupdDevice *self, guint64 version_lowest_raw)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->version_lowest_raw = version_lowest_raw;
}

/**
 * fwupd_device_get_version_bootloader:
 * @self: a #FwupdDevice
 *
 * Gets the version of the bootloader.
 *
 * Returns: the device version_bootloader, or %NULL if unset
 *
 * Since: 0.9.3
 **/
const gchar *
fwupd_device_get_version_bootloader (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->version_bootloader;
}

/**
 * fwupd_device_set_version_bootloader:
 * @self: a #FwupdDevice
 * @version_bootloader: (nullable): the version
 *
 * Sets the bootloader version.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_version_bootloader (FwupdDevice *self, const gchar *version_bootloader)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->version_bootloader, version_bootloader) == 0)
		return;

	g_free (priv->version_bootloader);
	priv->version_bootloader = g_strdup (version_bootloader);
}

/**
 * fwupd_device_get_version_bootloader_raw:
 * @self: a #FwupdDevice
 *
 * Gets the bootloader version of firmware the device will accept in raw format.
 *
 * Returns: integer version number, or %0 if unset
 *
 * Since: 1.4.0
 **/
guint64
fwupd_device_get_version_bootloader_raw (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->version_bootloader_raw;
}

/**
 * fwupd_device_set_version_bootloader_raw:
 * @self: a #FwupdDevice
 * @version_bootloader_raw: the raw hardware version
 *
 * Sets the raw bootloader version number from the hardware before converted to a string.
 *
 * Since: 1.4.0
 **/
void
fwupd_device_set_version_bootloader_raw (FwupdDevice *self, guint64 version_bootloader_raw)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->version_bootloader_raw = version_bootloader_raw;
}

/**
 * fwupd_device_get_flashes_left:
 * @self: a #FwupdDevice
 *
 * Gets the number of flash cycles left on the device
 *
 * Returns: the flash cycles left, or %NULL if unset
 *
 * Since: 0.9.3
 **/
guint32
fwupd_device_get_flashes_left (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->flashes_left;
}

/**
 * fwupd_device_set_flashes_left:
 * @self: a #FwupdDevice
 * @flashes_left: the description
 *
 * Sets the number of flash cycles left on the device
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_flashes_left (FwupdDevice *self, guint32 flashes_left)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->flashes_left = flashes_left;
}

/**
 * fwupd_device_get_install_duration:
 * @self: a #FwupdDevice
 *
 * Gets the time estimate for firmware installation (in seconds)
 *
 * Returns: the estimated time to flash this device (or 0 if unset)
 *
 * Since: 1.1.3
 **/
guint32
fwupd_device_get_install_duration (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->install_duration;
}

/**
 * fwupd_device_set_install_duration:
 * @self: a #FwupdDevice
 * @duration: the amount of time
 *
 * Sets the time estimate for firmware installation (in seconds)
 *
 * Since: 1.1.3
 **/
void
fwupd_device_set_install_duration (FwupdDevice *self, guint32 duration)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->install_duration = duration;
}

/**
 * fwupd_device_get_plugin:
 * @self: a #FwupdDevice
 *
 * Gets the plugin that created the device.
 *
 * Returns: the plugin name, or %NULL if unset
 *
 * Since: 1.0.0
 **/
const gchar *
fwupd_device_get_plugin (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->plugin;
}

/**
 * fwupd_device_set_plugin:
 * @self: a #FwupdDevice
 * @plugin: (nullable): the plugin name, e.g. `colorhug`
 *
 * Sets the plugin that created the device.
 *
 * Since: 1.0.0
 **/
void
fwupd_device_set_plugin (FwupdDevice *self, const gchar *plugin)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->plugin, plugin) == 0)
		return;

	g_free (priv->plugin);
	priv->plugin = g_strdup (plugin);
}

/**
 * fwupd_device_get_protocol:
 * @self: a #FwupdDevice
 *
 * Gets the protocol that the device uses for updating.
 *
 * Returns: the protocol name, or %NULL if unset
 *
 * Since: 1.3.6
 *
 * Deprecated: 1.5.8: Use fwupd_device_get_protocols() instead.
 **/
const gchar *
fwupd_device_get_protocol (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->protocol;
}

/**
 * fwupd_device_set_protocol:
 * @self: a #FwupdDevice
 * @protocol: the protocol name, e.g. `com.hughski.colorhug`
 *
 * Sets the protocol that is used to update the device.
 *
 * Since: 1.3.6
 *
 * Deprecated: 1.5.8: Use fwupd_device_add_protocol() instead.
 **/
void
fwupd_device_set_protocol (FwupdDevice *self, const gchar *protocol)
{
	g_auto(GStrv) protocols = NULL;

	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (protocol != NULL);

	/* add all */
	protocols = g_strsplit (protocol, "|", -1);
	for (guint i = 0; protocols[i] != NULL; i++)
		fwupd_device_add_protocol (self, protocols[i]);
}

/**
 * fwupd_device_get_protocols:
 * @self: a #FwupdDevice
 *
 * Gets the device protocol.
 *
 * Returns: (element-type utf8) (transfer none): the device protocol
 *
 * Since: 1.5.8
 **/
GPtrArray *
fwupd_device_get_protocols (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->protocols;
}

/**
 * fwupd_device_has_protocol:
 * @self: a #FwupdDevice
 * @protocol: the ID, e.g. 'USB:0x1234'
 *
 * Finds out if the device has this specific protocol.
 *
 * Returns: %TRUE if the ID is found
 *
 * Since: 1.5.8
 **/
gboolean
fwupd_device_has_protocol (FwupdDevice *self, const gchar *protocol)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (protocol != NULL, FALSE);

	for (guint i = 0; i < priv->protocols->len; i++) {
		const gchar *protocol_tmp = g_ptr_array_index (priv->protocols, i);
		if (g_strcmp0 (protocol, protocol_tmp) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fwupd_device_add_protocol:
 * @self: a #FwupdDevice
 * @protocol: the ID, e.g. 'USB:0x1234'
 *
 * Adds a device protocol.
 *
 * Since: 1.5.8
 **/
void
fwupd_device_add_protocol (FwupdDevice *self, const gchar *protocol)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_auto(GStrv) protocols_tmp = NULL;

	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (protocol != NULL);

	if (fwupd_device_has_protocol (self, protocol))
		return;
	g_ptr_array_add (priv->protocols, g_strdup (protocol));

	/* build for compatibility */
	protocols_tmp = g_new0 (gchar *, priv->protocols->len + 1);
	for (guint i = 0; i < priv->protocols->len; i++) {
		const gchar *protocol_tmp = g_ptr_array_index (priv->protocols, i);
		protocols_tmp[i] = g_strdup (protocol_tmp);
	}
	g_free (priv->protocol);
	priv->protocol = g_strjoinv ("|", protocols_tmp);
}

/**
 * fwupd_device_get_flags:
 * @self: a #FwupdDevice
 *
 * Gets device flags.
 *
 * Returns: device flags, or 0 if unset
 *
 * Since: 0.9.3
 **/
guint64
fwupd_device_get_flags (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->flags;
}

/**
 * fwupd_device_set_flags:
 * @self: a #FwupdDevice
 * @flags: device flags, e.g. %FWUPD_DEVICE_FLAG_REQUIRE_AC
 *
 * Sets device flags.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_flags (FwupdDevice *self, guint64 flags)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	if (priv->flags == flags)
		return;
	priv->flags = flags;
	g_object_notify (G_OBJECT (self), "flags");
}

/**
 * fwupd_device_add_flag:
 * @self: a #FwupdDevice
 * @flag: the #FwupdDeviceFlags
 *
 * Adds a specific device flag to the device.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_add_flag (FwupdDevice *self, FwupdDeviceFlags flag)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	if (flag == 0)
		return;
	if ((priv->flags | flag) == priv->flags)
		return;
	priv->flags |= flag;
	g_object_notify (G_OBJECT (self), "flags");
}

/**
 * fwupd_device_remove_flag:
 * @self: a #FwupdDevice
 * @flag: the #FwupdDeviceFlags
 *
 * Removes a specific device flag from the device.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_remove_flag (FwupdDevice *self, FwupdDeviceFlags flag)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	if (flag == 0)
		return;
	if ((priv->flags & flag) == 0)
		return;
	priv->flags &= ~flag;
	g_object_notify (G_OBJECT (self), "flags");
}

/**
 * fwupd_device_has_flag:
 * @self: a #FwupdDevice
 * @flag: the #FwupdDeviceFlags
 *
 * Finds if the device has a specific device flag.
 *
 * Returns: %TRUE if the flag is set
 *
 * Since: 0.9.3
 **/
gboolean
fwupd_device_has_flag (FwupdDevice *self, FwupdDeviceFlags flag)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FALSE);
	return (priv->flags & flag) > 0;
}

/**
 * fwupd_device_get_created:
 * @self: a #FwupdDevice
 *
 * Gets when the device was created.
 *
 * Returns: the UNIX time, or 0 if unset
 *
 * Since: 0.9.3
 **/
guint64
fwupd_device_get_created (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->created;
}

/**
 * fwupd_device_set_created:
 * @self: a #FwupdDevice
 * @created: the UNIX time
 *
 * Sets when the device was created.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_created (FwupdDevice *self, guint64 created)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->created = created;
}

/**
 * fwupd_device_get_modified:
 * @self: a #FwupdDevice
 *
 * Gets when the device was modified.
 *
 * Returns: the UNIX time, or 0 if unset
 *
 * Since: 0.9.3
 **/
guint64
fwupd_device_get_modified (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->modified;
}

/**
 * fwupd_device_set_modified:
 * @self: a #FwupdDevice
 * @modified: the UNIX time
 *
 * Sets when the device was modified.
 *
 * Since: 0.9.3
 **/
void
fwupd_device_set_modified (FwupdDevice *self, guint64 modified)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->modified = modified;
}

/**
 * fwupd_device_incorporate:
 * @self: a #FwupdDevice
 * @donor: Another #FwupdDevice
 *
 * Copy all properties from the donor object if they have not already been set.
 *
 * Since: 1.1.0
 **/
void
fwupd_device_incorporate (FwupdDevice *self, FwupdDevice *donor)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	FwupdDevicePrivate *priv_donor = GET_PRIVATE (donor);

	fwupd_device_add_flag (self, priv_donor->flags);
	if (priv->created == 0)
		fwupd_device_set_created (self, priv_donor->created);
	if (priv->modified == 0)
		fwupd_device_set_modified (self, priv_donor->modified);
	if (priv->version_build_date == 0)
		fwupd_device_set_version_build_date (self, priv_donor->version_build_date);
	if (priv->flashes_left == 0)
		fwupd_device_set_flashes_left (self, priv_donor->flashes_left);
	if (priv->install_duration == 0)
		fwupd_device_set_install_duration (self, priv_donor->install_duration);
	if (priv->update_state == 0)
		fwupd_device_set_update_state (self, priv_donor->update_state);
	if (priv->description == NULL)
		fwupd_device_set_description (self, priv_donor->description);
	if (priv->id == NULL)
		fwupd_device_set_id (self, priv_donor->id);
	if (priv->parent_id == NULL)
		fwupd_device_set_parent_id (self, priv_donor->parent_id);
	if (priv->composite_id == NULL)
		fwupd_device_set_composite_id (self, priv_donor->composite_id);
	if (priv->name == NULL)
		fwupd_device_set_name (self, priv_donor->name);
	if (priv->serial == NULL)
		fwupd_device_set_serial (self, priv_donor->serial);
	if (priv->summary == NULL)
		fwupd_device_set_summary (self, priv_donor->summary);
	if (priv->branch == NULL)
		fwupd_device_set_branch (self, priv_donor->branch);
	if (priv->vendor == NULL)
		fwupd_device_set_vendor (self, priv_donor->vendor);
	for (guint i = 0; i < priv_donor->vendor_ids->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv_donor->vendor_ids, i);
		fwupd_device_add_vendor_id (self, tmp);
	}
	if (priv->plugin == NULL)
		fwupd_device_set_plugin (self, priv_donor->plugin);
	for (guint i = 0; i < priv_donor->protocols->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv_donor->protocols, i);
		fwupd_device_add_protocol (self, tmp);
	}
	if (priv->update_error == NULL)
		fwupd_device_set_update_error (self, priv_donor->update_error);
	if (priv->update_message == NULL)
		fwupd_device_set_update_message (self, priv_donor->update_message);
	if (priv->update_image == NULL)
		fwupd_device_set_update_image (self, priv_donor->update_image);
	if (priv->version == NULL)
		fwupd_device_set_version (self, priv_donor->version);
	if (priv->version_lowest == NULL)
		fwupd_device_set_version_lowest (self, priv_donor->version_lowest);
	if (priv->version_bootloader == NULL)
		fwupd_device_set_version_bootloader (self, priv_donor->version_bootloader);
	if (priv->version_format == FWUPD_VERSION_FORMAT_UNKNOWN)
		fwupd_device_set_version_format (self, priv_donor->version_format);
	if (priv->version_raw == 0)
		fwupd_device_set_version_raw (self, priv_donor->version_raw);
	if (priv->version_lowest_raw == 0)
		fwupd_device_set_version_lowest_raw (self, priv_donor->version_lowest_raw);
	if (priv->version_bootloader_raw == 0)
		fwupd_device_set_version_bootloader_raw (self, priv_donor->version_bootloader_raw);
	for (guint i = 0; i < priv_donor->guids->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv_donor->guids, i);
		fwupd_device_add_guid (self, tmp);
	}
	for (guint i = 0; i < priv_donor->instance_ids->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv_donor->instance_ids, i);
		fwupd_device_add_instance_id (self, tmp);
	}
	for (guint i = 0; i < priv_donor->icons->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv_donor->icons, i);
		fwupd_device_add_icon (self, tmp);
	}
	for (guint i = 0; i < priv_donor->checksums->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv_donor->checksums, i);
		fwupd_device_add_checksum (self, tmp);
	}
	for (guint i = 0; i < priv_donor->releases->len; i++) {
		FwupdRelease *tmp = g_ptr_array_index (priv_donor->releases, i);
		fwupd_device_add_release (self, tmp);
	}
}

/**
 * fwupd_device_to_variant_full:
 * @self: a #FwupdDevice
 * @flags: device flags
 *
 * Serialize the device data.
 * Optionally provides additional data based upon flags
 *
 * Returns: the serialized data, or %NULL for error
 *
 * Since: 1.1.2
 **/
GVariant *
fwupd_device_to_variant_full (FwupdDevice *self, FwupdDeviceFlags flags)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	GVariantBuilder builder;

	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);

	/* create an array with all the metadata in */
	g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);
	if (priv->id != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_DEVICE_ID,
				       g_variant_new_string (priv->id));
	}
	if (priv->parent_id != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_PARENT_DEVICE_ID,
				       g_variant_new_string (priv->parent_id));
	}
	if (priv->composite_id != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_COMPOSITE_ID,
				       g_variant_new_string (priv->composite_id));
	}
	if (priv->guids->len > 0) {
		const gchar * const *tmp = (const gchar * const *) priv->guids->pdata;
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_GUID,
				       g_variant_new_strv (tmp, priv->guids->len));
	}
	if (priv->icons->len > 0) {
		const gchar * const *tmp = (const gchar * const *) priv->icons->pdata;
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_ICON,
				       g_variant_new_strv (tmp, priv->icons->len));
	}
	if (priv->name != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_NAME,
				       g_variant_new_string (priv->name));
	}
	if (priv->vendor != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VENDOR,
				       g_variant_new_string (priv->vendor));
	}
	if (priv->vendor_ids->len > 0) {
		g_autoptr(GString) str = g_string_new (NULL);
		for (guint i = 0; i < priv->vendor_ids->len; i++) {
			const gchar *tmp = g_ptr_array_index (priv->vendor_ids, i);
			g_string_append_printf (str, "%s|", tmp);
		}
		if (str->len > 0)
			g_string_truncate (str, str->len - 1);
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VENDOR_ID,
				       g_variant_new_string (str->str));
	}
	if (priv->flags > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_FLAGS,
				       g_variant_new_uint64 (priv->flags));
	}
	if (priv->created > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_CREATED,
				       g_variant_new_uint64 (priv->created));
	}
	if (priv->modified > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_MODIFIED,
				       g_variant_new_uint64 (priv->modified));
	}
	if (priv->version_build_date > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_BUILD_DATE,
				       g_variant_new_uint64 (priv->version_build_date));
	}

	if (priv->description != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_DESCRIPTION,
				       g_variant_new_string (priv->description));
	}
	if (priv->summary != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_SUMMARY,
				       g_variant_new_string (priv->summary));
	}
	if (priv->branch != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_BRANCH,
				       g_variant_new_string (priv->branch));
	}
	if (priv->checksums->len > 0) {
		g_autoptr(GString) str = g_string_new ("");
		for (guint i = 0; i < priv->checksums->len; i++) {
			const gchar *checksum = g_ptr_array_index (priv->checksums, i);
			g_string_append_printf (str, "%s,", checksum);
		}
		if (str->len > 0)
			g_string_truncate (str, str->len - 1);
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_CHECKSUM,
				       g_variant_new_string (str->str));
	}
	if (priv->plugin != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_PLUGIN,
				       g_variant_new_string (priv->plugin));
	}
	if (priv->protocols->len > 0) {
		g_autoptr(GString) str = g_string_new (NULL);
		for (guint i = 0; i < priv->protocols->len; i++) {
			const gchar *tmp = g_ptr_array_index (priv->protocols, i);
			g_string_append_printf (str, "%s|", tmp);
		}
		if (str->len > 0)
			g_string_truncate (str, str->len - 1);
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_PROTOCOL,
				       g_variant_new_string (str->str));
	}
	if (priv->version != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION,
				       g_variant_new_string (priv->version));
	}
	if (priv->version_lowest != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_LOWEST,
				       g_variant_new_string (priv->version_lowest));
	}
	if (priv->version_bootloader != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_BOOTLOADER,
				       g_variant_new_string (priv->version_bootloader));
	}
	if (priv->version_raw > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_RAW,
				       g_variant_new_uint64 (priv->version_raw));
	}
	if (priv->version_lowest_raw > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_LOWEST_RAW,
				       g_variant_new_uint64 (priv->version_raw));
	}
	if (priv->version_bootloader_raw > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_BOOTLOADER_RAW,
				       g_variant_new_uint64 (priv->version_raw));
	}
	if (priv->flashes_left > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_FLASHES_LEFT,
				       g_variant_new_uint32 (priv->flashes_left));
	}
	if (priv->install_duration > 0) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_INSTALL_DURATION,
				       g_variant_new_uint32 (priv->install_duration));
	}
	if (priv->update_error != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_UPDATE_ERROR,
				       g_variant_new_string (priv->update_error));
	}
	if (priv->update_message != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_UPDATE_MESSAGE,
				       g_variant_new_string (priv->update_message));
	}
	if (priv->update_image != NULL) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_UPDATE_IMAGE,
				       g_variant_new_string (priv->update_image));
	}
	if (priv->update_state != FWUPD_UPDATE_STATE_UNKNOWN) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_UPDATE_STATE,
				       g_variant_new_uint32 (priv->update_state));
	}
	if (priv->status != FWUPD_STATUS_UNKNOWN) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_STATUS,
				       g_variant_new_uint32 (priv->status));
	}
	if (priv->update_message_kind != FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_UPDATE_MESSAGE_KIND,
				       g_variant_new_uint32 (priv->update_message_kind));
	}
	if (priv->version_format != FWUPD_VERSION_FORMAT_UNKNOWN) {
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_VERSION_FORMAT,
				       g_variant_new_uint32 (priv->version_format));
	}
	if (flags & FWUPD_DEVICE_FLAG_TRUSTED) {
		if (priv->serial != NULL) {
			g_variant_builder_add (&builder, "{sv}",
					       FWUPD_RESULT_KEY_SERIAL,
					       g_variant_new_string (priv->serial));
		}
		if (priv->instance_ids->len > 0) {
			const gchar * const *tmp = (const gchar * const *) priv->instance_ids->pdata;
			g_variant_builder_add (&builder, "{sv}",
					       FWUPD_RESULT_KEY_INSTANCE_IDS,
					       g_variant_new_strv (tmp, priv->instance_ids->len));
		}
	}

	/* create an array with all the metadata in */
	if (priv->releases->len > 0) {
		g_autofree GVariant **children = NULL;
		children = g_new0 (GVariant *, priv->releases->len);
		for (guint i = 0; i < priv->releases->len; i++) {
			FwupdRelease *release = g_ptr_array_index (priv->releases, i);
			children[i] = fwupd_release_to_variant (release);
		}
		g_variant_builder_add (&builder, "{sv}",
				       FWUPD_RESULT_KEY_RELEASE,
				       g_variant_new_array (G_VARIANT_TYPE ("a{sv}"),
							    children,
							    priv->releases->len));
	}
	return g_variant_new ("a{sv}", &builder);
}

/**
 * fwupd_device_to_variant:
 * @self: a #FwupdDevice
 *
 * Serialize the device data omitting sensitive fields
 *
 * Returns: the serialized data, or %NULL for error
 *
 * Since: 1.0.0
 **/
GVariant *
fwupd_device_to_variant (FwupdDevice *self)
{
	return fwupd_device_to_variant_full (self, FWUPD_DEVICE_FLAG_NONE);
}

static void
fwupd_device_from_key_value (FwupdDevice *self, const gchar *key, GVariant *value)
{
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_RELEASE) == 0) {
		GVariantIter iter;
		GVariant *child;
		g_variant_iter_init (&iter, value);
		while ((child = g_variant_iter_next_value (&iter))) {
			g_autoptr(FwupdRelease) release = fwupd_release_from_variant (child);
			if (release != NULL)
				fwupd_device_add_release (self, release);
			g_variant_unref (child);
		}
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_DEVICE_ID) == 0) {
		fwupd_device_set_id (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_PARENT_DEVICE_ID) == 0) {
		fwupd_device_set_parent_id (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_COMPOSITE_ID) == 0) {
		fwupd_device_set_composite_id (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_FLAGS) == 0) {
		fwupd_device_set_flags (self, g_variant_get_uint64 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_CREATED) == 0) {
		fwupd_device_set_created (self, g_variant_get_uint64 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_MODIFIED) == 0) {
		fwupd_device_set_modified (self, g_variant_get_uint64 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_BUILD_DATE) == 0) {
		fwupd_device_set_version_build_date (self, g_variant_get_uint64 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_GUID) == 0) {
		g_autofree const gchar **guids = g_variant_get_strv (value, NULL);
		for (guint i = 0; guids != NULL && guids[i] != NULL; i++)
			fwupd_device_add_guid (self, guids[i]);
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_INSTANCE_IDS) == 0) {
		g_autofree const gchar **instance_ids = g_variant_get_strv (value, NULL);
		for (guint i = 0; instance_ids != NULL && instance_ids[i] != NULL; i++)
			fwupd_device_add_instance_id (self, instance_ids[i]);
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_ICON) == 0) {
		g_autofree const gchar **icons = g_variant_get_strv (value, NULL);
		for (guint i = 0; icons != NULL && icons[i] != NULL; i++)
			fwupd_device_add_icon (self, icons[i]);
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_NAME) == 0) {
		fwupd_device_set_name (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VENDOR) == 0) {
		fwupd_device_set_vendor (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VENDOR_ID) == 0) {
		g_auto(GStrv) vendor_ids = NULL;
		vendor_ids = g_strsplit (g_variant_get_string (value, NULL), "|", -1);
		for (guint i = 0; vendor_ids[i] != NULL; i++)
			fwupd_device_add_vendor_id (self, vendor_ids[i]);
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_SERIAL) == 0) {
		fwupd_device_set_serial (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_SUMMARY) == 0) {
		fwupd_device_set_summary (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_BRANCH) == 0) {
		fwupd_device_set_branch (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_DESCRIPTION) == 0) {
		fwupd_device_set_description (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_CHECKSUM) == 0) {
		const gchar *checksums = g_variant_get_string (value, NULL);
		if (checksums != NULL) {
			g_auto(GStrv) split = g_strsplit (checksums, ",", -1);
			for (guint i = 0; split[i] != NULL; i++)
				fwupd_device_add_checksum (self, split[i]);
		}
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_PLUGIN) == 0) {
		fwupd_device_set_plugin (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_PROTOCOL) == 0) {
		g_auto(GStrv) protocols = NULL;
		protocols = g_strsplit (g_variant_get_string (value, NULL), "|", -1);
		for (guint i = 0; protocols[i] != NULL; i++)
			fwupd_device_add_protocol (self, protocols[i]);
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION) == 0) {
		fwupd_device_set_version (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_LOWEST) == 0) {
		fwupd_device_set_version_lowest (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_BOOTLOADER) == 0) {
		fwupd_device_set_version_bootloader (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_FLASHES_LEFT) == 0) {
		fwupd_device_set_flashes_left (self, g_variant_get_uint32 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_INSTALL_DURATION) == 0) {
		fwupd_device_set_install_duration (self, g_variant_get_uint32 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_UPDATE_ERROR) == 0) {
		fwupd_device_set_update_error (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_UPDATE_MESSAGE) == 0) {
		fwupd_device_set_update_message (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_UPDATE_IMAGE) == 0) {
		fwupd_device_set_update_image (self, g_variant_get_string (value, NULL));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_UPDATE_STATE) == 0) {
		fwupd_device_set_update_state (self, g_variant_get_uint32 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_STATUS) == 0) {
		fwupd_device_set_status (self, g_variant_get_uint32 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_UPDATE_MESSAGE_KIND) == 0) {
		fwupd_device_set_update_message_kind (self, g_variant_get_uint32 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_FORMAT) == 0) {
		fwupd_device_set_version_format (self, g_variant_get_uint32 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_RAW) == 0) {
		fwupd_device_set_version_raw (self, g_variant_get_uint64 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_LOWEST_RAW) == 0) {
		fwupd_device_set_version_lowest_raw (self, g_variant_get_uint64 (value));
		return;
	}
	if (g_strcmp0 (key, FWUPD_RESULT_KEY_VERSION_BOOTLOADER_RAW) == 0) {
		fwupd_device_set_version_bootloader_raw (self, g_variant_get_uint64 (value));
		return;
	}
}

static void
fwupd_pad_kv_str (GString *str, const gchar *key, const gchar *value)
{
	/* ignore */
	if (key == NULL || value == NULL)
		return;
	g_string_append_printf (str, "  %s: ", key);
	for (gsize i = strlen (key); i < 20; i++)
		g_string_append (str, " ");
	g_string_append_printf (str, "%s\n", value);
}

static void
fwupd_pad_kv_unx (GString *str, const gchar *key, guint64 value)
{
	g_autoptr(GDateTime) date = NULL;
	g_autofree gchar *tmp = NULL;

	/* ignore */
	if (value == 0)
		return;

	date = g_date_time_new_from_unix_utc ((gint64) value);
	tmp = g_date_time_format (date, "%F");
	fwupd_pad_kv_str (str, key, tmp);
}

static void
fwupd_pad_kv_dfl (GString *str, const gchar *key, guint64 device_flags)
{
	g_autoptr(GString) tmp = g_string_new ("");
	for (guint i = 0; i < 64; i++) {
		if ((device_flags & ((guint64) 1 << i)) == 0)
			continue;
		g_string_append_printf (tmp, "%s|",
					fwupd_device_flag_to_string ((guint64) 1 << i));
	}
	if (tmp->len == 0) {
		g_string_append (tmp, fwupd_device_flag_to_string (0));
	} else {
		g_string_truncate (tmp, tmp->len - 1);
	}
	fwupd_pad_kv_str (str, key, tmp->str);
}

static void
fwupd_pad_kv_int (GString *str, const gchar *key, guint32 value)
{
	g_autofree gchar *tmp = NULL;

	/* ignore */
	if (value == 0)
		return;
	tmp = g_strdup_printf("%" G_GUINT32_FORMAT, value);
	fwupd_pad_kv_str (str, key, tmp);
}

/**
 * fwupd_device_get_update_state:
 * @self: a #FwupdDevice
 *
 * Gets the update state.
 *
 * Returns: the update state, or %FWUPD_UPDATE_STATE_UNKNOWN if unset
 *
 * Since: 0.9.8
 **/
FwupdUpdateState
fwupd_device_get_update_state (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FWUPD_UPDATE_STATE_UNKNOWN);
	return priv->update_state;
}

/**
 * fwupd_device_set_update_state:
 * @self: a #FwupdDevice
 * @update_state: the state, e.g. %FWUPD_UPDATE_STATE_PENDING
 *
 * Sets the update state.
 *
 * Since: 0.9.8
 **/
void
fwupd_device_set_update_state (FwupdDevice *self, FwupdUpdateState update_state)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	if (priv->update_state == update_state)
		return;
	priv->update_state = update_state;
	g_object_notify (G_OBJECT (self), "update-state");
}

/**
 * fwupd_device_get_version_format:
 * @self: a #FwupdDevice
 *
 * Gets the update state.
 *
 * Returns: the update state, or %FWUPD_VERSION_FORMAT_UNKNOWN if unset
 *
 * Since: 1.2.9
 **/
FwupdVersionFormat
fwupd_device_get_version_format (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), FWUPD_VERSION_FORMAT_UNKNOWN);
	return priv->version_format;
}

/**
 * fwupd_device_set_version_format:
 * @self: a #FwupdDevice
 * @version_format: the state, e.g. %FWUPD_VERSION_FORMAT_PENDING
 *
 * Sets the update state.
 *
 * Since: 1.2.9
 **/
void
fwupd_device_set_version_format (FwupdDevice *self, FwupdVersionFormat version_format)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->version_format = version_format;
}

/**
 * fwupd_device_get_version_raw:
 * @self: a #FwupdDevice
 *
 * Gets the raw version number from the hardware before converted to a string.
 *
 * Returns: the hardware version, or 0 if unset
 *
 * Since: 1.3.6
 **/
guint64
fwupd_device_get_version_raw (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->version_raw;
}

/**
 * fwupd_device_set_version_raw:
 * @self: a #FwupdDevice
 * @version_raw: the raw hardware version
 *
 * Sets the raw version number from the hardware before converted to a string.
 *
 * Since: 1.3.6
 **/
void
fwupd_device_set_version_raw (FwupdDevice *self, guint64 version_raw)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->version_raw = version_raw;
}

/**
 * fwupd_device_get_version_build_date:
 * @self: a #FwupdDevice
 *
 * Gets the date when the firmware was built.
 *
 * Returns: the UNIX time, or 0 if unset
 *
 * Since: 1.6.2
 **/
guint64
fwupd_device_get_version_build_date (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->version_build_date;
}

/**
 * fwupd_device_set_version_build_date:
 * @self: a #FwupdDevice
 * @version_build_date: the UNIX time
 *
 * Sets the date when the firmware was built.
 *
 * Since: 1.6.2
 **/
void
fwupd_device_set_version_build_date (FwupdDevice *self, guint64 version_build_date)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	priv->version_build_date = version_build_date;
}

/**
 * fwupd_device_get_update_message:
 * @self: a #FwupdDevice
 *
 * Gets the update message.
 *
 * Returns: the update message, or %NULL if unset
 *
 * Since: 1.2.4
 **/
const gchar *
fwupd_device_get_update_message (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->update_message;
}

/**
 * fwupd_device_set_update_message:
 * @self: a #FwupdDevice
 * @update_message: (nullable): the update message string
 *
 * Sets the update message.
 *
 * Since: 1.2.4
 **/
void
fwupd_device_set_update_message (FwupdDevice *self, const gchar *update_message)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->update_message, update_message) == 0)
		return;

	g_free (priv->update_message);
	priv->update_message = g_strdup (update_message);
	g_object_notify (G_OBJECT (self), "update-message");
}

/**
 * fwupd_device_get_update_image:
 * @self: a #FwupdDevice
 *
 * Gets the update image.
 *
 * Returns: the update image URL, or %NULL if unset
 *
 * Since: 1.4.5
 **/
const gchar *
fwupd_device_get_update_image (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->update_image;
}

/**
 * fwupd_device_set_update_image:
 * @self: a #FwupdDevice
 * @update_image: (nullable): the update image URL
 *
 * Sets the update image.
 *
 * Since: 1.4.5
 **/
void
fwupd_device_set_update_image (FwupdDevice *self, const gchar *update_image)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->update_image, update_image) == 0)
		return;

	g_free (priv->update_image);
	priv->update_image = g_strdup (update_image);
	g_object_notify (G_OBJECT (self), "update-image");
}

/**
 * fwupd_device_get_update_error:
 * @self: a #FwupdDevice
 *
 * Gets the update error.
 *
 * Returns: the update error, or %NULL if unset
 *
 * Since: 0.9.8
 **/
const gchar *
fwupd_device_get_update_error (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->update_error;
}

/**
 * fwupd_device_set_update_error:
 * @self: a #FwupdDevice
 * @update_error: (nullable): the update error string
 *
 * Sets the update error.
 *
 * Since: 0.9.8
 **/
void
fwupd_device_set_update_error (FwupdDevice *self, const gchar *update_error)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->update_error, update_error) == 0)
		return;

	g_free (priv->update_error);
	priv->update_error = g_strdup (update_error);
}

/**
 * fwupd_device_get_release_default:
 * @self: a #FwupdDevice
 *
 * Gets the default release for this device.
 *
 * Returns: (transfer none): the #FwupdRelease, or %NULL if not set
 *
 * Since: 0.9.8
 **/
FwupdRelease *
fwupd_device_get_release_default (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	if (priv->releases->len == 0)
		return NULL;
	return FWUPD_RELEASE (g_ptr_array_index (priv->releases, 0));
}

/**
 * fwupd_device_get_releases:
 * @self: a #FwupdDevice
 *
 * Gets all the releases for this device.
 *
 * Returns: (transfer none) (element-type FwupdRelease): array of releases
 *
 * Since: 0.9.8
 **/
GPtrArray *
fwupd_device_get_releases (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);
	return priv->releases;
}

/**
 * fwupd_device_add_release:
 * @self: a #FwupdDevice
 * @release: a release
 *
 * Adds a release for this device.
 *
 * Since: 0.9.8
 **/
void
fwupd_device_add_release (FwupdDevice *self, FwupdRelease *release)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_ptr_array_add (priv->releases, g_object_ref (release));
}

/**
 * fwupd_device_get_update_message_kind:
 * @self: a #FwupdDevice
 *
 * Gets the current message kind.
 *
 * Returns: the update_message_kind value, e.g. %FWUPD_DEVICE_MESSAGE_KIND_POST
 *
 * Since: 1.6.2
 **/
FwupdDeviceMessageKind
fwupd_device_get_update_message_kind (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->update_message_kind;
}

/**
 * fwupd_device_set_update_message_kind:
 * @self: a #FwupdDevice
 * @update_message_kind: the update_message_kind value, e.g. %FWUPD_DEVICE_MESSAGE_KIND_POST
 *
 * Sets the current message kind.
 *
 * Since: 1.6.2
 **/
void
fwupd_device_set_update_message_kind (FwupdDevice *self, FwupdDeviceMessageKind update_message_kind)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	if (priv->update_message_kind == update_message_kind)
		return;
	priv->update_message_kind = update_message_kind;
	g_object_notify (G_OBJECT (self), "update-message-kind");
}

/**
 * fwupd_device_get_status:
 * @self: a #FwupdDevice
 *
 * Returns what the device is currently doing.
 *
 * Returns: the status value, e.g. %FWUPD_STATUS_DEVICE_WRITE
 *
 * Since: 1.4.0
 **/
FwupdStatus
fwupd_device_get_status (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self), 0);
	return priv->status;
}

/**
 * fwupd_device_set_status:
 * @self: a #FwupdDevice
 * @status: the status value, e.g. %FWUPD_STATUS_DEVICE_WRITE
 *
 * Sets what the device is currently doing.
 *
 * Since: 1.4.0
 **/
void
fwupd_device_set_status (FwupdDevice *self, FwupdStatus status)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FWUPD_IS_DEVICE (self));
	if (priv->status == status)
		return;
	priv->status = status;
	g_object_notify (G_OBJECT (self), "status");
}

static void
fwupd_pad_kv_ups (GString *str, const gchar *key, FwupdUpdateState value)
{
	if (value == FWUPD_UPDATE_STATE_UNKNOWN)
		return;
	fwupd_pad_kv_str (str, key, fwupd_update_state_to_string (value));
}

static void
fwupd_device_json_add_string (JsonBuilder *builder, const gchar *key, const gchar *str)
{
	if (str == NULL)
		return;
	json_builder_set_member_name (builder, key);
	json_builder_add_string_value (builder, str);
}

static void
fwupd_device_json_add_int (JsonBuilder *builder, const gchar *key, guint64 num)
{
	if (num == 0)
		return;
	json_builder_set_member_name (builder, key);
	json_builder_add_int_value (builder, num);
}

/**
 * fwupd_device_to_json:
 * @self: a #FwupdDevice
 * @builder: a JSON builder
 *
 * Adds a fwupd device to a JSON builder
 *
 * Since: 1.2.6
 **/
void
fwupd_device_to_json (FwupdDevice *self, JsonBuilder *builder)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FWUPD_IS_DEVICE (self));
	g_return_if_fail (builder != NULL);

	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_NAME, priv->name);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_DEVICE_ID, priv->id);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_PARENT_DEVICE_ID,
				      priv->parent_id);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_COMPOSITE_ID,
				      priv->composite_id);
	if (priv->guids->len > 0) {
		json_builder_set_member_name (builder, FWUPD_RESULT_KEY_GUID);
		json_builder_begin_array (builder);
		for (guint i = 0; i < priv->guids->len; i++) {
			const gchar *guid = g_ptr_array_index (priv->guids, i);
			json_builder_add_string_value (builder, guid);
		}
		json_builder_end_array (builder);
	}
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_SERIAL, priv->serial);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_SUMMARY, priv->summary);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_DESCRIPTION, priv->description);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_BRANCH, priv->branch);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_PLUGIN, priv->plugin);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_PROTOCOL, priv->protocol);
	if (priv->protocols->len > 1) { /* --> 0 when bumping API */
		json_builder_set_member_name (builder, "VendorIds");
		json_builder_begin_array (builder);
		for (guint i = 0; i < priv->protocols->len; i++) {
			const gchar *tmp = g_ptr_array_index (priv->protocols, i);
			json_builder_add_string_value (builder, tmp);
		}
		json_builder_end_array (builder);
	}
	if (priv->flags != FWUPD_DEVICE_FLAG_NONE) {
		json_builder_set_member_name (builder, FWUPD_RESULT_KEY_FLAGS);
		json_builder_begin_array (builder);
		for (guint i = 0; i < 64; i++) {
			const gchar *tmp;
			if ((priv->flags & ((guint64) 1 << i)) == 0)
				continue;
			tmp = fwupd_device_flag_to_string ((guint64) 1 << i);
			json_builder_add_string_value (builder, tmp);
		}
		json_builder_end_array (builder);
	}
	if (priv->checksums->len > 0) {
		json_builder_set_member_name (builder, "Checksums");
		json_builder_begin_array (builder);
		for (guint i = 0; i < priv->checksums->len; i++) {
			const gchar *checksum = g_ptr_array_index (priv->checksums, i);
			json_builder_add_string_value (builder, checksum);
		}
		json_builder_end_array (builder);
	}
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_VENDOR, priv->vendor);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_VENDOR_ID, priv->vendor_id);
	if (priv->vendor_ids->len > 1) { /* --> 0 when bumping API */
		json_builder_set_member_name (builder, "VendorIds");
		json_builder_begin_array (builder);
		for (guint i = 0; i < priv->vendor_ids->len; i++) {
			const gchar *tmp = g_ptr_array_index (priv->vendor_ids, i);
			json_builder_add_string_value (builder, tmp);
		}
		json_builder_end_array (builder);
	}
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_VERSION, priv->version);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_VERSION_LOWEST, priv->version_lowest);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_VERSION_BOOTLOADER, priv->version_bootloader);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_VERSION_FORMAT,
				      fwupd_version_format_to_string (priv->version_format));
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_FLASHES_LEFT, priv->flashes_left);
	if (priv->version_raw > 0)
		fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_VERSION_RAW, priv->version_raw);
	if (priv->version_lowest_raw > 0)
		fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_VERSION_LOWEST_RAW, priv->version_lowest_raw);
	if (priv->version_bootloader_raw > 0)
		fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_VERSION_BOOTLOADER_RAW, priv->version_bootloader_raw);
	if (priv->version_build_date > 0)
		fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_VERSION_BUILD_DATE, priv->version_build_date);
	if (priv->icons->len > 0) {
		json_builder_set_member_name (builder, "Icons");
		json_builder_begin_array (builder);
		for (guint i = 0; i < priv->icons->len; i++) {
			const gchar *icon = g_ptr_array_index (priv->icons, i);
			json_builder_add_string_value (builder, icon);
		}
		json_builder_end_array (builder);
	}
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_INSTALL_DURATION, priv->install_duration);
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_CREATED, priv->created);
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_MODIFIED, priv->modified);
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_UPDATE_STATE, priv->update_state);
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_STATUS, priv->status);
	fwupd_device_json_add_int (builder, FWUPD_RESULT_KEY_UPDATE_MESSAGE_KIND, priv->update_message_kind);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_UPDATE_ERROR, priv->update_error);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_UPDATE_MESSAGE, priv->update_message);
	fwupd_device_json_add_string (builder, FWUPD_RESULT_KEY_UPDATE_IMAGE, priv->update_image);
	if (priv->releases->len > 0) {
		json_builder_set_member_name (builder, "Releases");
		json_builder_begin_array (builder);
		for (guint i = 0; i < priv->releases->len; i++) {
			FwupdRelease *release = g_ptr_array_index (priv->releases, i);
			json_builder_begin_object (builder);
			fwupd_release_to_json (release, builder);
			json_builder_end_object (builder);
		}
		json_builder_end_array (builder);
	}
}

static gchar *
fwupd_device_verstr_raw (guint64 value_raw)
{
	if (value_raw > 0xffffffff) {
		return g_strdup_printf ("0x%08x%08x",
				        (guint) (value_raw >> 32),
				        (guint) (value_raw & 0xffffffff));
	}
	return g_strdup_printf ("0x%08x", (guint) value_raw);
}

typedef struct {
	gchar		*guid;
	gchar		*instance_id;
} FwupdDeviceGuidHelper;

static void
fwupd_device_guid_helper_new (FwupdDeviceGuidHelper *helper)
{
	g_free (helper->guid);
	g_free (helper->instance_id);
	g_free (helper);
}

static FwupdDeviceGuidHelper *
fwupd_device_guid_helper_array_find (GPtrArray *array, const gchar *guid)
{
	for (guint i = 0; i < array->len; i++) {
		FwupdDeviceGuidHelper *helper = g_ptr_array_index (array, i);
		if (g_strcmp0 (helper->guid, guid) == 0)
			return helper;
	}
	return NULL;
}

/**
 * fwupd_device_to_string:
 * @self: a #FwupdDevice
 *
 * Builds a text representation of the object.
 *
 * Returns: text, or %NULL for invalid
 *
 * Since: 0.9.3
 **/
gchar *
fwupd_device_to_string (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	GString *str;
	g_autoptr(GPtrArray) guid_helpers = NULL;

	g_return_val_if_fail (FWUPD_IS_DEVICE (self), NULL);

	str = g_string_new ("");
	if (priv->name != NULL)
		g_string_append_printf (str, "%s\n", priv->name);
	else
		str = g_string_append (str, "Unknown Device\n");
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_DEVICE_ID, priv->id);
	if (g_strcmp0 (priv->composite_id, priv->parent_id) != 0)
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_PARENT_DEVICE_ID, priv->parent_id);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_COMPOSITE_ID, priv->composite_id);
	if (priv->status != FWUPD_STATUS_UNKNOWN) {
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_STATUS,
				  fwupd_status_to_string (priv->status));
	}

	/* show InstanceIDs optionally mapped to GUIDs, and also "standalone" GUIDs */
	guid_helpers = g_ptr_array_new_with_free_func ((GDestroyNotify) fwupd_device_guid_helper_new);
	for (guint i = 0; i < priv->instance_ids->len; i++) {
		FwupdDeviceGuidHelper *helper = g_new0 (FwupdDeviceGuidHelper, 1);
		const gchar *instance_id = g_ptr_array_index (priv->instance_ids, i);
		helper->guid = fwupd_guid_hash_string (instance_id);
		helper->instance_id = g_strdup (instance_id);
		g_ptr_array_add (guid_helpers, helper);
	}
	for (guint i = 0; i < priv->guids->len; i++) {
		const gchar *guid = g_ptr_array_index (priv->guids, i);
		if (fwupd_device_guid_helper_array_find (guid_helpers, guid) == NULL) {
			FwupdDeviceGuidHelper *helper = g_new0 (FwupdDeviceGuidHelper, 1);
			helper->guid = g_strdup (guid);
			g_ptr_array_add (guid_helpers, helper);
		}
	}
	for (guint i = 0; i < guid_helpers->len; i++) {
		FwupdDeviceGuidHelper *helper = g_ptr_array_index (guid_helpers, i);
		g_autoptr(GString) tmp = g_string_new (helper->guid);
		if (helper->instance_id != NULL)
			g_string_append_printf (tmp, " ← %s", helper->instance_id);
		if (!fwupd_device_has_guid (self, helper->guid))
			g_string_append (tmp, " ⚠");
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_GUID, tmp->str);
	}

	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_SERIAL, priv->serial);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_SUMMARY, priv->summary);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_DESCRIPTION, priv->description);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_BRANCH, priv->branch);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_PLUGIN, priv->plugin);
	for (guint i = 0; i < priv->protocols->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv->protocols, i);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_PROTOCOL, tmp);
	}
	fwupd_pad_kv_dfl (str, FWUPD_RESULT_KEY_FLAGS, priv->flags);
	for (guint i = 0; i < priv->checksums->len; i++) {
		const gchar *checksum = g_ptr_array_index (priv->checksums, i);
		g_autofree gchar *checksum_display = fwupd_checksum_format_for_display (checksum);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_CHECKSUM, checksum_display);
	}
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VENDOR, priv->vendor);
	for (guint i = 0; i < priv->vendor_ids->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv->vendor_ids, i);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VENDOR_ID, tmp);
	}
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION, priv->version);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION_LOWEST, priv->version_lowest);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION_BOOTLOADER, priv->version_bootloader);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION_FORMAT,
			  fwupd_version_format_to_string (priv->version_format));
	if (priv->flashes_left < 2)
		fwupd_pad_kv_int (str, FWUPD_RESULT_KEY_FLASHES_LEFT, priv->flashes_left);
	if (priv->version_raw > 0) {
		g_autofree gchar *tmp = fwupd_device_verstr_raw (priv->version_raw);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION_RAW, tmp);
	}
	if (priv->version_lowest_raw > 0) {
		g_autofree gchar *tmp = fwupd_device_verstr_raw (priv->version_lowest_raw);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION_LOWEST_RAW, tmp);
	}
	if (priv->version_build_date > 0) {
		fwupd_pad_kv_unx (str, FWUPD_RESULT_KEY_VERSION_BUILD_DATE,
				  priv->version_build_date);
	}
	if (priv->version_bootloader_raw > 0) {
		g_autofree gchar *tmp = fwupd_device_verstr_raw (priv->version_bootloader_raw);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_VERSION_BOOTLOADER_RAW, tmp);
	}
	if (priv->icons->len > 0) {
		g_autoptr(GString) tmp = g_string_new (NULL);
		for (guint i = 0; i < priv->icons->len; i++) {
			const gchar *icon = g_ptr_array_index (priv->icons, i);
			g_string_append_printf (tmp, "%s,", icon);
		}
		if (tmp->len > 1)
			g_string_truncate (tmp, tmp->len - 1);
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_ICON, tmp->str);
	}
	fwupd_pad_kv_int (str, FWUPD_RESULT_KEY_INSTALL_DURATION, priv->install_duration);
	fwupd_pad_kv_unx (str, FWUPD_RESULT_KEY_CREATED, priv->created);
	fwupd_pad_kv_unx (str, FWUPD_RESULT_KEY_MODIFIED, priv->modified);
	fwupd_pad_kv_ups (str, FWUPD_RESULT_KEY_UPDATE_STATE, priv->update_state);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_UPDATE_ERROR, priv->update_error);
	if (priv->update_message_kind != FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN) {
		fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_UPDATE_MESSAGE_KIND,
				  fwupd_device_message_kind_to_string (priv->update_message_kind));
	}
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_UPDATE_MESSAGE, priv->update_message);
	fwupd_pad_kv_str (str, FWUPD_RESULT_KEY_UPDATE_IMAGE, priv->update_image);
	for (guint i = 0; i < priv->releases->len; i++) {
		FwupdRelease *release = g_ptr_array_index (priv->releases, i);
		g_autofree gchar *tmp = fwupd_release_to_string (release);
		g_string_append_printf (str, "  \n  [%s]\n%s",
					FWUPD_RESULT_KEY_RELEASE, tmp);
	}

	return g_string_free (str, FALSE);
}

static void
fwupd_device_get_property (GObject *object, guint prop_id,
			   GValue *value, GParamSpec *pspec)
{
	FwupdDevice *self = FWUPD_DEVICE (object);
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	switch (prop_id) {
	case PROP_VERSION_FORMAT:
		g_value_set_uint (value, priv->version_format);
		break;
	case PROP_FLAGS:
		g_value_set_uint64 (value, priv->flags);
		break;
	case PROP_PROTOCOL:
		g_value_set_string (value, priv->protocol);
		break;
	case PROP_UPDATE_MESSAGE:
		g_value_set_string (value, priv->update_message);
		break;
	case PROP_UPDATE_IMAGE:
		g_value_set_string (value, priv->update_image);
		break;
	case PROP_STATUS:
		g_value_set_uint (value, priv->status);
		break;
	case PROP_UPDATE_MESSAGE_KIND:
		g_value_set_uint (value, priv->update_message_kind);
		break;
	case PROP_PARENT:
		g_value_set_object (value, priv->parent);
		break;
	case PROP_UPDATE_STATE:
		g_value_set_uint (value, priv->update_state);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fwupd_device_set_property (GObject *object, guint prop_id,
			   const GValue *value, GParamSpec *pspec)
{
	FwupdDevice *self = FWUPD_DEVICE (object);
	switch (prop_id) {
	case PROP_VERSION_FORMAT:
		fwupd_device_set_version_format (self, g_value_get_uint (value));
		break;
	case PROP_FLAGS:
		fwupd_device_set_flags (self, g_value_get_uint64 (value));
		break;
	case PROP_PROTOCOL:
		fwupd_device_add_protocol (self, g_value_get_string (value));
		break;
	case PROP_UPDATE_MESSAGE:
		fwupd_device_set_update_message (self, g_value_get_string (value));
		break;
	case PROP_UPDATE_IMAGE:
		fwupd_device_set_update_image (self, g_value_get_string (value));
		break;
	case PROP_STATUS:
		fwupd_device_set_status (self, g_value_get_uint (value));
		break;
	case PROP_UPDATE_MESSAGE_KIND:
		fwupd_device_set_update_message_kind (self, g_value_get_uint (value));
		break;
	case PROP_PARENT:
		fwupd_device_set_parent (self, g_value_get_object (value));
		break;
	case PROP_UPDATE_STATE:
		fwupd_device_set_update_state (self, g_value_get_uint (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fwupd_device_class_init (FwupdDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GParamSpec *pspec;

	object_class->finalize = fwupd_device_finalize;
	object_class->get_property = fwupd_device_get_property;
	object_class->set_property = fwupd_device_set_property;

	pspec = g_param_spec_uint ("version-format", NULL, NULL,
				   FWUPD_VERSION_FORMAT_UNKNOWN,
				   FWUPD_VERSION_FORMAT_LAST,
				   FWUPD_VERSION_FORMAT_UNKNOWN,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_VERSION_FORMAT, pspec);

	pspec = g_param_spec_uint64 ("flags", NULL, NULL,
				     FWUPD_DEVICE_FLAG_NONE,
				     FWUPD_DEVICE_FLAG_UNKNOWN,
				     FWUPD_DEVICE_FLAG_NONE,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_FLAGS, pspec);

	pspec = g_param_spec_string ("protocol", NULL, NULL, NULL,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_PROTOCOL, pspec);

	pspec = g_param_spec_uint ("status", NULL, NULL,
				   FWUPD_STATUS_UNKNOWN,
				   FWUPD_STATUS_LAST,
				   FWUPD_STATUS_UNKNOWN,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_STATUS, pspec);

	pspec = g_param_spec_uint ("update-message-kind", NULL, NULL,
				   FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN,
				   FWUPD_DEVICE_MESSAGE_KIND_LAST,
				   FWUPD_DEVICE_MESSAGE_KIND_UNKNOWN,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_UPDATE_MESSAGE_KIND, pspec);

	pspec = g_param_spec_object ("parent", NULL, NULL,
				     FWUPD_TYPE_DEVICE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_PARENT, pspec);

	pspec = g_param_spec_uint ("update-state", NULL, NULL,
				   FWUPD_UPDATE_STATE_UNKNOWN,
				   FWUPD_UPDATE_STATE_LAST,
				   FWUPD_UPDATE_STATE_UNKNOWN,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_UPDATE_STATE, pspec);

	pspec = g_param_spec_string ("update-message", NULL, NULL, NULL,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_UPDATE_MESSAGE, pspec);

	pspec = g_param_spec_string ("update-image", NULL, NULL, NULL,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_UPDATE_IMAGE, pspec);
}

static void
fwupd_device_init (FwupdDevice *self)
{
	FwupdDevicePrivate *priv = GET_PRIVATE (self);
	priv->guids = g_ptr_array_new_with_free_func (g_free);
	priv->instance_ids = g_ptr_array_new_with_free_func (g_free);
	priv->icons = g_ptr_array_new_with_free_func (g_free);
	priv->checksums = g_ptr_array_new_with_free_func (g_free);
	priv->vendor_ids = g_ptr_array_new_with_free_func (g_free);
	priv->protocols = g_ptr_array_new_with_free_func (g_free);
	priv->children = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	priv->releases = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
}

static void
fwupd_device_finalize (GObject *object)
{
	FwupdDevice *self = FWUPD_DEVICE (object);
	FwupdDevicePrivate *priv = GET_PRIVATE (self);

	if (priv->parent != NULL)
		g_object_remove_weak_pointer (G_OBJECT (priv->parent), (gpointer *) &priv->parent);
	for (guint i = 0; i < priv->children->len; i++) {
		FwupdDevice *child = g_ptr_array_index (priv->children, i);
		g_object_weak_unref (G_OBJECT (child),
				     fwupd_device_child_finalized_cb,
				     self);
	}

	g_free (priv->description);
	g_free (priv->id);
	g_free (priv->parent_id);
	g_free (priv->composite_id);
	g_free (priv->name);
	g_free (priv->serial);
	g_free (priv->summary);
	g_free (priv->branch);
	g_free (priv->vendor);
	g_free (priv->vendor_id);
	g_free (priv->plugin);
	g_free (priv->protocol);
	g_free (priv->update_error);
	g_free (priv->update_message);
	g_free (priv->update_image);
	g_free (priv->version);
	g_free (priv->version_lowest);
	g_free (priv->version_bootloader);
	g_ptr_array_unref (priv->guids);
	g_ptr_array_unref (priv->vendor_ids);
	g_ptr_array_unref (priv->protocols);
	g_ptr_array_unref (priv->instance_ids);
	g_ptr_array_unref (priv->icons);
	g_ptr_array_unref (priv->checksums);
	g_ptr_array_unref (priv->children);
	g_ptr_array_unref (priv->releases);

	G_OBJECT_CLASS (fwupd_device_parent_class)->finalize (object);
}

static void
fwupd_device_set_from_variant_iter (FwupdDevice *self, GVariantIter *iter)
{
	GVariant *value;
	const gchar *key;
	while (g_variant_iter_next (iter, "{&sv}", &key, &value)) {
		fwupd_device_from_key_value (self, key, value);
		g_variant_unref (value);
	}
}

/**
 * fwupd_device_from_variant:
 * @value: the serialized data
 *
 * Creates a new device using serialized data.
 *
 * Returns: (transfer full): a new #FwupdDevice, or %NULL if @value was invalid
 *
 * Since: 1.0.0
 **/
FwupdDevice *
fwupd_device_from_variant (GVariant *value)
{
	FwupdDevice *dev = NULL;
	const gchar *type_string;
	g_autoptr(GVariantIter) iter = NULL;

	/* format from GetDetails */
	type_string = g_variant_get_type_string (value);
	if (g_strcmp0 (type_string, "(a{sv})") == 0) {
		dev = fwupd_device_new ();
		g_variant_get (value, "(a{sv})", &iter);
		fwupd_device_set_from_variant_iter (dev, iter);
	} else if (g_strcmp0 (type_string, "a{sv}") == 0) {
		dev = fwupd_device_new ();
		g_variant_get (value, "a{sv}", &iter);
		fwupd_device_set_from_variant_iter (dev, iter);
	} else {
		g_warning ("type %s not known", type_string);
	}
	return dev;
}

/**
 * fwupd_device_array_ensure_parents:
 * @devices: (element-type FwupdDevice): devices
 *
 * Sets the parent object on all devices in the array using the parent-id.
 *
 * Since: 1.3.7
 **/
void
fwupd_device_array_ensure_parents (GPtrArray *devices)
{
	g_autoptr(GHashTable) devices_by_id = NULL;

	/* create hash of ID->FwupdDevice */
	devices_by_id = g_hash_table_new (g_str_hash, g_str_equal);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		if (fwupd_device_get_id (dev) == NULL)
			continue;
		g_hash_table_insert (devices_by_id,
				     (gpointer) fwupd_device_get_id (dev),
				     (gpointer) dev);
	}

	/* set the parent on each child */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		const gchar *parent_id = fwupd_device_get_parent_id (dev);
		if (parent_id != NULL) {
			FwupdDevice *dev_tmp;
			dev_tmp = g_hash_table_lookup (devices_by_id, parent_id);
			if (dev_tmp != NULL)
				fwupd_device_set_parent (dev, dev_tmp);
		}
	}
}

/**
 * fwupd_device_array_from_variant:
 * @value: the serialized data
 *
 * Creates an array of new devices using serialized data.
 *
 * Returns: (transfer container) (element-type FwupdDevice): devices, or %NULL if @value was invalid
 *
 * Since: 1.2.10
 **/
GPtrArray *
fwupd_device_array_from_variant (GVariant *value)
{
	GPtrArray *array = NULL;
	gsize sz;
	g_autoptr(GVariant) untuple = NULL;

	array = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	untuple = g_variant_get_child_value (value, 0);
	sz = g_variant_n_children (untuple);
	for (guint i = 0; i < sz; i++) {
		FwupdDevice *dev;
		g_autoptr(GVariant) data = NULL;
		data = g_variant_get_child_value (untuple, i);
		dev = fwupd_device_from_variant (data);
		if (dev == NULL)
			continue;
		g_ptr_array_add (array, dev);
	}

	/* set the parent on each child */
	fwupd_device_array_ensure_parents (array);
	return array;
}

/**
 * fwupd_device_compare:
 * @self1: a device
 * @self2: a different device
 *
 * Comparison function for comparing two device objects.
 *
 * Returns: negative, 0 or positive
 *
 * Since: 1.1.1
 **/
gint
fwupd_device_compare (FwupdDevice *self1, FwupdDevice *self2)
{
	FwupdDevicePrivate *priv1 = GET_PRIVATE (self1);
	FwupdDevicePrivate *priv2 = GET_PRIVATE (self2);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self1), 0);
	g_return_val_if_fail (FWUPD_IS_DEVICE (self2), 0);
	return g_strcmp0 (priv1->id, priv2->id);
}

/**
 * fwupd_device_new:
 *
 * Creates a new device.
 *
 * Returns: a new #FwupdDevice
 *
 * Since: 0.9.3
 **/
FwupdDevice *
fwupd_device_new (void)
{
	FwupdDevice *self;
	self = g_object_new (FWUPD_TYPE_DEVICE, NULL);
	return FWUPD_DEVICE (self);
}
