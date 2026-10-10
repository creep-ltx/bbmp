# SPDX-License-Identifier: GPL-3.0-or-later
"""Settings, cache trimming, subsongs, repeat and shuffle, statistics."""

import math
import os
import time

import pytest

from bbmp.audio import NullOutput
from bbmp.backends.base import RATE, Decoder, TrackInfo, Unsupported
from bbmp.library import Library
from bbmp.player import Player
from bbmp.settings import Settings


# ---- settings ----

def test_settings_save_on_change(tmp_path):
    path = tmp_path / "settings.json"
    s = Settings.load(path)
    assert s.volume == 80 and not path.exists()
    s.update(volume=55, repeat="all")
    again = Settings.load(path)
    assert (again.volume, again.repeat) == (55, "all")


def test_settings_survive_a_broken_file(tmp_path):
    path = tmp_path / "settings.json"
    path.write_text("{not json")
    assert Settings.load(path).volume == 80
    path.write_text('{"volume": 30, "shuffle": "yes", "unknown": 1}')
    s = Settings.load(path)
    assert s.volume == 30 and s.shuffle is False   # wrong type falls back to the default


def test_plain_settings_are_not_written(tmp_path, monkeypatch):
    monkeypatch.chdir(tmp_path)
    Settings().update(volume=10)
    assert list(tmp_path.iterdir()) == []


# ---- cache ----

def test_trim_removes_least_recently_used_first(fetcher):
    now = time.time()
    for i, name in enumerate(["A/old.mod", "A/mid.mod", "A/new.mod", "B/fav.mod"]):
        f = fetcher.local_path(name)
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(b"x" * 1000)
        age = {"A/old.mod": 400, "A/mid.mod": 300, "A/new.mod": 100, "B/fav.mod": 500}[name]
        os.utime(f, (now - age, now - age))
    freed = fetcher.trim(2000, keep={"B/fav.mod"})
    assert freed == 2000
    assert not fetcher.is_cached("A/old.mod") and not fetcher.is_cached("A/mid.mod")
    assert fetcher.is_cached("A/new.mod") and fetcher.is_cached("B/fav.mod")
    assert fetcher.trim(0) == 0   # 0 means no limit



def test_measuring_the_cache_passes_over_a_file_that_goes(fetcher, monkeypatch):
    """A trim deleting files while the Settings tab measured the cache closed the
    player (7 October 2026): a file gone meanwhile is left out, and the two take turns."""
    import threading
    f = fetcher.local_path("A/here.mod")
    f.parent.mkdir(parents=True, exist_ok=True)
    f.write_bytes(b"x" * 1000)
    gone = f.parent / "gone.mod"
    real = type(fetcher)._files
    monkeypatch.setattr(type(fetcher), "_files", lambda self: iter([*real(self), gone]))
    assert fetcher.cache_size() == 1000
    fetcher.trim(1)                         # no error either
    monkeypatch.undo()
    # while a trim holds the cache, measuring waits for it
    sizes = []
    with fetcher._trim_lock:
        t = threading.Thread(target=lambda: sizes.append(fetcher.cache_size()))
        t.start()
        t.join(0.2)
        assert sizes == []
    t.join(5)
    assert len(sizes) == 1

def test_using_a_song_marks_it_recent(fetcher, catalog):
    fetcher.get("Protracker/Tester/beep.mod")
    f = fetcher.local_path("Protracker/Tester/beep.mod")
    os.utime(f, (1000, 1000))
    fetcher.get("Protracker/Tester/beep.mod")
    assert f.stat().st_mtime > time.time() - 60


# ---- subsongs, repeat, shuffle ----

class FakeDecoder(Decoder):
    """Subsongs of 0.5 s of tone; the ones listed in `silent` are silence."""
    backend = "fake"

    def __init__(self, subsongs=3, silent=(), seconds=0.5):
        self.info = TrackInfo(subsongs=subsongs, duration=seconds)
        self.silent, self.seconds = set(silent), seconds
        self.played = []
        self.select_subsong(0)

    def select_subsong(self, n):
        self.info.subsong = n
        self.left = int(self.seconds * RATE) if n not in self.silent else 10 * RATE
        self.played.append(n)

    def read(self, frames):
        n = min(frames, self.left)
        self.left -= n
        value = 0 if self.info.subsong in self.silent else 8000
        return (value.to_bytes(2, "little", signed=True) * 2) * n


def run_player(catalog, fetcher, monkeypatch, settings, decoders, songs=None, wait=5.0):
    made = []

    def fake_open(path, fmt, lengths=(), local=False):
        d = decoders()
        made.append(d)
        return d
    monkeypatch.setattr("bbmp.player.open_song", fake_open)
    started = []
    p = Player(catalog, fetcher, NullOutput(), settings=settings, on_song=lambda s: started.append(s.title))
    p.play_list(songs or catalog.songs("Protracker", "Tester"))
    end = time.time() + wait
    while time.time() < end and (p.status().state != "stopped" or not started):
        time.sleep(0.02)
    state = p.status().state
    p.shutdown()
    return made, started, state


def test_all_subsongs_play_and_silent_ones_are_skipped(catalog, fetcher, monkeypatch):
    made, started, _ = run_player(catalog, fetcher, monkeypatch, Settings(),
                                  lambda: FakeDecoder(subsongs=4, silent={1}))
    assert made[0].played == [0, 1, 2, 3]      # 1 was tried, stayed silent, and was skipped
    assert started == ["beep", "beep two"]


def test_album_starts_at_the_files_start_song(catalog, fetcher, monkeypatch):
    class StartsAtTwo(FakeDecoder):
        def __init__(self):
            super().__init__(subsongs=4)
            self.played.clear()
            self.select_subsong(2)
    made, _, _ = run_player(catalog, fetcher, monkeypatch, Settings(), StartsAtTwo)
    assert made[0].played == [2, 3, 0, 1]


def test_one_subsong_per_file(catalog, fetcher, monkeypatch):
    made, started, _ = run_player(catalog, fetcher, monkeypatch, Settings(subsongs="one"),
                                  lambda: FakeDecoder(subsongs=4))
    assert made[0].played == [0] and started == ["beep", "beep two"]


def test_repeat_one_and_all(catalog, fetcher, monkeypatch):
    _, started, state = run_player(catalog, fetcher, monkeypatch, Settings(repeat="one"),
                                   lambda: FakeDecoder(subsongs=1), wait=1.5)
    assert set(started) == {"beep"} and len(started) > 2 and state != "stopped"
    _, started, _ = run_player(catalog, fetcher, monkeypatch, Settings(repeat="all"),
                               lambda: FakeDecoder(subsongs=1), wait=1.5)
    assert started[:4] == ["beep", "beep two", "beep", "beep two"]


def test_shuffle_never_repeats_the_current_song(catalog, fetcher, monkeypatch):
    _, started, _ = run_player(catalog, fetcher, monkeypatch, Settings(shuffle=True, repeat="all"),
                               lambda: FakeDecoder(subsongs=1), wait=1.5)
    assert len(started) > 3
    assert all(a != b for a, b in zip(started, started[1:]))


def test_listening_time_is_reported(catalog, fetcher, monkeypatch):
    done = []

    def fake_open(path, fmt, lengths=(), local=False):
        return FakeDecoder(subsongs=2, seconds=0.5)
    monkeypatch.setattr("bbmp.player.open_song", fake_open)
    p = Player(catalog, fetcher, NullOutput(), settings=Settings(),
               on_done=lambda s, secs: done.append((s.title, round(secs, 1))))
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 5
    while time.time() < end and len(done) < 2:
        time.sleep(0.02)
    p.shutdown()
    assert done[:2] == [("beep", 1.0), ("beep two", 1.0)]   # two 0.5 s subsongs each


# ---- statistics ----

def test_stats_count_listens_not_skips(tmp_path, catalog):
    lib = Library(tmp_path / "user.db")
    beep = catalog.by_path("Protracker/Tester/beep.mod")
    duet = catalog.by_path("Protracker/Other Guy/coop-Tester/duet.mod")
    sid = catalog.by_path("HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")
    for song, seconds in [(beep, 120), (beep, 90), (beep, 5), (duet, 200), (sid, 60)]:
        lib.finish_play(lib.start_play(song), seconds)
    st = lib.stats()
    assert st["plays"] == 4 and st["songs"] == 3 and st["seconds"] == 470
    assert st["top_songs"][0][1] == "beep" and st["top_songs"][0][4] == 2
    assert {a for a, _, _ in st["top_artists"]} == {"Tester", "Other Guy", "Rob Hubbard"}
    assert st["top_formats"][0][:2] == ("Protracker", 3)
    assert lib.stats(since=int(time.time()) + 3600)["plays"] == 0


def test_old_history_is_migrated(tmp_path):
    import sqlite3
    db = tmp_path / "user.db"
    con = sqlite3.connect(db)
    con.execute("CREATE TABLE history(path TEXT, played INTEGER)")
    con.execute("INSERT INTO history VALUES ('AHX/Pink/inside.ahx', 1700000000)")
    con.commit()
    con.close()
    lib = Library(db)
    rows = lib.con.execute("SELECT path, format, artist, started FROM plays").fetchall()
    assert rows == [("modland:AHX/Pink/inside.ahx", "AHX", "Pink", 1700000000)]


# ---- gapless ----

class TimedOutput(NullOutput):
    """Keeps real time and remembers when each piece of audio arrived and every flush."""

    def __init__(self):
        super().__init__(realtime=True)
        self.times, self.flush_times = [], []

    def write(self, pcm):
        self.times.append(time.monotonic())
        super().write(pcm)

    def flush(self):
        self.flush_times.append(time.monotonic())

    @property
    def cuts(self) -> int:
        """Flushes while music was playing: each one cuts off sound still in the buffer."""
        if not self.times:
            return 0
        return sum(1 for t in self.flush_times if self.times[0] < t < self.times[-1])


def slow_player(catalog, fetcher, monkeypatch, settings, out, seconds=1.0):
    opened = []

    def slow_open(path, fmt, lengths=(), local=False):
        time.sleep(0.5)                       # a replayer that takes its time to start
        opened.append(path.name)
        return FakeDecoder(subsongs=1, seconds=seconds)
    monkeypatch.setattr("bbmp.player.open_song", slow_open)
    started = []
    p = Player(catalog, fetcher, out, settings=settings, on_song=lambda s: started.append(s.title))
    return p, started, opened


def test_songs_follow_each_other_without_a_gap(catalog, fetcher, monkeypatch):
    out = TimedOutput()
    p, started, _ = slow_player(catalog, fetcher, monkeypatch, Settings(), out)
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 6
    while time.time() < end and not (len(started) == 2 and p.status().state == "stopped"):
        time.sleep(0.02)
    p.shutdown()
    assert started == ["beep", "beep two"]
    gaps = [b - a for a, b in zip(out.times, out.times[1:])]
    assert max(gaps) < 0.2, f"longest pause between pieces of audio: {max(gaps):.2f} s"
    assert out.cuts == 0             # nothing was cut off where one song ended and the next began


def test_repeat_one_is_gapless_too(catalog, fetcher, monkeypatch):
    out = TimedOutput()
    p, started, _ = slow_player(catalog, fetcher, monkeypatch, Settings(repeat="one"), out)
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 6
    while time.time() < end and len(started) < 3:
        time.sleep(0.02)
    p.shutdown()
    assert started[:3] == ["beep", "beep", "beep"]
    gaps = [b - a for a, b in zip(out.times, out.times[1:])]
    assert max(gaps) < 0.2


def test_skipping_uses_the_prepared_song_and_cuts_the_sound(catalog, fetcher, monkeypatch):
    out = TimedOutput()
    p, started, opened = slow_player(catalog, fetcher, monkeypatch, Settings(), out)
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 3
    while time.time() < end and len(opened) < 2:      # wait until the next song is ready
        time.sleep(0.02)
    t = time.monotonic()
    p.next()
    while time.time() < end and len(started) < 2:
        time.sleep(0.01)
    took = time.monotonic() - t
    p.shutdown()
    assert started == ["beep", "beep two"] and took < 0.2, f"skip took {took:.2f} s"
    assert opened == ["beep.mod", "beep two.mod"]     # opened once each, not again on skip
    assert out.cuts == 1                               # a skip does cut the old song's tail


def test_a_changed_queue_discards_the_prepared_song(catalog, fetcher, monkeypatch):
    out = TimedOutput()
    p, started, opened = slow_player(catalog, fetcher, monkeypatch, Settings(), out)
    songs = catalog.songs("Protracker", "Tester")
    duet = catalog.by_path("Protracker/Other Guy/coop-Tester/duet.mod")
    p.play_list(songs[:1])
    end = time.time() + 6
    while time.time() < end and not started:
        time.sleep(0.02)
    p.enqueue([duet])                                  # the plan was "nothing next"; now there is
    while time.time() < end and len(started) < 2:
        time.sleep(0.02)
    p.shutdown()
    assert started == ["beep", "duet"]
    gaps = [b - a for a, b in zip(out.times, out.times[1:])]
    assert max(gaps) < 0.2



def test_play_next_goes_right_after_the_song_that_plays(catalog, fetcher, monkeypatch):
    out = TimedOutput()
    # songs of 2 s: the song put next has time to open (0.5 s) before the one playing
    # ends; with 1 s songs that was a race, won or lost by a few milliseconds
    p, started, opened = slow_player(catalog, fetcher, monkeypatch, Settings(), out, seconds=2.0)
    songs = catalog.songs("Protracker", "Tester")              # beep, beep two
    duet = catalog.by_path("Protracker/Other Guy/coop-Tester/duet.mod")
    p.play_list(songs)
    end = time.time() + 12
    while time.time() < end and len(opened) < 2:              # beep two is ready to follow
        time.sleep(0.02)
    p.play_next([duet])
    while time.time() < end and len(started) < 3:
        time.sleep(0.02)
    queue, _ = p.queue()
    p.shutdown()
    assert [s.title for s in queue] == ["beep", "duet", "beep two"]
    assert started == ["beep", "duet", "beep two"]
    gaps = [b - a for a, b in zip(out.times, out.times[1:])]
    assert max(gaps) < 0.2                                    # and still without a gap


def test_play_next_with_nothing_playing_plays(catalog, fetcher):
    p = Player(catalog, fetcher, NullOutput(realtime=False), settings=Settings())
    p.play_next([catalog.by_path("Protracker/Tester/beep.mod")])
    end = time.time() + 5
    while time.time() < end and p.status().state != "playing":
        time.sleep(0.02)
    assert p.status().song.title == "beep"
    p.shutdown()

# ---- export ----

def test_export_writes_tagged_files(catalog, fetcher, tmp_path):
    import wave as wave_mod
    from bbmp.export import encoder, export_song
    song = catalog.by_path("Protracker/Tester/beep.mod")
    (wav,) = export_song(catalog, fetcher, song, Settings(), tmp_path / "out", "wav")
    assert wav.name == "Tester - beep.wav"
    with wave_mod.open(str(wav)) as w:
        assert (w.getframerate(), w.getnchannels()) == (48000, 2)
        assert 5 < w.getnframes() / 48000 < 10
    if encoder():
        (flac,) = export_song(catalog, fetcher, song, Settings(), tmp_path / "out", "flac")
        assert flac.suffix == ".flac" and flac.stat().st_size > 1000
        assert not (tmp_path / "out" / "Tester - beep.wav").exists() or flac != wav


@pytest.mark.parametrize("kind, starts", [("mp3", (b"ID3", b"\xff\xfb")), ("ogg", (b"OggS",)),
                                          ("opus", (b"OggS",))])
def test_export_writes_the_smaller_kinds(catalog, fetcher, tmp_path, kind, starts):
    from bbmp.export import ExportError, can_write, export_song
    song = catalog.by_path("Protracker/Tester/beep.mod")
    if not can_write(kind):
        with pytest.raises(ExportError, match="ffmpeg"):
            export_song(catalog, fetcher, song, Settings(), tmp_path / "out", kind)
        return
    (file,) = export_song(catalog, fetcher, song, Settings(export_format=kind), tmp_path / "out")
    assert file.name == f"Tester - beep.{kind}" and file.read_bytes().startswith(starts)
    assert 3000 < file.stat().st_size < 400_000            # some seconds, and smaller than WAV
    assert [f.name for f in (tmp_path / "out").iterdir()] == [file.name]    # no WAV left behind
    if kind == "mp3":
        assert b"Tester" in file.read_bytes()[:2000] and b"beep" in file.read_bytes()[:2000]


def test_export_refuses_a_kind_it_does_not_know(catalog, fetcher, tmp_path):
    from bbmp.export import ExportError, export_song
    song = catalog.by_path("Protracker/Tester/beep.mod")
    with pytest.raises(ExportError, match="not a kind"):
        export_song(catalog, fetcher, song, Settings(), tmp_path / "out", "wma")


def test_export_one_file_per_subsong_and_skips_silent_ones(catalog, fetcher, tmp_path, monkeypatch):
    from bbmp.export import export_song
    monkeypatch.setattr("bbmp.export.open_song",
                        lambda *a, **k: FakeDecoder(subsongs=4, silent={2}, seconds=1.0))
    song = catalog.by_path("Protracker/Tester/beep.mod")
    files = export_song(catalog, fetcher, song, Settings(), tmp_path, "wav")
    assert [f.name for f in files] == ["Tester - beep (01 of 4).wav", "Tester - beep (02 of 4).wav",
                                      "Tester - beep (04 of 4).wav"]
    files = export_song(catalog, fetcher, song, Settings(subsongs="one"), tmp_path / "one", "wav")
    assert [f.name for f in files] == ["Tester - beep (01 of 4).wav"]


def test_export_file_names_are_safe():
    from bbmp.export import safe_name
    assert safe_name('AC/DC: "back" <in> black?') == "AC_DC_ _back_ _in_ black_"
    assert safe_name("...") == "song"


# ---- playlists ----

def test_m3u_lists_are_matched_to_the_catalog(catalog, tmp_path):
    from bbmp import playlists
    text = """#EXTM3U
#EXTINF:31,Beep
http://ftp.amigascne.org/mirrors/ftp.modland.com/pub/modules/Protracker/Tester/beep%20two.mod
ftp://ftp.modland.com/pub/modules/Protracker/-%20unknown/duet.mod
http://example.org/pub/modules/Protracker/Somebody/gone.mod
https://example.org/not/modland.mp3
"""
    paths_ = playlists.parse_m3u(text)
    assert paths_ == ["modland:Protracker/Tester/beep two.mod", "modland:Protracker/- unknown/duet.mod",
                      "modland:Protracker/Somebody/gone.mod"]
    found, missing = playlists.resolve(paths_, catalog)
    # duet was filed under "- unknown" when the list was made; it has an artist now
    assert found == ["modland:Protracker/Tester/beep two.mod",
                     "modland:Protracker/Other Guy/coop-Tester/duet.mod"]
    assert missing == 1
    songs = [catalog.resolve(k) for k in found]
    from bbmp import collection as collections_
    links = "\n".join(collections_.get(s.collection).url(s.path) for s in songs)
    assert playlists.parse_m3u(links) == found       # the links the player gives out
    assert playlists.list_name("assembly_92.m3u") == "Assembly 92"


def test_relocate_never_guesses(catalog):
    # the same file name under another named artist is a different song
    assert catalog.relocate("Protracker/Somebody Else/beep.mod") is None
    assert catalog.relocate("Fasttracker 2/- unknown/beep.mod") is None      # other format
    assert catalog.relocate("Protracker/- unknown/beep.mod").artist == "Tester"


def test_playlist_songs_can_be_moved_and_playlists_renamed(tmp_path):
    lib = Library(tmp_path / "user.db")
    pid = lib.create_playlist("Mix", ["a", "b", "c"])
    assert lib.move_in_playlist(pid, "c", -1) == 1 and lib.playlist_keys(pid) == ["a", "c", "b"]
    assert lib.move_in_playlist(pid, "a", -1) is None            # already first
    assert lib.move_in_playlist(pid, "b", 1) is None             # already last
    assert lib.move_in_playlist(pid, "nope", 1) is None
    lib.remove_from_playlist(pid, "c")                           # leaves a hole in the numbering
    assert lib.move_in_playlist(pid, "b", -1) == 0 and lib.playlist_keys(pid) == ["b", "a"]
    assert lib.add_to_playlist(pid, ["d"]) == 1 and lib.playlist_keys(pid) == ["b", "a", "d"]

    other = lib.create_playlist("Other")
    assert lib.rename_playlist(pid, "Road Trip") and lib.playlist_name(pid) == "Road Trip"
    assert not lib.rename_playlist(pid, "other")                 # taken, whatever the case
    assert not lib.rename_playlist(pid, "  ")
    assert lib.rename_playlist(pid, "road trip")                 # its own name in another case
    assert lib.playlist_name(other) == "Other"


def test_import_a_playlist_file(catalog, tmp_path, mod_bytes):
    from bbmp import playlists
    music = tmp_path / "music"
    (music / "sub").mkdir(parents=True)
    (music / "here.mod").write_bytes(mod_bytes)
    (music / "sub" / "there.mod").write_bytes(mod_bytes)
    lib = Library(tmp_path / "user.db")
    m3u = music / "summer_mix.m3u"
    m3u.write_bytes(("#EXTM3U\r\n#EXTINF:12,Here\r\nhere.mod\r\nsub/there.mod\r\n"
                     f"{music}/sub/there.mod\r\n"                       # the same song again
                     "gone.mod\r\n"
                     "ftp://ftp.modland.com/pub/modules/Protracker/Tester/beep.mod\r\n"
                     "caf\xe9.mod\r\n").encode("cp1252"))                 # not UTF-8
    pid, name, found, missing = playlists.import_file(m3u, catalog, lib)
    assert (name, found, missing) == ("Summer Mix", 3, 3)
    assert lib.playlist_keys(pid) == [str(music / "here.mod"), str(music / "sub" / "there.mod"),
                                       "modland:Protracker/Tester/beep.mod"]
    assert [s.title for s in lib.playlist_songs(pid, catalog)] == ["here", "there", "beep"]
    # a second import of the same file makes a second playlist, it doesn't touch the first
    _, again, _, _ = playlists.import_file(m3u, catalog, lib)
    assert again == "Summer Mix 2"

    empty = music / "empty.m3u"
    empty.write_text("#EXTM3U\nnothing-here.mod\n")
    import pytest
    with pytest.raises(ValueError):
        playlists.import_file(empty, catalog, lib)
    with pytest.raises(OSError):
        playlists.import_file(music / "no-such-file.m3u", catalog, lib)


def test_playlist_files_from_other_players_are_read(tmp_path, mod_bytes):
    from bbmp import playlists
    music = tmp_path / "music"
    (music / "sub").mkdir(parents=True)
    (music / "sub" / "there.mod").write_bytes(mod_bytes)
    (music / "lists").mkdir()
    m3u = music / "lists" / "mixed.m3u"
    m3u.write_text("../sub/./there.mod\n"               # a way round to it
                   "..\\sub\\there.mod\n"              # Windows' way, in a list that has "/" too
                   f"file://localhost{music}/sub/there.mod\n"
                   f"file://{music}/sub/../sub/there.mod\n"
                   "file://other-computer/music/there.mod\n")      # not had
    assert playlists.read_playlist_file(m3u) == [str(music / "sub" / "there.mod")] * 4


def test_two_playlists_made_at_once_never_share_a_name(tmp_path):
    first, second = Library(tmp_path / "user.db"), Library(tmp_path / "user.db")
    second.create_playlist("Chips")
    # the name looked free a moment ago: another player took it since
    first.playlist_id = lambda name: None
    assert first.playlist_name(first.create_playlist("Chips")) == "Chips 2"
    assert first.playlist_name(first.create_playlist("chips")) == "chips 3"     # any case



# ---- themes ----

def test_palette_follows_the_theme():
    from bbmp import theme
    p = theme.from_variables({"accent": "#112233", "primary": "#445566", "success": "#00ff00",
                              "warning": "#ffff00", "error": "#ff0000", "foreground": "ansi_nonsense"})
    assert (p.accent, p.primary) == ("#112233", "#445566")
    assert p.text == theme.Palette().text                     # not a colour: the default is kept
    assert [p.bar(h) for h in (0.1, 0.6, 0.95)] == ["#00ff00", "#ffff00", "#ff0000"]

    shaded = theme.from_variables({"modland-bars": "#000000,#ffffff"})
    assert [shaded.bar(h) for h in (0.0, 0.5, 1.0)] == ["#000000", "#808080", "#ffffff"]
    stepped = theme.from_variables({"modland-bars": "#000000,#ffffff", "modland-bars-mode": "steps"})
    assert [stepped.bar(h) for h in (0.0, 0.49, 0.5, 1.0)] == ["#000000", "#000000", "#ffffff", "#ffffff"]
    assert theme.from_variables({"modland-bars": "#000000,nope"}).bars == ()
    named = theme.from_variables({"primary": "#6688bb", "modland-primary": "#000000"})
    assert named.primary == "#000000"


def test_amiga_themes_use_the_workbench_palettes():
    from bbmp import amiga_themes, theme
    names = [t.name for t in amiga_themes.THEMES]
    assert names == ["amiga-workbench-1.3", "amiga-workbench-3.1", "amiga-copper",
                     "amiga-guru-meditation"]
    wb13, wb31 = amiga_themes.WORKBENCH_13, amiga_themes.WORKBENCH_31
    assert (wb13.background, wb13.foreground, wb13.primary, wb13.error) == (
        "#0055AA", "#FFFFFF", "#FF8800", "#000022")
    assert (wb31.background, wb31.foreground, wb31.primary) == ("#AAAAAA", "#000000", "#6688BB")
    for t in amiga_themes.THEMES:
        palette = theme.from_variables({**t.variables, "accent": t.accent, "primary": t.primary})
        assert palette.bars, t.name
        four = {wb13.background, wb13.foreground, wb13.primary, wb13.error}
        if t is wb13:
            assert set(palette.bars) <= four and not palette.blend


# ---- what is known about formats ----

def test_what_is_known_about_a_format(monkeypatch):
    from bbmp import formats_info
    monkeypatch.setattr(formats_info, "NOTES", {
        "Protracker": ("verified", "40 of 40 tried", "Compared with the original."),
        "Fasttracker 2": ("faults", "38 of 40 tried", "2 of 40 tried are silent."),
        "Renoise Old": ("no", "0", "Renoise's effect devices were never published."),
        "Video Game Music": ("fine", "40 of 40 tried", ""),
        "TFMX": ("no", "0", "From before it played."),
    })
    good = formats_info.about("Protracker")
    assert (good.group, good.mark, good.title) == ("verified", "✓", "Verified against the original")
    assert good.line == ("Verified against the original · 40 of 40 tried songs play"
                         " · Compared with the original.")
    assert formats_info.about("Fasttracker 2").mark == "!"
    # a system of Video Game Music has what is known about Video Game Music
    assert formats_info.about("Video Game Music - Sega Megadrive").plays == "40 of 40 tried"
    # no notes: what the player knows anyway
    assert formats_info.about("IFF-SMUS").group == "fine"
    assert formats_info.about("IFF-SMUS").text == ""
    # notes that say a format doesn't play, from before it did, are not shown
    assert formats_info.about("TFMX").group == "fine"
    gone = formats_info.about("Renoise Old")
    assert gone.group == "no" and gone.mark == "✗"
    assert formats_info.why_not("Renoise Old") == (
        "Can't play Renoise Old files. Renoise's effect devices were never published.")


# ---- extra files ----

def test_extra_files_are_found_in_their_folder(tmp_path):
    from bbmp import extras, formats_info
    folder = extras.folder()
    assert folder == tmp_path / "extras"
    coleco, mario = extras.get("coleco"), extras.get("mariopaint")
    assert extras.state(coleco) == (None, "not there")          # no folder yet
    assert [e.id for e in extras.for_format("SGC")] == ["coleco"]
    assert extras.missing("SGC") == []                          # most SGC songs do without
    assert [e.id for e in extras.missing("Shroom")] == ["mariopaint"]

    folder.mkdir()
    (folder / "COLECO.ROM").write_bytes(b"x" * 100)             # some other file of that name
    path, note = extras.state(coleco)
    assert path is None and "100 bytes" in note and "8,192" in note
    (folder / "COLECO.ROM").write_bytes(b"x" * 8192)
    assert extras.state(coleco) == (folder / "COLECO.ROM", "there")
    assert extras.env("sgc", ["bbmp-nez", "sgc"])["BBMP_COLECO_BIOS"] == str(folder / "COLECO.ROM")
    assert "BBMP_COLECO_BIOS" not in extras.env("openmpt", [])

    text = extras.describe(mario)
    assert "mario-paint-notes.wav" in text and str(folder) in text and "not there" in text
    assert "can't be played" in text
    with open(folder / "notes.wav", "wb") as f:                 # the name it has in Mariopants
        f.truncate(mario.size)
    assert extras.find("mariopaint") == folder / "notes.wav"
    assert extras.missing("Shroom") == []
    assert "It is there" in extras.describe(mario)


def test_the_playstation_bios_is_known_by_its_contents(monkeypatch):
    import dataclasses
    import hashlib
    from bbmp import extras
    bios = extras.get("ps2bios")
    assert [e.id for e in extras.missing("Playstation Sound Format")] == ["ps2bios"]
    assert [e.id for e in extras.missing("Playstation 2 Sound Format")] == ["ps2bios"]
    assert "BBMP_PS2_BIOS" not in extras.env("zxtune", [])
    folder = extras.folder()
    folder.mkdir()
    (folder / "HEBIOS.BIN").write_bytes(bytes(bios.size))       # the right size, another file
    path, note = extras.state(bios)
    assert path is None and "another file" in note
    assert extras.missing("Playstation Sound Format") == [bios]
    good = dataclasses.replace(bios, sha256=(hashlib.sha256(bytes(bios.size)).hexdigest(),))
    monkeypatch.setattr(extras, "EXTRAS", tuple(good if e.id == "ps2bios" else e
                                                for e in extras.EXTRAS))
    assert extras.state(good) == (folder / "HEBIOS.BIN", "there")
    assert extras.missing("Playstation 2 Sound Format") == []
    assert extras.env("zxtune", ["bbmp-zxtune"])["BBMP_PS2_BIOS"] == str(folder / "HEBIOS.BIN")
    assert "BBMP_PS2_BIOS" not in extras.env("openmpt", [])


def test_the_ym2608_rom_goes_to_every_helper_that_plays_the_chip():
    """The YM2608's drums are not built into bbmp-vgm or Furnace: they read the user's ROM."""
    from bbmp import extras
    folder = extras.folder()
    folder.mkdir()
    (folder / "ym2608_adpcm_rom.bin").write_bytes(bytes(8192))
    for backend in ("pmdc", "fmp", "vgm", "furnace"):
        assert extras.env(backend, [])["BBMP_YM2608_ROM"] == str(folder / "ym2608_adpcm_rom.bin")
    assert "BBMP_YM2608_ROM" not in extras.env("openmpt", [])
    assert [e.id for e in extras.for_format("S98")] == ["ym2608"]
    assert [e.id for e in extras.for_format("Furnace")] == ["ym2608"]


def test_a_format_that_needs_a_file_says_so(monkeypatch):
    from bbmp import extras, formats_info
    monkeypatch.setattr(formats_info, "NOTES", {"Shroom": ("fine", "63 of 63", "Compared.")})
    monkeypatch.setattr(formats_info, "is_supported", lambda fmt: True)
    info = formats_info.about("Shroom")
    assert (info.group, info.mark, info.title) == ("needs", "+", "Needs a file")
    assert info.counted == "63 of 63 songs play with it"
    assert "mario-paint-notes.wav (26,936,466 bytes)" in info.files
    said = formats_info.why_not("Shroom")
    assert said.startswith("Shroom needs a file. mario-paint-notes.wav")
    assert str(extras.folder()) in said
    # a file that only some songs use doesn't keep the format from playing
    pmd = formats_info.about("PMD")
    assert pmd.group == "fine" and "ym2608_adpcm_rom.bin" in pmd.files and "not there" in pmd.files
    assert formats_info.about("Protracker").files == ""


# ---- downloads ----

def _site(tmp_path, files: dict[str, bytes]):
    """A web server on this computer that has these files."""
    import functools
    import http.server
    import threading
    site = tmp_path / "site"
    site.mkdir()
    for name, data in files.items():
        (site / name).write_bytes(data)
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(site))
    handler.log_message = lambda *a: None
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def _disk_zip() -> bytes:
    import io
    import zipfile
    data = io.BytesIO()
    with zipfile.ZipFile(data, "w") as z:
        z.writestr("CyberTracker V1_01.d64", b"a disk image")
        z.writestr("readme.txt", b"hello")
    return data.getvalue()


def test_a_download_is_fetched_checked_and_unpacked(tmp_path, monkeypatch):
    import dataclasses
    import hashlib
    import pytest
    from bbmp import downloads, formats_info
    monkeypatch.setattr(formats_info, "is_supported", lambda fmt: True)
    packed = _disk_zip()
    server = _site(tmp_path, {"tracker.zip": packed, "other.zip": b"something else"})
    base = f"http://127.0.0.1:{server.server_port}/"
    real = downloads.get("cybertracker")
    disk = dataclasses.replace(real, url=base + "tracker.zip", size=len(packed),
                               sha256=hashlib.sha256(packed).hexdigest())
    try:
        assert not downloads.installed(disk) and downloads.missing("Cybertracker C64") == [real]
        info = formats_info.about("Cybertracker C64")
        assert info.group == "needs" and "Downloads in Settings fetches it" in info.files
        assert formats_info.why_not("Cybertracker C64").startswith(
            "Cybertracker C64 needs a file. CyberTracker 1.01:")
        assert downloads.env("cybertracker") == {}

        said = []
        downloads.install(disk, progress=said.append)
        assert said == ["Downloading CyberTracker 1.01...", "CyberTracker 1.01 is there."]
        kept = downloads.folder(disk) / "cybertracker101.d64"
        assert kept.read_bytes() == b"a disk image"
        assert sorted(f.name for f in downloads.folder().iterdir()) == ["cybertracker"]
        assert downloads.installed(real) and downloads.missing("Cybertracker C64") == []
        assert downloads.env("cybertracker") == {"BBMP_CYBERTRACKER_DISK": str(kept)}
        assert formats_info.about("Cybertracker C64").group != "needs"

        # what arrives is checked: another file is not taken, and what was there stays
        wrong = dataclasses.replace(disk, url=base + "other.zip")
        with pytest.raises(ValueError, match="not the file that was expected"):
            downloads.install(wrong)
        assert kept.read_bytes() == b"a disk image"
        with pytest.raises(OSError):
            downloads.install(dataclasses.replace(disk, url=base + "gone.zip"))
        downloads.remove(disk)
        assert not downloads.installed(disk)
        assert list(downloads.folder().iterdir()) == []         # and nothing is left lying
    finally:
        server.shutdown()
        server.server_close()


def _drivers_tar(headers: dict[str, bytes]) -> bytes:
    import io
    import tarfile
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as tar:
        for name, text in headers.items():
            info = tarfile.TarInfo(f"kss-drivers-e8032da/{name}")
            info.size = len(text)
            tar.addfile(info, io.BytesIO(text))
    return data.getvalue()


def test_the_msx_drivers_are_pinned_and_unpacked_from_their_c_arrays(tmp_path, monkeypatch):
    import dataclasses
    import hashlib
    import pytest
    from bbmp import downloads, formats_info
    monkeypatch.setattr(formats_info, "is_supported", lambda fmt: True)
    real = downloads.get("msxdrivers")
    # pinned: one commit of kss-drivers, the archive's SHA-256 and each driver's
    assert "e8032da38ea8439caca699954fce82b5050bf86f" in real.url and len(real.sha256) == 64
    assert sorted(downloads.MSX_DRIVERS) == ["kinrou5", "mpk103", "mpk106", "opx4kss"]
    assert real.files == tuple(f"{n}.bin" for n in downloads.MSX_DRIVERS)
    assert real.backend == "kss" and set(real.formats) == {"MuSICA", "MPK", "OPLLDRV"}
    # a C array's bytes, its comments (which hold other hex numbers) left out
    assert downloads.c_array("/*\n kinrou5.drv (SHA1: 0x91bf)\n*/\n0xFE,0x00,\n0x0a // 0x11\n") \
        == b"\xfe\x00\x0a"

    drivers = {n: bytes([i, 0xC3, 0x10 + i]) for i, n in enumerate(downloads.MSX_DRIVERS)}
    headers = {f"{n}.h": ("/* " + n + " */\n" + ",".join(f"0x{b:02X}" for b in d)).encode()
               for n, d in drivers.items()}
    headers["mgsdrv.h"] = b"0x01"           # the ones built in are not taken
    packed = _drivers_tar(headers)
    monkeypatch.setattr(downloads, "MSX_DRIVERS",
                        {n: hashlib.sha256(d).hexdigest() for n, d in drivers.items()})
    other = dict(headers, **{"opx4kss.h": b"0x00,0x01"})
    server = _site(tmp_path, {"drivers.tar.gz": packed, "other.tar.gz": _drivers_tar(other)})
    base = f"http://127.0.0.1:{server.server_port}/"
    fake = dataclasses.replace(real, url=base + "drivers.tar.gz", size=len(packed),
                               sha256=hashlib.sha256(packed).hexdigest())
    asks = "MuSICA songs need MuSICA's driver (kinrou5), which is not here"
    try:
        assert downloads.missing("MuSICA") == [real] and downloads.missing("MGSDRV") == []
        assert formats_info.about("OPLLDRV").group == "needs"
        assert downloads.env("kss") == {}
        # advised when the helper asks for it, and not for every KSS song that fails
        assert "bbmp download msxdrivers" in downloads.advice("kss", asks)
        assert downloads.advice("kss", "libkss can't load this file") == ""
        assert downloads.advice("kss") == ""

        downloads.install(fake)
        for n, d in drivers.items():
            assert (downloads.folder(real) / f"{n}.bin").read_bytes() == d
        assert sorted(f.name for f in downloads.folder(real).iterdir()) == sorted(real.files)
        assert downloads.installed(real) and downloads.missing("MuSICA") == []
        assert downloads.env("kss") == {"BBMP_MSX_DRIVERS": str(downloads.folder(real))}
        assert downloads.advice("kss", asks) == ""

        # a driver that is not the one expected is not taken, though the archive's pin
        # (here made to fit) let it through
        wrong = _drivers_tar(other)
        with pytest.raises(ValueError, match="opx4kss.h is not the driver that was expected"):
            downloads.install(dataclasses.replace(
                fake, url=base + "other.tar.gz", size=len(wrong),
                sha256=hashlib.sha256(wrong).hexdigest()))
        assert downloads.installed(real)                        # what was there stays
    finally:
        server.shutdown()
        server.server_close()


def test_the_kss_helper_is_told_where_the_msx_drivers_are(tmp_path, monkeypatch):
    import pytest
    from bbmp import downloads
    from bbmp.backends import pipe
    from bbmp.backends.base import Unsupported
    script = tmp_path / "kss-helper"
    script.write_text(
        '#!/bin/sh\n'
        'if [ -n "$BBMP_MSX_DRIVERS" ]; then\n'
        '  echo "{\\"tracks\\": 1, \\"title\\": \\"$BBMP_MSX_DRIVERS\\"}"; head -c 4000 /dev/zero\n'
        'else\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"MuSICA songs need MuSICA\'s driver (kinrou5), which is not here\\"}"\n'
        'fi\n')
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script)])
    song = tmp_path / "song.bgm"
    song.write_bytes(b"BGM")
    with pytest.raises(Unsupported) as refused:
        pipe.PipeDecoder("kss", song)
    assert "MuSICA's driver (kinrou5)" in str(refused.value)
    assert "bbmp download msxdrivers" in str(refused.value)
    there = downloads.folder(downloads.get("msxdrivers"))
    there.mkdir(parents=True)
    for f in downloads.get("msxdrivers").files:
        (there / f).write_bytes(b"\xc3")
    d = pipe.PipeDecoder("kss", song)
    try:
        assert d.info.title == str(there)
    finally:
        d.close()


def _beepola_tar(images: dict[str, bytes]) -> bytes:
    import io
    import tarfile
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as tar:
        for name, content in images.items():
            info = tarfile.TarInfo(f"beepola-4145966ce0e6d96eeed57b522882915fbad23244/{name}")
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
    return data.getvalue()


def test_beepolas_data_is_pinned_and_cut_out_of_its_memory_images(tmp_path, monkeypatch):
    import dataclasses
    import hashlib
    import pytest
    from bbmp import download_pins, downloads, formats_info
    monkeypatch.setattr(formats_info, "is_supported", lambda fmt: True)
    real = downloads.get("beepoladata")
    # pinned: the commit bbmp-ay is built from, the archive's SHA-256 and each piece's
    # place in it and SHA-256
    assert "4145966ce0e6d96eeed57b522882915fbad23244" in real.url and len(real.sha256) == 64
    assert real.files == ("zx48.rom", "p1d-samples.bin", "svg-table-tail.bin")
    assert [p[2] for p in download_pins.BEEPOLA_DATA.values()] == [16384, 1024, 136]
    assert real.backend == "beepola" and real.formats == ("Beepola",)

    # memory images of 64 KB holding the pieces where the pins say
    pieces = {n: bytes((i * 7 + k) & 0xFF for k in range(p[2]))
              for i, (n, p) in enumerate(download_pins.BEEPOLA_DATA.items())}
    images: dict[str, bytearray] = {}
    for name, (image, at, size, _) in download_pins.BEEPOLA_DATA.items():
        img = images.setdefault(image, bytearray(65536))
        img[at:at + size] = pieces[name]
    packed = _beepola_tar({**{k: bytes(v) for k, v in images.items()}, "LICENSE.txt": b"x"})
    monkeypatch.setattr(downloads, "BEEPOLA_DATA", {
        n: (*p[:3], hashlib.sha256(pieces[n]).hexdigest())
        for n, p in download_pins.BEEPOLA_DATA.items()})
    changed = {k: bytes(v) for k, v in images.items()}
    svg = bytearray(changed["svg.img"])
    svg[download_pins.BEEPOLA_DATA["svg-table-tail.bin"][1]] ^= 1
    changed["svg.img"] = bytes(svg)
    wrong = _beepola_tar(changed)
    server = _site(tmp_path, {"beepola.tar.gz": packed, "other.tar.gz": wrong})
    base = f"http://127.0.0.1:{server.server_port}/"
    fake = dataclasses.replace(real, url=base + "beepola.tar.gz", size=len(packed),
                               sha256=hashlib.sha256(packed).hexdigest())
    asks = "This song needs Beepola's data (zx48.rom), which is not here"
    try:
        assert downloads.missing("Beepola") == []          # not every Beepola song needs it
        assert downloads.env("beepola") == {}
        assert "bbmp download beepoladata" in downloads.advice("beepola", asks)
        assert downloads.advice("beepola", "Beepola can't play this file") == ""
        assert downloads.advice("ay", asks) == ""          # the other engines: not theirs

        downloads.install(fake)
        for n, d in pieces.items():
            assert (downloads.folder(real) / n).read_bytes() == d
        assert sorted(f.name for f in downloads.folder(real).iterdir()) == sorted(real.files)
        assert downloads.installed(real)
        assert downloads.env("beepola") == {"BBMP_BEEPOLA_DATA": str(downloads.folder(real))}
        assert downloads.env("fls") == {}
        assert downloads.advice("beepola", asks) == ""

        # a piece that is not the one expected is not taken, though the archive's pin
        # (here made to fit) let it through
        with pytest.raises(ValueError, match="svg.img does not hold the svg-table-tail.bin"):
            downloads.install(dataclasses.replace(
                fake, url=base + "other.tar.gz", size=len(wrong),
                sha256=hashlib.sha256(wrong).hexdigest()))
        assert downloads.installed(real)                        # what was there stays
    finally:
        server.shutdown()
        server.server_close()


def test_the_beepola_engine_is_told_where_beepolas_data_is(tmp_path, monkeypatch):
    import pytest
    from bbmp import downloads
    from bbmp.backends import pipe
    from bbmp.backends.base import NeedsDownload
    script = tmp_path / "ay-helper"
    script.write_text(
        '#!/bin/sh\n'
        'if [ -n "$BBMP_BEEPOLA_DATA" ]; then\n'
        '  echo "{\\"tracks\\": 1, \\"title\\": \\"$BBMP_BEEPOLA_DATA\\"}"; head -c 4000 /dev/zero\n'
        'else\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"This song needs Beepola\'s data (p1d-samples.bin), which is not here\\"}"\n'
        'fi\n')
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script)])
    song = tmp_path / "song.bbsong"
    song.write_bytes(b"BBSONG")
    with pytest.raises(NeedsDownload) as refused:
        pipe.PipeDecoder("beepola", song)
    assert "Beepola's data (p1d-samples.bin)" in str(refused.value)
    assert "bbmp download beepoladata" in str(refused.value)
    there = downloads.folder(downloads.get("beepoladata"))
    there.mkdir(parents=True)
    for f in downloads.get("beepoladata").files:
        (there / f).write_bytes(b"\x00")
    d = pipe.PipeDecoder("beepola", song)
    try:
        assert d.info.title == str(there)
    finally:
        d.close()


def _ixalance_tar(source: str) -> bytes:
    import io
    import tarfile
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as tar:
        for name, content in (("src/WaveLibrary.c", source.encode()), ("LICENSE", b"x")):
            info = tarfile.TarInfo(
                f"playback-ixalance-9b474c80ce1e6f6b77d0290fd51958b3663ba8af/{name}")
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
    return data.getvalue()


def test_ixalances_library_samples_are_pinned_and_read_out_of_c_arrays(tmp_path, monkeypatch):
    import dataclasses
    import hashlib
    import pytest
    from bbmp import download_pins, downloads, formats_info
    monkeypatch.setattr(formats_info, "is_supported", lambda fmt: True)
    real = downloads.get("ixalancesamples")
    # pinned: the plugin's commit, the archive's SHA-256, each sample's array, length and
    # SHA-256
    assert "9b474c80ce1e6f6b77d0290fd51958b3663ba8af" in real.url and len(real.sha256) == 64
    assert real.files == ("909OH.WAV", "909SD.WAV", "CRASH.WAV")
    assert [p[1] for p in download_pins.IXS_LIBRARY.values()] == [3424, 3413, 16098]
    assert real.backend == "ixalance" and real.formats == ("Ixalance",)

    # a source file with the samples as C arrays, decimal, one of them in hex
    samples = {n: bytes((i * 11 + k) & 0xFF for k in range(p[1]))
               for i, (n, p) in enumerate(download_pins.IXS_LIBRARY.items())}

    def source(samples: dict[str, bytes]) -> str:
        out = ["/* a comment { 1, 2 } */\n// just samples\n#include <x.h>\n"]
        for i, (n, (array, size, _)) in enumerate(download_pins.IXS_LIBRARY.items()):
            nums = ",".join((f"0x{b:02x}" if i == 1 else str(b)) for b in samples[n])
            out.append(f"const unsigned char {array}[{size}]  = {{\n{nums}\n}};\n")
        return "".join(out)
    packed = _ixalance_tar(source(samples))
    monkeypatch.setattr(downloads, "IXS_LIBRARY", {
        n: (*p[:2], hashlib.sha256(samples[n]).hexdigest())
        for n, p in download_pins.IXS_LIBRARY.items()})
    changed = dict(samples)
    changed["CRASH.WAV"] = bytes([samples["CRASH.WAV"][0] ^ 1]) + samples["CRASH.WAV"][1:]
    wrong = _ixalance_tar(source(changed))
    server = _site(tmp_path, {"ixs.tar.gz": packed, "other.tar.gz": wrong})
    base = f"http://127.0.0.1:{server.server_port}/"
    fake = dataclasses.replace(real, url=base + "ixs.tar.gz", size=len(packed),
                               sha256=hashlib.sha256(packed).hexdigest())
    asks = "This song needs iXalance's library samples (909OH.WAV, CRASH.WAV), which are not here"
    try:
        assert downloads.missing("Ixalance") == []        # 17 of the 24 songs play without
        assert downloads.env("ixalance") == {}
        assert "bbmp download ixalancesamples" in downloads.advice("ixalance", asks)
        assert downloads.advice("ixalance", "ixalance can't play this file") == ""
        assert downloads.advice("klystrack", asks) == ""

        downloads.install(fake)
        for n, d in samples.items():
            assert (downloads.folder(real) / n).read_bytes() == d
        assert sorted(f.name for f in downloads.folder(real).iterdir()) == sorted(real.files)
        assert downloads.installed(real)
        assert downloads.env("ixalance") == {"BBMP_IXS_DATA": str(downloads.folder(real))}
        assert downloads.env("klystrack") == {}
        assert downloads.advice("ixalance", asks) == ""

        # a sample that is not the one expected is not taken, though the archive's pin
        # (here made to fit) let it through
        with pytest.raises(ValueError, match="WAVE_802 is not the CRASH.WAV"):
            downloads.install(dataclasses.replace(
                fake, url=base + "other.tar.gz", size=len(wrong),
                sha256=hashlib.sha256(wrong).hexdigest()))
        assert downloads.installed(real)                        # what was there stays
    finally:
        server.shutdown()
        server.server_close()


def test_the_ixalance_engine_is_told_where_its_library_samples_are(tmp_path, monkeypatch):
    import pytest
    from bbmp import downloads
    from bbmp.backends import pipe
    from bbmp.backends.base import NeedsDownload
    script = tmp_path / "rv-helper"
    script.write_text(
        '#!/bin/sh\n'
        'if [ -n "$BBMP_IXS_DATA" ]; then\n'
        '  echo "{\\"tracks\\": 1, \\"title\\": \\"$BBMP_IXS_DATA\\"}"; head -c 4000 /dev/zero\n'
        'else\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"This song needs iXalance\'s library samples (CRASH.WAV), which are not here\\"}"\n'
        'fi\n')
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script)])
    song = tmp_path / "song.ixs"
    song.write_bytes(b"IXS!")
    with pytest.raises(NeedsDownload) as refused:
        pipe.PipeDecoder("ixalance", song)
    assert "library samples (CRASH.WAV)" in str(refused.value)
    assert "bbmp download ixalancesamples" in str(refused.value)
    there = downloads.folder(downloads.get("ixalancesamples"))
    there.mkdir(parents=True)
    for f in downloads.get("ixalancesamples").files:
        (there / f).write_bytes(b"\x00")
    d = pipe.PipeDecoder("ixalance", song)
    try:
        assert d.info.title == str(there)
        assert there in d._reads()                       # the sandbox lets it read them
    finally:
        d.close()


def _players_tar(files: dict[str, bytes], root: str) -> bytes:
    import io
    import tarfile
    data = io.BytesIO()
    with tarfile.open(fileobj=data, mode="w:gz") as tar:
        for name, content in files.items():
            info = tarfile.TarInfo(f"{root}/{name}")
            info.size = len(content)
            tar.addfile(info, io.BytesIO(content))
    return data.getvalue()


def test_the_build_keeps_exactly_the_amiga_players_and_sc68_replays_not_downloaded():
    """The players the builds keep (their terms allow passing them on) and those the
    player downloads are the whole of UADE's and sc68's, and none is both."""
    import re
    from pathlib import Path
    from bbmp.download_pins import AMIGA_PLAYERS, SC68_REPLAYS
    root = Path(__file__).resolve().parents[1] / "tools"

    def listed(script: str, var: str) -> set[str]:
        text = re.sub(r"#[^\n]*", "", (root / script).read_text())
        return set(re.search(var + r"=\((.*?)\)", text, re.S).group(1).split())
    kept = listed("build-uade.sh", "FINE_PLAYERS")
    fetched = {n for n in AMIGA_PLAYERS if "/" not in n}
    assert len(kept) == 18 and len(fetched) == 158 and not kept & fetched
    assert all(n.startswith("ENV/EaglePlayer/") for n in AMIGA_PLAYERS if "/" in n)
    assert listed("build-amiga.sh", "FINE_PLAYERS") == kept | {"StoneTracker", "S"}
    replays = listed("build-chip.sh", "FINE_REPLAYS")
    assert replays == {"dwaga", "mon", "none", "note", "note2", "timerc"}
    assert len(SC68_REPLAYS) == 78 and not replays & set(SC68_REPLAYS)


def test_the_amiga_players_and_sc68_replays_are_pinned_and_kept_where_the_helpers_look(
        tmp_path, monkeypatch):
    import dataclasses
    import hashlib
    import pytest
    from bbmp import downloads
    amiga, sc68 = downloads.get("amigaplayers"), downloads.get("sc68replays")
    # pinned: the UADE commit and the sc68 release the helpers are built from
    assert "8d77cd773e10d33de44f2b6c52201bb54732b701" in amiga.url and "path=players" in amiga.url
    assert "sc68/2.2.1/sc68-2.2.1.tar.gz" in sc68.url
    assert len(amiga.sha256) == 64 and len(sc68.sha256) == 64
    assert len(amiga.files) == 168 and "AbyssHighestExperience" in amiga.files
    assert "ENV/EaglePlayer/ahx.waves" in amiga.files and "Fred" not in amiga.files
    assert len(sc68.files) == 78 and "Replay/fc13_14.bin" in sc68.files
    assert "Replay/none.bin" not in sc68.files
    assert not amiga.required and not sc68.required        # some songs play without

    players = {"TFMX-Pro": b"\x00\x00\x03\xf3tfmx", "ENV/EaglePlayer/ahx.waves": b"waves"}
    replays = {"fc13_14": b"\x60\x00fc", "ssd1": b"\x60\x00ssd"}
    monkeypatch.setattr(downloads, "AMIGA_PLAYERS",
                        {n: hashlib.sha256(d).hexdigest() for n, d in players.items()})
    monkeypatch.setattr(downloads, "SC68_REPLAYS",
                        {n: hashlib.sha256(d).hexdigest() for n, d in replays.items()})
    root = "uade-8d77cd773e10d33de44f2b6c52201bb54732b701-players/players"
    uade_tar = _players_tar({**players, "Fred": b"kept with the player, not taken"}, root)
    sc68_tar = _players_tar({**{f"data/Replay/{n}.bin": d for n, d in replays.items()},
                             "data/Replay/none.bin": b"own", "data/config.txt": b"x"},
                            "sc68-2.2.1")
    wrong_tar = _players_tar({**players, "TFMX-Pro": b"another player"}, root)
    server = _site(tmp_path, {"uade.tar.gz": uade_tar, "sc68.tar.gz": sc68_tar,
                              "wrong.tar.gz": wrong_tar})
    base = f"http://127.0.0.1:{server.server_port}/"

    def fake(d, name, data):
        return dataclasses.replace(d, url=base + name, size=len(data),
                                   sha256=hashlib.sha256(data).hexdigest(),
                                   files=tuple(d.files[:0]) + (
                                       tuple(players) if d is amiga
                                       else tuple(f"Replay/{n}.bin" for n in replays)))
    asks = "UADE's Amiga player TFMX-Pro is not there"
    try:
        assert downloads.env("uade") == {} and downloads.env("sc68") == {}
        # advised to the helpers that play with them, when what they say asks for it
        for backend in ("uade", "zoundmonitor", "jesperolsen", "stonetracker"):
            assert "bbmp download amigaplayers" in downloads.advice(backend, asks)
        assert downloads.advice("uade", "UADE doesn't recognise this file") == ""
        assert "bbmp download sc68replays" in downloads.advice(
            "sc68", "This song needs sc68's replay routine fc13_14, which is not here")
        assert downloads.advice("sc68", "sc68 can't load this file") == ""

        a, s = fake(amiga, "uade.tar.gz", uade_tar), fake(sc68, "sc68.tar.gz", sc68_tar)
        downloads.install(a)
        downloads.install(s)
        there = downloads.folder(amiga)
        assert (there / "TFMX-Pro").read_bytes() == players["TFMX-Pro"]
        # ENV: beside the players, as UADE looks for it in the player's own folder
        assert (there / "ENV/EaglePlayer/ahx.waves").read_bytes() == b"waves"
        assert not (there / "Fred").exists()
        assert sorted(str(f.relative_to(there)) for f in there.rglob("*") if f.is_file()) \
            == sorted(players)
        assert (downloads.folder(sc68) / "Replay/fc13_14.bin").read_bytes() == replays["fc13_14"]
        assert not (downloads.folder(sc68) / "Replay/none.bin").exists()
        assert downloads.installed(a) and downloads.installed(s)
        monkeypatch.setattr(downloads, "DOWNLOADS", (a, s))
        assert downloads.env("uade") == {"BBMP_UADE_PLAYERS": str(there)}
        assert downloads.env("zoundmonitor") == {"BBMP_UADE_PLAYERS": str(there)}
        assert downloads.env("sc68") == {"BBMP_SC68_REPLAYS": str(downloads.folder(sc68))}
        assert downloads.advice("uade", asks) == ""

        # a player that is not the one expected is not taken
        with pytest.raises(ValueError, match="TFMX-Pro is not the file that was expected"):
            downloads.install(fake(amiga, "wrong.tar.gz", wrong_tar))
        assert downloads.installed(a)                           # what was there stays
    finally:
        server.shutdown()
        server.server_close()


def test_a_song_whose_downloaded_part_is_missing_is_refused_with_advice(tmp_path, monkeypatch):
    import pytest
    from bbmp import downloads
    from bbmp.backends import pipe
    from bbmp.backends.base import NeedsDownload, Unsupported
    script = tmp_path / "helper"
    script.write_text(
        '#!/bin/sh\n'
        'if [ -n "$BBMP_UADE_PLAYERS" ]; then\n'
        '  echo "{\\"tracks\\": 1, \\"title\\": \\"$BBMP_UADE_PLAYERS\\"}"; head -c 4000 /dev/zero\n'
        'elif [ "$1" = sc68 ]; then\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"This song needs sc68\'s replay routine ssd1, which is not here\\"}"\n'
        'elif [ "$1" = other ]; then\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"UADE can\'t play this file\\"}"\n'
        'else\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"UADE\'s Amiga player ZoundMonitor is not there\\"}"\n'
        'fi\n')
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script), backend])
    song = tmp_path / "song.sng"
    song.write_bytes(b"ZM")
    with pytest.raises(NeedsDownload) as refused:
        pipe.PipeDecoder("zoundmonitor", song)
    assert str(refused.value).startswith("UADE's Amiga player ZoundMonitor is not there. To play")
    assert "bbmp download amigaplayers" in str(refused.value)
    with pytest.raises(NeedsDownload, match="bbmp download sc68replays"):
        pipe.PipeDecoder("sc68", song)
    with pytest.raises(Unsupported) as other:              # not every refusal is advised
        pipe.PipeDecoder("other", song)
    assert not isinstance(other.value, NeedsDownload)
    there = downloads.folder(downloads.get("amigaplayers"))
    for f in downloads.get("amigaplayers").files:
        (there / f).parent.mkdir(parents=True, exist_ok=True)
        (there / f).write_bytes(b"\x00")
    d = pipe.PipeDecoder("zoundmonitor", song)
    try:
        assert d.info.title == str(there)
    finally:
        d.close()


def test_a_missing_download_is_what_is_said_when_no_replayer_plays_the_song(monkeypatch):
    """The others are still tried (a file under another format's name may be theirs);
    when none plays it, the advice is what the player says, not a list of failures."""
    from pathlib import Path
    import pytest
    from bbmp import backends
    from bbmp.backends.base import NeedsDownload, Unsupported
    tried = []

    def needs(path, lengths):
        tried.append("uade")
        raise NeedsDownload("UADE's Amiga player TFMX-Pro is not there. To play ...")

    def refuses(name):
        def opener(path, lengths):
            tried.append(name)
            raise Unsupported(f"{name} can't")
        return opener
    for name in backends.FALLBACK:
        monkeypatch.setitem(backends.OPENERS, name, refuses(name))
    monkeypatch.setitem(backends.OPENERS, "uade", needs)
    monkeypatch.setitem(backends.BACKEND_BY_FORMAT, "TFMX", "uade")
    monkeypatch.setattr("bbmp.backends.mend.open_mended", lambda *a: None)
    with pytest.raises(NeedsDownload, match="TFMX-Pro is not there"):
        backends.open_song(Path("mdat.song"), "TFMX")
    assert tried[0] == "uade" and "openmpt" in tried
    played = object()
    monkeypatch.setitem(backends.OPENERS, "openmpt", lambda p, l: played)
    assert backends.open_song(Path("mdat.song"), "TFMX") is played


def test_uade_names_the_amiga_player_that_is_not_there(tmp_path):
    import pytest
    from bbmp.backends import uade
    from bbmp.backends.base import NeedsDownload
    si = uade._SongInfo()
    uade._refuse_missing_player(si)                 # no player named: another refusal
    si.playerfname = str(tmp_path / "Fred").encode()
    (tmp_path / "Fred").write_bytes(b"")
    uade._refuse_missing_player(si)                 # it was there: another refusal
    si.playerfname = str(tmp_path / "SonicArranger").encode()
    with pytest.raises(NeedsDownload) as refused:
        uade._refuse_missing_player(si)
    assert str(refused.value).startswith("UADE's Amiga player SonicArranger is not there.")
    assert "bbmp download amigaplayers" in str(refused.value)


def test_an_sc68_song_whose_routine_is_downloaded_stays_with_sc68(tmp_path, monkeypatch):
    """Not installed, a routine sc68 is given on request is still sc68's: such a song is
    not taken for Amiga music."""
    import struct
    from bbmp.backends import sc68_amiga

    def chunk(tag, data=b""):
        return tag + struct.pack("<I", len(data)) + data
    monkeypatch.setattr(sc68_amiga, "SC68_REPLAYS", tmp_path / "Replay")
    song = tmp_path / "x.sc68"
    for name, amiga in ((b"fc13_14", False), (b"ssd1", False), (b"soundfx", True),
                        (b"JamCrackerPro", True)):        # as before: by the name as written
        song.write_bytes(b"SC68 Music-file / (c) (BeN)jamin Gerard / SasHipA-Dev  "
                         + b"SC68" + struct.pack("<I", 0) + chunk(b"SCMU")
                         + chunk(b"SCRE", name + b"\0") + chunk(b"SCDA", b"data") + chunk(b"SCEF"))
        assert (sc68_amiga.amiga_song(song) is not None) == amiga


# ---- the same loudness for every song ----

def _tone(amplitude: float, seconds: float, freq: float = 440.0) -> bytes:
    import numpy as np
    t = np.arange(int(seconds * RATE)) / RATE
    s = (np.sin(2 * np.pi * freq * t) * amplitude * 32767).astype("<i2")
    return np.stack((s, s), axis=1).tobytes()


def test_loudness_is_measured_as_the_standard_says():
    from bbmp import loudness
    # ITU-R BS.1770: a 1 kHz tone at full scale on both sides is 0 LUFS, on one -3.01
    assert abs(loudness.measure(_tone(1.0, 3, 1000)) - 0.0) < 0.1
    assert abs(loudness.measure(_tone(0.1, 3, 1000)) - -20.0) < 0.1
    assert loudness.measure(_tone(0.1, 3, 100)) < loudness.measure(_tone(0.1, 3, 1000)) - 1
    assert loudness.measure(bytes(RATE * 4 * 2)) is None                 # silence
    # quiet passages don't count: the loud part decides
    both = loudness.measure(_tone(0.01, 5) + _tone(0.5, 5))
    assert abs(both - loudness.measure(_tone(0.5, 5))) < 0.5
    assert loudness.gain_db(-10.0) == -8.0 and loudness.gain_db(-24.0) == 6.0
    assert loudness.gain_db(-60.0) == loudness.MOST_BOOST and loudness.gain_db(None) == 0.0


def test_a_song_is_not_made_to_clip(tmp_path):
    import numpy as np
    from bbmp import loudness
    # quiet on the whole, with a loud hit in it
    leveller = loudness.Leveller(known=-30.0)
    assert leveller.db == 12.0
    out = b"".join(leveller.process(chunk) for chunk in (
        _tone(0.02, 0.05), _tone(0.9, 0.05), _tone(0.02, 0.05)))
    got = np.frombuffer(out, dtype="<i2")
    assert abs(got).max() <= 32767 * loudness.CEILING + 1
    assert abs(got[:4800]).max() > 0.02 * 32767 * 3.5                    # up by 12 dB


class LoudDecoder(Decoder):
    """Ten seconds of tone at a loudness of its own."""
    backend = "fake"

    def __init__(self, amplitude):
        self.info = TrackInfo(subsongs=1, duration=10.0)
        self.sound = _tone(amplitude, 10.0)

    def read(self, frames):
        pcm, self.sound = self.sound[:frames * 4], self.sound[frames * 4:]
        return pcm


class Recorder(NullOutput):
    def __init__(self):
        super().__init__()
        self.sound = []

    def write(self, pcm):
        self.sound.append(pcm)


def test_songs_play_at_the_same_loudness(catalog, fetcher, monkeypatch, tmp_path):
    from bbmp import loudness
    levels = iter([0.05, 0.6])
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: LoudDecoder(next(levels)))
    lib = Library(tmp_path / "user.db")
    started, out = [], Recorder()
    songs = catalog.songs("Protracker", "Tester")
    # a song is told of once its first sound is written: that piece is the song's
    p = Player(catalog, fetcher, out, settings=Settings(), levels=lib,
               on_song=lambda s: started.append((s.key, len(out.sound) - 1)))
    p.play_list(songs)
    end = time.time() + 20
    while time.time() < end and (p.status().state != "stopped" or len(started) < 2):
        time.sleep(0.02)
    p.shutdown()
    assert len(started) == 2
    first = b"".join(out.sound[:started[1][1]])
    second = b"".join(out.sound[started[1][1]:])
    assert len(first) == len(second) == 10 * RATE * 4                    # nothing lost
    volume = 20 * math.log10(Settings().volume / 100)
    one, two = loudness.measure(first), loudness.measure(second)
    assert abs(one - (loudness.TARGET + volume)) < 1.0                   # 22 dB apart as they are
    assert abs(two - (loudness.TARGET + volume)) < 1.0
    # what was measured is kept, for the next time
    assert abs(lib.loudness(songs[0].key, 0) - loudness.measure(_tone(0.05, 10))) < 0.2
    assert abs(lib.loudness(songs[1].key, 0) - loudness.measure(_tone(0.6, 10))) < 0.2


def test_loudness_can_be_left_alone(catalog, fetcher, monkeypatch, tmp_path):
    from bbmp import loudness
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: LoudDecoder(0.02))
    out = Recorder()
    p = Player(catalog, fetcher, out, settings=Settings(level=False, volume=100),
               levels=Library(tmp_path / "user.db"))
    p.play_list(catalog.songs("Protracker", "Tester")[:1])
    end = time.time() + 10
    while time.time() < end and (p.status().state != "stopped" or not out.sound):
        time.sleep(0.02)
    p.shutdown()
    assert b"".join(out.sound) == _tone(0.02, 10.0)


def test_the_c64s_roms_are_given_to_the_sid_player(tmp_path):
    from bbmp import extras
    from bbmp.backends import sid
    given = []

    class Lib:
        def sid_roms(self, *roms):
            given.append(tuple(len(r) if r else None for r in roms))
    real, sid._lib, sid._roms = sid._lib, Lib(), None
    try:
        sid.use_roms()
        assert given == [(None, None, None)]
        extras.folder().mkdir(parents=True)
        (extras.folder() / "kernal-901227-03.bin").write_bytes(b"k" * 8192)   # as VICE names it
        (extras.folder() / "basic.bin").write_bytes(b"b" * 8192)
        (extras.folder() / "chargen.bin").write_bytes(b"c" * 100)              # not the ROM
        sid.use_roms()
        assert given[-1] == (8192, 8192, None)
        sid.use_roms()
        assert len(given) == 2                          # given once, until something changes
    finally:
        sid._lib, sid._roms = real, None
    assert [e.id for e in extras.for_format("HVSC - Musicians")] == [
        "c64kernal", "c64basic", "c64chargen"]


def test_exports_are_as_loud_as_the_player_plays(catalog, fetcher, tmp_path):
    import wave as wave_mod

    import numpy as np

    from bbmp import loudness
    from bbmp.export import export_song
    song = catalog.by_path("Protracker/Tester/beep.mod")

    def lufs(file):
        with wave_mod.open(str(file)) as w:
            return loudness.measure(w.readframes(w.getnframes()))
    (plain,) = export_song(catalog, fetcher, song, Settings(level=False), tmp_path / "plain", "wav")
    (level,) = export_song(catalog, fetcher, song, Settings(level=True), tmp_path / "level", "wav")
    assert abs(lufs(plain) - loudness.TARGET) > 1                   # the test song is off
    assert abs(lufs(level) - loudness.TARGET) < 1.0
    with wave_mod.open(str(level)) as w:
        peak = np.abs(np.frombuffer(w.readframes(w.getnframes()), "<i2")).max()
    assert peak <= loudness.CEILING * 32767 + 1                     # and never clips


def test_several_songs_go_into_a_folder_of_their_own(catalog, fetcher, tmp_path):
    from bbmp.export import export_songs
    songs = [catalog.by_path("Protracker/Tester/beep.mod"),
             catalog.by_path("Renoise Old/Someone/opening.rns"),         # can't be played
             catalog.by_path("Protracker/Tester/beep two.mod")]
    said = []
    folder, files, failed = export_songs(catalog, fetcher, songs, Settings(), "My/List",
                                         tmp_path, "wav", lambda *a: said.append(a[:2]))
    assert folder == tmp_path / "My_List"
    assert [f.name for f in files] == ["01 - Tester - beep.wav", "03 - Tester - beep two.wav"]
    assert [s.title for s, _ in failed] == ["opening"]
    assert said[0] == (0, 3) and said[-1] == (3, 3)


def test_repeating_a_song_plays_it_whole_every_time(catalog, fetcher, monkeypatch, tmp_path):
    # its loudness is known by the second time, and what was read ahead for measuring
    # it must still be played, or the song starts 15 seconds in
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: LoudDecoder(0.3))
    started, out = [], Recorder()
    p = Player(catalog, fetcher, out, settings=Settings(repeat="one"),
               levels=Library(tmp_path / "user.db"),
               on_song=lambda s: started.append(len(out.sound)))
    p.play_list(catalog.songs("Protracker", "Tester")[:1])
    end = time.time() + 20
    while time.time() < end and len(started) < 4:
        time.sleep(0.02)
    p.shutdown()
    lengths = [len(b"".join(out.sound[a:b])) for a, b in zip(started, started[1:])]
    assert lengths[:2] == [10 * RATE * 4] * 2


class Breaks(LoudDecoder):
    """Plays a moment, then goes wrong the way a replayer's garbage would."""
    def __init__(self):
        super().__init__(0.3)
        self.reads = 0

    def read(self, frames):
        self.reads += 1
        if self.reads > 3:
            raise ValueError("the replayer said something that isn't JSON")
        return super().read(frames)

    def seek(self, seconds):
        raise Unsupported("the helper didn't start again")


def test_a_song_that_goes_wrong_is_passed_over(catalog, fetcher, monkeypatch):
    made = []
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: made.append(
                            Breaks() if not made else LoudDecoder(0.3)) or made[-1])
    started = []
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(level=False),
               on_song=lambda s: started.append(s.title))
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 5
    while time.time() < end and len(started) < 2:
        time.sleep(0.02)
    assert started == ["beep", "beep two"]
    p.toggle_pause()                            # the player still hears its keys
    end = time.time() + 2
    while time.time() < end and p.status().state != "paused":
        time.sleep(0.02)
    assert p.status().state == "paused"
    p.shutdown()


def test_a_seek_the_replayer_refuses_ends_the_song_not_the_player(catalog, fetcher, monkeypatch):
    class NoSeek(LoudDecoder):
        def seek(self, seconds):
            raise Unsupported("the helper didn't start again")
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: NoSeek(0.3))
    started = []
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(level=False),
               on_song=lambda s: started.append(s.title))
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 3
    while time.time() < end and not started:
        time.sleep(0.02)
    p.seek(5)
    end = time.time() + 3
    while time.time() < end and len(started) < 2:
        time.sleep(0.02)
    p.shutdown()
    assert started == ["beep", "beep two"]


def test_a_restored_song_starts_where_it_was(catalog, fetcher, monkeypatch, tmp_path):
    class Seeks(LoudDecoder):
        def seek(self, seconds):
            self.sound = self.sound[int(seconds * RATE) * 4:]
            return True
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: Seeks(0.3))
    p = Player(catalog, fetcher, NullOutput(), settings=Settings(),
               levels=Library(tmp_path / "user.db"))
    p.restore(catalog.songs("Protracker", "Tester")[:1], 0, 0, 6.0)
    end = time.time() + 3
    while time.time() < end and p.status().state != "paused":
        time.sleep(0.02)
    p.shutdown()
    assert p._ahead == b""          # not the song's first seconds, played as if at 0:06


def test_shuffle_plays_every_song_once_and_ends(catalog, fetcher, monkeypatch):
    songs = catalog.songs("Protracker", "Tester") + catalog.songs("Protracker", "Other Guy")
    _, started, state = run_player(catalog, fetcher, monkeypatch, Settings(shuffle=True),
                                   lambda: FakeDecoder(subsongs=1, seconds=0.1), songs=songs)
    assert sorted(started) == sorted(s.title for s in songs) and state == "stopped"


def test_shuffle_goes_back_to_the_song_before(catalog, fetcher, monkeypatch):
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: FakeDecoder(subsongs=1, seconds=30))
    songs = catalog.songs("Protracker", "Tester") + catalog.songs("Protracker", "Other Guy")
    started = []
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(shuffle=True),
               on_song=lambda s: started.append(s.key))

    def wait(n):
        end = time.time() + 3
        while time.time() < end and len(started) < n:
            time.sleep(0.02)
    p.play_list(songs)
    wait(1)
    p.next()
    wait(2)
    p.next()
    wait(3)
    p.prev()
    wait(4)
    p.shutdown()
    assert started[3] == started[1]         # the one before, not the one above it in the queue


def test_next_pressed_many_times_opens_only_the_last(catalog, fetcher, monkeypatch):
    opened = []

    def slow_open(path, fmt, lengths=(), local=False):
        time.sleep(0.2)
        opened.append(path.name)
        return FakeDecoder(subsongs=1, seconds=30)
    monkeypatch.setattr("bbmp.player.open_song", slow_open)
    songs = (catalog.songs("Protracker", "Tester") + catalog.songs("Protracker", "Other Guy")) * 4
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(level=False))
    p.play_list(songs)
    end = time.time() + 3
    while time.time() < end and p.status().state != "playing":
        time.sleep(0.02)
    for _ in range(7):
        p.next()
    end = time.time() + 5
    while time.time() < end and p.status().queue_pos != 7:
        time.sleep(0.02)
    p.shutdown()
    assert p.status().queue_pos == 7
    assert len(opened) <= 4          # the first, one prepared ahead, the last and its next


def test_an_export_stops_within_a_song_and_leaves_no_half_file(catalog, fetcher, tmp_path,
                                                                monkeypatch):
    import threading
    from bbmp.export import Stopped, export_song

    stop = threading.Event()

    class Long(FakeDecoder):
        def read(self, frames):
            if self.reads_left() < 0:
                stop.set()                  # the user presses Stop meanwhile
            return super().read(frames)

        def reads_left(self):
            self.n = getattr(self, "n", 50) - 1
            return self.n
    monkeypatch.setattr("bbmp.export.open_song",
                        lambda path, fmt, lengths=(), local=False: Long(subsongs=1, seconds=600))
    song = catalog.songs("Protracker", "Tester")[0]
    with pytest.raises(Stopped):
        export_song(catalog, fetcher, song, Settings(level=False), tmp_path / "out", "wav",
                    stop=stop)
    assert list((tmp_path / "out").iterdir()) == []


def test_a_replayer_error_during_an_export_leaves_no_half_file(catalog, fetcher, tmp_path,
                                                               monkeypatch):
    from bbmp.export import ExportError, export_song

    class Breaks(FakeDecoder):
        def read(self, frames):
            self.n = getattr(self, "n", 0) + 1
            if self.n > 20:
                raise OSError("the replayer's process ended")
            return super().read(frames)
    monkeypatch.setattr("bbmp.export.open_song",
                        lambda path, fmt, lengths=(), local=False: Breaks(subsongs=1, seconds=600))
    song = catalog.songs("Protracker", "Tester")[0]
    with pytest.raises(ExportError):
        export_song(catalog, fetcher, song, Settings(level=False), tmp_path / "out", "wav")
    assert list((tmp_path / "out").iterdir()) == []


def test_the_pattern_and_channels_go_with_the_sound_read_ahead(catalog, fetcher, monkeypatch,
                                                               tmp_path):
    # the song's first 15 seconds are read ahead to measure its loudness: the pattern
    # view stood still on the row the decoder had reached, then ran 15 s ahead
    class Counts(LoudDecoder):
        def __init__(self):
            super().__init__(0.3)
            self.frames = 0

        def read(self, frames):
            pcm = super().read(frames)
            self.frames += len(pcm) // 4
            return pcm

        def pattern_view(self, rows):
            return 0, [f"row at {self.frames // RATE} s"]

        def channel_levels(self):
            return [self.frames / RATE / 10]
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: Counts())
    seen = []
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(),
               levels=Library(tmp_path / "user.db"))
    p.want_pattern(True)
    p.play_list(catalog.songs("Protracker", "Tester")[:1])
    end = time.time() + 3
    while time.time() < end and p.status().state != "playing":
        time.sleep(0.01)
    end = time.time() + 3.5
    while time.time() < end:
        st = p.status()
        if st.pattern and st.state == "playing":
            seen.append((st.elapsed, st.pattern[1][0]))
        time.sleep(0.005)
    p.shutdown()
    assert len(seen) > 20
    for elapsed, row in seen:
        shown = int(row.split()[2])
        assert abs(shown - elapsed) <= 1.5, (elapsed, row)


def test_every_replayer_says_what_it_is():
    from bbmp import replayer_facts
    from bbmp.backends import BACKEND_BY_FORMAT, OPENERS
    known = replayer_facts.facts()
    assert set(BACKEND_BY_FORMAT.values()) | set(OPENERS) <= set(known)
    assert replayer_facts.line("openmpt").startswith("libopenmpt ")
    assert "BSD" in replayer_facts.line("openmpt")
    assert replayer_facts.line("fac").endswith("(written for this player)")
    assert ("this player's own",
            "written for this player in a clean room, from a description of the format "
            "only; the same chip writes as before in all 644 songs") in \
        replayer_facts.details("fac")


# ---- how a song ends: one rule (ending.py) ----

def test_silence_after_a_song_is_held_back_and_let_go_at_its_end():
    from bbmp import ending
    s = ending.Silence(RATE, 4)
    loud, quiet = b"\x10\x10" * 200, b"\x00\x00" * 200
    assert s.take(quiet, True) == []                 # held
    assert s.take(loud, False) == [quiet, loud]      # and played when the song sounds again
    assert s.take(quiet, True) == []
    s.clear()                                        # the song ended: let go
    assert s.take(loud, False) == [loud]
    long_quiet = b"\x00" * int(ending.SILENT_END * RATE) * 4
    assert s.take(long_quiet, True) is None          # silence this long: the song is over


def test_which_songs_the_player_stops():
    from bbmp import ending
    assert ending.limit("sidplayfp", 95.0) == 95.0          # never ends: its known length
    assert ending.limit("zxtune", None) == ending.DEFAULT_LENGTH
    assert ending.limit("openmpt", None) == ending.LONGEST  # ends by itself: not cut short
    assert ending.limit("mdx", 100.0) == 180.0              # only well past where it said
    # a song its decoder knows never ends, though its replayer's songs do (DeltaMusic 2)
    assert ending.limit("uade", None, never_ends=True) == ending.DEFAULT_LENGTH
    assert ending.fade(10, 100) == 1.0 and ending.fade(98, 100) == 0.5


def test_a_song_that_ends_in_silence_ends_where_its_sound_does(catalog, fetcher, monkeypatch):
    class Tail(FakeDecoder):
        """Two seconds of tone, then eight of silence, as UADE gives a song's end."""
        backend = "uade"

        def __init__(self):
            super().__init__(subsongs=1, seconds=10.0)
            self.info.duration = None
            self.given = 0

        def read(self, frames):
            n = min(frames, self.left)
            self.left -= n
            value = 8000 if self.given < 2 * RATE else 0
            self.given += n
            return (value.to_bytes(2, "little", signed=True) * 2) * n
    monkeypatch.setattr("bbmp.player.open_song", lambda path, fmt, lengths=(), local=False: Tail())
    out = Recorder()
    p = Player(catalog, fetcher, out, settings=Settings(level=False))
    p.play_list(catalog.songs("Protracker", "Tester")[:1])
    end = time.time() + 5
    while time.time() < end and (p.status().state != "stopped" or not out.sound):
        time.sleep(0.02)
    p.shutdown()
    played = len(b"".join(out.sound)) // 4
    assert abs(played - 2 * RATE) <= 4800            # the tone, not the silence after it


def test_a_song_that_ends_by_itself_is_not_cut_at_three_minutes(catalog, fetcher, monkeypatch):
    class Long(FakeDecoder):
        backend = "adplug"                          # it was stopped at 180 s, 4 s faded

        def __init__(self):
            super().__init__(subsongs=1, seconds=200.0)
            self.info.duration = None
    monkeypatch.setattr("bbmp.player.open_song", lambda path, fmt, lengths=(), local=False: Long())
    out = Recorder()
    p = Player(catalog, fetcher, out, settings=Settings(level=False))
    p.play_list(catalog.songs("Protracker", "Tester")[:1])
    end = time.time() + 20
    while time.time() < end and (p.status().state != "stopped" or not out.sound):
        time.sleep(0.05)
    p.shutdown()
    assert len(b"".join(out.sound)) // 4 == 200 * RATE


def test_on_and_on_keeps_the_song_going_and_only_that_song(catalog, fetcher, monkeypatch):
    # a SID tune is stopped at its length; "o" lets it go on, and a song that ends by
    # itself starts again; the next song ends as songs do
    class Short(FakeDecoder):
        backend = "sidplayfp"

        def __init__(self):
            super().__init__(subsongs=1, seconds=100.0)
            self.info.duration = 0.4               # the player would stop it here
            self.forever = None

        def loop_forever(self, on):
            self.forever = on
            return False
    made = []
    monkeypatch.setattr("bbmp.player.open_song",
                        lambda path, fmt, lengths=(), local=False: made.append(Short()) or made[-1])
    started = []
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(level=False),
               on_song=lambda s: started.append(s.title))
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 3
    while time.time() < end and not started:
        time.sleep(0.01)
    p.forever()
    end = time.time() + 3
    while time.time() < end and not p.status().forever:
        time.sleep(0.01)
    assert p.status().forever and made[0].forever is True
    time.sleep(1.2)                                 # three times its length
    assert started == ["beep"] and p.status().state == "playing"
    p.next()
    end = time.time() + 3
    while time.time() < end and len(started) < 2:
        time.sleep(0.01)
    time.sleep(0.2)
    assert started[1] == "beep two" and not p.status().forever
    p.shutdown()


# ---- which visualisers a song has ----

class _Tracker(FakeDecoder):
    """A replayer that gives channel levels and a pattern, as the tracker ones do."""
    def channel_levels(self):
        return [0.5, 0.25]

    def pattern_view(self, rows):
        return (0, ["C-4 01 ...."] * rows)


def _shows_while_playing(catalog, fetcher, monkeypatch, decoder):
    monkeypatch.setattr("bbmp.player.open_song", lambda *a, **k: decoder())
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(level=False))
    p.play_list(catalog.songs("Protracker", "Tester"))
    seen = set()
    end = time.time() + 3
    while time.time() < end and p.status().state != "playing":
        time.sleep(0.02)
    while time.time() < end and p.status().state == "playing":
        seen |= p.status().shows
        time.sleep(0.02)
    p.shutdown()
    return seen


from bbmp import visuals  # noqa: E402


def test_a_song_shows_only_the_visualisers_it_has_data_for(catalog, fetcher, monkeypatch):
    plain = _shows_while_playing(catalog, fetcher, monkeypatch, lambda: FakeDecoder(subsongs=1))
    sound = set(visuals.FROM_SOUND)
    assert plain == sound
    full = _shows_while_playing(catalog, fetcher, monkeypatch, lambda: _Tracker(subsongs=1))
    assert full == sound | {"channels", "scopes", "pattern"}


def test_the_visualiser_skips_what_the_song_lacks_and_keeps_the_choice():
    from bbmp.tui.widgets import Visualizer
    vis = Visualizer()
    vis.set_mode("pattern")
    vis.available = frozenset(visuals.FROM_SOUND)             # a song without a pattern
    assert vis.shown(elapsed=0.1) == "pattern"                 # its data may still come
    assert vis.shown(elapsed=2.0) == "spectrum"                # it doesn't: the spectrum
    assert vis.mode == "pattern"                               # the choice is kept
    shown = [m for m in visuals.FROM_SOUND if m != "demo"]    # demo only when Settings says
    order = [vis.cycle() for _ in range(len(shown) + 1)]
    assert order == [*shown, None]                             # channels, scopes, pattern skipped
    vis.demo = True
    assert "demo" in [vis.cycle() for _ in range(len(visuals.FROM_SOUND) + 1)]
    vis.demo = False
    assert not vis.wants_pattern
    vis.available = frozenset(visuals.MODES)
    vis.set_mode("pattern")
    assert vis.shown(elapsed=2.0) == "pattern"                 # a song that has it: shown again


def test_paths_are_shown_with_the_home_folder_as_a_tilde():
    from pathlib import Path
    from bbmp import paths
    home = Path.home()
    assert paths.shown(home / ".local/share/bbmp/extras") == "~/.local/share/bbmp/extras"
    assert paths.shown("/usr/share/bbmp") == "/usr/share/bbmp"
    assert paths.shown(home) == str(home)


def test_organyas_sound_bank_is_downloaded_whole_and_named_to_the_helper(tmp_path, monkeypatch):
    import dataclasses
    import hashlib
    import pytest
    from bbmp import downloads
    from bbmp.backends import pipe
    from bbmp.backends.base import NeedsDownload
    real = downloads.get("organyabank")
    # pinned: organya.h's commit and the bank's SHA-256
    assert "f7a086fc00f21de5e1c410d38d7412627b8d2cf0" in real.url and len(real.sha256) == 64
    assert real.files == ("default.wdb",) and real.backend == "organya"
    assert set(real.formats) == {"Organya", "Organya 2"} and not real.required

    bank = bytes(range(256)) * 200
    server = _site(tmp_path, {"default.wdb": bank})
    fake = dataclasses.replace(real, url=f"http://127.0.0.1:{server.server_port}/default.wdb",
                               size=len(bank), sha256=hashlib.sha256(bank).hexdigest())
    script = tmp_path / "chip-helper"
    script.write_text(
        '#!/bin/sh\n'
        'if [ -n "$BBMP_ORGANYA_BANK" ]; then\n'
        '  echo "{\\"tracks\\": 1, \\"title\\": \\"$BBMP_ORGANYA_BANK\\"}"; head -c 4000 /dev/zero\n'
        'else\n'
        '  echo "{\\"tracks\\": 0, \\"error\\": \\"Organya soundbank missing: /app/opt/bbmp-chip/share/default.wdb\\"}"\n'
        'fi\n')
    script.chmod(0o755)
    monkeypatch.setattr(pipe, "helper", lambda backend: [str(script)])
    song = tmp_path / "song.org"
    song.write_bytes(b"Org-02")
    try:
        assert downloads.env("organya") == {}
        with pytest.raises(NeedsDownload) as refused:
            pipe.PipeDecoder("organya", song)
        assert "bbmp download organyabank" in str(refused.value)
        assert downloads.advice("organya", "not an Organya song") == ""

        downloads.install(fake)
        there = downloads.folder(real) / "default.wdb"
        assert there.read_bytes() == bank and downloads.installed(real)
        assert downloads.env("organya") == {"BBMP_ORGANYA_BANK": str(there)}
        assert downloads.env("pxtone") == {}
        d = pipe.PipeDecoder("organya", song)
        try:
            assert d.info.title == str(there)
            assert there in d._reads()                   # the sandbox lets it read it
        finally:
            d.close()
    finally:
        server.shutdown()
        server.server_close()
