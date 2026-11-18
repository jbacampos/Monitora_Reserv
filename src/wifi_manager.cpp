#include "wifi_manager.h"

#include <ESP8266WiFi.h>

#include "config.h"
#include "credentials.h"

namespace {

double seconds(uint32_t nowMs) { return static_cast<double>(nowMs) / 1000.0; }

// Motivo da ultima desconexao, informado pelo SDK no evento de estacao.
// Escrito no contexto de rede e apenas lido pelo loop, em update().
volatile bool g_reasonPending = false;
volatile uint8_t g_reasonCode = 0;
WiFiEventHandler g_disconnectHandler;

const char *wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_NO_SHIELD:
      return "WL_NO_SHIELD";
    case WL_IDLE_STATUS:
      return "WL_IDLE_STATUS";
    case WL_NO_SSID_AVAIL:
      return "WL_NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED:
      return "WL_SCAN_COMPLETED";
    case WL_CONNECTED:
      return "WL_CONNECTED";
    case WL_CONNECT_FAILED:
      return "WL_CONNECT_FAILED";
    case WL_CONNECTION_LOST:
      return "WL_CONNECTION_LOST";
    case WL_WRONG_PASSWORD:
      return "WL_WRONG_PASSWORD";
    case WL_DISCONNECTED:
      return "WL_DISCONNECTED";
  }
  return "WL_STATUS_DESCONHECIDO";
}

// Codigo 802.11/SDK do motivo da desconexao. O numero e sempre registrado,
// mesmo quando o codigo nao e mapeado abaixo.
const char *wifiReasonName(uint8_t reason) {
  switch (reason) {
    case 1:
      return "UNSPECIFIED";
    case 2:
      return "AUTH_EXPIRE";
    case 3:
      return "AUTH_LEAVE";
    case 4:
      return "ASSOC_EXPIRE";
    case 5:
      return "ASSOC_TOOMANY";
    case 8:
      return "ASSOC_LEAVE";
    case 15:
      return "4WAY_HANDSHAKE_TIMEOUT";
    case 200:
      return "BEACON_TIMEOUT";
    case 201:
      return "NO_AP_FOUND";
    case 202:
      return "AUTH_FAIL";
    case 203:
      return "ASSOC_FAIL";
    case 204:
      return "HANDSHAKE_TIMEOUT";
  }
  return "NAO_MAPEADO";
}

}  // namespace

void WiFiManager::begin(uint32_t nowMs) {
  m_credentialsPending = wifiCredentialsPending();

  if (m_credentialsPending) {
    Serial.printf(
        "Wi-Fi: credenciais pendentes em %s (preencha e recompile). "
        "Conexao nao sera tentada.\n",
        activeSecretsFileName());
    return;
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  // O SDK informa o codigo do motivo da queda por este evento; o handler e
  // mantido vivo pelo WiFiEventHandler guardado em escopo de arquivo.
  g_disconnectHandler = WiFi.onStationModeDisconnected(
      [](const WiFiEventStationModeDisconnected &event) {
        g_reasonCode = event.reason;
        g_reasonPending = true;
      });

  m_started = true;
  startConnection(nowMs);
}

void WiFiManager::startConnection(uint32_t nowMs) {
  m_lastAttemptMs = nowMs;
  ++m_attemptCount;

  Serial.printf("[%7.2f s] Wi-Fi: conectando a '%s' (tentativa %u)\n",
                seconds(nowMs), ACTIVE_WIFI_SSID,
                static_cast<unsigned>(m_attemptCount));

  WiFi.begin(ACTIVE_WIFI_SSID, ACTIVE_WIFI_PASSWORD);
}

void WiFiManager::update(uint32_t nowMs) {
  if (!m_started) {
    return;
  }

  const bool isConnected = connected();

  if (isConnected != m_wasConnected) {
    m_wasConnected = isConnected;

    if (isConnected) {
      m_attemptCount = 0;
      logConnected(nowMs);
    } else {
      logDisconnected(nowMs);
    }
  }

  if (isConnected) {
    return;
  }

  // Reconexao agendada: nunca bloqueia o loop.
  if ((nowMs - m_lastAttemptMs) < WIFI_RETRY_MS) {
    return;
  }

  // Limpa o estado da tentativa anterior antes de comecar outra.
  WiFi.disconnect(false);
  startConnection(nowMs);
}

bool WiFiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

void WiFiManager::logConnected(uint32_t nowMs) {
  Serial.printf("[%7.2f s] Wi-Fi: conectado (ip=%s, canal=%d, rssi=%d dBm)\n",
                seconds(nowMs), WiFi.localIP().toString().c_str(),
                static_cast<int>(WiFi.channel()), WiFi.RSSI());
}

void WiFiManager::logDisconnected(uint32_t nowMs) {
  const wl_status_t status = WiFi.status();
  unsigned reasonCode = 0;
  const char *reasonName = "SEM_CODIGO";

  if (g_reasonPending) {
    reasonCode = g_reasonCode;
    reasonName = wifiReasonName(static_cast<uint8_t>(reasonCode));
    g_reasonPending = false;
  }

  Serial.printf(
      "[%7.2f s] Wi-Fi: desconectado (motivo=%s, codigo=%u, status=%s) - "
      "nova tentativa em %lu ms\n",
      seconds(nowMs), reasonName, reasonCode, wifiStatusName(status),
      static_cast<unsigned long>(WIFI_RETRY_MS));
}
