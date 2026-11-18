#include "offline_queue.h"

#include <FS.h>
#include <LittleFS.h>

#include <algorithm>

// ============================================================================
//  Fila offline - implementacao
//
//  Estrutura portada do Ecowitt2TB: constantes, ordem das verificacoes e
//  mensagens de diagnostico seguem o projeto de referencia; muda apenas o
//  conteudo do registro (timestamp + volume).
// ============================================================================
namespace {

// Limites herdados do projeto de referencia.
const size_t MAX_SEGMENTS = 128;
const size_t FILESYSTEM_SAFETY_MARGIN = 16 * 1024;
const uint32_t SEGMENT_SEQUENCE_RESERVATION = 16;
const uint32_t RECORD_SEQUENCE_RESERVATION = 64;
const uint32_t SEQUENCE_META_MAGIC = 0x4D515451UL;
const size_t SEQUENCE_META_SIZE = 20;
const char *const SEQUENCE_META_FILES[2] = {"/q-seq-a.dat", "/q-seq-b.dat"};
const uint32_t SEGMENT_SEQUENCE_LIMIT = 99999999UL;

bool writeExact(File &file, const uint8_t *data, size_t size) {
  return file.write(data, size) == size;
}

bool readExact(File &file, uint8_t *data, size_t size) {
  return file.read(data, size) == static_cast<int>(size);
}

// "/q-00000001.seg" -> sequence. Mesmo nome do projeto de referencia, para
// reaproveitar sem alteracao a regra de ordenacao/recuperacao.
bool parseSegmentName(const char *name, uint32_t &sequence) {
  if (name == nullptr || strlen(name) != 14 || name[0] != 'q' ||
      name[1] != '-') {
    return false;
  }
  for (uint8_t i = 2; i < 10; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return false;
    }
  }
  if (strcmp(name + 10, ".seg") != 0) {
    return false;
  }
  sequence = static_cast<uint32_t>(strtoul(name + 2, nullptr, 10));
  return sequence != 0;
}

uint32_t maxU32(uint32_t a, uint32_t b) { return a > b ? a : b; }

}  // namespace

bool OfflineQueue::begin() {
  // Nunca formata automaticamente: a falha de montagem pode ser um sistema de
  // arquivos corrompido com registros recuperaveis (mesma escolha do
  // Ecowitt2TB).
  LittleFSConfig config(false);
  LittleFS.setConfig(config);

  if (!LittleFS.begin()) {
    m_mountFailed = true;
    Serial.println("Fila: LittleFS nao montado (formatacao automatica desativada).");
    Serial.println(
        "Fila: envie o sistema de arquivos uma vez (pio run -t uploadfs). "
        "A fila fica desativada e a telemetria online continua.");
    return false;
  }

  return recover();
}

bool OfflineQueue::getFilesystemSpace(size_t &total, size_t &used) const {
  FSInfo info;
  if (!LittleFS.info(info)) {
    return false;
  }
  total = info.totalBytes;
  used = info.usedBytes;
  return true;
}

bool OfflineQueue::readSequenceMeta(int slot, uint32_t &generation,
                                    uint32_t &segmentLimit,
                                    uint32_t &recordLimit) const {
  File file = LittleFS.open(SEQUENCE_META_FILES[slot], "r");
  uint8_t data[SEQUENCE_META_SIZE];
  const bool valid =
      file && file.size() == sizeof(data) && readExact(file, data, sizeof(data));
  if (file) {
    file.close();
  }
  if (!valid || queue_record::get32(data) != SEQUENCE_META_MAGIC ||
      queue_record::get32(data + 16) != queue_record::crc32(data, 16)) {
    return false;
  }
  generation = queue_record::get32(data + 4);
  segmentLimit = queue_record::get32(data + 8);
  recordLimit = queue_record::get32(data + 12);
  return segmentLimit != 0 && recordLimit != 0;
}

bool OfflineQueue::writeSequenceMeta(uint32_t segmentLimit,
                                     uint32_t recordLimit) {
  // Dois arquivos alternados com geracao: uma queda de energia no meio da
  // escrita invalida apenas o slot novo, e o slot anterior continua valido.
  const int target = m_sequenceMetaSlot == 0 ? 1 : 0;
  const uint32_t generation = m_sequenceMetaGeneration + 1;
  uint8_t data[SEQUENCE_META_SIZE] = {};
  queue_record::put32(data, SEQUENCE_META_MAGIC);
  queue_record::put32(data + 4, generation);
  queue_record::put32(data + 8, segmentLimit);
  queue_record::put32(data + 12, recordLimit);
  queue_record::put32(data + 16, queue_record::crc32(data, 16));

  File file = LittleFS.open(SEQUENCE_META_FILES[target], "w");
  const bool written = file && writeExact(file, data, sizeof(data));
  if (file) {
    file.flush();
    file.close();
  }

  uint32_t checkGeneration = 0;
  uint32_t checkSegments = 0;
  uint32_t checkRecords = 0;
  const bool verified =
      written &&
      readSequenceMeta(target, checkGeneration, checkSegments, checkRecords) &&
      checkGeneration == generation && checkSegments == segmentLimit &&
      checkRecords == recordLimit;
  if (!verified) {
    return false;
  }

  m_sequenceMetaSlot = target;
  m_sequenceMetaGeneration = generation;
  m_reservedSegmentLimit = segmentLimit;
  m_reservedRecordLimit = recordLimit;
  return true;
}

bool OfflineQueue::reserveSequenceRanges(uint32_t segmentFloor,
                                         uint32_t recordFloor) {
  const uint32_t segmentBase =
      maxU32(m_reservedSegmentLimit, maxU32(segmentFloor, m_nextSegmentSequence));
  const uint32_t recordBase =
      maxU32(m_reservedRecordLimit, maxU32(recordFloor, m_nextRecordSequence));

  if (segmentBase > SEGMENT_SEQUENCE_LIMIT - SEGMENT_SEQUENCE_RESERVATION ||
      recordBase > UINT32_MAX - RECORD_SEQUENCE_RESERVATION - 1) {
    Serial.println("Fila: espaco de sequencias esgotado; fila desativada.");
    return false;
  }

  return writeSequenceMeta(segmentBase + SEGMENT_SEQUENCE_RESERVATION,
                           recordBase + RECORD_SEQUENCE_RESERVATION);
}

bool OfflineQueue::initializeSequenceMeta(uint32_t segmentFloor,
                                          uint32_t recordFloor,
                                          uint32_t &segmentResume,
                                          uint32_t &recordResume) {
  const bool exists[2] = {LittleFS.exists(SEQUENCE_META_FILES[0]),
                          LittleFS.exists(SEQUENCE_META_FILES[1])};
  uint32_t generation[2] = {};
  uint32_t segmentLimit[2] = {};
  uint32_t recordLimit[2] = {};
  bool valid[2] = {readSequenceMeta(0, generation[0], segmentLimit[0],
                                    recordLimit[0]),
                   readSequenceMeta(1, generation[1], segmentLimit[1],
                                    recordLimit[1])};

  m_sequenceMetaSlot = -1;
  const bool hadValidMeta = valid[0] || valid[1];

  if (!hadValidMeta && (exists[0] || exists[1])) {
    Serial.println("Fila: metadados de sequencia invalidos; fila em quarentena.");
    return false;
  }

  if (hadValidMeta) {
    const bool useSecond =
        valid[1] && (!valid[0] ||
                     static_cast<int32_t>(generation[1] - generation[0]) > 0);
    m_sequenceMetaSlot = useSecond ? 1 : 0;
    m_sequenceMetaGeneration = generation[m_sequenceMetaSlot];
    m_reservedSegmentLimit = segmentLimit[m_sequenceMetaSlot];
    m_reservedRecordLimit = recordLimit[m_sequenceMetaSlot];
    segmentResume = m_reservedSegmentLimit;
    recordResume = m_reservedRecordLimit;
  } else {
    m_sequenceMetaGeneration = 0;
    m_reservedSegmentLimit = segmentFloor;
    m_reservedRecordLimit = recordFloor;
    segmentResume = segmentFloor;
    recordResume = recordFloor;
  }

  if (!reserveSequenceRanges(segmentFloor, recordFloor)) {
    return false;
  }

  if (hadValidMeta) {
    segmentResume =
        maxU32(segmentResume, m_reservedSegmentLimit - SEGMENT_SEQUENCE_RESERVATION);
    recordResume =
        maxU32(recordResume, m_reservedRecordLimit - RECORD_SEQUENCE_RESERVATION);
  }
  return true;
}

bool OfflineQueue::createSegment(uint32_t sequence, Segment &segment) {
  size_t total = 0;
  size_t used = 0;
  if (!getFilesystemSpace(total, used) || total <= used ||
      total - used <=
          FILESYSTEM_SAFETY_MARGIN + queue_record::SEGMENT_MAX_SIZE) {
    Serial.println("Fila: LittleFS sem espaco seguro para novo segmento.");
    return false;
  }

  Segment created = {};
  created.sequence = sequence;
  created.firstRecordSequence = m_nextRecordSequence;
  created.count = 0;
  created.closed = false;
  created.valid = true;
  created.recoverableTail = false;
  snprintf(created.path, sizeof(created.path), "/q-%08lu.seg",
           static_cast<unsigned long>(sequence));

  if (LittleFS.exists(created.path)) {
    Serial.println("Fila: nome de segmento ja existe; recusa sobrescrever.");
    return false;
  }

  File file = LittleFS.open(created.path, "w");
  if (!file) {
    return false;
  }
  uint8_t header[queue_record::SEGMENT_HEADER_SIZE];
  queue_record::encodeSegmentHeader(header, sequence);
  const bool written = writeExact(file, header, sizeof(header));
  file.flush();
  file.close();

  File check = LittleFS.open(created.path, "r");
  const bool headerValid = check && check.size() == sizeof(header);
  if (check) {
    check.close();
  }
  if (!written || !headerValid) {
    Serial.println("Fila: falha ao validar header do segmento novo.");
    return false;
  }

  segment = created;
  return true;
}

bool OfflineQueue::closeSegment(Segment &segment) {
  if (segment.closed) {
    return true;
  }

  const size_t recordsEnd =
      queue_record::SEGMENT_HEADER_SIZE +
      static_cast<size_t>(segment.count) * queue_record::RECORD_SIZE;

  // Uma escrita interrompida de footer pode ter deixado bytes extras: volta ao
  // fim canonico dos registros antes de tentar novamente.
  File repair = LittleFS.open(segment.path, "r+");
  const bool repaired = repair && repair.truncate(recordsEnd);
  if (repair) {
    repair.flush();
    repair.close();
  }
  if (!repaired) {
    return false;
  }

  File file = LittleFS.open(segment.path, "a");
  if (!file) {
    return false;
  }
  uint8_t footer[queue_record::SEGMENT_FOOTER_SIZE];
  queue_record::encodeSegmentFooter(footer, segment.count);
  const bool written = writeExact(file, footer, sizeof(footer));
  file.flush();
  file.close();
  if (!written) {
    return false;
  }

  File check = LittleFS.open(segment.path, "r");
  bool footerValid = false;
  if (check &&
      check.size() == recordsEnd + queue_record::SEGMENT_FOOTER_SIZE &&
      check.seek(static_cast<uint32_t>(
          check.size() - queue_record::SEGMENT_FOOTER_SIZE))) {
    uint8_t stored[queue_record::SEGMENT_FOOTER_SIZE];
    footerValid = readExact(check, stored, sizeof(stored)) &&
                  queue_record::decodeSegmentFooter(stored, segment.count);
  }
  if (check) {
    check.close();
  }
  if (!footerValid) {
    return false;
  }

  segment.closed = true;
  return true;
}

OfflineQueue::Segment *OfflineQueue::activeSegment() {
  for (auto &segment : m_segments) {
    if (segment.valid && !segment.closed) {
      return &segment;
    }
  }
  return nullptr;
}

bool OfflineQueue::inspectSegment(Segment &segment, uint32_t &lastRecordSequence,
                                  bool &haveRecordSequence) {
  File file = LittleFS.open(segment.path, "r");
  if (!file) {
    return false;
  }

  uint8_t header[queue_record::SEGMENT_HEADER_SIZE];
  bool valid = readExact(file, header, sizeof(header)) &&
               queue_record::decodeSegmentHeader(header, segment.sequence);

  uint32_t expected = 0;
  uint16_t count = 0;
  while (valid && count < queue_record::MAX_RECORDS_PER_SEGMENT &&
         (file.size() - file.position()) >= queue_record::RECORD_SIZE) {
    uint8_t record[queue_record::RECORD_SIZE];
    queue_record::Record decoded;
    if (!readExact(file, record, sizeof(record))) {
      valid = false;
      break;
    }

    const uint32_t storedSequence = queue_record::recordSequence(record);
    if (count == 0) {
      expected = storedSequence;
      segment.firstRecordSequence = expected;
      if (haveRecordSequence && expected <= lastRecordSequence) {
        valid = false;
        break;
      }
    } else if (storedSequence != expected) {
      valid = false;
      break;
    }
    if (!queue_record::decodeRecord(record, expected, decoded)) {
      valid = false;
      break;
    }

    ++count;
    lastRecordSequence = expected;
    haveRecordSequence = true;
    ++expected;
  }

  const size_t tail = file.size() - file.position();
  bool closed = false;
  if (valid && tail == queue_record::SEGMENT_FOOTER_SIZE) {
    uint8_t footer[queue_record::SEGMENT_FOOTER_SIZE];
    closed = readExact(file, footer, sizeof(footer)) &&
             queue_record::decodeSegmentFooter(footer, count);
    valid = closed;
  } else if (count == queue_record::MAX_RECORDS_PER_SEGMENT &&
             tail < queue_record::SEGMENT_FOOTER_SIZE) {
    // Inclui ausencia de footer e footer truncado por queda de energia.
    segment.recoverableTail = true;
  } else if (tail != 0 && count < queue_record::MAX_RECORDS_PER_SEGMENT) {
    segment.recoverableTail = true;
  } else if (tail != 0 || count == queue_record::MAX_RECORDS_PER_SEGMENT) {
    valid = false;
  }

  file.close();
  segment.count = count;
  segment.closed = closed;
  segment.valid = valid;
  return valid;
}

bool OfflineQueue::truncateRecoverableTail() {
  if (m_segments.empty() || !m_segments.back().recoverableTail) {
    return true;
  }

  const size_t validEnd =
      queue_record::SEGMENT_HEADER_SIZE +
      static_cast<size_t>(m_segments.back().count) * queue_record::RECORD_SIZE;

  // O LittleFS do ESP8266 implementa truncate em arquivo aberto para escrita.
  File active = LittleFS.open(m_segments.back().path, "r+");
  const bool truncated = active && active.truncate(validEnd);
  if (active) {
    active.flush();
    active.close();
  }
  if (!truncated) {
    Serial.println(
        "Fila: nao foi possivel remover a cauda parcial; fila preservada.");
    return false;
  }

  File verify = LittleFS.open(m_segments.back().path, "r");
  const bool verified = verify && verify.size() == validEnd;
  if (verify) {
    verify.close();
  }
  if (!verified) {
    Serial.println("Fila: tamanho apos truncate invalido; fila em quarentena.");
    return false;
  }

  m_segments.back().recoverableTail = false;
  return true;
}

bool OfflineQueue::recover() {
  m_segments.clear();
  m_highestSegmentSequence = 0;

  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    uint32_t sequence = 0;
    if (!parseSegmentName(dir.fileName().c_str(), sequence)) {
      continue;
    }
    if (sequence > m_highestSegmentSequence) {
      m_highestSegmentSequence = sequence;
    }
    if (m_segments.size() >= MAX_SEGMENTS) {
      Serial.println("Fila: limite de segmentos excedido; recuperacao interrompida.");
      return false;
    }

    Segment segment = {};
    segment.sequence = sequence;
    segment.count = 0;
    segment.closed = false;
    segment.valid = false;
    segment.recoverableTail = false;
    snprintf(segment.path, sizeof(segment.path), "/%s", dir.fileName().c_str());
    m_segments.push_back(segment);
  }

  std::sort(m_segments.begin(), m_segments.end(),
            [](const Segment &a, const Segment &b) {
              return a.sequence < b.sequence;
            });

  if (m_highestSegmentSequence >= SEGMENT_SEQUENCE_LIMIT) {
    Serial.println("Fila: sequencias de segmento esgotadas; fila preservada.");
    return false;
  }
  m_nextSegmentSequence = m_highestSegmentSequence + 1;

  uint32_t lastRecord = 0;
  bool haveRecord = false;
  for (size_t i = 0; i < m_segments.size(); ++i) {
    if (i && m_segments[i - 1].sequence == m_segments[i].sequence) {
      Serial.println("Fila: sequencia de segmento duplicada; fila em quarentena.");
      return false;
    }
    if (!inspectSegment(m_segments[i], lastRecord, haveRecord)) {
      Serial.print("Fila: segmento invalido preservado: ");
      Serial.println(m_segments[i].path);
      return false;
    }
    if (m_segments[i].recoverableTail && i + 1 != m_segments.size()) {
      Serial.print("Fila: cauda incompleta fora do segmento mais recente: ");
      Serial.println(m_segments[i].path);
      return false;
    }
    if (!m_segments[i].closed && i + 1 != m_segments.size()) {
      Serial.print("Fila: segmento aberto nao e o mais recente: ");
      Serial.println(m_segments[i].path);
      return false;
    }
  }

  // O primeiro registro restante pode seguir segmentos ja removidos pelo replay.
  if (haveRecord && lastRecord == UINT32_MAX) {
    Serial.println("Fila: sequencias de registro esgotadas; fila preservada.");
    return false;
  }
  m_nextRecordSequence = lastRecord + 1;

  if (!truncateRecoverableTail()) {
    return false;
  }

  uint32_t segmentResume = 0;
  uint32_t recordResume = 0;
  if (!initializeSequenceMeta(m_nextSegmentSequence, m_nextRecordSequence,
                              segmentResume, recordResume)) {
    return false;
  }
  if (segmentResume > SEGMENT_SEQUENCE_LIMIT || recordResume == UINT32_MAX) {
    return false;
  }

  if (!m_segments.empty() && !m_segments.back().closed) {
    if (!closeSegment(m_segments.back())) {
      return false;
    }
  }

  m_nextSegmentSequence = maxU32(m_nextSegmentSequence, segmentResume);
  m_nextRecordSequence = maxU32(m_nextRecordSequence, recordResume);
  m_replaySegment = 0;
  m_replayRecord = 0;
  m_ready = true;

  size_t total = 0;
  size_t used = 0;
  if (getFilesystemSpace(total, used)) {
    Serial.printf("Fila: LittleFS total=%u usado=%u livre=%u\n",
                  static_cast<unsigned>(total), static_cast<unsigned>(used),
                  static_cast<unsigned>(total - used));
  }
  return true;
}

bool OfflineQueue::append(uint64_t timestampMs, float volumeL) {
  if (!m_ready) {
    ++m_rejected;
    return false;
  }

  // Antes de esgotar a faixa reservada, grava uma nova faixa nos metadados A/B.
  // Sem isso, um reboot poderia repetir sequencias e o replay perderia a ordem.
  if (m_nextRecordSequence >= m_reservedRecordLimit ||
      m_nextSegmentSequence >= m_reservedSegmentLimit) {
    Segment *current = activeSegment();
    if (current && m_nextRecordSequence >= m_reservedRecordLimit &&
        current->count && !closeSegment(*current)) {
      ++m_rejected;
      return false;
    }
    if (!reserveSequenceRanges(m_nextSegmentSequence, m_nextRecordSequence)) {
      ++m_rejected;
      return false;
    }
  }

  if (m_nextSegmentSequence > SEGMENT_SEQUENCE_LIMIT) {
    Serial.println("Fila: espaco de sequencias de segmento esgotado.");
    ++m_rejected;
    return false;
  }
  if (m_nextRecordSequence == UINT32_MAX) {
    Serial.println("Fila: espaco de sequencias de registro esgotado.");
    ++m_rejected;
    return false;
  }

  Segment *active = activeSegment();

  if (active && active->count >= queue_record::MAX_RECORDS_PER_SEGMENT) {
    if (!closeSegment(*active)) {
      if (active->closed) {
        active->recoverableTail = false;
      }
      ++m_rejected;
      return false;
    }
    active = nullptr;
  }

  if (!active && !m_segments.empty()) {
    const Segment &last = m_segments.back();
    if (!last.closed) {
      ++m_rejected;
      return false;
    }
    const uint32_t expectedFirst = last.firstRecordSequence + last.count;
    if (m_nextRecordSequence < expectedFirst) {
      ++m_rejected;
      return false;
    }
  }

  if (!active) {
    Segment created = {};
    if (!createSegment(m_nextSegmentSequence, created)) {
      ++m_rejected;
      return false;
    }
    ++m_nextSegmentSequence;
    m_segments.push_back(created);
    active = &m_segments.back();
  }

  size_t total = 0;
  size_t used = 0;
  if (!getFilesystemSpace(total, used) || total <= used ||
      total - used <=
          FILESYSTEM_SAFETY_MARGIN +
              (active->count == 0
                   ? queue_record::SEGMENT_MAX_SIZE
                   : queue_record::RECORD_SIZE +
                         (active->count + 1 ==
                                  queue_record::MAX_RECORDS_PER_SEGMENT
                              ? queue_record::SEGMENT_FOOTER_SIZE
                              : 0))) {
    Serial.println("Fila: margem de seguranca impede enfileirar amostra.");
    ++m_rejected;
    return false;
  }

  File file = LittleFS.open(active->path, "a");
  const size_t expectedBefore =
      queue_record::SEGMENT_HEADER_SIZE +
      static_cast<size_t>(active->count) * queue_record::RECORD_SIZE;
  if (!file || file.size() != expectedBefore) {
    if (file) {
      file.close();
    }
    Serial.println("Fila: segmento ativo com tamanho inesperado; append cancelado.");
    ++m_rejected;
    return false;
  }

  uint8_t record[queue_record::RECORD_SIZE];
  queue_record::encodeRecord(record, m_nextRecordSequence, timestampMs, volumeL);
  const bool written = writeExact(file, record, sizeof(record));
  file.flush();
  file.close();

  File verify = LittleFS.open(active->path, "r");
  const size_t expectedAfter = expectedBefore + queue_record::RECORD_SIZE;
  const bool verified = verify && verify.size() == expectedAfter;
  if (verify) {
    verify.close();
  }
  if (!written || !verified) {
    Serial.println("Fila: falha ao gravar registro binario completo.");
    ++m_rejected;
    return false;
  }

  ++active->count;
  ++m_nextRecordSequence;
  ++m_stored;

  if (active->count == queue_record::MAX_RECORDS_PER_SEGMENT &&
      !closeSegment(*active)) {
    if (active->closed) {
      active->recoverableTail = false;
    }
    Serial.println("Fila: falha ao fechar segmento cheio; dados preservados.");
    ++m_rejected;
    return false;
  }

  return true;
}

OfflineQueue::PeekResult OfflineQueue::peek(queue_record::Record &record) {
  if (!m_ready) {
    return PeekResult::ERROR;
  }

  while (m_replaySegment < static_cast<int>(m_segments.size())) {
    Segment &segment = m_segments[static_cast<size_t>(m_replaySegment)];
    if (!segment.valid) {
      return PeekResult::ERROR;
    }

    if (m_replayRecord >= segment.count) {
      if (segment.closed) {
        // Segmento totalmente publicado: so agora o arquivo e removido.
        if (!removeCompletedSegment(segment)) {
          return PeekResult::ERROR;
        }
        m_segments.erase(m_segments.begin() + m_replaySegment);
        m_replayRecord = 0;
        continue;
      }
      if (!closeSegment(segment)) {
        return PeekResult::ERROR;
      }
      segment.recoverableTail = false;
      continue;
    }

    if (!readQueuedRecord(segment, m_replayRecord, record)) {
      Serial.println("Fila: falha de integridade no replay; segmento preservado.");
      return PeekResult::ERROR;
    }
    return PeekResult::RECORD;
  }

  return PeekResult::EMPTY;
}

bool OfflineQueue::confirm() {
  if (!m_ready || m_replaySegment >= static_cast<int>(m_segments.size())) {
    return false;
  }
  Segment &segment = m_segments[static_cast<size_t>(m_replaySegment)];
  if (m_replayRecord >= segment.count) {
    return false;
  }
  ++m_replayRecord;
  return true;
}

bool OfflineQueue::hasPending() const { return pendingRecords() != 0; }

uint32_t OfflineQueue::pendingRecords() const {
  uint32_t pending = 0;
  for (size_t i = 0; i < m_segments.size(); ++i) {
    if (static_cast<int>(i) < m_replaySegment) {
      continue;
    }
    uint32_t consumed = 0;
    if (static_cast<int>(i) == m_replaySegment) {
      consumed = m_replayRecord;
    }
    if (m_segments[i].count > consumed) {
      pending += static_cast<uint32_t>(m_segments[i].count - consumed);
    }
  }
  return pending;
}

bool OfflineQueue::readQueuedRecord(const Segment &segment, uint16_t index,
                                    queue_record::Record &record) const {
  File file = LittleFS.open(segment.path, "r");
  if (!file ||
      !file.seek(static_cast<uint32_t>(
          queue_record::SEGMENT_HEADER_SIZE +
          static_cast<size_t>(index) * queue_record::RECORD_SIZE))) {
    if (file) {
      file.close();
    }
    return false;
  }

  uint8_t bytes[queue_record::RECORD_SIZE];
  const bool ok =
      readExact(file, bytes, sizeof(bytes)) &&
      queue_record::decodeRecord(bytes, queue_record::recordSequence(bytes),
                                 record) &&
      record.sequence == segment.firstRecordSequence + index;
  file.close();
  return ok;
}

bool OfflineQueue::removeCompletedSegment(const Segment &segment) {
  if (!LittleFS.remove(segment.path)) {
    Serial.print("Fila: falha ao remover segmento concluido: ");
    Serial.println(segment.path);
    return false;
  }
  return true;
}





