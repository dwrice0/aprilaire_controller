# Sensor Calibration Guide

The BME280 temperature and humidity sensor may read slightly differently from the E070's internal sensor due to differences in placement, airflow, and sensor self-heating. Applying calibration offsets ensures the values your controller sends to the E070 accurately reflect actual conditions.

## Overview

The calibration process involves:

1. Running your controller **alongside the original Model 76** for 48–72 hours
2. Comparing the BME280 readings against the Model 76's sensor values
3. Calculating a fixed offset for both RH and temperature
4. Applying those offsets in the firmware before disconnecting the Model 76

## Prerequisites

- Original Model 76 still connected to the RS-485 bus
- Controller flashed and running with `send_r_frame()` commented out (receive-only mode)
- MQTT broker running and accessible
- Home Assistant with InfluxDB and Grafana (recommended) or just the HA history graphs

## Step 1 — Enable Calibration Publishing

The firmware includes a calibration MQTT topic that publishes BME280 readings alongside the Model 76's values and the calculated difference every 5 seconds.

In `main.c`, confirm `MQTT_TOPIC_CALIBRATE` is defined and the calibration publish block in `bme280_task` is active:

```c
#define MQTT_TOPIC_CALIBRATE    "aprilaire/calibrate"
```

The calibration payload looks like:

```json
{
  "bme280_rh": 52.3,
  "bme280_temp": 21.6,
  "model76_rh": 56.0,
  "model76_temp": 22.9,
  "diff_rh": -3.7,
  "diff_temp": -1.3
}
```

## Step 2 — Set Up Home Assistant Sensors

Add the following to `configuration.yaml` to create HA sensors from the calibration topic:

```yaml
mqtt:
  sensor:
    - name: "AprilAire BME280 RH"
      state_topic: "aprilaire/calibrate"
      value_template: "{{ value_json.bme280_rh }}"
      unit_of_measurement: "%"
      device_class: humidity
      unique_id: aprilaire_bme280_rh

    - name: "AprilAire BME280 Temp"
      state_topic: "aprilaire/calibrate"
      value_template: "{{ value_json.bme280_temp }}"
      unit_of_measurement: "°C"
      device_class: temperature
      unique_id: aprilaire_bme280_temp

    - name: "AprilAire Model76 RH"
      state_topic: "aprilaire/calibrate"
      value_template: "{{ value_json.model76_rh }}"
      unit_of_measurement: "%"
      device_class: humidity
      unique_id: aprilaire_model76_rh

    - name: "AprilAire Model76 Temp"
      state_topic: "aprilaire/calibrate"
      value_template: "{{ value_json.model76_temp }}"
      unit_of_measurement: "°C"
      device_class: temperature
      unique_id: aprilaire_model76_temp

    - name: "AprilAire RH Diff"
      state_topic: "aprilaire/calibrate"
      value_template: "{{ value_json.diff_rh }}"
      unit_of_measurement: "%"
      unique_id: aprilaire_diff_rh

    - name: "AprilAire Temp Diff"
      state_topic: "aprilaire/calibrate"
      value_template: "{{ value_json.diff_temp }}"
      unit_of_measurement: "°C"
      unique_id: aprilaire_diff_temp
```

Restart Home Assistant after saving.

## Step 3 — Set Up the Calibration Dashboard

Add the following dashboard view in Home Assistant (raw YAML mode):

```yaml
cards:
  - type: entities
    title: Current readings
    entities:
      - entity: sensor.aprilaire_bme280_rh
        name: BME280 RH
      - entity: sensor.aprilaire_model76_rh
        name: Model 76 RH
      - entity: sensor.aprilaire_diff_rh
        name: RH diff (BME280 − Model 76)
      - entity: sensor.aprilaire_bme280_temp
        name: BME280 temp
      - entity: sensor.aprilaire_model76_temp
        name: Model 76 temp
      - entity: sensor.aprilaire_diff_temp
        name: Temp diff (BME280 − Model 76)

  - type: history-graph
    title: RH history
    hours_to_show: 24
    entities:
      - entity: sensor.aprilaire_bme280_rh
        name: BME280
      - entity: sensor.aprilaire_model76_rh
        name: Model 76

  - type: history-graph
    title: Temperature history
    hours_to_show: 24
    entities:
      - entity: sensor.aprilaire_bme280_temp
        name: BME280
      - entity: sensor.aprilaire_model76_temp
        name: Model 76

  - type: history-graph
    title: RH diff history
    hours_to_show: 24
    entities:
      - entity: sensor.aprilaire_diff_rh
        name: RH diff

  - type: history-graph
    title: Temp diff history
    hours_to_show: 24
    entities:
      - entity: sensor.aprilaire_diff_temp
        name: Temp diff
```

## Step 4 — Optional: InfluxDB and Grafana

For more detailed analysis, configure Home Assistant to write the calibration sensors to InfluxDB by adding to `configuration.yaml`:

```yaml
influxdb:
  host: YOUR_INFLUXDB_HOST
  port: 8086
  database: YOUR_DATABASE
  include:
    entities:
      - sensor.aprilaire_bme280_rh
      - sensor.aprilaire_model76_rh
      - sensor.aprilaire_diff_rh
      - sensor.aprilaire_bme280_temp
      - sensor.aprilaire_model76_temp
      - sensor.aprilaire_diff_temp
```

In Grafana, create a dashboard with the following panels:

**RH and Temperature time series** — plot BME280 vs Model 76 over the full collection period to visually confirm the sensors are tracking each other.

**RH fixed offset (mean diff):**
```sql
SELECT mean(value) FROM "%" 
WHERE entity_id = 'aprilaire_diff_rh' 
AND $timeFilter
```

**Temp fixed offset (mean diff):**
```sql
SELECT mean(value) FROM "°C" 
WHERE entity_id = 'aprilaire_diff_temp' 
AND $timeFilter
```

**RH offset stability (stddev):**
```sql
SELECT stddev(value) FROM "%" 
WHERE entity_id = 'aprilaire_diff_rh' 
AND $timeFilter
```

**Temp offset stability (stddev):**
```sql
SELECT stddev(value) FROM "°C" 
WHERE entity_id = 'aprilaire_diff_temp' 
AND $timeFilter
```

## Step 5 — Interpreting the Results

### Temperature

The temperature offset is typically very stable. Once the BME280 has been running for a few hours and the stddev is below 0.5°C, the mean diff is your offset.

### Relative Humidity

The RH offset takes longer to stabilize due to the BME280's thermal settling and response lag during dehumidifier cycles. Key observations:

- **Allow 48–72 hours** of data collection for the BME280 to fully stabilize
- **Ignore early readings** — the BME280 takes several hours to thermally settle after first power-up
- **Response lag artifact** — during active dehumidifier cycles the diff will temporarily spike as the two sensors respond at different speeds. This is normal and should be ignored when calculating the offset
- **Use steady-state data** — the reliable offset is visible when the dehumidifier is off and RH is stable

A stddev below 1.5% over the full collection period, combined with a stable mean, indicates the fixed offset is reliable.

### When to use linear regression instead

If the RH diff history shows a clear slope — i.e. the diff is consistently larger at high RH and smaller at low RH — a linear correction may be more accurate than a fixed offset:

```
corrected_rh = (bme280_rh * slope) + intercept
```

However for most installations covering a typical dehumidifier operating range of 40–65% RH, a fixed offset is sufficient.

## Step 6 — Apply the Offsets

Once you have stable offset values, update `main.c`:

```c
/* Calibration offsets — adjust based on your measurements */
#define RH_OFFSET    3.0f   /* % — typical range: 2.0 to 5.0 */
#define TEMP_OFFSET  1.27f  /* °C — typical range: 0.5 to 2.0 */
```

In `send_r_frame()`:

```c
if (r_temp_valid) {
    r_rh   = r_rh   + (int)(RH_OFFSET   * 10.0f);
    r_temp = r_temp + (int)(TEMP_OFFSET  * 10.0f);

    /* Clamp to valid ranges */
    r_rh   = r_rh   < 0 ? 0 : r_rh   > 1000 ? 1000 : r_rh;
    r_temp = r_temp < 0 ? 0 : r_temp > 500  ? 500  : r_temp;
}
```

## Step 7 — Go Live

Once offsets are applied and the firmware is reflashed:

1. Take a logic analyzer baseline capture of the Model 76's R-frames
2. Uncomment `send_r_frame()` in `uart_event_task()`
3. Re-enable the BME280 state update in `bme280_task()`
4. Remove the `FRAME_R_UPDATED` writeback block from `uart_event_task()` 
5. Remove the calibration publish block from `bme280_task()`
6. Remove `MQTT_TOPIC_CALIBRATE` define
7. Reflash
8. Disconnect the Model 76
9. Verify E070 continues responding to M-frames with your R-frames on the logic analyzer
10. Confirm dryness setpoint changes and on/off commands work from Home Assistant

## Typical Offset Values

Based on the reference installation (BME280 mounted on the controller PCB, controller located adjacent to the E070):

| Sensor | Typical offset |
|--------|---------------|
| RH | +3.0% |
| Temperature | +1.27°C |

Your values may differ depending on sensor placement, airflow, and ambient conditions. Always measure rather than assume.