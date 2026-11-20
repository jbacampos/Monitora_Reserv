#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "queue_record.h"
#include "volume.h"

// ============================================================================
//  Alertas de nivel - logica PURA (sem Arduino, sem flash, sem rede)
//
//  Decide QUAL mensagem enviar quando o volume cruza os dois limiares e define o
//  formato do registro que preserva configuracao, estado e alertas pendentes no
//  LittleFS. Tudo aqui e testavel no host (tools/host_test).
//
//  Eixo de comparacao: o volume em LITROS do firmware (o MESMO valor do display,
//  do MQTT e do /status). Como a calibracao e linear, comparar em litros equivale
//  a comparar no nivel - e permite converter o percentual para litros uma unica
//  vez (ver litersForPct()).
//
//  Histerese (recuperacao) de um lado so, como especificado:
//    entrada     -> volume <= limiar
//    recuperacao -> volume >= limiar + hist
// ============================================================================
namespace alerts {

// Percentual do MAIOR VOLUME MEDIVEL (volume::maxMeasurableLiters(), 116.651 L).
// 0 em qualquer campo = "nao configurado" (validate() recusa).
struct Config {
  uint8_t riskPct = 0;
  uint8_t criticalPct = 0;
  uint8_t histPct = 0;
};

// Estado do nivel. NORMAL = acima do risco; RISK = entre risco e critico;
// CRITICAL = no critico. Persistido para nao reenviar alertas a cada boot.
enum class Level : uint8_t {
  NORMAL = 0,
  RISK = 1,
  CRITICAL = 2,
};

enum class Event : uint8_t {
  NONE = 0,
  RISK_ENTERED = 1,
  CRITICAL_ENTERED = 2,
  CRITICAL_RECOVERED = 3,
  RISK_RECOVERED = 4,
};

enum class ConfigError : uint8_t {
  OK = 0,
  RISK_RANGE,      // riskPct fora de 1..100
  CRITICAL_RANGE,  // criticalPct fora de 1..100
  HIST_RANGE,      // histPct fora de 1..100
  ORDER,           // criticalPct >= riskPct
  HIST_OVERFLOW,   // riskPct + histPct > 100 (o risco nunca recuperaria)
};

// ---------------------------------------------------------------------------
//  Limiares em litros
// ---------------------------------------------------------------------------
// Referencia dos percentuais: o MAIOR volume medivel (100%). Fonte unica da
// calibracao em include/volume.h.
inline float maxReferenceLiters() { return volume::maxMeasurableLiters(); }

inline float litersForPct(uint8_t pct) {
  return maxReferenceLiters() * (static_cast<float>(pct) / 100.0f);
}

inline float enterRiskLiters(const Config &cfg) {
  return litersForPct(cfg.riskPct);
}
inline float enterCriticalLiters(const Config &cfg) {
  return litersForPct(cfg.criticalPct);
}
// A recuperacao e "hist pontos percentuais ACIMA do limiar": como a escala e
// percentual, basta somar os dois percentuais (validate() garante <= 100).
inline float recoverRiskLiters(const Config &cfg) {
  return litersForPct(static_cast<uint8_t>(cfg.riskPct + cfg.histPct));
}
inline float recoverCriticalLiters(const Config &cfg) {
  return litersForPct(static_cast<uint8_t>(cfg.criticalPct + cfg.histPct));
}

// ---------------------------------------------------------------------------
//  Validacao da configuracao
// ---------------------------------------------------------------------------
inline ConfigError validate(const Config &cfg) {
  if (cfg.riskPct < 1 || cfg.riskPct > 100) {
    return ConfigError::RISK_RANGE;
  }
  if (cfg.criticalPct < 1 || cfg.criticalPct > 100) {
    return ConfigError::CRITICAL_RANGE;
  }
  if (cfg.histPct < 1 || cfg.histPct > 100) {
    return ConfigError::HIST_RANGE;
  }
  if (cfg.criticalPct >= cfg.riskPct) {
    return ConfigError::ORDER;
  }
  if (static_cast<uint16_t>(cfg.riskPct) + cfg.histPct > 100) {
    return ConfigError::HIST_OVERFLOW;
  }
  return ConfigError::OK;
}

inline bool sameConfig(const Config &a, const Config &b) {
  return a.riskPct == b.riskPct && a.criticalPct == b.criticalPct &&
         a.histPct == b.histPct;
}

// ---------------------------------------------------------------------------
//  Maquina de estados
// ---------------------------------------------------------------------------
// Aplica UMA transicao e devolve o evento correspondente (NONE se nada mudou).
// A ordem de avaliacao e critico -> risco, o que cobre um salto que cruze as
// duas recuperacoes: a chamada seguinte completa a segunda transicao.
//
// Degrau direto de NORMAL para abaixo do critico emite APENAS o alerta critico
// (o de risco seria redundante: o nivel esta pior que o risco).
inline Event step(Level &level, const Config &cfg, float volumeL) {
  if (level == Level::CRITICAL) {
    if (volumeL >= recoverCriticalLiters(cfg)) {
      level = Level::RISK;
      return Event::CRITICAL_RECOVERED;
    }
    return Event::NONE;
  }

  if (level == Level::RISK) {
    if (volumeL >= recoverRiskLiters(cfg)) {
      level = Level::NORMAL;
      return Event::RISK_RECOVERED;
    }
    if (volumeL <= enterCriticalLiters(cfg)) {
      level = Level::CRITICAL;
      return Event::CRITICAL_ENTERED;
    }
    return Event::NONE;
  }

  // NORMAL
  if (volumeL <= enterCriticalLiters(cfg)) {
    level = Level::CRITICAL;
    return Event::CRITICAL_ENTERED;
  }
  if (volumeL <= enterRiskLiters(cfg)) {
    level = Level::RISK;
    return Event::RISK_ENTERED;
  }
  return Event::NONE;
}

// Estado que o volume corresponde AGORA, sem emitir evento: usado ao aplicar uma
// configuracao nova, para nao disparar alerta de uma transicao que nao existiu.
inline Level levelFor(const Config &cfg, float volumeL) {
  if (volumeL <= enterCriticalLiters(cfg)) {
    return Level::CRITICAL;
  }
  if (volumeL <= enterRiskLiters(cfg)) {
    return Level::RISK;
  }
  return Level::NORMAL;
}

// ---------------------------------------------------------------------------
//  Registro persistido - 32 B (dois slots A/B, como o historico)
//
//   0..3   magic "WCFG"
//   4      versao
//   5      riskPct
//   6      criticalPct
//   7      histPct
//   8      estado (Level)
//   9      pendentes (0..2)
//  10      pendente 0: evento
//  11..14  pendente 0: volume (float32)
//  15      pendente 1: evento
//  16..19  pendente 1: volume (float32)
//  20..23  geracao (escolhe o slot mais novo)
//  24..27  reservado
//  28..31  CRC32 dos 28 primeiros bytes
// ---------------------------------------------------------------------------
constexpr size_t RECORD_SIZE = 32;
constexpr uint32_t RECORD_MAGIC = 0x47464357UL;  // "WCFG"
constexpr uint8_t RECORD_VERSION = 1;
constexpr uint8_t MAX_PENDING = 2;

// Cabecalho 10 B (magic 4 + versao 1 + 3 percentuais + estado + contagem),
// 5 B por pendente (tipo 1 + volume 4) e cauda 12 B (geracao 4 + reservado 4 +
// CRC 4). Manter RECORD_SIZE coerente com MAX_PENDING e obrigatorio: o
// verificador de host le os dois valores do proprio header.
static_assert(RECORD_SIZE == 10 + MAX_PENDING * 5 + 12,
              "ajuste RECORD_SIZE ao mudar MAX_PENDING");

struct Persisted {
  Config config;
  Level level = Level::NORMAL;
  uint8_t pendingCount = 0;
  Event pendingEvent[MAX_PENDING] = {Event::NONE, Event::NONE};
  float pendingVolumeL[MAX_PENDING] = {0.0f, 0.0f};
  uint32_t generation = 0;
};

inline void putFloat(uint8_t *out, float value) {
  uint32_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  queue_record::put32(out, bits);
}

inline float getFloat(const uint8_t *in) {
  const uint32_t bits = queue_record::get32(in);
  float value = 0.0f;
  memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void encodeRecord(uint8_t out[RECORD_SIZE], const Persisted &p,
                         uint32_t generation) {
  static_assert(sizeof(float) == 4, "o registro exige IEEE-754 float32");
  memset(out, 0, RECORD_SIZE);
  queue_record::put32(out, RECORD_MAGIC);
  out[4] = RECORD_VERSION;
  out[5] = p.config.riskPct;
  out[6] = p.config.criticalPct;
  out[7] = p.config.histPct;
  out[8] = static_cast<uint8_t>(p.level);
  out[9] = p.pendingCount;
  out[10] = static_cast<uint8_t>(p.pendingEvent[0]);
  putFloat(out + 11, p.pendingVolumeL[0]);
  out[15] = static_cast<uint8_t>(p.pendingEvent[1]);
  putFloat(out + 16, p.pendingVolumeL[1]);
  queue_record::put32(out + 20, generation);
  queue_record::put32(out + 28, queue_record::crc32(out, 28));
}

inline uint32_t recordGeneration(const uint8_t in[RECORD_SIZE]) {
  return queue_record::get32(in + 20);
}

// Aceita apenas o que e internamente coerente: magic, versao, CRC, configuracao
// valida, estado e eventos conhecidos e pendentes dentro do limite.
inline bool decodeRecord(const uint8_t in[RECORD_SIZE], Persisted &p) {
  if (queue_record::get32(in) != RECORD_MAGIC || in[4] != RECORD_VERSION) {
    return false;
  }
  if (queue_record::get32(in + 28) != queue_record::crc32(in, 28)) {
    return false;
  }

  Persisted parsed;
  parsed.config.riskPct = in[5];
  parsed.config.criticalPct = in[6];
  parsed.config.histPct = in[7];
  if (validate(parsed.config) != ConfigError::OK) {
    return false;
  }
  if (in[8] > static_cast<uint8_t>(Level::CRITICAL)) {
    return false;
  }
  parsed.level = static_cast<Level>(in[8]);

  if (in[9] > MAX_PENDING) {
    return false;
  }
  parsed.pendingCount = in[9];
  parsed.pendingEvent[0] = static_cast<Event>(in[10]);
  parsed.pendingVolumeL[0] = getFloat(in + 11);
  parsed.pendingEvent[1] = static_cast<Event>(in[15]);
  parsed.pendingVolumeL[1] = getFloat(in + 16);
  for (uint8_t i = 0; i < MAX_PENDING; ++i) {
    if (parsed.pendingEvent[i] > Event::RISK_RECOVERED) {
      return false;
    }
  }

  parsed.generation = queue_record::get32(in + 20);
  p = parsed;
  return true;
}

// ---------------------------------------------------------------------------
//  Fila de pendentes (RAM e flash)
// ---------------------------------------------------------------------------
inline bool pushPending(Persisted &p, Event event, float volumeL) {
  if (event == Event::NONE || p.pendingCount >= MAX_PENDING) {
    return false;
  }
  p.pendingEvent[p.pendingCount] = event;
  p.pendingVolumeL[p.pendingCount] = volumeL;
  ++p.pendingCount;
  return true;
}

// Remove o mais antigo (chamado depois de o envio ser confirmado).
inline void popPending(Persisted &p) {
  if (p.pendingCount == 0) {
    return;
  }
  for (uint8_t i = 1; i < p.pendingCount; ++i) {
    p.pendingEvent[i - 1] = p.pendingEvent[i];
    p.pendingVolumeL[i - 1] = p.pendingVolumeL[i];
  }
  --p.pendingCount;
  p.pendingEvent[p.pendingCount] = Event::NONE;
  p.pendingVolumeL[p.pendingCount] = 0.0f;
}

}  // namespace alerts