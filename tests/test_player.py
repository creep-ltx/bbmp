# SPDX-License-Identifier: GPL-3.0-or-later
import time

from bbmp.audio import NullOutput
from bbmp.index import Song
from bbmp.player import Player


def wait_for(cond, timeout=5.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.02)
    return False


def make_player(catalog, fetcher, realtime=True):
    played = []
    p = Player(catalog, fetcher, NullOutput(realtime=realtime), on_song=lambda s: played.append(s.path))
    return p, played


def test_play_pause_next_stop(catalog, fetcher):
    p, played = make_player(catalog, fetcher)
    try:
        songs = catalog.songs("Protracker", "Tester")
        p.play_list(songs)
        assert wait_for(lambda: p.status().state == "playing")
        st = p.status()
        assert st.song.path == songs[0].path and st.backend == "openmpt"
        assert wait_for(lambda: p.status().elapsed > 0.2)
        # the next song was prefetched in the background
        assert wait_for(lambda: len(fetcher.downloads) == 2)
        p.toggle_pause()
        assert wait_for(lambda: p.status().state == "paused")
        p.toggle_pause()
        assert wait_for(lambda: p.status().state == "playing")
        p.next()
        assert wait_for(lambda: p.status().song.path == songs[1].path)
        p.stop()
        assert wait_for(lambda: p.status().state == "stopped")
        assert played == [songs[0].path, songs[1].path]
    finally:
        p.shutdown()


def test_songs_play_to_the_end_and_advance(catalog, fetcher):
    p, played = make_player(catalog, fetcher, realtime=False)
    try:
        songs = catalog.songs("Protracker", "Tester")
        p.play_list(songs)
        assert wait_for(lambda: len(played) == 2 and p.status().state == "stopped")
    finally:
        p.shutdown()


def test_failed_songs_are_skipped(catalog, fetcher):
    p, played = make_player(catalog, fetcher)
    try:
        missing = Song(0, "Protracker/Nobody/missing.mod", "Protracker", "Nobody", "", "missing.mod")
        good = catalog.by_path("Protracker/Tester/beep.mod")
        p.play_list([missing, good])
        assert wait_for(lambda: p.status().song == good and p.status().state == "playing")
        # (a song is told of as its first sound goes out)
        assert wait_for(lambda: played == [good.path])
    finally:
        p.shutdown()


def test_radio_picks_from_scope(catalog, fetcher):
    p, played = make_player(catalog, fetcher)
    try:
        p.radio("Protracker", "Tester")
        assert wait_for(lambda: p.status().state == "playing")
        assert p.status().radio == "Protracker / Tester"
        for _ in range(3):
            p.next()
            time.sleep(0.2)
        assert wait_for(lambda: len(played) >= 4)
        assert all(x.startswith("Protracker/Tester/") for x in played)
        p.radio_off()
        assert wait_for(lambda: p.status().radio == "")
    finally:
        p.shutdown()


def test_volume_and_levels(catalog, fetcher):
    p, _ = make_player(catalog, fetcher)
    try:
        p.play_list(catalog.songs("Protracker", "Tester"))
        assert wait_for(lambda: max(p.status().levels) > 0.05)
        loud = max(p.status().levels)
        p.set_volume(10)
        assert wait_for(lambda: 0 < max(p.status().levels) < loud / 3)
        p.set_volume(250)
        assert p.status().volume == 100
    finally:
        p.shutdown()
