/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib.h>

G_BEGIN_DECLS

/**
 * FwupdStatus:
 * @FWUPD_STATUS_UNKNOWN:			Unknown state
 * @FWUPD_STATUS_IDLE:				Idle
 * @FWUPD_STATUS_LOADING:			Loading a resource
 * @FWUPD_STATUS_DECOMPRESSING:			Decompressing firmware
 * @FWUPD_STATUS_DEVICE_RESTART:		Restarting the device
 * @FWUPD_STATUS_DEVICE_WRITE:			Writing to a device
 * @FWUPD_STATUS_DEVICE_VERIFY:			Verifying (reading) a device
 * @FWUPD_STATUS_SCHEDULING:			Scheduling an offline update
 * @FWUPD_STATUS_DOWNLOADING:			A file is downloading
 * @FWUPD_STATUS_DEVICE_READ:			Reading from a device
 * @FWUPD_STATUS_DEVICE_ERASE:			Erasing a device
 * @FWUPD_STATUS_WAITING_FOR_AUTH:		Waiting for authentication
 * @FWUPD_STATUS_DEVICE_BUSY:			The device is busy
 * @FWUPD_STATUS_SHUTDOWN:			The daemon is shutting down
 *
 * The flags to show daemon status.
 **/
typedef enum {
	FWUPD_STATUS_UNKNOWN,				/* Since: 0.1.1 */
	FWUPD_STATUS_IDLE,				/* Since: 0.1.1 */
	FWUPD_STATUS_LOADING,				/* Since: 0.1.1 */
	FWUPD_STATUS_DECOMPRESSING,			/* Since: 0.1.1 */
	FWUPD_STATUS_DEVICE_RESTART,			/* Since: 0.1.1 */
	FWUPD_STATUS_DEVICE_WRITE,			/* Since: 0.1.1 */
	FWUPD_STATUS_DEVICE_VERIFY,			/* Since: 0.1.1 */
	FWUPD_STATUS_SCHEDULING,			/* Since: 0.1.1 */
	FWUPD_STATUS_DOWNLOADING,			/* Since: 0.9.4 */
	FWUPD_STATUS_DEVICE_READ,			/* Since: 1.0.0 */
	FWUPD_STATUS_DEVICE_ERASE,			/* Since: 1.0.0 */
	FWUPD_STATUS_WAITING_FOR_AUTH,			/* Since: 1.0.0 */
	FWUPD_STATUS_DEVICE_BUSY,			/* Since: 1.0.1 */
	FWUPD_STATUS_SHUTDOWN,				/* Since: 1.2.1 */
	/*< private >*/
	FWUPD_STATUS_LAST
} FwupdStatus;

/**
 * FwupdTrustFlags:
 * @FWUPD_TRUST_FLAG_NONE:			No trust
 * @FWUPD_TRUST_FLAG_PAYLOAD:			The firmware is trusted
 * @FWUPD_TRUST_FLAG_METADATA:			The metadata is trusted
 *
 * The flags to show the level of trust.
 **/
typedef enum {
	FWUPD_TRUST_FLAG_NONE		= 0,		/* Since: 0.1.2 */
	FWUPD_TRUST_FLAG_PAYLOAD	= 1 << 0,	/* Since: 0.1.2 */
	FWUPD_TRUST_FLAG_METADATA	= 1 << 1,	/* Since: 0.1.2 */
	/*< private >*/
	FWUPD_TRUST_FLAG_LAST
} FwupdTrustFlags;

/**
 * FwupdFeatureFlags:
 * @FWUPD_FEATURE_FLAG_NONE:			No trust
 * @FWUPD_FEATURE_FLAG_CAN_REPORT:		Can upload a report of the update back to the server
 * @FWUPD_FEATURE_FLAG_DETACH_ACTION:		Can perform detach action, typically showing text
 * @FWUPD_FEATURE_FLAG_UPDATE_ACTION:		Can perform update action, typically showing text
 * @FWUPD_FEATURE_FLAG_SWITCH_BRANCH:		Can switch the firmware branch
 * @FWUPD_FEATURE_FLAG_IMMEDIATE_MESSAGE:	Can show immediate messages
 *
 * The flags to the feature capabilities of the front-end client.
 **/
typedef enum {
	FWUPD_FEATURE_FLAG_NONE			= 0,		/* Since: 1.4.5 */
	FWUPD_FEATURE_FLAG_CAN_REPORT		= 1 << 0,	/* Since: 1.4.5 */
	FWUPD_FEATURE_FLAG_DETACH_ACTION	= 1 << 1,	/* Since: 1.4.5 */
	FWUPD_FEATURE_FLAG_UPDATE_ACTION	= 1 << 2,	/* Since: 1.4.5 */
	FWUPD_FEATURE_FLAG_SWITCH_BRANCH	= 1 << 3,	/* Since: 1.5.0 */
	FWUPD_FEATURE_FLAG_IMMEDIATE_MESSAGE	= 1 << 4,	/* Since: 1.6.2 */
	/*< private >*/
	FWUPD_FEATURE_FLAG_LAST
} FwupdFeatureFlags;

/**
 * FWUPD_DEVICE_FLAG_NONE:
 *
 * No flags set
 *
 * Since 0.1.3
 */
#define FWUPD_DEVICE_FLAG_NONE			(0u)
/**
 * FWUPD_DEVICE_FLAG_INTERNAL:
 *
 * Device is internal to the platform and cannot be removed easily.
 *
 * Since 0.1.3
 */
#define FWUPD_DEVICE_FLAG_INTERNAL		(1u << 0)
/**
 * FWUPD_DEVICE_FLAG_UPDATABLE:
 *
 * Device has the ability to be updated in this or any other mode.
 *
 * Since 0.9.7
 */
#define FWUPD_DEVICE_FLAG_UPDATABLE		(1u << 1)
/**
 * FWUPD_DEVICE_FLAG_ONLY_OFFLINE:
 *
 * Update can only be done from a limited functionality OS (offline mode).
 *
 * Since 0.9.7
 */
#define FWUPD_DEVICE_FLAG_ONLY_OFFLINE		(1u << 2)
/**
 * FWUPD_DEVICE_FLAG_REQUIRE_AC:
 *
 * Device requires an external power source to be connected or the battery
 * level at a minimum threshold to update.
 *
 * Since 0.6.3
 */
#define FWUPD_DEVICE_FLAG_REQUIRE_AC		(1u << 3)
/**
 * FWUPD_DEVICE_FLAG_LOCKED:
 *
 * The device can not be updated without manual user interaction.
 *
 * Since 0.6.3
 */
#define FWUPD_DEVICE_FLAG_LOCKED		(1u << 4)
/**
 * FWUPD_DEVICE_FLAG_SUPPORTED:
 *
 * The device is found in metadata loaded into the daemon.
 *
 * Since 0.7.1
 */
#define FWUPD_DEVICE_FLAG_SUPPORTED		(1u << 5)
/**
 * FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER:
 *
 * The device requires entering a bootloader mode to be manually.
 *
 * Since 0.7.3
 */
#define FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER	(1u << 6)
/**
 * FWUPD_DEVICE_FLAG_REGISTERED:
 *
 * The device has been registered with other plugins.
 *
 * Since 0.9.7
 */
#define FWUPD_DEVICE_FLAG_REGISTERED		(1u << 7)
/**
 * FWUPD_DEVICE_FLAG_NEEDS_REBOOT:
 *
 * The device requires a system reboot to apply firmware or to reload hardware.
 *
 * Since 0.9.7
 */
#define FWUPD_DEVICE_FLAG_NEEDS_REBOOT		(1u << 8)
/**
 * FWUPD_DEVICE_FLAG_REPORTED:
 *
 * The success or failure of a previous update has been reported to a metadata server.
 *
 * Since: 1.0.4
 */
#define FWUPD_DEVICE_FLAG_REPORTED		(1u << 9)
/**
 * FWUPD_DEVICE_FLAG_NOTIFIED:
 *
 * The user has been notified about a change in the device state.
 *
 * Since: 1.0.5
 */
#define FWUPD_DEVICE_FLAG_NOTIFIED		(1u << 10)
/**
 * FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION:
 *
 * The device will always display use the runtime version rather than the bootloader version.
 *
 * Since: 1.0.6
 */
#define FWUPD_DEVICE_FLAG_USE_RUNTIME_VERSION	(1u << 11)
/**
 * FWUPD_DEVICE_FLAG_INSTALL_PARENT_FIRST:
 *
 * The composite device requires installation of composite firmware on the parent before the child.
 * Normally the child is installed before the parent.
 *
 * Since: 1.0.8
 */
#define FWUPD_DEVICE_FLAG_INSTALL_PARENT_FIRST	(1u << 12)
/**
 * FWUPD_DEVICE_FLAG_IS_BOOTLOADER:
 *
 * The device is currently in a read-only bootloader mode and not running application code.
 *
 * Since: 1.0.8
 */
#define FWUPD_DEVICE_FLAG_IS_BOOTLOADER		(1u << 13)
/**
 * FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG:
 *
 * The device is in the middle of and update and the hardware is waiting to be probed/replugged.
 *
 * Since: 1.1.2
 */
#define FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG	(1u << 14)
/**
 * FWUPD_DEVICE_FLAG_IGNORE_VALIDATION:
 *
 * When processing an update for the device, plugins should ignore all validation safety checks.
 *
 * Since: 1.1.2
 */
#define FWUPD_DEVICE_FLAG_IGNORE_VALIDATION	(1u << 15)
/**
 * FWUPD_DEVICE_FLAG_TRUSTED:
 *
 * A trusted client is reading information about the device.
 * Extra metadata such as serial number can be exposed about this device.
 *
 * Since: 1.1.2
 */
#define FWUPD_DEVICE_FLAG_TRUSTED		(1u << 16)
/**
 * FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN:
 *
 * The device requires the system to be shutdown to finish application of new firmware.
 *
 * Since: 1.2.4
 */
#define FWUPD_DEVICE_FLAG_NEEDS_SHUTDOWN	(1u << 17)
/**
 * FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED:
 *
 * The device requires the update to be retried, possibly with a different plugin.
 *
 * Since: 1.2.5
 */
#define FWUPD_DEVICE_FLAG_ANOTHER_WRITE_REQUIRED (1u << 18)
/**
 * FWUPD_DEVICE_FLAG_NO_AUTO_INSTANCE_IDS:
 *
 * Deprecated, no not use
 *
 * Since: 1.2.5
 * Deprecated 1.5.5
 */
#define FWUPD_DEVICE_FLAG_NO_AUTO_INSTANCE_IDS	(1u << 19)
/**
 * FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION:
 *
 * The device update needs to be separately activated.
 * This process may occur automatically on shutdown in some operating systems
 * or when the device is unplugged with some devices.
 *
 * Since: 1.2.6
 */
#define FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION	(1u << 20)
/**
 * FWUPD_DEVICE_FLAG_ENSURE_SEMVER:
 *
 * Deprecated, no not use
 *
 * Since: 1.2.9
 * Deprecate: 1.5.5
 */
#define FWUPD_DEVICE_FLAG_ENSURE_SEMVER		(1u << 21)
/**
 * FWUPD_DEVICE_FLAG_HISTORICAL:
 *
 * The device is used for historical data only.
 *
 * Since: 1.3.2
 */
#define FWUPD_DEVICE_FLAG_HISTORICAL		(1u << 22)
/**
 * FWUPD_DEVICE_FLAG_ONLY_SUPPORTED:
 *
 * Deprecated, no not use
 *
 * Since: 1.3.3
 * Deprecated 1.5.5
 */
#define FWUPD_DEVICE_FLAG_ONLY_SUPPORTED	(1u << 23)
/**
 * FWUPD_DEVICE_FLAG_WILL_DISAPPEAR:
 *
 * The device will disappear after the update is complete and success
 * or failure can't be verified.
 *
 * Since: 1.3.3
 */
#define FWUPD_DEVICE_FLAG_WILL_DISAPPEAR	(1u << 24)
/**
 * FWUPD_DEVICE_FLAG_CAN_VERIFY:
 *
 * THe device checksums can be compared against metadata.
 *
 * Since: 1.3.3
 */
#define FWUPD_DEVICE_FLAG_CAN_VERIFY		(1u << 25)
/**
 * FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE:
 *
 * The device application firmware image can be dumped from device for verification.
 *
 * Since: 1.3.3
 */
#define FWUPD_DEVICE_FLAG_CAN_VERIFY_IMAGE	(1u << 26)
/**
 * FWUPD_DEVICE_FLAG_DUAL_IMAGE:
 *
 * The device firmware update architecture uses a redundancy mechanism such
 * as A/B partitions for updates.
 *
 * Since: 1.3.3
 */
#define FWUPD_DEVICE_FLAG_DUAL_IMAGE		(1u << 27)
/**
 * FWUPD_DEVICE_FLAG_SELF_RECOVERY:
 *
 * In flashing mode, the device will only accept intended payloads and will
 * revert back to a valid firmware image if an invalid or incomplete payload was sent.
 *
 * Since: 1.3.3
 */
#define FWUPD_DEVICE_FLAG_SELF_RECOVERY		(1u << 28)
/**
 * FWUPD_DEVICE_FLAG_USABLE_DURING_UPDATE:
 *
 * The device remains usable while the update flashes or schedules the update.
 * The update will implicitly be applied next time the device is power cycled or possibly activated.
 *
 * Since: 1.3.3
 */
#define FWUPD_DEVICE_FLAG_USABLE_DURING_UPDATE	(1u << 29)
/**
 * FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED:
 *
 * All firmware updates for this device require a firmware version check.
 *
 * Since: 1.3.7
 */
#define FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED (1u << 30)
/**
 * FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES:
 *
 * Install each intermediate releases for the device rather than jumping directly to the newest.
 *
 * Since: 1.3.7
 */
#define FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES	(1u << 31)
/**
 * FWUPD_DEVICE_FLAG_MD_SET_NAME:
 *
 * Deprecated, no not use
 *
 * Since: 1.4.0
 * Deprecated 1.5.5
 */
#define FWUPD_DEVICE_FLAG_MD_SET_NAME		(1llu << 32)
/**
 * FWUPD_DEVICE_FLAG_MD_SET_NAME_CATEGORY:
 *
 * Deprecated, no not use
 *
 * Since: 1.4.0
 * Deprecated 1.5.5
 */
#define FWUPD_DEVICE_FLAG_MD_SET_NAME_CATEGORY	(1llu << 33)
/**
 * FWUPD_DEVICE_FLAG_MD_SET_VERFMT:
 *
 * Deprecated, no not use
 *
 * Since: 1.4.0
 * Deprecated 1.5.5
 */
#define FWUPD_DEVICE_FLAG_MD_SET_VERFMT		(1llu << 34)
/**
 * FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS:
 *
 * The device will add counterpart GUIDs from an alternate mode like bootloader.
 * This flag is typically specified in a quirk.
 *
 * Since: 1.4.0
 */
#define FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS	(1llu << 35)
/**
 * FWUPD_DEVICE_FLAG_NO_GUID_MATCHING:
 *
 * Deprecated, no not use
 *
 * Since: 1.4.1
 * Deprecated 1.5.8
 */
#define FWUPD_DEVICE_FLAG_NO_GUID_MATCHING	(1llu << 36)
/**
 * FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN:
 *
 * The device is updatable but is currently inhbitied from updates in the client.
 * Reasons include but are not limited to low power or requiring reboot from a previous update.
 *
 * Since: 1.4.1
 */
#define FWUPD_DEVICE_FLAG_UPDATABLE_HIDDEN	(1llu << 37)
/**
 * FWUPD_DEVICE_FLAG_SKIPS_RESTART:
 *
 * The device relies upon activation or power cycle to load firmware.
 *
 * Since: 1.5.0
 */
#define FWUPD_DEVICE_FLAG_SKIPS_RESTART		(1llu << 38)
/**
 * FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES:
 *
 * The device supports switching to a different stream of firmware.
 *
 * Since: 1.5.0
 */
#define FWUPD_DEVICE_FLAG_HAS_MULTIPLE_BRANCHES	(1llu << 39)
/**
 * FWUPD_DEVICE_FLAG_BACKUP_BEFORE_INSTALL:
 *
 * The device firmware should be saved before installing firmware.
 *
 * Since: 1.5.0
 */
#define FWUPD_DEVICE_FLAG_BACKUP_BEFORE_INSTALL	(1llu << 40)
/**
 * FWUPD_DEVICE_FLAG_MD_SET_ICON:
 *
 *Deprecated, no not use
 *
 * Since: 1.5.2
 * Deprecated 1.5.5
 */
#define FWUPD_DEVICE_FLAG_MD_SET_ICON		(1llu << 41)
/**
 * FWUPD_DEVICE_FLAG_UNKNOWN:
 *
 * This flag is not defined, this typically will happen from mismatched
 * fwupd library and clients.
 *
 * Since 0.7.3
 */
#define FWUPD_DEVICE_FLAG_UNKNOWN		G_MAXUINT64
/**
 * FwupdDeviceFlags:
 *
 * Flags used to represent device attributes
 */
typedef guint64 FwupdDeviceFlags;

/**
 * FWUPD_RELEASE_FLAG_NONE:
 *
 * No flags are set.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_NONE			(0u)
/**
 * FWUPD_RELEASE_FLAG_TRUSTED_PAYLOAD:
 *
 * The payload binary is trusted.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_TRUSTED_PAYLOAD	(1u << 0)
/**
 * FWUPD_RELEASE_FLAG_TRUSTED_METADATA:
 *
 * The payload metadata is trusted.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_TRUSTED_METADATA	(1u << 1)
/**
 * FWUPD_RELEASE_FLAG_IS_UPGRADE:
 *
 * The release is newer than the device version.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_IS_UPGRADE		(1u << 2)
/**
 * FWUPD_RELEASE_FLAG_IS_DOWNGRADE:
 *
 * The release is older than the device version.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_IS_DOWNGRADE		(1u << 3)
/**
 * FWUPD_RELEASE_FLAG_BLOCKED_VERSION:
 *
 * The installation of the release is blocked as below device version-lowest.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_BLOCKED_VERSION	(1u << 4)
/**
 * FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL:
 *
 * The installation of the release is blocked as release not approved by an administrator.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_BLOCKED_APPROVAL	(1u << 5)
/**
 * FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH:
 *
 * The release is an alternate branch of firmware.
 *
 * Since: 1.5.0
 */
#define FWUPD_RELEASE_FLAG_IS_ALTERNATE_BRANCH	(1u << 6)
/**
 * FWUPD_RELEASE_FLAG_UNKNOWN:
 *
 * The release flag is unknown, typically caused by using mismatched client and daemon.
 *
 * Since: 1.2.6
 */
#define FWUPD_RELEASE_FLAG_UNKNOWN		G_MAXUINT64
/**
 * FwupdReleaseFlags:
 *
 * Flags used to represent release attributes
 */
typedef guint64 FwupdReleaseFlags;

/**
 * FwupdReleaseUrgency:
 * @FWUPD_RELEASE_URGENCY_UNKNOWN:		Unknown
 * @FWUPD_RELEASE_URGENCY_LOW:			Low
 * @FWUPD_RELEASE_URGENCY_MEDIUM:		Medium
 * @FWUPD_RELEASE_URGENCY_HIGH:			High
 * @FWUPD_RELEASE_URGENCY_CRITICAL:		Critical, e.g. a security fix
 *
 * The release urgency.
 **/
typedef enum {
	FWUPD_RELEASE_URGENCY_UNKNOWN,				/* Since: 1.4.0 */
	FWUPD_RELEASE_URGENCY_LOW,				/* Since: 1.4.0 */
	FWUPD_RELEASE_URGENCY_MEDIUM,				/* Since: 1.4.0 */
	FWUPD_RELEASE_URGENCY_HIGH,				/* Since: 1.4.0 */
	FWUPD_RELEASE_URGENCY_CRITICAL,				/* Since: 1.4.0 */
	/*< private >*/
	FWUPD_RELEASE_URGENCY_LAST
} FwupdReleaseUrgency;

/**
 * FWUPD_PLUGIN_FLAG_NONE:
 *
 * No plugin flags are set.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_NONE			(0u)
/**
 * FWUPD_PLUGIN_FLAG_DISABLED:
 *
 * The plugin has been disabled, either by daemon configuration or a problem.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_DISABLED		(1u << 0)
/**
 * FWUPD_PLUGIN_FLAG_USER_WARNING:
 *
 * The plugin has a problem and would like to show a user warning to a supported client.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_USER_WARNING		(1u << 1)
/**
 * FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE:
 *
 * When the plugin loads it should clear the UPDATABLE flag from any devices.
 * This typically happens when the device requires a system restart.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_CLEAR_UPDATABLE	(1u << 2)
/**
 * FWUPD_PLUGIN_FLAG_NO_HARDWARE:
 *
 * The plugin won't load because no supported hardware was found.
 * This typically happens with plugins designed for a specific platform design
 * (such as the dell plugin only works on Dell systems).
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_NO_HARDWARE		(1u << 3)
/**
 * FWUPD_PLUGIN_FLAG_CAPSULES_UNSUPPORTED:
 *
 * The plugin discovered that UEFI UpdateCapsule are unsupported.
 * Supported clients will display this information to a user.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_CAPSULES_UNSUPPORTED	(1u << 4)
/**
 * FWUPD_PLUGIN_FLAG_UNLOCK_REQUIRED:
 *
 * The plugin discovered that hardware unlock is required.
 * Supported clients will display this information to a user.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_UNLOCK_REQUIRED	(1u << 5)
/**
 * FWUPD_PLUGIN_FLAG_EFIVAR_NOT_MOUNTED:
 *
 * The plugin discovered the efivar filesystem is not found and is required for this plugin.
 * Supported clients will display this information to a user.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_EFIVAR_NOT_MOUNTED	(1u << 6)
/**
 * FWUPD_PLUGIN_FLAG_ESP_NOT_FOUND:
 *
 * The plugins discovered that the EFI system partition was not found.
 * Supported clients will display this information to a user.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_ESP_NOT_FOUND		(1u << 7)
/**
 * FWUPD_PLUGIN_FLAG_LEGACY_BIOS:
 *
 * The plugin discovered the system is running in legacy CSM mode.
 * Supported clients will display this information to a user.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_LEGACY_BIOS		(1u << 8)
/**
 * FWUPD_PLUGIN_FLAG_FAILED_OPEN:
 *
 * Failed to open plugin (missing dependency).
 * Supported clients will display this information to a user.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_FAILED_OPEN		(1u << 9)
/**
 * FWUPD_PLUGIN_FLAG_REQUIRE_HWID:
 *
 * A specific HWID is required to use this plugin.
 *
 * Since: 1.5.8
 */
#define FWUPD_PLUGIN_FLAG_REQUIRE_HWID		(1u << 10)
/**
 * FWUPD_PLUGIN_FLAG_KERNEL_TOO_OLD:
 *
 * The feature is not supported as the kernel is too old.
 *
 * Since: 1.6.2
 */
#define FWUPD_PLUGIN_FLAG_KERNEL_TOO_OLD	(1u << 11)
/**
 * FWUPD_PLUGIN_FLAG_AUTH_REQUIRED:
 *
 * The plugin requires the user to provide authentication details.
 * Supported clients will display this information to a user.
 *
 * Since: 1.6.2
 */
#define FWUPD_PLUGIN_FLAG_AUTH_REQUIRED		(1u << 12)
/**
 * FWUPD_PLUGIN_FLAG_UNKNOWN:
 *
 * The plugin flag is Unknown.
 * This is usually caused by a mismatched libfwupdplugin and daemon.
 *
 * Since: 1.5.0
 */
#define FWUPD_PLUGIN_FLAG_UNKNOWN		G_MAXUINT64
 /**
  * FwupdPluginFlags:
  *
  * Flags used to represent plugin attributes
  */
typedef guint64 FwupdPluginFlags;

/**
 * FwupdInstallFlags:
 * @FWUPD_INSTALL_FLAG_NONE:			No flags set
 * @FWUPD_INSTALL_FLAG_OFFLINE:			Schedule this for next boot
 * @FWUPD_INSTALL_FLAG_ALLOW_REINSTALL:		Allow reinstalling the same version
 * @FWUPD_INSTALL_FLAG_ALLOW_OLDER:		Allow downgrading firmware
 * @FWUPD_INSTALL_FLAG_FORCE:			Force the update even if not a good idea
 * @FWUPD_INSTALL_FLAG_NO_HISTORY:		Do not write to the history database
 * @FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH:	Allow firmware branch switching
 * @FWUPD_INSTALL_FLAG_IGNORE_CHECKSUM:		Ignore firmware CRCs and checksums
 * @FWUPD_INSTALL_FLAG_IGNORE_VID_PID:		Ignore firmware vendor and project checks
 * @FWUPD_INSTALL_FLAG_IGNORE_POWER:		Ignore requirement of external power source
 * @FWUPD_INSTALL_FLAG_NO_SEARCH:		Do not use heuristics when parsing the image
 *
 * Flags to set when performing the firmware update or install.
 **/
typedef enum {
	FWUPD_INSTALL_FLAG_NONE			= 0,		/* Since: 0.7.0 */
	FWUPD_INSTALL_FLAG_OFFLINE		= 1 << 0,	/* Since: 0.7.0 */
	FWUPD_INSTALL_FLAG_ALLOW_REINSTALL	= 1 << 1,	/* Since: 0.7.0 */
	FWUPD_INSTALL_FLAG_ALLOW_OLDER		= 1 << 2,	/* Since: 0.7.0 */
	FWUPD_INSTALL_FLAG_FORCE		= 1 << 3,	/* Since: 0.7.1 */
	FWUPD_INSTALL_FLAG_NO_HISTORY		= 1 << 4,	/* Since: 1.0.8 */
	FWUPD_INSTALL_FLAG_ALLOW_BRANCH_SWITCH	= 1 << 5,	/* Since: 1.5.0 */
	FWUPD_INSTALL_FLAG_IGNORE_CHECKSUM	= 1 << 6,	/* Since: 1.5.0 */
	FWUPD_INSTALL_FLAG_IGNORE_VID_PID	= 1 << 7,	/* Since: 1.5.0 */
	FWUPD_INSTALL_FLAG_IGNORE_POWER		= 1 << 8,	/* Since: 1.5.0 */
	FWUPD_INSTALL_FLAG_NO_SEARCH		= 1 << 9,	/* Since: 1.5.0 */
	/*< private >*/
	FWUPD_INSTALL_FLAG_LAST
} FwupdInstallFlags;

/**
 * FwupdSelfSignFlags:
 * @FWUPD_SELF_SIGN_FLAG_NONE:			No flags set
 * @FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP:		Add the timestamp to the detached signature
 * @FWUPD_SELF_SIGN_FLAG_ADD_CERT:		Add the certificate to the detached signature
 *
 * Flags to set when performing the firmware update or install.
 **/
typedef enum {
	FWUPD_SELF_SIGN_FLAG_NONE		= 0,		/* Since: 1.2.6 */
	FWUPD_SELF_SIGN_FLAG_ADD_TIMESTAMP	= 1 << 0,	/* Since: 1.2.6 */
	FWUPD_SELF_SIGN_FLAG_ADD_CERT		= 1 << 1,	/* Since: 1.2.6 */
	/*< private >*/
	FWUPD_SELF_SIGN_FLAG_LAST
} FwupdSelfSignFlags;

/**
 * FwupdUpdateState:
 * @FWUPD_UPDATE_STATE_UNKNOWN:			Unknown
 * @FWUPD_UPDATE_STATE_PENDING:			Update is pending
 * @FWUPD_UPDATE_STATE_SUCCESS:			Update was successful
 * @FWUPD_UPDATE_STATE_FAILED:			Update failed
 * @FWUPD_UPDATE_STATE_NEEDS_REBOOT:		Waiting for a reboot to apply
 * @FWUPD_UPDATE_STATE_FAILED_TRANSIENT:	Update failed due to transient issue, e.g. AC power required
 *
 * The update state.
 **/
typedef enum {
	FWUPD_UPDATE_STATE_UNKNOWN,			/* Since: 0.7.0 */
	FWUPD_UPDATE_STATE_PENDING,			/* Since: 0.7.0 */
	FWUPD_UPDATE_STATE_SUCCESS,			/* Since: 0.7.0 */
	FWUPD_UPDATE_STATE_FAILED,			/* Since: 0.7.0 */
	FWUPD_UPDATE_STATE_NEEDS_REBOOT,		/* Since: 1.0.4 */
	FWUPD_UPDATE_STATE_FAILED_TRANSIENT,		/* Since: 1.2.7 */
	/*< private >*/
	FWUPD_UPDATE_STATE_LAST
} FwupdUpdateState;

/**
 * FwupdKeyringKind:
 * @FWUPD_KEYRING_KIND_UNKNOWN:			Unknown
 * @FWUPD_KEYRING_KIND_NONE:			No verification
 * @FWUPD_KEYRING_KIND_GPG:			Verification using GPG
 * @FWUPD_KEYRING_KIND_PKCS7:			Verification using PKCS7
 * @FWUPD_KEYRING_KIND_JCAT:			Verification using Jcat
 *
 * The update state.
 **/
typedef enum {
	FWUPD_KEYRING_KIND_UNKNOWN,			/* Since: 0.9.7 */
	FWUPD_KEYRING_KIND_NONE,			/* Since: 0.9.7 */
	FWUPD_KEYRING_KIND_GPG,				/* Since: 0.9.7 */
	FWUPD_KEYRING_KIND_PKCS7,			/* Since: 0.9.7 */
	FWUPD_KEYRING_KIND_JCAT,			/* Since: 1.4.0 */
	/*< private >*/
	FWUPD_KEYRING_KIND_LAST
} FwupdKeyringKind;

/**
 * FwupdVersionFormat:
 * @FWUPD_VERSION_FORMAT_UNKNOWN:		Unknown version format
 * @FWUPD_VERSION_FORMAT_PLAIN:			An unidentified format text string
 * @FWUPD_VERSION_FORMAT_NUMBER:		A single integer version number
 * @FWUPD_VERSION_FORMAT_PAIR:			Two AABB.CCDD version numbers
 * @FWUPD_VERSION_FORMAT_TRIPLET:		Microsoft-style AA.BB.CCDD version numbers
 * @FWUPD_VERSION_FORMAT_QUAD:			UEFI-style AA.BB.CC.DD version numbers
 * @FWUPD_VERSION_FORMAT_BCD:			Binary coded decimal notation
 * @FWUPD_VERSION_FORMAT_INTEL_ME:		Intel ME-style bitshifted notation
 * @FWUPD_VERSION_FORMAT_INTEL_ME2:		Intel ME-style A.B.CC.DDDD notation notation
 * @FWUPD_VERSION_FORMAT_SURFACE_LEGACY:	Legacy Microsoft Surface 10b.12b.10b
 * @FWUPD_VERSION_FORMAT_SURFACE:		Microsoft Surface 8b.16b.8b
 * @FWUPD_VERSION_FORMAT_DELL_BIOS:		Dell BIOS BB.CC.DD style
 * @FWUPD_VERSION_FORMAT_HEX:			Hexadecimal 0xAABCCDD style
 *
 * The flags used when parsing version numbers.
 *
 * If no verification is required then %FWUPD_VERSION_FORMAT_PLAIN should
 * be used to signify an unparsable text string.
 **/
typedef enum {
	FWUPD_VERSION_FORMAT_UNKNOWN,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_PLAIN,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_NUMBER,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_PAIR,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_TRIPLET,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_QUAD,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_BCD,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_INTEL_ME,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_INTEL_ME2,			/* Since: 1.2.9 */
	FWUPD_VERSION_FORMAT_SURFACE_LEGACY,		/* Since: 1.3.4 */
	FWUPD_VERSION_FORMAT_SURFACE,			/* Since: 1.3.4 */
	FWUPD_VERSION_FORMAT_DELL_BIOS,			/* Since: 1.3.6 */
	FWUPD_VERSION_FORMAT_HEX,		/* Since: 1.4.0 */
	/*< private >*/
	FWUPD_VERSION_FORMAT_LAST
} FwupdVersionFormat;

const gchar	*fwupd_status_to_string			(FwupdStatus	 status);
FwupdStatus	 fwupd_status_from_string		(const gchar	*status);
const gchar	*fwupd_device_flag_to_string		(FwupdDeviceFlags device_flag);
FwupdDeviceFlags fwupd_device_flag_from_string		(const gchar	*device_flag);
const gchar	*fwupd_plugin_flag_to_string		(FwupdPluginFlags plugin_flag);
FwupdPluginFlags fwupd_plugin_flag_from_string		(const gchar	*plugin_flag);
const gchar	*fwupd_release_flag_to_string		(FwupdReleaseFlags release_flag);
FwupdReleaseFlags fwupd_release_flag_from_string	(const gchar	*release_flag);
const gchar	*fwupd_release_urgency_to_string	(FwupdReleaseUrgency release_urgency);
FwupdReleaseUrgency fwupd_release_urgency_from_string	(const gchar	*release_urgency);
const gchar	*fwupd_update_state_to_string		(FwupdUpdateState update_state);
FwupdUpdateState fwupd_update_state_from_string		(const gchar	*update_state);
const gchar	*fwupd_trust_flag_to_string		(FwupdTrustFlags trust_flag);
FwupdTrustFlags	 fwupd_trust_flag_from_string		(const gchar	*trust_flag);
const gchar	*fwupd_feature_flag_to_string		(FwupdFeatureFlags feature_flag);
FwupdFeatureFlags fwupd_feature_flag_from_string	(const gchar	*feature_flag);
FwupdKeyringKind fwupd_keyring_kind_from_string		(const gchar	*keyring_kind);
const gchar	*fwupd_keyring_kind_to_string		(FwupdKeyringKind keyring_kind);
FwupdVersionFormat fwupd_version_format_from_string	(const gchar	*str);
const gchar	*fwupd_version_format_to_string		(FwupdVersionFormat kind);

G_END_DECLS
