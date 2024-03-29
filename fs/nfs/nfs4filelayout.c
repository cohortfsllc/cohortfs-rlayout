/*
 *  Module for the pnfs nfs4 file layout driver.
 *  Defines all I/O and Policy interface operations, plus code
 *  to register itself with the pNFS client.
 *
 *  Copyright (c) 2002
 *  The Regents of the University of Michigan
 *  All Rights Reserved
 *
 *  Dean Hildebrand <dhildebz@umich.edu>
 *
 *  Permission is granted to use, copy, create derivative works, and
 *  redistribute this software and such derivative works for any purpose,
 *  so long as the name of the University of Michigan is not used in
 *  any advertising or publicity pertaining to the use or distribution
 *  of this software without specific, written prior authorization. If
 *  the above copyright notice or any other identification of the
 *  University of Michigan is included in any copy of any portion of
 *  this software, then the disclaimer below must also be included.
 *
 *  This software is provided as is, without representation or warranty
 *  of any kind either express or implied, including without limitation
 *  the implied warranties of merchantability, fitness for a particular
 *  purpose, or noninfringement.  The Regents of the University of
 *  Michigan shall not be liable for any damages, including special,
 *  indirect, incidental, or consequential damages, with respect to any
 *  claim arising out of or in connection with the use of the software,
 *  even if it has been or is hereafter advised of the possibility of
 *  such damages.
 */

#include <linux/nfs_fs.h>

#include "internal.h"
#include "nfs4filelayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dean Hildebrand <dhildebz@umich.edu>");
MODULE_DESCRIPTION("The NFSv4 file layout driver");

static int
filelayout_set_layoutdriver(struct nfs_server *nfss, const struct nfs_fh *mntfh)
{
	int status = pnfs_alloc_init_deviceid_cache(nfss->nfs_client,
						nfs4_fl_free_deviceid_callback);
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
filelayout_clear_layoutdriver(struct nfs_server *nfss)
{
	dprintk("--> %s\n", __func__);

	if (nfss->nfs_client->cl_devid_cache)
		pnfs_put_deviceid_cache(nfss->nfs_client);
	return 0;
}

/* This function is used by the layout driver to calculate the
 * offset of the file on the dserver based on whether the
 * layout type is STRIPE_DENSE or STRIPE_SPARSE
 */
static loff_t
filelayout_get_dserver_offset(struct pnfs_layout_segment *lseg, loff_t offset)
{
	struct nfs4_filelayout_segment *flseg = FILELAYOUT_LSEG(lseg);

	switch (flseg->stripe_type) {
	case STRIPE_SPARSE:
		return offset;

	case STRIPE_DENSE:
	{
		u32 stripe_width;
		u64 tmp, off;
		u32 unit = flseg->stripe_unit;

		stripe_width = unit * flseg->dsaddr->stripe_count;
		tmp = off = offset - flseg->pattern_offset;
		do_div(tmp, stripe_width);
		return tmp * unit + do_div(off, unit);
	}
	default:
		BUG();
	}

	/* We should never get here... just to stop the gcc warning */
	return 0;
}

/*
 * Call ops for the async read/write cases
 * In the case of dense layouts, the offset needs to be reset to its
 * original value.
 */
static void filelayout_read_call_done(struct rpc_task *task, void *data)
{
	struct nfs_read_data *rdata = (struct nfs_read_data *)data;

	if (rdata->fldata.orig_offset) {
		dprintk("%s new off %llu orig offset %llu\n", __func__,
			rdata->args.offset, rdata->fldata.orig_offset);
		rdata->args.offset = rdata->fldata.orig_offset;
	}

	/* Note this may cause RPC to be resent */
	rdata->pdata.call_ops->rpc_call_done(task, data);
}

static void filelayout_read_release(void *data)
{
	struct nfs_read_data *rdata = (struct nfs_read_data *)data;

	put_lseg(rdata->pdata.lseg);
	rdata->pdata.lseg = NULL;
	rdata->pdata.call_ops->rpc_release(data);
}

static void filelayout_write_call_done(struct rpc_task *task, void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	if (wdata->fldata.orig_offset) {
		dprintk("%s new off %llu orig offset %llu\n", __func__,
			wdata->args.offset, wdata->fldata.orig_offset);
		wdata->args.offset = wdata->fldata.orig_offset;
	}

	/* Note this may cause RPC to be resent */
	wdata->pdata.call_ops->rpc_call_done(task, data);
}

static void filelayout_write_release(void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	put_lseg(wdata->pdata.lseg);
	wdata->pdata.lseg = NULL;
	wdata->pdata.call_ops->rpc_release(data);
}

struct rpc_call_ops filelayout_read_call_ops = {
	.rpc_call_prepare = nfs_read_prepare,
	.rpc_call_done = filelayout_read_call_done,
	.rpc_release = filelayout_read_release,
};

struct rpc_call_ops filelayout_write_call_ops = {
	.rpc_call_prepare = nfs_write_prepare,
	.rpc_call_done = filelayout_write_call_done,
	.rpc_release = filelayout_write_release,
};

/* Perform sync or async reads.
 *
 * An optimization for the NFS file layout driver
 * allows the original read/write data structs to be passed in the
 * last argument.
 *
 * TODO: join with write_pagelist?
 */
static enum pnfs_try_status
filelayout_read_pagelist(struct nfs_read_data *data, unsigned nr_pages)
{
	struct pnfs_layout_segment *lseg = data->pdata.lseg;
	struct nfs4_pnfs_ds *ds;
	loff_t offset = data->args.offset;
	u32 idx;
	struct nfs_fh *fh;

	dprintk("--> %s ino %lu nr_pages %d pgbase %u req %Zu@%llu\n",
		__func__, data->inode->i_ino, nr_pages,
		data->args.pgbase, (size_t)data->args.count, offset);

	/* Retrieve the correct rpc_client for the byte range */
	idx = nfs4_fl_calc_ds_index(lseg, offset);
	ds = nfs4_fl_prepare_ds(lseg, idx);
	if (!ds) {
		printk(KERN_ERR "%s: prepare_ds failed, use MDS\n", __func__);
		return PNFS_NOT_ATTEMPTED;
	}
	dprintk("%s USE DS:ip %x %hu\n", __func__,
		ntohl(ds->ds_ip_addr), ntohs(ds->ds_port));

	/* just try the first data server for the index..*/
	data->fldata.ds_nfs_client = ds->ds_clp;
	fh = nfs4_fl_select_ds_fh(lseg, offset);
	if (fh)
		data->args.fh = fh;

	/*
	 * Now get the file offset on the dserver
	 * Set the read offset to this offset, and
	 * save the original offset in orig_offset
	 * In the case of aync reads, the offset will be reset in the
	 * call_ops->rpc_call_done() routine.
	 */
	data->args.offset = filelayout_get_dserver_offset(lseg, offset);
	data->fldata.orig_offset = offset;

	/* Perform an asynchronous read */
	nfs_initiate_read(data, ds->ds_clp->cl_rpcclient,
			  &filelayout_read_call_ops);

	data->pdata.pnfs_error = 0;

	return PNFS_ATTEMPTED;
}

/* Perform async writes. */
static enum pnfs_try_status
filelayout_write_pagelist(struct nfs_write_data *data, unsigned nr_pages, int sync)
{
	struct pnfs_layout_segment *lseg = data->pdata.lseg;
	struct nfs4_pnfs_ds *ds;
	loff_t offset = data->args.offset;
	u32 idx;
	struct nfs_fh *fh;

	/* Retrieve the correct rpc_client for the byte range */
	idx = nfs4_fl_calc_ds_index(lseg, offset);
	ds = nfs4_fl_prepare_ds(lseg, idx);
	if (!ds) {
		printk(KERN_ERR "%s: prepare_ds failed, use MDS\n", __func__);
		return PNFS_NOT_ATTEMPTED;
	}
	dprintk("%s ino %lu sync %d req %Zu@%llu DS:%x:%hu\n", __func__,
		data->inode->i_ino, sync, (size_t) data->args.count, offset,
		ntohl(ds->ds_ip_addr), ntohs(ds->ds_port));

	data->fldata.ds_nfs_client = ds->ds_clp;
	fh = nfs4_fl_select_ds_fh(lseg, offset);
	if (fh)
		data->args.fh = fh;
	/*
	 * Get the file offset on the dserver. Set the write offset to
	 * this offset and save the original offset.
	 */
	data->args.offset = filelayout_get_dserver_offset(lseg, offset);
	data->fldata.orig_offset = offset;

	/*
	 * Perform an asynchronous write The offset will be reset in the
	 * call_ops->rpc_call_done() routine
	 */
	nfs_initiate_write(data, ds->ds_clp->cl_rpcclient,
			   &filelayout_write_call_ops, sync);

	data->pdata.pnfs_error = 0;
	return PNFS_ATTEMPTED;
}

/*
 * filelayout_check_layout()
 *
 * Make sure layout segment parameters are sane WRT the device.
 * At this point no generic layer initialization of the lseg has occurred,
 * and nothing has been added to the layout_hdr cache.
 *
 */
static int
filelayout_check_layout(struct pnfs_layout_hdr *lo,
			struct nfs4_filelayout_segment *fl,
			struct nfs4_layoutget_res *lgr,
			struct nfs4_deviceid *id)
{
	struct nfs4_file_layout_dsaddr *dsaddr;
	int status = -EINVAL;
	struct nfs_server *nfss = NFS_SERVER(lo->inode);

	dprintk("--> %s\n", __func__);

	if (fl->pattern_offset > lgr->range.offset) {
		dprintk("%s pattern_offset %lld to large\n",
				__func__, fl->pattern_offset);
		goto out;
	}

	if (fl->stripe_unit % PAGE_SIZE) {
		dprintk("%s Stripe unit (%u) not page aligned\n",
			__func__, fl->stripe_unit);
		goto out;
	}

	/* find and reference the deviceid */
	dsaddr = nfs4_fl_find_get_deviceid(nfss->nfs_client, id);
	if (dsaddr == NULL) {
		dsaddr = get_device_info(lo->inode, id);
		if (dsaddr == NULL)
			goto out;
	}
	fl->dsaddr = dsaddr;

	if (fl->first_stripe_index < 0 ||
	    fl->first_stripe_index >= dsaddr->stripe_count) {
		dprintk("%s Bad first_stripe_index %d\n",
				__func__, fl->first_stripe_index);
		goto out_put;
	}

	if ((fl->stripe_type == STRIPE_SPARSE &&
	    fl->num_fh > 1 && fl->num_fh != dsaddr->ds_num) ||
	    (fl->stripe_type == STRIPE_DENSE &&
	    fl->num_fh != dsaddr->stripe_count)) {
		dprintk("%s num_fh %u not valid for given packing\n",
			__func__, fl->num_fh);
		goto out_put;
	}

	if (fl->stripe_unit % nfss->rsize || fl->stripe_unit % nfss->wsize) {
		dprintk("%s Stripe unit (%u) not aligned with rsize %u "
			"wsize %u\n", __func__, fl->stripe_unit, nfss->rsize,
			nfss->wsize);
	}

	status = 0;
out:
	dprintk("--> %s returns %d\n", __func__, status);
	return status;
out_put:
	pnfs_put_deviceid(nfss->nfs_client->cl_devid_cache, &dsaddr->deviceid);
	goto out;
}

static void filelayout_free_fh_array(struct nfs4_filelayout_segment *fl)
{
	int i;

	for (i = 0; i < fl->num_fh; i++) {
		if (!fl->fh_array[i])
			break;
		kfree(fl->fh_array[i]);
	}
	kfree(fl->fh_array);
	fl->fh_array = NULL;
}

static void
_filelayout_free_lseg(struct nfs4_filelayout_segment *fl)
{
	filelayout_free_fh_array(fl);
	kfree(fl);
}

static int
filelayout_decode_layout(struct pnfs_layout_hdr *flo,
			 struct nfs4_filelayout_segment *fl,
			 struct nfs4_layoutget_res *lgr,
			 struct nfs4_deviceid *id)
{
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

	return 0;
}

static struct pnfs_layout_segment *
filelayout_alloc_lseg(struct pnfs_layout_hdr *layoutid,
		      struct nfs4_layoutget_res *lgr)
{
	struct nfs4_filelayout_segment *fl;
	int rc;
	struct nfs4_deviceid id;

	dprintk("--> %s\n", __func__);
	fl = kzalloc(sizeof(*fl), GFP_KERNEL);
	if (!fl)
		return NULL;

	rc = filelayout_decode_layout(layoutid, fl, lgr, &id);
	if (rc != 0 || filelayout_check_layout(layoutid, fl, lgr, &id)) {
		_filelayout_free_lseg(fl);
		return NULL;
	}
	return &fl->generic_hdr;
}

static void
filelayout_free_lseg(struct pnfs_layout_segment *lseg)
{
	struct nfs_server *nfss = NFS_SERVER(lseg->layout->inode);
	struct nfs4_filelayout_segment *fl = FILELAYOUT_LSEG(lseg);

	dprintk("--> %s\n", __func__);
	pnfs_put_deviceid(nfss->nfs_client->cl_devid_cache,
			  &fl->dsaddr->deviceid);
	_filelayout_free_lseg(fl);
}

/* Allocate a new nfs_write_data struct and initialize */
static struct nfs_write_data *
filelayout_clone_write_data(struct nfs_write_data *old)
{
	static struct nfs_write_data *new;

	new = nfs_commitdata_alloc();
	if (!new)
		goto out;
	kref_init(&new->refcount);
	new->parent      = old;
	kref_get(&old->refcount);
	new->inode       = old->inode;
	new->cred        = old->cred;
	new->args.offset = 0;
	new->args.count  = 0;
	new->res.count   = 0;
	new->res.fattr   = &new->fattr;
	nfs_fattr_init(&new->fattr);
	new->res.verf    = &new->verf;
	new->args.context = get_nfs_open_context(old->args.context);
	new->pdata.lseg = NULL;
	new->pdata.call_ops = old->pdata.call_ops;
	new->pdata.how = old->pdata.how;
out:
	return new;
}

static void filelayout_commit_call_done(struct rpc_task *task, void *data)
{
	struct nfs_write_data *wdata = (struct nfs_write_data *)data;

	wdata->pdata.call_ops->rpc_call_done(task, data);
}

static struct rpc_call_ops filelayout_commit_call_ops = {
	.rpc_call_prepare = nfs_write_prepare,
	.rpc_call_done = filelayout_commit_call_done,
	.rpc_release = filelayout_write_release,
};

/*
 * Execute a COMMIT op to the MDS or to each data server on which a page
 * in 'pages' exists.
 * Invoke the pnfs_commit_complete callback.
 */
enum pnfs_try_status
filelayout_commit(struct nfs_write_data *data, int sync)
{
	LIST_HEAD(head);
	struct nfs_page *req;
	loff_t file_offset = 0;
	u16 idx, i;
	struct list_head **ds_page_list = NULL;
	u16 *indices_used;
	int num_indices_seen = 0;
	bool used_mds = false;
	const struct rpc_call_ops *call_ops;
	struct rpc_clnt *clnt;
	struct nfs_write_data **clone_list = NULL;
	struct nfs_write_data *dsdata;
	struct nfs4_pnfs_ds *ds;

	dprintk("%s data %p sync %d\n", __func__, data, sync);

	/* Alloc room for both in one go */
	ds_page_list = kzalloc((NFS4_PNFS_MAX_MULTI_CNT + 1) *
			       (sizeof(u16) + sizeof(struct list_head *)),
			       GFP_KERNEL);
	if (!ds_page_list)
		goto mem_error;
	indices_used = (u16 *) (ds_page_list + NFS4_PNFS_MAX_MULTI_CNT + 1);
	/*
	 * Sort pages based on which ds to send to.
	 * MDS is given index equal to NFS4_PNFS_MAX_MULTI_CNT.
	 * Note we are assuming there is only a single lseg in play.
	 * When that is not true, we could first sort on lseg, then
	 * sort within each as we do here.
	 */
	while (!list_empty(&data->pages)) {
		req = nfs_list_entry(data->pages.next);
		nfs_list_remove_request(req);
		if (!req->wb_lseg ||
		    ((struct nfs4_filelayout_segment *)
		     FILELAYOUT_LSEG(req->wb_lseg))->commit_through_mds)
			idx = NFS4_PNFS_MAX_MULTI_CNT;
		else {
			file_offset = (loff_t)req->wb_index << PAGE_CACHE_SHIFT;
			idx = nfs4_fl_calc_ds_index(req->wb_lseg, file_offset);
		}
		if (ds_page_list[idx]) {
			/* Already seen this idx */
			list_add(&req->wb_list, ds_page_list[idx]);
		} else {
			/* New idx not seen so far */
			list_add_tail(&req->wb_list, &head);
			indices_used[num_indices_seen++] = idx;
		}
		ds_page_list[idx] = &req->wb_list;
	}
	/* Once created, clone must be released via call_op */
	clone_list = kzalloc(num_indices_seen *
			     sizeof(struct nfs_write_data *), GFP_KERNEL);
	if (!clone_list)
		goto mem_error;
	for (i = 0; i < num_indices_seen - 1; i++) {
		if (indices_used[i] == NFS4_PNFS_MAX_MULTI_CNT) {
			used_mds = true;
			clone_list[i] = data;
		} else {
			clone_list[i] = filelayout_clone_write_data(data);
			if (!clone_list[i])
				goto mem_error;
		}
	}
	if (used_mds) {
		clone_list[i] = filelayout_clone_write_data(data);
		if (!clone_list[i])
			goto mem_error;
	} else
		clone_list[i] = data;
	/*
	 * Now send off the RPCs to each ds.  Note that it is important
	 * that any RPC to the MDS be sent last (or at least after all
	 * clones have been made.)
	 */
	for (i = 0; i < num_indices_seen; i++) {
		dsdata = clone_list[i];
		idx = indices_used[i];
		list_cut_position(&dsdata->pages, &head, ds_page_list[idx]);
		if (idx == NFS4_PNFS_MAX_MULTI_CNT) {
			call_ops = data->pdata.call_ops;;
			clnt = NFS_CLIENT(dsdata->inode);
			ds = NULL;
		} else {
			struct nfs_fh *fh;

			call_ops = &filelayout_commit_call_ops;
			req = nfs_list_entry(dsdata->pages.next);
			ds = nfs4_fl_prepare_ds(req->wb_lseg, idx);
			if (!ds) {
				/* Trigger retry of this chunk through MDS */
				dsdata->task.tk_status = -EIO;
				data->pdata.call_ops->rpc_release(dsdata);
				continue;
			}
			clnt = ds->ds_clp->cl_rpcclient;
			dsdata->fldata.ds_nfs_client = ds->ds_clp;
			file_offset = (loff_t)req->wb_index << PAGE_CACHE_SHIFT;
			fh = nfs4_fl_select_ds_fh(req->wb_lseg, file_offset);
			if (fh)
				dsdata->args.fh = fh;
		}
		dprintk("%s: Initiating commit: %llu USE DS:\n",
			__func__, file_offset);
		ifdebug(FACILITY)
			print_ds(ds);

		/* Send COMMIT to data server */
		nfs_initiate_commit(dsdata, clnt, call_ops, sync);
	}
	kfree(clone_list);
	kfree(ds_page_list);
	data->pdata.pnfs_error = 0;
	return PNFS_ATTEMPTED;

 mem_error:
	if (clone_list) {
		for (i = 0; i < num_indices_seen - 1; i++) {
			if (!clone_list[i])
				break;
			data->pdata.call_ops->rpc_release(clone_list[i]);
		}
		kfree(clone_list);
	}
	kfree(ds_page_list);
	/* One of these will be empty, but doesn't hurt to do both */
	nfs_mark_list_commit(&head);
	nfs_mark_list_commit(&data->pages);
	data->pdata.call_ops->rpc_release(data);
	return PNFS_ATTEMPTED;
}

/*
 * filelayout_pg_test(). Called by nfs_can_coalesce_requests()
 *
 * return 1 :  coalesce page
 * return 0 :  don't coalesce page
 *
 * By the time this is called, we know req->wb_lseg == prev->wb_lseg
 */
int
filelayout_pg_test(struct nfs_pageio_descriptor *pgio, struct nfs_page *prev,
		   struct nfs_page *req)
{
	u64 p_stripe, r_stripe;
	u32 stripe_unit;

	if (!req->wb_lseg)
		return 1;
	p_stripe = (u64)prev->wb_index << PAGE_CACHE_SHIFT;
	r_stripe = (u64)req->wb_index << PAGE_CACHE_SHIFT;
	stripe_unit = FILELAYOUT_LSEG(req->wb_lseg)->stripe_unit;

	do_div(p_stripe, stripe_unit);
	do_div(r_stripe, stripe_unit);

	return (p_stripe == r_stripe);
}

static struct pnfs_layoutdriver_type filelayout_type = {
	.id = LAYOUT_NFSV4_1_FILES,
	.name = "LAYOUT_NFSV4_1_FILES",
	.owner = THIS_MODULE,
	.flags                   = PNFS_USE_RPC_CODE,
	.set_layoutdriver = filelayout_set_layoutdriver,
	.clear_layoutdriver = filelayout_clear_layoutdriver,
	.alloc_lseg              = filelayout_alloc_lseg,
	.free_lseg               = filelayout_free_lseg,
	.pg_test                 = filelayout_pg_test,
	.read_pagelist           = filelayout_read_pagelist,
	.write_pagelist          = filelayout_write_pagelist,
	.commit                  = filelayout_commit,
};

static int __init nfs4filelayout_init(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Registering...\n",
	       __func__);
	return pnfs_register_layoutdriver(&filelayout_type);
}

static void __exit nfs4filelayout_exit(void)
{
	printk(KERN_INFO "%s: NFSv4 File Layout Driver Unregistering...\n",
	       __func__);
	pnfs_unregister_layoutdriver(&filelayout_type);
}

module_init(nfs4filelayout_init);
module_exit(nfs4filelayout_exit);
