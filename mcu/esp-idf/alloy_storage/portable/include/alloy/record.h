// alloy/record.h: one CSV row being built, plus the small text helpers around it.
//
// Platform-independent (no ESP-IDF, no FreeRTOS) so it can move into core/ and is unit-tested on
// the host. Wire format is identical to the Arduino package: a header line "t_ns,<field>,..." and
// value rows "<19-digit t_ns>,<%.6g>,..." with an empty cell for NaN/Inf.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ALLOY_CHAN_MAX
#define ALLOY_CHAN_MAX 24  // bytes in a channel name, NUL included
#endif
#ifndef ALLOY_HDR_MAX
#define ALLOY_HDR_MAX 320  // bytes in one CSV header line
#endif
#ifndef ALLOY_ROW_MAX
#define ALLOY_ROW_MAX 256  // bytes in one CSV value row
#endif

// Real epoch seconds are above this; anything below is seconds since boot (clock not set yet).
#define ALLOY_EPOCH_SANE 1700000000u
// Boot-relative stamps are below 1e18 ns; real epoch ns are about 1.8e18.
#define ALLOY_BOOT_NS_LIMIT 1000000000000000000ull

typedef struct {
    char chan[ALLOY_CHAN_MAX];
    char hdr[ALLOY_HDR_MAX];  // "t_ns,heading,pitch,roll"
    char row[ALLOY_ROW_MAX];  // "1782715694000000000,-1.23,0.52,3.14"
    int hlen, rlen;
    uint32_t sig;             // FNV-1a of the header text, folded as fields are added
    bool done;                // committed, or unusable
} alloy_csv_row_t;

// Starts a row stamped with t_ns (zero-padded to 19 digits so a boot-relative stamp can be
// rebased in place later). An empty or NULL channel marks the row unusable.
void alloy_csv_row_init(alloy_csv_row_t *r, const char *channel, uint64_t t_ns);

// Appends one field. Returns false (and leaves the row unchanged) when the name is empty or holds
// a CSV delimiter, or when the field does not fit the header or row; earlier fields are kept.
bool alloy_csv_row_set(alloy_csv_row_t *r, const char *name, float value);

// FNV-1a, seedable so fields can be folded in one at a time. Seed with ALLOY_FNV_SEED.
#define ALLOY_FNV_SEED 2166136261u
uint32_t alloy_fnv1a(const char *s, size_t n, uint32_t h);

// Ingest object keys allow [A-Za-z0-9_-]; anything else becomes '_'.
void alloy_sanitize(char *s);

// Rewrites every boot-relative 19-digit row stamp in a CSV chunk to wall clock by adding
// offset_ns. Same width, so the chunk layout does not change. Header lines are left alone.
void alloy_rebase_rows(char *data, size_t len, uint64_t offset_ns);

#ifdef __cplusplus
}
#endif
