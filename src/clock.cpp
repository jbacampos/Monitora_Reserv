#include "clock.h"

#include <sys/time.h>
#include <time.h>

#include "config.h"

// ============================================================================
//  Relogio do dispositivo (NTP)
//
//  O core do ESP8266 implementa gettimeofday() com resolucao de microssegundos
//  a partir de timeshift64 + micros64(): segundos e fracao de segundo vem do
//  MESMO contador, deslocado pelo ajuste do SNTP. Por isso o timestamp usa
//  gettimeofday(), e nao time() + millis() % 1000 - millis() mede o tempo desde
//  o boot e nao corresponde necessariamente a fracao de segundo do relogio.
// ============================================================================
namespace {

bool g_started = false;
bool g_synced = false;
uint32_t g_lastCheckMs = 0;

// Le o epoch do relogio do sistema com a fracao de segundo do mesmo contador.
// Devolve false enquanto o SNTP nao ajustou o relogio.
bool readEpoch(uint32_t &epochSeconds, uint32_t &epochMillis) {
  struct timeval tv = {};
  if (gettimeofday(&tv, nullptr) != 0) {
    return false;
  }

  int64_t seconds = static_cast<int64_t>(tv.tv_sec);
  int64_t usec = static_cast<int64_t>(tv.tv_usec);

  // Normalizacao defensiva, caso tv_usec venha fora de [0, 1000000).
  if (usec < 0) {
    usec += 1000000;
    seconds -= 1;
  }
  seconds += usec / 1000000;
  usec %= 1000000;

  if (seconds < static_cast<int64_t>(NTP_MIN_VALID_EPOCH_SECONDS)) {
    return false;
  }

  epochSeconds = static_cast<uint32_t>(seconds);
  epochMillis = static_cast<uint32_t>(usec / 1000);
  return true;
}

}  // namespace

void device_clock::begin(uint32_t nowMs) {
  g_started = false;
  g_synced = false;
  g_lastCheckMs = nowMs;
}

void device_clock::update(uint32_t nowMs, bool wifiConnected) {
  if (g_synced) {
    return;
  }

  if (!wifiConnected) {
    // Reconfigura o SNTP quando o Wi-Fi voltar.
    g_started = false;
    return;
  }

  if (!g_started) {
    configTime(UTC_OFFSET_SECONDS, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
    g_started = true;
    g_lastCheckMs = nowMs;
    Serial.println("Relogio: sincronizacao NTP iniciada.");
    return;
  }

  if ((nowMs - g_lastCheckMs) < 1000UL) {
    return;
  }
  g_lastCheckMs = nowMs;

  uint32_t epochSeconds = 0;
  uint32_t epochMillis = 0;
  if (readEpoch(epochSeconds, epochMillis)) {
    g_synced = true;
    Serial.printf("Relogio: sincronizado (epoch=%lu.%03lu)\n",
                  static_cast<unsigned long>(epochSeconds),
                  static_cast<unsigned long>(epochMillis));
  }
}

bool device_clock::synced() { return g_synced; }

uint64_t device_clock::nowMs() {
  if (!g_synced) {
    return 0;
  }

  // Segundos e milissegundos vem do mesmo relogio (gettimeofday).
  uint32_t epochSeconds = 0;
  uint32_t epochMillis = 0;
  if (!readEpoch(epochSeconds, epochMillis)) {
    return 0;
  }
  return static_cast<uint64_t>(epochSeconds) * 1000ULL + epochMillis;
}
