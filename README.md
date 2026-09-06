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

[![OpenDTU Build](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/build.yml/badge.svg)](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/build.yml)
[![cpplint](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/cpplint.yml/badge.svg)](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/cpplint.yml)
[![Yarn Linting](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/yarnlint.yml/badge.svg)](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/yarnlint.yml)
[![Yarn Prettier](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/yarnprettier.yml/badge.svg)](https://github.com/xX-nichtlachen-Xx/OpenDTU/actions/workflows/yarnprettier.yml)
