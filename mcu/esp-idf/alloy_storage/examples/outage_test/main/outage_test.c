// outage_test: store-and-forward through a Wi-Fi outage.
//
// Logs a 20 Hz sequence counter on channel "outage". As soon as the first buffer is queued for
// upload, Wi-Fi is stopped for EXAMPLE_OUTAGE_SECONDS (25 s by default), then restarted. Logging
// never pauses. At the end every counter value must be in Alloy Storage exactly once:
//     SELECT count(*), count(DISTINCT data.seq), min(data.seq), max(data.seq) FROM ...outage
// and alloy_storage_end() must return ESP_OK.

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_timer.h"
#include "esp_wifi.h"

#include "alloy_storage.h"
#include "alloy_storage_wifi.h"

static void report(alloy_storage_handle_t alloy, const char *label, uint32_t seq)
{
    alloy_storage_stats_t s;
    alloy_storage_get_stats(alloy, &s);
    const char *err = alloy_storage_last_error(alloy);
    printf("[%s] seq=%lu delivered=%lu queued=%lu retried=%lu failed=%lu dropped=%lu "
           "dropped_rows=%lu stale=%lu%s%s\n",
           label, (unsigned long)seq, (unsigned long)s.delivered, (unsigned long)s.queued,
           (unsigned long)s.retried, (unsigned long)s.failed, (unsigned long)s.dropped,
           (unsigned long)s.dropped_rows, (unsigned long)s.stale, err[0] ? " error=" : "", err);
}

void app_main(void)
{
    printf("\n=== Alloy Storage outage test (ESP-IDF) ===\n");
    if (alloy_storage_wifi_connect(CONFIG_EXAMPLE_WIFI_SSID, CONFIG_EXAMPLE_WIFI_PASSWORD, 20000) != ESP_OK) {
        printf("FAIL: Wi-Fi did not connect\n");
        return;
    }
    alloy_storage_config_t cfg = ALLOY_STORAGE_DEFAULT_CONFIG();
    cfg.api_key = CONFIG_EXAMPLE_ALLOY_API_KEY;
    cfg.mesh_path = CONFIG_EXAMPLE_ALLOY_MESH_PATH;
    cfg.device_id = "esp32-outage-test";
    cfg.mission = "wifi outage store-and-forward";
    cfg.ingest_url = CONFIG_EXAMPLE_INGEST_URL[0] ? CONFIG_EXAMPLE_INGEST_URL : NULL;
    alloy_storage_handle_t alloy;
    esp_err_t err = alloy_storage_begin(&cfg, &alloy);
    if (err != ESP_OK) {
        printf("FAIL: alloy_storage_begin: %s\n", esp_err_to_name(err));
        return;
    }

    const int64_t run_us = (int64_t)CONFIG_EXAMPLE_RUN_SECONDS * 1000000;
    const int64_t outage_us = (int64_t)CONFIG_EXAMPLE_OUTAGE_SECONDS * 1000000;
    int64_t t0 = esp_timer_get_time(), outage_start = 0, last_report = 0;
    bool outage_done = false;
    uint32_t seq = 0, logged = 0;
    TickType_t wake = xTaskGetTickCount();
    while (esp_timer_get_time() - t0 < run_us || !outage_done) {
        alloy_record_t r;
        alloy_record_begin(&r, alloy, "outage");
        alloy_record_set(&r, "seq", (float)seq);
        if (alloy_record_commit(&r) == ESP_OK) logged++;
        seq++;

        int64_t now = esp_timer_get_time();
        alloy_storage_stats_t s;
        alloy_storage_get_stats(alloy, &s);
        if (!outage_start && s.queued > 0) {
            outage_start = now;
            report(alloy, "outage start", seq);
            esp_wifi_stop();
        } else if (outage_start && !outage_done && now - outage_start >= outage_us) {
            outage_done = true;
            report(alloy, "outage end", seq);
            esp_wifi_start();  // the helper reconnects on WIFI_EVENT_STA_START
        }
        if (now - last_report >= 5000000) {
            last_report = now;
            report(alloy, outage_start && !outage_done ? "offline" : "online", seq);
        }
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(50));
    }

    printf("Logged %lu rows (seq 0..%lu). Ending...\n", (unsigned long)logged, (unsigned long)(seq - 1));
    esp_err_t end = alloy_storage_end(alloy, 90000);
    report(alloy, "end", seq);
    alloy_storage_stats_t s;
    alloy_storage_get_stats(alloy, &s);
    bool pass = end == ESP_OK && logged == seq && !s.failed && !s.dropped && !s.dropped_rows && !s.stale;
    printf("%s: end=%s, run %lu, expect seq 0..%lu exactly once in %s/%lu\n", pass ? "PASS" : "FAIL",
           esp_err_to_name(end), (unsigned long)s.session, (unsigned long)(seq - 1),
           CONFIG_EXAMPLE_ALLOY_MESH_PATH, (unsigned long)s.session);
}
