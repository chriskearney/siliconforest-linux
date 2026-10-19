// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Which display processors go on without Type-C routes when the device tree
 * leaves every route disabled. A processor with an output of its own (the
 * HDMI processor of the 14" and 16" M1 Pro and M1 Max, the J613 panel's)
 * keeps that output; one that drives nothing but its routes (T6030, T8122
 * and H17P external processors) fails to probe. The containers come from
 * typec-routes-test.dtso and go through the production
 * dcp_register_typec_routes().
 */
#include <kunit/device.h>
#include <kunit/of.h>
#include <kunit/test.h>
#include <linux/device.h>
#include <linux/of.h>

#include "dcp-fabric.h"
#include "dcp-internal.h"

#define ROUTES_TEST_ROOT "/dcp-typec-routes-test/"

enum routes_test_dcp {
	ROUTES_DCP_HDMI,	/* T600x dcpext0: fixed HDMI output */
	ROUTES_DCP_PANEL,	/* J613 internal panel processor */
	ROUTES_DCP_T6030_EXT,
	ROUTES_DCP_T8122_EXT,
	ROUTES_DCP_H17P_EXT,
};

static int routes_test_init(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, 0, of_overlay_apply_kunit(test, typec_routes_test));
	return 0;
}

/* A processor of @kind whose device node is the one at @path. */
static struct apple_dcp *routes_test_dcp(struct kunit *test, enum routes_test_dcp kind,
					 const char *path)
{
	struct apple_dcp *dcp;
	struct device_node *np;
	struct device *dev;
	char name[32];

	np = of_find_node_by_path(path);
	KUNIT_ASSERT_NOT_NULL(test, np);
	of_node_put_kunit(test, np);

	snprintf(name, sizeof(name), "dcp-typec-routes-test.%u", kind);
	dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	device_set_node(dev, of_fwnode_handle(np));

	dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, dcp);
	dcp->dev = dev;
	switch (kind) {
	case ROUTES_DCP_HDMI:
		dcp->fw_compat = DCP_FIRMWARE_V_13_5;
		break;
	case ROUTES_DCP_PANEL:
		dcp->fw_compat = DCP_FIRMWARE_V_14_7;
		break;
	case ROUTES_DCP_T6030_EXT:
		dcp->fw_compat = DCP_FIRMWARE_V_14_7;
		dcp->external = true;
		dcp->external_native = true;
		break;
	case ROUTES_DCP_T8122_EXT:
		dcp->fw_compat = DCP_FIRMWARE_V_14_7;
		dcp->ext_failsafe = true;
		break;
	case ROUTES_DCP_H17P_EXT:
		dcp->fw_compat = DCP_FIRMWARE_H17P;
		dcp->hw.iomfb_method_profile = DCP_IOMFB_METHODS_H17P_EXTERNAL;
		break;
	}
	return dcp;
}

static void routes_test_expect_none(struct kunit *test, struct apple_dcp *dcp)
{
	KUNIT_EXPECT_EQ(test, dcp->nr_typec_routes, 0U);
	KUNIT_EXPECT_FALSE(test, dcp->phy_managed_by_typec);
	KUNIT_EXPECT_NULL(test, dcp->phy);
}

/* An older boot loader enabled dcpext0 but not its routes: HDMI only. */
static void routes_left_disabled_hdmi_test(struct kunit *test)
{
	struct apple_dcp *dcp = routes_test_dcp(test, ROUTES_DCP_HDMI,
						ROUTES_TEST_ROOT "routes-left-disabled");

	KUNIT_EXPECT_EQ(test, dcp_register_typec_routes(dcp), 0);
	routes_test_expect_none(test, dcp);
}

/* The J613 panel before, or without, the display gate enabling its route. */
static void routes_left_disabled_panel_test(struct kunit *test)
{
	struct apple_dcp *dcp = routes_test_dcp(test, ROUTES_DCP_PANEL,
						ROUTES_TEST_ROOT "routes-left-disabled");

	KUNIT_EXPECT_EQ(test, dcp_register_typec_routes(dcp), 0);
	routes_test_expect_none(test, dcp);
}

/* An external processor is never admitted without an enabled route. */
static void routes_left_disabled_external_test(struct kunit *test)
{
	static const enum routes_test_dcp kinds[] = {
		ROUTES_DCP_T6030_EXT, ROUTES_DCP_T8122_EXT, ROUTES_DCP_H17P_EXT,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(kinds); i++) {
		struct apple_dcp *dcp = routes_test_dcp(test, kinds[i],
							ROUTES_TEST_ROOT "routes-left-disabled");

		KUNIT_EXPECT_EQ_MSG(test, dcp_register_typec_routes(dcp), -EINVAL,
				    "processor kind %u", kinds[i]);
		routes_test_expect_none(test, dcp);
	}
}

/*
 * An enabled route beside a disabled one is registered, not passed over: here
 * the attempt fails at its PHY, which the test node does not describe.
 */
static void route_enabled_registered_test(struct kunit *test)
{
	struct apple_dcp *dcp = routes_test_dcp(test, ROUTES_DCP_HDMI,
						ROUTES_TEST_ROOT "route-enabled");

	KUNIT_EXPECT_LT(test, dcp_register_typec_routes(dcp), 0);
	routes_test_expect_none(test, dcp);
}

/* Without a container every processor goes on, as before. */
static void no_routes_test(struct kunit *test)
{
	static const enum routes_test_dcp kinds[] = {
		ROUTES_DCP_HDMI, ROUTES_DCP_PANEL, ROUTES_DCP_T6030_EXT,
		ROUTES_DCP_T8122_EXT, ROUTES_DCP_H17P_EXT,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(kinds); i++) {
		struct apple_dcp *dcp = routes_test_dcp(test, kinds[i],
							ROUTES_TEST_ROOT "no-routes");

		KUNIT_EXPECT_EQ_MSG(test, dcp_register_typec_routes(dcp), 0,
				    "processor kind %u", kinds[i]);
		routes_test_expect_none(test, dcp);
	}
}

static struct kunit_case typec_routes_test_cases[] = {
	KUNIT_CASE(routes_left_disabled_hdmi_test),
	KUNIT_CASE(routes_left_disabled_panel_test),
	KUNIT_CASE(routes_left_disabled_external_test),
	KUNIT_CASE(route_enabled_registered_test),
	KUNIT_CASE(no_routes_test),
	{}
};

static struct kunit_suite typec_routes_test_suite = {
	.name = "dcp_typec_routes_probe",
	.init = routes_test_init,
	.test_cases = typec_routes_test_cases,
};

kunit_test_suite(typec_routes_test_suite);
