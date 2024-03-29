/*
 * linux/fs/nfs/read.c
 *
 * Block I/O for NFS
 *
 * Partial copy of Linus' read cache modifications to fs/nfs/file.c
 * modified for async RPC by okir@monad.swb.de
 */

#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/sunrpc/clnt.h>
#include <linux/nfs_fs.h>
#include <linux/nfs_page.h>
#include <linux/smp_lock.h>
#include <linux/module.h>

#include <asm/system.h>
#include <linux/module.h>
#include "pnfs.h"

#include "nfs4_fs.h"
#include "internal.h"
#include "iostat.h"
#include "fscache.h"

#define NFSDBG_FACILITY		NFSDBG_PAGECACHE

static int nfs_pagein_multi(struct inode *, struct list_head *, unsigned int, size_t, int);
static int nfs_pagein_one(struct inode *, struct list_head *, unsigned int, size_t, int);
static const struct rpc_call_ops nfs_read_partial_ops;
static const struct rpc_call_ops nfs_read_full_ops;

static struct kmem_cache *nfs_rdata_cachep;
static mempool_t *nfs_rdata_mempool;

#define MIN_POOL_READ	(32)

struct nfs_read_data *nfs_readdata_alloc(unsigned int pagecount)
{
	struct nfs_read_data *p = mempool_alloc(nfs_rdata_mempool, GFP_KERNEL);

	if (p) {
		memset(p, 0, sizeof(*p));
		INIT_LIST_HEAD(&p->pages);
		p->npages = pagecount;
		if (pagecount <= ARRAY_SIZE(p->page_array))
			p->pagevec = p->page_array;
		else {
			p->pagevec = kcalloc(pagecount, sizeof(struct page *), GFP_KERNEL);
			if (!p->pagevec) {
				mempool_free(p, nfs_rdata_mempool);
				p = NULL;
			}
		}
	}
	return p;
}

void nfs_readdata_free(struct nfs_read_data *p)
{
	if (p && (p->pagevec != &p->page_array[0]))
		kfree(p->pagevec);
	mempool_free(p, nfs_rdata_mempool);
}

static void nfs_readdata_release(struct nfs_read_data *rdata)
{
	put_nfs_open_context(rdata->args.context);
	nfs_readdata_free(rdata);
}

static
int nfs_return_empty_page(struct page *page)
{
	zero_user(page, 0, PAGE_CACHE_SIZE);
	SetPageUptodate(page);
	unlock_page(page);
	return 0;
}

static void nfs_readpage_truncate_uninitialised_page(struct nfs_read_data *data)
{
	unsigned int remainder = data->args.count - data->res.count;
	unsigned int base = data->args.pgbase + data->res.count;
	unsigned int pglen;
	struct page **pages;

	if (data->res.eof == 0 || remainder == 0)
		return;
	/*
	 * Note: "remainder" can never be negative, since we check for
	 * 	this in the XDR code.
	 */
	pages = &data->args.pages[base >> PAGE_CACHE_SHIFT];
	base &= ~PAGE_CACHE_MASK;
	pglen = PAGE_CACHE_SIZE - base;
	for (;;) {
		if (remainder <= pglen) {
			zero_user(*pages, base, remainder);
			break;
		}
		zero_user(*pages, base, pglen);
		pages++;
		remainder -= pglen;
		pglen = PAGE_CACHE_SIZE;
		base = 0;
	}
}

int nfs_readpage_async(struct nfs_open_context *ctx, struct inode *inode,
		       struct page *page)
{
	LIST_HEAD(one_request);
	struct nfs_page	*new;
	unsigned int len;
	loff_t pgoffs;
	struct pnfs_layout_segment *lseg;

	len = nfs_page_length(page);
	if (len == 0)
		return nfs_return_empty_page(page);
	pgoffs = (loff_t)page->index << PAGE_CACHE_SHIFT;
	lseg = pnfs_update_layout(inode, ctx, pgoffs, len, IOMODE_READ);
	new = nfs_create_request(ctx, inode, page, 0, len, lseg);
	put_lseg(lseg);
	if (IS_ERR(new)) {
		unlock_page(page);
		return PTR_ERR(new);
	}
	if (len < PAGE_CACHE_SIZE)
		zero_user_segment(page, len, PAGE_CACHE_SIZE);

	nfs_list_add_request(new, &one_request);
	if (NFS_SERVER(inode)->rsize < PAGE_CACHE_SIZE)
		nfs_pagein_multi(inode, &one_request, 1, len, 0);
	else
		nfs_pagein_one(inode, &one_request, 1, len, 0);
	return 0;
}

static void nfs_readpage_release(struct nfs_page *req)
{
	struct inode *d_inode = req->wb_context->path.dentry->d_inode;

	if (PageUptodate(req->wb_page))
		nfs_readpage_to_fscache(d_inode, req->wb_page, 0);

	unlock_page(req->wb_page);

	dprintk("NFS: read done (%s/%Ld %d@%Ld)\n",
			req->wb_context->path.dentry->d_inode->i_sb->s_id,
			(long long)NFS_FILEID(req->wb_context->path.dentry->d_inode),
			req->wb_bytes,
			(long long)req_offset(req));
	nfs_clear_request(req);
	nfs_release_request(req);
}

int nfs_initiate_read(struct nfs_read_data *data, struct rpc_clnt *clnt,
		      const struct rpc_call_ops *call_ops)
{
	struct inode *inode = data->inode;
	int swap_flags = IS_SWAPFILE(inode) ? NFS_RPC_SWAPFLAGS : 0;
	struct rpc_task *task;
	struct rpc_message msg = {
		.rpc_argp = &data->args,
		.rpc_resp = &data->res,
		.rpc_cred = data->cred,
	};
	struct rpc_task_setup task_setup_data = {
		.task = &data->task,
		.rpc_client = clnt,
		.rpc_message = &msg,
		.callback_ops = call_ops,
		.callback_data = data,
		.workqueue = nfsiod_workqueue,
		.flags = RPC_TASK_ASYNC | swap_flags,
	};

	/* Set up the initial task struct. */
	NFS_PROTO(inode)->read_setup(data, &msg);

	dprintk("NFS: %5u initiated read call (req %s/%Ld, %u bytes @ offset %Lu)\n",
			data->task.tk_pid,
			inode->i_sb->s_id,
			(long long)NFS_FILEID(inode),
			data->args.count,
			(unsigned long long)data->args.offset);

	task = rpc_run_task(&task_setup_data);
	if (IS_ERR(task))
		return PTR_ERR(task);
	rpc_put_task(task);
	return 0;
}
EXPORT_SYMBOL(nfs_initiate_read);

int pnfs_initiate_read(struct nfs_read_data *data, struct rpc_clnt *clnt,
		       const struct rpc_call_ops *call_ops)
{
	if (data->req->wb_lseg &&
	    (pnfs_try_to_read_data(data, call_ops) == PNFS_ATTEMPTED))
		return pnfs_get_read_status(data);

	return nfs_initiate_read(data, clnt, call_ops);
}

/*
 * Set up the NFS read request struct
 */
static int nfs_read_rpcsetup(struct nfs_page *req, struct nfs_read_data *data,
		const struct rpc_call_ops *call_ops,
		unsigned int count, unsigned int offset)
{
	struct inode *inode = req->wb_context->path.dentry->d_inode;

	data->req	  = req;
	data->inode	  = inode;
	data->cred	  = req->wb_context->cred;

	data->args.fh     = NFS_FH(inode);
	data->args.offset = req_offset(req) + offset;
	data->args.pgbase = req->wb_pgbase + offset;
	data->args.pages  = data->pagevec;
	data->args.count  = count;
	data->args.context = get_nfs_open_context(req->wb_context);
	data->args.lock_context = req->wb_lock_context;

	data->res.fattr   = &data->fattr;
	data->res.count   = count;
	data->res.eof     = 0;
	nfs_fattr_init(&data->fattr);

	return pnfs_initiate_read(data, NFS_CLIENT(inode), call_ops);
}

static void
nfs_async_read_error(struct list_head *head)
{
	struct nfs_page	*req;

	while (!list_empty(head)) {
		req = nfs_list_entry(head->next);
		nfs_list_remove_request(req);
		SetPageError(req->wb_page);
		nfs_readpage_release(req);
	}
}

/*
 * Generate multiple requests to fill a single page.
 *
 * We optimize to reduce the number of read operations on the wire.  If we
 * detect that we're reading a page, or an area of a page, that is past the
 * end of file, we do not generate NFS read operations but just clear the
 * parts of the page that would have come back zero from the server anyway.
 *
 * We rely on the cached value of i_size to make this determination; another
 * client can fill pages on the server past our cached end-of-file, but we
 * won't see the new data until our attribute cache is updated.  This is more
 * or less conventional NFS client behavior.
 */
static int nfs_pagein_multi(struct inode *inode, struct list_head *head, unsigned int npages, size_t count, int flags)
{
	struct nfs_page *req = nfs_list_entry(head->next);
	struct page *page = req->wb_page;
	struct nfs_read_data *data;
	size_t rsize = NFS_SERVER(inode)->rsize, nbytes;
	unsigned int offset;
	int requests = 0;
	int ret = 0;
	LIST_HEAD(list);

	nfs_list_remove_request(req);

	nbytes = count;
	do {
		size_t len = min(nbytes,rsize);

		data = nfs_readdata_alloc(1);
		if (!data)
			goto out_bad;
		list_add(&data->pages, &list);
		requests++;
		nbytes -= len;
	} while(nbytes != 0);
	atomic_set(&req->wb_complete, requests);

	ClearPageError(page);
	offset = 0;
	nbytes = count;
	do {
		int ret2;

		data = list_entry(list.next, struct nfs_read_data, pages);
		list_del_init(&data->pages);

		data->pagevec[0] = page;

		if (nbytes < rsize)
			rsize = nbytes;
		ret2 = nfs_read_rpcsetup(req, data, &nfs_read_partial_ops,
				  rsize, offset);
		if (ret == 0)
			ret = ret2;
		offset += rsize;
		nbytes -= rsize;
	} while (nbytes != 0);

	return ret;

out_bad:
	while (!list_empty(&list)) {
		data = list_entry(list.next, struct nfs_read_data, pages);
		list_del(&data->pages);
		nfs_readdata_free(data);
	}
	SetPageError(page);
	nfs_readpage_release(req);
	return -ENOMEM;
}

static int nfs_pagein_one(struct inode *inode, struct list_head *head, unsigned int npages, size_t count, int flags)
{
	struct nfs_page		*req;
	struct page		**pages;
	struct nfs_read_data	*data;
	int ret = -ENOMEM;

	data = nfs_readdata_alloc(npages);
	if (!data)
		goto out_bad;

	pages = data->pagevec;
	while (!list_empty(head)) {
		req = nfs_list_entry(head->next);
		nfs_list_remove_request(req);
		nfs_list_add_request(req, &data->pages);
		ClearPageError(req->wb_page);
		*pages++ = req->wb_page;
	}
	req = nfs_list_entry(data->pages.next);

	return nfs_read_rpcsetup(req, data, &nfs_read_full_ops, count, 0);
out_bad:
	nfs_async_read_error(head);
	return ret;
}

/*
 * This is the callback from RPC telling us whether a reply was
 * received or some error occurred (timeout or socket shutdown).
 */
int nfs_readpage_result(struct rpc_task *task, struct nfs_read_data *data)
{
	int status;

	dprintk("NFS: %s: %5u, (status %d)\n", __func__, task->tk_pid,
			task->tk_status);

	status = NFS_PROTO(data->inode)->read_done(task, data);
	if (status != 0)
		return status;

	nfs_add_stats(data->inode, NFSIOS_SERVERREADBYTES, data->res.count);

	if (task->tk_status == -ESTALE) {
		set_bit(NFS_INO_STALE, &NFS_I(data->inode)->flags);
		nfs_mark_for_revalidate(data->inode);
	}
	return 0;
}

static void nfs_readpage_retry(struct rpc_task *task, struct nfs_read_data *data)
{
	struct nfs_readargs *argp = &data->args;
	struct nfs_readres *resp = &data->res;
	struct nfs_client *clp = NFS_SERVER(data->inode)->nfs_client;

#ifdef CONFIG_NFS_V4_1
	if (data->fldata.ds_nfs_client) {
		dprintk("%s DS read\n", __func__);
		clp = data->fldata.ds_nfs_client;
	}
#endif /* CONFIG_NFS_V4_1 */
	if (resp->eof || resp->count == argp->count)
		return;

	/* This is a short read! */
	nfs_inc_stats(data->inode, NFSIOS_SHORTREAD);
	/* Has the server at least made some progress? */
	if (resp->count == 0)
		return;

	/* Yes, so retry the read at the end of the data */
	argp->offset += resp->count;
	argp->pgbase += resp->count;
	argp->count -= resp->count;
#ifdef CONFIG_NFS_V4_1
	data->pdata.pnfs_error = -EAGAIN;
#endif /* CONFIG_NFS_V4_1 */
	nfs_restart_rpc(task, clp);
}

/*
 * Handle a read reply that fills part of a page.
 */
static void nfs_readpage_result_partial(struct rpc_task *task, void *calldata)
{
	struct nfs_read_data *data = calldata;
 
	if (nfs_readpage_result(task, data) != 0)
		return;
	if (task->tk_status < 0)
		return;

	nfs_readpage_truncate_uninitialised_page(data);
	nfs_readpage_retry(task, data);
}

static void nfs_readpage_release_partial(void *calldata)
{
	struct nfs_read_data *data = calldata;
	struct nfs_page *req = data->req;
	struct page *page = req->wb_page;
	int status = data->task.tk_status;

	if (status < 0)
		SetPageError(page);

	if (atomic_dec_and_test(&req->wb_complete)) {
		if (!PageError(page))
			SetPageUptodate(page);
		nfs_readpage_release(req);
	}
	nfs_readdata_release(calldata);
}

#if defined(CONFIG_NFS_V4_1)
void nfs_read_prepare(struct rpc_task *task, void *calldata)
{
	struct nfs_read_data *data = calldata;
	struct nfs4_session *ds_session = NULL;

	if (data->fldata.ds_nfs_client) {
		dprintk("%s DS read\n", __func__);
		ds_session = data->fldata.ds_nfs_client->cl_session;
	}
	if (nfs4_setup_sequence(NFS_SERVER(data->inode), ds_session,
				&data->args.seq_args, &data->res.seq_res,
				0, task))
		return;
	rpc_call_start(task);
}
EXPORT_SYMBOL(nfs_read_prepare);
#endif /* CONFIG_NFS_V4_1 */

static const struct rpc_call_ops nfs_read_partial_ops = {
#if defined(CONFIG_NFS_V4_1)
	.rpc_call_prepare = nfs_read_prepare,
#endif /* CONFIG_NFS_V4_1 */
	.rpc_call_done = nfs_readpage_result_partial,
	.rpc_release = nfs_readpage_release_partial,
};

static void nfs_readpage_set_pages_uptodate(struct nfs_read_data *data)
{
	unsigned int count = data->res.count;
	unsigned int base = data->args.pgbase;
	struct page **pages;

	if (data->res.eof)
		count = data->args.count;
	if (unlikely(count == 0))
		return;
	pages = &data->args.pages[base >> PAGE_CACHE_SHIFT];
	base &= ~PAGE_CACHE_MASK;
	count += base;
	for (;count >= PAGE_CACHE_SIZE; count -= PAGE_CACHE_SIZE, pages++)
		SetPageUptodate(*pages);
	if (count == 0)
		return;
	/* Was this a short read? */
	if (data->res.eof || data->res.count == data->args.count)
		SetPageUptodate(*pages);
}

/*
 * This is the callback from RPC telling us whether a reply was
 * received or some error occurred (timeout or socket shutdown).
 */
static void nfs_readpage_result_full(struct rpc_task *task, void *calldata)
{
	struct nfs_read_data *data = calldata;

	if (nfs_readpage_result(task, data) != 0)
		return;
	if (task->tk_status < 0)
		return;
	/*
	 * Note: nfs_readpage_retry may change the values of
	 * data->args. In the multi-page case, we therefore need
	 * to ensure that we call nfs_readpage_set_pages_uptodate()
	 * first.
	 */
	nfs_readpage_truncate_uninitialised_page(data);
	nfs_readpage_set_pages_uptodate(data);
	nfs_readpage_retry(task, data);
}

static void nfs_readpage_release_full(void *calldata)
{
	struct nfs_read_data *data = calldata;

	while (!list_empty(&data->pages)) {
		struct nfs_page *req = nfs_list_entry(data->pages.next);

		nfs_list_remove_request(req);
		nfs_readpage_release(req);
	}
	nfs_readdata_release(calldata);
}

static const struct rpc_call_ops nfs_read_full_ops = {
#if defined(CONFIG_NFS_V4_1)
	.rpc_call_prepare = nfs_read_prepare,
#endif /* CONFIG_NFS_V4_1 */
	.rpc_call_done = nfs_readpage_result_full,
	.rpc_release = nfs_readpage_release_full,
};

/*
 * Read a page over NFS.
 * We read the page synchronously in the following case:
 *  -	The error flag is set for this page. This happens only when a
 *	previous async read operation failed.
 */
int nfs_readpage(struct file *file, struct page *page)
{
	struct nfs_open_context *ctx;
	struct inode *inode = page->mapping->host;
	int		error;

	dprintk("NFS: nfs_readpage (%p %ld@%lu)\n",
		page, PAGE_CACHE_SIZE, page->index);
	nfs_inc_stats(inode, NFSIOS_VFSREADPAGE);
	nfs_add_stats(inode, NFSIOS_READPAGES, 1);

	/*
	 * Try to flush any pending writes to the file..
	 *
	 * NOTE! Because we own the page lock, there cannot
	 * be any new pending writes generated at this point
	 * for this page (other pages can be written to).
	 */
	error = nfs_wb_page(inode, page);
	if (error)
		goto out_unlock;
	if (PageUptodate(page))
		goto out_unlock;

	error = -ESTALE;
	if (NFS_STALE(inode))
		goto out_unlock;

	if (file == NULL) {
		error = -EBADF;
		ctx = nfs_find_open_context(inode, NULL, FMODE_READ);
		if (ctx == NULL)
			goto out_unlock;
	} else
		ctx = get_nfs_open_context(nfs_file_open_context(file));

	if (!IS_SYNC(inode)) {
		error = nfs_readpage_from_fscache(ctx, inode, page);
		if (error == 0)
			goto out;
	}

	error = nfs_readpage_async(ctx, inode, page);

out:
	put_nfs_open_context(ctx);
	return error;
out_unlock:
	unlock_page(page);
	return error;
}

struct nfs_readdesc {
	struct nfs_pageio_descriptor *pgio;
	struct nfs_open_context *ctx;
};

static int
readpage_async_filler(void *data, struct page *page)
{
	struct nfs_readdesc *desc = (struct nfs_readdesc *)data;
	struct inode *inode = page->mapping->host;
	struct nfs_page *new;
	unsigned int len;
	int error;

	len = nfs_page_length(page);
	if (len == 0)
		return nfs_return_empty_page(page);

	if (desc->pgio->pg_lseg) {
		loff_t pgoff = (loff_t)page->index << PAGE_CACHE_SHIFT;
		struct pnfs_layout_range *range = &desc->pgio->pg_lseg->range;

		/* retry later with the right lseg? */
		if (range->offset > pgoff + len ||
		    range->offset + range->length < pgoff) {
			new = ERR_PTR(-EAGAIN);
			goto out_error;
		}
	}

	new = nfs_create_request(desc->ctx, inode, page, 0, len,
				 desc->pgio->pg_lseg);
	if (IS_ERR(new))
		goto out_error;

	if (len < PAGE_CACHE_SIZE)
		zero_user_segment(page, len, PAGE_CACHE_SIZE);
	if (!nfs_pageio_add_request(desc->pgio, new)) {
		error = desc->pgio->pg_error;
		goto out_unlock;
	}
	return 0;
out_error:
	error = PTR_ERR(new);
	SetPageError(page);
out_unlock:
	unlock_page(page);
	return error;
}

int nfs_readpages(struct file *filp, struct address_space *mapping,
		struct list_head *pages, unsigned nr_pages)
{
	struct nfs_pageio_descriptor pgio;
	struct nfs_readdesc desc = {
		.pgio = &pgio,
	};
	struct inode *inode = mapping->host;
	struct nfs_server *server = NFS_SERVER(inode);
	size_t rsize = server->rsize;
	unsigned long npages;
	int ret = -ESTALE;

	dprintk("NFS: nfs_readpages (%s/%Ld %d)\n",
			inode->i_sb->s_id,
			(long long)NFS_FILEID(inode),
			nr_pages);
	nfs_inc_stats(inode, NFSIOS_VFSREADPAGES);

	if (NFS_STALE(inode))
		goto out;

	if (filp == NULL) {
		desc.ctx = nfs_find_open_context(inode, NULL, FMODE_READ);
		if (desc.ctx == NULL)
			return -EBADF;
	} else
		desc.ctx = get_nfs_open_context(nfs_file_open_context(filp));

	/* attempt to read as many of the pages as possible from the cache
	 * - this returns -ENOBUFS immediately if the cookie is negative
	 */
	ret = nfs_readpages_from_fscache(desc.ctx, inode, mapping,
					 pages, &nr_pages);
	if (ret == 0)
		goto read_complete; /* all pages were read */

	pnfs_pageio_init_read(&pgio, inode, desc.ctx, pages, &rsize);
	if (rsize < PAGE_CACHE_SIZE)
		nfs_pageio_init(&pgio, inode, nfs_pagein_multi, rsize, 0);
	else
		nfs_pageio_init(&pgio, inode, nfs_pagein_one, rsize, 0);

	ret = read_cache_pages(mapping, pages, readpage_async_filler, &desc);

	nfs_pageio_complete(&pgio);
	put_lseg(pgio.pg_lseg);
	npages = (pgio.pg_bytes_written + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	nfs_add_stats(inode, NFSIOS_READPAGES, npages);
read_complete:
	put_nfs_open_context(desc.ctx);
out:
	return ret;
}

int __init nfs_init_readpagecache(void)
{
	nfs_rdata_cachep = kmem_cache_create("nfs_read_data",
					     sizeof(struct nfs_read_data),
					     0, SLAB_HWCACHE_ALIGN,
					     NULL);
	if (nfs_rdata_cachep == NULL)
		return -ENOMEM;

	nfs_rdata_mempool = mempool_create_slab_pool(MIN_POOL_READ,
						     nfs_rdata_cachep);
	if (nfs_rdata_mempool == NULL)
		return -ENOMEM;

	return 0;
}

void nfs_destroy_readpagecache(void)
{
	mempool_destroy(nfs_rdata_mempool);
	kmem_cache_destroy(nfs_rdata_cachep);
}
