# Alloy device HTTP protocol v1 (CSV chunks)

The protocol the ESP32 packages speak to the ingest service: the Arduino package (`mcu/arduino`, cloud
mode) and the ESP-IDF component (`mcu/esp-idf`). A device streams one CSV file per channel in chunks;
the service assembles every chunk of a run into one indexed MCAP in the user's Alloy Storage mesh.

This document describes the protocol as `services/ingest` implements it (`src/index.ts`,
`src/session-do.ts`, `src/csv.ts`, `src/mcap.ts`). Where this text and that code disagree, the code
is the reference and this file has a bug.

Base URL: `https://ingest.alloylogger.com`. All requests use HTTPS and verify the server certificate
by default.

## Identity

A run is identified by the triple `(API key, device, session)`. Every request of the run carries all
three, and the service routes them to one serialization point per triple (a Durable Object keyed by
`sha256(key):device:session`).

| Header | Rule | Meaning |
|---|---|---|
| `Authorization` | `Bearer <Alloy data API key>` | The key also names the destination org. |
| `X-Alloy-Device` | `^[A-Za-z0-9_-]{1,64}$` | Stable device id. Devices sanitize to this set. |
| `X-Alloy-Session` | `^\d{1,12}$` | Epoch seconds when the device's UTC clock was first set this power-on. A new power-on is a new run. |
| `X-Alloy-Mesh-Path` | after trimming leading and trailing `/`: `^[A-Za-z0-9_/-]{1,128}$` | Mesh folder. The MCAP lands in `<mesh path>/<session>/`. |
| `X-Alloy-Finalize-Ms` | optional, `^\d{4,8}$` | Inactivity window after which the service finalizes the run. Clamped server-side to 30 000 .. 1 800 000 ms. Default 120 000. Pinned by the first request of the run. |

The service validates the key on every request (results cached 5 minutes for a valid key, 1 minute
for an invalid one) by minting an Alloy upload session for the mesh path.

## Requests

### `POST /v1/meta`

Optional metadata sidecar. `Content-Type: application/json`, body at most 16 KiB.

```json
{
  "device": "esp32-first-mission",
  "firmware": "first-mission-v1",
  "mission": "first ESP32 mission",
  "session": "2026-11-05T01:12:09Z",
  "fields": [
    {"channel": "sample", "name": "wave", "min": -1, "max": 1, "about": "generated sine wave"},
    {"channel": "sample", "name": "uptime_s", "unit": "s", "min": 0, "max": 20}
  ]
}
```

Every key is optional. `firmware`, `mission` and `session` become MCAP metadata record `alloy`
(keys `firmware`, `mission`, `session_iso`, next to `device`, `session` and `mesh_path`), which Alloy
Storage indexes as `mcap.alloy.<key>`. `fields` adds units, ranges and descriptions to the channel
schemas. The latest upload wins. Devices send it once, before their first chunk, and retry it like
a chunk.

### `POST /v1/chunk`

One sealed buffer of one channel. `Content-Type: text/csv`, body at most 64 KiB (devices default to
12 KiB buffers).

| Header | Rule |
|---|---|
| `X-Alloy-Channel` | `^[A-Za-z0-9_-]{1,64}$` |
| `X-Alloy-Seq` | `^\d{1,9}$`. Unique per chunk within the run, across all channels. A retry reuses the seq. |

Body:

```
t_ns,wave,uptime_s
1793927529051000000,0.0998334,0.05
1793927529101000000,0.198669,0.1
```

- Line 1 is the header: `t_ns` followed by the field names. Field names hold no `,` or newline.
- Every other line is a row: a UTC timestamp in nanoseconds, then one cell per field. Devices write
  the stamp zero-padded to 19 digits. An empty cell is null (devices encode NaN and infinity this
  way). Rows with an unparsable stamp are skipped.
- Lines end with `\n`; a trailing newline is allowed.
- A channel's field set changes only at a chunk boundary: the device seals the open chunk when it
  sees a different header. A new header starts a new schema generation of that channel in the MCAP.
- Chunks of one channel are assembled in seq order, and rows are merged across channels by `t_ns`.

`(channel, seq)` is the dedupe key: a chunk whose `(channel, seq)` the run already holds is answered
`204` and not stored again. Devices therefore retry freely, but must never reuse a seq for different
data.

### `POST /v1/end`

Optional graceful end. Empty body. The service finalizes the run now instead of after the
inactivity window. Without it, power loss or a dropped connection ends the run after the window.

### `GET /v1/health`

Returns `200 {"ok":true}` with no authentication. Devices also use it as a clock source: its `Date`
response header (IMF-fixdate, for example `Thu, 05 Nov 2026 01:12:09 GMT`) sets the UTC clock when
SNTP is unavailable.

## Responses

| Status | When | Device action |
|---|---|---|
| `204` | chunk or meta stored, or chunk was a duplicate | delivered |
| `202` | `/v1/end` accepted; finalization runs asynchronously. Also `/v1/end` while finalizing (`Retry-After: 2`), or `/v1/end` on a run whose finalization failed (finalization restarts) | finalization accepted |
| `204` | `/v1/end` on a run that is already done | finalization accepted |
| `400` | a header or route rule above is broken | terminal |
| `401` | missing or invalid key | terminal |
| `404`, `405` | unknown route, or not `POST` | terminal |
| `409` | chunk or meta for a run that is already finalized, or a v1 request for a run that started on the binary protocol (`/v2/frame`) | terminal for this run: the data cannot join it |
| `413` | `Content-Length` above the chunk or meta limit | terminal |
| `429` | more than 120 chunks in 60 s for one run | retry |
| `503` | auth backend unavailable (`Retry-After: 5`), chunk or meta while finalizing (`Retry-After: 5`), or chunk or meta on a run whose finalization failed | retry |
| other `5xx` | service error | retry |

## Run lifecycle

```
receiving --(inactivity window, or /v1/end)--> finalizing --> done
                                                   |
                                                   +--(10 failed attempts)--> failed --(/v1/end)--> finalizing
```

The first request of a run (any route) creates it and pins the inactivity window. Every chunk, meta
or duplicate re-arms the window. Finalization reads every staged chunk, builds one MCAP and uploads
it to `<mesh path>/<session>/<device>_<session>.mcap`, with the sidecar next to it as
`<device>_meta.json`. Failed finalizations back off exponentially (up to 60 s) for 10 attempts.

## Device behaviour (shared by the ESP32 packages)

Not enforced by the service, but both packages behave this way and new packages should too.

**Clock.** Rows, the session id and the MCAP all need UTC. Start SNTP; if the clock is still unset
after 5 s, take it from the `Date` header of `GET /v1/health` (retrying with the backoff below until
one works). A clock that is not set yet reads below `1700000000` epoch seconds. Rows logged before
the clock is set carry boot-relative nanoseconds (below `10^18`) in the same 19-digit field; the
device rewrites them in place to wall clock, adding one boot-to-UTC offset captured when the clock
was first set, just before each chunk is uploaded.

**Order.** Metadata first, then chunks oldest first, one request at a time on one kept-alive
connection, then `/v1/end`. Chunks are numbered `0, 1, 2, ...` in upload order.

**Retry.** A transport error (DNS, TCP, TLS, timeout, no network) and `408`, `425`, `429` and `5xx`
are retried after 1, 2, 4, 8, 16, then 30 s per further attempt, without limit, holding the chunk.
`409` is counted as stale and the chunk is released. Any other status is terminal: the chunk is
released and counted as failed.

**Buffering.** A fixed RAM pool (default 4 x 12 KiB). Each channel fills its own buffer; a buffer is
sealed for upload at 90% full, when its field set changes, or on a timer (default 4 s after it
opened) only if a replacement buffer is free and no sealed buffer is still waiting. During an
outage the timer therefore stops spending buffers on part-full chunks. With no free buffer, the
oldest sealed buffer is shed (counted as dropped), never the one sealed for the current row.

**Graceful end.** Stop accepting rows, seal every open buffer, wait until the metadata has settled
and every buffer is back in the pool, then send `/v1/end`. Report success only if the metadata and
every chunk were delivered, nothing was shed, and `/v1/end` was answered `2xx`.
