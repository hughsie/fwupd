/*
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include <fwupd.h>

#include "fu-kinetic-dp-aux-isp.h"
#include "fu-kinetic-dp-secure-aux-isp.h"

typedef struct
{
	KtChipId	chip_id;
	KtFwRunState	fw_run_state;
	guint8		id_str[DPCD_SIZE_BRANCH_DEV_ID_STR];
	guint8		str_len;
} KtDpChipBrIdStrTable;

/* Kinetic chip DPCD branch ID string table */
static const KtDpChipBrIdStrTable kt_dp_branch_dev_info_table[] =
{
	/* Jaguar MCDP50x0 */
	{KT_CHIP_JAGUAR_5000,	KT_FW_STATE_RUN_IROM,	{'5', '0', '1', '0', 'I', 'R'},	6},
	{KT_CHIP_JAGUAR_5000,	KT_FW_STATE_RUN_APP,	{'K', 'T', '5', '0', 'X', '0'},	6},
	/* Mustang MCDP52x0 */
	{KT_CHIP_MUSTANG_5200,	KT_FW_STATE_RUN_IROM,	{'5', '2', '1', '0', 'I', 'R'},	6},
	{KT_CHIP_MUSTANG_5200,	KT_FW_STATE_RUN_APP,	{'K', 'T', '5', '2', 'X', '0'},	6},
};

static KtDpDevInfo dp_dev_infos[MAX_DEV_NUM];
static KtChipId dp_root_dev_chip_id = KT_CHIP_NONE;
static KtFwRunState dp_root_dev_state = KT_FW_STATE_RUN_NONE;
static const gchar *kt_dp_fw_run_state_strs[KT_FW_STATE_NUM] = {"iROM",
								"App",
								"Boot-Code",
								"Unknown"};

const gchar *
fu_kinetic_dp_aux_isp_get_chip_id_str (KtChipId chip_id)
{
	if (chip_id == KT_CHIP_JAGUAR_5000)
		return "Jaguar";

	if (chip_id == KT_CHIP_MUSTANG_5200)
		return "Mustang";

	return "";
}

const gchar *
fu_kinetic_dp_aux_isp_get_fw_run_state_str (KtFwRunState fw_run_state)
{
	return (fw_run_state < KT_FW_STATE_NUM) ? kt_dp_fw_run_state_strs[fw_run_state] : NULL;
}

guint16
fu_kinetic_dp_aux_isp_get_numeric_chip_id (KtChipId chip_id)
{
	if (chip_id == KT_CHIP_MUSTANG_5200)
		return 0x5200U;
	if (chip_id == KT_CHIP_JAGUAR_5000)
		return 0x5000U;
	return 0;
}

static gboolean
fu_kinetic_dp_aux_isp_get_dev_info_from_branch_id (const guint8 *br_id_str_buf,
						   const guint8 br_id_str_buf_size,
						   KtDpDevInfo *dev_info,
						   GError **error)
{
	guint32 i = 0;
	guint32 num = sizeof (kt_dp_branch_dev_info_table) / sizeof (kt_dp_branch_dev_info_table[0]);
	g_autofree gchar *str = NULL;   /* Just for printing error log */

	g_return_val_if_fail (br_id_str_buf != NULL, FALSE);
	g_return_val_if_fail (br_id_str_buf_size >= DPCD_SIZE_BRANCH_DEV_ID_STR, FALSE);
	g_return_val_if_fail (dev_info != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	dev_info->chip_id = KT_CHIP_NONE;
	dev_info->fw_run_state = KT_FW_STATE_RUN_NONE;
	memset (dev_info->branch_id_str, 0, DPCD_SIZE_BRANCH_DEV_ID_STR);	/* Clear the buffer to all 0s as DP spec mentioned */

	/* Find the device info by branch ID string */
	for (i = 0; i < num; i++) {
		if (0 == memcmp (br_id_str_buf, kt_dp_branch_dev_info_table[i].id_str, kt_dp_branch_dev_info_table[i].str_len)) {
			/* Found the chip in the table */
			dev_info->chip_id = kt_dp_branch_dev_info_table[i].chip_id;
			dev_info->fw_run_state = kt_dp_branch_dev_info_table[i].fw_run_state;
			if (!fu_memcpy_safe (dev_info->branch_id_str, sizeof(dev_info->branch_id_str), 0x0,	/* dst */
					     br_id_str_buf, br_id_str_buf_size, 0x0,				/* src */
					     DPCD_SIZE_BRANCH_DEV_ID_STR, error))
				return FALSE;

			return TRUE;
		}
	}

	/* There might not always be null-terminated character '\0' in DPCD branch ID string (when length is 6) */
	str = g_strndup ((const gchar *)br_id_str_buf, DPCD_SIZE_BRANCH_DEV_ID_STR);
	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_INTERNAL,
		     "%s is not a Kinetic device",
		     str);

	return FALSE;
}

static gboolean
fu_kinetic_dp_aux_isp_read_chip_id_and_state (FuKineticDpConnection *connection,
					      /*KtDpDevPort dev_port,*/
					      KtDpDevInfo *dev_info,
					      GError **error)
{
	guint8 branch_id[DPCD_SIZE_BRANCH_DEV_ID_STR] = {0};

	/* Detail information is obtained from DPCD branch ID string */
	if (!fu_kinetic_dp_aux_dpcd_read_branch_id_str (connection, branch_id, DPCD_SIZE_BRANCH_DEV_ID_STR, error)) {
		return FALSE;
	}

	if (!fu_kinetic_dp_aux_isp_get_dev_info_from_branch_id (branch_id, DPCD_SIZE_BRANCH_DEV_ID_STR, dev_info, error))
		return FALSE;

	return TRUE;
}

gboolean
fu_kinetic_dp_aux_isp_enable_aux_forward (FuKineticDpConnection *connection,
					  KtChipId root_dev_chip_id,
					  KtFwRunState root_dev_state,
					  KtDpDevPort target_port,
					  GError **error)
{
	if (root_dev_state != KT_FW_STATE_RUN_APP) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Host device [%s %s] doesn't support to enable AUX forwarding!",
			     fu_kinetic_dp_aux_isp_get_chip_id_str (root_dev_chip_id),
			     fu_kinetic_dp_aux_isp_get_fw_run_state_str (root_dev_state));

		return FALSE;
	}

	if (root_dev_chip_id == KT_CHIP_JAGUAR_5000 || root_dev_chip_id == KT_CHIP_MUSTANG_5200) {
		if (!fu_kinetic_dp_secure_aux_isp_enable_aux_forward (connection, target_port, error)) {
			g_prefix_error (error, "Failed to enable AUX forwarding: ");
			return FALSE;
		}

		g_usleep (10 * 1000);	/* Wait 10ms for host to process AUX forwarding command */

		return TRUE;
	}

	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_INTERNAL,
		     "Host device [%s] doesn't support to enable AUX forwarding!",
		     fu_kinetic_dp_aux_isp_get_chip_id_str (root_dev_chip_id));

	return FALSE;
}

gboolean
fu_kinetic_dp_aux_isp_disable_aux_forward (FuKineticDpConnection *connection,
					   KtChipId root_dev_chip_id,
					   KtFwRunState root_dev_state,
					   GError **error)
{
	if (root_dev_state != KT_FW_STATE_RUN_APP) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Host device [%s %s] doesn't support to disable AUX forwarding!",
			     fu_kinetic_dp_aux_isp_get_chip_id_str (root_dev_chip_id),
			     fu_kinetic_dp_aux_isp_get_fw_run_state_str (root_dev_state));

		return FALSE;
	}

	if (root_dev_chip_id == KT_CHIP_JAGUAR_5000 || root_dev_chip_id == KT_CHIP_MUSTANG_5200) {
		g_usleep (5 * 1000);	/* Wait 5ms */
		return fu_kinetic_dp_secure_aux_isp_disable_aux_forward (connection, error);
	}

	g_set_error (error,
		     FWUPD_ERROR,
		     FWUPD_ERROR_INTERNAL,
		     "Host device [%s] doesn't support to disable AUX forwarding!",
		     fu_kinetic_dp_aux_isp_get_chip_id_str (root_dev_chip_id));

	return FALSE;
}

gboolean
fu_kinetic_dp_aux_isp_read_device_info (FuKineticDpDevice *self,
					KtDpDevPort target_port,
					KtDpDevInfo **dev_info,
					GError **error)
{
	g_autoptr (FuKineticDpConnection) connection = NULL;
	KtDpDevInfo dev_info_local;

	dev_info_local.chip_id = KT_CHIP_NONE;
	dev_info_local.chip_type = 0;
	dev_info_local.chip_sn = 0;
	dev_info_local.fw_run_state = KT_FW_STATE_RUN_NONE;
	dev_info_local.fw_info.std_fw_ver = 0;
	dev_info_local.fw_info.std_cmdb_ver = 0;
	dev_info_local.fw_info.cmdb_rev = 0;
	dev_info_local.fw_info.boot_code_ver = 0;
	dev_info_local.fw_info.customer_project_id = 0;
	dev_info_local.fw_info.customer_fw_ver = 0;
	dev_info_local.is_dual_bank_supported = FALSE;
	dev_info_local.flash_bank_idx = BANK_NONE;

	/* Get basic chip information (Chip ID, F/W work state) */
	connection = fu_kinetic_dp_connection_new (fu_udev_device_get_fd (FU_UDEV_DEVICE (self)));

	/* <TODO> AUX-ISP for DFP device
	 *	  Only AUX-ISP for host chip is considered in current implementation
	 */
	if (!fu_kinetic_dp_aux_isp_read_chip_id_and_state (connection, &dev_info_local, error)) {
		g_prefix_error (error, "Failed to read chip ID and state: ");
		return FALSE;
	}

	/* Get more information from each control library */
	if (KT_CHIP_JAGUAR_5000 == dev_info_local.chip_id || KT_CHIP_MUSTANG_5200 == dev_info_local.chip_id) {
		/* <TODO> Make the control a derivable class to support different behaviors and DPCD definitions
		 *        while processing ISP. (if needed)
		 */
		if (!fu_kinetic_dp_secure_aux_isp_get_device_info (connection, &dev_info_local, error)) {
		    g_prefix_error (error, "Failed to read other device information: ");
		    return FALSE;
		}
	} else {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Not supported chip [%u] to do AUX-ISP",
			     dev_info_local.chip_id);
		return FALSE;
	}

	if (dp_root_dev_chip_id == KT_CHIP_NONE) {
		dp_root_dev_chip_id = dev_info_local.chip_id;
		dp_root_dev_state = dev_info_local.fw_run_state;
	}

	/* Store read info to static allocated structure */
	if (!fu_memcpy_safe((guint8 *)&dp_dev_infos[target_port], sizeof (dp_dev_infos[target_port]), 0x0,	/* dst */
			     (guint8 *)&dev_info_local, sizeof(dev_info_local), 0x0,				/* src */
			     sizeof(KtDpDevInfo), error))							/* size */
		return FALSE;

	/* Assign pointer to specified structure to output parameter */
	*dev_info = &dp_dev_infos[target_port];

	return TRUE;
}

void
fu_kinetic_dp_aux_isp_init (void)
{
	dp_root_dev_chip_id = KT_CHIP_NONE;
	dp_root_dev_state = KT_FW_STATE_RUN_NONE;

	/* Init Secure AUX-ISP */
	fu_kinetic_dp_secure_aux_isp_init ();

	/* Init other kind of AUX-ISP protocol here */
}

gboolean
fu_kinetic_dp_aux_isp_start (FuKineticDpDevice *self,
			     FuFirmware *firmware,
			     GError **error)
{
	/* <TODO> Only test ISP for host device now
	 *        AUX-ISP for DFP devices is not implemented yet
	 */
	KtDpDevInfo *dev_info = &dp_dev_infos[DEV_HOST];

	if (KT_CHIP_JAGUAR_5000 == dev_info->chip_id || KT_CHIP_MUSTANG_5200 == dev_info->chip_id) {
		if (!fu_kinetic_dp_secure_aux_isp_update_firmware (self, firmware, dev_info, error)) {
			g_prefix_error (error, "Failed to Secure AUX-ISP: ");
			return FALSE;
		}
	} else {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Not supported to do ISP for this chip [%u]",
			     dev_info->chip_id);
		return FALSE;
	}

	return TRUE;
}
