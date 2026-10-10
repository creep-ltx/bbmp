# SPDX-License-Identifier: GPL-3.0-or-later
"""Files a little damaged on the archive, mended so that their replayer takes them
(backends/mend.py), and the replayer choices made for whole formats."""

from pathlib import Path

import pytest

from bbmp import companions
from bbmp.backends import Unsupported, open_song
from bbmp.backends import mend
from bbmp.backends.base import Decoder, TrackInfo


def hvl(names: bytes, samples: int) -> bytes:
    head = bytearray(b"HVL\0" + bytes(12))
    head[4:6] = (16).to_bytes(2, "big")         # the names right after the header
    head[12] = samples
    return bytes(head) + names


def test_a_hivelytracker_song_gets_back_the_zero_its_last_name_lost():
    cut = hvl(b"title\0lead\0bas", samples=2)
    assert mend._names_closed(cut) == cut + b"\0"
    assert mend._names_closed(hvl(b"title\0lead\0bass\0", samples=2)) is None
    assert mend._names_closed(b"MOD" + bytes(40)) is None


def test_a_sap_files_last_block_is_filled_up_to_its_end():
    text = b'SAP\r\nTYPE B\r\nINIT 2000\r\nPLAYER 2003\r\n'
    blocks = b"\xff\xff" + bytes((0x00, 0x20, 0x07, 0x20)) + bytes(6)    # $2000-$2007: 8
    assert mend._sap_cut(text + blocks) == text + blocks + bytes(2)
    whole = text + b"\xff\xff" + bytes((0x00, 0x20, 0x01, 0x20)) + bytes(2)
    assert mend._sap_cut(whole) is None
    assert mend._sap_cut(b"PSID" + bytes(100)) is None


def test_a_fast_tracker_file_cut_short_gets_zeros_after_it():
    song = b"Module: " + bytes(300)
    assert mend._ftc_cut(song) == song + bytes(mend.FTC_PAD)
    assert mend._ftc_cut(b"PT3" + bytes(300)) is None


def st11(base: int = 56) -> bytearray:
    data = bytearray(b"ZXAYST11".ljust(base, b"\0") + bytes(mend.ST_PATTERNS + 2 * mend.ST_PATTERN))
    data[base + mend.ST_PATTERN_LENGTH] = 32
    return data


def test_a_sound_tracker_song_loses_only_what_its_replay_never_reads():
    data = st11()
    base, sample = 56, 56 + 3 * mend.ST_SAMPLE
    assert mend._st11(bytes(data)) is None              # nothing to put right
    data[sample + 32] = 0x2F                            # noise with bit 5
    data[sample + 64 + 1] = 0x52                        # a tone shift's high byte
    data[sample + 128], data[sample + 129] = 32, 25     # a loop past the 32 steps
    data[sample + mend.ST_SAMPLE + 128] = 4             # a loop that runs past the end:
    data[sample + mend.ST_SAMPLE + 129] = 40            # it stops there anyway
    row40 = base + mend.ST_PATTERNS + mend.ST_PATTERN + 40 * mend.ST_ROW
    data[row40] = 0x31                                  # a note past the pattern's 32 rows
    row5 = base + mend.ST_PATTERNS + 5 * mend.ST_ROW
    data[row5] = 0x31                                   # and one within them
    fixed = mend._st11(bytes(data))
    assert fixed[sample + 32] == 0x0F
    assert fixed[sample + 64 + 1] == 0x12
    assert fixed[sample + 128:sample + 130] == b"\0\0"
    assert fixed[sample + mend.ST_SAMPLE + 128:sample + mend.ST_SAMPLE + 130] == bytes((4, 31))
    assert fixed[row40] == 0 and fixed[row5] == 0x31
    assert len(fixed) == len(data)


class Fake(Decoder):
    backend = "zxtune"

    def __init__(self, path: Path):
        self.path = path
        self.info = TrackInfo(title="mended")
        self.closed = False

    def read(self, frames):
        return b"\1\0" * 2 * frames

    def close(self):
        self.closed = True


def test_a_song_its_replayer_refuses_is_played_from_a_mended_copy(tmp_path, monkeypatch):
    from bbmp import backends
    song = tmp_path / "song.hvl"
    song.write_bytes(hvl(b"title\0lead\0bas", samples=2))
    opened = []

    def zxtune(path, lengths):
        data = path.read_bytes()
        if data.count(0, 16) < 3:
            raise Unsupported("no such track")
        opened.append(Fake(path))
        return opened[-1]
    monkeypatch.setitem(backends.OPENERS, "zxtune", zxtune)
    monkeypatch.setitem(backends.BACKEND_BY_FORMAT, "HivelyTracker", "zxtune")
    d = open_song(song, "HivelyTracker")
    assert isinstance(d, mend.Mended) and d.backend == "zxtune"
    assert d.info.title == "mended" and d.read(2) == b"\1\0" * 4
    copy = opened[0].path
    assert copy != song and copy.name == song.name and copy.exists()
    d.close()
    assert opened[0].closed and not copy.exists()
    assert song.read_bytes().endswith(b"bas")           # the song itself is left alone


def test_a_song_the_mended_copy_doesnt_help_still_fails(tmp_path, monkeypatch):
    from bbmp import backends
    song = tmp_path / "song.ftc"
    song.write_bytes(b"Module: " + bytes(100))

    def refuses(path, lengths):
        raise Unsupported("no such track")
    for name in ("zxtune", *backends.FALLBACK):
        monkeypatch.setitem(backends.OPENERS, name, refuses)
    monkeypatch.setitem(backends.BACKEND_BY_FORMAT, "Spectrum - Fast Tracker", "zxtune")
    with pytest.raises(Unsupported):
        open_song(song, "Spectrum - Fast Tracker")


def test_the_companions_of_audio_sculpture_and_old_musicmaker_songs():
    assert companions.goes_with("daisy.adsc", "daisy.adsc.as", "Audio Sculpture")
    assert not companions.goes_with("daisy.adsc", "turtle.adsc.as", "Audio Sculpture")
    for name in ("moveback.ip", "moveback.ip.l", "moveback.ip.n"):
        assert companions.is_companion("MusicMaker V8 Old", name)
        assert companions.goes_with("moveback.sdata", name, "MusicMaker V8 Old")
    assert not companions.goes_with("moveback.sdata", "other.ip", "MusicMaker V8 Old")
    assert not companions.is_companion("MusicMaker V8 Old", "moveback.sdata")
