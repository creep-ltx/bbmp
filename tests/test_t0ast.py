# SPDX-License-Identifier: GPL-3.0-or-later
"""T0AST, "The 0ok Amazing Synth Tracker" (.t0ast), on the player's own clean-room
synthesizer (bbmp-t0ast)."""

import array
import struct

import pytest

from bbmp.backends import Unsupported, backend_for, is_supported, pipe


def _f32(*values: float) -> bytes:
    return struct.pack(f"<{len(values)}f", *values)


def _text(t: bytes) -> bytes:
    return struct.pack("<I", len(t)) + t


def _song(*, order: bytes = b"\x00", last_row_effect: bytes = b"", channels: int = 16,
          old: bool = False) -> bytes:
    """One pattern: channel 0 plays A-4 (synth note 57, 440 Hz) at full volume on row 0
    and holds it; every other channel is empty. The tracker's defaults otherwise."""
    head = b"T0AST0OK" if old else b"T0AST\x01\x00\x01"
    ins = (bytes(16) + b"\xff" * 16 + _f32(*[0] * 16) + _f32(*[1] * 16) * 4
           + b"\x80" * 16 + bytes(192))
    env = (b"" if old else _f32(*[0.01] * 16) + _f32(*[0] * 16) + _f32(*[1] * 16)
           + b"\x01" * 16 + _f32(*[-0.01] * 16))
    chans = bytes([channels]) + b"\x01\x00\x00\x01" * channels
    names = b"\x01" + bytes([4]) + b"Sine".ljust(24, b"\x00") + b"\x00" * (channels - 1)
    pattern = b"\x01"
    for ch in range(channels):
        if ch == 0:
            # row 0: note and volume; rows 1-62 empty; row 63 empty or an effect
            pattern += b"\x03\x3a\x40" + b"\x80" + b"\x40\x3e"
            pattern += (b"\x0c" + last_row_effect) if last_row_effect else b"\x80"
        else:
            pattern += b"\x40\x40"           # an empty cell, 64 times
    body = (b"\x04\x10\x00\x00" + chans + names + struct.pack("<H", len(order)) + order
            + pattern + bytes(255) + b"\x00"
            + _text(b"Tone") + _text(b"Nobody") + _text(b"A test"))
    return head + ins + env + body


def _need():
    if not pipe.available("t0ast"):
        pytest.skip("bbmp-t0ast not built")


def test_t0ast_is_played_by_its_helper():
    assert backend_for("The 0ok Amazing Synth Tracker") == "t0ast"
    assert is_supported("The 0ok Amazing Synth Tracker")


def test_t0ast_plays_a_note_for_the_length_of_its_pattern(tmp_path):
    _need()
    np = pytest.importorskip("numpy")
    song = tmp_path / "tone.t0ast"
    song.write_bytes(_song())
    d = pipe.PipeDecoder("t0ast", song)
    try:
        assert d.info.title == "Tone"
        assert d.info.author == "Nobody"
        # 64 rows of 6 ticks of 882 samples, then to the next block of 2048 frames
        assert d.info.duration == pytest.approx(344064 / 44100, abs=0.002)
        a = array.array("h")
        while chunk := d.read(65536):
            a.frombytes(chunk)
        assert len(a) // 2 == pytest.approx(344064 * 48000 / 44100, abs=2)
        left, right = np.array(a[0::2], float), np.array(a[1::2], float)
        assert np.abs(left).max() > 1000
        assert np.abs(left - right).max() < 0.02 * np.abs(left).max()   # panning in the middle
        # 440 Hz: the waveform repeats every 48000 / 440 = 109.1 samples
        x = left[20000:60000] - left[20000:60000].mean()
        corr = [np.dot(x[:-lag], x[lag:]) / np.dot(x[:-lag], x[:-lag]) for lag in range(60, 160)]
        assert 60 + int(np.argmax(corr)) in (108, 109, 110)
    finally:
        d.close()


def test_t0ast_plays_the_first_version_of_the_format(tmp_path):
    _need()
    song = tmp_path / "old.t0ast"
    song.write_bytes(_song(old=True))
    d = pipe.PipeDecoder("t0ast", song)
    try:
        assert d.info.duration == pytest.approx(344064 / 44100, abs=0.002)
    finally:
        d.close()


def test_t0ast_refuses_damaged_files(tmp_path):
    _need()
    good = _song()
    at = len(good) - len(_text(b"Tone") + _text(b"Nobody") + _text(b"A test")) - 256
    nan = bytearray(good)
    nan[8 + 96:8 + 100] = struct.pack("<f", float("nan"))    # channel 0's output level
    cases = {
        "not a song": b"T0ASTXYZ" + good[8:],
        "cut short": good[:900],
        "patterns cut short": good[:at - 40],
        "too many channels": good.replace(b"\x04\x10\x00\x00\x10", b"\x04\x10\x00\x00\x11", 1),
        "order list too long": _song(order=bytes(300)),
        "copies from outside": _song(last_row_effect=b"").replace(b"\x40\x3e\x80", b"\x40\x3e\x81\x50", 1),
        "impossible setting": bytes(nan),
        "text past the end": good[:-len(_text(b"A test"))] + struct.pack("<I", 1000) + b"A test",
        "never ends": _song(last_row_effect=bytes([2, 0])),   # jump to order 0 for ever
    }
    for name, data in cases.items():
        bad = tmp_path / f"{name.replace(' ', '_')}.t0ast"
        bad.write_bytes(data)
        with pytest.raises(Unsupported):
            pipe.PipeDecoder("t0ast", bad)
