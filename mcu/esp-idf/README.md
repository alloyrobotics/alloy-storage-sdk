# ESP-IDF package

The native ESP-IDF component of the Alloy Storage SDK, for firmware that doesn't use the Arduino
framework. It publishes to the ESP Component Registry as `alloyrobotics/alloy_storage`.

| Path | |
|---|---|
| [`alloy_storage/`](alloy_storage) | The component: [README](alloy_storage/README.md), API, Kconfig, examples. This folder is what the registry publishes. |
| [`alloy_storage/portable/`](alloy_storage/portable) | Framework-free core (CSV rows, buffer pool, retry and end policy, `Date` parser, metadata JSON), ready to move into [`core/`](../../core). |
| [`test/host/`](test/host) | Host tests for the portable core, under ASan and UBSan: `make -C mcu/esp-idf/test/host test`. |
| [`test/build-all.sh`](test/build-all.sh) | Builds every example for esp32, esp32s2, esp32s3, esp32c3 and esp32c6. |

Wire protocol: [`spec/esp32-http-v1.md`](../../spec/esp32-http-v1.md), shared with the Arduino
package.
