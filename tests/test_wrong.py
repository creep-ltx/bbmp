# SPDX-License-Identifier: GPL-3.0-or-later
"""The "this sounds wrong" list: what the ! key writes, and `bbmp wrong`."""

from bbmp import cli, wrong
from bbmp.index import Song


def song(**kw):
    return Song(id=1, path="AHX/Pink/inside.ahx", format="AHX", artist="Pink", album="",
                name="inside.ahx", **kw)


def test_note_writes_what_a_tester_needs(tmp_path):
    f = tmp_path / "data" / "sounds-wrong.txt"
    wrong.note(song(), "  drums   too loud ", at=83.6, subsong=1, subsongs=5,
               replayer="ahx", file=f, when=0)
    wrong.note(song(), "", at=5, file=f, when=0)
    text = f.read_text()
    first, second = text.split("\n\n")[:2]
    assert "modland:AHX/Pink/inside.ahx" in first
    assert "format AHX, replayer ahx, subsong 2 of 5, at 1:23, bbmp " in first
    assert '"drums too loud"' in first
    assert "at 0:05" in second and "subsong" not in second and '"' not in second
    assert wrong.count(f) == 2


def test_count_without_a_list(tmp_path):
    assert wrong.count(tmp_path / "none.txt") == 0


def test_bbmp_wrong_shows_the_list(tmp_path, monkeypatch, capsys):
    f = tmp_path / "sounds-wrong.txt"
    monkeypatch.setattr(wrong, "FILE", f)
    assert cli.main(["wrong"]) == 0
    assert "No song is on the list" in capsys.readouterr().out
    wrong.note(song(), "hiss", file=f)
    assert cli.main(["wrong"]) == 0
    out = capsys.readouterr().out
    assert "1 song that sounds wrong" in out and str(f) in out and '"hiss"' in out
