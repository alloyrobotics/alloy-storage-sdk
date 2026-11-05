/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include "alloy_policy.h"

ZTEST_SUITE(alloy_policy, NULL, NULL, NULL, NULL, NULL);

ZTEST(alloy_policy, test_retryable_statuses)
{
	zassert_true(alloy_retryable_status(-ETIMEDOUT));
	zassert_true(alloy_retryable_status(-ENETDOWN));
	zassert_true(alloy_retryable_status(0));
	zassert_true(alloy_retryable_status(408));
	zassert_true(alloy_retryable_status(425));
	zassert_true(alloy_retryable_status(429));
	zassert_true(alloy_retryable_status(500));
	zassert_true(alloy_retryable_status(503));
	zassert_false(alloy_retryable_status(400));
	zassert_false(alloy_retryable_status(401));
	zassert_false(alloy_retryable_status(403));
	zassert_false(alloy_retryable_status(409));
	zassert_false(alloy_retryable_status(413));
}

ZTEST(alloy_policy, test_backoff_doubles_to_thirty_seconds)
{
	static const uint32_t want[] = {1000, 2000, 4000, 8000, 16000, 30000, 30000, 30000};

	for (uint8_t i = 0; i < ARRAY_SIZE(want); i++) {
		zassert_equal(alloy_retry_delay_ms(i), want[i], "attempt %u", i);
	}
	zassert_equal(alloy_retry_delay_ms(255), 30000);
}

ZTEST(alloy_policy, test_end_gate)
{
	zassert_equal(alloy_end_gate(false, false, 4, 4, 0), ALLOY_END_WAITING, "meta pending");
	zassert_equal(alloy_end_gate(true, true, 3, 4, 0), ALLOY_END_WAITING, "buffer out");
	zassert_equal(alloy_end_gate(true, true, 3, 4, 409), ALLOY_END_WAITING,
		      "failure waits for the drain");
	zassert_equal(alloy_end_gate(true, true, 4, 4, 0), ALLOY_END_READY);
	zassert_equal(alloy_end_gate(true, false, 4, 4, 0), ALLOY_END_FAILED, "meta lost");
	zassert_equal(alloy_end_gate(true, true, 4, 4, -ENOBUFS), ALLOY_END_FAILED, "shed");
	zassert_equal(alloy_end_gate(true, true, 4, 4, 400), ALLOY_END_FAILED, "rejected");
}

ZTEST(alloy_policy, test_http_date_parses_imf_fixdate)
{
	uint32_t e = 0;

	zassert_true(alloy_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT", &e));
	zassert_equal(e, 0);
	zassert_true(alloy_parse_http_date("Wed, 04 Nov 2026 13:06:33 GMT", &e));
	zassert_equal(e, 1793797593u);
	zassert_true(alloy_parse_http_date("Thu, 29 Feb 2024 23:59:59 GMT", &e));
	zassert_equal(e, 1709251199u);
	zassert_true(alloy_parse_http_date("Tue, 31 Dec 2099 12:00:00 GMT", &e));
	zassert_equal(e, 4102401600u);
}

ZTEST(alloy_policy, test_http_date_rejects_other_forms)
{
	uint32_t e = 7;

	zassert_false(alloy_parse_http_date(NULL, &e));
	zassert_false(alloy_parse_http_date("", &e));
	zassert_false(alloy_parse_http_date("Wednesday, 04-Nov-26 13:06:33 GMT", &e), "RFC 850");
	zassert_false(alloy_parse_http_date("Wed Nov  4 13:06:33 2026", &e), "asctime");
	zassert_false(alloy_parse_http_date("Wed, 04 Nov 2026 13:06:33 UTC", &e));
	zassert_false(alloy_parse_http_date("Wed, 04 Nov 2026 13:06:33 GMT ", &e));
	zassert_false(alloy_parse_http_date("Thu, 30 Feb 2024 00:00:00 GMT", &e));
	zassert_false(alloy_parse_http_date("Thu, 29 Feb 2023 00:00:00 GMT", &e));
	zassert_false(alloy_parse_http_date("Thu, 01 Foo 2024 00:00:00 GMT", &e));
	zassert_false(alloy_parse_http_date("Thu, 01 Jan 1969 00:00:00 GMT", &e));
	zassert_false(alloy_parse_http_date("Thu, 01 Jan 2024 24:00:00 GMT", &e));
	zassert_false(alloy_parse_http_date("Thu, 0x Jan 2024 00:00:00 GMT", &e));
	zassert_equal(e, 7, "output untouched on failure");
}

ZTEST(alloy_policy, test_iso8601_round_trips_with_the_date_parser)
{
	static const struct {
		const char *http;
		const char *iso;
	} cases[] = {
		{"Thu, 01 Jan 1970 00:00:00 GMT", "1970-01-01T00:00:00Z"},
		{"Wed, 04 Nov 2026 13:06:33 GMT", "2026-11-04T13:06:33Z"},
		{"Thu, 29 Feb 2024 23:59:59 GMT", "2024-02-29T23:59:59Z"},
		{"Fri, 01 Mar 2024 00:00:00 GMT", "2024-03-01T00:00:00Z"},
		{"Fri, 31 Dec 2038 23:59:59 GMT", "2038-12-31T23:59:59Z"},
		{"Mon, 01 Jan 2100 00:00:00 GMT", "2100-01-01T00:00:00Z"},
	};
	char iso[21];

	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		uint32_t e;

		zassert_true(alloy_parse_http_date(cases[i].http, &e), "%s", cases[i].http);
		alloy_epoch_to_iso8601(e, iso);
		zassert_str_equal(iso, cases[i].iso);
	}
}

ZTEST(alloy_policy, test_mesh_path_validation)
{
	char long_ok[129];
	char too_long[130];

	memset(long_ok, 'a', 128);
	long_ok[128] = '\0';
	memset(too_long, 'a', 129);
	too_long[129] = '\0';

	zassert_true(alloy_valid_mesh_path("robots/sbr"));
	zassert_true(alloy_valid_mesh_path("hardware-verification/esp32c3-zephyr_1"));
	zassert_true(alloy_valid_mesh_path("a"));
	zassert_true(alloy_valid_mesh_path(long_ok));
	zassert_false(alloy_valid_mesh_path(too_long));
	zassert_false(alloy_valid_mesh_path(NULL));
	zassert_false(alloy_valid_mesh_path(""));
	zassert_false(alloy_valid_mesh_path("/robots"));
	zassert_false(alloy_valid_mesh_path("robots/"));
	zassert_false(alloy_valid_mesh_path("robots sbr"));
	zassert_false(alloy_valid_mesh_path("robots.sbr"));
	zassert_false(alloy_valid_mesh_path("röbots"));
}
