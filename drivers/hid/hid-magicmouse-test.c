// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KUnit tests for the MTP and SPI trackpad reports of hid-magicmouse,
 * including the J700 C1FE layout and its palm rejection.
 *
 * Reports go through the driver's raw_event entry into a registered input
 * device set up by the driver itself; the tests read back the slots and
 * keys the input core holds, and count the events a handler receives.
 */

#include <kunit/test.h>

enum mm_board { MM_C1FE, MM_MTP, MM_SPI };

#define MM_TOUCHING	4
#define MM_APPROACH	1
#define MM_LIFTING	5

#define MM_C1FE_SIZE(n)	(MAGICMOUSE_C1FE_HEADER_SIZE + 30 * (n) + \
			 MAGICMOUSE_C1FE_TRAILER_SIZE)
#define MM_MTP_HDR	((int)sizeof(struct tp_header))
#define MM_SPI_HDR	((int)sizeof(struct tp_mouse_report))
#define MM_BUF_SIZE	(MM_SPI_HDR + MM_MTP_HDR + 30 * (MAX_CONTACTS + 1) + 8)

struct mm_test {
	struct kunit *test;
	enum mm_board board;
	struct hid_device *hdev;
	struct magicmouse_sc *msc;
	struct input_dev *input;
	struct input_handler handler;
	struct input_device_id ids[2];
	struct input_handle *handle;
	u8 *buf;
	unsigned int events;
	unsigned int pressure_events;
};

/* A contact record; the fields are the raw 16-bit words. */
struct mm_rec {
	u8 id, state, class;
	s16 x, y;
	u16 touch_major, touch_minor, tool_major, tool_minor, orientation;
	u16 pressure;
};

static struct mm_rec mm_finger(u8 id, s16 x, s16 y)
{
	return (struct mm_rec){
		.id = id, .state = MM_TOUCHING, .class = 2, .x = x, .y = y,
		.touch_major = 400, .touch_minor = 300,
		.tool_major = 800, .tool_minor = 600, .orientation = 15000,
	};
}

static struct mm_rec mm_palm(u8 id, s16 x, s16 y)
{
	struct mm_rec r = mm_finger(id, x, y);

	r.class = 1;	/* the controller takes many palm heels for thumbs */
	r.touch_major = 1500;
	r.tool_major = 2600;
	return r;
}

static unsigned int mm_events(struct input_handle *handle,
			      struct input_value *vals, unsigned int count)
{
	struct mm_test *s = handle->handler->private;
	unsigned int i;

	for (i = 0; i < count; i++) {
		s->events++;
		if (vals[i].type == EV_ABS && vals[i].code == ABS_MT_PRESSURE)
			s->pressure_events++;
	}
	return count;
}

static bool mm_match(struct input_handler *handler, struct input_dev *dev)
{
	struct mm_test *s = handler->private;

	return dev == s->input;
}

static int mm_connect(struct input_handler *handler, struct input_dev *dev,
		      const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;
	handle->dev = dev;
	handle->handler = handler;
	handle->name = "magicmouse-kunit";
	((struct mm_test *)handler->private)->handle = handle;

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void mm_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* A transport for the trackpad's open and close to reach; nothing to start. */
static int mm_ll_open(struct hid_device *hdev)
{
	return 0;
}

static void mm_ll_close(struct hid_device *hdev)
{
}

static const struct hid_ll_driver mm_ll_driver = {
	.open = mm_ll_open,
	.close = mm_ll_close,
};

static void mm_unregister_handler(void *handler)
{
	input_unregister_handler(handler);
}

static void mm_unregister_device(void *input)
{
	input_unregister_device(input);
}

static struct mm_test *mm_setup(struct kunit *test, enum mm_board board)
{
	struct mm_test *s;
	struct input_dev *input;
	int ret;

	s = kunit_kzalloc(test, sizeof(*s), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s);
	s->test = test;
	s->board = board;
	s->buf = kunit_kzalloc(test, MM_BUF_SIZE, GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s->buf);
	s->hdev = kunit_kzalloc(test, sizeof(*s->hdev), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s->hdev);
	s->msc = kunit_kzalloc(test, sizeof(*s->msc), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, s->msc);

	s->hdev->bus = board == MM_SPI ? BUS_SPI : BUS_HOST;
	s->hdev->dev.init_name = "magicmouse-kunit";
	s->hdev->ll_driver = &mm_ll_driver;
	s->hdev->driver = &magicmouse_driver;
	mutex_init(&s->hdev->ll_open_lock);
	s->msc->hdev = s->hdev;
	s->msc->mtp_c1fe = board == MM_C1FE;
	if (board == MM_SPI) {
		s->msc->input_ops.raw_event = magicmouse_raw_event_spi;
		s->msc->input_ops.setup_input = magicmouse_setup_input_spi;
	} else {
		s->msc->input_ops.raw_event = magicmouse_raw_event_mtp;
		s->msc->input_ops.setup_input = magicmouse_setup_input_mtp;
	}
	hid_set_drvdata(s->hdev, s->msc);

	input = input_allocate_device();
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, input);
	input->name = "magicmouse-kunit";
	input->id.bustype = s->hdev->bus;
	input_set_drvdata(input, s->hdev);
	s->msc->input = input;

	/* What hid-input may have mapped from the report descriptor. */
	__set_bit(EV_ABS, input->evbit);
	__set_bit(ABS_PRESSURE, input->absbit);
	__set_bit(ABS_MT_PRESSURE, input->absbit);
	__set_bit(ABS_TOOL_WIDTH, input->absbit);

	ret = magicmouse_setup_input(input, s->hdev);
	if (ret) {
		input_free_device(input);
		KUNIT_FAIL_AND_ABORT(test, "setup_input: %d", ret);
	}
	/*
	 * The handler opens the device through the driver's open, which has
	 * no dimensions to query here. An SPI trackpad's open would send it
	 * the multitouch enable, so that one is left out.
	 */
	s->msc->query_dimensions = false;
	if (board == MM_SPI) {
		input->open = NULL;
		input->close = NULL;
	}

	ret = input_register_device(input);
	if (ret) {
		input_free_device(input);
		KUNIT_FAIL_AND_ABORT(test, "input_register_device: %d", ret);
	}
	s->input = input;
	ret = kunit_add_action_or_reset(test, mm_unregister_device, input);
	KUNIT_ASSERT_EQ(test, ret, 0);

	/* Any device with absolute axes; mm_match picks this one. */
	s->ids[0].flags = INPUT_DEVICE_ID_MATCH_EVBIT;
	__set_bit(EV_ABS, s->ids[0].evbit);
	s->handler.private = s;
	s->handler.name = "magicmouse-kunit";
	s->handler.events = mm_events;
	s->handler.match = mm_match;
	s->handler.connect = mm_connect;
	s->handler.disconnect = mm_disconnect;
	s->handler.id_table = s->ids;
	ret = input_register_handler(&s->handler);
	KUNIT_ASSERT_EQ(test, ret, 0);
	ret = kunit_add_action_or_reset(test, mm_unregister_handler, &s->handler);
	KUNIT_ASSERT_EQ(test, ret, 0);
	KUNIT_ASSERT_EQ(test, input->users, 1U);
	return s;
}

static void mm_put_rec(u8 *p, const struct mm_rec *r)
{
	struct tp_finger *f = (struct tp_finger *)p;

	memset(f, 0, sizeof(*f));
	f->unknown1 = cpu_to_le16(r->state << 8 | r->id);
	f->unknown2 = cpu_to_le16(r->class);
	f->abs_x = cpu_to_le16(r->x);
	f->abs_y = cpu_to_le16(r->y);
	f->tool_major = cpu_to_le16(r->tool_major);
	f->tool_minor = cpu_to_le16(r->tool_minor);
	f->orientation = cpu_to_le16(r->orientation);
	f->touch_major = cpu_to_le16(r->touch_major);
	f->touch_minor = cpu_to_le16(r->touch_minor);
	f->pressure = cpu_to_le16(r->pressure);
}

/* Builds a report for the board in s->buf and returns its size. */
static int mm_build(struct mm_test *s, const struct mm_rec *recs, int n,
		    u8 buttons)
{
	u8 *p = s->buf;
	int i, hdr;

	memset(s->buf, 0, MM_BUF_SIZE);
	if (s->board == MM_C1FE) {
		p[0] = MAGICMOUSE_C1FE_REPORT_ID;
		p[2] = MAGICMOUSE_C1FE_HEADER_SIZE;
		p[3] = 4;
		put_unaligned_le32(30 * n, p + 16);
		put_unaligned_le16(MAGICMOUSE_C1FE_TRAILER_SIZE, p + 20);
		hdr = MAGICMOUSE_C1FE_HEADER_SIZE;
	} else {
		if (s->board == MM_SPI) {
			p[0] = SPI_REPORT_ID;
			p += MM_SPI_HDR;
		}
		hdr = MM_MTP_HDR;
	}
	p[22] = n;
	p[23] = buttons;
	for (i = 0; i < n; i++)
		mm_put_rec(p + hdr + 30 * i, &recs[i]);

	if (s->board == MM_C1FE)
		return MM_C1FE_SIZE(n);
	return (p - s->buf) + hdr + 30 * n;
}

static int mm_send_raw(struct mm_test *s, int size)
{
	return magicmouse_raw_event(s->hdev, NULL, s->buf, size);
}

/*
 * Sends one well-formed report; the driver must take it. The input core
 * passes nothing on for a report that changes nothing.
 */
static void mm_send(struct mm_test *s, const struct mm_rec *recs, int n,
		    u8 buttons)
{
	KUNIT_ASSERT_EQ(s->test, mm_send_raw(s, mm_build(s, recs, n, buttons)), 1);
}

static void mm_send1(struct mm_test *s, struct mm_rec r)
{
	mm_send(s, &r, 1, 0);
}

static void mm_send0(struct mm_test *s)
{
	mm_send(s, NULL, 0, 0);
}

static struct input_mt_slot *mm_slot(struct mm_test *s, s16 x, s16 y)
{
	struct input_mt *mt = s->input->mt;
	int i;

	for (i = 0; i < mt->num_slots; i++) {
		struct input_mt_slot *slot = &mt->slots[i];

		if (input_mt_get_value(slot, ABS_MT_TRACKING_ID) >= 0 &&
		    input_mt_get_value(slot, ABS_MT_POSITION_X) == x &&
		    input_mt_get_value(slot, ABS_MT_POSITION_Y) == -y)
			return slot;
	}
	return NULL;
}

static int mm_active(struct mm_test *s)
{
	struct input_mt *mt = s->input->mt;
	int i, n = 0;

	for (i = 0; i < mt->num_slots; i++)
		n += input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) >= 0;
	return n;
}

/* The value of an axis for the contact reported at (x, y). */
static int mm_get(struct mm_test *s, s16 x, s16 y, unsigned int code)
{
	struct input_mt_slot *slot = mm_slot(s, x, y);

	KUNIT_ASSERT_NOT_NULL_MSG(s->test, slot, "no contact at %d,%d", x, y);
	return input_mt_get_value(slot, code);
}

static int mm_tool(struct mm_test *s, s16 x, s16 y)
{
	return mm_get(s, x, y, ABS_MT_TOOL_TYPE);
}

static int mm_tid(struct mm_test *s, s16 x, s16 y)
{
	return mm_get(s, x, y, ABS_MT_TRACKING_ID);
}

static void mm_expect_range(struct kunit *test, struct input_dev *input,
			    unsigned int code, int min, int max)
{
	KUNIT_EXPECT_TRUE_MSG(test, test_bit(code, input->absbit), "abs %u", code);
	KUNIT_EXPECT_EQ_MSG(test, input_abs_get_min(input, code), min, "abs %u", code);
	KUNIT_EXPECT_EQ_MSG(test, input_abs_get_max(input, code), max, "abs %u", code);
}

/* The J700 trackpad advertises its areas, orientation and tool type, no pressure. */
static void mm_c1fe_caps(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct input_dev *input = s->input;

	mm_expect_range(test, input, ABS_MT_TOUCH_MAJOR, 0, 2 * S16_MAX);
	mm_expect_range(test, input, ABS_MT_TOUCH_MINOR, 0, 2 * S16_MAX);
	mm_expect_range(test, input, ABS_MT_WIDTH_MAJOR, 0, 2 * S16_MAX);
	mm_expect_range(test, input, ABS_MT_WIDTH_MINOR, 0, 2 * S16_MAX);
	mm_expect_range(test, input, ABS_MT_ORIENTATION, -16384, 16384);
	mm_expect_range(test, input, ABS_MT_TOOL_TYPE, 0, MT_TOOL_MAX);
	KUNIT_EXPECT_FALSE(test, test_bit(ABS_MT_PRESSURE, input->absbit));
	KUNIT_EXPECT_FALSE(test, test_bit(ABS_PRESSURE, input->absbit));
	KUNIT_EXPECT_FALSE(test, test_bit(ABS_TOOL_WIDTH, input->absbit));
	KUNIT_EXPECT_FALSE(test, test_bit(INPUT_PROP_PRESSUREPAD, input->propbit));
	KUNIT_EXPECT_TRUE(test, test_bit(INPUT_PROP_BUTTONPAD, input->propbit));
}

/* The other MTP and SPI trackpads advertise what they did before palm rejection. */
static void mm_other_caps(struct kunit *test)
{
	enum mm_board boards[] = { MM_MTP, MM_SPI };
	int i;

	for (i = 0; i < ARRAY_SIZE(boards); i++) {
		struct input_dev *input = mm_setup(test, boards[i])->input;

		mm_expect_range(test, input, ABS_MT_TOUCH_MAJOR, 0, 5000);
		mm_expect_range(test, input, ABS_MT_TOUCH_MINOR, 0, 5000);
		mm_expect_range(test, input, ABS_MT_WIDTH_MAJOR, 0, 5000);
		mm_expect_range(test, input, ABS_MT_WIDTH_MINOR, 0, 5000);
		mm_expect_range(test, input, ABS_MT_ORIENTATION, -16384, 16384);
		mm_expect_range(test, input, ABS_MT_PRESSURE, 0, 6000);
		KUNIT_EXPECT_EQ(test, input_abs_get_res(input, ABS_MT_PRESSURE), 1);
		KUNIT_EXPECT_FALSE(test, test_bit(ABS_MT_TOOL_TYPE, input->absbit));
		KUNIT_EXPECT_TRUE(test, test_bit(INPUT_PROP_PRESSUREPAD, input->propbit));
	}
}

/* A J700 finger: its areas and orientation, no pressure, and the button. */
static void mm_c1fe_finger(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r = mm_finger(3, 100, 200);

	r.pressure = 77;
	mm_send(s, &r, 1, 1);
	KUNIT_EXPECT_EQ(test, mm_active(s), 1);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 100, 200), MT_TOOL_FINGER);
	KUNIT_EXPECT_EQ(test, mm_get(s, 100, 200, ABS_MT_TOUCH_MAJOR), 800);
	KUNIT_EXPECT_EQ(test, mm_get(s, 100, 200, ABS_MT_TOUCH_MINOR), 600);
	KUNIT_EXPECT_EQ(test, mm_get(s, 100, 200, ABS_MT_WIDTH_MAJOR), 1600);
	KUNIT_EXPECT_EQ(test, mm_get(s, 100, 200, ABS_MT_WIDTH_MINOR), 1200);
	KUNIT_EXPECT_EQ(test, mm_get(s, 100, 200, ABS_MT_ORIENTATION), 16384 - 15000);
	KUNIT_EXPECT_EQ(test, s->pressure_events, 0U);
	KUNIT_EXPECT_TRUE(test, test_bit(BTN_LEFT, s->input->key));

	mm_send(s, &r, 1, 0);
	KUNIT_EXPECT_FALSE(test, test_bit(BTN_LEFT, s->input->key));
	mm_send0(s);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
}

/*
 * The controller's palm classes make a palm in the frames they are given,
 * and only then: a contact taken for a palm while landing moves the pointer
 * once it is classed as a thumb. Classes past the width of a long are no
 * palm, whatever they alias modulo it.
 */
static void mm_c1fe_classes(struct kunit *test)
{
	static const u8 palms[] = { 6, 7, 11 };
	static const u8 others[] = { 0, 1, 2, 3, 4, 5, 8, 10, 12, 63, 64, 70, 71, 75, 255 };
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r = mm_finger(9, 50, 50);
	int i;

	for (i = 0; i < ARRAY_SIZE(palms); i++) {
		r.class = palms[i];
		mm_send1(s, r);
		KUNIT_EXPECT_EQ_MSG(test, mm_tool(s, 50, 50), MT_TOOL_PALM, "class %u", palms[i]);
	}
	for (i = 0; i < ARRAY_SIZE(others); i++) {
		r.class = others[i];
		mm_send1(s, r);
		KUNIT_EXPECT_EQ_MSG(test, mm_tool(s, 50, 50), MT_TOOL_FINGER, "class %u", others[i]);
	}

	/* Palm, then thumb, in one touch. */
	r.class = 7;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 50, 50), MT_TOOL_PALM);
	r.class = 1;
	r.x = 60;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 60, 50), MT_TOOL_FINGER);
}

/*
 * A tool area past 2200 (4400 reported) makes a palm, which stays one while
 * it touches whatever its area does, and ends when the contact is gone.
 */
static void mm_c1fe_tool_latch(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r = mm_finger(5, 300, 300);
	int tid;

	r.tool_major = 2200;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 300, 300), MT_TOOL_FINGER);
	tid = mm_tid(s, 300, 300);

	r.tool_major = 2201;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 300, 300), MT_TOOL_PALM);
	KUNIT_EXPECT_EQ(test, mm_tid(s, 300, 300), tid);
	KUNIT_EXPECT_EQ(test, mm_get(s, 300, 300, ABS_MT_WIDTH_MAJOR), 4402);

	r.tool_major = 900;
	r.x = 310;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 310, 300), MT_TOOL_PALM);
	r.x = 320;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 320, 300), MT_TOOL_PALM);

	/* Gone for one report: the next contact with its ID is new. */
	mm_send0(s);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 320, 300), MT_TOOL_FINGER);
}

/*
 * A palm stays a palm in its lift-off records, and a record of its ID that
 * enters the touching state again, with no report without the ID between,
 * is a new contact and no palm.
 */
static void mm_c1fe_lift_reuse(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec palm = mm_palm(4, 400, 100);
	struct mm_rec r;

	/* Lift-off with its areas, then a finger lands under the ID. */
	mm_send1(s, palm);
	palm.tool_major = 1000;
	mm_send1(s, palm);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 400, 100), MT_TOOL_PALM);
	r = palm;
	r.state = MM_LIFTING;
	r.x = 405;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 405, 100), MT_TOOL_PALM);
	r = mm_finger(4, -900, 500);
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 1);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -900, 500), MT_TOOL_FINGER);

	/* Lift-off without areas, then a finger lands under the ID. */
	mm_send1(s, palm);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 400, 100), MT_TOOL_FINGER);
	palm.tool_major = 2600;
	mm_send1(s, palm);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 400, 100), MT_TOOL_PALM);
	r = palm;
	r.state = MM_LIFTING;
	r.touch_major = r.touch_minor = r.tool_major = r.tool_minor = 0;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	r = mm_finger(4, 900, 500);
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 900, 500), MT_TOOL_FINGER);

	/* A palm-sized approach is no palm once the contact has landed small. */
	mm_send0(s);
	r = mm_finger(8, 0, 0);
	r.state = MM_APPROACH;
	r.touch_major = 0;
	r.tool_major = 3000;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	r = mm_finger(8, 0, 0);
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 0, 0), MT_TOOL_FINGER);
}

/*
 * A touching record without a touch area is still the same contact; records
 * without one that approach or lift off are not reported, nor are they with
 * one that reads negative.
 */
static void mm_c1fe_zero_area(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r = mm_finger(2, 700, 700);
	int tid;

	mm_send1(s, r);
	tid = mm_tid(s, 700, 700);
	r.touch_major = r.touch_minor = 0;
	r.x = 702;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tid(s, 702, 700), tid);
	KUNIT_EXPECT_EQ(test, mm_get(s, 702, 700, ABS_MT_TOUCH_MAJOR), 0);
	r = mm_finger(2, 704, 700);
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tid(s, 704, 700), tid);

	r.state = MM_LIFTING;
	r.touch_major = 0;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);

	r = mm_finger(6, 10, 10);
	r.state = MM_APPROACH;
	r.touch_major = 0;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	r.touch_major = 0x8000;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	r.state = MM_LIFTING;
	r.touch_major = 0xffff;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);

	/* Approach and lift-off records with a touch area are reported. */
	r.state = MM_APPROACH;
	r.touch_major = 100;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 1);
	r.state = MM_LIFTING;
	r.x = 12;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_get(s, 12, 10, ABS_MT_TOUCH_MAJOR), 200);
}

/*
 * Fields with bit 15 set read negative: J700 areas are reported as 0 and its
 * orientation within range, and such a tool area makes no palm. Such a touch
 * area makes no contact of an approach or lift-off record, but a touching
 * record is one. The largest positive fields fill the advertised ranges.
 */
static void mm_c1fe_high_bit(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r = mm_finger(1, 20, 30);

	r.touch_major = 0x8000;
	r.touch_minor = 0xffff;
	r.tool_major = 0x8001;
	r.tool_minor = 0xfffe;
	r.orientation = 0x8000;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 20, 30), MT_TOOL_FINGER);
	KUNIT_EXPECT_EQ(test, mm_get(s, 20, 30, ABS_MT_TOUCH_MAJOR), 0);
	KUNIT_EXPECT_EQ(test, mm_get(s, 20, 30, ABS_MT_TOUCH_MINOR), 0);
	KUNIT_EXPECT_EQ(test, mm_get(s, 20, 30, ABS_MT_WIDTH_MAJOR), 0);
	KUNIT_EXPECT_EQ(test, mm_get(s, 20, 30, ABS_MT_WIDTH_MINOR), 0);
	KUNIT_EXPECT_EQ(test, mm_get(s, 20, 30, ABS_MT_ORIENTATION), 16384);

	r.orientation = 0xffff;
	r.x = 21;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_get(s, 21, 30, ABS_MT_ORIENTATION), 16384);
	r.orientation = 0x7fff;
	r.x = 22;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_get(s, 22, 30, ABS_MT_ORIENTATION), 16384 - 0x7fff);

	r.touch_major = r.touch_minor = r.tool_major = r.tool_minor = 0x7fff;
	r.x = 23;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_get(s, 23, 30, ABS_MT_TOUCH_MAJOR), 2 * S16_MAX);
	KUNIT_EXPECT_EQ(test, mm_get(s, 23, 30, ABS_MT_WIDTH_MINOR), 2 * S16_MAX);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 23, 30), MT_TOOL_PALM);

	mm_send0(s);
	r = mm_finger(3, 40, 30);
	r.state = MM_APPROACH;
	r.touch_major = 0x8000;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	r.state = MM_LIFTING;
	r.touch_major = 0xffff;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	r.state = MM_TOUCHING;
	r.touch_major = 0x8000;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_active(s), 1);
	KUNIT_EXPECT_EQ(test, mm_get(s, 40, 30, ABS_MT_TOUCH_MAJOR), 0);
}

/*
 * Reports of the wrong shape are dropped whole: no events, no contact
 * change, and the palms and touching contacts tracked so far are kept.
 */
static void mm_c1fe_malformed(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec recs[MAX_CONTACTS + 1];
	struct mm_rec palm = mm_palm(12, -100, 600);
	unsigned int events;
	int size, i, tid;

	mm_send1(s, palm);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -100, 600), MT_TOOL_PALM);
	tid = mm_tid(s, -100, 600);
	palm.tool_major = 700;
	palm.x = -110;
	events = s->events;

	for (i = 0; i < 10; i++) {
		size = mm_build(s, &palm, 1, 1);
		switch (i) {
		case 0: s->buf[0] = 0x74; break;
		case 1: s->buf[2] = 31; break;
		case 2: s->buf[3] = 5; break;
		case 3: put_unaligned_le32(29, s->buf + 16); break;
		case 4: put_unaligned_le16(9, s->buf + 20); break;
		case 5: s->buf[22] = 2; break;
		case 6: size++; break;
		case 7: size--; break;
		case 8: size = MAGICMOUSE_C1FE_HEADER_SIZE - 1; break;
		case 9:
			for (int j = 0; j <= MAX_CONTACTS; j++)
				recs[j] = mm_finger(j + 20, j, j);
			size = mm_build(s, recs, MAX_CONTACTS + 1, 1);
			break;
		}
		KUNIT_EXPECT_EQ_MSG(test, mm_send_raw(s, size), 0, "variant %d", i);
	}
	KUNIT_EXPECT_EQ(test, s->events, events);
	KUNIT_EXPECT_EQ(test, mm_tid(s, -100, 600), tid);
	KUNIT_EXPECT_FALSE(test, test_bit(BTN_LEFT, s->input->key));

	mm_send1(s, palm);
	KUNIT_EXPECT_EQ(test, mm_tid(s, -110, 600), tid);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -110, 600), MT_TOOL_PALM);
}

/* Palms are told apart by ID: a finger next to one, and IDs 0 and 255. */
static void mm_c1fe_two_contacts(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r[2] = { mm_palm(0, -2000, 3000), mm_finger(255, 2000, 1000) };

	mm_send(s, r, 2, 0);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -2000, 3000), MT_TOOL_PALM);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 2000, 1000), MT_TOOL_FINGER);

	r[0].tool_major = 500;
	r[0].x = -1990;
	r[1].x = 2010;
	mm_send(s, r, 2, 0);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -1990, 3000), MT_TOOL_PALM);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 2010, 1000), MT_TOOL_FINGER);

	/* The palm lifts; the finger is unaffected and lifts in turn. */
	r[1].x = 2020;
	mm_send(s, &r[1], 1, 0);
	KUNIT_EXPECT_EQ(test, mm_active(s), 1);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 2020, 1000), MT_TOOL_FINGER);

	r[1] = mm_palm(255, 2030, 1000);
	mm_send(s, &r[1], 1, 0);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 2030, 1000), MT_TOOL_PALM);
	r[0] = mm_finger(0, -1980, 3000);
	r[1].tool_major = 300;
	mm_send(s, r, 2, 0);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -1980, 3000), MT_TOOL_FINGER);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 2030, 1000), MT_TOOL_PALM);
}

/*
 * The other MTP and SPI trackpads report what they did before palm
 * rejection: zero-area records are skipped whatever their state, palm-sized
 * contacts are fingers, the areas are doubled, negative ones included and in
 * any state, and pressure and the firmware's button are reported.
 */
static void mm_other_reports(struct kunit *test)
{
	enum mm_board boards[] = { MM_MTP, MM_SPI };
	int i;

	for (i = 0; i < ARRAY_SIZE(boards); i++) {
		struct mm_test *s = mm_setup(test, boards[i]);
		struct mm_rec r[4] = {
			mm_palm(1, 100, 100), mm_finger(2, 900, 900),
			mm_finger(3, 500, 500), mm_finger(4, 300, 300),
		};

		r[0].class = 6;
		r[0].pressure = 40;
		r[1].pressure = 30;
		r[1].touch_major = 0x8000;
		r[1].tool_minor = 0xffff;
		r[1].orientation = 0x8000;
		r[2].touch_major = 0;
		r[3].state = MM_LIFTING;
		r[3].touch_major = 0x8000;
		mm_send(s, r, 4, 1);
		KUNIT_EXPECT_EQ(test, mm_active(s), 3);
		KUNIT_EXPECT_EQ(test, mm_get(s, 300, 300, ABS_MT_TOUCH_MAJOR), -65536);
		KUNIT_EXPECT_NULL(test, mm_slot(s, 500, 500));
		KUNIT_EXPECT_EQ(test, mm_tool(s, 100, 100), MT_TOOL_FINGER);
		KUNIT_EXPECT_EQ(test, mm_get(s, 100, 100, ABS_MT_TOUCH_MAJOR), 3000);
		KUNIT_EXPECT_EQ(test, mm_get(s, 100, 100, ABS_MT_WIDTH_MAJOR), 5200);
		KUNIT_EXPECT_EQ(test, mm_get(s, 100, 100, ABS_MT_PRESSURE), 40);
		KUNIT_EXPECT_EQ(test, mm_get(s, 900, 900, ABS_MT_TOUCH_MAJOR), -65536);
		KUNIT_EXPECT_EQ(test, mm_get(s, 900, 900, ABS_MT_WIDTH_MINOR), -2);
		KUNIT_EXPECT_EQ(test, mm_get(s, 900, 900, ABS_MT_ORIENTATION), 16384 + 32768);
		KUNIT_EXPECT_EQ(test, mm_get(s, 900, 900, ABS_MT_PRESSURE), 30);
		KUNIT_EXPECT_GT(test, s->pressure_events, 0U);
		KUNIT_EXPECT_TRUE(test, test_bit(BTN_LEFT, s->input->key));

		r[0].tool_major = 400;
		r[0].x = 110;
		mm_send(s, r, 1, 0);
		KUNIT_EXPECT_EQ(test, mm_tool(s, 110, 100), MT_TOOL_FINGER);
		KUNIT_EXPECT_FALSE(test, test_bit(BTN_LEFT, s->input->key));
		mm_send0(s);
		KUNIT_EXPECT_EQ(test, mm_active(s), 0);
	}
}

/*
 * No report arrives while the J700 trackpad is closed, so a lift-off can go
 * unseen: once it is opened again, a contact touching under the ID of an old
 * palm is a new one. Reports of the wrong shape leave that for the next good
 * one, and the other trackpads are not asked to forget anything.
 */
static void mm_c1fe_reopen(struct kunit *test)
{
	struct mm_test *s = mm_setup(test, MM_C1FE);
	struct mm_rec r = mm_palm(7, 100, 100);
	int size;

	mm_send1(s, r);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(s->msc->c1fe_reset_pending));
	r.tool_major = 900;
	r.x = 110;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, 110, 100), MT_TOOL_PALM);

	input_close_device(s->handle);
	KUNIT_EXPECT_EQ(test, s->input->users, 0U);
	KUNIT_ASSERT_EQ(test, input_open_device(s->handle), 0);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(s->msc->c1fe_reset_pending));

	size = mm_build(s, &r, 1, 0);
	s->buf[3] = 5;
	KUNIT_EXPECT_EQ(test, mm_send_raw(s, size), 0);
	size = mm_build(s, &r, 1, 0);
	KUNIT_EXPECT_EQ(test, mm_send_raw(s, size + 1), 0);
	KUNIT_EXPECT_TRUE(test, READ_ONCE(s->msc->c1fe_reset_pending));

	/* The palm lifted unseen; a finger touches under its ID. */
	r = mm_finger(7, -500, 400);
	mm_send1(s, r);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(s->msc->c1fe_reset_pending));
	KUNIT_EXPECT_EQ(test, mm_tool(s, -500, 400), MT_TOOL_FINGER);

	/* From there on the latch works as before. */
	r.tool_major = 2600;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -500, 400), MT_TOOL_PALM);
	r.tool_major = 900;
	r.x = -490;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -490, 400), MT_TOOL_PALM);

	/* A palm still palm-sized after an open is still one. */
	input_close_device(s->handle);
	KUNIT_ASSERT_EQ(test, input_open_device(s->handle), 0);
	r.tool_major = 2600;
	r.x = -480;
	mm_send1(s, r);
	KUNIT_EXPECT_EQ(test, mm_tool(s, -480, 400), MT_TOOL_PALM);

	s = mm_setup(test, MM_MTP);
	KUNIT_EXPECT_EQ(test, s->input->users, 1U);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(s->msc->c1fe_reset_pending));
}

static struct kunit_case magicmouse_mtp_cases[] = {
	KUNIT_CASE(mm_c1fe_caps),
	KUNIT_CASE(mm_other_caps),
	KUNIT_CASE(mm_c1fe_finger),
	KUNIT_CASE(mm_c1fe_classes),
	KUNIT_CASE(mm_c1fe_tool_latch),
	KUNIT_CASE(mm_c1fe_lift_reuse),
	KUNIT_CASE(mm_c1fe_zero_area),
	KUNIT_CASE(mm_c1fe_high_bit),
	KUNIT_CASE(mm_c1fe_malformed),
	KUNIT_CASE(mm_c1fe_two_contacts),
	KUNIT_CASE(mm_c1fe_reopen),
	KUNIT_CASE(mm_other_reports),
	{}
};

static struct kunit_suite magicmouse_mtp_suite = {
	.name = "hid-magicmouse-mtp",
	.test_cases = magicmouse_mtp_cases,
};

kunit_test_suite(magicmouse_mtp_suite);
