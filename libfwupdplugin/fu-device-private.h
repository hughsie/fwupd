/*
 * Copyright (C) 2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#pragma once

#include <fu-device.h>
#include <xmlb.h>

#define fu_device_set_plugin(d,v)		fwupd_device_set_plugin(FWUPD_DEVICE(d),v)

const gchar	*fu_device_internal_flag_to_string	(FuDeviceInternalFlags flag);
FuDeviceInternalFlags fu_device_internal_flag_from_string (const gchar	*flag);

GPtrArray	*fu_device_get_parent_guids		(FuDevice	*self);
gboolean	 fu_device_has_parent_guid		(FuDevice	*self,
							 const gchar	*guid);
void		 fu_device_set_parent			(FuDevice	*self,
							 FuDevice	*parent);
gint		 fu_device_get_order			(FuDevice	*self);
void		 fu_device_set_order			(FuDevice	*self,
							 gint		 order);
void		 fu_device_set_alternate		(FuDevice	*self,
							 FuDevice	*alternate);
gboolean	 fu_device_ensure_id			(FuDevice	*self,
							 GError		**error)
							 G_GNUC_WARN_UNUSED_RESULT;
void		 fu_device_incorporate_from_component	(FuDevice	*device,
							 XbNode		*component);
gchar		*fu_device_get_guids_as_str		(FuDevice	*self);
GPtrArray	*fu_device_get_possible_plugins		(FuDevice	*self);
void		 fu_device_add_possible_plugin		(FuDevice	*self,
							 const gchar	*plugin);
