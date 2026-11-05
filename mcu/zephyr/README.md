# Alloy Storage SDK for Zephyr

A native Zephyr module that streams robot telemetry into [Alloy Storage](https://usealloy.ai). You
log named fields on a channel from any thread. A background thread uploads them, and each power-on
becomes one indexed MCAP in your mesh that you can replay, inspect, query with SQL and analyze over
MCP.

It uses only Zephyr APIs (`net_mgmt`/`wifi_mgmt`, conn_mgr, BSD sockets with TLS, SNTP, kernel
threads, `k_mutex`, `k_event`), so it runs on any Zephyr board with a network interface. It speaks
the same wire protocol as the [Arduino package](../arduino), and the ingest service treats the two
identically.

| | |
|---|---|
| Version | 0.1.0 |
| Zephyr | v4.5.0 |
| Verified on hardware | ESP32-C3 (`esp32c3_supermini`) |
| Also builds for | nRF7002 DK (`nrf7002dk/nrf5340/cpuapp`) |

## Add it to your project

As a west project, in your `west.yml`:

```yaml
manifest:
  remotes:
    - name: alloyrobotics
      url-base: https://github.com/alloyrobotics
  projects:
    - name: alloy-storage-sdk
      remote: alloyrobotics
      revision: main
      path: modules/lib/alloy-storage-sdk
```

then point Zephyr at the module folder inside it (the repo holds every SDK package):

```cmake
# CMakeLists.txt, before find_package(Zephyr)
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../modules/lib/alloy-storage-sdk/mcu/zephyr)
```

Or, without west, clone the repo anywhere and pass it on the command line:

```bash
west build -b esp32c3_supermini app -- -DZEPHYR_EXTRA_MODULES=/path/to/alloy-storage-sdk/mcu/zephyr
```

## Quick start

`prj.conf`:

```
CONFIG_ALLOY_STORAGE=y
CONFIG_NETWORKING=y
CONFIG_NET_IPV4=y
CONFIG_NET_TCP=y
CONFIG_NET_UDP=y
CONFIG_NET_DHCPV4=y
CONFIG_WIFI=y
CONFIG_NET_CONTEXT_RCVTIMEO=y
CONFIG_NET_CONTEXT_SNDTIMEO=y
CONFIG_ENTROPY_GENERATOR=y
CONFIG_MBEDTLS_ENABLE_HEAP=y
CONFIG_MBEDTLS_HEAP_SIZE=56000
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
CONFIG_MAIN_STACK_SIZE=4096
```

`CONFIG_ALLOY_STORAGE` selects the rest (sockets, TLS, SNTP, conn_mgr, the mbedTLS ciphersuites and
PSA algorithms for the built-in CA bundle). The samples' `prj.conf` is a working starting point.

```c
#include <alloy_storage/alloy_storage.h>

static const struct alloy_storage_config cfg = {
	.api_key = ALLOY_DEVICE_KEY,       /* this robot's device key; keep it out of git */
	.mesh_path = "robots/rover",
	.mission = "driveway test",
};

int main(void)
{
	alloy_storage_wifi_connect(WIFI_SSID, WIFI_PSK, K_SECONDS(30)); /* optional */
	alloy_storage_describe("imu", "pitch", "rad", -1.6f, 1.6f, "body pitch");
	alloy_storage_begin(&cfg);

	for (int i = 0; i < 400; i++) {
		ALLOY_LOG("imu", {"pitch", read_pitch()}, {"roll", read_roll()});
		alloy_storage_log("battery", read_volts());
		k_sleep(K_MSEC(50));
	}
	return alloy_storage_end(K_SECONDS(15)); /* 0: every buffer delivered, run finalized */
}
```

Each channel uploads as its own CSV chunk: a header (`t_ns` plus your field names) and bare value
rows. One channel with one field set becomes one queryable table, `alloy.fleet.<channel>`.

## API

All functions return `0` or a negative errno unless noted. Logging works from any thread, not from
ISRs.

| Function | |
|---|---|
| `alloy_storage_begin(&cfg)` | Validates the config and starts the uploader. Does not need the network yet. `-EINVAL` for a missing key, a bad mesh path or a bad URL; `-EALREADY` if started. |
| `ALLOY_LOG(chan, {"name", v}, ...)` | Logs one row. `-ENOSPC` if the row was dropped (counted), `-EPERM` before begin or after end. |
| `alloy_storage_log(chan, v)` | One value as field `value`. |
| `alloy_storage_log_fields(chan, fields, n)` | Same as `ALLOY_LOG` with an array. |
| `alloy_storage_record()` / `alloy_record_set()` / `alloy_storage_commit()` | Builds a row field by field (for loops or conditional fields). |
| `alloy_storage_describe(chan, field, unit, lo, hi, about)` | Units, range and meaning for Alloy AI. Before begin. Pass `NAN` for an unknown bound. |
| `alloy_storage_end(timeout)` | Stops logging, seals open buffers, waits for metadata and every buffer, then asks the service to finalize the MCAP now. `0` only when everything was delivered and finalization was accepted; `-EINPROGRESS` on timeout (data keeps retrying); `-EIO` if something was lost or rejected. |
| `alloy_storage_get_stats(&s)` | `delivered`, `failed`, `retried`, `dropped`, `dropped_rows`, `stale`, `queued`, `ready`, `clock`, `session`, `last_status`. |
| `alloy_storage_last_error()` | Printable explanation of `last_status`, `""` after a success. |
| `alloy_storage_now_ns()` | The run clock: UTC ns once set, boot ns before. |
| `alloy_storage_wifi_connect(ssid, psk, timeout)` | Optional helper: connects the first Wi-Fi interface, retries failed associations until the timeout, waits for an IPv4 address. |
| `alloy_storage_net_ready()` | True while conn_mgr reports L4 connectivity. |

`struct alloy_storage_config`: `api_key` (this robot's device key, see [Device key](#device-key)) and `mesh_path` are required. `mesh_path` must match
`^[A-Za-z0-9_/-]{1,128}$` with no leading or trailing slash. `device_id` (default: the board name
plus three bytes of the hardware id), `firmware` and `mission` end up in the MCAP metadata
(`mcap.alloy.device`, `mcap.alloy.firmware`, `mcap.alloy.mission`). `finalize_after_s` sets how long
the service waits after the last data before finalizing on its own (default about 2 minutes).
`insecure` skips certificate verification, for TLS-intercepting proxies only.

## Kconfig

| Option | Default | |
|---|---|---|
| `CONFIG_ALLOY_STORAGE_INGEST_URL` | `https://ingest.alloylogger.com` | Ingest service; `cfg.ingest_url` overrides it. |
| `CONFIG_ALLOY_STORAGE_BUFFER_COUNT` | 4 | RAM buffers. Keep it above the number of channels you log at once. |
| `CONFIG_ALLOY_STORAGE_BUFFER_SIZE` | 12288 | Bytes per buffer (one chunk upload). |
| `CONFIG_ALLOY_STORAGE_FLUSH_INTERVAL_MS` | 4000 | Seal a part-full buffer after this long, unless an earlier chunk is still queued. |
| `CONFIG_ALLOY_STORAGE_MAX_CHANNELS` | 8 | Distinct channels. |
| `CONFIG_ALLOY_STORAGE_MAX_DESCRIBE` | 16 | `describe()` entries. |
| `CONFIG_ALLOY_STORAGE_THREAD_STACK_SIZE` | 8192 | Uploader stack (DNS, SNTP and the TLS handshake run on it). |
| `CONFIG_ALLOY_STORAGE_THREAD_PRIORITY` | 10 | Preemptible; keep it below your control loops. |
| `CONFIG_ALLOY_STORAGE_HTTP_TIMEOUT_MS` | 15000 | Socket send and receive timeout. |
| `CONFIG_ALLOY_STORAGE_SNTP_SERVER` | `pool.ntp.org` | |
| `CONFIG_ALLOY_STORAGE_SNTP_GRACE_MS` | 5000 | SNTP wait before the HTTP `Date` fallback. |
| `CONFIG_ALLOY_STORAGE_TLS_SEC_TAG` | 1095521281 | Security tag for the built-in CA bundle. |
| `CONFIG_ALLOY_STORAGE_WIFI_RECONNECT` | y | Uploader re-requests the Wi-Fi connection (at most every 10 s) while the network is down. |
| `CONFIG_ALLOY_STORAGE_DEVICE_ID` | `""` | Default device id. |
| `CONFIG_ALLOY_STORAGE_LOG_LEVEL_*` | INF | Standard Zephyr log level for module `alloy_storage`. |

Memory is static: `BUFFER_COUNT x BUFFER_SIZE` (48 KB by default) plus the uploader stack. Add the
mbedTLS heap (about 56 KB for a verified handshake with 16 KB records).

## Device key

Give each robot its own **device key**. With a coding agent connected to the
[Alloy MCP](https://docs.usealloy.ai/docs/integrations/coding-agents), ask it to "create a device
key for this robot": it calls `create_dev_device_key` and saves the key from a download link that
works for five minutes. The robot then shows under **Mesh Storage → Devices**.

**Never flash the org API key** from **Mesh Storage → API keys**. It is shared by your whole org
and can read and query all of its data, so one dumped board exposes everything, and revoking it cuts
off every robot. Reject a lost robot's device in **Mesh Storage → Devices** (or ask your agent to)
and its key stops working within about 30 seconds.

Today a device key can still read your org's data, not only upload to its own path. MCU flash can be
dumped, so keep the key in a git-ignored conf file and reject the device if a board is lost.

## How delivery works

- **Store and forward.** Rows go into a static pool of RAM buffers, one active buffer per channel. A
  buffer is sealed when it is 90% full, when its field set changes, or after the flush interval.
  While a sealed chunk is still waiting to upload, the interval flush pauses and the active buffer
  keeps filling, so an outage spends RAM rather than buffers. Only when no buffer is free is the
  oldest queued chunk shed (counted in `dropped`, and `end()` then fails).
- **Retries.** Transport errors, 408, 425, 429 and 5xx are retried with backoff (1 s doubling to
  30 s). Other 4xx are terminal (`failed`); 409 means the run already finalized (`stale`). Each
  chunk keeps its sequence number across retries, so the service drops duplicates.
- **Clock.** SNTP first. If it has not answered after `SNTP_GRACE_MS`, the clock comes from the
  `Date` header of `GET /v1/health` on the same TLS connection (certificate dates are not checked,
  so this works before the clock is set). Rows logged before the clock is set carry boot time and
  are rebased to UTC in place before upload. The run id is the epoch second at clock sync.
- **TLS.** Verified by default against a built-in bundle (GTS Root R1 and R4, ISRG Root X1 and X2:
  `certs/ca-bundle.pem`). One keep-alive connection is reused; a verified handshake costs about
  1.5 s on a single-core ESP32-C3.
- **End of run.** `end()` returns 0 only after the metadata was delivered, every buffer came back
  and `/v1/end` was accepted. Without `end()` (power loss) the service finalizes after
  `finalize_after_s` of silence.

## Samples

| Sample | |
|---|---|
| [`samples/first_mission`](samples/first_mission) | 20 s of generated telemetry with progress lines, then `PASS` or `FAIL` with recovery hints. Mirrors the Arduino FirstMission. |
| [`samples/outage_test`](samples/outage_test) | 20 Hz sequence counter; drops Wi-Fi for 25 s the moment the first chunk is queued, reconnects, ends the run. |

Keep credentials in a git-ignored conf file:

```
# secrets.conf (chmod 600, never committed)
CONFIG_APP_WIFI_SSID="my-2.4GHz-network"
CONFIG_APP_WIFI_PSK="..."
CONFIG_APP_ALLOY_API_KEY="..."   # this robot's device key
```

ESP32-C3 build and flash (simple boot, so the image goes at offset 0x0):

```bash
west blobs fetch hal_espressif
```

```bash
west build -b esp32c3_supermini samples/first_mission -- -DZEPHYR_EXTRA_MODULES=$PWD -DEXTRA_CONF_FILE=secrets.conf
```

```bash
esptool --chip esp32c3 --port /dev/cu.usbmodem* --after watchdog-reset write-flash 0x0 build/zephyr/zephyr.bin
```

`esp32c3_supermini` puts the console on the C3's built-in USB-Serial/JTAG. For an
`esp32c3_devkitm` with a UART bridge, use that board name instead.

## Tests

The platform-independent logic lives in [`core/`](core) with no Zephyr headers, so it can move to
the repo's shared `core/` later: CSV row encoding and in-place rebasing, the buffer pool, the end
gate, retry policy, the HTTP `Date` parser, mesh path validation and the metadata JSON. Its ztest
suite runs on `native_sim`:

```bash
west twister -T tests -p native_sim -x=ZEPHYR_EXTRA_MODULES=$PWD
```

CI (`.github/workflows/zephyr.yml`) runs that suite and builds both samples for the ESP32-C3 and the
nRF7002 DK in the `zephyrprojectrtos/ci` image.

## Hardware verification (0.1.0)

ESP32-C3 (QFN32 rev 0.4, 4 MB embedded flash) on 2.4 GHz Wi-Fi at about -78 dBm, against the staging
ingest Worker, checked in Alloy Storage:

| Gate | Result |
|---|---|
| First mission | `end()` = 0, MCAP `ready`, 392 of 392 rows, `mcap.alloy.mission` present |
| SNTP unreachable (`CONFIG_ALLOY_STORAGE_SNTP_SERVER="192.0.2.1"`) | Clock set from the HTTP `Date` header, run lands, MCAP `ready`, 392 of 392 rows |
| 25 s Wi-Fi outage from the first queued chunk, 20 Hz counter | 1200 of 1200 values present exactly once, 0 dropped, `end()` = 0 |

## Differences from the Arduino package

- C API with errno returns and Kconfig instead of a C++ builder.
- `begin()` does not require the network to be up; Wi-Fi is the optional
  `alloy_storage_wifi_connect()` helper.
- Buffers are static (Kconfig), not heap-allocated.
- Not yet ported: `watch()`/`watchAnalog()` auto-sampling, `scope()` GPIO capture, the legacy direct
  R2 mode.

MIT licensed.
