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
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */



#include <sys/types.h>
#include <sys/param.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/buf.h>
#include <sys/modctl.h>
#include <sys/open.h>
#include <sys/file.h>
#include <sys/kmem.h>
#include <sys/conf.h>
#include <sys/cmn_err.h>
#include <sys/stat.h>
#include <sys/zfs_ioctl.h>
#include <sys/zfs_i18n.h>
#include <sys/zfs_znode.h>
#include <sys/zap.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev.h>
#include <sys/vdev_impl.h>
#include <sys/dmu.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_deleg.h>
#include <sys/dmu_objset.h>
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>
#include <sys/policy.h>
#include <sys/zone.h>
#include <sys/nvpair.h>
#include <sys/pathname.h>
#include <sys/mount.h>
#include <sys/sdt.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_ctldir.h>
#include <sys/zfs_dir.h>
#include <sys/zvol.h>
#include <sharefs/share.h>
#include <sys/zfs_znode.h>
#include <sys/zfs_vfsops.h>
#include <sys/dmu_objset.h>

#include "zfs_namecheck.h"
#include "zfs_prop.h"
#include "zfs_deleg.h"

extern struct modlfs zfs_modlfs;

extern void zfs_init(void);
extern void zfs_fini(void);

ldi_ident_t zfs_li = NULL;
dev_info_t *zfs_dip;

typedef int zfs_ioc_func_t(zfs_cmd_t *);
typedef int zfs_secpolicy_func_t(zfs_cmd_t *, cred_t *);

typedef struct zfs_ioc_vec {
	zfs_ioc_func_t		*zvec_func;
	zfs_secpolicy_func_t	*zvec_secpolicy;
	enum {
		NO_NAME,
		POOL_NAME,
		DATASET_NAME
	} zvec_namecheck;
	boolean_t		zvec_his_log;
} zfs_ioc_vec_t;

/* _NOTE(PRINTFLIKE(4)) - this is printf-like, but lint is too whiney */
void
__dprintf(const char *file, const char *func, int line, const char *fmt, ...)
{
	const char *newfile;
	char buf[256];
	va_list adx;

	/*
	 * Get rid of annoying "../common/" prefix to filename.
	 */
	newfile = strrchr(file, '/');
	if (newfile != NULL) {
		newfile = newfile + 1; /* Get rid of leading / */
	} else {
		newfile = file;
	}

	va_start(adx, fmt);
	(void) vsnprintf(buf, sizeof (buf), fmt, adx);
	va_end(adx);

// 	fprintf(stderr, "%s", buf);

	/*
	 * To get this data, use the zfs-dprintf probe as so:
	 * dtrace -q -n 'zfs-dprintf \
	 *	/stringof(arg0) == "dbuf.c"/ \
	 *	{printf("%s: %s", stringof(arg1), stringof(arg3))}'
	 * arg0 = file name
	 * arg1 = function name
	 * arg2 = line number
	 * arg3 = message
	 */
	DTRACE_PROBE4(zfs__dprintf,
	    char *, newfile, char *, func, int, line, char *, buf);
}

static void
history_str_free(char *buf)
{
	kmem_free(buf, HIS_MAX_RECORD_LEN);
}

static char *
history_str_get(zfs_cmd_t *zc)
{
	char *buf;

	if (zc->zc_history == NULL)
		return (NULL);

	buf = kmem_alloc(HIS_MAX_RECORD_LEN, KM_SLEEP);
	if (copyinstr((void *)(uintptr_t)zc->zc_history,
	    buf, HIS_MAX_RECORD_LEN, NULL) != 0) {
		history_str_free(buf);
		return (NULL);
	}

	buf[HIS_MAX_RECORD_LEN -1] = '\0';

	return (buf);
}

/*
 * zfs_check_version
 *
 *	Return non-zero if the spa version is less than requested version.
 */
static int
zfs_check_version(const char *name, int version)
{

	spa_t *spa;

	if (spa_open(name, &spa, FTAG) == 0) {
		if (spa_version(spa) < version) {
			spa_close(spa, FTAG);
			return (1);
		}
		spa_close(spa, FTAG);
	}
	return (0);
}

static void
zfs_log_history(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *buf;

	if ((buf = history_str_get(zc)) == NULL)
		return;

	if (spa_open(zc->zc_name, &spa, FTAG) == 0) {
		if (spa_version(spa) >= SPA_VERSION_ZPOOL_HISTORY)
			(void) spa_history_log(spa, buf, LOG_CMD_NORMAL);
		spa_close(spa, FTAG);
	}
	history_str_free(buf);
}

/*
 * Policy for top-level read operations (list pools).  Requires no privileges,
 * and can be used in the local zone, as there is no associated dataset.
 */
/* ARGSUSED */
static int
zfs_secpolicy_none(zfs_cmd_t *zc, cred_t *cr)
{
	return (0);
}

/*
 * Policy for dataset read operations (list children, get statistics).  Requires
 * no privileges, but must be visible in the local zone.
 */
/* ARGSUSED */
static int
zfs_secpolicy_read(zfs_cmd_t *zc, cred_t *cr)
{
	if (INGLOBALZONE(curproc) ||
	    zone_dataset_visible(zc->zc_name, NULL))
		return (0);

	return (ENOENT);
}

static int
zfs_dozonecheck(const char *dataset, cred_t *cr)
{
	uint64_t zoned;
	int writable = 1;

	/*
	 * The dataset must be visible by this zone -- check this first
	 * so they don't see EPERM on something they shouldn't know about.
	 */
	if (!INGLOBALZONE(curproc) &&
	    !zone_dataset_visible(dataset, &writable))
		return (ENOENT);

	if (dsl_prop_get_integer(dataset, "zoned", &zoned, NULL))
		return (ENOENT);

	if (INGLOBALZONE(curproc)) {
		/*
		 * If the fs is zoned, only root can access it from the
		 * global zone.
		 */
		if (secpolicy_zfs(cr) && zoned)
			return (EPERM);
	} else {
		/*
		 * If we are in a local zone, the 'zoned' property must be set.
		 */
		if (!zoned)
			return (EPERM);

		/* must be writable by this zone */
		if (!writable)
			return (EPERM);
	}
	return (0);
}

int
zfs_secpolicy_write_perms(const char *name, const char *perm, cred_t *cr)
{
	int error;

	error = zfs_dozonecheck(name, cr);
	if (error == 0) {
		error = secpolicy_zfs(cr);
		if (error)
			error = dsl_deleg_access(name, perm, cr);
	}
	return (error);
}

static int
zfs_secpolicy_setprop(const char *name, zfs_prop_t prop, cred_t *cr)
{
	/*
	 * Check permissions for special properties.
	 */
	switch (prop) {
	case ZFS_PROP_ZONED:
		/*
		 * Disallow setting of 'zoned' from within a local zone.
		 */
		if (!INGLOBALZONE(curproc))
			return (EPERM);
		break;

	case ZFS_PROP_QUOTA:
		if (!INGLOBALZONE(curproc)) {
			uint64_t zoned;
			char setpoint[MAXNAMELEN];
			/*
			 * Unprivileged users are allowed to modify the
			 * quota on things *under* (ie. contained by)
			 * the thing they own.
			 */
			if (dsl_prop_get_integer(name, "zoned", &zoned,
			    setpoint))
				return (EPERM);
			if (!zoned || strlen(name) <= strlen(setpoint))
				return (EPERM);
		}
		break;
	}

	return (zfs_secpolicy_write_perms(name, zfs_prop_to_name(prop), cr));
}

int
zfs_secpolicy_fsacl(zfs_cmd_t *zc, cred_t *cr)
{
	int error;

	error = zfs_dozonecheck(zc->zc_name, cr);
	if (error)
		return (error);

	/*
	 * permission to set permissions will be evaluated later in
	 * dsl_deleg_can_allow()
	 */
	return (0);
}

int
zfs_secpolicy_rollback(zfs_cmd_t *zc, cred_t *cr)
{
	int error;
	error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_ROLLBACK, cr);
	if (error == 0)
		error = zfs_secpolicy_write_perms(zc->zc_name,
		    ZFS_DELEG_PERM_MOUNT, cr);
	return (error);
}

int
zfs_secpolicy_send(zfs_cmd_t *zc, cred_t *cr)
{
	return (zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_SEND, cr));
}

int
zfs_secpolicy_share(zfs_cmd_t *zc, cred_t *cr)
{
	if (!INGLOBALZONE(curproc))
		return (EPERM);

	if (secpolicy_nfs(cr) == 0) {
		return (0);
	} else {
		vnode_t *vp;
		int error;

		if ((error = lookupname(zc->zc_value, UIO_SYSSPACE,
		    NO_FOLLOW, NULL, &vp)) != 0)
			return (error);

		/* Now make sure mntpnt and dataset are ZFS */

		if (vp->v_vfsp->vfs_fstype != zfsfstype ||
		    (strcmp((char *)refstr_value(vp->v_vfsp->vfs_resource),
		    zc->zc_name) != 0)) {
			VN_RELE(vp);
			return (EPERM);
		}

		VN_RELE(vp);
		return (dsl_deleg_access(zc->zc_name,
		    ZFS_DELEG_PERM_SHARE, cr));
	}
}

static int
zfs_get_parent(const char *datasetname, char *parent, int parentsize)
{
	char *cp;

	/*
	 * Remove the @bla or /bla from the end of the name to get the parent.
	 */
	(void) strncpy(parent, datasetname, parentsize);
	cp = strrchr(parent, '@');
	if (cp != NULL) {
		cp[0] = '\0';
	} else {
		cp = strrchr(parent, '/');
		if (cp == NULL)
			return (ENOENT);
		cp[0] = '\0';
	}

	return (0);
}

int
zfs_secpolicy_destroy_perms(const char *name, cred_t *cr)
{
	int error;

	if ((error = zfs_secpolicy_write_perms(name,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	return (zfs_secpolicy_write_perms(name, ZFS_DELEG_PERM_DESTROY, cr));
}

static int
zfs_secpolicy_destroy(zfs_cmd_t *zc, cred_t *cr)
{
	return (zfs_secpolicy_destroy_perms(zc->zc_name, cr));
}

/*
 * Must have sys_config privilege to check the iscsi permission
 */
/* ARGSUSED */
static int
zfs_secpolicy_iscsi(zfs_cmd_t *zc, cred_t *cr)
{
	return (secpolicy_zfs(cr));
}

int
zfs_secpolicy_rename_perms(const char *from, const char *to, cred_t *cr)
{
	char 	parentname[MAXNAMELEN];
	int	error;

	if ((error = zfs_secpolicy_write_perms(from,
	    ZFS_DELEG_PERM_RENAME, cr)) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(from,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	if ((error = zfs_get_parent(to, parentname,
	    sizeof (parentname))) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_CREATE, cr)) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	return (error);
}

static int
zfs_secpolicy_rename(zfs_cmd_t *zc, cred_t *cr)
{
	return (zfs_secpolicy_rename_perms(zc->zc_name, zc->zc_value, cr));
}

static int
zfs_secpolicy_promote(zfs_cmd_t *zc, cred_t *cr)
{
	char 	parentname[MAXNAMELEN];
	objset_t *clone;
	int error;

	error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_PROMOTE, cr);
	if (error)
		return (error);

	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &clone);

	if (error == 0) {
		dsl_dataset_t *pclone = NULL;
		dsl_dir_t *dd;
		dd = clone->os->os_dsl_dataset->ds_dir;

		rw_enter(&dd->dd_pool->dp_config_rwlock, RW_READER);
		error = dsl_dataset_open_obj(dd->dd_pool,
		    dd->dd_phys->dd_origin_obj, NULL,
		    DS_MODE_NONE, FTAG, &pclone);
		rw_exit(&dd->dd_pool->dp_config_rwlock);
		if (error) {
			dmu_objset_close(clone);
			return (error);
		}

		error = zfs_secpolicy_write_perms(zc->zc_name,
		    ZFS_DELEG_PERM_MOUNT, cr);

		dsl_dataset_name(pclone, parentname);
		dmu_objset_close(clone);
		dsl_dataset_close(pclone, DS_MODE_NONE, FTAG);
		if (error == 0)
			error = zfs_secpolicy_write_perms(parentname,
			    ZFS_DELEG_PERM_PROMOTE, cr);
	}
	return (error);
}

static int
zfs_secpolicy_receive(zfs_cmd_t *zc, cred_t *cr)
{
	int error;

	if ((error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_RECEIVE, cr)) != 0)
		return (error);

	if ((error = zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_MOUNT, cr)) != 0)
		return (error);

	return (zfs_secpolicy_write_perms(zc->zc_name,
	    ZFS_DELEG_PERM_CREATE, cr));
}

int
zfs_secpolicy_snapshot_perms(const char *name, cred_t *cr)
{
	int error;

	if ((error = zfs_secpolicy_write_perms(name,
	    ZFS_DELEG_PERM_SNAPSHOT, cr)) != 0)
		return (error);

	error = zfs_secpolicy_write_perms(name,
	    ZFS_DELEG_PERM_MOUNT, cr);

	return (error);
}

static int
zfs_secpolicy_snapshot(zfs_cmd_t *zc, cred_t *cr)
{

	return (zfs_secpolicy_snapshot_perms(zc->zc_name, cr));
}

static int
zfs_secpolicy_create(zfs_cmd_t *zc, cred_t *cr)
{
	char 	parentname[MAXNAMELEN];
	int 	error;

	if ((error = zfs_get_parent(zc->zc_name, parentname,
	    sizeof (parentname))) != 0)
		return (error);

	if (zc->zc_value[0] != '\0') {
		if ((error = zfs_secpolicy_write_perms(zc->zc_value,
		    ZFS_DELEG_PERM_CLONE, cr)) != 0)
			return (error);
	}

	if ((error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_CREATE, cr)) != 0)
		return (error);

	error = zfs_secpolicy_write_perms(parentname,
	    ZFS_DELEG_PERM_MOUNT, cr);

	return (error);
}

static int
zfs_secpolicy_umount(zfs_cmd_t *zc, cred_t *cr)
{
	int error;

	error = secpolicy_fs_unmount(cr, NULL);
	if (error) {
		error = dsl_deleg_access(zc->zc_name, ZFS_DELEG_PERM_MOUNT, cr);
	}
	return (error);
}

/*
 * Policy for pool operations - create/destroy pools, add vdevs, etc.  Requires
 * SYS_CONFIG privilege, which is not available in a local zone.
 */
/* ARGSUSED */
static int
zfs_secpolicy_config(zfs_cmd_t *zc, cred_t *cr)
{
	if (secpolicy_sys_config(cr, B_FALSE) != 0)
		return (EPERM);

	return (0);
}

/*
 * Just like zfs_secpolicy_config, except that we will check for
 * mount permission on the dataset for permission to create/remove
 * the minor nodes.
 */
static int
zfs_secpolicy_minor(zfs_cmd_t *zc, cred_t *cr)
{
	if (secpolicy_sys_config(cr, B_FALSE) != 0) {
		return (dsl_deleg_access(zc->zc_name,
		    ZFS_DELEG_PERM_MOUNT, cr));
	}

	return (0);
}

/*
 * Policy for fault injection.  Requires all privileges.
 */
/* ARGSUSED */
static int
zfs_secpolicy_inject(zfs_cmd_t *zc, cred_t *cr)
{
	return (secpolicy_zinject(cr));
}

static int
zfs_secpolicy_inherit(zfs_cmd_t *zc, cred_t *cr)
{
	zfs_prop_t prop = zfs_name_to_prop(zc->zc_value);

	if (prop == ZPROP_INVAL) {
		if (!zfs_prop_user(zc->zc_value))
			return (EINVAL);
		return (zfs_secpolicy_write_perms(zc->zc_name,
		    ZFS_DELEG_PERM_USERPROP, cr));
	} else {
		if (!zfs_prop_inheritable(prop))
			return (EINVAL);
		return (zfs_secpolicy_setprop(zc->zc_name, prop, cr));
	}
}

/*
 * Returns the nvlist as specified by the user in the zfs_cmd_t.
 */
static int
get_nvlist(uint64_t nvl, uint64_t size, nvlist_t **nvp)
{
	char *packed;
	int error;
	nvlist_t *list = NULL;

	/*
	 * Read in and unpack the user-supplied nvlist.
	 */
	if (size == 0)
		return (EINVAL);

	packed = kmem_alloc(size, KM_SLEEP);

	if ((error = xcopyin((void *)(uintptr_t)nvl, packed, size)) != 0) {
		kmem_free(packed, size);
		return (error);
	}

	if ((error = nvlist_unpack(packed, size, &list, 0)) != 0) {
		kmem_free(packed, size);
		return (error);
	}

	kmem_free(packed, size);

	*nvp = list;
	return (0);
}

static int
put_nvlist(zfs_cmd_t *zc, nvlist_t *nvl)
{
	char *packed = NULL;
	size_t size;
	int error;

	VERIFY(nvlist_size(nvl, &size, NV_ENCODE_NATIVE) == 0);

	if (size > zc->zc_nvlist_dst_size) {
		error = ENOMEM;
	} else {
		packed = kmem_alloc(size, KM_SLEEP);
		VERIFY(nvlist_pack(nvl, &packed, &size, NV_ENCODE_NATIVE,
		    KM_SLEEP) == 0);
		error = xcopyout(packed, (void *)(uintptr_t)zc->zc_nvlist_dst,
		    size);
		kmem_free(packed, size);
	}

	zc->zc_nvlist_dst_size = size;
	return (error);
}

static int
zfs_ioc_pool_create(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *config, *props = NULL;
	char *buf;

	if (error = get_nvlist(zc->zc_nvlist_conf, zc->zc_nvlist_conf_size,
	    &config))
		return (error);

	if (zc->zc_nvlist_src_size != 0 && (error =
	    get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size, &props))) {
		nvlist_free(config);
		return (error);
	}

	buf = history_str_get(zc);

	error = spa_create(zc->zc_name, config, props, buf);

	if (buf != NULL)
		history_str_free(buf);

	nvlist_free(config);

	if (props)
		nvlist_free(props);

	return (error);
}

static int
zfs_ioc_pool_destroy(zfs_cmd_t *zc)
{
	int error;
	zfs_log_history(zc);
	error = spa_destroy(zc->zc_name);
	return (error);
}

static int
zfs_ioc_pool_import(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *config, *props = NULL;
	uint64_t guid;

	if ((error = get_nvlist(zc->zc_nvlist_conf, zc->zc_nvlist_conf_size,
	    &config)) != 0)
		return (error);

	if (zc->zc_nvlist_src_size != 0 && (error =
	    get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size, &props))) {
		nvlist_free(config);
		return (error);
	}

	if (nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID, &guid) != 0 ||
	    guid != zc->zc_guid)
		error = EINVAL;
	else
		error = spa_import(zc->zc_name, config, props);

	nvlist_free(config);

	if (props)
		nvlist_free(props);

	return (error);
}

static int
zfs_ioc_pool_export(zfs_cmd_t *zc)
{
	int error;
	zfs_log_history(zc);
	error = spa_export(zc->zc_name, NULL);
	return (error);
}

static int
zfs_ioc_pool_configs(zfs_cmd_t *zc)
{
	nvlist_t *configs;
	int error;

	if ((configs = spa_all_configs(&zc->zc_cookie)) == NULL)
		return (EEXIST);

	error = put_nvlist(zc, configs);

	nvlist_free(configs);

	return (error);
}

static int
zfs_ioc_pool_stats(zfs_cmd_t *zc)
{
	nvlist_t *config;
	int error;
	int ret = 0;

	error = spa_get_stats(zc->zc_name, &config, zc->zc_value,
	    sizeof (zc->zc_value));

	if (config != NULL) {
		ret = put_nvlist(zc, config);
		nvlist_free(config);

		/*
		 * The config may be present even if 'error' is non-zero.
		 * In this case we return success, and preserve the real errno
		 * in 'zc_cookie'.
		 */
		zc->zc_cookie = error;
	} else {
		ret = error;
	}

	return (ret);
}

/*
 * Try to import the given pool, returning pool stats as appropriate so that
 * user land knows which devices are available and overall pool health.
 */
static int
zfs_ioc_pool_tryimport(zfs_cmd_t *zc)
{
	nvlist_t *tryconfig, *config;
	int error;

	if ((error = get_nvlist(zc->zc_nvlist_conf, zc->zc_nvlist_conf_size,
	    &tryconfig)) != 0)
		return (error);

	config = spa_tryimport(tryconfig);

	nvlist_free(tryconfig);

	if (config == NULL)
		return (EINVAL);

	error = put_nvlist(zc, config);
	nvlist_free(config);

	return (error);
}

static int
zfs_ioc_pool_scrub(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	mutex_enter(&spa_namespace_lock);
	error = spa_scrub(spa, zc->zc_cookie, B_FALSE);
	mutex_exit(&spa_namespace_lock);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_freeze(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error == 0) {
		spa_freeze(spa);
		spa_close(spa, FTAG);
	}
	return (error);
}

static int
zfs_ioc_pool_upgrade(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	if (zc->zc_cookie < spa_version(spa) || zc->zc_cookie > SPA_VERSION) {
		spa_close(spa, FTAG);
		return (EINVAL);
	}

	spa_upgrade(spa, zc->zc_cookie);
	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_get_history(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *hist_buf;
	uint64_t size;
	int error;

	if ((size = zc->zc_history_len) == 0)
		return (EINVAL);

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	if (spa_version(spa) < SPA_VERSION_ZPOOL_HISTORY) {
		spa_close(spa, FTAG);
		return (ENOTSUP);
	}

	hist_buf = kmem_alloc(size, KM_SLEEP);
	if ((error = spa_history_get(spa, &zc->zc_history_offset,
	    &zc->zc_history_len, hist_buf)) == 0) {
		error = xcopyout(hist_buf,
		    (char *)(uintptr_t)zc->zc_history,
		    zc->zc_history_len);
	}

	spa_close(spa, FTAG);
	kmem_free(hist_buf, size);
	return (error);
}

static int
zfs_ioc_dsobj_to_dsname(zfs_cmd_t *zc)
{
	int error;

	if (error = dsl_dsobj_to_dsname(zc->zc_name, zc->zc_obj, zc->zc_value))
		return (error);

	return (0);
}

static int
zfs_ioc_obj_to_path(zfs_cmd_t *zc)
{
	objset_t *osp;
	int error;

	if ((error = dmu_objset_open(zc->zc_name, DMU_OST_ZFS,
	    DS_MODE_NONE | DS_MODE_READONLY, &osp)) != 0)
		return (error);

	error = zfs_obj_to_path(osp, zc->zc_obj, zc->zc_value,
	    sizeof (zc->zc_value));
	dmu_objset_close(osp);

	return (error);
}

static int
zfs_ioc_vdev_add(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	nvlist_t *config;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);

	/*
	 * A root pool with concatenated devices is not supported.
	 * Thus, can not add a device to a root pool with one device.
	 */
	if (spa->spa_root_vdev->vdev_children == 1 && spa->spa_bootfs != 0) {
		spa_close(spa, FTAG);
		return (EDOM);
	}

	if ((error = get_nvlist(zc->zc_nvlist_conf, zc->zc_nvlist_conf_size,
	    &config)) == 0) {
		error = spa_vdev_add(spa, config);
		nvlist_free(config);
	}
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_remove(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);
	error = spa_vdev_remove(spa, zc->zc_guid, B_FALSE);
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_set_state(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	vdev_state_t newstate = VDEV_STATE_UNKNOWN;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);
	switch (zc->zc_cookie) {
	case VDEV_STATE_ONLINE:
		error = vdev_online(spa, zc->zc_guid, zc->zc_obj, &newstate);
		break;

	case VDEV_STATE_OFFLINE:
		error = vdev_offline(spa, zc->zc_guid, zc->zc_obj);
		break;

	case VDEV_STATE_FAULTED:
		error = vdev_fault(spa, zc->zc_guid);
		break;

	case VDEV_STATE_DEGRADED:
		error = vdev_degrade(spa, zc->zc_guid);
		break;

	default:
		error = EINVAL;
	}
	zc->zc_cookie = newstate;
	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_attach(zfs_cmd_t *zc)
{
	spa_t *spa;
	int replacing = zc->zc_cookie;
	nvlist_t *config;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	if ((error = get_nvlist(zc->zc_nvlist_conf, zc->zc_nvlist_conf_size,
	    &config)) == 0) {
		error = spa_vdev_attach(spa, zc->zc_guid, config, replacing);
		nvlist_free(config);
	}

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_detach(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_vdev_detach(spa, zc->zc_guid, B_FALSE);

	spa_close(spa, FTAG);
	return (error);
}

static int
zfs_ioc_vdev_setpath(zfs_cmd_t *zc)
{
	spa_t *spa;
	char *path = zc->zc_value;
	uint64_t guid = zc->zc_guid;
	int error;

	error = spa_open(zc->zc_name, &spa, FTAG);
	if (error != 0)
		return (error);

	error = spa_vdev_setpath(spa, guid, path);
	spa_close(spa, FTAG);
	return (error);
}

/*
 * inputs:
 * zc_name		name of filesystem
 * zc_nvlist_dst_size	size of buffer for property nvlist
 *
 * outputs:
 * zc_objset_stats	stats
 * zc_nvlist_dst	property nvlist
 * zc_nvlist_dst_size	size of property nvlist
 * zc_value		alternate root
 */
static int
zfs_ioc_objset_stats(zfs_cmd_t *zc)
{
	objset_t *os = NULL;
	int error;
	nvlist_t *nv;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		return (error);
	}

	dmu_objset_fast_stat(os, &zc->zc_objset_stats);

	if (zc->zc_nvlist_dst != 0 &&
	    (error = dsl_prop_get_all(os, &nv)) == 0) {
		dmu_objset_stats(os, nv);
		/*
		 * NB: zvol_get_stats() will read the objset contents,
		 * which we aren't supposed to do with a
		 * DS_MODE_STANDARD open, because it could be
		 * inconsistent.  So this is a bit of a workaround...
		 */
		if (!zc->zc_objset_stats.dds_inconsistent) {
			if (dmu_objset_type(os) == DMU_OST_ZVOL)
				VERIFY(zvol_get_stats(os, nv) == 0);
		}
		error = put_nvlist(zc, nv);
		nvlist_free(nv);
	}

	spa_altroot(dmu_objset_spa(os), zc->zc_value, sizeof (zc->zc_value));

	dmu_objset_close(os);
	return (error);
}

/*
 * inputs:
 * zc_name		name of filesystem
 * zc_cookie		zap cursor
 * zc_nvlist_dst_size	size of buffer for property nvlist
 *
 * outputs:
 * zc_name		name of next filesystem
 * zc_objset_stats	stats
 * zc_nvlist_dst	property nvlist
 * zc_nvlist_dst_size	size of property nvlist
 * zc_value		alternate root
 */
static int
zfs_ioc_objset_version(zfs_cmd_t *zc)
{
	objset_t *os = NULL;
	int error;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		return (error);
	}

	dmu_objset_fast_stat(os, &zc->zc_objset_stats);

	/*
	 * NB: zfs_get_version() will read the objset contents,
	 * which we aren't supposed to do with a
	 * DS_MODE_STANDARD open, because it could be
	 * inconsistent.  So this is a bit of a workaround...
	 */
	zc->zc_cookie = 0;
	if (!zc->zc_objset_stats.dds_inconsistent)
		if (dmu_objset_type(os) == DMU_OST_ZFS)
			(void) zfs_get_version(os, &zc->zc_cookie);

	dmu_objset_close(os);
	return (0);
}

static int
zfs_ioc_dataset_list_next(zfs_cmd_t *zc)
{
	objset_t *os;
	int error;
	char *p;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		if (error == ENOENT)
			error = ESRCH;
		return (error);
	}

	p = strrchr(zc->zc_name, '/');
	if (p == NULL || p[1] != '\0')
		(void) strlcat(zc->zc_name, "/", sizeof (zc->zc_name));
	p = zc->zc_name + strlen(zc->zc_name);

	do {
		error = dmu_dir_list_next(os,
		    sizeof (zc->zc_name) - (p - zc->zc_name), p,
		    NULL, &zc->zc_cookie);
		if (error == ENOENT)
			error = ESRCH;
	} while (error == 0 && !INGLOBALZONE(curproc) &&
	    !zone_dataset_visible(zc->zc_name, NULL));

	/*
	 * If it's a hidden dataset (ie. with a '$' in its name), don't
	 * try to get stats for it.  Userland will skip over it.
	 */
	if (error == 0 && strchr(zc->zc_name, '$') == NULL)
		error = zfs_ioc_objset_stats(zc); /* fill in the stats */

	dmu_objset_close(os);
	return (error);
}

/*
 * inputs:
 * zc_name		name of filesystem
 * zc_cookie		zap cursor
 * zc_nvlist_dst_size	size of buffer for property nvlist
 *
 * outputs:
 * zc_name		name of next snapshot
 * zc_objset_stats	stats
 * zc_nvlist_dst	property nvlist
 * zc_nvlist_dst_size	size of property nvlist
 * zc_value		alternate root
 */
static int
zfs_ioc_snapshot_list_next(zfs_cmd_t *zc)
{
	objset_t *os;
	int error;

retry:
	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (error != 0) {
		/*
		 * This is ugly: dmu_objset_open() can return EBUSY if
		 * the objset is held exclusively. Fortunately this hold is
		 * only for a short while, so we retry here.
		 * This avoids user code having to handle EBUSY,
		 * for example for a "zfs list".
		 */
		if (error == EBUSY) {
			delay(1);
			goto retry;
		}
		if (error == ENOENT)
			error = ESRCH;
		return (error);
	}

	/*
	 * A dataset name of maximum length cannot have any snapshots,
	 * so exit immediately.
	 */
	if (strlcat(zc->zc_name, "@", sizeof (zc->zc_name)) >= MAXNAMELEN) {
		dmu_objset_close(os);
		return (ESRCH);
	}

	error = dmu_snapshot_list_next(os,
	    sizeof (zc->zc_name) - strlen(zc->zc_name),
	    zc->zc_name + strlen(zc->zc_name), NULL, &zc->zc_cookie);
	if (error == ENOENT)
		error = ESRCH;

	if (error == 0)
		error = zfs_ioc_objset_stats(zc); /* fill in the stats */

	/* if we failed, undo the @ that we tacked on to zc_name */
	if (error != 0)
		*strchr(zc->zc_name, '@') = '\0';

	dmu_objset_close(os);
	return (error);
}

static int
zfs_set_prop_nvlist(const char *name, nvlist_t *nvl)
{
	nvpair_t *elem;
	int error;
	uint64_t intval;
	char *strval;

	/*
	 * First validate permission to set all of the properties
	 */
	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {
		const char *propname = nvpair_name(elem);
		zfs_prop_t prop = zfs_name_to_prop(propname);

		if (prop == ZPROP_INVAL) {
			/*
			 * If this is a user-defined property, it must be a
			 * string, and there is no further validation to do.
			 */
			if (!zfs_prop_user(propname) ||
			    nvpair_type(elem) != DATA_TYPE_STRING)
				return (EINVAL);

			if (error = zfs_secpolicy_write_perms(name,
			    ZFS_DELEG_PERM_USERPROP, CRED()))
				return (error);
			continue;
		}

		if ((error = zfs_secpolicy_setprop(name, prop, CRED())) != 0)
			return (error);

		/*
		 * Check that this value is valid for this pool version
		 */
		switch (prop) {
		case ZFS_PROP_COMPRESSION:
			/*
			 * If the user specified gzip compression, make sure
			 * the SPA supports it. We ignore any errors here since
			 * we'll catch them later.
			 */
			if (nvpair_type(elem) == DATA_TYPE_UINT64 &&
			    nvpair_value_uint64(elem, &intval) == 0 &&
			    intval >= ZIO_COMPRESS_GZIP_1 &&
			    intval <= ZIO_COMPRESS_GZIP_9) {
				if (zfs_check_version(name,
				    SPA_VERSION_GZIP_COMPRESSION))
					return (ENOTSUP);
			}
			break;

		case ZFS_PROP_COPIES:
			if (zfs_check_version(name, SPA_VERSION_DITTO_BLOCKS))
				return (ENOTSUP);
			break;
		case ZFS_PROP_NORMALIZE:
		case ZFS_PROP_UTF8ONLY:
		case ZFS_PROP_CASE:
			if (zfs_check_version(name, SPA_VERSION_NORMALIZATION))
				return (ENOTSUP);

		}
		if ((error = zfs_secpolicy_setprop(name, prop, CRED())) != 0)
			return (error);
	}

	elem = NULL;
	while ((elem = nvlist_next_nvpair(nvl, elem)) != NULL) {
		const char *propname = nvpair_name(elem);
		zfs_prop_t prop = zfs_name_to_prop(propname);

		if (prop == ZPROP_INVAL) {
			VERIFY(nvpair_value_string(elem, &strval) == 0);
			error = dsl_prop_set(name, propname, 1,
			    strlen(strval) + 1, strval);
			if (error == 0)
				continue;
			else
				return (error);
		}

		switch (prop) {
		case ZFS_PROP_QUOTA:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dir_set_quota(name, intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_REFQUOTA:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dataset_set_quota(name, intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_RESERVATION:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dir_set_reservation(name,
			    intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_REFRESERVATION:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = dsl_dataset_set_reservation(name,
			    intval)) != 0)
				return (error);
			break;

		case ZFS_PROP_VOLSIZE:
			/* ZFSFUSE: ZVols not implemented */
			return ENXIO;

		case ZFS_PROP_VOLBLOCKSIZE:
			/* ZFSFUSE: ZVols not implemented */
			return ENXIO;
			break;

		case ZFS_PROP_VERSION:
			if ((error = nvpair_value_uint64(elem, &intval)) != 0 ||
			    (error = zfs_set_version(name, intval)) != 0)
			break;

		default:
			if (nvpair_type(elem) == DATA_TYPE_STRING) {
				if (zfs_prop_get_type(prop) !=
				    PROP_TYPE_STRING)
					return (EINVAL);
				VERIFY(nvpair_value_string(elem, &strval) == 0);
				if ((error = dsl_prop_set(name,
				    nvpair_name(elem), 1, strlen(strval) + 1,
				    strval)) != 0)
					return (error);
			} else if (nvpair_type(elem) == DATA_TYPE_UINT64) {
				const char *unused;

				VERIFY(nvpair_value_uint64(elem, &intval) == 0);

				switch (zfs_prop_get_type(prop)) {
				case PROP_TYPE_NUMBER:
					break;
				case PROP_TYPE_STRING:
					return (EINVAL);
				case PROP_TYPE_INDEX:
					if (zfs_prop_index_to_string(prop,
					    intval, &unused) != 0)
						return (EINVAL);
					break;
				default:
					cmn_err(CE_PANIC,
					    "unknown property type");
					break;
				}

				if ((error = dsl_prop_set(name, propname,
				    8, 1, &intval)) != 0)
					return (error);
			} else {
				return (EINVAL);
			}
			break;
		}
	}

	return (0);
}

/*
 * inputs:
 * zc_name		name of filesystem
 * zc_value		name of property to inherit
 * zc_nvlist_src{_size}	nvlist of properties to apply
 *
 * outputs:		none
 */
static int
zfs_ioc_set_prop(zfs_cmd_t *zc)
{
	nvlist_t *nvl;
	int error;

	if ((error = get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size,
	    &nvl)) != 0)
		return (error);

	error = zfs_set_prop_nvlist(zc->zc_name, nvl);

	nvlist_free(nvl);
	return (error);
}

/*
 * inputs:
 * zc_name		name of filesystem
 * zc_value		name of property to inherit
 *
 * outputs:		none
 */
static int
zfs_ioc_inherit_prop(zfs_cmd_t *zc)
{
	/* the property name has been validated by zfs_secpolicy_inherit() */
	return (dsl_prop_set(zc->zc_name, zc->zc_value, 0, 0, NULL));
}

static int
zfs_ioc_pool_set_props(zfs_cmd_t *zc)
{
	nvlist_t *props;
	spa_t *spa;
	int error;

	if ((error = get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size,
	    &props)))
		return (error);

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0) {
		nvlist_free(props);
		return (error);
	}

	error = spa_prop_set(spa, props);

	nvlist_free(props);
	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_pool_get_props(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	nvlist_t *nvp = NULL;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_prop_get(spa, &nvp);

	if (error == 0 && zc->zc_nvlist_dst != NULL)
		error = put_nvlist(zc, nvp);
	else
		error = EFAULT;

	spa_close(spa, FTAG);

	if (nvp)
		nvlist_free(nvp);
	return (error);
}

static int
zfs_ioc_iscsi_perm_check(zfs_cmd_t *zc)
{
	nvlist_t *nvp;
	int error;
	uint32_t uid;
	uint32_t gid;
	uint32_t *groups;
	uint_t group_cnt;
	cred_t	*usercred;

	if ((error = get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size,
	    &nvp)) != 0) {
		return (error);
	}

	if ((error = nvlist_lookup_uint32(nvp,
	    ZFS_DELEG_PERM_UID, &uid)) != 0) {
		nvlist_free(nvp);
		return (EPERM);
	}

	if ((error = nvlist_lookup_uint32(nvp,
	    ZFS_DELEG_PERM_GID, &gid)) != 0) {
		nvlist_free(nvp);
		return (EPERM);
	}

	if ((error = nvlist_lookup_uint32_array(nvp, ZFS_DELEG_PERM_GROUPS,
	    &groups, &group_cnt)) != 0) {
		nvlist_free(nvp);
		return (EPERM);
	}
	usercred = cralloc();
	if ((crsetugid(usercred, uid, gid) != 0) ||
	    (crsetgroups(usercred, group_cnt, (gid_t *)groups) != 0)) {
		nvlist_free(nvp);
		crfree(usercred);
		return (EPERM);
	}
	nvlist_free(nvp);
	error = dsl_deleg_access(zc->zc_name,
	    zfs_prop_to_name(ZFS_PROP_SHAREISCSI), usercred);
	crfree(usercred);
	return (error);
}

/*
 * inputs:
 * zc_name		name of filesystem
 * zc_nvlist_src{_size}	nvlist of delegated permissions
 * zc_perm_action	allow/unallow flag
 *
 * outputs:		none
 */
static int
zfs_ioc_set_fsacl(zfs_cmd_t *zc)
{
	int error;
	nvlist_t *fsaclnv = NULL;

	if ((error = get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size,
	    &fsaclnv)) != 0)
		return (error);

	/*
	 * Verify nvlist is constructed correctly
	 */
	if ((error = zfs_deleg_verify_nvlist(fsaclnv)) != 0) {
		nvlist_free(fsaclnv);
		return (EINVAL);
	}

	/*
	 * If we don't have PRIV_SYS_MOUNT, then validate
	 * that user is allowed to hand out each permission in
	 * the nvlist(s)
	 */

	error = secpolicy_zfs(CRED());
	if (error) {
		if (zc->zc_perm_action == B_FALSE) {
			error = dsl_deleg_can_allow(zc->zc_name,
			    fsaclnv, CRED());
		} else {
			error = dsl_deleg_can_unallow(zc->zc_name,
			    fsaclnv, CRED());
		}
	}

	if (error == 0)
		error = dsl_deleg_set(zc->zc_name, fsaclnv, zc->zc_perm_action);

	nvlist_free(fsaclnv);
	return (error);
}

/*
 * inputs:
 * zc_name		name of filesystem
 *
 * outputs:
 * zc_nvlist_src{_size}	nvlist of delegated permissions
 */
static int
zfs_ioc_get_fsacl(zfs_cmd_t *zc)
{
	nvlist_t *nvp;
	int error;

	if ((error = dsl_deleg_get(zc->zc_name, &nvp)) == 0) {
		error = put_nvlist(zc, nvp);
		nvlist_free(nvp);
	}

	return (error);
}

/*
 * inputs:
 * zc_name		name of volume
 *
 * outputs:		none
 */
static int
zfs_ioc_create_minor(zfs_cmd_t *zc)
{
	/* ZFSFUSE TODO: implement ZVOLs */
	return ENXIO;
}

/*
 * inputs:
 * zc_name		name of volume
 *
 * outputs:		none
 */
static int
zfs_ioc_remove_minor(zfs_cmd_t *zc)
{
	/* ZFSFUSE TODO: implement ZVOLs */
	return ENXIO;
}

/*
 * Search the vfs list for a specified resource.  Returns a pointer to it
 * or NULL if no suitable entry is found. The caller of this routine
 * is responsible for releasing the returned vfs pointer.
 */
#if 0
static vfs_t *
zfs_get_vfs(const char *resource)
{
	struct vfs *vfsp;
	struct vfs *vfs_found = NULL;

	vfs_list_read_lock();
	vfsp = rootvfs;
	do {
		if (strcmp(refstr_value(vfsp->vfs_resource), resource) == 0) {
			VFS_HOLD(vfsp);
			vfs_found = vfsp;
			break;
		}
		vfsp = vfsp->vfs_next;
	} while (vfsp != rootvfs);
	vfs_list_unlock();
	return (vfs_found);
}
#endif

/* ARGSUSED */
static void
zfs_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	zfs_creat_t *zct = arg;
	uint64_t version;

	if (spa_version(dmu_objset_spa(os)) >= SPA_VERSION_FUID)
		version = ZPL_VERSION;
	else
		version = ZPL_VERSION_FUID - 1;

	(void) nvlist_lookup_uint64(zct->zct_props,
	    zfs_prop_to_name(ZFS_PROP_VERSION), &version);

	zfs_create_fs(os, cr, version, zct->zct_norm, tx);
}

/*
 * zfs_prop_lookup()
 *
 * Look for the property first in the existing property nvlist.  If
 * it's already present, you're done.  If it's not there, attempt to
 * find the property value from a parent dataset.  If that fails, fall
 * back to the property's default value.  In either of these two
 * cases, if update is TRUE, add a value for the property to the
 * property nvlist.
 *
 * If the rval pointer is non-NULL, copy the discovered value to rval.
 *
 * If we get any unexpected errors, bail and return the error number
 * to the caller.
 *
 * If we succeed, return 0.
 */
static int
zfs_prop_lookup(const char *parentname, zfs_prop_t propnum,
    nvlist_t *proplist, uint64_t *rval, boolean_t update)
{
	const char *propname;
	uint64_t value;
	int error = ENOENT;

	propname = zfs_prop_to_name(propnum);
	if (proplist != NULL)
		error = nvlist_lookup_uint64(proplist, propname, &value);
	if (error == ENOENT) {
		error = dsl_prop_get_integer(parentname, propname,
		    &value, NULL);
		if (error == ENOENT)
			value = zfs_prop_default_numeric(propnum);
		else if (error != 0)
			return (error);
		if (update) {
			ASSERT(proplist != NULL);
			error = nvlist_add_uint64(proplist, propname, value);
		}
	}
	if (error == 0 && rval)
		*rval = value;
	return (error);
}

/*
 * zfs_normalization_get
 *
 * Get the normalization flag value.  If the properties have
 * non-default values, make sure the pool version is recent enough to
 * support these choices.
 */
static int
zfs_normalization_get(const char *dataset, nvlist_t *proplist, int *norm,
    boolean_t update)
{
	char parentname[MAXNAMELEN];
	char poolname[MAXNAMELEN];
	char *cp;
	uint64_t value;
	int check = 0;
	int error;

	ASSERT(norm != NULL);
	*norm = 0;

	(void) strncpy(parentname, dataset, sizeof (parentname));
	cp = strrchr(parentname, '@');
	if (cp != NULL) {
		cp[0] = '\0';
	} else {
		cp = strrchr(parentname, '/');
		if (cp == NULL)
			return (ENOENT);
		cp[0] = '\0';
	}

	(void) strncpy(poolname, dataset, sizeof (poolname));
	cp = strchr(poolname, '/');
	if (cp != NULL)
		cp[0] = '\0';

	/*
	 * Make sure pool is of new enough vintage to support normalization.
	 */
	if (zfs_check_version(poolname, SPA_VERSION_NORMALIZATION))
		return (0);

	error = zfs_prop_lookup(parentname, ZFS_PROP_UTF8ONLY,
	    proplist, &value, update);
	if (error != 0)
		return (error);
	if (value != zfs_prop_default_numeric(ZFS_PROP_UTF8ONLY))
		check = 1;

	error = zfs_prop_lookup(parentname, ZFS_PROP_NORMALIZE,
	    proplist, &value, update);
	if (error != 0)
		return (error);
	if (value != zfs_prop_default_numeric(ZFS_PROP_NORMALIZE)) {
		check = 1;
		switch ((int)value) {
		case ZFS_NORMALIZE_NONE:
			break;
		case ZFS_NORMALIZE_C:
			*norm |= U8_TEXTPREP_NFC;
			break;
		case ZFS_NORMALIZE_D:
			*norm |= U8_TEXTPREP_NFD;
			break;
		case ZFS_NORMALIZE_KC:
			*norm |= U8_TEXTPREP_NFKC;
			break;
		case ZFS_NORMALIZE_KD:
			*norm |= U8_TEXTPREP_NFKD;
			break;
		default:
			ASSERT((int)value >= ZFS_NORMALIZE_NONE);
			ASSERT((int)value <= ZFS_NORMALIZE_KD);
			break;
		}
	}

	error = zfs_prop_lookup(parentname, ZFS_PROP_CASE,
	    proplist, &value, update);
	if (error != 0)
		return (error);
	if (value != zfs_prop_default_numeric(ZFS_PROP_CASE)) {
		check = 1;
		switch ((int)value) {
		case ZFS_CASE_SENSITIVE:
			break;
		case ZFS_CASE_INSENSITIVE:
			*norm |= U8_TEXTPREP_TOUPPER;
			break;
		case ZFS_CASE_MIXED:
			*norm |= U8_TEXTPREP_TOUPPER;
			break;
		default:
			ASSERT((int)value >= ZFS_CASE_SENSITIVE);
			ASSERT((int)value <= ZFS_CASE_MIXED);
			break;
		}
	}

	/*
	 * At the moment we are disabling non-default values for these
	 * properties because they cannot be preserved properly with a
	 * zfs send.
	 */
	if (check == 1)
		return (ENOTSUP);

	return (0);
}

/*
 * inputs:
 * zc_objset_type	type of objset to create (fs vs zvol)
 * zc_name		name of new objset
 * zc_value		name of snapshot to clone from (may be empty)
 * zc_nvlist_src{_size}	nvlist of properties to apply
 *
 * outputs:		none
 */
static int
zfs_ioc_create(zfs_cmd_t *zc)
{
	objset_t *clone;
	int error = 0;
	zfs_creat_t zct;
	nvlist_t *nvprops = NULL;
	void (*cbfunc)(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx);
	dmu_objset_type_t type = zc->zc_objset_type;

	switch (type) {

	case DMU_OST_ZFS:
		cbfunc = zfs_create_cb;
		break;
	case DMU_OST_ZVOL:
		/* ZFSFUSE: TODO Implement ZVOLs */
		/*cbfunc = zvol_create_cb;*/
		return ENOSYS;
	default:
		cbfunc = NULL;
	}
	if (strchr(zc->zc_name, '@') ||
	    strchr(zc->zc_name, '%'))
		return (EINVAL);

	if (zc->zc_nvlist_src != (uint64_t)(uintptr_t) NULL &&
	    (error = get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size,
	    &nvprops)) != 0)
		return (error);

	zct.zct_norm = 0;
	zct.zct_props = nvprops;

	if (zc->zc_value[0] != '\0') {
		/*
		 * We're creating a clone of an existing snapshot.
		 */
		zc->zc_value[sizeof (zc->zc_value) - 1] = '\0';
		if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0) {
			nvlist_free(nvprops);
			return (EINVAL);
		}

		error = dmu_objset_open(zc->zc_value, type,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &clone);
		if (error) {
			nvlist_free(nvprops);
			return (error);
		}
		error = dmu_objset_create(zc->zc_name, type, clone, NULL, NULL);
		if (error) {
			dmu_objset_close(clone);
			nvlist_free(nvprops);
			return (error);
		}
		/*
		 * If caller did not provide any properties, allocate
		 * an nvlist for properties, as we will be adding our set-once
		 * properties to it.  This carries the choices made on the
		 * original file system into the clone.
		 */
		if (nvprops == NULL)
			VERIFY(nvlist_alloc(&nvprops,
			    NV_UNIQUE_NAME, KM_SLEEP) == 0);

		/*
		 * We have to have normalization and case-folding
		 * flags correct when we do the file system creation,
		 * so go figure them out now.  All we really care about
		 * here is getting these values into the property list.
		 */
		error = zfs_normalization_get(zc->zc_value, nvprops,
		    &zct.zct_norm, B_TRUE);
		if (error != 0) {
			dmu_objset_close(clone);
			nvlist_free(nvprops);
			return (error);
		}
		dmu_objset_close(clone);
	} else {
		if (cbfunc == NULL) {
			nvlist_free(nvprops);
			return (EINVAL);
		}

	/* ZFSFUSE: TODO Implement ZVOLs */
#if 0
		if (type == DMU_OST_ZVOL) {
			uint64_t volsize, volblocksize;

			if (nvprops == NULL ||
			    nvlist_lookup_uint64(nvprops,
			    zfs_prop_to_name(ZFS_PROP_VOLSIZE),
			    &volsize) != 0) {
				nvlist_free(nvprops);
				return (EINVAL);
			}

			if ((error = nvlist_lookup_uint64(nvprops,
			    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE),
			    &volblocksize)) != 0 && error != ENOENT) {
				nvlist_free(nvprops);
				return (EINVAL);
			}

			if (error != 0)
				volblocksize = zfs_prop_default_numeric(
				    ZFS_PROP_VOLBLOCKSIZE);

			if ((error = zvol_check_volblocksize(
			    volblocksize)) != 0 ||
			    (error = zvol_check_volsize(volsize,
			    volblocksize)) != 0) {
				nvlist_free(nvprops);
				return (error);
			}
		} else if (type == DMU_OST_ZFS) {
			uint64_t version;
			int error;

			error = nvlist_lookup_uint64(nvprops,
			    zfs_prop_to_name(ZFS_PROP_VERSION), &version);

			if (error == 0 && (version < ZPL_VERSION_INITIAL ||
			    version > ZPL_VERSION)) {
				nvlist_free(nvprops);
				return (ENOTSUP);
			} else if (error == 0 && version >= ZPL_VERSION_FUID &&
			    zfs_check_version(zc->zc_name, SPA_VERSION_FUID)) {
				nvlist_free(nvprops);
				return (ENOTSUP);
			}
#endif

			/*
			 * We have to have normalization and
			 * case-folding flags correct when we do the
			 * file system creation, so go figure them out
			 * now.  The final argument to zfs_normalization_get()
			 * tells that routine not to update the nvprops
			 * list.
			 */
			error = zfs_normalization_get(zc->zc_name, nvprops,
			    &zct.zct_norm, B_FALSE);
			if (error != 0) {
				nvlist_free(nvprops);
				return (error);
			}
		}
		error = dmu_objset_create(zc->zc_name, type, NULL, cbfunc,
		    &zct);
	}

	/*
	 * It would be nice to do this atomically.
	 */
	if (error == 0) {
		if ((error = zfs_set_prop_nvlist(zc->zc_name, nvprops)) != 0)
			(void) dmu_objset_destroy(zc->zc_name);
	}

	nvlist_free(nvprops);
	return (error);
}

/*
 * inputs:
 * zc_name	name of filesystem
 * zc_value	short name of snapshot
 * zc_cookie	recursive flag
 *
 * outputs:	none
 */
static int
zfs_ioc_snapshot(zfs_cmd_t *zc)
{
	if (snapshot_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);
	return (dmu_objset_snapshot(zc->zc_name,
	    zc->zc_value, zc->zc_cookie));
}

int
zfs_unmount_snap(char *name, void *arg)
{
	/* ZFSFUSE: TODO */
#if 0
	char *snapname = arg;
	char *cp;
	vfs_t *vfsp = NULL;

	/*
	 * Snapshots (which are under .zfs control) must be unmounted
	 * before they can be destroyed.
	 */

	if (snapname) {
		(void) strcat(name, "@");
		(void) strcat(name, snapname);
		vfsp = zfs_get_vfs(name);
		cp = strchr(name, '@');
		*cp = '\0';
	} else if (strchr(name, '@')) {
		vfsp = zfs_get_vfs(name);
	}

	if (vfsp) {
		/*
		 * Always force the unmount for snapshots.
		 */
		int flag = MS_FORCE;
		int err;

		if ((err = vn_vfswlock(vfsp->vfs_vnodecovered)) != 0) {
			VFS_RELE(vfsp);
			return (err);
		}
		VFS_RELE(vfsp);
		if ((err = dounmount(vfsp, flag, kcred)) != 0)
			return (err);
	}
#endif
	return (0);
}

/*
 * inputs:
 * zc_name	name of filesystem
 * zc_value	short name of snapshot
 *
 * outputs:	none
 */
static int
zfs_ioc_destroy_snaps(zfs_cmd_t *zc)
{
	int err;

	if (snapshot_namecheck(zc->zc_value, NULL, NULL) != 0)
		return (EINVAL);
	err = dmu_objset_find(zc->zc_name,
	    zfs_unmount_snap, zc->zc_value, DS_FIND_CHILDREN);
	if (err)
		return (err);
	return (dmu_snapshots_destroy(zc->zc_name, zc->zc_value));
}

/*
 * inputs:
 * zc_name		name of dataset to destroy
 * zc_objset_type	type of objset
 *
 * outputs:		none
 */
static int
zfs_ioc_destroy(zfs_cmd_t *zc)
{
	if (strchr(zc->zc_name, '@') && zc->zc_objset_type == DMU_OST_ZFS) {
		int err = zfs_unmount_snap(zc->zc_name, NULL);
		if (err)
			return (err);
	}

	return (dmu_objset_destroy(zc->zc_name));
}

/*
 * inputs:
 * zc_name	name of snapshot to roll back to
 *
 * outputs:	none
 */
static int
zfs_ioc_rollback(zfs_cmd_t *zc)
{
	return (dmu_objset_rollback(zc->zc_name));
}

/*
 * inputs:
 * zc_name	old name of dataset
 * zc_value	new name of dataset
 * zc_cookie	recursive flag (only valid for snapshots)
 *
 * outputs:	none
 */
static int
zfs_ioc_rename(zfs_cmd_t *zc)
{
	boolean_t recursive = zc->zc_cookie & 1;

	zc->zc_value[sizeof (zc->zc_value) - 1] = '\0';
	if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0 ||
	    strchr(zc->zc_value, '%'))
		return (EINVAL);

	/*
	 * Unmount snapshot unless we're doing a recursive rename,
	 * in which case the dataset code figures out which snapshots
	 * to unmount.
	 */
	if (!recursive && strchr(zc->zc_name, '@') != NULL &&
	    zc->zc_objset_type == DMU_OST_ZFS) {
		int err = zfs_unmount_snap(zc->zc_name, NULL);
		if (err)
			return (err);
	}

	return (dmu_objset_rename(zc->zc_name, zc->zc_value, recursive));
}

/*
 * inputs:
 * zc_name		name of containing filesystem
 * zc_nvlist_src{_size}	nvlist of properties to apply
 * zc_value		name of snapshot to create
 * zc_string		name of clone origin (if DRR_FLAG_CLONE)
 * zc_cookie		file descriptor to recv from
 * zc_begin_record	the BEGIN record of the stream (not byteswapped)
 * zc_guid		force flag
 *
 * outputs:
 * zc_cookie		number of bytes read
 */
static int
zfs_ioc_recv(zfs_cmd_t *zc)
{
	file_t *fp;
	objset_t *os;
	dmu_recv_cookie_t drc;
	zfsvfs_t *zfsvfs = NULL;
	boolean_t force = (boolean_t)zc->zc_guid;
	int error, fd;
	offset_t off;
	nvlist_t *props = NULL;
	objset_t *origin = NULL;
	char *tosnap;
	char tofs[ZFS_MAXNAMELEN];

	if (dataset_namecheck(zc->zc_value, NULL, NULL) != 0 ||
	    strchr(zc->zc_value, '@') == NULL ||
	    strchr(zc->zc_value, '%'))
		return (EINVAL);

	(void) strcpy(tofs, zc->zc_value);
	tosnap = strchr(tofs, '@');
	*tosnap = '\0';
	tosnap++;

	if (zc->zc_nvlist_src != NULL &&
	    (error = get_nvlist(zc->zc_nvlist_src, zc->zc_nvlist_src_size,
	    &props)) != 0)
		return (error);

	fd = zc->zc_cookie;
	fp = getf(fd);
	if (fp == NULL) {
		nvlist_free(props);
		return (EBADF);
	}

	/*
	 * Get the zfsvfs for the receiving objset. There
	 * won't be one if we're operating on a zvol, if the
	 * objset doesn't exist yet, or is not mounted.
	 */

	error = dmu_objset_open(tofs, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &os);
	if (!error) {
		if (dmu_objset_type(os) == DMU_OST_ZFS) {
			mutex_enter(&os->os->os_user_ptr_lock);
			zfsvfs = dmu_objset_get_user(os);
			if (zfsvfs != NULL)
				VFS_HOLD(zfsvfs->z_vfs);
			mutex_exit(&os->os->os_user_ptr_lock);
		}
		dmu_objset_close(os);
	}

	if (zc->zc_string[0]) {
		error = dmu_objset_open(zc->zc_string, DMU_OST_ANY,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &origin);
		if (error) {
			if (zfsvfs != NULL)
				VFS_RELE(zfsvfs->z_vfs);
			nvlist_free(props);
			releasef(fd);
			return (error);
		}
	}

	error = dmu_recv_begin(tofs, tosnap, &zc->zc_begin_record,
	    force, origin, zfsvfs != NULL, &drc);
	if (origin)
		dmu_objset_close(origin);
	if (error) {
		if (zfsvfs != NULL)
			VFS_RELE(zfsvfs->z_vfs);
		nvlist_free(props);
		releasef(fd);
		return (error);
	}

	/*
	 * If properties are supplied, they are to completely replace
	 * the existing ones; "inherit" any existing properties.
	 */
	if (props) {
		objset_t *os;
		nvlist_t *nv = NULL;

		error = dmu_objset_open(tofs, DMU_OST_ANY,
		    DS_MODE_STANDARD | DS_MODE_READONLY | DS_MODE_INCONSISTENT,
		    &os);
		if (error == 0) {
			error = dsl_prop_get_all(os, &nv);
			dmu_objset_close(os);
		}
		if (error == 0) {
			nvpair_t *elem;
			zfs_cmd_t zc2 = { 0 };

			(void) strcpy(zc2.zc_name, tofs);
			for (elem = nvlist_next_nvpair(nv, NULL); elem;
			    elem = nvlist_next_nvpair(nv, elem)) {
				(void) strcpy(zc2.zc_value, nvpair_name(elem));
				if (zfs_secpolicy_inherit(&zc2, CRED()) == 0)
					(void) zfs_ioc_inherit_prop(&zc2);
			}
		}
		if (nv)
			nvlist_free(nv);
	}

	/*
	 * Set properties.  Note, we ignore errors.  Would be better to
	 * do best-effort in zfs_set_prop_nvlist, too.
	 */
	(void) zfs_set_prop_nvlist(tofs, props);
	nvlist_free(props);

	off = fp->f_offset;
	error = dmu_recv_stream(&drc, fp->f_vnode, &off);

	if (error == 0) {
		if (zfsvfs != NULL) {
			char osname[MAXNAMELEN];
			int mode;

			(void) zfs_suspend_fs(zfsvfs, osname, &mode);
			error = dmu_recv_end(&drc);
			error |= zfs_resume_fs(zfsvfs, osname, mode);
		} else {
			error = dmu_recv_end(&drc);
		}
	}
	if (zfsvfs != NULL)
		VFS_RELE(zfsvfs->z_vfs);

	zc->zc_cookie = off - fp->f_offset;
	if (VOP_SEEK(fp->f_vnode, fp->f_offset, &off, NULL) == 0)
		fp->f_offset = off;

	releasef(fd);
	return (error);
}

/*
 * inputs:
 * zc_name	name of snapshot to send
 * zc_value	short name of incremental fromsnap (may be empty)
 * zc_cookie	file descriptor to send stream to
 * zc_obj	fromorigin flag (mutually exclusive with zc_value)
 *
 * outputs: none
 */
static int
zfs_ioc_send(zfs_cmd_t *zc)
{
	objset_t *fromsnap = NULL;
	objset_t *tosnap;
	file_t *fp;
	int error;
	offset_t off;

	error = dmu_objset_open(zc->zc_name, DMU_OST_ANY,
	    DS_MODE_STANDARD | DS_MODE_READONLY, &tosnap);
	if (error)
		return (error);

	if (zc->zc_value[0] != '\0') {
		char buf[MAXPATHLEN];
		char *cp;

		(void) strncpy(buf, zc->zc_name, sizeof (buf));
		cp = strchr(buf, '@');
		if (cp)
			*(cp+1) = 0;
		(void) strncat(buf, zc->zc_value, sizeof (buf));
		error = dmu_objset_open(buf, DMU_OST_ANY,
		    DS_MODE_STANDARD | DS_MODE_READONLY, &fromsnap);
		if (error) {
			dmu_objset_close(tosnap);
			return (error);
		}
	}

	fp = getf(zc->zc_cookie);
	if (fp == NULL) {
		dmu_objset_close(tosnap);
		if (fromsnap)
			dmu_objset_close(fromsnap);
		return (EBADF);
	}

	off = fp->f_offset;
	error = dmu_sendbackup(tosnap, fromsnap, zc->zc_obj, fp->f_vnode, &off);

	if (VOP_SEEK(fp->f_vnode, fp->f_offset, &off, NULL) == 0)
		fp->f_offset = off;
	releasef(zc->zc_cookie);
	if (fromsnap)
		dmu_objset_close(fromsnap);
	dmu_objset_close(tosnap);
	return (error);
}

static int
zfs_ioc_inject_fault(zfs_cmd_t *zc)
{
	int id, error;

	error = zio_inject_fault(zc->zc_name, (int)zc->zc_guid, &id,
	    &zc->zc_inject_record);

	if (error == 0)
		zc->zc_guid = (uint64_t)id;

	return (error);
}

static int
zfs_ioc_clear_fault(zfs_cmd_t *zc)
{
	return (zio_clear_fault((int)zc->zc_guid));
}

static int
zfs_ioc_inject_list_next(zfs_cmd_t *zc)
{
	int id = (int)zc->zc_guid;
	int error;

	error = zio_inject_list_next(&id, zc->zc_name, sizeof (zc->zc_name),
	    &zc->zc_inject_record);

	zc->zc_guid = id;

	return (error);
}

static int
zfs_ioc_error_log(zfs_cmd_t *zc)
{
	spa_t *spa;
	int error;
	size_t count = (size_t)zc->zc_nvlist_dst_size;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	error = spa_get_errlog(spa, (void *)(uintptr_t)zc->zc_nvlist_dst,
	    &count);
	if (error == 0)
		zc->zc_nvlist_dst_size = count;
	else
		zc->zc_nvlist_dst_size = spa_get_errlog_size(spa);

	spa_close(spa, FTAG);

	return (error);
}

static int
zfs_ioc_clear(zfs_cmd_t *zc)
{
	spa_t *spa;
	vdev_t *vd;
	uint64_t txg;
	int error;

	if ((error = spa_open(zc->zc_name, &spa, FTAG)) != 0)
		return (error);

	/*
	 * Try to resume any I/Os which may have been suspended
	 * as a result of a complete pool failure.
	 */
	if (!list_is_empty(&spa->spa_zio_list)) {
		if (zio_vdev_resume_io(spa) != 0) {
			spa_close(spa, FTAG);
			return (EIO);
		}
	}

	txg = spa_vdev_enter(spa);

	if (zc->zc_guid == 0) {
		vd = NULL;
	} else if ((vd = spa_lookup_by_guid(spa, zc->zc_guid)) == NULL) {
		(void) spa_vdev_exit(spa, NULL, txg, ENODEV);
		spa_close(spa, FTAG);
		return (ENODEV);
	}

	vdev_clear(spa, vd, B_TRUE);

	(void) spa_vdev_exit(spa, NULL, txg, 0);

	spa_close(spa, FTAG);

	return (0);
}

/*
 * inputs:
 * zc_name	name of filesystem
 * zc_value	name of origin snapshot
 *
 * outputs:	none
 */
static int
zfs_ioc_promote(zfs_cmd_t *zc)
{
	char *cp;

	/*
	 * We don't need to unmount *all* the origin fs's snapshots, but
	 * it's easier.
	 */
	cp = strchr(zc->zc_value, '@');
	if (cp)
		*cp = '\0';
	(void) dmu_objset_find(zc->zc_value,
	    zfs_unmount_snap, NULL, DS_FIND_SNAPSHOTS);
	return (dsl_dataset_promote(zc->zc_name));
}

/*
 * We don't want to have a hard dependency
 * against some special symbols in sharefs
 * nfs, and smbsrv.  Determine them if needed when
 * the first file system is shared.
 * Neither sharefs, nfs or smbsrv are unloadable modules.
 */
int (*znfsexport_fs)(void *arg);
int (*zshare_fs)(enum sharefs_sys_op, share_t *, uint32_t);
int (*zsmbexport_fs)(void *arg, boolean_t add_share);

int zfs_nfsshare_inited;
int zfs_smbshare_inited;

ddi_modhandle_t nfs_mod;
ddi_modhandle_t sharefs_mod;
ddi_modhandle_t smbsrv_mod;
kmutex_t zfs_share_lock;

static int
zfs_init_sharefs()
{
	int error;

	ASSERT(MUTEX_HELD(&zfs_share_lock));
	/* Both NFS and SMB shares also require sharetab support. */
	if (sharefs_mod == NULL && ((sharefs_mod =
	    ddi_modopen("fs/sharefs",
	    KRTLD_MODE_FIRST, &error)) == NULL)) {
		return (ENOSYS);
	}
	if (zshare_fs == NULL && ((zshare_fs =
	    (int (*)(enum sharefs_sys_op, share_t *, uint32_t))
	    ddi_modsym(sharefs_mod, "sharefs_impl", &error)) == NULL)) {
		return (ENOSYS);
	}
	return (0);
}

static int
zfs_ioc_share(zfs_cmd_t *zc)
{
	int error;
	int opcode;

	switch (zc->zc_share.z_sharetype) {
	case ZFS_SHARE_NFS:
	case ZFS_UNSHARE_NFS:
		if (zfs_nfsshare_inited == 0) {
			mutex_enter(&zfs_share_lock);
			if (nfs_mod == NULL && ((nfs_mod = ddi_modopen("fs/nfs",
			    KRTLD_MODE_FIRST, &error)) == NULL)) {
				mutex_exit(&zfs_share_lock);
				return (ENOSYS);
			}
			if (znfsexport_fs == NULL &&
			    ((znfsexport_fs = (int (*)(void *))
			    ddi_modsym(nfs_mod,
			    "nfs_export", &error)) == NULL)) {
				mutex_exit(&zfs_share_lock);
				return (ENOSYS);
			}
			error = zfs_init_sharefs();
			if (error) {
				mutex_exit(&zfs_share_lock);
				return (ENOSYS);
			}
			zfs_nfsshare_inited = 1;
			mutex_exit(&zfs_share_lock);
		}
		break;
	case ZFS_SHARE_SMB:
	case ZFS_UNSHARE_SMB:
		if (zfs_smbshare_inited == 0) {
			mutex_enter(&zfs_share_lock);
			if (smbsrv_mod == NULL && ((smbsrv_mod =
			    ddi_modopen("drv/smbsrv",
			    KRTLD_MODE_FIRST, &error)) == NULL)) {
				mutex_exit(&zfs_share_lock);
				return (ENOSYS);
			}
			if (zsmbexport_fs == NULL && ((zsmbexport_fs =
			    (int (*)(void *, boolean_t))ddi_modsym(smbsrv_mod,
			    "lmshrd_share_upcall", &error)) == NULL)) {
				mutex_exit(&zfs_share_lock);
				return (ENOSYS);
			}
			error = zfs_init_sharefs();
			if (error) {
				mutex_exit(&zfs_share_lock);
				return (ENOSYS);
			}
			zfs_smbshare_inited = 1;
			mutex_exit(&zfs_share_lock);
		}
		break;
	default:
		return (EINVAL);
	}

	switch (zc->zc_share.z_sharetype) {
	case ZFS_SHARE_NFS:
	case ZFS_UNSHARE_NFS:
		if (error =
		    znfsexport_fs((void *)
		    (uintptr_t)zc->zc_share.z_exportdata))
			return (error);
		break;
	case ZFS_SHARE_SMB:
	case ZFS_UNSHARE_SMB:
		if (error = zsmbexport_fs((void *)
		    (uintptr_t)zc->zc_share.z_exportdata,
		    zc->zc_share.z_sharetype == ZFS_SHARE_SMB ?
		    B_TRUE : B_FALSE)) {
			return (error);
		}
		break;
	}

	opcode = (zc->zc_share.z_sharetype == ZFS_SHARE_NFS ||
	    zc->zc_share.z_sharetype == ZFS_SHARE_SMB) ?
	    SHAREFS_ADD : SHAREFS_REMOVE;

	/*
	 * Add or remove share from sharetab
	 */
	error = zshare_fs(opcode,
	    (void *)(uintptr_t)zc->zc_share.z_sharedata,
	    zc->zc_share.z_sharemax);

	return (error);

}

/*
 * pool create, destroy, and export don't log the history as part of
 * zfsdev_ioctl, but rather zfs_ioc_pool_create, and zfs_ioc_pool_export
 * do the logging of those commands.
 */
static zfs_ioc_vec_t zfs_ioc_vec[] = {
	{ zfs_ioc_pool_create, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_destroy,	zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_import, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_export, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_configs,	zfs_secpolicy_none, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_stats, zfs_secpolicy_read, POOL_NAME, B_FALSE },
	{ zfs_ioc_pool_tryimport, zfs_secpolicy_config, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_scrub, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_freeze, zfs_secpolicy_config, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_upgrade,	zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_get_history, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_vdev_add, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_remove, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_set_state, zfs_secpolicy_config,	POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_attach, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_detach, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_vdev_setpath,	zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_objset_stats,	zfs_secpolicy_read, DATASET_NAME, B_FALSE },
	{ zfs_ioc_objset_version, zfs_secpolicy_read, DATASET_NAME, B_FALSE },
	{ zfs_ioc_dataset_list_next, zfs_secpolicy_read,
	    DATASET_NAME, B_FALSE },
	{ zfs_ioc_snapshot_list_next, zfs_secpolicy_read,
	    DATASET_NAME, B_FALSE },
	{ zfs_ioc_set_prop, zfs_secpolicy_none, DATASET_NAME, B_TRUE },
	{ zfs_ioc_create_minor,	zfs_secpolicy_minor, DATASET_NAME, B_FALSE },
	{ zfs_ioc_remove_minor,	zfs_secpolicy_minor, DATASET_NAME, B_FALSE },
	{ zfs_ioc_create, zfs_secpolicy_create, DATASET_NAME, B_TRUE },
	{ zfs_ioc_destroy, zfs_secpolicy_destroy, DATASET_NAME, B_TRUE },
	{ zfs_ioc_rollback, zfs_secpolicy_rollback, DATASET_NAME, B_TRUE },
	{ zfs_ioc_rename, zfs_secpolicy_rename,	DATASET_NAME, B_TRUE },
	{ zfs_ioc_recv, zfs_secpolicy_receive, DATASET_NAME, B_TRUE },
	{ zfs_ioc_send, zfs_secpolicy_send, DATASET_NAME, B_TRUE },
	{ zfs_ioc_inject_fault,	zfs_secpolicy_inject, NO_NAME, B_FALSE },
	{ zfs_ioc_clear_fault, zfs_secpolicy_inject, NO_NAME, B_FALSE },
	{ zfs_ioc_inject_list_next, zfs_secpolicy_inject, NO_NAME, B_FALSE },
	{ zfs_ioc_error_log, zfs_secpolicy_inject, POOL_NAME, B_FALSE },
	{ zfs_ioc_clear, zfs_secpolicy_config, POOL_NAME, B_TRUE },
	{ zfs_ioc_promote, zfs_secpolicy_promote, DATASET_NAME, B_TRUE },
	{ zfs_ioc_destroy_snaps, zfs_secpolicy_destroy,	DATASET_NAME, B_TRUE },
	{ zfs_ioc_snapshot, zfs_secpolicy_snapshot, DATASET_NAME, B_TRUE },
	{ zfs_ioc_dsobj_to_dsname, zfs_secpolicy_config, POOL_NAME, B_FALSE },
	{ zfs_ioc_obj_to_path, zfs_secpolicy_config, NO_NAME, B_FALSE },
	{ zfs_ioc_pool_set_props, zfs_secpolicy_config,	POOL_NAME, B_TRUE },
	{ zfs_ioc_pool_get_props, zfs_secpolicy_read, POOL_NAME, B_FALSE },
	{ zfs_ioc_set_fsacl, zfs_secpolicy_fsacl, DATASET_NAME, B_TRUE },
	{ zfs_ioc_get_fsacl, zfs_secpolicy_read, DATASET_NAME, B_FALSE },
	{ zfs_ioc_iscsi_perm_check, zfs_secpolicy_iscsi,
	    DATASET_NAME, B_FALSE },
	{ zfs_ioc_share, zfs_secpolicy_share, DATASET_NAME, B_FALSE },
	{ zfs_ioc_inherit_prop, zfs_secpolicy_inherit, DATASET_NAME, B_TRUE },
};

int
zfsdev_ioctl(dev_t dev, int cmd, intptr_t arg, int flag, cred_t *cr, int *rvalp)
{
	zfs_cmd_t *zc;
	uint_t vec;
	int error, rc;

/* zfs-fuse: not implemented */
#if 0
	if (getminor(dev) != 0)
		return (zvol_ioctl(dev, cmd, arg, flag, cr, rvalp));
#endif

	vec = cmd - ZFS_IOC;
	ASSERT3U(getmajor(dev), ==, ddi_driver_major(zfs_dip));

	if (vec >= sizeof (zfs_ioc_vec) / sizeof (zfs_ioc_vec[0]))
		return (EINVAL);

	zc = kmem_zalloc(sizeof (zfs_cmd_t), KM_SLEEP);

	error = xcopyin((void *)arg, zc, sizeof (zfs_cmd_t));

	if (error == 0)
		error = zfs_ioc_vec[vec].zvec_secpolicy(zc, cr);

	/*
	 * Ensure that all pool/dataset names are valid before we pass down to
	 * the lower layers.
	 */
	if (error == 0) {
		zc->zc_name[sizeof (zc->zc_name) - 1] = '\0';
		switch (zfs_ioc_vec[vec].zvec_namecheck) {
		case POOL_NAME:
			if (pool_namecheck(zc->zc_name, NULL, NULL) != 0)
				error = EINVAL;
			break;

		case DATASET_NAME:
			if (dataset_namecheck(zc->zc_name, NULL, NULL) != 0)
				error = EINVAL;
			break;

		case NO_NAME:
			break;
		}
	}

	if (error == 0)
		error = zfs_ioc_vec[vec].zvec_func(zc);

	rc = xcopyout(zc, (void *)arg, sizeof (zfs_cmd_t));
	if (error == 0) {
		error = rc;
		if (zfs_ioc_vec[vec].zvec_his_log == B_TRUE)
			zfs_log_history(zc);
	}

	kmem_free(zc, sizeof (zfs_cmd_t));
	return (error);
}

#if 0
static int
zfs_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
	if (cmd != DDI_ATTACH)
		return (DDI_FAILURE);

	if (ddi_create_minor_node(dip, "zfs", S_IFCHR, 0,
	    DDI_PSEUDO, 0) == DDI_FAILURE)
		return (DDI_FAILURE);

	zfs_dip = dip;

	ddi_report_dev(dip);

	return (DDI_SUCCESS);
}

static int
zfs_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
	if (spa_busy() || zfs_busy() || zvol_busy())
		return (DDI_FAILURE);

	if (cmd != DDI_DETACH)
		return (DDI_FAILURE);

	zfs_dip = NULL;

	ddi_prop_remove_all(dip);
	ddi_remove_minor_node(dip, NULL);

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
zfs_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
	switch (infocmd) {
	case DDI_INFO_DEVT2DEVINFO:
		*result = zfs_dip;
		return (DDI_SUCCESS);

	case DDI_INFO_DEVT2INSTANCE:
		*result = (void *)0;
		return (DDI_SUCCESS);
	}

	return (DDI_FAILURE);
}

/*
 * OK, so this is a little weird.
 *
 * /dev/zfs is the control node, i.e. minor 0.
 * /dev/zvol/[r]dsk/pool/dataset are the zvols, minor > 0.
 *
 * /dev/zfs has basically nothing to do except serve up ioctls,
 * so most of the standard driver entry points are in zvol.c.
 */
static struct cb_ops zfs_cb_ops = {
	zvol_open,	/* open */
	zvol_close,	/* close */
	zvol_strategy,	/* strategy */
	nodev,		/* print */
	nodev,		/* dump */
	zvol_read,	/* read */
	zvol_write,	/* write */
	zfsdev_ioctl,	/* ioctl */
	nodev,		/* devmap */
	nodev,		/* mmap */
	nodev,		/* segmap */
	nochpoll,	/* poll */
	ddi_prop_op,	/* prop_op */
	NULL,		/* streamtab */
	D_NEW | D_MP | D_64BIT,		/* Driver compatibility flag */
	CB_REV,		/* version */
	nodev,		/* async read */
	nodev,		/* async write */
};

static struct dev_ops zfs_dev_ops = {
	DEVO_REV,	/* version */
	0,		/* refcnt */
	zfs_info,	/* info */
	nulldev,	/* identify */
	nulldev,	/* probe */
	zfs_attach,	/* attach */
	zfs_detach,	/* detach */
	nodev,		/* reset */
	&zfs_cb_ops,	/* driver operations */
	NULL		/* no bus operations */
};

static struct modldrv zfs_modldrv = {
	&mod_driverops, "ZFS storage pool version " SPA_VERSION_STRING,
	    &zfs_dev_ops
};

static struct modlinkage modlinkage = {
	MODREV_1,
	(void *)&zfs_modlfs,
	(void *)&zfs_modldrv,
	NULL
};
#endif

uint_t zfs_fsyncer_key;
extern uint_t rrw_tsd_key;

int
zfs_ioctl_init(void)
{
	spa_init(FREAD | FWRITE);

	zfs_init();

	/* zfs-fuse: not implemented */
	/*zvol_init();*/

#if 0
	if ((error = mod_install(&modlinkage)) != 0) {
		zvol_fini();
		zfs_fini();
		spa_fini();
		return (error);
	}
	tsd_create(&zfs_fsyncer_key, NULL);
	tsd_create(&rrw_tsd_key, NULL);

	error = ldi_ident_from_mod(&modlinkage, &zfs_li);
	ASSERT(error == 0);
	mutex_init(&zfs_share_lock, NULL, MUTEX_DEFAULT, NULL);
#endif

	return (0);
}

int
zfs_ioctl_fini(void)
{
	int error = 0;

	if (spa_busy() || zfs_busy() || /*zvol_busy() ||*/ zio_injection_enabled)
		return (EBUSY);

#if 0
	if ((error = mod_remove(&modlinkage)) != 0)
		return (error);
#endif

	/* zfs-fuse: not implemented */
	/*zvol_fini();*/

	zfs_fini();
	spa_fini();
	if (zfs_nfsshare_inited)
		(void) ddi_modclose(nfs_mod);
	if (zfs_smbshare_inited)
		(void) ddi_modclose(smbsrv_mod);
	if (zfs_nfsshare_inited || zfs_smbshare_inited)
		(void) ddi_modclose(sharefs_mod);

	tsd_destroy(&zfs_fsyncer_key);
#if 0
	ldi_ident_release(zfs_li);
#endif
	zfs_li = NULL;
	mutex_destroy(&zfs_share_lock);

	return (error);
}

#if 0
int
_info(struct modinfo *modinfop)
{
	return (mod_info(&modlinkage, modinfop));
}
#endif
