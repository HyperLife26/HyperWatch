# Wiring: HyperWatch

## ESP32-S3 SuperMini

| ESP32-S3 pin | Connects to |
|---|---|
| GPIO 8 | SDA: MAX30102, TMP117, MPU6050, SCD30/SCD40, SH1106 (shared I2C) |
| GPIO 9 | SCL: MAX30102, TMP117, MPU6050, SCD30/SCD40, SH1106 (shared I2C) |
| GPIO 10 | one side of momentary button; other side to GND |
| 3V3 | VCC of each breakout (only if the breakout accepts 3.3 V) |
| GND | common ground (sensors, OLED, button) |

I2C is initialised at **100 kHz**. The Sensirion SCD30/SCD4x are 100 kHz parts;
the MAX30102, TMP117 and SH1106 all work at that rate.

## I2C addresses

| Device | Address | Notes |
|---|---|---|
| MAX30102 | `0x57` | SparkFun MAX3010x library |
| TMP117 | `0x49` | Adafruit TMP117 |
| MPU6050 | `0x68` (AD0 low) or `0x69` (AD0 high) | raw-I2C `MotionMonitor` |
| SCD30 | `0x61` | raw-I2C `CO2Monitor` |
| SCD40 / SCD41 | `0x62` | raw-I2C `CO2Monitor` (autodetected) |
| SH1106 OLED | `0x3C` or `0x3D` | U8g2 `U8G2_SH1106_128X64_NONAME_F_HW_I2C` |

If the OLED is not detected, the firmware still runs and reports over serial.

## Power notes

- Many MAX30102 breakout boards are 3.3 V only; check before connecting 5 V.
- The SH1106 OLED and both sensors share the bus; keep leads short.
- The button uses `INPUT_PULLUP`; no external resistor is required.
- If you power from USB while flashing, the sensor rail should be stable before calibration.

## Firmware bring-up

Requires MAX30102 + TMP117 + OLED + button. The MPU6050 and SCD30/SCD40 are
optional: the firmware detects them and degrades gracefully if they are absent.

1. Detects TMP117 at `0x49`, MAX30102 at `0x57`, MPU6050 at `0x68`/`0x69`,
   and an SCD30 at `0x61` or SCD4x at `0x62`.
2. Streams PPG windows through `PPGMonitor`.
3. Samples the MPU6050 at 20 Hz for activity, steps and the 24 h sleep/exercise
   ring. Sleep is only credited for rest runs of at least 45 minutes.
4. Reads cabin CO2 every 2 s (SCD30) or 5 s (SCD4x) and applies an air-quality
   penalty to the immune/sleep scores. CO2 never changes the radiation score.
5. Captures a personal HR/HRV/SpO2/temperature baseline after 20 accepted HR values.
6. Runs the rule engine + int8 model every 200 ms. If the vitals snapshot is
   invalid, physiological terms fall back to the crew baseline so a
   disconnected probe cannot fabricate a cardiac alert.
7. Saves mission/health state to NVS every 30 s.

## Serial

Both builds print JSON at **115200 baud**.

If `arduino-cli monitor` shows nothing, it is holding the ESP32-S3 in reset by
asserting DTR/RTS. Use the bundled reader instead:

```bash
python3 tools/serial_monitor.py -p /dev/ttyACM0
```

The production build also accepts configuration commands (newline-terminated):

```
help
status
crew ASTRO-07
age 47
cvd 1
phase 3
day 120
shield 2.5
sleep 7.5
exercise 60
motion
co2
resetmotion
reset
```

`phase` updates gravity and the daily dose rate; `shield` rescales the dose.
Changes persist in NVS on the next autosave.

Example JSON:

```json
{"t":12000,"crew":"ASTRO-01","phase":"MICROGRAV","mission_day":42,
 "gravity":0.000,"rad_daily":0.500,"rad_cum":21.00,
 "hr":72.1,"hrv":48.3,"spo2":97.4,"temp":36.70,
 "signal":{"finger":true,"quality":85,"sensor":"READY",
           "hr_valid":true,"spo2_valid":true,"vitals_valid":true},
 "env":{"co2":842,"valid":true,"air":"ELEVATED","temp":23.4,"rh":41.2,"sensor":"SCD4x"},
 "motion":{"valid":true,"activity":18.3,"steps":4210,"exercise_min":37,
           "sleep_h":7.2,"resting":false},
 "risk":{"cardio":7.4,"bone":9.7,"immune":20.0,"radiation":5.5,"sleep":14.8},
 "composite":11.2,"cardiac_prob":0.01,"alert":false,
 "symptom":false,"model_us":85}
```
