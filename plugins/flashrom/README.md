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

| Quirk                  | Description                                 | Minimum fwupd version |
|------------------------|---------------------------------------------|-----------------------|
|`FlashromProgrammer`    | Used to specify the libflashrom programmer to be used.     | 1.5.9                 |


External interface access
---
This plugin requires access to all interfaces that `libflashrom` has been compiled for.
This typically is `/sys/bus/spi` but there may be other interfaces as well.
