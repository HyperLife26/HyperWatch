# Model Card: HyperWatch Risk MLP

## Model details

| Field | Value |
|---|---|
| Name | HyperWatch Risk MLP |
| Version | 1.0 |
| Type | Fully-connected neural network, int8 quantised |
| Architecture | 13 → 16 (ReLU) → 8 (ReLU) → 6 (sigmoid) |
| Parameters | 384 int8 weights + 30 int32 biases |
| Footprint | ~1 KB flash |
| Runtime | Pure C, no external ML dependency |
| Inputs | 13 features (see below) |
| Outputs | 5 risk scores (0–100) + acute cardiac-event probability (0–1) |
| Weights | `firmware/HyperWatchCore/src/risk_model_weights.h` |

### Input features

`hr`, `hrv`, `spo2`, `skin_temp`, `temp_slope`, `activity`, `sleep_hours`, `mission_day`, `gravity`, `daily_radiation`, `age`, `known_cvd`, `symptom`

All features are normalised to `[0, 1]` before int8 quantisation.

### Outputs

`cardio`, `bone`, `immune`, `radiation`, `sleep` (0–100) and `cardiac_event` (0–1).

### Sensor inputs around the model

The quantised model's 13 inputs are unchanged. The production firmware adds two
sensor-derived signals around it:

- **MPU6050** supplies `activity` (measured, replacing the HR-derived proxy),
  plus step count, exercise minutes and a 24 h sleep/rest estimate. Sleep is
  only credited for rest runs of at least 45 minutes.
- **SCD30 / SCD40 / SCD41** supplies cabin CO2. It is **not** a model input
  and **not** a radiation measurement. It is applied after fusion as a
  transparent air-quality penalty to the immune and sleep scores:
  `penalty = min((ppm − 800) / 100, 12)`.

When the vitals snapshot is invalid (no finger, sensor timeout), the engine
substitutes the crew member's stored baseline for HR/HRV/SpO2/temperature
before evaluating, so a disconnected probe cannot fabricate a cardiac alert.
A manually reported symptom still raises the alert.

## On-device behaviour

- Runs every 200 ms alongside the rule engine.
- Measured inference time on the ESP32-S3: **41 µs** (shown on OLED page 9).
- No heap allocation during inference; weights and activations are static.
- The diagnostics page reports both the rule score and the model score.

## Intended use

- Educational and hackathon decision support for astronaut health monitoring.
- Showing on-device int8 inference on an ESP32-S3.
- Showing how physiological, mission-context and profile signals can be combined into transparent risk bands.

## Out-of-scope use

- Clinical diagnosis, treatment decisions, or real astronaut medical operations.
- Heart-attack detection. The cardiac output is a risk flag, not a diagnosis.
- Replacing a dosimeter, bone-density scan, blood panel, or physician assessment.

## Quantisation

- Symmetric int8 weight quantisation, per tensor.
- Asymmetric int8 activations (`zero_point = -128`).
- Int32 accumulation with float rescaling at layer boundaries.
- Output head evaluated in float with sigmoid.

## Fusion with rules

The deployed score is:

```
final = 0.55 × rule_score + 0.45 × model_score
```

The cardiac-event output is the maximum of the rule and model estimates, with a
manual symptom flag forcing at least 0.55. The OLED diagnostics page shows both
rule and model values so the result is never a black box.

Cabin CO2 is applied **after** this fusion, only to immune and sleep, and is
never applied to radiation. This keeps the model semantics intact while still
letting the environmental sensor influence the displayed risk.

## Limitations

- No real-world validation, calibration, or bias audit.
- Features such as bone density and radiation are modelled/inputs, not measured.
- SpO2 and temperature are sensor estimates with their own error.
- MPU6050 activity, steps, exercise and sleep are behavioural estimates, not clinical sleep staging.
- SCD30/SCD40 CO2 is an air-quality measurement, not a radiation measurement.
- Rule weights and thresholds are illustrative, not clinical guidelines.
- The model has no notion of medical history beyond a binary `known_cvd` flag.

## Ethical considerations

- Must be labelled a research prototype in any publication.
- Do not present the cardiac output as heart-attack detection.
- Sex and race are not used as risk multipliers.
- Any real deployment would require ethics review, clinical validation, and regulatory approval.
