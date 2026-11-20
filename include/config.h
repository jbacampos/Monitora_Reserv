#pragma once

// ============================================================================
//  Monitora_Reserv - configuracao do firmware
//
//  Ponto central de configuracao. Nao existe outro lugar no projeto que
//  escolha entre teste e producao (ver TEST_MODE abaixo).
// ============================================================================

#include <Arduino.h>

#include "volume.h"

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
//  Versao do firmware - definicao UNICA
//
//  Usada pelo /status do Telegram (e disponivel para OTA/telemetria no futuro).
//  Nao existe outra copia da versao no projeto.
// ---------------------------------------------------------------------------
constexpr char FIRMWARE_VERSION[] = "1.0.0";

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
constexpr uint32_t DISPLAY_REFRESH_MS     = 15000;  // atualizacao do display
constexpr uint32_t DISPLAY_ON_TIME_MS     = 28000;  // desligamento automatico
constexpr uint32_t DIAGNOSTIC_INTERVAL_MS = 5000;   // periodicidade do Serial
constexpr uint32_t BUTTON_DEBOUNCE_MS     = 30;     // debounce do botao

// A atualizacao precisa caber na janela em que o display esta ligado: um refresh
// maior que o tempo ligado deixaria o valor congelado na tela.
static_assert(DISPLAY_REFRESH_MS < DISPLAY_ON_TIME_MS,
              "o intervalo de atualizacao do display deve caber no tempo ligado");

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
  CYCLE,       // esvazia, enche e recomeca (exercita os QUATRO alertas)
};

// Cenario ativo. Troque aqui para testar os demais comportamentos.
constexpr SimScenario SIM_SCENARIO = SimScenario::CYCLE;

constexpr bool  SIM_NOISE     = true;   // ruido artificial nas amostras ADC
constexpr float SIM_TIME_SCALE = 1.0f; // 1x: rampas de 120 s reais

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

// CYCLE: esvazia de SIM_DRAINING_START_MM ate SIM_DRAINING_END_MM em
// SIM_CYCLE_DRAIN_MS, enche de volta em SIM_CYCLE_FILL_MS e recomeca, para
// sempre. Com os limites de fabrica (risco 60%, critico 30%, histerese 5%)
// um ciclo de 240 s percorre, nesta ordem: atingiu o risco (~41 s), atingiu o
// critico (~84 s), recuperou-se do critico (~163 s) e recuperou-se do risco
// (~206 s) - os quatro avisos, mais as histereses de um lado so. O
// OSCILLATING, em contraste, oscila em volta de 1500 mm e so cruza o risco.
constexpr uint32_t SIM_CYCLE_DRAIN_MS = 120000UL;
constexpr uint32_t SIM_CYCLE_FILL_MS  = 120000UL;

static_assert(SIM_CYCLE_DRAIN_MS > 0 && SIM_CYCLE_FILL_MS > 0,
              "as duas pernas do CYCLE precisam ter duracao");
static_assert(SIM_DRAINING_END_MM < SIM_DRAINING_START_MM,
              "o CYCLE espera uma rampa de descida e a mesma de volta");

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

// Buffers TLS do BearSSL, em bytes de DADOS (o core soma 325 B ao RX e 85 B ao
// TX para o overhead do protocolo antes de alocar). O padrao do core e
// (16384, 512), que reserva ~17 KB de heap enquanto a sessao TLS existe; os
// valores abaixo reservam ~5 KB.
//
// Dimensionamento medido no broker atual (mqtt.datalog.top:8883, TLS 1.2, o
// mesmo protocolo maximo do BearSSL):
//   - maior registro RECEBIDO: 1735 B (Certificate: folha de 1213 B + entrada
//     vazia). A CA nao trafega: ela e ancora local (datalog_mqtt_ca.h);
//   - ClientHello ENVIADO: ~211 B (o do BearSSL e da mesma ordem de grandeza);
//   - maior PUBLISH do firmware: ~100 B (payload de ~70 B em g_payload[192]).
// RX = 3072 (3397 B alocados com os 325 B de overhead): 1,96x de margem sobre o
// maior registro medido (1735 B) e 1 KB de heap PERMANENTE devolvido ao sistema.
// O motivo e a reconexao do MQTT enquanto a sessao TLS do Telegram esta aberta:
// com o broker fora, so os buffers do MQTT voltam (~8,4 KB), o heap fica em
// ~13,9 KB e o pico do handshake do MQTT e 3408 (ctx) + 3397 (rx) + 597 (tx) +
// 342 (socket) + 3064 (validador X.509) = 10 808 B, deixando ~3,1 KB de folga -
// acima dos TELEGRAM_PEAK_HEADROOM (2 248 B) que o projeto definiu como seguro.
// Com 4096 o mesmo pico daria 11 832 B e a folga cairia para ~2,0 KB, abaixo do
// limiar (medido em campo em 2026-11-20: heap 5 464 B com as duas sessoes vivas).
// Se o broker passar a enviar um registro maior que 3397 B (ex.: cadeia com uma
// intermediaria a mais), o BearSSL falha com BR_ERR_TOO_LARGE (codigo 6) e a
// unica saida e recompilar com 4096. 512 de TX cobrem o ClientHello e qualquer
// PUBLICACAO; o buffer do PubSubClient (MQTT_BUFFER_SIZE) nao depende disto.
constexpr int MQTT_TLS_RX_BUFFER = 3072;
constexpr int MQTT_TLS_TX_BUFFER = 512;

// RX de EMERGENCIA, usado SOZINHO quando o broker muda a cadeia e o registro
// deixa de caber no buffer normal: 4096 + 325 = 4421 B alocados (2,55x de
// margem sobre os 1735 B medidos). O firmware troca para este valor quando o
// BearSSL reporta BR_ERR_TOO_LARGE (codigo 6) e volta ao normal se a alocacao
// falhar por falta de heap (codigo -1000) - ver ThingsBoard::handleTlsFailure.
// Custo: 1 KB de heap enquanto o MQTT estiver ampliado (e um handshake de
// 11 832 B, que com a sessao do Telegram aberta deixa ~2,4 KB de folga: o mesmo
// piso que o projeto ja aceitava antes, acima dos 2 248 B medidos como seguros).
constexpr int MQTT_TLS_RX_BUFFER_FALLBACK = 4096;

// O core (WiFiClientSecureBearSSL) limita os dois valores a [512, 16384].
static_assert(MQTT_TLS_RX_BUFFER >= 512 && MQTT_TLS_RX_BUFFER <= 16384,
              "MQTT_TLS_RX_BUFFER deve estar em [512, 16384]");
// Acima de 3072 a folga do handshake cai abaixo do limiar documentado acima;
// subir para 4096 e a saida de EMERGENCIA se o broker mudar a cadeia (o valor
// continua valido, apenas consome 1 KB a mais de heap permanente).
static_assert(MQTT_TLS_RX_BUFFER_FALLBACK > MQTT_TLS_RX_BUFFER,
              "o RX de emergencia deve ser MAIOR que o normal");
static_assert(MQTT_TLS_RX_BUFFER_FALLBACK >= 2048 &&
                  MQTT_TLS_RX_BUFFER_FALLBACK <= 16384,
              "MQTT_TLS_RX_BUFFER_FALLBACK deve estar em [2048, 16384]");
static_assert(MQTT_TLS_TX_BUFFER >= 512 && MQTT_TLS_TX_BUFFER <= 16384,
              "MQTT_TLS_TX_BUFFER deve estar em [512, 16384]");
// O maior registro do broker medido tem ~1,8 KB: nao reduzir o RX abaixo disso.
static_assert(MQTT_TLS_RX_BUFFER >= 2048,
              "MQTT_TLS_RX_BUFFER deve cobrir o maior registro do broker (~1,8 KB)");

// ---------------------------------------------------------------------------
//  Relogio (NTP)
//
//  A fila offline guarda o timestamp ORIGINAL da medicao, entao a hora precisa
//  estar sincronizada antes de publicar ou enfileirar. Sem sincronismo a
//  telemetria apenas aguarda: nada e gravado com timestamp invalido.
// ---------------------------------------------------------------------------
constexpr char NTP_SERVER_1[] = "time.cloudflare.com";
constexpr char NTP_SERVER_2[] = "time.google.com";
constexpr char NTP_SERVER_3[] = "pool.ntp.org";

// Deslocamento usado apenas nos logs (horario de Brasilia). O epoch gravado na
// fila e sempre UTC.
constexpr int32_t UTC_OFFSET_SECONDS = -3 * 3600;

// Qualquer data a partir de 2024-01-01 indica que o NTP ja ajustou o relogio.
constexpr uint32_t NTP_MIN_VALID_EPOCH_SECONDS = 1704067200UL;

// ---------------------------------------------------------------------------
//  ThingsBoard - topicos e chaves
// ---------------------------------------------------------------------------

// Topico de telemetria do ThingsBoard (autenticacao por device access token).
// A telemetria leva apenas o timestamp original (`ts`) e o volume.
constexpr char TB_TELEMETRY_TOPIC[] = "v1/devices/me/telemetry";

// Atributos do dispositivo: `uptime_s` fica separado da telemetria e nunca
// entra na fila offline. E atualizado a cada publicacao de `volume_l` e na
// reconexao, sempre com o uptime atual - sem temporizador proprio.
constexpr char TB_ATTRIBUTES_TOPIC[] = "v1/devices/me/attributes";

// Chaves publicadas.
constexpr char TB_KEY_TS[]     = "ts";
constexpr char TB_KEY_VOLUME[] = "volume_l";
constexpr char TB_KEY_UPTIME[] = "uptime_s";

// Replay da fila offline: orcamento por iteracao do loop, igual ao Ecowitt2TB
// (mantem o loop responsivo e o display sem travamentos).
constexpr uint8_t  TB_QUEUE_REPLAY_MAX_RECORDS = 5;
constexpr uint32_t TB_QUEUE_REPLAY_BUDGET_MS   = 20;

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

// ---------------------------------------------------------------------------
//  Historico diario (recurso auxiliar)
//
//  O volume e ESTIMADO, nao medido: perder um dia ocasionalmente e aceitavel.
//  O historico nunca bloqueia sensor, display, MQTT, fila offline ou Telegram,
//  nunca formata o LittleFS e nao usa fila de recuperacao.
//
//  Retencao e layout binario ficam em history_record.h (30 slots, registro de
//  52 B, tabela de 1600 B); aqui ficam apenas temporizacao e limiares.
// ---------------------------------------------------------------------------
constexpr uint32_t HIST_SAMPLE_INTERVAL_MS   = 10000;   // amostragem nominal do historico
constexpr uint32_t HIST_SNAPSHOT_INTERVAL_MS = 300000;  // snapshot do dia corrente (5 min)
constexpr uint32_t HIST_RETRY_INTERVAL_MS    = 60000;   // nova tentativa de gravar o dia
constexpr uint32_t HIST_GAP_SECONDS          = 20;      // 2 acrescimos sem amostra

// Limiar de classificacao COMPLETO: amostragem e cobertura temporal do dia.
constexpr float HIST_COMPLETE_RATIO = 0.995f;

// Espaco livre minimo para iniciar uma gravacao, em blocos do LittleFS. E uma
// heuristica (nao uma garantia): cobre o bloco de dados + commit dos metadados
// e preserva a margem que a fila offline exige. Ajustar se o blockSize real
// (FSInfo::blockSize) nao for 4096 B.
constexpr uint8_t HIST_MIN_FREE_BLOCKS = 8;

static_assert(HIST_SAMPLE_INTERVAL_MS % 1000UL == 0,
              "HIST_SAMPLE_INTERVAL_MS deve ser multiplo de 1 s");
static_assert(HIST_GAP_SECONDS == 2 * (HIST_SAMPLE_INTERVAL_MS / 1000UL),
              "HIST_GAP_SECONDS deve ser o dobro do intervalo de amostragem");
static_assert(HIST_SNAPSHOT_INTERVAL_MS > HIST_SAMPLE_INTERVAL_MS,
              "o snapshot deve ser mais lento que a amostragem");
static_assert(HIST_COMPLETE_RATIO > 0.0f && HIST_COMPLETE_RATIO <= 1.0f,
              "HIST_COMPLETE_RATIO deve estar em (0, 1]");
static_assert(HIST_MIN_FREE_BLOCKS > 0, "HIST_MIN_FREE_BLOCKS deve ser positivo");

// ---------------------------------------------------------------------------
//  Telegram (Bot API, HTTPS direto no ESP8266, sem servico intermediario)
//
//  O modulo consulta getUpdates a cada TELEGRAM_POLL_INTERVAL_MS e responde
//  apenas aos chats autorizados (include/secrets.h). Nada aqui e bloqueante por
//  mais que TELEGRAM_RESPONSE_TIMEOUT_MS. A sessao TLS e REAPROVEITADA entre
//  consultas (ver TELEGRAM_PERSISTENT_SESSION) e renovada por idade; ver
//  TELEGRAM_SESSION_MAX_AGE_MS e a liberacao da sessao morta em telegram.cpp.
// ---------------------------------------------------------------------------
constexpr char TELEGRAM_HOST[]    = "api.telegram.org";
constexpr uint16_t TELEGRAM_PORT  = 443;

// Intervalo entre consultas (contado do INICIO da consulta anterior, o que
// garante que nunca existam duas consultas simultaneas).
constexpr uint32_t TELEGRAM_POLL_INTERVAL_MS = 20000UL;

// Atraso da PRIMEIRA consulta depois do boot (as seguintes respeitam
// TELEGRAM_POLL_INTERVAL_MS). A sessao TLS precisa nascer com o HEAP INTEIRO,
// ANTES de o MQTT reservar os ~3,4 KB do TLS dele: com o MQTT ja conectado o
// heap livre cai para ~12,8 KB e o gate (TELEGRAM_MIN_FREE_HEAP, 14,08 KB)
// NUNCA abre. Medido em campo em 2026-11-20: o bot ficou mudo, o aviso de boot
// e os alertas presos na fila, com `tg=adiado(12336B)` a cada 5 s no log.
// A consulta so sai quando Wi-Fi E relogio (NTP) estiverem prontos - ver o
// guarda de relogio em Telegram::update(). Depois de criada, a sessao e
// REAPROVEITADA e o gate deixa de ser consultado (sessionReady()).
constexpr uint32_t TELEGRAM_FIRST_POLL_DELAY_MS = 1000UL;
static_assert(TELEGRAM_FIRST_POLL_DELAY_MS < TELEGRAM_POLL_INTERVAL_MS,
              "a primeira consulta precisa vir antes do intervalo normal");

// Limites de tempo de UMA consulta, incluindo DNS, handshake TLS e leitura da
// resposta HTTP. O core do ESP8266 faz o DNS do connect com timeout fixo de
// 10 s; por isso o nome e resolvido ANTES com WiFi.hostByName(), limitado a
// TELEGRAM_DNS_TIMEOUT_MS (o resultado fica no cache do lwIP e o connect nao
// repete a consulta).
constexpr uint16_t TELEGRAM_DNS_TIMEOUT_MS      = 1500;
constexpr uint16_t TELEGRAM_SOCKET_TIMEOUT_MS   = 2500;
constexpr uint32_t TELEGRAM_RESPONSE_TIMEOUT_MS = 3000UL;

// Buffers TLS do Telegram, em bytes de DADOS (o core soma 325 B ao RX e 85 B ao
// TX antes de alocar). Dimensionamento medido em 2026-11-19 em api.telegram.org
// (TLS 1.2): a cadeia de certificados chega em UM registro de 3919 B (folha
// 1698 B + intermediaria 1236 B + raiz 969 B).
//
// RX = 4096 (4421 B alocados): cabe o registro de 3919 B com 177 B de folga e
// reduz em 1024 B o pico de DRAM do handshake, que e o que limita a operacao
// (ver TELEGRAM_MIN_FREE_HEAP). _alloc_iobuf() tenta IRAM ANTES da DRAM: se os
// 4421 B couberem em IRAM o pico de DRAM cai ~4,4 KB. O diagnostico mostra qual
// dos dois aconteceu (delta de "heap(pos-connect)").
//
// TX = 512 (597 B): cobre o ClientHello (~300 B) e o POST de uma resposta
// (~400 B); 512 e o menor valor aceito pelo core.
// Os buffers do MQTT (MQTT_TLS_RX_BUFFER/TX) NAO mudam.
constexpr int TELEGRAM_TLS_RX_BUFFER = 4096;
constexpr int TELEGRAM_TLS_TX_BUFFER = 512;

// Corpo HTTP maximo aceito. getUpdates com limit=1 devolve ~700 B (mensagem de
// texto) e sendMessage ~400 B; 2048 B e o teto que o leitor nunca ultrapassa
// (resposta maior e descartada como falha, sem truncar JSON).
constexpr size_t TELEGRAM_BODY_MAX = 2048;

// Maior bloco contiguo exigido para CRIAR a sessao TLS. Necessidade real medida
// em 2026-11-19: o core aloca, em sequencia, contexto do cliente (3408 B) +
// buffer RX (4421 B) + buffer TX (597 B) = 8 426 B (+ ~24 B de cabecalhos do
// alocador). O validador X.509 (3064 B) vem de outro bloco, e por isso o gate de
// heap LIVRE (abaixo) e igualmente obrigatorio. 9 216 B = 8 426 + ~790 de folga.
// Observacao de campo: com o MQTT conectado o bloco fica em ~14,2 KB; com o
// broker fora do ar fica em ~10 KB (por isso nao se usa 12 288 aqui).
constexpr size_t TELEGRAM_MIN_FREE_BLOCK = 9216;

// Piso de EMERGENCIA do maior bloco contiguo, usado APENAS para recriar uma
// sessao que acabou de morrer (ver TELEGRAM_GATE_RECOVERY_PERIOD_MS). O piso
// normal acima protege a PRIMEIRA sessao, quando o heap ainda esta inteiro.
//
// Quando a sessao cai (queda do socket, erro de HTTP ou simplesmente o servidor
// fechando a conexao ociosa), o bloco que ela ocupava volta para o heap como um
// buraco do tamanho exato da sessao. Medido em campo em 2026-11-20: buraco de
// 8 976 B com 14 656 B livres - ou seja, 240 B ABAIXO do piso normal e 550 B
// ACIMA da necessidade real (8 426 B de alocacao + ~24 B de cabecalhos). Sem
// este piso o gate (que so vale quando a sessao precisa ser CRIADA) ficava
// fechado para sempre: o log repetia `tg=adiado(8976B)` por horas e /help e
// /status nunca eram respondidos, embora o resto do firmware continuasse
// publicando. 8 704 B = 8 426 + ~278 B de folga.
constexpr size_t TELEGRAM_MIN_FREE_BLOCK_RECOVERY = 8704;

// Menor heap LIVRE TOTAL aceito para tentar uma consulta. O maior bloco sozinho
// NAO basta: medido em 2026-11-19, o handshake consumia 9792 B (contexto do
// cliente BearSSL + buffers RX=5120/TX + socket) MAIS 3064 B do validador X.509
// durante a validacao da cadeia (liberado antes de connect() retornar), ou seja
// ~12,9 KB no pico. Com 14696 B livres no inicio do poll o pico deixava ~1,8 KB
// para o lwIP/WiFi e o firmware morreu em memcpy(NULL) no contexto sys
// (exception 29, StoreProhibited).
//
// Pico REAL de DRAM durante o handshake, medido em 2026-11-19 com RX = 4096:
// delta de "heap(pos-connect)" = -8768 B (contexto 3408 + RX 4421 + TX 597 +
// socket ~342) MAIS o validador X.509 (3064 B), liberado antes de connect()
// retornar. O RX ficou em DRAM (nao em IRAM).
constexpr size_t TELEGRAM_HANDSHAKE_PEAK = 11832;

// Folga minima exigida no pico para o lwIP/WiFi. Medido: 1 550 B livres = crash
// (memcpy(NULL) no contexto sys), 1 848 B = handshake degradado, 2 040 B =
// sobreviveu. 2 248 B fica acima do pior caso que sobreviveu.
constexpr size_t TELEGRAM_PEAK_HEADROOM = 2248;

// Menor heap LIVRE TOTAL aceito para CRIAR a sessao TLS (= pico + folga).
// Reaproveitar a sessao aberta nao passa por este gate: nao aloca nada.
// 14080 B: o device opera com ~15,7 KB livres (MQTT conectado, RX=3072) e
// ~7,5 KB livres enquanto a sessao TLS do Telegram esta aberta (com o RX antigo
// de 4096 eram ~14,7 e ~5,5 KB - o KB devolvido pelo MQTT aparece aqui).
constexpr size_t TELEGRAM_MIN_FREE_HEAP =
    TELEGRAM_HANDSHAKE_PEAK + TELEGRAM_PEAK_HEADROOM;

// Sessao TLS entre consultas.
//   true  -> a sessao e criada UMA vez (no primeiro poll, com o heap inteiro) e
//            REAPROVEITADA nas seguintes: nenhuma alocacao por consulta, nenhum
//            handshake, nenhuma fragmentacao nova. Custo medido em 2026-11-19:
//            ~8,8 KB residentes (heap livre cai de ~16,3 para ~7,5 KB com o RX
//            do MQTT em 3072). O servidor fecha a conexao com ~1 h de idade, por
//            isso a renovacao preventiva (TELEGRAM_SESSION_MAX_AGE_MS) e a
//            liberacao da sessao morta acontecem antes do gate medir o heap.
//   false -> desenho antigo (cria e destroi a cada 20 s). Medido: cada consulta
//            deixa um buraco de ~8,5 KB (contexto+RX+TX) e o maior bloco cai
//            abaixo do piso, parando as consultas depois de 1-2 polls.
// Reversivel: basta voltar para false.
constexpr bool TELEGRAM_PERSISTENT_SESSION = true;

// DIAGNOSTICO - periodo minimo entre dois avisos de heap insuficiente.
// O adiamento NAO consome o intervalo de polling: o gate continua sendo
// reavaliado a cada loop(). Apenas a MENSAGEM e limitada, para o log nao
// repetir a mesma linha a cada iteracao do loop.
constexpr uint32_t TELEGRAM_HEAP_WARN_PERIOD_MS = 300000UL;  // 5 min

// DIAGNOSTICO/RECUPERACAO - tempo minimo com o gate fechado antes de UMA
// tentativa de recriar a sessao com o piso de emergencia. O gate e reavaliado a
// cada loop() e nao consome o intervalo de polling, portanto sem este atraso a
// tentativa se repetiria a cada iteracao e cada falha custaria um handshake
// completo. Uma tentativa por minuto deixa o bot voltar a responder em ate ~1
// min depois de perder a sessao, sem martelar a Bot API enquanto ela estiver
// inacessivel.
constexpr uint32_t TELEGRAM_GATE_RECOVERY_PERIOD_MS = 60000UL;  // 1 min

// Idade maxima da sessao TLS antes de RECRIA-LA DE PROPOSITO. Medido em campo em
// 2026-11-20, em duas capturas independentes (uptime 3 640 s e 3 660 s): o
// api.telegram.org encerra a conexao com ~1 h de idade, MESMO com keep-alive a
// cada 20 s. Prova: a sessao foi reaproveitada por 182 consultas (3 620 s de
// idade, sem nenhum handshake - "etapa1 handshake ok em=0 ms") e caiu 20 s
// depois de uma resposta 200 normal.
//
// Renovar antes troca uma queda descontrolada por um fechamento previsivel. O
// fechamento e barato porque acontece DEPOIS de a consulta terminar e ANTES de o
// gate medir o heap: os ~8,4 KB voltam ao heap e a consulta seguinte recria a
// sessao com o heap inteiro.
constexpr uint32_t TELEGRAM_SESSION_MAX_AGE_MS = 50UL * 60UL * 1000UL;  // 50 min

static_assert(TELEGRAM_SESSION_MAX_AGE_MS >= 5UL * TELEGRAM_POLL_INTERVAL_MS,
              "renovar a sessao a cada poucos polls nao faz sentido");
static_assert(TELEGRAM_SESSION_MAX_AGE_MS < 3600000UL,
              "o servidor fecha a sessao com ~1 h: renovar antes disso");

// ---------------------------------------------------------------------------
//  Alertas de nivel (Telegram)
//
//  O firmware compara o volume FILTRADO (o mesmo do display, do MQTT e do
//  /status) com dois limiares e envia UMA mensagem por transicao:
//    volume <= lr                  -> "atingiu o nivel de risco"
//    volume <= lc                  -> "atingiu o nivel critico"
//    subiu para >= lc + hist       -> "recuperou-se do nivel critico"
//    subiu para >= lr + hist       -> "recuperou-se do nivel de risco"
//
//  Os limiares sao PERCENTUAIS DO MAIOR VOLUME MEDIVEL (fundo de escala do
//  sensor): 100% = volume::maxMeasurableLiters() = 116.651 L (nivel corrigido de
//  2.809,5 mm). Usa-se o teto do sensor, e nao os 3 m de agua do reservatorio,
//  porque o ADC satura em 3,0 V - ou seja, 3 m de agua NAO seriam medidos e um
//  limiar acima de 93,65% nunca dispararia.
//
//  Quando o nivel maximo real for identificado em producao, basta ajustar a
//  calibracao em include/volume.h (as constantes sao a fonte unica deste numero).
// ---------------------------------------------------------------------------
constexpr uint8_t ALERT_DEFAULT_RISK_PCT     = 60;
constexpr uint8_t ALERT_DEFAULT_CRITICAL_PCT = 30;
constexpr uint8_t ALERT_DEFAULT_HIST_PCT     = 5;

// Tempo entre o boot e a PRIMEIRA avaliacao: da tempo de sair do ruido da
// primeira amostra e de o filtro encher (MOVING_AVERAGE_WINDOW amostras).
constexpr uint32_t ALERT_SETTLE_MS = 15000UL;

// Arquivo de configuracao/estado dos alertas: dois slots A/B (mesmo padrao do
// historico). Nada aqui formata o LittleFS: se a montagem falhar, os alertas
// funcionam em RAM com os defaults e o /w avisa que nao houve persistencia.
constexpr char ALERT_FILE_A[] = "/w-alerts-a.dat";
constexpr char ALERT_FILE_B[] = "/w-alerts-b.dat";

static_assert(volume::maxMeasurableLiters() > 116000.0f &&
                  volume::maxMeasurableLiters() < 117000.0f,
              "a referencia dos alertas deve ser o teto do sensor (116.651 L)");
static_assert(ALERT_DEFAULT_CRITICAL_PCT < ALERT_DEFAULT_RISK_PCT,
              "o limiar critico deve ser menor que o de risco");
static_assert(ALERT_DEFAULT_RISK_PCT + ALERT_DEFAULT_HIST_PCT <= 100,
              "risco + histerese nao pode passar de 100% (nunca recuperaria)");
static_assert(ALERT_SETTLE_MS >= MOVING_AVERAGE_WINDOW * SAMPLE_INTERVAL_MS,
              "o settle deve cobrir o filtro cheio (janela x intervalo)");
// ---------------------------------------------------------------------------
//  Guarda de bloqueio - reboot controlado
//
//  Os dois servicos externos podem ficar bloqueados SEM prazo: o Telegram quando
//  o gate de heap nao abre (fragmentacao) e o MQTT quando o broker/TLS nao aceita
//  a sessao. O reboot devolve o heap inteiro e refaz as sessoes TLS do zero.
//
//  ULTIMO RECURSO: o reboot NAO e um evento periodico. Ele exige as DUAS
//  condicoes ao mesmo tempo: (1) um servico bloqueado ha TELEGRAM_STALL_REBOOT_MS
//  / MQTT_STALL_REBOOT_MS e (2) o heap fragmentado a ponto de nao dar mais para
//  recriar a sessao TLS - maior bloco contiguo abaixo de STALL_GUARD_MAX_BLOCK_FLOOR
//  por STALL_GUARD_FRAGMENTED_MS. Um servico parado com o heap intacto NAO
//  reinicia: o MQTT se recupera no backoff e o Telegram tem o piso de emergencia
//  do gate (o caso real de campo era o heap preso abaixo do piso).
//
//  Regras: so conta com Wi-Fi conectado e fora da carencia do boot; nao conta
//  quando as credenciais estao pendentes (nao ha o que reconectar); avisa uma vez
//  ao passar de STALL_GUARD_FIRST_WARN_MS (servico) e ao persistir a fragmentacao.
//
//  Custo de um reboot: uptime volta a zero, o historico do dia perde o acumulado
//  desde o ultimo snapshot (ate HIST_SNAPSHOT_INTERVAL_MS) e a fila offline e
//  preservada (LittleFS, com CRC e reparo no boot).
// ---------------------------------------------------------------------------
constexpr uint32_t STALL_GUARD_GRACE_MS = 60000UL;  // 1 min apos o boot
constexpr uint32_t STALL_GUARD_FIRST_WARN_MS = 600000UL;  // 10 min: primeiro aviso
constexpr uint32_t TELEGRAM_STALL_REBOOT_MS = 1800000UL;  // 30 min sem consulta ok
constexpr uint32_t MQTT_STALL_REBOOT_MS = 1800000UL;      // 30 min sem sessao

// Piso do maior bloco contiguo que AUTORIZA o reboot. Abaixo dele nem a
// recuperacao consegue recriar a sessao TLS (TELEGRAM_MIN_FREE_BLOCK_RECOVERY):
// e o estado "nao mais administravel" em que o reboot passa a valer a pena.
constexpr size_t STALL_GUARD_MAX_BLOCK_FLOOR = TELEGRAM_MIN_FREE_BLOCK_RECOVERY;
// Quanto tempo o bloco tem de ficar abaixo do piso para o reboot ser permitido.
// O bloco oscila durante publicacao/handshake; so um estado SUSTENTADO conta.
constexpr uint32_t STALL_GUARD_FRAGMENTED_MS = 300000UL;  // 5 min

static_assert(STALL_GUARD_GRACE_MS >= TELEGRAM_POLL_INTERVAL_MS,
              "a carencia deve cobrir pelo menos um intervalo de polling");
static_assert(STALL_GUARD_FIRST_WARN_MS >= STALL_GUARD_GRACE_MS,
              "o primeiro aviso nao pode ser anterior a carencia");
static_assert(TELEGRAM_STALL_REBOOT_MS > STALL_GUARD_FIRST_WARN_MS,
              "o reboot deve vir depois do aviso");
static_assert(TELEGRAM_STALL_REBOOT_MS > TELEGRAM_GATE_RECOVERY_PERIOD_MS,
              "a recuperacao do gate deve ter chance antes do reboot");
static_assert(MQTT_STALL_REBOOT_MS > MQTT_MAX_RECONNECT_MS,
              "o backoff do MQTT deve ter chance antes do reboot");
static_assert(TELEGRAM_STALL_REBOOT_MS <= 7200000UL && MQTT_STALL_REBOOT_MS <= 7200000UL,
              "bloqueio de mais de 2 h nao deve esperar o reboot");
// O piso do reboot nao pode afrouxar a criacao da sessao: ele representa o
// estado em que a recriacao ja falhou.
static_assert(STALL_GUARD_MAX_BLOCK_FLOOR >= TELEGRAM_MIN_FREE_BLOCK_RECOVERY,
              "o piso do reboot deve cobrir a recriacao da sessao TLS");
// Exige persistencia: o gate de emergencia tenta uma vez por minuto e a
// oscilacao do bloco nao pode disparar o reboot.
static_assert(STALL_GUARD_FRAGMENTED_MS >= TELEGRAM_GATE_RECOVERY_PERIOD_MS,
              "a fragmentacao precisa persistir alem da recuperacao do gate");

// DIAGNOSTICO - instante da amostra de heap depois que a consulta termina
// (sessao TLS ja destruida). Da tempo para o lwIP devolver os pbufs e para o
// alocador fundir os blocos livres.
constexpr uint32_t TELEGRAM_POST_POLL_SAMPLE_MS = 5000UL;  // 5 s

// Comando recebido: texto util (o resto e ignorado).
constexpr size_t TELEGRAM_TEXT_MAX = 64;

// Aviso de boot (mensagem enviada uma vez a cada boot). Cada consulta tenta
// entrega-lo; depois deste numero de tentativas o aviso e descartado ate o
// proximo boot. Nada e persistido em flash.
constexpr uint8_t TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS = 3;

static_assert(TELEGRAM_PORT == 443, "TELEGRAM_PORT deve ser 443 (HTTPS)");
static_assert(TELEGRAM_TLS_RX_BUFFER >= 512 && TELEGRAM_TLS_RX_BUFFER <= 16384,
              "TELEGRAM_TLS_RX_BUFFER deve estar em [512, 16384]");
static_assert(TELEGRAM_TLS_TX_BUFFER >= 512 && TELEGRAM_TLS_TX_BUFFER <= 16384,
              "TELEGRAM_TLS_TX_BUFFER deve estar em [512, 16384]");
// A cadeia do Telegram medida tem 3919 B em um registro: nao reduzir.
static_assert(TELEGRAM_TLS_RX_BUFFER >= 4096,
              "TELEGRAM_TLS_RX_BUFFER deve cobrir a cadeia do Telegram (3919 B)");
static_assert(TELEGRAM_RESPONSE_TIMEOUT_MS < TELEGRAM_POLL_INTERVAL_MS,
              "o timeout da resposta deve caber no intervalo de polling");
static_assert(TELEGRAM_BODY_MAX <= 4096, "corpo HTTP maximo acima do razoavel");
// O piso de heap livre tem de cobrir o pico do handshake (bloco + validacao).
static_assert(TELEGRAM_MIN_FREE_HEAP > TELEGRAM_MIN_FREE_BLOCK,
              "TELEGRAM_MIN_FREE_HEAP deve ser maior que o bloco minimo");
// O piso de emergencia so pode ser MENOR que o normal: ele existe para o buraco
// deixado pela sessao anterior caber na recriacao, nunca para afrouxar o gate.
static_assert(TELEGRAM_MIN_FREE_BLOCK_RECOVERY < TELEGRAM_MIN_FREE_BLOCK,
              "o piso de emergencia deve ser menor que o piso normal");
// Cobre 8 426 B de alocacao + cabecalhos do alocador (medido: 8 450 B).
static_assert(TELEGRAM_MIN_FREE_BLOCK_RECOVERY >= 8576,
              "o piso de emergencia deve cobrir a alocacao real da sessao");
// Uma recuperacao por loop seria um handshake por iteracao.
static_assert(TELEGRAM_GATE_RECOVERY_PERIOD_MS >= TELEGRAM_POLL_INTERVAL_MS,
              "a recuperacao nao pode ser mais frequente que o polling");


