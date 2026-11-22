#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verificador dos alertas de nivel (include/alerts.h + src/alert_monitor.cpp).

Valida, sem device, o que sustenta a preservacao de dados do /w:

  1. LE o layout real de include/alerts.h (falha se o C++ mudar sem o
     verificador acompanhar) e reproduz o registro byte a byte;
  2. CRC32, magic, versao e recusa de registro incoerente;
  3. dois slots A/B com geracao crescente (escolha do mais novo) e sem
     sobrescrever o slot bom quando a gravacao falha;
  4. simulacao de REBOOT: limites e estado sobrevivem, alerta pendente e
     reentregue e NENHUM alerta duplicado e emitido;
  5. fila de pendentes (limite, descarte do mais antigo) e rebaseline do /w;
  6. REBASELINE pos-boot: a MELHORA e silenciosa (nao gera a mensagem enganosa
     "recuperou-se... 100.622 L" logo apos o reboot) e a PIORA segue como alerta
     normal, sem tocar nos pendentes;
  7. CYCLE: o cenario de bancada cobre os QUATRO avisos dentro de um ciclo.

Uso: python check_alerts.py
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
ALERTS_HEADER = os.path.join(ROOT, "include", "alerts.h")
VOLUME_HEADER = os.path.join(ROOT, "include", "volume.h")
CONFIG_HEADER = os.path.join(ROOT, "include", "config.h")
MONITOR_CPP = os.path.join(ROOT, "src", "alert_monitor.cpp")

FAILURES = []


def check(condition, message):
    if condition:
        print("  [ok]   " + message)
    else:
        print("  [FALHOU] " + message)
        FAILURES.append(message)


def read_int(path, name):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"\b" + name + r"\s*=\s*([0-9]+)[uU]?[lL]*;", text)
    if not match:
        raise RuntimeError("constante inteira %s nao encontrada em %s" % (name, path))
    return int(match.group(1))


def read_hex32(path, name):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    match = re.search(r"\b" + name + r"\s*=\s*(0x[0-9A-Fa-f]+)", text)
    if not match:
        raise RuntimeError("constante hex %s nao encontrada em %s" % (name, path))
    return int(match.group(1), 16)


def read_float_constants(path, names):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    values = {}
    for name in names:
        match = re.search(r"\b" + name + r"\s*=\s*([^;]+);", text)
        if not match:
            raise RuntimeError("constante %s nao encontrada em %s" % (name, path))
        values[name] = float(eval(match.group(1).replace("f", "").strip(),
                                  {"__builtins__": {}}, values))
    return values


# ---------------------------------------------------------------------------
#  CRC32 do projeto (queue_record::crc32)
# ---------------------------------------------------------------------------
def crc32(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)))
    return (~crc) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
#  Espelho de include/volume.h (referencia dos percentuais)
# ---------------------------------------------------------------------------
VOL = read_float_constants(
    VOLUME_HEADER,
    ["kAdcReferenceV", "kSensorFullScaleV", "kSensorFullScaleMm",
     "kLevelOffsetMm", "kLevelCorrection", "kLitersPerMm"],
)
MAX_LITERS = (
    ((VOL["kSensorFullScaleMm"] * (VOL["kAdcReferenceV"] / VOL["kSensorFullScaleV"]))
     - VOL["kLevelOffsetMm"]) / VOL["kLevelCorrection"]
) * VOL["kLitersPerMm"]

# ---------------------------------------------------------------------------
#  Layout do registro (include/alerts.h)
# ---------------------------------------------------------------------------
RECORD_SIZE = read_int(ALERTS_HEADER, "RECORD_SIZE")
RECORD_VERSION = read_int(ALERTS_HEADER, "RECORD_VERSION")
RECORD_MAGIC = read_hex32(ALERTS_HEADER, "RECORD_MAGIC")
MAX_PENDING = read_int(ALERTS_HEADER, "MAX_PENDING")

_config_text = open(CONFIG_HEADER, encoding="utf-8").read()
ALERT_FILE_A = re.search(r'ALERT_FILE_A\[\]\s*=\s*"([^"]+)"', _config_text).group(1)
ALERT_FILE_B = re.search(r'ALERT_FILE_B\[\]\s*=\s*"([^"]+)"', _config_text).group(1)
DEFAULT_CFG = {
    "risk": read_int(CONFIG_HEADER, "ALERT_DEFAULT_RISK_PCT"),
    "critical": read_int(CONFIG_HEADER, "ALERT_DEFAULT_CRITICAL_PCT"),
    "hist": read_int(CONFIG_HEADER, "ALERT_DEFAULT_HIST_PCT"),
}
SETTLE_MS = read_int(CONFIG_HEADER, "ALERT_SETTLE_MS")

NORMAL, RISK, CRITICAL = 0, 1, 2
NONE = 0
RISK_ENTERED, CRITICAL_ENTERED, CRITICAL_RECOVERED, RISK_RECOVERED = 1, 2, 3, 4

LEVEL_NAME = {NORMAL: "Normal", RISK: "Atenção", CRITICAL: "Critico"}
EVENT_NAME = {NONE: "nada", RISK_ENTERED: "atingiu a atenção",
              CRITICAL_ENTERED: "atingiu o critico",
              CRITICAL_RECOVERED: "recuperou do critico",
              RISK_RECOVERED: "recuperou da atenção"}


def encode_record(state, generation):
    """Espelho de alerts::encodeRecord() (offsets do comentario do header)."""
    out = bytearray(RECORD_SIZE)
    out[0:4] = struct.pack("<I", RECORD_MAGIC)
    out[4] = RECORD_VERSION
    out[5] = state["risk"]
    out[6] = state["critical"]
    out[7] = state["hist"]
    out[8] = state["level"]
    out[9] = len(state["pending"])
    for index in range(MAX_PENDING):
        base = 10 + 5 * index
        if index < len(state["pending"]):
            event, volume = state["pending"][index]
            out[base] = event
            out[base + 1:base + 5] = struct.pack("<f", volume)
    out[20:24] = struct.pack("<I", generation)
    out[28:32] = struct.pack("<I", crc32(out[0:28]))
    return bytes(out)


def decode_record(raw):
    """Espelho de alerts::decodeRecord(): estado ou None."""
    if raw is None or len(raw) != RECORD_SIZE:
        return None
    if struct.unpack("<I", raw[0:4])[0] != RECORD_MAGIC or raw[4] != RECORD_VERSION:
        return None
    if struct.unpack("<I", raw[28:32])[0] != crc32(raw[0:28]):
        return None

    flat = {"risk": raw[5], "critical": raw[6], "hist": raw[7]}
    if validate(flat) != "OK":
        return None
    if raw[8] > CRITICAL or raw[9] > MAX_PENDING:
        return None

    state = {"risk": raw[5], "critical": raw[6], "hist": raw[7], "level": raw[8],
             "pending": [], "generation": struct.unpack("<I", raw[20:24])[0]}
    for index in range(raw[9]):
        base = 10 + 5 * index
        event = raw[base]
        if event > RISK_RECOVERED:
            return None
        state["pending"].append((event, struct.unpack("<f", raw[base + 1:base + 5])[0]))
    return state


def pct_liters(pct):
    return MAX_LITERS * (pct / 100.0)


def enter_risk(cfg):
    return pct_liters(cfg["risk"])


def enter_critical(cfg):
    return pct_liters(cfg["critical"])


def recover_risk(cfg):
    return pct_liters(cfg["risk"] + cfg["hist"])


def recover_critical(cfg):
    return pct_liters(cfg["critical"] + cfg["hist"])


def validate(cfg):
    if not 1 <= cfg["risk"] <= 100:
        return "RISK_RANGE"
    if not 1 <= cfg["critical"] <= 100:
        return "CRITICAL_RANGE"
    if not 1 <= cfg["hist"] <= 100:
        return "HIST_RANGE"
    if cfg["critical"] >= cfg["risk"]:
        return "ORDER"
    if cfg["risk"] + cfg["hist"] > 100:
        return "HIST_OVERFLOW"
    return "OK"


def step(level, cfg, volume):
    if level == CRITICAL:
        if volume >= recover_critical(cfg):
            return CRITICAL_RECOVERED, RISK
        return NONE, level
    if level == RISK:
        if volume >= recover_risk(cfg):
            return RISK_RECOVERED, NORMAL
        if volume <= enter_critical(cfg):
            return CRITICAL_ENTERED, CRITICAL
        return NONE, level
    if volume <= enter_critical(cfg):
        return CRITICAL_ENTERED, CRITICAL
    if volume <= enter_risk(cfg):
        return RISK_ENTERED, RISK
    return NONE, level


def level_for(cfg, volume):
    if volume <= enter_critical(cfg):
        return CRITICAL
    if volume <= enter_risk(cfg):
        return RISK
    return NORMAL

# ---------------------------------------------------------------------------
#  LittleFS simulado e espelho de AlertMonitor (src/alert_monitor.cpp)
# ---------------------------------------------------------------------------
class Flash:
    """LittleFS simulado: arquivos em memoria e falha de gravacao programavel."""

    def __init__(self):
        self.files = {}
        self.mounted = True
        self.fail_write = None

    def info(self):
        return self.mounted

    def read(self, path):
        return self.files.get(path)

    def write(self, path, data):
        if self.fail_write == path:
            return False
        self.files[path] = bytes(data)
        return True


def latest(flash):
    """Registro valido de maior geracao (escolha do boot)."""
    best = None
    for path in (ALERT_FILE_A, ALERT_FILE_B):
        decoded = decode_record(flash.read(path))
        if decoded is not None and (best is None or
                                    decoded["generation"] >= best["generation"]):
            best = decoded
    return best


class Monitor:
    """Espelho de AlertMonitor: carga, avaliacao, /w e peek/confirm."""

    def __init__(self, flash, boot_ms=0):
        self.flash = flash
        self.boot_ms = boot_ms
        self.state = {"risk": DEFAULT_CFG["risk"],
                      "critical": DEFAULT_CFG["critical"],
                      "hist": DEFAULT_CFG["hist"],
                      "level": NORMAL, "pending": [], "generation": 0}
        self.persisted = False
        self.active_slot = 0
        self.rebaselined = False
        self.load()

    def load(self):
        if not self.flash.info():
            self.persisted = False
            return
        chosen = None
        for index, path in enumerate((ALERT_FILE_A, ALERT_FILE_B)):
            decoded = decode_record(self.flash.read(path))
            if decoded is None:
                continue
            if chosen is None or decoded["generation"] >= chosen["generation"]:
                chosen = decoded
                self.active_slot = index
        if chosen is not None:
            self.state = chosen
            self.persisted = True

    def store(self):
        if not self.flash.info():
            self.persisted = False
            return False
        generation = self.state["generation"] + 1
        path = ALERT_FILE_B if self.active_slot == 0 else ALERT_FILE_A
        if not self.flash.write(path, encode_record(self.state, generation)):
            return False
        self.state["generation"] = generation
        self.active_slot = 1 - self.active_slot
        self.persisted = True
        return True

    def update(self, now_ms, volume, sample_valid=True):
        if not sample_valid or validate(self.state) != "OK":
            return []
        if now_ms - self.boot_ms < SETTLE_MS:
            return []
        # Rebaseline da PRIMEIRA avaliacao util do boot (espelho de
        # AlertMonitor::update): so a MELHORA e silenciosa - o aparelho nao viu a
        # recuperacao e o volume de agora nao e o da travessia. A piora (e o
        # estado igual) seguem pelo step() normal, que ja emite so o alerta
        # devido.
        if not self.rebaselined:
            self.rebaselined = True
            now_level = level_for(self.state, volume)
            if now_level < self.state["level"]:
                self.state["level"] = now_level
                self.store()
        events = []
        for _ in range(MAX_PENDING):
            event, level = step(self.state["level"], self.state, volume)
            if event == NONE:
                break
            self.state["level"] = level
            events.append(event)
            if len(self.state["pending"]) >= MAX_PENDING:
                self.state["pending"].pop(0)  # descarta o MAIS ANTIGO
            self.state["pending"].append((event, volume))
        if events:
            self.store()
        return events

    def apply_config(self, cfg, volume, sample_valid=True):
        error = validate(cfg)
        if error != "OK":
            return error
        self.state["risk"] = cfg["risk"]
        self.state["critical"] = cfg["critical"]
        self.state["hist"] = cfg["hist"]
        if sample_valid:
            self.state["level"] = level_for(cfg, volume)
        self.state["pending"] = []
        # O /w ja recalculou o estado ("rebaseline" do operador): a primeira
        # avaliacao do boot nao tem mais o que rebaselinar (um /w pode chegar
        # antes do settle).
        self.rebaselined = True
        self.store()
        return "OK"

    def peek(self):
        return self.state["pending"][0] if self.state["pending"] else None

    def confirm(self):
        if self.state["pending"]:
            self.state["pending"].pop(0)
            self.store()

def main():
    print("Alertas de nivel - verificacao de host")
    alerts_header = open(ALERTS_HEADER, encoding="utf-8").read()
    monitor_cpp = open(MONITOR_CPP, encoding="utf-8").read()

    print("\n1. Layout lido do header C++")
    check(RECORD_SIZE == 32, "registro de 32 B (lido: %d)" % RECORD_SIZE)
    check(RECORD_MAGIC == 0x47464357, "magic 'WCFG'")
    check(MAX_PENDING == 2, "fila de %d pendentes" % MAX_PENDING)
    check("11..14  pendente 0" in alerts_header and "16..19  pendente 1" in alerts_header
          and "28..31  CRC32" in alerts_header, "comentario de layout no header")
    check("crc32(out, 28)" in alerts_header and "get32(in + 28)" in alerts_header,
          "CRC32 cobre os 28 primeiros bytes (offset 28 nos dois sentidos)")
    check("putFloat(out + 11" in alerts_header and "putFloat(out + 16" in alerts_header,
          "volume de cada pendente gravado como float32")
    check(ALERT_FILE_A != ALERT_FILE_B and ALERT_FILE_A.startswith("/"),
          "dois arquivos A/B na raiz do FS (%s, %s)" % (ALERT_FILE_A, ALERT_FILE_B))
    check(SETTLE_MS > 0, "settle de %d ms antes da primeira avaliacao" % SETTLE_MS)
    check("ALERT_SETTLE_MS" in monitor_cpp and "alerts::levelFor" in monitor_cpp,
          "runtime usa o settle e o rebaseline")
    check("m_state.generation + 1" in monitor_cpp and "m_activeSlot" in monitor_cpp,
          "runtime alterna os slots com geracao crescente")

    print("\n2. Ida e volta e recusa de registro incoerente")
    flash = Flash()
    monitor = Monitor(flash)
    check(not monitor.persisted, "sem arquivo: opera em RAM com os defaults")
    check((monitor.state["risk"], monitor.state["critical"], monitor.state["hist"]) ==
          (DEFAULT_CFG["risk"], DEFAULT_CFG["critical"], DEFAULT_CFG["hist"]),
          "defaults de config.h: %d/%d/%d" % (DEFAULT_CFG["risk"],
                                              DEFAULT_CFG["critical"],
                                              DEFAULT_CFG["hist"]))
    monitor.state["level"] = RISK
    monitor.state["pending"] = [(RISK_ENTERED, 12345.5)]
    raw = encode_record(monitor.state, 7)
    check(len(raw) == RECORD_SIZE, "registro codificado tem %d B" % len(raw))
    decoded = decode_record(raw)
    check(decoded is not None, "registro valido e aceito")
    check(decoded["risk"] == DEFAULT_CFG["risk"] and decoded["level"] == RISK,
          "limites e estado preservados")
    check(decoded["pending"] == [(RISK_ENTERED, 12345.5)],
          "pendente com tipo e volume")
    check(decoded["generation"] == 7, "geracao preservada")

    corrupted = bytearray(raw)
    corrupted[12] ^= 0x01
    check(decode_record(bytes(corrupted)) is None, "um byte alterado invalida o CRC")
    bad_magic = bytearray(raw)
    bad_magic[0] ^= 0xFF
    check(decode_record(bytes(bad_magic)) is None, "magic errado e recusado")
    bad_version = bytearray(raw)
    bad_version[4] = 9
    check(decode_record(bytes(bad_version)) is None, "versao desconhecida e recusada")
    bad_level = bytearray(raw)
    bad_level[8] = 7
    bad_level[28:32] = struct.pack("<I", crc32(bad_level[0:28]))
    check(decode_record(bytes(bad_level)) is None, "estado desconhecido e recusado")
    bad_cfg = bytearray(raw)
    bad_cfg[5], bad_cfg[6] = DEFAULT_CFG["critical"], DEFAULT_CFG["risk"]
    bad_cfg[28:32] = struct.pack("<I", crc32(bad_cfg[0:28]))
    check(decode_record(bytes(bad_cfg)) is None,
          "critico >= atenção no registro e recusado")
    check(decode_record(None) is None and decode_record(b"\x00" * 8) is None,
          "arquivo ausente ou curto e recusado")

    print("\n3. Dois slots A/B com geracao crescente")
    flash = Flash()
    monitor = Monitor(flash)
    check(monitor.apply_config({"risk": 70, "critical": 40, "hist": 5}, 90000.0) == "OK",
          "limites aplicados e gravados")
    check(monitor.persisted and ALERT_FILE_B in flash.files and
          ALERT_FILE_A not in flash.files,
          "a primeira gravacao cria o slot B (o ativo era o A)")
    monitor.apply_config({"risk": 75, "critical": 45, "hist": 5}, 90000.0)
    check(ALERT_FILE_A in flash.files and ALERT_FILE_B in flash.files,
          "a segunda gravacao usa o OUTRO slot")
    generation_a = decode_record(flash.files[ALERT_FILE_A])["generation"]
    generation_b = decode_record(flash.files[ALERT_FILE_B])["generation"]
    check(generation_a == 2 and generation_b == 1,
          "geracoes crescentes (%d e %d)" % (generation_a, generation_b))

    reborn = Monitor(flash)
    check(reborn.state["risk"] == 75 and reborn.state["critical"] == 45,
          "reboot carrega o slot de maior geracao")
    check(reborn.persisted, "configuracao carregada do flash")

    # A gravacao sempre vai para o slot INATIVO: falhar nele e o que preserva o
    # registro bom (o ativo).
    active = ALERT_FILE_A if reborn.active_slot == 0 else ALERT_FILE_B
    inactive = ALERT_FILE_B if reborn.active_slot == 0 else ALERT_FILE_A
    flash.fail_write = inactive
    reborn.apply_config({"risk": 80, "critical": 50, "hist": 5}, 90000.0)
    check(decode_record(flash.files[active])["risk"] == 75,
          "gravacao que falha NAO sobrescreve o slot bom")
    check(reborn.persisted, "o registro anterior continua valido (persistido)")
    check(Monitor(flash).state["risk"] == 75,
          "reboot apos a falha volta ao ultimo estado gravado com sucesso")

    print("\n4. Reboot: pendente reentregue e sem alerta duplicado")
    flash = Flash()
    monitor = Monitor(flash)
    monitor.apply_config({"risk": 60, "critical": 30, "hist": 5}, 90000.0)
    events = monitor.update(SETTLE_MS, 30000.0)
    check(events == [CRITICAL_ENTERED], "queda direta para o critico gera UM evento")
    check(monitor.state["level"] == CRITICAL, "estado vai para CRITICAL")
    check(monitor.peek() == (CRITICAL_ENTERED, 30000.0), "evento fica pendente")

    reborn = Monitor(flash)
    check(reborn.peek() == (CRITICAL_ENTERED, 30000.0),
          "pendente sobrevive ao reboot ANTES do envio")
    check(reborn.state["level"] == CRITICAL, "estado critico preservado no reboot")
    check(reborn.update(SETTLE_MS, 30000.0) == [],
          "mesmo volume apos o reboot NAO gera alerta novo (sem duplicar)")
    check(reborn.peek() == (CRITICAL_ENTERED, 30000.0), "o pendente continua o mesmo")

    reborn.confirm()
    check(reborn.peek() is None, "confirmacao entrega o alerta")
    check(latest(flash)["pending"] == [], "a fila vazia e persistida")
    check(reborn.update(SETTLE_MS, 30000.0) == [], "ja entregue: nada e reenviado")
    third = Monitor(flash)
    check(third.peek() is None and third.state["level"] == CRITICAL,
          "reboot depois da entrega nao reenvia o alerta")

    print("\n5. Recuperacao, histerese e transicoes em cadeia")
    flash = Flash()
    monitor = Monitor(flash)
    monitor.apply_config({"risk": 60, "critical": 30, "hist": 5}, 90000.0)
    monitor.update(SETTLE_MS, 20000.0)
    monitor.confirm()
    check(monitor.state["level"] == CRITICAL, "estado critico apos a queda")
    check(monitor.update(SETTLE_MS + 1000, 40000.0) == [],
          "acima do critico mas abaixo de critico+hist: nada")
    events = monitor.update(SETTLE_MS + 2000, 41000.0)
    check(events == [CRITICAL_RECOVERED], "critico + hist recupera o critico")
    reborn = Monitor(flash)
    check(reborn.state["level"] == RISK,
          "reboot no meio da recuperacao preserva o estado RISK")
    check(reborn.peek() == (CRITICAL_RECOVERED, 41000.0),
          "mensagem de recuperacao ainda nao enviada sobrevive ao reboot")
    reborn.confirm()
    # O tanque ja tinha subido acima de atenção+hist ENQUANTO o aparelho reiniciava:
    # a travessia nao foi observada, entao o reboot rebaselina em silencio (secao
    # 8). A recuperacao AO VIVO - a que interessa - vem logo abaixo.
    check(reborn.update(SETTLE_MS + 3000, 80000.0) == [] and
          reborn.state["level"] == NORMAL,
          "reboot + tanque cheio: recuperacao nao observada nao gera mensagem")
    check(reborn.update(SETTLE_MS + 4000, 50000.0) == [RISK_ENTERED],
          "queda ao vivo volta a entrar na atenção")
    reborn.confirm()
    check(reborn.update(SETTLE_MS + 5000, 80000.0) == [RISK_RECOVERED],
          "acima de atenção+hist recupera a atenção (ao vivo)")
    reborn.confirm()
    check(reborn.state["level"] == NORMAL, "estado volta a NORMAL")

    flash2 = Flash()
    chained = Monitor(flash2)
    chained.apply_config({"risk": 60, "critical": 30, "hist": 5}, 20000.0)
    check(chained.state["level"] == CRITICAL, "rebaseline com volume critico")
    check(chained.state["pending"] == [],
          "rebaseline do /w NAO gera alerta (o operador so ajustou os limites)")
    events = chained.update(SETTLE_MS, 90000.0)
    check(events == [CRITICAL_RECOVERED, RISK_RECOVERED],
          "salto grande gera as duas recuperacoes")
    check(len(chained.state["pending"]) == 2, "as duas ficam na fila")
    check(latest(flash2)["pending"] == chained.state["pending"],
          "a fila com dois pendentes e persistida")

    print("\n6. Fila cheia, settle e LittleFS ausente")
    flash = Flash()
    monitor = Monitor(flash)
    monitor.apply_config({"risk": 60, "critical": 30, "hist": 5}, 20000.0)
    monitor.update(SETTLE_MS, 90000.0)
    check(len(monitor.state["pending"]) == 2, "fila cheia com 2 pendentes")
    monitor.update(SETTLE_MS + 1, 20000.0)
    check(monitor.state["pending"][-1] == (CRITICAL_ENTERED, 20000.0),
          "o evento NOVO entra na fila")
    check(monitor.state["pending"][0] == (RISK_RECOVERED, 90000.0) and
          len(monitor.state["pending"]) == 2,
          "fila cheia descarta o alerta MAIS ANTIGO")

    flash = Flash()
    monitor = Monitor(flash)
    monitor.apply_config({"risk": 60, "critical": 30, "hist": 5}, 90000.0)
    check(monitor.update(SETTLE_MS - 1, 20000.0) == [],
          "antes do settle nada e avaliado")
    check(monitor.state["level"] == NORMAL and monitor.state["pending"] == [],
          "estado e fila intactos antes do settle")
    check(monitor.update(SETTLE_MS, 20000.0) == [CRITICAL_ENTERED],
          "no settle a avaliacao comeca")
    check(monitor.update(SETTLE_MS + 1, 20000.0, sample_valid=False) == [],
          "sem amostra valida nada e avaliado")

    flash = Flash()
    flash.mounted = False
    monitor = Monitor(flash)
    check(not monitor.persisted and flash.files == {},
          "sem LittleFS nada e gravado")
    monitor.apply_config({"risk": 70, "critical": 40, "hist": 5}, 90000.0)
    check(not monitor.persisted and flash.files == {},
          "aplicar limites sem FS roda em RAM e avisa que nao persistiu")
    check(Monitor(flash).state["risk"] == DEFAULT_CFG["risk"],
          "sem FS o reboot volta aos defaults de config.h")

    print("\n7. Limites invalidos nao alteram nada")
    flash = Flash()
    monitor = Monitor(flash)
    monitor.apply_config({"risk": 70, "critical": 40, "hist": 5}, 90000.0)
    before = latest(flash)
    for candidate, expected in (({"risk": 40, "critical": 40, "hist": 5}, "ORDER"),
                                ({"risk": 30, "critical": 40, "hist": 5}, "ORDER"),
                                ({"risk": 98, "critical": 40, "hist": 5}, "HIST_OVERFLOW"),
                                ({"risk": 0, "critical": 40, "hist": 5}, "RISK_RANGE"),
                                ({"risk": 90, "critical": 0, "hist": 5}, "CRITICAL_RANGE"),
                                ({"risk": 90, "critical": 40, "hist": 0}, "HIST_RANGE")):
        check(monitor.apply_config(candidate, 90000.0) == expected,
              "%r recusado com %s" % (candidate, expected))
    check(monitor.state["risk"] == 70 and monitor.state["critical"] == 40,
          "limites vigentes mantidos apos as recusas")
    check(latest(flash)["generation"] == before["generation"],
          "configuracao recusada NAO grava no flash")

    print("\n8. Rebaseline pos-boot: melhora silenciosa, piora anunciada")
    normal_liters = pct_liters(80)
    risk_liters = pct_liters(45)
    critical_liters = pct_liters(10)

    def boot_with(level, volume, pending=()):
        """Reboot com um registro persistido e a primeira avaliacao util."""
        flash = Flash()
        state = {"risk": DEFAULT_CFG["risk"], "critical": DEFAULT_CFG["critical"],
                 "hist": DEFAULT_CFG["hist"], "level": level,
                 "pending": list(pending), "generation": 7}
        flash.files[ALERT_FILE_A] = encode_record(state, 7)
        monitor = Monitor(flash, boot_ms=0)
        return monitor, monitor.update(SETTLE_MS + 100, volume)

    # MELHORA: o aparelho nao viu a recuperacao -> nenhuma mensagem, mas o estado
    # passa a refletir o volume de agora.
    for persisted, volume, expected in ((CRITICAL, normal_liters, NORMAL),
                                        (CRITICAL, risk_liters, RISK),
                                        (RISK, normal_liters, NORMAL)):
        monitor, events = boot_with(persisted, volume)
        check(events == [] and monitor.state["level"] == expected,
              "persistido %s -> %.0f L: silencioso, estado %s"
              % (LEVEL_NAME[persisted], volume, LEVEL_NAME[expected]))

    # PIORA: um UNICO alerta (o degrau direto nao duplica o de atenção) com o nivel
    # de AGORA, que e a informacao util para quem le.
    for persisted, volume, event, expected in (
            (NORMAL, critical_liters, CRITICAL_ENTERED, CRITICAL),
            (NORMAL, risk_liters, RISK_ENTERED, RISK),
            (RISK, critical_liters, CRITICAL_ENTERED, CRITICAL)):
        monitor, events = boot_with(persisted, volume)
        check(events == [event] and monitor.state["level"] == expected,
              "piora %s -> %.0f L: um alerta (%s)"
              % (LEVEL_NAME[persisted], volume, EVENT_NAME[event]))
        pending = monitor.peek()
        check(pending is not None and pending[0] == event and
              abs(pending[1] - volume) < 1.0,
              "o alerta leva o volume de AGORA (%.0f L)" % volume)

    # IGUAL: nada e enviado (e o que impede alerta repetido a cada reboot).
    for level, volume in ((CRITICAL, critical_liters), (RISK, risk_liters),
                          (NORMAL, normal_liters)):
        monitor, events = boot_with(level, volume)
        check(events == [] and monitor.state["level"] == level,
              "estado %s sem mudanca: reboot nao repete alerta"
              % LEVEL_NAME[level])

    # O caso de campo (2026-11-20): 2 pendentes reais + 2 "recuperacoes" espurias
    # de reboot estouravam a fila de 2 e descartavam o alerta CRITICO mais antigo.
    # Com o rebaseline silencioso a fila fica intacta.
    flash = Flash()
    state = {"risk": 60, "critical": 30, "hist": 5, "level": CRITICAL,
             "pending": [(RISK_ENTERED, 69917.0), (CRITICAL_ENTERED, 34803.0)],
             "generation": 2}
    flash.files[ALERT_FILE_A] = encode_record(state, 2)
    monitor = Monitor(flash, boot_ms=0)
    check(monitor.update(SETTLE_MS + 100, normal_liters) == [],
          "reboot com o tanque cheio: nenhuma recuperacao espuria")
    check([event for event, _ in monitor.state["pending"]] ==
          [RISK_ENTERED, CRITICAL_ENTERED],
          "os DOIS pendentes reais sobrevivem (o critico de 34.803 L nao se perde)")

    check("m_rebaselined" in monitor_cpp and "m_rebaselined = false;" in monitor_cpp,
          "rebaseline roda uma unica vez por boot")
    check("m_rebaselined = true;" in monitor_cpp,
          "o /w consome o rebaseline (comando pode chegar antes do settle)")
    check("alerts::levelFor(m_state.config, volumeL)" in monitor_cpp,
          "o nivel de agora vem de levelFor() (sem evento)")
    check("static_cast<uint8_t>(now) < static_cast<uint8_t>(m_state.level)"
          in monitor_cpp,
          "so a MELHORA e silenciosa (comparacao pelo valor do enum)")
    check("estado pos-reboot %s -> %s" in monitor_cpp and
          "[sem evento]" in monitor_cpp and "[alerta emitido]" in monitor_cpp,
          "uma linha de log, com sufixo por caso")
    monitor_header = open(os.path.join(ROOT, "include", "alert_monitor.h"),
                          encoding="utf-8").read()
    check("levelForVolume" in monitor_header and
          "alerts::levelFor(m_state.config, volumeL)" in monitor_header,
          "levelForVolume() expoe o estado de AGORA para o aviso de boot")

    print("\n9. Cenário CYCLE: um ciclo cobre os QUATRO avisos")
    sim = read_float_constants(CONFIG_HEADER,
                              ["SIM_DRAINING_START_MM", "SIM_DRAINING_END_MM"])
    drain_ms = read_int(CONFIG_HEADER, "SIM_CYCLE_DRAIN_MS")
    fill_ms = read_int(CONFIG_HEADER, "SIM_CYCLE_FILL_MS")
    simulator_cpp = open(os.path.join(ROOT, "src", "simulator.cpp"),
                         encoding="utf-8").read()
    top = sim["SIM_DRAINING_START_MM"]
    bottom = sim["SIM_DRAINING_END_MM"]
    period_ms = drain_ms + fill_ms

    def cycle_liters(ms):
        """Espelho do cenario CYCLE de src/simulator.cpp (onda triangular)."""
        phase = ms % period_ms
        if phase < drain_ms:
            mm = top + (bottom - top) * (phase / drain_ms)
        else:
            mm = bottom + (top - bottom) * ((phase - drain_ms) / fill_ms)
        return mm * VOL["kLitersPerMm"]

    check("CYCLE," in _config_text and
          "SIM_SCENARIO = SimScenario::CYCLE" in _config_text,
          "CYCLE existe e e o cenario de bancada")
    check("case SimScenario::CYCLE" in simulator_cpp and '"CYCLE"' in simulator_cpp,
          "o simulador implementa e nomeia o cenario")
    check("fmodf(" in simulator_cpp,
          "o ciclo se repete pelo modulo do periodo (sem reiniciar o aparelho)")

    level = NORMAL
    events = []
    ms = 0
    while ms <= period_ms:
        event, new_level = step(level, DEFAULT_CFG, cycle_liters(ms))
        if event != NONE:
            events.append((ms / 1000.0, event))
            level = new_level
        ms += 250
    check([event for _, event in events] ==
          [RISK_ENTERED, CRITICAL_ENTERED, CRITICAL_RECOVERED, RISK_RECOVERED],
          "os quatro avisos saem em um ciclo (vistos: %s)"
          % ", ".join(EVENT_NAME[event] for _, event in events))
    if len(events) == 4:
        times = [time_s for time_s, _ in events]
        check(times == sorted(times),
              "em ordem: %s" % ", ".join("%.0f s" % t for t in times))
        check(times[0] > SETTLE_MS / 1000.0,
              "o primeiro alerta vem depois do settle do boot (%.0f s)" % times[0])
        check(times[-1] < period_ms / 1000.0 - 5.0,
              "o quarto aviso cabe no ciclo (%.0f s < %.0f s)"
              % (times[-1], period_ms / 1000.0))

    # Margens: sem elas o cenario nao exercita entrada E recuperacao.
    check(cycle_liters(0) >= recover_risk(DEFAULT_CFG),
          "comeca em %.0f L, acima da recuperacao da atenção (%.0f L)"
          % (cycle_liters(0), recover_risk(DEFAULT_CFG)))
    check(cycle_liters(drain_ms) <= enter_critical(DEFAULT_CFG),
          "desce a %.0f L, abaixo da entrada do critico (%.0f L)"
          % (cycle_liters(drain_ms), enter_critical(DEFAULT_CFG)))
    check(abs(cycle_liters(period_ms) - cycle_liters(0)) < 1.0,
          "e volta ao topo (ciclo periodico)")
    check(cycle_liters(SETTLE_MS) > recover_risk(DEFAULT_CFG),
          "no settle do boot ainda esta Normal (%.0f L): o rebaseline sai "
          "silencioso" % cycle_liters(SETTLE_MS))

    print()
    if FAILURES:
        print("%d verificacao(oes) falharam:" % len(FAILURES))
        for item in FAILURES:
            print("  - " + item)
        return 1
    print("TODOS OS TESTES PASSARAM (0 falha(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())