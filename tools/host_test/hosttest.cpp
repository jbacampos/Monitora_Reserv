// Teste de host (fora do firmware) dos modulos puros: MovingAverage e volume.
// Compilado com clang++ junto com src/moving_average.cpp e src/volume.cpp.
#include <cmath>
#include <cstdio>
#include <cstdint>

#include "moving_average.h"
#include "publish_policy.h"
#include "volume.h"

namespace {

int g_failures = 0;

void check(bool ok, const char *label) {
  std::printf("%s %s\n", ok ? "[ok]   " : "[FALHA]", label);
  if (!ok) {
    ++g_failures;
  }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

// Cadeia do programa original, com os tipos inteiros que ele usava.
// Serve apenas como referencia para medir a diferenca de arredondamento.
long originalLiters(int adc) {
  const double tensao = adc * (3.3 / 1023.0);
  const long leitura = static_cast<long>(tensao / 1.1 * 1000.0);
  long nivel = static_cast<long>((leitura - 50) / 1.05);
  if (nivel < 0) {
    nivel = 0;
  }
  return static_cast<long>(nivel * 41.52);
}

// Pre-visualizacao do display: mesma sequencia da biblioteca
// (showNumberBaseEx com leading_zero=false + showDots sobre 6 digitos).
void preview(long value, uint8_t dots, char *out) {
  uint8_t digits[6] = {0, 0, 0, 0, 0, 0};
  long v = value;
  for (int i = 5; i >= 0; --i) {
    digits[i] = (uint8_t)(v % 10);
    v /= 10;
  }
  bool blank = true;
  for (int i = 0; i < 6; ++i) {
    if (digits[i] != 0) {
      blank = false;
    }
    if (blank) {
      digits[i] = 0xFF;  // marcador de digito apagado
    }
  }
  bool dp[6] = {false, false, false, false, false, false};
  uint8_t d = dots;
  for (int i = 0; i < 6; ++i) {
    dp[i] = (d & 0x80) != 0;
    d <<= 1;
  }
  int n = 0;
  for (int i = 0; i < 6; ++i) {
    if (digits[i] != 0xFF) {
      out[n++] = (char)('0' + digits[i]);
    }
    if (dp[i]) {
      out[n++] = '.';
    }
  }
  if (n == 0) {
    out[n++] = '0';
  }
  out[n] = '\0';
}

// Espelho da resolucao de ThingsBoard::update()/enqueue(): a decisao nunca
// depende do transporte e o baseline da politica so avanca depois de a
// gravacao ser confirmada na fila.
struct Gate {
  bool hasPublished = false;
  bool publishAfterReconnect = false;
  bool appendOk = true;
  float lastPublishedL = 0.0f;
  uint32_t lastPublishMs = 0;
  uint32_t pending = 0;
  uint32_t published = 0;

  void registerPublished(uint32_t nowMs, float volumeL) {
    hasPublished = true;
    lastPublishedL = volumeL;
    lastPublishMs = nowMs;
  }

  bool enqueue(uint32_t nowMs, float volumeL) {
    if (!appendOk) {
      return false;  // gravacao nao confirmada: nada avanca
    }
    registerPublished(nowMs, volumeL);
    publishAfterReconnect = false;
    ++pending;
    return true;
  }

  publish_policy::Reason step(uint32_t nowMs, float volumeL, bool online) {
    publish_policy::Input in;
    in.sampleValid = true;
    in.hasPublished = hasPublished;
    in.publishAfterReconnect = publishAfterReconnect;
    in.volumeL = volumeL;
    in.lastPublishedL = lastPublishedL;
    in.nowMs = nowMs;
    in.lastPublishMs = lastPublishMs;
    in.deltaLiters = 50.0f;
    in.heartbeatMs = 3600000UL;
    in.minIntervalMs = 10000UL;

    const publish_policy::Reason reason = publish_policy::decide(in);
    if (reason == publish_policy::Reason::NONE) {
      return reason;
    }
    if (online && pending == 0) {
      registerPublished(nowMs, volumeL);
      publishAfterReconnect = false;
      ++published;
      return reason;
    }
    enqueue(nowMs, volumeL);
    return reason;
  }
};

}  // namespace
int main() {
  std::printf("== MovingAverage ==\n");

  MovingAverage mv(50);
  check(mv.sampleCount() == 0 && mv.average() == 0.0f,
        "buffer zerado: contagem 0 e media 0");

  mv.add(10.0f);
  check(near(mv.average(), 10.0, 1e-6) && mv.sampleCount() == 1,
        "1 amostra: media = 10 (posicoes nao preenchidas ignoradas)");

  mv.add(20.0f);
  check(near(mv.average(), 15.0, 1e-6), "2 amostras: media = 15");

  MovingAverage full(50);
  for (int i = 1; i <= 50; ++i) {
    full.add((float)i);
  }
  check(full.full() && full.sampleCount() == 50 && near(full.average(), 25.5, 1e-5),
        "50 amostras 1..50: media = 25,5 e janela cheia");

  full.add(100.0f);
  check(near(full.average(), (1275.0 - 1.0 + 100.0) / 50.0, 1e-5),
        "janela cheia: descarta a mais antiga (media = 27,48)");

  // Equivalencia com a media ingenua das ultimas 50 amostras.
  MovingAverage cont(50);
  float history[50] = {0};
  size_t idx = 0;
  size_t seen = 0;
  uint32_t rng = 0x12345678u;
  double worst = 0.0;
  for (int step = 0; step < 20000; ++step) {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    const float value = (float)(rng % 100000u) + 0.5f;
    cont.add(value);
    history[idx] = value;
    idx = (idx + 1) % 50;
    if (seen < 50) {
      ++seen;
    }
    double sum = 0.0;
    for (size_t i = 0; i < seen; ++i) {
      sum += history[i];
    }
    const double delta = std::fabs(sum / (double)seen - (double)cont.average());
    if (delta > worst) {
      worst = delta;
    }
  }
  std::printf("        maior desvio vs media ingenua em 20000 passos: %.6f L\n",
              worst);
  check(worst < 0.02,
        "soma acumulada equivale a media ingenua (O(1) por amostra)");

  cont.reset();
  check(cont.sampleCount() == 0 && cont.average() == 0.0f,
        "reset(): contagem e media voltam a zero");

  MovingAverage small(5);
  for (int i = 1; i <= 9; ++i) {
    small.add((float)i);
  }
  check(small.sampleCount() == 5 && near(small.average(), 7.0, 1e-6),
        "janela de 5 amostras (5..9 apos descartar 1..4)");

  std::printf("\n== volume::compute ==\n");

  const volume::Measurement zero = volume::compute(0);
  check(zero.liters == 0.0f && zero.levelMm == 0.0f, "ADC 0 -> volume 0");

  const volume::Measurement top = volume::compute(1023);
  check(near(top.voltageV, 3.3, 1e-4) && near(top.sensorMm, 3000.0, 1e-3) &&
            near(top.levelMm, (3000.0 - 50.0) / 1.05, 1e-3) &&
            near(top.liters, (3000.0 - 50.0) / 1.05 * 41.52, 1e-2),
        "ADC 1023 -> 3,3 V | 3000 mm | 2809,52 mm | 116651,4 L");

  const volume::Measurement critical = volume::compute(17);
  check(critical.levelMm == 0.0f && critical.liters == 0.0f &&
            critical.sensorMm < 50.0f,
        "abaixo do offset: nivel e volume limitados a zero (CRITICAL)");

  bool roundTripOk = true;
  const float levels[] = {0.0f, 200.0f, 1500.0f, 2500.0f};
  for (float level : levels) {
    const float adc = volume::levelMmToAdc(level);
    const volume::Measurement m = volume::compute((uint16_t)(adc + 0.5f));
    if (!near(m.levelMm, level, 1.0)) {
      roundTripOk = false;
      std::printf("        nivel %.0f mm -> ADC %.2f -> nivel %.2f mm\n", level,
                  adc, m.levelMm);
    }
  }
  check(roundTripOk, "levelMmToAdc/compute sao inversos (erro < 1 mm)");

  std::printf("\n== comparacao com o programa original (inteiros) ==\n");
  const int adcs[] = {17, 100, 342, 500, 750, 900, 1023};
  double worstLiters = 0.0;
  for (int adc : adcs) {
    const volume::Measurement m = volume::compute((uint16_t)adc);
    const long reference = originalLiters(adc);
    const double delta = std::fabs((double)m.liters - (double)reference);
    if (delta > worstLiters) {
      worstLiters = delta;
    }
    std::printf("        ADC %4d: novo=%9.1f L   original=%9ld L   dif=%+.1f L\n",
                adc, (double)m.liters, reference,
                (double)m.liters - (double)reference);
  }
  std::printf("        maior diferenca: %.1f L (truncamento dos long do original)\n",
              worstLiters);

  std::printf("\n== formato do display (showNumberDec + ponto de milhar) ==\n");
  struct Case {
    long value;
    const char *expected;
  };
  const Case cases[] = {
      {830, "830"},   {1234, "1.234"},   {12345, "12.345"},
      {123456, "123.456"}, {0, "0"},
  };
  bool formatOk = true;
  for (const Case &c : cases) {
    char text[16];
    const uint8_t dots = (c.value < 1000) ? 0 : 0b00100000;
    preview(c.value, dots, text);
    bool same = true;
    for (int i = 0;; ++i) {
      if (c.expected[i] != text[i]) {
        same = (c.expected[i] == '\0' && text[i] == '\0');
        if (same) {
          break;
        }
        formatOk = false;
        break;
      }
      if (c.expected[i] == '\0') {
        break;
      }
    }
    std::printf("        %7ld -> \"%s\" (esperado \"%s\")\n", c.value, text,
                c.expected);
  }
  check(formatOk, "ponto de milhar apenas a partir de 1000 L");

  // -------------------------------------------------------------------------
  //  Politica de publicacao (Etapa 2)
  // -------------------------------------------------------------------------
  std::printf("\n== publish_policy::decide ==\n");

  publish_policy::Input in;
  in.deltaLiters = 50.0f;
  in.heartbeatMs = 3600000UL;
  in.minIntervalMs = 10000UL;
  in.lastPublishMs = 1000;
  in.nowMs = 2000;
  in.lastPublishedL = 1000.0f;
  in.volumeL = 1000.0f;
  in.hasPublished = false;
  in.publishAfterReconnect = false;
  in.sampleValid = false;

  check(publish_policy::decide(in) == publish_policy::Reason::NONE,
        "sem leitura valida: nao publica");

  in.sampleValid = true;
  check(publish_policy::decide(in) == publish_policy::Reason::FIRST,
        "primeira leitura valida: publica (motivo FIRST)");

  in.hasPublished = true;
  check(publish_policy::decide(in) == publish_policy::Reason::NONE,
        "volume igual e heartbeat distante: nao publica");

  in.volumeL = 1049.0f;  // +49 L
  check(publish_policy::decide(in) == publish_policy::Reason::NONE,
        "variacao abaixo do delta (49 L): nao publica");

  in.volumeL = 1050.0f;  // +50 L, mas so 1 s desde a ultima publicacao
  check(publish_policy::decide(in) == publish_policy::Reason::NONE,
        "delta atingido antes do intervalo minimo: espera");

  in.nowMs = 11000;  // 10 s desde a ultima publicacao
  check(publish_policy::decide(in) == publish_policy::Reason::DELTA,
        "delta de 50 L apos o intervalo minimo: publica (motivo DELTA)");

  in.nowMs = 1000UL + 3600000UL;
  in.volumeL = 1000.0f;
  check(publish_policy::decide(in) == publish_policy::Reason::HEARTBEAT,
        "60 min sem mudanca: publica (motivo HEARTBEAT)");

  in.nowMs = 2000;
  in.publishAfterReconnect = true;
  check(publish_policy::decide(in) == publish_policy::Reason::RECONNECT,
        "reconexao: publica imediatamente mesmo sem mudanca");

  // -------------------------------------------------------------------------
  //  Offline: decisao independente do transporte (fila preserva a amostra)
  // -------------------------------------------------------------------------
  std::printf("\n== offline: decisao independente do transporte ==\n");

  Gate gate;
  gate.step(1000, 1000.0f, true);
  check(gate.published == 1 && gate.pending == 0,
        "online: FIRST publica e a fila fica vazia");

  gate.publishAfterReconnect = true;
  check(gate.step(2000, 1000.0f, false) == publish_policy::Reason::RECONNECT,
        "offline: politica decide RECONNECT");
  check(gate.pending == 1 && !gate.publishAfterReconnect,
        "offline: RECONNECT vira 1 registro e nao fica pendente");

  for (int step = 0; step < 39; ++step) {
    gate.step(2250 + 250 * (uint32_t)step, (step % 2) ? 1005.0f : 1000.0f, false);
  }
  check(gate.pending == 1,
        "offline ~10 s sem variacao: nada entra por ciclo de 250 ms");

  gate.step(12000, 1080.0f, false);
  check(gate.pending == 2, "offline: DELTA apos o intervalo minimo enfileira");
  gate.step(12250, 1140.0f, false);
  check(gate.pending == 2, "offline: DELTA antes do intervalo minimo espera");
  gate.step(22000, 1140.0f, false);
  check(gate.pending == 3, "offline: DELTA apos o intervalo enfileira de novo");

  Gate hb;
  hb.step(1000, 500.0f, true);
  hb.publishAfterReconnect = true;
  hb.step(2000, 500.0f, false);
  hb.step(2000UL + 3600000UL, 500.0f, false);
  check(hb.pending == 2 && hb.step(2000UL + 3600000UL, 500.0f, false) ==
                               publish_policy::Reason::NONE,
        "offline: heartbeat enfileira uma vez, sem repetir no ciclo seguinte");

  Gate bad;
  bad.step(1000, 500.0f, true);
  bad.publishAfterReconnect = true;
  bad.appendOk = false;
  bad.step(2000, 500.0f, false);
  check(bad.pending == 0 && bad.publishAfterReconnect && bad.lastPublishMs == 1000,
        "falha de gravacao: nada e criado e o baseline nao avanca");
  bad.appendOk = true;
  bad.step(2250, 500.0f, false);
  check(bad.pending == 1 && !bad.publishAfterReconnect,
        "retentativa: grava uma unica vez e libera o RECONNECT");

  Gate backlog;
  backlog.step(1000, 500.0f, true);
  backlog.publishAfterReconnect = true;
  backlog.step(2000, 500.0f, false);
  backlog.step(22000, 620.0f, false);
  backlog.step(33000, 700.0f, true);
  check(backlog.pending == 3 && backlog.published == 1,
        "reconexao com backlog: leitura nova entra na fila (ordem preservada)");
  backlog.pending = 0;
  backlog.step(45000, 780.0f, true);
  check(backlog.published == 2, "fila vazia: volta a publicar direto");

  Gate boot;
  boot.step(1000, 400.0f, false);
  check(boot.pending == 1 && boot.published == 0,
        "primeira leitura sem conexao: vai para a fila (FIRST)");

  std::printf("\n%s (%d falha(s))\n",
              g_failures == 0 ? "TODOS OS TESTES PASSARAM" : "HOUVE FALHAS",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}

