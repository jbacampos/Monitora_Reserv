#include <Arduino.h>

#include "alert_monitor.h"
#include "clock.h"
#include "config.h"
#include "credentials.h"
#include "display.h"
#include "history.h"
#include "moving_average.h"
#include "sensor.h"
#include "simulator.h"
#include "stall_guard.h"
#include "telegram.h"
#include "thingsboard.h"
#include "uptime.h"
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
History g_history;
Telegram g_telegram;
AlertMonitor g_alerts;

// Uptime acumulado (imune ao rollover de millis()) e a ultima medicao.
UptimeCounter g_uptime;

volume::Measurement g_lastMeasurement;
float g_filteredLiters = 0.0f;
bool g_hasSample = false;
uint32_t g_lastSampleMs = 0;
uint64_t g_lastSampleTimestampMs = 0;
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

  // Timestamp ORIGINAL da medicao (0 enquanto o NTP nao sincroniza).
  g_lastSampleTimestampMs = device_clock::nowMs();
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

  // `mqtt` mostra tambem o RX de emergencia quando ativo (off(rx4096)): sem isso
  // uma sessao recusada por registro grande pareceria apenas "broker fora".
  char mqttState[12];
  snprintf(mqttState, sizeof(mqttState), "%s%s",
           g_tb.connected() ? "ok" : "off",
           g_tb.rxEscalated() ? "(rx4096)" : "");

  Serial.printf(" | wifi=%s | mqtt=%s | hora=%s | publicadas=%lu | fila=%lu",
                g_wifi.connected() ? "ok" : "off", mqttState,
                device_clock::synced() ? "ok" : "off",
                static_cast<unsigned long>(g_tb.publishCount()),
                static_cast<unsigned long>(g_tb.queuePending()));

  Serial.printf(" | hist=%s",
                !g_history.ready() ? "off"
                                   : (g_history.degraded() ? "degradado" : "ok"));

  // Heap livre/bloco contiguo: e o insumo do roteiro de teste do Telegram
  // (antes e depois das consultas). `tg` resume o modulo sem expor credenciais:
  // off / adiado(<bloco>B) / aguardando / ultima=<s>s (tempo desde o INICIO da
  // ultima consulta). Antes "ok" significava apenas "ja houve uma consulta",
  // o que escondia um gate de heap fechado.
  char tgState[24];
  g_telegram.stateText(tgState, sizeof(tgState), nowMs);
  Serial.printf(" | heap=%u/%u | tg=%s",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxFreeBlockSize()), tgState);

  // Alertas de nivel: estado e quantos ainda esperam envio. Sem isso, um alerta
  // parado na fila (heap fechado) ficaria invisivel no log.
  Serial.printf(" | alertas=%s(%u)", g_alerts.levelText(),
                static_cast<unsigned>(g_alerts.pendingCount()));

  Serial.println();
}

void printStartup() {
  Serial.println();
  Serial.println(F("=== MoniRes - Etapa 2 ==="));
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

  Serial.println(
      "Fila offline: LittleFS (segmentos com CRC32, replay cronologico)");
  Serial.println("Telemetria: volume_l + ts (epoch ms) | uptime_s: atributo");
  Serial.printf("Historico: %u dias | snapshot a cada %lu min | amostragem %lu s\n",
                static_cast<unsigned>(history_record::SLOTS),
                static_cast<unsigned long>(HIST_SNAPSHOT_INTERVAL_MS / 60000UL),
                static_cast<unsigned long>(HIST_SAMPLE_INTERVAL_MS / 1000UL));

  // Alertas: os defaults ficam em config.h; os limites EFETIVOS podem ter sido
  // trocados por /w e sao carregados do LittleFS em g_alerts.begin().
  Serial.printf("Alertas: risco %u%% | critico %u%% | histerese %u%% | "
                "ref. 100%% = %lu L | settle %lu s\n",
                static_cast<unsigned>(ALERT_DEFAULT_RISK_PCT),
                static_cast<unsigned>(ALERT_DEFAULT_CRITICAL_PCT),
                static_cast<unsigned>(ALERT_DEFAULT_HIST_PCT),
                static_cast<unsigned long>(volume::maxMeasurableLiters() + 0.5f),
                static_cast<unsigned long>(ALERT_SETTLE_MS / 1000UL));

  Serial.printf(
      "Guarda: TG %lu min | MQTT %lu min | aviso %lu min | carencia %lu s | "
      "renovacao TG %lu min\n",
      static_cast<unsigned long>(TELEGRAM_STALL_REBOOT_MS / 60000UL),
      static_cast<unsigned long>(MQTT_STALL_REBOOT_MS / 60000UL),
      static_cast<unsigned long>(STALL_GUARD_FIRST_WARN_MS / 60000UL),
      static_cast<unsigned long>(STALL_GUARD_GRACE_MS / 1000UL),
      static_cast<unsigned long>(TELEGRAM_SESSION_MAX_AGE_MS / 60000UL));
}

}  // namespace

void setup() {
  Serial.begin(SERIAL_BAUD);
  printStartup();

  const uint32_t now = millis();

  // O uptime passa a ser acumulado a partir daqui (nao usa millis() direto).
  g_uptime.begin(now);

  sensor::begin(now);
  g_display.begin(now);
  g_wifi.begin(now);
  device_clock::begin(now);
  g_tb.begin(now);  // monta o LittleFS (sem formatacao automatica)

  // Historico depois do ThingsBoard: reutiliza o LittleFS ja montado pela fila.
  g_history.begin(now);

  // Alertas de nivel: mesmos LittleFS e mesmo volume filtrado do resto.
  g_alerts.begin(now);

  // Telegram: apenas estado (nenhuma conexao no setup). A primeira consulta a
  // Bot API acontece TELEGRAM_POLL_INTERVAL_MS depois do boot.
  g_telegram.begin(now);

  // Guarda de bloqueio: nenhum reboot nos primeiros STALL_GUARD_GRACE_MS.
  stall_guard::begin(now);

  g_lastSampleMs = now;

  // Primeira amostra e primeiro diagnostico imediatos, sem esperar os
  // intervalos de 250 ms e de DIAGNOSTIC_INTERVAL_MS.
  sampleAndProcess(now);
  printDiagnostics(now);
  g_lastDiagnosticMs = now;
}

void loop() {
  const uint32_t now = millis();

  // Acumula o uptime em toda iteracao: o delta e wrap-safe, portanto o valor
  // continua correto depois do rollover de millis() e dentro das consultas do
  // Telegram que bloqueiam alguns segundos.
  g_uptime.update(now);

  // Aquisicao a cada SAMPLE_INTERVAL_MS.
  if ((now - g_lastSampleMs) >= SAMPLE_INTERVAL_MS) {
    g_lastSampleMs = now;
    sampleAndProcess(now);
  }

  // Refresh, desligamento e botao do display (temporizacao interna propria).
  g_display.update(now);

  // Wi-Fi: reconexao agendada, sem bloquear a medicao nem o display.
  g_wifi.update(now);

  // Relogio: NTP inicia quando o Wi-Fi conecta; sem hora valida nao ha
  // telemetria nem fila (o timestamp original e obrigatorio).
  device_clock::update(now, g_wifi.connected());

  // ThingsBoard: recebe os valores ja calculados e decide quando publicar.
  TelemetrySample sample;
  sample.volumeL = g_filteredLiters;
  sample.timestampMs = g_lastSampleTimestampMs;
  sample.uptimeS = g_uptime.seconds();
  sample.valid = g_hasSample;
  g_tb.update(now, sample);

  // Historico diario: auxiliar e independente da comunicacao. Usa o mesmo
  // timestamp da medicao que a telemetria recebeu.
  g_history.update(now, device_clock::synced(), g_lastSampleTimestampMs,
                   g_filteredLiters, g_hasSample);

  // Alertas de nivel: avalia o volume FILTRADO e enfileira no maximo um evento
  // por passo. Vem ANTES do Telegram para o alerta sair na mesma consulta.
  g_alerts.update(now, g_filteredLiters, g_hasSample);

  // Telegram: primeira consulta a Bot API TELEGRAM_POLL_INTERVAL_MS depois do
  // boot. Recebe o MESMO volume enviado ao ThingsBoard, o uptime acumulado
  // (imune ao rollover de millis()) e o historico (unica fonte dos agregados).
  // A consulta e limitada em tempo e nao altera nenhum outro modulo.
  TelegramContext telegramContext;
  telegramContext.volumeL = g_filteredLiters;
  telegramContext.uptimeS = g_uptime.seconds();
  telegramContext.sampleValid = g_hasSample;
  telegramContext.history = &g_history;
  telegramContext.alerts = &g_alerts;
  g_telegram.update(now, telegramContext);

  // Diagnostico em frequencia propria, independente da aquisicao.
  if ((now - g_lastDiagnosticMs) >= DIAGNOSTIC_INTERVAL_MS) {
    g_lastDiagnosticMs = now;
    printDiagnostics(now);
  }

  // Guarda de bloqueio: Telegram ou MQTT sem sucesso por tempo demais nao se
  // recuperam sozinhos (heap fragmentado / broker recusando a sessao). O reboot
  // e a ULTIMA saida - so com um servico bloqueado E o heap fragmentado sem
  // volta - e devolve o heap inteiro. Sem Wi-Fi, nada e contado.
  stall_guard::Input guardInput;
  guardInput.wifiConnected = g_wifi.connected();
  guardInput.telegramEnabled = g_telegram.enabled();
  guardInput.mqttEnabled = !tbTokenPending();
  guardInput.telegramLastOkMs = g_telegram.lastSuccessMs();
  // Sessao MQTT de pe conta como viva (o PubSubClient derruba o socket quando ela
  // morre): sem isso uma conexao longa e saudavel, que nunca reconecta, parecia
  // "parada" e reiniciava o ESP a cada 30 min. Sem sessao, o silencio e medido
  // desde o ultimo aceite ou a ultima publicacao confirmada.
  guardInput.mqttLastOkMs = g_tb.connected() ? millis() : g_tb.lastActivityMs();
  // Maior bloco contiguo para o criterio de fragmentacao inadministravel.
  guardInput.maxFreeBlockBytes = ESP.getMaxFreeBlockSize();
  // Carimbo NOVO: os modulos acima podem ter bloqueado (a consulta do Telegram
  // segura ~1,4 s) e marcar millis() maior que o `now` do inicio do loop. Com o
  // `now` defasado a conta do silencio ficava negativa e a guarda reiniciava o
  // ESP logo depois do primeiro /help (medido em campo em 2026-11-20).
  stall_guard::update(millis(), guardInput);

  // Espaco livre para os proximos modulos assincronos: Telegram, alertas, OTA
  // e persistencia de configuracoes.
}
