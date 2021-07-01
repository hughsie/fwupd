/*
 * Copyright (C) 2016 Mario Limonciello <mario.limonciello@dell.com>
 * Copyright (C) 2017 Peichen Huang <peichenhuang@tw.synaptics.com>
 * Copyright (C) 2019 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib.h>

#include "fu-kinetic-dp-aux-isp.h"

#define KINETIC_FLASH_MODE_DELAY	3	/* seconds */

/**
 * FuKineticDpMode:
 * @FU_KINETIC_DP_MODE_UNKNOWN:    Type invalid or not known
 * @FU_KINETIC_DP_MODE_DIRECT:     Directly addressable
 * @FU_KINETIC_DP_MODE_REMOTE:     Requires remote register work
 *
 * The device type.
 **/
typedef enum {
	FU_KINETIC_DP_MODE_UNKNOWN,
	FU_KINETIC_DP_MODE_DIRECT,
	FU_KINETIC_DP_MODE_REMOTE,
	/*< private >*/
	FU_KINETIC_DP_MODE_LAST
} FuKineticDpMode;

/**
 * FuKineticDpFamily:
 * @FU_KINETIC_DP_FAMILY_UNKNOWN:  Family invalid or not known
 * @FU_KINETIC_DP_FAMILY_MUSTANG:  Mustang
 * @FU_KINETIC_DP_FAMILY_JAGUAR:   Jaguar
 *
 * The chip family.
 **/
typedef enum {
	FU_KINETIC_DP_FAMILY_UNKNOWN,

	FU_KINETIC_DP_FAMILY_MUSTANG,
	FU_KINETIC_DP_FAMILY_JAGUAR,
	/*<private >*/
	FU_KINETIC_DP_FAMILY_LAST
} FuKineticDpFamily;

const gchar		*fu_kinetic_dp_mode_to_string		(FuKineticDpMode	mode);
const gchar		*fu_kinetic_dp_family_to_string		(FuKineticDpFamily	family);
FuKineticDpFamily	fu_kinetic_dp_chip_id_to_family		(KtChipId		chip_id);
