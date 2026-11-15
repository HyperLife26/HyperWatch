#pragma once

#include <Preferences.h>
#include "HealthTypes.h"

namespace astro {

// Versioned, CRC-protected NVS persistence for mission context + health state.
class StateStore {
 public:
  bool begin(const char* ns = "hyperwatch");
  bool load(MissionContext& ctx, HealthState& health);
  bool save(const MissionContext& ctx, const HealthState& health);
  void erase();
  bool ready() const { return ready_; }

 private:
  Preferences prefs_;
  bool ready_ = false;
};

uint32_t crc32(const uint8_t* data, size_t length);

}  // namespace astro
