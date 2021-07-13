/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#define G_LOG_DOMAIN				"FuDevice"

#include "config.h"

#include <string.h>
#include <glib-object.h>
#include <gio/gio.h>

#include "fu-common.h"
#include "fu-common-version.h"
#include "fu-device-private.h"
#include "fu-mutex.h"
#include "fu-quirks.h"

#include "fwupd-common.h"
#include "fwupd-device-private.h"

#define FU_DEVICE_RETRY_OPEN_COUNT			5
#define FU_DEVICE_RETRY_OPEN_DELAY			500 /* ms */

#define FU_DEVICE_DEFAULT_BATTERY_THRESHOLD		10 /* % */

/**
 * FuDevice:
 *
 * A physical or logical device that is exported to the daemon.
 *
 * See also: [class@FuDeviceLocker], [class@Fwupd.Device]
 */

static void fu_device_finalize			 (GObject *object);

typedef struct {
	gchar				*alternate_id;
	gchar				*equivalent_id;
	gchar				*physical_id;
	gchar				*logical_id;
	gchar				*backend_id;
	gchar				*proxy_guid;
	FuDevice			*alternate;
	FuDevice			*proxy;		/* noref */
	FuContext			*ctx;
	GHashTable			*inhibits;	/* (nullable) */
	GHashTable			*metadata;	/* (nullable) */
	GRWLock				 metadata_mutex;
	GPtrArray			*parent_guids;
	GRWLock				 parent_guids_mutex;
	GPtrArray			*parent_physical_ids;	/* (nullable) */
	guint				 remove_delay;	/* ms */
	guint				 progress;
	guint				 battery_level;
	guint				 battery_threshold;
	gint				 order;
	guint				 priority;
	guint				 poll_id;
	gboolean			 done_probe;
	gboolean			 done_setup;
	gboolean			 device_id_valid;
	guint64				 size_min;
	guint64				 size_max;
	gint				 open_refcount;	/* atomic */
	GType				 specialized_gtype;
	GPtrArray			*possible_plugins;
	GPtrArray			*retry_recs;	/* of FuDeviceRetryRecovery */
	guint				 retry_delay;
	FuDeviceInternalFlags		 internal_flags;
	guint64				 private_flags;
	GPtrArray			*private_flag_items;	/* (nullable) */
} FuDevicePrivate;

typedef struct {
	GQuark				 domain;
	gint				 code;
	FuDeviceRetryFunc		 recovery_func;
} FuDeviceRetryRecovery;

typedef struct {
	gchar		*inhibit_id;
	gchar		*reason;
} FuDeviceInhibit;

enum {
	PROP_0,
	PROP_PROGRESS,
	PROP_BATTERY_LEVEL,
	PROP_BATTERY_THRESHOLD,
	PROP_PHYSICAL_ID,
	PROP_LOGICAL_ID,
	PROP_BACKEND_ID,
	PROP_CONTEXT,
	PROP_PROXY,
	PROP_PARENT,
	PROP_LAST
};

enum {
	SIGNAL_CHILD_ADDED,
	SIGNAL_CHILD_REMOVED,
	SIGNAL_LAST
};

static guint signals[SIGNAL_LAST] = { 0 };

G_DEFINE_TYPE_WITH_PRIVATE (FuDevice, fu_device, FWUPD_TYPE_DEVICE)
#define GET_PRIVATE(o) (fu_device_get_instance_private (o))

static void
fu_device_get_property (GObject *object, guint prop_id,
			GValue *value, GParamSpec *pspec)
{
	FuDevice *self = FU_DEVICE (object);
	FuDevicePrivate *priv = GET_PRIVATE (self);
	switch (prop_id) {
	case PROP_PROGRESS:
		g_value_set_uint (value, priv->progress);
		break;
	case PROP_BATTERY_LEVEL:
		g_value_set_uint (value, priv->battery_level);
		break;
	case PROP_BATTERY_THRESHOLD:
		g_value_set_uint (value, priv->battery_threshold);
		break;
	case PROP_PHYSICAL_ID:
		g_value_set_string (value, priv->physical_id);
		break;
	case PROP_LOGICAL_ID:
		g_value_set_string (value, priv->logical_id);
		break;
	case PROP_BACKEND_ID:
		g_value_set_string (value, priv->backend_id);
		break;
	case PROP_CONTEXT:
		g_value_set_object (value, priv->ctx);
		break;
	case PROP_PROXY:
		g_value_set_object (value, priv->proxy);
		break;
	case PROP_PARENT:
		g_value_set_object (value, fu_device_get_parent (self));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

static void
fu_device_set_property (GObject *object, guint prop_id,
			const GValue *value, GParamSpec *pspec)
{
	FuDevice *self = FU_DEVICE (object);
	switch (prop_id) {
	case PROP_PROGRESS:
		fu_device_set_progress (self, g_value_get_uint (value));
		break;
	case PROP_BATTERY_LEVEL:
		fu_device_set_battery_level (self, g_value_get_uint (value));
		break;
	case PROP_BATTERY_THRESHOLD:
		fu_device_set_battery_threshold (self, g_value_get_uint (value));
		break;
	case PROP_PHYSICAL_ID:
		fu_device_set_physical_id (self, g_value_get_string (value));
		break;
	case PROP_LOGICAL_ID:
		fu_device_set_logical_id (self, g_value_get_string (value));
		break;
	case PROP_BACKEND_ID:
		fu_device_set_backend_id (self, g_value_get_string (value));
		break;
	case PROP_CONTEXT:
		fu_device_set_context (self, g_value_get_object (value));
		break;
	case PROP_PROXY:
		fu_device_set_proxy (self, g_value_get_object (value));
		break;
	case PROP_PARENT:
		fu_device_set_parent (self, g_value_get_object (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
		break;
	}
}

/**
 * fu_device_internal_flag_to_string:
 * @flag: an internal device flag, e.g. %FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON
 *
 * Converts an internal device flag to a string.
 *
 * Returns: identifier string
 *
 * Since: 1.5.5
 **/
const gchar *
fu_device_internal_flag_to_string (FuDeviceInternalFlags flag)
{
	if (flag == FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON)
		return "md-set-icon";
	if (flag == FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME)
		return "md-set-name";
	if (flag == FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME_CATEGORY)
		return "md-set-name-category";
	if (flag == FU_DEVICE_INTERNAL_FLAG_MD_SET_VERFMT)
		return "md-set-verfmt";
	if (flag == FU_DEVICE_INTERNAL_FLAG_ONLY_SUPPORTED)
		return "only-supported";
	if (flag == FU_DEVICE_INTERNAL_FLAG_NO_AUTO_INSTANCE_IDS)
		return "no-auto-instance-ids";
	if (flag == FU_DEVICE_INTERNAL_FLAG_ENSURE_SEMVER)
		return "ensure-semver";
	if (flag == FU_DEVICE_INTERNAL_FLAG_RETRY_OPEN)
		return "retry-open";
	if (flag == FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID)
		return "replug-match-guid";
	if (flag == FU_DEVICE_INTERNAL_FLAG_INHERIT_ACTIVATION)
		return "inherit-activation";
	if (flag == FU_DEVICE_INTERNAL_FLAG_IS_OPEN)
		return "is-open";
	if (flag == FU_DEVICE_INTERNAL_FLAG_NO_SERIAL_NUMBER)
		return "no-serial-number";
	if (flag == FU_DEVICE_INTERNAL_FLAG_AUTO_PARENT_CHILDREN)
		return "auto-parent-children";
	if (flag == FU_DEVICE_INTERNAL_FLAG_ATTACH_EXTRA_RESET)
		return "attach-extra-reset";
	if (flag == FU_DEVICE_INTERNAL_FLAG_INHIBIT_CHILDREN)
		return "inhibit-children";
	if (flag == FU_DEVICE_INTERNAL_FLAG_NO_AUTO_REMOVE_CHILDREN)
		return "no-auto-remove-children";
	if (flag == FU_DEVICE_INTERNAL_FLAG_USE_PARENT_FOR_OPEN)
		return "use-parent-for-open";
	return NULL;
}

/**
 * fu_device_internal_flag_from_string:
 * @flag: a string, e.g. `md-set-icon`
 *
 * Converts a string to an internal device flag.
 *
 * Returns: enumerated value
 *
 * Since: 1.5.5
 **/
FuDeviceInternalFlags
fu_device_internal_flag_from_string (const gchar *flag)
{
	if (g_strcmp0 (flag, "md-set-icon") == 0)
		return FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON;
	if (g_strcmp0 (flag, "md-set-name") == 0)
		return FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME;
	if (g_strcmp0 (flag, "md-set-name-category") == 0)
		return FU_DEVICE_INTERNAL_FLAG_MD_SET_NAME_CATEGORY;
	if (g_strcmp0 (flag, "md-set-verfmt") == 0)
		return FU_DEVICE_INTERNAL_FLAG_MD_SET_VERFMT;
	if (g_strcmp0 (flag, "only-supported") == 0)
		return FU_DEVICE_INTERNAL_FLAG_ONLY_SUPPORTED;
	if (g_strcmp0 (flag, "no-auto-instance-ids") == 0)
		return FU_DEVICE_INTERNAL_FLAG_NO_AUTO_INSTANCE_IDS;
	if (g_strcmp0 (flag, "ensure-semver") == 0)
		return FU_DEVICE_INTERNAL_FLAG_ENSURE_SEMVER;
	if (g_strcmp0 (flag, "retry-open") == 0)
		return FU_DEVICE_INTERNAL_FLAG_RETRY_OPEN;
	if (g_strcmp0 (flag, "replug-match-guid") == 0)
		return FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID;
	if (g_strcmp0 (flag, "inherit-activation") == 0)
		return FU_DEVICE_INTERNAL_FLAG_INHERIT_ACTIVATION;
	if (g_strcmp0 (flag, "is-open") == 0)
		return FU_DEVICE_INTERNAL_FLAG_IS_OPEN;
	if (g_strcmp0 (flag, "no-serial-number") == 0)
		return FU_DEVICE_INTERNAL_FLAG_NO_SERIAL_NUMBER;
	if (g_strcmp0 (flag, "auto-parent-children") == 0)
		return FU_DEVICE_INTERNAL_FLAG_AUTO_PARENT_CHILDREN;
	if (g_strcmp0 (flag, "attach-extra-reset") == 0)
		return FU_DEVICE_INTERNAL_FLAG_ATTACH_EXTRA_RESET;
	if (g_strcmp0 (flag, "inhibit-children") == 0)
		return FU_DEVICE_INTERNAL_FLAG_INHIBIT_CHILDREN;
	if (g_strcmp0 (flag, "no-auto-remove-children") == 0)
		return FU_DEVICE_INTERNAL_FLAG_NO_AUTO_REMOVE_CHILDREN;
	if (g_strcmp0 (flag, "use-parent-for-open") == 0)
		return FU_DEVICE_INTERNAL_FLAG_USE_PARENT_FOR_OPEN;
	return FU_DEVICE_INTERNAL_FLAG_UNKNOWN;
}

/**
 * fu_device_add_internal_flag:
 * @self: a #FuDevice
 * @flag: an internal device flag, e.g. %FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON
 *
 * Adds a private flag that stays internal to the engine and is not leaked to the client.
 *
 * Since: 1.5.5
 **/
void
fu_device_add_internal_flag (FuDevice *self, FuDeviceInternalFlags flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->internal_flags |= flag;
}

/**
 * fu_device_remove_internal_flag:
 * @self: a #FuDevice
 * @flag: an internal device flag, e.g. %FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON
 *
 * Removes a private flag that stays internal to the engine and is not leaked to the client.
 *
 * Since: 1.5.5
 **/
void
fu_device_remove_internal_flag (FuDevice *self, FuDeviceInternalFlags flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->internal_flags &= ~flag;
}

/**
 * fu_device_has_internal_flag:
 * @self: a #FuDevice
 * @flag: an internal device flag, e.g. %FU_DEVICE_INTERNAL_FLAG_MD_SET_ICON
 *
 * Tests for a private flag that stays internal to the engine and is not leaked to the client.
 *
 * Since: 1.5.5
 **/
gboolean
fu_device_has_internal_flag (FuDevice *self, FuDeviceInternalFlags flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	return (priv->internal_flags & flag) > 0;
}

/**
 * fu_device_add_private_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Adds a private flag that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
void
fu_device_add_private_flag (FuDevice *self, guint64 flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->private_flags |= flag;
}

/**
 * fu_device_remove_private_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Removes a private flag that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
void
fu_device_remove_private_flag (FuDevice *self, guint64 flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->private_flags &= ~flag;
}

/**
 * fu_device_has_private_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Tests for a private flag that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
gboolean
fu_device_has_private_flag (FuDevice *self, guint64 flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	return (priv->private_flags & flag) > 0;
}

/**
 * fu_device_get_private_flags:
 * @self: a #FuDevice
 *
 * Returns all the private flags that can be used by the plugin for any purpose.
 *
 * Returns: flags
 *
 * Since: 1.6.2
 **/
guint64
fu_device_get_private_flags (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), G_MAXUINT64);
	return priv->private_flags;
}

/**
 * fu_device_set_private_flags:
 * @self: a #FuDevice
 * @flag: flags
 *
 * Sets private flags that can be used by the plugin for any purpose.
 *
 * Since: 1.6.2
 **/
void
fu_device_set_private_flags (FuDevice *self, guint64 flag)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->private_flags = flag;
}

/**
 * fu_device_get_possible_plugins:
 * @self: a #FuDevice
 *
 * Gets the list of possible plugin names, typically added from quirk files.
 *
 * Returns: (element-type utf8) (transfer container): plugin names
 *
 * Since: 1.3.3
 **/
GPtrArray *
fu_device_get_possible_plugins (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	return g_ptr_array_ref (priv->possible_plugins);
}

/**
 * fu_device_add_possible_plugin:
 * @self: a #FuDevice
 * @plugin: a plugin name, e.g. `dfu`
 *
 * Adds a plugin name to the list of plugins that *might* be able to handle this
 * device. This is tyically called from a quirk handler.
 *
 * Duplicate plugin names are ignored.
 *
 * Since: 1.5.1
 **/
void
fu_device_add_possible_plugin (FuDevice *self, const gchar *plugin)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (plugin != NULL);

	/* add if it does not already exist */
#if GLIB_CHECK_VERSION(2,54,3)
	if (g_ptr_array_find_with_equal_func (priv->possible_plugins, plugin,
					      g_str_equal, NULL))
		return;
#endif
	g_ptr_array_add (priv->possible_plugins, g_strdup (plugin));
}

/**
 * fu_device_retry_add_recovery:
 * @self: a #FuDevice
 * @domain: a #GQuark, or %0 for all domains
 * @code: a #GError code
 * @func: (scope async) (nullable): a function to recover the device
 *
 * Sets the optional function to be called when fu_device_retry() fails, which
 * is possibly a device reset.
 *
 * If @func is %NULL then recovery is not possible and an error is returned
 * straight away.
 *
 * Since: 1.4.0
 **/
void
fu_device_retry_add_recovery (FuDevice *self,
			      GQuark domain,
			      gint code,
			      FuDeviceRetryFunc func)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDeviceRetryRecovery *rec;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (domain != 0);

	rec = g_new (FuDeviceRetryRecovery, 1);
	rec->domain = domain;
	rec->code = code;
	rec->recovery_func = func;
	g_ptr_array_add (priv->retry_recs, rec);
}

/**
 * fu_device_retry_set_delay:
 * @self: a #FuDevice
 * @delay: delay in ms
 *
 * Sets the recovery delay between failed retries.
 *
 * Since: 1.4.0
 **/
void
fu_device_retry_set_delay (FuDevice *self, guint delay)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->retry_delay = delay;
}

/**
 * fu_device_retry_full:
 * @self: a #FuDevice
 * @func: (scope async): a function to execute
 * @count: the number of tries to try the function
 * @delay: the delay between each try in ms
 * @user_data: (nullable): a helper to pass to @func
 * @error: (nullable): optional return location for an error
 *
 * Calls a specific function a number of times, optionally handling the error
 * with a reset action.
 *
 * If fu_device_retry_add_recovery() has not been used then all errors are
 * considered non-fatal until the last try.
 *
 * If the reset function returns %FALSE, then the function returns straight away
 * without processing any pending retries.
 *
 * Since: 1.5.5
 **/
gboolean
fu_device_retry_full (FuDevice *self,
		      FuDeviceRetryFunc func,
		      guint count,
		      guint delay,
		      gpointer user_data,
		      GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (func != NULL, FALSE);
	g_return_val_if_fail (count >= 1, FALSE);
	g_return_val_if_fail (error != NULL, FALSE);

	for (guint i = 0; ; i++) {
		g_autoptr(GError) error_local =	NULL;

		/* delay */
		if (i > 0 && delay > 0)
			g_usleep (delay * 1000);

		/* run function, if success return success */
		if (func (self, user_data, &error_local))
			break;

		/* sanity check */
		if (error_local == NULL) {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_FAILED,
				     "exec failed but no error set!");
			return FALSE;
		}

		/* too many retries */
		if (i >= count - 1) {
			g_propagate_prefixed_error (error,
						    g_steal_pointer (&error_local),
						    "failed after %u retries: ",
						    count);
			return FALSE;
		}

		/* show recoverable error on the console */
		if (priv->retry_recs->len == 0) {
			g_debug ("failed on try %u of %u: %s",
				 i + 1, count, error_local->message);
			continue;
		}

		/* find the condition that matches */
		for (guint j = 0; j < priv->retry_recs->len; j++) {
			FuDeviceRetryRecovery *rec = g_ptr_array_index (priv->retry_recs, j);
			if (g_error_matches (error_local, rec->domain, rec->code)) {
				if (rec->recovery_func != NULL) {
					if (!rec->recovery_func (self, user_data, error))
						return FALSE;
				} else {
					g_set_error (error,
						     G_IO_ERROR,
						     G_IO_ERROR_FAILED,
						     "device recovery not possible");
					return FALSE;
				}
			}
		}
	}

	/* success */
	return TRUE;
}

/**
 * fu_device_retry:
 * @self: a #FuDevice
 * @func: (scope async): a function to execute
 * @count: the number of tries to try the function
 * @user_data: (nullable): a helper to pass to @func
 * @error: (nullable): optional return location for an error
 *
 * Calls a specific function a number of times, optionally handling the error
 * with a reset action.
 *
 * If fu_device_retry_add_recovery() has not been used then all errors are
 * considered non-fatal until the last try.
 *
 * If the reset function returns %FALSE, then the function returns straight away
 * without processing any pending retries.
 *
 * Since: 1.4.0
 **/
gboolean
fu_device_retry (FuDevice *self,
		 FuDeviceRetryFunc func,
		 guint count,
		 gpointer user_data,
		 GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	return fu_device_retry_full (self, func, count,
				     priv->retry_delay,
				     user_data, error);
}

/**
 * fu_device_poll:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Polls a device, typically querying the hardware for status.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_poll (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* subclassed */
	if (klass->poll != NULL) {
		if (!klass->poll (self, error))
			return FALSE;
	}
	return TRUE;
}

static gboolean
fu_device_poll_cb (gpointer user_data)
{
	FuDevice *self = FU_DEVICE (user_data);
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GError) error_local = NULL;
	if (!fu_device_poll (self, &error_local)) {
		g_warning ("disabling polling: %s", error_local->message);
		priv->poll_id = 0;
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

/**
 * fu_device_set_poll_interval:
 * @self: a #FuPlugin
 * @interval: duration in ms, or 0 to disable
 *
 * Polls the hardware every interval period. If the subclassed `->poll()` method
 * returns %FALSE then a warning is printed to the console and the poll is
 * disabled until the next call to fu_device_set_poll_interval().
 *
 * Since: 1.1.2
 **/
void
fu_device_set_poll_interval (FuDevice *self, guint interval)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FU_IS_DEVICE (self));

	if (priv->poll_id != 0) {
		g_source_remove (priv->poll_id);
		priv->poll_id = 0;
	}
	if (interval == 0)
		return;
	if (interval % 1000 == 0) {
		priv->poll_id = g_timeout_add_seconds (interval / 1000,
						       fu_device_poll_cb,
						       self);
	} else {
		priv->poll_id = g_timeout_add (interval, fu_device_poll_cb, self);
	}
}

/**
 * fu_device_get_order:
 * @self: a #FuPlugin
 *
 * Gets the device order, where higher numbers are installed after lower
 * numbers.
 *
 * Returns: the integer value
 *
 * Since: 1.0.8
 **/
gint
fu_device_get_order (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return priv->order;
}

/**
 * fu_device_set_order:
 * @self: a #FuDevice
 * @order: an integer value
 *
 * Sets the device order, where higher numbers are installed after lower
 * numbers.
 *
 * Since: 1.0.8
 **/
void
fu_device_set_order (FuDevice *self, gint order)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->order = order;
}

/**
 * fu_device_get_priority:
 * @self: a #FuPlugin
 *
 * Gets the device priority, where higher numbers are better.
 *
 * Returns: the integer value
 *
 * Since: 1.1.1
 **/
guint
fu_device_get_priority (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return priv->priority;
}

/**
 * fu_device_set_priority:
 * @self: a #FuDevice
 * @priority: an integer value
 *
 * Sets the device priority, where higher numbers are better.
 *
 * Since: 1.1.1
 **/
void
fu_device_set_priority (FuDevice *self, guint priority)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->priority = priority;
}

/**
 * fu_device_get_equivalent_id:
 * @self: a #FuDevice
 *
 * Gets any equivalent ID for a device
 *
 * Returns: (transfer none): a #gchar or NULL
 *
 * Since: 0.6.1
 **/
const gchar *
fu_device_get_equivalent_id (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->equivalent_id;
}

/**
 * fu_device_set_equivalent_id:
 * @self: a #FuDevice
 * @equivalent_id: (nullable): a string
 *
 * Sets any equivalent ID for a device
 *
 * Since: 0.6.1
 **/
void
fu_device_set_equivalent_id (FuDevice *self, const gchar *equivalent_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->equivalent_id, equivalent_id) == 0)
		return;

	g_free (priv->equivalent_id);
	priv->equivalent_id = g_strdup (equivalent_id);
}

/**
 * fu_device_get_alternate_id:
 * @self: a #FuDevice
 *
 * Gets any alternate device ID. An alternate device may be linked to the primary
 * device in some way.
 *
 * Returns: (transfer none): a device or %NULL
 *
 * Since: 1.1.0
 **/
const gchar *
fu_device_get_alternate_id (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->alternate_id;
}

/**
 * fu_device_set_alternate_id:
 * @self: a #FuDevice
 * @alternate_id: (nullable): Another #FuDevice ID
 *
 * Sets any alternate device ID. An alternate device may be linked to the primary
 * device in some way.
 *
 * Since: 1.1.0
 **/
void
fu_device_set_alternate_id (FuDevice *self, const gchar *alternate_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->alternate_id, alternate_id) == 0)
		return;

	g_free (priv->alternate_id);
	priv->alternate_id = g_strdup (alternate_id);
}

/**
 * fu_device_get_alternate:
 * @self: a #FuDevice
 *
 * Gets any alternate device. An alternate device may be linked to the primary
 * device in some way.
 *
 * The alternate object will be matched from the ID set in fu_device_set_alternate_id()
 * and will be assigned by the daemon. This means if the ID is not found as an
 * added device, then this function will return %NULL.
 *
 * Returns: (transfer none): a device or %NULL
 *
 * Since: 0.7.2
 **/
FuDevice *
fu_device_get_alternate (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->alternate;
}

/**
 * fu_device_set_alternate:
 * @self: a #FuDevice
 * @alternate: Another #FuDevice
 *
 * Sets any alternate device. An alternate device may be linked to the primary
 * device in some way.
 *
 * This function is only usable by the daemon, not directly from plugins.
 *
 * Since: 0.7.2
 **/
void
fu_device_set_alternate (FuDevice *self, FuDevice *alternate)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	g_set_object (&priv->alternate, alternate);
}

/**
 * fu_device_get_parent:
 * @self: a #FuDevice
 *
 * Gets any parent device. An parent device is logically "above" the current
 * device and this may be reflected in client tools.
 *
 * This information also allows the plugin to optionally verify the parent
 * device, for instance checking the parent device firmware version.
 *
 * The parent object is not refcounted and if destroyed this function will then
 * return %NULL.
 *
 * Returns: (transfer none): a device or %NULL
 *
 * Since: 1.0.8
 **/
FuDevice *
fu_device_get_parent (FuDevice *self)
{
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return FU_DEVICE (fwupd_device_get_parent (FWUPD_DEVICE (self)));
}

/**
 * fu_device_get_root:
 * @self: a #FuDevice
 *
 * Gets the root parent device. A parent device is logically "above" the current
 * device and this may be reflected in client tools.
 *
 * If there is no parent device defined, then @self is returned.
 *
 * Returns: (transfer full): a device
 *
 * Since: 1.4.0
 **/
FuDevice *
fu_device_get_root (FuDevice *self)
{
	FuDevice *parent;
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	do {
		parent = fu_device_get_parent (self);
		if (parent != NULL)
			self = parent;
	} while (parent != NULL);
	return g_object_ref (self);
}

static void
fu_device_set_composite_id (FuDevice *self, const gchar *composite_id)
{
	GPtrArray *children;

	/* subclassed simple setter */
	fwupd_device_set_composite_id (FWUPD_DEVICE (self), composite_id);

	/* all children */
	children = fu_device_get_children (self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index (children, i);
		fu_device_set_composite_id (child_tmp, composite_id);
	}
}

/**
 * fu_device_set_parent:
 * @self: a #FuDevice
 * @parent: (nullable): a device
 *
 * Sets any parent device. An parent device is logically "above" the current
 * device and this may be reflected in client tools.
 *
 * This information also allows the plugin to optionally verify the parent
 * device, for instance checking the parent device firmware version.
 *
 * Since: 1.0.8
 **/
void
fu_device_set_parent (FuDevice *self, FuDevice *parent)
{
	g_return_if_fail (FU_IS_DEVICE (self));

	/* debug */
	if (parent != NULL) {
		g_debug ("setting parent of %s [%s] to be %s [%s]",
			 fu_device_get_name (self), fu_device_get_id (self),
			 fu_device_get_name (parent), fu_device_get_id (parent));
	}

	/* set the composite ID on the children and grandchildren */
	if (parent != NULL)
		fu_device_set_composite_id (self, fu_device_get_composite_id (parent));

	/* if the parent has a context, make the child inherit it */
	if (parent != NULL) {
		if (fu_device_get_context (self) == NULL &&
		    fu_device_get_context (parent) != NULL)
			fu_device_set_context (self, fu_device_get_context (parent));
	}

	fwupd_device_set_parent (FWUPD_DEVICE (self), FWUPD_DEVICE (parent));
}

/**
 * fu_device_set_proxy:
 * @self: a #FuDevice
 * @proxy: a device
 *
 * Sets any proxy device. A proxy device can be used to perform an action on
 * behalf of another device, for instance attach()ing it after a successful
 * update.
 *
 * Since: 1.4.1
 **/
void
fu_device_set_proxy (FuDevice *self, FuDevice *proxy)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FU_IS_DEVICE (self));

	if (priv->proxy != NULL)
		g_object_remove_weak_pointer (G_OBJECT (priv->proxy), (gpointer *) &priv->proxy);
	if (proxy != NULL)
		g_object_add_weak_pointer (G_OBJECT (proxy), (gpointer *) &priv->proxy);
	priv->proxy = proxy;
}

/**
 * fu_device_get_proxy:
 * @self: a #FuDevice
 *
 * Gets any proxy device. A proxy device can be used to perform an action on
 * behalf of another device, for instance attach()ing it after a successful
 * update.
 *
 * The proxy object is not refcounted and if destroyed this function will then
 * return %NULL.
 *
 * Returns: (transfer none): a device or %NULL
 *
 * Since: 1.4.1
 **/
FuDevice *
fu_device_get_proxy (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->proxy;
}

/**
 * fu_device_get_children:
 * @self: a #FuDevice
 *
 * Gets any child devices. A child device is logically "below" the current
 * device and this may be reflected in client tools.
 *
 * Returns: (transfer none) (element-type FuDevice): child devices
 *
 * Since: 1.0.8
 **/
GPtrArray *
fu_device_get_children (FuDevice *self)
{
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return fwupd_device_get_children (FWUPD_DEVICE (self));
}

/**
 * fu_device_add_child:
 * @self: a #FuDevice
 * @child: Another #FuDevice
 *
 * Sets any child device. An child device is logically linked to the primary
 * device in some way.
 *
 * Since: 1.0.8
 **/
void
fu_device_add_child (FuDevice *self, FuDevice *child)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDevicePrivate *priv_child = GET_PRIVATE (child);
	GPtrArray *children;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (FU_IS_DEVICE (child));

	/* add if the child does not already exist */
	fwupd_device_add_child (FWUPD_DEVICE (self), FWUPD_DEVICE (child));

	/* propagate inhibits to children */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_INHIBIT_CHILDREN)) {
		g_autoptr(GList) values = g_hash_table_get_values (priv->inhibits);
		for (GList *l = values; l != NULL; l = l->next) {
			FuDeviceInhibit *inhibit = (FuDeviceInhibit *) l->data;
			fu_device_inhibit (child, inhibit->inhibit_id, inhibit->reason);
		}
	}

	/* ensure the parent has the MAX() of the children's removal delay  */
	children = fu_device_get_children (self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index (children, i);
		guint remove_delay = fu_device_get_remove_delay (child_tmp);
		if (remove_delay > priv->remove_delay) {
			g_debug ("setting remove delay to %u as child is greater than %u",
				 remove_delay, priv->remove_delay);
			priv->remove_delay = remove_delay;
		}
	}

	/* copy from main device if unset */
	if (fu_device_get_physical_id (child) == NULL &&
	    fu_device_get_physical_id (self) != NULL)
		fu_device_set_physical_id (child, fu_device_get_physical_id (self));
	if (priv_child->backend_id == NULL && priv->backend_id != NULL)
		fu_device_set_backend_id (child, priv->backend_id);
	if (priv_child->ctx == NULL && priv->ctx != NULL)
		fu_device_set_context (child, priv->ctx);
	if (fu_device_get_vendor (child) == NULL)
		fu_device_set_vendor (child, fu_device_get_vendor (self));
	if (priv_child->remove_delay == 0 && priv->remove_delay != 0)
		fu_device_set_remove_delay (child, priv->remove_delay);
	if (fu_device_get_vendor_ids(child)->len == 0) {
		GPtrArray *vendor_ids = fu_device_get_vendor_ids (self);
		for (guint i = 0; i < vendor_ids->len; i++) {
			const gchar *vendor_id = g_ptr_array_index (vendor_ids, i);
			fu_device_add_vendor_id (child, vendor_id);
		}
	}
	if (fu_device_get_icons(child)->len == 0) {
		GPtrArray *icons = fu_device_get_icons (self);
		for (guint i = 0; i < icons->len; i++) {
			const gchar *icon_name = g_ptr_array_index (icons, i);
			fu_device_add_icon (child, icon_name);
		}
	}

	/* ensure the ID is converted */
	if (!fu_device_ensure_id (child, &error))
		g_warning ("failed to ensure child: %s", error->message);

	/* ensure the parent is also set on the child */
	fu_device_set_parent (child, self);

	/* signal to the plugin in case this is done after setup */
	g_signal_emit (self, signals[SIGNAL_CHILD_ADDED], 0, child);
}

/**
 * fu_device_remove_child:
 * @self: a #FuDevice
 * @child: Another #FuDevice
 *
 * Removes child device.
 *
 * Since: 1.6.2
 **/
void
fu_device_remove_child (FuDevice *self, FuDevice *child)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (FU_IS_DEVICE (child));

	/* proxy */
	fwupd_device_remove_child (FWUPD_DEVICE (self), FWUPD_DEVICE (child));

	/* signal to the plugin */
	g_signal_emit (self, signals[SIGNAL_CHILD_REMOVED], 0, child);
}

/**
 * fu_device_get_parent_guids:
 * @self: a #FuDevice
 *
 * Gets any parent device GUIDs. If a device is added to the daemon that matches
 * any GUIDs added from fu_device_add_parent_guid() then this device is marked the parent of @self.
 *
 * Returns: (transfer none) (element-type utf8): a list of GUIDs
 *
 * Since: 1.0.8
 **/
GPtrArray *
fu_device_get_parent_guids (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GRWLockReaderLocker) locker = g_rw_lock_reader_locker_new (&priv->parent_guids_mutex);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	g_return_val_if_fail (locker != NULL, NULL);
	return priv->parent_guids;
}

/**
 * fu_device_has_parent_guid:
 * @self: a #FuDevice
 * @guid: a GUID
 *
 * Searches the list of parent GUIDs for a string match.
 *
 * Returns: %TRUE if the parent GUID exists
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_has_parent_guid (FuDevice *self, const gchar *guid)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GRWLockReaderLocker) locker = g_rw_lock_reader_locker_new (&priv->parent_guids_mutex);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (guid != NULL, FALSE);
	g_return_val_if_fail (locker != NULL, FALSE);

	for (guint i = 0; i < priv->parent_guids->len; i++) {
		const gchar *guid_tmp = g_ptr_array_index (priv->parent_guids, i);
		if (g_strcmp0 (guid_tmp, guid) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_device_add_parent_guid:
 * @self: a #FuDevice
 * @guid: a GUID
 *
 * Sets any parent device using a GUID. An parent device is logically linked to
 * the primary device in some way and can be added before or after @self.
 *
 * The GUIDs are searched in order, and so the order of adding GUIDs may be
 * important if more than one parent device might match.
 *
 * If the parent device is removed, any children logically linked to it will
 * also be removed.
 *
 * Since: 1.0.8
 **/
void
fu_device_add_parent_guid (FuDevice *self, const gchar *guid)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GRWLockWriterLocker) locker = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (guid != NULL);

	/* make valid */
	if (!fwupd_guid_is_valid (guid)) {
		g_autofree gchar *tmp = fwupd_guid_hash_string (guid);
		if (fu_device_has_parent_guid (self, tmp))
			return;
		g_debug ("using %s for %s", tmp, guid);
		g_ptr_array_add (priv->parent_guids, g_steal_pointer (&tmp));
		return;
	}

	/* already valid */
	if (fu_device_has_parent_guid (self, guid))
		return;
	locker = g_rw_lock_writer_locker_new (&priv->parent_guids_mutex);
	g_return_if_fail (locker != NULL);
	g_ptr_array_add (priv->parent_guids, g_strdup (guid));
}

/**
 * fu_device_get_parent_physical_ids:
 * @self: a #FuDevice
 *
 * Gets any parent device IDs. If a device is added to the daemon that matches
 * the physical ID added from fu_device_add_parent_physical_id() then this
 * device is marked the parent of @self.
 *
 * Returns: (transfer none) (element-type utf8) (nullable): a list of IDs
 *
 * Since: 1.6.2
 **/
GPtrArray *
fu_device_get_parent_physical_ids (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->parent_physical_ids;
}

/**
 * fu_device_has_parent_physical_id:
 * @self: a #FuDevice
 * @physical_id: a device physical ID
 *
 * Searches the list of parent IDs for a string match.
 *
 * Returns: %TRUE if the parent ID exists
 *
 * Since: 1.6.2
 **/
gboolean
fu_device_has_parent_physical_id (FuDevice *self, const gchar *physical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (physical_id != NULL, FALSE);

	if (priv->parent_physical_ids == NULL)
		return FALSE;
	for (guint i = 0; i < priv->parent_physical_ids->len; i++) {
		const gchar *tmp = g_ptr_array_index (priv->parent_physical_ids, i);
		if (g_strcmp0 (tmp, physical_id) == 0)
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_device_add_parent_physical_id:
 * @self: a #FuDevice
 * @physical_id: a device physical ID
 *
 * Sets any parent device using the physical ID. An parent device is logically
 * linked to the primary device in some way and can be added before or after @self.
 *
 * The IDs are searched in order, and so the order of adding IDs may be
 * important if more than one parent device might match.
 *
 * Since: 1.6.2
 **/
void
fu_device_add_parent_physical_id (FuDevice *self, const gchar *physical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (physical_id != NULL);

	/* ensure exists */
	if (priv->parent_physical_ids == NULL)
		priv->parent_physical_ids = g_ptr_array_new_with_free_func (g_free);

	/* already present */
	if (fu_device_has_parent_physical_id (self, physical_id))
		return;
	g_ptr_array_add (priv->parent_physical_ids, g_strdup (physical_id));
}

static gboolean
fu_device_add_child_by_type_guid (FuDevice *self,
				  GType type,
				  const gchar *guid,
				  GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(FuDevice) child = NULL;
	child = g_object_new (type,
			      "context", priv->ctx,
			      "logical-id", guid,
			      NULL);
	fu_device_add_guid (child, guid);
	if (fu_device_get_physical_id (self) != NULL)
		fu_device_set_physical_id (child, fu_device_get_physical_id (self));
	if (!fu_device_ensure_id (self, error))
		return FALSE;
	if (!fu_device_probe (child, error))
		return FALSE;
	fu_device_convert_instance_ids (child);
	fu_device_add_child (self, child);
	return TRUE;
}

static gboolean
fu_device_add_child_by_kv (FuDevice *self, const gchar *str, GError **error)
{
	g_auto(GStrv) split = g_strsplit (str, "|", -1);

	/* type same as parent */
	if (g_strv_length (split) == 1) {
		return fu_device_add_child_by_type_guid (self,
							 G_OBJECT_TYPE (self),
							 split[1],
							 error);
	}

	/* type specified */
	if (g_strv_length (split) == 2) {
		GType devtype = g_type_from_name (split[0]);
		if (devtype == 0) {
			g_set_error_literal (error,
					     G_IO_ERROR,
					     G_IO_ERROR_NOT_FOUND,
					     "no GType registered");
			return FALSE;
		}
		return fu_device_add_child_by_type_guid (self,
							 devtype,
							 split[1],
							 error);
	}

	/* more than one '|' */
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "unable to add parse child section");
	return FALSE;
}

static gboolean
fu_device_set_quirk_kv (FuDevice *self,
			const gchar *key,
			const gchar *value,
			GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	if (g_strcmp0 (key, FU_QUIRKS_PLUGIN) == 0) {
		fu_device_add_possible_plugin (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_FLAGS) == 0) {
		fu_device_set_custom_flags (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_NAME) == 0) {
		fu_device_set_name (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_SUMMARY) == 0) {
		fu_device_set_summary (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_BRANCH) == 0) {
		fu_device_set_branch (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_VENDOR) == 0) {
		fu_device_set_vendor (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_VENDOR_ID) == 0) {
		fu_device_add_vendor_id (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_PROTOCOL) == 0) {
		g_auto(GStrv) sections = g_strsplit (value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++)
			fu_device_add_protocol (self, sections[i]);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_VERSION) == 0) {
		fu_device_set_version (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_UPDATE_MESSAGE) == 0) {
		fu_device_set_update_message_kind (self, FWUPD_DEVICE_MESSAGE_KIND_POST);
		fu_device_set_update_message (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_UPDATE_IMAGE) == 0) {
		fu_device_set_update_image (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_ICON) == 0) {
		fu_device_add_icon (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_GUID) == 0) {
		fu_device_add_guid (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_COUNTERPART_GUID) == 0) {
		fu_device_add_counterpart_guid (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_PARENT_GUID) == 0) {
		fu_device_add_parent_guid (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_PROXY_GUID) == 0) {
		fu_device_set_proxy_guid (self, value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_FIRMWARE_SIZE_MIN) == 0) {
		fu_device_set_firmware_size_min (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_FIRMWARE_SIZE_MAX) == 0) {
		fu_device_set_firmware_size_max (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_FIRMWARE_SIZE) == 0) {
		fu_device_set_firmware_size (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_INSTALL_DURATION) == 0) {
		fu_device_set_install_duration (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_PRIORITY) == 0) {
		fu_device_set_priority (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_BATTERY_THRESHOLD) == 0) {
		fu_device_set_battery_threshold (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_REMOVE_DELAY) == 0) {
		fu_device_set_remove_delay (self, fu_common_strtoull (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_VERSION_FORMAT) == 0) {
		fu_device_set_version_format (self, fwupd_version_format_from_string (value));
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_INHIBIT) == 0) {
		if (value != NULL)
			fu_device_inhibit (self, "quirk", value);
		else
			fu_device_uninhibit (self, "quirk");
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_GTYPE) == 0) {
		if (priv->specialized_gtype != G_TYPE_INVALID) {
			g_debug ("already set GType to %s, ignoring %s",
				 g_type_name (priv->specialized_gtype), value);
			return TRUE;
		}
		priv->specialized_gtype = g_type_from_name (value);
		return TRUE;
	}
	if (g_strcmp0 (key, FU_QUIRKS_CHILDREN) == 0) {
		g_auto(GStrv) sections = g_strsplit (value, ",", -1);
		for (guint i = 0; sections[i] != NULL; i++) {
			if (!fu_device_add_child_by_kv (self, sections[i], error))
				return FALSE;
		}
		return TRUE;
	}

	/* optional device-specific method */
	if (klass->set_quirk_kv != NULL)
		return klass->set_quirk_kv (self, key, value, error);

	/* failed */
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "quirk key not supported");
	return FALSE;
}

/**
 * fu_device_get_specialized_gtype:
 * @self: a #FuDevice
 *
 * Gets the specialized type of the device
 *
 * Returns:#GType
 *
 * Since: 1.3.3
 **/
GType
fu_device_get_specialized_gtype (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	return priv->specialized_gtype;
}

static void
fu_device_quirks_iter_cb (FuContext *ctx, const gchar *key, const gchar *value, gpointer user_data)
{
	FuDevice *self = FU_DEVICE (user_data);
	g_autoptr(GError) error = NULL;
	if (!fu_device_set_quirk_kv (self, key, value, &error)) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
			g_warning ("failed to set quirk key %s=%s: %s",
				   key, value, error->message);
		}
	}
}

static void
fu_device_add_guid_quirks (FuDevice *self, const gchar *guid)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	if (priv->ctx == NULL) {
		g_autofree gchar *str = fu_device_to_string (self);
		g_critical ("no FuContext assigned for %s", str);
		return;
	}
	fu_context_lookup_quirk_by_id_iter (priv->ctx, guid, fu_device_quirks_iter_cb, self);
}

/**
 * fu_device_set_firmware_size:
 * @self: a #FuDevice
 * @size: Size in bytes
 *
 * Sets the exact allowed size of the firmware blob.
 *
 * Since: 1.2.6
 **/
void
fu_device_set_firmware_size (FuDevice *self, guint64 size)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->size_min = size;
	priv->size_max = size;
}

/**
 * fu_device_set_firmware_size_min:
 * @self: a #FuDevice
 * @size_min: Size in bytes
 *
 * Sets the minimum allowed size of the firmware blob.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_firmware_size_min (FuDevice *self, guint64 size_min)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->size_min = size_min;
}

/**
 * fu_device_set_firmware_size_max:
 * @self: a #FuDevice
 * @size_max: Size in bytes
 *
 * Sets the maximum allowed size of the firmware blob.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_firmware_size_max (FuDevice *self, guint64 size_max)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->size_max = size_max;
}

/**
 * fu_device_get_firmware_size_min:
 * @self: a #FuDevice
 *
 * Gets the minimum size of the firmware blob.
 *
 * Returns: Size in bytes, or 0 if unset
 *
 * Since: 1.2.6
 **/
guint64
fu_device_get_firmware_size_min (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return priv->size_min;
}

/**
 * fu_device_get_firmware_size_max:
 * @self: a #FuDevice
 *
 * Gets the maximum size of the firmware blob.
 *
 * Returns: Size in bytes, or 0 if unset
 *
 * Since: 1.2.6
 **/
guint64
fu_device_get_firmware_size_max (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return priv->size_max;
}

static void
fu_device_add_guid_safe (FuDevice *self, const gchar *guid, FuDeviceInstanceFlags flags)
{
	/* add the device GUID before adding additional GUIDs from quirks
	 * to ensure the bootloader GUID is listed after the runtime GUID */
	if ((flags & FU_DEVICE_INSTANCE_FLAG_ONLY_QUIRKS) == 0)
		fwupd_device_add_guid (FWUPD_DEVICE (self), guid);
	if ((flags & FU_DEVICE_INSTANCE_FLAG_NO_QUIRKS) == 0)
		fu_device_add_guid_quirks (self, guid);
}

/**
 * fu_device_has_guid:
 * @self: a #FuDevice
 * @guid: a GUID, e.g. `WacomAES`
 *
 * Finds out if the device has a specific GUID.
 *
 * Returns: %TRUE if the GUID is found
 *
 * Since: 1.2.2
 **/
gboolean
fu_device_has_guid (FuDevice *self, const gchar *guid)
{
	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (guid != NULL, FALSE);

	/* make valid */
	if (!fwupd_guid_is_valid (guid)) {
		g_autofree gchar *tmp = fwupd_guid_hash_string (guid);
		return fwupd_device_has_guid (FWUPD_DEVICE (self), tmp);
	}

	/* already valid */
	return fwupd_device_has_guid (FWUPD_DEVICE (self), guid);
}

/**
 * fu_device_add_instance_id_full:
 * @self: a #FuDevice
 * @instance_id: a Instance ID, e.g. `WacomAES`
 * @flags: instance ID flags
 *
 * Adds an instance ID with all parameters set
 *
 *
 * Since: 1.2.9
 **/
void
fu_device_add_instance_id_full (FuDevice *self,
				const gchar *instance_id,
				FuDeviceInstanceFlags flags)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autofree gchar *guid = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (instance_id != NULL);

	if (fwupd_guid_is_valid (instance_id)) {
		g_warning ("use fu_device_add_guid(\"%s\") instead!", instance_id);
		fu_device_add_guid_safe (self, instance_id, flags);
		return;
	}

	/* it seems odd adding the instance ID and the GUID quirks and not just
	 * calling fu_device_add_guid_safe() -- but we want the quirks to match
	 * so the plugin is set, but not the LVFS metadata to match firmware
	 * until we're sure the device isn't using _NO_AUTO_INSTANCE_IDS */
	guid = fwupd_guid_hash_string (instance_id);
	if ((flags & FU_DEVICE_INSTANCE_FLAG_NO_QUIRKS) == 0)
		fu_device_add_guid_quirks (self, guid);
	if ((flags & FU_DEVICE_INSTANCE_FLAG_ONLY_QUIRKS) == 0)
		fwupd_device_add_instance_id (FWUPD_DEVICE (self), instance_id);

	/* already done by ->setup(), so this must be ->registered() */
	if (priv->done_setup)
		fwupd_device_add_guid (FWUPD_DEVICE (self), guid);
}

/**
 * fu_device_add_instance_id:
 * @self: a #FuDevice
 * @instance_id: the InstanceID, e.g. `PCI\VEN_10EC&DEV_525A`
 *
 * Adds an instance ID to the device. If the @instance_id argument is already a
 * valid GUID then fu_device_add_guid() should be used instead.
 *
 * Since: 1.2.5
 **/
void
fu_device_add_instance_id (FuDevice *self, const gchar *instance_id)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (instance_id != NULL);
	fu_device_add_instance_id_full (self, instance_id, FU_DEVICE_INSTANCE_FLAG_NONE);
}

/**
 * fu_device_add_guid:
 * @self: a #FuDevice
 * @guid: a GUID, e.g. `2082b5e0-7a64-478a-b1b2-e3404fab6dad`
 *
 * Adds a GUID to the device. If the @guid argument is not a valid GUID then it
 * is converted to a GUID using fwupd_guid_hash_string().
 *
 * Since: 0.7.2
 **/
void
fu_device_add_guid (FuDevice *self, const gchar *guid)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (guid != NULL);
	if (!fwupd_guid_is_valid (guid)) {
		fu_device_add_instance_id (self, guid);
		return;
	}
	fu_device_add_guid_safe (self, guid, FU_DEVICE_INSTANCE_FLAG_NONE);
}

/**
 * fu_device_add_guid_full:
 * @self: a #FuDevice
 * @guid: a GUID, e.g. `2082b5e0-7a64-478a-b1b2-e3404fab6dad`
 * @flags: instance ID flags
 *
 * Adds a GUID to the device. If the @guid argument is not a valid GUID then it
 * is converted to a GUID using fwupd_guid_hash_string().
 *
 * Since: 1.6.2
 **/
void
fu_device_add_guid_full (FuDevice *self, const gchar *guid, FuDeviceInstanceFlags flags)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (guid != NULL);
	if (!fwupd_guid_is_valid (guid)) {
		fu_device_add_instance_id_full (self, guid, flags);
		return;
	}
	fu_device_add_guid_safe (self, guid, flags);
}

/**
 * fu_device_add_counterpart_guid:
 * @self: a #FuDevice
 * @guid: a GUID, e.g. `2082b5e0-7a64-478a-b1b2-e3404fab6dad`
 *
 * Adds a GUID to the device. If the @guid argument is not a valid GUID then it
 * is converted to a GUID using fwupd_guid_hash_string().
 *
 * A counterpart GUID is typically the GUID of the same device in bootloader
 * or runtime mode, if they have a different device PCI or USB ID. Adding this
 * type of GUID does not cause a "cascade" by matching using the quirk database.
 *
 * Since: 1.1.2
 **/
void
fu_device_add_counterpart_guid (FuDevice *self, const gchar *guid)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (guid != NULL);

	/* make valid */
	if (!fwupd_guid_is_valid (guid)) {
		g_autofree gchar *tmp = fwupd_guid_hash_string (guid);
		fwupd_device_add_guid (FWUPD_DEVICE (self), tmp);
		return;
	}

	/* already valid */
	fwupd_device_add_guid (FWUPD_DEVICE (self), guid);
}

/**
 * fu_device_get_guids_as_str:
 * @self: a #FuDevice
 *
 * Gets the device GUIDs as a joined string, which may be useful for error
 * messages.
 *
 * Returns: a string, which may be empty length but not %NULL
 *
 * Since: 1.0.8
 **/
gchar *
fu_device_get_guids_as_str (FuDevice *self)
{
	GPtrArray *guids;
	g_autofree gchar **tmp = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);

	guids = fu_device_get_guids (self);
	tmp = g_new0 (gchar *, guids->len + 1);
	for (guint i = 0; i < guids->len; i++)
		tmp[i] = g_ptr_array_index (guids, i);
	return g_strjoinv (",", tmp);
}

/**
 * fu_device_get_metadata:
 * @self: a #FuDevice
 * @key: the key
 *
 * Gets an item of metadata from the device.
 *
 * Returns: a string value, or %NULL for unfound.
 *
 * Since: 0.1.0
 **/
const gchar *
fu_device_get_metadata (FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GRWLockReaderLocker) locker = g_rw_lock_reader_locker_new (&priv->metadata_mutex);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	g_return_val_if_fail (key != NULL, NULL);
	g_return_val_if_fail (locker != NULL, NULL);
	if (priv->metadata == NULL)
		return NULL;
	return g_hash_table_lookup (priv->metadata, key);
}

/**
 * fu_device_get_metadata_boolean:
 * @self: a #FuDevice
 * @key: the key
 *
 * Gets an item of metadata from the device.
 *
 * Returns: a boolean value, or %FALSE for unfound or failure to parse.
 *
 * Since: 0.9.7
 **/
gboolean
fu_device_get_metadata_boolean (FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	const gchar *tmp;
	g_autoptr(GRWLockReaderLocker) locker = g_rw_lock_reader_locker_new (&priv->metadata_mutex);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (key != NULL, FALSE);
	g_return_val_if_fail (locker != NULL, FALSE);

	if (priv->metadata == NULL)
		return FALSE;
	tmp = g_hash_table_lookup (priv->metadata, key);
	if (tmp == NULL)
		return FALSE;
	return g_strcmp0 (tmp, "true") == 0;
}

/**
 * fu_device_get_metadata_integer:
 * @self: a #FuDevice
 * @key: the key
 *
 * Gets an item of metadata from the device.
 *
 * Returns: a string value, or %G_MAXUINT for unfound or failure to parse.
 *
 * Since: 0.9.7
 **/
guint
fu_device_get_metadata_integer (FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	const gchar *tmp;
	gchar *endptr = NULL;
	guint64 val;
	g_autoptr(GRWLockReaderLocker) locker = g_rw_lock_reader_locker_new (&priv->metadata_mutex);

	g_return_val_if_fail (FU_IS_DEVICE (self), G_MAXUINT);
	g_return_val_if_fail (key != NULL, G_MAXUINT);
	g_return_val_if_fail (locker != NULL, G_MAXUINT);

	if (priv->metadata == NULL)
		return G_MAXUINT;
	tmp = g_hash_table_lookup (priv->metadata, key);
	if (tmp == NULL)
		return G_MAXUINT;
	val = g_ascii_strtoull (tmp, &endptr, 10);
	if (endptr != NULL && endptr[0] != '\0')
		return G_MAXUINT;
	if (val > G_MAXUINT)
		return G_MAXUINT;
	return (guint) val;
}

/**
 * fu_device_remove_metadata:
 * @self: a #FuDevice
 * @key: the key
 *
 * Removes an item of metadata on the device.
 *
 * Since: 1.3.3
 **/
void
fu_device_remove_metadata (FuDevice *self, const gchar *key)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GRWLockWriterLocker) locker = g_rw_lock_writer_locker_new (&priv->metadata_mutex);
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (key != NULL);
	g_return_if_fail (locker != NULL);
	if (priv->metadata == NULL)
		return;
	g_hash_table_remove (priv->metadata, key);
}

/**
 * fu_device_set_metadata:
 * @self: a #FuDevice
 * @key: the key
 * @value: the string value
 *
 * Sets an item of metadata on the device.
 *
 * Since: 0.1.0
 **/
void
fu_device_set_metadata (FuDevice *self, const gchar *key, const gchar *value)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(GRWLockWriterLocker) locker = g_rw_lock_writer_locker_new (&priv->metadata_mutex);
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (key != NULL);
	g_return_if_fail (value != NULL);
	g_return_if_fail (locker != NULL);
	if (priv->metadata == NULL) {
		priv->metadata = g_hash_table_new_full (g_str_hash, g_str_equal,
							g_free, g_free);
	}
	g_hash_table_insert (priv->metadata, g_strdup (key), g_strdup (value));
}

/**
 * fu_device_set_metadata_boolean:
 * @self: a #FuDevice
 * @key: the key
 * @value: the boolean value
 *
 * Sets an item of metadata on the device. When @value is set to %TRUE
 * the actual stored value is `true`.
 *
 * Since: 0.9.7
 **/
void
fu_device_set_metadata_boolean (FuDevice *self, const gchar *key, gboolean value)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (key != NULL);

	fu_device_set_metadata (self, key, value ? "true" : "false");
}

/**
 * fu_device_set_metadata_integer:
 * @self: a #FuDevice
 * @key: the key
 * @value: the unsigned integer value
 *
 * Sets an item of metadata on the device. The integer is stored as a
 * base-10 string internally.
 *
 * Since: 0.9.7
 **/
void
fu_device_set_metadata_integer (FuDevice *self, const gchar *key, guint value)
{
	g_autofree gchar *tmp = g_strdup_printf ("%u", value);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (key != NULL);

	fu_device_set_metadata (self, key, tmp);
}

/* ensure the name does not have the vendor name as the prefix */
static void
fu_device_fixup_vendor_name (FuDevice *self)
{
	const gchar *name = fu_device_get_name (self);
	const gchar *vendor = fu_device_get_vendor (self);
	if (name != NULL && vendor != NULL) {
		if (g_str_has_prefix (name, vendor)) {
			gsize vendor_len = strlen (vendor);
			g_autofree gchar *name1 = g_strdup (name + vendor_len);
			g_autofree gchar *name2 = fu_common_strstrip (name1);
			g_debug ("removing vendor prefix of '%s' from '%s'",
				 vendor, name);
			fwupd_device_set_name (FWUPD_DEVICE (self), name2);
		}
	}
}

/**
 * fu_device_set_vendor:
 * @self: a #FuDevice
 * @vendor: a device vendor
 *
 * Sets the vendor name on the device.
 *
 * Since: 1.6.2
 **/
void
fu_device_set_vendor (FuDevice *self, const gchar *vendor)
{
	fwupd_device_set_vendor (FWUPD_DEVICE (self), vendor);
	fu_device_fixup_vendor_name (self);
}

/**
 * fu_device_set_name:
 * @self: a #FuDevice
 * @value: a device name
 *
 * Sets the name on the device. Any invalid parts will be converted or removed.
 *
 * Since: 0.7.1
 **/
void
fu_device_set_name (FuDevice *self, const gchar *value)
{
	g_autoptr(GString) new = g_string_new (value);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (value != NULL);

	/* overwriting? */
	if (g_strcmp0 (value, fu_device_get_name (self)) == 0) {
		const gchar *id = fu_device_get_id (self);
		g_debug ("%s device overwriting same name value: %s",
			 id != NULL ? id : "unknown", value);
		return;
	}

	/* changing */
	if (fu_device_get_name (self) != NULL) {
		const gchar *id = fu_device_get_id (self);
		g_debug ("%s device overwriting name value: %s->%s",
			id != NULL ? id : "unknown",
			 fu_device_get_name (self),
			 value);
	}

	g_strdelimit (new->str, "_", ' ');
	fu_common_string_replace (new, "(TM)", "™");
	fwupd_device_set_name (FWUPD_DEVICE (self), new->str);
	fu_device_fixup_vendor_name (self);
}

/**
 * fu_device_set_id:
 * @self: a #FuDevice
 * @id: a string, e.g. `tbt-port1`
 *
 * Sets the ID on the device. The ID should represent the *connection* of the
 * device, so that any similar device plugged into a different slot will
 * have a different @id string.
 *
 * The @id will be converted to a SHA1 hash if required before the device is
 * added to the daemon, and plugins should not assume that the ID that is set
 * here is the same as what is returned by fu_device_get_id().
 *
 * Since: 0.7.1
 **/
void
fu_device_set_id (FuDevice *self, const gchar *id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	GPtrArray *children;
	g_autofree gchar *id_hash = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (id != NULL);

	/* allow sane device-id to be set directly */
	if (fwupd_device_id_is_valid (id)) {
		id_hash = g_strdup (id);
	} else {
		id_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, id, -1);
		g_debug ("using %s for %s", id_hash, id);
	}
	fwupd_device_set_id (FWUPD_DEVICE (self), id_hash);
	priv->device_id_valid = TRUE;

	/* ensure the parent ID is set */
	children = fu_device_get_children (self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *devtmp = g_ptr_array_index (children, i);
		fwupd_device_set_parent_id (FWUPD_DEVICE (devtmp), id_hash);
	}
}

/**
 * fu_device_set_version_format:
 * @self: a #FuDevice
 * @fmt: the version format, e.g. %FWUPD_VERSION_FORMAT_PLAIN
 *
 * Sets the device version format.
 *
 * Since: 1.4.0
 **/
void
fu_device_set_version_format (FuDevice *self, FwupdVersionFormat fmt)
{
	/* same */
	if (fu_device_get_version_format (self) == fmt)
		return;
	if (fu_device_get_version_format (self) != FWUPD_VERSION_FORMAT_UNKNOWN) {
		g_debug ("changing verfmt for %s: %s->%s",
			 fu_device_get_id (self),
			 fwupd_version_format_to_string (fu_device_get_version_format (self)),
			 fwupd_version_format_to_string (fmt));
	}
	fwupd_device_set_version_format (FWUPD_DEVICE (self), fmt);
}

/**
 * fu_device_set_version:
 * @self: a #FuDevice
 * @version: (nullable): a string, e.g. `1.2.3`
 *
 * Sets the device version, sanitizing the string if required.
 *
 * Since: 1.2.9
 **/
void
fu_device_set_version (FuDevice *self, const gchar *version)
{
	g_autofree gchar *version_safe = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));

	/* sanitize if required */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_ENSURE_SEMVER)) {
		version_safe = fu_common_version_ensure_semver (version);
		if (g_strcmp0 (version, version_safe) != 0)
			g_debug ("converted '%s' to '%s'", version, version_safe);
	} else {
		version_safe = g_strdup (version);
	}

	/* print a console warning for an invalid version, if semver */
	if (version_safe != NULL &&
	    !fu_common_version_verify_format (version_safe, fu_device_get_version_format (self), &error))
		g_warning ("%s", error->message);

	/* if different */
	if (g_strcmp0 (fu_device_get_version (self), version_safe) != 0) {
		if (fu_device_get_version (self) != NULL) {
			g_debug ("changing version for %s: %s->%s",
				 fu_device_get_id (self),
				 fu_device_get_version (self),
				 version_safe);
		}
		fwupd_device_set_version (FWUPD_DEVICE (self), version_safe);
	}
}

/**
 * fu_device_set_version_lowest:
 * @self: a #FuDevice
 * @version: (nullable): a string, e.g. `1.2.3`
 *
 * Sets the device lowest version, sanitizing the string if required.
 *
 * Since: 1.4.0
 **/
void
fu_device_set_version_lowest (FuDevice *self, const gchar *version)
{
	g_autofree gchar *version_safe = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));

	/* sanitize if required */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_ENSURE_SEMVER)) {
		version_safe = fu_common_version_ensure_semver (version);
		if (g_strcmp0 (version, version_safe) != 0)
			g_debug ("converted '%s' to '%s'", version, version_safe);
	} else {
		version_safe = g_strdup (version);
	}

	/* print a console warning for an invalid version, if semver */
	if (version_safe != NULL &&
	    !fu_common_version_verify_format (version_safe, fu_device_get_version_format (self), &error))
		g_warning ("%s", error->message);

	/* if different */
	if (g_strcmp0 (fu_device_get_version_lowest (self), version_safe) != 0) {
		if (fu_device_get_version_lowest (self) != NULL) {
			g_debug ("changing version lowest for %s: %s->%s",
				 fu_device_get_id (self),
				 fu_device_get_version_lowest (self),
				 version_safe);
		}
		fwupd_device_set_version_lowest (FWUPD_DEVICE (self), version_safe);
	}
}

/**
 * fu_device_set_version_bootloader:
 * @self: a #FuDevice
 * @version: (nullable): a string, e.g. `1.2.3`
 *
 * Sets the device bootloader version, sanitizing the string if required.
 *
 * Since: 1.4.0
 **/
void
fu_device_set_version_bootloader (FuDevice *self, const gchar *version)
{
	g_autofree gchar *version_safe = NULL;
	g_autoptr(GError) error = NULL;

	g_return_if_fail (FU_IS_DEVICE (self));

	/* sanitize if required */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_ENSURE_SEMVER)) {
		version_safe = fu_common_version_ensure_semver (version);
		if (g_strcmp0 (version, version_safe) != 0)
			g_debug ("converted '%s' to '%s'", version, version_safe);
	} else {
		version_safe = g_strdup (version);
	}

	/* print a console warning for an invalid version, if semver */
	if (version_safe != NULL &&
	    !fu_common_version_verify_format (version_safe, fu_device_get_version_format (self), &error))
		g_warning ("%s", error->message);

	/* if different */
	if (g_strcmp0 (fu_device_get_version_bootloader (self), version_safe) != 0) {
		if (fu_device_get_version_bootloader (self) != NULL) {
			g_debug ("changing version for %s: %s->%s",
				 fu_device_get_id (self),
				 fu_device_get_version_bootloader (self),
				 version_safe);
		}
		fwupd_device_set_version_bootloader (FWUPD_DEVICE (self), version_safe);
	}
}

static void
fu_device_inhibit_free (FuDeviceInhibit *inhibit)
{
	g_free (inhibit->inhibit_id);
	g_free (inhibit->reason);
	g_free (inhibit);
}

static void
fu_device_ensure_inhibits (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	guint nr_inhibits = g_hash_table_size (priv->inhibits);

	/* was okay -> not okay */
	if (fu_device_has_flag (self, FWUPD_DEVICE_FLAG_UPDATABLE) &&
	    nr_inhibits > 0) {
		g_autofree gchar *reasons_str = NULL;
		g_autoptr(GList) values = g_hash_table_get_values (priv->inhibits);
		g_autoptr(GPtrArray) reasons = g_ptr_array_new ();

		fu_device_remove_flag (self, FWUPD_DEVICE_FLAG_UPDATABLE);
		fu_device_add_flag (self, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN);

		/* update update error */
		for (GList *l = values; l != NULL; l = l->next) {
			FuDeviceInhibit *inhibit = (FuDeviceInhibit *) l->data;
			g_ptr_array_add (reasons, inhibit->reason);
		}
		reasons_str = fu_common_strjoin_array (", ", reasons);
		fu_device_set_update_error (self, reasons_str);
	}

	/* not okay -> is okay */
	if (fu_device_has_flag (self, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN) &&
	    nr_inhibits == 0) {
		fu_device_remove_flag (self, FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN);
		fu_device_add_flag (self, FWUPD_DEVICE_FLAG_UPDATABLE);
		fu_device_set_update_error (self, NULL);
	}
}

/**
 * fu_device_inhibit:
 * @self: a #FuDevice
 * @inhibit_id: an ID used for uninhibiting, e.g. `low-power`
 * @reason: (nullable): a string, e.g. `Cannot update as foo [bar] needs reboot`
 *
 * Prevent the device from being updated, changing it from %FWUPD_DEVICE_FLAG_UPDATABLE
 * to %FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN if not already inhibited.
 *
 * If the device already has an inhibit with the same @inhibit_id then the request
 * is ignored.
 *
 * Since: 1.6.0
 **/
void
fu_device_inhibit (FuDevice *self, const gchar *inhibit_id, const gchar *reason)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDeviceInhibit *inhibit;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (inhibit_id != NULL);

	/* lazy create as most devices will not need this */
	if (priv->inhibits == NULL) {
		priv->inhibits = g_hash_table_new_full (g_str_hash,
							g_str_equal,
							NULL,
							(GDestroyNotify) fu_device_inhibit_free);
	}

	/* already exists */
	inhibit = g_hash_table_lookup (priv->inhibits, inhibit_id);
	if (inhibit != NULL)
		return;

	/* create new */
	inhibit = g_new0 (FuDeviceInhibit, 1);
	inhibit->inhibit_id = g_strdup (inhibit_id);
	inhibit->reason = g_strdup (reason);
	g_hash_table_insert (priv->inhibits, inhibit->inhibit_id, inhibit);

	/* refresh */
	fu_device_ensure_inhibits (self);

	/* propagate to children */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_INHIBIT_CHILDREN)) {
		GPtrArray *children = fu_device_get_children (self);
		for (guint i = 0; i < children->len; i++) {
			FuDevice *child = g_ptr_array_index (children, i);
			fu_device_inhibit (child, inhibit_id, reason);
		}
	}
}

/**
 * fu_device_uninhibit:
 * @self: a #FuDevice
 * @inhibit_id: an ID used for uninhibiting, e.g. `low-power`
 *
 * Allow the device from being updated if there are no other inhibitors,
 * changing it from %FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN to %FWUPD_DEVICE_FLAG_UPDATABLE.
 *
 * If the device already has no inhibit with the @inhibit_id then the request
 * is ignored.
 *
 * Since: 1.6.0
 **/
void
fu_device_uninhibit (FuDevice *self, const gchar *inhibit_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (inhibit_id != NULL);

	if (priv->inhibits == NULL)
		return;
	if (g_hash_table_remove (priv->inhibits, inhibit_id))
		fu_device_ensure_inhibits (self);

	/* propagate to children */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_INHIBIT_CHILDREN)) {
		GPtrArray *children = fu_device_get_children (self);
		for (guint i = 0; i < children->len; i++) {
			FuDevice *child = g_ptr_array_index (children, i);
			fu_device_uninhibit (child, inhibit_id);
		}
	}
}

/**
 * fu_device_ensure_id:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * If not already set, generates a device ID with the optional physical and
 * logical IDs.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_ensure_id (FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autofree gchar *device_id = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* already set */
	if (priv->device_id_valid)
		return TRUE;

	/* nothing we can do! */
	if (priv->physical_id == NULL) {
		g_autofree gchar *tmp = fu_device_to_string (self);
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_FAILED,
			     "cannot ensure ID: %s", tmp);
		return FALSE;
	}

	/* logical may be NULL */
	device_id = g_strjoin (":",
			       fu_device_get_physical_id (self),
			       fu_device_get_logical_id (self),
			       NULL);
	fu_device_set_id (self, device_id);
	return TRUE;
}

/**
 * fu_device_get_logical_id:
 * @self: a #FuDevice
 *
 * Gets the logical ID set for the device, which disambiguates devices with the
 * same physical ID.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.2
 **/
const gchar *
fu_device_get_logical_id (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->logical_id;
}

/**
 * fu_device_set_logical_id:
 * @self: a #FuDevice
 * @logical_id: a string, e.g. `dev2`
 *
 * Sets the logical ID on the device. This is designed to disambiguate devices
 * with the same physical ID.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_logical_id (FuDevice *self, const gchar *logical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->logical_id, logical_id) == 0)
		return;

	/* not allowed after ->probe() and ->setup() have completed */
	if (priv->done_setup) {
		g_warning ("cannot change %s logical ID from %s to %s as "
			   "FuDevice->setup() has already completed",
			   fu_device_get_id (self),
			   priv->logical_id,
			   logical_id);
		return;
	}

	g_free (priv->logical_id);
	priv->logical_id = g_strdup (logical_id);
	priv->device_id_valid = FALSE;
	g_object_notify (G_OBJECT (self), "logical-id");
}

/**
 * fu_device_get_backend_id:
 * @self: a #FuDevice
 *
 * Gets the ID set for the device as recognized by the backend. This is typically
 * a Linux sysfs path or USB platform ID. If unset, it also falls back to the
 * physical ID as this may be the same value.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.5.8
 **/
const gchar *
fu_device_get_backend_id (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	if (priv->backend_id != NULL)
		return priv->backend_id;
	return priv->physical_id;
}

/**
 * fu_device_set_backend_id:
 * @self: a #FuDevice
 * @backend_id: a string, e.g. `dev2`
 *
 * Sets the backend ID on the device. This is designed to disambiguate devices
 * with the same physical ID. This is typically a Linux sysfs path or USB
 * platform ID.
 *
 * Since: 1.5.8
 **/
void
fu_device_set_backend_id (FuDevice *self, const gchar *backend_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->backend_id, backend_id) == 0)
		return;

	g_free (priv->backend_id);
	priv->backend_id = g_strdup (backend_id);
	priv->device_id_valid = FALSE;
	g_object_notify (G_OBJECT (self), "backend-id");
}

/**
 * fu_device_get_proxy_guid:
 * @self: a #FuDevice
 *
 * Gets the proxy GUID device, which which is set to let the engine match up the
 * proxy between plugins.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.4.1
 **/
const gchar *
fu_device_get_proxy_guid (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->proxy_guid;
}

/**
 * fu_device_set_proxy_guid:
 * @self: a #FuDevice
 * @proxy_guid: a string, e.g. `USB\VID_413C&PID_B06E&hub`
 *
 * Sets the GUID of the proxy device. The proxy device may update @self.
 *
 * Since: 1.4.1
 **/
void
fu_device_set_proxy_guid (FuDevice *self, const gchar *proxy_guid)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));

	/* not changed */
	if (g_strcmp0 (priv->proxy_guid, proxy_guid) == 0)
		return;

	g_free (priv->proxy_guid);
	priv->proxy_guid = g_strdup (proxy_guid);
}

/**
 * fu_device_set_physical_id:
 * @self: a #FuDevice
 * @physical_id: a string that identifies the physical device connection
 *
 * Sets the physical ID on the device which represents the electrical connection
 * of the device to the system. Multiple #FuDevices can share a physical ID.
 *
 * The physical ID is used to remove logical devices when a physical device has
 * been removed from the system.
 *
 * A sysfs or devpath is not a physical ID, but could be something like
 * `PCI_SLOT_NAME=0000:3e:00.0`.
 *
 * Since: 1.1.2
 **/
void
fu_device_set_physical_id (FuDevice *self, const gchar *physical_id)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (physical_id != NULL);

	/* not changed */
	if (g_strcmp0 (priv->physical_id, physical_id) == 0)
		return;

	/* not allowed after ->probe() and ->setup() have completed */
	if (priv->done_setup) {
		g_warning ("cannot change %s physical ID from %s to %s as "
			   "FuDevice->setup() has already completed",
			   fu_device_get_id (self),
			   priv->physical_id,
			   physical_id);
		return;
	}

	g_free (priv->physical_id);
	priv->physical_id = g_strdup (physical_id);
	priv->device_id_valid = FALSE;
	g_object_notify (G_OBJECT (self), "physical-id");
}

/**
 * fu_device_get_physical_id:
 * @self: a #FuDevice
 *
 * Gets the physical ID set for the device, which represents the electrical
 * connection used to compare devices.
 *
 * Multiple #FuDevices can share a single physical ID.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.2
 **/
const gchar *
fu_device_get_physical_id (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->physical_id;
}

/**
 * fu_device_remove_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Removes a device flag from the device.
 *
 * Since: 1.6.0
 **/
void
fu_device_remove_flag (FuDevice *self, FwupdDeviceFlags flag)
{
	/* proxy */
	fwupd_device_remove_flag (FWUPD_DEVICE (self), flag);

	/* allow it to be updatable again */
	if (flag & FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)
		fu_device_uninhibit (self, "needs-activation");
}

/**
 * fu_device_add_flag:
 * @self: a #FuDevice
 * @flag: a device flag
 *
 * Adds a device flag to the device.
 *
 * Since: 0.1.0
 **/
void
fu_device_add_flag (FuDevice *self, FwupdDeviceFlags flag)
{
	/* none is not used as an "exported" flag */
	if (flag == FWUPD_DEVICE_FLAG_NONE)
		return;

	/* being both a bootloader and requiring a bootloader is invalid */
	if (flag & FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER)
		fu_device_remove_flag (self, FWUPD_DEVICE_FLAG_IS_BOOTLOADER);
	if (flag & FWUPD_DEVICE_FLAG_IS_BOOTLOADER)
		fu_device_remove_flag (self, FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER);

	/* one implies the other */
	if (flag & FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE)
		flag |= FWUPD_DEVICE_FLAG_CAN_VERIFY;
	if (flag & FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES)
		flag |= FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED;
	fwupd_device_add_flag (FWUPD_DEVICE (self), flag);

	/* activatable devices shouldn't be allowed to update again until activated */
	/* don't let devices be updated until activated */
	if (flag & FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)
		fu_device_inhibit (self, "needs-activation", "Pending activation");
}

typedef struct {
	guint64		 value;
	gchar		*value_str;
} FuDevicePrivateFlagItem;

static void
fu_device_private_flag_item_free (FuDevicePrivateFlagItem *item)
{
	g_free (item->value_str);
	g_free (item);
}

static FuDevicePrivateFlagItem *
fu_device_private_flag_item_find_by_str (FuDevice *self, const gchar *value_str)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	if (priv->private_flag_items == NULL)
		return NULL;
	for (guint i = 0; i < priv->private_flag_items->len; i++) {
		FuDevicePrivateFlagItem *item = g_ptr_array_index (priv->private_flag_items, i);
		if (g_strcmp0 (item->value_str, value_str) == 0)
			return item;
	}
	return NULL;
}

static FuDevicePrivateFlagItem *
fu_device_private_flag_item_find_by_val (FuDevice *self, guint64 value)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	if (priv->private_flag_items == NULL)
		return NULL;
	for (guint i = 0; i < priv->private_flag_items->len; i++) {
		FuDevicePrivateFlagItem *item = g_ptr_array_index (priv->private_flag_items, i);
		if (item->value == value)
			return item;
	}
	return NULL;
}

/**
 * fu_device_register_private_flag:
 * @self: a #FuDevice
 * @value: an integer value
 * @value_str: a string that represents @value
 *
 * Registers a private device flag so that it can be set from quirk files.
 *
 * Since: 1.6.2
 **/
void
fu_device_register_private_flag (FuDevice *self,
				 guint64 value,
				 const gchar *value_str)
{
	FuDevicePrivateFlagItem *item;
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (value != 0);
	g_return_if_fail (value_str != NULL);

	/* ensure exists */
	if (priv->private_flag_items == NULL)
		priv->private_flag_items = g_ptr_array_new_with_free_func ((GDestroyNotify) fu_device_private_flag_item_free);

	item = g_new0 (FuDevicePrivateFlagItem, 1);
	item->value = value;
	item->value_str = g_strdup (value_str);
	g_ptr_array_add (priv->private_flag_items, item);
}

static void
fu_device_set_custom_flag (FuDevice *self, const gchar *hint)
{
	FwupdDeviceFlags flag;
	FuDevicePrivateFlagItem *item;
	FuDeviceInternalFlags internal_flag;
	FuDevicePrivate *priv = GET_PRIVATE (self);

	/* is this a negated device flag */
	if (g_str_has_prefix (hint, "~")) {
		flag = fwupd_device_flag_from_string (hint + 1);
		if (flag != FWUPD_DEVICE_FLAG_UNKNOWN) {
			fu_device_remove_flag (self, flag);
			return;
		}
		internal_flag = fu_device_internal_flag_from_string (hint + 1);
		if (internal_flag != FU_DEVICE_INTERNAL_FLAG_UNKNOWN) {
			fu_device_remove_internal_flag (self, internal_flag);
			return;
		}
		item = fu_device_private_flag_item_find_by_str (self, hint + 1);
		if (item != NULL) {
			priv->private_flags &= ~item->value;
			return;
		}
		g_debug ("failed to find registered custom flag %s", hint + 1);
		return;
	}

	/* is this a known device flag */
	flag = fwupd_device_flag_from_string (hint);
	if (flag != FWUPD_DEVICE_FLAG_UNKNOWN) {
		fu_device_add_flag (self, flag);
		return;
	}
	internal_flag = fu_device_internal_flag_from_string (hint);
	if (internal_flag != FU_DEVICE_INTERNAL_FLAG_UNKNOWN) {
		fu_device_add_internal_flag (self, internal_flag);
		return;
	}
	item = fu_device_private_flag_item_find_by_str (self, hint);
	if (item != NULL) {
		priv->private_flags |= item->value;
		return;
	}
	g_debug ("failed to find registered custom flag %s", hint);
}

/**
 * fu_device_set_custom_flags:
 * @self: a #FuDevice
 * @custom_flags: a string
 *
 * Sets the custom flags from the quirk system that can be used to
 * affect device matching. The actual string format is defined by the plugin.
 *
 * Since: 1.1.0
 **/
void
fu_device_set_custom_flags (FuDevice *self, const gchar *custom_flags)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (custom_flags != NULL);

	/* display what was set when converting to a string */
	fu_device_set_metadata (self, "CustomFlags", custom_flags);

	/* look for any standard FwupdDeviceFlags */
	if (custom_flags != NULL) {
		g_auto(GStrv) hints = g_strsplit (custom_flags, ",", -1);
		for (guint i = 0; hints[i] != NULL; i++)
			fu_device_set_custom_flag (self, hints[i]);
	}
}

/**
 * fu_device_get_custom_flags:
 * @self: a #FuDevice
 *
 * Gets the custom flags for the device from the quirk system.
 *
 * Returns: a string value, or %NULL if never set.
 *
 * Since: 1.1.0
 **/
const gchar *
fu_device_get_custom_flags (FuDevice *self)
{
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return fu_device_get_metadata (self, "CustomFlags");
}

/**
 * fu_device_has_custom_flag:
 * @self: a #FuDevice
 * @hint: a string, e.g. `bootloader`
 *
 * Checks if the custom flag exists for the device from the quirk system.
 *
 * It may be more efficient to call fu_device_get_custom_flags() and split the
 * string locally if checking for lots of different flags.
 *
 * Returns: %TRUE if the hint exists
 *
 * Since: 1.1.0
 **/
gboolean
fu_device_has_custom_flag (FuDevice *self, const gchar *hint)
{
	const gchar *hint_str;
	g_auto(GStrv) hints = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (hint != NULL, FALSE);

	/* no hint is perfectly valid */
	hint_str = fu_device_get_custom_flags (self);
	if (hint_str == NULL)
		return FALSE;
	hints = g_strsplit (hint_str, ",", -1);
	return g_strv_contains ((const gchar * const *) hints, hint);
}

/**
 * fu_device_get_remove_delay:
 * @self: a #FuDevice
 *
 * Returns the maximum delay expected when replugging the device going into
 * bootloader mode.
 *
 * Returns: time in milliseconds
 *
 * Since: 1.0.2
 **/
guint
fu_device_get_remove_delay (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return priv->remove_delay;
}

/**
 * fu_device_set_remove_delay:
 * @self: a #FuDevice
 * @remove_delay: the delay value
 *
 * Sets the amount of time a device is allowed to return in bootloader mode.
 *
 * NOTE: this should be less than 3000ms for devices that just have to reset
 * and automatically re-enumerate, but significantly longer if it involves a
 * user removing a cable, pressing several buttons and removing a cable.
 * A suggested value for this would be 10,000ms.
 *
 * Since: 1.0.2
 **/
void
fu_device_set_remove_delay (FuDevice *self, guint remove_delay)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->remove_delay = remove_delay;
}

/**
 * fu_device_get_status:
 * @self: a #FuDevice
 *
 * Returns what the device is currently doing.
 *
 * Returns: the status value, e.g. %FWUPD_STATUS_DEVICE_WRITE
 *
 * Since: 1.0.3
 **/
FwupdStatus
fu_device_get_status (FuDevice *self)
{
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return fwupd_device_get_status (FWUPD_DEVICE (self));
}

/**
 * fu_device_set_status:
 * @self: a #FuDevice
 * @status: the status value, e.g. %FWUPD_STATUS_DEVICE_WRITE
 *
 * Sets what the device is currently doing.
 *
 * Since: 1.0.3
 **/
void
fu_device_set_status (FuDevice *self, FwupdStatus status)
{
	g_return_if_fail (FU_IS_DEVICE (self));
	fwupd_device_set_status (FWUPD_DEVICE (self), status);
}

/**
 * fu_device_get_progress:
 * @self: a #FuDevice
 *
 * Returns the progress completion.
 *
 * Returns: value in percent
 *
 * Since: 1.0.3
 **/
guint
fu_device_get_progress (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), 0);
	return priv->progress;
}

/**
 * fu_device_set_progress:
 * @self: a #FuDevice
 * @progress: the progress percentage value
 *
 * Sets the progress completion.
 *
 * Since: 1.0.3
 **/
void
fu_device_set_progress (FuDevice *self, guint progress)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	if (priv->progress == progress)
		return;
	priv->progress = progress;
	g_object_notify (G_OBJECT (self), "progress");
}

/**
 * fu_device_set_progress_full:
 * @self: a #FuDevice
 * @progress_done: the bytes already done
 * @progress_total: the total number of bytes
 *
 * Sets the progress completion using the raw progress values.
 *
 * Since: 1.0.3
 **/
void
fu_device_set_progress_full (FuDevice *self, gsize progress_done, gsize progress_total)
{
	gdouble percentage = 0.f;
	g_return_if_fail (FU_IS_DEVICE (self));
	if (progress_total > 0)
		percentage = (100.f * (gdouble) progress_done) / (gdouble) progress_total;
	fu_device_set_progress (self, (guint) percentage);
}

/**
 * fu_device_sleep_with_progress:
 * @self: a #FuDevice
 * @delay_secs: the delay in seconds
 *
 * Sleeps, setting the device progress from 0..100% as time continues.
 * The value is gven in whole seconds as it does not make sense to show the
 * progressbar advancing so quickly for durations of less than one second.
 *
 * Since: 1.5.0
 **/
void
fu_device_sleep_with_progress (FuDevice *self, guint delay_secs)
{
	gulong delay_us_pc = (delay_secs * G_USEC_PER_SEC) / 100;

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (delay_secs > 0);

	fu_device_set_progress (self, 0);
	for (guint i = 0; i < 100; i++) {
		g_usleep (delay_us_pc);
		fu_device_set_progress (self, i + 1);
	}
}

static void
fu_device_ensure_battery_inhibit (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	if (priv->battery_level == FU_BATTERY_VALUE_INVALID ||
	    priv->battery_level >= fu_device_get_battery_threshold (self)) {
		fu_device_uninhibit (self, "battery");
		return;
	}
	fu_device_inhibit (self, "battery", "Battery level is too low");
}

/**
 * fu_device_get_battery_level:
 * @self: a #FuDevice
 *
 * Returns the battery level.
 *
 * Returns: value in percent
 *
 * Since: 1.5.8
 **/
guint
fu_device_get_battery_level (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), FU_BATTERY_VALUE_INVALID);
	return priv->battery_level;
}

/**
 * fu_device_set_battery_level:
 * @self: a #FuDevice
 * @battery_level: the percentage value
 *
 * Sets the battery level, or %FU_BATTERY_VALUE_INVALID.
 *
 * Setting this allows fwupd to show a warning if the device change is too low
 * to perform the update.
 *
 * Since: 1.5.8
 **/
void
fu_device_set_battery_level (FuDevice *self, guint battery_level)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (battery_level <= FU_BATTERY_VALUE_INVALID);
	if (priv->battery_level == battery_level)
		return;
	priv->battery_level = battery_level;
	g_object_notify (G_OBJECT (self), "battery-level");
	fu_device_ensure_battery_inhibit (self);
}

/**
 * fu_device_get_battery_threshold:
 * @self: a #FuDevice
 *
 * Returns the battery threshold under which a firmware update cannot be
 * performed.
 *
 * If fu_device_set_battery_threshold() has not been used, a default value is
 * used instead.
 *
 * Returns: value in percent
 *
 * Since: 1.6.0
 **/
guint
fu_device_get_battery_threshold (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), FU_BATTERY_VALUE_INVALID);

	/* default value */
	if (priv->battery_threshold == FU_BATTERY_VALUE_INVALID)
		return FU_DEVICE_DEFAULT_BATTERY_THRESHOLD;

	return priv->battery_threshold;
}

/**
 * fu_device_set_battery_threshold:
 * @self: a #FuDevice
 * @battery_threshold: the percentage value
 *
 * Sets the battery level, or %FU_BATTERY_VALUE_INVALID for the default.
 *
 * Setting this allows fwupd to show a warning if the device change is too low
 * to perform the update.
 *
 * Since: 1.6.0
 **/
void
fu_device_set_battery_threshold (FuDevice *self, guint battery_threshold)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (battery_threshold <= FU_BATTERY_VALUE_INVALID);
	if (priv->battery_threshold == battery_threshold)
		return;
	priv->battery_threshold = battery_threshold;
	g_object_notify (G_OBJECT (self), "battery-threshold");
	fu_device_ensure_battery_inhibit (self);
}

static void
fu_device_add_string (FuDevice *self, guint idt, GString *str)
{
	GPtrArray *children;
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autofree gchar *tmp = NULL;
	g_autoptr(GRWLockReaderLocker) locker = g_rw_lock_reader_locker_new (&priv->metadata_mutex);

	g_return_if_fail (locker != NULL);

	/* subclassed type */
	fu_common_string_append_kv (str, idt, G_OBJECT_TYPE_NAME (self), NULL);

	tmp = fwupd_device_to_string (FWUPD_DEVICE (self));
	if (tmp != NULL && tmp[0] != '\0')
		g_string_append (str, tmp);
	if (priv->alternate_id != NULL)
		fu_common_string_append_kv (str, idt + 1, "AlternateId", priv->alternate_id);
	if (priv->equivalent_id != NULL)
		fu_common_string_append_kv (str, idt + 1, "EquivalentId", priv->equivalent_id);
	if (priv->physical_id != NULL)
		fu_common_string_append_kv (str, idt + 1, "PhysicalId", priv->physical_id);
	if (priv->logical_id != NULL)
		fu_common_string_append_kv (str, idt + 1, "LogicalId", priv->logical_id);
	if (priv->backend_id != NULL)
		fu_common_string_append_kv (str, idt + 1, "BackendId", priv->backend_id);
	if (priv->proxy != NULL)
		fu_common_string_append_kv (str, idt + 1, "ProxyId", fu_device_get_id (priv->proxy));
	if (priv->proxy_guid != NULL)
		fu_common_string_append_kv (str, idt + 1, "ProxyGuid", priv->proxy_guid);
	if (priv->battery_level != FU_BATTERY_VALUE_INVALID)
		fu_common_string_append_ku (str, idt + 1, "BatteryLevel", priv->battery_level);
	if (priv->battery_threshold != FU_BATTERY_VALUE_INVALID)
		fu_common_string_append_ku (str, idt + 1, "BatteryThreshold", priv->battery_threshold);
	if (priv->size_min > 0) {
		g_autofree gchar *sz = g_strdup_printf ("%" G_GUINT64_FORMAT, priv->size_min);
		fu_common_string_append_kv (str, idt + 1, "FirmwareSizeMin", sz);
	}
	if (priv->size_max > 0) {
		g_autofree gchar *sz = g_strdup_printf ("%" G_GUINT64_FORMAT, priv->size_max);
		fu_common_string_append_kv (str, idt + 1, "FirmwareSizeMax", sz);
	}
	if (priv->order != G_MAXINT) {
		g_autofree gchar *order = g_strdup_printf ("%i", priv->order);
		fu_common_string_append_kv (str, idt + 1, "Order", order);
	}
	if (priv->priority > 0)
		fu_common_string_append_ku (str, idt + 1, "Priority", priv->priority);
	if (priv->metadata != NULL) {
		g_autoptr(GList) keys = g_hash_table_get_keys (priv->metadata);
		for (GList *l = keys; l != NULL; l = l->next) {
			const gchar *key = l->data;
			const gchar *value = g_hash_table_lookup (priv->metadata, key);
			fu_common_string_append_kv (str, idt + 1, key, value);
		}
	}
	for (guint i = 0; i < priv->possible_plugins->len; i++) {
		const gchar *name = g_ptr_array_index (priv->possible_plugins, i);
		fu_common_string_append_kv (str, idt + 1, "PossiblePlugin", name);
	}
	if (priv->parent_physical_ids != NULL && priv->parent_physical_ids->len > 0) {
		g_autofree gchar *flags = fu_common_strjoin_array (",", priv->parent_physical_ids);
		fu_common_string_append_kv (str, idt + 1, "ParentPhysicalIds", flags);
	}
	if (priv->internal_flags != FU_DEVICE_INTERNAL_FLAG_NONE) {
		g_autoptr(GString) tmp2 = g_string_new ("");
		for (guint i = 0; i < 64; i++) {
			if ((priv->internal_flags & ((guint64) 1 << i)) == 0)
				continue;
			g_string_append_printf (tmp2, "%s|",
						fu_device_internal_flag_to_string ((guint64) 1 << i));
		}
		if (tmp2->len > 0)
			g_string_truncate (tmp2, tmp2->len - 1);
		fu_common_string_append_kv (str, idt + 1, "InternalFlags", tmp2->str);
	}
	if (priv->private_flags > 0) {
		g_autoptr(GPtrArray) tmpv = g_ptr_array_new ();
		g_autofree gchar *tmps = NULL;
		for (guint64 i = 0; i < 64; i++) {
			FuDevicePrivateFlagItem *item;
			guint64 value = 1ull << i;
			if ((priv->private_flags & value) == 0)
				continue;
			item = fu_device_private_flag_item_find_by_val (self, value);
			if (item == NULL)
				continue;
			g_ptr_array_add (tmpv, item->value_str);
		}
		tmps = fu_common_strjoin_array (",", tmpv);
		fu_common_string_append_kv (str, idt + 1, "PrivateFlags", tmps);
	}

	/* subclassed */
	if (klass->to_string != NULL)
		klass->to_string (self, idt + 1, str);

	/* print children also */
	children = fu_device_get_children (self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child = g_ptr_array_index (children, i);
		fu_device_add_string (child, idt + 1, str);
	}
}

/**
 * fu_device_to_string:
 * @self: a #FuDevice
 *
 * This allows us to easily print the device, the release and the
 * daemon-specific metadata.
 *
 * Returns: a string value, or %NULL for invalid.
 *
 * Since: 0.9.8
 **/
gchar *
fu_device_to_string (FuDevice *self)
{
	GString *str = g_string_new (NULL);
	fu_device_add_string (self, 0, str);
	return g_string_free (str, FALSE);
}

/**
 * fu_device_set_context:
 * @self: a #FuDevice
 * @ctx: (nullable): optional #FuContext
 *
 * Sets the optional context which may be useful to this device.
 * This is typically set after the device has been created, but before
 * the device has been opened or probed.
 *
 * Since: 1.6.0
 **/
void
fu_device_set_context (FuDevice *self, FuContext *ctx)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	if (g_set_object (&priv->ctx, ctx))
		g_object_notify (G_OBJECT (self), "context");
}

/**
 * fu_device_get_context:
 * @self: a #FuDevice
 *
 * Gets the context assigned for this device.
 *
 * Returns: (transfer none): the #FuContext object, or %NULL
 *
 * Since: 1.6.0
 **/
FuContext *
fu_device_get_context (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	return priv->ctx;
}

/**
 * fu_device_get_release_default:
 * @self: a #FuDevice
 *
 * Gets the default release for the device, creating one if not found.
 *
 * Returns: (transfer none): the #FwupdRelease object
 *
 * Since: 1.0.5
 **/
FwupdRelease *
fu_device_get_release_default (FuDevice *self)
{
	g_autoptr(FwupdRelease) rel = NULL;
	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	if (fwupd_device_get_release_default (FWUPD_DEVICE (self)) != NULL)
		return fwupd_device_get_release_default (FWUPD_DEVICE (self));
	rel = fwupd_release_new ();
	fwupd_device_add_release (FWUPD_DEVICE (self), rel);
	return rel;
}

/**
 * fu_device_write_firmware:
 * @self: a #FuDevice
 * @fw: firmware blob
 * @flags: install flags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Writes firmware to the device by calling a plugin-specific vfunc.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_write_firmware (FuDevice *self,
			  GBytes *fw,
			  FwupdInstallFlags flags,
			  GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	g_autoptr(FuFirmware) firmware = NULL;
	g_autofree gchar *str = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (klass->write_firmware == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "not supported");
		return FALSE;
	}

	/* prepare (e.g. decompress) firmware */
	firmware = fu_device_prepare_firmware (self, fw, flags, error);
	if (firmware == NULL)
		return FALSE;
	str = fu_firmware_to_string (firmware);
	g_debug ("installing onto %s:\n%s", fu_device_get_id (self), str);

	/* call vfunc */
	return klass->write_firmware (self, firmware, flags, error);
}

/**
 * fu_device_prepare_firmware:
 * @self: a #FuDevice
 * @fw: firmware blob
 * @flags: install flags, e.g. %FWUPD_INSTALL_FLAG_FORCE
 * @error: (nullable): optional return location for an error
 *
 * Prepares the firmware by calling an optional device-specific vfunc for the
 * device, which can do things like decompressing or parsing of the firmware
 * data.
 *
 * For all firmware, this checks the size of the firmware if limits have been
 * set using fu_device_set_firmware_size_min(), fu_device_set_firmware_size_max()
 * or using a quirk entry.
 *
 * Returns: (transfer full): a new #GBytes, or %NULL for error
 *
 * Since: 1.1.2
 **/
FuFirmware *
fu_device_prepare_firmware (FuDevice *self,
			    GBytes *fw,
			    FwupdInstallFlags flags,
			    GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_autoptr(FuFirmware) firmware = NULL;
	g_autoptr(GBytes) fw_def = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	g_return_val_if_fail (fw != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* optionally subclassed */
	if (klass->prepare_firmware != NULL) {
		fu_device_set_status (self, FWUPD_STATUS_DECOMPRESSING);
		firmware = klass->prepare_firmware (self, fw, flags, error);
		if (firmware == NULL)
			return NULL;
	} else {
		firmware = fu_firmware_new_from_bytes (fw);
	}

	/* check size */
	fw_def = fu_firmware_get_bytes (firmware, NULL);
	if (fw_def != NULL) {
		guint64 fw_sz = (guint64) g_bytes_get_size (fw_def);
		if (priv->size_max > 0 && fw_sz > priv->size_max) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "firmware is %04x bytes larger than the allowed "
				     "maximum size of %04x bytes",
				     (guint) (fw_sz - priv->size_max),
				     (guint) priv->size_max);
			return NULL;
		}
		if (priv->size_min > 0 && fw_sz < priv->size_min) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "firmware is %04x bytes smaller than the allowed "
				     "minimum size of %04x bytes",
				     (guint) (priv->size_min - fw_sz),
				     (guint) priv->size_max);
			return NULL;
		}
	}

	/* success */
	return g_steal_pointer (&firmware);
}

/**
 * fu_device_read_firmware:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Reads firmware from the device by calling a plugin-specific vfunc.
 * The device subclass should try to ensure the firmware does not contain any
 * serial numbers or user-configuration values and can be used to calculate the
 * device checksum.
 *
 * The return value can be converted to a blob of memory using fu_firmware_write().
 *
 * Returns: (transfer full): a #FuFirmware, or %NULL for error
 *
 * Since: 1.0.8
 **/
FuFirmware *
fu_device_read_firmware (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	g_autoptr(GBytes) fw = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* device does not support reading for verification CRCs */
	if (!fu_device_has_flag (self, FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "not supported");
		return NULL;
	}

	/* call vfunc */
	if (klass->read_firmware != NULL)
		return klass->read_firmware (self, error);

	/* use the default FuFirmware when only ->dump_firmware is provided */
	fw = fu_device_dump_firmware (self, error);
	if (fw == NULL)
		return NULL;
	return fu_firmware_new_from_bytes (fw);
}

/**
 * fu_device_dump_firmware:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Reads the raw firmware image from the device by calling a plugin-specific
 * vfunc. This raw firmware image may contain serial numbers or device-specific
 * configuration but should be a byte-for-byte match compared to using an
 * external SPI programmer.
 *
 * Returns: (transfer full): a #GBytes, or %NULL for error
 *
 * Since: 1.5.0
 **/
GBytes *
fu_device_dump_firmware (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* use the default FuFirmware when only ->dump_firmware is provided */
	if (klass->dump_firmware == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "not supported");
		return NULL;
	}

	/* proxy */
	return klass->dump_firmware (self, error);
}

/**
 * fu_device_detach:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Detaches a device from the application into bootloader mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_detach (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (klass->detach == NULL)
		return TRUE;

	/* call vfunc */
	return klass->detach (self, error);
}

/**
 * fu_device_attach:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Attaches a device from the bootloader into application mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.0.8
 **/
gboolean
fu_device_attach (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (klass->attach == NULL)
		return TRUE;

	/* call vfunc */
	return klass->attach (self, error);
}

/**
 * fu_device_reload:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Reloads a device that has just gone from bootloader into application mode.
 *
 * Returns: %TRUE on success
 *
 * Since: 1.3.3
 **/
gboolean
fu_device_reload (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (klass->reload == NULL)
		return TRUE;

	/* call vfunc */
	return klass->reload (self, error);
}

/**
 * fu_device_prepare:
 * @self: a #FuDevice
 * @flags: install flags
 * @error: (nullable): optional return location for an error
 *
 * Prepares a device for update. A different plugin can handle each of
 * FuDevice->prepare(), FuDevice->detach() and FuDevice->write_firmware().
 *
 * Returns: %TRUE on success
 *
 * Since: 1.3.3
 **/
gboolean
fu_device_prepare (FuDevice *self, FwupdInstallFlags flags, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (klass->prepare == NULL)
		return TRUE;

	/* call vfunc */
	return klass->prepare (self, flags, error);
}

/**
 * fu_device_cleanup:
 * @self: a #FuDevice
 * @flags: install flags
 * @error: (nullable): optional return location for an error
 *
 * Cleans up a device after an update. A different plugin can handle each of
 * FuDevice->write_firmware(), FuDevice->attach() and FuDevice->cleanup().
 *
 * Returns: %TRUE on success
 *
 * Since: 1.3.3
 **/
gboolean
fu_device_cleanup (FuDevice *self, FwupdInstallFlags flags, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* no plugin-specific method */
	if (klass->cleanup == NULL)
		return TRUE;

	/* call vfunc */
	return klass->cleanup (self, flags, error);
}

static gboolean
fu_device_open_cb (FuDevice *self, gpointer user_data, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	return klass->open (self, error);
}

static gboolean
fu_device_open_internal (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	FuDevicePrivate *priv = GET_PRIVATE (self);

	/* already open */
	g_atomic_int_inc (&priv->open_refcount);
	if (priv->open_refcount > 1)
		return TRUE;

	/* probe */
	if (!fu_device_probe (self, error))
		return FALSE;

	/* ensure the device ID is already setup */
	if (!fu_device_ensure_id (self, error))
		return FALSE;

	/* subclassed */
	if (klass->open != NULL) {
		if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_RETRY_OPEN)) {
			if (!fu_device_retry_full (self, fu_device_open_cb,
						   FU_DEVICE_RETRY_OPEN_COUNT,
						   FU_DEVICE_RETRY_OPEN_DELAY,
						   NULL, error))
				return FALSE;
		} else {
			if (!klass->open (self, error))
				return FALSE;
		}
	}

	/* setup */
	if (!fu_device_setup (self, error))
		return FALSE;

	/* ensure the device ID is still valid */
	if (!fu_device_ensure_id (self, error))
		return FALSE;

	/* success */
	fu_device_add_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_IS_OPEN);
	return TRUE;
}

/**
 * fu_device_open:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Opens a device, optionally running a object-specific vfunc.
 *
 * Plugins can call fu_device_open() multiple times without calling
 * fu_device_close(), but only the first call will actually invoke the vfunc.
 *
 * It is expected that plugins issue the same number of fu_device_open() and
 * fu_device_close() methods when using a specific @self.
 *
 * If the `->probe()`, `->open()` and `->setup()` actions all complete
 * successfully the internal device flag %FU_DEVICE_INTERNAL_FLAG_IS_OPEN will
 * be set.
 *
 * NOTE: It is important to still call fu_device_close() even if this function
 * fails as the device may still be partially initialized.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_open (FuDevice *self, GError **error)
{
	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_RETRY_OPEN)) {
		FuDevice *parent = fu_device_get_parent (self);
		if (parent == NULL) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NOT_SUPPORTED,
					     "no parent device");
			return FALSE;
		}
		return fu_device_open_internal (parent, error);
	}
	return fu_device_open_internal (self, error);
}

/**
 * fu_device_close:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Closes a device, optionally running a object-specific vfunc.
 *
 * Plugins can call fu_device_close() multiple times without calling
 * fu_device_open(), but only the last call will actually invoke the vfunc.
 *
 * It is expected that plugins issue the same number of fu_device_open() and
 * fu_device_close() methods when using a specific @self.
 *
 * An error is returned if this method is called without having used the
 * fu_device_open() method beforehand.
 *
 * If the close action completed successfully the internal device flag
 * %FU_DEVICE_INTERNAL_FLAG_IS_OPEN will be cleared.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_close (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* not yet open */
	if (priv->open_refcount == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "cannot close device, refcount already zero");
		return FALSE;
	}
	if (!g_atomic_int_dec_and_test (&priv->open_refcount))
		return TRUE;

	/* subclassed */
	if (klass->close != NULL) {
		if (!klass->close (self, error))
			return FALSE;
	}

	/* success */
	fu_device_remove_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_IS_OPEN);
	return TRUE;
}

/**
 * fu_device_probe:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Probes a device, setting parameters on the object that does not need
 * the device open or the interface claimed.
 * If the device is not compatible then an error should be returned.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_probe (FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* already done */
	if (priv->done_probe)
		return TRUE;

	/* subclassed */
	if (klass->probe != NULL) {
		if (!klass->probe (self, error))
			return FALSE;
	}
	priv->done_probe = TRUE;
	return TRUE;
}

/**
 * fu_device_rescan:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Rescans a device, re-adding GUIDs or flags based on some hardware change.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.3.1
 **/
gboolean
fu_device_rescan (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* remove all GUIDs */
	g_ptr_array_set_size (fu_device_get_instance_ids (self), 0);
	g_ptr_array_set_size (fu_device_get_guids (self), 0);

	/* subclassed */
	if (klass->rescan != NULL) {
		if (!klass->rescan (self, error)) {
			fu_device_convert_instance_ids (self);
			return FALSE;
		}
	}

	fu_device_convert_instance_ids (self);
	return TRUE;
}

/**
 * fu_device_convert_instance_ids:
 * @self: a #FuDevice
 *
 * Converts all the Device Instance IDs added using fu_device_add_instance_id()
 * into actual GUIDs, **unless** %FU_DEVICE_INTERNAL_FLAG_NO_AUTO_INSTANCE_IDS has
 * been set.
 *
 * Plugins will only need to need to call this manually when adding child
 * devices, as fu_device_setup() automatically calls this after the
 * fu_device_probe() and fu_device_setup() virtual functions have been run.
 *
 * Since: 1.2.5
 **/
void
fu_device_convert_instance_ids (FuDevice *self)
{
	GPtrArray *instance_ids;

	/* OEM specific hardware */
	if (fu_device_has_internal_flag (self, FU_DEVICE_INTERNAL_FLAG_NO_AUTO_INSTANCE_IDS))
		return;
	instance_ids = fwupd_device_get_instance_ids (FWUPD_DEVICE (self));
	for (guint i = 0; i < instance_ids->len; i++) {
		const gchar *instance_id = g_ptr_array_index (instance_ids, i);
		g_autofree gchar *guid = fwupd_guid_hash_string (instance_id);
		fwupd_device_add_guid (FWUPD_DEVICE (self), guid);
	}
}

/**
 * fu_device_setup:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Sets up a device, setting parameters on the object that requires
 * the device to be open and have the interface claimed.
 * If the device is not compatible then an error should be returned.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.1.2
 **/
gboolean
fu_device_setup (FuDevice *self, GError **error)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	GPtrArray *children;

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* should have already been called */
	if (!fu_device_probe (self, error))
		return FALSE;

	/* already done */
	if (priv->done_setup)
		return TRUE;

	/* subclassed */
	if (klass->setup != NULL) {
		if (!klass->setup (self, error))
			return FALSE;
	}

	/* run setup on the children too (unless done already) */
	children = fu_device_get_children (self);
	for (guint i = 0; i < children->len; i++) {
		FuDevice *child_tmp = g_ptr_array_index (children, i);
		if (!fu_device_setup (child_tmp, error))
			return FALSE;
	}

	/* convert the instance IDs to GUIDs */
	fu_device_convert_instance_ids (self);

	/* subclassed */
	if (klass->ready != NULL) {
		if (!klass->ready (self, error))
			return FALSE;
	}

	priv->done_setup = TRUE;
	return TRUE;
}

/**
 * fu_device_activate:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Activates up a device, which normally means the device switches to a new
 * firmware version. This should only be called when data loss cannot occur.
 *
 * Returns: %TRUE for success
 *
 * Since: 1.2.6
 **/
gboolean
fu_device_activate (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* subclassed */
	if (klass->activate != NULL) {
		if (!klass->activate (self, error))
			return FALSE;
	}

	return TRUE;
}

/**
 * fu_device_probe_invalidate:
 * @self: a #FuDevice
 *
 * Normally when calling fu_device_probe() multiple times it is only done once.
 * Calling this method causes the next requests to fu_device_probe() and
 * fu_device_setup() actually probe the hardware.
 *
 * This should be done in case the backing device has changed, for instance if
 * a USB device has been replugged.
 *
 * Since: 1.1.2
 **/
void
fu_device_probe_invalidate (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	g_return_if_fail (FU_IS_DEVICE (self));
	priv->done_probe = FALSE;
	priv->done_setup = FALSE;
}

/**
 * fu_device_report_metadata_pre:
 * @self: a #FuDevice
 *
 * Collects metadata that would be useful for debugging a failed update report.
 *
 * Returns: (transfer full) (nullable): a #GHashTable, or %NULL if there is no data
 *
 * Since: 1.5.0
 **/
GHashTable *
fu_device_report_metadata_pre (FuDevice *self)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	g_autoptr(GHashTable) metadata = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);

	/* not implemented */
	if (klass->report_metadata_pre == NULL)
		return NULL;

	/* metadata for all devices */
	metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	klass->report_metadata_pre (self, metadata);
	return g_steal_pointer (&metadata);
}

/**
 * fu_device_report_metadata_post:
 * @self: a #FuDevice
 *
 * Collects metadata that would be useful for debugging a failed update report.
 *
 * Returns: (transfer full) (nullable): a #GHashTable, or %NULL if there is no data
 *
 * Since: 1.5.0
 **/
GHashTable *
fu_device_report_metadata_post (FuDevice *self)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	g_autoptr(GHashTable) metadata = NULL;

	g_return_val_if_fail (FU_IS_DEVICE (self), NULL);

	/* not implemented */
	if (klass->report_metadata_post == NULL)
		return NULL;

	/* metadata for all devices */
	metadata = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	klass->report_metadata_post (self, metadata);
	return g_steal_pointer (&metadata);
}

/**
 * fu_device_add_security_attrs:
 * @self: a #FuDevice
 * @attrs: a security attribute
 *
 * Adds HSI security attributes.
 *
 * Since: 1.6.0
 **/
void
fu_device_add_security_attrs (FuDevice *self, FuSecurityAttrs *attrs)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_if_fail (FU_IS_DEVICE (self));

	/* optional */
	if (klass->add_security_attrs != NULL)
		return klass->add_security_attrs (self, attrs);
}

/**
 * fu_device_bind_driver:
 * @self: a #FuDevice
 * @subsystem: a subsystem string, e.g. `pci`
 * @driver: a kernel module name, e.g. `tg3`
 * @error: (nullable): optional return location for an error
 *
 * Binds a driver to the device, which normally means the kernel driver takes
 * control of the hardware.
 *
 * Returns: %TRUE if driver was bound.
 *
 * Since: 1.5.0
 **/
gboolean
fu_device_bind_driver (FuDevice *self,
		       const gchar *subsystem,
		       const gchar *driver,
		       GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (subsystem != NULL, FALSE);
	g_return_val_if_fail (driver != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* not implemented */
	if (klass->bind_driver == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "not supported");
		return FALSE;
	}

	/* subclass */
	return klass->bind_driver (self, subsystem, driver, error);
}

/**
 * fu_device_unbind_driver:
 * @self: a #FuDevice
 * @error: (nullable): optional return location for an error
 *
 * Unbinds the driver from the device, which normally means the kernel releases
 * the hardware so it can be used from userspace.
 *
 * If there is no driver bound then this function will return with success
 * without actually doing anything.
 *
 * Returns: %TRUE if driver was unbound.
 *
 * Since: 1.5.0
 **/
gboolean
fu_device_unbind_driver (FuDevice *self, GError **error)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);

	g_return_val_if_fail (FU_IS_DEVICE (self), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* not implemented */
	if (klass->unbind_driver == NULL) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "not supported");
		return FALSE;
	}

	/* subclass */
	return klass->unbind_driver (self, error);
}

/**
 * fu_device_incorporate:
 * @self: a #FuDevice
 * @donor: Another #FuDevice
 *
 * Copy all properties from the donor object if they have not already been set.
 *
 * Since: 1.1.0
 **/
void
fu_device_incorporate (FuDevice *self, FuDevice *donor)
{
	FuDeviceClass *klass = FU_DEVICE_GET_CLASS (self);
	FuDevicePrivate *priv = GET_PRIVATE (self);
	FuDevicePrivate *priv_donor = GET_PRIVATE (donor);
	GPtrArray *instance_ids = fu_device_get_instance_ids (donor);
	GPtrArray *parent_guids = fu_device_get_parent_guids (donor);
	GPtrArray *parent_physical_ids = fu_device_get_parent_physical_ids (donor);

	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (FU_IS_DEVICE (donor));

	/* copy from donor FuDevice if has not already been set */
	if (priv->alternate_id == NULL)
		fu_device_set_alternate_id (self, fu_device_get_alternate_id (donor));
	if (priv->equivalent_id == NULL)
		fu_device_set_equivalent_id (self, fu_device_get_equivalent_id (donor));
	if (priv->physical_id == NULL && priv_donor->physical_id != NULL)
		fu_device_set_physical_id (self, priv_donor->physical_id);
	if (priv->logical_id == NULL && priv_donor->logical_id != NULL)
		fu_device_set_logical_id (self, priv_donor->logical_id);
	if (priv->backend_id == NULL && priv_donor->backend_id != NULL)
		fu_device_set_backend_id (self, priv_donor->backend_id);
	if (priv->proxy == NULL && priv_donor->proxy != NULL)
		fu_device_set_proxy (self, priv_donor->proxy);
	if (priv->proxy_guid == NULL && priv_donor->proxy_guid != NULL)
		fu_device_set_proxy_guid (self, priv_donor->proxy_guid);
	if (priv->ctx == NULL)
		fu_device_set_context (self, fu_device_get_context (donor));
	g_rw_lock_reader_lock (&priv_donor->parent_guids_mutex);
	for (guint i = 0; i < parent_guids->len; i++)
		fu_device_add_parent_guid (self, g_ptr_array_index (parent_guids, i));
	g_rw_lock_reader_unlock (&priv_donor->parent_guids_mutex);
	if (parent_physical_ids != NULL) {
		for (guint i = 0; i < parent_physical_ids->len; i++) {
			const gchar *tmp = g_ptr_array_index (parent_physical_ids, i);
			fu_device_add_parent_physical_id (self, tmp);
		}
	}
	g_rw_lock_reader_lock (&priv_donor->metadata_mutex);
	if (priv->metadata != NULL) {
		g_autoptr(GList) keys = g_hash_table_get_keys (priv_donor->metadata);
		for (GList *l = keys; l != NULL; l = l->next) {
			const gchar *key = l->data;
			if (g_hash_table_lookup (priv->metadata, key) == NULL) {
				const gchar *value = g_hash_table_lookup (priv_donor->metadata, key);
				fu_device_set_metadata (self, key, value);
			}
		}
	}
	g_rw_lock_reader_unlock (&priv_donor->metadata_mutex);

	/* now the base class, where all the interesting bits are */
	fwupd_device_incorporate (FWUPD_DEVICE (self), FWUPD_DEVICE (donor));

	/* set by the superclass */
	if (fu_device_get_id (self) != NULL)
		priv->device_id_valid = TRUE;

	/* optional subclass */
	if (klass->incorporate != NULL)
		klass->incorporate (self, donor);

	/* call the set_quirk_kv() vfunc for the superclassed object */
	for (guint i = 0; i < instance_ids->len; i++) {
		const gchar *instance_id = g_ptr_array_index (instance_ids, i);
		g_autofree gchar *guid = fwupd_guid_hash_string (instance_id);
		fu_device_add_guid_quirks (self, guid);
	}
}

/**
 * fu_device_incorporate_flag:
 * @self: a #FuDevice
 * @donor: another device
 * @flag: device flags
 *
 * Copy the value of a specific flag from the donor object.
 *
 * Since: 1.3.5
 **/
void
fu_device_incorporate_flag (FuDevice *self, FuDevice *donor, FwupdDeviceFlags flag)
{
	if (fu_device_has_flag (donor, flag) && !fu_device_has_flag (self, flag)) {
		g_debug ("donor set %s", fwupd_device_flag_to_string (flag));
		fu_device_add_flag (self, flag);
	} else if (!fu_device_has_flag (donor, flag) && fu_device_has_flag (self, flag)) {
		g_debug ("donor unset %s", fwupd_device_flag_to_string (flag));
		fu_device_remove_flag (self, flag);
	}
}

/**
 * fu_device_incorporate_from_component: (skip):
 * @device: a device
 * @component: a Xmlb node
 *
 * Copy all properties from the donor AppStream component.
 *
 * Since: 1.2.4
 **/
void
fu_device_incorporate_from_component (FuDevice *self, XbNode *component)
{
	const gchar *tmp;
	g_return_if_fail (FU_IS_DEVICE (self));
	g_return_if_fail (XB_IS_NODE (component));
	tmp = xb_node_query_text (component, "custom/value[@key='LVFS::UpdateMessage']", NULL);
	if (tmp != NULL) {
		fu_device_set_update_message_kind (self, FWUPD_DEVICE_MESSAGE_KIND_POST);
		fu_device_set_update_message (self, tmp);
	}
	tmp = xb_node_query_text (component, "custom/value[@key='LVFS::UpdateImage']", NULL);
	if (tmp != NULL)
		fu_device_set_update_image (self, tmp);
}

static void
fu_device_class_init (FuDeviceClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	GParamSpec *pspec;
	object_class->finalize = fu_device_finalize;
	object_class->get_property = fu_device_get_property;
	object_class->set_property = fu_device_set_property;

	signals[SIGNAL_CHILD_ADDED] =
		g_signal_new ("child-added",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (FuDeviceClass, child_added),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, FU_TYPE_DEVICE);
	signals[SIGNAL_CHILD_REMOVED] =
		g_signal_new ("child-removed",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (FuDeviceClass, child_removed),
			      NULL, NULL, g_cclosure_marshal_VOID__OBJECT,
			      G_TYPE_NONE, 1, FU_TYPE_DEVICE);

	pspec = g_param_spec_string ("physical-id", NULL, NULL, NULL,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_PHYSICAL_ID, pspec);

	pspec = g_param_spec_string ("logical-id", NULL, NULL, NULL,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_LOGICAL_ID, pspec);

	pspec = g_param_spec_string ("backend-id", NULL, NULL, NULL,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_BACKEND_ID, pspec);

	pspec = g_param_spec_uint ("progress", NULL, NULL,
				   0, 100, 0,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_PROGRESS, pspec);

	pspec = g_param_spec_uint ("battery-level", NULL, NULL,
				   0,
				   FU_BATTERY_VALUE_INVALID,
				   FU_BATTERY_VALUE_INVALID,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_BATTERY_LEVEL, pspec);

	pspec = g_param_spec_uint ("battery-threshold", NULL, NULL,
				   0,
				   FU_BATTERY_VALUE_INVALID,
				   FU_BATTERY_VALUE_INVALID,
				   G_PARAM_READWRITE |
				   G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_BATTERY_THRESHOLD, pspec);

	pspec = g_param_spec_object ("context", NULL, NULL,
				     FU_TYPE_CONTEXT,
				     G_PARAM_READWRITE |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_CONTEXT, pspec);

	pspec = g_param_spec_object ("proxy", NULL, NULL,
				     FU_TYPE_DEVICE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_PROXY, pspec);

	pspec = g_param_spec_object ("parent", NULL, NULL,
				     FU_TYPE_DEVICE,
				     G_PARAM_READWRITE |
				     G_PARAM_CONSTRUCT |
				     G_PARAM_STATIC_NAME);
	g_object_class_install_property (object_class, PROP_PARENT, pspec);
}

static void
fu_device_init (FuDevice *self)
{
	FuDevicePrivate *priv = GET_PRIVATE (self);
	priv->order = G_MAXINT;
	priv->battery_level = FU_BATTERY_VALUE_INVALID;
	priv->battery_threshold = FU_BATTERY_VALUE_INVALID;
	priv->parent_guids = g_ptr_array_new_with_free_func (g_free);
	priv->possible_plugins = g_ptr_array_new_with_free_func (g_free);
	priv->retry_recs = g_ptr_array_new_with_free_func (g_free);
	g_rw_lock_init (&priv->parent_guids_mutex);
	g_rw_lock_init (&priv->metadata_mutex);
}

static void
fu_device_finalize (GObject *object)
{
	FuDevice *self = FU_DEVICE (object);
	FuDevicePrivate *priv = GET_PRIVATE (self);

	g_rw_lock_clear (&priv->metadata_mutex);
	g_rw_lock_clear (&priv->parent_guids_mutex);

	if (priv->alternate != NULL)
		g_object_unref (priv->alternate);
	if (priv->proxy != NULL)
		g_object_remove_weak_pointer (G_OBJECT (priv->proxy), (gpointer *) &priv->proxy);
	if (priv->ctx != NULL)
		g_object_unref (priv->ctx);
	if (priv->poll_id != 0)
		g_source_remove (priv->poll_id);
	if (priv->metadata != NULL)
		g_hash_table_unref (priv->metadata);
	if (priv->inhibits != NULL)
		g_hash_table_unref (priv->inhibits);
	if (priv->parent_physical_ids != NULL)
		g_ptr_array_unref (priv->parent_physical_ids);
	if (priv->private_flag_items != NULL)
		g_ptr_array_unref (priv->private_flag_items);
	g_ptr_array_unref (priv->parent_guids);
	g_ptr_array_unref (priv->possible_plugins);
	g_ptr_array_unref (priv->retry_recs);
	g_free (priv->alternate_id);
	g_free (priv->equivalent_id);
	g_free (priv->physical_id);
	g_free (priv->logical_id);
	g_free (priv->backend_id);
	g_free (priv->proxy_guid);

	G_OBJECT_CLASS (fu_device_parent_class)->finalize (object);
}

/**
 * fu_device_new:
 *
 * Creates a new #Fudevice
 *
 * Since: 0.1.0
 **/
FuDevice *
fu_device_new (void)
{
	return fu_device_new_with_context (NULL);
}

/**
 * fu_device_new_with_context:
 *
 * Creates a new #Fudevice
 *
 * Since: 1.6.2
 **/
FuDevice *
fu_device_new_with_context (FuContext *ctx)
{
	FuDevice *self = g_object_new (FU_TYPE_DEVICE,
				       "context", ctx,
				       NULL);
	return FU_DEVICE (self);
}
