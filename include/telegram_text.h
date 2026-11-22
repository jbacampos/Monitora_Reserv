#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "alerts.h"
#include "boot_reason.h"
#include "rate.h"

// ---------------------------------------------------------------------------
//  Literais na FLASH (nao na DRAM)
//
//  No ESP8266 o .rodata vive na DRAM (ver o linker script do core): cada byte de
//  texto custa 1 byte de HEAP. As mensagens do Telegram sao a maior parte do
//  texto do firmware, entao elas vao para a flash com PSTR(), que o core coloca
//  em ".irom0.pstr.*" - uma regiao mapeada em memoria, lida como um ponteiro
//  normal. No host (espelhos em Python, sem Arduino) a macro nao existe e os
//  literais continuam comuns, preservando a "camada pura".
//
//  O QUE muda e apenas ONDE o texto mora: a formatacao continua sendo feita pelo
//  snprintf/Serial.printf de sempre (newlib), inclusive %f e %lu.
// ---------------------------------------------------------------------------
#if defined(ARDUINO)
#include <pgmspace.h>
#define TG_TEXT(s) PSTR(s)
#else
#define TG_TEXT(s) (s)
#endif

// ============================================================================
//  Telegram - camada de TEXTO pura (sem Arduino, sem rede, sem alocacao)
//
//  Concentra a classificacao de comandos e a construcao das mensagens, que sao
//  a unica parte testavel sem hardware. O modulo src/telegram.cpp cuida de TLS,
//  HTTP, JSON e das consultas ao historico; nada aqui conhece a rede.
//
//  Comandos: /help (/h), /status (/s) e /warnings (/w). /reboot e /ota NAO
//  existem e nao sao anunciados. Comandos enderecados a um bot
//  ("/status@QualquerBot") nao existem neste projeto: qualquer texto com '@' e
//  recusado.
//
//  As mensagens usam acentuacao em UTF-8 (o urlEncodeInto() de src/telegram.cpp
//  codifica byte a byte) e separador de milhar com ponto nos litros.
// ============================================================================
enum class TgCommand : uint8_t {
  None = 0,
  Help = 1,
  Status = 2,
  Warnings = 3,
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
// Depois compara com o PRIMEIRO token: "/help"/"/h"/"/status"/"/s" (sem nada
// depois) e "/w"/"/warnings" (com ou sem argumentos).
inline bool tgMatchCommand(const char *normalized, const char *name) {
  const size_t len = strlen(name);
  return strncmp(normalized, name, len) == 0 &&
         (normalized[len] == '\0' || tgIsSpace(normalized[len]));
}

// Argumentos de um comando que os aceita: aponta para dentro de `normalized`,
// ja sem os separadores entre o comando e o primeiro argumento.
inline const char *tgArgsOf(const char *normalized) {
  while (*normalized != '\0' && !tgIsSpace(*normalized)) {
    ++normalized;
  }
  while (*normalized != '\0' && tgIsSpace(*normalized)) {
    ++normalized;
  }
  return normalized;
}

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

  // O token e comparado por prefixo delimitado; o que vem depois fica em `out`
  // para o parser de argumentos (apenas /w usa).
  if (tgMatchCommand(out, "/help") || tgMatchCommand(out, "/h")) {
    return tgArgsOf(out)[0] == '\0' ? TgCommand::Help : TgCommand::None;
  }
  if (tgMatchCommand(out, "/status") || tgMatchCommand(out, "/s")) {
    return tgArgsOf(out)[0] == '\0' ? TgCommand::Status : TgCommand::None;
  }
  if (tgMatchCommand(out, "/w") || tgMatchCommand(out, "/warnings")) {
    return TgCommand::Warnings;
  }
  return TgCommand::None;
}

// ---------------------------------------------------------------------------
//  Argumentos do /w
// ---------------------------------------------------------------------------
enum class WarningsParse : uint8_t {
  SHOW = 0,     // "/w" sozinho: mostrar os limites
  SET = 1,      // "/w lr lc hist": aplicar
  INVALID = 2,  // quantidade/formato invalidos
};

// Le tres inteiros separados por espacos. Devolve false em qualquer caractere
// fora de digito, argumento a mais ou valor fora de 1..100. A coerencia entre os
// tres (ordem e histerese) fica em alerts::validate(), para a mensagem de erro
// poder dizer exatamente o que esta errado.
inline bool parseWarningsTriple(const char *args, uint8_t &riskPct,
                                uint8_t &criticalPct, uint8_t &histPct) {
  if (args == nullptr || args[0] == '\0') {
    return false;
  }

  unsigned values[3] = {0, 0, 0};
  int index = 0;

  while (*args != '\0') {
    while (tgIsSpace(*args)) {
      ++args;
    }
    if (*args == '\0') {
      break;
    }
    if (index >= 3 || *args < '0' || *args > '9') {
      return false;
    }

    unsigned value = 0;
    while (*args >= '0' && *args <= '9') {
      value = value * 10u + static_cast<unsigned>(*args - '0');
      if (value > 100u) {
        return false;  // ja passou do maximo aceito
      }
      ++args;
    }
    if (*args != '\0' && !tgIsSpace(*args)) {
      return false;  // "20x", "20.5", ...
    }
    values[index++] = value;
  }

  if (index != 3) {
    return false;
  }
  if (values[0] == 0 || values[1] == 0 || values[2] == 0) {
    return false;  // 0 nao e percentual valido
  }

  riskPct = static_cast<uint8_t>(values[0]);
  criticalPct = static_cast<uint8_t>(values[1]);
  histPct = static_cast<uint8_t>(values[2]);
  return true;
}

inline WarningsParse parseWarnings(const char *args, alerts::Config &cfg) {
  if (args == nullptr || args[0] == '\0') {
    return WarningsParse::SHOW;
  }

  uint8_t riskPct = 0;
  uint8_t criticalPct = 0;
  uint8_t histPct = 0;
  if (!parseWarningsTriple(args, riskPct, criticalPct, histPct)) {
    return WarningsParse::INVALID;
  }

  cfg.riskPct = riskPct;
  cfg.criticalPct = criticalPct;
  cfg.histPct = histPct;
  return WarningsParse::SET;
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

// Inteiro com separador de milhar por ponto ("830", "1.234", "116.651"). O ponto
// e "seguro" no URL-encoding, portanto nao encarece o POST.
inline void formatGroupedLong(long value, char *out, size_t outSize) {
  char digits[16];
  int count = 0;
  const bool negative = value < 0;
  unsigned long magnitude = negative ? static_cast<unsigned long>(-value)
                                     : static_cast<unsigned long>(value);

  do {
    digits[count++] = static_cast<char>('0' + (magnitude % 10UL));
    magnitude /= 10UL;
  } while (magnitude != 0 && count < 15);

  size_t written = 0;
  if (negative && written + 1 < outSize) {
    out[written++] = '-';
  }
  for (int i = count - 1; i >= 0; --i) {
    if (written + 1 >= outSize) {
      break;
    }
    out[written++] = digits[i];
    if (i > 0 && (i % 3) == 0 && written + 1 < outSize) {
      out[written++] = '.';
    }
  }
  out[written] = '\0';
}

// Litros inteiros SEM unidade ("1.234"), para frases que ja dizem "litros".
inline void formatLitersBare(float liters, char *out, size_t outSize) {
  const long value =
      static_cast<long>(liters < 0.0f ? liters - 0.5f : liters + 0.5f);
  formatGroupedLong(value, out, outSize);
}

// Litros em "digitos-emoji" (keycap): "1️⃣0️⃣3️⃣.5️⃣8️⃣7️⃣". Sem unidade, para o
// bloco de nivel das mensagens de boot e de alerta; o separador de milhar
// continua sendo o ponto. Cada digito vira ASCII + U+FE0F (seletor de variacao)
// + U+20E3 (keycap) = 7 bytes, montados byte a byte para nao gastar DRAM com
// uma tabela de literais. Buffer seguro: 48 B (pior caso "116.651" = 43 B).
inline void formatLitersKeycap(float liters, char *out, size_t outSize) {
  char grouped[12];
  formatLitersBare(liters, grouped, sizeof(grouped));

  size_t written = 0;
  for (const char *p = grouped; *p != '\0'; ++p) {
    if (*p >= '0' && *p <= '9') {
      if (written + 7 >= outSize) {
        break;
      }
      out[written++] = *p;
      out[written++] = static_cast<char>(0xEF);
      out[written++] = static_cast<char>(0xB8);
      out[written++] = static_cast<char>(0x8F);
      out[written++] = static_cast<char>(0xE2);
      out[written++] = static_cast<char>(0x83);
      out[written++] = static_cast<char>(0xA3);
    } else {
      if (written + 1 >= outSize) {
        break;
      }
      out[written++] = *p;
    }
  }
  out[written] = '\0';
}

// Taxa recente ("↑ 1.950 L/h", "↓ 950 L/h", "→ estável" ou "sem base"). O valor
// e arredondado a 10 L/h e o sinal vira seta: a mensagem nunca mostra "-". Dentro
// da banda estavel o que existe e a quantizacao do ADC (1 contagem = 116 L), nao
// movimento do tanque.
inline void formatRate(float litersPerHour, bool hasRate, char *out,
                       size_t outSize) {
  if (!hasRate) {
    snprintf(out, outSize, TG_TEXT("sem base"));
    return;
  }
  if (litersPerHour > -rate::RATE_STABLE_LPH &&
      litersPerHour < rate::RATE_STABLE_LPH) {
    snprintf(out, outSize, TG_TEXT("→ estável"));
    return;
  }

  const float magnitude =
      (litersPerHour < 0.0f) ? -litersPerHour : litersPerHour;
  const long rounded = static_cast<long>(magnitude / 10.0f + 0.5f) * 10L;
  char grouped[16];
  formatGroupedLong(rounded, grouped, sizeof(grouped));
  snprintf(out, outSize, TG_TEXT("%s %s L/h"),
           (litersPerHour > 0.0f) ? TG_TEXT("↑") : TG_TEXT("↓"), grouped);
}

// Nome do estado do nivel ("Normal", "Atenção", "Crítico"). Usado pelo aviso de
// boot e pelos dois blocos do /w - uma unica fonte para os TRES textos.
inline const char *alertLevelName(alerts::Level level) {
  switch (level) {
    case alerts::Level::RISK:
      return TG_TEXT("Atenção");
    case alerts::Level::CRITICAL:
      return TG_TEXT("Crítico");
    case alerts::Level::NORMAL:
    default:
      return TG_TEXT("Normal");
  }
}

// Emblema do nível: emoji + nome em negrito (parse_mode=HTML). Fonte unica do
// NORMAL/ATENÇÃO/CRÍTICO do aviso de boot e das quatro mensagens de alerta.
inline const char *alertLevelBadge(alerts::Level level) {
  switch (level) {
    case alerts::Level::RISK:
      return TG_TEXT("⚠️ <b>ATENÇÃO</b>");
    case alerts::Level::CRITICAL:
      return TG_TEXT("‼️ <b>CRÍTICO</b>");
    case alerts::Level::NORMAL:
    default:
      return TG_TEXT("✅ <b>NORMAL</b>");
  }
}

// ---------------------------------------------------------------------------
//  /status
// ---------------------------------------------------------------------------
// Tudo o que a mensagem mostra, ja extraido do historico por quem chama. O
// modulo Telegram NAO recalcula agregados: apenas formata o que History devolve.
struct TelegramStatusData {
  bool hasVolume = false;  // ha leitura AGORA (context.sampleValid)
  float volumeL = 0.0f;    // volume filtrado, em litros
  // Estado de AGORA do tanque (o MESMO do aviso de boot): o emblema precisa do
  // volume filtrado E dos limites vigentes, por isso vem pronto do chamador.
  bool hasLevel = false;
  alerts::Level level = alerts::Level::NORMAL;
  // Taxa recente (include/rate.h), ja calculada pelo chamador.
  bool hasRate = false;
  float rateLph = 0.0f;

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

// Uma linha da tabela: rotulo a esquerda, valores a direita. Periodo sem dados
// mostra "-" (mantem as QUATRO linhas e o alinhamento). Colunas de 7: cabem
// "Período"/"30 dias" e o maior volume possivel (116.651), com o POST ainda
// folgado (medido: 538 de 640 B no pior caso - o espaco custa 3 B no encoding).
inline void formatStatusRow(const char *label, bool hasData, float minL,
                            float maxL, char *out, size_t outSize) {
  char low[12];
  char high[12];
  if (hasData) {
    const long lo = static_cast<long>(minL < 0.0f ? minL - 0.5f : minL + 0.5f);
    const long hi = static_cast<long>(maxL < 0.0f ? maxL - 0.5f : maxL + 0.5f);
    formatGroupedLong(lo, low, sizeof(low));
    formatGroupedLong(hi, high, sizeof(high));
  } else {
    snprintf(low, sizeof(low), TG_TEXT("-"));
    snprintf(high, sizeof(high), TG_TEXT("-"));
  }

  snprintf(out, outSize, TG_TEXT("%-7s|%7s|%7s|"), label, low, high);
}

// Bloco <pre> do /status. So dentro de <pre> o Telegram usa a fonte FIXA e
// preserva os espacos multiplos; fora dele a tabela colapsa e desalinha.
inline void formatStatusTable(const TelegramStatusData &data, char *out,
                              size_t outSize) {
  char today[40];
  char yesterday[40];
  char last7[40];
  char last30[40];
  formatStatusRow(TG_TEXT("Hoje"), data.hasToday, data.todayMinL,
                  data.todayMaxL, today, sizeof(today));
  formatStatusRow(TG_TEXT("Ontem"), data.hasYesterday, data.yesterdayMinL,
                  data.yesterdayMaxL, yesterday, sizeof(yesterday));
  formatStatusRow(TG_TEXT("7 dias"), data.hasLast7, data.last7MinL,
                  data.last7MaxL, last7, sizeof(last7));
  formatStatusRow(TG_TEXT("30 dias"), data.hasLast30, data.last30MinL,
                  data.last30MaxL, last30, sizeof(last30));

  snprintf(out, outSize,
           TG_TEXT("<pre>%-7s|%7s|%7s|\n"
                   "-------|-------|-------|\n"
                   "%s\n%s\n%s\n%s</pre>"),
           TG_TEXT("Período"), TG_TEXT("  Mín  "), TG_TEXT("  Máx  "), today,
           yesterday, last7, last30);
}

// Mensagem do /status: o estado de AGORA (mesmo padrao do aviso de boot e das
// mensagens de alerta: emblema, linha em branco, litros em digitos-emoji, linha
// em branco), a taxa recente, a tabela de minimos e maximos, a versao e o uptime.
inline void buildStatusMessage(const TelegramStatusData &data,
                               const char *version, uint32_t uptimeS, char *out,
                               size_t outSize) {
  char uptime[32];
  formatUptime(uptimeS, uptime, sizeof(uptime));

  char table[224];
  formatStatusTable(data, table, sizeof(table));

  char rateText[24];
  formatRate(data.rateLph, data.hasRate, rateText, sizeof(rateText));

  if (!data.hasLevel) {
    snprintf(out, outSize,
             TG_TEXT("Volume atual: sem leitura\n"
                     "\n"
                     "Taxa (1 h): %s\n"
                     "\n"
                     "%s\n"
                     "Firmware: %s\n"
                     "Uptime: %s"),
             rateText, table, version, uptime);
    return;
  }

  char liters[48];
  formatLitersKeycap(data.volumeL, liters, sizeof(liters));
  snprintf(out, outSize,
           TG_TEXT("Volume atual: %s\n"
                   "\n"
                   "%s\n"
                   "\n"
                   "Taxa (1 h): %s\n"
                   "\n"
                   "%s\n"
                   "Firmware: %s\n"
                   "Uptime: %s"),
           alertLevelBadge(data.level), liters, rateText, table, version,
           uptime);
}

// Mensagem do /help: somente os comandos que existem nesta etapa.
inline void buildHelpMessage(char *out, size_t outSize) {
  snprintf(out, outSize,
           TG_TEXT("Comandos disponíveis:\n"
                   "/help ou /h - lista de comandos disponíveis\n"
                   "/status ou /s - situação atual do reservatório\n"
                   "/warnings ou /w - limites e alertas de nível"));
}

// ---------------------------------------------------------------------------
//  Aviso de boot
// ---------------------------------------------------------------------------
// Mesmo padrao visual e de conteudo do Monitora_DG:
//   Reiniciado.
//   Motivo: Energização
//   Versão: 1.0.0
//   Volume atual: ✅ <b>NORMAL</b>      (ou "Volume atual: sem leitura")
//   <linha em branco>
//   1️⃣0️⃣3️⃣.5️⃣8️⃣7️⃣                     (litros em digitos-emoji; some sem leitura)
// O texto usa acentuacao (UTF-8) como todas as mensagens do MoniRes; o motivo
// vem da tabela unica de include/boot_reason.h.
//
// A linha do nivel e o estado AGORA (derivado do volume que o chamador passa),
// nao o estado persistido: o reboot pode ter acontecido com o tanque em outro
// nivel, e e essa a informacao que da contexto aos alertas que vierem depois
// (inclusive o rebaseline silencioso de AlertMonitor, que nao envia mensagem).
inline void buildBootMessage(uint8_t bootReason, const char *version,
                             alerts::Level level, bool hasVolume, float volumeL,
                             char *out, size_t outSize) {
  if (!hasVolume) {
    snprintf(out, outSize,
             TG_TEXT("Reiniciado.\n"
                     "Motivo: %s\n"
                     "Versão: %s\n"
                     "Volume atual: sem leitura"),
             bootReasonDescription(bootReason), version);
    return;
  }

  char liters[48];
  formatLitersKeycap(volumeL, liters, sizeof(liters));
  snprintf(out, outSize,
           TG_TEXT("Reiniciado.\n"
                   "Motivo: %s\n"
                   "Versão: %s\n"
                   "Volume atual: %s\n\n"
                   "%s"),
           bootReasonDescription(bootReason), version, alertLevelBadge(level),
           liters);
}

// ---------------------------------------------------------------------------
//  /warnings - limites
// ---------------------------------------------------------------------------
// Motivo, em portugues, de uma configuracao recusada.
inline const char *warningsErrorText(alerts::ConfigError error) {
  switch (error) {
    case alerts::ConfigError::RISK_RANGE:
      return TG_TEXT("a atenção deve ficar entre 1 e 100");
    case alerts::ConfigError::CRITICAL_RANGE:
      return TG_TEXT("o crítico deve ficar entre 1 e 100");
    case alerts::ConfigError::HIST_RANGE:
      return TG_TEXT("a histerese deve ficar entre 1 e 100");
    case alerts::ConfigError::ORDER:
      return TG_TEXT("o crítico deve ser menor que a atenção");
    case alerts::ConfigError::HIST_OVERFLOW:
      return TG_TEXT("atenção + histerese não pode passar de 100");
    case alerts::ConfigError::OK:
    default:
      return TG_TEXT("limites válidos");
  }
}

// Bloco comum aos modos do /w: os limites vigentes (em % e em litros) e o
// estado atual.
inline void buildWarningsLimits(alerts::Config cfg, alerts::Level level,
                                float volumeL, bool hasVolume, char *out,
                                size_t outSize) {
  char risk[24];
  char critical[24];
  formatGroupedLong(
      static_cast<long>(alerts::enterRiskLiters(cfg) + 0.5f), risk,
      sizeof(risk));
  formatGroupedLong(
      static_cast<long>(alerts::enterCriticalLiters(cfg) + 0.5f), critical,
      sizeof(critical));

  char state[64];
  if (hasVolume) {
    char current[24];
    formatGroupedLong(static_cast<long>(volumeL + 0.5f), current,
                      sizeof(current));
    snprintf(state, sizeof(state), TG_TEXT("%s - %s L"), alertLevelName(level),
             current);
  } else {
    snprintf(state, sizeof(state), TG_TEXT("%s (sem leitura)"),
             alertLevelName(level));
  }

  snprintf(out, outSize,
           TG_TEXT("Atenção: %u%% = %s L\n"
                   "Crítico: %u%% = %s L\n"
                   "Histerese: %u pontos\n"
                   "Estado atual: %s"),
           static_cast<unsigned>(cfg.riskPct), risk,
           static_cast<unsigned>(cfg.criticalPct), critical,
           static_cast<unsigned>(cfg.histPct), state);
}

// "/w" (sem argumentos): referencia, limites e estado atual.
inline void buildWarningsMessage(alerts::Config cfg, alerts::Level level,
                                 float volumeL, bool hasVolume, char *out,
                                 size_t outSize) {
  char limits[200];
  buildWarningsLimits(cfg, level, volumeL, hasVolume, limits, sizeof(limits));

  char reference[24];
  formatGroupedLong(static_cast<long>(alerts::maxReferenceLiters() + 0.5f),
                    reference, sizeof(reference));

  snprintf(out, outSize,
           TG_TEXT("Alertas de nível\n"
                   "Referência (100%%): %s L\n"
                   "%s"),
           reference, limits);
}

// "/w lr lc hist" aceito.
inline void buildWarningsAppliedMessage(alerts::Config cfg, alerts::Level level,
                                        float volumeL, bool hasVolume, char *out,
                                        size_t outSize) {
  char limits[200];
  buildWarningsLimits(cfg, level, volumeL, hasVolume, limits, sizeof(limits));

  snprintf(out, outSize, TG_TEXT("Limites atualizados.\n%s"), limits);
}

// "/w ..." recusado: motivo + uso + limites vigentes (que NAO mudaram).
inline void buildWarningsErrorMessage(const char *reason, alerts::Config cfg,
                                      char *out, size_t outSize) {
  snprintf(out, outSize,
           TG_TEXT("Limites não alterados: %s\n"
                   "Uso: /w lr lc hist (percentuais de 1 a 100)\n"
                   "Atuais: atenção %u%%, crítico %u%%, histerese %u%%"),
           reason, static_cast<unsigned>(cfg.riskPct),
           static_cast<unsigned>(cfg.criticalPct),
           static_cast<unsigned>(cfg.histPct));
}

// ---------------------------------------------------------------------------
//  Alertas enviados pelo firmware (nao sao resposta a comando)
// ---------------------------------------------------------------------------
// As quatro mensagens: ALERTA ao piorar o nível, RECUPERAÇÃO ao melhorar, sempre
// com o cabeçalho padrao, o nível resultante (emoji + nome em negrito), os litros
// do instante da transicao em digitos-emoji e - quando ha base - a taxa recente.
inline void buildAlertMessage(alerts::Event event, float volumeL, float rateLph,
                              bool hasRate, char *out, size_t outSize) {
  char liters[48];
  formatLitersKeycap(volumeL, liters, sizeof(liters));

  // A taxa entra como ultima linha; sem base a mensagem fica exatamente como era.
  char tail[40];
  tail[0] = '\0';
  if (hasRate) {
    char rateText[24];
    formatRate(rateLph, true, rateText, sizeof(rateText));
    snprintf(tail, sizeof(tail), TG_TEXT("\n\nTaxa (1 h): %s"), rateText);
  }

  switch (event) {
    case alerts::Event::RISK_ENTERED:
      snprintf(out, outSize,
               TG_TEXT("***** ALERTA *****\n\nVolume atual: %s\n\n%s%s"),
               alertLevelBadge(alerts::Level::RISK), liters, tail);
      break;
    case alerts::Event::CRITICAL_ENTERED:
      snprintf(out, outSize,
               TG_TEXT("***** ALERTA *****\n\nVolume atual: %s\n\n%s%s"),
               alertLevelBadge(alerts::Level::CRITICAL), liters, tail);
      break;
    case alerts::Event::CRITICAL_RECOVERED:
      snprintf(out, outSize,
               TG_TEXT("***** RECUPERAÇÃO *****\n\nVolume atual: %s\n\n%s%s"),
               alertLevelBadge(alerts::Level::RISK), liters, tail);
      break;
    case alerts::Event::RISK_RECOVERED:
      snprintf(out, outSize,
               TG_TEXT("***** RECUPERAÇÃO *****\n\nVolume atual: %s\n\n%s%s"),
               alertLevelBadge(alerts::Level::NORMAL), liters, tail);
      break;
    case alerts::Event::NONE:
    default:
      out[0] = '\0';
      break;
  }
}
