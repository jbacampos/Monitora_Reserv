#pragma once

#include <stdint.h>

// ============================================================================
//  Calibracao e calculo de volume
//
//  Converte contagens do ADC (0..1023) em tensao, altura de coluna d'agua e
//  volume em litros, preservando as formulas do programa original.
//
//  As constantes de calibracao ficam AQUI (e nao em volume.cpp) porque sao
//  constexpr e servem tambem a quem precisa da referencia de maximo em tempo de
//  compilacao: include/config.h valida o teto do sensor com static_assert e
//  include/alerts.h converte os percentuais de alerta em litros.
// ============================================================================
namespace volume {

constexpr float kAdcReferenceV     = 3.3f;    // tensao de referencia do ADC
constexpr float kAdcMaxCounts      = 1023.0f; // resolucao de 10 bits
constexpr float kSensorFullScaleV  = 1.1f;    // fundo de escala do QDY30A
constexpr float kSensorFullScaleMm = 1000.0f; // 1,1 V equivalem a 1000 mm
constexpr float kLevelOffsetMm     = 50.0f;   // offset de montagem
constexpr float kLevelCorrection   = 1.05f;   // correcao de linearidade
constexpr float kLitersPerMm       = 41.52f;  // geometria do reservatorio

struct Measurement {
  uint16_t adc = 0;        // contagem bruta do ADC
  float voltageV = 0.0f;   // tensao no pino A0
  float sensorMm = 0.0f;   // leitura do sensor ("leitura")
  float levelMm = 0.0f;    // altura corrigida, nunca negativa ("nivel")
  float liters = 0.0f;     // volume instantaneo
};

// Etapas da cadeia de calibracao, expostas para reuso (alerta, teste, simulador).
constexpr float levelMmFromSensorMm(float sensorMm) {
  const float level = (sensorMm - kLevelOffsetMm) / kLevelCorrection;
  return (level < 0.0f) ? 0.0f : level;
}

constexpr float litersForLevelMm(float levelMm) { return levelMm * kLitersPerMm; }

// Leitura do sensor no FUNDO DE ESCALA do ADC: 3,3 V / 1,1 V * 1000 mm = 3000 mm.
// Acima disso o ADC satura em 1023 e o firmware nao ve mais nada.
constexpr float maxMeasurableSensorMm() {
  return kSensorFullScaleMm * (kAdcReferenceV / kSensorFullScaleV);
}

// MAIOR nivel corrigido que o sistema consegue medir: 2 809,5 mm. E a referencia
// dos percentuais de alerta (100% = esta altura), e nao os 3 m de agua do sensor,
// que o ADC satura antes de alcancar.
constexpr float maxMeasurableLevelMm() {
  return levelMmFromSensorMm(maxMeasurableSensorMm());
}

// Volume correspondente: 116 651,43 L. E o maior volume que o firmware reporta.
constexpr float maxMeasurableLiters() {
  return litersForLevelMm(maxMeasurableLevelMm());
}

// Aplica a cadeia de calibracao completa a uma leitura do ADC.
Measurement compute(uint16_t adc);

// Inverso de compute() para a etapa de altura: informa a contagem ADC que o
// sensor produziria para um dado nivel corrigido. Usado apenas pelo simulador.
float levelMmToAdc(float levelMm);

}  // namespace volume
