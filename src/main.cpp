#include <Arduino.h>

#include "config.h"
#include "display.h"
#include "moving_average.h"
#include "sensor.h"
#include "simulator.h"
#include "thingsboard.h"
#include "volume.h"
#include "wifi_manager.h"

// ============================================================================
//  Aplicacao
//
//  O loop principal nao usa delay(): aquisicao, display, botao, Wi-Fi,
//  ThingsBoard e diagnostico sao agendados por millis(), deixando o loop livre
//  para os proximos modulos (Telegram, alertas, OTA, persistencia).
//
//  A medicao e o display sao independentes da comunicacao: se Wi-Fi ou MQTT
//  estiverem indisponiveis, tudo continua funcionando e a publicacao retoma
//  sozinha na reconexao.
// ============================================================================
namespace {

MovingAverage g_volumeFilter(MOVING_AVERAGE_WINDOW);
DisplayManager g_display;
WiFiManager g_wifi;
ThingsBoard g_tb;

volume::Measurement g_lastMeasurement;
float g_filteredLiters = 0.0f;
bool g_hasSample = false;
uint32_t g_lastSampleMs = 0;
uint32_t g_lastDiagnosticMs = 0;

static_assert(MOVING_AVERAGE_WINDOW <= MovingAverage::kMaxWindow,
              "MOVING_AVERAGE_WINDOW excede a capacidade do filtro");

void sampleAndProcess(uint32_t nowMs) {
  const uint16_t adc = sensor::read(nowMs);

  g_lastMeasurement = volume::compute(adc);
  g_volumeFilter.add(g_lastMeasurement.liters);
  g_filteredLiters = g_volumeFilter.average();

  // O display recebe o volume filtrado; a escrita fisica e temporizada.
  g_display.setLiters(g_filteredLiters);
  g_hasSample = true;
}

void printDiagnostics(uint32_t nowMs) {
  const volume::Measurement& m = g_lastMeasurement;

  Serial.printf(
      "[%7.2f s] adc=%u | tensao=%.3f V | leitura=%.1f mm | altura=%.1f mm | "
      "vol_inst=%.1f L | vol_filt=%.1f L",
      static_cast<double>(nowMs) / 1000.0, static_cast<unsigned>(m.adc),
      static_cast<double>(m.voltageV), static_cast<double>(m.sensorMm),
      static_cast<double>(m.levelMm), static_cast<double>(m.liters),
      static_cast<double>(g_filteredLiters));

  if constexpr (TEST_MODE) {
    Serial.printf(" | cenario=%s | amostras=%u/%u",
                  Simulator::scenarioName(SIM_SCENARIO),
                  static_cast<unsigned>(g_volumeFilter.sampleCount()),
                  static_cast<unsigned>(MOVING_AVERAGE_WINDOW));
  }

  Serial.printf(" | wifi=%s | mqtt=%s | publicadas=%lu",
                g_wifi.connected() ? "ok" : "off",
                g_tb.connected() ? "ok" : "off",
                static_cast<unsigned long>(g_tb.publishCount()));

  Serial.println();
}

void printStartup() {
  Serial.println();
  Serial.println(F("=== Monitora_Reserv - Etapa 2 ==="));
  Serial.printf("Modo: %s\n", TEST_MODE ? "TESTE (simulador + credenciais de teste)"
                                        : "PRODUCAO (analogRead A0 + credenciais de producao)");

  if constexpr (TEST_MODE) {
    Serial.printf("Cenario: %s | SIM_NOISE=%s | SIM_TIME_SCALE=%.1fx\n",
                  Simulator::scenarioName(SIM_SCENARIO), SIM_NOISE ? "on" : "off",
                  static_cast<double>(SIM_TIME_SCALE));
  }

  Serial.printf(
      "Amostragem: %lu ms | display: %lu ms | desliga: %lu ms | diag: %lu ms\n",
      static_cast<unsigned long>(SAMPLE_INTERVAL_MS),
      static_cast<unsigned long>(DISPLAY_REFRESH_MS),
      static_cast<unsigned long>(DISPLAY_ON_TIME_MS),
      static_cast<unsigned long>(DIAGNOSTIC_INTERVAL_MS));

  Serial.printf(
      "Publicacao: delta >= %.1f L | heartbeat: %lu min | intervalo minimo: %lu s\n",
      static_cast<double>(TB_PUBLISH_DELTA_LITERS),
      static_cast<unsigned long>(TB_HEARTBEAT_INTERVAL_MS / 60000UL),
      static_cast<unsigned long>(TB_MIN_PUBLISH_INTERVAL_MS / 1000UL));
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  printStartup();

  const uint32_t now = millis();

  sensor::begin(now);
  g_display.begin(now);
  g_wifi.begin(now);
  g_tb.begin(now);

  g_lastSampleMs = now;

  // Primeira amostra e primeiro diagnostico imediatos, sem esperar os
  // intervalos de 250 ms e de DIAGNOSTIC_INTERVAL_MS.
  sampleAndProcess(now);
  printDiagnostics(now);
  g_lastDiagnosticMs = now;
}

void loop() {
  const uint32_t now = millis();

  // Aquisicao a cada SAMPLE_INTERVAL_MS.
  if ((now - g_lastSampleMs) >= SAMPLE_INTERVAL_MS) {
    g_lastSampleMs = now;
    sampleAndProcess(now);
  }

  // Refresh, desligamento e botao do display (temporizacao interna propria).
  g_display.update(now);

  // Wi-Fi: reconexao agendada, sem bloquear a medicao nem o display.
  g_wifi.update(now);

  // ThingsBoard: recebe os valores ja calculados e decide quando publicar.
  TelemetrySample sample;
  sample.volumeL = g_filteredLiters;
  sample.levelMm = g_lastMeasurement.levelMm;
  sample.uptimeS = now / 1000UL;
  sample.valid = g_hasSample;
  g_tb.update(now, sample);

  // Diagnostico em frequencia propria, independente da aquisicao.
  if ((now - g_lastDiagnosticMs) >= DIAGNOSTIC_INTERVAL_MS) {
    g_lastDiagnosticMs = now;
    printDiagnostics(now);
  }

  // Espaco livre para os proximos modulos assincronos: Telegram, alertas, OTA
  // e persistencia de configuracoes.
}
