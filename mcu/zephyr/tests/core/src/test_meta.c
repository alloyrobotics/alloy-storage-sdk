/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <math.h>

#include <zephyr/ztest.h>

#include "alloy_meta.h"

ZTEST_SUITE(alloy_meta, NULL, NULL, NULL, NULL, NULL);

ZTEST(alloy_meta, test_minimal_sidecar)
{
	char out[256];
	int n = alloy_meta_json(out, sizeof(out), "rover", NULL, "", 1793797593u, NULL, 0);

	zassert_true(n > 0);
	zassert_str_equal(out, "{\"device\":\"rover\",\"session\":\"2026-11-04T13:06:33Z\","
			       "\"fields\":[]}");
	zassert_equal(n, (int)strlen(out));
}

ZTEST(alloy_meta, test_full_sidecar_with_fields)
{
	struct alloy_desc d[2];
	char out[512];

	alloy_desc_set(&d[0], "sample", "wave", "", -1.0f, 1.0f, "generated sine wave");
	alloy_desc_set(&d[1], "batt", "volts", "V", NAN, 12.6f, NULL);
	zassert_true(alloy_meta_json(out, sizeof(out), "esp32-first-mission", "fm-v1",
				     "first Zephyr mission", 0, d, 2) > 0);
	zassert_str_equal(out,
			  "{\"device\":\"esp32-first-mission\",\"firmware\":\"fm-v1\","
			  "\"mission\":\"first Zephyr mission\",\"session\":\"1970-01-01T00:00:00Z\","
			  "\"fields\":[{\"channel\":\"sample\",\"name\":\"wave\",\"min\":-1,\"max\":1,"
			  "\"about\":\"generated sine wave\"},"
			  "{\"channel\":\"batt\",\"name\":\"volts\",\"unit\":\"V\",\"max\":12.6}]}");
}

ZTEST(alloy_meta, test_strings_are_escaped)
{
	char out[256];

	zassert_true(alloy_meta_json(out, sizeof(out), "d", NULL, "say \"hi\"\\\n\t\x01", 0, NULL,
				     0) > 0);
	zassert_not_null(strstr(out, "\"mission\":\"say \\\"hi\\\"\\\\\\n\\t\\u0001\""), "%s",
			 out);
}

ZTEST(alloy_meta, test_desc_strings_are_truncated)
{
	struct alloy_desc d;

	alloy_desc_set(&d, "a_channel_name_longer_than_24", "field_name_longer_than_20",
		       "unit_too_long", 0, 1, NULL);
	zassert_equal(strlen(d.chan), sizeof(d.chan) - 1);
	zassert_equal(strlen(d.field), sizeof(d.field) - 1);
	zassert_equal(strlen(d.unit), sizeof(d.unit) - 1);
	zassert_str_equal(d.about, "");
}

ZTEST(alloy_meta, test_overflow_is_reported)
{
	char out[40];

	zassert_equal(alloy_meta_json(out, sizeof(out), "rover", "v1", "a mission label", 0,
				      NULL, 0),
		      -ENOSPC);
}
