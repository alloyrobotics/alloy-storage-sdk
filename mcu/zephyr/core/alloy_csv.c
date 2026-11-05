/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include "alloy_csv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

uint32_t alloy_fnv1a(const char *s, size_t n, uint32_t h)
{
	for (size_t i = 0; i < n; i++) {
		h ^= (uint8_t)s[i];
		h *= 16777619u;
	}
	return h;
}

void alloy_sanitize_name(char *s)
{
	for (; *s; s++) {
		char c = *s;
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '-' || c == '_';
		if (!ok) {
			*s = '_';
		}
	}
}

void alloy_format_ts(char *out, uint64_t ns)
{
	for (int i = ALLOY_TS_DIGITS - 1; i >= 0; i--) {
		out[i] = (char)('0' + (ns % 10));
		ns /= 10;
	}
}

void alloy_record_init(struct alloy_record *r, const char *chan, uint64_t t_ns)
{
	size_t n = chan ? strlen(chan) : 0;

	if (n >= ALLOY_CHAN_MAX) {
		n = ALLOY_CHAN_MAX - 1;
	}
	memcpy(r->chan, chan ? chan : "", n);
	r->chan[n] = '\0';

	memcpy(r->hdr, "t_ns", 5);
	r->hlen = 4;
	alloy_format_ts(r->row, t_ns);
	r->row[ALLOY_TS_DIGITS] = '\0';
	r->rlen = ALLOY_TS_DIGITS;
	r->sig = alloy_fnv1a(r->hdr, (size_t)r->hlen, ALLOY_FNV_SEED);
	r->done = (n == 0);
}

void alloy_record_set(struct alloy_record *r, const char *name, float value)
{
	if (r->done || !name) {
		return;
	}

	int nn = (int)strlen(name);
	bool hfits = r->hlen + 1 + nn < ALLOY_HDR_MAX; /* ",name" + NUL */
	int rn;

	if (isfinite(value)) {
		rn = snprintf(r->row + r->rlen, (size_t)(ALLOY_ROW_MAX - r->rlen), ",%.6g",
			      (double)value);
	} else if (r->rlen + 2 <= ALLOY_ROW_MAX) {
		r->row[r->rlen] = ',';
		r->row[r->rlen + 1] = '\0';
		rn = 1;
	} else {
		rn = ALLOY_ROW_MAX; /* force the rollback below */
	}

	if (nn > 0 && hfits && rn > 0 && rn < ALLOY_ROW_MAX - r->rlen) {
		r->hdr[r->hlen] = ',';
		memcpy(r->hdr + r->hlen + 1, name, (size_t)nn + 1);
		r->sig = alloy_fnv1a(r->hdr + r->hlen, (size_t)(1 + nn), r->sig);
		r->hlen += 1 + nn;
		r->rlen += rn;
	} else {
		r->hdr[r->hlen] = '\0';
		r->row[r->rlen] = '\0';
	}
}

size_t alloy_rebase_rows(char *data, size_t len, uint64_t offset_ns)
{
	size_t rebased = 0;
	char *p = data;
	char *end = data + len;

	if (offset_ns == 0) {
		return 0;
	}
	while (p < end) {
		char *nl = memchr(p, '\n', (size_t)(end - p));
		size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);

		if (linelen >= ALLOY_TS_DIGITS &&
		    (linelen == ALLOY_TS_DIGITS || p[ALLOY_TS_DIGITS] == ',')) {
			uint64_t v = 0;
			bool digits = true;

			for (int i = 0; i < ALLOY_TS_DIGITS; i++) {
				if (p[i] < '0' || p[i] > '9') {
					digits = false;
					break;
				}
				v = v * 10 + (uint64_t)(p[i] - '0');
			}
			if (digits && v < ALLOY_WALL_NS_MIN) {
				alloy_format_ts(p, v + offset_ns);
				rebased++;
			}
		}
		p += linelen + 1;
	}
	return rebased;
}
