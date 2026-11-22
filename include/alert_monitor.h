#pragma once

#include <Arduino.h>

#include "alerts.h"

// ============================================================================
//  Alertas de nivel - runtime (estado + LittleFS)
//
//  Responsabilidades:
//    - avaliar o volume FILTRADO e enfileirar os eventos de transicao;
//    - persistir configuracao, estado e pendentes nos dois slots A/B, para o
//      reboot (inclusive o controlado pela guarda) nao reenviar nem perder
//      alerta;
//    - entregar UMA mensagem por vez ao Telegram, no padrao peek/confirm.
//
//  O LittleFS e o mesmo ja montado pela fila offline (OfflineQueue::begin) e
//  NUNCA e formatado aqui: sem ele, os alertas funcionam em RAM com os limites
//  de include/config.h e o /w avisa que nao houve persistencia.
//
//  Nada aqui aloca memoria dinamica: apenas POD em .bss.
// ============================================================================
class AlertMonitor {
 public:
  // Deve ser chamado depois de ThingsBoard::begin (que monta o LittleFS).
  void begin(uint32_t nowMs);

  // Chamado continuamente pelo loop, ANTES de Telegram::update (para o alerta
  // sair na mesma consulta). Sem amostra valida ou dentro do ALERT_SETTLE_MS
  // nada e avaliado.
  void update(uint32_t nowMs, float volumeL, bool sampleValid);

  bool persisted() const { return m_persisted; }
  alerts::Config config() const { return m_state.config; }
  alerts::Level level() const { return m_state.level; }
  // Nivel que o volume corresponde AGORA, sem emitir evento. Usado pelo aviso de
  // boot, que precisa do estado REAL do tanque e nao do estado persistido (que
  // pode ser de antes do reboot).
  alerts::Level levelForVolume(float volumeL) const {
    return alerts::levelFor(m_state.config, volumeL);
  }
  // Estado em texto ("Normal"/"Atenção"/"Critico") para o log serial.
  const char *levelText() const;
  uint8_t pendingCount() const { return m_state.pendingCount; }
  uint32_t events() const { return m_events; }

  // Aplica limites novos. Recalcula o estado para o volume atual SEM emitir
  // evento (o operador so ajustou os limites - nao houve transicao) e zera a
  // fila de pendentes, que perdeu o sentido. Devolve OK quando aplicado.
  alerts::ConfigError applyConfig(const alerts::Config &cfg, float volumeL,
                                  bool sampleValid);

  // Proxima mensagem a enviar (a mais antiga). false = nada pendente. A taxa
  // recente (include/rate.h) entra na mensagem quando o chamador a tem; sem base
  // (ou sem leitura) a linha da taxa simplesmente nao aparece.
  bool peekPending(float rateLph, bool hasRate, char *out, size_t cap) const;
  // Confirma o envio da mensagem devolvida por peekPending() e persiste.
  void confirmPending();

 private:
  void load();
  void store();
  bool readRecord(const char *path, uint8_t *raw) const;

  alerts::Persisted m_state;
  uint32_t m_bootMs = 0;
  uint32_t m_events = 0;
  uint8_t m_activeSlot = 0;  // slot que contem o estado carregado
  bool m_persisted = false;
  bool m_fsWarned = false;
  // Primeira avaliacao util do boot ja passou? (ver o rebaseline em update())
  bool m_rebaselined = false;
};