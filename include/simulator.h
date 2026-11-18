#pragma once

#include <stdint.h>

#include "config.h"

// ============================================================================
//  Simulador de sensor (somente modo TESTE)
//
//  Gera contagens de ADC equivalentes as de um QDY30A real, aplicando o
//  inverso da calibracao de volume. A evolucao do cenario usa apenas o tempo
//  recebido em sample()/levelMm(), sem delay().
// ============================================================================
class Simulator {
 public:
  explicit Simulator(SimScenario scenario = SIM_SCENARIO,
                     float timeScale = SIM_TIME_SCALE,
                     bool noiseEnabled = SIM_NOISE);

  void begin(uint32_t nowMs);

  // Contagem ADC simulada, sempre dentro de 0..1023.
  uint16_t sample(uint32_t nowMs);

  // Nivel corrigido (mm) do cenario no instante informado.
  float levelMm(uint32_t nowMs) const;

  SimScenario scenario() const { return m_scenario; }
  bool noiseEnabled() const { return m_noiseEnabled; }
  float timeScale() const { return m_timeScale; }

  static const char* scenarioName(SimScenario scenario);

 private:
  float noiseCounts();
  float scaledElapsedMs(uint32_t nowMs) const;

  SimScenario m_scenario;
  float m_timeScale;
  bool m_noiseEnabled;
  uint32_t m_startMs;
  uint32_t m_rngState;
};
