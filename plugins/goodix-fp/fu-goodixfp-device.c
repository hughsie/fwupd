/*
 * Copyright (C) 2016-2017 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2020 boger wang <boger@goodix.com>
 * 
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <string.h>

#include "fu-chunk.h"
#include "fu-goodixfp-common.h"
#include "fu-goodixfp-device.h"

struct _FuGoodixFpDevice
{
  FuUsbDevice parent_instance;
  guint16     start_addr;
};

G_DEFINE_TYPE (FuGoodixFpDevice, fu_goodixfp_device, FU_TYPE_USB_DEVICE)


#define GX_USB_BULK_EP_IN  (3 | 0x80)
#define GX_USB_BULK_EP_OUT (1 | 0x00)

#define GX_USB_BULK_EP_SIZE 64
#define GX_USB_INTERFACE 0x0000

#define GX_USB_DATAIN_TIMEOUT 2000                   /* ms */
#define GX_USB_DATAOUT_TIMEOUT 2000                  /* ms */
#define GX_FLASH_TRANSFER_BLOCK_SIZE 1000            /* 1000  */


static gboolean
goodixfp_device_cmd_send (GUsbDevice *usbdevice,
                          guint8      cmd0,
                          guint8      cmd1,
                          guint8      pkg_eop,
                          GByteArray *request,
                          GError    **error)
{
  gboolean ret = FALSE;
  pack_header header = {0, };
  guint32 crc_calc = 0;
  gsize actual_len = 0;

  g_autoptr(GByteArray) buf = g_byte_array_new ();

  init_pack_header (&header, request->len, cmd0, cmd1, pkg_eop);
  g_byte_array_append (buf, (guint8 *) &header, PACKAGE_HEADER_SIZE);
  g_byte_array_append (buf, request->data, request->len);
  gx_proto_crc32_calc (buf->data, PACKAGE_HEADER_SIZE + request->len, &crc_calc);

  fu_byte_array_append_uint32 (buf, crc_calc, G_LITTLE_ENDIAN);

  /* send zero length package */
  ret = g_usb_device_bulk_transfer (usbdevice,
                                    GX_USB_BULK_EP_OUT,
                                    NULL,
                                    0,
                                    NULL,
                                    GX_USB_DATAOUT_TIMEOUT, NULL, error);
  if (!ret)
    {
      g_prefix_error (error, "failed to request: ");
      return FALSE;
    }

  if (g_getenv ("FWUPD_GOODIXFP_VERBOSE") != NULL)
    {
      fu_common_dump_full (G_LOG_DOMAIN, "REQST",
                           buf->data, buf->len, 16,
                           FU_DUMP_FLAGS_SHOW_ADDRESSES);
    }
  /* send data */  
  ret = g_usb_device_bulk_transfer (usbdevice,
                                    GX_USB_BULK_EP_OUT,
                                    buf->data,
                                    buf->len,
                                    &actual_len,
                                    GX_USB_DATAOUT_TIMEOUT, NULL, error);
  if (!ret)
    {
      g_prefix_error (error, "failed to request: ");
      return FALSE;
    }

  if (actual_len != buf->len)
    {
      g_set_error_literal (error,
                           FWUPD_ERROR,
                           FWUPD_ERROR_INTERNAL,
                           "Invalid length");
      return FALSE;
    }

  /* success */
  return TRUE;
}

static gboolean
goodixfp_device_cmd_recv (GUsbDevice          *usbdevice,
                          pgxfp_cmd_response_t presponse,
                          gboolean             data_reply,
                          GError             **error)
{
  gboolean ret = FALSE;
  pack_header header = {0, };
  guint32 crc32_calc = 0;
  gsize actual_len = 0;
  GByteArray *reply = NULL;

  g_assert (presponse != NULL);

  /*
   * package format
   * | zlp | ack | zlp | data |
   */
  while (1)
    {
      if (reply != NULL)
        g_byte_array_free (reply, TRUE);
      reply = g_byte_array_new ();
      g_byte_array_set_size (reply, GX_FLASH_TRANSFER_BLOCK_SIZE);
      ret = g_usb_device_bulk_transfer (usbdevice,
                                        GX_USB_BULK_EP_IN,
                                        reply->data,
                                        reply->len,
                                        &actual_len, /* allowed to return short read */
                                        GX_USB_DATAIN_TIMEOUT, NULL, error);
      if (!ret)
        {
          g_prefix_error (error, "failed to reply: ");
          return FALSE;
        }
      if (ret && (actual_len == 0))
        /* receive zero length package */
        continue;
      if (g_getenv ("FWUPD_GOODIXFP_VERBOSE") != NULL)
        {
          fu_common_dump_full (G_LOG_DOMAIN, "REPLY",
                               reply->data, actual_len, 16,
                               FU_DUMP_FLAGS_SHOW_ADDRESSES);
        }
      /* parse package header */
      ret = gx_proto_parse_header (reply->data, actual_len, &header);
      if (!ret)
        {
          g_set_error_literal (error,
                               FWUPD_ERROR,
                               FWUPD_ERROR_INTERNAL,
                               "Invalid value");
          return FALSE;
        }
      gx_proto_crc32_calc (reply->data, PACKAGE_HEADER_SIZE + header.len, &crc32_calc);

      if(crc32_calc != GUINT32_FROM_LE (*(guint32 *) (reply->data + PACKAGE_HEADER_SIZE + header.len)))
        {
          g_set_error_literal (error,
                               FWUPD_ERROR,
                               FWUPD_ERROR_INTERNAL,
                               "Invalid checksum");
          return FALSE;
        }
      /* parse package data */
      ret = gx_proto_parse_body (header.cmd0, reply->data + PACKAGE_HEADER_SIZE, header.len, presponse);
      if (!ret)
        {
          g_set_error_literal (error,
                               FWUPD_ERROR,
                               FWUPD_ERROR_INTERNAL,
                               "Invalid value");
          return FALSE;
        }
      if((header.cmd0 == GX_CMD_ACK) && data_reply )
        continue;
      break;
    }
  /* success */
  return TRUE;
}

static gboolean
fu_goodixfp_device_cmd_xfer (FuGoodixFpDevice    *device,
                             guint8               cmd0,
                             guint8               cmd1,
                             guint8               pkg_eop,
                             GByteArray          *request,
                             pgxfp_cmd_response_t presponse,
                             gboolean             data_reply,
                             GError             **error)
{
  gboolean ret = FALSE;

  GUsbDevice *usb_device = fu_usb_device_get_dev (FU_USB_DEVICE (device));

  ret = goodixfp_device_cmd_send (usb_device, cmd0, cmd1, pkg_eop, request, error);
  if (!ret)
    {
      return FALSE;
    }

  ret = goodixfp_device_cmd_recv (usb_device, presponse, data_reply, error);
  if (!ret)
    {
      return FALSE;
    }
  return TRUE;
}

static gchar *
fu_goodixfp_device_get_version (FuGoodixFpDevice *self, GError **error)
{
  gxfp_cmd_response_t reponse = {0, };
  guint8 dummy = 0;
  gchar ver[9] = {0};

  g_autoptr(GByteArray) request = g_byte_array_new ();
  fu_byte_array_append_uint8 (request, dummy);
  if (!fu_goodixfp_device_cmd_xfer (self, GX_CMD_VERSION, GX_CMD1_DEFAULT,
                                    0,
                                    request,
                                    &reponse,
                                    TRUE,
                                    error))
    return NULL;
  memcpy (ver, reponse.version_info.fwversion, 8);
  return g_strdup (ver);
}

static gboolean
fu_goodixfp_device_update_init (FuGoodixFpDevice *self, GError **error)
{
  gxfp_cmd_response_t reponse = {0, };
  g_autoptr(GByteArray) request = g_byte_array_new ();
  /* update initial */
  if (!fu_goodixfp_device_cmd_xfer (self, GX_CMD_UPGRADE, GX_CMD_UPGRADE_INIT,
                                    0,
                                    request,
                                    &reponse,
                                    TRUE,
                                    error))
    return FALSE;
  return (reponse.result == 0) ? TRUE : FALSE;
}

static gboolean
fu_goodixfp_device_attach (FuDevice *device, GError **error)
{
  FuGoodixFpDevice *self = FU_GOODIXFP_DEVICE (device);
  g_autoptr(GByteArray) request = g_byte_array_new ();
  gxfp_cmd_response_t reponse = {0, };
  
  fu_device_set_status (device, FWUPD_STATUS_DEVICE_RESTART);
  
  /* reset device */
  if (!fu_goodixfp_device_cmd_xfer (self, GX_CMD_RESET, 0x03,
                                    0,
                                    request,
                                    &reponse,
                                    FALSE,
                                    error))
    return FALSE;
  if (reponse.result != 0)
    return FALSE;
    
  fu_device_add_flag (device, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
  return TRUE;
}

static gboolean
fu_goodixfp_device_open (FuUsbDevice *device, GError **error)
{
  GUsbDevice *usb_device = fu_usb_device_get_dev (device);

  if (!g_usb_device_claim_interface (usb_device, GX_USB_INTERFACE,
                                     G_USB_DEVICE_CLAIM_INTERFACE_BIND_KERNEL_DRIVER,
                                     error))
    return FALSE;

  /* success */
  return TRUE;
}

static gboolean
fu_goodixfp_device_setup (FuDevice *device, GError **error)
{
  FuGoodixFpDevice *self = FU_GOODIXFP_DEVICE (device);
  g_autofree gchar *version = NULL;
  g_autoptr(GError) error_local = NULL;

  version = fu_goodixfp_device_get_version (self, &error_local);
  if (version != NULL)
    {
      g_debug ("obtained fwver using API '%s'", version);
      fu_device_set_version (device, version);
    }
  else
    {
      g_warning ("failed to get firmware version: %s",
                 error_local->message);
    }

  /* success */
  return TRUE;
}

static gboolean
fu_goodixfp_device_write_firmware (FuDevice         *device,
                                   FuFirmware       *firmware,
                                   FwupdInstallFlags flags,
                                   GError          **error)
{
  FuGoodixFpDevice *self = FU_GOODIXFP_DEVICE (device);
  g_autoptr(GBytes) fw = NULL;
  g_autoptr(GPtrArray) chunks = NULL;
  g_autoptr(GError) error_local = NULL;
  gboolean wait_data_reply = FALSE;
  guint8 pkg_eop = 0x80;
  gxfp_cmd_response_t reponse = {0, };
  /* get default image */
  fw = fu_firmware_get_image_default_bytes (firmware, error);
  if (fw == NULL)
    return FALSE;

  /* build packets */
  chunks = fu_chunk_array_new_from_bytes (fw,
                                          0x00,
                                          0x00,         /* page_sz */
                                          GX_FLASH_TRANSFER_BLOCK_SIZE);
  /* don't auto-boot firmware */
  fu_device_set_status (device, FWUPD_STATUS_DEVICE_WRITE);
  if(!fu_goodixfp_device_update_init(self, &error_local))
  {
    g_set_error (error,
          FWUPD_ERROR,
          FWUPD_ERROR_WRITE,
          "failed to init update: %s",
          error_local->message);
    return FALSE;
  }
  /* write each block */
  for (guint i = 0; i < chunks->len; i++)
    {
      FuChunk *chk = g_ptr_array_index (chunks, i);
      g_autoptr(GByteArray) request = g_byte_array_new ();
      g_byte_array_append (request, chk->data, chk->data_sz);
      if (i == (chunks->len -1)) // the last chunk
      {
        wait_data_reply = TRUE;
        pkg_eop = 0;
      }      
      if (!fu_goodixfp_device_cmd_xfer (self, GX_CMD_UPGRADE, GX_CMD_UPGRADE_DATA,
                                        pkg_eop,
                                        request,
                                        &reponse,
                                        wait_data_reply,
                                        &error_local))
      {
          g_set_error (error,
                FWUPD_ERROR,
                FWUPD_ERROR_WRITE,
                "failed to write: %s",
                error_local->message);
        return FALSE;
      }
      
      /* update progress */
      fu_device_set_progress_full (device, (gsize) i, (gsize) chunks->len);
    } 
  /* success! */
  return TRUE;
}

static void
fu_goodixfp_device_init (FuGoodixFpDevice *self)
{
  /* this is the application code */
  //g_setenv ("FWUPD_GOODIXFP_VERBOSE", "1", TRUE);
  fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_UPDATABLE);
  fu_device_add_flag (FU_DEVICE (self), FWUPD_DEVICE_FLAG_CAN_VERIFY);
  fu_device_set_version_format (FU_DEVICE (self), FWUPD_VERSION_FORMAT_PLAIN);
  fu_device_set_remove_delay (FU_DEVICE (self), 5000);
  fu_device_set_name (FU_DEVICE (self), "Fingerprint Sensor");
  fu_device_set_summary (FU_DEVICE (self), "Match-On-Chip Fingerprint Sensor");
  fu_device_set_vendor (FU_DEVICE (self), "Goodix");
}

static void
fu_goodixfp_device_class_init (FuGoodixFpDeviceClass *klass)
{
  FuDeviceClass *klass_device = FU_DEVICE_CLASS (klass);
  FuUsbDeviceClass *klass_usb_device = FU_USB_DEVICE_CLASS (klass);

  klass_device->write_firmware = fu_goodixfp_device_write_firmware;
  klass_device->setup = fu_goodixfp_device_setup;
  klass_device->attach = fu_goodixfp_device_attach;  
  klass_usb_device->open = fu_goodixfp_device_open;

}
