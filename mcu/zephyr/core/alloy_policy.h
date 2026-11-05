/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * Small delivery policies shared with the other C/C++ packages: retry and backoff, the end-of-run
 * gate, the HTTP Date clock fallback and mesh path validation. Platform independent.
 */

#ifndef ALLOY_CORE_POLICY_H_
#define ALLOY_CORE_POLICY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* time() below this means the clock has not been set yet. */
#define ALLOY_EPOCH_SANE 1700000000u

/* Transport errors (negative) and 408/425/429/5xx are retried; other HTTP statuses are terminal. */
bool alloy_retryable_status(int status);

/* 1 s, 2 s, 4 s ... capped at 30 s. */
uint32_t alloy_retry_delay_ms(uint8_t attempt);

enum alloy_end_gate {
	ALLOY_END_WAITING,
	ALLOY_END_READY,
	ALLOY_END_FAILED,
};

/* A graceful end is ready only after metadata has settled and every buffer has come back to the
 * free ring. Returned buffers are not proof of delivery when one was terminally lost, so any
 * delivery failure (or undelivered metadata) turns a complete drain into END_FAILED.
 */
enum alloy_end_gate alloy_end_gate(bool meta_settled, bool meta_delivered, unsigned int free_bufs,
				   unsigned int total_bufs, int delivery_failure);

/* HTTP IMF-fixdate ("Wed, 04 Nov 2026 13:06:33 GMT", RFC 9110) to Unix seconds. The obsolete
 * RFC 850 and asctime forms are rejected.
 */
bool alloy_parse_http_date(const char *s, uint32_t *epoch);

/* Unix seconds to "YYYY-MM-DDTHH:MM:SSZ" (out holds at least 21 bytes). */
void alloy_epoch_to_iso8601(uint32_t epoch, char *out);

/* ^[A-Za-z0-9_/-]{1,128}$ with no leading or trailing slash. */
bool alloy_valid_mesh_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ALLOY_CORE_POLICY_H_ */
