/*
 * Copyright (C) 2021 Intel Corporation.
 * Copyright (C) 2021 Dell Inc.
 * All rights reserved.
 *
 * This software and associated documentation (if any) is furnished
 * under a license and may only be used or copied in accordance
 * with the terms of the license.
 *
 * This file is provided under a dual MIT/LGPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 * Dell Chooses the MIT license part of Dual MIT/LGPLv2 license agreement.
 *
 * SPDX-License-Identifier: LGPL-2.1+ OR MIT
 */

#pragma once

#include "config.h"

#include <fwupdplugin.h>

#define FU_TYPE_DELL_DOCK_USB4 (fu_dell_dock_usb4_get_type ())
G_DECLARE_FINAL_TYPE (FuDellDockUsb4, fu_dell_dock_usb4, FU, DELL_DOCK_USB4, FuUsbDevice)

FuDellDockUsb4 *fu_dell_dock_usb4_new (FuUsbDevice *device);
