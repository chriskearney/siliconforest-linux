/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __CD321X_PM_H__
#define __CD321X_PM_H__

#include <linux/errno.h>

#include <linux/minmax.h>
#include <linux/types.h>

#define CD321X_RESUME_ATTEMPTS 3
#define CD321X_LINK_EVENTS 3
#define CD321X_ACK_RETRY_MS 500
#define CD321X_ACK_RETRY_MAX_MS 30000

enum cd321x_pm_phase {
	CD321X_PM_RUNNING,
	CD321X_PM_PREPARED,
	CD321X_PM_REVALIDATE,
	CD321X_PM_APPLY,
	CD321X_PM_STALE,
	CD321X_PM_REMOVED,
	CD321X_PM_INITIALIZING,
};

/* All transitions and the associated work scheduling hold tps->lock. */
struct cd321x_pm_state {
	enum cd321x_pm_phase phase;
	unsigned int attempts_left;
	unsigned int link_events_left;
	/* Non-zero while events may be left unacknowledged: the retry delay. */
	unsigned int ack_retry_ms;
	bool force_reconnect;
};

static inline void cd321x_pm_new_connection(struct cd321x_pm_state *pm)
{
	pm->link_events_left = CD321X_LINK_EVENTS;
}

static inline void cd321x_pm_init(struct cd321x_pm_state *pm)
{
	pm->phase = CD321X_PM_INITIALIZING;
	cd321x_pm_new_connection(pm);
}

static inline bool cd321x_pm_ready(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_INITIALIZING)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	return true;
}

static inline bool cd321x_pm_can_update(const struct cd321x_pm_state *pm)
{
	return pm->phase == CD321X_PM_RUNNING || pm->phase == CD321X_PM_APPLY;
}

static inline void cd321x_pm_prepare(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_REMOVED)
		pm->phase = CD321X_PM_PREPARED;
	pm->attempts_left = 0;
}

static inline bool cd321x_pm_resume(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_PREPARED)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	cd321x_pm_new_connection(pm);
	return true;
}

static inline bool cd321x_pm_link_event(struct cd321x_pm_state *pm)
{
	if (pm->phase == CD321X_PM_INITIALIZING ||
	    pm->phase == CD321X_PM_PREPARED || pm->phase == CD321X_PM_REMOVED ||
	    !pm->link_events_left)
		return false;

	/* Host reprobes must not create an unlimited series of recovery runs. */
	pm->link_events_left--;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	/* A stale notification does not itself prove the current session failed. */
	return true;
}

static inline bool cd321x_pm_begin_read(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_REVALIDATE || !pm->attempts_left)
		return false;
	pm->attempts_left--;
	return true;
}

static inline void cd321x_pm_snapshot_ready(struct cd321x_pm_state *pm)
{
	pm->phase = CD321X_PM_APPLY;
}

static inline bool cd321x_pm_retry(struct cd321x_pm_state *pm, bool reconnect)
{
	/* Runtime cable updates need the same bounded fresh-read recovery. */
	if (pm->phase == CD321X_PM_RUNNING) {
		pm->force_reconnect |= reconnect;
		pm->phase = CD321X_PM_REVALIDATE;
		pm->attempts_left = CD321X_RESUME_ATTEMPTS;
		return true;
	}
	if (pm->phase != CD321X_PM_REVALIDATE && pm->phase != CD321X_PM_APPLY)
		return false;
	pm->force_reconnect |= reconnect;
	if (pm->attempts_left) {
		pm->phase = CD321X_PM_REVALIDATE;
		return true;
	}
	/* Only a fresh read, triggered by a later event, can reopen updates. */
	pm->phase = CD321X_PM_STALE;
	return false;
}

static inline bool cd321x_pm_event(struct cd321x_pm_state *pm)
{
	if (pm->phase != CD321X_PM_STALE)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	return true;
}

/*
 * Events that could not be read and acknowledged stay latched in the
 * controller, which then sends no further interrupt. Only a fresh read can
 * acknowledge them, so request one with a full budget even if an earlier
 * recovery gave up. Sleep and probe already end with such a read.
 */
static inline bool cd321x_pm_event_lost(struct cd321x_pm_state *pm)
{
	if (pm->phase == CD321X_PM_INITIALIZING ||
	    pm->phase == CD321X_PM_PREPARED || pm->phase == CD321X_PM_REMOVED)
		return false;
	pm->phase = CD321X_PM_REVALIDATE;
	pm->attempts_left = CD321X_RESUME_ATTEMPTS;
	return true;
}

/*
 * Until the latched events are acknowledged, the controller raises no
 * interrupt that could restart a recovery that gave up. So a snapshot read
 * that fails to acknowledge them does not count against the cable-state
 * budget, and is never given up: it gives back the attempt taken by
 * cd321x_pm_begin_read() and is retried after a delay that doubles up to
 * CD321X_ACK_RETRY_MAX_MS. Sleep cancels the retry and the resume read takes
 * it over; removal ends it. Returns the delay in milliseconds.
 */
static inline unsigned int cd321x_pm_ack_failed(struct cd321x_pm_state *pm)
{
	pm->attempts_left++;
	pm->ack_retry_ms = clamp(2 * pm->ack_retry_ms, CD321X_ACK_RETRY_MS,
				 CD321X_ACK_RETRY_MAX_MS);
	return pm->ack_retry_ms;
}

/* Returns true if acknowledging the events had failed before. */
static inline bool cd321x_pm_acked(struct cd321x_pm_state *pm)
{
	bool retried = pm->ack_retry_ms;

	pm->ack_retry_ms = 0;
	return retried;
}

static inline void cd321x_pm_complete(struct cd321x_pm_state *pm)
{
	if (pm->phase == CD321X_PM_APPLY) {
		pm->phase = CD321X_PM_RUNNING;
		pm->attempts_left = 0;
		pm->force_reconnect = false;
	}
}

static inline void cd321x_pm_remove(struct cd321x_pm_state *pm)
{
	pm->phase = CD321X_PM_REMOVED;
	pm->attempts_left = 0;
}

/*
 * A display mux reports -ENOSPC when every display pipeline is driving
 * another port.  That lasts until the other display goes away, so the port's
 * USB path is set up without the display and nothing is retried.  Any other
 * error, such as -EBUSY from a PHY in the middle of a transition, is retried.
 */
static inline bool cd321x_display_taken(int ret)
{
	return ret == -ENOSPC;
}

#endif /* __CD321X_PM_H__ */
