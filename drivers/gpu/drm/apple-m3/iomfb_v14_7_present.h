/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * DCP firmware 14.7 (J514S internal panel): the present encoding.
 *
 * The panel is qualified with one profile: a single linear XRGB8888 or
 * ARGB8888 surface in surface slot 0, full panel and unscaled, tagged
 * transfer 13 / primaries 12 with per-layer word 0; every present updates
 * the background and layers 0-2 (0x80000007) over a black background, and
 * carries the panel level in nits only when it changes.  Any second surface
 * routes the whole frame through the firmware's blend and colour processing,
 * which casts this panel warm, so nothing here can describe one.
 *
 * These helpers build that A407 request without driver state, so that KUnit
 * can compare it byte for byte with reference requests of that profile.
 */

#ifndef __APPLE_IOMFB_V14_7_PRESENT_H__
#define __APPLE_IOMFB_V14_7_PRESENT_H__

#include <linux/array_size.h>
#include <linux/bitops.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_plane.h>

#include "iomfb.h"
#include "iomfb_plane.h"
#include "iomfb_v14_7.h"

/* Update masks of every 14.7 present: the background and layers 0..2. */
#define IOMFB_V14_7_SWAP_MASK		(IOMFB_SET_BACKGROUND | 0x7)
#define IOMFB_V14_7_BACKGROUND		0xff000000U
/* Identity of the single surface, in its descriptor and in the swap. */
#define IOMFB_V14_7_SURFACE_ID		1
/* Plane-0 element description of the 4-byte linear pixel profile. */
#define IOMFB_V14_7_PLANE_TILE_SIZE	4
/* The A407 nits field takes integral levels up to the panel maximum. */
#define IOMFB_V14_7_NITS_MAX		500
/* Level presented by the first present after takeover. */
#define IOMFB_V14_7_NITS_DEFAULT	140
/* Scanout DVAs: the display subsystem's 64-GiB DART range at its 1-TiB alias. */
#define IOMFB_V14_7_SCANOUT_BASE	(1ULL << 40)
#define IOMFB_V14_7_SCANOUT_SIZE	(1ULL << 36)

/* The surface-0 framebuffer of one present, at its scanout DVA. */
struct iomfb_v14_7_frame {
	u64 dva;
	u64 backing;		/* bytes mapped from dva */
	u32 width;
	u32 height;
	u32 stride;
	bool alpha;		/* ARGB8888; XRGB8888 otherwise */
};

/*
 * A frame the panel takes: exactly the panel size, a 64-byte aligned stride
 * of at least four bytes per pixel, below 4 GiB, and backed by one mapping
 * that starts 16K-aligned inside the scanout aperture.
 */
static inline int iomfb_v14_7_frame_check(const struct iomfb_v14_7_frame *frame,
					  u32 panel_width, u32 panel_height)
{
	u64 size = (u64)frame->stride * frame->height;

	if (frame->width != panel_width || frame->height != panel_height ||
	    frame->stride < (u64)frame->width * 4 || frame->stride % 64 ||
	    !size || size > 0xffffffffULL || frame->backing < size ||
	    !frame->dva || frame->dva % 0x4000 ||
	    frame->dva < IOMFB_V14_7_SCANOUT_BASE ||
	    frame->dva - IOMFB_V14_7_SCANOUT_BASE >= IOMFB_V14_7_SCANOUT_SIZE ||
	    frame->backing > IOMFB_V14_7_SCANOUT_SIZE -
			     (frame->dva - IOMFB_V14_7_SCANOUT_BASE))
		return -EINVAL;
	return 0;
}

/*
 * Describe a plane state as the surface-0 frame: one linear XRGB8888 or
 * ARGB8888 GEM DMA buffer at offset 0, scanned unscaled over the whole
 * panel.  Imported dma-bufs are scanned at their display-subsystem DVA.
 */
static inline int iomfb_v14_7_plane_frame(const struct drm_plane_state *state,
					  u32 panel_width, u32 panel_height,
					  struct iomfb_v14_7_frame *frame)
{
	struct drm_framebuffer *fb = state->fb;
	struct drm_gem_dma_object *obj;

	if (!fb || fb->format->num_planes != 1 || fb->offsets[0] ||
	    fb->modifier != DRM_FORMAT_MOD_LINEAR ||
	    (fb->format->format != DRM_FORMAT_XRGB8888 &&
	     fb->format->format != DRM_FORMAT_ARGB8888) ||
	    state->src_x || state->src_y || state->crtc_x || state->crtc_y ||
	    state->src_w != (u64)panel_width << 16 ||
	    state->src_h != (u64)panel_height << 16 ||
	    state->crtc_w != panel_width || state->crtc_h != panel_height)
		return -EINVAL;
	obj = drm_fb_dma_get_gem_obj(fb, 0);
	if (!obj)
		return -EINVAL;
	*frame = (struct iomfb_v14_7_frame) {
		.dva = obj->dma_addr,
		.backing = obj->base.size,
		.width = fb->width,
		.height = fb->height,
		.stride = fb->pitches[0],
		.alpha = fb->format->format == DRM_FORMAT_ARGB8888,
	};
	return iomfb_v14_7_frame_check(frame, panel_width, panel_height);
}

/* Integral nits as IEEE 754 binary64, without kernel floating point. */
static inline u64 iomfb_v14_7_nits_binary64(u32 nits)
{
	unsigned int bit;

	if (!nits)
		return 0;
	bit = fls(nits) - 1;
	return ((u64)(1023 + bit) << 52) |
	       ((u64)(nits - (1U << bit)) << (52 - bit));
}

/*
 * The single-layer descriptor.  +0x14 = 12 is the colour-primaries index the
 * panel is qualified with (the common driver's DCP_COLORSPACE_NATIVE); the
 * premultiplied byte is set for XRGB8888, which DCP takes as premultiplied
 * over the black background.
 */
static inline void iomfb_v14_7_encode_surface(struct dcp_surface *surface,
					      const struct iomfb_v14_7_frame *frame)
{
	u32 size = frame->stride * frame->height;

	*surface = (struct dcp_surface) {
		.is_premultiplied = !frame->alpha,
		.plane_cnt = 1,
		.plane_cnt2 = 1,
		.format = DCP_FORMAT_BGRA,
		.xfer_func = DCP_XFER_FUNC_SDR,
		.colorspace = DCP_COLORSPACE_NATIVE,
		.stride = frame->stride,
		.pix_size = 1,
		.pel_w = 1,
		.pel_h = 1,
		.width = frame->width,
		.height = frame->height,
		.buf_size = size,
		.surface_id = IOMFB_V14_7_SURFACE_ID,
		.has_comp = 1,
		.planes[0] = {
			.width = frame->width,
			.height = frame->height,
			.stride = frame->stride,
			.size = size,
			.tile_size = IOMFB_V14_7_PLANE_TILE_SIZE,
			.tile_w = 1,
			.tile_h = 1,
		},
		.has_planes = 1,
	};
}

/*
 * A complete present: surface 0 from @frame, or the blank background when
 * @frame is NULL.  The swap ID comes from A406 and the nits block from the
 * backlight policy; both are zero here.
 */
static inline void iomfb_v14_7_encode_present(struct dcp_swap_submit_req_v14_7 *req,
					      const struct iomfb_v14_7_frame *frame)
{
	unsigned int i;

	memset(req, 0, sizeof(*req));
	req->swap.swap_enabled = IOMFB_V14_7_SWAP_MASK;
	req->swap.swap_completed = IOMFB_V14_7_SWAP_MASK;
	req->swap.bg_color = IOMFB_V14_7_BACKGROUND;
	for (i = 0; i < ARRAY_SIZE(req->surf_null); i++)
		req->surf_null[i] = 1;
	for (i = 0; i < ARRAY_SIZE(req->surf2_null); i++)
		req->surf2_null[i] = 1;
	req->unknown_pointer_null = 1;
	req->unknown_output_null = 1;
	if (!frame)
		return;

	req->swap.surf_ids[0] = IOMFB_V14_7_SURFACE_ID;
	req->swap.src_rect[0] = (struct dcp_rect) {
		.w = frame->width,
		.h = frame->height,
	};
	req->swap.dst_rect[0] = req->swap.src_rect[0];
	iomfb_v14_7_encode_surface(&req->surf[0].base, frame);
	req->surf_iova[0] = frame->dva;
	req->surf_null[0] = 0;
}

/* The panel level block: present byte +0x354, binary64 nits at +0x35c. */
static inline int iomfb_v14_7_encode_nits(struct dcp_swap_submit_req_v14_7 *req,
					  bool change, u32 nits)
{
	if (change && nits > IOMFB_V14_7_NITS_MAX)
		return -EINVAL;
	req->swap.bl_nits_present = change;
	req->swap.bl_nits = change ? iomfb_v14_7_nits_binary64(nits) : 0;
	return 0;
}

/* Whether a request names nothing but surface 0. */
static inline bool iomfb_v14_7_single_surface(const struct dcp_swap_submit_req_v14_7 *req)
{
	unsigned int i;

	if (req->swap.swap_enabled != IOMFB_V14_7_SWAP_MASK ||
	    req->swap.swap_completed != IOMFB_V14_7_SWAP_MASK)
		return false;
	for (i = 1; i < ARRAY_SIZE(req->surf_null); i++)
		if (!req->surf_null[i] || req->surf_iova[i] ||
		    req->swap.surf_ids[i] || req->swap.surf_flags[i])
			return false;
	for (i = 0; i < ARRAY_SIZE(req->surf2_null); i++)
		if (!req->surf2_null[i] || req->surf2_iova[i])
			return false;
	return !req->swap.surf_flags[0];
}

#endif /* __APPLE_IOMFB_V14_7_PRESENT_H__ */
