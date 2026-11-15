#include "MissionContext.h"

#include <math.h>
#include <string.h>

namespace astro {

float defaultGravityForPhase(MissionPhase phase) {
  switch (phase) {
    case PHASE_PRELAUNCH: return 1.0f;
    case PHASE_LAUNCH: return 1.0f;
    case PHASE_MICROGRAVITY: return 0.0f;
    case PHASE_TRANSIT: return 0.0f;
    case PHASE_SURFACE: return 0.377f;  // Mars surface default
    case PHASE_RETURN: return 1.0f;
    case PHASE_POSTFLIGHT: return 1.0f;
  }
  return 1.0f;
}

float defaultRadiationForPhase(MissionPhase phase) {
  switch (phase) {
    case PHASE_PRELAUNCH: return 0.004f;
    case PHASE_LAUNCH: return 0.02f;
    case PHASE_MICROGRAVITY: return 0.50f;  // LEO / ISS order of magnitude
    case PHASE_TRANSIT: return 1.80f;       // deep-space transit
    case PHASE_SURFACE: return 0.70f;       // Mars surface
    case PHASE_RETURN: return 1.00f;
    case PHASE_POSTFLIGHT: return 0.004f;
  }
  return 0.004f;
}

MissionContext defaultMissionContext() {
  MissionContext ctx;
  strncpy(ctx.crewId, "ASTRO-01", sizeof(ctx.crewId) - 1);
  ctx.crewId[sizeof(ctx.crewId) - 1] = '\0';
  ctx.age = 42;
  ctx.sex = SEX_FEMALE;
  ctx.heightCm = 170.0f;
  ctx.weightKg = 70.0f;
  ctx.knownCVD = false;
  ctx.hypertension = false;
  ctx.diabetes = false;
  ctx.smoker = false;
  ctx.phase = PHASE_MICROGRAVITY;
  ctx.missionDay = 42;
  ctx.gravityG = defaultGravityForPhase(ctx.phase);
  ctx.shieldingFactor = 1.0f;
  ctx.dailyRadiationmSv = defaultRadiationForPhase(ctx.phase) / ctx.shieldingFactor;
  ctx.cumulativeRadiationmSv = ctx.dailyRadiationmSv * ctx.missionDay;
  ctx.sleepHours24h = 6.5f;
  ctx.exerciseMinutes24h = 45;
  ctx.isolationDays = ctx.missionDay;
  ctx.dayRemainder = 0.0f;
  return ctx;
}

void advanceMissionDay(MissionContext& ctx, float days) {
  if (days <= 0.0f) return;

  ctx.gravityG = defaultGravityForPhase(ctx.phase);
  const float base = defaultRadiationForPhase(ctx.phase);
  ctx.dailyRadiationmSv = base / fmaxf(ctx.shieldingFactor, 0.1f);
  ctx.cumulativeRadiationmSv += ctx.dailyRadiationmSv * days;

  // Keep a fractional remainder so sub-day steps still accumulate.
  ctx.dayRemainder += days;
  if (ctx.dayRemainder >= 1.0f) {
    const uint32_t whole = (uint32_t)ctx.dayRemainder;
    const uint32_t newDay = (uint32_t)ctx.missionDay + whole;
    ctx.missionDay = (uint16_t)((newDay > 65535U) ? 65535U : newDay);
    ctx.dayRemainder -= (float)whole;
    ctx.isolationDays = ctx.missionDay;
  }
}

}  // namespace astro
