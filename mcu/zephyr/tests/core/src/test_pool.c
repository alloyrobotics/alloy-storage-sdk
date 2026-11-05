/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "alloy_csv.h"
#include "alloy_pool.h"

#define NBUF  4
#define BYTES 12288
#define FLUSH 4000

static char mem[NBUF * BYTES];
static struct alloy_pool pool;

static void reset(void *f)
{
	ARG_UNUSED(f);
	zassert_ok(alloy_pool_init(&pool, mem, NBUF, BYTES, 8, FLUSH));
}

ZTEST_SUITE(alloy_pool, NULL, NULL, reset, NULL, NULL);

static int commit(const char *chan, uint64_t ts, const char *field, float v, uint32_t now)
{
	struct alloy_record r;

	alloy_record_init(&r, chan, ts);
	alloy_record_set(&r, field, v);
	return alloy_pool_commit(&pool, r.chan, r.hdr, r.hlen, r.row, r.rlen, r.sig, now);
}

/* Drains every sealed chunk as an uploader would, appending the bodies to out. */
static size_t drain(char *out, size_t cap)
{
	size_t len = 0;
	uint8_t idx;

	while (alloy_pool_take(&pool, &idx)) {
		struct alloy_buf *b = &pool.bufs[idx];

		zassert_true(len + b->len < cap);
		memcpy(out + len, b->data, b->len);
		len += b->len;
		alloy_pool_release(&pool, idx);
	}
	out[len] = '\0';
	return len;
}

ZTEST(alloy_pool, test_first_row_carries_the_header)
{
	uint8_t idx;

	zassert_ok(commit("imu", 1, "pitch", 1.0f, 0));
	zassert_ok(commit("imu", 2, "pitch", 2.0f, 10));
	alloy_pool_seal_all(&pool);
	zassert_true(alloy_pool_take(&pool, &idx));
	zassert_str_equal(pool.bufs[idx].chan, "imu");
	pool.bufs[idx].data[pool.bufs[idx].len] = '\0';
	zassert_str_equal(pool.bufs[idx].data, "t_ns,pitch\n"
					       "0000000000000000001,1\n"
					       "0000000000000000002,2\n");
}

ZTEST(alloy_pool, test_buffer_channel_name_is_sanitized)
{
	uint8_t idx;

	zassert_ok(commit("left wheel", 1, "rpm", 1.0f, 0));
	alloy_pool_seal_all(&pool);
	zassert_true(alloy_pool_take(&pool, &idx));
	zassert_str_equal(pool.bufs[idx].chan, "left_wheel");
}

ZTEST(alloy_pool, test_timed_flush_seals_when_nothing_is_queued)
{
	zassert_ok(commit("a", 1, "v", 1.0f, 0));
	zassert_ok(commit("a", 2, "v", 1.0f, FLUSH - 1));
	zassert_equal(alloy_pool_pending_count(&pool), 0);
	zassert_ok(commit("a", 3, "v", 1.0f, FLUSH));
	zassert_equal(alloy_pool_pending_count(&pool), 1, "flush interval elapsed");
	zassert_equal(alloy_pool_free_count(&pool), NBUF - 2, "replacement claimed atomically");
}

ZTEST(alloy_pool, test_timed_flush_waits_while_a_chunk_is_queued)
{
	/* The outage fix: with a sealed chunk still waiting, keep filling instead of sealing
	 * part-full buffers on the timer.
	 */
	zassert_ok(commit("a", 1, "v", 1.0f, 0));
	zassert_ok(commit("a", 2, "v", 1.0f, FLUSH));
	zassert_equal(alloy_pool_pending_count(&pool), 1);
	for (uint32_t t = FLUSH; t < 10 * FLUSH; t += 100) {
		zassert_ok(commit("a", t, "v", 1.0f, t));
	}
	zassert_equal(alloy_pool_pending_count(&pool), 1, "no timed seal while backlogged");
	zassert_equal(alloy_pool_flush_due(&pool, 20 * FLUSH), 0, "idle flush also waits");
}

ZTEST(alloy_pool, test_idle_flush_seals_due_channels)
{
	zassert_ok(commit("a", 1, "v", 1.0f, 0));
	zassert_equal(alloy_pool_flush_due(&pool, FLUSH - 1), 0);
	zassert_equal(alloy_pool_flush_due(&pool, FLUSH), 1);
	zassert_equal(alloy_pool_pending_count(&pool), 1);
}

ZTEST(alloy_pool, test_ninety_percent_full_seals)
{
	uint32_t rows = 0;

	while (alloy_pool_pending_count(&pool) == 0) {
		zassert_ok(commit("a", rows, "v", 123.456f, 0));
		rows++;
	}
	uint8_t idx;

	zassert_true(alloy_pool_take(&pool, &idx));
	zassert_true(pool.bufs[idx].len >= BYTES * 9 / 10);
	zassert_true(pool.bufs[idx].len <= BYTES);
}

ZTEST(alloy_pool, test_field_set_change_starts_a_new_chunk)
{
	char out[256];
	struct alloy_record r;

	zassert_ok(commit("a", 1, "x", 1.0f, 0));
	alloy_record_init(&r, "a", 2);
	alloy_record_set(&r, "x", 2.0f);
	alloy_record_set(&r, "y", 3.0f);
	zassert_ok(alloy_pool_commit(&pool, r.chan, r.hdr, r.hlen, r.row, r.rlen, r.sig, 1));
	zassert_equal(alloy_pool_pending_count(&pool), 1);
	alloy_pool_seal_all(&pool);
	drain(out, sizeof(out));
	zassert_str_equal(out, "t_ns,x\n0000000000000000001,1\n"
			       "t_ns,x,y\n0000000000000000002,2,3\n");
}

ZTEST(alloy_pool, test_sheds_oldest_pending_only_when_no_buffer_is_free)
{
	/* Fill all four buffers on four channels and seal three: one active, three pending,
	 * none free. A fifth channel can only get a buffer by shedding the oldest chunk.
	 */
	zassert_ok(alloy_pool_init(&pool, mem, NBUF, BYTES, 8, FLUSH));
	for (int c = 0; c < 4; c++) {
		char chan[4];

		snprintf(chan, sizeof(chan), "c%d", c);
		zassert_ok(commit(chan, 1, "v", 1.0f, 0));
	}
	zassert_equal(alloy_pool_free_count(&pool), 0);
	zassert_equal(alloy_pool_flush_due(&pool, FLUSH), 0, "no replacement, so no timed seal");
	zassert_equal(pool.dropped_bufs, 0);
	alloy_pool_seal_all(&pool);
	zassert_equal(alloy_pool_pending_count(&pool), 4);

	zassert_ok(commit("c4", 2, "v", 1.0f, FLUSH));
	zassert_equal(pool.dropped_bufs, 1, "oldest pending shed for the new channel");
	zassert_equal(alloy_pool_pending_count(&pool), 3);
}

ZTEST(alloy_pool, test_never_sheds_the_chunk_just_sealed)
{
	/* Two 230-byte buffers, one in flight. 74-byte rows overflow the buffer before it reaches
	 * the 90% seal, so the overflow path seals it into the only pending slot and must drop the
	 * incoming row rather than recycle the chunk it just sealed.
	 */
	struct alloy_record r;
	uint8_t inflight;
	int rc = 0;

	zassert_ok(alloy_pool_init(&pool, mem, 2, 230, 8, FLUSH));
	zassert_ok(commit("a", 0, "v", 1.0f, 0));
	alloy_pool_seal_all(&pool);
	zassert_true(alloy_pool_take(&pool, &inflight));

	for (int i = 0; i < 4 && rc == 0; i++) {
		alloy_record_init(&r, "b", (uint64_t)i);
		for (char f = 'a'; f <= 'f'; f++) {
			char name[2] = {f, '\0'};

			alloy_record_set(&r, name, -1.23456f);
		}
		rc = alloy_pool_commit(&pool, r.chan, r.hdr, r.hlen, r.row, r.rlen, r.sig, 0);
	}
	zassert_equal(rc, -ENOSPC);
	zassert_equal(pool.dropped_bufs, 0, "the sealed chunk survives");
	zassert_equal(pool.dropped_rows, 1);
	zassert_equal(alloy_pool_pending_count(&pool), 1);
	zassert_equal(pool.bufs[pool.pending_q.idx[pool.pending_q.head]].len, 17 + 2 * 74);
}

ZTEST(alloy_pool, test_rejects_oversize_rows_and_extra_channels)
{
	char out[64];

	zassert_ok(alloy_pool_init(&pool, mem, NBUF, 64, 2, FLUSH));
	struct alloy_record r;

	alloy_record_init(&r, "a", 0);
	alloy_record_set(&r, "a_rather_long_field_name", 1.0f);
	alloy_record_set(&r, "another_long_field_name", 2.0f);
	zassert_equal(alloy_pool_commit(&pool, r.chan, r.hdr, r.hlen, r.row, r.rlen, r.sig, 0),
		      -ENOSPC);
	zassert_ok(commit("a", 0, "v", 1.0f, 0));
	zassert_ok(commit("b", 0, "v", 1.0f, 0));
	zassert_equal(commit("c", 0, "v", 1.0f, 0), -ENOSPC);
	zassert_equal(pool.dropped_rows, 2);
	alloy_pool_seal_all(&pool);
	drain(out, sizeof(out));
}

ZTEST(alloy_pool, test_seal_all_frees_empty_buffers)
{
	zassert_ok(commit("a", 1, "v", 1.0f, 0));
	zassert_ok(commit("a", 2, "v", 1.0f, FLUSH)); /* timed seal, fresh empty active */
	zassert_equal(alloy_pool_free_count(&pool), NBUF - 2);
	alloy_pool_seal_all(&pool);
	zassert_equal(alloy_pool_pending_count(&pool), 1);
	zassert_equal(alloy_pool_free_count(&pool), NBUF - 1);
}

/* Gate 3 in the lab, in miniature: 20 Hz sequence counter (~30 B/row, as on the C3), the
 * uploader stuck for 25 s from the moment the first chunk is queued, then the backlog drains.
 * Every value must come out exactly once and nothing may be shed.
 */
ZTEST(alloy_pool, test_outage_keeps_every_row)
{
	static char out[NBUF * BYTES * 4];
	uint32_t now = 0;
	uint32_t seq = 0;
	uint32_t outage_start = UINT32_MAX;
	size_t len = 0;

	for (; now < 70000; now += 50) {
		bool outage = outage_start != UINT32_MAX && now - outage_start < 25000;
		struct alloy_record r;

		alloy_record_init(&r, "seq", 1782715694000000000ULL + (uint64_t)now * 1000000ULL);
		alloy_record_set(&r, "seq", (float)seq++);
		alloy_record_set(&r, "t_s", (float)now / 1000.0f);
		zassert_ok(alloy_pool_commit(&pool, r.chan, r.hdr, r.hlen, r.row, r.rlen, r.sig,
					     now));
		if (outage_start == UINT32_MAX && alloy_pool_pending_count(&pool) > 0) {
			outage_start = now;
		}
		if (!outage && now % 250 == 0) {
			alloy_pool_flush_due(&pool, now);
			len += drain(out + len, sizeof(out) - len);
		}
	}
	alloy_pool_seal_all(&pool);
	len += drain(out + len, sizeof(out) - len);
	zassert_equal(pool.dropped_bufs, 0, "outage shed a buffer");
	zassert_equal(pool.dropped_rows, 0);

	/* Every sequence value exactly once, in order. */
	uint32_t expect = 0;
	char *line = strtok(out, "\n");

	while (line) {
		if (strncmp(line, "t_ns", 4) != 0) {
			char *comma = strchr(line, ',');

			zassert_not_null(comma);
			zassert_equal((uint32_t)strtoul(comma + 1, NULL, 10), expect, "row %u",
				      expect);
			expect++;
		}
		line = strtok(NULL, "\n");
	}
	zassert_equal(expect, seq);
}
