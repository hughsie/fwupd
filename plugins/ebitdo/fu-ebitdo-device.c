/*
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>
#include <string.h>

#include "fu-ebitdo-common.h"
#include "fu-ebitdo-device.h"
#include "fu-ebitdo-firmware.h"

struct _FuEbitdoDevice {
	FuUsbDevice		 parent_instance;
	guint32			 serial[9];
};

G_DEFINE_TYPE (FuEbitdoDevice, fu_ebitdo_device, FU_TYPE_USB_DEVICE)

static gboolean
fu_ebitdo_device_send (FuEbitdoDevice *self,
		       FuEbitdoPktType type,
		       FuEbitdoPktCmd subtype,
		       FuEbitdoPktCmd cmd,
		       const guint8 *in,
		       gsize in_len,
		       GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (self));
	guint8 packet[FU_EBITDO_USB_EP_SIZE] = {0};
	gsize actual_length;
	guint8 ep_out = FU_EBITDO_USB_RUNTIME_EP_OUT;
	g_autoptr(GError) error_local = NULL;
	FuEbitdoPkt *hdr = (FuEbitdoPkt *) packet;

	/* different */
	if (fu_device_has_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_IS_BOOTLOADER))
		ep_out = FU_EBITDO_USB_BOOTLOADER_EP_OUT;

	/* check size */
	if (in_len > 64 - 8) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "input buffer too large");
		return FALSE;
	}

	/* packet[0] is the total length of the packet */
	hdr->type = type;
	hdr->subtype = subtype;

	/* do we have a payload */
	if (in_len > 0) {
		hdr->cmd_len = GUINT16_TO_LE (in_len + 3);
		hdr->cmd = cmd;
		hdr->payload_len = GUINT16_TO_LE (in_len);
		if (!fu_memcpy_safe (packet, sizeof(packet), 0x08,	/* dst */
				     in, in_len, 0x0,			/* src */
				     in_len, error))
			return FALSE;
		hdr->pkt_len = (guint8) (in_len + 7);
	} else {
		hdr->cmd_len = GUINT16_TO_LE (in_len + 1);
		hdr->cmd = cmd;
		hdr->pkt_len = 5;
	}

	/* debug */
	if (g_getenv ("FWUPD_EBITDO_VERBOSE") != NULL) {
		fu_common_dump_raw (G_LOG_DOMAIN, "->DEVICE", packet, (gsize) hdr->pkt_len + 1);
		fu_ebitdo_dump_pkt (hdr);
	}

	/* get data from device */
	if (!g_usb_device_interrupt_transfer (usb_device,
					      ep_out,
					      packet,
					      FU_EBITDO_USB_EP_SIZE,
					      &actual_length,
					      FU_EBITDO_USB_TIMEOUT,
					      NULL, /* cancellable */
					      &error_local)) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "failed to send to device on ep 0x%02x: %s",
			     (guint) FU_EBITDO_USB_BOOTLOADER_EP_OUT,
			     error_local->message);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_ebitdo_device_receive (FuEbitdoDevice *self,
		       guint8 *out,
		       gsize out_len,
		       GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (self));
	guint8 packet[FU_EBITDO_USB_EP_SIZE] = {0};
	gsize actual_length;
	guint8 ep_in = FU_EBITDO_USB_RUNTIME_EP_IN;
	g_autoptr(GError) error_local = NULL;
	FuEbitdoPkt *hdr = (FuEbitdoPkt *) packet;

	/* different */
	if (fu_device_has_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_IS_BOOTLOADER))
		ep_in = FU_EBITDO_USB_BOOTLOADER_EP_IN;

	/* get data from device */
	if (!g_usb_device_interrupt_transfer (usb_device,
					      ep_in,
					      packet,
					      FU_EBITDO_USB_EP_SIZE,
					      &actual_length,
					      FU_EBITDO_USB_TIMEOUT,
					      NULL, /* cancellable */
					      &error_local)) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "failed to retrieve from device on ep 0x%02x: %s",
			     (guint) ep_in,
			     error_local->message);
		return FALSE;
	}

	/* debug */
	if (g_getenv ("FWUPD_EBITDO_VERBOSE") != NULL) {
		fu_common_dump_raw (G_LOG_DOMAIN, "<-DEVICE", packet, actual_length);
		fu_ebitdo_dump_pkt (hdr);
	}

	/* get-version (booloader) */
	if (hdr->type == FU_EBITDO_PKT_TYPE_USER_CMD &&
	    hdr->subtype == FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA &&
	    hdr->cmd == FU_EBITDO_PKT_CMD_FW_GET_VERSION) {
		if (out != NULL) {
			if (hdr->payload_len != out_len) {
				g_set_error (error,
					     G_IO_ERROR,
					     G_IO_ERROR_INVALID_DATA,
					     "outbuf size wrong, expected %" G_GSIZE_FORMAT " got %u",
					     out_len,
					     hdr->payload_len);
				return FALSE;
			}
			if (!fu_memcpy_safe (out, out_len, 0x0,					/* dst */
					     packet, sizeof(packet), sizeof(FuEbitdoPkt),	/* src */
					     hdr->payload_len, error))
				return FALSE;
		}
		return TRUE;
	}

	/* get-version (firmware) -- not a packet, just raw data! */
	if (hdr->pkt_len == FU_EBITDO_PKT_CMD_GET_VERSION_RESPONSE) {
		if (out != NULL) {
			if (out_len != 4) {
				g_set_error (error,
					     G_IO_ERROR,
					     G_IO_ERROR_INVALID_DATA,
					     "outbuf size wrong, expected 4 got %" G_GSIZE_FORMAT,
					     out_len);
				return FALSE;
			}
			if (!fu_memcpy_safe (out, out_len, 0x0,					/* dst */
					     packet, sizeof(packet), 0x1,			/* src */
					     4, error))
				return FALSE;
		}
		return TRUE;
	}

	/* verification-id response */
	if (hdr->type == FU_EBITDO_PKT_TYPE_USER_CMD &&
	    hdr->subtype == FU_EBITDO_PKT_CMD_VERIFICATION_ID) {
		if (out != NULL) {
			if (hdr->cmd_len != out_len) {
				g_set_error (error,
					     G_IO_ERROR,
					     G_IO_ERROR_INVALID_DATA,
					     "outbuf size wrong, expected %" G_GSIZE_FORMAT " got %i",
					     out_len,
					     hdr->cmd_len);
				return FALSE;
			}
			if (!fu_memcpy_safe (out, out_len, 0x0,					/* dst */
					     packet, sizeof(packet), sizeof(FuEbitdoPkt) - 3,	/* src */
					     hdr->cmd_len, error))
				return FALSE;
		}
		return TRUE;
	}

	/* update-firmware-data */
	if (hdr->type == FU_EBITDO_PKT_TYPE_USER_CMD &&
	    hdr->subtype == FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA &&
	    hdr->payload_len == 0x00) {
		if (hdr->cmd != FU_EBITDO_PKT_CMD_ACK) {
			g_set_error (error,
				     G_IO_ERROR,
				     G_IO_ERROR_INVALID_DATA,
				     "write failed, got %s",
				     fu_ebitdo_pkt_cmd_to_string (hdr->cmd));
			return FALSE;
		}
		return TRUE;
	}

	/* unhandled */
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "unexpected device response");
	return FALSE;
}

static void
fu_ebitdo_device_set_version (FuEbitdoDevice *self, guint32 version)
{
	g_autofree gchar *tmp = NULL;
	tmp = g_strdup_printf ("%u.%02u", version / 100, version % 100);
	fu_device_set_version_format (FU_DEVICE (self), FWUPD_VERSION_FORMAT_PAIR);
	fu_device_set_version_raw (FU_DEVICE (self), version);
	fu_device_set_version (FU_DEVICE (self), tmp);
}

static gboolean
fu_ebitdo_device_validate (FuEbitdoDevice *self, GError **error)
{
	const gchar *ven;
	const gchar *allowlist[] = {
		"8Bitdo",
		"SFC30",
		NULL };

	/* this is a new, always valid, VID */
	if (fu_usb_device_get_vid (FU_USB_DEVICE (self)) == 0x2dc8)
		return TRUE;

	/* verify the vendor prefix against a allowlist */
	ven = fu_device_get_vendor (FU_DEVICE (self));
	if (ven == NULL) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "could not check vendor descriptor: ");
		return FALSE;
	}
	for (guint i = 0; allowlist[i] != NULL; i++) {
		if (g_str_has_prefix (ven, allowlist[i]))
			return TRUE;
	}
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_INVALID_DATA,
		     "vendor '%s' did not match allowlist, "
		     "probably not a 8BitDo device…", ven);
	return FALSE;
}

static gboolean
fu_ebitdo_device_open (FuDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	FuEbitdoDevice *self = FU_EBITDO_DEVICE (device);

	/* FuUsbDevice->open */
	if (!FU_DEVICE_CLASS (fu_ebitdo_device_parent_class)->open (device, error))
		return FALSE;

	/* open, then ensure this is actually 8BitDo hardware */
	if (!fu_ebitdo_device_validate (self, error))
		return FALSE;
	if (!g_usb_device_claim_interface (usb_device, 0, /* 0 = idx? */
					   G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					   error)) {
		return FALSE;
	}

	/* success */
	return TRUE;
}

static gboolean
fu_ebitdo_device_setup (FuDevice *device, GError **error)
{
	FuEbitdoDevice *self = FU_EBITDO_DEVICE (device);
	gdouble tmp;
	guint32 version_tmp = 0;
	guint32 serial_tmp[9] = {0};

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS (fu_ebitdo_device_parent_class)->setup (device, error))
		return FALSE;

	/* in firmware mode */
	if (!fu_device_has_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		if (!fu_ebitdo_device_send (self,
					 FU_EBITDO_PKT_TYPE_USER_CMD,
					 FU_EBITDO_PKT_CMD_GET_VERSION,
					 0,
					 NULL, 0, /* in */
					 error)) {
			return FALSE;
		}
		if (!fu_ebitdo_device_receive (self,
					    (guint8 *) &version_tmp,
					    sizeof(version_tmp),
					    error)) {
			return FALSE;
		}
		tmp = (gdouble) GUINT32_FROM_LE (version_tmp);
		fu_ebitdo_device_set_version (self, tmp);
		return TRUE;
	}

	/* get version */
	if (!fu_ebitdo_device_send (self,
				 FU_EBITDO_PKT_TYPE_USER_CMD,
				 FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA,
				 FU_EBITDO_PKT_CMD_FW_GET_VERSION,
				 NULL, 0, /* in */
				 error)) {
		return FALSE;
	}
	if (!fu_ebitdo_device_receive (self,
				    (guint8 *) &version_tmp,
				    sizeof(version_tmp),
				    error)) {
		return FALSE;
	}
	tmp = (gdouble) GUINT32_FROM_LE (version_tmp);
	fu_ebitdo_device_set_version (self, tmp);

	/* get verification ID */
	if (!fu_ebitdo_device_send (self,
				 FU_EBITDO_PKT_TYPE_USER_CMD,
				 FU_EBITDO_PKT_CMD_GET_VERIFICATION_ID,
				 0x00, /* cmd */
				 NULL, 0,
				 error)) {
		return FALSE;
	}
	if (!fu_ebitdo_device_receive (self,
				    (guint8 *) &serial_tmp, sizeof(serial_tmp),
				    error)) {
		return FALSE;
	}
	for (guint i = 0; i < 9; i++)
		self->serial[i] = GUINT32_FROM_LE (serial_tmp[i]);

	/* success */
	return TRUE;
}

static gboolean
fu_ebitdo_device_detach (FuDevice *device, GError **error)
{
	/* not required */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER))
		return TRUE;

	/* generate a message if not already set from the metadata */
	if (fu_device_get_update_message (device) == NULL) {
		GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
		g_autoptr(GString) msg = g_string_new (NULL);
		g_string_append (msg, "Not in bootloader mode: Disconnect the controller, ");
		switch (g_usb_device_get_pid (usb_device)) {
		case 0xab11: /* FC30 */
		case 0xab12: /* NES30 */
		case 0xab21: /* SFC30 */
		case 0xab20: /* SNES30 */
		case 0x9012: /* SN30v2 */
			g_string_append (msg, "hold down L+R+START for 3 seconds until "
					      "both LED lights flashing, ");
			break;
		case 0x9000: /* FC30PRO */
		case 0x9001: /* NES30PRO */
			g_string_append (msg, "hold down RETURN+POWER for 3 seconds until "
					      "both LED lights flashing, ");
			break;
		case 0x1002: /* FC30-ARCADE */
			g_string_append (msg, "hold down L1+R1+HOME for 3 seconds until "
					      "both blue LED and green LED blink, ");
			break;
		case 0x6000: /* SF30 pro: Dinput mode */
		case 0x6001: /* SN30 pro: Dinput mode */
		case 0x6002: /* SN30 pro+: Dinput mode */
		case 0x028e: /* SF30/SN30 pro: Xinput mode */
		case 0x5006: /* M30 */
			g_string_append (msg, "press and hold L1+R1+START for 3 seconds "
					      "until the LED on top blinks red, ");
			break;
		case 0x2100: /* SN30 for Android */
		case 0x2101: /* SN30 for Android */
			g_string_append (msg, "press and hold LB+RB+Xbox buttons "
					      "both white LED and green LED blink, ");
			break;
		case 0x9015: /* N30 Pro 2 */
		    g_string_append (msg, "press and hold L1+R1+START buttons "
					  "until the yellow LED blinks, ");
			break;
		default:
			g_string_append (msg, "do what it says in the manual, ");
			break;
		}
		g_string_append (msg, "then re-connect controller");
		fu_device_set_update_message_kind (device, FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE);
		fu_device_set_update_message (device, msg->str);
	}

	/* wait */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_BUSY);
	fu_device_set_progress (device, 0);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER);
	return TRUE;
}

static gboolean
fu_ebitdo_device_write_firmware (FuDevice *device,
				 FuFirmware *firmware,
				 FwupdInstallFlags flags,
				 GError **error)
{
	FuEbitdoDevice *self = FU_EBITDO_DEVICE (device);
	const guint8 *buf;
	gsize bufsz = 0;
	guint32 serial_new[3];
	g_autoptr(GBytes) fw_hdr = NULL;
	g_autoptr(GBytes) fw_payload = NULL;
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GPtrArray) chunks = NULL;
	const guint32 app_key_index[16] = {
		0x186976e5, 0xcac67acd, 0x38f27fee, 0x0a4948f1,
		0xb75b7753, 0x1f8ffa5c, 0xbff8cf43, 0xc4936167,
		0x92bd03f0, 0x5573c6ed, 0x57d8845b, 0x827197ac,
		0xb91901c9, 0x3917edfe, 0xbcd6344f, 0xcf9e23b5
	};

	/* not in bootloader mode */
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NEEDS_USER_ACTION,
				     "Not in bootloader mode");
		return FALSE;
	}

	/* get header and payload */
	fw_hdr = fu_firmware_get_image_by_id_bytes (firmware,
						    FU_FIRMWARE_ID_HEADER,
						    error);
	if (fw_hdr == NULL)
		return FALSE;
	fw_payload = fu_firmware_get_bytes (firmware, error);
	if (fw_payload == NULL)
		return FALSE;

	/* set up the firmware header */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
	buf = g_bytes_get_data (fw_hdr, &bufsz);
	if (!fu_ebitdo_device_send (self,
				    FU_EBITDO_PKT_TYPE_USER_CMD,
				    FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA,
				    FU_EBITDO_PKT_CMD_FW_UPDATE_HEADER,
				    buf, bufsz, error)) {
		g_prefix_error (error, "failed to set up firmware header: ");
		return FALSE;
	}
	if (!fu_ebitdo_device_receive (self, NULL, 0, error)) {
		g_prefix_error (error, "failed to get ACK for fw update header: ");
		return FALSE;
	}

	/* flash the firmware in 32 byte blocks */
	chunks = fu_chunk_array_new_from_bytes (fw_payload, 0x0, 0x0, 32);
	for (guint i = 0; i < chunks->len; i++) {
		FuChunk *chk = g_ptr_array_index (chunks, i);
		if (g_getenv ("FWUPD_EBITDO_VERBOSE") != NULL) {
			g_debug ("writing %u bytes to 0x%04x of 0x%04x",
				 fu_chunk_get_data_sz (chk),
				 fu_chunk_get_address (chk),
				 fu_chunk_get_data_sz (chk));
		}
		if (!fu_ebitdo_device_send (self,
					    FU_EBITDO_PKT_TYPE_USER_CMD,
					    FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA,
					    FU_EBITDO_PKT_CMD_FW_UPDATE_DATA,
					    fu_chunk_get_data (chk),
					    fu_chunk_get_data_sz (chk),
					    error)) {
			g_prefix_error (error,
					"failed to write firmware @0x%04x: ",
					fu_chunk_get_address (chk));
			return FALSE;
		}
		if (!fu_ebitdo_device_receive (self, NULL, 0, error)) {
			g_prefix_error (error,
					"failed to get ACK for write firmware @0x%04x: ",
					fu_chunk_get_address (chk));
			return FALSE;
		}
		fu_device_set_progress_full (device, fu_chunk_get_idx (chk), chunks->len);
	}

	/* set the "encode id" which is likely a checksum, bluetooth pairing
	 * or maybe just security-through-obscurity -- also note:
	 * SET_ENCODE_ID enforces no read for success?! */
	serial_new[0] = self->serial[0] ^ app_key_index[self->serial[0] & 0x0f];
	serial_new[1] = self->serial[1] ^ app_key_index[self->serial[1] & 0x0f];
	serial_new[2] = self->serial[2] ^ app_key_index[self->serial[2] & 0x0f];
	if (!fu_ebitdo_device_send (self,
				 FU_EBITDO_PKT_TYPE_USER_CMD,
				 FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA,
				 FU_EBITDO_PKT_CMD_FW_SET_ENCODE_ID,
				 (guint8 *) serial_new,
				 sizeof(serial_new),
				 error)) {
		g_prefix_error (error, "failed to set encoding ID: ");
		return FALSE;
	}

	/* mark flash as successful */
	if (!fu_ebitdo_device_send (self,
				 FU_EBITDO_PKT_TYPE_USER_CMD,
				 FU_EBITDO_PKT_CMD_UPDATE_FIRMWARE_DATA,
				 FU_EBITDO_PKT_CMD_FW_UPDATE_OK,
				 NULL, 0,
				 error)) {
		g_prefix_error (error, "failed to mark firmware as successful: ");
		return FALSE;
	}
	if (!fu_ebitdo_device_receive (self, NULL, 0, &error_local)) {
		g_prefix_error (&error_local, "failed to get ACK for mark firmware as successful: ");
		if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_WILL_DISAPPEAR)) {
			fu_device_set_remove_delay (device, 0);
			g_debug ("%s", error_local->message);
			return TRUE;
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}

	/* success! */
	return TRUE;
}

static gboolean
fu_ebitdo_device_attach (FuDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	g_autoptr(GError) error_local = NULL;

	/* when doing a soft-reboot the device does not re-enumerate properly
	 * so manually reboot the GUsbDevice */
	fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
	if (!g_usb_device_reset (usb_device, &error_local)) {
		g_prefix_error (&error_local, "failed to force-reset device: ");
		if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_WILL_DISAPPEAR)) {
			fu_device_set_remove_delay (device, 0);
			g_debug ("%s", error_local->message);
			return TRUE;
		}
		g_propagate_error (error, g_steal_pointer (&error_local));
		return FALSE;
	}

	/* not all 8bito devices come back in the right mode */
	if (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_WILL_DISAPPEAR))
		fu_device_set_remove_delay (device, 0);
	else
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);

	/* success! */
	return TRUE;
}

static gboolean
fu_ebitdo_device_probe (FuDevice *device, GError **error)
{
	/* FuUsbDevice->probe */
	if (!FU_DEVICE_CLASS (fu_ebitdo_device_parent_class)->probe (device, error))
		return FALSE;

	/* allowed, but requires manual bootloader step */
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_set_remove_delay (device, FU_DEVICE_REMOVE_DELAY_USER_REPLUG);

	/* set name and vendor */
	fu_device_set_summary (device, "A redesigned classic game controller");
	fu_device_set_vendor (device, "8BitDo");

	/* add a hardcoded icon name */
	fu_device_add_icon (device, "input-gaming");

	/* only the bootloader can do the update */
	if (!fu_device_has_flag (device, FWUPD_DEVICE_FLAG_IS_BOOTLOADER)) {
		fu_device_add_counterpart_guid (device, "USB\\VID_0483&PID_5750");
		fu_device_add_counterpart_guid (device, "USB\\VID_2DC8&PID_5750");
	}

	/* success */
	return TRUE;
}

static FuFirmware *
fu_ebitdo_device_prepare_firmware (FuDevice *device,
				   GBytes *fw,
				   FwupdInstallFlags flags,
				   GError **error)
{
	g_autoptr(FuFirmware) firmware = fu_ebitdo_firmware_new ();
	if (!fu_firmware_parse (firmware, fw, flags, error))
		return NULL;
	return g_steal_pointer (&firmware);
}

static void
fu_ebitdo_device_init (FuEbitdoDevice *self)
{
	fu_device_add_protocol (FU_DEVICE (self), "com.8bitdo");
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS);
	fu_device_add_internal_flag (FU_DEVICE (self), FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
}

static void
fu_ebitdo_device_class_init (FuEbitdoDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	klass_device->write_firmware = fu_ebitdo_device_write_firmware;
	klass_device->setup = fu_ebitdo_device_setup;
	klass_device->detach = fu_ebitdo_device_detach;
	klass_device->attach = fu_ebitdo_device_attach;
	klass_device->open = fu_ebitdo_device_open;
	klass_device->probe = fu_ebitdo_device_probe;
	klass_device->prepare_firmware = fu_ebitdo_device_prepare_firmware;
}
