#include "display.h"

#include <Arduino.h>
#include <math.h>

DisplayManager::DisplayManager()
    : m_display(PIN_DISPLAY_CLK, PIN_DISPLAY_DIO),
      m_liters(0.0f),
      m_on(true),
      m_buttonLatched(false),
      m_lastRawButtonLevel(HIGH),
      m_lastRawChangeMs(0),
      m_lastRefreshMs(0),
      m_onSinceMs(0) {}

void DisplayManager::begin(uint32_t nowMs) {
  m_display.begin();
  m_display.setBrightness(DISPLAY_BRIGHTNESS, true);

  // Programa original: display.showString("Inicio") no setup.
  m_display.showString(DISPLAY_BOOT_TEXT);

  // O original nao configurava o pino; o nivel de repouso vem do circuito
  // externo. O pull-up interno so e habilitado se configurado.
  pinMode(PIN_BUTTON, BUTTON_USE_INTERNAL_PULLUP ? INPUT_PULLUP : INPUT);
  m_lastRawButtonLevel = digitalRead(PIN_BUTTON);
  m_lastRawChangeMs = nowMs;
  m_buttonLatched = false;

  m_on = true;
  m_onSinceMs = nowMs;
  // Primeira indicacao de volume apenas no primeiro ciclo de 4 s, como no
  // programa original (a mensagem de boot permanece visivel nesse intervalo).
  m_lastRefreshMs = nowMs;
}

void DisplayManager::setLiters(float liters) { m_liters = liters; }

void DisplayManager::update(uint32_t nowMs) {
  handleButton(nowMs);

  if (m_on && (nowMs - m_onSinceMs) >= DISPLAY_ON_TIME_MS) {
    powerOff();
  }

  if (m_on && (nowMs - m_lastRefreshMs) >= DISPLAY_REFRESH_MS) {
    render();
    m_lastRefreshMs = nowMs;
  }
}

void DisplayManager::handleButton(uint32_t nowMs) {
  const int level = digitalRead(PIN_BUTTON);

  if (level != m_lastRawButtonLevel) {
    m_lastRawButtonLevel = level;
    m_lastRawChangeMs = nowMs;
  }

  // Acionado = nivel estavel igual ao do circuito original (HIGH) por
  // BUTTON_DEBOUNCE_MS.
  const bool pressed = (level == BUTTON_ACTIVE_LEVEL) &&
                       ((nowMs - m_lastRawChangeMs) >= BUTTON_DEBOUNCE_MS);

  if (!pressed) {
    m_buttonLatched = false;
    return;
  }

  // Equivale a "tempo_display = 0" do programa original: enquanto o botao
  // estiver acionado, o contador dos 28 s permanece reiniciado.
  m_onSinceMs = nowMs;

  if (!m_on) {
    m_on = true;
    m_display.setBrightness(DISPLAY_BRIGHTNESS, true);
  }

  if (!m_buttonLatched) {
    m_buttonLatched = true;
    // Reprograma a indicacao imediatamente ao religar (o original tambem
    // reapresentava o valor no mesmo ciclo do botao).
    render();
    m_lastRefreshMs = nowMs;
  }
}

void DisplayManager::render() {
  // Mesmo recurso do programa original: showNumberDec(volume, dots, false, 6).
  // O volume era long no original; aqui a media e float, entao arredonda-se
  // para o litro mais proximo antes de exibir.
  long liters = lroundf(m_liters);
  if (liters < 0) {
    liters = 0;
  } else if (liters > DISPLAY_MAX_VALUE) {
    liters = DISPLAY_MAX_VALUE;
  }

  // Abaixo de 1000 L: sem ponto. A partir de 1000 L: ponto no 3o digito
  // fisico, funcionando como separador de milhar ("1.234", "12.345").
  const uint8_t dots =
      (liters < DISPLAY_DOT_MIN_LITERS) ? 0 : DISPLAY_THOUSANDS_DOT;

  m_display.showNumberDec(liters, dots, false, DISPLAY_DIGITS);
}

void DisplayManager::powerOff() {
  // Recurso do programa original: brilho 0 com enable desligado apaga os
  // segmentos e mantem o conteudo no controlador.
  m_display.setBrightness(DISPLAY_BRIGHTNESS, false);
  m_on = false;
}
