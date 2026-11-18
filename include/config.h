#pragma once

// ============================================================================
//  Monitora_Reserv - configuracao do firmware
//
//  Ponto central de configuracao. Nao existe outro lugar no projeto que
//  escolha entre teste e producao (ver TEST_MODE abaixo).
// ============================================================================

#include <Arduino.h>

// ---------------------------------------------------------------------------
//  Modo de teste - a UNICA chave de selecao de ambiente
//
//  Alterar SOMENTE este valor seleciona, em um unico ponto, quais credenciais e
//  destinos toda a comunicacao usa:
//    true  -> ambiente de TESTE (simulador de sensor + credenciais de teste)
//    false -> PRODUCAO (analogRead(A0) + credenciais de producao)
//
//  Nenhum outro arquivo (nem os ambientes do platformio.ini) define modo:
//  sensor, Wi-Fi, ThingsBoard e, futuramente, Telegram e OTA derivam tudo
//  daqui, por if constexpr ou por funcoes constexpr (ver credentials.h).
// ---------------------------------------------------------------------------
constexpr bool TEST_MODE = true;  // true = test, false = production

// ---------------------------------------------------------------------------
//  Hardware (NodeMCU ESP8266 / ESP-12E)
// ---------------------------------------------------------------------------
constexpr uint8_t PIN_SENSOR_ADC  = A0;   // QDY30A
constexpr uint8_t PIN_DISPLAY_CLK = D1;   // TM1637 CLK
constexpr uint8_t PIN_DISPLAY_DIO = D2;   // TM1637 DIO
constexpr uint8_t PIN_BUTTON      = D7;   // botao de religamento do display

// Logica eletrica do botao, preservada do programa original:
//     if (digitalRead(PinButton) == HIGH) { ... }
// O original nao configurava o pino (sem INPUT_PULLUP) e considerava o nivel
// ALTO como botao acionado, ou seja, o circuito externo mantem D7 em LOW em
// repouso e leva o pino a 3,3 V quando o botao e pressionado.
constexpr uint8_t BUTTON_ACTIVE_LEVEL = HIGH;

// O original nao habilitava pull-up interno. Mantenha false para nao inverter
// a polaridade do circuito; habilite somente se o pino ficar flutuando e a
// leitura de repouso nao for confiavel.
constexpr bool BUTTON_USE_INTERNAL_PULLUP = false;

// ---------------------------------------------------------------------------
//  Temporizacao (todo o controle e feito com millis(), sem delay())
// ---------------------------------------------------------------------------
constexpr uint32_t SAMPLE_INTERVAL_MS     = 250;    // aquisicao do sensor
constexpr uint32_t DISPLAY_REFRESH_MS     = 4000;   // atualizacao do display
constexpr uint32_t DISPLAY_ON_TIME_MS     = 28000;  // desligamento automatico
constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 5000;   // periodicidade do Serial
constexpr uint32_t BUTTON_DEBOUNCE_MS     = 30;     // debounce do botao

// ---------------------------------------------------------------------------
//  Filtro e display
// ---------------------------------------------------------------------------
constexpr size_t  MOVING_AVERAGE_WINDOW = 50;  // 50 leituras (12,5 s a 250 ms)
constexpr uint32_t SERIAL_BAUD          = 115200;

// Brilho 0..7 conforme a biblioteca. O programa original usava 0 (minimo).
constexpr uint8_t DISPLAY_BRIGHTNESS = 0;
constexpr uint8_t DISPLAY_DIGITS     = 6;  // TM1637TinyDisplay6

// Mensagem exibida no boot, como no programa original (display.showString).
constexpr char DISPLAY_BOOT_TEXT[] = "Inicio";

// Formato do volume, preservado do programa original:
//   volume <  1000 L -> sem separador (ex.: "830")
//   volume >= 1000 L -> ponto no 3o digito fisico, que atua como separador de
//                       milhar (ex.: "1.234", "12.345", "123.456")
constexpr long    DISPLAY_DOT_MIN_LITERS = 1000;        // limite do separador
constexpr uint8_t DISPLAY_THOUSANDS_DOT  = 0b00100000;  // ponto apos o 3o digito
constexpr long    DISPLAY_MAX_VALUE      = 999999;      // limite dos 6 digitos

// ---------------------------------------------------------------------------
//  Simulador (usado somente no modo TESTE)
// ---------------------------------------------------------------------------
enum class SimScenario : uint8_t {
  CONSTANT,    // nivel fixo
  FILLING,     // enchimento linear
  DRAINING,    // esvaziamento linear
  OSCILLATING, // oscilacao senoidal
  CRITICAL,    // nivel abaixo do offset do sensor (volume nulo)
  RECOVERY,    // recuperacao a partir de nivel critico
};

// Cenario ativo. Troque aqui para testar os demais comportamentos.
constexpr SimScenario SIM_SCENARIO = SimScenario::FILLING;

constexpr bool  SIM_NOISE     = true;   // ruido artificial nas amostras ADC
constexpr float SIM_TIME_SCALE = 20.0f; // 20x mais rapido que o tempo real

constexpr uint32_t SIM_SCENARIO_DURATION_MS = 120000; // rampas/recuperacao
constexpr float    SIM_NOISE_AMPLITUDE_COUNTS = 3.0f; // +/- 3 contagens ADC

constexpr float SIM_CONSTANT_LEVEL_MM      = 1500.0f;
constexpr float SIM_FILLING_START_MM       = 300.0f;
constexpr float SIM_FILLING_END_MM         = 2500.0f;
constexpr float SIM_DRAINING_START_MM      = 2500.0f;
constexpr float SIM_DRAINING_END_MM        = 200.0f;
constexpr float SIM_OSCILLATING_CENTER_MM  = 1500.0f;
constexpr float SIM_OSCILLATING_AMPLITUDE_MM = 400.0f;
constexpr float SIM_OSCILLATING_PERIOD_MS    = 60000.0f;
// Nivel igual a zero: a leitura fica abaixo do offset de 50 mm e a calibracao
// deve limitar nivel e volume a zero (exercita o clamp de segurança).
constexpr float SIM_CRITICAL_LEVEL_MM        = 0.0f;
constexpr float SIM_RECOVERY_START_MM      = 200.0f;
constexpr float SIM_RECOVERY_END_MM        = 2200.0f;

static_assert(SIM_TIME_SCALE > 0.0f, "SIM_TIME_SCALE deve ser maior que zero");
static_assert(SAMPLE_INTERVAL_MS > 0, "SAMPLE_INTERVAL_MS deve ser maior que zero");

// ---------------------------------------------------------------------------
//  Comunicacao (Wi-Fi + ThingsBoard)
//
//  Os parametros abaixo valem para os dois ambientes. O que muda entre TESTE e
//  PRODUCAO sao apenas as credenciais e o destino (credentials.h), sempre
//  derivados de TEST_MODE.
// ---------------------------------------------------------------------------

// Identificacao usada no clientId do MQTT (facilita identificar a sessao no
// ThingsBoard e nos logs do broker).
constexpr char DEVICE_ID[] = "MonRes-01";

// Wi-Fi: intervalo entre tentativas de reconexao (nao bloqueante).
constexpr uint32_t WIFI_RETRY_MS = 10000;

// MQTT: intervalo entre tentativas de conexao e timeouts. Os timeouts limitam
// quanto tempo UMA tentativa pode segurar o loop:
//   - MQTT_SOCKET_TIMEOUT_MS: DNS + conexao TCP (WiFiClient);
//   - MQTT_CONNACK_TIMEOUT_S: espera do CONNACK (PubSubClient, em segundos).
// Com os valores abaixo, uma tentativa contra um broker fora do ar custa no
// maximo ~1,8 s, e nao a cada ciclo: falhas seguidas aplicam backoff crescente
// ate MQTT_MAX_RECONNECT_MS.
constexpr uint32_t MQTT_RECONNECT_MS      = 5000;
constexpr uint32_t MQTT_MAX_RECONNECT_MS  = 60000;
constexpr uint32_t MQTT_SOCKET_TIMEOUT_MS = 800;
constexpr uint16_t MQTT_CONNACK_TIMEOUT_S = 1;
constexpr uint16_t MQTT_BUFFER_SIZE       = 256;

// Topico de telemetria do ThingsBoard (autenticacao por device access token).
constexpr char TB_TELEMETRY_TOPIC[] = "v1/devices/me/telemetry";

// Chaves publicadas na telemetria.
constexpr char TB_KEY_VOLUME[] = "volume_l";
constexpr char TB_KEY_LEVEL[]  = "nivel_mm";
constexpr char TB_KEY_UPTIME[] = "uptime";

// Politica de publicacao:
//  - diferenca minima, em litros, para publicar por mudanca significativa;
//  - heartbeat periodico (1 hora), mesmo sem mudanca;
//  - intervalo minimo entre publicacoes por mudanca, para nao inundar o broker
//    enquanto o filtro esta subindo ou descendo rapido.
constexpr float    TB_PUBLISH_DELTA_LITERS    = 50.0f;
constexpr uint32_t TB_HEARTBEAT_INTERVAL_MS   = 60UL * 60UL * 1000UL;
constexpr uint32_t TB_MIN_PUBLISH_INTERVAL_MS = 10000;

static_assert(TB_PUBLISH_DELTA_LITERS > 0.0f,
              "TB_PUBLISH_DELTA_LITERS deve ser maior que zero");
static_assert(TB_HEARTBEAT_INTERVAL_MS > TB_MIN_PUBLISH_INTERVAL_MS,
              "o heartbeat deve ser maior que o intervalo minimo de publicacao");
static_assert(MQTT_SOCKET_TIMEOUT_MS < WIFI_RETRY_MS,
              "o timeout do socket deve ser menor que o intervalo de reconexao");

