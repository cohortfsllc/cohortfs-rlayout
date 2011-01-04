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

static int
cohort_rpl_clear_layoutdriver(struct nfs_server *nfss)
{
	dprintk("--> %s\n", __func__);

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
