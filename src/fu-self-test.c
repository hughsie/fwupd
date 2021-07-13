/*
 * Copyright (C) 2015 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <xmlb.h>
#include <fwupdplugin.h>
#include <glib-object.h>
#include <glib/gstdio.h>
#include <libgcab.h>
#include <stdlib.h>
#include <string.h>

#include "fu-config.h"
#include "fu-context-private.h"
#include "fu-device-list.h"
#include "fu-device-private.h"
#include "fu-engine.h"
#include "fu-history.h"
#include "fu-install-task.h"
#include "fu-plugin-private.h"
#include "fu-plugin-list.h"
#include "fu-progressbar.h"
#include "fu-hash.h"
#include "fu-security-attr.h"
#include "fu-smbios-private.h"

typedef struct {
	FuPlugin	*plugin;
} FuTest;

static GMainLoop *_test_loop = NULL;
static guint _test_loop_timeout_id = 0;

static gboolean
fu_test_hang_check_cb (gpointer user_data)
{
	g_main_loop_quit (_test_loop);
	_test_loop_timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void
fu_test_loop_run_with_timeout (guint timeout_ms)
{
	g_assert (_test_loop_timeout_id == 0);
	g_assert (_test_loop == NULL);
	_test_loop = g_main_loop_new (NULL, FALSE);
	_test_loop_timeout_id = g_timeout_add (timeout_ms, fu_test_hang_check_cb, NULL);
	g_main_loop_run (_test_loop);
}

static void
fu_test_loop_quit (void)
{
	if (_test_loop_timeout_id > 0) {
		g_source_remove (_test_loop_timeout_id);
		_test_loop_timeout_id = 0;
	}
	if (_test_loop != NULL) {
		g_main_loop_quit (_test_loop);
		g_main_loop_unref (_test_loop);
		_test_loop = NULL;
	}
}

static void
fu_self_test_mkroot (void)
{
	if (g_file_test ("/tmp/fwupd-self-test", G_FILE_TEST_EXISTS)) {
		g_autoptr(GError) error = NULL;
		if (!fu_common_rmtree ("/tmp/fwupd-self-test", &error))
			g_warning ("failed to mkroot: %s", error->message);
	}
	g_assert_cmpint (g_mkdir_with_parents ("/tmp/fwupd-self-test/var/lib/fwupd", 0755), ==, 0);
}

static gboolean
fu_test_compare_lines (const gchar *txt1, const gchar *txt2, GError **error)
{
	g_autofree gchar *output = NULL;
	if (g_strcmp0 (txt1, txt2) == 0)
		return TRUE;
	if (fu_common_fnmatch (txt2, txt1))
		return TRUE;
	if (!g_file_set_contents ("/tmp/a", txt1, -1, error))
		return FALSE;
	if (!g_file_set_contents ("/tmp/b", txt2, -1, error))
		return FALSE;
	if (!g_spawn_command_line_sync ("diff -urNp /tmp/b /tmp/a",
					&output, NULL, NULL, error))
		return FALSE;
	g_set_error_literal (error, 1, 0, output);
	return FALSE;
}

static void
fu_test_free (FuTest *self)
{
	g_object_unref (self->plugin);
	g_free (self);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuTest, fu_test_free)
#pragma clang diagnostic pop

static void
fu_engine_generate_md_func (gconstpointer user_data)
{
	const gchar *tmp;
	gboolean ret;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(GBytes) data = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;

	/* put cab file somewhere we can parse it */
	filename = g_build_filename (TESTDATADIR_DST, "colorhug", "colorhug-als-3.0.2.cab", NULL);
	data = fu_common_get_contents_bytes (filename, &error);
	g_assert_no_error (error);
	g_assert_nonnull (data);
	ret = fu_common_set_contents_bytes ("/tmp/fwupd-self-test/var/cache/fwupd/foo.cab",
					    data, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* load engine and check the device was found */
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_REMOTES, &error);
	g_assert_no_error (error);
	g_assert (ret);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	component = fu_engine_get_component_by_guids (engine, device);
	g_assert_nonnull (component);

	/* check remote ID set */
	tmp = xb_node_query_text (component, "../custom/value[@key='fwupd::RemoteId']", NULL);
	g_assert_cmpstr (tmp, ==, "directory");

	/* verify checksums */
	tmp = xb_node_query_text (component, "releases/release/checksum[@target='container']", NULL);
	g_assert_cmpstr (tmp, !=, NULL);
	tmp = xb_node_query_text (component, "releases/release/checksum[@target='content']", NULL);
	g_assert_cmpstr (tmp, ==, NULL);
}

static void
fu_plugin_hash_func (gconstpointer user_data)
{
	GError *error = NULL;
	g_autofree gchar *pluginfn = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuPlugin) plugin = fu_plugin_new (NULL);
	gboolean ret = FALSE;

	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* make sure not tainted */
	ret = fu_engine_get_tainted (engine);
	g_assert_false (ret);

	/* create a tainted plugin */
	pluginfn = g_build_filename (PLUGINBUILDDIR,
				     "libfu_plugin_invalid." G_MODULE_SUFFIX,
				     NULL);
	ret = fu_plugin_open (plugin, pluginfn, &error);
	g_assert_true (ret);
	g_assert_no_error (error);

	/* make sure it tainted now */
	g_test_expect_message ("FuEngine", G_LOG_LEVEL_WARNING, "* has incorrect built version*");
	fu_engine_add_plugin (engine, plugin);
	ret = fu_engine_get_tainted (engine);
	g_assert_true (ret);
}

static void
fu_engine_requirements_missing_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <id compare=\"ge\" version=\"1.2.3\">not.going.to.exist</id>"
		"  </requires>"
		"</component>";

	/* set up a dummy version */
	fu_engine_add_runtime_version (engine, "org.test.dummy", "1.2.3");

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (!ret);
}

static void
fu_engine_requirements_soft_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *xml =
		"<component>"
		"  <suggests>"
		"    <id compare=\"ge\" version=\"1.2.3\">not.going.to.exist</id>"
		"  </suggests>"
		"</component>";

	/* set up a dummy version */
	fu_engine_add_runtime_version (engine, "org.test.dummy", "1.2.3");

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_FORCE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_client_fail_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <client>detach-action</client>"
		"  </requires>"
		"</component>";

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert (!ret);
}

static void
fu_engine_requirements_client_invalid_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <client>hello-dave</client>"
		"  </requires>"
		"</component>";

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (!ret);
}

static void
fu_engine_requirements_client_pass_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <client>detach-action</client>"
		"  </requires>"
		"</component>";

	/* set up a dummy version */
	fu_engine_request_set_feature_flags (request,
					     FWUPD_FEATURE_FLAG_DETACH_ACTION);

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_version_require_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"    </release>"
		"  </releases>"
		"</component>";

	/* set up a dummy device */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	fu_device_set_version_bootloader (device, "4.5.6");
	fu_device_add_vendor_id (device, "FFFF");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert (g_str_has_prefix (error->message, "device requires firmware with a version check"));
	g_assert (!ret);
}

static void
fu_engine_requirements_unsupported_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <UNKNOWN compare=\"ge\" version=\"2.6.0\"/>"
		"  </requires>"
		"</component>";

	/* set up a dummy version */
	fu_engine_add_runtime_version (engine, "org.test.dummy", "1.2.3");

	/* make the component require one thing that we don't support */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert (!ret);
}

static void
fu_engine_requirements_child_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) child = fu_device_new ();
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <firmware compare=\"eq\" version=\"0.0.1\">not-child</firmware>"
		"  </requires>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* set up a dummy device */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	fu_device_set_version_bootloader (device, "4.5.6");
	fu_device_add_vendor_id (device, "FFFF");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_set_version_format (child, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (child, "0.0.999");
	fu_device_set_physical_id (child, "dummy");
	fu_device_add_child (device, child);

	/* make the component require three things */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_child_fail_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) child = fu_device_new ();
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <firmware compare=\"glob\" version=\"0.0.*\">not-child</firmware>"
		"  </requires>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* set up a dummy device */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	fu_device_set_version_bootloader (device, "4.5.6");
	fu_device_add_vendor_id (device, "FFFF");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_set_version_format (child, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (child, "0.0.1");
	fu_device_set_physical_id (child, "dummy");
	fu_device_add_child (device, child);

	/* make the component require three things */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert_nonnull (g_strstr_len (error->message, -1,
					"Not compatible with child device version"));
        g_assert (!ret);
}

static void
fu_engine_requirements_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <id compare=\"ge\" version=\"1.2.3\">org.test.dummy</id>"
		"  </requires>"
		"</component>";

	/* set up some dummy versions */
	fu_engine_add_runtime_version (engine, "org.test.dummy", "1.2.3");
	fu_engine_add_runtime_version (engine, "com.hughski.colorhug", "7.8.9");

	/* make the component require one thing */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (NULL, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_device_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <firmware compare=\"ge\" version=\"1.2.3\"/>"
		"    <firmware compare=\"eq\" version=\"4.5.6\">bootloader</firmware>"
		"    <firmware compare=\"regex\" version=\"USB:0xFFFF|DMI:Lenovo\">vendor-id</firmware>"
#ifdef __linux__
		"    <id compare=\"ge\" version=\"4.0.0\">org.kernel</id>"
#endif
		"  </requires>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* set up a dummy device */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	fu_device_set_version_bootloader (device, "4.5.6");
	fu_device_add_vendor_id (device, "USB:0xFFFF");
	fu_device_add_vendor_id (device, "PCI:0x0000");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_VERSION_CHECK_REQUIRED);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");

	/* make the component require three things */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_device_plain_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"51H0AALB\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* set up a dummy device */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_PLAIN);
	fu_device_set_version (device, "5101AALB");
	fu_device_add_vendor_id (device, "FFFF");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");

	/* make the component require three things */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_version_format_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	const gchar *xml =
		"<component>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"  <custom>"
		"    <value key=\"LVFS::VersionFormat\">triplet</value>"
		"  </custom>"
		"</component>";

	/* set up a dummy device */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_QUAD);
	fu_device_set_version (device, "1.2.3.4");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");

	/* make the component require three things */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert_nonnull (g_strstr_len (error->message, -1,
					"Firmware version formats were different"));
	g_assert (!ret);
}

static void
fu_engine_requirements_sibling_device_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) unrelated_device3 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) parent = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <firmware depth=\"0\">1ff60ab2-3905-06a1-b476-0371f00c9e9b</firmware>"
		"  </requires>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up a dummy device */
	fu_device_set_id (device1, "id1");
	fu_device_set_version_format (device1, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device1, "1.2.3");
	fu_device_add_vendor_id (device1, "FFFF");
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device1, "12345678-1234-1234-1234-123456789012");
	fu_device_add_protocol (device1, "com.acme");
	fu_engine_add_device (engine, device1);

	/* setup the parent */
	fu_device_set_id (parent, "parent");
	fu_device_set_version_format (parent, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (parent, "1.0.0");
	fu_device_add_flag (parent, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (parent, "42f3d696-0b6f-4d69-908f-357f98ef115e");
	fu_device_add_protocol (parent, "com.acme");
	fu_device_add_child (parent, device1);
	fu_engine_add_device (engine, parent);

	/* set up a different device */
	fu_device_set_id (unrelated_device3, "id3");
	fu_device_add_vendor_id (unrelated_device3, "USB:FFFF");
	fu_device_add_protocol (unrelated_device3, "com.acme");
	fu_device_set_name (unrelated_device3, "Foo bar device");
	fu_device_set_version_format (unrelated_device3, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (unrelated_device3, "1.5.3");
	fu_device_add_vendor_id (unrelated_device3, "FFFF");
	fu_device_add_flag (unrelated_device3, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (unrelated_device3, "3e455c08-352e-4a16-84d3-f04287289fa2");
	fu_engine_add_device (engine, unrelated_device3);

	/* import firmware metainfo */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task = fu_install_task_new (device1, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert_false (ret);
	g_clear_error (&error);

	/* set up a sibling device */
	fu_device_set_id (device2, "id2");
	fu_device_add_vendor_id (device2, "USB:FFFF");
	fu_device_add_protocol (device2, "com.acme");
	fu_device_set_name (device2, "Secondary firmware");
	fu_device_set_version_format (device2, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device2, "4.5.6");
	fu_device_add_vendor_id (device2, "FFFF");
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device2, "1ff60ab2-3905-06a1-b476-0371f00c9e9b");
	fu_device_add_child (parent, device2);
	fu_engine_add_device (engine, device2);

	/* check this passes */
	task = fu_install_task_new (device1, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_other_device_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <firmware compare=\"gt\" version=\"4.0.0\">1ff60ab2-3905-06a1-b476-0371f00c9e9b</firmware>"
		"  </requires>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"1.2.4\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up a dummy device */
	fu_device_set_version_format (device1, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device1, "1.2.3");
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device1, "12345678-1234-1234-1234-123456789012");

	/* set up a different device */
	fu_device_set_id (device2, "id2");
	fu_device_add_vendor_id (device2, "USB:FFFF");
	fu_device_add_protocol (device2, "com.acme");
	fu_device_set_name (device2, "Secondary firmware");
	fu_device_set_version_format (device2, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device2, "4.5.6");
	fu_device_add_vendor_id (device2, "FFFF");
	fu_device_add_guid (device2, "1ff60ab2-3905-06a1-b476-0371f00c9e9b");
	fu_engine_add_device (engine, device2);

	/* import firmware metainfo */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (device1, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_protocol_check_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(FuInstallTask) task1 = NULL;
	g_autoptr(FuInstallTask) task2 = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	gboolean ret;

	const gchar *xml =
		"<component>"
		"  <provides>"
		"    <firmware type=\"flashed\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"4.5.7\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"  <custom>"
		"    <value key=\"LVFS::UpdateProtocol\">org.bar</value>"
		"  </custom>"

		"</component>";

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	fu_device_set_id (device1, "NVME");
	fu_device_add_protocol (device1, "com.acme");
	fu_device_set_name (device1, "NVME device");
	fu_device_add_vendor_id (device1, "ACME");
	fu_device_set_version_format (device1, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device1, "1.2.3");
	fu_device_add_guid (device1, "12345678-1234-1234-1234-123456789012");
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_engine_add_device (engine, device1);

	fu_device_set_id (device2, "UEFI");
	fu_device_add_protocol (device2, "org.bar");
	fu_device_set_name (device2, "UEFI device");
	fu_device_add_vendor_id (device2, "ACME");
	fu_device_set_version_format (device2, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device2, "1.2.3");
	fu_device_add_guid (device2, "12345678-1234-1234-1234-123456789012");
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_engine_add_device (engine, device2);

	/* make sure both devices added */
	devices = fu_engine_get_devices (engine, &error);
	g_assert_no_error (error);
	g_assert_nonnull (devices);
	g_assert_cmpint (devices->len, ==, 2);

	/* import firmware metainfo */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this fails */
	task1 = fu_install_task_new (device1, component);
	ret = fu_engine_check_requirements (engine, request, task1,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert_false (ret);
	g_clear_error (&error);

	/* check this passes */
	task2 = fu_install_task_new (device2, component);
	ret = fu_engine_check_requirements (engine, request, task2,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);

	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_requirements_parent_device_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	const gchar *xml =
		"<component>"
		"  <requires>"
		"    <firmware depth=\"1\" compare=\"eq\" version=\"1.2.3\"/>"
		"    <firmware depth=\"1\">12345678-1234-1234-1234-123456789012</firmware>"
		"  </requires>"
		"  <provides>"
		"    <firmware type=\"flashed\">1ff60ab2-3905-06a1-b476-0371f00c9e9b</firmware>"
		"  </provides>"
		"  <releases>"
		"    <release version=\"4.5.7\">"
		"      <checksum type=\"sha1\" filename=\"bios.bin\" target=\"content\"/>"
		"    </release>"
		"  </releases>"
		"</component>";

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up child device */
	fu_device_set_id (device2, "child");
	fu_device_set_name (device2, "child");
	fu_device_set_version_format (device2, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device2, "4.5.6");
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_guid (device2, "1ff60ab2-3905-06a1-b476-0371f00c9e9b");

	/* set up a parent device */
	fu_device_set_id (device1, "parent");
	fu_device_add_vendor_id (device1, "USB:FFFF");
	fu_device_add_protocol (device1, "com.acme");
	fu_device_set_name (device1, "parent");
	fu_device_set_version_format (device1, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device1, "1.2.3");
	fu_device_add_guid (device1, "12345678-1234-1234-1234-123456789012");
	fu_device_add_child (device1, device2);
	fu_engine_add_device (engine, device1);

	/* import firmware metainfo */
	silo = xb_silo_new_from_xml (xml, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "component", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check this passes */
	task = fu_install_task_new (device2, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_device_parent_guid_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device3 = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* add child */
	fu_device_set_id (device1, "child");
	fu_device_add_vendor_id (device2, "USB:FFFF");
	fu_device_add_protocol (device2, "com.acme");
	fu_device_add_instance_id (device1, "child-GUID-1");
	fu_device_add_parent_guid (device1, "parent-GUID");
	fu_device_convert_instance_ids (device1);
	fu_engine_add_device (engine, device1);

	/* parent */
	fu_device_set_id (device2, "parent");
	fu_device_add_vendor_id (device2, "USB:FFFF");
	fu_device_add_protocol (device2, "com.acme");
	fu_device_add_instance_id (device2, "parent-GUID");
	fu_device_set_vendor (device2, "oem");
	fu_device_convert_instance_ids (device2);

	/* add another child */
	fu_device_set_id (device3, "child2");
	fu_device_add_instance_id (device3, "child-GUID-2");
	fu_device_add_parent_guid (device3, "parent-GUID");
	fu_device_convert_instance_ids (device3);
	fu_device_add_child (device2, device3);

	/* add two together */
	fu_engine_add_device (engine, device2);

	/* this is normally done by fu_plugin_device_add() */
	fu_engine_add_device (engine, device3);

	/* verify both children were adopted */
	g_assert (fu_device_get_parent (device3) == device2);
	g_assert (fu_device_get_parent (device1) == device2);
	g_assert_cmpstr (fu_device_get_vendor (device3), ==, "oem");

	/* verify order */
	g_assert_cmpint (fu_device_get_order (device1), ==, -1);
	g_assert_cmpint (fu_device_get_order (device2), ==, 0);
	g_assert_cmpint (fu_device_get_order (device3), ==, -1);
}

static void
fu_engine_device_parent_id_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device3 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device4 = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* add child */
	fu_device_set_id (device1, "child1");
	fu_device_set_name (device1, "Child1");
	fu_device_set_physical_id (device1, "child-ID1");
	fu_device_add_vendor_id (device1, "USB:FFFF");
	fu_device_add_protocol (device1, "com.acme");
	fu_device_add_instance_id (device1, "child-GUID-1");
	fu_device_add_parent_physical_id (device1, "parent-ID-notfound");
	fu_device_add_parent_physical_id (device1, "parent-ID");
	fu_device_convert_instance_ids (device1);
	fu_engine_add_device (engine, device1);

	/* parent */
	fu_device_set_id (device2, "parent");
	fu_device_set_name (device2, "Parent");
	fu_device_set_physical_id (device2, "parent-ID");
	fu_device_add_vendor_id (device2, "USB:FFFF");
	fu_device_add_protocol (device2, "com.acme");
	fu_device_add_instance_id (device2, "parent-GUID");
	fu_device_set_vendor (device2, "oem");
	fu_device_add_internal_flag (device2, FU_DEVICE_INTERNAL_FLAG_AUTO_PARENT_CHILDREN);
	fu_device_convert_instance_ids (device2);

	/* add another child */
	fu_device_set_id (device3, "child2");
	fu_device_set_name (device3, "Child2");
	fu_device_set_physical_id (device3, "child-ID2");
	fu_device_add_instance_id (device3, "child-GUID-2");
	fu_device_add_parent_physical_id (device3, "parent-ID");
	fu_device_convert_instance_ids (device3);
	fu_device_add_child (device2, device3);

	/* add two together */
	fu_engine_add_device (engine, device2);

	/* add non-child */
	fu_device_set_id (device4, "child4");
	fu_device_set_name (device4, "Child4");
	fu_device_set_physical_id (device4, "child-ID4");
	fu_device_add_vendor_id (device4, "USB:FFFF");
	fu_device_add_protocol (device4, "com.acme");
	fu_device_add_instance_id (device4, "child-GUID-4");
	fu_device_add_parent_physical_id (device4, "parent-ID");
	fu_device_convert_instance_ids (device4);
	fu_engine_add_device (engine, device4);

	/* this is normally done by fu_plugin_device_add() */
	fu_engine_add_device (engine, device4);

	/* verify both children were adopted */
	g_assert (fu_device_get_parent (device3) == device2);
	g_assert (fu_device_get_parent (device4) == device2);
	g_assert (fu_device_get_parent (device1) == device2);
	g_assert_cmpstr (fu_device_get_vendor (device3), ==, "oem");
}

static void
fu_engine_partial_hash_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuPlugin) plugin = fu_plugin_new (NULL);
	g_autoptr(GError) error = NULL;
	g_autoptr(GError) error_none = NULL;
	g_autoptr(GError) error_both = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up dummy plugin */
	fu_plugin_set_name (plugin, "test");
	fu_plugin_set_build_hash (plugin, FU_BUILD_HASH);
	fu_engine_add_plugin (engine, plugin);

	/* add two dummy devices */
	fu_device_set_id (device1, "device1");
	fu_device_add_vendor_id (device1, "USB:FFFF");
	fu_device_add_protocol (device1, "com.acme");
	fu_device_set_plugin (device1, "test");
	fu_device_add_guid (device1, "12345678-1234-1234-1234-123456789012");
	fu_engine_add_device (engine, device1);
	fu_device_set_id (device2, "device21");
	fu_device_add_vendor_id (device2, "USB:FFFF");
	fu_device_add_protocol (device2, "com.acme");
	fu_device_set_plugin (device2, "test");
	fu_device_set_equivalent_id (device2, "b92f5b7560b84ca005a79f5a15de3c003ce494cf");
	fu_device_add_guid (device2, "87654321-1234-1234-1234-123456789012");
	fu_engine_add_device (engine, device2);

	/* match nothing */
	ret = fu_engine_unlock (engine, "deadbeef", &error_none);
	g_assert_error (error_none, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (!ret);

	/* match both */
	ret = fu_engine_unlock (engine, "9", &error_both);
	g_assert_error (error_both, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert (!ret);

	/* match one exactly */
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_LOCKED);
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_LOCKED);
	ret = fu_engine_unlock (engine, "934b4162a6daa0b033d649c8d464529cec41d3de", &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* match one partially */
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_LOCKED);
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_LOCKED);
	ret = fu_engine_unlock (engine, "934b", &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* match equivalent ID */
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_LOCKED);
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_LOCKED);
	ret = fu_engine_unlock (engine, "b92f", &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fu_engine_device_unlock_func (gconstpointer user_data)
{
	gboolean ret;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file = NULL;
	g_autoptr(XbBuilder) builder = xb_builder_new ();
	g_autoptr(XbBuilderSource) source = xb_builder_source_new ();
	g_autoptr(XbSilo) silo = NULL;

	/* load engine to get FuConfig set up */
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* add the hardcoded 'fwupd' metadata */
	filename = g_build_filename (TESTDATADIR_SRC, "metadata.xml", NULL);
	file = g_file_new_for_path (filename);
	ret = xb_builder_source_load_file (source, file,
					   XB_BUILDER_SOURCE_FLAG_NONE,
					   NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	xb_builder_import_source (builder, source);
	silo = xb_builder_compile (builder, XB_BUILDER_COMPILE_FLAG_NONE, NULL, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	fu_engine_set_silo (engine, silo);

	/* add a dummy device */
	fu_device_set_id (device, "UEFI-dummy-dev0");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_add_guid (device, "2d47f29b-83a2-4f31-a2e8-63474f4d4c2e");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_LOCKED);
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_PLAIN);
	fu_engine_add_device (engine, device);

	/* ensure the metainfo was matched */
	g_assert_nonnull (fwupd_device_get_release_default (FWUPD_DEVICE (device)));
}

static void
fu_engine_require_hwid_func (gconstpointer user_data)
{
	gboolean ret;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	g_autoptr(XbSilo) silo = NULL;

#if defined(__s390x__)
	/* See https://github.com/fwupd/fwupd/issues/318 for more information */
	g_test_skip ("Skipping HWID test on s390x due to known problem with gcab");
	return;
#endif

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* load engine to get FuConfig set up */
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* get generated file as a blob */
	filename = g_build_filename (TESTDATADIR_DST, "missing-hwid", "hwid-1.2.3.cab", NULL);
	blob_cab = fu_common_get_contents_bytes	(filename, &error);
	g_assert_no_error (error);
	g_assert (blob_cab != NULL);
	silo = fu_engine_get_silo_from_blob (engine, blob_cab, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);

	/* add a dummy device */
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_engine_add_device (engine, device);

	/* get component */
	component = xb_silo_query_first (silo, "components/component/id[text()='com.hughski.test.firmware']/..", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* check requirements */
	task = fu_install_task_new (device, component);
	ret = fu_engine_check_requirements (engine, request, task,
					    FWUPD_INSTALL_FLAG_NONE,
					    &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_INVALID_FILE);
	g_assert (error != NULL);
	g_assert_cmpstr (error->message, ==,
			 "no HWIDs matched 9342d47a-1bab-5709-9869-c840b2eac501");
	g_assert (!ret);
}

static void
fu_engine_downgrade_func (gconstpointer user_data)
{
	FwupdRelease *rel;
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices_pre = NULL;
	g_autoptr(GPtrArray) releases_dg = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(GPtrArray) releases_up = NULL;
	g_autoptr(GPtrArray) remotes = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();

	/* ensure empty tree */
	fu_self_test_mkroot ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* write a broken file */
	ret = g_file_set_contents ("/tmp/fwupd-self-test/broken.xml.gz",
				   "this is not a valid", -1, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* write the main file */
	ret = g_file_set_contents ("/tmp/fwupd-self-test/stable.xml",
				   "<components>"
				   "  <component type=\"firmware\">"
				   "    <id>test</id>"
				   "    <name>Test Device</name>"
				   "    <provides>"
				   "      <firmware type=\"flashed\">aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee</firmware>"
				   "    </provides>"
				   "    <releases>"
				   "      <release version=\"1.2.3\" date=\"2017-09-15\">"
				   "        <size type=\"installed\">123</size>"
				   "        <size type=\"download\">456</size>"
				   "        <location>https://test.org/foo.cab</location>"
				   "        <checksum filename=\"foo.cab\" target=\"container\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "        <checksum filename=\"firmware.bin\" target=\"content\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "      </release>"
				   "      <release version=\"1.2.2\" date=\"2017-09-01\">"
				   "        <size type=\"installed\">123</size>"
				   "        <size type=\"download\">456</size>"
				   "        <location>https://test.org/foo.cab</location>"
				   "        <checksum filename=\"foo.cab\" target=\"container\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "        <checksum filename=\"firmware.bin\" target=\"content\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "      </release>"
				   "    </releases>"
				   "  </component>"
				   "</components>", -1, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* write the extra file */
	ret = g_file_set_contents ("/tmp/fwupd-self-test/testing.xml",
				   "<components>"
				   "  <component type=\"firmware\">"
				   "    <id>test</id>"
				   "    <name>Test Device</name>"
				   "    <provides>"
				   "      <firmware type=\"flashed\">aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee</firmware>"
				   "    </provides>"
				   "    <releases>"
				   "      <release version=\"1.2.5\" date=\"2017-09-16\">"
				   "        <size type=\"installed\">123</size>"
				   "        <size type=\"download\">456</size>"
				   "        <location>https://test.org/foo.cab</location>"
				   "        <checksum filename=\"foo.cab\" target=\"container\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "        <checksum filename=\"firmware.bin\" target=\"content\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "      </release>"
				   "      <release version=\"1.2.4\" date=\"2017-09-15\">"
				   "        <size type=\"installed\">123</size>"
				   "        <size type=\"download\">456</size>"
				   "        <location>https://test.org/foo.cab</location>"
				   "        <checksum filename=\"foo.cab\" target=\"container\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "        <checksum filename=\"firmware.bin\" target=\"content\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "      </release>"
				   "    </releases>"
				   "  </component>"
				   "</components>", -1, &error);
	g_assert_no_error (error);
	g_assert (ret);

	g_setenv ("CONFIGURATION_DIRECTORY", TESTDATADIR_SRC, TRUE);
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_REMOTES, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fu_engine_get_status (engine), ==, FWUPD_STATUS_IDLE);
	g_test_assert_expected_messages ();

	/* return all the remotes, even the broken one */
	remotes = fu_engine_get_remotes (engine, &error);
	g_assert_no_error (error);
	g_assert (remotes != NULL);
	g_assert_cmpint (remotes->len, ==, 4);

	/* ensure there are no devices already */
	devices_pre = fu_engine_get_devices (engine, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO);
	g_assert (devices_pre == NULL);
	g_clear_error (&error);

	/* add a device so we can get upgrades and downgrades */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_add_guid (device, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
#ifndef HAVE_POLKIT
	g_test_expect_message ("FuEngine", G_LOG_LEVEL_WARNING, "*archive signature missing or not trusted");
#endif
	fu_engine_add_device (engine, device);
	devices = fu_engine_get_devices (engine, &error);
	g_assert_no_error (error);
	g_assert (devices != NULL);
	g_assert_cmpint (devices->len, ==, 1);
#ifdef HAVE_POLKIT
	g_assert (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_SUPPORTED));
#endif
	g_assert (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED));

	/* get the releases for one device */
	releases = fu_engine_get_releases (engine,
					   request,
					   fu_device_get_id (device),
					   &error);
	g_assert_no_error (error);
	g_assert (releases != NULL);
	g_assert_cmpint (releases->len, ==, 4);

	/* no upgrades, as no firmware is approved */
	releases_up = fu_engine_get_upgrades (engine,
					      request,
					      fu_device_get_id (device),
					      &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO);
	g_assert_null (releases_up);
	g_clear_error (&error);

	/* retry with approved firmware set */
	fu_engine_add_approved_firmware (engine, "deadbeefdeadbeefdeadbeefdeadbeef");
	fu_engine_add_approved_firmware (engine, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");

	/* upgrades */
	releases_up = fu_engine_get_upgrades (engine,
					      request,
					      fu_device_get_id (device),
					      &error);
	g_assert_no_error (error);
	g_assert (releases_up != NULL);
	g_assert_cmpint (releases_up->len, ==, 2);

	/* ensure the list is sorted */
	rel = FWUPD_RELEASE (g_ptr_array_index (releases_up, 0));
	g_assert_cmpstr (fwupd_release_get_version (rel), ==, "1.2.5");
	rel = FWUPD_RELEASE (g_ptr_array_index (releases_up, 1));
	g_assert_cmpstr (fwupd_release_get_version (rel), ==, "1.2.4");

	/* downgrades */
	releases_dg = fu_engine_get_downgrades (engine,
						request,
						fu_device_get_id (device),
						&error);
	g_assert_no_error (error);
	g_assert (releases_dg != NULL);
	g_assert_cmpint (releases_dg->len, ==, 1);
	rel = FWUPD_RELEASE (g_ptr_array_index (releases_dg, 0));
	g_assert_cmpstr (fwupd_release_get_version (rel), ==, "1.2.2");
}

static void
fu_engine_install_duration_func (gconstpointer user_data)
{
	FwupdRelease *rel;
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) releases = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();

	/* ensure empty tree */
	fu_self_test_mkroot ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* write the main file */
	ret = g_file_set_contents ("/tmp/fwupd-self-test/stable.xml",
				   "<components>"
				   "  <component type=\"firmware\">"
				   "    <id>test</id>"
				   "    <provides>"
				   "      <firmware type=\"flashed\">aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee</firmware>"
				   "    </provides>"
				   "    <releases>"
				   "      <release version=\"1.2.3\" date=\"2017-09-15\" install_duration=\"120\">"
				   "        <location>https://test.org/foo.cab</location>"
				   "        <checksum filename=\"foo.cab\" target=\"container\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "        <checksum filename=\"firmware.bin\" target=\"content\" type=\"md5\">deadbeefdeadbeefdeadbeefdeadbeef</checksum>"
				   "      </release>"
				   "    </releases>"
				   "  </component>"
				   "</components>", -1, &error);
	g_assert_no_error (error);
	g_assert (ret);

	g_setenv ("CONFIGURATION_DIRECTORY", TESTDATADIR_SRC, TRUE);
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_REMOTES, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* add a device so we can get the install duration */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.3");
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_add_guid (device, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
	fu_device_set_install_duration (device, 999);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
#ifndef HAVE_POLKIT
	g_test_expect_message ("FuEngine", G_LOG_LEVEL_WARNING, "*archive signature missing or not trusted");
#endif
	fu_engine_add_device (engine, device);
	devices = fu_engine_get_devices (engine, &error);
	g_assert_no_error (error);
	g_assert (devices != NULL);
	g_assert_cmpint (devices->len, ==, 1);
#ifdef HAVE_POLKIT
	g_assert (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_SUPPORTED));
#endif
	/* check the release install duration */
	releases = fu_engine_get_releases (engine,
					   request,
					   fu_device_get_id (device),
					   &error);
	g_assert_no_error (error);
	g_assert (releases != NULL);
	g_assert_cmpint (releases->len, ==, 1);
	rel = FWUPD_RELEASE (g_ptr_array_index (releases, 0));
	g_assert_cmpint (fwupd_release_get_install_duration (rel), ==, 120);
}

static void
fu_engine_history_func (gconstpointer user_data)
{
	FuTest *self = (FuTest *) user_data;
	gboolean ret;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *device_str_expected = NULL;
	g_autofree gchar *device_str = NULL;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device2 = NULL;
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuHistory) history = NULL;
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(FwupdDevice) device3 = NULL;
	g_autoptr(FwupdDevice) device4 = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	g_autoptr(XbSilo) silo = NULL;

	/* ensure empty tree */
	fu_self_test_mkroot ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up dummy plugin */
	fu_engine_add_plugin (engine, self->plugin);

	g_setenv ("CONFIGURATION_DIRECTORY", TESTDATADIR_SRC, TRUE);
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fu_engine_get_status (engine), ==, FWUPD_STATUS_IDLE);

	/* add a device so we can get upgrade it */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_set_plugin (device, "test");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_add_checksum (device, "0123456789abcdef0123456789abcdef01234567");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_set_created (device, 1515338000);
	fu_engine_add_device (engine, device);
	devices = fu_engine_get_devices (engine, &error);
	g_assert_no_error (error);
	g_assert (devices != NULL);
	g_assert_cmpint (devices->len, ==, 1);
	g_assert (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED));

	filename = g_build_filename (TESTDATADIR_DST, "missing-hwid", "noreqs-1.2.3.cab", NULL);
	blob_cab = fu_common_get_contents_bytes	(filename, &error);
	g_assert_no_error (error);
	g_assert (blob_cab != NULL);
	silo = fu_engine_get_silo_from_blob (engine, blob_cab, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);

	/* get component */
	component = xb_silo_query_first (silo, "components/component/id[text()='com.hughski.test.firmware']/..", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* set the counter */
	g_setenv ("FWUPD_PLUGIN_TEST", "another-write-required", TRUE);
	fu_device_set_metadata_integer (device, "nr-update", 0);

	/* install it */
	task = fu_install_task_new (device, component);
	ret = fu_engine_install (engine, task, blob_cab,
				 FWUPD_INSTALL_FLAG_NONE,
				 FWUPD_FEATURE_FLAG_NONE,
				 &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* check the write was done more than once */
	g_assert_cmpint (fu_device_get_metadata_integer (device, "nr-update"), ==, 2);

	/* check the history database */
	history = fu_history_new ();
	device2 = fu_history_get_device_by_id (history, fu_device_get_id (device), &error);
	g_assert_no_error (error);
	g_assert (device2 != NULL);
	g_assert_cmpint (fu_device_get_update_state (device2), ==, FWUPD_UPDATE_STATE_SUCCESS);
	g_assert_cmpstr (fu_device_get_update_error (device2), ==, NULL);
	fu_device_set_modified (device2, 1514338000);
	g_hash_table_remove_all (fwupd_release_get_metadata (fu_device_get_release_default (device2)));
	device_str = fu_device_to_string (device2);
	checksum = g_compute_checksum_for_bytes (G_CHECKSUM_SHA1, blob_cab);
	device_str_expected = g_strdup_printf (
		"FuDevice:\n"
		"Test Device\n"
		"  DeviceId:             894e8c17a29428b09d10cd90d1db74ea76fbcfe8\n"
		"  Guid:                 12345678-1234-1234-1234-123456789012\n"
		"  Plugin:               test\n"
		"  Flags:                updatable|historical\n"
		"  Version:              1.2.2\n"
		"  Created:              2018-01-07\n"
		"  Modified:             2017-12-27\n"
		"  UpdateState:          success\n"
		"  \n"
		"  [Release]\n"
		"  Version:              1.2.3\n"
		"  Checksum:             SHA1(%s)\n"
		"  Flags:                none\n",
		checksum);
	ret = fu_test_compare_lines (device_str, device_str_expected, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* GetResults() */
	device3 = fu_engine_get_results (engine, FWUPD_DEVICE_ID_ANY, &error);
	g_assert (device3 != NULL);
	g_assert_cmpstr (fu_device_get_id (device3), ==,
			 "894e8c17a29428b09d10cd90d1db74ea76fbcfe8");
	g_assert_cmpint (fu_device_get_update_state (device3), ==, FWUPD_UPDATE_STATE_SUCCESS);
	g_assert_cmpstr (fu_device_get_update_error (device3), ==, NULL);

	/* ClearResults() */
	ret = fu_engine_clear_results (engine, FWUPD_DEVICE_ID_ANY, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* GetResults() */
	device4 = fu_engine_get_results (engine, FWUPD_DEVICE_ID_ANY, &error);
	g_assert (device4 == NULL);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO);
}

static void
fu_engine_multiple_rels_func (gconstpointer user_data)
{
	FuTest *self = (FuTest *) user_data;
	gboolean ret;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	g_autoptr(XbSilo) silo = NULL;

	/* ensure empty tree */
	fu_self_test_mkroot ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up dummy plugin */
	fu_engine_add_plugin (engine, self->plugin);

	g_setenv ("CONFIGURATION_DIRECTORY", TESTDATADIR_SRC, TRUE);
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fu_engine_get_status (engine), ==, FWUPD_STATUS_IDLE);

	/* add a device so we can get upgrade it */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_set_plugin (device, "test");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_add_checksum (device, "0123456789abcdef0123456789abcdef01234567");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_INSTALL_ALL_RELEASES);
	fu_device_set_created (device, 1515338000);
	fu_engine_add_device (engine, device);

	filename = g_build_filename (TESTDATADIR_DST, "multiple-rels", "multiple-rels-1.2.4.cab", NULL);
	blob_cab = fu_common_get_contents_bytes	(filename, &error);
	g_assert_no_error (error);
	g_assert (blob_cab != NULL);
	silo = fu_engine_get_silo_from_blob (engine, blob_cab, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);

	/* get component */
	component = xb_silo_query_first (silo, "components/component/id[text()='com.hughski.test.firmware']/..", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* set up counter */
	fu_device_set_metadata_integer (device, "nr-update", 0);

	/* install it */
	task = fu_install_task_new (device, component);
	ret = fu_engine_install (engine, task, blob_cab,
				 FWUPD_INSTALL_FLAG_NONE,
				 FWUPD_FEATURE_FLAG_NONE,
				 &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* check we did 1.2.2 -> 1.2.3 -> 1.2.4 */
	g_assert_cmpint (fu_device_get_metadata_integer (device, "nr-update"), ==, 2);
	g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.4");
}

static void
fu_engine_history_inherit (gconstpointer user_data)
{
	FuTest *self = (FuTest *) user_data;
	gboolean ret;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	g_autoptr(XbSilo) silo = NULL;

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up dummy plugin */
	g_setenv ("FWUPD_PLUGIN_TEST", "fail", TRUE);
	fu_engine_add_plugin (engine, self->plugin);
	g_setenv ("CONFIGURATION_DIRECTORY", TESTDATADIR_SRC, TRUE);
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fu_engine_get_status (engine), ==, FWUPD_STATUS_IDLE);

	/* add a device so we can get upgrade it */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_set_plugin (device, "test");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_set_created (device, 1515338000);
	fu_engine_add_device (engine, device);
	devices = fu_engine_get_devices (engine, &error);
	g_assert_no_error (error);
	g_assert (devices != NULL);
	g_assert_cmpint (devices->len, ==, 1);
	g_assert (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED));

	filename = g_build_filename (TESTDATADIR_DST, "missing-hwid", "noreqs-1.2.3.cab", NULL);
	blob_cab = fu_common_get_contents_bytes	(filename, &error);
	g_assert_no_error (error);
	g_assert (blob_cab != NULL);
	silo = fu_engine_get_silo_from_blob (engine, blob_cab, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);

	/* get component */
	component = xb_silo_query_first (silo, "components/component/id[text()='com.hughski.test.firmware']/..", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);

	/* install it */
	g_setenv ("FWUPD_PLUGIN_TEST", "requires-activation", TRUE);
	task = fu_install_task_new (device, component);
	ret = fu_engine_install (engine, task, blob_cab,
				 FWUPD_INSTALL_FLAG_NONE,
				 FWUPD_FEATURE_FLAG_NONE,
				 &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* check the device requires an activation */
	g_assert_true (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION));
	g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.2");

	/* activate the device */
	ret = fu_engine_activate (engine, fu_device_get_id (device), &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* check the device no longer requires an activation */
	g_assert_false (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION));
	g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.3");

	/* emulate getting the flag for a fresh boot on old firmware */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	ret = fu_engine_install (engine, task, blob_cab,
				 FWUPD_INSTALL_FLAG_NONE,
				 FWUPD_FEATURE_FLAG_NONE,
				 &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_object_unref (engine);
	g_object_unref (device);
	engine = fu_engine_new (FU_APP_FLAGS_NONE);
	fu_engine_set_silo (engine, silo_empty);
	fu_engine_add_plugin (engine, self->plugin);
	device = fu_device_new_with_context (ctx);
	fu_device_add_internal_flag (device, FU_DEVICE_INTERNAL_FLAG_INHERIT_ACTIVATION);
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_engine_add_device (engine, device);
	g_assert_true (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION));

	/* emulate not getting the flag */
	g_object_unref (engine);
	g_object_unref (device);
	engine = fu_engine_new (FU_APP_FLAGS_NONE);
	fu_engine_set_silo (engine, silo_empty);
	fu_engine_add_plugin (engine, self->plugin);
	device = fu_device_new_with_context (ctx);
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_engine_add_device (engine, device);
	g_assert_false (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_ACTIVATION));

}

static void
fu_engine_history_error_func (gconstpointer user_data)
{
	FuTest *self = (FuTest *) user_data;
	gboolean ret;
	g_autofree gchar *checksum = NULL;
	g_autofree gchar *device_str_expected = NULL;
	g_autofree gchar *device_str = NULL;
	g_autofree gchar *filename = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device2 = NULL;
	g_autoptr(FuDevice) device = fu_device_new_with_context (ctx);
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuHistory) history = NULL;
	g_autoptr(FuInstallTask) task = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GError) error2 = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(XbNode) component = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	g_autoptr(XbSilo) silo = NULL;

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* set up dummy plugin */
	g_setenv ("FWUPD_PLUGIN_TEST", "fail", TRUE);
	fu_engine_add_plugin (engine, self->plugin);

	g_setenv ("CONFIGURATION_DIRECTORY", TESTDATADIR_SRC, TRUE);
	ret = fu_engine_load (engine, FU_ENGINE_LOAD_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fu_engine_get_status (engine), ==, FWUPD_STATUS_IDLE);

	/* add a device so we can get upgrade it */
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "1.2.2");
	fu_device_set_id (device, "test_device");
	fu_device_add_vendor_id (device, "USB:FFFF");
	fu_device_add_protocol (device, "com.acme");
	fu_device_set_name (device, "Test Device");
	fu_device_set_plugin (device, "test");
	fu_device_add_guid (device, "12345678-1234-1234-1234-123456789012");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_UPDATABLE);
	fu_device_set_created (device, 1515338000);
	fu_engine_add_device (engine, device);
	devices = fu_engine_get_devices (engine, &error);
	g_assert_no_error (error);
	g_assert (devices != NULL);
	g_assert_cmpint (devices->len, ==, 1);
	g_assert (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_REGISTERED));

	/* install the wrong thing */
	filename = g_build_filename (TESTDATADIR_DST, "missing-hwid", "noreqs-1.2.3.cab", NULL);
	blob_cab = fu_common_get_contents_bytes	(filename, &error);
	g_assert_no_error (error);
	g_assert (blob_cab != NULL);
	silo = fu_engine_get_silo_from_blob (engine, blob_cab, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	component = xb_silo_query_first (silo, "components/component/id[text()='com.hughski.test.firmware']/..", &error);
	g_assert_no_error (error);
	g_assert_nonnull (component);
	task = fu_install_task_new (device, component);
	ret = fu_engine_install (engine, task, blob_cab,
				 FWUPD_INSTALL_FLAG_NONE,
				 FWUPD_FEATURE_FLAG_NONE,
				 &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED);
	g_assert (error != NULL);
	g_assert_cmpstr (error->message, ==,
			 "device was not in supported mode");
	g_assert (!ret);

	/* check the history database */
	history = fu_history_new ();
	device2 = fu_history_get_device_by_id (history, fu_device_get_id (device), &error2);
	g_assert_no_error (error2);
	g_assert (device2 != NULL);
	g_assert_cmpint (fu_device_get_update_state (device2), ==, FWUPD_UPDATE_STATE_FAILED);
	g_assert_cmpstr (fu_device_get_update_error (device2), ==, error->message);
	g_clear_error (&error);
	fu_device_set_modified (device2, 1514338000);
	g_hash_table_remove_all (fwupd_release_get_metadata (fu_device_get_release_default (device2)));
	device_str = fu_device_to_string (device2);
	checksum = g_compute_checksum_for_bytes (G_CHECKSUM_SHA1, blob_cab);
	device_str_expected = g_strdup_printf (
		"FuDevice:\n"
		"Test Device\n"
		"  DeviceId:             894e8c17a29428b09d10cd90d1db74ea76fbcfe8\n"
		"  Guid:                 12345678-1234-1234-1234-123456789012\n"
		"  Plugin:               test\n"
		"  Flags:                updatable|historical\n"
		"  Version:              1.2.2\n"
		"  Created:              2018-01-07\n"
		"  Modified:             2017-12-27\n"
		"  UpdateState:          failed\n"
		"  UpdateError:          device was not in supported mode\n"
		"  \n"
		"  [Release]\n"
		"  Version:              1.2.3\n"
		"  Checksum:             SHA1(%s)\n"
		"  Flags:                none\n",
		checksum);
	ret = fu_test_compare_lines (device_str, device_str_expected, &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
_device_list_count_cb (FuDeviceList *device_list, FuDevice *device, gpointer user_data)
{
	guint *cnt = (guint *) user_data;
	(*cnt)++;
}

static void
fu_device_list_no_auto_remove_children_func (gconstpointer user_data)
{
	g_autoptr(FuDevice) child = fu_device_new ();
	g_autoptr(FuDevice) parent = fu_device_new ();
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	g_autoptr(GPtrArray) active1 = NULL;
	g_autoptr(GPtrArray) active2 = NULL;
	g_autoptr(GPtrArray) active3 = NULL;

	/* normal behavior, remove child with parent */
	fu_device_set_id (parent, "parent");
	fu_device_set_id (child, "child");
	fu_device_add_child (parent, child);
	fu_device_list_add (device_list, parent);
	fu_device_list_add (device_list, child);
	fu_device_list_remove (device_list, parent);
	active1 = fu_device_list_get_active (device_list);
	g_assert_cmpint (active1->len, ==, 0);

	/* new-style behavior, do not remove child */
	fu_device_add_internal_flag (parent, FU_DEVICE_INTERNAL_FLAG_NO_AUTO_REMOVE_CHILDREN);
	fu_device_list_add (device_list, parent);
	fu_device_list_add (device_list, child);
	fu_device_list_remove (device_list, parent);
	active2 = fu_device_list_get_active (device_list);
	g_assert_cmpint (active2->len, ==, 1);
	fu_device_list_remove (device_list, child);
	active3 = fu_device_list_get_active (device_list);
	g_assert_cmpint (active3->len, ==, 0);
}

static void
fu_device_list_delay_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	guint added_cnt = 0;
	guint changed_cnt = 0;
	guint removed_cnt = 0;

	g_signal_connect (device_list, "added",
			  G_CALLBACK (_device_list_count_cb),
			  &added_cnt);
	g_signal_connect (device_list, "removed",
			  G_CALLBACK (_device_list_count_cb),
			  &removed_cnt);
	g_signal_connect (device_list, "changed",
			  G_CALLBACK (_device_list_count_cb),
			  &changed_cnt);

	/* add one device */
	fu_device_set_id (device1, "device1");
	fu_device_add_instance_id (device1, "foobar");
	fu_device_set_remove_delay (device1, 100);
	fu_device_convert_instance_ids (device1);
	fu_device_list_add (device_list, device1);
	g_assert_cmpint (added_cnt, ==, 1);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 0);

	/* add the same device again */
	fu_device_list_add (device_list, device1);
	g_assert_cmpint (added_cnt, ==, 1);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 1);

	/* add a device with the same ID */
	fu_device_set_id (device2, "device1");
	fu_device_list_add (device_list, device2);
	fu_device_set_remove_delay (device2, 100);
	g_assert_cmpint (added_cnt, ==, 1);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 2);

	/* spin a bit */
	fu_test_loop_run_with_timeout (10);
	fu_test_loop_quit ();

	/* verify only a changed event was generated */
	added_cnt = removed_cnt = changed_cnt = 0;
	fu_device_list_remove (device_list, device1);
	fu_device_list_add (device_list, device1);
	g_assert_cmpint (added_cnt, ==, 0);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 1);
}

typedef struct {
	FuDevice	*device_new;
	FuDevice	*device_old;
	FuDeviceList	*device_list;
} FuDeviceListReplugHelper;

static gboolean
fu_device_list_remove_cb (gpointer user_data)
{
	FuDeviceListReplugHelper *helper = (FuDeviceListReplugHelper *) user_data;
	fu_device_list_remove (helper->device_list, helper->device_old);
	return FALSE;
}

static gboolean
fu_device_list_add_cb (gpointer user_data)
{
	FuDeviceListReplugHelper *helper = (FuDeviceListReplugHelper *) user_data;
	fu_device_list_add (helper->device_list, helper->device_new);
	return FALSE;
}

static void
fu_device_list_replug_auto_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuDevice) device1 = fu_device_new ();
	g_autoptr(FuDevice) device2 = fu_device_new ();
	g_autoptr(FuDevice) parent = fu_device_new ();
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	g_autoptr(GError) error = NULL;
	FuDeviceListReplugHelper helper;

	/* parent */
	fu_device_set_id (parent, "parent");

	/* fake child devices */
	fu_device_set_id (device1, "device1");
	fu_device_add_internal_flag (device1, FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_set_physical_id (device1, "ID");
	fu_device_set_plugin (device1, "self-test");
	fu_device_set_remove_delay (device1, FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE);
	fu_device_add_child (parent, device1);
	fu_device_set_id (device2, "device2");
	fu_device_add_internal_flag (device2, FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_set_physical_id (device2, "ID"); /* matches */
	fu_device_set_plugin (device2, "self-test");
	fu_device_set_remove_delay (device2, FU_DEVICE_REMOVE_DELAY_RE_ENUMERATE);

	/* not yet added */
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* add device */
	fu_device_list_add (device_list, device1);

	/* not waiting */
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* waiting */
	helper.device_old = device1;
	helper.device_new = device2;
	helper.device_list = device_list;
	g_timeout_add (100, fu_device_list_remove_cb, &helper);
	g_timeout_add (200, fu_device_list_add_cb, &helper);
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_false (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));

	/* check device2 now has parent too */
	g_assert (fu_device_get_parent (device2) == parent);

	/* waiting, failed */
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (!ret);
	g_assert_false (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));
}

static void
fu_device_list_replug_user_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	g_autoptr(GError) error = NULL;
	FuDeviceListReplugHelper helper;

	/* fake devices */
	fu_device_set_id (device1, "device1");
	fu_device_set_name (device1, "device1");
	fu_device_add_internal_flag (device1, FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_add_instance_id (device1, "foo");
	fu_device_add_instance_id (device1, "bar");
	fu_device_set_plugin (device1, "self-test");
	fu_device_set_remove_delay (device1, FU_DEVICE_REMOVE_DELAY_USER_REPLUG);
	fu_device_convert_instance_ids (device1);
	fu_device_set_id (device2, "device2");
	fu_device_set_name (device2, "device2");
	fu_device_add_internal_flag (device2, FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_add_instance_id (device2, "baz");
	fu_device_add_instance_id (device2, "bar"); /* matches */
	fu_device_set_plugin (device2, "self-test");
	fu_device_set_remove_delay (device2, FU_DEVICE_REMOVE_DELAY_USER_REPLUG);
	fu_device_convert_instance_ids (device2);

	/* not yet added */
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* add device */
	fu_device_list_add (device_list, device1);

	/* add duplicate */
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_device_list_add (device_list, device1);
	g_assert_false (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));

	/* not waiting */
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* waiting */
	helper.device_old = device1;
	helper.device_new = device2;
	helper.device_list = device_list;
	g_timeout_add (100, fu_device_list_remove_cb, &helper);
	g_timeout_add (200, fu_device_list_add_cb, &helper);
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	ret = fu_device_list_wait_for_replug (device_list, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_false (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));

	/* should not be possible, but here we are */
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_device_list_add (device_list, device1);
	g_assert_false (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));
	g_assert_false (fu_device_has_flag (device2, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));

	/* add back the old device */
	fu_device_add_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_device_add_flag (device2, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG);
	fu_device_list_remove (device_list, device2);
	fu_device_list_add (device_list, device1);
	g_assert_false (fu_device_has_flag (device1, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));
	g_assert_false (fu_device_has_flag (device2, FWUPD_DEVICE_FLAG_WAIT_FOR_REPLUG));
}

static void
fu_device_list_compatible_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device_old = NULL;
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	g_autoptr(GPtrArray) devices_all = NULL;
	g_autoptr(GPtrArray) devices_active = NULL;
	FuDevice *device;
	guint added_cnt = 0;
	guint changed_cnt = 0;
	guint removed_cnt = 0;

	g_signal_connect (device_list, "added",
			  G_CALLBACK (_device_list_count_cb),
			  &added_cnt);
	g_signal_connect (device_list, "removed",
			  G_CALLBACK (_device_list_count_cb),
			  &removed_cnt);
	g_signal_connect (device_list, "changed",
			  G_CALLBACK (_device_list_count_cb),
			  &changed_cnt);

	/* add one device in runtime mode */
	fu_device_set_id (device1, "device1");
	fu_device_set_plugin (device1, "plugin-for-runtime");
	fu_device_add_vendor_id (device1, "USB:0x20A0");
	fu_device_set_version_format (device1, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device1, "1.2.3");
	fu_device_add_internal_flag (device1, FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_add_instance_id (device1, "foobar");
	fu_device_add_instance_id (device1, "bootloader");
	fu_device_set_remove_delay (device1, 100);
	fu_device_convert_instance_ids (device1);
	fu_device_list_add (device_list, device1);
	g_assert_cmpint (added_cnt, ==, 1);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 0);

	/* add another device in bootloader mode */
	fu_device_set_id (device2, "device2");
	fu_device_set_plugin (device2, "plugin-for-bootloader");
	fu_device_add_instance_id (device2, "bootloader");
	fu_device_add_internal_flag (device2, FU_DEVICE_INTERNAL_FLAG_REPLUG_MATCH_GUID);
	fu_device_convert_instance_ids (device2);

	/* verify only a changed event was generated */
	added_cnt = removed_cnt = changed_cnt = 0;
	fu_device_list_remove (device_list, device1);
	fu_device_list_add (device_list, device2);
	g_assert_cmpint (added_cnt, ==, 0);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 1);

	/* device2 should inherit the vendor ID and version from device1 */
	g_assert_true (fu_device_has_vendor_id (device2, "USB:0x20A0"));
	g_assert_cmpstr (fu_device_get_version (device2), ==, "1.2.3");

	/* one device is active */
	devices_active = fu_device_list_get_active (device_list);
	g_assert_cmpint (devices_active->len, ==, 1);
	device = g_ptr_array_index (devices_active, 0);
	g_assert_cmpstr (fu_device_get_id (device), ==,
			 "1a8d0d9a96ad3e67ba76cf3033623625dc6d6882");

	/* the list knows about both devices, list in order of active->old */
	devices_all = fu_device_list_get_all (device_list);
	g_assert_cmpint (devices_all->len, ==, 2);
	device = g_ptr_array_index (devices_all, 0);
	g_assert_cmpstr (fu_device_get_id (device), ==,
			 "1a8d0d9a96ad3e67ba76cf3033623625dc6d6882");
	device = g_ptr_array_index (devices_all, 1);
	g_assert_cmpstr (fu_device_get_id (device), ==,
			 "99249eb1bd9ef0b6e192b271a8cb6a3090cfec7a");

	/* verify we can get the old device from the new device */
	device_old = fu_device_list_get_old (device_list, device2);
	g_assert (device_old == device1);
}

static void
fu_device_list_remove_chain_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	g_autoptr(FuDevice) device_child = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device_parent = fu_device_new_with_context (ctx);

	guint added_cnt = 0;
	guint changed_cnt = 0;
	guint removed_cnt = 0;

	g_signal_connect (device_list, "added",
			  G_CALLBACK (_device_list_count_cb),
			  &added_cnt);
	g_signal_connect (device_list, "removed",
			  G_CALLBACK (_device_list_count_cb),
			  &removed_cnt);
	g_signal_connect (device_list, "changed",
			  G_CALLBACK (_device_list_count_cb),
			  &changed_cnt);

	/* add child */
	fu_device_set_id (device_child, "child");
	fu_device_add_instance_id (device_child, "child-GUID-1");
	fu_device_convert_instance_ids (device_child);
	fu_device_list_add (device_list, device_child);
	g_assert_cmpint (added_cnt, ==, 1);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 0);

	/* add parent */
	fu_device_set_id (device_parent, "parent");
	fu_device_add_instance_id (device_parent, "parent-GUID-1");
	fu_device_convert_instance_ids (device_parent);
	fu_device_add_child (device_parent, device_child);
	fu_device_list_add (device_list, device_parent);
	g_assert_cmpint (added_cnt, ==, 2);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 0);

	/* make sure that removing the parent causes both to go; but the child to go first */
	fu_device_list_remove (device_list, device_parent);
	g_assert_cmpint (added_cnt, ==, 2);
	g_assert_cmpint (removed_cnt, ==, 2);
	g_assert_cmpint (changed_cnt, ==, 0);
}

static void
fu_device_list_func (gconstpointer user_data)
{
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDeviceList) device_list = fu_device_list_new ();
	g_autoptr(FuDevice) device1 = fu_device_new_with_context (ctx);
	g_autoptr(FuDevice) device2 = fu_device_new_with_context (ctx);
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) devices2 = NULL;
	g_autoptr(GError) error = NULL;
	FuDevice *device;
	guint added_cnt = 0;
	guint changed_cnt = 0;
	guint removed_cnt = 0;

	g_signal_connect (device_list, "added",
			  G_CALLBACK (_device_list_count_cb),
			  &added_cnt);
	g_signal_connect (device_list, "removed",
			  G_CALLBACK (_device_list_count_cb),
			  &removed_cnt);
	g_signal_connect (device_list, "changed",
			  G_CALLBACK (_device_list_count_cb),
			  &changed_cnt);

	/* add both */
	fu_device_set_id (device1, "device1");
	fu_device_add_instance_id (device1, "foobar");
	fu_device_convert_instance_ids (device1);
	fu_device_list_add (device_list, device1);
	fu_device_set_id (device2, "device2");
	fu_device_add_instance_id (device2, "baz");
	fu_device_convert_instance_ids (device2);
	fu_device_list_add (device_list, device2);
	g_assert_cmpint (added_cnt, ==, 2);
	g_assert_cmpint (removed_cnt, ==, 0);
	g_assert_cmpint (changed_cnt, ==, 0);

	/* get all */
	devices = fu_device_list_get_all (device_list);
	g_assert_cmpint (devices->len, ==, 2);
	device = g_ptr_array_index (devices, 0);
	g_assert_cmpstr (fu_device_get_id (device), ==,
			 "99249eb1bd9ef0b6e192b271a8cb6a3090cfec7a");

	/* find by ID */
	device = fu_device_list_get_by_id (device_list,
					   "99249eb1bd9ef0b6e192b271a8cb6a3090cfec7a",
					   &error);
	g_assert_no_error (error);
	g_assert (device != NULL);
	g_assert_cmpstr (fu_device_get_id (device), ==,
					   "99249eb1bd9ef0b6e192b271a8cb6a3090cfec7a");
	g_clear_object (&device);

	/* find by GUID */
	device = fu_device_list_get_by_guid (device_list,
					     "579a3b1c-d1db-5bdc-b6b9-e2c1b28d5b8a",
					     &error);
	g_assert_no_error (error);
	g_assert (device != NULL);
	g_assert_cmpstr (fu_device_get_id (device), ==,
			 "1a8d0d9a96ad3e67ba76cf3033623625dc6d6882");
	g_clear_object (&device);

	/* find by missing GUID */
	device = fu_device_list_get_by_guid (device_list, "notfound", &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (device == NULL);

	/* remove device */
	added_cnt = removed_cnt = changed_cnt = 0;
	fu_device_list_remove (device_list, device1);
	g_assert_cmpint (added_cnt, ==, 0);
	g_assert_cmpint (removed_cnt, ==, 1);
	g_assert_cmpint (changed_cnt, ==, 0);
	devices2 = fu_device_list_get_all (device_list);
	g_assert_cmpint (devices2->len, ==, 1);
	device = g_ptr_array_index (devices2, 0);
	g_assert_cmpstr (fu_device_get_id (device), ==,
			 "1a8d0d9a96ad3e67ba76cf3033623625dc6d6882");
}

static void
fu_plugin_list_func (gconstpointer user_data)
{
	GPtrArray *plugins;
	FuPlugin *plugin;
	g_autoptr(FuPluginList) plugin_list = fu_plugin_list_new ();
	g_autoptr(FuPlugin) plugin1 = fu_plugin_new (NULL);
	g_autoptr(FuPlugin) plugin2 = fu_plugin_new (NULL);
	g_autoptr(GError) error = NULL;

	fu_plugin_set_name (plugin1, "plugin1");
	fu_plugin_set_name (plugin2, "plugin2");

	/* get all the plugins */
	fu_plugin_list_add (plugin_list, plugin1);
	fu_plugin_list_add (plugin_list, plugin2);
	plugins = fu_plugin_list_get_all (plugin_list);
	g_assert_cmpint (plugins->len, ==, 2);

	/* get a single plugin */
	plugin = fu_plugin_list_find_by_name (plugin_list, "plugin1", &error);
	g_assert_no_error (error);
	g_assert (plugin != NULL);
	g_assert_cmpstr (fu_plugin_get_name (plugin), ==, "plugin1");

	/* does not exist */
	plugin = fu_plugin_list_find_by_name (plugin_list, "nope", &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (plugin == NULL);
}

static void
fu_plugin_list_depsolve_func (gconstpointer user_data)
{
	GPtrArray *plugins;
	FuPlugin *plugin;
	gboolean ret;
	g_autoptr(FuPluginList) plugin_list = fu_plugin_list_new ();
	g_autoptr(FuPlugin) plugin1 = fu_plugin_new (NULL);
	g_autoptr(FuPlugin) plugin2 = fu_plugin_new (NULL);
	g_autoptr(GError) error = NULL;

	fu_plugin_set_name (plugin1, "plugin1");
	fu_plugin_set_name (plugin2, "plugin2");

	/* add rule then depsolve */
	fu_plugin_list_add (plugin_list, plugin1);
	fu_plugin_list_add (plugin_list, plugin2);
	fu_plugin_add_rule (plugin1, FU_PLUGIN_RULE_RUN_AFTER, "plugin2");
	ret = fu_plugin_list_depsolve (plugin_list, &error);
	g_assert_no_error (error);
	g_assert (ret);
	plugins = fu_plugin_list_get_all (plugin_list);
	g_assert_cmpint (plugins->len, ==, 2);
	plugin = g_ptr_array_index (plugins, 0);
	g_assert_cmpstr (fu_plugin_get_name (plugin), ==, "plugin2");
	g_assert_cmpint (fu_plugin_get_order (plugin), ==, 0);
	g_assert_false (fu_plugin_has_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED));

	/* add another rule, then re-depsolve */
	fu_plugin_add_rule (plugin1, FU_PLUGIN_RULE_CONFLICTS, "plugin2");
	ret = fu_plugin_list_depsolve (plugin_list, &error);
	g_assert_no_error (error);
	g_assert (ret);
	plugin = fu_plugin_list_find_by_name (plugin_list, "plugin1", &error);
	g_assert_no_error (error);
	g_assert (plugin != NULL);
	g_assert_false (fu_plugin_has_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED));
	plugin = fu_plugin_list_find_by_name (plugin_list, "plugin2", &error);
	g_assert_no_error (error);
	g_assert (plugin != NULL);
	g_assert_true (fu_plugin_has_flag (plugin, FWUPD_PLUGIN_FLAG_DISABLED));
}

static void
fu_history_migrate_func (gconstpointer user_data)
{
	gboolean ret;
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) file_dst = NULL;
	g_autoptr(GFile) file_src = NULL;
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuHistory) history = NULL;
	g_autofree gchar *filename = NULL;

	/* load old version */
	filename = g_build_filename (TESTDATADIR_SRC, "history_v1.db", NULL);
	file_src = g_file_new_for_path (filename);
	file_dst = g_file_new_for_path ("/tmp/fwupd-self-test/var/lib/fwupd/pending.db");
	ret = g_file_copy (file_src, file_dst, G_FILE_COPY_OVERWRITE, NULL,
			   NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* create, migrating as required */
	history = fu_history_new ();
	g_assert (history != NULL);

	/* get device */
	device = fu_history_get_device_by_id (history, "2ba16d10df45823dd4494ff10a0bfccfef512c9d", &error);
	g_assert_no_error (error);
	g_assert (device != NULL);
	g_assert_cmpstr (fu_device_get_id (device), ==, "2ba16d10df45823dd4494ff10a0bfccfef512c9d");
}

static void
_plugin_status_changed_cb (FuDevice *device, GParamSpec *pspec, gpointer user_data)
{
	guint *cnt = (guint *) user_data;
	g_debug ("device %s now %s",
		 fu_device_get_id (device),
		 fwupd_status_to_string (fu_device_get_status (device)));
	(*cnt)++;
	fu_test_loop_quit ();
}

static void
_plugin_device_added_cb (FuPlugin *plugin, FuDevice *device, gpointer user_data)
{
	FuDevice **dev = (FuDevice **) user_data;
	*dev = g_object_ref (device);
	fu_test_loop_quit ();
}

static void
_plugin_device_register_cb (FuPlugin *plugin, FuDevice *device, gpointer user_data)
{
	/* fake being a daemon */
	fu_plugin_runner_device_register (plugin, device);
}

static void
fu_plugin_module_func (gconstpointer user_data)
{
	FuTest *self = (FuTest *) user_data;
	GError *error = NULL;
	FuDevice *device_tmp;
	FwupdRelease *release;
	gboolean ret;
	guint cnt = 0;
	g_autofree gchar *localstatedir = NULL;
	g_autofree gchar *mapped_file_fn = NULL;
	g_autofree gchar *pending_cap = NULL;
	g_autofree gchar *history_db = NULL;
	g_autoptr(FuDevice) device = NULL;
	g_autoptr(FuDevice) device2 = NULL;
	g_autoptr(FuDevice) device3 = NULL;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuHistory) history = NULL;
	g_autoptr(GBytes) blob_cab = NULL;
	g_autoptr(GMappedFile) mapped_file = NULL;
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* create a fake device */
	g_setenv ("FWUPD_PLUGIN_TEST", "registration", TRUE);
	ret = fu_plugin_runner_startup (self->plugin, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_signal_connect (self->plugin, "device-added",
			  G_CALLBACK (_plugin_device_added_cb),
			  &device);
	g_signal_connect (self->plugin, "device-register",
			  G_CALLBACK (_plugin_device_register_cb),
			  &device);
	ret = fu_plugin_runner_coldplug (self->plugin, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* check we did the right thing */
	g_assert (device != NULL);
	g_assert_cmpstr (fu_device_get_id (device), ==, "08d460be0f1f9f128413f816022a6439e0078018");
	g_assert_cmpstr (fu_device_get_version_lowest (device), ==, "1.2.0");
	g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.2");
	g_assert_cmpstr (fu_device_get_version_bootloader (device), ==, "0.1.2");
	g_assert_cmpstr (fu_device_get_guid_default (device), ==,
			 "b585990a-003e-5270-89d5-3705a17f9a43");
	g_assert_cmpstr (fu_device_get_name (device), ==,
			 "Integrated Webcam™");
	g_signal_handlers_disconnect_by_data (self->plugin, &device);

#ifdef _WIN32
	g_test_skip ("No offline update support on Windows");
	return;
#endif
	/* schedule an offline update */
	g_signal_connect (device, "notify::status",
			  G_CALLBACK (_plugin_status_changed_cb),
			  &cnt);
	mapped_file_fn = g_build_filename (TESTDATADIR_SRC, "colorhug", "firmware.bin", NULL);
	mapped_file = g_mapped_file_new (mapped_file_fn, FALSE, &error);
	g_assert_no_error (error);
	g_assert (mapped_file != NULL);
	blob_cab = g_mapped_file_get_bytes (mapped_file);
	release = fu_device_get_release_default (device);
	fwupd_release_set_version (release, "1.2.3");
	ret = fu_engine_schedule_update (engine, device, release, blob_cab,
					 FWUPD_INSTALL_FLAG_NONE, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (cnt, ==, 1);

	/* set on the current device */
	g_assert_true (fu_device_has_flag (device, FWUPD_DEVICE_FLAG_NEEDS_REBOOT));

	/* lets check the history */
	history = fu_history_new ();
	device2 = fu_history_get_device_by_id (history, fu_device_get_id (device), &error);
	g_assert_no_error (error);
	g_assert (device2 != NULL);
	g_assert_cmpint (fu_device_get_update_state (device2), ==, FWUPD_UPDATE_STATE_PENDING);
	g_assert_cmpstr (fu_device_get_update_error (device2), ==, NULL);
	g_assert_true (fu_device_has_flag (device2, FWUPD_DEVICE_FLAG_NEEDS_REBOOT));
	release = fu_device_get_release_default (device2);
	g_assert (release != NULL);
	g_assert_cmpstr (fwupd_release_get_filename (release), !=, NULL);
	g_assert_cmpstr (fwupd_release_get_version (release), ==, "1.2.3");

	/* save this; we'll need to delete it later */
	pending_cap = g_strdup (fwupd_release_get_filename (release));

	/* lets do this online */
	fu_engine_add_device (engine, device);
	fu_engine_add_plugin (engine, self->plugin);
	ret = fu_engine_install_blob (engine, device, blob_cab,
				      FWUPD_INSTALL_FLAG_NONE,
				      FWUPD_FEATURE_FLAG_NONE,
				      &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (cnt, ==, 4);

	/* check the new version */
	g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.3");
	g_assert_cmpstr (fu_device_get_version_bootloader (device), ==, "0.1.2");

	/* lets check the history */
	device3 = fu_history_get_device_by_id (history, fu_device_get_id (device), &error);
	g_assert_no_error (error);
	g_assert (device3 != NULL);
	g_assert_cmpint (fu_device_get_update_state (device3), ==, FWUPD_UPDATE_STATE_SUCCESS);
	g_assert_cmpstr (fu_device_get_update_error (device3), ==, NULL);

	/* get the status */
	device_tmp = fu_device_new ();
	fu_device_set_id (device_tmp, "FakeDevice");
	ret = fu_plugin_runner_get_results (self->plugin, device_tmp, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fu_device_get_update_state (device_tmp), ==, FWUPD_UPDATE_STATE_SUCCESS);
	g_assert_cmpstr (fu_device_get_update_error (device_tmp), ==, NULL);

	/* clear */
	ret = fu_plugin_runner_clear_results (self->plugin, device_tmp, &error);
	g_assert_no_error (error);
	g_assert (ret);

	g_object_unref (device_tmp);
	g_clear_error (&error);

	/* delete files */
	localstatedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	history_db = g_build_filename (localstatedir, "pending.db", NULL);
	g_unlink (history_db);
	g_unlink (pending_cap);
}

static void
fu_history_func (gconstpointer user_data)
{
	GError *error = NULL;
	GPtrArray *checksums;
	gboolean ret;
	FuDevice *device;
	FwupdRelease *release;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuDevice) device_found = NULL;
	g_autoptr(FuHistory) history = NULL;
	g_autoptr(GPtrArray) approved_firmware = NULL;
	g_autofree gchar *dirname = NULL;
	g_autofree gchar *filename = NULL;

	/* create */
	history = fu_history_new ();
	g_assert (history != NULL);

	/* delete the database */
	dirname = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	if (!g_file_test (dirname, G_FILE_TEST_IS_DIR))
		return;
	filename = g_build_filename (dirname, "pending.db", NULL);
	g_unlink (filename);

	/* add a device */
	device = fu_device_new_with_context (ctx);
	fu_device_set_id (device, "self-test");
	fu_device_set_name (device, "ColorHug"),
	fu_device_set_version_format (device, FWUPD_VERSION_FORMAT_TRIPLET);
	fu_device_set_version (device, "3.0.1"),
	fu_device_set_update_state (device, FWUPD_UPDATE_STATE_FAILED);
	fu_device_set_update_error (device, "word");
	fu_device_add_guid (device, "827edddd-9bb6-5632-889f-2c01255503da");
	fu_device_add_flag (device, FWUPD_DEVICE_FLAG_INTERNAL);
	fu_device_set_created (device, 123);
	fu_device_set_modified (device, 456);
	release = fwupd_release_new ();
	fwupd_release_set_filename (release, "/var/lib/dave.cap"),
	fwupd_release_add_checksum (release, "abcdef");
	fwupd_release_set_version (release, "3.0.2");
	fwupd_release_add_metadata_item (release, "FwupdVersion", VERSION);
	ret = fu_history_add_device (history, device, release, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_object_unref (release);

	/* ensure database was created */
	g_assert (g_file_test (filename, G_FILE_TEST_EXISTS));

	g_object_unref (device);

	/* get device */
	device = fu_history_get_device_by_id (history, "2ba16d10df45823dd4494ff10a0bfccfef512c9d", &error);
	g_assert_no_error (error);
	g_assert (device != NULL);
	g_assert_cmpstr (fu_device_get_id (device), ==, "2ba16d10df45823dd4494ff10a0bfccfef512c9d");
	g_assert_cmpstr (fu_device_get_name (device), ==, "ColorHug");
	g_assert_cmpstr (fu_device_get_version (device), ==, "3.0.1");
	g_assert_cmpint (fu_device_get_update_state (device), ==, FWUPD_UPDATE_STATE_FAILED);
	g_assert_cmpstr (fu_device_get_update_error (device), ==, "word");
	g_assert_cmpstr (fu_device_get_guid_default (device), ==, "827edddd-9bb6-5632-889f-2c01255503da");
	g_assert_cmpint (fu_device_get_flags (device), ==, FWUPD_DEVICE_FLAG_INTERNAL | FWUPD_DEVICE_FLAG_HISTORICAL);
	g_assert_cmpint (fu_device_get_created (device), ==, 123);
	g_assert_cmpint (fu_device_get_modified (device), ==, 456);
	release = fu_device_get_release_default (device);
	g_assert (release != NULL);
	g_assert_cmpstr (fwupd_release_get_version (release), ==, "3.0.2");
	g_assert_cmpstr (fwupd_release_get_filename (release), ==, "/var/lib/dave.cap");
	g_assert_cmpstr (fwupd_release_get_metadata_item (release, "FwupdVersion"), ==, VERSION);
	checksums = fwupd_release_get_checksums (release);
	g_assert (checksums != NULL);
	g_assert_cmpint (checksums->len, ==, 1);
	g_assert_cmpstr (fwupd_checksum_get_by_kind (checksums, G_CHECKSUM_SHA1), ==, "abcdef");
	ret = fu_history_add_device (history, device, release, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* get device that does not exist */
	device_found = fu_history_get_device_by_id (history, "XXXXXXXXXXXXX", &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (device_found == NULL);
	g_clear_error (&error);

	/* get device that does exist */
	device_found = fu_history_get_device_by_id (history, "2ba16d10df45823dd4494ff10a0bfccfef512c9d", &error);
	g_assert_no_error (error);
	g_assert (device_found != NULL);
	g_object_unref (device_found);

	/* remove device */
	ret = fu_history_remove_device (history, device, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_object_unref (device);

	/* get device that does not exist */
	device_found = fu_history_get_device_by_id (history, "2ba16d10df45823dd4494ff10a0bfccfef512c9d", &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (device_found == NULL);
	g_clear_error (&error);

	/* approved firmware */
	ret = fu_history_clear_approved_firmware (history, &error);
	g_assert_no_error (error);
	g_assert (ret);
	ret = fu_history_add_approved_firmware (history, "foo", &error);
	g_assert_no_error (error);
	g_assert (ret);
	ret = fu_history_add_approved_firmware (history, "bar", &error);
	g_assert_no_error (error);
	g_assert (ret);
	approved_firmware = fu_history_get_approved_firmware (history, &error);
	g_assert_no_error (error);
	g_assert_nonnull (approved_firmware);
	g_assert_cmpint (approved_firmware->len, ==, 2);
	g_assert_cmpstr (g_ptr_array_index (approved_firmware, 0), ==, "foo");
	g_assert_cmpstr (g_ptr_array_index (approved_firmware, 1), ==, "bar");
}

static GBytes *
_build_cab (GCabCompression compression, ...)
{
	gboolean ret;
	va_list args;
	g_autoptr(GCabCabinet) cabinet = NULL;
	g_autoptr(GCabFolder) cabfolder = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GOutputStream) op = NULL;

	/* create a new archive */
	cabinet = gcab_cabinet_new ();
	cabfolder = gcab_folder_new (compression);
	ret = gcab_cabinet_add_folder (cabinet, cabfolder, &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* add each file */
	va_start (args, compression);
	do {
		const gchar *fn;
		const gchar *text;
		g_autoptr(GCabFile) cabfile = NULL;
		g_autoptr(GBytes) blob = NULL;

		/* get filename */
		fn = va_arg (args, const gchar *);
		if (fn == NULL)
			break;

		/* get contents */
		text = va_arg (args, const gchar *);
		if (text == NULL)
			break;
		g_debug ("creating %s with %s", fn, text);

		/* add a GCabFile to the cabinet */
		blob = g_bytes_new_static (text, strlen (text));
		cabfile = gcab_file_new_with_bytes (fn, blob);
		ret = gcab_folder_add_file (cabfolder, cabfile, FALSE, NULL, &error);
		g_assert_no_error (error);
		g_assert (ret);
	} while (TRUE);
	va_end (args);

	/* write the archive to a blob */
	op = g_memory_output_stream_new_resizable ();
	ret = gcab_cabinet_write_simple  (cabinet, op, NULL, NULL, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);
	ret = g_output_stream_close (op, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);
	return g_memory_output_stream_steal_as_bytes (G_MEMORY_OUTPUT_STREAM (op));
}

static void
_plugin_composite_device_added_cb (FuPlugin *plugin, FuDevice *device, gpointer user_data)
{
	GPtrArray *devices = (GPtrArray *) user_data;
	g_ptr_array_add (devices, g_object_ref (device));
}

static void
fu_plugin_composite_func (gconstpointer user_data)
{
	FuTest *self = (FuTest *) user_data;
	GError *error = NULL;
	gboolean ret;
	g_autoptr(FuEngine) engine = fu_engine_new (FU_APP_FLAGS_NONE);
	g_autoptr(FuEngineRequest) request = fu_engine_request_new ();
	g_autoptr(GBytes) blob = NULL;
	g_autoptr(GPtrArray) components = NULL;
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) install_tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	g_autoptr(XbSilo) silo_empty = xb_silo_new ();
	g_autoptr(XbSilo) silo = NULL;

	/* no metadata in daemon */
	fu_engine_set_silo (engine, silo_empty);

	/* create CAB file */
	blob = _build_cab (GCAB_COMPRESSION_NONE,
			   "acme.metainfo.xml",
	"<component type=\"firmware\">\n"
	"  <id>com.acme.example.firmware</id>\n"
	"  <provides>\n"
	"    <firmware type=\"flashed\">b585990a-003e-5270-89d5-3705a17f9a43</firmware>\n"
	"  </provides>\n"
	"  <releases>\n"
	"    <release version=\"1.2.3\"/>\n"
	"  </releases>\n"
	"</component>",
	"acme.module1.metainfo.xml",
	"<component type=\"firmware\">\n"
	"  <id>com.acme.example.firmware.module1</id>\n"
	"  <provides>\n"
	"    <firmware type=\"flashed\">7fddead7-12b5-4fb9-9fa0-6d30305df755</firmware>\n"
	"  </provides>\n"
	"  <releases>\n"
	"    <release version=\"2\"/>\n"
	"  </releases>\n"
	"  <custom>\n"
	"    <value key=\"LVFS::VersionFormat\">plain</value>\n"
	"  </custom>\n"
	"</component>",
	"acme.module2.metainfo.xml",
	"<component type=\"firmware\">\n"
	"  <id>com.acme.example.firmware.module2</id>\n"
	"  <provides>\n"
	"    <firmware type=\"flashed\">b8fe6b45-8702-4bcd-8120-ef236caac76f</firmware>\n"
	"  </provides>\n"
	"  <releases>\n"
	"    <release version=\"11\"/>\n"
	"  </releases>\n"
	"  <custom>\n"
	"    <value key=\"LVFS::VersionFormat\">plain</value>\n"
	"  </custom>\n"
	"</component>",
			   "firmware.bin", "world",
			   NULL);
	silo = fu_common_cab_build_silo (blob, 10240, &error);
	g_assert_no_error (error);
	g_assert_nonnull (silo);
	components = xb_silo_query (silo, "components/component", 0, &error);
	g_assert_no_error (error);
	g_assert_nonnull (components);
	g_assert_cmpint (components->len, ==, 3);

	/* set up dummy plugin */
	g_setenv ("FWUPD_PLUGIN_TEST", "composite", TRUE);
	fu_engine_add_plugin (engine, self->plugin);

	ret = fu_plugin_runner_startup (self->plugin, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	devices = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	g_signal_connect (self->plugin, "device-added",
			  G_CALLBACK (_plugin_composite_device_added_cb),
			  devices);

	ret = fu_plugin_runner_coldplug (self->plugin, &error);
	g_assert_no_error (error);
	g_assert_true (ret);

	/* check we found all composite devices  */
	g_assert_cmpint (devices->len, ==, 3);
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index (devices, i);
		fu_engine_add_device (engine, device);
		if (g_strcmp0 (fu_device_get_id (device),
				"08d460be0f1f9f128413f816022a6439e0078018") == 0) {
			g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.2");
		} else if (g_strcmp0 (fu_device_get_id (device),
					"c0a0a4aa6480ac28eea1ce164fbb466ca934e1ff") == 0) {
			g_assert_cmpstr (fu_device_get_version (device), ==, "1");
			g_assert_nonnull (fu_device_get_parent (device));
		} else if (g_strcmp0 (fu_device_get_id (device),
					"bf455e9f371d2608d1cb67660fd2b335d3f6ef73") == 0) {
			g_assert_cmpstr (fu_device_get_version (device), ==, "10");
			g_assert_nonnull (fu_device_get_parent (device));
		}
	}

	/* produce install tasks */
	for (guint i = 0; i < components->len; i++) {
		XbNode *component = g_ptr_array_index (components, i);

		/* do any devices pass the requirements */
		for (guint j = 0; j < devices->len; j++) {
			FuDevice *device = g_ptr_array_index (devices, j);
			g_autoptr(FuInstallTask) task = NULL;
			g_autoptr(GError) error_local = NULL;

			/* is this component valid for the device */
			task = fu_install_task_new (device, component);
			if (!fu_engine_check_requirements (engine,
							   request,
							   task,
							   0,
							   &error_local)) {
				g_debug ("requirement on %s:%s failed: %s",
					 fu_device_get_id (device),
					 xb_node_query_text (component, "id", NULL),
					 error_local->message);
				continue;
			}

			g_ptr_array_add (install_tasks, g_steal_pointer (&task));
		}
	}
	g_assert_cmpint (install_tasks->len, ==, 3);

	/* install the cab */
	ret = fu_engine_install_tasks (engine,
				       request,
				       install_tasks,
				       blob,
				       FWUPD_DEVICE_FLAG_NONE,
				       &error);
	g_assert_no_error (error);
	g_assert_true (ret);

	/* verify everything upgraded */
	for (guint i = 0; i < devices->len; i++) {
		FuDevice *device = g_ptr_array_index (devices, i);
		const gchar *metadata;
		if (g_strcmp0 (fu_device_get_id (device),
				"08d460be0f1f9f128413f816022a6439e0078018") == 0) {
			g_assert_cmpstr (fu_device_get_version (device), ==, "1.2.3");
		} else if (g_strcmp0 (fu_device_get_id (device),
					"c0a0a4aa6480ac28eea1ce164fbb466ca934e1ff") == 0) {
			g_assert_cmpstr (fu_device_get_version (device), ==, "2");
		} else if (g_strcmp0 (fu_device_get_id (device),
					"bf455e9f371d2608d1cb67660fd2b335d3f6ef73") == 0) {
			g_assert_cmpstr (fu_device_get_version (device), ==, "11");
		}

		/* verify prepare and cleanup ran on all devices */
		metadata = fu_device_get_metadata (device, "frimbulator");
		g_assert_cmpstr (metadata, ==, "1");
		metadata = fu_device_get_metadata (device, "frombulator");
		g_assert_cmpstr (metadata, ==, "1");
	}
}

static void
fu_security_attr_func (gconstpointer user_data)
{
	g_autoptr(FwupdSecurityAttr) attr = fwupd_security_attr_new (NULL);
	for (guint i = 0; i < FWUPD_SECURITY_ATTR_RESULT_LAST; i++) {
		fwupd_security_attr_set_result (attr, i);
		g_assert_cmpstr (fu_security_attr_get_result (attr), !=, NULL);
	}
}

static void
fu_memcpy_func (gconstpointer user_data)
{
	const guint8 src[] = {'a', 'b', 'c', 'd', 'e' };
	gboolean ret;
	guint8 dst[4];
	g_autoptr(GError) error = NULL;

	/* copy entire buffer */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x0,
			      4, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	g_assert (memcmp (src, dst, 4) == 0);

	/* copy first char */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x0,
			      1, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	g_assert_cmpint (dst[0], ==, 'a');

	/* copy last char */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x4,
			      1, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	g_assert_cmpint (dst[0], ==, 'e');

	/* copy nothing */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x0,
			      0, &error);
	g_assert_no_error (error);
	g_assert_true (ret);

	/* write past the end of dst */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x0,
			      5, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_WRITE);
	g_assert_false (ret);
	g_clear_error (&error);

	/* write past the end of dst with offset */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x1,
			      src, sizeof(src), 0x0,
			      4, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_WRITE);
	g_assert_false (ret);
	g_clear_error (&error);

	/* read past past the end of dst */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x0,
			      6, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_READ);
	g_assert_false (ret);
	g_clear_error (&error);

	/* read past the end of src with offset */
	ret = fu_memcpy_safe (dst, sizeof(dst), 0x0,
			      src, sizeof(src), 0x4,
			      4, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_READ);
	g_assert_false (ret);
	g_clear_error (&error);
}

static void
fu_progressbar_func (gconstpointer user_data)
{
	g_autoptr(FuProgressbar) progressbar = fu_progressbar_new ();

	fu_progressbar_set_length_status (progressbar, 20);
	fu_progressbar_set_length_percentage (progressbar, 50);

	g_print ("\n");
	for (guint i = 0; i < 100; i++) {
		fu_progressbar_update (progressbar, FWUPD_STATUS_DECOMPRESSING, i);
		g_usleep (10000);
	}
	fu_progressbar_update (progressbar, FWUPD_STATUS_IDLE, 0);
	for (guint i = 0; i < 100; i++) {
		guint pc = (i > 25 && i < 75) ? 0 : i;
		fu_progressbar_update (progressbar, FWUPD_STATUS_LOADING, pc);
		g_usleep (10000);
	}
	fu_progressbar_update (progressbar, FWUPD_STATUS_IDLE, 0);

	for (guint i = 0; i < 5000; i++) {
		fu_progressbar_update (progressbar, FWUPD_STATUS_LOADING, 0);
		g_usleep (1000);
	}
	fu_progressbar_update (progressbar, FWUPD_STATUS_IDLE, 0);
}

static gint
fu_install_task_compare_func_cb (gconstpointer a, gconstpointer b)
{
	FuInstallTask *task_a = *((FuInstallTask **) a);
	FuInstallTask *task_b = *((FuInstallTask **) b);
	return fu_install_task_compare (task_a, task_b);
}

static void
fu_install_task_compare_func (gconstpointer user_data)
{
	FuDevice *device_tmp;
	g_autoptr(GPtrArray) install_tasks = NULL;
	g_autoptr(FuDevice) device1 = fu_device_new ();
	g_autoptr(FuDevice) device2 = fu_device_new ();
	g_autoptr(FuDevice) device3 = fu_device_new ();

	install_tasks = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

	fu_device_set_order (device1, 99);
	g_ptr_array_add (install_tasks, fu_install_task_new (device1, NULL));
	fu_device_set_order (device2, 11);
	g_ptr_array_add (install_tasks, fu_install_task_new (device2, NULL));
	fu_device_set_order (device3, 33);
	g_ptr_array_add (install_tasks, fu_install_task_new (device3, NULL));

	/* order the install tasks */
	g_ptr_array_sort (install_tasks, fu_install_task_compare_func_cb);
	g_assert_cmpint (install_tasks->len, ==, 3);
	device_tmp = fu_install_task_get_device (g_ptr_array_index (install_tasks, 0));
	g_assert_cmpint (fu_device_get_order (device_tmp), ==, 11);
	device_tmp = fu_install_task_get_device (g_ptr_array_index (install_tasks, 1));
	g_assert_cmpint (fu_device_get_order (device_tmp), ==, 33);
	device_tmp = fu_install_task_get_device (g_ptr_array_index (install_tasks, 2));
	g_assert_cmpint (fu_device_get_order (device_tmp), ==, 99);
}

int
main (int argc, char **argv)
{
	gboolean ret;
	g_autofree gchar *pluginfn = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(FuContext) ctx = fu_context_new ();
	g_autoptr(FuTest) self = g_new0 (FuTest, 1);

	g_test_init (&argc, &argv, NULL);

	/* only critical and error are fatal */
	g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
	g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);
	g_setenv ("FWUPD_DEVICE_LIST_VERBOSE", "1", TRUE);
	g_setenv ("FWUPD_DATADIR", TESTDATADIR_SRC, TRUE);
	g_setenv ("FWUPD_PLUGINDIR", TESTDATADIR_SRC, TRUE);
	g_setenv ("FWUPD_SYSCONFDIR", TESTDATADIR_SRC, TRUE);
	g_setenv ("FWUPD_SYSFSFWDIR", TESTDATADIR_SRC, TRUE);
	g_setenv ("FWUPD_OFFLINE_TRIGGER", "/tmp/fwupd-self-test/system-update", TRUE);
	g_setenv ("FWUPD_LOCALSTATEDIR", "/tmp/fwupd-self-test/var", TRUE);

	/* ensure empty tree */
	fu_self_test_mkroot ();

	/* load the test plugin */
	self->plugin = fu_plugin_new (ctx);
	pluginfn = g_build_filename (PLUGINBUILDDIR,
				     "libfu_plugin_test." G_MODULE_SUFFIX,
				     NULL);
	ret = fu_plugin_open (self->plugin, pluginfn, &error);
	g_assert_no_error (error);
	g_assert_true (ret);

	/* tests go here */
	if (g_test_slow ()) {
		g_test_add_data_func ("/fwupd/progressbar", self,
				      fu_progressbar_func);
	}
	g_test_add_data_func ("/fwupd/plugin{build-hash}", self,
			      fu_plugin_hash_func);
	g_test_add_data_func ("/fwupd/plugin{module}", self,
			      fu_plugin_module_func);
	g_test_add_data_func ("/fwupd/memcpy", self,
			      fu_memcpy_func);
	g_test_add_data_func ("/fwupd/security-attr", self,
			      fu_security_attr_func);
	g_test_add_data_func ("/fwupd/device-list", self,
			      fu_device_list_func);
	g_test_add_data_func ("/fwupd/device-list{delay}", self,
			      fu_device_list_delay_func);
	g_test_add_data_func ("/fwupd/device-list{no-auto-remove-children}", self,
			      fu_device_list_no_auto_remove_children_func);
	g_test_add_data_func ("/fwupd/device-list{compatible}", self,
			      fu_device_list_compatible_func);
	g_test_add_data_func ("/fwupd/device-list{remove-chain}", self,
			      fu_device_list_remove_chain_func);
	g_test_add_data_func ("/fwupd/install-task{compare}", self,
			      fu_install_task_compare_func);
	g_test_add_data_func ("/fwupd/engine{device-unlock}", self,
			      fu_engine_device_unlock_func);
	g_test_add_data_func ("/fwupd/engine{multiple-releases}", self,
			      fu_engine_multiple_rels_func);
	g_test_add_data_func ("/fwupd/engine{history-success}", self,
			      fu_engine_history_func);
	g_test_add_data_func ("/fwupd/engine{history-error}", self,
			      fu_engine_history_error_func);
	if (g_test_slow ()) {
		g_test_add_data_func ("/fwupd/device-list{replug-auto}", self,
				      fu_device_list_replug_auto_func);
	}
	g_test_add_data_func ("/fwupd/device-list{replug-user}", self,
			      fu_device_list_replug_user_func);
	g_test_add_data_func ("/fwupd/engine{require-hwid}", self,
			      fu_engine_require_hwid_func);
	g_test_add_data_func ("/fwupd/engine{history-inherit}", self,
			      fu_engine_history_inherit);
	g_test_add_data_func ("/fwupd/engine{partial-hash}", self,
			      fu_engine_partial_hash_func);
	g_test_add_data_func ("/fwupd/engine{downgrade}", self,
			      fu_engine_downgrade_func);
	g_test_add_data_func ("/fwupd/engine{requirements-success}", self,
			      fu_engine_requirements_func);
	g_test_add_data_func ("/fwupd/engine{requirements-soft}", self,
			      fu_engine_requirements_soft_func);
	g_test_add_data_func ("/fwupd/engine{requirements-missing}", self,
			      fu_engine_requirements_missing_func);
	g_test_add_data_func ("/fwupd/engine{requirements-client-fail}", self,
			      fu_engine_requirements_client_fail_func);
	g_test_add_data_func ("/fwupd/engine{requirements-client-invalid}", self,
			      fu_engine_requirements_client_invalid_func);
	g_test_add_data_func ("/fwupd/engine{requirements-client-pass}", self,
			      fu_engine_requirements_client_pass_func);
	g_test_add_data_func ("/fwupd/engine{requirements-version-require}", self,
			      fu_engine_requirements_version_require_func);
	g_test_add_data_func ("/fwupd/engine{requirements-parent-device}", self,
			      fu_engine_requirements_parent_device_func);
	g_test_add_data_func ("/fwupd/engine{requirements_protocol_check_func}", self,
			      fu_engine_requirements_protocol_check_func);
	g_test_add_data_func ("/fwupd/engine{requirements-not-child}", self,
			      fu_engine_requirements_child_func);
	g_test_add_data_func ("/fwupd/engine{requirements-not-child-fail}", self,
			      fu_engine_requirements_child_fail_func);
	g_test_add_data_func ("/fwupd/engine{requirements-unsupported}", self,
			      fu_engine_requirements_unsupported_func);
	g_test_add_data_func ("/fwupd/engine{requirements-device}", self,
			      fu_engine_requirements_device_func);
	g_test_add_data_func ("/fwupd/engine{requirements-device-plain}", self,
			      fu_engine_requirements_device_plain_func);
	g_test_add_data_func ("/fwupd/engine{requirements-version-format}", self,
			      fu_engine_requirements_version_format_func);
	g_test_add_data_func ("/fwupd/engine{device-auto-parent-id}", self,
			      fu_engine_device_parent_id_func);
	g_test_add_data_func ("/fwupd/engine{device-auto-parent-guid}", self,
			      fu_engine_device_parent_guid_func);
	g_test_add_data_func ("/fwupd/engine{install-duration}", self,
			      fu_engine_install_duration_func);
	g_test_add_data_func ("/fwupd/engine{generate-md}", self,
			      fu_engine_generate_md_func);
	g_test_add_data_func ("/fwupd/engine{requirements-other-device}", self,
			      fu_engine_requirements_other_device_func);
	g_test_add_data_func ("/fwupd/engine{fu_engine_requirements_sibling_device_func}", self,
			      fu_engine_requirements_sibling_device_func);
	g_test_add_data_func ("/fwupd/plugin{composite}", self,
			      fu_plugin_composite_func);
	g_test_add_data_func ("/fwupd/history", self,
			      fu_history_func);
	g_test_add_data_func ("/fwupd/history{migrate}", self,
			      fu_history_migrate_func);
	g_test_add_data_func ("/fwupd/plugin-list", self,
			      fu_plugin_list_func);
	g_test_add_data_func ("/fwupd/plugin-list{depsolve}", self,
			      fu_plugin_list_depsolve_func);
	return g_test_run ();
}
