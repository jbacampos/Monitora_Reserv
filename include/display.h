#pragma once

#include <stdint.h>

#include <TM1637TinyDisplay6.h>

#include "config.h"

// ============================================================================
//  Display TM1637 (6 digitos)
//
//  Responsabilidades:
//   - mostrar o volume filtrado em litros, com o mesmo formato do programa
//     original (sem ponto abaixo de 1000 L; ponto de milhar a partir disso);
//   - atualizar a indicacao a cada DISPLAY_REFRESH_MS;
//   - apagar o display apos DISPLAY_ON_TIME_MS;
//   - religar e reiniciar o contador de tempo quando o botao (D7) e acionado.
// ============================================================================
class DisplayManager {
 public:
  DisplayManager();

  void begin(uint32_t nowMs);

  // Armazena o valor mais recente; a escrita no display segue a temporizacao.
  void setLiters(float liters);

  // Deve ser chamada continuamente pelo loop (sem delay()).
  void update(uint32_t nowMs);

  bool isOn() const { return m_on; }

 private:
  void render();
  void powerOff();
  void handleButton(uint32_t nowMs);

  TM1637TinyDisplay6 m_display;
  float m_liters;
  bool m_on;
  bool m_buttonLatched;      // borda de pressao ja processada
  int m_lastRawButtonLevel;  // ultimo nivel lido do pino
  uint32_t m_lastRawChangeMs;
  uint32_t m_lastRefreshMs;
  uint32_t m_onSinceMs;
};
