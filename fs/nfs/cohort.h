/*
 *  Cohort replication layout driver.
 *
 *  Copyright (c) 2010
 *  The Linux Box Corporation
 *  All Rights Reserved
 *
 *  Dean Hildebrand <matt@linuxbox.com>
 */

#ifndef FS_NFS_PNFS_COHORT_H
#define FS_NFS_PNFS_COHORT_H

#ifdef CONFIG_NFS_V4_1

extern void cohort_set_layoutdrivers(struct nfs_server *,
                                     const struct nfs_fh *,
                                     struct nfs_fsinfo *);

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
