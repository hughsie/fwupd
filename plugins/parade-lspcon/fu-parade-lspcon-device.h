/*
 * Copyright (C) 2021 Peter Marheine <pmarheine@chromium.org>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fwupdplugin.h>

#define FU_TYPE_PARADE_LSPCON_DEVICE (fu_parade_lspcon_device_get_type ())
G_DECLARE_FINAL_TYPE (FuParadeLspconDevice,
		      fu_parade_lspcon_device, FU,
		      PARADE_LSPCON_DEVICE, FuI2cDevice)
