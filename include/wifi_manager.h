#pragma once

#include <Arduino.h>

// ============================================================================
//  Gerenciamento da conexao Wi-Fi
//
//  A rede usada e sempre ACTIVE_WIFI_SSID (credentials.h), derivada de
//  TEST_MODE. Nada aqui bloqueia o loop: as tentativas de reconexao sao
//  agendadas por millis() e o estado e apenas consultado.
// ============================================================================
class WiFiManager {
 public:
  void begin(uint32_t nowMs);

  // Deve ser chamada continuamente pelo loop (sem delay()).
  void update(uint32_t nowMs);

  bool connected() const;
  bool credentialsPending() const { return m_credentialsPending; }

 private:
  void startConnection(uint32_t nowMs);
  void logConnected(uint32_t nowMs);
  void logDisconnected(uint32_t nowMs);

  bool m_started = false;
  bool m_credentialsPending = false;
  bool m_wasConnected = false;
  uint32_t m_lastAttemptMs = 0;
  uint16_t m_attemptCount = 0;
};
