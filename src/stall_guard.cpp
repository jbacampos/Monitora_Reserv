#include "stall_guard.h"

#include <Arduino.h>

#include "config.h"

// ============================================================================
//  Guarda de bloqueio - implementacao
//
//  O silencio de cada servico e medido a partir do ultimo sinal de vida: a
//  ultima consulta OK do Telegram (Telegram::lastSuccessMs) e, no MQTT, a sessao
//  de pe (o main passa millis() enquanto connected()) ou, sem sessao, o ultimo
//  aceite/publicacao (ThingsBoard::lastActivityMs). Nao existe contador
//  incremental: se o servico volta a responder, o silencio cai sozinho e o aviso
//  e rearmado - nada precisa ser resetado a mao.
//
//  O reboot e o ULTIMO RECURSO: alem do bloqueio de um servico, exige o maior
//  bloco contiguo abaixo de STALL_GUARD_MAX_BLOCK_FLOOR por
//  STALL_GUARD_FRAGMENTED_MS (heap inadministravel, sem volta).
// ============================================================================
namespace stall_guard {
namespace {

uint32_t g_bootMs = 0;
bool g_telegramWarned = false;
bool g_mqttWarned = false;
bool g_fragWarned = false;
// Estado da fragmentacao inadministravel: instante em que o maior bloco caiu
// abaixo do piso. A persistencia (STALL_GUARD_FRAGMENTED_MS) e o que autoriza o
// reboot - um vale transitorio nao conta.
bool g_fragTracking = false;
uint32_t g_fragSinceMs = 0;

double seconds(uint32_t ms) { return static_cast<double>(ms) / 1000.0; }

// Silencio atual em ms. Sem nenhum sucesso desde o boot (lastOkMs == 0) o
// relogio comeca no boot, e nao "ha infinito": a carencia ja foi aplicada antes
// de chegar aqui.
//
// O carimbo pode estar no FUTURO em relacao a `nowMs`: o loop captura `now` no
// inicio e os modulos marcam millis() depois de bloquear (o poll do Telegram
// segura ~1,4 s com handshake + resposta). Medido em campo em 2026-11-20: a
// consulta marcou lastSuccess=61 700 ms enquanto a guarda recebia now=60 270 ms;
// a subtracao wrap-safe virou 4 294 966 s e o ESP reiniciou com 61 s de uptime,
// logo depois do primeiro /help. Carimbo no futuro = silencio zero.
uint32_t silenceMs(uint32_t nowMs, uint32_t lastOkMs, bool enabled) {
  if (!enabled) {
    return 0;  // credenciais pendentes: nao ha o que reconectar
  }
  const uint32_t reference = (lastOkMs == 0) ? g_bootMs : lastOkMs;
  const uint32_t delta = static_cast<uint32_t>(nowMs - reference);
  return (static_cast<int32_t>(delta) < 0) ? 0 : delta;
}

// Um aviso por episodio de bloqueio. Rearma quando o silencio volta a ficar
// abaixo do limiar (o servico respondeu).
void warnOnce(bool &warned, uint32_t nowMs, uint32_t silence, const char *who,
              uint32_t limitMs) {
  if (silence < STALL_GUARD_FIRST_WARN_MS) {
    warned = false;
    return;
  }
  if (warned) {
    return;
  }
  warned = true;
  Serial.printf(
      "[%7.2f s] GUARDA: %s sem sucesso ha %lu s (reboot em %lu s se o heap "
      "estiver inadministravel)\n",
      seconds(nowMs), who, static_cast<unsigned long>(silence / 1000UL),
      static_cast<unsigned long>((limitMs - silence) / 1000UL));
}

// Aviso unico por episodio de fragmentacao inadministravel. Diferente do
// warnOnce, dispara assim que o bloco fica preso abaixo do piso por tempo
// suficiente, mesmo antes de um servico ficar bloqueado ha 30 min - o operador
// ve a escalada antes do reboot.
void warnFragOnce(uint32_t nowMs, uint32_t block) {
  if (g_fragWarned) {
    return;
  }
  g_fragWarned = true;
  Serial.printf(
      "[%7.2f s] GUARDA: heap inadministravel (maior bloco=%u B < %u B) ha %lu s;"
      " reboot so se um servico continuar bloqueado\n",
      seconds(nowMs), static_cast<unsigned>(block),
      static_cast<unsigned>(STALL_GUARD_MAX_BLOCK_FLOOR),
      static_cast<unsigned long>(
          static_cast<uint32_t>(nowMs - g_fragSinceMs) / 1000UL));
}

}  // namespace

void begin(uint32_t nowMs) {
  g_bootMs = nowMs;
  g_telegramWarned = false;
  g_mqttWarned = false;
  g_fragWarned = false;
  g_fragTracking = false;
  g_fragSinceMs = 0;
}

void update(uint32_t nowMs, const Input &input) {
  // Wi-Fi fora: o reboot nao ajuda (as sessoes TLS nem existem) e so custaria
  // uptime e um snapshot do historico.
  if (!input.wifiConnected) {
    return;
  }

  // Carencia do boot: da tempo de Wi-Fi, NTP, MQTT e a primeira consulta do
  // Telegram acontecerem antes de qualquer contagem.
  if (static_cast<uint32_t>(nowMs - g_bootMs) < STALL_GUARD_GRACE_MS) {
    return;
  }

  const uint32_t telegramSilence =
      silenceMs(nowMs, input.telegramLastOkMs, input.telegramEnabled);
  const uint32_t mqttSilence =
      silenceMs(nowMs, input.mqttLastOkMs, input.mqttEnabled);

  warnOnce(g_telegramWarned, nowMs, telegramSilence, "Telegram",
           TELEGRAM_STALL_REBOOT_MS);
  warnOnce(g_mqttWarned, nowMs, mqttSilence, "MQTT", MQTT_STALL_REBOOT_MS);

  const bool telegramStuck = telegramSilence >= TELEGRAM_STALL_REBOOT_MS;
  const bool mqttStuck = mqttSilence >= MQTT_STALL_REBOOT_MS;
  const bool serviceStuck = telegramStuck || mqttStuck;

  // Fragmentacao inadministravel: maior bloco contiguo abaixo do piso de
  // emergencia (nem recriar a sessao TLS daria). Exige persistencia, senao o
  // reboot reagiria a um vale transitorio da publicacao/handshake.
  if (input.maxFreeBlockBytes < STALL_GUARD_MAX_BLOCK_FLOOR) {
    if (!g_fragTracking) {
      g_fragTracking = true;
      g_fragSinceMs = nowMs;
    }
  } else {
    g_fragTracking = false;
    g_fragSinceMs = 0;
    g_fragWarned = false;
  }
  const bool fragSustained =
      g_fragTracking &&
      static_cast<uint32_t>(nowMs - g_fragSinceMs) >= STALL_GUARD_FRAGMENTED_MS;
  if (fragSustained) {
    warnFragOnce(nowMs, static_cast<uint32_t>(input.maxFreeBlockBytes));
  }

  // O reboot e o ULTIMO RECURSO: exige as DUAS condicoes - um servico parado ha
  // tempo demais E um heap fragmentado sem volta. Um servico parado com o heap
  // intacto nao justifica reiniciar (o MQTT se recupera no backoff; o Telegram
  // tem o piso de emergencia do gate).
  if (!serviceStuck || !fragSustained) {
    return;
  }

  const char *who = telegramStuck ? "Telegram" : "MQTT";
  const uint32_t silence = telegramStuck ? telegramSilence : mqttSilence;

  // O heap fragmentado nao se desfaz sozinho e o servico nao se recupera: o
  // reboot devolve o heap inteiro.
  Serial.printf(
      "[%7.2f s] GUARDA: reboot controlado (%s bloqueado ha %lu s, maior bloco="
      "%u B < %u B) heap=%u/%u B\n",
      seconds(nowMs), who, static_cast<unsigned long>(silence / 1000UL),
      static_cast<unsigned>(input.maxFreeBlockBytes),
      static_cast<unsigned>(STALL_GUARD_MAX_BLOCK_FLOOR),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
  Serial.flush();
  ESP.restart();
}

}  // namespace stall_guard
