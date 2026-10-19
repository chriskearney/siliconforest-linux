// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The 14.7 panel's mailbox through system sleep, included at the end of
 * iomfb_v14_7.c so that the production functions run: iomfb_v14_7_hold_rx()
 * and iomfb_v14_7_release_rx(), the notification pump dcp_v14_idle() and the
 * call funnel. Only the two RTKit calls are stubbed; a callback is queued on
 * the link as its receive path queues one.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct v14_sleep {
	struct apple_dcp *dcp;
	struct apple_dcp_v14 *v14;
	unsigned int holds, resumes, pumps;
};

static struct v14_sleep *v14_sleep_current;

static void v14_sleep_rx_hold(struct apple_dcp_v14 *v14)
{
	v14_sleep_current->holds++;
}

static void v14_sleep_rx_resume(struct apple_dcp_v14 *v14)
{
	v14_sleep_current->resumes++;
}

/* Stands in for the idle work: counts the kicks, consumes nothing. */
static void v14_sleep_pump_kicked(struct work_struct *work)
{
	v14_sleep_current->pumps++;
}

static void v14_sleep_stop(void *data)
{
	struct apple_dcp_v14 *v14 = data;

	cancel_work_sync(&v14->idle_work);
	/* Frees the events a case left queued. */
	dcp_v14_link_fail(&v14->link);
}

/* A started T6030 panel session, with no DCP behind it. */
static struct v14_sleep *v14_sleep_setup(struct kunit *test)
{
	struct v14_sleep *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, s);
	dev = kunit_device_register(test, "dcp-v14-sleep");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	/* Freed with the device, after its devres. */
	s->dcp = devm_kzalloc(dev, sizeof(*s->dcp), GFP_KERNEL);
	s->v14 = devm_kzalloc(dev, sizeof(*s->v14), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s->dcp);
	KUNIT_ASSERT_NOT_NULL(test, s->v14);
	s->dcp->dev = dev;
	s->dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	s->dcp->v14 = s->v14;
	s->v14->dev = dev;
	s->v14->dcp = s->dcp;
	s->v14->board = &dcp_v14_board_t6030;
	s->v14->started = true;
	/* Never dereferenced: the RTKit calls are stubbed. */
	s->v14->rtk = (struct apple_rtkit *)s;
	mutex_init(&s->v14->lock);
	dcp_v14_link_init(&s->v14->link, dev, NULL);
	INIT_WORK(&s->v14->idle_work, v14_sleep_pump_kicked);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, v14_sleep_stop, s->v14), 0);
	v14_sleep_current = s;
	kunit_activate_static_stub(test, dcp_v14_rx_hold, v14_sleep_rx_hold);
	kunit_activate_static_stub(test, dcp_v14_rx_resume, v14_sleep_rx_resume);
	return s;
}

/* A D577 notification (4 bytes in, none out), queued as received. */
static void v14_sleep_queue_callback(struct v14_sleep *s, u64 message)
{
	struct {
		struct apple_dcp_link_rpc_header h;
		u8 in[4];
	} __packed cb = {};

	apple_dcp_link_rpc_header_encode(&cb.h, D(577), sizeof(cb.in), 0);
	dcp_v14_link_test_callback(&s->v14->link, message, &cb, sizeof(cb));
}

/* A callback queued at the hold stays queued while held: nothing answers it. */
static void v14_sleep_queued_callback_kept(struct kunit *test)
{
	struct v14_sleep *s = v14_sleep_setup(test);

	v14_sleep_queue_callback(s, 0x1234);
	KUNIT_ASSERT_EQ(test, s->v14->link.event_count, 1U);
	iomfb_v14_7_hold_rx(s->dcp);
	KUNIT_EXPECT_TRUE(test, s->v14->rx_held);
	KUNIT_EXPECT_EQ(test, s->holds, 1U);
	/* The production pump, as the work would run it during sleep. */
	dcp_v14_idle(&s->v14->idle_work);
	KUNIT_EXPECT_EQ(test, s->v14->link.event_count, 1U);
	KUNIT_EXPECT_FALSE(test, s->v14->failed);
	/* A call from anywhere is refused without stopping the session. */
	mutex_lock(&s->v14->lock);
	KUNIT_EXPECT_EQ(test, dcp_v14_call(s->v14, A(406), NULL, 0, NULL, 0, 0), -EAGAIN);
	mutex_unlock(&s->v14->lock);
	KUNIT_EXPECT_FALSE(test, s->v14->failed);
}

/* Resume delivers the held messages and runs the pump for what was queued. */
static void v14_sleep_release_kicks_pump(struct kunit *test)
{
	struct v14_sleep *s = v14_sleep_setup(test);

	v14_sleep_queue_callback(s, 0x1234);
	iomfb_v14_7_hold_rx(s->dcp);
	iomfb_v14_7_release_rx(s->dcp);
	flush_work(&s->v14->idle_work);
	KUNIT_EXPECT_FALSE(test, s->v14->rx_held);
	KUNIT_EXPECT_EQ(test, s->resumes, 1U);
	KUNIT_EXPECT_EQ(test, s->pumps, 1U);
	/* Still queued for that pump: the hold consumed nothing. */
	KUNIT_EXPECT_EQ(test, s->v14->link.event_count, 1U);
}

/*
 * An aborted suspend: resume follows the hold at once, or comes with no hold
 * (suspend returned before its last step). Either way nothing is lost and a
 * release without a hold does nothing.
 */
static void v14_sleep_abort_and_unheld_release(struct kunit *test)
{
	struct v14_sleep *s = v14_sleep_setup(test);

	iomfb_v14_7_release_rx(s->dcp);
	KUNIT_EXPECT_EQ(test, s->resumes, 0U);
	KUNIT_EXPECT_EQ(test, s->pumps, 0U);

	iomfb_v14_7_hold_rx(s->dcp);
	iomfb_v14_7_hold_rx(s->dcp);
	KUNIT_EXPECT_EQ(test, s->holds, 1U);
	iomfb_v14_7_release_rx(s->dcp);
	iomfb_v14_7_release_rx(s->dcp);
	flush_work(&s->v14->idle_work);
	KUNIT_EXPECT_EQ(test, s->resumes, 1U);
	KUNIT_EXPECT_EQ(test, s->pumps, 1U);
	KUNIT_EXPECT_FALSE(test, s->v14->rx_held);
}

/* No hold for a failed or unstarted session, nor the J613 pipe on Type-C. */
static void v14_sleep_hold_scope(struct kunit *test)
{
	struct v14_sleep *s = v14_sleep_setup(test);

	s->v14->failed = true;
	iomfb_v14_7_hold_rx(s->dcp);
	s->v14->failed = false;
	s->v14->started = false;
	iomfb_v14_7_hold_rx(s->dcp);
	s->v14->started = true;
	WRITE_ONCE(s->v14->typec, true);
	iomfb_v14_7_hold_rx(s->dcp);
	WRITE_ONCE(s->v14->typec, false);
	s->dcp->external = true;
	iomfb_v14_7_hold_rx(s->dcp);
	KUNIT_EXPECT_EQ(test, s->holds, 0U);
	KUNIT_EXPECT_FALSE(test, s->v14->rx_held);
}

static struct kunit_case v14_sleep_cases[] = {
	KUNIT_CASE(v14_sleep_queued_callback_kept),
	KUNIT_CASE(v14_sleep_release_kicks_pump),
	KUNIT_CASE(v14_sleep_abort_and_unheld_release),
	KUNIT_CASE(v14_sleep_hold_scope),
	{}
};

static struct kunit_suite v14_sleep_suite = {
	.name = "dcp_v14_sleep",
	.test_cases = v14_sleep_cases,
};

kunit_test_suite(v14_sleep_suite);
