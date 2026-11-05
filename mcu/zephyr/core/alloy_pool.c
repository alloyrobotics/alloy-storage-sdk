/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include "alloy_pool.h"

#include <errno.h>
#include <string.h>

static void ring_push(struct alloy_ring *r, uint8_t idx)
{
	r->idx[(r->head + r->count) % ALLOY_POOL_MAX_BUFS] = idx;
	r->count++;
}

static bool ring_pop(struct alloy_ring *r, uint8_t *idx)
{
	if (r->count == 0) {
		return false;
	}
	*idx = r->idx[r->head];
	r->head = (uint8_t)((r->head + 1) % ALLOY_POOL_MAX_BUFS);
	r->count--;
	return true;
}

static void copy_name(char *dst, const char *src)
{
	size_t n = strlen(src);

	if (n >= ALLOY_CHAN_MAX) {
		n = ALLOY_CHAN_MAX - 1;
	}
	memcpy(dst, src, n);
	dst[n] = '\0';
}

int alloy_pool_init(struct alloy_pool *p, char *mem, uint8_t n, size_t buf_bytes,
		    uint8_t max_channels, uint32_t flush_ms)
{
	if (!p || !mem || n < 2 || n > ALLOY_POOL_MAX_BUFS || buf_bytes < 64 ||
	    max_channels == 0 || max_channels > ALLOY_POOL_MAX_CHANNELS) {
		return -EINVAL;
	}
	memset(p, 0, sizeof(*p));
	p->n = n;
	p->buf_bytes = buf_bytes;
	p->flush_ms = flush_ms;
	p->max_slots = max_channels;
	for (uint8_t i = 0; i < n; i++) {
		p->bufs[i].data = mem + (size_t)i * buf_bytes;
		ring_push(&p->free_q, i);
	}
	return 0;
}

/* Pending is never full when a buffer outside it is sealed (it holds at most n - 1 others), but
 * keep the shed-oldest fallback so the invariant failing can only lose data, never corrupt.
 */
static void seal(struct alloy_pool *p, uint8_t idx)
{
	if (p->pending_q.count >= p->n) {
		uint8_t old;

		if (ring_pop(&p->pending_q, &old)) {
			p->bufs[old].len = 0;
			p->dropped_bufs++;
			ring_push(&p->free_q, old);
		}
	}
	ring_push(&p->pending_q, idx);
}

/* A free buffer, else the oldest pending one (shed). Never sheds `avoid`, the chunk just sealed:
 * when it is the only pending chunk the new row is dropped instead.
 */
static uint8_t get_free(struct alloy_pool *p, uint8_t avoid)
{
	uint8_t idx;

	if (ring_pop(&p->free_q, &idx)) {
		return idx;
	}
	if (p->pending_q.count > 0) {
		idx = p->pending_q.idx[p->pending_q.head];
		if (idx == avoid) {
			return ALLOY_POOL_NONE;
		}
		ring_pop(&p->pending_q, &idx);
		p->dropped_bufs++;
		p->bufs[idx].len = 0;
		return idx;
	}
	return ALLOY_POOL_NONE;
}

static struct alloy_slot *slot_for(struct alloy_pool *p, const char *chan)
{
	for (uint8_t i = 0; i < p->n_slots; i++) {
		if (strcmp(p->slots[i].chan, chan) == 0) {
			return &p->slots[i];
		}
	}
	if (p->n_slots >= p->max_slots) {
		return NULL;
	}
	struct alloy_slot *s = &p->slots[p->n_slots++];

	copy_name(s->chan, chan);
	s->active = ALLOY_POOL_NONE;
	s->since_ms = 0;
	s->sig = 0;
	return s;
}

static void start_buf(struct alloy_pool *p, struct alloy_slot *s, uint8_t idx, uint32_t sig,
		      uint32_t now_ms)
{
	struct alloy_buf *b = &p->bufs[idx];

	b->len = 0;
	copy_name(b->chan, s->chan);
	alloy_sanitize_name(b->chan);
	s->active = idx;
	s->since_ms = now_ms;
	s->sig = sig;
}

int alloy_pool_commit(struct alloy_pool *p, const char *chan, const char *hdr, int hlen,
		      const char *row, int rlen, uint32_t sig, uint32_t now_ms)
{
	if (hlen <= 0 || rlen <= 0 || (size_t)hlen + 1 + (size_t)rlen + 1 > p->buf_bytes) {
		p->dropped_rows++;
		return -ENOSPC;
	}

	struct alloy_slot *s = slot_for(p, chan);

	if (!s) {
		p->dropped_rows++;
		return -ENOSPC;
	}

	/* Field set changed: close the chunk so each file has one consistent schema. */
	if (s->active != ALLOY_POOL_NONE && p->bufs[s->active].len > 0 && s->sig != sig) {
		seal(p, s->active);
		s->active = ALLOY_POOL_NONE;
	}

	if (s->active == ALLOY_POOL_NONE) {
		uint8_t idx = get_free(p, ALLOY_POOL_NONE);

		if (idx == ALLOY_POOL_NONE) {
			p->dropped_rows++;
			return -ENOSPC;
		}
		start_buf(p, s, idx, sig, now_ms);
	}

	struct alloy_buf *b = &p->bufs[s->active];
	bool need_hdr = (b->len == 0);
	size_t need = (need_hdr ? (size_t)hlen + 1 : 0) + (size_t)rlen + 1;

	if (b->len + need > p->buf_bytes) {
		uint8_t sealed = s->active;
		uint8_t idx;

		seal(p, sealed);
		s->active = ALLOY_POOL_NONE;
		idx = get_free(p, sealed);
		if (idx == ALLOY_POOL_NONE) {
			p->dropped_rows++;
			return -ENOSPC;
		}
		start_buf(p, s, idx, sig, now_ms);
		b = &p->bufs[idx];
		need_hdr = true;
	}

	if (need_hdr) {
		memcpy(b->data + b->len, hdr, (size_t)hlen);
		b->len += (size_t)hlen;
		b->data[b->len++] = '\n';
	}
	memcpy(b->data + b->len, row, (size_t)rlen);
	b->len += (size_t)rlen;
	b->data[b->len++] = '\n';

	/* 90% full seals unconditionally: that is real backpressure. The timed flush claims its
	 * replacement atomically (or defers), and waits while a sealed chunk is still queued.
	 */
	if (b->len >= (p->buf_bytes * 9) / 10) {
		seal(p, s->active);
		s->active = ALLOY_POOL_NONE;
	} else if ((uint32_t)(now_ms - s->since_ms) >= p->flush_ms && p->pending_q.count == 0) {
		uint8_t nb;

		if (ring_pop(&p->free_q, &nb)) {
			uint8_t old = s->active;

			seal(p, old);
			p->bufs[nb].len = 0;
			memcpy(p->bufs[nb].chan, p->bufs[old].chan, ALLOY_CHAN_MAX);
			s->active = nb;
			s->since_ms = now_ms;
		}
	}
	return 0;
}

bool alloy_pool_take(struct alloy_pool *p, uint8_t *idx)
{
	return ring_pop(&p->pending_q, idx);
}

void alloy_pool_release(struct alloy_pool *p, uint8_t idx)
{
	p->bufs[idx].len = 0;
	ring_push(&p->free_q, idx);
}

unsigned int alloy_pool_flush_due(struct alloy_pool *p, uint32_t now_ms)
{
	unsigned int sealed = 0;

	for (uint8_t i = 0; i < p->n_slots; i++) {
		struct alloy_slot *s = &p->slots[i];
		uint8_t nb;

		if (p->pending_q.count > 0) {
			break; /* backlog: keep filling */
		}
		if (s->active == ALLOY_POOL_NONE || p->bufs[s->active].len == 0 ||
		    (uint32_t)(now_ms - s->since_ms) < p->flush_ms) {
			continue;
		}
		if (!ring_pop(&p->free_q, &nb)) {
			continue; /* no replacement yet; retry on the next tick */
		}
		uint8_t old = s->active;

		seal(p, old);
		p->bufs[nb].len = 0;
		memcpy(p->bufs[nb].chan, p->bufs[old].chan, ALLOY_CHAN_MAX);
		s->active = nb;
		s->since_ms = now_ms;
		sealed++;
	}
	return sealed;
}

void alloy_pool_seal_all(struct alloy_pool *p)
{
	for (uint8_t i = 0; i < p->n_slots; i++) {
		struct alloy_slot *s = &p->slots[i];

		if (s->active == ALLOY_POOL_NONE) {
			continue;
		}
		if (p->bufs[s->active].len > 0) {
			seal(p, s->active);
		} else {
			ring_push(&p->free_q, s->active);
		}
		s->active = ALLOY_POOL_NONE;
	}
}
