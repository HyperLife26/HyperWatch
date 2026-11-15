#pragma once

#include <Arduino.h>
#include <Wire.h>
#include "MAX30105.h"

// Validated output from the MAX30102 processing pipeline.
// HR is produced by the custom autocorrelation/beat-interval estimator.
// SpO2 is an experimental estimate from the SparkFun/Maxim routine and is
// not device-calibrated or suitable for clinical decisions.
struct PPGReading {
  uint32_t irRaw = 0;
  uint32_t redRaw = 0;
  uint32_t windowEndMillis = 0;

  float heartRateBpm = 0.0f;
  float spo2Percent = 0.0f;
  float irPiPercent = 0.0f;
  float redPiPercent = 0.0f;
  float pulseCorrelation = 0.0f;
  float hrPeriodicity = 0.0f;
  float hrIntervalSamples = 0.0f;

  uint8_t qualityScore = 0;

  bool sensorReady = false;
  bool windowReady = false;
  bool fingerPresent = false;
  bool signalGood = false;
  bool hrValid = false;
  bool spo2Valid = false;

  // Set only when a newly analysed window has passed stability checks.
  // Consumers should update their models only when these flags are true.
  bool hrFresh = false;
  bool spo2Fresh = false;

  char sensorStatus[16] = "NOT STARTED";
  char signalStatus[20] = "STARTING";
  char hrStatus[16] = "WAIT";
  char spo2Status[16] = "WAIT";
};

class PPGMonitor {
 public:
  PPGMonitor();

  // The caller must initialise Wire with the desired SDA/SCL pins first.
  // Example: Wire.begin(8, 9); Wire.setClock(400000); ppg.begin(Wire);
  bool begin(TwoWire &wire = Wire);

  // Call frequently from loop(); this method is non-blocking during normal
  // operation and performs FIFO acquisition and periodic recovery attempts.
  void update();

  // Returns true once for every analysed window or contact/sensor-state change.
  // The returned snapshot contains hrFresh/spo2Fresh for model ingestion.
  bool takeUpdate(PPGReading &out);

  const PPGReading &getReading() const;

  // Copies up to `points` evenly-spaced samples of the current filtered IR
  // waveform. Returns false until at least two samples are available.
  bool copyWaveform(float *destination, uint8_t points) const;

  // Clears contact/window/estimate history without reinitialising the sensor.
  void reset();

 private:
  static constexpr uint16_t HR_WINDOW_LEN = 200; // 8 seconds at 25 Hz
  static constexpr uint8_t ANALYZE_EVERY_N_SAMPLES = 25;
  static constexpr uint8_t RESULT_HISTORY_LEN = 5;

  static constexpr float FS_HZ = 25.0f;
  static constexpr float HR_MIN_BPM = 40.0f;
  static constexpr float HR_MAX_BPM = 180.0f;
  static constexpr int MIN_PEAK_SPACING = 8;

  static constexpr uint32_t CONTACT_ON_IR = 8000UL;
  static constexpr uint32_t CONTACT_OFF_IR = 6000UL;
  static constexpr uint32_t ADC_CLIP_LEVEL = 260000UL;
  static constexpr float MIN_IR_PI_PERCENT = 0.025f;
  static constexpr float MAX_IR_PI_PERCENT = 8.0f;
  static constexpr float MIN_IR_AC_RMS = 2.0f;

  static constexpr uint8_t LED_BRIGHTNESS = 50;
  static constexpr uint8_t SAMPLE_AVERAGE = 4;
  static constexpr uint8_t LED_MODE = 2;
  static constexpr uint16_t SENSOR_SAMPLE_RATE = 100;
  static constexpr uint16_t PULSE_WIDTH = 411;
  static constexpr uint16_t ADC_RANGE = 4096;

  static constexpr uint32_t SENSOR_RETRY_MS = 2000UL;
  static constexpr uint32_t SAMPLE_TIMEOUT_MS = 1800UL;

  MAX30105 sensor_;
  TwoWire *wire_ = nullptr;
  PPGReading reading_;
  bool updatePending_ = false;

  uint32_t rawIR_[HR_WINDOW_LEN] = {};
  uint32_t rawRed_[HR_WINDOW_LEN] = {};
  float filteredIR_[HR_WINDOW_LEN] = {};
  float filteredRed_[HR_WINDOW_LEN] = {};
  uint16_t windowCount_ = 0;
  uint8_t samplesSinceAnalysis_ = 0;

  uint32_t lastSampleMillis_ = 0;
  uint32_t lastRetryMillis_ = 0;
  bool filterPrimed_ = false;
  float irHpX_ = 0.0f, irHpY_ = 0.0f, irLpY_ = 0.0f;
  float redHpX_ = 0.0f, redHpY_ = 0.0f, redLpY_ = 0.0f;
  uint8_t contactHighCount_ = 0;
  uint8_t contactLowCount_ = 0;

  float irMean_ = 0.0f;
  float redMean_ = 0.0f;
  float irAcRms_ = 0.0f;
  float redAcRms_ = 0.0f;
  uint16_t clippedSamples_ = 0;

  float hrHistory_[RESULT_HISTORY_LEN] = {};
  uint8_t hrHistoryCount_ = 0;
  uint8_t hrHistoryPos_ = 0;
  float hrPending_ = 0.0f;
  uint8_t hrPendingCount_ = 0;
  bool hrResultValid_ = false;
  float hrResult_ = 0.0f;
  uint8_t hrBadWindows_ = 0;

  float spo2History_[RESULT_HISTORY_LEN] = {};
  uint8_t spo2HistoryCount_ = 0;
  uint8_t spo2HistoryPos_ = 0;
  float spo2Pending_ = 0.0f;
  uint8_t spo2PendingCount_ = 0;
  bool spo2ResultValid_ = false;
  float spo2Result_ = 0.0f;
  uint8_t spo2BadWindows_ = 0;

  float hrPeriodicity_ = 0.0f;
  float hrInterval_ = 0.0f;
  float pulseCorrelation_ = 0.0f;
  float irPiPercent_ = 0.0f;
  float redPiPercent_ = 0.0f;

  bool initSensor();
  bool syncSensorFIFO();
  void setText(char *destination, size_t size, const char *value);
  void clearWindow();
  void clearHRHistory();
  void clearSpO2History();
  void updateFingerPresence(uint32_t ir);
  void primeFilter(uint32_t ir, uint32_t red);
  void addSample(uint32_t ir, uint32_t red);
  float bandpass(float x, float &hpX, float &hpY, float &lpY);
  void calculateMetrics();
  void calculateQuality();
  bool basicSignalUsable();
  bool estimateHR(float &bpmOut, float &periodicityOut, float &intervalOut);
  bool calculateSpO2(float &valueOut);
  void analyzeWindow();
  void appendHRHistory(float value);
  void appendSpO2History(float value);
  void rejectHRWindow();
  void rejectSpO2Window();
  float median(const float *values, uint8_t count) const;
  void sort(float *values, int count) const;
  float normalizedAutocorrelation(const float *values, int count, int lag) const;
  void publishResults();
  void markUpdated();
};
