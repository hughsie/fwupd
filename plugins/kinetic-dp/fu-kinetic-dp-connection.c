/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2016 Mario Limonciello <mario.limonciello@dell.com>
 * Copyright (C) 2017 Peichen Huang <peichenhuang@tw.synaptics.com>
 * Copyright (C) 2021 Jeffrey Lin <jlin@kinet-ic.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include "fu-kinetic-dp-connection.h"
#include "fu-kinetic-dp-common.h"

#define UNIT_SIZE	32
#define MAX_WAIT_TIME	3	/* unit : second */

struct _FuKineticDpConnection {
	GObject		parent_instance;
	gint		fd;        /* not owned by the connection */
};

G_DEFINE_TYPE (FuKineticDpConnection, fu_kinetic_dp_connection, G_TYPE_OBJECT)

static void
fu_kinetic_dp_connection_init (FuKineticDpConnection *self)
{
}

static void
fu_kinetic_dp_connection_class_init (FuKineticDpConnectionClass *klass)
{
}

static gboolean
fu_kinetic_dp_connection_aux_node_read (FuKineticDpConnection *self,
					guint32 offset,
					guint8 *buf,
					gint length,
					GError **error)
{
	if (lseek (self->fd, offset, SEEK_SET) != offset)
	{
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "Failed to lseek to 0x%x",
			     offset);
		return FALSE;
	}

	if (read (self->fd, buf, length) != length)
	{
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "Failed to read 0x%x bytes",
			     (guint) length);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_connection_aux_node_write (FuKineticDpConnection *self,
					 guint32 offset,
					 const guint8 *buf,
					 gint length,
					 GError **error)
{
	if (lseek (self->fd, offset, SEEK_SET) != offset)
	{
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "Failed to lseek to 0x%x",
			     offset);
		return FALSE;
	}

	if (write (self->fd, buf, length) != length)
	{
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "Failed to write 0x%x bytes",
			    (guint) length);
		return FALSE;
	}

	return TRUE;
}

static gboolean
fu_kinetic_dp_connection_bus_read (FuKineticDpConnection *self,
				   guint32 offset,
				   guint8 *buf,
				   guint32 length,
				   GError **error)
{
	return fu_kinetic_dp_connection_aux_node_read (self, offset, buf, length, error);
}

static gboolean
fu_kinetic_dp_connection_bus_write (FuKineticDpConnection *self,
				    guint32 offset,
				    const guint8 *buf,
				    guint32 length,
				    GError **error)
{
	return fu_kinetic_dp_connection_aux_node_write (self, offset, buf, length, error);
}

FuKineticDpConnection *
fu_kinetic_dp_connection_new (gint fd)
{
	FuKineticDpConnection *self = g_object_new (FU_TYPE_KINETIC_DP_CONNECTION, NULL);

	self->fd = fd;

	return self;
}

gboolean
fu_kinetic_dp_connection_read (FuKineticDpConnection *self,
			       guint32 offset,
			       guint8 *buf,
			       guint32 length,
			       GError **error)
{
	return fu_kinetic_dp_connection_bus_read (self, offset, buf, length, error);
}

gboolean
fu_kinetic_dp_connection_write (FuKineticDpConnection *self,
				guint32 offset,
				const guint8 *buf,
				guint32 length,
				GError **error)
{
	return fu_kinetic_dp_connection_bus_write (self, offset, buf, length, error);
}
