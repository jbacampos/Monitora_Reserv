#include "thingsboard.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>

#include "config.h"
#include "credentials.h"
#include "datalog_mqtt_ca.h"

namespace {

double seconds(uint32_t nowMs) { return static_cast<double>(nowMs) / 1000.0; }

// Transporte e sessao MQTT unicos do firmware.
WiFiClientSecure g_transport;
BearSSL::X509List g_mqttCa(DATALOG_MQTT_CA);
PubSubClient g_mqtt(g_transport);
char g_payload[192];

// Codigos de estado do PubSubClient.
const char *mqttStateName(int state) {
  switch (state) {
    case MQTT_CONNECTED:
      return "conectado";
    case MQTT_DISCONNECTED:
      return "desconectado";
    case MQTT_CONNECT_FAILED:
      return "falha de rede";
    case MQTT_CONNECTION_TIMEOUT:
      return "timeout de conexao";
    case MQTT_CONNECTION_LOST:
      return "conexao perdida";
    case MQTT_CONNECT_BAD_PROTOCOL:
      return "protocolo invalido";
    case MQTT_CONNECT_BAD_CLIENT_ID:
      return "client id rejeitado";
    case MQTT_CONNECT_UNAVAILABLE:
      return "servidor indisponivel";
    case MQTT_CONNECT_BAD_CREDENTIALS:
      return "credenciais invalidas";
    case MQTT_CONNECT_UNAUTHORIZED:
      return "nao autorizado";
  }
  return "estado desconhecido";
}

}  // namespace

void ThingsBoard::begin(uint32_t nowMs) {
  // Timeouts curtos: limitam o tempo que uma tentativa pode segurar o loop
  // (resolucao de nome + conexao TCP + espera do CONNACK).
  g_transport.setTrustAnchors(&g_mqttCa);
  g_transport.setTimeout(MQTT_SOCKET_TIMEOUT_MS);

  g_mqtt.setServer(TB_HOST, TB_PORT);
  g_mqtt.setSocketTimeout(MQTT_CONNACK_TIMEOUT_S);
  g_mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  // Fila offline: monta o LittleFS sem formatacao automatica e recupera os
  // registros pendentes de execucoes anteriores.
  const bool queueOk = m_queue.begin();
  Serial.printf("Fila offline: %s", queueOk ? "pronta" : "indisponivel");
  if (queueOk) {
    Serial.printf(" | pendentes=%lu",
                  static_cast<unsigned long>(m_queue.pendingRecords()));
  }
  Serial.println();

  // Permite a primeira tentativa ja no primeiro update().
  m_lastAttemptMs = nowMs - MQTT_RECONNECT_MS;
  m_retryDelayMs = MQTT_RECONNECT_MS;
}

bool ThingsBoard::connectSession(uint32_t nowMs) {
  if (tbTokenPending()) {
    if (!m_tokenPendingWarned) {
      m_tokenPendingWarned = true;
      Serial.printf(
          "TB: token pendente em %s (preencha e recompile). "
          "Conexao MQTT/TLS nao sera tentada.\n",
          activeSecretsFileName());
    }
    return false;
  }

  // clientId estavel e curto, sem alocacao de String.
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "%s-%06x", DEVICE_ID,
           static_cast<unsigned>(ESP.getChipId()));

  Serial.printf("[%7.2f s] TB: conectando a %s:%u (clientId=%s)\n",
                seconds(nowMs), TB_HOST, static_cast<unsigned>(TB_PORT),
                clientId);

  const uint32_t startedMs = millis();
  const bool ok = g_mqtt.connect(clientId, ACTIVE_TB_TOKEN, nullptr);
  const uint32_t elapsedMs = millis() - startedMs;

  if (!ok) {
    Serial.printf(
        "[%7.2f s] TB: falha ao conectar (motivo=%s, estado=%d) em %lu ms\n",
        seconds(nowMs), mqttStateName(g_mqtt.state()), g_mqtt.state(),
        static_cast<unsigned long>(elapsedMs));
    return false;
  }

  Serial.printf("[%7.2f s] TB: conectado em %lu ms\n", seconds(nowMs),
                static_cast<unsigned long>(elapsedMs));
  return true;
}

bool ThingsBoard::publishValues(uint32_t nowMs, uint64_t timestampMs,
                                float volumeL, const char *origin) {
  // `ts` em milissegundos sem printf de 64 bits: segundos + 3 digitos.
  const unsigned long secondsPart =
      static_cast<unsigned long>(timestampMs / 1000ULL);
  const unsigned long millisPart =
      static_cast<unsigned long>(timestampMs % 1000ULL);

  const int written = snprintf(
      g_payload, sizeof(g_payload), "{\"%s\":%lu%03lu,\"values\":{\"%s\":%.1f}}",
      TB_KEY_TS, secondsPart, millisPart, TB_KEY_VOLUME,
      static_cast<double>(volumeL));

  if (written <= 0 || static_cast<size_t>(written) >= sizeof(g_payload)) {
    Serial.printf("TB: payload nao cabe no buffer (%d bytes)\n", written);
    return false;
  }

  if (!g_mqtt.connected()) {
    m_connected = false;
    return false;
  }

  if (!g_mqtt.publish(TB_TELEMETRY_TOPIC, g_payload, false)) {
    Serial.printf("[%7.2f s] TB: falha ao publicar (motivo=%s, estado=%d)\n",
                  seconds(nowMs), mqttStateName(g_mqtt.state()),
                  g_mqtt.state());
    m_connected = false;
    return false;
  }

  Serial.printf("[%7.2f s] TB: publicado (%s): %s\n", seconds(nowMs), origin,
                g_payload);
  return true;
}

void ThingsBoard::registerPublished(uint32_t nowMs, float volumeL) {
  m_hasPublished = true;
  m_lastPublishedL = volumeL;
  m_lastPublishMs = nowMs;
  ++m_publishCount;
}

bool ThingsBoard::enqueue(uint32_t nowMs, const TelemetrySample &sample) {
  if (!m_queue.append(sample.timestampMs, sample.volumeL)) {
    if (!m_queueFullWarned) {
      m_queueFullWarned = true;
      Serial.printf(
          "[%7.2f s] TB: fila offline recusou a amostra (sem espaco ou falha "
          "de gravacao); registros existentes preservados\n",
          seconds(nowMs));
    }
    // Gravacao NAO confirmada: a decisao continua pendente. O baseline da
    // politica fica intocado para a proxima amostra tentar de novo - sem
    // registro persistido nao existe nada para o replay enviar.
    return false;
  }
  // Gravacao confirmada: a amostra preservada ocupa o lugar da publicacao,
  // entao o estado da politica avanca igual ao caminho online. Limpar
  // `m_publishAfterReconnect` aqui e o que impede RECONNECT de virar estado
  // continuo e enfileirar um registro a cada ciclo de amostragem.
  registerPublished(nowMs, sample.volumeL);
  m_publishAfterReconnect = false;
  return true;
}

bool ThingsBoard::publishUptime(uint32_t nowMs, uint32_t uptimeS) {
  char payload[48];
  const int written = snprintf(payload, sizeof(payload), "{\"%s\":%lu}",
                               TB_KEY_UPTIME,
                               static_cast<unsigned long>(uptimeS));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(payload)) {
    return false;
  }

  if (!g_mqtt.publish(TB_ATTRIBUTES_TOPIC, payload, false)) {
    Serial.printf("[%7.2f s] TB: falha ao publicar atributo %s\n",
                  seconds(nowMs), TB_KEY_UPTIME);
    return false;
  }

  Serial.printf("[%7.2f s] TB: atributo %s\n", seconds(nowMs), payload);
  return true;
}

void ThingsBoard::drainQueue(uint32_t nowMs) {
  if (!m_queue.ready()) {
    return;
  }

  const uint32_t startedMs = millis();
  uint8_t budget = TB_QUEUE_REPLAY_MAX_RECORDS;

  while (budget > 0 && (millis() - startedMs) < TB_QUEUE_REPLAY_BUDGET_MS) {
    queue_record::Record record;
    const OfflineQueue::PeekResult result = m_queue.peek(record);

    if (result == OfflineQueue::PeekResult::EMPTY) {
      return;
    }
    if (result == OfflineQueue::PeekResult::ERROR) {
      if (!m_queueErrorWarned) {
        m_queueErrorWarned = true;
        Serial.printf(
            "[%7.2f s] TB: fila offline com falha; registros preservados\n",
            seconds(nowMs));
      }
      return;
    }

    // Ordem cronologica: publica o registro mais antigo e so entao avanca.
    if (!publishValues(nowMs, record.timestampMs, record.volumeL,
                       "fila offline")) {
      return;
    }
    if (!m_queue.confirm()) {
      return;
    }
    registerPublished(nowMs, record.volumeL);
    --budget;
  }
}

void ThingsBoard::update(uint32_t nowMs, const TelemetrySample &sample) {
  // 1) Conectividade: mantem ou retoma a sessao. Nada aqui interrompe a
  // funcao - decidir se a amostra deve ser preservada nao depende do
  // transporte (mesma separacao do Ecowitt2TB).
  if (WiFi.status() != WL_CONNECTED) {
    if (m_connected) {
      Serial.printf("[%7.2f s] TB: sessao encerrada (Wi-Fi indisponivel)\n",
                    seconds(nowMs));
      m_connected = false;
      m_publishAfterReconnect = true;
    }
  } else if (!g_mqtt.connected()) {
    if (m_connected) {
      Serial.printf("[%7.2f s] TB: desconectado (motivo=%s, estado=%d)\n",
                    seconds(nowMs), mqttStateName(g_mqtt.state()),
                    g_mqtt.state());
      m_connected = false;
      m_publishAfterReconnect = true;
    }

    if ((nowMs - m_lastAttemptMs) >= m_retryDelayMs) {
      m_lastAttemptMs = nowMs;

      if (!connectSession(nowMs)) {
        // Backoff: cada falha seguida dobra o intervalo ate o limite, para nao
        // gastar o loop tentando um broker que esta fora do ar.
        m_retryDelayMs = m_retryDelayMs * 2UL;
        if (m_retryDelayMs > MQTT_MAX_RECONNECT_MS) {
          m_retryDelayMs = MQTT_MAX_RECONNECT_MS;
        }
      } else {
        m_retryDelayMs = MQTT_RECONNECT_MS;
        m_connected = true;
        // Garante uma telemetria imediatamente apos reconectar, mesmo sem
        // mudanca de volume. Na primeira conexao do boot, a politica devolve
        // FIRST; enfileirar a amostra tambem satisfaz esta marca (enqueue()).
        m_publishAfterReconnect = true;
        // Atributo com o uptime ATUAL: vale mesmo que o replay da fila ainda
        // esteja enviando registros historicos (que nao guardam uptime).
        publishUptime(nowMs, sample.uptimeS);
      }
    }
  } else {
    m_connected = true;
  }

  const bool online = m_connected && g_mqtt.connected();

  if (online) {
    g_mqtt.loop();  // mantem a sessao (keepalive) sem bloquear
    // Historico pendente primeiro: preserva a ordem cronologica no ThingsBoard
    // e impede que a leitura atual passe a frente dos registros antigos.
    drainQueue(nowMs);
  }

  // 2) Politica de publicacao: decide SEMPRE, com ou sem transporte.
  if (!sample.valid) {
    return;
  }

  if (sample.timestampMs == 0) {
    if (!m_clockWarned) {
      m_clockWarned = true;
      Serial.printf(
          "[%7.2f s] TB: aguardando NTP; telemetria nao sera publicada nem "
          "enfileirada\n",
          seconds(nowMs));
    }
    return;
  }

  publish_policy::Input input;
  input.sampleValid = sample.valid;
  input.hasPublished = m_hasPublished;
  input.publishAfterReconnect = m_publishAfterReconnect;
  input.volumeL = sample.volumeL;
  input.lastPublishedL = m_lastPublishedL;
  input.nowMs = nowMs;
  input.lastPublishMs = m_lastPublishMs;
  input.deltaLiters = TB_PUBLISH_DELTA_LITERS;
  input.heartbeatMs = TB_HEARTBEAT_INTERVAL_MS;
  input.minIntervalMs = TB_MIN_PUBLISH_INTERVAL_MS;

  const publish_policy::Reason reason = publish_policy::decide(input);

  if (reason == publish_policy::Reason::NONE) {
    return;
  }

  // 3) Resolucao da decisao: o transporte e apenas o meio preferido. O backlog
  // pendente sempre tem prioridade e, sem sessao, a amostra vira registro.
  const bool pendingBacklog = m_queue.hasPending();

  if (online && !pendingBacklog &&
      publishValues(nowMs, sample.timestampMs, sample.volumeL,
                    publish_policy::reasonName(reason))) {
    registerPublished(nowMs, sample.volumeL);
    m_publishAfterReconnect = false;
    // Toda publicacao de volume_l atualiza o atributo com o uptime atual.
    publishUptime(nowMs, sample.uptimeS);
    return;
  }

  // Sem sessao, com backlog ou com falha de envio: preserva a amostra com o
  // timestamp ORIGINAL. `enqueue()` so avanca o estado da politica depois de
  // confirmar que o registro foi gravado.
  if (enqueue(nowMs, sample)) {
    Serial.printf("[%7.2f s] TB: amostra na fila (%s, %s), pendentes=%lu\n",
                  seconds(nowMs), publish_policy::reasonName(reason),
                  pendingBacklog
                      ? "backlog pendente"
                      : (online ? "falha de publicacao" : "offline"),
                  static_cast<unsigned long>(m_queue.pendingRecords()));
  }
}
