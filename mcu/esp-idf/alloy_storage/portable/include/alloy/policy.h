// alloy/policy.h: retry and backoff, the graceful-end gate, the HTTP Date clock fallback,
// mesh path validation and status text. Platform-independent; same policy as the Arduino package.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Local (non-HTTP) status values. Negative, so delivery code treats them as retryable unless
// stated otherwise. -1000..-1005 and -1010 match the Arduino package.
#define ALLOY_STATUS_OFFLINE       (-1000)  // no network interface with an IP
#define ALLOY_STATUS_CONFIG        (-1001)  // missing API key, bad mesh path or bad config
#define ALLOY_STATUS_MEMORY        (-1002)
#define ALLOY_STATUS_TASK          (-1003)
#define ALLOY_STATUS_DRAIN_TIMEOUT (-1004)
#define ALLOY_STATUS_DATA_LOSS     (-1005)  // a buffered chunk was dropped before delivery
#define ALLOY_STATUS_TRANSPORT     (-1006)  // DNS, TCP, TLS or HTTP transport error
#define ALLOY_STATUS_TLS_CONFIG    (-1007)  // no way to verify the server (see insecure)

// How long the uploader waits for SNTP before taking the server's Date header instead. Some
// networks filter or delay NTP for a minute or more; waiting would hold every buffer.
#define ALLOY_SNTP_GRACE_MS 5000u

// 0/negative (transport), 408, 425, 429 and 5xx retry; every other status is terminal.
bool alloy_retryable(int status);

// 1 s, 2 s, 4 s ... capped at 30 s.
uint32_t alloy_retry_delay_ms(uint8_t attempt);

// Parses an HTTP IMF-fixdate ("Wed, 04 Nov 2026 13:06:33 GMT", RFC 9110) to Unix seconds.
// The obsolete RFC 850 and asctime forms are rejected.
bool alloy_parse_http_date(const char *s, uint32_t *epoch);

typedef enum {
    ALLOY_END_WAITING,
    ALLOY_END_READY,
    ALLOY_END_FAILED,
} alloy_end_gate_t;

// A graceful end is ready only after metadata has settled and every buffer is back in the free
// pool. Returned buffers are not proof of delivery when one was terminally lost.
alloy_end_gate_t alloy_end_gate(bool meta_settled, bool meta_delivered, unsigned free_buffers,
                                unsigned total_buffers, int delivery_failure_status);

// ^[A-Za-z0-9_/-]{1,128}$ (the ingest service's rule), and no leading or trailing '/'.
bool alloy_valid_mesh_path(const char *path);

// X-Alloy-Finalize-Ms for a requested inactivity window, clamped to what the service accepts
// (30 s .. 30 min). 0 means "server default" and sends no header.
uint32_t alloy_finalize_ms(uint32_t seconds);

// Printable explanation of a status; "" for 0 and 2xx.
void alloy_status_text(int status, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
