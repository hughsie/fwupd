/*
 * Copyright (C) 2019 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2021 Jeremy Soller <jeremy@system76.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fu-system76-launch-device.h"

#define SYSTEM76_LAUNCH_CMD_VERSION	3
#define SYSTEM76_LAUNCH_CMD_RESET	6
#define SYSTEM76_LAUNCH_TIMEOUT		1000

struct _FuSystem76LaunchDevice {
	FuUsbDevice			 parent_instance;
};

G_DEFINE_TYPE (FuSystem76LaunchDevice, fu_system76_launch_device, FU_TYPE_USB_DEVICE)

static gboolean
fu_system76_launch_device_command (FuDevice *device, guint8 *data, gsize len, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	const guint8 ep_in = 0x82;
	const guint8 ep_out = 0x03;
	gsize actual_len = 0;

	/* send command */
	if (!g_usb_device_interrupt_transfer (usb_device,
					      ep_out,
					      data,
					      len,
					      &actual_len,
					      SYSTEM76_LAUNCH_TIMEOUT,
					      NULL,
					      error)) {
		g_prefix_error (error, "failed to send command: ");
		return FALSE;
	}
	if (actual_len < len) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "command truncated: sent %" G_GSIZE_FORMAT " bytes",
			     actual_len);
		return FALSE;
	}

	/* receive response */
	if (!g_usb_device_interrupt_transfer (usb_device,
					      ep_in,
					      data,
					      len,
					      &actual_len,
					      SYSTEM76_LAUNCH_TIMEOUT,
					      NULL,
					      error)) {
		g_prefix_error (error, "failed to read response: ");
		return FALSE;
	}
	if (actual_len < len) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "response truncated: received %" G_GSIZE_FORMAT " bytes",
			     actual_len);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_system76_launch_device_setup (FuDevice *device, GError **error)
{
	guint8 data[32] = { 0 };
	g_autofree gchar *version = NULL;

	/* FuUsbDevice->setup */
	if (!FU_DEVICE_CLASS (fu_system76_launch_device_parent_class)->setup (device, error))
		return FALSE;

	/* execute version command */
	data[0] = SYSTEM76_LAUNCH_CMD_VERSION;
	if (!fu_system76_launch_device_command (device, data, sizeof(data), error)) {
		g_prefix_error (error, "failed to execute version command: ");
		return FALSE;
	}

	version = g_strdup_printf ("%s", &data[2]);
	fu_device_set_version (device, version);

	return TRUE;
}

static gboolean
fu_system76_launch_device_reset (FuDevice *device, guint8 *rc, GError **error)
{
	guint8 data[32] = { SYSTEM76_LAUNCH_CMD_RESET, 0 };

	/* execute reset command */
	if (!fu_system76_launch_device_command (device, data, sizeof(data), error)) {
		g_prefix_error (error, "failed to execute reset command: ");
		return FALSE;
	}

	*rc = data[1];
	return TRUE;
}

static gboolean
fu_system76_launch_device_detach (FuDevice *device, GError **error)
{
	guint8 rc = 0x0;

	/* prompt for unlock if reset was blocked */
	if (!fu_system76_launch_device_reset (device, &rc, error))
		return FALSE;
	if (rc != 0) {
		g_autoptr(GTimer) timer = g_timer_new ();

		/* generate a message if not already set */
		if (fu_device_get_update_message (device) == NULL) {
			g_autofree gchar *msg = NULL;
			msg = g_strdup_printf ("To ensure you have physical access, %s needs to be manually unlocked. "
					       "Please press Fn+Esc to unlock and re-run the update.",
					       fu_device_get_name (device));
			fu_device_set_update_message_kind (device, FWUPD_DEVICE_MESSAGE_KIND_IMMEDIATE);
			fu_device_set_update_message (device, msg);
		}

		/* the user has to do something */
		fu_device_set_progress (device, 0);
		fu_device_add_flag (device, FWUPD_DEVICE_FLAG_NEEDS_BOOTLOADER);

		/* poll for the user-unlock */
		do {
			g_usleep (G_USEC_PER_SEC);
			if (!fu_system76_launch_device_reset (device, &rc, error))
				return FALSE;
		} while (rc != 0 &&
			 g_timer_elapsed (timer, NULL) * 1000.f < FU_DEVICE_REMOVE_DELAY_USER_REPLUG);
		if (rc != 0) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_NEEDS_USER_ACTION,
					     fu_device_get_update_message (device));
			return FALSE;
		}
		return TRUE;
	}

	fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	return TRUE;
}

static gboolean
fu_system76_launch_device_open (FuDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	const guint8 iface_idx = 0x01;

	/* FuUsbDevice->open */
	if (!FU_DEVICE_CLASS (fu_system76_launch_device_parent_class)->open (device, error))
		return FALSE;

	if (!g_usb_device_claim_interface (usb_device, iface_idx,
					   G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					   error)) {
		g_prefix_error (error, "failed to claim interface: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_system76_launch_device_close (FuDevice *device, GError **error)
{
	GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));
	const guint8 iface_idx = 0x01;

	if (!g_usb_device_release_interface (usb_device, iface_idx,
					     G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
					     error)) {
		g_prefix_error (error, "failed to release interface: ");
		return FALSE;
	}

	/* FuUsbDevice->close */
	return FU_DEVICE_CLASS (fu_system76_launch_device_parent_class)->close (device, error);
}

static void
fu_system76_launch_device_init (FuSystem76LaunchDevice *self)
{
	fu_device_set_remove_delay (FU_DEVICE (self), FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_ADD_COUNTERPART_GUIDS);
	fu_device_add_internal_flag (FU_DEVICE (self), FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_set_version_format (FU_DEVICE (self), FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_add_protocol (FU_DEVICE (self), "org.usb.dfu");
	fu_device_retry_set_delay (FU_DEVICE (self), 100);
}

static void
fu_system76_launch_device_class_init (FuSystem76LaunchDeviceClass *klass)
{
	FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
	klass_device->setup = fu_system76_launch_device_setup;
	klass_device->detach = fu_system76_launch_device_detach;
	klass_device->open = fu_system76_launch_device_open;
	klass_device->close = fu_system76_launch_device_close;
}
