# Wire protocol

What every SDK package and the ingest service agree on.

- [`alloy-device-wire-v1.md`](alloy-device-wire-v1.md): the binary device protocol (`POST /v2/frame`)
  used by the UNO R4 WiFi package.

The ESP32 path (`POST /v1/chunk`, `/v1/meta`, `/v1/end`, `GET /v1/health`) is defined by the ingest service
([`services/ingest/src/index.ts`](../services/ingest/src/index.ts)) and will be written up here as its
own spec.
