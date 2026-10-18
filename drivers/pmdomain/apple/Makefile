# SPDX-License-Identifier: GPL-2.0-only
ifeq ($(CONFIG_SILICONFOREST_M3),y)
obj-$(CONFIG_APPLE_PMGR_PWRSTATE) += m3/pmgr-pwrstate.o
obj-$(CONFIG_APPLE_PMP_T6030_PTD) += m3/pmp-ptd.o
else
obj-$(CONFIG_APPLE_PMGR_PWRSTATE) += pmgr-pwrstate.o
endif
obj-$(CONFIG_APPLE_PMGR_PWRSTATE)	+= gfx-tvm.o
obj-$(CONFIG_APPLE_PMP_REPORT)	+= pmp-report.o
obj-$(CONFIG_APPLE_PMP_REPORT_KUNIT_TEST) += pmp-report-test.o

obj-$(CONFIG_APPLE_PMC) += pmc.o
