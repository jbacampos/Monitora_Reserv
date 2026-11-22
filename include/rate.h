#pragma once

#include <stdint.h>

// ============================================================================
//  Taxa de variacao recente do volume - logica PURA (sem Arduino, sem flash)
//
//  Mantem uma serie CURTA em RAM com a media movel JA publicada
//  (g_filteredLiters - a mesma do display, do MQTT, do /status e dos alertas) e
//  devolve a inclinacao em litros por hora. Nada e persistido: a serie e perdida
//  no reboot e o indicador volta a "sem base" ate a janela encher. As constantes
//  de tempo e de limiar ficam logo abaixo (RATE_*).
// ============================================================================
namespace rate {

// ---------------------------------------------------------------------------
//  Configuracao
//
//  Fica AQUI (e nao em include/config.h) porque este header e PURO: config.h
//  inclui <Arduino.h> e arrastaria a camada Arduino para dentro do modulo - mesmo
//  padrao de include/volume.h, cujas constantes de calibracao tambem moram no
//  proprio header.
//
//  Dimensionamento (manda a resolucao do sensor): 1 contagem do ADC equivale a
//  2,93 mm = 116 L, portanto a taxa so e significativa se a variacao na janela
//  passar de ~2 contagens (~230 L):
//
//      janela >= 230 L / taxa esperada
//
//  Em 1 h: 2.000 L/h = 17 contagens (excelente), 500 L/h = 4,3 (bom),
//  250 L/h = 2,2 (limitrofe: 2 h ficaria melhor), 100 L/h = 0,9 (ruido: so com
//  ~3 h). Ajustar RATE_WINDOW_MS depois de olhar a taxa no log de producao.
// ---------------------------------------------------------------------------
constexpr uint32_t RATE_SAMPLE_INTERVAL_MS = 300000UL;  // 5 min (mesmo tick do snapshot)
constexpr uint32_t RATE_WINDOW_MS          = 3600000UL; // 1 h
constexpr uint8_t  RATE_SLOTS =
    static_cast<uint8_t>(RATE_WINDOW_MS / RATE_SAMPLE_INTERVAL_MS);
constexpr float    RATE_STABLE_LPH  = 150.0f;    // ~1,3 contagem/h: piso do ADC
constexpr uint8_t  RATE_MIN_SAMPLES = 7;         // base pronta em ~30 min
constexpr uint32_t RATE_MIN_SPAN_MS = 1800000UL; // 30 min

static_assert(RATE_WINDOW_MS % RATE_SAMPLE_INTERVAL_MS == 0,
              "a janela deve ser multipla do intervalo de amostragem");
static_assert(RATE_SLOTS * RATE_SAMPLE_INTERVAL_MS == RATE_WINDOW_MS,
              "RATE_SLOTS deve cobrir a janela inteira");
static_assert(RATE_MIN_SPAN_MS == (RATE_MIN_SAMPLES - 1) * RATE_SAMPLE_INTERVAL_MS,
              "o span minimo e o que RATE_MIN_SAMPLES pontos cobrem");
static_assert(RATE_MIN_SAMPLES <= RATE_SLOTS,
              "o minimo de pontos nao pode passar do anel");
static_assert(RATE_MIN_SAMPLES >= 3, "minimos quadrados exigem ao menos 3 pontos");
static_assert(RATE_STABLE_LPH > 0.0f, "a banda estavel deve ser positiva");

struct Ring {
  float liters[RATE_SLOTS] = {};
  uint32_t ms[RATE_SLOTS] = {};  // millis() do instante de cada amostra
  uint8_t head = 0;              // proximo slot a escrever
  uint8_t count = 0;             // pontos guardados (satura em RATE_SLOTS)
  uint32_t lastPushMs = 0;       // instante do ultimo ponto guardado
};

// Zera a serie. O ultimo push fica UM intervalo atras, de forma que a primeira
// amostra valida ja entra no anel (e nao so 5 min depois do boot). A conta
// funciona no rollover de millis() porque tudo aqui e aritmetica de uint32_t.
inline void begin(Ring &r, uint32_t nowMs) {
  r = Ring();
  r.lastPushMs = nowMs - RATE_SAMPLE_INTERVAL_MS;
}

// Indice do ponto de ordem `i` (0 = mais antigo, count-1 = mais novo). O anel
// nao esta em ordem cronologica: e uma rotacao que comeca no ponto mais antigo.
inline uint8_t slotIndex(const Ring &r, uint8_t i) {
  const uint8_t oldest =
      static_cast<uint8_t>((r.head + RATE_SLOTS - r.count) % RATE_SLOTS);
  return static_cast<uint8_t>((oldest + i) % RATE_SLOTS);
}

// Base pronta: pontos suficientes E o span minimo coberto.
inline bool hasRate(const Ring &r) {
  if (r.count < RATE_MIN_SAMPLES) {
    return false;
  }
  const uint32_t oldest = r.ms[slotIndex(r, 0)];
  const uint32_t newest = r.ms[slotIndex(r, static_cast<uint8_t>(r.count - 1))];
  return static_cast<uint32_t>(newest - oldest) >= RATE_MIN_SPAN_MS;
}

// Inclinacao em litros por hora, por MINIMOS QUADRADOS (um ponto ruim nao
// estraga a hora inteira, como aconteceria so com primeiro/ultimo). `x` entra em
// segundos relativos ao ponto mais antigo: a diferenca de uint32_t ja e o delta,
// portanto o rollover de millis() nao afeta o resultado. Devolve 0 sem base.
inline float litersPerHour(const Ring &r) {
  if (!hasRate(r)) {
    return 0.0f;
  }

  const uint32_t t0 = r.ms[slotIndex(r, 0)];
  float sx = 0.0f;
  float sy = 0.0f;
  float sxx = 0.0f;
  float sxy = 0.0f;
  for (uint8_t i = 0; i < r.count; ++i) {
    const uint8_t idx = slotIndex(r, i);
    const float x =
        static_cast<float>(static_cast<uint32_t>(r.ms[idx] - t0)) / 1000.0f;
    const float y = r.liters[idx];
    sx += x;
    sy += y;
    sxx += x * x;
    sxy += x * y;
  }

  const float n = static_cast<float>(r.count);
  const float den = n * sxx - sx * sx;
  if (den <= 0.0f) {
    return 0.0f;  // todos os pontos no mesmo instante
  }
  return ((n * sxy - sx * sy) / den) * 3600.0f;
}

// Chamado a cada iteracao do loop: guarda um ponto a cada
// RATE_SAMPLE_INTERVAL_MS (wrap-safe). Sem leitura valida nada e guardado - a
// serie acumula lacunas de tempo REAIS em vez de pontos repetidos, e a
// inclinacao continua correta porque `x` usa o instante de cada ponto.
inline void update(Ring &r, uint32_t nowMs, float liters, bool valid) {
  if (!valid) {
    return;
  }
  if (static_cast<uint32_t>(nowMs - r.lastPushMs) < RATE_SAMPLE_INTERVAL_MS) {
    return;
  }

  r.lastPushMs = nowMs;
  r.liters[r.head] = liters;
  r.ms[r.head] = nowMs;
  r.head = static_cast<uint8_t>((r.head + 1) % RATE_SLOTS);
  if (r.count < RATE_SLOTS) {
    ++r.count;
  }
}

}  // namespace rate
