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


class PublishGate:
    """Copia da resolucao de ThingsBoard::update()/enqueue().

    Reproduz a separacao entre decisao (sempre, com ou sem transporte) e
    transporte (condicional): sem sessao MQTT ou com backlog pendente a amostra
    vira registro na fila, e o baseline da politica so avanca depois de a
    gravacao ser confirmada.
    """

    def __init__(self, delta=50.0, heartbeat_ms=3600000, min_interval_ms=10000):
        self.delta = F32(delta)
        self.heartbeat_ms = heartbeat_ms
        self.min_interval_ms = min_interval_ms
        self.has_published = False
        self.publish_after_reconnect = False
        self.last_published = F32(0.0)
        self.last_publish_ms = 0
        self.pending = 0        # registros pendentes na fila offline
        self.append_ok = True   # LittleFS disponivel / gravacao confirmada
        self.published = 0
        self.enqueued = 0
        self.last_reason = 'NONE'

    def connection_lost(self):
        self.publish_after_reconnect = True

    def _register(self, now_ms, volume):
        self.has_published = True
        self.last_published = F32(volume)
        self.last_publish_ms = now_ms

    def _enqueue(self, now_ms, volume):
        if not self.append_ok:
            return False
        self._register(now_ms, volume)
        self.publish_after_reconnect = False
        self.pending += 1
        self.enqueued += 1
        return True

    def step(self, now_ms, volume, online):
        self.last_reason = decide_publish({
            'sampleValid': True,
            'hasPublished': self.has_published,
            'publishAfterReconnect': self.publish_after_reconnect,
            'volumeL': F32(volume),
            'lastPublishedL': self.last_published,
            'nowMs': now_ms,
            'lastPublishMs': self.last_publish_ms,
            'deltaLiters': self.delta,
            'heartbeatMs': self.heartbeat_ms,
            'minIntervalMs': self.min_interval_ms,
        })
        if self.last_reason == 'NONE':
            return self.last_reason
        if online and self.pending == 0:
            self._register(now_ms, volume)
            self.publish_after_reconnect = False
            self.published += 1
            return self.last_reason
        self._enqueue(now_ms, volume)
        return self.last_reason


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

    print('\n== offline: decisao independente do transporte ==')
    gate = PublishGate()
    gate.step(1000, 1000.0, True)
    check(gate.published == 1 and gate.pending == 0,
          'online: FIRST publica e a fila fica vazia')

    gate.connection_lost()
    check(gate.step(2000, 1000.0, False) == 'RECONNECT',
          'offline: politica decide RECONNECT')
    check(gate.pending == 1, 'offline: RECONNECT vira 1 registro')
    check(not gate.publish_after_reconnect,
          'offline: RECONNECT nao permanece como estado continuo')

    for step in range(39):
        gate.step(2250 + 250 * step, 1005.0 if step % 2 else 1000.0, False)
    check(gate.pending == 1,
          'offline ~10 s sem variacao: nenhum registro por ciclo de 250 ms')

    gate.step(12000, 1080.0, False)
    check(gate.last_reason == 'DELTA' and gate.pending == 2,
          'offline: DELTA apos o intervalo minimo enfileira')
    gate.step(12250, 1140.0, False)
    check(gate.pending == 2, 'offline: DELTA antes do intervalo minimo espera')
    gate.step(22000, 1140.0, False)
    check(gate.pending == 3, 'offline: DELTA apos o intervalo enfileira de novo')

    hb = PublishGate()
    hb.step(1000, 500.0, True)
    hb.connection_lost()
    hb.step(2000, 500.0, False)
    check(hb.pending == 1, 'heartbeat: registro de reconexao enfileirado')
    hb.step(2000 + 3600000, 500.0, False)
    check(hb.last_reason == 'HEARTBEAT' and hb.pending == 2,
          'offline: heartbeat enfileira mesmo sem variacao')

    bad = PublishGate()
    bad.step(1000, 500.0, True)
    bad.connection_lost()
    bad.append_ok = False
    bad.step(2000, 500.0, False)
    check(bad.pending == 0, 'falha de gravacao: nenhum registro criado')
    check(bad.publish_after_reconnect,
          'falha de gravacao: RECONNECT continua pendente')
    check(bad.last_publish_ms == 1000,
          'falha de gravacao: baseline da politica nao avanca')
    bad.append_ok = True
    bad.step(2250, 500.0, False)
    check(bad.pending == 1 and not bad.publish_after_reconnect,
          'retentativa: grava uma unica vez e libera o RECONNECT')

    backlog = PublishGate()
    backlog.step(1000, 500.0, True)
    backlog.connection_lost()
    backlog.step(2000, 500.0, False)
    backlog.step(22000, 620.0, False)
    backlog.step(33000, 700.0, True)
    check(backlog.pending == 3 and backlog.published == 1,
          'reconexao com backlog: leitura nova entra na fila (ordem preservada)')
    backlog.pending = 0
    backlog.step(45000, 780.0, True)
    check(backlog.published == 2, 'fila vazia: volta a publicar direto')

    boot = PublishGate()
    boot.step(1000, 400.0, False)
    check(boot.pending == 1 and boot.published == 0,
          'primeira leitura sem conexao: vai para a fila (FIRST)')

    print('\n%s (%d falha(s))'
          % ('TODOS OS TESTES PASSARAM' if failures == 0 else 'HOUVE FALHAS', failures))
    return 0 if failures == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main())
