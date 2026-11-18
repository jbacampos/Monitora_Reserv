#include "moving_average.h"

MovingAverage::MovingAverage(size_t window)
    : m_window(window), m_next(0), m_count(0), m_sum(0.0f) {
  if (m_window == 0) {
    m_window = 1;
  } else if (m_window > kMaxWindow) {
    m_window = kMaxWindow;
  }
  reset();
}

void MovingAverage::reset() {
  // Zera o buffer para que nenhuma posicao invalida participe da media.
  for (size_t i = 0; i < kMaxWindow; ++i) {
    m_buffer[i] = 0.0f;
  }
  m_next = 0;
  m_count = 0;
  m_sum = 0.0f;
}

void MovingAverage::add(float value) {
  if (m_count == m_window) {
    // Janela cheia: remove a amostra mais antiga antes de sobrescreve-la.
    m_sum -= m_buffer[m_next];
  } else {
    ++m_count;
  }

  m_buffer[m_next] = value;
  m_sum += value;

  ++m_next;
  if (m_next >= m_window) {
    m_next = 0;
  }
}

float MovingAverage::average() const {
  if (m_count == 0) {
    return 0.0f;
  }
  return m_sum / static_cast<float>(m_count);
}
