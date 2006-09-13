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
#include <errno.h>
#include <libgen.h>
#include <libintl.h>
#include <libuutil.h>
#include <locale.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <zone.h>
#include <sys/mkdev.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <libzfs.h>

#include "zfs_iter.h"
#include "zfs_util.h"

libzfs_handle_t *g_zfs;

static FILE *mnttab_file;

static int zfs_do_clone(int argc, char **argv);
static int zfs_do_create(int argc, char **argv);
static int zfs_do_destroy(int argc, char **argv);
static int zfs_do_get(int argc, char **argv);
static int zfs_do_inherit(int argc, char **argv);
static int zfs_do_list(int argc, char **argv);
static int zfs_do_mount(int argc, char **argv);
static int zfs_do_rename(int argc, char **argv);
static int zfs_do_rollback(int argc, char **argv);
static int zfs_do_set(int argc, char **argv);
static int zfs_do_snapshot(int argc, char **argv);
static int zfs_do_unmount(int argc, char **argv);
static int zfs_do_share(int argc, char **argv);
static int zfs_do_unshare(int argc, char **argv);
static int zfs_do_send(int argc, char **argv);
static int zfs_do_receive(int argc, char **argv);
static int zfs_do_promote(int argc, char **argv);

/*
 * These libumem hooks provide a reasonable set of defaults for the allocator's
 * debugging facilities.
 */
const char *
_umem_debug_init()
{
	return ("default,verbose"); /* $UMEM_DEBUG setting */
}

const char *
_umem_logging_init(void)
{
	return ("fail,contents"); /* $UMEM_LOGGING setting */
}

typedef enum {
	HELP_CLONE,
	HELP_CREATE,
	HELP_DESTROY,
	HELP_GET,
	HELP_INHERIT,
	HELP_LIST,
	HELP_MOUNT,
	HELP_PROMOTE,
	HELP_RECEIVE,
	HELP_RENAME,
	HELP_ROLLBACK,
	HELP_SEND,
	HELP_SET,
	HELP_SHARE,
	HELP_SNAPSHOT,
	HELP_UNMOUNT,
	HELP_UNSHARE
} zfs_help_t;

typedef struct zfs_command {
	const char	*name;
	int		(*func)(int argc, char **argv);
	zfs_help_t	usage;
} zfs_command_t;

/*
 * Master command table.  Each ZFS command has a name, associated function, and
 * usage message.  The usage messages need to be internationalized, so we have
 * to have a function to return the usage message based on a command index.
 *
 * These commands are organized according to how they are displayed in the usage
 * message.  An empty command (one with a NULL name) indicates an empty line in
 * the generic usage message.
 */
static zfs_command_t command_table[] = {
	{ "create",	zfs_do_create,		HELP_CREATE		},
	{ "destroy",	zfs_do_destroy,		HELP_DESTROY		},
	{ NULL },
	{ "snapshot",	zfs_do_snapshot,	HELP_SNAPSHOT		},
	{ "rollback",	zfs_do_rollback,	HELP_ROLLBACK		},
	{ "clone",	zfs_do_clone,		HELP_CLONE		},
	{ "promote",	zfs_do_promote,		HELP_PROMOTE		},
	{ "rename",	zfs_do_rename,		HELP_RENAME		},
	{ NULL },
	{ "list",	zfs_do_list,		HELP_LIST		},
	{ NULL },
	{ "set",	zfs_do_set,		HELP_SET		},
	{ "get", 	zfs_do_get,		HELP_GET		},
	{ "inherit",	zfs_do_inherit,		HELP_INHERIT		},
	{ NULL },
	{ "mount",	zfs_do_mount,		HELP_MOUNT		},
	{ NULL },
	{ "unmount",	zfs_do_unmount,		HELP_UNMOUNT		},
	{ NULL },
	{ "share",	zfs_do_share,		HELP_SHARE		},
	{ NULL },
	{ "unshare",	zfs_do_unshare,		HELP_UNSHARE		},
	{ NULL },
	{ "send",	zfs_do_send,		HELP_SEND		},
	{ "receive",	zfs_do_receive,		HELP_RECEIVE		},
};

#define	NCOMMAND	(sizeof (command_table) / sizeof (command_table[0]))

zfs_command_t *current_command;

static const char *
get_usage(zfs_help_t idx)
{
	switch (idx) {
	case HELP_CLONE:
		return (gettext("\tclone <snapshot> <filesystem|volume>\n"));
	case HELP_CREATE:
		return (gettext("\tcreate <filesystem>\n"
		    "\tcreate [-s] [-b blocksize] -V <size> <volume>\n"));
	case HELP_DESTROY:
		return (gettext("\tdestroy [-rRf] "
		    "<filesystem|volume|snapshot>\n"));
	case HELP_GET:
		return (gettext("\tget [-rHp] [-o field[,field]...] "
		    "[-s source[,source]...]\n"
		    "\t    <all | property[,property]...> "
		    "<filesystem|volume|snapshot> ...\n"));
	case HELP_INHERIT:
		return (gettext("\tinherit [-r] <property> "
		    "<filesystem|volume> ...\n"));
	case HELP_LIST:
		return (gettext("\tlist [-rH] [-o property[,property]...] "
		    "[-t type[,type]...]\n"
		    "\t    [-s property [-s property]...]"
		    " [-S property [-S property]...]\n"
		    "\t    [filesystem|volume|snapshot] ...\n"));
	case HELP_MOUNT:
		return (gettext("\tmount\n"
		    "\tmount [-o opts] [-O] -a\n"
		    "\tmount [-o opts] [-O] <filesystem>\n"));
	case HELP_PROMOTE:
		return (gettext("\tpromote <clone filesystem>\n"));
	case HELP_RECEIVE:
		return (gettext("\treceive [-vn] <filesystem|volume|snapshot>\n"
		    "\treceive [-vn] -d <filesystem>\n"));
	case HELP_RENAME:
		return (gettext("\trename <filesystem|volume|snapshot> "
		    "<filesystem|volume|snapshot>\n"));
	case HELP_ROLLBACK:
		return (gettext("\trollback [-rRf] <snapshot>\n"));
	case HELP_SEND:
		return (gettext("\tsend [-i <snapshot>] <snapshot>\n"));
	case HELP_SET:
		return (gettext("\tset <property=value> "
		    "<filesystem|volume> ...\n"));
	case HELP_SHARE:
		return (gettext("\tshare -a\n"
		    "\tshare <filesystem>\n"));
	case HELP_SNAPSHOT:
		return (gettext("\tsnapshot [-r] "
		    "<filesystem@name|volume@name>\n"));
	case HELP_UNMOUNT:
		return (gettext("\tunmount [-f] -a\n"
		    "\tunmount [-f] <filesystem|mountpoint>\n"));
	case HELP_UNSHARE:
		return (gettext("\tunshare [-f] -a\n"
		    "\tunshare [-f] <filesystem|mountpoint>\n"));
	}

	abort();
	/* NOTREACHED */
}

/*
 * Utility function to guarantee malloc() success.
 */
void *
safe_malloc(size_t size)
{
	void *data;

	if ((data = calloc(1, size)) == NULL) {
		(void) fprintf(stderr, "internal error: out of memory\n");
		exit(1);
	}

	return (data);
}

/*
 * Display usage message.  If we're inside a command, display only the usage for
 * that command.  Otherwise, iterate over the entire command table and display
 * a complete usage message.
 */
static void
usage(boolean_t requested)
{
	int i;
	boolean_t show_properties = B_FALSE;
	FILE *fp = requested ? stdout : stderr;

	if (current_command == NULL) {

		(void) fprintf(fp, gettext("usage: zfs command args ...\n"));
		(void) fprintf(fp,
		    gettext("where 'command' is one of the following:\n\n"));

		for (i = 0; i < NCOMMAND; i++) {
			if (command_table[i].name == NULL)
				(void) fprintf(fp, "\n");
			else
				(void) fprintf(fp, "%s",
				    get_usage(command_table[i].usage));
		}

		(void) fprintf(fp, gettext("\nEach dataset is of the form: "
		    "pool/[dataset/]*dataset[@name]\n"));
	} else {
		(void) fprintf(fp, gettext("usage:\n"));
		(void) fprintf(fp, "%s", get_usage(current_command->usage));
	}

	if (current_command != NULL &&
	    (strcmp(current_command->name, "set") == 0 ||
	    strcmp(current_command->name, "get") == 0 ||
	    strcmp(current_command->name, "inherit") == 0 ||
	    strcmp(current_command->name, "list") == 0))
		show_properties = B_TRUE;

	if (show_properties) {

		(void) fprintf(fp,
		    gettext("\nThe following properties are supported:\n"));

		(void) fprintf(fp, "\n\t%-13s  %s  %s   %s\n\n",
		    "PROPERTY", "EDIT", "INHERIT", "VALUES");

		for (i = 0; i < ZFS_NPROP_VISIBLE; i++) {
			(void) fprintf(fp, "\t%-13s  ", zfs_prop_to_name(i));

			if (zfs_prop_readonly(i))
				(void) fprintf(fp, "  NO    ");
			else
				(void) fprintf(fp, " YES    ");

			if (zfs_prop_inheritable(i))
				(void) fprintf(fp, "  YES   ");
			else
				(void) fprintf(fp, "   NO   ");

			if (zfs_prop_values(i) == NULL)
				(void) fprintf(fp, "-\n");
			else
				(void) fprintf(fp, "%s\n", zfs_prop_values(i));
		}
		(void) fprintf(fp, gettext("\nSizes are specified in bytes "
		    "with standard units such as K, M, G, etc.\n"));
	} else {
		/*
		 * TRANSLATION NOTE:
		 * "zfs set|get" must not be localised this is the
		 * command name and arguments.
		 */
		(void) fprintf(fp,
		    gettext("\nFor the property list, run: zfs set|get\n"));
	}

	exit(requested ? 0 : 2);
}

/*
 * zfs clone <fs, snap, vol> fs
 *
 * Given an existing dataset, create a writable copy whose initial contents
 * are the same as the source.  The newly created dataset maintains a
 * dependency on the original; the original cannot be destroyed so long as
 * the clone exists.
 */
static int
zfs_do_clone(int argc, char **argv)
{
	zfs_handle_t *zhp;
	int ret;

	/* check options */
	if (argc > 1 && argv[1][0] == '-') {
		(void) fprintf(stderr, gettext("invalid option '%c'\n"),
		    argv[1][1]);
		usage(B_FALSE);
	}

	/* check number of arguments */
	if (argc < 2) {
		(void) fprintf(stderr, gettext("missing source dataset "
		    "argument\n"));
		usage(B_FALSE);
	}
	if (argc < 3) {
		(void) fprintf(stderr, gettext("missing target dataset "
		    "argument\n"));
		usage(B_FALSE);
	}
	if (argc > 3) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	/* open the source dataset */
	if ((zhp = zfs_open(g_zfs, argv[1], ZFS_TYPE_SNAPSHOT)) == NULL)
		return (1);

	/* pass to libzfs */
	ret = zfs_clone(zhp, argv[2]);

	/* create the mountpoint if necessary */
	if (ret == 0) {
		zfs_handle_t *clone = zfs_open(g_zfs, argv[2], ZFS_TYPE_ANY);
		if (clone != NULL) {
			if ((ret = zfs_mount(clone, NULL, 0)) == 0)
				ret = zfs_share(clone);
			zfs_close(clone);
		}
	}

	zfs_close(zhp);

	return (ret == 0 ? 0 : 1);
}

/*
 * zfs create fs
 * zfs create [-s] [-b blocksize] -V vol size
 *
 * Create a new dataset.  This command can be used to create filesystems
 * and volumes.  Snapshot creation is handled by 'zfs snapshot'.
 * For volumes, the user must specify a size to be used.
 *
 * The '-s' flag applies only to volumes, and indicates that we should not try
 * to set the reservation for this volume.  By default we set a reservation
 * equal to the size for any volume.
 */
static int
zfs_do_create(int argc, char **argv)
{
	zfs_type_t type = ZFS_TYPE_FILESYSTEM;
	zfs_handle_t *zhp;
	char *size = NULL;
	char *blocksize = NULL;
	int c;
	boolean_t noreserve = B_FALSE;
	int ret;

	/* check options */
	while ((c = getopt(argc, argv, ":V:b:s")) != -1) {
		switch (c) {
		case 'V':
			type = ZFS_TYPE_VOLUME;
			size = optarg;
			break;
		case 'b':
			blocksize = optarg;
			break;
		case 's':
			noreserve = B_TRUE;
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing size "
			    "argument\n"));
			usage(B_FALSE);
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	if (noreserve && type != ZFS_TYPE_VOLUME) {
		(void) fprintf(stderr, gettext("'-s' can only be used when "
		    "creating a volume\n"));
		usage(B_FALSE);
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc == 0) {
		(void) fprintf(stderr, gettext("missing %s argument\n"),
		    zfs_type_to_name(type));
		usage(B_FALSE);
	}
	if (argc > 1) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	/* pass to libzfs */
	if (zfs_create(g_zfs, argv[0], type, size, blocksize) != 0)
		return (1);

	if ((zhp = zfs_open(g_zfs, argv[0], ZFS_TYPE_ANY)) == NULL)
		return (1);

	/*
	 * Volume handling.  By default, we try to create a reservation of equal
	 * size for the volume.  If we can't do this, then destroy the dataset
	 * and report an error.
	 */
	if (type == ZFS_TYPE_VOLUME && !noreserve) {
		if (zfs_prop_set(zhp, ZFS_PROP_RESERVATION, size) != 0) {
			(void) fprintf(stderr, gettext("use '-s' to create a "
			    "volume without a matching reservation\n"));
			(void) zfs_destroy(zhp);
			return (1);
		}
	}

	/*
	 * Mount and/or share the new filesystem as appropriate.  We provide a
	 * verbose error message to let the user know that their filesystem was
	 * in fact created, even if we failed to mount or share it.
	 */
	if (zfs_mount(zhp, NULL, 0) != 0) {
		(void) fprintf(stderr, gettext("filesystem successfully "
		    "created, but not mounted\n"));
		ret = 1;
	} else if (zfs_share(zhp) != 0) {
		(void) fprintf(stderr, gettext("filesystem successfully "
		    "created, but not shared\n"));
		ret = 1;
	} else {
		ret = 0;
	}

	zfs_close(zhp);
	return (ret);
}

/*
 * zfs destroy [-rf] <fs, snap, vol>
 *
 * 	-r	Recursively destroy all children
 * 	-R	Recursively destroy all dependents, including clones
 * 	-f	Force unmounting of any dependents
 *
 * Destroys the given dataset.  By default, it will unmount any filesystems,
 * and refuse to destroy a dataset that has any dependents.  A dependent can
 * either be a child, or a clone of a child.
 */
typedef struct destroy_cbdata {
	boolean_t	cb_first;
	int		cb_force;
	int		cb_recurse;
	int		cb_error;
	int		cb_needforce;
	int		cb_doclones;
	zfs_handle_t	*cb_target;
	char		*cb_snapname;
} destroy_cbdata_t;

/*
 * Check for any dependents based on the '-r' or '-R' flags.
 */
static int
destroy_check_dependent(zfs_handle_t *zhp, void *data)
{
	destroy_cbdata_t *cbp = data;
	const char *tname = zfs_get_name(cbp->cb_target);
	const char *name = zfs_get_name(zhp);

	if (strncmp(tname, name, strlen(tname)) == 0 &&
	    (name[strlen(tname)] == '/' || name[strlen(tname)] == '@')) {
		/*
		 * This is a direct descendant, not a clone somewhere else in
		 * the hierarchy.
		 */
		if (cbp->cb_recurse)
			goto out;

		if (cbp->cb_first) {
			(void) fprintf(stderr, gettext("cannot destroy '%s': "
			    "%s has children\n"),
			    zfs_get_name(cbp->cb_target),
			    zfs_type_to_name(zfs_get_type(cbp->cb_target)));
			(void) fprintf(stderr, gettext("use '-r' to destroy "
			    "the following datasets:\n"));
			cbp->cb_first = B_FALSE;
			cbp->cb_error = 1;
		}

		(void) fprintf(stderr, "%s\n", zfs_get_name(zhp));
	} else {
		/*
		 * This is a clone.  We only want to report this if the '-r'
		 * wasn't specified, or the target is a snapshot.
		 */
		if (!cbp->cb_recurse &&
		    zfs_get_type(cbp->cb_target) != ZFS_TYPE_SNAPSHOT)
			goto out;

		if (cbp->cb_first) {
			(void) fprintf(stderr, gettext("cannot destroy '%s': "
			    "%s has dependent clones\n"),
			    zfs_get_name(cbp->cb_target),
			    zfs_type_to_name(zfs_get_type(cbp->cb_target)));
			(void) fprintf(stderr, gettext("use '-R' to destroy "
			    "the following datasets:\n"));
			cbp->cb_first = B_FALSE;
			cbp->cb_error = 1;
		}

		(void) fprintf(stderr, "%s\n", zfs_get_name(zhp));
	}

out:
	zfs_close(zhp);
	return (0);
}

static int
destroy_callback(zfs_handle_t *zhp, void *data)
{
	destroy_cbdata_t *cbp = data;

	/*
	 * Ignore pools (which we've already flagged as an error before getting
	 * here.
	 */
	if (strchr(zfs_get_name(zhp), '/') == NULL &&
	    zfs_get_type(zhp) == ZFS_TYPE_FILESYSTEM) {
		zfs_close(zhp);
		return (0);
	}

	/*
	 * Bail out on the first error.
	 */
	if (zfs_unmount(zhp, NULL, cbp->cb_force ? MS_FORCE : 0) != 0 ||
	    zfs_destroy(zhp) != 0) {
		zfs_close(zhp);
		return (-1);
	}

	zfs_close(zhp);
	return (0);
}

static int
destroy_snap_clones(zfs_handle_t *zhp, void *arg)
{
	destroy_cbdata_t *cbp = arg;
	char thissnap[MAXPATHLEN];
	zfs_handle_t *szhp;

	(void) snprintf(thissnap, sizeof (thissnap),
	    "%s@%s", zfs_get_name(zhp), cbp->cb_snapname);

	libzfs_print_on_error(g_zfs, B_FALSE);
	szhp = zfs_open(g_zfs, thissnap, ZFS_TYPE_SNAPSHOT);
	libzfs_print_on_error(g_zfs, B_TRUE);
	if (szhp) {
		/*
		 * Destroy any clones of this snapshot
		 */
		if (zfs_iter_dependents(szhp, B_FALSE, destroy_callback,
		    cbp) != 0) {
			zfs_close(szhp);
			return (-1);
		}
		zfs_close(szhp);
	}

	return (zfs_iter_filesystems(zhp, destroy_snap_clones, arg));
}

static int
zfs_do_destroy(int argc, char **argv)
{
	destroy_cbdata_t cb = { 0 };
	int c;
	zfs_handle_t *zhp;
	char *cp;

	/* check options */
	while ((c = getopt(argc, argv, "frR")) != -1) {
		switch (c) {
		case 'f':
			cb.cb_force = 1;
			break;
		case 'r':
			cb.cb_recurse = 1;
			break;
		case 'R':
			cb.cb_recurse = 1;
			cb.cb_doclones = 1;
			break;
		case '?':
		default:
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc == 0) {
		(void) fprintf(stderr, gettext("missing path argument\n"));
		usage(B_FALSE);
	}
	if (argc > 1) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	/*
	 * If we are doing recursive destroy of a snapshot, then the
	 * named snapshot may not exist.  Go straight to libzfs.
	 */
	if (cb.cb_recurse && (cp = strchr(argv[0], '@'))) {
		int ret;

		*cp = '\0';
		if ((zhp = zfs_open(g_zfs, argv[0], ZFS_TYPE_ANY)) == NULL)
			return (1);
		*cp = '@';
		cp++;

		if (cb.cb_doclones) {
			cb.cb_snapname = cp;
			if (destroy_snap_clones(zhp, &cb) != 0) {
				zfs_close(zhp);
				return (1);
			}
		}

		ret = zfs_destroy_snaps(zhp, cp);
		zfs_close(zhp);
		if (ret) {
			(void) fprintf(stderr,
			    gettext("no snapshots destroyed\n"));
		}
		return (ret != 0);
	}


	/* Open the given dataset */
	if ((zhp = zfs_open(g_zfs, argv[0], ZFS_TYPE_ANY)) == NULL)
		return (1);

	cb.cb_target = zhp;

	/*
	 * Perform an explicit check for pools before going any further.
	 */
	if (!cb.cb_recurse && strchr(zfs_get_name(zhp), '/') == NULL &&
	    zfs_get_type(zhp) == ZFS_TYPE_FILESYSTEM) {
		(void) fprintf(stderr, gettext("cannot destroy '%s': "
		    "operation does not apply to pools\n"),
		    zfs_get_name(zhp));
		(void) fprintf(stderr, gettext("use 'zfs destroy -r "
		    "%s' to destroy all datasets in the pool\n"),
		    zfs_get_name(zhp));
		(void) fprintf(stderr, gettext("use 'zpool destroy %s' "
		    "to destroy the pool itself\n"), zfs_get_name(zhp));
		zfs_close(zhp);
		return (1);
	}

	/*
	 * Check for any dependents and/or clones.
	 */
	cb.cb_first = B_TRUE;
	if (!cb.cb_doclones &&
	    zfs_iter_dependents(zhp, B_TRUE, destroy_check_dependent,
	    &cb) != 0) {
		zfs_close(zhp);
		return (1);
	}


	if (cb.cb_error ||
	    zfs_iter_dependents(zhp, B_FALSE, destroy_callback, &cb) != 0) {
		zfs_close(zhp);
		return (1);
	}

	/*
	 * Do the real thing.  The callback will close the handle regardless of
	 * whether it succeeds or not.
	 */
	if (destroy_callback(zhp, &cb) != 0)
		return (1);

	return (0);
}

/*
 * zfs get [-rHp] [-o field[,field]...] [-s source[,source]...]
 * 	< all | property[,property]... > < fs | snap | vol > ...
 *
 *	-r	recurse over any child datasets
 *	-H	scripted mode.  Headers are stripped, and fields are separated
 *		by tabs instead of spaces.
 *	-o	Set of fields to display.  One of "name,property,value,source".
 *		Default is all four.
 *	-s	Set of sources to allow.  One of
 *		"local,default,inherited,temporary,none".  Default is all
 *		five.
 *	-p	Display values in parsable (literal) format.
 *
 *  Prints properties for the given datasets.  The user can control which
 *  columns to display as well as which property types to allow.
 */
typedef struct get_cbdata {
	int cb_sources;
	int cb_columns[4];
	int cb_nprop;
	boolean_t cb_scripted;
	boolean_t cb_literal;
	boolean_t cb_isall;
	zfs_prop_t cb_prop[ZFS_NPROP_ALL];
} get_cbdata_t;

#define	GET_COL_NAME		1
#define	GET_COL_PROPERTY	2
#define	GET_COL_VALUE		3
#define	GET_COL_SOURCE		4

/*
 * Display a single line of output, according to the settings in the callback
 * structure.
 */
static void
print_one_property(zfs_handle_t *zhp, get_cbdata_t *cbp, zfs_prop_t prop,
    const char *value, zfs_source_t sourcetype, const char *source)
{
	int i;
	int width;
	const char *str;
	char buf[128];

	/*
	 * Ignore those source types that the user has chosen to ignore.
	 */
	if ((sourcetype & cbp->cb_sources) == 0)
		return;

	for (i = 0; i < 4; i++) {
		switch (cbp->cb_columns[i]) {
		case GET_COL_NAME:
			width = 15;
			str = zfs_get_name(zhp);
			break;

		case GET_COL_PROPERTY:
			width = 13;
			str = zfs_prop_to_name(prop);
			break;

		case GET_COL_VALUE:
			width = 25;
			str = value;
			break;

		case GET_COL_SOURCE:
			width = 15;
			switch (sourcetype) {
			case ZFS_SRC_NONE:
				str = "-";
				break;

			case ZFS_SRC_DEFAULT:
				str = "default";
				break;

			case ZFS_SRC_LOCAL:
				str = "local";
				break;

			case ZFS_SRC_TEMPORARY:
				str = "temporary";
				break;

			case ZFS_SRC_INHERITED:
				(void) snprintf(buf, sizeof (buf),
				    "inherited from %s", source);
				str = buf;
				break;
			}
			break;

		default:
			continue;
		}

		if (cbp->cb_columns[i + 1] == 0)
			(void) printf("%s", str);
		else if (cbp->cb_scripted)
			(void) printf("%s\t", str);
		else
			(void) printf("%-*s  ", width, str);

	}

	(void) printf("\n");
}

/*
 * Invoked to display the properties for a single dataset.
 */
static int
get_callback(zfs_handle_t *zhp, void *data)
{
	char buf[ZFS_MAXPROPLEN];
	zfs_source_t sourcetype;
	char source[ZFS_MAXNAMELEN];
	get_cbdata_t *cbp = data;
	int i;

	for (i = 0; i < cbp->cb_nprop; i++) {
		if (zfs_prop_get(zhp, cbp->cb_prop[i], buf,
		    sizeof (buf), &sourcetype, source, sizeof (source),
		    cbp->cb_literal) != 0) {
			if (cbp->cb_isall)
				continue;
			(void) strlcpy(buf, "-", sizeof (buf));
			sourcetype = ZFS_SRC_NONE;
		}

		print_one_property(zhp, cbp, cbp->cb_prop[i],
		    buf, sourcetype, source);
	}

	return (0);
}

static int
zfs_do_get(int argc, char **argv)
{
	get_cbdata_t cb = { 0 };
	boolean_t recurse = B_FALSE;
	int c;
	char *value, *fields, *badopt;
	int i;
	int ret;

	/*
	 * Set up default columns and sources.
	 */
	cb.cb_sources = ZFS_SRC_ALL;
	cb.cb_columns[0] = GET_COL_NAME;
	cb.cb_columns[1] = GET_COL_PROPERTY;
	cb.cb_columns[2] = GET_COL_VALUE;
	cb.cb_columns[3] = GET_COL_SOURCE;

	/* check options */
	while ((c = getopt(argc, argv, ":o:s:rHp")) != -1) {
		switch (c) {
		case 'p':
			cb.cb_literal = B_TRUE;
			break;
		case 'r':
			recurse = B_TRUE;
			break;
		case 'H':
			cb.cb_scripted = B_TRUE;
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing argument for "
			    "'%c' option\n"), optopt);
			usage(B_FALSE);
			break;
		case 'o':
			/*
			 * Process the set of columns to display.  We zero out
			 * the structure to give us a blank slate.
			 */
			bzero(&cb.cb_columns, sizeof (cb.cb_columns));
			i = 0;
			while (*optarg != '\0') {
				static char *col_subopts[] =
				    { "name", "property", "value", "source",
				    NULL };

				if (i == 4) {
					(void) fprintf(stderr, gettext("too "
					    "many fields given to -o "
					    "option\n"));
					usage(B_FALSE);
				}

				switch (getsubopt(&optarg, col_subopts,
				    &value)) {
				case 0:
					cb.cb_columns[i++] = GET_COL_NAME;
					break;
				case 1:
					cb.cb_columns[i++] = GET_COL_PROPERTY;
					break;
				case 2:
					cb.cb_columns[i++] = GET_COL_VALUE;
					break;
				case 3:
					cb.cb_columns[i++] = GET_COL_SOURCE;
					break;
				default:
					(void) fprintf(stderr,
					    gettext("invalid column name "
					    "'%s'\n"), value);
					    usage(B_FALSE);
				}
			}
			break;

		case 's':
			cb.cb_sources = 0;
			while (*optarg != '\0') {
				static char *source_subopts[] = {
					"local", "default", "inherited",
					"temporary", "none", NULL };

				switch (getsubopt(&optarg, source_subopts,
				    &value)) {
				case 0:
					cb.cb_sources |= ZFS_SRC_LOCAL;
					break;
				case 1:
					cb.cb_sources |= ZFS_SRC_DEFAULT;
					break;
				case 2:
					cb.cb_sources |= ZFS_SRC_INHERITED;
					break;
				case 3:
					cb.cb_sources |= ZFS_SRC_TEMPORARY;
					break;
				case 4:
					cb.cb_sources |= ZFS_SRC_NONE;
					break;
				default:
					(void) fprintf(stderr,
					    gettext("invalid source "
					    "'%s'\n"), value);
					    usage(B_FALSE);
				}
			}
			break;

		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing property "
		    "argument\n"));
		usage(B_FALSE);
	}

	fields = argv[0];

	/*
	 * If the user specifies 'all', the behavior of 'zfs get' is slightly
	 * different, because we don't show properties which don't apply to the
	 * given dataset.
	 */
	if (strcmp(fields, "all") == 0)
		cb.cb_isall = B_TRUE;

	if ((ret = zfs_get_proplist(fields, cb.cb_prop, ZFS_NPROP_ALL,
	    &cb.cb_nprop, &badopt)) != 0) {
		if (ret == EINVAL)
			(void) fprintf(stderr, gettext("invalid property "
			    "'%s'\n"), badopt);
		else
			(void) fprintf(stderr, gettext("too many properties "
			    "specified\n"));
		usage(B_FALSE);
	}

	argc--;
	argv++;

	/* check for at least one dataset name */
	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing dataset argument\n"));
		usage(B_FALSE);
	}

	/*
	 * Print out any headers
	 */
	if (!cb.cb_scripted) {
		int i;
		for (i = 0; i < 4; i++) {
			switch (cb.cb_columns[i]) {
			case GET_COL_NAME:
				(void) printf("%-15s  ", "NAME");
				break;
			case GET_COL_PROPERTY:
				(void) printf("%-13s  ", "PROPERTY");
				break;
			case GET_COL_VALUE:
				(void) printf("%-25s  ", "VALUE");
				break;
			case GET_COL_SOURCE:
				(void) printf("%s", "SOURCE");
				break;
			}
		}
		(void) printf("\n");
	}

	/* run for each object */
	return (zfs_for_each(argc, argv, recurse, ZFS_TYPE_ANY, NULL,
	    get_callback, &cb));

}

/*
 * inherit [-r] <property> <fs|vol> ...
 *
 * 	-r	Recurse over all children
 *
 * For each dataset specified on the command line, inherit the given property
 * from its parent.  Inheriting a property at the pool level will cause it to
 * use the default value.  The '-r' flag will recurse over all children, and is
 * useful for setting a property on a hierarchy-wide basis, regardless of any
 * local modifications for each dataset.
 */
static int
inherit_callback(zfs_handle_t *zhp, void *data)
{
	zfs_prop_t prop = (zfs_prop_t)data;

	return (zfs_prop_inherit(zhp, prop) != 0);
}

static int
zfs_do_inherit(int argc, char **argv)
{
	boolean_t recurse = B_FALSE;
	int c;
	zfs_prop_t prop;
	char *propname;

	/* check options */
	while ((c = getopt(argc, argv, "r")) != -1) {
		switch (c) {
		case 'r':
			recurse = B_TRUE;
			break;
		case '?':
		default:
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing property argument\n"));
		usage(B_FALSE);
	}
	if (argc < 2) {
		(void) fprintf(stderr, gettext("missing dataset argument\n"));
		usage(B_FALSE);
	}

	propname = argv[0];

	/*
	 * Get and validate the property before iterating over the datasets.  We
	 * do this now so as to avoid printing out an error message for each and
	 * every dataset.
	 */
	if ((prop = zfs_name_to_prop(propname)) == ZFS_PROP_INVAL) {
		(void) fprintf(stderr, gettext("invalid property '%s'\n"),
		    propname);
		usage(B_FALSE);
	}
	if (zfs_prop_readonly(prop)) {
		(void) fprintf(stderr, gettext("%s property is read-only\n"),
		    propname);
		return (1);
	}
	if (!zfs_prop_inheritable(prop)) {
		(void) fprintf(stderr, gettext("%s property cannot be "
		    "inherited\n"), propname);
		(void) fprintf(stderr, gettext("use 'zfs set %s=none' to "
		    "clear\n"), propname);
		return (1);
	}

	return (zfs_for_each(argc - 1, argv + 1, recurse,
	    ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME, NULL,
	    inherit_callback, (void *)prop));
}

/*
 * list [-rH] [-o property[,property]...] [-t type[,type]...]
 *      [-s property [-s property]...] [-S property [-S property]...]
 *      <dataset> ...
 *
 * 	-r	Recurse over all children
 * 	-H	Scripted mode; elide headers and separate colums by tabs
 * 	-o	Control which fields to display.
 * 	-t	Control which object types to display.
 *	-s	Specify sort columns, descending order.
 *	-S	Specify sort columns, ascending order.
 *
 * When given no arguments, lists all filesystems in the system.
 * Otherwise, list the specified datasets, optionally recursing down them if
 * '-r' is specified.
 */
typedef struct list_cbdata {
	boolean_t	cb_first;
	boolean_t	cb_scripted;
	zfs_prop_t	cb_fields[ZFS_NPROP_ALL];
	int		cb_fieldcount;
} list_cbdata_t;

/*
 * Given a list of columns to display, output appropriate headers for each one.
 */
static void
print_header(zfs_prop_t *fields, size_t count)
{
	int i;

	for (i = 0; i < count; i++) {
		if (i != 0)
			(void) printf("  ");
		if (i == count - 1)
			(void) printf("%s", zfs_prop_column_name(fields[i]));
		else	/* LINTED - format specifier */
			(void) printf(zfs_prop_column_format(fields[i]),
			    zfs_prop_column_name(fields[i]));
	}

	(void) printf("\n");
}

/*
 * Given a dataset and a list of fields, print out all the properties according
 * to the described layout.
 */
static void
print_dataset(zfs_handle_t *zhp, zfs_prop_t *fields, size_t count, int scripted)
{
	int i;
	char property[ZFS_MAXPROPLEN];

	for (i = 0; i < count; i++) {
		if (i != 0) {
			if (scripted)
				(void) printf("\t");
			else
				(void) printf("  ");
		}

		if (zfs_prop_get(zhp, fields[i], property,
		    sizeof (property), NULL, NULL, 0, B_FALSE) != 0)
			(void) strlcpy(property, "-", sizeof (property));

		/*
		 * If this is being called in scripted mode, or if this is the
		 * last column and it is left-justified, don't include a width
		 * format specifier.
		 */
		if (scripted || (i == count - 1 &&
		    strchr(zfs_prop_column_format(fields[i]), '-') != NULL))
			(void) printf("%s", property);
		else	/* LINTED - format specifier */
			(void) printf(zfs_prop_column_format(fields[i]),
			    property);
	}

	(void) printf("\n");
}

/*
 * Generic callback function to list a dataset or snapshot.
 */
static int
list_callback(zfs_handle_t *zhp, void *data)
{
	list_cbdata_t *cbp = data;

	if (cbp->cb_first) {
		if (!cbp->cb_scripted)
			print_header(cbp->cb_fields, cbp->cb_fieldcount);
		cbp->cb_first = B_FALSE;
	}

	print_dataset(zhp, cbp->cb_fields, cbp->cb_fieldcount,
	    cbp->cb_scripted);

	return (0);
}

static int
zfs_do_list(int argc, char **argv)
{
	int c;
	boolean_t recurse = B_FALSE;
	boolean_t scripted = B_FALSE;
	static char default_fields[] =
	    "name,used,available,referenced,mountpoint";
	int types = ZFS_TYPE_ANY;
	char *fields = NULL;
	char *basic_fields = default_fields;
	list_cbdata_t cb = { 0 };
	char *value;
	int ret;
	char *type_subopts[] = { "filesystem", "volume", "snapshot", NULL };
	char *badopt;
	int alloffset;
	zfs_sort_column_t *sortcol = NULL;

	/* check options */
	while ((c = getopt(argc, argv, ":o:rt:Hs:S:")) != -1) {
		zfs_prop_t	prop;

		switch (c) {
		case 'o':
			fields = optarg;
			break;
		case 'r':
			recurse = B_TRUE;
			break;
		case 'H':
			scripted = B_TRUE;
			break;
		case 's':
			if ((prop = zfs_name_to_prop(optarg)) ==
			    ZFS_PROP_INVAL) {
				(void) fprintf(stderr,
				    gettext("invalid property '%s'\n"), optarg);
				usage(B_FALSE);
			}
			zfs_add_sort_column(&sortcol, prop, B_FALSE);
			break;
		case 'S':
			if ((prop = zfs_name_to_prop(optarg)) ==
			    ZFS_PROP_INVAL) {
				(void) fprintf(stderr,
				    gettext("invalid property '%s'\n"), optarg);
				usage(B_FALSE);
			}
			zfs_add_sort_column(&sortcol, prop, B_TRUE);
			break;
		case 't':
			types = 0;
			while (*optarg != '\0') {
				switch (getsubopt(&optarg, type_subopts,
				    &value)) {
				case 0:
					types |= ZFS_TYPE_FILESYSTEM;
					break;
				case 1:
					types |= ZFS_TYPE_VOLUME;
					break;
				case 2:
					types |= ZFS_TYPE_SNAPSHOT;
					break;
				default:
					(void) fprintf(stderr,
					    gettext("invalid type '%s'\n"),
					    value);
					usage(B_FALSE);
				}
			}
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing argument for "
			    "'%c' option\n"), optopt);
			usage(B_FALSE);
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	if (fields == NULL)
		fields = basic_fields;

	/*
	 * If the user specifies '-o all', the zfs_get_proplist() doesn't
	 * normally include the name of the dataset.  For 'zfs list', we always
	 * want this property to be first.
	 */
	if (strcmp(fields, "all") == 0) {
		cb.cb_fields[0] = ZFS_PROP_NAME;
		alloffset = 1;
	} else {
		alloffset = 0;
	}

	if ((ret = zfs_get_proplist(fields, cb.cb_fields + alloffset,
	    ZFS_NPROP_ALL - alloffset, &cb.cb_fieldcount, &badopt)) != 0) {
		if (ret == EINVAL)
			(void) fprintf(stderr, gettext("invalid property "
			    "'%s'\n"), badopt);
		else
			(void) fprintf(stderr, gettext("too many properties "
			    "specified\n"));
		usage(B_FALSE);
	}

	cb.cb_fieldcount += alloffset;
	cb.cb_scripted = scripted;
	cb.cb_first = B_TRUE;

	ret = zfs_for_each(argc, argv, recurse, types, sortcol,
	    list_callback, &cb);

	zfs_free_sort_columns(sortcol);

	if (ret == 0 && cb.cb_first)
		(void) printf(gettext("no datasets available\n"));

	return (ret);
}

/*
 * zfs rename <fs | snap | vol> <fs | snap | vol>
 *
 * Renames the given dataset to another of the same type.
 */
/* ARGSUSED */
static int
zfs_do_rename(int argc, char **argv)
{
	zfs_handle_t *zhp;
	int ret;

	/* check options */
	if (argc > 1 && argv[1][0] == '-') {
		(void) fprintf(stderr, gettext("invalid option '%c'\n"),
		    argv[1][1]);
		usage(B_FALSE);
	}

	/* check number of arguments */
	if (argc < 2) {
		(void) fprintf(stderr, gettext("missing source dataset "
		    "argument\n"));
		usage(B_FALSE);
	}
	if (argc < 3) {
		(void) fprintf(stderr, gettext("missing target dataset "
		    "argument\n"));
		usage(B_FALSE);
	}
	if (argc > 3) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	if ((zhp = zfs_open(g_zfs, argv[1], ZFS_TYPE_ANY)) == NULL)
		return (1);

	ret = (zfs_rename(zhp, argv[2]) != 0);

	zfs_close(zhp);
	return (ret);
}

/*
 * zfs promote <fs>
 *
 * Promotes the given clone fs to be the parent
 */
/* ARGSUSED */
static int
zfs_do_promote(int argc, char **argv)
{
	zfs_handle_t *zhp;
	int ret;

	/* check options */
	if (argc > 1 && argv[1][0] == '-') {
		(void) fprintf(stderr, gettext("invalid option '%c'\n"),
		    argv[1][1]);
		usage(B_FALSE);
	}

	/* check number of arguments */
	if (argc < 2) {
		(void) fprintf(stderr, gettext("missing clone filesystem"
		    "argument\n"));
		usage(B_FALSE);
	}
	if (argc > 2) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	zhp = zfs_open(g_zfs, argv[1], ZFS_TYPE_FILESYSTEM | ZFS_TYPE_VOLUME);
	if (zhp == NULL)
		return (1);

	ret = (zfs_promote(zhp) != 0);

	zfs_close(zhp);
	return (ret);
}

/*
 * zfs rollback [-rfR] <snapshot>
 *
 * 	-r	Delete any intervening snapshots before doing rollback
 * 	-R	Delete any snapshots and their clones
 * 	-f	Force unmount filesystems, even if they are in use.
 *
 * Given a filesystem, rollback to a specific snapshot, discarding any changes
 * since then and making it the active dataset.  If more recent snapshots exist,
 * the command will complain unless the '-r' flag is given.
 */
typedef struct rollback_cbdata {
	uint64_t	cb_create;
	boolean_t	cb_first;
	int		cb_doclones;
	char		*cb_target;
	int		cb_error;
	boolean_t	cb_recurse;
	boolean_t	cb_dependent;
} rollback_cbdata_t;

/*
 * Report any snapshots more recent than the one specified.  Used when '-r' is
 * not specified.  We reuse this same callback for the snapshot dependents - if
 * 'cb_dependent' is set, then this is a dependent and we should report it
 * without checking the transaction group.
 */
static int
rollback_check(zfs_handle_t *zhp, void *data)
{
	rollback_cbdata_t *cbp = data;

	if (cbp->cb_doclones) {
		zfs_close(zhp);
		return (0);
	}

	if (!cbp->cb_dependent) {
		if (strcmp(zfs_get_name(zhp), cbp->cb_target) != 0 &&
		    zfs_get_type(zhp) == ZFS_TYPE_SNAPSHOT &&
		    zfs_prop_get_int(zhp, ZFS_PROP_CREATETXG) >
		    cbp->cb_create) {

			if (cbp->cb_first && !cbp->cb_recurse) {
				(void) fprintf(stderr, gettext("cannot "
				    "rollback to '%s': more recent snapshots "
				    "exist\n"),
				    cbp->cb_target);
				(void) fprintf(stderr, gettext("use '-r' to "
				    "force deletion of the following "
				    "snapshots:\n"));
				cbp->cb_first = 0;
				cbp->cb_error = 1;
			}

			if (cbp->cb_recurse) {
				cbp->cb_dependent = B_TRUE;
				if (zfs_iter_dependents(zhp, B_TRUE,
				    rollback_check, cbp) != 0) {
					zfs_close(zhp);
					return (-1);
				}
				cbp->cb_dependent = B_FALSE;
			} else {
				(void) fprintf(stderr, "%s\n",
				    zfs_get_name(zhp));
			}
		}
	} else {
		if (cbp->cb_first && cbp->cb_recurse) {
			(void) fprintf(stderr, gettext("cannot rollback to "
			    "'%s': clones of previous snapshots exist\n"),
			    cbp->cb_target);
			(void) fprintf(stderr, gettext("use '-R' to "
			    "force deletion of the following clones and "
			    "dependents:\n"));
			cbp->cb_first = 0;
			cbp->cb_error = 1;
		}

		(void) fprintf(stderr, "%s\n", zfs_get_name(zhp));
	}

	zfs_close(zhp);
	return (0);
}

static int
zfs_do_rollback(int argc, char **argv)
{
	int ret;
	int c;
	rollback_cbdata_t cb = { 0 };
	zfs_handle_t *zhp, *snap;
	char parentname[ZFS_MAXNAMELEN];
	char *delim;
	int force = 0;

	/* check options */
	while ((c = getopt(argc, argv, "rfR")) != -1) {
		switch (c) {
		case 'f':
			force = 1;
			break;
		case 'r':
			cb.cb_recurse = 1;
			break;
		case 'R':
			cb.cb_recurse = 1;
			cb.cb_doclones = 1;
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing dataset argument\n"));
		usage(B_FALSE);
	}
	if (argc > 1) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	/* open the snapshot */
	if ((snap = zfs_open(g_zfs, argv[0], ZFS_TYPE_SNAPSHOT)) == NULL)
		return (1);

	/* open the parent dataset */
	(void) strlcpy(parentname, argv[0], sizeof (parentname));
	verify((delim = strrchr(parentname, '@')) != NULL);
	*delim = '\0';
	if ((zhp = zfs_open(g_zfs, parentname, ZFS_TYPE_ANY)) == NULL) {
		zfs_close(snap);
		return (1);
	}

	/*
	 * Check for more recent snapshots and/or clones based on the presence
	 * of '-r' and '-R'.
	 */
	cb.cb_target = argv[0];
	cb.cb_create = zfs_prop_get_int(snap, ZFS_PROP_CREATETXG);
	cb.cb_first = B_TRUE;
	cb.cb_error = 0;
	if ((ret = zfs_iter_children(zhp, rollback_check, &cb)) != 0)
		goto out;

	if ((ret = cb.cb_error) != 0)
		goto out;

	/*
	 * Rollback parent to the given snapshot.
	 */
	ret = zfs_rollback(zhp, snap, force);

out:
	zfs_close(snap);
	zfs_close(zhp);

	if (ret == 0)
		return (0);
	else
		return (1);
}

/*
 * zfs set property=value { fs | snap | vol } ...
 *
 * Sets the given property for all datasets specified on the command line.
 */
typedef struct set_cbdata {
	char		*cb_propname;
	char		*cb_value;
	zfs_prop_t	cb_prop;
} set_cbdata_t;

static int
set_callback(zfs_handle_t *zhp, void *data)
{
	set_cbdata_t *cbp = data;
	int ret = 1;

	/* don't allow setting of properties for snapshots */
	if (zfs_get_type(zhp) == ZFS_TYPE_SNAPSHOT) {
		(void) fprintf(stderr, gettext("cannot set %s property for "
		    "'%s': snapshot properties cannot be modified\n"),
		    cbp->cb_propname, zfs_get_name(zhp));
		return (1);
	}

	/*
	 * If we're changing the volsize, make sure the value is appropriate,
	 * and set the reservation if this is a non-sparse volume.
	 */
	if (cbp->cb_prop == ZFS_PROP_VOLSIZE &&
	    zfs_get_type(zhp) == ZFS_TYPE_VOLUME) {
		uint64_t volsize = zfs_prop_get_int(zhp, ZFS_PROP_VOLSIZE);
		uint64_t avail = zfs_prop_get_int(zhp, ZFS_PROP_AVAILABLE);
		uint64_t reservation = zfs_prop_get_int(zhp,
		    ZFS_PROP_RESERVATION);
		uint64_t blocksize = zfs_prop_get_int(zhp,
		    ZFS_PROP_VOLBLOCKSIZE);
		uint64_t value;

		verify(zfs_nicestrtonum(cbp->cb_value, &value) == 0);

		if (value % blocksize != 0) {
			char buf[64];

			zfs_nicenum(blocksize, buf, sizeof (buf));
			(void) fprintf(stderr, gettext("cannot set %s for "
			    "'%s': must be a multiple of volume block size "
			    "(%s)\n"), cbp->cb_propname, zfs_get_name(zhp),
			    buf);
			return (1);
		}

		if (value == 0) {
			(void) fprintf(stderr, gettext("cannot set %s for "
			    "'%s': cannot be zero\n"), cbp->cb_propname,
			    zfs_get_name(zhp));
			return (1);
		}

		if (volsize == reservation) {
			if (value > volsize && (value - volsize) > avail) {
				(void) fprintf(stderr, gettext("cannot set "
				    "%s property for '%s': volume size exceeds "
				    "amount of available space\n"),
				    cbp->cb_propname, zfs_get_name(zhp));
				return (1);
			}

			if (zfs_prop_set(zhp, ZFS_PROP_RESERVATION,
			    cbp->cb_value) != 0) {
				(void) fprintf(stderr, gettext("volsize and "
				    "reservation must remain equal\n"));
				return (1);
			}
		}
	}

	/*
	 * Do not allow the reservation to be set above the volume size. We do
	 * this here instead of inside libzfs because libzfs violates this rule
	 * internally.
	 */
	if (cbp->cb_prop == ZFS_PROP_RESERVATION &&
	    zfs_get_type(zhp) == ZFS_TYPE_VOLUME) {
		uint64_t value;
		uint64_t volsize;

		volsize = zfs_prop_get_int(zhp, ZFS_PROP_VOLSIZE);
		if (strcmp(cbp->cb_value, "none") == 0)
			value = 0;
		else
			verify(zfs_nicestrtonum(cbp->cb_value, &value) == 0);

		if (value > volsize) {
			(void) fprintf(stderr, gettext("cannot set %s "
			    "for '%s': size is greater than current "
			    "volume size\n"), cbp->cb_propname,
			    zfs_get_name(zhp));
			return (-1);
		}
	}

	if (zfs_prop_set(zhp, cbp->cb_prop, cbp->cb_value) != 0) {
		switch (libzfs_errno(g_zfs)) {
		case EZFS_MOUNTFAILED:
			(void) fprintf(stderr, gettext("property may be set "
			    "but unable to remount filesystem\n"));
			break;
		case EZFS_SHAREFAILED:
			(void) fprintf(stderr, gettext("property may be set "
			    "but unable to reshare filesystem\n"));
			break;
		}
		return (1);
	}
	ret = 0;
error:
	return (ret);
}

static int
zfs_do_set(int argc, char **argv)
{
	set_cbdata_t cb;

	/* check for options */
	if (argc > 1 && argv[1][0] == '-') {
		(void) fprintf(stderr, gettext("invalid option '%c'\n"),
		    argv[1][1]);
		usage(B_FALSE);
	}

	/* check number of arguments */
	if (argc < 2) {
		(void) fprintf(stderr, gettext("missing property=value "
		    "argument\n"));
		usage(B_FALSE);
	}
	if (argc < 3) {
		(void) fprintf(stderr, gettext("missing dataset name\n"));
		usage(B_FALSE);
	}

	/* validate property=value argument */
	cb.cb_propname = argv[1];
	if ((cb.cb_value = strchr(cb.cb_propname, '=')) == NULL) {
		(void) fprintf(stderr, gettext("missing value in "
		    "property=value argument\n"));
		usage(B_FALSE);
	}

	*cb.cb_value = '\0';
	cb.cb_value++;

	if (*cb.cb_propname == '\0') {
		(void) fprintf(stderr,
		    gettext("missing property in property=value argument\n"));
		usage(B_FALSE);
	}
	if (*cb.cb_value == '\0') {
		(void) fprintf(stderr,
		    gettext("missing value in property=value argument\n"));
		usage(B_FALSE);
	}

	/* get the property type */
	if ((cb.cb_prop = zfs_name_to_prop(cb.cb_propname)) ==
	    ZFS_PROP_INVAL) {
		(void) fprintf(stderr,
		    gettext("invalid property '%s'\n"), cb.cb_propname);
		usage(B_FALSE);
	}

	/*
	 * Validate that the value is appropriate for this property.  We do this
	 * once now so we don't generate multiple errors each time we try to
	 * apply it to a dataset.
	 */
	if (zfs_prop_validate(g_zfs, cb.cb_prop, cb.cb_value, NULL) != 0)
		return (1);

	return (zfs_for_each(argc - 2, argv + 2, B_FALSE,
	    ZFS_TYPE_ANY, NULL, set_callback, &cb));
}

/*
 * zfs snapshot [-r] <fs@snap>
 *
 * Creates a snapshot with the given name.  While functionally equivalent to
 * 'zfs create', it is a separate command to diffferentiate intent.
 */
static int
zfs_do_snapshot(int argc, char **argv)
{
	int recursive = B_FALSE;
	int ret;
	char c;

	/* check options */
	while ((c = getopt(argc, argv, ":r")) != -1) {
		switch (c) {
		case 'r':
			recursive = B_TRUE;
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing snapshot argument\n"));
		usage(B_FALSE);
	}
	if (argc > 1) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	ret = zfs_snapshot(g_zfs, argv[0], recursive);
	if (ret && recursive)
		(void) fprintf(stderr, gettext("no snapshots were created\n"));
	return (ret != 0);

}

/*
 * zfs send [-i <fs@snap>] <fs@snap>
 *
 * Send a backup stream to stdout.
 */
static int
zfs_do_send(int argc, char **argv)
{
	char *fromname = NULL;
	zfs_handle_t *zhp_from = NULL, *zhp_to;
	int c, err;

	/* check options */
	while ((c = getopt(argc, argv, ":i:")) != -1) {
		switch (c) {
		case 'i':
			fromname = optarg;
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing argument for "
			    "'%c' option\n"), optopt);
			usage(B_FALSE);
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing snapshot argument\n"));
		usage(B_FALSE);
	}
	if (argc > 1) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	if (isatty(STDOUT_FILENO)) {
		(void) fprintf(stderr,
		    gettext("Error: Stream can not be written "
			    "to a terminal.\n"
			    "You must redirect standard output.\n"));
		return (1);
	}

	if (fromname) {
		if ((zhp_from = zfs_open(g_zfs, fromname,
		    ZFS_TYPE_SNAPSHOT)) == NULL)
			return (1);
	}
	if ((zhp_to = zfs_open(g_zfs, argv[0], ZFS_TYPE_SNAPSHOT)) == NULL)
		return (1);

	err = zfs_send(zhp_to, zhp_from);

	if (zhp_from)
		zfs_close(zhp_from);
	zfs_close(zhp_to);

	return (err != 0);
}

/*
 * zfs receive <fs@snap>
 *
 * Restore a backup stream from stdin.
 */
static int
zfs_do_receive(int argc, char **argv)
{
	int c, err;
	boolean_t isprefix = B_FALSE;
	boolean_t dryrun = B_FALSE;
	boolean_t verbose = B_FALSE;

	/* check options */
	while ((c = getopt(argc, argv, ":dnv")) != -1) {
		switch (c) {
		case 'd':
			isprefix = B_TRUE;
			break;
		case 'n':
			dryrun = B_TRUE;
			break;
		case 'v':
			verbose = B_TRUE;
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing argument for "
			    "'%c' option\n"), optopt);
			usage(B_FALSE);
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (argc < 1) {
		(void) fprintf(stderr, gettext("missing snapshot argument\n"));
		usage(B_FALSE);
	}
	if (argc > 1) {
		(void) fprintf(stderr, gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	if (isatty(STDIN_FILENO)) {
		(void) fprintf(stderr,
		    gettext("Error: Backup stream can not be read "
			    "from a terminal.\n"
			    "You must redirect standard input.\n"));
		return (1);
	}

	err = zfs_receive(g_zfs, argv[0], isprefix, verbose, dryrun);
	return (err != 0);
}

typedef struct get_all_cbdata {
	zfs_handle_t	**cb_handles;
	size_t		cb_alloc;
	size_t		cb_used;
} get_all_cbdata_t;

static int
get_one_filesystem(zfs_handle_t *zhp, void *data)
{
	get_all_cbdata_t *cbp = data;

	/*
	 * Skip any zvols
	 */
	if (zfs_get_type(zhp) != ZFS_TYPE_FILESYSTEM) {
		zfs_close(zhp);
		return (0);
	}

	if (cbp->cb_alloc == cbp->cb_used) {
		zfs_handle_t **handles;

		if (cbp->cb_alloc == 0)
			cbp->cb_alloc = 64;
		else
			cbp->cb_alloc *= 2;

		handles = safe_malloc(cbp->cb_alloc * sizeof (void *));

		if (cbp->cb_handles) {
			bcopy(cbp->cb_handles, handles,
			    cbp->cb_used * sizeof (void *));
			free(cbp->cb_handles);
		}

		cbp->cb_handles = handles;
	}

	cbp->cb_handles[cbp->cb_used++] = zhp;

	return (zfs_iter_filesystems(zhp, get_one_filesystem, data));
}

static void
get_all_filesystems(zfs_handle_t ***fslist, size_t *count)
{
	get_all_cbdata_t cb = { 0 };

	(void) zfs_iter_root(g_zfs, get_one_filesystem, &cb);

	*fslist = cb.cb_handles;
	*count = cb.cb_used;
}

static int
mountpoint_compare(const void *a, const void *b)
{
	zfs_handle_t **za = (zfs_handle_t **)a;
	zfs_handle_t **zb = (zfs_handle_t **)b;
	char mounta[MAXPATHLEN];
	char mountb[MAXPATHLEN];

	verify(zfs_prop_get(*za, ZFS_PROP_MOUNTPOINT, mounta,
	    sizeof (mounta), NULL, NULL, 0, B_FALSE) == 0);
	verify(zfs_prop_get(*zb, ZFS_PROP_MOUNTPOINT, mountb,
	    sizeof (mountb), NULL, NULL, 0, B_FALSE) == 0);

	return (strcmp(mounta, mountb));
}

/*
 * Generic callback for sharing or mounting filesystems.  Because the code is so
 * similar, we have a common function with an extra parameter to determine which
 * mode we are using.
 */
#define	OP_SHARE	0x1
#define	OP_MOUNT	0x2

typedef struct share_mount_cbdata {
	int	cb_type;
	int	cb_explicit;
	int	cb_flags;
	const char *cb_options;
} share_mount_cbdata_t;

/*
 * Share or mount the filesystem.
 */
static int
share_mount_callback(zfs_handle_t *zhp, void *data)
{
	char mountpoint[ZFS_MAXPROPLEN];
	char shareopts[ZFS_MAXPROPLEN];
	share_mount_cbdata_t *cbp = data;
	const char *cmdname = cbp->cb_type == OP_SHARE ? "share" : "mount";
	struct mnttab mnt;
	uint64_t zoned;

	if (cbp->cb_options == NULL)
		mnt.mnt_mntopts = "";
	else
		mnt.mnt_mntopts = (char *)cbp->cb_options;

	/*
	 * Check to make sure we can mount/share this dataset.  If we are in the
	 * global zone and the filesystem is exported to a local zone, or if we
	 * are in a local zone and the filesystem is not exported, then it is an
	 * error.
	 */
	zoned = zfs_prop_get_int(zhp, ZFS_PROP_ZONED);

	if (zoned && getzoneid() == GLOBAL_ZONEID) {
		if (!cbp->cb_explicit)
			return (0);

		(void) fprintf(stderr, gettext("cannot %s '%s': dataset is "
		    "exported to a local zone\n"), cmdname, zfs_get_name(zhp));
		return (1);

	} else if (!zoned && getzoneid() != GLOBAL_ZONEID) {
		if (!cbp->cb_explicit)
			return (0);

		(void) fprintf(stderr, gettext("cannot %s '%s': permission "
		    "denied\n"), cmdname, zfs_get_name(zhp));
		return (1);
	}

	/*
	 * Inore any filesystems which don't apply to us.  This includes those
	 * with a legacy mountpoint, or those with legacy share options.
	 */
	verify(zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, mountpoint,
	    sizeof (mountpoint), NULL, NULL, 0, B_FALSE) == 0);
	verify(zfs_prop_get(zhp, ZFS_PROP_SHARENFS, shareopts,
	    sizeof (shareopts), NULL, NULL, 0, B_FALSE) == 0);

	if (cbp->cb_type == OP_SHARE) {
		if (strcmp(shareopts, "off") == 0) {
			if (!cbp->cb_explicit)
				return (0);

			(void) fprintf(stderr, gettext("cannot share '%s': "
			    "legacy share\n"), zfs_get_name(zhp));
			(void) fprintf(stderr, gettext("use share(1M) to "
			    "share this filesystem\n"));
			return (1);
		}
	}

	/*
	 * We cannot share or mount legacy filesystems.  If the shareopts is
	 * non-legacy but the mountpoint is legacy, we treat it as a legacy
	 * share.
	 */
	if (strcmp(mountpoint, "legacy") == 0) {
		if (!cbp->cb_explicit)
			return (0);

		(void) fprintf(stderr, gettext("cannot %s '%s': "
		    "legacy mountpoint\n"), cmdname, zfs_get_name(zhp));
		(void) fprintf(stderr, gettext("use %s to "
		    "%s this filesystem\n"), cbp->cb_type == OP_SHARE ?
		    "share(1M)" : "mount(1M)", cmdname);
		return (1);
	}

	if (strcmp(mountpoint, "none") == 0) {
		if (!cbp->cb_explicit)
			return (0);

		(void) fprintf(stderr, gettext("cannot %s '%s': no "
		    "mountpoint set\n"), cmdname, zfs_get_name(zhp));
		return (1);
	}

	/*
	 * At this point, we have verified that the mountpoint and/or shareopts
	 * are appropriate for auto management.  Determine if the filesystem is
	 * currently mounted or shared, and abort if this is an explicit
	 * request.
	 */
	switch (cbp->cb_type) {
	case OP_SHARE:
		if (zfs_is_shared(zhp, NULL)) {
			if (cbp->cb_explicit) {
				(void) fprintf(stderr, gettext("cannot share "
				    "'%s': filesystem already shared\n"),
				    zfs_get_name(zhp));
				return (1);
			} else {
				return (0);
			}
		}
		break;

	case OP_MOUNT:
		if (!hasmntopt(&mnt, MNTOPT_REMOUNT) &&
		    zfs_is_mounted(zhp, NULL)) {
			if (cbp->cb_explicit) {
				(void) fprintf(stderr, gettext("cannot mount "
				    "'%s': filesystem already mounted\n"),
				    zfs_get_name(zhp));
				return (1);
			} else {
				return (0);
			}
		}
		break;
	}

	/*
	 * Mount and optionally share the filesystem.
	 */
	switch (cbp->cb_type) {
	case OP_SHARE:
		{
			if (!zfs_is_mounted(zhp, NULL) &&
			    zfs_mount(zhp, NULL, 0) != 0)
				return (1);

			if (zfs_share(zhp) != 0)
				return (1);
		}
		break;

	case OP_MOUNT:
		if (zfs_mount(zhp, cbp->cb_options, cbp->cb_flags) != 0)
			return (1);
		break;
	}

	return (0);
}

static int
share_or_mount(int type, int argc, char **argv)
{
	int do_all = 0;
	int c, ret = 0;
	share_mount_cbdata_t cb = { 0 };

	cb.cb_type = type;

	/* check options */
	while ((c = getopt(argc, argv, type == OP_MOUNT ? ":ao:O" : "a"))
	    != -1) {
		switch (c) {
		case 'a':
			do_all = 1;
			break;
		case 'o':
			cb.cb_options = optarg;
			break;
		case 'O':
			cb.cb_flags |= MS_OVERLAY;
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing argument for "
			    "'%c' option\n"), optopt);
			usage(B_FALSE);
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* check number of arguments */
	if (do_all) {
		zfs_handle_t **fslist = NULL;
		size_t i, count = 0;

		if (argc != 0) {
			(void) fprintf(stderr, gettext("too many arguments\n"));
			usage(B_FALSE);
		}

		get_all_filesystems(&fslist, &count);

		if (count == 0)
			return (0);

		qsort(fslist, count, sizeof (void *), mountpoint_compare);

		for (i = 0; i < count; i++) {
			if (share_mount_callback(fslist[i], &cb) != 0)
				ret = 1;
		}

		for (i = 0; i < count; i++)
			zfs_close(fslist[i]);

		free(fslist);
	} else if (argc == 0) {
		struct mnttab entry;

		if (type == OP_SHARE) {
			(void) fprintf(stderr, gettext("missing filesystem "
			    "argument\n"));
			usage(B_FALSE);
		}

		/*
		 * When mount is given no arguments, go through /etc/mnttab and
		 * display any active ZFS mounts.  We hide any snapshots, since
		 * they are controlled automatically.
		 */
		rewind(mnttab_file);
		while (getmntent(mnttab_file, &entry) == 0) {
			if (strcmp(entry.mnt_fstype, MNTTYPE_ZFS) != 0 ||
			    strchr(entry.mnt_special, '@') != NULL)
				continue;

			(void) printf("%-30s  %s\n", entry.mnt_special,
			    entry.mnt_mountp);
		}

	} else {
		zfs_handle_t *zhp;

		if (argc > 1) {
			(void) fprintf(stderr,
			    gettext("too many arguments\n"));
			usage(B_FALSE);
		}

		if ((zhp = zfs_open(g_zfs, argv[0],
		    ZFS_TYPE_FILESYSTEM)) == NULL)
			ret = 1;
		else {
			cb.cb_explicit = B_TRUE;
			ret = share_mount_callback(zhp, &cb);
			zfs_close(zhp);
		}
	}

	return (ret);
}

/*
 * zfs mount -a
 * zfs mount filesystem
 *
 * Mount all filesystems, or mount the given filesystem.
 */
static int
zfs_do_mount(int argc, char **argv)
{
	return (share_or_mount(OP_MOUNT, argc, argv));
}

/*
 * zfs share -a
 * zfs share filesystem
 *
 * Share all filesystems, or share the given filesystem.
 */
static int
zfs_do_share(int argc, char **argv)
{
	return (share_or_mount(OP_SHARE, argc, argv));
}

typedef struct unshare_unmount_node {
	zfs_handle_t	*un_zhp;
	char		*un_mountp;
	uu_avl_node_t	un_avlnode;
} unshare_unmount_node_t;

/* ARGSUSED */
static int
unshare_unmount_compare(const void *larg, const void *rarg, void *unused)
{
	const unshare_unmount_node_t *l = larg;
	const unshare_unmount_node_t *r = rarg;

	return (strcmp(l->un_mountp, r->un_mountp));
}

/*
 * Convenience routine used by zfs_do_umount() and manual_unmount().  Given an
 * absolute path, find the entry /etc/mnttab, verify that its a ZFS filesystem,
 * and unmount it appropriately.
 */
static int
unshare_unmount_path(int type, char *path, int flags, boolean_t is_manual)
{
	zfs_handle_t *zhp;
	int ret;
	struct stat64 statbuf;
	struct extmnttab entry;
	const char *cmdname = (type == OP_SHARE) ? "unshare" : "unmount";
	char property[ZFS_MAXPROPLEN];

	/*
	 * Search for the path in /etc/mnttab.  Rather than looking for the
	 * specific path, which can be fooled by non-standard paths (i.e. ".."
	 * or "//"), we stat() the path and search for the corresponding
	 * (major,minor) device pair.
	 */
	if (stat64(path, &statbuf) != 0) {
		(void) fprintf(stderr, gettext("cannot %s '%s': %s\n"),
		    cmdname, path, strerror(errno));
		return (1);
	}

	/*
	 * Search for the given (major,minor) pair in the mount table.
	 */
	rewind(mnttab_file);
	while ((ret = getextmntent(mnttab_file, &entry, 0)) == 0) {
		if (entry.mnt_major == major(statbuf.st_dev) &&
		    entry.mnt_minor == minor(statbuf.st_dev))
			break;
	}
	if (ret != 0) {
		(void) fprintf(stderr, gettext("cannot %s '%s': not "
		    "currently mounted\n"), cmdname, path);
		return (1);
	}

	if (strcmp(entry.mnt_fstype, MNTTYPE_ZFS) != 0) {
		(void) fprintf(stderr, gettext("cannot %s '%s': not a ZFS "
		    "filesystem\n"), cmdname, path);
		return (1);
	}

	if ((zhp = zfs_open(g_zfs, entry.mnt_special,
	    ZFS_TYPE_FILESYSTEM)) == NULL)
		return (1);

	verify(zfs_prop_get(zhp, type == OP_SHARE ?
		ZFS_PROP_SHARENFS : ZFS_PROP_MOUNTPOINT, property,
		sizeof (property), NULL, NULL, 0, B_FALSE) == 0);

	if (type == OP_SHARE) {
		if (strcmp(property, "off") == 0) {
			(void) fprintf(stderr, gettext("cannot unshare "
			    "'%s': legacy share\n"), path);
			(void) fprintf(stderr, gettext("use "
			    "unshare(1M) to unshare this filesystem\n"));
			ret = 1;
		} else if (!zfs_is_shared(zhp, NULL)) {
			(void) fprintf(stderr, gettext("cannot unshare '%s': "
			    "not currently shared\n"), path);
			ret = 1;
		} else {
			ret = zfs_unshareall(zhp);
		}
	} else {
		if (is_manual) {
			ret = zfs_unmount(zhp, NULL, flags);
		} else if (strcmp(property, "legacy") == 0) {
			(void) fprintf(stderr, gettext("cannot unmount "
			    "'%s': legacy mountpoint\n"),
			    zfs_get_name(zhp));
			(void) fprintf(stderr, gettext("use umount(1M) "
			    "to unmount this filesystem\n"));
			ret = 1;
		} else {
			ret = zfs_unmountall(zhp, flags);
		}
	}

	zfs_close(zhp);

	return (ret != 0);
}

/*
 * Generic callback for unsharing or unmounting a filesystem.
 */
static int
unshare_unmount(int type, int argc, char **argv)
{
	int do_all = 0;
	int flags = 0;
	int ret = 0;
	int c;
	zfs_handle_t *zhp;
	char property[ZFS_MAXPROPLEN];

	/* check options */
	while ((c = getopt(argc, argv, type == OP_SHARE ? "a" : "af")) != -1) {
		switch (c) {
		case 'a':
			do_all = 1;
			break;
		case 'f':
			flags = MS_FORCE;
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			usage(B_FALSE);
		}
	}

	argc -= optind;
	argv += optind;

	/* ensure correct number of arguments */
	if (do_all) {
		if (argc != 0) {
			(void) fprintf(stderr, gettext("too many arguments\n"));
			usage(B_FALSE);
		}
	} else if (argc != 1) {
		if (argc == 0)
			(void) fprintf(stderr,
			    gettext("missing filesystem argument\n"));
		else
			(void) fprintf(stderr,
			    gettext("too many arguments\n"));
		usage(B_FALSE);
	}

	if (do_all) {
		/*
		 * We could make use of zfs_for_each() to walk all datasets in
		 * the system, but this would be very inefficient, especially
		 * since we would have to linearly search /etc/mnttab for each
		 * one.  Instead, do one pass through /etc/mnttab looking for
		 * zfs entries and call zfs_unmount() for each one.
		 *
		 * Things get a little tricky if the administrator has created
		 * mountpoints beneath other ZFS filesystems.  In this case, we
		 * have to unmount the deepest filesystems first.  To accomplish
		 * this, we place all the mountpoints in an AVL tree sorted by
		 * the special type (dataset name), and walk the result in
		 * reverse to make sure to get any snapshots first.
		 */
		struct mnttab entry;
		uu_avl_pool_t *pool;
		uu_avl_t *tree;
		unshare_unmount_node_t *node;
		uu_avl_index_t idx;
		uu_avl_walk_t *walk;

		if ((pool = uu_avl_pool_create("unmount_pool",
		    sizeof (unshare_unmount_node_t),
		    offsetof(unshare_unmount_node_t, un_avlnode),
		    unshare_unmount_compare,
		    UU_DEFAULT)) == NULL) {
			(void) fprintf(stderr, gettext("internal error: "
			    "out of memory\n"));
			exit(1);
		}

		if ((tree = uu_avl_create(pool, NULL, UU_DEFAULT)) == NULL) {
			(void) fprintf(stderr, gettext("internal error: "
			    "out of memory\n"));
			exit(1);
		}

		rewind(mnttab_file);
		while (getmntent(mnttab_file, &entry) == 0) {

			/* ignore non-ZFS entries */
			if (strcmp(entry.mnt_fstype, MNTTYPE_ZFS) != 0)
				continue;

			/* ignore snapshots */
			if (strchr(entry.mnt_special, '@') != NULL)
				continue;

			if ((zhp = zfs_open(g_zfs, entry.mnt_special,
			    ZFS_TYPE_FILESYSTEM)) == NULL) {
				ret = 1;
				continue;
			}

			verify(zfs_prop_get(zhp, type == OP_SHARE ?
			    ZFS_PROP_SHARENFS : ZFS_PROP_MOUNTPOINT,
			    property, sizeof (property), NULL, NULL,
			    0, B_FALSE) == 0);

			/* Ignore legacy mounts and shares */
			if ((type == OP_SHARE &&
			    strcmp(property, "off") == 0) ||
			    (type == OP_MOUNT &&
			    strcmp(property, "legacy") == 0)) {
				zfs_close(zhp);
				continue;
			}

			node = safe_malloc(sizeof (unshare_unmount_node_t));
			node->un_zhp = zhp;

			if ((node->un_mountp = strdup(entry.mnt_mountp)) ==
			    NULL) {
				(void) fprintf(stderr, gettext("internal error:"
				    " out of memory\n"));
				exit(1);
			}

			uu_avl_node_init(node, &node->un_avlnode, pool);

			if (uu_avl_find(tree, node, NULL, &idx) == NULL) {
				uu_avl_insert(tree, node, idx);
			} else {
				zfs_close(node->un_zhp);
				free(node->un_mountp);
				free(node);
			}
		}

		/*
		 * Walk the AVL tree in reverse, unmounting each filesystem and
		 * removing it from the AVL tree in the process.
		 */
		if ((walk = uu_avl_walk_start(tree,
		    UU_WALK_REVERSE | UU_WALK_ROBUST)) == NULL) {
			(void) fprintf(stderr,
			    gettext("internal error: out of memory"));
			exit(1);
		}

		while ((node = uu_avl_walk_next(walk)) != NULL) {
			uu_avl_remove(tree, node);

			switch (type) {
			case OP_SHARE:
				if (zfs_unshare(node->un_zhp,
				    node->un_mountp) != 0)
					ret = 1;
				break;

			case OP_MOUNT:
				if (zfs_unmount(node->un_zhp,
				    node->un_mountp, flags) != 0)
					ret = 1;
				break;
			}

			zfs_close(node->un_zhp);
			free(node->un_mountp);
			free(node);
		}

		uu_avl_walk_end(walk);
		uu_avl_destroy(tree);
		uu_avl_pool_destroy(pool);
	} else {
		/*
		 * We have an argument, but it may be a full path or a ZFS
		 * filesystem.  Pass full paths off to unmount_path() (shared by
		 * manual_unmount), otherwise open the filesystem and pass to
		 * zfs_unmount().
		 */
		if (argv[0][0] == '/')
			return (unshare_unmount_path(type, argv[0],
				flags, B_FALSE));

		if ((zhp = zfs_open(g_zfs, argv[0],
		    ZFS_TYPE_FILESYSTEM)) == NULL)
			return (1);

		verify(zfs_prop_get(zhp, type == OP_SHARE ?
		    ZFS_PROP_SHARENFS : ZFS_PROP_MOUNTPOINT, property,
		    sizeof (property), NULL, NULL, 0, B_FALSE) == 0);

		switch (type) {
		case OP_SHARE:
			if (strcmp(property, "off") == 0) {
				(void) fprintf(stderr, gettext("cannot unshare "
				    "'%s': legacy share\n"), zfs_get_name(zhp));
				(void) fprintf(stderr, gettext("use unshare(1M)"
				    " to unshare this filesystem\n"));
				ret = 1;
			} else if (!zfs_is_shared(zhp, NULL)) {
				(void) fprintf(stderr, gettext("cannot unshare "
				    "'%s': not currently shared\n"),
				    zfs_get_name(zhp));
				ret = 1;
			} else if (zfs_unshareall(zhp) != 0) {
				ret = 1;
			}
			break;

		case OP_MOUNT:
			if (strcmp(property, "legacy") == 0) {
				(void) fprintf(stderr, gettext("cannot unmount "
				    "'%s': legacy mountpoint\n"),
				    zfs_get_name(zhp));
				(void) fprintf(stderr, gettext("use umount(1M) "
				    "to unmount this filesystem\n"));
				ret = 1;
			} else if (!zfs_is_mounted(zhp, NULL)) {
				(void) fprintf(stderr, gettext("cannot unmount "
				    "'%s': not currently mounted\n"),
				    zfs_get_name(zhp));
				ret = 1;
			} else if (zfs_unmountall(zhp, flags) != 0) {
				ret = 1;
			}
		}

		zfs_close(zhp);
	}

	return (ret);
}

/*
 * zfs unmount -a
 * zfs unmount filesystem
 *
 * Unmount all filesystems, or a specific ZFS filesystem.
 */
static int
zfs_do_unmount(int argc, char **argv)
{
	return (unshare_unmount(OP_MOUNT, argc, argv));
}

/*
 * zfs unshare -a
 * zfs unshare filesystem
 *
 * Unshare all filesystems, or a specific ZFS filesystem.
 */
static int
zfs_do_unshare(int argc, char **argv)
{
	return (unshare_unmount(OP_SHARE, argc, argv));
}

/*
 * Called when invoked as /etc/fs/zfs/mount.  Do the mount if the mountpoint is
 * 'legacy'.  Otherwise, complain that use should be using 'zfs mount'.
 */
static int
manual_mount(int argc, char **argv)
{
	zfs_handle_t *zhp;
	char mountpoint[ZFS_MAXPROPLEN];
	char mntopts[MNT_LINE_MAX] = { '\0' };
	int ret;
	int c;
	int flags = 0;
	char *dataset, *path;

	/* check options */
	while ((c = getopt(argc, argv, ":mo:O")) != -1) {
		switch (c) {
		case 'o':
			(void) strlcpy(mntopts, optarg, sizeof (mntopts));
			break;
		case 'O':
			flags |= MS_OVERLAY;
			break;
		case 'm':
			flags |= MS_NOMNTTAB;
			break;
		case ':':
			(void) fprintf(stderr, gettext("missing argument for "
			    "'%c' option\n"), optopt);
			usage(B_FALSE);
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			(void) fprintf(stderr, gettext("usage: mount [-o opts] "
			    "<path>\n"));
			return (2);
		}
	}

	argc -= optind;
	argv += optind;

	/* check that we only have two arguments */
	if (argc != 2) {
		if (argc == 0)
			(void) fprintf(stderr, gettext("missing dataset "
			    "argument\n"));
		else if (argc == 1)
			(void) fprintf(stderr,
			    gettext("missing mountpoint argument\n"));
		else
			(void) fprintf(stderr, gettext("too many arguments\n"));
		(void) fprintf(stderr, "usage: mount <dataset> <mountpoint>\n");
		return (2);
	}

	dataset = argv[0];
	path = argv[1];

	/* try to open the dataset */
	if ((zhp = zfs_open(g_zfs, dataset, ZFS_TYPE_FILESYSTEM)) == NULL)
		return (1);

	(void) zfs_prop_get(zhp, ZFS_PROP_MOUNTPOINT, mountpoint,
	    sizeof (mountpoint), NULL, NULL, 0, B_FALSE);

	/* check for legacy mountpoint and complain appropriately */
	ret = 0;
	if (strcmp(mountpoint, ZFS_MOUNTPOINT_LEGACY) == 0) {
		if (mount(dataset, path, MS_OPTIONSTR | flags, MNTTYPE_ZFS,
		    NULL, 0, mntopts, sizeof (mntopts)) != 0) {
			(void) fprintf(stderr, gettext("mount failed: %s\n"),
			    strerror(errno));
			ret = 1;
		}
	} else {
		(void) fprintf(stderr, gettext("filesystem '%s' cannot be "
		    "mounted using 'mount -F zfs'\n"), dataset);
		(void) fprintf(stderr, gettext("Use 'zfs set mountpoint=%s' "
		    "instead.\n"), path);
		(void) fprintf(stderr, gettext("If you must use 'mount -F zfs' "
		    "or /etc/vfstab, use 'zfs set mountpoint=legacy'.\n"));
		(void) fprintf(stderr, gettext("See zfs(1M) for more "
		    "information.\n"));
		ret = 1;
	}

	return (ret);
}

/*
 * Called when invoked as /etc/fs/zfs/umount.  Unlike a manual mount, we allow
 * unmounts of non-legacy filesystems, as this is the dominant administrative
 * interface.
 */
static int
manual_unmount(int argc, char **argv)
{
	int flags = 0;
	int c;

	/* check options */
	while ((c = getopt(argc, argv, "f")) != -1) {
		switch (c) {
		case 'f':
			flags = MS_FORCE;
			break;
		case '?':
			(void) fprintf(stderr, gettext("invalid option '%c'\n"),
			    optopt);
			(void) fprintf(stderr, gettext("usage: unmount [-f] "
			    "<path>\n"));
			return (2);
		}
	}

	argc -= optind;
	argv += optind;

	/* check arguments */
	if (argc != 1) {
		if (argc == 0)
			(void) fprintf(stderr, gettext("missing path "
			    "argument\n"));
		else
			(void) fprintf(stderr, gettext("too many arguments\n"));
		(void) fprintf(stderr, gettext("usage: unmount [-f] <path>\n"));
		return (2);
	}

	return (unshare_unmount_path(OP_MOUNT, argv[0], flags, B_TRUE));
}

static int
volcheck(zpool_handle_t *zhp, void *data)
{
	int isinit = (int)data;

	if (isinit)
		return (zpool_create_zvol_links(zhp));
	else
		return (zpool_remove_zvol_links(zhp));
}

/*
 * Iterate over all pools in the system and either create or destroy /dev/zvol
 * links, depending on the value of 'isinit'.
 */
static int
do_volcheck(boolean_t isinit)
{
	return (zpool_iter(g_zfs, volcheck, (void *)isinit) ? 1 : 0);
}

int
main(int argc, char **argv)
{
	int ret;
	int i;
	char *progname;
	char *cmdname;

	(void) setlocale(LC_ALL, "");
	(void) textdomain(TEXT_DOMAIN);

	opterr = 0;

	if ((g_zfs = libzfs_init()) == NULL) {
		(void) fprintf(stderr, gettext("internal error: failed to "
		    "initialize ZFS library\n"));
		return (1);
	}

	libzfs_print_on_error(g_zfs, B_TRUE);

	if ((mnttab_file = fopen(MNTTAB, "r")) == NULL) {
		(void) fprintf(stderr, gettext("internal error: unable to "
		    "open %s\n"), MNTTAB);
		return (1);
	}

	/*
	 * This command also doubles as the /etc/fs mount and unmount program.
	 * Determine if we should take this behavior based on argv[0].
	 */
	progname = basename(argv[0]);
	if (strcmp(progname, "mount") == 0) {
		ret = manual_mount(argc, argv);
	} else if (strcmp(progname, "umount") == 0) {
		ret = manual_unmount(argc, argv);
	} else {
		/*
		 * Make sure the user has specified some command.
		 */
		if (argc < 2) {
			(void) fprintf(stderr, gettext("missing command\n"));
			usage(B_FALSE);
		}

		cmdname = argv[1];

		/*
		 * The 'umount' command is an alias for 'unmount'
		 */
		if (strcmp(cmdname, "umount") == 0)
			cmdname = "unmount";

		/*
		 * The 'recv' command is an alias for 'receive'
		 */
		if (strcmp(cmdname, "recv") == 0)
			cmdname = "receive";

		/*
		 * Special case '-?'
		 */
		if (strcmp(cmdname, "-?") == 0)
			usage(B_TRUE);

		/*
		 * 'volinit' and 'volfini' do not appear in the usage message,
		 * so we have to special case them here.
		 */
		if (strcmp(cmdname, "volinit") == 0)
			return (do_volcheck(B_TRUE));
		else if (strcmp(cmdname, "volfini") == 0)
			return (do_volcheck(B_FALSE));

		/*
		 * Run the appropriate command.
		 */
		for (i = 0; i < NCOMMAND; i++) {
			if (command_table[i].name == NULL)
				continue;

			if (strcmp(cmdname, command_table[i].name) == 0) {
				current_command = &command_table[i];
				ret = command_table[i].func(argc - 1, argv + 1);
				break;
			}
		}

		if (i == NCOMMAND) {
			(void) fprintf(stderr, gettext("unrecognized "
			    "command '%s'\n"), cmdname);
			usage(B_FALSE);
		}
	}

	(void) fclose(mnttab_file);

	libzfs_fini(g_zfs);

	/*
	 * The 'ZFS_ABORT' environment variable causes us to dump core on exit
	 * for the purposes of running ::findleaks.
	 */
	if (getenv("ZFS_ABORT") != NULL) {
		(void) printf("dumping core by request\n");
		abort();
	}

	return (ret);
}
