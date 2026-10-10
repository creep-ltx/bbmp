# SPDX-License-Identifier: GPL-3.0-or-later
"""BeRoTracker (.brt), on the player's own clean-room replayer (bbmp-berotracker)."""

import array
import math
import struct

import pytest

from bbmp.backends import Unsupported, backend_for, is_supported, pipe


def _chunk(tag: bytes, data: bytes) -> bytes:
    return tag + struct.pack("<I", len(data)) + data


def _str8(t: bytes) -> bytes:
    return bytes([len(t)]) + t


def _aplib_literals(data: bytes) -> bytes:
    """data in the aPLib form, every byte a literal: the first as it is, then a 0 bit before
    each byte, then the end marker (bits 1 1 0 and an offset byte of 0)."""
    out = bytearray(data[:1])
    tag_at, bits = None, 0

    def bit(b: int) -> None:
        nonlocal tag_at, bits
        if bits == 0:
            tag_at, bits = len(out), 8
            out.append(0)
        bits -= 1
        out[tag_at] |= b << bits

    for c in data[1:]:
        bit(0)
        out.append(c)
    for b in (1, 1, 0):
        bit(b)
    out.append(0)
    return struct.pack("<I", len(data)) + bytes(out)


def _envelope(flags: int = 0, nodes: tuple = (), loop: tuple = (0, 0)) -> bytes:
    head = struct.pack("<BHHHHHHbb4Bbb", flags, len(nodes), loop[0], loop[1], 0, 0, 0, 0, 0,
                       0, 0, 0, 0, 0, 1)
    return (head + b"".join(struct.pack("<H", t) for t, _ in nodes)
            + bytes(v for _, v in nodes))


def _song(*, info: dict | None = None, order: bytes = b"\x00\xff", rows: int = 64,
          pattern: bytes | None = None, volume_envelope: bytes | None = None,
          sample_length: int = 32, modu: bytes = b"") -> bytes:
    """One channel plays note 61 (the sample's own rate: 14080 Hz, a 32-sample sine, so
    440 Hz) on row 0 of a 64-row pattern and holds it; 6 ticks of 882 samples a row."""
    i = dict(version=0, flags=0x828D, speed=6, bpm=125, sep=128, gv=256, mix=128, channels=1)
    i.update(info or {})
    head = struct.pack("<HHBBHHHHHH", i["version"], i["flags"], i["speed"], i["bpm"], i["sep"],
                       i["gv"], i["mix"], 0, i["channels"], 0)
    if pattern is None:
        # row 0: note 61, instrument 1; then the empty cell for the other rows
        pattern = bytes([0x03, 61, 1, 0xC3, rows - 1])
    patt = b"\x00" + _str8(b"") + struct.pack("<H", rows)
    packed = _aplib_literals(pattern)
    patt += struct.pack("<I", len(packed)) + packed
    sine = bytes((round(64 * math.sin(2 * math.pi * k / 32)) & 0xFF) for k in range(32))
    sain = (struct.pack("<H", 1) + _str8(b"Sine") + _str8(b"") + b"\x01"
            + struct.pack("<6I", sample_length, 0, 32, 0, 0, 14080)
            + struct.pack("<HHH", 128, 256, 64) + bytes([0, 0, 0, 0, 1, 8, 0]))
    inst = (b"\x01" + _str8(b"Tone") + _str8(b"") + bytes([0, 0, 0])
            + struct.pack("<HH", 128, 0) + b"\x00" + struct.pack("<H", 128) + bytes([0, 0, 0, 0])
            + struct.pack("<H", 0) + b"\x00"
            + b"".join(bytes([n, 1]) for n in range(1, 121))
            + bytes([0, 0x80]) + struct.pack("<I", 0)
            + (volume_envelope or _envelope()) + _envelope() * 4 + bytes([0, 0, 0, 0, 0]))
    body = (_chunk(b"NAME", _str8(b"Tone")) + _chunk(b"INFO", head) + _chunk(b"BPMI", b"\x00")
            + _chunk(b"CHAN", b"\x00" + _str8(b"") + b"\x00" + struct.pack("<HH", 128, 64))
            + _chunk(b"PORD", struct.pack("<H", len(order) - 1) + order)
            + _chunk(b"PATT", patt) + _chunk(b"SAIN", sain)
            + _chunk(b"SADA", struct.pack("<H", 1) + sine) + _chunk(b"INST", inst)
            + (_chunk(b"MODU", modu) if modu else b"") + _chunk(b"DONE", b""))
    return b"BRTF" + struct.pack("<I", len(body)) + body


def _need():
    if not pipe.available("berotracker"):
        pytest.skip("bbmp-berotracker not built")


def test_berotracker_is_played_by_its_helper():
    assert backend_for("BeRoTracker") == "berotracker"
    assert is_supported("BeRoTracker")


def test_berotracker_plays_a_note_for_the_length_of_its_pattern(tmp_path):
    _need()
    np = pytest.importorskip("numpy")
    song = tmp_path / "tone.brt"
    song.write_bytes(_song())
    d = pipe.PipeDecoder("berotracker", song)
    try:
        assert d.info.title == "Tone"
        # 64 rows of 6 ticks of 882 samples, and the song ends with its order list
        assert d.info.duration == pytest.approx(338688 / 44100, abs=0.002)
        a = array.array("h")
        while chunk := d.read(65536):
            a.frombytes(chunk)
        assert len(a) // 2 == pytest.approx(338688 * 48000 / 44100, abs=2)
        left, right = np.array(a[0::2], float), np.array(a[1::2], float)
        assert np.abs(left).max() > 1000
        assert np.abs(left - right).max() < 0.02 * np.abs(left).max()   # panning in the middle
        # 440 Hz: the waveform repeats every 48000 / 440 = 109.1 samples
        x = left[20000:60000] - left[20000:60000].mean()
        corr = [np.dot(x[:-lag], x[lag:]) / np.dot(x[:-lag], x[:-lag]) for lag in range(60, 160)]
        assert 60 + int(np.argmax(corr)) in (108, 109, 110)
    finally:
        d.close()


def test_berotracker_refuses_damaged_files(tmp_path):
    _need()
    good = _song()
    # master (0) -> reverb (1) -> output (2); the reverb's wet level is not a number
    rev = struct.pack("<ff", float("nan"), 1.0) + struct.pack("<iii", 20, 126, 175) \
        + struct.pack("<ff", 0.97, 0.32) + struct.pack("<i", 16)
    blocks = [b"\x03" + bytes(16) + struct.pack("<iii", 1, 0, 0),
              b"\x09" + bytes(16) + struct.pack("<iii", 1, 1, 0) + struct.pack("<i", 0)
              + b"REVE" + struct.pack("<i", 0) + b"\x00" + struct.pack("<i", 9 + len(rev))
              + b"\x01" + struct.pack("<i", len(rev)) + rev + struct.pack("<f", 0) + b"\x00",
              b"\x02" + bytes(16) + struct.pack("<iii", 1, 1, 0) + struct.pack("<i", 1)]
    modu = b"".join(struct.pack("<i", len(b)) + b for b in blocks)
    cases = {
        "not a song": b"XXXX" + good[4:],
        "cut short": good[: len(good) // 2],
        "no channels": _song(info={"channels": 0}),
        "order list too long": _song(order=bytes(300)),
        "packed data from outside": _song(pattern=bytes([0x03])).replace(
            _aplib_literals(bytes([0x03])), struct.pack("<I", 8) + bytes([0x03, 0b11000000, 0x0A])),
        "impossible setting": _song(modu=modu),
        "envelope outside its points": _song(volume_envelope=_envelope(3, ((0, 64), (10, 32)), (0, 5))),
        "sample too long": _song(sample_length=1 << 27),
        "never ends": _song(info={"speed": 255, "bpm": 1}),
    }
    for name, data in cases.items():
        bad = tmp_path / f"{name.replace(' ', '_')}.brt"
        bad.write_bytes(data)
        with pytest.raises(Unsupported):
            pipe.PipeDecoder("berotracker", bad)
