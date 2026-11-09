# Alloy Storage SDK for ESP-IDF

Stream robot telemetry from an ESP32 straight into [Alloy Storage](https://usealloy.ai). You log
named fields on a channel; the component buffers them in RAM and uploads them from a background
task. Every power-on becomes one indexed MCAP in your mesh that you can replay, inspect, query with
SQL and analyze over MCP.

A native ESP-IDF component: `esp_http_client` with the certificate bundle, `esp_netif`, SNTP and
FreeRTOS. No Arduino. It speaks the same protocol as the Arduino package
([`spec/esp32-http-v1.md`](../../../spec/esp32-http-v1.md)), so a run looks the same in Alloy
Storage whichever one recorded it.

- C API, usable from C and C++, `esp_err_t` returns, config struct, Kconfig defaults.
- Never blocks your control loop: logging is a `memcpy` into a RAM buffer under a mutex.
- Store-and-forward through Wi-Fi outages: buffers wait in RAM and upload when the network is back.
- `alloy_storage_end()` returns `ESP_OK` only when every buffer was delivered and the service
  accepted finalization.
- Verified TLS by default.

Targets: ESP32, ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6. ESP-IDF 5.3 or later.

## Install

From the [ESP Component Registry](https://components.espressif.com/components/alloyrobotics/alloy_storage):

```sh
idf.py add-dependency "alloyrobotics/alloy_storage^0.1.0"
```

To track unreleased changes, depend on it from git in your `main/idf_component.yml` instead:

```yaml
dependencies:
  alloyrobotics/alloy_storage:
    git: https://github.com/alloyrobotics/alloy-storage-sdk.git
    path: mcu/esp-idf/alloy_storage
```

or from a local checkout with `override_path: /path/to/alloy-storage-sdk/mcu/esp-idf/alloy_storage`.

## Quick start

```c
#include "alloy_storage.h"
#include "alloy_storage_wifi.h"

void app_main(void)
{
    // Skip this if your app already brings up Wi-Fi or Ethernet.
    ESP_ERROR_CHECK(alloy_storage_wifi_connect("my-2g4-network", "password", 20000));

    alloy_storage_config_t cfg = ALLOY_STORAGE_DEFAULT_CONFIG();
    cfg.api_key = CONFIG_MY_ALLOY_API_KEY;      // keep the key out of version control
    cfg.mesh_path = "robots/sbr";
    cfg.mission = "driveway test";
    alloy_storage_handle_t alloy;
    ESP_ERROR_CHECK(alloy_storage_begin(&cfg, &alloy));

    for (int i = 0; i < 200; i++) {
        alloy_record_t r;
        alloy_record_begin(&r, alloy, "imu");
        alloy_record_set(&r, "pitch", read_pitch());
        alloy_record_set(&r, "roll", read_roll());
        alloy_record_commit(&r);

        alloy_storage_log(alloy, "battery", read_volts());   // one field, named "value"
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // Optional. Delivers everything and finalizes the MCAP now instead of after 2 minutes idle.
    if (alloy_storage_end(alloy, 15000) == ESP_OK) printf("mission saved\n");
}
```

The MCAP lands at `<mesh path>/<session>/<device>_<session>.mcap`, where the session is the epoch
second the clock was set. Each channel is one table (`alloy.fleet.<channel>`) with one column per
field.

The [`first_mission`](examples/first_mission) example is the full version of this, printing each
stage and a PASS or FAIL. [`outage_test`](examples/outage_test) stops Wi-Fi for 25 s mid-run and
checks that nothing was lost. Both read Wi-Fi, the device key and the mesh path from menuconfig:

```sh
cd examples/first_mission
idf.py set-target esp32c3 menuconfig flash monitor
```

## API

All of it is in [`alloy_storage.h`](include/alloy_storage.h).

| Call | What it does |
|---|---|
| `alloy_storage_begin(&cfg, &handle)` | Validates the config, allocates the buffer pool, starts SNTP and the upload task. One instance per app. |
| `alloy_record_begin(&r, handle, channel)` | Starts a row on `channel`, stamped now. |
| `alloy_record_set(&r, name, value)` | Adds a `float` field. NaN and infinity are stored as null. |
| `alloy_record_commit(&r)` | Buffers the row. `ESP_ERR_NO_MEM` means it was dropped. |
| `alloy_storage_log(handle, channel, value)` | One-field row (field `value`). |
| `alloy_storage_log_fields(handle, channel, names, values, n)` | Several fields from two arrays. |
| `alloy_storage_end(handle, timeout_ms)` | Seals every buffer, waits for delivery, then finalizes. `ESP_OK`, `ESP_ERR_TIMEOUT` (still delivering, call again to retry) or `ESP_FAIL` (something was lost or rejected). |
| `alloy_storage_get_stats(handle, &stats)` | Counters and state, below. |
| `alloy_storage_last_error(handle)` | Printable explanation of the last status, `""` after success. |
| `alloy_storage_wifi_connect(ssid, pass, timeout_ms)` | Optional helper: station mode with automatic reconnect. |

Logging works from any task, not from an ISR. A record is about 620 bytes; keep it on the stack of
a task with room for it.

### Config

| Field | Default | |
|---|---|---|
| `api_key` | required | This robot's Alloy device key (never the org API key; see [Device key](#device-key)). |
| `mesh_path` | required | 1 to 128 of `A-Z a-z 0-9 _ - /`, no leading or trailing `/`. Checked by `begin()`. |
| `device_id` | project name | Sanitized to `A-Z a-z 0-9 _ -`, at most 23 characters. |
| `firmware` | app version | Stored in the MCAP metadata. |
| `mission` | none | Human-readable label for the run, stored in the MCAP metadata (`mcap.alloy.mission`). |
| `fields`, `field_count` | none | Units, ranges and descriptions per field (`alloy_field_desc_t`). |
| `finalize_after_s` | 120 (service) | Idle time after which the service finalizes the run, 30 to 1800. |
| `ingest_url` | `CONFIG_ALLOY_INGEST_URL` | |
| `sntp_server` | `CONFIG_ALLOY_SNTP_SERVER` | `""` leaves SNTP to your app. |
| `buffer_count`, `buffer_bytes` | Kconfig | Buffer pool. |
| `flush_interval_ms` | Kconfig | |
| `insecure` | false | Skip TLS verification (below). |

### Stats

`delivered` (chunks accepted), `failed` (rejected terminally), `retried` (transient attempts
retried), `dropped` (sealed buffers shed because the pool was full), `dropped_rows` (rows that never
made it into a buffer), `stale` (chunks refused because the run had already finalized), `queued`
(sealed or in flight now), `ready` (clock set, run started), `session`, `clock`, `last_status` (the
HTTP status, or a negative `ALLOY_STATUS_*` code from `alloy/policy.h`) and `last_transport_err`.

## Kconfig

`idf.py menuconfig`, then Component config, Alloy Storage.

| Option | Default | |
|---|---|---|
| `ALLOY_INGEST_URL` | `https://ingest.alloylogger.com` | |
| `ALLOY_POOL_SIZE` | 4 | RAM buffers. Keep it above the number of channels you log at once. |
| `ALLOY_BUFFER_BYTES` | 12288 | Bytes per buffer; one sealed buffer is one upload. |
| `ALLOY_FLUSH_INTERVAL_MS` | 4000 | A part-full buffer is uploaded this long after it opened. |
| `ALLOY_SNTP_SERVER` | `pool.ntp.org` | |
| `ALLOY_HTTP_TIMEOUT_MS` | 10000 | |
| `ALLOY_TASK_STACK_SIZE` | 8192 | The upload task runs the TLS handshake. |
| `ALLOY_TASK_PRIORITY` | 4 | |
| `ALLOY_TASK_CORE_ID` | -1 (any) | |

4 x 12 KB leaves room for a verified TLS handshake (about 40 KB of heap) on every chip. Raise the
pool only with heap to spare.

## How delivery works

- **Buffers.** Each channel fills its own buffer. A buffer is uploaded when it is 90% full, when the
  channel's set of fields changes, or `ALLOY_FLUSH_INTERVAL_MS` after it opened. The timed upload
  happens only while nothing else is waiting to upload, so during an outage buffers keep filling
  instead of being sealed part-empty. When the pool is full, the oldest waiting buffer is dropped
  and counted.
- **Retries.** Network errors and HTTP 408, 425, 429 and 5xx retry after 1, 2, 4, 8, 16, then every
  30 s, holding the buffer. Each chunk keeps its sequence number across retries, and the service
  ignores duplicates.
- **Clock.** Rows need UTC. `begin()` starts SNTP. If the clock is still unset after 5 s, the upload
  task takes it from the ingest service's HTTP `Date` header. Rows logged before that are rebased to
  UTC before upload. lwIP delays its first SNTP request by a random 0 to 5 s by default
  (`CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY`); the examples set 1 s so SNTP usually wins.
- **Network.** Delivery needs any interface with an IPv4 address, Wi-Fi or not. While there is
  none it waits and retries; reconnecting is your app's job (the Wi-Fi helper does it for you).
- **Single-core chips** (C3, C6, S2): each TLS handshake stalls lower-priority tasks for about 1.5 s.
  The connection is kept alive between uploads, so that happens once per connection, not per chunk.

## TLS

Server certificates are verified against ESP-IDF's certificate bundle
(`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`, on by default). Without the bundle, `begin()` returns
`ESP_ERR_NOT_SUPPORTED`. For networks where verification can't work (TLS-intercepting proxies), set
`cfg.insecure = true` and enable `CONFIG_ESP_TLS_INSECURE` and
`CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY`. Both steps are required, so it can't happen by accident.

The `Date` clock fallback relies on ESP-IDF's default of not checking certificate dates
(`CONFIG_MBEDTLS_HAVE_TIME_DATE` off). If you turn that on, set the clock before `begin()`.

## Device key

Give each robot its own **device key**. With a coding agent connected to the
[Alloy MCP](https://docs.usealloy.ai/docs/integrations/coding-agents), ask it to "create a device
key for this robot": it calls `create_dev_device_key` and saves the key from a download link that
works for five minutes. The robot then shows under **Mesh Storage → Devices**.

**Never flash the org API key** from **Mesh Storage → API keys**. It is shared by your whole org
and can read and query all of its data, so one dumped board exposes everything, and revoking it cuts
off every robot. Reject a lost robot's device in **Mesh Storage → Devices** (or ask your agent to)
and its key stops working within about 30 seconds.

Today a device key can still read your org's data, not only upload to its own path. Keep it out of
version control (a gitignored `sdkconfig` file, or NVS).

## Not yet

- `scope()` (automatic GPIO capture) and `watch()` (timer-sampled signals) from the Arduino package.
- `alloy_storage_delete()`: one run per boot, like the Arduino package.

## Tests

The protocol-independent logic (CSV encoding, buffer pool, retry policy, end gate, `Date` parser,
metadata JSON) lives in [`portable/`](portable) with no ESP-IDF dependency, so it can move into the
shared `core/`. Its host tests run under ASan and UBSan:

```sh
make -C mcu/esp-idf/test/host test
```

MIT licensed.
