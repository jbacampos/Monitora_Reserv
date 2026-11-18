#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ============================================================================
//  Formato binario da fila offline (puro, sem Arduino)
//
//  Porte de Ecowitt2TB/include/telemetry_record.h, adaptado ao MoniRes:
//  cada registro guarda apenas o timestamp original da medicao (Unix epoch em
//  milissegundos) e o volume em litros. `nivel_mm` e `uptime_s` NAO entram na
//  fila.
//
//  Layout do arquivo de segmento:
//    [header 24 B][registro 25 B] x ate MAX_RECORDS_PER_SEGMENT[footer 12 B]
//
//  Tudo tem CRC32 proprio e a sequencia e continua dentro do segmento, o que
//  permite detectar corrupcao/cauda parcial sem descartar dados bons.
// ============================================================================
namespace queue_record {

// ---------------------------------------------------------------------------
//  Registro
// ---------------------------------------------------------------------------
//  0..1   magic
//  2      versao
//  3..4   tamanho do registro
//  5..8   sequencia (global, continua entre segmentos)
//  9..16  timestamp Unix epoch em ms
//  17..20 volume em litros (IEEE-754 float32)
//  21..24 CRC32 dos 21 bytes anteriores
constexpr uint16_t RECORD_SIZE = 25;
constexpr uint8_t RECORD_VERSION = 1;
constexpr uint16_t RECORD_MAGIC = 0x524D;  // "MR" em little-endian

// ---------------------------------------------------------------------------
//  Segmento
// ---------------------------------------------------------------------------
constexpr uint32_t SEGMENT_MAGIC = 0x534D5251UL;  // "QRMS"
constexpr uint32_t FOOTER_MAGIC = 0x464D5251UL;   // "QRMF"
constexpr uint8_t SEGMENT_VERSION = 1;
constexpr uint16_t SEGMENT_HEADER_SIZE = 24;
constexpr uint16_t SEGMENT_FOOTER_SIZE = 12;

// O registro do MoniRes e 25 B (contra 112 B do Ecowitt2TB). Com 128 registros
// o segmento inteiro continua abaixo de um bloco de 4 KiB do LittleFS, o que
// quadruplica a capacidade da fila sem gastar mais flash.
constexpr uint16_t MAX_RECORDS_PER_SEGMENT = 128;

constexpr size_t SEGMENT_MAX_SIZE =
    SEGMENT_HEADER_SIZE + static_cast<size_t>(MAX_RECORDS_PER_SEGMENT) * RECORD_SIZE +
    SEGMENT_FOOTER_SIZE;

static_assert(SEGMENT_MAX_SIZE <= 4096,
              "um segmento deve caber em um bloco de 4 KiB do LittleFS");

// ---------------------------------------------------------------------------
//  CRC32 (mesmo polinomio do Ecowitt2TB)
// ---------------------------------------------------------------------------
inline uint32_t crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return ~crc;
}

inline void put16(uint8_t *out, uint16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
}

inline void put32(uint8_t *out, uint32_t value) {
  for (uint8_t i = 0; i < 4; ++i) {
    out[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

inline void put64(uint8_t *out, uint64_t value) {
  for (uint8_t i = 0; i < 8; ++i) {
    out[i] = static_cast<uint8_t>(value >> (8 * i));
  }
}

inline uint16_t get16(const uint8_t *in) {
  return static_cast<uint16_t>(in[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(in[1]) << 8);
}

inline uint32_t get32(const uint8_t *in) {
  uint32_t value = 0;
  for (uint8_t i = 0; i < 4; ++i) {
    value |= static_cast<uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

inline uint64_t get64(const uint8_t *in) {
  uint64_t value = 0;
  for (uint8_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

// ---------------------------------------------------------------------------
//  Registro
// ---------------------------------------------------------------------------
struct Record {
  uint32_t sequence = 0;
  uint64_t timestampMs = 0;
  float volumeL = 0.0f;
};

inline void encodeRecord(uint8_t out[RECORD_SIZE], uint32_t sequence,
                         uint64_t timestampMs, float volumeL) {
  static_assert(sizeof(float) == 4, "o registro exige IEEE-754 float32");
  memset(out, 0, RECORD_SIZE);
  put16(out, RECORD_MAGIC);
  out[2] = RECORD_VERSION;
  put16(out + 3, RECORD_SIZE);
  put32(out + 5, sequence);
  put64(out + 9, timestampMs);
  uint32_t bits = 0;
  memcpy(&bits, &volumeL, sizeof(bits));
  put32(out + 17, bits);
  put32(out + 21, crc32(out, 21));
}

inline bool decodeRecord(const uint8_t in[RECORD_SIZE], uint32_t expectedSequence,
                         Record &record) {
  if (get16(in) != RECORD_MAGIC || in[2] != RECORD_VERSION ||
      get16(in + 3) != RECORD_SIZE || get32(in + 5) != expectedSequence ||
      get32(in + 21) != crc32(in, 21)) {
    return false;
  }

  uint32_t bits = get32(in + 17);
  record.sequence = expectedSequence;
  record.timestampMs = get64(in + 9);
  memcpy(&record.volumeL, &bits, sizeof(bits));
  return true;
}

inline uint32_t recordSequence(const uint8_t in[RECORD_SIZE]) {
  return get32(in + 5);
}

// ---------------------------------------------------------------------------
//  Header e footer do segmento
// ---------------------------------------------------------------------------
inline void encodeSegmentHeader(uint8_t out[SEGMENT_HEADER_SIZE],
                                uint32_t sequence) {
  memset(out, 0, SEGMENT_HEADER_SIZE);
  put32(out, SEGMENT_MAGIC);
  out[4] = SEGMENT_VERSION;
  out[5] = static_cast<uint8_t>(SEGMENT_HEADER_SIZE);
  put32(out + 8, sequence);
  put32(out + 20, crc32(out, 20));
}

inline bool decodeSegmentHeader(const uint8_t in[SEGMENT_HEADER_SIZE],
                                uint32_t expectedSequence) {
  return get32(in) == SEGMENT_MAGIC && in[4] == SEGMENT_VERSION &&
         in[5] == static_cast<uint8_t>(SEGMENT_HEADER_SIZE) &&
         get32(in + 8) == expectedSequence &&
         get32(in + 20) == crc32(in, 20);
}

inline void encodeSegmentFooter(uint8_t out[SEGMENT_FOOTER_SIZE],
                                uint16_t recordCount) {
  memset(out, 0, SEGMENT_FOOTER_SIZE);
  put32(out, FOOTER_MAGIC);
  put32(out + 4, recordCount);
  put32(out + 8, crc32(out, 8));
}

inline bool decodeSegmentFooter(const uint8_t in[SEGMENT_FOOTER_SIZE],
                                uint16_t expectedCount) {
  return get32(in) == FOOTER_MAGIC && get32(in + 4) == expectedCount &&
         get32(in + 8) == crc32(in, 8);
}

}  // namespace queue_record

