/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2006 Ricardo Correia.  All rights reserved.
 * Use is subject to license terms.
 */

#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#include <sys/mntent.h>

#include "libzfs_impl.h"

int zfsfuse_open(const char *pathname, int flags)
{
	struct sockaddr_un name;

	int sock;
	size_t size;

	/* Create the socket. */
	sock = socket(PF_LOCAL, SOCK_STREAM, 0);
	if(sock == -1) {
		perror("socket");
		return -1;
	}

	/* Bind a name to the socket. */
	name.sun_family = AF_LOCAL;
	strncpy(name.sun_path, pathname, sizeof(name.sun_path));

	name.sun_path[sizeof(name.sun_path) - 1] = '\0';

	size = SUN_LEN(&name);

	if(connect(sock, (struct sockaddr *) &name, size) == -1) {
		perror("connect");
		return -1;
	}

	return sock;
}

/*
 * This function is repeated in zfs-fuse/zfsfuse_ioctl.c
 * and in zfs-fuse/fuse_listener.c
 */
int zfsfuse_ioctl_read_loop(int fd, void *buf, int bytes)
{
	int read_bytes = 0;
	int left_bytes = bytes;

	while(left_bytes > 0) {
		int ret = recvfrom(fd, buf + read_bytes, left_bytes, 0, NULL, NULL);
		if(ret == 0) {
			fprintf(stderr, "zfsfuse_ioctl_read_loop(): file descriptor closed\n");
			errno = EIO;
			return -1;
		}
		if(ret == -1) {
// 			perror("recvfrom");
			return -1;
		}
		read_bytes += ret;
		left_bytes -= ret;
	}

	return 0;
}

int zfsfuse_ioctl(int fd, int32_t request, void *arg)
{
	zfsfuse_cmd_t cmd;

	cmd.cmd_type = IOCTL_REQ;
	cmd.cmd_u.ioctl_req.cmd = request;
	cmd.cmd_u.ioctl_req.arg = (uint64_t)(uintptr_t) arg;

	if(write(fd, &cmd, sizeof(zfsfuse_cmd_t)) != sizeof(zfsfuse_cmd_t))
		return -1;

	for(;;) {
		if(zfsfuse_ioctl_read_loop(fd, &cmd, sizeof(zfsfuse_cmd_t)) != 0)
			return -1;

		switch(cmd.cmd_type) {
			case IOCTL_REQ:
			case MOUNT_REQ:
				abort();
			case IOCTL_ANS:
				errno = cmd.cmd_u.ioctl_ans_ret;
				return errno;
			case COPYIN_REQ:
				if(write(fd, (void *)(uintptr_t) cmd.cmd_u.copy_req.ptr, cmd.cmd_u.copy_req.size) != cmd.cmd_u.copy_req.size)
					return -1;
				break;
			case COPYOUT_REQ:
				if(zfsfuse_ioctl_read_loop(fd, (void *)(uintptr_t) cmd.cmd_u.copy_req.ptr, cmd.cmd_u.copy_req.size) != 0)
					return -1;
				break;
		}
	}
}

/* If you change this, check _sol_mount in lib/libsolcompat/include/sys/mount.h */
int zfsfuse_mount(libzfs_handle_t *hdl, const char *spec, const char *dir, int mflag, char *fstype, char *dataptr, int datalen, char *optptr, int optlen)
{
	assert(dataptr == NULL);
	assert(datalen == 0);
	assert(mflag == 0);
	assert(strcmp(fstype, MNTTYPE_ZFS) == 0);

	zfsfuse_cmd_t cmd;

	uint32_t speclen = strlen(spec);
	uint32_t dirlen = strlen(dir);

	cmd.cmd_type = MOUNT_REQ;
	cmd.cmd_u.mount_req.speclen = speclen;
	cmd.cmd_u.mount_req.dirlen = dirlen;
	cmd.cmd_u.mount_req.mflag = mflag;
	cmd.cmd_u.mount_req.optlen = optlen;

	if(write(hdl->libzfs_fd, &cmd, sizeof(zfsfuse_cmd_t)) != sizeof(zfsfuse_cmd_t))
		return -1;

	if(write(hdl->libzfs_fd, spec, speclen) != speclen)
		return -1;

	if(write(hdl->libzfs_fd, dir, dirlen) != dirlen)
		return -1;

	if(write(hdl->libzfs_fd, optptr, optlen) != optlen)
		return -1;

	uint32_t error;

	if(zfsfuse_ioctl_read_loop(hdl->libzfs_fd, &error, sizeof(uint32_t)) != 0)
		return -1;

	if(error == 0)
		return error;

	errno = error;
	return -1;
}
