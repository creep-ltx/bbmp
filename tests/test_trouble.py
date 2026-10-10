# SPDX-License-Identifier: GPL-3.0-or-later
"""Songs known not to play: from the census, and noted as they fail here."""

import time

from bbmp.audio import NullOutput
from bbmp.backends import Unsupported
from bbmp.library import Library
from bbmp.player import Player
from bbmp.settings import Settings
from bbmp.trouble import Trouble

from test_refinements import FakeDecoder


def test_what_failed_here_is_kept_and_let_go_once_it_plays(tmp_path):
    lib = Library(tmp_path / "user.db")
    t = Trouble(lib, known={"modland:A/b/c.mod": ("failed", "fails: census")})
    assert t.get("modland:A/b/c.mod") == "fails: census"
    t.note("modland:X/y/z.mod", "silent", "stayed silent")
    again = Trouble(Library(tmp_path / "user.db"), known={})
    assert again.get("modland:X/y/z.mod") == "stayed silent"        # kept for the next start
    assert again.keys() == {"modland:X/y/z.mod"}
    again.played("modland:X/y/z.mod")
    assert Trouble(Library(tmp_path / "user.db"), known={}).get("modland:X/y/z.mod") is None
    t.played("modland:A/b/c.mod")                                     # the census was wrong here
    assert t.get("modland:A/b/c.mod") is None and "modland:A/b/c.mod" not in t.keys()


def test_the_player_notes_a_song_that_fails_or_stays_silent(catalog, fetcher, monkeypatch, tmp_path):
    songs = catalog.songs("Protracker", "Tester")                    # beep, beep two

    def open_song(path, fmt, lengths=(), local=False):
        if path.name == "beep.mod":
            raise Unsupported("the replayer refuses it")
        return FakeDecoder(subsongs=1, silent={0}, seconds=1.0)
    monkeypatch.setattr("bbmp.player.open_song", open_song)
    monkeypatch.setattr("bbmp.ending.SILENT_SONG", 0.5)
    t = Trouble(Library(tmp_path / "user.db"), known={})
    p = Player(catalog, fetcher, NullOutput(realtime=False), settings=Settings(), trouble=t)
    p.play_list(songs)
    end = time.time() + 8
    while time.time() < end and len(t.keys()) < 2:
        time.sleep(0.05)
    p.shutdown()
    assert t.get(songs[0].key) == "the replayer refuses it"
    assert "stayed silent" in t.get(songs[1].key)


def test_a_server_that_does_not_answer_is_not_the_songs_trouble(catalog, fetcher, monkeypatch, tmp_path):
    def unreachable(key, companions=()):
        raise OSError("no server could be reached")
    monkeypatch.setattr(fetcher, "get", unreachable)
    t = Trouble(Library(tmp_path / "user.db"), known={})
    p = Player(catalog, fetcher, NullOutput(realtime=False), settings=Settings(), trouble=t)
    p.play_list(catalog.songs("Protracker", "Tester")[:1])
    time.sleep(1.0)
    p.shutdown()
    assert t.keys() == set()


def test_radio_passes_over_songs_known_to_fail(catalog, fetcher, tmp_path):
    beep = catalog.by_path("Protracker/Tester/beep.mod")
    t = Trouble(Library(tmp_path / "user.db"), known={beep.key: ("failed", "fails")})
    p = Player(catalog, fetcher, NullOutput(realtime=False), settings=Settings(), trouble=t)
    assert not p._radio_plays(beep)
    assert p._radio_plays(catalog.by_path("Protracker/Tester/beep two.mod"))
    p.shutdown()


def test_the_census_list_is_about_formats_that_play():
    from bbmp import known_trouble
    from bbmp.backends import is_supported
    assert len(known_trouble.KNOWN) > 100
    formats = {k.split(":", 1)[1].split("/")[0] for k in known_trouble.KNOWN}
    assert "Deflemask" in formats
    assert all(state in ("failed", "silent", "noise") for state, _ in known_trouble.KNOWN.values())
