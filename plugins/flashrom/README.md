Flashrom
========

Introduction
------------

This plugin uses `flashrom` to update the system firmware.

Firmware Format
---------------

The daemon will decompress the cabinet archive and extract a firmware blob in
an unspecified binary file format, which is typically the raw input for an
EEPROM programmer.

This plugin supports the following protocol ID:

 * org.flashrom

Coreboot Version String
-----------------------

The coreboot version string can have an optional prefix (see below).
After the optional prefix the *major*, *minor* string follows and finally
the *build string*, containing the exact commit and repository state, follows.

For example `4.10-989-gc8a4e4b9c5-dirty`

**Exception on Lenovo devices:**

The thinkpad_acpi kernel module requires a specific pattern in the DMI version
string. To satisfy those requirements coreboot adds the CBETxxxx prefix to the
DMI version string on all Lenovo devices.

For example `CBET4000 4.10-989-gc8a4e4b9c5-dirty`

The coreboot DMI version string always starts with `CBET`.

GUID Generation
---------------

Internal device uses hardware ID values which are derived from SMBIOS.

 * HardwareID-3
 * HardwareID-4
 * HardwareID-5
 * HardwareID-6
 * HardwareID-10

They should match the values provided by `fwupdtool hwids` or the
`ComputerHardwareIds.exe` Windows utility.

lspcon-i2c-spi devices use the customized DeviceInstanceId values, e.g.

 * FLASHROM-LSPCON-I2C-SPI\VEN_1AF8&DEV_0175

Update Behavior
---------------

The firmware is deployed to the SPI chip when the machine is in normal runtime
mode, but it is only used when the device is rebooted.

Vendor ID Security
------------------

The vendor ID is set from the BIOS vendor, for example `DMI:Google`

Quirk use
---------
This plugin uses the following plugin-specific quirks:

| Quirk                         | Description                                             | Minimum fwupd version |
|-------------------------------|---------------------------------------------------------|-----------------------|
|`FlashromProgrammer`           | Used to specify the libflashrom programmer to be used.  | 1.5.9                 |
|`FlashromNeedsFdopssUnlock`    | Enables ME region flashing via FDOPSS override.         | 1.6.2                 |

The flashrom plugin uses the firmware size obtained from DMI, in case that value
is not correct, it's possible to add a `FirmwareSizeMax` quirk for your device.

External interface access
---
This plugin requires access to all interfaces that `libflashrom` has been compiled for.
This typically is `/sys/bus/spi` but there may be other interfaces as well.


Unlocking the ME region
-----------------------

Some machines support unlocking the ME region of the SPI chip for flashing via
the use of the Intel ME Debug Mode Pin-Strap (FDOPSS). In that case, before
installing an update, the device will need to be unlocked.
To unlock the ME region, run:

```bash
# fwupdmgr unlock
```

You will then be prompted to shut down the machine for the unlock to take effect.
After powering it on again, you will be able to update the device as usual.
