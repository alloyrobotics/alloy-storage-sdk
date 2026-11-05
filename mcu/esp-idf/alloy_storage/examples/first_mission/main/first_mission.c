// first_mission: the shortest path from a fresh ESP32 to a confirmed Alloy mission.
//
// Set Wi-Fi, the API key and the mesh path in menuconfig ("First mission"), flash, and open the
// serial monitor. The app records generated telemetry for 20 seconds, prints upload health as it
// runs and finalizes the mission explicitly. No sensors are required.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"

#include "alloy_storage.h"
#include "alloy/policy.h"
#include "alloy_storage_wifi.h"

#define MISSION_SECONDS 20

static alloy_storage_handle_t alloy;

static void stop(const char *message)
{
    printf("\nSTOP: %s\nFix the setting in menuconfig (First mission), then build and flash again.\n",
           message);
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

static bool placeholder(const char *value, const char *ph) { return !value[0] || !strcmp(value, ph); }

static const char *clock_name(alloy_clock_source_t c)
{
    switch (c) {
    case ALLOY_CLOCK_SYSTEM: return "system";
    case ALLOY_CLOCK_SNTP: return "SNTP";
    case ALLOY_CLOCK_HTTP_DATE: return "HTTP Date fallback";
    default: return "not set";
    }
}

static void print_health(uint32_t samples, uint32_t elapsed_s)
{
    alloy_storage_stats_t s;
    alloy_storage_get_stats(alloy, &s);
    const char *error = alloy_storage_last_error(alloy);
    int rssi = alloy_storage_wifi_rssi();
    const char *state = !rssi ? "wifi-lost" : !s.ready ? "syncing-clock" : error[0] ? "delivery-error" : "uploading";
    printf("[progress] %lus / %us | state=%s | samples=%lu delivered=%lu queued=%lu failed=%lu "
           "retried=%lu dropped=%lu dropped_rows=%lu stale=%lu",
           (unsigned long)elapsed_s, MISSION_SECONDS, state, (unsigned long)samples,
           (unsigned long)s.delivered, (unsigned long)s.queued, (unsigned long)s.failed,
           (unsigned long)s.retried, (unsigned long)s.dropped, (unsigned long)s.dropped_rows,
           (unsigned long)s.stale);
    if (error[0]) printf(" last_status=%d error=\"%s\"", s.last_status, error);
    if (rssi) printf(" rssi=%d_dBm", rssi);
    printf("\n");
}

static void recovery_guidance(esp_err_t end)
{
    alloy_storage_stats_t s;
    alloy_storage_get_stats(alloy, &s);
    if (end != ESP_OK)
        printf("- Finalization was not acknowledged (%s). Retryable data stays queued; terminal loss "
               "shows in the counters above.\n", esp_err_to_name(end));
    if (!alloy_storage_wifi_rssi()) printf("- Wi-Fi was lost. Move closer to the access point.\n");
    if (!s.ready) printf("- The clock never got set: neither SNTP nor the ingest service answered.\n");
    int st = s.last_status;
    if (st == 400 || st == 413) printf("- A setting or payload was rejected. Re-check the mesh path.\n");
    else if (st == 401 || st == 403) printf("- The Alloy data API key was rejected. Copy a fresh key.\n");
    else if (st == 409) printf("- This run had already finalized. Reset to start a fresh mission.\n");
    else if (st == 429 || st >= 500) printf("- Alloy Storage ingest is temporarily unavailable.\n");
    else if (st < 0 && alloy_storage_last_error(alloy)[0])
        printf("- HTTPS could not complete. Restore Wi-Fi or try another network.\n");
    if (s.dropped || s.dropped_rows) printf("- Data was dropped before delivery.\n");
}

void app_main(void)
{
    printf("\n=== Alloy Storage first mission (ESP-IDF) ===\n");
    if (placeholder(CONFIG_EXAMPLE_WIFI_SSID, "YOUR_2_4_GHZ_WIFI_NAME")) stop("Wi-Fi SSID is still the placeholder.");
    if (placeholder(CONFIG_EXAMPLE_WIFI_PASSWORD, "YOUR_WIFI_PASSWORD")) stop("Wi-Fi password is still the placeholder.");
    if (placeholder(CONFIG_EXAMPLE_ALLOY_API_KEY, "YOUR_ALLOY_DATA_API_KEY")) stop("Alloy API key is still the placeholder.");
    if (!alloy_valid_mesh_path(CONFIG_EXAMPLE_ALLOY_MESH_PATH))
        stop("Mesh path must be 1-128 characters of letters, numbers, _, - and / (no edge slash).");

    printf("[1/4] Connecting to 2.4 GHz Wi-Fi: %s\n", CONFIG_EXAMPLE_WIFI_SSID);
    if (alloy_storage_wifi_connect(CONFIG_EXAMPLE_WIFI_SSID, CONFIG_EXAMPLE_WIFI_PASSWORD, 20000) != ESP_OK)
        stop("Wi-Fi did not connect in 20 seconds. Check the SSID, password and 2.4 GHz network.");
    printf("      Connected | RSSI %d dBm\n", alloy_storage_wifi_rssi());

    printf("[2/4] Starting the Alloy Storage SDK | mesh path: %s\n", CONFIG_EXAMPLE_ALLOY_MESH_PATH);
    static const alloy_field_desc_t fields[] = {
        {.channel = "sample", .name = "wave", .min = -1, .max = 1, .has_range = true, .about = "generated sine wave"},
        {.channel = "sample", .name = "uptime_s", .unit = "s", .min = 0, .max = MISSION_SECONDS,
         .has_range = true, .about = "seconds since mission start"},
        {.channel = "run", .name = "clock_source", .about = "1 system, 2 SNTP, 3 HTTP Date fallback"},
    };
    alloy_storage_config_t cfg = ALLOY_STORAGE_DEFAULT_CONFIG();
    cfg.api_key = CONFIG_EXAMPLE_ALLOY_API_KEY;
    cfg.mesh_path = CONFIG_EXAMPLE_ALLOY_MESH_PATH;
    cfg.device_id = "esp32-first-mission";
    cfg.firmware = "first-mission-v1";
    cfg.mission = CONFIG_EXAMPLE_MISSION;
    cfg.sntp_server = CONFIG_EXAMPLE_SNTP_SERVER;
    cfg.ingest_url = CONFIG_EXAMPLE_INGEST_URL[0] ? CONFIG_EXAMPLE_INGEST_URL : NULL;
    cfg.finalize_after_s = 30;
    cfg.fields = fields;
    cfg.field_count = sizeof(fields) / sizeof(fields[0]);
    esp_err_t err = alloy_storage_begin(&cfg, &alloy);
    if (err != ESP_OK) {
        printf("      alloy_storage_begin: %s\n", esp_err_to_name(err));
        stop("The Alloy Storage SDK could not start.");
    }

    printf("[3/4] Recording generated data for %d seconds...\n", MISSION_SECONDS);
    int64_t started = esp_timer_get_time();
    uint32_t samples = 0, last_progress = 0;
    bool clock_logged = false;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        float elapsed = (float)(esp_timer_get_time() - started) / 1e6f;
        if (elapsed >= MISSION_SECONDS) break;
        alloy_record_t r;
        alloy_record_begin(&r, alloy, "sample");
        alloy_record_set(&r, "wave", sinf(elapsed * 2.0f));
        alloy_record_set(&r, "uptime_s", elapsed);
        alloy_record_commit(&r);
        samples++;

        alloy_storage_stats_t s;
        alloy_storage_get_stats(alloy, &s);
        if (s.ready && !clock_logged) {
            clock_logged = true;
            printf("      Clock set from %s | run %lu\n", clock_name(s.clock), (unsigned long)s.session);
            alloy_record_t run;
            alloy_record_begin(&run, alloy, "run");
            alloy_record_set(&run, "clock_source", (float)s.clock);
            alloy_record_commit(&run);
        }
        uint32_t elapsed_s = (uint32_t)elapsed;
        if (elapsed_s >= last_progress + 2) {
            last_progress = elapsed_s;
            print_health(samples, elapsed_s);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(50));
    }

    printf("[4/4] Draining uploads and finalizing the mission...\n");
    esp_err_t end = alloy_storage_end(alloy, 15000);
    print_health(samples, MISSION_SECONDS);

    alloy_storage_stats_t s;
    alloy_storage_get_stats(alloy, &s);
    bool healthy = end == ESP_OK && s.delivered > 0 && !s.failed && !s.dropped && !s.dropped_rows &&
                   !s.stale && !alloy_storage_last_error(alloy)[0];
    if (healthy) {
        printf("\nPASS: Alloy accepted the data and acknowledged mission finalization.\n");
        printf("Open Alloy > Mesh Storage and browse to %s/%lu for the new .mcap mission.\n",
               CONFIG_EXAMPLE_ALLOY_MESH_PATH, (unsigned long)s.session);
    } else {
        printf("\nFAIL: the first mission was not fully confirmed from this ESP32.\n");
        recovery_guidance(end);
    }
}
