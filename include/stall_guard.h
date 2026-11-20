#pragma once

#include <stddef.h>
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
//  O reboot devolve o heap inteiro e refaz as sessoes TLS do zero. Ele e o
//  ULTIMO RECURSO: so acontece quando um servico esta bloqueado E o heap esta
//  fragmentado a ponto de nao dar mais para recriar a sessao TLS (maior bloco
//  contiguo abaixo de STALL_GUARD_MAX_BLOCK_FLOOR por STALL_GUARD_FRAGMENTED_MS).
//  Servico parado com heap intacto NAO reinicia. As regras ficam em config.h.
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
  // Instante (millis) do ultimo sinal de vida de cada servico. 0 = nenhum desde
  // o boot; nesse caso o silencio conta a partir do fim da carencia do boot. No
  // MQTT, com a sessao de pe o main passa millis() (o servico esta vivo).
  uint32_t telegramLastOkMs = 0;
  uint32_t mqttLastOkMs = 0;
  // Maior bloco contiguo livre (ESP.getMaxFreeBlockSize()) lido pelo chamador.
  // Autoriza o reboot quando fica abaixo de STALL_GUARD_MAX_BLOCK_FLOOR: abaixo
  // disso nem a recuperacao do gate consegue recriar a sessao TLS (heap "nao mais
  // administravel"). A persistencia e cobrada por STALL_GUARD_FRAGMENTED_MS.
  size_t maxFreeBlockBytes = 0;
};

// Inicia a janela de carencia (nenhum reboot nos primeiros
// STALL_GUARD_GRACE_MS) e zera os avisos.
void begin(uint32_t nowMs);

// Chame a cada loop(). Avisa uma vez quando o bloqueio passa de
// STALL_GUARD_FIRST_WARN_MS (e quando a fragmentacao persiste) e reinicia o ESP
// SOMENTE ao atingir TELEGRAM_STALL_REBOOT_MS / MQTT_STALL_REBOOT_MS com o maior
// bloco contiguo abaixo de STALL_GUARD_MAX_BLOCK_FLOOR por
// STALL_GUARD_FRAGMENTED_MS. Retorna somente quando NAO reiniciou - ESP.restart()
// nao retorna.
void update(uint32_t nowMs, const Input &input);

}  // namespace stall_guard
