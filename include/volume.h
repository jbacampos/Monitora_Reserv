#pragma once

#include <stdint.h>

// ============================================================================
//  Calibracao e calculo de volume
//
//  Converte contagens do ADC (0..1023) em tensao, altura de coluna d'agua e
//  volume em litros, preservando as formulas do programa original.
// ============================================================================
namespace volume {

struct Measurement {
  uint16_t adc = 0;        // contagem bruta do ADC
  float voltageV = 0.0f;   // tensao no pino A0
  float sensorMm = 0.0f;   // leitura do sensor ("leitura")
  float levelMm = 0.0f;    // altura corrigida, nunca negativa ("nivel")
  float liters = 0.0f;     // volume instantaneo
};

// Aplica a cadeia de calibracao completa a uma leitura do ADC.
Measurement compute(uint16_t adc);

// Inverso de compute() para a etapa de altura: informa a contagem ADC que o
// sensor produziria para um dado nivel corrigido. Usado apenas pelo simulador.
float levelMmToAdc(float levelMm);

}  // namespace volume
