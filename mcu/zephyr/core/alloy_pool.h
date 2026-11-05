/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * RAM buffer pool with store-and-forward. Platform independent: the caller supplies the memory,
 * the clock (now_ms) and the locking (every call must hold the caller's pool lock).
 *
 * Each buffer is in exactly one place: the free ring, the pending ring (sealed, oldest first), a
 * channel slot (being filled) or the uploader's hands (in flight). Producers fill a channel's
 * active buffer and seal it when it is 90% full, when its field set changes, or on the flush
 * interval. The interval flush never runs while a sealed chunk is still pending: during an outage
 * a backlogged channel keeps filling to the 90% seal instead of spending the pool on mostly
 * empty chunks. Only when no buffer is free does the oldest pending chunk get shed.
 */

#ifndef ALLOY_CORE_POOL_H_
#define ALLOY_CORE_POOL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alloy_csv.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALLOY_POOL_MAX_BUFS
#define ALLOY_POOL_MAX_BUFS 16
#endif
#ifndef ALLOY_POOL_MAX_CHANNELS
#define ALLOY_POOL_MAX_CHANNELS 16
#endif

#define ALLOY_POOL_NONE 0xFF

struct alloy_buf {
	char *data;
	size_t len;
	char chan[ALLOY_CHAN_MAX]; /* sanitized */
};

struct alloy_slot {
	char chan[ALLOY_CHAN_MAX]; /* as logged */
	uint8_t active;            /* buffer index or ALLOY_POOL_NONE */
	uint32_t since_ms;
	uint32_t sig;
};

struct alloy_ring {
	uint8_t idx[ALLOY_POOL_MAX_BUFS];
	uint8_t head;
	uint8_t count;
};

struct alloy_pool {
	struct alloy_buf bufs[ALLOY_POOL_MAX_BUFS];
	uint8_t n;
	size_t buf_bytes;
	uint32_t flush_ms;
	struct alloy_ring free_q;
	struct alloy_ring pending_q;
	struct alloy_slot slots[ALLOY_POOL_MAX_CHANNELS];
	uint8_t n_slots;
	uint8_t max_slots;
	uint32_t dropped_bufs; /* sealed chunks shed before delivery */
	uint32_t dropped_rows; /* rows refused before buffering */
};

/* mem must hold n * buf_bytes bytes. Returns 0 or -EINVAL. */
int alloy_pool_init(struct alloy_pool *p, char *mem, uint8_t n, size_t buf_bytes,
		    uint8_t max_channels, uint32_t flush_ms);

/* Appends one row (header first if the buffer is fresh). Returns 0, or -ENOSPC when the row was
 * dropped (counted in dropped_rows): too big for any buffer, too many channels, or no buffer.
 */
int alloy_pool_commit(struct alloy_pool *p, const char *chan, const char *hdr, int hlen,
		      const char *row, int rlen, uint32_t sig, uint32_t now_ms);

/* Uploader side: takes the oldest sealed chunk (now in flight). */
bool alloy_pool_take(struct alloy_pool *p, uint8_t *idx);

/* Uploader side: returns an in-flight buffer to the free ring. */
void alloy_pool_release(struct alloy_pool *p, uint8_t idx);

/* Uploader idle tick: seals channels whose flush interval elapsed, claiming each replacement
 * buffer atomically. Does nothing while a chunk is pending. Returns the number sealed.
 */
unsigned int alloy_pool_flush_due(struct alloy_pool *p, uint32_t now_ms);

/* End of run: seals every non-empty active buffer and frees the empty ones. */
void alloy_pool_seal_all(struct alloy_pool *p);

static inline unsigned int alloy_pool_free_count(const struct alloy_pool *p)
{
	return p->free_q.count;
}

static inline unsigned int alloy_pool_pending_count(const struct alloy_pool *p)
{
	return p->pending_q.count;
}

#ifdef __cplusplus
}
#endif

#endif /* ALLOY_CORE_POOL_H_ */
