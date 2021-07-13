/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#define G_LOG_DOMAIN				"FuMain"

#include "config.h"

#include <fwupd.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#ifdef HAVE_GIO_UNIX
#include <glib-unix.h>
#endif
#include <fcntl.h>
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>
#include <jcat.h>

#include "fu-cabinet.h"
#include "fu-context-private.h"
#include "fu-device-private.h"
#include "fu-engine.h"
#include "fu-history.h"
#include "fu-plugin-private.h"
#include "fu-progressbar.h"
#include "fu-security-attrs-private.h"
#include "fu-smbios-private.h"
#include "fu-util-common.h"
#include "fu-hwids.h"
#include "fu-debug.h"
#include "fwupd-common-private.h"
#include "fwupd-device-private.h"

#ifdef HAVE_SYSTEMD
#include "fu-systemd.h"
#endif

/* custom return code */
#define EXIT_NOTHING_TO_DO		2

typedef enum {
	FU_UTIL_OPERATION_UNKNOWN,
	FU_UTIL_OPERATION_UPDATE,
	FU_UTIL_OPERATION_INSTALL,
	FU_UTIL_OPERATION_READ,
	FU_UTIL_OPERATION_LAST
} FuUtilOperation;

struct FuUtilPrivate {
	GCancellable		*cancellable;
	GMainContext		*main_ctx;
	GMainLoop		*loop;
	GOptionContext		*context;
	FuEngine		*engine;
	FuEngineRequest		*request;
	FuProgressbar		*progressbar;
	gboolean		 no_reboot_check;
	gboolean		 no_safety_check;
	gboolean		 prepare_blob;
	gboolean		 cleanup_blob;
	gboolean		 enable_json_state;
	FwupdInstallFlags	 flags;
	gboolean		 show_all;
	gboolean		 disable_ssl_strict;
	gint			 lock_fd;
	/* only valid in update and downgrade */
	FuUtilOperation		 current_operation;
	FwupdDevice		*current_device;
	gchar			*current_message;
	FwupdDeviceFlags	 completion_flags;
	FwupdDeviceFlags	 filter_include;
	FwupdDeviceFlags	 filter_exclude;
};

static gboolean
fu_util_save_current_state (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autofree gchar *state = NULL;
	g_autofree gchar *dirname = NULL;
	g_autofree gchar *filename = NULL;

	if (!priv->enable_json_state)
		return TRUE;

	devices = fu_engine_get_devices (priv->engine, error);
	if (devices == NULL)
		return FALSE;
	fwupd_device_array_ensure_parents (devices);

	/* create header */
	builder = json_builder_new ();
	json_builder_begin_object (builder);

	/* add each device */
	json_builder_set_member_name (builder, "Devices");
	json_builder_begin_array (builder);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		json_builder_begin_object (builder);
		fwupd_device_to_json (dev, builder);
		json_builder_end_object (builder);
	}
	json_builder_end_array (builder);
	json_builder_end_object (builder);

	/* export as a string */
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	state = json_generator_to_data (json_generator, NULL);
	if (state == NULL)
		return FALSE;
	dirname = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	filename = g_build_filename (dirname, "state.json", NULL);
	return g_file_set_contents (filename, state, -1, error);
}

static void
fu_util_show_plugin_warnings (FuUtilPrivate *priv)
{
	FwupdPluginFlags flags = FWUPD_PLUGIN_FLAG_NONE;
	GPtrArray *plugins;

	/* get a superset so we do not show the same message more than once */
	plugins = fu_engine_get_plugins (priv->engine);
	for (guint i = 0; i < plugins->len; i++) {
		FwupdPlugin *plugin = g_ptr_array_index (plugins, i);
		if (!fwupd_plugin_has_flag (plugin, FWUPD_PLUGIN_FLAG_USER_WARNING))
			continue;
		flags |= fwupd_plugin_get_flags (plugin);
	}

	/* never show these, they're way too generic */
	flags &= ~FWUPD_PLUGIN_FLAG_DISABLED;
	flags &= ~FWUPD_PLUGIN_FLAG_NO_HARDWARE;
	flags &= ~FWUPD_PLUGIN_FLAG_REQUIRE_HWID;

	/* print */
	for (guint i = 0; i < 64; i++) {
		FwupdPluginFlags flag = (guint64) 1 << i;
		const gchar *tmp;
		g_autofree gchar *fmt = NULL;
		g_autofree gchar *url= NULL;
		g_autoptr(GString) str = g_string_new (NULL);
		if ((flags & flag) == 0)
			continue;
		tmp = fu_util_plugin_flag_to_string ((guint64) 1 << i);
		if (tmp == NULL)
			continue;
		/* TRANSLATORS: this is a prefix on the console */
		fmt = fu_util_term_format (_("WARNING:"), FU_UTIL_TERM_COLOR_RED);
		g_string_append_printf (str, "%s %s\n", fmt, tmp);

		url = g_strdup_printf ("https://github.com/fwupd/fwupd/wiki/PluginFlag:%s",
				       fwupd_plugin_flag_to_string (flag));
		g_string_append (str, "  ");
		/* TRANSLATORS: %s is a link to a website */
		g_string_append_printf (str, _("See %s for more information."), url);
		g_string_append (str, "\n");
		g_printerr ("%s", str->str);
	}
}

static gboolean
fu_util_lock (FuUtilPrivate *priv, GError **error)
{
#ifdef HAVE_WRLCK
	struct flock lockp = {
		.l_type = F_WRLCK,
		.l_whence = SEEK_SET,
	};
	g_autofree gchar *lockdir = NULL;
	g_autofree gchar *lockfn = NULL;

	/* open file */
	lockdir = fu_common_get_path (FU_PATH_KIND_LOCKDIR);
	lockfn = g_build_filename (lockdir, "fwupdtool", NULL);
	if (!fu_common_mkdir_parent (lockfn, error))
		return FALSE;
	priv->lock_fd = g_open (lockfn, O_RDWR | O_CREAT, S_IRWXU);
	if (priv->lock_fd < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "failed to open %s",
			     lockfn);
		return FALSE;
	}

	/* write lock */
#ifdef HAVE_OFD
	if (fcntl (priv->lock_fd, F_OFD_SETLK, &lockp) < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "another instance has locked %s",
			     lockfn);
		return FALSE;
	}
#else
	if (fcntl (priv->lock_fd, F_SETLK, &lockp) < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "another instance has locked %s",
			     lockfn);
		return FALSE;
	}
#endif

	/* success */
	g_debug ("locked %s", lockfn);
#endif
	return TRUE;
}

static gboolean
fu_util_start_engine (FuUtilPrivate *priv, FuEngineLoadFlags flags, GError **error)
{
#ifdef HAVE_SYSTEMD
	g_autoptr(GError) error_local = NULL;
#endif
	if (!fu_util_lock (priv, error)) {
		/* TRANSLATORS: another fwupdtool instance is already running */
		g_prefix_error (error, "%s: ", _("Failed to lock"));
		return FALSE;
	}
#ifdef HAVE_SYSTEMD
	if (!fu_systemd_unit_stop (fu_util_get_systemd_unit (), &error_local))
		g_debug ("Failed to stop daemon: %s", error_local->message);
#endif
	if (!fu_engine_load (priv->engine, flags, error))
		return FALSE;
	if (fu_engine_get_tainted (priv->engine)) {
		g_autofree gchar *fmt = NULL;

		/* TRANSLATORS: this is a prefix on the console */
		fmt = fu_util_term_format (_("WARNING:"), FU_UTIL_TERM_COLOR_RED);
		g_printerr ("%s This tool has loaded 3rd party code and "
			    "is no longer supported by the upstream developers!\n",
			    fmt);
	}
	fu_util_show_plugin_warnings (priv);
	fu_util_show_unsupported_warn ();
	return TRUE;
}

static void
fu_util_maybe_prefix_sandbox_error (const gchar *value, GError **error)
{
	g_autofree gchar *path = g_path_get_dirname (value);
	if (!g_file_test (path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_DIR)) {
		g_prefix_error (error,
				"Unable to access %s. You may need to copy %s to %s: ",
				path, value, g_getenv ("HOME"));
	}
}

static void
fu_util_cancelled_cb (GCancellable *cancellable, gpointer user_data)
{
	FuUtilPrivate *priv = (FuUtilPrivate *) user_data;
	/* TRANSLATORS: this is when a device ctrl+c's a watch */
	g_print ("%s\n", _("Cancelled"));
	g_main_loop_quit (priv->loop);
}

static gboolean
fu_util_smbios_dump (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autofree gchar *tmp = NULL;
	g_autoptr(FuSmbios) smbios = NULL;
	if (g_strv_length (values) < 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}
	smbios = fu_smbios_new ();
	if (!fu_smbios_setup_from_file (smbios, values[0], error))
		return FALSE;
	tmp = fu_smbios_to_string (smbios);
	g_print ("%s\n", tmp);
	return TRUE;
}

#ifdef HAVE_GIO_UNIX
static gboolean
fu_util_sigint_cb (gpointer user_data)
{
	FuUtilPrivate *priv = (FuUtilPrivate *) user_data;
	g_debug ("Handling SIGINT");
	g_cancellable_cancel (priv->cancellable);
	return FALSE;
}
#endif

static void
fu_util_private_free (FuUtilPrivate *priv)
{
	if (priv->current_device != NULL)
		g_object_unref (priv->current_device);
	if (priv->engine != NULL)
		g_object_unref (priv->engine);
	if (priv->request != NULL)
		g_object_unref (priv->request);
	if (priv->main_ctx != NULL)
		g_main_context_unref (priv->main_ctx);
	if (priv->loop != NULL)
		g_main_loop_unref (priv->loop);
	if (priv->cancellable != NULL)
		g_object_unref (priv->cancellable);
	if (priv->progressbar != NULL)
		g_object_unref (priv->progressbar);
	if (priv->context != NULL)
		g_option_context_free (priv->context);
	if (priv->lock_fd != 0)
		g_close (priv->lock_fd, NULL);
	g_free (priv->current_message);
	g_free (priv);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuUtilPrivate, fu_util_private_free)
#pragma clang diagnostic pop


static void
fu_main_engine_device_added_cb (FuEngine *engine,
				FuDevice *device,
				FuUtilPrivate *priv)
{
	g_autofree gchar *tmp = fu_device_to_string (device);
	g_debug ("ADDED:\n%s", tmp);
}

static void
fu_main_engine_device_removed_cb (FuEngine *engine,
				  FuDevice *device,
				  FuUtilPrivate *priv)
{
	g_autofree gchar *tmp = fu_device_to_string (device);
	g_debug ("REMOVED:\n%s", tmp);
}

static void
fu_main_engine_status_changed_cb (FuEngine *engine,
				  FwupdStatus status,
				  FuUtilPrivate *priv)
{
	fu_progressbar_update (priv->progressbar, status, 0);
}

static void
fu_main_engine_percentage_changed_cb (FuEngine *engine,
				      guint percentage,
				      FuUtilPrivate *priv)
{
	fu_progressbar_update (priv->progressbar, FWUPD_STATUS_UNKNOWN, percentage);
}

static gboolean
fu_util_watch (FuUtilPrivate *priv, gchar **values, GError **error)
{
	if (!fu_util_start_engine (priv, FU_ENGINE_LOAD_FLAG_COLDPLUG, error))
		return FALSE;
	g_main_loop_run (priv->loop);
	return TRUE;
}

static gint
fu_util_plugin_name_sort_cb (FuPlugin **item1, FuPlugin **item2)
{
	return fu_plugin_name_compare (*item1, *item2);
}

static gboolean
fu_util_get_plugins (FuUtilPrivate *priv, gchar **values, GError **error)
{
	GPtrArray *plugins;

	/* load engine */
	if (!fu_util_start_engine (priv, FU_ENGINE_LOAD_FLAG_NONE, error))
		return FALSE;

	/* print */
	plugins = fu_engine_get_plugins (priv->engine);
	g_ptr_array_sort (plugins, (GCompareFunc) fu_util_plugin_name_sort_cb);
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		g_autofree gchar *str = fu_util_plugin_to_string (FWUPD_PLUGIN (plugin), 0);
		g_print ("%s\n", str);
	}
	if (plugins->len == 0) {
		/* TRANSLATORS: nothing found */
		g_print ("%s\n", _("No plugins found"));
	}

	return TRUE;
}

static gboolean
fu_util_filter_device (FuUtilPrivate *priv, FwupdDevice *dev)
{
	if (priv->filter_include != FWUPD_DEVICE_FLAG_NONE) {
		if (!fwupd_device_has_flag (dev, priv->filter_include))
			return FALSE;
	}
	if (priv->filter_exclude != FWUPD_DEVICE_FLAG_NONE) {
		if (fwupd_device_has_flag (dev, priv->filter_exclude))
			return FALSE;
	}
	return TRUE;
}

static gchar *
fu_util_get_tree_title (FuUtilPrivate *priv)
{
	return g_strdup (fu_engine_get_host_product (priv->engine));
}

static FuDevice *
fu_util_prompt_for_device (FuUtilPrivate *priv, GPtrArray *devices_opt, GError **error)
{
	FuDevice *dev;
	guint idx;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_filtered = NULL;

	/* get devices from daemon */
	if (devices_opt != NULL) {
		devices = g_ptr_array_ref (devices_opt);
	} else {
		devices = fu_engine_get_devices (priv->engine, error);
		if (devices == NULL)
			return NULL;
	}
	fwupd_device_array_ensure_parents (devices);

	/* filter results */
	devices_filtered = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < devices->len; i++) {
		dev = g_ptr_array_index (devices, i);
		if (!fu_util_filter_device (priv, FWUPD_DEVICE (dev)))
			continue;
		g_ptr_array_add (devices_filtered, g_object_ref (dev));
	}

	/* nothing */
	if (devices_filtered->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No supported devices");
		return NULL;
	}

	/* exactly one */
	if (devices_filtered->len == 1) {
		dev = g_ptr_array_index (devices_filtered, 0);
		/* TRANSLATORS: Device has been chosen by the daemon for the user */
		g_print ("%s: %s\n", _("Selected device"), fu_device_get_name (dev));
		return g_object_ref (dev);
	}

	/* TRANSLATORS: get interactive prompt */
	g_print ("%s\n", _("Choose a device:"));
	/* TRANSLATORS: this is to abort the interactive prompt */
	g_print ("0.\t%s\n", _("Cancel"));
	for (guint i = 0; i < devices_filtered->len; i++) {
		dev = g_ptr_array_index (devices_filtered, i);
		g_print ("%u.\t%s (%s)\n",
			 i + 1,
			 fu_device_get_id (dev),
			 fu_device_get_name (dev));
	}
	idx = fu_util_prompt_for_number (devices_filtered->len);
	if (idx == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "Request canceled");
		return NULL;
	}
	dev = g_ptr_array_index (devices_filtered, idx - 1);
	return g_object_ref (dev);
}

static FuDevice *
fu_util_get_device (FuUtilPrivate *priv, const gchar *id, GError **error)
{
	if (fwupd_guid_is_valid (id)) {
		g_autoptr(GPtrArray) devices = NULL;
		devices = fu_engine_get_devices_by_guid (priv->engine, id, error);
		if (devices == NULL)
			return NULL;
		return fu_util_prompt_for_device (priv, devices, error);
	}

	/* did this look like a GUID? */
	for (guint i = 0; id[i] != '\0'; i++) {
		if (id[i] == '-') {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_ARGS,
					     "Invalid arguments");
			return NULL;
		}
	}
	return fu_engine_get_device (priv->engine, id, error);
}

static gboolean
fu_util_get_updates (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = NULL;
	gboolean no_updates_header = FALSE;
	gboolean latest_header = FALSE;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;
	title = fu_util_get_tree_title (priv);

	/* parse arguments */
	if (g_strv_length (values) == 0) {
		devices = fu_engine_get_devices (priv->engine, error);
		if (devices == NULL)
			return FALSE;
	} else if (g_strv_length (values) == 1) {
		FuDevice *device;
		device = fu_util_get_device (priv, values[0], error);
		if (device == NULL)
			return FALSE;
		devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
		g_ptr_array_add (devices, device);
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	fwupd_device_array_ensure_parents (devices);
	g_ptr_array_sort (devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;
		GNode *child;

		/* not going to have results, so save a engine round-trip */
		if (!fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE))
			continue;
		if (!fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED)) {
			if (!no_updates_header) {
				/* TRANSLATORS: message letting the user know no device upgrade available due to missing on LVFS */
				g_printerr ("%s\n", _("Devices with no available firmware updates: "));
				no_updates_header = TRUE;
			}
			g_printerr (" • %s\n", fwupd_device_get_name (dev));
			continue;
		}
		if (!fu_util_filter_device (priv, dev))
			continue;

		/* get the releases for this device and filter for validity */
		rels = fu_engine_get_upgrades (priv->engine,
					       priv->request,
					       fwupd_device_get_id (dev),
					       &error_local);
		if (rels == NULL) {
			if (!latest_header) {
				/* TRANSLATORS: message letting the user know no device upgrade available */
				g_printerr ("%s\n", _("Devices with the latest available firmware version:"));
				latest_header = TRUE;
			}
			g_printerr (" • %s\n", fwupd_device_get_name (dev));
			/* discard the actual reason from user, but leave for debugging */
			g_debug ("%s", error_local->message);
			continue;
		}
		child = g_node_append_data (root, dev);

		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel = g_ptr_array_index (rels, j);
			g_node_append_data (child, g_object_ref (rel));
		}
	}
	/* save the device state for other applications to see */
	if (!fu_util_save_current_state (priv, error))
		return FALSE;

	/* updates */
	if (g_node_n_nodes (root, G_TRAVERSE_ALL) <= 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No updates available for remaining devices");
		return FALSE;
	}

	fu_util_print_tree (root, title);
	return TRUE;
}

static gboolean
fu_util_get_details (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) array = NULL;
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = NULL;
	gint fd;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;
	title = fu_util_get_tree_title (priv);

	/* check args */
	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* implied, important for get-details on a device not in your system */
	priv->show_all = TRUE;

	/* open file */
	fd = open (values[0], O_RDONLY);
	if (fd < 0) {
		fu_util_maybe_prefix_sandbox_error (values[0], error);
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "failed to open %s",
			     values[0]);
		return FALSE;
	}
	array = fu_engine_get_details (priv->engine, priv->request, fd, error);
	close (fd);

	if (array == NULL)
		return FALSE;
	for (guint i = 0; i < array->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (array, i);
		FwupdRelease *rel;
		GNode *child;
		if (!fu_util_filter_device (priv, dev))
			continue;
		child = g_node_append_data (root, dev);
		rel = fwupd_device_get_release_default (dev);
		if (rel != NULL)
			g_node_append_data (child, rel);

	}
	fu_util_print_tree (root, title);

	return TRUE;
}

static gboolean
fu_util_get_device_flags (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GString) str = g_string_new (NULL);

	for (FwupdDeviceFlags i = FWUPD_DEVICE_FLAG_INTERNAL; i < FWUPD_DEVICE_FLAG_UNKNOWN; i<<=1) {
		const gchar *tmp = fwupd_device_flag_to_string (i);
		if (tmp == NULL)
			break;
		if (i != FWUPD_DEVICE_FLAG_INTERNAL)
			g_string_append (str, " ");
		g_string_append (str, tmp);
		g_string_append (str, " ~");
		g_string_append (str, tmp);
	}
	g_print ("%s\n", str->str);

	return TRUE;
}

static void
fu_util_build_device_tree (FuUtilPrivate *priv, GNode *root, GPtrArray *devs, FuDevice *dev)
{
	for (guint i = 0; i < devs->len; i++) {
		FuDevice *dev_tmp = g_ptr_array_index (devs, i);
		if (!fu_util_filter_device (priv, FWUPD_DEVICE (dev_tmp)))
			continue;
		if (!priv->show_all &&
		    !fu_util_is_interesting_device (FWUPD_DEVICE (dev_tmp)))
			continue;
		if (fu_device_get_parent (dev_tmp) == dev) {
			GNode *child = g_node_append_data (root, dev_tmp);
			fu_util_build_device_tree (priv, child, devs, dev_tmp);
		}
	}
}

static gboolean
fu_util_get_devices (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = NULL;
	g_autoptr(GPtrArray) devs = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;
	title = fu_util_get_tree_title (priv);

	/* get devices and build tree */
	devs = fu_engine_get_devices (priv->engine, error);
	if (devs == NULL)
		return FALSE;
	if (devs->len > 0) {
		fwupd_device_array_ensure_parents (devs);
		fu_util_build_device_tree (priv, root, devs, NULL);
	}

	/* print */
	if (g_node_n_children (root) == 0) {
		/* TRANSLATORS: nothing attached that can be upgraded */
		g_print ("%s\n", _("No hardware detected with firmware update capability"));
		return TRUE;
	}
	fu_util_print_tree (root, title);

	/* save the device state for other applications to see */
	return fu_util_save_current_state (priv, error);
}

static void
fu_util_update_device_changed_cb (FwupdClient *client,
				  FwupdDevice *device,
				  FuUtilPrivate *priv)
{
	g_autofree gchar *str = NULL;

	/* allowed to set whenever the device has changed */
	if (fwupd_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN))
		priv->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
	if (fwupd_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT))
		priv->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;

	/* same as last time, so ignore */
	if (priv->current_device != NULL &&
	    fwupd_device_compare (priv->current_device, device) == 0)
		return;

	/* ignore indirect devices that might have changed */
	if (fwupd_device_get_status (device) == FWUPD_STATUS_IDLE ||
	    fwupd_device_get_status (device) == FWUPD_STATUS_UNKNOWN) {
		g_debug ("ignoring %s with status %s",
			 fwupd_device_get_name (device),
			 fwupd_status_to_string (fwupd_device_get_status (device)));
		return;
	}

	/* show message in progressbar */
	if (priv->current_operation == FU_UTIL_OPERATION_UPDATE) {
		/* TRANSLATORS: %1 is a device name */
		str = g_strdup_printf (_("Updating %s…"),
				       fwupd_device_get_name (device));
		fu_progressbar_set_title (priv->progressbar, str);
	} else if (priv->current_operation == FU_UTIL_OPERATION_INSTALL) {
		/* TRANSLATORS: %1 is a device name  */
		str = g_strdup_printf (_("Installing on %s…"),
				       fwupd_device_get_name (device));
		fu_progressbar_set_title (priv->progressbar, str);
	} else if (priv->current_operation == FU_UTIL_OPERATION_READ) {
		/* TRANSLATORS: %1 is a device name  */
		str = g_strdup_printf (_("Reading from %s…"),
				       fwupd_device_get_name (device));
		fu_progressbar_set_title (priv->progressbar, str);
	} else {
		g_warning ("no FuUtilOperation set");
	}
	g_set_object (&priv->current_device, device);

	if (priv->current_message == NULL) {
		const gchar *tmp = fwupd_device_get_update_message (priv->current_device);
		if (tmp != NULL)
			priv->current_message = g_strdup (tmp);
	}
}

static void
fu_util_display_current_message (FuUtilPrivate *priv)
{
	if (priv->current_message == NULL)
		return;
	g_print ("%s\n", priv->current_message);
	g_clear_pointer (&priv->current_message, g_free);
}

static gboolean
fu_util_install_blob (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GBytes) blob_fw = NULL;

	/* invalid args */
	if (g_strv_length (values) == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* parse blob */
	blob_fw = fu_common_get_contents_bytes (values[0], error);
	if (blob_fw == NULL) {
		fu_util_maybe_prefix_sandbox_error (values[0], error);
		return FALSE;
	}

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) >= 2) {
		device = fu_util_get_device (priv, values[1], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device (priv, NULL, error);
		if (device == NULL)
			return FALSE;
	}

	priv->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect (priv->engine, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);

	/* write bare firmware */
	if (priv->prepare_blob) {
		g_autoptr(GPtrArray) devices = NULL;
		devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
		g_ptr_array_add (devices, g_object_ref (device));
		if (!fu_engine_composite_prepare (priv->engine, devices, error)) {
			g_prefix_error (error, "failed to prepare composite action: ");
			return FALSE;
		}
	}
	priv->flags |= FWUPD_INSTALL_FLAG_NO_HISTORY;
	if (!fu_engine_install_blob (priv->engine, device, blob_fw,
				     priv->flags,
				     fu_engine_request_get_feature_flags (priv->request),
				     error))
		return FALSE;
	if (priv->cleanup_blob) {
		g_autoptr(FuDevice) device_new = NULL;
		g_autoptr(GError) error_local = NULL;

		/* get the possibly new device from the old ID */
		device_new = fu_util_get_device (priv,
						 fu_device_get_id (device),
						 &error_local);
		if (device_new == NULL) {
			g_debug ("failed to find new device: %s",
				 error_local->message);
		} else {
			g_autoptr(GPtrArray) devices_new = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
			g_ptr_array_add (devices_new, g_steal_pointer (&device_new));
			if (!fu_engine_composite_cleanup (priv->engine, devices_new, error)) {
				g_prefix_error (error, "failed to cleanup composite action: ");
				return FALSE;
			}
		}
	}

	fu_util_display_current_message (priv);

	/* success */
	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_firmware_sign (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuCabinet) cabinet = fu_cabinet_new ();
	g_autoptr(GBytes) archive_blob_new = NULL;
	g_autoptr(GBytes) archive_blob_old = NULL;
	g_autoptr(GBytes) cert = NULL;
	g_autoptr(GBytes) privkey = NULL;

	/* invalid args */
	if (g_strv_length (values) != 3) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments, expected firmware.cab "
				     "certificate.pem privatekey.pfx");
		return FALSE;
	}

	/* load arguments */
	archive_blob_old = fu_common_get_contents_bytes (values[0], error);
	if (archive_blob_old == NULL)
		return FALSE;
	cert = fu_common_get_contents_bytes (values[1], error);
	if (cert == NULL)
		return FALSE;
	privkey = fu_common_get_contents_bytes (values[2], error);
	if (privkey == NULL)
		return FALSE;

	/* load, sign, export */
	if (!fu_cabinet_parse (cabinet, archive_blob_old,
			       FU_CABINET_PARSE_FLAG_NONE,
			       error))
		return FALSE;
	if (!fu_cabinet_sign (cabinet, cert, privkey,
			      FU_CABINET_SIGN_FLAG_NONE,
			      error))
		return FALSE;
	archive_blob_new = fu_cabinet_export (cabinet,
					      FU_CABINET_EXPORT_FLAG_NONE,
					      error);
	if (archive_blob_new == NULL)
		return FALSE;
	return fu_common_set_contents_bytes (values[0], archive_blob_new, error);
}

static gboolean
fu_util_firmware_dump (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(GBytes) blob_empty = g_bytes_new (NULL, 0);
	g_autoptr(GBytes) blob_fw = NULL;

	/* invalid args */
	if (g_strv_length (values) == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* file already exists */
	if ((priv->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    g_file_test (values[0], G_FILE_TEST_EXISTS)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Filename already exists");
		return FALSE;
	}

	/* write a zero length file to ensure the destination is writable to
	 * avoid failing at the end of a potentially lengthy operation */
	if (!fu_common_set_contents_bytes (values[0], blob_empty, error))
		return FALSE;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) >= 2) {
		device = fu_util_get_device (priv, values[1], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device (priv, NULL, error);
		if (device == NULL)
			return FALSE;
	}
	priv->current_operation = FU_UTIL_OPERATION_READ;
	g_signal_connect (priv->engine, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);

	/* dump firmware */
	blob_fw = fu_engine_firmware_dump (priv->engine, device, priv->flags, error);
	if (blob_fw == NULL)
		return FALSE;
	return fu_common_set_contents_bytes (values[0], blob_fw, error);
}

static gint
fu_util_install_task_sort_cb (gconstpointer a, gconstpointer b)
{
	FuInstallTask *task1 = *((FuInstallTask **) a);
	FuInstallTask *task2 = *((FuInstallTask **) b);
	return fu_install_task_compare (task1, task2);
}

static gboolean
fu_util_download_out_of_process (const gchar *uri, const gchar *fn, GError **error)
{
	const gchar *argv[][5] = { { "wget", uri, "-O", fn, NULL },
				   { "curl", uri, "--output", fn, NULL },
				   { NULL } };
	for (guint i = 0; argv[i][0] != NULL; i++) {
		g_autoptr(GError) error_local = NULL;
		g_autofree gchar *fn_tmp = NULL;
		fn_tmp = fu_common_find_program_in_path (argv[i][0], &error_local);
		if (fn_tmp == NULL) {
			g_debug ("%s", error_local->message);
			continue;
		}
		return fu_common_spawn_sync (argv[i], NULL, NULL, 0, NULL, error);
	}
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_FOUND,
			     "no supported out-of-process downloaders found");
	return FALSE;
}

static gchar *
fu_util_download_if_required (FuUtilPrivate *priv, const gchar *perhapsfn, GError **error)
{
	g_autofree gchar *filename = NULL;

	/* a local file */
	if (g_file_test (perhapsfn, G_FILE_TEST_EXISTS))
		return g_strdup (perhapsfn);
	if (!fu_util_is_url (perhapsfn))
		return g_strdup (perhapsfn);

	/* download the firmware to a cachedir */
	filename = fu_util_get_user_cache_path (perhapsfn);
	if (!fu_common_mkdir_parent (filename, error))
		return NULL;
	if (!fu_util_download_out_of_process (perhapsfn, filename, error))
		return NULL;
	return g_steal_pointer (&filename);
}

static gboolean
fu_util_install (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autofree gchar *filename = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(GPtrArray) devices_possible = NULL;
	g_autoptr(GPtrArray) errors = NULL;
	g_autoptr(GPtrArray) install_tasks = NULL;
	g_autoptr(XbSilo) silo = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* handle both forms */
	if (g_strv_length (values) == 1) {
		devices_possible = fu_engine_get_devices (priv->engine, error);
		if (devices_possible == NULL)
			return FALSE;
		fwupd_device_array_ensure_parents (devices_possible);
	} else if (g_strv_length (values) == 2) {
		FuDevice *device = fu_util_get_device (priv, values[1], error);
		if (device == NULL)
			return FALSE;
		devices_possible = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
		g_ptr_array_add (devices_possible, device);
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* download if required */
	filename = fu_util_download_if_required (priv, values[0], error);
	if (filename == NULL)
		return FALSE;

	/* parse silo */
	blob_cab = fu_common_get_contents_bytes (filename, error);
	if (blob_cab == NULL) {
		fu_util_maybe_prefix_sandbox_error (filename, error);
		return FALSE;
	}
	silo = fu_engine_get_silo_from_blob (priv->engine, blob_cab, error);
	if (silo == NULL)
		return FALSE;
	components = xb_silo_query (silo, "components/component", 0, error);
	if (components == NULL)
		return FALSE;

	/* for each component in the silo */
	errors = g_ptr_array_new_with_free_func ((GDestroyNotify) g_error_free);
	install_tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < components->len; i++) {
		XbNode *component = g_ptr_array_index (components, i);

		/* do any devices pass the requirements */
		for (guint j = 0; j < devices_possible->len; j++) {
			FuDevice *device = g_ptr_array_index (devices_possible, j);
			g_autoptr(FuInstallTask) task = NULL;
			g_autoptr(GError) error_local = NULL;

			/* is this component valid for the device */
			task = fu_install_task_new (device, component);
			if (!fu_engine_check_requirements (priv->engine,
							   priv->request,
							   task,
							   priv->flags | FWUPD_INSTALL_FLAG_FORCE,
							   &error_local)) {
				g_debug ("first pass requirement on %s:%s failed: %s",
					 fu_device_get_id (device),
					 xb_node_query_text (component, "id", NULL),
					 error_local->message);
				g_ptr_array_add (errors, g_steal_pointer (&error_local));
				continue;
			}

			/* make a second pass using possibly updated version format now */
			fu_engine_md_refresh_device_from_component (priv->engine, device, component);
			if (!fu_engine_check_requirements (priv->engine,
							   priv->request,
							   task,
							   priv->flags,
							   &error_local)) {
				g_debug ("second pass requirement on %s:%s failed: %s",
					 fu_device_get_id (device),
					 xb_node_query_text (component, "id", NULL),
					 error_local->message);
				g_ptr_array_add (errors, g_steal_pointer (&error_local));
				continue;
			}

			/* if component should have an update message from CAB */
			fu_device_incorporate_from_component (device, component);

			/* success */
			g_ptr_array_add (install_tasks, g_steal_pointer (&task));
		}
	}

	/* order the install tasks by the device priority */
	g_ptr_array_sort (install_tasks, fu_util_install_task_sort_cb);

	/* nothing suitable */
	if (install_tasks->len == 0) {
		GError *error_tmp = fu_common_error_array_get_best (errors);
		g_propagate_error (error, error_tmp);
		return FALSE;
	}

	priv->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect (priv->engine, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);

	/* install all the tasks */
	if (!fu_engine_install_tasks (priv->engine,
				      priv->request,
				      install_tasks,
				      blob_cab,
				      priv->flags,
				      error))
		return FALSE;

	fu_util_display_current_message (priv);

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	/* save the device state for other applications to see */
	if (!fu_util_save_current_state (priv, error))
		return FALSE;

	/* success */
	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_install_release (FuUtilPrivate *priv, FwupdRelease *rel, GError **error)
{
	FwupdRemote *remote;
	GPtrArray *locations;
	const gchar *remote_id;
	const gchar *uri_tmp;
	g_auto(GStrv) argv = NULL;

	/* get the default release only until other parts of fwupd can cope */
	locations = fwupd_release_get_locations (rel);
	if (locations->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "release missing URI");
		return FALSE;
	}
	uri_tmp = g_ptr_array_index (locations, 0);
	remote_id = fwupd_release_get_remote_id (rel);
	if (remote_id == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_FILE,
			     "failed to find remote for %s",
			     uri_tmp);
		return FALSE;
	}

	remote = fu_engine_get_remote_by_id (priv->engine,
					     remote_id,
					     error);
	if (remote == NULL)
		return FALSE;

	argv = g_new0 (gchar *, 2);
	/* local remotes may have the firmware already */
	if (fwupd_remote_get_kind (remote) == FWUPD_REMOTE_KIND_LOCAL &&
	    !fu_util_is_url (uri_tmp)) {
		const gchar *fn_cache = fwupd_remote_get_filename_cache (remote);
		g_autofree gchar *path = g_path_get_dirname (fn_cache);
		argv[0] = g_build_filename (path, uri_tmp, NULL);
	} else if (fwupd_remote_get_kind (remote) == FWUPD_REMOTE_KIND_DIRECTORY) {
		argv[0] = g_strdup (uri_tmp + 7);
	/* web remote, fu_util_install will download file */
	} else {
		argv[0] = fwupd_remote_build_firmware_uri (remote, uri_tmp, error);
	}
	return fu_util_install (priv, argv, error);
}

static gboolean
fu_util_update_all (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	gboolean no_updates_header = FALSE;
	gboolean latest_header = FALSE;

	devices = fu_engine_get_devices (priv->engine, error);
	if (devices == NULL)
		return FALSE;
	fwupd_device_array_ensure_parents (devices);
	g_ptr_array_sort (devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel;
		const gchar *device_id;
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;

		if (!fu_util_is_interesting_device (dev))
			continue;
		/* only show stuff that has metadata available */
		if (!fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE))
			continue;
		if (!fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED)) {
			if (!no_updates_header) {
				/* TRANSLATORS: message letting the user know no device upgrade available due to missing on LVFS */
				g_printerr ("%s\n", _("Devices with no available firmware updates: "));
				no_updates_header = TRUE;
			}
			g_printerr (" • %s\n", fwupd_device_get_name (dev));
			continue;
		}
		if (!fu_util_filter_device (priv, dev))
			continue;

		device_id = fu_device_get_id (dev);
		rels = fu_engine_get_upgrades (priv->engine,
					       priv->request,
					       device_id,
					       &error_local);
		if (rels == NULL) {
			if (!latest_header) {
				/* TRANSLATORS: message letting the user know no device upgrade available */
				g_printerr ("%s\n", _("Devices with the latest available firmware version:"));
				latest_header = TRUE;
			}
			g_printerr (" • %s\n", fwupd_device_get_name (dev));
			/* discard the actual reason from user, but leave for debugging */
			g_debug ("%s", error_local->message);
			continue;
		}

		rel = g_ptr_array_index (rels, 0);
		if (!priv->no_safety_check) {
			if (!fu_util_prompt_warning (dev,
						     rel,
						     fu_util_get_tree_title (priv),
						     error))
				return FALSE;
		}

		if (!fu_util_install_release (priv, rel, &error_local)) {
			g_printerr ("%s\n", error_local->message);
			continue;
		}
		fu_util_display_current_message (priv);
	}
	return TRUE;
}

static gboolean
fu_util_update_by_id (FuUtilPrivate *priv, const gchar *id, GError **error)
{
	FwupdRelease *rel;
	g_autoptr(FuDevice) dev = NULL;
	g_autoptr(GPtrArray) rels = NULL;

	/* do not allow a partial device-id, lookup GUIDs */
	dev = fu_util_get_device (priv, id, error);
	if (dev == NULL)
		return FALSE;

	/* get the releases for this device and filter for validity */
	rels = fu_engine_get_upgrades (priv->engine,
				       priv->request,
				       fu_device_get_id (dev),
				       error);
	if (rels == NULL)
		return FALSE;
	rel = g_ptr_array_index (rels, 0);
	if (!fu_util_install_release (priv, rel, error))
		return FALSE;
	fu_util_display_current_message (priv);

	return TRUE;
}

static gboolean
fu_util_update (FuUtilPrivate *priv, gchar **values, GError **error)
{
	if (priv->flags & FWUPD_INSTALL_FLAG_ALLOW_OLDER) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "--allow-older is not supported for this command");
		return FALSE;
	}

	if (priv->flags & FWUPD_INSTALL_FLAG_ALLOW_REINSTALL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "--allow-reinstall is not supported for this command");
		return FALSE;
	}

	if (g_strv_length (values) > 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	priv->current_operation = FU_UTIL_OPERATION_UPDATE;
	g_signal_connect (priv->engine, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);

	if (g_strv_length (values) == 1) {
		if (!fu_util_update_by_id (priv, values[0], error))
			return FALSE;
	} else {
		if (!fu_util_update_all (priv, error))
			return FALSE;
	}

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	/* save the device state for other applications to see */
	if (!fu_util_save_current_state (priv, error))
		return FALSE;

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_reinstall (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(FuDevice) dev = NULL;

	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	dev = fu_util_get_device (priv, values[0], error);
	if (dev == NULL)
		return FALSE;

	/* try to lookup/match release from client */
	rels = fu_engine_get_releases_for_device (priv->engine,
						  priv->request,
						  dev,
						  error);
	if (rels == NULL)
		return FALSE;

	for (guint j = 0; j < rels->len; j++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (rels, j);
		if (fu_common_vercmp_full (fwupd_release_get_version (rel_tmp),
					   fu_device_get_version (dev),
					   fu_device_get_version_format (dev)) == 0) {
			rel = g_object_ref (rel_tmp);
			break;
		}
	}
	if (rel == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Unable to locate release for %s version %s",
			     fu_device_get_name (dev),
			     fu_device_get_version (dev));
		return FALSE;
	}

	/* update the console if composite devices are also updated */
	priv->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect (priv->engine, "device-changed",
			G_CALLBACK (fu_util_update_device_changed_cb), priv);
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	if (!fu_util_install_release (priv, rel, error))
		return FALSE;
	fu_util_display_current_message (priv);

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	/* save the device state for other applications to see */
	if (!fu_util_save_current_state (priv, error))
		return FALSE;

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_detach (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) >= 1) {
		device = fu_util_get_device (priv, values[0], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device (priv, NULL, error);
		if (device == NULL)
			return FALSE;
	}

	/* run vfunc */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_detach (device, error);
}

static gboolean
fu_util_unbind_driver (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) == 1) {
		device = fu_util_get_device (priv, values[0], error);
	} else {
		device = fu_util_prompt_for_device (priv, NULL, error);
	}
	if (device == NULL)
		return FALSE;

	/* run vfunc */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_unbind_driver (device, error);
}

static gboolean
fu_util_bind_driver (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) == 3) {
		device = fu_util_get_device (priv, values[2], error);
		if (device == NULL)
			return FALSE;
	} else if (g_strv_length (values) == 2) {
		device = fu_util_prompt_for_device (priv, NULL, error);
		if (device == NULL)
			return FALSE;
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* run vfunc */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_bind_driver (device, values[0], values[1], error);
}

static gboolean
fu_util_attach (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) >= 1) {
		device = fu_util_get_device (priv, values[0], error);
		if (device == NULL)
			return FALSE;
	} else {
		device = fu_util_prompt_for_device (priv, NULL, error);
		if (device == NULL)
			return FALSE;
	}

	/* run vfunc */
	locker = fu_device_locker_new (device, error);
	if (locker == NULL)
		return FALSE;
	return fu_device_attach (device, error);
}

static gboolean
fu_util_check_activation_needed (FuUtilPrivate *priv, GError **error)
{
	gboolean has_pending = FALSE;
	g_autoptr(FuHistory) history = fu_history_new ();
	g_autoptr(GPtrArray) devices = fu_history_get_devices (history, error);
	if (devices == NULL)
		return FALSE;

	/* only start up the plugins needed */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *dev = g_ptr_array_index (devices, i);
		if (fu_device_has_flag (dev, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
			fu_engine_add_plugin_filter (priv->engine,
						     fu_device_get_plugin (dev));
			has_pending = TRUE;
		}
	}

	if (!has_pending) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No devices to activate");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_activate (FuUtilPrivate *priv, gchar **values, GError **error)
{
	gboolean has_pending = FALSE;
	g_autoptr(GPtrArray) devices = NULL;

	/* check the history database before starting the daemon */
	if (!fu_util_check_activation_needed (priv, error))
		return FALSE;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_READONLY |
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_REMOTES |
				   FU_ENGINE_LOAD_FLAG_HWINFO,
				   error))
		return FALSE;

	/* parse arguments */
	if (g_strv_length (values) == 0) {
		devices = fu_engine_get_devices (priv->engine, error);
		if (devices == NULL)
			return FALSE;
	} else if (g_strv_length (values) == 1) {
		FuDevice *device;
		device = fu_util_get_device (priv, values[0], error);
		if (device == NULL)
			return FALSE;
		devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
		g_ptr_array_add (devices, device);
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* activate anything with _NEEDS_ACTIVATION */
	/* order by device priority */
	g_ptr_array_sort (devices, fu_util_device_order_sort_cb);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index (devices, i);
		if (!fu_util_filter_device (priv, FWUPD_DEVICE (device)))
			continue;
		if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION))
			continue;
		has_pending = TRUE;
		/* TRANSLATORS: shown when shutting down to switch to the new version */
		g_print ("%s %s…\n", _("Activating firmware update"), fu_device_get_name (device));
		if (!fu_engine_activate (priv->engine, fu_device_get_id (device), error))
			return FALSE;
	}

	if (!has_pending) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No devices to activate");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_export_hwids (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuHwids) hwids = fu_hwids_new ();
	g_autoptr(FuSmbios) smbios = fu_smbios_new ();
	g_autoptr(GKeyFile) kf = g_key_file_new ();
	g_autoptr(GPtrArray) hwid_keys = NULL;

	/* check args */
	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments, expected HWIDS-FILE");
		return FALSE;
	}

	/* setup default hwids */
	if (!fu_smbios_setup (smbios, error))
		return FALSE;
	if (!fu_hwids_setup (hwids, smbios, error))
		return FALSE;

	/* save all keys */
	hwid_keys = fu_hwids_get_keys (hwids);
	for (guint i = 0; i < hwid_keys->len; i++) {
		const gchar *hwid_key = g_ptr_array_index (hwid_keys, i);
		const gchar *value = fu_hwids_get_value (hwids, hwid_key);
		g_key_file_set_string (kf, "HwIds", hwid_key, value);
	}

	/* success */
	return g_key_file_save_to_file (kf, values[0], error);
}

static gboolean
fu_util_hwids (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuSmbios) smbios = NULL;
	g_autoptr(FuHwids) hwids = fu_hwids_new ();
	g_autoptr(GPtrArray) hwid_keys = fu_hwids_get_keys (hwids);

	/* read DMI data */
	if (g_strv_length (values) == 0) {
		smbios = fu_smbios_new ();
		if (!fu_smbios_setup (smbios, error))
			return FALSE;
	} else if (g_strv_length (values) == 1) {
		/* a keyfile with overrides */
		g_autoptr(GKeyFile) kf = g_key_file_new ();
		if (g_key_file_load_from_file (kf, values[0], G_KEY_FILE_NONE, NULL)) {
			for (guint i = 0; i < hwid_keys->len; i++) {
				const gchar *hwid_key = g_ptr_array_index (hwid_keys, i);
				g_autofree gchar *tmp = NULL;
				tmp = g_key_file_get_string (kf, "HwIds", hwid_key, NULL);
				fu_hwids_add_smbios_override (hwids, hwid_key, tmp);
			}
		/* a DMI blob */
		} else {
			smbios = fu_smbios_new ();
			if (!fu_smbios_setup_from_file (smbios, values[0], error))
				return FALSE;
		}
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}
	if (!fu_hwids_setup (hwids, smbios, error))
		return FALSE;

	/* show debug output */
	g_print ("Computer Information\n");
	g_print ("--------------------\n");
	for (guint i = 0; i < hwid_keys->len; i++) {
		const gchar *hwid_key = g_ptr_array_index (hwid_keys, i);
		const gchar *value = fu_hwids_get_value (hwids, hwid_key);
		if (value == NULL)
			continue;
		if (g_strcmp0 (hwid_key, FU_HWIDS_KEY_BIOS_MAJOR_RELEASE) == 0 ||
		    g_strcmp0 (hwid_key, FU_HWIDS_KEY_BIOS_MINOR_RELEASE) == 0) {
			guint64 val = g_ascii_strtoull (value, NULL, 16);
			g_print ("%s: %" G_GUINT64_FORMAT "\n", hwid_key, val);
		} else {
			g_print ("%s: %s\n", hwid_key, value);
		}
	}

	/* show GUIDs */
	g_print ("\nHardware IDs\n");
	g_print ("------------\n");
	for (guint i = 0; i < 15; i++) {
		const gchar *keys = NULL;
		g_autofree gchar *guid = NULL;
		g_autofree gchar *key = NULL;
		g_autofree gchar *keys_str = NULL;
		g_auto(GStrv) keysv = NULL;
		g_autoptr(GError) error_local = NULL;

		/* get the GUID */
		key = g_strdup_printf ("HardwareID-%u", i);
		keys = fu_hwids_get_replace_keys (hwids, key);
		guid = fu_hwids_get_guid (hwids, key, &error_local);
		if (guid == NULL) {
			g_print ("%s\n", error_local->message);
			continue;
		}

		/* show what makes up the GUID */
		keysv = g_strsplit (keys, "&", -1);
		keys_str = g_strjoinv (" + ", keysv);
		g_print ("{%s}   <- %s\n", guid, keys_str);
	}

	return TRUE;
}

static gboolean
fu_util_firmware_builder (FuUtilPrivate *priv, gchar **values, GError **error)
{
	const gchar *script_fn = "startup.sh";
	const gchar *output_fn = "firmware.bin";
	g_autoptr(GBytes) archive_blob = NULL;
	g_autoptr(GBytes) firmware_blob = NULL;
	if (g_strv_length (values) < 2) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}
	archive_blob = fu_common_get_contents_bytes (values[0], error);
	if (archive_blob == NULL)
		return FALSE;
	if (g_strv_length (values) > 2)
		script_fn = values[2];
	if (g_strv_length (values) > 3)
		output_fn = values[3];
	firmware_blob = fu_common_firmware_builder (archive_blob, script_fn, output_fn, error);
	if (firmware_blob == NULL)
		return FALSE;
	return fu_common_set_contents_bytes (values[1], firmware_blob, error);
}

static gboolean
fu_util_self_sign (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autofree gchar *sig = NULL;

	/* check args */
	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: value expected");
		return FALSE;
	}

	/* start engine */
	if (!fu_util_start_engine (priv, FU_ENGINE_LOAD_FLAG_NONE, error))
		return FALSE;
	sig = fu_engine_self_sign (priv->engine, values[0],
				   JCAT_SIGN_FLAG_ADD_TIMESTAMP |
				   JCAT_SIGN_FLAG_ADD_CERT, error);
	if (sig == NULL)
		return FALSE;
	g_print ("%s\n", sig);
	return TRUE;
}

static void
fu_util_device_added_cb (FwupdClient *client,
			 FwupdDevice *device,
			 gpointer user_data)
{
	g_autofree gchar *tmp = fu_util_device_to_string (device, 0);
	/* TRANSLATORS: this is when a device is hotplugged */
	g_print ("%s\n%s", _("Device added:"), tmp);
}

static void
fu_util_device_removed_cb (FwupdClient *client,
			   FwupdDevice *device,
			   gpointer user_data)
{
	g_autofree gchar *tmp = fu_util_device_to_string (device, 0);
	/* TRANSLATORS: this is when a device is hotplugged */
	g_print ("%s\n%s", _("Device removed:"), tmp);
}

static void
fu_util_device_changed_cb (FwupdClient *client,
			   FwupdDevice *device,
			   gpointer user_data)
{
	g_autofree gchar *tmp = fu_util_device_to_string (device, 0);
	/* TRANSLATORS: this is when a device has been updated */
	g_print ("%s\n%s", _("Device changed:"), tmp);
}

static void
fu_util_changed_cb (FwupdClient *client, gpointer user_data)
{
	/* TRANSLATORS: this is when the daemon state changes */
	g_print ("%s\n", _("Changed"));
}

static gboolean
fu_util_monitor (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdClient) client = fwupd_client_new ();
	fwupd_client_set_main_context (client, priv->main_ctx);

	/* get all the devices */
	if (!fwupd_client_connect (client, priv->cancellable, error))
		return FALSE;

	/* watch for any hotplugged device */
	g_signal_connect (client, "changed",
			  G_CALLBACK (fu_util_changed_cb), priv);
	g_signal_connect (client, "device-added",
			  G_CALLBACK (fu_util_device_added_cb), priv);
	g_signal_connect (client, "device-removed",
			  G_CALLBACK (fu_util_device_removed_cb), priv);
	g_signal_connect (client, "device-changed",
			  G_CALLBACK (fu_util_device_changed_cb), priv);
	g_signal_connect (priv->cancellable, "cancelled",
			  G_CALLBACK (fu_util_cancelled_cb), priv);
	g_main_loop_run (priv->loop);
	return TRUE;
}

static gboolean
fu_util_get_firmware_types (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) firmware_types = NULL;

	/* load engine */
	if (!fu_engine_load (priv->engine, FU_ENGINE_LOAD_FLAG_READONLY, error))
		return FALSE;

	firmware_types = fu_context_get_firmware_gtype_ids (fu_engine_get_context (priv->engine));
	for (guint i = 0; i < firmware_types->len; i++) {
		const gchar *id = g_ptr_array_index (firmware_types, i);
		g_print ("%s\n", id);
	}
	if (firmware_types->len == 0) {
		/* TRANSLATORS: nothing found */
		g_print ("%s\n", _("No firmware IDs found"));
		return TRUE;
	}

	return TRUE;
}

static gchar *
fu_util_prompt_for_firmware_type (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(GPtrArray) firmware_types = NULL;
	guint idx;
	firmware_types = fu_context_get_firmware_gtype_ids (fu_engine_get_context (priv->engine));

	/* TRANSLATORS: get interactive prompt */
	g_print ("%s\n", _("Choose a firmware type:"));
	/* TRANSLATORS: this is to abort the interactive prompt */
	g_print ("0.\t%s\n", _("Cancel"));
	for (guint i = 0; i < firmware_types->len; i++) {
		const gchar *id = g_ptr_array_index (firmware_types, i);
		g_print ("%u.\t%s\n", i + 1, id);
	}
	idx = fu_util_prompt_for_number (firmware_types->len);
	if (idx == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "Request canceled");
		return NULL;
	}

	return g_strdup (g_ptr_array_index (firmware_types, idx - 1));
}

static gboolean
fu_util_firmware_parse (FuUtilPrivate *priv, gchar **values, GError **error)
{
	GType gtype;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;

	/* check args */
	if (g_strv_length (values) == 0 || g_strv_length (values) > 2) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: filename required");
		return FALSE;
	}

	if (g_strv_length (values) == 2)
		firmware_type = g_strdup (values[1]);

	/* load file */
	blob = fu_common_get_contents_bytes (values[0], error);
	if (blob == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load (priv->engine, FU_ENGINE_LOAD_FLAG_READONLY, error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type == NULL)
		firmware_type = fu_util_prompt_for_firmware_type (priv, error);
	if (firmware_type == NULL)
		return FALSE;
	gtype = fu_context_get_firmware_gtype_by_id (fu_engine_get_context (priv->engine), firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "GType %s not supported", firmware_type);
		return FALSE;
	}
	firmware = g_object_new (gtype, NULL);
	if (!fu_firmware_parse (firmware, blob, priv->flags, error))
		return FALSE;
	str = fu_firmware_to_string (firmware);
	g_print ("%s", str);
	return TRUE;
}

static gboolean
fu_util_firmware_export (FuUtilPrivate *priv, gchar **values, GError **error)
{
	FuFirmwareExportFlags flags = FU_FIRMWARE_EXPORT_FLAG_NONE;
	GType gtype;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;

	/* check args */
	if (g_strv_length (values) == 0 || g_strv_length (values) > 2) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: filename required");
		return FALSE;
	}

	if (g_strv_length (values) == 2)
		firmware_type = g_strdup (values[1]);

	/* load file */
	blob = fu_common_get_contents_bytes (values[0], error);
	if (blob == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load (priv->engine, FU_ENGINE_LOAD_FLAG_READONLY, error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type == NULL)
		firmware_type = fu_util_prompt_for_firmware_type (priv, error);
	if (firmware_type == NULL)
		return FALSE;
	gtype = fu_context_get_firmware_gtype_by_id (fu_engine_get_context (priv->engine), firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "GType %s not supported", firmware_type);
		return FALSE;
	}
	firmware = g_object_new (gtype, NULL);
	if (!fu_firmware_parse (firmware, blob, priv->flags, error))
		return FALSE;
	if (priv->show_all)
		flags |= FU_FIRMWARE_EXPORT_FLAG_INCLUDE_DEBUG;
	str = fu_firmware_export_to_xml (firmware, flags, error);
	if (str == NULL)
		return FALSE;
	g_print ("%s", str);
	return TRUE;
}

static gboolean
fu_util_firmware_extract (FuUtilPrivate *priv, gchar **values, GError **error)
{
	GType gtype;
	g_autofree gchar *firmware_type = NULL;
	g_autofree gchar *str = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GPtrArray) images = NULL;

	/* check args */
	if (g_strv_length (values) == 0 || g_strv_length (values) > 2) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: filename required");
		return FALSE;
	}
	if (g_strv_length (values) == 2)
		firmware_type = g_strdup (values[1]);

	/* load file */
	blob = fu_common_get_contents_bytes (values[0], error);
	if (blob == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load (priv->engine, FU_ENGINE_LOAD_FLAG_READONLY, error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type == NULL)
		firmware_type = fu_util_prompt_for_firmware_type (priv, error);
	if (firmware_type == NULL)
		return FALSE;
	gtype = fu_context_get_firmware_gtype_by_id (fu_engine_get_context (priv->engine), firmware_type);
	if (gtype == G_TYPE_INVALID) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "GType %s not supported", firmware_type);
		return FALSE;
	}
	firmware = g_object_new (gtype, NULL);
	if (!fu_firmware_parse (firmware, blob, priv->flags, error))
		return FALSE;
	str = fu_firmware_to_string (firmware);
	g_print ("%s", str);
	images = fu_firmware_get_images (firmware);
	for (guint i = 0; i < images->len; i++) {
		FuFirmware *img = g_ptr_array_index (images, i);
		g_autofree gchar *fn = NULL;
		g_autoptr(GBytes) blob_img = NULL;

		/* get raw image without generated header, footer or crc */
		blob_img = fu_firmware_get_bytes (img, error);
		if (blob_img == NULL)
			return FALSE;
		if (g_bytes_get_size (blob_img) == 0)
			continue;

		/* use suitable filename */
		if (fu_firmware_get_filename (img) != NULL) {
			fn = g_strdup (fu_firmware_get_filename (img));
		} else if (fu_firmware_get_id (img) != NULL) {
			fn = g_strdup_printf ("id-%s.fw", fu_firmware_get_id (img));
		} else if (fu_firmware_get_idx (img) != 0x0) {
			fn = g_strdup_printf ("idx-0x%x.fw", (guint) fu_firmware_get_idx (img));
		} else {
			fn = g_strdup_printf ("img-0x%x.fw", i);
		}
		/* TRANSLATORS: decompressing images from a container firmware */
		g_print ("%s : %s\n", _("Writing file:"), fn);
		if (!fu_common_set_contents_bytes (fn, blob_img, error))
			return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_firmware_build (FuUtilPrivate *priv, gchar **values, GError **error)
{
	GType gtype = FU_TYPE_FIRMWARE;
	const gchar *tmp;
	g_autofree gchar *str = NULL;
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(FuFirmware) firmware_dst = NULL;
	g_autoptr(GBytes) blob_dst = NULL;
	g_autoptr(GBytes) blob_src = NULL;
	g_autoptr(XbBuilder) builder = xb_builder_new ();
	g_autoptr(XbBuilderSource) source = xb_builder_source_new ();
	g_autoptr(XbNode) n = NULL;
	g_autoptr(XbSilo) silo = NULL;

	/* check args */
	if (g_strv_length (values) != 2) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: filename required");
		return FALSE;
	}

	/* load file */
	blob_src = fu_common_get_contents_bytes (values[0], error);
	if (blob_src == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load (priv->engine, FU_ENGINE_LOAD_FLAG_READONLY, error))
		return FALSE;

	/* parse XML */
	if (!xb_builder_source_load_bytes (source, blob_src,
					   XB_BUILDER_SOURCE_FLAG_NONE,
					   error)) {
		g_prefix_error (error, "could not parse XML: ");
		return FALSE;
	}
	xb_builder_import_source (builder, source);
	silo = xb_builder_compile (builder, XB_BUILDER_COMPILE_FLAG_NONE, NULL, error);
	if (silo == NULL)
		return FALSE;

	/* create FuFirmware of specific GType */
	n = xb_silo_query_first (silo, "firmware", error);
	if (n == NULL)
		return FALSE;
	tmp = xb_node_get_attr (n, "gtype");
	if (tmp != NULL) {
		gtype = g_type_from_name (tmp);
		if (gtype == G_TYPE_INVALID) {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_NOT_FOUND,
				     "GType %s not registered", tmp);
			return FALSE;
		}
	}
	tmp = xb_node_get_attr (n, "id");
	if (tmp != NULL) {
		gtype = fu_context_get_firmware_gtype_by_id (fu_engine_get_context (priv->engine), tmp);
		if (gtype == G_TYPE_INVALID) {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_NOT_FOUND,
				     "GType %s not supported", tmp);
			return FALSE;
		}
	}
	firmware = g_object_new (gtype, NULL);
	if (!fu_firmware_build (firmware, n, error))
		return FALSE;

	/* write new file */
	blob_dst = fu_firmware_write (firmware, error);
	if (blob_dst == NULL)
		return FALSE;
	if (!fu_common_set_contents_bytes (values[1], blob_dst, error))
		return FALSE;

	/* show what we wrote */
	firmware_dst = g_object_new (gtype, NULL);
	if (!fu_firmware_parse (firmware_dst, blob_dst, priv->flags, error))
		return FALSE;
	str = fu_firmware_to_string (firmware_dst);
	g_print ("%s", str);

	/* success */
	return TRUE;
}

static gboolean
fu_util_firmware_convert (FuUtilPrivate *priv, gchar **values, GError **error)
{
	FuContext *ctx = fu_engine_get_context (priv->engine);
	GType gtype_dst;
	GType gtype_src;
	g_autofree gchar *firmware_type_dst = NULL;
	g_autofree gchar *firmware_type_src = NULL;
	g_autofree gchar *str_dst = NULL;
	g_autofree gchar *str_src = NULL;
	g_autoptr(FuFirmware) firmware_dst = NULL;
	g_autoptr(FuFirmware) firmware_src = NULL;
	g_autoptr(GBytes) blob_dst = NULL;
	g_autoptr(GBytes) blob_src = NULL;
	g_autoptr(GPtrArray) images = NULL;

	/* check args */
	if (g_strv_length (values) < 2 || g_strv_length (values) > 4) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: filename required");
		return FALSE;
	}

	if (g_strv_length (values) > 2)
		firmware_type_src = g_strdup (values[2]);
	if (g_strv_length (values) > 3)
		firmware_type_dst = g_strdup (values[3]);

	/* load file */
	blob_src = fu_common_get_contents_bytes (values[0], error);
	if (blob_src == NULL)
		return FALSE;

	/* load engine */
	if (!fu_engine_load (priv->engine, FU_ENGINE_LOAD_FLAG_READONLY, error))
		return FALSE;

	/* find the GType to use */
	if (firmware_type_src == NULL)
		firmware_type_src = fu_util_prompt_for_firmware_type (priv, error);
	if (firmware_type_src == NULL)
		return FALSE;
	if (firmware_type_dst == NULL)
		firmware_type_dst = fu_util_prompt_for_firmware_type (priv, error);
	if (firmware_type_dst == NULL)
		return FALSE;
	gtype_src = fu_context_get_firmware_gtype_by_id (fu_engine_get_context (priv->engine),
							 firmware_type_src);
	if (gtype_src == G_TYPE_INVALID) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "GType %s not supported", firmware_type_src);
		return FALSE;
	}
	firmware_src = g_object_new (gtype_src, NULL);
	if (!fu_firmware_parse (firmware_src, blob_src, priv->flags, error))
		return FALSE;
	gtype_dst = fu_context_get_firmware_gtype_by_id (ctx, firmware_type_dst);
	if (gtype_dst == G_TYPE_INVALID) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "GType %s not supported", firmware_type_dst);
		return FALSE;
	}
	str_src = fu_firmware_to_string (firmware_src);
	g_print ("%s", str_src);

	/* copy images */
	firmware_dst = g_object_new (gtype_dst, NULL);
	images = fu_firmware_get_images (firmware_src);
	for (guint i = 0; i < images->len; i++) {
		FuFirmware *img = g_ptr_array_index (images, i);
		fu_firmware_add_image (firmware_dst, img);
	}

	/* write new file */
	blob_dst = fu_firmware_write (firmware_dst, error);
	if (blob_dst == NULL)
		return FALSE;
	if (!fu_common_set_contents_bytes (values[1], blob_dst, error))
		return FALSE;
	str_dst = fu_firmware_to_string (firmware_dst);
	g_print ("%s", str_dst);

	/* success */
	return TRUE;
}

static gboolean
fu_util_verify_update (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autofree gchar *str = NULL;
	g_autoptr(FuDevice) dev = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* get device */
	if (g_strv_length (values) == 1) {
		dev = fu_util_get_device (priv, values[0], error);
		if (dev == NULL)
			return FALSE;
	} else {
		dev = fu_util_prompt_for_device (priv, NULL, error);
		if (dev == NULL)
			return FALSE;
	}

	/* add checksums */
	if (!fu_engine_verify_update (priv->engine, fu_device_get_id (dev), error))
		return FALSE;

	/* show checksums */
	str = fu_device_to_string (dev);
	g_print ("%s\n", str);
	return TRUE;
}

static gboolean
fu_util_get_history (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;
	title = fu_util_get_tree_title (priv);

	/* get all devices from the history database */
	devices = fu_engine_get_history (priv->engine, error);
	if (devices == NULL)
		return FALSE;

	/* show each device */
	for (guint i = 0; i < devices->len; i++) {
		g_autoptr(GPtrArray) rels = NULL;
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel;
		const gchar *remote;
		GNode *child;

		if (!fu_util_filter_device (priv, dev))
			continue;
		child = g_node_append_data (root, dev);

		rel = fwupd_device_get_release_default (dev);
		if (rel == NULL)
			continue;
		remote = fwupd_release_get_remote_id (rel);

		/* doesn't actually map to remote */
		if (remote == NULL) {
			g_node_append_data (child, rel);
			continue;
		}

		/* try to lookup releases from client */
		rels = fu_engine_get_releases (priv->engine,
					       priv->request,
					       fwupd_device_get_id (dev),
					       error);
		if (rels == NULL)
			return FALSE;

		/* map to a release in client */
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel2 = g_ptr_array_index (rels, j);
			if (g_strcmp0 (remote,
				       fwupd_release_get_remote_id (rel2)) != 0)
				continue;
			if (g_strcmp0 (fwupd_release_get_version (rel),
				       fwupd_release_get_version (rel2)) != 0)
				continue;
			g_node_append_data (child, g_object_ref (rel2));
			rel = NULL;
			break;
		}

		/* didn't match anything */
		if (rels->len == 0 || rel != NULL) {
			g_node_append_data (child, rel);
			continue;
		}

	}
	fu_util_print_tree (root, title);

	return TRUE;
}

static gboolean
fu_util_refresh_remote (FuUtilPrivate *priv, FwupdRemote *remote, GError **error)
{
	const gchar *metadata_uri = NULL;
	g_autofree gchar *fn_raw = NULL;
	g_autofree gchar *fn_sig = NULL;
	g_autoptr(GBytes) bytes_raw = NULL;
	g_autoptr(GBytes) bytes_sig = NULL;

	/* signature */
	metadata_uri = fwupd_remote_get_metadata_uri_sig (remote);
	if (metadata_uri == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "no metadata signature URI available for %s",
			     fwupd_remote_get_id (remote));
		return FALSE;
	}
	fn_sig = fu_util_get_user_cache_path (metadata_uri);
	if (!fu_common_mkdir_parent (fn_sig, error))
		return FALSE;
	if (!fu_util_download_out_of_process (metadata_uri, fn_sig, error))
		return FALSE;
	bytes_sig = fu_common_get_contents_bytes (fn_sig, error);
	if (bytes_sig == NULL)
		return FALSE;
	if (!fwupd_remote_load_signature_bytes (remote, bytes_sig, error))
		return FALSE;

	/* payload */
	metadata_uri = fwupd_remote_get_metadata_uri (remote);
	if (metadata_uri == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "no metadata URI available for %s",
			     fwupd_remote_get_id (remote));
		return FALSE;
	}
	fn_raw = fu_util_get_user_cache_path (metadata_uri);
	if (!fu_util_download_out_of_process (metadata_uri, fn_raw, error))
		return FALSE;
	bytes_raw = fu_common_get_contents_bytes (fn_raw, error);
	if (bytes_raw == NULL)
		return FALSE;

	/* send to daemon */
	g_debug ("updating %s", fwupd_remote_get_id (remote));
	return fu_engine_update_metadata_bytes (priv->engine,
						fwupd_remote_get_id (remote),
						bytes_raw,
						bytes_sig,
						error);

}

static gboolean
fu_util_refresh (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* download new metadata */
	remotes = fu_engine_get_remotes (priv->engine, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		if (!fwupd_remote_get_enabled (remote))
			continue;
		if (fwupd_remote_get_kind (remote) != FWUPD_REMOTE_KIND_DOWNLOAD)
			continue;
		if (!fu_util_refresh_remote (priv, remote, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
fu_util_get_remotes (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autoptr(GPtrArray) remotes = NULL;
	g_autofree gchar *title = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv, FU_ENGINE_LOAD_FLAG_REMOTES, error))
		return FALSE;
	title = fu_util_get_tree_title (priv);

	/* list remotes */
	remotes = fu_engine_get_remotes (priv->engine, error);
	if (remotes == NULL)
		return FALSE;
	if (remotes->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "no remotes available");
		return FALSE;
	}
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote_tmp = g_ptr_array_index (remotes, i);
		g_node_append_data (root, remote_tmp);
	}
	fu_util_print_tree (root, title);

	return TRUE;
}

static gboolean
fu_util_security (FuUtilPrivate *priv, gchar **values, GError **error)
{
	FuSecurityAttrToStringFlags flags = FU_SECURITY_ATTR_TO_STRING_FLAG_NONE;
	g_autoptr(FuSecurityAttrs) attrs = NULL;
	g_autoptr(GPtrArray) items = NULL;
	g_autofree gchar *str = NULL;

	/* not ready yet */
	if ((priv->flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "The HSI specification is not yet complete. "
				     "To ignore this warning, use --force");
		return FALSE;
	}

	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* TRANSLATORS: this is a string like 'HSI:2-U' */
	g_print ("%s \033[1m%s\033[0m\n", _("Host Security ID:"),
		 fu_engine_get_host_security_id (priv->engine));

	/* show or hide different elements */
	if (priv->show_all) {
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_OBSOLETES;
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_URLS;
	}

	/* print the "why" */
	attrs = fu_engine_get_host_security_attrs (priv->engine);
	items = fu_security_attrs_get_all (attrs);
	str = fu_util_security_attrs_to_string (items, flags);
	g_print ("%s\n", str);
	return TRUE;
}

static FuVolume *
fu_util_prompt_for_volume (GError **error)
{
	FuVolume *volume;
	guint idx;
	gboolean is_fallback = FALSE;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GPtrArray) volumes_vfat = g_ptr_array_new ();
	g_autoptr(GError) error_local = NULL;

	/* exactly one */
	volumes = fu_common_get_volumes_by_kind (FU_VOLUME_KIND_ESP, &error_local);
	if (volumes == NULL) {
		is_fallback = TRUE;
		g_debug ("%s, falling back to %s", error_local->message, FU_VOLUME_KIND_BDP);
		volumes = fu_common_get_volumes_by_kind (FU_VOLUME_KIND_BDP, error);
		if (volumes == NULL) {
			g_prefix_error (error, "%s: ", error_local->message);
			return NULL;
		}
	}
	/* on fallback: only add internal vfat partitions */
	for (guint i = 0; i < volumes->len; i++) {
		FuVolume *vol = g_ptr_array_index (volumes, i);
		g_autofree gchar *type = fu_volume_get_id_type (vol);
		if (type == NULL)
			continue;
		if (is_fallback && !fu_volume_is_internal (vol))
			continue;
		if (g_strcmp0 (type, "vfat") == 0)
			g_ptr_array_add (volumes_vfat, vol);
	}
	if (volumes_vfat->len == 1) {
		volume = g_ptr_array_index (volumes_vfat, 0);
		/* TRANSLATORS: Volume has been chosen by the user */
		g_print ("%s: %s\n", _("Selected volume"), fu_volume_get_id (volume));
		return g_object_ref (volume);
	}

	/* TRANSLATORS: get interactive prompt */
	g_print ("%s\n", _("Choose a volume:"));
	/* TRANSLATORS: this is to abort the interactive prompt */
	g_print ("0.\t%s\n", _("Cancel"));
	for (guint i = 0; i < volumes_vfat->len; i++) {
		volume = g_ptr_array_index (volumes_vfat, i);
		g_print ("%u.\t%s\n", i + 1, fu_volume_get_id (volume));
	}
	idx = fu_util_prompt_for_number (volumes_vfat->len);
	if (idx == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "Request canceled");
		return NULL;
	}
	volume = g_ptr_array_index (volumes_vfat, idx - 1);
	return g_object_ref (volume);

}

static gboolean
fu_util_esp_mount (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuVolume) volume = NULL;
	volume = fu_util_prompt_for_volume (error);
	if (volume == NULL)
		return FALSE;
	return fu_volume_mount (volume, error);
}

static gboolean
fu_util_esp_unmount (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuVolume) volume = NULL;
	volume = fu_util_prompt_for_volume (error);
	if (volume == NULL)
		return FALSE;
	return fu_volume_unmount (volume, error);
}

static gboolean
fu_util_esp_list (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autofree gchar *mount_point = NULL;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuVolume) volume = NULL;
	g_autoptr(GPtrArray) files = NULL;

	volume = fu_util_prompt_for_volume (error);
	if (volume == NULL)
		return FALSE;
	locker = fu_volume_locker (volume, error);
	if (locker == NULL)
		return FALSE;
	mount_point = fu_volume_get_mount_point (volume);
	files = fu_common_get_files_recursive (mount_point, error);
	if (files == NULL)
		return FALSE;
	for (guint i = 0; i < files->len; i++) {
		const gchar *fn = g_ptr_array_index (files, i);
		g_print ("%s\n", fn);
	}
	return TRUE;
}

static gboolean
_g_str_equal0 (gconstpointer str1, gconstpointer str2)
{
	return g_strcmp0 (str1, str2) == 0;
}

static gboolean
fu_util_switch_branch (FuUtilPrivate *priv, gchar **values, GError **error)
{
	const gchar *branch;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(GPtrArray) branches = g_ptr_array_new_with_free_func (g_free);
	g_autoptr(FuDevice) dev = NULL;

	/* load engine */
	if (!fu_util_start_engine (priv,
				   FU_ENGINE_LOAD_FLAG_COLDPLUG |
				   FU_ENGINE_LOAD_FLAG_HWINFO |
				   FU_ENGINE_LOAD_FLAG_REMOTES,
				   error))
		return FALSE;

	/* find the device and check it has multiple branches */
	priv->filter_include |= FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES;
	priv->filter_include |= FWUPD_DEVICE_FLAG_UPDATABLE;
	if (g_strv_length (values) == 1)
		dev = fu_util_get_device (priv, values[1], error);
	else
		dev = fu_util_prompt_for_device (priv, NULL, error);
	if (dev == NULL)
		return FALSE;
	if (!fu_device_has_flag (dev, FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "Multiple branches not available");
		return FALSE;
	}

	/* get all releases, including the alternate branch versions */
	rels = fu_engine_get_releases (priv->engine,
				       priv->request,
				       fu_device_get_id (dev),
				       error);
	if (rels == NULL)
		return FALSE;

	/* get all the unique branches */
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (rels, i);
		const gchar *branch_tmp = fwupd_release_get_branch (rel_tmp);
#if GLIB_CHECK_VERSION(2,54,3)
		if (g_ptr_array_find_with_equal_func (branches, branch_tmp,
						      _g_str_equal0, NULL))
			continue;
#endif
		g_ptr_array_add (branches, g_strdup (branch_tmp));
	}

	/* branch name is optional */
	if (g_strv_length (values) > 1) {
		branch = values[1];
	} else if (branches->len == 1) {
		branch = g_ptr_array_index (branches, 0);
	} else {
		guint idx;

		/* TRANSLATORS: get interactive prompt, where branch is the
		 * supplier of the firmware, e.g. "non-free" or "free" */
		g_print ("%s\n", _("Choose a branch:"));
		/* TRANSLATORS: this is to abort the interactive prompt */
		g_print ("0.\t%s\n", _("Cancel"));
		for (guint i = 0; i < branches->len; i++) {
			const gchar *branch_tmp = g_ptr_array_index (branches, i);
			g_print ("%u.\t%s\n", i + 1, fu_util_branch_for_display (branch_tmp));
		}
		idx = fu_util_prompt_for_number (branches->len);
		if (idx == 0) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOTHING_TO_DO,
					     "Request canceled");
			return FALSE;
		}
		branch = g_ptr_array_index (branches, idx - 1);
	}

	/* sanity check */
	if (g_strcmp0 (branch, fu_device_get_branch (dev)) == 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Device %s is already on branch %s",
			     fu_device_get_name (dev),
			     fu_util_branch_for_display (branch));
		return FALSE;
	}

	/* the releases are ordered by version */
	for (guint j = 0; j < rels->len; j++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (rels, j);
		if (g_strcmp0 (fwupd_release_get_branch (rel_tmp), branch) == 0) {
			rel = g_object_ref (rel_tmp);
			break;
		}
	}
	if (rel == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "No releases for branch %s",
			     fu_util_branch_for_display (branch));
		return FALSE;
	}

	/* we're switching branch */
	if (!fu_util_switch_branch_warning (FWUPD_DEVICE (dev), rel, FALSE, error))
		return FALSE;

	/* update the console if composite devices are also updated */
	priv->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect (priv->engine, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	if (!fu_util_install_release (priv, rel, error))
		return FALSE;
	fu_util_display_current_message (priv);

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

int
main (int argc, char *argv[])
{
	gboolean allow_branch_switch = FALSE;
	gboolean allow_older = FALSE;
	gboolean allow_reinstall = FALSE;
	gboolean force = FALSE;
	gboolean ret;
	gboolean version = FALSE;
	gboolean ignore_checksum = FALSE;
	gboolean ignore_power = FALSE;
	gboolean ignore_vid_pid = FALSE;
	gboolean interactive = isatty (fileno (stdout)) != 0;
	g_auto(GStrv) plugin_glob = NULL;
	g_autoptr(FuUtilPrivate) priv = g_new0 (FuUtilPrivate, 1);
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) cmd_array = fu_util_cmd_array_new ();
	g_autofree gchar *cmd_descriptions = NULL;
	g_autofree gchar *filter = NULL;
	const GOptionEntry options[] = {
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &version,
			/* TRANSLATORS: command line option */
			_("Show client and daemon versions"), NULL },
		{ "allow-reinstall", '\0', 0, G_OPTION_ARG_NONE, &allow_reinstall,
			/* TRANSLATORS: command line option */
			_("Allow reinstalling existing firmware versions"), NULL },
		{ "allow-older", '\0', 0, G_OPTION_ARG_NONE, &allow_older,
			/* TRANSLATORS: command line option */
			_("Allow downgrading firmware versions"), NULL },
		{ "allow-branch-switch", '\0', 0, G_OPTION_ARG_NONE, &allow_branch_switch,
			/* TRANSLATORS: command line option */
			_("Allow switching firmware branch"), NULL },
		{ "force", '\0', 0, G_OPTION_ARG_NONE, &force,
			/* TRANSLATORS: command line option */
			_("Force the action by relaxing some runtime checks"), NULL },
		{ "ignore-checksum", '\0', 0, G_OPTION_ARG_NONE, &ignore_checksum,
			/* TRANSLATORS: command line option */
			_("Ignore firmware checksum failures"), NULL },
		{ "ignore-vid-pid", '\0', 0, G_OPTION_ARG_NONE, &ignore_vid_pid,
			/* TRANSLATORS: command line option */
			_("Ignore firmware hardware mismatch failures"), NULL },
		{ "ignore-power", '\0', 0, G_OPTION_ARG_NONE, &ignore_power,
			/* TRANSLATORS: command line option */
			_("Ignore requirement of external power source"), NULL },
		{ "no-reboot-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_reboot_check,
			/* TRANSLATORS: command line option */
			_("Do not check or prompt for reboot after update"), NULL },
		{ "no-safety-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_safety_check,
			/* TRANSLATORS: command line option */
			_("Do not perform device safety checks"), NULL },
		{ "show-all", '\0', 0, G_OPTION_ARG_NONE, &priv->show_all,
			/* TRANSLATORS: command line option */
			_("Show all results"), NULL },
		{ "show-all-devices", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &priv->show_all,
			/* TRANSLATORS: command line option */
			_("Show devices that are not updatable"), NULL },
		{ "plugins", '\0', 0, G_OPTION_ARG_STRING_ARRAY, &plugin_glob,
			/* TRANSLATORS: command line option */
			_("Manually enable specific plugins"), NULL },
		{ "plugin-whitelist", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_STRING_ARRAY, &plugin_glob,
			/* TRANSLATORS: command line option */
			_("Manually enable specific plugins"), NULL },
		{ "prepare", '\0', 0, G_OPTION_ARG_NONE, &priv->prepare_blob,
			/* TRANSLATORS: command line option */
			_("Run the plugin composite prepare routine when using install-blob"), NULL },
		{ "cleanup", '\0', 0, G_OPTION_ARG_NONE, &priv->cleanup_blob,
			/* TRANSLATORS: command line option */
			_("Run the plugin composite cleanup routine when using install-blob"), NULL },
		{ "enable-json-state", '\0', 0, G_OPTION_ARG_NONE, &priv->enable_json_state,
			/* TRANSLATORS: command line option */
			_("Save device state into a JSON file between executions"), NULL },
		{ "disable-ssl-strict", '\0', 0, G_OPTION_ARG_NONE, &priv->disable_ssl_strict,
			/* TRANSLATORS: command line option */
			_("Ignore SSL strict checks when downloading files"), NULL },
		{ "filter", '\0', 0, G_OPTION_ARG_STRING, &filter,
			/* TRANSLATORS: command line option */
			_("Filter with a set of device flags using a ~ prefix to "
			  "exclude, e.g. 'internal,~needs-reboot'"), NULL },
		{ NULL}
	};

#ifdef _WIN32
	/* workaround Windows setting the codepage to 1252 */
	g_setenv ("LANG", "C.UTF-8", FALSE);
#endif

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, FWUPD_LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

#ifdef HAVE_GETUID
	/* ensure root user */
	if (interactive && (getuid () != 0 || geteuid () != 0))
		/* TRANSLATORS: we're poking around as a power user */
		g_printerr ("%s\n", _("This program may only work correctly as root"));
#endif

	/* create helper object */
	priv->main_ctx = g_main_context_new ();
	priv->loop = g_main_loop_new (priv->main_ctx, FALSE);
	priv->progressbar = fu_progressbar_new ();
	priv->request = fu_engine_request_new ();

	/* add commands */
	fu_util_cmd_array_add (cmd_array,
		     "build-firmware",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILE-IN FILE-OUT [SCRIPT] [OUTPUT]"),
		     /* TRANSLATORS: command description */
		     _("Build firmware using a sandbox"),
		     fu_util_firmware_builder);
	fu_util_cmd_array_add (cmd_array,
		     "smbios-dump",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILE"),
		     /* TRANSLATORS: command description */
		     _("Dump SMBIOS data from a file"),
		     fu_util_smbios_dump);
	fu_util_cmd_array_add (cmd_array,
		     "get-plugins",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Get all enabled plugins registered with the system"),
		     fu_util_get_plugins);
	fu_util_cmd_array_add (cmd_array,
		     "get-details",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Gets details about a firmware file"),
		     fu_util_get_details);
	fu_util_cmd_array_add (cmd_array,
		     "get-history",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Show history of firmware updates"),
		     fu_util_get_history);
	fu_util_cmd_array_add (cmd_array,
		     "get-updates,get-upgrades",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Gets the list of updates for connected hardware"),
		     fu_util_get_updates);
	fu_util_cmd_array_add (cmd_array,
		     "get-devices,get-topology",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Get all devices that support firmware updates"),
		     fu_util_get_devices);
	fu_util_cmd_array_add (cmd_array,
		     "get-device-flags",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Get all device flags supported by fwupd"),
		     fu_util_get_device_flags);
	fu_util_cmd_array_add (cmd_array,
		     "watch",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Watch for hardware changes"),
		     fu_util_watch);
	fu_util_cmd_array_add (cmd_array,
		     "install-blob",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME DEVICE-ID"),
		     /* TRANSLATORS: command description */
		     _("Install a firmware blob on a device"),
		     fu_util_install_blob);
	fu_util_cmd_array_add (cmd_array,
		     "install",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILE [DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Install a firmware file on this hardware"),
		     fu_util_install);
	fu_util_cmd_array_add (cmd_array,
		     "reinstall",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("DEVICE-ID|GUID"),
		     /* TRANSLATORS: command description */
		     _("Reinstall firmware on a device"),
		     fu_util_reinstall);
	fu_util_cmd_array_add (cmd_array,
		     "attach",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("DEVICE-ID|GUID"),
		     /* TRANSLATORS: command description */
		     _("Attach to firmware mode"),
		     fu_util_attach);
	fu_util_cmd_array_add (cmd_array,
		     "detach",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("DEVICE-ID|GUID"),
		     /* TRANSLATORS: command description */
		     _("Detach to bootloader mode"),
		     fu_util_detach);
	fu_util_cmd_array_add (cmd_array,
		     "unbind-driver",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Unbind current driver"),
		     fu_util_unbind_driver);
	fu_util_cmd_array_add (cmd_array,
		     "bind-driver",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("SUBSYSTEM DRIVER [DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Bind new kernel driver"),
		     fu_util_bind_driver);
	fu_util_cmd_array_add (cmd_array,
		     "activate",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Activate pending devices"),
		     fu_util_activate);
	fu_util_cmd_array_add (cmd_array,
		     "hwids",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[SMBIOS-FILE|HWIDS-FILE]"),
		     /* TRANSLATORS: command description */
		     _("Return all the hardware IDs for the machine"),
		     fu_util_hwids);
	fu_util_cmd_array_add (cmd_array,
		     "export-hwids",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("HWIDS-FILE"),
		     /* TRANSLATORS: command description */
		     _("Save a file that allows generation of hardware IDs"),
		     fu_util_export_hwids);
	fu_util_cmd_array_add (cmd_array,
		     "monitor",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Monitor the daemon for events"),
		     fu_util_monitor);
	fu_util_cmd_array_add (cmd_array,
		     "update,upgrade",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Update all devices that match local metadata"),
		     fu_util_update);
	fu_util_cmd_array_add (cmd_array,
		     "self-sign",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("TEXT"),
		     /* TRANSLATORS: command description */
		     C_("command-description",
			"Sign data using the client certificate"),
		     fu_util_self_sign);
	fu_util_cmd_array_add (cmd_array,
		     "verify-update",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Update the stored metadata with current contents"),
		     fu_util_verify_update);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-sign",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME CERTIFICATE PRIVATE-KEY"),
		     /* TRANSLATORS: command description */
		     _("Sign a firmware with a new key"),
		     fu_util_firmware_sign);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-dump",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME [DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Read a firmware blob from a device"),
		     fu_util_firmware_dump);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-convert",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME-SRC FILENAME-DST [FIRMWARE-TYPE-SRC] [FIRMWARE-TYPE-DST]"),
		     /* TRANSLATORS: command description */
		     _("Convert a firmware file"),
		     fu_util_firmware_convert);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-build",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("BUILDER-XML FILENAME-DST"),
		     /* TRANSLATORS: command description */
		     _("Build a firmware file"),
		     fu_util_firmware_build);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-parse",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME [FIRMWARE-TYPE]"),
		     /* TRANSLATORS: command description */
		     _("Parse and show details about a firmware file"),
		     fu_util_firmware_parse);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-export",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME [FIRMWARE-TYPE]"),
		     /* TRANSLATORS: command description */
		     _("Export a firmware file structure to XML"),
		     fu_util_firmware_export);
	fu_util_cmd_array_add (cmd_array,
		     "firmware-extract",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILENAME [FIRMWARE-TYPE]"),
		     /* TRANSLATORS: command description */
		     _("Extract a firmware blob to images"),
		     fu_util_firmware_extract);
	fu_util_cmd_array_add (cmd_array,
		     "get-firmware-types",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("List the available firmware types"),
		     fu_util_get_firmware_types);
	fu_util_cmd_array_add (cmd_array,
		     "get-remotes",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Gets the configured remotes"),
		     fu_util_get_remotes);
	fu_util_cmd_array_add (cmd_array,
		     "refresh",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Refresh metadata from remote server"),
		     fu_util_refresh);
	fu_util_cmd_array_add (cmd_array,
		     "security",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Gets the host security attributes"),
		     fu_util_security);
	fu_util_cmd_array_add (cmd_array,
		     "esp-mount",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Mounts the ESP"),
		     fu_util_esp_mount);
	fu_util_cmd_array_add (cmd_array,
		     "esp-unmount",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Unmounts the ESP"),
		     fu_util_esp_unmount);
	fu_util_cmd_array_add (cmd_array,
		     "esp-list",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Lists files on the ESP"),
		     fu_util_esp_list);
	fu_util_cmd_array_add (cmd_array,
		     "switch-branch",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID] [BRANCH]"),
		     /* TRANSLATORS: command description */
		     _("Switch the firmware branch on the device"),
		     fu_util_switch_branch);

	/* do stuff on ctrl+c */
	priv->cancellable = g_cancellable_new ();
#ifdef HAVE_GIO_UNIX
	g_unix_signal_add_full (G_PRIORITY_DEFAULT,
				SIGINT, fu_util_sigint_cb,
				priv, NULL);
#endif
	g_signal_connect (priv->cancellable, "cancelled",
			  G_CALLBACK (fu_util_cancelled_cb), priv);

	/* sort by command name */
	fu_util_cmd_array_sort (cmd_array);

	/* non-TTY consoles cannot answer questions */
	if (!interactive) {
		priv->no_reboot_check = TRUE;
		priv->no_safety_check = TRUE;
		fu_progressbar_set_interactive (priv->progressbar, FALSE);
	} else {
		/* set our implemented feature set */
		fu_engine_request_set_feature_flags (priv->request,
						     FWUPD_FEATURE_FLAG_DETACH_ACTION |
						     FWUPD_FEATURE_FLAG_SWITCH_BRANCH |
						     FWUPD_FEATURE_FLAG_UPDATE_ACTION);
	}

	/* get a list of the commands */
	priv->context = g_option_context_new (NULL);
	cmd_descriptions = fu_util_cmd_array_to_string (cmd_array);
	g_option_context_set_summary (priv->context, cmd_descriptions);
	g_option_context_set_description (priv->context,
		/* TRANSLATORS: CLI description */
		_("This tool allows an administrator to use the fwupd plugins "
		  "without being installed on the host system."));

	/* TRANSLATORS: program name */
	g_set_application_name (_("Firmware Utility"));
	g_option_context_add_main_entries (priv->context, options, NULL);
	g_option_context_add_group (priv->context, fu_debug_get_option_group ());
	ret = g_option_context_parse (priv->context, &argc, &argv, &error);
	if (!ret) {
		/* TRANSLATORS: the user didn't read the man page */
		g_print ("%s: %s\n", _("Failed to parse arguments"),
			 error->message);
		return EXIT_FAILURE;
	}

	/* allow disabling SSL strict mode for broken corporate proxies */
	if (priv->disable_ssl_strict) {
		g_autofree gchar *fmt = NULL;
		/* TRANSLATORS: this is a prefix on the console */
		fmt = fu_util_term_format (_("WARNING:"), FU_UTIL_TERM_COLOR_RED);
		/* TRANSLATORS: try to help */
		g_printerr ("%s %s\n", fmt, _("Ignoring SSL strict checks, "
					      "to do this automatically in the future "
					      "export DISABLE_SSL_STRICT in your environment"));
		g_setenv ("DISABLE_SSL_STRICT", "1", TRUE);
	}

	/* parse filter flags */
	if (filter != NULL) {
		if (!fu_util_parse_filter_flags (filter,
						 &priv->filter_include,
						 &priv->filter_exclude,
						 &error)) {
			/* TRANSLATORS: the user didn't read the man page */
			g_print ("%s: %s\n", _("Failed to parse flags for --filter"),
				 error->message);
			return EXIT_FAILURE;
		}
	}


	/* set flags */
	if (allow_reinstall)
		priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	if (allow_older)
		priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;
	if (allow_branch_switch)
		priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	if (force) {
		priv->flags |= FWUPD_INSTALL_FLAG_FORCE;
		priv->flags |= FWUPD_INSTALL_FLAG_IGNORE_POWER;
	}
	if (ignore_checksum)
		priv->flags |= FWUPD_INSTALL_FLAG_IGNORE_CHECKSUM;
	if (ignore_vid_pid)
		priv->flags |= FWUPD_INSTALL_FLAG_IGNORE_VID_PID;
	if (ignore_power)
		priv->flags |= FWUPD_INSTALL_FLAG_IGNORE_POWER;

	/* load engine */
	priv->engine = fu_engine_new (FU_APP_FLAGS_NO_IDLE_SOURCES);
	g_signal_connect (priv->engine, "device-added",
			  G_CALLBACK (fu_main_engine_device_added_cb),
			  priv);
	g_signal_connect (priv->engine, "device-removed",
			  G_CALLBACK (fu_main_engine_device_removed_cb),
			  priv);
	g_signal_connect (priv->engine, "status-changed",
			  G_CALLBACK (fu_main_engine_status_changed_cb),
			  priv);
	g_signal_connect (priv->engine, "percentage-changed",
			  G_CALLBACK (fu_main_engine_percentage_changed_cb),
			  priv);

	/* just show versions and exit */
	if (version) {
		g_autofree gchar *version_str = fu_util_get_versions ();
		g_print ("%s\n", version_str);
		return EXIT_SUCCESS;
	}

	/* any plugin allowlist specified */
	for (guint i = 0; plugin_glob != NULL && plugin_glob[i] != NULL; i++)
		fu_engine_add_plugin_filter (priv->engine, plugin_glob[i]);

	/* run the specified command */
	ret = fu_util_cmd_array_run (cmd_array, priv, argv[1], (gchar**) &argv[2], &error);
	if (!ret) {
		g_printerr ("%s\n", error->message);
		if (g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_INVALID_ARGS)) {
			/* TRANSLATORS: error message explaining command to run to how to get help */
			g_printerr ("\n%s\n", _("Use fwupdtool --help for help"));
		} else if (g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_debug ("%s\n", error->message);
			return EXIT_NOTHING_TO_DO;
		}
		return EXIT_FAILURE;
	}

	/* success */
	return EXIT_SUCCESS;
}
