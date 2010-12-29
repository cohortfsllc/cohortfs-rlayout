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

#include <linux/nfs_fs.h>

#include "../internal.h"
#include "../nfs4filelayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Matt Benjamin <matt@linuxbox.com>");
MODULE_DESCRIPTION("The Cohort NFSv4 replication layout driver");

static int
cohort_rpl_set_layoutdriver(struct nfs_server *nfss, const struct nfs_fh *mntfh)
{

        int status = 0; /* XXX do it */
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
_cohort_rpl_free_lseg(struct nfs4_filelayout_segment *fl)
{
	kfree(fl);
}

static int
cohort_rpl_decode_layout(struct pnfs_layout_hdr *flo,
			 struct nfs4_filelayout_segment *fl,
			 struct nfs4_layoutget_res *lgr,
			 struct nfs4_deviceid *id)
{
#if 0
	uint32_t *p = (uint32_t *)lgr->layout.buf;
	uint32_t nfl_util;
	int i;

	dprintk("%s: set_layout_map Begin\n", __func__);

	memcpy(id, p, sizeof(*id));
	p += XDR_QUADLEN(NFS4_DEVICEID4_SIZE);
	print_deviceid(id);

	nfl_util = be32_to_cpup(p++);
	if (nfl_util & NFL4_UFLG_COMMIT_THRU_MDS)
		fl->commit_through_mds = 1;
	if (nfl_util & NFL4_UFLG_DENSE)
		fl->stripe_type = STRIPE_DENSE;
	else
		fl->stripe_type = STRIPE_SPARSE;
	fl->stripe_unit = nfl_util & ~NFL4_UFLG_MASK;

	fl->first_stripe_index = be32_to_cpup(p++);
	p = xdr_decode_hyper(p, &fl->pattern_offset);
	fl->num_fh = be32_to_cpup(p++);

	dprintk("%s: nfl_util 0x%X num_fh %u fsi %u po %llu\n",
		__func__, nfl_util, fl->num_fh, fl->first_stripe_index,
		fl->pattern_offset);

	fl->fh_array = kzalloc(fl->num_fh * sizeof(struct nfs_fh *),
			       GFP_KERNEL);
	if (!fl->fh_array)
		return -ENOMEM;

	for (i = 0; i < fl->num_fh; i++) {
		/* Do we want to use a mempool here? */
		fl->fh_array[i] = kmalloc(sizeof(struct nfs_fh), GFP_KERNEL);
		if (!fl->fh_array[i]) {
			filelayout_free_fh_array(fl);
			return -ENOMEM;
		}
		fl->fh_array[i]->size = be32_to_cpup(p++);
		if (sizeof(struct nfs_fh) < fl->fh_array[i]->size) {
			printk(KERN_ERR "Too big fh %d received %d\n",
			       i, fl->fh_array[i]->size);
			filelayout_free_fh_array(fl);
			return -EIO;
		}
		memcpy(fl->fh_array[i]->data, p, fl->fh_array[i]->size);
		p += XDR_QUADLEN(fl->fh_array[i]->size);
		dprintk("DEBUG: %s: fh len %d\n", __func__,
			fl->fh_array[i]->size);
	}
#endif
	return 0;
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
			struct nfs4_filelayout_segment *fl,
			struct nfs4_layoutget_res *lgr,
			struct nfs4_deviceid *id)
{
	int status = 0;
out:
	dprintk("--> %s returns %d\n", __func__, status);
	return status;
}

static struct pnfs_layout_segment *
cohort_rpl_alloc_lseg(struct pnfs_layout_hdr *layoutid,
		      struct nfs4_layoutget_res *lgr)
{
	struct nfs4_filelayout_segment *fl;
	int rc;
	struct nfs4_deviceid id;

	dprintk("--> %s\n", __func__);
	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return NULL;

	rc = cohort_rpl_decode_layout(layoutid, fl, lgr, &id);
	if (rc != 0 || cohort_rpl_check_layout(layoutid, fl, lgr, &id)) {
		_cohort_rpl_free_lseg(fl);
		return NULL;
	}
	return &fl->generic_hdr;
}

static void
cohort_rpl_free_lseg(struct pnfs_layout_segment *lseg)
{
	struct nfs_server *nfss = NFS_SERVER(lseg->layout->inode);
#if 0
	struct nfs4_filelayout_segment *fl = FILELAYOUT_LSEG(lseg);

	dprintk("--> %s\n", __func__);
	pnfs_put_deviceid(nfss->nfs_client->cl_devid_cache,
			  &fl->dsaddr->deviceid);
	_filelayout_free_lseg(fl);
#endif
}

static void cohort_rpl_commit_call_done(struct rpc_task *task, void *data)
{
#if 0
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	wdata->pdata.call_ops->rpc_call_done(task, data);
#endif
}

/* XXX need new prepare op */
static struct rpc_call_ops filelayout_commit_call_ops = {
        .rpc_call_prepare = NULL /* nfs_write_prepare */,
	.rpc_call_done = cohort_rpl_commit_call_done,
	.rpc_release = NULL /* filelayout_write_release */,
};

/*
 * Ug.  nfs_write_data seems not apropos.
 * Invoke the pnfs_commit_complete callback.
 */
enum pnfs_try_status
cohort_rpl_commit(struct nfs_write_data *data, int sync)
{
	data->pdata.pnfs_error = 0;
	return PNFS_ATTEMPTED;
}

static struct pnfs_layoutdriver_type cohort_rpl_layout_type = {
	.id = LAYOUT4_COHORT_REPLICATION,
	.name = "LAYOUT4_COHORT_REPLICATION",
	.owner = THIS_MODULE,
	.flags                   = PNFS_USE_RPC_CODE,
	.set_layoutdriver = cohort_rpl_set_layoutdriver,
	.clear_layoutdriver = cohort_rpl_clear_layoutdriver,
	.alloc_lseg              = cohort_rpl_alloc_lseg,
	.free_lseg               = cohort_rpl_free_lseg,
	.pg_test                 = NULL,
	.read_pagelist           = NULL,
	.write_pagelist          = NULL,
	.commit                  = cohort_rpl_commit,
};

static int __init cohort_rpl_init(void)
{
	printk(KERN_INFO "%s: Cohort Replication Layout Driver Registering\n",
	       __func__);
        /* We are a layout driver of a novel class, so won't register
         * here. */
#if 0
	return pnfs_register_layoutdriver(&cohort_rpl_layout_type);
#endif
        return (0);
}

static void __exit cohort_rpl_exit(void)
{
	printk(KERN_INFO "%s: Cohort Replication Layout Driver Unregistering\n",
	       __func__);
        /* We are a layout driver of a novel class, so won't register
         * here. */
#if 0
	pnfs_unregister_layoutdriver(&cohort_rpl_layout_type);
#endif
}

module_init(cohort_rpl_init);
module_exit(cohort_rpl_exit);
