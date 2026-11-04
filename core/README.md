# Portable core

Framework-free C/C++ shared by the C and C++ packages: retry and backoff policy, the graceful-end
gate, the HTTP `Date` clock fallback, and the device wire framing.

Today it has one consumer, so it still lives inside the Arduino package:

- `mcu/arduino/src/AlloyReliability.h`: retry, end-gate and clock policies
- `mcu/arduino/src/alloy/device/`: wire protocol framing and the binary capture core

It moves here when the ESP-IDF component lands, so both packages build from one copy. The portable
tests (`mcu/arduino/test/portable`, run on the host under ASan/UBSan) move with it.
