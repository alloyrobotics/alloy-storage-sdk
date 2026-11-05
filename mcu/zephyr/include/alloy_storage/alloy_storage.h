/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

/**
 * @file
 * @brief Alloy Storage SDK for Zephyr: stream robot telemetry into Alloy Storage.
 *
 * Log named fields on a channel from any thread. Rows are buffered in a static RAM pool and a
 * background thread uploads each channel as CSV chunks to Alloy Storage ingest, which assembles
 * the run into one indexed MCAP in your mesh. Logging never blocks on the network: during an
 * outage chunks wait in RAM (store and forward) and upload when the network returns.
 *
 * @code
 * static const struct alloy_storage_config cfg = {
 *	.api_key = ALLOY_API_KEY,
 *	.mesh_path = "robots/rover",
 *	.mission = "driveway test",
 * };
 *
 * alloy_storage_wifi_connect(SSID, PSK, K_SECONDS(20));   // optional
 * alloy_storage_begin(&cfg);
 *
 * ALLOY_LOG("imu", {"pitch", pitch}, {"roll", roll});      // one row, from any thread
 * alloy_storage_log("battery", volts);                     // single "value" field
 *
 * alloy_storage_end(K_SECONDS(15));                        // 0 = every buffer delivered
 * @endcode
 */

#ifndef ALLOY_STORAGE_ALLOY_STORAGE_H_
#define ALLOY_STORAGE_ALLOY_STORAGE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#include "alloy_csv.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Run configuration. Strings are copied by alloy_storage_begin(). */
struct alloy_storage_config {
	/** Alloy data API key (required). */
	const char *api_key;
	/** Mesh folder for the run, e.g. "robots/rover" (required). [A-Za-z0-9_/-], 1-128
	 *  characters, no leading or trailing slash.
	 */
	const char *mesh_path;
	/** Device id. NULL uses CONFIG_ALLOY_STORAGE_DEVICE_ID, or the board name plus a hardware
	 *  id suffix when that is empty. Sanitized to [A-Za-z0-9_-], at most 23 characters.
	 */
	const char *device_id;
	/** Firmware version, stored in the MCAP metadata (optional). */
	const char *firmware;
	/** Human-readable label for this run, stored in the MCAP metadata (optional). */
	const char *mission;
	/** Ingest base URL. NULL uses CONFIG_ALLOY_STORAGE_INGEST_URL. */
	const char *ingest_url;
	/** Seconds after the last data before the service finalizes the run on its own
	 *  (0 = service default, about 2 minutes; clamped server side to 30 s..30 min).
	 */
	uint32_t finalize_after_s;
	/** Skip TLS certificate verification. Only for networks where verification cannot work,
	 *  such as TLS-intercepting proxies. Default is verified against the built-in CA bundle.
	 */
	bool insecure;
};

/** One named value, for ALLOY_LOG() and alloy_storage_log_fields(). */
struct alloy_field {
	const char *name;
	float value;
};

/** Where the run's UTC clock came from. */
enum alloy_clock_source {
	ALLOY_CLOCK_NONE = 0,
	ALLOY_CLOCK_SNTP,
	ALLOY_CLOCK_HTTP_DATE,
};

/** Delivery counters (a consistent snapshot). */
struct alloy_storage_stats {
	uint32_t delivered;    /**< data chunks accepted by ingest */
	uint32_t failed;       /**< chunks rejected terminally (4xx other than 409) */
	uint32_t retried;      /**< transient upload attempts retried */
	uint32_t dropped;      /**< sealed chunks shed under backpressure */
	uint32_t dropped_rows; /**< rows refused before buffering */
	uint32_t stale;        /**< chunks refused because the run had finalized (409) */
	uint32_t queued;       /**< sealed or in-flight chunks right now */
	bool ready;            /**< UTC clock set and run identity fixed */
	enum alloy_clock_source clock;
	uint32_t session;      /**< run id (epoch seconds at clock sync), 0 until ready */
	int last_status;       /**< HTTP status of the last request, or negative errno */
};

/**
 * @brief Add a field description for Alloy AI (unit, range, meaning). Call before begin.
 *
 * @param lo, hi Expected range; NAN when unknown.
 * @return 0, -EALREADY after begin, -ENOMEM when CONFIG_ALLOY_STORAGE_MAX_DESCRIBE is reached.
 */
int alloy_storage_describe(const char *channel, const char *field, const char *unit, float lo,
			   float hi, const char *about);

/**
 * @brief Validate the configuration and start the background uploader.
 *
 * Does not need the network to be up: rows are buffered and stamped with boot time until the
 * clock is set, then rebased to UTC before upload.
 *
 * @return 0, -EINVAL (missing key, bad mesh path or URL), -EALREADY if already started.
 */
int alloy_storage_begin(const struct alloy_storage_config *cfg);

/** @brief Start a row on @p channel stamped now. Fill it with alloy_record_set(). */
void alloy_storage_record(struct alloy_record *rec, const char *channel);

/**
 * @brief Commit a row built with alloy_storage_record() and alloy_record_set().
 * @return 0, -ENOSPC when the row was dropped (counted), -EPERM before begin or after end.
 */
int alloy_storage_commit(struct alloy_record *rec);

/** @brief Log one row of @p n fields on @p channel. */
int alloy_storage_log_fields(const char *channel, const struct alloy_field *fields, size_t n);

/** @brief Log a single value as field "value" on @p channel. */
static inline int alloy_storage_log(const char *channel, float value)
{
	const struct alloy_field f = {"value", value};

	return alloy_storage_log_fields(channel, &f, 1);
}

/** @brief Log one row: ALLOY_LOG("imu", {"pitch", p}, {"roll", r}). */
#define ALLOY_LOG(channel, ...)                                                                    \
	alloy_storage_log_fields(                                                                  \
		(channel), (const struct alloy_field[]){__VA_ARGS__},                              \
		sizeof((const struct alloy_field[]){__VA_ARGS__}) / sizeof(struct alloy_field))

/**
 * @brief Graceful end of run.
 *
 * Stops logging, seals every open buffer, waits up to @p timeout for metadata and every buffer
 * to be delivered, then asks the service to finalize the MCAP now instead of after the
 * inactivity window.
 *
 * @retval 0 every buffer was delivered and finalization was accepted.
 * @retval -EINPROGRESS timed out; metadata or data is still retrying in the background.
 * @retval -EIO something inside the run was lost or rejected (see last_status).
 * @retval -EPERM not started.
 */
int alloy_storage_end(k_timeout_t timeout);

/** @brief Snapshot of the delivery counters. */
void alloy_storage_get_stats(struct alloy_storage_stats *stats);

/** @brief Printable explanation of the last status; "" when the last request succeeded. */
const char *alloy_storage_last_error(void);

/** @brief Wall-clock ns once the clock is set, boot-relative ns before. */
uint64_t alloy_storage_now_ns(void);

/**
 * @brief Connect the default Wi-Fi interface and wait for an IPv4 address.
 *
 * Optional helper. With CONFIG_ALLOY_STORAGE_WIFI_RECONNECT the uploader reuses these
 * credentials to reconnect when the link drops. An empty @p psk joins an open network.
 *
 * @return 0, -ETIMEDOUT, -ENODEV (no Wi-Fi interface) or the net_mgmt error.
 */
int alloy_storage_wifi_connect(const char *ssid, const char *psk, k_timeout_t timeout);

/** @brief True while the network has a usable IP connection (conn_mgr L4 state). */
bool alloy_storage_net_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* ALLOY_STORAGE_ALLOY_STORAGE_H_ */
