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

#include "../internal.h"
#include "../pnfs.h"
#include "../cohort.h"
#include "cohortlayout.h"
#include "../nfs4filelayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

static int
cohort_rpl_set_layoutdriver(struct nfs_server *nfss,
                            const struct nfs_fh *mntfh)
{
    int status;
    status = pnfs_alloc_init_deviceid_cache(
        nfss->nfs_client,
        cohort_rpl_free_deviceid_callback);
    if (status) {
        printk(KERN_WARNING "%s: deviceid cache could not be "
               "initialized\n", __func__);
        return status;
    }
    dprintk("%s: deviceid cache has been initialized successfully\n",
            __func__);
    return 0;
}

/* Clear out the layout by destroying its device list */
static int
cohort_rpl_clear_layoutdriver(struct nfs_server *nfss)
{
	dprintk("--> %s\n", __func__);

	if (nfss->nfs_client->cl_devid_cache)
		pnfs_put_deviceid_cache(nfss->nfs_client);
	return 0;
}

static void
_cohort_rpl_free_lseg(struct cohort_replication_layout_segment *rpl)
{
    /* free any dynamic members of the segment */
    kfree(rpl);
}

static void
cohort_rpl_free_lseg(struct pnfs_layout_segment *lseg)
{
	struct nfs_server *nfss = NFS_SERVER(lseg->layout->inode);
	struct cohort_replication_layout_segment *rpl;

	dprintk("--> %s\n", __func__);

        rpl = COHORT_RPL_LSEG(lseg);
	pnfs_put_deviceid(nfss->nfs_client->cl_devid_cache,
			  &rpl->dsaddr->deviceid);
	_cohort_rpl_free_lseg(rpl);
}

/*
 * cohort_rpl_check_layout()
 *
 * Make sure layout segment parameters are sane WRT the device.
 * At this point no generic layer initialization of the lseg has occurred,
 * and nothing has been added to the layout_hdr cache.
 *
 */
static int
cohort_rpl_check_layout(struct pnfs_layout_hdr *lo,
			struct cohort_replication_layout_segment *rpl,
			struct nfs4_layoutget_res *lgr,
			struct nfs4_deviceid *id)
{
	struct cohort_replication_layout_rmds_addr *dsaddr;
	struct nfs_server *nfss;
	int status = -EINVAL;

	dprintk("--> %s\n", __func__);

	nfss = NFS_SERVER(lo->inode);

	/* find and reference the deviceid */
	dsaddr = cohort_rpl_find_get_deviceid(nfss->nfs_client, id);
	if (dsaddr == NULL) {
		dsaddr = cohort_rpl_get_device_info(lo->inode, id);
		if (dsaddr == NULL)
			goto out;
	}
	rpl->dsaddr = dsaddr;

	status = 0;
out:
	dprintk("--> %s returns %d\n", __func__, status);
	return status;
}

static int
cohort_rpl_decode_layout(struct pnfs_layout_hdr *flo,
			 struct cohort_replication_layout_segment *rpl,
			 struct nfs4_layoutget_res *lgr,
			 struct nfs4_deviceid *id)
{
	uint32_t *p = (uint32_t *)lgr->layout.buf;

	dprintk("--> %s \n", __func__);

	memcpy(id, p, sizeof(*id));
	p += XDR_QUADLEN(NFS4_DEVICEID4_SIZE);
	cohort_rpl_print_deviceid(id);

        memcpy(&rpl->fh.data, p, rpl->fh.size);
        p += XDR_QUADLEN(rpl->fh.size);

	return 0;
}

static struct pnfs_layout_segment *
cohort_rpl_alloc_lseg(struct pnfs_layout_hdr *layoutid,
		      struct nfs4_layoutget_res *lgr)
{
	struct cohort_replication_layout_segment *rpl;
	struct nfs4_deviceid id;
	int rc;

	dprintk("--> %s\n", __func__);

	rpl = kzalloc(sizeof(struct cohort_replication_layout_segment),
                      GFP_KERNEL);
	if (!rpl)
		return NULL;

	rc = cohort_rpl_decode_layout(layoutid, rpl, lgr, &id);
	if (rc != 0 || cohort_rpl_check_layout(layoutid, rpl, lgr, &id)) {
		_cohort_rpl_free_lseg(rpl);
		return NULL;
	}
	return &rpl->generic_hdr;
}

/* Not used. */
static int
cohort_rpl_pg_test(struct nfs_pageio_descriptor *pgio, struct nfs_page *prev,
		   struct nfs_page *req)
{
	return (0);
}

static enum pnfs_try_status
cohort_rpl_read_pagelist(struct nfs_read_data *data, unsigned nr_pages)
{
    return PNFS_ATTEMPTED;
}

static enum pnfs_try_status
cohort_rpl_write_pagelist(struct nfs_write_data *data, unsigned nr_pages,
                          int sync)
{
    return PNFS_ATTEMPTED;
}

static enum pnfs_try_status
cohort_rpl_commit(struct nfs_write_data *data, int sync)
{
    dprintk("--> %s\n", __func__);

    /* XXX Finish */

    return PNFS_ATTEMPTED;
}

/* XXX pretty sure NOT NEEDED */
static int
cohort_rpl_metadata_commit(struct nfs_server *server, int sync)
{
    return (0);
}

/*
 * Generic preamble for Cohort replication operations.
 *
 * Acquire and, where applicable, ref each OUT variable in the appropriate
 * order.  NFS_SERVER(d_ino)->s_ino->i_lock is not locked on entry, nor
 * locked on exit.  If either of lo or lseg is non-NULL on exit, the
 * caller is responsible to call the corresponding put_xxx routine to balance
 * its refcount.
 */
static inline int
cohort_rpl_op_preamble(const char *tag,
                       struct inode *d_ino,
                       /* OUT */
                       struct nfs_server **server,
                       struct inode **s_ino,
                       struct pnfs_layout_hdr **lo,
                       struct pnfs_layout_segment **lseg)
{
    struct pnfs_layout_range range;
    int code;

    dprintk("--> %s\n", __func__);

    code = -EINVAL;

    *lo = NULL;
    *lseg = NULL;
    *server = NFS_SERVER(d_ino);
    *s_ino = (*server) ? (*server)->s_ino : NULL;

    if (! *s_ino) {
        dprintk("%s %s no super s_ino\n", __func__,
                tag);
        goto out_err;
    }

    if (!(*server)->pnfs_meta_ld || 
        ((*server)->pnfs_meta_ld->id != LAYOUT4_COHORT_REPLICATION)) {
        dprintk("%s %s no valid layout (%p)\n", __func__,
                tag,
                (*server)->pnfs_meta_ld);
        goto out_err;
    }

    /* Find and ref layout for d_ino, if possible */
    spin_lock(&(*s_ino)->i_lock);
    *lo = pnfs_find_inode_layout(*s_ino);
    if (! *lo) {
        dprintk("%s %s no valid layout (%p, %p)\n", __func__,
                tag,
                (*server)->pnfs_meta_ld,
                *s_ino);
        goto out_unlock;
    }

    /* Try to find the corresponding layout segment */
    range.iomode = IOMODE_RW;
    range.offset = 0ULL;
    range.length = NFS4_MAX_UINT64;

    *lseg = pnfs_find_lseg(*lo, &range);
    if (*lseg)
        get_lseg(*lseg);

    code = NFS4_OK;
out_unlock:
    spin_unlock(&(*s_ino)->i_lock);
out_err:
    return (code);
}

/*
 * Generic postamble for Cohort replication operations.
 *
 * Unref each of lseg and lo in the appropriate order.  Note s_ino is assumed
 * to be held, but is not unrefed. NFS_SERVER(d_ino)->s_ino->i_lock is not
 * locked on entry, nor is it locked on exit.
 */
static inline int
cohort_rpl_op_postamble(const char *tag,
                        struct inode *d_ino,
                        struct nfs_server *server,
                        struct inode *s_ino,
                        struct pnfs_layout_hdr *lo,
                        struct pnfs_layout_segment *lseg)
{
    int code;

    dprintk("--> %s\n", __func__);

    code = -EINVAL;

    /* !s_ino --> !lo and !lseg */
    if (!s_ino)
        goto out_err;

    /* !i_locked */
    spin_lock(&s_ino->i_lock);

    if (lseg)
        put_lseg_locked2(lseg);

    if (lo)
        put_layout_hdr_locked(lo);

    spin_unlock(&s_ino->i_lock);
    code = NFS4_OK;

out_err:
    return (code);
}

static inline int
nfs41_call_sync(struct nfs_server *server,
                struct nfs_client *client,
                struct rpc_message *msg,
                struct nfs4_sequence_args *seq_args,
                struct nfs4_sequence_res *seq_res,
                int cache_reply)
{
    int code;

    /* set up seq_args with client session */
    seq_args->sa_session = client->cl_session;

    /* call with supplied client */
    code = client->cl_mvops->call_sync(
        server, msg, seq_args, seq_res, cache_reply);

    return (code);
}

/* RINTEGRITY RPC support */

struct cohort_rintegrity_data {
	struct rpc_message msg;
	struct nfs41_rintegrity_arg arg;
	struct nfs41_rintegrity_res res;
};

static struct cohort_rintegrity_data *
cohort_alloc_rintegrity_data()
{
    struct cohort_rintegrity_data *data;

    data = kzalloc(sizeof(struct cohort_rintegrity_data), GFP_KERNEL);
    if (data != NULL) {
        // struct nfs_server *server = NFS_SERVER(dir);

        data->msg.rpc_proc = &nfs4_procedures[NFSPROC4_CLNT_RINTEGRITY];
        data->msg.rpc_argp = &data->arg;
        data->msg.rpc_resp = &data->res;
#if 0 /* XXX Finish! */
        data->arg.fh = fh;
        data->arg.stateid = stateid;
        data->arg.client = client;
#endif
    }
    return data;
}

static inline void
cohort_rpl_updatedata_create(struct nfs4_createdata *data)
{
    data->arg.crt_fh = data->res.fh;
}

static int
cohort_rpl_create(struct nfs_server *server, struct inode *d_ino,
                  struct dentry *dentry, struct nfs4_createdata *data)
{
    struct pnfs_layout_hdr *lo;
    struct pnfs_layout_segment *lseg;
    struct cohort_replication_layout_rmds *rmds;
    struct inode *s_ino = server->s_ino;

    int code2, code = -EINVAL;

    code = cohort_rpl_op_preamble(__func__, d_ino, &server, &s_ino,
                                  &lo, &lseg);
    if (code)
        goto out_postamble;

    /* Call */
    dprintk("%s found replication layout (%p, %p)\n", __func__,
            lo, lseg);

    /* XXX Finish */
    dprintk_fh(__func__, "dir_fh", data->arg.dir_fh);
    dprintk_fh(__func__, "fh", data->res.fh);

    /* Ok, for now, we know 1 is the the fixed ds offset of the replica MDS
     * (0 is offset of the primary MDS). */
    rmds = cohort_rpl_prepare_ds(lseg, 1);
    if (!rmds) {
        dprintk("%s couldnt instantiate replica rmds\n", __func__);
        code = NFS4ERR_STALE;
        goto out_postamble;
    }
    dprintk("%s rmds[1] %p ds_session %p\n", __func__, rmds,
        rmds->ds_client->cl_session);

    /* Update call for mirror at DS (e.g., set FH) */
    cohort_rpl_updatedata_create(data);

    /* Session-aware call_sync wrapper */
    code = nfs41_call_sync(server, rmds->ds_client, &data->msg,
                           &data->arg.seq_args, &data->res.seq_res,
                           1 /* cache reply */);

    if (!code)
        pnfs_need_layoutcommit(NFS_I(s_ino), NULL);

out_postamble:
    code2 = cohort_rpl_op_postamble(__func__, d_ino, server, s_ino,
                                    lo, lseg);

    return (code);
}

static int
cohort_rpl_remove(struct nfs_server *server, struct inode *d_ino,
                  struct rpc_message *msg, struct nfs_removeargs *arg,
                  struct nfs_removeres *res)
{
    struct pnfs_layout_hdr *lo;
    struct pnfs_layout_segment *lseg;
    struct cohort_replication_layout_rmds *rmds;
    struct inode *s_ino = server->s_ino;

    int code2, code = -EINVAL;

    code = cohort_rpl_op_preamble(__func__, d_ino, &server, &s_ino,
                                  &lo, &lseg);
    if (code)
        goto out_postamble;

    dprintk("%s found replication layout (%p, %p)\n", __func__,
            lo, lseg);

    /* Ok, for now, we know 1 is the the fixed ds offset of the replica MDS
     * (0 is offset of the primary MDS). */
    rmds = cohort_rpl_prepare_ds(lseg, 1);
    if (!rmds) {
        dprintk("%s couldnt instantiate replica rmds\n", __func__);
        code = NFS4ERR_STALE;
        goto out_postamble;
    }
    dprintk("%s rmds[1] %p ds_session %p\n", __func__, rmds,
        rmds->ds_client->cl_session);

    /* Session-aware call_sync wrapper */
    code = nfs41_call_sync(server, rmds->ds_client, msg, &arg->seq_args,
                           &res->seq_res,
                           1 /* cache reply */);

    if (!code)
        pnfs_need_layoutcommit(NFS_I(s_ino), NULL);

out_postamble:
    code2 = cohort_rpl_op_postamble(__func__, d_ino, server, s_ino,
                                    lo, lseg);

    return (code);
}

int cohort_rpl_open(struct nfs_server *server, struct inode *d_ino,
                    struct nfs4_opendata *opendata)
{
    int code = 0;

    dprintk("--> %s\n", __func__);

    return (code);
}

static struct pnfs_layoutdriver_type cohort_replication_layout = {
	.id = LAYOUT4_COHORT_REPLICATION,
	.name = "LAYOUT4_COHORT_REPLICATION",
	.owner = THIS_MODULE,
	.flags                   = PNFS_USE_RPC_CODE,
	.set_layoutdriver = cohort_rpl_set_layoutdriver,
	.clear_layoutdriver = cohort_rpl_clear_layoutdriver,
	.alloc_lseg              = cohort_rpl_alloc_lseg,
	.free_lseg               = cohort_rpl_free_lseg,
	.pg_test                 = cohort_rpl_pg_test,
	.read_pagelist           = cohort_rpl_read_pagelist,
	.write_pagelist          = cohort_rpl_write_pagelist,
	.commit                  = cohort_rpl_commit,
	.metadata_commit         = cohort_rpl_metadata_commit,
	.create                  = cohort_rpl_create,
	.remove                  = cohort_rpl_remove,
        .open                    = cohort_rpl_open,
	/* XXX finish! */
};

static
int __init cohort_rpl_init(void)
{
	printk(KERN_INFO "%s: Cohort Replication Layout Driver Init\n",
	       __func__);
	return pnfs_register_layoutdriver(&cohort_replication_layout);
}

static
void __exit cohort_rpl_exit(void)
{
	printk(KERN_INFO "%s: Cohort Replication Layout Driver Exit\n",
	       __func__);
	pnfs_unregister_layoutdriver(&cohort_replication_layout);
}

MODULE_AUTHOR("Matt Benjamin <matt@linuxbox.com>");
MODULE_DESCRIPTION("The Cohort NFSv4 replication layout driver");
MODULE_LICENSE("GPL");

module_init(cohort_rpl_init);
module_exit(cohort_rpl_exit);
