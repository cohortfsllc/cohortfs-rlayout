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
                             struct inode *s_ino,
                             const struct nfs_fh *mntfh)

{
    struct nfs4_layoutget *lgp;
    struct pnfs_layout_segment *lseg;
    struct pnfs_layout_range range;
    struct pnfs_layout_hdr *layout_hdr = NULL;
    int code;

    dprintk("--> %s\n", __func__);

    code = -EINVAL;

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
     * and linked from NFS_I(s_ino) */
    spin_lock(&s_ino->i_lock);
    layout_hdr = pnfs_find_alloc_layout(s_ino);
    spin_unlock(&s_ino->i_lock);
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
    lgp->args.inode = s_ino;
    lgp->lsegpp = &lseg;
        
    /* Synchronously retrieve layout information from server and
     * store in lseg. */
    nfs4_proc_layoutget(lgp);

    /* Install super inode in server->s_ino.
     * XXX Need ihold?
     * XXX What lock protects super?
     */
    server->s_ino = s_ino;
    goto out;
out_fail:
    server->s_ino = NULL;
out:
    return (code);
}
EXPORT_SYMBOL_GPL(cohort_replication_layoutget);


void
dprintk_fh(const char *func, const char *tag, const struct nfs_fh *fh)
{
    int ix, ix2, len, max;
    char *buf;
    u32 *p;

    ix2 = 0;
    max = fh->size - sizeof(u32);
    buf = kzalloc(1024, GFP_KERNEL);
    for (ix = 0, ix2 = 0; ix < max; ix += sizeof(u32), ix2 += len) {
        p = (u32 *) (fh->data)+ix;
        sprintf(buf+ix2, "%x%x%x%x-", p[0], p[1], p[2], p[3]);
        len = strlen(buf+ix2);
    }
    buf[ix2-1] = 0;
    dprintk("%s: %s 0x%p: %s (%d)\n", func, tag, fh, buf, fh->size);;
    kfree(buf);
}
EXPORT_SYMBOL(dprintk_fh);

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

/*
 * Return replication layout(s) for a superblock being unmounted.
 */
void
cohort_rpl_return_layouts(struct super_block *sb)
{
    struct pnfs_layout_range range;
    struct pnfs_layout_hdr *lo = NULL;
    struct pnfs_layout_segment *lseg = NULL;
    struct nfs_server *server = NFS_SERVER_SB(sb);
    struct inode *s_ino = server->s_ino;
    int code;

    dprintk("--> %s\n", __func__);

    /* Do nothing if no super inode */
    if (!s_ino)
        goto out;

    /* Do nothing if no replication layout driver */
    if (!server->pnfs_meta_ld || 
        (server->pnfs_meta_ld->id != LAYOUT4_COHORT_REPLICATION))
        goto out;

    /* Return layout, commit if appropriate */
    range.iomode = IOMODE_RW;
    range.offset = 0ULL;
    range.length = NFS4_MAX_UINT64;

    /* XXX Finish!  The following code has been validated to set up and
     * make potentially valid LAYOUTCOMMIT and LAYOUTRETURN calls.  The
     * code can't be called yet, because:
     * a. Ganesha can't decode either call
     * b. the Linux client retries the operations...forever ??
     * So, we call our own interim cleanup code for now.
     */
#if 0
    code = pnfs_return_layout(s_ino, &range, true);
#else
#if 0 /* and we cant call LAYOUTRETURN either */
    _pnfs_return_layout(rdata->inode, &range, true);
#endif
    /* Find and ref layout for d_ino, if possible */
    spin_lock(&s_ino->i_lock);
    lo = pnfs_find_inode_layout(s_ino);
    if (lo) {
        range.iomode = IOMODE_RW;
        range.offset = 0ULL;
        range.length = NFS4_MAX_UINT64;

        /* balances -initial layoutget- */
        lseg = pnfs_find_lseg(lo, &range);
        if (lseg)
            put_lseg_locked2(lseg);

        /* put hdr */
        put_layout_hdr_locked(lo);
    }
    spin_unlock(&s_ino->i_lock);
#endif

out:
    return;
}
EXPORT_SYMBOL_GPL(cohort_rpl_return_layouts);
