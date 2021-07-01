/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_KINETIC_DP_DEVICE (fu_kinetic_dp_device_get_type ())
G_DECLARE_FINAL_TYPE (FuKineticDpDevice, fu_kinetic_dp_device, FU, KINETIC_DP_DEVICE, FuUdevDevice)

FuKineticDpDevice	*fu_kinetic_dp_device_new		(FuUdevDevice		*device);

void			fu_kinetic_dp_device_set_system_type	(FuKineticDpDevice	*self,
								 const gchar		*system_type);
