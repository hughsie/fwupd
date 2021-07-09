/*
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "fu-kinetic-dp-secure-aux-isp.h"

#include "fu-kinetic-dp-aux-isp.h"
#include "fu-kinetic-dp-connection.h"
#include "fu-kinetic-dp-aux-dpcd.h"

/* OUI of MegaChips America */
#define MCA_OUI_BYTE_0				0x00
#define MCA_OUI_BYTE_1				0x60
#define MCA_OUI_BYTE_2				0xAD

/* Kinetic proprietary DPCD fields for Jaguar/Mustang, for both application and ISP driver */
#define DPCD_ADDR_FLOAT_CMD_STATUS_REG		0x0050D
#define DPCD_ADDR_FLOAT_PARAM_REG		0x0050E

/* DPCD registers are used while running application */
#define DPCD_ADDR_FLOAT_CUSTOMER_FW_MIN_REV	0x00514
#define DPCD_SIZE_FLOAT_CUSTOMER_FW_MIN_REV	1
#define DPCD_ADDR_FLOAT_CUSTOMER_PROJ_ID	0x00515
#define DPCD_SIZE_FLOAT_CUSTOMER_PROJ_ID	1
#define DPCD_ADDR_FLOAT_PRODUCT_TYPE		0x00516
#define DPCD_SIZE_FLOAT_PRODUCT_TYPE		1

/* DPCD registers are used while running ISP driver */
#define DPCD_ADDR_FLOAT_ISP_REPLY_LEN_REG	0x00513
#define DPCD_SIZE_FLOAT_ISP_REPLY_LEN_REG	1		/* 0x00513			*/

#define DPCD_ADDR_FLOAT_ISP_REPLY_DATA_REG	0x00514		/* While running ISP driver	*/
#define DPCD_SIZE_FLOAT_ISP_REPLY_DATA_REG	12		/* 0x00514 ~ 0x0051F		*/

#define DPCD_ADDR_KT_AUX_WIN			0x80000ul
#define DPCD_SIZE_KT_AUX_WIN			0x8000ul	/* 0x80000ul ~ 0x87FFF, 32 KB	*/
#define DPCD_ADDR_KT_AUX_WIN_END		(DPCD_ADDR_KT_AUX_WIN +  DPCD_SIZE_KT_AUX_WIN - 1)

#define CRC_INIT_KT_PROP_CRC16			0x1021		/* init value for Kinetic's proprietary CRC-16 calculation */
#define CRC_POLY_KT_PROP_CRC16			0x1021		/* polynomial for Kinetic's proprietary CRC-16 calculation */

/* each device should have a corresponding ISP info instance */
static guint32 isp_payload_procd_size;
static guint32 isp_procd_size;
static guint32 isp_total_data_size;

static guint16 read_flash_prog_time;
static guint16 flash_id;
static guint16 flash_size;

static gboolean is_isp_secure_auth_mode;

static gboolean
fu_kinetic_dp_secure_aux_isp_read_param_reg (FuKineticDpConnection *self,
					     guint8 *dpcd_val,
					     GError **error)
{
	if (!fu_kinetic_dp_connection_read (self,
					    DPCD_ADDR_FLOAT_PARAM_REG,
					    dpcd_val, 1, error)) {
		g_prefix_error (error, "failed to read DPCD_KT_PARAM_REG: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_write_kt_prop_cmd (FuKineticDpConnection *connection,
						guint8 cmd_id,
						GError **error)
{
	cmd_id |= DPCD_KT_CONFIRMATION_BIT;

	if (!fu_kinetic_dp_connection_write (connection,
					     DPCD_ADDR_FLOAT_CMD_STATUS_REG,
					     &cmd_id, sizeof(cmd_id),
					     error)) {
		g_prefix_error (error, "failed to write DPCD_KT_CMD_STATUS_REG: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_clear_kt_prop_cmd (FuKineticDpConnection *connection,
						GError **error)
{
	guint8 cmd_id = KT_DPCD_CMD_STS_NONE;

	if (!fu_kinetic_dp_connection_write (connection,
					     DPCD_ADDR_FLOAT_CMD_STATUS_REG,
					     &cmd_id, sizeof(cmd_id),
					     error)) {
		g_prefix_error (error, "failed to write DPCD_KT_CMD_STATUS_REG: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (FuKineticDpConnection *self,
					       guint8 cmd_id,
					       guint32 max_time_ms,
					       guint16 poll_interval_ms,
					       guint8 *status,
					       GError **error)
{
	guint8 dpcd_val = KT_DPCD_CMD_STS_NONE;

	if (!fu_kinetic_dp_secure_aux_isp_write_kt_prop_cmd (self, cmd_id, error))
		return FALSE;

	*status = KT_DPCD_CMD_STS_NONE;

	/* Wait for the sent proprietary command to be processed */
	while (max_time_ms != 0) {
		if (!fu_kinetic_dp_connection_read (self,
						    DPCD_ADDR_FLOAT_CMD_STATUS_REG,
						    (guint8 *) &dpcd_val, 1,
						    error)) {
			return FALSE;
		}

		/* target responded */
		if (dpcd_val != (cmd_id | DPCD_KT_CONFIRMATION_BIT)) {
			if (dpcd_val != cmd_id) {
				*status = dpcd_val & DPCD_KT_COMMAND_MASK;

				if (KT_DPCD_STS_CRC_FAILURE == *status) {
					g_set_error_literal (error,
							     FWUPD_ERROR,
							     FWUPD_ERROR_INTERNAL,
							     "checking CRC of chunk data is failed");
				} else {
					g_set_error (error,
						     FWUPD_ERROR,
						     FWUPD_ERROR_INTERNAL,
						     "invalid replied value in DPCD_KT_CMD_STATUS_REG: 0x%X",
						     *status);
				}
				return FALSE;
			} else {	/* dpcd_val == cmd_id */
				/* confirmation bit is cleared by sink,
				 * means that sent command is processed */
				return TRUE;
			}
		}

		g_usleep (((gulong) poll_interval_ms) * 1000);

		if (max_time_ms > poll_interval_ms)
			max_time_ms -= poll_interval_ms;
		else
			max_time_ms = 0;
	}

	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "waiting DPCD_KT_CMD_STATUS_REG timed-out");
	return FALSE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_read_dpcd_reply_data_reg (FuKineticDpConnection *self,
						       guint8 *buf,
						       const guint8 buf_size,
						       guint8 *read_len,
						       GError **error)
{
	guint8 read_data_len;

	/* set the output to 0 */
	*read_len = 0;

	if (!fu_kinetic_dp_connection_read (self,
					    DPCD_ADDR_FLOAT_ISP_REPLY_LEN_REG,
					    &read_data_len, 1, error)) {
		g_prefix_error (error, "failed to read DPCD_ISP_REPLY_DATA_LEN_REG: ");
		return FALSE;
	}

	if (buf_size < read_data_len) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "buffer size [%u] is not enough to read DPCD_ISP_REPLY_DATA_REG [%u]",
			     buf_size,
			     read_data_len);
		return FALSE;
	}

	if (read_data_len > 0) {
		if (!fu_kinetic_dp_connection_read (self,
						    DPCD_ADDR_FLOAT_ISP_REPLY_DATA_REG,
						    buf, read_data_len, error)) {
			g_prefix_error (error, "failed to read DPCD_ISP_REPLY_DATA_REG: ");
			return FALSE;
		}
		*read_len = read_data_len;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_write_dpcd_reply_data_reg (FuKineticDpConnection *self,
							const guint8 *buf,
							guint8 len,
							GError **error)
{
	gboolean ret = FALSE;
	gboolean res;

	if (len > DPCD_SIZE_FLOAT_ISP_REPLY_DATA_REG) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "length bigger than DPCD_SIZE_FLOAT_ISP_REPLY_DATA_REG [%u]",
			     len);
		return FALSE;
	}

	res = fu_kinetic_dp_connection_write (self,
					      DPCD_ADDR_FLOAT_ISP_REPLY_DATA_REG,
					      buf, len, error);
	if (!res) {
		/* clear reply data length to 0 if failed to write reply data */
		g_prefix_error (error, "failed to write DPCD_KT_REPLY_DATA_REG: ");
		len = 0;
	}

	if (fu_kinetic_dp_connection_write (self,
					    DPCD_ADDR_FLOAT_ISP_REPLY_LEN_REG,
					    &len, DPCD_SIZE_FLOAT_ISP_REPLY_LEN_REG,
					    error) && res) {
		/* both reply data and reply length are written successfully */
		ret = TRUE;
	}

	return ret;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_write_mca_oui (FuKineticDpConnection *connection,
					    GError **error)
{
	guint8 mca_oui[DPCD_SIZE_IEEE_OUI] = {
		MCA_OUI_BYTE_0,
		MCA_OUI_BYTE_1,
		MCA_OUI_BYTE_2
	};
	return fu_kinetic_dp_aux_dpcd_write_oui (connection, mca_oui, error);
}

static gboolean
fu_kinetic_dp_secure_aux_isp_enter_code_loading_mode (FuKineticDpConnection *self,
						      gboolean is_app_mode,
						      const guint32 code_size,
						      GError **error)
{
	guint8 status;

	if (is_app_mode) {
		/* send "DPCD_MCA_CMD_PREPARE_FOR_ISP_MODE" command first to make DPCD 514h ~ 517h writable */
		if (!fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (self,
								    KT_DPCD_CMD_PREPARE_FOR_ISP_MODE,
								    500, 10,
								    &status,
								    error))
			return FALSE;
	}

	/* Update payload size to DPCD reply data reg first */
	if (!fu_kinetic_dp_secure_aux_isp_write_dpcd_reply_data_reg (self,
								     (guint8 *) &code_size,
								     sizeof(code_size),
								     error))
		return FALSE;

	if (!fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (self,
							    KT_DPCD_CMD_ENTER_CODE_LOADING_MODE,
							    500, 10,
							    &status,
							    error))
		return FALSE;

	return TRUE;
}

/**
 * fu_kinetic_dp_secure_aux_isp_crc16:
 * @buf: memory buffer
 * @bufsz: size of @buf
 *
 * Returns the cyclic redundancy check value for the given memory buffer.
 * This is a proprietary implementation only can be used in Kinetic's
 * Secure AUX-ISP protocol
 *
 * Returns: CRC value
 **/
static guint16
fu_kinetic_dp_secure_aux_isp_crc16 (const guint8 *buf, guint16 bufsize)
{
	guint16 crc = CRC_INIT_KT_PROP_CRC16;

	for (guint16 i = 0; i < bufsize; i++) {
		guint16 crc_tmp;
		guint8 data, flag;

		crc_tmp = crc;
		data = buf[i];

		for (guint8 j = 8; j; j--) {
			flag = data ^ (crc_tmp >> 8);
			crc_tmp <<= 1;

			if (flag & 0x80)
				crc_tmp ^= CRC_POLY_KT_PROP_CRC16;

			data <<= 1;
		}

		crc = crc_tmp;
	}

	return crc;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_send_payload (FuKineticDpConnection *connection,
					   const guint8 *payload,
					   const guint32 payload_size,
					   guint32 wait_time_ms,
					   gint32 wait_interval_ms,
					   GError **error)
{
	guint8 *remain_payload = (guint8 *)payload;
	guint32 remain_payload_len = payload_size;
	guint32 chunk_crc16;
	guint32 chunk_len;
	guint32 chunk_remaian_len;
	guint32 chunk_offset;
	guint8 status;

	while (remain_payload_len > 0) {
		chunk_len = (remain_payload_len >= DPCD_SIZE_KT_AUX_WIN) ? DPCD_SIZE_KT_AUX_WIN : remain_payload_len;

		/* send a maximum 32KB chunk of payload to AUX window */
		chunk_remaian_len = chunk_len;
		chunk_offset = 0;

		while (chunk_remaian_len > 0) {
			/* maximum length of each AUX write transaction is 16 bytes */
			guint32 aux_wr_size = (chunk_remaian_len >= 16) ? 16 : chunk_remaian_len;
			if (!fu_kinetic_dp_connection_write (connection,
							     DPCD_ADDR_KT_AUX_WIN + chunk_offset,
							     remain_payload + chunk_offset,
							     aux_wr_size,
							     error)) {
				g_prefix_error (error,
						"failed to AUX write at payload 0x%lX: ",
						(guint64)((remain_payload + chunk_offset) - payload));
				return FALSE;
			}

			chunk_offset += aux_wr_size;
			chunk_remaian_len -= aux_wr_size;
		}

		/* send the CRC16 of current 32KB chunk to DPCD_REPLY_DATA_REG */
		chunk_crc16 = (guint32) fu_kinetic_dp_secure_aux_isp_crc16 (remain_payload, chunk_len);
		if (!fu_kinetic_dp_secure_aux_isp_write_dpcd_reply_data_reg (connection,
									     (guint8 *)&chunk_crc16,
									     sizeof(chunk_crc16),
									     error)) {
			g_prefix_error(error, "failed to send CRC16 to reply data register: ");
			return FALSE;
		}

		/* notify that a chunk of payload has been sent to AUX window */
		if (!fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (connection,
								    KT_DPCD_CMD_CHUNK_DATA_PROCESSED,
								    wait_time_ms,
								    wait_interval_ms,
								    &status,
								    error)) {
			g_prefix_error (error, "target failed to process payload chunk: ");
			return FALSE;
		}

		remain_payload += chunk_len;
		remain_payload_len -= chunk_len;
		isp_procd_size += chunk_len;
		isp_payload_procd_size += chunk_len;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_wait_dpcd_cmd_cleared (FuKineticDpConnection *self,
						    guint16 wait_time_ms,
						    guint16 poll_interval_ms,
						    guint8 *status,
						    GError **error)
{
	guint8 dpcd_val = KT_DPCD_CMD_STS_NONE;

	*status = KT_DPCD_CMD_STS_NONE;

	while (wait_time_ms > 0) {
		if (!fu_kinetic_dp_connection_read (self, DPCD_ADDR_FLOAT_CMD_STATUS_REG,
						     (guint8 *)&dpcd_val, 1, error))
			return FALSE;

		if (dpcd_val == KT_DPCD_CMD_STS_NONE)
			return TRUE;	/* Status is cleared by sink */

		if ((dpcd_val & DPCD_KT_CONFIRMATION_BIT) != DPCD_KT_CONFIRMATION_BIT) {
			/* status is not cleared but confirmation bit is cleared,
			 * it means that target responded with failure */
			*status = dpcd_val;
			return FALSE;
		}

		/* Sleep for polling interval */
		g_usleep (((gulong) poll_interval_ms) * 1000);

		if (wait_time_ms >= poll_interval_ms)
			wait_time_ms -= poll_interval_ms;
		else
			wait_time_ms = 0;
	}

	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Waiting DPCD_ISP_SINK_STATUS_REG timed-out");
	return FALSE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_execute_isp_drv (FuKineticDpConnection *connection, GError **error)
{
	guint8 status;
	guint8 read_len;
	guint8 reply_data[6] = { 0 };

	/* in Jaguar, it takes about 1000 ms to boot up and initialize */

	flash_id = 0;
	flash_size = 0;
	read_flash_prog_time = 10;

	if (!fu_kinetic_dp_secure_aux_isp_write_kt_prop_cmd (connection,
							     KT_DPCD_CMD_EXECUTE_RAM_CODE,
							     error))
		return FALSE;

	if (!fu_kinetic_dp_secure_aux_isp_wait_dpcd_cmd_cleared (connection,
								 1500, 100,
								 &status,
								 error)) {
		if (KT_DPCD_STS_INVALID_IMAGE == status)
			g_prefix_error (error, "invalid ISP driver: ");
		else
			g_prefix_error (error, "failed to execute ISP driver: ");
		return FALSE;
	}

	if (!fu_kinetic_dp_secure_aux_isp_read_param_reg (connection, &status, error))
		return FALSE;

	if (status != KT_DPCD_STS_SECURE_ENABLED &&
	    status != KT_DPCD_STS_SECURE_DISABLED) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "waiting for ISP driver ready failed!");
		return FALSE;
	}

	if (KT_DPCD_STS_SECURE_ENABLED == status) {
		is_isp_secure_auth_mode = TRUE;
	} else {
		is_isp_secure_auth_mode = FALSE;
		isp_total_data_size -= (FW_CERTIFICATE_SIZE * 2 + FW_RSA_SIGNATURE_BLOCK_SIZE * 2);
	}

	if (!fu_kinetic_dp_secure_aux_isp_read_dpcd_reply_data_reg (connection,
								    reply_data,
								    sizeof(reply_data),
								    &read_len,
								    error)) {
		g_prefix_error (error, "failed to read flash ID and size: ");
		return FALSE;
	}

	if (!fu_common_read_uint16_safe (reply_data, sizeof(reply_data), 0,
					 &flash_id, G_BIG_ENDIAN, error))
		return FALSE;
	if (!fu_common_read_uint16_safe (reply_data, sizeof(reply_data), 2,
					 &flash_size, G_BIG_ENDIAN, error))
		return FALSE;
	if (!fu_common_read_uint16_safe (reply_data, sizeof(reply_data), 4,
					 &read_flash_prog_time, G_BIG_ENDIAN, error))
		return FALSE;

	if (read_flash_prog_time == 0)
		read_flash_prog_time = 10;

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_send_isp_drv (FuKineticDpConnection *connection,
					   gboolean is_app_mode,
					   const guint8 *isp_drv_data,
					   guint32 isp_drv_len,
					   GError **error)
{
	g_debug ("sending ISP driver payload... started");

	if (!fu_kinetic_dp_secure_aux_isp_enter_code_loading_mode (connection,
								  is_app_mode,
								  isp_drv_len,
								  error)) {
		g_prefix_error (error, "enabling code-loading mode failed: ");
		return FALSE;
	}

	if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection,
							isp_drv_data,
							isp_drv_len,
							10000, 50,
							error)) {
		g_prefix_error (error, "sending ISP driver payload failed: ");
		return FALSE;
	}

	g_debug ("sending ISP driver payload... done!");
	if (!fu_kinetic_dp_secure_aux_isp_execute_isp_drv (connection, error)) {
		g_prefix_error (error, "ISP driver booting up failed: ");
		return FALSE;
	}

	g_debug ("flash ID: 0x%04X", flash_id);

	if (flash_size) {
		/* one bank size in Jaguar is 1024KB */
		if (flash_size < 2048) {
			g_debug ("flash Size: %d KB, Dual Bank is not supported!",
				 flash_size);
		} else {
			g_debug ("flash Size: %d KB", flash_size);
		}
	} else {
		if (flash_id) {
			g_set_error_literal(error,
					    FWUPD_ERROR,
					    FWUPD_ERROR_INTERNAL,
					    "SPI flash not supported");
			return FALSE;
		}
		g_set_error_literal(error,
				    FWUPD_ERROR,
				    FWUPD_ERROR_INTERNAL,
				    "SPI flash not connected");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_enable_fw_update_mode (FuKineticDpFirmware *firmware,
						    FuKineticDpConnection *connection,
						    GError **error)
{
	guint8 status;
	guint8 pl_size_data[12] = { 0 };

	g_debug ("entering F/W loading mode...");

	/* Send payload size to DPCD_MCA_REPLY_DATA_REG */
	if (!fu_common_write_uint32_safe (pl_size_data, sizeof(pl_size_data), 0,
					  fu_kinetic_dp_firmware_get_esm_payload_size (firmware),
					  G_LITTLE_ENDIAN,
					  error))
		return FALSE;
	if (!fu_common_write_uint32_safe (pl_size_data, sizeof(pl_size_data), 4,
					  fu_kinetic_dp_firmware_get_arm_app_code_size (firmware),
					  G_LITTLE_ENDIAN,
					  error))
		return FALSE;
	if (!fu_common_write_uint16_safe (pl_size_data, sizeof(pl_size_data), 8,
					  fu_kinetic_dp_firmware_get_app_init_data_size (firmware),
					  G_LITTLE_ENDIAN,
					  error))
		return FALSE;
	if (!fu_common_write_uint16_safe (pl_size_data, sizeof(pl_size_data), 10,
					  (fu_kinetic_dp_firmware_get_is_fw_esm_xip_enabled (firmware) ? (1 << 15) : 0) |
					   fu_kinetic_dp_firmware_get_cmdb_block_size (firmware),
					  G_LITTLE_ENDIAN,
					  error))
		return FALSE;

	if (!fu_kinetic_dp_secure_aux_isp_write_dpcd_reply_data_reg (connection, pl_size_data, sizeof(pl_size_data), error)) {
		g_prefix_error (error, "send payload size failed: ");
		return FALSE;
	}

	if (!fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (connection, KT_DPCD_CMD_ENTER_FW_UPDATE_MODE, 200000,
							    500, &status, error)) {
		g_prefix_error (error, "entering F/W update mode failed: ");
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_send_fw_payload (FuKineticDpConnection *connection,
					      FuKineticDpFirmware *firmware,
					      const guint8 *fw_data,
					      guint32 fw_len,
					      GError **error)
{
	guint8 *ptr;

	if (is_isp_secure_auth_mode) {
		/* Send ESM and App Certificates & RSA Signatures */
		g_debug ("sending certificates... started");

		ptr = (guint8 *)fw_data;
		if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection, ptr,
								FW_CERTIFICATE_SIZE * 2 + FW_RSA_SIGNATURE_BLOCK_SIZE * 2,
								10000, 200, error)) {
			g_prefix_error (error, "sending certificates failed: ");
			return FALSE;
		}

		g_debug ("sending certificates... done");
	}

	/* Send ESM code */
	g_debug ("sending ESM... started");

	ptr = (guint8 *)fw_data + SPI_ESM_PAYLOAD_START;
	if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection,
							ptr,
							fu_kinetic_dp_firmware_get_esm_payload_size (firmware),
							 10000, 200, error)) {
		g_prefix_error (error, "sending ESM failed: ");
		return FALSE;
	}

	g_debug ("sending ESM... done");

	/* Send App code */
	g_debug ("sending App... started");

	ptr = (guint8 *)fw_data + SPI_APP_PAYLOAD_START;
	if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection,
							ptr,
							fu_kinetic_dp_firmware_get_arm_app_code_size (firmware),
							10000, 200, error)) {
		g_prefix_error (error, "sending App failed: ");
		return FALSE;
	}

	g_debug ("sending App... done");

	/* Send App initialized data */
	g_debug ("sending App init data... started");

	ptr = (guint8 *)fw_data + (fu_kinetic_dp_firmware_get_is_fw_esm_xip_enabled (firmware) ? SPI_APP_EXTEND_INIT_DATA_START : SPI_APP_NORMAL_INIT_DATA_START);
	if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection, ptr,
							fu_kinetic_dp_firmware_get_app_init_data_size (firmware),
							10000, 200, error)) {
		g_prefix_error (error, "sending App init data failed: ");
		return FALSE;
	}

	g_debug ("sending App init data... done");

	if (fu_kinetic_dp_firmware_get_cmdb_block_size (firmware)) {
		/* Send CMDB */
		g_debug ("sending CMDB... started");

		ptr = (guint8 *)fw_data + SPI_CMDB_BLOCK_START;
		if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection,
								ptr,
								fu_kinetic_dp_firmware_get_cmdb_block_size (firmware),
								10000, 200, error)) {
			g_prefix_error (error, "sending CMDB failed: ");
			return FALSE;
		}

		g_debug ("sending CMDB... done");
	}

	/* Send Application Identifier */
	ptr = (guint8 *)fw_data + SPI_APP_ID_DATA_START;
	if (!fu_kinetic_dp_secure_aux_isp_send_payload (connection, ptr, STD_APP_ID_SIZE, 10000, 200, error)) {
		g_prefix_error (error, "sending App ID data failed: ");
		return FALSE;
	}

	g_debug ("sending App ID data... done");
	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_install_fw_images (FuKineticDpConnection *connection,
						GError **error)
{
	guint8 cmd_id = KT_DPCD_CMD_INSTALL_IMAGES;
	guint8 status;
	guint16 wait_count = 1500;
	guint16 progress_inc = FLASH_PROGRAM_COUNT / ((read_flash_prog_time * 1000) / WAIT_PROG_INTERVAL_MS);

	if (!fu_kinetic_dp_secure_aux_isp_write_kt_prop_cmd (connection, cmd_id, error)) {
		g_prefix_error (error, "sending DPCD command failed: ");
		return FALSE;
	}

	while (wait_count-- > 0) {
		if (!fu_kinetic_dp_connection_read (connection,
						    DPCD_ADDR_FLOAT_CMD_STATUS_REG,
						    &status, 1, error)) {
			g_prefix_error(error, "reading DPCD_MCA_CMD_REG failed: ");
			return FALSE;
		}

		/* target responded */
		if (status != (cmd_id | DPCD_KT_CONFIRMATION_BIT)) {
			/* confirmation bit is cleared */
			if (status == cmd_id) {
				isp_payload_procd_size += (isp_total_data_size - isp_procd_size);
				g_debug ("programming F/W payload... done");
				return TRUE;
			} else {
				g_set_error_literal (error,
						     FWUPD_ERROR,
						     FWUPD_ERROR_INTERNAL,
						     "installing images failed");
				return FALSE;
			}
		}

		if (isp_procd_size < isp_total_data_size) {
			isp_procd_size += progress_inc;
			isp_payload_procd_size += progress_inc;
		}

		/* Wait 50ms */
		g_usleep (50 * 1000);
	}

	g_set_error_literal(error,
			    FWUPD_ERROR,
			    FWUPD_ERROR_INTERNAL,
			    "Installing images timed-out");

	return FALSE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_send_reset_command (FuKineticDpConnection *connection,
						 GError **error)
{
	if (!fu_kinetic_dp_secure_aux_isp_write_kt_prop_cmd (connection,
							     KT_DPCD_CMD_RESET_SYSTEM,
							     error)) {
		g_prefix_error (error, "resetting system failed: ");
		return FALSE;
	}
	return TRUE;
}

static KtFlashBankIdx
fu_kinetic_dp_secure_aux_isp_get_flash_bank_idx (FuKineticDpConnection *connection,
						 GError **error)
{
	guint8 status;
	guint8 prev_src_oui[DPCD_SIZE_IEEE_OUI] = { 0 };
	guint8 res = BANK_NONE;

	if (!fu_kinetic_dp_aux_dpcd_read_oui (connection,
					      prev_src_oui, sizeof(prev_src_oui),
					      error))
		return BANK_NONE;

	if (!fu_kinetic_dp_secure_aux_isp_write_mca_oui (connection, error))
		return BANK_NONE;

	if (fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (connection,
							   KT_DPCD_CMD_GET_ACTIVE_FLASH_BANK,
							   100, 20, &status, error) &&
	    !fu_kinetic_dp_secure_aux_isp_read_param_reg (connection, &res, error))
		res = BANK_NONE;

	fu_kinetic_dp_secure_aux_isp_clear_kt_prop_cmd (connection, error);

	/* Restore previous source OUI */
	fu_kinetic_dp_aux_dpcd_write_oui (connection, prev_src_oui, error);

	return (KtFlashBankIdx)res;
}

gboolean
fu_kinetic_dp_secure_aux_isp_enable_aux_forward (FuKineticDpConnection *connection,
						 KtDpDevPort target_port,
						 GError **error)
{
	gboolean ret;
	guint8 status;
	guint8 cmd_id;

	if (!fu_kinetic_dp_secure_aux_isp_write_mca_oui (connection, error))
		return FALSE;

	cmd_id = (guint8)target_port;
	if (!fu_kinetic_dp_connection_write (connection,
					     DPCD_ADDR_FLOAT_PARAM_REG,
					     &cmd_id, sizeof(cmd_id),
					     error))
		return FALSE;

	ret = fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (connection,
							     KT_DPCD_CMD_ENABLE_AUX_FORWARD,
							     1000, 20,
							     &status,
							     error);

	/* Clear CMD_STATUS_REG */
	cmd_id = KT_DPCD_CMD_STS_NONE;
	fu_kinetic_dp_connection_write (connection,
					DPCD_ADDR_FLOAT_CMD_STATUS_REG,
					&cmd_id, sizeof(cmd_id),
					error);

	return ret;
}

gboolean
fu_kinetic_dp_secure_aux_isp_disable_aux_forward (FuKineticDpConnection *connection,
						  GError **error)
{
	gboolean ret;
	guint8 status;
	guint8 cmd_id;

	if (!fu_kinetic_dp_secure_aux_isp_write_mca_oui (connection, error))
		return FALSE;

	ret = fu_kinetic_dp_secure_aux_isp_send_kt_prop_cmd (connection,
							     KT_DPCD_CMD_DISABLE_AUX_FORWARD,
							     1000, 20,
							     &status,
							     error);

	/* clear CMD_STATUS_REG */
	cmd_id = KT_DPCD_CMD_STS_NONE;
	fu_kinetic_dp_connection_write (connection,
					DPCD_ADDR_FLOAT_CMD_STATUS_REG,
					&cmd_id, sizeof(cmd_id),
					error);

	return ret;
}

gboolean
fu_kinetic_dp_secure_aux_isp_get_device_info (FuKineticDpConnection *connection,
					      KtDpDevInfo *dev_info,
					      GError **error)
{
	guint8 dpcd_buf[16] = { 0 };

	/* chip ID, FW work state, and branch ID string are known */

	if (!fu_kinetic_dp_connection_read (connection, DPCD_ADDR_BRANCH_HW_REV, dpcd_buf,
					     sizeof(dpcd_buf), error))
		return FALSE;

	dev_info->chip_rev = dpcd_buf[0];									/* DPCD 0x509		*/
	dev_info->fw_info.std_fw_ver = (guint32)dpcd_buf[1] << 16 | (guint32)dpcd_buf[2] << 8 | dpcd_buf[3];	/* DPCD 0x50A ~ 0x50C	*/
	dev_info->fw_info.customer_project_id = dpcd_buf[12];							/* DPCD 0x515		*/
	dev_info->fw_info.customer_fw_ver = (guint16)dpcd_buf[6] << 8 | (guint16)dpcd_buf[11];			/* DPCD (0x50F | 0x514)	*/
	dev_info->chip_type = dpcd_buf[13];									/* DPCD 0x516		*/

	if (KT_FW_STATE_RUN_APP == dev_info->fw_run_state) {
		dev_info->is_dual_bank_supported = TRUE;
		dev_info->flash_bank_idx = fu_kinetic_dp_secure_aux_isp_get_flash_bank_idx (connection, error);
	}

	dev_info->fw_info.boot_code_ver = 0;
	/* TODO: Add function to read CMDB information */
	dev_info->fw_info.std_cmdb_ver = 0;
	dev_info->fw_info.cmdb_rev = 0;

	return TRUE;
}

static gboolean
fu_kinetic_dp_secure_aux_isp_start_isp (FuKineticDpDevice *self,
					FuFirmware *firmware,
					const KtDpDevInfo *dev_info,
					GError **error)
{
	FuKineticDpFirmware *firmware_self = FU_KINETIC_DP_FIRMWARE (firmware);
	gboolean ret = FALSE;
	gboolean is_app_mode = (KT_FW_STATE_RUN_APP == dev_info->fw_run_state) ? TRUE : FALSE;
	const guint8 *payload_data;
	gsize payload_len;
	g_autoptr(FuDeviceLocker) locker = NULL;
	g_autoptr(FuFirmware) img = NULL;
	g_autoptr(FuKineticDpConnection) connection = NULL;
	g_autoptr(GBytes) app = NULL;
	g_autoptr(GBytes) isp_drv = NULL;

	connection = fu_kinetic_dp_connection_new (fu_udev_device_get_fd (FU_UDEV_DEVICE (self)));

	isp_procd_size = 0;

	g_debug ("start secure AUX-ISP [%s]...", fu_kinetic_dp_aux_isp_get_chip_id_str (dev_info->chip_id));

	/* write MCA OUI */
	if (!fu_kinetic_dp_secure_aux_isp_write_mca_oui (connection, error))
		goto SECURE_AUX_ISP_END;

	/* send ISP driver and execute it */
	img = fu_firmware_get_image_by_idx (firmware, FU_KT_FW_IMG_IDX_ISP_DRV, error);
	if (NULL == img)
		return FALSE;

	isp_drv = fu_firmware_write (img, error);
	if (isp_drv == NULL)
		return FALSE;

	payload_data = g_bytes_get_data (isp_drv, &payload_len);
	if (payload_len > 0) {
		if (!fu_kinetic_dp_secure_aux_isp_send_isp_drv (connection,
								is_app_mode,
								payload_data,
								payload_len,
								error))
			goto SECURE_AUX_ISP_END;
	}

	/* enable FW update mode */
	if (!fu_kinetic_dp_secure_aux_isp_enable_fw_update_mode (firmware_self,
								 connection,
								 error))
		goto SECURE_AUX_ISP_END;

	/* send App FW image */
	img = fu_firmware_get_image_by_idx (firmware, FU_KT_FW_IMG_IDX_APP_FW, error);
	if (NULL == img)
		return FALSE;

	app = fu_firmware_write (img, error);
	if (app == NULL)
		return FALSE;

	payload_data = g_bytes_get_data (app, &payload_len);
	if (!fu_kinetic_dp_secure_aux_isp_send_fw_payload (connection,
							   firmware_self,
							   payload_data,
							   payload_len,
							   error))
		goto SECURE_AUX_ISP_END;

	/* install FW images */
	ret = fu_kinetic_dp_secure_aux_isp_install_fw_images (connection, error);

SECURE_AUX_ISP_END:
	/* send reset command */
	fu_kinetic_dp_secure_aux_isp_send_reset_command (connection, error);

	return ret;
}

gboolean
fu_kinetic_dp_secure_aux_isp_update_firmware (FuKineticDpDevice *self,
					      FuFirmware *firmware,
					      const KtDpDevInfo *dev_info,
					      GError **error)
{
	return fu_kinetic_dp_secure_aux_isp_start_isp (self,
						       firmware,
						       dev_info,
						       error);
}

void
fu_kinetic_dp_secure_aux_isp_init (void)
{
	read_flash_prog_time = 10;
	flash_id = 0;
	flash_size = 0;

	isp_payload_procd_size = 0;
	isp_procd_size = 0;
	isp_total_data_size = 0;

	is_isp_secure_auth_mode = TRUE;
}
