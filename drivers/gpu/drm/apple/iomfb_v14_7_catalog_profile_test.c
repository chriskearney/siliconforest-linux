// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Which 14.7 sessions parse a display's catalog with the external role's
 * rules, included at the end of iomfb_v14_7.c so that the production
 * dcp_v14_catalog_profile() runs. The J613 panel pipe's Type-C display is
 * described through the same callbacks, and keeps the common rules.
 */
#include <kunit/test.h>

static void v14_catalog_profile_external(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_dcp_v14 *v14 = kunit_kzalloc(test, sizeof(*v14), GFP_KERNEL);
	struct dcp_parse_ctx ctx = {};

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, v14);
	v14->external = true;
	dcp->dptxport[0].lane_count = 2;
	dcp_v14_catalog_profile(v14, dcp, &ctx);
	KUNIT_EXPECT_TRUE(test, ctx.v14_7_rgb8);
	KUNIT_EXPECT_TRUE(test, ctx.v14_7_dsc);
	KUNIT_EXPECT_EQ(test, ctx.link_lanes, 2U);
	KUNIT_EXPECT_EQ(test, ctx.link_rate, (u32)LINK_RATE_HBR3);
}

static void v14_catalog_profile_typec_panel_pipe(struct kunit *test)
{
	struct apple_dcp *dcp = kunit_kzalloc(test, sizeof(*dcp), GFP_KERNEL);
	struct apple_dcp_v14 *v14 = kunit_kzalloc(test, sizeof(*v14), GFP_KERNEL);
	struct dcp_parse_ctx ctx = {};

	KUNIT_ASSERT_NOT_NULL(test, dcp);
	KUNIT_ASSERT_NOT_NULL(test, v14);
	v14->typec = true;
	v14->board = &dcp_v14_board_j613;
	dcp->dptxport[0].lane_count = 4;
	dcp_v14_catalog_profile(v14, dcp, &ctx);
	KUNIT_EXPECT_FALSE(test, ctx.v14_7_rgb8);
	KUNIT_EXPECT_FALSE(test, ctx.v14_7_dsc);
	KUNIT_EXPECT_EQ(test, ctx.link_lanes, 0U);
	KUNIT_EXPECT_EQ(test, ctx.link_rate, 0U);
}

static struct kunit_case v14_catalog_profile_cases[] = {
	KUNIT_CASE(v14_catalog_profile_external),
	KUNIT_CASE(v14_catalog_profile_typec_panel_pipe),
	{}
};

static struct kunit_suite v14_catalog_profile_suite = {
	.name = "dcp_v14_catalog_profile",
	.test_cases = v14_catalog_profile_cases,
};

kunit_test_suite(v14_catalog_profile_suite);
