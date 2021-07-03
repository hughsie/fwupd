/*
 * Copyright (C) 2017 Richard Hughes <richard@hughsie.com>
 *
 * SPDX-License-Identifier: LGPL-2.1+
 */

#define G_LOG_DOMAIN				"FuCommon"

#include <config.h>

#ifdef HAVE_GIO_UNIX
#include <gio/gunixinputstream.h>
#endif
#include <glib/gstdio.h>

#ifdef HAVE_KENV_H
#include <kenv.h>
#endif

#ifdef HAVE_CPUID_H
#include <cpuid.h>
#endif

#ifdef HAVE_UTSNAME_H
#include <sys/utsname.h>
#endif

#ifdef HAVE_LIBARCHIVE
#include <archive_entry.h>
#include <archive.h>
#endif
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "fwupd-error.h"

#include "fu-common-private.h"
#include "fu-common-version.h"
#include "fu-firmware.h"
#include "fu-volume-private.h"

/**
 * fu_common_rmtree:
 * @directory: a directory name
 * @error: (nullable): optional return location for an error
 *
 * Recursively removes a directory.
 *
 * Returns: %TRUE for success, %FALSE otherwise
 *
 * Since: 0.9.7
 **/
gboolean
fu_common_rmtree (const gchar *directory, GError **error)
{
	const gchar *filename;
	g_autoptr(GDir) dir = NULL;

	g_return_val_if_fail (directory != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* try to open */
	g_debug ("removing %s", directory);
	dir = g_dir_open (directory, 0, error);
	if (dir == NULL)
		return FALSE;

	/* find each */
	while ((filename = g_dir_read_name (dir))) {
		g_autofree gchar *src = NULL;
		src = g_build_filename (directory, filename, NULL);
		if (g_file_test (src, G_FILE_TEST_IS_DIR)) {
			if (!fu_common_rmtree (src, error))
				return FALSE;
		} else {
			if (g_unlink (src) != 0) {
				g_set_error (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INTERNAL,
					     "Failed to delete: %s", src);
				return FALSE;
			}
		}
	}
	if (g_remove (directory) != 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Failed to delete: %s", directory);
		return FALSE;
	}
	return TRUE;
}

static gboolean
fu_common_get_file_list_internal (GPtrArray *files, const gchar *directory, GError **error)
{
	const gchar *filename;
	g_autoptr(GDir) dir = NULL;

	/* try to open */
	dir = g_dir_open (directory, 0, error);
	if (dir == NULL)
		return FALSE;

	/* find each */
	while ((filename = g_dir_read_name (dir))) {
		g_autofree gchar *src = g_build_filename (directory, filename, NULL);
		if (g_file_test (src, G_FILE_TEST_IS_DIR)) {
			if (!fu_common_get_file_list_internal (files, src, error))
				return FALSE;
		} else {
			g_ptr_array_add (files, g_steal_pointer (&src));
		}
	}
	return TRUE;

}

/**
 * fu_common_get_files_recursive:
 * @path: a directory name
 * @error: (nullable): optional return location for an error
 *
 * Returns every file found under @directory, and any subdirectory.
 * If any path under @directory cannot be accessed due to permissions an error
 * will be returned.
 *
 * Returns: (transfer container) (element-type utf8): array of files, or %NULL for error
 *
 * Since: 1.0.6
 **/
GPtrArray *
fu_common_get_files_recursive (const gchar *path, GError **error)
{
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func (g_free);

	g_return_val_if_fail (path != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (!fu_common_get_file_list_internal (files, path, error))
		return NULL;
	return g_steal_pointer (&files);
}
/**
 * fu_common_mkdir_parent:
 * @filename: a full pathname
 * @error: (nullable): optional return location for an error
 *
 * Creates any required directories, including any parent directories.
 *
 * Returns: %TRUE for success
 *
 * Since: 0.9.7
 **/
gboolean
fu_common_mkdir_parent (const gchar *filename, GError **error)
{
	g_autofree gchar *parent = NULL;

	g_return_val_if_fail (filename != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	parent = g_path_get_dirname (filename);
	if (!g_file_test (parent, G_FILE_TEST_IS_DIR))
		g_debug ("creating path %s", parent);
	if (g_mkdir_with_parents (parent, 0755) == -1) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Failed to create '%s': %s",
			     parent, g_strerror (errno));
		return FALSE;
	}
	return TRUE;
}

/**
 * fu_common_set_contents_bytes:
 * @filename: a filename
 * @bytes: data to write
 * @error: (nullable): optional return location for an error
 *
 * Writes a blob of data to a filename, creating the parent directories as
 * required.
 *
 * Returns: %TRUE for success
 *
 * Since: 0.9.5
 **/
gboolean
fu_common_set_contents_bytes (const gchar *filename, GBytes *bytes, GError **error)
{
	const gchar *data;
	gsize size;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFile) file_parent = NULL;

	g_return_val_if_fail (filename != NULL, FALSE);
	g_return_val_if_fail (bytes != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	file = g_file_new_for_path (filename);
	file_parent = g_file_get_parent (file);
	if (!g_file_query_exists (file_parent, NULL)) {
		if (!g_file_make_directory_with_parents (file_parent, NULL, error))
			return FALSE;
	}
	data = g_bytes_get_data (bytes, &size);
	g_debug ("writing %s with %" G_GSIZE_FORMAT " bytes", filename, size);
	return g_file_set_contents (filename, data, size, error);
}

/**
 * fu_common_get_contents_bytes:
 * @filename: a filename
 * @error: (nullable): optional return location for an error
 *
 * Reads a blob of data from a file.
 *
 * Returns: a #GBytes, or %NULL for failure
 *
 * Since: 0.9.7
 **/
GBytes *
fu_common_get_contents_bytes (const gchar *filename, GError **error)
{
	gchar *data = NULL;
	gsize len = 0;

	g_return_val_if_fail (filename != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	if (!g_file_get_contents (filename, &data, &len, error))
		return NULL;
	g_debug ("reading %s with %" G_GSIZE_FORMAT " bytes", filename, len);
	return g_bytes_new_take (data, len);
}

/**
 * fu_common_get_contents_fd:
 * @fd: a file descriptor
 * @count: the maximum number of bytes to read
 * @error: (nullable): optional return location for an error
 *
 * Reads a blob from a specific file descriptor.
 *
 * Note: this will close the fd when done
 *
 * Returns: (transfer full): a #GBytes, or %NULL
 *
 * Since: 0.9.5
 **/
GBytes *
fu_common_get_contents_fd (gint fd, gsize count, GError **error)
{
#ifdef HAVE_GIO_UNIX
	guint8 tmp[0x8000] = { 0x0 };
	g_autoptr(GByteArray) buf = g_byte_array_new ();
	g_autoptr(GError) error_local = NULL;
	g_autoptr(GInputStream) stream = NULL;

	g_return_val_if_fail (fd > 0, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* this is invalid */
	if (count == 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "A maximum read size must be specified");
		return NULL;
	}

	/* read the entire fd to a data blob */
	stream = g_unix_input_stream_new (fd, TRUE);

	/* read from stream in 32kB chunks */
	while (TRUE) {
		gssize sz;
		sz = g_input_stream_read (stream, tmp, sizeof(tmp), NULL, &error_local);
		if (sz == 0)
			break;
		if (sz < 0) {
			g_set_error_literal (error,
					     FWUPD_ERROR,
					     FWUPD_ERROR_INVALID_FILE,
					     error_local->message);
			return NULL;
		}
		g_byte_array_append (buf, tmp, sz);
		if (buf->len > count) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INVALID_FILE,
				     "cannot read from fd: 0x%x > 0x%x",
				     buf->len, (guint) count);
			return NULL;
		}
	}
	return g_byte_array_free_to_bytes (g_steal_pointer (&buf));
#else
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "Not supported as <glib-unix.h> is unavailable");
	return NULL;
#endif
}

#ifdef HAVE_LIBARCHIVE
static gboolean
fu_common_extract_archive_entry (struct archive_entry *entry, const gchar *dir)
{
	const gchar *tmp;
	g_autofree gchar *buf = NULL;

	/* no output file */
	if (archive_entry_pathname (entry) == NULL)
		return FALSE;

	/* update output path */
	tmp = archive_entry_pathname (entry);
	buf = g_build_filename (dir, tmp, NULL);
	archive_entry_update_pathname_utf8 (entry, buf);
	return TRUE;
}
#endif

/**
 * fu_common_extract_archive:
 * @blob: data archive as a blob
 * @dir: a directory name to extract to
 * @error: (nullable): optional return location for an error
 *
 * Extracts an archive to a directory.
 *
 * Returns: %TRUE for success
 *
 * Since: 0.9.7
 **/
gboolean
fu_common_extract_archive (GBytes *blob, const gchar *dir, GError **error)
{
#ifdef HAVE_LIBARCHIVE
	gboolean ret = TRUE;
	int r;
	struct archive *arch = NULL;
	struct archive_entry *entry;

	g_return_val_if_fail (blob != NULL, FALSE);
	g_return_val_if_fail (dir != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* decompress anything matching either glob */
	g_debug ("decompressing into %s", dir);
	arch = archive_read_new ();
	archive_read_support_format_all (arch);
	archive_read_support_filter_all (arch);
	r = archive_read_open_memory (arch,
				      (void *) g_bytes_get_data (blob, NULL),
				      (size_t) g_bytes_get_size (blob));
	if (r != 0) {
		ret = FALSE;
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "Cannot open: %s",
			     archive_error_string (arch));
		goto out;
	}
	for (;;) {
		gboolean valid;
		r = archive_read_next_header (arch, &entry);
		if (r == ARCHIVE_EOF)
			break;
		if (r != ARCHIVE_OK) {
			ret = FALSE;
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "Cannot read header: %s",
				     archive_error_string (arch));
			goto out;
		}

		/* only extract if valid */
		valid = fu_common_extract_archive_entry (entry, dir);
		if (!valid)
			continue;
		r = archive_read_extract (arch, entry, 0);
		if (r != ARCHIVE_OK) {
			ret = FALSE;
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "Cannot extract: %s",
				     archive_error_string (arch));
			goto out;
		}
	}
out:
	if (arch != NULL) {
		archive_read_close (arch);
		archive_read_free (arch);
	}
	return ret;
#else
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "missing libarchive support");
	return FALSE;
#endif
}

static void
fu_common_add_argv (GPtrArray *argv, const gchar *fmt, ...) G_GNUC_PRINTF (2, 3);

static void
fu_common_add_argv (GPtrArray *argv, const gchar *fmt, ...)
{
	va_list args;
	g_autofree gchar *tmp = NULL;
	g_auto(GStrv) split = NULL;

	va_start (args, fmt);
	tmp = g_strdup_vprintf (fmt, args);
	va_end (args);

	split = g_strsplit (tmp, " ", -1);
	for (guint i = 0; split[i] != NULL; i++)
		g_ptr_array_add (argv, g_strdup (split[i]));
}

/**
 * fu_common_find_program_in_path:
 * @basename: the program to search
 * @error: (nullable): optional return location for an error
 *
 * Looks for a program in the PATH variable
 *
 * Returns: a new #gchar, or %NULL for error
 *
 * Since: 1.1.2
 **/
gchar *
fu_common_find_program_in_path (const gchar *basename, GError **error)
{
	gchar *fn = g_find_program_in_path (basename);
	if (fn == NULL) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "missing executable %s in PATH",
			     basename);
		return NULL;
	}
	return fn;
}

static gboolean
fu_common_test_namespace_support (GError **error)
{
	/* test if CONFIG_USER_NS is valid */
	if (!g_file_test ("/proc/self/ns/user", G_FILE_TEST_IS_SYMLINK)) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_NOT_SUPPORTED,
			     "missing CONFIG_USER_NS in kernel");
		return FALSE;
	}
	if (g_file_test ("/proc/sys/kernel/unprivileged_userns_clone", G_FILE_TEST_EXISTS)) {
		g_autofree gchar *clone = NULL;
		if (!g_file_get_contents ("/proc/sys/kernel/unprivileged_userns_clone", &clone, NULL, error))
			return FALSE;
		if (g_ascii_strtoll (clone, NULL, 10) == 0) {
			g_set_error (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_NOT_SUPPORTED,
				     "unprivileged user namespace clones disabled by distro");
			return FALSE;
		}
	}
	return TRUE;
}

/**
 * fu_common_firmware_builder:
 * @bytes: the data to use
 * @script_fn: Name of the script to run in the tarball, e.g. `startup.sh`
 * @output_fn: Name of the generated firmware, e.g. `firmware.bin`
 * @error: (nullable): optional return location for an error
 *
 * Builds a firmware file using tools from the host session in a bubblewrap
 * jail. Several things happen during build:
 *
 * 1. The @bytes data is untarred to a temporary location
 * 2. A bubblewrap container is set up
 * 3. The startup.sh script is run inside the container
 * 4. The firmware.bin is extracted from the container
 * 5. The temporary location is deleted
 *
 * Returns: a new #GBytes, or %NULL for error
 *
 * Since: 0.9.7
 **/
GBytes *
fu_common_firmware_builder (GBytes *bytes,
			    const gchar *script_fn,
			    const gchar *output_fn,
			    GError **error)
{
	gint rc = 0;
	g_autofree gchar *argv_str = NULL;
	g_autofree gchar *bwrap_fn = NULL;
	g_autofree gchar *localstatebuilderdir = NULL;
	g_autofree gchar *localstatedir = NULL;
	g_autofree gchar *output2_fn = NULL;
	g_autofree gchar *standard_error = NULL;
	g_autofree gchar *standard_output = NULL;
	g_autofree gchar *tmpdir = NULL;
	g_autoptr(GBytes) firmware_blob = NULL;
	g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func (g_free);

	g_return_val_if_fail (bytes != NULL, NULL);
	g_return_val_if_fail (script_fn != NULL, NULL);
	g_return_val_if_fail (output_fn != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find bwrap in the path */
	bwrap_fn = fu_common_find_program_in_path ("bwrap", error);
	if (bwrap_fn == NULL)
		return NULL;

	/* test if CONFIG_USER_NS is valid */
	if (!fu_common_test_namespace_support (error))
		return NULL;

	/* untar file to temp location */
	tmpdir = g_dir_make_tmp ("fwupd-gen-XXXXXX", error);
	if (tmpdir == NULL)
		return NULL;
	if (!fu_common_extract_archive (bytes, tmpdir, error))
		return NULL;

	/* this is shared with the plugins */
	localstatedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR_PKG);
	localstatebuilderdir = g_build_filename (localstatedir, "builder", NULL);

	/* launch bubblewrap and generate firmware */
	g_ptr_array_add (argv, g_steal_pointer (&bwrap_fn));
	fu_common_add_argv (argv, "--die-with-parent");
	fu_common_add_argv (argv, "--ro-bind /usr /usr");
	fu_common_add_argv (argv, "--ro-bind /lib /lib");
	fu_common_add_argv (argv, "--ro-bind-try /lib64 /lib64");
	fu_common_add_argv (argv, "--ro-bind /bin /bin");
	fu_common_add_argv (argv, "--ro-bind /sbin /sbin");
	fu_common_add_argv (argv, "--dir /tmp");
	fu_common_add_argv (argv, "--dir /var");
	fu_common_add_argv (argv, "--bind %s /tmp", tmpdir);
	if (g_file_test (localstatebuilderdir, G_FILE_TEST_EXISTS))
		fu_common_add_argv (argv, "--ro-bind %s /boot", localstatebuilderdir);
	fu_common_add_argv (argv, "--dev /dev");
	fu_common_add_argv (argv, "--chdir /tmp");
	fu_common_add_argv (argv, "--unshare-all");
	fu_common_add_argv (argv, "/tmp/%s", script_fn);
	g_ptr_array_add (argv, NULL);
	argv_str = g_strjoinv (" ", (gchar **) argv->pdata);
	g_debug ("running '%s' in %s", argv_str, tmpdir);
	if (!g_spawn_sync ("/tmp",
			   (gchar **) argv->pdata,
			   NULL,
			   G_SPAWN_SEARCH_PATH,
			   NULL, NULL, /* child_setup */
			   &standard_output,
			   &standard_error,
			   &rc,
			   error)) {
		g_prefix_error (error, "failed to run '%s': ", argv_str);
		return NULL;
	}
	if (standard_output != NULL && standard_output[0] != '\0')
		g_debug ("console output was: %s", standard_output);
	if (rc != 0) {
		FwupdError code = FWUPD_ERROR_INTERNAL;
		if (errno == ENOTTY)
			code = FWUPD_ERROR_PERMISSION_DENIED;
		g_set_error (error,
			     FWUPD_ERROR,
			     code,
			     "failed to build firmware: %s",
			     standard_error);
		return NULL;
	}

	/* get generated file */
	output2_fn = g_build_filename (tmpdir, output_fn, NULL);
	firmware_blob = fu_common_get_contents_bytes (output2_fn, error);
	if (firmware_blob == NULL)
		return NULL;

	/* cleanup temp directory */
	if (!fu_common_rmtree (tmpdir, error))
		return NULL;

	/* success */
	return g_steal_pointer (&firmware_blob);
}

typedef struct {
	FuOutputHandler		 handler_cb;
	gpointer		 handler_user_data;
	GMainLoop		*loop;
	GSource			*source;
	GInputStream		*stream;
	GCancellable		*cancellable;
	guint			 timeout_id;
} FuCommonSpawnHelper;

static void fu_common_spawn_create_pollable_source (FuCommonSpawnHelper *helper);

static gboolean
fu_common_spawn_source_pollable_cb (GObject *stream, gpointer user_data)
{
	FuCommonSpawnHelper *helper = (FuCommonSpawnHelper *) user_data;
	gchar buffer[1024];
	gssize sz;
	g_auto(GStrv) split = NULL;
	g_autoptr(GError) error = NULL;

	/* read from stream */
	sz = g_pollable_input_stream_read_nonblocking (G_POLLABLE_INPUT_STREAM (stream),
						       buffer,
						       sizeof(buffer) - 1,
						       NULL,
						       &error);
	if (sz < 0) {
		if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
			g_warning ("failed to get read from nonblocking fd: %s",
				   error->message);
		}
		return G_SOURCE_REMOVE;
	}

	/* no read possible */
	if (sz == 0)
		g_main_loop_quit (helper->loop);

	/* emit lines */
	if (helper->handler_cb != NULL) {
		buffer[sz] = '\0';
		split = g_strsplit (buffer, "\n", -1);
		for (guint i = 0; split[i] != NULL; i++) {
			if (split[i][0] == '\0')
				continue;
			helper->handler_cb (split[i], helper->handler_user_data);
		}
	}

	/* set up the source for the next read */
	fu_common_spawn_create_pollable_source (helper);
	return G_SOURCE_REMOVE;
}

static void
fu_common_spawn_create_pollable_source (FuCommonSpawnHelper *helper)
{
	if (helper->source != NULL)
		g_source_destroy (helper->source);
	helper->source = g_pollable_input_stream_create_source (G_POLLABLE_INPUT_STREAM (helper->stream),
								helper->cancellable);
	g_source_attach (helper->source, NULL);
	g_source_set_callback (helper->source, (GSourceFunc) fu_common_spawn_source_pollable_cb, helper, NULL);
}

static void
fu_common_spawn_helper_free (FuCommonSpawnHelper *helper)
{
	g_object_unref (helper->cancellable);
	if (helper->stream != NULL)
		g_object_unref (helper->stream);
	if (helper->source != NULL)
		g_source_destroy (helper->source);
	if (helper->loop != NULL)
		g_main_loop_unref (helper->loop);
	if (helper->timeout_id != 0)
		g_source_remove (helper->timeout_id);
	g_free (helper);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FuCommonSpawnHelper, fu_common_spawn_helper_free)
#pragma clang diagnostic pop

static gboolean
fu_common_spawn_timeout_cb (gpointer user_data)
{
	FuCommonSpawnHelper *helper = (FuCommonSpawnHelper *) user_data;
	g_cancellable_cancel (helper->cancellable);
	g_main_loop_quit (helper->loop);
	helper->timeout_id = 0;
	return G_SOURCE_REMOVE;
}

static void
fu_common_spawn_cancelled_cb (GCancellable *cancellable, FuCommonSpawnHelper *helper)
{
	/* just propagate */
	g_cancellable_cancel (helper->cancellable);
}

/**
 * fu_common_spawn_sync:
 * @argv: the argument list to run
 * @handler_cb: (scope call) (nullable): optional #FuOutputHandler
 * @handler_user_data: (nullable): the user data to pass to @handler_cb
 * @timeout_ms: a timeout in ms, or 0 for no limit
 * @cancellable: (nullable): optional #GCancellable
 * @error: (nullable): optional return location for an error
 *
 * Runs a subprocess and waits for it to exit. Any output on standard out or
 * standard error will be forwarded to @handler_cb as whole lines.
 *
 * Returns: %TRUE for success
 *
 * Since: 0.9.7
 **/
gboolean
fu_common_spawn_sync (const gchar * const * argv,
		      FuOutputHandler handler_cb,
		      gpointer handler_user_data,
		      guint timeout_ms,
		      GCancellable *cancellable, GError **error)
{
	g_autoptr(FuCommonSpawnHelper) helper = NULL;
	g_autoptr(GSubprocess) subprocess = NULL;
	g_autofree gchar *argv_str = NULL;
	gulong cancellable_id = 0;

	g_return_val_if_fail (argv != NULL, FALSE);
	g_return_val_if_fail (cancellable == NULL || G_IS_CANCELLABLE (cancellable), FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* create subprocess */
	argv_str = g_strjoinv (" ", (gchar **) argv);
	g_debug ("running '%s'", argv_str);
	subprocess = g_subprocess_newv (argv, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
					      G_SUBPROCESS_FLAGS_STDERR_MERGE, error);
	if (subprocess == NULL)
		return FALSE;

	/* watch for process to exit */
	helper = g_new0 (FuCommonSpawnHelper, 1);
	helper->handler_cb = handler_cb;
	helper->handler_user_data = handler_user_data;
	helper->loop = g_main_loop_new (NULL, FALSE);
	helper->stream = g_subprocess_get_stdout_pipe (subprocess);

	/* always create a cancellable, and connect up the parent */
	helper->cancellable = g_cancellable_new ();
	if (cancellable != NULL) {
		cancellable_id = g_cancellable_connect (cancellable,
							G_CALLBACK (fu_common_spawn_cancelled_cb),
							helper, NULL);
	}

	/* allow timeout */
	if (timeout_ms > 0) {
		helper->timeout_id = g_timeout_add (timeout_ms,
						    fu_common_spawn_timeout_cb,
						    helper);
	}
	fu_common_spawn_create_pollable_source (helper);
	g_main_loop_run (helper->loop);
	g_cancellable_disconnect (cancellable, cancellable_id);
	if (g_cancellable_set_error_if_cancelled (helper->cancellable, error))
		return FALSE;
	return g_subprocess_wait_check (subprocess, cancellable, error);
}

/**
 * fu_common_write_uint16:
 * @buf: a writable buffer
 * @val_native: a value in host byte-order
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 *
 * Writes a value to a buffer using a specified endian.
 *
 * Since: 1.0.3
 **/
void
fu_common_write_uint16 (guint8 *buf, guint16 val_native, FuEndianType endian)
{
	guint16 val_hw;
	switch (endian) {
	case G_BIG_ENDIAN:
		val_hw = GUINT16_TO_BE(val_native);
		break;
	case G_LITTLE_ENDIAN:
		val_hw = GUINT16_TO_LE(val_native);
		break;
	default:
		g_assert_not_reached ();
	}
	memcpy (buf, &val_hw, sizeof(val_hw));
}

/**
 * fu_common_write_uint32:
 * @buf: a writable buffer
 * @val_native: a value in host byte-order
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 *
 * Writes a value to a buffer using a specified endian.
 *
 * Since: 1.0.3
 **/
void
fu_common_write_uint32 (guint8 *buf, guint32 val_native, FuEndianType endian)
{
	guint32 val_hw;
	switch (endian) {
	case G_BIG_ENDIAN:
		val_hw = GUINT32_TO_BE(val_native);
		break;
	case G_LITTLE_ENDIAN:
		val_hw = GUINT32_TO_LE(val_native);
		break;
	default:
		g_assert_not_reached ();
	}
	memcpy (buf, &val_hw, sizeof(val_hw));
}

/**
 * fu_common_write_uint64:
 * @buf: a writable buffer
 * @val_native: a value in host byte-order
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 *
 * Writes a value to a buffer using a specified endian.
 *
 * Since: 1.5.8
 **/
void
fu_common_write_uint64 (guint8 *buf, guint64 val_native, FuEndianType endian)
{
	guint64 val_hw;
	switch (endian) {
	case G_BIG_ENDIAN:
		val_hw = GUINT64_TO_BE(val_native);
		break;
	case G_LITTLE_ENDIAN:
		val_hw = GUINT64_TO_LE(val_native);
		break;
	default:
		g_assert_not_reached ();
	}
	memcpy (buf, &val_hw, sizeof(val_hw));
}

/**
 * fu_common_read_uint16:
 * @buf: a readable buffer
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 *
 * Read a value from a buffer using a specified endian.
 *
 * Returns: a value in host byte-order
 *
 * Since: 1.0.3
 **/
guint16
fu_common_read_uint16 (const guint8 *buf, FuEndianType endian)
{
	guint16 val_hw, val_native;
	memcpy (&val_hw, buf, sizeof(val_hw));
	switch (endian) {
	case G_BIG_ENDIAN:
		val_native = GUINT16_FROM_BE(val_hw);
		break;
	case G_LITTLE_ENDIAN:
		val_native = GUINT16_FROM_LE(val_hw);
		break;
	default:
		g_assert_not_reached ();
	}
	return val_native;
}

/**
 * fu_common_read_uint32:
 * @buf: a readable buffer
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 *
 * Read a value from a buffer using a specified endian.
 *
 * Returns: a value in host byte-order
 *
 * Since: 1.0.3
 **/
guint32
fu_common_read_uint32 (const guint8 *buf, FuEndianType endian)
{
	guint32 val_hw, val_native;
	memcpy (&val_hw, buf, sizeof(val_hw));
	switch (endian) {
	case G_BIG_ENDIAN:
		val_native = GUINT32_FROM_BE(val_hw);
		break;
	case G_LITTLE_ENDIAN:
		val_native = GUINT32_FROM_LE(val_hw);
		break;
	default:
		g_assert_not_reached ();
	}
	return val_native;
}

/**
 * fu_common_read_uint64:
 * @buf: a readable buffer
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 *
 * Read a value from a buffer using a specified endian.
 *
 * Returns: a value in host byte-order
 *
 * Since: 1.5.8
 **/
guint64
fu_common_read_uint64 (const guint8 *buf, FuEndianType endian)
{
	guint64 val_hw, val_native;
	memcpy (&val_hw, buf, sizeof(val_hw));
	switch (endian) {
	case G_BIG_ENDIAN:
		val_native = GUINT64_FROM_BE(val_hw);
		break;
	case G_LITTLE_ENDIAN:
		val_native = GUINT64_FROM_LE(val_hw);
		break;
	default:
		g_assert_not_reached ();
	}
	return val_native;
}

/**
 * fu_common_strtoull:
 * @str: a string, e.g. `0x1234`
 *
 * Converts a string value to an integer. Values are assumed base 10, unless
 * prefixed with "0x" where they are parsed as base 16.
 *
 * Returns: integer value, or 0x0 for error
 *
 * Since: 1.1.2
 **/
guint64
fu_common_strtoull (const gchar *str)
{
	guint base = 10;
	if (str == NULL)
		return 0x0;
	if (g_str_has_prefix (str, "0x")) {
		str += 2;
		base = 16;
	}
	return g_ascii_strtoull (str, NULL, base);
}

/**
 * fu_common_strstrip:
 * @str: a string, e.g. ` test `
 *
 * Removes leading and trailing whitespace from a constant string.
 *
 * Returns: newly allocated string
 *
 * Since: 1.1.2
 **/
gchar *
fu_common_strstrip (const gchar *str)
{
	guint head = G_MAXUINT;
	guint tail = 0;

	g_return_val_if_fail (str != NULL, NULL);

	/* find first non-space char */
	for (guint i = 0; str[i] != '\0'; i++) {
		if (str[i] != ' ') {
			head = i;
			break;
		}
	}
	if (head == G_MAXUINT)
		return g_strdup ("");

	/* find last non-space char */
	for (guint i = head; str[i] != '\0'; i++) {
		if (!g_ascii_isspace (str[i]))
			tail = i;
	}
	return g_strndup (str + head, tail - head + 1);
}

static const GError *
fu_common_error_array_find (GPtrArray *errors, FwupdError error_code)
{
	for (guint j = 0; j < errors->len; j++) {
		const GError *error = g_ptr_array_index (errors, j);
		if (g_error_matches (error, FWUPD_ERROR, error_code))
			return error;
	}
	return NULL;
}

static guint
fu_common_error_array_count (GPtrArray *errors, FwupdError error_code)
{
	guint cnt = 0;
	for (guint j = 0; j < errors->len; j++) {
		const GError *error = g_ptr_array_index (errors, j);
		if (g_error_matches (error, FWUPD_ERROR, error_code))
			cnt++;
	}
	return cnt;
}

static gboolean
fu_common_error_array_matches_any (GPtrArray *errors, FwupdError *error_codes)
{
	for (guint j = 0; j < errors->len; j++) {
		const GError *error = g_ptr_array_index (errors, j);
		gboolean matches_any = FALSE;
		for (guint i = 0; error_codes[i] != FWUPD_ERROR_LAST; i++) {
			if (g_error_matches (error, FWUPD_ERROR, error_codes[i])) {
				matches_any = TRUE;
				break;
			}
		}
		if (!matches_any)
			return FALSE;
	}
	return TRUE;
}

/**
 * fu_common_error_array_get_best:
 * @errors: (element-type GError): array of errors
 *
 * Finds the 'best' error to show the user from a array of errors, creating a
 * completely bespoke error where required.
 *
 * Returns: (transfer full): a #GError, never %NULL
 *
 * Since: 1.0.8
 **/
GError *
fu_common_error_array_get_best (GPtrArray *errors)
{
	FwupdError err_prio[] =		{ FWUPD_ERROR_INVALID_FILE,
					  FWUPD_ERROR_VERSION_SAME,
					  FWUPD_ERROR_VERSION_NEWER,
					  FWUPD_ERROR_NOT_SUPPORTED,
					  FWUPD_ERROR_INTERNAL,
					  FWUPD_ERROR_NOT_FOUND,
					  FWUPD_ERROR_LAST };
	FwupdError err_all_uptodate[] =	{ FWUPD_ERROR_VERSION_SAME,
					  FWUPD_ERROR_NOT_FOUND,
					  FWUPD_ERROR_NOT_SUPPORTED,
					  FWUPD_ERROR_LAST };
	FwupdError err_all_newer[] =	{ FWUPD_ERROR_VERSION_NEWER,
					  FWUPD_ERROR_VERSION_SAME,
					  FWUPD_ERROR_NOT_FOUND,
					  FWUPD_ERROR_NOT_SUPPORTED,
					  FWUPD_ERROR_LAST };

	/* are all the errors either GUID-not-matched or version-same? */
	if (fu_common_error_array_count (errors, FWUPD_ERROR_VERSION_SAME) > 1 &&
	    fu_common_error_array_matches_any (errors, err_all_uptodate)) {
		return g_error_new (FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "All updatable firmware is already installed");
	}

	/* are all the errors either GUID-not-matched or version same or newer? */
	if (fu_common_error_array_count (errors, FWUPD_ERROR_VERSION_NEWER) > 1 &&
	    fu_common_error_array_matches_any (errors, err_all_newer)) {
		return g_error_new (FWUPD_ERROR,
				    FWUPD_ERROR_NOTHING_TO_DO,
				    "All updatable devices already have newer versions");
	}

	/* get the most important single error */
	for (guint i = 0; err_prio[i] != FWUPD_ERROR_LAST; i++) {
		const GError *error_tmp = fu_common_error_array_find (errors, err_prio[i]);
		if (error_tmp != NULL)
			return g_error_copy (error_tmp);
	}

	/* fall back to something */
	return g_error_new (FWUPD_ERROR,
			    FWUPD_ERROR_NOT_FOUND,
			    "No supported devices found");
}

/**
 * fu_common_get_path:
 * @path_kind: a #FuPathKind e.g. %FU_PATH_KIND_DATADIR_PKG
 *
 * Gets a fwupd-specific system path. These can be overridden with various
 * environment variables, for instance %FWUPD_DATADIR.
 *
 * Returns: a system path, or %NULL if invalid
 *
 * Since: 1.0.8
 **/
gchar *
fu_common_get_path (FuPathKind path_kind)
{
	const gchar *tmp;
	g_autofree gchar *basedir = NULL;

	switch (path_kind) {
	/* /var */
	case FU_PATH_KIND_LOCALSTATEDIR:
		tmp = g_getenv ("FWUPD_LOCALSTATEDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		tmp = g_getenv ("SNAP_USER_DATA");
		if (tmp != NULL)
			return g_build_filename (tmp, FWUPD_LOCALSTATEDIR, NULL);
		return g_build_filename (FWUPD_LOCALSTATEDIR, NULL);
	/* /proc */
	case FU_PATH_KIND_PROCFS:
		tmp = g_getenv ("FWUPD_PROCFS");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/proc");
	/* /sys/firmware */
	case FU_PATH_KIND_SYSFSDIR_FW:
		tmp = g_getenv ("FWUPD_SYSFSFWDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/sys/firmware");
	/* /sys/class/tpm */
	case FU_PATH_KIND_SYSFSDIR_TPM:
		tmp = g_getenv ("FWUPD_SYSFSTPMDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/sys/class/tpm");
	/* /sys/bus/platform/drivers */
	case FU_PATH_KIND_SYSFSDIR_DRIVERS:
		tmp = g_getenv ("FWUPD_SYSFSDRIVERDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/sys/bus/platform/drivers");
	/* /sys/kernel/security */
	case FU_PATH_KIND_SYSFSDIR_SECURITY:
		tmp = g_getenv ("FWUPD_SYSFSSECURITYDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/sys/kernel/security");
	/* /sys/firmware/acpi/tables */
	case FU_PATH_KIND_ACPI_TABLES:
		tmp = g_getenv ("FWUPD_ACPITABLESDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/sys/firmware/acpi/tables");
	/* /etc */
	case FU_PATH_KIND_SYSCONFDIR:
		tmp = g_getenv ("FWUPD_SYSCONFDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		tmp = g_getenv ("SNAP_USER_DATA");
		if (tmp != NULL)
			return g_build_filename (tmp, FWUPD_SYSCONFDIR, NULL);
		return g_strdup (FWUPD_SYSCONFDIR);
	/* /usr/lib/<triplet>/fwupd-plugins-3 */
	case FU_PATH_KIND_PLUGINDIR_PKG:
		tmp = g_getenv ("FWUPD_PLUGINDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		tmp = g_getenv ("SNAP");
		if (tmp != NULL)
			return g_build_filename (tmp, FWUPD_PLUGINDIR, NULL);
		return g_build_filename (FWUPD_PLUGINDIR, NULL);
	/* /usr/share/fwupd */
	case FU_PATH_KIND_DATADIR_PKG:
		tmp = g_getenv ("FWUPD_DATADIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		tmp = g_getenv ("SNAP");
		if (tmp != NULL)
			return g_build_filename (tmp, FWUPD_DATADIR, PACKAGE_NAME, NULL);
		return g_build_filename (FWUPD_DATADIR, PACKAGE_NAME, NULL);
	/* /usr/libexec/fwupd/efi */
	case FU_PATH_KIND_EFIAPPDIR:
		tmp = g_getenv ("FWUPD_EFIAPPDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
#ifdef EFI_APP_LOCATION
		tmp = g_getenv ("SNAP");
		if (tmp != NULL)
			return g_build_filename (tmp, EFI_APP_LOCATION, NULL);
		return g_strdup (EFI_APP_LOCATION);
#else
		return NULL;
#endif
	/* /etc/fwupd */
	case FU_PATH_KIND_SYSCONFDIR_PKG:
		tmp = g_getenv ("CONFIGURATION_DIRECTORY");
		if (tmp != NULL && g_file_test (tmp, G_FILE_TEST_EXISTS))
			return g_build_filename (tmp, NULL);
		basedir = fu_common_get_path (FU_PATH_KIND_SYSCONFDIR);
		return g_build_filename (basedir, PACKAGE_NAME, NULL);
	/* /var/lib/fwupd */
	case FU_PATH_KIND_LOCALSTATEDIR_PKG:
		tmp = g_getenv ("STATE_DIRECTORY");
		if (tmp != NULL && g_file_test (tmp, G_FILE_TEST_EXISTS))
			return g_build_filename (tmp, NULL);
		basedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR);
		return g_build_filename (basedir, "lib", PACKAGE_NAME, NULL);
	/* /var/cache/fwupd */
	case FU_PATH_KIND_CACHEDIR_PKG:
		tmp = g_getenv ("CACHE_DIRECTORY");
		if (tmp != NULL && g_file_test (tmp, G_FILE_TEST_EXISTS))
			return g_build_filename (tmp, NULL);
		basedir = fu_common_get_path (FU_PATH_KIND_LOCALSTATEDIR);
		return g_build_filename (basedir, "cache", PACKAGE_NAME, NULL);
	/* /run/lock */
	case FU_PATH_KIND_LOCKDIR:
		return g_strdup ("/run/lock");
	/* /sys/class/firmware-attributes */
	case FU_PATH_KIND_SYSFSDIR_FW_ATTRIB:
		tmp = g_getenv ("FWUPD_SYSFSFWATTRIBDIR");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/sys/class/firmware-attributes");
	case FU_PATH_KIND_OFFLINE_TRIGGER:
		tmp = g_getenv ("FWUPD_OFFLINE_TRIGGER");
		if (tmp != NULL)
			return g_strdup (tmp);
		return g_strdup ("/system-update");
	case FU_PATH_KIND_POLKIT_ACTIONS:
#ifdef POLKIT_ACTIONDIR
		return g_strdup (POLKIT_ACTIONDIR);
#else
	return NULL;
#endif
	/* this shouldn't happen */
	default:
		g_warning ("cannot build path for unknown kind %u", path_kind);
	}

	return NULL;
}

/**
 * fu_common_string_replace:
 * @string: the #GString to operate on
 * @search: the text to search for
 * @replace: the text to use for substitutions
 *
 * Performs multiple search and replace operations on the given string.
 *
 * Returns: the number of replacements done, or 0 if @search is not found.
 *
 * Since: 1.2.0
 **/
guint
fu_common_string_replace (GString *string, const gchar *search, const gchar *replace)
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

/**
 * fu_common_strwidth:
 * @text: the string to operate on
 *
 * Returns the width of the string in displayed characters on the console.
 *
 * Returns: width of text
 *
 * Since: 1.3.2
 **/
gsize
fu_common_strwidth (const gchar *text)
{
	const gchar *p = text;
	gsize width = 0;

	g_return_val_if_fail (text != NULL, 0);

	while (*p) {
		gunichar c = g_utf8_get_char (p);
		if (g_unichar_iswide (c))
			width += 2;
		else if (!g_unichar_iszerowidth (c))
			width += 1;
		p = g_utf8_next_char (p);
	}
	return width;
}

/**
 * fu_common_string_append_kv:
 * @str: a #GString
 * @idt: the indent
 * @key: a string to append
 * @value: a string to append
 *
 * Appends a key and string value to a string
 *
 * Since: 1.2.4
 */
void
fu_common_string_append_kv (GString *str, guint idt, const gchar *key, const gchar *value)
{
	const guint align = 24;
	gsize keysz;

	g_return_if_fail (idt * 2 < align);

	/* ignore */
	if (key == NULL)
		return;
	for (gsize i = 0; i < idt; i++)
		g_string_append (str, "  ");
	if (key[0] != '\0') {
		g_string_append_printf (str, "%s:", key);
		keysz = (idt * 2) + fu_common_strwidth (key) + 1;
	} else {
		keysz = idt * 2;
	}
	if (value != NULL) {
		g_auto(GStrv) split = NULL;
		split = g_strsplit (value, "\n", -1);
		for (guint i = 0; split[i] != NULL; i++) {
			if (i == 0) {
				for (gsize j = keysz; j < align; j++)
					g_string_append (str, " ");
			} else {
				g_string_append (str, "\n");
				for (gsize j = 0; j < idt; j++)
					g_string_append (str, "  ");
			}
			g_string_append (str, split[i]);
		}
	}
	g_string_append (str, "\n");
}

/**
 * fu_common_string_append_ku:
 * @str: a #GString
 * @idt: the indent
 * @key: a string to append
 * @value: guint64
 *
 * Appends a key and unsigned integer to a string
 *
 * Since: 1.2.4
 */
void
fu_common_string_append_ku (GString *str, guint idt, const gchar *key, guint64 value)
{
	g_autofree gchar *tmp = g_strdup_printf ("%" G_GUINT64_FORMAT, value);
	fu_common_string_append_kv (str, idt, key, tmp);
}

/**
 * fu_common_string_append_kx:
 * @str: a #GString
 * @idt: the indent
 * @key: a string to append
 * @value: guint64
 *
 * Appends a key and hex integer to a string
 *
 * Since: 1.2.4
 */
void
fu_common_string_append_kx (GString *str, guint idt, const gchar *key, guint64 value)
{
	g_autofree gchar *tmp = g_strdup_printf ("0x%x", (guint) value);
	fu_common_string_append_kv (str, idt, key, tmp);
}

/**
 * fu_common_string_append_kb:
 * @str: a #GString
 * @idt: the indent
 * @key: a string to append
 * @value: Boolean
 *
 * Appends a key and boolean value to a string
 *
 * Since: 1.2.4
 */
void
fu_common_string_append_kb (GString *str, guint idt, const gchar *key, gboolean value)
{
	fu_common_string_append_kv (str, idt, key, value ? "true" : "false");
}

/**
 * fu_common_dump_full:
 * @log_domain: (nullable): optional log domain, typically %G_LOG_DOMAIN
 * @title: (nullable): optional prefix title
 * @data: buffer to print
 * @len: the size of @data
 * @columns: break new lines after this many bytes
 * @flags: dump flags, e.g. %FU_DUMP_FLAGS_SHOW_ASCII
 *
 * Dumps a raw buffer to the screen.
 *
 * Since: 1.2.4
 **/
void
fu_common_dump_full (const gchar *log_domain,
		     const gchar *title,
		     const guint8 *data,
		     gsize len,
		     guint columns,
		     FuDumpFlags flags)
{
	g_autoptr(GString) str = g_string_new (NULL);

	/* optional */
	if (title != NULL)
		g_string_append_printf (str, "%s:", title);

	/* if more than can fit on one line then start afresh */
	if (len > columns || flags & FU_DUMP_FLAGS_SHOW_ADDRESSES) {
		g_string_append (str, "\n");
	} else {
		for (gsize i = str->len; i < 16; i++)
			g_string_append (str, " ");
	}

	/* offset line */
	if (flags & FU_DUMP_FLAGS_SHOW_ADDRESSES) {
		g_string_append (str, "       │ ");
		for (gsize i = 0; i < columns; i++)
			g_string_append_printf (str, "%02x ", (guint) i);
		g_string_append (str, "\n───────┼");
		for (gsize i = 0; i < columns; i++)
			g_string_append (str, "───");
		g_string_append_printf (str, "\n0x%04x │ ", (guint) 0);
	}

	/* print each row */
	for (gsize i = 0; i < len; i++) {
		g_string_append_printf (str, "%02x ", data[i]);

		/* optionally print ASCII char */
		if (flags & FU_DUMP_FLAGS_SHOW_ASCII) {
			if (g_ascii_isprint (data[i]))
				g_string_append_printf (str, "[%c] ", data[i]);
			else
				g_string_append (str, "[?] ");
		}

		/* new row required */
		if (i > 0 && i != len - 1 && (i + 1) % columns == 0) {
			g_string_append (str, "\n");
			if (flags & FU_DUMP_FLAGS_SHOW_ADDRESSES)
				g_string_append_printf (str, "0x%04x │ ", (guint) i + 1);
		}
	}
	g_log (log_domain, G_LOG_LEVEL_DEBUG, "%s", str->str);
}

/**
 * fu_common_dump_raw:
 * @log_domain: (nullable): optional log domain, typically %G_LOG_DOMAIN
 * @title: (nullable): optional prefix title
 * @data: buffer to print
 * @len: the size of @data
 *
 * Dumps a raw buffer to the screen.
 *
 * Since: 1.2.2
 **/
void
fu_common_dump_raw (const gchar *log_domain,
		    const gchar *title,
		    const guint8 *data,
		    gsize len)
{
	FuDumpFlags flags = FU_DUMP_FLAGS_NONE;
	if (len > 64)
		flags |= FU_DUMP_FLAGS_SHOW_ADDRESSES;
	fu_common_dump_full (log_domain, title, data, len, 32, flags);
}

/**
 * fu_common_dump_bytes:
 * @log_domain: (nullable): optional log domain, typically %G_LOG_DOMAIN
 * @title: (nullable): optional prefix title
 * @bytes: data blob
 *
 * Dumps a byte buffer to the screen.
 *
 * Since: 1.2.2
 **/
void
fu_common_dump_bytes (const gchar *log_domain,
		      const gchar *title,
		      GBytes *bytes)
{
	gsize len = 0;
	const guint8 *data = g_bytes_get_data (bytes, &len);
	fu_common_dump_raw (log_domain, title, data, len);
}

/**
 * fu_common_bytes_align:
 * @bytes: data blob
 * @blksz: block size in bytes
 * @padval: the byte used to pad the byte buffer
 *
 * Aligns a block of memory to @blksize using the @padval value; if
 * the block is already aligned then the original @bytes is returned.
 *
 * Returns: (transfer full): a #GBytes, possibly @bytes
 *
 * Since: 1.2.4
 **/
GBytes *
fu_common_bytes_align (GBytes *bytes, gsize blksz, gchar padval)
{
	const guint8 *data;
	gsize sz;

	g_return_val_if_fail (bytes != NULL, NULL);
	g_return_val_if_fail (blksz > 0, NULL);

	/* pad */
	data = g_bytes_get_data (bytes, &sz);
	if (sz % blksz != 0) {
		gsize sz_align = ((sz / blksz) + 1) * blksz;
		guint8 *data_align = g_malloc (sz_align);
		memcpy (data_align, data, sz);
		memset (data_align + sz, padval, sz_align - sz);
		g_debug ("aligning 0x%x bytes to 0x%x",
			 (guint) sz, (guint) sz_align);
		return g_bytes_new_take (data_align, sz_align);
	}

	/* perfectly aligned */
	return g_bytes_ref (bytes);
}

/**
 * fu_common_bytes_is_empty:
 * @bytes: data blob
 *
 * Checks if a byte array are just empty (0xff) bytes.
 *
 * Returns: %TRUE if @bytes is empty
 *
 * Since: 1.2.6
 **/
gboolean
fu_common_bytes_is_empty (GBytes *bytes)
{
	gsize sz = 0;
	const guint8 *buf = g_bytes_get_data (bytes, &sz);
	for (gsize i = 0; i < sz; i++) {
		if (buf[i] != 0xff)
			return FALSE;
	}
	return TRUE;
}

/**
 * fu_common_bytes_compare_raw:
 * @buf1: a buffer
 * @bufsz1: sizeof @buf1
 * @buf2: another buffer
 * @bufsz2: sizeof @buf2
 * @error: (nullable): optional return location for an error
 *
 * Compares the buffers for equality.
 *
 * Returns: %TRUE if @buf1 and @buf2 are identical
 *
 * Since: 1.3.2
 **/
gboolean
fu_common_bytes_compare_raw (const guint8 *buf1, gsize bufsz1,
			     const guint8 *buf2, gsize bufsz2,
			     GError **error)
{
	g_return_val_if_fail (buf1 != NULL, FALSE);
	g_return_val_if_fail (buf2 != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* not the same length */
	if (bufsz1 != bufsz2) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "got %" G_GSIZE_FORMAT " bytes, expected "
			     "%" G_GSIZE_FORMAT, bufsz1, bufsz2);
		return FALSE;
	}

	/* check matches */
	for (guint i = 0x0; i < bufsz1; i++) {
		if (buf1[i] != buf2[i]) {
			g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "got 0x%02x, expected 0x%02x @ 0x%04x",
			     buf1[i], buf2[i], i);
			return FALSE;
		}
	}

	/* success */
	return TRUE;
}

/**
 * fu_common_bytes_compare:
 * @bytes1: a data blob
 * @bytes2: another #GBytes
 * @error: (nullable): optional return location for an error
 *
 * Compares the buffers for equality.
 *
 * Returns: %TRUE if @bytes1 and @bytes2 are identical
 *
 * Since: 1.2.6
 **/
gboolean
fu_common_bytes_compare (GBytes *bytes1, GBytes *bytes2, GError **error)
{
	const guint8 *buf1;
	const guint8 *buf2;
	gsize bufsz1;
	gsize bufsz2;

	g_return_val_if_fail (bytes1 != NULL, FALSE);
	g_return_val_if_fail (bytes2 != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	buf1 = g_bytes_get_data (bytes1, &bufsz1);
	buf2 = g_bytes_get_data (bytes2, &bufsz2);
	return fu_common_bytes_compare_raw (buf1, bufsz1, buf2, bufsz2, error);
}

/**
 * fu_common_bytes_pad:
 * @bytes: data blob
 * @sz: the desired size in bytes
 *
 * Pads a GBytes to a minimum @sz with `0xff`.
 *
 * Returns: (transfer full): a data blob
 *
 * Since: 1.3.1
 **/
GBytes *
fu_common_bytes_pad (GBytes *bytes, gsize sz)
{
	gsize bytes_sz;

	g_return_val_if_fail (bytes != NULL, NULL);
	g_return_val_if_fail (sz != 0, NULL);

	/* pad */
	bytes_sz = g_bytes_get_size (bytes);
	if (bytes_sz < sz) {
		const guint8 *data = g_bytes_get_data (bytes, NULL);
		guint8 *data_new = g_malloc (sz);
		memcpy (data_new, data, bytes_sz);
		memset (data_new + bytes_sz, 0xff, sz - bytes_sz);
		return g_bytes_new_take (data_new, sz);
	}

	/* not required */
	return g_bytes_ref (bytes);
}

/**
 * fu_common_bytes_new_offset:
 * @bytes: data blob
 * @offset: where subsection starts at
 * @length: length of subsection
 * @error: (nullable): optional return location for an error
 *
 * Creates a #GBytes which is a subsection of another #GBytes.
 *
 * Returns: (transfer full): a #GBytes, or #NULL if range is invalid
 *
 * Since: 1.5.4
 **/
GBytes *
fu_common_bytes_new_offset (GBytes *bytes,
			    gsize offset,
			    gsize length,
			    GError **error)
{
	g_return_val_if_fail (bytes != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* sanity check */
	if (offset + length > g_bytes_get_size (bytes)) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "cannot create bytes @0x%02x for 0x%02x "
			     "as buffer only 0x%04x bytes in size",
			     (guint) offset,
			     (guint) length,
			     (guint) g_bytes_get_size (bytes));
		return NULL;
	}
	return g_bytes_new_from_bytes (bytes, offset, length);
}

/**
 * fu_common_realpath:
 * @filename: a filename
 * @error: (nullable): optional return location for an error
 *
 * Finds the canonicalized absolute filename for a path.
 *
 * Returns: a filename, or %NULL if invalid or not found
 *
 * Since: 1.2.6
 **/
gchar *
fu_common_realpath (const gchar *filename, GError **error)
{
	char full_tmp[PATH_MAX];

	g_return_val_if_fail (filename != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

#ifdef HAVE_REALPATH
	if (realpath (filename, full_tmp) == NULL) {
#else
	if (_fullpath (full_tmp, filename, sizeof(full_tmp)) == NULL) {
#endif
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "cannot resolve path: %s",
			     strerror (errno));
		return NULL;
	}
	if (!g_file_test (full_tmp, G_FILE_TEST_EXISTS)) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "cannot find path: %s",
			     full_tmp);
		return NULL;
	}
	return g_strdup (full_tmp);
}

/**
 * fu_common_fnmatch:
 * @pattern: a glob pattern, e.g. `*foo*`
 * @str: a string to match against the pattern, e.g. `bazfoobar`
 *
 * Matches a string against a glob pattern.
 *
 * Returns: %TRUE if the string matched
 *
 * Since: 1.3.5
 **/
gboolean
fu_common_fnmatch (const gchar *pattern, const gchar *str)
{
	g_return_val_if_fail (pattern != NULL, FALSE);
	g_return_val_if_fail (str != NULL, FALSE);
	return fu_common_fnmatch_impl (pattern, str);
}

static gint
fu_common_filename_glob_sort_cb (gconstpointer a, gconstpointer b)
{
	return g_strcmp0 (*(const gchar **)a, *(const gchar **)b);
}

/**
 * fu_common_filename_glob:
 * @directory: a directory path
 * @pattern: a glob pattern, e.g. `*foo*`
 * @error: (nullable): optional return location for an error
 *
 * Returns all the filenames that match a specific glob pattern.
 * Any results are sorted. No matching files will set @error.
 *
 * Returns:  (element-type utf8) (transfer container): matching files, or %NULL
 *
 * Since: 1.5.0
 **/
GPtrArray *
fu_common_filename_glob (const gchar *directory, const gchar *pattern, GError **error)
{
	const gchar *basename;
	g_autoptr(GDir) dir = NULL;
	g_autoptr(GPtrArray) files = g_ptr_array_new_with_free_func (g_free);

	g_return_val_if_fail (directory != NULL, NULL);
	g_return_val_if_fail (pattern != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	dir = g_dir_open (directory, 0, error);
	if (dir == NULL)
		return NULL;
	while ((basename = g_dir_read_name (dir)) != NULL) {
		if (!fu_common_fnmatch (pattern, basename))
			continue;
		g_ptr_array_add (files, g_build_filename (directory, basename, NULL));
	}
	if (files->len == 0) {
		g_set_error_literal (error,
				     G_IO_ERROR,
				     G_IO_ERROR_NOT_FOUND,
				     "no files matched pattern");
		return NULL;
	}
	g_ptr_array_sort (files, fu_common_filename_glob_sort_cb);
	return g_steal_pointer (&files);
}

/**
 * fu_common_strnsplit:
 * @str: a string to split
 * @sz: size of @str
 * @delimiter: a string which specifies the places at which to split the string
 * @max_tokens: the maximum number of pieces to split @str into
 *
 * Splits a string into a maximum of @max_tokens pieces, using the given
 * delimiter. If @max_tokens is reached, the remainder of string is appended
 * to the last token.
 *
 * Returns: (transfer full): a newly-allocated NULL-terminated array of strings
 *
 * Since: 1.3.1
 **/
gchar **
fu_common_strnsplit (const gchar *str, gsize sz,
		     const gchar *delimiter, gint max_tokens)
{
	if (str[sz - 1] != '\0') {
		g_autofree gchar *str2 = g_strndup (str, sz);
		return g_strsplit (str2, delimiter, max_tokens);
	}
	return g_strsplit (str, delimiter, max_tokens);
}

/**
 * fu_common_strsafe:
 * @str: (nullable): a string to make safe for printing
 * @maxsz: maximum size of returned string
 *
 * Converts a string into something that can be safely printed.
 *
 * Returns: (transfer full): safe string, or %NULL if there was nothing valid
 *
 * Since: 1.5.5
 **/
gchar *
fu_common_strsafe (const gchar *str, gsize maxsz)
{
	gboolean valid = FALSE;
	g_autoptr(GString) tmp = NULL;

	/* sanity check */
	if (str == NULL || maxsz == 0)
		return NULL;

	/* replace non-printable chars with '.' */
	tmp = g_string_sized_new (maxsz);
	for (gsize i = 0; i < maxsz && str[i] != '\0'; i++) {
		if (!g_ascii_isprint (str[i])) {
			g_string_append_c (tmp, '.');
			continue;
		}
		g_string_append_c (tmp, str[i]);
		if (!g_ascii_isspace (str[i]))
			valid = TRUE;
	}

	/* if just junk, don't return 'all dots' */
	if (tmp->len == 0 || !valid)
		return NULL;
	return g_string_free (g_steal_pointer (&tmp), FALSE);
}


/**
 * fu_common_strjoin_array:
 * @separator: (nullable): string to insert between each of the strings
 * @array: (element-type utf8): a #GPtrArray
 *
 * Joins an array of strings together to form one long string, with the optional
 * separator inserted between each of them.
 *
 * If @array has no items, the return value will be an empty string.
 * If @array contains a single item, separator will not appear in the resulting
 * string.
 *
 * Returns: a string
 *
 * Since: 1.5.6
 **/
gchar *
fu_common_strjoin_array (const gchar *separator, GPtrArray *array)
{
	g_autofree const gchar **strv = NULL;

	g_return_val_if_fail (array != NULL, NULL);

	strv = g_new0 (const gchar *, array->len + 1);
	for (guint i = 0; i < array->len; i++)
		strv[i] = g_ptr_array_index (array, i);
	return g_strjoinv (separator, (gchar **) strv);
}

/**
 * fu_memcpy_safe:
 * @dst: destination buffer
 * @dst_sz: maximum size of @dst, typically `sizeof(dst)`
 * @dst_offset: offset in bytes into @dst to copy to
 * @src: source buffer
 * @src_sz: maximum size of @dst, typically `sizeof(src)`
 * @src_offset: offset in bytes into @src to copy from
 * @n: number of bytes to copy from @src+@offset from
 * @error: (nullable): optional return location for an error
 *
 * Copies some memory using memcpy in a safe way. Providing the buffer sizes
 * of both the destination and the source allows us to check for buffer overflow.
 *
 * Providing the buffer offsets also allows us to check reading past the end of
 * the source buffer. For this reason the caller should NEVER add an offset to
 * @src or @dst.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if the bytes were copied, %FALSE otherwise
 *
 * Since: 1.3.1
 **/
gboolean
fu_memcpy_safe (guint8 *dst, gsize dst_sz, gsize dst_offset,
		const guint8 *src, gsize src_sz, gsize src_offset,
		gsize n, GError **error)
{
	g_return_val_if_fail (dst != NULL, FALSE);
	g_return_val_if_fail (src != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	if (n == 0)
		return TRUE;

	if (n > src_sz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_READ,
			     "attempted to read 0x%02x bytes from buffer of 0x%02x",
			     (guint) n, (guint) src_sz);
		return FALSE;
	}
	if (n + src_offset > src_sz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_READ,
			     "attempted to read 0x%02x bytes at offset 0x%02x from buffer of 0x%02x",
			     (guint) n, (guint) src_offset, (guint) src_sz);
		return FALSE;
	}
	if (n > dst_sz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "attempted to write 0x%02x bytes to buffer of 0x%02x",
			     (guint) n, (guint) dst_sz);
		return FALSE;
	}
	if (n + dst_offset > dst_sz) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_WRITE,
			     "attempted to write 0x%02x bytes at offset 0x%02x to buffer of 0x%02x",
			     (guint) n, (guint) dst_offset, (guint) dst_sz);
		return FALSE;
	}

	/* phew! */
	memcpy (dst + dst_offset, src + src_offset, n);
	return TRUE;
}

/**
 * fu_memdup_safe:
 * @src: source buffer
 * @n: number of bytes to copy from @src
 * @error: (nullable): optional return location for an error
 *
 * Duplicates some memory using memdup in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * NOTE: This function intentionally limits allocation size to 1GB.
 *
 * Returns: (transfer full): block of allocated memory, or %NULL for an error.
 *
 * Since: 1.5.6
 **/
guint8 *
fu_memdup_safe (const guint8 *src, gsize n, GError **error)
{
	/* sanity check */
	if (n > 0x40000000) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "cannot allocate %uGB of memory",
			     (guint) (n / 0x40000000));
		return NULL;
	}

#if GLIB_CHECK_VERSION(2,67,3)
	/* linear block of memory */
	return g_memdup2 (src, n);
#else
	return g_memdup (src, (guint) n);
#endif
}

/**
 * fu_common_read_uint8_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to copy from
 * @value: (out) (nullable): the parsed value
 * @error: (nullable): optional return location for an error
 *
 * Read a value from a buffer in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was set, %FALSE otherwise
 *
 * Since: 1.3.3
 **/
gboolean
fu_common_read_uint8_safe (const guint8 *buf,
			   gsize bufsz,
			   gsize offset,
			   guint8 *value,
			   GError **error)
{
	guint8 tmp;

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	if (!fu_memcpy_safe (&tmp, sizeof(tmp), 0x0,	/* dst */
			     buf, bufsz, offset,	/* src */
			     sizeof(tmp), error))
		return FALSE;
	if (value != NULL)
		*value = tmp;
	return TRUE;
}

/**
 * fu_common_read_uint16_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to copy from
 * @value: (out) (nullable): the parsed value
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 * @error: (nullable): optional return location for an error
 *
 * Read a value from a buffer using a specified endian in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was set, %FALSE otherwise
 *
 * Since: 1.3.3
 **/
gboolean
fu_common_read_uint16_safe (const guint8 *buf,
			    gsize bufsz,
			    gsize offset,
			    guint16 *value,
			    FuEndianType endian,
			    GError **error)
{
	guint8 dst[2] = { 0x0 };

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	if (!fu_memcpy_safe (dst, sizeof(dst), 0x0,	/* dst */
			     buf, bufsz, offset,	/* src */
			     sizeof(dst), error))
		return FALSE;
	if (value != NULL)
		*value = fu_common_read_uint16 (dst, endian);
	return TRUE;
}

/**
 * fu_common_read_uint32_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to copy from
 * @value: (out) (nullable): the parsed value
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 * @error: (nullable): optional return location for an error
 *
 * Read a value from a buffer using a specified endian in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was set, %FALSE otherwise
 *
 * Since: 1.3.3
 **/
gboolean
fu_common_read_uint32_safe (const guint8 *buf,
			    gsize bufsz,
			    gsize offset,
			    guint32 *value,
			    FuEndianType endian,
			    GError **error)
{
	guint8 dst[4] = { 0x0 };

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	if (!fu_memcpy_safe (dst, sizeof(dst), 0x0,	/* dst */
			     buf, bufsz, offset,	/* src */
			     sizeof(dst), error))
		return FALSE;
	if (value != NULL)
		*value = fu_common_read_uint32 (dst, endian);
	return TRUE;
}

/**
 * fu_common_read_uint64_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to copy from
 * @value: (out) (nullable): the parsed value
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 * @error: (nullable): optional return location for an error
 *
 * Read a value from a buffer using a specified endian in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was set, %FALSE otherwise
 *
 * Since: 1.5.8
 **/
gboolean
fu_common_read_uint64_safe (const guint8 *buf,
			    gsize bufsz,
			    gsize offset,
			    guint64 *value,
			    FuEndianType endian,
			    GError **error)
{
	guint8 dst[8] = { 0x0 };

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	if (!fu_memcpy_safe (dst, sizeof(dst), 0x0,	/* dst */
			     buf, bufsz, offset,	/* src */
			     sizeof(dst), error))
		return FALSE;
	if (value != NULL)
		*value = fu_common_read_uint64 (dst, endian);
	return TRUE;
}

/**
 * fu_common_write_uint8_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to write to
 * @value: the value to write
 * @error: (nullable): optional return location for an error
 *
 * Write a value to a buffer in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was written, %FALSE otherwise
 *
 * Since: 1.5.8
 **/
gboolean
fu_common_write_uint8_safe (guint8 *buf,
			    gsize bufsz,
			    gsize offset,
			    guint8 value,
			    GError **error)
{
	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	return fu_memcpy_safe (buf, bufsz, offset,		/* dst */
			       &value, sizeof(value), 0x0,	/* src */
			       sizeof(value), error);
}

/**
 * fu_common_write_uint16_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to write to
 * @value: the value to write
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 * @error: (nullable): optional return location for an error
 *
 * Write a value to a buffer using a specified endian in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was written, %FALSE otherwise
 *
 * Since: 1.5.8
 **/
gboolean
fu_common_write_uint16_safe (guint8 *buf,
			     gsize bufsz,
			     gsize offset,
			     guint16 value,
			     FuEndianType endian,
			     GError **error)
{
	guint8 tmp[2] = { 0x0 };

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	fu_common_write_uint16 (tmp, value, endian);
	return fu_memcpy_safe (buf, bufsz, offset,	/* dst */
			       tmp, sizeof(tmp), 0x0,	/* src */
			       sizeof(tmp), error);
}

/**
 * fu_common_write_uint32_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to write to
 * @value: the value to write
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 * @error: (nullable): optional return location for an error
 *
 * Write a value to a buffer using a specified endian in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was written, %FALSE otherwise
 *
 * Since: 1.5.8
 **/
gboolean
fu_common_write_uint32_safe (guint8 *buf,
			     gsize bufsz,
			     gsize offset,
			     guint32 value,
			     FuEndianType endian,
			     GError **error)
{
	guint8 tmp[4] = { 0x0 };

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	fu_common_write_uint32 (tmp, value, endian);
	return fu_memcpy_safe (buf, bufsz, offset,	/* dst */
			       tmp, sizeof(tmp), 0x0,	/* src */
			       sizeof(tmp), error);
}

/**
 * fu_common_write_uint64_safe:
 * @buf: source buffer
 * @bufsz: maximum size of @buf, typically `sizeof(buf)`
 * @offset: offset in bytes into @buf to write to
 * @value: the value to write
 * @endian: an endian type, e.g. %G_LITTLE_ENDIAN
 * @error: (nullable): optional return location for an error
 *
 * Write a value to a buffer using a specified endian in a safe way.
 *
 * You don't need to use this function in "obviously correct" cases, nor should
 * you use it when performance is a concern. Only us it when you're not sure if
 * malicious data from a device or firmware could cause memory corruption.
 *
 * Returns: %TRUE if @value was written, %FALSE otherwise
 *
 * Since: 1.5.8
 **/
gboolean
fu_common_write_uint64_safe (guint8 *buf,
			     gsize bufsz,
			     gsize offset,
			     guint64 value,
			     FuEndianType endian,
			     GError **error)
{
	guint8 tmp[8] = { 0x0 };

	g_return_val_if_fail (buf != NULL, FALSE);
	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	fu_common_write_uint64 (tmp, value, endian);
	return fu_memcpy_safe (buf, bufsz, offset,	/* dst */
			       tmp, sizeof(tmp), 0x0,	/* src */
			       sizeof(tmp), error);
}

/**
 * fu_byte_array_append_uint8:
 * @array: a #GByteArray
 * @data: value
 *
 * Adds a 8 bit integer to a byte array.
 *
 * Since: 1.3.1
 **/
void
fu_byte_array_append_uint8 (GByteArray *array, guint8 data)
{
	g_byte_array_append (array, &data, sizeof(data));
}

/**
 * fu_byte_array_append_uint16:
 * @array: a #GByteArray
 * @data: value
 * @endian: endian type, e.g. #G_LITTLE_ENDIAN
 *
 * Adds a 16 bit integer to a byte array.
 *
 * Since: 1.3.1
 **/
void
fu_byte_array_append_uint16 (GByteArray *array, guint16 data, FuEndianType endian)
{
	guint8 buf[2];
	fu_common_write_uint16 (buf, data, endian);
	g_byte_array_append (array, buf, sizeof(buf));
}

/**
 * fu_byte_array_append_uint32:
 * @array: a #GByteArray
 * @data: value
 * @endian: endian type, e.g. #G_LITTLE_ENDIAN
 *
 * Adds a 32 bit integer to a byte array.
 *
 * Since: 1.3.1
 **/
void
fu_byte_array_append_uint32 (GByteArray *array, guint32 data, FuEndianType endian)
{
	guint8 buf[4];
	fu_common_write_uint32 (buf, data, endian);
	g_byte_array_append (array, buf, sizeof(buf));
}

/**
 * fu_byte_array_append_uint64:
 * @array: a #GByteArray
 * @data: value
 * @endian: endian type, e.g. #G_LITTLE_ENDIAN
 *
 * Adds a 64 bit integer to a byte array.
 *
 * Since: 1.5.8
 **/
void
fu_byte_array_append_uint64 (GByteArray *array, guint64 data, FuEndianType endian)
{
	guint8 buf[8];
	fu_common_write_uint64 (buf, data, endian);
	g_byte_array_append (array, buf, sizeof(buf));
}

/**
 * fu_byte_array_append_bytes:
 * @array: a #GByteArray
 * @bytes: data blob
 *
 * Adds the contents of a GBytes to a byte array.
 *
 * Since: 1.5.8
 **/
void
fu_byte_array_append_bytes (GByteArray *array, GBytes *bytes)
{
	g_byte_array_append (array,
			     g_bytes_get_data (bytes, NULL),
			     g_bytes_get_size (bytes));
}

/**
 * fu_byte_array_set_size_full:
 * @array: a #GByteArray
 * @length:  the new size of the GByteArray
 * @data: the byte used to pad the array
 *
 * Sets the size of the GByteArray, expanding with @data as required.
 *
 * Since: 1.6.0
 **/
void
fu_byte_array_set_size_full (GByteArray *array, guint length, guint8 data)
{
	guint oldlength = array->len;
	g_byte_array_set_size (array, length);
	if (length > oldlength)
		memset (array->data + oldlength, data, length - oldlength);
}

/**
 * fu_byte_array_set_size:
 * @array: a #GByteArray
 * @length: the new size of the GByteArray
 *
 * Sets the size of the GByteArray, expanding it with NULs if necessary.
 *
 * Since: 1.5.0
 **/
void
fu_byte_array_set_size (GByteArray *array, guint length)
{
	return fu_byte_array_set_size_full (array, length, 0x0);
}

/**
 * fu_byte_array_align_up:
 * @array: a #GByteArray
 * @alignment: align to this power of 2
 * @data: the byte used to pad the array
 *
 * Align a byte array length to a power of 2 boundary, where @alignment is the
 * bit position to align to. If @alignment is zero then @array is unchanged.
 *
 * Since: 1.6.0
 **/
void
fu_byte_array_align_up (GByteArray *array, guint8 alignment, guint8 data)
{
	fu_byte_array_set_size_full (array,
				     fu_common_align_up (array->len, alignment),
				     data);
}

/**
 * fu_common_kernel_locked_down:
 *
 * Determines if kernel lockdown in effect
 *
 * Since: 1.3.8
 **/
gboolean
fu_common_kernel_locked_down (void)
{
#ifdef __linux__
	gsize len = 0;
	g_autofree gchar *dir = fu_common_get_path (FU_PATH_KIND_SYSFSDIR_SECURITY);
	g_autofree gchar *fname = g_build_filename (dir, "lockdown", NULL);
	g_autofree gchar *data = NULL;
	g_auto(GStrv) options = NULL;

	if (!g_file_test (fname, G_FILE_TEST_EXISTS))
		return FALSE;
	if (!g_file_get_contents (fname, &data, &len, NULL))
		return FALSE;
	if (len < 1)
		return FALSE;
	options = g_strsplit (data, " ", -1);
	for (guint i = 0; options[i] != NULL; i++) {
		if (g_strcmp0 (options[i], "[none]") == 0)
			return FALSE;
	}
	return TRUE;
#else
	return FALSE;
#endif
}

/**
 * fu_common_check_kernel_version :
 * @minimum_kernel: (not nullable): The minimum kernel version to check against
 * @error: (nullable): optional return location for an error
 *
 * Determines if the system is running at least a certain required kernel version
 *
 * Since: 1.6.2
 **/
gboolean
fu_common_check_kernel_version (const gchar *minimum_kernel, GError **error)
{
#ifdef HAVE_UTSNAME_H
	struct utsname name_tmp;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
	g_return_val_if_fail (minimum_kernel != NULL, FALSE);

	memset (&name_tmp, 0, sizeof(struct utsname));
	if (uname (&name_tmp) < 0) {
		g_set_error_literal (error,
				     FWUPD_ERROR,
				     FWUPD_ERROR_INTERNAL,
				     "failed to read kernel version");
		return FALSE;
	}
	if (fu_common_vercmp_full (name_tmp.release,
				   minimum_kernel,
				   FWUPD_VERSION_FORMAT_TRIPLET) < 0) {
		g_set_error (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "kernel %s doesn't meet minimum %s",
			     name_tmp.release, minimum_kernel);
		return FALSE;
	}

	return TRUE;
#else
	g_set_error_literal (error,
			     FWUPD_ERROR,
			     FWUPD_ERROR_INTERNAL,
			     "platform doesn't support checking for minimum Linux kernel");
	return FALSE;
#endif

}

/**
 * fu_common_cpuid:
 * @leaf: the CPUID level, now called the 'leaf' by Intel
 * @eax: (out) (nullable): EAX register
 * @ebx: (out) (nullable): EBX register
 * @ecx: (out) (nullable): ECX register
 * @edx: (out) (nullable): EDX register
 * @error: (nullable): optional return location for an error
 *
 * Calls CPUID and returns the registers for the given leaf.
 *
 * Returns: %TRUE if the registers are set.
 *
 * Since: 1.5.0
 **/
gboolean
fu_common_cpuid (guint32 leaf,
		 guint32 *eax,
		 guint32 *ebx,
		 guint32 *ecx,
		 guint32 *edx,
		 GError **error)
{
#ifdef HAVE_CPUID_H
	guint eax_tmp = 0;
	guint ebx_tmp = 0;
	guint ecx_tmp = 0;
	guint edx_tmp = 0;

	g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

	/* get vendor */
	__get_cpuid_count (leaf, 0x0, &eax_tmp, &ebx_tmp, &ecx_tmp, &edx_tmp);
	if (eax != NULL)
		*eax = eax_tmp;
	if (ebx != NULL)
		*ebx = ebx_tmp;
	if (ecx != NULL)
		*ecx = ecx_tmp;
	if (edx != NULL)
		*edx = edx_tmp;
	return TRUE;
#else
	g_set_error_literal (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_SUPPORTED,
			     "no <cpuid.h> support");
	return FALSE;
#endif
}

/**
 * fu_common_get_cpu_vendor:
 *
 * Uses CPUID to discover the CPU vendor.
 *
 * Returns: a CPU vendor, e.g. %FU_CPU_VENDOR_AMD if the vendor was AMD.
 *
 * Since: 1.5.5
 **/
FuCpuVendor
fu_common_get_cpu_vendor (void)
{
#ifdef HAVE_CPUID_H
	guint ebx = 0;
	guint ecx = 0;
	guint edx = 0;

	if (fu_common_cpuid (0x0, NULL, &ebx, &ecx, &edx, NULL)) {
		if (ebx == signature_INTEL_ebx &&
		    edx == signature_INTEL_edx &&
		    ecx == signature_INTEL_ecx) {
			return FU_CPU_VENDOR_INTEL;
		}
		if (ebx == signature_AMD_ebx &&
		    edx == signature_AMD_edx &&
		    ecx == signature_AMD_ecx) {
			return FU_CPU_VENDOR_AMD;
		}
	}
#endif

	/* failed */
	return FU_CPU_VENDOR_UNKNOWN;
}

/**
 * fu_common_is_live_media:
 *
 * Checks if the user is running from a live media using various heuristics.
 *
 * Returns: %TRUE if live
 *
 * Since: 1.4.6
 **/
gboolean
fu_common_is_live_media (void)
{
	gsize bufsz = 0;
	g_autofree gchar *buf = NULL;
	g_auto(GStrv) tokens = NULL;
	const gchar *args[] = {
		"rd.live.image",
		"boot=live",
		NULL, /* last entry */
	};
	if (g_file_test ("/cdrom/.disk/info", G_FILE_TEST_EXISTS))
		return TRUE;
	if (!g_file_get_contents ("/proc/cmdline", &buf, &bufsz, NULL))
		return FALSE;
	if (bufsz == 0)
		return FALSE;
	tokens = fu_common_strnsplit (buf, bufsz - 1, " ", -1);
	for (guint i = 0; args[i] != NULL; i++) {
		if (g_strv_contains ((const gchar * const *) tokens, args[i]))
			return TRUE;
	}
	return FALSE;
}

/**
 * fu_common_get_memory_size:
 *
 * Returns the size of physical memory.
 *
 * Returns: bytes
 *
 * Since: 1.5.6
 **/
guint64
fu_common_get_memory_size (void)
{
	return fu_common_get_memory_size_impl ();
}

const gchar *
fu_common_convert_to_gpt_type (const gchar *type)
{
	struct {
		const gchar *gpt;
		const gchar *mbrs[4];
	} typeguids[] = {
		{ "c12a7328-f81f-11d2-ba4b-00a0c93ec93b",	/* esp */
			{ "0xef", "efi", NULL }},
		{ "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7",	/* fat32 */
			{ "0x0b", "fat32", "fat32lba", NULL }},
		{ NULL, { NULL } }
	};
	for (guint i = 0; typeguids[i].gpt != NULL; i++) {
		for (guint j = 0; typeguids[i].mbrs[j] != NULL; j++) {
			if (g_strcmp0 (type, typeguids[i].mbrs[j]) == 0)
				return typeguids[i].gpt;
		}
	}
	return type;
}

/**
 * fu_common_get_volumes_by_kind:
 * @kind: a volume kind, typically a GUID
 * @error: (nullable): optional return location for an error
 *
 * Finds all volumes of a specific partition type
 *
 * Returns: (transfer container) (element-type FuVolume): a #GPtrArray, or %NULL if the kind was not found
 *
 * Since: 1.4.6
 **/
GPtrArray *
fu_common_get_volumes_by_kind (const gchar *kind, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;
	g_autoptr(GPtrArray) volumes = NULL;

	g_return_val_if_fail (kind != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	devices = fu_common_get_block_devices (error);
	if (devices == NULL)
		return NULL;
	volumes = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
	for (guint i = 0; i < devices->len; i++) {
		GDBusProxy *proxy_blk = g_ptr_array_index (devices, i);
		const gchar *type_str;
		g_autoptr(FuVolume) vol = NULL;
		g_autoptr(GDBusProxy) proxy_part = NULL;
		g_autoptr(GDBusProxy) proxy_fs = NULL;
		g_autoptr(GVariant) val = NULL;

		proxy_part = g_dbus_proxy_new_sync (g_dbus_proxy_get_connection (proxy_blk),
						    G_DBUS_PROXY_FLAGS_NONE, NULL,
						    UDISKS_DBUS_SERVICE,
						    g_dbus_proxy_get_object_path (proxy_blk),
						    UDISKS_DBUS_INTERFACE_PARTITION,
						    NULL, error);
		if (proxy_part == NULL) {
			g_prefix_error (error, "failed to initialize d-bus proxy %s: ",
					g_dbus_proxy_get_object_path (proxy_blk));
			return NULL;
		}
		val = g_dbus_proxy_get_cached_property (proxy_part, "Type");
		if (val == NULL)
			continue;

		g_variant_get (val, "&s", &type_str);
		proxy_fs = g_dbus_proxy_new_sync (g_dbus_proxy_get_connection (proxy_blk),
						  G_DBUS_PROXY_FLAGS_NONE, NULL,
						  UDISKS_DBUS_SERVICE,
						  g_dbus_proxy_get_object_path (proxy_blk),
						  UDISKS_DBUS_INTERFACE_FILESYSTEM,
						  NULL, error);
		if (proxy_fs == NULL) {
			g_prefix_error (error, "failed to initialize d-bus proxy %s: ",
					g_dbus_proxy_get_object_path (proxy_blk));
			return NULL;
		}
		vol = g_object_new (FU_TYPE_VOLUME,
				    "proxy-block", proxy_blk,
				    "proxy-filesystem", proxy_fs,
				    NULL);

		/* convert reported type to GPT type */
		type_str = fu_common_convert_to_gpt_type (type_str);
		g_debug ("device %s, type: %s, internal: %d, fs: %s",
			 g_dbus_proxy_get_object_path (proxy_blk), type_str,
			 fu_volume_is_internal (vol),
			 fu_volume_get_id_type (vol));
		if (g_strcmp0 (type_str, kind) != 0)
			continue;
		g_ptr_array_add (volumes, g_steal_pointer (&vol));
	}
	if (volumes->len == 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_NOT_FOUND,
			     "no volumes of type %s", kind);
		return NULL;
	}
	return g_steal_pointer (&volumes);
}

/**
 * fu_common_get_volume_by_device:
 * @device: a device string, typically starting with `/dev/`
 * @error: (nullable): optional return location for an error
 *
 * Finds the first volume from the specified device.
 *
 * Returns: (transfer full): a volume, or %NULL if the device was not found
 *
 * Since: 1.5.1
 **/
FuVolume *
fu_common_get_volume_by_device (const gchar *device, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	g_return_val_if_fail (device != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find matching block device */
	devices = fu_common_get_block_devices (error);
	if (devices == NULL)
		return NULL;
	for (guint i = 0; i < devices->len; i++) {
		GDBusProxy *proxy_blk = g_ptr_array_index (devices, i);
		g_autoptr(GVariant) val = NULL;
		val = g_dbus_proxy_get_cached_property (proxy_blk, "Device");
		if (val == NULL)
			continue;
		if (g_strcmp0 (g_variant_get_bytestring (val), device) == 0) {
			return g_object_new (FU_TYPE_VOLUME,
					     "proxy-block", proxy_blk,
					     NULL);
		}
	}

	/* failed */
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_NOT_FOUND,
		     "no volumes for device %s",
		     device);
	return NULL;
}

/**
 * fu_common_get_volume_by_devnum:
 * @devnum: a device number
 * @error: (nullable): optional return location for an error
 *
 * Finds the first volume from the specified device.
 *
 * Returns: (transfer full): a volume, or %NULL if the device was not found
 *
 * Since: 1.5.1
 **/
FuVolume *
fu_common_get_volume_by_devnum (guint32 devnum, GError **error)
{
	g_autoptr(GPtrArray) devices = NULL;

	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* find matching block device */
	devices = fu_common_get_block_devices (error);
	if (devices == NULL)
		return NULL;
	for (guint i = 0; i < devices->len; i++) {
		GDBusProxy *proxy_blk = g_ptr_array_index (devices, i);
		g_autoptr(GVariant) val = NULL;
		val = g_dbus_proxy_get_cached_property (proxy_blk, "DeviceNumber");
		if (val == NULL)
			continue;
		if (devnum == g_variant_get_uint64 (val)) {
			return g_object_new (FU_TYPE_VOLUME,
					     "proxy-block", proxy_blk,
					     NULL);
		}
	}

	/* failed */
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_NOT_FOUND,
		     "no volumes for devnum %u",
		     devnum);
	return NULL;
}

/**
 * fu_common_get_esp_default:
 * @error: (nullable): optional return location for an error
 *
 * Gets the platform default ESP
 *
 * Returns: (transfer full): a volume, or %NULL if the ESP was not found
 *
 * Since: 1.4.6
 **/
FuVolume *
fu_common_get_esp_default (GError **error)
{
	const gchar *path_tmp;
	gboolean has_internal = FALSE;
	g_autoptr(GPtrArray) volumes_fstab = g_ptr_array_new ();
	g_autoptr(GPtrArray) volumes_mtab = g_ptr_array_new ();
	g_autoptr(GPtrArray) volumes_vfat = g_ptr_array_new ();
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GError) error_local = NULL;

	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	/* for the test suite use local directory for ESP */
	path_tmp = g_getenv ("FWUPD_UEFI_ESP_PATH");
	if (path_tmp != NULL)
		return fu_volume_new_from_mount_path (path_tmp);

	volumes = fu_common_get_volumes_by_kind (FU_VOLUME_KIND_ESP, &error_local);
	if (volumes == NULL) {
		g_debug ("%s, falling back to %s", error_local->message, FU_VOLUME_KIND_BDP);
		volumes = fu_common_get_volumes_by_kind (FU_VOLUME_KIND_BDP, error);
		if (volumes == NULL) {
			g_prefix_error (error, "%s: ", error_local->message);
			return NULL;
		}
	}

	/* are there _any_ internal vfat partitions?
	 * remember HintSystem is just that -- a hint! */
	for (guint i = 0; i < volumes->len; i++) {
		FuVolume *vol = g_ptr_array_index (volumes, i);
		g_autofree gchar *type = fu_volume_get_id_type (vol);
		if (g_strcmp0 (type, "vfat") == 0 &&
		    fu_volume_is_internal (vol)) {
			has_internal = TRUE;
			break;
		}
	}

	/* filter to vfat partitions */
	for (guint i = 0; i < volumes->len; i++) {
		FuVolume *vol = g_ptr_array_index (volumes, i);
		g_autofree gchar *type = fu_volume_get_id_type (vol);
		if (type == NULL)
			continue;
		if (has_internal && !fu_volume_is_internal (vol))
			continue;
		if (g_strcmp0 (type, "vfat") == 0)
			g_ptr_array_add (volumes_vfat, vol);
	}
	if (volumes_vfat->len == 0) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_FILENAME,
			     "No ESP found");
		return NULL;
	}
	for (guint i = 0; i < volumes_vfat->len; i++) {
		FuVolume *vol = g_ptr_array_index (volumes_vfat, i);
		g_ptr_array_add (fu_volume_is_mounted (vol) ? volumes_mtab : volumes_fstab, vol);
	}
	if (volumes_mtab->len == 1) {
		FuVolume *vol = g_ptr_array_index (volumes_mtab, 0);
		return g_object_ref (vol);
	}
	if (volumes_mtab->len == 0 && volumes_fstab->len == 1) {
		FuVolume *vol = g_ptr_array_index (volumes_fstab, 0);
		return g_object_ref (vol);
	}
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_INVALID_FILENAME,
		     "More than one available ESP");
	return NULL;
}

/**
 * fu_common_get_esp_for_path:
 * @esp_path: a path to the ESP
 * @error: (nullable): optional return location for an error
 *
 * Gets the platform ESP using a UNIX or UDisks path
 *
 * Returns: (transfer full): a #volume, or %NULL if the ESP was not found
 *
 * Since: 1.4.6
 **/
FuVolume *
fu_common_get_esp_for_path (const gchar *esp_path, GError **error)
{
	g_autofree gchar *basename = NULL;
	g_autoptr(GPtrArray) volumes = NULL;
	g_autoptr(GError) error_local = NULL;

	g_return_val_if_fail (esp_path != NULL, NULL);
	g_return_val_if_fail (error == NULL || *error == NULL, NULL);

	volumes = fu_common_get_volumes_by_kind (FU_VOLUME_KIND_ESP, &error_local);
	if (volumes == NULL) {
		/* check if it's a valid directory already */
		if (g_file_test (esp_path, G_FILE_TEST_IS_DIR))
			return fu_volume_new_from_mount_path (esp_path);
		g_propagate_error (error, g_steal_pointer (&error_local));
		return NULL;
	}
	basename = g_path_get_basename (esp_path);
	for (guint i = 0; i < volumes->len; i++) {
		FuVolume *vol = g_ptr_array_index (volumes, i);
		g_autofree gchar *vol_basename = g_path_get_basename (fu_volume_get_mount_point (vol));
		if (g_strcmp0 (basename, vol_basename) == 0)
			return g_object_ref (vol);
	}
	g_set_error (error,
		     G_IO_ERROR,
		     G_IO_ERROR_INVALID_FILENAME,
		     "No ESP with path %s",
		     esp_path);
	return NULL;
}

/**
 * fu_common_crc8:
 * @buf: memory buffer
 * @bufsz: size of @buf
 *
 * Returns the cyclic redundancy check value for the given memory buffer.
 *
 * Returns: CRC value
 *
 * Since: 1.5.0
 **/
guint8
fu_common_crc8 (const guint8 *buf, gsize bufsz)
{
	guint32 crc = 0;
	for (gsize j = bufsz; j > 0; j--) {
		crc ^= (*(buf++) << 8);
		for (guint32 i = 8; i; i--) {
			if (crc & 0x8000)
				crc ^= (0x1070 << 3);
			crc <<= 1;
		}
	}
	return ~((guint8) (crc >> 8));
}

/**
 * fu_common_crc16_full:
 * @buf: memory buffer
 * @bufsz: size of @buf
 * @crc: initial CRC value, typically 0xFFFF
 * @polynomial: CRC polynomial, typically 0xA001 for IBM or 0x1021 for CCITT
 *
 * Returns the cyclic redundancy check value for the given memory buffer.
 *
 * Returns: CRC value
 *
 * Since: 1.6.2
 **/
guint16
fu_common_crc16_full (const guint8 *buf, gsize bufsz, guint16 crc, guint16 polynomial)
{
	for (gsize len = bufsz; len > 0; len--) {
		crc = (guint16) (crc ^ (*buf++));
		for (guint8 i = 0; i < 8; i++) {
			if (crc & 0x1) {
				crc = (crc >> 1) ^ polynomial;
			} else {
				crc >>= 1;
			}
		}
	}
	return ~crc;
}

/**
 * fu_common_crc16:
 * @buf: memory buffer
 * @bufsz: size of @buf
 *
 * Returns the CRC-16-IBM cyclic redundancy value for the given memory buffer.
 *
 * Returns: CRC value
 *
 * Since: 1.5.0
 **/
guint16
fu_common_crc16 (const guint8 *buf, gsize bufsz)
{
	return fu_common_crc16_full (buf, bufsz, 0xFFFF, 0xA001);
}

/**
 * fu_common_crc32_full:
 * @buf: memory buffer
 * @bufsz: size of @buf
 * @crc: initial CRC value, typically 0xFFFFFFFF
 * @polynomial: CRC polynomial, typically 0xEDB88320
 *
 * Returns the cyclic redundancy check value for the given memory buffer.
 *
 * Returns: CRC value
 *
 * Since: 1.5.0
 **/
guint32
fu_common_crc32_full (const guint8 *buf, gsize bufsz, guint32 crc, guint32 polynomial)
{
	for (guint32 idx = 0; idx < bufsz; idx++) {
		guint8 data = *buf++;
		crc = crc ^ data;
		for (guint32 bit = 0; bit < 8; bit++) {
			guint32 mask = -(crc & 1);
			crc = (crc >> 1) ^ (polynomial & mask);
		}
	}
	return ~crc;
}

/**
 * fu_common_crc32:
 * @buf: memory buffer
 * @bufsz: size of @buf
 *
 * Returns the cyclic redundancy check value for the given memory buffer.
 *
 * Returns: CRC value
 *
 * Since: 1.5.0
 **/
guint32
fu_common_crc32 (const guint8 *buf, gsize bufsz)
{
	return fu_common_crc32_full (buf, bufsz, 0xFFFFFFFF, 0xEDB88320);
}

/**
 * fu_common_uri_get_scheme:
 * @uri: valid URI, e.g. `https://foo.bar/baz`
 *
 * Returns the USI scheme for the given URI.
 *
 * Returns: scheme value, or %NULL if invalid, e.g. `https`
 *
 * Since: 1.5.6
 **/
gchar *
fu_common_uri_get_scheme (const gchar *uri)
{
	gchar *tmp;

	g_return_val_if_fail (uri != NULL, NULL);

	tmp = g_strstr_len (uri, -1, ":");
	if (tmp == NULL || tmp[0] == '\0')
		return NULL;
	return g_utf8_strdown (uri, tmp - uri);
}

/**
 * fu_common_align_up:
 * @value: value to align
 * @alignment: align to this power of 2, where 0x1F is the maximum value of 2GB
 *
 * Align a value to a power of 2 boundary, where @alignment is the bit position
 * to align to. If @alignment is zero then @value is always returned unchanged.
 *
 * Returns: aligned value, which will be the same as @value if already aligned,
 * 		or %G_MAXSIZE if the value would overflow
 *
 * Since: 1.6.0
 **/
gsize
fu_common_align_up (gsize value, guint8 alignment)
{
	gsize value_new;
	guint32 mask = 1 << alignment;

	g_return_val_if_fail (alignment <= FU_FIRMWARE_ALIGNMENT_2G, G_MAXSIZE);

	/* no alignment required */
	if ((value & (mask - 1)) == 0)
		return value;

	/* increment up to the next alignment value */
	value_new = value + mask;
	value_new &= ~(mask - 1);

	/* overflow */
	if (value_new < value)
		return G_MAXSIZE;

	/* success */
	return value_new;
}

/**
 * fu_battery_state_to_string:
 * @battery_state: a battery state, e.g. %FU_BATTERY_STATE_FULLY_CHARGED
 *
 * Converts an enumerated type to a string.
 *
 * Returns: a string, or %NULL for invalid
 *
 * Since: 1.6.0
 **/
const gchar *
fu_battery_state_to_string (FuBatteryState battery_state)
{
	if (battery_state == FU_BATTERY_STATE_UNKNOWN)
		return "unknown";
	if (battery_state == FU_BATTERY_STATE_CHARGING)
		return "charging";
	if (battery_state == FU_BATTERY_STATE_DISCHARGING)
		return "discharging";
	if (battery_state == FU_BATTERY_STATE_EMPTY)
		return "empty";
	if (battery_state == FU_BATTERY_STATE_FULLY_CHARGED)
		return "fully-charged";
	return NULL;
}

/**
 * fu_bytes_get_data_safe:
 * @bytes: data blob
 * @bufsz: (out) (optional): location to return size of byte data
 * @error: (nullable): optional return location for an error
 *
 * Get the byte data in the #GBytes. This data should not be modified.
 * This function will always return the same pointer for a given #GBytes.
 *
 * If the size of @bytes is zero, then %NULL is returned and the @error is set,
 * which differs in behavior to that of g_bytes_get_data().
 *
 * This may be useful when calling g_mapped_file_new() on a zero-length file.
 *
 * Returns: a pointer to the byte data, or %NULL.
 *
 * Since: 1.6.0
 **/
const guint8 *
fu_bytes_get_data_safe (GBytes *bytes, gsize *bufsz, GError **error)
{
	const guint8 *buf = g_bytes_get_data (bytes, bufsz);
	if (buf == NULL) {
		g_set_error (error,
			     G_IO_ERROR,
			     G_IO_ERROR_INVALID_DATA,
			     "invalid data");
		return NULL;
	}
	return buf;
}

/**
 * fu_xmlb_builder_insert_kv:
 * @bn: #XbBuilderNode
 * @key: string key
 * @value: string value
 *
 * Convenience function to add an XML node with a string value. If @value is %NULL
 * then no member is added.
 *
 * Since: 1.6.0
 **/
void
fu_xmlb_builder_insert_kv (XbBuilderNode *bn, const gchar *key, const gchar *value)
{
	if (value == NULL)
		return;
	xb_builder_node_insert_text (bn, key, value, NULL);
}

/**
 * fu_xmlb_builder_insert_kx:
 * @bn: #XbBuilderNode
 * @key: string key
 * @value: integer value
 *
 * Convenience function to add an XML node with an integer value. If @value is 0
 * then no member is added.
 *
 * Since: 1.6.0
 **/
void
fu_xmlb_builder_insert_kx (XbBuilderNode *bn, const gchar *key, guint64 value)
{
	g_autofree gchar *value_hex = NULL;
	if (value == 0)
		return;
	value_hex = g_strdup_printf ("0x%x", (guint) value);
	xb_builder_node_insert_text (bn, key, value_hex, NULL);
}

/**
 * fu_xmlb_builder_insert_kb:
 * @bn: #XbBuilderNode
 * @key: string key
 * @value: boolean value
 *
 * Convenience function to add an XML node with a boolean value.
 *
 * Since: 1.6.0
 **/
void
fu_xmlb_builder_insert_kb (XbBuilderNode *bn, const gchar *key, gboolean value)
{
	xb_builder_node_insert_text (bn, key, value ? "true" : "false", NULL);
}
