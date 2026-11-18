#pragma once

#include <Arduino.h>

#include "config.h"
#include "offline_queue.h"
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
//  Persistencia (porte do Ecowitt2TB): a decisao da politica NAO depende do
//  transporte. Com backlog pendente ou sem sessao MQTT a amostra vai para a
//  fila LittleFS com o timestamp ORIGINAL; com sessao e fila vazia, tenta
//  publicar. Na reconexao o historico e enviado em ordem cronologica antes de
//  qualquer amostra nova. `uptime_s` nao entra na telemetria nem na fila: e
//  publicado como atributo do dispositivo (TB_ATTRIBUTES_TOPIC).
// ============================================================================
struct TelemetrySample {
  float volumeL = 0.0f;      // volume filtrado, em litros
  uint64_t timestampMs = 0;  // epoch ms da medicao (0 = relogio sem NTP)
  uint32_t uptimeS = 0;      // segundos desde o boot (atributo)
  bool valid = false;        // existe ao menos uma leitura valida
};

class ThingsBoard {
 public:
  void begin(uint32_t nowMs);

  // Deve ser chamada continuamente pelo loop (sem delay()).
  void update(uint32_t nowMs, const TelemetrySample &sample);

  bool connected() const { return m_connected; }
  uint32_t publishCount() const { return m_publishCount; }
  bool queueReady() const { return m_queue.ready(); }
  uint32_t queuePending() const { return m_queue.pendingRecords(); }
  uint32_t queueRejected() const { return m_queue.rejectedRecords(); }

 private:
  bool connectSession(uint32_t nowMs);
  void drainQueue(uint32_t nowMs);
  bool publishValues(uint32_t nowMs, uint64_t timestampMs, float volumeL,
                     const char *origin);
  bool publishUptime(uint32_t nowMs, uint32_t uptimeS);
  bool enqueue(uint32_t nowMs, const TelemetrySample &sample);
  void registerPublished(uint32_t nowMs, float volumeL);

  bool m_connected = false;
  bool m_hasPublished = false;
  bool m_publishAfterReconnect = false;
  bool m_tokenPendingWarned = false;
  bool m_clockWarned = false;
  bool m_queueErrorWarned = false;
  bool m_queueFullWarned = false;
  uint32_t m_lastAttemptMs = 0;
  uint32_t m_lastPublishMs = 0;
  uint32_t m_publishCount = 0;
  uint32_t m_retryDelayMs = MQTT_RECONNECT_MS;
  float m_lastPublishedL = 0.0f;
  OfflineQueue m_queue;
};
