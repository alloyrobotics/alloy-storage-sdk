/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include <math.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "alloy_csv.h"

ZTEST_SUITE(alloy_csv, NULL, NULL, NULL, NULL, NULL);

ZTEST(alloy_csv, test_record_header_and_row)
{
	struct alloy_record r;

	alloy_record_init(&r, "imu", 1782715694000000000ULL);
	alloy_record_set(&r, "pitch", 0.52f);
	alloy_record_set(&r, "roll", -1.25f);
	alloy_record_set(&r, "count", 1234567.0f);

	zassert_str_equal(r.chan, "imu");
	zassert_str_equal(r.hdr, "t_ns,pitch,roll,count");
	zassert_str_equal(r.row, "1782715694000000000,0.52,-1.25,1.23457e+06");
	zassert_equal(r.hlen, (int)strlen(r.hdr));
	zassert_equal(r.rlen, (int)strlen(r.row));
	zassert_equal(r.sig, alloy_fnv1a(r.hdr, strlen(r.hdr), ALLOY_FNV_SEED),
		      "signature must equal one pass over the header text");
}

ZTEST(alloy_csv, test_boot_relative_stamp_is_zero_padded)
{
	struct alloy_record r;

	alloy_record_init(&r, "c", 1500000000ULL);
	alloy_record_set(&r, "v", 1.0f);
	zassert_str_equal(r.row, "0000000001500000000,1");
}

ZTEST(alloy_csv, test_nan_and_inf_are_empty_cells)
{
	struct alloy_record r;

	alloy_record_init(&r, "c", 0);
	alloy_record_set(&r, "a", NAN);
	alloy_record_set(&r, "b", INFINITY);
	alloy_record_set(&r, "c", 2.0f);
	zassert_str_equal(r.hdr, "t_ns,a,b,c");
	zassert_str_equal(r.row, "0000000000000000000,,,2");
}

ZTEST(alloy_csv, test_overflowing_field_rolls_back_both)
{
	struct alloy_record r;
	char name[40];
	int fields = 0;

	alloy_record_init(&r, "wide", 0);
	for (int i = 0; i < 100; i++) {
		snprintf(name, sizeof(name), "field_with_long_name_%02d", i);
		alloy_record_set(&r, name, -1.23456f);
	}
	for (const char *p = r.hdr; *p; p++) {
		fields += *p == ',';
	}
	int cells = 0;

	for (const char *p = r.row; *p; p++) {
		cells += *p == ',';
	}
	zassert_true(fields > 0);
	zassert_equal(fields, cells, "header and row must stay aligned");
	zassert_true(r.hlen < ALLOY_HDR_MAX && r.rlen < ALLOY_ROW_MAX);
}

ZTEST(alloy_csv, test_channel_truncated_and_sanitized)
{
	struct alloy_record r;
	char chan[ALLOY_CHAN_MAX];

	alloy_record_init(&r, "a very long channel name with spaces", 0);
	zassert_equal(strlen(r.chan), ALLOY_CHAN_MAX - 1);
	strcpy(chan, r.chan);
	alloy_sanitize_name(chan);
	zassert_str_equal(chan, "a_very_long_channel_nam");
}

ZTEST(alloy_csv, test_empty_channel_is_refused)
{
	struct alloy_record r;

	alloy_record_init(&r, "", 0);
	alloy_record_set(&r, "v", 1.0f);
	zassert_true(r.done);
	zassert_str_equal(r.hdr, "t_ns");
}

ZTEST(alloy_csv, test_rebase_rewrites_only_boot_relative_rows)
{
	char buf[] = "t_ns,v\n"
		     "0000000001000000000,1\n"
		     "1782715694000000000,2\n"
		     "0000000002000000000,3";
	size_t n = alloy_rebase_rows(buf, strlen(buf), 1782715690000000000ULL);

	zassert_equal(n, 2);
	zassert_str_equal(buf, "t_ns,v\n"
			       "1782715691000000000,1\n"
			       "1782715694000000000,2\n"
			       "1782715692000000000,3");
}

ZTEST(alloy_csv, test_rebase_without_offset_is_a_noop)
{
	char buf[] = "t_ns,v\n0000000001000000000,1\n";

	zassert_equal(alloy_rebase_rows(buf, strlen(buf), 0), 0);
	zassert_str_equal(buf, "t_ns,v\n0000000001000000000,1\n");
}
