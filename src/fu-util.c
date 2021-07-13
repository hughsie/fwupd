/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#define G_LOG_DOMAIN				"FuMain"

#include "config.h"

#include <fwupd.h>
#include <xmlb.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib/gi18n.h>
#ifdef HAVE_GIO_UNIX
#include <glib-unix.h>
#endif
#include <json-glib/json-glib.h>
#include <locale.h>
#include <stdlib.h>
#include <unistd.h>

#include "fu-history.h"
#include "fu-plugin-private.h"
#include "fu-polkit-agent.h"
#include "fu-progressbar.h"
#include "fu-security-attrs.h"
#include "fu-util-common.h"
#include "fwupd-common-private.h"

#ifdef HAVE_SYSTEMD
#include "fu-systemd.h"
#endif

/* custom return code */
#define EXIT_NOTHING_TO_DO		2

typedef enum {
	FU_UTIL_OPERATION_UNKNOWN,
	FU_UTIL_OPERATION_UPDATE,
	FU_UTIL_OPERATION_DOWNGRADE,
	FU_UTIL_OPERATION_INSTALL,
	FU_UTIL_OPERATION_LAST
} FuUtilOperation;

struct FuUtilPrivate {
	GCancellable		*cancellable;
	GMainContext		*main_ctx;
	GOptionContext		*context;
	FwupdInstallFlags	 flags;
	FwupdClientDownloadFlags download_flags;
	FwupdClient		*client;
	FuProgressbar		*progressbar;
	gboolean		 no_remote_check;
	gboolean		 no_metadata_check;
	gboolean		 no_reboot_check;
	gboolean		 no_unreported_check;
	gboolean		 no_safety_check;
	gboolean		 assume_yes;
	gboolean		 sign;
	gboolean		 show_all;
	gboolean		 disable_ssl_strict;
	/* only valid in update and downgrade */
	FuUtilOperation		 current_operation;
	FwupdDevice		*current_device;
	gchar			*current_message;
	FwupdDeviceFlags	 completion_flags;
	FwupdDeviceFlags	 filter_include;
	FwupdDeviceFlags	 filter_exclude;
};

static gboolean	fu_util_report_history (FuUtilPrivate *priv, gchar **values, GError **error);

static void
fu_util_client_notify_cb (GObject *object,
			  GParamSpec *pspec,
			  FuUtilPrivate *priv)
{
	fu_progressbar_update (priv->progressbar,
			       fwupd_client_get_status (priv->client),
			       fwupd_client_get_percentage (priv->client));
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

	/* immediate action required */
	if (fwupd_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER) &&
	    fwupd_device_get_update_message_kind (device) == FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE &&
	    fwupd_device_get_update_message (device) != NULL) {
		g_autofree gchar *fmt = NULL;
		g_autofree gchar *tmp = NULL;

		/* TRANSLATORS: the user needs to do something, e.g. remove the device */
		fmt = fu_util_term_format (_("Action Required:"), FU_UTIL_TERM_COLOR_RED);
		tmp = g_strdup_printf ("%s %s", fmt,
				       fwupd_device_get_update_message (device));
		fu_progressbar_set_title (priv->progressbar, tmp);
	}

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
	} else if (priv->current_operation == FU_UTIL_OPERATION_DOWNGRADE) {
		/* TRANSLATORS: %1 is a device name */
		str = g_strdup_printf (_("Downgrading %s…"),
				       fwupd_device_get_name (device));
		fu_progressbar_set_title (priv->progressbar, str);
	} else if (priv->current_operation == FU_UTIL_OPERATION_INSTALL) {
		/* TRANSLATORS: %1 is a device name  */
		str = g_strdup_printf (_("Installing on %s…"),
				       fwupd_device_get_name (device));
		fu_progressbar_set_title (priv->progressbar, str);
	} else {
		g_warning ("no FuUtilOperation set");
	}
	g_set_object (&priv->current_device, device);

	if (fwupd_device_get_update_message_kind (device) == FWUPD_DEVICE_MESSAGE_KIND_POST &&
	    fwupd_device_get_update_message (device) != NULL &&
	    priv->current_message == NULL) {
		const gchar *tmp = fwupd_device_get_update_message (device);
		if (tmp != NULL)
			priv->current_message = g_strdup (tmp);
	}
}

static gboolean
fu_util_filter_device (FuUtilPrivate *priv, FwupdDevice *dev)
{
	for (guint i = 0; i < 64; i++) {
		FwupdDeviceFlags flag = 1LLU << i;
		if (priv->filter_include & flag) {
			if (!fwupd_device_has_flag (dev, flag))
				return FALSE;
		}
		if (priv->filter_exclude & flag) {
			if (fwupd_device_has_flag (dev, flag))
				return FALSE;
		}
	}
	return TRUE;
}

static FwupdDevice *
fu_util_prompt_for_device (FuUtilPrivate *priv, GPtrArray *devices, GError **error)
{
	FwupdDevice *dev;
	guint idx;
	g_autoptr(GPtrArray) devices_filtered = NULL;

	/* filter results */
	devices_filtered = g_ptr_array_new ();
	for (guint i = 0; i < devices->len; i++) {
		dev = g_ptr_array_index (devices, i);
		if (!fu_util_filter_device (priv, dev))
			continue;
		g_ptr_array_add (devices_filtered, dev);
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
		g_print ("%s: %s\n", _("Selected device"), fwupd_device_get_name (dev));
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
			 fwupd_device_get_id (dev),
			 fwupd_device_get_name (dev));
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

static gboolean
fu_util_perhaps_show_unreported (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_failed = g_ptr_array_new ();
	g_autoptr(GPtrArray) devices_success = g_ptr_array_new ();
	g_autoptr(GPtrArray) remotes = NULL;
	g_autoptr(GHashTable) remote_id_uri_map = NULL;
	gboolean all_automatic = FALSE;

	/* we don't want to ask anything */
	if (priv->no_unreported_check) {
		g_debug ("skipping unreported check");
		return TRUE;
	}

	/* get all devices from the history database */
	devices = fwupd_client_get_history (priv->client, NULL, &error_local);
	if (devices == NULL) {
		if (g_error_matches (error_local, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO))
			return TRUE;
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}

	/* create a map of RemoteID to RemoteURI */
	remotes = fwupd_client_get_remotes (priv->client, NULL, error);
	if (remotes == NULL)
		return FALSE;
	remote_id_uri_map = g_hash_table_new (g_str_hash, g_str_equal);
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		gboolean remote_automatic;
		if (fwupd_remote_get_id (remote) == NULL)
			continue;
		if (fwupd_remote_get_report_uri (remote) == NULL)
			continue;
		g_debug ("adding %s for %s",
			 fwupd_remote_get_report_uri (remote),
			 fwupd_remote_get_id (remote));
		g_hash_table_insert (remote_id_uri_map,
				     (gpointer) fwupd_remote_get_id (remote),
				     (gpointer) fwupd_remote_get_report_uri (remote));
		remote_automatic = fwupd_remote_get_automatic_reports (remote);
		g_debug ("%s is %d", fwupd_remote_get_title (remote), remote_automatic);
		if (remote_automatic && !all_automatic)
			all_automatic = TRUE;
		if (!remote_automatic && all_automatic) {
			all_automatic = FALSE;
			break;
		}
	}

	/* check that they can be reported */
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel = fwupd_device_get_release_default (dev);
		const gchar *remote_id;
		const gchar *remote_uri;

		if (!fu_util_filter_device (priv, dev))
			continue;
		if (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_REPORTED))
			continue;
		if (!fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED))
			continue;

		/* find the RemoteURI to use for the device */
		remote_id = fwupd_release_get_remote_id (rel);
		if (remote_id == NULL) {
			g_debug ("%s has no RemoteID", fwupd_device_get_id (dev));
			continue;
		}
		remote_uri = g_hash_table_lookup (remote_id_uri_map, remote_id);
		if (remote_uri == NULL) {
			g_debug ("%s has no RemoteURI", remote_id);
			continue;
		}

		/* only send success and failure */
		if (fwupd_device_get_update_state (dev) == FWUPD_UPDATE_STATE_FAILED) {
			g_ptr_array_add (devices_failed, dev);
		} else if (fwupd_device_get_update_state (dev) == FWUPD_UPDATE_STATE_SUCCESS) {
			g_ptr_array_add (devices_success, dev);
		} else {
			g_debug ("ignoring %s with UpdateState %s",
				 fwupd_device_get_id (dev),
				 fwupd_update_state_to_string (fwupd_device_get_update_state (dev)));
		}
	}

	/* nothing to do */
	if (devices_failed->len == 0 && devices_success->len == 0) {
		g_debug ("no unreported devices");
		return TRUE;
	}

	g_debug ("All automatic: %d", all_automatic);
	/* show the success and failures */
	if (!priv->assume_yes && !all_automatic) {

		/* delimit */
		g_print ("________________________________________________\n");

		/* failures */
		if (devices_failed->len > 0) {
			/* TRANSLATORS: a list of failed updates */
			g_print ("\n%s\n\n", _("Devices that were not updated correctly:"));
			for (guint i = 0; i < devices_failed->len; i++) {
				FwupdDevice *dev = g_ptr_array_index (devices_failed, i);
				FwupdRelease *rel = fwupd_device_get_release_default (dev);
				g_print (" • %s (%s → %s)\n",
					 fwupd_device_get_name (dev),
					 fwupd_device_get_version (dev),
					 fwupd_release_get_version (rel));
			}
		}

		/* success */
		if (devices_success->len > 0) {
			/* TRANSLATORS: a list of successful updates */
			g_print ("\n%s\n\n", _("Devices that have been updated successfully:"));
			for (guint i = 0; i < devices_success->len; i++) {
				FwupdDevice *dev = g_ptr_array_index (devices_success, i);
				FwupdRelease *rel = fwupd_device_get_release_default (dev);
				g_print (" • %s (%s → %s)\n",
					 fwupd_device_get_name (dev),
					 fwupd_device_get_version (dev),
					 fwupd_release_get_version (rel));
			}
		}

		/* ask for permission */
		g_print ("\n%s\n%s (%s) [Y|n]:\n",
			 /* TRANSLATORS: explain why we want to upload */
			 _("Uploading firmware reports helps hardware vendors"
			   " to quickly identify failing and successful updates"
			   " on real devices."),
			 /* TRANSLATORS: ask the user to upload */
			 _("Upload report now?"),
			 /* TRANSLATORS: metadata is downloaded from the Internet */
			 _("Requires internet connection"));
		if (!fu_util_prompt_for_boolean (TRUE)) {
			g_print ("\n%s [y|N]:\n",
				 /* TRANSLATORS: offer to disable this nag */
				 _("Do you want to disable this feature for future updates?"));
			if (fu_util_prompt_for_boolean (FALSE)) {
				for (guint i = 0; i < remotes->len; i++) {
					FwupdRemote *remote = g_ptr_array_index (remotes, i);
					const gchar *remote_id = fwupd_remote_get_id (remote);
					if (fwupd_remote_get_report_uri (remote) == NULL)
						continue;
					if (!fwupd_client_modify_remote (priv->client,
									 remote_id, "ReportURI", "",
									 NULL, error))
						return FALSE;
				}
			}
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOTHING_TO_DO,
					     "Declined upload");
			return FALSE;
		}
	}

	/* upload */
	if (!fu_util_report_history (priv, NULL, error))
		return FALSE;

	/* offer to make automatic */
	if (!priv->assume_yes && !all_automatic) {
		g_print ("\n%s [y|N]:\n",
			 /* TRANSLATORS: offer to stop asking the question */
			 _("Do you want to upload reports automatically for future updates?"));
		if (fu_util_prompt_for_boolean (FALSE)) {
			for (guint i = 0; i < remotes->len; i++) {
				FwupdRemote *remote = g_ptr_array_index (remotes, i);
				const gchar *remote_id = fwupd_remote_get_id (remote);
				if (fwupd_remote_get_report_uri (remote) == NULL)
					continue;
				if (fwupd_remote_get_automatic_reports (remote))
					continue;
				if (!fwupd_client_modify_remote (priv->client,
								 remote_id, "AutomaticReports", "true",
								 NULL, error))
					return FALSE;
			}
		}
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_modify_remote_warning (FuUtilPrivate *priv, FwupdRemote *remote, GError **error)
{
	const gchar *warning_markup = NULL;
	g_autofree gchar *warning_plain = NULL;

	/* get formatted text */
	warning_markup = fwupd_remote_get_agreement (remote);
	if (warning_markup == NULL)
		return TRUE;
	warning_plain = fu_util_convert_description (warning_markup, error);
	if (warning_plain == NULL)
		return FALSE;

	/* TRANSLATORS: a remote here is like a 'repo' or software source */
	fu_util_warning_box (_("Enable new remote?"), warning_plain, 80);
	if (!priv->assume_yes) {
		/* ask for permission */
		g_print ("\n%s [Y|n]: ",
			 /* TRANSLATORS: should the remote still be enabled */
			 _("Agree and enable the remote?"));
		if (!fu_util_prompt_for_boolean (TRUE)) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOTHING_TO_DO,
					     "Declined agreement");
			return FALSE;
		}
	}
	return TRUE;
}

static void
fu_util_build_device_tree (FuUtilPrivate *priv, GNode *root, GPtrArray *devs, FwupdDevice *dev)
{
	for (guint i = 0; i < devs->len; i++) {
		FwupdDevice *dev_tmp = g_ptr_array_index (devs, i);
		if (!fu_util_filter_device (priv, dev_tmp))
			continue;
		if (!priv->show_all &&
		    !fu_util_is_interesting_device (dev_tmp))
			continue;
		if (fwupd_device_get_parent (dev_tmp) == dev) {
			FwupdRelease *rel = fwupd_device_get_release_default (dev_tmp);
			GNode *child = g_node_append_data (root, dev_tmp);
			if (rel != NULL)
				g_node_append_data (child, rel);
			fu_util_build_device_tree (priv, child, devs, dev_tmp);
		}
	}
}

static gchar *
fu_util_get_tree_title (FuUtilPrivate *priv)
{
	return g_strdup (fwupd_client_get_host_product (priv->client));
}

static gboolean
fu_util_get_devices (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autoptr(GPtrArray) devs = NULL;
	g_autofree gchar *title = fu_util_get_tree_title (priv);

	/* get results from daemon */
	devs = fwupd_client_get_devices (priv->client, NULL, error);
	if (devs == NULL)
		return FALSE;
	if (devs->len > 0)
		fu_util_build_device_tree (priv, root, devs, NULL);

	/* print */
	if (g_node_n_children (root) == 0) {
		/* TRANSLATORS: nothing attached that can be upgraded */
		g_print ("%s\n", _("No hardware detected with firmware update capability"));
		return TRUE;
	}
	fu_util_print_tree (root, title);

	/* nag? */
	if (!fu_util_perhaps_show_unreported (priv, error))
		return FALSE;

	return TRUE;
}

static gboolean
fu_util_get_plugins (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) plugins = NULL;

	/* get results from daemon */
	plugins = fwupd_client_get_plugins (priv->client, NULL, error);
	if (plugins == NULL)
		return FALSE;

	/* print */
	for (guint i = 0; i < plugins->len; i++) {
		FuPlugin *plugin = g_ptr_array_index (plugins, i);
		g_autofree gchar *str = fu_util_plugin_to_string (FWUPD_PLUGIN (plugin), 0);
		g_print ("%s\n", str);
	}
	if (plugins->len == 0) {
		/* TRANSLATORS: nothing found */
		g_print ("%s\n", _("No plugins found"));
	}

	/* success */
	return TRUE;
}

static gchar *
fu_util_download_if_required (FuUtilPrivate *priv, const gchar *perhapsfn, GError **error)
{
	g_autofree gchar *filename = NULL;
	g_autoptr(GBytes) blob = NULL;

	/* a local file */
	if (g_file_test (perhapsfn, G_FILE_TEST_EXISTS))
		return g_strdup (perhapsfn);
	if (!fu_util_is_url (perhapsfn))
		return g_strdup (perhapsfn);

	/* download the firmware to a cachedir */
	filename = fu_util_get_user_cache_path (perhapsfn);
	if (!fu_common_mkdir_parent (filename, error))
		return NULL;
	blob = fwupd_client_download_bytes (priv->client, perhapsfn,
					    priv->download_flags,
					    priv->cancellable, error);
	if (blob == NULL)
		return NULL;

	/* save file to cache */
	if (!fu_common_set_contents_bytes (filename, blob, error))
		return NULL;
	return g_steal_pointer (&filename);
}

static void
fu_util_display_current_message (FuUtilPrivate *priv)
{
	if (priv->current_message == NULL) {
		/* TRANSLATORS: success message */
		g_print ("%s\n", _("Successfully installed firmware"));
		return;
	}
	/* TRANSLATORS: success message */
	g_print ("%s: %s\n", _("Successfully installed firmware"), priv->current_message);
	g_clear_pointer (&priv->current_message, g_free);
}

static gboolean
fu_util_install (FuUtilPrivate *priv, gchar **values, GError **error)
{
	const gchar *id;
	g_autofree gchar *filename = NULL;

	/* handle both forms */
	if (g_strv_length (values) == 1) {
		id = FWUPD_DEVICE_ID_ANY;
	} else if (g_strv_length (values) == 2) {
		id = values[1];
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	priv->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect (priv->client, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);

	/* install with flags chosen by the user */
	filename = fu_util_download_if_required (priv, values[0], error);
	if (filename == NULL)
		return FALSE;

	if (!fwupd_client_install (priv->client, id, filename, priv->flags, NULL, error))
		return FALSE;

	fu_util_display_current_message (priv);

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	/* show reboot if needed */
	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_get_details (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) array = NULL;
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = fu_util_get_tree_title (priv);

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

	array = fwupd_client_get_details (priv->client, values[0], NULL, error);
	if (array == NULL)
		return FALSE;
	fu_util_build_device_tree (priv, root, array, NULL);
	fu_util_print_tree (root, title);

	return TRUE;
}

static gboolean
fu_util_clear_history (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuHistory) history = fu_history_new ();
	return fu_history_remove_all (history, error);
}

static gboolean
fu_util_report_history_for_remote (FuUtilPrivate *priv,
				const gchar *remote_id,
				GPtrArray *devices,
				GError **error)
{
	g_autofree gchar *data = NULL;
	g_autofree gchar *sig = NULL;
	g_autofree gchar *uri = NULL;
	g_autoptr(FwupdRemote) remote = NULL;

	/* convert to JSON */
	data = fwupd_build_history_report_json (devices, error);
	if (data == NULL)
		return FALSE;

	/* self sign data */
	if (priv->sign) {
		sig = fwupd_client_self_sign (priv->client, data,
					      FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP,
					      priv->cancellable, error);
		if (sig == NULL)
			return FALSE;
	}

	remote = fwupd_client_get_remote_by_id (priv->client, remote_id,
						NULL, error);
	if (remote == NULL)
		return FALSE;

	/* ask for permission */
	if (!priv->assume_yes && !fwupd_remote_get_automatic_reports (remote)) {
		fu_util_print_data (_("Target"), fwupd_remote_get_report_uri (remote));
		fu_util_print_data (_("Payload"), data);
		if (sig != NULL)
			fu_util_print_data (_("Signature"), sig);
		g_print ("%s [Y|n]: ", _("Proceed with upload?"));
		if (!fu_util_prompt_for_boolean (TRUE)) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_PERMISSION_DENIED,
					     "User declined action");
			return FALSE;
		}
	}

	/* POST request and parse reply */
	if (!fu_util_send_report (priv->client,
				  fwupd_remote_get_report_uri (remote),
				  data, sig, &uri, error))
		return FALSE;

	/* server wanted us to see a message */
	if (uri != NULL) {
		g_print ("%s %s\n",
			 /* TRANSLATORS: the server sent the user a small message */
			 _("Update failure is a known issue, visit this URL for more information:"),
			 uri);
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_report_history (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GHashTable) report_map = NULL;
	g_autoptr(GList) ids = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GString) str = g_string_new (NULL);

	/* get all devices from the history database, then filter them,
	 * adding to a hash map of report-ids */
	devices = fwupd_client_get_history (priv->client, NULL, error);
	if (devices == NULL)
		return FALSE;
	report_map = g_hash_table_new_full (g_str_hash, g_str_equal,
					    g_free, (GDestroyNotify) g_ptr_array_unref);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel = fwupd_device_get_release_default (dev);
		const gchar *remote_id;
		GPtrArray *devices_tmp;
		g_autoptr(FwupdRemote) remote = NULL;

		/* filter, if not forcing */
		if (!fu_util_filter_device (priv, dev))
			continue;
		if ((priv->flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
			if (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_REPORTED))
				continue;
			if (!fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED))
				continue;
		}
		/* only send success and failure */
		if (fwupd_device_get_update_state (dev) != FWUPD_UPDATE_STATE_FAILED &&
		    fwupd_device_get_update_state (dev) != FWUPD_UPDATE_STATE_SUCCESS) {
			g_debug ("ignoring %s with UpdateState %s",
				 fwupd_device_get_id (dev),
				 fwupd_update_state_to_string (fwupd_device_get_update_state (dev)));
			continue;
		}

		/* find the RemoteURI to use for the device */
		remote_id = fwupd_release_get_remote_id (rel);
		if (remote_id == NULL) {
			g_debug ("%s has no RemoteID", fwupd_device_get_id (dev));
			continue;
		}
		remote = fwupd_client_get_remote_by_id (priv->client, remote_id,
							NULL, error);
		if (remote == NULL)
			return FALSE;
		if (fwupd_remote_get_report_uri (remote) == NULL) {
			g_debug ("%s has no RemoteURI", fwupd_remote_get_report_uri (remote));
			continue;
		}

		/* add this to the hash map */
		devices_tmp = g_hash_table_lookup (report_map, remote_id);
		if (devices_tmp == NULL) {
			devices_tmp = g_ptr_array_new ();
			g_hash_table_insert (report_map, g_strdup (remote_id), devices_tmp);
		}
		g_debug ("using %s for %s", remote_id, fwupd_device_get_id (dev));
		g_ptr_array_add (devices_tmp, dev);
	}

	/* nothing to report */
	if (g_hash_table_size (report_map) == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "No reports require uploading");
		return FALSE;
	}

	/* process each uri */
	ids = g_hash_table_get_keys (report_map);
	for (GList *l = ids; l != NULL; l = l->next) {
		const gchar *id = l->data;
		GPtrArray *devices_tmp = g_hash_table_lookup (report_map, id);
		if (!fu_util_report_history_for_remote (priv, id, devices_tmp, error))
			return FALSE;

		/* mark each device as reported */
		for (guint i = 0; i < devices_tmp->len; i++) {
			FwupdDevice *dev = g_ptr_array_index (devices_tmp, i);
			g_debug ("setting flag on %s", fwupd_device_get_id (dev));
			if (!fwupd_client_modify_device (priv->client,
							 fwupd_device_get_id (dev),
							 "Flags", "reported",
							 NULL, error))
				return FALSE;
		}
	}

	/* TRANSLATORS: success message -- where the user has uploaded
	 * success and/or failure reports to the remote server */
	g_string_append_printf (str, ngettext ("Successfully uploaded %u report",
					       "Successfully uploaded %u reports",
					       g_hash_table_size (report_map)),
					       g_hash_table_size (report_map));
	g_print ("%s\n", str->str);
	return TRUE;
}

static gboolean
fu_util_get_history (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = fu_util_get_tree_title (priv);

	/* get all devices from the history database */
	devices = fwupd_client_get_history (priv->client, NULL, error);
	if (devices == NULL)
		return FALSE;

	/* show each device */
	for (guint i = 0; i < devices->len; i++) {
		g_autoptr(GPtrArray) rels = NULL;
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel;
		const gchar *remote;
		GNode *child;
		g_autoptr(GError) error_local = NULL;

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
		rels = fwupd_client_get_releases (priv->client,
						  fwupd_device_get_id (dev),
						  NULL, &error_local);
		if (rels == NULL) {
			g_debug ("failed to get releases for %s: %s",
				 fwupd_device_get_id (dev),
				 error_local->message);
			g_node_append_data (child, rel);
			continue;
		}

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

static FwupdDevice *
fu_util_get_device_by_id (FuUtilPrivate *priv, const gchar *id, GError **error)
{
	if (fwupd_guid_is_valid (id)) {
		g_autoptr(GPtrArray) devices = NULL;
		devices = fwupd_client_get_devices_by_guid (priv->client, id,
							    NULL, error);
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
	return fwupd_client_get_device_by_id (priv->client, id, NULL, error);
}

static FwupdDevice *
fu_util_get_device_or_prompt (FuUtilPrivate *priv, gchar **values, GError **error)
{
	FwupdDevice *dev = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	/* get device to use */
	if (g_strv_length (values) >= 1) {
		g_autoptr(GError) error_local = NULL;
		if (g_strv_length (values) > 1) {
			for (guint i = 1; i < g_strv_length (values); i++)
				g_debug ("Ignoring extra input %s", values[i]);
		}
		dev = fu_util_get_device_by_id (priv, values[0], &error_local);
		if (dev != NULL)
			return dev;
		g_print ("%s\n",  error_local->message);
	}

	/* get all devices from daemon */
	devices = fwupd_client_get_devices (priv->client, NULL, error);
	if (devices == NULL)
		return NULL;
	return fu_util_prompt_for_device (priv, devices, error);
}

static gboolean
fu_util_clear_results (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	return fwupd_client_clear_results (priv->client, fwupd_device_get_id (dev), NULL, error);
}

static gboolean
fu_util_clear_offline (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FuHistory) history = fu_history_new ();
	return fu_history_remove_all_with_state (history, FWUPD_UPDATE_STATE_PENDING, error);
}

static gboolean
fu_util_verify_update (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	priv->filter_include |= FWUPD_DEVICE_FLAG_CAN_VERIFY;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	if (!fwupd_client_verify_update (priv->client, fwupd_device_get_id (dev), NULL, error)) {
		g_prefix_error (error, "failed to verify update %s: ", fu_device_get_name (dev));
		return FALSE;
	}
	/* TRANSLATORS: success message when user refreshes device checksums */
	g_print ("%s\n", _("Successfully updated device checksums"));

	return TRUE;
}

static gboolean
fu_util_download_metadata_enable_lvfs (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;

	/* is the LVFS available but disabled? */
	remote = fwupd_client_get_remote_by_id (priv->client, "lvfs", NULL, error);
	if (remote == NULL)
		return TRUE;
	g_print ("%s\n%s\n%s [Y|n]: ",
		/* TRANSLATORS: explain why no metadata available */
		_("No remotes are currently enabled so no metadata is available."),
		/* TRANSLATORS: explain why no metadata available */
		_("Metadata can be obtained from the Linux Vendor Firmware Service."),
		/* TRANSLATORS: Turn on the remote */
		_("Enable this remote?"));
	if (!fu_util_prompt_for_boolean (TRUE))
		return TRUE;
	if (!fwupd_client_modify_remote (priv->client,
					 fwupd_remote_get_id (remote),
					 "Enabled", "true",
					 priv->cancellable, error))
		return FALSE;
	if (!fu_util_modify_remote_warning (priv, remote, error))
		return FALSE;

	/* refresh the newly-enabled remote */
	return fwupd_client_refresh_remote (priv->client, remote,
					    priv->cancellable, error);
}

static gboolean
fu_util_check_oldest_remote (FuUtilPrivate *priv, guint64 *age_oldest, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;
	gboolean checked = FALSE;

	/* get the age of the oldest enabled remotes */
	remotes = fwupd_client_get_remotes (priv->client, NULL, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		if (!fwupd_remote_get_enabled (remote))
			continue;
		if (fwupd_remote_get_kind (remote) != FWUPD_REMOTE_KIND_DOWNLOAD)
			continue;
		checked = TRUE;
		if (fwupd_remote_get_age (remote) > *age_oldest)
			*age_oldest = fwupd_remote_get_age (remote);
	}
	if (!checked) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     /* TRANSLATORS: error message for a user who ran fwupdmgr refresh recently but no remotes */
				     "No remotes enabled.");
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_util_download_metadata (FuUtilPrivate *priv, GError **error)
{
	gboolean download_remote_enabled = FALSE;
	guint devices_supported_cnt = 0;
	g_autoptr(GPtrArray) devs = NULL;
	g_autoptr(GPtrArray) remotes = NULL;
	g_autoptr(GString) str = g_string_new (NULL);

	/* metadata refreshed recently */
	if ((priv->flags & FWUPD_INSTALL_FLAG_FORCE) == 0) {
		guint64 age_oldest = 0;
		const guint64 age_limit_hours = 24;

		if (!fu_util_check_oldest_remote (priv, &age_oldest, error))
			return FALSE;
		if (age_oldest < 60 * 60 * age_limit_hours) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     /* TRANSLATORS: error message for a user who ran fwupdmgr refresh recently
				        %1 is an already translated timestamp such as 6 hours or 15 seconds */
				     "Firmware metadata last refresh: %s ago. "
				     "Use --force to refresh again.",
				     fu_util_time_to_str (age_oldest));
			return FALSE;
		}
	}

	remotes = fwupd_client_get_remotes (priv->client, NULL, error);
	if (remotes == NULL)
		return FALSE;
	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		if (!fwupd_remote_get_enabled (remote))
			continue;
		if (fwupd_remote_get_kind (remote) != FWUPD_REMOTE_KIND_DOWNLOAD)
			continue;
		download_remote_enabled = TRUE;
		g_print ("%s %s\n", _("Updating"), fwupd_remote_get_id (remote));
		if (!fwupd_client_refresh_remote (priv->client, remote,
						  priv->cancellable, error))
			return FALSE;
	}

	/* no web remote is declared; try to enable LVFS */
	if (!download_remote_enabled) {
		/* we don't want to ask anything */
		if (priv->no_remote_check) {
			g_debug ("skipping remote check");
			return TRUE;
		}

		if (!fu_util_download_metadata_enable_lvfs (priv, error))
			return FALSE;
	}

	/* get devices from daemon */
	devs = fwupd_client_get_devices (priv->client, NULL, error);
	if (devs == NULL)
		return FALSE;

	/* get results */
	for (guint i = 0; i < devs->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devs, i);
		if (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_SUPPORTED))
			devices_supported_cnt++;
	}

	/* TRANSLATORS: success message -- where 'metadata' is information
	 * about available firmware on the remote server */
	g_string_append (str, _("Successfully downloaded new metadata: "));

	/* TRANSLATORS: how many local devices can expect updates now */
	g_string_append_printf (str, ngettext ("%u local device supported",
					       "%u local devices supported",
					       devices_supported_cnt),
					       devices_supported_cnt);
	g_print ("%s\n", str->str);
	return TRUE;
}

static gboolean
fu_util_refresh (FuUtilPrivate *priv, gchar **values, GError **error)
{
	if (g_strv_length (values) == 0)
		return fu_util_download_metadata (priv, error);
	if (g_strv_length (values) != 3) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* open file */
	if (!fwupd_client_update_metadata (priv->client,
					   values[2],
					   values[0],
					   values[1],
					   NULL,
					   error))
		return FALSE;

	/* TRANSLATORS: success message -- the user can do this by-hand too */
	g_print ("%s\n", _("Successfully refreshed metadata manually"));
	return TRUE;
}

static gboolean
fu_util_get_results (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autofree gchar *tmp = NULL;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(FwupdDevice) rel = NULL;

	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	rel = fwupd_client_get_results (priv->client, fwupd_device_get_id (dev), NULL, error);
	if (rel == NULL)
		return FALSE;
	tmp = fu_util_device_to_string (rel, 0);
	g_print ("%s", tmp);
	return TRUE;
}

static gboolean
fu_util_get_releases (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(GPtrArray) rels = NULL;

	priv->filter_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	/* get the releases for this device */
	rels = fwupd_client_get_releases (priv->client, fwupd_device_get_id (dev), NULL, error);
	if (rels == NULL)
		return FALSE;

	if (rels->len == 0) {
		/* TRANSLATORS: no repositories to download from */
		g_print ("%s\n", _("No releases available"));
		return TRUE;
	}
	if (g_getenv ("FWUPD_VERBOSE") != NULL) {
		for (guint i = 0; i < rels->len; i++) {
			FwupdRelease *rel = g_ptr_array_index (rels, i);
			g_autofree gchar *tmp = fwupd_release_to_string (rel);
			g_print ("%s\n", tmp);
		}
	} else {
		g_autoptr(GNode) root = g_node_new (NULL);
		g_autofree gchar *title = fu_util_get_tree_title (priv);
		for (guint i = 0; i < rels->len; i++) {
			FwupdRelease *rel = g_ptr_array_index (rels, i);
			g_node_append_data (root, rel);
		}
		fu_util_print_tree (root, title);
	}

	return TRUE;
}

static FwupdRelease *
fu_util_prompt_for_release (FuUtilPrivate *priv, GPtrArray *rels, GError **error)
{
	FwupdRelease *rel;
	guint idx;

	/* nothing */
	if (rels->len == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No supported releases");
		return NULL;
	}

	/* exactly one */
	if (rels->len == 1) {
		rel = g_ptr_array_index (rels, 0);
		return g_object_ref (rel);
	}

	/* TRANSLATORS: get interactive prompt */
	g_print ("%s\n", _("Choose a release:"));
	/* TRANSLATORS: this is to abort the interactive prompt */
	g_print ("0.\t%s\n", _("Cancel"));
	for (guint i = 0; i < rels->len; i++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (rels, i);
		g_print ("%u.\t%s\n", i + 1, fwupd_release_get_version (rel_tmp));
	}
	idx = fu_util_prompt_for_number (rels->len);
	if (idx == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "Request canceled");
		return NULL;
	}
	rel = g_ptr_array_index (rels, idx - 1);
	return g_object_ref (rel);
}

static gboolean
fu_util_verify (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	priv->filter_include |= FWUPD_DEVICE_FLAG_CAN_VERIFY;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	if (!fwupd_client_verify (priv->client, fwupd_device_get_id (dev), NULL, error)) {
		g_prefix_error (error, "failed to verify %s: ", fu_device_get_name (dev));
		return FALSE;
	}
	/* TRANSLATORS: success message when user verified device checksums */
	g_print ("%s\n", _("Successfully verified device checksums"));

	return TRUE;
}

static gboolean
fu_util_unlock (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdDevice) dev = NULL;

	priv->filter_include |= FWUPD_DEVICE_FLAG_LOCKED;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	if (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN))
		priv->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
	if (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_NEEDS_REBOOT))
		priv->completion_flags |= FWUPD_DEVICE_FLAG_NEEDS_REBOOT;

	if (!fwupd_client_unlock (priv->client, fwupd_device_get_id (dev), NULL, error))
		return FALSE;

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_perhaps_refresh_remotes (FuUtilPrivate *priv, GError **error)
{
	guint64 age_oldest = 0;
	const guint64 age_limit_days = 30;

	/* we don't want to ask anything */
	if (priv->no_metadata_check) {
		g_debug ("skipping metadata check");
		return TRUE;
	}

	if (!fu_util_check_oldest_remote (priv, &age_oldest, NULL))
		return TRUE;

	/* metadata is new enough */
	if (age_oldest < 60 * 60 * 24 * age_limit_days)
		return TRUE;

	/* ask for permission */
	if (!priv->assume_yes) {
		/* TRANSLATORS: the metadata is very out of date; %u is a number > 1 */
		g_print (ngettext("Firmware metadata has not been updated for %u"
				  " day and may not be up to date.",
				  "Firmware metadata has not been updated for %u"
				  " days and may not be up to date.",
				  (gint) age_limit_days), (guint) age_limit_days);
		g_print ("\n\n");
		g_print ("%s (%s) [y|N]: ",
			 /* TRANSLATORS: ask the user if we can update the metadata */
			 _("Update now?"),
			 /* TRANSLATORS: metadata is downloaded from the Internet */
			 _("Requires internet connection"));
		if (!fu_util_prompt_for_boolean (FALSE))
			return TRUE;
	}

	/* downloads new metadata */
	return fu_util_download_metadata (priv, error);
}

static gboolean
fu_util_get_updates (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	gboolean supported = FALSE;
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autofree gchar *title = fu_util_get_tree_title (priv);
	gboolean no_updates_header = FALSE;
	gboolean latest_header = FALSE;

	/* are the remotes very old */
	if (!fu_util_perhaps_refresh_remotes (priv, error))
		return FALSE;

	/* handle both forms */
	if (g_strv_length (values) == 0) {
		devices = fwupd_client_get_devices (priv->client, NULL, error);
		if (devices == NULL)
			return FALSE;
	} else if (g_strv_length (values) == 1) {
		FwupdDevice *device = fu_util_get_device_by_id (priv, values[0], error);
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
	g_ptr_array_sort (devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;
		GNode *child;

		/* not going to have results, so save a D-Bus round-trip */
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
		supported = TRUE;

		/* get the releases for this device and filter for validity */
		rels = fwupd_client_get_upgrades (priv->client,
						  fwupd_device_get_id (dev),
						  NULL, &error_local);
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

		/* add all releases */
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel = g_ptr_array_index (rels, j);
			g_node_append_data (child, g_object_ref (rel));
		}
	}

	/* nag? */
	if (!fu_util_perhaps_show_unreported (priv, error))
		return FALSE;

	/* no devices supported by LVFS or all are filtered */
	if (!supported) {
		g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOTHING_TO_DO,
			     "No updatable devices");
		return FALSE;
	}
	/* no updates available */
	if (g_node_n_nodes (root, G_TRAVERSE_ALL) <= 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No updates available for remaining devices");
		return FALSE;
	}

	fu_util_print_tree (root, title);

	/* success */
	return TRUE;
}

static gboolean
fu_util_get_remotes (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GNode) root = g_node_new (NULL);
	g_autoptr(GPtrArray) remotes = NULL;
	g_autofree gchar *title = fu_util_get_tree_title (priv);

	remotes = fwupd_client_get_remotes (priv->client, NULL, error);
	if (remotes == NULL)
		return FALSE;

	if (remotes->len == 0) {
		/* TRANSLATORS: no repositories to download from */
		g_print ("%s\n", _("No remotes available"));
		return TRUE;
	}

	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote_tmp = g_ptr_array_index (remotes, i);
		g_node_append_data (root, remote_tmp);
	}
	fu_util_print_tree (root, title);

	return TRUE;
}

static gboolean
fu_util_prompt_warning_composite (FuUtilPrivate *priv,
				  FwupdDevice *dev,
				  FwupdRelease *rel,
				  GError **error)
{
	const gchar *rel_csum;
	g_autoptr(GPtrArray) devices = NULL;

	/* get the default checksum */
	rel_csum = fwupd_checksum_get_best (fwupd_release_get_checksums (rel));
	if (rel_csum == NULL) {
		g_debug ("no checksum for release!");
		return TRUE;
	}

	/* find other devices matching the composite ID and the release checksum */
	devices = fwupd_client_get_devices (priv->client, NULL, error);
	if (devices == NULL)
		return FALSE;
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev_tmp = g_ptr_array_index (devices, i);
		g_autoptr(GError) error_local = NULL;
		g_autoptr(GPtrArray) rels = NULL;

		/* not the parent device */
		if (g_strcmp0 (fwupd_device_get_id (dev),
			       fwupd_device_get_id (dev_tmp)) == 0)
			continue;

		/* not the same composite device */
		if (g_strcmp0 (fwupd_device_get_composite_id (dev),
			       fwupd_device_get_composite_id (dev_tmp)) != 0)
			continue;

		/* get releases */
		if (!fwupd_device_has_flag (dev_tmp, FWUPD_DEVICE_FLAG_UPDATABLE))
			continue;
		rels = fwupd_client_get_releases (priv->client,
						  fwupd_device_get_id (dev_tmp),
						  NULL, &error_local);
		if (rels == NULL) {
			g_debug ("ignoring: %s", error_local->message);
			continue;
		}

		/* do any releases match this checksum */
		for (guint j = 0; j < rels->len; j++) {
			FwupdRelease *rel_tmp = g_ptr_array_index (rels, j);
			if (fwupd_release_has_checksum (rel_tmp, rel_csum)) {
				if (!fu_util_prompt_warning (dev_tmp, rel_tmp,
							     fu_util_get_tree_title (priv),
							     error))
					return FALSE;
				break;
			}
		}
	}

	/* success */
	return TRUE;
}

static gboolean
fu_util_update_device_with_release (FuUtilPrivate *priv,
				    FwupdDevice *dev,
				    FwupdRelease *rel,
				    GError **error)
{
	if (!priv->no_safety_check && !priv->assume_yes) {
		if (!fu_util_prompt_warning_composite (priv, dev, rel, error))
			return FALSE;
		if (!fu_util_prompt_warning (dev, rel,
					     fu_util_get_tree_title (priv),
					     error))
			return FALSE;
		if (!fu_util_prompt_warning_composite (priv, dev, rel, error))
			return FALSE;
	}
	return fwupd_client_install_release2 (priv->client, dev, rel, priv->flags,
					      priv->download_flags,
					      priv->cancellable, error);
}

static gboolean
fu_util_maybe_send_reports (FuUtilPrivate *priv, const gchar *remote_id,
			    GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GError) error_local = NULL;
	if (remote_id == NULL) {
		g_debug ("not sending reports, no remote");
		return TRUE;
	}
	remote = fwupd_client_get_remote_by_id (priv->client,
						remote_id,
						NULL,
						error);
	if (remote == NULL)
		return FALSE;
	if (fwupd_remote_get_automatic_reports (remote)) {
		if (!fu_util_report_history (priv, NULL, &error_local))
			if (!g_error_matches (error_local,
					      FWUPD_ERROR,
					      FWUPD_ERROR_NOT_SUPPORTED))
				g_warning ("%s", error_local->message);
	}

	return TRUE;
}

static gboolean
fu_util_update_all (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	gboolean supported = FALSE;
	gboolean no_updates_header = FALSE;
	gboolean latest_header = FALSE;

	/* get devices from daemon */
	devices = fwupd_client_get_devices (priv->client, NULL, error);
	if (devices == NULL)
		return FALSE;
	priv->current_operation = FU_UTIL_OPERATION_UPDATE;
	g_signal_connect (priv->client, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);
	g_ptr_array_sort (devices, fu_util_sort_devices_by_flags_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *dev = g_ptr_array_index (devices, i);
		FwupdRelease *rel;
		const gchar *remote_id;
		g_autoptr(GPtrArray) rels = NULL;
		g_autoptr(GError) error_local = NULL;

		/* not going to have results, so save a D-Bus round-trip */
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
		supported = TRUE;

		/* get the releases for this device and filter for validity */
		rels = fwupd_client_get_upgrades (priv->client,
						  fwupd_device_get_id (dev),
						  NULL, &error_local);
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
		if (!fu_util_update_device_with_release (priv, dev, rel, error))
			return FALSE;

		fu_util_display_current_message (priv);

		/* send report if we're supposed to */
		remote_id = fwupd_release_get_remote_id (rel);
		if (!fu_util_maybe_send_reports (priv, remote_id, error))
			return FALSE;
	}

	/* no devices supported by LVFS or all are filtered */
	if (!supported) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No updatable devices");
		return FALSE;
	}

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_update_by_id (FuUtilPrivate *priv, const gchar *device_id, GError **error)
{
	FwupdRelease *rel;
	const gchar *remote_id;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(GPtrArray) rels = NULL;

	/* do not allow a partial device-id */
	dev = fu_util_get_device_by_id (priv, device_id, error);
	if (dev == NULL)
		return FALSE;

	/* get devices from daemon */
	priv->current_operation = FU_UTIL_OPERATION_UPDATE;
	g_signal_connect (priv->client, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);

	/* get the releases for this device and filter for validity */
	rels = fwupd_client_get_upgrades (priv->client,
					  fwupd_device_get_id (dev),
					  NULL, error);
	if (rels == NULL)
		return FALSE;
	rel = g_ptr_array_index (rels, 0);
	if (!fu_util_update_device_with_release (priv, dev, rel, error))
		return FALSE;
	fu_util_display_current_message (priv);

	/* send report if we're supposed to */
	remote_id = fwupd_release_get_remote_id (rel);
	if (!fu_util_maybe_send_reports (priv, remote_id, error))
		return FALSE;

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	/* the update needs the user to restart the computer */
	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
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

	if (g_strv_length (values) == 0)
		return fu_util_update_all (priv, error);
	if (g_strv_length (values) == 1)
		return fu_util_update_by_id (priv, values[0], error);
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INVALID_ARGS,
			     "Invalid arguments");
	return FALSE;
}

static gboolean
fu_util_remote_modify (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;
	if (g_strv_length (values) < 3) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* ensure the remote exists */
	remote = fwupd_client_get_remote_by_id (priv->client, values[0],
						priv->cancellable, error);
	if (remote == NULL)
		return FALSE;
	if (!fwupd_client_modify_remote (priv->client,
					 fwupd_remote_get_id (remote),
					 values[1], values[2],
					 priv->cancellable, error))
		return FALSE;

	/* TRANSLATORS: success message for a per-remote setting change */
	g_print ("%s\n", _("Successfully modified remote"));
	return TRUE;
}

static gboolean
fu_util_remote_enable (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;
	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}
	remote = fwupd_client_get_remote_by_id (priv->client, values[0],
						priv->cancellable, error);
	if (remote == NULL)
		return FALSE;
	if (!fu_util_modify_remote_warning (priv, remote, error))
		return FALSE;
	if (!fwupd_client_modify_remote (priv->client,
					 fwupd_remote_get_id (remote),
					 "Enabled", "true",
					 priv->cancellable, error))
		return FALSE;

	/* ask for permission to refresh */
	if (priv->no_remote_check ||
	    fwupd_remote_get_kind (remote) != FWUPD_REMOTE_KIND_DOWNLOAD) {
		/* TRANSLATORS: success message */
		g_print ("%s\n", _("Successfully enabled remote"));
		return TRUE;
	}
	if (!priv->assume_yes) {
		g_print ("%s (%s) [Y|n]: ",
			 /* TRANSLATORS: ask the user if we can update the metadata */
			 _("Do you want to refresh this remote now?"),
			 /* TRANSLATORS: metadata is downloaded from the Internet */
			 _("Requires internet connection"));
		if (!fu_util_prompt_for_boolean (TRUE)) {
			/* TRANSLATORS: success message */
			g_print ("%s\n", _("Successfully enabled remote"));
			return TRUE;
		}
	}
	if (!fwupd_client_refresh_remote (priv->client, remote,
					  priv->cancellable, error))
		return FALSE;

	/* TRANSLATORS: success message */
	g_print ("\n%s\n", _("Successfully enabled and refreshed remote"));
	return TRUE;
}

static gboolean
fu_util_remote_disable (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(FwupdRemote) remote = NULL;

	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* ensure the remote exists */
	remote = fwupd_client_get_remote_by_id (priv->client, values[0],
						priv->cancellable, error);
	if (remote == NULL)
		return FALSE;
	if (!fwupd_client_modify_remote (priv->client,
					 values[0], "Enabled", "false",
					 priv->cancellable, error))
		return FALSE;

	/* TRANSLATORS: success message */
	g_print ("%s\n", _("Successfully disabled remote"));
	return TRUE;
}

static gboolean
fu_util_downgrade (FuUtilPrivate *priv, gchar **values, GError **error)
{
	const gchar *remote_id;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;

	if (priv->flags & FWUPD_INSTALL_FLAG_ALLOW_REINSTALL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "--allow-reinstall is not supported for this command");
		return FALSE;
	}

	priv->filter_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	/* get the releases for this device and filter for validity */
	rels = fwupd_client_get_downgrades (priv->client,
					    fwupd_device_get_id (dev),
					    NULL, error);
	if (rels == NULL) {
		/* TRANSLATORS: message letting the user know no device downgrade available
		 * %1 is the device name */
		g_autofree gchar *downgrade_str = g_strdup_printf (_("No downgrades for %s"),
								   fwupd_device_get_name (dev));
		g_prefix_error (error, "%s: ", downgrade_str);
		return FALSE;
	}

	/* get the chosen release */
	rel = fu_util_prompt_for_release (priv, rels, error);
	if (rel == NULL)
		return FALSE;

	/* update the console if composite devices are also updated */
	priv->current_operation = FU_UTIL_OPERATION_DOWNGRADE;
	g_signal_connect (priv->client, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_OLDER;
	if (!fu_util_update_device_with_release (priv, dev, rel, error))
		return FALSE;

	fu_util_display_current_message (priv);

	/* send report if we're supposed to */
	remote_id = fwupd_release_get_remote_id (rel);
	if (!fu_util_maybe_send_reports (priv, remote_id, error))
		return FALSE;

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_reinstall (FuUtilPrivate *priv, gchar **values, GError **error)
{
	const gchar *remote_id;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(FwupdDevice) dev = NULL;

	priv->filter_include |= FWUPD_DEVICE_FLAG_SUPPORTED;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	/* try to lookup/match release from client */
	rels = fwupd_client_get_releases (priv->client, fwupd_device_get_id (dev),
					  NULL, error);
	if (rels == NULL)
		return FALSE;
	for (guint j = 0; j < rels->len; j++) {
		FwupdRelease *rel_tmp = g_ptr_array_index (rels, j);
		if (fu_common_vercmp_full (fwupd_release_get_version (rel_tmp),
					   fu_device_get_version (dev),
					   fwupd_device_get_version_format (dev)) == 0) {
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
	g_signal_connect (priv->client, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	if (!fu_util_update_device_with_release (priv, dev, rel, error))
		return FALSE;
	fu_util_display_current_message (priv);

	/* send report if we're supposed to */
	remote_id = fwupd_release_get_remote_id (rel);
	if (!fu_util_maybe_send_reports (priv, remote_id, error))
		return FALSE;

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
_g_str_equal0 (gconstpointer str1, gconstpointer str2)
{
	return g_strcmp0 (str1, str2) == 0;
}

static gboolean
fu_util_switch_branch (FuUtilPrivate *priv, gchar **values, GError **error)
{
	const gchar *remote_id;
	const gchar *branch;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GPtrArray) rels = NULL;
	g_autoptr(GPtrArray) branches = g_ptr_array_new_with_free_func (g_free);
	g_autoptr(FwupdDevice) dev = NULL;

	/* find the device and check it has multiple branches */
	priv->filter_include |= FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES;
	priv->filter_include |= FWUPD_DEVICE_FLAG_UPDATABLE;
	dev = fu_util_get_device_or_prompt (priv, values, error);
	if (dev == NULL)
		return FALSE;

	/* get all releases, including the alternate branch versions */
	rels = fwupd_client_get_releases (priv->client, fwupd_device_get_id (dev),
					  NULL, error);
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
	if (!fu_util_switch_branch_warning (dev, rel, priv->assume_yes, error))
		return FALSE;

	/* update the console if composite devices are also updated */
	priv->current_operation = FU_UTIL_OPERATION_INSTALL;
	g_signal_connect (priv->client, "device-changed",
			  G_CALLBACK (fu_util_update_device_changed_cb), priv);
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_REINSTALL;
	priv->flags |= FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH;
	if (!fu_util_update_device_with_release (priv, dev, rel, error))
		return FALSE;
	fu_util_display_current_message (priv);

	/* send report if we're supposed to */
	remote_id = fwupd_release_get_remote_id (rel);
	if (!fu_util_maybe_send_reports (priv, remote_id, error))
		return FALSE;

	/* we don't want to ask anything */
	if (priv->no_reboot_check) {
		g_debug ("skipping reboot check");
		return TRUE;
	}

	return fu_util_prompt_complete (priv->completion_flags, TRUE, error);
}

static gboolean
fu_util_activate (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	gboolean has_pending = FALSE;

	/* handle both forms */
	if (g_strv_length (values) == 0) {
		/* activate anything with _NEEDS_ACTIVATION */
		devices = fwupd_client_get_devices (priv->client, NULL, error);
		if (devices == NULL)
			return FALSE;
		for (guint i = 0; i < devices->len; i++) {
			FuDevice *device = g_ptr_array_index (devices, i);
			if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)) {
				has_pending = TRUE;
				break;
			}
		}
	} else if (g_strv_length (values) == 1) {
		FwupdDevice *device = fu_util_get_device_by_id (priv, values[0], error);
		if (device == NULL)
			return FALSE;
		devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
		g_ptr_array_add (devices, device);
		if (fwupd_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION))
			has_pending = TRUE;
	} else {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments");
		return FALSE;
	}

	/* nothing to do */
	if (!has_pending) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No firmware to activate");
		return FALSE;
	}

	/* activate anything with _NEEDS_ACTIVATION */
	/* order by device priority */
	g_ptr_array_sort (devices, fu_util_device_order_sort_cb);
	for (guint i = 0; i < devices->len; i++) {
		FwupdDevice *device = g_ptr_array_index (devices, i);
		if (!fu_util_filter_device (priv, device))
			continue;
		if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION))
			continue;
		/* TRANSLATORS: shown when shutting down to switch to the new version */
		g_print ("%s %s…\n", _("Activating firmware update for"),
			 fwupd_device_get_name (device));
		if (!fwupd_client_activate (priv->client, NULL,
					    fwupd_device_get_id (device), error))
			return FALSE;
	}

	/* TRANSLATORS: success message -- where activation is making the new
	 * firmware take effect, usually after updating offline */
	g_print ("%s\n", _("Successfully activated all devices"));
	return TRUE;
}

static gboolean
fu_util_set_approved_firmware (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_auto(GStrv) checksums = NULL;

	/* check args */
	if (g_strv_length (values) != 1) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: list of checksums expected");
		return FALSE;
	}

	/* call into daemon */
	checksums = g_strsplit (values[0], ",", -1);
	return fwupd_client_set_approved_firmware (priv->client,
						   checksums,
						   priv->cancellable,
						   error);
}

static gboolean
fu_util_get_approved_firmware (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_auto(GStrv) checksums = NULL;

	/* check args */
	if (g_strv_length (values) != 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: none expected");
		return FALSE;
	}

	/* call into daemon */
	checksums = fwupd_client_get_approved_firmware (priv->client,
							priv->cancellable,
							error);
	if (checksums == NULL)
		return FALSE;
	if (g_strv_length (checksums) == 0) {
		/* TRANSLATORS: approved firmware has been checked by
		 * the domain administrator */
		g_print ("%s\n", _("There is no approved firmware."));
	} else {
		/* TRANSLATORS: approved firmware has been checked by
		 * the domain administrator */
		g_print ("%s\n", ngettext ("Approved firmware:",
					   "Approved firmware:",
					   g_strv_length (checksums)));
		for (guint i = 0; checksums[i] != NULL; i++)
			g_print (" * %s\n", checksums[i]);
	}
	return TRUE;
}

static gboolean
fu_util_modify_config (FuUtilPrivate *priv, gchar **values, GError **error)
{
	/* check args */
	if (g_strv_length (values) != 2) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_ARGS,
				     "Invalid arguments: KEY VALUE expected");
		return FALSE;
	}
	if (!fwupd_client_modify_config (priv->client,
					 values[0], values[1],
					 priv->cancellable,
					 error))
		return FALSE;
	if (!priv->assume_yes) {
		g_print ("%s [Y|n]: ",
			/* TRANSLATORS: configuration changes only take effect on restart */
			 _("Restart the daemon to make the change effective?"));
		if (!fu_util_prompt_for_boolean (FALSE))
			return TRUE;
	}
#ifdef HAVE_SYSTEMD
	if (!fu_systemd_unit_stop (fu_util_get_systemd_unit (), error))
		return FALSE;
#endif
	/* TRANSLATORS: success message -- a per-system setting value */
	g_print ("%s\n", _("Successfully modified configuration value"));
	return TRUE;
}

static FwupdRemote *
fu_util_get_remote_with_security_report_uri (FuUtilPrivate *priv, GError **error)
{
	g_autoptr(GPtrArray) remotes = NULL;

	/* get all remotes */
	remotes = fwupd_client_get_remotes (priv->client, NULL, error);
	if (remotes == NULL)
		return NULL;

	for (guint i = 0; i < remotes->len; i++) {
		FwupdRemote *remote = g_ptr_array_index (remotes, i);
		if (!fwupd_remote_get_enabled (remote))
			continue;
		if (fwupd_remote_get_security_report_uri (remote) != NULL)
			return g_object_ref (remote);
	}

	/* failed */
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "No remotes specified SecurityReportURI");
	return NULL;
}

static gboolean
fu_util_upload_security (FuUtilPrivate *priv, GPtrArray *attrs, GError **error)
{
	GHashTableIter iter;
	const gchar *key;
	const gchar *value;
	g_autofree gchar *data = NULL;
	g_autofree gchar *sig = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GBytes) upload_response = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GHashTable) metadata = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;

	/* can we find a remote with a security attr */
	remote = fu_util_get_remote_with_security_report_uri (priv, &error_local);
	if (remote == NULL) {
		g_debug ("failed to find suitable remote: %s", error_local->message);
		return TRUE;
	}
	if (!priv->assume_yes &&
	    !fwupd_remote_get_automatic_security_reports (remote)) {
		g_autofree gchar *tmp = NULL;
		/* TRANSLATORS: ask the user to share, %s is something like:
		 * "Linux Vendor Firmware Service" */
		tmp = g_strdup_printf ("Upload these anonymous results to the %s to help other users?",
				       fwupd_remote_get_title (remote));

		g_print ("\n%s [y|N]: ", tmp);
		if (!fu_util_prompt_for_boolean (FALSE)) {
			g_print ("%s [Y|n]: ",
				 /* TRANSLATORS: stop nagging the user */
				 _("Ask again next time?"));
			if (!fu_util_prompt_for_boolean (TRUE)) {
				if (!fwupd_client_modify_remote (priv->client,
								 fwupd_remote_get_id (remote),
								 "SecurityReportURI", "",
								 NULL, error))
					return FALSE;
			}
			return TRUE;
		}
	}

	/* get metadata */
	metadata = fwupd_client_get_report_metadata (priv->client,
						     priv->cancellable,
						     error);
	if (metadata == NULL)
		return FALSE;

	/* create header */
	builder = json_builder_new ();
	json_builder_begin_object (builder);
	json_builder_set_member_name (builder, "ReportVersion");
	json_builder_add_int_value (builder, 2);
	json_builder_set_member_name (builder, "MachineId");
	json_builder_add_string_value (builder, fwupd_client_get_host_machine_id (priv->client));

	/* this is system metadata not stored in the database */
	json_builder_set_member_name (builder, "Metadata");
	json_builder_begin_object (builder);

	g_hash_table_iter_init (&iter, metadata);
	while (g_hash_table_iter_next (&iter, (gpointer *) &key, (gpointer *) &value)) {
		json_builder_set_member_name (builder, key);
		json_builder_add_string_value (builder, value);
	}
	json_builder_set_member_name (builder, "HostSecurityId");
	json_builder_add_string_value (builder, fwupd_client_get_host_security_id (priv->client));
	json_builder_end_object (builder);

	/* attrs */
	json_builder_set_member_name (builder, "SecurityAttributes");
	json_builder_begin_array (builder);
	for (guint i = 0; i < attrs->len; i++) {
		FwupdSecurityAttr *attr = g_ptr_array_index (attrs, i);
		json_builder_begin_object (builder);
		fwupd_security_attr_to_json (attr, builder);
		json_builder_end_object (builder);
	}
	json_builder_end_array (builder);
	json_builder_end_object (builder);

	/* export as a string */
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	data = json_generator_to_data (json_generator, NULL);
	if (data == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "Failed to convert to JSON string");
		return FALSE;
	}

	/* self sign data */
	if (priv->sign) {
		sig = fwupd_client_self_sign (priv->client, data,
					      FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP,
					      priv->cancellable, error);
		if (sig == NULL)
			return FALSE;
	}

	/* ask for permission */
	if (!priv->assume_yes &&
	    !fwupd_remote_get_automatic_security_reports (remote)) {
		fu_util_print_data (_("Target"), fwupd_remote_get_security_report_uri (remote));
		fu_util_print_data (_("Payload"), data);
		if (sig != NULL)
			fu_util_print_data (_("Signature"), sig);
		g_print ("%s [Y|n]: ", _("Proceed with upload?"));
		if (!fu_util_prompt_for_boolean (TRUE)) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_PERMISSION_DENIED,
					     "User declined action");
			return FALSE;
		}
	}

	/* POST request */
	upload_response = fwupd_client_upload_bytes (priv->client,
						     fwupd_remote_get_security_report_uri (remote),
						     data, sig,
						     FWUPD_CLIENT_UPLOAD_FLAG_ALWAYS_MULTIPART,
						     priv->cancellable, error);
	if (upload_response == NULL)
		return FALSE;

	/* TRANSLATORS: success, so say thank you to the user */
	g_print ("%s\n", "Host Security ID attributes uploaded successfully, thanks!");

	/* as this worked, ask if the user want to do this every time */
	if (!fwupd_remote_get_automatic_security_reports (remote)) {
		g_print ("%s [y|N]: ",
			 /* TRANSLATORS: can we JFDI? */
			 _("Automatically upload every time?"));
		if (fu_util_prompt_for_boolean (FALSE)) {
			if (!fwupd_client_modify_remote (priv->client,
							 fwupd_remote_get_id (remote),
							 "AutomaticSecurityReports", "true",
							 NULL, error))
				return FALSE;
		}
	}

	return TRUE;
}

static gboolean
fu_util_security (FuUtilPrivate *priv, gchar **values, GError **error)
{
	FuSecurityAttrToStringFlags flags = FU_SECURITY_ATTR_TO_STRING_FLAG_NONE;
	g_autoptr(GPtrArray) attrs = NULL;
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

	/* TRANSLATORS: this is a string like 'HSI:2-U' */
	g_print ("%s \033[1m%s\033[0m\n", _("Host Security ID:"),
		 fwupd_client_get_host_security_id (priv->client));

	/* print the "why" */
	attrs = fwupd_client_get_host_security_attrs (priv->client,
						      priv->cancellable,
						      error);
	if (attrs == NULL)
		return FALSE;

	/* show or hide different elements */
	if (priv->show_all) {
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_OBSOLETES;
		flags |= FU_SECURITY_ATTR_TO_STRING_FLAG_SHOW_URLS;
	}
	str = fu_util_security_attrs_to_string (attrs, flags);
	g_print ("%s\n", str);

	/* opted-out */
	if (priv->no_unreported_check)
		return TRUE;

	/* upload, with confirmation */
	return fu_util_upload_security (priv, attrs, error);
}

static void
fu_util_ignore_cb (const gchar *log_domain, GLogLevelFlags log_level,
		   const gchar *message, gpointer user_data)
{
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
	if (priv->client != NULL)
		g_object_unref (priv->client);
	if (priv->current_device != NULL)
		g_object_unref (priv->current_device);
	g_free (priv->current_message);
	g_main_context_unref (priv->main_ctx);
	g_object_unref (priv->cancellable);
	g_object_unref (priv->progressbar);
	g_option_context_free (priv->context);
	g_free (priv);
}

static gboolean
fu_util_check_daemon_version (FuUtilPrivate *priv, GError **error)
{
	const gchar *daemon = fwupd_client_get_daemon_version (priv->client);

	if (daemon == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     /* TRANSLATORS: error message */
				     _("Unable to connect to service"));
		return FALSE;
	}

	if (g_strcmp0 (daemon, SOURCE_VERSION) != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     /* TRANSLATORS: error message */
			     _("Unsupported daemon version %s, client version is %s"),
			     daemon, SOURCE_VERSION);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_util_check_polkit_actions (GError **error)
{
#ifdef HAVE_POLKIT
	g_autofree gchar *directory = fu_common_get_path (FU_PATH_KIND_POLKIT_ACTIONS);
	g_autofree gchar *filename = g_build_filename (directory,
						       "org.freedesktop.fwupd.policy",
						       NULL);
	if (!g_file_test (filename, G_FILE_TEST_IS_REGULAR)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_AUTH_FAILED,
				     "PolicyKit files are missing, see https://github.com/fwupd/fwupd/wiki/PolicyKit-files-are-missing");
		return FALSE;
	}
#endif

	return TRUE;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuUtilPrivate, fu_util_private_free)
#pragma clang diagnostic pop

static gchar *
fu_util_get_history_checksum (FuUtilPrivate *priv, GError **error)
{
	const gchar *csum;
	g_autoptr(FwupdDevice) device = NULL;
	g_autoptr(FwupdRelease) release = NULL;
	g_autoptr(GPtrArray) devices = NULL;

	devices = fwupd_client_get_history (priv->client, NULL, error);
	if (devices == NULL)
		return NULL;
	device = fu_util_prompt_for_device (priv, devices, error);
	if (device == NULL)
		return NULL;
	release = fu_util_prompt_for_release (priv, fwupd_device_get_releases (device), error);
	if (release == NULL)
		return NULL;
	csum = fwupd_checksum_get_best (fwupd_release_get_checksums (release));
	if (csum == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     "No suitable checksums");
		return NULL;
	}
	return g_strdup (csum);
}

static gboolean
fu_util_block_firmware (FuUtilPrivate *priv, gchar **values, GError **error)
{
	guint idx = 0;
	g_autofree gchar *csum = NULL;
	g_auto(GStrv) csums_new = NULL;
	g_auto(GStrv) csums = NULL;

	/* get existing checksums */
	csums = fwupd_client_get_blocked_firmware (priv->client, priv->cancellable, error);
	if (csums == NULL)
		return FALSE;

	/* get new value */
	if (g_strv_length (values) == 0) {
		csum = fu_util_get_history_checksum (priv, error);
		if (csum == NULL)
			return FALSE;
	} else {
		csum = g_strdup (values[0]);
	}

	/* ensure it's not already there */
	if (g_strv_contains ((const gchar * const *) csums, csum)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     /* TRANSLATORS: user selected something not possible */
				     _("Firmware is already blocked"));
		return FALSE;
	}

	/* TRANSLATORS: we will not offer this firmware to the user */
	g_print ("%s %s\n", _("Blocking firmware:"), csum);

	/* remove it from the new list */
	csums_new = g_new0 (gchar *, g_strv_length (csums) + 2);
	for (guint i = 0; csums[i] != NULL; i++) {
		if (g_strcmp0 (csums[i], csum) != 0)
			csums_new[idx++] = g_strdup (csums[i]);
	}
	csums_new[idx] = g_strdup (csum);
	return fwupd_client_set_blocked_firmware (priv->client, csums_new,
						  priv->cancellable, error);
}

static gboolean
fu_util_unblock_firmware (FuUtilPrivate *priv, gchar **values, GError **error)
{
	guint idx = 0;
	g_auto(GStrv) csums = NULL;
	g_auto(GStrv) csums_new = NULL;
	g_autofree gchar *csum = NULL;

	/* get existing checksums */
	csums = fwupd_client_get_blocked_firmware (priv->client, priv->cancellable, error);
	if (csums == NULL)
		return FALSE;

	/* empty list */
	if (g_strv_length (csums) == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     /* TRANSLATORS: nothing to show */
				     _("There are no blocked firmware files"));
		return FALSE;
	}

	/* get new value */
	if (g_strv_length (values) == 0) {
		csum = fu_util_get_history_checksum (priv, error);
		if (csum == NULL)
			return FALSE;
	} else {
		csum = g_strdup (values[0]);
	}

	/* ensure it's there */
	if (!g_strv_contains ((const gchar * const *) csums, csum)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOTHING_TO_DO,
				     /* TRANSLATORS: user selected something not possible */
				     _("Firmware is not already blocked"));
		return FALSE;
	}

	/* TRANSLATORS: we will now offer this firmware to the user */
	g_print ("%s %s\n", _("Unblocking firmware:"), csum);

	/* remove it from the new list */
	csums_new = g_new0 (gchar *, g_strv_length (csums));
	for (guint i = 0; csums[i] != NULL; i++) {
		if (g_strcmp0 (csums[i], csum) != 0)
			csums_new[idx++] = g_strdup (csums[i]);
	}
	return fwupd_client_set_blocked_firmware (priv->client, csums_new,
						  priv->cancellable, error);
}

static gboolean
fu_util_get_blocked_firmware (FuUtilPrivate *priv, gchar **values, GError **error)
{
	g_auto(GStrv) csums = NULL;

	/* get checksums */
	csums = fwupd_client_get_blocked_firmware (priv->client, priv->cancellable, error);
	if (csums == NULL)
		return FALSE;

	/* empty list */
	if (g_strv_length (csums) == 0) {
		/* TRANSLATORS: nothing to show */
		g_print ("%s\n", _("There are no blocked firmware files"));
		return TRUE;
	}

	/* TRANSLATORS: there follows a list of hashes */
	g_print ("%s\n", _("Blocked firmware files:"));
	for (guint i = 0; csums[i] != NULL; i++) {
		g_print ("%u.\t%s\n", i + 1, csums[i]);
	}

	/* success */
	return TRUE;
}

static void
fu_util_show_plugin_warnings (FuUtilPrivate *priv)
{
	FwupdPluginFlags flags = FWUPD_PLUGIN_FLAG_NONE;
	g_autoptr(GPtrArray) plugins = NULL;

	/* get plugins from daemon, ignoring if the daemon is too old */
	plugins = fwupd_client_get_plugins (priv->client, NULL, NULL);
	if (plugins == NULL)
		return;

	/* get a superset so we do not show the same message more than once */
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
		tmp = fu_util_plugin_flag_to_string (flag);
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

int
main (int argc, char *argv[])
{
	gboolean force = FALSE;
	gboolean allow_branch_switch = FALSE;
	gboolean allow_older = FALSE;
	gboolean allow_reinstall = FALSE;
	gboolean enable_ipfs = FALSE;
	gboolean ignore_power = FALSE;
	gboolean is_interactive = TRUE;
	gboolean no_history = FALSE;
	gboolean offline = FALSE;
	gboolean ret;
	gboolean verbose = FALSE;
	gboolean version = FALSE;
	g_autoptr(FuUtilPrivate) priv = g_new0 (FuUtilPrivate, 1);
	g_autoptr(GDateTime) dt_now = g_date_time_new_now_utc ();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) cmd_array = fu_util_cmd_array_new ();
	g_autofree gchar *cmd_descriptions = NULL;
	g_autofree gchar *filter = NULL;
	const GOptionEntry options[] = {
		{ "verbose", 'v', 0, G_OPTION_ARG_NONE, &verbose,
			/* TRANSLATORS: command line option */
			_("Show extra debugging information"), NULL },
		{ "version", '\0', 0, G_OPTION_ARG_NONE, &version,
			/* TRANSLATORS: command line option */
			_("Show client and daemon versions"), NULL },
		{ "offline", '\0', 0, G_OPTION_ARG_NONE, &offline,
			/* TRANSLATORS: command line option */
			_("Schedule installation for next reboot when possible"), NULL },
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
		{ "assume-yes", 'y', 0, G_OPTION_ARG_NONE, &priv->assume_yes,
			/* TRANSLATORS: command line option */
			_("Answer yes to all questions"), NULL },
		{ "sign", '\0', 0, G_OPTION_ARG_NONE, &priv->sign,
			/* TRANSLATORS: command line option */
			_("Sign the uploaded data with the client certificate"), NULL },
		{ "no-unreported-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_unreported_check,
			/* TRANSLATORS: command line option */
			_("Do not check for unreported history"), NULL },
		{ "no-metadata-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_metadata_check,
			/* TRANSLATORS: command line option */
			_("Do not check for old metadata"), NULL },
		{ "no-remote-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_remote_check,
			/* TRANSLATORS: command line option */
			_("Do not check if download remotes should be enabled"), NULL },
		{ "no-reboot-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_reboot_check,
			/* TRANSLATORS: command line option */
			_("Do not check or prompt for reboot after update"), NULL },
		{ "no-safety-check", '\0', 0, G_OPTION_ARG_NONE, &priv->no_safety_check,
			/* TRANSLATORS: command line option */
			_("Do not perform device safety checks"), NULL },
		{ "no-history", '\0', 0, G_OPTION_ARG_NONE, &no_history,
			/* TRANSLATORS: command line option */
			_("Do not write to the history database"), NULL },
		{ "show-all", '\0', 0, G_OPTION_ARG_NONE, &priv->show_all,
			/* TRANSLATORS: command line option */
			_("Show all results"), NULL },
		{ "show-all-devices", '\0', G_OPTION_FLAG_HIDDEN, G_OPTION_ARG_NONE, &priv->show_all,
			/* TRANSLATORS: command line option */
			_("Show devices that are not updatable"), NULL },
		{ "disable-ssl-strict", '\0', 0, G_OPTION_ARG_NONE, &priv->disable_ssl_strict,
			/* TRANSLATORS: command line option */
			_("Ignore SSL strict checks when downloading files"), NULL },
		{ "ipfs", '\0', 0, G_OPTION_ARG_NONE, &enable_ipfs,
			/* TRANSLATORS: command line option */
			_("Only use IPFS when downloading files"), NULL },
		{ "filter", '\0', 0, G_OPTION_ARG_STRING, &filter,
			/* TRANSLATORS: command line option */
			_("Filter with a set of device flags using a ~ prefix to "
			  "exclude, e.g. 'internal,~needs-reboot'"), NULL },
		{ "ignore-power", '\0', 0, G_OPTION_ARG_NONE, &ignore_power,
			/* TRANSLATORS: command line option */
			_("Ignore requirement of external power source"), NULL },
		{ NULL}
	};

	setlocale (LC_ALL, "");

	bindtextdomain (GETTEXT_PACKAGE, FWUPD_LOCALEDIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);

	/* ensure D-Bus errors are registered */
	fwupd_error_quark ();

	/* create helper object */
	priv->main_ctx = g_main_context_new ();
	priv->progressbar = fu_progressbar_new ();
	fu_progressbar_set_main_context (priv->progressbar, priv->main_ctx);

	/* add commands */
	fu_util_cmd_array_add (cmd_array,
		     "get-devices,get-topology",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Get all devices that support firmware updates"),
		     fu_util_get_devices);
	fu_util_cmd_array_add (cmd_array,
		     "get-history",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Show history of firmware updates"),
		     fu_util_get_history);
	fu_util_cmd_array_add (cmd_array,
		     "clear-history",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Erase all firmware update history"),
		     fu_util_clear_history);
	fu_util_cmd_array_add (cmd_array,
		     "report-history",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Share firmware history with the developers"),
		     fu_util_report_history);
	fu_util_cmd_array_add (cmd_array,
		     "install",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILE [DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Install a firmware file on this hardware"),
		     fu_util_install);
	fu_util_cmd_array_add (cmd_array,
		     "get-details",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("FILE"),
		     /* TRANSLATORS: command description */
		     _("Gets details about a firmware file"),
		     fu_util_get_details);
	fu_util_cmd_array_add (cmd_array,
		     "get-updates,get-upgrades",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Gets the list of updates for connected hardware"),
		     fu_util_get_updates);
	fu_util_cmd_array_add (cmd_array,
		     "update,upgrade",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Updates all firmware to latest versions available"),
		     fu_util_update);
	fu_util_cmd_array_add (cmd_array,
		     "verify",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Checks cryptographic hash matches firmware"),
		     fu_util_verify);
	fu_util_cmd_array_add (cmd_array,
		     "unlock",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("DEVICE-ID|GUID"),
		     /* TRANSLATORS: command description */
		     _("Unlocks the device for firmware access"),
		     fu_util_unlock);
	fu_util_cmd_array_add (cmd_array,
		     "clear-results",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("DEVICE-ID|GUID"),
		     /* TRANSLATORS: command description */
		     _("Clears the results from the last update"),
		     fu_util_clear_results);
	fu_util_cmd_array_add (cmd_array,
		     "clear-offline",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Clears any updates scheduled to be updated offline"),
		     fu_util_clear_offline);
	fu_util_cmd_array_add (cmd_array,
		     "get-results",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("DEVICE-ID|GUID"),
		     /* TRANSLATORS: command description */
		     _("Gets the results from the last update"),
		     fu_util_get_results);
	fu_util_cmd_array_add (cmd_array,
		     "get-releases",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Gets the releases for a device"),
		     fu_util_get_releases);
	fu_util_cmd_array_add (cmd_array,
		     "get-remotes",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Gets the configured remotes"),
		     fu_util_get_remotes);
	fu_util_cmd_array_add (cmd_array,
		     "downgrade",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Downgrades the firmware on a device"),
		     fu_util_downgrade);
	fu_util_cmd_array_add (cmd_array,
		     "refresh",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[FILE FILE_SIG REMOTE-ID]"),
		     /* TRANSLATORS: command description */
		     _("Refresh metadata from remote server"),
		     fu_util_refresh);
	fu_util_cmd_array_add (cmd_array,
		     "verify-update",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Update the stored cryptographic hash with current ROM contents"),
		     fu_util_verify_update);
	fu_util_cmd_array_add (cmd_array,
		     "modify-remote",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("REMOTE-ID KEY VALUE"),
		     /* TRANSLATORS: command description */
		     _("Modifies a given remote"),
		     fu_util_remote_modify);
	fu_util_cmd_array_add (cmd_array,
		     "enable-remote",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("REMOTE-ID"),
		     /* TRANSLATORS: command description */
		     _("Enables a given remote"),
		     fu_util_remote_enable);
	fu_util_cmd_array_add (cmd_array,
		     "disable-remote",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("REMOTE-ID"),
		     /* TRANSLATORS: command description */
		     _("Disables a given remote"),
		     fu_util_remote_disable);
	fu_util_cmd_array_add (cmd_array,
		     "activate",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Activate devices"),
		     fu_util_activate);
	fu_util_cmd_array_add (cmd_array,
		     "get-approved-firmware",
		     NULL,
		     /* TRANSLATORS: firmware approved by the admin */
		     _("Gets the list of approved firmware"),
		     fu_util_get_approved_firmware);
	fu_util_cmd_array_add (cmd_array,
		     "set-approved-firmware",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("CHECKSUM1[,CHECKSUM2][,CHECKSUM3]"),
		     /* TRANSLATORS: firmware approved by the admin */
		     _("Sets the list of approved firmware"),
		     fu_util_set_approved_firmware);
	fu_util_cmd_array_add (cmd_array,
		     "modify-config",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("KEY,VALUE"),
		     /* TRANSLATORS: sets something in daemon.conf */
		     _("Modifies a daemon configuration value"),
		     fu_util_modify_config);
	fu_util_cmd_array_add (cmd_array,
		     "reinstall",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID]"),
		     /* TRANSLATORS: command description */
		     _("Reinstall current firmware on the device"),
		     fu_util_reinstall);
	fu_util_cmd_array_add (cmd_array,
		     "switch-branch",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[DEVICE-ID|GUID] [BRANCH]"),
		     /* TRANSLATORS: command description */
		     _("Switch the firmware branch on the device"),
		     fu_util_switch_branch);
	fu_util_cmd_array_add (cmd_array,
		     "security",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Gets the host security attributes"),
		     fu_util_security);
	fu_util_cmd_array_add (cmd_array,
		     "block-firmware",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[CHECKSUM]"),
		     /* TRANSLATORS: command description */
		     _("Blocks a specific firmware from being installed"),
		     fu_util_block_firmware);
	fu_util_cmd_array_add (cmd_array,
		     "unblock-firmware",
		     /* TRANSLATORS: command argument: uppercase, spaces->dashes */
		     _("[CHECKSUM]"),
		     /* TRANSLATORS: command description */
		     _("Unblocks a specific firmware from being installed"),
		     fu_util_unblock_firmware);
	fu_util_cmd_array_add (cmd_array,
		     "get-blocked-firmware",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Gets the list of blocked firmware"),
		     fu_util_get_blocked_firmware);
	fu_util_cmd_array_add (cmd_array,
		     "get-plugins",
		     NULL,
		     /* TRANSLATORS: command description */
		     _("Get all enabled plugins registered with the system"),
		     fu_util_get_plugins);

	/* do stuff on ctrl+c */
	priv->cancellable = g_cancellable_new ();
#ifdef HAVE_GIO_UNIX
	g_unix_signal_add_full (G_PRIORITY_DEFAULT,
				SIGINT, fu_util_sigint_cb,
				priv, NULL);
#endif

	/* sort by command name */
	fu_util_cmd_array_sort (cmd_array);

	/* get a list of the commands */
	priv->context = g_option_context_new (NULL);
	cmd_descriptions = fu_util_cmd_array_to_string (cmd_array);
	g_option_context_set_summary (priv->context, cmd_descriptions);
	g_option_context_set_description (priv->context,
		/* TRANSLATORS: CLI description */
		_("This tool allows an administrator to query and control the "
		  "fwupd daemon, allowing them to perform actions such as "
		  "installing or downgrading firmware."));

	/* TRANSLATORS: program name */
	g_set_application_name (_("Firmware Utility"));
	g_option_context_add_main_entries (priv->context, options, NULL);
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

	/* this doesn't have to be precise (e.g. using the build-year) as we just
	 * want to check the clock is not set to the default of 1970-01-01... */
	if (g_date_time_get_year (dt_now) < 2021) {
		g_autofree gchar *fmt = NULL;
		/* TRANSLATORS: this is a prefix on the console */
		fmt = fu_util_term_format (_("WARNING:"), FU_UTIL_TERM_COLOR_RED);
		/* TRANSLATORS: try to help */
		g_printerr ("%s %s\n", fmt, _("The system clock has not been set "
					      "correctly and downloading files may fail."));
	}

	/* non-TTY consoles cannot answer questions */
	if (isatty (fileno (stdout)) == 0) {
		is_interactive = FALSE;
		priv->no_unreported_check = TRUE;
		priv->no_metadata_check = TRUE;
		priv->no_reboot_check = TRUE;
		priv->no_safety_check = TRUE;
		priv->no_remote_check = TRUE;
		fu_progressbar_set_interactive (priv->progressbar, FALSE);
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

	/* set verbose? */
	if (verbose) {
		g_setenv ("G_MESSAGES_DEBUG", "all", FALSE);
		g_setenv ("FWUPD_VERBOSE", "1", FALSE);
	} else {
		g_log_set_handler (G_LOG_DOMAIN, G_LOG_LEVEL_DEBUG,
				   fu_util_ignore_cb, NULL);
	}

	/* set flags */
	if (offline)
		priv->flags |= FWUPD_INSTALL_FLAG_OFFLINE;
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
	if (no_history)
		priv->flags |= FWUPD_INSTALL_FLAG_NO_HISTORY;
	if (ignore_power)
		priv->flags |= FWUPD_INSTALL_FLAG_IGNORE_POWER;

	/* use IPFS for metadata and firmware *only* if specified */
	if (enable_ipfs)
		priv->download_flags |= FWUPD_CLIENT_DOWNLOAD_FLAG_ONLY_IPFS;

#ifdef HAVE_POLKIT
	/* start polkit tty agent to listen for password requests */
	if (is_interactive) {
		g_autoptr(GError) error_polkit = NULL;
		if (!fu_polkit_agent_open (&error_polkit)) {
			g_printerr ("Failed to open polkit agent: %s\n",
				    error_polkit->message);
		}
	}
#endif

	/* connect to the daemon */
	priv->client = fwupd_client_new ();
	fwupd_client_set_main_context (priv->client, priv->main_ctx);
	g_signal_connect (priv->client, "notify::percentage",
			  G_CALLBACK (fu_util_client_notify_cb), priv);
	g_signal_connect (priv->client, "notify::status",
			  G_CALLBACK (fu_util_client_notify_cb), priv);

	/* just show versions and exit */
	if (version) {
		g_autofree gchar *version_str = fu_util_get_versions();
		g_print ("%s\n", version_str);
		if (!fwupd_client_connect (priv->client, priv->cancellable, &error)) {
			g_printerr ("Failed to connect to daemon: %s\n",
				    error->message);
			return EXIT_FAILURE;
		}
		g_print ("daemon version:\t%s\n",
			 fwupd_client_get_daemon_version (priv->client));
		return EXIT_SUCCESS;
	}

	/* show a warning if the daemon is tainted */
	if (!fwupd_client_connect (priv->client, priv->cancellable, &error)) {
		g_printerr ("Failed to connect to daemon: %s\n",
			    error->message);
		return EXIT_FAILURE;
	}
	if (fwupd_client_get_tainted (priv->client)) {
		g_autofree gchar *fmt = NULL;
		/* TRANSLATORS: this is a prefix on the console */
		fmt = fu_util_term_format (_("WARNING:"), FU_UTIL_TERM_COLOR_RED);
		g_printerr ("%s %s\n",
			    fmt,
			    /* TRANSLATORS: the user is SOL for support... */
			    _("The daemon has loaded 3rd party code and "
			      "is no longer supported by the upstream developers!"));
	}

	/* show user-visible warnings from the plugins */
	fu_util_show_plugin_warnings (priv);

	/* show any unsupported warnings */
	fu_util_show_unsupported_warn ();

	/* we know the runtime daemon version now */
	fwupd_client_set_user_agent_for_package (priv->client, "fwupdmgr", PACKAGE_VERSION);

	/* check that we have at least this version daemon running */
	if ((priv->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    !fu_util_check_daemon_version (priv, &error)) {
		g_printerr ("%s\n", error->message);
		return EXIT_FAILURE;
	}

#ifdef HAVE_SYSTEMD
	/* make sure the correct daemon is in use */
	if ((priv->flags & FWUPD_INSTALL_FLAG_FORCE) == 0 &&
	    !fwupd_client_get_daemon_interactive (priv->client) &&
	    !fu_util_using_correct_daemon (&error)) {
		g_printerr ("%s\n", error->message);
		return EXIT_FAILURE;
	}
#endif

	/* make sure polkit actions were installed */
	if (!fu_util_check_polkit_actions (&error)) {
		g_printerr ("%s\n", error->message);
		return EXIT_FAILURE;
	}

	/* send our implemented feature set */
	if (is_interactive) {
		if (!fwupd_client_set_feature_flags (priv->client,
						     FWUPD_FEATURE_FLAG_CAN_REPORT |
						     FWUPD_FEATURE_FLAG_SWITCH_BRANCH |
						     FWUPD_FEATURE_FLAG_UPDATE_ACTION |
						     FWUPD_FEATURE_FLAG_IMMEDIATE_MESSAGE |
						     FWUPD_FEATURE_FLAG_DETACH_ACTION,
						     priv->cancellable, &error)) {
			g_printerr ("Failed to set front-end features: %s\n",
				    error->message);
			return EXIT_FAILURE;
		}
	}

	/* run the specified command */
	ret = fu_util_cmd_array_run (cmd_array, priv, argv[1], (gchar**) &argv[2], &error);
	if (!ret) {
		g_printerr ("%s\n", error->message);
		if (g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_INVALID_ARGS)) {
			/* TRANSLATORS: error message explaining command to run to how to get help */
			g_printerr ("\n%s\n", _("Use fwupdmgr --help for help"));
		} else if (g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
			g_debug ("%s\n", error->message);
			return EXIT_NOTHING_TO_DO;
		}
		return EXIT_FAILURE;
	}


#ifdef HAVE_POLKIT
	/* stop listening for polkit questions */
	fu_polkit_agent_close ();
#endif

	/* success */
	return EXIT_SUCCESS;
}
