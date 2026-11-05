// alloy_storage_wifi.h: optional quick-start helper that brings up a Wi-Fi station.
//
// Apps that already manage Wi-Fi (or Ethernet) don't need this: alloy_storage only needs some
// network interface with an IPv4 address, and waits for one when it goes away.

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initializes NVS (if not yet), the default event loop, esp_netif and a station interface,
// connects to ssid and reconnects on its own whenever the connection drops. Blocks up to
// timeout_ms for an IP address; returns ESP_ERR_TIMEOUT if none arrived (it keeps trying).
esp_err_t alloy_storage_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

// RSSI of the current access point in dBm, or 0 when not connected.
int alloy_storage_wifi_rssi(void);

#ifdef __cplusplus
}
#endif
