// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>
#include <linux/module.h>

#include "cd321x-pm.h"

static void cd321x_resume_requires_snapshot_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	/* A queued cable update may not apply the pre-sleep snapshot. */
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	KUNIT_EXPECT_TRUE(test, cd321x_pm_can_update(&pm));
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_RUNNING);
}

static void cd321x_revalidation_errors_bounded_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
		KUNIT_EXPECT_EQ(test, cd321x_pm_retry(&pm, true),
				i + 1 < CD321X_RESUME_ATTEMPTS);
	}
	/* Give up without permitting an update from the old cable snapshot. */
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
}

static void cd321x_activation_failure_rereads_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	/* A PHY or ACIO error must not be treated as successful restoration. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_TRUE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_EQ(test, pm.attempts_left, 0U);
}

static void cd321x_abort_and_duplicate_completion_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	/* Prepare followed by completion, with no device suspend, is an abort. */
	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_resume(&pm));
	KUNIT_EXPECT_EQ(test, pm.attempts_left, CD321X_RESUME_ATTEMPTS - 1U);
	cd321x_pm_snapshot_ready(&pm);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_TRUE(test, cd321x_pm_can_update(&pm));
}

static void cd321x_repeat_suspend_cancels_retry_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	/* The next prepare wins over a delayed retry from the previous wake. */
	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_retry(&pm, true));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_EXPECT_EQ(test, pm.attempts_left, (unsigned int)CD321X_RESUME_ATTEMPTS);
}

static void cd321x_suspend_before_apply_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	/* The debounce timer has not applied the freshly read state yet. */
	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_retry(&pm, true));
}

static void cd321x_remove_drains_resume_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	cd321x_pm_remove(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_retry(&pm, true));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_resume(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_REMOVED);
}

static void cd321x_read_and_apply_share_budget_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		if (i & 1)
			cd321x_pm_snapshot_ready(&pm);
		KUNIT_EXPECT_EQ(test, cd321x_pm_retry(&pm, true),
				i + 1 < CD321X_RESUME_ATTEMPTS);
	}
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
}

static void cd321x_status_event_after_exhaustion_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		cd321x_pm_retry(&pm, true);
	}
	/* A power/plug-only interrupt has not read the data or cable registers. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_event(&pm));
	KUNIT_EXPECT_EQ(test, pm.attempts_left, (unsigned int)CD321X_RESUME_ATTEMPTS);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	KUNIT_EXPECT_TRUE(test, cd321x_pm_can_update(&pm));
	cd321x_pm_remove(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_event(&pm));
}

static void cd321x_runtime_failure_revalidates_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	/* A failed ordinary cable update has already consumed its IRQ changes. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		cd321x_pm_snapshot_ready(&pm);
		KUNIT_EXPECT_EQ(test, cd321x_pm_retry(&pm, true),
				i + 1 < CD321X_RESUME_ATTEMPTS);
	}
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_STALE);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	/* A later physical event can retry, but cannot directly apply stale data. */
	KUNIT_EXPECT_TRUE(test, cd321x_pm_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
}

static void cd321x_healthy_resume_keeps_connection_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	/* A successful resume and an aborted suspend both revalidate in place. */
	for (i = 0; i < 2; i++) {
		cd321x_pm_prepare(&pm);
		KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		cd321x_pm_snapshot_ready(&pm);
		KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
		cd321x_pm_complete(&pm);
		KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
	}
}

static void cd321x_failed_resume_forces_reconnect_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	/* Fresh cable data cannot override a provider's recorded resume failure. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
	/* A new suspend must preserve recovery that has not been applied yet. */
	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
}

static void cd321x_stale_recovery_keeps_reconnect_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		cd321x_pm_retry(&pm, true);
	}
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_STALE);
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_event(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
}

static void cd321x_late_link_failure_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	cd321x_pm_complete(&pm);
	/* The link drops after the final successful resume health check. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	/* Only the provider's failed check requests a fresh forced reconnect. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_EQ(test, pm.link_events_left, CD321X_LINK_EVENTS - 1U);
}

static void cd321x_link_event_during_final_apply_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_prepare(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		if (i + 1 < CD321X_RESUME_ATTEMPTS)
			KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	}
	cd321x_pm_snapshot_ready(&pm);
	KUNIT_ASSERT_EQ(test, pm.attempts_left, 0U);
	/* A queued APPLY must not hide an event with its exhausted read budget. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
}

static void cd321x_link_reprobe_storm_bounded_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int event, attempt;

	cd321x_pm_new_connection(&pm);
	for (event = 0; event < CD321X_LINK_EVENTS; event++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
		for (attempt = 0; attempt < CD321X_RESUME_ATTEMPTS; attempt++) {
			KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
			cd321x_pm_snapshot_ready(&pm);
			KUNIT_EXPECT_EQ(test, cd321x_pm_retry(&pm, true),
					attempt + 1 < CD321X_RESUME_ATTEMPTS);
		}
	}
	KUNIT_EXPECT_FALSE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_STALE);
}

static void cd321x_success_does_not_refill_link_budget_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_new_connection(&pm);
	for (i = 0; i < CD321X_LINK_EVENTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		cd321x_pm_snapshot_ready(&pm);
		/* An old host event can reach a now healthy session: keep it. */
		KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
		cd321x_pm_complete(&pm);
	}
	KUNIT_EXPECT_FALSE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_RUNNING);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
}

static void cd321x_link_event_init_and_remove_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_init(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_ready(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_ready(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_remove(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_ready(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
}

static void cd321x_link_event_suspend_and_rearm_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_new_connection(&pm);
	for (i = 0; i < CD321X_LINK_EVENTS; i++)
		KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_link_event(&pm));
	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_link_event(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_EXPECT_EQ(test, pm.link_events_left, (unsigned int)CD321X_LINK_EVENTS);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
	/* An actual attachment change also permits a bounded new sequence. */
	cd321x_pm_new_connection(&pm);
	KUNIT_EXPECT_EQ(test, pm.link_events_left, (unsigned int)CD321X_LINK_EVENTS);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
}

static void cd321x_busy_provider_preserves_connection_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_new_connection(&pm);
	/* PCIe activation runs asynchronously after the Type-C switch returns. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, false));
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_RUNNING);
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
}

static void cd321x_busy_provider_completion_reopens_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i;

	cd321x_pm_new_connection(&pm);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, false));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		cd321x_pm_snapshot_ready(&pm);
		cd321x_pm_retry(&pm, false);
	}
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_STALE);
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
	/* The PCIe worker's completion notification permits a fresh check. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_link_event(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	cd321x_pm_complete(&pm);
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_RUNNING);
	KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
}

static void cd321x_busy_provider_keeps_real_failure_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	cd321x_pm_snapshot_ready(&pm);
	/* Busy retries must not erase recovery requested by a real failure. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, false));
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
}

static void cd321x_lost_event_rereads_test(struct kunit *test)
{
	static const enum cd321x_pm_phase phases[] = {
		CD321X_PM_RUNNING, CD321X_PM_REVALIDATE,
		CD321X_PM_APPLY, CD321X_PM_STALE,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(phases); i++) {
		struct cd321x_pm_state pm = { .phase = phases[i] };

		/* Even an exhausted recovery must read the latched events. */
		KUNIT_ASSERT_TRUE(test, cd321x_pm_event_lost(&pm));
		KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_REVALIDATE);
		KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
		KUNIT_EXPECT_FALSE(test, pm.force_reconnect);
		KUNIT_EXPECT_TRUE(test, cd321x_pm_begin_read(&pm));
		KUNIT_EXPECT_EQ(test, pm.attempts_left, CD321X_RESUME_ATTEMPTS - 1U);
	}
}

static void cd321x_lost_event_left_to_resume_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	cd321x_pm_init(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_event_lost(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_INITIALIZING);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_ready(&pm));

	/* A read failing during suspend is redone by the resume read. */
	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_event_lost(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_PREPARED);
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_EXPECT_TRUE(test, cd321x_pm_begin_read(&pm));

	cd321x_pm_remove(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_event_lost(&pm));
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_REMOVED);
}

static void cd321x_lost_event_keeps_reconnect_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};

	KUNIT_ASSERT_TRUE(test, cd321x_pm_retry(&pm, true));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_event_lost(&pm));
	/* A failed read is no evidence either way about the cable. */
	KUNIT_EXPECT_TRUE(test, pm.force_reconnect);
}

static void cd321x_unacked_events_outlast_budget_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int i, delay = 0;

	KUNIT_ASSERT_TRUE(test, cd321x_pm_event_lost(&pm));
	for (i = 0; i < 4 * CD321X_RESUME_ATTEMPTS; i++) {
		unsigned int next;

		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		next = cd321x_pm_ack_failed(&pm);
		/* Never given up, and not charged to the cable-state budget... */
		KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_REVALIDATE);
		KUNIT_EXPECT_EQ(test, pm.attempts_left, (unsigned int)CD321X_RESUME_ATTEMPTS);
		KUNIT_EXPECT_FALSE(test, cd321x_pm_can_update(&pm));
		/* ...only retried less and less often, down to a limit. */
		KUNIT_EXPECT_EQ(test, next, i ? min(2 * delay, CD321X_ACK_RETRY_MAX_MS) :
					     CD321X_ACK_RETRY_MS);
		delay = next;
	}
	KUNIT_EXPECT_EQ(test, delay, CD321X_ACK_RETRY_MAX_MS);

	/* Once acknowledged, the cable state has its bounded budget again. */
	KUNIT_EXPECT_TRUE(test, cd321x_pm_acked(&pm));
	KUNIT_EXPECT_FALSE(test, cd321x_pm_acked(&pm));
	for (i = 0; i < CD321X_RESUME_ATTEMPTS; i++) {
		KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
		KUNIT_EXPECT_EQ(test, cd321x_pm_retry(&pm, true),
				i + 1 < CD321X_RESUME_ATTEMPTS);
	}
	KUNIT_EXPECT_EQ(test, pm.phase, CD321X_PM_STALE);
}

static void cd321x_unacked_events_across_sleep_test(struct kunit *test)
{
	struct cd321x_pm_state pm = {};
	unsigned int delay;

	KUNIT_ASSERT_TRUE(test, cd321x_pm_event_lost(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	delay = cd321x_pm_ack_failed(&pm);
	cd321x_pm_prepare(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
	/* The resume read takes over the retries, without starting afresh. */
	KUNIT_ASSERT_TRUE(test, cd321x_pm_resume(&pm));
	KUNIT_ASSERT_TRUE(test, cd321x_pm_begin_read(&pm));
	KUNIT_EXPECT_GT(test, cd321x_pm_ack_failed(&pm), delay);
	cd321x_pm_remove(&pm);
	KUNIT_EXPECT_FALSE(test, cd321x_pm_begin_read(&pm));
}

/* Only a pipeline owned by another port skips the retry. */
static void cd321x_display_taken_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, cd321x_display_taken(-ENOSPC));
	/* A PHY mid-transition or a pending SAFE state must be retried. */
	KUNIT_EXPECT_FALSE(test, cd321x_display_taken(-EBUSY));
	KUNIT_EXPECT_FALSE(test, cd321x_display_taken(-EAGAIN));
	KUNIT_EXPECT_FALSE(test, cd321x_display_taken(-ETIMEDOUT));
	KUNIT_EXPECT_FALSE(test, cd321x_display_taken(0));
}

static struct kunit_case cd321x_pm_cases[] = {
	KUNIT_CASE(cd321x_busy_provider_preserves_connection_test),
	KUNIT_CASE(cd321x_busy_provider_completion_reopens_test),
	KUNIT_CASE(cd321x_busy_provider_keeps_real_failure_test),
	KUNIT_CASE(cd321x_late_link_failure_test),
	KUNIT_CASE(cd321x_link_event_during_final_apply_test),
	KUNIT_CASE(cd321x_link_reprobe_storm_bounded_test),
	KUNIT_CASE(cd321x_success_does_not_refill_link_budget_test),
	KUNIT_CASE(cd321x_link_event_init_and_remove_test),
	KUNIT_CASE(cd321x_link_event_suspend_and_rearm_test),
	KUNIT_CASE(cd321x_healthy_resume_keeps_connection_test),
	KUNIT_CASE(cd321x_failed_resume_forces_reconnect_test),
	KUNIT_CASE(cd321x_stale_recovery_keeps_reconnect_test),
	KUNIT_CASE(cd321x_resume_requires_snapshot_test),
	KUNIT_CASE(cd321x_revalidation_errors_bounded_test),
	KUNIT_CASE(cd321x_activation_failure_rereads_test),
	KUNIT_CASE(cd321x_abort_and_duplicate_completion_test),
	KUNIT_CASE(cd321x_repeat_suspend_cancels_retry_test),
	KUNIT_CASE(cd321x_suspend_before_apply_test),
	KUNIT_CASE(cd321x_remove_drains_resume_test),
	KUNIT_CASE(cd321x_read_and_apply_share_budget_test),
	KUNIT_CASE(cd321x_status_event_after_exhaustion_test),
	KUNIT_CASE(cd321x_runtime_failure_revalidates_test),
	KUNIT_CASE(cd321x_lost_event_rereads_test),
	KUNIT_CASE(cd321x_lost_event_left_to_resume_test),
	KUNIT_CASE(cd321x_lost_event_keeps_reconnect_test),
	KUNIT_CASE(cd321x_unacked_events_outlast_budget_test),
	KUNIT_CASE(cd321x_unacked_events_across_sleep_test),
	KUNIT_CASE(cd321x_display_taken_test),
	{}
};

static struct kunit_suite cd321x_pm_suite = {
	.name = "cd321x-pm",
	.test_cases = cd321x_pm_cases,
};

kunit_test_suite(cd321x_pm_suite);
MODULE_DESCRIPTION("KUnit tests for CD321x system sleep and cable-state revalidation");
MODULE_LICENSE("GPL");
