/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * First mission: the shortest path from a Zephyr Wi-Fi board to a confirmed Alloy mission.
 *
 * Set the Wi-Fi name, password, device key and mesh path (see README), flash, then open the console.
 * The sample records generated telemetry for CONFIG_APP_MISSION_SECONDS, prints upload health as
 * it runs and explicitly finalizes the mission. No sensors are required.
 */

#include <math.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include <alloy_storage/alloy_storage.h>

#define MISSION_SECONDS CONFIG_APP_MISSION_SECONDS

static bool unchanged(const char *value, const char *placeholder)
{
	return !value[0] || strcmp(value, placeholder) == 0;
}

static void stop_with_message(const char *message)
{
	printk("\nSTOP: %s\n", message);
	printk("Fix the setting (see the sample README), then build and flash again.\n");
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

static int wifi_rssi(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_iface_status status = {0};

	if (!iface || net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status))) {
		return 0;
	}
	return status.rssi;
}

static const char *clock_name(enum alloy_clock_source c)
{
	return c == ALLOY_CLOCK_SNTP ? "sntp" : c == ALLOY_CLOCK_HTTP_DATE ? "http-date" : "none";
}

static void print_health(uint32_t samples, uint32_t elapsed_s)
{
	struct alloy_storage_stats s;
	const char *error = alloy_storage_last_error();
	const char *state;

	alloy_storage_get_stats(&s);
	state = !alloy_storage_net_ready() ? "wifi-lost"
		: !s.ready		   ? "syncing-clock"
		: error[0]		   ? "delivery-error"
					   : "uploading";
	printk("[progress] %us / %us | state=%s | samples=%u delivered=%u queued=%u failed=%u "
	       "retried=%u dropped=%u dropped_rows=%u stale=%u clock=%s",
	       elapsed_s, MISSION_SECONDS, state, samples, s.delivered, s.queued, s.failed,
	       s.retried, s.dropped, s.dropped_rows, s.stale, clock_name(s.clock));
	if (error[0]) {
		printk(" last_status=%d error=\"%s\"", s.last_status, error);
	}
	if (alloy_storage_net_ready()) {
		printk(" rssi=%d_dBm", wifi_rssi());
	}
	printk("\n");
}

static void print_recovery_guidance(int end_rc)
{
	struct alloy_storage_stats s;
	const char *error = alloy_storage_last_error();

	alloy_storage_get_stats(&s);
	if (end_rc != 0) {
		printk("- Finalization was not acknowledged (end() = %d). Retryable data stays queued; "
		       "terminal loss shows in the counters below.\n", end_rc);
	}
	if (!alloy_storage_net_ready()) {
		printk("- Wi-Fi was lost. Move closer to the access point or restore the network.\n");
	}
	if (!s.ready) {
		printk("- The clock never synced (neither SNTP nor the ingest Date header).\n");
	}
	if (s.last_status == 400 || s.last_status == 413) {
		printk("- A setting or payload was rejected. Re-check the mesh path.\n");
	} else if (s.last_status == 401 || s.last_status == 403) {
		printk("- Alloy rejected the device key. Create a new device key and rebuild.\n");
	} else if (s.last_status == 409) {
		printk("- This run had already finalized. Reset the board to start a fresh mission.\n");
	} else if (s.last_status == 429 || s.last_status >= 500) {
		printk("- Alloy Storage ingest is temporarily unavailable. Buffers keep retrying.\n");
	} else if (s.last_status < 0 && error[0]) {
		printk("- HTTPS could not complete. Restore Wi-Fi or try another network.\n");
	}
	if (s.dropped > 0 || s.dropped_rows > 0) {
		printk("- Data was dropped before delivery. Retry on stronger Wi-Fi.\n");
	}
}

int main(void)
{
	static const struct alloy_storage_config cfg = {
		.api_key = CONFIG_APP_ALLOY_API_KEY,
		.mesh_path = CONFIG_APP_ALLOY_MESH_PATH,
		.device_id = "zephyr-first-mission",
		.firmware = "first-mission-v1",
		.mission = CONFIG_APP_MISSION_LABEL,
		.finalize_after_s = 30,
	};
	struct alloy_storage_stats s;
	uint32_t samples = 0;
	uint32_t last_progress = 0;
	int64_t started;
	int rc;

	k_sleep(K_SECONDS(1));
	printk("\n=== Alloy Storage first mission (Zephyr, %s) ===\n", CONFIG_BOARD);

	if (unchanged(CONFIG_APP_WIFI_SSID, "YOUR_2_4_GHZ_WIFI_NAME")) {
		stop_with_message("CONFIG_APP_WIFI_SSID is still the placeholder.");
	}
	if (strcmp(CONFIG_APP_WIFI_PSK, "YOUR_WIFI_PASSWORD") == 0) {
		stop_with_message("CONFIG_APP_WIFI_PSK is still the placeholder.");
	}
	if (unchanged(CONFIG_APP_ALLOY_API_KEY, "YOUR_ALLOY_DEVICE_KEY")) {
		stop_with_message("CONFIG_APP_ALLOY_API_KEY is still the placeholder.");
	}

	printk("[1/4] Connecting to 2.4 GHz Wi-Fi: %s\n", CONFIG_APP_WIFI_SSID);
	rc = alloy_storage_wifi_connect(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PSK, K_SECONDS(30));
	if (rc) {
		printk("      wifi_connect() = %d\n", rc);
		stop_with_message("Wi-Fi did not connect in 30 seconds. Check the SSID, password, "
				  "and 2.4 GHz network.");
	}
	printk("      Connected | RSSI %d dBm\n", wifi_rssi());

	printk("[2/4] Starting the Alloy Storage SDK | mesh path: %s\n", cfg.mesh_path);
	alloy_storage_describe("sample", "wave", "", -1, 1, "generated sine wave");
	alloy_storage_describe("sample", "uptime_s", "s", 0, MISSION_SECONDS,
			       "seconds since mission start");
	rc = alloy_storage_begin(&cfg);
	if (rc) {
		printk("      begin() = %d: %s\n", rc, alloy_storage_last_error());
		stop_with_message("The Alloy Storage SDK could not start. ALLOY_MESH_PATH must be "
				  "1-128 characters of letters, numbers, _, - and / (no edge "
				  "slash).");
	}

	printk("[3/4] Recording generated data for %u seconds...\n", MISSION_SECONDS);
	started = k_uptime_get();
	while (k_uptime_get() - started < MISSION_SECONDS * 1000LL) {
		float elapsed = (float)(k_uptime_get() - started) / 1000.0f;
		uint32_t elapsed_s = (uint32_t)((k_uptime_get() - started) / 1000);

		ALLOY_LOG("sample", {"wave", sinf(elapsed * 2.0f)}, {"uptime_s", elapsed});
		samples++;
		if (elapsed_s >= last_progress + 2) {
			last_progress = elapsed_s;
			print_health(samples, elapsed_s);
		}
		k_sleep(K_MSEC(50));
	}

	printk("[4/4] Draining uploads and finalizing the mission...\n");
	rc = alloy_storage_end(K_SECONDS(30));
	print_health(samples, MISSION_SECONDS);

	alloy_storage_get_stats(&s);
	bool healthy = rc == 0 && s.delivered > 0 && s.failed == 0 && s.dropped == 0 &&
		       s.dropped_rows == 0 && s.stale == 0 && !alloy_storage_last_error()[0];

	printk("\nrun: %s/%u device=zephyr-first-mission clock=%s\n", cfg.mesh_path, s.session,
	       clock_name(s.clock));
	if (healthy) {
		printk("PASS: Alloy accepted the data and acknowledged mission finalization.\n");
		printk("Open Alloy > Mesh Storage and browse to %s for the new .mcap mission.\n",
		       cfg.mesh_path);
	} else {
		printk("FAIL: the first mission was not fully confirmed from this board.\n");
		print_recovery_guidance(rc);
	}
	return 0;
}
