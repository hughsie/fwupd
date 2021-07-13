/*
 * Copyright (C) 2016 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#include "config.h"

#include <glib-object.h>
#include <string.h>
#ifdef HAVE_FNMATCH_H
#include <fnmatch.h>
#endif

#include "fwupd-client.h"
#include "fwupd-client-sync.h"
#include "fwupd-common.h"
#include "fwupd-enums.h"
#include "fwupd-error.h"
#include "fwupd-device-private.h"
#include "fwupd-release-private.h"
#include "fwupd-remote-private.h"

static gboolean
fu_test_compare_lines (const gchar *txt1, const gchar *txt2, GError **error)
{
	g_autofree gchar *output = NULL;

	/* exactly the same */
	if (g_strcmp0 (txt1, txt2) == 0)
		return TRUE;

	/* matches a pattern */
#ifdef HAVE_FNMATCH_H
	if (fnmatch (txt2, txt1, FNM_NOESCAPE) == 0)
		return TRUE;
#else
	if (g_strcmp0 (txt1, txt2) == 0)
		return TRUE;
#endif

	/* save temp files and diff them */
	if (!g_file_set_contents ("/tmp/a", txt1, -1, error))
		return FALSE;
	if (!g_file_set_contents ("/tmp/b", txt2, -1, error))
		return FALSE;
	if (!g_spawn_command_line_sync ("diff -urNp /tmp/b /tmp/a",
					&output, NULL, NULL, error))
		return FALSE;

	/* just output the diff */
	g_set_error_literal (error, 1, 0, output);
	return FALSE;
}

/* https://gitlab.gnome.org/GNOME/glib/issues/225 */
static guint
_g_string_replace (GString *string, const gchar *search, const gchar *replace)
{
	gchar *tmp;
	guint count = 0;
	gsize search_idx = 0;
	gsize replace_len;
	gsize search_len;

	g_return_val_if_fail (string != NULL, 0);
	g_return_val_if_fail (search != NULL, 0);
	g_return_val_if_fail (replace != NULL, 0);

	/* nothing to do */
	if (string->len == 0)
		return 0;

	search_len = strlen (search);
	replace_len = strlen (replace);

	do {
		tmp = g_strstr_len (string->str + search_idx, -1, search);
		if (tmp == NULL)
			break;

		/* advance the counter in case @replace contains @search */
		search_idx = (gsize) (tmp - string->str);

		/* reallocate the string if required */
		if (search_len > replace_len) {
			g_string_erase (string,
					(gssize) search_idx,
					(gssize) (search_len - replace_len));
			memcpy (tmp, replace, replace_len);
		} else if (search_len < replace_len) {
			g_string_insert_len (string,
					     (gssize) search_idx,
					     replace,
					     (gssize) (replace_len - search_len));
			/* we have to treat this specially as it could have
			 * been reallocated when the insertion happened */
			memcpy (string->str + search_idx, replace, replace_len);
		} else {
			/* just memcmp in the new string */
			memcpy (tmp, replace, replace_len);
		}
		search_idx += replace_len;
		count++;
	} while (TRUE);

	return count;
}

static void
fwupd_enums_func (void)
{
	/* enums */
	for (guint i = 0; i < FWUPD_ERROR_LAST; i++) {
		const gchar *tmp = fwupd_error_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_error_from_string (tmp), ==, i);
	}
	for (guint i = 0; i < FWUPD_STATUS_LAST; i++) {
		const gchar *tmp = fwupd_status_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_status_from_string (tmp), ==, i);
	}
	for (guint i = 0; i < FWUPD_UPDATE_STATE_LAST; i++) {
		const gchar *tmp = fwupd_update_state_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_update_state_from_string (tmp), ==, i);
	}
	for (guint i = 0; i < FWUPD_TRUST_FLAG_LAST; i++) {
		const gchar *tmp = fwupd_trust_flag_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_trust_flag_from_string (tmp), ==, i);
	}
	for (guint i = 0; i < FWUPD_DEVICE_MESSAGE_KIND_LAST; i++) {
		const gchar *tmp = fwupd_device_message_kind_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_device_message_kind_from_string (tmp), ==, i);
	}
	for (guint i = FWUPD_RELEASE_URGENCY_UNKNOWN + 1; i < FWUPD_RELEASE_URGENCY_LAST; i++) {
		const gchar *tmp = fwupd_release_urgency_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_release_urgency_from_string (tmp), ==, i);
	}
	for (guint i = 1; i < FWUPD_VERSION_FORMAT_LAST; i++) {
		const gchar *tmp = fwupd_version_format_to_string (i);
		g_assert_cmpstr (tmp, !=, NULL);
		g_assert_cmpint (fwupd_version_format_from_string (tmp), ==, i);
	}

	/* bitfield */
	for (guint64 i = 1; i < FWUPD_DEVICE_FLAG_UNKNOWN; i *= 2) {
		const gchar *tmp = fwupd_device_flag_to_string (i);
		if (tmp == NULL)
			break;
		g_assert_cmpint (fwupd_device_flag_from_string (tmp), ==, i);
	}
	for (guint64 i = 1; i < FWUPD_PLUGIN_FLAG_UNKNOWN; i *= 2) {
		const gchar *tmp = fwupd_plugin_flag_to_string (i);
		if (tmp == NULL)
			break;
		g_assert_cmpint (fwupd_plugin_flag_from_string (tmp), ==, i);
	}
}

static void
fwupd_remote_download_func (void)
{
	gboolean ret;
	g_autofree gchar *fn = NULL;
	g_autofree gchar *directory = NULL;
	g_autofree gchar *expected_metadata = NULL;
	g_autofree gchar *expected_signature = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GError) error = NULL;

	remote = fwupd_remote_new ();
	directory = g_build_filename (FWUPD_LOCALSTATEDIR,
				      "lib",
				      "fwupd",
				      "remotes.d",
				      NULL);
	expected_metadata = g_build_filename (FWUPD_LOCALSTATEDIR,
					      "lib",
					      "fwupd",
					      "remotes.d",
					      "lvfs",
					      "metadata.xml.gz",
					      NULL);
	expected_signature = g_strdup_printf ("%s.jcat", expected_metadata);
	fwupd_remote_set_remotes_dir (remote, directory);
	fn = g_build_filename (FU_SELF_TEST_REMOTES_DIR, "remotes.d", "lvfs.conf", NULL);
	ret = fwupd_remote_load_from_filename (remote, fn, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);
	ret = fwupd_remote_setup (remote, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	g_assert_cmpint (fwupd_remote_get_kind (remote), ==, FWUPD_REMOTE_KIND_DOWNLOAD);
	g_assert_cmpint (fwupd_remote_get_keyring_kind (remote), ==, FWUPD_KEYRING_KIND_JCAT);
	g_assert_cmpint (fwupd_remote_get_priority (remote), ==, 0);
	g_assert (fwupd_remote_get_enabled (remote));
	g_assert (fwupd_remote_get_metadata_uri (remote) != NULL);
	g_assert (fwupd_remote_get_metadata_uri_sig (remote) != NULL);
	g_assert_cmpstr (fwupd_remote_get_title (remote), ==, "Linux Vendor Firmware Service");
	g_assert_cmpstr (fwupd_remote_get_report_uri (remote), ==, "https://fwupd.org/lvfs/firmware/report");
	g_assert_cmpstr (fwupd_remote_get_filename_cache (remote), ==, expected_metadata);
	g_assert_cmpstr (fwupd_remote_get_filename_cache_sig (remote), ==, expected_signature);
}

/* verify we used the FirmwareBaseURI just for firmware */
static void
fwupd_remote_baseuri_func (void)
{
	gboolean ret;
	g_autofree gchar *firmware_uri = NULL;
	g_autofree gchar *fn = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autofree gchar *directory = NULL;
	g_autoptr(GError) error = NULL;

	remote = fwupd_remote_new ();
	directory = g_build_filename (FWUPD_LOCALSTATEDIR,
				      "lib",
				      "fwupd",
				      "remotes.d",
				      NULL);
	fwupd_remote_set_remotes_dir (remote, directory);
	fn = g_build_filename (TESTDATADIR, "tests", "firmware-base-uri.conf", NULL);
	ret = fwupd_remote_load_from_filename (remote, fn, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fwupd_remote_get_kind (remote), ==, FWUPD_REMOTE_KIND_DOWNLOAD);
	g_assert_cmpint (fwupd_remote_get_keyring_kind (remote), ==, FWUPD_KEYRING_KIND_JCAT);
	g_assert_cmpint (fwupd_remote_get_priority (remote), ==, 0);
	g_assert (fwupd_remote_get_enabled (remote));
	g_assert_cmpstr (fwupd_remote_get_checksum (remote), ==, NULL);
	g_assert_cmpstr (fwupd_remote_get_metadata_uri (remote), ==,
			 "https://s3.amazonaws.com/lvfsbucket/downloads/firmware.xml.gz");
	g_assert_cmpstr (fwupd_remote_get_metadata_uri_sig (remote), ==,
			 "https://s3.amazonaws.com/lvfsbucket/downloads/firmware.xml.gz.jcat");
	firmware_uri = fwupd_remote_build_firmware_uri (remote, "http://bbc.co.uk/firmware.cab", &error);
	g_assert_no_error (error);
	g_assert_cmpstr (firmware_uri, ==, "https://my.fancy.cdn/firmware.cab");
}

static void
fwupd_remote_duplicate_func (void)
{
	gboolean ret;
	g_autofree gchar *fn2 = NULL;
	g_autofree gchar *fn = NULL;
	g_autoptr(FwupdRemote) remote = fwupd_remote_new ();
	g_autoptr(GError) error = NULL;

	fn = g_build_filename (TESTDATADIR, "tests", "remotes.d", "stable.conf", NULL);
	ret = fwupd_remote_load_from_filename (remote, fn, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	fn2 = g_build_filename (TESTDATADIR, "tests", "disabled.conf", NULL);
	ret = fwupd_remote_load_from_filename (remote, fn2, NULL, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	ret = fwupd_remote_setup (remote, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	ret = fwupd_remote_setup (remote, &error);
	g_assert_no_error (error);
	g_assert_true (ret);
	g_assert_false (fwupd_remote_get_enabled (remote));
	g_assert_cmpint (fwupd_remote_get_keyring_kind (remote), ==, FWUPD_KEYRING_KIND_NONE);
	g_assert_cmpstr (fwupd_remote_get_username (remote), ==, NULL);
	g_assert_cmpstr (fwupd_remote_get_password (remote), ==, "");
	g_assert_cmpstr (fwupd_remote_get_filename_cache (remote), ==, "/tmp/fwupd-self-test/stable.xml");
}

/* verify we used the metadata path for firmware */
static void
fwupd_remote_nopath_func (void)
{
	gboolean ret;
	g_autofree gchar *firmware_uri = NULL;
	g_autofree gchar *fn = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *directory = NULL;

	remote = fwupd_remote_new ();
	directory = g_build_filename (FWUPD_LOCALSTATEDIR,
				      "lib",
				      "fwupd",
				      "remotes.d",
				      NULL);
	fwupd_remote_set_remotes_dir (remote, directory);
	fn = g_build_filename (TESTDATADIR, "tests", "firmware-nopath.conf", NULL);
	ret = fwupd_remote_load_from_filename (remote, fn, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fwupd_remote_get_kind (remote), ==, FWUPD_REMOTE_KIND_DOWNLOAD);
	g_assert_cmpint (fwupd_remote_get_keyring_kind (remote), ==, FWUPD_KEYRING_KIND_JCAT);
	g_assert_cmpint (fwupd_remote_get_priority (remote), ==, 0);
	g_assert (fwupd_remote_get_enabled (remote));
	g_assert_cmpstr (fwupd_remote_get_checksum (remote), ==, NULL);
	g_assert_cmpstr (fwupd_remote_get_metadata_uri (remote), ==,
			 "https://s3.amazonaws.com/lvfsbucket/downloads/firmware.xml.gz");
	g_assert_cmpstr (fwupd_remote_get_metadata_uri_sig (remote), ==,
			 "https://s3.amazonaws.com/lvfsbucket/downloads/firmware.xml.gz.jcat");
	firmware_uri = fwupd_remote_build_firmware_uri (remote, "firmware.cab", &error);
	g_assert_no_error (error);
	g_assert_cmpstr (firmware_uri, ==, "https://s3.amazonaws.com/lvfsbucket/downloads/firmware.cab");
}

static void
fwupd_remote_local_func (void)
{
	gboolean ret;
	g_autofree gchar *fn = NULL;
	g_autoptr(FwupdRemote) remote = NULL;
	g_autoptr(GError) error = NULL;

	remote = fwupd_remote_new ();
	fn = g_build_filename (FU_LOCAL_REMOTE_DIR, "dell-esrt.conf", NULL);
	ret = fwupd_remote_load_from_filename (remote, fn, NULL, &error);
	g_assert_no_error (error);
	g_assert (ret);
	g_assert_cmpint (fwupd_remote_get_kind (remote), ==, FWUPD_REMOTE_KIND_LOCAL);
	g_assert_cmpint (fwupd_remote_get_keyring_kind (remote), ==, FWUPD_KEYRING_KIND_NONE);
	g_assert (fwupd_remote_get_enabled (remote));
	g_assert (fwupd_remote_get_metadata_uri (remote) == NULL);
	g_assert (fwupd_remote_get_metadata_uri_sig (remote) == NULL);
	g_assert (fwupd_remote_get_report_uri (remote) == NULL);
	g_assert_cmpstr (fwupd_remote_get_title (remote), ==, "Enable UEFI capsule updates on Dell systems");
	g_assert_cmpstr (fwupd_remote_get_filename_cache (remote), ==, "@datadir@/fwupd/remotes.d/dell-esrt/metadata.xml");
	g_assert_cmpstr (fwupd_remote_get_filename_cache_sig (remote), ==, NULL);
	g_assert_cmpstr (fwupd_remote_get_checksum (remote), ==, NULL);
}

static void
fwupd_release_func (void)
{
	g_autoptr(FwupdRelease) release1 = NULL;
	g_autoptr(FwupdRelease) release2 = NULL;
	g_autoptr(GVariant) data = NULL;

	release1 = fwupd_release_new ();
	fwupd_release_add_metadata_item (release1, "foo", "bar");
	fwupd_release_add_metadata_item (release1, "baz", "bam");
	data = fwupd_release_to_variant (release1);
	release2 = fwupd_release_from_variant (data);
	g_assert_cmpstr (fwupd_release_get_metadata_item (release2, "foo"), ==, "bar");
	g_assert_cmpstr (fwupd_release_get_metadata_item (release2, "baz"), ==, "bam");
}

static void
fwupd_device_func (void)
{
	gboolean ret;
	g_autofree gchar *data = NULL;
	g_autofree gchar *str = NULL;
	g_autoptr(FwupdDevice) dev = NULL;
	g_autoptr(FwupdRelease) rel = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GString) str_ascii = NULL;
	g_autoptr(JsonBuilder) builder = NULL;
	g_autoptr(JsonGenerator) json_generator = NULL;
	g_autoptr(JsonNode) json_root = NULL;

	/* create dummy object */
	dev = fwupd_device_new ();
	fwupd_device_add_checksum (dev, "beefdead");
	fwupd_device_set_created (dev, 1);
	fwupd_device_add_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE);
	fwupd_device_set_id (dev, "USB:foo");
	fwupd_device_set_modified (dev, 60 * 60 * 24);
	fwupd_device_set_name (dev, "ColorHug2");
	fwupd_device_add_guid (dev, "2082b5e0-7a64-478a-b1b2-e3404fab6dad");
	fwupd_device_add_guid (dev, "00000000-0000-0000-0000-000000000000");
	fwupd_device_add_icon (dev, "input-gaming");
	fwupd_device_add_icon (dev, "input-mouse");
	fwupd_device_add_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE |
				    FWUPD_DEVICE_FLAG_REQUIRE_AC);
	g_assert_true (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_REQUIRE_AC));
	g_assert_true (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_UPDATABLE));
	g_assert_false (fwupd_device_has_flag (dev, FWUPD_DEVICE_FLAG_HISTORICAL));
	rel = fwupd_release_new ();
	fwupd_release_add_flag (rel, FWUPD_RELEASE_FLAG_TRUSTED_PAYLOAD);
	fwupd_release_add_checksum (rel, "deadbeef");
	fwupd_release_set_description (rel, "<p>Hi there!</p>");
	fwupd_release_set_filename (rel, "firmware.bin");
	fwupd_release_set_appstream_id (rel, "org.dave.ColorHug.firmware");
	fwupd_release_set_size (rel, 1024);
	fwupd_release_add_location (rel, "http://foo.com");
	fwupd_release_add_location (rel, "ftp://foo.com");
	fwupd_release_set_version (rel, "1.2.3");
	fwupd_device_add_release (dev, rel);
	str = fwupd_device_to_string (dev);
	g_print ("\n%s", str);

	/* check GUIDs */
	g_assert (fwupd_device_has_guid (dev, "2082b5e0-7a64-478a-b1b2-e3404fab6dad"));
	g_assert (fwupd_device_has_guid (dev, "00000000-0000-0000-0000-000000000000"));
	g_assert (!fwupd_device_has_guid (dev, "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"));

	/* convert the new non-breaking space back into a normal space:
	 * https://gitlab.gnome.org/GNOME/glib/commit/76af5dabb4a25956a6c41a75c0c7feeee74496da */
	str_ascii = g_string_new (str);
	_g_string_replace (str_ascii, " ", " ");
	ret = fu_test_compare_lines (str_ascii->str,
		"ColorHug2\n"
		"  DeviceId:             USB:foo\n"
		"  Guid:                 2082b5e0-7a64-478a-b1b2-e3404fab6dad\n"
		"  Guid:                 00000000-0000-0000-0000-000000000000\n"
		"  Flags:                updatable|require-ac\n"
		"  Checksum:             SHA1(beefdead)\n"
		"  Icon:                 input-gaming,input-mouse\n"
		"  Created:              1970-01-01\n"
		"  Modified:             1970-01-02\n"
		"  \n"
		"  [Release]\n"
		"  AppstreamId:          org.dave.ColorHug.firmware\n"
		"  Description:          <p>Hi there!</p>\n"
		"  Version:              1.2.3\n"
		"  Filename:             firmware.bin\n"
		"  Checksum:             SHA1(deadbeef)\n"
		"  Size:                 1.0 kB\n"
		"  Uri:                  http://foo.com\n"
		"  Uri:                  ftp://foo.com\n"
		"  Flags:                trusted-payload\n", &error);
	g_assert_no_error (error);
	g_assert (ret);

	/* export to json */
	builder = json_builder_new ();
	json_builder_begin_object (builder);
	fwupd_device_to_json (dev, builder);
	json_builder_end_object (builder);
	json_root = json_builder_get_root (builder);
	json_generator = json_generator_new ();
	json_generator_set_pretty (json_generator, TRUE);
	json_generator_set_root (json_generator, json_root);
	data = json_generator_to_data (json_generator, NULL);
	g_assert_nonnull (data);
	ret = fu_test_compare_lines (data,
		"{\n"
		"  \"Name\" : \"ColorHug2\",\n"
		"  \"DeviceId\" : \"USB:foo\",\n"
		"  \"Guid\" : [\n"
		"    \"2082b5e0-7a64-478a-b1b2-e3404fab6dad\",\n"
		"    \"00000000-0000-0000-0000-000000000000\"\n"
		"  ],\n"
		"  \"Flags\" : [\n"
		"    \"updatable\",\n"
		"    \"require-ac\"\n"
		"  ],\n"
		"  \"Checksums\" : [\n"
		"    \"beefdead\"\n"
		"  ],\n"
		"  \"Icons\" : [\n"
		"    \"input-gaming\",\n"
		"    \"input-mouse\"\n"
		"  ],\n"
		"  \"Created\" : 1,\n"
		"  \"Modified\" : 86400,\n"
		"  \"Releases\" : [\n"
		"    {\n"
		"      \"AppstreamId\" : \"org.dave.ColorHug.firmware\",\n"
		"      \"Description\" : \"<p>Hi there!</p>\",\n"
		"      \"Version\" : \"1.2.3\",\n"
		"      \"Filename\" : \"firmware.bin\",\n"
		"      \"Checksum\" : [\n"
		"        \"deadbeef\"\n"
		"      ],\n"
		"      \"Size\" : 1024,\n"
		"      \"Locations\" : [\n"
		"        \"http://foo.com\",\n"
		"        \"ftp://foo.com\"\n"
		"      ],\n"
		"      \"Uri\" : \"http://foo.com\",\n"
		"      \"Flags\" : [\n"
		"        \"trusted-payload\"\n"
		"      ]\n"
		"    }\n"
		"  ]\n"
		"}", &error);
	g_assert_no_error (error);
	g_assert (ret);
}

static void
fwupd_client_devices_func (void)
{
	FwupdDevice *dev;
	gboolean ret;
	g_autoptr(FwupdClient) client = NULL;
	g_autoptr(GPtrArray) array = NULL;
	g_autoptr(GError) error = NULL;

	client = fwupd_client_new ();

	/* only run if running fwupd is new enough */
	ret = fwupd_client_connect (client, NULL, &error);
	if (ret == FALSE && g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT)) {
		g_debug ("%s", error->message);
		g_test_skip ("timeout connecting to daemon");
		return;
	}
	g_assert_no_error (error);
	g_assert_true (ret);
	if (fwupd_client_get_daemon_version (client) == NULL) {
		g_test_skip ("no enabled fwupd daemon");
		return;
	}
	if (!g_str_has_prefix (fwupd_client_get_daemon_version (client), "1.")) {
		g_test_skip ("running fwupd is too old");
		return;
	}

	array = fwupd_client_get_devices (client, NULL, &error);
	if (array == NULL &&
	    g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
		g_test_skip ("no available fwupd devices");
		return;
	}
	if (array == NULL &&
	    g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
		g_test_skip ("no available fwupd daemon");
		return;
	}
	g_assert_no_error (error);
	g_assert (array != NULL);
	g_assert_cmpint (array->len, >, 0);

	/* check device */
	dev = g_ptr_array_index (array, 0);
	g_assert (FWUPD_IS_DEVICE (dev));
	g_assert_cmpstr (fwupd_device_get_guid_default (dev), !=, NULL);
	g_assert_cmpstr (fwupd_device_get_id (dev), !=, NULL);
}

static void
fwupd_client_remotes_func (void)
{
	gboolean ret;
	g_autoptr(FwupdClient) client = NULL;
	g_autoptr(FwupdRemote) remote2 = NULL;
	g_autoptr(FwupdRemote) remote3 = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GPtrArray) array = NULL;

	g_setenv ("FU_SELF_TEST_REMOTES_DIR", FU_SELF_TEST_REMOTES_DIR, TRUE);

	client = fwupd_client_new ();

	/* only run if running fwupd is new enough */
	ret = fwupd_client_connect (client, NULL, &error);
	if (ret == FALSE && g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_TIMED_OUT)) {
		g_debug ("%s", error->message);
		g_test_skip ("timeout connecting to daemon");
		return;
	}
	g_assert_no_error (error);
	g_assert_true (ret);
	if (fwupd_client_get_daemon_version (client) == NULL) {
		g_test_skip ("no enabled fwupd daemon");
		return;
	}
	if (!g_str_has_prefix (fwupd_client_get_daemon_version (client), "1.")) {
		g_test_skip ("running fwupd is too old");
		return;
	}

	array = fwupd_client_get_remotes (client, NULL, &error);
	if (array == NULL &&
	    g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOTHING_TO_DO)) {
		g_test_skip ("no available fwupd remotes");
		return;
	}
	if (array == NULL &&
	    g_error_matches (error, FWUPD_ERROR, FWUPD_ERROR_NOT_SUPPORTED)) {
		g_test_skip ("no available fwupd daemon");
		return;
	}
	g_assert_no_error (error);
	g_assert (array != NULL);
	g_assert_cmpint (array->len, >, 0);

	/* check we can find the right thing */
	remote2 = fwupd_client_get_remote_by_id (client, "lvfs", NULL, &error);
	g_assert_no_error (error);
	g_assert (remote2 != NULL);
	g_assert_cmpstr (fwupd_remote_get_id (remote2), ==, "lvfs");
	g_assert (fwupd_remote_get_metadata_uri (remote2) != NULL);

	/* check we set an error when unfound */
	remote3 = fwupd_client_get_remote_by_id (client, "XXXX", NULL, &error);
	g_assert_error (error, FWUPD_ERROR, FWUPD_ERROR_NOT_FOUND);
	g_assert (remote3 == NULL);
}

static gboolean
fwupd_has_system_bus (void)
{
	g_autoptr(GDBusConnection) conn = NULL;
	conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, NULL);
	if (conn != NULL)
		return TRUE;
	g_debug ("D-Bus system bus unavailable, skipping tests.");
	return FALSE;
}

static void
fwupd_common_machine_hash_func (void)
{
	gsize sz = 0;
	g_autofree gchar *buf = NULL;
	g_autofree gchar *mhash1 = NULL;
	g_autofree gchar *mhash2 = NULL;
	g_autoptr(GError) error = NULL;

	if (!g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS)) {
		g_test_skip ("Missing /etc/machine-id");
		return;
	}
	if (!g_file_get_contents ("/etc/machine-id", &buf, &sz, &error)) {
		g_test_skip ("/etc/machine-id is unreadable");
		return;
	}

	if (sz == 0) {
		g_test_skip ("Empty /etc/machine-id");
		return;
	}

	mhash1 = fwupd_build_machine_id ("salt1", &error);
	g_assert_no_error (error);
	g_assert_cmpstr (mhash1, !=, NULL);
	mhash2 = fwupd_build_machine_id ("salt2", &error);
	g_assert_no_error (error);
	g_assert_cmpstr (mhash2, !=, NULL);
	g_assert_cmpstr (mhash2, !=, mhash1);
}

static void
fwupd_common_device_id_func (void)
{
	g_assert_false (fwupd_device_id_is_valid (NULL));
	g_assert_false (fwupd_device_id_is_valid (""));
	g_assert_false (fwupd_device_id_is_valid ("1ff60ab2-3905-06a1-b476-0371f00c9e9b"));
	g_assert_false (fwupd_device_id_is_valid ("aaaaaad3fae86d95e5d56626129d00e332c4b8dac95442"));
	g_assert_false (fwupd_device_id_is_valid ("x3fae86d95e5d56626129d00e332c4b8dac95442"));
	g_assert_false (fwupd_device_id_is_valid ("D3FAE86D95E5D56626129D00E332C4B8DAC95442"));
	g_assert_false (fwupd_device_id_is_valid (FWUPD_DEVICE_ID_ANY));
	g_assert_true (fwupd_device_id_is_valid ("d3fae86d95e5d56626129d00e332c4b8dac95442"));
}

static void
fwupd_common_guid_func (void)
{
	g_autofree gchar *guid1 = NULL;
	g_autofree gchar *guid2 = NULL;
	g_autofree gchar *guid_be = NULL;
	g_autofree gchar *guid_me = NULL;
	fwupd_guid_t buf = { 0x0 };
	gboolean ret;
	g_autoptr(GError) error = NULL;

	/* invalid */
	g_assert (!fwupd_guid_is_valid (NULL));
	g_assert (!fwupd_guid_is_valid (""));
	g_assert (!fwupd_guid_is_valid ("1ff60ab2-3905-06a1-b476"));
	g_assert (!fwupd_guid_is_valid ("1ff60ab2-XXXX-XXXX-XXXX-0371f00c9e9b"));
	g_assert (!fwupd_guid_is_valid (" 1ff60ab2-3905-06a1-b476-0371f00c9e9b"));
	g_assert (!fwupd_guid_is_valid ("00000000-0000-0000-0000-000000000000"));

	/* valid */
	g_assert (fwupd_guid_is_valid ("1ff60ab2-3905-06a1-b476-0371f00c9e9b"));

	/* make valid */
	guid1 = fwupd_guid_hash_string ("python.org");
	g_assert_cmpstr (guid1, ==, "886313e1-3b8a-5372-9b90-0c9aee199e5d");

	guid2 = fwupd_guid_hash_string ("8086:0406");
	g_assert_cmpstr (guid2, ==, "1fbd1f2c-80f4-5d7c-a6ad-35c7b9bd5486");

	/* round-trip BE */
	ret = fwupd_guid_from_string ("00112233-4455-6677-8899-aabbccddeeff", &buf,
				      FWUPD_GUID_FLAG_NONE, &error);
	g_assert_true (ret);
	g_assert_no_error (error);
	g_assert (memcmp (buf, "\x00\x11\x22\x33\x44\x55\x66\x77\x88\x99\xaa\xbb\xcc\xdd\xee\xff", sizeof(buf)) == 0);
	guid_be = fwupd_guid_to_string ((const fwupd_guid_t *) &buf, FWUPD_GUID_FLAG_NONE);
	g_assert_cmpstr (guid_be, ==, "00112233-4455-6677-8899-aabbccddeeff");

	/* round-trip mixed encoding */
	ret = fwupd_guid_from_string ("00112233-4455-6677-8899-aabbccddeeff", &buf,
				      FWUPD_GUID_FLAG_MIXED_ENDIAN, &error);
	g_assert_true (ret);
	g_assert_no_error (error);
	g_assert (memcmp (buf, "\x33\x22\x11\x00\x55\x44\x77\x66\x88\x99\xaa\xbb\xcc\xdd\xee\xff", sizeof(buf)) == 0);
	guid_me = fwupd_guid_to_string ((const fwupd_guid_t *) &buf, FWUPD_GUID_FLAG_MIXED_ENDIAN);
	g_assert_cmpstr (guid_me, ==, "00112233-4455-6677-8899-aabbccddeeff");

	/* check failure */
	g_assert_false (fwupd_guid_from_string ("001122334455-6677-8899-aabbccddeeff", NULL, 0, NULL));
	g_assert_false (fwupd_guid_from_string ("0112233-4455-6677-8899-aabbccddeeff", NULL, 0, NULL));
}

int
main (int argc, char **argv)
{
	g_test_init (&argc, &argv, NULL);

	/* only critical and error are fatal */
	g_log_set_fatal_mask (NULL, G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
	g_setenv ("G_MESSAGES_DEBUG", "all", TRUE);

	/* tests go here */
	g_test_add_func ("/fwupd/enums", fwupd_enums_func);
	g_test_add_func ("/fwupd/common{machine-hash}", fwupd_common_machine_hash_func);
	g_test_add_func ("/fwupd/common{device-id}", fwupd_common_device_id_func);
	g_test_add_func ("/fwupd/common{guid}", fwupd_common_guid_func);
	g_test_add_func ("/fwupd/release", fwupd_release_func);
	g_test_add_func ("/fwupd/device", fwupd_device_func);
	g_test_add_func ("/fwupd/remote{download}", fwupd_remote_download_func);
	g_test_add_func ("/fwupd/remote{base-uri}", fwupd_remote_baseuri_func);
	g_test_add_func ("/fwupd/remote{no-path}", fwupd_remote_nopath_func);
	g_test_add_func ("/fwupd/remote{local}", fwupd_remote_local_func);
	g_test_add_func ("/fwupd/remote{duplicate}", fwupd_remote_duplicate_func);
	if (fwupd_has_system_bus ()) {
		g_test_add_func ("/fwupd/client{remotes}", fwupd_client_remotes_func);
		g_test_add_func ("/fwupd/client{devices}", fwupd_client_devices_func);
	}
	return g_test_run ();
}
