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



#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <devid.h>
#include <fcntl.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/zfs_ioctl.h>
#include <sys/zio.h>

#include "zfs_namecheck.h"
#include "libzfs_impl.h"

/*
 * Validate the given pool name, optionally putting an extended error message in
 * 'buf'.
 */
static boolean_t
zpool_name_valid(libzfs_handle_t *hdl, boolean_t isopen, const char *pool)
{
	namecheck_err_t why;
	char what;
	int ret;

	ret = pool_namecheck(pool, &why, &what);

	/*
	 * The rules for reserved pool names were extended at a later point.
	 * But we need to support users with existing pools that may now be
	 * invalid.  So we only check for this expanded set of names during a
	 * create (or import), and only in userland.
	 */
	if (ret == 0 && !isopen &&
	    (strncmp(pool, "mirror", 6) == 0 ||
	    strncmp(pool, "raidz", 5) == 0 ||
	    strncmp(pool, "spare", 5) == 0)) {
		zfs_error_aux(hdl,
		    dgettext(TEXT_DOMAIN, "name is reserved"));
		return (B_FALSE);
	}


	if (ret != 0) {
		if (hdl != NULL) {
			switch (why) {
			case NAME_ERR_TOOLONG:
				zfs_error_aux(hdl,
				    dgettext(TEXT_DOMAIN, "name is too long"));
				break;

			case NAME_ERR_INVALCHAR:
				zfs_error_aux(hdl,
				    dgettext(TEXT_DOMAIN, "invalid character "
				    "'%c' in pool name"), what);
				break;

			case NAME_ERR_NOLETTER:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "name must begin with a letter"));
				break;

			case NAME_ERR_RESERVED:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "name is reserved"));
				break;

			case NAME_ERR_DISKLIKE:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "pool name is reserved"));
				break;

			case NAME_ERR_LEADING_SLASH:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "leading slash in name"));
				break;

			case NAME_ERR_EMPTY_COMPONENT:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "empty component in name"));
				break;

			case NAME_ERR_TRAILING_SLASH:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "trailing slash in name"));
				break;

			case NAME_ERR_MULTIPLE_AT:
				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "multiple '@' delimiters in name"));
				break;
			}
		}
		return (B_FALSE);
	}

	return (B_TRUE);
}

/*
 * Set the pool-wide health based on the vdev state of the root vdev.
 */
int
set_pool_health(nvlist_t *config)
{
	nvlist_t *nvroot;
	vdev_stat_t *vs;
	uint_t vsc;
	char *health;

	verify(nvlist_lookup_nvlist(config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) == 0);
	verify(nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_STATS,
	    (uint64_t **)&vs, &vsc) == 0);

	switch (vs->vs_state) {

	case VDEV_STATE_CLOSED:
	case VDEV_STATE_CANT_OPEN:
	case VDEV_STATE_OFFLINE:
		health = dgettext(TEXT_DOMAIN, "FAULTED");
		break;

	case VDEV_STATE_DEGRADED:
		health = dgettext(TEXT_DOMAIN, "DEGRADED");
		break;

	case VDEV_STATE_HEALTHY:
		health = dgettext(TEXT_DOMAIN, "ONLINE");
		break;

	default:
		abort();
	}

	return (nvlist_add_string(config, ZPOOL_CONFIG_POOL_HEALTH, health));
}

/*
 * Open a handle to the given pool, even if the pool is currently in the FAULTED
 * state.
 */
zpool_handle_t *
zpool_open_canfail(libzfs_handle_t *hdl, const char *pool)
{
	zpool_handle_t *zhp;
	boolean_t missing;

	/*
	 * Make sure the pool name is valid.
	 */
	if (!zpool_name_valid(hdl, B_TRUE, pool)) {
		(void) zfs_error(hdl, EZFS_INVALIDNAME,
		    dgettext(TEXT_DOMAIN, "cannot open '%s'"),
		    pool);
		return (NULL);
	}

	if ((zhp = zfs_alloc(hdl, sizeof (zpool_handle_t))) == NULL)
		return (NULL);

	zhp->zpool_hdl = hdl;
	(void) strlcpy(zhp->zpool_name, pool, sizeof (zhp->zpool_name));

	if (zpool_refresh_stats(zhp, &missing) != 0) {
		zpool_close(zhp);
		return (NULL);
	}

	if (missing) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "no such pool"));
		(void) zfs_error(hdl, EZFS_NOENT,
		    dgettext(TEXT_DOMAIN, "cannot open '%s'"),
		    pool);
		zpool_close(zhp);
		return (NULL);
	}

	return (zhp);
}

/*
 * Like the above, but silent on error.  Used when iterating over pools (because
 * the configuration cache may be out of date).
 */
int
zpool_open_silent(libzfs_handle_t *hdl, const char *pool, zpool_handle_t **ret)
{
	zpool_handle_t *zhp;
	boolean_t missing;

	if ((zhp = zfs_alloc(hdl, sizeof (zpool_handle_t))) == NULL)
		return (-1);

	zhp->zpool_hdl = hdl;
	(void) strlcpy(zhp->zpool_name, pool, sizeof (zhp->zpool_name));

	if (zpool_refresh_stats(zhp, &missing) != 0) {
		zpool_close(zhp);
		return (-1);
	}

	if (missing) {
		zpool_close(zhp);
		*ret = NULL;
		return (0);
	}

	*ret = zhp;
	return (0);
}

/*
 * Similar to zpool_open_canfail(), but refuses to open pools in the faulted
 * state.
 */
zpool_handle_t *
zpool_open(libzfs_handle_t *hdl, const char *pool)
{
	zpool_handle_t *zhp;

	if ((zhp = zpool_open_canfail(hdl, pool)) == NULL)
		return (NULL);

	if (zhp->zpool_state == POOL_STATE_UNAVAIL) {
		(void) zfs_error(hdl, EZFS_POOLUNAVAIL,
		    dgettext(TEXT_DOMAIN, "cannot open '%s'"), zhp->zpool_name);
		zpool_close(zhp);
		return (NULL);
	}

	return (zhp);
}

/*
 * Close the handle.  Simply frees the memory associated with the handle.
 */
void
zpool_close(zpool_handle_t *zhp)
{
	if (zhp->zpool_config)
		nvlist_free(zhp->zpool_config);
	if (zhp->zpool_old_config)
		nvlist_free(zhp->zpool_old_config);
	if (zhp->zpool_error_log) {
		int i;
		for (i = 0; i < zhp->zpool_error_count; i++)
			nvlist_free(zhp->zpool_error_log[i]);
		free(zhp->zpool_error_log);
	}
	free(zhp);
}

/*
 * Return the name of the pool.
 */
const char *
zpool_get_name(zpool_handle_t *zhp)
{
	return (zhp->zpool_name);
}

/*
 * Return the GUID of the pool.
 */
uint64_t
zpool_get_guid(zpool_handle_t *zhp)
{
	uint64_t guid;

	verify(nvlist_lookup_uint64(zhp->zpool_config, ZPOOL_CONFIG_POOL_GUID,
	    &guid) == 0);
	return (guid);
}

/*
 * Return the version of the pool.
 */
uint64_t
zpool_get_version(zpool_handle_t *zhp)
{
	uint64_t version;

	verify(nvlist_lookup_uint64(zhp->zpool_config, ZPOOL_CONFIG_VERSION,
	    &version) == 0);

	return (version);
}

/*
 * Return the amount of space currently consumed by the pool.
 */
uint64_t
zpool_get_space_used(zpool_handle_t *zhp)
{
	nvlist_t *nvroot;
	vdev_stat_t *vs;
	uint_t vsc;

	verify(nvlist_lookup_nvlist(zhp->zpool_config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) == 0);
	verify(nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_STATS,
	    (uint64_t **)&vs, &vsc) == 0);

	return (vs->vs_alloc);
}

/*
 * Return the total space in the pool.
 */
uint64_t
zpool_get_space_total(zpool_handle_t *zhp)
{
	nvlist_t *nvroot;
	vdev_stat_t *vs;
	uint_t vsc;

	verify(nvlist_lookup_nvlist(zhp->zpool_config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) == 0);
	verify(nvlist_lookup_uint64_array(nvroot, ZPOOL_CONFIG_STATS,
	    (uint64_t **)&vs, &vsc) == 0);

	return (vs->vs_space);
}

/*
 * Return the alternate root for this pool, if any.
 */
int
zpool_get_root(zpool_handle_t *zhp, char *buf, size_t buflen)
{
	zfs_cmd_t zc = { 0 };

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_OBJSET_STATS, &zc) != 0 ||
	    zc.zc_root[0] == '\0')
		return (-1);

	(void) strlcpy(buf, zc.zc_root, buflen);

	return (0);
}

/*
 * Return the state of the pool (ACTIVE or UNAVAILABLE)
 */
int
zpool_get_state(zpool_handle_t *zhp)
{
	return (zhp->zpool_state);
}

/*
 * Create the named pool, using the provided vdev list.  It is assumed
 * that the consumer has already validated the contents of the nvlist, so we
 * don't have to worry about error semantics.
 */
int
zpool_create(libzfs_handle_t *hdl, const char *pool, nvlist_t *nvroot,
    const char *altroot)
{
	zfs_cmd_t zc = { 0 };
	char *packed;
	size_t len;
	char msg[1024];

	(void) snprintf(msg, sizeof (msg), dgettext(TEXT_DOMAIN,
	    "cannot create '%s'"), pool);

	if (!zpool_name_valid(hdl, B_FALSE, pool))
		return (zfs_error(hdl, EZFS_INVALIDNAME, msg));

	if (altroot != NULL && altroot[0] != '/')
		return (zfs_error(hdl, EZFS_BADPATH,
		    dgettext(TEXT_DOMAIN, "bad alternate root '%s'"), altroot));

	if (nvlist_size(nvroot, &len, NV_ENCODE_NATIVE) != 0)
		return (no_memory(hdl));

	if ((packed = zfs_alloc(hdl, len)) == NULL)
		return (-1);

	if (nvlist_pack(nvroot, &packed, &len,
	    NV_ENCODE_NATIVE, 0) != 0) {
		free(packed);
		return (no_memory(hdl));
	}

	(void) strlcpy(zc.zc_name, pool, sizeof (zc.zc_name));
	zc.zc_config_src = (uint64_t)(uintptr_t)packed;
	zc.zc_config_src_size = len;

	if (altroot != NULL)
		(void) strlcpy(zc.zc_root, altroot, sizeof (zc.zc_root));

	if (ioctl(hdl->libzfs_fd, ZFS_IOC_POOL_CREATE, &zc) != 0) {
		free(packed);

		switch (errno) {
		case EBUSY:
			/*
			 * This can happen if the user has specified the same
			 * device multiple times.  We can't reliably detect this
			 * until we try to add it and see we already have a
			 * label.
			 */
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "one or more vdevs refer to the same device"));
			return (zfs_error(hdl, EZFS_BADDEV, msg));

		case EOVERFLOW:
			/*
			 * This occurs when one of the devices is below
			 * SPA_MINDEVSIZE.  Unfortunately, we can't detect which
			 * device was the problem device since there's no
			 * reliable way to determine device size from userland.
			 */
			{
				char buf[64];

				zfs_nicenum(SPA_MINDEVSIZE, buf, sizeof (buf));

				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "one or more devices is less than the "
				    "minimum size (%s)"), buf);
			}
			return (zfs_error(hdl, EZFS_BADDEV, msg));

		case ENOSPC:
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "one or more devices is out of space"));
			return (zfs_error(hdl, EZFS_BADDEV, msg));

		default:
			return (zpool_standard_error(hdl, errno, msg));
		}
	}

	free(packed);

	/*
	 * If this is an alternate root pool, then we automatically set the
	 * moutnpoint of the root dataset to be '/'.
	 */
	if (altroot != NULL) {
		zfs_handle_t *zhp;

		verify((zhp = zfs_open(hdl, pool, ZFS_TYPE_ANY)) != NULL);
		verify(zfs_prop_set(zhp, ZFS_PROP_MOUNTPOINT, "/") == 0);

		zfs_close(zhp);
	}

	return (0);
}

/*
 * Destroy the given pool.  It is up to the caller to ensure that there are no
 * datasets left in the pool.
 */
int
zpool_destroy(zpool_handle_t *zhp)
{
	zfs_cmd_t zc = { 0 };
	zfs_handle_t *zfp = NULL;
	libzfs_handle_t *hdl = zhp->zpool_hdl;
	char msg[1024];

	if (zhp->zpool_state == POOL_STATE_ACTIVE &&
	    (zfp = zfs_open(zhp->zpool_hdl, zhp->zpool_name,
	    ZFS_TYPE_FILESYSTEM)) == NULL)
		return (-1);

	if (zpool_remove_zvol_links(zhp) != 0)
		return (-1);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));

	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_POOL_DESTROY, &zc) != 0) {
		(void) snprintf(msg, sizeof (msg), dgettext(TEXT_DOMAIN,
		    "cannot destroy '%s'"), zhp->zpool_name);

		if (errno == EROFS) {
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "one or more devices is read only"));
			(void) zfs_error(hdl, EZFS_BADDEV, msg);
		} else {
			(void) zpool_standard_error(hdl, errno, msg);
		}

		if (zfp)
			zfs_close(zfp);
		return (-1);
	}

	if (zfp) {
		remove_mountpoint(zfp);
		zfs_close(zfp);
	}

	return (0);
}

/*
 * Add the given vdevs to the pool.  The caller must have already performed the
 * necessary verification to ensure that the vdev specification is well-formed.
 */
int
zpool_add(zpool_handle_t *zhp, nvlist_t *nvroot)
{
	char *packed;
	size_t len;
	zfs_cmd_t zc;
	int ret;
	libzfs_handle_t *hdl = zhp->zpool_hdl;
	char msg[1024];
	nvlist_t **spares;
	uint_t nspares;

	(void) snprintf(msg, sizeof (msg), dgettext(TEXT_DOMAIN,
	    "cannot add to '%s'"), zhp->zpool_name);

	if (zpool_get_version(zhp) < ZFS_VERSION_SPARES &&
	    nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_SPARES,
	    &spares, &nspares) == 0) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "pool must be "
		    "upgraded to add hot spares"));
		return (zfs_error(hdl, EZFS_BADVERSION, msg));
	}

	verify(nvlist_size(nvroot, &len, NV_ENCODE_NATIVE) == 0);

	if ((packed = zfs_alloc(zhp->zpool_hdl, len)) == NULL)
		return (-1);

	verify(nvlist_pack(nvroot, &packed, &len, NV_ENCODE_NATIVE, 0) == 0);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	zc.zc_config_src = (uint64_t)(uintptr_t)packed;
	zc.zc_config_src_size = len;

	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_VDEV_ADD, &zc) != 0) {
		switch (errno) {
		case EBUSY:
			/*
			 * This can happen if the user has specified the same
			 * device multiple times.  We can't reliably detect this
			 * until we try to add it and see we already have a
			 * label.
			 */
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "one or more vdevs refer to the same device"));
			(void) zfs_error(hdl, EZFS_BADDEV, msg);
			break;

		case EOVERFLOW:
			/*
			 * This occurrs when one of the devices is below
			 * SPA_MINDEVSIZE.  Unfortunately, we can't detect which
			 * device was the problem device since there's no
			 * reliable way to determine device size from userland.
			 */
			{
				char buf[64];

				zfs_nicenum(SPA_MINDEVSIZE, buf, sizeof (buf));

				zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
				    "device is less than the minimum "
				    "size (%s)"), buf);
			}
			(void) zfs_error(hdl, EZFS_BADDEV, msg);
			break;

		case ENOTSUP:
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "pool must be upgraded to add raidz2 vdevs"));
			(void) zfs_error(hdl, EZFS_BADVERSION, msg);
			break;

		default:
			(void) zpool_standard_error(hdl, errno, msg);
		}

		ret = -1;
	} else {
		ret = 0;
	}

	free(packed);

	return (ret);
}

/*
 * Exports the pool from the system.  The caller must ensure that there are no
 * mounted datasets in the pool.
 */
int
zpool_export(zpool_handle_t *zhp)
{
	zfs_cmd_t zc = { 0 };

	if (zpool_remove_zvol_links(zhp) != 0)
		return (-1);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));

	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_POOL_EXPORT, &zc) != 0)
		return (zpool_standard_error(zhp->zpool_hdl, errno,
		    dgettext(TEXT_DOMAIN, "cannot export '%s'"),
		    zhp->zpool_name));

	return (0);
}

/*
 * Import the given pool using the known configuration.  The configuration
 * should have come from zpool_find_import().  The 'newname' and 'altroot'
 * parameters control whether the pool is imported with a different name or with
 * an alternate root, respectively.
 */
int
zpool_import(libzfs_handle_t *hdl, nvlist_t *config, const char *newname,
    const char *altroot)
{
	zfs_cmd_t zc;
	char *packed;
	size_t len;
	char *thename;
	char *origname;
	int ret;

	verify(nvlist_lookup_string(config, ZPOOL_CONFIG_POOL_NAME,
	    &origname) == 0);

	if (newname != NULL) {
		if (!zpool_name_valid(hdl, B_FALSE, newname))
			return (zfs_error(hdl, EZFS_INVALIDNAME,
			    dgettext(TEXT_DOMAIN, "cannot import '%s'"),
			    newname));
		thename = (char *)newname;
	} else {
		thename = origname;
	}

	if (altroot != NULL && altroot[0] != '/')
		return (zfs_error(hdl, EZFS_BADPATH,
		    dgettext(TEXT_DOMAIN, "bad alternate root '%s'"),
		    altroot));

	(void) strlcpy(zc.zc_name, thename, sizeof (zc.zc_name));

	if (altroot != NULL)
		(void) strlcpy(zc.zc_root, altroot, sizeof (zc.zc_root));
	else
		zc.zc_root[0] = '\0';

	verify(nvlist_lookup_uint64(config, ZPOOL_CONFIG_POOL_GUID,
	    &zc.zc_guid) == 0);

	verify(nvlist_size(config, &len, NV_ENCODE_NATIVE) == 0);

	if ((packed = zfs_alloc(hdl, len)) == NULL)
		return (-1);

	verify(nvlist_pack(config, &packed, &len, NV_ENCODE_NATIVE, 0) == 0);

	zc.zc_config_src = (uint64_t)(uintptr_t)packed;
	zc.zc_config_src_size = len;

	ret = 0;
	if (ioctl(hdl->libzfs_fd, ZFS_IOC_POOL_IMPORT, &zc) != 0) {
		char desc[1024];
		if (newname == NULL)
			(void) snprintf(desc, sizeof (desc),
			    dgettext(TEXT_DOMAIN, "cannot import '%s'"),
			    thename);
		else
			(void) snprintf(desc, sizeof (desc),
			    dgettext(TEXT_DOMAIN, "cannot import '%s' as '%s'"),
			    origname, thename);

		switch (errno) {
		case ENOTSUP:
			/*
			 * Unsupported version.
			 */
			(void) zfs_error(hdl, EZFS_BADVERSION, desc);
			break;

		case EINVAL:
			(void) zfs_error(hdl, EZFS_INVALCONFIG, desc);
			break;

		default:
			(void) zpool_standard_error(hdl, errno, desc);
		}

		ret = -1;
	} else {
		zpool_handle_t *zhp;
		/*
		 * This should never fail, but play it safe anyway.
		 */
		if (zpool_open_silent(hdl, thename, &zhp) != 0) {
			ret = -1;
		} else if (zhp != NULL) {
			ret = zpool_create_zvol_links(zhp);
			zpool_close(zhp);
		}
	}

	free(packed);
	return (ret);
}

/*
 * Scrub the pool.
 */
int
zpool_scrub(zpool_handle_t *zhp, pool_scrub_type_t type)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	zc.zc_cookie = type;

	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_POOL_SCRUB, &zc) == 0)
		return (0);

	(void) snprintf(msg, sizeof (msg),
	    dgettext(TEXT_DOMAIN, "cannot scrub %s"), zc.zc_name);

	if (errno == EBUSY)
		return (zfs_error(hdl, EZFS_RESILVERING, msg));
	else
		return (zpool_standard_error(hdl, errno, msg));
}

/*
 * 'avail_spare' is set to TRUE if the provided guid refers to an AVAIL
 * spare; but FALSE if its an INUSE spare.
 */
static nvlist_t *
vdev_to_nvlist_iter(nvlist_t *nv, const char *search, uint64_t guid,
    boolean_t *avail_spare)
{
	uint_t c, children;
	nvlist_t **child;
	uint64_t theguid, present;
	char *path;
	uint64_t wholedisk = 0;
	nvlist_t *ret;

	verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID, &theguid) == 0);

	if (search == NULL &&
	    nvlist_lookup_uint64(nv, ZPOOL_CONFIG_NOT_PRESENT, &present) == 0) {
		/*
		 * If the device has never been present since import, the only
		 * reliable way to match the vdev is by GUID.
		 */
		if (theguid == guid)
			return (nv);
	} else if (search != NULL &&
	    nvlist_lookup_string(nv, ZPOOL_CONFIG_PATH, &path) == 0) {
		(void) nvlist_lookup_uint64(nv, ZPOOL_CONFIG_WHOLE_DISK,
		    &wholedisk);
		if (wholedisk) {
			/*
			 * For whole disks, the internal path has 's0', but the
			 * path passed in by the user doesn't.
			 */
			if (strlen(search) == strlen(path) - 2 &&
			    strncmp(search, path, strlen(search)) == 0)
				return (nv);
		} else if (strcmp(search, path) == 0) {
			return (nv);
		}
	}

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) != 0)
		return (NULL);

	for (c = 0; c < children; c++)
		if ((ret = vdev_to_nvlist_iter(child[c], search, guid,
		    avail_spare)) != NULL)
			return (ret);

	if (nvlist_lookup_nvlist_array(nv, ZPOOL_CONFIG_SPARES,
	    &child, &children) == 0) {
		for (c = 0; c < children; c++) {
			if ((ret = vdev_to_nvlist_iter(child[c], search, guid,
			    avail_spare)) != NULL) {
				*avail_spare = B_TRUE;
				return (ret);
			}
		}
	}

	return (NULL);
}

nvlist_t *
zpool_find_vdev(zpool_handle_t *zhp, const char *path, boolean_t *avail_spare)
{
	char buf[MAXPATHLEN];
	const char *search;
	char *end;
	nvlist_t *nvroot;
	uint64_t guid;

	guid = strtoull(path, &end, 10);
	if (guid != 0 && *end == '\0') {
		search = NULL;
	} else if (path[0] != '/') {
		(void) snprintf(buf, sizeof (buf), "%s%s", "/dev/dsk/", path);
		search = buf;
	} else {
		search = path;
	}

	verify(nvlist_lookup_nvlist(zhp->zpool_config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) == 0);

	*avail_spare = B_FALSE;
	return (vdev_to_nvlist_iter(nvroot, search, guid, avail_spare));
}

/*
 * Returns TRUE if the given guid corresponds to a spare (INUSE or not).
 */
static boolean_t
is_spare(zpool_handle_t *zhp, uint64_t guid)
{
	uint64_t spare_guid;
	nvlist_t *nvroot;
	nvlist_t **spares;
	uint_t nspares;
	int i;

	verify(nvlist_lookup_nvlist(zhp->zpool_config, ZPOOL_CONFIG_VDEV_TREE,
	    &nvroot) == 0);
	if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_SPARES,
	    &spares, &nspares) == 0) {
		for (i = 0; i < nspares; i++) {
			verify(nvlist_lookup_uint64(spares[i],
			    ZPOOL_CONFIG_GUID, &spare_guid) == 0);
			if (guid == spare_guid)
				return (B_TRUE);
		}
	}

	return (B_FALSE);
}

/*
 * Bring the specified vdev online
 */
int
zpool_vdev_online(zpool_handle_t *zhp, const char *path)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	nvlist_t *tgt;
	boolean_t avail_spare;
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	(void) snprintf(msg, sizeof (msg),
	    dgettext(TEXT_DOMAIN, "cannot online %s"), path);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if ((tgt = zpool_find_vdev(zhp, path, &avail_spare)) == NULL)
		return (zfs_error(hdl, EZFS_NODEVICE, msg));

	verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID, &zc.zc_guid) == 0);

	if (avail_spare || is_spare(zhp, zc.zc_guid) == B_TRUE)
		return (zfs_error(hdl, EZFS_ISSPARE, msg));

	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_VDEV_ONLINE, &zc) == 0)
		return (0);

	return (zpool_standard_error(hdl, errno, msg));
}

/*
 * Take the specified vdev offline
 */
int
zpool_vdev_offline(zpool_handle_t *zhp, const char *path, int istmp)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	nvlist_t *tgt;
	boolean_t avail_spare;
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	(void) snprintf(msg, sizeof (msg),
	    dgettext(TEXT_DOMAIN, "cannot offline %s"), path);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if ((tgt = zpool_find_vdev(zhp, path, &avail_spare)) == NULL)
		return (zfs_error(hdl, EZFS_NODEVICE, msg));

	verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID, &zc.zc_guid) == 0);

	if (avail_spare || is_spare(zhp, zc.zc_guid) == B_TRUE)
		return (zfs_error(hdl, EZFS_ISSPARE, msg));

	zc.zc_cookie = istmp;

	if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_VDEV_OFFLINE, &zc) == 0)
		return (0);

	switch (errno) {
	case EBUSY:

		/*
		 * There are no other replicas of this device.
		 */
		return (zfs_error(hdl, EZFS_NOREPLICAS, msg));

	default:
		return (zpool_standard_error(hdl, errno, msg));
	}
}

/*
 * Returns TRUE if the given nvlist is a vdev that was originally swapped in as
 * a hot spare.
 */
static boolean_t
is_replacing_spare(nvlist_t *search, nvlist_t *tgt, int which)
{
	nvlist_t **child;
	uint_t c, children;
	char *type;

	if (nvlist_lookup_nvlist_array(search, ZPOOL_CONFIG_CHILDREN, &child,
	    &children) == 0) {
		verify(nvlist_lookup_string(search, ZPOOL_CONFIG_TYPE,
		    &type) == 0);

		if (strcmp(type, VDEV_TYPE_SPARE) == 0 &&
		    children == 2 && child[which] == tgt)
			return (B_TRUE);

		for (c = 0; c < children; c++)
			if (is_replacing_spare(child[c], tgt, which))
				return (B_TRUE);
	}

	return (B_FALSE);
}

/*
 * Attach new_disk (fully described by nvroot) to old_disk.
 * If 'replacing' is specified, tne new disk will replace the old one.
 */
int
zpool_vdev_attach(zpool_handle_t *zhp,
    const char *old_disk, const char *new_disk, nvlist_t *nvroot, int replacing)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	char *packed;
	int ret;
	size_t len;
	nvlist_t *tgt;
	boolean_t avail_spare;
	uint64_t val;
	char *path;
	nvlist_t **child;
	uint_t children;
	nvlist_t *config_root;
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	if (replacing)
		(void) snprintf(msg, sizeof (msg), dgettext(TEXT_DOMAIN,
		    "cannot replace %s with %s"), old_disk, new_disk);
	else
		(void) snprintf(msg, sizeof (msg), dgettext(TEXT_DOMAIN,
		    "cannot attach %s to %s"), new_disk, old_disk);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if ((tgt = zpool_find_vdev(zhp, old_disk, &avail_spare)) == 0)
		return (zfs_error(hdl, EZFS_NODEVICE, msg));

	if (avail_spare)
		return (zfs_error(hdl, EZFS_ISSPARE, msg));

	verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID, &zc.zc_guid) == 0);
	zc.zc_cookie = replacing;

	if (nvlist_lookup_nvlist_array(nvroot, ZPOOL_CONFIG_CHILDREN,
	    &child, &children) != 0 || children != 1) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "new device must be a single disk"));
		return (zfs_error(hdl, EZFS_INVALCONFIG, msg));
	}

	verify(nvlist_lookup_nvlist(zpool_get_config(zhp, NULL),
	    ZPOOL_CONFIG_VDEV_TREE, &config_root) == 0);

	/*
	 * If the target is a hot spare that has been swapped in, we can only
	 * replace it with another hot spare.
	 */
	if (replacing &&
	    nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_IS_SPARE, &val) == 0 &&
	    nvlist_lookup_string(child[0], ZPOOL_CONFIG_PATH, &path) == 0 &&
	    (zpool_find_vdev(zhp, path, &avail_spare) == NULL ||
	    !avail_spare) && is_replacing_spare(config_root, tgt, 1)) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "can only be replaced by another hot spare"));
		return (zfs_error(hdl, EZFS_BADTARGET, msg));
	}

	/*
	 * If we are attempting to replace a spare, it canot be applied to an
	 * already spared device.
	 */
	if (replacing &&
	    nvlist_lookup_string(child[0], ZPOOL_CONFIG_PATH, &path) == 0 &&
	    zpool_find_vdev(zhp, path, &avail_spare) != NULL && avail_spare &&
	    is_replacing_spare(config_root, tgt, 0)) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "device has already been replaced with a spare"));
		return (zfs_error(hdl, EZFS_BADTARGET, msg));
	}

	verify(nvlist_size(nvroot, &len, NV_ENCODE_NATIVE) == 0);

	if ((packed = zfs_alloc(zhp->zpool_hdl, len)) == NULL)
		return (-1);

	verify(nvlist_pack(nvroot, &packed, &len, NV_ENCODE_NATIVE, 0) == 0);

	zc.zc_config_src = (uint64_t)(uintptr_t)packed;
	zc.zc_config_src_size = len;

	ret = ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_VDEV_ATTACH, &zc);

	free(packed);

	if (ret == 0)
		return (0);

	switch (errno) {
	case ENOTSUP:
		/*
		 * Can't attach to or replace this type of vdev.
		 */
		if (replacing)
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "cannot replace a replacing device"));
		else
			zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
			    "can only attach to mirrors and top-level "
			    "disks"));
		(void) zfs_error(hdl, EZFS_BADTARGET, msg);
		break;

	case EINVAL:
		/*
		 * The new device must be a single disk.
		 */
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "new device must be a single disk"));
		(void) zfs_error(hdl, EZFS_INVALCONFIG, msg);
		break;

	case EBUSY:
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "%s is busy"),
		    new_disk);
		(void) zfs_error(hdl, EZFS_BADDEV, msg);
		break;

	case EOVERFLOW:
		/*
		 * The new device is too small.
		 */
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "device is too small"));
		(void) zfs_error(hdl, EZFS_BADDEV, msg);
		break;

	case EDOM:
		/*
		 * The new device has a different alignment requirement.
		 */
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "devices have different sector alignment"));
		(void) zfs_error(hdl, EZFS_BADDEV, msg);
		break;

	case ENAMETOOLONG:
		/*
		 * The resulting top-level vdev spec won't fit in the label.
		 */
		(void) zfs_error(hdl, EZFS_DEVOVERFLOW, msg);
		break;

	default:
		(void) zpool_standard_error(hdl, errno, msg);
	}

	return (-1);
}

/*
 * Detach the specified device.
 */
int
zpool_vdev_detach(zpool_handle_t *zhp, const char *path)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	nvlist_t *tgt;
	boolean_t avail_spare;
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	(void) snprintf(msg, sizeof (msg),
	    dgettext(TEXT_DOMAIN, "cannot detach %s"), path);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if ((tgt = zpool_find_vdev(zhp, path, &avail_spare)) == 0)
		return (zfs_error(hdl, EZFS_NODEVICE, msg));

	if (avail_spare)
		return (zfs_error(hdl, EZFS_ISSPARE, msg));

	verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID, &zc.zc_guid) == 0);

	if (ioctl(hdl->libzfs_fd, ZFS_IOC_VDEV_DETACH, &zc) == 0)
		return (0);

	switch (errno) {

	case ENOTSUP:
		/*
		 * Can't detach from this type of vdev.
		 */
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN, "only "
		    "applicable to mirror and replacing vdevs"));
		(void) zfs_error(zhp->zpool_hdl, EZFS_BADTARGET, msg);
		break;

	case EBUSY:
		/*
		 * There are no other replicas of this device.
		 */
		(void) zfs_error(hdl, EZFS_NOREPLICAS, msg);
		break;

	default:
		(void) zpool_standard_error(hdl, errno, msg);
	}

	return (-1);
}

/*
 * Remove the given device.  Currently, this is supported only for hot spares.
 */
int
zpool_vdev_remove(zpool_handle_t *zhp, const char *path)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	nvlist_t *tgt;
	boolean_t avail_spare;
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	(void) snprintf(msg, sizeof (msg),
	    dgettext(TEXT_DOMAIN, "cannot remove %s"), path);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if ((tgt = zpool_find_vdev(zhp, path, &avail_spare)) == 0)
		return (zfs_error(hdl, EZFS_NODEVICE, msg));

	if (!avail_spare) {
		zfs_error_aux(hdl, dgettext(TEXT_DOMAIN,
		    "only hot spares can be removed"));
		return (zfs_error(hdl, EZFS_NODEVICE, msg));
	}

	verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID, &zc.zc_guid) == 0);

	if (ioctl(hdl->libzfs_fd, ZFS_IOC_VDEV_REMOVE, &zc) == 0)
		return (0);

	return (zpool_standard_error(hdl, errno, msg));
}

/*
 * Clear the errors for the pool, or the particular device if specified.
 */
int
zpool_clear(zpool_handle_t *zhp, const char *path)
{
	zfs_cmd_t zc = { 0 };
	char msg[1024];
	nvlist_t *tgt;
	boolean_t avail_spare;
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	if (path)
		(void) snprintf(msg, sizeof (msg),
		    dgettext(TEXT_DOMAIN, "cannot clear errors for %s"),
		    zc.zc_prop_value);
	else
		(void) snprintf(msg, sizeof (msg),
		    dgettext(TEXT_DOMAIN, "cannot clear errors for %s"),
		    zhp->zpool_name);

	(void) strlcpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	if (path) {
		if ((tgt = zpool_find_vdev(zhp, path, &avail_spare)) == 0)
			return (zfs_error(hdl, EZFS_NODEVICE, msg));

		if (avail_spare)
			return (zfs_error(hdl, EZFS_ISSPARE, msg));

		verify(nvlist_lookup_uint64(tgt, ZPOOL_CONFIG_GUID,
		    &zc.zc_guid) == 0);
	}

	if (ioctl(hdl->libzfs_fd, ZFS_IOC_CLEAR, &zc) == 0)
		return (0);

	return (zpool_standard_error(hdl, errno, msg));
}

static int
do_zvol(zfs_handle_t *zhp, void *data)
{
	int linktype = (int)(uintptr_t)data;
	int ret;

	/*
	 * We check for volblocksize intead of ZFS_TYPE_VOLUME so that we
	 * correctly handle snapshots of volumes.
	 */
	if (zhp->zfs_volblocksize != 0) {
		if (linktype)
			ret = zvol_create_link(zhp->zfs_hdl, zhp->zfs_name);
		else
			ret = zvol_remove_link(zhp->zfs_hdl, zhp->zfs_name);
	}

	ret = zfs_iter_children(zhp, do_zvol, data);

	zfs_close(zhp);
	return (ret);
}

/*
 * Iterate over all zvols in the pool and make any necessary minor nodes.
 */
int
zpool_create_zvol_links(zpool_handle_t *zhp)
{
	zfs_handle_t *zfp;
	int ret;

	/*
	 * If the pool is unavailable, just return success.
	 */
	if ((zfp = make_dataset_handle(zhp->zpool_hdl,
	    zhp->zpool_name)) == NULL)
		return (0);

	ret = zfs_iter_children(zfp, do_zvol, (void *)B_TRUE);

	zfs_close(zfp);
	return (ret);
}

/*
 * Iterate over all zvols in the poool and remove any minor nodes.
 */
int
zpool_remove_zvol_links(zpool_handle_t *zhp)
{
	zfs_handle_t *zfp;
	int ret;

	/*
	 * If the pool is unavailable, just return success.
	 */
	if ((zfp = make_dataset_handle(zhp->zpool_hdl,
	    zhp->zpool_name)) == NULL)
		return (0);

	ret = zfs_iter_children(zfp, do_zvol, (void *)B_FALSE);

	zfs_close(zfp);
	return (ret);
}

/*
 * Convert from a devid string to a path.
 */
static char *
devid_to_path(char *devid_str)
{
	ddi_devid_t devid;
	char *minor;
	char *path;
	devid_nmlist_t *list = NULL;
	int ret;

	if (devid_str_decode(devid_str, &devid, &minor) != 0)
		return (NULL);

	ret = devid_deviceid_to_nmlist("/dev", devid, minor, &list);

	devid_str_free(minor);
	devid_free(devid);

	if (ret != 0)
		return (NULL);

	if ((path = strdup(list[0].devname)) == NULL)
		return (NULL);

	devid_free_nmlist(list);

	return (path);
}

/*
 * Convert from a path to a devid string.
 */
static char *
path_to_devid(const char *path)
{
	int fd;
	ddi_devid_t devid;
	char *minor, *ret;

	if ((fd = open(path, O_RDONLY)) < 0)
		return (NULL);

	minor = NULL;
	ret = NULL;
	if (devid_get(fd, &devid) == 0) {
		if (devid_get_minor_name(fd, &minor) == 0)
			ret = devid_str_encode(devid, minor);
		if (minor != NULL)
			devid_str_free(minor);
		devid_free(devid);
	}
	(void) close(fd);

	return (ret);
}

/*
 * Issue the necessary ioctl() to update the stored path value for the vdev.  We
 * ignore any failure here, since a common case is for an unprivileged user to
 * type 'zpool status', and we'll display the correct information anyway.
 */
static void
set_path(zpool_handle_t *zhp, nvlist_t *nv, const char *path)
{
	zfs_cmd_t zc = { 0 };

	(void) strncpy(zc.zc_name, zhp->zpool_name, sizeof (zc.zc_name));
	(void) strncpy(zc.zc_prop_value, path, sizeof (zc.zc_prop_value));
	verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID,
	    &zc.zc_guid) == 0);

	(void) ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_VDEV_SETPATH, &zc);
}

/*
 * Given a vdev, return the name to display in iostat.  If the vdev has a path,
 * we use that, stripping off any leading "/dev/dsk/"; if not, we use the type.
 * We also check if this is a whole disk, in which case we strip off the
 * trailing 's0' slice name.
 *
 * This routine is also responsible for identifying when disks have been
 * reconfigured in a new location.  The kernel will have opened the device by
 * devid, but the path will still refer to the old location.  To catch this, we
 * first do a path -> devid translation (which is fast for the common case).  If
 * the devid matches, we're done.  If not, we do a reverse devid -> path
 * translation and issue the appropriate ioctl() to update the path of the vdev.
 * If 'zhp' is NULL, then this is an exported pool, and we don't need to do any
 * of these checks.
 */
/*
 * zfs-fuse FIXME: Handle this properly
 */
char *
zpool_vdev_name(libzfs_handle_t *hdl, zpool_handle_t *zhp, nvlist_t *nv)
{
	char *path, *devid;
	uint64_t value;
	char buf[64];

	if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_NOT_PRESENT,
	    &value) == 0) {
		verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_GUID,
		    &value) == 0);
		(void) snprintf(buf, sizeof (buf), "%llu", (u_longlong_t) value);
		path = buf;
	} else if (nvlist_lookup_string(nv, ZPOOL_CONFIG_PATH, &path) == 0) {

		if (zhp != NULL &&
		    nvlist_lookup_string(nv, ZPOOL_CONFIG_DEVID, &devid) == 0) {
			/*
			 * Determine if the current path is correct.
			 */
			char *newdevid = path_to_devid(path);

			if (newdevid == NULL ||
			    strcmp(devid, newdevid) != 0) {
				char *newpath;

				if ((newpath = devid_to_path(devid)) != NULL) {
					/*
					 * Update the path appropriately.
					 */
					set_path(zhp, nv, newpath);
					if (nvlist_add_string(nv,
					    ZPOOL_CONFIG_PATH, newpath) == 0)
						verify(nvlist_lookup_string(nv,
						    ZPOOL_CONFIG_PATH,
						    &path) == 0);
					free(newpath);
				}
			}

			if (newdevid)
				devid_str_free(newdevid);
		}

		if (strncmp(path, "/dev/dsk/", 9) == 0)
			path += 9;

		if (nvlist_lookup_uint64(nv, ZPOOL_CONFIG_WHOLE_DISK,
		    &value) == 0 && value) {
			char *tmp = zfs_strdup(hdl, path);
			if (tmp == NULL)
				return (NULL);
			tmp[strlen(path) - 2] = '\0';
			return (tmp);
		}
	} else {
		verify(nvlist_lookup_string(nv, ZPOOL_CONFIG_TYPE, &path) == 0);

		/*
		 * If it's a raidz device, we need to stick in the parity level.
		 */
		if (strcmp(path, VDEV_TYPE_RAIDZ) == 0) {
			verify(nvlist_lookup_uint64(nv, ZPOOL_CONFIG_NPARITY,
			    &value) == 0);
			(void) snprintf(buf, sizeof (buf), "%s%llu", path,
			    (u_longlong_t) value);
			path = buf;
		}
	}

	return (zfs_strdup(hdl, path));
}

static int
zbookmark_compare(const void *a, const void *b)
{
	return (memcmp(a, b, sizeof (zbookmark_t)));
}

/*
 * Retrieve the persistent error log, uniquify the members, and return to the
 * caller.
 */
int
zpool_get_errlog(zpool_handle_t *zhp, nvlist_t ***list, size_t *nelem)
{
	zfs_cmd_t zc = { 0 };
	uint64_t count;
	zbookmark_t *zb;
	int i, j;

	if (zhp->zpool_error_log != NULL) {
		*list = zhp->zpool_error_log;
		*nelem = zhp->zpool_error_count;
		return (0);
	}

	/*
	 * Retrieve the raw error list from the kernel.  If the number of errors
	 * has increased, allocate more space and continue until we get the
	 * entire list.
	 */
	verify(nvlist_lookup_uint64(zhp->zpool_config, ZPOOL_CONFIG_ERRCOUNT,
	    &count) == 0);
	if ((zc.zc_config_dst = (uintptr_t)zfs_alloc(zhp->zpool_hdl,
	    count * sizeof (zbookmark_t))) == (uintptr_t) NULL)
		return (-1);
	zc.zc_config_dst_size = count;
	(void) strcpy(zc.zc_name, zhp->zpool_name);
	for (;;) {
		if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_ERROR_LOG,
		    &zc) != 0) {
			free((void *)(uintptr_t)zc.zc_config_dst);
			if (errno == ENOMEM) {
				if ((zc.zc_config_dst = (uintptr_t)
				    zfs_alloc(zhp->zpool_hdl,
				    zc.zc_config_dst_size)) == (uintptr_t) NULL)
					return (-1);
			} else {
				return (-1);
			}
		} else {
			break;
		}
	}

	/*
	 * Sort the resulting bookmarks.  This is a little confusing due to the
	 * implementation of ZFS_IOC_ERROR_LOG.  The bookmarks are copied last
	 * to first, and 'zc_config_dst_size' indicates the number of boomarks
	 * _not_ copied as part of the process.  So we point the start of our
	 * array appropriate and decrement the total number of elements.
	 */
	zb = ((zbookmark_t *)(uintptr_t)zc.zc_config_dst) +
	    zc.zc_config_dst_size;
	count -= zc.zc_config_dst_size;

	qsort(zb, count, sizeof (zbookmark_t), zbookmark_compare);

	/*
	 * Count the number of unique elements
	 */
	j = 0;
	for (i = 0; i < count; i++) {
		if (i > 0 && memcmp(&zb[i - 1], &zb[i],
		    sizeof (zbookmark_t)) == 0)
			continue;
		j++;
	}

	/*
	 * If the user has only requested the number of items, return it now
	 * without bothering with the extra work.
	 */
	if (list == NULL) {
		*nelem = j;
		free((void *)(uintptr_t)zc.zc_config_dst);
		return (0);
	}

	zhp->zpool_error_count = j;

	/*
	 * Allocate an array of nvlists to hold the results
	 */
	if ((zhp->zpool_error_log = zfs_alloc(zhp->zpool_hdl,
	    j * sizeof (nvlist_t *))) == NULL) {
		free((void *)(uintptr_t)zc.zc_config_dst);
		return (-1);
	}

	/*
	 * Fill in the results with names from the kernel.
	 */
	j = 0;
	for (i = 0; i < count; i++) {
		char buf[64];
		nvlist_t *nv;

		if (i > 0 && memcmp(&zb[i - 1], &zb[i],
		    sizeof (zbookmark_t)) == 0)
			continue;

		if (nvlist_alloc(&nv, NV_UNIQUE_NAME,
		    0) != 0)
			goto nomem;
		zhp->zpool_error_log[j] = nv;

		zc.zc_bookmark = zb[i];
		if (ioctl(zhp->zpool_hdl->libzfs_fd, ZFS_IOC_BOOKMARK_NAME,
		    &zc) == 0) {
			if (nvlist_add_string(nv, ZPOOL_ERR_DATASET,
			    zc.zc_prop_name) != 0 ||
			    nvlist_add_string(nv, ZPOOL_ERR_OBJECT,
			    zc.zc_prop_value) != 0 ||
			    nvlist_add_string(nv, ZPOOL_ERR_RANGE,
			    zc.zc_filename) != 0)
				goto nomem;
		} else {
			(void) snprintf(buf, sizeof (buf), "%llx",
			    (longlong_t) zb[i].zb_objset);
			if (nvlist_add_string(nv,
			    ZPOOL_ERR_DATASET, buf) != 0)
				goto nomem;
			(void) snprintf(buf, sizeof (buf), "%llx",
			    (longlong_t) zb[i].zb_object);
			if (nvlist_add_string(nv, ZPOOL_ERR_OBJECT,
			    buf) != 0)
				goto nomem;
			(void) snprintf(buf, sizeof (buf), "lvl=%u blkid=%llu",
			    (int)zb[i].zb_level, (long long)zb[i].zb_blkid);
			if (nvlist_add_string(nv, ZPOOL_ERR_RANGE,
			    buf) != 0)
				goto nomem;
		}

		j++;
	}

	*list = zhp->zpool_error_log;
	*nelem = zhp->zpool_error_count;

	free((void *)(uintptr_t)zc.zc_config_dst);

	return (0);

nomem:
	free((void *)(uintptr_t)zc.zc_config_dst);
	for (i = 0; i < zhp->zpool_error_count; i++) {
		if (zhp->zpool_error_log[i])
			free(zhp->zpool_error_log[i]);
	}
	free(zhp->zpool_error_log);
	zhp->zpool_error_log = NULL;
	return (no_memory(zhp->zpool_hdl));
}

/*
 * Upgrade a ZFS pool to the latest on-disk version.
 */
int
zpool_upgrade(zpool_handle_t *zhp)
{
	zfs_cmd_t zc = { 0 };
	libzfs_handle_t *hdl = zhp->zpool_hdl;

	(void) strcpy(zc.zc_name, zhp->zpool_name);
	if (ioctl(hdl->libzfs_fd, ZFS_IOC_POOL_UPGRADE, &zc) != 0)
		return (zpool_standard_error(hdl, errno,
		    dgettext(TEXT_DOMAIN, "cannot upgrade '%s'"),
		    zhp->zpool_name));

	return (0);
}
