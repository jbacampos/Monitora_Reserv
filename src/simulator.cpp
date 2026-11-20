#include "simulator.h"

#include <math.h>

#include "volume.h"

namespace {

constexpr float kAdcMaxCounts = 1023.0f;
constexpr float kTwoPi = 6.28318530718f;

// Progresso 0..1 de uma rampa, saturado no fim.
float progress(float elapsedMs, float durationMs) {
  if (durationMs <= 0.0f) {
    return 1.0f;
  }
  const float t = elapsedMs / durationMs;
  return (t >= 1.0f) ? 1.0f : t;
}

float lerp(float from, float to, float t) { return from + (to - from) * t; }

}  // namespace

Simulator::Simulator(SimScenario scenario, float timeScale, bool noiseEnabled)
    : m_scenario(scenario),
      m_timeScale(timeScale > 0.0f ? timeScale : 1.0f),
      m_noiseEnabled(noiseEnabled),
      m_startMs(0),
      m_rngState(0x2545F491u) {}

void Simulator::begin(uint32_t nowMs) { m_startMs = nowMs; }

float Simulator::scaledElapsedMs(uint32_t nowMs) const {
  return static_cast<float>(nowMs - m_startMs) * m_timeScale;
}

float Simulator::levelMm(uint32_t nowMs) const {
  const float elapsedMs = scaledElapsedMs(nowMs);
  float level = 0.0f;

  switch (m_scenario) {
    case SimScenario::CONSTANT:
      level = SIM_CONSTANT_LEVEL_MM;
      break;

    case SimScenario::FILLING:
      level = lerp(SIM_FILLING_START_MM, SIM_FILLING_END_MM,
                   progress(elapsedMs, static_cast<float>(SIM_SCENARIO_DURATION_MS)));
      break;

    case SimScenario::DRAINING:
      level = lerp(SIM_DRAINING_START_MM, SIM_DRAINING_END_MM,
                   progress(elapsedMs, static_cast<float>(SIM_SCENARIO_DURATION_MS)));
      break;

    case SimScenario::OSCILLATING: {
      const float phase =
          kTwoPi * elapsedMs / SIM_OSCILLATING_PERIOD_MS;
      level = SIM_OSCILLATING_CENTER_MM +
              SIM_OSCILLATING_AMPLITUDE_MM * sinf(phase);
      break;
    }

    case SimScenario::CRITICAL:
      // Abaixo do offset de 50 mm: a calibracao deve limitar o nivel a zero.
      level = SIM_CRITICAL_LEVEL_MM;
      break;

    case SimScenario::RECOVERY:
      level = lerp(SIM_RECOVERY_START_MM, SIM_RECOVERY_END_MM,
                   progress(elapsedMs, static_cast<float>(SIM_SCENARIO_DURATION_MS)));
      break;

    case SimScenario::CYCLE: {
      // Onda triangular: desce SIM_CYCLE_DRAIN_MS, sobe SIM_CYCLE_FILL_MS e
      // recomeca. O modulo do periodo e o que torna o cenario repetitivo - e
      // por isso ele cobre os QUATRO avisos (entrada e recuperacao do risco e
      // do critico) sem precisar de reinicio.
      const float period = static_cast<float>(SIM_CYCLE_DRAIN_MS + SIM_CYCLE_FILL_MS);
      float phase = elapsedMs;
      if (period > 0.0f) {
        phase = fmodf(elapsedMs, period);
      }
      if (phase < static_cast<float>(SIM_CYCLE_DRAIN_MS)) {
        level = lerp(SIM_DRAINING_START_MM, SIM_DRAINING_END_MM,
                     progress(phase, static_cast<float>(SIM_CYCLE_DRAIN_MS)));
      } else {
        level = lerp(SIM_DRAINING_END_MM, SIM_DRAINING_START_MM,
                     progress(phase - static_cast<float>(SIM_CYCLE_DRAIN_MS),
                              static_cast<float>(SIM_CYCLE_FILL_MS)));
      }
      break;
    }
  }

  return (level < 0.0f) ? 0.0f : level;
}

uint16_t Simulator::sample(uint32_t nowMs) {
  float counts = volume::levelMmToAdc(levelMm(nowMs));

  if (m_noiseEnabled) {
    counts += noiseCounts();
  }

  if (counts < 0.0f) {
    counts = 0.0f;
  } else if (counts > kAdcMaxCounts) {
    counts = kAdcMaxCounts;
  }

  return static_cast<uint16_t>(counts + 0.5f);
}

float Simulator::noiseCounts() {
  // xorshift32: determinístico, barato e sem depender de randomSeed().
  uint32_t x = m_rngState;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  m_rngState = x;

  const int32_t spread = static_cast<int32_t>(x % 2001u) - 1000;
  return (static_cast<float>(spread) / 1000.0f) * SIM_NOISE_AMPLITUDE_COUNTS;
}

const char* Simulator::scenarioName(SimScenario scenario) {
  switch (scenario) {
    case SimScenario::CONSTANT:
      return "CONSTANT";
    case SimScenario::FILLING:
      return "FILLING";
    case SimScenario::DRAINING:
      return "DRAINING";
    case SimScenario::OSCILLATING:
      return "OSCILLATING";
    case SimScenario::CRITICAL:
      return "CRITICAL";
    case SimScenario::RECOVERY:
      return "RECOVERY";
    case SimScenario::CYCLE:
      return "CYCLE";
  }
  return "UNKNOWN";
}
