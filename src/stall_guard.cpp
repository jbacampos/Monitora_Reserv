#include "stall_guard.h"

#include <Arduino.h>

#include "config.h"

// ============================================================================
//  Guarda de bloqueio - implementacao
//
//  O silencio de cada servico e medido a partir do ultimo sucesso registrado por
//  ele (Telegram::lastSuccessMs / ThingsBoard::lastConnectedMs). Nao existe
//  contador incremental: se o servico volta a responder, o silencio cai sozinho
//  e o aviso e rearmado - nada precisa ser resetado a mao.
// ============================================================================
namespace stall_guard {
namespace {

uint32_t g_bootMs = 0;
bool g_telegramWarned = false;
bool g_mqttWarned = false;

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
  Serial.printf("[%7.2f s] GUARDA: %s sem sucesso ha %lu s (reboot em %lu s)\n",
                seconds(nowMs), who,
                static_cast<unsigned long>(silence / 1000UL),
                static_cast<unsigned long>((limitMs - silence) / 1000UL));
}

}  // namespace

void begin(uint32_t nowMs) {
  g_bootMs = nowMs;
  g_telegramWarned = false;
  g_mqttWarned = false;
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
  if (!telegramStuck && !mqttStuck) {
    return;
  }

  // O reboot e a ultima saida: o heap fragmentado nao se desfaz sozinho e o
  // broker pode estar recusando a sessao desde o boot.
  Serial.printf("[%7.2f s] GUARDA: reboot controlado (%s ha %lu s) heap=%u/%u B\n",
                seconds(nowMs),
                telegramStuck ? "Telegram bloqueado" : "MQTT bloqueado",
                static_cast<unsigned long>(
                    (telegramStuck ? telegramSilence : mqttSilence) / 1000UL),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
  Serial.flush();
  ESP.restart();
}

}  // namespace stall_guard
