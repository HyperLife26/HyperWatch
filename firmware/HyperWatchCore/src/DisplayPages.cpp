#include "DisplayPages.h"

#include <stdarg.h>
#include <stdio.h>

namespace astro {

void DisplayPages::begin(U8G2* display) {
  d_ = display;
}

void DisplayPages::header(const char* title) {
  if (!d_) return;
  d_->setFont(u8g2_font_6x10_tf);
  d_->drawStr(0, 9, title);
  d_->drawHLine(0, 11, 128);
}

void DisplayPages::textf(uint8_t y, const char* fmt, ...) {
  if (!d_) return;
  char buf[40];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  d_->drawStr(0, y, buf);
}

void DisplayPages::bar(uint8_t y, const char* label, float value) {
  if (!d_) return;
  d_->drawStr(0, y, label);
  d_->drawFrame(30, y - 7, 96, 7);
  int w = (int)(94.0f * clampf(value, 0.0f, 100.0f) / 100.0f);
  if (w > 0) d_->drawBox(31, y - 6, w, 5);
}

void DisplayPages::riskPage(uint8_t idx, const Vitals& v, const MissionContext& m,
                            const RiskScores& r, const ModelOutput& ml) {
  header(riskName(idx));
  bar(22, "RISK", r.risk[idx]);
  textf(34, "%s  %5.1f / 100", bandText(r.band[idx]), r.risk[idx]);

  switch (idx) {
    case RISK_CARDIO:
      textf(45, "HR %5.1f  HRV %4.1f", v.hr, v.hrv);
      textf(55, "SpO2 %5.1f  T %4.1f", v.spo2, v.skinTempC);
      textf(63, "Event %3.0f%%  ML %.0f / R %.0f",
            r.cardiacEventProb * 100.0f, ml.risk[idx], r.ruleRisk[idx]);
      break;
    case RISK_BONE: {
      const float loss = estimatedBoneLossPercent(
          m.missionDay, m.gravityG, v.activityIndex, m.age);
      textf(45, "Bone loss %5.1f %%", loss);
      textf(55, "Day %u   g %.2f", (unsigned)m.missionDay, m.gravityG);
      textf(63, "Exercise %u min  ML %.0f / R %.0f",
            (unsigned)m.exerciseMinutes24h, ml.risk[idx], r.ruleRisk[idx]);
      break;
    }
    case RISK_IMMUNE:
      textf(45, "Temp %4.1f C  Slope %+0.2f", v.skinTempC, v.tempSlopeCPerH);
      textf(55, "Sleep %4.1f h  Rad %4.2f", m.sleepHours24h, m.dailyRadiationmSv);
      if (v.co2Valid) {
        textf(63, "CO2 %4u ppm %s", (unsigned)v.co2Ppm,
              airQualityText(v.co2Ppm));
      } else {
        textf(63, "Day %u  ML %.0f / R %.0f",
              (unsigned)m.missionDay, ml.risk[idx], r.ruleRisk[idx]);
      }
      break;
    case RISK_RADIATION:
      textf(45, "Daily %5.2f mSv", m.dailyRadiationmSv);
      textf(55, "Cum %6.1f mSv", m.cumulativeRadiationmSv);
      textf(63, "Career %4.1f%%  ML %.0f / R %.0f",
            m.cumulativeRadiationmSv / 600.0f * 100.0f,
            ml.risk[idx], r.ruleRisk[idx]);
      break;
    case RISK_SLEEP:
      textf(45, "Sleep %4.1f h", m.sleepHours24h);
      textf(55, "Debt %4.1f h  Iso %u d",
            8.0f - m.sleepHours24h, (unsigned)m.isolationDays);
      if (v.motionValid) {
        textf(63, "Act %3.0f  Stp %5lu", v.activityIndex,
              (unsigned long)v.steps);
      } else {
        textf(63, "Activity %3.0f  ML %.0f / R %.0f",
              v.activityIndex, ml.risk[idx], r.ruleRisk[idx]);
      }
      break;
    default:
      break;
  }
}

void DisplayPages::draw(uint8_t page, const Vitals& v, const MissionContext& m,
                        const RiskScores& r, const ModelOutput& ml) {
  if (!d_) return;

  d_->clearBuffer();
  d_->setFont(u8g2_font_6x10_tf);

  switch (page % PAGE_COUNT) {
    case 0:
      header("HYPERWATCH");
      textf(22, "Crew: %s", m.crewId);
      textf(32, "%u y/o %s  %.0f kg", (unsigned)m.age,
            (m.sex == SEX_FEMALE) ? "F" : "M", m.weightKg);
      textf(42, "%s  D%u", phaseText(m.phase), (unsigned)m.missionDay);
      textf(52, "Gravity %.2f g", m.gravityG);
      textf(62, "Shield x%.1f", m.shieldingFactor);
      break;

    case 1:
      header("MISSION CONTEXT");
      textf(22, "Phase %s", phaseText(m.phase));
      textf(32, "Day %u   Iso %u d", (unsigned)m.missionDay,
            (unsigned)m.isolationDays);
      textf(42, "Gravity %.2f g", m.gravityG);
      textf(52, "Rad %.2f mSv/d", m.dailyRadiationmSv);
      textf(62, "Cum %.1f mSv  Slp %.1f h", m.cumulativeRadiationmSv,
            m.sleepHours24h);
      break;

    case 2:
      header("LIVE VITALS");
      textf(22, "HR   %5.1f bpm", v.hr);
      textf(32, "HRV  %5.1f ms", v.hrv);
      textf(42, "SpO2 %5.1f %%", v.spo2);
      textf(52, "Temp %5.1f C", v.skinTempC);
      if (v.motionValid) {
        textf(62, "Act %3.0f  Stp %5lu", v.activityIndex,
              (unsigned long)v.steps);
      } else {
        textf(62, "Act %3.0f  %s", v.activityIndex,
              r.dataUnreliable ? "NO SIGNAL" : "SIGNAL OK");
      }
      break;

    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
      riskPage((uint8_t)(page - 3), v, m, r, ml);
      break;

    case 8:
      header("MISSION RISK");
      if (r.cardiacAlert) {
        d_->drawBox(0, 13, 128, 10);
        d_->setDrawColor(0);
        d_->drawStr(2, 21, "*** CARDIAC ALERT ***");
        d_->setDrawColor(1);
      } else {
        textf(21, "Composite %5.1f  %s", r.composite,
              bandText(r.compositeBand));
      }
      bar(31, "CAR", r.risk[RISK_CARDIO]);
      bar(39, "BON", r.risk[RISK_BONE]);
      bar(47, "IMM", r.risk[RISK_IMMUNE]);
      bar(55, "RAD", r.risk[RISK_RADIATION]);
      bar(63, "SLP", r.risk[RISK_SLEEP]);
      break;

    case 9:
      header("MODEL DIAGNOSTICS");
      textf(22, "int8 MLP 13-16-8-6");
      textf(32, "Inference %lu us", (unsigned long)ml.inferenceUs);
      textf(42, "Ver %u  Ran %s", (unsigned)ml.version,
            ml.ran ? "YES" : "NO");
      textf(52, "Rule/ML: CAR %.0f/%.0f", r.ruleRisk[RISK_CARDIO],
            ml.risk[RISK_CARDIO]);
      textf(62, "BON %.0f/%.0f  IMM %.0f/%.0f",
            r.ruleRisk[RISK_BONE], ml.risk[RISK_BONE],
            r.ruleRisk[RISK_IMMUNE], ml.risk[RISK_IMMUNE]);
      break;
  }

  if (r.cardiacAlert && (page % PAGE_COUNT) != 8) {
    d_->drawFrame(0, 0, 128, 64);
  }

  d_->sendBuffer();
}

}  // namespace astro
