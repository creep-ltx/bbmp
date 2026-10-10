# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-pcmusic: MONOTONE and DigiTrekker, on songs made here."""

import array
import struct

import pytest

from bbmp.backends import pipe


def _pcm(decoder, frames):
    out = b""
    while len(out) < frames * 4:
        got = decoder.read(frames - len(out) // 4)
        if not got:
            break
        out += got
    a = array.array("h")
    a.frombytes(out[: len(out) // 2 * 2])
    return a


def _monotone(tmp_path):
    """One pattern of 4 tracks: A-4 on track 1 for one row, then off; the default
    speed (4 ticks a row, 60 ticks a second)."""
    head = b"\x08MONOTONE" + b"\x04test" + b" " * 36 + b"\x00" + b" " * 40
    head += bytes([1, 1, 4, 2]) + bytes([0]) + b"\xff" * 255
    rows = bytearray(64 * 4 * 2)
    struct.pack_into("<H", rows, 0, 49 << 9)              # A-4 (note 1 is A-0)
    struct.pack_into("<H", rows, 8, 127 << 9)             # row 1: off
    p = tmp_path / "test.mon"
    p.write_bytes(head + bytes(rows))
    return p


def test_monotone_plays_a_note_at_its_pitch_for_its_row(tmp_path):
    if not pipe.available("monotone"):
        pytest.skip("bbmp-pcmusic not built")
    d = pipe.PipeDecoder("monotone", _monotone(tmp_path))
    try:
        assert d.info.duration == pytest.approx((1 + 64 * 4) / 60, abs=0.02)
        a = _pcm(d, 48000)
        left = a[0::2]
        sounding = [i for i, x in enumerate(left) if x]
        tone = left[sounding[0]:sounding[-1] + 1]
        # one row is 4 ticks; as in MONOTONE, the speaker takes the note a tick late
        # here (on the row's first tick it serves track 2, which holds the silence)
        assert len(tone) / 48000 == pytest.approx(3 / 60, abs=0.004)
        crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < 0) != (y < 0))
        hz = crossings / 2 / (len(tone) / 48000)
        assert hz == pytest.approx(1193182 / (1193182 // 440), rel=0.03)
    finally:
        d.close()


def _digitrekker(tmp_path):
    """A DigiTrekker module of 1 channel: a looped square wave at C-4 for 4 rows."""
    def chunk(name, body):
        return name + struct.pack("<I", len(body)) + body
    wave = bytes([0x60] * 16 + [0xA0] * 16) * 8           # 32-sample period, 256 samples
    rows = 64
    trak = bytearray(2 + 6 * rows)
    struct.pack_into("<H", trak, 0, rows)
    trak[2 + 0] = 49                                       # pitch: C-4
    trak[2 + rows + 0] = 1                                 # instrument 1
    trak[2 + 0 + 4] = 0x80                                 # row 4: note off
    samp = b"square".ljust(32, b"\0") + struct.pack("<IIIHBB", len(wave), 0, len(wave), 8192, 64, 8)
    samp += b"SQUARE.NTI".ljust(13, b"\0") + b"\0" * 3
    body = chunk(b"NAME", b"test\0")
    body += chunk(b"INFO", struct.pack("<5H", 1, 1, 1, 2, 1))
    body += chunk(b"INIT", chunk(b"sped", struct.pack("<2H", 6, 125)) + chunk(b"vpan", bytes([64, 64])))
    body += chunk(b"PSEQ", b"\0")
    body += chunk(b"PATT", struct.pack("<H", 1))
    body += chunk(b"INST", chunk(b"samp", samp))
    body += chunk(b"TRAK", chunk(b"trak", struct.pack("<H", rows) + bytes(6 * rows)) + chunk(b"trak", bytes(trak)))
    body += chunk(b"SAMP", chunk(b"samp", wave))
    p = tmp_path / "test.dtm"
    p.write_bytes(chunk(b"SONG", body))
    return p


def test_digitrekker_plays_its_sample_at_the_c4_rate_and_its_tempo(tmp_path):
    if not pipe.available("digitrekker"):
        pytest.skip("bbmp-pcmusic not built")
    d = pipe.PipeDecoder("digitrekker", _digitrekker(tmp_path))
    try:
        # 64 rows of 6 frames at 50 frames a second
        assert d.info.duration == pytest.approx(64 * 6 / 50, abs=0.02)
        a = _pcm(d, 48000)
        left = a[0::2]
        tone = left[: 48000 * 4 * 6 // 50]                # rows 0-3
        crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < 0) != (y < 0))
        hz = crossings / 2 / (len(tone) / 48000)
        assert hz == pytest.approx(8192 / 32, rel=0.03)
        assert max(abs(x) for x in left[48000 * 4 * 6 // 50 + 10:]) == 0
    finally:
        d.close()
