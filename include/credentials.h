#pragma once

// ============================================================================
//  Selecao central de credenciais
//
//  Este e o UNICO lugar do projeto que escolhe entre producao e teste. Tudo
//  deriva da constante TEST_MODE (config.h): nenhum modulo decide ambiente por
//  conta propria, apenas consome os ACTIVE_* definidos aqui.
//
//  Os valores brutos ficam em include/secrets.h (unico arquivo de credenciais,
//  no mesmo padrao do Monitora_DG), que NAO e versionado (modelo:
//  include/secrets.h.example).
// ============================================================================

#include "config.h"
#include "secrets.h"

// Escolha unica e explicita entre o valor de producao e o de teste. Fica como
// funcao constexpr (e nao if constexpr) porque o resultado inicializa
// constantes no escopo de namespace, onde if constexpr nao se aplica.
template <typename T>
constexpr T selectValue(bool testMode, T production, T test) {
  return testMode ? test : production;
}

constexpr const char *ACTIVE_WIFI_SSID =
    selectValue(TEST_MODE, WIFI_SSID_PROD, WIFI_SSID_TEST);
constexpr const char *ACTIVE_WIFI_PASSWORD =
    selectValue(TEST_MODE, WIFI_PASSWORD_PROD, WIFI_PASSWORD_TEST);
constexpr const char *ACTIVE_TB_TOKEN =
    selectValue(TEST_MODE, TB_ACCESS_TOKEN_PROD, TB_ACCESS_TOKEN_TEST);

// Host e porta do broker sao unicos para os dois ambientes (MQTT sobre TLS).
// Nao existe porta 1883 no MoniRes: texto puro nao e usado.
static_assert(TB_PORT == 8883, "TB_PORT deve ser 8883 (MQTT sobre TLS)");

// ---------------------------------------------------------------------------
//  Credenciais pendentes
//
//  O arquivo de segredos e entregue com placeholders "COLOQUE_..." para que
//  o projeto compile antes de o usuario informar os valores reais. Enquanto
//  isso, a comunicacao nao tenta conectar e o Serial avisa - em vez de ficar
//  tentando indefinidamente com um valor invalido.
// ---------------------------------------------------------------------------
constexpr bool hasPlaceholderPrefix(const char *value) {
  constexpr char kPrefix[] = "COLOQUE_";
  if (value == nullptr) {
    return true;
  }
  for (size_t i = 0; kPrefix[i] != '\0'; ++i) {
    if (value[i] != kPrefix[i]) {
      return false;
    }
  }
  return true;
}

constexpr bool wifiCredentialsPending() {
  return ACTIVE_WIFI_SSID[0] == '\0' || hasPlaceholderPrefix(ACTIVE_WIFI_SSID);
}

constexpr bool tbTokenPending() {
  return ACTIVE_TB_TOKEN[0] == '\0' || hasPlaceholderPrefix(ACTIVE_TB_TOKEN);
}

// Nome do arquivo de segredos, para as mensagens de Serial. Existe apenas um
// arquivo para os dois ambientes.
constexpr const char *activeSecretsFileName() {
  return "include/secrets.h";
}
