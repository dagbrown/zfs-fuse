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
 * Copyright 2006 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */



#include <sys/types.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/pathname.h>
#include <sys/acl.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/mntent.h>
#include <sys/mount.h>
#include <sys/cmn_err.h>
#include "fs/fs_subr.h"
#include <sys/zfs_znode.h>
#include <sys/zil.h>
#include <sys/fs/zfs.h>
#include <sys/dmu.h>
#include <sys/dsl_prop.h>
#include <sys/spa.h>
#include <sys/zap.h>
#include <sys/varargs.h>
#include <sys/policy.h>
#include <sys/atomic.h>
#include <sys/mkdev.h>
#include <sys/modctl.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_ctldir.h>
#include <sys/bootconf.h>
#include <sys/sunddi.h>
#include <sys/dnlc.h>

int zfsfstype;
vfsops_t *zfs_vfsops = NULL;
/* ZFSFUSE: not needed */
/*static major_t zfs_major;
static minor_t zfs_minor;*/
static kmutex_t	zfs_dev_mtx;

/*extern char zfs_bootpath[BO_MAXOBJNAME];*/

static int zfs_mount(vfs_t *vfsp, vnode_t *mvp, struct mounta *uap, cred_t *cr);
static int zfs_umount(vfs_t *vfsp, int fflag, cred_t *cr);
static int zfs_mountroot(vfs_t *vfsp, enum whymountroot);
static int zfs_root(vfs_t *vfsp, vnode_t **vpp);
static int zfs_statvfs(vfs_t *vfsp, struct statvfs64 *statp);
static int zfs_vget(vfs_t *vfsp, vnode_t **vpp, fid_t *fidp);
static void zfs_freevfs(vfs_t *vfsp);
static void zfs_objset_close(zfsvfs_t *zfsvfs);

static const fs_operation_def_t zfs_vfsops_template[] = {
	VFSNAME_MOUNT, zfs_mount,
	VFSNAME_MOUNTROOT, zfs_mountroot,
	VFSNAME_UNMOUNT, zfs_umount,
	VFSNAME_ROOT, zfs_root,
	VFSNAME_STATVFS, zfs_statvfs,
	VFSNAME_SYNC, (fs_generic_func_p) zfs_sync,
	VFSNAME_VGET, zfs_vget,
	VFSNAME_FREEVFS, (fs_generic_func_p) zfs_freevfs,
	NULL, NULL
};

#if 0
static const fs_operation_def_t zfs_vfsops_eio_template[] = {
	VFSNAME_FREEVFS, (fs_generic_func_p) zfs_freevfs,
	NULL, NULL
};
#endif

/*
 * We need to keep a count of active fs's.
 * This is necessary to prevent our module
 * from being unloaded after a umount -f
 */
static uint32_t	zfs_active_fs_count = 0;

#if 0
static char *noatime_cancel[] = { MNTOPT_ATIME, NULL };
static char *atime_cancel[] = { MNTOPT_NOATIME, NULL };

static mntopt_t mntopts[] = {
	{ MNTOPT_XATTR, NULL, NULL, MO_NODISPLAY|MO_DEFAULT, NULL },
	{ MNTOPT_NOATIME, noatime_cancel, NULL, MO_DEFAULT, NULL },
	{ MNTOPT_ATIME, atime_cancel, NULL, 0, NULL }
};

static mntopts_t zfs_mntopts = {
	sizeof (mntopts) / sizeof (mntopt_t),
	mntopts
};
#endif

/*ARGSUSED*/
int
zfs_sync(vfs_t *vfsp, short flag, cred_t *cr)
{
	/* ZFSFUSE: not implemented */
	abort();
#if 0
	/*
	 * Data integrity is job one.  We don't want a compromised kernel
	 * writing to the storage pool, so we never sync during panic.
	 */
	if (panicstr)
		return (0);

	/*
	 * SYNC_ATTR is used by fsflush() to force old filesystems like UFS
	 * to sync metadata, which they would otherwise cache indefinitely.
	 * Semantically, the only requirement is that the sync be initiated.
	 * The DMU syncs out txgs frequently, so there's nothing to do.
	 */
	if (flag & SYNC_ATTR)
		return (0);

	if (vfsp != NULL) {
		/*
		 * Sync a specific filesystem.
		 */
		zfsvfs_t *zfsvfs = vfsp->vfs_data;

		ZFS_ENTER(zfsvfs);
		if (zfsvfs->z_log != NULL)
			zil_commit(zfsvfs->z_log, UINT64_MAX, 0);
		else
			txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
		ZFS_EXIT(zfsvfs);
	} else {
		/*
		 * Sync all ZFS filesystems.  This is what happens when you
		 * run sync(1M).  Unlike other filesystems, ZFS honors the
		 * request by waiting for all pools to commit all dirty data.
		 */
		spa_sync_allpools();
	}

	return (0);
#endif
}

#if 0
static int
zfs_create_unique_device(dev_t *dev)
{
	major_t new_major;

	do {
		ASSERT3U(zfs_minor, <=, MAXMIN32);
		minor_t start = zfs_minor;
		do {
			mutex_enter(&zfs_dev_mtx);
			if (zfs_minor >= MAXMIN32) {
				/*
				 * If we're still using the real major
				 * keep out of /dev/zfs and /dev/zvol minor
				 * number space.  If we're using a getudev()'ed
				 * major number, we can use all of its minors.
				 */
				if (zfs_major == ddi_name_to_major(ZFS_DRIVER))
					zfs_minor = ZFS_MIN_MINOR;
				else
					zfs_minor = 0;
			} else {
				zfs_minor++;
			}
			*dev = makedevice(zfs_major, zfs_minor);
			mutex_exit(&zfs_dev_mtx);
		} while (vfs_devismounted(*dev) && zfs_minor != start);
		if (zfs_minor == start) {
			/*
			 * We are using all ~262,000 minor numbers for the
			 * current major number.  Create a new major number.
			 */
			if ((new_major = getudev()) == (major_t)-1) {
				cmn_err(CE_WARN,
				    "zfs_mount: Can't get unique major "
				    "device number.");
				return (-1);
			}
			mutex_enter(&zfs_dev_mtx);
			zfs_major = new_major;
			zfs_minor = 0;

			mutex_exit(&zfs_dev_mtx);
		} else {
			break;
		}
		/* CONSTANTCONDITION */
	} while (1);

	return (0);
}
#endif
static void
atime_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == TRUE) {
		zfsvfs->z_atime = TRUE;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOATIME);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_ATIME, NULL, 0);
	} else {
		zfsvfs->z_atime = FALSE;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_ATIME);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOATIME, NULL, 0);
	}
}

#if 0
static void
blksz_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval < SPA_MINBLOCKSIZE ||
	    newval > SPA_MAXBLOCKSIZE || !ISP2(newval))
		newval = SPA_MAXBLOCKSIZE;

	zfsvfs->z_max_blksz = newval;
	zfsvfs->z_vfs->vfs_bsize = newval;
}
#endif
static void
readonly_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval) {
		/* XXX locking on vfs_flag? */
		zfsvfs->z_vfs->vfs_flag |= VFS_RDONLY;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_RW);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_RO, NULL, 0);
		(void) zfs_delete_thread_target(zfsvfs, 0);
	} else {
		/* XXX locking on vfs_flag? */
		zfsvfs->z_vfs->vfs_flag &= ~VFS_RDONLY;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_RO);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_RW, NULL, 0);
		(void) zfs_delete_thread_target(zfsvfs, 1);
	}
}

static void
devices_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == FALSE) {
		zfsvfs->z_vfs->vfs_flag |= VFS_NODEVICES;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_DEVICES);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NODEVICES, NULL, 0);
	} else {
		zfsvfs->z_vfs->vfs_flag &= ~VFS_NODEVICES;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NODEVICES);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_DEVICES, NULL, 0);
	}
}

static void
setuid_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == FALSE) {
		zfsvfs->z_vfs->vfs_flag |= VFS_NOSETUID;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_SETUID);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOSETUID, NULL, 0);
	} else {
		zfsvfs->z_vfs->vfs_flag &= ~VFS_NOSETUID;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOSETUID);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_SETUID, NULL, 0);
	}
}

static void
exec_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	if (newval == FALSE) {
		zfsvfs->z_vfs->vfs_flag |= VFS_NOEXEC;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_EXEC);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_NOEXEC, NULL, 0);
	} else {
		zfsvfs->z_vfs->vfs_flag &= ~VFS_NOEXEC;
		vfs_clearmntopt(zfsvfs->z_vfs, MNTOPT_NOEXEC);
		vfs_setmntopt(zfsvfs->z_vfs, MNTOPT_EXEC, NULL, 0);
	}
}

#if 0
static void
snapdir_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_show_ctldir = newval;
}

static void
acl_mode_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_acl_mode = newval;
}

static void
acl_inherit_changed_cb(void *arg, uint64_t newval)
{
	zfsvfs_t *zfsvfs = arg;

	zfsvfs->z_acl_inherit = newval;
}
#endif

static int
zfs_refresh_properties(vfs_t *vfsp)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;

	/*
	 * Remount operations default to "rw" unless "ro" is explicitly
	 * specified.
	 */
	if (vfs_optionisset(vfsp, MNTOPT_RO, NULL)) {
		readonly_changed_cb(zfsvfs, B_TRUE);
	} else {
		if (!dmu_objset_is_snapshot(zfsvfs->z_os))
			readonly_changed_cb(zfsvfs, B_FALSE);
		else if (vfs_optionisset(vfsp, MNTOPT_RW, NULL))
			    return (EROFS);
	}

	if (vfs_optionisset(vfsp, MNTOPT_NOSUID, NULL)) {
		devices_changed_cb(zfsvfs, B_FALSE);
		setuid_changed_cb(zfsvfs, B_FALSE);
	} else {
		if (vfs_optionisset(vfsp, MNTOPT_NODEVICES, NULL))
			devices_changed_cb(zfsvfs, B_FALSE);
		else if (vfs_optionisset(vfsp, MNTOPT_DEVICES, NULL))
			devices_changed_cb(zfsvfs, B_TRUE);

		if (vfs_optionisset(vfsp, MNTOPT_NOSETUID, NULL))
			setuid_changed_cb(zfsvfs, B_FALSE);
		else if (vfs_optionisset(vfsp, MNTOPT_SETUID, NULL))
			setuid_changed_cb(zfsvfs, B_TRUE);
	}

	if (vfs_optionisset(vfsp, MNTOPT_NOEXEC, NULL))
		exec_changed_cb(zfsvfs, B_FALSE);
	else if (vfs_optionisset(vfsp, MNTOPT_EXEC, NULL))
		exec_changed_cb(zfsvfs, B_TRUE);

	if (vfs_optionisset(vfsp, MNTOPT_ATIME, NULL))
		atime_changed_cb(zfsvfs, B_TRUE);
	else if (vfs_optionisset(vfsp, MNTOPT_NOATIME, NULL))
		atime_changed_cb(zfsvfs, B_FALSE);

	return (0);
}

#if 0
static int
zfs_register_callbacks(vfs_t *vfsp)
{
	struct dsl_dataset *ds = NULL;
	objset_t *os = NULL;
	zfsvfs_t *zfsvfs = NULL;
	int do_readonly = FALSE, readonly;
	int do_setuid = FALSE, setuid;
	int do_exec = FALSE, exec;
	int do_devices = FALSE, devices;
	int error = 0;

	ASSERT(vfsp);
	zfsvfs = vfsp->vfs_data;
	ASSERT(zfsvfs);
	os = zfsvfs->z_os;

	/*
	 * The act of registering our callbacks will destroy any mount
	 * options we may have.  In order to enable temporary overrides
	 * of mount options, we stash away the current values and restore
	 * restore them after we register the callbacks.
	 */
	if (vfs_optionisset(vfsp, MNTOPT_RO, NULL)) {
		readonly = B_TRUE;
		do_readonly = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_RW, NULL)) {
		readonly = B_FALSE;
		do_readonly = B_TRUE;
	}
	if (vfs_optionisset(vfsp, MNTOPT_NOSUID, NULL)) {
		devices = B_FALSE;
		setuid = B_FALSE;
		do_devices = B_TRUE;
		do_setuid = B_TRUE;
	} else {
		if (vfs_optionisset(vfsp, MNTOPT_NODEVICES, NULL)) {
			devices = B_FALSE;
			do_devices = B_TRUE;
		} else if (vfs_optionisset(vfsp,
			    MNTOPT_DEVICES, NULL)) {
			devices = B_TRUE;
			do_devices = B_TRUE;
		}

		if (vfs_optionisset(vfsp, MNTOPT_NOSETUID, NULL)) {
			setuid = B_FALSE;
			do_setuid = B_TRUE;
		} else if (vfs_optionisset(vfsp, MNTOPT_SETUID, NULL)) {
			setuid = B_TRUE;
			do_setuid = B_TRUE;
		}
	}
	if (vfs_optionisset(vfsp, MNTOPT_NOEXEC, NULL)) {
		exec = B_FALSE;
		do_exec = B_TRUE;
	} else if (vfs_optionisset(vfsp, MNTOPT_EXEC, NULL)) {
		exec = B_TRUE;
		do_exec = B_TRUE;
	}

	/*
	 * Register property callbacks.
	 *
	 * It would probably be fine to just check for i/o error from
	 * the first prop_register(), but I guess I like to go
	 * overboard...
	 */
	ds = dmu_objset_ds(os);
	error = dsl_prop_register(ds, "atime", atime_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "recordsize", blksz_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "readonly", readonly_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "devices", devices_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "setuid", setuid_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "exec", exec_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "snapdir", snapdir_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "aclmode", acl_mode_changed_cb, zfsvfs);
	error = error ? error : dsl_prop_register(ds,
	    "aclinherit", acl_inherit_changed_cb, zfsvfs);
	if (error)
		goto unregister;

	/*
	 * Invoke our callbacks to restore temporary mount options.
	 */
	if (do_readonly)
		readonly_changed_cb(zfsvfs, readonly);
	if (do_setuid)
		setuid_changed_cb(zfsvfs, setuid);
	if (do_exec)
		exec_changed_cb(zfsvfs, exec);
	if (do_devices)
		devices_changed_cb(zfsvfs, devices);

	return (0);

unregister:
	/*
	 * We may attempt to unregister some callbacks that are not
	 * registered, but this is OK; it will simply return ENOMSG,
	 * which we will ignore.
	 */
	(void) dsl_prop_unregister(ds, "atime", atime_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "recordsize", blksz_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "readonly", readonly_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "devices", devices_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "setuid", setuid_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "exec", exec_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "snapdir", snapdir_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "aclmode", acl_mode_changed_cb, zfsvfs);
	(void) dsl_prop_unregister(ds, "aclinherit", acl_inherit_changed_cb,
	    zfsvfs);
	return (error);

}
#endif

static int
zfs_domount(vfs_t *vfsp, char *osname, cred_t *cr)
{
	dev_t mount_dev;
	uint64_t recordsize, readonly;
	int error = 0;
	int mode;
	zfsvfs_t *zfsvfs;
	znode_t *zp = NULL;

	ASSERT(vfsp);
	ASSERT(osname);

	/*
	 * Initialize the zfs-specific filesystem structure.
	 * Should probably make this a kmem cache, shuffle fields,
	 * and just bzero up to z_hold_mtx[].
	 */
	zfsvfs = kmem_zalloc(sizeof (zfsvfs_t), KM_SLEEP);
	zfsvfs->z_vfs = vfsp;
	zfsvfs->z_parent = zfsvfs;
	zfsvfs->z_assign = TXG_NOWAIT;
	zfsvfs->z_max_blksz = SPA_MAXBLOCKSIZE;
	zfsvfs->z_show_ctldir = ZFS_SNAPDIR_VISIBLE;

	mutex_init(&zfsvfs->z_znodes_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&zfsvfs->z_all_znodes, sizeof (znode_t),
	    offsetof(znode_t, z_link_node));
	rw_init(&zfsvfs->z_um_lock, NULL, RW_DEFAULT, NULL);

	/* Initialize the generic filesystem structure. */
	vfsp->vfs_bcount = 0;
	vfsp->vfs_data = NULL;

	/* ZFSFUSE: not needed */
	/*if (zfs_create_unique_device(&mount_dev) == -1) {
		error = ENODEV;
		goto out;
	}*/

	ASSERT(vfs_devismounted(mount_dev) == 0);

	if (error = dsl_prop_get_integer(osname, "recordsize", &recordsize,
	    NULL))
		goto out;

	vfsp->vfs_dev = mount_dev;
	vfsp->vfs_fstype = zfsfstype;
	vfsp->vfs_bsize = recordsize;
	vfsp->vfs_flag |= VFS_NOTRUNC;
	vfsp->vfs_data = zfsvfs;

	if (error = dsl_prop_get_integer(osname, "readonly", &readonly, NULL))
		goto out;

	if (readonly)
		mode = DS_MODE_PRIMARY | DS_MODE_READONLY;
	else
		mode = DS_MODE_PRIMARY;

	error = dmu_objset_open(osname, DMU_OST_ZFS, mode, &zfsvfs->z_os);
	if (error == EROFS) {
		mode = DS_MODE_PRIMARY | DS_MODE_READONLY;
		error = dmu_objset_open(osname, DMU_OST_ZFS, mode,
		    &zfsvfs->z_os);
	}

	if (error)
		goto out;

	if (error = zfs_init_fs(zfsvfs, &zp, cr))
		goto out;

	/* The call to zfs_init_fs leaves the vnode held, release it here. */
	VN_RELE(ZTOV(zp));

	if (dmu_objset_is_snapshot(zfsvfs->z_os)) {
		ASSERT(mode & DS_MODE_READONLY);
		atime_changed_cb(zfsvfs, B_FALSE);
		readonly_changed_cb(zfsvfs, B_TRUE);
		zfsvfs->z_issnap = B_TRUE;
	} else {
		/* ZFSFUSE: not necessary */
		/*error = zfs_register_callbacks(vfsp);
		if (error)
			goto out;*/

		/*
		 * Start a delete thread running.
		 */
		(void) zfs_delete_thread_target(zfsvfs, 1);

		/*
		 * Parse and replay the intent log.
		 */
		zil_replay(zfsvfs->z_os, zfsvfs, &zfsvfs->z_assign,
		    zfs_replay_vector, (void (*)(void *))zfs_delete_wait_empty); 

		if (!zil_disable)
			zfsvfs->z_log = zil_open(zfsvfs->z_os, zfs_get_data);
	}

	/* ZFSFUSE: FIXME
	if (!zfsvfs->z_issnap)
		zfsctl_create(zfsvfs);*/
out:
	if (error) {
		if (zfsvfs->z_os)
			dmu_objset_close(zfsvfs->z_os);
		kmem_free(zfsvfs, sizeof (zfsvfs_t));
	} else {
		atomic_add_32(&zfs_active_fs_count, 1);
	}

	return (error);

}

#if 0
void
zfs_unregister_callbacks(zfsvfs_t *zfsvfs)
{
	objset_t *os = zfsvfs->z_os;
	struct dsl_dataset *ds;

	/*
	 * Unregister properties.
	 */
	if (!dmu_objset_is_snapshot(os)) {
		ds = dmu_objset_ds(os);
		VERIFY(dsl_prop_unregister(ds, "atime", atime_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "recordsize", blksz_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "readonly", readonly_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "devices", devices_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "setuid", setuid_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "exec", exec_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "snapdir", snapdir_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "aclmode", acl_mode_changed_cb,
		    zfsvfs) == 0);

		VERIFY(dsl_prop_unregister(ds, "aclinherit",
		    acl_inherit_changed_cb, zfsvfs) == 0);
	}
}
#endif

static int
zfs_mountroot(vfs_t *vfsp, enum whymountroot why)
{
	/* ZFSFUSE: not used */
	abort();
#if 0
	int error = 0;
	int ret = 0;
	static int zfsrootdone = 0;
	zfsvfs_t *zfsvfs = NULL;
	znode_t *zp = NULL;
	vnode_t *vp = NULL;

	ASSERT(vfsp);

	/*
	 * The filesystem that we mount as root is defined in
	 * /etc/system using the zfsroot variable.  The value defined
	 * there is copied early in startup code to zfs_bootpath
	 * (defined in modsysfile.c).
	 */
	if (why == ROOT_INIT) {
		if (zfsrootdone++)
			return (EBUSY);

		/*
		 * This needs to be done here, so that when we return from
		 * mountroot, the vfs resource name will be set correctly.
		 */
		if (snprintf(rootfs.bo_name, BO_MAXOBJNAME, "%s", zfs_bootpath)
		    >= BO_MAXOBJNAME)
			return (ENAMETOOLONG);

		if (error = vfs_lock(vfsp))
			return (error);

		if (error = zfs_domount(vfsp, zfs_bootpath, CRED()))
			goto out;

		zfsvfs = (zfsvfs_t *)vfsp->vfs_data;
		ASSERT(zfsvfs);
		if (error = zfs_zget(zfsvfs, zfsvfs->z_root, &zp))
			goto out;

		vp = ZTOV(zp);
		mutex_enter(&vp->v_lock);
		vp->v_flag |= VROOT;
		mutex_exit(&vp->v_lock);
		rootvp = vp;

		/*
		 * The zfs_zget call above returns with a hold on vp, we release
		 * it here.
		 */
		VN_RELE(vp);

		/*
		 * Mount root as readonly initially, it will be remouted
		 * read/write by /lib/svc/method/fs-usr.
		 */
		readonly_changed_cb(vfsp->vfs_data, B_TRUE);
		vfs_add((struct vnode *)0, vfsp,
		    (vfsp->vfs_flag & VFS_RDONLY) ? MS_RDONLY : 0);
out:
		vfs_unlock(vfsp);
		ret = (error) ? error : 0;
		return (ret);

	} else if (why == ROOT_REMOUNT) {

		readonly_changed_cb(vfsp->vfs_data, B_FALSE);
		vfsp->vfs_flag |= VFS_REMOUNT;
		return (zfs_refresh_properties(vfsp));

	} else if (why == ROOT_UNMOUNT) {
		zfs_unregister_callbacks((zfsvfs_t *)vfsp->vfs_data);
		(void) zfs_sync(vfsp, 0, 0);
		return (0);
	}

	/*
	 * if "why" is equal to anything else other than ROOT_INIT,
	 * ROOT_REMOUNT, or ROOT_UNMOUNT, we do not support it.
	 */
	return (ENOTSUP);
#endif
}

/*ARGSUSED*/
int
zfs_mount(vfs_t *vfsp, vnode_t *mvp, struct mounta *uap, cred_t *cr)
{
	char		*osname;
	pathname_t	spn;
	int		error = 0;
	uio_seg_t	fromspace = (uap->flags & MS_SYSSPACE) ?
				UIO_SYSSPACE : UIO_USERSPACE;
	int		canwrite;

	if (mvp->v_type != VDIR)
		return (ENOTDIR);

	mutex_enter(&mvp->v_lock);
	if ((uap->flags & MS_REMOUNT) == 0 &&
	    (uap->flags & MS_OVERLAY) == 0 &&
	    (mvp->v_count != 1 || (mvp->v_flag & VROOT))) {
		mutex_exit(&mvp->v_lock);
		return (EBUSY);
	}
	mutex_exit(&mvp->v_lock);

	/*
	 * ZFS does not support passing unparsed data in via MS_DATA.
	 * Users should use the MS_OPTIONSTR interface; this means
	 * that all option parsing is already done and the options struct
	 * can be interrogated.
	 */
	if ((uap->flags & MS_DATA) && uap->datalen > 0)
		return (EINVAL);

	/*
	 * When doing a remount, we simply refresh our temporary properties
	 * according to those options set in the current VFS options.
	 */
	if (uap->flags & MS_REMOUNT) {
		return (zfs_refresh_properties(vfsp));
	}

	/*
	 * Get the objset name (the "special" mount argument).
	 */
	if (error = pn_get(uap->spec, fromspace, &spn))
		return (error);

	osname = spn.pn_path;

	if ((error = secpolicy_fs_mount(cr, mvp, vfsp)) != 0)
		goto out;

	/*
	 * Refuse to mount a filesystem if we are in a local zone and the
	 * dataset is not visible.
	 */
	if (!INGLOBALZONE(curproc) &&
	    (!zone_dataset_visible(osname, &canwrite) || !canwrite)) {
		error = EPERM;
		goto out;
	}

	error = zfs_domount(vfsp, osname, cr);

out:
	pn_free(&spn);
	return (error);
}

int
zfs_statvfs(vfs_t *vfsp, struct statvfs64 *statp)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;
	/* dev32_t d32; */
	uint64_t refdbytes, availbytes, usedobjs, availobjs;

	ZFS_ENTER(zfsvfs);

	dmu_objset_space(zfsvfs->z_os,
	    &refdbytes, &availbytes, &usedobjs, &availobjs);

	/*
	 * The underlying storage pool actually uses multiple block sizes.
	 * We report the fragsize as the smallest block size we support,
	 * and we report our blocksize as the filesystem's maximum blocksize.
	 */
	statp->f_frsize = 1UL << SPA_MINBLOCKSHIFT;
	statp->f_bsize = zfsvfs->z_max_blksz;

	/*
	 * The following report "total" blocks of various kinds in the
	 * file system, but reported in terms of f_frsize - the
	 * "fragment" size.
	 */

	statp->f_blocks = (refdbytes + availbytes) >> SPA_MINBLOCKSHIFT;
	statp->f_bfree = availbytes >> SPA_MINBLOCKSHIFT;
	statp->f_bavail = statp->f_bfree; /* no root reservation */

	/*
	 * statvfs() should really be called statufs(), because it assumes
	 * static metadata.  ZFS doesn't preallocate files, so the best
	 * we can do is report the max that could possibly fit in f_files,
	 * and that minus the number actually used in f_ffree.
	 * For f_ffree, report the smaller of the number of object available
	 * and the number of blocks (each object will take at least a block).
	 */
	statp->f_ffree = MIN(availobjs, statp->f_bfree);
	statp->f_favail = statp->f_ffree;	/* no "root reservation" */
	statp->f_files = statp->f_ffree + usedobjs;

	/* ZFSFUSE: not necessary? see 'man statfs' */
	/*(void) cmpldev(&d32, vfsp->vfs_dev);
	statp->f_fsid = d32;*/

	/*
	 * We're a zfs filesystem.
	 */
	/* ZFSFUSE: not necessary */
	/*(void) strcpy(statp->f_basetype, vfssw[vfsp->vfs_fstype].vsw_name);

	statp->f_flag = vf_to_stf(vfsp->vfs_flag);*/

	statp->f_namemax = ZFS_MAXNAMELEN;

	/*
	 * We have all of 32 characters to stuff a string here.
	 * Is there anything useful we could/should provide?
	 */
	bzero(statp->f_fstr, sizeof (statp->f_fstr));

	ZFS_EXIT(zfsvfs);
	return (0);
}

static int
zfs_root(vfs_t *vfsp, vnode_t **vpp)
{
	/* ZFSFUSE: not used */
	abort();
#if 0
	zfsvfs_t *zfsvfs = vfsp->vfs_data;
	znode_t *rootzp;
	int error;

	ZFS_ENTER(zfsvfs);

	error = zfs_zget(zfsvfs, zfsvfs->z_root, &rootzp);
	if (error == 0)
		*vpp = ZTOV(rootzp);

	ZFS_EXIT(zfsvfs);
	return (error);
#endif
}

/*ARGSUSED*/
int
zfs_umount(vfs_t *vfsp, int fflag, cred_t *cr)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;
	int ret;

	if ((ret = secpolicy_fs_unmount(cr, vfsp)) != 0)
		return (ret);


	(void) dnlc_purge_vfsp(vfsp, 0);

	/*
	 * Unmount any snapshots mounted under .zfs before unmounting the
	 * dataset itself.
	 */
	/* ZFSFUSE: FIXME */
	/*if (zfsvfs->z_ctldir != NULL &&
	    (ret = zfsctl_umount_snapshots(vfsp, fflag, cr)) != 0)
		return (ret);*/

	if (fflag & MS_FORCE) {
		vfsp->vfs_flag |= VFS_UNMOUNTED;
		zfsvfs->z_unmounted1 = B_TRUE;

		/*
		 * Wait for all zfs threads to leave zfs.
		 * Grabbing a rwlock as reader in all vops and
		 * as writer here doesn't work because it too easy to get
		 * multiple reader enters as zfs can re-enter itself.
		 * This can lead to deadlock if there is an intervening
		 * rw_enter as writer.
		 * So a file system threads ref count (z_op_cnt) is used.
		 * A polling loop on z_op_cnt may seem inefficient, but
		 * - this saves all threads on exit from having to grab a
		 *   mutex in order to cv_signal
		 * - only occurs on forced unmount in the rare case when
		 *   there are outstanding threads within the file system.
		 */
		while (zfsvfs->z_op_cnt) {
			delay(1);
		}

		zfs_objset_close(zfsvfs);

		return (0);
	}
	/*
	 * Stop all delete threads.
	 */
	(void) zfs_delete_thread_target(zfsvfs, 0);

	/*
	 * Check the number of active vnodes in the file system.
	 * Our count is maintained in the vfs structure, but the number
	 * is off by 1 to indicate a hold on the vfs structure itself.
	 *
	 * The '.zfs' directory maintains a reference of its own, and any active
	 * references underneath are reflected in the vnode count.
	 */
	if (zfsvfs->z_ctldir == NULL) {
		if (vfsp->vfs_count > 1) {
			if ((zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) == 0)
				(void) zfs_delete_thread_target(zfsvfs, 1);
			return (EBUSY);
		}
	} else {
		if (vfsp->vfs_count > 2 ||
		    (zfsvfs->z_ctldir->v_count > 1 && !(fflag & MS_FORCE))) {
			if ((zfsvfs->z_vfs->vfs_flag & VFS_RDONLY) == 0)
				(void) zfs_delete_thread_target(zfsvfs, 1);
			return (EBUSY);
		}
	}

	vfsp->vfs_flag |= VFS_UNMOUNTED;
	zfs_objset_close(zfsvfs);

	return (0);
}

static int
zfs_vget(vfs_t *vfsp, vnode_t **vpp, fid_t *fidp)
{
	/* ZFSFUSE: not used */
	abort();
#if 0
	zfsvfs_t	*zfsvfs = vfsp->vfs_data;
	znode_t		*zp;
	uint64_t	object = 0;
	uint64_t	fid_gen = 0;
	uint64_t	gen_mask;
	uint64_t	zp_gen;
	int 		i, err;

	*vpp = NULL;

	ZFS_ENTER(zfsvfs);

	if (fidp->fid_len == LONG_FID_LEN) {
		zfid_long_t	*zlfid = (zfid_long_t *)fidp;
		uint64_t	objsetid = 0;
		uint64_t	setgen = 0;

		for (i = 0; i < sizeof (zlfid->zf_setid); i++)
			objsetid |= ((uint64_t)zlfid->zf_setid[i]) << (8 * i);

		for (i = 0; i < sizeof (zlfid->zf_setgen); i++)
			setgen |= ((uint64_t)zlfid->zf_setgen[i]) << (8 * i);

		ZFS_EXIT(zfsvfs);

		err = zfsctl_lookup_objset(vfsp, objsetid, &zfsvfs);
		if (err)
			return (EINVAL);
		ZFS_ENTER(zfsvfs);
	}

	if (fidp->fid_len == SHORT_FID_LEN || fidp->fid_len == LONG_FID_LEN) {
		zfid_short_t	*zfid = (zfid_short_t *)fidp;

		for (i = 0; i < sizeof (zfid->zf_object); i++)
			object |= ((uint64_t)zfid->zf_object[i]) << (8 * i);

		for (i = 0; i < sizeof (zfid->zf_gen); i++)
			fid_gen |= ((uint64_t)zfid->zf_gen[i]) << (8 * i);
	} else {
		ZFS_EXIT(zfsvfs);
		return (EINVAL);
	}

	/* A zero fid_gen means we are in the .zfs control directories */
	if (fid_gen == 0 &&
	    (object == ZFSCTL_INO_ROOT || object == ZFSCTL_INO_SNAPDIR)) {
		*vpp = zfsvfs->z_ctldir;
		ASSERT(*vpp != NULL);
		if (object == ZFSCTL_INO_SNAPDIR) {
			VERIFY(zfsctl_root_lookup(*vpp, "snapshot", vpp, NULL,
			    0, NULL, NULL) == 0);
		} else {
			VN_HOLD(*vpp);
		}
		ZFS_EXIT(zfsvfs);
		return (0);
	}

	gen_mask = -1ULL >> (64 - 8 * i);

	dprintf("getting %llu [%u mask %llx]\n", object, fid_gen, gen_mask);
	if (err = zfs_zget(zfsvfs, object, &zp)) {
		ZFS_EXIT(zfsvfs);
		return (err);
	}
	zp_gen = zp->z_phys->zp_gen & gen_mask;
	if (zp_gen == 0)
		zp_gen = 1;
	if (zp->z_reap || zp_gen != fid_gen) {
		dprintf("znode gen (%u) != fid gen (%u)\n", zp_gen, fid_gen);
		VN_RELE(ZTOV(zp));
		ZFS_EXIT(zfsvfs);
		return (EINVAL);
	}

	*vpp = ZTOV(zp);
	ZFS_EXIT(zfsvfs);
	return (0);
#endif
}

static void
zfs_objset_close(zfsvfs_t *zfsvfs)
{
	zfs_delete_t	*zd = &zfsvfs->z_delete_head;
	znode_t		*zp, *nextzp;
	objset_t	*os = zfsvfs->z_os;

	/*
	 * Stop all delete threads.
	 */
	(void) zfs_delete_thread_target(zfsvfs, 0);

	/*
	 * For forced unmount, at this point all vops except zfs_inactive
	 * are erroring EIO. We need to now suspend zfs_inactive threads
	 * while we are freeing dbufs before switching zfs_inactive
	 * to use behaviour without a objset.
	 */
	rw_enter(&zfsvfs->z_um_lock, RW_WRITER);

	/*
	 * Release all delete in progress znodes
	 * They will be processed when the file system remounts.
	 */
	mutex_enter(&zd->z_mutex);
	while (zp = list_head(&zd->z_znodes)) {
		list_remove(&zd->z_znodes, zp);
		zp->z_dbuf_held = 0;
		dmu_buf_rele(zp->z_dbuf, NULL);
	}
	mutex_exit(&zd->z_mutex);

	/*
	 * Release all holds on dbufs
	 * Note, although we have stopped all other vop threads and
	 * zfs_inactive(), the dmu can callback via znode_pageout_func()
	 * which can zfs_znode_free() the znode.
	 * So we lock z_all_znodes; search the list for a held
	 * dbuf; drop the lock (we know zp can't disappear if we hold
	 * a dbuf lock; then regrab the lock and restart.
	 */
	mutex_enter(&zfsvfs->z_znodes_lock);
	for (zp = list_head(&zfsvfs->z_all_znodes); zp; zp = nextzp) {
		nextzp = list_next(&zfsvfs->z_all_znodes, zp);
		if (zp->z_dbuf_held) {
			/* dbufs should only be held when force unmounting */
			zp->z_dbuf_held = 0;
			mutex_exit(&zfsvfs->z_znodes_lock);
			dmu_buf_rele(zp->z_dbuf, NULL);
			/* Start again */
			mutex_enter(&zfsvfs->z_znodes_lock);
			nextzp = list_head(&zfsvfs->z_all_znodes);
		}
	}
	mutex_exit(&zfsvfs->z_znodes_lock);

	/*
	 * Unregister properties.
	 */
	/* ZFSFUSE: unnecessary */
	/* if (!dmu_objset_is_snapshot(os))
		zfs_unregister_callbacks(zfsvfs);*/

	/*
	 * Switch zfs_inactive to behaviour without an objset.
	 * It just tosses cached pages and frees the znode & vnode.
	 * Then re-enable zfs_inactive threads in that new behaviour.
	 */
	zfsvfs->z_unmounted2 = B_TRUE;
	rw_exit(&zfsvfs->z_um_lock); /* re-enable any zfs_inactive threads */

	/*
	 * Close the zil. Can't close the zil while zfs_inactive
	 * threads are blocked as zil_close can call zfs_inactive.
	 */
	if (zfsvfs->z_log) {
		zil_close(zfsvfs->z_log);
		zfsvfs->z_log = NULL;
	}

	/*
	 * Evict all dbufs so that cached znodes will be freed
	 */
	if (dmu_objset_evict_dbufs(os, 1)) {
		txg_wait_synced(dmu_objset_pool(zfsvfs->z_os), 0);
		(void) dmu_objset_evict_dbufs(os, 0);
	}

	/*
	 * Finally close the objset
	 */
	dmu_objset_close(os);

	/*
	 * We can now safely destroy the '.zfs' directory node.
	 */
	/* ZFSFUSE: FIXME
	if (zfsvfs->z_ctldir != NULL)
		zfsctl_destroy(zfsvfs); */

}

void
zfs_freevfs(vfs_t *vfsp)
{
	zfsvfs_t *zfsvfs = vfsp->vfs_data;

	kmem_free(zfsvfs, sizeof (zfsvfs_t));

	atomic_add_32(&zfs_active_fs_count, -1);
}

/*
 * VFS_INIT() initialization.  Note that there is no VFS_FINI(),
 * so we can't safely do any non-idempotent initialization here.
 * Leave that to zfs_init() and zfs_fini(), which are called
 * from the module's _init() and _fini() entry points.
 */
/*ARGSUSED*/
int
zfs_vfsinit(int fstype, char *name)
{
	int error;

	zfsfstype = fstype;

	/*
	 * Setup vfsops and vnodeops tables.
	 */
	error = vfs_setfsops(fstype, zfs_vfsops_template, &zfs_vfsops);
	if (error != 0) {
		cmn_err(CE_WARN, "zfs: bad vfs ops template");
	}

	error = zfs_create_op_tables();
	if (error) {
		zfs_remove_op_tables();
		cmn_err(CE_WARN, "zfs: bad vnode ops template");
		(void) vfs_freevfsops_by_type(zfsfstype);
		return (error);
	}

	mutex_init(&zfs_dev_mtx, NULL, MUTEX_DEFAULT, NULL);

	/*
	 * Unique major number for all zfs mounts.
	 * If we run out of 32-bit minors, we'll getudev() another major.
	 */
	/* ZFSFUSE: not needed */
	/*zfs_major = ddi_name_to_major(ZFS_DRIVER);
	zfs_minor = ZFS_MIN_MINOR;*/

	return (0);
}

void
zfs_init(void)
{
	/*
	 * Initialize .zfs directory structures
	 */
	/* ZFSFUSE: TODO */
	/* zfsctl_init(); */

	/*
	 * Initialize znode cache, vnode ops, etc...
	 */
	zfs_znode_init();
}

void
zfs_fini(void)
{
	/* ZFSFUSE: TODO */
	/* zfsctl_fini(); */
	zfs_znode_fini();
}

int
zfs_busy(void)
{
	return (zfs_active_fs_count != 0);
}

#if 0
static vfsdef_t vfw = {
	VFSDEF_VERSION,
	MNTTYPE_ZFS,
	zfs_vfsinit,
	VSW_HASPROTO|VSW_CANRWRO|VSW_CANREMOUNT|VSW_VOLATILEDEV|VSW_STATS,
	&zfs_mntopts
};

struct modlfs zfs_modlfs = {
	&mod_fsops, "ZFS filesystem version " ZFS_VERSION_STRING, &vfw
};
#endif
