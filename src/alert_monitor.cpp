#include "alert_monitor.h"

#include <LittleFS.h>

#include "config.h"
#include "telegram_text.h"

// ============================================================================
//  Alertas de nivel - implementacao
//
//  Persistencia: dois arquivos de 32 B (A/B) com CRC32 e geracao, escolhidos no
//  boot pelo maior numero de geracao valido - mesmo padrao da tabela A/B do
//  historico. A gravacao sempre vai para o slot que NAO esta ativo; se falhar, o
//  slot anterior continua valido e o estado em RAM nao muda.
// ============================================================================
namespace {

alerts::Config defaultConfig() {
  alerts::Config cfg;
  cfg.riskPct = ALERT_DEFAULT_RISK_PCT;
  cfg.criticalPct = ALERT_DEFAULT_CRITICAL_PCT;
  cfg.histPct = ALERT_DEFAULT_HIST_PCT;
  return cfg;
}

const char *levelName(alerts::Level level) {
  switch (level) {
    case alerts::Level::RISK:
      return PSTR("Atenção");
    case alerts::Level::CRITICAL:
      return PSTR("Critico");
    case alerts::Level::NORMAL:
    default:
      return PSTR("Normal");
  }
}

const char *eventName(alerts::Event event) {
  switch (event) {
    case alerts::Event::RISK_ENTERED:
      return PSTR("nivel de atencao");
    case alerts::Event::CRITICAL_ENTERED:
      return PSTR("nivel critico");
    case alerts::Event::CRITICAL_RECOVERED:
      return PSTR("recuperado do critico");
    case alerts::Event::RISK_RECOVERED:
      return PSTR("recuperado da atencao");
    case alerts::Event::NONE:
    default:
      return PSTR("?");
  }
}

}  // namespace

void AlertMonitor::begin(uint32_t nowMs) {
  m_bootMs = nowMs;
  m_state = alerts::Persisted();
  m_state.config = defaultConfig();
  m_rebaselined = false;
  load();
}

const char *AlertMonitor::levelText() const { return levelName(m_state.level); }

void AlertMonitor::load() {
  FSInfo info = {};
  if (!LittleFS.info(info)) {
    m_persisted = false;
    if (!m_fsWarned) {
      m_fsWarned = true;
      Serial.println(
          PSTR("Alertas: LittleFS indisponivel; limites e estado em RAM ate o "
               "proximo reboot."));
    }
    return;
  }

  const char *paths[2] = {ALERT_FILE_A, ALERT_FILE_B};
  uint8_t raw[alerts::RECORD_SIZE];
  bool found = false;
  alerts::Persisted chosen;

  for (uint8_t i = 0; i < 2; ++i) {
    if (!readRecord(paths[i], raw)) {
      continue;
    }
    alerts::Persisted decoded;
    if (!alerts::decodeRecord(raw, decoded)) {
      Serial.printf(PSTR("Alertas: %s invalido (CRC/versao); ignorado.\n"),
                    paths[i]);
      continue;
    }
    if (!found || decoded.generation >= chosen.generation) {
      chosen = decoded;
      m_activeSlot = i;
      found = true;
    }
  }

  if (!found) {
    Serial.printf(
        PSTR("Alertas: sem registro valido; usando atenção=%u%%, critico=%u%%, "
             "histerese=%u%%\n"),
        static_cast<unsigned>(m_state.config.riskPct),
        static_cast<unsigned>(m_state.config.criticalPct),
        static_cast<unsigned>(m_state.config.histPct));
    return;
  }

  m_state = chosen;
  m_persisted = true;
  Serial.printf(
      PSTR("[%7.2f s] Alertas: limites carregados (atenção=%u%%, critico=%u%%, "
           "histerese=%u%%, estado=%s, pendentes=%u, geracao=%lu)\n"),
      static_cast<double>(m_bootMs) / 1000.0,
      static_cast<unsigned>(m_state.config.riskPct),
      static_cast<unsigned>(m_state.config.criticalPct),
      static_cast<unsigned>(m_state.config.histPct), levelName(m_state.level),
      static_cast<unsigned>(m_state.pendingCount),
      static_cast<unsigned long>(m_state.generation));
}

bool AlertMonitor::readRecord(const char *path, uint8_t *raw) const {
  File file = LittleFS.open(path, "r");
  if (!file) {
    return false;
  }
  const size_t read = file.read(raw, alerts::RECORD_SIZE);
  file.close();
  return read == alerts::RECORD_SIZE;
}

void AlertMonitor::store() {
  FSInfo info = {};
  if (!LittleFS.info(info)) {
    m_persisted = false;
    if (!m_fsWarned) {
      m_fsWarned = true;
      Serial.println(PSTR("Alertas: sem LittleFS; estado nao sera preservado."));
    }
    return;
  }

  const uint32_t generation = m_state.generation + 1;
  uint8_t raw[alerts::RECORD_SIZE];
  alerts::encodeRecord(raw, m_state, generation);

  const char *path = (m_activeSlot == 0) ? ALERT_FILE_B : ALERT_FILE_A;
  File file = LittleFS.open(path, "w");
  if (!file) {
    Serial.printf(PSTR("Alertas: falha ao gravar %s; slot anterior preservado.\n"),
                  path);
    return;
  }
  const size_t written = file.write(raw, sizeof(raw));
  file.close();
  if (written != sizeof(raw)) {
    Serial.printf(PSTR("Alertas: gravacao parcial em %s (%u/%u B); ignorada.\n"),
                  path, static_cast<unsigned>(written),
                  static_cast<unsigned>(sizeof(raw)));
    return;
  }

  m_state.generation = generation;
  m_activeSlot = static_cast<uint8_t>(m_activeSlot == 0 ? 1 : 0);
  m_persisted = true;
}

void AlertMonitor::update(uint32_t nowMs, float volumeL, bool sampleValid) {
  if (!sampleValid ||
      alerts::validate(m_state.config) != alerts::ConfigError::OK) {
    return;
  }
  // Sem settle nao ha alerta: o filtro ainda esta vazio (ruido de boot).
  if (static_cast<uint32_t>(nowMs - m_bootMs) < ALERT_SETTLE_MS) {
    return;
  }

  // PRIMEIRA avaliacao util depois do boot: o estado veio da FLASH (ou dos
  // defaults) e o volume pode ter mudado enquanto o aparelho estava fora.
  //   PIOROU  -> nada especial: o step() abaixo emite o alerta devido (degrau
  //              NORMAL->critico ja emite SO o alerta critico) - o operador
  //              precisa saber, mesmo sem ter visto a travessia.
  //   MELHOROU -> rebaseline SILENCIOSO. O aparelho nao viu a recuperacao e o
  //              volume de agora nao e o da travessia, o que produzia
  //              "reservatorio recuperou-se do nivel critico - 100.622 L" logo
  //              apos o reboot. Mesma ideia do /w, que tambem recalcula o estado
  //              sem evento ao trocar os limites.
  if (!m_rebaselined) {
    m_rebaselined = true;
    const alerts::Level now = alerts::levelFor(m_state.config, volumeL);
    if (static_cast<uint8_t>(now) < static_cast<uint8_t>(m_state.level)) {
      Serial.printf(
          PSTR("[%7.2f s] Alertas: estado pos-reboot %s -> %s (%.1f L) [sem evento]\n"),
          static_cast<double>(nowMs) / 1000.0, levelName(m_state.level),
          levelName(now), static_cast<double>(volumeL));
      m_state.level = now;
      store();
    } else if (now != m_state.level) {
      Serial.printf(
          PSTR("[%7.2f s] Alertas: estado pos-reboot %s -> %s (%.1f L) [alerta emitido]\n"),
          static_cast<double>(nowMs) / 1000.0, levelName(m_state.level),
          levelName(now), static_cast<double>(volumeL));
    }
  }

  bool changed = false;
  // Esvazia transicoes em CADEIA no mesmo passo (ex.: recuperou do critico e,
  // logo, da atenção). No maximo MAX_PENDING eventos por avaliacao.
  for (uint8_t i = 0; i < alerts::MAX_PENDING; ++i) {
    const alerts::Event event =
        alerts::step(m_state.level, m_state.config, volumeL);
    if (event == alerts::Event::NONE) {
      break;
    }
    changed = true;
    ++m_events;

    if (!alerts::pushPending(m_state, event, volumeL)) {
      // Fila cheia (envio bloqueado por muito tempo): o alerta MAIS RECENTE
      // importa mais, entao o mais antigo e descartado.
      alerts::popPending(m_state);
      alerts::pushPending(m_state, event, volumeL);
      Serial.println(PSTR("Alertas: fila cheia; alerta mais antigo descartado."));
    }

    Serial.printf(PSTR("[%7.2f s] Alertas: %s (%.1f L)\n"),
                  static_cast<double>(nowMs) / 1000.0, eventName(event),
                  static_cast<double>(volumeL));
  }

  if (changed) {
    store();
  }
}

alerts::ConfigError AlertMonitor::applyConfig(const alerts::Config &cfg,
                                              float volumeL, bool sampleValid) {
  const alerts::ConfigError error = alerts::validate(cfg);
  if (error != alerts::ConfigError::OK) {
    Serial.printf(
        PSTR("[%7.2f s] Alertas: limites recusados (erro=%u); atuais mantidos.\n"),
        static_cast<double>(millis()) / 1000.0, static_cast<unsigned>(error));
    return error;
  }

  const bool changed = !alerts::sameConfig(cfg, m_state.config);
  m_state.config = cfg;
  // Rebaseline SILENCIOSO: o estado passa a refletir o volume atual, sem evento
  // - o operador apenas redefiniu os limites, nao houve transicao de nivel.
  if (sampleValid) {
    m_state.level = alerts::levelFor(cfg, volumeL);
  }
  // Alertas pendentes foram decididos pelos limites antigos: perdem o sentido.
  m_state.pendingCount = 0;
  for (uint8_t i = 0; i < alerts::MAX_PENDING; ++i) {
    m_state.pendingEvent[i] = alerts::Event::NONE;
    m_state.pendingVolumeL[i] = 0.0f;
  }

  // O /w acabou de definir o estado a partir do volume atual: nao existe mais
  // "estado de antes do reboot" para a primeira avaliacao rebaselinar (um /w
  // pode chegar antes do ALERT_SETTLE_MS).
  m_rebaselined = true;

  store();

  const char *result = changed ? PSTR("atualizados") : PSTR("reconfirmados");
  const char *persisted = m_persisted ? PSTR("") : PSTR(" [nao persistido]");
  Serial.printf(
      PSTR("[%7.2f s] Alertas: limites %s (atenção=%u%%, critico=%u%%, "
           "histerese=%u%%, estado=%s)%s\n"),
      static_cast<double>(millis()) / 1000.0, result,
      static_cast<unsigned>(cfg.riskPct),
      static_cast<unsigned>(cfg.criticalPct),
      static_cast<unsigned>(cfg.histPct), levelName(m_state.level), persisted);
  return alerts::ConfigError::OK;
}

bool AlertMonitor::peekPending(float rateLpm, bool hasRate, char *out,
                               size_t cap) const {
  if (out == nullptr || cap == 0 || m_state.pendingCount == 0) {
    return false;
  }
  buildAlertMessage(m_state.pendingEvent[0], m_state.pendingVolumeL[0], rateLpm,
                    hasRate, out, cap);
  return out[0] != '\0';
}

void AlertMonitor::confirmPending() {
  if (m_state.pendingCount == 0) {
    return;
  }
  alerts::popPending(m_state);
  store();
}