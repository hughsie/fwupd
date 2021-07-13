/*
 * Copyright (C) 2021 Twain Byrnes <binarynewts@google.com>
 * Copyright (C) 2021 George Popoola <gpopoola@google.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <fwupdplugin.h>

void
fu_plugin_init (FuPlugin *plugin)
{
        fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
}

gboolean
fu_plugin_startup (FuPlugin *plugin, GError **error)
{
        return TRUE;
}

gboolean
fu_plugin_update_prepare (FuPlugin *plugin,
                          FwupdInstallFlags flags,
                          FuDevice *device,
                          GError **error)
{
        return TRUE;
}
