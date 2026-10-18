/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/* Copyright The Asahi Linux Contributors */

/*
 * Host-side property registry of an adopted DCP session (14.7, J514S).
 *
 * The firmware publishes properties to the AP (number, Boolean, string and
 * serialized-dictionary setters, and chunked DCPAV transfers) and later
 * reads some of them back (D400 raw, D401 numeric).  The registry keeps them
 * keyed by service selector and key, bounded in count and bytes, so that the
 * getters answer what the firmware set instead of an invented value.
 *
 * One serialized owner (the IOMFB receive path) uses a store; it holds no
 * DMA memory and is never visible to the firmware.
 */

#ifndef __APPLE_DCP_PROPERTY_STORE_H__
#define __APPLE_DCP_PROPERTY_STORE_H__

#include <linux/types.h>

#define DCP_PROPERTY_KEY_LEN		64
#define DCP_PROPERTY_SCALARS		256
#define DCP_PROPERTY_RAWS		32
#if IS_ENABLED(CONFIG_DRM_APPLE_DCP_T6030_EXT)
/*
 * The external role publishes the sink's TimingElements as one chunked
 * transfer: an HDMI 2.1 (FRL) sink's catalog exceeds 1 MiB (a 4K120 TV's is
 * 1.38 MB), so a property may take 8 MiB and a store 24 MiB.
 */
#define DCP_PROPERTY_RAW_LIMIT		(8U << 20)
#define DCP_PROPERTY_RAW_TOTAL		(24U << 20)
#else
#define DCP_PROPERTY_RAW_LIMIT		(1U << 20)
#define DCP_PROPERTY_RAW_TOTAL		(8U << 20)
#endif

struct dcp_property_scalar {
	char key[DCP_PROPERTY_KEY_LEN];
	u32 service;
	u64 value;
	bool used;
};

struct dcp_property_raw {
	char key[DCP_PROPERTY_KEY_LEN];
	u32 service;
	u32 size;
	void *data;
	bool used;
};

struct dcp_property_store {
	struct dcp_property_scalar scalar[DCP_PROPERTY_SCALARS];
	struct dcp_property_raw raw[DCP_PROPERTY_RAWS];
	/* Chunked DCPAV transfer in progress (D127/D128/D129). */
	void *chunk;
	u32 chunk_size;
	u32 chunk_filled;
	bool chunk_active;
	u32 raw_bytes;
};

struct dcp_property_store *dcp_property_store_create(void);
void dcp_property_store_destroy(struct dcp_property_store *store);

/* 0 if @key is NUL-terminated within DCP_PROPERTY_KEY_LEN bytes. */
int dcp_property_key_valid(const void *key);

int dcp_property_set_scalar(struct dcp_property_store *store, u32 service,
			    const char *key, u64 value);
int dcp_property_remove_scalar(struct dcp_property_store *store, u32 service,
			       const char *key);
/* -ENOENT if absent. */
int dcp_property_get_scalar(struct dcp_property_store *store, u32 service,
			    const char *key, u64 *value);

/* Copies @size bytes of @data. */
int dcp_property_set_raw(struct dcp_property_store *store, u32 service,
			 const char *key, const void *data, u32 size);
/* Returns the stored bytes (owned by the store) or NULL if absent. */
const void *dcp_property_get_raw(struct dcp_property_store *store, u32 service,
				 const char *key, u32 *size);

/*
 * D127: begin a transfer of @announced bytes, which counts one byte the
 * firmware never sends (the serialized object is @announced - 1 bytes).
 */
int dcp_property_chunk_start(struct dcp_property_store *store, u32 announced);
/* D128: @count bytes at @offset, which must continue the transfer. */
int dcp_property_chunk_add(struct dcp_property_store *store, const void *data,
			   u32 offset, u32 count);
/* D129: the complete transfer becomes raw property @key of service 0. */
int dcp_property_chunk_end(struct dcp_property_store *store, const char *key);

#endif /* __APPLE_DCP_PROPERTY_STORE_H__ */
