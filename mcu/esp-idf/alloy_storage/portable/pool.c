// pool.c: buffer pool policy. See alloy/pool.h.

#include "alloy/pool.h"

#include <string.h>

static void ring_push(alloy_ring_t *q, uint8_t i)
{
    q->idx[(q->head + q->count) % ALLOY_POOL_MAX] = i;
    q->count++;
}

static int ring_pop(alloy_ring_t *q)
{
    if (!q->count) return -1;
    int i = q->idx[q->head];
    q->head = (uint8_t)((q->head + 1) % ALLOY_POOL_MAX);
    q->count--;
    return i;
}

bool alloy_pool_init(alloy_pool_t *p, alloy_buf_t *bufs, uint8_t n, size_t buf_bytes,
                     uint32_t flush_ms)
{
    if (!p || !bufs || n == 0 || n > ALLOY_POOL_MAX || buf_bytes == 0) return false;
    memset(p, 0, sizeof(*p));
    p->bufs = bufs;
    p->n = n;
    p->buf_bytes = buf_bytes;
    p->flush_ms = flush_ms;
    for (uint8_t i = 0; i < n; i++) {
        bufs[i].len = 0;
        bufs[i].chan[0] = 0;
        ring_push(&p->free_q, i);
    }
    return true;
}

static alloy_slot_t *slot_for(alloy_pool_t *p, const char *chan)
{
    for (uint8_t i = 0; i < p->nslots; i++)
        if (!strcmp(p->slots[i].chan, chan)) return &p->slots[i];
    if (p->nslots >= ALLOY_MAX_CHANNELS) return NULL;
    alloy_slot_t *s = &p->slots[p->nslots++];
    strncpy(s->chan, chan, ALLOY_CHAN_MAX - 1);
    s->chan[ALLOY_CHAN_MAX - 1] = 0;
    s->active = -1;
    s->since_ms = 0;
    s->sig = 0;
    return s;
}

// Queue a sealed chunk. The pending queue holds at most n entries, so it can't overflow while
// each buffer is in exactly one place; the guard keeps the invariant explicit.
static void seal(alloy_pool_t *p, int b)
{
    if (p->pending_q.count >= p->n) {
        int old = ring_pop(&p->pending_q);
        p->bufs[old].len = 0;
        p->dropped_bufs++;
        ring_push(&p->free_q, (uint8_t)old);
    }
    ring_push(&p->pending_q, (uint8_t)b);
}

// A free buffer, else the oldest pending one (shed). Never the chunk `avoid` that this commit
// just sealed: then the caller drops the new row instead.
static int get_free(alloy_pool_t *p, int avoid)
{
    int b = ring_pop(&p->free_q);
    if (b >= 0) return b;
    if (p->pending_q.count == 0) return -1;
    if (p->pending_q.idx[p->pending_q.head] == avoid) return -1;
    b = ring_pop(&p->pending_q);
    p->dropped_bufs++;
    p->bufs[b].len = 0;
    return b;
}

static void open_chunk(alloy_pool_t *p, alloy_slot_t *s, int b, uint32_t sig, uint32_t now_ms)
{
    alloy_buf_t *buf = &p->bufs[b];
    buf->len = 0;
    memcpy(buf->chan, s->chan, ALLOY_CHAN_MAX);
    alloy_sanitize(buf->chan);
    s->active = b;
    s->since_ms = now_ms;
    s->sig = sig;
}

// Timed flush of one slot: claim the replacement atomically or wait. Never while a chunk is
// pending (see the header).
static bool timed_flush(alloy_pool_t *p, alloy_slot_t *s, uint32_t now_ms)
{
    if (s->active < 0 || p->bufs[s->active].len == 0) return false;
    if ((uint32_t)(now_ms - s->since_ms) < p->flush_ms) return false;
    if (p->pending_q.count > 0 || p->free_q.count == 0) return false;
    int nb = ring_pop(&p->free_q);
    seal(p, s->active);
    open_chunk(p, s, nb, s->sig, now_ms);
    return true;
}

alloy_commit_t alloy_pool_commit(alloy_pool_t *p, const alloy_csv_row_t *r, uint32_t now_ms)
{
    if (p->closed) return ALLOY_COMMIT_CLOSED;
    if (r->done || r->hlen <= 0 || r->rlen <= 0 ||
        (size_t)r->hlen + 1 + (size_t)r->rlen + 1 > p->buf_bytes) {
        p->dropped_rows++;  // unusable, or can never fit a buffer
        return ALLOY_COMMIT_DROPPED;
    }
    alloy_slot_t *s = slot_for(p, r->chan);
    if (!s) {
        p->dropped_rows++;  // too many channels
        return ALLOY_COMMIT_DROPPED;
    }
    bool sealed = false;
    int just_sealed = -1;

    // Field set changed mid-stream: close the chunk so each one has one consistent schema.
    if (s->active >= 0 && p->bufs[s->active].len > 0 && s->sig != r->sig) {
        just_sealed = s->active;
        seal(p, just_sealed);
        s->active = -1;
        sealed = true;
    }
    if (s->active < 0) {
        int b = get_free(p, just_sealed);
        if (b < 0) {
            p->dropped_rows++;
            return ALLOY_COMMIT_DROPPED | (sealed ? ALLOY_COMMIT_SEALED : 0);
        }
        open_chunk(p, s, b, r->sig, now_ms);
    }

    alloy_buf_t *b = &p->bufs[s->active];
    bool need_hdr = b->len == 0;
    size_t need = (need_hdr ? (size_t)r->hlen + 1 : 0) + (size_t)r->rlen + 1;
    if (b->len + need > p->buf_bytes) {  // chunk full: seal it and start a fresh one
        int full = s->active;
        seal(p, full);
        sealed = true;
        s->active = -1;
        int nb = get_free(p, full);
        if (nb < 0) {
            p->dropped_rows++;
            return ALLOY_COMMIT_SEALED | ALLOY_COMMIT_DROPPED;
        }
        open_chunk(p, s, nb, r->sig, now_ms);
        b = &p->bufs[nb];
        need_hdr = true;
    }
    if (need_hdr) {
        memcpy(b->data + b->len, r->hdr, (size_t)r->hlen);
        b->len += (size_t)r->hlen;
        b->data[b->len++] = '\n';
    }
    memcpy(b->data + b->len, r->row, (size_t)r->rlen);
    b->len += (size_t)r->rlen;
    b->data[b->len++] = '\n';

    // 90% full seals unconditionally: that is genuine backpressure.
    if (b->len >= (p->buf_bytes * 9) / 10) {
        seal(p, s->active);
        s->active = -1;
        sealed = true;
    } else if (timed_flush(p, s, now_ms)) {
        sealed = true;
    }
    return sealed ? ALLOY_COMMIT_SEALED : ALLOY_COMMIT_OK;
}

bool alloy_pool_flush_due(alloy_pool_t *p, uint32_t now_ms)
{
    bool sealed = false;
    for (uint8_t i = 0; i < p->nslots; i++)
        if (timed_flush(p, &p->slots[i], now_ms)) sealed = true;
    return sealed;
}

int alloy_pool_take(alloy_pool_t *p)
{
    int b = ring_pop(&p->pending_q);
    if (b >= 0) p->in_flight++;
    return b;
}

void alloy_pool_release(alloy_pool_t *p, int idx)
{
    if (idx < 0 || idx >= p->n || p->in_flight == 0) return;
    p->in_flight--;
    p->bufs[idx].len = 0;
    ring_push(&p->free_q, (uint8_t)idx);
}

void alloy_pool_close(alloy_pool_t *p)
{
    p->closed = true;
    for (uint8_t i = 0; i < p->nslots; i++) {
        alloy_slot_t *s = &p->slots[i];
        if (s->active < 0) continue;
        if (p->bufs[s->active].len > 0) seal(p, s->active);
        else ring_push(&p->free_q, (uint8_t)s->active);
        s->active = -1;
    }
}
