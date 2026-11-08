----------------------------------------------------------------------
## openDTUonTuning
----------------------------------------------------------------------
## About

This OpenDTU fork focuses on Hoymiles microinverters and aims to adapt the software power limit to the technical limits of the hardware, where technically possible and supported by the manufacturer.

Knowledge and findings from the OpenDTU community have been collected and consolidated for this project. Thanks to everyone who contributed.

## Known Original Board Limits

| Series | Maximum Power |
|--------|---------------|
| HM_1T  | up to 400 W   |
| HM_2T  | up to 800 W   |
| HM_4T  | up to 1500 W  |
|--------|---------------|
| HMS_1T | up to 500 W   |
| HMS_2T | up to 1000 W  |
| HMS_4T | up to 2000 W  |
|--------|---------------|
| HMT_4T | up to 2000 W  |
|------------------------|
| HMT_6T | up to 2250 W  |


## Notes

The `HMS_*xxxx*W_*x*T` series may also be technically capable of operating beyond these limits. However, this is currently not possible via OTA updates.



[Nichtlachen's openDTUonTuning discussion](https://github.com/xX-nichtlachen-Xx/OpenDTU/discussions/8)


[openDTU discussion Channel](https://github.com/tbnobody/OpenDTU/discussions/3318)

[Main Discord Channel](https://discord.com/channels/984173303147155506/1547311994300928040)

[Feel free to join the Discord for updates and discussions, etc.](https://discord.com/invite/CmhuZwWZM2)



## Background

This project was started from [this](https://www.mikrocontroller.net/topic/525778) discussion (Mikrocontroller.net).
It was the goal to replace the original Hoymiles DTU (Telemetry Gateway) with their cloud access. With a lot of reverse engineering the Hoymiles protocol was decrypted and analyzed.

## Documentation

The documentation can be found [here](https://opendtu.solar/).
Please feel free to support and create a PR in [this](https://github.com/tbnobody/OpenDTU-docs) repository to make the documentation even better.


## Currently supported Inverters

A list of all currently supported inverters can be found in the [supported inverter overview](https://www.opendtu.solar/hardware/inverter_overview/).
