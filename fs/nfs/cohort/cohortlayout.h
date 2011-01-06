/*
 *  NFSv4 file layout driver data structures.
 *
 *  Copyright (c) 2010
 *  The Linux Box Corporation
 *  All Rights Reserved
 *
 *  Matt Benjamin <matt@linuxbox.com>
 *
 */

#ifndef FS_NFS_COHORTLAYOUT_H
#define FS_NFS_COHORTLAYOUT_H

#include "../pnfs.h"

struct cohort_replication_layout_rmds {
	struct list_head	ds_node;  /* nfs4_pnfs_dev_hlist dev_dslist */
	u32			ds_ip_addr;
	u32			ds_port;
	struct nfs_client	*ds_client;
	atomic_t		ds_count;
};

struct cohort_replication_layout_rmds_addr {
	struct pnfs_deviceid_node		deviceid;
	u32					ds_num;
	struct cohort_replication_layout_rmds	*ds_list[1];
};

struct cohort_replication_layout_segment {
	struct pnfs_layout_segment generic_hdr;
	struct cohort_replication_layout_rmds_addr *dsaddr;
	struct nfs_fh fh; /* unused, temporary */
};

static inline struct cohort_replication_layout_segment *
COHORT_RPL_LSEG(struct pnfs_layout_segment *lseg)
{
	return container_of(lseg,
			    struct cohort_replication_layout_segment,
			    generic_hdr);
}

extern struct cohort_replication_layout_rmds_addr *
cohort_rpl_get_device_info(struct inode *inode, struct nfs4_deviceid *dev_id);

extern struct cohort_replication_layout_rmds_addr *
cohort_rpl_find_get_deviceid(struct nfs_client *clp, struct nfs4_deviceid *id);

extern void cohort_rpl_free_deviceid_callback(
    struct pnfs_deviceid_node *device);

extern void cohort_rpl_print_deviceid(struct nfs4_deviceid *id);

extern struct cohort_replication_layout_rmds *
cohort_rpl_prepare_ds(struct pnfs_layout_segment *lseg, u32 ds_idx);

#endif /* FS_NFS_COHORTLAYOUT_H */
