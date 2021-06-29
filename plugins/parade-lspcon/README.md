Parade LSPCON
=============

Introduction
------------

This plugin updates the firmware of HDMI level shifter and protocol converter
(LSPCON) devices made by Parade Technologies, such as the PS175.

These devices communicate over I²C, via either the DisplayPort aux channel or a
dedicated bus- this plugin uses a dedicated bus declared by system firmware for,
flashing, and reads the device firmware version from DPCD. Quirks specify the
DisplayPort bus over which DPCD is read for a given system.

Firmware is stored on an external flash attached to an SPI bus on the device.
The attached flash is assumed to be compatible with the W25Q20 series
of devices, in particular supporting a 64k Block Erase command (0xD8)
with 24-bit address and Write Enable for Volatile Status Register (0x05).

Firmware Format
---------------

The device firmware is in an unspecified binary format that is written directly
to an inactive partition of the Flash attached to the device.

This plugin supports the following protocol ID:

 * com.paradetech.ps176

GUID Generation
---------------

The plugin uses a custom DeviceInstanceId value derived from the device name
provided by system firmware and read from sysfs, such as:

 * `PARADE-LSPCON\NAME_1AF80175:00`
 * `PARADE-LSPCON\NAME_1AF80175:00&FAMILY_Google_Hatch`

Quirk use
---------

This plugin uses the following plugin-specific quirks:

| Quirk                       | Description                                                                 | Minimum fwupd version |
| `ParadeLspconAuxDeviceName` | sysfs name of the `drm_dp_aux_dev` over which device version should be read | 1.6.2 |

Vendor ID security
------------------

The vendor ID is specified by system firmware (such as ACPI tables) and is
part of the device's name as read from sysfs.

External interface access
-------------------------

This plugin requires access to the DisplayPort aux channel to read DPCD, such
as `/dev/drm_dp_aux0` as well as the i2c bus attached to the device, such as
`/dev/i2c-7`.
