#pragma once

#include <stdint.h>

// ============================================================================
//  Uptime acumulado - imune ao rollover de millis()
//
//  millis() e um contador de 32 bits de MILISSEGUNDOS: volta a zero a cada
//  ~49,7 dias. Calcular o uptime com `millis() / 1000` faz o tempo "recomecar"
//  nesse instante. Este contador acumula os DELTAS entre leituras sucessivas em
//  um inteiro de 64 bits:
//
//    - a subtracao e feita em uint32_t, portanto `agora - anterior` ja trata o
//      rollover corretamente (aritmetica modular);
//    - o acumulado (uint64_t) cobre ~584 milhoes de anos em milissegundos, ou
//      seja, nao existe um novo rollover em operacao normal;
//    - o valor devolvido e independente de NTP/hora civil: conta apenas tempo
//      decorrido desde begin().
//
//  Requisito de uso: update() deve ser chamado a intervalos MENORES que 2^32 ms
//  (49,7 dias) - o loop() do firmware chama em toda iteracao, e ate uma consulta
//  do Telegram que bloqueie alguns segundos e contabilizada integralmente no
//  delta seguinte (o tempo nao e perdido nem contado duas vezes).
//
//  Puro (sem Arduino), para poder ser exercitado pelos testes de host.
// ============================================================================
class UptimeCounter {
 public:
  // Marca o instante inicial. O uptime passa a contar de zero a partir daqui.
  void begin(uint32_t nowMs) {
    m_lastMs = nowMs;
    m_ms = 0;
    m_started = true;
  }

  // Acumula o tempo decorrido desde a chamada anterior (wrap-safe).
  void update(uint32_t nowMs) {
    if (!m_started) {
      begin(nowMs);
      return;
    }

    const uint32_t deltaMs = static_cast<uint32_t>(nowMs - m_lastMs);
    m_lastMs = nowMs;
    m_ms += deltaMs;
  }

  // Milissegundos acumulados desde begin() (nunca reinicia por rollover).
  uint64_t milliseconds() const { return m_ms; }

  // Segundos acumulados. Cabe em uint32_t por mais de 136 anos, muito acima da
  // vida util do dispositivo; o acumulado de 64 bits continua sendo a fonte.
  uint32_t seconds() const { return static_cast<uint32_t>(m_ms / 1000ULL); }

  bool started() const { return m_started; }

 private:
  uint32_t m_lastMs = 0;
  uint64_t m_ms = 0;
  bool m_started = false;
};
