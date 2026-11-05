// Host tests for the portable core (portable/). Plain C11, run under ASan/UBSan: make -C test/host

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alloy/meta.h"
#include "alloy/policy.h"
#include "alloy/pool.h"
#include "alloy/record.h"

static int failures, checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        checks++;                                                                \
        if (!(cond)) {                                                           \
            failures++;                                                          \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECK_STR(a, b)                                                                \
    do {                                                                               \
        checks++;                                                                      \
        if (strcmp((a), (b)) != 0) {                                                   \
            failures++;                                                                \
            fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------- record

static void test_record_encoding(void)
{
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "imu", 1782715694000000000ull);
    CHECK(alloy_csv_row_set(&r, "pitch", -1.25f));
    CHECK(alloy_csv_row_set(&r, "roll", 3.14159265f));
    CHECK(alloy_csv_row_set(&r, "gone", NAN));
    CHECK(alloy_csv_row_set(&r, "inf", INFINITY));
    CHECK(alloy_csv_row_set(&r, "big", 123456789.0f));
    CHECK_STR(r.hdr, "t_ns,pitch,roll,gone,inf,big");
    CHECK_STR(r.row, "1782715694000000000,-1.25,3.14159,,,1.23457e+08");
    CHECK(r.hlen == (int)strlen(r.hdr) && r.rlen == (int)strlen(r.row));
    CHECK(r.sig == alloy_fnv1a(r.hdr, strlen(r.hdr), ALLOY_FNV_SEED));
    CHECK(!r.done);
}

static void test_record_boot_stamp_padded(void)
{
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "a", 1234ull);
    CHECK_STR(r.row, "0000000000000001234");
}

static void test_record_rejects_bad_names_and_channel(void)
{
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "a", 1);
    CHECK(!alloy_csv_row_set(&r, "", 1));
    CHECK(!alloy_csv_row_set(&r, NULL, 1));
    CHECK(!alloy_csv_row_set(&r, "x,y", 1));
    CHECK(!alloy_csv_row_set(&r, "x\ny", 1));
    CHECK_STR(r.hdr, "t_ns");
    alloy_csv_row_init(&r, "", 1);
    CHECK(r.done);
    alloy_csv_row_init(&r, NULL, 1);
    CHECK(r.done);
    CHECK(!alloy_csv_row_set(&r, "x", 1));
}

static void test_record_channel_truncated(void)
{
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "abcdefghijklmnopqrstuvwxyz0123456789", 1);
    CHECK(strlen(r.chan) == ALLOY_CHAN_MAX - 1);
}

static void test_record_overflow_rolls_back(void)
{
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "wide", 1);
    int added = 0;
    char name[16];
    for (int i = 0; i < 200; i++) {
        snprintf(name, sizeof(name), "f%d", i);
        if (alloy_csv_row_set(&r, name, -1.23456e-30f)) added++;
    }
    CHECK(added > 0 && added < 200);
    CHECK(r.rlen < ALLOY_ROW_MAX && r.hlen < ALLOY_HDR_MAX);
    // header and row still have the same number of cells
    int hc = 0, rc = 0;
    for (const char *p = r.hdr; *p; p++) hc += *p == ',';
    for (const char *p = r.row; *p; p++) rc += *p == ',';
    CHECK(hc == added && rc == added);
    CHECK(r.sig == alloy_fnv1a(r.hdr, strlen(r.hdr), ALLOY_FNV_SEED));
}

static void test_sanitize(void)
{
    char s[] = "my chan/1.x-y_Z";
    alloy_sanitize(s);
    CHECK_STR(s, "my_chan_1_x-y_Z");
}

static void test_rebase(void)
{
    char buf[256];
    snprintf(buf, sizeof(buf),
             "t_ns,a\n0000000000005000000,1\n1782715694000000000,2\n0000000000006000000");
    alloy_rebase_rows(buf, strlen(buf), 1700000000000000000ull);
    CHECK_STR(buf, "t_ns,a\n1700000000005000000,1\n1782715694000000000,2\n1700000000006000000");
    char same[] = "t_ns,a\n0000000000005000000,1\n";
    alloy_rebase_rows(same, strlen(same), 0);
    CHECK_STR(same, "t_ns,a\n0000000000005000000,1\n");
    char odd[] = "00000000000000000x1,1\n123\n";  // not a stamp: untouched
    alloy_rebase_rows(odd, strlen(odd), 5);
    CHECK_STR(odd, "00000000000000000x1,1\n123\n");
}

// ---------------------------------------------------------------- pool

#define TEST_BUF_BYTES 256

typedef struct {
    alloy_pool_t pool;
    alloy_buf_t bufs[ALLOY_POOL_MAX];
    char storage[ALLOY_POOL_MAX][TEST_BUF_BYTES];
} fixture_t;

static fixture_t *fixture(uint8_t n, size_t bytes, uint32_t flush_ms)
{
    fixture_t *f = calloc(1, sizeof(*f));
    for (int i = 0; i < ALLOY_POOL_MAX; i++) f->bufs[i].data = f->storage[i];
    CHECK(alloy_pool_init(&f->pool, f->bufs, n, bytes, flush_ms));
    return f;
}

static alloy_commit_t log_seq(alloy_pool_t *p, const char *chan, uint32_t seq, uint32_t now)
{
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, chan, 1782715694000000000ull + seq);
    alloy_csv_row_set(&r, "seq", (float)seq);
    return alloy_pool_commit(p, &r, now);
}

// Counts each "<t_ns>,<seq>" row of a chunk into seen[seq].
static void tally(const alloy_buf_t *b, unsigned *seen, unsigned max)
{
    const char *end = b->data + b->len;
    const char *p = (const char *)memchr(b->data, '\n', b->len) + 1;  // skip the header
    while (p < end) {
        unsigned v = (unsigned)atoi(p + 20);
        if (v < max) seen[v]++;
        p = (const char *)memchr(p, '\n', (size_t)(end - p)) + 1;
    }
}

static unsigned count_rows(const alloy_buf_t *b)
{
    unsigned n = 0;
    for (size_t i = 0; i < b->len; i++) n += b->data[i] == '\n';
    return n - 1;  // header
}

static void test_pool_init_bounds(void)
{
    alloy_pool_t p;
    alloy_buf_t b[1];
    char d[16];
    b[0].data = d;
    CHECK(!alloy_pool_init(&p, b, 0, 16, 1));
    CHECK(!alloy_pool_init(&p, b, ALLOY_POOL_MAX + 1, 16, 1));
    CHECK(alloy_pool_init(&p, b, 1, 16, 1));
    CHECK(alloy_pool_free(&p) == 1);
}

static void test_pool_header_once_per_chunk(void)
{
    fixture_t *f = fixture(4, TEST_BUF_BYTES, 100000);
    CHECK(log_seq(&f->pool, "s", 1, 0) == ALLOY_COMMIT_OK);
    CHECK(log_seq(&f->pool, "s", 2, 0) == ALLOY_COMMIT_OK);
    alloy_pool_close(&f->pool);
    int b = alloy_pool_take(&f->pool);
    CHECK(b >= 0);
    char text[TEST_BUF_BYTES + 1];
    memcpy(text, f->bufs[b].data, f->bufs[b].len);
    text[f->bufs[b].len] = 0;
    CHECK_STR(text, "t_ns,seq\n1782715694000000001,1\n1782715694000000002,2\n");
    CHECK_STR(f->bufs[b].chan, "s");
    free(f);
}

static void test_pool_seals_at_90_percent(void)
{
    fixture_t *f = fixture(4, TEST_BUF_BYTES, 100000);
    unsigned sealed_at = 0;
    for (uint32_t i = 0; i < 20 && !sealed_at; i++)
        if (log_seq(&f->pool, "s", i, 0) == ALLOY_COMMIT_SEALED) sealed_at = i + 1;
    CHECK(sealed_at > 0);
    CHECK(alloy_pool_pending(&f->pool) == 1);
    int b = alloy_pool_take(&f->pool);
    CHECK(f->bufs[b].len >= TEST_BUF_BYTES * 9 / 10 && f->bufs[b].len <= TEST_BUF_BYTES);
    CHECK(count_rows(&f->bufs[b]) == sealed_at);
    free(f);
}

static void test_pool_field_change_reseals(void)
{
    fixture_t *f = fixture(4, TEST_BUF_BYTES, 100000);
    log_seq(&f->pool, "s", 1, 0);
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "s", 5);
    alloy_csv_row_set(&r, "other", 1);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == ALLOY_COMMIT_SEALED);
    CHECK(alloy_pool_pending(&f->pool) == 1);
    alloy_pool_close(&f->pool);
    CHECK(alloy_pool_pending(&f->pool) == 2);
    int a = alloy_pool_take(&f->pool), b = alloy_pool_take(&f->pool);
    CHECK(strncmp(f->bufs[a].data, "t_ns,seq\n", 9) == 0);
    CHECK(strncmp(f->bufs[b].data, "t_ns,other\n", 11) == 0);
    free(f);
}

static void test_pool_timed_flush(void)
{
    fixture_t *f = fixture(4, TEST_BUF_BYTES, 1000);
    CHECK(log_seq(&f->pool, "s", 1, 0) == ALLOY_COMMIT_OK);
    CHECK(log_seq(&f->pool, "s", 2, 999) == ALLOY_COMMIT_OK);
    CHECK(log_seq(&f->pool, "s", 3, 1000) == ALLOY_COMMIT_SEALED);  // on commit
    CHECK(alloy_pool_pending(&f->pool) == 1);
    CHECK(alloy_pool_take(&f->pool) >= 0);
    CHECK(log_seq(&f->pool, "s", 4, 1500) == ALLOY_COMMIT_OK);
    CHECK(!alloy_pool_flush_due(&f->pool, 1999));
    CHECK(alloy_pool_flush_due(&f->pool, 2000));  // on the uploader's idle tick
    CHECK(alloy_pool_pending(&f->pool) == 1);
    free(f);
}

// The outage fix: while a sealed chunk is still queued, the timer must not seal part-full chunks.
static void test_pool_no_timed_flush_while_pending(void)
{
    fixture_t *f = fixture(4, TEST_BUF_BYTES, 1000);
    log_seq(&f->pool, "s", 1, 0);
    CHECK(log_seq(&f->pool, "s", 2, 1000) == ALLOY_COMMIT_SEALED);
    CHECK(alloy_pool_pending(&f->pool) == 1);
    for (uint32_t t = 1001; t < 1004; t++) CHECK(log_seq(&f->pool, "s", t, t + 5000) == ALLOY_COMMIT_OK);
    CHECK(!alloy_pool_flush_due(&f->pool, 60000));
    CHECK(alloy_pool_pending(&f->pool) == 1);
    CHECK(alloy_pool_free(&f->pool) == 2);
    free(f);
}

static void test_pool_timed_flush_needs_free_buffer(void)
{
    fixture_t *f = fixture(2, TEST_BUF_BYTES, 1000);
    log_seq(&f->pool, "a", 1, 0);
    log_seq(&f->pool, "b", 1, 0);
    CHECK(alloy_pool_free(&f->pool) == 0);
    CHECK(!alloy_pool_flush_due(&f->pool, 5000));  // no replacement: defer, don't shed
    CHECK(alloy_pool_pending(&f->pool) == 0);
    CHECK(f->pool.dropped_bufs == 0);
    free(f);
}

static void test_pool_sheds_oldest_pending(void)
{
    fixture_t *f = fixture(3, TEST_BUF_BYTES, 100000);
    uint32_t seq = 0;
    // fill until every buffer is pending or active and a shed happens
    while (f->pool.dropped_bufs == 0 && seq < 1000) log_seq(&f->pool, "s", seq++, 0);
    CHECK(f->pool.dropped_bufs == 1);
    CHECK(alloy_pool_pending(&f->pool) + 1 == 3);  // shed one, reused it as the active chunk
    free(f);
}

// A row that doesn't fit seals the chunk; with nothing free the row is dropped rather than
// recycling the chunk it just sealed.
static void test_pool_never_sheds_just_sealed(void)
{
    fixture_t *f = fixture(1, 64, 100000);
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "s", 1);
    alloy_csv_row_set(&r, "v", 1);  // header 6+1, row 21+1: 29 then 51 bytes (90% of 64 = 57)
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == ALLOY_COMMIT_OK);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == ALLOY_COMMIT_OK);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == (ALLOY_COMMIT_SEALED | ALLOY_COMMIT_DROPPED));
    CHECK(alloy_pool_pending(&f->pool) == 1);
    CHECK(f->pool.dropped_bufs == 0 && f->pool.dropped_rows == 1);
    free(f);
}

// Same on a field-set change: the chunk sealed by the change is not shed for the new row.
static void test_pool_field_change_never_sheds_its_chunk(void)
{
    fixture_t *f = fixture(1, TEST_BUF_BYTES, 100000);
    log_seq(&f->pool, "s", 1, 0);
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "s", 5);
    alloy_csv_row_set(&r, "other", 1);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == (ALLOY_COMMIT_SEALED | ALLOY_COMMIT_DROPPED));
    CHECK(f->pool.dropped_bufs == 0 && alloy_pool_pending(&f->pool) == 1);
    free(f);
}

static void test_pool_drops_unfittable_rows_and_extra_channels(void)
{
    fixture_t *f = fixture(4, 32, 100000);
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "s", 1);
    alloy_csv_row_set(&r, "a_long_field_name", 1);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == ALLOY_COMMIT_DROPPED);
    alloy_csv_row_init(&r, "", 1);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == ALLOY_COMMIT_DROPPED);
    CHECK(f->pool.dropped_rows == 2);
    free(f);

    f = fixture(ALLOY_POOL_MAX, TEST_BUF_BYTES, 100000);
    char chan[16];
    for (unsigned i = 0; i < ALLOY_MAX_CHANNELS; i++) {
        snprintf(chan, sizeof(chan), "c%u", i % 1000u);
        CHECK(log_seq(&f->pool, chan, 1, 0) == ALLOY_COMMIT_OK);
    }
    CHECK(log_seq(&f->pool, "one_too_many", 1, 0) == ALLOY_COMMIT_DROPPED);
    free(f);
}

static void test_pool_sanitizes_buffer_channel(void)
{
    fixture_t *f = fixture(2, TEST_BUF_BYTES, 100000);
    log_seq(&f->pool, "my imu.0", 1, 0);
    alloy_pool_close(&f->pool);
    int b = alloy_pool_take(&f->pool);
    CHECK_STR(f->bufs[b].chan, "my_imu_0");
    free(f);
}

static void test_pool_close_take_release(void)
{
    fixture_t *f = fixture(4, TEST_BUF_BYTES, 100000);
    log_seq(&f->pool, "a", 1, 0);
    log_seq(&f->pool, "b", 1, 0);
    alloy_csv_row_t r;
    alloy_csv_row_init(&r, "c", 1);
    alloy_csv_row_set(&r, "v", 1);
    alloy_pool_commit(&f->pool, &r, 0);
    alloy_pool_close(&f->pool);
    CHECK(alloy_pool_commit(&f->pool, &r, 0) == ALLOY_COMMIT_CLOSED);
    CHECK(f->pool.dropped_rows == 0);
    CHECK(alloy_pool_pending(&f->pool) == 3 && alloy_pool_free(&f->pool) == 1);
    int x = alloy_pool_take(&f->pool);
    CHECK(alloy_pool_queued(&f->pool) == 3);
    alloy_pool_release(&f->pool, x);
    alloy_pool_release(&f->pool, x);  // double release ignored (nothing in flight)
    CHECK(alloy_pool_queued(&f->pool) == 2 && alloy_pool_free(&f->pool) == 2);
    while ((x = alloy_pool_take(&f->pool)) >= 0) alloy_pool_release(&f->pool, x);
    CHECK(alloy_pool_free(&f->pool) == 4 && alloy_pool_queued(&f->pool) == 0);
    free(f);
}

// Simulated outage with the real defaults' shape: a 20 Hz counter, 4 buffers, a 4 s flush timer,
// the uploader stuck on the first chunk for 25 s. Every value must be delivered exactly once.
static void test_pool_outage_keeps_every_row(void)
{
    enum { BYTES = 12 * 1024 };
    alloy_pool_t pool;
    alloy_buf_t bufs[4];
    static char storage[4][BYTES];
    for (int i = 0; i < 4; i++) bufs[i].data = storage[i];
    CHECK(alloy_pool_init(&pool, bufs, 4, BYTES, 4000));

    static unsigned seen[2000];
    memset(seen, 0, sizeof(seen));
    uint32_t seq = 0;
    int in_flight = -1;
    uint32_t outage_start = 0, outage_end = 0;
    for (uint32_t t = 0; t < 60000; t += 50) {
        alloy_csv_row_t r;
        alloy_csv_row_init(&r, "outage", 1782715694000000000ull + (uint64_t)t * 1000000ull);
        alloy_csv_row_set(&r, "seq", (float)seq++);
        alloy_pool_commit(&pool, &r, t);
        if (t % 250 == 0) {  // uploader idle tick
            alloy_pool_flush_due(&pool, t);
            if (in_flight < 0) in_flight = alloy_pool_take(&pool);
            if (in_flight >= 0 && !outage_start) {
                outage_start = t;
                outage_end = t + 25000;
            }
            if (in_flight >= 0 && t >= outage_end) {  // delivered
                tally(&bufs[in_flight], seen, 2000);
                alloy_pool_release(&pool, in_flight);
                in_flight = -1;
            }
        }
    }
    alloy_pool_close(&pool);
    for (int b; (b = alloy_pool_take(&pool)) >= 0;) {
        tally(&bufs[b], seen, 2000);
        alloy_pool_release(&pool, b);
    }
    CHECK(pool.dropped_bufs == 0 && pool.dropped_rows == 0);
    unsigned ok = 0;
    for (uint32_t i = 0; i < seq; i++) ok += seen[i] == 1;
    CHECK(ok == seq);
}

// ---------------------------------------------------------------- policy

static void test_retry_policy(void)
{
    CHECK(alloy_retryable(0) && alloy_retryable(-1) && alloy_retryable(ALLOY_STATUS_OFFLINE));
    CHECK(alloy_retryable(408) && alloy_retryable(425) && alloy_retryable(429));
    CHECK(alloy_retryable(500) && alloy_retryable(503));
    CHECK(!alloy_retryable(200) && !alloy_retryable(400) && !alloy_retryable(401));
    CHECK(!alloy_retryable(403) && !alloy_retryable(409) && !alloy_retryable(413));
    const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};
    for (uint8_t i = 0; i < 8; i++) CHECK(alloy_retry_delay_ms(i) == want[i]);
    CHECK(alloy_retry_delay_ms(255) == 30000);
}

static void test_http_date(void)
{
    uint32_t e = 0;
    CHECK(alloy_parse_http_date("Wed, 04 Nov 2026 13:06:33 GMT", &e) && e == 1793797593u);
    CHECK(alloy_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT", &e) && e == 0);
    CHECK(alloy_parse_http_date("Thu, 29 Feb 2024 12:00:00 GMT", &e) && e == 1709208000u);
    CHECK(alloy_parse_http_date("Sun, 07 Feb 2106 06:28:15 GMT", &e) == false);  // > 2105
    CHECK(!alloy_parse_http_date("Fri, 29 Feb 2025 12:00:00 GMT", &e));
    CHECK(!alloy_parse_http_date("Wed, 04 Nov 2026 24:00:00 GMT", &e));
    CHECK(!alloy_parse_http_date("Wed, 04 Foo 2026 13:06:33 GMT", &e));
    CHECK(!alloy_parse_http_date("Wednesday, 04-Nov-26 13:06:33 GMT", &e));  // RFC 850
    CHECK(!alloy_parse_http_date("Wed Nov  4 13:06:33 2026", &e));           // asctime
    CHECK(!alloy_parse_http_date("Wed, 04 Nov 2026 13:06:33 UTC", &e));
    CHECK(!alloy_parse_http_date("", &e) && !alloy_parse_http_date(NULL, &e));
    CHECK(!alloy_parse_http_date("Wed, 04 Nov 2026 13:06:33 GMT", NULL));
}

static void test_end_gate(void)
{
    CHECK(alloy_end_gate(false, false, 4, 4, 0) == ALLOY_END_WAITING);
    CHECK(alloy_end_gate(true, true, 3, 4, 0) == ALLOY_END_WAITING);
    CHECK(alloy_end_gate(true, true, 4, 4, 0) == ALLOY_END_READY);
    CHECK(alloy_end_gate(true, false, 4, 4, 0) == ALLOY_END_FAILED);
    CHECK(alloy_end_gate(true, true, 4, 4, ALLOY_STATUS_DATA_LOSS) == ALLOY_END_FAILED);
    CHECK(alloy_end_gate(true, true, 4, 4, 409) == ALLOY_END_FAILED);
}

static void test_mesh_path(void)
{
    CHECK(alloy_valid_mesh_path("robots/sbr"));
    CHECK(alloy_valid_mesh_path("hardware-verification/esp32c3-esp-idf_first"));
    CHECK(alloy_valid_mesh_path("a"));
    char max[129];
    memset(max, 'a', 128);
    max[128] = 0;
    CHECK(alloy_valid_mesh_path(max));
    char over[130];
    memset(over, 'a', 129);
    over[129] = 0;
    CHECK(!alloy_valid_mesh_path(over));
    CHECK(!alloy_valid_mesh_path(""));
    CHECK(!alloy_valid_mesh_path(NULL));
    CHECK(!alloy_valid_mesh_path("/robots"));
    CHECK(!alloy_valid_mesh_path("robots/"));
    CHECK(!alloy_valid_mesh_path("robots sbr"));
    CHECK(!alloy_valid_mesh_path("robots.sbr"));
    CHECK(!alloy_valid_mesh_path("caf\xc3\xa9"));
}

static void test_finalize_ms(void)
{
    CHECK(alloy_finalize_ms(0) == 0);
    CHECK(alloy_finalize_ms(1) == 30000);
    CHECK(alloy_finalize_ms(30) == 30000);
    CHECK(alloy_finalize_ms(120) == 120000);
    CHECK(alloy_finalize_ms(100000) == 1800000);
}

static void test_status_text(void)
{
    char s[128];
    alloy_status_text(204, s, sizeof(s));
    CHECK_STR(s, "");
    alloy_status_text(0, s, sizeof(s));
    CHECK_STR(s, "");
    alloy_status_text(401, s, sizeof(s));
    CHECK_STR(s, "Alloy API key rejected (HTTP 401)");
    alloy_status_text(503, s, sizeof(s));
    CHECK(strstr(s, "503") && strstr(s, "retrying"));
    alloy_status_text(ALLOY_STATUS_DATA_LOSS, s, sizeof(s));
    CHECK(strstr(s, "dropped") != NULL);
    alloy_status_text(-7, s, sizeof(s));
    CHECK_STR(s, "local error (-7)");
    alloy_status_text(401, s, 8);
    CHECK(strlen(s) == 7);
}

// ---------------------------------------------------------------- meta

static void test_meta_json(void)
{
    alloy_meta_field_t f[2];
    memset(f, 0, sizeof(f));
    alloy_copy_str(f[0].channel, sizeof(f[0].channel), "sample");
    alloy_copy_str(f[0].name, sizeof(f[0].name), "wave");
    alloy_copy_str(f[0].about, sizeof(f[0].about), "generated \"sine\"\n");
    f[0].min = -1;
    f[0].max = 1;
    alloy_copy_str(f[1].channel, sizeof(f[1].channel), "sample");
    alloy_copy_str(f[1].name, sizeof(f[1].name), "uptime_s");
    alloy_copy_str(f[1].unit, sizeof(f[1].unit), "s");
    f[1].min = NAN;
    f[1].max = 20.5f;
    alloy_meta_t m = {"esp32-first", "v1", "first \\ mission", 1793797593u, f, 2};
    char out[512];
    size_t n = alloy_meta_json(&m, out, sizeof(out));
    CHECK(n == strlen(out));
    CHECK_STR(out,
              "{\"device\":\"esp32-first\",\"firmware\":\"v1\",\"mission\":\"first \\\\ mission\","
              "\"session\":\"2026-11-04T13:06:33Z\",\"fields\":["
              "{\"channel\":\"sample\",\"name\":\"wave\",\"min\":-1,\"max\":1,"
              "\"about\":\"generated \\\"sine\\\"\\n\"},"
              "{\"channel\":\"sample\",\"name\":\"uptime_s\",\"unit\":\"s\",\"max\":20.5}]}");
    alloy_meta_t bare = {"d", NULL, "", 0, NULL, 0};
    alloy_meta_json(&bare, out, sizeof(out));
    CHECK_STR(out, "{\"device\":\"d\",\"session\":\"1970-01-01T00:00:00Z\",\"fields\":[]}");
    CHECK(alloy_meta_json(&m, out, 40) == 0 && out[0] == 0);
    char ctl[] = "a\x01z";
    alloy_meta_t c = {ctl, NULL, NULL, 0, NULL, 0};
    alloy_meta_json(&c, out, sizeof(out));
    CHECK(strstr(out, "\"a\\u0001z\"") != NULL);
}

static void test_copy_str(void)
{
    char s[4];
    alloy_copy_str(s, sizeof(s), "abcdef");
    CHECK_STR(s, "abc");
    alloy_copy_str(s, sizeof(s), NULL);
    CHECK_STR(s, "");
}

int main(void)
{
    test_record_encoding();
    test_record_boot_stamp_padded();
    test_record_rejects_bad_names_and_channel();
    test_record_channel_truncated();
    test_record_overflow_rolls_back();
    test_sanitize();
    test_rebase();
    test_pool_init_bounds();
    test_pool_header_once_per_chunk();
    test_pool_seals_at_90_percent();
    test_pool_field_change_reseals();
    test_pool_timed_flush();
    test_pool_no_timed_flush_while_pending();
    test_pool_timed_flush_needs_free_buffer();
    test_pool_sheds_oldest_pending();
    test_pool_never_sheds_just_sealed();
    test_pool_field_change_never_sheds_its_chunk();
    test_pool_drops_unfittable_rows_and_extra_channels();
    test_pool_sanitizes_buffer_channel();
    test_pool_close_take_release();
    test_pool_outage_keeps_every_row();
    test_retry_policy();
    test_http_date();
    test_end_gate();
    test_mesh_path();
    test_finalize_ms();
    test_status_text();
    test_meta_json();
    test_copy_str();
    if (failures) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("all %d checks passed\n", checks);
    return 0;
}
