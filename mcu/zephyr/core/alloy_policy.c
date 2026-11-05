/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#include "alloy_policy.h"

#include <string.h>

bool alloy_retryable_status(int status)
{
	return status <= 0 || status == 408 || status == 425 || status == 429 || status >= 500;
}

uint32_t alloy_retry_delay_ms(uint8_t attempt)
{
	uint8_t shift = attempt > 5 ? 5 : attempt;
	uint32_t ms = 1000u << shift;

	return ms > 30000u ? 30000u : ms;
}

enum alloy_end_gate alloy_end_gate(bool meta_settled, bool meta_delivered, unsigned int free_bufs,
				   unsigned int total_bufs, int delivery_failure)
{
	if (!meta_settled || free_bufs < total_bufs) {
		return ALLOY_END_WAITING;
	}
	if (!meta_delivered || delivery_failure != 0) {
		return ALLOY_END_FAILED;
	}
	return ALLOY_END_READY;
}

static bool parse_digits(const char *p, int count, int *out)
{
	int value = 0;

	for (int i = 0; i < count; i++) {
		if (p[i] < '0' || p[i] > '9') {
			return false;
		}
		value = value * 10 + (p[i] - '0');
	}
	*out = value;
	return true;
}

bool alloy_parse_http_date(const char *s, uint32_t *epoch)
{
	static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
	static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int day, year, hour, minute, second, month = 0;

	if (!s || !epoch || strlen(s) != 29) {
		return false;
	}
	if (s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
	    s[19] != ':' || s[22] != ':' || s[25] != ' ' || strcmp(s + 26, "GMT") != 0) {
		return false;
	}
	if (!parse_digits(s + 5, 2, &day) || !parse_digits(s + 12, 4, &year) ||
	    !parse_digits(s + 17, 2, &hour) || !parse_digits(s + 20, 2, &minute) ||
	    !parse_digits(s + 23, 2, &second)) {
		return false;
	}
	for (int m = 0; m < 12; m++) {
		if (strncmp(s + 8, months + 3 * m, 3) == 0) {
			month = m + 1;
		}
	}
	bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;

	if (month == 0 || year < 1970 || year > 2105 || hour > 23 || minute > 59 || second > 60 ||
	    day < 1 || day > mdays[month - 1] + (month == 2 && leap ? 1 : 0)) {
		return false;
	}
	/* Days since 1970-01-01 (Hinnant's days_from_civil; years >= 1970 keep every term >= 0). */
	int y = year - (month <= 2 ? 1 : 0);
	int era = y / 400;
	int yoe = y - era * 400;
	int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	uint32_t days = (uint32_t)(era * 146097 + doe - 719468);

	*epoch = days * 86400u + (uint32_t)(hour * 3600 + minute * 60 + second);
	return true;
}

static void put_digits(char *out, uint32_t v, int width)
{
	for (int i = width - 1; i >= 0; i--) {
		out[i] = (char)('0' + v % 10u);
		v /= 10u;
	}
}

void alloy_epoch_to_iso8601(uint32_t epoch, char *out)
{
	/* Hinnant's civil_from_days. */
	uint32_t days = epoch / 86400u;
	uint32_t secs = epoch % 86400u;
	uint32_t z = days + 719468u;
	uint32_t era = z / 146097u;
	uint32_t doe = z - era * 146097u;
	uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
	uint32_t y = yoe + era * 400u;
	uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
	uint32_t mp = (5u * doy + 2u) / 153u;
	uint32_t d = doy - (153u * mp + 2u) / 5u + 1u;
	uint32_t m = mp < 10u ? mp + 3u : mp - 9u;

	if (m <= 2u) {
		y++;
	}
	put_digits(out, y, 4);
	out[4] = '-';
	put_digits(out + 5, m, 2);
	out[7] = '-';
	put_digits(out + 8, d, 2);
	out[10] = 'T';
	put_digits(out + 11, secs / 3600u, 2);
	out[13] = ':';
	put_digits(out + 14, secs / 60u % 60u, 2);
	out[16] = ':';
	put_digits(out + 17, secs % 60u, 2);
	out[19] = 'Z';
	out[20] = '\0';
}

bool alloy_valid_mesh_path(const char *path)
{
	size_t len;

	if (!path) {
		return false;
	}
	len = strlen(path);
	if (len == 0 || len > 128 || path[0] == '/' || path[len - 1] == '/') {
		return false;
	}
	for (size_t i = 0; i < len; i++) {
		char c = path[i];
		bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
			  (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '/';
		if (!ok) {
			return false;
		}
	}
	return true;
}
