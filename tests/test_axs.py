# SPDX-License-Identifier: GPL-3.0-or-later
"""AXS (.axs), on the player's own clean-room replayer (bbmp-axs)."""

import array
import struct

import pytest

from bbmp.backends import Unsupported, backend_for, is_supported, pipe


def _chunk(tag: bytes, data: bytes) -> bytes:
    return tag + struct.pack(">I", len(data)) + data


def _instrument(**settings: int) -> bytes:
    """An instrument's 128 controller values: a plain sine (OSC 1 only), full volume,
    sustained, the filter open, following the keyboard."""
    c = bytearray(128)
    base = {30: 5, 31: 5, 33: 64, 34: 64, 37: 127, 44: 60, 54: 64, 70: 127, 72: 40,
            74: 127, 77: 64, 78: 64, 79: 64}
    base.update(settings)
    for k, v in base.items():
        c[k] = v
    return bytes(c)


def _song(*, positions: int = 1, last_row: bytes = b"\x00\x00\x00\x00", inst_byte: int = 0,
          dnot: bytes = b"", samp: bytes = b"", tracks: int = 1, blocks: list | None = None) -> bytes:
    """Track 0 plays A-4 (note byte 70: MIDI note 69, 440 Hz) on row 0 of a 16-row block and
    holds it; 134 BPM, 6 ticks a row."""
    shdr = bytes([2, 2, 134, 6, 0, 3, 20, 0, 0, 0, 0, positions]) + bytes(8)
    song = b"".join(struct.pack("<II", b, 0) for b in (blocks or [0] * positions))
    rows = bytearray(16 * 4)
    rows[0:4] = bytes([70, 100, 0, 0])
    rows[60:64] = last_row
    blok = struct.pack("<II", tracks, 16) + bytes([inst_byte]) + bytes(15)
    body = (_chunk(b"USER", b"Nobody\x00\x00") + _chunk(b"SHDR", shdr) + _chunk(b"SONG", song)
            + _chunk(b"BLOK", blok) + _chunk(b"TRAK", bytes(rows)))
    for k in range(8):
        body += _chunk(b"INST", _instrument()) + _chunk(b"INNM", b"SINE%d\x00\x00\x00\x00" % k)
    body += dnot + samp
    return b"FORM" + struct.pack(">I", len(body) + 4) + b"AXSF" + body


def _need():
    if not pipe.available("axs"):
        pytest.skip("bbmp-axs not built")


def test_axs_is_played_by_its_helper():
    assert backend_for("AXS") == "axs"
    assert is_supported("AXS")


def test_axs_plays_a_note_until_the_song_ends(tmp_path):
    _need()
    np = pytest.importorskip("numpy")
    song = tmp_path / "tone.axs"
    song.write_bytes(_song())
    d = pipe.PipeDecoder("axs", song)
    try:
        # 16 rows of 6 ticks; the song ends on tick 97 (79,808 frames in), in block 139
        # of 576 frames
        assert d.info.duration == pytest.approx(139 * 576 / 44100, abs=0.002)
        a = array.array("h")
        while chunk := d.read(65536):
            a.frombytes(chunk)
        assert len(a) // 2 == pytest.approx(139 * 576 * 48000 / 44100, abs=2)
        left, right = np.array(a[0::2], float), np.array(a[1::2], float)
        assert np.abs(left).max() > 1000
        # the right side is the left one turned over and 441 frames (at 44100 Hz) late
        lag = round(441 * 48000 / 44100)
        assert np.corrcoef(left[20000:60000], -right[20000 + lag:60000 + lag])[0, 1] > 0.99
        # 440 Hz: the waveform repeats every 48000 / 440 = 109.1 samples
        x = left[20000:60000] - left[20000:60000].mean()
        corr = [np.dot(x[:-k], x[k:]) / np.dot(x[:-k], x[:-k]) for k in range(60, 160)]
        assert 60 + int(np.argmax(corr)) in (108, 109, 110)
    finally:
        d.close()


def test_axs_stops_a_song_that_repeats_for_ever_at_its_own_length(tmp_path):
    _need()
    song = tmp_path / "loop.axs"
    song.write_bytes(_song(positions=2, last_row=bytes([0, 0, 28, 0])))   # jump to 0
    d = pipe.PipeDecoder("axs", song)
    try:
        # AXS gives it 2 x 16 rows x 6 ticks at 134 BPM: 3582 ms, so 275 blocks
        assert d.info.duration == pytest.approx(275 * 576 / 44100, abs=0.002)
    finally:
        d.close()


def test_axs_refuses_damaged_files(tmp_path):
    _need()
    good = _song()
    at_song = good.index(b"SONG")
    cases = {
        "not a song": b"FORM\x00\x00\x00\x04AXSX" + good[12:],
        "no position list": good[:at_song],
        "missing block": _song(blocks=[3]),
        "instrument out of range": _song(inst_byte=9),
        "drum note out of range": _song(dnot=_chunk(b"DNOT", struct.pack("<10i", 200, *[0] * 9))),
        "negative sample length": _song(samp=_chunk(b"SAMP", struct.pack("<i", 1) + bytes(160)
                                                    + struct.pack("<ii", -5, 44100) + bytes(8))),
        "no song": b"FORM\x00\x00\x00\x04AXSF",
    }
    for name, data in cases.items():
        bad = tmp_path / f"{name.replace(' ', '_')}.axs"
        bad.write_bytes(data)
        with pytest.raises(Unsupported):
            pipe.PipeDecoder("axs", bad)


def test_axs_plays_a_file_cut_short_in_its_last_sample(tmp_path):
    _need()
    samp = _chunk(b"SAMP", struct.pack("<i", 1) + bytes(160) + struct.pack("<ii", 1000, 22050)
                  + bytes(8) + bytes(2000))
    song = tmp_path / "cut.axs"
    song.write_bytes(_song(samp=samp)[:-500])     # as two of Modland's songs are
    d = pipe.PipeDecoder("axs", song)
    try:
        assert d.info.duration == pytest.approx(139 * 576 / 44100, abs=0.002)
    finally:
        d.close()
