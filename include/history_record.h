#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "queue_record.h"

// ============================================================================
//  Formato binario do historico diario (puro, sem Arduino)
//
//  Reaproveita os helpers little-endian e o CRC32 de queue_record.h: os dois
//  formatos serializam campo a campo, sem nenhum struct gravado com sizeof e,
//  portanto, sem padding de compilador.
//
//  Tabela (/h-days-a.dat e /h-days-b.dat):
//    [header 28 B][registro 52 B] x 30[footer 12 B] = 1600 B
//
//  O anel e indexado por dayIndex % 30: a ordem FISICA dos slots e uma rotacao
//  da ordem cronologica, nunca crescente. Por isso a validacao cronologica e
//  feita percorrendo o anel a partir do menor dayIndex, e nao na ordem dos
//  indices dos slots.
//
//  Estados de um slot (inequivocos entre si):
//    VAZIO       - os 52 bytes sao exatamente 0x00 (nunca passa por CRC);
//    VALIDO      - versao, dayIndex != 0, dayIndex % 30 == slot e CRC32 ok;
//    CORROMPIDO  - qualquer outro conteudo (CRC errado, versao invalida,
//                  registro no slot errado, escrita parcial).
// ============================================================================
namespace history_record {

// Serializacao e CRC compartilhados com a fila offline (queue_record.h).
using queue_record::crc32;
using queue_record::get16;
using queue_record::get32;
using queue_record::get64;
using queue_record::put16;
using queue_record::put32;
using queue_record::put64;

// ---------------------------------------------------------------------------
//  Registro diario - 52 B
//
//   0..3   geracao de quem gravou (informativa na tabela; no snapshot e o
//          criterio de escolha entre A e B)
//   4..7   dayIndex
//   8..11  volume minimo do dia (IEEE-754 float32)
//  12..15  timestamp do minimo (epoch s)
//  16..19  volume maximo do dia (float32)
//  20..23  timestamp do maximo (epoch s)
//  24..31  soma dos volumes (float64, evita perder precisao em 8640 amostras)
//  32..35  numero de amostras contabilizadas
//  36..39  timestamp da primeira amostra
//  40..43  timestamp da ultima amostra
//  44..45  lacunas (intervalos sem amostra)
//  46     flags (FLAG_*)
//  47     versao do registro
//  48..51  CRC32 dos 48 bytes anteriores
// ---------------------------------------------------------------------------
constexpr uint16_t RECORD_SIZE = 52;
constexpr uint8_t RECORD_VERSION = 1;

// ---------------------------------------------------------------------------
//  Cabecalho (28 B) e rodape (12 B)
//
//   0..3   magic
//   4      versao / 5 headerSize / 6..7 flags (HEADER_DEGRADED)
//   8..11  geracao
//  12..15  firstDay (menor dayIndex presente)
//  16..19  lastDay (maior dayIndex presente)
//  20..21  count (registros validos no momento da gravacao)
//  22..23  reservado
//  24..27  CRC32 dos 24 bytes anteriores
//
// `count` e o que o gravador viu; na leitura o modulo recalcula os registros
// validos e trata divergencia como apenas um aviso (o historico e auxiliar).
// ---------------------------------------------------------------------------
constexpr uint32_t TABLE_MAGIC = 0x31445448UL;   // "HTD1"
constexpr uint32_t FOOTER_MAGIC = 0x32445448UL;  // "HTD2"
constexpr uint8_t TABLE_VERSION = 1;
constexpr uint16_t HEADER_SIZE = 28;
constexpr uint16_t FOOTER_SIZE = 12;

constexpr uint16_t SLOTS = 30;  // dias fechados mantidos no anel

constexpr size_t TABLE_SIZE =
    HEADER_SIZE + static_cast<size_t>(SLOTS) * RECORD_SIZE + FOOTER_SIZE;
constexpr size_t SNAPSHOT_SIZE = RECORD_SIZE;

static_assert(TABLE_SIZE == 1600, "a tabela diaria deve ter 1600 bytes");
static_assert(RECORD_SIZE == 52, "o registro diario deve ter 52 bytes");
static_assert(HEADER_SIZE == 28, "o cabecalho deve ter 28 bytes");

// ---------------------------------------------------------------------------
//  Flags
// ---------------------------------------------------------------------------
constexpr uint8_t FLAG_RESTARTED = 0x01;  // dia acumulado em mais de uma execucao
constexpr uint8_t FLAG_LATE_START = 0x02;  // primeira amostra muito depois da meia-noite
constexpr uint8_t FLAG_EARLY_END = 0x04;   // ultima amostra muito antes da meia-noite

constexpr uint16_t HEADER_DEGRADED = 0x0001;  // houve dia perdido por falha de gravacao

// ---------------------------------------------------------------------------
//  Dia civil (horario de Brasilia, UTC-3)
//
//  O deslocamento local e NEGATIVO (UTC-3) e tem o mesmo valor de
//  UTC_OFFSET_SECONDS em config.h; history.cpp tem um static_assert ligando os
//  dois para que nao divirjam. A meia-noite local acontece quando
//  epoch % 86400 == 3*3600, por isso o deslocamento entra somado no epoch antes
//  da divisao e subtraido de volta em dayStart().
//
//  Exemplo: 2024-05-31 23:59:59 BRT = 2024-06-01 02:59:59 UTC -> dia anterior.
// ---------------------------------------------------------------------------
constexpr int32_t LOCAL_OFFSET_SECONDS = -3 * 3600;
constexpr uint32_t DAY_SECONDS = 86400UL;

// Amostras nominais de um dia completo (intervalo de 10 s).
constexpr uint32_t NOMINAL_SAMPLES_PER_DAY = DAY_SECONDS / 10UL;  // 8640

inline uint32_t dayIndex(uint32_t epochSeconds) {
  const int64_t local = static_cast<int64_t>(epochSeconds) + LOCAL_OFFSET_SECONDS;
  return static_cast<uint32_t>(local / static_cast<int64_t>(DAY_SECONDS));
}

inline uint32_t dayStart(uint32_t day) {
  const int64_t local =
      static_cast<int64_t>(day) * static_cast<int64_t>(DAY_SECONDS);
  return static_cast<uint32_t>(local - LOCAL_OFFSET_SECONDS);
}


// ---------------------------------------------------------------------------
//  Registro
// ---------------------------------------------------------------------------
struct Record {
  uint32_t gen = 0;
  uint32_t dayIndex = 0;
  float minL = 0.0f;
  uint32_t tsMin = 0;
  float maxL = 0.0f;
  uint32_t tsMax = 0;
  double sumL = 0.0;
  uint32_t n = 0;
  uint32_t firstTs = 0;
  uint32_t lastTs = 0;
  uint16_t gaps = 0;
  uint8_t flags = 0;
};

enum class SlotState : uint8_t {
  EMPTY = 0,
  VALID = 1,
  CORRUPT = 2,
};

inline bool isSlotEmpty(const uint8_t in[RECORD_SIZE]) {
  for (uint16_t i = 0; i < RECORD_SIZE; ++i) {
    if (in[i] != 0) {
      return false;
    }
  }
  return true;
}

inline void encodeRecord(uint8_t out[RECORD_SIZE], const Record &record) {
  static_assert(sizeof(float) == 4, "minimo/maximo exigem IEEE-754 float32");
  static_assert(sizeof(double) == 8, "a soma exige IEEE-754 float64");

  memset(out, 0, RECORD_SIZE);
  put32(out + 0, record.gen);
  put32(out + 4, record.dayIndex);

  uint32_t bits = 0;
  memcpy(&bits, &record.minL, sizeof(bits));
  put32(out + 8, bits);
  put32(out + 12, record.tsMin);
  memcpy(&bits, &record.maxL, sizeof(bits));
  put32(out + 16, bits);
  put32(out + 20, record.tsMax);

  uint64_t sumBits = 0;
  memcpy(&sumBits, &record.sumL, sizeof(sumBits));
  put64(out + 24, sumBits);

  put32(out + 32, record.n);
  put32(out + 36, record.firstTs);
  put32(out + 40, record.lastTs);
  put16(out + 44, record.gaps);
  out[46] = record.flags;
  out[47] = RECORD_VERSION;
  put32(out + 48, crc32(out, 48));
}

// Classifica o conteudo de um slot. VAZIO e CORROMPIDO nunca sao confundidos:
// so os 52 bytes zerados contam como vazio.
inline SlotState decodeRecord(const uint8_t in[RECORD_SIZE], Record &record) {
  if (isSlotEmpty(in)) {
    return SlotState::EMPTY;
  }
  if (in[47] != RECORD_VERSION || get32(in + 48) != crc32(in, 48)) {
    return SlotState::CORRUPT;
  }

  Record parsed;
  parsed.gen = get32(in + 0);
  parsed.dayIndex = get32(in + 4);
  if (parsed.dayIndex == 0) {
    return SlotState::CORRUPT;
  }

  uint32_t bits = get32(in + 8);
  memcpy(&parsed.minL, &bits, sizeof(bits));
  parsed.tsMin = get32(in + 12);
  bits = get32(in + 16);
  memcpy(&parsed.maxL, &bits, sizeof(bits));
  parsed.tsMax = get32(in + 20);

  const uint64_t sumBits = get64(in + 24);
  memcpy(&parsed.sumL, &sumBits, sizeof(sumBits));

  parsed.n = get32(in + 32);
  parsed.firstTs = get32(in + 36);
  parsed.lastTs = get32(in + 40);
  parsed.gaps = get16(in + 44);
  parsed.flags = in[46];

  record = parsed;
  return SlotState::VALID;
}

// Registro de um slot da tabela: valida tambem a posicao no anel.
inline SlotState decodeSlot(const uint8_t in[RECORD_SIZE], uint16_t slot,
                            Record &record) {
  const SlotState state = decodeRecord(in, record);
  if (state != SlotState::VALID) {
    return state;
  }
  if ((record.dayIndex % SLOTS) != slot) {
    return SlotState::CORRUPT;
  }
  return SlotState::VALID;
}

// ---------------------------------------------------------------------------
//  Cabecalho e rodape da tabela
// ---------------------------------------------------------------------------
struct Header {
  uint16_t flags = 0;
  uint32_t gen = 0;
  uint32_t firstDay = 0;
  uint32_t lastDay = 0;
  uint16_t count = 0;
};

inline void encodeHeader(uint8_t out[HEADER_SIZE], const Header &header) {
  memset(out, 0, HEADER_SIZE);
  put32(out + 0, TABLE_MAGIC);
  out[4] = TABLE_VERSION;
  out[5] = static_cast<uint8_t>(HEADER_SIZE);
  put16(out + 6, header.flags);
  put32(out + 8, header.gen);
  put32(out + 12, header.firstDay);
  put32(out + 16, header.lastDay);
  put16(out + 20, header.count);
  put16(out + 22, 0);  // reservado
  put32(out + 24, crc32(out, 24));
}

inline bool decodeHeader(const uint8_t in[HEADER_SIZE], Header &header) {
  if (get32(in + 0) != TABLE_MAGIC || in[4] != TABLE_VERSION ||
      in[5] != static_cast<uint8_t>(HEADER_SIZE) ||
      get32(in + 24) != crc32(in, 24)) {
    return false;
  }
  header.flags = get16(in + 6);
  header.gen = get32(in + 8);
  header.firstDay = get32(in + 12);
  header.lastDay = get32(in + 16);
  header.count = get16(in + 20);
  return true;
}

inline void encodeFooter(uint8_t out[FOOTER_SIZE], uint16_t count) {
  memset(out, 0, FOOTER_SIZE);
  put32(out + 0, FOOTER_MAGIC);
  put16(out + 4, count);
  put16(out + 6, TABLE_VERSION);
  put32(out + 8, crc32(out, 8));
}

inline bool decodeFooter(const uint8_t in[FOOTER_SIZE], uint16_t expectedCount) {
  return get32(in + 0) == FOOTER_MAGIC && get16(in + 4) == expectedCount &&
         get16(in + 6) == TABLE_VERSION && get32(in + 8) == crc32(in, 8);
}

}  // namespace history_record

