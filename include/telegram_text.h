#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "boot_reason.h"

// ============================================================================
//  Telegram - camada de TEXTO pura (sem Arduino, sem rede, sem alocacao)
//
//  Concentra a classificacao de comandos e a construcao das mensagens, que sao
//  a unica parte testavel sem hardware. O modulo src/telegram.cpp cuida de TLS,
//  HTTP, JSON e das consultas ao historico; nada aqui conhece a rede.
//
//  Nesta etapa existem apenas /help e /status. /reboot e /ota NAO existem e nao
//  sao anunciados. Comandos enderecados a um bot ("/status@QualquerBot") nao
//  existem neste projeto: qualquer texto com '@' e recusado.
// ============================================================================
enum class TgCommand : uint8_t {
  None = 0,
  Help = 1,
  Status = 2,
};

// Tamanho maximo de uma mensagem enviada (texto puro, antes do URL-encoding).
constexpr size_t TG_MESSAGE_MAX = 384;

// ---------------------------------------------------------------------------
//  Comandos
// ---------------------------------------------------------------------------
inline bool tgIsSpace(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// Normaliza o texto recebido em `out` (obrigatorio):
//   - remove espacos/tabs/quebras nas duas pontas;
//   - converte para minusculas;
//   - RECUSA qualquer texto que contenha '@'. O MoniRes nao se endereca por
//     username: "/status@MoniResBot", "/help@OutroBot" e qualquer variacao sao
//     simplesmente ignorados, independentemente do nome informado.
// Depois compara com os comandos desta etapa ("/help" e "/status", apenas).
inline TgCommand parseTgCommand(const char *text, char *out, size_t outSize) {
  if (out == nullptr || outSize == 0) {
    return TgCommand::None;
  }
  out[0] = '\0';
  if (text == nullptr) {
    return TgCommand::None;
  }

  const char *begin = text;
  while (*begin != '\0' && tgIsSpace(*begin)) {
    ++begin;
  }

  const char *end = begin + strlen(begin);
  while (end > begin && tgIsSpace(end[-1])) {
    --end;
  }

  // Enderecamento a bot: nao existe getMe, username nem configuracao de nome.
  for (const char *p = begin; p < end; ++p) {
    if (*p == '@') {
      return TgCommand::None;
    }
  }

  size_t len = 0;
  for (const char *p = begin; p < end; ++p) {
    if (len + 1 >= outSize) {
      return TgCommand::None;  // comando absurdo: nao trunca, ignora
    }
    char c = *p;
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
    out[len++] = c;
  }
  out[len] = '\0';

  if (strcmp(out, "/help") == 0) {
    return TgCommand::Help;
  }
  if (strcmp(out, "/status") == 0) {
    return TgCommand::Status;
  }
  return TgCommand::None;
}

// ---------------------------------------------------------------------------
//  Formatacao de valores
// ---------------------------------------------------------------------------

// Uptime em segundos acumulados -> "3d 4h 5m", "4h 5m" ou "5m" (unidades
// omitidas por magnitude, sem zeros a esquerda). O valor vem de UptimeCounter
// (include/uptime.h), que nao reinicia no rollover de millis(); nao tem relacao
// com hora civil.
inline void formatUptime(uint32_t uptimeS, char *out, size_t outSize) {
  const uint32_t days = uptimeS / 86400UL;
  const uint32_t hours = (uptimeS / 3600UL) % 24UL;
  const uint32_t minutes = (uptimeS / 60UL) % 60UL;

  if (days > 0) {
    snprintf(out, outSize, "%lud %luh %lum", static_cast<unsigned long>(days),
             static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  } else if (hours > 0) {
    snprintf(out, outSize, "%luh %lum", static_cast<unsigned long>(hours),
             static_cast<unsigned long>(minutes));
  } else {
    snprintf(out, outSize, "%lum", static_cast<unsigned long>(minutes));
  }
}

// Litros inteiros ("1234 L"). O volume do firmware e sempre >= 0.
inline void formatLiters(float liters, char *out, size_t outSize) {
  const long value =
      static_cast<long>(liters < 0.0f ? liters - 0.5f : liters + 0.5f);
  snprintf(out, outSize, "%ld L", value);
}

// ---------------------------------------------------------------------------
//  /status
// ---------------------------------------------------------------------------
// Tudo o que a mensagem mostra, ja extraido do historico por quem chama. O
// modulo Telegram NAO recalcula agregados: apenas formata o que History devolve.
struct TelegramStatusData {
  bool hasVolume = false;
  float volumeL = 0.0f;

  bool hasToday = false;
  float todayMinL = 0.0f;
  float todayMaxL = 0.0f;

  bool hasYesterday = false;
  float yesterdayMinL = 0.0f;
  float yesterdayMaxL = 0.0f;

  bool hasLast7 = false;
  float last7MinL = 0.0f;
  float last7MaxL = 0.0f;

  bool hasLast30 = false;
  float last30MinL = 0.0f;
  float last30MaxL = 0.0f;
};

// "min 1200 L | max 1310 L" ou "Sem dados".
inline void formatMinMax(bool hasData, float minL, float maxL, char *out,
                         size_t outSize) {
  if (!hasData) {
    snprintf(out, outSize, "Sem dados");
    return;
  }

  const long low = static_cast<long>(minL < 0.0f ? minL - 0.5f : minL + 0.5f);
  const long high = static_cast<long>(maxL < 0.0f ? maxL - 0.5f : maxL + 0.5f);
  snprintf(out, outSize, "min %ld L | max %ld L", low, high);
}

// Mensagem do /status: os sete grupos pedidos, nesta ordem, e nada mais.
inline void buildStatusMessage(const TelegramStatusData &data,
                               const char *version, uint32_t uptimeS, char *out,
                               size_t outSize) {
  char volume[24];
  if (data.hasVolume) {
    formatLiters(data.volumeL, volume, sizeof(volume));
  } else {
    snprintf(volume, sizeof(volume), "Sem dados");
  }

  char today[40];
  formatMinMax(data.hasToday, data.todayMinL, data.todayMaxL, today,
               sizeof(today));

  char yesterday[40];
  formatMinMax(data.hasYesterday, data.yesterdayMinL, data.yesterdayMaxL,
               yesterday, sizeof(yesterday));

  char last7[40];
  formatMinMax(data.hasLast7, data.last7MinL, data.last7MaxL, last7,
               sizeof(last7));

  char last30[40];
  formatMinMax(data.hasLast30, data.last30MinL, data.last30MaxL, last30,
               sizeof(last30));

  char uptime[32];
  formatUptime(uptimeS, uptime, sizeof(uptime));

  snprintf(out, outSize,
           "Reservatorio\n"
           "Volume atual: %s\n"
           "Hoje: %s\n"
           "Ontem: %s\n"
           "Ultimos 7 dias: %s\n"
           "Ultimos 30 dias: %s\n"
           "Firmware: %s\n"
           "Uptime: %s",
           volume, today, yesterday, last7, last30, version, uptime);
}

// Mensagem do /help: somente os comandos que existem nesta etapa.
inline void buildHelpMessage(char *out, size_t outSize) {
  snprintf(out, outSize,
           "Comandos disponiveis:\n"
           "/help - lista de comandos disponiveis\n"
           "/status - situacao atual do reservatorio");
}

// ---------------------------------------------------------------------------
//  Aviso de boot
// ---------------------------------------------------------------------------
// Mesmo padrao visual e de conteudo do Monitora_DG: tres linhas, nada mais.
//   Monitora_Reserv reiniciado.
//   Versao: 1.0.0
//   Motivo: Energizacao
// Texto em ASCII (como todas as mensagens do MoniRes); o motivo vem da tabela
// unica de include/boot_reason.h.
inline void buildBootMessage(uint8_t bootReason, const char *version, char *out,
                             size_t outSize) {
  snprintf(out, outSize,
           "Monitora_Reserv reiniciado.\n"
           "Versao: %s\n"
           "Motivo: %s",
           version, bootReasonDescription(bootReason));
}
