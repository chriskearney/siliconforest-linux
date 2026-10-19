#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host regression tests for the ATCPHY USB3 handoff.

Compile the actual driver functions, defines, enums and mode table. MMIO polling
models a single completed timeout/success per invocation; no timing, electrical
behavior, firmware, real BIST sequence or kernel mutex behavior is simulated.
Hardware qualification remains necessary. Run --mutations for sensitivity checks.
"""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


def block(source, marker):
    if marker.startswith("static"):
        # Skip a forward declaration; the next opening brace after a prototype
        # belongs to an unrelated function.
        match = re.search(re.escape(marker) + r"[^;{]*\{", source)
        if match is None:
            raise ValueError("function definition not found: " + marker)
        start = match.start()
    else:
        start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    pos = opening + 1
    while depth:
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
        pos += 1
    return source[start:pos] + (";" if source[pos:pos + 1] == ";" else "")


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stddef.h>
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(h,l) ((UINT32_MAX >> (31-(h))) & (UINT32_MAX << (l)))
#define dev_warn(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_dbg(...) ((void)0)
#define lockdep_assert_held(...) ((void)0)
#define guard(which) (void)
#define __iomem
#define FIELD_PREP(mask,value) (((u32)(value) << __builtin_ctz(mask)) & (mask))
#define FIELD_GET(mask,value) (((value) & (mask)) >> __builtin_ctz(mask))
#define fallthrough __attribute__((fallthrough))
#define WARN_ON_ONCE(cond) do { if (cond) warnings++; } while (0)
#define container_of(ptr,type,member) ((type *)((char *)(ptr) - offsetof(type,member)))
'''

STUBS = r'''
struct atcphy_hw { enum atcphy_generation gen;
    bool has_t8140_dp, dp_t8122, has_usb4, park_pipe_unlocked, park_dummy_phy; };
struct reset_controller_dev { int unused; };
struct apple_atcphy {
    struct atcphy_hw *hw;
    struct { unsigned char *pipehandler, *core; } regs;
    void *dev;
    int lock;
    struct reset_controller_dev rcdev;
    enum atcphy_mode mode;
    enum atcphy_pipehandler_state pipe_state;
    enum atcphy_mode typec_mode;
    bool pipe_pending, t8103_dummy_pending, t8103_usb3_retry, mode_dirty, usb_off_pending;
    bool host_active, dp_only, ss_tunables, fixed_usb2, t8140_dp_tunables, dp_tunables;
};
struct phy { struct apple_atcphy *data; };
struct typec_mux_dev { struct apple_atcphy *data; };
struct typec_altmode { unsigned svid; };
struct typec_mux_state { unsigned long mode; struct typec_altmode *alt; void *data; };
struct enter_usb_data { u32 eudo; };
enum phy_mode { PHY_MODE_USB_HOST, PHY_MODE_USB_DEVICE, PHY_MODE_INVALID };
static unsigned char memory[0x18000] __attribute__((aligned(4)));
static struct atcphy_hw hw;
static struct apple_atcphy p;
static struct phy phy;
static unsigned polls, sleeps, sets, clears, muxes, bist_old, bist_new;
static unsigned configure_calls, restore_calls, unlocks, warnings;
static unsigned dummy_muxes, pending_seen_at_unlock;
static unsigned usb2_power_off_calls;
static unsigned ack_on;
static int check_ret, bist_ret, unlock_ret, configure_ret;
static bool concurrent_override_change;
static u32 readl(const void *ptr) { u32 v; memcpy(&v, ptr, sizeof(v)); return v; }
static void writel(u32 val, void *ptr) { memcpy(ptr, &val, sizeof(val)); }
static void set32(void *ptr, u32 mask) {
    if (ptr == memory + PIPEHANDLER_LOCK_REQ) sets++;
    writel(readl(ptr) | mask, ptr);
}
static void clear32(void *ptr, u32 mask) {
    if (ptr == memory + PIPEHANDLER_LOCK_REQ) clears++;
    writel(readl(ptr) & ~mask, ptr);
}
static void mask32(void *ptr, u32 mask, u32 value) {
    writel((readl(ptr) & ~mask) | value, ptr);
}
static u32 poll_value(void *ptr) {
    assert(ptr == memory + PIPEHANDLER_LOCK_ACK);
    polls++;
    if (concurrent_override_change) {
        set32(memory + PIPEHANDLER_OVERRIDE, BIT(9));
        set32(memory + PIPEHANDLER_OVERRIDE_VALUES, BIT(9));
    }
    return ack_on && polls >= ack_on ? PIPEHANDLER_LOCK_EN : 0;
}
#define readl_poll_timeout(addr,val,cond,delay,timeout) \
    ({ assert((delay) == 10 && (timeout) == 1000); \
       (val) = poll_value(addr); (cond) ? 0 : -ETIMEDOUT; })
static void usleep_range(unsigned lo, unsigned hi) {
    assert(lo == 5000 && hi == 10000); sleeps++;
}
static int atcphy_pipehandler_check(struct apple_atcphy *a) { (void)a; return check_ret; }
static int atcphy_pipehandler_unlock(struct apple_atcphy *a) {
    unlocks++;
    if (a->pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && a->t8103_dummy_pending)
        pending_seen_at_unlock++;
    clear32(a->regs.pipehandler + PIPEHANDLER_LOCK_REQ, PIPEHANDLER_LOCK_EN);
    return unlock_ret;
}
static int atcphy_configure_pipehandler_usb3_host_t8103(struct apple_atcphy *a) {
    (void)a; bist_old++; return bist_ret;
}
static int atcphy_configure_pipehandler_usb3_host_t8122(struct apple_atcphy *a) {
    (void)a; bist_new++; return bist_ret;
}
static void atcphy_pipehandler_set_mux(struct apple_atcphy *a, u32 data, u32 clk) {
    (void)a;
    assert((data == PIPEHANDLER_MUX_CTRL_DATA_USB3 && clk == PIPEHANDLER_MUX_CTRL_CLK_USB3) ||
           (data == PIPEHANDLER_MUX_CTRL_DATA_DUMMY && clk == PIPEHANDLER_MUX_CTRL_CLK_DUMMY) ||
           (data == PIPEHANDLER_MUX_CTRL_DATA_USB4 && clk == PIPEHANDLER_MUX_CTRL_CLK_USB4));
    if (data == PIPEHANDLER_MUX_CTRL_DATA_DUMMY) dummy_muxes++;
    muxes++;
}
static struct apple_atcphy *phy_get_drvdata(struct phy *f) { return f->data; }
static struct apple_atcphy *typec_mux_get_drvdata(struct typec_mux_dev *m) { return m->data; }
static void atcphy_restore_after_pd_off(struct apple_atcphy *a) { (void)a; restore_calls++; }
static int atcphy_usb2_power_off(struct apple_atcphy *a) {
    (void)a; usb2_power_off_calls++; return 0;
}
static int atcphy_configure(struct apple_atcphy *atcphy, enum atcphy_mode mode) {
    /* ACTUAL_CONFIGURE_ENTRY */
    struct apple_atcphy *a = atcphy;
    configure_calls++;
    assert(!a->t8103_dummy_pending);
    if (!configure_ret) a->mode = mode;
    return configure_ret;
}
static void reset(enum atcphy_generation generation, bool t8140) {
    memset(memory, 0, sizeof(memory));
    hw = (struct atcphy_hw){ .gen = generation, .has_t8140_dp = t8140 };
    p = (struct apple_atcphy){ .hw = &hw, .regs = {memory, memory},
        .mode = APPLE_ATCPHY_MODE_OFF, .ss_tunables = true };
    phy.data = &p;
    polls = sleeps = sets = clears = muxes = bist_old = bist_new = 0;
    configure_calls = restore_calls = unlocks = warnings = 0;
    dummy_muxes = pending_seen_at_unlock = 0;
    usb2_power_off_calls = 0;
    ack_on = 1;
    check_ret = bist_ret = unlock_ret = configure_ret = 0;
    concurrent_override_change = false;
}
'''

TESTS = r'''
static unsigned tests;
#define TEST(name) do { name(); tests++; } while (0)
static void lock_retry_last_success(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 10;
    writel(BIT(7), memory + PIPEHANDLER_LOCK_REQ);
    assert(!atcphy_pipehandler_lock(&p, true));
    assert(polls == 10 && sleeps == 9 && sets == 10 && clears == 9);
    assert(readl(memory + PIPEHANDLER_LOCK_REQ) == (BIT(7) | PIPEHANDLER_LOCK_EN));
}
static void lock_retry_exhausted(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 0;
    writel(BIT(7), memory + PIPEHANDLER_LOCK_REQ);
    assert(atcphy_pipehandler_lock(&p, true) == -ETIMEDOUT);
    assert(polls == 10 && sleeps == 9 && sets == 10 && clears == 10);
    assert(readl(memory + PIPEHANDLER_LOCK_REQ) == BIT(7));
}
static void lock_requested_is_not_ack(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 2;
    writel(BIT(7) | PIPEHANDLER_LOCK_EN, memory + PIPEHANDLER_LOCK_REQ);
    assert(!atcphy_pipehandler_lock(&p, true));
    assert(polls == 2 && sleeps == 1 && sets == 1 && clears == 1);
}
static void lock_requested_ack_immediate(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    writel(BIT(7) | PIPEHANDLER_LOCK_EN, memory + PIPEHANDLER_LOCK_REQ);
    assert(!atcphy_pipehandler_lock(&p, true));
    assert(polls == 1 && !sleeps && !sets && !clears);
    assert(readl(memory + PIPEHANDLER_LOCK_REQ) == (BIT(7) | PIPEHANDLER_LOCK_EN));
}
static void lock_later_gen_once(void) {
    reset(ATCPHY_GENERATION_T8122, false); ack_on = 0;
    assert(atcphy_pipehandler_lock(&p, true) == -ETIMEDOUT);
    assert(polls == 1 && sleeps == 0 && sets == 1 && clears == 1);
}
static void lock_legacy_requested_unchanged(void) {
    reset(ATCPHY_GENERATION_T8122, false); ack_on = 0;
    writel(PIPEHANDLER_LOCK_EN, memory + PIPEHANDLER_LOCK_REQ);
    assert(!atcphy_pipehandler_lock(&p, true)); assert(!polls && !sleeps && !clears);
}
static void lock_t8140_requested_needs_ack(void) {
    reset(ATCPHY_GENERATION_T8122, true); ack_on = 0;
    writel(BIT(7) | PIPEHANDLER_LOCK_EN, memory + PIPEHANDLER_LOCK_REQ);
    assert(atcphy_pipehandler_lock(&p, true) == -ETIMEDOUT);
    assert(polls == 1 && !sleeps && !sets && clears == 1);
    assert(readl(memory + PIPEHANDLER_LOCK_REQ) == BIT(7));
}
static void host_lock_failure_cleanup(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 0;
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE);
    writel(BIT(7) | PIPEHANDLER_OVERRIDE_VAL_RXDETECT0 |
           PIPEHANDLER_OVERRIDE_VAL_RXDETECT1, memory + PIPEHANDLER_OVERRIDE_VALUES);
    assert(atcphy_configure_pipehandler_usb3(&p, true) == -ETIMEDOUT);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) == BIT(7));
    assert(readl(memory + PIPEHANDLER_OVERRIDE_VALUES) == BIT(7));
    assert(!muxes && !bist_old && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
}
static void later_gen_failure_unchanged(void) {
    reset(ATCPHY_GENERATION_T8122, false); ack_on = 0;
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE);
    assert(atcphy_configure_pipehandler_usb3(&p, true) == -ETIMEDOUT);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) ==
        (BIT(7) | PIPEHANDLER_OVERRIDE_RXVALID | PIPEHANDLER_OVERRIDE_RXDETECT));
}
static void device_mode_no_lock_bist(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    assert(!atcphy_configure_pipehandler_usb3(&p, false));
    assert(muxes == 1 && !polls && !bist_old && !bist_new);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
}
static void success_clears_override(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE);
    assert(!atcphy_configure_pipehandler_usb3(&p, true));
    assert(muxes == 1 && bist_old == 1 && !bist_new);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) == BIT(7));
}
static void failed_setup_remains_retryable(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    check_ret = -EIO;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EIO);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !muxes);
    check_ret = 0;
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && muxes == 1);
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(muxes == 1 && !bist_old && !polls);
}
static void failed_backend_check_does_not_record_success(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    check_ret = -EIO;
    assert(atcphy_configure_pipehandler(&p, true) == -EIO);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !muxes);
    assert(!readl(memory + PIPEHANDLER_OVERRIDE));
    check_ret = 0;
    assert(!atcphy_configure_pipehandler(&p, true));
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && muxes == 1);
}
static void t8103_non_usb3_failure_state_unchanged(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB4;
    check_ret = -EIO;
    assert(atcphy_configure_pipehandler(&p, true) == -EIO);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB4 && !muxes);
}
static void later_gen_once_per_mode_unchanged(void) {
    reset(ATCPHY_GENERATION_T8122, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    check_ret = -EIO;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EIO);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0)); assert(!muxes);
}
static void off_configures_usb3(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(configure_calls == 1 && p.mode == APPLE_ATCPHY_MODE_USB3 && muxes == 1);
    assert(p.host_active && !bist_old && !bist_new);
}
static void off_without_tunables_refused(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.ss_tunables = false;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EOPNOTSUPP);
    assert(!configure_calls && !muxes && p.mode == APPLE_ATCPHY_MODE_OFF);
}
static void off_dp_only_refused(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.dp_only = true;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EOPNOTSUPP);
    assert(!configure_calls && !muxes && p.mode == APPLE_ATCPHY_MODE_OFF);
}
static void off_configuration_failure_propagated(void) {
    reset(ATCPHY_GENERATION_T8103, false); configure_ret = -EIO;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EIO);
    assert(configure_calls == 1 && !muxes && p.mode == APPLE_ATCPHY_MODE_OFF);
}
static void later_gen_off_unchanged(void) {
    reset(ATCPHY_GENERATION_T8122, false);
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(!configure_calls && !muxes && p.mode == APPLE_ATCPHY_MODE_OFF);
}
static void later_gen_host_still_bists(void) {
    reset(ATCPHY_GENERATION_T8122, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(polls == 1 && bist_new == 1 && !bist_old && muxes == 1);
}
static void device_role_from_off(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.host_active = true;
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_DEVICE, 0));
    assert(!p.host_active && configure_calls == 1 && muxes == 1 && !polls);
}
static void usb3_dp_same_backend_no_duplicate(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    p.mode = APPLE_ATCPHY_MODE_USB3_DP;
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(muxes == 1 && !configure_calls);
}
static void invalid_mode_refused(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_INVALID, 0) == -EINVAL);
    assert(!configure_calls && !muxes && !p.host_active);
}
static void t8140_pending_failure_retained(void) {
    reset(ATCPHY_GENERATION_T8122, true); p.mode = APPLE_ATCPHY_MODE_USB3;
    check_ret = -EIO;
    assert(atcphy_configure_pipehandler(&p, true) == -EIO);
    assert(p.pipe_pending && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
}
static void t8140_bist_failure_requires_rebuild(void) {
    reset(ATCPHY_GENERATION_T8122, true); p.mode = APPLE_ATCPHY_MODE_USB3;
    bist_ret = -EIO;
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE);
    assert(atcphy_configure_pipehandler(&p, true) == -EIO);
    assert(p.pipe_pending && p.mode_dirty && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) == BIT(7));
    assert(!muxes && bist_new == 1 && !readl(memory + PIPEHANDLER_LOCK_REQ));
}
static void t8140_unlock_failure_keeps_selected_backend(void) {
    reset(ATCPHY_GENERATION_T8122, true); p.mode = APPLE_ATCPHY_MODE_USB3;
    unlock_ret = -EIO;
    assert(atcphy_configure_pipehandler(&p, true) == -EIO);
    assert(p.pipe_pending && p.mode_dirty && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    assert(muxes == 1 && bist_new == 1);
}
static void dirty_and_off_pending_refused(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode_dirty = true;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EBUSY);
    assert(!configure_calls && !muxes);
    reset(ATCPHY_GENERATION_T8103, false); p.usb_off_pending = true;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EBUSY);
    assert(!p.host_active && !configure_calls && !muxes);
}
static void strict_park_other_generation_refused(void) {
    reset(ATCPHY_GENERATION_T8122, false); p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    assert(atcphy_park_live_t8103_pipehandler(&p) == -EOPNOTSUPP);
    assert(!polls && !unlocks && !muxes && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
}
static void strict_park_dummy_noop_usb4_refused(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    assert(!atcphy_park_live_t8103_pipehandler(&p));
    assert(!polls && !unlocks && !muxes);
    p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB4;
    assert(atcphy_park_live_t8103_pipehandler(&p) == -EBUSY);
    assert(!polls && !unlocks && !muxes);
}
static void strict_park_check_failure_untouched(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    check_ret = -EIO;
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE);
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE_VALUES);
    assert(atcphy_park_live_t8103_pipehandler(&p) == -EIO);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) == BIT(7));
    assert(readl(memory + PIPEHANDLER_OVERRIDE_VALUES) == BIT(7));
    assert(!polls && !muxes && !p.t8103_dummy_pending && !p.mode_dirty);
}
static void strict_park_lock_failure_restores_masks(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    ack_on = 0; concurrent_override_change = true;
    writel(BIT(7) | PIPEHANDLER_OVERRIDE_RXVALID, memory + PIPEHANDLER_OVERRIDE);
    writel(BIT(7) | PIPEHANDLER_OVERRIDE_VAL_RXDETECT1, memory + PIPEHANDLER_OVERRIDE_VALUES);
    assert(atcphy_park_live_t8103_pipehandler(&p) == -ETIMEDOUT);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) ==
        (BIT(7) | BIT(9) | PIPEHANDLER_OVERRIDE_RXVALID));
    assert(readl(memory + PIPEHANDLER_OVERRIDE_VALUES) ==
        (BIT(7) | BIT(9) | PIPEHANDLER_OVERRIDE_VAL_RXDETECT1));
    assert(polls == 10 && !muxes && !unlocks);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && !p.t8103_dummy_pending && !p.mode_dirty);
}
static void strict_park_success_resets_native(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    writel(BIT(7) | PIPEHANDLER_NATIVE_POWER_DOWN, memory + PIPEHANDLER_NONSELECTED_OVERRIDE);
    assert(!atcphy_park_live_t8103_pipehandler(&p));
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !p.t8103_dummy_pending && !p.mode_dirty);
    assert(p.t8103_usb3_retry);
    assert(polls == 1 && unlocks == 1 && dummy_muxes == 1 && pending_seen_at_unlock == 1);
    assert(readl(memory + PIPEHANDLER_NONSELECTED_OVERRIDE) ==
        (BIT(7) | PIPEHANDLER_NATIVE_RESET | FIELD_PREP(PIPEHANDLER_NATIVE_POWER_DOWN, 2)));
}
static void strict_park_pending_completion_retry(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    unlock_ret = -EIO;
    assert(atcphy_park_live_t8103_pipehandler(&p) == -EIO);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && p.t8103_dummy_pending && !p.mode_dirty);
    assert(p.t8103_usb3_retry);
    assert(polls == 1 && unlocks == 1 && dummy_muxes == 1 && !readl(memory + PIPEHANDLER_NONSELECTED_OVERRIDE));
    assert(atcphy_park_live_t8103_pipehandler(&p) == -EIO);
    assert(polls == 1 && unlocks == 2 && dummy_muxes == 1 && p.t8103_dummy_pending);
    unlock_ret = 0;
    assert(!atcphy_park_live_t8103_pipehandler(&p));
    assert(polls == 1 && unlocks == 3 && dummy_muxes == 1 && pending_seen_at_unlock == 3);
    assert(!p.t8103_dummy_pending && !p.mode_dirty);
}
static void generic_park_pending_cannot_bypass_stopped(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    hw.park_pipe_unlocked = true; unlock_ret = -EIO;
    assert(atcphy_park_pipehandler(&p, false) == -EIO);
    assert(atcphy_park_pipehandler(&p, true) == -EIO);
    assert(p.t8103_dummy_pending && unlocks == 2 && !muxes && !polls);
}
static void configure_pending_refuses_lane_work(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    assert(atcphy_configure(&p, APPLE_ATCPHY_MODE_USB3) == -EBUSY);
    assert(atcphy_configure(&p, APPLE_ATCPHY_MODE_OFF) == -EBUSY);
    assert(!configure_calls && !muxes);
}
static void set_mode_finishes_pending_before_reconfigure(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true; unlock_ret = -EIO;
    assert(atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0) == -EIO);
    assert(p.t8103_dummy_pending && !muxes && !configure_calls && !p.host_active);
    unlock_ret = 0;
    assert(!atcphy_usb3_set_mode(&phy, PHY_MODE_USB_HOST, 0));
    assert(!p.t8103_dummy_pending && muxes == 1 && !dummy_muxes && !configure_calls);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && !p.mode_dirty && p.host_active);
}
static void mux_same_mode_finishes_pending_before_return(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    struct typec_mux_dev mux = {.data = &p};
    struct typec_mux_state state = {.mode = TYPEC_STATE_SAFE};
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true; unlock_ret = -EIO;
    assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
    assert(p.t8103_dummy_pending && !configure_calls && !muxes);
    unlock_ret = 0;
    assert(!atcphy_mux_set_locked(&mux, &state));
    assert(!p.t8103_dummy_pending && !configure_calls && !muxes && unlocks == 2);
}
static void mux_usb3_to_safe_requires_completed_park(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    struct typec_mux_dev mux = {.data = &p};
    struct typec_mux_state state = {.mode = TYPEC_STATE_SAFE};
    unlock_ret = -EIO;
    assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
    assert(p.mode == APPLE_ATCPHY_MODE_USB3 && p.t8103_dummy_pending && !configure_calls && !p.mode_dirty);
    unlock_ret = 0;
    assert(!atcphy_mux_set_locked(&mux, &state));
    assert(p.mode == APPLE_ATCPHY_MODE_OFF && !p.t8103_dummy_pending && configure_calls == 1);
    assert(dummy_muxes == 1 && polls == 1 && unlocks == 2 && !warnings);
}
static void mux_usb3_dp_does_not_park_or_warn(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    struct typec_mux_dev mux = {.data = &p};
    struct typec_altmode alt = {.svid = USB_TYPEC_DP_SID};
    struct typec_mux_state state = {.mode = TYPEC_DP_STATE_D, .alt = &alt};
    assert(!atcphy_mux_set_locked(&mux, &state));
    assert(p.mode == APPLE_ATCPHY_MODE_USB3_DP && configure_calls == 1);
    assert(!dummy_muxes && !polls && !unlocks && !warnings && !p.t8103_dummy_pending);
}
static void mux_later_gen_teardown_unchanged(void) {
    reset(ATCPHY_GENERATION_T8122, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    struct typec_mux_dev mux = {.data = &p};
    struct typec_mux_state state = {.mode = TYPEC_STATE_SAFE};
    assert(!atcphy_mux_set_locked(&mux, &state));
    assert(configure_calls == 1 && !polls && !unlocks && !dummy_muxes && warnings == 1);
}
static void mux_admission_precedes_lane_work(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.ss_tunables = false;
    struct typec_mux_dev mux = {.data = &p};
    struct typec_mux_state state = {.mode = TYPEC_STATE_USB};
    assert(atcphy_mux_set_locked(&mux, &state) == -EOPNOTSUPP);
    assert(!configure_calls && !polls && !muxes);
    p.ss_tunables = true; p.dp_only = true;
    assert(atcphy_mux_set_locked(&mux, &state) == -EOPNOTSUPP);
    assert(!configure_calls && !polls && !muxes);
}
static void canceled_mux_same_mode_restores_backend(void) {
    for (unsigned host = 0; host <= 1; host++) {
        reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
        p.host_active = host; p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
        struct typec_mux_dev mux = {.data = &p};
        struct typec_mux_state state = {.mode = TYPEC_STATE_USB};
        assert(!atcphy_mux_set_locked(&mux, &state));
        assert(!p.t8103_dummy_pending && p.host_active == !!host);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && muxes == 1);
        assert(!dummy_muxes && !polls && unlocks == 1 && !bist_old && !configure_calls);
    }
}
static void configure_backend_pending_refused(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    assert(atcphy_configure_pipehandler(&p, false) == -EBUSY);
    assert(!muxes && p.t8103_dummy_pending && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
}
static void canceled_mux_reroute_failure_remains_retryable(void) {
    for (unsigned host = 0; host <= 1; host++) {
        reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
        p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
        p.host_active = host; check_ret = -EIO;
        struct typec_mux_dev mux = {.data = &p};
        struct typec_mux_state state = {.mode = TYPEC_STATE_USB};
        assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
        assert(!p.t8103_dummy_pending && p.t8103_usb3_retry);
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
        check_ret = 0;
        assert(!atcphy_mux_set_locked(&mux, &state));
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && muxes == 1);
        assert(!bist_old && !configure_calls && !p.mode_dirty && !p.t8103_usb3_retry);
        assert(p.host_active == !!host);
    }
}
static void reset_assert_pending_failure_preserves_controller(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    p.host_active = true; unlock_ret = -EIO;
    writel(BIT(7) | PIPEHANDLER_AON_GEN_DWC3_RESET_N, memory + PIPEHANDLER_AON_GEN);
    assert(atcphy_dwc3_reset_assert(&p.rcdev, 0) == -EIO);
    assert(readl(memory + PIPEHANDLER_AON_GEN) == (BIT(7) | PIPEHANDLER_AON_GEN_DWC3_RESET_N));
    assert(!usb2_power_off_calls && p.host_active && p.t8103_dummy_pending && !p.mode_dirty);
    assert(p.t8103_usb3_retry);
    assert(!muxes && unlocks == 1);
}
static void reset_assert_pending_success_then_resets(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.t8103_dummy_pending = true; p.t8103_usb3_retry = true; p.host_active = true;
    writel(BIT(7) | PIPEHANDLER_AON_GEN_DWC3_RESET_N, memory + PIPEHANDLER_AON_GEN);
    assert(!atcphy_dwc3_reset_assert(&p.rcdev, 0));
    assert(readl(memory + PIPEHANDLER_AON_GEN) == (BIT(7) | PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN));
    assert(usb2_power_off_calls == 1 && !p.host_active && !p.t8103_dummy_pending);
    assert(!p.t8103_usb3_retry);
    assert(!muxes && unlocks == 1);
}
static void reset_deassert_retries_completion_before_register_writes(void) {
    for (unsigned host = 0; host <= 1; host++) {
        reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
        p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
        p.host_active = host; unlock_ret = -ETIMEDOUT;
        writel(BIT(7) | PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN, memory + PIPEHANDLER_AON_GEN);
        assert(atcphy_dwc3_reset_deassert(&p.rcdev, 0) == -ETIMEDOUT);
        assert(readl(memory + PIPEHANDLER_AON_GEN) ==
            (BIT(7) | PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN));
        assert(p.t8103_dummy_pending && p.t8103_usb3_retry && p.host_active == !!host);
        assert(unlocks == 1 && !polls && !muxes && !usb2_power_off_calls && !configure_calls);
        unlock_ret = 0;
        assert(!atcphy_dwc3_reset_deassert(&p.rcdev, 0));
        assert(readl(memory + PIPEHANDLER_AON_GEN) == (BIT(7) | PIPEHANDLER_AON_GEN_DWC3_RESET_N));
        assert(!p.t8103_dummy_pending && p.t8103_usb3_retry && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
        assert(unlocks == 2 && !polls && !muxes && !usb2_power_off_calls && !configure_calls);
        assert(!atcphy_usb3_set_mode(&phy, host ? PHY_MODE_USB_HOST : PHY_MODE_USB_DEVICE, 0));
        assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3 && !p.t8103_usb3_retry);
        assert(muxes == 1 && !dummy_muxes && unlocks == 2 && !polls && !bist_old && !p.mode_dirty);
    }
}
static void later_gen_reset_deassert_unchanged(void) {
    for (unsigned t8140 = 0; t8140 <= 1; t8140++) {
        reset(ATCPHY_GENERATION_T8122, t8140);
        check_ret = unlock_ret = -EIO;
        writel(BIT(7) | PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN, memory + PIPEHANDLER_AON_GEN);
        assert(!atcphy_dwc3_reset_deassert(&p.rcdev, 0));
        assert(readl(memory + PIPEHANDLER_AON_GEN) == (BIT(7) | PIPEHANDLER_AON_GEN_DWC3_RESET_N));
        assert(!p.t8103_dummy_pending && !p.t8103_usb3_retry);
        assert(!polls && !unlocks && !muxes && !usb2_power_off_calls && !configure_calls);
    }
}
static void usb3_power_off_pending_preserves_live_controller(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true; p.host_active = true; unlock_ret = -EIO;
    assert(atcphy_usb3_power_off(&phy) == -EIO);
    assert(p.t8103_dummy_pending && p.host_active && p.mode == APPLE_ATCPHY_MODE_USB3);
    assert(p.t8103_usb3_retry);
    assert(!configure_calls && !muxes);
    unlock_ret = 0;
    assert(!atcphy_usb3_power_off(&phy));
    assert(!p.t8103_dummy_pending && !p.host_active && p.mode == APPLE_ATCPHY_MODE_OFF);
    assert(!p.t8103_usb3_retry);
    assert(configure_calls == 1 && !muxes);
}
static void failed_target_configuration_retains_retry_until_commit(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    struct typec_mux_dev mux = {.data = &p};
    struct typec_mux_state state = {.mode = TYPEC_STATE_SAFE};
    configure_ret = -EIO;
    assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
    assert(p.t8103_usb3_retry && !p.t8103_dummy_pending && p.mode == APPLE_ATCPHY_MODE_USB3);
    configure_ret = 0;
    assert(!atcphy_mux_set_locked(&mux, &state));
    assert(!p.t8103_usb3_retry && p.mode == APPLE_ATCPHY_MODE_OFF);
    assert(configure_calls == 2 && !muxes && unlocks == 1);
}
static void failed_target_then_canceled_transition_reconnects(void) {
    for (unsigned host = 0; host <= 1; host++) {
        reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
        p.t8103_dummy_pending = true; p.t8103_usb3_retry = true; p.host_active = host;
        struct typec_mux_dev mux = {.data = &p};
        struct typec_mux_state state = {.mode = TYPEC_STATE_SAFE};
        configure_ret = -EIO;
        assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
        assert(p.t8103_usb3_retry && p.mode == APPLE_ATCPHY_MODE_USB3);
        state.mode = TYPEC_STATE_USB; configure_ret = 0;
        assert(!atcphy_mux_set_locked(&mux, &state));
        assert(!p.t8103_usb3_retry && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
        assert(muxes == 1 && configure_calls == 1 && p.host_active == !!host);
    }
}
static void changed_usb3_dp_target_preserves_retry_across_two_failures(void) {
    reset(ATCPHY_GENERATION_T8103, false); p.mode = APPLE_ATCPHY_MODE_USB3;
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    struct typec_mux_dev mux = {.data = &p};
    struct typec_altmode alt = {.svid = USB_TYPEC_DP_SID};
    struct typec_mux_state state = {.mode = TYPEC_DP_STATE_D, .alt = &alt};
    configure_ret = -EIO;
    assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
    assert(p.t8103_usb3_retry && !p.t8103_dummy_pending && p.mode == APPLE_ATCPHY_MODE_USB3);
    configure_ret = 0; check_ret = -EIO;
    assert(atcphy_mux_set_locked(&mux, &state) == -EIO);
    assert(p.t8103_usb3_retry && p.mode == APPLE_ATCPHY_MODE_USB3_DP);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY);
    check_ret = 0;
    assert(!atcphy_mux_set_locked(&mux, &state));
    assert(!p.t8103_usb3_retry && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    assert(configure_calls == 2 && muxes == 1 && !dummy_muxes && !warnings);
}
static void known_reset_setup_clears_both_flags(void) {
    reset(ATCPHY_GENERATION_T8103, false);
    p.t8103_dummy_pending = true; p.t8103_usb3_retry = true;
    p.pipe_pending = true;
    atcphy_setup_pipehandler(&p);
    assert(!p.t8103_dummy_pending && !p.t8103_usb3_retry && !p.pipe_pending);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && dummy_muxes == 1 && !unlocks);
}
static void nonlive_t8103_lock_does_not_retry(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 0;
    assert(atcphy_pipehandler_lock(&p, false) == -ETIMEDOUT);
    assert(polls == 1 && !sleeps && sets == 1 && clears == 1);
}
static void usb4_without_pipe_clock_uses_single_poll(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 0;
    writel(BIT(7), memory + PIPEHANDLER_OVERRIDE);
    assert(!atcphy_configure_pipehandler_usb4(&p));
    assert(polls == 1 && !sleeps && muxes == 1 && !dummy_muxes && unlocks == 1);
    assert(readl(memory + PIPEHANDLER_OVERRIDE) == BIT(7));
}
static void postclamp_legacy_dummy_uses_single_poll(void) {
    reset(ATCPHY_GENERATION_T8103, false); ack_on = 0;
    p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3;
    _atcphy_dwc3_reset_assert(&p);
    assert(!atcphy_park_pipehandler(&p, true));
    assert(polls == 1 && !sleeps && dummy_muxes == 1 && unlocks == 1);
    assert(p.pipe_state == ATCPHY_PIPEHANDLER_STATE_DUMMY && !p.t8103_dummy_pending);
    assert(readl(memory + PIPEHANDLER_AON_GEN) == PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
}
static void fixedhub_host_dispatch_skips_only_t8103_bist(void) {
    for (unsigned chip = 0; chip < 3; chip++) {
        reset(chip ? ATCPHY_GENERATION_T8122 : ATCPHY_GENERATION_T8103, chip == 2);
        p.fixed_usb2 = true; p.host_active = true;
        struct typec_mux_dev mux = {.data = &p};
        struct typec_mux_state state = {.mode = TYPEC_STATE_USB};
        assert(!atcphy_mux_set_locked(&mux, &state));
        assert(p.mode == APPLE_ATCPHY_MODE_USB3 && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
        assert(muxes == 1 && !bist_old && configure_calls == 1 && !p.pipe_pending);
        assert(bist_new == (chip != 0) && polls == (chip != 0));
    }
}
static void t8140_live_dummy_failure_remains_strict(void) {
    reset(ATCPHY_GENERATION_T8122, true); ack_on = 0;
    p.pipe_state = ATCPHY_PIPEHANDLER_STATE_USB3; p.fixed_usb2 = true; p.host_active = true;
    assert(atcphy_park_pipehandler(&p, false) == -ETIMEDOUT);
    assert(polls == 1 && !sleeps && !muxes && !unlocks);
    assert(p.mode_dirty && p.pipe_state == ATCPHY_PIPEHANDLER_STATE_USB3);
    assert(!p.t8103_dummy_pending && !p.t8103_usb3_retry);
}
int main(void) {
    TEST(lock_retry_last_success); TEST(lock_retry_exhausted);
    TEST(lock_requested_is_not_ack); TEST(lock_requested_ack_immediate); TEST(lock_later_gen_once);
    TEST(lock_legacy_requested_unchanged); TEST(lock_t8140_requested_needs_ack);
    TEST(host_lock_failure_cleanup); TEST(later_gen_failure_unchanged);
    TEST(device_mode_no_lock_bist); TEST(success_clears_override);
    TEST(failed_setup_remains_retryable); TEST(failed_backend_check_does_not_record_success);
    TEST(t8103_non_usb3_failure_state_unchanged); TEST(later_gen_once_per_mode_unchanged);
    TEST(off_configures_usb3); TEST(off_without_tunables_refused);
    TEST(off_dp_only_refused); TEST(off_configuration_failure_propagated);
    TEST(later_gen_off_unchanged); TEST(later_gen_host_still_bists); TEST(device_role_from_off);
    TEST(usb3_dp_same_backend_no_duplicate); TEST(invalid_mode_refused);
    TEST(t8140_pending_failure_retained); TEST(t8140_bist_failure_requires_rebuild);
    TEST(t8140_unlock_failure_keeps_selected_backend); TEST(dirty_and_off_pending_refused);
    TEST(strict_park_other_generation_refused); TEST(strict_park_dummy_noop_usb4_refused);
    TEST(strict_park_check_failure_untouched); TEST(strict_park_lock_failure_restores_masks);
    TEST(strict_park_success_resets_native); TEST(strict_park_pending_completion_retry);
    TEST(generic_park_pending_cannot_bypass_stopped); TEST(configure_pending_refuses_lane_work);
    TEST(set_mode_finishes_pending_before_reconfigure); TEST(mux_same_mode_finishes_pending_before_return);
    TEST(mux_usb3_to_safe_requires_completed_park); TEST(mux_usb3_dp_does_not_park_or_warn);
    TEST(mux_later_gen_teardown_unchanged); TEST(mux_admission_precedes_lane_work);
    TEST(canceled_mux_same_mode_restores_backend); TEST(configure_backend_pending_refused);
    TEST(canceled_mux_reroute_failure_remains_retryable);
    TEST(reset_assert_pending_failure_preserves_controller); TEST(reset_assert_pending_success_then_resets);
    TEST(reset_deassert_retries_completion_before_register_writes);
    TEST(later_gen_reset_deassert_unchanged);
    TEST(usb3_power_off_pending_preserves_live_controller);
    TEST(failed_target_configuration_retains_retry_until_commit);
    TEST(failed_target_then_canceled_transition_reconnects);
    TEST(changed_usb3_dp_target_preserves_retry_across_two_failures);
    TEST(known_reset_setup_clears_both_flags);
    TEST(nonlive_t8103_lock_does_not_retry);
    TEST(usb4_without_pipe_clock_uses_single_poll);
    TEST(postclamp_legacy_dummy_uses_single_poll);
    TEST(fixedhub_host_dispatch_skips_only_t8103_bist);
    TEST(t8140_live_dummy_failure_remains_strict);
    printf("%u ATCPHY USB3 host tests passed\n", tests);
}
'''


def harness(source):
    # Preserve continuation lines: a line-only regexp can swallow the next enum
    # as part of a trailing backslash macro and silently change its meaning.
    definitions = []
    continuing = False
    for line in source.splitlines():
        if continuing or line.startswith("#define "):
            definitions.append(line)
            continuing = line.endswith("\\")
    defines = "\n".join(definitions)
    root = Path(__file__).resolve().parents[2]
    alt_header = (root / "include/linux/usb/typec_altmode.h").read_text()
    dp_header = (root / "include/linux/usb/typec_dp.h").read_text()
    usb_types = "\n".join((block(alt_header, "enum {\n\tTYPEC_STATE_SAFE"),
                            block(alt_header, "enum {\n\tTYPEC_MODE_USB2"),
                            block(dp_header, "enum {\n\tTYPEC_DP_STATE_A")))
    usb_defines = []
    for name in ("pd.h", "typec_dp.h", "typec_tbt.h"):
        for line in (root / "include/linux/usb" / name).read_text().splitlines():
            if re.match(r"#define\s+(EUDO_USB_MODE_|USB_TYPEC_(DP_SID|TBT_SID|VENDOR_INTEL))", line):
                usb_defines.append(line)
    types = usb_types + "\n" + "\n".join(block(source, "enum " + name + " {") for name in (
        "atcphy_generation", "atcphy_lane_mode", "atcphy_pipehandler_state",
        "atcphy_mode", "atcphy_lane"))
    types += "\n" + block(source, "struct atcphy_mode_configuration {")
    start = source.index("static const struct {\n\tconst struct atcphy_mode_configuration normal;")
    end = source.index("\n};", source.index("} atcphy_modes[] = {", start)) + 3
    table = source[start:end]
    function_specs = [
        ("int", "atcphy_pipehandler_lock"),
        ("int", "atcphy_configure_pipehandler_usb3"),
        ("int", "atcphy_configure_pipehandler_usb4"),
        ("void", "atcphy_enable_dummy_phy"),
        ("bool", "atcphy_needs_dummy_phy"),
        ("int", "atcphy_configure_pipehandler_dummy"),
        ("int", "atcphy_configure_pipehandler"),
        ("int", "atcphy_park_live_t8103_pipehandler"),
        ("int", "atcphy_park_pipehandler"),
        ("int", "atcphy_usb3_set_mode"),
        ("int", "atcphy_mux_set_locked"),
        ("int", "atcphy_usb3_power_off"),
        ("void", "_atcphy_dwc3_reset_assert"),
        ("int", "atcphy_dwc3_reset_assert"),
        ("int", "atcphy_dwc3_reset_deassert"),
        ("void", "atcphy_setup_pipehandler"),
    ]
    functions = "\n".join(block(source, "static " + kind + " " + name + "(")
                          for kind, name in function_specs)
    configure = block(source, "static int atcphy_configure(")
    guard_start = configure.index("lockdep_assert_held")
    guard_end = configure.index("/* The T8140 transition state")
    stubs = STUBS.replace("/* ACTUAL_CONFIGURE_ENTRY */", configure[guard_start:guard_end])
    return PRELUDE + defines + "\n" + "\n".join(usb_defines) + "\n" + types + "\n" + table + stubs + functions + TESTS


MUTATIONS = {
    "no-t8103-retry": ("? 10 : 1;", "? 1 : 1;"),
    "retry-all-generations": ("? 10 : 1;", "? 10 : 10;"),
    "request-mistaken-for-ack": ("if (atcphy->hw->gen != ATCPHY_GENERATION_T8103 &&\n\t\t    !atcphy->hw->has_t8140_dp)", "if (true)"),
    "sleep-after-last-poll": ("if (attempt + 1 < attempts)", "if (true)"),
    "no-override-cleanup": ("if (atcphy->hw->gen == ATCPHY_GENERATION_T8103)\n\t\t\t\tclear32", "if (false)\n\t\t\t\tclear32"),
    "cleanup-all-generations": ("if (atcphy->hw->gen == ATCPHY_GENERATION_T8103)\n\t\t\t\tclear32", "if (true)\n\t\t\t\tclear32"),
    "failed-setup-recorded": ("!(atcphy->hw->gen == ATCPHY_GENERATION_T8103 &&\n\t\t       state == ATCPHY_PIPEHANDLER_STATE_USB3)", "true"),
    "missing-tunable-admission": ("if (atcphy->dp_only || !atcphy->ss_tunables)", "if (atcphy->dp_only)"),
    "missing-dp-only-admission": ("if (atcphy->dp_only || !atcphy->ss_tunables)", "if (!atcphy->ss_tunables)"),
    "off-config-all-generations": ("if (atcphy->hw->gen == ATCPHY_GENERATION_T8103 &&\n\t    atcphy->mode == APPLE_ATCPHY_MODE_OFF)", "if (atcphy->mode == APPLE_ATCPHY_MODE_OFF)"),
    "bist-skipped-all-generations": ("host && atcphy->hw->gen != ATCPHY_GENERATION_T8103", "false"),
    "t8103-bist-restored": ("host && atcphy->hw->gen != ATCPHY_GENERATION_T8103", "host"),
    "retry-applied-to-nonlive-lock": ("atcphy_pipehandler_lock",
        "unsigned int attempts = live &&", "unsigned int attempts ="),
    "usb4-mistaken-for-live-pipe": ("atcphy_configure_pipehandler_usb4",
        "atcphy_pipehandler_lock(atcphy, false)", "atcphy_pipehandler_lock(atcphy, true)"),
    "legacy-dummy-mistaken-for-live-pipe": ("atcphy_configure_pipehandler_dummy",
        "atcphy_pipehandler_lock(atcphy, false)", "atcphy_pipehandler_lock(atcphy, true)"),
    "strict-park-generation-gate-removed": ("atcphy_park_live_t8103_pipehandler",
        "if (atcphy->hw->gen != ATCPHY_GENERATION_T8103)", "if (false)"),
    "strict-park-no-value-restore": ("atcphy_park_live_t8103_pipehandler",
        "mask32(pipe + PIPEHANDLER_OVERRIDE_VALUES, value_mask,\n\t\t\t       values & value_mask);", "(void)values;"),
    "strict-park-no-override-restore": ("atcphy_park_live_t8103_pipehandler",
        "mask32(pipe + PIPEHANDLER_OVERRIDE, override_mask,\n\t\t\t       overrides & override_mask);", "(void)overrides;"),
    "strict-park-clobbers-unrelated-bits": ("atcphy_park_live_t8103_pipehandler",
        "mask32(pipe + PIPEHANDLER_OVERRIDE, override_mask,\n\t\t\t       overrides & override_mask);", "writel(overrides, pipe + PIPEHANDLER_OVERRIDE);"),
    "strict-park-does-not-record-physical-backend": ("atcphy_park_live_t8103_pipehandler",
        "atcphy->pipe_state = ATCPHY_PIPEHANDLER_STATE_DUMMY;", "/* omitted */"),
    "strict-park-does-not-retain-pending": ("atcphy_park_live_t8103_pipehandler",
        "atcphy->t8103_dummy_pending = true;", "atcphy->t8103_dummy_pending = false;"),
    "strict-park-relocks-pending": ("atcphy_park_live_t8103_pipehandler",
        "if (!atcphy->t8103_dummy_pending)", "if (true)"),
    "strict-park-clears-pending-on-error": ("atcphy_park_live_t8103_pipehandler",
        "ret = atcphy_pipehandler_unlock(atcphy);\n\tif (ret)",
        "ret = atcphy_pipehandler_unlock(atcphy);\n\tatcphy->t8103_dummy_pending = false;\n\tif (ret)"),
    "strict-park-poisons-mode-dirty": ("atcphy_park_live_t8103_pipehandler",
        "atcphy->t8103_dummy_pending = true;", "atcphy->t8103_dummy_pending = true; atcphy->mode_dirty = true;"),
    "generic-park-bypasses-pending": ("atcphy_park_pipehandler",
        "if (atcphy->t8103_dummy_pending)\n\t\treturn atcphy_park_live_t8103_pipehandler(atcphy);", "/* omitted */"),
    "configure-lanes-ignores-pending": ("atcphy_configure",
        "if (atcphy->t8103_dummy_pending)\n\t\treturn -EBUSY;", "/* omitted */"),
    "configure-backend-ignores-pending": ("atcphy_configure_pipehandler",
        "if (atcphy->t8103_dummy_pending)\n\t\treturn -EBUSY;", "/* omitted */"),
    "set-mode-ignores-pending": ("atcphy_usb3_set_mode",
        "if (atcphy->t8103_dummy_pending) {", "if (false) {"),
    "mux-ignores-pending": ("atcphy_mux_set_locked",
        "if (atcphy->t8103_dummy_pending) {", "if (false) {"),
    "mux-canceled-transition-not-rerouted": ("atcphy_mux_set_locked",
        "if (atcphy->t8103_usb3_retry &&", "if (false &&"),
    "mux-usb3-dp-incorrectly-parked": ("atcphy_mux_set_locked",
        "atcphy_modes[target_mode].pipehandler_state == ATCPHY_PIPEHANDLER_STATE_DUMMY) {", "true) {"),
    "reset-assert-ignores-pending": ("atcphy_dwc3_reset_assert",
        "atcphy->hw->has_t8140_dp || atcphy->t8103_dummy_pending", "atcphy->hw->has_t8140_dp"),
    "reset-deassert-ignores-pending": ("atcphy_dwc3_reset_deassert",
        "if (atcphy->t8103_dummy_pending) {", "if (false) {"),
    "reset-deassert-strands-completion": ("atcphy_dwc3_reset_deassert",
        "if (atcphy->t8103_dummy_pending) {", "if (atcphy->t8103_dummy_pending) return -EBUSY;\n\tif (atcphy->t8103_dummy_pending) {"),
    "reset-deassert-ignores-completion-error": ("atcphy_dwc3_reset_deassert",
        "if (ret)\n\t\t\treturn ret;", "if (false)\n\t\t\treturn ret;"),
    "reset-deassert-writes-before-completion": ("atcphy_dwc3_reset_deassert",
        "if (atcphy->t8103_dummy_pending) {",
        "clear32(atcphy->regs.pipehandler + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);\n\tif (atcphy->t8103_dummy_pending) {"),
    "sticky-retry-not-set-after-mux": ("atcphy_park_live_t8103_pipehandler",
        "atcphy->t8103_usb3_retry = true;", "/* omitted */"),
    "sticky-retry-cleared-after-unlock": ("atcphy_park_live_t8103_pipehandler",
        "atcphy->t8103_dummy_pending = false;", "atcphy->t8103_dummy_pending = false; atcphy->t8103_usb3_retry = false;"),
    "sticky-retry-cleared-on-failed-reroute": ("atcphy_configure_pipehandler",
        "if (!ret && atcphy->hw->gen == ATCPHY_GENERATION_T8103 &&", "if (atcphy->hw->gen == ATCPHY_GENERATION_T8103 &&"),
    "sticky-retry-not-cleared-on-success": ("atcphy_configure_pipehandler",
        "atcphy->t8103_usb3_retry = false;", "(void)0;"),
    "sticky-retry-cleared-before-target-commit": ("atcphy_mux_set_locked",
        "ret = atcphy_configure(atcphy, target_mode);\n\tif (ret)\n\t\treturn ret;\n\tif (atcphy->t8103_usb3_retry)",
        "atcphy->t8103_usb3_retry = false;\n\tret = atcphy_configure(atcphy, target_mode);\n\tif (ret)\n\t\treturn ret;\n\tif (atcphy->t8103_usb3_retry)"),
    "sticky-retry-not-cleared-on-nonusb3-commit": ("atcphy_mux_set_locked",
        "atcphy->t8103_usb3_retry = false;", "(void)0;"),
    "sticky-retry-not-cleared-on-reset": ("atcphy_dwc3_reset_assert",
        "atcphy->t8103_usb3_retry = false;", "(void)0;"),
    "sticky-retry-not-cleared-on-poweroff": ("atcphy_usb3_power_off",
        "atcphy->t8103_usb3_retry = false;", "(void)0;"),
    "sticky-retry-not-cleared-on-known-reset-setup": ("void atcphy_setup_pipehandler",
        "atcphy->t8103_usb3_retry = false;", "(void)0;"),
}


def run(code, cc, directory, name):
    cfile = directory / (name + ".c")
    binary = directory / name
    cfile.write_text(code)
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-Wall", "-Wextra",
                            "-Wno-unused-parameter", "-Wno-unused-function",
                            str(cfile), "-o", str(binary)], text=True, capture_output=True)
    if build.returncode:
        raise RuntimeError("compile failed for " + name + ":\n" + build.stderr)
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1")
    return subprocess.run([str(binary)], text=True, capture_output=True, env=env)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path,
                        default=Path(__file__).resolve().parents[2] / "drivers/phy/apple/atc.c")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--mutations", action="store_true")
    args = parser.parse_args()
    code = harness(args.source.read_text())
    results = {}
    with tempfile.TemporaryDirectory(prefix="atc-usb3-tests-") as tmp:
        directory = Path(tmp)
        result = run(code, args.cc, directory, "actual")
        if result.returncode:
            raise SystemExit(result.stderr or result.stdout)
        print(result.stdout, end="")
        if args.mutations:
            for name, mutation in MUTATIONS.items():
                if len(mutation) == 3:
                    function, before, after = mutation
                    declaration = function if " " in function else "int " + function
                    old_scope = block(code, "static " + declaration + "(")
                else:
                    before, after = mutation
                    old_scope = code
                if old_scope.count(before) != 1:
                    raise RuntimeError(name + ": expected one mutation site")
                changed = old_scope.replace(before, after, 1)
                mutated = code.replace(old_scope, changed, 1)
                result = run(mutated, args.cc, directory, name)
                if result.returncode == 0:
                    raise RuntimeError("mutation survived: " + name)
                results[name] = "caught"
            print(json.dumps({"mutations": results}, sort_keys=True))


if __name__ == "__main__":
    main()
