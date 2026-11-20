#include "volume.h"

// ============================================================================
//  Implementacao
//
//  As constantes de calibracao vivem em volume.h (constexpr) e sao compartilhadas
//  com include/alerts.h e include/config.h. Aqui fica apenas a aplicacao da
//  cadeia, na mesma ordem do programa original.
// ============================================================================
namespace volume {

Measurement compute(uint16_t adc) {
  Measurement result;
  result.adc = adc;

  // tensao = sensorValue * (3.3 / 1023.0)
  result.voltageV = static_cast<float>(adc) * (kAdcReferenceV / kAdcMaxCounts);

  // leitura = tensao / 1.1 * 1000.0
  result.sensorMm = result.voltageV / kSensorFullScaleV * kSensorFullScaleMm;

  // nivel = (leitura - 50) / 1.05, limitado a zero
  result.levelMm = levelMmFromSensorMm(result.sensorMm);

  // volume = nivel * 41.52
  result.liters = litersForLevelMm(result.levelMm);

  return result;
}

float levelMmToAdc(float levelMm) {
  if (levelMm < 0.0f) {
    levelMm = 0.0f;
  }

  // Inverso exato de compute(): nivel -> leitura -> tensao -> contagens.
  const float sensorMm = levelMm * kLevelCorrection + kLevelOffsetMm;
  const float voltageV = sensorMm * (kSensorFullScaleV / kSensorFullScaleMm);
  return voltageV * (kAdcMaxCounts / kAdcReferenceV);
}

}  // namespace volume
