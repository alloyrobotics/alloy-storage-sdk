# Portable core

Framework-free C/C++ shared by the C and C++ packages: retry and backoff policy, the graceful-end
gate, the HTTP `Date` clock fallback, and the device wire framing.

Each C/C++ package currently carries its own copy of this logic, written natively for its framework
and tested on the host:

| Package | Portable code | Host tests |
|---|---|---|
| Arduino | `mcu/arduino/src/AlloyReliability.h`, `mcu/arduino/src/alloy/device/` | `make -C mcu/arduino/test/portable test` |
| ESP-IDF | `mcu/esp-idf/alloy_storage/portable/` | `make -C mcu/esp-idf/test/host test` |
| Zephyr | `mcu/zephyr/core/` | twister on `native_sim` (`mcu/zephyr/tests`) |

None of these copies include framework headers. Merging them into one C core here, with one test
suite every package runs, is the next step; until then a protocol or policy change has to land in
all three, and [`spec/esp32-http-v1.md`](../spec/esp32-http-v1.md) is the reference they follow.
