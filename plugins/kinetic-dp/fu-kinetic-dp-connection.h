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
#include <gio/gio.h>

#define FU_TYPE_KINETIC_DP_CONNECTION (fu_kinetic_dp_connection_get_type ())
G_DECLARE_FINAL_TYPE (FuKineticDpConnection, fu_kinetic_dp_connection, FU, KINETIC_DP_CONNECTION, GObject)

FuKineticDpConnection	*fu_kinetic_dp_connection_new	(gint			fd);


gboolean		fu_kinetic_dp_connection_read	(FuKineticDpConnection	*self,
							 guint32		offset,
							 guint8			*buf,
							 guint32		length,
							 GError			**error);
gboolean		fu_kinetic_dp_connection_write	(FuKineticDpConnection	*self,
							 guint32		offset,
							 const guint8		*buf,
							 guint32		length,
							 GError			**error);
