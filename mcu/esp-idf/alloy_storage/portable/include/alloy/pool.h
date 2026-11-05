// alloy/pool.h: the RAM buffer pool with store-and-forward, one open CSV chunk per channel.
//
// Platform-independent and not thread-safe: the caller serializes every call (the ESP-IDF
// component holds one mutex). Time is passed in as milliseconds from any monotonic clock.
//
// Buffers move free -> active (a channel is filling it) -> pending (sealed, FIFO) -> in flight
// (the uploader holds it) -> free. Policy, identical to the Arduino package:
//   * a channel seals its chunk at 90% full, or on a field-set change, and takes a fresh buffer;
//   * with no free buffer, the oldest pending chunk is shed (counted in dropped_bufs), except the
//     one just sealed by the same commit: then the new row is dropped instead;
//   * a timed flush (flush_ms after the chunk opened) seals a part-full chunk only when a
//     replacement buffer is free AND nothing is pending. During an outage the pending chunk stays
//     queued, so active chunks keep filling instead of spending the pool on mostly empty chunks.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alloy/record.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALLOY_POOL_MAX 32
#ifndef ALLOY_MAX_CHANNELS
#define ALLOY_MAX_CHANNELS 8
#endif

typedef struct {
    char *data;
    size_t len;
    char chan[ALLOY_CHAN_MAX];  // sanitized channel name
} alloy_buf_t;

typedef struct {
    char chan[ALLOY_CHAN_MAX];  // as logged (lookup key)
    int active;                 // buffer index, or -1
    uint32_t since_ms;          // when the active chunk opened
    uint32_t sig;               // field-set hash of the active chunk
} alloy_slot_t;

typedef struct {
    uint8_t idx[ALLOY_POOL_MAX];
    uint8_t head, count;
} alloy_ring_t;

typedef struct {
    alloy_buf_t *bufs;
    uint8_t n;
    size_t buf_bytes;
    uint32_t flush_ms;
    alloy_ring_t free_q, pending_q;
    uint8_t in_flight;
    alloy_slot_t slots[ALLOY_MAX_CHANNELS];
    uint8_t nslots;
    bool closed;            // end(): no more rows
    uint32_t dropped_bufs;  // sealed chunks shed under backpressure
    uint32_t dropped_rows;  // rows rejected before buffering
} alloy_pool_t;

// alloy_pool_commit result: a bitmask (a row can be dropped by the commit that seals a chunk).
#define ALLOY_COMMIT_OK      0u
#define ALLOY_COMMIT_SEALED  1u  // at least one chunk is newly pending: wake the uploader
#define ALLOY_COMMIT_DROPPED 2u  // row rejected (counted in dropped_rows)
#define ALLOY_COMMIT_CLOSED  4u  // pool closed by end(); not counted
typedef unsigned alloy_commit_t;

// bufs[i].data must point at buf_bytes of storage each. n is 1..ALLOY_POOL_MAX.
bool alloy_pool_init(alloy_pool_t *p, alloy_buf_t *bufs, uint8_t n, size_t buf_bytes,
                     uint32_t flush_ms);

alloy_commit_t alloy_pool_commit(alloy_pool_t *p, const alloy_csv_row_t *row, uint32_t now_ms);

// Uploader idle tick: timed flush of chunks older than flush_ms, under the same rules as commit.
// Returns true if anything was sealed.
bool alloy_pool_flush_due(alloy_pool_t *p, uint32_t now_ms);

// Oldest pending chunk for upload (now in flight), or -1.
int alloy_pool_take(alloy_pool_t *p);
// Uploader is done with a buffer (delivered or given up): back to free.
void alloy_pool_release(alloy_pool_t *p, int idx);

// end(): closes the pool to new rows, seals every non-empty active chunk, frees empty ones.
void alloy_pool_close(alloy_pool_t *p);

static inline unsigned alloy_pool_free(const alloy_pool_t *p) { return p->free_q.count; }
static inline unsigned alloy_pool_pending(const alloy_pool_t *p) { return p->pending_q.count; }
static inline unsigned alloy_pool_queued(const alloy_pool_t *p)
{
    return (unsigned)p->pending_q.count + p->in_flight;
}

#ifdef __cplusplus
}
#endif
