# Kinetic DP

This plugin supports updating FW for Kinetic DP converter chips.

Firmware Format
---------------

The daemon will decompress the cabinet archive and extract a firmware blob in
an unspecified binary file format.

This plugin supports the following protocol ID:

 * com.kinet-ic.dp

GUID Generation
---------------

These devices use custom GUID values, e.g.

 * `KT-DP-$(chip_family)-$(chip-ID)`
 * `KT-DP-$(chip_family)`

Please refer to the plugin source for more details about how the GUID is
constructed for specific hardware.

Vendor ID Security
------------------

The vendor ID is set from the PCI vendor, for example set to `DRM_DP_AUX_DEV:0x$(vid)`

## Requirements
### (Kernel) DP Aux Interface
Kernel 4.6 introduced an DRM DP Aux interface for manipulation of the registers
needed to access an DP hub.
This patch can be backported to earlier kernels:
https://github.com/torvalds/linux/commit/e94cb37b34eb8a88fe847438dba55c3f18bf024a

## Usage
Supported devices will be displayed in `# fwupdmgr get-devices` output.

## Supported devices

External interface access
-------------------------
This plugin requires read/write access to `/dev/drm_dp_aux*`.
