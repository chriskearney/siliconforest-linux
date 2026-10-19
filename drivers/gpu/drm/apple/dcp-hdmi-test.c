// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * The HDMI converter service poll, included at the end of dcp-hdmi.c so that
 * the production worker runs: a failed request is asked again at the next
 * period, three refused in a row stop the poll until the next sink, and a
 * delivered request resets the count. Only the request itself is replaced
 * (a static stub), so no DCP is needed.
 */
#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

struct hdmi_test {
	struct apple_dcp *dcp;
	struct dcp_hdmi *hdmi;
	/* Results the stubbed request returns, in order, and how many it gave. */
	const int *results;
	unsigned int count, calls;
};

static struct hdmi_test *hdmi_test_current;

static int hdmi_test_sink_irq(struct apple_dcp *dcp)
{
	struct hdmi_test *t = hdmi_test_current;

	return t->calls < t->count ? t->results[t->calls++] : 0;
}

static void hdmi_test_stop(void *data)
{
	struct dcp_hdmi *hdmi = data;

	cancel_delayed_work_sync(&hdmi->work);
}

static struct hdmi_test *hdmi_test_setup(struct kunit *test)
{
	struct hdmi_test *t = kunit_kzalloc(test, sizeof(*t), GFP_KERNEL);
	struct device *dev;

	KUNIT_ASSERT_NOT_NULL(test, t);
	dev = kunit_device_register(test, "dcp-hdmi-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	t->dcp = kunit_kzalloc(test, sizeof(*t->dcp), GFP_KERNEL);
	t->hdmi = kunit_kzalloc(test, sizeof(*t->hdmi), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, t->dcp);
	KUNIT_ASSERT_NOT_NULL(test, t->hdmi);
	t->dcp->dev = dev;
	t->dcp->dptxport[0].enabled = true;
	t->dcp->dptxport[0].connected = true;
	t->hdmi->dcp = t->dcp;
	t->dcp->hdmi = t->hdmi;
	INIT_DELAYED_WORK(&t->hdmi->work, dcp_hdmi_work);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, hdmi_test_stop, t->hdmi), 0);
	/* The firmware holds the sink: the poll is armed. */
	t->hdmi->armed = true;
	hdmi_test_current = t;
	kunit_activate_static_stub(test, dcp_hdmi_sink_irq, hdmi_test_sink_irq);
	return t;
}

/* One run of the production worker; returns whether it queued the next poll. */
static bool hdmi_test_run(struct hdmi_test *t)
{
	bool queued;

	dcp_hdmi_work(&t->hdmi->work.work);
	queued = delayed_work_pending(&t->hdmi->work);
	cancel_delayed_work_sync(&t->hdmi->work);
	return queued;
}

static void hdmi_service_failure_is_retried(struct kunit *test)
{
	static const int results[] = { -EIO, 0 };
	struct hdmi_test *t = hdmi_test_setup(test);

	t->results = results;
	t->count = ARRAY_SIZE(results);
	/* A transient failure: still armed, and the next poll is queued. */
	KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
	KUNIT_EXPECT_EQ(test, t->hdmi->refused, 1U);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->hdmi->armed));
	/* The retry is delivered: the count starts again. */
	KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
	KUNIT_EXPECT_EQ(test, t->hdmi->refused, 0U);
	KUNIT_EXPECT_EQ(test, t->calls, 2U);
}

static void hdmi_service_stops_after_three_refusals(struct kunit *test)
{
	static const int results[] = { -EIO, -EIO, -EIO, 0 };
	struct hdmi_test *t = hdmi_test_setup(test);

	t->results = results;
	t->count = ARRAY_SIZE(results);
	KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
	KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
	/* The third refusal in a row: disarmed, nothing queued. */
	KUNIT_EXPECT_FALSE(test, hdmi_test_run(t));
	KUNIT_EXPECT_FALSE(test, READ_ONCE(t->hdmi->armed));
	/* A later run (e.g. a stray pulse-less wakeup) sends nothing. */
	KUNIT_EXPECT_FALSE(test, hdmi_test_run(t));
	KUNIT_EXPECT_EQ(test, t->calls, 3U);
	/* The next sink arms the poll again, with a fresh count. */
	dcp_hdmi_sink_held(t->dcp, false);
	dcp_hdmi_sink_held(t->dcp, true);
	cancel_delayed_work_sync(&t->hdmi->work);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->hdmi->armed));
	KUNIT_EXPECT_EQ(test, t->hdmi->refused, 0U);
	KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
}

static void hdmi_service_refusals_must_be_in_a_row(struct kunit *test)
{
	static const int results[] = { -EIO, -EIO, 0, -EIO, -EIO };
	struct hdmi_test *t = hdmi_test_setup(test);
	unsigned int i;

	t->results = results;
	t->count = ARRAY_SIZE(results);
	for (i = 0; i < ARRAY_SIZE(results); i++)
		KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
	KUNIT_EXPECT_TRUE(test, READ_ONCE(t->hdmi->armed));
	KUNIT_EXPECT_EQ(test, t->hdmi->refused, 2U);
}

/* No link yet: no request, but the poll goes on. */
static void hdmi_service_waits_for_the_link(struct kunit *test)
{
	struct hdmi_test *t = hdmi_test_setup(test);

	t->dcp->dptxport[0].connected = false;
	KUNIT_EXPECT_TRUE(test, hdmi_test_run(t));
	KUNIT_EXPECT_EQ(test, t->calls, 0U);
	KUNIT_EXPECT_EQ(test, t->hdmi->refused, 0U);
}

static struct kunit_case hdmi_service_cases[] = {
	KUNIT_CASE(hdmi_service_failure_is_retried),
	KUNIT_CASE(hdmi_service_stops_after_three_refusals),
	KUNIT_CASE(hdmi_service_refusals_must_be_in_a_row),
	KUNIT_CASE(hdmi_service_waits_for_the_link),
	{}
};

static struct kunit_suite hdmi_service_suite = {
	.name = "dcp_hdmi_service",
	.test_cases = hdmi_service_cases,
};

kunit_test_suite(hdmi_service_suite);
