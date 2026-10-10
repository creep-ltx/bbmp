# SPDX-License-Identifier: GPL-3.0-or-later
"""bbmp-retro's vgemm engine: VGM Music Maker songs made here."""

import array
import struct
import subprocess

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


def _rle(data):
    """TFM Music Maker's runs: a byte, 0x80, the run's length (7 bits a byte, the last
    with its top bit set); 0x80 itself is 0x80 0x80."""
    out = bytearray()
    i = 0
    while i < len(data):
        b = data[i]
        j = i
        while j < len(data) and data[j] == b:
            j += 1
        if b != 0x80 and j - i >= 4:
            out += bytes([b, 0x80])
            n = j - i
            while n >= 0x80:
                out.append(n & 0x7F)
                n >>= 7
            out.append(n | 0x80)
            i = j
        else:
            out += b"\x80\x80" if b == 0x80 else bytes([b])
            i += 1
    return bytes(out)


def _sine():
    """Instrument bytes: algorithm 7, operator 4 alone (TL 0, fastest attack, full
    sustain, fastest release), the other three silent (TL 127). Stored as the editor
    keeps them: TL, AR, DR, SR and RR inverted."""
    ins = bytearray([7, 0, 0])
    for k in range(4):
        tl = 0 if k == 3 else 127
        # mult, dt, tl^7f, rs, ar^1f, dr^1f, sr^1f, rr^f, sl, ssg
        ins += bytes([1, 0, tl ^ 0x7F, 0, 31 ^ 0x1F, 0 ^ 0x1F, 0 ^ 0x1F, 15 ^ 0xF, 0, 0])
    return bytes(ins)


def _song(tmp_path, cells, rows=16, speed=3):
    """A version 2 module: one order of one pattern; <cells> maps (channel, row) to
    (note, volume, instrument). Notes are semitones (the editor stores them inverted)."""
    body = bytearray()
    body += b"test author".ljust(64, b"\0") + b"test song".ljust(64, b"\0") + bytes(384)
    body += bytes(256)                                      # order list: pattern 0
    body += b"sine".ljust(16, b"\0") + b"\xff" * 16 * 254   # instrument names
    body += _sine() + bytes(43 * 254)
    body += b"\xff" * 16 * 255 + b"\xff" * 4 * 255          # no samples
    lengths = bytearray(256)
    lengths[0] = rows
    body += lengths
    pattern = bytearray(10 * 11 * 256)
    for (ch, row), (note, vol, ins) in cells.items():
        base = ch * 11 * 256
        pattern[base + row] = 1 if note is None else (~note) & 0xFF
        pattern[base + 256 + row] = vol
        pattern[base + 512 + row] = ins
    body += pattern + bytes(10 * 11 * 256 * 255)
    comp = _rle(bytes(body))
    desc = _rle(bytes(255 * 7))
    head = b"VGEfmtV2" + struct.pack("<II", len(comp) + 13, len(desc))
    head += bytes([speed, speed, 1]) + struct.pack("<HHHH", 11025, 0, 0, 1) + bytes([1, 0])
    p = tmp_path / "test.vge"
    p.write_bytes(head + comp + desc)
    return p


def _available():
    cmd = pipe.helper("vgemm")
    if not cmd:
        pytest.skip("bbmp-retro not built")
    # a bbmp-retro built before the engine was added names the engines it has
    usage = subprocess.run(cmd[:1], capture_output=True, text=True).stderr
    if "vgemm" not in usage:
        pytest.skip("bbmp-retro built without the vgemm engine")


def _tone(samples):
    sounding = [i for i, x in enumerate(samples) if abs(x) > 50]
    tone = samples[sounding[0]:sounding[-1] + 1]
    mean = sum(tone) / len(tone)
    crossings = sum(1 for x, y in zip(tone, tone[1:]) if (x < mean) != (y < mean))
    return len(tone) / 48000, crossings / 2 / (len(tone) / 48000)


def test_an_fm_note_sounds_at_its_pitch_until_its_key_off(tmp_path):
    _available()
    # A (semitone 57) on FM 1 at row 0, key off at row 8; 3 frames a row at 50 Hz
    d = pipe.PipeDecoder("vgemm", _song(tmp_path, {(0, 0): (57, 31, 1), (0, 8): (None, 0, 0)}))
    try:
        assert d.info.duration == pytest.approx(16 * 3 / 50, abs=0.01)
        assert d.info.title == "test song"
        left = _pcm(d, 48000)[0::2]
        seconds, hz = _tone(left)
        # the editor's F-number for A is 1085, block 3 (as its own export writes it)
        assert hz == pytest.approx(1085 * 8 * 7600489 / 144 / 2 ** 21, rel=0.02)
        # 8 rows to the key off, then the fastest release
        assert seconds == pytest.approx(8 * 3 / 50, abs=0.02)
    finally:
        d.close()


def test_a_psg_note_is_a_square_two_octaves_above_fm(tmp_path):
    _available()
    d = pipe.PipeDecoder("vgemm", _song(tmp_path, {(6, 0): (57, 31, 1)}))
    try:
        left = _pcm(d, 48000)[0::2]
        seconds, hz = _tone(left)
        period = int(3546894 / (32 * 880))                 # the PSG plays this A at 880 Hz
        assert hz == pytest.approx(3546894 / 32 / period, rel=0.02)
        assert seconds == pytest.approx(16 * 3 / 50, abs=0.03)
    finally:
        d.close()


def test_damaged_modules_are_refused_without_harm(tmp_path):
    _available()
    good = _song(tmp_path, {(0, 0): (57, 31, 1)}).read_bytes()
    # cut short, cut in the middle of the patterns, an unknown version, runs of nothing
    cases = (good[:20], good[:len(good) // 2], b"VGEfmtV9" + good[8:], good[:29] + b"\x80" * 64)
    for i, bad in enumerate(cases):
        p = tmp_path / f"bad{i}.vge"
        p.write_bytes(bad)
        with pytest.raises(Exception):
            d = pipe.PipeDecoder("vgemm", p)
            d.close()
