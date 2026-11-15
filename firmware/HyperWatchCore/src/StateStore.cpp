#include "StateStore.h"

#include <stddef.h>
#include <string.h>

namespace astro {
namespace {

constexpr uint32_t STATE_MAGIC = 0x48595052UL;  // "HYPR"
constexpr uint16_t STATE_VERSION = 2;

struct PersistedState {
  uint32_t magic;
  uint16_t version;
  MissionContext ctx;
  HealthState health;
  uint32_t crc;
};

}  // namespace

uint32_t crc32(const uint8_t* data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc & 1UL) ? ((crc >> 1) ^ 0xEDB88320UL) : (crc >> 1);
    }
  }
  return ~crc;
}

bool StateStore::begin(const char* ns) {
  ready_ = prefs_.begin(ns, false);
  return ready_;
}

bool StateStore::load(MissionContext& ctx, HealthState& health) {
  if (!ready_) return false;

  PersistedState blob;
  if (prefs_.getBytesLength("state") != sizeof(blob)) return false;
  if (prefs_.getBytes("state", &blob, sizeof(blob)) != sizeof(blob)) return false;
  if (blob.magic != STATE_MAGIC || blob.version != STATE_VERSION) return false;

  const uint32_t expected = crc32(
      reinterpret_cast<const uint8_t*>(&blob),
      offsetof(PersistedState, crc));
  if (blob.crc != expected) return false;

  ctx = blob.ctx;
  health = blob.health;
  return true;
}

bool StateStore::save(const MissionContext& ctx, const HealthState& health) {
  if (!ready_) return false;

  PersistedState blob;
  memset(&blob, 0, sizeof(blob));
  blob.magic = STATE_MAGIC;
  blob.version = STATE_VERSION;
  blob.ctx = ctx;
  blob.health = health;
  blob.crc = crc32(reinterpret_cast<const uint8_t*>(&blob),
                   offsetof(PersistedState, crc));

  return prefs_.putBytes("state", &blob, sizeof(blob)) == sizeof(blob);
}

void StateStore::erase() {
  if (ready_) prefs_.remove("state");
}

}  // namespace astro
