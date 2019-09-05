/*
 * Copyright (C) 2019 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include "fu-plugin.h"

G_BEGIN_DECLS

#define FU_TYPE_FASTBOOT_DEVICE (fu_thelio_io_device_get_type ())
G_DECLARE_FINAL_TYPE (FuThelioIoDevice, fu_thelio_io_device, FU, FASTBOOT_DEVICE, FuUsbDevice)

FuThelioIoDevice	*fu_thelio_io_device_new	(FuUsbDevice	*device);

G_END_DECLS
