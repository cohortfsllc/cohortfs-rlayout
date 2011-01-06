/*
 *  Device operations for the Cohort replication layout driver.
 *
 *  Copyright (c) 2010
 *  The Linux Box Corporation
 *  All Rights Reserved
 *
 *  Matt Benjamin <matt@linuxbox.com>
 *
 *  As is evident below, this file is minimally changed from its original in
 *  nfs4filelayoutdev.c.  There may be no reason that a single deviceid cache
 *  can't be used after refactoring.
 *
 *  Copyright (c) 2002
 *  The Regents of the University of Michigan
 *  All Rights Reserved
 *
 *  Dean Hildebrand <dhildebz@umich.edu>
 *  Garth Goodson   <Garth.Goodson@netapp.com>
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
#include <linux/vmalloc.h>

#include "../internal.h"
#include "../pnfs.h"
#include "cohortlayout.h"
#include "../nfs4filelayout.h"


#define NFSDBG_FACILITY		NFSDBG_PNFS_LD

/*
 * Data server cache
 *
 * Like data servers, Cohort rmdses can be mapped to different device ids.
 * reference is also counting as for nfs4_pnfs_ds
 *   - set to 1 on allocation
 *   - incremented when a device id maps a data server already in the cache.
 *   - decremented when deviceid is removed from the cache.
 */
DEFINE_SPINLOCK(cohort_rmds_cache_lock);
static LIST_HEAD(cohort_rmds_cache);

/* Debug routines */
void
print_rmds(struct cohort_replication_layout_rmds *rmds)
{
	if (rmds == NULL) {
		printk("%s NULL device\n", __func__);
		return;
	}
	printk("        ip_addr %x port %hu\n"
		"        ref count %d\n"
		"        client %p\n"
		"        cl_exchange_flags %x\n",
		ntohl(rmds->ds_ip_addr), ntohs(rmds->ds_port),
		atomic_read(&rmds->ds_count), rmds->ds_client,
		rmds->ds_client ? rmds->ds_client->cl_exchange_flags : 0);
}

void
print_rmds_list(struct cohort_replication_layout_rmds_addr *dsaddr)
{
	int i;

	ifdebug(FACILITY) {
		printk("%s dsaddr->ds_num %d\n", __func__,
		       dsaddr->ds_num);
		for (i = 0; i < dsaddr->ds_num; i++)
			print_rmds(dsaddr->ds_list[i]);
	}
}

void cohort_rpl_print_deviceid(struct nfs4_deviceid *id)
{
	u32 *p = (u32 *)id;

	dprintk("%s: device id= [%x%x%x%x]\n", __func__,
		p[0], p[1], p[2], p[3]);
}

/* rmds_cache_lock is held */
static struct cohort_replication_layout_rmds *
_cohort_rmds_lookup_locked(u32 ip_addr, u32 port)
{
	struct cohort_replication_layout_rmds *ds;

	dprintk("_rmds_lookup: ip_addr=%x port=%hu\n",
			ntohl(ip_addr), ntohs(port));

	list_for_each_entry(ds, &cohort_rmds_cache, ds_node) {
		if (ds->ds_ip_addr == ip_addr &&
		    ds->ds_port == port) {
			return ds;
		}
	}
	return NULL;
}

/* Create an rpc to the data server defined in 'dev_list' */
static int
cohort_rpl_rmds_create(struct nfs_server *mds_srv,
                       struct cohort_replication_layout_rmds *ds)
{
	struct nfs_server	*tmp;
	struct sockaddr_in	sin;
	struct rpc_clnt		*mds_clnt = mds_srv->client;
	struct nfs_client	*clp = mds_srv->nfs_client;
	struct sockaddr		*mds_addr;
	int err = 0;

	dprintk("--> %s ip:port %x:%hu au_flavor %d\n", __func__,
		ntohl(ds->ds_ip_addr), ntohs(ds->ds_port),
		mds_clnt->cl_auth->au_flavor);

	sin.sin_family = AF_INET;
	sin.sin_addr.s_addr = ds->ds_ip_addr;
	sin.sin_port = ds->ds_port;

	/*
	 * If this DS is also the MDS, use the MDS session only if the
	 * MDS exchangeid flags show the EXCHGID4_FLAG_USE_PNFS_DS pNFS role.
	 */
	mds_addr = (struct sockaddr *)&clp->cl_addr;
	if (nfs_sockaddr_cmp((struct sockaddr *)&sin, mds_addr)) {
		if (!(clp->cl_exchange_flags & EXCHGID4_FLAG_USE_PNFS_DS)) {
			printk(KERN_INFO
			       "ip:port %x:%hu is not a pNFS Data Server\n",
			       ntohl(ds->ds_ip_addr), ntohs(ds->ds_port));
			err = -ENODEV;
		} else {
			atomic_inc(&clp->cl_count);
			ds->ds_client = clp;
			dprintk("%s Using MDS Session for DS\n", __func__);
		}
		goto out;
	}

	/* Temporay server for nfs4_set_client */
	tmp = kzalloc(sizeof(struct nfs_server), GFP_KERNEL);
	if (!tmp)
		goto out;

	/*
	 * Set a retrans, timeout interval, and authflavor equual to the MDS
	 * values. Use the MDS nfs_client cl_ipaddr field so as to use the
	 * same co_ownerid as the MDS.
	 */
	err = nfs4_set_client(tmp,
			      mds_srv->nfs_client->cl_hostname,
			      (struct sockaddr *)&sin,
			      sizeof(struct sockaddr),
			      mds_srv->nfs_client->cl_ipaddr,
			      mds_clnt->cl_auth->au_flavor,
			      IPPROTO_TCP,
			      mds_clnt->cl_xprt->timeout,
			      1 /* minorversion */);
	if (err < 0)
		goto out_free;

	clp = tmp->nfs_client;

	/* Ask for only the EXCHGID4_FLAG_USE_PNFS_DS pNFS role */
	dprintk("%s EXCHANGE_ID for clp %p\n", __func__, clp);
	clp->cl_exchange_flags = EXCHGID4_FLAG_USE_PNFS_DS;

	err = nfs4_recover_expired_lease(clp);
	if (!err)
		err = nfs4_check_client_ready(clp);
	if (err)
		goto out_put;

        /* XXX In future, we may cease to check for _USE_PNFS_DS.
         * We have mostly ruled out adding our own enumeration value(s),
         * since exhange_flags wasn't designed for 3rd party extensions.
         */
	if (!(clp->cl_exchange_flags & EXCHGID4_FLAG_USE_PNFS_DS)) {
		printk(KERN_INFO "ip:port %x:%hu is not a pNFS Data Server\n",
		       ntohl(ds->ds_ip_addr), ntohs(ds->ds_port));
		err = -ENODEV;
		goto out_put;
	}
	/*
	 * Set DS lease equal to the MDS lease, renewal is scheduled in
	 * create_session
	 */
	spin_lock(&mds_srv->nfs_client->cl_lock);
	clp->cl_lease_time = mds_srv->nfs_client->cl_lease_time;
	spin_unlock(&mds_srv->nfs_client->cl_lock);
	clp->cl_last_renewal = jiffies;

	clear_bit(NFS4CLNT_SESSION_RESET, &clp->cl_state);
	ds->ds_client = clp;

	dprintk("%s: ip=%x, port=%hu, rpcclient %p\n", __func__,
				ntohl(ds->ds_ip_addr), ntohs(ds->ds_port),
				clp->cl_rpcclient);
out_free:
	kfree(tmp);
out:
	dprintk("%s Returns %d\n", __func__, err);
	return err;
out_put:
	nfs_put_client(clp);
	goto out_free;
}

static void
destroy_ds(struct cohort_replication_layout_rmds *ds)
{
	dprintk("--> %s\n", __func__);
	ifdebug(FACILITY)
		print_rmds(ds);

	if (ds->ds_client)
		nfs_put_client(ds->ds_client);
	kfree(ds);
}

static void
cohort_rpl_free_deviceid(struct cohort_replication_layout_rmds_addr *dsaddr)
{
	struct cohort_replication_layout_rmds *ds;
	int i;

	cohort_rpl_print_deviceid(&dsaddr->deviceid.de_id);

	for (i = 0; i < dsaddr->ds_num; i++) {
		ds = dsaddr->ds_list[i];
		if (ds != NULL) {
			if (atomic_dec_and_lock(&ds->ds_count,
						&cohort_rmds_cache_lock)) {
				list_del_init(&ds->ds_node);
				spin_unlock(&cohort_rmds_cache_lock);
				destroy_ds(ds);
			}
		}
	}
	kfree(dsaddr);
}

void
cohort_rpl_free_deviceid_callback(struct pnfs_deviceid_node *device)
{
	struct cohort_replication_layout_rmds_addr *dsaddr =
		container_of(device,
                             struct cohort_replication_layout_rmds_addr,
                             deviceid);

	cohort_rpl_free_deviceid(dsaddr);
}

static struct cohort_replication_layout_rmds *
cohort_replication_layout_rmds_add(struct inode *inode, u32 ip_addr, u32 port)
{
	struct cohort_replication_layout_rmds *tmp_ds, *ds;

	ds = kzalloc(sizeof(*tmp_ds), GFP_KERNEL);
	if (!ds)
		goto out;

	spin_lock(&cohort_rmds_cache_lock);
	tmp_ds = _cohort_rmds_lookup_locked(ip_addr, port);
	if (tmp_ds == NULL) {
		ds->ds_ip_addr = ip_addr;
		ds->ds_port = port;
		atomic_set(&ds->ds_count, 1);
		INIT_LIST_HEAD(&ds->ds_node);
		ds->ds_client = NULL;
		list_add(&ds->ds_node, &cohort_rmds_cache);
		dprintk("%s add new rmds ip 0x%x\n", __func__,
			ds->ds_ip_addr);
	} else {
		kfree(ds);
		atomic_inc(&tmp_ds->ds_count);
		dprintk("%s rmds found ip 0x%x, inc'ed ds_count to %d\n",
			__func__, tmp_ds->ds_ip_addr,
			atomic_read(&tmp_ds->ds_count));
		ds = tmp_ds;
	}
	spin_unlock(&cohort_rmds_cache_lock);
out:
	return ds;
}

/*
 * Currently only support ipv4.  Original comment notwithstanding,
 * this routine has no idea of the length of any multipath list it
 * is (partially) decoding.  Shareable.
 */
static struct cohort_replication_layout_rmds *
cohort_rpl_decode_and_add_ds(__be32 **pp, struct inode *inode)
{
	struct cohort_replication_layout_rmds *ds = NULL;
	char *buf;
	const char *ipend, *pstr;
	u32 ip_addr, port;
	int nlen, rlen, i;
	int tmp[2];
	__be32 *r_netid, *r_addr, *p = *pp;

        dprintk("%s -->\n", __func__);

	/* r_netid */
	nlen = be32_to_cpup(p++);
	r_netid = p;
	p += XDR_QUADLEN(nlen);

	/* r_addr */
	rlen = be32_to_cpup(p++);
	r_addr = p;
	p += XDR_QUADLEN(rlen);
	*pp = p;

	/* Check that netid is "tcp" */
	if (nlen != 3 ||  memcmp((char *)r_netid, "tcp", 3)) {
		dprintk("%s: ERROR: non ipv4 TCP r_netid\n", __func__);
		goto out_err;
	}

	/* ipv6 length plus port is legal */
	if (rlen > INET6_ADDRSTRLEN + 8) {
		dprintk("%s Invalid address, length %d\n", __func__,
			rlen);
		goto out_err;
	}

	buf = kmalloc(rlen + 1, GFP_KERNEL);
	buf[rlen] = '\0';
	memcpy(buf, r_addr, rlen);

	/* replace the port dots with dashes for the in4_pton() delimiter*/
	for (i = 0; i < 2; i++) {
		char *res = strrchr(buf, '.');
		*res = '-';
	}

	/* Currently only support ipv4 address */
	if (in4_pton(buf, rlen, (u8 *)&ip_addr, '-', &ipend) == 0) {
		dprintk("%s: Only ipv4 addresses supported\n", __func__);
		goto out_free;
	}

	/* port */
	pstr = ipend;
	sscanf(pstr, "-%d-%d", &tmp[0], &tmp[1]);
	port = htons((tmp[0] << 8) | (tmp[1]));

	ds = cohort_replication_layout_rmds_add(inode, ip_addr, port);
	dprintk("%s Decoded address and port %s\n", __func__, buf);
out_free:
	kfree(buf);
out_err:
	return ds;
}

/*
 * Decode opaque device data and return the result.  We must support
 * multipath list count > 1, since each is a replica server. 
 */
static struct cohort_replication_layout_rmds_addr*
cohort_rpl_decode_device(struct inode *ino, struct pnfs_device *pdev)
{
	struct cohort_replication_layout_rmds_addr *dsaddr;
	u32 i, num;
	__be32 *p;

        p = (__be32 *) pdev->area;

	num = be32_to_cpup(p++);
	dprintk("%s decoding %u replicas\n", __func__, num);
	dsaddr = kzalloc(sizeof(*dsaddr) +
			(sizeof(struct cohort_replication_layout_rmds_addr *)
                         * (num - 1)),
			GFP_KERNEL);
	if (!dsaddr)
		goto out_err;

	dsaddr->ds_num = num;
	memcpy(&dsaddr->deviceid.de_id, &pdev->dev_id, sizeof(pdev->dev_id));

	for (i = 0; i < dsaddr->ds_num; i++) {
            /* decode ds and advance p */
            dsaddr->ds_list[i] = cohort_rpl_decode_and_add_ds(&p, ino);
            if (dsaddr->ds_list[i] == NULL)
                goto out_err_free;
        }
	return dsaddr;

out_err_free:
	cohort_rpl_free_deviceid(dsaddr);
out_err:
	dprintk("%s ERROR: returning NULL\n", __func__);
	return NULL;
}

/*
 * Decode the opaque device specified in 'dev'
 * and add it to the list of available devices.
 * If the deviceid is already cached, pnfs_add_deviceid will return
 * a pointer to the cached struct and throw away the new.
 */
static struct cohort_replication_layout_rmds_addr*
cohort_rpl_decode_and_add_device(struct inode *inode, struct pnfs_device *dev)
{
	struct cohort_replication_layout_rmds_addr *dsaddr;
	struct pnfs_deviceid_node *d;

	dsaddr = cohort_rpl_decode_device(inode, dev);
	if (!dsaddr) {
		printk(KERN_WARNING "%s: Could not decode or add device\n",
			__func__);
		return NULL;
	}

	d = pnfs_add_deviceid(NFS_SERVER(inode)->nfs_client->cl_devid_cache,
			      &dsaddr->deviceid);

	return container_of(d, struct cohort_replication_layout_rmds_addr,
                            deviceid);
}

/*
 * Retrieve the information for dev_id, add it to the list
 * of available devices, and return it.
 */
struct cohort_replication_layout_rmds_addr *
cohort_rpl_get_device_info(struct inode *inode, struct nfs4_deviceid *dev_id)
{
	struct pnfs_device *pdev = NULL;
	u32 max_resp_sz;
	int max_pages;
	struct page **pages = NULL;
	struct cohort_replication_layout_rmds_addr *dsaddr = NULL;
	int rc, i;
	struct nfs_server *server = NFS_SERVER(inode);

	/*
	 * Use the session max response size as the basis for setting
	 * GETDEVICEINFO's maxcount
	 */
	max_resp_sz = server->nfs_client->cl_session->fc_attrs.max_resp_sz;
	max_pages = max_resp_sz >> PAGE_SHIFT;
	dprintk("%s inode %p max_resp_sz %u max_pages %d\n",
		__func__, inode, max_resp_sz, max_pages);

	pdev = kzalloc(sizeof(struct pnfs_device), GFP_KERNEL);
	if (pdev == NULL)
		return NULL;

	pages = kzalloc(max_pages * sizeof(struct page *), GFP_KERNEL);
	if (pages == NULL) {
		kfree(pdev);
		return NULL;
	}
	for (i = 0; i < max_pages; i++) {
		pages[i] = alloc_page(GFP_KERNEL);
		if (!pages[i])
			goto out_free;
	}

	/* set pdev->area */
	pdev->area = vmap(pages, max_pages, VM_MAP, PAGE_KERNEL);
	if (!pdev->area)
		goto out_free;

	memcpy(&pdev->dev_id, dev_id, sizeof(*dev_id));
	pdev->layout_type = LAYOUT4_COHORT_REPLICATION;
	pdev->pages = pages;
	pdev->pgbase = 0;
	pdev->pglen = PAGE_SIZE * max_pages;
	pdev->mincount = 0;

	rc = nfs4_proc_getdeviceinfo(server, pdev);
	dprintk("%s getdevice info returns %d\n", __func__, rc);
	if (rc)
		goto out_free;

	/*
	 * Found new device, need to decode it and then add it to the
	 * list of known devices for this mountpoint.
	 */
	dsaddr = cohort_rpl_decode_and_add_device(inode, pdev);
out_free:
	if (pdev->area != NULL)
		vunmap(pdev->area);
	for (i = 0; i < max_pages; i++)
		__free_page(pages[i]);
	kfree(pages);
	kfree(pdev);
	dprintk("<-- %s dsaddr %p\n", __func__, dsaddr);
	return dsaddr;
}

struct cohort_replication_layout_rmds_addr *
cohort_rpl_find_get_deviceid(struct nfs_client *clp, struct nfs4_deviceid *id)
{
	struct pnfs_deviceid_node *d;

	d = pnfs_find_get_deviceid(clp->cl_devid_cache, id);
	return (d == NULL) ? NULL :
		container_of(d, struct cohort_replication_layout_rmds_addr,
                             deviceid);
}

#if 0 /* XXX not obviously useful */
/*
 * Want res = (offset - layout->pattern_offset)/ layout->stripe_unit
 * Then: ((res + fsi) % dsaddr->stripe_count)
 */
static u32
_nfs4_fl_calc_j_index(struct pnfs_layout_segment *lseg, loff_t offset)
{
	struct nfs4_filelayout_segment *flseg = FILELAYOUT_LSEG(lseg);
	u64 tmp;

	tmp = offset - flseg->pattern_offset;
	do_div(tmp, flseg->stripe_unit);
	tmp += flseg->first_stripe_index;
	return do_div(tmp, flseg->dsaddr->stripe_count);
}

u32
nfs4_fl_calc_ds_index(struct pnfs_layout_segment *lseg, loff_t offset)
{
	u32 j;

	j = _nfs4_fl_calc_j_index(lseg, offset);
	return FILELAYOUT_LSEG(lseg)->dsaddr->stripe_indices[j];
}

struct nfs_fh *
nfs4_fl_select_ds_fh(struct pnfs_layout_segment *lseg, loff_t offset)
{
	struct nfs4_filelayout_segment *flseg = FILELAYOUT_LSEG(lseg);
	u32 i;

	if (flseg->stripe_type == STRIPE_SPARSE) {
		if (flseg->num_fh == 1)
			i = 0;
		else if (flseg->num_fh == 0)
			return NULL;
		else
			i = nfs4_fl_calc_ds_index(lseg, offset);
	} else
		i = _nfs4_fl_calc_j_index(lseg, offset);
	return flseg->fh_array[i];
}
#endif

struct cohort_replication_layout_rmds *
cohort_rpl_prepare_ds(struct pnfs_layout_segment *lseg, u32 ds_idx)
{
	struct cohort_replication_layout_rmds_addr *dsaddr;

	dsaddr = COHORT_RPL_LSEG(lseg)->dsaddr;
	if (dsaddr->ds_list[ds_idx] == NULL) {
		printk(KERN_ERR "%s: No rmds for device id!\n",
			__func__);
		return NULL;
	}

	if (!dsaddr->ds_list[ds_idx]->ds_client) {
		int err;

		err = cohort_rpl_rmds_create(NFS_SERVER(lseg->layout->inode),
                                             dsaddr->ds_list[ds_idx]);
		if (err) {
			printk(KERN_ERR "%s nfs4_pnfs_ds_create error %d\n",
			       __func__, err);
			return NULL;
		}
	}
	return dsaddr->ds_list[ds_idx];
}
EXPORT_SYMBOL_GPL(cohort_rpl_prepare_ds);
