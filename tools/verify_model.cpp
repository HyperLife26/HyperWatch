// Host-side check that RiskModel.cpp matches tools/verify_model.py.
// Build (from the repository root):
//   g++ -std=c++17 -Itools/shim -Ifirmware/HyperWatchCore/src \
//       tools/verify_model.cpp firmware/HyperWatchCore/src/RiskModel.cpp -o verify_model
#include "RiskModel.h"
#include <cstdio>

unsigned long millis() { return 0; }
unsigned long micros() { return 0; }

int main() {
  astro::Vitals v;
  v.hr = 120.0f;
  v.hrv = 20.0f;
  v.spo2 = 92.0f;
  v.skinTempC = 38.0f;
  v.tempSlopeCPerH = 0.2f;
  v.activityIndex = 80.0f;
  v.symptomFlag = true;

  astro::MissionContext m;
  m.sleepHours24h = 5.0f;
  m.missionDay = 100;
  m.gravityG = 0.0f;
  m.dailyRadiationmSv = 1.5f;
  m.age = 55;
  m.knownCVD = true;

  astro::RiskModel model;
  model.begin();
  const astro::ModelOutput out = model.predict(v, m);

  const char* names[6] = {"cardio", "bone", "immune", "radiation", "sleep", "cardiac_event"};
  std::printf("C++ int8 prediction (cardiac_event shown as 0-100):\n");
  for (int i = 0; i < 5; ++i) {
    std::printf("  %-14s %7.3f\n", names[i], out.risk[i]);
  }
  std::printf("  %-14s %7.3f\n", names[5], out.cardiacEventProb * 100.0f);
  return 0;
}
