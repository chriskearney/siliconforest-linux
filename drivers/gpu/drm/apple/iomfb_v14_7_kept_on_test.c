// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * An external display kept powered for its inactive sink, included at the
 * end of iomfb_v14_7.c so that the production iomfb_v14_7_poweroff(),
 * iomfb_v14_7_external_sink_released() and dcp_v14_kept_on_work() run. Only
 * the firmware's power call is stubbed; the work runs on the test's thread,
 * where the stub applies, and the queued one only counts.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct v14_kept {
	struct apple_dcp *dcp;
	struct apple_dcp_v14 *v14;
	unsigned int offs, ons, queued;
};

static struct v14_kept *v14_kept_current;

static int v14_kept_power_call(struct apple_dcp_v14 *v14, bool on)
{
	if (on)
		v14_kept_current->ons++;
	else
		v14_kept_current->offs++;
	return 0;
}

static void v14_kept_queued(struct work_struct *work)
{
	v14_kept_current->queued++;
}

static void v14_kept_stop(void *data)
{
	struct apple_dcp_v14 *v14 = data;

	cancel_work_sync(&v14->kept_on_work);
}

/* A powered native external display, its sink @inactive. */
static struct v14_kept *v14_kept_setup(struct kunit *test, bool inactive)
{
	struct v14_kept *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, s);
	dev = kunit_device_register(test, "dcp-v14-kept");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	s->dcp = devm_kzalloc(dev, sizeof(*s->dcp), GFP_KERNEL);
	s->v14 = devm_kzalloc(dev, sizeof(*s->v14), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s->dcp);
	KUNIT_ASSERT_NOT_NULL(test, s->v14);
	s->dcp->dev = dev;
	s->dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	s->dcp->external = true;
	s->dcp->external_native = true;
	s->dcp->dptxport[0].sink_inactive = inactive;
	s->dcp->external_phase = DCPEXT_RUNNING;
	spin_lock_init(&s->dcp->mode_state.lock);
	s->dcp->v14 = s->v14;
	s->v14->dev = dev;
	s->v14->dcp = s->dcp;
	s->v14->external = true;
	s->v14->started = true;
	s->v14->opened = true;
	s->v14->powered = true;
	mutex_init(&s->v14->lock);
	INIT_WORK(&s->v14->kept_on_work, v14_kept_queued);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, v14_kept_stop, s->v14), 0);
	v14_kept_current = s;
	kunit_activate_static_stub(test, dcp_v14_power_call, v14_kept_power_call);
	return s;
}

/*
 * The release (dcp_dptx_release_locked()): the sink is no longer inactive.
 * Returns whether the work was queued; it has run either way, as it would.
 */
static bool v14_kept_release(struct v14_kept *s)
{
	unsigned int queued;

	WRITE_ONCE(s->dcp->dptxport[0].sink_inactive, false);
	iomfb_v14_7_external_sink_released(s->dcp);
	flush_work(&s->v14->kept_on_work);
	queued = s->queued;
	dcp_v14_kept_on_work(&s->v14->kept_on_work);
	return queued;
}

/* DPMS off keeps an inactive sink's display on; its release powers it down. */
static void v14_kept_released_powers_down(struct kunit *test)
{
	struct v14_kept *s = v14_kept_setup(test, true);

	iomfb_v14_7_poweroff(s->dcp);
	KUNIT_EXPECT_EQ(test, s->offs, 0U);
	KUNIT_EXPECT_TRUE(test, s->v14->powered);
	KUNIT_EXPECT_TRUE(test, s->v14->kept_on);
	/* Sleep is refused while it is on. */
	KUNIT_EXPECT_NOT_NULL(test, iomfb_v14_7_external_busy(s->dcp));
	KUNIT_EXPECT_TRUE(test, v14_kept_release(s));
	KUNIT_EXPECT_EQ(test, s->offs, 1U);
	KUNIT_EXPECT_FALSE(test, s->v14->powered);
	KUNIT_EXPECT_FALSE(test, s->v14->kept_on);
	/* And no longer once it is down, nothing attached. */
	KUNIT_EXPECT_NULL(test, iomfb_v14_7_external_busy(s->dcp));
}

/* An active sink's display powers down at DPMS off; its release has nothing to do. */
static void v14_kept_active_sink(struct kunit *test)
{
	struct v14_kept *s = v14_kept_setup(test, false);

	iomfb_v14_7_poweroff(s->dcp);
	KUNIT_EXPECT_EQ(test, s->offs, 1U);
	KUNIT_EXPECT_FALSE(test, s->v14->kept_on);
	KUNIT_EXPECT_FALSE(test, v14_kept_release(s));
	KUNIT_EXPECT_EQ(test, s->offs, 1U);
}

/* A power-on since (the sink came back, a mode set): the release leaves it on. */
static void v14_kept_power_on_clears(struct kunit *test)
{
	struct v14_kept *s = v14_kept_setup(test, true);

	iomfb_v14_7_poweroff(s->dcp);
	KUNIT_ASSERT_TRUE(test, s->v14->kept_on);
	mutex_lock(&s->v14->lock);
	KUNIT_EXPECT_EQ(test, dcp_v14_set_power(s->v14, true), 0);
	mutex_unlock(&s->v14->lock);
	KUNIT_EXPECT_FALSE(test, s->v14->kept_on);
	KUNIT_EXPECT_FALSE(test, v14_kept_release(s));
	KUNIT_EXPECT_EQ(test, s->offs, 0U);
	KUNIT_EXPECT_TRUE(test, s->v14->powered);
}

static struct kunit_case v14_kept_cases[] = {
	KUNIT_CASE(v14_kept_released_powers_down),
	KUNIT_CASE(v14_kept_active_sink),
	KUNIT_CASE(v14_kept_power_on_clears),
	{}
};

static struct kunit_suite v14_kept_suite = {
	.name = "dcp_v14_external_kept_on",
	.test_cases = v14_kept_cases,
};

kunit_test_suite(v14_kept_suite);
