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
  // Instante (millis) da ultima sessao MQTT ACEITA pelo broker (0 = nenhuma).
  // Diagnostico e apoio da guarda de bloqueio (stall_guard.h).
  uint32_t lastConnectedMs() const { return m_lastConnectedMs; }
  // Ultimo sinal de vida do MQTT: aceite de sessao OU publicacao confirmada. A
  // guarda usa este valor SOMENTE quando NAO ha sessao (com a sessao de pe o main
  // passa millis(), pois o servico esta vivo); sem isso, uma sessao longa e
  // saudavel - que nunca reconecta - parecia "parada" e reiniciava o ESP a cada
  // MQTT_STALL_REBOOT_MS.
  uint32_t lastActivityMs() const {
    return (m_lastPublishMs > m_lastConnectedMs) ? m_lastPublishMs
                                                 : m_lastConnectedMs;
  }
  // true quando o RX do TLS foi ampliado para MQTT_TLS_RX_BUFFER_FALLBACK por
  // causa de um registro maior que o normal (o broker mudou a cadeia). Apenas
  // diagnostico: aparece no log serial e na linha periodica (mqtt=off(rx4096)).
  bool rxEscalated() const { return m_rxEscalated; }
  uint32_t publishCount() const { return m_publishCount; }
  bool queueReady() const { return m_queue.ready(); }
  uint32_t queuePending() const { return m_queue.pendingRecords(); }
  uint32_t queueRejected() const { return m_queue.rejectedRecords(); }

 private:
  bool connectSession(uint32_t nowMs);
  // Interpreta o erro de TLS da tentativa que falhou: amplia o RX quando o
  // registro nao coube, volta ao normal quando nao ha heap e libera os buffers
  // da tentativa. Chamada apenas de connectSession (caminho de falha).
  void handleTlsFailure(uint32_t nowMs);
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
  // Ultima sessao MQTT aceita (0 = nenhuma). Alimenta a guarda de bloqueio.
  uint32_t m_lastConnectedMs = 0;
  uint32_t m_publishCount = 0;
  uint32_t m_retryDelayMs = MQTT_RECONNECT_MS;
  // RX do TLS ampliado (registro maior que o buffer normal) e pedido de nova
  // tentativa imediata: os buffers foram reajustados e nao vale esperar o
  // backoff dobrado para saber se o novo tamanho resolve.
  bool m_rxEscalated = false;
  bool m_retryNow = false;
  // Ultimo codigo de TLS ja reportado (evita repetir a mesma linha a cada
  // tentativa enquanto o broker estiver recusando a sessao).
  int m_lastSslCode = 0;
  float m_lastPublishedL = 0.0f;
  OfflineQueue m_queue;
};
