# OpenDTU with Hoymiles HM / HMS / HMT Inverter Firmware Update Function / Updater


Update your HM / HMS / HMT inverter firmware.
Can be used as an alternative to the original Hoymiles DTU for updating inverter firmware.
The Hoymiles DTU/Cloud itself is neither involved nor required.
Supported models: Hoymiles HM / HMS / HMT 300, 350, 400, 600, 700, 800, 1000, 1200, 1500, 1600, 1800, 2000, 2250.
If the update aborts or the inverter becomes unresponsive after flashing, perform a reflash.
Firmware files are located in: ./Firmware

Handle with care and on your own risk!
PSRAM is optional now!
It is recommended to disable all NRF24/CMT2300A/Inverter traffic during the update.
Update takes about half an hour per inverter.

Upload Firmware File.
Start Update.

[Manual](https://github.com/tbnobody/OpenDTU/discussions/3168/).

Fimware files are stored in (./Firmware)-

|  ?  | Phase | Type | DSP | ? | ? |MI/B| Version (hex/dec)| CRC |**Serial**|   Info              |
|-----|-------|------|------|---|--|----|------------------|------|----------|-------------------|
| 0   | 1     | 0    | 0   | 0 | 0 | 0   | 2721 / 1.0.17   | 81  | **1121** | HM 1T MI |
| 0   | 1     | 0    | 0   | 0 | 0 | 0   | 2774 / 1.1.0    | 2E  | **1121** | HM 1T MI |
| 0   | 1     | 1    | 0   | 0 | 0 | 0   | 2720 / 1.0.16   | 81  | **1141** | HM 2T MI |
| 0   | 1     | 1    | 0   | 0 | 0 | 0   | 2776 / 1.1.2    | 2B  | **1141** | HM 2T MI |
| 0   | 1     | 2    | 0   | 0 | 0 | 0   | 2722 / 1.0.18   | 7E  | **1161** | HM 4T MI |
| 0   | 1     | 2    | 0   | 0 | 0 | 0   | 2777 / 1.1.3    | 29  | **1161** | HM 4T MI |
| 0   | 1     | 0    | 0   | 0 | 0 | 0   | 4E22 / 2.0.2     | 59  | **1124** | HMS 1T MI (HM-Board) |
| 0   | 1     | 0    | 0   | 0 | 0 | 1   | 4E20 / 2.0.0     | 5A  | **1125/1400/1403** | HMS 1T B |
| 0   | 1     | 0    | 0   | 0 | 0 | 2   | 2774 / 1.1.0     | 2C  | **1126** | HMS 1T US |
| 0   | 1     | 1    | 1   | 0 | 0 | 1   | 2718 / 1.0.8     | 78  | **1143/1144/1144/1410/1412** | HMS 2T / HMS_W_2T |
| 0   | 1     | 1    | 1   | 0 | 0 | 1   | 2845 / 1.3.9     | 4A  | **1143/1144/1144/1410/1412** | HMS 2T / HMS_W_2T |
| 0   | 1     | 1    | 1   | 0 | 0 | 2   | 2778 / 1.1.4     | 17  | **1146** | HMS 2T US |
| 0   | 1     | 2    | 1   | 0 | 0 | 0   | 272B / 1.0.27   | 65  | **1164** | HMS 4T |
| 0   | 1     | 2    | 1   | 0 | 0 | 0   | 2780 / 1.1.12   | 10  | **1164** | HMS 4T |
| 0   | 1     | 2    | 1   | 0 | 0 | 0   | 2786 / 1.1.18   | 0A  | **1164** | HMS 4T Thailand|
| 0   | 1     | 2    | 1   | 0 | 0 | 0   | 4E24 / 2.0.4    | 45  | **1164** | HMS 4T |
| 0   | 4     | 2    | 0   | 0 | 0 | 0   | 2719 / 1.0.9     | 57  | **1162** | HME1 4T MI|
| 0   | 1     | 2    | 2   | 0 | 0 | 0   | 2719 / 1.0.9     | 67  | **1165** | HMS 4T 2000B_T |
| 0   | 1     | 2    | 1   | 1 | 0 | 1   | 4E25 / 2.0.5     | 42  | **1166/1421** | HMS 4T 2000C_B |
| 0   | 1     | 2    | 3   | 0 | 0 | 1   | 283D / 1.3.1     | 31  | **1620** | HMS 4T WB_B  |
| 0   | 3     | 5    | 0   | 0 | 0 | 0   | 2779 / 1.1.5     | 04  | **1361** | HMT 4T |
| 0   | 3     | 6    | 0   | 0 | 0 | 0   | 2716 / 1.0.6     | 66  | **1362** | HMT 4T NA R |
| 0   | 3     | 3    | 0   | 0 | 0 | 0   | 27F3 / 1.2.27    | 8C  | **1382** | HMT 6T |
| 2   | 1     | 6    | 0   | 0 | 0 | 0   | 28A0 / 1.0.4     | F9  | **1520** | MIT-5000 (?F280034) |

If your inverter serial is blocked or not authorized, feel free to contact me.

[![OpenDTU Build](https://github.com/tbnobody/OpenDTU/actions/workflows/build.yml/badge.svg)](https://github.com/tbnobody/OpenDTU/actions/workflows/build.yml)
[![cpplint](https://github.com/tbnobody/OpenDTU/actions/workflows/cpplint.yml/badge.svg)](https://github.com/tbnobody/OpenDTU/actions/workflows/cpplint.yml)
[![Yarn Linting](https://github.com/tbnobody/OpenDTU/actions/workflows/yarnlint.yml/badge.svg)](https://github.com/tbnobody/OpenDTU/actions/workflows/yarnlint.yml)
[![Yarn Prettier](https://github.com/tbnobody/OpenDTU/actions/workflows/yarnprettier.yml/badge.svg)](https://github.com/tbnobody/OpenDTU/actions/workflows/yarnprettier.yml)

## !! IMPORTANT UPGRADE NOTES !!

If you are upgrading from a version before 15.03.2023 you have to upgrade the partition table of the ESP32. Please follow the [this](docs/UpgradePartition.md) documentation!

## Background

This project was started from [this](https://www.mikrocontroller.net/topic/525778) discussion (Mikrocontroller.net).
It was the goal to replace the original Hoymiles DTU (Telemetry Gateway) with their cloud access. With a lot of reverse engineering the Hoymiles protocol was decrypted and analyzed.

## Documentation

The documentation can be found [here](https://tbnobody.github.io/OpenDTU-docs/).
Please feel free to support and create a PR in [this](https://github.com/tbnobody/OpenDTU-docs) repository to make the documentation even better.

## Breaking changes

Generated using: `git log --date=short --pretty=format:"* %h%x09%ad%x09%s" | grep BREAKING`

```code
* 8cab3335      2025-08-07      BREAKING CHANGE: WebAPI endpoint `/api/limit/config` requires different parameters
* 8372deaf      2025-04-18      BREAKING CHANGE: Logging newline changed from "\r\n" to "\n"
* 1b637f08      2024-01-30      BREAKING CHANGE: Web API Endpoint /api/livedata/status and /api/prometheus/metrics
* e1564780      2024-01-30      BREAKING CHANGE: Web API Endpoint /api/livedata/status and /api/prometheus/metrics
* f0b5542c      2024-01-30      BREAKING CHANGE: Web API Endpoint /api/livedata/status and /api/prometheus/metrics
* c27ecc36      2024-01-29      BREAKING CHANGE: Web API Endpoint /api/livedata/status
* 71d1b3b       2023-11-07      BREAKING CHANGE: Home Assistant Auto Discovery to new naming scheme
* 04f62e0       2023-04-20      BREAKING CHANGE: Web API Endpoint /api/eventlog/status no nested serial object
* 59f43a8       2023-04-17      BREAKING CHANGE: Web API Endpoint /api/devinfo/status requires GET parameter inv=
* 318136d       2023-03-15      BREAKING CHANGE: Updated partition table: Make sure you have a configuration backup and completly reflash the device!
* 3b7aef6       2023-02-13      BREAKING CHANGE: Web API!
* d4c838a       2023-02-06      BREAKING CHANGE: Prometheus API!
* daf847e       2022-11-14      BREAKING CHANGE: Removed deprecated config parsing method
* 69b675b       2022-11-01      BREAKING CHANGE: Structure WebAPI /api/livedata/status changed
* 27ed4e3       2022-10-31      BREAKING: Change power factor from percent value to value between 0 and 1
```

## Currently supported Inverters

A list of all currently supported inverters can be found [here](https://www.opendtu.solar/hardware/inverter_overview/)
