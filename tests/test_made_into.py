# SPDX-License-Identifier: GPL-3.0-or-later
"""Formats played as another, once made into it: CYM logs as VGM, Polly Tracker
modules as C64 tunes, SoundMon 1 and Digital Mugician 3 under their kin's labels, and
CHIPNSFX on the tracker's own code."""

import array
import struct

import pytest

from bbmp.backends import Unsupported, pipe, uade
from bbmp.backends.cym import FRAME, to_vgm
from bbmp.backends.polly import END_HIGH, HEADER, PLAYER, tune


def test_a_cym_log_becomes_a_vgm_for_one_ym2151():
    # two frames' wait, a key-on, one frame
    vgm = to_vgm(b"\x00\x00\x08\x78\x00")
    assert vgm[:4] == b"Vgm "
    eof, version = struct.unpack_from("<II", vgm, 4)
    assert eof == len(vgm) - 4 and version == 0x151
    assert struct.unpack_from("<I", vgm, 0x18)[0] == 3 * FRAME       # samples in all
    assert struct.unpack_from("<I", vgm, 0x30)[0] == 3_579_545       # the YM2151's clock
    data = vgm[0x34 + struct.unpack_from("<I", vgm, 0x34)[0]:]
    assert data == b"\x62\x62\x54\x08\x78\x62\x66"


def test_a_file_without_writes_or_time_is_no_cym_log():
    with pytest.raises(Unsupported):
        to_vgm(b"\x00\x00\x00")
    with pytest.raises(Unsupported):
        to_vgm(b"\x08\x78")


def test_a_polly_module_goes_behind_the_player_which_learns_where_it_ends():
    player = bytes(HEADER) + bytes(PLAYER)
    module = bytes(range(256)) * 40                    # 10,240 bytes
    sid = tune(module, "Song", player)
    assert sid[HEADER + PLAYER:] == module
    # loaded at $0801: two bytes of load address, then the player, then the module
    assert sid[HEADER + END_HIGH] == (0x0801 + PLAYER - 2 + len(module) - 1) >> 8
    assert sid[0x16:0x1A] == b"Song"
    with pytest.raises(Unsupported):
        tune(bytes(0x10000), "too big", player)


def test_soundmon_1_and_mugician_3_get_their_kins_labels(tmp_path):
    bp = tmp_path / "song.bp"
    bp.write_bytes(b"SONG".ljust(26, b"\0") + b"BPSM" + bytes(40))
    tmp, made = uade._relabelled_copy(bp)
    try:
        assert made.read_bytes()[26:30] == b"V.2\0"
        assert made.read_bytes()[30:] == bytes(40)
    finally:
        tmp.cleanup()
    dmu = tmp_path / "song.dmu"
    dmu.write_bytes(b" MUGICIAN3/SOFTEYES 1990" + bytes(8))
    tmp, made = uade._relabelled_copy(dmu)
    try:
        assert made.read_bytes()[:24] == b" MUGICIAN2/SOFTEYES 1990"
    finally:
        tmp.cleanup()
    other = tmp_path / "song.mod"
    other.write_bytes(bytes(64))
    assert uade._relabelled_copy(other) is None


SONG = """CHIPNSFX format
Test
Nobody
===
50/1 +00
#00+00 #01+00 #01+00
===
#00 A-4:01 ...:.. ...:.. ...:..
#01 ...:.. ...:.. ...:.. ...:..
===
:01 F000 0000 000 Square
===
"""


def test_chipnsfx_plays_a_note_for_its_rows(tmp_path):
    if not pipe.available("chipnsfx"):
        pytest.skip("bbmp-chipnsfx not built")
    song = tmp_path / "test.chp"
    song.write_text(SONG)
    d = pipe.PipeDecoder("chipnsfx", song)
    try:
        assert d.info.title == "Test"
        assert d.info.duration == pytest.approx(4 / 50, abs=0.011)
        a = array.array("h")
        a.frombytes(d.read(48000))
        left = a[0::2]
        assert 0.07 < len(left) / 48000 < 0.1
        # A-4 is 440 Hz: about 2 x 440 x seconds crossings of zero
        crossings = sum(1 for x, y in zip(left, left[1:]) if (x < 0) != (y < 0))
        assert crossings / 2 / (len(left) / 48000) == pytest.approx(440, rel=0.1)
    finally:
        d.close()


def test_chipnsfx_refuses_what_is_no_song(tmp_path):
    if not pipe.available("chipnsfx"):
        pytest.skip("bbmp-chipnsfx not built")
    bad = tmp_path / "bad.chp"
    bad.write_text("not a song\n")
    with pytest.raises(Unsupported):
        pipe.PipeDecoder("chipnsfx", bad)
    stuck = tmp_path / "stuck.chp"
    stuck.write_text(SONG.replace("50/1", "0/1"))
    with pytest.raises(Unsupported, match="tempo"):
        pipe.PipeDecoder("chipnsfx", stuck)
