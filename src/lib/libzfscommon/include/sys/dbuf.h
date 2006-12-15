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

#ifndef	_SYS_DBUF_H
#define	_SYS_DBUF_H



#include <sys/dmu.h>
#include <sys/spa.h>
#include <sys/txg.h>
#include <sys/zio.h>
#include <sys/arc.h>
#include <sys/zfs_context.h>
#include <sys/refcount.h>

#ifdef	__cplusplus
extern "C" {
#endif

#define	DB_BONUS_BLKID (-1ULL)
#define	IN_DMU_SYNC ((blkptr_t *)-1)

/*
 * define flags for dbuf_read
 */

#define	DB_RF_MUST_SUCCEED	(1 << 0)
#define	DB_RF_CANFAIL		(1 << 1)
#define	DB_RF_HAVESTRUCT	(1 << 2)
#define	DB_RF_NOPREFETCH	(1 << 3)
#define	DB_RF_NEVERWAIT		(1 << 4)
#define	DB_RF_CACHED		(1 << 5)

/*
 * The state transition diagram for dbufs looks like:
 *
 *		+----> READ ----+
 *		|		|
 *		|		V
 *  (alloc)-->UNCACHED	     CACHED-->EVICTING-->(free)
 *		|		^
 *		|		|
 *		+----> FILL ----+
 */
typedef enum dbuf_states {
	DB_UNCACHED,
	DB_FILL,
	DB_READ,
	DB_CACHED,
	DB_EVICTING
} dbuf_states_t;

struct objset_impl;
struct dnode;
struct dmu_tx;

/*
 * level = 0 means the user data
 * level = 1 means the single indirect block
 * etc.
 */

#define	LIST_LINK_INACTIVE(link) \
	((link)->list_next == NULL && (link)->list_prev == NULL)

typedef struct dmu_buf_impl {
	/*
	 * The following members are immutable, with the exception of
	 * db.db_data, which is protected by db_mtx.
	 */

	/* the publicly visible structure */
	dmu_buf_t db;

	/* the objset we belong to */
	struct objset_impl *db_objset;

	/*
	 * the dnode we belong to (NULL when evicted)
	 */
	struct dnode *db_dnode;

	/*
	 * our parent buffer; if the dnode points to us directly,
	 * db_parent == db_dnode->dn_dbuf
	 * only accessed by sync thread ???
	 * (NULL when evicted)
	 */
	struct dmu_buf_impl *db_parent;

	/*
	 * link for hash table of all dmu_buf_impl_t's
	 */
	struct dmu_buf_impl *db_hash_next;

	/* our block number */
	uint64_t db_blkid;

	/*
	 * Pointer to the blkptr_t which points to us. May be NULL if we
	 * don't have one yet. (NULL when evicted)
	 */
	blkptr_t *db_blkptr;

	/*
	 * Our indirection level.  Data buffers have db_level==0.
	 * Indirect buffers which point to data buffers have
	 * db_level==1. etc.  Buffers which contain dnodes have
	 * db_level==0, since the dnodes are stored in a file.
	 */
	uint8_t db_level;

	/* db_mtx protects the members below */
	kmutex_t db_mtx;

	/*
	 * Current state of the buffer
	 */
	dbuf_states_t db_state;

	/*
	 * Refcount accessed by dmu_buf_{hold,rele}.
	 * If nonzero, the buffer can't be destroyed.
	 * Protected by db_mtx.
	 */
	refcount_t db_holds;

	/* buffer holding our data */
	arc_buf_t *db_buf;

	kcondvar_t db_changed;
	arc_buf_t *db_data_pending;

	/*
	 * Last time (transaction group) this buffer was dirtied.
	 */
	uint64_t db_dirtied;

	/*
	 * If db_dnode != NULL, our link on the owner dnodes's dn_dbufs list.
	 * Protected by its dn_dbufs_mtx.
	 */
	list_node_t db_link;

	/* Our link on dn_dirty_dbufs[txg] */
	list_node_t db_dirty_node[TXG_SIZE];
	uint8_t db_dirtycnt;

	/*
	 * Data which is unique to data (leaf) blocks:
	 */
	struct {
		/* stuff we store for the user (see dmu_buf_set_user) */
		void *db_user_ptr;
		void **db_user_data_ptr_ptr;
		dmu_buf_evict_func_t *db_evict_func;
		uint8_t db_immediate_evict;
		uint8_t db_freed_in_flight;

		/*
		 * db_data_old[txg&TXG_MASK] is set when we
		 * dirty the buffer, so that we can retain the
		 * pointer even if it gets COW'd in a subsequent
		 * transaction group.
		 *
		 * If the buffer is dirty in any txg, it can't
		 * be destroyed.
		 */
		/*
		 * XXX Protected by db_mtx and dn_dirty_mtx.
		 * db_mtx must be held to read db_dirty[], and
		 * both db_mtx and dn_dirty_mtx must be held to
		 * modify (dirty or clean). db_mtx must be held
		 * before dn_dirty_mtx.
		 */
		arc_buf_t *db_data_old[TXG_SIZE];
		blkptr_t *db_overridden_by[TXG_SIZE];
	} db_d;
} dmu_buf_impl_t;

/* Note: the dbuf hash table is exposed only for the mdb module */
#define	DBUF_MUTEXES 256
#define	DBUF_HASH_MUTEX(h, idx) (&(h)->hash_mutexes[(idx) & (DBUF_MUTEXES-1)])
typedef struct dbuf_hash_table {
	uint64_t hash_table_mask;
	dmu_buf_impl_t **hash_table;
	kmutex_t hash_mutexes[DBUF_MUTEXES];
} dbuf_hash_table_t;


uint64_t dbuf_whichblock(struct dnode *di, uint64_t offset);

dmu_buf_impl_t *dbuf_create_tlib(struct dnode *dn, char *data);
dmu_buf_impl_t *dbuf_create_bonus(struct dnode *dn);

dmu_buf_impl_t *dbuf_hold(struct dnode *dn, uint64_t blkid, void *tag);
dmu_buf_impl_t *dbuf_hold_level(struct dnode *dn, int level, uint64_t blkid,
    void *tag);
int dbuf_hold_impl(struct dnode *dn, uint8_t level, uint64_t blkid, int create,
    void *tag, dmu_buf_impl_t **dbp);

void dbuf_prefetch(struct dnode *dn, uint64_t blkid);

void dbuf_add_ref(dmu_buf_impl_t *db, void *tag);
uint64_t dbuf_refcount(dmu_buf_impl_t *db);

void dbuf_rele(dmu_buf_impl_t *db, void *tag);

dmu_buf_impl_t *dbuf_find(struct dnode *dn, uint8_t level, uint64_t blkid);

int dbuf_read(dmu_buf_impl_t *db, zio_t *zio, uint32_t flags);
void dbuf_will_dirty(dmu_buf_impl_t *db, dmu_tx_t *tx);
void dmu_buf_will_fill(dmu_buf_t *db, dmu_tx_t *tx);
void dbuf_fill_done(dmu_buf_impl_t *db, dmu_tx_t *tx);
void dmu_buf_will_fill(dmu_buf_t *db, dmu_tx_t *tx);
void dmu_buf_fill_done(dmu_buf_t *db, dmu_tx_t *tx);
void dbuf_dirty(dmu_buf_impl_t *db, dmu_tx_t *tx);

void dbuf_clear(dmu_buf_impl_t *db);
void dbuf_evict(dmu_buf_impl_t *db);

void dbuf_setdirty(dmu_buf_impl_t *db, dmu_tx_t *tx);
void dbuf_sync(dmu_buf_impl_t *db, zio_t *zio, dmu_tx_t *tx);
void dbuf_unoverride(dmu_buf_impl_t *db, uint64_t txg);

void dbuf_free_range(struct dnode *dn, uint64_t blkid, uint64_t nblks,
    struct dmu_tx *);

void dbuf_new_size(dmu_buf_impl_t *db, int size, dmu_tx_t *tx);

void dbuf_init(void);
void dbuf_fini(void);

#ifdef ZFS_DEBUG

/*
 * There should be a ## between the string literal and fmt, to make it
 * clear that we're joining two strings together, but gcc does not
 * support that preprocessor token.
 */
#define	dprintf_dbuf(dbuf, fmt, ...) do { \
	if (zfs_flags & ZFS_DEBUG_DPRINTF) { \
	char __db_buf[32]; \
	uint64_t __db_obj = (dbuf)->db.db_object; \
	if (__db_obj == DMU_META_DNODE_OBJECT) \
		(void) strcpy(__db_buf, "mdn"); \
	else \
		(void) snprintf(__db_buf, sizeof (__db_buf), "%lld", \
		    (u_longlong_t)__db_obj); \
	dprintf_ds((dbuf)->db_objset->os_dsl_dataset, \
	    "obj=%s lvl=%u blkid=%lld " fmt, \
	    __db_buf, (dbuf)->db_level, \
	    (u_longlong_t)(dbuf)->db_blkid, __VA_ARGS__); \
	} \
_NOTE(CONSTCOND) } while (0)

#define	dprintf_dbuf_bp(db, bp, fmt, ...) do {			\
	if (zfs_flags & ZFS_DEBUG_DPRINTF) {			\
	char __blkbuf[BP_SPRINTF_LEN];				\
	sprintf_blkptr(__blkbuf, BP_SPRINTF_LEN, bp);		\
	dprintf_dbuf(db, fmt " %s\n", __VA_ARGS__, __blkbuf);	\
	} \
_NOTE(CONSTCOND) } while (0)

#define	DBUF_VERIFY(db)	dbuf_verify(db)

#else

#define	dprintf_dbuf(db, fmt, ...)
#define	dprintf_dbuf_bp(db, bp, fmt, ...)
#define	DBUF_VERIFY(db)

#endif


#ifdef	__cplusplus
}
#endif

#endif /* _SYS_DBUF_H */
