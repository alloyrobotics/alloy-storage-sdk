/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * CSV row encoding for Alloy Storage chunks. Platform independent: no Zephyr headers.
 *
 * A chunk is one header line "t_ns,<field>,<field>,..." followed by bare value rows. The stamp is
 * always 19 zero-padded digits so rows stamped before the clock is set (boot-relative ns, below
 * 1e18) can be rebased to wall-clock ns in place, without moving any bytes.
 */

#ifndef ALLOY_CORE_CSV_H_
#define ALLOY_CORE_CSV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALLOY_CHAN_MAX
#define ALLOY_CHAN_MAX 24 /* bytes in a channel name, NUL included */
#endif
#ifndef ALLOY_HDR_MAX
#define ALLOY_HDR_MAX 320 /* bytes in one CSV header line */
#endif
#ifndef ALLOY_ROW_MAX
#define ALLOY_ROW_MAX 256 /* bytes in one CSV value row */
#endif

#define ALLOY_TS_DIGITS    19
/* Stamps below this are boot-relative; real epoch ns are about 1.8e18. */
#define ALLOY_WALL_NS_MIN  1000000000000000000ULL
#define ALLOY_FNV_SEED     2166136261u

/* One row under construction: field names (header) and values (row) side by side. */
struct alloy_record {
	char chan[ALLOY_CHAN_MAX];
	char hdr[ALLOY_HDR_MAX];
	char row[ALLOY_ROW_MAX];
	int hlen;
	int rlen;
	uint32_t sig; /* FNV-1a of the header text, folded as fields are added */
	bool done;    /* nothing more may be added (or the record was never valid) */
};

/* FNV-1a, seedable so names can be folded in one at a time. */
uint32_t alloy_fnv1a(const char *s, size_t n, uint32_t h);

/* Object keys allow [A-Za-z0-9_-]; anything else becomes '_'. */
void alloy_sanitize_name(char *s);

/* Writes exactly ALLOY_TS_DIGITS zero-padded digits (no NUL). */
void alloy_format_ts(char *out, uint64_t ns);

void alloy_record_init(struct alloy_record *r, const char *chan, uint64_t t_ns);

/* Appends one field. NaN/inf become an empty cell (null). A field that does not fit in either the
 * header or the row is skipped, so header and row always stay aligned.
 */
void alloy_record_set(struct alloy_record *r, const char *name, float value);

/* Rewrites boot-relative stamps at the start of each line to stamp + offset_ns, in place.
 * Returns the number of rows rewritten. Header lines and wall-clock stamps are left alone.
 */
size_t alloy_rebase_rows(char *data, size_t len, uint64_t offset_ns);

#ifdef __cplusplus
}
#endif

#endif /* ALLOY_CORE_CSV_H_ */
