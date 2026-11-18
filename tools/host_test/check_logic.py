"""Verificacao numerica da logica de src/moving_average.cpp e src/volume.cpp.

Este PC nao possui compilador C++ nativo (apenas o cross-compiler xtensa do
ESP8266), portanto a conferencia dos valores e feita aqui, reproduzindo passo a
passo o mesmo algoritmo com precisao float32 (struct 'f'), igual ao 'float' do
ESP8266. O teste C++ equivalente esta em tools/host_test/hosttest.cpp.
"""
import struct

F32 = lambda x: struct.unpack('f', struct.pack('f', float(x)))[0]


class MovingAverage:
    """Copia de include/moving_average.h + src/moving_average.cpp."""

    def __init__(self, window=50):
        self.k_max = 50
        self.window = min(max(window, 1), self.k_max)
        self.buf = [0.0] * self.k_max
        self.next = 0
        self.count = 0
        self.sum = F32(0.0)

    def add(self, value):
        if self.count == self.window:
            self.sum = F32(self.sum - self.buf[self.next])
        else:
            self.count += 1
        self.buf[self.next] = F32(value)
        self.sum = F32(self.sum + F32(value))
        self.next += 1
        if self.next >= self.window:
            self.next = 0

    def average(self):
        if self.count == 0:
            return F32(0.0)
        return F32(self.sum / F32(self.count))


def volume_compute(adc):
    """Copia de src/volume.cpp (namespace volume, compute)."""
    v_ref, adc_max, fs_v, fs_mm = 3.3, 1023.0, 1.1, 1000.0
    offset, corr, lpm = 50.0, 1.05, 41.52
    voltage = F32(F32(adc) * F32(v_ref / adc_max))
    sensor_mm = F32(F32(voltage / fs_v) * fs_mm)
    level = F32(F32(sensor_mm - offset) / corr)
    if level < 0.0:
        level = F32(0.0)
    liters = F32(level * lpm)
    return voltage, sensor_mm, level, liters


def original_liters(adc):
    """Cadeia do programa original, com os tipos inteiros que ele usava."""
    tensao = adc * (3.3 / 1023.0)
    leitura = int(tensao / 1.1 * 1000.0)
    nivel = int((leitura - 50) / 1.05)
    if nivel < 0:
        nivel = 0
    return int(nivel * 41.52)


def decide_publish(inp):
    """Copia de include/publish_policy.h (publish_policy::decide)."""
    if not inp['sampleValid']:
        return 'NONE'
    if not inp['hasPublished']:
        return 'FIRST'
    if inp['publishAfterReconnect']:
        return 'RECONNECT'
    since = (inp['nowMs'] - inp['lastPublishMs']) & 0xFFFFFFFF
    changed = abs(F32(inp['volumeL'] - inp['lastPublishedL'])) >= inp['deltaLiters']
    if changed and since >= inp['minIntervalMs']:
        return 'DELTA'
    if since >= inp['heartbeatMs']:
        return 'HEARTBEAT'
    return 'NONE'


def main():
    failures = 0

    def check(ok, label):
        nonlocal failures
        print(('  [ok]   ' if ok else '  [FALHA] ') + label)
        if not ok:
            failures += 1

    print('== MovingAverage ==')
    mv = MovingAverage(50)
    check(mv.count == 0 and mv.average() == 0.0,
          'buffer zerado: contagem 0 e media 0')

    mv.add(10.0)
    check(abs(mv.average() - 10.0) < 1e-6 and mv.count == 1,
          '1 amostra: media = 10 (posicoes nao preenchidas ignoradas)')
    mv.add(20.0)
    check(abs(mv.average() - 15.0) < 1e-6, '2 amostras: media = 15')

    full = MovingAverage(50)
    for i in range(1, 51):
        full.add(float(i))
    check(full.count == 50 and abs(full.average() - 25.5) < 1e-5,
          '50 amostras 1..50: media = 25,5 e janela cheia')

    full.add(100.0)
    check(abs(full.average() - (1275.0 - 1.0 + 100.0) / 50.0) < 1e-5,
          'janela cheia: descarta a mais antiga (media = 27,48)')

    cont = MovingAverage(50)
    history, idx, seen = [0.0] * 50, 0, 0
    rng, worst = 0x12345678, 0.0
    for _ in range(20000):
        rng ^= (rng << 13) & 0xFFFFFFFF
        rng ^= rng >> 17
        rng ^= (rng << 5) & 0xFFFFFFFF
        rng &= 0xFFFFFFFF
        value = float(rng % 100000) + 0.5
        cont.add(value)
        history[idx] = value
        idx = (idx + 1) % 50
        if seen < 50:
            seen += 1
        naive = sum(history[:seen]) / seen
        worst = max(worst, abs(naive - cont.average()))
    print('        maior desvio vs media ingenua em 20000 passos: %.6f L' % worst)
    check(worst < 0.02, 'soma acumulada equivale a media ingenua (O(1))')

    cont.count = 0
    cont.next = 0
    cont.sum = F32(0.0)
    check(cont.count == 0 and cont.average() == 0.0,
          'reset(): contagem e media voltam a zero')

    small = MovingAverage(5)
    for i in range(1, 10):
        small.add(float(i))
    check(small.count == 5 and abs(small.average() - 7.0) < 1e-6,
          'janela de 5 amostras (5..9 apos descartar 1..4)')

    print('\n== volume::compute ==')
    v, s, lv, li = volume_compute(0)
    check(li == 0.0 and lv == 0.0, 'ADC 0 -> volume 0')
    v, s, lv, li = volume_compute(1023)
    check(abs(v - 3.3) < 1e-4 and abs(s - 3000.0) < 1e-3
          and abs(lv - (3000.0 - 50.0) / 1.05) < 1e-2
          and abs(li - (3000.0 - 50.0) / 1.05 * 41.52) < 0.1,
          'ADC 1023 -> 3,3 V | 3000 mm | nivel 2809,52 mm | 116651,4 L')
    v, s, lv, li = volume_compute(17)
    check(lv == 0.0 and li == 0.0 and s < 50.0,
          'abaixo do offset: nivel e volume limitados a zero (CRITICAL)')

    print('\n== comparacao com o programa original (inteiros) ==')
    worst = 0.0
    for adc in (17, 100, 342, 500, 750, 900, 1023):
        _, _, level, liters = volume_compute(adc)
        ref = original_liters(adc)
        worst = max(worst, abs(liters - ref))
        print('        ADC %4d: novo=%9.1f L   original=%9d L   dif=%+.1f L'
              % (adc, liters, ref, liters - ref))
    print('        maior diferenca: %.1f L (truncamento dos long do original)' % worst)

    print('\n== publish_policy::decide (Etapa 2) ==')
    policy = {
        'sampleValid': False, 'hasPublished': False, 'publishAfterReconnect': False,
        'volumeL': 1000.0, 'lastPublishedL': 1000.0, 'nowMs': 2000,
        'lastPublishMs': 1000, 'deltaLiters': 50.0,
        'heartbeatMs': 3600000, 'minIntervalMs': 10000,
    }

    def policy_case(expected, label, **changes):
        policy.update(changes)
        got = decide_publish(policy)
        check(got == expected, '%s -> %s (esperado %s)' % (label, got, expected))

    policy_case('NONE', 'sem leitura valida')
    policy_case('FIRST', 'primeira leitura valida', sampleValid=True)
    policy_case('NONE', 'volume igual, heartbeat distante', hasPublished=True)
    policy_case('NONE', 'variacao de 49 L (< delta)', volumeL=1049.0)
    policy_case('NONE', 'delta atingido antes do intervalo minimo', volumeL=1050.0)
    policy_case('DELTA', 'delta de 50 L apos 10 s', nowMs=11000)
    policy_case('HEARTBEAT', '60 min sem mudanca', nowMs=1000 + 3600000, volumeL=1000.0)
    policy_case('RECONNECT', 'reconexao publica imediatamente', nowMs=2000,
                publishAfterReconnect=True)

    print('\n%s (%d falha(s))'
          % ('TODOS OS TESTES PASSARAM' if failures == 0 else 'HOUVE FALHAS', failures))
    return 0 if failures == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
