/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * Apple T6030 (M3 Pro) ATC: native DisplayPort on the USB-C ports
 *
 * The Type-C mux installs the negotiated DisplayPort mode: pin C or E
 * (APPLE_ATCPHY_MODE_DP, four lanes on both physical pairs) or pin D
 * (APPLE_ATCPHY_MODE_USB3_DP, two lanes on the pair USB3 does not use:
 * pair 1 in normal orientation, pair 0 flipped). The display controller then
 * drives the link through the DP PHY:
 *
 * - power_on takes a display hold on the installed mode and orientation;
 *   while it is held, Type-C mode and orientation changes and the USB3
 *   power-off are recorded, not applied, so the display can park its route
 *   on a PHY that is still powered. power_off (with the link stopped and AUX
 *   off) drops the hold and applies what Type-C asked for meanwhile.
 * - set_mode(PHY_MODE_DP) turns AUX on, set_mode(PHY_MODE_INVALID) stops the
 *   main link if it runs and turns AUX off.
 * - configure sets the link rate (RBR/HBR/HBR2/HBR3: the shared PLL, then
 *   TX and RX of each DP pair), the lane count (the mode's width or zero)
 *   and the drive levels; the caller parks its display crossbar around rate
 *   changes. validate reports the mode's width and HBR3.
 *
 * The PLL rows, lane sequences and drive presets are the same on every
 * T6030 ATC; what differs per ATC is its calibration (AUSCMN_DIG,
 * AUSPLL_CORE, AUX_TOP, AUSCMN_SHM, CLKMON_CFG of the ATC's ADT node), which
 * the Type-C side applies with its common tunables on every mode change.
 *
 * An ATC wired as a fixed four-lane DisplayPort transmitter instead of a
 * USB-C port ("apple,t6030-atcphy-dp": the J514S built-in HDMI's ATC3, into
 * the HDMI converter) has no Type-C side. Its DP PHY is the only PHY:
 * power_on powers the PHY up cold (power and reset, the boot loader's
 * calibration, the fixed lane mode on both pairs, common block) and gives
 * the same link interface on both pairs; power_off powers it down again.
 * Nothing is touched at probe.
 *
 * All accesses are u32 read-modify-writes of the core registers, in the
 * order and with the delays and acknowledgement bounds the hardware needs;
 * a register "of a pair" lies at its pair-0 offset + pair * 0x7000. A
 * failed step stops the sequence and keeps the hold.
 */
#ifndef _ATC_T6030_DP_H
#define _ATC_T6030_DP_H

#define T6030_DP_PAIR(p, offset)	((offset) + (p) * 0x7000)
#define for_each_t6030_dp_pair(p, pairs) \
	for (p = 0; p < 2; p++) if ((pairs) & BIT(p))

/* Firmware link-rate codes: RBR, HBR, HBR2, HBR3. */
#define T6030_DP_RBR	0x06
#define T6030_DP_HBR	0x0a
#define T6030_DP_HBR2	0x14
#define T6030_DP_HBR3	0x1e

/* R(offset, clear, set): keep the bits outside clear, then OR set. */
static void t6030_dp_rmw(struct apple_atcphy *atcphy, u32 offset, u32 clear, u32 set)
{
	void __iomem *reg = atcphy->regs.core + offset;

	writel((readl(reg) & ~clear) | set, reg);
}

static bool atcphy_t6030_dp(const struct apple_atcphy *atcphy)
{
	return atcphy->hw->has_t6030_dp;
}

/* ---- AUX, separate from the main link ---- */

static int t6030_dp_aux_on(struct apple_atcphy *atcphy)
{
	t6030_dp_rmw(atcphy, 0x16400, 0, 0x3);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x16400, 0, 0xc);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x16400, 0x300, 0x100);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x16000, 0x1, 0);
	udelay(1);
	if ((core_read32(atcphy, 0x16400) & 0x30f) != 0x10f ||
	    (core_read32(atcphy, 0x16000) & 0x1))
		return -EIO;
	return 0;
}

static int t6030_dp_aux_off(struct apple_atcphy *atcphy)
{
	t6030_dp_rmw(atcphy, 0x16000, 0, 0x1);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x16400, 0, 0x200);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x16400, 0x8, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x16400, 0x2, 0);
	udelay(1);
	if ((core_read32(atcphy, 0x16400) & 0x20a) != 0x200 ||
	    !(core_read32(atcphy, 0x16000) & 0x1))
		return -EIO;
	return 0;
}

/* ---- Shared PLL ---- */

static int t6030_dp_pll_row(u32 rate, u32 *w)
{
	switch (rate) {
	case T6030_DP_RBR:
		w[0] = 0x1e0e021c; w[1] = 0x00000000; w[2] = 0x156600;
		return 0;
	case T6030_DP_HBR:
		w[0] = 0x1e0e01c2; w[1] = 0x07fffffe; w[2] = 0x155200;
		return 0;
	case T6030_DP_HBR2:
		w[0] = 0x1e0e01c2; w[1] = 0x07fffffe; w[2] = 0x554800;
		return 0;
	case T6030_DP_HBR3:
		w[0] = 0x1e0e02a3; w[1] = 0x0bff7ffc; w[2] = 0x564800;
		return 0;
	}
	return -EINVAL;
}

/*
 * The APB request handshake on 0x2000: flip bit 0 against the acknowledge
 * bit 1 (a), wait for the acknowledge to follow, and return a for the second
 * phase.
 */
static int t6030_dp_pll_request(struct apple_atcphy *atcphy, unsigned int bound_us)
{
	u32 old = core_read32(atcphy, 0x2000), a = (old >> 1) & 1, v;
	int ret;

	writel((old & 0xe0000006) | 0x10000000 | (a ^ 1), atcphy->regs.core + 0x2000);
	ret = readl_poll_timeout_atomic(atcphy->regs.core + 0x2000, v,
					((v >> 1) & 1) == (a ^ 1), 1, bound_us);
	return ret ? ret : a;
}

static int t6030_dp_pll_prepare(struct apple_atcphy *atcphy, unsigned int pairs, u32 rate)
{
	unsigned int p;
	u32 w[3], v;
	int a, ret;

	ret = t6030_dp_pll_row(rate, w);
	if (ret)
		return ret;
	t6030_dp_rmw(atcphy, 0x2234, 0x3, 0);
	t6030_dp_rmw(atcphy, 0x7000, 0x1ff0, 0x490);
	writel(w[0], atcphy->regs.core + 0x2080);
	writel(w[1], atcphy->regs.core + 0x2084);
	writel(w[2], atcphy->regs.core + 0x2088);
	t6030_dp_rmw(atcphy, 0x2208, 0x1f0000, 0x70000);
	t6030_dp_rmw(atcphy, 0x2218, 0x1, 0x1);
	t6030_dp_rmw(atcphy, 0x2200, 0x8, 0x8);
	a = t6030_dp_pll_request(atcphy, 30);
	if (a < 0)
		return a;
	ret = readl_poll_timeout_atomic(atcphy->regs.core + 0x7034, v, v & BIT(3), 1, 20);
	if (ret)
		return ret;
	t6030_dp_rmw(atcphy, 0x2000, 0x1ffffff9, 0x10014000 | a);
	ret = readl_poll_timeout_atomic(atcphy->regs.core + 0x2000, v,
					((v >> 1) & 1) == (u32)a, 1, 5);
	if (ret)
		return ret;
	for_each_t6030_dp_pair(p, pairs)
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb064), 0xc, 0x8);
	t6030_dp_rmw(atcphy, 0x7000, 0xc, 0xc);
	udelay(1);
	return 0;
}

static int t6030_dp_pll_stop(struct apple_atcphy *atcphy)
{
	u32 v;
	int a, ret;

	t6030_dp_rmw(atcphy, 0x7000, 0xc, 0x8);
	a = t6030_dp_pll_request(atcphy, 500);
	if (a < 0)
		return a;
	t6030_dp_rmw(atcphy, 0x2000, 0x1ffffff9, 0x10000018 | a);
	ret = readl_poll_timeout_atomic(atcphy->regs.core + 0x2000, v,
					((v >> 1) & 1) == (u32)a, 1, 500);
	if (ret)
		return ret;
	ret = readl_poll_timeout_atomic(atcphy->regs.core + 0x7034, v, !(v & BIT(3)), 1, 5);
	if (ret)
		return ret;
	t6030_dp_rmw(atcphy, 0x2200, 0x8, 0);
	return 0;
}

/* ---- Lane power of a pair (common registers, bits indexed by the pair) ---- */

static void t6030_dp_tx_power(struct apple_atcphy *atcphy, unsigned int p, bool on)
{
	if (on) {
		t6030_dp_rmw(atcphy, 0x10c, 0, BIT(4 + p) | BIT(6 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x10c, 0, BIT(0 + p) | BIT(2 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x10c, BIT(8 + p), BIT(10 + p));
		udelay(1);
	} else {
		t6030_dp_rmw(atcphy, 0x10c, 0, BIT(8 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x10c, BIT(0 + p), 0);
		udelay(1);
		t6030_dp_rmw(atcphy, 0x10c, BIT(4 + p), 0);
		udelay(1);
	}
}

static void t6030_dp_rx_power(struct apple_atcphy *atcphy, unsigned int p, bool on)
{
	if (on) {
		t6030_dp_rmw(atcphy, 0xc, 0, BIT(4 + p) | BIT(6 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0xc, 0, BIT(0 + p) | BIT(2 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x8, 0, BIT(10 + p) | BIT(12 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x8, 0, BIT(6 + p) | BIT(8 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x8, BIT(14 + p), BIT(16 + p));
		udelay(1);
	} else {
		t6030_dp_rmw(atcphy, 0x8, 0, BIT(14 + p));
		udelay(1);
		t6030_dp_rmw(atcphy, 0x8, BIT(6 + p), 0);
		udelay(1);
		t6030_dp_rmw(atcphy, 0x8, BIT(10 + p), 0);
		udelay(1);
		t6030_dp_rmw(atcphy, 0xc, BIT(0 + p), 0);
		udelay(1);
		t6030_dp_rmw(atcphy, 0xc, BIT(4 + p), 0);
		udelay(1);
	}
}

/* ---- Main link of a pair: TX and RX prepare, stop ---- */

static void t6030_dp_tx_prepare(struct apple_atcphy *atcphy, unsigned int p, u32 rate)
{
	t6030_dp_tx_power(atcphy, p, true);
	writel(0x0aafb800, atcphy->regs.core + T6030_DP_PAIR(p, 0xd044));
	ndelay(500);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x1000000, 0x1000000);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x4000000, 0x4000000);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x7c0, 0x300);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x6c000, 0);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc0d0), 0x100000, 0x100000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc050), 0xc00f, 0);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc0d4), 0x10000000, 0x10000000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc0d0), 0x7ffff, 0x1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x100000, 0x100000);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x400000, 0x400000);
	udelay(1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd004), 0x3000000, 0x2000000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd004), 0x30000000, 0x30000000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd00c), 0x7000000,
		     rate == T6030_DP_RBR ? 0x5000000 : 0x4000000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd010), 0x600, 0x600);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd01c), 0xc00000, 0x800000);
}

static void t6030_dp_rx_prepare(struct apple_atcphy *atcphy, unsigned int p, u32 rate)
{
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9160), 0x1, 0x1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x91dc), 0x18000, 0x18000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a4), 0x3, 0x2);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a8), 0x18000, 0x10000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb06c), 0x18000, 0x10000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d4), 0x1f1f80, 0x121180);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb064), 0xfc00, 0xb800);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d8), 0x7, 0x4);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0dc), 0x18, 0);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d4), 0xe000, 0xc000);
	ndelay(100);
	t6030_dp_rx_power(atcphy, p, true);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb06c), 0x800, 0x800);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x91d4), 0x3, 0x1);
	ndelay(10);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x91d4), 0x2, 0x2);
	/* The set bits deliberately include one outside the cleared field. */
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb06c), 0x600, 0xc00);
	ndelay(100);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d4), 0x50000, 0x50000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d8), 0x1, 0x1);
	ndelay(500);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d4), 0x40000, 0);
	ndelay(200);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb068), 0x180, 0x180);
	ndelay(200);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb068), 0x60, 0x60);
	ndelay(200);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d8), 0x3, 0x3);
	udelay(1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d8), 0x3, 0x2);
	udelay(1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d8), 0x3, 0);
	ndelay(750);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a8), 0x60000, 0x60000);
	udelay(1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb06c), 0x6000, 0x4000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9210), 0xc00f, 0);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9248), 0x10000000, 0x10000000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9244), 0x7ffff, 0x1f81);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a8), 0x380000,
		     rate == T6030_DP_RBR ? 0x280000 : 0x200000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a8), 0xc00000, 0x800000);
	ndelay(50);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb030), 0xc00, 0xc00);
	ndelay(100);
}

static void t6030_dp_pair_stop(struct apple_atcphy *atcphy, unsigned int p)
{
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd01c), 0xc00000, 0xc00000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd010), 0x600, 0x400);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd004), 0x30000000, 0x20000000);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x100000, 0);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x400000, 0);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x4000000, 0);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x1000000, 0);
	ndelay(250);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xd044), 0x1000, 0);
	t6030_dp_tx_power(atcphy, p, false);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb030), 0xc00, 0x800);
	ndelay(50);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a8), 0xc00000, 0xc00000);
	ndelay(500);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0a8), 0x60000, 0x40000);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb068), 0x60, 0x40);
	ndelay(200);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb068), 0x180, 0x100);
	ndelay(200);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb0d4), 0x10000, 0);
	ndelay(100);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb06c), 0x600, 0x600);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x91d4), 0x3, 0x1);
	t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xb06c), 0x800, 0);
	t6030_dp_rx_power(atcphy, p, false);
}

/* ---- Drive levels ---- */

/* Packed presets by voltage swing and pre-emphasis level (sum at most 3). */
static const u32 t6030_dp_drive[4][4] = {
	{ 0x3f024, 0x237013, 0x37200b, 0x46e004 },
	{ 0x3f01c, 0x23700b, 0x372004, 0x3f000 },
	{ 0x3f013, 0x237004, 0x3f000, 0x3f000 },
	{ 0x3f004, 0x3f000, 0x3f000, 0x3f000 },
};

/* Member 0 is the pair's RX side, member 1 its TX side. */
static void t6030_dp_drive_member(struct apple_atcphy *atcphy, unsigned int p,
				  unsigned int member, unsigned int voltage, unsigned int pre)
{
	u32 v = t6030_dp_drive[voltage][pre];
	u32 select = ((v & 3) << 14) | ((v >> 2) & 0xf);
	u32 amplitude = (((v >> 6) & 0x3ffff) << 1) | 1;

	if (!member) {
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9210), 0xc00f, select);
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9248), 0x10000000, 0x10000000);
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0x9244), 0x7ffff, amplitude);
	} else {
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc0d0), 0x100000, 0x100000);
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc050), 0xc00f, select);
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc0d4), 0x10000000, 0x10000000);
		t6030_dp_rmw(atcphy, T6030_DP_PAIR(p, 0xc0d0), 0x7ffff, amplitude);
	}
}

/* ---- Link state ---- */

static int t6030_dp_stop(struct apple_atcphy *atcphy)
{
	unsigned int p;
	int ret;

	if (!atcphy->t6030.rate)
		return 0;
	t6030_dp_rmw(atcphy, 0x7000, 0x1, 0x1);
	for_each_t6030_dp_pair(p, atcphy->t6030.pairs)
		t6030_dp_pair_stop(atcphy, p);
	ret = t6030_dp_pll_stop(atcphy);
	if (ret)
		return ret;
	atcphy->t6030.rate = 0;
	return 0;
}

static int t6030_dp_set_rate(struct apple_atcphy *atcphy, unsigned int link_rate)
{
	unsigned int p;
	u32 rate;
	int ret;

	switch (link_rate) {
	case 0:
		rate = 0;
		break;
	case 1620:
		rate = T6030_DP_RBR;
		break;
	case 2700:
		rate = T6030_DP_HBR;
		break;
	case 5400:
		rate = T6030_DP_HBR2;
		break;
	case 8100:
		rate = T6030_DP_HBR3;
		break;
	default:
		return -EINVAL;
	}
	if (!atcphy->t6030.aux)
		return -ENOLINK;
	ret = t6030_dp_stop(atcphy);
	if (ret || !rate)
		return ret;
	ret = t6030_dp_pll_prepare(atcphy, atcphy->t6030.pairs, rate);
	if (ret)
		return ret;
	for_each_t6030_dp_pair(p, atcphy->t6030.pairs) {
		t6030_dp_tx_prepare(atcphy, p, rate);
		t6030_dp_rx_prepare(atcphy, p, rate);
	}
	t6030_dp_rmw(atcphy, 0x7000, 0x1, 0);
	atcphy->t6030.rate = rate;
	return 0;
}

/*
 * One voltage/pre-emphasis request per lane. Pin D: lane 0 drives the pair's
 * RX side, lane 1 its TX side. Pin C: lanes 0/1 and 2/3 are the RX/TX sides
 * of the two pairs, but which pair carries which lanes is not known, so both
 * pairs get the same setting per side: the stronger of the two requests.
 * The programmed levels are written back into @dp.
 */
static int t6030_dp_set_voltages(struct apple_atcphy *atcphy, struct phy_configure_opts_dp *dp)
{
	unsigned int lanes = atcphy->t6030.lanes, p;
	unsigned int level[2][2];

	if (dp->lanes != lanes)
		return -EINVAL;
	for (unsigned int i = 0; i < lanes; i++)
		if (dp->voltage[i] > 3 || dp->pre[i] > 3 || dp->voltage[i] + dp->pre[i] > 3)
			return -EINVAL;
	if (!atcphy->t6030.rate)
		return -ENOLINK;
	for (unsigned int m = 0; m < 2; m++) {
		unsigned int v = dp->voltage[m], pre = dp->pre[m];

		if (lanes == 4) {
			v = max(v, dp->voltage[m + 2]);
			pre = min(max(pre, dp->pre[m + 2]), 3 - v);
		}
		level[m][0] = v;
		level[m][1] = pre;
		for_each_t6030_dp_pair(p, atcphy->t6030.pairs)
			t6030_dp_drive_member(atcphy, p, m, v, pre);
	}
	for (unsigned int i = 0; i < lanes; i++) {
		dp->voltage[i] = level[i & 1][0];
		dp->pre[i] = level[i & 1][1];
	}
	return 0;
}

/* ---- The display hold ---- */

/*
 * Record a Type-C mode for a held route; applied when the hold is dropped.
 * True if the route is held.
 */
static bool atcphy_t6030_dp_defer(struct apple_atcphy *atcphy, enum atcphy_mode mode)
{
	if (!atcphy_t6030_dp(atcphy) || !atcphy->t6030.held)
		return false;
	atcphy->t6030.deferred_mode = mode;
	atcphy->t6030.deferred = true;
	dev_dbg(atcphy->dev, "DP held: mode %u deferred\n", mode);
	return true;
}

/* Likewise for the orientation. */
static bool atcphy_t6030_dp_defer_swap(struct apple_atcphy *atcphy, bool swap_lanes)
{
	if (!atcphy_t6030_dp(atcphy) || !atcphy->t6030.held)
		return false;
	atcphy->t6030.deferred_swap = swap_lanes;
	atcphy->t6030.deferred = true;
	dev_dbg(atcphy->dev, "DP held: orientation %u deferred\n", swap_lanes);
	return true;
}

static bool atcphy_t6030_dp_held(const struct apple_atcphy *atcphy)
{
	/* The flag first: the mux tests run on a PHY without hardware data. */
	return atcphy->t6030.held && atcphy_t6030_dp(atcphy);
}

static int atcphy_t6030_dpphy_power_on(struct phy *phy)
{
	struct apple_atcphy *atcphy = phy_get_drvdata(phy);

	guard(mutex)(&atcphy->lock);
	if (atcphy->t6030.held)
		return -EBUSY;
	if (atcphy->mode != APPLE_ATCPHY_MODE_DP && atcphy->mode != APPLE_ATCPHY_MODE_USB3_DP)
		return -ENOLINK;
	/* The installed mode must be complete: no pending transition, PHY up. */
	if (atcphy->mode_dirty || atcphy->pipe_pending || atcphy->usb_off_pending ||
	    (core_read32(atcphy, ATCPHY_POWER_CTRL) & 0x1f) != 0x1b ||
	    !(core_read32(atcphy, AUS_COMMON_DIG_RCAL1) & AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE))
		return -EBUSY;

	atcphy->t6030.held = true;
	atcphy->t6030.deferred = false;
	atcphy->t6030.deferred_mode = atcphy->mode;
	atcphy->t6030.deferred_swap = atcphy->swap_lanes;
	if (atcphy->mode == APPLE_ATCPHY_MODE_DP) {
		atcphy->t6030.lanes = 4;
		atcphy->t6030.pairs = BIT(0) | BIT(1);
	} else {
		atcphy->t6030.lanes = 2;
		atcphy->t6030.pairs = atcphy->swap_lanes ? BIT(0) : BIT(1);
	}
	atcphy->t6030.lanes_set = 0;
	dev_info(atcphy->dev, "DP held: pin %c, %u lanes, pairs %#x, %s orientation\n",
		 atcphy->t6030.lanes == 4 ? 'C' : 'D', atcphy->t6030.lanes,
		 atcphy->t6030.pairs, atcphy->swap_lanes ? "reverse" : "normal");
	return 0;
}

static int atcphy_t6030_dpphy_power_off(struct phy *phy)
{
	struct apple_atcphy *atcphy = phy_get_drvdata(phy);
	enum atcphy_mode mode;
	int ret;

	guard(mutex)(&atcphy->lock);
	if (!atcphy->t6030.held)
		return 0;
	if (atcphy->t6030.rate || atcphy->t6030.aux)
		return -EBUSY;
	atcphy->t6030.held = false;
	dev_info(atcphy->dev, "DP released%s\n", atcphy->t6030.deferred ? ", applying Type-C" : "");
	if (!atcphy->t6030.deferred)
		return 0;
	atcphy->t6030.deferred = false;

	/* What the Type-C mux and switch asked for while the route was held. */
	mode = atcphy->t6030.deferred_mode;
	if (atcphy->swap_lanes != atcphy->t6030.deferred_swap) {
		atcphy->swap_lanes = atcphy->t6030.deferred_swap;
		atcphy->mode_dirty = true;
	}
	if (atcphy->mode == mode && !atcphy->mode_dirty)
		return 0;
	atcphy->typec_mode = mode;
	if (atcphy->pipe_pending) {
		ret = atcphy_park_pipehandler(atcphy);
		if (ret)
			return ret;
	}
	return atcphy_configure(atcphy, mode);
}

static int atcphy_t6030_dpphy_set_mode(struct phy *phy, enum phy_mode mode, int submode)
{
	struct apple_atcphy *atcphy = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&atcphy->lock);
	if (!atcphy->t6030.held)
		return -ENOLINK;
	switch (mode) {
	case PHY_MODE_DP:
		if (atcphy->t6030.aux)
			return 0;
		ret = t6030_dp_aux_on(atcphy);
		/* An incomplete power-up must still be undone. */
		atcphy->t6030.aux = true;
		break;
	case PHY_MODE_INVALID:
		ret = t6030_dp_stop(atcphy);
		if (ret || !atcphy->t6030.aux)
			break;
		ret = t6030_dp_aux_off(atcphy);
		if (!ret)
			atcphy->t6030.aux = false;
		break;
	default:
		return -EINVAL;
	}
	if (ret)
		dev_err(atcphy->dev, "DP AUX %s failed: %d\n",
			mode == PHY_MODE_DP ? "on" : "off", ret);
	return ret;
}

static int atcphy_t6030_dpphy_configure(struct phy *phy, union phy_configure_opts *opts)
{
	struct apple_atcphy *atcphy = phy_get_drvdata(phy);
	struct phy_configure_opts_dp *dp = &opts->dp;
	int ret = 0;

	guard(mutex)(&atcphy->lock);
	if (!atcphy->t6030.held)
		return -ENOLINK;
	if (dp->set_lanes) {
		if (dp->lanes && dp->lanes != atcphy->t6030.lanes)
			return -EINVAL;
		atcphy->t6030.lanes_set = dp->lanes;
	}
	if (dp->set_rate) {
		if (dp->ssc)
			return -EINVAL;
		ret = t6030_dp_set_rate(atcphy, dp->link_rate);
		dev_info(atcphy->dev, "DP link rate %u: %d\n", dp->link_rate, ret);
		if (ret)
			return ret;
	}
	if (dp->set_voltages) {
		ret = t6030_dp_set_voltages(atcphy, dp);
		if (ret)
			dev_err(atcphy->dev, "DP drive levels refused: %d\n", ret);
	}
	return ret;
}

/* ---- Fixed four-lane DP PHY (no Type-C): cold power-up and power-down ---- */

#define T6030_DP_POWER_IDLE	0x4	/* power control at reset: clamped, all off */

static int t6030_dp_fixed_poll(struct apple_atcphy *atcphy, u32 offset, u32 bit, bool set)
{
	u32 v;

	return readl_poll_timeout(atcphy->regs.core + offset, v, !!(v & bit) == set, 1, 1000);
}

/*
 * Power and reset with the PHY reset held, the boot loader's calibration
 * (each value read back), DP on both pairs in the fixed lane mode (no
 * orientation), PHY reset release, then the common block. AUX and the main
 * link follow through set_mode and configure.
 */
static int t6030_dp_fixed_up(struct apple_atcphy *atcphy)
{
	struct apple_tunable *cal = atcphy->t6030.calibration;
	u32 v;
	int ret;

	if (core_read32(atcphy, ATCPHY_POWER_CTRL) != T6030_DP_POWER_IDLE ||
	    core_read32(atcphy, ATCPHY_POWER_STAT) || core_read32(atcphy, 0x20008)) {
		dev_err(atcphy->dev, "DP PHY not idle: %#x %#x %#x\n",
			core_read32(atcphy, ATCPHY_POWER_CTRL),
			core_read32(atcphy, ATCPHY_POWER_STAT), core_read32(atcphy, 0x20008));
		return -EBUSY;
	}
	ndelay(500);
	t6030_dp_rmw(atcphy, 0x20008, 0, BIT(0));
	udelay(1);
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, 0, ATCPHY_POWER_SLEEP_SMALL);
	udelay(1);
	ret = t6030_dp_fixed_poll(atcphy, ATCPHY_POWER_STAT, ATCPHY_POWER_SLEEP_SMALL, true);
	if (ret)
		return ret;
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, 0, ATCPHY_POWER_SLEEP_BIG);
	udelay(1);
	ret = t6030_dp_fixed_poll(atcphy, ATCPHY_POWER_STAT, ATCPHY_POWER_SLEEP_BIG, true);
	if (ret)
		return ret;
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_CLAMP_EN, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, 0, ATCPHY_POWER_APB_RESET_N);
	udelay(1);
	if (core_read32(atcphy, ATCPHY_POWER_CTRL) != 0xb || core_read32(atcphy, 0x20008) != 1)
		return -EIO;

	for (size_t i = 0; i < cal->sz; i++) {
		t6030_dp_rmw(atcphy, cal->values[i].offset, cal->values[i].mask,
			     cal->values[i].value);
		if ((core_read32(atcphy, cal->values[i].offset) & cal->values[i].mask) !=
		    cal->values[i].value)
			return -EIO;
	}

	/* DP on both pairs (lane mode 3); no orientation on a fixed route. */
	t6030_dp_rmw(atcphy, 0x60, 0xfff, 0x492);
	t6030_dp_rmw(atcphy, 0x64, 0x1f, 0x14);
	t6030_dp_rmw(atcphy, 0x64, 0x1ffe0, 0x22000);
	udelay(10);
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, 0, ATCPHY_POWER_PHY_RESET_N);
	udelay(100);

	/* Common block. */
	t6030_dp_rmw(atcphy, 0x8, 0, 0x4);
	t6030_dp_rmw(atcphy, 0x8, 0, 0x8);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x8, 0, 0x1);
	t6030_dp_rmw(atcphy, 0x8, 0, 0x2);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x8, 0x10, 0x20);
	udelay(1);
	t6030_dp_rmw(atcphy, 0xa00, 0, 0x2);
	udelay(15);
	return readl_poll_timeout_atomic(atcphy->regs.core + AUS_COMMON_DIG_RCAL1, v,
					 v & AUS_COMMON_DIG_RCAL1_ALL_CODES_DONE, 1, 100);
}

/* Main link and AUX stopped: common block off, then power down to the idle image. */
static int t6030_dp_fixed_down(struct apple_atcphy *atcphy)
{
	int ret;

	t6030_dp_rmw(atcphy, 0xa00, 0x2, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x8, 0, 0x10);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x8, 0x1, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x8, 0x4, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_PHY_RESET_N, ATCPHY_POWER_CLAMP_EN);
	udelay(1);
	t6030_dp_rmw(atcphy, 0x20008, 0x5, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_APB_RESET_N, 0);
	udelay(1);
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_SLEEP_BIG, 0);
	udelay(1);
	ret = t6030_dp_fixed_poll(atcphy, ATCPHY_POWER_STAT, ATCPHY_POWER_SLEEP_BIG, false);
	if (ret)
		return ret;
	t6030_dp_rmw(atcphy, ATCPHY_POWER_CTRL, ATCPHY_POWER_SLEEP_SMALL, 0);
	udelay(1);
	ret = t6030_dp_fixed_poll(atcphy, ATCPHY_POWER_STAT, ATCPHY_POWER_SLEEP_SMALL, false);
	if (ret)
		return ret;
	return core_read32(atcphy, ATCPHY_POWER_CTRL) == T6030_DP_POWER_IDLE &&
	       !core_read32(atcphy, ATCPHY_POWER_STAT) && !core_read32(atcphy, 0x20008) ?
	       0 : -EIO;
}

/* Power-up gives the link interface on both pairs, like a pin C hold. */
static int atcphy_t6030_fixed_power_on(struct phy *phy)
{
	struct apple_atcphy *atcphy = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&atcphy->lock);
	if (atcphy->t6030.held)
		return -EBUSY;
	ret = t6030_dp_fixed_up(atcphy);
	if (ret) {
		dev_err(atcphy->dev, "DP PHY power-up failed: %d\n", ret);
		return ret;
	}
	atcphy->t6030.held = true;
	atcphy->t6030.lanes = 4;
	atcphy->t6030.pairs = BIT(0) | BIT(1);
	atcphy->t6030.lanes_set = 0;
	dev_info(atcphy->dev, "DP PHY up: 4 lanes, fixed route\n");
	return 0;
}

static int atcphy_t6030_fixed_power_off(struct phy *phy)
{
	struct apple_atcphy *atcphy = phy_get_drvdata(phy);
	int ret;

	guard(mutex)(&atcphy->lock);
	if (!atcphy->t6030.held)
		return 0;
	if (atcphy->t6030.rate || atcphy->t6030.aux)
		return -EBUSY;
	ret = t6030_dp_fixed_down(atcphy);
	if (ret) {
		dev_err(atcphy->dev, "DP PHY power-down failed: %d\n", ret);
		return ret;
	}
	atcphy->t6030.held = false;
	dev_info(atcphy->dev, "DP PHY down\n");
	return 0;
}

/* Four lanes up to HBR3. */
static int atcphy_t6030_fixed_validate(struct phy *phy, enum phy_mode mode, int submode,
				       union phy_configure_opts *opts)
{
	if (mode != PHY_MODE_DP || submode)
		return -EINVAL;
	opts->dp.lanes = 4;
	opts->dp.link_rate = 8100;
	return 0;
}

static const struct phy_ops apple_atc_t6030_fixed_phy_ops = {
	.owner = THIS_MODULE,
	.power_on = atcphy_t6030_fixed_power_on,
	.power_off = atcphy_t6030_fixed_power_off,
	.configure = atcphy_t6030_dpphy_configure,
	.validate = atcphy_t6030_fixed_validate,
	.set_mode = atcphy_t6030_dpphy_set_mode,
};

static struct phy *atcphy_t6030_fixed_xlate(struct device *dev, const struct of_phandle_args *args)
{
	struct apple_atcphy *atcphy = dev_get_drvdata(dev);

	return args->args[0] == PHY_TYPE_DP ? atcphy->phys.dp : ERR_PTR(-ENODEV);
}

/*
 * The core registers and the calibration only. The PHY's power domain is on
 * while the driver is bound; no register is touched before power_on.
 */
static int atcphy_t6030_fixed_probe(struct platform_device *pdev, struct apple_atcphy *atcphy)
{
	struct device *dev = atcphy->dev;
	struct apple_tunable *cal;
	struct resource *res;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core");
	atcphy->regs.core = devm_ioremap_resource(dev, res);
	if (IS_ERR(atcphy->regs.core))
		return dev_err_probe(dev, PTR_ERR(atcphy->regs.core), "Unable to map core regs\n");
	atcphy->res.core = res;

	cal = devm_apple_tunable_parse(dev, atcphy->np, "apple,tunable-dp-calibration", res);
	if (IS_ERR(cal))
		return dev_err_probe(dev, PTR_ERR(cal), "No DP calibration from the boot loader\n");
	if (!cal->sz)
		return dev_err_probe(dev, -EINVAL, "Empty DP calibration\n");
	for (size_t i = 0; i < cal->sz; i++)
		if (cal->values[i].value & ~cal->values[i].mask)
			return dev_err_probe(dev, -EINVAL, "DP calibration %#x outside its mask\n",
					     cal->values[i].offset);
	atcphy->t6030.calibration = cal;

	atcphy->phys.dp = devm_phy_create(dev, NULL, &apple_atc_t6030_fixed_phy_ops);
	if (IS_ERR(atcphy->phys.dp))
		return PTR_ERR(atcphy->phys.dp);
	phy_set_drvdata(atcphy->phys.dp, atcphy);

	atcphy->phy_provider = devm_of_phy_provider_register(dev, atcphy_t6030_fixed_xlate);
	if (IS_ERR(atcphy->phy_provider))
		return PTR_ERR(atcphy->phy_provider);
	dev_info(dev, "fixed DP PHY, %zu calibration values\n", cal->sz);
	return 0;
}

#endif /* _ATC_T6030_DP_H */
