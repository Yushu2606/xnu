/*
 * Copyright (c) 2024 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/* compile: xcrun -sdk macosx.internal clang -ldarwintest -lsandbox -o named_fork_path named_fork_path.c -g -Weverything */

#include <sys/xattr.h>
#include <sandbox/libsandbox.h>
#include <TargetConditionals.h>

#include <darwintest.h>
#include <darwintest/utils.h>

#define RUN_TEST     TARGET_OS_OSX

static char template[MAXPATHLEN];
static char *testdir = NULL;
static char rsrc[PATH_MAX];
static char file[PATH_MAX], file_rfork[PATH_MAX];
static char file2[PATH_MAX], file2_rfork[PATH_MAX];
static sandbox_params_t params = NULL;
static sandbox_profile_t profile = NULL;

T_GLOBAL_META(
	T_META_NAMESPACE("xnu.vfs"),
	T_META_RADAR_COMPONENT_NAME("xnu"),
	T_META_RADAR_COMPONENT_VERSION("vfs"),
	T_META_ASROOT(false),
	T_META_ENABLED(RUN_TEST),
	T_META_CHECK_LEAKS(false));

static void
cleanup(void)
{
	if (profile) {
		sandbox_free_profile(profile);
	}
	if (params) {
		sandbox_free_params(params);
	}
	if (file[0] != '\0') {
		unlink(file);
	}
	if (file2[0] != '\0') {
		unlink(file2);
	}
	if (rsrc[0] != '\0') {
		unlink(rsrc);
	}
	if (testdir) {
		rmdir(testdir);
	}
}

static void
create_profile_string(char *buff, size_t size)
{
	snprintf(buff, size, "(version 1) \n\
                          (allow default) \n\
                          (import \"system.sb\") \n\
                          (deny file-read-xattr file-write-xattr (path \"%s\")) \n\
                          (deny file-read-xattr file-write-xattr (path \"%s\")) \n",
	    file, file2);
}

T_DECL(named_fork_path,
    "Named fork paths to check file-read-xattr and file-write-xattr Sandbox permissions")
{
#if (!RUN_TEST)
	T_SKIP("Not macOS");
#endif

	int fd, fd2, fd3, fd_rfork;
	char xattr_buff[100];
	char *sberror = NULL;
	const char *xattr = "test1234";
	char profile_string[1000];
	size_t xattr_len = strlen(xattr);
	char testdir_path[MAXPATHLEN];

	file[0] = file2[0] = rsrc[0] = '\0';

	T_ATEND(cleanup);
	T_SETUPBEGIN;

	/* Create test root dir */
	snprintf(template, sizeof(template), "%s/named_fork_path-XXXXXX", dt_tmpdir());
	T_ASSERT_POSIX_NOTNULL((testdir = mkdtemp(template)), "Creating test root dir");
	T_ASSERT_POSIX_SUCCESS((fd = open(testdir, O_SEARCH, 0777)), "Opening test root directory %s", testdir);
	T_ASSERT_POSIX_SUCCESS(fcntl(fd, F_GETPATH, testdir_path), "Calling fcntl() to get the path");
	T_ASSERT_POSIX_SUCCESS(close(fd), "Closing %s", testdir_path);

	/* Setup file names */
	snprintf(file, sizeof(file), "%s/%s", testdir_path, "file");
	snprintf(file_rfork, sizeof(file_rfork), "%s/..namedfork/rsrc", file);

	snprintf(file2, sizeof(file2), "%s/%s", testdir_path, "file2");
	snprintf(file2_rfork, sizeof(file2_rfork), "%s/..namedfork/rsrc", file2);

	snprintf(rsrc, sizeof(rsrc), "%s/%s", testdir_path, "rsrc");

	/* Create the test files */
	T_ASSERT_POSIX_SUCCESS((fd = open(file, O_CREAT | O_RDWR, 0777)), "Creating %s", file);
	T_ASSERT_POSIX_SUCCESS((fd2 = open(file2, O_CREAT | O_RDWR, 0777)), "Creating %s", file2);
	T_ASSERT_POSIX_SUCCESS((fd3 = open(rsrc, O_CREAT | O_RDWR, 0777)), "Creating %s", rsrc);

	/* Set ResourceFork extended attribute */
	T_ASSERT_POSIX_SUCCESS(fsetxattr(fd, XATTR_RESOURCEFORK_NAME, xattr, xattr_len, 0, 0), "Setting ResourceFork of %s to '%s'", file, xattr);

	/* Create sandbox variables */
	T_ASSERT_POSIX_NOTNULL(params = sandbox_create_params(), "Creating Sandbox params object");
	create_profile_string(profile_string, sizeof(profile_string));
	T_ASSERT_POSIX_NOTNULL(profile = sandbox_compile_string(profile_string, params, &sberror), "Creating Sandbox profile object");

	T_SETUPEND;

	/* Test rename to/from an ..namedfork/rsrc path */
	T_ASSERT_POSIX_FAILURE(rename(file_rfork, rsrc), EPERM, "Verifying rename from an ..namedfork/rsrc path isn't a supported (EPERM)");
	T_ASSERT_POSIX_FAILURE(rename(rsrc, file_rfork), EPERM, "Verifying trename to an ..namedfork/rsrc path isn't a supported (EPERM)");

	/* Read ResourceFork extended attribute using getxattr() */
	T_ASSERT_EQ((ssize_t)xattr_len, fgetxattr(fd, XATTR_RESOURCEFORK_NAME, xattr_buff, sizeof(xattr_buff), 0, 0),
	    "Trying to get ResourceFork extended attribute");
	T_ASSERT_EQ(0, strncmp(xattr, xattr_buff, xattr_len), "Verifying ResourceFork extended content");

	/* Read ResourceFork extended attribute using the ..namedfork/rsrc path */
	T_ASSERT_POSIX_SUCCESS((fd_rfork = open(file_rfork, O_RDONLY, 0777)), "Opening %s", file_rfork);
	T_ASSERT_EQ((ssize_t)xattr_len, read(fd_rfork, xattr_buff, sizeof(xattr_buff)), "Trying to read ResourceFork extended attribute");
	T_ASSERT_EQ(0, strncmp(xattr, xattr_buff, xattr_len), "Verifying ResourceFork extended content");
	T_ASSERT_POSIX_SUCCESS(close(fd_rfork), "Closing %s", file_rfork);

	/* Apply sandbox profile */
	T_ASSERT_POSIX_SUCCESS(sandbox_apply(profile), "Applying Sandbox profile");

	/* Test ResourceFork extended attribute using fgetxattr(), fsetxattr() and fremovexattr() */
	T_ASSERT_POSIX_FAILURE(fgetxattr(fd, XATTR_RESOURCEFORK_NAME, xattr_buff, sizeof(xattr_buff), 0, 0), EPERM, "Verifying that fgetxattr() fails to get ResourceFork with EPERM");
	T_ASSERT_POSIX_FAILURE(fremovexattr(fd, XATTR_RESOURCEFORK_NAME, 0), EPERM, "Verifying that fremovexattr() fails to remove ResourceFork with EPERM");
	T_ASSERT_POSIX_FAILURE(fsetxattr(fd2, XATTR_RESOURCEFORK_NAME, xattr, xattr_len, 0, 0), EPERM, "Verifying that fsetxattr() fails to set ResourceFork with EPERM");

	/* Test ResourceFork extended attribute using the ..namedfork/rsrc path */
	T_ASSERT_POSIX_FAILURE((fd_rfork = open(file_rfork, O_RDONLY, 0777)), EPERM, "Verifying that open() fails with EPERM");
	T_ASSERT_POSIX_FAILURE((fd_rfork = open(file2_rfork, O_CREAT | O_RDONLY, 0777)), EPERM, "Verifying that open(O_CREAT) fails with EPERM");
	T_ASSERT_POSIX_FAILURE(unlink(file_rfork), EPERM, "Verifying that unlink() fails with EPERM");

	/* Close the open files */
	T_ASSERT_POSIX_SUCCESS(close(fd), "Closing %s", file);
	T_ASSERT_POSIX_SUCCESS(close(fd2), "Closing %s", file2);
	T_ASSERT_POSIX_SUCCESS(close(fd3), "Closing %s", rsrc);
}
