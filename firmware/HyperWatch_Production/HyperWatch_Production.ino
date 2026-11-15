/*
 * HyperWatch Production - real-sensor astronaut health monitor
 * ------------------------------------------------------------
 * ESP32-S3 SuperMini + MAX30102 (PPG) + TMP117 (skin temp) + MPU6050 (motion)
 * + SCD30/SCD40 (CO2) + SH1106 OLED.
 *
 * Pipeline:
 *   MAX30102 -> PPGMonitor -> HR / SpO2 / quality / contact
 *   TMP117   -> skin temperature + slope
 *   MPU6050  -> activity, steps, exercise minutes, sleep/rest estimate
 *   SCD30/40 -> cabin CO2 (air quality; NOT radiation)
 *   profile  -> mission context (phase, day, gravity, radiation, sleep)
 *   rules + int8 quantised MLP -> 5 risk domains + acute cardiac event risk
 *   NVS      -> baselines, bone load, cumulative dose, motion history
 *
 * Short press = next page. Long press = toggle "symptom reported".
 *
 * Research prototype. NOT a medical device and NOT a diagnostic system.
 */

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_TMP117.h>
#include <Adafruit_Sensor.h>
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <HyperWatchCore.h>
#include "PPGMonitor.h"
#include "MotionMonitor.h"
#include "CO2Monitor.h"

// ---------------------------------------------------------------------------
// Hardware
// ---------------------------------------------------------------------------
static constexpr uint8_t I2C_SDA = 8;
static constexpr uint8_t I2C_SCL = 9;
static constexpr uint8_t BUTTON_PIN = 10;
static constexpr uint8_t TMP117_ADDRESS = 0x49;
static constexpr uint32_t I2C_HZ = 100000UL;

U8G2_SH1106_128X64_NONAME_F_HW_I2C display(U8G2_R0, U8X8_PIN_NONE);
Adafruit_TMP117 tmp117;
PPGMonitor ppg;
MotionMonitor motion;
CO2Monitor co2;

// ---------------------------------------------------------------------------
// Core
// ---------------------------------------------------------------------------
astro::MissionContext ctx;
astro::HealthState health;
astro::RiskEngine engine;
astro::StateStore store;
astro::DisplayPages pages;

astro::Vitals vitals;
astro::RiskScores scores;
PPGReading ppgReading;

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
uint8_t page = 0;
bool symptomFlag = false;

float skinTempC = 0.0f;
float tempTrendEmaC = 0.0f;
bool tempTrendInit = false;
float tempSlopeCPerH = 0.0f;
bool tempValid = false;
uint32_t lastTempMs = 0;
uint32_t lastTempSampleMs = 0;

float hrHistory[20] = {};
uint8_t hrCount = 0;
uint8_t hrPos = 0;
float hrvMs = 45.0f;
bool baselineCaptured = false;

uint32_t lastSampleMs = 0;
uint32_t lastDrawMs = 0;
uint32_t lastSerialMs = 0;
uint32_t lastSaveMs = 0;

// Mission clock: 1 mission day per 10 real minutes. Tune for the mission profile.
static constexpr float DAYS_PER_SECOND = 1.0f / 600.0f;
static constexpr uint32_t SAMPLE_MS = 200;
static constexpr uint32_t DRAW_MS = 250;
static constexpr uint32_t SERIAL_MS = 5000;
static constexpr uint32_t SAVE_MS = 30000;
static constexpr uint32_t TEMP_MS = 1000;

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------
bool lastReading = HIGH;
bool stableState = HIGH;
uint32_t lastDebounceMs = 0;
uint32_t pressStartMs = 0;
bool longHandled = false;

static constexpr uint32_t DEBOUNCE_MS = 35;
static constexpr uint32_t LONG_PRESS_MS = 1200;

void pollButton() {
  const bool reading = digitalRead(BUTTON_PIN);
  if (reading != lastReading) {
    lastReading = reading;
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > DEBOUNCE_MS && reading != stableState) {
    stableState = reading;
    if (stableState == LOW) {
      pressStartMs = millis();
      longHandled = false;
    } else {
      const uint32_t held = millis() - pressStartMs;
      if (!longHandled && held >= 60 && held < LONG_PRESS_MS) {
        page = (uint8_t)((page + 1) % astro::DisplayPages::PAGE_COUNT);
      }
    }
  }

  if (stableState == LOW && !longHandled &&
      (millis() - pressStartMs) >= LONG_PRESS_MS) {
    longHandled = true;
    symptomFlag = !symptomFlag;
    Serial.printf("SYMPTOM FLAG -> %s\n", symptomFlag ? "REPORTED" : "CLEAR");
  }
}

// ---------------------------------------------------------------------------
// HRV proxy: standard deviation of recent accepted HR values.
// A true RMSSD needs beat-to-beat intervals; this is a documented proxy.
// ---------------------------------------------------------------------------
void pushHeartRate(float hr) {
  if (hr < 30.0f || hr > 220.0f) return;
  hrHistory[hrPos] = hr;
  hrPos = (uint8_t)((hrPos + 1) % 20);
  if (hrCount < 20) ++hrCount;

  if (hrCount < 5) {
    hrvMs = health.baselineHrv;
    return;
  }

  float mean = 0.0f;
  for (uint8_t i = 0; i < hrCount; ++i) mean += hrHistory[i];
  mean /= (float)hrCount;

  float var = 0.0f;
  for (uint8_t i = 0; i < hrCount; ++i) {
    const float d = hrHistory[i] - mean;
    var += d * d;
  }
  var /= (float)hrCount;
  hrvMs = sqrtf(var);
}

// Capture a personal baseline once 20 accepted HR values are available.
void maybeCaptureBaseline() {
  if (baselineCaptured || hrCount < 20) return;

  float mean = 0.0f;
  for (uint8_t i = 0; i < hrCount; ++i) mean += hrHistory[i];
  mean /= (float)hrCount;

  health.baselineHr = mean;
  health.baselineHrv = hrvMs;
  if (tempValid) health.baselineTempC = skinTempC;
  if (ppgReading.spo2Valid) health.baselineSpo2 = ppgReading.spo2Percent;

  engine.setBaseline(health.baselineHr, health.baselineHrv,
                     health.baselineSpo2, health.baselineTempC);
  baselineCaptured = true;

  Serial.printf("BASELINE CAPTURED: HR %.1f HRV %.1f SpO2 %.1f T %.2f\n",
                health.baselineHr, health.baselineHrv,
                health.baselineSpo2, health.baselineTempC);
}

void readTemperature() {
  sensors_event_t event;
  if (tmp117.getEvent(&event)) {
    const float readingC = event.temperature;
    const uint32_t t = millis();
    if (!tempTrendInit) {
      tempTrendEmaC = readingC;
      tempTrendInit = true;
    } else if (t > lastTempSampleMs) {
      // 10-minute trend. Differentiating 1 s samples would amplify TMP117
      // noise into a permanently saturated slope.
      constexpr float TREND_TAU_H = 10.0f / 60.0f;
      const float hours = (t - lastTempSampleMs) / 3600000.0f;
      if (hours > 0.0f) {
        const float alpha = astro::clampf(hours / (hours + TREND_TAU_H), 0.0f, 1.0f);
        const float instant =
            astro::clampf((readingC - tempTrendEmaC) / TREND_TAU_H, -5.0f, 5.0f);
        tempTrendEmaC += alpha * (readingC - tempTrendEmaC);
        tempSlopeCPerH += 0.05f * (instant - tempSlopeCPerH);
        tempSlopeCPerH = astro::clampf(tempSlopeCPerH, -0.5f, 0.5f);
      }
    }
    lastTempSampleMs = t;
    skinTempC = readingC;
    tempValid = true;
  } else {
    tempValid = false;
  }
}

// High cabin CO2 is an environmental stressor. It is applied after the
// rule+model fusion so the model semantics stay intact.
// It never touches the radiation score: CO2 is not a radiation measurement.
void applyCabinAirQuality(astro::RiskScores &s, uint16_t ppm) {
  if (ppm <= 800) return;

  const float penalty =
      astro::clampf((float)(ppm - 800) / 100.0f, 0.0f, 12.0f);
  s.risk[astro::RISK_IMMUNE] =
      astro::clampf(s.risk[astro::RISK_IMMUNE] + penalty, 0.0f, 100.0f);
  s.risk[astro::RISK_SLEEP] =
      astro::clampf(s.risk[astro::RISK_SLEEP] + penalty * 0.75f, 0.0f, 100.0f);
  s.band[astro::RISK_IMMUNE] = astro::bandFor(s.risk[astro::RISK_IMMUNE]);
  s.band[astro::RISK_SLEEP] = astro::bandFor(s.risk[astro::RISK_SLEEP]);

  s.composite = 0.30f * s.risk[astro::RISK_CARDIO] +
                0.20f * s.risk[astro::RISK_BONE] +
                0.20f * s.risk[astro::RISK_IMMUNE] +
                0.15f * s.risk[astro::RISK_RADIATION] +
                0.15f * s.risk[astro::RISK_SLEEP];
  s.composite = astro::clampf(s.composite, 0.0f, 100.0f);
  if (s.cardiacAlert) s.composite = fmaxf(s.composite, 85.0f);
  s.compositeBand = astro::bandFor(s.composite);
}

// ---------------------------------------------------------------------------
void printJson() {
  const CO2Reading &cr = co2.getReading();
  const MotionReading &mr = motion.getReading();

  Serial.printf(
      "{\"t\":%lu,\"crew\":\"%s\",\"phase\":\"%s\",\"mission_day\":%u,"
      "\"gravity\":%.3f,\"rad_daily\":%.3f,\"rad_cum\":%.2f,"
      "\"hr\":%.1f,\"hrv\":%.1f,\"spo2\":%.1f,\"temp\":%.2f,"
      "\"signal\":{\"finger\":%s,\"quality\":%u,\"sensor\":\"%s\","
      "\"hr_valid\":%s,\"spo2_valid\":%s,\"vitals_valid\":%s},"
      "\"env\":{\"co2\":%u,\"valid\":%s,\"air\":\"%s\",\"temp\":%.1f,"
      "\"rh\":%.1f,\"sensor\":\"%s\"},"
      "\"motion\":{\"valid\":%s,\"activity\":%.1f,\"steps\":%lu,"
      "\"exercise_min\":%u,\"sleep_h\":%.1f,\"resting\":%s},"
      "\"risk\":{\"cardio\":%.1f,\"bone\":%.1f,\"immune\":%.1f,"
      "\"radiation\":%.1f,\"sleep\":%.1f},"
      "\"composite\":%.1f,\"cardiac_prob\":%.2f,\"alert\":%s,"
      "\"symptom\":%s,\"model_us\":%lu}\n",
      (unsigned long)millis(), ctx.crewId, astro::phaseText(ctx.phase),
      (unsigned)ctx.missionDay, ctx.gravityG, ctx.dailyRadiationmSv,
      ctx.cumulativeRadiationmSv, vitals.hr, vitals.hrv, vitals.spo2,
      vitals.skinTempC, vitals.fingerPresent ? "true" : "false",
      (unsigned)ppgReading.qualityScore, ppgReading.sensorStatus,
      ppgReading.hrValid ? "true" : "false",
      ppgReading.spo2Valid ? "true" : "false",
      vitals.valid ? "true" : "false",
      (unsigned)cr.ppm, cr.valid ? "true" : "false",
      CO2Monitor::airQualityText(cr.ppm), cr.temperatureC,
      cr.humidityPercent, cr.sensor, mr.valid ? "true" : "false",
      mr.activityIndex, (unsigned long)mr.steps,
      (unsigned)mr.exerciseMinutes24h, mr.sleepHours24h,
      mr.resting ? "true" : "false",
      scores.risk[astro::RISK_CARDIO], scores.risk[astro::RISK_BONE],
      scores.risk[astro::RISK_IMMUNE], scores.risk[astro::RISK_RADIATION],
      scores.risk[astro::RISK_SLEEP], scores.composite,
      scores.cardiacEventProb, scores.cardiacAlert ? "true" : "false",
      symptomFlag ? "true" : "false",
      (unsigned long)scores.ml.inferenceUs);
}

// ---------------------------------------------------------------------------
// Serial configuration interface (mission context + personal profile).
// Commands are newline-terminated at 115200 baud. Everything is persisted in
// NVS on the next autosave (or immediately for `reset`).
// ---------------------------------------------------------------------------
char cmdBuf[48];
uint8_t cmdLen = 0;

void printHelp() {
  Serial.println("Commands:");
  Serial.println("  help                 this list");
  Serial.println("  status               print current JSON snapshot");
  Serial.println("  crew <id>            e.g. crew ASTRO-07");
  Serial.println("  age <years>          18..80");
  Serial.println("  cvd <0|1>            known cardiovascular disease");
  Serial.println("  phase <0..6>         0 prelaunch, 1 launch, 2 microgravity,");
  Serial.println("                       3 transit, 4 surface, 5 return, 6 postflight");
  Serial.println("  day <n>              mission day");
  Serial.println("  shield <factor>      shielding factor, 0.1..10 (higher = more)");
  Serial.println("  sleep <hours>        sleep in last 24 h, 0..12");
  Serial.println("  exercise <minutes>   exercise in last 24 h, 0..300");
  Serial.println("  motion               MPU6050 activity/steps/sleep state");
  Serial.println("  co2                  SCD30/SCD40 CO2 sensor state");
  Serial.println("  resetmotion          clear the 24 h motion ring");
  Serial.println("  reset                erase NVS and restore defaults");
}

void applyPhase(uint8_t phase) {
  ctx.phase = (astro::MissionPhase)phase;
  ctx.gravityG = astro::defaultGravityForPhase(ctx.phase);
  ctx.dailyRadiationmSv =
      astro::defaultRadiationForPhase(ctx.phase) /
      astro::clampf(ctx.shieldingFactor, 0.1f, 10.0f);
}

void handleCommand(char* line) {
  char* cmd = strtok(line, " \t");
  if (cmd == nullptr) return;
  char* arg = strtok(nullptr, " \t");
  for (char* p = cmd; *p; ++p) *p = (char)tolower((unsigned char)*p);

  if (strcmp(cmd, "help") == 0) {
    printHelp();
  } else if (strcmp(cmd, "status") == 0) {
    printJson();
  } else if (strcmp(cmd, "crew") == 0 && arg != nullptr) {
    strncpy(ctx.crewId, arg, sizeof(ctx.crewId) - 1);
    ctx.crewId[sizeof(ctx.crewId) - 1] = '\0';
    Serial.printf("crew -> %s\n", ctx.crewId);
  } else if (strcmp(cmd, "age") == 0 && arg != nullptr) {
    ctx.age = (uint8_t)astro::clampf((float)atoi(arg), 18.0f, 80.0f);
    Serial.printf("age -> %u\n", (unsigned)ctx.age);
  } else if (strcmp(cmd, "cvd") == 0 && arg != nullptr) {
    ctx.knownCVD = (atoi(arg) != 0);
    Serial.printf("knownCVD -> %s\n", ctx.knownCVD ? "true" : "false");
  } else if (strcmp(cmd, "phase") == 0 && arg != nullptr) {
    const int p = atoi(arg);
    if (p >= 0 && p <= 6) {
      applyPhase((uint8_t)p);
      Serial.printf("phase -> %s, gravity %.3f g, %.2f mSv/day\n",
                    astro::phaseText(ctx.phase), ctx.gravityG,
                    ctx.dailyRadiationmSv);
    } else {
      Serial.println("phase: expected 0..6");
    }
  } else if (strcmp(cmd, "day") == 0 && arg != nullptr) {
    const int d = atoi(arg);
    if (d >= 0 && d <= 65535) {
      ctx.missionDay = (uint16_t)d;
      ctx.isolationDays = ctx.missionDay;
      ctx.dayRemainder = 0.0f;
      Serial.printf("missionDay -> %u\n", (unsigned)ctx.missionDay);
    } else {
      Serial.println("day: expected 0..65535");
    }
  } else if (strcmp(cmd, "shield") == 0 && arg != nullptr) {
    ctx.shieldingFactor = astro::clampf(atof(arg), 0.1f, 10.0f);
    ctx.dailyRadiationmSv =
        astro::defaultRadiationForPhase(ctx.phase) / ctx.shieldingFactor;
    Serial.printf("shielding -> %.2f, %.2f mSv/day\n", ctx.shieldingFactor,
                  ctx.dailyRadiationmSv);
  } else if (strcmp(cmd, "sleep") == 0 && arg != nullptr) {
    ctx.sleepHours24h = astro::clampf(atof(arg), 0.0f, 12.0f);
    Serial.printf("sleep -> %.1f h\n", ctx.sleepHours24h);
  } else if (strcmp(cmd, "exercise") == 0 && arg != nullptr) {
    ctx.exerciseMinutes24h =
        (uint16_t)astro::clampf((float)atoi(arg), 0.0f, 300.0f);
    Serial.printf("exercise -> %u min\n", (unsigned)ctx.exerciseMinutes24h);
  } else if (strcmp(cmd, "motion") == 0) {
    const MotionReading &mr = motion.getReading();
    Serial.printf(
        "motion %s valid=%s act=%.1f steps=%lu exercise=%u min sleep=%.1f h\n",
        mr.status, mr.valid ? "true" : "false", mr.activityIndex,
        (unsigned long)mr.steps, (unsigned)mr.exerciseMinutes24h,
        mr.sleepHours24h);
  } else if (strcmp(cmd, "co2") == 0) {
    const CO2Reading &cr = co2.getReading();
    Serial.printf("co2 %s %s valid=%s ppm=%u temp=%.1f rh=%.1f\n", cr.sensor,
                  cr.status, cr.valid ? "true" : "false", (unsigned)cr.ppm,
                  cr.temperatureC, cr.humidityPercent);
  } else if (strcmp(cmd, "resetmotion") == 0) {
    motion.resetDaily();
    Serial.println("motion ring cleared");
  } else if (strcmp(cmd, "reset") == 0) {
    store.erase();
    ctx = astro::defaultMissionContext();
    health = astro::HealthState();
    engine.begin(health);
    baselineCaptured = false;
    Serial.println("state reset to defaults");
  } else {
    Serial.printf("unknown command: %s (try help)\n", cmd);
  }
}

void pollSerial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen > 0) {
        cmdBuf[cmdLen] = '\0';
        handleCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(400);

  Serial.println();
  Serial.println("==============================================");
  Serial.println(" HYPERWATCH PRODUCTION - REAL SENSORS");
  Serial.println(" MAX30102 + TMP117 + MPU6050 + SCD30/40");
  Serial.println(" SH1106 OLED + int8 quantised model");
  Serial.println("==============================================");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  lastReading = digitalRead(BUTTON_PIN);
  stableState = lastReading;
  lastDebounceMs = millis();

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_HZ);
  Wire.setTimeOut(50);

  display.begin();
  // U8g2 selects 400 kHz for the SH1106 during begin(). The SCD30/SCD4x are
  // 100 kHz parts, so return the shared bus to 100 kHz afterwards.
  Wire.setClock(I2C_HZ);
  display.clearBuffer();
  display.setFont(u8g2_font_6x10_tf);
  display.drawStr(0, 16, "HYPERWATCH");
  display.drawStr(0, 30, "PRODUCTION");
  display.drawStr(0, 44, "init sensors...");
  display.sendBuffer();

  pages.begin(&display);

  if (tmp117.begin(TMP117_ADDRESS, &Wire)) {
    Serial.println("TMP117: OK");
  } else {
    Serial.println("TMP117: FAIL");
  }

  if (ppg.begin(Wire)) {
    Serial.println("MAX30102: OK");
  } else {
    Serial.println("MAX30102: FAIL");
  }

  if (motion.begin(Wire)) {
    Serial.println("MPU6050: OK");
  } else {
    Serial.println("MPU6050: FAIL (check AD0 / wiring)");
  }

  if (co2.begin(Wire)) {
    Serial.printf("CO2: %s OK\n", co2.getReading().sensor);
  } else {
    Serial.println("CO2: FAIL (SCD30/SCD40 not found)");
  }

  if (!store.begin()) {
    Serial.println("NVS: unavailable");
  } else if (!store.load(ctx, health)) {
    ctx = astro::defaultMissionContext();
    health = astro::HealthState();
    Serial.println("NVS: fresh state");
  } else {
    Serial.println("NVS: state restored");
  }

  engine.begin(health);
  motion.seedSteps(health.stepCount);
  ppgReading = ppg.getReading();

  Serial.printf("Crew %s, %u y/o, phase %s, day %u\n",
                ctx.crewId, (unsigned)ctx.age, astro::phaseText(ctx.phase),
                (unsigned)ctx.missionDay);
  Serial.println("Type 'help' for mission/profile commands.");
}

// ---------------------------------------------------------------------------
void loop() {
  pollButton();
  pollSerial();

  const uint32_t now = millis();

  // Sensor service
  ppg.update();
  if (ppg.takeUpdate(ppgReading)) {
    if (ppgReading.hrFresh && ppgReading.hrValid) {
      pushHeartRate(ppgReading.heartRateBpm);
      maybeCaptureBaseline();
    }
  }

  motion.update();
  co2.update();

  if (now - lastTempMs >= TEMP_MS) {
    lastTempMs = now;
    readTemperature();
  }

  if (now - lastSampleMs >= SAMPLE_MS) {
    const float dt = (now - lastSampleMs) / 1000.0f;
    lastSampleMs = now;

    astro::advanceMissionDay(ctx, DAYS_PER_SECOND * dt);

    // Build a vitals snapshot. Keep the last valid value when a channel drops.
    if (ppgReading.hrValid) vitals.hr = ppgReading.heartRateBpm;
    if (ppgReading.spo2Valid) vitals.spo2 = ppgReading.spo2Percent;
    if (tempValid) vitals.skinTempC = skinTempC;

    vitals.hrv = hrvMs;
    vitals.tempSlopeCPerH = tempSlopeCPerH;
    vitals.fingerPresent = ppgReading.fingerPresent;
    vitals.valid = ppgReading.sensorReady && tempValid &&
                   (ppgReading.hrValid || ppgReading.spo2Valid);
    vitals.symptomFlag = symptomFlag;
    vitals.timestampMs = now;

    // MPU6050: activity, steps and the rolling 24 h sleep/exercise estimate.
    const MotionReading &mr = motion.getReading();
    if (mr.valid) {
      vitals.activityIndex = mr.activityIndex;
      vitals.steps = mr.steps;
      vitals.motionValid = true;
      if (motion.hasSleepEstimate()) ctx.sleepHours24h = mr.sleepHours24h;
      if (motion.hasExerciseEstimate()) {
        ctx.exerciseMinutes24h = mr.exerciseMinutes24h;
      }
      health.stepCount = mr.steps;
      health.measuredSleepHours24h = mr.sleepHours24h;
      health.measuredExerciseMinutes24h = mr.exerciseMinutes24h;
    } else {
      // Heart-rate-derived fallback when the IMU is absent or unreadable.
      vitals.activityIndex = astro::clampf(
          (vitals.hr - health.baselineHr) * 2.0f, 0.0f, 100.0f);
      vitals.steps = health.stepCount;
      vitals.motionValid = false;
    }

    // SCD30/SCD40 cabin CO2. Applied as an air-quality stressor below.
    const CO2Reading &cr = co2.getReading();
    vitals.co2Ppm = cr.valid ? (float)cr.ppm : 0.0f;
    vitals.co2Valid = cr.valid;
    health.lastCo2Ppm = vitals.co2Ppm;

    scores = engine.evaluate(vitals, ctx);
    if (cr.valid) applyCabinAirQuality(scores, cr.ppm);

    health.cumulativeRadiationmSv = ctx.cumulativeRadiationmSv;
    health.sleepDebtHours = astro::clampf(8.0f - ctx.sleepHours24h, 0.0f, 8.0f);
    health.boneLossPercent = astro::estimatedBoneLossPercent(
        ctx.missionDay, ctx.gravityG, vitals.activityIndex, ctx.age);
    health.lastComposite = scores.composite;
    health.lastMissionDay = ctx.missionDay;
  }

  if (now - lastDrawMs >= DRAW_MS) {
    lastDrawMs = now;
    pages.draw(page, vitals, ctx, scores, scores.ml);
  }

  if (now - lastSerialMs >= SERIAL_MS) {
    lastSerialMs = now;
    printJson();
  }

  if (now - lastSaveMs >= SAVE_MS) {
    lastSaveMs = now;
    store.save(ctx, health);
  }
}
