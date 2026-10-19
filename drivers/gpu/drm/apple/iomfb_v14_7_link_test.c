// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The link's record of dropped swaps, included at the end of
 * iomfb_v14_7_link.c so that the production helpers run: a D589 for a
 * swap the firmware reported dropped completes nothing, including after
 * later drops.
 */
#include <kunit/test.h>

static struct dcp_v14_link *link_drops_setup(struct kunit *test)
{
	struct dcp_v14_link *link = kunit_kzalloc(test, sizeof(*link), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, link);
	dcp_v14_link_init(link, NULL, NULL);
	return link;
}

/* A dropped swap's D589 is accounted for once; nothing matches id 0. */
static void link_drops_one(struct kunit *test)
{
	struct dcp_v14_link *link = link_drops_setup(test);

	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 0));
	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 7));
	link_drop_note(link, 7);
	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 8));
	KUNIT_EXPECT_TRUE(test, link_drop_take(link, 7));
	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 7));
	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 0));
}

/* Two drops, then the first one's D589: still a dropped swap's. */
static void link_drops_two_then_late_first(struct kunit *test)
{
	struct dcp_v14_link *link = link_drops_setup(test);

	link_drop_note(link, 7);
	link_drop_note(link, 8);
	KUNIT_EXPECT_TRUE(test, link_drop_take(link, 7));
	KUNIT_EXPECT_TRUE(test, link_drop_take(link, 8));
	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 7));
}

/* Only the last DCP_V14_LINK_DROPPED drops are remembered. */
static void link_drops_keeps_the_last(struct kunit *test)
{
	struct dcp_v14_link *link = link_drops_setup(test);
	u32 id;

	for (id = 1; id <= DCP_V14_LINK_DROPPED + 1; id++)
		link_drop_note(link, id);
	KUNIT_EXPECT_FALSE(test, link_drop_take(link, 1));
	for (id = 2; id <= DCP_V14_LINK_DROPPED + 1; id++)
		KUNIT_EXPECT_TRUE(test, link_drop_take(link, id));
}

static struct kunit_case link_drops_cases[] = {
	KUNIT_CASE(link_drops_one),
	KUNIT_CASE(link_drops_two_then_late_first),
	KUNIT_CASE(link_drops_keeps_the_last),
	{}
};

static struct kunit_suite link_drops_suite = {
	.name = "dcp_v14_link_drops",
	.test_cases = link_drops_cases,
};

kunit_test_suite(link_drops_suite);
