"""Verificador da taxa de variacao recente (include/rate.h + include/config.h).

Valida a serie CURTA em RAM (12 pontos de 5 min = 1 h) e a inclinacao por
minimos quadrados, sem device. Le as constantes reais de include/rate.h (header PURO) e falha
se elas mudarem sem que o verificador acompanhe.

O que NAO esta aqui: a medicao em si (a serie usa a media movel publicada, que e
medida por check_logic/hosttest) e a persistencia (nao existe: a serie e perdida
no reboot, de proposito).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CONFIG_HEADER = os.path.join(ROOT, "include", "config.h")
RATE_HEADER = os.path.join(ROOT, "include", "rate.h")
MAIN_CPP = os.path.join(ROOT, "src", "main.cpp")

FAILURES = []


def check(condition, message):
    if condition:
        print("  [ok]   %s" % message)
    else:
        print("  [FALHOU] %s" % message)
        FAILURES.append(message)


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    without_block = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", without_block)


def read_int(path, name):
    match = re.search(r"\b%s\s*=\s*([0-9]+)" % name, read_text(path))
    if match is None:
        raise SystemExit("constante %s nao encontrada em %s" % (name, path))
    return int(match.group(1))


def read_float(path, name):
    match = re.search(r"\b%s\s*=\s*([0-9.]+)f" % name, read_text(path))
    if match is None:
        raise SystemExit("constante %s nao encontrada em %s" % (name, path))
    return float(match.group(1))


SAMPLE_MS = read_int(RATE_HEADER, "RATE_SAMPLE_INTERVAL_MS")
WINDOW_MS = read_int(RATE_HEADER, "RATE_WINDOW_MS")
STABLE_LPH = read_float(RATE_HEADER, "RATE_STABLE_LPH")
MIN_SAMPLES = read_int(RATE_HEADER, "RATE_MIN_SAMPLES")
MIN_SPAN_MS = read_int(RATE_HEADER, "RATE_MIN_SPAN_MS")
SLOTS = WINDOW_MS // SAMPLE_MS
MASK32 = 0xFFFFFFFF


# ---------------------------------------------------------------------------
#  Espelhos de include/rate.h
# ---------------------------------------------------------------------------
def begin(now_ms):
    """Espelho de rate::begin(): o ultimo push fica UM intervalo atras."""
    return {"liters": [0.0] * SLOTS, "ms": [0] * SLOTS, "head": 0, "count": 0,
            "last_push_ms": (now_ms - SAMPLE_MS) & MASK32}


def slot_index(ring, i):
    oldest = (ring["head"] + SLOTS - ring["count"]) % SLOTS
    return (oldest + i) % SLOTS


def has_rate(ring):
    if ring["count"] < MIN_SAMPLES:
        return False
    oldest = ring["ms"][slot_index(ring, 0)]
    newest = ring["ms"][slot_index(ring, ring["count"] - 1)]
    return ((newest - oldest) & MASK32) >= MIN_SPAN_MS


def liters_per_hour(ring):
    if not has_rate(ring):
        return 0.0
    t0 = ring["ms"][slot_index(ring, 0)]
    sx = sy = sxx = sxy = 0.0
    for i in range(ring["count"]):
        idx = slot_index(ring, i)
        x = ((ring["ms"][idx] - t0) & MASK32) / 1000.0
        y = ring["liters"][idx]
        sx += x
        sy += y
        sxx += x * x
        sxy += x * y
    n = float(ring["count"])
    den = n * sxx - sx * sx
    if den <= 0.0:
        return 0.0
    return (n * sxy - sx * sy) / den * 3600.0


def update(ring, now_ms, liters, valid=True):
    if not valid:
        return
    if ((now_ms - ring["last_push_ms"]) & MASK32) < SAMPLE_MS:
        return
    ring["last_push_ms"] = now_ms & MASK32
    ring["liters"][ring["head"]] = liters
    ring["ms"][ring["head"]] = now_ms & MASK32
    ring["head"] = (ring["head"] + 1) % SLOTS
    if ring["count"] < SLOTS:
        ring["count"] += 1


def format_grouped(value):
    digits = str(abs(int(value)))
    grouped = ""
    for index, char in enumerate(digits):
        remaining = len(digits) - index
        grouped += char
        if remaining > 1 and (remaining - 1) % 3 == 0:
            grouped += "."
    return grouped


def format_rate(liters_per_hour_value, has):
    """Espelho de formatRate() em include/telegram_text.h."""
    if not has:
        return "sem base"
    if -STABLE_LPH < liters_per_hour_value < STABLE_LPH:
        return "→ estável"
    magnitude = abs(liters_per_hour_value)
    rounded = int(magnitude / 10.0 + 0.5) * 10
    arrow = "↑" if liters_per_hour_value > 0 else "↓"
    return "%s %s L/h" % (arrow, format_grouped(rounded))


def run(liters_per_hour_value, slots=SLOTS, start_ms=0, start_liters=50000.0):
    """Serie ideal: `slots` pontos de 5 min com a taxa pedida."""
    ring = begin(start_ms)
    for i in range(slots):
        now = start_ms + i * SAMPLE_MS
        volume = start_liters + liters_per_hour_value * (i * SAMPLE_MS / 3600000.0)
        update(ring, now, volume)
    return ring


# ---------------------------------------------------------------------------
#  Baterias
# ---------------------------------------------------------------------------
def test_constants():
    print("\n1. Constantes (lidas de include/rate.h)")
    check(SAMPLE_MS == 300000, "amostragem de 5 min (mesmo tick do snapshot)")
    check(WINDOW_MS == 3600000, "janela de 1 h")
    check(SLOTS == 12, "12 pontos cobrem a janela (%d)" % SLOTS)
    check(abs(STABLE_LPH - 150.0) < 0.01,
          "banda estavel de 150 L/h (~1,3 contagem/h do ADC)")
    check(MIN_SAMPLES == 7, "base pronta com 7 pontos")
    check(MIN_SPAN_MS == (MIN_SAMPLES - 1) * SAMPLE_MS,
          "span minimo = o que os pontos cobrem (%d min)" % (MIN_SPAN_MS // 60000))
    check(WINDOW_MS % SAMPLE_MS == 0 and SLOTS * SAMPLE_MS == WINDOW_MS,
          "RATE_SLOTS cobre a janela inteira")
    check(MIN_SAMPLES >= 3 and MIN_SAMPLES <= SLOTS,
          "minimos quadrados com pontos suficientes e dentro do anel")


def test_base():
    print("\n2. Base: quando a taxa esta disponivel")
    ring = begin(0)
    check(not has_rate(ring) and liters_per_hour(ring) == 0.0,
          "anel vazio: sem base e taxa zero")
    check(not has_rate(run(300.0, slots=MIN_SAMPLES - 1)),
          "%d pontos (%d min): ainda sem base"
          % (MIN_SAMPLES - 1, (MIN_SAMPLES - 2) * SAMPLE_MS // 60000))
    check(has_rate(run(300.0, slots=MIN_SAMPLES)),
          "%d pontos (%d min): base pronta"
          % (MIN_SAMPLES, MIN_SPAN_MS // 60000))
    check(format_rate(liters_per_hour(ring), has_rate(ring)) == "sem base",
          "sem base o texto e 'sem base'")


def test_slope():
    print("\n3. Inclinacao por minimos quadrados")
    for expected in (0.0, 300.0, -450.0, -6000.0, 116651.0):
        ring = run(expected)
        check(abs(liters_per_hour(ring) - expected) < 0.5,
              "rampa de %+9.0f L/h -> %+9.1f" % (expected, liters_per_hour(ring)))
    check(abs(liters_per_hour(run(6000.0)) - 6000.0) < 0.5,
          "500 L por 5 min = 6.000 L/h")
    # Um ponto atipico NAO inverte o sentido (com primeiro/ultimo, inverteria).
    ring = run(-1000.0)
    ring["liters"][slot_index(ring, 3)] += 5000.0
    slope = liters_per_hour(ring)
    check(-2500.0 < slope < 0.0,
          "um ponto ruim nao inverte o sentido (%.0f L/h)" % slope)


def test_ring_gaps_rollover():
    print("\n4. Anel: rotacao, lacunas e rollover de millis()")
    ring = begin(0)
    for i in range(3 * SLOTS):
        update(ring, i * SAMPLE_MS, 1000.0 * i)
    check(ring["count"] == SLOTS, "o anel satura em %d pontos" % SLOTS)
    check(abs(liters_per_hour(ring) - 1000.0 * SLOTS) < 0.5,
          "so a janela conta: 1.000 L por 5 min = %d L/h" % (1000 * SLOTS))

    # Regime antigo (enchimento) sai da janela: a taxa segue o trecho novo.
    ring = begin(0)
    for i in range(2 * SLOTS):
        update(ring, i * SAMPLE_MS, 1000.0 * i)
    for i in range(2 * SLOTS, 3 * SLOTS):
        update(ring, i * SAMPLE_MS, 1000.0 * (2 * SLOTS) - 40.0 * (i - 2 * SLOTS))
    check(-600.0 < liters_per_hour(ring) < -400.0,
          "o regime antigo saiu da janela (-40 L/5min = -480 L/h): %.0f L/h"
          % liters_per_hour(ring))

    # Lacuna: amostra invalida nao entra e o `x` usa o tempo REAL de cada ponto.
    ring = begin(0)
    for i in range(9):
        update(ring, i * SAMPLE_MS, 50000.0 + 100.0 * i, i != 4)
    check(ring["count"] == 8,
          "amostra invalida nao entra no anel (%d pontos)" % ring["count"])
    check(abs(liters_per_hour(ring) - 1200.0) < 0.5,
          "lacuna nao distorce a taxa (100 L/5min = 1.200 L/h): %.0f"
          % liters_per_hour(ring))

    # Rollover de millis(): os mesmos pontos deslocados dao a mesma taxa.
    reference = run(750.0)
    roll = begin(0xFFFFF000)
    for i in range(SLOTS):
        update(roll, (0xFFFFF000 + i * SAMPLE_MS) & MASK32,
               50000.0 + 750.0 * (i * SAMPLE_MS / 3600000.0))
    check(abs(liters_per_hour(roll) - liters_per_hour(reference)) < 1.0,
          "rollover de millis() nao muda a taxa (%.1f vs %.1f)"
          % (liters_per_hour(roll), liters_per_hour(reference)))
    check(abs(liters_per_hour(roll) - 750.0) < 0.5,
          "e a taxa continua correta (%.1f L/h)" % liters_per_hour(roll))


def test_format():
    print("\n5. Formato do texto (mesma fonte das mensagens)")
    check(format_rate(0.0, False) == "sem base", "sem base")
    check(format_rate(0.0, True) == "→ estável", "parado -> 'estável'")
    check(format_rate(STABLE_LPH - 1.0, True) == "→ estável", "abaixo da banda")
    check(format_rate(STABLE_LPH, True) == "↑ 150 L/h", "no limiar ja vira seta")
    check(format_rate(-STABLE_LPH, True) == "↓ 150 L/h", "esvaziamento no limiar")
    check(format_rate(-STABLE_LPH + 1.0, True) == "→ estável",
          "quase no limiar ainda e estavel")
    check(format_rate(1955.0, True) == "↑ 1.960 L/h",
          "milhar com ponto e arredondamento a 10")
    check(format_rate(-116651.0, True) == "↓ 116.650 L/h", "magnitude grande")
    check(format_rate(1490.0, True) == "↑ 1.490 L/h", "sem separador sobrando")


def test_source():
    print("\n6. Fonte: modulo puro e ligacao no loop/Telegram")
    code = strip_comments(read_text(RATE_HEADER))
    check("namespace rate" in code and "struct Ring" in code,
          "serie e logica em include/rate.h")
    check("n * sxy - sx * sy" in code, "inclinacao por minimos quadrados")
    check("3600.0f" in code, "converte por segundo para por hora")
    check("RATE_SLOTS" in code and "RATE_STABLE_LPH" in code,
          "as constantes de tempo e de limiar vivem no proprio rate.h")
    check("config.h" not in code and "Arduino.h" not in code,
          "header PURO: nao inclui config.h nem Arduino.h")
    check("RATE_SAMPLE_INTERVAL_MS" not in read_text(CONFIG_HEADER),
          "as constantes NAO ficaram em config.h (que inclui <Arduino.h>)")
    check("LittleFS" not in code and "malloc" not in code and "new " not in code,
          "sem flash e sem alocacao dinamica")
    main_cpp = strip_comments(read_text(MAIN_CPP))
    check("rate::begin(g_rate, now)" in main_cpp, "serie zerada no setup")
    check("rate::update(g_rate, now, g_filteredLiters, g_hasSample)" in main_cpp,
          "a serie usa a MESMA media movel publicada")
    check("formatRate(rate::litersPerHour(g_rate), rate::hasRate(g_rate)"
          in main_cpp,
          "a taxa aparece no log periodico (ajuste da janela)")
    check("telegramContext.rateLph = rate::litersPerHour(g_rate)" in main_cpp and
          "telegramContext.hasRate = rate::hasRate(g_rate)" in main_cpp,
          "a taxa chega ao Telegram pelo contexto")


def main():
    print("Taxa de variacao recente (include/rate.h) - verificacao de host")
    test_constants()
    test_base()
    test_slope()
    test_ring_gaps_rollover()
    test_format()
    test_source()

    print()
    if FAILURES:
        print("%d verificacao(oes) falharam:" % len(FAILURES))
        for item in FAILURES:
            print("  - " + item)
        return 1
    print("TODAS AS VERIFICACOES PASSARAM (0 falha)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
