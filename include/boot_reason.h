#pragma once

#include <stdint.h>
#include <string.h>

// ============================================================================
//  Motivo do boot - tabela UNICA de descricao
//
//  Mesmo padrao do Monitora_DG: o CODIGO e o valor estavel; a tabela abaixo e a
//  unica fonte do texto humano, usada pelo aviso de boot do Telegram e pelo log
//  serial. Nada mais no projeto repete essas descricoes.
//
//   1  Power On               "Energizacao (power-on)"
//   2  Hardware Watchdog      "Watchdog de hardware"
//   3  Exception              "Excecao de software"
//   4  Software Watchdog      "Watchdog de software"
//   5  Software/System reset  "Reinicio (/reboot)"
//   6  Deep-Sleep Wake        "Despertar de deep sleep"
//   7  External System        "Reset externo"
//   8  Unknown                "Desconhecido"
//   9  OTA                    "Atualizacao OTA (/ota)"
//
//  Sem Arduino e sem alocacao: testavel no host (tools/host_test).
// ============================================================================
enum BootReason : uint8_t {
  BOOT_REASON_NONE = 0,
  BOOT_REASON_POWER_ON = 1,
  BOOT_REASON_HARDWARE_WATCHDOG = 2,
  BOOT_REASON_EXCEPTION = 3,
  BOOT_REASON_SOFTWARE_WATCHDOG = 4,
  BOOT_REASON_SOFTWARE_RESTART = 5,
  BOOT_REASON_DEEP_SLEEP = 6,
  BOOT_REASON_EXTERNAL_SYSTEM = 7,
  BOOT_REASON_UNKNOWN = 8,
  BOOT_REASON_OTA = 9,
};

// Descricao em portugues. NONE nunca deveria chegar aqui (o classificador
// devolve UNKNOWN no pior caso), mas devolve "Desconhecido" para que o aviso de
// boot jamais imprima "Motivo:" vazio.
inline const char *bootReasonDescription(uint8_t code) {
  switch (code) {
    case BOOT_REASON_POWER_ON:
      return "Energizacao";
    case BOOT_REASON_HARDWARE_WATCHDOG:
      return "Watchdog de hardware";
    case BOOT_REASON_EXCEPTION:
      return "Excecao de software";
    case BOOT_REASON_SOFTWARE_WATCHDOG:
      return "Watchdog de software";
    case BOOT_REASON_SOFTWARE_RESTART:
      return "Reinicio (/reboot)";
    case BOOT_REASON_DEEP_SLEEP:
      return "Retorno de deep sleep";
    case BOOT_REASON_EXTERNAL_SYSTEM:
      return "Reset externo";
    case BOOT_REASON_OTA:
      return "Atualizacao OTA (/ota)";
    case BOOT_REASON_UNKNOWN:
    default:
      return "Desconhecido";
  }
}

// Traduz ESP.getResetReason() (a string exata do SDK) para o codigo estavel.
// `otaMarker` representa a marca persistida antes de uma atualizacao OTA e tem
// PRIORIDADE sobre a string: o ESP8266 reinicia apos gravar a flash como
// "Software/System restart", exatamente a mesma string de um /reboot, portanto a
// marca e a unica forma de distinguir os dois. Funcao pura: coberta pelos testes
// de host.
inline uint8_t classifyBootReason(bool otaMarker, const char *resetReason) {
  if (otaMarker) {
    return BOOT_REASON_OTA;
  }

  if (resetReason == nullptr) {
    return BOOT_REASON_UNKNOWN;
  }

  if (strcmp(resetReason, "Power On") == 0) {
    return BOOT_REASON_POWER_ON;
  }

  if (strcmp(resetReason, "Hardware Watchdog") == 0) {
    return BOOT_REASON_HARDWARE_WATCHDOG;
  }

  if (strcmp(resetReason, "Exception") == 0) {
    return BOOT_REASON_EXCEPTION;
  }

  if (strcmp(resetReason, "Software Watchdog") == 0) {
    return BOOT_REASON_SOFTWARE_WATCHDOG;
  }

  if (strcmp(resetReason, "Software/System restart") == 0) {
    return BOOT_REASON_SOFTWARE_RESTART;
  }

  if (strcmp(resetReason, "Deep-Sleep Wake") == 0) {
    return BOOT_REASON_DEEP_SLEEP;
  }

  if (strcmp(resetReason, "External System") == 0) {
    return BOOT_REASON_EXTERNAL_SYSTEM;
  }

  return BOOT_REASON_UNKNOWN;
}
