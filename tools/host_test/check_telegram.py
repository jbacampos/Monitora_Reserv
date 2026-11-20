#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verificador do modulo Telegram (camada de texto + invariantes do projeto).

O firmware conversa com a Bot API diretamente no ESP8266. A parte testavel sem
hardware e a camada de texto (include/telegram_text.h): classificacao de comandos
e construcao das mensagens de /help e /status. Este script:

  1. LE as constantes reais de include/config.h (intervalo de polling, buffers
     TLS, corpo HTTP, versao) e falha se os valores mudarem sem que o
     verificador acompanhe;
  2. reproduz campo a campo as mensagens, comparando com o texto EXATO enviado
     (inclusive a ausencia de /reboot e /ota);
  3. simula o UptimeCounter de include/uptime.h, inclusive a passagem pelo
     rollover de millis() (a cada 49,7 dias) e um acumulado maior que 2^32 ms;
  4. confere que NENHUM comando com '@' e aceito (sem username, sem getMe);
  5. confere invariantes de seguranca: ausencia de setInsecure(), CA validada
     nos dois transportes, buffers do MQTT preservados (3072/512), credenciais
     reais apenas em include/secrets.h e a impressao digital da ancora TLS do
     Telegram igual ao SHA-256 documentado;
  6. confere que o /status usa dayReport() e closedPeriodReport() e NAO
     recalcula agregados.

Uso: python check_telegram.py
"""

import base64
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CONFIG_HEADER = os.path.join(ROOT, "include", "config.h")
CA_HEADER = os.path.join(ROOT, "include", "telegram_ca.h")
TEXT_HEADER = os.path.join(ROOT, "include", "telegram_text.h")
TELEGRAM_CPP = os.path.join(ROOT, "src", "telegram.cpp")
SECRETS = os.path.join(ROOT, "include", "secrets.h")
SECRETS_EXAMPLE = os.path.join(ROOT, "include", "secrets.h.example")
THINGSBOARD_CPP = os.path.join(ROOT, "src", "thingsboard.cpp")

FAILURES = []


def check(condition, message):
    if condition:
        print("  [ok]   " + message)
    else:
        print("  [FALHOU] " + message)
        FAILURES.append(message)


def read_constants(path, names):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    values = {}
    environment = {"__builtins__": {}}
    for name in names:
        match = re.search(r"\b" + name + r"\s*(?:\[[^\]]*\])?\s*=\s*([^;]+);", text)
        if not match:
            raise RuntimeError("constante %s nao encontrada em %s" % (name, path))
        expression = match.group(1).replace("UL", "").replace("ull", "").strip()
        if expression.startswith('"'):
            values[name] = expression.strip('"')
        else:
            values[name] = int(eval(expression, environment))
        environment[name] = values[name]
    return values


CFG = read_constants(
    CONFIG_HEADER,
    [
        "FIRMWARE_VERSION",
        "TELEGRAM_PORT",
        "TELEGRAM_POLL_INTERVAL_MS",
        "TELEGRAM_DNS_TIMEOUT_MS",
        "TELEGRAM_SOCKET_TIMEOUT_MS",
        "TELEGRAM_RESPONSE_TIMEOUT_MS",
        "TELEGRAM_TLS_RX_BUFFER",
        "TELEGRAM_TLS_TX_BUFFER",
        "TELEGRAM_BODY_MAX",
        "TELEGRAM_MIN_FREE_BLOCK",
        "TELEGRAM_MIN_FREE_BLOCK_RECOVERY",
        "TELEGRAM_GATE_RECOVERY_PERIOD_MS",
        "TELEGRAM_SESSION_MAX_AGE_MS",
        "STALL_GUARD_GRACE_MS",
        "STALL_GUARD_FIRST_WARN_MS",
        "TELEGRAM_STALL_REBOOT_MS",
        "MQTT_STALL_REBOOT_MS",
        "TELEGRAM_HANDSHAKE_PEAK",
        "TELEGRAM_PEAK_HEADROOM",
        "TELEGRAM_MIN_FREE_HEAP",
        "TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS",
        "MQTT_TLS_RX_BUFFER",
        "MQTT_TLS_TX_BUFFER",
        "MQTT_TLS_RX_BUFFER_FALLBACK",
        "MQTT_BUFFER_SIZE",
        "MQTT_MAX_RECONNECT_MS",
    ],
)

with open(TEXT_HEADER, "r", encoding="utf-8") as _handle:
    _text_header = _handle.read()
_m = re.search(r"TELEGRAM_TEXT_MAX\s*=\s*([0-9]+)", _text_header)
TG_TEXT_MAX = int(_m.group(1)) if _m else 64
_m = re.search(r"TG_MESSAGE_MAX\s*=\s*([0-9]+)", _text_header)
TG_MESSAGE_MAX = int(_m.group(1)) if _m else 384

# Capacidade do corpo do POST em src/telegram.cpp (g_form) e do comando
# normalizado (g_normalized). Lidas do proprio fonte para nao divergirem.
with open(TELEGRAM_CPP, "r", encoding="utf-8") as _handle:
    _tg_cpp = _handle.read()
_m = re.search(r"char g_form\[([0-9]+)\];", _tg_cpp)
FORM_CAPACITY = int(_m.group(1)) if _m else 640


# ---------------------------------------------------------------------------
#  Espelhos da camada de texto (include/telegram_text.h)
# ---------------------------------------------------------------------------
def parse_tg_command(text):
    """Espelho de parseTgCommand(): devolve (comando, texto normalizado).

    Qualquer texto com '@' e recusado antes da normalizacao (o MoniRes nao se
    endereca por username); nesse caso o normalizado fica vazio, como no C++,
    onde `out` ja recebeu '\\0' na entrada.
    """
    if text is None:
        return "None", ""
    begin = 0
    while begin < len(text) and text[begin] in " \t\r\n":
        begin += 1
    end = len(text)
    while end > begin and text[end - 1] in " \t\r\n":
        end -= 1

    trimmed = text[begin:end]
    if "@" in trimmed:
        return "None", ""

    normalized = []
    for char in trimmed:
        if len(normalized) + 1 >= 32:  # g_normalized
            return "None", ""
        normalized.append(char.lower())
    joined = "".join(normalized)

    if joined == "/help":
        return "Help", joined
    if joined == "/status":
        return "Status", joined
    return "None", joined


def format_uptime(uptime_s):
    days = uptime_s // 86400
    hours = (uptime_s // 3600) % 24
    minutes = (uptime_s // 60) % 60
    if days > 0:
        return "%dd %dh %dm" % (days, hours, minutes)
    if hours > 0:
        return "%dh %dm" % (hours, minutes)
    return "%dm" % minutes


def format_liters(liters):
    value = int(liters + 0.5) if liters >= 0 else int(liters - 0.5)
    return "%d L" % value


def format_min_max(has_data, minimum, maximum):
    if not has_data:
        return "Sem dados"
    low = int(minimum + 0.5) if minimum >= 0 else int(minimum - 0.5)
    high = int(maximum + 0.5) if maximum >= 0 else int(maximum - 0.5)
    return "min %d L | max %d L" % (low, high)


def build_status_message(data, version, uptime_s):
    volume = format_liters(data["volume_l"]) if data["has_volume"] else "Sem dados"
    return (
        "Reservatorio\n"
        "Volume atual: %s\n"
        "Hoje: %s\n"
        "Ontem: %s\n"
        "Ultimos 7 dias: %s\n"
        "Ultimos 30 dias: %s\n"
        "Firmware: %s\n"
        "Uptime: %s"
        % (
            volume,
            format_min_max(data["has_today"], data["today_min"], data["today_max"]),
            format_min_max(data["has_yesterday"], data["yesterday_min"],
                           data["yesterday_max"]),
            format_min_max(data["has_last7"], data["last7_min"], data["last7_max"]),
            format_min_max(data["has_last30"], data["last30_min"], data["last30_max"]),
            version,
            format_uptime(uptime_s),
        )
    )


HELP_MESSAGE = (
    "Comandos disponiveis:\n"
    "/help - lista de comandos disponiveis\n"
    "/status - situacao atual do reservatorio"
)


def url_encode(text):
    """Espelho de urlEncodeInto(): mesmos caracteres seguros do firmware."""
    out = []
    for char in text:
        if char.isalnum() or char in "-_.~":
            out.append(char)
        else:
            for byte in char.encode("utf-8"):
                out.append("%%%02X" % byte)
    return "".join(out)


# ---------------------------------------------------------------------------
#  Espelhos das comparacoes do leitor HTTP (src/telegram.cpp)
# ---------------------------------------------------------------------------
def has_prefix_ignore_case(text, prefix):
    """Espelho de hasPrefixIgnoreCase(): strncasecmp(text, prefix, strlen(prefix)).

    Os DOIS lados sao comparados sem caixa; um `text` mais curto que o prefixo
    nao casa (strncasecmp para no NUL).
    """
    return text[:len(prefix)].lower() == prefix.lower()


def contains_token_ignore_case(haystack, token):
    """Espelho de containsTokenIgnoreCase(): token em qualquer posicao, sem caixa."""
    return token.lower() in haystack.lower()


def full_status_data():
    return {
        "has_volume": True, "volume_l": 1234.0,
        "has_today": True, "today_min": 1200.0, "today_max": 1310.0,
        "has_yesterday": True, "yesterday_min": 1150.0, "yesterday_max": 1290.0,
        "has_last7": True, "last7_min": 1100.0, "last7_max": 1320.0,
        "has_last30": True, "last30_min": 980.0, "last30_max": 1330.0,
    }


def empty_status_data():
    return {
        "has_volume": False, "volume_l": 0.0,
        "has_today": False, "today_min": 0.0, "today_max": 0.0,
        "has_yesterday": False, "yesterday_min": 0.0, "yesterday_max": 0.0,
        "has_last7": False, "last7_min": 0.0, "last7_max": 0.0,
        "has_last30": False, "last30_min": 0.0, "last30_max": 0.0,
    }


# ---------------------------------------------------------------------------
#  Baterias de teste
# ---------------------------------------------------------------------------
def test_constants():
    print("\n1. Constantes do firmware (lidas de config.h)")
    check(CFG["TELEGRAM_POLL_INTERVAL_MS"] == 20000, "polling = 20 s")
    check(CFG["TELEGRAM_PORT"] == 443, "porta HTTPS = 443")
    check(CFG["TELEGRAM_TLS_RX_BUFFER"] == 4096,
          "buffer TLS RX do Telegram = 4096 B (cobre a cadeia e reduz o pico)")
    check(CFG["TELEGRAM_TLS_TX_BUFFER"] == 512, "buffer TLS TX do Telegram = 512 B")
    check(CFG["TELEGRAM_BODY_MAX"] == 2048, "corpo HTTP maximo = 2048 B")
    check(CFG["TELEGRAM_RESPONSE_TIMEOUT_MS"] < CFG["TELEGRAM_POLL_INTERVAL_MS"],
          "timeout da resposta cabe no intervalo de polling")
    check(CFG["TELEGRAM_DNS_TIMEOUT_MS"] < 3000,
          "DNS limitado (nao usa os 10 s fixos do connect)")
    check(CFG["TELEGRAM_MIN_FREE_BLOCK"] >= 8192,
          "bloco contiguo minimo protege a alocacao do TLS")
    # O buraco deixado por uma sessao que morreu (campo: 8 976 B) fica abaixo do
    # piso normal, mas acima da necessidade real (8 450 B). Sem o piso de
    # emergencia o gate ficava fechado para sempre e o bot parava de responder.
    check(CFG["TELEGRAM_MIN_FREE_BLOCK_RECOVERY"] >= 8576,
          "piso de emergencia cobre a alocacao real da sessao")
    check(CFG["TELEGRAM_MIN_FREE_BLOCK_RECOVERY"] < CFG["TELEGRAM_MIN_FREE_BLOCK"],
          "piso de emergencia menor que o normal (so recria a sessao caida)")
    check(CFG["TELEGRAM_GATE_RECOVERY_PERIOD_MS"] >= CFG["TELEGRAM_POLL_INTERVAL_MS"],
          "recuperacao do gate nao e mais frequente que o polling")
    # O servidor fecha a conexao com ~1 h (medido em duas capturas independentes).
    # Renovar antes mantem o churn previsivel e sempre com o heap conhecido.
    check(CFG["TELEGRAM_SESSION_MAX_AGE_MS"] < 3600000,
          "renovacao preventiva antes do corte de ~1 h do servidor")
    check(CFG["TELEGRAM_SESSION_MAX_AGE_MS"] >= 5 * CFG["TELEGRAM_POLL_INTERVAL_MS"],
          "renovacao nao acontece a cada poucos polls")

    # Guarda de bloqueio: reboot controlado quando TG ou MQTT ficam presos.
    check(CFG["STALL_GUARD_GRACE_MS"] >= CFG["TELEGRAM_POLL_INTERVAL_MS"],
          "carencia da guarda cobre ao menos um intervalo de polling")
    check(CFG["STALL_GUARD_FIRST_WARN_MS"] >= CFG["STALL_GUARD_GRACE_MS"],
          "primeiro aviso da guarda vem depois da carencia")
    check(CFG["TELEGRAM_STALL_REBOOT_MS"] > CFG["TELEGRAM_GATE_RECOVERY_PERIOD_MS"],
          "a recuperacao do gate tem chance antes do reboot")
    check(CFG["TELEGRAM_STALL_REBOOT_MS"] > CFG["STALL_GUARD_FIRST_WARN_MS"] and
          CFG["MQTT_STALL_REBOOT_MS"] > CFG["STALL_GUARD_FIRST_WARN_MS"],
          "nos dois servicos o reboot vem depois do aviso")
    check(CFG["MQTT_STALL_REBOOT_MS"] > CFG["MQTT_MAX_RECONNECT_MS"],
          "o backoff do MQTT tem chance antes do reboot")
    check(CFG["TELEGRAM_STALL_REBOOT_MS"] <= 7200000 and
          CFG["MQTT_STALL_REBOOT_MS"] <= 7200000,
          "nenhum bloqueio espera mais de 2 h pelo reboot")
    # O pico do handshake (~12,9 KB com RX=5120) deixava < 2 KB livres para o
    # lwIP/WiFi e o firmware morreu em memcpy(NULL) no contexto sys. O piso de
    # heap LIVRE TOTAL e o que impede repetir isso.
    check(CFG["TELEGRAM_MIN_FREE_HEAP"] > CFG["TELEGRAM_MIN_FREE_BLOCK"],
          "piso de heap livre acima do bloco contiguo minimo")
    check(CFG["TELEGRAM_MIN_FREE_HEAP"] >= 13312,
          "piso de heap livre cobre o pico do handshake com folga")
    check(CFG["FIRMWARE_VERSION"] == "1.0.0", "versao do firmware = 1.0.0")

    print("\n2. Buffers do MQTT preservados")
    check(CFG["MQTT_TLS_RX_BUFFER"] == 3072,
          "MQTT RX = 3072 B (3397 B alocados para um registro de 1735 B)")
    check(CFG["MQTT_TLS_RX_BUFFER_FALLBACK"] == 4096,
          "RX de emergencia do MQTT = 4096 B (4421 B alocados)")
    check(CFG["MQTT_TLS_RX_BUFFER_FALLBACK"] > CFG["MQTT_TLS_RX_BUFFER"],
          "RX de emergencia e maior que o normal (so amplia, nunca reduz)")
    check(CFG["MQTT_TLS_RX_BUFFER_FALLBACK"] <= 16384,
          "RX de emergencia dentro do limite do core (16384 B)")
    check(CFG["MQTT_TLS_TX_BUFFER"] == 512, "MQTT TX continua 512 B")
    check(CFG["MQTT_BUFFER_SIZE"] == 256, "buffer do PubSubClient continua 256 B")


def test_commands():
    print("\n3. Classificacao de comandos")
    cases = [
        ("/help", "Help"),
        ("/status", "Status"),
        ("/HELP", "Help"),
        ("/Status", "Status"),
        ("  /status  ", "Status"),
        ("\r\n/help\r\n", "Help"),
        ("/status agora", "None"),
        ("/statusx", "None"),
        ("/s", "None"),
        ("status", "None"),
        ("", "None"),
        ("   ", "None"),
        ("/reboot", "None"),
        ("/ota", "None"),
    ]
    for text, expected in cases:
        result, _ = parse_tg_command(text)
        check(result == expected, "%r -> %s" % (text, expected))

    check(parse_tg_command(None)[0] == "None", "texto ausente -> None")
    long_text = "/status" + "x" * 40
    check(parse_tg_command(long_text)[0] == "None",
          "comando absurdamente longo e ignorado (sem truncar)")

    print("\n3b. Comandos enderecados a bot sao SEMPRE rejeitados")
    addressed = [
        "/help@MoniResBot",
        "/status@MoniResBot",
        "/help@OutroBot",
        "/status@OutroBot",
        "/status@",
        "/help@",
        "/status@moniresbot",
        "/STATUS@MoniResBot",
        "  /status@MoniResBot  ",
        "/status@MoniResBot argumento",
        "@MoniResBot /status",
        "/status @MoniResBot",
    ]
    for text in addressed:
        result, normalized = parse_tg_command(text)
        check(result == "None", "%r rejeitado" % text)
        check(normalized == "", "%r nao deixa comando normalizado" % text)


class UptimeCounter:
    """Espelho de include/uptime.h: acumulado de 64 bits com delta wrap-safe."""

    MASK32 = 0xFFFFFFFF

    def __init__(self):
        self._last = 0
        self._ms = 0
        self._started = False

    def begin(self, now_ms):
        self._last = now_ms & self.MASK32
        self._ms = 0
        self._started = True

    def update(self, now_ms):
        now_ms &= self.MASK32
        if not self._started:
            self.begin(now_ms)
            return
        delta = (now_ms - self._last) & self.MASK32
        self._last = now_ms
        self._ms += delta

    def milliseconds(self):
        return self._ms

    def seconds(self):
        return self._ms // 1000


def test_uptime():
    print("\n4. Uptime acumulado (continuo no rollover de millis())")

    print("  4.1 operacao normal antes do rollover")
    counter = UptimeCounter()
    counter.begin(0)
    for now in (0, 250, 500, 1000, 2000):
        counter.update(now)
    check(counter.milliseconds() == 2000, "6 chamadas de 0 a 2000 ms somam 2 s")
    check(counter.seconds() == 2, "seconds() acompanha o acumulado")
    counter.update(2000)
    check(counter.milliseconds() == 2000, "update repetido com o mesmo now nao conta")

    print("  4.2 passagem pelo rollover de millis()")
    counter = UptimeCounter()
    counter.begin(0xFFFFFF00)  # 256 ms antes de virar
    counter.update(0xFFFFFF80)  # +128 ms (falta 128 ms)
    check(counter.milliseconds() == 128, "antes da virada: 128 ms")
    counter.update(0xFFFFFFFE)  # +126 ms (falta 2 ms)
    check(counter.milliseconds() == 254, "a 2 ms da virada: 254 ms")
    counter.update(0x00000002)  # VIRA aqui: +4 ms
    check(counter.milliseconds() == 258, "depois da virada: 258 ms (sem salto)")
    counter.update(0x000003E8)  # +998 ms
    check(counter.milliseconds() == 1256, "continua somando depois da virada")
    check(counter.seconds() == 1, "seconds() = 1 apos 1256 ms")

    print("  4.3 continuidade em rajada que atravessa a virada")
    counter = UptimeCounter()
    counter.begin(0xFFFFFF00)
    now = 0xFFFFFF00
    previous = 0
    monotonic = True
    for _ in range(1000):
        now = (now + 500) & 0xFFFFFFFF
        counter.update(now)
        if counter.milliseconds() < previous:
            monotonic = False
        previous = counter.milliseconds()
    check(monotonic, "acumulado nunca diminui (1000 passos cruzando a virada)")
    check(counter.milliseconds() == 500000, "1000 x 500 ms = 500 s acumulados")
    check(counter.seconds() == 500, "500 s depois da virada")

    print("  4.4 acumulado maior que 49,7 dias (2^32 ms) nao reinicia")
    counter = UptimeCounter()
    counter.begin(0)
    now = 0
    steps = 71600  # 71 600 x 60 s = 4 296 000 000 ms (~49,72 dias)
    for _ in range(steps):
        now = (now + 60000) & 0xFFFFFFFF
        counter.update(now)
    expected_ms = steps * 60000
    check(expected_ms > 0xFFFFFFFF, "o cenario realmente passa de 2^32 ms")
    check(counter.milliseconds() == expected_ms,
          "acumulado de 64 bits preserva os 49,7 dias completos")
    check(counter.seconds() == expected_ms // 1000,
          "seconds() acompanha o acumulado apos o rollover")
    check(counter.seconds() > now // 1000,
          "millis() cru estaria reiniciado (o bug antigo e demonstrado)")
    formatted = format_uptime(counter.seconds())
    check(formatted == "49d 17h 20m", "formatacao em dias apos o rollover: %s" % formatted)

    print("  4.5 pausa longa (consulta do Telegram) contada uma unica vez")
    counter = UptimeCounter()
    counter.begin(1000)
    counter.update(2500)  # +1,5 s
    counter.update(5500)  # +3,0 s de consulta bloqueante
    check(counter.milliseconds() == 4500, "o tempo bloqueado nao se perde nem duplica")

    print("  4.6 formatacao (minutos, horas e dias)")
    cases = [
        (0, "0m"),
        (59, "0m"),
        (60, "1m"),
        (45 * 60, "45m"),
        (3599, "59m"),
        (3600, "1h 0m"),
        (2 * 3600 + 15 * 60, "2h 15m"),
        (86399, "23h 59m"),
        (86400, "1d 0h 0m"),
        (3 * 86400 + 7 * 3600 + 42 * 60, "3d 7h 42m"),
    ]
    for seconds, expected in cases:
        check(format_uptime(seconds) == expected,
              "%d s -> %s" % (seconds, expected))

    print("  4.7 o /status usa o acumulado")
    line = build_status_message(full_status_data(), "1.0.0",
                                UptimeCounter().seconds()).split("\n")[-1]
    check(line == "Uptime: 0m", "uptime zerado no inicio: %s" % line)
    counter = UptimeCounter()
    counter.begin(0xFFFFFF00)
    counter.update(0x00000002)  # 258 ms apos a virada
    counter.update((0x00000002 + 2 * 60 * 60 * 1000) & 0xFFFFFFFF)  # +2 h
    line = build_status_message(full_status_data(), "1.0.0",
                                counter.seconds()).split("\n")[-1]
    check(line == "Uptime: 2h 0m", "2 h depois da virada: %s" % line)


def test_help():
    print("\n5. Mensagem do /help")
    check(HELP_MESSAGE.splitlines()[0] == "Comandos disponiveis:",
          "cabecalho correto")
    check("/reboot" not in HELP_MESSAGE, "/reboot NAO e anunciado")
    check("/ota" not in HELP_MESSAGE, "/ota NAO e anunciado")
    check(HELP_MESSAGE.count("/help") == 1 and HELP_MESSAGE.count("/status") == 1,
          "somente /help e /status listados")
    check(len(HELP_MESSAGE) < TG_MESSAGE_MAX, "cabe no buffer de mensagem")


BOOT_REASON_TEXT = {
    1: "Energizacao",
    2: "Watchdog de hardware",
    3: "Excecao de software",
    4: "Watchdog de software",
    5: "Reinicio (/reboot)",
    6: "Retorno de deep sleep",
    7: "Reset externo",
    8: "Desconhecido",
    9: "Atualizacao OTA (/ota)",
}

BOOT_SDK_STRINGS = (
    "Power On",
    "Hardware Watchdog",
    "Exception",
    "Software Watchdog",
    "Software/System restart",
    "Deep-Sleep Wake",
    "External System",
)

BOOT_REASON_CASES = (
    (False, "Power On", 1),
    (False, "Hardware Watchdog", 2),
    (False, "Exception", 3),
    (False, "Software Watchdog", 4),
    (False, "Software/System restart", 5),
    (False, "Deep-Sleep Wake", 6),
    (False, "External System", 7),
    (False, "motivo novo do SDK", 8),
    (False, None, 8),
    (True, "Power On", 9),
    (True, "Software/System restart", 9),
    (True, None, 9),
)


def classify_boot_reason(ota_marker, reset_reason):
    if ota_marker:
        return 9
    if reset_reason is None:
        return 8
    for index, text in enumerate(BOOT_SDK_STRINGS, start=1):
        if reset_reason == text:
            return index
    return 8


def build_boot_message(reason_code, version):
    return "Monitora_Reserv reiniciado.\nVersao: %s\nMotivo: %s" % (
        version, BOOT_REASON_TEXT[reason_code])


def test_boot_notice():
    print("\n5b. Aviso de boot (mensagem, motivo e envio unico)")
    boot_header = strip_comments(
        read_text(os.path.join(ROOT, "include", "boot_reason.h")))
    text_header = strip_comments(read_text(TEXT_HEADER))
    telegram = strip_comments(read_text(TELEGRAM_CPP))

    # 1) Mensagem no MESMO padrao do Monitora_DG: tres linhas, nada mais.
    message = build_boot_message(1, "1.0.0")
    lines = message.split("\n")
    check(len(lines) == 3, "aviso tem exatamente 3 linhas (%d)" % len(lines))
    check(lines[0] == "Monitora_Reserv reiniciado.", "1a linha: '<firmware> reiniciado.'")
    check(lines[1] == "Versao: 1.0.0", "2a linha: versao do firmware")
    check(lines[2] == "Motivo: Energizacao", "3a linha: motivo do boot")
    check(len(message) < TG_MESSAGE_MAX, "cabe no buffer de mensagem (%d B)"
          % len(message))
    check('"Monitora_Reserv reiniciado.\\n"' in text_header,
          "texto do aviso vive em include/telegram_text.h")
    check('"Versao: %s\\n"' in text_header and '"Motivo: %s"' in text_header,
          "buildBootMessage usa versao e motivo")

    # 2) Tabela unica codigo -> descricao (nenhuma copia em outro arquivo).
    for code, description in BOOT_REASON_TEXT.items():
        check('"%s"' % description in boot_header,
              "motivo %d descrito como '%s'" % (code, description))

    # 3) Classificacao: toda string do SDK mapeada; a marca de OTA vence.
    for ota, reason, expected in BOOT_REASON_CASES:
        check(classify_boot_reason(ota, reason) == expected,
              "ota=%s reset=%r -> %d" % (ota, reason, expected))
    for sdk_string in BOOT_SDK_STRINGS:
        check('"%s"' % sdk_string in boot_header,
              "string do SDK tratada: '%s'" % sdk_string)
    check("if (otaMarker)" in boot_header,
          "marca persistida de OTA tem prioridade sobre a string")
    check("nullptr" in boot_header, "reason ausente cai em 'Desconhecido'")

    # 4) Envio: uma vez por boot, pelo caminho que ja funciona.
    check("void Telegram::sendBootNotice(uint32_t nowMs)" in telegram,
          "aviso implementado em src/telegram.cpp")
    check("if (!m_bootNoticeSent)" in telegram,
          "enviado uma unica vez por boot")
    check("m_bootNoticeSent = true;" in telegram,
          "marca de enviado so depois do envio")
    check("sendMessage(chatId, g_message)" in telegram,
          "usa o MESMO sendMessage das respostas de comando")
    check("buildBootMessage(m_bootReason, FIRMWARE_VERSION" in telegram,
          "texto vem da camada pura (telegram_text.h)")
    check("TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS" in telegram,
          "desiste depois de N consultas sem entrega")
    body = telegram.split("void Telegram::sendBootNotice")[1].split(
        "void Telegram::pollOnce")[0]
    check("getUpdates" not in body and "offset" not in body,
          "aviso nao toca em getUpdates nem no offset")
    check("readResponse" not in body, "aviso nao le resposta de consulta")
    check("getResetReason" not in telegram,
          "sem ESP.getResetReason() (String/newlib custam ~650 B de RAM)")
    check("getResetInfoPtr" in telegram,
          "motivo vem de rst_info, sem String")
    check(CFG["TELEGRAM_BOOT_NOTICE_MAX_ATTEMPTS"] == 3,
          "3 tentativas de entrega (config.h)")


def test_status():
    print("\n6. Mensagem do /status (grupos e ordem)")
    message = build_status_message(full_status_data(), "1.0.0", 2 * 3600 + 15 * 60)
    lines = message.split("\n")
    check(lines[0] == "Reservatorio", "1o grupo: titulo")
    check(lines[1] == "Volume atual: 1234 L", "2o grupo: volume atual em litros")
    check(lines[2] == "Hoje: min 1200 L | max 1310 L", "3o grupo: hoje (min/max)")
    check(lines[3] == "Ontem: min 1150 L | max 1290 L", "4o grupo: ontem (min/max)")
    check(lines[4] == "Ultimos 7 dias: min 1100 L | max 1320 L",
          "5o grupo: ultimos 7 dias completos")
    check(lines[5] == "Ultimos 30 dias: min 980 L | max 1330 L",
          "6o grupo: ultimos 30 dias completos")
    check(lines[6] == "Firmware: 1.0.0", "7o grupo: versao do firmware")
    check(lines[7] == "Uptime: 2h 15m", "8o grupo: uptime")
    check(len(lines) == 8, "nenhuma linha extra")

    for forbidden in ("Media", "media", "cobertura", "Wi-Fi", "WiFi", "MQTT",
                      "fila", "RSSI", "degradado"):
        check(forbidden not in message,
              "sem informacao proibida nesta etapa: %s" % forbidden)

    print("\n7. Mensagem do /status sem dados")
    empty = build_status_message(empty_status_data(), "1.0.0", 30)
    check(empty.count("Sem dados") == 5,
          "volume, hoje, ontem, 7 dias e 30 dias mostram 'Sem dados'")
    check("Firmware: 1.0.0" in empty, "a versao aparece mesmo sem historico")
    check("Uptime: 0m" in empty, "o uptime aparece mesmo sem historico")

    print("\n8. Mensagem do /status parcial (somente alguns periodos)")
    partial_data = empty_status_data()
    partial_data["has_volume"] = True
    partial_data["volume_l"] = 830.4
    partial_data["has_today"] = True
    partial_data["today_min"] = 800.6
    partial_data["today_max"] = 830.4
    partial = build_status_message(partial_data, "1.0.0", 90061)
    check("Volume atual: 830 L" in partial, "volume arredondado para litros")
    check("Hoje: min 801 L | max 830 L" in partial, "hoje com dados parciais")
    check("Ontem: Sem dados" in partial, "ontem sem dados")
    check("Ultimos 7 dias: Sem dados" in partial, "7 dias sem dados")
    check("Ultimos 30 dias: Sem dados" in partial, "30 dias sem dados")
    check("Uptime: 1d 1h 1m" in partial, "uptime em dias, horas e minutos")

    print("\n9. Tamanho e codificacao para o POST")
    big = {
        "has_volume": True, "volume_l": 99999.0,
        "has_today": True, "today_min": 99999.0, "today_max": 99999.0,
        "has_yesterday": True, "yesterday_min": 99999.0, "yesterday_max": 99999.0,
        "has_last7": True, "last7_min": 99999.0, "last7_max": 99999.0,
        "has_last30": True, "last30_min": 99999.0, "last30_max": 99999.0,
    }
    for label, data, uptime in (
            ("completo", full_status_data(), 3 * 86400 + 7 * 3600 + 42 * 60),
            ("vazio", empty_status_data(), 0),
            ("valores grandes", big, 0)):
        text = build_status_message(data, CFG["FIRMWARE_VERSION"], uptime)
        encoded = url_encode(text)
        body = "chat_id=-1001234567890&text=" + encoded
        check(len(text) < TG_MESSAGE_MAX,
              "%s: mensagem (%d B) cabe em TG_MESSAGE_MAX" % (label, len(text)))
        check(len(body) < FORM_CAPACITY,
              "%s: POST (%d B) cabe em g_form" % (label, len(body)))
        check("%0A" in encoded, "%s: quebras de linha codificadas" % label)


def read_text(path):
    with open(path, "r", encoding="utf-8") as handle:
        return handle.read()


def strip_comments(text):
    """Remove comentarios // e /* */ para checar apenas CODIGO de verdade.

    Os comentarios do projeto citam setInsecure() justamente para registrar que
    ele NAO e usado; procurar a string crua daria falso positivo.
    """
    without_block = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", without_block)


def project_sources():
    """(nome, conteudo) de todos os .h/.cpp de include/ e src/."""
    files = []
    for folder in ("include", "src"):
        directory = os.path.join(ROOT, folder)
        for name in sorted(os.listdir(directory)):
            if name.endswith(".h") or name.endswith(".cpp"):
                path = os.path.join(directory, name)
                files.append((folder + "/" + name, read_text(path)))
    return files


def test_invariants():
    print("\n10. Invariantes de seguranca do projeto")
    sources = project_sources()

    offenders = [name for name, text in sources if "setInsecure" in strip_comments(text)]
    check(not offenders, "nenhum setInsecure() no projeto (%s)" % (offenders or "-"))

    thingsboard = read_text(THINGSBOARD_CPP)
    check("setTrustAnchors(&g_mqttCa)" in thingsboard,
          "MQTT continua validando a CA (setTrustAnchors)")
    check("setBufferSizes(4096, 512)" not in thingsboard and
          "setBufferSizes(MQTT_TLS_RX_BUFFER" in thingsboard,
          "MQTT continua com buffers explicitos de config.h")
    # Fallback: QUEM escolhe a cadeia e o broker, e ela precisa caber inteira no
    # RX. Codigo 6 = BR_ERR_TOO_LARGE; -1000 = OOM do core. Sem isso, uma rotacao
    # de certificado derrubava a telemetria ate alguem recompilar o firmware.
    check("getLastSSLError" in thingsboard and
          "kSslRecordTooLarge = 6" in thingsboard,
          "falha de TLS do MQTT classificada pelo codigo do BearSSL")
    check("setBufferSizes(MQTT_TLS_RX_BUFFER_FALLBACK" in thingsboard,
          "codigo 6 amplia o RX para MQTT_TLS_RX_BUFFER_FALLBACK")
    oom_at = thingsboard.find("sslCode == kSslOutOfMemory")
    revert_at = thingsboard.find(
        "setBufferSizes(MQTT_TLS_RX_BUFFER, MQTT_TLS_TX_BUFFER);",
        oom_at if oom_at != -1 else 0)
    check(oom_at != -1 and revert_at > oom_at,
          "sem heap (codigo -1000) o fallback volta ao buffer normal")
    check("g_transport.stop();" in thingsboard,
          "tentativa que falhou libera contexto e buffers na hora")
    check("m_retryNow" in thingsboard,
          "depois de reajustar os buffers a nova tentativa nao espera o backoff")
    main_diag = read_text(os.path.join(ROOT, "src", "main.cpp"))
    check("g_tb.rxEscalated()" in main_diag,
          "linha periodica mostra o RX de emergencia (mqtt=off(rx4096))")
    # TLS so com relogio valido: com a hora em 1970 o BearSSL recusa a cadeia
    # ("expired or not yet valid", codigo 54). Medido em campo em 2026-11-20, no
    # boot em que a tentativa saiu antes da resposta do SNTP.
    check("const bool clockReady = sample.timestampMs != 0;" in thingsboard and
          "clockReady && (nowMs - m_lastAttemptMs)" in thingsboard,
          "MQTT so tenta o TLS com o relogio sincronizado")
    check("g_transport.setX509Time(epochNow);" in thingsboard,
          "MQTT passa a hora para a validacao X.509")
    check("client->setX509Time(epochNow);" in
          read_text(os.path.join(ROOT, "src", "telegram.cpp")),
          "Telegram passa a hora para a validacao X.509")

    telegram = read_text(TELEGRAM_CPP)
    check("setTrustAnchors(&g_trustAnchor)" in telegram,
          "Telegram valida a CA (setTrustAnchors)")
    check("setBufferSizes(TELEGRAM_TLS_RX_BUFFER, TELEGRAM_TLS_TX_BUFFER)" in telegram,
          "Telegram usa os proprios buffers TLS")
    check("MQTT_TLS_RX_BUFFER" not in telegram,
          "Telegram nao toca nos buffers do MQTT")
    check("setTimeout(TELEGRAM_SOCKET_TIMEOUT_MS)" in telegram,
          "timeout do socket aplicado antes do connect")
    check("TELEGRAM_MIN_FREE_BLOCK_RECOVERY" in telegram and
          "TELEGRAM_GATE_RECOVERY_PERIOD_MS" in telegram,
          "gate recria a sessao caida pelo piso de emergencia (nao trava para sempre)")
    # A liberacao da sessao morta tem de vir ANTES da leitura do heap: se o gate
    # medir primeiro, o bloco continua preso e nem o piso de emergencia dispara
    # (caso de campo: bloco=5240 B, livre=5544 B, bot mudo por horas).
    guard_at = telegram.find("g_client != nullptr && !g_client->connected()")
    measured_at = telegram.find("const size_t block = ESP.getMaxFreeBlockSize();",
                                guard_at if guard_at != -1 else 0)
    check(guard_at != -1 and measured_at > guard_at,
          "sessao morta e liberada ANTES de o gate medir o heap")
    reclaim = (telegram[guard_at:measured_at]
               if guard_at != -1 and measured_at > guard_at else "")
    check("closeConnection();" in reclaim,
          "o caminho da sessao morta chama closeConnection()")
    check("m_gateBlocked = false;" in reclaim and
          "m_gateBlockedSinceMs = 0;" in reclaim,
          "liberar a sessao morta tambem limpa o estado do gate")
    # Segunda rede: o piso de emergencia so pode valer DEPOIS de o gate recusar
    # pelo piso normal (nunca como atalho para criar a sessao com heap apertado).
    check("!canPoll && block >= TELEGRAM_MIN_FREE_BLOCK_RECOVERY" in telegram and
          "m_gateBlocked &&" in telegram,
          "piso de emergencia e segunda rede (so com o gate ja fechado)")
    check("TELEGRAM_SESSION_MAX_AGE_MS" in telegram and
          "renovacao preventiva" in telegram,
          "renovacao preventiva da sessao usa o limite de config.h")

    leaked = [line for line in telegram.splitlines()
              if "Serial" in line and ("TELEGRAM_BOT_TOKEN" in line or
                                       "ACTIVE_TELEGRAM" in line)]
    check(not leaked, "token/chat nunca aparecem em log serial (%s)" % (leaked or "-"))
    check("getLastSSLError" in telegram,
          "falha de TLS registra o codigo BearSSL (sem credenciais)")

    print("\n11. Credenciais fora do codigo versionado")
    gitignore = read_text(os.path.join(ROOT, ".gitignore"))
    check("include/secrets.h" in gitignore, "include/secrets.h esta no .gitignore")
    check(os.path.exists(SECRETS), "include/secrets.h existe (nao versionado)")
    example = read_text(SECRETS_EXAMPLE)
    token_pattern = re.compile(r"\b\d{8,10}:[A-Za-z0-9_-]{30,}\b")
    check(token_pattern.search(example) is None,
          "secrets.h.example nao contem token real")
    secrets_text = read_text(SECRETS) if os.path.exists(SECRETS) else ""
    for field in ("TELEGRAM_BOT_TOKEN_TEST", "TELEGRAM_CHAT_ID_TEST",
                  "TELEGRAM_BOT_TOKEN_PROD", "TELEGRAM_CHAT_ID_PROD",
                  "TELEGRAM_CHAT_ID2_TEST", "TELEGRAM_CHAT_ID2_PROD"):
        check(field in secrets_text,
              "secrets.h declara %s (fora do Git, valores nao verificados aqui)"
              % field)
    versioned = [(name, text) for name, text in sources if not name.endswith("secrets.h")]
    leaked_tokens = [name for name, text in versioned if token_pattern.search(text)]
    check(not leaked_tokens,
          "nenhum arquivo versionado traz token de bot (%s)" % (leaked_tokens or "-"))
    check("TELEGRAM_BOT_TOKEN_TEST" in example and "TELEGRAM_CHAT_ID2_PROD" in example,
          "modelo documenta token e os dois chats autorizados")

    print("\n12. Ancora TLS do Telegram")
    ca = read_text(CA_HEADER)
    check(ca.count("BEGIN CERTIFICATE") == 1, "exatamente um certificado embutido")
    block = ca.split("-----BEGIN CERTIFICATE-----")[1].split("-----END CERTIFICATE-----")[0]
    der = base64.b64decode("".join(block.split()))
    digest = hashlib.sha256(der).hexdigest().upper()
    check(digest == "45140B3247EB9CC8C5B4F0D7B53091F73292089E6E5A63E2749DD3ACA9198EDA",
          "SHA-256 do DER confere com o documentado")
    check("Go Daddy Root Certificate Authority - G2" in ca,
          "ancora e a raiz Go Daddy G2")
    check("2037" in ca, "validade da ancora (ate 2037) documentada")
    check("api.telegram.org" in ca, "origem da ancora documentada")
    check("setInsecure" not in strip_comments(ca),
          "nenhuma chamada a setInsecure no cabecalho da CA")

    print("\n13. Fontes do /status")
    check("->dayReport(" in telegram, "usa History::dayReport()")
    check(telegram.count("->dayReport(") == 2, "dayReport() para hoje e para ontem")
    check(telegram.count("->closedPeriodReport(") == 2,
          "closedPeriodReport() para 7 e para 30 dias fechados")
    check("periodReport(" not in telegram,
          "nao usa periodReport() (a janela incluiria hoje)")
    check("fmin" not in telegram and "fmax" not in telegram,
          "nao recalcula extremos")
    check(telegram.count("= day.minL") == 2 and telegram.count("= period.minL") == 2,
          "min/max vem prontos do relatorio")

    print("\n14. Versao unica e integracao no loop")
    definitions = [(name, text.count("FIRMWARE_VERSION[] ="))
                   for name, text in sources if "FIRMWARE_VERSION[] =" in text]
    check(len(definitions) == 1 and definitions[0][1] == 1,
          "FIRMWARE_VERSION definida em um unico lugar (%s)" % (definitions,))
    main_cpp = read_text(os.path.join(ROOT, "src", "main.cpp"))
    check("g_telegram.begin(now)" in main_cpp, "Telegram inicializado no setup")
    check("g_telegram.update(now, telegramContext)" in main_cpp,
          "Telegram servido pelo loop")
    check("telegramContext.history = &g_history" in main_cpp,
          "historico unico passado ao Telegram")
    check("telegramContext.volumeL = g_filteredLiters" in main_cpp,
          "volume atual vem da mesma fonte do ThingsBoard")

    print("\n15. Uptime acumulado (imune ao rollover de millis())")
    uptime_code = strip_comments(read_text(os.path.join(ROOT, "include", "uptime.h")))
    check("uint64_t m_ms" in uptime_code, "acumulador de 64 bits em include/uptime.h")
    check("static_cast<uint32_t>(nowMs - m_lastMs)" in uptime_code,
          "delta entre leituras calculado em uint32_t (wrap-safe)")
    check("uint32_t seconds() const" in uptime_code,
          "seconds() exposto para o /status e o ThingsBoard")
    check("g_uptime.begin(now)" in main_cpp, "contador iniciado no setup")
    check("g_uptime.update(now)" in main_cpp, "contador atualizado no loop")
    check("telegramContext.uptimeS = g_uptime.seconds()" in main_cpp,
          "/status recebe o uptime acumulado")
    check("sample.uptimeS = g_uptime.seconds()" in main_cpp,
          "o atributo uptime_s do ThingsBoard usa o mesmo acumulado")
    check("now / 1000" not in main_cpp,
          "millis() cru nao alimenta mais nenhum uptime")

    print("\n16. Nenhum enderecamento a bot (username/getMe)")
    addressing = ("getMe", "get_me", "getme", "username", "userName", "botName",
                  "botUsername", "BOT_USERNAME", "TELEGRAM_BOT_USERNAME")
    found = []
    for name, text in sources:
        code = strip_comments(text)
        for token in addressing:
            if token in code:
                found.append("%s:%s" % (name, token))
    check(not found, "sem username, getMe ou configuracao de nome (%s)"
          % (found or "-"))
    text_header_code = strip_comments(read_text(TEXT_HEADER))
    check("*p == '@'" in text_header_code,
          "parseTgCommand recusa qualquer texto com '@'")
    check("/help" in text_header_code and "/status" in text_header_code,
          "apenas /help e /status sao reconhecidos")

    print("\n17. Comparacoes do leitor HTTP (os dois lados sem caixa)")
    tg_code = strip_comments(telegram)
    check("strncasecmp(text, prefix, strlen(prefix)) == 0" in tg_code,
          "hasPrefixIgnoreCase usa strncasecmp (compara os dois lados)")
    check("!= prefix[i]" not in tg_code,
          "o laco antigo (tolower(text[i]) != prefix[i]) nao existe mais")
    check("tolower(static_cast<unsigned char>(token[i]))" in tg_code,
          "containsTokenIgnoreCase converte tambem o token")
    check('hasPrefixIgnoreCase(g_line, "HTTP/")' in tg_code,
          "readResponse valida a linha de status com esse prefixo")

    print("\n18. Guarda de bloqueio (reboot controlado)")
    guard_header = read_text(os.path.join(ROOT, "include", "stall_guard.h"))
    guard_code = strip_comments(read_text(os.path.join(ROOT, "src", "stall_guard.cpp")))
    check("void begin(uint32_t nowMs)" in guard_header and
          "void update(uint32_t nowMs, const Input &input)" in guard_header,
          "modulo expoe begin/update com o estado dos dois servicos")
    check("ESP.restart()" in guard_code, "reinicia o ESP (ultima saida)")
    check("if (!input.wifiConnected)" in guard_code,
          "sem Wi-Fi nao reinicia (o reboot nao ajudaria)")
    check("STALL_GUARD_GRACE_MS" in guard_code, "respeita a carencia do boot")
    check("warnOnce" in guard_code and "STALL_GUARD_FIRST_WARN_MS" in guard_code,
          "avisa uma vez antes de reiniciar")
    check("input.telegramEnabled" in guard_code and "input.mqttEnabled" in guard_code,
          "credenciais pendentes nao contam como bloqueio")
    check("TELEGRAM_STALL_REBOOT_MS" in guard_code and
          "MQTT_STALL_REBOOT_MS" in guard_code,
          "limites lidos de config.h para os dois servicos")
    check("new " not in guard_code and "String" not in guard_code,
          "guarda nao aloca (apenas contadores em .bss)")

    telegram_header = read_text(os.path.join(ROOT, "include", "telegram.h"))
    check("uint32_t lastSuccessMs() const" in telegram_header,
          "Telegram expoe a ultima consulta bem-sucedida")
    check("m_lastSuccessMs = millis();" in telegram,
          "sucesso do getUpdates alimenta a guarda")
    board_header = read_text(os.path.join(ROOT, "include", "thingsboard.h"))
    check("uint32_t lastConnectedMs() const" in board_header,
          "ThingsBoard expoe a ultima sessao aceita")
    check("m_lastConnectedMs = nowMs;" in thingsboard,
          "conexao MQTT aceita alimenta a guarda")
    check("stall_guard::begin(now)" in main_cpp, "guarda iniciada no setup")
    # Carimbo novo: a consulta do Telegram bloqueia ~1,4 s e marca millis()
    # maior que o `now` do inicio do loop; usar o `now` defasado fazia a conta
    # do silencio ficar negativa (4 294 966 s) e reiniciar o ESP no primeiro
    # /help. Medido em campo em 2026-11-20.
    check("stall_guard::update(millis(), guardInput)" in main_cpp,
          "guarda recebe carimbo NOVO (modulos acima podem ter bloqueado)")
    check("static_cast<int32_t>(delta) < 0" in guard_code,
          "carimbo no futuro conta como silencio zero (nao reinicia)")
    check("guardInput.telegramLastOkMs = g_telegram.lastSuccessMs()" in main_cpp and
          "guardInput.mqttLastOkMs = g_tb.lastConnectedMs()" in main_cpp,
          "loop passa os dois instantes de sucesso")
    check("guardInput.mqttEnabled = !tbTokenPending()" in main_cpp,
          "token pendente desliga a contagem do MQTT")
    check("guardInput.telegramEnabled = g_telegram.enabled()" in main_cpp,
          "Telegram desativado nao conta como bloqueio")

    # A conta do silencio e espelhada aqui porque o modulo depende de Arduino.
    # O caso de campo (2026-11-20) esta na lista: o poll do Telegram marcou
    # 61 700 ms e a guarda recebeu 60 270 ms do inicio do loop - a subtracao
    # wrap-safe dava 4 294 966 s e reiniciava o ESP logo depois do /help.
    print("  silenceMs: carimbo no futuro e rollover de millis()")

    def silence_ms(now_ms, last_ok_ms, boot_ms, enabled):
        if not enabled:
            return 0
        reference = boot_ms if last_ok_ms == 0 else last_ok_ms
        delta = (now_ms - reference) & 0xFFFFFFFF
        return 0 if delta >= 0x80000000 else delta

    silence_cases = [
        (60000, 40000, 270, True, 20000, "sucesso recente"),
        (60270, 61700, 270, True, 0, "carimbo no futuro (poll bloqueante)"),
        (60000, 0, 270, True, 59730, "nenhum sucesso: conta desde o boot"),
        (60000, 0, 270, False, 0, "credenciais pendentes nao contam"),
        # 0xFFFFF000 = 4 294 963 200 ms; apos o rollover: 1000 - 0xFFFFF000
        # = 5 096 ms de silencio (nao pode virar ~49 dias).
        (1000, 0xFFFFF000, 0, True, 5096, "rollover de millis()"),
    ]
    for now_ms, last_ok, boot_ms, enabled, expected, label in silence_cases:
        result = silence_ms(now_ms, last_ok, boot_ms, enabled)
        check(result == expected, "%s -> %u ms" % (label, result))
    check(silence_ms(60270, 61700, 270, True) < CFG["STALL_GUARD_FIRST_WARN_MS"],
          "o caso de campo nao dispara aviso nem reboot")


def test_http_helpers():
    print("\n9b. Comparacoes do leitor HTTP (maiusculas/minusculas)")

    print("  hasPrefixIgnoreCase: linha de status")
    prefix_cases = [
        ("HTTP/1.1 200 OK", "HTTP/", True),
        ("http/1.1 200 OK", "HTTP/", True),
        ("Http/1.1 401 Unauthorized", "HTTP/", True),
        ("HTTP/1.1 200 OK", "HTTP/1.1", True),
        ("XTTP/1.1 200 OK", "HTTP/", False),
        ("HTT", "HTTP/", False),
        ("", "HTTP/", False),
        ("HTTP", "HTTP/", False),
        ("HTTP/1.1 200 OK", "HTTPS/", False),
    ]
    for text, prefix, expected in prefix_cases:
        result = has_prefix_ignore_case(text, prefix)
        check(result is expected, "%r com prefixo %r -> %s"
              % (text, prefix, expected))

    print("  hasPrefixIgnoreCase: regressao do defeito original")
    check(has_prefix_ignore_case("HTTP/1.1 200 OK", "HTTP/") is True,
          "prefixo MAIUSCULO 'HTTP/' casa (era o defeito: tolower(text) != prefix cru)")
    check(has_prefix_ignore_case("http/1.1 200 OK", "HTTP/") is True,
          "prefixo maiusculo casa tambem com texto minusculo")

    print("  containsTokenIgnoreCase: token nos cabecalhos")
    token_cases = [
        ("chunked", "chunked", True),
        ("Chunked", "chunked", True),
        ("CHUNKED", "chunked", True),
        ("gzip, chunked", "chunked", True),
        ("gzip, CHUNKED", "chunked", True),
        ("chunked", "CHUNKED", True),
        ("Chunked", "CHUNKED", True),
        ("gzip", "chunked", False),
        ("", "chunked", False),
        ("chunke", "chunked", False),
        ("xchunkedx", "chunked", True),
    ]
    for haystack, token, expected in token_cases:
        result = contains_token_ignore_case(haystack, token)
        check(result is expected, "%r contem %r -> %s"
              % (haystack, token, expected))


def main():
    print("Telegram - verificacao de host")
    test_constants()
    test_commands()
    test_uptime()
    test_help()
    test_boot_notice()
    test_status()
    test_http_helpers()
    test_invariants()

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
