/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <glib-object.h>
#include <gio/gio.h>
#include <gusb.h>

#include "fu-quirks.h"

#include "dfu-device.h"

void		 dfu_device_error_fixup			(DfuDevice	*device,
							 GError		**error);
guint		 dfu_device_get_download_timeout	(DfuDevice	*device);
gchar		*dfu_device_get_attributes_as_string	(DfuDevice	*device);
gboolean	 dfu_device_ensure_interface		(DfuDevice	*device,
							 GError		**error);
