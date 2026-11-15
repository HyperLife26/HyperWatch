#pragma once

#include <Arduino.h>
#include <Wire.h>

// Raw-I2C driver for the Sensirion SCD30 and SCD4x (SCD40/SCD41) CO2 sensors.
// Autodetects at 0x62 (SCD4x) then 0x61 (SCD30), so either breakout works
// without a compile-time choice or an external library.
//
// A CO2 sensor measures cabin air quality. It is NOT a radiation detector.
struct CO2Reading {
  bool ready = false;
  bool valid = false;
  uint16_t ppm = 0;
  float temperatureC = 0.0f;
  float humidityPercent = 0.0f;
  bool hasTempHum = false;
  char sensor[12] = "NONE";
  char status[16] = "NOT STARTED";
  uint32_t lastUpdateMs = 0;
};

class CO2Monitor {
 public:
  bool begin(TwoWire &wire = Wire);
  void update();

  const CO2Reading &getReading() const { return reading_; }

  static const char *airQualityText(uint16_t ppm);

 private:
  enum SensorType : uint8_t { TYPE_NONE = 0, TYPE_SCD30, TYPE_SCD4X };

  static constexpr uint8_t SCD30_ADDR = 0x61;
  static constexpr uint8_t SCD4X_ADDR = 0x62;

  static constexpr uint16_t SCD30_CMD_START = 0x0010;
  static constexpr uint16_t SCD30_CMD_READ = 0x0300;
  static constexpr uint16_t SCD30_CMD_DATA_READY = 0x0202;
  static constexpr uint16_t SCD30_CMD_INTERVAL = 0x4600;

  static constexpr uint16_t SCD4X_CMD_START = 0x21B1;
  static constexpr uint16_t SCD4X_CMD_STOP = 0x3F86;
  static constexpr uint16_t SCD4X_CMD_READ = 0xEC05;
  static constexpr uint16_t SCD4X_CMD_DATA_READY = 0xE4B8;

  static constexpr uint32_t RETRY_MS = 10000;
  static constexpr uint32_t SCD30_POLL_MS = 2000;
  static constexpr uint32_t SCD4X_POLL_MS = 5000;
  static constexpr uint32_t STALE_MS = 15000;

  TwoWire *wire_ = nullptr;
  CO2Reading reading_;
  SensorType type_ = TYPE_NONE;
  uint32_t lastPollMs_ = 0;
  uint32_t lastRetryMs_ = 0;

  bool probe(uint8_t address);
  bool sendCommand(uint8_t address, uint16_t command);
  bool sendCommandWithArg(uint8_t address, uint16_t command, uint16_t argument);
  bool readBytes(uint8_t address, uint8_t *buffer, uint8_t length);
  bool scd30DataReady();
  bool scd4xDataReady();
  bool readSCD30();
  bool readSCD4X();
  void setStatus(const char *text);
};
