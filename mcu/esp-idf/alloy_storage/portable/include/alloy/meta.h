// alloy/meta.h: the run's metadata sidecar (POST /v1/meta), same JSON shape as the Arduino
// package: {"device","firmware","mission","session":"<ISO-8601 UTC>","fields":[...]}.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char channel[24];
    char name[20];
    char unit[12];
    char about[48];
    float min, max;  // emitted only when finite
} alloy_meta_field_t;

typedef struct {
    const char *device;
    const char *firmware;  // optional
    const char *mission;   // optional
    uint32_t session;      // epoch seconds
    const alloy_meta_field_t *fields;
    size_t field_count;
} alloy_meta_t;

// Writes NUL-terminated JSON into out. Returns its length, or 0 if it does not fit in cap.
size_t alloy_meta_json(const alloy_meta_t *m, char *out, size_t cap);

// Copies src into a fixed field, truncating. NULL becomes "".
void alloy_copy_str(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif
