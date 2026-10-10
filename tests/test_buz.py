# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-synth buz: Buzzic 1 and Buzzic 2, on tiny songs made here."""

import array
import struct
import subprocess

import pytest

from bbmp.backends import Unsupported, pipe

B1_SIZE, B1_Q = 44712, 148           # Buzzic 1.0: the size is the only mark


def _b1(path, bpm=120, last=0, size=B1_SIZE):
    """One sine instrument: note 45 (440 Hz) on row 0 only, for one second, at half
    volume, sustained at 0.7 the whole note."""
    d = bytearray(B1_SIZE)
    ins = bytearray(B1_Q)
    ins[:4] = b"Sine"
    ins[129] = 0xFF            # note from sequence 1
    ins[131] = 100             # 1.00 s
    ins[132] = 50              # volume 50 %
    ins[137] = 100             # sustain to the end: 0.7 throughout
    d[:B1_Q] = ins
    base = 128 * B1_Q
    struct.pack_into("<i", d, base, 1)
    d[base + 4] = 45                               # pattern 1, row 0
    d[base + 1156] = 1                             # sequence 1, block 0: pattern 1
    struct.pack_into("<4i", d, base + 17540, 1, 0, 1, 0)
    struct.pack_into("<fiiii", d, base + 17556, 1.0, bpm, 0, last, 0)
    path.write_bytes(bytes(d[:size]) + bytes(max(0, size - B1_SIZE)))
    return path


SINE_PROGRAM = (bytes([1]) + struct.pack("<f", 0.0) + bytes([2, 0, 4])
                + bytes([1]) + struct.pack("<f", 0.5) + bytes([9]))   # sin(freq(note) t) * 0.5


def _b2(path, program=SINE_PROGRAM, plen=None, bpm=120, editor=0, tables=(1, 1, 1)):
    """Note 57 (880 Hz) on row 0 only, eight rows long, at full volume."""
    P, S, Ls = tables
    head = b"buz2" + struct.pack("<HHfiii", 2, 1, 1.0, bpm, 0, 0) + bytes(156 - 24)
    pats = bytearray(16 * max(P, 0))
    if P > 0:
        pats[0] = 57
    seqs = bytearray(max(S, 0) * max(Ls, 0))
    if seqs:
        seqs[0] = 1
    ins = bytearray(9232)
    ins[:4] = b"Sine"
    ins[129] = 0xFF            # note from sequence 1
    ins[130] = 64              # eight rows
    ins[131] = 100             # volume 100 %
    struct.pack_into("<i", ins, 388, editor)
    ins[8076:8076 + len(program)] = program
    struct.pack_into("<i", ins, 9100, len(program) if plen is None else plen)
    data = (head + struct.pack("<iiiii", 1, 1, P, S, Ls) + pats + seqs + bytes(8192)
            + struct.pack("<i", 1) + ins + bytes(552 * max(editor, 0)))
    path.write_bytes(data)
    return path


def _cut(path, n):
    path.write_bytes(path.read_bytes()[:-n])
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
    cmd = pipe.helper("buz")
    if cmd is None:
        pytest.skip("bbmp-synth not built")
    p = subprocess.run(cmd, capture_output=True, text=True)
    if " buz" not in p.stderr.split("engines:")[-1].split("\n")[0] + " ":
        pytest.skip("bbmp-synth without the buz engine")


def _tone(left, end):
    tone = left[:end]
    crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < 0) != (y < 0))
    return crossings / 2 / (len(tone) / 48000)


def test_a_buzzic_1_note_plays_at_its_pitch_for_its_length(tmp_path):
    _needs_helper()
    d = pipe.PipeDecoder("buz", _b1(tmp_path / "test.buz"))
    try:
        # 16 rows of 2646000 / 480 = 5512 frames at 44100 Hz
        assert d.info.duration == pytest.approx(16 * 5512 / 44100, abs=0.001)
        a = _pcm(d, 200000)
        assert len(a) // 2 == pytest.approx(16 * 5512 * 48000 / 44100, abs=2)
        left, right = a[0::2], a[1::2]
        assert left == right                             # 1.0 is mono
        sounding = [i for i, x in enumerate(left) if abs(x) > 50]
        assert sounding[-1] == pytest.approx(48000, abs=30)   # one second
        assert _tone(left, 47000) == pytest.approx(440, rel=0.01)
        # 0.7 (sustain) x 0.5 (volume) x 0.9765625 (the output factor)
        assert max(left) == pytest.approx(0.7 * 0.5 * 0.9765625 * 32767, rel=0.01)
    finally:
        d.close()


def test_a_buzzic_2_program_plays_at_its_pitch_for_its_length(tmp_path):
    _needs_helper()
    d = pipe.PipeDecoder("buz", _b2(tmp_path / "test.buz2"))
    try:
        assert d.info.duration == pytest.approx(16 * 5512 / 44100, abs=0.001)
        a = _pcm(d, 200000)
        assert len(a) // 2 == pytest.approx(16 * 5512 * 48000 / 44100, abs=2)
        left, right = a[0::2], a[1::2]
        assert left == right
        sounding = [i for i, x in enumerate(left) if abs(x) > 50]
        assert sounding[-1] == pytest.approx(8 * 5512 * 48000 / 44100, abs=30)   # eight rows
        assert _tone(left, 47000) == pytest.approx(880, rel=0.01)
        assert max(left) == pytest.approx(0.5 * 32767, rel=0.01)
    finally:
        d.close()


@pytest.mark.parametrize("make, why", [
    (lambda p: _b1(p, size=B1_SIZE - 1), "not a Buzzic song"),
    (lambda p: _b1(p, bpm=5), "not a Buzzic song"),
    (lambda p: _b1(p, last=128), "not a Buzzic song"),
    (lambda p: _cut(_b2(p), 100), "cut short"),
    (lambda p: _b2(p, editor=65), "cut short"),
    (lambda p: _b2(p, tables=(0, 1, 1)), "tables of the song make no sense"),
    (lambda p: _b2(p, program=bytes([20])), "can't be run"),                 # no such code
    (lambda p: _b2(p, program=SINE_PROGRAM + bytes([3])), "can't be run"),   # two values left
    (lambda p: _b2(p, program=bytes([9])), "can't be run"),                  # nothing to pop
    (lambda p: _b2(p, program=bytes([0, 0x80])), "can't be run"),
    (lambda p: _b2(p, program=SINE_PROGRAM[:-1]), "can't be run"),          # cut operand
    (lambda p: _b2(p, plen=1025), "can't be run"),
    (lambda p: _b2(p, plen=6), "can't be run"),                              # inside an operand
    # 8 bpm: rows of 82687 frames; 127 echoes of 127 eighths reach too far
    (lambda p: _b2(p, bpm=8, program=SINE_PROGRAM + bytes([16, 50, 127, 127])), "echo"),
    (lambda p: _b2(p, bpm=7), "header of the song makes no sense"),
])
def test_damaged_songs_are_refused(tmp_path, make, why):
    _needs_helper()
    song = make(tmp_path / "bad.buz2")
    with pytest.raises(Unsupported, match=why):
        pipe.PipeDecoder("buz", song)
