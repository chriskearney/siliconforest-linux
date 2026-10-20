// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * DPMS and system sleep panel power on the J613 (panel_power), included at
 * the end of iomfb_v14_7.c so that the production iomfb_v14_7_poweroff(),
 * iomfb_v14_7_poweron(), iomfb_v14_7_set_lid(), the present path, the mailbox
 * hold and the internal callback dispatcher run. Only the link is faked
 * (dcp_v14_link_rpc()) and the two RTKit receive calls: the fake answers
 * swaps, A475 and A472 as the firmware does, logs them in order, and refuses
 * (a status) or fails (a transport error) one call on request.
 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>

#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>

#include <kunit/device.h>
#include <kunit/static_stub.h>
#include <kunit/test.h>

enum v14_pp_event {
	V14_PP_NONE,
	V14_PP_BLACK,		/* A407 without a surface */
	V14_PP_FRAME,		/* A407 with a surface */
	V14_PP_LID_OPEN,	/* A475(1) */
	V14_PP_LID_CLOSED,	/* A475(2) */
	V14_PP_OFF,		/* A472(0) */
	V14_PP_ON,		/* A472(1) */
};

#define V14_PP_MAX_EVENTS	16
#define V14_PP_IOVA0		0x10000000ULL
#define V14_PP_IOVA1		0x20000000ULL

struct v14_pp_fb {
	struct drm_framebuffer fb;
	struct drm_gem_dma_object obj;
};

struct v14_pp {
	struct kunit *test;
	struct apple_dcp *dcp;
	struct apple_dcp_v14 *v14;
	struct v14_pp_fb fbs[2];
	unsigned int frees, holds, resumes, pumps;
	u32 swap_id;
	/* Every call the fake answered, refused or failed, in order. */
	enum v14_pp_event events[V14_PP_MAX_EVENTS];
	unsigned int count;
	/* The next call of this kind is refused with a status, or fails. */
	enum v14_pp_event refuse, fail;
	bool fail_start;
	/* The locks held while A472(0) was answered. */
	bool off_present_lock, off_lock;
	/* Runs as a black frame is answered, with its locks held. */
	void (*on_black)(struct v14_pp *s);
	/*
	 * A present from a thread of its own (v14_pp_concurrent_present()).
	 * Once the thread was created the cleanup waits for it to end, also
	 * after a failed assertion: it uses this state until then.
	 */
	struct completion presenter_started, presenter_done;
	bool presenter_created, presented;
};

static struct v14_pp *v14_pp_current;

static enum v14_pp_event v14_pp_classify(u32 tag, const u8 *in, u32 in_size, u32 out_size)
{
	if (tag == A(407) && in_size == DCP_V14_SWAP_SIZE && out_size == 12)
		return get_unaligned_le64(in + DCP_V14_SWAP_IOVA(0)) ? V14_PP_FRAME : V14_PP_BLACK;
	if (tag == A(475) && in_size == 4 && out_size == 0)
		return get_unaligned_le32(in) == 2 ? V14_PP_LID_CLOSED : V14_PP_LID_OPEN;
	if (tag == A(472) && in_size == 12 && out_size == 8)
		return get_unaligned_le64(in) ? V14_PP_ON : V14_PP_OFF;
	return V14_PP_NONE;
}

static int v14_pp_link_rpc(struct apple_dcp_v14 *v14, u32 tag, const void *input, u32 in_size,
			   void *output, u32 out_size, u32 completion,
			   dcp_v14_admit_fn admit, void *admit_cookie, bool *vetoed)
{
	struct v14_pp *s = v14_pp_current;
	struct kunit *test = s->test;
	enum v14_pp_event event;
	const u8 *in = input;
	u8 *out = output;

	*vetoed = false;
	if (admit && !admit(admit_cookie)) {
		*vetoed = true;
		return -EAGAIN;
	}
	/* Swap start: a swap ID, status 0. */
	if (tag == A(406) && in_size == 16 && out_size == 8) {
		if (s->fail_start) {
			s->fail_start = false;
			return -ETIMEDOUT;
		}
		put_unaligned_le32(++s->swap_id, out);
		put_unaligned_le32(0, out + 4);
		return 0;
	}
	event = v14_pp_classify(tag, in, in_size, out_size);
	if (event == V14_PP_NONE) {
		KUNIT_FAIL(test, "unexpected call %#x %u/%u", tag, in_size, out_size);
		return -EPROTO;
	}
	/* The wire layouts: A475 one u32 of 1 or 2, A472 a u64 bool and four zero bytes. */
	if (tag == A(475))
		KUNIT_EXPECT_TRUE(test, get_unaligned_le32(in) == 1 || get_unaligned_le32(in) == 2);
	if (tag == A(472)) {
		KUNIT_EXPECT_LE(test, get_unaligned_le64(in), 1ULL);
		KUNIT_EXPECT_EQ(test, get_unaligned_le32(in + 8), 0U);
	}
	if (event == V14_PP_OFF) {
		s->off_present_lock = mutex_is_locked(&v14->present_lock);
		s->off_lock = mutex_is_locked(&v14->lock);
	}
	if (s->count < V14_PP_MAX_EVENTS)
		s->events[s->count] = event;
	s->count++;
	if (out_size)
		memset(out, 0, out_size);
	if (s->fail == event) {
		s->fail = V14_PP_NONE;
		return -ETIMEDOUT;
	}
	if (s->refuse == event) {
		s->refuse = V14_PP_NONE;
		/* The status: byte 4 of the A472 reply, byte 5 of the A407 one. */
		if (tag == A(472))
			put_unaligned_le32(1, out + 4);
		else if (tag == A(407))
			put_unaligned_le32(1, out + 5);
	}
	if (event == V14_PP_BLACK && s->on_black)
		s->on_black(s);
	return 0;
}

static void v14_pp_rx_hold(struct apple_dcp_v14 *v14)
{
	v14_pp_current->holds++;
}

static void v14_pp_rx_resume(struct apple_dcp_v14 *v14)
{
	v14_pp_current->resumes++;
}

static void v14_pp_pump(struct work_struct *work)
{
	v14_pp_current->pumps++;
}

static void v14_pp_fb_free(struct kref *kref)
{
	v14_pp_current->frees++;
}

static void v14_pp_stop(void *data)
{
	struct v14_pp *s = data;

	if (s->presenter_created)
		wait_for_completion(&s->presenter_done);
	cancel_work_sync(&s->v14->idle_work);
}

/* A started internal panel session on @board, its panel on, no frame shown yet. */
static struct v14_pp *v14_pp_setup(struct kunit *test, const struct dcp_v14_board *board)
{
	struct v14_pp *s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	static atomic_t sessions;
	struct device *dev;
	unsigned int i;
	char *name;

	KUNIT_ASSERT_NOT_NULL(test, s);
	/*
	 * One device, and so one driver name, per session: a case may set up
	 * more. Allocated first, the name is freed after the device.
	 */
	name = kunit_kzalloc(test, 32, GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, name);
	snprintf(name, 32, "dcp-v14-pp-%d", atomic_inc_return(&sessions));
	dev = kunit_device_register(test, name);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, dev);
	s->test = test;
	s->dcp = devm_kzalloc(dev, sizeof(*s->dcp), GFP_KERNEL);
	s->v14 = devm_kzalloc(dev, sizeof(*s->v14), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, s->dcp);
	KUNIT_ASSERT_NOT_NULL(test, s->v14);
	s->dcp->dev = dev;
	s->dcp->fw_compat = DCP_FIRMWARE_V_14_7;
	s->dcp->v14 = s->v14;
	spin_lock_init(&s->dcp->mode_state.lock);
	s->v14->dev = dev;
	s->v14->dcp = s->dcp;
	s->v14->board = board;
	s->v14->started = true;
	s->v14->panel_width = 2560;
	s->v14->panel_height = 1664;
	/* Never dereferenced: the RTKit calls are stubbed. */
	s->v14->rtk = (struct apple_rtkit *)s;
	mutex_init(&s->v14->lock);
	mutex_init(&s->v14->present_lock);
	mutex_init(&s->v14->kept_lock);
	INIT_WORK(&s->v14->idle_work, v14_pp_pump);
	for (i = 0; i < ARRAY_SIZE(s->fbs); i++) {
		struct v14_pp_fb *f = &s->fbs[i];

		f->obj.dma_addr = i ? V14_PP_IOVA1 : V14_PP_IOVA0;
		f->fb.obj[0] = &f->obj.base;
		f->fb.format = drm_format_info(DRM_FORMAT_XRGB8888);
		f->fb.width = 2560;
		f->fb.height = 1600;
		f->fb.pitches[0] = 2560 * 4;
		kref_init(&f->fb.base.refcount);
		f->fb.base.free_cb = v14_pp_fb_free;
	}
	init_completion(&s->presenter_started);
	init_completion(&s->presenter_done);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, v14_pp_stop, s), 0);
	v14_pp_current = s;
	kunit_activate_static_stub(test, dcp_v14_link_rpc, v14_pp_link_rpc);
	kunit_activate_static_stub(test, dcp_v14_rx_hold, v14_pp_rx_hold);
	kunit_activate_static_stub(test, dcp_v14_rx_resume, v14_pp_rx_resume);
	return s;
}

/* The J613 internal pipe with a Type-C route, the lid state as IOMFB was told. */
static struct v14_pp *v14_pp_clamshell(struct kunit *test, u32 lid)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);

	s->dcp->clamshell = true;
	s->v14->clamshell = true;
	s->v14->lid = lid;
	return s;
}

static struct drm_framebuffer *v14_pp_fb(struct v14_pp *s, unsigned int i)
{
	return &s->fbs[i].fb;
}

static unsigned int v14_pp_refs(struct v14_pp *s, unsigned int i)
{
	return kref_read(&s->fbs[i].fb.base.refcount);
}

/* A frame of framebuffer @i below the notch, as the CRTC presents it. */
static bool v14_pp_present(struct v14_pp *s, unsigned int i)
{
	return dcp_v14_present(s->v14, v14_pp_fb(s, i), 2560, 1600, 64, NULL);
}

static void v14_pp_expect_events(struct kunit *test, struct v14_pp *s,
				 const enum v14_pp_event *want, unsigned int n)
{
	unsigned int i;

	KUNIT_EXPECT_EQ(test, s->count, n);
	for (i = 0; i < min3(n, s->count, (unsigned int)V14_PP_MAX_EVENTS); i++)
		KUNIT_EXPECT_EQ_MSG(test, s->events[i], want[i], "call %u of %u", i, n);
	s->count = 0;
}

#define V14_PP_EXPECT(s, ...)							\
	do {									\
		static const enum v14_pp_event want[] = { __VA_ARGS__ };	\
		v14_pp_expect_events(test, s, want, ARRAY_SIZE(want));		\
	} while (0)

#define V14_PP_EXPECT_NONE(s)							\
	do {									\
		KUNIT_EXPECT_EQ(test, (s)->count, 0U);				\
		(s)->count = 0;							\
	} while (0)

/* A frame on screen, then a clean log. */
static void v14_pp_show(struct kunit *test, struct v14_pp *s, unsigned int i)
{
	KUNIT_ASSERT_TRUE(test, v14_pp_present(s, i));
	KUNIT_ASSERT_PTR_EQ(test, s->v14->active_fb, v14_pp_fb(s, i));
	V14_PP_EXPECT(s, V14_PP_FRAME);
}

/*
 * DPMS off and on without a lid switch: the black frame completes, then
 * A472(0), with present_lock and the lock held; on again A475(1), then
 * A472(1), at every power-on, and the colour matrix is sent again. Repeated
 * calls change nothing.
 */
static void v14_pp_dpms_cycle(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);
	struct apple_dcp_v14 *v14 = s->v14;
	unsigned int cycle;

	for (cycle = 0; cycle < 2; cycle++) {
		v14_pp_show(test, s, cycle);
		KUNIT_EXPECT_EQ(test, v14_pp_refs(s, cycle), 2U);
		s->off_present_lock = s->off_lock = false;
		iomfb_v14_7_poweroff(s->dcp);
		V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
		KUNIT_EXPECT_TRUE(test, s->off_present_lock);
		KUNIT_EXPECT_TRUE(test, s->off_lock);
		KUNIT_EXPECT_TRUE(test, v14->panel_off);
		KUNIT_EXPECT_TRUE(test, v14->panel_blank);
		/* The black frame replaced the frame: released. */
		KUNIT_EXPECT_NULL(test, v14->active_fb);
		KUNIT_EXPECT_EQ(test, v14_pp_refs(s, cycle), 1U);

		iomfb_v14_7_poweroff(s->dcp);
		V14_PP_EXPECT_NONE(s);

		v14->ctm_valid = true;
		iomfb_v14_7_poweron(s->dcp);
		V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
		KUNIT_EXPECT_FALSE(test, v14->panel_off);
		KUNIT_EXPECT_FALSE(test, v14->panel_blank);
		KUNIT_EXPECT_FALSE(test, v14->ctm_valid);
		/* No lid switch: what IOMFB was told is not kept, and is told again. */
		KUNIT_EXPECT_EQ(test, v14->lid, 0U);

		iomfb_v14_7_poweron(s->dcp);
		V14_PP_EXPECT_NONE(s);
	}
	KUNIT_EXPECT_FALSE(test, v14->failed);
	KUNIT_EXPECT_EQ(test, s->frees, 0U);
}

static int v14_pp_presenter(void *data)
{
	struct v14_pp *s = data;

	/* The fake link answers on this thread too. */
	current->kunit_test = s->test;
	complete(&s->presenter_started);
	s->presented = v14_pp_present(s, 1);
	current->kunit_test = NULL;
	/* All: the test and the cleanup both wait for it. */
	complete_all(&s->presenter_done);
	return 0;
}

/* A present that arrives while the black frame is answered. */
static void v14_pp_start_presenter(struct v14_pp *s)
{
	struct task_struct *task;

	s->on_black = NULL;
	task = kthread_run(v14_pp_presenter, s, "v14-pp-present");
	if (IS_ERR(task)) {
		KUNIT_FAIL(s->test, "no presenter thread: %ld", PTR_ERR(task));
		complete_all(&s->presenter_done);
		return;
	}
	s->presenter_created = true;
	KUNIT_EXPECT_TRUE(s->test, wait_for_completion_timeout(&s->presenter_started, HZ));
	/* Long enough for it to wait on present_lock. */
	msleep(50);
}

/*
 * A present that comes during DPMS off's black frame waits for the
 * power-off, then finds the panel dark: it never reaches the firmware
 * between the black frame and A472(0), and its framebuffer is not kept.
 */
static void v14_pp_concurrent_present(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);

	v14_pp_show(test, s, 0);
	s->on_black = v14_pp_start_presenter;
	iomfb_v14_7_poweroff(s->dcp);
	KUNIT_ASSERT_TRUE(test, wait_for_completion_timeout(&s->presenter_done, 5 * HZ));
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	KUNIT_EXPECT_FALSE(test, s->presented);
	KUNIT_EXPECT_TRUE(test, s->v14->panel_off);
	KUNIT_EXPECT_NULL(test, s->v14->active_fb);
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 1), 1U);
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 0), 1U);
	KUNIT_EXPECT_EQ(test, s->frees, 0U);
}

/*
 * A black frame that did not complete: no A472(0), the panel is not off,
 * and the frame shown stays pinned. Refused or failed on the wire, the
 * session ends, as for any failed panel swap; while the mailbox is held
 * nothing is sent and the session goes on.
 */
static void v14_pp_failed_black(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);
	struct apple_dcp_v14 *v14 = s->v14;

	v14_pp_show(test, s, 0);
	s->refuse = V14_PP_BLACK;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK);
	KUNIT_EXPECT_TRUE(test, v14->failed);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	KUNIT_EXPECT_TRUE(test, v14->panel_blank);
	KUNIT_EXPECT_PTR_EQ(test, v14->active_fb, v14_pp_fb(s, 0));
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 0), 2U);
	iomfb_v14_7_poweron(s->dcp);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT_NONE(s);

	s = v14_pp_setup(test, &dcp_v14_board_j613);
	v14 = s->v14;
	v14_pp_show(test, s, 0);
	s->fail = V14_PP_BLACK;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK);
	KUNIT_EXPECT_TRUE(test, v14->failed);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	KUNIT_EXPECT_PTR_EQ(test, v14->active_fb, v14_pp_fb(s, 0));
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 0), 2U);

	s = v14_pp_setup(test, &dcp_v14_board_j613);
	v14 = s->v14;
	v14_pp_show(test, s, 0);
	s->fail_start = true;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_TRUE(test, v14->failed);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	KUNIT_EXPECT_PTR_EQ(test, v14->active_fb, v14_pp_fb(s, 0));

	s = v14_pp_setup(test, &dcp_v14_board_j613);
	v14 = s->v14;
	v14_pp_show(test, s, 0);
	v14->rx_held = true;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_FALSE(test, v14->failed);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	KUNIT_EXPECT_TRUE(test, v14->panel_blank);
	KUNIT_EXPECT_PTR_EQ(test, v14->active_fb, v14_pp_fb(s, 0));
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 0), 2U);
	/* Once released, the next DPMS cycle powers it off. */
	v14->rx_held = false;
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 0), 1U);
}

/*
 * A472(0) refused: the panel stays on, black, the session goes on, and the
 * next DPMS off tries again. Failed on the wire: the session ends.
 */
static void v14_pp_refused_off(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);
	struct apple_dcp_v14 *v14 = s->v14;

	v14_pp_show(test, s, 0);
	s->refuse = V14_PP_OFF;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, v14->failed);
	KUNIT_EXPECT_TRUE(test, v14->panel_blank);
	KUNIT_EXPECT_NULL(test, v14->active_fb);
	/* On: nothing to power on. */
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_TRUE(test, v14_pp_present(s, 1));
	V14_PP_EXPECT(s, V14_PP_FRAME);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);

	s = v14_pp_setup(test, &dcp_v14_board_j613);
	v14 = s->v14;
	v14_pp_show(test, s, 0);
	s->fail = V14_PP_OFF;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	KUNIT_EXPECT_TRUE(test, v14->failed);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
}

/*
 * A472(1) refused: the panel stays off and takes no frame, its colour matrix
 * is left as it was, and the next CRTC enable tries again. A475(1) that
 * fails on the wire ends the session before any A472(1).
 */
static void v14_pp_refused_on(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);
	struct apple_dcp_v14 *v14 = s->v14;

	v14_pp_show(test, s, 0);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	v14->ctm_valid = true;
	s->refuse = V14_PP_ON;
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, v14->panel_blank);
	KUNIT_EXPECT_TRUE(test, v14->ctm_valid);
	KUNIT_EXPECT_FALSE(test, v14->failed);
	/* No frame reaches a panel that is off, and none is kept. */
	KUNIT_EXPECT_FALSE(test, v14_pp_present(s, 1));
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_NULL(test, v14->active_fb);
	KUNIT_EXPECT_EQ(test, v14_pp_refs(s, 1), 1U);
	/* DPMS off again: nothing to blank or power off; on again: retried. */
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT_NONE(s);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, v14->ctm_valid);
	KUNIT_EXPECT_TRUE(test, v14_pp_present(s, 1));
	V14_PP_EXPECT(s, V14_PP_FRAME);

	s = v14_pp_setup(test, &dcp_v14_board_j613);
	v14 = s->v14;
	v14_pp_show(test, s, 0);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	s->fail = V14_PP_LID_OPEN;
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN);
	KUNIT_EXPECT_TRUE(test, v14->failed);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
}

/*
 * Other panels keep DPMS as it was: a black frame, the panel left on, no
 * A475 or A472; nor do their dispatchers take the J613's power callbacks.
 */
static void v14_pp_other_boards(struct kunit *test)
{
	static const struct dcp_v14_board *const boards[] = {
		&dcp_v14_board_t6030, &dcp_v14_board_j615, &dcp_v14_board_j516c,
		&dcp_v14_board_j514c,
	};
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(boards); i++) {
		struct v14_pp *s = v14_pp_setup(test, boards[i]);
		struct apple_dcp_v14 *v14 = s->v14;
		u8 in[20] = {}, out[4] = {};

		kunit_info(test, "%s\n", boards[i]->name);
		v14_pp_show(test, s, 0);
		iomfb_v14_7_poweroff(s->dcp);
		V14_PP_EXPECT(s, V14_PP_BLACK);
		KUNIT_EXPECT_FALSE(test, v14->panel_off);
		KUNIT_EXPECT_TRUE(test, v14->panel_blank);
		KUNIT_EXPECT_NULL(test, v14->active_fb);
		iomfb_v14_7_poweron(s->dcp);
		V14_PP_EXPECT_NONE(s);
		KUNIT_EXPECT_FALSE(test, v14->panel_blank);
		KUNIT_EXPECT_EQ(test, v14->lid, 0U);
		KUNIT_EXPECT_FALSE(test, v14->failed);

		mutex_lock(&v14->lock);
		KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(2), NULL, 0, NULL, 0), -EOPNOTSUPP);
		KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(591), in, 20, NULL, 0), -EOPNOTSUPP);
		KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(593), in, 4, NULL, 0), -EOPNOTSUPP);
		in[0] = 2;
		KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(574), in, 4, out, 4), -EOPNOTSUPP);
		mutex_unlock(&v14->lock);
	}
}

/*
 * The J613 panel's power callbacks, without a lid switch: answered on its
 * internal dispatcher, D574 only for a bool, and only in their own shapes.
 */
static void v14_pp_callbacks(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);
	struct apple_dcp_v14 *v14 = s->v14;
	u8 in[20] = {}, out[4] = {};

	mutex_lock(&v14->lock);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(2), NULL, 0, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(591), in, 20, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(593), in, 4, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(574), in, 4, out, 4), 0);
	in[0] = 1;
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(574), in, 4, out, 4), 0);
	in[0] = 2;
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(574), in, 4, out, 4), -EINVAL);
	in[0] = 0;
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(2), in, 4, NULL, 0), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(593), in, 4, out, 4), -EOPNOTSUPP);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(v14, D(574), in, 4, NULL, 0), -EOPNOTSUPP);
	mutex_unlock(&v14->lock);
	KUNIT_EXPECT_EQ(test, s->count, 0U);
	KUNIT_EXPECT_FALSE(test, v14->failed);

	/* The J615 on a pipe with a Type-C route answers them as before. */
	s = v14_pp_setup(test, &dcp_v14_board_j615);
	s->v14->clamshell = true;
	mutex_lock(&s->v14->lock);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(s->v14, D(2), NULL, 0, NULL, 0), 0);
	KUNIT_EXPECT_EQ(test, dcp_v14_callback(s->v14, D(593), in, 4, NULL, 0), 0);
	mutex_unlock(&s->v14->lock);
}

/*
 * With a lid switch: DPMS off with the lid open powers the panel off, but
 * leaves the pipe the panel's; closing the lid then frees it with A475(2)
 * only. Opening it while the CRTC is off does not power the panel on; the
 * CRTC enable does, with A472(1) only, IOMFB knowing the lid is open. With
 * the lid closed a CRTC enable leaves the panel off until the lid opens.
 */
static void v14_pp_lid(struct kunit *test)
{
	struct v14_pp *s = v14_pp_clamshell(test, 1);
	struct apple_dcp_v14 *v14 = s->v14;

	v14_pp_show(test, s, 0);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, iomfb_v14_7_clamshell_panel_off(s->dcp));

	iomfb_v14_7_set_lid(s->dcp, true);
	V14_PP_EXPECT(s, V14_PP_LID_CLOSED);
	KUNIT_EXPECT_EQ(test, v14->lid, 2U);
	KUNIT_EXPECT_TRUE(test, iomfb_v14_7_clamshell_panel_off(s->dcp));

	iomfb_v14_7_set_lid(s->dcp, false);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_TRUE(test, v14->panel_blank);
	KUNIT_EXPECT_FALSE(test, iomfb_v14_7_clamshell_panel_off(s->dcp));

	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_ON);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);

	/* DPMS off, the lid closed, then the CRTC on: the panel stays off. */
	v14_pp_show(test, s, 1);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	iomfb_v14_7_set_lid(s->dcp, true);
	V14_PP_EXPECT(s, V14_PP_LID_CLOSED);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, v14->panel_blank);
	KUNIT_EXPECT_FALSE(test, v14_pp_present(s, 0));
	V14_PP_EXPECT_NONE(s);
	/* The lid opens with the CRTC on: on, after A475(1). */
	iomfb_v14_7_set_lid(s->dcp, false);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);

	/* The lid closing with the CRTC on is as before. */
	v14_pp_show(test, s, 0);
	s->dcp->clamshell_crtc_on = true;
	iomfb_v14_7_set_lid(s->dcp, true);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_LID_CLOSED, V14_PP_OFF);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, v14->panel_blank);
	KUNIT_EXPECT_FALSE(test, v14->failed);
}

/*
 * A lid switch whose state was not read yet: the first power-on tells IOMFB
 * the lid is open and keeps that; the next one sends A472(1) only.
 */
static void v14_pp_lid_unread(struct kunit *test)
{
	struct v14_pp *s = v14_pp_clamshell(test, 0);
	struct apple_dcp_v14 *v14 = s->v14;

	v14_pp_show(test, s, 0);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
	KUNIT_EXPECT_EQ(test, v14->lid, 1U);
	v14_pp_show(test, s, 1);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_ON);
	/* The lid work then finds IOMFB told: nothing to send. */
	iomfb_v14_7_set_lid(s->dcp, false);
	V14_PP_EXPECT_NONE(s);
}

/*
 * The pipe driving the display on its Type-C route: DPMS powers that
 * display, not the panel, which stays as it was; no black frame.
 */
static void v14_pp_typec(struct kunit *test)
{
	struct v14_pp *s = v14_pp_clamshell(test, 2);
	struct apple_dcp_v14 *v14 = s->v14;

	v14->panel_off = true;
	v14->panel_blank = true;
	v14->typec = true;
	v14->powered = true;
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_OFF);
	KUNIT_EXPECT_FALSE(test, v14->powered);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_TRUE(test, v14->panel_blank);
}

/*
 * System sleep: the CRTC is switched off before the mailbox is held, and on
 * after it is released. A CRTC enable while it is still held sends nothing
 * and leaves the panel off, for the next enable to power on.
 */
static void v14_pp_sleep(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);
	struct apple_dcp_v14 *v14 = s->v14;

	v14_pp_show(test, s, 0);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	iomfb_v14_7_hold_rx(s->dcp);
	KUNIT_EXPECT_EQ(test, s->holds, 1U);
	KUNIT_EXPECT_TRUE(test, v14->rx_held);
	iomfb_v14_7_release_rx(s->dcp);
	flush_work(&v14->idle_work);
	KUNIT_EXPECT_EQ(test, s->resumes, 1U);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);

	v14_pp_show(test, s, 1);
	iomfb_v14_7_poweroff(s->dcp);
	V14_PP_EXPECT(s, V14_PP_BLACK, V14_PP_OFF);
	iomfb_v14_7_hold_rx(s->dcp);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_TRUE(test, v14->panel_off);
	KUNIT_EXPECT_FALSE(test, v14->failed);
	iomfb_v14_7_release_rx(s->dcp);
	flush_work(&v14->idle_work);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT(s, V14_PP_LID_OPEN, V14_PP_ON);
	KUNIT_EXPECT_FALSE(test, v14->panel_off);
}

/* A failed or unstarted session sends nothing. */
static void v14_pp_inactive(struct kunit *test)
{
	struct v14_pp *s = v14_pp_setup(test, &dcp_v14_board_j613);

	s->v14->failed = true;
	iomfb_v14_7_poweroff(s->dcp);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_FALSE(test, s->v14->panel_off);

	s = v14_pp_setup(test, &dcp_v14_board_j613);
	s->v14->started = false;
	iomfb_v14_7_poweroff(s->dcp);
	iomfb_v14_7_poweron(s->dcp);
	V14_PP_EXPECT_NONE(s);
	KUNIT_EXPECT_FALSE(test, s->v14->panel_off);
}

static struct kunit_case v14_pp_cases[] = {
	KUNIT_CASE(v14_pp_dpms_cycle),
	KUNIT_CASE(v14_pp_concurrent_present),
	KUNIT_CASE(v14_pp_failed_black),
	KUNIT_CASE(v14_pp_refused_off),
	KUNIT_CASE(v14_pp_refused_on),
	KUNIT_CASE(v14_pp_other_boards),
	KUNIT_CASE(v14_pp_callbacks),
	KUNIT_CASE(v14_pp_lid),
	KUNIT_CASE(v14_pp_lid_unread),
	KUNIT_CASE(v14_pp_typec),
	KUNIT_CASE(v14_pp_sleep),
	KUNIT_CASE(v14_pp_inactive),
	{}
};

static struct kunit_suite v14_pp_suite = {
	.name = "dcp_v14_panel_power",
	.test_cases = v14_pp_cases,
};

kunit_test_suite(v14_pp_suite);
