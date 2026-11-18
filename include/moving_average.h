#pragma once

#include <stddef.h>

// ============================================================================
//  Media movel de janela fixa, com soma acumulada (O(1) por amostra).
//
//  Enquanto a janela nao estiver completa, apenas as posicoes ja preenchidas
//  entram na media. Depois de cheia, a amostra mais antiga e descartada e a
//  soma e corrigida, sem percorrer o buffer.
// ============================================================================
class MovingAverage {
 public:
  static constexpr size_t kMaxWindow = 50;

  explicit MovingAverage(size_t window = kMaxWindow);

  void add(float value);
  float average() const;
  size_t sampleCount() const { return m_count; }
  bool full() const { return m_count == m_window; }
  void reset();

 private:
  float m_buffer[kMaxWindow];
  size_t m_window;
  size_t m_next;   // proxima posicao a escrever
  size_t m_count;  // amostras validas (satura em m_window)
  float m_sum;     // soma das amostras validas
};
