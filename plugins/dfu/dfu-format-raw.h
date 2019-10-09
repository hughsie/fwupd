/*
 * Copyright (C) 2015-2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>

#include "dfu-firmware.h"

DfuFirmwareFormat	 dfu_firmware_detect_raw	(GBytes		*bytes);
GBytes			*dfu_firmware_to_raw		(DfuFirmware	*firmware,
							GError		**error);
gboolean		 dfu_firmware_from_raw		(DfuFirmware	*firmware,
							GBytes		*bytes,
							DfuFirmwareParseFlags flags,
							GError		**error);
