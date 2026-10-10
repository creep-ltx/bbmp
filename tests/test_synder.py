# SPDX-License-Identifier: GPL-3.0-or-later
"""Synder SNG files (Modland's "Synder SNG-Player" and "Synder SNG-Player Stereo") on
the player's own clean-room synthesizer (bbmp-synder)."""

import array
import struct

import pytest

from bbmp.backends import Unsupported, backend_for, is_supported, pipe


class _Packer:
    """aPLib's plain stream, the simplest way: bytes as they are, a 0 byte as
    "1 1 1" and four 0 bits, and the end mark "1 1 0" with the byte 0."""

    def __init__(self):
        self.out = bytearray()
        self.tag = -1          # where the control byte being filled is
        self.bits = 0

    def bit(self, b: int):
        if self.bits == 0:
            self.tag = len(self.out)
            self.out.append(0)
            self.bits = 8
        self.bits -= 1
        self.out[self.tag] |= b << self.bits

    def pack(self, data: bytes) -> bytes:
        self.out.append(data[0])
        for byte in data[1:]:
            if byte == 0:
                for b in (1, 1, 1, 0, 0, 0, 0):
                    self.bit(b)
            else:
                self.bit(0)
                self.out.append(byte)
        for b in (1, 1, 0):
            self.bit(b)
        self.out.append(0)
        return bytes(self.out)


def _song(frames: int = 25, rate: int = 50) -> bytes:
    """Two voices: voice 0 a note of 440 Hz at full loudness, unfiltered, in the middle
    (E = 4, M = 184: (256 + 184) * 16 / 16); voice 1 silent."""
    V = 2
    planes = []
    for k, (v0, v1) in enumerate([(4, 0), (184, 0), (128, 0), (32, 32), (255, 0), (255, 255)]):
        planes.append(bytes([v0]) * frames + bytes([v1]) * frames)
    data = b"".join(planes)
    text = b"Tone - by Nobody : released 2026 Test\x00"
    head = b"SYN" + bytes([0x40 | V]) + struct.pack("<HIHI", rate, 0, len(text), len(data))
    return head + text + _Packer().pack(data)


def _need():
    if not pipe.available("synder"):
        pytest.skip("bbmp-synder not built")


def test_synder_formats_are_played_by_their_helper():
    for name in ("Synder SNG-Player", "Synder SNG-Player Stereo"):
        assert backend_for(name) == "synder"
        assert is_supported(name)


def test_synder_plays_its_frames_at_the_pitch_they_give(tmp_path):
    _need()
    np = pytest.importorskip("numpy")
    song = tmp_path / "tone.sng"
    song.write_bytes(_song())
    d = pipe.PipeDecoder("synder", song)
    try:
        assert d.info.title == "Tone"
        assert d.info.author == "Nobody"
        assert d.info.duration == pytest.approx(0.5, abs=0.001)
        a = array.array("h")
        a.frombytes(d.read(48000))
        assert len(a) == 2 * 24000           # 25 frames of 960 samples
        left, right = np.array(a[0::2], float), np.array(a[1::2], float)
        assert (left == right).all()         # placement code 0 on both sides
        assert np.abs(left).max() > 4000
        # the waveform repeats every 48000 / 440 = 109.1 samples
        x = left[2000:20000] - left[2000:20000].mean()
        corr = [np.dot(x[:-lag], x[lag:]) / np.dot(x[:-lag], x[:-lag]) for lag in range(60, 160)]
        assert 60 + int(np.argmax(corr)) in (108, 109, 110)
    finally:
        d.close()


def test_synder_refuses_damaged_files(tmp_path):
    _need()
    good = _song()
    cases = {
        "cut short": good[:-20],
        "tracker": b"SYNDAPS0" + bytes(40),
        "no frames": good[:12] + struct.pack("<I", 0) + good[16:],
        "part frame": good[:12] + struct.pack("<I", 25 * 12 + 1) + good[16:],
        "too many voices": good[:3] + b"\x49" + good[4:],
        "no rate": good[:4] + b"\x00\x00" + good[6:],
        "text too long": good[:10] + b"\xff\xff" + good[12:],
        "huge": good[:12] + struct.pack("<I", 0xFFFFFFF0) + good[16:],
    }
    for name, data in cases.items():
        bad = tmp_path / f"{name.replace(' ', '_')}.sng"
        bad.write_bytes(data)
        with pytest.raises(Unsupported):
            pipe.PipeDecoder("synder", bad)
