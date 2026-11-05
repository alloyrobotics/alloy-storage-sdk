/*
 * Copyright (c) 2026 Alloy Robotics
 * SPDX-License-Identifier: MIT
 */

#ifndef ALLOY_STORAGE_NET_H_
#define ALLOY_STORAGE_NET_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

/* Waits until the network is up. Returns true if it is. */
bool alloy_net_wait(k_timeout_t timeout);

/* Re-requests the Wi-Fi connection given to alloy_storage_wifi_connect(), at most every 10 s,
 * when CONFIG_ALLOY_STORAGE_WIFI_RECONNECT is set and the network is down.
 */
void alloy_net_kick(void);

#endif /* ALLOY_STORAGE_NET_H_ */
