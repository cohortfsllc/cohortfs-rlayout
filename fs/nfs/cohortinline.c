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
    struct pnfs_layout_segment *lseg; /* XXX trouble */
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

    /* XXX testing only */
    if (! (cohort_debug & COHORT_DEBUG_LAYOUTGET))
        goto out_fail;

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
    /* XXX I'm not 100% sure that it can be used for Cohort
     * layouts, but there is layoutrecall and outstanding layoutget
     * synchronization logic (eg, nfs4_layoutget_prepare) that uses
     * nfs4_inode.  If we -can- get a directory inode (presumably
     * of the mount), it seems correct to do so. */
    lgp->args.inode = ino;
    lgp->lsegpp = &lseg;
        
    /* Synchronously retrieve layout information from server and
     * store in lseg. */
    nfs4_proc_layoutget(lgp);

    /* XXX finish */

    out_fail:
        return (-EINVAL);
}

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
