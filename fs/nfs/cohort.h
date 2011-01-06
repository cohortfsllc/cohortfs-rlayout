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

static inline int
cohort_replicas_p(struct inode *ino) {
    struct nfs_server *server = NFS_SERVER(ino);
    if (server->pnfs_meta_ld && 
        (server->pnfs_meta_ld->id == LAYOUT4_COHORT_REPLICATION)) {
#if 0 /* XXXX FINISH */
        if (server->pnfs_meta_ld->layout_p()) {
            return 1;
        }
#else
        return 1;
#endif
    }
    return 0;
}

extern void cohort_set_layoutdrivers(struct nfs_server *,
                                     const struct nfs_fh *,
                                     struct nfs_fsinfo *);

extern int cohort_replication_layoutget(struct nfs_server *server,
                                        struct inode *inode,
                                        const struct nfs_fh *mntfh);

extern int cohort_rpl_create(struct inode *dir, struct dentry *dentry,
                             struct nfs4_createdata *data);

extern void cohort_rpl_return_layouts(struct super_block *sb);

extern void dprintk_fh(const char *func, const char *tag, struct nfs_fh *fh);


#endif /* CONFIG_NFS_V4_1 */

#endif /* FS_NFS_PNFS_H */
