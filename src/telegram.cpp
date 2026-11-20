#include "telegram.h"

#include <ArduinoJson.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "clock.h"
#include "config.h"
#include "credentials.h"
#include "history_record.h"
#include "boot_reason.h"
#include "telegram_ca.h"

// ============================================================================
//  Telegram - implementacao
//
//  Cliente HTTP/1.1 proprio sobre WiFiClientSecure (BearSSL), adaptado do
//  Monitora_DG mas SEM String: todos os buffers sao fixos e reaproveitados.
//
//  Nada aqui usa setInsecure(): a cadeia e validada contra a raiz Go Daddy G2
//  embutida em telegram_ca.h (a intermediaria vem no fio). O handshake, a
//  requisicao e a leitura acontecem dentro de UMA consulta. A sessao TLS e
//  PERSISTENTE entre consultas (TELEGRAM_PERSISTENT_SESSION) porque o ESP8266
//  mantem a sessao do MQTT aberta ao mesmo tempo: criar e destruir ~8,5 KB a
//  cada 20 s fragmentava o heap. O ciclo de vida da sessao e:
//    - renovada de proposito a cada TELEGRAM_SESSION_MAX_AGE_MS (o servidor
//      fecha a conexao com ~1 h de idade);
//    - liberada ANTES do gate medir o heap quando o socket morreu entre
//      consultas (sem isso os ~8,4 KB ficavam presos e o bot emudecia);
//    - descartada nos caminhos de erro de transporte/HTTP/JSON.
// ============================================================================
namespace {

// Ancora de confianca do Telegram, carregada uma unica vez (mesma abordagem do
// MQTT em thingsboard.cpp). ~1 KB de RAM permanente, sem realocar a cada poll.
BearSSL::X509List g_trustAnchor(TELEGRAM_CA);

// Sessao TLS da consulta corrente: criada em ensureConnection() e reaproveitada
// entre consultas (TELEGRAM_PERSISTENT_SESSION) ate cair ou ser renovada.
WiFiClientSecure *g_client = nullptr;

// Instante em que a sessao TLS atual foi ABERTA (0 = nenhuma). Usado pela
// renovacao preventiva: o api.telegram.org encerra a conexao com ~1 h de idade,
// mesmo com keep-alive a cada 20 s (medido em campo em 2026-11-20).
uint32_t g_sessionStartMs = 0;

uint32_t sessionAgeMs(uint32_t nowMs) {
  return g_sessionStartMs == 0
             ? 0
             : static_cast<uint32_t>(nowMs - g_sessionStartMs);
}

// Buffers fixos do protocolo, usados apenas durante uma consulta. Ficam em
// .bss (nao no heap) para nao fragmentar a memoria nem arriscar a pilha.
char g_body[TELEGRAM_BODY_MAX];   // corpo da resposta HTTP (lido e parseado)
char g_form[640];                 // corpo do POST: chat_id + text codificado
char g_message[TG_MESSAGE_MAX];   // texto da resposta (/help ou /status)
char g_request[256];              // linha de requisicao + cabecalhos
char g_line[128];                 // uma linha de cabecalho por vez
char g_normalized[32];            // comando normalizado

// Documento do getUpdates limitado aos campos realmente lidos:
//   ok, error_code, description,
//   result[i].update_id, result[i].message.text, result[i].message.chat.id
// O Filter descarta o resto (from, date, entities, ...), o que mantem o
// documento em algumas centenas de bytes. Soma do pior caso:
//   64 (raiz, 4 membros) + 24 (array de 1) + 32 (update) + 32 (message)
//   + 16 (chat) + 72 (texto de 64) + 56 (description de 48) = 296 B.
constexpr size_t kDocCapacity = 384;
constexpr size_t kFilterCapacity = 320;

// ---------------------------------------------------------------------------
//  DIAGNOSTICO (temporario) - observa a consulta sem alterar o comportamento
//
//  kDiag liga/desliga todas as linhas "TG: ..." desta secao. Nao ha String nem
//  alocacao dinamica: apenas buffers fixos e POD. Nenhum token, URL ou chat e
//  impresso - o hex dump e sempre da RESPOSTA do servidor.
//
//  Nao se usa status() para o estado TCP: em WiFiClientSecureCtx o
//  "ClientContext* _client" da classe base e sempre nullptr, portanto
//  WiFiClient::status() responderia CLOSED (0) mesmo com o socket vivo.
//  O estado e caracterizado por connected() + available() + getLastSSLError().
// ---------------------------------------------------------------------------
constexpr bool kDiag = true;

char g_rxHead[32];        // primeiros bytes recebidos (impressos em hex)
size_t g_rxHeadLen = 0;
size_t g_rxTotal = 0;     // bytes recebidos (resposta inteira ou ate a falha)
uint32_t g_rxFirstMs = 0; // millis() do primeiro byte (0 = nenhum byte)

const char *g_failStage = nullptr; // etapa corrente/final (escrita, headers...)
const char *g_failCause = nullptr; // causa da PRIMEIRA falha observada

void diagResetRx() {
  g_rxHeadLen = 0;
  g_rxTotal = 0;
  g_rxFirstMs = 0;
  g_failStage = nullptr;
  g_failCause = nullptr;
}

// Avanca a etapa observada (sobrescreve a anterior).
void diagSetStage(const char *stage) {
  if constexpr (kDiag) {
    g_failStage = stage;
  }
}

// A causa e registrada uma unica vez: as falhas seguintes sao consequencia.
void diagNoteCause(const char *cause) {
  if constexpr (kDiag) {
    if (g_failCause == nullptr) {
      g_failCause = cause;
    }
  }
}

void diagNoteFail(const char *stage, const char *cause) {
  if constexpr (kDiag) {
    if (stage != nullptr) {
      g_failStage = stage;
    }
    diagNoteCause(cause);
  }
}

void diagCaptureByte(int c) {
  if constexpr (kDiag) {
    if (g_rxFirstMs == 0) {
      g_rxFirstMs = millis();
    }
    ++g_rxTotal;
    if (g_rxHeadLen < sizeof(g_rxHead)) {
      g_rxHead[g_rxHeadLen++] = static_cast<char>(c);
    }
  }
}

// "48 54 54 50 ..." - primeiros bytes da RESPOSTA (nunca da requisicao).
void diagHexHead(char *out, size_t outSize) {
  if (outSize == 0) {
    return;
  }
  size_t pos = 0;
  out[0] = '\0';
  for (size_t i = 0; i < g_rxHeadLen && pos + 4 < outSize; ++i) {
    pos += static_cast<size_t>(snprintf(
        out + pos, outSize - pos, "%02X ",
        static_cast<unsigned>(static_cast<unsigned char>(g_rxHead[i]))));
  }
  if (pos > 0 && pos <= outSize) {
    out[pos - 1] = '\0';
  }
}

// Estado da conexao TLS neste instante. available() apenas alimenta/cacheia o
// buffer do engine (nao consome bytes da aplicacao). getLastSSLError() devolve
// 0/"OK" sem erro, "SSL received fatal alert - ..." quando o servidor enviou
// alerta fatal, -1000 para OOM e o nome de cada BR_ERR_*.
//
// diagPrintf formata em um buffer FIXO e escreve direto no Serial:
// Print::printf() usa char[64] e, para linhas maiores, faz new[]/delete[]
// (alocacao dinamica no meio da consulta), o que aqui nao e aceitavel.
void diagPrintf(const char *fmt, ...) {
  if constexpr (!kDiag) {
    (void)fmt;
    return;
  }

  char line[192];
  va_list args;
  va_start(args, fmt);
  const int len = vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);

  if (len > 0) {
    const size_t size = static_cast<size_t>(len) < sizeof(line) - 1
                            ? static_cast<size_t>(len)
                            : sizeof(line) - 1;
    Serial.write(reinterpret_cast<const uint8_t *>(line), size);
  }
}

// ---------------------------------------------------------------------------
//  Amostra de heap rotulada (DIAGNOSTICO)
//
//  Cada linha mostra o valor absoluto e a VARIACAO em relacao a amostra
//  anterior, para atribuir o custo de cada passo da consulta. Apenas leituras de
//  ESP.getFreeHeap()/getMaxFreeBlockSize(): nenhuma alocacao, nenhum objeto,
//  nenhum efeito sobre o ciclo de vida da sessao TLS.
// ---------------------------------------------------------------------------
int g_diagFreePrev = 0;
int g_diagBlockPrev = 0;

void diagHeap(const char *label) {
  if constexpr (!kDiag) {
    (void)label;
    return;
  }

  const int freeNow = static_cast<int>(ESP.getFreeHeap());
  const int blockNow = static_cast<int>(ESP.getMaxFreeBlockSize());
  diagPrintf("[%7.2f s] TG: heap(%s) livre=%d (%+d) bloco=%d (%+d)\n",
             static_cast<double>(millis()) / 1000.0, label, freeNow,
             freeNow - g_diagFreePrev, blockNow, blockNow - g_diagBlockPrev);
  g_diagFreePrev = freeNow;
  g_diagBlockPrev = blockNow;
}

void diagConnState(const char *where) {
  if constexpr (!kDiag) {
    (void)where;
    return;
  }

  if (g_client == nullptr) {
    diagPrintf("[%7.2f s] TG: estado(%s) cliente=null heap=%u/%u\n",
               static_cast<double>(millis()) / 1000.0, where,
               static_cast<unsigned>(ESP.getFreeHeap()),
               static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
    return;
  }

  char sslText[96] = {0};
  const int sslCode = g_client->getLastSSLError(sslText, sizeof(sslText));

  diagPrintf(
      "[%7.2f s] TG: estado(%s) connected=%d avail=%d ssl=%d erro=\"%s\" "
      "heap=%u/%u\n",
      static_cast<double>(millis()) / 1000.0, where,
      g_client->connected() ? 1 : 0, g_client->available(), sslCode,
      sslCode != 0 ? sslText : "-", static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
}

uint32_t elapsedMs(uint32_t start) {
  return static_cast<uint32_t>(millis() - start);
}

bool expired(uint32_t deadline) {
  return static_cast<int32_t>(millis() - deadline) >= 0;
}

// ---------------------------------------------------------------------------
//  Amostra agendada para depois do fim da consulta (DIAGNOSTICO)
//
//  closeConnection() arma a amostra e update() a executa quando o prazo vence.
//  Nao existe timer nem alocacao: apenas um flag e um instante em .bss.
// ---------------------------------------------------------------------------
bool g_postPollPending = false;
uint32_t g_postPollAtMs = 0;

void diagArmPostPollSample() {
  if constexpr (kDiag) {
    g_postPollPending = true;
    g_postPollAtMs = millis() + TELEGRAM_POST_POLL_SAMPLE_MS;
  }
}

void diagServicePostPollSample() {
  if constexpr (kDiag) {
    if (g_postPollPending && expired(g_postPollAtMs)) {
      g_postPollPending = false;
      diagHeap("5s-apos-consulta");
    }
  }
}

// ---------------------------------------------------------------------------
//  Leitura do socket com deadline absoluto e yield()
// ---------------------------------------------------------------------------
int readByte(uint32_t deadline) {
  while (!expired(deadline)) {
    if (g_client->available()) {
      const int c = g_client->read();
      if (c >= 0) {
        diagCaptureByte(c);
        return c;
      }
    } else if (!g_client->connected()) {
      diagNoteCause("socket-nao-conectado");
      return -1;
    }
    yield();
  }
  diagNoteCause("timeout-sem-dados");
  return -1;
}

// Le uma linha CRLF (o CR e descartado). false em timeout, desconexao ou linha
// maior que o buffer (cabecalho malformado).
bool readLine(uint32_t deadline, char *out, size_t cap) {
  size_t len = 0;
  for (;;) {
    const int c = readByte(deadline);
    if (c < 0) {
      return false;
    }
    if (c == '\n') {
      out[len] = '\0';
      return true;
    }
    if (c == '\r') {
      continue;
    }
    if (len + 1 >= cap) {
      diagNoteCause("linha-longa");
      return false;
    }
    out[len++] = static_cast<char>(c);
  }
}

// Token presente no texto, sem diferenciar maiusculas/minusculas. Os DOIS lados
// sao convertidos: comparar tolower(p[i]) com token[i] cru so funcionava com um
// token que ja estivesse em minusculas.
bool containsTokenIgnoreCase(const char *haystack, const char *token) {
  const size_t tokenLen = strlen(token);
  for (const char *p = haystack; *p != '\0'; ++p) {
    size_t i = 0;
    while (i < tokenLen && p[i] != '\0' &&
           tolower(static_cast<unsigned char>(p[i])) ==
               tolower(static_cast<unsigned char>(token[i]))) {
      ++i;
    }
    if (i == tokenLen) {
      return true;
    }
  }
  return false;
}

// Prefixo sem diferenciar maiusculas/minusculas. strncasecmp compara os dois
// lados convertidos; o laco manual anterior comparava tolower(text[i]) com
// prefix[i] cru e por isso NUNCA casava com "HTTP/" (o primeiro caractere dava
// 'h' != 'H', e toda resposta valida era recusada como "sem-prefixo-http").
bool hasPrefixIgnoreCase(const char *text, const char *prefix) {
  return strncasecmp(text, prefix, strlen(prefix)) == 0;
}

}  // namespace


namespace {

// ---------------------------------------------------------------------------
//  Escrita e leitura de UMA resposta HTTP/1.1
// ---------------------------------------------------------------------------
// Devolve quantos bytes foram efetivamente aceitos pelo cliente TLS (igual a
// `len` somente quando a requisicao inteira saiu). DIAG: a contagem e o valor
// que a instrumentacao publica; a decisao do chamador continua sendo == len.
size_t writeAll(const char *data, size_t len) {
  size_t sent = 0;
  const uint32_t deadline = millis() + TELEGRAM_RESPONSE_TIMEOUT_MS;
  while (sent < len && !expired(deadline)) {
    const size_t written = g_client->write(
        reinterpret_cast<const uint8_t *>(data) + sent, len - sent);
    if (written == 0) {
      if (!g_client->connected()) {
        diagNoteFail("escrita", "socket-nao-conectado");
        return sent;
      }
      yield();
      continue;
    }
    sent += written;
  }
  if (sent < len) {
    diagNoteFail("escrita", "timeout-escrita");
  }
  return sent;
}

// Consome as linhas de trailer de um corpo chunked (ate a linha vazia).
bool consumeTrailers(uint32_t deadline) {
  char line[128];
  for (;;) {
    if (!readLine(deadline, line, sizeof(line))) {
      return false;
    }
    if (line[0] == '\0') {
      return true;
    }
  }
}

// Le status line + cabecalhos + corpo. Content-Length e chunked sao aceitos;
// qualquer outra forma (sem tamanho) e tratada como falha, porque nao ha como
// delimitar o corpo sem depender do fechamento do socket.
bool readResponse(uint32_t deadline, char *body, size_t cap, size_t &len,
                  int &status) {
  len = 0;
  body[0] = '\0';
  status = 0;

  diagSetStage("status-line");
  if (!readLine(deadline, g_line, sizeof(g_line))) {
    return false;
  }
  if (!hasPrefixIgnoreCase(g_line, "HTTP/")) {
    diagNoteCause("sem-prefixo-http");
    return false;
  }
  const char *space = strchr(g_line, ' ');
  if (space == nullptr) {
    diagNoteCause("status-sem-espaco");
    return false;
  }
  status = atoi(space + 1);

  bool chunked = false;
  long contentLength = -1;

  diagSetStage("headers");
  for (;;) {
    if (!readLine(deadline, g_line, sizeof(g_line))) {
      return false;
    }
    if (g_line[0] == '\0') {
      break;
    }

    char *colon = strchr(g_line, ':');
    if (colon == nullptr) {
      continue;
    }
    *colon = '\0';
    const char *value = colon + 1;
    while (*value == ' ' || *value == '\t') {
      ++value;
    }

    if (strcasecmp(g_line, "content-length") == 0) {
      contentLength = atol(value);
    } else if (strcasecmp(g_line, "transfer-encoding") == 0 &&
               containsTokenIgnoreCase(value, "chunked")) {
      chunked = true;
    }
  }

  if (chunked) {
    diagSetStage("chunked");
    for (;;) {
      if (!readLine(deadline, g_line, sizeof(g_line))) {
        return false;
      }
      char *semi = strchr(g_line, ';');
      if (semi != nullptr) {
        *semi = '\0';  // descarta a extensao do chunk
      }
      char *hexEnd = nullptr;
      const long size = strtol(g_line, &hexEnd, 16);
      if (hexEnd == g_line || size < 0) {
        diagNoteCause("chunk-invalido");
        return false;
      }
      if (size == 0) {
        diagSetStage("trailers");
        return consumeTrailers(deadline);
      }
      if (len + static_cast<size_t>(size) >= cap) {
        diagNoteCause("corpo-maior-que-buffer");
        return false;  // corpo maior que o buffer: nao trunca
      }
      for (long i = 0; i < size; ++i) {
        const int c = readByte(deadline);
        if (c < 0) {
          return false;
        }
        body[len++] = static_cast<char>(c);
      }
      body[len] = '\0';
      if (!readLine(deadline, g_line, sizeof(g_line)) || g_line[0] != '\0') {
        diagNoteCause("chunk-sem-crlf");
        return false;  // CRLF depois dos dados do chunk
      }
    }
  }

  if (contentLength < 0) {
    diagNoteCause("sem-content-length");
    return false;
  }
  if (static_cast<size_t>(contentLength) >= cap) {
    diagNoteCause("corpo-maior-que-buffer");
    return false;  // corpo maior que o buffer: nao trunca
  }
  diagSetStage("body");
  for (long i = 0; i < contentLength; ++i) {
    const int c = readByte(deadline);
    if (c < 0) {
      return false;
    }
    body[len++] = static_cast<char>(c);
  }
  body[len] = '\0';
  return true;
}

// ---------------------------------------------------------------------------
//  URL encoding e sessao TLS
// ---------------------------------------------------------------------------
// Codifica `src` em `dst` (application/x-www-form-urlencoded). Devolve o
// comprimento escrito ou 0 quando nao cabe (nada e truncado em silencio).
size_t urlEncodeInto(char *dst, size_t cap, const char *src) {
  static const char kHex[] = "0123456789ABCDEF";
  size_t len = 0;
  for (const unsigned char *p = reinterpret_cast<const unsigned char *>(src);
       *p != '\0'; ++p) {
    const bool safe = isalnum(*p) != 0 || *p == '-' || *p == '_' ||
                      *p == '.' || *p == '~';
    const size_t need = safe ? 1u : 3u;
    if (len + need >= cap) {
      return 0;
    }
    if (safe) {
      dst[len++] = static_cast<char>(*p);
    } else {
      dst[len++] = '%';
      dst[len++] = kHex[(*p >> 4) & 0x0F];
      dst[len++] = kHex[*p & 0x0F];
    }
  }
  dst[len] = '\0';
  return len;
}

void closeConnection() {
  if (g_client != nullptr) {
    g_client->stop();
    delete g_client;
    g_client = nullptr;
    g_sessionStartMs = 0;
    // DIAG: amostra logo apos o encerramento e agendamento da amostra de 5 s.
    // Vale para TODOS os caminhos de saida da consulta, porque todos passam
    // por aqui (nao ha caminho que destrua a sessao fora desta funcao).
    diagHeap("pos-close");
    diagArmPostPollSample();
  }
}

// Sessao TLS pronta para ser REAPROVEITADA? Reaproveitar nao aloca nada: o
// contexto BearSSL e os dois buffers ja estao no heap desde o primeiro connect.
bool sessionReady() {
  return g_client != nullptr && g_client->connected();
}

// Fim de consulta SEM erro de transporte/protocolo. Na variante persistente a
// sessao fica aberta para a proxima consulta (Connection: keep-alive); na
// variante antiga e destruida, devolvendo o heap mas deixando o buraco que
// impede o handshake seguinte.
void endPoll() {
  if constexpr (!TELEGRAM_PERSISTENT_SESSION) {
    closeConnection();
  }
}

// Abre a sessao TLS da consulta. O DNS e resolvido ANTES, com timeout curto: o
// connect do core faria a mesma consulta com 10 s fixos, o que travaria o loop.
bool ensureConnection(uint32_t nowMs) {
  if (g_client != nullptr) {
    // Sessao reaproveitavel: nada e alocado nem reconectado.
    if (g_client->connected()) {
      return true;
    }
    // Socket que morreu DURANTE a consulta atual. A morte ENTRE consultas ja foi
    // tratada em update(), antes de o gate medir o heap; este caminho cobre a
    // sessao que cai no meio do poll (escrita/leitura) e e a rede de seguranca.
    closeConnection();
  }

  // A sessao anterior (se havia) acabou de ser fechada e o bloco que ela ocupava
  // ja voltou ao heap. A recriacao aloca os MESMOS ~8,5 KB, portanto o piso de
  // emergencia e o menor valor aceito aqui: o piso normal ja foi avaliado em
  // update() para a primeira sessao. Sem esta conferencia uma sessao morta no
  // meio de uma consulta seria recriada sem checar memoria nenhuma.
  const size_t block = ESP.getMaxFreeBlockSize();
  const size_t freeHeap = ESP.getFreeHeap();
  if (block < TELEGRAM_MIN_FREE_BLOCK_RECOVERY ||
      freeHeap < TELEGRAM_MIN_FREE_HEAP) {
    Serial.printf(
        "[%7.2f s] TG: sessao nao recriada (bloco=%u/%u B, livre=%u/%u B)\n",
        static_cast<double>(nowMs) / 1000.0, static_cast<unsigned>(block),
        static_cast<unsigned>(TELEGRAM_MIN_FREE_BLOCK_RECOVERY),
        static_cast<unsigned>(freeHeap),
        static_cast<unsigned>(TELEGRAM_MIN_FREE_HEAP));
    return false;
  }

  IPAddress resolved;
  const uint32_t dnsStart = millis();
  if (!WiFi.hostByName(TELEGRAM_HOST, resolved, TELEGRAM_DNS_TIMEOUT_MS)) {
    Serial.printf("[%7.2f s] TG: DNS de %s falhou (%lu ms)\n",
                  static_cast<double>(nowMs) / 1000.0, TELEGRAM_HOST,
                  static_cast<unsigned long>(elapsedMs(dnsStart)));
    return false;
  }

  diagHeap("antes-new");  // referencia: heap com o DNS ja resolvido
  WiFiClientSecure *client = new WiFiClientSecure();
  diagHeap("pos-new");    // objeto WiFiClientSecure + contexto TCP do core
  if (client == nullptr) {
    Serial.printf("[%7.2f s] TG: sem memoria para o cliente TLS\n",
                  static_cast<double>(nowMs) / 1000.0);
    return false;
  }

  // Buffers TLS explicitos (config.h) e CA real: nao existe setInsecure().
  client->setBufferSizes(TELEGRAM_TLS_RX_BUFFER, TELEGRAM_TLS_TX_BUFFER);
  diagHeap("pos-setBufferSizes");  // setBufferSizes so guarda os tamanhos
  client->setTrustAnchors(&g_trustAnchor);
  client->setTimeout(TELEGRAM_SOCKET_TIMEOUT_MS);

  // Data/hora para a validacao X.509 (o core so a usa quando setX509Time() e
  // chamado). Com o NTP ja sincronizado a janela de validade da cadeia e
  // conferida de verdade; antes disso o BearSSL fica sem referencia de tempo e
  // o resultado do handshake passa a depender de o SNTP ter respondido, como
  // aconteceu no MQTT em 2026-11-20 (codigo 54 no boot sem relogio).
  const time_t epochNow = time(nullptr);
  if (epochNow >= static_cast<time_t>(NTP_MIN_VALID_EPOCH_SECONDS)) {
    client->setX509Time(epochNow);
  }

  const uint32_t connectStart = millis();
  const bool tlsUp = client->connect(TELEGRAM_HOST, TELEGRAM_PORT);
  // DIAG: inclui o que o core aloca dentro de _connectSSL() (contexto BearSSL do
  // cliente + buffers RX/TX) e TODO o handshake. O validador X.509 minimo ja foi
  // liberado pelo core ao fim do handshake, portanto NAO aparece nesta amostra.
  diagHeap("pos-connect");
  if (!tlsUp) {
    char sslError[96] = {0};
    const int sslCode = client->getLastSSLError(sslError, sizeof(sslError));
    Serial.printf("[%7.2f s] TG: TLS falhou (codigo=%d, %lu ms)%s%s\n",
                  static_cast<double>(nowMs) / 1000.0, sslCode,
                  static_cast<unsigned long>(elapsedMs(connectStart)),
                  sslCode != 0 ? ": " : "", sslCode != 0 ? sslError : "");
    client->stop();
    delete client;
    diagHeap("pos-delete-erro");  // confirma que a falha devolve o heap
    return false;
  }

  g_client = client;
  g_sessionStartMs = nowMs;  // renovacao preventiva conta a partir daqui
  Serial.printf("[%7.2f s] TG: TLS ok com %s:%u (%lu ms)\n",
                static_cast<double>(nowMs) / 1000.0, TELEGRAM_HOST,
                static_cast<unsigned>(TELEGRAM_PORT),
                static_cast<unsigned long>(elapsedMs(connectStart)));
  return true;
}

// ---------------------------------------------------------------------------
//  JSON da Bot API
// ---------------------------------------------------------------------------
void buildUpdateFilter(JsonObject filter) {
  filter["ok"] = true;
  filter["error_code"] = true;
  filter["description"] = true;
  filter["result"][0]["update_id"] = true;
  filter["result"][0]["message"]["text"] = true;
  filter["result"][0]["message"]["chat"]["id"] = true;
}

// Um chat autorizado? Os IDs vem como numero de 64 bits (grupos -100... nao
// cabem em 32) e sao comparados com o valor de credentials.h convertido.
bool isAuthorizedChat(int64_t chatId) {
  const char *primary = ACTIVE_TELEGRAM_CHAT_ID;
  const char *secondary = ACTIVE_TELEGRAM_CHAT_ID2;

  if (primary != nullptr && primary[0] != '\0' &&
      chatId == strtoll(primary, nullptr, 10)) {
    return true;
  }
  if (secondary != nullptr && secondary[0] != '\0' &&
      chatId == strtoll(secondary, nullptr, 10)) {
    return true;
  }
  return false;
}

}  // namespace

namespace {

// Extrai do historico SOMENTE o que a mensagem mostra. Nenhum agregado e
// recalculado: dayReport() e closedPeriodReport() ja devolvem min/max.
void buildStatusData(const TelegramContext &context, TelegramStatusData &data) {
  data = TelegramStatusData();
  data.hasVolume = context.sampleValid;
  data.volumeL = context.volumeL;

  const History *history = context.history;
  if (history == nullptr) {
    return;
  }

  const uint64_t epochMs = device_clock::nowMs();
  if (epochMs == 0) {
    return;  // sem NTP nao existe "hoje" nem "ontem"
  }

  const uint32_t nowEpochS = static_cast<uint32_t>(epochMs / 1000ULL);
  const uint32_t today = history_record::dayIndex(nowEpochS);
  if (today == 0) {
    return;
  }

  HistoryDayReport day;
  if (history->dayReport(today, nowEpochS, day)) {
    data.hasToday = true;
    data.todayMinL = day.minL;
    data.todayMaxL = day.maxL;
  }
  if (history->dayReport(today - 1, nowEpochS, day)) {
    data.hasYesterday = true;
    data.yesterdayMinL = day.minL;
    data.yesterdayMaxL = day.maxL;
  }

  HistoryPeriodReport period;
  if (history->closedPeriodReport(nowEpochS, 7, period)) {
    data.hasLast7 = true;
    data.last7MinL = period.minL;
    data.last7MaxL = period.maxL;
  }
  if (history->closedPeriodReport(nowEpochS, 30, period)) {
    data.hasLast30 = true;
    data.last30MinL = period.minL;
    data.last30MaxL = period.maxL;
  }
}

// Motivo do reset vindo do ENUM do SDK (rst_info::reason), traduzido para a
// MESMA string que ESP.getResetReason() devolveria. Evita getResetReason(): ele
// constroi uma String e arrasta ~650 B de dados/codigo da newlib para o binario
// (medido: RAM estatica caia de 44 116 para 43 46x B ao trocar).
const char *resetReasonText(uint32_t reason) {
  switch (reason) {
    case REASON_DEFAULT_RST:
      return "Power On";
    case REASON_WDT_RST:
      return "Hardware Watchdog";
    case REASON_EXCEPTION_RST:
      return "Exception";
    case REASON_SOFT_WDT_RST:
      return "Software Watchdog";
    case REASON_SOFT_RESTART:
      return "Software/System restart";
    case REASON_DEEP_SLEEP_AWAKE:
      return "Deep-Sleep Wake";
    case REASON_EXT_SYS_RST:
      return "External System";
    default:
      return nullptr;
  }
}

}  // namespace

// ---------------------------------------------------------------------------
//  Ciclo de vida
// ---------------------------------------------------------------------------
void Telegram::stateText(char *out, size_t cap, uint32_t nowMs) const {
  if (out == nullptr || cap == 0) {
    return;
  }
  if (!m_enabled) {
    snprintf(out, cap, "off");
    return;
  }
  if (m_gateBlocked) {
    // Gate fechado: mostrar o bloco contiguo ATUAL (o valor muda sozinho se o
    // heap se recuperar, antes mesmo da proxima consulta).
    snprintf(out, cap, "adiado(%uB)",
             static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
    return;
  }
  if (m_polls == 0) {
    snprintf(out, cap, "aguardando");
    return;
  }
  snprintf(out, cap, "ultima=%lus",
           static_cast<unsigned long>(
               static_cast<uint32_t>(nowMs - m_lastPollMs) / 1000UL));
}

void Telegram::begin(uint32_t nowMs) {
  m_enabled = !telegramCredentialsPending();
  m_firstPoll = true;
  m_updateOffset = 0;
  // Primeira consulta TELEGRAM_FIRST_POLL_DELAY_MS depois do boot; as seguintes
  // respeitam TELEGRAM_POLL_INTERVAL_MS. O valor inicial "desloca" o intervalo
  // (conta wrap-safe, com millis() pequeno no setup) para a janela ja estar
  // vencida assim que Wi-Fi e relogio ficarem prontos: assim a sessao TLS nasce
  // com o heap inteiro, antes de o MQTT reservar o TLS dele.
  m_lastPollMs =
      nowMs - (TELEGRAM_POLL_INTERVAL_MS - TELEGRAM_FIRST_POLL_DELAY_MS);
  m_polls = 0;
  m_commands = 0;
  m_failures = 0;
  m_pendingWarned = false;
  m_heapWarned = false;
  m_lastHeapWarnMs = 0;
  m_gateBlocked = false;
  m_gateBlockedSinceMs = 0;
  m_lastSuccessMs = 0;

  // Motivo do reset: classificado UMA vez no boot, sem String (resetReasonText).
  // A marca persistida de OTA ainda nao existe, entao false.
  const struct rst_info *resetInfo = ESP.getResetInfoPtr();
  m_bootReason = classifyBootReason(
      false, resetInfo != nullptr ? resetReasonText(resetInfo->reason) : nullptr);
  m_bootNoticeAttempts = 0;
  m_bootNoticeSent = false;

  if (!m_enabled) {
    m_pendingWarned = true;
    Serial.printf(
        "Telegram: desativado (token/chat ausentes em %s). "
        "Sensor, display, MQTT e historico seguem normais.\n",
        activeSecretsFileName());
    return;
  }

  Serial.printf(
      "Telegram: ativo | consulta a cada %lu s | TLS %d/%d B | "
      "CA Go Daddy Root G2 | buffer HTTP %u B\n",
      static_cast<unsigned long>(TELEGRAM_POLL_INTERVAL_MS / 1000UL),
      TELEGRAM_TLS_RX_BUFFER, TELEGRAM_TLS_TX_BUFFER,
      static_cast<unsigned>(TELEGRAM_BODY_MAX));

  Serial.printf("Telegram: aviso de boot pendente (motivo=%s)\n",
                bootReasonDescription(m_bootReason));

  // DIAG: separa o que o BearSSL aloca (sizeof e constante de COMPILACAO, custo
  // zero em RAM) do que nos pedimos ao core. O core soma 325 B ao RX e 85 B ao
  // TX; o contexto do cliente e o validador X.509 vao para o heap no
  // _connectSSL(). Nenhuma alteracao no core foi necessaria para obter isto.
  if constexpr (kDiag) {
    diagPrintf(
        "TG: BearSSL sizeof(ctx)=%u B sizeof(x509)=%u B | buffers pedidos "
        "rx=%d(+325)=%d B tx=%d(+85)=%d B\n",
        static_cast<unsigned>(sizeof(br_ssl_client_context)),
        static_cast<unsigned>(sizeof(br_x509_minimal_context)),
        TELEGRAM_TLS_RX_BUFFER, TELEGRAM_TLS_RX_BUFFER + 325,
        TELEGRAM_TLS_TX_BUFFER, TELEGRAM_TLS_TX_BUFFER + 85);
  }
}

void Telegram::update(uint32_t nowMs, const TelegramContext &context) {
  // DIAG: executa a amostra de heap agendada por closeConnection(). Fora do modo
  // de diagnostico a funcao e vazia (if constexpr) e nada muda.
  diagServicePostPollSample();

  if (!m_enabled) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    closeConnection();
    return;
  }

  // Sem relogio valido o BearSSL recusa a cadeia (BR_ERR_X509_EXPIRED, medido em
  // campo em 2026-11-20): a consulta ESPERA o NTP sem consumir o intervalo. E o
  // que permite a primeira consulta sair poucos segundos depois do boot, quando
  // o heap ainda esta inteiro.
  if (!device_clock::synced()) {
    return;
  }

  // Intervalo contado do INICIO da consulta anterior. Como a consulta e
  // sincrona, nunca existem duas ao mesmo tempo.
  if (static_cast<uint32_t>(nowMs - m_lastPollMs) < TELEGRAM_POLL_INTERVAL_MS) {
    return;
  }

  // ------------------------------------------------------------------------
  //  Sessao TLS: liberar a morta e renovar a antiga ANTES de medir o heap
  //
  //  (1) Sessao aberta com socket morto. Quando o servidor fecha a conexao ENTRE
  //      duas consultas, sessionReady() fica false mas o contexto e os buffers
  //      continuam alocados. Medido em campo (2026-11-20): maior bloco=5240 B e
  //      heap=5544 B com a sessao morta PRESA - o gate recusava para sempre (o
  //      piso de emergencia nunca disparava, 5240 < 8704) e os ~8,4 KB ficavam
  //      presos ate o proximo reboot. Liberar aqui devolve o bloco e a consulta
  //      segue no MESMO ciclo: recuperacao em 0 s, nao em 1 min.
  //
  //  (2) Renovacao preventiva em TELEGRAM_SESSION_MAX_AGE_MS: o servidor derruba
  //      a conexao com ~1 h de idade (duas capturas independentes). Fechar de
  //      proposito mantem o churn previsivel e acontece com o heap conhecido.
  //
  //  Em ambos os casos o fechamento vem antes de ler block/freeHeap, que e o que
  //  torna o gate correto: ele passa a medir o heap DEPOIS da devolucao.
  //  As linhas ficam abaixo de 64 caracteres de proposito: Print::printf aloca
  //  (new char[len+1]) quando a mensagem nao cabe no buffer interno de 64 B.
  // ------------------------------------------------------------------------
  if (g_client != nullptr && !g_client->connected()) {
    const size_t blockBefore = ESP.getMaxFreeBlockSize();
    closeConnection();
    m_gateBlocked = false;
    m_gateBlockedSinceMs = 0;
    Serial.printf("[%7.2f s] TG: sessao morta liberada (bloco=%u->%u B)\n",
                  static_cast<double>(nowMs) / 1000.0,
                  static_cast<unsigned>(blockBefore),
                  static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
  } else if (sessionReady() &&
             sessionAgeMs(nowMs) >= TELEGRAM_SESSION_MAX_AGE_MS) {
    Serial.printf("[%7.2f s] TG: renovacao preventiva (idade=%lu s)\n",
                  static_cast<double>(nowMs) / 1000.0,
                  static_cast<unsigned long>(sessionAgeMs(nowMs) / 1000UL));
    closeConnection();
    m_gateBlocked = false;
    m_gateBlockedSinceMs = 0;
  }

  // Memoria suficiente = maior bloco contiguo E heap livre total. So o bloco nao
  // basta: no pico do handshake o heap livre cai a ~1,8 KB e o lwIP/WiFi morre em
  // memcpy(NULL) no contexto sys (exception 29). Abaixo de qualquer um dos pisos
  // a consulta e adiada sem consumir o intervalo (o proximo loop tenta de novo);
  // apenas a MENSAGEM e limitada a uma a cada TELEGRAM_HEAP_WARN_PERIOD_MS.
  // O gate vale SO quando a sessao precisa ser CRIADA (handshake + alocacoes).
  // Reaproveitar a sessao aberta nao aloca nada: contexto e buffers ja estao no
  // heap desde o primeiro connect.
  const size_t block = ESP.getMaxFreeBlockSize();
  const size_t freeHeap = ESP.getFreeHeap();

  // Dois pisos de bloco contiguo:
  //   normal     - criar a sessao com o heap inteiro (primeira consulta do boot);
  //   emergencia - RECRIAR a sessao que acabou de cair. O buraco deixado por ela
  //                tem o tamanho exato da sessao (campo: 8 976 B) e pode ficar
  //                abaixo do piso normal (9 216 B) sem ser insuficiente.
  // O piso de heap LIVRE TOTAL e o mesmo nos dois casos: e ele que protege o
  // lwIP/WiFi no pico do handshake.
  bool canPoll = sessionReady() ||
                 (block >= TELEGRAM_MIN_FREE_BLOCK &&
                  freeHeap >= TELEGRAM_MIN_FREE_HEAP);

  if (!canPoll && block >= TELEGRAM_MIN_FREE_BLOCK_RECOVERY &&
      freeHeap >= TELEGRAM_MIN_FREE_HEAP && m_gateBlocked &&
      static_cast<uint32_t>(nowMs - m_gateBlockedSinceMs) >=
          TELEGRAM_GATE_RECOVERY_PERIOD_MS) {
    canPoll = true;
    // Reinicia a contagem: se a tentativa falhar, a proxima so daqui a
    // TELEGRAM_GATE_RECOVERY_PERIOD_MS (nunca uma por iteracao do loop).
    m_gateBlockedSinceMs = nowMs;
    Serial.printf(
        "[%7.2f s] TG: recuperacao - recriando a sessao com bloco=%u B "
        "(normal=%u, emergencia=%u) e livre=%u B\n",
        static_cast<double>(nowMs) / 1000.0, static_cast<unsigned>(block),
        static_cast<unsigned>(TELEGRAM_MIN_FREE_BLOCK),
        static_cast<unsigned>(TELEGRAM_MIN_FREE_BLOCK_RECOVERY),
        static_cast<unsigned>(freeHeap));
  }

  if (!canPoll) {
    // DIAG: desde quando o gate esta fechado. O adiamento em si nao muda.
    if (!m_gateBlocked) {
      m_gateBlocked = true;
      m_gateBlockedSinceMs = nowMs;
    }
    if (!m_heapWarned ||
        static_cast<uint32_t>(nowMs - m_lastHeapWarnMs) >=
            TELEGRAM_HEAP_WARN_PERIOD_MS) {
      m_heapWarned = true;
      m_lastHeapWarnMs = nowMs;
      Serial.printf(
          "[%7.2f s] TG: sem sessao e heap insuficiente (maior bloco=%u/%u B, "
          "livre=%u/%u B); gate fechado ha %lu s; consulta adiada "
          "(novo aviso em %lu s)\n",
          static_cast<double>(nowMs) / 1000.0, static_cast<unsigned>(block),
          static_cast<unsigned>(TELEGRAM_MIN_FREE_BLOCK),
          static_cast<unsigned>(freeHeap),
          static_cast<unsigned>(TELEGRAM_MIN_FREE_HEAP),
          static_cast<unsigned long>(
              static_cast<uint32_t>(nowMs - m_gateBlockedSinceMs) / 1000UL),
          static_cast<unsigned long>(TELEGRAM_HEAP_WARN_PERIOD_MS / 1000UL));
    }
    return;
  }
  m_gateBlocked = false;

  m_lastPollMs = nowMs;

  // Aviso de boot: sai ANTES da consulta, usando a mesma sessao que a consulta
  // vai usar em seguida (ensureConnection nao aloca duas vezes). Uma tentativa
  // por consulta; apos TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS o aviso e descartado.
  if (!m_bootNoticeSent) {
    sendBootNotice(nowMs, context);
  }

  // Alertas de nivel: no maximo UM por consulta, pela mesma sessao (e antes de
  // a consulta usa-la). O evento ja foi enfileirado por AlertMonitor::update().
  flushPendingAlerts(nowMs, context.alerts);

  // DIAG: heap no inicio e no fim do poll. Fica aqui (ponto unico) para cobrir
  // todos os caminhos de saida de pollOnce().
  if constexpr (kDiag) {
    diagPrintf(
        "[%7.2f s] TG: poll #%lu inicio heap_livre=%u maior_bloco=%u\n",
        static_cast<double>(nowMs) / 1000.0,
        static_cast<unsigned long>(m_polls + 1),
        static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(block));
  }

  pollOnce(nowMs, context);

  if constexpr (kDiag) {
    diagPrintf(
        "[%7.2f s] TG: poll #%lu fim heap_livre=%u maior_bloco=%u\n",
        static_cast<double>(millis()) / 1000.0,
        static_cast<unsigned long>(m_polls),
        static_cast<unsigned>(ESP.getFreeHeap()),
        static_cast<unsigned>(ESP.getMaxFreeBlockSize()));
  }
}

// ---------------------------------------------------------------------------
//  Aviso de boot - uma mensagem por boot
//
//  Reusa o MESMO caminho de envio das respostas (ensureConnection +
//  sendMessage), portanto a mesma sessao TLS, o mesmo chat e os mesmos buffers.
//  Nao toca em getUpdates, no offset nem no intervalo de consulta: se o envio
//  falhar, a proxima consulta tenta de novo, ate
//  TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS.
// ---------------------------------------------------------------------------
void Telegram::sendBootNotice(uint32_t nowMs, const TelegramContext &context) {
  if (m_bootNoticeSent ||
      m_bootNoticeAttempts >= TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS) {
    return;
  }

  const int64_t chatId = strtoll(ACTIVE_TELEGRAM_CHAT_ID, nullptr, 10);
  if (chatId == 0) {
    m_bootNoticeAttempts = TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS;
    return;
  }

  if (!ensureConnection(nowMs)) {
    ++m_bootNoticeAttempts;  // ensureConnection ja registrou o motivo
    return;
  }
  ++m_bootNoticeAttempts;

  // Linha do nivel: o estado do volume de AGORA (nao o persistido, que pode ser
  // de antes do reboot). Sem amostra valida ainda, a mensagem diz "sem leitura".
  const bool hasLevel = context.sampleValid && context.alerts != nullptr;
  const alerts::Level level =
      hasLevel ? context.alerts->levelForVolume(context.volumeL)
               : alerts::Level::NORMAL;

  buildBootMessage(m_bootReason, FIRMWARE_VERSION, level, hasLevel,
                   context.volumeL, g_message, sizeof(g_message));
  if (sendMessage(chatId, g_message)) {
    m_bootNoticeSent = true;
    Serial.printf("[%7.2f s] TG: aviso de boot enviado (motivo=%s)\n",
                  static_cast<double>(millis()) / 1000.0,
                  bootReasonDescription(m_bootReason));
    return;
  }

  Serial.printf("[%7.2f s] TG: aviso de boot nao entregue (%u/%u)\n",
                static_cast<double>(millis()) / 1000.0,
                static_cast<unsigned>(m_bootNoticeAttempts),
                static_cast<unsigned>(TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS));
}

// ---------------------------------------------------------------------------
//  Alertas de nivel - no maximo UMA mensagem por consulta
//
//  Mesmo caminho do aviso de boot (ensureConnection + sendMessage), portanto a
//  mesma sessao TLS, o mesmo chat primario e os mesmos buffers: reusa g_message,
//  que e consumido dentro do proprio sendMessage. Sem pendencia, sem sessao ou
//  com falha de envio o alerta PERMANECE na fila persistida e a consulta
//  seguinte tenta de novo.
// ---------------------------------------------------------------------------
void Telegram::flushPendingAlerts(uint32_t nowMs, AlertMonitor *alerts) {
  if (alerts == nullptr || !alerts->peekPending(g_message, sizeof(g_message))) {
    return;
  }

  const int64_t chatId = strtoll(ACTIVE_TELEGRAM_CHAT_ID, nullptr, 10);
  if (chatId == 0) {
    return;
  }

  if (!ensureConnection(nowMs)) {
    return;  // sem sessao agora; o alerta continua pendente
  }

  if (!sendMessage(chatId, g_message)) {
    ++m_failures;
    Serial.printf(PSTR("[%7.2f s] TG: alerta nao entregue; permanece na fila\n"),
                  static_cast<double>(millis()) / 1000.0);
    return;
  }

  alerts->confirmPending();
  Serial.printf(PSTR("[%7.2f s] TG: alerta enviado (pendentes=%u)\n"),
                static_cast<double>(millis()) / 1000.0,
                static_cast<unsigned>(alerts->pendingCount()));
}

// ---------------------------------------------------------------------------
//  UMA consulta completa: getUpdates -> (comando) -> sendMessage
//
//  A sessao TLS e reaproveitada entre consultas (TELEGRAM_PERSISTENT_SESSION):
//  abrir/fechar a cada 20 s deixava um buraco de ~8,5 KB no heap e a consulta
//  seguinte nao cabia. Nos caminhos de ERRO a sessao e sempre descartada.
// ---------------------------------------------------------------------------
void Telegram::pollOnce(uint32_t nowMs, const TelegramContext &context) {
  const uint32_t start = millis();
  ++m_polls;

  if (!ensureConnection(nowMs)) {
    ++m_failures;
    closeConnection();
    return;
  }
  const uint32_t connectDone = millis();

  // DIAG: etapa 1 (DNS + alocacao + handshake) e estado IMEDIATAMENTE depois.
  // diagConnState() chama available(), que apenas alimenta/cacheia o buffer do
  // engine: nenhum byte da resposta e consumido.
  diagResetRx();
  if constexpr (kDiag) {
    diagPrintf(
        "[%7.2f s] TG: etapa1 handshake ok em=%lu ms (dns+aloc+handshake)\n",
        static_cast<double>(millis()) / 1000.0,
        static_cast<unsigned long>(connectDone - start));
    diagConnState("pos-handshake");
  }

  char offsetText[24];
  snprintf(offsetText, sizeof(offsetText), "%lld",
           m_firstPoll ? -1LL : static_cast<long long>(m_updateOffset));

  const int requestLen = snprintf(
      g_request, sizeof(g_request),
      "GET /bot%s/getUpdates?offset=%s&limit=1&timeout=0 HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Connection: keep-alive\r\n\r\n",
      ACTIVE_TELEGRAM_BOT_TOKEN, offsetText, TELEGRAM_HOST);
  if (requestLen <= 0 ||
      static_cast<size_t>(requestLen) >= sizeof(g_request)) {
    Serial.println("TG: requisicao getUpdates nao cabe no buffer");
    ++m_failures;
    closeConnection();
    return;
  }

  const uint32_t deadline = millis() + TELEGRAM_RESPONSE_TIMEOUT_MS;
  int status = 0;
  size_t bodyLen = 0;

  // DIAG: etapa 2 - escrita da requisicao (bytes aceitos + duracao).
  const uint32_t writeStart = millis();
  const size_t written = writeAll(g_request, static_cast<size_t>(requestLen));
  const uint32_t writeDone = millis();
  const bool writeOk = (written == static_cast<size_t>(requestLen));

  if constexpr (kDiag) {
    diagPrintf(
        "[%7.2f s] TG: etapa2 escrita %u/%u B ok=%d em=%lu ms offset=%s\n",
        static_cast<double>(millis()) / 1000.0, static_cast<unsigned>(written),
        static_cast<unsigned>(requestLen), writeOk ? 1 : 0,
        static_cast<unsigned long>(writeDone - writeStart), offsetText);
  }

  // DIAG: etapa 3 - leitura da resposta (duracao total e ate o 1o byte).
  const uint32_t readStart = millis();
  const bool framed =
      writeOk && readResponse(deadline, g_body, sizeof(g_body), bodyLen, status);
  const uint32_t readDone = millis();

  if (!writeOk || !framed) {
    Serial.printf(
        "[%7.2f s] TG: getUpdates sem resposta completa (%lu ms)\n",
        static_cast<double>(nowMs) / 1000.0,
        static_cast<unsigned long>(elapsedMs(start)));

    if constexpr (kDiag) {
      diagPrintf(
          "[%7.2f s] TG: falha etapa=%s causa=%s status=%d lidos=%u "
          "1o_byte=%ld ms leitura=%lu ms\n",
          static_cast<double>(millis()) / 1000.0,
          g_failStage != nullptr ? g_failStage : "?",
          g_failCause != nullptr ? g_failCause : "?", status,
          static_cast<unsigned>(g_rxTotal),
          g_rxFirstMs != 0 ? static_cast<long>(g_rxFirstMs - writeDone) : -1L,
          static_cast<unsigned long>(readDone - readStart));
      diagConnState("falha");
      if (g_rxTotal > 0) {
        char hex[110];
        diagHexHead(hex, sizeof(hex));
        diagPrintf("[%7.2f s] TG: rx primeiros %u B: %s (total %u)\n",
                   static_cast<double>(millis()) / 1000.0,
                   static_cast<unsigned>(g_rxHeadLen), hex,
                   static_cast<unsigned>(g_rxTotal));
      }
    }

    ++m_failures;
    closeConnection();
    return;
  }

  if constexpr (kDiag) {
    diagPrintf(
        "[%7.2f s] TG: etapa3 leitura ok em=%lu ms (1o byte em %ld ms) "
        "status=%d lidos=%u corpo=%u\n",
        static_cast<double>(millis()) / 1000.0,
        static_cast<unsigned long>(readDone - readStart),
        g_rxFirstMs != 0 ? static_cast<long>(g_rxFirstMs - writeDone) : -1L,
        status, static_cast<unsigned>(g_rxTotal),
        static_cast<unsigned>(bodyLen));
  }

  if (status != 200) {
    Serial.printf("[%7.2f s] TG: getUpdates HTTP %d\n",
                  static_cast<double>(nowMs) / 1000.0, status);
    ++m_failures;
    closeConnection();
    return;
  }

  StaticJsonDocument<kFilterCapacity> filter;
  buildUpdateFilter(filter.to<JsonObject>());

  StaticJsonDocument<kDocCapacity> doc;
  const DeserializationError jsonError = deserializeJson(
      doc, g_body, DeserializationOption::Filter(filter));
  if (jsonError) {
    Serial.printf("[%7.2f s] TG: JSON invalido (%s)\n",
                  static_cast<double>(nowMs) / 1000.0, jsonError.c_str());
    ++m_failures;
    closeConnection();
    return;
  }

  if (!(doc["ok"] | false)) {
    Serial.printf("[%7.2f s] TG: API recusou a consulta (error_code=%d)\n",
                  static_cast<double>(nowMs) / 1000.0, doc["error_code"] | 0);
    ++m_failures;
    closeConnection();
    return;
  }

  // Consulta bem-sucedida (HTTP 200 + ok:true): alimenta a guarda de bloqueio.
  m_lastSuccessMs = millis();

  JsonArray results = doc["result"].as<JsonArray>();
  if (results.isNull() || results.size() == 0) {
    m_firstPoll = false;  // nada novo: o proximo polling ja usa o offset
    endPoll();
    return;
  }

  // Um unico update por consulta (limit=1; o modulo nunca processa o lote).
  JsonObject update = results[0];
  const int64_t updateId = update["update_id"] | 0LL;

  if (m_firstPoll) {
    // Backlog anterior ao boot: descarta e passa a acompanhar a partir dele.
    m_updateOffset = updateId + 1;
    m_firstPoll = false;
    Serial.printf("TG: backlog descartado (update_id=%lld, offset=%lld)\n",
                  static_cast<long long>(updateId),
                  static_cast<long long>(m_updateOffset));
    endPoll();
    return;
  }

  // O offset avanca ANTES de responder: uma falha no envio nunca reenvia a
  // mesma mensagem (o pedido simplesmente nao recebe resposta).
  m_updateOffset = updateId + 1;

  const int64_t chatId = update["message"]["chat"]["id"] | 0LL;
  const char *text = update["message"]["text"] | "";
  if (text[0] == '\0') {
    endPoll();  // foto, sticker, entrada/saida de membro...
    return;
  }

  if (!isAuthorizedChat(chatId)) {
    Serial.printf(
        "[%7.2f s] TG: mensagem de chat nao autorizado ignorada "
        "(update_id=%lld)\n",
        static_cast<double>(nowMs) / 1000.0, static_cast<long long>(updateId));
    endPoll();
    return;
  }

  const TgCommand command =
      parseTgCommand(text, g_normalized, sizeof(g_normalized));
  if (command == TgCommand::None) {
    Serial.printf("[%7.2f s] TG: comando desconhecido ignorado\n",
                  static_cast<double>(nowMs) / 1000.0);
    endPoll();
    return;
  }

  answerCommand(command, context, chatId);  // usa a sessao TLS ja aberta
  endPoll();
}

// ---------------------------------------------------------------------------
//  Respostas
// ---------------------------------------------------------------------------
// Nome do comando para o log (o texto cru recebido nao e guardado em lugar
// nenhum alem do buffer normalizado).
const char *commandName(TgCommand command) {
  switch (command) {
    case TgCommand::Help:
      return "/help";
    case TgCommand::Status:
      return "/status";
    case TgCommand::Warnings:
      return "/warnings";
    case TgCommand::None:
    default:
      return "?";
  }
}

void Telegram::answerCommand(TgCommand command, const TelegramContext &context,
                             int64_t chatId) {
  if (command == TgCommand::Help) {
    buildHelpMessage(g_message, sizeof(g_message));
  } else if (command == TgCommand::Status) {
    TelegramStatusData data;
    buildStatusData(context, data);
    buildStatusMessage(data, FIRMWARE_VERSION, context.uptimeS, g_message,
                       sizeof(g_message));
  } else if (command == TgCommand::Warnings) {
    AlertMonitor *alerts = context.alerts;
    if (alerts == nullptr) {
      snprintf(g_message, sizeof(g_message),
               PSTR("Alertas de nível indisponíveis."));
    } else {
      // Os limites vigentes ficam guardados para a mensagem de erro: um /w
      // recusado NAO pode alterar nada.
      const alerts::Config current = alerts->config();
      alerts::Config requested;
      const WarningsParse parsed =
          parseWarnings(tgArgsOf(g_normalized), requested);

      if (parsed == WarningsParse::SHOW) {
        buildWarningsMessage(current, alerts->level(), context.volumeL,
                             context.sampleValid, g_message, sizeof(g_message));
      } else if (parsed == WarningsParse::INVALID) {
        buildWarningsErrorMessage(PSTR("informe três números: /w lr lc hist"),
                                  current, g_message, sizeof(g_message));
      } else {
        const alerts::ConfigError error = alerts->applyConfig(
            requested, context.volumeL, context.sampleValid);
        if (error == alerts::ConfigError::OK) {
          // Reapresenta os limites EFETIVADOS e o estado JA recalculado.
          buildWarningsAppliedMessage(alerts->config(), alerts->level(),
                                      context.volumeL, context.sampleValid,
                                      g_message, sizeof(g_message));
        } else {
          buildWarningsErrorMessage(warningsErrorText(error), current,
                                    g_message, sizeof(g_message));
        }
      }
    }
  } else {
    return;
  }

  const uint32_t start = millis();
  if (sendMessage(chatId, g_message)) {
    ++m_commands;
    Serial.printf("[%7.2f s] TG: %s respondido (%lu ms)\n",
                  static_cast<double>(millis()) / 1000.0, commandName(command),
                  static_cast<unsigned long>(elapsedMs(start)));
  } else {
    ++m_failures;
    // DIAG: estado da sessao no instante da falha, antes do closeConnection().
    if constexpr (kDiag) {
      diagConnState("falha-sendMessage");
    }
    Serial.printf("[%7.2f s] TG: falha ao enviar a resposta\n",
                  static_cast<double>(millis()) / 1000.0);
  }
}

// POST sendMessage com o corpo application/x-www-form-urlencoded montado em
// g_form (chat_id + texto codificado). A resposta e pequena e cabe em g_body.
bool Telegram::sendMessage(int64_t chatId, const char *text) {
  const int prefix = snprintf(g_form, sizeof(g_form), "chat_id=%lld&text=",
                              static_cast<long long>(chatId));
  if (prefix <= 0 || static_cast<size_t>(prefix) >= sizeof(g_form)) {
    return false;
  }

  const size_t encodedLen =
      urlEncodeInto(g_form + prefix, sizeof(g_form) - static_cast<size_t>(prefix),
                    text);
  if (encodedLen == 0) {
    Serial.println("TG: mensagem nao cabe no corpo do POST (descartada)");
    return false;
  }
  const size_t formLen = static_cast<size_t>(prefix) + encodedLen;

  const int requestLen = snprintf(
      g_request, sizeof(g_request),
      "POST /bot%s/sendMessage HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Content-Type: application/x-www-form-urlencoded\r\n"
      "Content-Length: %u\r\n"
      "Connection: keep-alive\r\n\r\n",
      ACTIVE_TELEGRAM_BOT_TOKEN, TELEGRAM_HOST,
      static_cast<unsigned>(formLen));
  if (requestLen <= 0 ||
      static_cast<size_t>(requestLen) >= sizeof(g_request)) {
    Serial.println("TG: requisicao sendMessage nao cabe no buffer");
    return false;
  }

  if (writeAll(g_request, static_cast<size_t>(requestLen)) !=
          static_cast<size_t>(requestLen) ||
      writeAll(g_form, formLen) != formLen) {
    return false;
  }

  const uint32_t deadline = millis() + TELEGRAM_RESPONSE_TIMEOUT_MS;
  int status = 0;
  size_t bodyLen = 0;
  diagResetRx();
  if (!readResponse(deadline, g_body, sizeof(g_body), bodyLen, status)) {
    return false;
  }
  if (status != 200) {
    Serial.printf("TG: sendMessage HTTP %d\n", status);
    return false;
  }

  return strstr(g_body, "\"ok\":true") != nullptr;
}


