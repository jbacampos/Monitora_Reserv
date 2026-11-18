#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Verificador da fila offline (layout binario + regras de recuperacao/replay).

O firmware usa o mecanismo portado do Ecowitt2TB (segmentos com header/footer e
CRC32, sequencias reservadas, replay em ordem). Nao ha compilador C++ nativo
nesta maquina (ver README), entao este script:

  1. LE as constantes reais de ../../include/queue_record.h e falha se o layout
     mudar no C++ sem atualizar o verificador;
  2. reproduz byte a byte o formato e a maquina de estados da fila;
  3. simula a flash em um diretorio temporario, o que permite testar
     persistencia entre "reboots" (reabrir os mesmos arquivos), memoria cheia,
     corrupcao, cauda parcial e ordem de envio.

Uso: python check_queue.py
"""

import os
import re
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HEADER = os.path.join(HERE, "..", "..", "include", "queue_record.h")

FAILURES = []


def check(condition, message):
    if condition:
        print("  [ok]   " + message)
    else:
        print("  [FALHOU] " + message)
        FAILURES.append(message)


def load_constants(path):
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    patterns = {
        "RECORD_SIZE": r"RECORD_SIZE\s*=\s*(\d+)",
        "RECORD_VERSION": r"RECORD_VERSION\s*=\s*(\d+)",
        "RECORD_MAGIC": r"RECORD_MAGIC\s*=\s*0x([0-9A-Fa-f]+)",
        "SEGMENT_MAGIC": r"SEGMENT_MAGIC\s*=\s*0x([0-9A-Fa-f]+)",
        "FOOTER_MAGIC": r"FOOTER_MAGIC\s*=\s*0x([0-9A-Fa-f]+)",
        "SEGMENT_VERSION": r"SEGMENT_VERSION\s*=\s*(\d+)",
        "SEGMENT_HEADER_SIZE": r"SEGMENT_HEADER_SIZE\s*=\s*(\d+)",
        "SEGMENT_FOOTER_SIZE": r"SEGMENT_FOOTER_SIZE\s*=\s*(\d+)",
        "MAX_RECORDS_PER_SEGMENT": r"MAX_RECORDS_PER_SEGMENT\s*=\s*(\d+)",
    }
    values = {}
    for name, pattern in patterns.items():
        match = re.search(pattern, text)
        if not match:
            raise RuntimeError("constante %s nao encontrada em queue_record.h" % name)
        values[name] = int(match.group(1), 16) if "MAGIC" in name else int(match.group(1))
    return values


C = load_constants(HEADER)
RECORD_SIZE = C["RECORD_SIZE"]
RECORD_VERSION = C["RECORD_VERSION"]
RECORD_MAGIC = C["RECORD_MAGIC"]
SEGMENT_MAGIC = C["SEGMENT_MAGIC"]
FOOTER_MAGIC = C["FOOTER_MAGIC"]
SEGMENT_VERSION = C["SEGMENT_VERSION"]
SEGMENT_HEADER_SIZE = C["SEGMENT_HEADER_SIZE"]
SEGMENT_FOOTER_SIZE = C["SEGMENT_FOOTER_SIZE"]
MAX_RECORDS = C["MAX_RECORDS_PER_SEGMENT"]
SEGMENT_MAX_SIZE = SEGMENT_HEADER_SIZE + MAX_RECORDS * RECORD_SIZE + SEGMENT_FOOTER_SIZE

SAFETY_MARGIN = 16 * 1024


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


def encode_record(sequence, timestamp_ms, volume_l):
    body = (
        put16(RECORD_MAGIC)
        + bytes([RECORD_VERSION])
        + put16(RECORD_SIZE)
        + put32(sequence)
        + put64(timestamp_ms)
        + struct.pack("<f", volume_l)
    )
    assert len(body) == 21, len(body)
    return body + put32(crc32(body))


def decode_record(data, expected_sequence):
    if len(data) != RECORD_SIZE:
        return None
    if (
        get16(data) != RECORD_MAGIC
        or data[2] != RECORD_VERSION
        or get16(data, 3) != RECORD_SIZE
        or get32(data, 5) != expected_sequence
        or get32(data, 21) != crc32(data[:21])
    ):
        return None
    return {
        "sequence": expected_sequence,
        "timestamp_ms": get64(data, 9),
        "volume_l": struct.unpack_from("<f", data, 17)[0],
    }


def encode_header(sequence):
    body = (
        put32(SEGMENT_MAGIC)
        + bytes([SEGMENT_VERSION, SEGMENT_HEADER_SIZE])
        + b"\0\0"
        + put32(sequence)
    )
    body = body.ljust(20, b"\0")
    return body + put32(crc32(body))


def decode_header(data, expected_sequence):
    return (
        len(data) == SEGMENT_HEADER_SIZE
        and get32(data) == SEGMENT_MAGIC
        and data[4] == SEGMENT_VERSION
        and data[5] == SEGMENT_HEADER_SIZE
        and get32(data, 8) == expected_sequence
        and get32(data, 20) == crc32(data[:20])
    )


def encode_footer(count):
    body = put32(FOOTER_MAGIC) + put32(count)
    return body + put32(crc32(body))


def decode_footer(data, expected_count):
    return (
        len(data) == SEGMENT_FOOTER_SIZE
        and get32(data) == FOOTER_MAGIC
        and get32(data, 4) == expected_count
        and get32(data, 8) == crc32(data[:8])
    )


class Flash:
    """LittleFS simulado: um arquivo por caminho dentro de um diretorio."""

    def __init__(self, root, capacity=None):
        self.root = root
        self.capacity = capacity

    def path(self, name):
        return os.path.join(self.root, name.lstrip("/"))

    def exists(self, name):
        return os.path.exists(self.path(name))

    def read(self, name):
        with open(self.path(name), "rb") as handle:
            return handle.read()

    def write(self, name, data):
        with open(self.path(name), "wb") as handle:
            handle.write(data)

    def append(self, name, data):
        with open(self.path(name), "ab") as handle:
            handle.write(data)

    def truncate(self, name, size):
        with open(self.path(name), "r+b") as handle:
            handle.truncate(size)

    def remove(self, name):
        os.remove(self.path(name))

    def names(self):
        return sorted(os.listdir(self.root))

    def used(self):
        return sum(os.path.getsize(self.path(name)) for name in self.names())

    def free(self):
        if self.capacity is None:
            return 1 << 30
        return self.capacity - self.used()


class Queue:
    """Modelo fiel da OfflineQueue: mesmas regras de append/recuperacao/replay."""

    MAX_SEGMENTS = 128

    def __init__(self, flash):
        self.flash = flash
        self.segments = []
        self.next_segment = 1
        self.next_record = 1
        self.replay_segment = 0
        self.replay_record = 0
        self.ready = False
        self.stored = 0
        self.rejected = 0

    @staticmethod
    def path(sequence):
        return "/q-%08d.seg" % sequence

    def create(self, sequence):
        if self.flash.free() <= SAFETY_MARGIN + SEGMENT_MAX_SIZE:
            return False
        name = self.path(sequence)
        if self.flash.exists(name):
            return False
        self.flash.write(name, encode_header(sequence))
        return len(self.flash.read(name)) == SEGMENT_HEADER_SIZE

    def close(self, segment):
        if segment["closed"]:
            return True
        end = SEGMENT_HEADER_SIZE + segment["count"] * RECORD_SIZE
        self.flash.truncate(segment["path"], end)
        self.flash.append(segment["path"], encode_footer(segment["count"]))
        data = self.flash.read(segment["path"])
        if len(data) != end + SEGMENT_FOOTER_SIZE:
            return False
        if not decode_footer(data[end:], segment["count"]):
            return False
        segment["closed"] = True
        return True

    def inspect(self, segment, last_record, have_record):
        data = self.flash.read(segment["path"])
        valid = len(data) >= SEGMENT_HEADER_SIZE and decode_header(
            data[:SEGMENT_HEADER_SIZE], segment["sequence"]
        )
        count = 0
        expected = 0
        position = SEGMENT_HEADER_SIZE
        while valid and count < MAX_RECORDS and len(data) - position >= RECORD_SIZE:
            record = data[position : position + RECORD_SIZE]
            stored = get32(record, 5)
            if count == 0:
                expected = stored
                segment["first"] = stored
                if have_record and expected <= last_record:
                    valid = False
                    break
            elif stored != expected:
                valid = False
                break
            if decode_record(record, expected) is None:
                valid = False
                break
            count += 1
            last_record = expected
            have_record = True
            expected += 1
            position += RECORD_SIZE

        tail = len(data) - position
        closed = False
        if valid and tail == SEGMENT_FOOTER_SIZE:
            closed = decode_footer(data[position:], count)
            valid = closed
        elif count == MAX_RECORDS and tail < SEGMENT_FOOTER_SIZE:
            segment["tail"] = True
        elif tail != 0 and count < MAX_RECORDS:
            segment["tail"] = True
        elif tail != 0 or count == MAX_RECORDS:
            valid = False

        segment["count"] = count
        segment["closed"] = closed
        segment["valid"] = valid
        return valid, last_record, have_record

    def recover(self):
        self.segments = []
        sequences = []
        for name in self.flash.names():
            match = re.fullmatch(r"q-(\d{8})\.seg", name)
            if match:
                sequences.append(int(match.group(1)))
        sequences.sort()
        if len(sequences) > self.MAX_SEGMENTS:
            return False
        if sequences and sequences[-1] >= 99999999:
            return False
        self.next_segment = (sequences[-1] + 1) if sequences else 1
        for sequence in sequences:
            self.segments.append(
                {
                    "sequence": sequence,
                    "first": 0,
                    "path": self.path(sequence),
                    "count": 0,
                    "closed": False,
                    "valid": False,
                    "tail": False,
                }
            )

        last_record = 0
        have_record = False
        for index, segment in enumerate(self.segments):
            valid, last_record, have_record = self.inspect(
                segment, last_record, have_record
            )
            if not valid:
                return False
            if segment["tail"] and index + 1 != len(self.segments):
                return False
            if not segment["closed"] and index + 1 != len(self.segments):
                return False
        if have_record and last_record == 0xFFFFFFFF:
            return False
        self.next_record = last_record + 1

        if self.segments and self.segments[-1]["tail"]:
            segment = self.segments[-1]
            end = SEGMENT_HEADER_SIZE + segment["count"] * RECORD_SIZE
            self.flash.truncate(segment["path"], end)
            if os.path.getsize(self.flash.path(segment["path"])) != end:
                return False
            segment["tail"] = False
        if self.segments and not self.segments[-1]["closed"]:
            if not self.close(self.segments[-1]):
                return False

        self.replay_segment = 0
        self.replay_record = 0
        self.ready = True
        return True

    def begin(self):
        self.ready = self.recover()
        return self.ready

    def active(self):
        for segment in self.segments:
            if segment["valid"] and not segment["closed"]:
                return segment
        return None

    def append(self, timestamp_ms, volume_l):
        if not self.ready:
            self.rejected += 1
            return False
        active = self.active()
        if active and active["count"] >= MAX_RECORDS:
            if not self.close(active):
                self.rejected += 1
                return False
            active = None
        if active is None and self.segments:
            last = self.segments[-1]
            if not last["closed"]:
                self.rejected += 1
                return False
            if self.next_record < last["first"] + last["count"]:
                self.rejected += 1
                return False
        if active is None:
            sequence = self.next_segment
            if not self.create(sequence):
                self.rejected += 1
                return False
            self.next_segment += 1
            self.segments.append(
                {
                    "sequence": sequence,
                    "first": self.next_record,
                    "path": self.path(sequence),
                    "count": 0,
                    "closed": False,
                    "valid": True,
                    "tail": False,
                }
            )
            active = self.segments[-1]

        if active["count"] == 0:
            needed = SEGMENT_MAX_SIZE
        else:
            needed = RECORD_SIZE + (
                SEGMENT_FOOTER_SIZE if active["count"] + 1 == MAX_RECORDS else 0
            )
        if self.flash.free() <= SAFETY_MARGIN + needed:
            self.rejected += 1
            return False

        expected_before = SEGMENT_HEADER_SIZE + active["count"] * RECORD_SIZE
        if os.path.getsize(self.flash.path(active["path"])) != expected_before:
            self.rejected += 1
            return False

        self.flash.append(
            active["path"], encode_record(self.next_record, timestamp_ms, volume_l)
        )
        if os.path.getsize(self.flash.path(active["path"])) != (
            expected_before + RECORD_SIZE
        ):
            self.rejected += 1
            return False

        active["count"] += 1
        self.next_record += 1
        self.stored += 1
        if active["count"] == MAX_RECORDS and not self.close(active):
            self.rejected += 1
            return False
        return True

    def peek(self):
        if not self.ready:
            return "ERROR", None
        while self.replay_segment < len(self.segments):
            segment = self.segments[self.replay_segment]
            if not segment["valid"]:
                return "ERROR", None
            if self.replay_record >= segment["count"]:
                if segment["closed"]:
                    if not self.flash.exists(segment["path"]):
                        return "ERROR", None
                    self.flash.remove(segment["path"])
                    self.segments.pop(self.replay_segment)
                    self.replay_record = 0
                    continue
                if not self.close(segment):
                    return "ERROR", None
                continue
            data = self.flash.read(segment["path"])
            offset = SEGMENT_HEADER_SIZE + self.replay_record * RECORD_SIZE
            record = decode_record(
                data[offset : offset + RECORD_SIZE],
                segment["first"] + self.replay_record,
            )
            if record is None:
                return "ERROR", None
            return "RECORD", record
        return "EMPTY", None

    def confirm(self):
        if not self.ready or self.replay_segment >= len(self.segments):
            return False
        if self.replay_record >= self.segments[self.replay_segment]["count"]:
            return False
        self.replay_record += 1
        return True

    def pending(self):
        total = 0
        for index, segment in enumerate(self.segments):
            if index < self.replay_segment:
                continue
            consumed = self.replay_record if index == self.replay_segment else 0
            if segment["count"] > consumed:
                total += segment["count"] - consumed
        return total


BASE_TS = 1763500000123  # epoch ms de referencia


def test_layout():
    print("\n[1] Layout binario lido de queue_record.h")
    check(RECORD_SIZE == 25, "registro de 25 bytes")
    check(RECORD_MAGIC == 0x524D, "magic do registro 0x524D")
    check(
        SEGMENT_HEADER_SIZE == 24 and SEGMENT_FOOTER_SIZE == 12,
        "header de 24 B e footer de 12 B",
    )
    check(MAX_RECORDS == 128, "128 registros por segmento")
    check(SEGMENT_MAX_SIZE == 3236, "segmento completo de 3236 bytes")
    check(SEGMENT_MAX_SIZE <= 4096, "segmento cabe em um bloco LittleFS de 4 KiB")


def test_record():
    print("\n[2] Registro: ida e volta, offsets e campos")
    data = encode_record(7, BASE_TS, 1234.5)
    check(len(data) == RECORD_SIZE, "tamanho do registro")
    check(get16(data) == RECORD_MAGIC and data[2] == RECORD_VERSION, "magic + versao")
    check(get32(data, 5) == 7, "sequencia no offset 5")
    check(get64(data, 9) == BASE_TS, "timestamp no offset 9")
    check(
        abs(struct.unpack_from("<f", data, 17)[0] - 1234.5) < 1e-3,
        "volume no offset 17",
    )
    decoded = decode_record(data, 7)
    check(decoded is not None and decoded["timestamp_ms"] == BASE_TS,
          "timestamp original preservado")
    check(decoded is not None and abs(decoded["volume_l"] - 1234.5) < 1e-3,
          "volume preservado")
    check(
        set(decoded.keys()) == {"sequence", "timestamp_ms", "volume_l"},
        "registro guarda apenas sequencia, timestamp e volume",
    )


def test_integrity():
    print("\n[3] Integridade: CRC32")
    good = encode_record(1, BASE_TS, 830.0)
    detected = 0
    for index in range(21):
        bad = bytearray(good)
        bad[index] ^= 0x40
        if decode_record(bytes(bad), 1) is None:
            detected += 1
    check(detected == 21, "corrupcao detectada nos 21 bytes cobertos pelo CRC")
    bad = bytearray(good)
    bad[21] ^= 0x01
    check(decode_record(bytes(bad), 1) is None, "CRC alterado rejeitado")
    check(decode_record(good, 2) is None, "sequencia esperada diferente rejeitada")
    check(decode_record(good, 1) is not None, "registro intacto aceito")


def test_header_footer():
    print("\n[4] Header e footer do segmento")
    header = encode_header(3)
    check(len(header) == SEGMENT_HEADER_SIZE, "tamanho do header")
    check(decode_header(header, 3), "header valido")
    check(not decode_header(header, 4), "header de outra sequencia rejeitado")
    bad = bytearray(header)
    bad[8] ^= 1
    check(not decode_header(bytes(bad), 3), "header corrompido rejeitado")

    footer = encode_footer(MAX_RECORDS)
    check(decode_footer(footer, MAX_RECORDS), "footer valido")
    check(not decode_footer(footer, MAX_RECORDS - 1), "contagem divergente rejeitada")
    bad = bytearray(footer)
    bad[0] ^= 1
    check(not decode_footer(bytes(bad), MAX_RECORDS), "footer corrompido rejeitado")


def test_segments_and_reboot():
    print("\n[5] Segmentos, reboot e ordem cronologica")
    with tempfile.TemporaryDirectory() as root:
        flash = Flash(root)
        queue = Queue(flash)
        check(queue.begin(), "fila vazia recuperada")

        total = 3 * MAX_RECORDS - 40
        expected = []
        accepted = True
        for index in range(total):
            timestamp = BASE_TS + index * 10000
            volume = 100.0 + index
            expected.append((timestamp, volume))
            accepted = queue.append(timestamp, volume) and accepted
        check(accepted, "%d registros enfileirados" % total)
        check(len(queue.segments) == 3, "3 segmentos criados")

        first = flash.read(queue.path(1))
        check(len(first) == SEGMENT_MAX_SIZE, "segmento cheio no tamanho maximo")
        check(
            decode_footer(first[-SEGMENT_FOOTER_SIZE:], MAX_RECORDS),
            "footer do segmento cheio valido",
        )

        rebooted = Queue(flash)
        check(rebooted.begin(), "fila recuperada apos reboot")
        check(rebooted.pending() == total, "todos os registros pendentes apos reboot")
        check(rebooted.next_record == total + 1, "sequencia continua apos reboot")

        replayed = []
        while True:
            kind, record = rebooted.peek()
            if kind != "RECORD":
                break
            replayed.append((record["timestamp_ms"], record["volume_l"]))
            if not rebooted.confirm():
                break
        check(len(replayed) == total, "todos os registros entregues no replay")
        check(replayed == expected, "ordem cronologica e conteudo identicos")
        check(rebooted.pending() == 0, "fila vazia ao final do replay")
        check(
            not any(name.startswith("q-") for name in flash.names()),
            "segmentos removidos somente apos envio completo",
        )


def test_full_memory():
    print("\n[6] Memoria cheia: recusa sem descartar registros")
    with tempfile.TemporaryDirectory() as root:
        flash = Flash(root, capacity=64 * 1024)
        queue = Queue(flash)
        check(queue.begin(), "fila recuperada com 64 KiB simulados")

        accepted = 0
        while queue.append(BASE_TS + accepted * 1000, 10.0 + accepted):
            accepted += 1
            if accepted > 5000:
                break
        check(accepted > 0, "aceitou registros ate o limite (%d)" % accepted)
        check(queue.rejected > 0, "append recusado quando nao ha espaco seguro")
        before = queue.pending()

        rebooted = Queue(flash)
        check(rebooted.begin(), "fila recuperada apos memoria cheia")
        check(rebooted.pending() == before, "nenhum registro perdido (%d)" % before)
        check(not rebooted.append(BASE_TS, 1.0), "continua recusando sem espaco")
        check(rebooted.pending() == before, "recusa nao descarta registros existentes")

        delivered = 0
        while True:
            kind, _ = rebooted.peek()
            if kind != "RECORD":
                break
            rebooted.confirm()
            delivered += 1
        check(delivered == before, "todos os registros aceitos continuam legiveis")


def test_corruption_quarantine():
    print("\n[7] Corrupcao: quarentena sem apagar arquivos")
    with tempfile.TemporaryDirectory() as root:
        flash = Flash(root)
        queue = Queue(flash)
        queue.begin()
        for index in range(10):
            queue.append(BASE_TS + index, 1.0 + index)

        path = queue.segments[0]["path"]
        data = bytearray(flash.read(path))
        data[SEGMENT_HEADER_SIZE + 3 * RECORD_SIZE + 10] ^= 0xFF
        flash.write(path, bytes(data))

        rebooted = Queue(flash)
        check(not rebooted.begin(), "recuperacao recusa segmento corrompido")
        check(flash.exists(path), "arquivo corrompido preservado")
        check(not rebooted.ready, "fila nao fica pronta para novos appends")
        check(not rebooted.append(BASE_TS, 1.0), "append recusado em quarentena")


def test_partial_tail():
    print("\n[8] Cauda parcial (queda de energia) e continuidade")
    with tempfile.TemporaryDirectory() as root:
        flash = Flash(root)
        queue = Queue(flash)
        queue.begin()
        for index in range(5):
            queue.append(BASE_TS + index * 1000, 500.0 + index)

        path = queue.segments[0]["path"]
        partial = encode_record(6, BASE_TS + 5000, 505.0)[:11]
        flash.write(path, flash.read(path) + partial)

        rebooted = Queue(flash)
        check(rebooted.begin(), "recuperacao tolera cauda parcial no segmento recente")
        check(rebooted.pending() == 5, "5 registros completos preservados")
        check(
            os.path.getsize(flash.path(path))
            == SEGMENT_HEADER_SIZE + 5 * RECORD_SIZE + SEGMENT_FOOTER_SIZE,
            "cauda parcial removida e segmento fechado",
        )
        check(rebooted.next_record == 6, "proxima sequencia continua em 6")
        check(rebooted.append(BASE_TS + 6000, 506.0), "novo append aceito apos recuperar")

        check(
            rebooted.next_segment == 3 and flash.exists(rebooted.path(2)),
            "novo segmento 2 criado apos o anterior fechado",
        )
        queue_again = Queue(flash)
        check(queue_again.begin(), "segundo reboot recupera os dois segmentos")
        check(queue_again.pending() == 6, "6 registros pendentes apos segundo reboot")


def test_replay_retry():
    print("\n[9] Falha de envio: registro permanece e nao duplica")
    with tempfile.TemporaryDirectory() as root:
        flash = Flash(root)
        queue = Queue(flash)
        queue.begin()
        for index in range(4):
            queue.append(BASE_TS + index, 10.0 * index)

        delivered = []
        failures = 0
        guard = 0
        while guard < 20:
            guard += 1
            kind, record = queue.peek()
            if kind != "RECORD":
                break
            if len(delivered) == 2 and failures == 0:
                failures = 1
                _, again = queue.peek()
                check(
                    again is not None and again["sequence"] == record["sequence"],
                    "releitura entrega o mesmo registro pendente",
                )
                continue
            delivered.append(record["sequence"])
            queue.confirm()

        check(failures == 1, "falha de envio simulada uma vez")
        check(delivered == [1, 2, 3, 4], "ordem preservada apos a falha")
        check(len(set(delivered)) == 4, "nenhum registro duplicado")


def main():
    print("Verificacao da fila offline (porte do Ecowitt2TB)")
    test_layout()
    test_record()
    test_integrity()
    test_header_footer()
    test_segments_and_reboot()
    test_full_memory()
    test_corruption_quarantine()
    test_partial_tail()
    test_replay_retry()

    print()
    if FAILURES:
        print("%d FALHA(S):" % len(FAILURES))
        for item in FAILURES:
            print("  - " + item)
        return 1
    print("TODOS OS TESTES PASSARAM (0 falha(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())





