// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
#include "libxfs_priv.h"
#include "xfs_fs.h"
#include "xfs_shared.h"
#include "xfs_format.h"
#include "xfs_log_format.h"
#include "xfs_trans_resv.h"
#include "xfs_mount.h"
#include "xfs_defer.h"
#include "xfs_da_format.h"
#include "xfs_da_btree.h"
#include "xfs_attr_sf.h"
#include "xfs_inode.h"
#include "xfs_trans.h"
#include "xfs_bmap.h"
#include "xfs_bmap_btree.h"
#include "xfs_attr.h"
#include "xfs_attr_leaf.h"
#include "xfs_attr_remote.h"
#include "xfs_quota_defs.h"
#include "xfs_trans_space.h"
#include "xfs_trace.h"

struct kmem_cache		*xfs_attri_cache;
struct kmem_cache		*xfs_attrd_cache;

/*
 * xfs_attr.c
 *
 * Provide the external interfaces to manage attribute lists.
 */

/*========================================================================
 * Function prototypes for the kernel.
 *========================================================================*/

/*
 * Internal routines when attribute list fits inside the inode.
 */
STATIC int xfs_attr_shortform_addname(xfs_da_args_t *args);

/*
 * Internal routines when attribute list is one block.
 */
STATIC int xfs_attr_leaf_get(xfs_da_args_t *args);
STATIC int xfs_attr_leaf_removename(xfs_da_args_t *args);
STATIC int xfs_attr_leaf_hasname(struct xfs_da_args *args, struct xfs_buf **bp);
STATIC int xfs_attr_leaf_try_add(struct xfs_da_args *args, struct xfs_buf *bp);

/*
 * Internal routines when attribute list is more than one block.
 */
STATIC int xfs_attr_node_get(xfs_da_args_t *args);
STATIC void xfs_attr_restore_rmt_blk(struct xfs_da_args *args);
static int xfs_attr_node_try_addname(struct xfs_attr_item *attr);
STATIC int xfs_attr_node_addname_find_attr(struct xfs_attr_item *attr);
STATIC int xfs_attr_node_addname_clear_incomplete(struct xfs_attr_item *attr);
STATIC int xfs_attr_node_hasname(xfs_da_args_t *args,
				 struct xfs_da_state **state);
STATIC int xfs_attr_fillstate(xfs_da_state_t *state);
STATIC int xfs_attr_refillstate(xfs_da_state_t *state);
STATIC int xfs_attr_node_removename(struct xfs_da_args *args,
				    struct xfs_da_state *state);

int
xfs_inode_hasattr(
	struct xfs_inode	*ip)
{
	if (!XFS_IFORK_Q(ip) ||
	    (ip->i_afp->if_format == XFS_DINODE_FMT_EXTENTS &&
	     ip->i_afp->if_nextents == 0))
		return 0;
	return 1;
}

/*
 * Returns true if the there is exactly only block in the attr fork, in which
 * case the attribute fork consists of a single leaf block entry.
 */
bool
xfs_attr_is_leaf(
	struct xfs_inode	*ip)
{
	struct xfs_ifork	*ifp = ip->i_afp;
	struct xfs_iext_cursor	icur;
	struct xfs_bmbt_irec	imap;

	if (ifp->if_nextents != 1 || ifp->if_format != XFS_DINODE_FMT_EXTENTS)
		return false;

	xfs_iext_first(ifp, &icur);
	xfs_iext_get_extent(ifp, &icur, &imap);
	return imap.br_startoff == 0 && imap.br_blockcount == 1;
}

/*========================================================================
 * Overall external interface routines.
 *========================================================================*/

/*
 * Retrieve an extended attribute and its value.  Must have ilock.
 * Returns 0 on successful retrieval, otherwise an error.
 */
int
xfs_attr_get_ilocked(
	struct xfs_da_args	*args)
{
	ASSERT(xfs_isilocked(args->dp, XFS_ILOCK_SHARED | XFS_ILOCK_EXCL));

	if (!xfs_inode_hasattr(args->dp))
		return -ENOATTR;

	if (args->dp->i_afp->if_format == XFS_DINODE_FMT_LOCAL)
		return xfs_attr_shortform_getvalue(args);
	if (xfs_attr_is_leaf(args->dp))
		return xfs_attr_leaf_get(args);
	return xfs_attr_node_get(args);
}

/*
 * Retrieve an extended attribute by name, and its value if requested.
 *
 * If args->valuelen is zero, then the caller does not want the value, just an
 * indication whether the attribute exists and the size of the value if it
 * exists. The size is returned in args.valuelen.
 *
 * If args->value is NULL but args->valuelen is non-zero, allocate the buffer
 * for the value after existence of the attribute has been determined. The
 * caller always has to free args->value if it is set, no matter if this
 * function was successful or not.
 *
 * If the attribute is found, but exceeds the size limit set by the caller in
 * args->valuelen, return -ERANGE with the size of the attribute that was found
 * in args->valuelen.
 */
int
xfs_attr_get(
	struct xfs_da_args	*args)
{
	uint			lock_mode;
	int			error;

	XFS_STATS_INC(args->dp->i_mount, xs_attr_get);

	if (xfs_is_shutdown(args->dp->i_mount))
		return -EIO;

	args->geo = args->dp->i_mount->m_attr_geo;
	args->whichfork = XFS_ATTR_FORK;
	args->hashval = xfs_da_hashname(args->name, args->namelen);

	/* Entirely possible to look up a name which doesn't exist */
	args->op_flags = XFS_DA_OP_OKNOENT;

	lock_mode = xfs_ilock_attr_map_shared(args->dp);
	error = xfs_attr_get_ilocked(args);
	xfs_iunlock(args->dp, lock_mode);

	return error;
}

/*
 * Calculate how many blocks we need for the new attribute,
 */
int
xfs_attr_calc_size(
	struct xfs_da_args	*args,
	int			*local)
{
	struct xfs_mount	*mp = args->dp->i_mount;
	int			size;
	int			nblks;

	/*
	 * Determine space new attribute will use, and if it would be
	 * "local" or "remote" (note: local != inline).
	 */
	size = xfs_attr_leaf_newentsize(args, local);
	nblks = XFS_DAENTER_SPACE_RES(mp, XFS_ATTR_FORK);
	if (*local) {
		if (size > (args->geo->blksize / 2)) {
			/* Double split possible */
			nblks *= 2;
		}
	} else {
		/*
		 * Out of line attribute, cannot double split, but
		 * make room for the attribute value itself.
		 */
		uint	dblocks = xfs_attr3_rmt_blocks(mp, args->valuelen);
		nblks += dblocks;
		nblks += XFS_NEXTENTADD_SPACE_RES(mp, dblocks, XFS_ATTR_FORK);
	}

	return nblks;
}

/* Initialize transaction reservation for attr operations */
void
xfs_init_attr_trans(
	struct xfs_da_args	*args,
	struct xfs_trans_res	*tres,
	unsigned int		*total)
{
	struct xfs_mount	*mp = args->dp->i_mount;

	if (args->value) {
		tres->tr_logres = M_RES(mp)->tr_attrsetm.tr_logres +
				 M_RES(mp)->tr_attrsetrt.tr_logres *
				 args->total;
		tres->tr_logcount = XFS_ATTRSET_LOG_COUNT;
		tres->tr_logflags = XFS_TRANS_PERM_LOG_RES;
		*total = args->total;
	} else {
		*tres = M_RES(mp)->tr_attrrm;
		*total = XFS_ATTRRM_SPACE_RES(mp);
	}
}

/*
 * Add an attr to a shortform fork. If there is no space,
 * xfs_attr_shortform_addname() will convert to leaf format and return -ENOSPC.
 * to use.
 */
STATIC int
xfs_attr_try_sf_addname(
	struct xfs_inode	*dp,
	struct xfs_da_args	*args)
{

	int			error;

	/*
	 * Build initial attribute list (if required).
	 */
	if (dp->i_afp->if_format == XFS_DINODE_FMT_EXTENTS)
		xfs_attr_shortform_create(args);

	error = xfs_attr_shortform_addname(args);
	if (error == -ENOSPC)
		return error;

	/*
	 * Commit the shortform mods, and we're done.
	 * NOTE: this is also the error path (EEXIST, etc).
	 */
	if (!error && !(args->op_flags & XFS_DA_OP_NOTIME))
		xfs_trans_ichgtime(args->trans, dp, XFS_ICHGTIME_CHG);

	if (xfs_has_wsync(dp->i_mount))
		xfs_trans_set_sync(args->trans);

	return error;
}

static int
xfs_attr_sf_addname(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args		*args = attr->xattri_da_args;
	struct xfs_inode		*dp = args->dp;
	int				error = 0;

	error = xfs_attr_try_sf_addname(dp, args);
	if (error != -ENOSPC) {
		ASSERT(!error || error == -EEXIST);
		attr->xattri_dela_state = XFS_DAS_DONE;
		goto out;
	}

	/*
	 * It won't fit in the shortform, transform to a leaf block.  GROT:
	 * another possible req'mt for a double-split btree op.
	 */
	error = xfs_attr_shortform_to_leaf(args, &attr->xattri_leaf_bp);
	if (error)
		return error;

	/*
	 * Prevent the leaf buffer from being unlocked so that a concurrent AIL
	 * push cannot grab the half-baked leaf buffer and run into problems
	 * with the write verifier.
	 */
	xfs_trans_bhold(args->trans, attr->xattri_leaf_bp);
	attr->xattri_dela_state = XFS_DAS_LEAF_ADD;
	error = -EAGAIN;
out:
	trace_xfs_attr_sf_addname_return(attr->xattri_dela_state, args->dp);
	return error;
}

/*
 * When we bump the state to REPLACE, we may actually need to skip over the
 * state. When LARP mode is enabled, we don't need to run the atomic flags flip,
 * so we skip straight over the REPLACE state and go on to REMOVE_OLD.
 */
static void
xfs_attr_dela_state_set_replace(
	struct xfs_attr_item	*attr,
	enum xfs_delattr_state	replace)
{
	struct xfs_da_args	*args = attr->xattri_da_args;

	ASSERT(replace == XFS_DAS_LEAF_REPLACE ||
			replace == XFS_DAS_NODE_REPLACE);

	attr->xattri_dela_state = replace;
	if (xfs_has_larp(args->dp->i_mount))
		attr->xattri_dela_state++;
}

static int
xfs_attr_leaf_addname(
	struct xfs_attr_item	*attr)
{
	struct xfs_da_args	*args = attr->xattri_da_args;
	int			error;

	ASSERT(xfs_attr_is_leaf(args->dp));

	/*
	 * Use the leaf buffer we may already hold locked as a result of
	 * a sf-to-leaf conversion. The held buffer is no longer valid
	 * after this call, regardless of the result.
	 */
	error = xfs_attr_leaf_try_add(args, attr->xattri_leaf_bp);
	attr->xattri_leaf_bp = NULL;

	if (error == -ENOSPC) {
		error = xfs_attr3_leaf_to_node(args);
		if (error)
			return error;

		/*
		 * We're not in leaf format anymore, so roll the transaction and
		 * retry the add to the newly allocated node block.
		 */
		attr->xattri_dela_state = XFS_DAS_NODE_ADD;
		error = -EAGAIN;
		goto out;
	}
	if (error)
		return error;

	/*
	 * We need to commit and roll if we need to allocate remote xattr blocks
	 * or perform more xattr manipulations. Otherwise there is nothing more
	 * to do and we can return success.
	 */
	if (args->rmtblkno) {
		attr->xattri_dela_state = XFS_DAS_LEAF_SET_RMT;
		error = -EAGAIN;
	} else if (args->op_flags & XFS_DA_OP_RENAME) {
		xfs_attr_dela_state_set_replace(attr, XFS_DAS_LEAF_REPLACE);
		error = -EAGAIN;
	} else {
		attr->xattri_dela_state = XFS_DAS_DONE;
	}
out:
	trace_xfs_attr_leaf_addname_return(attr->xattri_dela_state, args->dp);
	return error;
}

static int
xfs_attr_node_addname(
	struct xfs_attr_item	*attr)
{
	struct xfs_da_args	*args = attr->xattri_da_args;
	int			error;

	ASSERT(!attr->xattri_leaf_bp);

	error = xfs_attr_node_addname_find_attr(attr);
	if (error)
		return error;

	error = xfs_attr_node_try_addname(attr);
	if (error)
		return error;

	if (args->rmtblkno) {
		attr->xattri_dela_state = XFS_DAS_NODE_SET_RMT;
		error = -EAGAIN;
	} else if (args->op_flags & XFS_DA_OP_RENAME) {
		xfs_attr_dela_state_set_replace(attr, XFS_DAS_NODE_REPLACE);
		error = -EAGAIN;
	} else {
		attr->xattri_dela_state = XFS_DAS_DONE;
	}

	trace_xfs_attr_node_addname_return(attr->xattri_dela_state, args->dp);
	return error;
}

static int
xfs_attr_rmtval_alloc(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args              *args = attr->xattri_da_args;
	int				error = 0;

	/*
	 * If there was an out-of-line value, allocate the blocks we
	 * identified for its storage and copy the value.  This is done
	 * after we create the attribute so that we don't overflow the
	 * maximum size of a transaction and/or hit a deadlock.
	 */
	if (attr->xattri_blkcnt > 0) {
		error = xfs_attr_rmtval_set_blk(attr);
		if (error)
			return error;
		/* Roll the transaction only if there is more to allocate. */
		if (attr->xattri_blkcnt > 0) {
			error = -EAGAIN;
			goto out;
		}
	}

	error = xfs_attr_rmtval_set_value(args);
	if (error)
		return error;

	/* If this is not a rename, clear the incomplete flag and we're done. */
	if (!(args->op_flags & XFS_DA_OP_RENAME)) {
		error = xfs_attr3_leaf_clearflag(args);
		attr->xattri_dela_state = XFS_DAS_DONE;
	} else {
		/*
		 * We are running a REPLACE operation, so we need to bump the
		 * state to the step in that operation.
		 */
		attr->xattri_dela_state++;
		xfs_attr_dela_state_set_replace(attr, attr->xattri_dela_state);
	}
out:
	trace_xfs_attr_rmtval_alloc(attr->xattri_dela_state, args->dp);
	return error;
}

/*
 * Set the attribute specified in @args.
 * This routine is meant to function as a delayed operation, and may return
 * -EAGAIN when the transaction needs to be rolled.  Calling functions will need
 * to handle this, and recall the function until a successful error code is
 * returned.
 */
int
xfs_attr_set_iter(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args              *args = attr->xattri_da_args;
	struct xfs_inode		*dp = args->dp;
	struct xfs_buf			*bp = NULL;
	int				forkoff, error = 0;

	/* State machine switch */
next_state:
	switch (attr->xattri_dela_state) {
	case XFS_DAS_UNINIT:
		ASSERT(0);
		return -EFSCORRUPTED;
	case XFS_DAS_SF_ADD:
		return xfs_attr_sf_addname(attr);
	case XFS_DAS_LEAF_ADD:
		return xfs_attr_leaf_addname(attr);
	case XFS_DAS_NODE_ADD:
		return xfs_attr_node_addname(attr);

	case XFS_DAS_LEAF_SET_RMT:
	case XFS_DAS_NODE_SET_RMT:
		error = xfs_attr_rmtval_find_space(attr);
		if (error)
			return error;
		attr->xattri_dela_state++;
		fallthrough;

	case XFS_DAS_LEAF_ALLOC_RMT:
	case XFS_DAS_NODE_ALLOC_RMT:
		error = xfs_attr_rmtval_alloc(attr);
		if (error)
			return error;
		if (attr->xattri_dela_state == XFS_DAS_DONE)
			break;
		goto next_state;

	case XFS_DAS_LEAF_REPLACE:
	case XFS_DAS_NODE_REPLACE:
		/*
		 * We must "flip" the incomplete flags on the "new" and "old"
		 * attribute/value pairs so that one disappears and one appears
		 * atomically.  Then we must remove the "old" attribute/value
		 * pair.
		 */
		error = xfs_attr3_leaf_flipflags(args);
		if (error)
			return error;
		/*
		 * Commit the flag value change and start the next trans
		 * in series at REMOVE_OLD.
		 */
		error = -EAGAIN;
		attr->xattri_dela_state++;
		break;

	case XFS_DAS_LEAF_REMOVE_OLD:
	case XFS_DAS_NODE_REMOVE_OLD:
		/*
		 * Dismantle the "old" attribute/value pair by removing a
		 * "remote" value (if it exists).
		 */
		xfs_attr_restore_rmt_blk(args);
		error = xfs_attr_rmtval_invalidate(args);
		if (error)
			return error;

		attr->xattri_dela_state++;
		fallthrough;
	case XFS_DAS_RM_LBLK:
	case XFS_DAS_RM_NBLK:
		if (args->rmtblkno) {
			error = xfs_attr_rmtval_remove(attr);
			if (error == -EAGAIN)
				trace_xfs_attr_set_iter_return(
					attr->xattri_dela_state, args->dp);
			if (error)
				return error;

			attr->xattri_dela_state = XFS_DAS_RD_LEAF;
			trace_xfs_attr_set_iter_return(attr->xattri_dela_state,
						       args->dp);
			return -EAGAIN;
		}

		/*
		 * This is the end of the shared leaf/node sequence. We need
		 * to continue at the next state in the sequence, but we can't
		 * easily just fall through. So we increment to the next state
		 * and then jump back to switch statement to evaluate the next
		 * state correctly.
		 */
		attr->xattri_dela_state++;
		goto next_state;

	case XFS_DAS_RD_LEAF:
		/*
		 * This is the last step for leaf format. Read the block with
		 * the old attr, remove the old attr, check for shortform
		 * conversion and return.
		 */
		error = xfs_attr3_leaf_read(args->trans, args->dp, args->blkno,
					   &bp);
		if (error)
			return error;

		xfs_attr3_leaf_remove(bp, args);

		forkoff = xfs_attr_shortform_allfit(bp, dp);
		if (forkoff)
			error = xfs_attr3_leaf_to_shortform(bp, args, forkoff);
			/* bp is gone due to xfs_da_shrink_inode */

		return error;

	case XFS_DAS_CLR_FLAG:
		/*
		 * The last state for node format. Look up the old attr and
		 * remove it.
		 */
		error = xfs_attr_node_addname_clear_incomplete(attr);
		break;
	default:
		ASSERT(0);
		break;
	}

	trace_xfs_attr_set_iter_return(attr->xattri_dela_state, args->dp);
	return error;
}


/*
 * Return EEXIST if attr is found, or ENOATTR if not
 */
static int
xfs_attr_lookup(
	struct xfs_da_args	*args)
{
	struct xfs_inode	*dp = args->dp;
	struct xfs_buf		*bp = NULL;
	int			error;

	if (!xfs_inode_hasattr(dp))
		return -ENOATTR;

	if (dp->i_afp->if_format == XFS_DINODE_FMT_LOCAL)
		return xfs_attr_sf_findname(args, NULL, NULL);

	if (xfs_attr_is_leaf(dp)) {
		error = xfs_attr_leaf_hasname(args, &bp);

		if (bp)
			xfs_trans_brelse(args->trans, bp);

		return error;
	}

	return xfs_attr_node_hasname(args, NULL);
}

static int
xfs_attr_item_init(
	struct xfs_da_args	*args,
	unsigned int		op_flags,	/* op flag (set or remove) */
	struct xfs_attr_item	**attr)		/* new xfs_attr_item */
{

	struct xfs_attr_item	*new;

	new = kmem_zalloc(sizeof(struct xfs_attr_item), KM_NOFS);
	new->xattri_op_flags = op_flags;
	new->xattri_da_args = args;

	*attr = new;
	return 0;
}

/* Sets an attribute for an inode as a deferred operation */
static int
xfs_attr_defer_add(
	struct xfs_da_args	*args)
{
	struct xfs_attr_item	*new;
	int			error = 0;

	error = xfs_attr_item_init(args, XFS_ATTR_OP_FLAGS_SET, &new);
	if (error)
		return error;

	new->xattri_dela_state = xfs_attr_init_add_state(args);
	xfs_defer_add(args->trans, XFS_DEFER_OPS_TYPE_ATTR, &new->xattri_list);
	trace_xfs_attr_defer_add(new->xattri_dela_state, args->dp);

	return 0;
}

/* Sets an attribute for an inode as a deferred operation */
static int
xfs_attr_defer_replace(
	struct xfs_da_args	*args)
{
	struct xfs_attr_item	*new;
	int			error = 0;

	error = xfs_attr_item_init(args, XFS_ATTR_OP_FLAGS_REPLACE, &new);
	if (error)
		return error;

	new->xattri_dela_state = xfs_attr_init_replace_state(args);
	xfs_defer_add(args->trans, XFS_DEFER_OPS_TYPE_ATTR, &new->xattri_list);
	trace_xfs_attr_defer_replace(new->xattri_dela_state, args->dp);

	return 0;
}

/* Removes an attribute for an inode as a deferred operation */
static int
xfs_attr_defer_remove(
	struct xfs_da_args	*args)
{

	struct xfs_attr_item	*new;
	int			error;

	error  = xfs_attr_item_init(args, XFS_ATTR_OP_FLAGS_REMOVE, &new);
	if (error)
		return error;

	new->xattri_dela_state = XFS_DAS_UNINIT;
	xfs_defer_add(args->trans, XFS_DEFER_OPS_TYPE_ATTR, &new->xattri_list);
	trace_xfs_attr_defer_remove(new->xattri_dela_state, args->dp);

	return 0;
}

/*
 * Note: If args->value is NULL the attribute will be removed, just like the
 * Linux ->setattr API.
 */
int
xfs_attr_set(
	struct xfs_da_args	*args)
{
	struct xfs_inode	*dp = args->dp;
	struct xfs_mount	*mp = dp->i_mount;
	struct xfs_trans_res	tres;
	bool			rsvd = (args->attr_filter & XFS_ATTR_ROOT);
	int			error, local;
	int			rmt_blks = 0;
	unsigned int		total;
	int			delayed = xfs_has_larp(mp);

	if (xfs_is_shutdown(dp->i_mount))
		return -EIO;

	error = xfs_qm_dqattach(dp);
	if (error)
		return error;

	args->geo = mp->m_attr_geo;
	args->whichfork = XFS_ATTR_FORK;
	args->hashval = xfs_da_hashname(args->name, args->namelen);

	/*
	 * We have no control over the attribute names that userspace passes us
	 * to remove, so we have to allow the name lookup prior to attribute
	 * removal to fail as well.
	 */
	args->op_flags = XFS_DA_OP_OKNOENT;

	if (args->value) {
		XFS_STATS_INC(mp, xs_attr_set);

		args->op_flags |= XFS_DA_OP_ADDNAME;
		args->total = xfs_attr_calc_size(args, &local);

		/*
		 * If the inode doesn't have an attribute fork, add one.
		 * (inode must not be locked when we call this routine)
		 */
		if (XFS_IFORK_Q(dp) == 0) {
			int sf_size = sizeof(struct xfs_attr_sf_hdr) +
				xfs_attr_sf_entsize_byname(args->namelen,
						args->valuelen);

			error = xfs_bmap_add_attrfork(dp, sf_size, rsvd);
			if (error)
				return error;
		}

		if (!local)
			rmt_blks = xfs_attr3_rmt_blocks(mp, args->valuelen);
	} else {
		XFS_STATS_INC(mp, xs_attr_remove);
		rmt_blks = xfs_attr3_rmt_blocks(mp, XFS_XATTR_SIZE_MAX);
	}

	if (delayed) {
		error = xfs_attr_use_log_assist(mp);
		if (error)
			return error;
	}

	/*
	 * Root fork attributes can use reserved data blocks for this
	 * operation if necessary
	 */
	xfs_init_attr_trans(args, &tres, &total);
	error = xfs_trans_alloc_inode(dp, &tres, total, 0, rsvd, &args->trans);
	if (error)
		goto drop_incompat;

	if (args->value || xfs_inode_hasattr(dp)) {
		error = xfs_iext_count_may_overflow(dp, XFS_ATTR_FORK,
				XFS_IEXT_ATTR_MANIP_CNT(rmt_blks));
		if (error == -EFBIG)
			error = xfs_iext_count_upgrade(args->trans, dp,
					XFS_IEXT_ATTR_MANIP_CNT(rmt_blks));
		if (error)
			goto out_trans_cancel;
	}

	error = xfs_attr_lookup(args);
	switch (error) {
	case -EEXIST:
		/* if no value, we are performing a remove operation */
		if (!args->value) {
			error = xfs_attr_defer_remove(args);
			break;
		}
		/* Pure create fails if the attr already exists */
		if (args->attr_flags & XATTR_CREATE)
			goto out_trans_cancel;

		error = xfs_attr_defer_replace(args);
		break;
	case -ENOATTR:
		/* Can't remove what isn't there. */
		if (!args->value)
			goto out_trans_cancel;

		/* Pure replace fails if no existing attr to replace. */
		if (args->attr_flags & XATTR_REPLACE)
			goto out_trans_cancel;

		error = xfs_attr_defer_add(args);
		break;
	default:
		goto out_trans_cancel;
	}
	if (error)
		goto out_trans_cancel;

	/*
	 * If this is a synchronous mount, make sure that the
	 * transaction goes to disk before returning to the user.
	 */
	if (xfs_has_wsync(mp))
		xfs_trans_set_sync(args->trans);

	if (!(args->op_flags & XFS_DA_OP_NOTIME))
		xfs_trans_ichgtime(args->trans, dp, XFS_ICHGTIME_CHG);

	/*
	 * Commit the last in the sequence of transactions.
	 */
	xfs_trans_log_inode(args->trans, dp, XFS_ILOG_CORE);
	error = xfs_trans_commit(args->trans);
out_unlock:
	xfs_iunlock(dp, XFS_ILOCK_EXCL);
drop_incompat:
	if (delayed)
		xlog_drop_incompat_feat(mp->m_log);
	return error;

out_trans_cancel:
	if (args->trans)
		xfs_trans_cancel(args->trans);
	goto out_unlock;
}

int __init
xfs_attri_init_cache(void)
{
	xfs_attri_cache = kmem_cache_create("xfs_attri",
					    sizeof(struct xfs_attri_log_item),
					    0, 0, NULL);

	return xfs_attri_cache != NULL ? 0 : -ENOMEM;
}

void
xfs_attri_destroy_cache(void)
{
	kmem_cache_destroy(xfs_attri_cache);
	xfs_attri_cache = NULL;
}

int __init
xfs_attrd_init_cache(void)
{
	xfs_attrd_cache = kmem_cache_create("xfs_attrd",
					    sizeof(struct xfs_attrd_log_item),
					    0, 0, NULL);

	return xfs_attrd_cache != NULL ? 0 : -ENOMEM;
}

void
xfs_attrd_destroy_cache(void)
{
	kmem_cache_destroy(xfs_attrd_cache);
	xfs_attrd_cache = NULL;
}

/*========================================================================
 * External routines when attribute list is inside the inode
 *========================================================================*/

static inline int xfs_attr_sf_totsize(struct xfs_inode *dp)
{
	struct xfs_attr_shortform *sf;

	sf = (struct xfs_attr_shortform *)dp->i_afp->if_u1.if_data;
	return be16_to_cpu(sf->hdr.totsize);
}

/*
 * Add a name to the shortform attribute list structure
 * This is the external routine.
 */
STATIC int
xfs_attr_shortform_addname(xfs_da_args_t *args)
{
	int newsize, forkoff, retval;

	trace_xfs_attr_sf_addname(args);

	retval = xfs_attr_shortform_lookup(args);
	if (retval == -ENOATTR && (args->attr_flags & XATTR_REPLACE))
		return retval;
	if (retval == -EEXIST) {
		if (args->attr_flags & XATTR_CREATE)
			return retval;
		retval = xfs_attr_sf_removename(args);
		if (retval)
			return retval;
		/*
		 * Since we have removed the old attr, clear ATTR_REPLACE so
		 * that the leaf format add routine won't trip over the attr
		 * not being around.
		 */
		args->attr_flags &= ~XATTR_REPLACE;
	}

	if (args->namelen >= XFS_ATTR_SF_ENTSIZE_MAX ||
	    args->valuelen >= XFS_ATTR_SF_ENTSIZE_MAX)
		return -ENOSPC;

	newsize = xfs_attr_sf_totsize(args->dp);
	newsize += xfs_attr_sf_entsize_byname(args->namelen, args->valuelen);

	forkoff = xfs_attr_shortform_bytesfit(args->dp, newsize);
	if (!forkoff)
		return -ENOSPC;

	xfs_attr_shortform_add(args, forkoff);
	return 0;
}


/*========================================================================
 * External routines when attribute list is one block
 *========================================================================*/

/* Store info about a remote block */
STATIC void
xfs_attr_save_rmt_blk(
	struct xfs_da_args	*args)
{
	args->blkno2 = args->blkno;
	args->index2 = args->index;
	args->rmtblkno2 = args->rmtblkno;
	args->rmtblkcnt2 = args->rmtblkcnt;
	args->rmtvaluelen2 = args->rmtvaluelen;
}

/* Set stored info about a remote block */
STATIC void
xfs_attr_restore_rmt_blk(
	struct xfs_da_args	*args)
{
	args->blkno = args->blkno2;
	args->index = args->index2;
	args->rmtblkno = args->rmtblkno2;
	args->rmtblkcnt = args->rmtblkcnt2;
	args->rmtvaluelen = args->rmtvaluelen2;
}

/*
 * Tries to add an attribute to an inode in leaf form
 *
 * This function is meant to execute as part of a delayed operation and leaves
 * the transaction handling to the caller.  On success the attribute is added
 * and the inode and transaction are left dirty.  If there is not enough space,
 * the attr data is converted to node format and -ENOSPC is returned. Caller is
 * responsible for handling the dirty inode and transaction or adding the attr
 * in node format.
 */
STATIC int
xfs_attr_leaf_try_add(
	struct xfs_da_args	*args,
	struct xfs_buf		*bp)
{
	int			error;

	/*
	 * If the caller provided a buffer to us, it is locked and held in
	 * the transaction because it just did a shortform to leaf conversion.
	 * Hence we don't need to read it again. Otherwise read in the leaf
	 * buffer.
	 */
	if (bp) {
		xfs_trans_bhold_release(args->trans, bp);
	} else {
		error = xfs_attr3_leaf_read(args->trans, args->dp, 0, &bp);
		if (error)
			return error;
	}

	/*
	 * Look up the xattr name to set the insertion point for the new xattr.
	 */
	error = xfs_attr3_leaf_lookup_int(bp, args);
	if (error != -ENOATTR && error != -EEXIST)
		goto out_brelse;
	if (error == -ENOATTR && (args->attr_flags & XATTR_REPLACE))
		goto out_brelse;
	if (error == -EEXIST) {
		if (args->attr_flags & XATTR_CREATE)
			goto out_brelse;

		trace_xfs_attr_leaf_replace(args);

		/* save the attribute state for later removal*/
		args->op_flags |= XFS_DA_OP_RENAME;	/* an atomic rename */
		xfs_attr_save_rmt_blk(args);

		/*
		 * clear the remote attr state now that it is saved so that the
		 * values reflect the state of the attribute we are about to
		 * add, not the attribute we just found and will remove later.
		 */
		args->rmtblkno = 0;
		args->rmtblkcnt = 0;
		args->rmtvaluelen = 0;
	}

	return xfs_attr3_leaf_add(bp, args);

out_brelse:
	xfs_trans_brelse(args->trans, bp);
	return error;
}

/*
 * Return EEXIST if attr is found, or ENOATTR if not
 */
STATIC int
xfs_attr_leaf_hasname(
	struct xfs_da_args	*args,
	struct xfs_buf		**bp)
{
	int                     error = 0;

	error = xfs_attr3_leaf_read(args->trans, args->dp, 0, bp);
	if (error)
		return error;

	error = xfs_attr3_leaf_lookup_int(*bp, args);
	if (error != -ENOATTR && error != -EEXIST)
		xfs_trans_brelse(args->trans, *bp);

	return error;
}

/*
 * Remove a name from the leaf attribute list structure
 *
 * This leaf block cannot have a "remote" value, we only call this routine
 * if bmap_one_block() says there is only one block (ie: no remote blks).
 */
STATIC int
xfs_attr_leaf_removename(
	struct xfs_da_args	*args)
{
	struct xfs_inode	*dp;
	struct xfs_buf		*bp;
	int			error, forkoff;

	trace_xfs_attr_leaf_removename(args);

	/*
	 * Remove the attribute.
	 */
	dp = args->dp;

	error = xfs_attr_leaf_hasname(args, &bp);

	if (error == -ENOATTR) {
		xfs_trans_brelse(args->trans, bp);
		return error;
	} else if (error != -EEXIST)
		return error;

	xfs_attr3_leaf_remove(bp, args);

	/*
	 * If the result is small enough, shrink it all into the inode.
	 */
	forkoff = xfs_attr_shortform_allfit(bp, dp);
	if (forkoff)
		return xfs_attr3_leaf_to_shortform(bp, args, forkoff);
		/* bp is gone due to xfs_da_shrink_inode */

	return 0;
}

/*
 * Look up a name in a leaf attribute list structure.
 *
 * This leaf block cannot have a "remote" value, we only call this routine
 * if bmap_one_block() says there is only one block (ie: no remote blks).
 *
 * Returns 0 on successful retrieval, otherwise an error.
 */
STATIC int
xfs_attr_leaf_get(xfs_da_args_t *args)
{
	struct xfs_buf *bp;
	int error;

	trace_xfs_attr_leaf_get(args);

	error = xfs_attr_leaf_hasname(args, &bp);

	if (error == -ENOATTR)  {
		xfs_trans_brelse(args->trans, bp);
		return error;
	} else if (error != -EEXIST)
		return error;


	error = xfs_attr3_leaf_getvalue(bp, args);
	xfs_trans_brelse(args->trans, bp);
	return error;
}

/*
 * Return EEXIST if attr is found, or ENOATTR if not
 * statep: If not null is set to point at the found state.  Caller will
 *         be responsible for freeing the state in this case.
 */
STATIC int
xfs_attr_node_hasname(
	struct xfs_da_args	*args,
	struct xfs_da_state	**statep)
{
	struct xfs_da_state	*state;
	int			retval, error;

	state = xfs_da_state_alloc(args);
	if (statep != NULL)
		*statep = state;

	/*
	 * Search to see if name exists, and get back a pointer to it.
	 */
	error = xfs_da3_node_lookup_int(state, &retval);
	if (error)
		retval = error;

	if (!statep)
		xfs_da_state_free(state);

	return retval;
}

/*========================================================================
 * External routines when attribute list size > geo->blksize
 *========================================================================*/

STATIC int
xfs_attr_node_addname_find_attr(
	 struct xfs_attr_item		*attr)
{
	struct xfs_da_args		*args = attr->xattri_da_args;
	int				retval;

	/*
	 * Search to see if name already exists, and get back a pointer
	 * to where it should go.
	 */
	retval = xfs_attr_node_hasname(args, &attr->xattri_da_state);
	if (retval != -ENOATTR && retval != -EEXIST)
		goto error;

	if (retval == -ENOATTR && (args->attr_flags & XATTR_REPLACE))
		goto error;
	if (retval == -EEXIST) {
		if (args->attr_flags & XATTR_CREATE)
			goto error;

		trace_xfs_attr_node_replace(args);

		/* save the attribute state for later removal*/
		args->op_flags |= XFS_DA_OP_RENAME;	/* atomic rename op */
		xfs_attr_save_rmt_blk(args);

		/*
		 * clear the remote attr state now that it is saved so that the
		 * values reflect the state of the attribute we are about to
		 * add, not the attribute we just found and will remove later.
		 */
		args->rmtblkno = 0;
		args->rmtblkcnt = 0;
		args->rmtvaluelen = 0;
	}

	return 0;
error:
	if (attr->xattri_da_state)
		xfs_da_state_free(attr->xattri_da_state);
	return retval;
}

/*
 * Add a name to a Btree-format attribute list.
 *
 * This will involve walking down the Btree, and may involve splitting
 * leaf nodes and even splitting intermediate nodes up to and including
 * the root node (a special case of an intermediate node).
 *
 * "Remote" attribute values confuse the issue and atomic rename operations
 * add a whole extra layer of confusion on top of that.
 *
 * This routine is meant to function as a delayed operation, and may return
 * -EAGAIN when the transaction needs to be rolled.  Calling functions will need
 * to handle this, and recall the function until a successful error code is
 *returned.
 */
static int
xfs_attr_node_try_addname(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args		*args = attr->xattri_da_args;
	struct xfs_da_state		*state = attr->xattri_da_state;
	struct xfs_da_state_blk		*blk;
	int				error;

	trace_xfs_attr_node_addname(args);

	blk = &state->path.blk[state->path.active-1];
	ASSERT(blk->magic == XFS_ATTR_LEAF_MAGIC);

	error = xfs_attr3_leaf_add(blk->bp, state->args);
	if (error == -ENOSPC) {
		if (state->path.active == 1) {
			/*
			 * Its really a single leaf node, but it had
			 * out-of-line values so it looked like it *might*
			 * have been a b-tree.
			 */
			xfs_da_state_free(state);
			state = NULL;
			error = xfs_attr3_leaf_to_node(args);
			if (error)
				goto out;

			/*
			 * Now that we have converted the leaf to a node, we can
			 * roll the transaction, and try xfs_attr3_leaf_add
			 * again on re-entry.  No need to set dela_state to do
			 * this. dela_state is still unset by this function at
			 * this point.
			 */
			trace_xfs_attr_node_addname_return(
					attr->xattri_dela_state, args->dp);
			return -EAGAIN;
		}

		/*
		 * Split as many Btree elements as required.
		 * This code tracks the new and old attr's location
		 * in the index/blkno/rmtblkno/rmtblkcnt fields and
		 * in the index2/blkno2/rmtblkno2/rmtblkcnt2 fields.
		 */
		error = xfs_da3_split(state);
		if (error)
			goto out;
	} else {
		/*
		 * Addition succeeded, update Btree hashvals.
		 */
		xfs_da3_fixhashpath(state, &state->path);
	}

out:
	if (state)
		xfs_da_state_free(state);
	return error;
}


STATIC int
xfs_attr_node_addname_clear_incomplete(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args		*args = attr->xattri_da_args;
	struct xfs_da_state		*state = NULL;
	struct xfs_mount		*mp = args->dp->i_mount;
	int				retval = 0;
	int				error = 0;

	/*
	 * Re-find the "old" attribute entry after any split ops. The INCOMPLETE
	 * flag means that we will find the "old" attr, not the "new" one.
	 */
	if (!xfs_has_larp(mp))
		args->attr_filter |= XFS_ATTR_INCOMPLETE;
	state = xfs_da_state_alloc(args);
	state->inleaf = 0;
	error = xfs_da3_node_lookup_int(state, &retval);
	if (error)
		goto out;

	error = xfs_attr_node_removename(args, state);

	/*
	 * Check to see if the tree needs to be collapsed.
	 */
	if (retval && (state->path.active > 1)) {
		error = xfs_da3_join(state);
		if (error)
			goto out;
	}
	retval = error = 0;

out:
	if (state)
		xfs_da_state_free(state);
	if (error)
		return error;
	return retval;
}

/*
 * Shrink an attribute from leaf to shortform
 */
STATIC int
xfs_attr_node_shrink(
	struct xfs_da_args	*args,
	struct xfs_da_state     *state)
{
	struct xfs_inode	*dp = args->dp;
	int			error, forkoff;
	struct xfs_buf		*bp;

	/*
	 * Have to get rid of the copy of this dabuf in the state.
	 */
	ASSERT(state->path.active == 1);
	ASSERT(state->path.blk[0].bp);
	state->path.blk[0].bp = NULL;

	error = xfs_attr3_leaf_read(args->trans, args->dp, 0, &bp);
	if (error)
		return error;

	forkoff = xfs_attr_shortform_allfit(bp, dp);
	if (forkoff) {
		error = xfs_attr3_leaf_to_shortform(bp, args, forkoff);
		/* bp is gone due to xfs_da_shrink_inode */
	} else
		xfs_trans_brelse(args->trans, bp);

	return error;
}

/*
 * Mark an attribute entry INCOMPLETE and save pointers to the relevant buffers
 * for later deletion of the entry.
 */
STATIC int
xfs_attr_leaf_mark_incomplete(
	struct xfs_da_args	*args,
	struct xfs_da_state	*state)
{
	int			error;

	/*
	 * Fill in disk block numbers in the state structure
	 * so that we can get the buffers back after we commit
	 * several transactions in the following calls.
	 */
	error = xfs_attr_fillstate(state);
	if (error)
		return error;

	/*
	 * Mark the attribute as INCOMPLETE
	 */
	return xfs_attr3_leaf_setflag(args);
}

/*
 * Initial setup for xfs_attr_node_removename.  Make sure the attr is there and
 * the blocks are valid.  Attr keys with remote blocks will be marked
 * incomplete.
 */
STATIC
int xfs_attr_node_removename_setup(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args		*args = attr->xattri_da_args;
	struct xfs_da_state		**state = &attr->xattri_da_state;
	int				error;

	error = xfs_attr_node_hasname(args, state);
	if (error != -EEXIST)
		goto out;
	error = 0;

	ASSERT((*state)->path.blk[(*state)->path.active - 1].bp != NULL);
	ASSERT((*state)->path.blk[(*state)->path.active - 1].magic ==
		XFS_ATTR_LEAF_MAGIC);

	if (args->rmtblkno > 0) {
		error = xfs_attr_leaf_mark_incomplete(args, *state);
		if (error)
			goto out;

		error = xfs_attr_rmtval_invalidate(args);
	}
out:
	if (error)
		xfs_da_state_free(*state);

	return error;
}

STATIC int
xfs_attr_node_removename(
	struct xfs_da_args	*args,
	struct xfs_da_state	*state)
{
	struct xfs_da_state_blk	*blk;
	int			retval;

	/*
	 * Remove the name and update the hashvals in the tree.
	 */
	blk = &state->path.blk[state->path.active-1];
	ASSERT(blk->magic == XFS_ATTR_LEAF_MAGIC);
	retval = xfs_attr3_leaf_remove(blk->bp, args);
	xfs_da3_fixhashpath(state, &state->path);

	return retval;
}

/*
 * Remove the attribute specified in @args.
 *
 * This will involve walking down the Btree, and may involve joining
 * leaf nodes and even joining intermediate nodes up to and including
 * the root node (a special case of an intermediate node).
 *
 * This routine is meant to function as either an in-line or delayed operation,
 * and may return -EAGAIN when the transaction needs to be rolled.  Calling
 * functions will need to handle this, and call the function until a
 * successful error code is returned.
 */
int
xfs_attr_remove_iter(
	struct xfs_attr_item		*attr)
{
	struct xfs_da_args		*args = attr->xattri_da_args;
	struct xfs_da_state		*state = attr->xattri_da_state;
	int				retval, error = 0;
	struct xfs_inode		*dp = args->dp;

	trace_xfs_attr_node_removename(args);

	switch (attr->xattri_dela_state) {
	case XFS_DAS_UNINIT:
		if (!xfs_inode_hasattr(dp))
			return -ENOATTR;

		/*
		 * Shortform or leaf formats don't require transaction rolls and
		 * thus state transitions. Call the right helper and return.
		 */
		if (dp->i_afp->if_format == XFS_DINODE_FMT_LOCAL)
			return xfs_attr_sf_removename(args);

		if (xfs_attr_is_leaf(dp))
			return xfs_attr_leaf_removename(args);

		/*
		 * Node format may require transaction rolls. Set up the
		 * state context and fall into the state machine.
		 */
		if (!attr->xattri_da_state) {
			error = xfs_attr_node_removename_setup(attr);
			if (error)
				return error;
			state = attr->xattri_da_state;
		}

		fallthrough;
	case XFS_DAS_RMTBLK:
		attr->xattri_dela_state = XFS_DAS_RMTBLK;

		/*
		 * If there is an out-of-line value, de-allocate the blocks.
		 * This is done before we remove the attribute so that we don't
		 * overflow the maximum size of a transaction and/or hit a
		 * deadlock.
		 */
		if (args->rmtblkno > 0) {
			/*
			 * May return -EAGAIN. Roll and repeat until all remote
			 * blocks are removed.
			 */
			error = xfs_attr_rmtval_remove(attr);
			if (error == -EAGAIN) {
				trace_xfs_attr_remove_iter_return(
					attr->xattri_dela_state, args->dp);
				return error;
			} else if (error) {
				goto out;
			}

			/*
			 * Refill the state structure with buffers (the prior
			 * calls released our buffers) and close out this
			 * transaction before proceeding.
			 */
			ASSERT(args->rmtblkno == 0);
			error = xfs_attr_refillstate(state);
			if (error)
				goto out;

			attr->xattri_dela_state = XFS_DAS_RM_NAME;
			trace_xfs_attr_remove_iter_return(
					attr->xattri_dela_state, args->dp);
			return -EAGAIN;
		}

		fallthrough;
	case XFS_DAS_RM_NAME:
		/*
		 * If we came here fresh from a transaction roll, reattach all
		 * the buffers to the current transaction.
		 */
		if (attr->xattri_dela_state == XFS_DAS_RM_NAME) {
			error = xfs_attr_refillstate(state);
			if (error)
				goto out;
		}

		retval = xfs_attr_node_removename(args, state);

		/*
		 * Check to see if the tree needs to be collapsed. If so, roll
		 * the transacton and fall into the shrink state.
		 */
		if (retval && (state->path.active > 1)) {
			error = xfs_da3_join(state);
			if (error)
				goto out;

			attr->xattri_dela_state = XFS_DAS_RM_SHRINK;
			trace_xfs_attr_remove_iter_return(
					attr->xattri_dela_state, args->dp);
			return -EAGAIN;
		}

		fallthrough;
	case XFS_DAS_RM_SHRINK:
		/*
		 * If the result is small enough, push it all into the inode.
		 * This is our final state so it's safe to return a dirty
		 * transaction.
		 */
		if (xfs_attr_is_leaf(dp))
			error = xfs_attr_node_shrink(args, state);
		ASSERT(error != -EAGAIN);
		break;
	default:
		ASSERT(0);
		error = -EINVAL;
		goto out;
	}
out:
	if (state)
		xfs_da_state_free(state);
	return error;
}

/*
 * Fill in the disk block numbers in the state structure for the buffers
 * that are attached to the state structure.
 * This is done so that we can quickly reattach ourselves to those buffers
 * after some set of transaction commits have released these buffers.
 */
STATIC int
xfs_attr_fillstate(xfs_da_state_t *state)
{
	xfs_da_state_path_t *path;
	xfs_da_state_blk_t *blk;
	int level;

	trace_xfs_attr_fillstate(state->args);

	/*
	 * Roll down the "path" in the state structure, storing the on-disk
	 * block number for those buffers in the "path".
	 */
	path = &state->path;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->bp) {
			blk->disk_blkno = xfs_buf_daddr(blk->bp);
			blk->bp = NULL;
		} else {
			blk->disk_blkno = 0;
		}
	}

	/*
	 * Roll down the "altpath" in the state structure, storing the on-disk
	 * block number for those buffers in the "altpath".
	 */
	path = &state->altpath;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->bp) {
			blk->disk_blkno = xfs_buf_daddr(blk->bp);
			blk->bp = NULL;
		} else {
			blk->disk_blkno = 0;
		}
	}

	return 0;
}

/*
 * Reattach the buffers to the state structure based on the disk block
 * numbers stored in the state structure.
 * This is done after some set of transaction commits have released those
 * buffers from our grip.
 */
STATIC int
xfs_attr_refillstate(xfs_da_state_t *state)
{
	xfs_da_state_path_t *path;
	xfs_da_state_blk_t *blk;
	int level, error;

	trace_xfs_attr_refillstate(state->args);

	/*
	 * Roll down the "path" in the state structure, storing the on-disk
	 * block number for those buffers in the "path".
	 */
	path = &state->path;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->disk_blkno) {
			error = xfs_da3_node_read_mapped(state->args->trans,
					state->args->dp, blk->disk_blkno,
					&blk->bp, XFS_ATTR_FORK);
			if (error)
				return error;
		} else {
			blk->bp = NULL;
		}
	}

	/*
	 * Roll down the "altpath" in the state structure, storing the on-disk
	 * block number for those buffers in the "altpath".
	 */
	path = &state->altpath;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->disk_blkno) {
			error = xfs_da3_node_read_mapped(state->args->trans,
					state->args->dp, blk->disk_blkno,
					&blk->bp, XFS_ATTR_FORK);
			if (error)
				return error;
		} else {
			blk->bp = NULL;
		}
	}

	return 0;
}

/*
 * Retrieve the attribute data from a node attribute list.
 *
 * This routine gets called for any attribute fork that has more than one
 * block, ie: both true Btree attr lists and for single-leaf-blocks with
 * "remote" values taking up more blocks.
 *
 * Returns 0 on successful retrieval, otherwise an error.
 */
STATIC int
xfs_attr_node_get(
	struct xfs_da_args	*args)
{
	struct xfs_da_state	*state;
	struct xfs_da_state_blk	*blk;
	int			i;
	int			error;

	trace_xfs_attr_node_get(args);

	/*
	 * Search to see if name exists, and get back a pointer to it.
	 */
	error = xfs_attr_node_hasname(args, &state);
	if (error != -EEXIST)
		goto out_release;

	/*
	 * Get the value, local or "remote"
	 */
	blk = &state->path.blk[state->path.active - 1];
	error = xfs_attr3_leaf_getvalue(blk->bp, args);

	/*
	 * If not in a transaction, we have to release all the buffers.
	 */
out_release:
	for (i = 0; state != NULL && i < state->path.active; i++) {
		xfs_trans_brelse(args->trans, state->path.blk[i].bp);
		state->path.blk[i].bp = NULL;
	}

	if (state)
		xfs_da_state_free(state);
	return error;
}

/* Returns true if the attribute entry name is valid. */
bool
xfs_attr_namecheck(
	const void	*name,
	size_t		length)
{
	/*
	 * MAXNAMELEN includes the trailing null, but (name/length) leave it
	 * out, so use >= for the length check.
	 */
	if (length >= MAXNAMELEN)
		return false;

	/* There shouldn't be any nulls here */
	return !memchr(name, 0, length);
}
