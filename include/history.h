#pragma once

#include <Arduino.h>

#include "history_record.h"

// ============================================================================
//  Historico diario do reservatorio (recurso auxiliar)
//
//  Guarda 30 dias fechados em uma tabela circular (A/B) e o dia corrente em um
//  snapshot gravado a cada 5 minutos (A/B). O volume e ESTIMADO: perder um dia
//  ocasionalmente e aceitavel, e nada aqui bloqueia sensor, display, MQTT ou
//  Telegram.
//
//  O LittleFS e montado pela fila offline (OfflineQueue::begin). Este modulo
//  apenas verifica se ele esta disponivel e NUNCA formata: se a montagem falhar,
//  o historico fica desativado e o resto do firmware segue normalmente.
//
//  Amostragem: uma contagem a cada HIST_SAMPLE_INTERVAL_MS (10 s), controlada
//  por millis(); o dia de cada amostra vem do timestamp da MEDICAO (NTP), nao
//  do instante em que o loop a processou.
// ============================================================================
enum class HistoryCoverage : uint8_t {
  NO_DATA = 0,   // nenhum registro no dia
  PARTIAL = 1,   // houve dados, mas com lacuna, reinicio, borda ou cobertura baixa
  COMPLETE = 2,  // dia inteiro, sem interrupcao conhecida
};

struct HistoryDayReport {
  uint32_t dayIndex = 0;
  bool hasData = false;
  float minL = 0.0f;
  float maxL = 0.0f;
  float avgL = 0.0f;
  uint32_t tsMin = 0;  // epoch s do minimo
  uint32_t tsMax = 0;  // epoch s do maximo
  uint32_t samples = 0;
  uint32_t spanS = 0;  // lastTs - firstTs + intervalo (nunca atravessa dias)
  uint16_t gaps = 0;
  uint8_t flags = 0;
  double sumL = 0.0;       // usado para agregar periodos sem perder precisao
  float coverage = 0.0f;   // amostras / 8640
  float sampleRatio = 0.0f;  // amostras / (span / 10 s)
  float timeRatio = 0.0f;    // span / dia inteiro (ou / tempo decorrido, hoje)
  HistoryCoverage status = HistoryCoverage::NO_DATA;
};

struct HistoryPeriodReport {
  uint16_t days = 0;          // dias civis do periodo
  uint16_t daysWithData = 0;  // dias com ao menos uma amostra
  uint32_t samples = 0;
  float minL = 0.0f;
  float maxL = 0.0f;
  float avgL = 0.0f;
  uint32_t tsMin = 0;
  uint32_t tsMax = 0;
  float coverage = 0.0f;
  float sampleRatio = 0.0f;
  float timeRatio = 0.0f;
  HistoryCoverage status = HistoryCoverage::NO_DATA;
};

class History {
 public:
  // Deve ser chamado depois de ThingsBoard::begin (que monta o LittleFS).
  void begin(uint32_t nowMs);

  // Chamado continuamente pelo loop. So contabiliza quando ha leitura valida e
  // relogio sincronizado; nunca bloqueia (as gravacoes sao pequenas e curtas).
  void update(uint32_t nowMs, bool clockSynced, uint64_t sampleTimestampMs,
              float volumeL, bool sampleValid);

  bool ready() const { return m_ready; }
  bool degraded() const { return m_degraded; }
  uint32_t writeFailures() const { return m_writeFailures; }
  bool hasPendingDay() const { return m_pending; }
  uint32_t currentDayIndex() const { return m_dayStarted ? m_acc.dayIndex : 0; }

  // Consultas (usadas pelo futuro /status). `nowEpochSeconds` permite que o dia
  // corrente seja comparado com o tempo ja decorrido, e nao com 24 h.
  bool dayReport(uint32_t dayIndex, uint32_t nowEpochSeconds,
                 HistoryDayReport &report) const;

  // Periodo que inclui o dia corrente: `days` = 7 cobre hoje e os 6 anteriores.
  bool periodReport(uint32_t nowEpochSeconds, uint16_t days,
                    HistoryPeriodReport &report) const;

  // Periodo composto SOMENTE por dias civis fechados, anteriores ao dia atual:
  //   days = 7  -> ontem e os 6 dias civis anteriores (hoje - 7 ... hoje - 1)
  //   days = 30 -> ontem e os 29 dias civis anteriores (hoje - 30 ... hoje - 1)
  //
  // O dia corrente NUNCA entra (nem o snapshot em RAM): a janela termina em
  // ontem. Todos os dias examinados sao fechados, portanto a referencia de
  // cobertura e sempre o dia inteiro (24 h), nunca o tempo decorrido.
  //
  // Usa a MESMA agregacao, os mesmos indicadores de cobertura e o mesmo
  // `HistoryPeriodReport` de periodReport(): dias sem dado sao pulados (e
  // tornam o periodo PARCIAL), dias parciais/degradados mantem o status que o
  // proprio dia reporta, e o status do periodo so e COMPLETO quando todos os
  // `days` solicitados existem e cada um e COMPLETO.
  //
  // `days` deve estar em [1, SLOTS] (30 = todo o anel de dias fechados). Com 0,
  // acima do anel, ou sem relogio valido (nowEpochSeconds == 0) devolve false e
  // o relatorio fica zerado (status NO_DATA). Quando o historico ainda nao tem
  // todos os dias do periodo, devolve o que existe com daysWithData < days e
  // status PARCIAL (cobertura sempre relativa aos `days` pedidos).
  bool closedPeriodReport(uint32_t nowEpochSeconds, uint16_t days,
                          HistoryPeriodReport &report) const;

 private:
  // Agregacao comum a periodReport() e closedPeriodReport(). Percorre
  // [firstDay, lastDay] inclusive e preenche `report`, que deve chegar ZERADO
  // (as duas APIs publicas fazem `report = HistoryPeriodReport()` antes). Nao
  // conhece o conceito de "hoje": quem define a janela e o chamador.
  bool aggregateDays(uint32_t firstDay, uint32_t lastDay, uint16_t days,
                     uint32_t nowEpochSeconds,
                     HistoryPeriodReport &report) const;

  bool readDay(uint32_t dayIndex, history_record::Record &record) const;
  void startFirstDay(uint32_t nowMs, uint32_t day, float volumeL,
                     uint64_t sampleTimestampMs);
  void startDay(uint32_t nowMs, uint32_t day, float volumeL,
                uint64_t sampleTimestampMs);
  void accumulate(uint32_t nowMs, float volumeL, uint64_t sampleTimestampMs);
  void closeDay();
  void maybeSnapshot(uint32_t nowMs);
  bool writeSnapshot();
  bool writePending(uint32_t nowMs);
  bool spaceAvailable(size_t &freeBytes);
  bool loadSnapshot();

  history_record::Record m_acc;         // dia corrente
  history_record::Record m_pendingRec;  // dia fechado aguardando gravacao
  history_record::Record m_snap;        // snapshot recuperado no boot

  uint32_t m_lastCountMs = 0;   // gate de 10 s (millis)
  uint32_t m_lastSnapMs = 0;    // ultima tentativa de snapshot
  uint32_t m_lastSnapN = 0;     // amostras no ultimo snapshot bem-sucedido
  uint32_t m_lastAttemptMs = 0; // ultima tentativa de gravar o dia fechado
  uint32_t m_tableGen = 0;
  uint32_t m_snapGen = 0;
  size_t m_minFreeBytes = 0;    // HIST_MIN_FREE_BLOCKS * blockSize real

  uint8_t m_activeTable = 0;
  uint8_t m_activeSnap = 0;

  bool m_ready = false;
  bool m_haveTable = false;
  bool m_haveSnapshot = false;
  bool m_dayStarted = false;
  bool m_pending = false;
  bool m_degraded = false;
  bool m_spaceWarned = false;
  uint32_t m_writeFailures = 0;
};
