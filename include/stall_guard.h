#pragma once

#include <stdint.h>

// ============================================================================
//  Guarda de bloqueio - reboot controlado
//
//  Por que existe: os dois servicos externos podem ficar bloqueados SEM prazo.
//
//    Telegram - o gate de heap pode nunca reabrir (fragmentacao). O caso real de
//               2026-11-20: sessao TLS morta presa com 8,4 KB alocados, maior
//               bloco em 5 240 B, `tg=adiado` por horas e /help sem resposta.
//               O gate e reavaliado a cada loop(), mas nao existe prazo que o
//               faca crescer: sem reboot, o bot ficou mudo por 6 h.
//
//    MQTT      - o broker pode recusar/derrubar a sessao indefinidamente (TLS
//               recusado, registro de certificado grande, credencial invalida).
//               O backoff chega a MQTT_MAX_RECONNECT_MS e fica repetindo para
//               sempre, sem devolver o heap que a tentativa consumiu.
//
//  O reboot devolve o heap inteiro e refaz as sessoes TLS do zero. As regras
//  temporais ficam em config.h (STALL_GUARD_*).
//
//  Nada aqui aloca: apenas contadores em .bss. Chame update() a cada loop().
// ============================================================================
namespace stall_guard {

struct Input {
  // Sem Wi-Fi o bloqueio nao e dos servicos: nada e contado nem reiniciado.
  bool wifiConnected = false;
  // Credenciais presentes? Com token/chat pendentes nao existe o que conectar,
  // e reiniciar em loop nao resolveria (o firmware avisa no boot).
  bool telegramEnabled = false;
  bool mqttEnabled = false;
  // Instante (millis) do ultimo sucesso de cada servico. 0 = nenhum desde o
  // boot; nesse caso o silencio conta a partir do fim da carencia do boot.
  uint32_t telegramLastOkMs = 0;
  uint32_t mqttLastOkMs = 0;
};

// Inicia a janela de carencia (nenhum reboot nos primeiros
// STALL_GUARD_GRACE_MS) e zera os avisos.
void begin(uint32_t nowMs);

// Chame a cada loop(). Avisa uma vez quando o bloqueio passa de
// STALL_GUARD_FIRST_WARN_MS e reinicia o ESP ao atingir
// TELEGRAM_STALL_REBOOT_MS / MQTT_STALL_REBOOT_MS. Retorna somente quando NAO
// reiniciou - ESP.restart() nao retorna.
void update(uint32_t nowMs, const Input &input);

}  // namespace stall_guard
