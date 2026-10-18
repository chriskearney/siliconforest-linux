// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * KUnit tests for the Type-C mux decisions of the Apple Type-C PHY driver.
 * This file is included from atc.c so that it can test static functions.
 */

#include <kunit/device.h>
#include <kunit/test.h>

static struct apple_atcphy *atcphy_test_alloc(struct kunit *test, bool fixed_usb2)
{
	struct apple_atcphy *atcphy;

	atcphy = kunit_kzalloc(test, sizeof(*atcphy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, atcphy);
	atcphy->fixed_usb2 = fixed_usb2;
	atcphy->mode = APPLE_ATCPHY_MODE_USB3_DP;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->host_active = fixed_usb2;

	return atcphy;
}

/* The mode lookup reports unsupported states against the PHY device */
static struct apple_atcphy *atcphy_test_alloc_dev(struct kunit *test, bool fixed_usb2)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, fixed_usb2);

	atcphy->dev = kunit_device_register(test, "atcphy-test");
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, atcphy->dev);

	return atcphy;
}

static void atcphy_test_target_mode(struct kunit *test)
{
	struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
	struct typec_mux_state state = {};
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);

	state.mode = TYPEC_STATE_SAFE;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_OFF);

	state.mode = TYPEC_STATE_USB;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB3);

	state.alt = &dp;
	state.mode = TYPEC_DP_STATE_C;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_DP);
	state.mode = TYPEC_DP_STATE_D;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state),
			APPLE_ATCPHY_MODE_USB3_DP);
	state.mode = TYPEC_DP_STATE_E;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_DP);
	state.mode = TYPEC_DP_STATE_A;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_OFF);
}

/* A port behind a fixed hub keeps USB2 for its hub whenever a mode turns the PHY off */
static void atcphy_test_target_mode_fixed_hub(struct kunit *test)
{
	struct typec_altmode dp = { .svid = USB_TYPEC_DP_SID };
	struct typec_mux_state state = {};
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, true);

	state.mode = TYPEC_STATE_SAFE;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB2);

	state.alt = &dp;
	state.mode = TYPEC_DP_STATE_A;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state), APPLE_ATCPHY_MODE_USB2);
	state.mode = TYPEC_DP_STATE_D;
	KUNIT_EXPECT_EQ(test, atcphy_typec_target_mode(atcphy, &state),
			APPLE_ATCPHY_MODE_USB3_DP);
}

static void atcphy_test_noop_same_mode(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	/* A DisplayPort HPD or status update keeps the pin assignment */
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* The same holds while the controller does not use the USB3 PHY */
	atcphy->host_active = false;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* Ports without a fixed hub never route the PIPE from the mux */
	atcphy = atcphy_test_alloc(test, false);
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_TRUE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
}

static void atcphy_test_noop_needs_work(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc(test, true);

	/* A new pin assignment or the safe state rebuild the lanes */
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_DP));
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB2));

	/* So does a failed transition or an orientation change */
	atcphy->mode_dirty = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
	atcphy->mode_dirty = false;

	/* A running controller is still owed its USB3 backend */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));

	/* or a handoff that has not completed */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_FALSE(test, atcphy_mux_is_noop(atcphy, APPLE_ATCPHY_MODE_USB3_DP));
}

/*
 * A J700-style PHY behind a fixed hub, in DisplayPort + USB3 with the PIPE on
 * USB3 for a running host, but without a device tree node. A mux update that
 * goes for the USB3 root hub then fails with -ENODEV at the controller lookup,
 * before any register access, which tells the two paths apart.
 */
static struct apple_atcphy *atcphy_test_alloc_fixed_hub(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, true);

	atcphy->hw = &atcphy_hw_t8140;
	mutex_init(&atcphy->lock);

	return atcphy;
}

static const struct typec_altmode atcphy_test_dp = { .svid = USB_TYPEC_DP_SID };

static int atcphy_test_update_dp(struct apple_atcphy *atcphy, unsigned long pin)
{
	struct typec_mux_state state = {
		.alt = (struct typec_altmode *)&atcphy_test_dp,
		.mode = pin,
	};

	return atcphy_mux_update(atcphy, &state);
}

static void atcphy_test_update_skips_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	/* An HPD or status update with the same pin assignment */
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);
	KUNIT_EXPECT_FALSE(test, atcphy->mode_dirty);

	/* The orientation the lanes already use */
	KUNIT_EXPECT_EQ(test, atcphy_set_orientation(atcphy, TYPEC_ORIENTATION_NORMAL), 0);
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), 0);
}

static void atcphy_test_update_takes_usb(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_fixed_hub(test);

	/* A new pin assignment */
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_C), -ENODEV);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);

	/* A PIPE handoff that has not completed */
	atcphy->pipe_pending = true;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
	atcphy->pipe_pending = false;

	/* A running host whose PIPE is not on the USB3 backend */
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;

	/* A cable flip moves the USB3 and DisplayPort lanes */
	KUNIT_EXPECT_EQ(test, atcphy_set_orientation(atcphy, TYPEC_ORIENTATION_REVERSE), 0);
	KUNIT_EXPECT_TRUE(test, atcphy->mode_dirty);
	KUNIT_EXPECT_EQ(test, atcphy_test_update_dp(atcphy, TYPEC_DP_STATE_D), -ENODEV);
}

/* USB shutdown can turn the lanes off before the SAFE mux request arrives. */
static struct apple_atcphy *atcphy_test_alloc_powered_off(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);

	atcphy->hw = &atcphy_hw_t8103;
	atcphy->mode = APPLE_ATCPHY_MODE_OFF;
	atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;
	atcphy->typec_mode = APPLE_ATCPHY_MODE_USB3;
	mutex_init(&atcphy->lock);

	return atcphy;
}

static void atcphy_test_safe_after_poweroff(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_powered_off(test);
	struct typec_mux_state safe = { .mode = TYPEC_STATE_SAFE };

	KUNIT_EXPECT_EQ(test, atcphy_mux_update(atcphy, &safe), 0);
	KUNIT_EXPECT_EQ(test, atcphy->typec_mode, APPLE_ATCPHY_MODE_OFF);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_DUMMY);
}

/* The locked recheck must also record SAFE if shutdown raced the mode lookup. */
static void atcphy_test_locked_safe_after_poweroff(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_powered_off(test);

	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_OFF), 0);
	KUNIT_EXPECT_EQ(test, atcphy->typec_mode, APPLE_ATCPHY_MODE_OFF);
}

struct atcphy_test_configure {
	unsigned int calls;
	unsigned int lane_requests;
};

/* Count lane requests instead of touching MMIO, including stale USB3 requests. */
static int atcphy_test_configure_stub(struct apple_atcphy *atcphy, enum atcphy_mode mode)
{
	struct kunit *test = kunit_get_current_test();
	struct atcphy_test_configure *config = test->priv;

	lockdep_assert_held(&atcphy->lock);
	config->calls++;
	if (mode != APPLE_ATCPHY_MODE_OFF) {
		config->lane_requests++;
		return -EINVAL;
	}
	return 0;
}

/* Rebinding after a failed role shutdown must not touch a live PIPE. */
static void atcphy_test_safe_live_pipe(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_dev(test, false);
	struct atcphy_test_configure config = {};

	test->priv = &config;
	kunit_activate_static_stub(test, atcphy_configure, atcphy_test_configure_stub);
	atcphy->hw = &atcphy_hw_t8103;
	atcphy->typec_mode = APPLE_ATCPHY_MODE_USB3;
	mutex_init(&atcphy->lock);
	KUNIT_EXPECT_EQ(test, atcphy_mux_set_locked(atcphy, APPLE_ATCPHY_MODE_OFF), -EBUSY);
	KUNIT_EXPECT_EQ(test, config.calls, 0U);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_USB3_DP);
	KUNIT_EXPECT_EQ(test, atcphy->typec_mode, APPLE_ATCPHY_MODE_USB3);
	KUNIT_EXPECT_EQ(test, atcphy->pipe_state, ATCPHY_PIPEHANDLER_STATE_USB3);
}

static void atcphy_test_usb2_init_after_safe(struct kunit *test)
{
	struct apple_atcphy *atcphy = atcphy_test_alloc_powered_off(test);
	struct typec_mux_state safe = { .mode = TYPEC_STATE_SAFE };
	struct atcphy_test_configure config = {};
	struct phy *phy;

	phy = kunit_kzalloc(test, sizeof(*phy), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, phy);
	phy_set_drvdata(phy, atcphy);
	test->priv = &config;
	kunit_activate_static_stub(test, atcphy_configure, atcphy_test_configure_stub);

	KUNIT_ASSERT_EQ(test, atcphy_mux_update(atcphy, &safe), 0);
	KUNIT_EXPECT_EQ(test, atcphy_usb2_init(phy), 0);
	KUNIT_EXPECT_EQ(test, config.calls, 1U);
	KUNIT_EXPECT_EQ(test, config.lane_requests, 0U);
	KUNIT_EXPECT_EQ(test, atcphy->mode, APPLE_ATCPHY_MODE_OFF);
}

static struct kunit_case atcphy_mux_test_cases[] = {
	KUNIT_CASE(atcphy_test_target_mode),
	KUNIT_CASE(atcphy_test_target_mode_fixed_hub),
	KUNIT_CASE(atcphy_test_noop_same_mode),
	KUNIT_CASE(atcphy_test_noop_needs_work),
	KUNIT_CASE(atcphy_test_update_skips_usb),
	KUNIT_CASE(atcphy_test_update_takes_usb),
	KUNIT_CASE(atcphy_test_safe_after_poweroff),
	KUNIT_CASE(atcphy_test_locked_safe_after_poweroff),
	KUNIT_CASE(atcphy_test_safe_live_pipe),
	KUNIT_CASE(atcphy_test_usb2_init_after_safe),
	{}
};

static struct kunit_suite atcphy_mux_test_suite = {
	.name = "phy-apple-atc-mux",
	.test_cases = atcphy_mux_test_cases,
};

kunit_test_suite(atcphy_mux_test_suite);
