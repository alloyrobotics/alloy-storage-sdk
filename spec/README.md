# Wire protocol

What every SDK package and the ingest service agree on.

- [`esp32-http-v1.md`](esp32-http-v1.md): the HTTP chunk protocol (`POST /v1/chunk`, `/v1/meta`,
  `/v1/end`, `GET /v1/health`) used by the Arduino ESP32, ESP-IDF and Zephyr packages.
- [`alloy-device-wire-v1.md`](alloy-device-wire-v1.md): the binary device protocol (`POST /v2/frame`)
  used by the UNO R4 WiFi package.
