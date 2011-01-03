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

extern unsigned int cohort_debug;

#define COHORT_DEBUG_LAYOUTGET    (1U << 0)

extern void cohort_set_layoutdrivers(struct nfs_server *,
                                     const struct nfs_fh *,
                                     struct nfs_fsinfo *);

extern int cohort_replication_layoutget(struct nfs_server *server,
                                        struct inode *inode,
                                        const struct nfs_fh *mntfh);

#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
