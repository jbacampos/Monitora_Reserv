#pragma once

#include <Arduino.h>

#include "alert_monitor.h"
#include "history.h"
#include "telegram_text.h"

// ============================================================================
//  Telegram - Bot API consultada DIRETAMENTE pelo ESP8266
//
//  Nao existe servico, bot ou proxy intermediario: o proprio firmware abre TLS
//  com api.telegram.org (CA em telegram_ca.h), faz GET getUpdates e responde
//  com POST sendMessage. Comandos desta etapa: /help, /status e /warnings, com
//  as abreviacoes /h, /s e /w.
//
//  Alem das respostas, o modulo entrega o aviso de boot e os ALERTAS de nivel
//  (include/alert_monitor.h) pelo mesmo caminho de envio, uma mensagem por
//  consulta.
//
//  Ciclo de vida:
//    begin(nowMs)  - apenas estado; nenhuma conexao (nao bloqueia o setup);
//    update(nowMs) - se o intervalo de polling venceu, executa UMA consulta e
//                    volta. A consulta e sincrona mas limitada a
//                    TELEGRAM_RESPONSE_TIMEOUT_MS, com yield() nas esperas.
//                    A sessao TLS e REAPROVEITADA entre consultas e renovada a
//                    cada TELEGRAM_SESSION_MAX_AGE_MS (o servidor fecha a
//                    conexao com ~1 h); uma sessao com socket morto e liberada
//                    ANTES de o gate de heap medir a memoria, para os ~8,4 KB
//                    dela voltarem ao heap em vez de ficarem presos.
//
//  Regras de operacao:
//    - uma consulta por vez (o intervalo conta do INICIO da anterior);
//    - primeiro polling com offset=-1&limit=1 descarta o backlog antigo;
//    - depois, offset = update_id + 1 do ultimo update processado;
//    - o offset avanca ANTES de responder, para nunca repetir uma resposta;
//    - update_id e offset vivem apenas em RAM nesta versao;
//    - mensagens de chats nao autorizados sao ignoradas em silencio;
//    - falha de rede/TLS/JSON apenas encerra a consulta: o firmware continua.
// ============================================================================
struct TelegramContext {
  // Volume atual calculado pelo firmware (o MESMO valor enviado ao ThingsBoard).
  float volumeL = 0.0f;
  // Uptime acumulado em segundos (include/uptime.h): nao reinicia no rollover de
  // millis() e nao e hora civil.
  uint32_t uptimeS = 0;
  bool sampleValid = false;
  // Taxa de variacao recente (include/rate.h), em litros por minuto, derivada da
  // MESMA media movel do volume. `hasRate` e falso enquanto a janela nao tem
  // base (ou depois de um reboot, quando a serie em RAM comeca do zero).
  float rateLpm = 0.0f;
  bool hasRate = false;
  // Fonte dos agregados do /status. Nao ha segunda fonte de historico.
  const History *history = nullptr;
  // Alertas de nivel: o /w le e escreve os limites e o modulo Telegram entrega
  // UMA mensagem pendente por consulta (peek/confirm). Nao-const de proposito.
  AlertMonitor *alerts = nullptr;
};

class Telegram {
 public:
  void begin(uint32_t nowMs);

  // Chamado continuamente pelo loop principal.
  void update(uint32_t nowMs, const TelegramContext &context);

  bool enabled() const { return m_enabled; }
  uint32_t polls() const { return m_polls; }
  uint32_t commands() const { return m_commands; }
  uint32_t failures() const { return m_failures; }
  int64_t updateOffset() const { return m_updateOffset; }

  // Instante (millis) da ultima consulta BEM-SUCEDIDA (HTTP 200 + ok:true).
  // 0 = nenhuma desde o boot. Unica fonte usada pela guarda de bloqueio
  // (stall_guard.h) para decidir o reboot controlado: se o bot nao responde ha
  // TELEGRAM_STALL_REBOOT_MS, a stack TLS/heap nao se recupera sozinha.
  uint32_t lastSuccessMs() const { return m_lastSuccessMs; }

  // Diagnostico: estado legivel para a linha periodica do Serial, sem alocacao
  // dinamica. Formas possiveis em `out`:
  //   "off"               - modulo desativado (credenciais ausentes)
  //   "adiado(<bloco>B)"  - gate de heap fechado, consulta adiada
  //   "aguardando"        - nenhuma consulta executada desde o boot
  //   "ultima=<s>s"       - segundos desde o INICIO da ultima consulta
  void stateText(char *out, size_t cap, uint32_t nowMs) const;

 private:
  void pollOnce(uint32_t nowMs, const TelegramContext &context);
  bool sendMessage(int64_t chatId, const char *text);
  void answerCommand(TgCommand command, const TelegramContext &context,
                     int64_t chatId);

  // Aviso de boot: uma mensagem por boot, enviada pelo MESMO caminho de envio
  // (ensureConnection + sendMessage) que as respostas de comando. Nao altera a
  // consulta (getUpdates) nem o offset.
  void sendBootNotice(uint32_t nowMs, const TelegramContext &context);

  // Entrega UM alerta pendente por consulta, tambem pelo caminho de envio, um
  // pouco antes da consulta (a mesma sessao serve as duas coisas). Sem pendencia
  // - ou sem conseguir abrir a sessao - nao faz nada: o alerta continua na fila
  // (persistida) e a proxima consulta tenta de novo.
  void flushPendingAlerts(uint32_t nowMs, const TelegramContext &context);

  bool m_enabled = false;
  bool m_firstPoll = true;
  bool m_pendingWarned = false;
  // Diagnostico: true depois do primeiro aviso de heap insuficiente. A partir
  // dai o aviso passa a ser limitado por m_lastHeapWarnMs
  // (TELEGRAM_HEAP_WARN_PERIOD_MS). Nao altera o adiamento.
  bool m_heapWarned = false;
  int64_t m_updateOffset = 0;
  uint32_t m_lastPollMs = 0;
  uint32_t m_lastHeapWarnMs = 0;
  // Instante da ultima consulta bem-sucedida (0 = nenhuma). Alimenta a guarda de
  // bloqueio, nunca o agendamento do polling.
  uint32_t m_lastSuccessMs = 0;

  // Diagnostico do gate de heap: instante em que o adiamento comecou. Como o
  // gate e reavaliado a cada loop(), o tempo "fechado ha N s" e continuo.
  bool m_gateBlocked = false;
  uint32_t m_gateBlockedSinceMs = 0;

  // Aviso de boot: motivo classificado uma unica vez no begin().
  uint8_t m_bootReason = 0;
  uint8_t m_bootNoticeAttempts = 0;
  bool m_bootNoticeSent = false;
  uint32_t m_polls = 0;
  uint32_t m_commands = 0;
  uint32_t m_failures = 0;
};
