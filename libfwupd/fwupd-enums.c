/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fwupd-enums.h"

/**
 * fwupd_status_to_string:
 * @status: a status, e.g. %FWUPD_STATUS_DECOMPRESSING
 *
 * Converts a enumerated status to a string.
 *
 * Returns: identifier string
 *
 * Since: 0.1.1
 **/
const gchar *
fwupd_status_to_string (FwupdStatus status)
{
	if (status == FWUPD_STATUS_UNKNOWN)
		return "unknown";
	if (status == FWUPD_STATUS_IDLE)
		return "idle";
	if (status == FWUPD_STATUS_DECOMPRESSING)
		return "decompressing";
	if (status == FWUPD_STATUS_LOADING)
		return "loading";
	if (status == FWUPD_STATUS_DEVICE_RESTART)
		return "device-restart";
	if (status == FWUPD_STATUS_DEVICE_WRITE)
		return "device-write";
	if (status == FWUPD_STATUS_DEVICE_READ)
		return "device-read";
	if (status == FWUPD_STATUS_DEVICE_ERASE)
		return "device-erase";
	if (status == FWUPD_STATUS_DEVICE_VERIFY)
		return "device-verify";
	if (status == FWUPD_STATUS_DEVICE_BUSY)
		return "device-busy";
	if (status == FWUPD_STATUS_SCHEDULING)
		return "scheduling";
	if (status == FWUPD_STATUS_DOWNLOADING)
		return "downloading";
	if (status == FWUPD_STATUS_WAITING_FOR_AUTH)
		return "waiting-for-auth";
	if (status == FWUPD_STATUS_SHUTDOWN)
		return "shutdown";
	return NULL;
}

/**
 * fwupd_status_from_string:
 * @status: a string, e.g. `decompressing`
 *
 * Converts a string to an enumerated status.
 *
 * Returns: enumerated value
 *
 * Since: 0.1.1
 **/
FwupdStatus
fwupd_status_from_string (const gchar *status)
{
	if (g_strcmp0 (status, "unknown") == 0)
		return FWUPD_STATUS_UNKNOWN;
	if (g_strcmp0 (status, "idle") == 0)
		return FWUPD_STATUS_IDLE;
	if (g_strcmp0 (status, "decompressing") == 0)
		return FWUPD_STATUS_DECOMPRESSING;
	if (g_strcmp0 (status, "loading") == 0)
		return FWUPD_STATUS_LOADING;
	if (g_strcmp0 (status, "device-restart") == 0)
		return FWUPD_STATUS_DEVICE_RESTART;
	if (g_strcmp0 (status, "device-write") == 0)
		return FWUPD_STATUS_DEVICE_WRITE;
	if (g_strcmp0 (status, "device-verify") == 0)
		return FWUPD_STATUS_DEVICE_VERIFY;
	if (g_strcmp0 (status, "scheduling") == 0)
		return FWUPD_STATUS_SCHEDULING;
	if (g_strcmp0 (status, "downloading") == 0)
		return FWUPD_STATUS_DOWNLOADING;
	if (g_strcmp0 (status, "device-read") == 0)
		return FWUPD_STATUS_DEVICE_READ;
	if (g_strcmp0 (status, "device-erase") == 0)
		return FWUPD_STATUS_DEVICE_ERASE;
	if (g_strcmp0 (status, "device-busy") == 0)
		return FWUPD_STATUS_DEVICE_BUSY;
	if (g_strcmp0 (status, "waiting-for-auth") == 0)
		return FWUPD_STATUS_WAITING_FOR_AUTH;
	if (g_strcmp0 (status, "shutdown") == 0)
		return FWUPD_STATUS_SHUTDOWN;
	return FWUPD_STATUS_LAST;
}

/**
 * fwupd_device_flag_to_string:
 * @device_flag: a device flag, e.g. %FWUPD_DEVICE_FLAG_REQUIRE_AC
 *
 * Converts a device flag to a string.
 *
 * Returns: identifier string
 *
 * Since: 0.7.0
 **/
const gchar *
fwupd_device_flag_to_string (FwupdDeviceFlags device_flag)
{
	if (device_flag == FWUPD_DEVICE_FLAG_NONE)
		return "none";
	if (device_flag == FWUPD_DEVICE_FLAG_INTERNAL)
		return "internal";
	if (device_flag == FWUPD_DEVICE_FLAG_UPDATABLE)
		return "updatable";
	if (device_flag == FWUPD_DEVICE_FLAG_ONLY_OFFLINE)
		return "only-offline";
	if (device_flag == FWUPD_DEVICE_FLAG_REQUIRE_AC)
		return "require-ac";
	if (device_flag == FWUPD_DEVICE_FLAG_LOCKED)
		return "locked";
	if (device_flag == FWUPD_DEVICE_FLAG_SUPPORTED)
		return "supported";
	if (device_flag == FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER)
		return "needs-bootloader";
	if (device_flag == FWUPD_DEVICE_FLAG_REGISTERED)
		return "registered";
	if (device_flag == FWUPD_DEVICE_FLAG_NEEDS_REBOOT)
		return "needs-reboot";
	if (device_flag == FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN)
		return "needs-shutdown";
	if (device_flag == FWUPD_DEVICE_FLAG_REPORTED)
		return "reported";
	if (device_flag == FWUPD_DEVICE_FLAG_NOTIFIED)
		return "notified";
	if (device_flag == FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION)
		return "use-runtime-version";
	if (device_flag == FWUPD_DEVICE_FLAG_INSTALL_PARENT_FIRST)
		return "install-parent-first";
	if (device_flag == FWUPD_DEVICE_FLAG_IS_BOOTLOADER)
		return "is-bootloader";
	if (device_flag == FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG)
		return "wait-for-replug";
	if (device_flag == FWUPD_DEVICE_FLAG_IGNORE_VALIDATION)
		return "ignore-validation";
	if (device_flag == FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED)
		return "another-write-required";
	if (device_flag == FWUPD_DEVICE_FLAG_NO_AUTO_INSTANCE_IDS)
		return "no-auto-instance-ids";
	if (device_flag == FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION)
		return "needs-activation";
	if (device_flag == FWUPD_DEVICE_FLAG_ENSURE_SEMVER)
		return "ensure-semver";
	if (device_flag == FWUPD_DEVICE_FLAG_HISTORICAL)
		return "historical";
	if (device_flag == FWUPD_DEVICE_FLAG_ONLY_SUPPORTED)
		return "only-supported";
	if (device_flag == FWUPD_DEVICE_FLAG_WILL_DISAPPEAR)
		return "will-disappear";
	if (device_flag == FWUPD_DEVICE_FLAG_CAN_VERIFY)
		return "can-verify";
	if (device_flag == FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE)
		return "can-verify-image";
	if (device_flag == FWUPD_DEVICE_FLAG_DUAL_IMAGE)
		return "dual-image";
	if (device_flag == FWUPD_DEVICE_FLAG_SELF_RECOVERY)
		return "self-recovery";
	if (device_flag == FWUPD_DEVICE_FLAG_USABLE_DURING_UPDATE)
		return "usable-during-update";
	if (device_flag == FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED)
		return "version-check-required";
	if (device_flag == FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES)
		return "install-all-releases";
	if (device_flag == FWUPD_DEVICE_FLAG_MD_SET_NAME)
		return "md-set-name";
	if (device_flag == FWUPD_DEVICE_FLAG_MD_SET_NAME_CATEGORY)
		return "md-set-name-category";
	if (device_flag == FWUPD_DEVICE_FLAG_MD_SET_VERFMT)
		return "md-set-verfmt";
	if (device_flag == FWUPD_DEVICE_FLAG_MD_SET_ICON)
		return "md-set-icon";
	if (device_flag == FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS)
		return "add-counterpart-guids";
	if (device_flag == FWUPD_DEVICE_FLAG_NO_GUID_MATCHING)
		return "no-guid-matching";
	if (device_flag == FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN)
		return "updatable-hidden";
	if (device_flag == FWUPD_DEVICE_FLAG_SKIPS_RESTART)
		return "skips-restart";
	if (device_flag == FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES)
		return "has-multiple-branches";
	if (device_flag == FWUPD_DEVICE_FLAG_BACKUP_BEFORE_INSTALL)
		return "backup-before-install";
	if (device_flag == FWUPD_DEVICE_FLAG_UNKNOWN)
		return "unknown";
	return NULL;
}

/**
 * fwupd_device_flag_from_string:
 * @device_flag: a string, e.g. `require-ac`
 *
 * Converts a string to a enumerated device flag.
 *
 * Returns: enumerated value
 *
 * Since: 0.7.0
 **/
FwupdDeviceFlags
fwupd_device_flag_from_string (const gchar *device_flag)
{
	if (g_strcmp0 (device_flag, "none") == 0)
		return FWUPD_DEVICE_FLAG_NONE;
	if (g_strcmp0 (device_flag, "internal") == 0)
		return FWUPD_DEVICE_FLAG_INTERNAL;
	if (g_strcmp0 (device_flag, "updatable") == 0 ||
	    g_strcmp0 (device_flag, "allow-online") == 0)
		return FWUPD_DEVICE_FLAG_UPDATABLE;
	if (g_strcmp0 (device_flag, "only-offline") == 0 ||
	    g_strcmp0 (device_flag, "allow-offline") == 0)
		return FWUPD_DEVICE_FLAG_ONLY_OFFLINE;
	if (g_strcmp0 (device_flag, "require-ac") == 0)
		return FWUPD_DEVICE_FLAG_REQUIRE_AC;
	if (g_strcmp0 (device_flag, "locked") == 0)
		return FWUPD_DEVICE_FLAG_LOCKED;
	if (g_strcmp0 (device_flag, "supported") == 0)
		return FWUPD_DEVICE_FLAG_SUPPORTED;
	if (g_strcmp0 (device_flag, "needs-bootloader") == 0)
		return FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER;
	if (g_strcmp0 (device_flag, "registered") == 0)
		return FWUPD_DEVICE_FLAG_REGISTERED;
	if (g_strcmp0 (device_flag, "needs-reboot") == 0)
		return FWUPD_DEVICE_FLAG_NEEDS_REBOOT;
	if (g_strcmp0 (device_flag, "needs-shutdown") == 0)
		return FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN;
	if (g_strcmp0 (device_flag, "reported") == 0)
		return FWUPD_DEVICE_FLAG_REPORTED;
	if (g_strcmp0 (device_flag, "notified") == 0)
		return FWUPD_DEVICE_FLAG_NOTIFIED;
	if (g_strcmp0 (device_flag, "use-runtime-version") == 0)
		return FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION;
	if (g_strcmp0 (device_flag, "install-parent-first") == 0)
		return FWUPD_DEVICE_FLAG_INSTALL_PARENT_FIRST;
	if (g_strcmp0 (device_flag, "is-bootloader") == 0)
		return FWUPD_DEVICE_FLAG_IS_BOOTLOADER;
	if (g_strcmp0 (device_flag, "wait-for-replug") == 0)
		return FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG;
	if (g_strcmp0 (device_flag, "ignore-validation") == 0)
		return FWUPD_DEVICE_FLAG_IGNORE_VALIDATION;
	if (g_strcmp0 (device_flag, "another-write-required") == 0)
		return FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED;
	if (g_strcmp0 (device_flag, "no-auto-instance-ids") == 0)
		return FWUPD_DEVICE_FLAG_NO_AUTO_INSTANCE_IDS;
	if (g_strcmp0 (device_flag, "needs-activation") == 0)
		return FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION;
	if (g_strcmp0 (device_flag, "ensure-semver") == 0)
		return FWUPD_DEVICE_FLAG_ENSURE_SEMVER;
	if (g_strcmp0 (device_flag, "historical") == 0)
		return FWUPD_DEVICE_FLAG_HISTORICAL;
	if (g_strcmp0 (device_flag, "only-supported") == 0)
		return FWUPD_DEVICE_FLAG_ONLY_SUPPORTED;
	if (g_strcmp0 (device_flag, "will-disappear") == 0)
		return FWUPD_DEVICE_FLAG_WILL_DISAPPEAR;
	if (g_strcmp0 (device_flag, "can-verify") == 0)
		return FWUPD_DEVICE_FLAG_CAN_VERIFY;
	if (g_strcmp0 (device_flag, "can-verify-image") == 0)
		return FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE;
	if (g_strcmp0 (device_flag, "dual-image") == 0)
		return FWUPD_DEVICE_FLAG_DUAL_IMAGE;
	if (g_strcmp0 (device_flag, "self-recovery") == 0)
		return FWUPD_DEVICE_FLAG_SELF_RECOVERY;
	if (g_strcmp0 (device_flag, "usable-during-update") == 0)
		return FWUPD_DEVICE_FLAG_USABLE_DURING_UPDATE;
	if (g_strcmp0 (device_flag, "version-check-required") == 0)
		return FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED;
	if (g_strcmp0 (device_flag, "install-all-releases") == 0)
		return FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES;
	if (g_strcmp0 (device_flag, "md-set-name") == 0)
		return FWUPD_DEVICE_FLAG_MD_SET_NAME;
	if (g_strcmp0 (device_flag, "md-set-name-category") == 0)
		return FWUPD_DEVICE_FLAG_MD_SET_NAME_CATEGORY;
	if (g_strcmp0 (device_flag, "md-set-verfmt") == 0)
		return FWUPD_DEVICE_FLAG_MD_SET_VERFMT;
	if (g_strcmp0 (device_flag, "md-set-icon") == 0)
		return FWUPD_DEVICE_FLAG_MD_SET_ICON;
	if (g_strcmp0 (device_flag, "add-counterpart-guids") == 0)
		return FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS;
	if (g_strcmp0 (device_flag, "no-guid-matching") == 0)
		return FWUPD_DEVICE_FLAG_NO_GUID_MATCHING;
	if (g_strcmp0 (device_flag, "updatable-hidden") == 0)
		return FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN;
	if (g_strcmp0 (device_flag, "skips-restart") == 0)
		return FWUPD_DEVICE_FLAG_SKIPS_RESTART;
	if (g_strcmp0 (device_flag, "has-multiple-branches") == 0)
		return FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES;
	if (g_strcmp0 (device_flag, "backup-before-install") == 0)
		return FWUPD_DEVICE_FLAG_BACKUP_BEFORE_INSTALL;
	return FWUPD_DEVICE_FLAG_UNKNOWN;
}

/**
 * fwupd_plugin_flag_to_string:
 * @plugin_flag: plugin flags, e.g. %FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE
 *
 * Converts an enumerated plugin flag to a string.
 *
 * Returns: identifier string
 *
 * Since: 1.5.0
 **/
const gchar *
fwupd_plugin_flag_to_string (FwupdPluginFlags plugin_flag)
{
	if (plugin_flag == FWUPD_DEVICE_FLAG_NONE)
		return "none";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_DISABLED)
		return "disabled";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_USER_WARNING)
		return "user-warning";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE)
		return "clear-updatable";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_NO_HARDWARE)
		return "no-hardware";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_CAPSULES_UNSUPPORTED)
		return "capsules-unsupported";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_UNLOCK_REQUIRED)
		return "unlock-required";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_EFIVAR_NOT_MOUNTED)
		return "efivar-not-mounted";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_ESP_NOT_FOUND)
		return "esp-not-found";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_LEGACY_BIOS)
		return "legacy-bios";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_FAILED_OPEN)
		return "failed-open";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_REQUIRE_HWID)
		return "require-hwid";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_KERNEL_TOO_OLD)
		return "kernel-too-old";
	if (plugin_flag == FWUPD_DEVICE_FLAG_UNKNOWN)
		return "unknown";
	if (plugin_flag == FWUPD_PLUGIN_FLAG_AUTH_REQUIRED)
		return "auth-required";
	return NULL;
}

/**
 * fwupd_plugin_flag_from_string:
 * @plugin_flag: a string, e.g. `require-ac`
 *
 * Converts a string to an enumerated plugin flag.
 *
 * Returns: enumerated value
 *
 * Since: 1.5.0
 **/
FwupdPluginFlags
fwupd_plugin_flag_from_string (const gchar *plugin_flag)
{
	if (g_strcmp0 (plugin_flag, "none") == 0)
		return FWUPD_DEVICE_FLAG_NONE;
	if (g_strcmp0 (plugin_flag, "disabled") == 0)
		return FWUPD_PLUGIN_FLAG_DISABLED;
	if (g_strcmp0 (plugin_flag, "user-warning") == 0)
		return FWUPD_PLUGIN_FLAG_USER_WARNING;
	if (g_strcmp0 (plugin_flag, "clear-updatable") == 0)
		return FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE;
	if (g_strcmp0 (plugin_flag, "no-hardware") == 0)
		return FWUPD_PLUGIN_FLAG_NO_HARDWARE;
	if (g_strcmp0 (plugin_flag, "capsules-unsupported") == 0)
		return FWUPD_PLUGIN_FLAG_CAPSULES_UNSUPPORTED;
	if (g_strcmp0 (plugin_flag, "unlock-required") == 0)
		return FWUPD_PLUGIN_FLAG_UNLOCK_REQUIRED;
	if (g_strcmp0 (plugin_flag, "efivar-not-mounted") == 0)
		return FWUPD_PLUGIN_FLAG_EFIVAR_NOT_MOUNTED;
	if (g_strcmp0 (plugin_flag, "esp-not-found") == 0)
		return FWUPD_PLUGIN_FLAG_ESP_NOT_FOUND;
	if (g_strcmp0 (plugin_flag, "legacy-bios") == 0)
		return FWUPD_PLUGIN_FLAG_LEGACY_BIOS;
	if (g_strcmp0 (plugin_flag, "failed-open") == 0)
		return FWUPD_PLUGIN_FLAG_FAILED_OPEN;
	if (g_strcmp0 (plugin_flag, "require-hwid") == 0)
		return FWUPD_PLUGIN_FLAG_REQUIRE_HWID;
	if (g_strcmp0 (plugin_flag, "kernel-too-old") == 0)
		return FWUPD_PLUGIN_FLAG_KERNEL_TOO_OLD;
	if (g_strcmp0 (plugin_flag, "auth-required") == 0)
		return FWUPD_PLUGIN_FLAG_AUTH_REQUIRED;
	return FWUPD_DEVICE_FLAG_UNKNOWN;
}

/**
 * fwupd_update_state_to_string:
 * @update_state: the update state, e.g. %FWUPD_UPDATE_STATE_PENDING
 *
 * Converts a enumerated update state to a string.
 *
 * Returns: identifier string
 *
 * Since: 0.7.0
 **/
const gchar *
fwupd_update_state_to_string (FwupdUpdateState update_state)
{
	if (update_state == FWUPD_UPDATE_STATE_UNKNOWN)
		return "unknown";
	if (update_state == FWUPD_UPDATE_STATE_PENDING)
		return "pending";
	if (update_state == FWUPD_UPDATE_STATE_SUCCESS)
		return "success";
	if (update_state == FWUPD_UPDATE_STATE_FAILED)
		return "failed";
	if (update_state == FWUPD_UPDATE_STATE_FAILED_TRANSIENT)
		return "failed-transient";
	if (update_state == FWUPD_UPDATE_STATE_NEEDS_REBOOT)
		return "needs-reboot";
	return NULL;
}

/**
 * fwupd_update_state_from_string:
 * @update_state: a string, e.g. `pending`
 *
 * Converts a string to a enumerated update state.
 *
 * Returns: enumerated value
 *
 * Since: 0.7.0
 **/
FwupdUpdateState
fwupd_update_state_from_string (const gchar *update_state)
{
	if (g_strcmp0 (update_state, "unknown") == 0)
		return FWUPD_UPDATE_STATE_UNKNOWN;
	if (g_strcmp0 (update_state, "pending") == 0)
		return FWUPD_UPDATE_STATE_PENDING;
	if (g_strcmp0 (update_state, "success") == 0)
		return FWUPD_UPDATE_STATE_SUCCESS;
	if (g_strcmp0 (update_state, "failed") == 0)
		return FWUPD_UPDATE_STATE_FAILED;
	if (g_strcmp0 (update_state, "failed-transient") == 0)
		return FWUPD_UPDATE_STATE_FAILED_TRANSIENT;
	if (g_strcmp0 (update_state, "needs-reboot") == 0)
		return FWUPD_UPDATE_STATE_NEEDS_REBOOT;
	return FWUPD_UPDATE_STATE_UNKNOWN;
}

/**
 * fwupd_trust_flag_to_string:
 * @trust_flag: the trust flags, e.g. %FWUPD_TRUST_FLAG_PAYLOAD
 *
 * Converts a enumerated trust flag to a string.
 *
 * Returns: identifier string
 *
 * Since: 0.7.0
 **/
const gchar *
fwupd_trust_flag_to_string (FwupdTrustFlags trust_flag)
{
	if (trust_flag == FWUPD_TRUST_FLAG_NONE)
		return "none";
	if (trust_flag == FWUPD_TRUST_FLAG_PAYLOAD)
		return "payload";
	if (trust_flag == FWUPD_TRUST_FLAG_METADATA)
		return "metadata";
	return NULL;
}

/**
 * fwupd_trust_flag_from_string:
 * @trust_flag: a string, e.g. `payload`
 *
 * Converts a string to a enumerated trust flag.
 *
 * Returns: enumerated value
 *
 * Since: 0.7.0
 **/
FwupdTrustFlags
fwupd_trust_flag_from_string (const gchar *trust_flag)
{
	if (g_strcmp0 (trust_flag, "none") == 0)
		return FWUPD_TRUST_FLAG_NONE;
	if (g_strcmp0 (trust_flag, "payload") == 0)
		return FWUPD_TRUST_FLAG_PAYLOAD;
	if (g_strcmp0 (trust_flag, "metadata") == 0)
		return FWUPD_TRUST_FLAG_METADATA;
	return FWUPD_TRUST_FLAG_LAST;
}

/**
 * fwupd_feature_flag_to_string:
 * @feature_flag: a single feature flag, e.g. %FWUPD_FEATURE_FLAG_DETACH_ACTION
 *
 * Converts a feature flag to a string.
 *
 * Returns: identifier string
 *
 * Since: 1.4.5
 **/
const gchar *
fwupd_feature_flag_to_string (FwupdFeatureFlags feature_flag)
{
	if (feature_flag == FWUPD_FEATURE_FLAG_NONE)
		return "none";
	if (feature_flag == FWUPD_FEATURE_FLAG_CAN_REPORT)
		return "can-report";
	if (feature_flag == FWUPD_FEATURE_FLAG_DETACH_ACTION)
		return "detach-action";
	if (feature_flag == FWUPD_FEATURE_FLAG_UPDATE_ACTION)
		return "update-action";
	if (feature_flag == FWUPD_FEATURE_FLAG_SWITCH_BRANCH)
		return "switch-branch";
	if (feature_flag == FWUPD_FEATURE_FLAG_IMMEDIATE_MESSAGE)
		return "immediate-message";
	return NULL;
}

/**
 * fwupd_feature_flag_from_string:
 * @feature_flag: a string, e.g. `detach-action`
 *
 * Converts a string to a enumerated feature flag.
 *
 * Returns: enumerated value
 *
 * Since: 1.4.5
 **/
FwupdFeatureFlags
fwupd_feature_flag_from_string (const gchar *feature_flag)
{
	if (g_strcmp0 (feature_flag, "none") == 0)
		return FWUPD_FEATURE_FLAG_NONE;
	if (g_strcmp0 (feature_flag, "can-report") == 0)
		return FWUPD_FEATURE_FLAG_CAN_REPORT;
	if (g_strcmp0 (feature_flag, "detach-action") == 0)
		return FWUPD_FEATURE_FLAG_DETACH_ACTION;
	if (g_strcmp0 (feature_flag, "update-action") == 0)
		return FWUPD_FEATURE_FLAG_UPDATE_ACTION;
	if (g_strcmp0 (feature_flag, "switch-branch") == 0)
		return FWUPD_FEATURE_FLAG_SWITCH_BRANCH;
	if (g_strcmp0 (feature_flag, "immediate-message") == 0)
		return FWUPD_FEATURE_FLAG_IMMEDIATE_MESSAGE;
	return FWUPD_FEATURE_FLAG_LAST;
}

/**
 * fwupd_keyring_kind_from_string:
 * @keyring_kind: a string, e.g. `gpg`
 *
 * Converts an printable string to an enumerated keyring kind.
 *
 * Returns: keyring kind, e.g. %FWUPD_KEYRING_KIND_GPG
 *
 * Since: 0.9.7
 **/
FwupdKeyringKind
fwupd_keyring_kind_from_string (const gchar *keyring_kind)
{
	if (g_strcmp0 (keyring_kind, "none") == 0)
		return FWUPD_KEYRING_KIND_NONE;
	if (g_strcmp0 (keyring_kind, "gpg") == 0)
		return FWUPD_KEYRING_KIND_GPG;
	if (g_strcmp0 (keyring_kind, "pkcs7") == 0)
		return FWUPD_KEYRING_KIND_PKCS7;
	if (g_strcmp0 (keyring_kind, "jcat") == 0)
		return FWUPD_KEYRING_KIND_JCAT;
	return FWUPD_KEYRING_KIND_UNKNOWN;
}

/**
 * fwupd_keyring_kind_to_string:
 * @keyring_kind: a #FwupdKeyringKind, e.g. %FWUPD_KEYRING_KIND_GPG
 *
 * Converts an enumerated keyring kind to a printable string.
 *
 * Returns: a string, e.g. `gpg`
 *
 * Since: 0.9.7
 **/
const gchar *
fwupd_keyring_kind_to_string (FwupdKeyringKind keyring_kind)
{
	if (keyring_kind == FWUPD_KEYRING_KIND_NONE)
		return "none";
	if (keyring_kind == FWUPD_KEYRING_KIND_GPG)
		return "gpg";
	if (keyring_kind == FWUPD_KEYRING_KIND_PKCS7)
		return "pkcs7";
	if (keyring_kind == FWUPD_KEYRING_KIND_JCAT)
		return "jcat";
	return NULL;
}

/**
 * fwupd_release_flag_to_string:
 * @release_flag: a release flag, e.g. %FWUPD_RELEASE_FLAG_TRUSTED_PAYLOAD
 *
 * Converts a enumerated release flag to a string.
 *
 * Returns: identifier string
 *
 * Since: 1.2.6
 **/
const gchar *
fwupd_release_flag_to_string (FwupdReleaseFlags release_flag)
{
	if (release_flag == FWUPD_RELEASE_FLAG_NONE)
		return "none";
	if (release_flag == FWUPD_RELEASE_FLAG_TRUSTED_PAYLOAD)
		return "trusted-payload";
	if (release_flag == FWUPD_RELEASE_FLAG_TRUSTED_METADATA)
		return "trusted-metadata";
	if (release_flag == FWUPD_RELEASE_FLAG_IS_UPGRADE)
		return "is-upgrade";
	if (release_flag == FWUPD_RELEASE_FLAG_IS_DOWNGRADE)
		return "is-downgrade";
	if (release_flag == FWUPD_RELEASE_FLAG_BLOCKED_VERSION)
		return "blocked-version";
	if (release_flag == FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL)
		return "blocked-approval";
	if (release_flag == FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH)
		return "is-alternate-branch";
	return NULL;
}

/**
 * fwupd_release_flag_from_string:
 * @release_flag: a string, e.g. `trusted-payload`
 *
 * Converts a string to an enumerated release flag.
 *
 * Returns: enumerated value
 *
 * Since: 1.2.6
 **/
FwupdReleaseFlags
fwupd_release_flag_from_string (const gchar *release_flag)
{
	if (g_strcmp0 (release_flag, "trusted-payload") == 0)
		return FWUPD_RELEASE_FLAG_TRUSTED_PAYLOAD;
	if (g_strcmp0 (release_flag, "trusted-metadata") == 0)
		return FWUPD_RELEASE_FLAG_TRUSTED_METADATA;
	if (g_strcmp0 (release_flag, "is-upgrade") == 0)
		return FWUPD_RELEASE_FLAG_IS_UPGRADE;
	if (g_strcmp0 (release_flag, "is-downgrade") == 0)
		return FWUPD_RELEASE_FLAG_IS_DOWNGRADE;
	if (g_strcmp0 (release_flag, "blocked-version") == 0)
		return FWUPD_RELEASE_FLAG_BLOCKED_VERSION;
	if (g_strcmp0 (release_flag, "blocked-approval") == 0)
		return FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL;
	if (g_strcmp0 (release_flag, "is-alternate-branch") == 0)
		return FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH;
	return FWUPD_RELEASE_FLAG_NONE;
}

/**
 * fwupd_release_urgency_to_string:
 * @release_urgency: a release urgency, e.g. %FWUPD_RELEASE_URGENCY_HIGH
 *
 * Converts an enumerated release urgency to a string.
 *
 * Returns: identifier string
 *
 * Since: 1.4.0
 **/
const gchar *
fwupd_release_urgency_to_string (FwupdReleaseUrgency release_urgency)
{
	if (release_urgency == FWUPD_RELEASE_URGENCY_LOW)
		return "low";
	if (release_urgency == FWUPD_RELEASE_URGENCY_MEDIUM)
		return "medium";
	if (release_urgency == FWUPD_RELEASE_URGENCY_HIGH)
		return "high";
	if (release_urgency == FWUPD_RELEASE_URGENCY_CRITICAL)
		return "critical";
	return NULL;
}

/**
 * fwupd_release_urgency_from_string:
 * @release_urgency: a string, e.g. `low`
 *
 * Converts a string to an enumerated release urgency value.
 *
 * Returns: enumerated value
 *
 * Since: 1.4.0
 **/
FwupdReleaseUrgency
fwupd_release_urgency_from_string (const gchar *release_urgency)
{
	if (g_strcmp0 (release_urgency, "low") == 0)
		return FWUPD_RELEASE_URGENCY_LOW;
	if (g_strcmp0 (release_urgency, "medium") == 0)
		return FWUPD_RELEASE_URGENCY_MEDIUM;
	if (g_strcmp0 (release_urgency, "high") == 0)
		return FWUPD_RELEASE_URGENCY_HIGH;
	if (g_strcmp0 (release_urgency, "critical") == 0)
		return FWUPD_RELEASE_URGENCY_CRITICAL;
	return FWUPD_RELEASE_URGENCY_UNKNOWN;
}

/**
 * fwupd_version_format_from_string:
 * @str: a string, e.g. `quad`
 *
 * Converts text to a display version type.
 *
 * Returns: an enumerated version format, e.g. %FWUPD_VERSION_FORMAT_TRIPLET
 *
 * Since: 1.2.9
 **/
FwupdVersionFormat
fwupd_version_format_from_string (const gchar *str)
{
	if (g_strcmp0 (str, "plain") == 0)
		return FWUPD_VERSION_FORMAT_PLAIN;
	if (g_strcmp0 (str, "pair") == 0)
		return FWUPD_VERSION_FORMAT_PAIR;
	if (g_strcmp0 (str, "number") == 0)
		return FWUPD_VERSION_FORMAT_NUMBER;
	if (g_strcmp0 (str, "triplet") == 0)
		return FWUPD_VERSION_FORMAT_TRIPLET;
	if (g_strcmp0 (str, "quad") == 0)
		return FWUPD_VERSION_FORMAT_QUAD;
	if (g_strcmp0 (str, "bcd") == 0)
		return FWUPD_VERSION_FORMAT_BCD;
	if (g_strcmp0 (str, "intel-me") == 0)
		return FWUPD_VERSION_FORMAT_INTEL_ME;
	if (g_strcmp0 (str, "intel-me2") == 0)
		return FWUPD_VERSION_FORMAT_INTEL_ME2;
	if (g_strcmp0 (str, "surface-legacy") == 0)
		return FWUPD_VERSION_FORMAT_SURFACE_LEGACY;
	if (g_strcmp0 (str, "surface") == 0)
		return FWUPD_VERSION_FORMAT_SURFACE;
	if (g_strcmp0 (str, "dell-bios") == 0)
		return FWUPD_VERSION_FORMAT_DELL_BIOS;
	if (g_strcmp0 (str, "hex") == 0)
		return FWUPD_VERSION_FORMAT_HEX;
	return FWUPD_VERSION_FORMAT_UNKNOWN;
}

/**
 * fwupd_version_format_to_string:
 * @kind: a version format, e.g. %FWUPD_VERSION_FORMAT_TRIPLET
 *
 * Converts an enumerated version format to text.
 *
 * Returns: a string, e.g. `quad`, or %NULL if not known
 *
 * Since: 1.2.9
 **/
const gchar *
fwupd_version_format_to_string (FwupdVersionFormat kind)
{
	if (kind == FWUPD_VERSION_FORMAT_PLAIN)
		return "plain";
	if (kind == FWUPD_VERSION_FORMAT_NUMBER)
		return "number";
	if (kind == FWUPD_VERSION_FORMAT_PAIR)
		return "pair";
	if (kind == FWUPD_VERSION_FORMAT_TRIPLET)
		return "triplet";
	if (kind == FWUPD_VERSION_FORMAT_QUAD)
		return "quad";
	if (kind == FWUPD_VERSION_FORMAT_BCD)
		return "bcd";
	if (kind == FWUPD_VERSION_FORMAT_INTEL_ME)
		return "intel-me";
	if (kind == FWUPD_VERSION_FORMAT_INTEL_ME2)
		return "intel-me2";
	if (kind == FWUPD_VERSION_FORMAT_SURFACE_LEGACY)
		return "surface-legacy";
	if (kind == FWUPD_VERSION_FORMAT_SURFACE)
		return "surface";
	if (kind == FWUPD_VERSION_FORMAT_DELL_BIOS)
		return "dell-bios";
	if (kind == FWUPD_VERSION_FORMAT_HEX)
		return "hex";
	return NULL;
}
