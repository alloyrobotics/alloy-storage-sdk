// policy.c: see alloy/policy.h.

#include "alloy/policy.h"

#include <stdio.h>
#include <string.h>

bool alloy_retryable(int status)
{
    return status <= 0 || status == 408 || status == 425 || status == 429 || status >= 500;
}

uint32_t alloy_retry_delay_ms(uint8_t attempt)
{
    uint8_t shift = attempt > 5 ? 5 : attempt;
    uint32_t ms = 1000u << shift;
    return ms > 30000u ? 30000u : ms;
}

static bool parse_digits(const char *p, int count, int *out)
{
    int value = 0;
    for (int i = 0; i < count; i++) {
        if (p[i] < '0' || p[i] > '9') return false;
        value = value * 10 + (p[i] - '0');
    }
    *out = value;
    return true;
}

bool alloy_parse_http_date(const char *s, uint32_t *epoch)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (!s || !epoch || strlen(s) != 29) return false;
    if (s[3] != ',' || s[4] != ' ' || s[7] != ' ' || s[11] != ' ' || s[16] != ' ' ||
        s[19] != ':' || s[22] != ':' || s[25] != ' ' || strcmp(s + 26, "GMT") != 0)
        return false;
    int day, year, hour, minute, second, month = 0;
    if (!parse_digits(s + 5, 2, &day) || !parse_digits(s + 12, 4, &year) ||
        !parse_digits(s + 17, 2, &hour) || !parse_digits(s + 20, 2, &minute) ||
        !parse_digits(s + 23, 2, &second))
        return false;
    for (int m = 0; m < 12; m++)
        if (strncmp(s + 8, months + 3 * m, 3) == 0) month = m + 1;
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if (month == 0 || year < 1970 || year > 2105 || hour > 23 || minute > 59 || second > 60 ||
        day < 1 || day > mdays[month - 1] + (month == 2 && leap ? 1 : 0))
        return false;
    // Days since 1970-01-01 (Hinnant's days_from_civil; years are >= 1970 so all terms are >= 0).
    int y = year - (month <= 2 ? 1 : 0);
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    uint32_t days = (uint32_t)(era * 146097 + doe - 719468);
    *epoch = days * 86400u + (uint32_t)(hour * 3600 + minute * 60 + second);
    return true;
}

alloy_end_gate_t alloy_end_gate(bool meta_settled, bool meta_delivered, unsigned free_buffers,
                                unsigned total_buffers, int delivery_failure_status)
{
    if (!meta_settled || free_buffers < total_buffers) return ALLOY_END_WAITING;
    if (!meta_delivered || delivery_failure_status != 0) return ALLOY_END_FAILED;
    return ALLOY_END_READY;
}

bool alloy_valid_mesh_path(const char *path)
{
    if (!path) return false;
    size_t len = strlen(path);
    if (len == 0 || len > 128 || path[0] == '/' || path[len - 1] == '/') return false;
    for (size_t i = 0; i < len; i++) {
        char c = path[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '-' || c == '/';
        if (!ok) return false;
    }
    return true;
}

uint32_t alloy_finalize_ms(uint32_t seconds)
{
    if (seconds == 0) return 0;
    if (seconds < 30) seconds = 30;
    if (seconds > 1800) seconds = 1800;
    return seconds * 1000u;
}

void alloy_status_text(int status, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    const char *fixed = NULL;
    if (status == 0 || (status >= 200 && status < 300)) fixed = "";
    else if (status == ALLOY_STATUS_OFFLINE) fixed = "network offline; delivery is retrying";
    else if (status == ALLOY_STATUS_CONFIG) fixed = "missing API key, invalid mesh path or invalid config";
    else if (status == ALLOY_STATUS_MEMORY) fixed = "could not allocate Alloy Storage buffers";
    else if (status == ALLOY_STATUS_TASK) fixed = "could not start the Alloy Storage upload task";
    else if (status == ALLOY_STATUS_DRAIN_TIMEOUT)
        fixed = "metadata or buffer drain timed out; delivery is still retrying";
    else if (status == ALLOY_STATUS_DATA_LOSS) fixed = "a buffered data chunk was dropped before delivery";
    else if (status == ALLOY_STATUS_TRANSPORT)
        fixed = "network or TLS transport error; delivery is retrying";
    else if (status == ALLOY_STATUS_TLS_CONFIG)
        fixed = "no TLS verification configured: enable CONFIG_MBEDTLS_CERTIFICATE_BUNDLE, or opt into insecure";
    if (fixed) {
        snprintf(out, cap, "%s", fixed);
    } else if (status < 0) {
        snprintf(out, cap, "local error (%d)", status);
    } else if (status == 400 || status == 413) {
        snprintf(out, cap, "upload rejected; check mesh path and buffer size (HTTP %d)", status);
    } else if (status == 401 || status == 403) {
        snprintf(out, cap, "Alloy API key rejected (HTTP %d)", status);
    } else if (status == 409) {
        snprintf(out, cap, "session was already finalized (HTTP 409)");
    } else if (status == 429) {
        snprintf(out, cap, "upload rate limited (HTTP 429); delivery is retrying");
    } else if (status >= 500) {
        snprintf(out, cap, "Alloy ingest temporarily unavailable (HTTP %d); delivery is retrying", status);
    } else {
        snprintf(out, cap, "upload failed (HTTP %d)", status);
    }
}
