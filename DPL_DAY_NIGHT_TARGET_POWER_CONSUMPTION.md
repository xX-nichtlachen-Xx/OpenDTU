# DPL day/night target power consumption change

This note documents the current branch changes that split the Dynamic Power Limiter target grid consumption into separate day and night values.

## Scope

This change duplicates `Angestrebter Netzbezug` / `Target Grid Consumption`.

It does not change `TotalUpperPowerLimit`.

## Behavior

The DPL now uses:

- `TargetPowerConsumptionDay` while `SunPosition.isDayPeriod()` is `true`
- `TargetPowerConsumptionNight` while `SunPosition.isDayPeriod()` is `false`

Runtime selection is implemented in `PowerLimiterClass::getTargetPowerConsumption()` and used by `calcTargetOutput()`.

## Backward compatibility

Old config payloads with only the legacy field are still accepted.

Deserialization fallback:

- read legacy `target_power_consumption`
- use it as fallback for both new fields
- new config serialization writes only:
  - `target_power_consumption_day`
  - `target_power_consumption_night`

Legacy MQTT command compatibility is also kept:

- `powerlimiter/cmd/target_power_consumption` sets both day and night values
- new commands:
  - `powerlimiter/cmd/target_power_consumption_day`
  - `powerlimiter/cmd/target_power_consumption_night`

## Touched files

### Firmware/config

- `include/Configuration.h`
- `include/defaults.h`
- `include/PowerLimiter.h`
- `src/Configuration.cpp`
- `src/PowerLimiter.cpp`

### MQTT

- `include/MqttHandlePowerLimiter.h`
- `src/MqttHandlePowerLimiter.cpp`
- `src/MqttHandlePowerLimiterHass.cpp`

### Webapp

- `webapp/src/types/PowerLimiterConfig.ts`
- `webapp/src/views/PowerLimiterAdminView.vue`
- `webapp/src/locales/de.json`
- `webapp/src/locales/en.json`
- `webapp/src/locales/fr.json`

## Data model changes

### C++ config struct

Replace:

```cpp
int16_t TargetPowerConsumption;
```

With:

```cpp
int16_t TargetPowerConsumptionDay;
int16_t TargetPowerConsumptionNight;
```

### Default macros

Add:

```cpp
#define POWERLIMITER_TARGET_POWER_CONSUMPTION_DAY POWERLIMITER_TARGET_POWER_CONSUMPTION
#define POWERLIMITER_TARGET_POWER_CONSUMPTION_NIGHT POWERLIMITER_TARGET_POWER_CONSUMPTION
```

### JSON keys

New keys:

```json
{
  "target_power_consumption_day": 0,
  "target_power_consumption_night": 0
}
```

Legacy fallback key still read during deserialization:

```json
{
  "target_power_consumption": 0
}
```

## MQTT topics

### Published status

- `powerlimiter/status/target_power_consumption`
  - active value for current day/night state
- `powerlimiter/status/target_power_consumption_day`
- `powerlimiter/status/target_power_consumption_night`

### Accepted commands

- `powerlimiter/cmd/target_power_consumption`
- `powerlimiter/cmd/target_power_consumption_day`
- `powerlimiter/cmd/target_power_consumption_night`

## Home Assistant

Added two new number entities:

- `Target Power Consumption Day`
- `Target Power Consumption Night`

The old `Target Power Consumption` entity remains and writes both values.

## UI changes

In Power Limiter admin, the single field was replaced with two inputs:

- `Target Grid Consumption (Day)`
- `Target Grid Consumption (Night)`

German locale:

- `Angestrebter Netzbezug (Tag)`
- `Angestrebter Netzbezug (Nacht)`

## Minimal porting checklist

1. Add the two fields in `POWERLIMITER_CONFIG_T`.
2. Add the two default macros.
3. Update power limiter config serialization/deserialization with legacy fallback.
4. Add `getTargetPowerConsumption()` to `PowerLimiterClass`.
5. Replace direct reads of `config.PowerLimiter.TargetPowerConsumption` with the helper.
6. Extend MQTT enum/subscriptions, status publishing, and command handling.
7. Extend Home Assistant number entities.
8. Replace the single webapp field with two typed properties and two inputs.
9. Add locale strings.

## Important note

This patch intentionally does not introduce day/night variants for:

- `BaseLoadLimit`
- `TotalUpperPowerLimit`
- `TargetPowerConsumptionHysteresis`

Only target grid consumption was split.
