/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 *
 * Network readiness (conn_mgr L4 events) and the optional Wi-Fi connect helper.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/conn_mgr_monitor.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include <alloy_storage/alloy_storage.h>
#include "alloy_net.h"

LOG_MODULE_DECLARE(alloy_storage, CONFIG_ALLOY_STORAGE_LOG_LEVEL);

#define NET_UP BIT(0)
#define WIFI_FAILED BIT(1)
#define RECONNECT_INTERVAL_MS 10000

static K_EVENT_DEFINE(net_events);
static struct net_mgmt_event_callback l4_cb;
static struct net_mgmt_event_callback wifi_cb;

static K_MUTEX_DEFINE(wifi_lock);
static char wifi_ssid[WIFI_SSID_MAX_LEN + 1];
static char wifi_psk[WIFI_PSK_MAX_LEN + 1];
static bool wifi_have_creds;
static int64_t wifi_last_request;

static void l4_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	ARG_UNUSED(cb);
	ARG_UNUSED(iface);

	if (event == NET_EVENT_L4_CONNECTED) {
		LOG_INF("network connected");
		k_event_post(&net_events, NET_UP);
	} else if (event == NET_EVENT_L4_DISCONNECTED) {
		LOG_INF("network disconnected");
		k_event_clear(&net_events, NET_UP);
	}
}

static void wifi_handler(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
	const struct wifi_status *status = (const struct wifi_status *)cb->info;

	if (event != NET_EVENT_WIFI_CONNECT_RESULT || !status) {
		return;
	}
	if (status->status != 0) {
		LOG_WRN("Wi-Fi connect failed (%d)", status->status);
		k_event_post(&net_events, WIFI_FAILED);
		return;
	}
	LOG_INF("Wi-Fi associated");
	if (IS_ENABLED(CONFIG_NET_DHCPV4)) {
		net_dhcpv4_start(iface);
	}
}

static int alloy_net_init(void)
{
	net_mgmt_init_event_callback(&l4_cb, l4_handler,
				     NET_EVENT_L4_CONNECTED | NET_EVENT_L4_DISCONNECTED);
	net_mgmt_add_event_callback(&l4_cb);
	net_mgmt_init_event_callback(&wifi_cb, wifi_handler, NET_EVENT_WIFI_CONNECT_RESULT);
	net_mgmt_add_event_callback(&wifi_cb);
	conn_mgr_mon_resend_status();
	return 0;
}

SYS_INIT(alloy_net_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

bool alloy_storage_net_ready(void)
{
	return k_event_test(&net_events, NET_UP) != 0;
}

bool alloy_net_wait(k_timeout_t timeout)
{
	return k_event_wait(&net_events, NET_UP, false, timeout) != 0;
}

static int request_connect(void)
{
	struct net_if *iface = net_if_get_first_wifi();
	struct wifi_connect_req_params params = {0};
	size_t psk_len = strlen(wifi_psk);

	if (!iface) {
		return -ENODEV;
	}
	params.ssid = (const uint8_t *)wifi_ssid;
	params.ssid_length = (uint8_t)strlen(wifi_ssid);
	params.psk = psk_len ? (const uint8_t *)wifi_psk : NULL;
	params.psk_length = (uint8_t)psk_len;
	params.security = psk_len ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE;
	params.band = WIFI_FREQ_BAND_UNKNOWN;
	params.channel = WIFI_CHANNEL_ANY;
	params.mfp = WIFI_MFP_OPTIONAL;
	params.timeout = SYS_FOREVER_MS;
	wifi_last_request = k_uptime_get();
	return net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
}

int alloy_storage_wifi_connect(const char *ssid, const char *psk, k_timeout_t timeout)
{
	k_timepoint_t deadline = sys_timepoint_calc(timeout);

	if (!ssid || !ssid[0] || strlen(ssid) > WIFI_SSID_MAX_LEN ||
	    (psk && strlen(psk) > WIFI_PSK_MAX_LEN)) {
		return -EINVAL;
	}
	k_mutex_lock(&wifi_lock, K_FOREVER);
	strcpy(wifi_ssid, ssid);
	strcpy(wifi_psk, psk ? psk : "");
	wifi_have_creds = true;
	k_mutex_unlock(&wifi_lock);

	/* Weak links often fail the first association: retry until the caller's timeout. */
	do {
		uint32_t got;
		int rc = 0;

		if (alloy_storage_net_ready()) {
			return 0;
		}
		k_event_clear(&net_events, WIFI_FAILED);
		k_mutex_lock(&wifi_lock, K_FOREVER);
		rc = request_connect();
		k_mutex_unlock(&wifi_lock);
		if (rc < 0 && rc != -EALREADY && rc != -EINPROGRESS) {
			return rc;
		}
		got = k_event_wait(&net_events, NET_UP | WIFI_FAILED, false,
				   sys_timepoint_timeout(deadline));
		if (got & NET_UP) {
			return 0;
		}
		if (got & WIFI_FAILED) {
			k_sleep(K_MSEC(500));
		}
	} while (!sys_timepoint_expired(deadline));
	return alloy_storage_net_ready() ? 0 : -ETIMEDOUT;
}

void alloy_net_kick(void)
{
	if (!IS_ENABLED(CONFIG_ALLOY_STORAGE_WIFI_RECONNECT) || alloy_storage_net_ready()) {
		return;
	}
	k_mutex_lock(&wifi_lock, K_FOREVER);
	if (wifi_have_creds && k_uptime_get() - wifi_last_request >= RECONNECT_INTERVAL_MS) {
		int rc = request_connect();

		LOG_INF("Wi-Fi reconnect requested (%d)", rc);
	}
	k_mutex_unlock(&wifi_lock);
}
