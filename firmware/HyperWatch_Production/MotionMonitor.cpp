#include "MotionMonitor.h"

#include <math.h>
#include <string.h>

MotionMonitor::MotionMonitor() {
  setStatus("NOT STARTED");
}

bool MotionMonitor::begin(TwoWire &wire, uint8_t address) {
  wire_ = &wire;
  addr_ = address;

  if (!detect()) {
    const uint8_t alternate = (address == 0x68) ? 0x69 : 0x68;
    addr_ = alternate;
    if (!detect()) {
      reading_.ready = false;
      setStatus("NOT FOUND");
      lastRetryMs_ = millis();
      return false;
    }
  }

  // Wake the device and put it on the X-gyro PLL for a stable clock.
  writeReg(REG_PWR_MGMT_1, 0x00);
  delay(50);
  writeReg(REG_SMPLRT_DIV, 0x04);   // 1 kHz / (1 + 4) = 200 Hz
  writeReg(REG_CONFIG, 0x03);       // 44 Hz digital low-pass
  writeReg(REG_ACCEL_CONFIG, 0x00); // +/-2 g, 16384 LSB/g
  writeReg(REG_PWR_MGMT_1, 0x01);

  reading_.ready = true;
  reading_.valid = false;
  primed_ = false;
  gravityEma_ = 1.0f;
  activityEma_ = 0.0f;
  lastSampleMs_ = millis();
  minuteStartMs_ = millis();
  setStatus("READY");
  return true;
}

bool MotionMonitor::detect() {
  uint8_t who = 0;
  if (!readBytes(REG_WHO_AM_I, &who, 1)) return false;
  // 0x68 = MPU6050, 0x70 = MPU6500, 0x71/0x73 = MPU9250 variants.
  return who == 0x68 || who == 0x70 || who == 0x71 || who == 0x73 ||
         who == 0x98;
}

void MotionMonitor::update() {
  const uint32_t now = millis();

  if (!reading_.ready) {
    if (now - lastRetryMs_ >= RETRY_MS) {
      lastRetryMs_ = now;
      begin(*wire_, addr_);
    }
    return;
  }

  if (now - lastSampleMs_ < SAMPLE_MS) return;
  lastSampleMs_ = now;

  uint8_t buffer[14];
  if (!readBytes(REG_ACCEL_XOUT_H, buffer, sizeof(buffer))) {
    reading_.valid = false;
    setStatus("READ ERR");
    return;
  }

  const int16_t ax = (int16_t)((buffer[0] << 8) | buffer[1]);
  const int16_t ay = (int16_t)((buffer[2] << 8) | buffer[3]);
  const int16_t az = (int16_t)((buffer[4] << 8) | buffer[5]);
  const float magnitude =
      sqrtf((float)ax * ax + (float)ay * ay + (float)az * az) /
      ACCEL_LSB_PER_G;

  if (!primed_) {
    gravityEma_ = magnitude;
    primed_ = true;
  }

  const float dynamic = magnitude - gravityEma_;
  gravityEma_ += GRAVITY_SMOOTH * dynamic;

  const float instant = constrain(fabsf(dynamic) * ACTIVITY_GAIN, 0.0f, 100.0f);
  activityEma_ += ACTIVITY_SMOOTH * (instant - activityEma_);
  if (activityEma_ < 0.0f) activityEma_ = 0.0f;

  reading_.accelG = magnitude;
  reading_.activityIndex = activityEma_;
  reading_.valid = true;
  setStatus("OK");

  // Step detection: a peak in the gravity-corrected magnitude, with
  // hysteresis and a refractory period so one footfall counts once.
  if (fabsf(dynamic) > STEP_THRESHOLD_G && stepArmed_ &&
      (now - lastStepMs_) >= STEP_REFRACTORY_MS) {
    ++reading_.steps;
    lastStepMs_ = now;
    stepArmed_ = false;
  } else if (fabsf(dynamic) < STEP_RELEASE_G) {
    stepArmed_ = true;
  }

  minuteSum_ += activityEma_;
  ++minuteSamples_;
  if (now - minuteStartMs_ >= 60000UL) {
    closeMinute(now);
  }
}

void MotionMonitor::closeMinute(uint32_t now) {
  const float average =
      (minuteSamples_ > 0) ? (minuteSum_ / (float)minuteSamples_) : 0.0f;
  const uint8_t level = (uint8_t)constrain(average, 0.0f, 100.0f);

  ring_[minuteIndex_] = level;
  sleepRing_[minuteIndex_] = 0;

  if (average < REST_THRESHOLD) {
    if (restRun_ < 60000) ++restRun_;
  } else {
    // A rest run only becomes sleep once it is long enough. Mark the
    // minutes that just ended, then start a new run.
    if (restRun_ >= SLEEP_RUN_MINUTES) {
      for (uint16_t k = 1; k <= restRun_; ++k) {
        const uint16_t index =
            (uint16_t)((minuteIndex_ + MINUTES_PER_DAY - k) % MINUTES_PER_DAY);
        sleepRing_[index] = 1;
      }
    }
    restRun_ = 0;
  }

  minuteIndex_ = (uint16_t)((minuteIndex_ + 1) % MINUTES_PER_DAY);
  if (minuteCount_ < MINUTES_PER_DAY) ++minuteCount_;

  minuteSum_ = 0.0f;
  minuteSamples_ = 0;
  minuteStartMs_ = now;

  reading_.activityAvg1Min = average;
  reading_.resting = (average < REST_THRESHOLD);

  recomputeTotals();
}

void MotionMonitor::recomputeTotals() {
  uint16_t exercise = 0;
  uint16_t sleep = 0;

  for (uint16_t i = 0; i < MINUTES_PER_DAY; ++i) {
    if (ring_[i] >= EXERCISE_THRESHOLD) ++exercise;
    if (sleepRing_[i]) ++sleep;
  }

  // Include a rest run that is still in progress so the display updates
  // during the night rather than only after the crew wakes up.
  if (restRun_ >= SLEEP_RUN_MINUTES) sleep += restRun_;
  if (sleep > MAX_SLEEP_MINUTES) sleep = MAX_SLEEP_MINUTES;

  reading_.exerciseMinutes24h = exercise;
  reading_.sleepMinutes24h = sleep;
  reading_.sleepHours24h = (float)sleep / 60.0f;
}

void MotionMonitor::seedSteps(uint32_t steps) {
  if (steps > reading_.steps) reading_.steps = steps;
}

void MotionMonitor::resetDaily() {
  memset(ring_, 0, sizeof(ring_));
  memset(sleepRing_, 0, sizeof(sleepRing_));
  minuteIndex_ = 0;
  minuteCount_ = 0;
  restRun_ = 0;
  minuteSum_ = 0.0f;
  minuteSamples_ = 0;
  reading_.steps = 0;
  reading_.exerciseMinutes24h = 0;
  reading_.sleepMinutes24h = 0;
  reading_.sleepHours24h = 0.0f;
  reading_.activityAvg1Min = 0.0f;
  reading_.resting = false;
}

bool MotionMonitor::writeReg(uint8_t reg, uint8_t value) {
  if (wire_ == nullptr) return false;
  wire_->beginTransmission(addr_);
  wire_->write(reg);
  wire_->write(value);
  return wire_->endTransmission() == 0;
}

bool MotionMonitor::readBytes(uint8_t reg, uint8_t *buffer, uint8_t length) {
  if (wire_ == nullptr || buffer == nullptr || length == 0) return false;

  wire_->beginTransmission(addr_);
  wire_->write(reg);
  if (wire_->endTransmission(false) != 0) return false;

  if (wire_->requestFrom((int)addr_, (int)length) != length) return false;
  for (uint8_t i = 0; i < length; ++i) buffer[i] = wire_->read();
  return true;
}

void MotionMonitor::setStatus(const char *text) {
  strncpy(reading_.status, text ? text : "", sizeof(reading_.status) - 1);
  reading_.status[sizeof(reading_.status) - 1] = '\0';
}
