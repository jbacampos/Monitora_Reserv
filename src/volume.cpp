#include "volume.h"

// ============================================================================
//  Constantes do programa original. Nao alterar sem recalibrar o sensor.
// ============================================================================
namespace {

constexpr float kAdcReferenceV      = 3.3f;    // tensao de referencia do ADC
constexpr float kAdcMaxCounts       = 1023.0f; // resolucao de 10 bits
constexpr float kSensorFullScaleV   = 1.1f;    // fundo de escala do QDY30A
constexpr float kSensorFullScaleMm  = 1000.0f; // 1,1 V equivalem a 1000 mm
constexpr float kLevelOffsetMm      = 50.0f;   // offset de montagem
constexpr float kLevelCorrection    = 1.05f;   // correcao de linearidade
constexpr float kLitersPerMm        = 41.52f;  // geometria do reservatorio

}  // namespace

namespace volume {

Measurement compute(uint16_t adc) {
  Measurement result;
  result.adc = adc;

  // tensao = sensorValue * (3.3 / 1023.0)
  result.voltageV = static_cast<float>(adc) * (kAdcReferenceV / kAdcMaxCounts);

  // leitura = tensao / 1.1 * 1000.0
  result.sensorMm = result.voltageV / kSensorFullScaleV * kSensorFullScaleMm;

  // nivel = (leitura - 50) / 1.05, limitado a zero
  result.levelMm = (result.sensorMm - kLevelOffsetMm) / kLevelCorrection;
  if (result.levelMm < 0.0f) {
    result.levelMm = 0.0f;
  }

  // volume = nivel * 41.52
  result.liters = result.levelMm * kLitersPerMm;

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
