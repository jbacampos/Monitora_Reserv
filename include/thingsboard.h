#pragma once

#include <Arduino.h>

#include "config.h"
#include "publish_policy.h"

// ============================================================================
//  Envio de telemetria ao ThingsBoard (MQTT)
//
//  O modulo NAO calcula volume nem altura: recebe TelemetrySample pronto do
//  main (que usa os modulos de volume e filtro), decide apenas QUANDO publicar
//  (publish_policy.h) e monta o JSON. O destino e o token vem de
//  credentials.h (ACTIVE_*), sempre derivados de TEST_MODE.
//
//  Nada aqui bloqueia o loop principal: as tentativas de conexao sao agendadas
//  por millis() e o socket usa MQTT_SOCKET_TIMEOUT_MS como limite.
// ============================================================================
struct TelemetrySample {
  float volumeL = 0.0f;  // volume filtrado, em litros
  float levelMm = 0.0f;  // altura corrigida, em milimetros
  uint32_t uptimeS = 0;  // segundos desde o boot
  bool valid = false;    // existe ao menos uma leitura valida
};

class ThingsBoard {
 public:
  void begin(uint32_t nowMs);

  // Deve ser chamada continuamente pelo loop (sem delay()).
  void update(uint32_t nowMs, const TelemetrySample &sample);

  bool connected() const { return m_connected; }
  uint32_t publishCount() const { return m_publishCount; }

 private:
  bool connectSession(uint32_t nowMs);
  bool publishTelemetry(uint32_t nowMs, const TelemetrySample &sample,
                        publish_policy::Reason reason);

  bool m_connected = false;
  bool m_hasPublished = false;
  bool m_publishAfterReconnect = false;
  bool m_tokenPendingWarned = false;
  uint32_t m_lastAttemptMs = 0;
  uint32_t m_lastPublishMs = 0;
  uint32_t m_publishCount = 0;
  uint32_t m_retryDelayMs = MQTT_RECONNECT_MS;
  float m_lastPublishedL = 0.0f;
};
