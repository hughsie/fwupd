/*
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib.h>

#include "fu-kinetic-dp-aux-dpcd.h"
#include "fu-kinetic-dp-device.h"
#include "fu-kinetic-dp-connection.h"

typedef enum {
	KT_CHIP_NONE		= 0,
	KT_CHIP_BOBCAT_2800	= 1,
	KT_CHIP_BOBCAT_2850	= 2,
	KT_CHIP_PEGASUS		= 3,
	KT_CHIP_MYSTIQUE	= 4,
	KT_CHIP_DP2VGA		= 5,
	KT_CHIP_PUMA_2900	= 6,
	KT_CHIP_PUMA_2920	= 7,
	KT_CHIP_JAGUAR_5000	= 8,
	KT_CHIP_MUSTANG_5200	= 9,
} KtChipId;

typedef enum {
	KT_FW_STATE_RUN_NONE		= 0,
	KT_FW_STATE_RUN_IROM		= 1,
	KT_FW_STATE_RUN_BOOT_CODE	= 2,
	KT_FW_STATE_RUN_APP		= 3,

	KT_FW_STATE_NUM			= 4
} KtFwRunState;

typedef struct {
	guint32		std_fw_ver;
	guint16		boot_code_ver;
	guint16		std_cmdb_ver;
	guint32		cmdb_rev;
	guint16		customer_fw_ver;
	guint8		customer_project_id;
} KtDpFwInfo;

typedef enum {
	BANK_A		= 0,
	BANK_B		= 1,
	BANK_TOTAL	= 2,

	BANK_NONE	= 0xFF
} KtFlashBankIdx;

typedef struct {
	KtChipId	chip_id;
	guint16		chip_rev;
	guint8		chip_type;
	guint32		chip_sn;
	KtFwRunState	fw_run_state;
	KtDpFwInfo	fw_info;
	guint8		branch_id_str[DPCD_SIZE_BRANCH_DEV_ID_STR];
	gboolean	is_dual_bank_supported;
	KtFlashBankIdx	flash_bank_idx;
} KtDpDevInfo;

typedef enum {
	DEV_HOST	= 0,
	DEV_PORT1	= 1,
	DEV_PORT2	= 2,
	DEV_PORT3	= 3,

	MAX_DEV_NUM	= 4,
	DEV_ALL		= 0xFF
} KtDpDevPort;

const gchar	*fu_kinetic_dp_aux_isp_get_chip_id_str		(KtChipId		chip_id);
const gchar	*fu_kinetic_dp_aux_isp_get_fw_run_state_str	(KtFwRunState		fw_run_state);
guint16		fu_kinetic_dp_aux_isp_get_numeric_chip_id	(KtChipId		chip_id);
void		fu_kinetic_dp_aux_isp_init			(void);
gboolean	fu_kinetic_dp_aux_isp_enable_aux_forward	(FuKineticDpConnection	*connection,
								 KtChipId		root_dev_chip_id,
								 KtFwRunState		root_dev_state,
								 KtDpDevPort		target_port,
								 GError			**error);
gboolean	fu_kinetic_dp_aux_isp_disable_aux_forward	(FuKineticDpConnection	*connection,
								 KtChipId		root_dev_chip_id,
								 KtFwRunState		root_dev_state,
								 GError			**error);
gboolean	fu_kinetic_dp_aux_isp_read_device_info		(FuKineticDpDevice	*self,
								 KtDpDevPort		target_port,
								 KtDpDevInfo		**dev_info,
								 GError			**error);
gboolean	fu_kinetic_dp_aux_isp_start			(FuKineticDpDevice	*self,
								 FuFirmware		*firmware,
								 GError			**error);
