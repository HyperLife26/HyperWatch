#pragma once

#include <Arduino.h>
#include <Wire.h>

// Raw-I2C MPU6050 driver (no external library required).
//
// Outputs:
//   activityIndex  0..100 motion intensity, suitable for Vitals.activityIndex
//   steps          cumulative step count (seeded from NVS on boot)
//   exerciseMinutes24h / sleepHours24h
//                  rolling 24-hour estimates from a 1-minute activity ring.
//                  Sleep is only credited for rest runs of >= 45 minutes,
//                  which avoids counting brief desk sitting as sleep.
//
// The MPU6050 is a consumer motion sensor. These values are behavioural
// estimates, not clinical sleep staging.
struct MotionReading {
  bool ready = false;
  bool valid = false;
  float accelG = 1.0f;
  float activityIndex = 0.0f;
  float activityAvg1Min = 0.0f;
  uint32_t steps = 0;
  uint16_t exerciseMinutes24h = 0;
  uint16_t sleepMinutes24h = 0;
  float sleepHours24h = 0.0f;
  bool resting = false;
  char status[16] = "NOT STARTED";
};

class MotionMonitor {
 public:
  MotionMonitor();

  bool begin(TwoWire &wire = Wire, uint8_t address = 0x68);
  void update();

  const MotionReading &getReading() const { return reading_; }

  // True once enough history exists for the estimate to be meaningful.
  bool hasSleepEstimate() const { return minuteCount_ >= 60; }
  bool hasExerciseEstimate() const { return minuteCount_ >= 30; }

  void seedSteps(uint32_t steps);
  void resetDaily();

 private:
  static constexpr uint16_t MINUTES_PER_DAY = 1440;

  static constexpr uint8_t REG_SMPLRT_DIV = 0x19;
  static constexpr uint8_t REG_CONFIG = 0x1A;
  static constexpr uint8_t REG_ACCEL_CONFIG = 0x1C;
  static constexpr uint8_t REG_ACCEL_XOUT_H = 0x3B;
  static constexpr uint8_t REG_PWR_MGMT_1 = 0x6B;
  static constexpr uint8_t REG_WHO_AM_I = 0x75;

  static constexpr uint32_t SAMPLE_MS = 50;       // 20 Hz
  static constexpr uint32_t RETRY_MS = 5000;
  static constexpr uint32_t STEP_REFRACTORY_MS = 280;
  static constexpr float ACCEL_LSB_PER_G = 16384.0f;  // +/-2 g
  static constexpr float STEP_THRESHOLD_G = 0.12f;
  static constexpr float STEP_RELEASE_G = 0.03f;
  static constexpr float ACTIVITY_GAIN = 250.0f;
  static constexpr float ACTIVITY_SMOOTH = 0.25f;
  static constexpr float GRAVITY_SMOOTH = 0.02f;
  static constexpr float REST_THRESHOLD = 8.0f;
  static constexpr float EXERCISE_THRESHOLD = 30.0f;
  static constexpr uint16_t SLEEP_RUN_MINUTES = 45;
  static constexpr uint16_t MAX_SLEEP_MINUTES = 720;

  TwoWire *wire_ = nullptr;
  uint8_t addr_ = 0x68;
  MotionReading reading_;

  uint32_t lastSampleMs_ = 0;
  uint32_t lastRetryMs_ = 0;
  uint32_t lastStepMs_ = 0;
  uint32_t minuteStartMs_ = 0;

  bool primed_ = false;
  float gravityEma_ = 1.0f;
  float activityEma_ = 0.0f;

  bool stepArmed_ = true;

  float minuteSum_ = 0.0f;
  uint16_t minuteSamples_ = 0;
  uint16_t minuteIndex_ = 0;
  uint16_t minuteCount_ = 0;
  uint16_t restRun_ = 0;

  uint8_t ring_[MINUTES_PER_DAY] = {};
  uint8_t sleepRing_[MINUTES_PER_DAY] = {};

  bool detect();
  bool writeReg(uint8_t reg, uint8_t value);
  bool readBytes(uint8_t reg, uint8_t *buffer, uint8_t length);
  void closeMinute(uint32_t now);
  void recomputeTotals();
  void setStatus(const char *text);
};
