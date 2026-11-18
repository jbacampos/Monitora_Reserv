#pragma once

#include <Arduino.h>

#include <vector>

#include "queue_record.h"

// ============================================================================
//  Fila offline de telemetria (LittleFS)
//
//  Porte do mecanismo do Ecowitt2TB (include/telemetry_record.h e a fila em
//  src/main.cpp): segmentos imutaveis com header/footer e CRC32, sequencias
//  reservadas em metadados A/B, recuperacao no boot e replay em ordem
//  cronologica. Cada registro guarda timestamp original (epoch ms) e volume.
//
//  Garantias herdadas do projeto de referencia:
//    - nunca formata o LittleFS automaticamente (dados preservados);
//    - qualquer inconsistencia coloca a fila em quarentena em vez de apagar;
//    - segmento so e removido depois de todos os registros publicados;
//    - cheio = recusa enfileirar, sem descartar registros antigos.
// ============================================================================
class OfflineQueue {
 public:
  enum class PeekResult : uint8_t {
    RECORD = 0,  // registro valido devolvido
    EMPTY,       // nao ha registros pendentes
    ERROR,       // falha de integridade/IO: fila preservada, nao avance
  };

  // Monta o LittleFS (sem auto-formatacao) e recupera a fila.
  bool begin();

  bool ready() const { return m_ready; }
  bool mountFailed() const { return m_mountFailed; }

  // Enfileira uma medicao. Retorna false (sem descartar nada) se a fila nao
  // estiver pronta, se a memoria acabou ou se a gravacao nao foi verificada.
  bool append(uint64_t timestampMs, float volumeL);

  bool hasPending() const;
  uint32_t pendingRecords() const;
  uint32_t storedRecords() const { return m_stored; }
  uint32_t rejectedRecords() const { return m_rejected; }

  // Le o proximo registro pendente sem remove-lo. Deve ser seguido de
  // confirm() apenas quando a publicacao der certo.
  PeekResult peek(queue_record::Record &record);
  bool confirm();

 private:
  struct Segment {
    uint32_t sequence;
    uint32_t firstRecordSequence;
    char path[20];
    uint16_t count;
    bool closed;
    bool valid;
    bool recoverableTail;
  };

  bool recover();
  bool inspectSegment(Segment &segment, uint32_t &lastRecordSequence,
                      bool &haveRecordSequence);
  bool truncateRecoverableTail();
  bool createSegment(uint32_t sequence, Segment &segment);
  bool closeSegment(Segment &segment);
  bool readQueuedRecord(const Segment &segment, uint16_t index,
                        queue_record::Record &record) const;
  bool removeCompletedSegment(const Segment &segment);
  bool getFilesystemSpace(size_t &total, size_t &used) const;
  Segment *activeSegment();

  bool readSequenceMeta(int slot, uint32_t &generation, uint32_t &segmentLimit,
                        uint32_t &recordLimit) const;
  bool writeSequenceMeta(uint32_t segmentLimit, uint32_t recordLimit);
  bool reserveSequenceRanges(uint32_t segmentFloor, uint32_t recordFloor);
  bool initializeSequenceMeta(uint32_t segmentFloor, uint32_t recordFloor,
                              uint32_t &segmentResume, uint32_t &recordResume);

  std::vector<Segment> m_segments;
  uint32_t m_nextSegmentSequence = 1;
  uint32_t m_nextRecordSequence = 1;
  int m_replaySegment = 0;
  uint16_t m_replayRecord = 0;
  bool m_ready = false;
  bool m_mountFailed = false;
  uint32_t m_highestSegmentSequence = 0;
  uint32_t m_reservedSegmentLimit = 1;
  uint32_t m_reservedRecordLimit = 1;
  uint32_t m_sequenceMetaGeneration = 0;
  int m_sequenceMetaSlot = -1;
  uint32_t m_stored = 0;
  uint32_t m_rejected = 0;
};
