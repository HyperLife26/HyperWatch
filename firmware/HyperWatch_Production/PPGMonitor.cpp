#include "PPGMonitor.h"
#include <math.h>
#include <string.h>
#include "spo2_algorithm.h"  // SparkFun defines FreqS=25 and BUFFER_SIZE=100

namespace {
  constexpr float HP_ALPHA = 0.888f;
  constexpr float LP_ALPHA = 0.634f;
  constexpr float MAX_IR_MEAN = 250000.0f;
  constexpr float MAX_RED_MEAN = 250000.0f;

  inline float ppgMax(float a, float b) { return (a > b) ? a : b; }
  inline float ppgMin(float a, float b) { return (a < b) ? a : b; }
}

PPGMonitor::PPGMonitor() {
  setText(reading_.sensorStatus, sizeof(reading_.sensorStatus), "NOT STARTED");
  setText(reading_.signalStatus, sizeof(reading_.signalStatus), "STARTING");
  setText(reading_.hrStatus, sizeof(reading_.hrStatus), "WAIT");
  setText(reading_.spo2Status, sizeof(reading_.spo2Status), "WAIT");
}

void PPGMonitor::setText(char *destination, size_t size, const char *value) {
  if (size == 0) return;
  snprintf(destination, size, "%s", value ? value : "");
}

bool PPGMonitor::begin(TwoWire &wire) {
  wire_ = &wire;
  reading_.sensorReady = initSensor();
  updatePending_ = true;
  return reading_.sensorReady;
}

bool PPGMonitor::initSensor() {
  if (wire_ == nullptr) {
    setText(reading_.sensorStatus, sizeof(reading_.sensorStatus), "NO I2C BUS");
    reading_.sensorReady = false;
    return false;
  }

  if (!sensor_.begin(*wire_, I2C_SPEED_STANDARD)) {
    setText(reading_.sensorStatus, sizeof(reading_.sensorStatus), "NOT FOUND");
    reading_.sensorReady = false;
    lastRetryMillis_ = millis();
    return false;
  }

  sensor_.setup(
    LED_BRIGHTNESS,
    SAMPLE_AVERAGE,
    LED_MODE,
    SENSOR_SAMPLE_RATE,
    PULSE_WIDTH,
    ADC_RANGE
  );
  sensor_.setPulseAmplitudeRed(LED_BRIGHTNESS);
  sensor_.setPulseAmplitudeIR(LED_BRIGHTNESS);
  sensor_.setPulseAmplitudeGreen(0);
  sensor_.clearFIFO();

  if (!syncSensorFIFO()) {
    setText(reading_.sensorStatus, sizeof(reading_.sensorStatus), "NO DATA");
    reading_.sensorReady = false;
    lastRetryMillis_ = millis();
    return false;
  }

  reading_.sensorReady = true;
  setText(reading_.sensorStatus, sizeof(reading_.sensorStatus), "READY");
  lastSampleMillis_ = millis();
  contactHighCount_ = 0;
  contactLowCount_ = 0;
  reading_.fingerPresent = false;
  clearWindow();
  markUpdated();
  return true;
}

bool PPGMonitor::syncSensorFIFO() {
  const uint32_t start = millis();
  while ((uint32_t)(millis() - start) < 1000UL) {
    sensor_.check();
    if (sensor_.available() > 0) {
      // Discard the first software-FIFO entry so subsequent tail reads begin
      // on a newly populated sample, as in the tested standalone sketch.
      sensor_.nextSample();
      return true;
    }
    delay(2);
  }
  return false;
}

void PPGMonitor::update() {
  // Freshness flags are one-update events, not persistent state.
  reading_.hrFresh = false;
  reading_.spo2Fresh = false;

  if (!reading_.sensorReady) {
    if (wire_ != nullptr && (uint32_t)(millis() - lastRetryMillis_) >= SENSOR_RETRY_MS) {
      initSensor();
    }
    return;
  }

  sensor_.check();
  while (sensor_.available() > 0) {
    const uint32_t ir = sensor_.getFIFOIR();
    const uint32_t red = sensor_.getFIFORed();
    sensor_.nextSample();

    reading_.irRaw = ir;
    reading_.redRaw = red;
    lastSampleMillis_ = millis();

    updateFingerPresence(ir);
    if (reading_.fingerPresent) addSample(ir, red);
  }

  if ((uint32_t)(millis() - lastSampleMillis_) > SAMPLE_TIMEOUT_MS) {
    reading_.sensorReady = false;
    reading_.fingerPresent = false;
    setText(reading_.sensorStatus, sizeof(reading_.sensorStatus), "TIMEOUT");
    clearWindow();
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "SENSOR TIMEOUT");
    markUpdated();
    lastRetryMillis_ = millis();
  }
}

bool PPGMonitor::takeUpdate(PPGReading &out) {
  if (!updatePending_) return false;
  out = reading_;
  updatePending_ = false;
  return true;
}

const PPGReading &PPGMonitor::getReading() const {
  return reading_;
}

bool PPGMonitor::copyWaveform(float *destination, uint8_t points) const {
  if (destination == nullptr || points < 2 || windowCount_ < 2) return false;
  for (uint8_t i = 0; i < points; ++i) {
    const uint32_t index = ((uint32_t)i * (windowCount_ - 1U)) / (points - 1U);
    destination[i] = filteredIR_[index];
  }
  return true;
}

void PPGMonitor::reset() {
  reading_.fingerPresent = false;
  contactHighCount_ = 0;
  contactLowCount_ = 0;
  clearWindow();
  setText(reading_.signalStatus, sizeof(reading_.signalStatus), "PLACE FINGER");
  markUpdated();
}

void PPGMonitor::markUpdated() {
  updatePending_ = true;
}

void PPGMonitor::clearHRHistory() {
  memset(hrHistory_, 0, sizeof(hrHistory_));
  hrHistoryCount_ = 0;
  hrHistoryPos_ = 0;
  hrPending_ = 0.0f;
  hrPendingCount_ = 0;
  hrResultValid_ = false;
  hrResult_ = 0.0f;
  hrBadWindows_ = 0;
  setText(reading_.hrStatus, sizeof(reading_.hrStatus), "WAIT");
}

void PPGMonitor::clearSpO2History() {
  memset(spo2History_, 0, sizeof(spo2History_));
  spo2HistoryCount_ = 0;
  spo2HistoryPos_ = 0;
  spo2Pending_ = 0.0f;
  spo2PendingCount_ = 0;
  spo2ResultValid_ = false;
  spo2Result_ = 0.0f;
  spo2BadWindows_ = 0;
  setText(reading_.spo2Status, sizeof(reading_.spo2Status), "WAIT");
}

void PPGMonitor::clearWindow() {
  windowCount_ = 0;
  samplesSinceAnalysis_ = 0;
  memset(rawIR_, 0, sizeof(rawIR_));
  memset(rawRed_, 0, sizeof(rawRed_));
  memset(filteredIR_, 0, sizeof(filteredIR_));
  memset(filteredRed_, 0, sizeof(filteredRed_));
  filterPrimed_ = false;
  irHpX_ = irHpY_ = irLpY_ = 0.0f;
  redHpX_ = redHpY_ = redLpY_ = 0.0f;
  irMean_ = redMean_ = irAcRms_ = redAcRms_ = 0.0f;
  clippedSamples_ = 0;
  hrPeriodicity_ = hrInterval_ = pulseCorrelation_ = 0.0f;
  irPiPercent_ = redPiPercent_ = 0.0f;
  clearHRHistory();
  clearSpO2History();

  reading_.windowReady = false;
  reading_.windowEndMillis = 0;
  reading_.signalGood = false;
  reading_.hrValid = false;
  reading_.spo2Valid = false;
  reading_.hrFresh = false;
  reading_.spo2Fresh = false;
  reading_.heartRateBpm = 0.0f;
  reading_.spo2Percent = 0.0f;
  reading_.qualityScore = 0;
  reading_.irPiPercent = 0.0f;
  reading_.redPiPercent = 0.0f;
  reading_.pulseCorrelation = 0.0f;
  reading_.hrPeriodicity = 0.0f;
  reading_.hrIntervalSamples = 0.0f;
  setText(reading_.signalStatus, sizeof(reading_.signalStatus),
          reading_.fingerPresent ? "COLLECTING 8S" : "PLACE FINGER");
  markUpdated();
}

void PPGMonitor::updateFingerPresence(uint32_t ir) {
  const bool wasPresent = reading_.fingerPresent;

  if (!reading_.fingerPresent) {
    contactLowCount_ = 0;
    if (ir >= CONTACT_ON_IR) {
      if (contactHighCount_ < 5) ++contactHighCount_;
      if (contactHighCount_ >= 3) reading_.fingerPresent = true;
    } else {
      contactHighCount_ = 0;
    }
  } else {
    contactHighCount_ = 0;
    if (ir <= CONTACT_OFF_IR) {
      if (contactLowCount_ < 8) ++contactLowCount_;
      if (contactLowCount_ >= 6) reading_.fingerPresent = false;
    } else {
      contactLowCount_ = 0;
    }
  }

  if (reading_.fingerPresent != wasPresent) {
    clearWindow();
    setText(reading_.signalStatus, sizeof(reading_.signalStatus),
            reading_.fingerPresent ? "COLLECTING 8S" : "PLACE FINGER");
    markUpdated();
  }
}

void PPGMonitor::primeFilter(uint32_t ir, uint32_t red) {
  irHpX_ = (float)ir;
  irHpY_ = 0.0f;
  irLpY_ = 0.0f;
  redHpX_ = (float)red;
  redHpY_ = 0.0f;
  redLpY_ = 0.0f;
  filterPrimed_ = true;
}

float PPGMonitor::bandpass(float x, float &hpX, float &hpY, float &lpY) {
  const float hp = HP_ALPHA * (hpY + x - hpX);
  hpX = x;
  hpY = hp;
  lpY += LP_ALPHA * (hp - lpY);
  return lpY;
}

void PPGMonitor::addSample(uint32_t ir, uint32_t red) {
  if (!filterPrimed_) primeFilter(ir, red);

  const float irF = bandpass((float)ir, irHpX_, irHpY_, irLpY_);
  const float redF = bandpass((float)red, redHpX_, redHpY_, redLpY_);

  if (windowCount_ < HR_WINDOW_LEN) {
    rawIR_[windowCount_] = ir;
    rawRed_[windowCount_] = red;
    filteredIR_[windowCount_] = irF;
    filteredRed_[windowCount_] = redF;
    ++windowCount_;

    if (windowCount_ == HR_WINDOW_LEN) {
      samplesSinceAnalysis_ = 0;
      analyzeWindow();
    }
    return;
  }

  memmove(rawIR_, rawIR_ + 1, (HR_WINDOW_LEN - 1U) * sizeof(rawIR_[0]));
  memmove(rawRed_, rawRed_ + 1, (HR_WINDOW_LEN - 1U) * sizeof(rawRed_[0]));
  memmove(filteredIR_, filteredIR_ + 1, (HR_WINDOW_LEN - 1U) * sizeof(filteredIR_[0]));
  memmove(filteredRed_, filteredRed_ + 1, (HR_WINDOW_LEN - 1U) * sizeof(filteredRed_[0]));
  rawIR_[HR_WINDOW_LEN - 1U] = ir;
  rawRed_[HR_WINDOW_LEN - 1U] = red;
  filteredIR_[HR_WINDOW_LEN - 1U] = irF;
  filteredRed_[HR_WINDOW_LEN - 1U] = redF;

  if (++samplesSinceAnalysis_ >= ANALYZE_EVERY_N_SAMPLES) {
    samplesSinceAnalysis_ = 0;
    analyzeWindow();
  }
}

void PPGMonitor::calculateMetrics() {
  uint64_t sumIR = 0;
  uint64_t sumRed = 0;
  clippedSamples_ = 0;
  float meanFilteredIR = 0.0f;
  float meanFilteredRed = 0.0f;

  for (uint16_t i = 0; i < HR_WINDOW_LEN; ++i) {
    sumIR += rawIR_[i];
    sumRed += rawRed_[i];
    meanFilteredIR += filteredIR_[i];
    meanFilteredRed += filteredRed_[i];
    if (rawIR_[i] >= ADC_CLIP_LEVEL || rawRed_[i] >= ADC_CLIP_LEVEL) ++clippedSamples_;
  }

  irMean_ = (float)sumIR / (float)HR_WINDOW_LEN;
  redMean_ = (float)sumRed / (float)HR_WINDOW_LEN;
  meanFilteredIR /= (float)HR_WINDOW_LEN;
  meanFilteredRed /= (float)HR_WINDOW_LEN;

  float vi = 0.0f;
  float vr = 0.0f;
  float covariance = 0.0f;
  for (uint16_t i = 0; i < HR_WINDOW_LEN; ++i) {
    const float a = filteredIR_[i] - meanFilteredIR;
    const float b = filteredRed_[i] - meanFilteredRed;
    vi += a * a;
    vr += b * b;
    covariance += a * b;
  }

  irAcRms_ = sqrtf(vi / (float)HR_WINDOW_LEN);
  redAcRms_ = sqrtf(vr / (float)HR_WINDOW_LEN);
  irPiPercent_ = (irMean_ > 0.0f) ? (100.0f * irAcRms_ / irMean_) : 0.0f;
  redPiPercent_ = (redMean_ > 0.0f) ? (100.0f * redAcRms_ / redMean_) : 0.0f;
  pulseCorrelation_ = (vi > 0.0f && vr > 0.0f) ? covariance / sqrtf(vi * vr) : 0.0f;

  reading_.irPiPercent = irPiPercent_;
  reading_.redPiPercent = redPiPercent_;
  reading_.pulseCorrelation = pulseCorrelation_;
}

void PPGMonitor::calculateQuality() {
  uint32_t minIR = UINT32_MAX;
  uint32_t maxIR = 0;
  uint32_t minRed = UINT32_MAX;
  uint32_t maxRed = 0;
  for (uint16_t i = 0; i < HR_WINDOW_LEN; ++i) {
    if (rawIR_[i] < minIR) minIR = rawIR_[i];
    if (rawIR_[i] > maxIR) maxIR = rawIR_[i];
    if (rawRed_[i] < minRed) minRed = rawRed_[i];
    if (rawRed_[i] > maxRed) maxRed = rawRed_[i];
  }

  const bool contact = reading_.fingerPresent && irMean_ >= CONTACT_ON_IR && irMean_ < MAX_IR_MEAN;
  const bool pulsatile = irAcRms_ >= MIN_IR_AC_RMS &&
                         irPiPercent_ >= MIN_IR_PI_PERCENT &&
                         irPiPercent_ <= MAX_IR_PI_PERCENT;
  const bool redSignal = redMean_ > 1000.0f && redMean_ < MAX_RED_MEAN &&
                         redAcRms_ >= MIN_IR_AC_RMS && redPiPercent_ >= 0.015f;
  const bool notClipped = clippedSamples_ <= 1;

  uint8_t score = 0;
  if (contact) score += 25;
  if (pulsatile) score += 25;
  if (redSignal) score += 15;
  if (notClipped) score += 20;
  if ((maxIR - minIR) >= 800UL) score += 15;

  reading_.qualityScore = score;
  reading_.signalGood = contact && pulsatile && notClipped;
}

bool PPGMonitor::basicSignalUsable() {
  if (!reading_.fingerPresent) {
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "PLACE FINGER");
    return false;
  }
  if (windowCount_ < HR_WINDOW_LEN) {
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "COLLECTING 8S");
    return false;
  }
  if (clippedSamples_ > 1) {
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "SATURATED");
    return false;
  }
  if (irMean_ < (float)CONTACT_ON_IR || irMean_ >= MAX_IR_MEAN) {
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "LOW CONTACT");
    return false;
  }
  if (irAcRms_ < MIN_IR_AC_RMS || irPiPercent_ < MIN_IR_PI_PERCENT) {
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "WEAK PULSE");
    return false;
  }
  if (irPiPercent_ > MAX_IR_PI_PERCENT) {
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "MOTION");
    return false;
  }
  return true;
}

void PPGMonitor::sort(float *values, int count) const {
  for (int i = 1; i < count; ++i) {
    const float key = values[i];
    int j = i - 1;
    while (j >= 0 && values[j] > key) {
      values[j + 1] = values[j];
      --j;
    }
    values[j + 1] = key;
  }
}

float PPGMonitor::median(const float *values, uint8_t count) const {
  if (count == 0) return 0.0f;
  float work[RESULT_HISTORY_LEN];
  if (count > RESULT_HISTORY_LEN) count = RESULT_HISTORY_LEN;
  for (uint8_t i = 0; i < count; ++i) work[i] = values[i];
  sort(work, count);
  if (count & 1U) return work[count / 2U];
  return 0.5f * (work[count / 2U - 1U] + work[count / 2U]);
}

float PPGMonitor::normalizedAutocorrelation(const float *values, int count, int lag) const {
  if (lag <= 0 || lag >= count - 4) return 0.0f;
  float mean = 0.0f;
  for (int i = 0; i < count; ++i) mean += values[i];
  mean /= (float)count;

  float xy = 0.0f, xx = 0.0f, yy = 0.0f;
  const int overlap = count - lag;
  for (int i = 0; i < overlap; ++i) {
    const float a = values[i] - mean;
    const float b = values[i + lag] - mean;
    xy += a * b;
    xx += a * a;
    yy += b * b;
  }
  if (xx < 1e-6f || yy < 1e-6f) return 0.0f;
  return xy / sqrtf(xx * yy);
}

bool PPGMonitor::estimateHR(float &bpmOut, float &periodicityOut, float &intervalOut) {
  float sorted[HR_WINDOW_LEN];
  for (uint16_t i = 0; i < HR_WINDOW_LEN; ++i) sorted[i] = filteredIR_[i];
  sort(sorted, HR_WINDOW_LEN);

  const float q10 = sorted[20];
  const float middle = sorted[HR_WINDOW_LEN / 2U];
  const float q90 = sorted[180];
  const float robustAmplitude = q90 - q10;
  if (robustAmplitude < 3.0f) return false;

  const float positiveSwing = q90 - middle;
  const float negativeSwing = middle - q10;
  const float polarity = (negativeSwing > positiveSwing) ? -1.0f : 1.0f;
  float pulse[HR_WINDOW_LEN];
  for (uint16_t i = 0; i < HR_WINDOW_LEN; ++i) {
    pulse[i] = (filteredIR_[i] - middle) * polarity;
  }

  float ac[HR_WINDOW_LEN] = {};
  for (int lag = 7; lag <= 39; ++lag) {
    ac[lag] = normalizedAutocorrelation(pulse, HR_WINDOW_LEN, lag);
  }

  int chosenLag = -1;
  for (int lag = 8; lag <= 38; ++lag) {
    if (!(ac[lag] > ac[lag - 1] && ac[lag] >= ac[lag + 1])) continue;
    if (ac[lag] < 0.30f) continue;

    float leftTrough = ac[lag - 1];
    float rightTrough = ac[lag + 1];
    for (int j = 2; j <= 3; ++j) {
      if (ac[lag - j] < leftTrough) leftTrough = ac[lag - j];
      if (ac[lag + j] < rightTrough) rightTrough = ac[lag + j];
    }
    const float prominence = ac[lag] - ppgMax(leftTrough, rightTrough);
    if (prominence >= 0.08f) {
      chosenLag = lag;
      break;
    }
  }
  if (chosenLag < 0) return false;

  float refinedLag = (float)chosenLag;
  const float c0 = ac[chosenLag - 1];
  const float c1 = ac[chosenLag];
  const float c2 = ac[chosenLag + 1];
  const float denominator = c0 - 2.0f * c1 + c2;
  if (fabsf(denominator) > 1e-5f) {
    float delta = 0.5f * (c0 - c2) / denominator;
    delta = constrain(delta, -0.5f, 0.5f);
    refinedLag += delta;
  }

  int refractory = (int)(0.55f * refinedLag);
  if (refractory < MIN_PEAK_SPACING) refractory = MIN_PEAK_SPACING;
  const float threshold = 0.18f * robustAmplitude;
  int peaks[32];
  uint8_t peakCount = 0;
  int lastPeak = -1000;

  for (int i = 2; i < (int)HR_WINDOW_LEN - 2; ++i) {
    const bool localMaximum = pulse[i] > pulse[i - 1] && pulse[i] >= pulse[i + 1];
    if (!localMaximum || pulse[i] < threshold) continue;

    float leftValley = pulse[i - 1];
    float rightValley = pulse[i + 1];
    for (int j = 1; j <= 4; ++j) {
      if (pulse[i - j] < leftValley) leftValley = pulse[i - j];
      if (pulse[i + j] < rightValley) rightValley = pulse[i + j];
    }
    const float prominence = pulse[i] - ppgMax(leftValley, rightValley);
    if (prominence < 0.12f * robustAmplitude) continue;

    if (i - lastPeak < refractory) {
      if (peakCount > 0 && pulse[i] > pulse[peaks[peakCount - 1]]) {
        peaks[peakCount - 1] = i;
        lastPeak = i;
      }
      continue;
    }

    if (peakCount < 32) {
      peaks[peakCount++] = i;
      lastPeak = i;
    }
  }

  if (peakCount < 4) return false;

  float intervals[31];
  uint8_t intervalCount = 0;
  for (uint8_t i = 1; i < peakCount; ++i) {
    const float distance = (float)(peaks[i] - peaks[i - 1]);
    if (distance >= 8.0f && distance <= (60.0f * FS_HZ / HR_MIN_BPM)) {
      intervals[intervalCount++] = distance;
    }
  }
  if (intervalCount < 3) return false;

  float sortedIntervals[31];
  for (uint8_t i = 0; i < intervalCount; ++i) sortedIntervals[i] = intervals[i];
  sort(sortedIntervals, intervalCount);
  const float medianInterval = (intervalCount & 1U)
    ? sortedIntervals[intervalCount / 2U]
    : 0.5f * (sortedIntervals[intervalCount / 2U - 1U] + sortedIntervals[intervalCount / 2U]);

  float deviations[31];
  for (uint8_t i = 0; i < intervalCount; ++i) deviations[i] = fabsf(intervals[i] - medianInterval);
  sort(deviations, intervalCount);
  const float mad = (intervalCount & 1U)
    ? deviations[intervalCount / 2U]
    : 0.5f * (deviations[intervalCount / 2U - 1U] + deviations[intervalCount / 2U]);
  if ((mad / medianInterval) > 0.22f) return false;

  if (fabsf(medianInterval - refinedLag) > ppgMax(3.0f, medianInterval * 0.15f)) return false;

  const float peakBpm = 60.0f * FS_HZ / medianInterval;
  const float corrBpm = 60.0f * FS_HZ / refinedLag;
  if (fabsf(peakBpm - corrBpm) > ppgMax(8.0f, peakBpm * 0.12f)) return false;

  const float fused = 0.5f * (peakBpm + corrBpm);
  if (fused < HR_MIN_BPM || fused > HR_MAX_BPM) return false;

  bpmOut = fused;
  periodicityOut = ac[chosenLag];
  intervalOut = medianInterval;
  return true;
}

void PPGMonitor::appendHRHistory(float value) {
  if (!hrResultValid_) {
    if (hrPendingCount_ == 0) {
      hrPending_ = value;
      hrPendingCount_ = 1;
      hrHistory_[0] = value;
      hrHistoryCount_ = 1;
      hrHistoryPos_ = 1;
      setText(reading_.hrStatus, sizeof(reading_.hrStatus), "LOCKING");
      return;
    }

    const float tolerance = ppgMax(8.0f, hrPending_ * 0.15f);
    if (fabsf(value - hrPending_) <= tolerance) {
      if (hrPendingCount_ < RESULT_HISTORY_LEN) {
        hrHistory_[hrPendingCount_] = value;
        ++hrPendingCount_;
        hrHistoryCount_ = hrPendingCount_;
        hrHistoryPos_ = hrHistoryCount_ % RESULT_HISTORY_LEN;
      }
      if (hrPendingCount_ >= 3) {
        hrResult_ = median(hrHistory_, hrHistoryCount_);
        hrResultValid_ = true;
        hrPendingCount_ = 0;
        setText(reading_.hrStatus, sizeof(reading_.hrStatus), "LOCKED");
      } else {
        setText(reading_.hrStatus, sizeof(reading_.hrStatus), "LOCKING");
      }
    } else {
      hrPending_ = value;
      hrPendingCount_ = 1;
      hrHistory_[0] = value;
      hrHistoryCount_ = 1;
      hrHistoryPos_ = 1;
      setText(reading_.hrStatus, sizeof(reading_.hrStatus), "LOCKING");
    }
    return;
  }

  const float tolerance = ppgMax(10.0f, hrResult_ * 0.18f);
  if (fabsf(value - hrResult_) > tolerance) {
    hrResultValid_ = false;
    hrHistoryCount_ = 0;
    hrHistoryPos_ = 0;
    hrPending_ = value;
    hrPendingCount_ = 0;
    appendHRHistory(value);
    setText(reading_.hrStatus, sizeof(reading_.hrStatus), "RELOCK");
    return;
  }

  hrHistory_[hrHistoryPos_] = value;
  hrHistoryPos_ = (uint8_t)((hrHistoryPos_ + 1U) % RESULT_HISTORY_LEN);
  if (hrHistoryCount_ < RESULT_HISTORY_LEN) ++hrHistoryCount_;
  hrResult_ = median(hrHistory_, hrHistoryCount_);
  setText(reading_.hrStatus, sizeof(reading_.hrStatus), "LOCKED");
}

void PPGMonitor::appendSpO2History(float value) {
  if (!spo2ResultValid_) {
    if (spo2PendingCount_ == 0) {
      spo2Pending_ = value;
      spo2PendingCount_ = 1;
      spo2History_[0] = value;
      spo2HistoryCount_ = 1;
      spo2HistoryPos_ = 1;
      setText(reading_.spo2Status, sizeof(reading_.spo2Status), "LOCKING");
      return;
    }

    if (fabsf(value - spo2Pending_) <= 3.0f) {
      if (spo2PendingCount_ < RESULT_HISTORY_LEN) {
        spo2History_[spo2PendingCount_] = value;
        ++spo2PendingCount_;
        spo2HistoryCount_ = spo2PendingCount_;
        spo2HistoryPos_ = spo2HistoryCount_ % RESULT_HISTORY_LEN;
      }
      if (spo2PendingCount_ >= 3) {
        spo2Result_ = median(spo2History_, spo2HistoryCount_);
        spo2ResultValid_ = true;
        spo2PendingCount_ = 0;
        setText(reading_.spo2Status, sizeof(reading_.spo2Status), "EST");
      } else {
        setText(reading_.spo2Status, sizeof(reading_.spo2Status), "LOCKING");
      }
    } else {
      spo2Pending_ = value;
      spo2PendingCount_ = 1;
      spo2History_[0] = value;
      spo2HistoryCount_ = 1;
      spo2HistoryPos_ = 1;
      setText(reading_.spo2Status, sizeof(reading_.spo2Status), "LOCKING");
    }
    return;
  }

  if (fabsf(value - spo2Result_) > 3.5f) {
    spo2ResultValid_ = false;
    spo2HistoryCount_ = 0;
    spo2HistoryPos_ = 0;
    spo2Pending_ = value;
    spo2PendingCount_ = 0;
    appendSpO2History(value);
    setText(reading_.spo2Status, sizeof(reading_.spo2Status), "RELOCK");
    return;
  }

  spo2History_[spo2HistoryPos_] = value;
  spo2HistoryPos_ = (uint8_t)((spo2HistoryPos_ + 1U) % RESULT_HISTORY_LEN);
  if (spo2HistoryCount_ < RESULT_HISTORY_LEN) ++spo2HistoryCount_;
  spo2Result_ = median(spo2History_, spo2HistoryCount_);
  setText(reading_.spo2Status, sizeof(reading_.spo2Status), "EST");
}

void PPGMonitor::rejectHRWindow() {
  if (hrBadWindows_ < 255) ++hrBadWindows_;
  if (hrBadWindows_ >= 2) clearHRHistory();
}

void PPGMonitor::rejectSpO2Window() {
  if (spo2BadWindows_ < 255) ++spo2BadWindows_;
  if (spo2BadWindows_ >= 3) clearSpO2History();
}

bool PPGMonitor::calculateSpO2(float &valueOut) {
  if (redMean_ < 1000.0f || redAcRms_ < MIN_IR_AC_RMS || redPiPercent_ < 0.015f) {
    setText(reading_.spo2Status, sizeof(reading_.spo2Status), "RED WEAK");
    return false;
  }
  if (fabsf(pulseCorrelation_) < 0.35f) {
    setText(reading_.spo2Status, sizeof(reading_.spo2Status), "CHANNELS");
    return false;
  }

  // The Maxim routine's expected input size is the library's BUFFER_SIZE
  // (FreqS * 4 = 100). Keep it separate from our 200-sample HR window.
  uint32_t maximIR[BUFFER_SIZE];
  uint32_t maximRed[BUFFER_SIZE];
  const uint16_t start = HR_WINDOW_LEN - BUFFER_SIZE;
  for (uint16_t i = 0; i < BUFFER_SIZE; ++i) {
    maximIR[i] = rawIR_[start + i];
    maximRed[i] = rawRed_[start + i];
  }

  int32_t maximSpO2 = 0;
  int8_t maximSpO2Valid = 0;
  int32_t ignoredHR = 0;
  int8_t ignoredHRValid = 0;
  maxim_heart_rate_and_oxygen_saturation(
    maximIR, BUFFER_SIZE, maximRed,
    &maximSpO2, &maximSpO2Valid,
    &ignoredHR, &ignoredHRValid
  );

  if (!maximSpO2Valid || maximSpO2 < 70 || maximSpO2 > 100) {
    setText(reading_.spo2Status, sizeof(reading_.spo2Status), "NO VALID O2");
    return false;
  }
  valueOut = (float)maximSpO2;
  return true;
}

void PPGMonitor::publishResults() {
  reading_.windowReady = (windowCount_ >= HR_WINDOW_LEN);
  reading_.hrValid = hrResultValid_;
  reading_.spo2Valid = spo2ResultValid_;
  reading_.heartRateBpm = hrResultValid_ ? hrResult_ : 0.0f;
  reading_.spo2Percent = spo2ResultValid_ ? spo2Result_ : 0.0f;
}

void PPGMonitor::analyzeWindow() {
  reading_.windowEndMillis = millis();
  calculateMetrics();
  calculateQuality();
  reading_.hrFresh = false;
  reading_.spo2Fresh = false;

  if (!basicSignalUsable()) {
    reading_.signalGood = false;
    rejectHRWindow();
    rejectSpO2Window();
    publishResults();
    markUpdated();
    return;
  }

  float hr = 0.0f;
  float periodicity = 0.0f;
  float interval = 0.0f;
  const bool hrOK = estimateHR(hr, periodicity, interval);
  hrPeriodicity_ = periodicity;
  hrInterval_ = interval;
  reading_.hrPeriodicity = periodicity;
  reading_.hrIntervalSamples = interval;

  if (hrOK) {
    hrBadWindows_ = 0;
    appendHRHistory(hr);
    reading_.hrFresh = hrResultValid_;
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "PULSE OK");
  } else {
    rejectHRWindow();
    setText(reading_.signalStatus, sizeof(reading_.signalStatus), "NO PULSE");
  }

  float oxygen = 0.0f;
  if (hrOK && calculateSpO2(oxygen)) {
    spo2BadWindows_ = 0;
    appendSpO2History(oxygen);
    reading_.spo2Fresh = spo2ResultValid_;
  } else {
    rejectSpO2Window();
  }

  publishResults();
  markUpdated();
}
