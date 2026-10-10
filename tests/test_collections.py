# SPDX-License-Identifier: GPL-3.0-or-later
"""More than one collection: Modland and a second one made up for the tests."""

import asyncio
import functools
import http.server
import os
import sqlite3
import threading
import time
from dataclasses import replace

import pytest

from conftest import CHIPLAND_SONGS, HVSC_COUNT, MODLAND_COUNT, LocalFetcher, modland_listing
from bbmp import collection as collections_, index as index_mod, playlists
from bbmp.audio import NullOutput
from bbmp.collection.transports import HttpTransport
from bbmp.fetch import Fetcher
from bbmp.index import Index
from bbmp.library import Library
from bbmp.player import Player
from bbmp.settings import Settings
from bbmp.tui import ModlandApp


def wait_for(cond, timeout=8.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.03)
    return False


# ---- keys ----

def test_a_key_names_the_collection():
    assert collections_.key("modland", "AHX/Pink/inside.ahx") == "modland:AHX/Pink/inside.ahx"
    assert collections_.split("modland:AHX/Pink/inside.ahx") == ("modland", "AHX/Pink/inside.ahx")
    assert collections_.split("hvsc:MUSICIANS/H/Hubbard_Rob/Commando.sid")[0] == "hvsc"
    assert collections_.split("/home/me/Music/song.mod") == ("local", "/home/me/Music/song.mod")
    # saved before there were collections, and names with a colon in them
    assert collections_.split("AHX/Pink/inside.ahx") == ("modland", "AHX/Pink/inside.ahx")
    assert collections_.split("Protracker/Artist/part 2: the end.mod")[0] == "modland"
    assert collections_.split("modland:Protracker/A/b: c.mod") == ("modland", "Protracker/A/b: c.mod")


def test_ids_are_checked():
    class Bad(collections_.Collection):
        id = "Not An Id"
    with pytest.raises(ValueError):
        collections_.register(Bad())
    Bad.id = "local"
    with pytest.raises(ValueError):
        collections_.register(Bad())
    assert [c.id for c in collections_.known()] == ["modland", "hvsc"]


# ---- the catalog ----

def test_the_catalog_holds_both(two):
    assert [(c, name, songs) for c, name, songs, _ in two.collections()] == [
        ("chipland", "Chipland", 3), ("modland", "Modland", MODLAND_COUNT)]
    assert two.song_count() == MODLAND_COUNT + 3 and two.song_count("chipland") == 3
    protracker = {f: n for f, n, _ in two.formats()}["Protracker"]
    assert protracker == 6
    assert {f: n for f, n, _ in two.formats("modland")}["Protracker"] == 3
    assert [f for f, _, _ in two.formats("chipland")] == ["Protracker"]
    assert [a for a, _ in two.artists("Protracker", "chipland")] == ["Stranger", "Tester"]
    assert [a for a, _ in two.artists("Protracker")] == ["Other Guy", "Stranger", "Tester"]
    assert [s.key for s in two.songs("Protracker", "Tester", "chipland")] == [
        "chipland:tunes/Tester/beep.mod", "chipland:tunes/Tester/Disk One/only here.mod"]


def test_the_same_path_in_two_collections_is_two_songs(two):
    theirs = two.by_path("tunes/Tester/beep.mod", "chipland")
    assert theirs.collection == "chipland" and theirs.key == "chipland:tunes/Tester/beep.mod"
    assert two.by_path("tunes/Tester/beep.mod") is None            # Modland has no such path
    # the same file as Modland's beep.mod: a song resolved says so, as lists do
    assert two.resolve(theirs.key) == replace(theirs, also=("modland",))
    assert two.resolve("modland:Protracker/Tester/beep.mod").collection == "modland"
    assert two.resolve("nowhere:tunes/Tester/beep.mod") is None


def test_a_saved_song_knows_the_other_collections_that_have_it(two):
    """A song of a playlist or the favorites is resolved from its key: its copies in
    other collections are there as they are in browse."""
    ours = two.resolve("modland:Protracker/Tester/beep.mod")
    assert ours.also == ("chipland",)
    assert [s.key for s in two.copies(ours)] == ["chipland:tunes/Tester/beep.mod"]
    assert two.resolve("modland:Protracker/Tester/beep two.mod").also == ()


def test_a_saved_tune_knows_the_collection_its_host_hosts(hosted):
    tune = hosted.resolve("modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")
    assert tune.also == ("hvsc",)
    assert [s.collection for s in hosted.copies(tune)] == ["hvsc"]


def test_search_covers_every_collection_or_one(two):
    assert [s.key for s in two.search("far away")] == ["chipland:tunes/Stranger/far away.mod"]
    assert two.search("far away", collection="modland") == []
    assert [s.key for s in two.search("only here", collection="chipland")] == [
        "chipland:tunes/Tester/Disk One/only here.mod"]
    for _ in range(20):
        assert two.random_song(collection="chipland").collection == "chipland"


def test_a_file_two_collections_have_shows_once(two):
    # by checksum: Chipland's beep.mod is the file Modland has
    assert [s.key for s in two.search("beep") if s.title == "beep"] == [
        "modland:Protracker/Tester/beep.mod"]
    assert "chipland:tunes/Tester/beep.mod" not in [s.key for s in two.songs("Protracker", "Tester")]
    assert "chipland:tunes/Tester/beep.mod" not in [s.key for s in two.artist_songs("Tester")]
    # in its own collection it is there
    assert [s.key for s in two.search("beep", collection="chipland")] == [
        "chipland:tunes/Tester/beep.mod"]
    assert len(two.songs("Protracker", "Tester", "chipland")) == 2
    # the same name is not the same file: "beep two" and "only here" have other checksums
    assert len(two.songs("Protracker", "Tester")) == 3


def test_artist_songs_come_from_every_collection(two):
    keys = [s.key for s in two.artist_songs("Tester")]
    assert "chipland:tunes/Tester/Disk One/only here.mod" in keys
    assert "modland:Protracker/Tester/beep two.mod" in keys
    only_here = two.resolve("chipland:tunes/Tester/Disk One/only here.mod")
    assert [s.key for s in two.album_songs(only_here)] == [only_here.key]


def test_updating_one_collection_keeps_the_others(tmp_path, chipland, monkeypatch):
    db = tmp_path / "index.db"
    modland = collections_.get("modland")
    monkeypatch.setattr(modland, "listing", lambda progress=None: modland_listing())
    assert index_mod.update(db_path=db) == MODLAND_COUNT + 3
    built = dict((c, b) for c, _, _, b in Index(db).collections())

    chipland.files["tunes/Stranger/new one.mod"] = b"x"
    said = []
    monkeypatch.setattr(modland, "listing", lambda progress=None: 1 / 0)   # must not be asked
    assert index_mod.update(only=["chipland"], db_path=db, progress=said.append) == MODLAND_COUNT + 4
    ix = Index(db)
    assert ix.song_count("modland") == MODLAND_COUNT and ix.song_count("chipland") == 4
    assert ix.lengths("modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid") == [235.594, 61.288, 6.0]
    assert ix.companions("modland:TFMX/Tester/mdat.level1") == ["modland:TFMX/Tester/smpl.level1"]
    assert dict((c, b) for c, _, _, b in ix.collections())["modland"] == built["modland"]
    # Modland's copy of a file both have is still the one that shows
    assert [s.key for s in ix.search("beep") if s.title == "beep"] == [
        "modland:Protracker/Tester/beep.mod"]


def test_a_collection_that_is_down_keeps_its_songs(tmp_path, chipland, monkeypatch):
    db = tmp_path / "index.db"
    modland = collections_.get("modland")
    monkeypatch.setattr(modland, "listing", lambda progress=None: modland_listing())
    index_mod.update(db_path=db)
    chipland.reachable = False
    said = []
    assert index_mod.update(db_path=db, progress=said.append) == MODLAND_COUNT + 3
    assert "Can't reach Chipland; keeping its songs as they were." in said
    with pytest.raises(OSError):
        index_mod.update(only=["chipland"], db_path=db)
    assert Index(db).song_count("chipland") == 3



def test_an_empty_list_leaves_the_catalog_as_it_was(tmp_path, chipland, monkeypatch):
    """An error page or an empty answer read as a list emptied the catalog, and then the
    music kept of it was taken away (7 October 2026). Such a list is not believed."""
    db = tmp_path / "index.db"
    modland = collections_.get("modland")
    monkeypatch.setattr(modland, "listing", lambda progress=None: modland_listing())
    index_mod.update(db_path=db)
    files, chipland.files = chipland.files, {}
    failed = {}
    assert index_mod.update(db_path=db, failed=failed) == MODLAND_COUNT + 3
    assert "chipland" in failed and "0 songs" in failed["chipland"]
    assert Index(db).song_count("chipland") == 3
    assert not list(db.glob(".chipland-*"))         # its unfinished catalog is gone
    # and a smaller one is taken where it is not far smaller
    chipland.files = dict(list(files.items())[:2])
    failed = {}
    index_mod.update(db_path=db, failed=failed)
    assert failed == {} and Index(db).song_count("chipland") == 2
    # a much smaller list of a big catalog is not believed either
    assert index_mod._too_small(40, 1000) and not index_mod._too_small(900, 1000)
    assert index_mod._too_small(0, 1) and not index_mod._too_small(0, 0)


def test_an_odd_answer_from_one_collection_leaves_the_others_updated(tmp_path, chipland,
                                                                      monkeypatch):
    """Any error from one collection's list (cut short, changed JSON) stopped the whole
    update, and the lists already had of the others were let go (7 October 2026)."""
    import http.client
    db = tmp_path / "index.db"
    modland = collections_.get("modland")
    monkeypatch.setattr(modland, "listing", lambda progress=None: modland_listing())
    index_mod.update(db_path=db)
    chipland.files["tunes/Stranger/new one.mod"] = b"x"

    def cut_short(progress=None):
        raise http.client.IncompleteRead(b"")
    monkeypatch.setattr(modland, "listing", cut_short)
    failed = {}
    assert index_mod.update(db_path=db, failed=failed) == MODLAND_COUNT + 4
    assert "modland" in failed
    assert Index(db).song_count("modland") == MODLAND_COUNT

def test_a_collection_the_player_no_longer_knows_leaves_the_catalog(tmp_path, chipland, monkeypatch):
    db = tmp_path / "index.db"
    index_mod.build({"modland": modland_listing(), "chipland": chipland.listing()}, db)
    collections_.unregister("chipland")
    index_mod.build({"modland": modland_listing()}, db)
    assert [c for c, *_ in Index(db).collections()] == ["modland"]


def test_a_catalog_from_before_collections_is_downloaded_again(tmp_path):
    folder = tmp_path / "catalogs"
    folder.mkdir()
    con = sqlite3.connect(folder / "modland.db")
    con.executescript("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);"
                      "INSERT INTO meta VALUES ('built', '1700000000');"
                      "CREATE TABLE files(id INTEGER PRIMARY KEY, path TEXT);")
    con.close()
    assert not Index.exists(folder)
    assert not Index.exists(tmp_path / "none")
    index_mod.build({"modland": modland_listing()}, folder)
    assert Index.exists(folder)


def test_the_one_catalog_of_before_is_split_into_one_per_collection(tmp_path, chipland):
    """Before, one file had every collection; it is split once, and taken away."""
    import sqlite3 as sql
    from bbmp import index as index_mod
    old = tmp_path / "index.db"
    con = sql.connect(old)
    con.executescript(index_mod.SCHEMA)
    con.execute("INSERT INTO meta VALUES ('version', ?)", (index_mod.OLD_VERSION,))
    for c, listing in (("modland", modland_listing()), ("chipland", chipland.listing())):
        for e in listing.entries:
            con.execute("INSERT INTO files(collection, path, format, artist, album, name, hidden,"
                        " md5, size) VALUES (?,?,?,?,?,?,?,?,?)",
                        (c, e.path, e.format, e.artist, e.album, e.name, int(e.hidden), e.md5,
                         e.size))
        con.execute("INSERT INTO collections VALUES (?, ?, 0, 1700000000, 0, 0, 5)", (c, c))
    con.execute("INSERT INTO changes VALUES ('modland', '2026-09-20', 'added',"
                " 'Protracker/Tester/beep.mod')")
    con.commit()
    con.close()
    folder = tmp_path / "catalogs"
    assert index_mod.move_old_catalog(folder, old)
    assert not old.exists() and sorted(f.name for f in folder.iterdir()) == [
        "chipland.db", "modland.db"]
    ix = Index(folder)
    assert ix.song_count("modland") == MODLAND_COUNT and ix.song_count("chipland") == 3
    assert ix.about("modland")["built"] == 1700000000 and ix.change_days()
    assert not index_mod.move_old_catalog(folder, old)          # once


# ---- downloading ----

def test_each_collection_has_its_folder_in_the_cache(two, chipland, fetcher):
    ours = fetcher.get("modland:Protracker/Tester/beep.mod")
    theirs = fetcher.get("chipland:tunes/Tester/beep.mod")
    assert ours == fetcher.cache_dir / "modland/Protracker/Tester/beep.mod"
    assert theirs == fetcher.cache_dir / "chipland/tunes/Tester/beep.mod"
    assert fetcher.downloads == ["Protracker/Tester/beep.mod"]
    assert chipland.downloads == ["tunes/Tester/beep.mod"]
    fetcher.get("chipland:tunes/Tester/beep.mod")
    assert chipland.downloads == ["tunes/Tester/beep.mod"]            # from the cache now
    assert fetcher.cache_size("chipland") == theirs.stat().st_size
    assert fetcher.cache_size() == ours.stat().st_size + theirs.stat().st_size
    with pytest.raises(FileNotFoundError):
        fetcher.get("chipland:tunes/Tester/not there.mod")
    assert not list(fetcher.cache_dir.rglob("*.part"))
    with pytest.raises(FileNotFoundError):
        fetcher.get("nowhere:some/song.mod")


def test_paths_can_not_leave_the_cache(fetcher):
    for key in ("chipland:../../outside.mod", "chipland:/etc/passwd", "modland:a//b.mod"):
        with pytest.raises(FileNotFoundError):
            fetcher.local_path(key)


def test_an_old_cache_becomes_modlands_folder(tmp_path, mod_bytes):
    cache = tmp_path / "cache"
    (cache / "Protracker/Tester").mkdir(parents=True)
    (cache / "Protracker/Tester/beep.mod").write_bytes(mod_bytes)
    (cache / "AHX/Pink").mkdir(parents=True)
    (cache / "AHX/Pink/inside.ahx").write_bytes(b"x")
    fetcher = Fetcher(cache)
    assert sorted(f.name for f in cache.iterdir()) == [".layout", "modland"]
    assert fetcher.is_cached("modland:Protracker/Tester/beep.mod")
    assert fetcher.is_cached("AHX/Pink/inside.ahx")
    assert fetcher.cache_size() == len(mod_bytes) + 1
    # and only once: a folder that is there after that is a collection's
    (cache / "chipland/tunes").mkdir(parents=True)
    (cache / "chipland/tunes/a.mod").write_bytes(b"xy")
    again = Fetcher(cache)
    assert again.is_cached("chipland:tunes/a.mod") and again.is_cached("AHX/Pink/inside.ahx")


def test_a_download_holds_up_only_its_own_collection(chipland, tmp_path):
    from conftest import Chipland

    class Other(Chipland):
        id, name = "otherland", "Otherland"
    other = collections_.register(Other({}))
    try:
        fetcher = Fetcher(tmp_path / "cache", kept_dir=tmp_path / "kept")
        downloading, finish, got = threading.Event(), threading.Event(), []

        def download():
            with fetcher.transport("chipland") as t:
                got.append(t)
                downloading.set()
                finish.wait(5)
        worker = threading.Thread(target=download)
        worker.start()
        assert downloading.wait(5)
        began = time.monotonic()
        with fetcher.transport("otherland"):        # another collection doesn't wait
            pass
        fetcher.close()                             # nor does quitting
        assert time.monotonic() - began < 1
        assert not got[0].closed                    # the download goes on with it
        finish.set()
        worker.join(5)
        assert got[0].closed                        # and it is closed when that is over
        with fetcher.transport("chipland") as t:
            assert t is not got[0] and not t.closed
    finally:
        collections_.unregister(other.id)


def test_trim_leaves_what_was_used_a_moment_ago(chipland, tmp_path):
    fetcher = Fetcher(tmp_path / "cache", kept_dir=tmp_path / "kept")
    fetcher.get("chipland:tunes/Tester/beep.mod")                 # being played
    old = fetcher.local_path("chipland:tunes/Stranger/far away.mod")
    fetcher.get("chipland:tunes/Stranger/far away.mod")
    os.utime(old, (time.time() - 3600,) * 2)
    part = old.with_name("next.mod.part")                         # being downloaded
    part.write_bytes(b"x" * 100)
    fetcher.trim(1)
    assert fetcher.local_path("chipland:tunes/Tester/beep.mod").exists()
    assert not old.exists() and part.exists()


def test_an_artist_two_collections_have_counts_once(two):
    # Tester is in Modland's Protracker and in Chipland's; Other Guy and Stranger in one
    artists = {f: a for f, _, a in two.formats()}
    assert artists["Protracker"] == 3
    assert {f: a for f, _, a in two.formats("chipland")}["Protracker"] == 2


def test_trim_and_keep_know_collections(two, chipland, fetcher, tmp_path):
    fetcher.get("modland:Protracker/Tester/beep.mod")
    fetcher.get("chipland:tunes/Tester/beep.mod")
    fetcher.get("chipland:tunes/Stranger/far away.mod")
    for f in fetcher._files():
        os.utime(f, (time.time() - 3600,) * 2)      # used an hour ago
    fetcher.trim(1, keep={"chipland:tunes/Tester/beep.mod"})
    assert fetcher.is_cached("chipland:tunes/Tester/beep.mod")
    assert not fetcher.is_cached("modland:Protracker/Tester/beep.mod")
    assert not fetcher.is_cached("chipland:tunes/Stranger/far away.mod")
    assert (fetcher.cache_dir / ".layout").exists()
    # kept music is in a folder for every collection, by its name
    from bbmp.keep import Keeper, Scope
    keeper = Keeper(two, fetcher, tmp_path / "Music")
    keeper.add(Scope.of(two.resolve("chipland:tunes/Stranger/far away.mod")))
    keeper.add(Scope.of(two.resolve("modland:Protracker/Tester/beep.mod")))
    assert keeper.run().failed == []
    assert (tmp_path / "Music/Chipland/tunes/Stranger/far away.mod").is_file()
    assert (tmp_path / "Music/Modland/Protracker/Tester/beep.mod").is_file()
    assert two.local_song(tmp_path / "Music/Chipland/tunes/Stranger/far away.mod").local
    two.kept_dir = tmp_path / "Music"
    assert two.local_song(tmp_path / "Music/Chipland/tunes/Stranger/far away.mod").key == (
        "chipland:tunes/Stranger/far away.mod")


def test_http_transport(tmp_path):
    site = tmp_path / "site"
    (site / "C64Music/MUSICIANS").mkdir(parents=True)
    (site / "C64Music/MUSICIANS/a tune.sid").write_bytes(b"PSID-ish")
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(site))
    handler.log_message = lambda *a: None
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        transport = HttpTransport(f"http://127.0.0.1:{server.server_port}/C64Music/")
        transport.download("MUSICIANS/a tune.sid", tmp_path / "got.sid")
        assert (tmp_path / "got.sid").read_bytes() == b"PSID-ish"
        with pytest.raises(FileNotFoundError):
            transport.download("MUSICIANS/none.sid", tmp_path / "none.sid")
    finally:
        server.shutdown()
        server.server_close()
    with pytest.raises(OSError):
        transport.download("MUSICIANS/a tune.sid", tmp_path / "again.sid")    # nobody there now


# ---- your own data ----

def test_saved_songs_from_before_collections_are_modlands(tmp_path):
    db = tmp_path / "user.db"
    con = sqlite3.connect(db)
    con.executescript("""
        CREATE TABLE favorites(path TEXT PRIMARY KEY, added INTEGER);
        CREATE TABLE plays(id INTEGER PRIMARY KEY, path TEXT NOT NULL, format TEXT NOT NULL,
            artist TEXT NOT NULL, title TEXT NOT NULL, started INTEGER NOT NULL,
            seconds REAL NOT NULL DEFAULT 0);
        CREATE TABLE playlists(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, created INTEGER NOT NULL);
        CREATE TABLE playlist_songs(playlist INTEGER NOT NULL, pos INTEGER NOT NULL, path TEXT NOT NULL,
            PRIMARY KEY (playlist, pos));
        INSERT INTO favorites VALUES ('AHX/Pink/inside.ahx', 1), ('/home/me/Music/own.mod', 2);
        INSERT INTO plays(path, format, artist, title, started, seconds)
            VALUES ('Protracker/Tester/beep.mod', 'Protracker', 'Tester', 'beep', 1700000000, 31);
        INSERT INTO playlists VALUES (1, 'Mix', 1);
        INSERT INTO playlist_songs VALUES (1, 0, 'Protracker/Tester/beep.mod'), (1, 1, '/home/me/a.mod');
    """)
    con.close()
    for _ in range(2):                  # opening it again changes nothing more
        lib = Library(db)
        assert lib.favorite_keys() == {"modland:AHX/Pink/inside.ahx", "/home/me/Music/own.mod"}
        assert lib.playlist_keys(1) == ["modland:Protracker/Tester/beep.mod", "/home/me/a.mod"]
        assert lib.stats()["top_songs"][0][0] == "modland:Protracker/Tester/beep.mod"
        lib.con.close()


def test_favorites_playlists_and_history_mix_collections(two, tmp_path, mod_bytes):
    own = tmp_path / "own.mod"
    own.write_bytes(mod_bytes)
    lib = Library(tmp_path / "user.db")
    songs = [two.resolve("chipland:tunes/Stranger/far away.mod"),
             two.resolve("modland:Protracker/Tester/beep.mod"), two.local_song(own)]
    for s in songs:
        lib.toggle_favorite(s.key)
        lib.finish_play(lib.start_play(s), 30)
    pid = lib.create_playlist("Everything", [s.key for s in songs])
    assert lib.playlist_songs(pid, two) == songs
    assert set(lib.favorites(two)) == set(songs)
    assert sorted(s.collection for s in lib.history(two)) == ["chipland", "local", "modland"]
    # a collection that isn't there any more: its songs don't show, and aren't lost
    collections_.unregister("chipland")
    index_mod.build({"modland": modland_listing()}, two.path)
    now = Index(two.path)
    assert [s.collection for s in lib.playlist_songs(pid, now)] == ["modland", "local"]
    assert lib.playlist_keys(pid)[0] == "chipland:tunes/Stranger/far away.mod"


def test_playlist_files_name_songs_in_any_collection(two, tmp_path, mod_bytes):
    music = tmp_path / "music"
    music.mkdir()
    (music / "here.mod").write_bytes(mod_bytes)
    m3u = music / "both.m3u"
    m3u.write_text("#EXTM3U\n"
                   "https://chipland.example/files/tunes/Stranger/far away.mod\n"
                   "ftp://ftp.modland.com/pub/modules/Protracker/Tester/beep%20two.mod\n"
                   "chipland:tunes/Tester/Disk One/only here.mod\n"
                   "Protracker/Other Guy/coop-Tester/duet.mod\n"
                   "here.mod\n"
                   "https://elsewhere.example/files/tunes/Stranger/far away.mod\n")
    keys = playlists.read_playlist_file(m3u)
    found, missing = playlists.resolve(keys, two)
    assert found == ["chipland:tunes/Stranger/far away.mod", "modland:Protracker/Tester/beep two.mod",
                     "chipland:tunes/Tester/Disk One/only here.mod",
                     "modland:Protracker/Other Guy/coop-Tester/duet.mod", str(music / "here.mod")]
    assert missing == 0 and len(keys) == 5          # the link to elsewhere names no song
    text = "\n".join(s.path if s.local else collections_.get(s.collection).url(s.path) for s in map(two.resolve, found))
    assert "https://chipland.example/files/tunes/Stranger/far away.mod" in text
    assert playlists.resolve(playlists.parse_m3u(text), two) == (found, 0)


# ---- playing ----

def test_the_player_plays_from_both(two, chipland, fetcher, tmp_path):
    played = []
    p = Player(two, fetcher, NullOutput(realtime=False), on_song=lambda s: played.append(s.key),
               settings=Settings(subsongs="one"))
    try:
        songs = [two.resolve("chipland:tunes/Stranger/far away.mod"),
                 two.resolve("modland:Protracker/Tester/beep two.mod")]
        p.play_list(songs)
        assert wait_for(lambda: len(played) == 2, 30)
        assert played == [s.key for s in songs]
        assert chipland.downloads == ["tunes/Stranger/far away.mod"]
        assert p.session()["queue"] == played
        p.radio(collection="chipland")
        assert wait_for(lambda: p.status().radio == "all of Chipland")
        assert wait_for(lambda: p.status().song and p.status().song.collection == "chipland")
    finally:
        p.shutdown()


def test_radio_says_where_it_plays_from(catalog, fetcher):
    p = Player(catalog, fetcher, NullOutput(realtime=False))
    try:
        p.radio()
        assert wait_for(lambda: p.status().radio == "all of Modland")     # the only collection
        p.radio("Protracker", "Tester")
        assert wait_for(lambda: p.status().radio == "Protracker / Tester")
    finally:
        p.shutdown()


# ---- the TUI ----

async def settle(pilot, cond, timeout=6.0):
    end = time.time() + timeout
    while time.time() < end:
        if cond():
            return True
        await pilot.pause(0.05)
    return False


def make_app(index, fetcher, tmp_path, settings=None):
    return ModlandApp(index, fetcher, Library(tmp_path / "user.db"), NullOutput(realtime=False),
                      settings=settings or Settings(resume=False),
                      session_path=tmp_path / "session.json")


NEW = "\x00new"


def formats(app):
    """The formats Browse lists (none before its columns are in place)."""
    from test_tui import formats_list
    from bbmp.tui.browse import id_step
    ol = formats_list(app) if getattr(app, "_placed", False) else None
    if ol is None:
        return []
    return [id_step(i)[1] for i in (ol.get_option_at_index(k).id for k in range(ol.option_count)) if i]


def formats_title(app):
    from test_tui import formats_list
    return str(formats_list(app).border_title)


def footer_keys(app):
    return {binding.key for _, binding, enabled, _ in app.screen.active_bindings.values()
            if enabled and binding.show}


def test_one_collection_shows_no_choice(catalog, fetcher, tmp_path):
    async def go():
        app = make_app(catalog, fetcher, tmp_path)
        async with app.run_test(size=(120, 40)) as pilot:
            assert await settle(pilot, lambda: "Protracker" in formats(app))
            assert "Collections" not in str(app._levels[0].options)       # nothing to choose
            assert "c" not in footer_keys(app)
            await pilot.press("c")
            assert app.settings.collection == ""
            await pilot.press("0")
            rows = app.query_one("#settings-list")
            assert "collection" not in [rows.get_option_at_index(i).id
                                        for i in range(rows.option_count)]
    asyncio.run(go())


def test_choosing_a_collection(two, chipland, fetcher, tmp_path):
    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        app = make_app(two, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(130, 40)) as pilot:
            assert await settle(pilot, lambda: "TFMX" in formats(app))
            assert "every collection" in formats_title(app)
            assert "c" in footer_keys(app)

            await pilot.press("slash")
            for ch in "beep":
                await pilot.press(ch)
            await pilot.press("enter")
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: len(table.songs) == 2)     # beep (once), beep two
            assert "Protracker · Modland" in str(table.get_row_at(0)[-1])

            await pilot.press("c")                                         # Chipland
            assert await settle(pilot, lambda: [s.key for s in table.songs]
                                == ["chipland:tunes/Tester/beep.mod"])
            assert "on Chipland" in table.border_title
            assert settings.collection == "chipland"
            await pilot.press("escape", "1")                               # Browse went along
            assert await settle(pilot, lambda: formats(app) == ["Protracker"])
            assert "Chipland" in formats_title(app)
            assert app._path == [("collection", "chipland"), ("menu", "formats")]
            await pilot.press("enter")
            await pilot.pause(0.3)
            artists = app._levels[-1]
            assert [artists.get_option_at_index(i).id.split("\x1e")[1]
                    for i in range(artists.option_count)] == ["Stranger", "Tester"]
            await pilot.press("enter", "enter")                            # play Stranger's song
            assert await settle(pilot, lambda: app.player.status().song is not None)
            assert app.player.status().song.key == "chipland:tunes/Stranger/far away.mod"
            await pilot.press("f")
            assert await settle(pilot, lambda: app.library.favorite_keys()
                                == {"chipland:tunes/Stranger/far away.mod"})

            await pilot.press("7")                                         # the Info tab
            assert await settle(pilot, lambda: "On Chipland" in
                                app.export_screenshot().replace("&#160;", " "))

            await pilot.press("1", "c")              # in Browse, c: All and the collections
            assert await settle(pilot, lambda: app._placed and app._path == []
                                and app._levels[-1].border_title == "Browse")
            from test_tui import put
            put(app._levels[-1], ("collection", "modland"))
            await pilot.press("right")
            assert await settle(pilot, lambda: app._path[-1] == ("collection", "modland"))
            await pilot.press("right")                                     # its formats
            assert await settle(pilot, lambda: "TFMX" in formats(app))
            assert "Modland" in formats_title(app) and settings.collection == "modland"
            await pilot.press("left", "left")                              # the first list
            ids = [app._levels[-1].get_option_at_index(i).id for i in range(app._levels[-1].option_count)]
            assert ids[0] == "collection\x1e"                              # All, first
            from test_tui import go_in
            await go_in(pilot, app, ("collection", ""), ("menu", "formats"))   # every collection's
            assert await settle(pilot, lambda: settings.collection == "" and "TFMX" in formats(app)
                                and "every collection" in formats_title(app))
            assert app._levels[-2].border_title == "All"
        assert Settings.load(tmp_path / "settings.json").collection == ""

        # it opens on the collection it was closed on
        app = make_app(two, fetcher, tmp_path, settings=Settings(resume=False, collection="chipland"))
        async with app.run_test(size=(130, 40)) as pilot:
            assert await settle(pilot, lambda: formats(app) == ["Protracker"])
            await pilot.press("0")
            rows = app.query_one("#settings-list")
            row = next(rows.get_option_at_index(i) for i in range(rows.option_count)
                       if rows.get_option_at_index(i).id == "collection")
            assert "Chipland" in str(row.prompt)
            await pilot.press("4")                                         # the favorite is there
            favs = app.query_one("#fav-songs")
            assert await settle(pilot, lambda: [s.key for s in favs.songs]
                                == ["chipland:tunes/Stranger/far away.mod"])
    asyncio.run(go())


# ---- a collection that another one hosts: HVSC, which Modland has ----

COMMANDO = "MUSICIANS/H/Hubbard_Rob/Commando.sid"


def test_a_hosted_collections_songs_are_the_hosts():
    assert collections_.hosted() == ["hvsc"]
    assert collections_.key("hvsc", COMMANDO) == "modland:HVSC/" + COMMANDO
    assert collections_.split("hvsc:" + COMMANDO) == ("hvsc", COMMANDO)
    source = collections_.get("hvsc")
    assert source.url(COMMANDO).endswith("/pub/modules/HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")
    assert source.path_in("https://hvsc.c64.org/download/C64Music/" + COMMANDO) == COMMANDO
    assert source.path_in("https://elsewhere.example/C64Music/" + COMMANDO) is None


def test_hvsc_is_browsed_in_its_own_layout(hosted):
    assert [(c, name, songs) for c, name, songs, _ in hosted.collections()] == [
        ("hvsc", "HVSC", HVSC_COUNT), ("modland", "Modland", MODLAND_COUNT)]
    assert [f for f, _, _ in hosted.formats("hvsc")] == ["HVSC - Games", "HVSC - Musicians"]
    assert [a for a, _ in hosted.artists("HVSC - Musicians", "hvsc")] == ["Rob Hubbard"]
    assert [a for a, _ in hosted.artists("HVSC - Games", "hvsc")] == ["A-F"]
    (song,) = hosted.songs("HVSC - Musicians", "Rob Hubbard", "hvsc")
    assert (song.collection, song.path, song.title) == ("hvsc", COMMANDO, "Commando")
    # and it is Modland's song
    assert song.key == "modland:HVSC/" + COMMANDO
    assert hosted.resolve("hvsc:" + COMMANDO) == song
    assert hosted.resolve(song.key).collection == "modland"
    assert hosted.lengths(song.key) == [235.594, 61.288, 6.0]


def test_modland_is_as_it_was_and_everything_shows_a_tune_once(hosted):
    assert hosted.song_count() == MODLAND_COUNT == hosted.song_count("modland")
    every = {f: n for f, n, _ in hosted.formats()}
    assert every["HVSC"] == HVSC_COUNT and "HVSC - Musicians" not in every
    assert [s.key for s in hosted.search("commando")] == ["modland:HVSC/" + COMMANDO]
    assert [s.collection for s in hosted.search("commando")] == ["modland"]
    assert [s.collection for s in hosted.search("commando", collection="hvsc")] == ["hvsc"]
    assert [s.collection for s in hosted.artist_songs("Rob Hubbard")] == ["modland"]
    for _ in range(30):
        assert hosted.random_song().collection == "modland"
    assert hosted.random_song(collection="hvsc").collection == "hvsc"


def test_hvsc_plays_by_the_sid_replayer():
    from bbmp.backends import backend_for, is_supported
    assert is_supported("HVSC - Musicians") and backend_for("HVSC - Games") == "sidplayfp"
    assert backend_for("Video Game Music - Sega Megadrive") == "gme"        # as before


def test_a_favorite_is_one_whichever_way_it_was_found(hosted, tmp_path, fetcher):
    lib = Library(tmp_path / "user.db")
    (ours,) = hosted.songs("HVSC - Musicians", "Rob Hubbard", "hvsc")
    theirs = hosted.by_path("HVSC/" + COMMANDO)
    lib.toggle_favorite(ours.key)
    assert lib.is_favorite(theirs.key)
    assert [s.collection for s in lib.favorites(hosted)] == ["modland"]
    assert fetcher.local_path(ours.key) == fetcher.cache_dir / "modland/HVSC" / COMMANDO
    keys = playlists.parse_m3u("https://hvsc.c64.org/download/C64Music/" + COMMANDO + "\n")
    assert playlists.resolve(keys, hosted) == (["modland:HVSC/" + COMMANDO], 0)


def test_the_player_shows_hvsc_as_a_collection(hosted, fetcher, tmp_path):
    async def go():
        settings = Settings.load(tmp_path / "settings.json")
        app = make_app(hosted, fetcher, tmp_path, settings=settings)
        async with app.run_test(size=(130, 40)) as pilot:
            assert await settle(pilot, lambda: "TFMX" in formats(app))
            assert "Modland" in formats_title(app)
            assert "HVSC" in formats(app) and "c" in footer_keys(app)
            await pilot.press("c")
            from test_tui import go_in
            await go_in(pilot, app, ("collection", "hvsc"), ("menu", "formats"))
            assert await settle(pilot, lambda: formats(app) == ["HVSC - Games", "HVSC - Musicians"])
            assert "HVSC" in formats_title(app)
            assert settings.collection == "hvsc"
            await pilot.press("slash")
            for ch in "commando":
                await pilot.press(ch)
            await pilot.press("enter")
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: [s.collection for s in table.songs] == ["hvsc"])
            assert "·" not in str(table.get_row_at(0)[-1])       # nothing to tell apart
            await pilot.press("f")
            assert await settle(pilot, lambda: app.library.favorite_keys()
                                == {"modland:HVSC/" + COMMANDO})
            await pilot.press("c")                                # Modland again: two in all
            assert await settle(pilot, lambda: settings.collection == "")
            assert await settle(pilot, lambda: "Modland" in formats_title(app))
            assert await settle(pilot, lambda: [s.collection for s in table.songs] == ["modland"])
            assert "♥" in str(table.get_row_at(0)[0])             # the same favorite
    asyncio.run(go())


# ---- mirrors ----

def _server(folder):
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=str(folder))
    handler.log_message = lambda *a: None
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server, f"http://127.0.0.1:{server.server_port}"


def test_the_next_server_is_asked_when_one_fails(tmp_path):
    import hashlib
    from bbmp.collection.transports import Below, Mirrors
    good, stale, partial = tmp_path / "good", tmp_path / "stale", tmp_path / "hvsc"
    for folder, song in ((good, b"the song"), (stale, b"an older version")):
        (folder / "AHX/Pink").mkdir(parents=True)
        (folder / "AHX/Pink/inside.ahx").write_bytes(song)
    (good / "HVSC/MUSICIANS").mkdir(parents=True)
    (good / "HVSC/MUSICIANS/tune.sid").write_bytes(b"a tune")
    (partial / "MUSICIANS").mkdir(parents=True)
    (partial / "MUSICIANS/tune.sid").write_bytes(b"a tune")
    (partial / "MUSICIANS/new.sid").write_bytes(b"only here")
    servers = [_server(f) for f in (good, stale, partial)]
    (s_good, u_good), (s_stale, u_stale), (s_hvsc, u_hvsc) = servers
    gone = "http://127.0.0.1:9"                      # nobody answers there
    dest = tmp_path / "got"

    def is_(data):
        return lambda file: hashlib.md5(file.read_bytes()).digest() == hashlib.md5(data).digest()
    try:
        mirrors = Mirrors([HttpTransport(gone), HttpTransport(u_stale), HttpTransport(u_good),
                           Below("HVSC", HttpTransport(u_hvsc))])
        # the first doesn't answer, the second has another file, the third is right
        mirrors.download("AHX/Pink/inside.ahx", dest, is_(b"the song"))
        assert dest.read_bytes() == b"the song" and mirrors.used == u_good
        assert not list(tmp_path.glob("*.other"))
        # and, the fastest to answer, it is the one that is asked first from now on; the
        # one that didn't answer is asked last
        order = mirrors.order()
        assert order[0].base == u_good and order[-1].base == gone
        # a server for a part of the collection has that part only
        mirrors.first = 3
        mirrors.download("HVSC/MUSICIANS/new.sid", dest, None)
        assert dest.read_bytes() == b"only here"
        mirrors.download("AHX/Pink/inside.ahx", dest, is_(b"the song"))
        assert dest.read_bytes() == b"the song"
        # no server has the file as the catalog has it, and they are plain http: what they
        # give may have been swapped on the way, and is not taken
        with pytest.raises(OSError, match="another file"):
            mirrors.download("AHX/Pink/inside.ahx", dest, is_(b"what the catalog remembers"))
        assert not dest.exists() and not list(tmp_path.glob("*.other"))
        # from https servers (these stand in for them): the catalog is old, the file is taken
        import bbmp.collection.transports as transports
        secure = transports._secure
        transports._secure = lambda t: True
        try:
            mirrors.download("AHX/Pink/inside.ahx", dest, is_(b"what the catalog remembers"))
        finally:
            transports._secure = secure
        assert dest.read_bytes() in (b"the song", b"an older version")
        assert not list(tmp_path.glob("*.other"))
        # nobody has it
        with pytest.raises(FileNotFoundError):
            mirrors.download("AHX/Pink/outside.ahx", dest)
        # nobody answers
        with pytest.raises(OSError, match="no server could be reached"):
            Mirrors([HttpTransport(gone), HttpTransport(gone)]).download("a/b", dest)
    finally:
        for server, _ in servers:
            server.shutdown()
            server.server_close()


def test_what_a_server_gives_is_checked_against_the_catalog(tmp_path, catalog, monkeypatch):
    import hashlib
    from bbmp.collection.transports import Mirrors
    wrong, right = tmp_path / "wrong", tmp_path / "right"
    for folder, song in ((wrong, b"something else"), (right, b"the song")):
        (folder / "Protracker/Tester").mkdir(parents=True)
        (folder / "Protracker/Tester/beep.mod").write_bytes(song)
    (s_wrong, u_wrong), (s_right, u_right) = _server(wrong), _server(right)
    modland = collections_.get("modland")
    monkeypatch.setattr(modland, "transport",
                        lambda: Mirrors([HttpTransport(u_wrong), HttpTransport(u_right)]))
    try:
        fetcher = Fetcher(tmp_path / "cache",
                          checksum=lambda key: hashlib.md5(b"the song").hexdigest())
        got = fetcher.get("modland:Protracker/Tester/beep.mod")
        assert got.read_bytes() == b"the song"
        left = [*(tmp_path / "cache").rglob("*.part"), *(tmp_path / "cache").rglob("*.other")]
        assert left == []
        fetcher.close()
    finally:
        for server in (s_wrong, s_right):
            server.shutdown()
            server.server_close()
    assert catalog.md5("modland:Protracker/Tester/beep.mod") == f"{0:032x}"
    assert catalog.md5("modland:Protracker/Nobody/none.mod") == ""


def test_http_keeps_its_connection_for_the_next_file(tmp_path):
    import http.server
    import threading

    from bbmp.collection.transports import HttpTransport

    connections = []

    class Server(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def setup(self):
            super().setup()
            connections.append(self.client_address)

        def do_GET(self):
            if self.path == "/files/moved.mod":
                self.send_response(302)
                self.send_header("Location", "/files/a%20song.mod")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = b"music " + self.path.encode() if "song" in self.path else b"no"
            self.send_response(200 if "song" in self.path else 404)
            self.send_header("Content-Length", str(len(body)))
            if "last" in self.path:
                self.send_header("Connection", "close")     # as a server does after a while
                self.close_connection = True
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Server)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        http_ = HttpTransport(f"http://127.0.0.1:{server.server_port}/files")
        dest = tmp_path / "f"
        http_.download("a song.mod", dest)
        assert dest.read_bytes() == b"music /files/a%20song.mod"
        with pytest.raises(FileNotFoundError):
            http_.download("not there.mod", dest)
        http_.download("song two.mod", dest)
        assert len(connections) == 1                        # one connection for the three
        http_.download("last song.mod", dest)               # the server closes it after this
        http_.download("song four.mod", dest)
        assert dest.read_bytes() == b"music /files/song%20four.mod" and len(connections) == 2
        http_.download("moved.mod", dest)                   # the server sends it elsewhere
        assert dest.read_bytes() == b"music /files/a%20song.mod"
        http_.close()
        server.shutdown()
        server.server_close()
        with pytest.raises(OSError):
            http_.download("a song.mod", dest)
    finally:
        server.shutdown()
        server.server_close()


def test_browse_says_which_collection_a_song_is_from(two, fetcher, tmp_path):
    async def go():
        app = make_app(two, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            from test_tui import browse_ready, go_in, put
            await browse_ready(pilot, app)
            await go_in(pilot, app, ("format", "Protracker"))
            put(app._levels[-1], ("artist", "Tester"))
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: table.row_count >= 3)
            albums = {str(table.get_row_at(i)[1]): str(table.get_row_at(i)[-1])
                      for i in range(table.row_count)}
            assert albums["only here"] == "Disk One · Chipland"
            assert albums["beep two"] == "Modland"
    asyncio.run(go())


def test_a_shared_song_says_every_collection_that_has_it(two, fetcher, tmp_path):
    """Modland and Chipland have beep.mod as the same file: one row, both named."""
    (beep,) = [s for s in two.search("beep") if s.title == "beep"]
    assert beep.collection == "modland" and beep.also == ("chipland",)
    assert [s.also for s in two.search("beep", collection="modland") if s.title == "beep"] == [()]

    async def go():
        app = make_app(two, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("slash", *"beep", "enter")
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: table.row_count >= 1)
            cells = [str(table.get_row_at(i)[-1]) for i in range(table.row_count)]
            assert "Protracker · Modland + Chipland" in cells
    asyncio.run(go())


def test_a_tune_hvsc_has_says_so_under_modland(hosted):
    (commando,) = [s for s in hosted.search("commando")]
    assert commando.collection == "modland" and commando.also == ("hvsc",)


def test_a_shared_song_opens_to_its_copies(two, fetcher, tmp_path):
    """→ on a song more than one collection has shows each, ← closes; Enter plays the
    copy chosen."""
    async def go():
        app = make_app(two, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            await pilot.press("slash", *"beep", "enter")
            table = app.query_one("#search-songs")
            assert await settle(pilot, lambda: table.row_count >= 2)
            row = next(i for i, s in enumerate(table.songs) if s.title == "beep" and s.also)
            table.move_cursor(row=row)
            assert str(table.get_row_at(row)[1]).startswith("▸ beep")
            n = table.row_count
            await pilot.press("right")
            assert await settle(pilot, lambda: table.row_count == n + 1)
            assert str(table.get_row_at(row)[1]).startswith("▾ beep")
            copy = table.songs[row + 1]
            assert copy.key == "chipland:tunes/Tester/beep.mod"
            assert str(table.get_row_at(row + 1)[1]).startswith("  ↳ beep")
            assert "Chipland" in str(table.get_row_at(row + 1)[-1])
            assert table.cursor_row == row + 1
            await pilot.press("enter")                       # plays Chipland's copy, then on
            assert await settle(pilot, lambda: app.player.queue()[0] != [])
            queue = [s.key for s in app.player.queue()[0]]
            assert queue[0] == copy.key and queue.count(copy.key) == 1
            assert "modland:Protracker/Tester/beep.mod" not in queue   # the row it came from
            await pilot.press("left")                        # from the copy: closes
            assert await settle(pilot, lambda: table.row_count == n)
            assert table.cursor_row == row
    asyncio.run(go())


def test_offline_a_song_is_not_taken_for_one_no_server_has(tmp_path):
    # Modland's servers don't answer, and HVSC's (which have only HVSC/) can't have the
    # song: that is "offline", not "no server has it" (which marks the song as failing)
    from bbmp.collection.transports import Below, HttpTransport, Mirrors
    gone = "http://127.0.0.1:9/"
    mirrors = Mirrors([HttpTransport(gone), Below("HVSC", HttpTransport(gone))])
    with pytest.raises(OSError) as e:
        mirrors.download("Protracker/Tester/beep.mod", tmp_path / "x")
    assert not isinstance(e.value, FileNotFoundError)


def test_in_the_queue_an_opened_song_does_not_shift_the_others(two, fetcher, tmp_path):
    async def go():
        app = make_app(two, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            (beep,) = [s for s in two.search("beep") if s.title == "beep"]
            other = next(s for s in two.search("beep") if s.title != "beep")
            app.player.play_list([beep, other])
            # not "playing": without real time, beep may be over before the first look
            assert await settle(pilot, lambda: app.player.status().song is not None)
            await pilot.press("3")
            table = app.query_one("#queue-songs")
            assert await settle(pilot, lambda: table.row_count == 2)
            table.focus()
            table.move_cursor(row=0)
            await pilot.press("right")
            assert await settle(pilot, lambda: table.row_count == 3)
            table.move_cursor(row=2)                          # the second song of the queue
            await pilot.press("enter")
            assert await settle(pilot, lambda: app.player.status().queue_pos == 1)
            assert app.player.status().song.key == other.key
            table.move_cursor(row=1)                          # beep's copy: plays in its place
            copy = table.songs[1]
            assert copy.collection == "chipland"
            await pilot.press("enter")
            assert await settle(pilot, lambda: [s.key for s in app.player.queue()[0]]
                                == [copy.key, other.key])
    asyncio.run(go())


def test_browse_by_platform_keeps_each_collections_layout(two, fetcher, tmp_path, monkeypatch):
    from bbmp import platforms
    monkeypatch.setattr(platforms, "_table", lambda: {
        "Protracker": (("amiga",), ("paula",)), "TFMX": (("amiga",), ("paula",)),
        "Fasttracker 2": (("pc",), ())})

    async def go():
        app = make_app(two, fetcher, tmp_path)
        async with app.run_test(size=(130, 44)) as pilot:
            from test_tui import browse_ready, formats_list
            await browse_ready(pilot, app)
            await pilot.press("b")                          # the platforms
            assert await settle(pilot, lambda: app._placed and app._path == [
                ("collection", ""), ("menu", "platforms")])
            platforms_ = app._levels[-1]
            names = [str(platforms_.get_option_at_index(i).prompt) for i in range(platforms_.option_count)]
            assert names[0].startswith("Amiga")
            await pilot.press("enter")                      # Amiga
            formats = formats_list(app)
            assert await settle(pilot, lambda: formats.option_count >= 4)
            ids = [(formats.get_option_at_index(i).id or "").split("\x1e")[-1] or None
                   for i in range(formats.option_count)]
            # a heading for each collection, and its formats on the Amiga under it
            heads = [i for i, x in enumerate(ids) if x is None]
            groups = [ids[a + 1:b] for a, b in zip(heads, heads[1:] + [len(ids)])]
            assert sorted(groups) == [["chipland\x1fProtracker"],
                                      ["modland\x1fProtracker", "modland\x1fTFMX"]]
            assert "Amiga" in str(formats.border_title)
            formats.highlighted = ids.index("chipland\x1fProtracker")
            await pilot.pause(0.05)
            await pilot.press("enter")                      # its artists
            table = app.query_one("#browse-songs")
            assert await settle(pilot, lambda: table.row_count >= 1)
            assert {s.collection for s in table.songs} == {"chipland"}
            assert app._view() == ("platform", "amiga")
            app._search("beep")                             # a search keeps to the view
            found = app.query_one("#search-songs")
            assert await settle(pilot, lambda: "Amiga" in str(found.border_title))
            assert found.songs and {s.format for s in found.songs} <= {"Protracker", "TFMX"}
            await pilot.press("1", "left", "left", "left", "left")  # out, to the first list
            assert await settle(pilot, lambda: app._path == [])
            assert app._view() is None
    from bbmp.tui.widgets import NEW
    asyncio.run(go())


def test_search_takes_platform_and_chip(two, monkeypatch):
    from bbmp import platforms
    monkeypatch.setattr(platforms, "_table", lambda: {
        "Protracker": (("amiga",), ("paula",)), "TFMX": (("amiga",), ("paula",))})
    assert {s.format for s in two.search("platform:amiga")} == {"Protracker", "TFMX"}
    assert {s.title for s in two.search("beep chip:paula")} >= {"beep", "beep two"}
    assert two.search("beep platform:genesis") == []           # none on the Mega Drive
    assert two.search("platform:nowhere") == []                 # no such platform
    # a file two collections share is one row, and counts twice
    assert two.search_count("platform:amiga") >= len(two.search("platform:amiga"))
    assert all(s.format != "Protracker" for s in two.search("tester -platform:amiga"))


def test_radio_keeps_to_the_view(two, monkeypatch):
    from bbmp import platforms
    from bbmp.player import Radio
    monkeypatch.setattr(platforms, "_table", lambda: {"TFMX": (("amiga",), ("paula",))})
    songs = {two.random_song(view=("platform", "amiga")).format for _ in range(20)}
    assert songs == {"TFMX"}
    assert two.random_song(view=("platform", "nes")) is None
    assert Radio(view=("platform", "amiga")).describe() == "Amiga"
    assert Radio(collection="modland", view=("chip", "sid8580")).describe().startswith(
        "SID 8580")


def test_an_ftp_server_that_is_busy_is_not_taken_for_one_without_the_file():
    import ftplib
    from bbmp.collection.transports import FtpTransport
    t = FtpTransport("ftp.example.org", "/pub")
    t._connect = lambda: None
    t._drop = lambda: None

    def busy(con):
        raise ftplib.error_perm("530 Too many connections")

    def gone(con):
        raise ftplib.error_perm("550 No such file")
    with pytest.raises(OSError) as e:
        t._retry(busy, "a/b.mod")
    assert not isinstance(e.value, FileNotFoundError)       # not "the song is broken"
    with pytest.raises(FileNotFoundError):
        t._retry(gone, "a/b.mod")


def test_a_home_mirror_is_asked_first_with_each_collections_layout(monkeypatch):
    from bbmp.collection.asma import Asma
    from bbmp.collection.keygenmusic import KeygenMusic
    from bbmp.collection.sndh import Sndh
    from bbmp.collection.zxart import ZxArt
    assert collections_.get("modland").transport().home is None     # none set: as before
    monkeypatch.setenv("BBMP_MIRROR", "192.168.1.2:8088/")
    want = {"modland": "http://192.168.1.2:8088/modland/pub/modules",
            "asma": "http://192.168.1.2:8088/asma/asma",
            "sndh": "http://192.168.1.2:8088/sndh/sndh/sndh_lf",
            "keygenmusic": "http://192.168.1.2:8088/keygenmusic/kgm"}
    for c in (collections_.get("modland"), Asma(), Sndh(), KeygenMusic()):
        mirrors = c.transport()
        assert mirrors.home is mirrors.transports[0] and str(mirrors.home) == want[c.id]
        assert mirrors.order()[0] is mirrors.home
        mirrors.first = 2           # a clone that asks another server first: the home one still
        assert mirrors.order()[0] is mirrors.home
    assert ZxArt().transport().home is None          # ZX-Art can't be mirrored
    # the setting, when the environment doesn't say
    monkeypatch.delenv("BBMP_MIRROR")
    from bbmp import settings
    monkeypatch.setattr(settings.Settings, "load",
                        classmethod(lambda cls, path=None: cls(mirror="http://nas:1")))
    assert str(Asma().transport().home) == "http://nas:1/asma/asma"


def test_a_home_mirror_that_cannot_be_reached_is_waited_for_once(tmp_path):
    from bbmp.collection.transports import Mirrors

    class Silent:
        calls = 0
        patience = 3.0

        def download(self, path, dest):
            Silent.calls += 1
            raise TimeoutError("timed out")

        def close(self):
            pass

        def __str__(self):
            return "http://silent-home-mirror.test"

    class Public(Silent):
        def download(self, path, dest):
            dest.write_bytes(b"song")

        def __str__(self):
            return "https://public.test"

    class Other(Silent):            # the same home mirror, for another collection
        def __str__(self):
            return "http://silent-home-mirror.test/other"

    for kind in (Silent, Silent, Other):     # three songs, each on a new connection
        mirrors = Mirrors([kind(), Public()])
        mirrors.home = mirrors.transports[0]
        mirrors.download("a/b.mod", tmp_path / "x")
        assert (tmp_path / "x").read_bytes() == b"song"
    assert Silent.calls == 1
