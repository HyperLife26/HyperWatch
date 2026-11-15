#pragma once

#include <U8g2lib.h>
#include "HealthTypes.h"

namespace astro {

// Ten compact OLED pages for a 128x64 SH1106.
class DisplayPages {
 public:
  static constexpr uint8_t PAGE_COUNT = 10;

  void begin(U8G2* display);
  void draw(uint8_t page, const Vitals& v, const MissionContext& m,
            const RiskScores& r, const ModelOutput& ml);

 private:
  U8G2* d_ = nullptr;

  void header(const char* title);
  void textf(uint8_t y, const char* fmt, ...);
  void bar(uint8_t y, const char* label, float value);
  void riskPage(uint8_t idx, const Vitals& v, const MissionContext& m,
                const RiskScores& r, const ModelOutput& ml);
};

}  // namespace astro
