/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * Zephyr glue: static buffer pool under a k_mutex, one uploader thread, SNTP with the HTTP Date
 * fallback, and the ingest protocol (POST /v1/meta, /v1/chunk, /v1/end).
 *
 * Wire behaviour matches the Arduino package (mcu/arduino): the ingest service cannot tell the
 * two apart.
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/sntp.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <alloy_storage/alloy_storage.h>

#include "alloy_csv.h"
#include "alloy_http.h"
#include "alloy_meta.h"
#include "alloy_net.h"
#include "alloy_policy.h"
#include "alloy_pool.h"

LOG_MODULE_REGISTER(alloy_storage, CONFIG_ALLOY_STORAGE_LOG_LEVEL);

#define NBUF       CONFIG_ALLOY_STORAGE_BUFFER_COUNT
#define BUF_BYTES  CONFIG_ALLOY_STORAGE_BUFFER_SIZE
#define META_BYTES (256 + CONFIG_ALLOY_STORAGE_MAX_DESCRIBE * 192)
#define IDLE_TICK  K_MSEC(250)
#define SNTP_TRY_MS 1000

BUILD_ASSERT(NBUF >= 2 && NBUF <= ALLOY_POOL_MAX_BUFS, "buffer count out of range");
BUILD_ASSERT(CONFIG_ALLOY_STORAGE_MAX_CHANNELS <= ALLOY_POOL_MAX_CHANNELS, "too many channels");

static const unsigned char ca_bundle[] = {
#include "ca-bundle.pem.inc"
	0x00, /* mbedTLS parses PEM only from a NUL-terminated buffer */
};

static char pool_mem[NBUF * BUF_BYTES] __aligned(4);
static struct alloy_pool pool;
static struct alloy_desc descs[CONFIG_ALLOY_STORAGE_MAX_DESCRIBE];
static size_t n_descs;
static char meta_json[META_BYTES];
static struct alloy_http http;

/* lock guards the pool, the counters and the flags below; http_lock the shared connection. */
static K_MUTEX_DEFINE(lock);
static K_MUTEX_DEFINE(http_lock);
static K_SEM_DEFINE(work, 0, 1);
static K_THREAD_STACK_DEFINE(uploader_stack, CONFIG_ALLOY_STORAGE_THREAD_STACK_SIZE);
static struct k_thread uploader;

/* The run's UTC clock: wall = uptime + wall_offset_ns, set once, then fixed for the run. */
static atomic_t clock_set;
static uint64_t wall_offset_ns;

static struct {
	char api_key[160];
	char mesh[129];
	char device[ALLOY_CHAN_MAX];
	char firmware[32];
	char mission[64];
	char finalize_ms[12];
	char session_str[12];
	char auth[168];
	bool started;
	bool ending;
	bool end_accepted;
	bool ready;
	bool meta_settled;
	bool meta_delivered;
	bool in_flight;
	enum alloy_clock_source clock;
	uint32_t session;
	uint32_t seq;
	uint32_t delivered;
	uint32_t failed;
	uint32_t retried;
	uint32_t stale;
	int last_status;
	int meta_status;
	int delivery_failure;
} st;

static uint64_t uptime_ns(void)
{
	return k_ticks_to_ns_floor64(k_uptime_ticks());
}

uint64_t alloy_storage_now_ns(void)
{
	uint64_t up = uptime_ns();

	return atomic_get(&clock_set) ? up + wall_offset_ns : up;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
	size_t n = src ? strlen(src) : 0;

	if (n >= cap) {
		n = cap - 1;
	}
	memcpy(dst, src ? src : "", n);
	dst[n] = '\0';
}

static void set_status(int status)
{
	k_mutex_lock(&lock, K_FOREVER);
	st.last_status = status;
	k_mutex_unlock(&lock);
}

static void count_retry(void)
{
	k_mutex_lock(&lock, K_FOREVER);
	st.retried++;
	k_mutex_unlock(&lock);
}

static uint32_t timeout_ms_left(k_timepoint_t deadline)
{
	k_timeout_t remain = sys_timepoint_timeout(deadline);

	if (K_TIMEOUT_EQ(remain, K_FOREVER)) {
		return UINT32_MAX;
	}
	return (uint32_t)MIN(k_ticks_to_ms_floor64(remain.ticks), UINT32_MAX);
}

static bool ok_status(int status)
{
	return status >= 200 && status < 300;
}

/* Delivery failure the end gate sees: a terminal upload status, else a shed buffer. */
static int delivery_failure_locked(void)
{
	if (st.delivery_failure) {
		return st.delivery_failure;
	}
	return pool.dropped_bufs ? -ENOBUFS : 0;
}

static void default_device_id(char *out)
{
	const char *cfg = CONFIG_ALLOY_STORAGE_DEVICE_ID;

	if (cfg[0]) {
		copy_str(out, ALLOY_CHAN_MAX, cfg);
		return;
	}
	copy_str(out, 17, CONFIG_BOARD);
#if defined(CONFIG_HWINFO)
	uint8_t id[16];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	if (n >= 3) {
		size_t len = strlen(out);

		snprintf(out + len, ALLOY_CHAN_MAX - len, "-%02x%02x%02x", id[n - 3], id[n - 2],
			 id[n - 1]);
	}
#endif
}

/* ---- delivery ------------------------------------------------------------------------- */

static bool wait_for_net(k_timeout_t timeout)
{
	if (alloy_storage_net_ready()) {
		return true;
	}
	set_status(-ENETDOWN);
	alloy_net_kick();
	return alloy_net_wait(timeout);
}

static int post(const char *route, const uint8_t *body, size_t len, const char *content_type,
		const char *chan, uint32_t seq)
{
	struct alloy_http_header headers[7];
	struct alloy_http_response resp;
	char seq_str[12];
	size_t n = 0;
	int rc;

	headers[n++] = (struct alloy_http_header){"Authorization", st.auth};
	headers[n++] = (struct alloy_http_header){"X-Alloy-Device", st.device};
	headers[n++] = (struct alloy_http_header){"X-Alloy-Session", st.session_str};
	headers[n++] = (struct alloy_http_header){"X-Alloy-Mesh-Path", st.mesh};
	if (st.finalize_ms[0]) {
		headers[n++] = (struct alloy_http_header){"X-Alloy-Finalize-Ms", st.finalize_ms};
	}
	if (chan) {
		snprintf(seq_str, sizeof(seq_str), "%u", (unsigned int)seq);
		headers[n++] = (struct alloy_http_header){"X-Alloy-Channel", chan};
		headers[n++] = (struct alloy_http_header){"X-Alloy-Seq", seq_str};
	}

	k_mutex_lock(&http_lock, K_FOREVER);
	rc = alloy_http_request(&http, "POST", route, headers, n, content_type, body, len, &resp);
	k_mutex_unlock(&http_lock);
	set_status(rc);
	if (!ok_status(rc)) {
		LOG_WRN("POST %s -> %d", route, rc);
	}
	return rc;
}

static void set_clock(uint64_t epoch_ns, enum alloy_clock_source source)
{
	wall_offset_ns = epoch_ns - uptime_ns();
	atomic_set(&clock_set, 1);
	k_mutex_lock(&lock, K_FOREVER);
	st.clock = source;
	k_mutex_unlock(&lock);
}

static bool clock_from_sntp(void)
{
	struct sntp_time ts;
	int rc = sntp_simple(CONFIG_ALLOY_STORAGE_SNTP_SERVER, SNTP_TRY_MS, &ts);

	if (rc < 0 || ts.seconds < ALLOY_EPOCH_SANE) {
		LOG_DBG("SNTP %s failed (%d)", CONFIG_ALLOY_STORAGE_SNTP_SERVER, rc);
		return false;
	}
	set_clock(ts.seconds * NSEC_PER_SEC + ((uint64_t)ts.fraction * NSEC_PER_SEC >> 32),
		  ALLOY_CLOCK_SNTP);
	LOG_INF("clock set from SNTP");
	return true;
}

/* Sets the clock from the Date header of GET /v1/health, on the same verified connection the
 * uploader uses. Certificate dates are not checked (MBEDTLS_HAVE_TIME_DATE is off), so this works
 * before the clock is set. Date is truncated to the second, so this clock runs up to 1 s behind.
 */
static bool clock_from_http_date(void)
{
	struct alloy_http_response resp;
	uint32_t epoch;
	int rc;

	k_mutex_lock(&http_lock, K_FOREVER);
	rc = alloy_http_request(&http, "GET", "/v1/health", NULL, 0, NULL, NULL, 0, &resp);
	k_mutex_unlock(&http_lock);
	if (rc < 0) {
		set_status(rc);
		LOG_WRN("clock fallback: GET /v1/health failed (%d)", rc);
		return false;
	}
	if (!alloy_parse_http_date(resp.date, &epoch) || epoch < ALLOY_EPOCH_SANE) {
		LOG_WRN("clock fallback: no usable Date header (HTTP %d)", rc);
		return false;
	}
	set_clock((uint64_t)epoch * NSEC_PER_SEC, ALLOY_CLOCK_HTTP_DATE);
	LOG_INF("clock set from HTTP Date (SNTP unavailable)");
	return true;
}

/* Prefer SNTP; after a short grace take the server's Date instead of holding every buffer. SNTP
 * keeps being tried between fallback attempts.
 */
static void acquire_clock(void)
{
	int64_t t0 = k_uptime_get();
	int64_t next_fallback = CONFIG_ALLOY_STORAGE_SNTP_GRACE_MS;
	uint8_t attempt = 0;

	while (!atomic_get(&clock_set)) {
		if (!wait_for_net(K_SECONDS(1))) {
			continue;
		}
		if (clock_from_sntp()) {
			break;
		}
		if (k_uptime_get() - t0 >= next_fallback) {
			if (clock_from_http_date()) {
				break;
			}
			next_fallback = k_uptime_get() - t0 + alloy_retry_delay_ms(attempt);
			if (attempt < 6) {
				attempt++;
			}
		}
		k_sleep(K_MSEC(200));
	}
}

static void upload_meta(void)
{
	int len = alloy_meta_json(meta_json, sizeof(meta_json), st.device, st.firmware,
				  st.mission, st.session, descs, n_descs);
	uint8_t attempt = 0;
	bool delivered = false;
	int status;

	if (len < 0) {
		LOG_ERR("metadata does not fit %u bytes", (unsigned int)sizeof(meta_json));
		len = alloy_meta_json(meta_json, sizeof(meta_json), st.device, st.firmware,
				      st.mission, st.session, NULL, 0);
	}
	/* Transient auth, network or service failures must not turn a healthy key into a
	 * data-less run: hold and retry the sidecar just like a data chunk.
	 */
	for (;;) {
		status = -ENETDOWN;
		if (wait_for_net(K_SECONDS(5))) {
			status = post("/v1/meta", (const uint8_t *)meta_json, (size_t)len,
				      "application/json", NULL, 0);
		}
		if (ok_status(status)) {
			delivered = true;
			break;
		}
		if (status == 409) {
			k_mutex_lock(&lock, K_FOREVER);
			st.stale++;
			k_mutex_unlock(&lock);
			break;
		}
		if (!alloy_retryable_status(status)) {
			k_mutex_lock(&lock, K_FOREVER);
			st.failed++;
			k_mutex_unlock(&lock);
			break;
		}
		count_retry();
		k_sleep(K_MSEC(alloy_retry_delay_ms(attempt)));
		if (attempt < 6) {
			attempt++;
		}
	}
	k_mutex_lock(&lock, K_FOREVER);
	st.meta_status = status;
	st.meta_delivered = delivered;
	st.meta_settled = true; /* last: end() may use the shared connection after this */
	k_mutex_unlock(&lock);
}

static void upload_chunk(uint8_t idx)
{
	struct alloy_buf *b = &pool.bufs[idx];
	uint32_t seq = st.seq++; /* once per buffer: retries reuse it (the dedupe key) */
	uint8_t attempt = 0;
	int status;

	alloy_rebase_rows(b->data, b->len, wall_offset_ns);
	for (;;) {
		status = -ENETDOWN;
		if (wait_for_net(K_SECONDS(5))) {
			status = post("/v1/chunk", (const uint8_t *)b->data, b->len, "text/csv",
				      b->chan, seq);
		}
		if (ok_status(status) || status == 409 || !alloy_retryable_status(status)) {
			break;
		}
		count_retry();
		k_sleep(K_MSEC(alloy_retry_delay_ms(attempt)));
		if (attempt < 6) {
			attempt++;
		}
	}

	k_mutex_lock(&lock, K_FOREVER);
	if (ok_status(status)) {
		st.delivered++;
	} else if (status == 409) {
		st.stale++; /* the run already finalized: terminal */
		st.delivery_failure = 409;
	} else {
		st.failed++;
		st.delivery_failure = status ? status : -ENOBUFS;
	}
	alloy_pool_release(&pool, idx);
	st.in_flight = false;
	k_mutex_unlock(&lock);
}

static void uploader_main(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	acquire_clock();

	k_mutex_lock(&lock, K_FOREVER);
	st.session = (uint32_t)(alloy_storage_now_ns() / NSEC_PER_SEC);
	snprintf(st.session_str, sizeof(st.session_str), "%u", (unsigned int)st.session);
	st.ready = true;
	k_mutex_unlock(&lock);
	LOG_INF("run %s/%u started as device %s", st.mesh, (unsigned int)st.session, st.device);

	upload_meta();

	for (;;) {
		uint8_t idx;
		bool have;

		k_mutex_lock(&lock, K_FOREVER);
		have = alloy_pool_take(&pool, &idx);
		st.in_flight = have;
		k_mutex_unlock(&lock);

		if (have) {
			upload_chunk(idx);
			continue;
		}
		k_sem_take(&work, IDLE_TICK);
		k_mutex_lock(&lock, K_FOREVER);
		alloy_pool_flush_due(&pool, k_uptime_get_32());
		k_mutex_unlock(&lock);
	}
}

/* ---- public API ----------------------------------------------------------------------- */

int alloy_storage_describe(const char *channel, const char *field, const char *unit, float lo,
			   float hi, const char *about)
{
	int rc = 0;

	if (!channel || !field) {
		return -EINVAL;
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (st.started) {
		rc = -EALREADY;
	} else if (n_descs >= ARRAY_SIZE(descs)) {
		rc = -ENOMEM;
	} else {
		alloy_desc_set(&descs[n_descs++], channel, field, unit, lo, hi, about);
	}
	k_mutex_unlock(&lock);
	return rc;
}

int alloy_storage_begin(const struct alloy_storage_config *cfg)
{
	const char *url;
	int rc;

	k_mutex_lock(&lock, K_FOREVER);
	if (st.started) {
		k_mutex_unlock(&lock);
		return -EALREADY;
	}
	k_mutex_unlock(&lock);

	if (!cfg || !cfg->api_key || !cfg->api_key[0] ||
	    strlen(cfg->api_key) >= sizeof(st.api_key) || !alloy_valid_mesh_path(cfg->mesh_path)) {
		set_status(-EINVAL);
		LOG_ERR("missing API key or invalid mesh path");
		return -EINVAL;
	}
	url = cfg->ingest_url ? cfg->ingest_url : CONFIG_ALLOY_STORAGE_INGEST_URL;
	rc = alloy_http_init(&http, url, cfg->insecure, CONFIG_ALLOY_STORAGE_TLS_SEC_TAG,
			     CONFIG_ALLOY_STORAGE_HTTP_TIMEOUT_MS);
	if (rc < 0) {
		set_status(-EINVAL);
		LOG_ERR("invalid ingest URL");
		return rc;
	}
	if (http.tls) {
		rc = tls_credential_add(CONFIG_ALLOY_STORAGE_TLS_SEC_TAG,
					TLS_CREDENTIAL_CA_CERTIFICATE, ca_bundle, sizeof(ca_bundle));
		if (rc < 0 && rc != -EEXIST) {
			LOG_ERR("could not register the CA bundle (%d)", rc);
			set_status(rc);
			return rc;
		}
	}

	copy_str(st.api_key, sizeof(st.api_key), cfg->api_key);
	snprintf(st.auth, sizeof(st.auth), "Bearer %s", st.api_key);
	copy_str(st.mesh, sizeof(st.mesh), cfg->mesh_path);
	if (cfg->device_id && cfg->device_id[0]) {
		copy_str(st.device, sizeof(st.device), cfg->device_id);
	} else {
		default_device_id(st.device);
	}
	alloy_sanitize_name(st.device);
	copy_str(st.firmware, sizeof(st.firmware), cfg->firmware);
	copy_str(st.mission, sizeof(st.mission), cfg->mission);
	st.finalize_ms[0] = '\0';
	if (cfg->finalize_after_s) {
		snprintf(st.finalize_ms, sizeof(st.finalize_ms), "%u",
			 (unsigned int)CLAMP(cfg->finalize_after_s * 1000u, 1000u, 99999999u));
	}

	rc = alloy_pool_init(&pool, pool_mem, NBUF, BUF_BYTES, CONFIG_ALLOY_STORAGE_MAX_CHANNELS,
			     CONFIG_ALLOY_STORAGE_FLUSH_INTERVAL_MS);
	if (rc < 0) {
		set_status(rc);
		return rc;
	}

	k_mutex_lock(&lock, K_FOREVER);
	st.started = true;
	st.last_status = 0;
	k_mutex_unlock(&lock);

	k_thread_create(&uploader, uploader_stack, K_THREAD_STACK_SIZEOF(uploader_stack),
			uploader_main, NULL, NULL, NULL, CONFIG_ALLOY_STORAGE_THREAD_PRIORITY, 0,
			K_NO_WAIT);
	k_thread_name_set(&uploader, "alloy_up");
	return 0;
}

void alloy_storage_record(struct alloy_record *rec, const char *channel)
{
	alloy_record_init(rec, channel, alloy_storage_now_ns());
}

int alloy_storage_commit(struct alloy_record *rec)
{
	bool wake;
	int rc;

	if (!rec || rec->done || rec->hlen <= 4) {
		return -EINVAL; /* no fields: ingest rejects a header without one */
	}
	k_mutex_lock(&lock, K_FOREVER);
	if (!st.started || st.ending) {
		k_mutex_unlock(&lock);
		return -EPERM;
	}
	rc = alloy_pool_commit(&pool, rec->chan, rec->hdr, rec->hlen, rec->row, rec->rlen,
			       rec->sig, k_uptime_get_32());
	wake = alloy_pool_pending_count(&pool) > 0;
	k_mutex_unlock(&lock);
	rec->done = true;
	if (wake) {
		k_sem_give(&work);
	}
	return rc;
}

int alloy_storage_log_fields(const char *channel, const struct alloy_field *fields, size_t n)
{
	struct alloy_record rec;

	alloy_storage_record(&rec, channel);
	for (size_t i = 0; i < n; i++) {
		alloy_record_set(&rec, fields[i].name, fields[i].value);
	}
	return alloy_storage_commit(&rec);
}

int alloy_storage_end(k_timeout_t timeout)
{
	k_timepoint_t deadline = sys_timepoint_calc(timeout);
	enum alloy_end_gate gate;
	uint8_t attempt = 0;
	int status;

	k_mutex_lock(&lock, K_FOREVER);
	if (!st.started) {
		k_mutex_unlock(&lock);
		return -EPERM;
	}
	if (st.end_accepted) {
		k_mutex_unlock(&lock);
		return 0;
	}
	st.ending = true; /* stop producers before sealing their active buffers */
	alloy_pool_seal_all(&pool);
	k_mutex_unlock(&lock);
	k_sem_give(&work);

	/* Gate on metadata settlement and on every buffer back in the free ring, so /v1/end can
	 * never race the shared connection or finalize ahead of a chunk still in flight.
	 */
	for (;;) {
		k_mutex_lock(&lock, K_FOREVER);
		gate = alloy_end_gate(st.meta_settled, st.meta_delivered,
				      alloy_pool_free_count(&pool), pool.n,
				      delivery_failure_locked());
		k_mutex_unlock(&lock);
		if (gate != ALLOY_END_WAITING || sys_timepoint_expired(deadline)) {
			break;
		}
		k_sleep(K_MSEC(50));
	}
	if (gate == ALLOY_END_WAITING) {
		set_status(-EINPROGRESS);
		return -EINPROGRESS; /* never finalize ahead of data still being retried */
	}
	if (gate == ALLOY_END_FAILED) {
		k_mutex_lock(&lock, K_FOREVER);
		if (!st.meta_delivered) {
			st.last_status = st.meta_status ? st.meta_status : -ENOBUFS;
		} else {
			st.last_status = delivery_failure_locked();
		}
		k_mutex_unlock(&lock);
		return -EIO;
	}

	/* Spend the rest of the bound surviving a dropped finalize request. 202/204 prove the
	 * service accepted it; the MCAP itself is assembled asynchronously.
	 */
	do {
		uint32_t remain_ms = timeout_ms_left(deadline);

		status = -ENETDOWN;
		if (wait_for_net(K_MSEC(MIN(remain_ms, 1000u)))) {
			status = post("/v1/end", NULL, 0, "application/octet-stream", NULL, 0);
		}
		if (ok_status(status)) {
			k_mutex_lock(&lock, K_FOREVER);
			st.end_accepted = true;
			k_mutex_unlock(&lock);
			return 0;
		}
		if (!alloy_retryable_status(status)) {
			return -EIO;
		}
		count_retry();
		remain_ms = timeout_ms_left(deadline);
		if (remain_ms == 0) {
			break;
		}
		k_sleep(K_MSEC(MIN(alloy_retry_delay_ms(attempt), remain_ms)));
		if (attempt < 6) {
			attempt++;
		}
	} while (!sys_timepoint_expired(deadline));
	set_status(-EINPROGRESS);
	return -EINPROGRESS;
}

void alloy_storage_get_stats(struct alloy_storage_stats *s)
{
	k_mutex_lock(&lock, K_FOREVER);
	s->delivered = st.delivered;
	s->failed = st.failed;
	s->retried = st.retried;
	s->dropped = pool.dropped_bufs;
	s->dropped_rows = pool.dropped_rows;
	s->stale = st.stale;
	s->queued = (st.started ? alloy_pool_pending_count(&pool) : 0) + (st.in_flight ? 1 : 0);
	s->ready = st.ready;
	s->clock = st.clock;
	s->session = st.session;
	s->last_status = st.last_status;
	k_mutex_unlock(&lock);
}

const char *alloy_storage_last_error(void)
{
	static char buf[96];
	int status;

	k_mutex_lock(&lock, K_FOREVER);
	status = st.last_status;
	k_mutex_unlock(&lock);

	if (status == 0 || ok_status(status)) {
		return "";
	}
	switch (status) {
	case -ENETDOWN:
		return "network not connected";
	case -EINVAL:
		return "missing API key, invalid mesh path or invalid ingest URL";
	case -EINPROGRESS:
		return "metadata or buffer drain timed out; delivery is still retrying";
	case -ENOBUFS:
		return "a buffered data chunk was dropped before delivery";
	case 400:
	case 413:
		snprintf(buf, sizeof(buf),
			 "upload rejected; check mesh path and buffer size (HTTP %d)", status);
		return buf;
	case 401:
	case 403:
		snprintf(buf, sizeof(buf), "Alloy API key rejected (HTTP %d)", status);
		return buf;
	case 409:
		return "run was already finalized (HTTP 409)";
	case 429:
		return "upload rate limited (HTTP 429); delivery is retrying";
	default:
		break;
	}
	if (status < 0) {
		snprintf(buf, sizeof(buf), "network or TLS transport error (%d); delivery is retrying",
			 status);
	} else if (status >= 500) {
		snprintf(buf, sizeof(buf),
			 "Alloy ingest temporarily unavailable (HTTP %d); delivery is retrying",
			 status);
	} else {
		snprintf(buf, sizeof(buf), "upload failed (HTTP %d)", status);
	}
	return buf;
}
