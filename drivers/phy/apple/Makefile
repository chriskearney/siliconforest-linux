# SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause

obj-$(CONFIG_PHY_APPLE_ATC)		+= phy-apple-atc.o
ifeq ($(CONFIG_SILICONFOREST_M3),y)
phy-apple-atc-y := m3/atc.o
else
phy-apple-atc-y := atc.o
endif

obj-$(CONFIG_PHY_APPLE_DPTX)	+= phy-apple-dptx.o
phy-apple-dptx-y		+= dptx.o

ifneq ($(CONFIG_SILICONFOREST_M3),y)
obj-$(CONFIG_PHY_APPLE_ATC_KUNIT_TEST)	+= atc-tunnel-test.o
obj-$(CONFIG_PHY_APPLE_ATC_KUNIT_TEST)	+= atc-t8122-dp-test.o

endif
