#
# Makefile for the Cohort replication layout driver kernel module
#

ifeq ($(CONFIG_PNFS_COHORT),m)
        EXTRA_CFLAGS += -DCONFIG_PNFS_COHORT
endif

ifeq ($(CONFIG_PNFS_COHORT),y)
        EXTRA_CFLAGS += -DCONFIG_PNFS_COHORT
endif

obj-$(CONFIG_PNFS_COHORT) += cohortlayout.o cohortlayoutdev.o

