// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright The Asahi Linux Contributors */

#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "dcp-property-store.h"

struct dcp_property_store *dcp_property_store_create(void)
{
	return kvzalloc(sizeof(struct dcp_property_store), GFP_KERNEL);
}

void dcp_property_store_destroy(struct dcp_property_store *store)
{
	unsigned int i;

	if (!store)
		return;
	for (i = 0; i < DCP_PROPERTY_RAWS; i++)
		kvfree(store->raw[i].data);
	kvfree(store->chunk);
	kvfree(store);
}

int dcp_property_key_valid(const void *key)
{
	return memchr(key, 0, DCP_PROPERTY_KEY_LEN) ? 0 : -EINVAL;
}

static int scalar_slot(struct dcp_property_store *store, u32 service,
		       const char *key, bool create)
{
	int free_slot = -ENOSPC;
	unsigned int i;

	for (i = 0; i < DCP_PROPERTY_SCALARS; i++) {
		const struct dcp_property_scalar *p = &store->scalar[i];

		if (p->used) {
			if (p->service == service && !strcmp(p->key, key))
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	return create ? free_slot : -ENOENT;
}

static int raw_slot(struct dcp_property_store *store, u32 service,
		    const char *key, bool create)
{
	int free_slot = -ENOSPC;
	unsigned int i;

	for (i = 0; i < DCP_PROPERTY_RAWS; i++) {
		const struct dcp_property_raw *p = &store->raw[i];

		if (p->used) {
			if (p->service == service && !strcmp(p->key, key))
				return i;
		} else if (free_slot < 0) {
			free_slot = i;
		}
	}
	return create ? free_slot : -ENOENT;
}

int dcp_property_set_scalar(struct dcp_property_store *store, u32 service,
			    const char *key, u64 value)
{
	int slot = scalar_slot(store, service, key, true);
	struct dcp_property_scalar *p;

	if (slot < 0)
		return slot;
	p = &store->scalar[slot];
	strscpy(p->key, key, sizeof(p->key));
	p->service = service;
	p->value = value;
	p->used = true;
	return 0;
}

int dcp_property_remove_scalar(struct dcp_property_store *store, u32 service,
			       const char *key)
{
	int slot = scalar_slot(store, service, key, false);

	if (slot >= 0)
		store->scalar[slot].used = false;
	return 0;
}

int dcp_property_get_scalar(struct dcp_property_store *store, u32 service,
			    const char *key, u64 *value)
{
	int slot = scalar_slot(store, service, key, false);

	if (slot < 0)
		return slot;
	*value = store->scalar[slot].value;
	return 0;
}

/* @take_chunk: adopt the completed chunk buffer instead of copying @data. */
static int raw_set(struct dcp_property_store *store, u32 service,
		   const char *key, const void *data, u32 size, bool take_chunk)
{
	int slot = raw_slot(store, service, key, true);
	struct dcp_property_raw *p;
	void *copy = NULL;

	if (slot < 0)
		return slot;
	if (size > DCP_PROPERTY_RAW_LIMIT ||
	    store->raw_bytes > DCP_PROPERTY_RAW_TOTAL - store->chunk_size)
		return -E2BIG;
	/* Old values and the transfer in progress count against the budget. */
	if (!take_chunk &&
	    size > DCP_PROPERTY_RAW_TOTAL - store->raw_bytes - store->chunk_size)
		return -ENOSPC;
	if (take_chunk) {
		if (!store->chunk_active || store->chunk_filled != store->chunk_size ||
		    size != store->chunk_size || data != store->chunk)
			return -EINVAL;
		copy = store->chunk;
	} else if (size) {
		copy = kvmalloc(size, GFP_KERNEL);
		if (!copy)
			return -ENOMEM;
		memcpy(copy, data, size);
	}
	p = &store->raw[slot];
	if (p->used) {
		store->raw_bytes -= p->size;
		kvfree(p->data);
	}
	strscpy(p->key, key, sizeof(p->key));
	p->service = service;
	p->size = size;
	p->data = copy;
	p->used = true;
	store->raw_bytes += size;
	if (take_chunk) {
		store->chunk = NULL;
		store->chunk_active = false;
		store->chunk_size = 0;
		store->chunk_filled = 0;
	}
	return 0;
}

int dcp_property_set_raw(struct dcp_property_store *store, u32 service,
			 const char *key, const void *data, u32 size)
{
	return raw_set(store, service, key, data, size, false);
}

const void *dcp_property_get_raw(struct dcp_property_store *store, u32 service,
				 const char *key, u32 *size)
{
	int slot = raw_slot(store, service, key, false);

	if (slot < 0)
		return NULL;
	*size = store->raw[slot].size;
	return store->raw[slot].data;
}

int dcp_property_chunk_start(struct dcp_property_store *store, u32 announced)
{
	u32 size = announced;

	if (!size || size > DCP_PROPERTY_RAW_LIMIT + 1 || store->chunk_active)
		return -EINVAL;
	size--;
	if (size > DCP_PROPERTY_RAW_TOTAL - store->raw_bytes)
		return -ENOSPC;
	store->chunk = size ? kvmalloc(size, GFP_KERNEL) : NULL;
	if (size && !store->chunk)
		return -ENOMEM;
	store->chunk_size = size;
	store->chunk_filled = 0;
	store->chunk_active = true;
	return 0;
}

int dcp_property_chunk_add(struct dcp_property_store *store, const void *data,
			   u32 offset, u32 count)
{
	if (!store->chunk_active || count > 4096 || offset != store->chunk_filled ||
	    count > store->chunk_size - store->chunk_filled)
		return -EINVAL;
	if (count)
		memcpy((u8 *)store->chunk + offset, data, count);
	store->chunk_filled += count;
	return 0;
}

int dcp_property_chunk_end(struct dcp_property_store *store, const char *key)
{
	return raw_set(store, 0, key, store->chunk, store->chunk_size, true);
}
