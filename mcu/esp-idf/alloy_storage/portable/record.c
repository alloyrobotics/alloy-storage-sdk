// record.c: CSV row encoding. See alloy/record.h.

#include "alloy/record.h"

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

void alloy_sanitize(char *s)
{
    for (; *s; s++) {
        char c = *s;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok) *s = '_';
    }
}

void alloy_csv_row_init(alloy_csv_row_t *r, const char *channel, uint64_t t_ns)
{
    r->done = false;
    r->chan[0] = 0;
    if (channel) {
        strncpy(r->chan, channel, ALLOY_CHAN_MAX - 1);
        r->chan[ALLOY_CHAN_MAX - 1] = 0;
    }
    memcpy(r->hdr, "t_ns", 5);
    r->hlen = 4;
    r->rlen = snprintf(r->row, ALLOY_ROW_MAX, "%019llu", (unsigned long long)t_ns);
    if (r->rlen != 19 || !r->chan[0]) {
        r->hlen = r->rlen = 0;
        r->done = true;
    }
    r->sig = alloy_fnv1a(r->hdr, (size_t)r->hlen, ALLOY_FNV_SEED);
}

bool alloy_csv_row_set(alloy_csv_row_t *r, const char *name, float v)
{
    if (r->done || !name || !name[0] || strpbrk(name, ",\r\n")) return false;
    // One snprintf per field. The name is appended with memcpy and folded into the field-set hash,
    // so the header text costs only a copy per row (it is written to a buffer once per chunk).
    size_t nn = strlen(name);
    if ((size_t)r->hlen + 1 + nn >= ALLOY_HDR_MAX) return false;  // ",name" + NUL
    int room = ALLOY_ROW_MAX - r->rlen;
    int rn;
    if (isfinite(v)) {
        rn = snprintf(r->row + r->rlen, (size_t)room, ",%.6g", (double)v);
    } else {
        rn = 1;  // empty cell = null in CSV
        if (room >= 2) {
            r->row[r->rlen] = ',';
            r->row[r->rlen + 1] = 0;
        }
    }
    if (rn <= 0 || rn >= room) {  // didn't fit: roll back so header and row stay aligned
        r->row[r->rlen] = 0;
        return false;
    }
    r->hdr[r->hlen] = ',';
    memcpy(r->hdr + r->hlen + 1, name, nn + 1);
    r->sig = alloy_fnv1a(r->hdr + r->hlen, 1 + nn, r->sig);
    r->hlen += 1 + (int)nn;
    r->rlen += rn;
    return true;
}

void alloy_rebase_rows(char *data, size_t len, uint64_t offset_ns)
{
    if (!offset_ns) return;
    char *p = data, *end = data + len;
    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t linelen = nl ? (size_t)(nl - p) : (size_t)(end - p);
        if (linelen >= 19 && (linelen == 19 || p[19] == ',')) {
            uint64_t v = 0;
            bool digits = true;
            for (int i = 0; i < 19 && digits; i++) {
                if (p[i] < '0' || p[i] > '9') digits = false;
                else v = v * 10 + (uint64_t)(p[i] - '0');
            }
            if (digits && v < ALLOY_BOOT_NS_LIMIT) {
                v += offset_ns;
                for (int i = 18; i >= 0; i--) {
                    p[i] = (char)('0' + (int)(v % 10));
                    v /= 10;
                }
            }
        }
        p += linelen + 1;
    }
}
