#pragma once

#include <Arduino.h>

// ============================================================================
//  Relogio do dispositivo (NTP)
//
//  O timestamp da medicao e Unix epoch em milissegundos, lido de
//  gettimeofday(): segundos e microssegundos vem do mesmo relogio do sistema,
//  ja ajustado pelo SNTP. Enquanto o NTP nao sincroniza, nowMs() devolve 0 e a
//  telemetria nao e publicada nem enfileirada (mesma regra do Ecowitt2TB, que
//  exige `collectedAt != 0`).
// ============================================================================
namespace device_clock {

void begin(uint32_t nowMs);

// Deve ser chamado continuamente pelo loop. Dispara o SNTP na primeira vez em
// que o Wi-Fi esta conectado e verifica o sincronismo uma vez por segundo.
void update(uint32_t nowMs, bool wifiConnected);

bool synced();

// Epoch em milissegundos, ou 0 se o relogio ainda nao foi sincronizado.
uint64_t nowMs();

}  // namespace device_clock
