#include "CO2Monitor.h"

#include <math.h>
#include <string.h>

namespace {

// Sensirion CRC-8: poly 0x31, init 0xFF, no final XOR.
uint8_t sensirionCrc8(const uint8_t *data, uint8_t length) {
  uint8_t crc = 0xFF;
  for (uint8_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
    }
  }
  return crc;
}

}  // namespace

bool CO2Monitor::begin(TwoWire &wire) {
  wire_ = &wire;
  type_ = TYPE_NONE;
  reading_ = CO2Reading();

  // SCD40/SCD41 live at 0x62. Probe it first so a board with only an SCD30
  // at 0x61 still falls through cleanly.
  if (probe(SCD4X_ADDR)) {
    type_ = TYPE_SCD4X;
    strncpy(reading_.sensor, "SCD4x", sizeof(reading_.sensor) - 1);

    // Stop any measurement left running from a previous reset, then start.
    sendCommand(SCD4X_ADDR, SCD4X_CMD_STOP);
    delay(500);
    sendCommand(SCD4X_ADDR, SCD4X_CMD_START);
    delay(5);

    reading_.ready = true;
    lastPollMs_ = millis();
    setStatus("STARTING");
    return true;
  }

  if (probe(SCD30_ADDR)) {
    type_ = TYPE_SCD30;
    strncpy(reading_.sensor, "SCD30", sizeof(reading_.sensor) - 1);

    // Start continuous measurement with no pressure compensation, then set
    // a 2-second measurement interval.
    sendCommandWithArg(SCD30_ADDR, SCD30_CMD_START, 0);
    sendCommandWithArg(SCD30_ADDR, SCD30_CMD_INTERVAL, 2);

    reading_.ready = true;
    lastPollMs_ = millis();
    setStatus("STARTING");
    return true;
  }

  reading_.ready = false;
  strncpy(reading_.sensor, "NONE", sizeof(reading_.sensor) - 1);
  setStatus("NOT FOUND");
  lastRetryMs_ = millis();
  return false;
}

void CO2Monitor::update() {
  const uint32_t now = millis();

  if (type_ == TYPE_NONE) {
    if (wire_ != nullptr && (now - lastRetryMs_) >= RETRY_MS) {
      lastRetryMs_ = now;
      begin(*wire_);
    }
    return;
  }

  const uint32_t period =
      (type_ == TYPE_SCD30) ? SCD30_POLL_MS : SCD4X_POLL_MS;
  if (now - lastPollMs_ < period) return;
  lastPollMs_ = now;

  const bool ok = (type_ == TYPE_SCD30) ? readSCD30() : readSCD4X();
  if (!ok) {
    if (reading_.valid && (now - reading_.lastUpdateMs) > STALE_MS) {
      reading_.valid = false;
      setStatus("STALE");
    }
    return;
  }

  reading_.valid = true;
  reading_.lastUpdateMs = now;
  setStatus("OK");
}

bool CO2Monitor::probe(uint8_t address) {
  if (wire_ == nullptr) return false;
  wire_->beginTransmission(address);
  return wire_->endTransmission() == 0;
}

bool CO2Monitor::sendCommand(uint8_t address, uint16_t command) {
  if (wire_ == nullptr) return false;
  wire_->beginTransmission(address);
  wire_->write((uint8_t)(command >> 8));
  wire_->write((uint8_t)(command & 0xFF));
  return wire_->endTransmission() == 0;
}

bool CO2Monitor::sendCommandWithArg(uint8_t address, uint16_t command,
                                    uint16_t argument) {
  if (wire_ == nullptr) return false;
  uint8_t payload[2] = {(uint8_t)(argument >> 8), (uint8_t)(argument & 0xFF)};

  wire_->beginTransmission(address);
  wire_->write((uint8_t)(command >> 8));
  wire_->write((uint8_t)(command & 0xFF));
  wire_->write(payload[0]);
  wire_->write(payload[1]);
  wire_->write(sensirionCrc8(payload, 2));
  return wire_->endTransmission() == 0;
}

bool CO2Monitor::readBytes(uint8_t address, uint8_t *buffer, uint8_t length) {
  if (wire_ == nullptr || buffer == nullptr || length == 0) return false;
  if (wire_->requestFrom((int)address, (int)length) != length) return false;
  for (uint8_t i = 0; i < length; ++i) buffer[i] = wire_->read();
  return true;
}

bool CO2Monitor::scd30DataReady() {
  uint8_t buffer[3];
  if (!sendCommand(SCD30_ADDR, SCD30_CMD_DATA_READY)) return false;
  delay(4);
  if (!readBytes(SCD30_ADDR, buffer, sizeof(buffer))) return false;
  if (sensirionCrc8(buffer, 2) != buffer[2]) return false;
  return ((uint16_t)((buffer[0] << 8) | buffer[1])) != 0;
}

bool CO2Monitor::scd4xDataReady() {
  uint8_t buffer[3];
  if (!sendCommand(SCD4X_ADDR, SCD4X_CMD_DATA_READY)) return false;
  delay(1);
  if (!readBytes(SCD4X_ADDR, buffer, sizeof(buffer))) return false;
  if (sensirionCrc8(buffer, 2) != buffer[2]) return false;
  const uint16_t status = (uint16_t)((buffer[0] << 8) | buffer[1]);
  return (status & 0x07FF) != 0;
}

bool CO2Monitor::readSCD30() {
  if (!scd30DataReady()) return false;

  // SCD30 read_measurement returns 18 bytes: three big-endian IEEE-754
  // floats, each sent as two 16-bit words followed by a CRC byte.
  uint8_t buffer[18];
  if (!sendCommand(SCD30_ADDR, SCD30_CMD_READ)) return false;
  delay(4);
  if (!readBytes(SCD30_ADDR, buffer, sizeof(buffer))) return false;

  for (uint8_t i = 0; i < sizeof(buffer); i += 3) {
    if (sensirionCrc8(buffer + i, 2) != buffer[i + 2]) return false;
  }

  const uint32_t co2Bits = ((uint32_t)buffer[0] << 24) |
                           ((uint32_t)buffer[1] << 16) |
                           ((uint32_t)buffer[3] << 8) | (uint32_t)buffer[4];
  const uint32_t tempBits = ((uint32_t)buffer[6] << 24) |
                            ((uint32_t)buffer[7] << 16) |
                            ((uint32_t)buffer[9] << 8) | (uint32_t)buffer[10];
  const uint32_t humBits = ((uint32_t)buffer[12] << 24) |
                           ((uint32_t)buffer[13] << 16) |
                           ((uint32_t)buffer[15] << 8) | (uint32_t)buffer[16];

  float co2 = 0.0f;
  float temperature = 0.0f;
  float humidity = 0.0f;
  memcpy(&co2, &co2Bits, sizeof(co2));
  memcpy(&temperature, &tempBits, sizeof(temperature));
  memcpy(&humidity, &humBits, sizeof(humidity));

  if (!isfinite(co2) || co2 < 0.0f || co2 > 40000.0f) return false;

  reading_.ppm = (uint16_t)(co2 + 0.5f);
  reading_.temperatureC = temperature;
  reading_.humidityPercent = humidity;
  reading_.hasTempHum = true;
  return true;
}

bool CO2Monitor::readSCD4X() {
  if (!scd4xDataReady()) return false;

  // SCD4x read_measurement returns 9 bytes: CO2, temperature and humidity
  // as 16-bit words, each followed by a CRC byte.
  uint8_t buffer[9];
  if (!sendCommand(SCD4X_ADDR, SCD4X_CMD_READ)) return false;
  delay(1);
  if (!readBytes(SCD4X_ADDR, buffer, sizeof(buffer))) return false;

  for (uint8_t i = 0; i < sizeof(buffer); i += 3) {
    if (sensirionCrc8(buffer + i, 2) != buffer[i + 2]) return false;
  }

  const uint16_t co2 = (uint16_t)((buffer[0] << 8) | buffer[1]);
  const uint16_t rawTemp = (uint16_t)((buffer[3] << 8) | buffer[4]);
  const uint16_t rawHum = (uint16_t)((buffer[6] << 8) | buffer[7]);

  reading_.ppm = co2;
  reading_.temperatureC = -45.0f + 175.0f * (float)rawTemp / 65535.0f;
  reading_.humidityPercent = 100.0f * (float)rawHum / 65535.0f;
  reading_.hasTempHum = true;
  return true;
}

const char *CO2Monitor::airQualityText(uint16_t ppm) {
  if (ppm < 800) return "GOOD";
  if (ppm < 1000) return "ELEVATED";
  if (ppm < 1500) return "POOR";
  return "VERY POOR";
}

void CO2Monitor::setStatus(const char *text) {
  strncpy(reading_.status, text ? text : "", sizeof(reading_.status) - 1);
  reading_.status[sizeof(reading_.status) - 1] = '\0';
}
