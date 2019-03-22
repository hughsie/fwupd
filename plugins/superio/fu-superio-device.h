/*
 * Copyright (C) 2018 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include "fu-plugin.h"

G_BEGIN_DECLS

#define FU_TYPE_SUPERIO_DEVICE (fu_superio_device_get_type ())
G_DECLARE_DERIVABLE_TYPE (FuSuperioDevice, fu_superio_device, FU, SUPERIO_DEVICE, FuDevice)

struct _FuSuperioDeviceClass
{
	FuDeviceClass		parent_class;
};

gboolean	 fu_superio_device_regval	(FuSuperioDevice	*self,
						 guint8			 addr,
						 guint8			*data,
						 GError			**error);
gboolean	 fu_superio_device_regval16	(FuSuperioDevice	*self,
						 guint8			 addr,
						 guint16		*data,
						 GError			**error);
gboolean	 fu_superio_device_regwrite	(FuSuperioDevice	*self,
						 guint8			 addr,
						 guint8			 data,
						 GError			**error);

G_END_DECLS
