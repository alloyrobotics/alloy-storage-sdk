// alloy_storage.c: the ESP-IDF side of the Alloy Storage SDK. The portable core (portable/) holds
// the policy; this file owns the clock, the upload task and the one HTTP connection.
//
// Wire protocol (spec/esp32-http-v1.md): POST {ingest}/v1/meta once, then POST /v1/chunk per
// sealed buffer, POST /v1/end on a graceful end. Headers on every POST: Authorization: Bearer,
// X-Alloy-Device, X-Alloy-Session, X-Alloy-Mesh-Path (+ X-Alloy-Finalize-Ms); /v1/chunk adds
// X-Alloy-Channel and X-Alloy-Seq. 2xx = accepted, 409 = run already finalized (terminal),
// 408/425/429/5xx and transport errors retry with backoff. GET /v1/health is the clock fallback.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include "esp_crt_bundle.h"
#endif

#include "alloy/meta.h"
#include "alloy/policy.h"
#include "alloy/pool.h"
#include "alloy_storage.h"

static const char *TAG = "alloy";

#define ALLOY_MAX_FIELDS 48
#define ALLOY_TASK_CORE (CONFIG_ALLOY_TASK_CORE_ID < 0 ? tskNO_AFFINITY : CONFIG_ALLOY_TASK_CORE_ID)

enum { END_IDLE, END_PENDING, END_OK, END_TIMEOUT, END_FAIL };

struct alloy_storage {
    // config, copied at begin()
    char *mesh, *ingest, *mission, *firmware, *auth;  // auth = "Bearer <api key>"
    char device[24];
    alloy_meta_field_t *fields;
    size_t nfields;
    uint32_t finalize_ms;

    // pool (guarded by mtx)
    SemaphoreHandle_t mtx;
    alloy_pool_t pool;
    alloy_buf_t *bufs;
    volatile bool ending;

    // upload task and its HTTP connection (task-owned)
    TaskHandle_t task;
    esp_http_client_handle_t http;
    bool connected;
    char date[32];  // last Date response header
    uint64_t boot_offset_ns;
    uint32_t seq;

    // status (written by the task, read anywhere)
    volatile uint32_t delivered, failed, retried, stale;
    volatile uint32_t session;
    volatile bool ready, meta_settled, meta_delivered;
    volatile int last_status, meta_status, delivery_failure;
    volatile esp_err_t last_err;
    volatile alloy_clock_source_t clock;

    // end() handshake (guarded by mtx)
    int end_state;
    bool end_posting, end_accepted;
    uint32_t end_deadline;

    char errbuf[160];
};

static struct alloy_storage *s_active;
static volatile bool s_sntp_synced;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool time_reached(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static bool clock_sane(void) { return time(NULL) >= (time_t)ALLOY_EPOCH_SANE; }

uint64_t alloy_storage_now_ns(void)
{
    // Before the clock is set, stamp boot-relative ns (< 1e18); the uploader rebases those rows
    // to wall clock once the offset is known.
    if (!clock_sane()) return (uint64_t)esp_timer_get_time() * 1000ull;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

// ------------------------------------------------------------------ network

static bool netif_has_ip(esp_netif_t *netif, void *ctx)
{
    (void)ctx;
    esp_netif_ip_info_t ip;
    return esp_netif_is_netif_up(netif) && esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
           ip.ip.addr != 0;
}

static bool online(void) { return esp_netif_find_if(netif_has_ip, NULL) != NULL; }

// Waits up to wait_ms for any interface with an IPv4 address.
static bool wait_online(uint32_t wait_ms)
{
    uint32_t t0 = now_ms();
    while (!online()) {
        if ((uint32_t)(now_ms() - t0) >= wait_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return true;
}

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    struct alloy_storage *a = e->user_data;
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
        strcasecmp(e->header_key, "Date") == 0)
        alloy_copy_str(a->date, sizeof(a->date), e->header_value);
    return ESP_OK;
}

// One request on the persistent connection. Returns the HTTP status, or ALLOY_STATUS_TRANSPORT
// with last_err set. A request that fails on a kept-alive connection is retried once on a fresh
// one: the server may have closed it while idle. Retrying a chunk is safe (deduped by seq).
static int http_request(struct alloy_storage *a, esp_http_client_method_t method,
                        const char *route, const char *content_type, const char *body, size_t len,
                        const char *channel, uint32_t seq)
{
    esp_http_client_handle_t h = a->http;
    char url[192];
    snprintf(url, sizeof(url), "%s%s", a->ingest, route);
    esp_http_client_set_url(h, url);
    esp_http_client_set_method(h, method);
    if (content_type) esp_http_client_set_header(h, "Content-Type", content_type);
    else esp_http_client_delete_header(h, "Content-Type");
    if (channel) {
        char s[12];
        snprintf(s, sizeof(s), "%lu", (unsigned long)seq);
        esp_http_client_set_header(h, "X-Alloy-Channel", channel);
        esp_http_client_set_header(h, "X-Alloy-Seq", s);
    } else {
        esp_http_client_delete_header(h, "X-Alloy-Channel");
        esp_http_client_delete_header(h, "X-Alloy-Seq");
    }
    esp_http_client_set_post_field(h, body, (int)len);

    for (int pass = 0; pass < 2; pass++) {
        bool reused = a->connected;
        esp_err_t err = esp_http_client_perform(h);
        if (err == ESP_OK) {
            a->connected = true;
            a->last_err = ESP_OK;
            return esp_http_client_get_status_code(h);
        }
        a->last_err = err;
        a->connected = false;
        esp_http_client_close(h);
        if (!reused) break;
    }
    return ALLOY_STATUS_TRANSPORT;
}

static bool ok_status(int s) { return s >= 200 && s < 300; }

// ------------------------------------------------------------------ clock

static void on_sntp_sync(struct timeval *tv)
{
    (void)tv;
    s_sntp_synced = true;
}

// Sets the clock from the Date header of the ingest service. ESP-IDF builds mbedTLS without
// certificate date checks by default (CONFIG_MBEDTLS_HAVE_TIME_DATE=n), so this verified request
// works before the clock is set. Date is truncated to the second, so this clock runs slightly
// behind; a later SNTP sync steps it forward. The run's boot offset is captured once either way.
static bool clock_from_server(struct alloy_storage *a)
{
    a->date[0] = 0;
    int status = http_request(a, HTTP_METHOD_GET, "/v1/health", NULL, NULL, 0, NULL, 0);
    uint32_t epoch = 0;
    if (status <= 0 || !alloy_parse_http_date(a->date, &epoch) || epoch < ALLOY_EPOCH_SANE)
        return false;
    if (clock_sane()) return true;  // SNTP landed during the request
    struct timeval tv = {.tv_sec = (time_t)epoch, .tv_usec = 0};
    if (settimeofday(&tv, NULL) != 0) return false;
    a->clock = ALLOY_CLOCK_HTTP_DATE;
    return true;
}

// Prefer SNTP; after a 5 s grace take the server's clock rather than hold every buffer.
static void wait_for_clock(struct alloy_storage *a)
{
    uint32_t t0 = now_ms(), next_try = ALLOY_SNTP_GRACE_MS;
    uint8_t attempt = 0;
    while (!clock_sane()) {
        if ((uint32_t)(now_ms() - t0) >= next_try) {
            if (wait_online(5000) && clock_from_server(a)) break;
            next_try = (uint32_t)(now_ms() - t0) + alloy_retry_delay_ms(attempt);
            if (attempt < 6) attempt++;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (a->clock == ALLOY_CLOCK_NONE) a->clock = s_sntp_synced ? ALLOY_CLOCK_SNTP : ALLOY_CLOCK_SYSTEM;
}

// ------------------------------------------------------------------ upload task

// Uploads one body with the retry policy. Returns its final status.
typedef enum { SEND_OK, SEND_STALE, SEND_TERMINAL } send_result_t;

static send_result_t send_with_retry(struct alloy_storage *a, const char *route, const char *ctype,
                                     const char *body, size_t len, const char *channel, uint32_t seq)
{
    uint8_t attempt = 0;
    for (;;) {
        int status = wait_online(5000)
                         ? http_request(a, HTTP_METHOD_POST, route, ctype, body, len, channel, seq)
                         : ALLOY_STATUS_OFFLINE;
        a->last_status = status;
        if (ok_status(status)) return SEND_OK;
        if (status == 409) return SEND_STALE;
        if (!alloy_retryable(status)) return SEND_TERMINAL;
        a->retried++;
        uint32_t delay = alloy_retry_delay_ms(attempt);
        if (status == ALLOY_STATUS_TRANSPORT)
            ESP_LOGW(TAG, "%s: %s, retry in %lu ms", route, esp_err_to_name(a->last_err),
                     (unsigned long)delay);
        else
            ESP_LOGW(TAG, "%s: status %d, retry in %lu ms", route, status, (unsigned long)delay);
        vTaskDelay(pdMS_TO_TICKS(delay));
        if (attempt < 6) attempt++;
    }
}

static void pin_session(struct alloy_storage *a)
{
    a->session = (uint32_t)time(NULL);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    uint64_t wall_ns = (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
    a->boot_offset_ns = wall_ns - (uint64_t)esp_timer_get_time() * 1000ull;

    char s[12];
    esp_http_client_set_header(a->http, "Authorization", a->auth);
    esp_http_client_set_header(a->http, "X-Alloy-Device", a->device);
    snprintf(s, sizeof(s), "%lu", (unsigned long)a->session);
    esp_http_client_set_header(a->http, "X-Alloy-Session", s);
    esp_http_client_set_header(a->http, "X-Alloy-Mesh-Path", a->mesh);
    if (a->finalize_ms) {
        snprintf(s, sizeof(s), "%lu", (unsigned long)a->finalize_ms);
        esp_http_client_set_header(a->http, "X-Alloy-Finalize-Ms", s);
    }
    a->ready = true;
}

// The metadata sidecar goes first and is held and retried like a data chunk: a transient failure
// must not turn a healthy run into one without semantics.
static void send_meta(struct alloy_storage *a)
{
    alloy_meta_t m = {a->device, a->firmware, a->mission, a->session, a->fields, a->nfields};
    size_t cap = 256 + a->nfields * 200;
    char *json = malloc(cap);
    size_t len = json ? alloy_meta_json(&m, json, cap) : 0;
    bool delivered = false;
    if (!len) {
        a->failed++;
        a->last_status = ALLOY_STATUS_MEMORY;
    } else {
        send_result_t r = send_with_retry(a, "/v1/meta", "application/json", json, len, NULL, 0);
        delivered = r == SEND_OK;
        if (r == SEND_STALE) a->stale++;
        else if (r == SEND_TERMINAL) a->failed++;
    }
    free(json);
    a->meta_status = a->last_status;
    a->meta_delivered = delivered;
    a->meta_settled = true;
}

static void deliver(struct alloy_storage *a, int idx)
{
    alloy_buf_t *b = &a->bufs[idx];
    alloy_rebase_rows(b->data, b->len, a->boot_offset_ns);
    uint32_t seq = a->seq++;  // once per buffer: retries reuse it (the service's dedupe key)
    send_result_t r = send_with_retry(a, "/v1/chunk", "text/csv", b->data, b->len, b->chan, seq);
    if (r == SEND_OK) {
        a->delivered++;
    } else if (r == SEND_STALE) {
        a->stale++;
        a->delivery_failure = 409;
    } else {
        a->failed++;
        a->delivery_failure = a->last_status ? a->last_status : ALLOY_STATUS_DATA_LOSS;
    }
    ESP_LOGD(TAG, "chunk %lu %s (%u bytes), stack free %u", (unsigned long)seq, b->chan,
             (unsigned)b->len, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreTake(a->mtx, portMAX_DELAY);
    alloy_pool_release(&a->pool, idx);
    xSemaphoreGive(a->mtx);
}

static int failure_status(struct alloy_storage *a)
{
    if (a->delivery_failure) return a->delivery_failure;
    return a->pool.dropped_bufs ? ALLOY_STATUS_DATA_LOSS : 0;
}

// Runs when end() is waiting and the uploader is idle: check the gate, then POST /v1/end with the
// retry policy until the caller's deadline.
static void run_end(struct alloy_storage *a)
{
    xSemaphoreTake(a->mtx, portMAX_DELAY);
    if (a->end_state != END_PENDING) {
        xSemaphoreGive(a->mtx);
        return;
    }
    alloy_end_gate_t gate = alloy_end_gate(a->meta_settled, a->meta_delivered,
                                           alloy_pool_free(&a->pool), a->pool.n, failure_status(a));
    if (gate == ALLOY_END_WAITING) {
        xSemaphoreGive(a->mtx);
        return;
    }
    if (gate == ALLOY_END_FAILED) {
        int fs = failure_status(a);
        a->last_status = !a->meta_delivered ? (a->meta_status ? a->meta_status : ALLOY_STATUS_DATA_LOSS)
                                            : (fs ? fs : ALLOY_STATUS_DATA_LOSS);
        a->end_state = END_FAIL;
        xSemaphoreGive(a->mtx);
        return;
    }
    a->end_posting = true;
    uint32_t deadline = a->end_deadline;
    xSemaphoreGive(a->mtx);

    int result = END_TIMEOUT;
    uint8_t attempt = 0;
    do {
        uint32_t now = now_ms();
        uint32_t remain = time_reached(now, deadline) ? 0 : deadline - now;
        int status = wait_online(remain > 1000 ? 1000 : remain)
                         ? http_request(a, HTTP_METHOD_POST, "/v1/end", "application/octet-stream",
                                        NULL, 0, NULL, 0)
                         : ALLOY_STATUS_OFFLINE;
        a->last_status = status;
        if (ok_status(status)) {  // 202/204: accepted; MCAP assembly may finish asynchronously
            result = END_OK;
            break;
        }
        if (!alloy_retryable(status)) {
            result = END_FAIL;
            break;
        }
        a->retried++;
        uint32_t delay = alloy_retry_delay_ms(attempt);
        if (attempt < 6) attempt++;
        now = now_ms();
        if (time_reached(now, deadline)) break;
        if (delay > deadline - now) delay = deadline - now;
        vTaskDelay(pdMS_TO_TICKS(delay));
    } while (!time_reached(now_ms(), deadline));

    xSemaphoreTake(a->mtx, portMAX_DELAY);
    a->end_posting = false;
    if (result == END_OK) a->end_accepted = true;
    a->end_state = result;
    xSemaphoreGive(a->mtx);
}

static void upload_task(void *arg)
{
    struct alloy_storage *a = arg;
    wait_for_clock(a);
    pin_session(a);
    ESP_LOGI(TAG, "run %lu: device %s, mesh %s, clock from %s", (unsigned long)a->session,
             a->device, a->mesh,
             a->clock == ALLOY_CLOCK_HTTP_DATE ? "HTTP Date" : a->clock == ALLOY_CLOCK_SNTP ? "SNTP" : "system");
    send_meta(a);

    for (;;) {
        xSemaphoreTake(a->mtx, portMAX_DELAY);
        int idx = alloy_pool_take(&a->pool);
        bool end_waiting = a->end_state == END_PENDING;
        xSemaphoreGive(a->mtx);
        if (idx >= 0) {
            deliver(a, idx);
            continue;
        }
        if (end_waiting) run_end(a);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
        // Timed flush. Skipped while a sealed chunk is queued: see alloy/pool.h.
        xSemaphoreTake(a->mtx, portMAX_DELAY);
        alloy_pool_flush_due(&a->pool, now_ms());
        xSemaphoreGive(a->mtx);
    }
}

// ------------------------------------------------------------------ public API

static char *dup_or_null(const char *s) { return s && s[0] ? strdup(s) : NULL; }

static void destroy(struct alloy_storage *a)
{
    if (!a) return;
    if (a->http) esp_http_client_cleanup(a->http);
    if (a->bufs) {
        for (uint8_t i = 0; i < a->pool.n; i++) free(a->bufs[i].data);
        free(a->bufs);
    }
    if (a->mtx) vSemaphoreDelete(a->mtx);
    free(a->mesh);
    free(a->ingest);
    free(a->mission);
    free(a->firmware);
    free(a->auth);
    free(a->fields);
    free(a);
}

static void default_device_id(char *out, size_t cap)
{
    const esp_app_desc_t *app = esp_app_get_description();
    alloy_copy_str(out, cap, app ? app->project_name : NULL);
    if (!out[0]) {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        snprintf(out, cap, "esp32-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
}

esp_err_t alloy_storage_begin(const alloy_storage_config_t *cfg, alloy_storage_handle_t *out)
{
    if (!cfg || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (s_active) return ESP_ERR_INVALID_STATE;
    uint8_t nbuf = cfg->buffer_count ? cfg->buffer_count : CONFIG_ALLOY_POOL_SIZE;
    size_t bytes = cfg->buffer_bytes ? cfg->buffer_bytes : CONFIG_ALLOY_BUFFER_BYTES;
    uint32_t flush_ms = cfg->flush_interval_ms ? cfg->flush_interval_ms : CONFIG_ALLOY_FLUSH_INTERVAL_MS;
    const char *ingest = cfg->ingest_url ? cfg->ingest_url : CONFIG_ALLOY_INGEST_URL;
    if (!cfg->api_key || !cfg->api_key[0]) {
        ESP_LOGE(TAG, "begin: api_key is required");
        return ESP_ERR_INVALID_ARG;
    }
    if (!alloy_valid_mesh_path(cfg->mesh_path)) {
        ESP_LOGE(TAG, "begin: mesh_path must be 1-128 of [A-Za-z0-9_/-] with no edge '/'");
        return ESP_ERR_INVALID_ARG;
    }
    if (nbuf < 2 || nbuf > ALLOY_POOL_MAX || bytes < 512 || bytes > 64 * 1024 ||
        cfg->field_count > ALLOY_MAX_FIELDS || (cfg->field_count && !cfg->fields) ||
        (strncmp(ingest, "https://", 8) != 0 && strncmp(ingest, "http://", 7) != 0) ||
        strlen(ingest) > 160) {
        ESP_LOGE(TAG, "begin: buffer_count 2-%d, buffer_bytes 512-65536, at most %d fields, "
                 "ingest_url http(s)://", ALLOY_POOL_MAX, ALLOY_MAX_FIELDS);
        return ESP_ERR_INVALID_ARG;
    }

    esp_http_client_config_t http = {
        .url = ingest,
        .timeout_ms = CONFIG_ALLOY_HTTP_TIMEOUT_MS,
        .event_handler = on_http_event,
        .buffer_size_tx = 1024,
    };
    if (cfg->insecure) {
#if !(CONFIG_ESP_TLS_INSECURE && CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY)
        ESP_LOGE(TAG, "begin: insecure needs CONFIG_ESP_TLS_INSECURE and "
                 "CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY");
        return ESP_ERR_NOT_SUPPORTED;
#endif
        ESP_LOGW(TAG, "TLS server verification is OFF (insecure)");
    } else {
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
        http.crt_bundle_attach = esp_crt_bundle_attach;
#else
        if (strncmp(ingest, "https://", 8) == 0) {
            ESP_LOGE(TAG, "begin: enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE to verify the server");
            return ESP_ERR_NOT_SUPPORTED;
        }
#endif
    }

    struct alloy_storage *a = calloc(1, sizeof(*a));
    if (!a) return ESP_ERR_NO_MEM;
    http.user_data = a;
    const esp_app_desc_t *app = esp_app_get_description();
    a->mesh = strdup(cfg->mesh_path);
    a->ingest = strdup(ingest);
    size_t il = strlen(a->ingest ? a->ingest : "");
    while (il > 0 && a->ingest[il - 1] == '/') a->ingest[--il] = 0;
    a->mission = dup_or_null(cfg->mission);
    a->firmware = dup_or_null(cfg->firmware ? cfg->firmware : app ? app->version : NULL);
    size_t al = strlen(cfg->api_key) + 8;
    a->auth = malloc(al);
    if (a->auth) snprintf(a->auth, al, "Bearer %s", cfg->api_key);
    if (cfg->device_id && cfg->device_id[0]) alloy_copy_str(a->device, sizeof(a->device), cfg->device_id);
    else default_device_id(a->device, sizeof(a->device));
    alloy_sanitize(a->device);
    a->finalize_ms = alloy_finalize_ms(cfg->finalize_after_s);
    a->nfields = cfg->field_count;
    if (a->nfields) a->fields = calloc(a->nfields, sizeof(alloy_meta_field_t));
    for (size_t i = 0; a->fields && i < a->nfields; i++) {
        const alloy_field_desc_t *d = &cfg->fields[i];
        alloy_meta_field_t *f = &a->fields[i];
        alloy_copy_str(f->channel, sizeof(f->channel), d->channel);
        alloy_sanitize(f->channel);  // the MCAP channel is the sanitized name
        alloy_copy_str(f->name, sizeof(f->name), d->name);
        alloy_copy_str(f->unit, sizeof(f->unit), d->unit);
        alloy_copy_str(f->about, sizeof(f->about), d->about);
        f->min = d->has_range ? d->min : NAN;
        f->max = d->has_range ? d->max : NAN;
    }
    a->bufs = calloc(nbuf, sizeof(alloy_buf_t));
    a->mtx = xSemaphoreCreateMutex();
    bool ok = a->mesh && a->ingest && a->auth && a->bufs && a->mtx &&
              (!a->nfields || a->fields) && (!cfg->mission || !cfg->mission[0] || a->mission);
    for (uint8_t i = 0; ok && i < nbuf; i++) {
        a->bufs[i].data = malloc(bytes);
        a->pool.n = (uint8_t)(i + 1);  // so destroy() frees what was allocated
        ok = a->bufs[i].data != NULL;
    }
    if (ok) ok = alloy_pool_init(&a->pool, a->bufs, nbuf, bytes, flush_ms);
    if (ok) {
        a->http = esp_http_client_init(&http);
        ok = a->http != NULL;
    }
    if (!ok) {
        ESP_LOGE(TAG, "begin: out of memory (%u x %u byte buffers)", (unsigned)nbuf, (unsigned)bytes);
        destroy(a);
        return ESP_ERR_NO_MEM;
    }

    // UTC clock: rows, the run id and the MCAP all need it.
    esp_netif_init();  // idempotent; SNTP needs the TCP/IP stack
    const char *sntp = cfg->sntp_server ? cfg->sntp_server : CONFIG_ALLOY_SNTP_SERVER;
    if (clock_sane()) {
        a->clock = ALLOY_CLOCK_SYSTEM;
    } else if (sntp[0] && !esp_sntp_enabled()) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(sntp);
        sc.sync_cb = on_sntp_sync;
        esp_err_t err = esp_netif_sntp_init(&sc);
        if (err != ESP_OK) ESP_LOGW(TAG, "SNTP not started (%s); using the HTTP Date fallback",
                                    esp_err_to_name(err));
    }

    s_active = a;
    if (xTaskCreatePinnedToCore(upload_task, "alloy_up", CONFIG_ALLOY_TASK_STACK_SIZE, a,
                                CONFIG_ALLOY_TASK_PRIORITY, &a->task, ALLOY_TASK_CORE) != pdPASS) {
        s_active = NULL;
        destroy(a);
        ESP_LOGE(TAG, "begin: could not start the upload task");
        return ESP_ERR_NO_MEM;
    }
    *out = a;
    return ESP_OK;
}

void alloy_record_begin(alloy_record_t *r, alloy_storage_handle_t h, const char *channel)
{
    r->owner = h;
    alloy_csv_row_init(&r->row, channel, alloy_storage_now_ns());
}

bool alloy_record_set(alloy_record_t *r, const char *name, float value)
{
    return alloy_csv_row_set(&r->row, name, value);
}

esp_err_t alloy_record_commit(alloy_record_t *r)
{
    struct alloy_storage *a = r->owner;
    r->owner = NULL;  // a record commits once
    if (!a || a->ending) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(a->mtx, portMAX_DELAY);
    alloy_commit_t c = alloy_pool_commit(&a->pool, &r->row, now_ms());
    xSemaphoreGive(a->mtx);
    if (c & ALLOY_COMMIT_SEALED) xTaskNotifyGive(a->task);
    if (c & ALLOY_COMMIT_CLOSED) return ESP_ERR_INVALID_STATE;
    return (c & ALLOY_COMMIT_DROPPED) ? ESP_ERR_NO_MEM : ESP_OK;
}

esp_err_t alloy_storage_log(alloy_storage_handle_t h, const char *channel, float value)
{
    alloy_record_t r;
    alloy_record_begin(&r, h, channel);
    alloy_record_set(&r, "value", value);
    return alloy_record_commit(&r);
}

esp_err_t alloy_storage_log_fields(alloy_storage_handle_t h, const char *channel,
                                   const char *const names[], const float values[], size_t count)
{
    if (count && (!names || !values)) return ESP_ERR_INVALID_ARG;
    alloy_record_t r;
    alloy_record_begin(&r, h, channel);
    for (size_t i = 0; i < count; i++) alloy_record_set(&r, names[i], values[i]);
    return alloy_record_commit(&r);
}

esp_err_t alloy_storage_end(alloy_storage_handle_t a, uint32_t timeout_ms)
{
    if (!a) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(a->mtx, portMAX_DELAY);
    if (a->end_accepted) {
        xSemaphoreGive(a->mtx);
        return ESP_OK;
    }
    a->ending = true;  // producers stop before their active chunks are sealed
    alloy_pool_close(&a->pool);
    a->end_deadline = now_ms() + timeout_ms;
    a->end_state = END_PENDING;
    xSemaphoreGive(a->mtx);
    xTaskNotifyGive(a->task);

    for (;;) {
        xSemaphoreTake(a->mtx, portMAX_DELAY);
        int st = a->end_state;
        if (st == END_PENDING && !a->end_posting && time_reached(now_ms(), a->end_deadline)) {
            // Never finalize ahead of metadata or data still being retried.
            a->end_state = END_IDLE;
            a->last_status = ALLOY_STATUS_DRAIN_TIMEOUT;
            xSemaphoreGive(a->mtx);
            return ESP_ERR_TIMEOUT;
        }
        if (st != END_PENDING) a->end_state = END_IDLE;
        xSemaphoreGive(a->mtx);
        if (st == END_OK) return ESP_OK;
        if (st == END_TIMEOUT) return ESP_ERR_TIMEOUT;
        if (st == END_FAIL) return ESP_FAIL;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

esp_err_t alloy_storage_get_stats(alloy_storage_handle_t a, alloy_storage_stats_t *s)
{
    if (!a || !s) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(a->mtx, portMAX_DELAY);
    s->dropped = a->pool.dropped_bufs;
    s->dropped_rows = a->pool.dropped_rows;
    s->queued = alloy_pool_queued(&a->pool);
    xSemaphoreGive(a->mtx);
    s->delivered = a->delivered;
    s->failed = a->failed;
    s->retried = a->retried;
    s->stale = a->stale;
    s->ready = a->ready;
    s->session = a->session;
    s->clock = a->clock;
    s->last_status = a->last_status;
    s->last_transport_err = a->last_err;
    return ESP_OK;
}

const char *alloy_storage_last_error(alloy_storage_handle_t a)
{
    if (!a) return "not started";
    int status = a->last_status;
    alloy_status_text(status, a->errbuf, sizeof(a->errbuf));
    esp_err_t err = a->last_err;
    if (status == ALLOY_STATUS_TRANSPORT && err != ESP_OK) {
        size_t n = strlen(a->errbuf);
        snprintf(a->errbuf + n, sizeof(a->errbuf) - n, " (%s)", esp_err_to_name(err));
    }
    return a->errbuf;
}
