# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-synth kdm: Ken's Digital Music, on a song and an instrument bank made here
(Ken's own waves.kwv is not needed: the bank's format is simple)."""

import array
import struct

import pytest

from bbmp.backends import Unsupported, pipe

SQUARE = bytes([0x60] * 16 + [0xA0] * 16)        # one period of 32 samples


def _bank(path, loop_start=0, loop_length=len(SQUARE)):
    """waves.kwv with one wave: one period of a square wave, looped."""
    head = struct.pack("<ii", 0, 1)
    wave = b"Square".ljust(16, b"\0") + struct.pack("<iiii", len(SQUARE), loop_start, loop_length, 0)
    path.write_bytes(head + wave + SQUARE)


def _song(path, notes, tracks=1):
    """notes: (time, track, pitch, left, right); no effects, with the panning table."""
    m = len(notes)
    data = struct.pack("<iii", 0, m, tracks) + bytes(tracks) + bytes([0] * tracks) + b"\x80" * tracks * 2
    data += struct.pack(f"<{m}i", *(n[0] for n in notes))
    for k in range(1, 5):
        data += bytes(n[k] for n in notes)
    data += bytes(m * 3)
    path.write_bytes(data)
    return path


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


def _needs_helper():
    if not pipe.available("kdm"):
        pytest.skip("bbmp-synth with kdm not built")


def test_a_note_plays_at_its_pitch_until_its_note_off(tmp_path):
    _needs_helper()
    _bank(tmp_path / "waves.kwv")
    # note 49 from tick 480, off at 540 (half a second), the song ends at 600
    song = _song(tmp_path / "test.kdm", [(480, 0, 49, 100, 100), (540, 0, 49, 0, 0), (600, 0, 49, 0, 0)])
    d = pipe.PipeDecoder("kdm", song)
    try:
        assert d.info.duration == pytest.approx(1.0)
        a = _pcm(d, 60000)
        assert len(a) == 48000 * 2                       # 120 ticks of 400 frames
        left = a[0::2]
        sounding = [i for i, x in enumerate(left) if x]
        assert sounding[0] <= 1                          # the crossfade in from silence
        # 60 ticks, give or take the 23-frame crossfade at the note off
        assert sounding[-1] + 1 == pytest.approx(24000, abs=23)
        tone = left[: 24000 - 23]
        crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < 0) != (y < 0))
        hz = crossings / 2 / (len(tone) / 48000)
        # note 49 steps 2805/4096 samples a frame; the wave is 32 samples long
        assert hz == pytest.approx(48000 * 2805 / 4096 / 32, rel=0.01)
        assert max(left) == 32 * 100                     # (0xA0 - 128) * volume
    finally:
        d.close()


def test_damaged_songs_and_banks_are_refused(tmp_path):
    _needs_helper()
    _bank(tmp_path / "waves.kwv")
    good = _song(tmp_path / "test.kdm", [(480, 0, 49, 100, 100), (600, 0, 49, 0, 0)])
    data = good.read_bytes()
    cut = tmp_path / "cut.kdm"
    cut.write_bytes(data[:-5])
    with pytest.raises(Unsupported, match="not a KDM song"):
        pipe.PipeDecoder("kdm", cut)
    bad_track = tmp_path / "track.kdm"
    bad_track.write_bytes(data[:12 + 4 + 8] + b"\x07" + data[12 + 4 + 8 + 1:])   # track 7 of 1
    with pytest.raises(Unsupported, match="can't be loaded"):
        pipe.PipeDecoder("kdm", bad_track)
    _bank(tmp_path / "waves.kwv", loop_start=20, loop_length=20)                  # loop past the end
    with pytest.raises(Unsupported, match="damaged"):
        pipe.PipeDecoder("kdm", good)
    (tmp_path / "waves.kwv").unlink()
    with pytest.raises(Unsupported, match="not next to the song"):
        pipe.PipeDecoder("kdm", good)
