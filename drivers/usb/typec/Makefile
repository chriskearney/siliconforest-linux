# SPDX-License-Identifier: GPL-2.0
obj-$(CONFIG_TYPEC)		+= typec.o
ifeq ($(CONFIG_SILICONFOREST_M3),y)
typec-y := class.o m3/mux.o bus.o pd.o retimer.o mode_selection.o
else
typec-y := class.o mux.o bus.o pd.o retimer.o mode_selection.o
endif
typec-$(CONFIG_ACPI)		+= port-mapper.o
obj-$(CONFIG_TYPEC)		+= altmodes/
obj-$(CONFIG_TYPEC_TCPM)	+= tcpm/
obj-$(CONFIG_TYPEC_UCSI)	+= ucsi/
ifeq ($(CONFIG_SILICONFOREST_M3),y)
obj-$(CONFIG_TYPEC_TPS6598X_CORE) += tipd-m3/
else
obj-$(CONFIG_TYPEC_TPS6598X_CORE) += tipd/
endif
obj-$(CONFIG_TYPEC_ANX7411)	+= anx7411.o
obj-$(CONFIG_TYPEC_HD3SS3220)	+= hd3ss3220.o
obj-$(CONFIG_TYPEC_STUSB160X) 	+= stusb160x.o
obj-$(CONFIG_TYPEC_RT1719)	+= rt1719.o
obj-$(CONFIG_TYPEC_WUSB3801)	+= wusb3801.o
obj-$(CONFIG_TYPEC)		+= mux/
obj-$(CONFIG_TYPEC_MUX_KUNIT_TEST)	+= typec-mux-test.o
typec-mux-test-y		:= mux-test.o typec-mux-test.dtbo.o
