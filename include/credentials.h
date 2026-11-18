#pragma once

// ============================================================================
//  Selecao central de credenciais
//
//  Este e o UNICO lugar do projeto que escolhe entre producao e teste. Tudo
//  deriva da constante TEST_MODE (config.h): nenhum modulo decide ambiente por
//  conta propria, apenas consome os ACTIVE_* definidos aqui.
//
//  Os valores brutos ficam em include/secrets_test.h e include/secrets_prod.h,
//  que NAO sao versionados (modelos: *.example).
// ============================================================================

#include "config.h"
#include "secrets_prod.h"
#include "secrets_test.h"

// Escolha unica e explicita entre o valor de producao e o de teste. Fica como
// funcao constexpr (e nao if constexpr) porque o resultado inicializa
// constantes no escopo de namespace, onde if constexpr nao se aplica.
template <typename T>
constexpr T selectValue(bool testMode, T production, T test) {
  return testMode ? test : production;
}

constexpr const char *ACTIVE_WIFI_SSID =
    selectValue(TEST_MODE, PROD_WIFI_SSID, TEST_WIFI_SSID);
constexpr const char *ACTIVE_WIFI_PASSWORD =
    selectValue(TEST_MODE, PROD_WIFI_PASSWORD, TEST_WIFI_PASSWORD);
constexpr const char *ACTIVE_MQTT_HOST =
    selectValue(TEST_MODE, PROD_MQTT_HOST, TEST_MQTT_HOST);
constexpr uint16_t ACTIVE_MQTT_PORT =
    selectValue<uint16_t>(TEST_MODE, PROD_MQTT_PORT, TEST_MQTT_PORT);
constexpr const char *ACTIVE_TB_TOKEN =
    selectValue(TEST_MODE, PROD_TB_TOKEN, TEST_TB_TOKEN);

// ---------------------------------------------------------------------------
//  Credenciais pendentes
//
//  Os arquivos de segredo sao entregues com placeholders "COLOQUE_..." para que
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

// Nome do arquivo de segredos do ambiente ativo, para as mensagens de Serial.
constexpr const char *activeSecretsFileName() {
  return TEST_MODE ? "include/secrets_test.h" : "include/secrets_prod.h";
}
