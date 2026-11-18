#pragma once

#include <stdint.h>

// ============================================================================
//  Politica de publicacao da telemetria
//
//  Regra pura (sem Arduino, sem broker), mantida separada do transporte para
//  poder ser verificada no teste de host - docs/host_test - sem device.
//
//  Ordem de decisao:
//    1. primeira leitura valida apos a inicializacao        -> FIRST
//    2. primeira publicacao apos (re)conexao MQTT            -> RECONNECT
//    3. variacao >= deltaLiters (respeitando minIntervalMs)  -> DELTA
//    4. heartbeat periodico, mesmo sem variacao              -> HEARTBEAT
// ============================================================================
namespace publish_policy {

enum class Reason : uint8_t {
  NONE = 0,
  FIRST,      // primeira leitura valida
  RECONNECT,  // imediatamente apos reconectar ao MQTT
  DELTA,      // mudanca significativa de volume
  HEARTBEAT,  // publicacao periodica
};

struct Input {
  bool sampleValid;            // existe ao menos uma leitura valida
  bool hasPublished;           // ja publicou alguma vez neste boot
  bool publishAfterReconnect;  // precisa publicar logo apos reconectar
  float volumeL;               // volume filtrado atual
  float lastPublishedL;        // ultimo volume publicado
  uint32_t nowMs;              // instante atual
  uint32_t lastPublishMs;      // instante da ultima publicacao
  float deltaLiters;           // variacao minima para publicar
  uint32_t heartbeatMs;        // periodo do heartbeat
  uint32_t minIntervalMs;      // intervalo minimo entre publicacoes por delta
};

constexpr float absValue(float value) { return value < 0.0f ? -value : value; }

constexpr Reason decide(const Input &input) {
  if (!input.sampleValid) {
    return Reason::NONE;
  }

  if (!input.hasPublished) {
    return Reason::FIRST;
  }

  if (input.publishAfterReconnect) {
    return Reason::RECONNECT;
  }

  // Aritmetica sem sinal: correta tambem na virada do millis().
  const uint32_t sinceLastPublishMs = input.nowMs - input.lastPublishMs;

  const bool changed =
      absValue(input.volumeL - input.lastPublishedL) >= input.deltaLiters;

  if (changed && sinceLastPublishMs >= input.minIntervalMs) {
    return Reason::DELTA;
  }

  if (sinceLastPublishMs >= input.heartbeatMs) {
    return Reason::HEARTBEAT;
  }

  return Reason::NONE;
}

constexpr const char *reasonName(Reason reason) {
  switch (reason) {
    case Reason::FIRST:
      return "primeira leitura";
    case Reason::RECONNECT:
      return "reconexao";
    case Reason::DELTA:
      return "mudanca significativa";
    case Reason::HEARTBEAT:
      return "heartbeat";
    case Reason::NONE:
      break;
  }
  return "nenhuma";
}

}  // namespace publish_policy
