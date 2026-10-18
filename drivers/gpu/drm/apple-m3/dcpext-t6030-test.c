// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

/*
 * KUnit for the J514S DCPEXT external role:
 *
 * - the DPTX callback profile, byte for byte (pin C and pin D, link training,
 *   DPMS, inactive sink, refusals);
 * - the HPD/IRQ_HPD bookkeeping and the Type-C duplicate-filter bypass;
 * - the sink catalog through the common parser's fw14 RGB8 profile and the
 *   link budget, on small fixtures derived from two captured fw14
 *   TimingElements publications (a Philips monitor and a Sony TV);
 * - the external present encoding against an independently built first-swap
 *   table;
 * - the AV service's EDID object.
 */

#include <kunit/test.h>
#include <linux/slab.h>
#include <linux/unaligned.h>

#include <drm/drm_modes.h>

#include "dcp.h"
#include "dcpext-t6030.h"
#include "iomfb_v14_7_present.h"
#include "parser.h"

/* ---- DPTX callbacks ---- */

/* The link of a freshly selected route: crossbar up at rate 0, nothing else. */
static struct dcpext_link link_routed(u32 lanes)
{
	return (struct dcpext_link) { .lanes_max = lanes, .xbar_up = true };
}

/* One callback: plan, the hardware step as a PHY would program it, reply. */
static int link_cb(struct kunit *test, struct dcpext_link *link, u32 index,
		   const u8 *data, size_t size, u8 *reply, struct dcpext_link_op *op,
		   bool symmetric_pairs)
{
	unsigned int i;
	int ret;

	ret = dcpext_link_plan(link, index, data, size, op);
	if (ret)
		return ret;
	/* Pin C: the PHY programs each pair member with the stronger request. */
	if (op->hw == DCPEXT_HW_DRIVE && symmetric_pairs && op->lanes == 4) {
		for (i = 0; i < 2; i++) {
			u32 v = max(op->voltage[i], op->voltage[i + 2]);
			u32 p = min(max(op->pre[i], op->pre[i + 2]), DCPEXT_DRIVE_MAX - v);

			op->voltage[i] = op->voltage[i + 2] = v;
			op->pre[i] = op->pre[i + 2] = p;
		}
	}
	if (op->hw == DCPEXT_HW_RATE || op->hw == DCPEXT_HW_DEACTIVATE)
		link->xbar_up = false;
	return dcpext_link_reply(link, index, op, data, size, reply, size);
}

static void fill(u8 *data, size_t size, u8 seed)
{
	size_t i;

	for (i = 0; i < size; i++)
		data[i] = seed + i;
}

/* A captured pin-C training order, four lanes, HBR2. */
static void dcpext_link_pin_c_training_test(struct kunit *test)
{
	struct dcpext_link link = link_routed(4);
	struct dcpext_link_op op;
	u8 data[256], reply[256];
	unsigned int i;

	/* 18: supports HPD. */
	fill(data, 32, 0x10);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 18, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_NONE);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), 1);
	KUNIT_EXPECT_MEMEQ(test, reply + 4, data + 4, 12);
	KUNIT_EXPECT_MEMEQ(test, reply + 20, data + 20, 12);

	/* 10: four lanes (u64), 0: AUX on. */
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 10, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(reply + 16), 4);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 0, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_AUX_ON);
	KUNIT_EXPECT_TRUE(test, link.aux);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);

	/* After the HPD reply and client open: 21, 7 (HBR2), 13. */
	link.linked = true;
	fill(data, 144, 0x40);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 21, data, 144, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);
	KUNIT_EXPECT_MEMEQ(test, reply + 4, data + 4, 140);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 7, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), DCPEXT_RATE_HBR2);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 13, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), 0);

	/* 5 parks the crossbar, 12 sets four lanes, 9 HBR2, 6 brings it up. */
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 5, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_XBAR_DOWN);
	KUNIT_EXPECT_FALSE(test, link.xbar_up);
	memset(data, 0, 32);
	put_unaligned_le64(4, data + 16);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 12, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_LANES);
	KUNIT_EXPECT_EQ(test, op.value, 4);
	KUNIT_EXPECT_EQ(test, link.lanes, 4);
	memset(data, 0, 32);
	put_unaligned_le32(DCPEXT_RATE_HBR2, data + 16);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 9, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_RATE);
	KUNIT_EXPECT_EQ(test, op.value * 270, 5400);
	KUNIT_EXPECT_EQ(test, link.rate, DCPEXT_RATE_HBR2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), DCPEXT_RATE_HBR2);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 6, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_XBAR_UP);
	KUNIT_EXPECT_TRUE(test, link.xbar_up);

	/* 15 (no down-spread), 8 and 11 report the state. */
	memset(data, 0, 32);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 15, data, 32, reply, &op, true), 0);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 8, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), DCPEXT_RATE_HBR2);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 11, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(reply + 16), 4);

	/* 2: maximum swing and pre-emphasis 3/3. */
	fill(data, 32, 0x80);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 2, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), 3);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 20), 3);

	/* 3: an asymmetric four-lane request (64 bytes) is programmed per pair. */
	memset(data, 0, 64);
	put_unaligned_le32(4, data + 16);
	put_unaligned_le32(0, data + 32);	/* lane 0: 0/1 */
	put_unaligned_le32(1, data + 36);
	put_unaligned_le32(1, data + 40);	/* lane 1: 1/0 */
	put_unaligned_le32(0, data + 44);
	put_unaligned_le32(2, data + 48);	/* lane 2: 2/0 */
	put_unaligned_le32(0, data + 52);
	put_unaligned_le32(0, data + 56);	/* lane 3: 0/2 */
	put_unaligned_le32(2, data + 60);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 3, data, 64, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_DRIVE);
	KUNIT_EXPECT_EQ(test, op.lanes, 4);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);

	/* 4 reports the lane count at +0 and what was programmed. */
	fill(data, 64, 0x20);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 4, data, 64, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 4);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), 0);
	for (i = 0; i < 4; i++) {
		static const u32 v[4] = { 2, 1, 2, 1 }, p[4] = { 1, 2, 1, 2 };

		KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 32 + 8 * i), v[i]);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 36 + 8 * i), p[i]);
	}
	KUNIT_EXPECT_MEMEQ(test, reply + 4, data + 4, 12);
}

/* Pin D: two lanes, 48-byte drive bodies, one pair. */
static void dcpext_link_pin_d_test(struct kunit *test)
{
	struct dcpext_link link = link_routed(2);
	struct dcpext_link_op op;
	u8 data[64] = {}, reply[64];

	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 10, data, 32, reply, &op, false), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le64(reply + 16), 2);
	put_unaligned_le64(2, data + 16);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 12, data, 32, reply, &op, false), 0);
	KUNIT_EXPECT_EQ(test, link.lanes, 2);

	/* Drive at rate 0 is stored, not programmed. */
	memset(data, 0, 48);
	put_unaligned_le32(2, data + 16);
	put_unaligned_le32(1, data + 32);
	put_unaligned_le32(2, data + 36);
	put_unaligned_le32(3, data + 40);
	put_unaligned_le32(0, data + 44);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 3, data, 48, reply, &op, false), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_NONE);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 4, data, 48, reply, &op, false), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 32), 1);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 36), 2);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 40), 3);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 44), 0);
}

/* DPMS: the link drops to rate 0 and the crossbar comes back up while linked. */
static void dcpext_link_dpms_test(struct kunit *test)
{
	struct dcpext_link link = {
		.lanes_max = 4, .lanes = 4, .rate = DCPEXT_RATE_HBR2,
		.aux = true, .xbar_up = true, .linked = true,
	};
	struct dcpext_link_op op;
	u8 data[32] = {}, reply[32];

	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 5, data, 16, reply, &op, true), 0);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 12, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.value, 0);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 9, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_RATE);
	KUNIT_EXPECT_EQ(test, link.rate, 0);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 6, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_XBAR_UP);
	KUNIT_EXPECT_TRUE(test, link.xbar_up);

	/* Not linked (released): rate 0 leaves the crossbar parked. */
	link.linked = false;
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 5, data, 16, reply, &op, true), 0);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 6, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_NONE);
	KUNIT_EXPECT_FALSE(test, link.xbar_up);

	/* 1: deactivate stops everything. */
	link = (struct dcpext_link) { .lanes_max = 4, .lanes = 4, .rate = 0x14,
				      .aux = true, .xbar_up = true, .linked = true };
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 1, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_DEACTIVATE);
	KUNIT_EXPECT_FALSE(test, link.aux);
	KUNIT_EXPECT_FALSE(test, link.xbar_up);
	KUNIT_EXPECT_EQ(test, link.rate, 0);
	KUNIT_EXPECT_EQ(test, link.lanes, 0);
}

/* Callback 20 holds presents until a link-up; 20 and 22 keep their data. */
static void dcpext_link_inactive_test(struct kunit *test)
{
	struct dcpext_link link = link_routed(4);
	struct dcpext_link_op op;
	u8 data[32], reply[32];

	link.linked = true;
	fill(data, 16, 0xa0);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 20, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_TRUE(test, link.inactive);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);
	KUNIT_EXPECT_MEMEQ(test, reply + 4, data + 4, 12);

	/* A rate-0 link-up keeps it inactive; a running link ends it. */
	link.xbar_up = false;
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 6, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_TRUE(test, link.inactive);
	link.rate = DCPEXT_RATE_HBR;
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 6, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_FALSE(test, link.inactive);

	/* 22: device not responding, ACKed with status 0, data kept. */
	fill(data, 16, 0x33);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 22, data, 16, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply), 0);
	KUNIT_EXPECT_MEMEQ(test, reply + 4, data + 4, 12);
}

/* Requests outside the qualified profile get no reply at all. */
static void dcpext_link_refusals_test(struct kunit *test)
{
	struct dcpext_link link = link_routed(4), unrouted = {};
	struct dcpext_link_op op;
	u8 data[64] = {};
	static const u32 unknown[] = { 17, 19, 25, 26, 31, 99 };
	unsigned int i;

	/* Pin C takes four lanes or none. */
	put_unaligned_le64(2, data + 16);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 12, data, 32, &op), -EINVAL);
	/* Down-spread, HBR3 and unknown codes. */
	memset(data, 0, 32);
	put_unaligned_le32(DCPEXT_RATE_HBR2, data + 16);
	put_unaligned_le32(1, data + 20);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 9, data, 32, &op), -EINVAL);
	put_unaligned_le32(0, data + 20);
	put_unaligned_le32(0x1e, data + 16);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 9, data, 32, &op), -EINVAL);
	put_unaligned_le32(0x0c, data + 16);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 9, data, 32, &op), -EINVAL);
	/* Drive: the wrong width, a level over 3, a sum over 3, a short body. */
	memset(data, 0, 64);
	put_unaligned_le32(2, data + 16);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 3, data, 64, &op), -EINVAL);
	put_unaligned_le32(4, data + 16);
	put_unaligned_le32(4, data + 32);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 3, data, 64, &op), -EINVAL);
	put_unaligned_le32(2, data + 32);
	put_unaligned_le32(2, data + 36);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 3, data, 64, &op), -EINVAL);
	put_unaligned_le32(0, data + 36);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 3, data, 56, &op), -EINVAL);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 4, data, 48, &op), -EMSGSIZE);
	/* Down-spread enable is refused. */
	memset(data, 0, 32);
	put_unaligned_le64(1, data + 16);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 15, data, 32, &op), -EINVAL);
	/* Short callbacks, AUX without a route, and every unknown index. */
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 9, data, 20, &op), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 0, data, 2, &op), -EMSGSIZE);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&unrouted, 0, data, 16, &op), -ENODEV);
	for (i = 0; i < ARRAY_SIZE(unknown); i++)
		KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, unknown[i], data, 32, &op),
				-EOPNOTSUPP);
	/* A reply never overruns its room. */
	KUNIT_ASSERT_EQ(test, dcpext_link_plan(&link, 18, data, 32, &op), 0);
	KUNIT_EXPECT_EQ(test, dcpext_link_reply(&link, 18, &op, data, 32, data + 32, 16),
			-EMSGSIZE);
}

/* A route whose PHY reports HBR3 (every T6030 ATC). */
static void dcpext_link_hdmi_hbr3_test(struct kunit *test)
{
	struct dcpext_link link = link_routed(4);
	struct dcpext_link_op op;
	u8 data[32] = {}, reply[32];

	link.rate_max = DCPEXT_RATE_HBR3;
	/* Callback 7 reports HBR3. */
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 7, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), DCPEXT_RATE_HBR3);
	/* Callback 9 programs HBR3 and parks the crossbar first. */
	put_unaligned_le32(DCPEXT_RATE_HBR3, data + 16);
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 9, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, op.hw, DCPEXT_HW_RATE);
	KUNIT_EXPECT_EQ(test, op.value, DCPEXT_RATE_HBR3);
	KUNIT_EXPECT_EQ(test, link.rate, DCPEXT_RATE_HBR3);
	KUNIT_EXPECT_FALSE(test, link.xbar_up);
	/* A PHY that reports no rate: HBR2, and HBR3 is refused. */
	link = link_routed(4);
	memset(data, 0, sizeof(data));
	KUNIT_ASSERT_EQ(test, link_cb(test, &link, 7, data, 32, reply, &op, true), 0);
	KUNIT_EXPECT_EQ(test, get_unaligned_le32(reply + 16), DCPEXT_RATE_HBR2);
	put_unaligned_le32(DCPEXT_RATE_HBR3, data + 16);
	KUNIT_EXPECT_EQ(test, dcpext_link_plan(&link, 9, data, 32, &op), -EINVAL);
}

static struct kunit_case dcpext_link_cases[] = {
	KUNIT_CASE(dcpext_link_hdmi_hbr3_test),
	KUNIT_CASE(dcpext_link_pin_c_training_test),
	KUNIT_CASE(dcpext_link_pin_d_test),
	KUNIT_CASE(dcpext_link_dpms_test),
	KUNIT_CASE(dcpext_link_inactive_test),
	KUNIT_CASE(dcpext_link_refusals_test),
	{}
};

static struct kunit_suite dcpext_link_suite = {
	.name = "apple-dcpext-dptx",
	.test_cases = dcpext_link_cases,
};

/* ---- HPD and IRQ_HPD ---- */

/* Each fresh IRQ_HPD is one InterruptRequestOccurred, identical or not. */
static void dcpext_hpd_irq_multiplicity_test(struct kunit *test)
{
	struct dcpext_hpd hpd = {};
	u64 generation;

	KUNIT_EXPECT_TRUE(test, dcpext_hpd_set(&hpd, true));
	KUNIT_EXPECT_FALSE(test, dcpext_hpd_set(&hpd, true));
	KUNIT_EXPECT_EQ(test, hpd.generation, 1);
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq(&hpd, true));
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq(&hpd, true));
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq_take(&hpd, &generation));
	KUNIT_EXPECT_EQ(test, generation, 1);
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq_take(&hpd, &generation));
	KUNIT_EXPECT_FALSE(test, dcpext_hpd_irq_take(&hpd, &generation));
	KUNIT_EXPECT_EQ(test, hpd.irq_queued, 2);
	KUNIT_EXPECT_EQ(test, hpd.irq_dropped, 0);
}

/* No IRQ for a sink the firmware does not have, none of an earlier sink. */
static void dcpext_hpd_irq_generation_test(struct kunit *test)
{
	struct dcpext_hpd hpd = {};
	u64 generation;

	/* HPD low, or high but not yet delivered: dropped. */
	KUNIT_EXPECT_FALSE(test, dcpext_hpd_irq(&hpd, true));
	dcpext_hpd_set(&hpd, true);
	KUNIT_EXPECT_FALSE(test, dcpext_hpd_irq(&hpd, false));
	KUNIT_EXPECT_EQ(test, hpd.irq_dropped, 2);

	/* Queued, then unplugged: dropped; the replugged sink starts empty. */
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq(&hpd, true));
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_set(&hpd, false));
	KUNIT_EXPECT_FALSE(test, dcpext_hpd_irq_take(&hpd, &generation));
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_set(&hpd, true));
	KUNIT_EXPECT_FALSE(test, dcpext_hpd_irq_take(&hpd, &generation));
	KUNIT_EXPECT_EQ(test, hpd.generation, 3);
	KUNIT_EXPECT_EQ(test, hpd.irq_dropped, 3);
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq(&hpd, true));
	KUNIT_EXPECT_TRUE(test, dcpext_hpd_irq_take(&hpd, &generation));
	KUNIT_EXPECT_EQ(test, generation, 3);
}

/* The route's duplicate-state filter lets a fresh IRQ_HPD through, once. */
static void dcpext_route_irq_bypass_test(struct kunit *test)
{
	struct apple_dcp *ext, *other;
	struct apple_dcp_typec_route *owner, *candidate, *panel_route;
	const u32 irq = DP_STATUS_HPD_STATE | DP_STATUS_IRQ_HPD;

	ext = kunit_kzalloc(test, sizeof(*ext), GFP_KERNEL);
	other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, ext);
	KUNIT_ASSERT_NOT_NULL(test, other);
	ext->fw_compat = DCP_FIRMWARE_V_14_7;
	ext->hw.v14_7_external = true;
	other->fw_compat = DCP_FIRMWARE_V_13_5;
	owner = &ext->typec_routes[0];
	owner->dcp = ext;
	candidate = &ext->typec_routes[1];
	candidate->dcp = ext;
	panel_route = &other->typec_routes[0];
	panel_route->dcp = other;

	KUNIT_EXPECT_TRUE(test, iomfb_is_v14_7_external(ext));
	KUNIT_EXPECT_FALSE(test, iomfb_is_v14_7_panel(ext));
	KUNIT_EXPECT_TRUE(test, dcp_typec_route_irq_hpd(owner, owner, irq));
	/* Only the owning route forwards it. */
	KUNIT_EXPECT_FALSE(test, dcp_typec_route_irq_hpd(candidate, owner, irq));
	KUNIT_EXPECT_FALSE(test, dcp_typec_route_irq_hpd(owner, NULL, irq));
	/* HPD low, or no IRQ bit: an ordinary duplicate. */
	KUNIT_EXPECT_FALSE(test, dcp_typec_route_irq_hpd(owner, owner, DP_STATUS_IRQ_HPD));
	KUNIT_EXPECT_FALSE(test, dcp_typec_route_irq_hpd(owner, owner, DP_STATUS_HPD_STATE));
	/* Other firmware keeps the duplicate filter. */
	KUNIT_EXPECT_FALSE(test, dcp_typec_route_irq_hpd(panel_route, panel_route, irq));
}

static struct kunit_case dcpext_hpd_cases[] = {
	KUNIT_CASE(dcpext_hpd_irq_multiplicity_test),
	KUNIT_CASE(dcpext_hpd_irq_generation_test),
	KUNIT_CASE(dcpext_route_irq_bypass_test),
	{}
};

static struct kunit_suite dcpext_hpd_suite = {
	.name = "apple-dcpext-hpd",
	.test_cases = dcpext_hpd_cases,
};

/* ---- Catalog: an OSSerialize writer and fw14-shaped fixtures ---- */

struct oss {
	struct kunit *test;
	u8 *buf;
	size_t len, cap;
};

static void oss_put(struct oss *o, const void *data, size_t n)
{
	KUNIT_ASSERT_LE(o->test, o->len + n + 4, o->cap);
	memcpy(o->buf + o->len, data, n);
	o->len += n;
}

static void oss_tag(struct oss *o, u32 type, u32 count)
{
	u8 tag[4];

	while (o->len % 4)
		o->buf[o->len++] = 0;
	put_unaligned_le32(type << 24 | count, tag);
	oss_put(o, tag, 4);
}

static void oss_num(struct oss *o, u64 value)
{
	u8 v[8];

	oss_tag(o, 4, 64);
	put_unaligned_le64(value, v);
	oss_put(o, v, 8);
}

static void oss_str(struct oss *o, const char *s)
{
	oss_tag(o, 9, strlen(s));
	oss_put(o, s, strlen(s));
}

static void oss_data(struct oss *o, size_t n)
{
	static const u8 zero[96];

	oss_tag(o, 10, n);
	oss_put(o, zero, n);
}

static void oss_bool(struct oss *o, const char *key, bool b)
{
	oss_str(o, key);
	oss_tag(o, 11, b);
}

static void oss_knum(struct oss *o, const char *key, u64 value)
{
	oss_str(o, key);
	oss_num(o, value);
}

/* A colour element as fw14 publishes it; score < 0 omits Score (virtual). */
struct fix_color {
	u32 id;
	bool virtual, supports_dsc;
	s64 score;
	u32 depth, encoding, range, eotf, colorimetry;
};

struct fix_axis {
	u32 active, total, front, sync, back, polarity, repetition, precise;
};

struct fix_timing {
	u32 id;
	s64 score;
	bool virtual, interlaced, split, preferred;
	s32 hpipes, vpipes;	/* < 0: absent, as on fw14 */
	struct fix_axis h, v;
	const struct fix_color *colors;
	unsigned int ncolors;
	const u32 *dsc;
	unsigned int ndsc;
	const u32 *unsafe;
	unsigned int nunsafe;
};

static void fix_axis(struct oss *o, const char *key, const struct fix_axis *a)
{
	oss_str(o, key);
	oss_tag(o, 1, 9);
	oss_knum(o, "Total", a->total);
	oss_knum(o, "FrontPorch", a->front);
	oss_knum(o, "SyncWidth", a->sync);
	oss_knum(o, "BackPorch", a->back);
	oss_knum(o, "Active", a->active);
	oss_knum(o, "SyncRate", a->precise);
	oss_knum(o, "PreciseSyncRate", a->precise);
	oss_knum(o, "SyncPolarity", a->polarity);
	oss_knum(o, "PixelRepetition", a->repetition);
}

static void fix_color(struct oss *o, const struct fix_color *c)
{
	bool metadata = !c->virtual && c->colorimetry == 16;

	oss_tag(o, 1, 11 + (c->score >= 0) + (metadata ? 3 : 1) - c->virtual);
	if (c->score >= 0)
		oss_knum(o, "Score", c->score);
	oss_knum(o, "ID", c->id);
	oss_bool(o, "IsVirtual", c->virtual);
	oss_knum(o, "ElementType", 1);
	oss_str(o, "ElementData");
	oss_data(o, 32);
	oss_knum(o, "Depth", c->depth);
	oss_knum(o, "PixelEncoding", c->encoding);
	oss_knum(o, "DynamicRange", c->range);
	oss_knum(o, "EOTF", c->eotf);
	oss_knum(o, "Colorimetry", c->colorimetry);
	oss_knum(o, "SupportsDSC", c->supports_dsc);
	oss_knum(o, "StandardType", 2);
	if (metadata) {
		/* Nested metadata the selection skips. */
		oss_str(o, "Chromaticity");
		oss_tag(o, 1, 3);
		oss_str(o, "Red");
		oss_tag(o, 1, 2);
		oss_knum(o, "X", 42432);
		oss_knum(o, "Y", 22336);
		oss_str(o, "Green");
		oss_tag(o, 1, 2);
		oss_knum(o, "X", 20864);
		oss_knum(o, "Y", 40512);
		oss_str(o, "Blue");
		oss_tag(o, 1, 2);
		oss_knum(o, "X", 9984);
		oss_knum(o, "Y", 3840);
		oss_str(o, "WhitePoint");
		oss_tag(o, 1, 3);
		oss_knum(o, "X", 20544);
		oss_knum(o, "Y", 21568);
		oss_knum(o, "Gamma", 144179);
	}
	if (!c->virtual) {
		oss_str(o, "HDRStaticMetadataTypes");
		oss_tag(o, 2, 0);
	}
}

static void fix_ids(struct oss *o, const char *key, const u32 *ids, unsigned int n)
{
	unsigned int i;

	oss_str(o, key);
	oss_tag(o, 2, n);
	for (i = 0; i < n; i++)
		oss_num(o, ids[i]);
}

static void fix_timing(struct oss *o, const struct fix_timing *t)
{
	unsigned int keys = 26 + (t->hpipes >= 0) + (t->vpipes >= 0), i;

	oss_tag(o, 1, keys);
	oss_knum(o, "Score", t->score);
	oss_knum(o, "ID", t->id);
	oss_bool(o, "IsVirtual", t->virtual);
	oss_knum(o, "ElementType", 0);
	oss_str(o, "ElementData");
	oss_data(o, 80);
	oss_str(o, "ColorModes");
	oss_tag(o, 2, t->ncolors);
	for (i = 0; i < t->ncolors; i++)
		fix_color(o, &t->colors[i]);
	fix_ids(o, "UnsafeColorElementIDs", t->unsafe, t->nunsafe);
	fix_ids(o, "DSCRequiredColorElementIDs", t->dsc, t->ndsc);
	fix_ids(o, "DiscreteMediaRefreshRates", NULL, 0);
	oss_bool(o, "IsInterlaced", t->interlaced);
	oss_bool(o, "IsSplit", t->split);
	oss_bool(o, "IsOverscanned", true);
	oss_bool(o, "IsPreferred", t->preferred);
	oss_bool(o, "IsPromoted", t->preferred);
	oss_knum(o, "ScanInformation", 1);
	oss_knum(o, "ValidPixelEncodings", 7037);
	oss_knum(o, "StandardType", 2);
	oss_knum(o, "TimingType", 4);
	oss_knum(o, "AspectRatio", 15);
	oss_knum(o, "PreciseAspectRatio", 116508);
	oss_knum(o, "TimingStandard", 4);
	oss_knum(o, "CEAShortID", 16);
	if (t->hpipes >= 0)
		oss_knum(o, "HorizontalPipeCount", t->hpipes);
	if (t->vpipes >= 0)
		oss_knum(o, "VerticalPipeCount", t->vpipes);
	fix_axis(o, "HorizontalAttributes", &t->h);
	fix_axis(o, "VerticalAttributes", &t->v);
	oss_knum(o, "MinimumVariableRefreshRate", 0);
	oss_knum(o, "MaximumVariableRefreshRate", 0);
}

/* The Philips monitor's colour list (timing 60 and its siblings). */
static const struct fix_color philips_colors[] = {
	{ .id = 1, .virtual = true, .score = -1, .depth = 8, .colorimetry = 10 },
	{ .id = 2, .virtual = true, .score = -1, .depth = 8, .range = 1, .colorimetry = 10 },
	{ .id = 73, .score = 232999, .depth = 8, .encoding = 3, .range = 1, .colorimetry = 1 },
	{ .id = 70, .score = 240999, .depth = 8, .colorimetry = 16 },
	{ .id = 71, .score = 240999, .depth = 8, .range = 1, .colorimetry = 16 },
	{ .id = 72, .score = 231999, .depth = 8, .encoding = 3, .range = 1 },
};

#define CEA_1080_H { 1920, 2200, 88, 44, 148, 1, 0, 4423680 }

/*
 * The Sony TV's admissible colours: 8-bit sRGB, AdobeRGB, 10-bit sRGB, each
 * taken compressed by the TV (SupportsDSC 1, as published on the HDMI port).
 */
static const struct fix_color sony_colors[] = {
	{ .id = 202, .score = 242999, .depth = 8, .colorimetry = 10, .supports_dsc = true },
	{ .id = 204, .score = 242999, .depth = 8, .range = 1, .colorimetry = 10,
	  .supports_dsc = true },
	{ .id = 5, .virtual = true, .score = -1, .depth = 8, .encoding = 3, .range = 1,
	  .colorimetry = 1 },
	{ .id = 203, .score = 244999, .depth = 8, .colorimetry = 13, .supports_dsc = true },
	{ .id = 211, .score = 442999, .depth = 10, .colorimetry = 10, .supports_dsc = true },
};

/* The same colours from a sink path without DSC (SupportsDSC 0). */
static const struct fix_color sony_colors_no_dsc[] = {
	{ .id = 202, .score = 242999, .depth = 8, .colorimetry = 10 },
	{ .id = 204, .score = 242999, .depth = 8, .range = 1, .colorimetry = 10 },
	{ .id = 203, .score = 244999, .depth = 8, .colorimetry = 13 },
	{ .id = 211, .score = 442999, .depth = 10, .colorimetry = 10 },
};

static const u32 sony_dsc[] = { 202, 204, 5, 203, 211 };

/* Derived from the captured publications (and variants of their timing 60). */
static const struct fix_timing catalog[] = {
	/* Philips 60: 1080p60, preferred. */
	{ .id = 60, .score = 16224, .preferred = true, .hpipes = -1, .vpipes = -1,
	  .h = CEA_1080_H, .v = { 1080, 1125, 4, 5, 36, 1, 0, 3932160 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	/* Philips 59: 1080p59.94, same score, listed later. */
	{ .id = 59, .score = 16224, .hpipes = -1, .vpipes = -1,
	  .h = CEA_1080_H, .v = { 1080, 1125, 4, 5, 36, 1, 0, 3928232 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	/* Philips 6: virtual 480p. */
	{ .id = 6, .virtual = true, .hpipes = -1, .vpipes = -1,
	  .h = { 720, 858, 16, 62, 60, 0, 0, 2062322 },
	  .v = { 480, 525, 9, 6, 30, 0, 0, 3928232 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	/* Philips 24: DMT 640x480@75, no horizontal precise rate. */
	{ .id = 24, .score = 15098, .hpipes = -1, .vpipes = -1,
	  .h = { 640, 840, 16, 64, 120, 0, 0, 0 },
	  .v = { 480, 500, 1, 3, 16, 0, 0, 4915200 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	/* Sony 167: 2160p60, 594 MHz: four lanes only. */
	{ .id = 167, .score = 16598, .preferred = true, .hpipes = -1, .vpipes = -1,
	  .h = { 3840, 4400, 176, 88, 296, 1, 0, 8847360 },
	  .v = { 2160, 2250, 8, 10, 72, 1, 0, 3932160 },
	  .colors = sony_colors, .ncolors = ARRAY_SIZE(sony_colors) },
	/* Sony 156: 2160p30. */
	{ .id = 156, .score = 15379, .hpipes = -1, .vpipes = -1,
	  .h = { 3840, 4400, 176, 88, 296, 1, 0, 4423680 },
	  .v = { 2160, 2250, 8, 10, 72, 1, 0, 1966080 },
	  .colors = sony_colors, .ncolors = ARRAY_SIZE(sony_colors) },
	/* Sony 164: 2160p120, its colours DSC-required. */
	{ .id = 164, .score = 16215, .hpipes = -1, .vpipes = -1,
	  .h = { 3840, 4400, 176, 88, 296, 1, 0, 17694720 },
	  .v = { 2160, 2250, 8, 10, 72, 1, 0, 7864320 },
	  .colors = sony_colors, .ncolors = ARRAY_SIZE(sony_colors),
	  .dsc = sony_dsc, .ndsc = ARRAY_SIZE(sony_dsc) },
	/* Sony 173: 4096x2160p60, wider than the qualified pipe. */
	{ .id = 173, .score = 16631, .hpipes = -1, .vpipes = -1,
	  .h = { 4096, 4400, 88, 88, 128, 1, 0, 8847360 },
	  .v = { 2160, 2250, 8, 10, 72, 1, 0, 3932160 },
	  .colors = sony_colors, .ncolors = ARRAY_SIZE(sony_colors) },
	/* Sony 158: 2160p23.976, below 24 Hz. */
	{ .id = 158, .score = 15323, .hpipes = -1, .vpipes = -1,
	  .h = { 3840, 5500, 1276, 88, 296, 1, 0, 3535409 },
	  .v = { 2160, 2250, 8, 10, 72, 1, 0, 1571293 },
	  .colors = sony_colors, .ncolors = ARRAY_SIZE(sony_colors) },
	/* Variants of Philips 60 outside the profile. */
	{ .id = 90, .score = 1, .interlaced = true, .hpipes = -1, .vpipes = -1,
	  .h = CEA_1080_H, .v = { 1080, 1125, 4, 5, 36, 1, 0, 3932160 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	{ .id = 91, .score = 1, .split = true, .hpipes = -1, .vpipes = -1,
	  .h = CEA_1080_H, .v = { 1080, 1125, 4, 5, 36, 1, 0, 3932160 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	{ .id = 92, .score = 1, .hpipes = 2, .vpipes = 1,
	  .h = CEA_1080_H, .v = { 1080, 1125, 4, 5, 36, 1, 0, 3932160 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	{ .id = 93, .score = 1, .hpipes = -1, .vpipes = -1,
	  .h = { 1920, 2200, 88, 44, 148, 1, 1, 4423680 },
	  .v = { 1080, 1125, 4, 5, 36, 1, 0, 3932160 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
	/* Present pipe counts of one are accepted. */
	{ .id = 94, .score = 2, .hpipes = 1, .vpipes = 1,
	  .h = CEA_1080_H, .v = { 1080, 1125, 4, 5, 36, 1, 0, 3276800 },
	  .colors = philips_colors, .ncolors = ARRAY_SIZE(philips_colors) },
};

static void *build_catalog(struct kunit *test, const struct fix_timing *t,
			   unsigned int n, size_t *size)
{
	struct oss o = { .test = test, .cap = SZ_64K };
	unsigned int i;
	u8 magic[4];

	o.buf = kunit_kzalloc(test, o.cap, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, o.buf);
	put_unaligned_le32(0xd3, magic);
	oss_put(&o, magic, 4);
	oss_tag(&o, 2, n);
	for (i = 0; i < n; i++)
		fix_timing(&o, &t[i]);
	*size = o.len;
	return o.buf;
}

static struct dcp_display_mode *catalog_parse(struct kunit *test, const void *blob,
					      size_t size, unsigned int *count, bool dsc)
{
	struct dcp_display_mode *modes;
	struct dcp_parse_ctx ctx;

	KUNIT_ASSERT_EQ(test, parse(blob, size, &ctx), 0);
	ctx.v14_7_rgb8 = true;
	ctx.v14_7_dsc = dsc;
	modes = enumerate_modes(&ctx, count, 0, 0, 0, false, true);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(modes));
	return modes;
}

static struct dcp_display_mode *catalog_modes(struct kunit *test, const void *blob,
					      size_t size, unsigned int *count)
{
	return catalog_parse(test, blob, size, count, false);
}

static const struct dcp_display_mode *find_timing(const struct dcp_display_mode *modes,
						  unsigned int count, u32 id)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (modes[i].timing_mode_id == id)
			return &modes[i];
	return NULL;
}

/* fw14 shape: virtual colours without Score, no pipe counts, nested metadata. */
static void dcpext_catalog_fw14_shape_test(struct kunit *test)
{
	const struct dcp_display_mode *m;
	struct dcp_display_mode *modes;
	unsigned int count, kept, rejected;
	size_t size;
	void *blob;

	blob = build_catalog(test, catalog, ARRAY_SIZE(catalog), &size);
	modes = catalog_modes(test, blob, size, &count);

	/* Parser: virtual 6, the variants 90-93 and DSC-only 164 are refused. */
	KUNIT_EXPECT_EQ(test, count, 8);
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 6));
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 90));
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 91));
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 92));
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 93));
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 164));

	/* Philips 60: colour 70 (limited range 71 and YCbCr 72/73 lose). */
	m = find_timing(modes, count, 60);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_EQ(test, m->color_mode_id, 70);
	KUNIT_EXPECT_EQ(test, m->mode.clock, 148500);
	KUNIT_EXPECT_EQ(test, m->mode.hdisplay, 1920);
	KUNIT_EXPECT_EQ(test, m->mode.hsync_start, 2008);
	KUNIT_EXPECT_EQ(test, m->mode.hsync_end, 2052);
	KUNIT_EXPECT_EQ(test, m->mode.htotal, 2200);
	KUNIT_EXPECT_EQ(test, m->mode.vdisplay, 1080);
	KUNIT_EXPECT_EQ(test, m->mode.vsync_start, 1084);
	KUNIT_EXPECT_EQ(test, m->mode.vsync_end, 1089);
	KUNIT_EXPECT_EQ(test, m->mode.vtotal, 1125);
	KUNIT_EXPECT_EQ(test, m->mode.flags, DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC);
	KUNIT_EXPECT_FALSE(test, m->vrr);
	/* DMT 640x480@75 without a horizontal precise rate is admitted. */
	m = find_timing(modes, count, 24);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_EQ(test, m->mode.clock, 31500);
	KUNIT_EXPECT_EQ(test, m->mode.flags, DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC);
	/* Sony: the 8-bit sRGB colour, never 10-bit 211 or AdobeRGB 203. */
	m = find_timing(modes, count, 167);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_EQ(test, m->color_mode_id, 202);
	KUNIT_EXPECT_EQ(test, m->mode.clock, 594000);
	KUNIT_ASSERT_NOT_NULL(test, find_timing(modes, count, 94));

	/* Four lanes of HBR2: 4K60 fits; 4096 wide and 23.976 Hz do not. */
	kept = dcpext_catalog_filter(modes, count, 4, 0, &rejected);
	KUNIT_EXPECT_EQ(test, kept, 6);
	KUNIT_EXPECT_EQ(test, rejected, 2);
	KUNIT_EXPECT_NULL(test, find_timing(modes, kept, 173));
	KUNIT_EXPECT_NULL(test, find_timing(modes, kept, 158));
	KUNIT_EXPECT_NOT_NULL(test, find_timing(modes, kept, 167));
	/* The highest score (173) is gone: the largest, fastest mode is preferred. */
	m = find_timing(modes, kept, 167);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_TRUE(test, m->mode.type & DRM_MODE_TYPE_PREFERRED);
	kfree(modes);
}

/* Two lanes (pin D, 8.64 Gb/s): 4K60 is over budget, 4K30 is the largest. */
static void dcpext_catalog_budget_test(struct kunit *test)
{
	const struct dcp_display_mode *m;
	struct dcp_display_mode *modes;
	unsigned int count, kept, rejected, i, preferred = 0;
	size_t size;
	void *blob;

	blob = build_catalog(test, catalog, ARRAY_SIZE(catalog), &size);
	modes = catalog_modes(test, blob, size, &count);
	kept = dcpext_catalog_filter(modes, count, 2, 0, &rejected);
	KUNIT_EXPECT_EQ(test, kept, 5);
	KUNIT_EXPECT_EQ(test, rejected, 3);
	KUNIT_EXPECT_NULL(test, find_timing(modes, kept, 167));
	m = find_timing(modes, kept, 156);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_TRUE(test, m->mode.type & DRM_MODE_TYPE_PREFERRED);
	for (i = 0; i < kept; i++)
		preferred += !!(modes[i].mode.type & DRM_MODE_TYPE_PREFERRED);
	KUNIT_EXPECT_EQ(test, preferred, 1);
	/* No route: nothing fits. */
	KUNIT_EXPECT_EQ(test, dcpext_catalog_filter(modes, kept, 0, 0, &rejected), 0);
	kfree(modes);

	/* Two lanes of HBR3 (12.96 Gb/s) carry 4K60 (14.26 Gb/s) neither. */
	modes = catalog_modes(test, blob, size, &count);
	kept = dcpext_catalog_filter(modes, count, 2, DCPEXT_RATE_HBR3, &rejected);
	KUNIT_EXPECT_NULL(test, find_timing(modes, kept, 167));
	kfree(modes);
	/* Four lanes of HBR3 do, as four of HBR2. */
	modes = catalog_modes(test, blob, size, &count);
	kept = dcpext_catalog_filter(modes, count, 4, DCPEXT_RATE_HBR3, &rejected);
	KUNIT_EXPECT_NOT_NULL(test, find_timing(modes, kept, 167));
	kfree(modes);
}

/* The firmware's own preferred timing stays preferred when it fits. */
static void dcpext_catalog_preferred_test(struct kunit *test)
{
	const struct dcp_display_mode *m;
	struct dcp_display_mode *modes;
	unsigned int count, kept, rejected;
	size_t size;
	void *blob;

	blob = build_catalog(test, catalog, 2, &size);
	modes = catalog_modes(test, blob, size, &count);
	KUNIT_ASSERT_EQ(test, count, 2);
	kept = dcpext_catalog_filter(modes, count, 2, 0, &rejected);
	KUNIT_EXPECT_EQ(test, kept, 2);
	m = find_timing(modes, kept, 60);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_TRUE(test, m->mode.type & DRM_MODE_TYPE_PREFERRED);
	m = find_timing(modes, kept, 59);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_FALSE(test, m->mode.type & DRM_MODE_TYPE_PREFERRED);
	kfree(modes);
}

/* Without the fw14 profile the common path keeps its own (10-bit) choice. */
static void dcpext_catalog_profile_scope_test(struct kunit *test)
{
	struct dcp_display_mode *modes;
	struct dcp_parse_ctx ctx;
	unsigned int count;
	size_t size;
	void *blob;

	blob = build_catalog(test, &catalog[4], 1, &size);
	KUNIT_ASSERT_EQ(test, parse(blob, size, &ctx), 0);
	modes = enumerate_modes(&ctx, &count, 0, 0, 0, false, true);
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(modes));
	KUNIT_ASSERT_EQ(test, count, 1);
	KUNIT_EXPECT_EQ(test, modes[0].color_mode_id, 211);
	kfree(modes);
}

/* Equal scores: the lower colour ID wins. */
static void dcpext_catalog_color_tie_test(struct kunit *test)
{
	static const struct fix_color tie[] = {
		{ .id = 77, .score = 5000, .depth = 8, .colorimetry = 16 },
		{ .id = 75, .score = 5000, .depth = 8, .colorimetry = 10 },
		{ .id = 76, .score = 4000, .depth = 8, .colorimetry = 1 },
	};
	struct fix_timing t = catalog[0];
	struct dcp_display_mode *modes;
	unsigned int count;
	size_t size;
	void *blob;

	t.colors = tie;
	t.ncolors = ARRAY_SIZE(tie);
	blob = build_catalog(test, &t, 1, &size);
	modes = catalog_modes(test, blob, size, &count);
	KUNIT_ASSERT_EQ(test, count, 1);
	KUNIT_EXPECT_EQ(test, modes[0].color_mode_id, 75);
	kfree(modes);
}

/*
 * DSC: Sony 164 (2160p120, 1188 MHz) has only DSC-required colours. It is
 * admitted with its 8-bit sRGB colour when the sink takes it compressed and
 * 8 bpp fits the lanes less FEC: four lanes of HBR2 (pin C) or HBR3 (the
 * HDMI port), not two of HBR2 (pin D). 4K60 stays uncompressed.
 */
static void dcpext_catalog_dsc_test(struct kunit *test)
{
	struct fix_timing sony[] = { catalog[4], catalog[6] };
	const struct dcp_display_mode *m;
	struct dcp_display_mode *modes;
	unsigned int count, kept, rejected;
	size_t size;
	void *blob;

	blob = build_catalog(test, sony, ARRAY_SIZE(sony), &size);
	modes = catalog_parse(test, blob, size, &count, true);
	KUNIT_ASSERT_EQ(test, count, 2);
	m = find_timing(modes, count, 164);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_TRUE(test, m->dsc);
	KUNIT_EXPECT_EQ(test, m->color_mode_id, 202);
	KUNIT_EXPECT_EQ(test, m->mode.clock, 1188000);
	m = find_timing(modes, count, 167);
	KUNIT_ASSERT_NOT_NULL(test, m);
	KUNIT_EXPECT_FALSE(test, m->dsc);
	KUNIT_EXPECT_EQ(test, m->color_mode_id, 202);

	kept = dcpext_catalog_filter(modes, count, 4, DCPEXT_RATE_HBR2, &rejected);
	KUNIT_EXPECT_EQ(test, kept, 2);
	KUNIT_EXPECT_EQ(test, rejected, 0);
	kfree(modes);
	modes = catalog_parse(test, blob, size, &count, true);
	kept = dcpext_catalog_filter(modes, count, 4, DCPEXT_RATE_HBR3, &rejected);
	KUNIT_EXPECT_NOT_NULL(test, find_timing(modes, kept, 164));
	kfree(modes);
	/* Pin D: 1188 MHz at 8 bpp is 9.5 Gb/s, over 8.64 less FEC; 4K60 too. */
	modes = catalog_parse(test, blob, size, &count, true);
	kept = dcpext_catalog_filter(modes, count, 2, DCPEXT_RATE_HBR2, &rejected);
	KUNIT_EXPECT_EQ(test, kept, 0);
	KUNIT_EXPECT_EQ(test, rejected, 2);
	kfree(modes);

	/* A sink path without DSC: 164 is refused, 167 unchanged. */
	sony[0].colors = sony[1].colors = sony_colors_no_dsc;
	sony[0].ncolors = sony[1].ncolors = ARRAY_SIZE(sony_colors_no_dsc);
	blob = build_catalog(test, sony, ARRAY_SIZE(sony), &size);
	modes = catalog_parse(test, blob, size, &count, true);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 164));
	KUNIT_EXPECT_NOT_NULL(test, find_timing(modes, count, 167));
	kfree(modes);

	/* An unsafe DSC colour is never taken (202 is the only full-range sRGB). */
	sony[0].colors = sony[1].colors = sony_colors;
	sony[0].ncolors = sony[1].ncolors = ARRAY_SIZE(sony_colors);
	sony[1].unsafe = sony_dsc;
	sony[1].nunsafe = 1;
	blob = build_catalog(test, sony, ARRAY_SIZE(sony), &size);
	modes = catalog_parse(test, blob, size, &count, true);
	KUNIT_EXPECT_EQ(test, count, 1);
	KUNIT_EXPECT_NULL(test, find_timing(modes, count, 164));
	kfree(modes);
}

static struct kunit_case dcpext_catalog_cases[] = {
	KUNIT_CASE(dcpext_catalog_fw14_shape_test),
	KUNIT_CASE(dcpext_catalog_budget_test),
	KUNIT_CASE(dcpext_catalog_preferred_test),
	KUNIT_CASE(dcpext_catalog_profile_scope_test),
	KUNIT_CASE(dcpext_catalog_color_tie_test),
	KUNIT_CASE(dcpext_catalog_dsc_test),
	{}
};

static struct kunit_suite dcpext_catalog_suite = {
	.name = "apple-dcpext-catalog",
	.test_cases = dcpext_catalog_cases,
};

/* ---- The external present ---- */

#define SWAP_SIZE sizeof(struct dcp_swap_submit_req_v14_7)

/* The first-swap table, field by field, independently of the encoder. */
static void reference_present(u8 *r, u32 swap_id, u32 w, u32 h, u32 stride, u64 dva,
			      bool alpha)
{
	u8 *s = r + 0x508;
	u32 bytes = stride * h;

	memset(r, 0, SWAP_SIZE);
	put_unaligned_le32(swap_id, r + 0x98);
	put_unaligned_le32(1, r + 0x9c);
	put_unaligned_le32(w, r + 0xb4);
	put_unaligned_le32(h, r + 0xb8);
	put_unaligned_le32(w, r + 0x114);
	put_unaligned_le32(h, r + 0x118);
	put_unaligned_le32(0x80000007, r + 0x14c);
	put_unaligned_le32(0x80000007, r + 0x150);
	put_unaligned_le32(0xff000000, r + 0x154);
	/* The surface: XRGB8888 is taken as premultiplied, as on the panel. */
	s[0x02] = !alpha;
	put_unaligned_le32(1, s + 0x03);
	put_unaligned_le32(1, s + 0x07);
	put_unaligned_le32(0x42475241, s + 0x0b);
	s[0x13] = 13;
	s[0x14] = 12;
	put_unaligned_le32(stride, s + 0x15);
	put_unaligned_le16(1, s + 0x19);
	s[0x1b] = 1;
	s[0x1c] = 1;
	put_unaligned_le32(w, s + 0x21);
	put_unaligned_le32(h, s + 0x25);
	put_unaligned_le32(bytes, s + 0x29);
	put_unaligned_le32(1, s + 0x35);
	put_unaligned_le64(1, s + 0x51);
	put_unaligned_le32(w, s + 0x59);
	put_unaligned_le32(h, s + 0x5d);
	put_unaligned_le32(stride, s + 0x69);
	put_unaligned_le32(bytes, s + 0x6d);
	put_unaligned_le16(4, s + 0x71);
	s[0x73] = 1;
	s[0x74] = 1;
	put_unaligned_le64(1, s + 0x149);
	put_unaligned_le64(dva, r + 0xdb8);
	memset(r + 0x1b4b, 1, 10);
	r[0x1b4b] = 0;
	r[0x1b56] = 1;
	r[0x1b57] = 1;
}

static void dcpext_present_test(struct kunit *test)
{
	static const struct iomfb_v14_7_frame frames[] = {
		/* A 1080p colour-bar swap, scanned through FE SID0. */
		{ .dva = 0x10030000000ULL, .backing = 0x7ec000, .width = 1920,
		  .height = 1080, .stride = 7680 },
		/* A 3840x1080 ARGB frame with a padded pitch. */
		{ .dva = 0x10ffe000000ULL, .backing = 0x1000000, .width = 3840,
		  .height = 1080, .stride = 15424, .alpha = true },
	};
	struct dcp_swap_submit_req_v14_7 *req;
	u8 *expected;
	unsigned int i;

	req = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	expected = kunit_kzalloc(test, SWAP_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, req);
	KUNIT_ASSERT_NOT_NULL(test, expected);

	for (i = 0; i < ARRAY_SIZE(frames); i++) {
		const struct iomfb_v14_7_frame *f = &frames[i];

		/* The external geometry is checked against the CRTC mode. */
		KUNIT_EXPECT_EQ(test, iomfb_v14_7_frame_check(f, f->width, f->height), 0);
		KUNIT_EXPECT_EQ(test, iomfb_v14_7_frame_check(f, 3024, 1964), -EINVAL);
		iomfb_v14_7_encode_present(req, f);
		req->swap.swap_id = 40 + i;
		KUNIT_EXPECT_EQ(test, iomfb_v14_7_encode_nits(req, false, 0), 0);
		KUNIT_EXPECT_TRUE(test, iomfb_v14_7_single_surface(req));
		reference_present(expected, 40 + i, f->width, f->height, f->stride, f->dva,
				  f->alpha);
		KUNIT_EXPECT_MEMEQ(test, req, expected, SWAP_SIZE);
	}

	/* No plane: the blank background, every surface null. */
	iomfb_v14_7_encode_present(req, NULL);
	KUNIT_EXPECT_EQ(test, req->surf_iova[0], 0);
	KUNIT_EXPECT_EQ(test, req->surf_null[0], 1);
	KUNIT_EXPECT_EQ(test, req->swap.swap_enabled, 0x80000007);
	KUNIT_EXPECT_TRUE(test, iomfb_v14_7_single_surface(req));
}

/* A472: 12 bytes, the state as u64 at +0, its status at +4 of 8 bytes. */
static void dcpext_power_layout_test(struct kunit *test)
{
	struct dcp_set_power_state_req_v14_7 on = { .unklong = 1 };
	u8 bytes[12] = { 1 };

	KUNIT_EXPECT_EQ(test, sizeof(on), 12);
	KUNIT_EXPECT_MEMEQ(test, &on, bytes, sizeof(bytes));
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_set_power_state_resp, ret), 4);
	KUNIT_EXPECT_EQ(test, sizeof(struct dcp_set_power_state_resp), 8);
	KUNIT_EXPECT_EQ(test, sizeof(struct dcp_set_digital_out_mode_req), 8);
	KUNIT_EXPECT_EQ(test, offsetof(struct dcp_set_digital_out_mode_req, timing_mode_id), 4);
}

static struct kunit_case dcpext_present_cases[] = {
	KUNIT_CASE(dcpext_present_test),
	KUNIT_CASE(dcpext_power_layout_test),
	{}
};

static struct kunit_suite dcpext_present_suite = {
	.name = "apple-dcpext-present",
	.test_cases = dcpext_present_cases,
};

/* ---- The AV service's EDID object ---- */

#define EDID_OBJECT	(64 + 48 + 1032)

/* A 256-byte EDID: base block announcing one extension, both summing to 0. */
static void edid_object(u8 *o, unsigned int blocks, unsigned int announced, u32 body)
{
	static const u8 header[] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0 };
	u32 n = 128 * blocks, i, j;
	u8 *edid = o + 120, sum;

	memset(o, 0, EDID_OBJECT);
	put_unaligned_le16(1, o + 2);
	put_unaligned_le32(7, o + 4);
	put_unaligned_le32(body, o + 8);
	put_unaligned_le32(0x69706378, o + 12);
	put_unaligned_le64(1032, o + 64);
	put_unaligned_le64(n + 8, o + 96);
	put_unaligned_le32(0xd3, o + 112);
	put_unaligned_le32(0x8a000000 | n, o + 116);
	memcpy(edid, header, sizeof(header));
	for (i = 8; i < n; i++)
		edid[i] = i * 7;
	edid[126] = announced;
	for (i = 0; i < n; i += 128) {
		for (sum = 0, j = 0; j < 127; j++)
			sum += edid[i + j];
		edid[i + 127] = -sum;
	}
}

static void dcpext_edid_test(struct kunit *test)
{
	const u8 *edid;
	u8 *o;

	o = kunit_kzalloc(test, EDID_OBJECT, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, o);

	/* Full-capacity reply (a captured run returned 1144 bytes). */
	edid_object(o, 2, 1, 1080);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), 256);
	KUNIT_EXPECT_PTR_EQ(test, edid, (const u8 *)o + 120);
	/* The body must cover the EDID it names. */
	edid_object(o, 2, 1, 56 + 256);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), 256);
	edid_object(o, 2, 1, 56 + 255);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	edid_object(o, 2, 1, 1081);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	/* The block count must match the base block. */
	edid_object(o, 2, 2, 1080);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	edid_object(o, 2, 0, 1080);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	/* A bad checksum in the extension. */
	edid_object(o, 2, 1, 1080);
	o[120 + 200] ^= 1;
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	/* Capacity not echoed, a foreign OSSerialize tag, a short used size. */
	edid_object(o, 2, 1, 1080);
	put_unaligned_le64(32776, o + 64);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	edid_object(o, 2, 1, 1080);
	put_unaligned_le32(0x8a000000 | 128, o + 116);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	edid_object(o, 2, 1, 1080);
	put_unaligned_le64(128, o + 96);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	/* Another command's reply. */
	edid_object(o, 1, 0, 1080);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), 128);
	put_unaligned_le32(6, o + 4);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), -EPROTO);
	/* Eight blocks is the maximum. */
	edid_object(o, 8, 7, 1080);
	KUNIT_EXPECT_EQ(test, dcpext_edid_object(o, EDID_OBJECT, &edid), 1024);
}

static struct kunit_case dcpext_edid_cases[] = {
	KUNIT_CASE(dcpext_edid_test),
	{}
};

static struct kunit_suite dcpext_edid_suite = {
	.name = "apple-dcpext-edid",
	.test_cases = dcpext_edid_cases,
};

kunit_test_suites(&dcpext_link_suite, &dcpext_hpd_suite, &dcpext_catalog_suite,
		  &dcpext_present_suite, &dcpext_edid_suite);
