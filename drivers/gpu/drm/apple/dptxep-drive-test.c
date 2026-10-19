// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The 14.7 external role's drive levels, included at the end of dptxep.c so
 * that the production handler runs against a PHY whose configure answers
 * as a test sets it.
 */
#include <kunit/device.h>
#include <kunit/test.h>

struct dptx_drive_fixture {
	struct apple_dcp *dcp;
	struct apple_dcp_afkep ep;
	struct apple_epic_service service;
	struct dptx_port port;
	u8 request[DPTX_DRIVE_LEVELS + 8 * 4], reply[DPTX_DRIVE_LEVELS + 8 * 4];
};

static int dptx_drive_phy_result;
static unsigned int dptx_drive_phy_calls, dptx_drive_phy_lanes;

static int dptx_drive_phy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	dptx_drive_phy_calls++;
	dptx_drive_phy_lanes = opts->dp.lanes;
	return dptx_drive_phy_result;
}

static const struct phy_ops dptx_drive_phy_ops = {
	.configure = dptx_drive_phy_configure,
	.owner = THIS_MODULE,
};

/* A running link on @lanes lanes asking for level 1/1 on each, the PHY answering @result. */
static struct dptx_drive_fixture *dptx_drive_setup(struct kunit *test, u32 lanes, int result)
{
	struct dptx_drive_fixture *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);
	struct device *dev;
	u32 i;

	KUNIT_ASSERT_NOT_NULL(test, f);
	dev = kunit_device_register(test, "dptx-drive");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	f->dcp = kunit_kzalloc(test, sizeof(*f->dcp), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->dcp);
	f->dcp->dev = dev;
	f->ep.dcp = f->dcp;
	f->service.ep = &f->ep;
	f->service.cookie = &f->port;
	f->port.atcphy = devm_phy_create(dev, NULL, &dptx_drive_phy_ops);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, f->port.atcphy);
	f->port.link_rate = LINK_RATE_HBR3;
	put_unaligned_le32(lanes, f->request + DPTX_DRIVE_LANES);
	for (i = 0; i < lanes; i++) {
		put_unaligned_le32(1, f->request + DPTX_DRIVE_LEVELS + 8 * i);
		put_unaligned_le32(1, f->request + DPTX_DRIVE_LEVELS + 8 * i + 4);
	}
	dptx_drive_phy_result = result;
	dptx_drive_phy_calls = 0;
	dptx_drive_phy_lanes = 0;
	return f;
}

static int dptx_drive_set(struct dptx_drive_fixture *f)
{
	return dptxport_call_set_drive_settings_v14_7(&f->service, f->request, sizeof(f->request),
						      f->reply, sizeof(f->reply));
}

/* The PHY takes them: programmed, recorded, answered as applied. */
static void dptx_drive_programmed(struct kunit *test)
{
	struct dptx_drive_fixture *f = dptx_drive_setup(test, 2, 0);

	KUNIT_EXPECT_EQ(test, dptx_drive_set(f), 0);
	KUNIT_EXPECT_EQ(test, dptx_drive_phy_calls, 1U);
	KUNIT_EXPECT_EQ(test, dptx_drive_phy_lanes, 2U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->reply), 0U);
	KUNIT_EXPECT_EQ(test, f->port.trained_voltage[1], 1U);
	KUNIT_EXPECT_EQ(test, f->port.trained_pre[1], 1U);
}

/* The PHY refuses (4 lanes on a 2-lane DP setup): recorded, still answered as applied. */
static void dptx_drive_phy_refusal_acked(struct kunit *test)
{
	struct dptx_drive_fixture *f = dptx_drive_setup(test, 4, -EINVAL);

	KUNIT_EXPECT_EQ(test, dptx_drive_set(f), 0);
	KUNIT_EXPECT_EQ(test, dptx_drive_phy_calls, 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->reply), 0U);
	KUNIT_EXPECT_EQ(test, f->port.trained_voltage[3], 1U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->reply + DPTX_DRIVE_LEVELS + 8 * 3), 1U);
}

/* No DP lanes on the PHY yet: the same. */
static void dptx_drive_phy_not_in_dp(struct kunit *test)
{
	struct dptx_drive_fixture *f = dptx_drive_setup(test, 2, -ENOLINK);

	KUNIT_EXPECT_EQ(test, dptx_drive_set(f), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->reply), 0U);
}

/* Levels out of range: a failure, the PHY not touched, nothing recorded. */
static void dptx_drive_bad_levels(struct kunit *test)
{
	struct dptx_drive_fixture *f = dptx_drive_setup(test, 2, 0);

	put_unaligned_le32(3, f->request + DPTX_DRIVE_LEVELS + 8);
	KUNIT_EXPECT_EQ(test, dptx_drive_set(f), 0);
	KUNIT_EXPECT_EQ(test, dptx_drive_phy_calls, 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->reply), 1U);
	KUNIT_EXPECT_EQ(test, f->port.trained_voltage[0], 0U);
}

/* The link down: recorded only. */
static void dptx_drive_link_down(struct kunit *test)
{
	struct dptx_drive_fixture *f = dptx_drive_setup(test, 2, 0);

	f->port.link_rate = 0;
	KUNIT_EXPECT_EQ(test, dptx_drive_set(f), 0);
	KUNIT_EXPECT_EQ(test, dptx_drive_phy_calls, 0U);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(f->reply), 0U);
	KUNIT_EXPECT_EQ(test, f->port.trained_pre[0], 1U);
}

static struct kunit_case dptx_drive_cases[] = {
	KUNIT_CASE(dptx_drive_programmed),
	KUNIT_CASE(dptx_drive_phy_refusal_acked),
	KUNIT_CASE(dptx_drive_phy_not_in_dp),
	KUNIT_CASE(dptx_drive_bad_levels),
	KUNIT_CASE(dptx_drive_link_down),
	{}
};

static struct kunit_suite dptx_drive_suite = {
	.name = "dptx_v14_7_drive_levels",
	.test_cases = dptx_drive_cases,
};

kunit_test_suite(dptx_drive_suite);
