# SPDX-License-Identifier: GPL-3.0-or-later
"""Music kept on this computer: what is fetched, what is played from where, and what
happens when the collection changes."""

from __future__ import annotations

import hashlib
from pathlib import Path
import shutil
from collections import namedtuple

import pytest

from conftest import SONGS, LocalFetcher, hvsc, modland
from bbmp import index as index_mod
from bbmp.keep import Keeper, Scope


def listing(songs, contents):
    """Modland's lists for these songs, with the checksums and sizes their files have."""
    raw = "\n".join(f"{hashlib.md5(contents[p]).hexdigest()} {p}" for p in songs).encode()
    sizes = "\n".join(f"{len(contents[p])}\t{p}" for p in songs).encode()
    return raw, sizes


@pytest.fixture
def server(mod_bytes):
    """Every file of the test catalog, each with a content of its own."""
    return {p: mod_bytes + p.encode() for p in SONGS}


def catalog_of(tmp_path, songs, contents, with_hvsc=False):
    raw, sizes = listing(songs, contents)
    lists = {"modland": modland.read_list(raw, b"", sizes, [], 0)}
    if with_hvsc:
        lists["hvsc"] = hvsc.read_list(raw, sizes, [], 0)
    index_mod.build(lists, tmp_path / "index.db")
    return index_mod.Index(tmp_path / "index.db")


@pytest.fixture
def keeper(tmp_path, server):
    index = catalog_of(tmp_path, SONGS, server)
    fetcher = LocalFetcher(tmp_path / "cache", server)
    fetcher.checksum = index.md5
    return Keeper(index, fetcher)


def test_an_artist_is_kept_and_played_from_the_disk(keeper, tmp_path):
    scope = Scope("modland", "Protracker", "Tester")
    assert scope.name == "Tester in Protracker on Modland"
    assert keeper.scopes() == [] and not (tmp_path / "kept").exists()   # nothing until asked
    keeper.add(scope)
    plan = keeper.plan()
    assert [f.path for f in plan.fetch] == ["Protracker/Tester/List/a list.m3u",
                                            "Protracker/Tester/beep two.mod",
                                            "Protracker/Tester/beep.mod"]
    said = []
    result = keeper.run(plan, said.append)
    assert (result.done, result.failed, result.stopped) == (3, [], "")
    assert said and said[-1].done == 3
    assert (tmp_path / "kept/Modland/Protracker/Tester/beep.mod").is_file()
    assert keeper.plan().done
    (state,) = keeper.states()
    assert state.complete and state.files == 3 and "all there" in state.line

    # the player takes the song from there, and asks no server
    fetcher = keeper.fetcher
    fetcher.downloads.clear()
    file = fetcher.get("modland:Protracker/Tester/beep.mod")
    assert file == tmp_path / "kept/Modland/Protracker/Tester/beep.mod"
    assert fetcher.downloads == [] and fetcher.is_kept("Protracker/Tester/beep.mod")
    assert not (tmp_path / "cache/modland/Protracker/Tester/beep.mod").exists()
    # the cache's limit is not the kept music's business
    fetcher.trim(1)
    assert file.is_file()


def test_a_format_comes_with_the_files_its_songs_need(keeper, tmp_path):
    keeper.add(Scope("modland", "TFMX"))
    keeper.run()
    kept = tmp_path / "kept/Modland/TFMX/Tester"
    assert sorted(f.name for f in kept.iterdir()) == ["mdat.level1", "mdat.level2",
                                                      "smpl.level1", "smpl.level2"]
    assert keeper.kept(Scope("modland", "TFMX", "Tester")) == Scope("modland", "TFMX")
    assert keeper.kept(Scope("modland", "Protracker", "Tester")) is None


def test_an_artist_comes_with_the_samples_the_format_shares(keeper, tmp_path):
    keeper.add(Scope("modland", "Zoundmonitor", "Someone"))
    keeper.run()
    assert (tmp_path / "kept/Modland/Zoundmonitor/Someone/tune.sng").is_file()
    assert (tmp_path / "kept/Modland/Zoundmonitor/Samples/perco").is_file()
    assert (tmp_path / "kept/Modland/Zoundmonitor/Samples/tom4").is_file()


def test_a_collection_that_is_hosted_is_kept_as_its_hosts_files(tmp_path, server):
    index = catalog_of(tmp_path, SONGS, server, with_hvsc=True)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    keeper.add(Scope("hvsc"))
    assert Scope("hvsc").name == "all of HVSC"
    keeper.run()
    assert (tmp_path / "kept/Modland/HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid").is_file()
    assert (tmp_path / "kept/Modland/HVSC/GAMES/A-F/Bionic.sid").is_file()
    assert keeper.size_of(Scope("hvsc"))[0] == 2
    # browsing Modland, the same tune is played from there too
    assert keeper.fetcher.is_kept("modland:HVSC/GAMES/A-F/Bionic.sid")


def test_what_the_cache_has_is_not_fetched_again(keeper, tmp_path):
    fetcher = keeper.fetcher
    cached = fetcher.get("modland:Protracker/Tester/beep.mod")
    assert cached == tmp_path / "cache/modland/Protracker/Tester/beep.mod"
    fetcher.downloads.clear()
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    assert sorted(fetcher.downloads) == ["Protracker/Tester/List/a list.m3u",
                                         "Protracker/Tester/beep two.mod"]
    kept = tmp_path / "kept/Modland/Protracker/Tester/beep.mod"
    assert kept.read_bytes() == cached.read_bytes()


def test_the_files_a_song_needs_are_put_where_the_song_is(keeper, tmp_path):
    """A replayer looks for them next to the song."""
    fetcher = keeper.fetcher
    keeper.add(Scope("modland", "Zoundmonitor", "Someone"))
    keeper.run()
    # a song in the cache whose samples are kept: they are had in the cache too
    shutil.rmtree(tmp_path / "kept/Modland/Zoundmonitor/Someone")
    fetcher.downloads.clear()
    key = "modland:Zoundmonitor/Someone/tune.sng"
    file = fetcher.get(key, keeper.index.companions(key))
    assert file == tmp_path / "cache/modland/Zoundmonitor/Someone/tune.sng"
    assert (tmp_path / "cache/modland/Zoundmonitor/Samples/perco").is_file()
    assert fetcher.downloads == ["Zoundmonitor/Someone/tune.sng"]
    # a kept song whose file is missing there: it is put there
    keeper.add(Scope("modland", "TFMX"))
    keeper.run()
    (tmp_path / "kept/Modland/TFMX/Tester/smpl.level1").unlink()
    key = "modland:TFMX/Tester/mdat.level1"
    file = fetcher.get(key, keeper.index.companions(key))
    assert file == tmp_path / "kept/Modland/TFMX/Tester/mdat.level1"
    assert (tmp_path / "kept/Modland/TFMX/Tester/smpl.level1").is_file()


def test_when_the_collection_changes(tmp_path, server):
    songs = [p for p in SONGS if p.startswith("Protracker/")]
    index = catalog_of(tmp_path, songs, server)
    fetcher = LocalFetcher(tmp_path / "cache", server)
    keeper = Keeper(index, fetcher)
    keeper.add(Scope("modland", "Protracker"))
    keeper.run()
    assert keeper.plan().done and keeper.plan().files == 4

    # the collection adds a song, changes one, moves one and takes one away
    now = dict(server)
    now["Protracker/Tester/new.mod"] = b"a new song"
    now["Protracker/Tester/beep.mod"] = server["Protracker/Tester/beep.mod"] + b" changed"
    now["Protracker/Tester Himself/beep two.mod"] = server["Protracker/Tester/beep two.mod"]
    songs = [p for p in songs if p not in ("Protracker/Tester/beep two.mod",
                                           "Protracker/Tester/List/a list.m3u")]
    songs += ["Protracker/Tester/new.mod", "Protracker/Tester Himself/beep two.mod"]
    index = catalog_of(tmp_path, songs, now)
    fetcher = LocalFetcher(tmp_path / "cache", now)
    keeper = Keeper(index, fetcher)
    plan = keeper.plan()
    assert [f.path for f in plan.fetch] == ["Protracker/Tester/beep.mod",
                                            "Protracker/Tester/new.mod"]
    assert [(a.path, b.path) for a, b in plan.move] == [
        ("Protracker/Tester/beep two.mod", "Protracker/Tester Himself/beep two.mod")]
    # what it took away stays here, marked: kept music is never taken away for that
    assert plan.remove == []
    assert [f.path for f in plan.stale] == ["Protracker/Tester/List/a list.m3u"]
    (state,) = keeper.states()
    assert not state.complete and "3 files not there yet" in state.line   # one of them only moved

    result = keeper.run(plan)
    assert (result.done, result.moved, result.removed, result.stale) == (2, 1, 0, 1)
    assert sorted(fetcher.downloads) == ["Protracker/Tester/beep.mod", "Protracker/Tester/new.mod"]
    kept = tmp_path / "kept/Modland/Protracker"
    assert (kept / "Tester/beep.mod").read_bytes().endswith(b" changed")
    assert (kept / "Tester Himself/beep two.mod").is_file()
    assert not (kept / "Tester/beep two.mod").exists()
    assert (kept / "Tester/List/a list.m3u").is_file()
    assert keeper.plan().done


def test_a_catalog_that_lost_its_songs_takes_no_kept_music_away(tmp_path, server):
    """A broken list emptied a collection's catalog, and the music kept of it was then
    taken away (7 October 2026). What the catalog no longer has stays."""
    index = catalog_of(tmp_path, SONGS, server)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    keeper.add(Scope("modland", "Protracker"))
    keeper.run()
    kept = sorted(p for p in (tmp_path / "kept/Modland/Protracker").rglob("*") if p.is_file())
    assert kept
    index.close()
    index = catalog_of(tmp_path, [p for p in SONGS if not p.startswith("Protracker/")], server)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    plan = keeper.plan()
    assert plan.remove == [] and len(plan.stale) == len(kept)
    result = keeper.run(plan)
    assert result.removed == 0
    assert sorted(p for p in (tmp_path / "kept/Modland/Protracker").rglob("*")
                  if p.is_file()) == kept


def test_it_is_not_begun_without_room_on_the_disk(keeper, monkeypatch):
    usage = namedtuple("usage", "total used free")
    monkeypatch.setattr(shutil, "disk_usage", lambda path: usage(10**12, 10**12 - 1000, 1000))
    keeper.add(Scope("modland", "Protracker"))
    result = keeper.run()
    assert "not room" in result.stopped and result.done == 0
    assert keeper.fetcher.downloads == [] and keeper.have() == {}


def test_a_file_no_server_has_is_told_and_the_rest_is_fetched(keeper):
    del keeper.fetcher.server["Protracker/Tester/beep.mod"]
    keeper.add(Scope("modland", "Protracker", "Tester"))
    result = keeper.run()
    assert result.done == 2 and result.failed == [("Protracker/Tester/beep.mod", "no server has it")]
    assert [f.path for f in keeper.plan().fetch] == ["Protracker/Tester/beep.mod"]


def test_to_stop_keeping_takes_away_what_nothing_else_needs(keeper, tmp_path):
    keeper.add(Scope("modland", "Protracker"))
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    kept = tmp_path / "kept/Modland/Protracker"
    assert keeper.drop(Scope("modland", "Protracker")) > 0
    assert (kept / "Tester/beep.mod").is_file() and not (kept / "Other Guy").exists()
    assert keeper.scopes() == [Scope("modland", "Protracker", "Tester")]
    keeper.drop(Scope("modland", "Protracker", "Tester"))
    assert not (tmp_path / "kept/Modland").exists() and keeper.size() == 0


def test_files_that_went_missing_are_fetched_again(keeper, tmp_path):
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    (tmp_path / "kept/Modland/Protracker/Tester/beep.mod").unlink()
    assert keeper.plan().done                  # the database doesn't know yet
    assert keeper.check() == 1
    assert [f.path for f in keeper.plan().fetch] == ["Protracker/Tester/beep.mod"]


@pytest.mark.parametrize("same_disk", [True, False])
def test_the_kept_music_moves_to_another_folder(keeper, tmp_path, monkeypatch, same_disk):
    from bbmp import keep

    monkeypatch.setattr(keep, "_same_disk", lambda a, b: same_disk)
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    music = tmp_path / "Music/Our Player"
    (tmp_path / "Music").mkdir()
    (tmp_path / "Music/my own song.mod").write_bytes(b"mine")
    fetcher = keeper.fetcher
    assert keeper.move_to(music) == keeper.size() > 0
    assert keeper.folder == fetcher.kept_dir == music
    assert (music / "Modland/Protracker/Tester/beep.mod").is_file() and keeper.is_in(music)
    assert not (tmp_path / "kept").exists()                     # nothing is left behind
    assert keeper.scopes() == [Scope("modland", "Protracker", "Tester")]
    assert keeper.plan().done and keeper.check() == 0
    fetcher.downloads.clear()
    assert fetcher.get("modland:Protracker/Tester/beep.mod") == (
        music / "Modland/Protracker/Tester/beep.mod")
    assert fetcher.downloads == []

    # into a folder that has other things: they are left alone, there and on the way out
    keeper.move_to(tmp_path / "Music")
    assert (tmp_path / "Music/Modland/Protracker/Tester/beep.mod").is_file()
    assert not music.exists()
    keeper.move_to(music)
    assert (tmp_path / "Music/my own song.mod").read_bytes() == b"mine"
    assert sorted(f.name for f in (tmp_path / "Music").iterdir()) == ["Our Player",
                                                                      "my own song.mod"]


def test_a_move_that_cannot_be_is_not_begun(keeper, tmp_path, monkeypatch):
    from bbmp import keep

    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    kept = tmp_path / "kept"
    # a folder with kept music of its own
    other = Keeper(keeper.index, keeper.fetcher.clone(), tmp_path / "other")
    other.add(Scope("modland", "TFMX"))
    other.close()
    with pytest.raises(OSError, match="kept music of its own"):
        keeper.move_to(tmp_path / "other")
    # a folder inside the kept music
    with pytest.raises(OSError, match="inside"):
        keeper.move_to(kept / "Modland/Protracker/new")
    # a disk without room
    usage = namedtuple("usage", "total used free")
    monkeypatch.setattr(keep, "_same_disk", lambda a, b: False)
    monkeypatch.setattr(shutil, "disk_usage", lambda path: usage(10**12, 10**12 - 1000, 1000))
    with pytest.raises(OSError, match="not room"):
        keeper.move_to(tmp_path / "small disk")
    assert keeper.folder == kept and keeper.fetcher.kept_dir == kept
    assert (kept / "Modland/Protracker/Tester/beep.mod").is_file()
    assert keeper.scopes() == [Scope("modland", "Protracker", "Tester")] and keeper.plan().done


def test_kept_music_is_in_the_music_folder_if_nothing_else_is_said(tmp_path, monkeypatch):
    from bbmp import paths

    monkeypatch.delenv("BBMP_KEPT")
    monkeypatch.setattr(paths, "KEPT_DIR", tmp_path / "Music/ByteBandit Music Player")
    monkeypatch.setattr(paths, "OWN_KEPT_DIR", tmp_path / "data/kept")
    assert paths.kept_dir() == tmp_path / "Music/ByteBandit Music Player"
    assert paths.kept_dir("~/elsewhere") == Path.home() / "elsewhere"      # the setting
    monkeypatch.setenv("BBMP_KEPT", str(tmp_path / "said"))
    assert paths.kept_dir("~/elsewhere") == tmp_path / "said"
    # the name it had before the player had its name still counts, the new one first
    monkeypatch.setenv("MODLAND_KEPT", str(tmp_path / "said before"))
    assert paths.kept_dir() == tmp_path / "said"
    monkeypatch.delenv("BBMP_KEPT")
    assert paths.kept_dir() == tmp_path / "said before"
    monkeypatch.delenv("MODLAND_KEPT")
    # music that was kept in the player's own folder is not lost sight of
    (tmp_path / "data/kept").mkdir(parents=True)
    (tmp_path / "data/kept/kept.db").write_bytes(b"")
    assert paths.kept_dir() == tmp_path / "data/kept"


def test_the_music_folder_is_the_one_the_desktop_names(tmp_path, monkeypatch):
    from bbmp import paths

    monkeypatch.setattr(paths, "CONFIG_DIR", tmp_path / "config/bbmp")
    assert paths._music_dir() == Path.home() / "Music"
    (tmp_path / "config").mkdir()
    (tmp_path / "config/user-dirs.dirs").write_text(
        '# made by xdg-user-dirs\nXDG_DESKTOP_DIR="$HOME/Skrivbord"\nXDG_MUSIC_DIR="$HOME/Musik"\n')
    assert paths._music_dir() == Path.home() / "Musik"
    (tmp_path / "config/user-dirs.dirs").write_text('XDG_MUSIC_DIR="$HOME/"\n')    # none set
    assert paths._music_dir() == Path.home() / "Music"


def test_a_kept_file_opened_as_a_file_is_the_collections_song(keeper, tmp_path):
    """In the Files tab and a file manager, kept music is files like any other."""
    keeper.add(Scope("modland", "TFMX"))
    keeper.run()
    index = keeper.index
    index.kept_dir = keeper.folder
    song = index.local_song(tmp_path / "kept/Modland/TFMX/Tester/mdat.level1")
    assert song.key == "modland:TFMX/Tester/mdat.level1" and not song.local
    assert index.local_song(tmp_path / "kept/Modland/TFMX/Tester/smpl.level1") is None   # samples
    assert index.local_song(tmp_path / "kept/kept.db") is None
    assert [s.key for s in index.local_songs(tmp_path / "kept", recursive=True)] == [
        "modland:TFMX/Tester/mdat.level1", "modland:TFMX/Tester/mdat.level2"]


def test_one_song_is_kept_with_the_files_it_needs(keeper, tmp_path):
    song = keeper.index.resolve("modland:TFMX/Tester/mdat.level1")
    scope = Scope.of(song)
    assert scope.name == "level1 by Tester"
    assert Scope("modland", "TFMX").holds(scope) and not scope.holds(Scope("modland", "TFMX"))
    keeper.add(scope)
    keeper.run()
    kept = tmp_path / "kept/Modland/TFMX/Tester"
    assert sorted(f.name for f in kept.iterdir()) == ["mdat.level1", "smpl.level1"]
    assert keeper.scopes() == [scope] and keeper.states()[0].complete
    # with the artist kept too, the song is let go and its files stay
    keeper.add(Scope("modland", "TFMX", "Tester"))
    keeper.run()
    keeper.drop(scope)
    assert len(list(kept.iterdir())) == 4
    keeper.drop(Scope("modland", "TFMX", "Tester"))
    assert not kept.exists()


def test_a_song_of_a_hosted_collection_is_kept_as_its_hosts(tmp_path, server):
    index = catalog_of(tmp_path, SONGS, server, with_hvsc=True)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    (song,) = [s for s in index.search("Commando", collection="hvsc")]
    assert song.collection == "hvsc"
    keeper.add(Scope.of(song))
    keeper.run()
    assert (tmp_path / "kept/Modland/HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid").is_file()
    assert keeper.fetcher.is_kept(song.key)


def test_a_song_the_user_chose_stays_when_the_collection_drops_it(tmp_path, server):
    songs = [p for p in SONGS if p.startswith("Protracker/")]
    index = catalog_of(tmp_path, songs, server)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    keeper.add(Scope.of(index.resolve("modland:Protracker/Tester/beep.mod")))
    keeper.run()
    songs.remove("Protracker/Tester/beep.mod")
    keeper = Keeper(catalog_of(tmp_path, songs, server), LocalFetcher(tmp_path / "cache", server))
    assert keeper.plan().done
    keeper.run()
    assert (tmp_path / "kept/Modland/Protracker/Tester/beep.mod").is_file()


def test_a_database_from_before_single_songs_is_read(keeper, tmp_path):
    import sqlite3
    (tmp_path / "kept").mkdir()
    con = sqlite3.connect(tmp_path / "kept/kept.db")
    con.executescript("CREATE TABLE scopes(collection TEXT, format TEXT, artist TEXT, added INTEGER,"
                      " PRIMARY KEY (collection, format, artist));"
                      "INSERT INTO scopes VALUES ('modland', 'TFMX', '', 1);")
    con.close()
    assert keeper.scopes() == [Scope("modland", "TFMX")]
    keeper.add(Scope.of(keeper.index.resolve("modland:Protracker/Tester/beep.mod")))
    assert len(keeper.scopes()) == 2


def test_the_folders_of_the_old_name_move_to_the_new(tmp_path, monkeypatch):
    from bbmp import paths
    for var, sub in (("XDG_DATA_HOME", "share"), ("XDG_CONFIG_HOME", "config"),
                     ("XDG_CACHE_HOME", "cache")):
        monkeypatch.setenv(var, str(tmp_path / sub))
        (tmp_path / sub / "modland-player").mkdir(parents=True)
        (tmp_path / sub / "modland-player" / "mine").write_text(sub)
    monkeypatch.setattr(paths, "DATA_DIR", tmp_path / "share/bbmp")
    monkeypatch.setattr(paths, "CONFIG_DIR", tmp_path / "config/bbmp")
    monkeypatch.setattr(paths, "CACHE_DIR", tmp_path / "cache/bbmp")
    monkeypatch.setattr(paths, "MUSIC_DIR", tmp_path / "Music")
    monkeypatch.setattr(paths, "KEPT_DIR", tmp_path / "Music/ByteBandit Music Player")
    (tmp_path / "Music/Modland Player/Modland").mkdir(parents=True)
    (tmp_path / "share/bbmp/native").mkdir(parents=True)     # made before the move
    (tmp_path / "share/modland-player/native").mkdir()
    assert len(paths.move_old_folders()) == 4
    assert (tmp_path / "share/modland-player/native").is_dir()    # the new one's stays
    assert (tmp_path / "share/bbmp/mine").read_text() == "share"
    assert (tmp_path / "config/bbmp/mine").read_text() == "config"
    assert (tmp_path / "Music/ByteBandit Music Player/Modland").is_dir()
    assert paths.move_old_folders() == []                 # once


def test_an_album_is_kept(keeper, tmp_path):
    song = keeper.index.resolve("modland:IFF-SMUS/Tester/Album/song.smus")
    scope = Scope.album_of(song)
    assert scope.name == "Album by Tester on Modland"
    keeper.add(scope)
    keeper.run()
    kept = tmp_path / "kept/Modland/IFF-SMUS/Tester/Album"
    assert (kept / "song.smus").is_file() and (kept / "Instruments/bass.instr").is_file()
    assert keeper.kept(Scope.of(song)) == scope          # the song is kept with its album
    assert keeper.kept(Scope("modland", "IFF-SMUS", "Tester")) is None


def test_a_playlist_is_kept_as_it_changes(tmp_path, server):
    from bbmp.library import Library
    from conftest import SONGS
    index = catalog_of(tmp_path, SONGS, server)
    lib = Library(tmp_path / "user.db")
    pid = lib.create_playlist("Chips")
    lib.add_to_playlist(pid, ["modland:Protracker/Tester/beep.mod",
                             "modland:TFMX/Tester/mdat.level1"])
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server), library=lib)
    scope = Scope.playlist(f"playlist:{pid}")
    assert scope.name == "the playlist Chips"
    keeper.add(scope)
    keeper.run()
    kept = tmp_path / "kept/Modland"
    assert (kept / "Protracker/Tester/beep.mod").is_file()
    assert (kept / "TFMX/Tester/smpl.level1").is_file()          # with what the song needs
    # a song added is fetched, a song taken out goes
    lib.add_to_playlist(pid, ["modland:Protracker/Tester/beep two.mod"])
    lib.remove_from_playlist(pid, "modland:Protracker/Tester/beep.mod")
    plan = keeper.plan()
    assert [f.path for f in plan.fetch] == ["Protracker/Tester/beep two.mod"]
    assert [f.path for f in plan.remove] == ["Protracker/Tester/beep.mod"]
    keeper.run(plan)
    assert (kept / "Protracker/Tester/beep two.mod").is_file()
    assert not (kept / "Protracker/Tester/beep.mod").exists()
    fav = Scope.playlist("favorites")
    assert fav.name == "your favorites"


def test_fetching_keeps_to_a_speed_and_waits_for_the_night(keeper):
    import time as t
    keeper.add(Scope("modland", "Protracker", "Tester"))
    plan = keeper.plan()
    size = plan.fetch_bytes
    began = t.monotonic()
    result = keeper.run(plan, speed=size * 2)            # the whole of it in half a second
    assert result.done == 3 and t.monotonic() - began >= 0.4
    keeper.add(Scope("modland", "TFMX"))
    result = keeper.run(allowed=lambda: False)           # not now
    assert result.done == 0 and "night" in result.stopped


def test_all_of_hvsc_comes_as_its_archive(tmp_path, server, monkeypatch):
    import subprocess
    from bbmp import keep
    if shutil.which("7z") is None:
        pytest.skip("7z is not here")
    index = catalog_of(tmp_path, SONGS, server, with_hvsc=True)
    fetcher = LocalFetcher(tmp_path / "cache", server)
    keeper = Keeper(index, fetcher)
    # HVSC's archive, as it publishes it: C64Music/..., one tune in another version
    music = tmp_path / "archive/C64Music"
    for p in SONGS:
        if p.startswith("HVSC/"):
            f = music / p[len("HVSC/"):]
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(server[p] if "Commando" in p else b"another version")
    packed = tmp_path / "hvsc.7z"
    subprocess.run(["7z", "a", "-bd", str(packed), "C64Music"], cwd=tmp_path / "archive",
                   check=True, capture_output=True)
    monkeypatch.setattr(keep, "_hvsc_archive_url", lambda: packed.as_uri())
    monkeypatch.setattr(Keeper, "ARCHIVE_FROM", 1)
    keeper.add(Scope("hvsc"))
    result = keeper.run()
    kept = tmp_path / "kept/Modland/HVSC"
    assert (kept / "MUSICIANS/H/Hubbard_Rob/Commando.sid").read_bytes() == \
        server["HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid"]
    # the tune the archive has in another version was fetched on its own
    assert fetcher.downloads == ["HVSC/GAMES/A-F/Bionic.sid"]
    assert (kept / "GAMES/A-F/Bionic.sid").read_bytes() == server["HVSC/GAMES/A-F/Bionic.sid"]
    assert result.done == 2 and result.failed == [] and keeper.plan().done
    assert not (tmp_path / "kept/.hvsc-archive").exists()


def test_saved_music_stays_when_its_collection_is_turned_off(tmp_path, server):
    songs = [p for p in SONGS if p.startswith(("Protracker/", "HVSC/"))]
    index = catalog_of(tmp_path, songs, server)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    keeper.add(Scope("modland", "Protracker"))
    keeper.run()
    kept = tmp_path / "kept/Modland/Protracker/Tester/beep.mod"
    assert kept.is_file()
    # Modland is turned off (or its catalog didn't load): only HVSC's catalog is there
    raw, sizes = listing(songs, server)
    other = tmp_path / "other"
    other.mkdir()
    index_mod.build({"hvsc": hvsc.read_list(raw, sizes, [], 0)}, other / "index.db")
    keeper = Keeper(index_mod.Index(other / "index.db"), LocalFetcher(tmp_path / "cache", server))
    assert keeper.plan().remove == []
    keeper.run()
    assert kept.is_file()
    # and with no catalog at all
    empty = tmp_path / "empty"
    empty.mkdir()
    keeper = Keeper(index_mod.Index(empty / "index.db"), LocalFetcher(tmp_path / "cache", server))
    assert keeper.plan().remove == []


def test_a_saved_list_without_its_library_takes_nothing_away(tmp_path, server):
    from bbmp.library import Library
    index = catalog_of(tmp_path, SONGS, server)
    lib = Library(tmp_path / "user.db")
    pid = lib.create_playlist("Chips")
    lib.add_to_playlist(pid, ["modland:Protracker/Tester/beep.mod"])
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server), library=lib)
    keeper.add(Scope.playlist(f"playlist:{pid}"))
    keeper.run()
    # `bbmp keep --folder` once made a Keeper without the library, which saw no songs
    # in the list and took them all away
    alone = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    assert alone.plan().remove == []


def test_a_favorite_that_moved_keeps_its_file_and_follows_the_song(tmp_path, server):
    from bbmp.library import Library
    songs = [p for p in SONGS if p.startswith("Protracker/")]
    index = catalog_of(tmp_path, songs, server)
    lib = Library(tmp_path / "user.db")
    lib.toggle_favorite("modland:Protracker/Tester/beep.mod")
    assert lib.relocate(index) == []            # it is where it was: its checksum is noted
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server), library=lib)
    keeper.add(Scope.playlist("favorites"))
    keeper.run()
    kept = tmp_path / "kept/Modland/Protracker"
    assert (kept / "Tester/beep.mod").is_file()
    # Modland moves the song to another folder
    now = dict(server)
    now["Protracker/Tester Himself/beep.mod"] = server["Protracker/Tester/beep.mod"]
    moved = [p for p in songs if p != "Protracker/Tester/beep.mod"] + [
        "Protracker/Tester Himself/beep.mod"]
    (tmp_path / "new").mkdir()
    index = catalog_of(tmp_path / "new", moved, now)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", now), library=lib)
    assert keeper.plan().remove == []           # not known where it went: it stays
    assert lib.relocate(index) == [("modland:Protracker/Tester/beep.mod",
                                    "modland:Protracker/Tester Himself/beep.mod")]
    assert lib.favorite_keys() == {"modland:Protracker/Tester Himself/beep.mod"}
    plan = keeper.plan()
    assert [(a.path, b.path) for a, b in plan.move] == [
        ("Protracker/Tester/beep.mod", "Protracker/Tester Himself/beep.mod")]
    keeper.run(plan)
    assert (kept / "Tester Himself/beep.mod").is_file()


# ---- the folder is the kept music's, and nothing outside it ----

def test_a_path_that_leads_out_of_the_folder_is_refused(keeper, tmp_path):
    from bbmp.keep import File, Outside, Plan
    keeper.fetcher.server["../../escape.mod"] = b"not wanted here"
    outside = tmp_path / "mine.mod"
    outside.write_bytes(b"the user's own")
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper._record([File("modland", "../../mine.mod", "", 14)])     # as if it were kept
    plan = keeper.plan()
    plan.fetch.append(File("modland", "../../escape.mod", "", 15))
    plan.fetch.append(File("modland", "/tmp/escape.mod", "", 15))
    plan.move.append((plan.fetch[0], File("modland", "../../moved.mod", "", 1)))
    plan.remove.append(File("modland", "../../mine.mod", "", 14))
    result = keeper.run(plan)
    assert result.done == 3 and sorted(p for p, _ in result.failed) == [
        "../../escape.mod", "/tmp/escape.mod"]
    assert not (tmp_path / "escape.mod").exists() and not (tmp_path / "moved.mod").exists()
    assert outside.read_bytes() == b"the user's own" and ("modland", "../../mine.mod") not in keeper.have()
    assert keeper.fetcher.downloads.count("../../escape.mod") == 0
    with pytest.raises(Outside):
        keeper.path(File("modland", "a/../../b", "", 0))


def test_stopped_the_folder_stays_locked_until_the_files_on_their_way_are_there(
        keeper, monkeypatch):
    import threading
    import time as t
    from conftest import LocalFetcher
    slow = LocalFetcher._download

    def download(self, key, dest=None):
        t.sleep(0.8)
        slow(self, key, dest)
    monkeypatch.setattr(LocalFetcher, "_download", download)
    keeper.add(Scope("modland", "Protracker"))
    stop = threading.Event()
    threading.Timer(0.1, stop.set).start()
    result = keeper.run(stop=stop)
    assert not [th for th in threading.enumerate() if th.name.startswith("keep-") and th.is_alive()]
    assert result.done == 4 and len(keeper.have()) == 4       # and written down


def test_files_deleted_by_hand_are_not_looked_for_while_music_is_fetched(keeper, tmp_path):
    import fcntl
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    (tmp_path / "kept/Modland/Protracker/Tester/beep.mod").unlink()
    with open(tmp_path / "kept/.fetching", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)          # another player fetches
        assert keeper.check() == 0
    assert keeper.check() == 1


def test_the_speed_limit_is_shared_fairly_by_the_workers(keeper, monkeypatch):
    import time as t
    from conftest import LocalFetcher
    began = []
    fetch = LocalFetcher._download

    def download(self, key, dest=None):
        began.append(t.monotonic())
        fetch(self, key, dest)
    monkeypatch.setattr(LocalFetcher, "_download", download)
    keeper.add(Scope("modland", "Protracker"))
    plan = keeper.plan()
    assert len(plan.fetch) == 4
    keeper.run(plan, speed=int(plan.fetch_bytes / 0.8), workers=4)    # 0.2 s a file
    # the four don't all begin at once and then wait: they take turns
    began.sort()
    assert began[-1] - began[0] >= 0.4


def test_a_move_that_fails_half_way_puts_everything_back(keeper, tmp_path, monkeypatch):
    import os
    from bbmp import keep
    monkeypatch.setattr(keep, "_same_disk", lambda a, b: True)
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    rename = os.rename

    def failing(a, b):
        if Path(a).name == "kept.db":
            raise OSError("the disk said no")
        rename(a, b)
    monkeypatch.setattr(os, "rename", failing)
    with pytest.raises(OSError, match="said no"):
        keeper.move_to(tmp_path / "elsewhere")
    monkeypatch.setattr(os, "rename", rename)
    kept = tmp_path / "kept"
    assert keeper.folder == kept and (kept / "Modland/Protracker/Tester/beep.mod").is_file()
    assert not (tmp_path / "elsewhere/Modland").exists() and keeper.plan().done


def test_a_database_that_cannot_be_brought_up_to_date_is_left_as_it_was(keeper, tmp_path):
    import sqlite3
    (tmp_path / "kept").mkdir()
    con = sqlite3.connect(tmp_path / "kept/kept.db")
    con.executescript("CREATE TABLE scopes(collection TEXT, format TEXT, artist TEXT,"
                      " PRIMARY KEY (collection, format, artist));"      # no "added"
                      "INSERT INTO scopes VALUES ('modland', 'TFMX', '');")
    con.close()
    with pytest.raises(sqlite3.OperationalError):
        keeper.scopes()
    con = sqlite3.connect(tmp_path / "kept/kept.db")
    assert con.execute("SELECT * FROM scopes").fetchall() == [("modland", "TFMX", "")]
    assert not con.execute("SELECT 1 FROM sqlite_master WHERE name = 'scopes_before'").fetchone()


def test_the_old_folders_things_the_new_has_too_are_kept_aside(tmp_path, monkeypatch):
    from bbmp import paths
    for var, sub in (("XDG_DATA_HOME", "share"), ("XDG_CONFIG_HOME", "config"),
                     ("XDG_CACHE_HOME", "cache")):
        monkeypatch.setenv(var, str(tmp_path / sub))
    monkeypatch.setattr(paths, "DATA_DIR", tmp_path / "share/bbmp")
    monkeypatch.setattr(paths, "CONFIG_DIR", tmp_path / "config/bbmp")
    monkeypatch.setattr(paths, "CACHE_DIR", tmp_path / "cache/bbmp")
    monkeypatch.setattr(paths, "MUSIC_DIR", tmp_path / "Music")
    monkeypatch.setattr(paths, "KEPT_DIR", tmp_path / "Music/ByteBandit Music Player")
    for d in ("share/modland-player", "share/bbmp", "cache/modland-player/files",
              "cache/bbmp/files"):
        (tmp_path / d).mkdir(parents=True)
    (tmp_path / "share/modland-player/user.db").write_text("the old favorites")
    (tmp_path / "share/bbmp/user.db").write_text("made by the new player")
    paths.move_old_folders()
    assert (tmp_path / "share/bbmp/user.db").read_text() == "made by the new player"
    assert (tmp_path / "share/bbmp/user.db.from-modland-player").read_text() == "the old favorites"
    assert not (tmp_path / "share/modland-player").exists()
    assert (tmp_path / "cache/modland-player/files").is_dir()       # the cache is left
    assert not (tmp_path / "cache/bbmp/files.from-modland-player").exists()


def test_what_is_made_for_a_replayer_counts_only_when_it_was_made_whole(tmp_path):
    import sys
    from bbmp import extras
    source = tmp_path / "rom.bin"
    source.write_bytes(b"rom")
    dest = tmp_path / "made/2608_RIM.WAV"
    half = [sys.executable, "-c", f"open({str(dest)!r}, 'w').write('half'); raise SystemExit(1)"]
    whole = [sys.executable, "-c", f"open({str(dest)!r}, 'w').write('whole')"]
    assert extras._made(source, dest, half) is None and dest.exists()
    assert extras._made(source, dest, whole) == dest and dest.read_text() == "whole"
    assert extras._made(source, dest, half) == dest            # made: not again


# ---- at night ----

def test_hvscs_archive_waits_for_the_night_too(tmp_path, server, monkeypatch):
    from bbmp import keep
    index = catalog_of(tmp_path, SONGS, server, with_hvsc=True)
    keeper = Keeper(index, LocalFetcher(tmp_path / "cache", server))
    asked = []
    monkeypatch.setattr(keep, "_hvsc_archive_url", lambda: asked.append(1) or "file:///none")
    monkeypatch.setattr(keep.shutil, "which", lambda name: "/usr/bin/7z")
    monkeypatch.setattr(Keeper, "ARCHIVE_FROM", 1)
    keeper.add(Scope("hvsc"))
    result = keeper.run(allowed=lambda: False)
    assert asked == [] and result.waiting and result.failed == [] and result.done == 0
    assert keeper.fetcher.downloads == []


def test_by_day_the_page_says_when_saving_begins(catalog, fetcher, tmp_path):
    from test_tui import make_app, run, settle
    from bbmp.settings import Settings
    keeper = Keeper(catalog, fetcher)
    keeper.add(Scope("modland", "Protracker", "Tester"))
    keeper.run()
    keeper.close()
    (tmp_path / "kept/Modland/Protracker/Tester/beep.mod").unlink()     # by hand

    async def go():
        app = make_app(catalog, fetcher, tmp_path,
                       settings=Settings(resume=False, keep_when="night"))
        app._keep_allowed = lambda: False            # it is day
        async with app.run_test(size=(130, 44)) as pilot:
            assert await settle(pilot, lambda: app.keeper is not None)
            app._keep(Scope("modland", "TFMX"))
            said = lambda: [(n.message, n.severity) for n in app._notifications]
            assert await settle(pilot, lambda: any("saving starts at 00:00" in m
                                                   for m, _ in said())), said()
            assert all(severity != "error" for _, severity in said())
            await pilot.press("t")
            shown = lambda: app.export_screenshot().replace("&#160;", " ")
            # the file deleted by hand is not shown as there
            assert await settle(pilot, lambda: "2 files not there yet" in shown()), shown()
            assert "Waiting for the night: saving starts at 00:00" in shown()
    run(go())
