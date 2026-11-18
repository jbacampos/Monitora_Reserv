#include "sensor.h"

#include <Arduino.h>

#include "config.h"
#include "simulator.h"

// ============================================================================
//  Aquisicao do sensor de pressao
//
//  TEST_MODE == true  -> leitura do simulador; analogRead() nao e emitido.
//  TEST_MODE == false -> leitura exclusiva de analogRead(A0).
// ============================================================================
namespace {

// Construido apenas quando usado: no modo PRODUCAO esta funcao nunca e chamada,
// entao o simulador nao ocupa RAM nem e inicializado no boot.
[[maybe_unused]] Simulator &simulator() {
  static Simulator instance;
  return instance;
}

}  // namespace

namespace sensor {

void begin(uint32_t nowMs) {
  if constexpr (TEST_MODE) {
    simulator().begin(nowMs);
  } else {
    (void)nowMs;
    // Descarta a primeira conversao, que costuma ser instavel apos o boot.
    analogRead(PIN_SENSOR_ADC);
  }
}

uint16_t read(uint32_t nowMs) {
  if constexpr (TEST_MODE) {
    return simulator().sample(nowMs);
  } else {
    (void)nowMs;
    return static_cast<uint16_t>(analogRead(PIN_SENSOR_ADC));
  }
}

}  // namespace sensor

