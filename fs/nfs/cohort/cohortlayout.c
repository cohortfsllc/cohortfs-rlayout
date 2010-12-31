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

/*
 * This module is a stub.  To properly support loadable layout drivers of
 * new functional types, we will need to propose an extension of the current
 * driver sub-module interfaces and driver cache.  This can't really happen
 * until we understand better what the class looks like, so for now, I've
 * added a new file to the pnfs module, and use direct calls to some
 * exported functions there.
 */

static int __init cohort_rpl_init(void)
{
	printk(KERN_INFO "%s: Cohort Replication Layout Driver Init\n",
	       __func__);

        /* We are a layout driver of a new class, extend driver cache
         * to permit registration here. */

        return (0);
}

static void __exit cohort_rpl_exit(void)
{
	printk(KERN_INFO "%s: Cohort Replication Layout Driver Exit\n",
	       __func__);
}

MODULE_AUTHOR("Matt Benjamin <matt@linuxbox.com>");
MODULE_DESCRIPTION("The Cohort NFSv4 replication layout driver");
MODULE_LICENSE("GPL");

module_init(cohort_rpl_init);
module_exit(cohort_rpl_exit);
