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
#include "../nfs4filelayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

static int
cohort_rpl_set_layoutdriver(struct nfs_server *nfss,
                            const struct nfs_fh *mntfh)
{
    /* It s not clear to me whether this is meaningful for metadata layouts,
     * or if signature is correct.  Possibly the right signature, but should
     * cache on superblock */
#if 0
	int status = pnfs_alloc_init_deviceid_cache(nfss->nfs_client,
						nfs4_fl_free_deviceid_callback);
	if (status) {
		printk(KERN_WARNING "%s: deviceid cache could not be "
			"initialized\n", __func__);
		return status;
	}
	dprintk("%s: deviceid cache has been initialized successfully\n",
		__func__);
#endif
	return 0;
}

static int
cohort_rpl_clear_layoutdriver(struct nfs_server *nfss)
{
	dprintk("--> %s\n", __func__);

	return 0;
}

static struct pnfs_layout_segment *
cohort_rpl_alloc_lseg(struct pnfs_layout_hdr *layoutid,
		      struct nfs4_layoutget_res *lgr)
{
#if 0
	struct nfs4_filelayout_segment *fl;
	int rc;
	struct nfs4_deviceid id;
#endif

	dprintk("--> %s\n", __func__);
#if 0
	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return NULL;

	rc = filelayout_decode_layout(layoutid, fl, lgr, &id);
	if (rc != 0 || filelayout_check_layout(layoutid, fl, lgr, &id)) {
		_filelayout_free_lseg(fl);
		return NULL;
	}
	return &fl->generic_hdr;
#endif
        return NULL;
}

static void
cohort_rpl_free_lseg(struct pnfs_layout_segment *lseg)
{
#if 0
	struct nfs_server *nfss = NFS_SERVER(lseg->layout->inode);
	struct nfs4_filelayout_segment *fl = FILELAYOUT_LSEG(lseg);
#endif
	dprintk("--> %s\n", __func__);
#if 0
	pnfs_put_deviceid(nfss->nfs_client->cl_devid_cache,
			  &fl->dsaddr->deviceid);
	_filelayout_free_lseg(fl);
	kfree(fl);
#endif
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
    return PNFS_ATTEMPTED;
}

static enum pnfs_try_status
cohort_rpl_metadata_commit(struct nfs_server *server, int sync)
{
    return PNFS_ATTEMPTED;
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
