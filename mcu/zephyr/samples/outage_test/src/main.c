/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * Outage test: logs a sequence counter at CONFIG_APP_RATE_HZ, drops Wi-Fi for
 * CONFIG_APP_OUTAGE_SECONDS the moment the first chunk is queued for upload, reconnects, then
 * ends the run. Pass means every counter value reaches Alloy Storage exactly once: the buffers
 * held the outage in RAM and the retries were deduplicated by sequence number.
 *
 * Check it in Storage afterwards, for example:
 *   SELECT count(*), count(DISTINCT data.seq), min(data.seq), max(data.seq)
 *   FROM alloy.fleet.seq WHERE _source_file LIKE '%<session>%'
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>

#include <alloy_storage/alloy_storage.h>

#define PERIOD_MS (1000 / CONFIG_APP_RATE_HZ)

static void print_stats(const char *tag, uint32_t seq)
{
	struct alloy_storage_stats s;

	alloy_storage_get_stats(&s);
	printk("[%s] t=%llds seq=%u net=%s delivered=%u queued=%u retried=%u failed=%u "
	       "dropped=%u dropped_rows=%u stale=%u last_status=%d\n",
	       tag, k_uptime_get() / 1000, seq, alloy_storage_net_ready() ? "up" : "down",
	       s.delivered, s.queued, s.retried, s.failed, s.dropped, s.dropped_rows, s.stale,
	       s.last_status);
}

static int wifi_disconnect(void)
{
	struct net_if *iface = net_if_get_first_wifi();

	return iface ? net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0) : -ENODEV;
}

int main(void)
{
	static const struct alloy_storage_config cfg = {
		.api_key = CONFIG_APP_ALLOY_API_KEY,
		.mesh_path = CONFIG_APP_ALLOY_MESH_PATH,
		.device_id = "zephyr-outage-test",
		.firmware = "outage-test-v1",
		.mission = "zephyr 25 s wifi outage",
		.finalize_after_s = 60,
	};
	struct alloy_storage_stats s;
	int64_t started, next, outage_at = -1, last_print = 0;
	bool outage_done = false;
	uint32_t seq = 0;
	int rc;

	k_sleep(K_SECONDS(1));
	printk("\n=== Alloy Storage outage test (Zephyr, %s) ===\n", CONFIG_BOARD);

	rc = alloy_storage_wifi_connect(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PSK, K_SECONDS(30));
	if (rc) {
		printk("FAIL: Wi-Fi did not connect (%d)\n", rc);
		return 0;
	}
	rc = alloy_storage_begin(&cfg);
	if (rc) {
		printk("FAIL: begin() = %d: %s\n", rc, alloy_storage_last_error());
		return 0;
	}

	started = k_uptime_get();
	next = started;
	while (k_uptime_get() - started < CONFIG_APP_RUN_SECONDS * 1000LL) {
		ALLOY_LOG("seq", {"seq", (float)seq}, {"rate_hz", CONFIG_APP_RATE_HZ});
		seq++;

		alloy_storage_get_stats(&s);
		if (outage_at < 0 && s.queued > 0) {
			outage_at = k_uptime_get();
			rc = wifi_disconnect();
			printk("[outage] chunk queued at seq=%u: Wi-Fi disconnect (%d) for %d s\n",
			       seq, rc, CONFIG_APP_OUTAGE_SECONDS);
		}
		if (outage_at >= 0 && !outage_done &&
		    k_uptime_get() - outage_at >= CONFIG_APP_OUTAGE_SECONDS * 1000LL) {
			outage_done = true;
			rc = alloy_storage_wifi_connect(CONFIG_APP_WIFI_SSID, CONFIG_APP_WIFI_PSK,
							K_NO_WAIT);
			printk("[outage] over at seq=%u: reconnect requested (%d)\n", seq, rc);
		}
		if (k_uptime_get() - last_print >= 2000) {
			last_print = k_uptime_get();
			print_stats("progress", seq);
		}
		next += PERIOD_MS;
		k_sleep(K_TIMEOUT_ABS_MS(next));
	}

	printk("[end] logged seq 0..%u (%u values); draining...\n", seq - 1, seq);
	rc = alloy_storage_end(K_SECONDS(90));
	print_stats("end", seq);
	alloy_storage_get_stats(&s);
	printk("run: %s/%u device=zephyr-outage-test values=%u outage=%s\n", cfg.mesh_path,
	       s.session, seq, outage_done ? "yes" : "no");
	if (rc == 0 && outage_done && s.dropped == 0 && s.dropped_rows == 0 && s.failed == 0 &&
	    s.stale == 0) {
		printk("PASS: end() accepted after a %d s outage, nothing dropped.\n",
		       CONFIG_APP_OUTAGE_SECONDS);
	} else {
		printk("FAIL: end() = %d: %s\n", rc, alloy_storage_last_error());
	}
	return 0;
}
