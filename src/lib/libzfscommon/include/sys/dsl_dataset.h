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

#ifndef	_SYS_DSL_DATASET_H
#define	_SYS_DSL_DATASET_H



#include <sys/dmu.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/bplist.h>
#include <sys/dsl_synctask.h>
#include <sys/zfs_context.h>

#ifdef	__cplusplus
extern "C" {
#endif

struct dsl_dataset;
struct dsl_dir;
struct dsl_pool;

typedef void dsl_dataset_evict_func_t(struct dsl_dataset *, void *);

#define	DS_FLAG_INCONSISTENT	(1ULL<<0)
/*
 * NB: nopromote can not yet be set, but we want support for it in this
 * on-disk version, so that we don't need to upgrade for it later.  It
 * will be needed when we implement 'zfs split' (where the split off
 * clone should not be promoted).
 */
#define	DS_FLAG_NOPROMOTE	(1ULL<<1)

/*
 * DS_FLAG_UNIQUE_ACCURATE is set if ds_unique_bytes has been correctly
 * calculated for head datasets (starting with SPA_VERSION_UNIQUE_ACCURATE,
 * refquota/refreservations).
 */
#define	DS_FLAG_UNIQUE_ACCURATE	(1ULL<<2)

typedef struct dsl_dataset_phys {
	uint64_t ds_dir_obj;
	uint64_t ds_prev_snap_obj;
	uint64_t ds_prev_snap_txg;
	uint64_t ds_next_snap_obj;
	uint64_t ds_snapnames_zapobj;	/* zap obj of snaps; ==0 for snaps */
	uint64_t ds_num_children;	/* clone/snap children; ==0 for head */
	uint64_t ds_creation_time;	/* seconds since 1970 */
	uint64_t ds_creation_txg;
	uint64_t ds_deadlist_obj;
	uint64_t ds_used_bytes;
	uint64_t ds_compressed_bytes;
	uint64_t ds_uncompressed_bytes;
	uint64_t ds_unique_bytes;	/* only relevant to snapshots */
	/*
	 * The ds_fsid_guid is a 56-bit ID that can change to avoid
	 * collisions.  The ds_guid is a 64-bit ID that will never
	 * change, so there is a small probability that it will collide.
	 */
	uint64_t ds_fsid_guid;
	uint64_t ds_guid;
	uint64_t ds_flags;
	blkptr_t ds_bp;
	uint64_t ds_pad[8]; /* pad out to 320 bytes for good measure */
} dsl_dataset_phys_t;

typedef struct dsl_dataset {
	/* Immutable: */
	struct dsl_dir *ds_dir;
	dsl_dataset_phys_t *ds_phys;
	dmu_buf_t *ds_dbuf;
	uint64_t ds_object;
	uint64_t ds_fsid_guid;

	/* only used in syncing context: */
	struct dsl_dataset *ds_prev; /* only valid for non-snapshots */

	/* has internal locking: */
	bplist_t ds_deadlist;

	/* protected by lock on pool's dp_dirty_datasets list */
	txg_node_t ds_dirty_link;
	list_node_t ds_synced_link;

	/*
	 * ds_phys->ds_<accounting> is also protected by ds_lock.
	 * Protected by ds_lock:
	 */
	kmutex_t ds_lock;
	void *ds_user_ptr;
	dsl_dataset_evict_func_t *ds_user_evict_func;
	uint64_t ds_open_refcount;

	/* no locking; only for making guesses */
	uint64_t ds_trysnap_txg;

	/* for objset_open() */
	kmutex_t ds_opening_lock;

	uint64_t ds_reserved;	/* cached refreservation */
	uint64_t ds_quota;	/* cached refquota */

	/* Protected by ds_lock; keep at end of struct for better locality */
	char ds_snapname[MAXNAMELEN];
} dsl_dataset_t;

#define	dsl_dataset_is_snapshot(ds)	\
	((ds)->ds_phys->ds_num_children != 0)

#define	DS_UNIQUE_IS_ACCURATE(ds)	\
	(((ds)->ds_phys->ds_flags & DS_FLAG_UNIQUE_ACCURATE) != 0)

int dsl_dataset_open_spa(spa_t *spa, const char *name, int mode,
    void *tag, dsl_dataset_t **dsp);
int dsl_dataset_open(const char *name, int mode, void *tag,
    dsl_dataset_t **dsp);
int dsl_dataset_open_obj(struct dsl_pool *dp, uint64_t dsobj,
    const char *tail, int mode, void *tag, dsl_dataset_t **);
void dsl_dataset_name(dsl_dataset_t *ds, char *name);
void dsl_dataset_close(dsl_dataset_t *ds, int mode, void *tag);
void dsl_dataset_downgrade(dsl_dataset_t *ds, int oldmode, int newmode);
boolean_t dsl_dataset_tryupgrade(dsl_dataset_t *ds, int oldmode, int newmode);
uint64_t dsl_dataset_create_sync_impl(dsl_dir_t *dd, dsl_dataset_t *origin,
    dmu_tx_t *tx);
uint64_t dsl_dataset_create_sync(dsl_dir_t *pds,
    const char *lastname, dsl_dataset_t *origin, cred_t *, dmu_tx_t *);
int dsl_dataset_destroy(dsl_dataset_t *ds, void *tag);
int dsl_snapshots_destroy(char *fsname, char *snapname);
dsl_checkfunc_t dsl_dataset_destroy_check;
dsl_syncfunc_t dsl_dataset_destroy_sync;
dsl_checkfunc_t dsl_dataset_snapshot_check;
dsl_syncfunc_t dsl_dataset_snapshot_sync;
int dsl_dataset_rollback(dsl_dataset_t *ds, dmu_objset_type_t ost);
int dsl_dataset_rename(char *name, const char *newname, boolean_t recursive);
int dsl_dataset_promote(const char *name);
int dsl_dataset_clone_swap(dsl_dataset_t *clone, dsl_dataset_t *origin_head,
    boolean_t force);

void *dsl_dataset_set_user_ptr(dsl_dataset_t *ds,
    void *p, dsl_dataset_evict_func_t func);
void *dsl_dataset_get_user_ptr(dsl_dataset_t *ds);

blkptr_t *dsl_dataset_get_blkptr(dsl_dataset_t *ds);
void dsl_dataset_set_blkptr(dsl_dataset_t *ds, blkptr_t *bp, dmu_tx_t *tx);

spa_t *dsl_dataset_get_spa(dsl_dataset_t *ds);

boolean_t dsl_dataset_modified_since_lastsnap(dsl_dataset_t *ds);

void dsl_dataset_sync(dsl_dataset_t *os, zio_t *zio, dmu_tx_t *tx);

void dsl_dataset_block_born(dsl_dataset_t *ds, blkptr_t *bp, dmu_tx_t *tx);
void dsl_dataset_block_kill(dsl_dataset_t *ds, blkptr_t *bp, zio_t *pio,
    dmu_tx_t *tx);
int dsl_dataset_block_freeable(dsl_dataset_t *ds, uint64_t blk_birth);
uint64_t dsl_dataset_prev_snap_txg(dsl_dataset_t *ds);

void dsl_dataset_dirty(dsl_dataset_t *ds, dmu_tx_t *tx);
void dsl_dataset_stats(dsl_dataset_t *os, nvlist_t *nv);
void dsl_dataset_fast_stat(dsl_dataset_t *ds, dmu_objset_stats_t *stat);
void dsl_dataset_space(dsl_dataset_t *ds,
    uint64_t *refdbytesp, uint64_t *availbytesp,
    uint64_t *usedobjsp, uint64_t *availobjsp);
uint64_t dsl_dataset_fsid_guid(dsl_dataset_t *ds);

void dsl_dataset_create_root(struct dsl_pool *dp, uint64_t *ddobjp,
    dmu_tx_t *tx);

int dsl_dsobj_to_dsname(char *pname, uint64_t obj, char *buf);

int dsl_dataset_check_quota(dsl_dataset_t *ds, boolean_t check_quota,
    uint64_t asize, uint64_t inflight, uint64_t *used);
int dsl_dataset_set_quota(const char *dsname, uint64_t quota);
void dsl_dataset_set_quota_sync(void *arg1, void *arg2, cred_t *cr,
    dmu_tx_t *tx);
int dsl_dataset_set_reservation(const char *dsname, uint64_t reservation);

#ifdef ZFS_DEBUG
#define	dprintf_ds(ds, fmt, ...) do { \
	if (zfs_flags & ZFS_DEBUG_DPRINTF) { \
	char *__ds_name = kmem_alloc(MAXNAMELEN, KM_SLEEP); \
	dsl_dataset_name(ds, __ds_name); \
	dprintf("ds=%s " fmt, __ds_name, __VA_ARGS__); \
	kmem_free(__ds_name, MAXNAMELEN); \
	} \
_NOTE(CONSTCOND) } while (0)
#else
#define	dprintf_ds(dd, fmt, ...)
#endif

#ifdef	__cplusplus
}
#endif

#endif /* _SYS_DSL_DATASET_H */
