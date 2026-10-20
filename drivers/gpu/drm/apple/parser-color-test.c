// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Included by parser.c: exercise the real serialized-colour parsing path. */
#include <kunit/test.h>
#include <linux/unaligned.h>

struct colour_fixture {
	s64 id, score, depth, colorimetry, range, eotf, encoding;
	u32 missing;
};

struct colour_blob {
	u8 data[2048];
	u32 len;
};

static void colour_tag(struct colour_blob *b, u32 type, u32 size)
{
	b->len = ALIGN(b->len, 4);
	put_unaligned_le32(type << 24 | size, b->data + b->len);
	b->len += 4;
}

static void colour_integer(struct colour_blob *b, const char *key, s64 value)
{
	colour_tag(b, DCP_TYPE_STRING, strlen(key));
	memcpy(b->data + b->len, key, strlen(key));
	b->len += strlen(key);
	colour_tag(b, DCP_TYPE_INT64, 0);
	put_unaligned_le64(value, b->data + b->len);
	b->len += 8;
}

static struct colour_fixture colour_rgb(s64 id, s64 score, s64 colorimetry)
{
	return (struct colour_fixture) {
		.id = id, .score = score, .depth = 10, .colorimetry = colorimetry,
		.range = DCP_COLOR_YCBCR_RANGE_FULL, .eotf = DCP_EOTF_SDR_GAMMA,
		.encoding = DCP_COLOR_FORMAT_RGB,
	};
}

static int colour_parse(struct kunit *test, const struct colour_fixture *colours,
			unsigned int count, int preferred, bool internal, bool strict,
			bool fw14, struct dcp_display_mode *out)
{
	static const char * const keys[] = {
		"ID", "Score", "Depth", "Colorimetry", "DynamicRange", "EOTF", "PixelEncoding",
	};
	struct colour_blob *b = kunit_kzalloc(test, sizeof(*b), GFP_KERNEL);
	struct timing_color_restrictions limits = {};
	struct dcp_parse_ctx ctx;
	int old, ret;

	if (!b)
		return -ENOMEM;
	put_unaligned_le32(DCP_PARSE_HEADER, b->data);
	b->len = 4;
	colour_tag(b, DCP_TYPE_ARRAY, count);
	for (unsigned int i = 0; i < count; i++) {
		const struct colour_fixture *c = &colours[i];
		s64 values[] = { c->id, c->score, c->depth, c->colorimetry,
				 c->range, c->eotf, c->encoding };

		colour_tag(b, DCP_TYPE_DICTIONARY, 8 - hweight32(c->missing));
		colour_tag(b, DCP_TYPE_STRING, strlen("IsVirtual"));
		memcpy(b->data + b->len, "IsVirtual", strlen("IsVirtual"));
		b->len += strlen("IsVirtual");
		colour_tag(b, DCP_TYPE_BOOL, 0);
		for (unsigned int field = 0; field < ARRAY_SIZE(keys); field++)
			if (!(c->missing & BIT(field)))
				colour_integer(b, keys[field], values[field]);
	}
	ret = parse(b->data, b->len, &ctx);
	if (ret)
		return ret;
	ctx.v14_7_rgb8 = fw14;
	old = READ_ONCE(external_colorimetry);
	WRITE_ONCE(external_colorimetry, preferred);
	ret = parse_color_modes(&ctx, out, internal, strict, &limits, NULL);
	WRITE_ONCE(external_colorimetry, old);
	return ret;
}

static void colour_preference_scope(struct kunit *test)
{
	struct colour_fixture c[] = {
		colour_rgb(69, 440999, DCP_COLORIMETRY_RGB),
		colour_rgb(68, 447999, DCP_COLORIMETRY_BT2020_RGB),
		colour_rgb(67, 200000, DCP_COLORIMETRY_SRGB),
	};
	struct dcp_display_mode out = {};

	KUNIT_EXPECT_EQ(test, READ_ONCE(external_colorimetry), -1);
	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 3, -1, false, false, false, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 68U);
	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 3, DCP_COLORIMETRY_SRGB,
					 false, false, false, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 67U);
	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 2, DCP_COLORIMETRY_SRGB,
					 false, false, false, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 69U);
	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 3, DCP_COLORIMETRY_SRGB,
					 true, false, false, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 68U);
	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 2, INT_MAX, false, false, false, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 68U);
}

static void colour_malformed_preference_keeps_fallback(struct kunit *test)
{
	for (unsigned int missing = 2; missing <= 4; missing += 2) {
		for (unsigned int order = 0; order < 2; order++) {
			struct colour_fixture c[2];
			struct dcp_display_mode out = {};

			c[order] = colour_rgb(69, 440999, DCP_COLORIMETRY_RGB);
			c[!order] = colour_rgb(67, 500000, DCP_COLORIMETRY_SRGB);
			c[!order].missing = BIT(missing);
			KUNIT_ASSERT_EQ(test, colour_parse(test, c, 2, DCP_COLORIMETRY_SRGB,
							 false, false, false, &out), 0);
			KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 69U);
			KUNIT_EXPECT_EQ(test, out.sdr_rgb.score, 440999LL);
		}
	}
}

static void colour_strict_profiles_keep_admission(struct kunit *test)
{
	struct colour_fixture c[] = {
		colour_rgb(69, 440999, DCP_COLORIMETRY_RGB),
		colour_rgb(68, 447999, DCP_COLORIMETRY_BT2020_RGB),
	};
	struct dcp_display_mode out = {};

	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 2, DCP_COLORIMETRY_BT2020_RGB,
					 false, true, false, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 69U);
	c[0].depth = 8;
	KUNIT_ASSERT_EQ(test, colour_parse(test, c, 2, DCP_COLORIMETRY_BT2020_RGB,
					 false, true, true, &out), 0);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.id, 69U);
	KUNIT_EXPECT_EQ(test, out.sdr_rgb.depth, (u8)8);
}

static struct kunit_case colour_cases[] = {
	KUNIT_CASE(colour_preference_scope),
	KUNIT_CASE(colour_malformed_preference_keeps_fallback),
	KUNIT_CASE(colour_strict_profiles_keep_admission),
	{}
};

static struct kunit_suite colour_suite = {
	.name = "apple-display-colour",
	.test_cases = colour_cases,
};

kunit_test_suite(colour_suite);
