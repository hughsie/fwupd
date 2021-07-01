/*
 * Copyright (C) 2021 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "fu-kinetic-dp-firmware.h"

#include "fu-kinetic-dp-connection.h"
#include "fu-kinetic-dp-aux-isp.h"
#include "fu-kinetic-dp-secure-aux-isp.h"

struct _FuKineticDpFirmware {
	FuFirmwareClass	parent_instance;

	/* <TODO> Declare as private member */
	guint32		esm_payload_size;
	guint32		arm_app_code_size;
	guint32		app_init_data_size;
	guint32		cmdb_block_size;

	gboolean	is_fw_esm_xip_enabled;
};

G_DEFINE_TYPE (FuKineticDpFirmware, fu_kinetic_dp_firmware, FU_TYPE_FIRMWARE)

#define HEADER_LEN_ISP_DRV_SIZE	4
#define APP_ID_STR_LEN		4

typedef struct {
	KtChipId	chip_id;
	guint32		app_id_offset;
	guint8		app_id_str[APP_ID_STR_LEN];
	guint16		fw_bin_flag;
} KtDpFwAppIdFlag;

/* Application signature/Identifier table */
static const KtDpFwAppIdFlag kt_dp_app_sign_id_table[] =
{
	/* Chip_ID		App ID Offset	App ID			FW Flag									*/
	{KT_CHIP_JAGUAR_5000,	0x0FFFE4UL,	{'J', 'A', 'G', 'R'},	KT_FW_BIN_FLAG_NONE	},	/* Jaguar 1024KB			*/
	{KT_CHIP_JAGUAR_5000,	0x0A7036UL,	{'J', 'A', 'G', 'R'},	KT_FW_BIN_FLAG_NONE	},	/* Jaguar 670KB, for ANZU		*/
	{KT_CHIP_JAGUAR_5000,	0x0FFFE4UL,	{'J', 'A', 'G', 'X'},	KT_FW_BIN_FLAG_XIP	},	/* Jaguar 1024KB (App 640KB)		*/
	{KT_CHIP_JAGUAR_5000,	0x0E7036UL,	{'J', 'A', 'G', 'X'},	KT_FW_BIN_FLAG_XIP	},	/* Jaguar 670KB, for ANZU (App 640KB)	*/
	{KT_CHIP_MUSTANG_5200,	0x0FFFE4UL,	{'M', 'S', 'T', 'G'},	KT_FW_BIN_FLAG_NONE	},	/* Mustang 1024KB			*/
	{KT_CHIP_MUSTANG_5200,	0x0A7036UL,	{'M', 'S', 'T', 'G'},	KT_FW_BIN_FLAG_NONE	},	/* Mustang 670KB, for ANZU		*/
	{KT_CHIP_MUSTANG_5200,	0x0FFFE4UL,	{'M', 'S', 'T', 'X'},	KT_FW_BIN_FLAG_XIP	},	/* Mustang 1024KB (App 640KB)		*/
	{KT_CHIP_MUSTANG_5200,	0x0E7036UL,	{'M', 'S', 'T', 'X'},	KT_FW_BIN_FLAG_XIP	},	/* Mustang 670KB, for ANZU (App 640KB)	*/
};

static guint32
_get_valid_payload_size (const guint8 *payload_data, const guint32 data_size)
{
	guint32 i = 0;

	payload_data += data_size - 1;  /* Start searching from the end of payload */
	while ((*(payload_data - i) == 0xFF) && (i < data_size)) {
		i++;
	}

	return (data_size - i);
}

static gboolean
kt_dp_get_chip_id_from_fw_buf (const guint8 *fw_bin_buf,
			       const guint32 fw_bin_size,
			       KtChipId *chip_id,
			       guint16 *fw_bin_flag)
{
	guint32 num = sizeof (kt_dp_app_sign_id_table) / sizeof (kt_dp_app_sign_id_table[0]);
	guint32 i;

	for (i = 0; i < num; i++) {
		guint32 app_id_offset = kt_dp_app_sign_id_table[i].app_id_offset;

		if ((app_id_offset + APP_ID_STR_LEN) < fw_bin_size) {
			if (0 == memcmp (&fw_bin_buf[app_id_offset], kt_dp_app_sign_id_table[i].app_id_str, APP_ID_STR_LEN)) {
				/* Found corresponding app ID */
				*chip_id = kt_dp_app_sign_id_table[i].chip_id;
				*fw_bin_flag = kt_dp_app_sign_id_table[i].fw_bin_flag;

				return TRUE;
			}
		}
	}

	return FALSE;
}

static gboolean
sec_aux_isp_parse_app_fw (FuKineticDpFirmware *firmware,
			  const guint8 *fw_bin_buf,
			  const guint32 fw_bin_size,
			  const KtChipId chip_id,
			  const guint16 fw_bin_flag,
			  GError **error)
{
	guint32 app_code_block_size = APP_CODE_NORMAL_BLOCK_SIZE;
	guint32 app_init_data_start_addr = SPI_APP_NORMAL_INIT_DATA_START;

	firmware->is_fw_esm_xip_enabled = FALSE;

	if (fw_bin_size != STD_FW_PAYLOAD_SIZE) {
		g_set_error(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "F/W payload size (%u) is not valid",
			    fw_bin_size);
		return FALSE;
	}

	if (fw_bin_flag & KT_FW_BIN_FLAG_XIP) {
		app_code_block_size = APP_CODE_EXTEND_BLOCK_SIZE;
		app_init_data_start_addr = SPI_APP_EXTEND_INIT_DATA_START;
		firmware->is_fw_esm_xip_enabled = TRUE;
	}

#if 0	/* <TODO> Get FW info embedded in FW file */
	fw_app_id = (KtJaguarAppId *)(fw_bin_buf + SPI_APP_ID_DATA_START);

	/* Get Standard F/W version */
	fw_file_info->fw_info.std_fw_ver = (guint32)(fw_app_id->fw_major_ver_num << 16);
	fw_file_info->fw_info.std_fw_ver += (guint32)(fw_app_id->fw_minor_ver_num << 8);
	fw_file_info->fw_info.std_fw_ver += fw_app_id->fw_rev_num;

	/* Get Customer Project ID */
	fw_file_info->fw_info.customer_project_id = fw_bin_buf[CUSTOMER_PROJ_ID_OFFSET];

	/* Get Customer F/W Version */
	memcpy (&fw_file_info->fw_info.customer_fw_ver, &fw_bin_buf[CUSTOMER_FW_VER_OFFSET], CUSTOMER_FW_VER_SIZE);
#endif

	/* Get each block size */
	firmware->esm_payload_size = _get_valid_payload_size (&fw_bin_buf[SPI_ESM_PAYLOAD_START], ESM_PAYLOAD_BLOCK_SIZE);
	firmware->arm_app_code_size = _get_valid_payload_size (&fw_bin_buf[SPI_APP_PAYLOAD_START], app_code_block_size);
	firmware->app_init_data_size = _get_valid_payload_size (&fw_bin_buf[app_init_data_start_addr], APP_INIT_DATA_BLOCK_SIZE);
	firmware->cmdb_block_size = _get_valid_payload_size (&fw_bin_buf[SPI_CMDB_BLOCK_START], CMDB_BLOCK_SIZE);

	return TRUE;
}

guint32
fu_kinetic_dp_firmware_get_esm_payload_size (FuKineticDpFirmware *self)
{
	g_return_val_if_fail (FU_KINETIC_DP_FIRMWARE (self), 0);
	return self->esm_payload_size;
}

guint32
fu_kinetic_dp_firmware_get_arm_app_code_size (FuKineticDpFirmware *self)
{
	g_return_val_if_fail (FU_KINETIC_DP_FIRMWARE (self), 0);
	return self->arm_app_code_size;
}

guint32
fu_kinetic_dp_firmware_get_app_init_data_size (FuKineticDpFirmware *self)
{
	g_return_val_if_fail (FU_KINETIC_DP_FIRMWARE (self), 0);
	return self->app_init_data_size;
}

guint32
fu_kinetic_dp_firmware_get_cmdb_block_size (FuKineticDpFirmware *self)
{
	g_return_val_if_fail (FU_KINETIC_DP_FIRMWARE (self), 0);
	return self->cmdb_block_size;
}

gboolean
fu_kinetic_dp_firmware_get_is_fw_esm_xip_enabled (FuKineticDpFirmware *self)
{
	g_return_val_if_fail (FU_KINETIC_DP_FIRMWARE (self), FALSE);
	return self->is_fw_esm_xip_enabled;
}

static gboolean
fu_kinetic_dp_firmware_parse (FuFirmware *firmware,
			      GBytes *fw,
			      guint64 addr_start,
			      guint64 addr_end,
			      FwupdInstallFlags flags,
			      GError **error)
{
	FuKineticDpFirmware *fw_self = FU_KINETIC_DP_FIRMWARE (firmware);
	const guint8 *buf;
	gsize bufsz;
	guint32 isp_drv_payload_size = 0, app_fw_payload_size = 0;
	g_autoptr (GBytes) isp_drv_payload = NULL;
	g_autoptr (GBytes) app_fw_payload = NULL;
	g_autoptr  (FuFirmware) isp_drv_img = NULL;
	g_autoptr (FuFirmware) app_fw_img = NULL;
	KtChipId chip_id = KT_CHIP_NONE;	/* <TODO> store in class private data */
	guint16 fw_bin_flag = 0;		/* <TODO> store in class private data */

	/* Parse firmware according to Kinetic's FW image format
	 * FW binary = 4 bytes header(Little-Endian) + ISP driver + app FW
	 * 4 bytes: size of ISP driver
	 */
	buf = g_bytes_get_data (fw, &bufsz);
	if (!fu_common_read_uint32_safe(buf, bufsz, 0,
					&isp_drv_payload_size, G_LITTLE_ENDIAN, error)) {
		return FALSE;
	}
	g_debug ("ISP driver payload size: %u bytes", isp_drv_payload_size);

	app_fw_payload_size = g_bytes_get_size (fw) - HEADER_LEN_ISP_DRV_SIZE - isp_drv_payload_size;
	g_debug ("App FW payload size: %u bytes", app_fw_payload_size);

	/* Add ISP driver as a new image into firmware */
	isp_drv_payload = g_bytes_new_from_bytes (fw, HEADER_LEN_ISP_DRV_SIZE, isp_drv_payload_size);
	isp_drv_img = fu_firmware_new_from_bytes (isp_drv_payload);
	fu_firmware_set_idx (isp_drv_img, FU_KT_FW_IMG_IDX_ISP_DRV);

	fu_firmware_add_image (firmware, isp_drv_img);

	/* Add App FW as a new image into firmware */
	app_fw_payload = g_bytes_new_from_bytes (fw, HEADER_LEN_ISP_DRV_SIZE + isp_drv_payload_size, app_fw_payload_size);
	buf = g_bytes_get_data (app_fw_payload, &bufsz);
	if (!kt_dp_get_chip_id_from_fw_buf (buf, bufsz, &chip_id, &fw_bin_flag)) {
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "No valid chip ID is found in the firmware");
		return FALSE;
	}

	if (!sec_aux_isp_parse_app_fw (fw_self, buf, bufsz, chip_id, fw_bin_flag, error)) {
		g_prefix_error (error, "Failed to parse FW info from firmware file: ");
		return FALSE;
	}

	app_fw_img = fu_firmware_new_from_bytes (app_fw_payload);
	fu_firmware_set_idx (app_fw_img, FU_KT_FW_IMG_IDX_APP_FW);

	fu_firmware_add_image (firmware, app_fw_img);

	return TRUE;
}

static void
fu_kinetic_dp_firmware_init (FuKineticDpFirmware *self)
{
	self->esm_payload_size = 0;
	self->arm_app_code_size = 0;
	self->app_init_data_size = 0;
	self->cmdb_block_size = 0;
	self->is_fw_esm_xip_enabled = FALSE;
}

static void
fu_kinetic_dp_firmware_class_init (FuKineticDpFirmwareClass *klass)
{
	FuFirmwareClass *klass_firmware = FU_FIRMWARE_CLASS (klass);
	klass_firmware->parse = fu_kinetic_dp_firmware_parse;
}

FuFirmware *
fu_kinetic_dp_firmware_new (void)
{
	return FU_FIRMWARE (g_object_new (FU_TYPE_KINETIC_DP_FIRMWARE, NULL));
}
