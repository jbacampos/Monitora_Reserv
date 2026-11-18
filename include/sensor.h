#pragma once

#include <stdint.h>

// ============================================================================
//  Aquisicao do sensor de pressao
//
//  PRODUCAO: leitura exclusiva de analogRead(A0).
//  TESTE:    leitura do simulador; analogRead() nao e referenciado.
// ============================================================================
namespace sensor {

void begin(uint32_t nowMs);
uint16_t read(uint32_t nowMs);

}  // namespace sensor
