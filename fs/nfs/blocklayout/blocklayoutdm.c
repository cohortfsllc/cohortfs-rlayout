/*
 *  linux/fs/nfs/blocklayout/blocklayoutdm.c
 *
 *  Module for the NFSv4.1 pNFS block layout driver.
 *
 *  Copyright (c) 2007 The Regents of the University of Michigan.
 *  All rights reserved.
 *
 *  Fred Isaman <iisaman@umich.edu>
 *  Andy Adamson <andros@citi.umich.edu>
 *
 * permission is granted to use, copy, create derivative works and
 * redistribute this software and such derivative works for any purpose,
 * so long as the name of the university of michigan is not used in
 * any advertising or publicity pertaining to the use or distribution
 * of this software without specific, written prior authorization.  if
 * the above copyright notice or any other identification of the
 * university of michigan is included in any copy of any portion of
 * this software, then the disclaimer below must also be included.
 *
 * this software is provided as is, without representation from the
 * university of michigan as to its fitness for any purpose, and without
 * warranty by the university of michigan of any kind, either express
 * or implied, including without limitation the implied warranties of
 * merchantability and fitness for a particular purpose.  the regents
 * of the university of michigan shall not be liable for any damages,
 * including special, indirect, incidental, or consequential damages,
 * with respect to any claim arising out or in connection with the use
 * of the software, even if it has been or is hereafter advised of the
 * possibility of such damages.
 */

#include <linux/genhd.h> /* gendisk - used in a dprintk*/
#include <linux/sched.h>
#include <linux/hash.h>

#include "blocklayout.h"

#define NFSDBG_FACILITY         NFSDBG_PNFS_LD

/* Defines used for calculating memory usage in nfs4_blk_flatten() */
#define ARGSIZE   24    /* Max bytes needed for linear target arg string */
#define SPECSIZE (sizeof8(struct dm_target_spec) + ARGSIZE)
#define SPECS_PER_PAGE (PAGE_SIZE / SPECSIZE)
#define SPEC_HEADER_ADJUST (SPECS_PER_PAGE - \
			    (PAGE_SIZE - sizeof8(struct dm_ioctl)) / SPECSIZE)
#define roundup8(x) (((x)+7) & ~7)
#define sizeof8(x) roundup8(sizeof(x))

static int dev_remove(dev_t dev)
{
	int ret = 1;
	struct pipefs_hdr *msg = NULL, *reply = NULL;
	uint64_t bl_dev;
	uint32_t major = MAJOR(dev), minor = MINOR(dev);

	dprintk("Entering %s\n", __func__);

	if (IS_ERR(bl_device_pipe))
		return ret;

	memcpy((void *)&bl_dev, &major, sizeof(uint32_t));
	memcpy((void *)&bl_dev + sizeof(uint32_t), &minor, sizeof(uint32_t));
	msg = pipefs_alloc_init_msg(0, BL_DEVICE_UMOUNT, 0, (void *)&bl_dev,
				    sizeof(uint64_t));
	if (IS_ERR(msg)) {
		dprintk("ERROR: couldn't make pipefs message.\n");
		goto out;
	}
	msg->msgid = hash_ptr(&msg, sizeof(msg->msgid) * 8);
	msg->status = BL_DEVICE_REQUEST_INIT;

	reply = pipefs_queue_upcall_waitreply(bl_device_pipe, msg,
					      &bl_device_list, 0, 0);
	if (IS_ERR(reply)) {
		dprintk("ERROR: upcall_waitreply failed\n");
		goto out;
	}

	if (reply->status == BL_DEVICE_REQUEST_PROC)
		ret = 0; /*TODO: what to return*/
out:
	if (!IS_ERR(reply))
		kfree(reply);
	if (!IS_ERR(msg))
		kfree(msg);
	return ret;
}

/*
 * Release meta device
 */
static int nfs4_blk_metadev_release(struct pnfs_block_dev *bdev)
{
	int rv;

	dprintk("%s Releasing\n", __func__);
	/* XXX Check return? */
	rv = nfs4_blkdev_put(bdev->bm_mdev);
	dprintk("%s nfs4_blkdev_put returns %d\n", __func__, rv);

	rv = dev_remove(bdev->bm_mdev->bd_dev);
	dprintk("%s Returns %d\n", __func__, rv);
	return rv;
}

void free_block_dev(struct pnfs_block_dev *bdev)
{
	if (bdev) {
		if (bdev->bm_mdev) {
			dprintk("%s Removing DM device: %d:%d\n",
				__func__,
				MAJOR(bdev->bm_mdev->bd_dev),
				MINOR(bdev->bm_mdev->bd_dev));
			/* XXX Check status ?? */
			nfs4_blk_metadev_release(bdev);
		}
		kfree(bdev);
	}
}
