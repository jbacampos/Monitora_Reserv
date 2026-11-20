#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verificador do historico diario (layout binario + regras do anel/cobertura).

O firmware grava duas familias de arquivos em LittleFS:

  /h-days-a.dat | /h-days-b.dat   tabela circular de 30 dias (1600 B)
  /h-cur-a.dat  | /h-cur-b.dat    snapshot do dia corrente (52 B)

Nao ha compilador C++ nativo nesta maquina (ver README). Este script:

  1. LE as constantes reais de ../../include/history_record.h e de
     ../../include/config.h, falhando se o layout C++ mudar sem que o
     verificador acompanhe;
  2. reproduz byte a byte o formato (registro, cabecalho, rodape, CRC32);
  3. simula a flash e o proprio modulo (gate de 10 s, virada de dia, snapshot
     A/B, tabela A/B, dia pendente) para testar rotacao do anel, slots vazios e
     corrompidos, cobertura por dia e agregacao de periodos.

Uso: python check_history.py
"""

import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
RECORD_HEADER = os.path.join(HERE, "..", "..", "include", "history_record.h")
CONFIG_HEADER = os.path.join(HERE, "..", "..", "include", "config.h")

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
        match = re.search(r"\b" + name + r"\s*=\s*([^;]+);", text)
        if not match:
            raise RuntimeError("constante %s nao encontrada em %s" % (name, path))
        expression = match.group(1).replace("UL", "").replace("ull", "").strip()
        values[name] = int(eval(expression, environment))
        environment[name] = values[name]
    return values


C = read_constants(
    RECORD_HEADER,
    [
        "RECORD_SIZE",
        "RECORD_VERSION",
        "TABLE_MAGIC",
        "FOOTER_MAGIC",
        "TABLE_VERSION",
        "HEADER_SIZE",
        "FOOTER_SIZE",
        "SLOTS",
        "LOCAL_OFFSET_SECONDS",
        "DAY_SECONDS",
        "NOMINAL_SAMPLES_PER_DAY",
        "FLAG_RESTARTED",
        "FLAG_LATE_START",
        "FLAG_EARLY_END",
        "HEADER_DEGRADED",
    ],
)
CFG = read_constants(
    CONFIG_HEADER,
    [
        "HIST_SAMPLE_INTERVAL_MS",
        "HIST_SNAPSHOT_INTERVAL_MS",
        "HIST_RETRY_INTERVAL_MS",
        "HIST_GAP_SECONDS",
        "HIST_MIN_FREE_BLOCKS",
    ],
)
COMPLETE_RATIO = 0.995  # HIST_COMPLETE_RATIO (float; lido abaixo)
with open(CONFIG_HEADER, "r", encoding="utf-8") as handle:
    _text = handle.read()
_m = re.search(r"HIST_COMPLETE_RATIO\s*=\s*([0-9.]+)f", _text)
if not _m:
    raise RuntimeError("HIST_COMPLETE_RATIO nao encontrada em config.h")
COMPLETE_RATIO = float(_m.group(1))

RECORD_SIZE = C["RECORD_SIZE"]
HEADER_SIZE = C["HEADER_SIZE"]
FOOTER_SIZE = C["FOOTER_SIZE"]
SLOTS = C["SLOTS"]
TABLE_SIZE = HEADER_SIZE + SLOTS * RECORD_SIZE + FOOTER_SIZE
INTERVAL_MS = CFG["HIST_SAMPLE_INTERVAL_MS"]
INTERVAL_S = INTERVAL_MS // 1000
BLOCK = 4096
MIN_FREE = CFG["HIST_MIN_FREE_BLOCKS"] * BLOCK



# ---------------------------------------------------------------------------
#  Formato binario (espelho de history_record.h)
# ---------------------------------------------------------------------------
def crc32(data):
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)))
    return (~crc) & 0xFFFFFFFF


def put16(value):
    return struct.pack("<H", value)


def put32(value):
    return struct.pack("<I", value)


def put64(value):
    return struct.pack("<Q", value)


def get16(data, offset=0):
    return struct.unpack_from("<H", data, offset)[0]


def get32(data, offset=0):
    return struct.unpack_from("<I", data, offset)[0]


def get64(data, offset=0):
    return struct.unpack_from("<Q", data, offset)[0]


def is_empty(buffer):
    return all(byte == 0 for byte in buffer)


def encode_record(record):
    body = (
        put32(record["gen"])
        + put32(record["day"])
        + struct.pack("<f", record["min_l"])
        + put32(record["ts_min"])
        + struct.pack("<f", record["max_l"])
        + put32(record["ts_max"])
        + struct.pack("<d", record["sum_l"])
        + put32(record["n"])
        + put32(record["first_ts"])
        + put32(record["last_ts"])
        + put16(record["gaps"])
        + bytes([record["flags"], C["RECORD_VERSION"]])
    )
    assert len(body) == RECORD_SIZE - 4, len(body)
    return body + put32(crc32(body))


def decode_record(buffer):
    if is_empty(buffer):
        return "EMPTY", None
    if buffer[47] != C["RECORD_VERSION"] or get32(buffer, 48) != crc32(buffer[:48]):
        return "CORRUPT", None
    day = get32(buffer, 4)
    if day == 0:
        return "CORRUPT", None
    record = {
        "gen": get32(buffer, 0),
        "day": day,
        "min_l": struct.unpack_from("<f", buffer, 8)[0],
        "ts_min": get32(buffer, 12),
        "max_l": struct.unpack_from("<f", buffer, 16)[0],
        "ts_max": get32(buffer, 20),
        "sum_l": struct.unpack_from("<d", buffer, 24)[0],
        "n": get32(buffer, 32),
        "first_ts": get32(buffer, 36),
        "last_ts": get32(buffer, 40),
        "gaps": get16(buffer, 44),
        "flags": buffer[46],
    }
    return "VALID", record


def decode_slot(buffer, slot):
    state, record = decode_record(buffer)
    if state != "VALID":
        return state, None
    if record["day"] % SLOTS != slot:
        return "CORRUPT", None
    return "VALID", record


def encode_header(header):
    body = (
        put32(C["TABLE_MAGIC"])
        + bytes([C["TABLE_VERSION"], HEADER_SIZE])
        + put16(header["flags"])
        + put32(header["gen"])
        + put32(header["first_day"])
        + put32(header["last_day"])
        + put16(header["count"])
        + put16(0)
    )
    return body + put32(crc32(body))


def decode_header(buffer):
    if (
        get32(buffer, 0) != C["TABLE_MAGIC"]
        or buffer[4] != C["TABLE_VERSION"]
        or buffer[5] != HEADER_SIZE
        or get32(buffer, 24) != crc32(buffer[:24])
    ):
        return None
    return {
        "flags": get16(buffer, 6),
        "gen": get32(buffer, 8),
        "first_day": get32(buffer, 12),
        "last_day": get32(buffer, 16),
        "count": get16(buffer, 20),
    }


def encode_footer(count):
    body = put32(C["FOOTER_MAGIC"]) + put16(count) + put16(C["TABLE_VERSION"])
    return body + put32(crc32(body))


def decode_footer(buffer, expected):
    return (
        get32(buffer, 0) == C["FOOTER_MAGIC"]
        and get16(buffer, 4) == expected
        and get16(buffer, 6) == C["TABLE_VERSION"]
        and get32(buffer, 8) == crc32(buffer[:8])
    )


def day_index(epoch_seconds):
    return (epoch_seconds + C["LOCAL_OFFSET_SECONDS"]) // C["DAY_SECONDS"]


def day_start(day):
    return day * C["DAY_SECONDS"] - C["LOCAL_OFFSET_SECONDS"]


def make_record(day, samples, first_ts, last_ts, value=100.0, gaps=0, flags=0,
                minimum=None, maximum=None):
    total = float(value) * samples
    return {
        "gen": 1,
        "day": day,
        "min_l": value if minimum is None else minimum,
        "ts_min": first_ts,
        "max_l": value if maximum is None else maximum,
        "ts_max": last_ts,
        "sum_l": total,
        "n": samples,
        "first_ts": first_ts,
        "last_ts": last_ts,
        "gaps": gaps,
        "flags": flags,
    }




# ---------------------------------------------------------------------------
#  Flash simulada e modelo do modulo (espelho de src/history.cpp)
# ---------------------------------------------------------------------------
class FakeFS:
    def __init__(self, free=900000):
        self.files = {}
        self.free = free
        self.writable = True
        self.writes = {}

    def write(self, path, data):
        if not self.writable:
            return False
        if path not in self.files:
            self.free -= BLOCK
        self.files[path] = bytes(data)
        self.writes[path] = self.writes.get(path, 0) + 1
        return True

    def read(self, path):
        return self.files.get(path)

    def remove(self, path):
        if not self.writable:
            return False
        if path in self.files:
            del self.files[path]
            self.free += BLOCK
            return True
        return False


class Scan:
    def __init__(self):
        self.header = {"flags": 0, "gen": 0, "first_day": 0, "last_day": 0, "count": 0}
        self.valid = 0
        self.empty = 0
        self.corrupt = 0
        self.first_day = 0
        self.last_day = 0
        self.size_ok = False
        self.header_ok = False
        self.footer_ok = False

    def usable(self):
        return self.size_ok and self.header_ok and self.footer_ok


class HistoryModel:
    TABLE = ("/h-days-a.dat", "/h-days-b.dat")
    SNAPSHOT = ("/h-cur-a.dat", "/h-cur-b.dat")

    def __init__(self, fs):
        self.fs = fs
        self.active_table = 0
        self.have_table = False
        self.table_gen = 0
        self.active_snap = 0
        self.have_snapshot = False
        self.snap_gen = 0
        self.snap = None
        self.acc = None
        self.pending_rec = None
        self.day_started = False
        self.pending = False
        self.degraded = False
        self.last_count_ms = 0
        self.last_snap_ms = 0
        self.last_snap_n = 0
        self.last_attempt_ms = -CFG["HIST_RETRY_INTERVAL_MS"]
        self.write_failures = 0

    # -- leitura ------------------------------------------------------------
    def scan_table(self, index, skip_slot=None):
        scan = Scan()
        data = self.fs.read(self.TABLE[index])
        if data is None:
            return scan
        scan.size_ok = len(data) == TABLE_SIZE
        if scan.size_ok:
            header = decode_header(data[:HEADER_SIZE])
            if header is not None:
                scan.header = header
                scan.header_ok = True
            for slot in range(SLOTS):
                offset = HEADER_SIZE + slot * RECORD_SIZE
                buffer = data[offset:offset + RECORD_SIZE]
                if len(buffer) < RECORD_SIZE:
                    scan.size_ok = False
                    break
                state, record = decode_slot(buffer, slot)
                if state == "EMPTY":
                    scan.empty += 1
                elif state == "CORRUPT":
                    scan.corrupt += 1
                elif slot != skip_slot:
                    scan.valid += 1
                    if scan.valid == 1 or record["day"] < scan.first_day:
                        scan.first_day = record["day"]
                    if scan.valid == 1 or record["day"] > scan.last_day:
                        scan.last_day = record["day"]
        if scan.size_ok:
            offset = HEADER_SIZE + SLOTS * RECORD_SIZE
            footer = data[offset:offset + FOOTER_SIZE]
            scan.footer_ok = (
                len(footer) == FOOTER_SIZE
                and decode_footer(footer, scan.header["count"])
            )
        return scan

    def begin(self):
        scans = [self.scan_table(0), self.scan_table(1)]
        chosen = None
        for index in (0, 1):
            if not scans[index].usable():
                continue
            if chosen is None or scans[index].header["gen"] >= scans[chosen].header["gen"]:
                chosen = index
        if chosen is not None:
            self.active_table = chosen
            self.have_table = True
            self.table_gen = scans[chosen].header["gen"]
            if scans[chosen].header["flags"] & C["HEADER_DEGRADED"]:
                self.degraded = True

        found = False
        for index in (0, 1):
            data = self.fs.read(self.SNAPSHOT[index])
            if data is None or len(data) != RECORD_SIZE:
                continue
            state, record = decode_record(data)
            if state != "VALID":
                continue
            if not found or record["gen"] >= self.snap["gen"]:
                self.snap = record
                self.snap_gen = record["gen"]
                self.active_snap = index
                found = True
        self.have_snapshot = found and self.snap["n"] > 0
        return scans[chosen] if chosen is not None else None

    # -- ciclo do dia -------------------------------------------------------
    def update(self, now_ms, ts_ms, liters, synced=True, valid=True):
        if not synced or not valid or ts_ms == 0:
            return
        day = day_index(ts_ms // 1000)

        if not self.day_started:
            self.start_first_day(now_ms, day, liters, ts_ms)
            self.maybe_snapshot(now_ms)
            return

        if day > self.acc["day"]:
            self.close_day()
            self.start_day(now_ms, day, liters, ts_ms)
            if self.pending:
                self.write_pending(now_ms)
            self.maybe_snapshot(now_ms)
            return

        if day < self.acc["day"]:
            return

        if self.pending and (now_ms - self.last_attempt_ms) >= CFG["HIST_RETRY_INTERVAL_MS"]:
            self.write_pending(now_ms)

        if (now_ms - self.last_count_ms) < INTERVAL_MS:
            self.maybe_snapshot(now_ms)
            return

        self.accumulate(now_ms, liters, ts_ms)
        self.maybe_snapshot(now_ms)

    def start_first_day(self, now_ms, day, liters, ts_ms):
        had_snapshot = self.have_snapshot
        self.have_snapshot = False

        if had_snapshot and self.snap["day"] == day:
            now_s = ts_ms // 1000
            self.acc = dict(self.snap)
            self.day_started = True
            if now_s > self.acc["last_ts"] + CFG["HIST_GAP_SECONDS"] and self.acc["gaps"] < 0xFFFF:
                self.acc["gaps"] += 1
            self.acc["flags"] |= C["FLAG_RESTARTED"]
            self.last_count_ms = now_ms - INTERVAL_MS
            self.last_snap_ms = now_ms
            self.last_snap_n = self.acc["n"]
            self.accumulate(now_ms, liters, ts_ms)
            return

        if had_snapshot and self.snap["day"] < day and (day - self.snap["day"]) < SLOTS:
            self.pending_rec = dict(self.snap)
            self.pending = True

        self.start_day(now_ms, day, liters, ts_ms)

        if self.pending:
            self.write_pending(now_ms)

    def start_day(self, now_ms, day, liters, ts_ms):
        self.acc = make_record(day, 0, 0, 0, value=0.0)
        self.acc["n"] = 0
        self.acc["sum_l"] = 0.0
        self.day_started = True
        self.last_count_ms = now_ms - INTERVAL_MS
        self.last_snap_ms = now_ms
        self.last_snap_n = 0
        if (ts_ms // 1000) > day_start(day) + CFG["HIST_GAP_SECONDS"]:
            self.acc["flags"] |= C["FLAG_LATE_START"]
        self.accumulate(now_ms, liters, ts_ms)

    def accumulate(self, now_ms, liters, ts_ms):
        ts = ts_ms // 1000
        if self.acc["n"] > 0:
            if (now_ms - self.last_count_ms) > 2 * INTERVAL_MS and self.acc["gaps"] < 0xFFFF:
                self.acc["gaps"] += 1
            if liters < self.acc["min_l"]:
                self.acc["min_l"] = liters
                self.acc["ts_min"] = ts
            if liters > self.acc["max_l"]:
                self.acc["max_l"] = liters
                self.acc["ts_max"] = ts
        else:
            self.acc["min_l"] = liters
            self.acc["max_l"] = liters
            self.acc["ts_min"] = ts
            self.acc["ts_max"] = ts
            self.acc["first_ts"] = ts
        self.acc["sum_l"] += float(liters)
        self.acc["n"] += 1
        self.acc["last_ts"] = ts
        self.last_count_ms = now_ms

    def close_day(self):
        if not self.day_started:
            return
        self.day_started = False
        if self.acc["n"] == 0:
            self.acc = None
            return
        day_end = day_start(self.acc["day"]) + C["DAY_SECONDS"]
        if self.acc["last_ts"] + CFG["HIST_GAP_SECONDS"] < day_end:
            self.acc["flags"] |= C["FLAG_EARLY_END"]
        if self.pending:
            self.degraded = True
        self.pending_rec = self.acc
        self.pending = True
        self.acc = None

    def maybe_snapshot(self, now_ms):
        if not self.day_started or self.acc["n"] == 0:
            return
        if self.acc["n"] == self.last_snap_n:
            return
        if (now_ms - self.last_snap_ms) < CFG["HIST_SNAPSHOT_INTERVAL_MS"]:
            return
        self.last_snap_ms = now_ms
        if self.write_snapshot():
            self.last_snap_n = self.acc["n"]

    # -- gravacao -----------------------------------------------------------
    def space_ok(self):
        return self.fs.free >= MIN_FREE

    def write_snapshot(self):
        if not self.space_ok():
            return False
        target = 1 - self.active_snap if self.have_snapshot else 0
        gen = self.snap_gen + 1
        record = dict(self.acc)
        record["gen"] = gen
        data = encode_record(record)
        if not self.fs.write(self.SNAPSHOT[target], data):
            self.write_failures += 1
            return False
        read_back = self.fs.read(self.SNAPSHOT[target]) or b""
        state, parsed = decode_record(read_back)
        if state != "VALID" or parsed["gen"] != gen or parsed["day"] != record["day"]:
            self.fs.remove(self.SNAPSHOT[target])
            self.write_failures += 1
            return False
        self.active_snap = target
        self.have_snapshot = True
        self.snap_gen = gen
        return True

    def write_pending(self, now_ms):
        self.last_attempt_ms = now_ms
        if not self.pending or not self.space_ok():
            return False

        slot = self.pending_rec["day"] % SLOTS
        target = (1 - self.active_table) if self.have_table else 0
        gen = self.table_gen + 1
        source = self.scan_table(self.active_table, slot) if self.have_table else Scan()
        source_data = self.fs.read(self.TABLE[self.active_table]) if self.have_table else None
        if self.have_table and source_data is None:
            self.write_failures += 1
            return False

        header = {
            "flags": source.header["flags"] | (C["HEADER_DEGRADED"] if self.degraded else 0),
            "gen": gen,
            "first_day": self.pending_rec["day"],
            "last_day": self.pending_rec["day"],
            "count": source.valid + 1,
        }
        if source.valid > 0:
            header["first_day"] = min(header["first_day"], source.first_day)
            header["last_day"] = max(header["last_day"], source.last_day)

        out = bytearray(encode_header(header))
        for index in range(SLOTS):
            if index == slot:
                record = dict(self.pending_rec)
                record["gen"] = gen
                out += encode_record(record)
            elif source_data is None:
                out += bytes(RECORD_SIZE)
            else:
                offset = HEADER_SIZE + index * RECORD_SIZE
                out += source_data[offset:offset + RECORD_SIZE]
        out += encode_footer(header["count"])

        if not self.fs.write(self.TABLE[target], out):
            self.write_failures += 1
            return False

        verify = self.scan_table(target)
        if not verify.usable() or verify.header["gen"] != gen or verify.valid != header["count"]:
            self.fs.remove(self.TABLE[target])
            self.write_failures += 1
            return False

        self.active_table = target
        self.have_table = True
        self.table_gen = gen
        self.pending = False
        return True

    # -- consultas ----------------------------------------------------------
    def read_day(self, day):
        if self.day_started and self.acc["day"] == day:
            return dict(self.acc)
        if self.pending and self.pending_rec["day"] == day:
            return dict(self.pending_rec)
        if not self.have_table:
            return None
        data = self.fs.read(self.TABLE[self.active_table])
        if data is None or len(data) != TABLE_SIZE:
            return None
        slot = day % SLOTS
        offset = HEADER_SIZE + slot * RECORD_SIZE
        state, record = decode_slot(data[offset:offset + RECORD_SIZE], slot)
        if state != "VALID" or record["day"] != day or record["n"] == 0:
            return None
        return record

    def fill_day(self, record, now_epoch):
        report = {
            "day": record["day"],
            "has_data": record["n"] > 0,
            "samples": record["n"],
            "gaps": record["gaps"],
            "flags": record["flags"],
            "sum": record["sum_l"],
            "min": record["min_l"],
            "max": record["max_l"],
            "ts_min": record["ts_min"],
            "ts_max": record["ts_max"],
        }
        report["avg"] = record["sum_l"] / record["n"] if record["n"] else 0.0
        report["span"] = (
            record["last_ts"] - record["first_ts"] + INTERVAL_S
            if record["last_ts"] >= record["first_ts"] else 0
        )

        reference = C["DAY_SECONDS"]
        if now_epoch and day_index(now_epoch) == record["day"]:
            elapsed = now_epoch - day_start(record["day"])
            reference = elapsed if elapsed > 0 else 1

        expected = report["span"] / INTERVAL_S if report["span"] > 0 else 0
        report["sample_ratio"] = (
            min(1.0, record["n"] / expected) if expected > 0 else 0.0
        )
        report["time_ratio"] = min(1.0, report["span"] / reference)
        report["coverage"] = min(
            1.0, record["n"] / C["NOMINAL_SAMPLES_PER_DAY"]
        )

        if not report["has_data"]:
            report["status"] = "NO_DATA"
            return report

        blocking = C["FLAG_RESTARTED"] | C["FLAG_LATE_START"] | C["FLAG_EARLY_END"]
        complete = (
            record["gaps"] == 0
            and (record["flags"] & blocking) == 0
            and report["sample_ratio"] >= COMPLETE_RATIO
            and report["time_ratio"] >= COMPLETE_RATIO
        )
        report["status"] = "COMPLETE" if complete else "PARTIAL"
        return report

    def day_report(self, day, now_epoch):
        record = self.read_day(day)
        if record is None:
            return {"day": day, "has_data": False, "status": "NO_DATA"}
        return self.fill_day(record, now_epoch)

    @staticmethod
    def _no_data_report(days):
        """Relatorio zerado: espelha o `report = HistoryPeriodReport()` que as
        duas APIs C++ devolvem junto com false quando recusam a consulta."""
        return {
            "days": days,
            "days_with_data": 0,
            "samples": 0,
            "span": 0,
            "sum": 0.0,
            "min": 0.0,
            "max": 0.0,
            "status": "NO_DATA",
        }

    def aggregate_days(self, first, last, days, now_epoch):
        """Espelho de History::aggregateDays(): agrega [first, last] inclusive.

        Soma as contribuicoes diarias (nenhum span atravessa a meia-noite), pula
        dias sem dado e derruba o status para PARCIAL quando algum dia nao e
        COMPLETO ou quando falta algum dos `days` pedidos.
        """
        report = {
            "days": days,
            "days_with_data": 0,
            "samples": 0,
            "span": 0,
            "sum": 0.0,
            "min": 0.0,
            "max": 0.0,
        }
        all_complete = True

        for day in range(first, last + 1):
            value = self.day_report(day, now_epoch)
            if not value["has_data"]:
                all_complete = False
                continue
            report["days_with_data"] += 1
            report["samples"] += value["samples"]
            report["span"] += value["span"]
            report["sum"] += value["sum"]
            if report["days_with_data"] == 1 or value["min"] < report["min"]:
                report["min"] = value["min"]
            if report["days_with_data"] == 1 or value["max"] > report["max"]:
                report["max"] = value["max"]
            if value["status"] != "COMPLETE":
                all_complete = False

        if report["samples"] == 0:
            report["status"] = "NO_DATA"
            return report

        report["avg"] = report["sum"] / report["samples"]
        report["coverage"] = min(
            1.0, report["samples"] / (days * C["NOMINAL_SAMPLES_PER_DAY"])
        )
        report["time_ratio"] = min(
            1.0, report["span"] / (days * C["DAY_SECONDS"])
        )
        report["sample_ratio"] = (
            min(1.0, report["samples"] / (report["span"] / INTERVAL_S))
            if report["span"] > 0 else 0.0
        )
        report["status"] = (
            "COMPLETE"
            if all_complete and report["days_with_data"] == days
            else "PARTIAL"
        )
        return report

    def period_report(self, now_epoch, days):
        """Espelho de History::periodReport(): janela que INCLUI hoje."""
        if days == 0 or now_epoch == 0:
            return self._no_data_report(0)
        today = day_index(now_epoch)
        first = today + 1 - days if today + 1 > days else 0
        return self.aggregate_days(first, today, days, now_epoch)

    def closed_period_report(self, now_epoch, days):
        """Espelho de History::closedPeriodReport(): so dias fechados, ate ontem.

        days = 7 -> hoje-7 .. hoje-1; days = 30 -> hoje-30 .. hoje-1.
        """
        if days == 0 or days > SLOTS or now_epoch == 0:
            return self._no_data_report(0)
        today = day_index(now_epoch)
        if today <= 0:
            return self._no_data_report(0)
        last = today - 1
        first = today - days if today > days else 1
        return self.aggregate_days(first, last, days, now_epoch)

    def legacy_period_report(self, now_epoch, days):
        """Copia CONGELADA de period_report() anterior a extracao de
        aggregate_days(). Serve para provar que a API antiga permaneceu igual."""
        report = {
            "days": days,
            "days_with_data": 0,
            "samples": 0,
            "span": 0,
            "sum": 0.0,
            "min": 0.0,
            "max": 0.0,
        }
        today = day_index(now_epoch)
        first = today + 1 - days if today + 1 > days else 0
        all_complete = True

        for day in range(first, today + 1):
            value = self.day_report(day, now_epoch)
            if not value["has_data"]:
                all_complete = False
                continue
            report["days_with_data"] += 1
            report["samples"] += value["samples"]
            report["span"] += value["span"]
            report["sum"] += value["sum"]
            if report["days_with_data"] == 1 or value["min"] < report["min"]:
                report["min"] = value["min"]
            if report["days_with_data"] == 1 or value["max"] > report["max"]:
                report["max"] = value["max"]
            if value["status"] != "COMPLETE":
                all_complete = False

        if report["samples"] == 0:
            report["status"] = "NO_DATA"
            return report

        report["avg"] = report["sum"] / report["samples"]
        report["coverage"] = min(
            1.0, report["samples"] / (days * C["NOMINAL_SAMPLES_PER_DAY"])
        )
        report["time_ratio"] = min(
            1.0, report["span"] / (days * C["DAY_SECONDS"])
        )
        report["sample_ratio"] = (
            min(1.0, report["samples"] / (report["span"] / INTERVAL_S))
            if report["span"] > 0 else 0.0
        )
        report["status"] = (
            "COMPLETE"
            if all_complete and report["days_with_data"] == days
            else "PARTIAL"
        )
        return report


# ---------------------------------------------------------------------------
#  Baterias de teste
# ---------------------------------------------------------------------------
def build_table(records, gen=1, flags=0, count_override=None):
    days = {item["day"] % SLOTS: item for item in records}
    header = {
        "flags": flags,
        "gen": gen,
        "first_day": min(item["day"] for item in records) if records else 0,
        "last_day": max(item["day"] for item in records) if records else 0,
        "count": len(records) if count_override is None else count_override,
    }
    out = bytearray(encode_header(header))
    for slot in range(SLOTS):
        out += encode_record(days[slot]) if slot in days else bytes(RECORD_SIZE)
    out += encode_footer(header["count"])
    return bytes(out)


def complete_days(last_day, count):
    """`count` dias completos consecutivos, terminando em `last_day`."""
    out = []
    for index in range(count):
        day = last_day - count + 1 + index
        start = day_start(day)
        out.append(make_record(day, C["NOMINAL_SAMPLES_PER_DAY"], start,
                               start + C["DAY_SECONDS"] - INTERVAL_S))
    return out


def varying_days(last_day, count, base=100.0):
    """`count` dias completos terminando em `last_day`, com min/max distintos.

    O dia de indice `i` (0 = mais antigo) tem min = base + i, max = base + 100 + i
    e soma = (base + 50 + i) x NOMINAL_SAMPLES_PER_DAY. Assim o extremo do
    periodo identifica sem ambiguidade qual dia o forneceu.
    """
    out = []
    for index in range(count):
        day = last_day - count + 1 + index
        start = day_start(day)
        out.append(make_record(day, C["NOMINAL_SAMPLES_PER_DAY"], start,
                               start + C["DAY_SECONDS"] - INTERVAL_S,
                               value=base + 50.0 + index,
                               minimum=base + index,
                               maximum=base + 100.0 + index))
    return out


def test_format():
    print("\n1. Layout binario")
    check(TABLE_SIZE == 1600, "tabela = cabecalho 28 + 30 x 52 + rodape 12 = 1600 B")
    check(HEADER_SIZE == 28 and FOOTER_SIZE == 12 and RECORD_SIZE == 52, "tamanhos 28/12/52")
    check(HEADER_SIZE + SLOTS * RECORD_SIZE == 1588, "rodape comeca no byte 1588")
    check(C["NOMINAL_SAMPLES_PER_DAY"] == 8640, "8640 amostras nominais por dia")

    print("\n2. Registro (ida e volta)")
    original = make_record(20000, 8640, 1728000000, 1728086390, value=1234.5,
                           minimum=830.5, maximum=2000.25, gaps=2, flags=0x02)
    decoded_state, decoded = decode_record(encode_record(original))
    check(decoded_state == "VALID", "registro valido apos decodificar")
    check(decoded["day"] == original["day"], "dayIndex preservado")
    check(decoded["min_l"] == original["min_l"], "minimo float32 preservado")
    check(decoded["max_l"] == original["max_l"], "maximo float32 preservado")
    check(decoded["sum_l"] == original["sum_l"], "soma float64 preservada")
    check(decoded["n"] == original["n"] and decoded["gaps"] == 2
          and decoded["flags"] == 0x02, "n/gaps/flags preservados")

    print("\n3. Slots vazios x corrompidos x validos")
    check(decode_record(bytes(RECORD_SIZE))[0] == "EMPTY", "52 bytes zero = VAZIO")
    damaged_empty = bytearray(RECORD_SIZE)
    damaged_empty[10] = 1
    check(decode_record(bytes(damaged_empty))[0] == "CORRUPT", "um byte nao zero = CORROMPIDO")
    valid = encode_record(make_record(20000, 10, 1728000000, 1728000090))
    check(decode_slot(valid, 20000 % SLOTS)[0] == "VALID", "registro no slot correto = VALIDO")
    check(decode_slot(valid, (20000 + 1) % SLOTS)[0] == "CORRUPT",
          "registro no slot errado = CORROMPIDO")
    broken = bytearray(valid)
    broken[5] ^= 0xFF
    check(decode_record(bytes(broken))[0] == "CORRUPT", "CRC errado = CORROMPIDO")

    print("\n4. Cabecalho e rodape")
    header = {"flags": C["HEADER_DEGRADED"], "gen": 7, "first_day": 19990,
              "last_day": 20019, "count": 30}
    parsed = decode_header(encode_header(header))
    check(parsed == header, "cabecalho (flags/gen/first/last/count) ida e volta")
    check(decode_footer(encode_footer(30), 30), "rodape aceita o count esperado")
    check(not decode_footer(encode_footer(30), 29), "rodape rejeita count diferente")
    bad_header = bytearray(encode_header(header))
    bad_header[9] ^= 0x01
    check(decode_header(bytes(bad_header)) is None, "cabecalho com CRC errado e rejeitado")

    print("\n5. Dia civil (BRT)")
    last_second = 1717124399  # 2024-05-31 23:59:59 BRT = 2024-06-01 02:59:59 UTC
    first_second = 1717124400  # 2024-06-01 00:00:00 BRT
    check(day_index(first_second) == day_index(last_second) + 1,
          "meia-noite local muda o dayIndex")
    check(day_start(day_index(first_second)) == first_second,
          "dayStart devolve exatamente a meia-noite local")
    check(day_start(day_index(last_second)) == last_second - 86399,
          "dayStart do dia anterior comeca 24 h antes")


def test_ring():
    print("\n6. Anel de 30 dias")
    fs = FakeFS()
    model = HistoryModel(fs)
    model.begin()

    base = 20000
    for record in complete_days(base + 29, 30):
        model.pending_rec = record
        model.pending = True
        if not model.write_pending(0):
            break

    scan = model.scan_table(model.active_table)
    check(scan.valid == 30, "30 dias gravados")
    check(scan.empty == 0 and scan.corrupt == 0, "sem slots vazios nem corrompidos")
    check(scan.header["count"] == 30, "count do cabecalho = 30")
    check(scan.header["first_day"] == base and scan.header["last_day"] == base + 29,
          "firstDay/lastDay = 20000..20029")

    data = fs.read(model.TABLE[model.active_table])
    slots = {}
    for slot in range(SLOTS):
        offset = HEADER_SIZE + slot * RECORD_SIZE
        state, record = decode_slot(data[offset:offset + RECORD_SIZE], slot)
        if state == "VALID":
            slots[slot] = record["day"]

    ordered = sorted(slots.items(), key=lambda item: item[1])
    check(all(b[1] - a[1] == 1 for a, b in zip(ordered, ordered[1:])),
          "ordem logica (do menor dayIndex) e estritamente crescente")
    check(all((b[0] - a[0]) % SLOTS == 1 for a, b in zip(ordered, ordered[1:])),
          "ordem fisica e exatamente uma rotacao do anel")
    physical = [slots[slot] for slot in sorted(slots)]
    check(physical != sorted(physical),
          "ordem fisica dos slots NAO e crescente (validacao antiga falharia)")

    # Dia seguinte: a janela desliza e o mais antigo sai.
    model.pending_rec = complete_days(base + 30, 1)[0]
    model.pending = True
    model.write_pending(0)
    scan = model.scan_table(model.active_table)
    check(scan.valid == 30 and scan.header["count"] == 30, "continua com 30 dias")
    check(scan.header["first_day"] == base + 1 and scan.header["last_day"] == base + 30,
          "janela passou a 20001..20030")
    check(model.read_day(base) is None, "o dia mais antigo saiu da janela")
    check(model.read_day(base + 30) is not None, "o dia novo entrou")

    # Um registro no slot errado e corrompido, nao "vazio".
    table = bytearray(build_table(complete_days(20051, 1)))
    source = HEADER_SIZE + (20051 % SLOTS) * RECORD_SIZE
    destination = HEADER_SIZE + (20050 % SLOTS) * RECORD_SIZE
    table[destination:destination + RECORD_SIZE] = table[source:source + RECORD_SIZE]
    table[source:source + RECORD_SIZE] = bytes(RECORD_SIZE)
    fs.files["/h-days-a.dat"] = bytes(table)
    fs.files.pop("/h-days-b.dat", None)
    scan = model.scan_table(0)
    check(scan.corrupt == 1 and scan.valid == 0, "registro fora do slot = corrompido")

    # count divergente do numero de validos: tabela continua utilizavel.
    table = bytearray(build_table(complete_days(20050, 5), count_override=5))
    header = decode_header(bytes(table[:HEADER_SIZE]))
    header["count"] = 4
    table[0:HEADER_SIZE] = encode_header(header)
    footer_offset = HEADER_SIZE + SLOTS * RECORD_SIZE
    table[footer_offset:footer_offset + FOOTER_SIZE] = encode_footer(4)
    fs.files["/h-days-a.dat"] = bytes(table)
    scan = model.scan_table(0)
    check(scan.usable(), "count divergente nao invalida a tabela")
    check(scan.valid == 5 and scan.header["count"] == 4,
          "leitura encontra 5 validos e reporta count=4")


def test_ab():
    print("\n7. A/B (snapshot e tabela)")
    fs = FakeFS()
    snapshot_a = make_record(20000, 100, 1728000000, 1728000990)
    snapshot_a["gen"] = 1
    snapshot_b = make_record(20000, 200, 1728000000, 1728001990)
    snapshot_b["gen"] = 2
    fs.write("/h-cur-a.dat", encode_record(snapshot_a))
    fs.write("/h-cur-b.dat", encode_record(snapshot_b))

    model = HistoryModel(fs)
    model.begin()
    check(model.have_snapshot and model.snap_gen == 2, "snapshot escolhido: maior geracao (B)")
    check(model.snap["n"] == 200, "snapshot B tem 200 amostras")

    damaged = bytearray(fs.read("/h-cur-b.dat"))
    damaged[0] ^= 0xFF
    fs.files["/h-cur-b.dat"] = bytes(damaged)
    model = HistoryModel(fs)
    model.begin()
    check(model.have_snapshot and model.snap_gen == 1, "B corrompido: usa A (gen 1)")

    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(complete_days(20010, 10), gen=3))
    fs.write("/h-days-b.dat", build_table(complete_days(20012, 12), gen=4))
    model = HistoryModel(fs)
    model.begin()
    check(model.active_table == 1 and model.table_gen == 4, "tabela escolhida: maior geracao (B)")

    damaged = bytearray(fs.read("/h-days-b.dat"))
    damaged[9] ^= 0xFF
    fs.files["/h-days-b.dat"] = bytes(damaged)
    model = HistoryModel(fs)
    model.begin()
    check(model.active_table == 0 and model.table_gen == 3, "B corrompido: usa A (gen 3)")
    check(model.read_day(20009) is not None, "A continua legivel")

    # Nenhuma versao valida: historico comeca vazio, sem apagar nada.
    fs = FakeFS()
    fs.write("/h-days-a.dat", bytes(TABLE_SIZE))
    fs.write("/h-days-b.dat", bytes(10))
    model = HistoryModel(fs)
    model.begin()
    check(not model.have_table, "sem tabela valida o historico inicia vazio")
    check(len(fs.files) == 2, "nada foi apagado")


def test_coverage():
    print("\n8. Cobertura por dia (sem atravessar dias)")
    fs = FakeFS()
    model = HistoryModel(fs)
    day = 20000
    start = day_start(day)
    end = start + C["DAY_SECONDS"]

    full = make_record(day, 8640, start, start + 86390)
    report = model.fill_day(full, end)
    check(report["status"] == "COMPLETE", "dia inteiro = COMPLETO")
    check(abs(report["coverage"] - 1.0) < 1e-6, "cobertura = 100%")
    check(abs(report["time_ratio"] - 1.0) < 1e-6, "span do dia = 86400 s")

    morning = make_record(day, 4320, start, start + 43190)
    report = model.fill_day(morning, end)
    check(report["status"] == "PARTIAL", "meio dia = PARCIAL")
    check(abs(report["coverage"] - 0.5) < 1e-3, "cobertura = 50%")
    check(abs(report["sample_ratio"] - 1.0) < 1e-3,
          "amostragem dentro da janela observada = 100%")

    gapped = make_record(day, 8640, start, start + 86390, gaps=1)
    check(model.fill_day(gapped, end)["status"] == "PARTIAL", "lacuna = PARCIAL")

    restarted = make_record(day, 8640, start, start + 86390,
                            flags=C["FLAG_RESTARTED"])
    check(model.fill_day(restarted, end)["status"] == "PARTIAL", "reinicio = PARCIAL")

    short = make_record(day, 8590, start, start + 86390)
    report = model.fill_day(short, end)
    check(report["sample_ratio"] < COMPLETE_RATIO and report["status"] == "PARTIAL",
          "menos de 99,5% das amostras = PARCIAL")

    empty = make_record(day, 0, 0, 0)
    check(model.fill_day(empty, end)["status"] == "NO_DATA", "sem amostras = SEM DADOS")

    today = make_record(day, 360, start, start + 3590)
    check(model.fill_day(today, start + 3600)["status"] == "COMPLETE",
          "hoje sem interrupcao desde a meia-noite = COMPLETO")

    print("\n9. Periodos (agregacao sem cruzar dias)")
    days = complete_days(20010, 7)
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(days))
    model = HistoryModel(fs)
    model.begin()
    report = model.period_report(day_start(20010) + 86390, 7)
    check(report["status"] == "COMPLETE", "7 dias completos = COMPLETO")
    check(report["days_with_data"] == 7, "7 dias com dados")
    check(abs(report["coverage"] - 1.0) < 1e-6, "cobertura do periodo = 100%")
    check(abs(report["time_ratio"] - 1.0) < 1e-6, "span agregado = 7 x 86400 s")

    partial = complete_days(20010, 7)
    del partial[3]
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(partial))
    model = HistoryModel(fs)
    model.begin()
    report = model.period_report(day_start(20010) + 86390, 7)
    check(report["status"] == "PARTIAL", "um dia faltante = PARCIAL")
    check(report["days_with_data"] == 6, "6 de 7 dias com dados")
    check(abs(report["coverage"] - 6.0 / 7.0) < 1e-6, "cobertura = 6/7")

    fs = FakeFS()
    model = HistoryModel(fs)
    model.begin()
    report = model.period_report(day_start(20010) + 100, 30)
    check(report["status"] == "NO_DATA", "sem registros = SEM DADOS")


def test_model():
    print("\n10. Um dia completo no modulo (8640 amostras)")
    fs = FakeFS()
    model = HistoryModel(fs)
    model.begin()

    day = 20000
    start = day_start(day)
    now = 1000
    for index in range(8640):
        model.update(now, (start + index * INTERVAL_S) * 1000, 1000.0 + (index % 100))
        now += INTERVAL_MS

    check(model.acc["n"] == 8640, "8640 amostras contabilizadas")
    check(model.acc["first_ts"] == start, "primeira amostra na meia-noite")
    check(model.acc["last_ts"] == start + 86390, "ultima amostra 10 s antes do fim")
    check(model.acc["flags"] == 0, "sem flag de borda/reinicio")
    check(model.acc["gaps"] == 0, "sem lacuna")

    expected_sum = 8640 * 1000.0 + sum(index % 100 for index in range(8640))
    check(abs(model.acc["sum_l"] - expected_sum) < 1e-6, "soma exata em float64")
    check(abs(model.acc["sum_l"] / model.acc["n"] - 1049.3611) < 1e-3,
          "media = soma / n")

    snapshots = sum(count for path, count in fs.writes.items()
                    if path.startswith("/h-cur"))
    check(snapshots == 287, "287 snapshots em 24 h (5 min cada), obtidos %d" % snapshots)

    before = model.acc["n"]
    repeated_now = now - INTERVAL_MS  # mesmo instante da ultima amostra contada
    model.update(repeated_now, (start + 86390) * 1000, 1.0)
    model.update(repeated_now, (start + 86390) * 1000, 1.0)
    check(model.acc["n"] == before, "chamadas repetidas de update() nao duplicam")

    model.update(now, (start + C["DAY_SECONDS"]) * 1000, 1200.0)
    check(model.acc["day"] == day + 1, "novo dia iniciado na virada")
    check(model.acc["n"] == 1, "a amostra da virada entra no dia novo")
    check(not model.pending, "dia anterior gravado na virada")

    scan = model.scan_table(model.active_table)
    check(scan.valid == 1 and scan.empty == SLOTS - 1, "1 dia gravado e 29 slots vazios")

    report = model.day_report(day, start + C["DAY_SECONDS"])
    check(report["status"] == "COMPLETE", "dia fechado = COMPLETO")
    check(abs(report["coverage"] - 1.0) < 1e-6, "cobertura do dia fechado = 100%")
    check(abs(report["avg"] - 1049.3611) < 1e-3,
          "media do dia gravada corretamente")

    print("\n11. Reboot no meio do dia (snapshot)")
    model.write_snapshot()
    rebooted = HistoryModel(fs)
    rebooted.begin()
    check(rebooted.have_snapshot and rebooted.snap["day"] == day + 1,
          "snapshot do dia corrente recuperado no boot")
    resumed = rebooted.snap["n"]
    rebooted.update(90000000, (start + C["DAY_SECONDS"] + INTERVAL_S) * 1000, 1300.0)
    check(rebooted.acc["n"] == resumed + 1, "contagem continua apos o reboot")
    check(rebooted.acc["flags"] & C["FLAG_RESTARTED"], "flag de reinicio marcada")

    print("\n12. Falha de gravacao e dia pendente")
    fs2 = FakeFS()
    fs2.writable = False
    model2 = HistoryModel(fs2)
    model2.begin()
    start2 = day_start(30000)

    model2.update(1000, start2 * 1000, 500.0)
    model2.update(2000, (start2 + C["DAY_SECONDS"]) * 1000, 600.0)
    check(model2.pending, "dia fica pendente quando a gravacao falha")
    check(not model2.degraded, "historico ainda nao esta degradado")
    check(model2.acc["day"] == 30001, "dia novo continua acumulando")

    model2.update(3000, (start2 + 2 * C["DAY_SECONDS"]) * 1000, 700.0)
    check(model2.degraded, "segunda virada com pendente marca degradado")
    check(model2.pending_rec["day"] == 30001, "mantem apenas o dia mais recente")

    fs2.writable = True
    check(model2.write_pending(4000), "gravacao funciona quando o FS volta")
    scan = model2.scan_table(model2.active_table)
    check(scan.valid == 1, "apenas um dia foi gravado")
    check(scan.header["flags"] & C["HEADER_DEGRADED"],
          "bit degradado propagado ao cabecalho")

    print("\n13. Espaco livre")
    fs3 = FakeFS(free=MIN_FREE - 1)
    model3 = HistoryModel(fs3)
    model3.begin()
    model3.pending_rec = complete_days(20010, 1)[0]
    model3.pending = True
    check(not model3.write_pending(1000), "sem espaco: nao grava")
    check(model3.pending, "o dia continua pendente")
    check(len(fs3.files) == 0, "nenhum arquivo parcial foi deixado")
    fs3.free = MIN_FREE
    check(model3.write_pending(2000), "com espaco: grava")


def test_closed_period():
    print("\n14. Periodos fechados: janelas de 7 e 30 dias")
    now = day_start(20020) + 3600  # 01:00 BRT do dia 20020
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(varying_days(20019, 30)))
    model = HistoryModel(fs)
    model.begin()

    report = model.closed_period_report(now, 7)
    check(report["days"] == 7, "7 dias: periodo pedido = 7")
    check(report["days_with_data"] == 7, "7 dias: 7 dias com dados")
    check(abs(report["min"] - 123.0) < 1e-6, "7 dias: minimo do dia 20013 (123)")
    check(abs(report["max"] - 229.0) < 1e-6, "7 dias: maximo de ontem (229)")
    check(report["samples"] == 7 * C["NOMINAL_SAMPLES_PER_DAY"],
          "7 dias: amostras somadas dos 7 dias")
    check(report["status"] == "COMPLETE", "7 dias completos = COMPLETO")
    check(abs(report["coverage"] - 1.0) < 1e-6, "7 dias: cobertura = 100%")
    check(abs(report["time_ratio"] - 1.0) < 1e-6, "7 dias: span = 7 x 86400 s")

    report = model.closed_period_report(now, 30)
    check(report["days"] == 30, "30 dias: periodo pedido = 30")
    check(report["days_with_data"] == 30, "30 dias: 30 dias com dados (anel inteiro)")
    check(abs(report["min"] - 100.0) < 1e-6,
          "30 dias: minimo vem do dia mais antigo do anel (hoje-30)")
    check(abs(report["max"] - 229.0) < 1e-6, "30 dias: maximo de ontem")
    check(report["samples"] == 30 * C["NOMINAL_SAMPLES_PER_DAY"],
          "30 dias: amostras dos 30 dias fechados")
    check(report["status"] == "COMPLETE", "30 dias completos = COMPLETO")
    check(abs(report["coverage"] - 1.0) < 1e-6, "30 dias: cobertura = 100%")

    print("\n15. O dia de hoje nunca entra")
    today_start = day_start(20020)
    model.day_started = True
    model.acc = make_record(20020, 360, today_start, today_start + 3590,
                            value=5000.0, minimum=1.0, maximum=9999.0)
    closed = model.closed_period_report(now, 7)
    check(abs(closed["min"] - 123.0) < 1e-6 and abs(closed["max"] - 229.0) < 1e-6,
          "periodo fechado ignora as amostras de hoje (min/max intactos)")
    check(closed["days_with_data"] == 7, "periodo fechado continua com 7 dias")
    check(closed["samples"] == 7 * C["NOMINAL_SAMPLES_PER_DAY"],
          "amostras de hoje nao entram na soma")
    old = model.period_report(now, 7)
    check(abs(old["min"] - 1.0) < 1e-6 and abs(old["max"] - 9999.0) < 1e-6,
          "periodReport (antiga) continua incluindo hoje")
    check(old["days_with_data"] == 7, "periodReport (antiga): 6 fechados + hoje")

    print("\n16. O dia de ontem entra")
    fs = FakeFS()
    start = day_start(20019)
    fs.write("/h-days-a.dat", build_table(
        [make_record(20019, C["NOMINAL_SAMPLES_PER_DAY"], start,
                     start + C["DAY_SECONDS"] - INTERVAL_S, value=777.0)]))
    model = HistoryModel(fs)
    model.begin()
    report = model.closed_period_report(now, 7)
    check(report["days_with_data"] == 1, "somente ontem tem dados")
    check(abs(report["min"] - 777.0) < 1e-6 and abs(report["max"] - 777.0) < 1e-6,
          "ontem fornece o minimo e o maximo")
    check(report["samples"] == C["NOMINAL_SAMPLES_PER_DAY"], "amostras de ontem")
    check(report["status"] == "PARTIAL", "1 de 7 dias = PARCIAL")
    check(abs(report["coverage"] - 1.0 / 7.0) < 1e-6,
          "cobertura relativa aos 7 dias pedidos")

    print("\n17. Virada de meia-noite BRT")
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(varying_days(20019, 30)))
    model = HistoryModel(fs)
    model.begin()
    before = model.closed_period_report(day_start(20020) - 1, 7)
    check(abs(before["min"] - 122.0) < 1e-6, "23:59:59: minimo do dia 20012")
    check(abs(before["max"] - 228.0) < 1e-6, "23:59:59: maximo do dia 20018")
    after = model.closed_period_report(day_start(20020), 7)
    check(abs(after["min"] - 123.0) < 1e-6,
          "00:00:00: a janela anda um dia (minimo do dia 20013)")
    check(abs(after["max"] - 229.0) < 1e-6,
          "00:00:00: o dia 20019 ja entra como fechado")
    check(abs(after["time_ratio"] - 1.0) < 1e-6,
          "dia recem-fechado usa a referencia de 24 h, nao o tempo decorrido")
    check(model.day_report(20019, day_start(20020))["status"] == "COMPLETE",
          "dia 20019 COMPLETO imediatamente apos a virada")

    print("\n18. Dias sem dados no periodo")
    records = [item for item in varying_days(20019, 30)
               if item["day"] not in (20005, 20015)]
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(records))
    model = HistoryModel(fs)
    model.begin()
    report = model.closed_period_report(now, 7)
    check(report["days_with_data"] == 6, "7 dias: 6 dias com dados (20015 faltando)")
    check(report["status"] == "PARTIAL", "dia faltando = PARCIAL")
    check(abs(report["coverage"] - 6.0 / 7.0) < 1e-6, "cobertura = 6/7")
    check(abs(report["min"] - 123.0) < 1e-6,
          "extremos calculados so com os dias presentes")
    report = model.closed_period_report(now, 30)
    check(report["days_with_data"] == 28, "30 dias: 28 dias com dados")
    check(abs(report["coverage"] - 28.0 / 30.0) < 1e-6, "cobertura = 28/30")
    check(report["status"] == "PARTIAL", "30 dias com 2 faltando = PARCIAL")

    print("\n19. Dias parciais e degradados")
    records = [item for item in varying_days(20019, 30) if item["day"] != 20016]
    start = day_start(20016)
    records.append(make_record(20016, 4320, start, start + 43190, value=200.0,
                               minimum=150.0, maximum=250.0))
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(records))
    model = HistoryModel(fs)
    model.begin()
    report = model.closed_period_report(now, 7)
    check(report["days_with_data"] == 7, "meio dia presente conta como dia com dados")
    check(abs(report["max"] - 250.0) < 1e-6, "o dia parcial participa dos extremos")
    check(report["status"] == "PARTIAL", "dia parcial = periodo PARCIAL")
    check(abs(report["coverage"] - 56160.0 / 60480.0) < 1e-6,
          "cobertura = (6 x 8640 + 4320) / (7 x 8640)")

    records = [item for item in varying_days(20019, 30) if item["day"] != 20015]
    start = day_start(20015)
    records.append(make_record(20015, C["NOMINAL_SAMPLES_PER_DAY"], start,
                               start + C["DAY_SECONDS"] - INTERVAL_S,
                               value=150.0, minimum=110.0, maximum=210.0,
                               flags=C["FLAG_RESTARTED"]))
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(records))
    model = HistoryModel(fs)
    model.begin()
    report = model.closed_period_report(now, 7)
    check(report["days_with_data"] == 7 and report["status"] == "PARTIAL",
          "dia com reinicio degrada o periodo")

    records = [item for item in varying_days(20019, 30) if item["day"] != 20014]
    start = day_start(20014)
    records.append(make_record(20014, C["NOMINAL_SAMPLES_PER_DAY"], start,
                               start + C["DAY_SECONDS"] - INTERVAL_S,
                               value=150.0, minimum=110.0, maximum=210.0, gaps=2))
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(records))
    model = HistoryModel(fs)
    model.begin()
    check(model.closed_period_report(now, 7)["status"] == "PARTIAL",
          "dia com lacuna degrada o periodo")

    fs = FakeFS()
    fs.write("/h-days-a.dat",
             build_table(varying_days(20019, 30), flags=C["HEADER_DEGRADED"]))
    model = HistoryModel(fs)
    model.begin()
    check(model.degraded, "cabecalho degradado continua sinalizado por degraded()")
    check(model.closed_period_report(now, 7)["status"] == "COMPLETE",
          "bit do cabecalho nao altera o status do periodo (igual a API antiga)")

    print("\n20. Extremos e media ponderados")
    fs = FakeFS()
    records = []
    for day, samples, value, low, high in (
            (20017, 1000, 15.0, 10.0, 20.0),
            (20018, 3000, 25.0, 5.0, 40.0),
            (20019, 5000, 18.0, 12.0, 30.0)):
        start = day_start(day)
        records.append(make_record(day, samples, start,
                                   start + (samples - 1) * INTERVAL_S,
                                   value=value, minimum=low, maximum=high))
    fs.write("/h-days-a.dat", build_table(records))
    model = HistoryModel(fs)
    model.begin()
    report = model.closed_period_report(now, 7)
    check(report["samples"] == 9000, "amostras somadas = 9000")
    check(abs(report["min"] - 5.0) < 1e-6, "minimo global = 5")
    check(abs(report["max"] - 40.0) < 1e-6, "maximo global = 40")
    check(abs(report["avg"] - 20.0) < 1e-6,
          "media ponderada = 180000 / 9000 = 20 (nao 19,33)")
    check(report["status"] == "PARTIAL", "3 de 7 dias = PARCIAL")

    print("\n21. Historico insuficiente e consultas recusadas")
    fs = FakeFS()
    fs.write("/h-days-a.dat", build_table(varying_days(20019, 3)))
    model = HistoryModel(fs)
    model.begin()
    report = model.closed_period_report(now, 7)
    check(report["days"] == 7, "periodo pedido continua sendo 7")
    check(report["days_with_data"] == 3, "apenas 3 dias existem")
    check(report["status"] == "PARTIAL", "3 de 7 = PARCIAL")
    check(abs(report["coverage"] - 3.0 / 7.0) < 1e-6, "cobertura = 3/7")

    fs = FakeFS()
    model = HistoryModel(fs)
    model.begin()
    check(model.closed_period_report(now, 7)["status"] == "NO_DATA",
          "historico vazio = SEM DADOS")
    check(model.closed_period_report(now, 31)["status"] == "NO_DATA",
          "31 dias (acima do anel) e recusado")
    check(model.closed_period_report(now, 31)["days"] == 0,
          "consulta recusada devolve relatorio zerado")
    check(model.closed_period_report(now, 0)["status"] == "NO_DATA",
          "0 dias e recusado")
    check(model.closed_period_report(0, 7)["status"] == "NO_DATA",
          "sem relogio valido e recusado")

    print("\n22. periodReport() antiga preservada")
    scenarios = (
        ("7 dias completos", varying_days(20019, 30), None),
        ("30 dias completos", varying_days(20019, 30), None),
        ("um dia faltando",
         [item for item in varying_days(20019, 30) if item["day"] != 20015], None),
        ("tabela vazia", [], None),
        ("hoje com dados", varying_days(20019, 30),
         make_record(20020, 360, day_start(20020), day_start(20020) + 3590,
                     value=5000.0, minimum=1.0, maximum=9999.0)),
    )
    for label, records, acc in scenarios:
        fs = FakeFS()
        fs.write("/h-days-a.dat", build_table(records))
        model = HistoryModel(fs)
        model.begin()
        if acc is not None:
            model.day_started = True
            model.acc = acc
        for days in (1, 7, 30):
            legacy = model.legacy_period_report(now, days)
            current = model.period_report(now, days)
            check(legacy == current,
                  "periodReport identica ao codigo congelado: %s (%d dias)"
                  % (label, days))





def main():
    print("Historico diario - verificacao de host")
    test_format()
    test_ring()
    test_ab()
    test_coverage()
    test_model()
    test_closed_period()

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
