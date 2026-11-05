// alloy_storage.h: Alloy Storage SDK for ESP-IDF. Stream robot telemetry straight to Alloy Storage.
//
// Log named fields on a channel; the component buffers them in RAM and uploads CSV chunks to the
// Alloy Storage ingest service from a background task. Each power-on becomes one indexed MCAP
// in your mesh. Same wire protocol and delivery policy as the Arduino package.
//
//     alloy_storage_config_t cfg = ALLOY_STORAGE_DEFAULT_CONFIG();
//     cfg.api_key = CONFIG_MY_ALLOY_API_KEY;
//     cfg.mesh_path = "robots/sbr";
//     alloy_storage_handle_t alloy;
//     ESP_ERROR_CHECK(alloy_storage_begin(&cfg, &alloy));
//
//     alloy_record_t r;
//     alloy_record_begin(&r, alloy, "imu");
//     alloy_record_set(&r, "pitch", pitch);
//     alloy_record_set(&r, "roll", roll);
//     alloy_record_commit(&r);
//
//     alloy_storage_end(alloy, 15000);   // optional: deliver everything and finalize now
//
// The network must already be up (or come up later: delivery waits for it). For a quick start,
// alloy_storage_wifi.h brings up a station interface with automatic reconnect.
//
// MIT licensed.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "alloy/record.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alloy_storage *alloy_storage_handle_t;

// Optional semantics for one field (unit, range, description), sent in the run's metadata.
typedef struct {
    const char *channel;
    const char *name;
    const char *unit;   // optional
    float min, max;     // used only when has_range is true
    bool has_range;
    const char *about;  // optional
} alloy_field_desc_t;

typedef struct {
    const char *api_key;    // required: Alloy data API key
    const char *mesh_path;  // required: ^[A-Za-z0-9_/-]{1,128}$, no leading or trailing '/'
    const char *device_id;  // NULL: the app's project name (sanitized, max 23 chars)
    const char *firmware;   // NULL: the app version from esp_app_desc
    const char *mission;    // optional human-readable label for this run
    const char *ingest_url; // NULL: CONFIG_ALLOY_INGEST_URL
    const char *sntp_server;  // NULL: CONFIG_ALLOY_SNTP_SERVER; "" leaves SNTP to the app
    uint8_t buffer_count;     // 0: CONFIG_ALLOY_POOL_SIZE
    size_t buffer_bytes;      // 0: CONFIG_ALLOY_BUFFER_BYTES
    uint32_t flush_interval_ms;  // 0: CONFIG_ALLOY_FLUSH_INTERVAL_MS
    uint32_t finalize_after_s;   // inactivity before the service finalizes the run (30..1800);
                                 // 0: service default (2 min)
    // Skip TLS server verification. Only for networks where verification can't work
    // (TLS-intercepting proxies). Also requires CONFIG_ESP_TLS_INSECURE and
    // CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY; without them begin() fails.
    bool insecure;
    const alloy_field_desc_t *fields;  // optional, copied at begin()
    size_t field_count;
} alloy_storage_config_t;

#define ALLOY_STORAGE_DEFAULT_CONFIG() { 0 }

// Validates the config, allocates the buffer pool, starts SNTP (unless the clock is already set
// or sntp_server is "") and the upload task. One instance per app.
//   ESP_ERR_INVALID_ARG    missing API key, invalid mesh path or out-of-range config
//   ESP_ERR_NOT_SUPPORTED  no TLS verification available and insecure not opted into
//   ESP_ERR_NO_MEM         buffers or task could not be allocated
//   ESP_ERR_INVALID_STATE  already started
esp_err_t alloy_storage_begin(const alloy_storage_config_t *config, alloy_storage_handle_t *out);

// ---- logging (any task; not from an ISR) ----

typedef struct {
    alloy_storage_handle_t owner;
    alloy_csv_row_t row;
} alloy_record_t;

// Starts one row on channel, stamped now. Stack-allocate the record (about 620 bytes).
void alloy_record_begin(alloy_record_t *r, alloy_storage_handle_t h, const char *channel);
// Adds a field. NaN and Inf are stored as null. Returns false when the field was skipped (name
// empty or containing ',' or a newline, or the row is full).
bool alloy_record_set(alloy_record_t *r, const char *name, float value);
// Buffers the row. ESP_ERR_NO_MEM: dropped (counted in dropped_rows). ESP_ERR_INVALID_STATE: not
// started or already ended.
esp_err_t alloy_record_commit(alloy_record_t *r);

// One-field shortcut: field "value", like the Arduino log(channel, value).
esp_err_t alloy_storage_log(alloy_storage_handle_t h, const char *channel, float value);
// Several fields in one row.
esp_err_t alloy_storage_log_fields(alloy_storage_handle_t h, const char *channel,
                                   const char *const names[], const float values[], size_t count);

// ---- end of run ----

// Graceful end: seals open buffers, waits up to timeout_ms for metadata and every buffer to be
// delivered, then asks the service to finalize the MCAP now instead of after the inactivity
// window. Optional: power loss is handled server-side.
//   ESP_OK                 every buffer delivered and finalization accepted
//   ESP_ERR_TIMEOUT        still delivering when the time ran out (delivery keeps retrying; call
//                          again to retry the finalization)
//   ESP_FAIL               something inside the run was lost or rejected (see last_error)
// Logging stops at the first call.
esp_err_t alloy_storage_end(alloy_storage_handle_t h, uint32_t timeout_ms);

// ---- status ----

typedef enum {
    ALLOY_CLOCK_NONE = 0,   // not set yet
    ALLOY_CLOCK_SYSTEM,     // already set when begin() ran (app-managed)
    ALLOY_CLOCK_SNTP,       // set by SNTP
    ALLOY_CLOCK_HTTP_DATE,  // SNTP missed its 5 s grace: set from the ingest Date header
} alloy_clock_source_t;

typedef struct {
    uint32_t delivered;     // data chunks accepted
    uint32_t failed;        // chunks (or metadata) rejected terminally
    uint32_t retried;       // transient upload attempts retried
    uint32_t dropped;       // sealed chunks shed under backpressure
    uint32_t dropped_rows;  // rows rejected before buffering
    uint32_t stale;         // chunks refused because the run was already finalized (409)
    uint32_t queued;        // sealed or in-flight chunks right now
    bool ready;             // UTC clock set and run identity pinned
    uint32_t session;       // run id (epoch seconds at clock sync), 0 until ready
    alloy_clock_source_t clock;
    int last_status;        // HTTP status, or a negative ALLOY_STATUS_* local code
    esp_err_t last_transport_err;  // esp_http_client error behind ALLOY_STATUS_TRANSPORT
} alloy_storage_stats_t;

esp_err_t alloy_storage_get_stats(alloy_storage_handle_t h, alloy_storage_stats_t *out);

// Printable explanation of the last status; "" after success. The string lives in the handle
// and is rewritten by the next call.
const char *alloy_storage_last_error(alloy_storage_handle_t h);

// Wall-clock ns used to stamp rows (boot-relative until the clock is set; rebased on upload).
uint64_t alloy_storage_now_ns(void);

#ifdef __cplusplus
}
#endif
