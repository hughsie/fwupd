/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#define G_LOG_DOMAIN				"FuEngine"

#include "config.h"

#include <gio/gio.h>
#ifdef HAVE_GIO_UNIX
#include <gio/gunixinputstream.h>
#endif
#include <glib-object.h>
#include <string.h>
#ifdef HAVE_UTSNAME_H
#include <sys/utsname.h>
#endif
#include <errno.h>

#include "fwupd-common-private.h"
#include "fwupd-enums-private.h"
#include "fwupd-error.h"
#include "fwupd-release-private.h"
#include "fwupd-remote-private.h"
#include "fwupd-resources.h"
#include "fwupd-security-attr-private.h"

#include "fu-backend.h"
#include "fu-cabinet.h"
#include "fu-common-cab.h"
#include "fu-common.h"
#include "fu-config.h"
#include "fu-context-private.h"
#include "fu-debug.h"
#include "fu-device-list.h"
#include "fu-device-private.h"
#include "fu-ifd-firmware.h"
#include "fu-ifd-bios.h"
#include "fu-efi-firmware-file.h"
#include "fu-efi-firmware-filesystem.h"
#include "fu-efi-firmware-section.h"
#include "fu-efi-firmware-volume.h"
#include "fu-engine.h"
#include "fu-engine-helper.h"
#include "fu-engine-request.h"
#include "fu-idle.h"
#include "fu-kenv.h"
#include "fu-keyring-utils.h"
#include "fu-hash.h"
#include "fu-history.h"
#include "fu-mutex.h"
#include "fu-plugin.h"
#include "fu-plugin-list.h"
#include "fu-plugin-private.h"
#include "fu-quirks.h"
#include "fu-remote-list.h"
#include "fu-security-attr.h"
#include "fu-security-attrs-private.h"
#include "fu-udev-device-private.h"
#include "fu-version.h"

#ifdef HAVE_GUDEV
#include "fu-udev-backend.h"
#endif
#ifdef HAVE_GUSB
#include "fu-usb-backend.h"
#endif
#ifdef HAVE_BLUEZ
#include "fu-bluez-backend.h"
#endif

#include "fu-dfu-firmware.h"
#include "fu-dfuse-firmware.h"
#include "fu-fmap-firmware.h"
#include "fu-ihex-firmware.h"
#include "fu-srec-firmware.h"

/* only needed until we hard depend on jcat 0.1.3 */
#include <libjcat/jcat-version.h>

#ifdef HAVE_SYSTEMD
#include "fu-systemd.h"
#endif

static void fu_engine_finalize	 (GObject *obj);
static void fu_engine_ensure_security_attrs	(FuEngine *self);

struct _FuEngine
{
	GObject			 parent_instance;
	FuAppFlags		 app_flags;
	GPtrArray		*backends;
	FuConfig		*config;
	FuRemoteList		*remote_list;
	FuDeviceList		*device_list;
	FwupdStatus		 status;
	gboolean		 tainted;
	guint			 percentage;
	FuHistory		*history;
	FuIdle			*idle;
	XbSilo			*silo;
	guint			 coldplug_id;
	FuPluginList		*plugin_list;
	GPtrArray		*plugin_filter;
	FuContext		*ctx;
	GHashTable		*runtime_versions;
	GHashTable		*compile_versions;
	GHashTable		*approved_firmware;	/* (nullable) */
	GHashTable		*blocked_firmware;	/* (nullable) */
	gchar			*host_machine_id;
	JcatContext		*jcat_context;
	gboolean		 loaded;
	gchar			*host_security_id;
	FuSecurityAttrs		*host_security_attrs;
};

enum {
	SIGNAL_CHANGED,
	SIGNAL_DEVICE_ADDED,
	SIGNAL_DEVICE_REMOVED,
	SIGNAL_DEVICE_CHANGED,
	SIGNAL_STATUS_CHANGED,
	SIGNAL_PERCENTAGE_CHANGED,
	SIGNAL_LAST
};

static guint signals[SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE (FuEngine, fu_engine, G_TYPE_OBJECT)

static void
fu_engine_emit_changed (FuEngine *self)
{
	g_signal_emit (self, signals[SIGNAL_CHANGED], 0);
	fu_engine_idle_reset (self);

	/* update the motd */
	if (self->loaded &&
	    fu_config_get_update_motd (self->config)) {
		g_autoptr(GError) error_local = NULL;
		if (!fu_engine_update_motd (self, &error_local))
			g_debug ("failed to update MOTD: %s",
				 error_local->message);
	}
}

static void
fu_engine_emit_device_changed (FuEngine *self, FuDevice *device)
{
	/* invalidate host security attributes */
	g_clear_pointer (&self->host_security_id, g_free);
	g_signal_emit (self, signals[SIGNAL_DEVICE_CHANGED], 0, device);
}

FuContext *
fu_engine_get_context (FuEngine *self)
{
	return self->ctx;
}

/**
 * fu_engine_get_status:
 * @self: a #FuEngine
 *
 * Gets the current engine status.
 *
 * Returns: a #FwupdStatus, e.g. %FWUPD_STATUS_DECOMPRESSING
 **/
FwupdStatus
fu_engine_get_status (FuEngine *self)
{
	g_return_val_if_fail (FU_IS_ENGINE (self), 0);
	return self->status;
}

static void
fu_engine_set_status (FuEngine *self, FwupdStatus status)
{
	if (self->status == status)
		return;
	self->status = status;

	/* emit changed */
	g_debug ("Emitting PropertyChanged('Status'='%s')",
		 fwupd_status_to_string (status));
	g_signal_emit (self, signals[SIGNAL_STATUS_CHANGED], 0, status);
}

static void
fu_engine_set_percentage (FuEngine *self, guint percentage)
{
	if (self->percentage == percentage)
		return;
	self->percentage = percentage;

	/* emit changed */
	g_signal_emit (self, signals[SIGNAL_PERCENTAGE_CHANGED], 0, percentage);
}

static void
fu_engine_progress_notify_cb (FuDevice *device, GParamSpec *pspec, FuEngine *self)
{
	if (fu_device_get_status (device) == FWUPD_STATUS_UNKNOWN)
		return;
	fu_engine_set_percentage (self, fu_device_get_progress (device));
	fu_engine_emit_device_changed (self, device);
}

static void
fu_engine_status_notify_cb (FuDevice *device, GParamSpec *pspec, FuEngine *self)
{
	fu_engine_set_status (self, fu_device_get_status (device));
	fu_engine_emit_device_changed (self, device);
}

static void
fu_engine_generic_notify_cb (FuDevice *device, GParamSpec *pspec, FuEngine *self)
{
	fu_engine_emit_device_changed (self, device);
}

static void
fu_engine_watch_device (FuEngine *self, FuDevice *device)
{
	g_autoptr(FuDevice) device_old = fu_device_list_get_old (self->device_list, device);
	if (device_old != NULL) {
		g_signal_handlers_disconnect_by_func (device_old,
						      fu_engine_progress_notify_cb,
						      self);
		g_signal_handlers_disconnect_by_func (device_old,
						      fu_engine_status_notify_cb,
						      self);
	}
	g_signal_connect (device, "notify::progress",
			  G_CALLBACK (fu_engine_progress_notify_cb), self);
	g_signal_connect (device, "notify::status",
			  G_CALLBACK (fu_engine_status_notify_cb), self);
	g_signal_connect (device, "notify::flags",
			  G_CALLBACK (fu_engine_generic_notify_cb), self);
	g_signal_connect (device, "notify::update-message",
			  G_CALLBACK (fu_engine_generic_notify_cb), self);
	g_signal_connect (device, "notify::update-image",
			  G_CALLBACK (fu_engine_generic_notify_cb), self);
}

static void
fu_engine_ensure_device_battery_inhibit (FuEngine *self, FuDevice *device)
{
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REQUIRE_AC) &&
	    (fu_context_get_battery_state (self->ctx) == FU_BATTERY_STATE_DISCHARGING ||
	     fu_context_get_battery_state (self->ctx) == FU_BATTERY_STATE_EMPTY)) {
		fu_device_inhibit (device, "battery-system",
				   "Cannot install update when not on AC power");
		return;
	}
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REQUIRE_AC) &&
	    fu_context_get_battery_level (self->ctx) != FU_BATTERY_VALUE_INVALID &&
	    fu_context_get_battery_threshold (self->ctx) != FU_BATTERY_VALUE_INVALID &&
	    fu_context_get_battery_level (self->ctx) < fu_context_get_battery_threshold (self->ctx)) {
		g_autofree gchar *reason = NULL;
		reason = g_strdup_printf ("Cannot install update when system battery is not at least %u%%",
					  fu_context_get_battery_threshold (self->ctx));
		fu_device_inhibit (device, "battery-system", reason);
		return;
	}
	fu_device_uninhibit (device, "battery-system");
}

static void
fu_engine_device_added_cb (FuDeviceList *device_list, FuDevice *device, FuEngine *self)
{
	fu_engine_watch_device (self, device);
	fu_engine_ensure_device_battery_inhibit (self, device);
	g_signal_emit (self, signals[SIGNAL_DEVICE_ADDED], 0, device);
}

static void
fu_engine_device_runner_device_removed (FuEngine *self, FuDevice *device)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		fu_plugin_runner_device_removed (plugin_tmp, device);
	}
}

static void
fu_engine_device_removed_cb (FuDeviceList *device_list, FuDevice *device, FuEngine *self)
{
	fu_engine_device_runner_device_removed (self, device);
	g_signal_handlers_disconnect_by_data (device, self);
	g_signal_emit (self, signals[SIGNAL_DEVICE_REMOVED], 0, device);
}

static void
fu_engine_device_changed_cb (FuDeviceList *device_list, FuDevice *device, FuEngine *self)
{
	fu_engine_watch_device (self, device);
	fu_engine_emit_device_changed (self, device);
}

/* convert hex and decimal versions to dotted style */
static gchar *
fu_engine_get_release_version (FuEngine *self, FuDevice *dev, XbNode *rel, GError **error)
{
	FwupdVersionFormat fmt = fu_device_get_version_format (dev);
	const gchar *version;
	guint64 ver_uint32;

	/* get version */
	version = xb_node_get_attr (rel, "version");
	if (version == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "version unset");
		return NULL;
	}

	/* already dotted notation */
	if (g_strstr_len (version, -1, ".") != NULL)
		return g_strdup (version);

	/* don't touch my version! */
	if (fmt == FWUPD_VERSION_FORMAT_PLAIN)
		return g_strdup (version);

	/* parse as integer */
	ver_uint32 = fu_common_strtoull (version);
	if (fmt == FWUPD_VERSION_FORMAT_UNKNOWN ||
	    ver_uint32 == 0 || ver_uint32 > G_MAXUINT32)
		return g_strdup (version);

	/* convert to dotted decimal */
	return fu_common_version_from_uint32 ((guint32) ver_uint32, fmt);
}

static gint
fu_engine_scheme_compare_cb (gconstpointer a, gconstpointer b, gpointer user_data)
{
	FuEngine *self = FU_ENGINE (user_data);
	const gchar *location1 = *((const gchar ** )a);
	const gchar *location2 = *((const gchar **) b);
	g_autofree gchar *scheme1 = fu_common_uri_get_scheme (location1);
	g_autofree gchar *scheme2 = fu_common_uri_get_scheme (location2);
	guint prio1 = fu_config_get_uri_scheme_prio (self->config, scheme1);
	guint prio2 = fu_config_get_uri_scheme_prio (self->config, scheme2);
	if (prio1 < prio2)
		return -1;
	if (prio1 > prio2)
		return 1;
	return 0;
}

static gboolean
fu_engine_set_release_from_artifact (FuEngine *self,
				     FwupdRelease *rel,
				     FwupdRemote *remote,
				     XbNode *artifact,
				     GError **error)
{
	const gchar *filename;
	guint64 size;
	g_autoptr(GPtrArray) locations = NULL;
	g_autoptr(GPtrArray) checksums = NULL;

	/* filename */
	filename = xb_node_query_text (artifact, "filename", NULL);
	if (filename != NULL)
		fwupd_release_set_filename (rel, filename);

	/* location */
	locations = xb_node_query (artifact, "location", 0, NULL);
	if (locations != NULL) {
		for (guint i = 0; i < locations->len; i++) {
			XbNode *n = g_ptr_array_index (locations, i);
			g_autofree gchar *scheme = NULL;

			/* check the scheme is allowed */
			scheme = fu_common_uri_get_scheme (xb_node_get_text (n));
			if (scheme != NULL) {
				guint prio = fu_config_get_uri_scheme_prio (self->config, scheme);
				if (prio == G_MAXUINT)
					continue;
			}

			/* build the complete URI */
			if (remote != NULL) {
				g_autofree gchar *uri = NULL;
				uri = fwupd_remote_build_firmware_uri (remote,
								       xb_node_get_text (n),
								       NULL);
				if (uri != NULL) {
					fwupd_release_add_location (rel, uri);
					continue;
				}
			}
			fwupd_release_add_location (rel, xb_node_get_text (n));
		}
	}

	/* checksum */
	checksums = xb_node_query (artifact, "checksum", 0, NULL);
	if (checksums != NULL) {
		for (guint i = 0; i < checksums->len; i++) {
			XbNode *n = g_ptr_array_index (checksums, i);
			fwupd_release_add_checksum (rel, xb_node_get_text (n));
		}
	}

	/* size */
	size = xb_node_query_text_as_uint (artifact, "size[@type='installed']", NULL);
	if (size != G_MAXUINT64)
		fwupd_release_set_size (rel, size);

	/* success */
	return TRUE;
}

static gboolean
fu_engine_set_release_from_appstream (FuEngine *self,
				      FuDevice *dev,
				      FwupdRelease *rel,
				      XbNode *component,
				      XbNode *release,
				      GError **error)
{
	FwupdRemote *remote = NULL;
	const gchar *tmp;
	const gchar *remote_id;
	guint64 tmp64;
	g_autofree gchar *version_rel = NULL;
	g_autoptr(GPtrArray) cats = NULL;
	g_autoptr(GPtrArray) issues = NULL;
	g_autoptr(XbNode) artifact = NULL;
	g_autoptr(XbNode) description = NULL;

	/* set from the component */
	tmp = xb_node_query_text (component, "id", NULL);
	if (tmp != NULL)
		fwupd_release_set_appstream_id (rel, tmp);
	tmp = xb_node_query_text (component, "url[@type='homepage']", NULL);
	if (tmp != NULL)
		fwupd_release_set_homepage (rel, tmp);
	tmp = xb_node_query_text (component, "project_license", NULL);
	if (tmp != NULL)
		fwupd_release_set_license (rel, tmp);
	tmp = xb_node_query_text (component, "name", NULL);
	if (tmp != NULL)
		fwupd_release_set_name (rel, tmp);
	tmp = xb_node_query_text (component, "summary", NULL);
	if (tmp != NULL)
		fwupd_release_set_summary (rel, tmp);
	tmp = xb_node_query_text (component, "branch", NULL);
	if (tmp != NULL)
		fwupd_release_set_branch (rel, tmp);
	tmp = xb_node_query_text (component, "developer_name", NULL);
	if (tmp != NULL)
		fwupd_release_set_vendor (rel, tmp);

	/* refresh the device and release to the new version format too */
	fu_engine_md_refresh_device_from_component (self, dev, component);

	/* the version is fixed up at runtime */
	version_rel = fu_engine_get_release_version (self, dev, release, error);
	if (version_rel == NULL)
		return FALSE;
	fwupd_release_set_version (rel, version_rel);

	/* find the remote */
	remote_id = xb_node_query_text (component, "../custom/value[@key='fwupd::RemoteId']", NULL);
	if (remote_id != NULL) {
		fwupd_release_set_remote_id (rel, remote_id);
		remote = fu_remote_list_get_by_id (self->remote_list, remote_id);
		if (remote == NULL)
			g_warning ("no remote found for release %s", version_rel);
	}
	artifact = xb_node_query_first (release, "artifacts/artifact", NULL);
	if (artifact != NULL) {
		if (!fu_engine_set_release_from_artifact (self,
							  rel,
							  remote,
							  artifact,
							  error))
			return FALSE;
	}
	description = xb_node_query_first (release, "description", NULL);
	if (description != NULL) {
		g_autofree gchar *xml = NULL;
		xml = xb_node_export (description, XB_NODE_EXPORT_FLAG_ONLY_CHILDREN, NULL);
		if (xml != NULL)
			fwupd_release_set_description (rel, xml);
	}
	if (artifact == NULL) {
		tmp = xb_node_query_text (release, "location", NULL);
		if (tmp != NULL) {
			g_autofree gchar *uri = NULL;
			if (remote != NULL)
				uri = fwupd_remote_build_firmware_uri (remote, tmp, NULL);
			if (uri == NULL)
				uri = g_strdup (tmp);
			fwupd_release_add_location (rel, uri);
		}
	}
	if (fwupd_release_get_locations(rel)->len == 0 && remote != NULL &&
	    fwupd_remote_get_kind (remote) == FWUPD_REMOTE_KIND_DIRECTORY) {
		tmp = xb_node_query_text (component, "../custom/value[@key='fwupd::FilenameCache']", NULL);
		if (tmp != NULL)  {
			g_autofree gchar *uri = g_strdup_printf ("file://%s", tmp);
			fwupd_release_add_location (rel, uri);
		}
	}
	if (artifact == NULL) {
		tmp = xb_node_query_text (release, "checksum[@target='content']", NULL);
		if (tmp != NULL)
			fwupd_release_set_filename (rel, tmp);
	}
	tmp = xb_node_query_text (release, "url[@type='details']", NULL);
	if (tmp != NULL)
		fwupd_release_set_details_url (rel, tmp);
	tmp = xb_node_query_text (release, "url[@type='source']", NULL);
	if (tmp != NULL)
		fwupd_release_set_source_url (rel, tmp);
	if (artifact == NULL) {
		tmp = xb_node_query_text (release, "checksum[@target='container']", NULL);
		if (tmp != NULL)
			fwupd_release_add_checksum (rel, tmp);
	}
	if (artifact == NULL) {
		tmp64 = xb_node_query_text_as_uint (release, "size[@type='installed']", NULL);
		if (tmp64 != G_MAXUINT64)
			fwupd_release_set_size (rel, tmp64);
	}
	if (fwupd_release_get_size (rel) == 0) {
		GBytes *sz = xb_node_get_data (release, "fwupd::ReleaseSize");
		if (sz != NULL) {
			const guint64 *sizeptr = g_bytes_get_data (sz, NULL);
			fwupd_release_set_size (rel, *sizeptr);
		}
	}
	tmp = xb_node_get_attr (release, "urgency");
	if (tmp != NULL)
		fwupd_release_set_urgency (rel, fwupd_release_urgency_from_string (tmp));
	tmp64 = xb_node_get_attr_as_uint (release, "install_duration");
	if (tmp64 != G_MAXUINT64)
		fwupd_release_set_install_duration (rel, tmp64);
	tmp64 = xb_node_get_attr_as_uint (release, "timestamp");
	if (tmp64 != G_MAXUINT64)
		fwupd_release_set_created (rel, tmp64);
	cats = xb_node_query (component, "categories/category", 0, NULL);
	if (cats != NULL) {
		for (guint i = 0; i < cats->len; i++) {
			XbNode *n = g_ptr_array_index (cats, i);
			fwupd_release_add_category (rel, xb_node_get_text (n));
		}
	}
	issues = xb_node_query (component, "issues/issue", 0, NULL);
	if (issues != NULL) {
		for (guint i = 0; i < issues->len; i++) {
			XbNode *n = g_ptr_array_index (issues, i);
			fwupd_release_add_issue (rel, xb_node_get_text (n));
		}
	}
	tmp = xb_node_query_text (component, "screenshots/screenshot/caption", NULL);
	if (tmp != NULL)
		fwupd_release_set_detach_caption (rel, tmp);
	tmp = xb_node_query_text (component, "screenshots/screenshot/image", NULL);
	if (tmp != NULL) {
		if (remote != NULL) {
			g_autofree gchar *img = NULL;
			img = fwupd_remote_build_firmware_uri (remote, tmp, error);
			if (img == NULL)
				return FALSE;
			fwupd_release_set_detach_image (rel, img);
		} else {
			fwupd_release_set_detach_image (rel, tmp);
		}
	}
	tmp = xb_node_query_text (component, "custom/value[@key='LVFS::UpdateProtocol']", NULL);
	if (tmp != NULL)
		fwupd_release_set_protocol (rel, tmp);
	tmp = xb_node_query_text (component, "custom/value[@key='LVFS::UpdateMessage']", NULL);
	if (tmp != NULL)
		fwupd_release_set_update_message (rel, tmp);
	tmp = xb_node_query_text (component, "custom/value[@key='LVFS::UpdateImage']", NULL);
	if (tmp != NULL) {
		if (remote != NULL) {
			g_autofree gchar *img = NULL;
			img = fwupd_remote_build_firmware_uri (remote, tmp, error);
			if (img == NULL)
				return FALSE;
			fwupd_release_set_update_image (rel, img);
		} else {
			fwupd_release_set_update_image (rel, tmp);
		}
	}

	/* sort the locations by scheme */
	g_ptr_array_sort_with_data (fwupd_release_get_locations (rel),
				    fu_engine_scheme_compare_cb, self);
	return TRUE;
}

/* finds the remote-id for the first firmware in the silo that matches this
 * container checksum */
static const gchar *
fu_engine_get_remote_id_for_checksum (FuEngine *self, const gchar *csum)
{
	g_autofree gchar *xpath = NULL;
	g_autoptr(XbNode) key = NULL;
	xpath = g_strdup_printf ("components/component[@type='firmware']/releases/release/"
				 "checksum[@target='container'][text()='%s']/../../"
				 "../../custom/value[@key='fwupd::RemoteId']", csum);
	key = xb_silo_query_first (self->silo, xpath, NULL);
	if (key == NULL)
		return NULL;
	return xb_node_get_text (key);
}

/**
 * fu_engine_unlock:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Unlocks a device.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_unlock (FuEngine *self, const gchar *device_id, GError **error)
{
	FuPlugin *plugin;
	g_autoptr(FuDevice) device = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (device_id != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* check the device exists */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return FALSE;

	/* get the plugin */
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;

	/* run the correct plugin that added this */
	if (!fu_plugin_runner_unlock (plugin, device, error))
		return FALSE;

	/* make the UI update */
	fu_engine_emit_device_changed (self, device);
	fu_engine_emit_changed (self);
	return TRUE;
}

gboolean
fu_engine_modify_config (FuEngine *self, const gchar *key, const gchar *value, GError **error)
{
	const gchar *keys[] = {
		"ArchiveSizeMax",
		"DisabledDevices",
		"BlockedFirmware",
		"DisabledPlugins",
		"IdleTimeout",
		"VerboseDomains",
		"UpdateMotd",
		"EnumerateAllDevices",
		NULL };

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (value != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* check keys are valid */
	if (!g_strv_contains (keys, key)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "key %s not supported", key);
		return FALSE;
	}

	/* modify, effective next reboot */
	return fu_config_set_key_value (self->config, key, value, error);
}

/**
 * fu_engine_modify_remote:
 * @self: a #FuEngine
 * @remote_id: a remote ID
 * @key: the key, e.g. `Enabled`
 * @value: the key, e.g. `true`
 * @error: (nullable): optional return location for an error
 *
 * Updates the verification silo entry for a specific device.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_modify_remote (FuEngine *self,
			 const gchar *remote_id,
			 const gchar *key,
			 const gchar *value,
			 GError **error)
{
	const gchar *keys[] = {
		"ApprovalRequired",
		"AutomaticReports",
		"AutomaticSecurityReports",
		"Enabled",
		"FirmwareBaseURI",
		"MetadataURI",
		"ReportURI",
		"SecurityReportURI",
		NULL,
	};

	/* check keys are valid */
	if (!g_strv_contains (keys, key)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "key %s not supported", key);
		return FALSE;
	}
	return fu_remote_list_set_key_value (self->remote_list, remote_id, key, value, error);
}

/**
 * fu_engine_modify_device:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @key: the key, e.g. `Flags`
 * @value: the key, e.g. `reported`
 * @error: (nullable): optional return location for an error
 *
 * Sets the reported flag for a specific device. This ensures that other
 * front-end clients for fwupd do not report the same event.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_modify_device (FuEngine *self,
			 const gchar *device_id,
			 const gchar *key,
			 const gchar *value,
			 GError **error)
{
	g_autoptr(FuDevice) device = NULL;

	/* find the correct device */
	device = fu_history_get_device_by_id (self->history, device_id, error);
	if (device == NULL)
		return FALSE;

	/* support adding a subset of device flags */
	if (g_strcmp0 (key, "Flags") == 0) {
		FwupdDeviceFlags flag = fwupd_device_flag_from_string (value);
		if (flag == FWUPD_DEVICE_FLAG_UNKNOWN) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "key %s not a valid flag", key);
			return FALSE;
		}
		if (flag != FWUPD_DEVICE_FLAG_REPORTED &&
		    flag != FWUPD_DEVICE_FLAG_NOTIFIED) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "flag %s cannot be set from client", key);
			return FALSE;
		}
		fu_device_add_flag (device, flag);
		return fu_history_modify_device (self->history, device, error);
	}

	/* others invalid */
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "key %s not supported", key);
	return FALSE;
}

static const gchar *
fu_engine_checksum_type_to_string (GChecksumType checksum_type)
{
	if (checksum_type == G_CHECKSUM_SHA1)
		return "sha1";
	if (checksum_type == G_CHECKSUM_SHA256)
		return "sha256";
	if (checksum_type == G_CHECKSUM_SHA512)
		return "sha512";
	return "sha1";
}

/**
 * fu_engine_verify_update:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Updates the verification silo entry for a specific device.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_verify_update (FuEngine *self, const gchar *device_id, GError **error)
{
	FuPlugin *plugin;
	GPtrArray *checksums;
	GPtrArray *guids;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *localstatedir = NULL;
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(XbBuilder) builder = xb_builder_new ();
	g_autoptr(XbBuilderNode) component = NULL;
	g_autoptr(XbBuilderNode) provides = NULL;
	g_autoptr(XbBuilderNode) release = NULL;
	g_autoptr(XbBuilderNode) releases = NULL;
	g_autoptr(XbSilo) silo = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (device_id != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* check the devices still exists */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return FALSE;

	/* get the plugin */
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;

	/* get the checksum */
	checksums = fu_device_get_checksums (device);
	if (checksums->len == 0) {
		if (!fu_plugin_runner_verify (plugin, device,
					      FU_PLUGIN_VERIFY_FLAG_NONE,
					      error))
			return FALSE;
		fu_engine_emit_device_changed (self, device);
	}

	/* we got nothing */
	if (checksums->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "device verification not supported");
		return FALSE;
	}

	/* build XML */
	component = xb_builder_node_insert (NULL, "component",
					    "type", "firmware",
					    NULL);
	provides = xb_builder_node_insert (component, "provides", NULL);
	guids = fu_device_get_guids (device);
	for (guint i = 0; i < guids->len; i++) {
		const gchar *guid = g_ptr_array_index (guids, i);
		g_autoptr(XbBuilderNode) provide = NULL;
		provide = xb_builder_node_insert (provides, "firmware",
						  "type", "flashed",
						  NULL);
		xb_builder_node_set_text (provide, guid, -1);
	}
	releases = xb_builder_node_insert (component, "releases", NULL);
	release = xb_builder_node_insert (releases, "release",
					  "version", fu_device_get_version (device),
					  NULL);
	for (guint i = 0; i < checksums->len; i++) {
		const gchar *checksum = g_ptr_array_index (checksums, i);
		GChecksumType kind = fwupd_checksum_guess_kind (checksum);
		g_autoptr(XbBuilderNode) csum = NULL;
		csum = xb_builder_node_insert (release, "checksum",
					       "type", fu_engine_checksum_type_to_string (kind),
					       "target", "content",
					       NULL);
		xb_builder_node_set_text (csum, checksum, -1);
	}
	xb_builder_import_node (builder, component);

	/* save silo */
	localstatedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	fn = g_strdup_printf ("%s/verify/%s.xml", localstatedir, device_id);
	if (!fu_common_mkdir_parent (fn, error))
		return FALSE;
	file = g_file_new_for_path (fn);
	silo = xb_builder_compile (builder, XB_BUILDER_COMPILE_FLAG_NONE, NULL, error);
	if (silo == NULL)
		return FALSE;
	if (!xb_silo_export_file (silo, file,
				  XB_NODE_EXPORT_FLAG_FORMAT_MULTILINE,
				  NULL, error))
		return FALSE;

	/* success */
	return TRUE;
}

XbNode *
fu_engine_get_component_by_guids (FuEngine *self, FuDevice *device)
{
	GPtrArray *guids = fu_device_get_guids (device);
	g_autoptr(GString) xpath = g_string_new (NULL);
	g_autoptr(XbNode) component = NULL;
	for (guint i = 0; i < guids->len; i++) {
		const gchar *guid = g_ptr_array_index (guids, i);
		xb_string_append_union (xpath,
					"components/component[@type='firmware']/"
					"provides/firmware[@type='flashed'][text()='%s']/"
					"../..", guid);
	}
	component = xb_silo_query_first (self->silo, xpath->str, NULL);
	if (component != NULL)
		return g_steal_pointer (&component);
	return NULL;
}

static XbNode *
fu_engine_verify_from_local_metadata (FuEngine *self,
				      FuDevice *device,
				      GError **error)
{
	g_autofree gchar *fn = NULL;
	g_autofree gchar *localstatedir = NULL;
	g_autofree gchar *xpath = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(XbBuilder) builder = xb_builder_new ();
	g_autoptr(XbBuilderSource) source = xb_builder_source_new ();
	g_autoptr(XbNode) release = NULL;
	g_autoptr(XbSilo) silo = NULL;

	localstatedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	fn = g_strdup_printf ("%s/verify/%s.xml",
			      localstatedir,
			      fu_device_get_id (device));
	file = g_file_new_for_path (fn);
	if (!g_file_query_exists (file, NULL)) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "failed to find %s", fn);
		return NULL;
	}

	if (!xb_builder_source_load_file (source, file,
					  XB_BUILDER_SOURCE_FLAG_NONE,
					  NULL, error))
		return NULL;
	xb_builder_import_source (builder, source);
	silo = xb_builder_compile (builder,
				   XB_BUILDER_COMPILE_FLAG_NONE,
				   NULL, error);
	if (silo == NULL)
		return NULL;
	xpath = g_strdup_printf ("component/releases/release[@version='%s']",
				 fu_device_get_version (device));
	release = xb_silo_query_first (silo, xpath, error);
	if (release == NULL)
		return NULL;

	/* silo has to have same lifecycle as node */
	g_object_set_data_full (G_OBJECT (release), "XbSilo",
				g_steal_pointer (&silo),
				(GDestroyNotify) g_object_unref);
	return g_steal_pointer (&release);
}

static XbNode *
fu_engine_verify_from_system_metadata (FuEngine *self,
				       FuDevice *device,
				       GError **error)
{
	FwupdVersionFormat fmt = fu_device_get_version_format (device);
	GPtrArray *guids = fu_device_get_guids (device);
	g_autoptr(XbQuery) query = NULL;

	/* prepare query with bound GUID parameter */
	query = xb_query_new_full (self->silo,
				   "components/component[@type='firmware']/"
				   "provides/firmware[@type='flashed'][text()=?]/"
				   "../../releases/release",
				   XB_QUERY_FLAG_OPTIMIZE |
				   XB_QUERY_FLAG_USE_INDEXES,
				   error);
	if (query == NULL)
		return NULL;

	/* use prepared query for each GUID */
	for (guint i = 0; i < guids->len; i++) {
		const gchar *guid = g_ptr_array_index (guids, i);
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GPtrArray) releases = NULL;
#if LIBXMLB_CHECK_VERSION(0,3,0)
		g_auto(XbQueryContext) context = XB_QUERY_CONTEXT_INIT ();
#endif

		/* bind GUID and then query */
#if LIBXMLB_CHECK_VERSION(0,3,0)
		xb_value_bindings_bind_str (xb_query_context_get_bindings (&context), 0, guid, NULL);
		releases = xb_silo_query_with_context (self->silo, query, &context, &error_local);
#else
		if (!xb_query_bind_str (query, 0, guid, error)) {
			g_prefix_error (error, "failed to bind string: ");
			return NULL;
		}
		releases = xb_silo_query_full (self->silo, query, &error_local);
#endif
		if (releases == NULL) {
			if (g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
			    g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
				g_debug ("could not find %s: %s",
					 guid, error_local->message);
				continue;
			}
			g_propagate_error (error, g_steal_pointer (&error_local));
			return NULL;
		}
		for (guint j = 0; j < releases->len; j++) {
			XbNode *rel = g_ptr_array_index (releases, j);
			const gchar *rel_ver = xb_node_get_attr (rel, "version");
			g_autofree gchar *tmp_ver = fu_common_version_parse_from_format (rel_ver, fmt);
			if (fu_common_vercmp_full (tmp_ver, fu_device_get_version (device), fmt) == 0)
				return g_object_ref (rel);
		}
	}

	/* not found */
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "failed to find release");
	return NULL;
}

/**
 * fu_engine_verify:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Verifies a device firmware checksum using the verification silo entry.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_verify (FuEngine *self, const gchar *device_id, GError **error)
{
	FuPlugin *plugin;
	GPtrArray *checksums;
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GString) xpath_csum = g_string_new (NULL);
	g_autoptr(XbNode) csum = NULL;
	g_autoptr(XbNode) release = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (device_id != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* check the id exists */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return FALSE;

	/* get the plugin */
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;

	/* update the device firmware hashes if possible */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE)) {
		if (!fu_plugin_runner_verify (plugin, device,
					      FU_PLUGIN_VERIFY_FLAG_NONE, error))
			return FALSE;
	}

	/* find component in local metadata */
	release = fu_engine_verify_from_local_metadata (self, device, &error_local);
	if (release == NULL) {
		if (!g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
		    !g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
			g_propagate_error (error, g_steal_pointer (&error_local));
			return FALSE;
		}
	}

	/* try again with the system metadata */
	if (release == NULL) {
		g_autoptr(GError) error_system = NULL;
		release = fu_engine_verify_from_system_metadata (self, device, &error_system);
		if (release == NULL) {
			if (!g_error_matches (error_system, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
			    !g_error_matches (error_system, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
				g_propagate_error (error, g_steal_pointer (&error_system));
				return FALSE;
			}
		}
	}
	if (release == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "No release found for version %s",
			     fu_device_get_version (device));
		return FALSE;
	}

	/* get the matching checksum */
	checksums = fu_device_get_checksums (device);
	if (checksums->len == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "No device checksums for %s",
			     fu_device_get_version (device));
		return FALSE;
	}

	/* do any of the checksums in the release match any in the device */
	for (guint j = 0; j < checksums->len; j++) {
		const gchar *hash_tmp = g_ptr_array_index (checksums, j);
		xb_string_append_union (xpath_csum,
					"checksum[@target='device'][text()='%s']",
					hash_tmp);
		xb_string_append_union (xpath_csum,
					"checksum[@target='content'][text()='%s']",
					hash_tmp);
	}
	csum = xb_node_query_first (release, xpath_csum->str, NULL);
	if (csum == NULL) {
		g_autoptr(GString) checksums_device = g_string_new (NULL);
		g_autoptr(GString) checksums_metadata = g_string_new (NULL);
		g_autoptr(GPtrArray) csums = NULL;
		g_autoptr(GString) xpath = g_string_new (NULL);

		/* get all checksums to display a useful error */
		xb_string_append_union (xpath, "checksum[@target='device']");
		if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE))
			xb_string_append_union (xpath, "checksum[@target='content']");
		csums = xb_node_query (release, xpath->str, 0, NULL);
		if (csums == NULL) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_FOUND,
				     "No stored checksums for %s",
				     fu_device_get_version (device));
			return FALSE;
		}
		for (guint i = 0; i < csums->len; i++) {
			XbNode *csum_tmp = g_ptr_array_index (csums, i);
			xb_string_append_union (checksums_metadata,
						"%s", xb_node_get_text (csum_tmp));
		}
		for (guint i = 0; i < checksums->len; i++) {
			const gchar *hash_tmp = g_ptr_array_index (checksums, i);
			xb_string_append_union (checksums_device, "%s", hash_tmp);
		}
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "For %s %s expected %s, got %s",
			     fu_device_get_name (device),
			     fu_device_get_version (device),
			     checksums_metadata->str,
			     checksums_device->str);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_engine_require_vercmp (XbNode *req,
			  const gchar *version,
			  FwupdVersionFormat fmt,
			  GError **error)
{
	gboolean ret = FALSE;
	gint rc = 0;
	const gchar *tmp = xb_node_get_attr (req, "compare");
	const gchar *version_req = xb_node_get_attr (req, "version");

	if (g_strcmp0 (tmp, "eq") == 0) {
		rc = fu_common_vercmp_full (version, version_req, fmt);
		ret = rc == 0;
	} else if (g_strcmp0 (tmp, "ne") == 0) {
		rc = fu_common_vercmp_full (version, version_req, fmt);
		ret = rc != 0;
	} else if (g_strcmp0 (tmp, "lt") == 0) {
		rc = fu_common_vercmp_full (version, version_req, fmt);
		ret = rc < 0;
	} else if (g_strcmp0 (tmp, "gt") == 0) {
		rc = fu_common_vercmp_full (version, version_req, fmt);
		ret = rc > 0;
	} else if (g_strcmp0 (tmp, "le") == 0) {
		rc = fu_common_vercmp_full (version, version_req, fmt);
		ret = rc <= 0;
	} else if (g_strcmp0 (tmp, "ge") == 0) {
		rc = fu_common_vercmp_full (version, version_req, fmt);
		ret = rc >= 0;
	} else if (g_strcmp0 (tmp, "glob") == 0) {
		ret = fu_common_fnmatch (version_req, version);
	} else if (g_strcmp0 (tmp, "regex") == 0) {
		ret = g_regex_match_simple (version_req, version, 0, 0);
	} else {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to compare [%s] and [%s]",
			     version_req,
			     version);
		return FALSE;
	}

	/* set error */
	if (!ret) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "failed predicate [%s %s %s]",
			     version_req, tmp, version);
	}
	return ret;
}

static gboolean
fu_engine_check_requirement_not_child (FuEngine *self, XbNode *req,
				       FuDevice *device, GError **error)
{
	GPtrArray *children = fu_device_get_children (device);

	/* only <firmware> supported */
	if (g_strcmp0 (xb_node_get_element (req), "firmware") != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "cannot handle not-child %s requirement",
			     xb_node_get_element (req));
		return FALSE;
	}

	/* check each child */
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child = g_ptr_array_index (children, i);
		const gchar *version = fu_device_get_version (child);
		if (version == NULL) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "no version provided by %s, child of %s",
				     fu_device_get_name (child),
				     fu_device_get_name (device));
			return FALSE;
		}
		if (fu_engine_require_vercmp (req, version,
					      fu_device_get_version_format (child),
					      NULL)) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "Not compatible with child device version %s",
				     version);
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
fu_engine_check_requirement_vendor_id (FuEngine *self, XbNode *req,
				       FuDevice *device, GError **error)
{
	GPtrArray *vendor_ids;
	const gchar *vendor_ids_metadata;
	g_autofree gchar *vendor_ids_device = NULL;

	/* devices without vendor IDs should not exist! */
	vendor_ids = fu_device_get_vendor_ids (device);
	if (vendor_ids->len == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "device [%s] has no vendor ID",
			     fu_device_get_id (device));
		return FALSE;
	}

	/* metadata with empty vendor IDs should not exist! */
	vendor_ids_metadata = xb_node_get_attr (req, "version");
	if (vendor_ids_metadata == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "metadata has no vendor ID");
		return FALSE;
	}

	/* it is always safe to use a regex, even for simple strings */
	vendor_ids_device = fu_common_strjoin_array ("|", vendor_ids);
	if (!g_regex_match_simple (vendor_ids_metadata, vendor_ids_device, 0, 0)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "Not compatible with vendor %s: got %s",
			     vendor_ids_device,
			     vendor_ids_metadata);
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_engine_check_requirement_firmware (FuEngine *self, XbNode *req, FuDevice *device,
				      FwupdInstallFlags flags, GError **error)
{
	guint64 depth;
	g_autoptr(FuDevice) device_actual = g_object_ref (device);
	g_autoptr(GError) error_local = NULL;

	/* look at the parent device */
	depth = xb_node_get_attr_as_uint (req, "depth");
	if (depth != G_MAXUINT64) {
		for (guint64 i = 0; i < depth; i++) {
			FuDevice *device_tmp = fu_device_get_parent (device_actual);
			if (device_actual == NULL) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "No parent device for %s "
					     "(%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT ")",
					     fu_device_get_name (device_actual), i, depth);
				return FALSE;
			}
			g_set_object (&device_actual, device_tmp);
		}
	}

	/* old firmware version */
	if (xb_node_get_text (req) == NULL) {
		const gchar *version = fu_device_get_version (device_actual);
		if (!fu_engine_require_vercmp (req, version,
					       fu_device_get_version_format (device_actual),
					       &error_local)) {
			if (g_strcmp0 (xb_node_get_attr (req, "compare"), "ge") == 0) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "Not compatible with firmware version %s, requires >= %s",
					     version, xb_node_get_attr (req, "version"));
			} else {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "Not compatible with firmware version: %s",
					     error_local->message);
			}
			return FALSE;
		}
		return TRUE;
	}

	/* bootloader version */
	if (g_strcmp0 (xb_node_get_text (req), "bootloader") == 0) {
		const gchar *version = fu_device_get_version_bootloader (device_actual);
		if (!fu_engine_require_vercmp (req, version,
					       fu_device_get_version_format (device_actual),
					       &error_local)) {
			if (g_strcmp0 (xb_node_get_attr (req, "compare"), "ge") == 0) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "Not compatible with bootloader version %s, requires >= %s",
                                             version, xb_node_get_attr (req, "version"));

			} else {
				g_debug ("Bootloader is not compatible: %s", error_local->message);
				g_set_error_literal (error,
						     FWUPD_ERROR,
						     FWUPD_ERROR_NOT_SUPPORTED,
						     "Bootloader is not compatible");
			}
			return FALSE;
		}
		return TRUE;
	}

	/* vendor ID */
	if (g_strcmp0 (xb_node_get_text (req), "vendor-id") == 0) {
		if (flags & FWUPD_INSTALL_FLAG_IGNORE_VID_PID)
			return TRUE;
		return fu_engine_check_requirement_vendor_id (self, req, device_actual, error);
	}

	/* child version */
	if (g_strcmp0 (xb_node_get_text (req), "not-child") == 0)
		return fu_engine_check_requirement_not_child (self, req, device_actual, error);

	/* another device */
	if (fwupd_guid_is_valid (xb_node_get_text (req))) {
		const gchar *guid = xb_node_get_text (req);
		const gchar *version;

		/* find if the other device exists */
		if (depth == G_MAXUINT64) {
			g_autoptr(FuDevice) device_tmp = NULL;
			device_tmp = fu_device_list_get_by_guid (self->device_list, guid, error);
			if (device_tmp == NULL)
				return FALSE;
			g_set_object (&device_actual, device_tmp);
		/* look for a sibling */
		} else if (depth == 0) {
			FuDevice *child = NULL;
			FuDevice *parent = fu_device_get_parent (device_actual);
			GPtrArray *children;
			if (parent == NULL) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "No parent specified for device %s",
					     fu_device_get_name (device_actual));
				return FALSE;
			}
			children = fu_device_get_children (parent);
			for (guint i = 0 ; i < children->len; i++) {
				child = g_ptr_array_index (children, i);
				if (fu_device_has_guid (child, guid))
					break;
				child = NULL;
			}
			if (child == NULL) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "No sibling found with GUID of %s",
					     guid);
				return FALSE;
			}
			g_set_object (&device_actual, child);
		/* verify the parent device has the GUID */
		} else {
			if (!fu_device_has_guid (device_actual, guid)) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "No GUID of %s on parent device %s",
					     guid, fu_device_get_name (device_actual));
				return FALSE;
			}
		}

		/* get the version of the other device */
		version = fu_device_get_version (device_actual);
		if (version != NULL &&
		    xb_node_get_attr (req, "compare") != NULL &&
		    !fu_engine_require_vercmp (req, version,
					       fu_device_get_version_format (device_actual),
					       &error_local)) {
			if (g_strcmp0 (xb_node_get_attr (req, "compare"), "ge") == 0) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "Not compatible with %s version %s, requires >= %s",
					     fu_device_get_name (device_actual),
					     version,
					     xb_node_get_attr (req, "version"));
			} else {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "Not compatible with %s: %s",
					     fu_device_get_name (device_actual),
					     error_local->message);
			}
			return FALSE;
		}
		return TRUE;

	}

	/* not supported */
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "cannot handle firmware requirement '%s'",
		     xb_node_get_text (req));
	return FALSE;
}

static gboolean
fu_engine_check_requirement_id (FuEngine *self, XbNode *req, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	const gchar *version = g_hash_table_lookup (self->runtime_versions,
						    xb_node_get_text (req));
	if (version == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "no version available for %s",
			     xb_node_get_text (req));
		return FALSE;
	}
	if (!fu_engine_require_vercmp (req, version, FWUPD_VERSION_FORMAT_UNKNOWN, &error_local)) {
		if (g_strcmp0 (xb_node_get_attr (req, "compare"), "ge") == 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "Not compatible with %s version %s, requires >= %s",
				     xb_node_get_text (req), version,
				     xb_node_get_attr (req, "version"));
		} else {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "Not compatible with %s version: %s",
				     xb_node_get_text (req), error_local->message);
		}
		return FALSE;
	}

	g_debug ("requirement %s %s %s -> %s passed",
		 xb_node_get_attr (req, "version"),
		 xb_node_get_attr (req, "compare"),
		 version, xb_node_get_text (req));
	return TRUE;
}

static gboolean
fu_engine_check_requirement_hardware (FuEngine *self, XbNode *req, GError **error)
{
	g_auto(GStrv) hwid_split = NULL;

	/* split and treat as OR */
	hwid_split = g_strsplit (xb_node_get_text (req), "|", -1);
	for (guint i = 0; hwid_split[i] != NULL; i++) {
		if (fu_context_has_hwid_guid (self->ctx, hwid_split[i])) {
			g_debug ("HWID provided %s", hwid_split[i]);
			return TRUE;
		}
	}

	/* nothing matched */
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_INVALID_FILE,
		     "no HWIDs matched %s",
		     xb_node_get_text (req));
	return FALSE;
}

static gboolean
fu_engine_check_requirement_client (FuEngine *self,
				    FuEngineRequest *request,
				    XbNode *req,
				    GError **error)
{
	FwupdFeatureFlags flags;
	g_auto(GStrv) feature_split = NULL;

	/* split and treat as AND */
	feature_split = g_strsplit (xb_node_get_text (req), "|", -1);
	flags = fu_engine_request_get_feature_flags (request);
	for (guint i = 0; feature_split[i] != NULL; i++) {
		FwupdFeatureFlags flag = fwupd_feature_flag_from_string (feature_split[i]);

		/* not recognized */
		if (flag == FWUPD_FEATURE_FLAG_LAST) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_FOUND,
				     "client requirement %s unknown",
				     feature_split[i]);
			return FALSE;
		}

		/* not supported */
		if ((flags & flag) == 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "client requirement %s not supported",
				     feature_split[i]);
			return FALSE;
		}
	}

	/* success */
	return TRUE;
}

static gboolean
fu_engine_check_requirement (FuEngine *self,
			     FuEngineRequest *request,
			     XbNode *req,
			     FuDevice *device,
			     FwupdInstallFlags flags,
			     GError **error)
{
	/* ensure component requirement */
	if (g_strcmp0 (xb_node_get_element (req), "id") == 0)
		return fu_engine_check_requirement_id (self, req, error);

	/* ensure firmware requirement */
	if (g_strcmp0 (xb_node_get_element (req), "firmware") == 0) {
		if (device == NULL)
			return TRUE;
		return fu_engine_check_requirement_firmware (self, req, device,
							     flags, error);
	}

	/* ensure hardware requirement */
	if (g_strcmp0 (xb_node_get_element (req), "hardware") == 0)
		return fu_engine_check_requirement_hardware (self, req, error);

	/* ensure client requirement */
	if (g_strcmp0 (xb_node_get_element (req), "client") == 0)
		return fu_engine_check_requirement_client (self, request, req, error);

	/* not supported */
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "cannot handle requirement type %s",
		     xb_node_get_element (req));
	return FALSE;
}

gboolean
fu_engine_check_trust (FuInstallTask *task, GError **error)
{
#ifndef HAVE_POLKIT
	if ((fu_install_task_get_trust_flags (task) & FWUPD_TRUST_FLAG_PAYLOAD) == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "archive signature missing or not trusted");
		return FALSE;
	}
#endif
	return TRUE;
}

static gboolean
fu_engine_check_soft_requirement (FuEngine *self,
				  FuEngineRequest *request,
				  XbNode *req,
				  FuDevice *device,
				  FwupdInstallFlags flags,
				  GError **error)
{
	g_autoptr(GError) error_local = NULL;
	if (!fu_engine_check_requirement (self, request, req, device,
					  flags, &error_local)) {
		if (flags & FWUPD_INSTALL_FLAG_FORCE) {
			g_debug ("ignoring soft-requirement due to --force: %s",
				 error_local->message);
			return TRUE;
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}
	return TRUE;
}

gboolean
fu_engine_check_requirements (FuEngine *self,
			      FuEngineRequest *request,
			      FuInstallTask *task,
			      FwupdInstallFlags flags,
			      GError **error)
{
	FuDevice *device = fu_install_task_get_device (task);
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) reqs = NULL;

	/* all install task checks require a device */
	if (device != NULL) {
		if (!fu_install_task_check_requirements (task, flags, error))
			return FALSE;
	}

	/* do engine checks */
	reqs = xb_node_query (fu_install_task_get_component (task),
			      "requires/*", 0, &error_local);
	if (reqs != NULL) {
		for (guint i = 0; i < reqs->len; i++) {
			XbNode *req = g_ptr_array_index (reqs, i);
			if (!fu_engine_check_requirement (self, request,
							  req, device,
							  flags, error))
				return FALSE;
		}
	} else if (!g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
		   !g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
			g_propagate_error (error, g_steal_pointer (&error_local));
			return FALSE;
	}

	/* soft requirements */
	g_clear_error (&error_local);
	reqs = xb_node_query (fu_install_task_get_component (task),
			      "suggests/*|recommends/*", 0, &error_local);
	if (reqs != NULL) {
		for (guint i = 0; i < reqs->len; i++) {
			XbNode *req = g_ptr_array_index (reqs, i);
			if (!fu_engine_check_soft_requirement (self, request,
							       req, device,
							       flags, error))
				return FALSE;
		}
	} else if (!g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) &&
		   !g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
			g_propagate_error (error, g_steal_pointer (&error_local));
			return FALSE;
	}

	return TRUE;
}

void
fu_engine_idle_reset (FuEngine *self)
{
	fu_idle_reset (self->idle);
}

static gchar *
fu_engine_get_boot_time (void)
{
	g_autofree gchar *buf = NULL;
	g_auto(GStrv) lines = NULL;
	if (!g_file_get_contents ("/proc/stat", &buf, NULL, NULL))
		return NULL;
	lines = g_strsplit (buf, "\n", -1);
	for (guint i = 0; lines[i] != NULL; i++) {
		if (g_str_has_prefix (lines[i], "btime "))
			return g_strdup (lines[i] + 6);
	}
	return NULL;
}

static gboolean
fu_engine_get_report_metadata_os_release (GHashTable *hash, GError **error)
{
	g_autoptr(GHashTable) os_release = NULL;
	struct {
		const gchar *key;
		const gchar *val;
	} distro_kv[] = {
		{ "ID",			"DistroId" },
		{ "VERSION_ID",		"DistroVersion" },
		{ "VARIANT_ID",		"DistroVariant" },
		{ NULL, NULL }
	};

	/* get all required os-release keys */
	os_release = fwupd_get_os_release (error);
	if (os_release == NULL)
		return FALSE;
	for (guint i = 0; distro_kv[i].key != NULL; i++) {
		const gchar *tmp = g_hash_table_lookup (os_release, distro_kv[i].key);
		if (tmp != NULL) {
			g_hash_table_insert (hash,
					     g_strdup (distro_kv[i].val),
					     g_strdup (tmp));
		}
	}
	return TRUE;
}

static gchar *
fu_engine_get_proc_cmdline (GError **error)
{
	gsize bufsz = 0;
	g_autofree gchar *buf = NULL;
	g_autoptr(GString) cmdline_safe = g_string_new (NULL);
	const gchar *ignore[] = {
		"",
		"auto",
		"boot",
		"BOOT_IMAGE",
		"console",
		"cryptdevice",
		"cryptkey",
		"earlycon",
		"earlyprintk",
		"ether",
		"initrd",
		"ip",
		"LANG",
		"loglevel",
		"luks.key",
		"luks.name",
		"luks.options",
		"luks.uuid",
		"mount.usr",
		"mount.usrflags",
		"mount.usrfstype",
		"netdev",
		"netroot",
		"nfsaddrs",
		"nfs.nfs4_unique_id",
		"nfsroot",
		"noplymouth",
		"ostree",
		"quiet",
		"rd.dm.uuid",
		"rd.luks.allow-discards",
		"rd.luks.key",
		"rd.luks.name",
		"rd.luks.options",
		"rd.luks.uuid",
		"rd.lvm.lv",
		"rd.lvm.vg",
		"rd.md.uuid",
		"rd.systemd.mask",
		"rd.systemd.wants",
		"resume",
		"resumeflags",
		"rhgb",
		"ro",
		"root",
		"rootflags",
		"roothash",
		"rw",
		"showopts",
		"splash",
		"swap",
		"systemd.mask",
		"systemd.unit",
		"systemd.verity_root_data",
		"systemd.verity_root_hash",
		"systemd.wants",
		"verbose",
		"vt.handoff",
		"zfs",
		NULL, /* last entry */
	};

	/* get a PII-safe kernel command line */
	if (!g_file_get_contents ("/proc/cmdline", &buf, &bufsz, error))
		return NULL;
	if (bufsz > 0) {
		g_auto(GStrv) tokens = fu_common_strnsplit (buf, bufsz - 1, " ", -1);
		for (guint i = 0; tokens[i] != NULL; i++) {
			g_auto(GStrv) kv = NULL;
			if (strlen (tokens[i]) == 0)
				continue;
			kv = g_strsplit (tokens[i], "=", 2);
			if (g_strv_contains (ignore, kv[0]))
				continue;
			if (cmdline_safe->len > 0)
				g_string_append (cmdline_safe, " ");
			g_string_append (cmdline_safe, tokens[i]);
		}
	}
	return g_string_free (g_steal_pointer (&cmdline_safe), FALSE);
}

static gchar *
fu_engine_get_kernel_cmdline (GError **error)
{
	/* Linuxish */
	if (g_file_test ("/proc/cmdline", G_FILE_TEST_EXISTS))
		return fu_engine_get_proc_cmdline (error);

	/* BSDish */
	return fu_kenv_get_string ("kernel_options", error);
}

static gboolean
fu_engine_get_report_metadata_kernel_cmdline (GHashTable *hash, GError **error)
{
	g_autofree gchar *cmdline = fu_engine_get_kernel_cmdline (error);
	if (cmdline == NULL)
		return FALSE;
	if (cmdline[0] != '\0') {
		g_hash_table_insert (hash,
				     g_strdup ("KernelCmdline"),
				     g_steal_pointer (&cmdline));
	}
	return TRUE;
}

GHashTable *
fu_engine_get_report_metadata (FuEngine *self, GError **error)
{
	const gchar *tmp;
	gchar *btime;
#ifdef HAVE_UTSNAME_H
	struct utsname name_tmp;
#endif
	g_autoptr(GHashTable) hash = NULL;
	g_autoptr(GList) compile_keys = g_hash_table_get_keys (self->compile_versions);
	g_autoptr(GList) runtime_keys = g_hash_table_get_keys (self->runtime_versions);

	/* convert all the runtime and compile-time versions */
	hash = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	for (GList *l = compile_keys; l != NULL; l = l->next) {
		const gchar *id = l->data;
		const gchar *version = g_hash_table_lookup (self->compile_versions, id);
		g_hash_table_insert (hash,
				     g_strdup_printf ("CompileVersion(%s)", id),
				     g_strdup (version));
	}
	for (GList *l = runtime_keys; l != NULL; l = l->next) {
		const gchar *id = l->data;
		const gchar *version = g_hash_table_lookup (self->runtime_versions, id);
		g_hash_table_insert (hash,
				     g_strdup_printf ("RuntimeVersion(%s)", id),
				     g_strdup (version));
	}
	if (!fu_engine_get_report_metadata_os_release (hash, error))
		return NULL;
	if (!fu_engine_get_report_metadata_kernel_cmdline (hash, error))
		return NULL;

	/* DMI data */
	tmp = fu_context_get_hwid_value (self->ctx, FU_HWIDS_KEY_PRODUCT_NAME);
	if (tmp != NULL)
		g_hash_table_insert (hash, g_strdup ("HostProduct"), g_strdup (tmp));
	tmp = fu_context_get_hwid_value (self->ctx, FU_HWIDS_KEY_FAMILY);
	if (tmp != NULL)
		g_hash_table_insert (hash, g_strdup ("HostFamily"), g_strdup (tmp));
	tmp = fu_context_get_hwid_value (self->ctx, FU_HWIDS_KEY_PRODUCT_SKU);
	if (tmp != NULL)
		g_hash_table_insert (hash, g_strdup ("HostSku"), g_strdup (tmp));
	tmp = fu_context_get_hwid_value (self->ctx, FU_HWIDS_KEY_MANUFACTURER);
	if (tmp != NULL)
		g_hash_table_insert (hash, g_strdup ("HostVendor"), g_strdup (tmp));

	/* kernel version is often important for debugging failures */
#ifdef HAVE_UTSNAME_H
	memset (&name_tmp, 0, sizeof (struct utsname));
	if (uname (&name_tmp) >= 0) {
		g_hash_table_insert (hash,
				     g_strdup ("CpuArchitecture"),
				     g_strdup (name_tmp.machine));
		g_hash_table_insert (hash,
				     g_strdup ("KernelName"),
				     g_strdup (name_tmp.sysname));
		g_hash_table_insert (hash,
				     g_strdup ("KernelVersion"),
				     g_strdup (name_tmp.release));
	}
#endif

	/* add the kernel boot time so we can detect a reboot */
	btime = fu_engine_get_boot_time ();
	if (btime != NULL)
		g_hash_table_insert (hash, g_strdup ("BootTime"), btime);

	return g_steal_pointer (&hash);
}

/**
 * fu_engine_composite_prepare:
 * @self: a #FuEngine
 * @devices: (element-type #FuDevice): devices that will be updated
 * @error: (nullable): optional return location for an error
 *
 * Calls into the plugin loader, informing each plugin of the pending upgrade(s).
 *
 * Any failure in any plugin will abort all of the actions before they are started.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_composite_prepare (FuEngine *self, GPtrArray *devices, GError **error)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		if (!fu_plugin_runner_composite_prepare (plugin_tmp, devices, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * fu_engine_composite_cleanup:
 * @self: a #FuEngine
 * @devices: (element-type #FuDevice): devices that will be updated
 * @error: (nullable): optional return location for an error
 *
 * Calls into the plugin loader, informing each plugin of the pending upgrade(s).
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_composite_cleanup (FuEngine *self, GPtrArray *devices, GError **error)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		if (!fu_plugin_runner_composite_cleanup (plugin_tmp, devices, error))
			return FALSE;
	}
	return TRUE;
}

/**
 * fu_engine_install_tasks:
 * @self: a #FuEngine
 * @request: a #FuEngineRequest
 * @install_tasks: (element-type FuInstallTask): a device
 * @blob_cab: the #GBytes of the .cab file
 * @flags: install flags, e.g. %FWUPD_DEVICE_FLAG_UPDATABLE
 * @error: (nullable): optional return location for an error
 *
 * Installs a specific firmware file on one or more install tasks.
 *
 * By this point all the requirements and tests should have been done in
 * fu_engine_check_requirements() so this should not fail before running
 * the plugin loader.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_install_tasks (FuEngine *self,
			 FuEngineRequest *request,
			 GPtrArray *install_tasks,
			 GBytes *blob_cab,
			 FwupdInstallFlags flags,
			 GError **error)
{
	FwupdFeatureFlags feature_flags = fu_engine_request_get_feature_flags (request);
	g_autoptr(FuIdleLocker) locker = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_new = NULL;

	/* do not allow auto-shutdown during this time */
	locker = fu_idle_locker_new (self->idle, "update");
	g_assert (locker != NULL);

	/* notify the plugins about the composite action */
	devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < install_tasks->len; i++) {
		FuInstallTask *task = g_ptr_array_index (install_tasks, i);
		g_debug ("composite update %u: %s", i + 1,
			 fu_device_get_id (fu_install_task_get_device (task)));
		g_ptr_array_add (devices, g_object_ref (fu_install_task_get_device (task)));
	}
	if (!fu_engine_composite_prepare (self, devices, error)) {
		g_prefix_error (error, "failed to prepare composite action: ");
		return FALSE;
	}

	/* all authenticated, so install all the things */
	for (guint i = 0; i < install_tasks->len; i++) {
		FuInstallTask *task = g_ptr_array_index (install_tasks, i);
		if (!fu_engine_install (self, task, blob_cab,
					flags, feature_flags,
					error)) {
			g_autoptr(GError) error_local = NULL;
			if (!fu_engine_composite_cleanup (self, devices, &error_local)) {
				g_warning ("failed to cleanup failed composite action: %s",
					   error_local->message);
			}
			return FALSE;
		}
	}

	/* set all the device statuses back to unknown */
	for (guint i = 0; i < install_tasks->len; i++) {
		FuInstallTask *task = g_ptr_array_index (install_tasks, i);
		FuDevice *device = fu_install_task_get_device (task);
		fu_device_set_status (device, FWUPD_STATUS_UNKNOWN);
	}

	/* get a new list of devices in case they replugged */
	devices_new = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device;
		g_autoptr(FuDevice) device_new = NULL;
		g_autoptr(GError) error_local = NULL;
		device = g_ptr_array_index (devices, i);
		device_new = fu_device_list_get_by_id (self->device_list,
						       fu_device_get_id (device),
						       &error_local);
		if (device_new == NULL) {
			g_debug ("failed to find new device: %s",
				 error_local->message);
			continue;
		}
		g_ptr_array_add (devices_new, g_steal_pointer (&device_new));
	}

	/* notify the plugins about the composite action */
	if (!fu_engine_composite_cleanup (self, devices_new, error)) {
		g_prefix_error (error, "failed to cleanup composite action: ");
		return FALSE;
	}

	/* success */
	return TRUE;
}

static FwupdRelease *
fu_engine_create_release_metadata (FuEngine *self,
				   FuDevice *device,
				   FuPlugin *plugin,
				   GError **error)
{
	GPtrArray *metadata_sources;
	g_autoptr(FwupdRelease) release = fwupd_release_new ();
	g_autoptr(GHashTable) metadata_device = NULL;
	g_autoptr(GHashTable) metadata_hash = NULL;

	/* build the version metadata */
	metadata_hash = fu_engine_get_report_metadata (self, error);
	if (metadata_hash == NULL)
		return NULL;
	fwupd_release_add_metadata (release, metadata_hash);
	if (fu_plugin_get_report_metadata (plugin) != NULL)
		fwupd_release_add_metadata (release, fu_plugin_get_report_metadata (plugin));
	metadata_device = fu_device_report_metadata_pre (device);
	if (metadata_device != NULL)
		fwupd_release_add_metadata (release, metadata_device);

	/* allow other plugins to contribute metadata too */
	metadata_sources = fu_plugin_get_rules (plugin, FU_PLUGIN_RULE_METADATA_SOURCE);
	if (metadata_sources != NULL) {
		for (guint i = 0; i < metadata_sources->len; i++) {
			FuPlugin *plugin_tmp;
			const gchar *plugin_name = g_ptr_array_index (metadata_sources, i);
			g_autoptr(GError) error_local = NULL;

			plugin_tmp = fu_plugin_list_find_by_name (self->plugin_list,
								  plugin_name,
								  &error_local);
			if (plugin_tmp == NULL) {
				g_warning ("could not add metadata for %s: %s",
					   plugin_name,
					   error_local->message);
				continue;
			}
			if (fu_plugin_get_report_metadata (plugin_tmp) != NULL) {
				fwupd_release_add_metadata (release,
							    fu_plugin_get_report_metadata (plugin_tmp));
			}
		}
	}
	return g_steal_pointer (&release);
}

static gboolean
fu_engine_is_running_offline (FuEngine *self)
{
#ifdef HAVE_SYSTEMD
	g_autofree gchar *default_target = NULL;
	g_autoptr(GError) error = NULL;
	default_target = fu_systemd_get_default_target (&error);
	if (default_target == NULL) {
		g_warning ("failed to get default.target: %s", error->message);
		return FALSE;
	}
	return g_strcmp0 (default_target, "system-update.target") == 0;
#else
	return FALSE;
#endif
}

static gboolean
fu_engine_offline_setup (GError **error)
{
#ifdef HAVE_GIO_UNIX
	gint rc;
	g_autofree gchar *filename = NULL;
	g_autofree gchar *symlink_target = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	g_autofree gchar *trigger = fu_common_get_path (FU_PATH_KIND_OFFLINE_TRIGGER);

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* does already exist */
	filename = fu_common_realpath (trigger, NULL);
	if (g_strcmp0 (filename, symlink_target) == 0) {
		g_debug ("%s already points to %s, skipping creation",
			 trigger, symlink_target);
		return TRUE;
	}

	/* create symlink for the systemd-system-update-generator */
	rc = symlink (symlink_target, trigger);
	if (rc < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Failed to create symlink %s to %s: %s",
			     trigger, symlink_target, strerror (errno));
		return FALSE;
	}
	return TRUE;
#else
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "Not supported as <gio-unix.h> not available");
	return FALSE;
#endif

}

static gboolean
fu_engine_offline_invalidate (GError **error)
{
	g_autofree gchar *trigger = fu_common_get_path (FU_PATH_KIND_OFFLINE_TRIGGER);
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GFile) file1 = NULL;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	file1 = g_file_new_for_path (trigger);
	if (!g_file_query_exists (file1, NULL))
		return TRUE;
	if (!g_file_delete (file1, NULL, &error_local)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Cannot delete %s: %s",
			     trigger,
			     error_local->message);
		return FALSE;
	}
	return TRUE;
}

/**
 * fu_engine_schedule_update:
 * @self: a #FuEngine
 * @device: a device
 * @release: a release
 * @blob_cab: a data blob
 * @flags: install flags
 * @error: (nullable): optional return location for an error
 *
 * Schedule an offline update for the device
 *
 * Returns: #TRUE for success, #FALSE for failure
 *
 * Since: 1.3.5
 **/
gboolean
fu_engine_schedule_update (FuEngine *self,
			   FuDevice *device,
			   FwupdRelease *release,
			   GBytes *blob_cab,
			   FwupdInstallFlags flags,
			   GError **error)
{
	gchar tmpname[] = {"XXXXXX.cab"};
	g_autofree gchar *dirname = NULL;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuHistory) history = NULL;
	g_autoptr(GFile) file = NULL;

	/* id already exists */
	history = fu_history_new ();
	if ((flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_autoptr(FuDevice) res_tmp = NULL;
		res_tmp = fu_history_get_device_by_id (history, fu_device_get_id (device), NULL);
		if (res_tmp != NULL &&
		    fu_device_get_update_state (res_tmp) == FWUPD_UPDATE_STATE_PENDING) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_ALREADY_PENDING,
				     "%s is already scheduled to be updated",
				     fu_device_get_id (device));
			return FALSE;
		}
	}

	/* create directory */
	dirname = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	file = g_file_new_for_path (dirname);
	if (!g_file_query_exists (file, NULL)) {
		if (!g_file_make_directory_with_parents (file, NULL, error))
			return FALSE;
	}

	/* get a random filename */
	for (guint i = 0; i < 6; i++)
		tmpname[i] = (gchar) g_random_int_range ('A', 'Z');
	filename = g_build_filename (dirname, tmpname, NULL);

	/* just copy to the temp file */
	fu_device_set_status (device, FWUPD_STATUS_SCHEDULING);
	if (!g_file_set_contents (filename,
				  g_bytes_get_data (blob_cab, NULL),
				  (gssize) g_bytes_get_size (blob_cab),
				  error))
		return FALSE;

	/* schedule for next boot */
	g_debug ("schedule %s to be installed to %s on next boot",
		 filename, fu_device_get_id (device));
	fwupd_release_set_filename (release, filename);

	/* add to database */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT);
	fu_device_set_update_state (device, FWUPD_UPDATE_STATE_PENDING);
	if (!fu_history_add_device (history, device, release, error))
		return FALSE;

	/* next boot we run offline */
	fu_device_set_progress (device, 100);
	return fu_engine_offline_setup (error);
}

static gboolean
fu_engine_install_release (FuEngine *self,
			   FuDevice *device_orig,
			   XbNode *component,
			   XbNode *rel,
			   FwupdInstallFlags flags,
			   FwupdFeatureFlags feature_flags,
			   GError **error)
{
	FuPlugin *plugin;
	FwupdVersionFormat fmt;
	GBytes *blob_fw;
	const gchar *tmp;
	g_autofree gchar *version_orig = NULL;
	g_autofree gchar *version_rel = NULL;
	g_autoptr(FuDevice) device_tmp = NULL;
	g_autoptr(FuDevice) device = g_object_ref (device_orig);
	g_autoptr(GBytes) blob_fw2 = NULL;
	g_autoptr(GError) error_local = NULL;

	/* get per-release firmware blob */
	blob_fw = xb_node_get_data (rel, "fwupd::FirmwareBlob");
	if (blob_fw == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "Failed to get firmware blob from release");
		return FALSE;
	}

	/* use a bubblewrap helper script to build the firmware */
	tmp = g_object_get_data (G_OBJECT (component), "fwupd::BuilderScript");
	if (tmp != NULL) {
		const gchar *tmp2 = g_object_get_data (G_OBJECT (component), "fwupd::BuilderOutput");
		if (tmp2 == NULL)
			tmp2 = "firmware.bin";
		blob_fw2 = fu_common_firmware_builder (blob_fw, tmp, tmp2, error);
		if (blob_fw2 == NULL)
			return FALSE;
	} else {
		blob_fw2 = g_bytes_ref (blob_fw);
	}

	/* get the plugin */
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;

	/* schedule this for the next reboot if not in system-update.target,
	 * but first check if allowed on battery power */
	version_rel = fu_engine_get_release_version (self, device, rel, error);
	if (version_rel == NULL) {
		g_prefix_error (error, "failed to get release version: ");
		return FALSE;
	}

	/* add device to database */
	if ((flags & FWUPD_INSTALL_FLAG_NO_HISTORY) == 0) {
		g_autoptr(FwupdRelease) release_tmp = NULL;
		release_tmp = fu_engine_create_release_metadata (self, device, plugin, error);
		if (release_tmp == NULL)
			return FALSE;
		tmp = xb_node_query_text (component,
					  "releases/release/checksum[@target='container']",
					  NULL);
		if (tmp != NULL)
			fwupd_release_add_checksum (release_tmp, tmp);
		fwupd_release_set_version (release_tmp, version_rel);
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
		if (!fu_history_add_device (self->history, device, release_tmp, error))
			return FALSE;
	}

	/* install firmware blob */
	version_orig = g_strdup (fu_device_get_version (device));
	if (!fu_engine_install_blob (self, device, blob_fw2,
				     flags, feature_flags,
				     &error_local)) {
		fu_device_set_status (device, FWUPD_STATUS_IDLE);
		if (g_error_matches (error_local,
				     FWUPD_ERROR,
				     FWUPD_ERROR_AC_POWER_REQUIRED) ||
		    g_error_matches (error_local,
				     FWUPD_ERROR,
				     FWUPD_ERROR_BATTERY_LEVEL_TOO_LOW) ||
		    g_error_matches (error_local,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NEEDS_USER_ACTION) ||
		    g_error_matches (error_local,
				     FWUPD_ERROR,
				     FWUPD_ERROR_BROKEN_SYSTEM)) {
			fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED_TRANSIENT);
		} else {
			fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
		}
		fu_device_set_update_error (device, error_local->message);
		if ((flags & FWUPD_INSTALL_FLAG_NO_HISTORY) == 0 &&
		    !fu_history_modify_device (self->history, device, error)) {
			return FALSE;
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}

	/* the device may have changed */
	device_tmp = fu_device_list_get_by_id (self->device_list,
					       fu_device_get_id (device),
					       error);
	if (device_tmp == NULL) {
		g_prefix_error (error, "failed to get device after install: ");
		return FALSE;
	}
	g_set_object (&device, device_tmp);

	/* update database */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT) ||
	    fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN)) {
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_NEEDS_REBOOT);
		if ((flags & FWUPD_INSTALL_FLAG_NO_HISTORY) == 0 &&
		    !fu_history_modify_device (self->history, device, error))
			return FALSE;
		/* success */
		return TRUE;
	}

	/* for online updates, verify the version changed if not a re-install */
	fmt = fu_device_get_version_format (device);
	if (version_rel != NULL &&
	    fu_common_vercmp_full (version_orig, version_rel, fmt) != 0 &&
	    fu_common_vercmp_full (version_orig, fu_device_get_version (device), fmt) == 0 &&
	    !fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
		g_autofree gchar *str = NULL;
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
		str = g_strdup_printf ("device version not updated on success, %s != %s",
				       version_rel, fu_device_get_version (device));
		fu_device_set_update_error (device, str);
	}

	/* success */
	if ((flags & FWUPD_INSTALL_FLAG_NO_HISTORY) == 0 &&
	    !fu_history_modify_device (self->history, device, error))
		return FALSE;

	/* make the UI update */
	fu_engine_emit_changed (self);

	return TRUE;
}

typedef struct {
	gboolean	 ret;
	GError		**error;
	FuEngine	*self;
	FuDevice	*device;
} FuEngineSortHelper;

static gint
fu_engine_sort_release_versions_cb (gconstpointer a, gconstpointer b, gpointer user_data)
{
	FuEngineSortHelper *helper = (FuEngineSortHelper *) user_data;
	XbNode *na = *((XbNode **) a);
	XbNode *nb = *((XbNode **) b);
	g_autofree gchar *va = NULL;
	g_autofree gchar *vb = NULL;

	/* already failed */
	if (!helper->ret)
		return 0;

	/* get the semver from the release */
	va = fu_engine_get_release_version (helper->self, helper->device, na, helper->error);
	if (va == NULL) {
		g_prefix_error (helper->error, "failed to get release version: ");
		return 0;
	}
	vb = fu_engine_get_release_version (helper->self, helper->device, nb, helper->error);
	if (vb == NULL) {
		g_prefix_error (helper->error, "failed to get release version: ");
		return 0;
	}
	return fu_common_vercmp_full (va, vb, fu_device_get_version_format (helper->device));
}

static gboolean
fu_engine_sort_releases (FuEngine *self, FuDevice *device, GPtrArray *rels, GError **error)
{
	FuEngineSortHelper helper = {
		.ret = TRUE,
		.self = self,
		.device = device,
		.error = error,
	};
	g_ptr_array_sort_with_data (rels, fu_engine_sort_release_versions_cb, &helper);
	return helper.ret;
}

/**
 * fu_engine_install:
 * @self: a #FuEngine
 * @task: a #FuInstallTask
 * @blob_cab: the #GBytes of the .cab file
 * @flags: install flags, e.g. %FWUPD_INSTALL_FLAG_ALLOW_OLDER
 * @feature_flags: feature flags, e.g. %FWUPD_FEATURE_FLAG_NONE
 * @error: (nullable): optional return location for an error
 *
 * Installs a specific firmware file on a device.
 *
 * By this point all the requirements and tests should have been done in
 * fu_engine_check_requirements() so this should not fail before running
 * the plugin loader.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_install (FuEngine *self,
		   FuInstallTask *task,
		   GBytes *blob_cab,
		   FwupdInstallFlags flags,
		   FwupdFeatureFlags feature_flags,
		   GError **error)
{
	XbNode *component = fu_install_task_get_component (task);
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(XbNode) rel_newest = NULL;
#if LIBXMLB_CHECK_VERSION(0,2,0)
	g_autoptr(XbQuery) query = NULL;
#endif

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (XB_IS_NODE (component), FALSE);
	g_return_val_if_fail (blob_cab != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* not in bootloader mode */
	device = g_object_ref (fu_install_task_get_device (task));
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		const gchar *caption = NULL;
		caption = xb_node_query_text (component,
					      "screenshots/screenshot/caption",
					      NULL);
		if (caption != NULL) {
			const gchar *image = NULL;
			g_autofree gchar *tmp = NULL;
			tmp = g_strdup_printf ("Device %s needs to manually be put in update mode: %s",
					       fu_device_get_name (device), caption);
			fu_device_set_update_message_kind (device, FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE);
			fu_device_set_update_message (device, tmp);

			/* image is even more optional */
			image = xb_node_query_text (component,
						    "screenshots/screenshot/image",
						    NULL);
			if (image != NULL)
				fu_device_set_update_image (device, image);
		}
	}

	/* get the newest version */
#if LIBXMLB_CHECK_VERSION(0,2,0)
	query = xb_query_new_full (xb_node_get_silo (component),
				   "releases/release",
				   XB_QUERY_FLAG_FORCE_NODE_CACHE,
				   error);
	if (query == NULL)
		return FALSE;
	rel_newest = xb_node_query_first_full (component, query, &error_local);
#else
	rel_newest = xb_node_query_first (component, "releases/release", &error_local);
#endif
	if (rel_newest == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "No releases in the firmware component: %s",
			     error_local->message);
		return FALSE;
	}

	/* schedule this for the next reboot if not in system-update.target,
	 * but first check if allowed on battery power */
	if ((flags & FWUPD_INSTALL_FLAG_OFFLINE) > 0 &&
	    !fu_engine_is_running_offline (self)) {
		FuPlugin *plugin;
		g_autoptr(FwupdRelease) release_tmp = NULL;
		g_autofree gchar *version_rel = NULL;
		version_rel = fu_engine_get_release_version (self, device, rel_newest, error);
		if (version_rel == NULL) {
			g_prefix_error (error, "failed to get release version: ");
			return FALSE;
		}
		plugin = fu_plugin_list_find_by_name (self->plugin_list, "upower", NULL);
		if (plugin != NULL) {
			if (!fu_plugin_runner_update_prepare (plugin, flags, device, error))
				return FALSE;
		}
		release_tmp = fu_engine_create_release_metadata (self, device, plugin, error);
		if (release_tmp == NULL)
			return FALSE;
		fwupd_release_set_version (release_tmp, version_rel);
		return fu_engine_schedule_update (self, device, release_tmp,
						  blob_cab, flags, error);
	}

	/* install each intermediate release, or install only the newest version */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES)) {
		g_autoptr(GPtrArray) rels = NULL;
#if LIBXMLB_CHECK_VERSION(0,2,0)
		rels = xb_node_query_full (component, query, &error_local);
#else
		rels = xb_node_query (component, "releases/release", 0, &error_local);
#endif
		if (rels == NULL) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "No releases in the firmware component: %s",
				     error_local->message);
			return FALSE;
		}
		if (!fu_engine_sort_releases (self, device, rels, error))
			return FALSE;
		for (guint i = 0; i < rels->len; i++) {
			XbNode *rel = g_ptr_array_index (rels, i);
			if (!fu_engine_install_release (self,
							device,
							component,
							rel,
							flags,
							feature_flags,
							error))
				return FALSE;
		}
	} else {
		if (!fu_engine_install_release (self,
						device,
						component,
						rel_newest,
						flags,
						feature_flags,
						error))
			return FALSE;
	}

	/* success */
	fu_device_set_update_state (device, FWUPD_UPDATE_STATE_SUCCESS);
	return TRUE;
}

/**
 * fu_engine_get_plugins:
 * @self: a #FuPluginList
 *
 * Gets all the plugins that have been added.
 *
 * Returns: (transfer none) (element-type FuPlugin): the plugins
 *
 * Since: 1.0.8
 **/
GPtrArray *
fu_engine_get_plugins (FuEngine *self)
{
	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	return fu_plugin_list_get_all (self->plugin_list);
}

/**
 * fu_engine_get_device:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Gets a specific device.
 *
 * Returns: (transfer full): a device, or %NULL if not found
 **/
FuDevice *
fu_engine_get_device (FuEngine *self, const gchar *device_id, GError **error)
{
	g_autoptr(FuDevice) device = NULL;

	/* wait for any device to disconnect and reconnect */
	if (!fu_device_list_wait_for_replug (self->device_list, error)) {
		g_prefix_error (error, "failed to wait for detach replug: ");
		return NULL;
	}

	/* get the new device */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL) {
		g_prefix_error (error, "failed to get device after replug: ");
		return NULL;
	}

	/* success */
	return g_steal_pointer (&device);
}

/* same as FuDevice->prepare, but with the device open */
static gboolean
fu_engine_device_prepare (FuEngine *self,
			  FuDevice *device,
			  FwupdInstallFlags flags,
			  GError **error)
{
	g_autoptr(FuDeviceLocker) locker = fu_device_locker_new (device, error);
	if (locker == NULL) {
		g_prefix_error (error, "failed to open device for prepare: ");
		return FALSE;
	}

	/* check battery level is sane */
	if (fu_device_get_battery_level (device) > 0 &&
	    fu_device_get_battery_level (device) < fu_device_get_battery_threshold (device)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_BATTERY_LEVEL_TOO_LOW,
			     "battery level is too low: %u%%",
			     fu_device_get_battery_level (device));
		return FALSE;
	}

	return fu_device_prepare (device, flags, error);
}

/* same as FuDevice->cleanup, but with the device open */
static gboolean
fu_engine_device_cleanup (FuEngine *self,
			  FuDevice *device,
			  FwupdInstallFlags flags,
			  GError **error)
{
	g_autoptr(FuDeviceLocker) locker = NULL;

	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_WILL_DISAPPEAR)) {
		g_debug ("skipping device cleanup due to will-disappear flag");
		return TRUE;
	}

	locker = fu_device_locker_new (device, error);
	if (locker == NULL) {
		g_prefix_error (error, "failed to open device for cleanup: ");
		return FALSE;
	}
	return fu_device_cleanup (device, flags, error);
}

static gboolean
fu_engine_update_prepare (FuEngine *self,
			  FwupdInstallFlags flags,
			  const gchar *device_id,
			  GError **error)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;

	/* the device and plugin both may have changed */
	device = fu_engine_get_device (self, device_id, error);
	if (device == NULL)
		return FALSE;

	/* don't rely on a plugin clearing this */
	fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED);

	str = fu_device_to_string (device);
	g_debug ("prepare -> %s", str);
	if (!fu_engine_device_prepare (self, device, flags, error))
		return FALSE;
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		if (!fu_plugin_runner_update_prepare (plugin_tmp, flags, device, error))
			return FALSE;
	}

	/* wait for device to disconnect and reconnect */
	if (!fu_device_list_wait_for_replug (self->device_list, error)) {
		g_prefix_error (error, "failed to wait for prepare replug: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_engine_update_cleanup (FuEngine *self,
			  FwupdInstallFlags flags,
			  const gchar *device_id,
			  GError **error)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;

	/* the device and plugin both may have changed */
	device = fu_engine_get_device (self, device_id, error);
	if (device == NULL)
		return FALSE;
	str = fu_device_to_string (device);
	g_debug ("cleanup -> %s", str);
	if (!fu_engine_device_cleanup (self, device, flags, error))
		return FALSE;
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		if (!fu_plugin_runner_update_cleanup (plugin_tmp, flags, device, error))
			return FALSE;
	}

	/* wait for device to disconnect and reconnect */
	if (!fu_device_list_wait_for_replug (self->device_list, error)) {
		g_prefix_error (error, "failed to wait for cleanup replug: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_engine_update_detach (FuEngine *self,
			 const gchar *device_id,
			 FwupdFeatureFlags feature_flags,
			 GError **error)
{
	FuPlugin *plugin;
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;

	/* the device and plugin both may have changed */
	device = fu_engine_get_device (self, device_id, error);
	if (device == NULL)
		return FALSE;
	str = fu_device_to_string (device);
	g_debug ("detach -> %s", str);
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;
	if (!fu_plugin_runner_update_detach (plugin, device, error))
		return FALSE;

	/* support older clients without the ability to do immediate messages */
	if ((feature_flags & FWUPD_FEATURE_FLAG_IMMEDIATE_MESSAGE) == 0 &&
	    fu_device_get_update_message_kind (device) == FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE) {
		fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
		fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER);
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NEEDS_USER_ACTION,
				     fu_device_get_update_message (device));
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_engine_update_attach (FuEngine *self, const gchar *device_id, GError **error)
{
	FuPlugin *plugin;
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;

	/* the device and plugin both may have changed */
	device = fu_engine_get_device (self, device_id, error);
	if (device == NULL) {
		g_prefix_error (error, "failed to get device after update: ");
		return FALSE;
	}
	str = fu_device_to_string (device);
	g_debug ("attach -> %s", str);
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;

	if (!fu_plugin_runner_update_attach (plugin, device, error))
		return FALSE;
	return TRUE;
}

gboolean
fu_engine_activate (FuEngine *self, const gchar *device_id, GError **error)
{
	FuPlugin *plugin;
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (device_id != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* check the device exists */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return FALSE;
	str = fu_device_to_string (device);
	g_debug ("activate -> %s", str);
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;
	g_debug ("Activating %s", fu_device_get_name (device));

	if (!fu_plugin_runner_activate (plugin, device, error))
		return FALSE;

	fu_engine_emit_device_changed (self, device);
	fu_engine_emit_changed (self);

	return TRUE;
}

static gboolean
fu_engine_update_reload (FuEngine *self, const gchar *device_id, GError **error)
{
	FuPlugin *plugin;
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;

	/* the device and plugin both may have changed */
	device = fu_engine_get_device (self, device_id, error);
	if (device == NULL) {
		g_prefix_error (error, "failed to get device after update: ");
		return FALSE;
	}
	str = fu_device_to_string (device);
	g_debug ("reload -> %s", str);
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;

	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_WILL_DISAPPEAR)) {
		g_debug ("skipping reload due to will-disappear flag");
		return TRUE;
	}

	if (!fu_plugin_runner_update_reload (plugin, device, error)) {
		g_prefix_error (error, "failed to reload device: ");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_engine_update (FuEngine *self,
		  const gchar *device_id,
		  GBytes *blob_fw2,
		  FwupdInstallFlags flags,
		  GError **error)
{
	FuPlugin *plugin;
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDevice) device_pending = NULL;

	/* cancel the pending action */
	if (!fu_engine_offline_invalidate (error))
		return FALSE;

	/* the device and plugin both may have changed */
	device = fu_engine_get_device (self, device_id, error);
	if (device == NULL) {
		g_prefix_error (error, "failed to get device after detach: ");
		return FALSE;
	}
	device_pending = fu_history_get_device_by_id (self->history, device_id, NULL);
	str = fu_device_to_string (device);
	g_debug ("update -> %s", str);
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin == NULL)
		return FALSE;
	if (!fu_plugin_runner_update (plugin, device, blob_fw2, flags, error)) {
		g_autoptr(GError) error_attach = NULL;
		g_autoptr(GError) error_cleanup = NULL;

		/* attack back into runtime then cleanup */
		if (!fu_plugin_runner_update_attach (plugin,
						     device,
						     &error_attach)) {
			g_warning ("failed to attach device after failed update: %s",
				   error_attach->message);
		}
		if (!fu_engine_update_cleanup (self, flags, device_id, &error_cleanup)) {
			g_warning ("failed to update-cleanup after failed update: %s",
				   error_cleanup->message);
		}
		return FALSE;
	}

	/* cleanup */
	if (device_pending != NULL) {
		const gchar *tmp;
		FwupdRelease *release;

		/* update history database */
		fu_device_set_update_state (device, FWUPD_UPDATE_STATE_SUCCESS);
		if (!fu_history_modify_device (self->history, device, error))
			return FALSE;

		/* delete cab file */
		release = fu_device_get_release_default (device_pending);
		tmp = fwupd_release_get_filename (release);
		if (tmp != NULL && g_str_has_prefix (tmp, FWUPD_LIBEXECDIR)) {
			g_autoptr(GError) error_delete = NULL;
			g_autoptr(GFile) file = NULL;
			file = g_file_new_for_path (tmp);
			if (!g_file_delete (file, NULL, &error_delete)) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     "Failed to delete %s: %s",
					     tmp, error_delete->message);
				return FALSE;
			}
		}
	}
	return TRUE;
}

GBytes *
fu_engine_firmware_dump (FuEngine *self,
			 FuDevice *device,
			 FwupdInstallFlags flags,
			 GError **error)
{
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* open, read, close */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL) {
		g_prefix_error (error, "failed to open device for firmware read: ");
		return NULL;
	}
	return fu_device_dump_firmware (device, error);
}

gboolean
fu_engine_install_blob (FuEngine *self,
			FuDevice *device,
			GBytes *blob_fw,
			FwupdInstallFlags flags,
			FwupdFeatureFlags feature_flags,
			GError **error)
{
	guint retries = 0;
	g_autofree gchar *device_id = NULL;
	g_autoptr(GTimer) timer = g_timer_new ();

	/* test the firmware is not an empty blob */
	if (g_bytes_get_size (blob_fw) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "Firmware is invalid as has zero size");
		return FALSE;
	}

	/* mark this as modified even if we actually fail to do the update */
	fu_device_set_modified (device, (guint64) g_get_real_time () / G_USEC_PER_SEC);

	/* plugins can set FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED to run again, but they
	 * must return TRUE rather than an error */
	device_id = g_strdup (fu_device_get_id (device));
	do {
		g_autoptr(FuDevice) device_tmp = NULL;

		/* check for a loop */
		if (++retries > 5) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INTERNAL,
					     "aborting device write loop, limit 5");
			return FALSE;
		}

		/* signal to all the plugins the update is about to happen */
		if (!fu_engine_update_prepare (self, flags, device_id, error))
			return FALSE;

		/* detach to bootloader mode */
		if (!fu_engine_update_detach (self, device_id, feature_flags, error))
			return FALSE;

		/* install */
		if (!fu_engine_update (self, device_id, blob_fw, flags, error))
			return FALSE;

		/* attach into runtime mode */
		if (!fu_engine_update_attach (self, device_id, error))
			return FALSE;

		/* the device and plugin both may have changed */
		device_tmp = fu_engine_get_device (self, device_id, error);
		if (device_tmp == NULL)
			return FALSE;
		if (!fu_device_has_flag (device_tmp, FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED))
			break;

	} while (TRUE);

	/* get the new version number */
	if (!fu_engine_update_reload (self, device_id, error))
		return FALSE;

	/* signal to all the plugins the update has happened */
	if (!fu_engine_update_cleanup (self, flags, device_id, error))
		return FALSE;

	/* make the UI update */
	fu_engine_set_status (self, FWUPD_STATUS_IDLE);
	g_debug ("Updating %s took %f seconds", fu_device_get_name (device),
		 g_timer_elapsed (timer, NULL));
	return TRUE;
}

static FuDevice *
fu_engine_get_item_by_id_fallback_history (FuEngine *self, const gchar *id, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* not a wildcard */
	if (g_strcmp0 (id, FWUPD_DEVICE_ID_ANY) != 0) {
		g_autoptr(FuDevice) dev = NULL;
		g_autoptr(GError) error_local = NULL;

		/* get this one device */
		dev = fu_history_get_device_by_id (self->history, id, &error_local);
		if (dev == NULL) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "Failed to find %s in history database: %s",
				     id, error_local->message);
			return NULL;
		}

		/* only useful */
		if (fu_device_get_update_state (dev) == FWUPD_UPDATE_STATE_SUCCESS ||
		    fu_device_get_update_state (dev) == FWUPD_UPDATE_STATE_FAILED_TRANSIENT ||
		    fu_device_get_update_state (dev) == FWUPD_UPDATE_STATE_FAILED) {
			return g_steal_pointer (&dev);
		}

		/* nothing in database */
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "Device %s has no results to report",
			     fu_device_get_id (dev));
		return NULL;
	}

	/* allow '*' for any */
	devices = fu_history_get_devices (self->history, error);
	if (devices == NULL)
		return NULL;
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		if (fu_device_get_update_state (dev) == FWUPD_UPDATE_STATE_SUCCESS ||
		    fu_device_get_update_state (dev) == FWUPD_UPDATE_STATE_FAILED_TRANSIENT ||
		    fu_device_get_update_state (dev) == FWUPD_UPDATE_STATE_FAILED)
			return g_object_ref (dev);
	}
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "Failed to find any useful results to report");
	return NULL;
}

/* for the self tests */
void
fu_engine_set_silo (FuEngine *self, XbSilo *silo)
{
	g_return_if_fail (FU_IS_ENGINE (self));
	g_return_if_fail (XB_IS_SILO (silo));
	g_set_object (&self->silo, silo);
}

static gboolean
fu_engine_appstream_upgrade_cb (XbBuilderFixup *self,
				XbBuilderNode *bn,
				gpointer user_data,
				GError **error)
{
	if (g_strcmp0 (xb_builder_node_get_element (bn), "metadata") == 0)
		xb_builder_node_set_element (bn, "custom");
	return TRUE;
}

static XbBuilderSource *
fu_engine_create_metadata_builder_source (FuEngine *self,
					  const gchar *fn,
					  GError **error)
{
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(XbBuilderSource) source = xb_builder_source_new ();
	g_autofree gchar *xml = NULL;

	g_debug ("building metadata for %s", fn);
	blob = fu_common_get_contents_bytes (fn, error);
	if (blob == NULL)
		return NULL;

	/* convert the silo for the CAB into a XbBuilderSource */
	silo = fu_engine_get_silo_from_blob (self, blob, error);
	if (silo == NULL)
		return NULL;
	xml = xb_silo_export (silo, XB_NODE_EXPORT_FLAG_NONE, error);
	if (xml == NULL)
		return NULL;
	if (!xb_builder_source_load_xml (source, xml,
					 XB_BUILDER_SOURCE_FLAG_NONE,
					 error))
		return NULL;
	return g_steal_pointer (&source);
}

static gboolean
fu_engine_create_metadata (FuEngine *self, XbBuilder *builder,
			   FwupdRemote *remote, GError **error)
{
	g_autoptr(GPtrArray) files = NULL;
	const gchar *path;

	/* find all files in directory */
	path = fwupd_remote_get_filename_cache (remote);
	files = fu_common_get_files_recursive (path, error);
	if (files == NULL)
		return FALSE;

	/* add each source */
	for (guint i = 0; i < files->len; i++) {
		g_autoptr(XbBuilderNode) custom = NULL;
		g_autoptr(XbBuilderSource) source = NULL;
		g_autoptr(GError) error_local = NULL;
		const gchar *fn = g_ptr_array_index (files, i);
		g_autofree gchar *fn_lowercase = g_ascii_strdown (fn, -1);

		/* check is cab file */
		if (!g_str_has_suffix (fn_lowercase, ".cab")) {
			g_debug ("ignoring: %s", fn);
			continue;
		}

		/* build source for file */
		source = fu_engine_create_metadata_builder_source (self, fn, &error_local);
		if (source == NULL) {
			g_warning ("failed to create builder source: %s",
				   error_local->message);
			continue;
		}

		/* add metadata */
		custom = xb_builder_node_new ("custom");
		xb_builder_node_insert_text (custom,
					     "value", fn,
					     "key", "fwupd::FilenameCache",
					     NULL);
		xb_builder_node_insert_text (custom,
					     "value", fwupd_remote_get_id (remote),
					     "key", "fwupd::RemoteId",
					     NULL);
		xb_builder_source_set_info (source, custom);
		xb_builder_import_source (builder, source);
	}
	return TRUE;
}

static void
fu_engine_ensure_device_supported (FuEngine *self, FuDevice *device)
{
	gboolean is_supported = FALSE;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();

	/* all flags set */
	fu_engine_request_set_feature_flags (request, ~0);

	/* get all releases that pass the requirements */
	releases = fu_engine_get_releases_for_device (self,
						      request,
						      device,
						      &error);
	if (releases == NULL) {
		if (!g_error_matches (error,
				      FWUPD_ERROR,
				      FWUPD_ERROR_NOTHING_TO_DO) &&
		    !g_error_matches (error,
				      FWUPD_ERROR,
				      FWUPD_ERROR_NOT_SUPPORTED)) {
			g_warning ("failed to get releases for %s: %s",
				   fu_device_get_name (device),
				   error->message);
		}
	} else {
		if (releases->len > 0)
			is_supported = TRUE;
	}

	/* was supported, now unsupported */
	if (!is_supported) {
		if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_SUPPORTED)) {
			fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_SUPPORTED);
			fu_engine_emit_device_changed (self, device);
		}
		return;
	}

	/* was unsupported, now supported */
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_SUPPORTED)) {
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_SUPPORTED);
		fu_engine_emit_device_changed (self, device);
	}
}

static void
fu_engine_md_refresh_device_name (FuEngine *self, FuDevice *device, XbNode *component)
{
	const gchar *name = NULL;

	/* require data */
	if (component == NULL)
		return;

	/* copy 1:1 */
	name = xb_node_query_text (component, "name", NULL);
	if (name != NULL) {
		fu_device_set_name (device, name);
		fu_device_remove_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME);
	}
}

static void
fu_engine_md_refresh_device_icon (FuEngine *self, FuDevice *device, XbNode *component)
{
	const gchar *icon = NULL;

	/* require data */
	if (component == NULL)
		return;

	/* copy 1:1 */
	icon = xb_node_query_text (component, "icon", NULL);
	if (icon != NULL) {
		fu_device_add_icon (device, icon);
		fu_device_remove_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON);
	}
}

static const gchar *
fu_common_device_category_to_name (const gchar *cat)
{
	if (g_strcmp0 (cat, "X-EmbeddedController") == 0)
		return "Embedded Controller";
	if (g_strcmp0 (cat, "X-ManagementEngine") == 0)
		return "Intel Management Engine";
	if (g_strcmp0 (cat, "X-CorporateManagementEngine") == 0)
		return "Intel Management Engine";
	if (g_strcmp0 (cat, "X-ConsumerManagementEngine") == 0)
		return "Intel Management Engine";
	if (g_strcmp0 (cat, "X-ThunderboltController") == 0)
		return "Thunderbolt Controller";
	if (g_strcmp0 (cat, "X-PlatformSecurityProcessor") == 0)
		return "Platform Security Processor";
	if (g_strcmp0 (cat, "X-CpuMicrocode") == 0)
		return "CPU Microcode";
	if (g_strcmp0 (cat, "X-Battery") == 0)
		return "Battery";
	if (g_strcmp0 (cat, "X-Camera") == 0)
		return "Camera";
	if (g_strcmp0 (cat, "X-TPM") == 0)
		return "TPM";
	if (g_strcmp0 (cat, "X-Touchpad") == 0)
		return "Touchpad";
	if (g_strcmp0 (cat, "X-Mouse") == 0)
		return "Mouse";
	if (g_strcmp0 (cat, "X-Keyboard") == 0)
		return "Keyboard";
	return NULL;
}

static void
fu_engine_md_refresh_device_name_category (FuEngine *self, FuDevice *device, XbNode *component)
{
	const gchar *name = NULL;
	g_autoptr(GPtrArray) cats = NULL;

	/* require data */
	if (component == NULL)
		return;

	/* get AppStream and safe-compat categories */
	cats = xb_node_query (component, "categories/category|X-categories/category", 0, NULL);
	if (cats == NULL)
		return;
	for (guint i = 0; i < cats->len; i++) {
		XbNode *n = g_ptr_array_index (cats, i);
		name = fu_common_device_category_to_name (xb_node_get_text (n));
		if (name != NULL)
			break;
	}
	if (name != NULL) {
		fu_device_set_name (device, name);
		fu_device_remove_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME_CATEGORY);
	}
}

static void
_g_ptr_array_reverse (GPtrArray *array)
{
	guint last_idx = array->len - 1;
	for (guint i = 0; i < array->len / 2; i++) {
		gpointer tmp = array->pdata[i];
		array->pdata[i] = array->pdata[last_idx - i];
		array->pdata[last_idx - i] = tmp;
	}
}

static void
fu_engine_md_refresh_device_verfmt (FuEngine *self, FuDevice *device, XbNode *component)
{
	FwupdVersionFormat verfmt = FWUPD_VERSION_FORMAT_UNKNOWN;
	g_autoptr(GPtrArray) verfmts = NULL;

	/* require data */
	if (component == NULL)
		return;

	/* get metadata */
	verfmts = xb_node_query (component, "custom/value[@key='LVFS::VersionFormat']", 0, NULL);
	if (verfmts == NULL)
		return;
	_g_ptr_array_reverse (verfmts);
	for (guint i = 0; i < verfmts->len; i++) {
		XbNode *value = g_ptr_array_index (verfmts, i);
		verfmt = fwupd_version_format_from_string (xb_node_get_text (value));
		if (verfmt != FWUPD_VERSION_FORMAT_UNKNOWN)
			break;
	}

	/* found and different to existing */
	if (verfmt != FWUPD_VERSION_FORMAT_UNKNOWN &&
	    fu_device_get_version_format (device) != verfmt) {
		fu_device_set_version_format (device, verfmt);
		if (fu_device_get_version_raw (device) != 0x0) {
			g_autofree gchar *version = NULL;
			version = fu_common_version_from_uint32 (fu_device_get_version_raw (device), verfmt);
			fu_device_set_version (device, version);
		}
		if (fu_device_get_version_lowest_raw (device) != 0x0) {
			g_autofree gchar *version = NULL;
			version = fu_common_version_from_uint32 (fu_device_get_version_lowest_raw (device), verfmt);
			fu_device_set_version_lowest (device, version);
		}
		if (fu_device_get_version_bootloader_raw (device) != 0x0) {
			g_autofree gchar *version = NULL;
			version = fu_common_version_from_uint32 (fu_device_get_version_bootloader_raw (device), verfmt);
			fu_device_set_version_bootloader (device, version);
		}
	}

	/* do not try to do this again */
	fu_device_remove_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_VERFMT);
}

void
fu_engine_md_refresh_device_from_component (FuEngine *self, FuDevice *device, XbNode *component)
{
	/* set the name */
	if (fu_device_has_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME))
		fu_engine_md_refresh_device_name (self, device, component);
	if (fu_device_has_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME_CATEGORY))
		fu_engine_md_refresh_device_name_category (self, device, component);
	if (fu_device_has_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON))
		fu_engine_md_refresh_device_icon (self, device, component);

	/* fix the version */
	if (fu_device_has_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_MD_SET_VERFMT))
		fu_engine_md_refresh_device_verfmt (self, device, component);
}

static void
fu_engine_md_refresh_devices (FuEngine *self)
{
	g_autoptr(GPtrArray) devices = fu_device_list_get_all (self->device_list);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index (devices, i);
		g_autoptr(XbNode) component = fu_engine_get_component_by_guids (self, device);

		/* set or clear the SUPPORTED flag */
		fu_engine_ensure_device_supported (self, device);

		/* fixup the name and format as needed */
		fu_engine_md_refresh_device_from_component (self, device, component);
	}
}

static gboolean
fu_engine_load_metadata_store (FuEngine *self, FuEngineLoadFlags flags, GError **error)
{
	GPtrArray *remotes;
	XbBuilderCompileFlags compile_flags = XB_BUILDER_COMPILE_FLAG_IGNORE_INVALID;
	g_autofree gchar *cachedirpkg = NULL;
	g_autofree gchar *xmlbfn = NULL;
	g_autoptr(GFile) xmlb = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(XbBuilder) builder = xb_builder_new ();

	/* clear existing silo */
	g_clear_object (&self->silo);

	/* verbose profiling */
	if (g_getenv ("FWUPD_XMLB_VERBOSE") != NULL) {
		xb_builder_set_profile_flags (builder,
					      XB_SILO_PROFILE_FLAG_XPATH |
					      XB_SILO_PROFILE_FLAG_DEBUG);
	}

	/* load each enabled metadata file */
	remotes = fu_remote_list_get_all (self->remote_list);
	for (guint i = 0; i < remotes->len; i++) {
		const gchar *path = NULL;
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GFile) file = NULL;
		g_autoptr(XbBuilderFixup) fixup = NULL;
		g_autoptr(XbBuilderNode) custom = NULL;
		g_autoptr(XbBuilderSource) source = xb_builder_source_new ();

		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		if (!fwupd_remote_get_enabled (remote))
			continue;
		path = fwupd_remote_get_filename_cache (remote);
		if (!g_file_test (path, G_FILE_TEST_EXISTS))
			continue;

		/* generate all metadata on demand */
		if (fwupd_remote_get_kind (remote) == FWUPD_REMOTE_KIND_DIRECTORY) {
			g_debug ("building metadata for remote '%s'",
				 fwupd_remote_get_id (remote));
			if (!fu_engine_create_metadata (self, builder, remote, &error_local)) {
				g_warning ("failed to generate remote %s: %s",
					   fwupd_remote_get_id (remote),
					   error_local->message);
			}
			continue;
		}

		/* save the remote-id in the custom metadata space */
		file = g_file_new_for_path (path);
		if (!xb_builder_source_load_file (source, file,
						  XB_BUILDER_SOURCE_FLAG_NONE,
						  NULL, &error_local)) {
			g_warning ("failed to load remote %s: %s",
				   fwupd_remote_get_id (remote),
				   error_local->message);
			continue;
		}

		/* fix up any legacy installed files */
		fixup = xb_builder_fixup_new ("AppStreamUpgrade",
					      fu_engine_appstream_upgrade_cb,
					      self, NULL);
		xb_builder_fixup_set_max_depth (fixup, 3);
		xb_builder_source_add_fixup (source, fixup);

		/* add metadata */
		custom = xb_builder_node_new ("custom");
		xb_builder_node_insert_text (custom,
					     "value", path,
					     "key", "fwupd::FilenameCache",
					     NULL);
		xb_builder_node_insert_text (custom,
					     "value", fwupd_remote_get_id (remote),
					     "key", "fwupd::RemoteId",
					     NULL);
		xb_builder_source_set_info (source, custom);

		/* we need to watch for changes? */
		xb_builder_import_source (builder, source);
	}

	/* on a read-only filesystem don't care about the cache GUID */
	if (flags & FU_ENGINE_LOAD_FLAG_READONLY)
		compile_flags |= XB_BUILDER_COMPILE_FLAG_IGNORE_GUID;

	/* ensure silo is up to date */
	cachedirpkg = fu_common_get_path (FU_PATH_KIND_CACHEDIR_PKG);
	xmlbfn = g_build_filename (cachedirpkg, "metadata.xmlb", NULL);
	xmlb = g_file_new_for_path (xmlbfn);
	self->silo = xb_builder_ensure (builder, xmlb, compile_flags, NULL, error);
	if (self->silo == NULL) {
		g_prefix_error (error, "cannot create file %s: ", xmlbfn);
		return FALSE;
	}

	/* print what we've got */
	components = xb_silo_query (self->silo,
				    "components/component[@type='firmware']",
				    0, NULL);
	if (components != NULL)
		g_debug ("%u components now in silo", components->len);

	/* build the index */
	if (!xb_silo_query_build_index (self->silo,
					"components/component",
					"type", error))
		return FALSE;
	if (!xb_silo_query_build_index (self->silo,
					"components/component[@type='firmware']/provides/firmware",
					"type", error))
		return FALSE;
	if (!xb_silo_query_build_index (self->silo,
					"components/component[@type='firmware']/provides/firmware",
					NULL, error))
		return FALSE;

	/* success */
	return TRUE;
}

static void
fu_engine_config_changed_cb (FuConfig *config, FuEngine *self)
{
	fu_idle_set_timeout (self->idle, fu_config_get_idle_timeout (config));
}

static void
fu_engine_remote_list_changed_cb (FuRemoteList *remote_list, FuEngine *self)
{
	g_autoptr(GError) error_local = NULL;
	if (!fu_engine_load_metadata_store (self, FU_ENGINE_LOAD_FLAG_NONE,
					    &error_local))
		g_warning ("Failed to reload metadata store: %s",
			   error_local->message);

	/* set device properties from the metadata */
	fu_engine_md_refresh_devices (self);

	/* invalidate host security attributes */
	g_clear_pointer (&self->host_security_id, g_free);

	/* make the UI update */
	fu_engine_emit_changed (self);
}

static gint
fu_engine_sort_jcat_results_timestamp_cb (gconstpointer a, gconstpointer b)
{
	JcatResult *ra = *((JcatResult **) a);
	JcatResult *rb = *((JcatResult **) b);
	if (jcat_result_get_timestamp (ra) < jcat_result_get_timestamp (rb))
		return 1;
	if (jcat_result_get_timestamp (ra) > jcat_result_get_timestamp (rb))
		return -1;
	return 0;
}

static JcatResult *
fu_engine_get_newest_signature_jcat_result (GPtrArray *results, GError **error)
{
	/* sort by timestamp, newest first */
	g_ptr_array_sort (results, fu_engine_sort_jcat_results_timestamp_cb);

	/* get the first signature, ignoring the checksums */
	for (guint i = 0; i < results->len; i++) {
		JcatResult *result = g_ptr_array_index (results, i);
#if LIBJCAT_CHECK_VERSION(0, 1, 3)
		if (jcat_result_get_method (result) == JCAT_BLOB_METHOD_SIGNATURE)
			return g_object_ref (result);
#else
		guint verify_kind = 0;
		g_autoptr(JcatEngine) engine = NULL;
		g_object_get (result, "engine", &engine, NULL);
		g_object_get (engine, "verify-kind", &verify_kind, NULL);
		if (verify_kind == 2) /* SIGNATURE */
			return g_object_ref (result);
#endif
	}

	/* should never happen due to %JCAT_VERIFY_FLAG_REQUIRE_SIGNATURE */
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "no signature method in results");
	return NULL;
}

static JcatResult *
fu_engine_get_system_jcat_result (FuEngine *self, FwupdRemote *remote, GError **error)
{
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GBytes) blob_sig = NULL;
	g_autoptr(GInputStream) istream = NULL;
	g_autoptr(GPtrArray) results = NULL;
	g_autoptr(JcatItem) jcat_item = NULL;
	g_autoptr(JcatFile) jcat_file = jcat_file_new ();

	blob = fu_common_get_contents_bytes (fwupd_remote_get_filename_cache (remote), error);
	if (blob == NULL)
		return NULL;
	blob_sig = fu_common_get_contents_bytes (fwupd_remote_get_filename_cache_sig (remote), error);
	if (blob_sig == NULL)
		return NULL;
	istream = g_memory_input_stream_new_from_bytes (blob_sig);
	if (!jcat_file_import_stream (jcat_file, istream,
				      JCAT_IMPORT_FLAG_NONE,
				      NULL, error))
		return NULL;
	jcat_item = jcat_file_get_item_default (jcat_file, error);
	if (jcat_item == NULL)
		return NULL;
	results = jcat_context_verify_item (self->jcat_context,
					    blob, jcat_item,
					    JCAT_VERIFY_FLAG_REQUIRE_CHECKSUM |
					    JCAT_VERIFY_FLAG_REQUIRE_SIGNATURE,
					    error);
	if (results == NULL)
		return NULL;

	/* return the newest signature */
	return fu_engine_get_newest_signature_jcat_result (results, error);
}

static gboolean
fu_engine_validate_result_timestamp (JcatResult *jcat_result,
				     JcatResult *jcat_result_old,
				     GError **error)
{
	gint64 delta = 0;

	g_return_val_if_fail (JCAT_IS_RESULT (jcat_result), FALSE);
	g_return_val_if_fail (JCAT_IS_RESULT (jcat_result_old), FALSE);

	if (jcat_result_get_timestamp (jcat_result) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "no signing timestamp");
		return FALSE;
	}
	if (jcat_result_get_timestamp (jcat_result_old) > 0) {
		delta = jcat_result_get_timestamp (jcat_result) -
			jcat_result_get_timestamp (jcat_result_old);
	}
	if (delta < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "new signing timestamp was %"
			     G_GINT64_FORMAT " seconds older",
			     -delta);
		return FALSE;
	}
	if (delta > 0)
		g_debug ("timestamp increased, so no rollback");
	return TRUE;
}

/**
 * fu_engine_update_metadata_bytes:
 * @self: a #FuEngine
 * @remote_id: a remote ID, e.g. `lvfs`
 * @bytes_raw: Blob of metadata
 * @bytes_sig: Blob of metadata signature, typically Jcat binary format
 * @error: (nullable): optional return location for an error
 *
 * Updates the metadata for a specific remote.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_update_metadata_bytes (FuEngine *self, const gchar *remote_id,
			        GBytes *bytes_raw, GBytes *bytes_sig, GError **error)
{
	FwupdKeyringKind keyring_kind;
	FwupdRemote *remote;
	JcatVerifyFlags jcat_flags = JCAT_VERIFY_FLAG_REQUIRE_SIGNATURE;
	g_autoptr(JcatFile) jcat_file = jcat_file_new ();

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (remote_id != NULL, FALSE);
	g_return_val_if_fail (bytes_raw != NULL, FALSE);
	g_return_val_if_fail (bytes_sig != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* check remote is valid */
	remote = fu_remote_list_get_by_id (self->remote_list, remote_id);
	if (remote == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "remote %s not found", remote_id);
		return FALSE;
	}
	if (!fwupd_remote_get_enabled (remote)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "remote %s not enabled", remote_id);
		return FALSE;
	}

	/* verify JCatFile, or create a dummy one from legacy data */
	keyring_kind = fwupd_remote_get_keyring_kind (remote);
	if (keyring_kind == FWUPD_KEYRING_KIND_JCAT) {
		g_autoptr(GInputStream) istream = NULL;
		istream = g_memory_input_stream_new_from_bytes (bytes_sig);
		if (!jcat_file_import_stream (jcat_file, istream,
					      JCAT_IMPORT_FLAG_NONE,
					      NULL, error))
			return FALSE;
		jcat_flags |= JCAT_VERIFY_FLAG_REQUIRE_CHECKSUM;
	} else if (keyring_kind == FWUPD_KEYRING_KIND_GPG) {
		g_autoptr(JcatBlob) jcab_blob = NULL;
		g_autoptr(JcatItem) jcat_item = jcat_item_new ("");
		jcab_blob = jcat_blob_new (JCAT_BLOB_KIND_GPG, bytes_sig);
		jcat_item_add_blob (jcat_item, jcab_blob);
		jcat_file_add_item (jcat_file, jcat_item);
	} else if (keyring_kind == FWUPD_KEYRING_KIND_PKCS7) {
		g_autoptr(JcatBlob) jcab_blob = NULL;
		g_autoptr(JcatItem) jcat_item = jcat_item_new ("");
		jcab_blob = jcat_blob_new (JCAT_BLOB_KIND_PKCS7, bytes_sig);
		jcat_item_add_blob (jcat_item, jcab_blob);
		jcat_file_add_item (jcat_file, jcat_item);
	}

	/* verify file */
	if (keyring_kind != FWUPD_KEYRING_KIND_NONE) {
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GPtrArray) results = NULL;
		g_autoptr(JcatItem) jcat_item = NULL;
		g_autoptr(JcatResult) jcat_result = NULL;
		g_autoptr(JcatResult) jcat_result_old = NULL;

		/* this should only be signing one thing */
		jcat_item = jcat_file_get_item_default (jcat_file, error);
		if (jcat_item == NULL)
			return FALSE;
		results = jcat_context_verify_item (self->jcat_context,
						    bytes_raw, jcat_item,
						    jcat_flags, error);
		if (results == NULL)
			return FALSE;

		/* return the newest signature */
		jcat_result = fu_engine_get_newest_signature_jcat_result (results, error);
		if (jcat_result == NULL)
			return FALSE;

		/* verify the metadata was signed later than the existing
		 * metadata for this remote to mitigate a rollback attack */
		jcat_result_old = fu_engine_get_system_jcat_result (self, remote, &error_local);
		if (jcat_result_old == NULL) {
			if (g_error_matches (error_local,
					     G_FILE_ERROR,
					     G_FILE_ERROR_NOENT)) {
				g_debug ("no existing valid keyrings: %s",
					 error_local->message);
			} else {
				g_warning ("could not get existing keyring result: %s",
					   error_local->message);
			}
		} else {
			if (!fu_engine_validate_result_timestamp (jcat_result,
								  jcat_result_old,
								  error))
				return FALSE;
		}
	}

	/* save XML and signature to remotes.d */
	if (!fu_common_set_contents_bytes (fwupd_remote_get_filename_cache (remote),
					   bytes_raw, error))
		return FALSE;
	if (keyring_kind != FWUPD_KEYRING_KIND_NONE) {
		if (!fu_common_set_contents_bytes (fwupd_remote_get_filename_cache_sig (remote),
						   bytes_sig, error))
			return FALSE;
	}
	if (!fu_engine_load_metadata_store (self, FU_ENGINE_LOAD_FLAG_NONE, error))
		return FALSE;

	/* refresh SUPPORTED flag on devices */
	fu_engine_md_refresh_devices (self);

	/* invalidate host security attributes */
	g_clear_pointer (&self->host_security_id, g_free);

	/* make the UI update */
	fu_engine_emit_changed (self);
	return TRUE;
}

/**
 * fu_engine_update_metadata:
 * @self: a #FuEngine
 * @remote_id: a remote ID, e.g. `lvfs`
 * @fd: file descriptor of the metadata
 * @fd_sig: file descriptor of the metadata signature
 * @error: (nullable): optional return location for an error
 *
 * Updates the metadata for a specific remote.
 *
 * Note: this will close the fds when done
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_update_metadata (FuEngine *self, const gchar *remote_id,
			   gint fd, gint fd_sig, GError **error)
{
#ifdef HAVE_GIO_UNIX
	g_autoptr(GBytes) bytes_raw = NULL;
	g_autoptr(GBytes) bytes_sig = NULL;
	g_autoptr(GInputStream) stream_fd = NULL;
	g_autoptr(GInputStream) stream_sig = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (remote_id != NULL, FALSE);
	g_return_val_if_fail (fd > 0, FALSE);
	g_return_val_if_fail (fd_sig > 0, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* ensures the fd's are closed on error */
	stream_fd = g_unix_input_stream_new (fd, TRUE);
	stream_sig = g_unix_input_stream_new (fd_sig, TRUE);

	/* read the entire file into memory */
	bytes_raw = g_input_stream_read_bytes (stream_fd, 0x100000, NULL, error);
	if (bytes_raw == NULL)
		return FALSE;

	/* read signature */
	bytes_sig = g_input_stream_read_bytes (stream_sig, 0x100000, NULL, error);
	if (bytes_sig == NULL)
		return FALSE;

	/* update with blobs */
	return fu_engine_update_metadata_bytes (self, remote_id,
						bytes_raw, bytes_sig,
						error);
#else
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_NOT_SUPPORTED,
		     "Not supported as <glib-unix.h> is unavailable");
	return FALSE;
#endif
}

/**
 * fu_engine_get_silo_from_blob:
 * @self: a #FuEngine
 * @blob_cab: a data blob
 * @error: (nullable): optional return location for an error
 *
 * Creates a silo from a .cab file blob.
 *
 * Returns: (transfer container): a #XbSilo, or %NULL
 **/
XbSilo *
fu_engine_get_silo_from_blob (FuEngine *self, GBytes *blob_cab, GError **error)
{
	g_autoptr(FuCabinet) cabinet = fu_cabinet_new ();
	g_autoptr(XbSilo) silo = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (blob_cab != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* load file */
	fu_engine_set_status (self, FWUPD_STATUS_DECOMPRESSING);
	fu_cabinet_set_size_max (cabinet, fu_engine_get_archive_size_max (self));
	fu_cabinet_set_jcat_context (cabinet, self->jcat_context);
	if (!fu_cabinet_parse (cabinet, blob_cab, FU_CABINET_PARSE_FLAG_NONE, error))
		return NULL;
	silo = fu_cabinet_get_silo (cabinet);
	fu_engine_set_status (self, FWUPD_STATUS_IDLE);
	return g_steal_pointer (&silo);
}

static FuDevice *
fu_engine_get_result_from_component (FuEngine *self,
				     FuEngineRequest *request,
				     XbNode *component,
				     GError **error)
{
	FwupdReleaseFlags release_flags = FWUPD_RELEASE_FLAG_NONE;
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(FuDevice) dev = NULL;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) provides = NULL;
	g_autoptr(XbNode) description = NULL;
	g_autoptr(XbNode) release = NULL;
#if LIBXMLB_CHECK_VERSION(0,2,0)
	g_autoptr(XbQuery) query = NULL;
#endif

	dev = fu_device_new ();
	provides = xb_node_query (component,
				  "provides/firmware[@type=$'flashed']",
				  0, &error_local);
	if (provides == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "failed to get release: %s",
			     error_local->message);
		return NULL;
	}
	for (guint i = 0; i < provides->len; i++) {
		XbNode *prov = XB_NODE (g_ptr_array_index (provides, i));
		const gchar *guid;
		g_autoptr(FuDevice) device = NULL;

		/* is a online or offline update appropriate */
		guid = xb_node_get_text (prov);
		if (guid == NULL)
			continue;
		device = fu_device_list_get_by_guid (self->device_list, guid, NULL);
		if (device != NULL) {
			fu_device_set_name (dev, fu_device_get_name (device));
			fu_device_set_flags (dev, fu_device_get_flags (device));
			fu_device_set_id (dev, fu_device_get_id (device));
			fu_device_set_version_raw (dev, fu_device_get_version_raw (device));
			fu_device_set_version_format (dev, fu_device_get_version_format (device));
			fu_device_set_version (dev, fu_device_get_version (device));
		}

		/* add GUID */
		fu_device_add_guid (dev, guid);
	}
	if (fu_device_get_guids(dev)->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "component has no GUIDs");
		return NULL;
	}

	/* check we can install it */
	task = fu_install_task_new (NULL, component);
	if (!fu_engine_check_requirements (self, request, task,
					   FWUPD_INSTALL_FLAG_IGNORE_VID_PID,
					   error))
		return NULL;

	/* verify trust */
#if LIBXMLB_CHECK_VERSION(0,2,0)
	query = xb_query_new_full (xb_node_get_silo (component),
				   "releases/release",
				   XB_QUERY_FLAG_FORCE_NODE_CACHE,
				   error);
	if (query == NULL)
		return NULL;
	release = xb_node_query_first_full (component, query, &error_local);
#else
	release = xb_node_query_first (component,
				       "releases/release",
				       &error_local);
#endif
	if (release == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "failed to get release: %s",
			     error_local->message);
		return NULL;
	}
	if (!fu_keyring_get_release_flags (release,
					   &release_flags,
					   &error_local)) {
		if (g_error_matches (error_local,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED)) {
			g_warning ("Ignoring verification: %s",
				   error_local->message);
		} else {
			g_propagate_error (error, g_steal_pointer (&error_local));
			return NULL;
		}
	}

	/* create a result with all the metadata in */
	description = xb_node_query_first (component, "description", NULL);
	if (description != NULL) {
		g_autofree gchar *xml = NULL;
		xml = xb_node_export (description,
				      XB_NODE_EXPORT_FLAG_ONLY_CHILDREN,
				      NULL);
		if (xml != NULL)
			fu_device_set_description (dev, xml);
	}
	rel = fwupd_release_new ();
	fwupd_release_set_flags (rel, release_flags);
	if (!fu_engine_set_release_from_appstream (self, dev, rel, component, release, error))
		return NULL;
	fu_device_add_release (dev, rel);
	return g_steal_pointer (&dev);
}

static gint
fu_engine_get_details_sort_cb (gconstpointer a, gconstpointer b)
{
	FuDevice *device1 = *((FuDevice **) a);
	FuDevice *device2 = *((FuDevice **) b);
	if (!fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    fu_device_has_flag (device2, FWUPD_DEVICE_FLAG_UPDATABLE))
		return 1;
	if (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    !fu_device_has_flag (device2, FWUPD_DEVICE_FLAG_UPDATABLE))
		return -1;
	return 0;
}

/**
 * fu_engine_get_details:
 * @self: a #FuEngine
 * @request: a #FuEngineRequest
 * @fd: a file descriptor
 * @error: (nullable): optional return location for an error
 *
 * Gets the details about a local file.
 *
 * Note: this will close the fd when done
 *
 * Returns: (transfer container) (element-type FuDevice): results
 **/
GPtrArray *
fu_engine_get_details (FuEngine *self, FuEngineRequest *request, gint fd, GError **error)
{
	const gchar *remote_id;
	g_autofree gchar *csum = NULL;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(GPtrArray) details = NULL;
	g_autoptr(XbSilo) silo = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (fd > 0, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* get all components */
	blob = fu_common_get_contents_fd (fd,
					  fu_engine_get_archive_size_max (self),
					  error);
	if (blob == NULL)
		return NULL;
	silo = fu_engine_get_silo_from_blob (self, blob, error);
	if (silo == NULL)
		return NULL;
	components = xb_silo_query (silo,
				    "components/component[@type='firmware']",
				    0, &error_local);
	if (components == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "no components: %s",
			     error_local->message);
		return NULL;
	}

	/* build the index */
	if (!xb_silo_query_build_index (silo, "components/component[@type='firmware']/provides/firmware",
					"type", error))
		return NULL;
	if (!xb_silo_query_build_index (silo, "components/component[@type='firmware']/provides/firmware",
					NULL, error))
		return NULL;

	/* does this exist in any enabled remote */
	csum = g_compute_checksum_for_bytes (G_CHECKSUM_SHA1, blob);
	remote_id = fu_engine_get_remote_id_for_checksum (self, csum);

	/* create results with all the metadata in */
	details = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < components->len; i++) {
		XbNode *component = g_ptr_array_index (components, i);
		FuDevice *dev;
		dev = fu_engine_get_result_from_component (self, request, component, error);
		if (dev == NULL)
			return NULL;
		if (remote_id != NULL) {
			FwupdRelease *rel = fu_device_get_release_default (dev);
			fwupd_release_set_remote_id (rel, remote_id);
			fu_device_add_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED);
		}
		if (fu_device_has_internal_flag (dev, FU_DEVICE_INTERNAL_FLAG_MD_SET_VERFMT))
			fu_engine_md_refresh_device_verfmt (self, dev, component);

		/* if this matched a device on the system, ensure all the
		 * requirements passed before setting UPDATABLE */
		if (fu_device_has_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE)) {
			g_autoptr(FuInstallTask) task = fu_install_task_new (dev, component);
			g_autoptr(GError) error_req = NULL;
			if (!fu_engine_check_requirements (self, request, task,
							   FWUPD_INSTALL_FLAG_OFFLINE |
							   FWUPD_INSTALL_FLAG_IGNORE_VID_PID |
							   FWUPD_INSTALL_FLAG_ALLOW_REINSTALL |
							   FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH |
							   FWUPD_INSTALL_FLAG_ALLOW_OLDER,
							   &error_req)) {
				g_debug ("%s failed requirement checks: %s",
					 fu_device_get_id (dev),
					 error_req->message);
				fu_device_remove_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE);
			} else {
				g_debug ("%s passed requirement checks",
					 fu_device_get_id (dev));
			}
		}

		g_ptr_array_add (details, dev);
	}

	/* order multiple devices so that the one that passes the requirement
	 * is listed first */
	g_ptr_array_sort (details, fu_engine_get_details_sort_cb);

	return g_steal_pointer (&details);
}

static gint
fu_engine_sort_devices_by_priority_name (gconstpointer a, gconstpointer b)
{
	FuDevice *dev_a = *((FuDevice **) a);
	FuDevice *dev_b = *((FuDevice **) b);
	gint prio_a = fu_device_get_priority (dev_a);
	gint prio_b = fu_device_get_priority (dev_b);
	const gchar *name_a = fu_device_get_name (dev_a);
	const gchar *name_b = fu_device_get_name (dev_b);

	if (prio_a > prio_b)
		return -1;
	if (prio_a < prio_b)
		return 1;
	if (g_strcmp0 (name_a, name_b) > 0)
		return 1;
	if (g_strcmp0 (name_a, name_b) < 0)
		return -1;
	return 0;
}

/**
 * fu_engine_get_devices:
 * @self: a #FuEngine
 * @error: (nullable): optional return location for an error
 *
 * Gets the list of devices.
 *
 * Returns: (transfer container) (element-type FwupdDevice): results
 **/
GPtrArray *
fu_engine_get_devices (FuEngine *self, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	devices = fu_device_list_get_active (self->device_list);
	if (devices->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No detected devices");
		return NULL;
	}
	g_ptr_array_sort (devices, fu_engine_sort_devices_by_priority_name);
	return g_steal_pointer (&devices);
}

/**
 * fu_engine_get_devices_by_guid:
 * @self: a #FuEngine
 * @guid: a GUID
 * @error: (nullable): optional return location for an error
 *
 * Gets a specific device.
 *
 * Returns: (transfer full): a device, or %NULL if not found
 **/
GPtrArray *
fu_engine_get_devices_by_guid (FuEngine *self, const gchar *guid, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_tmp = NULL;

	/* find the devices by GUID */
	devices_tmp = fu_device_list_get_all (self->device_list);
	devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < devices_tmp->len; i++) {
		FuDevice *dev_tmp = g_ptr_array_index (devices_tmp, i);
		if (fu_device_has_guid (dev_tmp, guid))
			g_ptr_array_add (devices, g_object_ref (dev_tmp));
	}

	/* nothing */
	if (devices->len == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "failed to find any device providing %s", guid);
		return NULL;
	}

	/* success */
	return g_steal_pointer (&devices);
}

/**
 * fu_engine_get_devices_by_composite_id:
 * @self: a #FuEngine
 * @composite_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Gets all devices that match a specific composite ID.
 *
 * Returns: (transfer full) (element-type FuDevice): devices
 **/
GPtrArray *
fu_engine_get_devices_by_composite_id (FuEngine *self,
				       const gchar *composite_id,
				       GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_tmp = NULL;

	/* find the devices by composite ID */
	devices_tmp = fu_device_list_get_all (self->device_list);
	devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < devices_tmp->len; i++) {
		FuDevice *dev_tmp = g_ptr_array_index (devices_tmp, i);
		if (g_strcmp0 (fu_device_get_composite_id (dev_tmp), composite_id) == 0)
			g_ptr_array_add (devices, g_object_ref (dev_tmp));
	}

	/* nothing */
	if (devices->len == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "failed to find any device with composite ID %s",
			     composite_id);
		return NULL;
	}

	/* success */
	return g_steal_pointer (&devices);
}

static void
fu_engine_get_history_set_hsi_attrs (FuEngine *self, FuDevice *device)
{
	g_autoptr(GPtrArray) vals = NULL;

	/* ensure up to date */
	fu_engine_ensure_security_attrs (self);

	/* add attributes */
	vals = fu_security_attrs_get_all (self->host_security_attrs);
	for (guint i = 0; i < vals->len; i++) {
		FwupdSecurityAttr *attr = g_ptr_array_index (vals, i);
		const gchar *tmp;
		tmp = fwupd_security_attr_result_to_string (fwupd_security_attr_get_result (attr));
		fu_device_set_metadata (device, fwupd_security_attr_get_appstream_id (attr), tmp);
	}

	/* computed value */
	fu_device_set_metadata (device, "HSI", self->host_security_id);
}

/**
 * fu_engine_get_history:
 * @self: a #FuEngine
 * @error: (nullable): optional return location for an error
 *
 * Gets the list of history.
 *
 * Returns: (transfer container) (element-type FwupdDevice): results
 **/
GPtrArray *
fu_engine_get_history (FuEngine *self, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	devices = fu_history_get_devices (self->history, error);
	if (devices == NULL)
		return NULL;
	if (devices->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No history");
		return NULL;
	}

	/* if this is the system firmware device, add the HSI attrs */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		if (fu_device_has_instance_id (dev, "main-system-firmware"))
			fu_engine_get_history_set_hsi_attrs (self, dev);
	}

	/* try to set the remote ID for each device */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel;
		GPtrArray *csums;

		/* get the checksums */
		rel = fu_device_get_release_default (dev);
		if (rel == NULL)
			continue;

		/* find the checksum that matches */
		csums = fwupd_release_get_checksums (rel);
		for (guint j = 0; j < csums->len; j++) {
			const gchar *csum = g_ptr_array_index (csums, j);
			const gchar *remote_id = fu_engine_get_remote_id_for_checksum (self, csum);
			if (remote_id != NULL) {
				fu_device_add_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED);
				fwupd_release_set_remote_id (rel, remote_id);
				break;
			}
		}
	}

	return g_steal_pointer (&devices);
}

#if !GLIB_CHECK_VERSION(2,62,0)
static GPtrArray *
g_ptr_array_copy (GPtrArray *array, GCopyFunc func, gpointer user_data)
{
	GPtrArray *new = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < array->len; i++) {
		GObject *obj = g_ptr_array_index (array, i);
		g_ptr_array_add (new, g_object_ref (obj));
	}
	return new;
}
#endif

/**
 * fu_engine_get_remotes:
 * @self: a #FuEngine
 * @error: (nullable): optional return location for an error
 *
 * Gets the list of remotes in use by the engine.
 *
 * Returns: (transfer container) (element-type FwupdRemote): results
 **/
GPtrArray *
fu_engine_get_remotes (FuEngine *self, GError **error)
{
	GPtrArray *remotes;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	remotes = fu_remote_list_get_all (self->remote_list);
	if (remotes->len == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "No remotes configured");
		return NULL;
	}

	/* deep copy so the remote list can be kept up to date */
	return g_ptr_array_copy (remotes, (GCopyFunc) g_object_ref, NULL);
}

/**
 * fu_engine_get_remote_by_id:
 * @self: a #FuEngine
 * @remote_id: a string representation of a remote
 * @error: (nullable): optional return location for an error
 *
 * Gets the FwupdRemote object.
 *
 * Returns: FwupdRemote
 **/
FwupdRemote *
fu_engine_get_remote_by_id (FuEngine *self, const gchar *remote_id, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;

	remotes = fu_engine_get_remotes (self, error);
	if (remotes == NULL)
		return NULL;

	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		if (g_strcmp0 (remote_id, fwupd_remote_get_id (remote)) == 0)
			return remote;
	}

	g_set_error (error, FWUPD_ERROR, FWUPD_ERROR_INTERNAL,
		     "Couldn't find remote %s", remote_id);

	return NULL;
}


static gint
fu_engine_sort_releases_cb (gconstpointer a, gconstpointer b, gpointer user_data)
{
	FuDevice *device = FU_DEVICE (user_data);
	FwupdRelease *rel_a = FWUPD_RELEASE (*((FwupdRelease **) a));
	FwupdRelease *rel_b = FWUPD_RELEASE (*((FwupdRelease **) b));
	gint rc;

	/* first by branch */
	rc = g_strcmp0 (fwupd_release_get_branch (rel_b), fwupd_release_get_branch (rel_a));
	if (rc != 0)
		return rc;

	/* then by version */
	return fu_common_vercmp_full (fwupd_release_get_version (rel_b),
				      fwupd_release_get_version (rel_a),
				      fu_device_get_version_format (device));
}

static gboolean
fu_engine_check_release_is_approved (FuEngine *self, FwupdRelease *rel)
{
	GPtrArray *csums = fwupd_release_get_checksums (rel);
	if (self->approved_firmware == NULL)
		return FALSE;
	for (guint i = 0; i < csums->len; i++) {
		const gchar *csum = g_ptr_array_index (csums, i);
		g_debug ("checking %s against approved list", csum);
		if (g_hash_table_lookup (self->approved_firmware, csum) != NULL)
			return TRUE;
	}
	return FALSE;
}

static gboolean
fu_engine_check_release_is_blocked (FuEngine *self, FwupdRelease *rel)
{
	GPtrArray *csums = fwupd_release_get_checksums (rel);
	if (self->blocked_firmware == NULL)
		return FALSE;
	for (guint i = 0; i < csums->len; i++) {
		const gchar *csum = g_ptr_array_index (csums, i);
		if (g_hash_table_lookup (self->blocked_firmware, csum) != NULL)
			return TRUE;
	}
	return FALSE;
}

static gboolean
fu_engine_add_releases_for_device_component (FuEngine *self,
					     FuEngineRequest *request,
					     FuDevice *device,
					     XbNode *component,
					     GPtrArray *releases,
					     GError **error)
{
	FwupdFeatureFlags feature_flags;
	FwupdVersionFormat fmt = fu_device_get_version_format (device);
	g_autoptr(GError) error_local = NULL;
	g_autoptr(FuInstallTask) task = fu_install_task_new (device, component);
	g_autoptr(GPtrArray) releases_tmp = NULL;

	if (!fu_engine_check_requirements (self, request, task,
					   FWUPD_INSTALL_FLAG_OFFLINE |
					   FWUPD_INSTALL_FLAG_IGNORE_VID_PID |
					   FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH |
					   FWUPD_INSTALL_FLAG_ALLOW_REINSTALL |
					   FWUPD_INSTALL_FLAG_ALLOW_OLDER,
					   error))
		return FALSE;

	/* get all releases */
	releases_tmp = xb_node_query (component, "releases/release", 0, &error_local);
	if (releases_tmp == NULL) {
		if (g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
			return TRUE;
		if (g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
			return TRUE;
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}
	feature_flags = fu_engine_request_get_feature_flags (request);
	for (guint i = 0; i < releases_tmp->len; i++) {
		XbNode *release = g_ptr_array_index (releases_tmp, i);
		const gchar *remote_id;
		const gchar *update_message;
		const gchar *update_image;
		gint vercmp;
		GPtrArray *checksums;
		GPtrArray *locations;
		g_autoptr(FwupdRelease) rel = fwupd_release_new ();
		g_autoptr(GError) error_loop = NULL;

		/* create new FwupdRelease for the XbNode */
		if (!fu_engine_set_release_from_appstream (self,
							   device,
							   rel,
							   component,
							   release,
							   &error_loop)) {
			g_warning ("failed to set release for component: %s",
				   error_loop->message);
			continue;
		}

		/* fall back to quirk-provided value */
		if (fwupd_release_get_install_duration (rel) == 0)
			fwupd_release_set_install_duration (rel, fu_device_get_install_duration (device));

		/* invalid */
		locations = fwupd_release_get_locations (rel);
		if (locations->len == 0)
			continue;
		checksums = fwupd_release_get_checksums (rel);
		if (checksums->len == 0)
			continue;

		/* different branch */
		if (g_strcmp0 (fwupd_release_get_branch (rel),
			       fu_device_get_branch (device)) != 0) {
			if ((feature_flags & FWUPD_FEATURE_FLAG_SWITCH_BRANCH) == 0) {
				g_debug ("client does not understand branches, skipping %s:%s",
					 fwupd_release_get_branch (rel),
					 fwupd_release_get_version (rel));
				continue;
			}
			fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH);
		}

		/* test for upgrade or downgrade */
		vercmp = fu_common_vercmp_full (fwupd_release_get_version (rel),
						fu_device_get_version (device),
						fmt);
		if (vercmp > 0)
			fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_IS_UPGRADE);
		else if (vercmp < 0)
			fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_IS_DOWNGRADE);

		/* lower than allowed to downgrade to */
		if (fu_device_get_version_lowest (device) != NULL &&
		    fu_common_vercmp_full (fwupd_release_get_version (rel),
					   fu_device_get_version_lowest (device),
					   fmt) < 0) {
			fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_BLOCKED_VERSION);
		}

		/* manually blocked */
		if (fu_engine_check_release_is_blocked (self, rel))
			fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL);

		/* check if remote is filtering firmware */
		remote_id = fwupd_release_get_remote_id (rel);
		if (remote_id != NULL) {
			FwupdRemote *remote = fu_engine_get_remote_by_id (self, remote_id, NULL);
			if (remote != NULL &&
			    fwupd_remote_get_approval_required (remote) &&
			    !fu_engine_check_release_is_approved (self, rel)) {
				fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL);
			}
		}

		/* add update message if exists but device doesn't already have one */
		update_message = fwupd_release_get_update_message (rel);
		if (fwupd_device_get_update_message (FWUPD_DEVICE (device)) == NULL &&
		    update_message != NULL) {
			fu_device_set_update_message_kind (device, FWUPD_DEVICE_MESSAGE_KIND_POST);
			fu_device_set_update_message (device, update_message);
		}
		update_image = fwupd_release_get_update_image (rel);
		if (fwupd_device_get_update_image (FWUPD_DEVICE (device)) == NULL &&
		    update_image != NULL) {
			fwupd_device_set_update_image (FWUPD_DEVICE (device), update_image);
		}
		/* success */
		g_ptr_array_add (releases, g_steal_pointer (&rel));
	}

	/* success */
	return TRUE;
}

static const gchar *
fu_engine_get_branch_fallback (const gchar *nullable_branch)
{
	if (nullable_branch == NULL)
		return "default";
	return nullable_branch;
}

GPtrArray *
fu_engine_get_releases_for_device (FuEngine *self,
				   FuEngineRequest *request,
				   FuDevice *device,
				   GError **error)
{
	GPtrArray *device_guids;
	GPtrArray *releases;
	const gchar *version;
	g_autoptr(GError) error_all = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) branches = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(GString) xpath = g_string_new (NULL);

	/* get device version */
	version = fu_device_get_version (device);
	if (version == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "no version set");
		return NULL;
	}

	/* only show devices that can be updated */
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    !fu_device_has_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "is not updatable");
		return NULL;
	}

	/* get all the components that provide any of these GUIDs */
	device_guids = fu_device_get_guids (device);
	for (guint i = 0; i < device_guids->len; i++) {
		const gchar *guid = g_ptr_array_index (device_guids, i);
		xb_string_append_union (xpath,
					"components/component[@type='firmware']/"
					"provides/firmware[@type=$'flashed'][text()=$'%s']/"
					"../..", guid);
	}
	components = xb_silo_query (self->silo, xpath->str, 0, &error_local);
	if (components == NULL) {
		if (g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
		    g_error_matches (error_local, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT)) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOTHING_TO_DO,
					     "No releases found");
			return NULL;
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return NULL;
	}

	/* find all the releases that pass all the requirements */
	releases = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < components->len; i++) {
		XbNode *component = XB_NODE (g_ptr_array_index (components, i));
		g_autoptr(GError) error_tmp = NULL;
		if (!fu_engine_add_releases_for_device_component (self,
								  request,
								  device,
								  component,
								  releases,
								  &error_tmp)) {
			if (error_all == NULL) {
				error_all = g_steal_pointer (&error_tmp);
				continue;
			}

			/* assume the domain and code is the same */
			g_prefix_error (&error_all, "%s, ", error_tmp->message);
		}
	}

	/* are there multiple branches available */
	branches = g_ptr_array_new_with_free_func (g_free);
	g_ptr_array_add (branches,
			 g_strdup (fu_engine_get_branch_fallback (fu_device_get_branch (device))));
	for (guint i = 0; i < releases->len; i++) {
		FwupdRelease *rel_tmp = FWUPD_RELEASE (g_ptr_array_index (releases, i));
		const gchar *branch_tmp = fu_engine_get_branch_fallback (fwupd_release_get_branch (rel_tmp));
#if GLIB_CHECK_VERSION(2,54,3)
		if (g_ptr_array_find_with_equal_func (branches, branch_tmp,
						      g_str_equal, NULL))
			continue;
#endif
		g_ptr_array_add (branches, g_strdup (branch_tmp));
	}
	if (branches->len > 1)
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES);

	/* return the compound error */
	if (releases->len == 0) {
		if (error_all != NULL) {
			g_propagate_prefixed_error (error, g_steal_pointer (&error_all),
						    "No releases found: ");
			return NULL;
		}
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "No releases found");
		return NULL;
	}
	return releases;
}

/**
 * fu_engine_get_releases:
 * @self: a #FuEngine
 * @request: a #FuEngineRequest
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Gets the releases available for a specific device.
 *
 * Returns: (transfer container) (element-type FwupdDevice): results
 **/
GPtrArray *
fu_engine_get_releases (FuEngine *self,
			FuEngineRequest *request,
			const gchar *device_id,
			GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GPtrArray) releases = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (device_id != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find the device */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return NULL;

	/* get all the releases for the device */
	releases = fu_engine_get_releases_for_device (self, request, device, error);
	if (releases == NULL)
		return NULL;
	if (releases->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No releases for device");
		return NULL;
	}
	g_ptr_array_sort_with_data (releases, fu_engine_sort_releases_cb, device);
	return g_steal_pointer (&releases);
}

/**
 * fu_engine_get_downgrades:
 * @self: a #FuEngine
 * @request: a #FuEngineRequest
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Gets the downgrades available for a specific device.
 *
 * Returns: (transfer container) (element-type FwupdDevice): results
 **/
GPtrArray *
fu_engine_get_downgrades (FuEngine *self,
			  FuEngineRequest *request,
			  const gchar *device_id,
			  GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(GPtrArray) releases_tmp = NULL;
	g_autoptr(GString) error_str = g_string_new (NULL);

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (device_id != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find the device */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return NULL;

	/* get all the releases for the device */
	releases_tmp = fu_engine_get_releases_for_device (self, request, device, error);
	if (releases_tmp == NULL)
		return NULL;
	releases = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < releases_tmp->len; i++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (releases_tmp, i);

		/* same as installed */
		if (!fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_UPGRADE) &&
		    !fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_DOWNGRADE)) {
			g_string_append_printf (error_str, "%s=same, ",
						fwupd_release_get_version (rel_tmp));
			g_debug ("ignoring %s as the same as %s",
				 fwupd_release_get_version (rel_tmp),
				 fu_device_get_version (device));
			continue;
		}

		/* newer than current */
		if (fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_UPGRADE)) {
			g_string_append_printf (error_str, "%s=newer, ",
						fwupd_release_get_version (rel_tmp));
			g_debug ("ignoring %s as newer than %s",
				 fwupd_release_get_version (rel_tmp),
				 fu_device_get_version (device));
			continue;
		}

		/* don't show releases we are not allowed to downgrade to */
		if (fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_BLOCKED_VERSION)) {
			g_string_append_printf (error_str, "%s=lowest, ",
						fwupd_release_get_version (rel_tmp));
			g_debug ("ignoring %s as older than lowest %s",
				 fwupd_release_get_version (rel_tmp),
				 fu_device_get_version_lowest (device));
			continue;
		}

		/* different branch */
		if (fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH)) {
			g_debug ("ignoring release %s as branch %s, and device is %s",
				 fwupd_release_get_version (rel_tmp),
				 fwupd_release_get_branch (rel_tmp),
				 fu_device_get_branch (device));
			continue;
		}

		g_ptr_array_add (releases, g_object_ref (rel_tmp));
	}
	if (error_str->len > 2)
		g_string_truncate (error_str, error_str->len - 2);
	if (releases->len == 0) {
		if (error_str->len > 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "current version is %s: %s",
				     fu_device_get_version (device),
				     error_str->str);
		} else {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "current version is %s",
				     fu_device_get_version (device));
		}
		return NULL;
	}
	g_ptr_array_sort_with_data (releases, fu_engine_sort_releases_cb, device);
	return g_steal_pointer (&releases);
}

GPtrArray *
fu_engine_get_approved_firmware (FuEngine *self)
{
	GPtrArray *checksums = g_ptr_array_new_with_free_func (g_free);
	if (self->approved_firmware != NULL) {
		g_autoptr(GList) keys = g_hash_table_get_keys (self->approved_firmware);
		for (GList *l = keys; l != NULL; l = l->next) {
			const gchar *csum = l->data;
			g_ptr_array_add (checksums, g_strdup (csum));
		}
	}
	return checksums;
}

void
fu_engine_add_approved_firmware (FuEngine *self, const gchar *checksum)
{
	if (self->approved_firmware == NULL) {
		self->approved_firmware = g_hash_table_new_full (g_str_hash,
								 g_str_equal,
								 g_free,
								 NULL);
	}
	g_hash_table_add (self->approved_firmware, g_strdup (checksum));
}

GPtrArray *
fu_engine_get_blocked_firmware (FuEngine *self)
{
	GPtrArray *checksums = g_ptr_array_new_with_free_func (g_free);
	if (self->blocked_firmware != NULL) {
		g_autoptr(GList) keys = g_hash_table_get_keys (self->blocked_firmware);
		for (GList *l = keys; l != NULL; l = l->next) {
			const gchar *csum = l->data;
			g_ptr_array_add (checksums, g_strdup (csum));
		}
	}
	return checksums;
}

void
fu_engine_add_blocked_firmware (FuEngine *self, const gchar *checksum)
{
	if (self->blocked_firmware == NULL) {
		self->blocked_firmware = g_hash_table_new_full (g_str_hash,
								g_str_equal,
								g_free,
								NULL);
	}
	g_hash_table_add (self->blocked_firmware, g_strdup (checksum));
}

gboolean
fu_engine_set_blocked_firmware (FuEngine *self, GPtrArray *checksums, GError **error)
{
	/* update in-memory hash */
	if (self->blocked_firmware != NULL) {
		g_hash_table_unref (self->blocked_firmware);
		self->blocked_firmware = NULL;
	}
	for (guint i = 0; i < checksums->len; i++) {
		const gchar *csum = g_ptr_array_index (checksums, i);
		fu_engine_add_blocked_firmware (self, csum);
	}

	/* save database */
	if (!fu_history_clear_blocked_firmware (self->history, error))
		return FALSE;
	for (guint i = 0; i < checksums->len; i++) {
		const gchar *csum = g_ptr_array_index (checksums, i);
		if (!fu_history_add_blocked_firmware (self->history, csum, error))
			return FALSE;
	}
	return TRUE;
}

gchar *
fu_engine_self_sign (FuEngine *self,
		     const gchar *value,
		     JcatSignFlags flags,
		     GError **error)
{
	g_autoptr(JcatBlob) jcat_signature = NULL;
	g_autoptr(JcatEngine) jcat_engine = NULL;
	g_autoptr(JcatResult) jcat_result = NULL;
	g_autoptr(GBytes) payload = NULL;

	/* create detached signature and verify */
	jcat_engine = jcat_context_get_engine (self->jcat_context,
					       JCAT_BLOB_KIND_PKCS7,
					       error);
	if (jcat_engine == NULL)
		return NULL;
	payload = g_bytes_new (value, strlen (value));
	jcat_signature = jcat_engine_self_sign (jcat_engine, payload, flags, error);
	if (jcat_signature == NULL)
		return NULL;
	jcat_result = jcat_engine_self_verify (jcat_engine, payload,
					       jcat_blob_get_data (jcat_signature),
					       JCAT_VERIFY_FLAG_NONE, error);
	if (jcat_result == NULL)
		return NULL;
	return jcat_blob_get_data_as_string (jcat_signature);
}

/**
 * fu_engine_get_upgrades:
 * @self: a #FuEngine
 * @request: a #FuEngineRequest
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Gets the upgrades available for a specific device.
 *
 * Returns: (transfer container) (element-type FwupdDevice): results
 **/
GPtrArray *
fu_engine_get_upgrades (FuEngine *self,
			FuEngineRequest *request,
			const gchar *device_id,
			GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(GPtrArray) releases_tmp = NULL;
	g_autoptr(GString) error_str = g_string_new (NULL);

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (device_id != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find the device */
	device = fu_device_list_get_by_id (self->device_list, device_id, error);
	if (device == NULL)
		return NULL;

	/* don't show upgrades again until we reboot */
	if (fu_device_get_update_state (device) == FWUPD_UPDATE_STATE_NEEDS_REBOOT) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "A reboot is pending");
		return NULL;
	}

	/* get all the releases for the device */
	releases_tmp = fu_engine_get_releases_for_device (self, request, device, error);
	if (releases_tmp == NULL)
		return NULL;
	releases = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < releases_tmp->len; i++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (releases_tmp, i);

		/* same as installed */
		if (!fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_UPGRADE) &&
		    !fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_DOWNGRADE)) {
			g_string_append_printf (error_str, "%s=same, ",
						fwupd_release_get_version (rel_tmp));
			g_debug ("ignoring %s == %s",
				 fwupd_release_get_version (rel_tmp),
				 fu_device_get_version (device));
			continue;
		}

		/* older than current */
		if (fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_DOWNGRADE)) {
			g_string_append_printf (error_str, "%s=older, ",
						fwupd_release_get_version (rel_tmp));
			g_debug ("ignoring %s < %s",
				 fwupd_release_get_version (rel_tmp),
				 fu_device_get_version (device));
			continue;
		}

		/* not approved */
		if (fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL)) {
			g_string_append_printf (error_str, "%s=not-approved, ",
						fwupd_release_get_version (rel_tmp));
			g_debug ("ignoring %s as not approved as required by %s",
				 fwupd_release_get_version (rel_tmp),
				 fwupd_release_get_remote_id (rel_tmp));
			continue;
		}

		/* different branch */
		if (fwupd_release_has_flag (rel_tmp, FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH)) {
			g_debug ("ignoring release %s as branch %s, and device is %s",
				 fwupd_release_get_version (rel_tmp),
				 fwupd_release_get_branch (rel_tmp),
				 fu_device_get_branch (device));
			continue;
		}

		g_ptr_array_add (releases, g_object_ref (rel_tmp));
	}
	if (error_str->len > 2)
		g_string_truncate (error_str, error_str->len - 2);
	if (releases->len == 0) {
		if (error_str->len > 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "current version is %s: %s",
				     fu_device_get_version (device),
				     error_str->str);
		} else {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "current version is %s",
				     fu_device_get_version (device));
		}
		return NULL;
	}
	g_ptr_array_sort_with_data (releases, fu_engine_sort_releases_cb, device);
	return g_steal_pointer (&releases);
}

/**
 * fu_engine_clear_results:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Clear the historical state of a specific device operation.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_clear_results (FuEngine *self, const gchar *device_id, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	FuPlugin *plugin;

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (device_id != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* find the device */
	device = fu_engine_get_item_by_id_fallback_history (self, device_id, error);
	if (device == NULL)
		return FALSE;

	/* already set on the database */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NOTIFIED)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "device already has notified flag");
		return FALSE;
	}

	/* call into the plugin if it still exists */
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (device),
					      error);
	if (plugin != NULL) {
		if (!fu_plugin_runner_clear_results (plugin, device, error))
			return FALSE;
	}

	/* override */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NOTIFIED);
	return fu_history_modify_device (self->history, device, error);
}

/**
 * fu_engine_get_results:
 * @self: a #FuEngine
 * @device_id: a device ID
 * @error: (nullable): optional return location for an error
 *
 * Gets the historical state of a specific device operation.
 *
 * Returns: (transfer container): a device, or %NULL
 **/
FwupdDevice *
fu_engine_get_results (FuEngine *self, const gchar *device_id, GError **error)
{
	g_autoptr(FuDevice) device = NULL;

	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	g_return_val_if_fail (device_id != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find the device */
	device = fu_engine_get_item_by_id_fallback_history (self, device_id, error);
	if (device == NULL)
		return NULL;

	/* the notification has already been shown to the user */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NOTIFIED)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "User has already been notified about %s [%s]",
			     fu_device_get_name (device),
			     fu_device_get_id (device));
		return NULL;
	}

	/* success */
	return g_object_ref (FWUPD_DEVICE (device));
}

static void
fu_engine_plugins_setup (FuEngine *self)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	for (guint i = 0; i < plugins->len; i++) {
		g_autoptr(GError) error = NULL;
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		if (!fu_plugin_runner_startup (plugin, &error)) {
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED);
			if (g_error_matches (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED)) {
				fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_NO_HARDWARE);
			}
			g_message ("disabling plugin because: %s", error->message);
		}
	}
}

static void
fu_engine_plugins_coldplug (FuEngine *self)
{
	GPtrArray *plugins;
	g_autoptr(GString) str = g_string_new (NULL);

	/* prepare */
	plugins = fu_plugin_list_get_all (self->plugin_list);
	for (guint i = 0; i < plugins->len; i++) {
		g_autoptr(GError) error = NULL;
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		if (!fu_plugin_runner_coldplug_prepare (plugin, &error))
			g_warning ("failed to prepare coldplug: %s", error->message);
	}

	/* exec */
	for (guint i = 0; i < plugins->len; i++) {
		g_autoptr(GError) error = NULL;
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		if (!fu_plugin_runner_coldplug (plugin, &error)) {
			fu_plugin_add_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED);
			g_message ("disabling plugin because: %s",
				   error->message);
		}
	}

	/* cleanup */
	for (guint i = 0; i < plugins->len; i++) {
		g_autoptr(GError) error = NULL;
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		if (!fu_plugin_runner_coldplug_cleanup (plugin, &error))
			g_warning ("failed to cleanup coldplug: %s", error->message);
	}

	/* print what we do have */
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		if (fu_plugin_has_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED))
			continue;
		g_string_append_printf (str, "%s, ", fu_plugin_get_name (plugin));
	}
	if (str->len > 2) {
		g_string_truncate (str, str->len - 2);
		g_debug ("using plugins: %s", str->str);
	}
}

static void
fu_engine_plugin_device_register (FuEngine *self, FuDevice *device)
{
	GPtrArray *plugins;
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED)) {
		g_warning ("already registered %s, ignoring",
			   fu_device_get_id (device));
		return;
	}
	plugins = fu_plugin_list_get_all (self->plugin_list);
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		fu_plugin_runner_device_register (plugin, device);
	}
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_REGISTERED);
}

static void
fu_engine_plugin_device_register_cb (FuPlugin *plugin,
				    FuDevice *device,
				    gpointer user_data)
{
	FuEngine *self = FU_ENGINE (user_data);
	fu_engine_plugin_device_register (self, device);
}

static void
fu_engine_plugin_device_added_cb (FuPlugin *plugin,
				  FuDevice *device,
				  gpointer user_data)
{
	FuEngine *self = FU_ENGINE (user_data);

	/* plugin has prio and device not already set from quirk */
	if (fu_plugin_get_priority (plugin) > 0 &&
	    fu_device_get_priority (device) == 0) {
		g_debug ("auto-setting %s priority to %u",
			 fu_device_get_id (device),
			 fu_plugin_get_priority (plugin));
		fu_device_set_priority (device, fu_plugin_get_priority (plugin));
	}

	fu_engine_add_device (self, device);
}

static void
fu_engine_adopt_children (FuEngine *self, FuDevice *device)
{
	GPtrArray *guids;
	g_autoptr(GPtrArray) devices = fu_device_list_get_active (self->device_list);

	/* find the parent in any existing device */
	if (fu_device_get_parent (device) == NULL) {
		for (guint i = 0; i < devices->len; i++) {
			FuDevice *device_tmp = g_ptr_array_index (devices, i);
			if (!fu_device_has_internal_flag (device_tmp, FU_DEVICE_INTERNAL_FLAG_AUTO_PARENT_CHILDREN))
				continue;
			if (fu_device_get_physical_id (device_tmp) == NULL)
				continue;
			if (fu_device_has_parent_physical_id (device, fu_device_get_physical_id (device_tmp))) {
				fu_device_set_parent (device, device_tmp);
				break;
			}
		}
	}
	if (fu_device_get_parent (device) == NULL) {
		guids = fu_device_get_parent_guids (device);
		for (guint j = 0; j < guids->len; j++) {
			const gchar *guid = g_ptr_array_index (guids, j);
			for (guint i = 0; i < devices->len; i++) {
				FuDevice *device_tmp = g_ptr_array_index (devices, i);
				if (fu_device_has_guid (device_tmp, guid)) {
					fu_device_set_parent (device, device_tmp);
					break;
				}
			}
		}
	}

	/* the new device is the parent to an existing child */
	for (guint j = 0; j < devices->len; j++) {
		GPtrArray *parent_physical_ids = NULL;
		FuDevice *device_tmp = g_ptr_array_index (devices, j);
		if (fu_device_get_parent (device_tmp) != NULL)
			continue;
		parent_physical_ids = fu_device_get_parent_physical_ids (device_tmp);
		if (parent_physical_ids == NULL)
			continue;
		for (guint i = 0; i < parent_physical_ids->len; i++) {
			const gchar *parent_physical_id = g_ptr_array_index (parent_physical_ids, i);
			if (g_strcmp0 (parent_physical_id, fu_device_get_physical_id (device)) == 0)
				fu_device_set_parent (device_tmp, device);
		}
	}
	guids = fu_device_get_guids (device);
	for (guint j = 0; j < guids->len; j++) {
		const gchar *guid = g_ptr_array_index (guids, j);
		for (guint i = 0; i < devices->len; i++) {
			FuDevice *device_tmp = g_ptr_array_index (devices, i);
			if (fu_device_get_parent (device_tmp) != NULL)
				continue;
			if (fu_device_has_parent_guid (device_tmp, guid))
				fu_device_set_parent (device_tmp, device);
		}
	}
}

static void
fu_engine_set_proxy_device (FuEngine *self, FuDevice *device)
{
	GPtrArray *guids;
	g_autoptr(FuDevice) proxy = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	if (fu_device_get_proxy (device) != NULL)
		return;
	if (fu_device_get_proxy_guid (device) == NULL)
		return;

	/* find the proxy GUID in any existing device */
	proxy = fu_device_list_get_by_guid (self->device_list,
					    fu_device_get_proxy_guid (device),
					    NULL);
	if (proxy != NULL) {
		g_debug ("setting proxy of %s to %s for %s",
			 fu_device_get_id (proxy),
			 fu_device_get_id (device),
			 fu_device_get_proxy_guid (device));
		fu_device_set_proxy (device, proxy);
		return;
	}

	/* are we the parent of an existing device */
	guids = fu_device_get_guids (device);
	for (guint j = 0; j < guids->len; j++) {
		const gchar *guid = g_ptr_array_index (guids, j);
		devices = fu_device_list_get_active (self->device_list);
		for (guint i = 0; i < devices->len; i++) {
			FuDevice *device_tmp = g_ptr_array_index (devices, i);
			if (g_strcmp0 (fu_device_get_proxy_guid (device_tmp), guid) == 0) {
				g_debug ("adding proxy of %s to %s for %s",
					 fu_device_get_id (device),
					 fu_device_get_id (device_tmp),
					 guid);
				fu_device_set_proxy (device_tmp, device);
				return;
			}
		}
	}

	/* nothing found */
	g_warning ("did not find proxy device %s",
		   fu_device_get_proxy_guid (device));
}

static void
fu_engine_device_inherit_history (FuEngine *self, FuDevice *device)
{
	g_autoptr(FuDevice) device_history = NULL;

	/* any success or failed update? */
	device_history = fu_history_get_device_by_id (self->history,
						      fu_device_get_id (device),
						      NULL);
	if (device_history == NULL)
		return;

	/* the device is still running the old firmware version and so if it
	 * required activation before, it still requires it now -- note:
	 * we can't just check for version_new=version to allow for re-installs */
	if (fu_device_has_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_INHERIT_ACTIVATION) &&
	    fu_device_has_flag (device_history, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
		FwupdRelease *release = fu_device_get_release_default (device_history);
		if (fu_common_vercmp_full (fu_device_get_version (device),
					   fwupd_release_get_version (release),
					   fu_device_get_version_format (device)) != 0) {
			g_debug ("inheriting needs-activation for %s as version %s != %s",
				 fu_device_get_name (device),
				 fu_device_get_version (device),
				 fwupd_release_get_version (release));
			fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION);
		}
	}
}

void
fu_engine_add_device (FuEngine *self, FuDevice *device)
{
	GPtrArray *disabled_devices;
	GPtrArray *device_guids;
	g_autoptr(XbNode) component = NULL;

	/* device has no GUIDs set! */
	device_guids = fu_device_get_guids (device);
	if (device_guids->len == 0) {
		g_warning ("no GUIDs for device %s [%s]",
			   fu_device_get_name (device),
			   fu_device_get_id (device));
		return;
	}

	/* is this GUID disabled */
	disabled_devices = fu_config_get_disabled_devices (self->config);
	for (guint i = 0; i < disabled_devices->len; i++) {
		const gchar *disabled_guid = g_ptr_array_index (disabled_devices, i);
		for (guint j = 0; j < device_guids->len; j++) {
			const gchar *device_guid = g_ptr_array_index (device_guids, j);
			if (g_strcmp0 (disabled_guid, device_guid) == 0) {
				g_debug ("%s [%s] is disabled [%s], ignoring from %s",
					 fu_device_get_name (device),
					 fu_device_get_id (device),
					 device_guid,
					 fu_device_get_plugin (device));
				return;
			}
		}
	}

	/* does the device not have an assigned protocol */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    fu_device_get_protocols (device)->len == 0) {
		g_warning ("device %s [%s] does not define an update protocol",
			   fu_device_get_id (device),
			   fu_device_get_name (device));
	}

	/* if this device is locked get some metadata from AppStream */
	component = fu_engine_get_component_by_guids (self, device);
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_LOCKED)) {
		if (component != NULL) {
			g_autoptr(XbNode) release = NULL;
			release = xb_node_query_first (component,
						       "releases/release",
						       NULL);
			if (release != NULL) {
				g_autoptr(FwupdRelease) rel = fwupd_release_new ();
				g_autoptr(GError) error_local = NULL;
				if (!fu_engine_set_release_from_appstream (self,
									   device,
									   rel,
									   component,
									   release,
									   &error_local)) {
					g_warning ("failed to set AppStream release: %s",
						   error_local->message);
				} else {
					fu_device_add_release (device, rel);
				}
			}
		}
	}

	/* adopt any required children, which may or may not already exist */
	fu_engine_adopt_children (self, device);

	/* set the proxy device if specified by GUID */
	fu_engine_set_proxy_device (self, device);

	/* set any alternate objects on the device from the ID */
	if (fu_device_get_alternate_id (device) != NULL) {
		g_autoptr(FuDevice) device_alt = NULL;
		device_alt = fu_device_list_get_by_id (self->device_list,
						       fu_device_get_alternate_id (device),
						       NULL);
		if (device_alt != NULL)
			fu_device_set_alternate (device, device_alt);
	}

	if (fu_device_get_version_format (device) == FWUPD_VERSION_FORMAT_UNKNOWN &&
	    fu_common_version_guess_format (fu_device_get_version (device)) == FWUPD_VERSION_FORMAT_NUMBER) {
		fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
		fu_device_set_update_error (device, "VersionFormat is ambiguous for this device");
	}

	/* no vendor-id, and so no way to lock it down! */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    fu_device_get_vendor_ids(device)->len == 0) {
		fu_device_remove_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
		fu_device_set_update_error (device, "No vendor ID set");
	}

	/* notify all plugins about this new device */
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED))
		fu_engine_plugin_device_register (self, device);

	/* does the device *still* not have a vendor ID? */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    fu_device_get_vendor_ids(device)->len == 0) {
		g_warning ("device %s [%s] does not define a vendor-id!",
			   fu_device_get_id (device),
			   fu_device_get_name (device));
	}

	/* create new device */
	fu_device_list_add (self->device_list, device);

	/* fix order */
	fu_device_list_depsolve_order (self->device_list, device);

	/* fixup the name and format as needed from cached metadata */
	if (component != NULL)
		fu_engine_md_refresh_device_from_component (self, device, component);

	/* match the metadata so clients can tell if the device is worthy */
	fu_engine_ensure_device_supported (self, device);

	/* sometimes inherit flags from recent history */
	fu_engine_device_inherit_history (self, device);

	fu_engine_emit_changed (self);
}

static void
fu_engine_plugin_rules_changed_cb (FuPlugin *plugin, gpointer user_data)
{
	FuEngine *self = FU_ENGINE (user_data);
	GPtrArray *rules = fu_plugin_get_rules (plugin, FU_PLUGIN_RULE_INHIBITS_IDLE);
	if (rules == NULL)
		return;
	for (guint j = 0; j < rules->len; j++) {
		const gchar *tmp = g_ptr_array_index (rules, j);
		fu_idle_inhibit (self->idle, tmp);
	}
}

static void
fu_engine_context_security_changed_cb (FuContext *ctx, gpointer user_data)
{
	FuEngine *self = FU_ENGINE (user_data);

	/* invalidate host security attributes */
	g_clear_pointer (&self->host_security_id, g_free);

	/* make UI refresh */
	fu_engine_emit_changed (self);
}

static void
fu_engine_plugin_device_removed_cb (FuPlugin *plugin,
				    FuDevice *device,
				    gpointer user_data)
{
	FuEngine *self = (FuEngine *) user_data;
	FuPlugin *plugin_old;
	g_autoptr(FuDevice) device_tmp = NULL;
	g_autoptr(GError) error = NULL;

	device_tmp = fu_device_list_get_by_id (self->device_list,
					       fu_device_get_id (device),
					       &error);
	if (device_tmp == NULL) {
		g_debug ("failed to find device %s: %s",
			 fu_device_get_id (device),
			 error->message);
		return;
	}

	/* get the plugin */
	plugin_old = fu_plugin_list_find_by_name (self->plugin_list,
						  fu_device_get_plugin (device),
						  &error);
	if (plugin_old == NULL) {
		g_debug ("failed to find plugin %s: %s",
			 fu_device_get_plugin (device),
			 error->message);
		return;
	}

	/* check this came from the same plugin */
	if (g_strcmp0 (fu_plugin_get_name (plugin),
		       fu_plugin_get_name (plugin_old)) != 0) {
		g_debug ("ignoring duplicate removal from %s",
			 fu_plugin_get_name (plugin));
		return;
	}

	/* make the UI update */
	fu_device_list_remove (self->device_list, device);
	fu_engine_emit_changed (self);
}

/* this is called by the self tests as well */
void
fu_engine_add_plugin (FuEngine *self, FuPlugin *plugin)
{
	if (fu_plugin_is_open (plugin)) {
		/* plugin does not match built version */
		if (fu_plugin_get_build_hash (plugin) == NULL) {
			const gchar *name = fu_plugin_get_name (plugin);
			g_warning ("%s should call fu_plugin_set_build_hash()",
				   name);
			self->tainted = TRUE;
		} else if (g_strcmp0 (fu_plugin_get_build_hash (plugin),
				      FU_BUILD_HASH) != 0) {
			const gchar *name = fu_plugin_get_name (plugin);
			g_warning ("%s has incorrect built version %s",
				   name, fu_plugin_get_build_hash (plugin));
			self->tainted = TRUE;
		}
	}

	fu_plugin_list_add (self->plugin_list, plugin);
}

static gboolean
fu_engine_is_plugin_name_disabled (FuEngine *self, const gchar *name)
{
	GPtrArray *disabled = fu_config_get_disabled_plugins (self->config);
	for (guint i = 0; i < disabled->len; i++) {
		const gchar *name_tmp = g_ptr_array_index (disabled, i);
		if (g_strcmp0 (name_tmp, name) == 0)
			return TRUE;
	}
	return FALSE;
}

static gboolean
fu_engine_is_plugin_name_enabled (FuEngine *self, const gchar *name)
{
	if (self->plugin_filter->len == 0)
		return TRUE;
	for (guint i = 0; i < self->plugin_filter->len; i++) {
		const gchar *name_tmp = g_ptr_array_index (self->plugin_filter, i);
		if (fu_common_fnmatch (name_tmp, name))
			return TRUE;
	}
	return FALSE;
}

void
fu_engine_add_plugin_filter (FuEngine *self, const gchar *plugin_glob)
{
	GString *str;
	g_return_if_fail (FU_IS_ENGINE (self));
	g_return_if_fail (plugin_glob != NULL);
	str = g_string_new (plugin_glob);
	fu_common_string_replace (str, "-", "_");
	g_ptr_array_add (self->plugin_filter, g_string_free (str, FALSE));
}

static gboolean
fu_engine_plugin_check_supported_cb (FuPlugin *plugin, const gchar *guid, FuEngine *self)
{
	g_autoptr(XbNode) n = NULL;
	g_autofree gchar *xpath = NULL;

	if (fu_config_get_enumerate_all_devices (self->config))
		return TRUE;

	xpath = g_strdup_printf ("components/component[@type='firmware']/"
				 "provides/firmware[@type='flashed'][text()='%s']",
				 guid);
	n = xb_silo_query_first (self->silo, xpath, NULL);
	return n != NULL;
}

gboolean
fu_engine_get_tainted (FuEngine *self)
{
	return self->tainted;
}

const gchar *
fu_engine_get_host_product (FuEngine *self)
{
	const gchar *result = NULL;
	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	result = fu_context_get_hwid_value (self->ctx, FU_HWIDS_KEY_PRODUCT_NAME);
	return result != NULL ? result : "Unknown Product";
}

const gchar *
fu_engine_get_host_machine_id (FuEngine *self)
{
	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	return self->host_machine_id;
}

static void
fu_engine_ensure_security_attrs_tainted (FuEngine *self)
{
	gboolean disabled_plugins = FALSE;
	GPtrArray *disabled = fu_config_get_disabled_plugins (self->config);
	g_autoptr(FwupdSecurityAttr) attr = fwupd_security_attr_new (FWUPD_SECURITY_ATTR_ID_FWUPD_PLUGINS);
	fwupd_security_attr_set_plugin (attr, "core");
	fwupd_security_attr_add_flag (attr, FWUPD_SECURITY_ATTR_FLAG_RUNTIME_ISSUE);

	fu_security_attrs_append (self->host_security_attrs, attr);
	for (guint i = 0; i < disabled->len; i++) {
		const gchar *name_tmp = g_ptr_array_index (disabled, i);
		if (!g_str_has_prefix (name_tmp, "test") &&
		    g_strcmp0 (name_tmp, "invalid") != 0) {
			disabled_plugins = TRUE;
			break;
		}
	}
	if (self->tainted) {
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_TAINTED);
		return;
	}
	if (self->plugin_filter->len > 0 || disabled_plugins) {
		fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_ENABLED);
		return;
	}

	/* success */
	fwupd_security_attr_add_flag (attr, FWUPD_SECURITY_ATTR_FLAG_SUCCESS);
	fwupd_security_attr_set_result (attr, FWUPD_SECURITY_ATTR_RESULT_NOT_TAINTED);
}

static gchar *
fu_engine_attrs_calculate_hsi_for_chassis (FuEngine *self)
{
	guint val;

	/* get chassis type from SMBIOS data */
	val = fu_context_get_smbios_integer (self->ctx,
					     FU_SMBIOS_STRUCTURE_TYPE_CHASSIS,
					     0x05);
	if (val == G_MAXUINT)
		return g_strdup ("HSI-INVALID:chassis");

	/* verify HSI makes sense for this chassis type */
	switch (val) {
	case FU_SMBIOS_CHASSIS_KIND_DESKTOP:
	case FU_SMBIOS_CHASSIS_KIND_LOW_PROFILE_DESKTOP:
	case FU_SMBIOS_CHASSIS_KIND_MINI_TOWER:
	case FU_SMBIOS_CHASSIS_KIND_TOWER:
	case FU_SMBIOS_CHASSIS_KIND_PORTABLE:
	case FU_SMBIOS_CHASSIS_KIND_LAPTOP:
	case FU_SMBIOS_CHASSIS_KIND_NOTEBOOK:
	case FU_SMBIOS_CHASSIS_KIND_ALL_IN_ONE:
	case FU_SMBIOS_CHASSIS_KIND_SUB_NOTEBOOK:
	case FU_SMBIOS_CHASSIS_KIND_LUNCH_BOX:
	case FU_SMBIOS_CHASSIS_KIND_MAIN_SERVER:
	case FU_SMBIOS_CHASSIS_KIND_TABLET:
	case FU_SMBIOS_CHASSIS_KIND_CONVERTIBLE:
	case FU_SMBIOS_CHASSIS_KIND_DETACHABLE:
	case FU_SMBIOS_CHASSIS_KIND_IOT_GATEWAY:
	case FU_SMBIOS_CHASSIS_KIND_EMBEDDED_PC:
	case FU_SMBIOS_CHASSIS_KIND_MINI_PC:
	case FU_SMBIOS_CHASSIS_KIND_STICK_PC:
		return fu_security_attrs_calculate_hsi (self->host_security_attrs,
							FU_SECURITY_ATTRS_FLAG_ADD_VERSION);
	default:
		break;
	}

	/* failed */
	return g_strdup_printf ("HSI-INVALID:chassis[0x%02x]", val);
}


static void
fu_engine_ensure_security_attrs (FuEngine *self)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	g_autoptr(GPtrArray) devices = fu_device_list_get_all (self->device_list);
	g_autoptr(GPtrArray) items = NULL;

	/* already valid */
	if (self->host_security_id != NULL)
		return;

	/* clear old values */
	fu_security_attrs_remove_all (self->host_security_attrs);

	/* built in */
	fu_engine_ensure_security_attrs_tainted (self);

	/* call into devices */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index (devices, i);
		fu_device_add_security_attrs (device, self->host_security_attrs);
	}

	/* call into plugins */
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		fu_plugin_runner_add_security_attrs (plugin_tmp, self->host_security_attrs);
	}

	/* set the fallback names for clients without native translations */
	items = fu_security_attrs_get_all (self->host_security_attrs);
	for (guint i = 0; i < items->len; i++) {
		FwupdSecurityAttr *attr = g_ptr_array_index (items, i);
		if (fwupd_security_attr_get_name (attr) == NULL) {
			g_autofree gchar *name_tmp = fu_security_attr_get_name (attr);
			if (name_tmp == NULL) {
				g_warning ("failed to get fallback for %s",
					   fwupd_security_attr_get_appstream_id (attr));
				continue;
			}
			fwupd_security_attr_set_name (attr, name_tmp);
		}
	}

	/* set the obsoletes flag for each attr */
	fu_security_attrs_depsolve (self->host_security_attrs);

	/* distil into one simple string */
	g_free (self->host_security_id);
	self->host_security_id = fu_engine_attrs_calculate_hsi_for_chassis (self);
}

const gchar *
fu_engine_get_host_security_id (FuEngine *self)
{
	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	fu_engine_ensure_security_attrs (self);
	return self->host_security_id;
}

FuSecurityAttrs *
fu_engine_get_host_security_attrs (FuEngine *self)
{
	g_return_val_if_fail (FU_IS_ENGINE (self), NULL);
	fu_engine_ensure_security_attrs (self);
	return g_object_ref (self->host_security_attrs);
}

gboolean
fu_engine_load_plugins (FuEngine *self, GError **error)
{
	const gchar *fn;
	g_autoptr(GDir) dir = NULL;
	g_autofree gchar *plugin_path = NULL;
	g_autofree gchar *suffix = g_strdup_printf (".%s", G_MODULE_SUFFIX);
	g_autoptr(GPtrArray) plugins_disabled = g_ptr_array_new_with_free_func (g_free);
	g_autoptr(GPtrArray) plugins_disabled_rt = g_ptr_array_new_with_free_func (g_free);

	/* search */
	plugin_path = fu_common_get_path (FU_PATH_KIND_PLUGINDIR_PKG);
	dir = g_dir_open (plugin_path, 0, error);
	if (dir == NULL)
		return FALSE;
	while ((fn = g_dir_read_name (dir)) != NULL) {
		g_autofree gchar *filename = NULL;
		g_autofree gchar *name = NULL;
		g_autoptr(FuPlugin) plugin = NULL;
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GPtrArray) firmware_gtypes = NULL;

		/* ignore non-plugins */
		if (!g_str_has_suffix (fn, suffix))
			continue;

		/* is disabled */
		name = fu_plugin_guess_name_from_fn (fn);
		if (name == NULL)
			continue;
		if (fu_engine_is_plugin_name_disabled (self, name) ||
		    !fu_engine_is_plugin_name_enabled (self, name)) {
			g_ptr_array_add (plugins_disabled, g_steal_pointer (&name));
			continue;
		}

		/* open module */
		filename = g_build_filename (plugin_path, fn, NULL);
		plugin = fu_plugin_new (self->ctx);
		fu_plugin_set_name (plugin, name);

		/* if loaded from fu_engine_load() open the plugin */
		firmware_gtypes = fu_context_get_firmware_gtype_ids (self->ctx);
		if (firmware_gtypes->len > 0) {
			if (!fu_plugin_open (plugin, filename, &error_local)) {
				g_warning ("cannot load: %s", error_local->message);
				fu_engine_add_plugin (self, plugin);
				continue;
			}
		}

		/* runtime disabled */
		if (fu_plugin_has_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED)) {
			g_ptr_array_add (plugins_disabled_rt, g_steal_pointer (&name));
			continue;
		}

		/* watch for changes */
		g_signal_connect (plugin, "device-added",
				  G_CALLBACK (fu_engine_plugin_device_added_cb),
				  self);
		g_signal_connect (plugin, "device-removed",
				  G_CALLBACK (fu_engine_plugin_device_removed_cb),
				  self);
		g_signal_connect (plugin, "device-register",
				  G_CALLBACK (fu_engine_plugin_device_register_cb),
				  self);
		g_signal_connect (plugin, "check-supported",
				  G_CALLBACK (fu_engine_plugin_check_supported_cb),
				  self);
		g_signal_connect (plugin, "rules-changed",
				  G_CALLBACK (fu_engine_plugin_rules_changed_cb),
				  self);

		/* add */
		fu_engine_add_plugin (self, plugin);
	}

	/* show list */
	if (plugins_disabled->len > 0) {
		g_autofree gchar *str = NULL;
		g_ptr_array_add (plugins_disabled, NULL);
		str = g_strjoinv (", ", (gchar **) plugins_disabled->pdata);
		g_debug ("plugins disabled: %s", str);
	}
	if (plugins_disabled_rt->len > 0) {
		g_autofree gchar *str = NULL;
		g_ptr_array_add (plugins_disabled_rt, NULL);
		str = g_strjoinv (", ", (gchar **) plugins_disabled_rt->pdata);
		g_debug ("plugins runtime-disabled: %s", str);
	}

	/* depsolve into the correct order */
	if (!fu_plugin_list_depsolve (self->plugin_list, error))
		return FALSE;

	/* success */
	return TRUE;
}

static gboolean
fu_engine_cleanup_state (GError **error)
{
	const gchar *filenames[] = {
		"/var/cache/app-info/xmls/fwupd-verify.xml",
		"/var/cache/app-info/xmls/fwupd.xml",
		NULL };
	for (guint i = 0; filenames[i] != NULL; i++) {
		g_autoptr(GFile) file = g_file_new_for_path (filenames[i]);
		if (g_file_query_exists (file, NULL)) {
			if (!g_file_delete (file, NULL, error))
				return FALSE;
		}
	}
	return TRUE;
}

guint64
fu_engine_get_archive_size_max (FuEngine *self)
{
	return fu_config_get_archive_size_max (self->config);
}

static void
fu_engine_backend_device_removed_cb (FuBackend *backend, FuDevice *device, FuEngine *self)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* debug */
	if (g_getenv ("FWUPD_PROBE_VERBOSE") != NULL) {
		g_debug ("%s removed %s",
			 fu_backend_get_name (backend),
			 fu_device_get_backend_id (device));
	}

	/* go through each device and remove any that match */
	devices = fu_device_list_get_all (self->device_list);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device_tmp = g_ptr_array_index (devices, i);
		if (g_strcmp0 (fu_device_get_backend_id (device_tmp),
			       fu_device_get_backend_id (device)) == 0) {
			g_debug ("auto-removing backend device %s [%s]",
				 fu_device_get_name (device_tmp),
				 fu_device_get_id (device_tmp));
			fu_device_list_remove (self->device_list, device_tmp);
		}
	}
}

static void
fu_engine_backend_device_added_cb (FuBackend *backend, FuDevice *device, FuEngine *self)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) possible_plugins = NULL;

	/* super useful for plugin development */
	if (g_getenv ("FWUPD_PROBE_VERBOSE") != NULL) {
		g_autofree gchar *str = fu_device_to_string (FU_DEVICE (device));
		g_debug ("%s added %s", fu_backend_get_name (backend), str);
	}

	/* add any extra quirks */
	fu_device_set_context (device, self->ctx);
	if (!fu_device_probe (device, &error_local)) {
		g_warning ("failed to probe device %s: %s",
			   fu_device_get_backend_id (device),
			   error_local->message);
		return;
	}

	/* super useful for plugin development */
	if (g_getenv ("FWUPD_PROBE_VERBOSE") != NULL) {
		g_autofree gchar *str = fu_device_to_string (FU_DEVICE (device));
		g_debug ("%s added %s", fu_backend_get_name (backend), str);
	}

	/* can be specified using a quirk */
	possible_plugins = fu_device_get_possible_plugins (device);
	for (guint i = 0; i < possible_plugins->len; i++) {
		FuPlugin *plugin;
		const gchar *plugin_name = g_ptr_array_index (possible_plugins, i);
		g_autoptr(GError) error = NULL;

		plugin = fu_plugin_list_find_by_name (self->plugin_list, plugin_name, NULL);
		if (plugin == NULL)
			continue;
		if (!fu_plugin_runner_backend_device_added (plugin, device, &error)) {
			if (g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
				if (g_getenv ("FWUPD_PROBE_VERBOSE") != NULL) {
					g_debug ("%s ignoring: %s",
						 fu_plugin_get_name (plugin),
						 error->message);
				}
				continue;
			}
			g_warning ("failed to add device %s: %s",
				   fu_device_get_backend_id (device),
				   error->message);
			continue;
		}
	}
}

static void
fu_engine_backend_device_changed_cb (FuBackend *backend, FuDevice *device, FuEngine *self)
{
	GPtrArray *plugins = fu_plugin_list_get_all (self->plugin_list);
	g_autoptr(GPtrArray) devices = NULL;

	/* debug */
	if (g_getenv ("FWUPD_PROBE_VERBOSE") != NULL) {
		g_debug ("%s changed %s",
			 fu_backend_get_name (backend),
			 fu_device_get_physical_id (device));
	}

	/* emit changed on any that match */
	devices = fu_device_list_get_all (self->device_list);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device_tmp = g_ptr_array_index (devices, i);
		if (!FU_IS_UDEV_DEVICE (device_tmp))
			continue;
		if (g_strcmp0 (fu_udev_device_get_sysfs_path (FU_UDEV_DEVICE (device_tmp)),
			       fu_udev_device_get_sysfs_path (FU_UDEV_DEVICE (device))) == 0) {
			fu_udev_device_emit_changed (FU_UDEV_DEVICE (device));
		}
	}

	/* run all plugins */
	for (guint j = 0; j < plugins->len; j++) {
		FuPlugin *plugin_tmp = g_ptr_array_index (plugins, j);
		g_autoptr(GError) error = NULL;
		if (!fu_plugin_runner_backend_device_changed (plugin_tmp, device, &error)) {
			if (g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
				g_debug ("%s ignoring: %s",
					 fu_plugin_get_name (plugin_tmp),
					 error->message);
				continue;
			}
			g_warning ("%s failed to change udev device %s: %s",
				   fu_plugin_get_name (plugin_tmp),
				   fu_udev_device_get_sysfs_path (FU_UDEV_DEVICE (device)),
				   error->message);
		}
	}
}

static void
fu_engine_load_quirks_for_hwid (FuEngine *self, const gchar *hwid)
{
	FuPlugin *plugin;
	const gchar *value;
	g_auto(GStrv) plugins = NULL;

	/* does prefixed quirk exist */
	value = fu_context_lookup_quirk_by_id (self->ctx, hwid, FU_QUIRKS_PLUGIN);
	if (value == NULL)
		return;
	plugins = g_strsplit (value, ",", -1);
	for (guint i = 0; plugins[i] != NULL; i++) {
		g_autoptr(GError) error_local = NULL;
		plugin = fu_plugin_list_find_by_name (self->plugin_list,
						      plugins[i], &error_local);
		if (plugin == NULL) {
			g_debug ("no %s plugin for HwId %s: %s",
				 plugins[i], hwid, error_local->message);
			continue;
		}
		g_debug ("enabling %s due to HwId %s", plugins[i], hwid);
		fu_plugin_remove_flag (plugin, FWUPD_PLUGIN_FLAG_REQUIRE_HWID);
	}
}

static void
fu_engine_load_quirks (FuEngine *self, FuQuirksLoadFlags quirks_flags)
{
	GPtrArray *guids = fu_context_get_hwid_guids (self->ctx);
	g_autoptr(GError) error = NULL;

	/* rebuild silo if required */
	if (!fu_context_load_quirks (self->ctx, quirks_flags, &error)) {
		g_warning ("Failed to load quirks: %s", error->message);
		return;
	}

	/* search each hwid */
	for (guint i = 0; i < guids->len; i++) {
		const gchar *hwid = g_ptr_array_index (guids, i);
		fu_engine_load_quirks_for_hwid (self, hwid);
	}
}

static gboolean
fu_engine_update_history_device (FuEngine *self, FuDevice *dev_history, GError **error)
{
	FuPlugin *plugin;
	FwupdRelease *rel_history;
	g_autofree gchar *btime = NULL;
	g_autoptr(FuDevice) dev = NULL;
	g_autoptr(GHashTable) metadata_device = NULL;

	/* is in the device list */
	dev = fu_device_list_get_by_id (self->device_list,
					fu_device_get_id (dev_history),
					error);
	if (dev == NULL)
		return FALSE;

	/* does the installed version match what we tried to install
	 * before fwupd was restarted */
	rel_history = fu_device_get_release_default (dev_history);
	if (rel_history == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "no release for history FuDevice");
		return FALSE;
	}

	/* is this the same boot time as when we scheduled the update,
	 * i.e. has fwupd been restarted before we rebooted */
	btime = fu_engine_get_boot_time ();
	if (g_strcmp0 (fwupd_release_get_metadata_item (rel_history, "BootTime"),
		       btime) == 0) {
		g_debug ("service restarted, but no reboot has taken place");

		/* if it needed reboot then, it also needs it now... */
		if (fu_device_get_update_state (dev_history) == FWUPD_UPDATE_STATE_NEEDS_REBOOT) {
			g_debug ("inheriting needs-reboot for %s",
				 fu_device_get_name (dev));
			fu_device_set_update_state (dev, FWUPD_UPDATE_STATE_NEEDS_REBOOT);
		}
		return TRUE;
	}

	/* save any additional report metadata */
	metadata_device = fu_device_report_metadata_post (dev);
	if (metadata_device != NULL && g_hash_table_size (metadata_device) > 0) {
		fwupd_release_add_metadata (rel_history, metadata_device);
		if (!fu_history_set_device_metadata (self->history,
						     fu_device_get_id (dev_history),
						     fwupd_release_get_metadata (rel_history),
						     error)) {
			g_prefix_error (error, "failed to set metadata: ");
			return FALSE;
		}
	}

	/* the system is running with the new firmware version */
	if (fu_common_vercmp_full (fu_device_get_version (dev),
				   fwupd_release_get_version (rel_history),
				   fu_device_get_version_format (dev)) == 0) {
		GPtrArray *checksums;
		g_debug ("installed version %s matching history %s",
			 fu_device_get_version (dev),
			 fwupd_release_get_version (rel_history));

		/* copy over runtime checksums if set from probe() */
		checksums = fu_device_get_checksums (dev);
		for (guint i = 0; i < checksums->len; i++) {
			const gchar *csum = g_ptr_array_index (checksums, i);
			fu_device_add_checksum (dev_history, csum);
		}
		fu_device_set_version_format (dev_history, fu_device_get_version_format (dev));
		fu_device_set_version (dev_history, fu_device_get_version (dev));
		fu_device_remove_flag (dev_history, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION);
		fu_device_set_update_state (dev_history, FWUPD_UPDATE_STATE_SUCCESS);
		fu_device_set_update_error (dev_history, NULL);
		return fu_history_modify_device (self->history, dev_history, error);
	}

	/* does the plugin know the update failure */
	plugin = fu_plugin_list_find_by_name (self->plugin_list,
					      fu_device_get_plugin (dev),
					      error);
	if (plugin == NULL)
		return FALSE;
	if (!fu_plugin_runner_get_results (plugin, dev, error))
		return FALSE;

	/* the plugin either can't tell us the error, or doesn't know itself */
	if (fu_device_get_update_state (dev) != FWUPD_UPDATE_STATE_FAILED &&
	    fu_device_get_update_state (dev) != FWUPD_UPDATE_STATE_FAILED_TRANSIENT) {
		g_debug ("falling back to generic failure");
		fu_device_set_update_state (dev_history, FWUPD_UPDATE_STATE_FAILED);
		fu_device_set_update_error (dev_history, "failed to run update on reboot");
	} else {
		fu_device_set_update_state (dev_history, fu_device_get_update_state (dev));
		fu_device_set_update_error (dev_history, fu_device_get_update_error (dev));
	}

	/* update the state in the database */
	return fu_history_modify_device (self->history, dev_history, error);
}

static gboolean
fu_engine_update_history_database (FuEngine *self, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	/* get any devices */
	devices = fu_history_get_devices (self->history, error);
	if (devices == NULL)
		return FALSE;
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		g_autoptr(GError) error_local = NULL;

		/* not in the required state */
		if (fu_device_get_update_state (dev) != FWUPD_UPDATE_STATE_NEEDS_REBOOT &&
		    fu_device_get_update_state (dev) != FWUPD_UPDATE_STATE_PENDING)
			continue;

		/* try to save the new update-state, but ignoring any error */
		if (!fu_engine_update_history_device (self, dev, &error_local)) {
			g_warning ("failed to update history database: %s",
				   error_local->message);
		}
	}
	return TRUE;
}

static void
fu_engine_ensure_client_certificate (FuEngine *self)
{
	g_autoptr(GBytes) blob = g_bytes_new_static ("test\0", 5);
	g_autoptr(GError) error = NULL;
	g_autoptr(JcatBlob) jcat_sig = NULL;
	g_autoptr(JcatEngine) jcat_engine = NULL;

	/* create keyring and sign dummy data to ensure certificate exists */
	jcat_engine = jcat_context_get_engine (self->jcat_context,
					       JCAT_BLOB_KIND_PKCS7,
					       &error);
	if (jcat_engine == NULL) {
		g_message ("failed to create keyring: %s", error->message);
		return;
	}
	jcat_sig = jcat_engine_self_sign (jcat_engine, blob, JCAT_SIGN_FLAG_NONE, &error);
	if (jcat_sig == NULL) {
		g_message ("failed to sign using keyring: %s", error->message);
		return;
	}
	g_debug ("client certificate exists and working");
}

/**
 * fu_engine_load:
 * @self: a #FuEngine
 * @flags: engine load flags, e.g. %FU_ENGINE_LOAD_FLAG_READONLY
 * @error: (nullable): optional return location for an error
 *
 * Load the firmware update engine so it is ready for use.
 *
 * Returns: %TRUE for success
 **/
gboolean
fu_engine_load (FuEngine *self, FuEngineLoadFlags flags, GError **error)
{
	FuQuirksLoadFlags quirks_flags = FU_QUIRKS_LOAD_FLAG_NONE;
	guint backend_cnt = 0;
	g_autoptr(GPtrArray) checksums_approved = NULL;
	g_autoptr(GPtrArray) checksums_blocked = NULL;
#ifndef _WIN32
	g_autoptr(GError) error_local = NULL;
#endif

	g_return_val_if_fail (FU_IS_ENGINE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* avoid re-loading a second time if fu-tool or fu-util request to */
	if (self->loaded)
		return TRUE;

	/* sanity check libraries are in sync with daemon */
	if (g_strcmp0 (fwupd_version_string (), VERSION) != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVAL,
			     "libfwupd version %s does not match daemon %s",
			     fwupd_version_string (),
			     VERSION);
		return FALSE;
	}
	if (g_strcmp0 (fu_version_string (), VERSION) != 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVAL,
			     "libfwupdplugin version %s does not match daemon %s",
			     fu_version_string (),
			     VERSION);
		return FALSE;
	}

/* TODO: Read registry key [HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Cryptography] "MachineGuid" */
#ifndef _WIN32
	/* cache machine ID so we can use it from a sandboxed app */
	self->host_machine_id = fwupd_build_machine_id ("fwupd", &error_local);
	if (self->host_machine_id == NULL)
		g_debug ("failed to build machine-id: %s", error_local->message);
#endif
	/* read config file */
	if (!fu_config_load (self->config, error)) {
		g_prefix_error (error, "Failed to load config: ");
		return FALSE;
	}

	/* read remotes */
	if (flags & FU_ENGINE_LOAD_FLAG_REMOTES) {
		FuRemoteListLoadFlags remote_list_flags = FU_REMOTE_LIST_LOAD_FLAG_NONE;
		if (flags & FU_ENGINE_LOAD_FLAG_READONLY)
			remote_list_flags |= FU_REMOTE_LIST_LOAD_FLAG_READONLY_FS;
		if (!fu_remote_list_load (self->remote_list, remote_list_flags, error)) {
			g_prefix_error (error, "Failed to load remotes: ");
			return FALSE;
		}
	}

	/* create client certificate */
	fu_engine_ensure_client_certificate (self);

	/* get hardcoded approved and blocked firmware */
	checksums_approved = fu_config_get_approved_firmware (self->config);
	for (guint i = 0; i < checksums_approved->len; i++) {
		const gchar *csum = g_ptr_array_index (checksums_approved, i);
		fu_engine_add_approved_firmware (self, csum);
	}
	checksums_blocked = fu_config_get_blocked_firmware (self->config);
	for (guint i = 0; i < checksums_blocked->len; i++) {
		const gchar *csum = g_ptr_array_index (checksums_blocked, i);
		fu_engine_add_blocked_firmware (self, csum);
	}

	/* get extra firmware saved to the database */
	checksums_approved = fu_history_get_approved_firmware (self->history, error);
	if (checksums_approved == NULL)
		return FALSE;
	for (guint i = 0; i < checksums_approved->len; i++) {
		const gchar *csum = g_ptr_array_index (checksums_approved, i);
		fu_engine_add_approved_firmware (self, csum);
	}
	checksums_blocked = fu_history_get_blocked_firmware (self->history, error);
	if (checksums_blocked == NULL)
		return FALSE;
	for (guint i = 0; i < checksums_blocked->len; i++) {
		const gchar *csum = g_ptr_array_index (checksums_blocked, i);
		fu_engine_add_blocked_firmware (self, csum);
	}

	/* set up idle exit */
	if ((self->app_flags & FU_APP_FLAGS_NO_IDLE_SOURCES) == 0)
		fu_idle_set_timeout (self->idle, fu_config_get_idle_timeout (self->config));

	/* load SMBIOS and the hwids */
	if (flags & FU_ENGINE_LOAD_FLAG_HWINFO)
		fu_context_load_hwinfo (self->ctx, NULL);

	/* load AppStream metadata */
	if (!fu_engine_load_metadata_store (self, flags, error)) {
		g_prefix_error (error, "Failed to load AppStream data: ");
		return FALSE;
	}

	/* add the "built-in" firmware types */
	fu_context_add_firmware_gtype (self->ctx, "raw", FU_TYPE_FIRMWARE);
	fu_context_add_firmware_gtype (self->ctx, "dfu", FU_TYPE_DFU_FIRMWARE);
	fu_context_add_firmware_gtype (self->ctx, "dfuse", FU_TYPE_DFUSE_FIRMWARE);
	fu_context_add_firmware_gtype (self->ctx, "fmap", FU_TYPE_FMAP_FIRMWARE);
	fu_context_add_firmware_gtype (self->ctx, "ihex", FU_TYPE_IHEX_FIRMWARE);
	fu_context_add_firmware_gtype (self->ctx, "srec", FU_TYPE_SREC_FIRMWARE);
	fu_context_add_firmware_gtype (self->ctx, "smbios", FU_TYPE_SMBIOS);
	fu_context_add_firmware_gtype (self->ctx, "efi-firmware-file", FU_TYPE_EFI_FIRMWARE_FILE);
	fu_context_add_firmware_gtype (self->ctx, "efi-firmware-filesystem", FU_TYPE_EFI_FIRMWARE_FILESYSTEM);
	fu_context_add_firmware_gtype (self->ctx, "efi-firmware-section", FU_TYPE_EFI_FIRMWARE_SECTION);
	fu_context_add_firmware_gtype (self->ctx, "efi-firmware-colume", FU_TYPE_EFI_FIRMWARE_VOLUME);
	fu_context_add_firmware_gtype (self->ctx, "ifd-bios", FU_TYPE_IFD_BIOS);
	fu_context_add_firmware_gtype (self->ctx, "ifd-firmware", FU_TYPE_IFD_FIRMWARE);

	/* set up backends */
	for (guint i = 0; i < self->backends->len; i++) {
		FuBackend *backend = g_ptr_array_index (self->backends, i);
		g_autoptr(GError) error_backend = NULL;
		if (!fu_backend_setup (backend, &error_backend)) {
			g_debug ("failed to setup backend %s: %s",
				 fu_backend_get_name (backend),
				 error_backend->message);
			continue;
		}
		backend_cnt++;
	}
	if (backend_cnt == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "all backends failed setup");
		return FALSE;
	}

	/* delete old data files */
	if (!fu_engine_cleanup_state (error)) {
		g_prefix_error (error, "Failed to clean up: ");
		return FALSE;
	}

	/* load plugin */
	if (!fu_engine_load_plugins (self, error)) {
		g_prefix_error (error, "Failed to load plugins: ");
		return FALSE;
	}

	/* on a read-only filesystem don't care about the cache GUID */
	if (flags & FU_ENGINE_LOAD_FLAG_READONLY)
		quirks_flags |= FU_QUIRKS_LOAD_FLAG_READONLY_FS;
	fu_engine_load_quirks (self, quirks_flags);

	/* watch the device list for updates and proxy */
	g_signal_connect (self->device_list, "added",
			  G_CALLBACK (fu_engine_device_added_cb),
			  self);
	g_signal_connect (self->device_list, "removed",
			  G_CALLBACK (fu_engine_device_removed_cb),
			  self);
	g_signal_connect (self->device_list, "changed",
			  G_CALLBACK (fu_engine_device_changed_cb),
			  self);
	fu_engine_set_status (self, FWUPD_STATUS_LOADING);

	/* add devices */
	fu_engine_plugins_setup (self);
	if (flags & FU_ENGINE_LOAD_FLAG_COLDPLUG)
		fu_engine_plugins_coldplug (self);

	/* coldplug backends */
	if (flags & FU_ENGINE_LOAD_FLAG_COLDPLUG) {
		for (guint i = 0; i < self->backends->len; i++) {
			FuBackend *backend = g_ptr_array_index (self->backends, i);
			g_autoptr(GError) error_backend = NULL;
			if (!fu_backend_get_enabled (backend))
				continue;
			g_signal_connect (backend, "device-added",
					  G_CALLBACK (fu_engine_backend_device_added_cb),
					  self);
			g_signal_connect (backend, "device-removed",
					  G_CALLBACK (fu_engine_backend_device_removed_cb),
					  self);
			g_signal_connect (backend, "device-changed",
					  G_CALLBACK (fu_engine_backend_device_changed_cb),
					  self);
			if (!fu_backend_coldplug (backend, &error_backend)) {
				g_warning ("failed to coldplug backend %s: %s",
					   fu_backend_get_name (backend),
					   error_backend->message);
				continue;
			}
		}
	}

	/* set device properties from the metadata */
	fu_engine_md_refresh_devices (self);

	/* update the db for devices that were updated during the reboot */
	if (!fu_engine_update_history_database (self, error))
		return FALSE;

	fu_engine_set_status (self, FWUPD_STATUS_IDLE);
	self->loaded = TRUE;

	/* let clients know engine finished starting up */
	fu_engine_emit_changed (self);

	/* success */
	return TRUE;
}

static void
fu_engine_class_init (FuEngineClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = fu_engine_finalize;

	signals[SIGNAL_CHANGED] =
		g_signal_new ("changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);
	signals[SIGNAL_DEVICE_ADDED] =
		g_signal_new ("device-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, FU_TYPE_DEVICE);
	signals[SIGNAL_DEVICE_REMOVED] =
		g_signal_new ("device-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, FU_TYPE_DEVICE);
	signals[SIGNAL_DEVICE_CHANGED] =
		g_signal_new ("device-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, FU_TYPE_DEVICE);
	signals[SIGNAL_STATUS_CHANGED] =
		g_signal_new ("status-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__UINT,
			      G_TYPE_NONE, 1, G_TYPE_UINT);
	signals[SIGNAL_PERCENTAGE_CHANGED] =
		g_signal_new ("percentage-changed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__UINT,
			      G_TYPE_NONE, 1, G_TYPE_UINT);
}

void
fu_engine_add_runtime_version (FuEngine *self,
			       const gchar *component_id,
			       const gchar *version)
{
	fu_context_add_runtime_version (self->ctx, component_id, version);
}

void
fu_engine_add_app_flag (FuEngine *self, FuAppFlags app_flags)
{
	g_return_if_fail (FU_IS_ENGINE (self));
	self->app_flags |= app_flags;
}

static void
fu_engine_context_battery_changed_cb (FuContext *ctx, GParamSpec *pspec, FuEngine *self)
{
	g_autoptr(GPtrArray) devices = fu_device_list_get_all (self->device_list);

	/* apply policy on any existing devices */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index (devices, i);
		fu_engine_ensure_device_battery_inhibit (self, device);
	}
}

static void
fu_engine_idle_status_notify_cb (FuIdle *idle, GParamSpec *pspec, FuEngine *self)
{
	FwupdStatus status = fu_idle_get_status (idle);
	if (status == FWUPD_STATUS_SHUTDOWN)
		fu_engine_set_status (self, status);
}

static void
fu_engine_init (FuEngine *self)
{
#ifdef HAVE_UTSNAME_H
	struct utsname uname_tmp;
#endif
	g_autofree gchar *keyring_path = NULL;
	g_autofree gchar *pkidir_fw = NULL;
	g_autofree gchar *pkidir_md = NULL;
	g_autofree gchar *sysconfdir = NULL;
	self->percentage = 0;
	self->status = FWUPD_STATUS_IDLE;
	self->config = fu_config_new ();
	self->remote_list = fu_remote_list_new ();
	self->device_list = fu_device_list_new ();
	self->ctx = fu_context_new ();
	self->idle = fu_idle_new ();
	self->history = fu_history_new ();
	self->plugin_list = fu_plugin_list_new ();
	self->plugin_filter = g_ptr_array_new_with_free_func (g_free);
	self->host_security_attrs = fu_security_attrs_new ();
	self->backends = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	self->runtime_versions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	self->compile_versions = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

	fu_context_set_runtime_versions (self->ctx, self->runtime_versions);
	fu_context_set_compile_versions (self->ctx, self->compile_versions);

	g_signal_connect (self->ctx, "security-changed",
			  G_CALLBACK (fu_engine_context_security_changed_cb),
			  self);
	g_signal_connect (self->ctx, "notify::battery-state",
			  G_CALLBACK (fu_engine_context_battery_changed_cb),
			  self);
	g_signal_connect (self->ctx, "notify::battery-level",
			  G_CALLBACK (fu_engine_context_battery_changed_cb),
			  self);
	g_signal_connect (self->ctx, "notify::battery-threshold",
			  G_CALLBACK (fu_engine_context_battery_changed_cb),
			  self);

	g_signal_connect (self->config, "changed",
			  G_CALLBACK (fu_engine_config_changed_cb),
			  self);
	g_signal_connect (self->remote_list, "changed",
			  G_CALLBACK (fu_engine_remote_list_changed_cb),
			  self);

	g_signal_connect (self->idle, "notify::status",
			  G_CALLBACK (fu_engine_idle_status_notify_cb), self);

	/* backends */
#ifdef HAVE_GUSB
	g_ptr_array_add (self->backends, fu_usb_backend_new ());
#endif
#ifdef HAVE_GUDEV
	g_ptr_array_add (self->backends, fu_udev_backend_new (fu_context_get_udev_subsystems (self->ctx)));
#endif
#ifdef HAVE_BLUEZ
	g_ptr_array_add (self->backends, fu_bluez_backend_new ());
#endif

	/* setup Jcat context */
	self->jcat_context = jcat_context_new ();
	keyring_path = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	jcat_context_set_keyring_path (self->jcat_context, keyring_path);
	sysconfdir = fu_common_get_path (FU_PATH_KIND_SYSCONFDIR);
	pkidir_fw = g_build_filename (sysconfdir, "pki", "fwupd", NULL);
	jcat_context_add_public_keys (self->jcat_context, pkidir_fw);
	pkidir_md = g_build_filename (sysconfdir, "pki", "fwupd-metadata", NULL);
	jcat_context_add_public_keys (self->jcat_context, pkidir_md);

	/* add some runtime versions of things the daemon depends on */
	fu_engine_add_runtime_version (self, "org.freedesktop.fwupd", VERSION);
	fu_engine_add_runtime_version (self, "com.redhat.fwupdate", "12");
	fu_engine_add_runtime_version (self, "org.freedesktop.appstream-glib", "0.7.14");
#if G_USB_CHECK_VERSION(0,3,1)
	fu_engine_add_runtime_version (self, "org.freedesktop.gusb", g_usb_version_string ());
#endif

	/* optional kernel version */
#ifdef HAVE_UTSNAME_H
	memset (&uname_tmp, 0, sizeof(uname_tmp));
	if (uname (&uname_tmp) >= 0)
		fu_engine_add_runtime_version (self, "org.kernel", uname_tmp.release);
#endif

	g_hash_table_insert (self->compile_versions,
			     g_strdup ("com.redhat.fwupdate"),
			     g_strdup ("12"));
	g_hash_table_insert (self->compile_versions,
			     g_strdup ("org.freedesktop.fwupd"),
			     g_strdup (VERSION));
#ifdef HAVE_GUSB
	g_hash_table_insert (self->compile_versions,
			     g_strdup ("org.freedesktop.gusb"),
			     g_strdup_printf ("%i.%i.%i",
					      G_USB_MAJOR_VERSION,
					      G_USB_MINOR_VERSION,
					      G_USB_MICRO_VERSION));
#endif
}

static void
fu_engine_finalize (GObject *obj)
{
	FuEngine *self = FU_ENGINE (obj);

	if (self->silo != NULL)
		g_object_unref (self->silo);
	if (self->coldplug_id != 0)
		g_source_remove (self->coldplug_id);
	if (self->approved_firmware != NULL)
		g_hash_table_unref (self->approved_firmware);
	if (self->blocked_firmware != NULL)
		g_hash_table_unref (self->blocked_firmware);

	g_free (self->host_machine_id);
	g_free (self->host_security_id);
	g_object_unref (self->host_security_attrs);
	g_object_unref (self->idle);
	g_object_unref (self->config);
	g_object_unref (self->remote_list);
	g_object_unref (self->ctx);
	g_object_unref (self->history);
	g_object_unref (self->device_list);
	g_object_unref (self->jcat_context);
	g_ptr_array_unref (self->plugin_filter);
	g_ptr_array_unref (self->backends);
	g_hash_table_unref (self->runtime_versions);
	g_hash_table_unref (self->compile_versions);
	g_object_unref (self->plugin_list);

	G_OBJECT_CLASS (fu_engine_parent_class)->finalize (obj);
}

FuEngine *
fu_engine_new (FuAppFlags app_flags)
{
	FuEngine *self;
	self = g_object_new (FU_TYPE_ENGINE, NULL);
	self->app_flags = app_flags;
	return FU_ENGINE (self);
}
