/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include "alloy_meta.h"
#include "alloy_policy.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void copy_trunc(char *dst, size_t cap, const char *src)
{
	size_t n = src ? strlen(src) : 0;

	if (n >= cap) {
		n = cap - 1;
	}
	if (n) {
		memcpy(dst, src, n);
	}
	dst[n] = '\0';
}

void alloy_desc_set(struct alloy_desc *d, const char *chan, const char *field, const char *unit,
		    float lo, float hi, const char *about)
{
	copy_trunc(d->chan, sizeof(d->chan), chan);
	copy_trunc(d->field, sizeof(d->field), field);
	copy_trunc(d->unit, sizeof(d->unit), unit);
	copy_trunc(d->about, sizeof(d->about), about);
	d->lo = lo;
	d->hi = hi;
}

struct writer {
	char *out;
	size_t cap;
	size_t len;
	bool overflow;
};

static void put(struct writer *w, const char *s, size_t n)
{
	if (w->overflow || w->len + n >= w->cap) {
		w->overflow = true;
		return;
	}
	memcpy(w->out + w->len, s, n);
	w->len += n;
	w->out[w->len] = '\0';
}

static void putf(struct writer *w, const char *fmt, ...)
{
	char tmp[32];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0 || (size_t)n >= sizeof(tmp)) {
		w->overflow = true;
		return;
	}
	put(w, tmp, (size_t)n);
}

static void put_str(struct writer *w, const char *s)
{
	put(w, "\"", 1);
	for (; *s; s++) {
		unsigned char c = (unsigned char)*s;

		if (c == '"' || c == '\\') {
			char esc[2] = {'\\', (char)c};

			put(w, esc, 2);
		} else if (c == '\n') {
			put(w, "\\n", 2);
		} else if (c == '\r') {
			put(w, "\\r", 2);
		} else if (c == '\t') {
			put(w, "\\t", 2);
		} else if (c < 0x20) {
			putf(w, "\\u%04x", c);
		} else {
			put(w, (const char *)&c, 1);
		}
	}
	put(w, "\"", 1);
}

static void put_key(struct writer *w, const char *key, bool *first)
{
	if (!*first) {
		put(w, ",", 1);
	}
	*first = false;
	put_str(w, key);
	put(w, ":", 1);
}

int alloy_meta_json(char *out, size_t cap, const char *device, const char *firmware,
		    const char *mission, uint32_t session, const struct alloy_desc *descs,
		    size_t n_descs)
{
	struct writer w = {.out = out, .cap = cap};
	bool first = true;
	char iso[21];

	if (!out || cap == 0) {
		return -ENOSPC;
	}
	out[0] = '\0';
	put(&w, "{", 1);
	put_key(&w, "device", &first);
	put_str(&w, device ? device : "");
	if (firmware && firmware[0]) {
		put_key(&w, "firmware", &first);
		put_str(&w, firmware);
	}
	if (mission && mission[0]) {
		put_key(&w, "mission", &first);
		put_str(&w, mission);
	}
	alloy_epoch_to_iso8601(session, iso);
	put_key(&w, "session", &first);
	put_str(&w, iso);
	put_key(&w, "fields", &first);
	put(&w, "[", 1);
	for (size_t i = 0; i < n_descs; i++) {
		const struct alloy_desc *d = &descs[i];
		bool ffirst = true;

		if (i) {
			put(&w, ",", 1);
		}
		put(&w, "{", 1);
		put_key(&w, "channel", &ffirst);
		put_str(&w, d->chan);
		put_key(&w, "name", &ffirst);
		put_str(&w, d->field);
		if (d->unit[0]) {
			put_key(&w, "unit", &ffirst);
			put_str(&w, d->unit);
		}
		if (isfinite(d->lo)) {
			put_key(&w, "min", &ffirst);
			putf(&w, "%.7g", (double)d->lo);
		}
		if (isfinite(d->hi)) {
			put_key(&w, "max", &ffirst);
			putf(&w, "%.7g", (double)d->hi);
		}
		if (d->about[0]) {
			put_key(&w, "about", &ffirst);
			put_str(&w, d->about);
		}
		put(&w, "}", 1);
	}
	put(&w, "]}", 2);
	return w.overflow ? -ENOSPC : (int)w.len;
}
