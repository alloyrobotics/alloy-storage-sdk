# Alloy Storage SDK

The Alloy Storage SDK gets robot data into [Alloy Storage](https://usealloy.ai): every power-on or
run becomes one indexed MCAP you can replay, inspect, query with SQL and analyze over MCP.

This repo holds every SDK package. Each folder is its own package, with its own version, tags and
release channel.

| Package | Folder | Install | Status |
|---|---|---|---|
| Arduino (ESP32 family, UNO R4 WiFi) | [`mcu/arduino`](mcu/arduino) | Arduino Library Manager: **AlloyStorage** | 0.6.0 |
| ESP-IDF component | [`mcu/esp-idf`](mcu/esp-idf) | ESP Component Registry | planned |
| MicroPython | [`mcu/micropython`](mcu/micropython) | `mip` | planned |
| Linux, Python and ROS 2 | [`linux`](linux) | | planned (Alloy Edge covers these today) |

The service these packages upload to, and the wire format they speak, live here too:

| Part | Folder |
|---|---|
| Ingest service (Cloudflare Worker at `ingest.alloylogger.com`) | [`services/ingest`](services/ingest) |
| Device wire protocol | [`spec`](spec) |
| Shared portable core | [`core`](core) |

## Layout

```
alloy-storage-sdk/
├── spec/               wire protocol every package speaks
├── core/               portable, framework-free code shared by C/C++ packages
├── mcu/
│   ├── arduino/        Arduino library (published to Library Manager)
│   ├── esp-idf/        ESP-IDF component (planned)
│   └── micropython/    MicroPython package (planned)
├── linux/              Linux, Python and ROS 2 packages (planned)
├── services/ingest/    ingest service
└── tools/release/      release scripts
```

## Releases

Each package is tagged on its own: `arduino-v0.6.0`, later `esp-idf-v0.1.0` and so on. A fix to one
package never forces a release of another.

The Arduino library is the one exception to "publish from a folder": Library Manager only indexes a
library at the root of its own repository. `tools/release/sync-arduino.sh` copies `mcu/arduino` into
[alloyrobotics/alloy-logger-arduino](https://github.com/alloyrobotics/alloy-logger-arduino), which is
the Library Manager source. Develop here, never there.

## Formerly AlloyLogger

This repo continues the `alloy-logger-arduino` history. AlloyLogger 0.5.0 became the Arduino package
of the Alloy Storage SDK in 0.6.0. Old sketches keep compiling through a deprecated `AlloyLogger`
alias; see [`mcu/arduino`](mcu/arduino).

MIT licensed.
