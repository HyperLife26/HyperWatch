#pragma once

#include "HealthTypes.h"

namespace astro {

// Order-of-magnitude mission defaults. Not authoritative mission planning values.
float defaultGravityForPhase(MissionPhase phase);
float defaultRadiationForPhase(MissionPhase phase);  // mSv/day before shielding
MissionContext defaultMissionContext();

// Advance the mission clock, recompute gravity/dose, and accumulate dose.
void advanceMissionDay(MissionContext& ctx, float days);

}  // namespace astro
