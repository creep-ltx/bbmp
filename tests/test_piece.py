# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-piece: the P/ECE music driver's songs, on a song made here."""

import array
import struct
import time

import pytest

from bbmp.backends import pipe


def _song(tmp_path):
    """One part: tempo 120, instrument 0, full volume, A-4 for 48 ticks, a rest of 48."""
    head = b"\x00\x06" + struct.pack("<H", 0x15) + b"\0\0" * 5 + struct.pack("<HHH", 0, 0, 0)
    part = bytes([0xE7, 0x78, 0xE8, 0x00, 0xE9, 0x7F, 0x30, 0xB9, 0x30, 0xE0, 0x00])
    p = tmp_path / "a4.pmd"
    p.write_bytes(head + b"\0" + part)
    return p


def test_piece_plays_a_note_at_its_pitch_for_its_length(tmp_path):
    if not pipe.available("piece"):
        pytest.skip("bbmp-piece not built")
    d = pipe.PipeDecoder("piece", _song(tmp_path))
    try:
        out = b""
        while len(out) < 2 * 48000 * 4:
            got = d.read(48000)
            if not got:
                break
            out += got
        a = array.array("h")
        a.frombytes(out[: len(out) // 2 * 2])
        left = a[0::2]
        sounding = [i for i, x in enumerate(left) if abs(x) > 100]
        assert sounding[0] / 48000 < 0.01
        assert sounding[-1] / 48000 == pytest.approx(1.0, abs=0.06)      # 48 ticks at 120
        tone = left[sounding[0]:sounding[0] + 24000]
        crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < 0) != (y < 0))
        assert crossings / 2 / 0.5 == pytest.approx(440, rel=0.02)
    finally:
        d.close()


def test_piece_measures_a_song_that_loops_inside_a_call_quickly(tmp_path):
    # A damaged song: a call whose part loops for ever inside it (so it never counts as
    # started over), through two empty repeats of 255 on every tick. The length used to
    # be measured over 30 minutes at the fastest tempo, 65000 commands a tick: hours.
    if not pipe.available("piece"):
        pytest.skip("bbmp-piece not built")
    head = b"\x00\x01" + struct.pack("<HHHH", 10, 0, 0, 0)
    x = 14
    part = (b"\xe3" + struct.pack("<H", x) + b"\x01"
            + b"\xe4\xff\xe4\xff\xe5\xe5\x01\xb9\xe2" + struct.pack("<H", x))
    p = tmp_path / "loop.pmd"
    p.write_bytes(head + part)
    t0 = time.monotonic()
    d = pipe.PipeDecoder("piece", p)
    try:
        assert time.monotonic() - t0 < 10
        assert d.info.duration is not None and d.info.duration <= 30 * 60
    finally:
        d.close()
