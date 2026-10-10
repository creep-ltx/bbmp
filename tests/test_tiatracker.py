# SPDX-License-Identifier: GPL-3.0-or-later
"""TIATracker songs (Atari 2600, .ttt) on the tracker's own code (bbmp-tiatracker)."""

import array
import copy
import json

import pytest

from bbmp.backends import Unsupported, backend_for, is_supported, pipe


def _song(rows: int = 8, speed: int = 5, frequency: int = 15) -> dict:
    """A tiny song: one note of a pure tone (waveform 4) held for `rows` rows on
    channel 1, nothing on channel 2, PAL."""
    tone = {"version": 1, "name": "tone", "waveform": 4, "envelopeLength": 3,
            "sustainStart": 0, "releaseStart": 1,
            "frequencies": [0, 0, 0], "volumes": [15, 15, 0]}
    empty_ins = {"version": 1, "name": "---", "waveform": 16, "envelopeLength": 2,
                 "sustainStart": 0, "releaseStart": 1, "frequencies": [0, 0], "volumes": [0, 0]}
    empty_perc = {"version": 1, "name": "---", "envelopeLength": 1, "overlay": False,
                  "frequencies": [0], "volumes": [0], "waveforms": [8]}
    hold = {"type": 0, "number": 0, "value": 0}
    note = {"type": 1, "number": 0, "value": frequency}
    return {
        "version": 1, "tvmode": "pal", "globalspeed": True, "evenspeed": speed,
        "oddspeed": speed, "rowsperbeat": 4, "startpattern0": 0, "startpattern1": 0,
        "metaAuthor": "Nobody", "metaName": "Test", "metaComment": "",
        "instruments": [tone] + [empty_ins] * 6,
        "percussion": [empty_perc] * 15,
        "patterns": [
            {"name": "a", "evenspeed": 3, "oddspeed": 3, "notes": [note] + [hold] * (rows - 1)},
            {"name": "b", "evenspeed": 3, "oddspeed": 3, "notes": [hold] * rows},
        ],
        "channels": [{"sequence": [{"patternindex": 0, "gototarget": -1}]},
                     {"sequence": [{"patternindex": 1, "gototarget": -1}]}],
    }


def _need():
    if not pipe.available("tiatracker"):
        pytest.skip("bbmp-tiatracker not built")


def test_tiatracker_is_played_by_its_helper():
    # ASMA's name, and the name older catalogs gave the same songs
    for name in ("TIATracker", "Theta Music Composer"):
        assert backend_for(name) == "tiatracker"
        assert is_supported(name)


def test_tiatracker_plays_a_note_for_its_rows(tmp_path):
    _need()
    song = tmp_path / "test.ttt"
    song.write_text(json.dumps(_song()))
    d = pipe.PipeDecoder("tiatracker", song)
    try:
        assert d.info.title == "Test"
        # 8 rows of 5 frames at 50 frames a second
        assert d.info.duration == pytest.approx(0.8, abs=0.021)
        a = array.array("h")
        a.frombytes(d.read(96000))
        left = a[0::2]
        seconds = len(left) / 48000
        assert 0.78 < seconds < 0.82
        # waveform 4 at frequency 15: the TIA's 31,400 Hz clock / 2 / (15 + 1) = 981 Hz,
        # about 2 x 981 x seconds crossings of zero
        crossings = sum(1 for x, y in zip(left, left[1:]) if (x < 0) != (y < 0))
        assert crossings / 2 / seconds == pytest.approx(31400 / 32, rel=0.1)
    finally:
        d.close()


def test_tiatracker_plays_a_loop_once(tmp_path):
    _need()
    s = _song(rows=4, speed=3)
    s["channels"][0]["sequence"][0]["gototarget"] = 0
    s["channels"][1]["sequence"][0]["gototarget"] = 0
    song = tmp_path / "loop.ttt"
    song.write_text(json.dumps(s))
    d = pipe.PipeDecoder("tiatracker", song)
    try:
        assert d.info.duration == pytest.approx(4 * 3 / 50, abs=0.021)
    finally:
        d.close()


def test_tiatracker_refuses_damaged_songs(tmp_path):
    _need()
    cases = {
        "not json": "{ this is not a song",
        "no instrument": None,
        "no pattern": None,
        "bad start": None,
        "bad speed": None,
        "bad goto": None,
    }
    s = _song()
    s["patterns"][0]["notes"][0]["number"] = 9          # instrument 10 of 7
    cases["no instrument"] = json.dumps(s)
    s = _song()
    s["channels"][1]["sequence"][0]["patternindex"] = 40
    cases["no pattern"] = json.dumps(s)
    s = _song()
    s["startpattern1"] = 5
    cases["bad start"] = json.dumps(s)
    s = _song(speed=0)
    cases["bad speed"] = json.dumps(s)
    s = copy.deepcopy(_song())
    s["channels"][0]["sequence"][0]["gototarget"] = 7
    cases["bad goto"] = json.dumps(s)
    for name, text in cases.items():
        bad = tmp_path / f"{name.replace(' ', '_')}.ttt"
        bad.write_text(text)
        with pytest.raises(Unsupported):
            pipe.PipeDecoder("tiatracker", bad)
