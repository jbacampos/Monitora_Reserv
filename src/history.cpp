#include "history.h"

#include <FS.h>
#include <LittleFS.h>

#include "config.h"

// ============================================================================
//  Historico diario - implementacao
//
//  Duas familias de arquivos, sempre em pares A/B:
//    /h-days-a.dat | /h-days-b.dat   tabela circular de 30 dias (1600 B)
//    /h-cur-a.dat  | /h-cur-b.dat    snapshot do dia corrente (52 B)
//
//  A gravacao sempre usa o arquivo INATIVO e so depois de verificar o conteudo
//  ele passa a ser o ativo. Se a gravacao for interrompida (queda de energia),
//  o arquivo ativo anterior continua valido e o boot escolhe o de maior geracao.
//  Nao existe rollback, fila de recuperacao nem transacao: se o dia nao pudo ser
//  gravado, ele fica em RAM e a perda eventual e aceita.
// ============================================================================
namespace {

const char *const TABLE_PATHS[2] = {"/h-days-a.dat", "/h-days-b.dat"};
const char *const SNAPSHOT_PATHS[2] = {"/h-cur-a.dat", "/h-cur-b.dat"};

constexpr uint32_t kIntervalS = HIST_SAMPLE_INTERVAL_MS / 1000UL;
constexpr uint16_t kNoSlot = 0xFFFF;

// O dia civil do historico e o mesmo de config.h (horario de Brasilia) e a
// amostragem nominal e a mesma do intervalo de contagem.
static_assert(history_record::LOCAL_OFFSET_SECONDS == UTC_OFFSET_SECONDS,
              "o deslocamento do dia civil divergiu de UTC_OFFSET_SECONDS");
static_assert(history_record::NOMINAL_SAMPLES_PER_DAY ==
                  history_record::DAY_SECONDS / kIntervalS,
              "NOMINAL_SAMPLES_PER_DAY deve corresponder ao intervalo de amostragem");
static_assert(HIST_GAP_SECONDS == 2 * kIntervalS,
              "HIST_GAP_SECONDS deve ser o dobro do intervalo de amostragem");
static_assert(HIST_SAMPLE_INTERVAL_MS % 1000UL == 0,
              "o intervalo de amostragem deve ser multiplo de 1 s");

bool readExact(File &file, uint8_t *data, size_t size) {
  return file.read(data, size) == static_cast<int>(size);
}

bool writeExact(File &file, const uint8_t *data, size_t size) {
  return file.write(data, size) == size;
}

// Resultado da varredura de uma tabela. `count` do cabecalho NAO e usado como
// verdade: os registros sao reclassificados aqui (vazio / valido / corrompido).
struct TableScan {
  history_record::Header header;
  uint16_t valid = 0;
  uint16_t empty = 0;
  uint16_t corrupt = 0;
  uint32_t firstDay = 0;
  uint32_t lastDay = 0;
  bool sizeOk = false;
  bool headerOk = false;
  bool footerOk = false;

  bool usable() const { return sizeOk && headerOk && footerOk; }
};

// Ordem cronologica: cada registro valido ja e verificado em decodeSlot
// (dayIndex % 30 == slot), o que garante a unicidade dentro do anel. Como a
// posicao fisica e uma rotacao da ordem cronologica, basta que todos os dias
// caibam na janela de 30 dias para que a sequencia logica seja crescente.
bool chronologicalWindow(const TableScan &scan) {
  return scan.valid < 2 || (scan.lastDay - scan.firstDay) < history_record::SLOTS;
}

void scanTable(uint8_t index, TableScan &scan, uint16_t skipSlot = kNoSlot) {
  scan = TableScan();

  File file = LittleFS.open(TABLE_PATHS[index], "r");
  if (!file) {
    return;
  }
  scan.sizeOk = file.size() == history_record::TABLE_SIZE;

  uint8_t header[history_record::HEADER_SIZE];
  if (scan.sizeOk && readExact(file, header, sizeof(header))) {
    scan.headerOk = history_record::decodeHeader(header, scan.header);
  }

  if (scan.sizeOk) {
    uint8_t buffer[history_record::RECORD_SIZE];
    for (uint16_t slot = 0; slot < history_record::SLOTS; ++slot) {
      if (!readExact(file, buffer, sizeof(buffer))) {
        scan.sizeOk = false;
        break;
      }
      history_record::Record record;
      const history_record::SlotState state =
          history_record::decodeSlot(buffer, slot, record);
      if (state == history_record::SlotState::EMPTY) {
        ++scan.empty;
        continue;
      }
      if (state == history_record::SlotState::CORRUPT) {
        ++scan.corrupt;
        continue;
      }
      if (slot == skipSlot) {
        continue;
      }
      ++scan.valid;
      if (scan.valid == 1 || record.dayIndex < scan.firstDay) {
        scan.firstDay = record.dayIndex;
      }
      if (scan.valid == 1 || record.dayIndex > scan.lastDay) {
        scan.lastDay = record.dayIndex;
      }
    }
  }

  if (scan.sizeOk) {
    uint8_t footer[history_record::FOOTER_SIZE];
    if (readExact(file, footer, sizeof(footer))) {
      scan.footerOk = history_record::decodeFooter(footer, scan.header.count);
    }
  }
  file.close();
}

}  // namespace

// ---------------------------------------------------------------------------
//  Inicializacao
//
//  O LittleFS ja foi montado pela fila offline. Se nao estiver montado, tenta
//  montar SEM formatacao automatica; falhando, o historico fica desativado.
// ---------------------------------------------------------------------------
void History::begin(uint32_t nowMs) {
  m_lastAttemptMs = nowMs - HIST_RETRY_INTERVAL_MS;  // permite tentar ja
  m_lastSnapMs = nowMs;

  FSInfo info = {};
  bool mounted = LittleFS.info(info);
  if (!mounted) {
    LittleFS.setConfig(LittleFSConfig(false));
    mounted = LittleFS.begin();
    if (mounted) {
      LittleFS.info(info);
    }
  }
  if (!mounted) {
    Serial.println("Historico: LittleFS indisponivel (sem formatacao automatica).");
    Serial.println("Historico: desativado; sensor, display, MQTT e fila seguem normais.");
    return;
  }

  const size_t blockSize = info.blockSize ? info.blockSize : 4096;
  m_minFreeBytes = static_cast<size_t>(HIST_MIN_FREE_BLOCKS) * blockSize;

  // Tabela valida de maior geracao.
  TableScan tables[2];
  scanTable(0, tables[0]);
  scanTable(1, tables[1]);

  uint8_t chosen = 0;
  bool found = false;
  for (uint8_t i = 0; i < 2; ++i) {
    if (!tables[i].usable()) {
      continue;
    }
    if (!found || tables[i].header.gen >= tables[chosen].header.gen) {
      chosen = i;
      found = true;
    }
  }

  if (found) {
    m_activeTable = chosen;
    m_haveTable = true;
    m_tableGen = tables[chosen].header.gen;
    if ((tables[chosen].header.flags & history_record::HEADER_DEGRADED) != 0) {
      m_degraded = true;
    }
    if (tables[chosen].header.count != tables[chosen].valid) {
      Serial.printf("Historico: cabecalho diz %u dias, leitura encontrou %u (aviso).\n",
                    static_cast<unsigned>(tables[chosen].header.count),
                    static_cast<unsigned>(tables[chosen].valid));
    }
    if (!chronologicalWindow(tables[chosen])) {
      Serial.println("Historico: registros fora da janela de 30 dias (aviso).");
    }
  }

  loadSnapshot();
  m_ready = true;

  Serial.printf("Historico: %u dias | vazios=%u | corrompidos=%u | gen=%lu | %s\n",
                static_cast<unsigned>(found ? tables[chosen].valid : 0),
                static_cast<unsigned>(found ? tables[chosen].empty
                                            : history_record::SLOTS),
                static_cast<unsigned>(found ? tables[chosen].corrupt : 0),
                static_cast<unsigned long>(m_tableGen),
                m_degraded ? "degradado" : "ok");
  Serial.printf("Historico: espaco minimo para gravar: %u B (%u blocos de %u B)\n",
                static_cast<unsigned>(m_minFreeBytes),
                static_cast<unsigned>(HIST_MIN_FREE_BLOCKS),
                static_cast<unsigned>(blockSize));

  if (m_haveSnapshot) {
    Serial.printf("Historico: snapshot recuperado do dia %lu (amostras=%lu)\n",
                  static_cast<unsigned long>(m_snap.dayIndex),
                  static_cast<unsigned long>(m_snap.n));
  }
}

bool History::loadSnapshot() {
  m_haveSnapshot = false;
  bool found = false;

  for (uint8_t i = 0; i < 2; ++i) {
    File file = LittleFS.open(SNAPSHOT_PATHS[i], "r");
    if (!file || file.size() != history_record::SNAPSHOT_SIZE) {
      if (file) {
        file.close();
      }
      continue;
    }

    uint8_t buffer[history_record::SNAPSHOT_SIZE];
    history_record::Record record;
    const bool ok =
        readExact(file, buffer, sizeof(buffer)) &&
        history_record::decodeRecord(buffer, record) ==
            history_record::SlotState::VALID;
    file.close();
    if (!ok) {
      continue;
    }
    if (!found || record.gen >= m_snap.gen) {
      m_snap = record;
      m_snapGen = record.gen;
      m_activeSnap = i;
      found = true;
    }
  }

  m_haveSnapshot = found && m_snap.n > 0;
  return m_haveSnapshot;
}

// Limiar simples de espaco livre. 8 blocos cobrem a escrita (bloco de dados +
// commit dos metadados do LittleFS) e ainda preservam a margem que a fila
// offline exige. Nao e uma garantia absoluta: e uma heuristica ajustavel.
bool History::spaceAvailable(size_t &freeBytes) {
  FSInfo info = {};
  if (!LittleFS.info(info)) {
    freeBytes = 0;
    return false;
  }
  freeBytes = info.totalBytes > info.usedBytes ? info.totalBytes - info.usedBytes : 0;
  if (freeBytes < m_minFreeBytes) {
    if (!m_spaceWarned) {
      m_spaceWarned = true;
      Serial.printf("Historico: %u B livres (minimo %u B); gravacao adiada.\n",
                    static_cast<unsigned>(freeBytes),
                    static_cast<unsigned>(m_minFreeBytes));
    }
    return false;
  }
  m_spaceWarned = false;
  return true;
}

// ---------------------------------------------------------------------------
//  Ciclo do dia
//
//  O dia de cada amostra vem do timestamp da MEDICAO (epoch NTP), nao do
//  instante em que o loop processou a leitura: um loop atrasado as 00:00:02
//  ainda contabiliza a amostra das 23:59:58 no dia anterior.
// ---------------------------------------------------------------------------
void History::update(uint32_t nowMs, bool clockSynced, uint64_t sampleTimestampMs,
                     float volumeL, bool sampleValid) {
  if (!m_ready || !clockSynced || !sampleValid || sampleTimestampMs == 0) {
    return;
  }

  const uint32_t epochS = static_cast<uint32_t>(sampleTimestampMs / 1000ULL);
  const uint32_t day = history_record::dayIndex(epochS);

  if (!m_dayStarted) {
    startFirstDay(nowMs, day, volumeL, sampleTimestampMs);
    maybeSnapshot(nowMs);
    return;
  }

  if (day > m_acc.dayIndex) {
    closeDay();
    startDay(nowMs, day, volumeL, sampleTimestampMs);
    if (m_pending) {
      writePending(nowMs);
    }
    maybeSnapshot(nowMs);
    return;
  }

  if (day < m_acc.dayIndex) {
    return;  // amostra fora de ordem: nao mexe em um dia ja fechado
  }

  // Nova tentativa de gravar o dia pendente, independente da amostragem.
  if (m_pending && (nowMs - m_lastAttemptMs) >= HIST_RETRY_INTERVAL_MS) {
    writePending(nowMs);
  }

  if ((nowMs - m_lastCountMs) < HIST_SAMPLE_INTERVAL_MS) {
    maybeSnapshot(nowMs);
    return;
  }

  accumulate(nowMs, volumeL, sampleTimestampMs);
  maybeSnapshot(nowMs);
}

// Primeiro dia apos o boot: retoma o snapshot do mesmo dia, fecha um snapshot
// de dia anterior ou simplesmente comeca um dia novo.
void History::startFirstDay(uint32_t nowMs, uint32_t day, float volumeL,
                            uint64_t sampleTimestampMs) {
  const bool hadSnapshot = m_haveSnapshot;
  m_haveSnapshot = false;

  if (hadSnapshot && m_snap.dayIndex == day) {
    const uint32_t nowS = static_cast<uint32_t>(sampleTimestampMs / 1000ULL);
    m_acc = m_snap;
    m_dayStarted = true;
    if (nowS > m_acc.lastTs + HIST_GAP_SECONDS && m_acc.gaps < 0xFFFF) {
      ++m_acc.gaps;
    }
    m_acc.flags |= history_record::FLAG_RESTARTED;
    m_lastCountMs = nowMs - HIST_SAMPLE_INTERVAL_MS;
    m_lastSnapMs = nowMs;
    m_lastSnapN = m_acc.n;
    accumulate(nowMs, volumeL, sampleTimestampMs);
    Serial.printf("Historico: dia %lu retomado do snapshot (amostras=%lu)\n",
                  static_cast<unsigned long>(m_acc.dayIndex),
                  static_cast<unsigned long>(m_acc.n));
    return;
  }

  // Snapshot de outro dia: entra na janela de retencao e vira pendente. Fora
  // dela (relogio para tras) e apenas ignorado.
  if (hadSnapshot && m_snap.dayIndex < day &&
      (day - m_snap.dayIndex) < history_record::SLOTS) {
    m_pendingRec = m_snap;
    m_pending = true;
    Serial.printf("Historico: fechando o dia %lu recuperado do snapshot\n",
                  static_cast<unsigned long>(m_pendingRec.dayIndex));
  }

  startDay(nowMs, day, volumeL, sampleTimestampMs);

  if (m_pending) {
    writePending(nowMs);  // gravacao imediata do dia fechado (sem bloquear)
  }
}

void History::startDay(uint32_t nowMs, uint32_t day, float volumeL,
                       uint64_t sampleTimestampMs) {
  m_acc = history_record::Record();
  m_acc.dayIndex = day;
  m_dayStarted = true;
  m_lastCountMs = nowMs - HIST_SAMPLE_INTERVAL_MS;  // a primeira amostra entra
  m_lastSnapMs = nowMs;
  m_lastSnapN = 0;

  const uint32_t nowS = static_cast<uint32_t>(sampleTimestampMs / 1000ULL);
  if (nowS > history_record::dayStart(day) + HIST_GAP_SECONDS) {
    m_acc.flags |= history_record::FLAG_LATE_START;
  }

  accumulate(nowMs, volumeL, sampleTimestampMs);
}

// Uma leitura valida = uma contagem. O gate de millis() impede duplicidade em
// chamadas repetidas de update(); atrasos maiores que 2 intervalos entram como
// lacuna, sem inventar as amostras que faltaram.
void History::accumulate(uint32_t nowMs, float volumeL,
                         uint64_t sampleTimestampMs) {
  const uint32_t ts = static_cast<uint32_t>(sampleTimestampMs / 1000ULL);

  if (m_acc.n > 0) {
    if ((nowMs - m_lastCountMs) > 2 * HIST_SAMPLE_INTERVAL_MS &&
        m_acc.gaps < 0xFFFF) {
      ++m_acc.gaps;
    }
    if (volumeL < m_acc.minL) {
      m_acc.minL = volumeL;
      m_acc.tsMin = ts;
    }
    if (volumeL > m_acc.maxL) {
      m_acc.maxL = volumeL;
      m_acc.tsMax = ts;
    }
  } else {
    m_acc.minL = volumeL;
    m_acc.maxL = volumeL;
    m_acc.tsMin = ts;
    m_acc.tsMax = ts;
    m_acc.firstTs = ts;
  }

  m_acc.sumL += static_cast<double>(volumeL);
  if (m_acc.n < 0xFFFFFFFFUL) {
    ++m_acc.n;
  }
  m_acc.lastTs = ts;
  m_lastCountMs = nowMs;
}

// Congela o dia corrente para gravacao. Se ja existe um dia pendente (mais de
// 24 h sem conseguir gravar), o mais antigo e perdido: isso e registrado no
// Serial e no bit global de historico degradado, sem fila nem recuperacao.
void History::closeDay() {
  if (!m_dayStarted) {
    return;
  }
  m_dayStarted = false;

  if (m_acc.n == 0) {
    m_acc = history_record::Record();
    return;
  }

  const uint32_t dayEnd =
      history_record::dayStart(m_acc.dayIndex) + history_record::DAY_SECONDS;
  if (m_acc.lastTs + HIST_GAP_SECONDS < dayEnd) {
    m_acc.flags |= history_record::FLAG_EARLY_END;
  }

  if (m_pending) {
    Serial.printf("Historico: dia %lu perdido (falha de gravacao)\n",
                  static_cast<unsigned long>(m_pendingRec.dayIndex));
    m_degraded = true;
  }

  m_pendingRec = m_acc;
  m_pending = true;
  m_acc = history_record::Record();
}

void History::maybeSnapshot(uint32_t nowMs) {
  if (!m_dayStarted || m_acc.n == 0) {
    return;
  }
  if (m_acc.n == m_lastSnapN) {
    return;  // nada novo desde o ultimo snapshot
  }
  if ((nowMs - m_lastSnapMs) < HIST_SNAPSHOT_INTERVAL_MS) {
    return;
  }
  m_lastSnapMs = nowMs;
  if (writeSnapshot()) {
    m_lastSnapN = m_acc.n;
  }
}

// ---------------------------------------------------------------------------
//  Gravacao (sempre no arquivo INATIVO, seguida de verificacao)
// ---------------------------------------------------------------------------
bool History::writeSnapshot() {
  size_t freeBytes = 0;
  if (!spaceAvailable(freeBytes)) {
    return false;
  }

  const uint8_t target =
      m_haveSnapshot ? static_cast<uint8_t>(1 - m_activeSnap) : 0;
  const uint32_t gen = m_snapGen + 1;

  history_record::Record record = m_acc;
  record.gen = gen;

  uint8_t buffer[history_record::SNAPSHOT_SIZE];
  history_record::encodeRecord(buffer, record);

  File out = LittleFS.open(SNAPSHOT_PATHS[target], "w");
  bool ok = out && writeExact(out, buffer, sizeof(buffer));
  if (out) {
    out.close();
  }

  if (ok) {
    File in = LittleFS.open(SNAPSHOT_PATHS[target], "r");
    uint8_t readBack[history_record::SNAPSHOT_SIZE];
    history_record::Record check;
    ok = in && in.size() == history_record::SNAPSHOT_SIZE &&
         readExact(in, readBack, sizeof(readBack)) &&
         history_record::decodeRecord(readBack, check) ==
             history_record::SlotState::VALID &&
         check.gen == gen && check.dayIndex == record.dayIndex;
    if (in) {
      in.close();
    }
  }

  if (!ok) {
    LittleFS.remove(SNAPSHOT_PATHS[target]);
    ++m_writeFailures;
    Serial.println("Historico: falha ao gravar snapshot; versao anterior mantida.");
    return false;
  }

  m_activeSnap = target;
  m_haveSnapshot = true;
  m_snapGen = gen;
  return true;
}

bool History::writePending(uint32_t nowMs) {
  m_lastAttemptMs = nowMs;
  if (!m_pending || !m_ready) {
    return false;
  }

  size_t freeBytes = 0;
  if (!spaceAvailable(freeBytes)) {
    return false;
  }

  const uint16_t slot =
      static_cast<uint16_t>(m_pendingRec.dayIndex % history_record::SLOTS);
  const uint8_t target =
      m_haveTable ? static_cast<uint8_t>(1 - m_activeTable) : 0;
  const uint32_t gen = m_tableGen + 1;

  // Pre-passo: conta os dias que serao mantidos, ignorando o slot substituido.
  TableScan source;
  if (m_haveTable) {
    scanTable(m_activeTable, source, slot);
  }

  history_record::Header header;
  header.flags = source.header.flags;
  if (m_degraded) {
    header.flags |= history_record::HEADER_DEGRADED;
  }
  header.gen = gen;
  header.count = static_cast<uint16_t>(source.valid + 1);
  header.firstDay = m_pendingRec.dayIndex;
  header.lastDay = m_pendingRec.dayIndex;
  if (source.valid > 0) {
    if (source.firstDay < header.firstDay) {
      header.firstDay = source.firstDay;
    }
    if (source.lastDay > header.lastDay) {
      header.lastDay = source.lastDay;
    }
  }

  uint8_t headerBytes[history_record::HEADER_SIZE];
  history_record::encodeHeader(headerBytes, header);

  File in;
  if (m_haveTable) {
    in = LittleFS.open(TABLE_PATHS[m_activeTable], "r");
    if (!in) {
      // Sem a tabela ativa nao ha como preservar os dias: nao grava nada.
      ++m_writeFailures;
      Serial.println("Historico: tabela ativa ilegivel; gravacao adiada.");
      return false;
    }
  }

  File out = LittleFS.open(TABLE_PATHS[target], "w");
  bool ok = out && writeExact(out, headerBytes, sizeof(headerBytes));

  uint8_t buffer[history_record::RECORD_SIZE];
  for (uint16_t i = 0; ok && i < history_record::SLOTS; ++i) {
    if (i == slot) {
      history_record::Record record = m_pendingRec;
      record.gen = gen;
      history_record::encodeRecord(buffer, record);
    } else {
      // Slots sao copiados byte a byte: vazios continuam zerados e registros
      // validos mantem exatamente o mesmo conteudo/CRC.
      memset(buffer, 0, sizeof(buffer));
      if (in) {
        const uint32_t offset =
            history_record::HEADER_SIZE +
            static_cast<uint32_t>(i) * history_record::RECORD_SIZE;
        if (!in.seek(offset) || !readExact(in, buffer, sizeof(buffer))) {
          // Nunca substitui um dia existente por um slot zerado.
          ok = false;
          break;
        }
      }
    }
    ok = writeExact(out, buffer, sizeof(buffer));
  }

  uint8_t footerBytes[history_record::FOOTER_SIZE];
  history_record::encodeFooter(footerBytes, header.count);
  if (ok) {
    ok = writeExact(out, footerBytes, sizeof(footerBytes));
  }

  if (out) {
    out.close();
  }
  if (in) {
    in.close();
  }

  if (!ok) {
    LittleFS.remove(TABLE_PATHS[target]);
    ++m_writeFailures;
    Serial.println("Historico: falha ao gravar tabela; versao anterior mantida.");
    return false;
  }

  TableScan verify;
  scanTable(target, verify);
  if (!verify.usable() || verify.header.gen != gen ||
      verify.valid != header.count) {
    LittleFS.remove(TABLE_PATHS[target]);
    ++m_writeFailures;
    Serial.println("Historico: verificacao falhou; versao anterior mantida.");
    return false;
  }

  m_activeTable = target;
  m_haveTable = true;
  m_tableGen = gen;
  m_pending = false;

  Serial.printf("Historico: dia %lu gravado (slot %u, %u dias, gen=%lu)\n",
                static_cast<unsigned long>(m_pendingRec.dayIndex),
                static_cast<unsigned>(slot),
                static_cast<unsigned>(header.count),
                static_cast<unsigned long>(gen));
  return true;
}

// ---------------------------------------------------------------------------
//  Consultas
//
//  A cobertura e sempre calculada DENTRO de um dia (firstTs/lastTs do proprio
//  registro). A agregacao de um periodo soma as contribuicoes diarias: nenhum
//  span atravessa a meia-noite.
// ---------------------------------------------------------------------------
namespace {

float clampRatio(float value) {
  if (!(value > 0.0f)) {
    return 0.0f;
  }
  return value > 1.0f ? 1.0f : value;
}

void fillDayReport(const history_record::Record &record, uint32_t nowEpochSeconds,
                   HistoryDayReport &report) {
  const uint32_t intervalS = HIST_SAMPLE_INTERVAL_MS / 1000UL;

  report.dayIndex = record.dayIndex;
  report.hasData = record.n > 0;
  report.minL = record.minL;
  report.maxL = record.maxL;
  report.tsMin = record.tsMin;
  report.tsMax = record.tsMax;
  report.samples = record.n;
  report.gaps = record.gaps;
  report.flags = record.flags;
  report.sumL = record.sumL;
  report.avgL = record.n ? static_cast<float>(record.sumL / record.n) : 0.0f;

  report.spanS = record.lastTs >= record.firstTs
                     ? record.lastTs - record.firstTs + intervalS
                     : 0;

  // Para o dia corrente a referencia e o tempo ja decorrido desde a meia-noite;
  // para dias fechados, o dia inteiro.
  uint32_t referenceS = history_record::DAY_SECONDS;
  if (nowEpochSeconds != 0 &&
      history_record::dayIndex(nowEpochSeconds) == record.dayIndex) {
    const uint32_t startS = history_record::dayStart(record.dayIndex);
    const uint32_t elapsed =
        nowEpochSeconds > startS ? nowEpochSeconds - startS : 0;
    referenceS = elapsed > 0 ? elapsed : 1;
  }

  const float expected =
      report.spanS > 0 ? static_cast<float>(report.spanS) / intervalS : 0.0f;
  report.sampleRatio =
      expected > 0.0f ? clampRatio(static_cast<float>(report.samples) / expected)
                      : 0.0f;
  report.timeRatio =
      clampRatio(static_cast<float>(report.spanS) / static_cast<float>(referenceS));
  report.coverage = clampRatio(
      static_cast<float>(report.samples) /
      static_cast<float>(history_record::NOMINAL_SAMPLES_PER_DAY));

  if (!report.hasData) {
    report.status = HistoryCoverage::NO_DATA;
    return;
  }

  const uint8_t blocking = history_record::FLAG_RESTARTED |
                           history_record::FLAG_LATE_START |
                           history_record::FLAG_EARLY_END;
  const bool complete =
      report.gaps == 0 && (report.flags & blocking) == 0 &&
      report.sampleRatio >= HIST_COMPLETE_RATIO &&
      report.timeRatio >= HIST_COMPLETE_RATIO;
  report.status = complete ? HistoryCoverage::COMPLETE : HistoryCoverage::PARTIAL;
}

}  // namespace

bool History::readDay(uint32_t day, history_record::Record &record) const {
  if (m_dayStarted && m_acc.dayIndex == day) {
    record = m_acc;
    return record.n > 0;
  }
  if (m_pending && m_pendingRec.dayIndex == day) {
    record = m_pendingRec;
    return record.n > 0;
  }
  if (!m_ready || !m_haveTable) {
    return false;
  }

  File file = LittleFS.open(TABLE_PATHS[m_activeTable], "r");
  if (!file) {
    return false;
  }

  const uint16_t slot = static_cast<uint16_t>(day % history_record::SLOTS);
  const uint32_t offset =
      history_record::HEADER_SIZE +
      static_cast<uint32_t>(slot) * history_record::RECORD_SIZE;

  uint8_t buffer[history_record::RECORD_SIZE];
  const bool ok =
      file.seek(offset) && readExact(file, buffer, sizeof(buffer)) &&
      history_record::decodeSlot(buffer, slot, record) ==
          history_record::SlotState::VALID &&
      record.dayIndex == day && record.n > 0;
  file.close();
  return ok;
}

bool History::dayReport(uint32_t day, uint32_t nowEpochSeconds,
                        HistoryDayReport &report) const {
  report = HistoryDayReport();
  report.dayIndex = day;

  history_record::Record record;
  if (!readDay(day, record)) {
    report.status = HistoryCoverage::NO_DATA;
    return false;
  }

  fillDayReport(record, nowEpochSeconds, report);
  return true;
}

bool History::periodReport(uint32_t nowEpochSeconds, uint16_t days,
                           HistoryPeriodReport &report) const {
  report = HistoryPeriodReport();
  if (days == 0 || nowEpochSeconds == 0) {
    return false;
  }

  const uint32_t today = history_record::dayIndex(nowEpochSeconds);
  const uint32_t firstDay =
      (today + 1) > static_cast<uint32_t>(days) ? (today + 1 - days) : 0;

  // Janela da API antiga, INALTERADA: comeca em firstDay e termina em HOJE
  // (o dia corrente entra e e comparado com o tempo ja decorrido).
  return aggregateDays(firstDay, today, days, nowEpochSeconds, report);
}

// Periodo composto apenas por dias civis fechados: a janela termina em ontem e
// recua `days` dias (7 -> hoje-7 .. hoje-1; 30 -> hoje-30 .. hoje-1, o anel
// inteiro). Como o dia corrente nunca e consultado, o snapshot em RAM nao
// participa e todos os dias examinados usam a referencia de 24 h.
bool History::closedPeriodReport(uint32_t nowEpochSeconds, uint16_t days,
                                 HistoryPeriodReport &report) const {
  report = HistoryPeriodReport();
  if (days == 0 || days > history_record::SLOTS || nowEpochSeconds == 0) {
    return false;
  }

  const uint32_t today = history_record::dayIndex(nowEpochSeconds);
  if (today == 0) {
    return false;  // sem dia civil valido nao existe "ontem"
  }

  const uint32_t lastDay = today - 1;
  // dayIndex 0 e invalido no formato: a janela nunca desce abaixo de 1.
  const uint32_t firstDay =
      today > static_cast<uint32_t>(days) ? (today - days) : 1;

  return aggregateDays(firstDay, lastDay, days, nowEpochSeconds, report);
}

// ---------------------------------------------------------------------------
//  Agregacao comum a periodReport() e closedPeriodReport()
//
//  Percorre [firstDay, lastDay] inclusive e soma as contribuicoes diarias:
//  nenhum span atravessa a meia-noite, entao o span total e a soma dos spans e
//  a media e ponderada pelo numero de amostras de cada dia. Dias sem dado sao
//  pulados (tornam o periodo PARCIAL) e um dia nao COMPLETO tambem impede o
//  status COMPLETO do periodo. `report` chega zerado do chamador.
// ---------------------------------------------------------------------------
bool History::aggregateDays(uint32_t firstDay, uint32_t lastDay, uint16_t days,
                            uint32_t nowEpochSeconds,
                            HistoryPeriodReport &report) const {
  report.days = days;
  bool allComplete = true;
  uint64_t spanTotal = 0;
  double sumTotal = 0.0;

  for (uint32_t day = firstDay; day <= lastDay; ++day) {
    HistoryDayReport dayValue;
    dayReport(day, nowEpochSeconds, dayValue);

    if (!dayValue.hasData) {
      allComplete = false;
      continue;
    }

    ++report.daysWithData;
    report.samples += dayValue.samples;
    spanTotal += dayValue.spanS;
    sumTotal += dayValue.sumL;

    if (report.daysWithData == 1 || dayValue.minL < report.minL) {
      report.minL = dayValue.minL;
      report.tsMin = dayValue.tsMin;
    }
    if (report.daysWithData == 1 || dayValue.maxL > report.maxL) {
      report.maxL = dayValue.maxL;
      report.tsMax = dayValue.tsMax;
    }
    if (dayValue.status != HistoryCoverage::COMPLETE) {
      allComplete = false;
    }
  }

  if (report.samples == 0) {
    report.status = HistoryCoverage::NO_DATA;
    return false;
  }

  const float referenceS =
      static_cast<float>(days) * history_record::DAY_SECONDS;
  const float nominal =
      static_cast<float>(days) * history_record::NOMINAL_SAMPLES_PER_DAY;

  report.avgL = static_cast<float>(sumTotal / report.samples);
  report.coverage = clampRatio(static_cast<float>(report.samples) / nominal);
  report.timeRatio = clampRatio(static_cast<float>(spanTotal) / referenceS);
  report.sampleRatio =
      spanTotal > 0
          ? clampRatio(static_cast<float>(report.samples) /
                       (static_cast<float>(spanTotal) / (HIST_SAMPLE_INTERVAL_MS / 1000UL)))
          : 0.0f;
  report.status = (allComplete && report.daysWithData == days)
                      ? HistoryCoverage::COMPLETE
                      : HistoryCoverage::PARTIAL;
  return true;
}





