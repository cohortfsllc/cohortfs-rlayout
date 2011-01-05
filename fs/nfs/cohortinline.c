/*
 *  Module for the Cohort nfsv4.1 replication layout driver.
 *
 *  Copyright (c) 2010
 *  The Linux Box Corporation
 *  All Rights Reserved
 *
 *  Matt Benjamin <matt@linuxbox.com>
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/nfs_fs.h>

#include "internal.h"
#include "pnfs.h"
#include "nfs4filelayout.h"
#include "cohort.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

/*
 * This file and its exported functions provide a Cohort replication layout
 * driver prototype.  Cohort layouts use the NFSv4.1 layout operations, but
 * extend their semantics beyond pNFS.  The replication layout driver, in
 * particular, does not provide pNFS operations, and provides a different set
 * of operations and types.  To simplify prototyping, I've extended the 
 * pnfs module with exported functions and types.  
 */

/* Initialize (called from module init) */
int cohort_init(void)
{
	printk(KERN_INFO "%s: Cohort Inline Replication Layout Driver Init\n",
	       __func__);

        /* We are a layout driver of a new class, extend driver cache
         * to permit registration here. */

        return (0);
}
EXPORT_SYMBOL_GPL(cohort_init);

void cohort_exit(void)
{
	printk(KERN_INFO "%s: Cohort Inline Replication Layout Driver Exit\n",
	       __func__);
}
EXPORT_SYMBOL_GPL(cohort_exit);

/*
 * Attempt to get a Cohort replication layout.  For the moment we will
 * request replication layouts only on filesystem (volume) mounts.
 */
int
cohort_replication_layoutget(struct nfs_server *server,
                             struct inode *ino,
                             const struct nfs_fh *mntfh)

{
    struct nfs4_layoutget *lgp;
    struct pnfs_layout_segment *lseg;
    struct pnfs_layout_range range;
    struct pnfs_layout_hdr *layout_hdr = NULL;

    dprintk("--> %s\n", __func__);

    if (! (server->layouttypes & FSINFO_LAYOUT_COHORT_REPLICATION)) {
        dprintk("%s: request replication layout unsupported by server\n",
                __func__);
        goto out_fail;
    }

    /* Could be improved--check its type */
    if (! server->pnfs_meta_ld) {
        dprintk("%s: replication layout driver not registered\n",
                __func__);
        goto out_fail;
    } else  {
        dprintk("%s: using replication layout driver %p\n",
                __func__,
                server->pnfs_meta_ld);
    }

    /* if the following succeeds, then layout_hdr is both allocated
     * and linked from NFS_I(ino) */
    spin_lock(&ino->i_lock);
    layout_hdr = pnfs_find_alloc_layout(ino);
    spin_unlock(&ino->i_lock);
    if (! layout_hdr) {
        dprintk("%s: pnfs_find_alloc_layout failed!\n", __func__);
        goto out_fail;
    }

    lgp = kzalloc(sizeof(*lgp), GFP_KERNEL);
    if (lgp == NULL) {
        dprintk("%s: cant kzalloc lgp!\n", __func__);
        goto out_fail;
    }

    /* Range is always full */
    range.iomode = IOMODE_RW;
    range.offset = 0ULL;
    range.length = NFS4_MAX_UINT64;

    /* Setup request */
    lgp->args.type = LAYOUT4_COHORT_REPLICATION;
    lgp->args.minlength = 0ULL;
    lgp->args.maxcount = PNFS_LAYOUT_MAXSIZE;

    lgp->args.range = range;
    lgp->args.u_lta.ch.server = server;
    lgp->args.u_lta.ch.mntfh = (struct nfs_fh *) mntfh;
    /* Nb.  Cohort inode is super's inode */
    lgp->args.inode = ino;
    lgp->lsegpp = &lseg;
        
    /* Synchronously retrieve layout information from server and
     * store in lseg. */
    nfs4_proc_layoutget(lgp);

    /* XXX need ihold? */
    server->s_ino = ino;

    /* XXX finish? */

out_fail:
    server->s_ino = NULL;
    return (-EINVAL);
}

int
cohort_rpl_create(struct inode *d_ino,
                  struct dentry *dentry,
                  struct nfs4_createdata *data)
{
    struct pnfs_layout_range range;
    struct pnfs_layout_hdr *lo = NULL;
    struct pnfs_layout_segment *lseg = NULL;
    struct nfs_server *server = NFS_SERVER(d_ino);
    struct nfs_inode *nfsi = NFS_I(d_ino);
    struct inode *s_ino = server->s_ino;
    int code;

    dprintk("--> %s\n", __func__);

    code = (-EINVAL);

    /* XXX Shareable preamble:
     * 1. get s_ino
     * 2. get lo
     * 3. get lseg
     */
    if (!s_ino) {
        dprintk("%s no super s_i\n", __func__);
        goto out_err;
    }

    if (!server->pnfs_meta_ld || 
        (server->pnfs_meta_ld->id != LAYOUT4_COHORT_REPLICATION)) {
        dprintk("%s no valid layout (%p)\n", __func__,
                server->pnfs_meta_ld);
        goto out_err;
    }

    /* Find and ref layout for d_ino, if possible */
    range.iomode = IOMODE_RW;
    range.offset = 0ULL;
    range.length = NFS4_MAX_UINT64;

    spin_lock(&s_ino->i_lock);
    lo = pnfs_find_inode_layout(s_ino);
    if (!lo) {
        dprintk("%s no valid layout (%p, %p)\n", __func__,
                server->pnfs_meta_ld,
                s_ino);
        goto out_unlock;
    }

    /* Try to find the corresponding layout segment */
    lseg = pnfs_find_lseg(lo, &range);
    if (lseg)
        get_lseg(lseg);
    else
        goto out_unref_lo;

    /* Call */
    /* XXX Finish */
    dprintk("%s got replication layout (%p, %p)\n", __func__,
            lo, lseg);

    /* Postamble. */
    spin_lock(&s_ino->i_lock);
    put_lseg_locked2(lseg);
out_unref_lo:
    put_layout_hdr_locked(lo);
out_unlock:
    spin_unlock(&s_ino->i_lock);
out_err:
    return (code);
}
EXPORT_SYMBOL_GPL(cohort_rpl_create);

void
cohort_set_layoutdrivers(struct nfs_server *server,
                         const struct nfs_fh *mntfh,
                         struct nfs_fsinfo *fsinfo)
{
    dprintk("%s: called with server %p, mntfh %p, fsinfo %p.\n",
            __func__, server, mntfh, fsinfo);
    if (fsinfo->layouttypes & FSINFO_LAYOUT_COHORT_REPLICATION) {
        dprintk("%s: request Cohort replication layout\n",
                __func__);
    }
}
EXPORT_SYMBOL_GPL(cohort_set_layoutdrivers);
