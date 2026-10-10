# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-synth jxs: JayTrax, on a tiny song made here."""

import array
import json
import math
import struct
import subprocess

import pytest

from bbmp.backends import Unsupported, pipe

SUB, INS = 16564, 8712


def _song(path, *, version=3457, S=1, channels=1, bpm=120, end=(1, 0), start=(0, 0), echo=1000,
          wavelen=256, note=105, script=(0, 0, 0), sample_len=None, cut=0):
    """One channel, one pattern of 64 rows: note 105 (440 Hz on a wave of 256) struck on
    row 0 with instrument 1, a sine at full volume; the track ends at bar 1 step 0."""
    head = struct.pack("<hhiii", version, 0, 1, S, 1) + bytes(36)
    sub = bytearray(SUB)
    struct.pack_into("<iiiiiiiih", sub, 80, bpm, 0, start[0], start[1], end[0], end[1], 0, 0, 0)
    sub[114:118] = b"Test"
    struct.pack_into("<hH", sub, 146, channels, echo)
    struct.pack_into("<h", sub, 166, 100)
    struct.pack_into("<hh", sub, 180, 0, 64)                 # channel 0: pattern 0, 64 rows
    pattern = bytearray(320)
    d, p, s = script
    pattern[0:5] = bytes([note, d, 1, p & 255, s])
    names = struct.pack("<i", 1) + b"\0"
    ins = bytearray(INS)
    ins[2:6] = b"Sine"
    struct.pack_into("<hhh", ins, 34, 0, wavelen, 255)       # wave 0, its length, volume
    ins[58] = 1                                               # wave 0 is copied in on a strike
    for i in range(256):
        struct.pack_into("<h", ins, 520 + 2 * i, round(16000 * math.sin(2 * math.pi * i / 256)))
    tail = b""
    if sample_len is not None:
        struct.pack_into("<ii", ins, 512, 1, sample_len)
        tail = bytes(max(0, min(sample_len, 64)))
    data = head + bytes(sub) * S + bytes(pattern) + names + bytes(ins) + tail + bytes(256)
    path.write_bytes(data[: len(data) - cut])
    return path


def _needs_helper():
    cmd = pipe.helper("jxs")
    if cmd is None:
        pytest.skip("bbmp-synth not built")
    p = subprocess.run(cmd, capture_output=True, text=True)
    if " jxs" not in p.stderr.split("engines:")[-1].split("\n")[0] + " ":
        pytest.skip("bbmp-synth without the jxs engine")


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


def test_a_note_plays_at_its_pitch_for_its_length(tmp_path):
    _needs_helper()
    d = pipe.PipeDecoder("jxs", _song(tmp_path / "test.jxs"))
    try:
        # 120 bpm: ticks of 689 frames; one silent tick, then 64 rows of 8 ticks
        assert d.info.duration == pytest.approx(513 * 689 / 44100, abs=0.001)
        assert d.info.title == "Test"
        a = _pcm(d, 500000)
        assert len(a) // 2 == pytest.approx(513 * 689 * 48000 / 44100, abs=2)
        left = a[0::2]      # (the right side is the same but for the fades at each tick)
        sounding = [i for i, x in enumerate(left) if abs(x) > 100]
        assert sounding[0] == pytest.approx(689 * 48000 / 44100, abs=40)    # after the first tick
        assert sounding[-1] > len(left) - 100                                # to the end
        tone = left[2000:]
        crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < 0) != (y < 0))
        # the step is a whole number: 653/256 of the wave per frame, 439.4 Hz
        assert crossings / 2 / (len(tone) / 48000) == pytest.approx(44100 * 653 / 65536, rel=0.005)
        # 16000 x 255/256 (volume), halved by the echo stage
        assert max(left) == pytest.approx(16000 * 255 / 256 / 2, rel=0.01)
    finally:
        d.close()


@pytest.mark.parametrize("make, why", [
    (lambda p: _song(p, version=3458), "not a JayTrax song"),
    (lambda p: _song(p, cut=1), "cut short"),
    (lambda p: _song(p, sample_len=1 << 30), "cut short"),
    (lambda p: _song(p, sample_len=-2), "cut short"),
    (lambda p: _song(p, S=0), "header of the song makes no sense"),
    (lambda p: _song(p, channels=0), "no channels"),
    (lambda p: _song(p, channels=17), "no channels"),
    (lambda p: _song(p, echo=0), "damaged"),
    (lambda p: _song(p, wavelen=0), "damaged"),
    (lambda p: _song(p, wavelen=257), "damaged"),
    (lambda p: _song(p, note=128), "damaged"),
    (lambda p: _song(p, script=(200, 0, 1)), "damaged"),         # a bend past the table
    (lambda p: _song(p, end=(0, 0)), "empty"),
])
def test_damaged_songs_are_refused(tmp_path, make, why):
    _needs_helper()
    song = make(tmp_path / "bad.jxs")
    with pytest.raises(Unsupported, match=why):
        pipe.PipeDecoder("jxs", song)


def test_a_track_that_is_not_there_is_refused(tmp_path):
    _needs_helper()
    song = _song(tmp_path / "test.jxs")
    p = subprocess.run([*pipe.helper("jxs"), str(song), "1", "48000"], capture_output=True, timeout=60)
    assert json.loads(p.stdout.split(b"\n")[0]) == {"tracks": 0, "error": "the song has one track"}
