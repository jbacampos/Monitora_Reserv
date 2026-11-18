#include "thingsboard.h"

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClient.h>

#include "config.h"
#include "credentials.h"

namespace {

double seconds(uint32_t nowMs) { return static_cast<double>(nowMs) / 1000.0; }

// Transporte e sessao MQTT unicos do firmware.
WiFiClient g_transport;
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
  g_transport.setTimeout(MQTT_SOCKET_TIMEOUT_MS);

  g_mqtt.setServer(ACTIVE_MQTT_HOST, ACTIVE_MQTT_PORT);
  g_mqtt.setSocketTimeout(MQTT_CONNACK_TIMEOUT_S);
  g_mqtt.setBufferSize(MQTT_BUFFER_SIZE);

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
          "Conexao MQTT nao sera tentada.\n",
          activeSecretsFileName());
    }
    return false;
  }

  // clientId estavel e curto, sem alocacao de String.
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "%s-%06x", DEVICE_ID,
           static_cast<unsigned>(ESP.getChipId()));

  Serial.printf("[%7.2f s] TB: conectando a %s:%u (clientId=%s)\n",
                seconds(nowMs), ACTIVE_MQTT_HOST,
                static_cast<unsigned>(ACTIVE_MQTT_PORT), clientId);

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

bool ThingsBoard::publishTelemetry(uint32_t nowMs,
                                   const TelemetrySample &sample,
                                   publish_policy::Reason reason) {
  const int written = snprintf(
      g_payload, sizeof(g_payload), "{\"%s\":%.1f,\"%s\":%.1f,\"%s\":%lu}",
      TB_KEY_VOLUME, static_cast<double>(sample.volumeL), TB_KEY_LEVEL,
      static_cast<double>(sample.levelMm), TB_KEY_UPTIME,
      static_cast<unsigned long>(sample.uptimeS));

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

  Serial.printf("[%7.2f s] TB: publicado (%s): %s\n", seconds(nowMs),
                publish_policy::reasonName(reason), g_payload);
  return true;
}

void ThingsBoard::update(uint32_t nowMs, const TelemetrySample &sample) {
  if (WiFi.status() != WL_CONNECTED) {
    if (m_connected) {
      Serial.printf("[%7.2f s] TB: sessao encerrada (Wi-Fi indisponivel)\n",
                    seconds(nowMs));
      m_connected = false;
      m_publishAfterReconnect = true;
    }
    return;
  }

  if (!g_mqtt.connected()) {
    if (m_connected) {
      Serial.printf("[%7.2f s] TB: desconectado (motivo=%s, estado=%d)\n",
                    seconds(nowMs), mqttStateName(g_mqtt.state()),
                    g_mqtt.state());
      m_connected = false;
      m_publishAfterReconnect = true;
    }

    if ((nowMs - m_lastAttemptMs) < m_retryDelayMs) {
      return;
    }
    m_lastAttemptMs = nowMs;

    if (!connectSession(nowMs)) {
      // Backoff: cada falha seguida dobra o intervalo ate o limite, para nao
      // gastar o loop tentando um broker que esta fora do ar.
      m_retryDelayMs = m_retryDelayMs * 2UL;
      if (m_retryDelayMs > MQTT_MAX_RECONNECT_MS) {
        m_retryDelayMs = MQTT_MAX_RECONNECT_MS;
      }
      return;
    }

    m_retryDelayMs = MQTT_RECONNECT_MS;
    m_connected = true;
    // Garante uma telemetria imediatamente apos reconectar, mesmo sem mudanca
    // de volume. Na primeira conexao do boot, a politica devolve FIRST.
    m_publishAfterReconnect = true;
  }

  m_connected = true;
  g_mqtt.loop();  // mantem a sessao (keepalive) sem bloquear

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

  if (!publishTelemetry(nowMs, sample, reason)) {
    return;
  }

  m_hasPublished = true;
  m_lastPublishedL = sample.volumeL;
  m_lastPublishMs = nowMs;
  m_publishAfterReconnect = false;
  ++m_publishCount;
}
