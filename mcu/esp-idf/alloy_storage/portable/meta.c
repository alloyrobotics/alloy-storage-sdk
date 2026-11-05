// meta.c: see alloy/meta.h.

#include "alloy/meta.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    char *out;
    size_t cap, len;
    bool overflow;
} writer_t;

static void put(writer_t *w, const char *fmt, ...)
{
    if (w->overflow) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(w->out + w->len, w->cap - w->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= w->cap - w->len) w->overflow = true;
    else w->len += (size_t)n;
}

static void put_str(writer_t *w, const char *s)
{
    put(w, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') put(w, "\\%c", *p);
        else if (*p == '\n') put(w, "\\n");
        else if (*p == '\r') put(w, "\\r");
        else if (*p == '\t') put(w, "\\t");
        else if (*p < 0x20) put(w, "\\u%04x", *p);
        else put(w, "%c", *p);
    }
    put(w, "\"");
}

static void put_key_str(writer_t *w, const char *key, const char *value)
{
    put(w, ",\"%s\":", key);
    put_str(w, value);
}

size_t alloy_meta_json(const alloy_meta_t *m, char *out, size_t cap)
{
    if (!m || !out || cap == 0) return 0;
    writer_t w = {out, cap, 0, false};
    put(&w, "{\"device\":");
    put_str(&w, m->device ? m->device : "");
    if (m->firmware && m->firmware[0]) put_key_str(&w, "firmware", m->firmware);
    if (m->mission && m->mission[0]) put_key_str(&w, "mission", m->mission);

    time_t t = (time_t)m->session;
    struct tm tm;
    gmtime_r(&t, &tm);
    char iso[24];
    strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%SZ", &tm);
    put_key_str(&w, "session", iso);

    put(&w, ",\"fields\":[");
    for (size_t i = 0; i < m->field_count; i++) {
        const alloy_meta_field_t *f = &m->fields[i];
        put(&w, i ? ",{\"channel\":" : "{\"channel\":");
        put_str(&w, f->channel);
        put_key_str(&w, "name", f->name);
        if (f->unit[0]) put_key_str(&w, "unit", f->unit);
        if (isfinite(f->min)) put(&w, ",\"min\":%.7g", (double)f->min);
        if (isfinite(f->max)) put(&w, ",\"max\":%.7g", (double)f->max);
        if (f->about[0]) put_key_str(&w, "about", f->about);
        put(&w, "}");
    }
    put(&w, "]}");
    if (w.overflow) {
        out[0] = 0;
        return 0;
    }
    return w.len;
}

void alloy_copy_str(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    dst[0] = 0;
    if (!src) return;
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}
