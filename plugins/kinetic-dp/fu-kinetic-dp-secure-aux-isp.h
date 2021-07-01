/*
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib-object.h>

#include "fu-kinetic-dp-device.h"
#include "fu-kinetic-dp-firmware.h"
#include "fu-kinetic-dp-aux-isp.h"

#define DPCD_KT_CONFIRMATION_BIT	(0x80)
#define DPCD_KT_COMMAND_MASK		(0x7F)

#define SIZE_1KB			(1   * 1024)
#define SIZE_4KB			(4   * 1024)
#define SIZE_8KB			(8   * 1024)
#define SIZE_16KB			(16  * 1024)
#define SIZE_24KB			(24  * 1024)
#define SIZE_32KB			(32  * 1024)
#define SIZE_248KB			(248 * 1024)
#define SIZE_256KB			(256 * 1024)
#define SIZE_128KB			(128 * 1024)
#define SIZE_144KB			(144 * 1024)
#define SIZE_240KB			(240 * 1024)
#define SIZE_360KB			(360 * 1024)
#define SIZE_384KB			(384 * 1024)
#define SIZE_512KB			(512 * 1024)
#define SIZE_640KB			(640 * 1024)
#define SIZE_1MB			(1024 * 1024)

/* Flash Memory Map */
#define STD_FW_PAYLOAD_SIZE		SIZE_1MB
#define STD_APP_ID_SIZE			32
#define STD_FW_SIGNATURE_OFFSET		(STD_FW_PAYLOAD_SIZE - STD_APP_ID_SIZE + 4)	/* 0xFFFE4 */
#define STD_FW_VER_OFFSET		(STD_FW_PAYLOAD_SIZE - STD_APP_ID_SIZE + 12)	/* 0xFFFEC */
#define STD_FW_VER_SIZE			3
#define CUSTOMER_PROJ_ID_OFFSET		(STD_FW_PAYLOAD_SIZE - STD_APP_ID_SIZE + 15)	/* 0xFFFEF */
#define CUSTOMER_FW_VER_OFFSET		(STD_FW_PAYLOAD_SIZE - STD_APP_ID_SIZE + 16)	/* 0xFFFF0 */
#define CUSTOMER_FW_VER_SIZE		2

#define FW_CERTIFICATE_SIZE		SIZE_1KB
#define FW_RSA_SIGNATURE_SIZE		256
#define FW_RSA_SIGNATURE_BLOCK_SIZE	SIZE_1KB
#define ESM_PAYLOAD_BLOCK_SIZE		SIZE_256KB
#define APP_CODE_NORMAL_BLOCK_SIZE	SIZE_384KB
#define APP_CODE_EXTEND_BLOCK_SIZE	SIZE_640KB
#define APP_INIT_DATA_BLOCK_SIZE	SIZE_24KB
#define CMDB_BLOCK_SIZE			SIZE_4KB

#define SPI_ESM_CERTIFICATE_START	0
#define SPI_APP_CERTIFICATE_START	(SPI_ESM_CERTIFICATE_START + FW_CERTIFICATE_SIZE)		/* 0x00400 */
#define SPI_ESM_RSA_SIGNATURE_START	(SPI_APP_CERTIFICATE_START + FW_CERTIFICATE_SIZE)		/* 0x00800 */
#define SPI_APP_RSA_SIGNATURE_START	(SPI_ESM_RSA_SIGNATURE_START + FW_RSA_SIGNATURE_BLOCK_SIZE)	/* 0x00C00 */
#define SPI_ESM_PAYLOAD_START		(SPI_APP_RSA_SIGNATURE_START + FW_RSA_SIGNATURE_BLOCK_SIZE)	/* 0x01000 */
#define SPI_APP_PAYLOAD_START		(SPI_ESM_PAYLOAD_START + ESM_PAYLOAD_BLOCK_SIZE)		/* 0x41000 */
#define SPI_APP_NORMAL_INIT_DATA_START	(SPI_APP_PAYLOAD_START + APP_CODE_NORMAL_BLOCK_SIZE)		/* 0xA1000 */
#define SPI_APP_EXTEND_INIT_DATA_START	(SPI_APP_PAYLOAD_START + APP_CODE_EXTEND_BLOCK_SIZE)		/* 0xE1000 */
#define SPI_CMDB_BLOCK_START		0xFE000UL
#define SPI_APP_ID_DATA_START		(STD_FW_PAYLOAD_SIZE - STD_APP_ID_SIZE)

#define FLASH_PROGRAM_COUNT		100000
#define WAIT_PROG_INTERVAL_MS		500

typedef enum {
	/* Status */
	KT_DPCD_CMD_STS_NONE			= 0x0,
	KT_DPCD_STS_INVALID_INFO		= 0x01,
	KT_DPCD_STS_CRC_FAILURE			= 0x02,
	KT_DPCD_STS_INVALID_IMAGE		= 0x03,
	KT_DPCD_STS_SECURE_ENABLED		= 0x04,
	KT_DPCD_STS_SECURE_DISABLED		= 0x05,
	KT_DPCD_STS_SPI_FLASH_FAILURE		= 0x06,

	/* Command */
	KT_DPCD_CMD_PREPARE_FOR_ISP_MODE	= 0x23,
	KT_DPCD_CMD_ENTER_CODE_LOADING_MODE	= 0x24,
	KT_DPCD_CMD_EXECUTE_RAM_CODE		= 0x25,
	KT_DPCD_CMD_ENTER_FW_UPDATE_MODE	= 0x26,
	KT_DPCD_CMD_CHUNK_DATA_PROCESSED	= 0x27,
	KT_DPCD_CMD_INSTALL_IMAGES		= 0x28,
	KT_DPCD_CMD_RESET_SYSTEM		= 0x29,

	/* Other command */
	KT_DPCD_CMD_ENABLE_AUX_FORWARD		= 0x31,
	KT_DPCD_CMD_DISABLE_AUX_FORWARD		= 0x32,
	KT_DPCD_CMD_GET_ACTIVE_FLASH_BANK	= 0x33,

	/* 0x70 ~ 0x7F are reserved for other usage */
	KT_DPCD_CMD_RESERVED			= 0x7F,
} KtSecureAuxIspCmdAndStatus;

typedef enum {
	KT_FW_BIN_FLAG_NONE			= 0,
	KT_FW_BIN_FLAG_XIP			= 1,
} KtFwBinFlag;

typedef struct {
	guint32	app_id_struct_ver;
	guint8	app_id[4];
	guint32	app_ver_id;
	guint8	fw_major_ver_num;
	guint8	fw_minor_ver_num;
	guint8	fw_rev_num;
	guint8	customer_fw_project_id;
	guint8	customer_fw_major_ver_num;
	guint8	customer_fw_minor_ver_num;
	guint8	chip_rev;
	guint8	is_fpga_enabled;
	guint8	reserved[12];
} KtJaguarAppId;

void		fu_kinetic_dp_secure_aux_isp_init			(void);
gboolean	fu_kinetic_dp_secure_aux_isp_get_device_info		(FuKineticDpConnection	*connection,
									 KtDpDevInfo		*dev_info,
									 GError			**error);
gboolean	fu_kinetic_dp_secure_aux_isp_enable_aux_forward		(FuKineticDpConnection	*connection,
									 KtDpDevPort		target_port,
									 GError			**error);
gboolean	fu_kinetic_dp_secure_aux_isp_disable_aux_forward	(FuKineticDpConnection	*connection,
									 GError			**error);
gboolean	fu_kinetic_dp_secure_aux_isp_update_firmware		(FuKineticDpDevice	*self,
									 FuFirmware		*firmware,
									 const KtDpDevInfo	*dev_info,
									 GError			**error);
