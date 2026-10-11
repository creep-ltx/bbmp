# SPDX-License-Identifier: GPL-3.0-or-later
"""The engine's speed fixes of 11 October 2026: each asks less of the catalogs, the
cache or the replayers' processes, and must give the same answers as before."""

import sqlite3

from conftest import modland_listing
from bbmp import index as index_mod


# ---- checksums: catalogs without them are not asked ----

def test_resolve_does_not_ask_a_catalog_without_checksums(tmp_path, chipland, monkeypatch):
    monkeypatch.setattr(type(chipland), "MD5", {})          # Chipland publishes none
    db = tmp_path / "index.db"
    index_mod.build({"modland": modland_listing(), "chipland": chipland.listing()}, db)
    ix = index_mod.Index(db)
    asked = []
    ix.con.set_trace_callback(asked.append)
    song = ix.resolve("modland:Protracker/Tester/beep.mod")
    assert song is not None and song.also == ()
    chipland_schema = ix._schema("chipland")
    assert not [q for q in asked if f"{chipland_schema}.files WHERE md5" in q]
    assert ix._has_md5("main") and not ix._has_md5(chipland_schema)


def test_resolve_still_finds_the_same_file_in_a_catalog_with_checksums(two):
    assert two.resolve("modland:Protracker/Tester/beep.mod").also == ("chipland",)


# ---- the chips joined from their own side ----

def test_the_chips_join_gives_what_the_old_one_did(catalog):
    catalog.close()
    con = sqlite3.connect(catalog._catalog_files[0])
    paths = [p for (p,) in con.execute("SELECT path FROM files WHERE hidden = 0 LIMIT 6")]
    con.executemany("INSERT INTO chips VALUES ('modland', ?, ?)",
                    [(p, chip) for i, p in enumerate(paths)
                     for chip in (["sid6581", "platform:c64"] if i % 2 else ["ym2149"])])
    con.commit()
    old = ("SELECT f.format, s.chip, count(*) FROM chips s JOIN files f ON f.path = s.path"
           " WHERE f.hidden = 0 GROUP BY f.format, s.chip")
    new = ("SELECT f.format, s.chip, count(*) FROM chips s CROSS JOIN files f"
           " ON f.collection = s.collection AND f.path = s.path WHERE f.hidden = 0"
           " GROUP BY f.format, s.chip")
    assert sorted(con.execute(old)) == sorted(con.execute(new)) and con.execute(new).fetchall()
    con.close()


# ---- counts kept beside the catalogs ----

def test_the_artists_of_each_format_are_kept_beside_the_catalogs(catalog):
    import json
    kept = catalog.path / "counts.json"
    data = json.loads(kept.read_text())
    key = "format-artists modland"
    assert dict(data["counts"][key]) == {f: a for f, _, a in catalog.formats("")}
    # a new Index takes them from the file, not from the catalogs
    data["counts"][key] = [[f, 999] for f, _ in data["counts"][key]]
    kept.write_text(json.dumps(data))
    assert {a for _, _, a in index_mod.Index(catalog.path).formats("")} == {999}
    # and counts again when a catalog is made anew
    index_mod.build({"modland": modland_listing()}, catalog.path)
    again = index_mod.Index(catalog.path)
    assert 999 not in {a for _, _, a in again.formats("")}
    assert json.loads(kept.read_text())["stamp"] == again._stamp


def test_the_platform_counts_are_kept_and_made_again_with_the_catalogs(catalog):
    import json
    catalog.close()
    con = sqlite3.connect(catalog._catalog_files[0])
    path = con.execute("SELECT path FROM files WHERE format = 'Protracker' AND hidden = 0").fetchone()[0]
    con.execute("INSERT INTO chips VALUES ('modland', ?, 'sid6581')", (path,))
    con.commit()
    con.close()
    ix = index_mod.Index(catalog.path)          # the file changed: counted anew
    assert ix._chip_counts("modland") == [("Protracker", "sid6581", 1)]
    kept = json.loads((catalog.path / "counts.json").read_text())
    assert kept["counts"]["chip-counts modland"] == [["Protracker", "sid6581", 1]]
    assert ix.view_counts("modland", "chip") == index_mod.Index(catalog.path).view_counts("modland", "chip")


def test_counts_of_catalogs_that_were_replaced_are_not_kept(catalog):
    import json
    before = json.loads((catalog.path / "counts.json").read_text())
    index_mod.build({"modland": modland_listing()}, catalog.path)
    stamp = json.loads((catalog.path / "counts.json").read_text())["stamp"]
    assert stamp != before["stamp"]
    catalog._kept("something", lambda: 1)       # the old Index counts, of its old files
    assert json.loads((catalog.path / "counts.json").read_text())["stamp"] == stamp


# ---- more than one connection ----

def test_a_question_does_not_wait_for_another_threads_long_one(catalog):
    import threading
    busy = catalog._take()                      # as a long query holds it
    got = []
    t = threading.Thread(target=lambda: got.append(catalog.lengths(
        "modland:HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")))
    t.start()
    t.join(5)
    assert not t.is_alive() and got and got[0][0] > 200
    assert len(catalog._cons) == 2
    # the temp table and views are in the new connection too
    assert catalog.formats("") and catalog.search("beep")
    catalog._give(busy)


def test_no_more_connections_than_the_most(catalog):
    import threading
    held = [catalog._take() for _ in range(catalog.CONNECTIONS)]
    assert len(set(map(id, held))) == catalog.CONNECTIONS
    got = []
    t = threading.Thread(target=lambda: got.append(catalog.song_count()))
    t.start()
    t.join(0.3)
    assert t.is_alive()                         # it waits for one to be let go
    catalog._give(held.pop())
    t.join(5)
    assert got and len(catalog._cons) == catalog.CONNECTIONS
    for con in held:
        catalog._give(con)


def test_no_new_connection_to_catalogs_that_were_replaced(catalog):
    import threading
    busy = catalog._take()
    index_mod.build({"modland": modland_listing()}, catalog.path)
    got = []
    t = threading.Thread(target=lambda: got.append(catalog.song_count()))
    t.start()
    t.join(0.3)
    assert t.is_alive() and len(catalog._cons) == 1     # it waits for the one there is
    catalog._give(busy)
    t.join(5)
    assert got


def test_a_closed_index_answers_nothing(catalog):
    import pytest
    catalog._give(catalog._take())
    catalog.close()
    with pytest.raises(sqlite3.ProgrammingError):
        catalog.formats("")
    catalog.close()


# ---- a library replayer's process, closed without waiting ----

def _remote_of(script: str):
    import subprocess
    import sys
    from bbmp.backends.remote import Remote
    r = Remote.__new__(Remote)
    r.backend = "test"
    r._proc = subprocess.Popen([sys.executable, "-c", script], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE)
    return r, r._proc


def _gone(proc, seconds: float = 5) -> bool:
    """Ended and reaped by the reaper: no zombie left (/proc has it no more)."""
    import os
    import time
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if proc.returncode is not None and not os.path.exists(f"/proc/{proc.pid}"):
            return True
        time.sleep(0.02)
    return False


def test_a_remote_replayer_is_closed_without_waiting_and_reaped():
    import time
    # it reads the close, sleeps a little as a library's close might, and ends
    r, proc = _remote_of("import sys, time; sys.stdin.readline(); time.sleep(0.3)")
    t0 = time.perf_counter()
    r.close()
    assert time.perf_counter() - t0 < 0.1 and r._proc is None
    assert _gone(proc) and proc.returncode == 0
    r.close()                                   # again: nothing


def test_a_remote_replayer_that_does_not_end_is_killed_and_reaped(monkeypatch):
    from bbmp.backends import remote
    monkeypatch.setattr(remote, "REAP_WAIT", 0.2)
    r, proc = _remote_of("import signal, time; signal.signal(signal.SIGPIPE, signal.SIG_IGN);"
                         " time.sleep(60)")
    r.close()
    assert _gone(proc) and proc.returncode < 0      # killed


def test_the_replayers_process_imports_little():
    """Not the collections' downloading code (urllib, http, ssl, email), nor the
    helper programs' side, nor the sandbox."""
    import subprocess
    import sys
    from pathlib import Path
    src = Path(__file__).resolve().parents[1] / "src"
    out = subprocess.run(
        [sys.executable, "-c", "import sys, bbmp.backends.remote;"
         " from bbmp.backends import LOCAL_OPENERS; print(' '.join(sys.modules))"],
        capture_output=True, text=True, env={"PYTHONPATH": str(src), "BBMP_REMOTE": "1"},
        check=True).stdout.split()
    for heavy in ("urllib.request", "http.client", "ssl", "email.utils", "bbmp.collection",
                  "bbmp.backends.pipe", "bbmp.sandbox", "bbmp.downloads", "concurrent.futures"):
        assert heavy not in out, heavy


def test_the_package_still_offers_what_it_did():
    from bbmp import backends
    assert callable(backends.left_out) and "openmpt" in backends.OPENERS
    assert backends.OPENERS is backends.OPENERS and backends.HELPERS and backends.OWN
    from bbmp.collection.modland import SUBFORMAT_DIRS
    assert "Video Game Music" in SUBFORMAT_DIRS


# ---- the cache: trimmed with room to spare, walked by scandir ----

def _cached(tmp_path, n: int, size: int = 1000):
    import os
    import time
    from bbmp.fetch import Fetcher
    f = Fetcher(tmp_path / "cache", kept_dir=tmp_path / "kept")
    old = time.time() - 3600
    for i in range(n):
        p = f.cache_dir / "modland" / f"Format {i % 3}" / f"Artist {i}" / f"song{i}.mod"
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"x" * size)
        os.utime(p, (old + i, old + i))         # the first is the least recently used
    (f.cache_dir / "modland" / "Format 0" / "half.mod.part").write_bytes(b"x" * 5)
    return f


def test_a_trim_leaves_room_under_the_limit(tmp_path):
    from bbmp import fetch
    f = _cached(tmp_path, 20)
    freed = f.trim(15_000)
    assert 20_000 - freed <= 15_000 * fetch.TRIM_TO and freed == 7000     # 13 files left
    assert not f.over_limit(15_000)
    left = sorted(p.name for p in f._files())
    assert left == sorted(f"song{i}.mod" for i in range(7, 20))
    # the folders of the files that went are gone, the others stay
    assert not (f.cache_dir / "modland" / "Format 0" / "Artist 0").exists()
    assert (f.cache_dir / "modland" / "Format 0" / "half.mod.part").exists()


def test_the_cache_is_measured_as_before(tmp_path):
    import os
    f = _cached(tmp_path, 12)
    (f.cache_dir / "stray").write_bytes(b"in the cache's own folder: not a song's")
    os.symlink(f.cache_dir / "modland", f.cache_dir / "modland" / "loop")
    before = []
    for p in f.cache_dir.rglob("*"):
        if p.is_file() and not p.name.endswith(".part") and p.parent != f.cache_dir:
            st = p.stat()
            before.append((st.st_mtime, st.st_size, p))
    assert sorted(f._measured()) == sorted(before) and len(before) == 12
    assert f.cache_size("modland") == 12_000 and f.cache_size() == 12_000


# ---- the catalog update in a process of its own ----

WORK = '''
import os
def double(progress, n):
    progress("halfway")
    print("what a library prints doesn't break the answers")
    progress("done")
    return {"n": n * 2, "pid": os.getpid()}
def unreachable(progress):
    raise ConnectionError("the server didn't answer")
def broken(progress):
    raise ValueError("a bug")
def dies(progress):
    os._exit(3)
'''


def test_work_is_done_in_a_process_of_its_own(tmp_path, monkeypatch):
    import os
    import pytest
    from bbmp import apart
    (tmp_path / "somework.py").write_text(WORK)
    monkeypatch.setenv("PYTHONPATH", str(tmp_path))
    said = []
    got = apart.run("somework:double", {"n": 21}, said.append)
    assert got["n"] == 42 and got["pid"] != os.getpid() and said == ["halfway", "done"]
    with pytest.raises(OSError, match="didn't answer"):
        apart.run("somework:unreachable", {})
    with pytest.raises(apart.Failed, match="a bug"):
        apart.run("somework:broken", {})
    with pytest.raises(OSError, match="without an answer"):
        apart.run("somework:dies", {})


def test_an_update_is_built_apart_and_says_what_it_did(tmp_path, monkeypatch):
    """The real catalogs.update, with the building done by a stand-in for the child
    (which the tests can't reach with their stand-in servers)."""
    from bbmp import apart, catalogs
    from bbmp.settings import Settings
    monkeypatch.setattr(catalogs, "APART", True)
    asked = {}

    def run(target, kwargs, progress):
        asked.update(target=target, **kwargs)
        progress("Building the catalog of Modland...")
        return {"songs": 7, "failed": {"hvsc": "down"}}
    monkeypatch.setattr(apart, "run", run)
    folder = tmp_path / "catalogs"
    s = Settings()
    s.collections = ["modland", "hvsc"]
    said = []
    done = catalogs.update(s, said.append, folder=folder)
    assert done.songs == 7 and done.failed == {"hvsc": "down"} and said
    assert asked["target"] == "bbmp.catalogs:_build_here" and asked["folder"] == str(folder)
    assert set(asked["only"]) == {"modland", "hvsc"}


def test_an_update_is_built_here_where_no_process_can_be_started(tmp_path, monkeypatch):
    from bbmp import apart, catalogs
    monkeypatch.setattr(catalogs, "APART", True)

    def cannot(*a, **k):
        raise apart.NotStarted("no python")
    monkeypatch.setattr(apart, "run", cannot)
    folder = tmp_path / "catalogs"
    failed = {}
    assert catalogs._build(lambda s: None, ["modland"], folder, ["modland"], failed) > 0
    assert (folder / "modland.db").is_file() and failed == {}


def test_entries_have_no_dict():
    from bbmp.collection.base import Entry
    assert not hasattr(Entry("a", "b", "c", "d", "e"), "__dict__")


# ---- an artist's songs, by the coop folders' index ----

OLD_ARTIST_SONGS = ("SELECT id, path, format, artist, album, name, collection, size, md5 FROM files"
                    " WHERE hidden = 0 AND (artist = ? OR album = ? OR album LIKE ? ESCAPE '\\')"
                    " ORDER BY id")


def _old_artist_songs(ix, artist):
    args = (artist, f"coop-{artist}", "coop-" + index_mod._like(artist) + "/%")
    songs = ix._songs(OLD_ARTIST_SONGS, args, once=True)
    return sorted(songs, key=lambda s: (s.format.lower(), s.album_name.lower(), s.title.lower()))


def test_an_artists_songs_are_as_they_were_with_the_coop_index_and_without(two):
    assert two._coop == {"main", "c1"}
    for artist in ("Tester", "Other Guy", "tester", "Stranger", "Nobody", "Test_r", "Tes%"):
        assert two.artist_songs(artist) == _old_artist_songs(two, artist), artist
    assert "Protracker/Other Guy/coop-Tester/duet.mod" in [s.path for s in two.artist_songs("Tester")]
    # a catalog made before the index: asked as before
    two.close()
    for f in two._catalog_files:
        con = sqlite3.connect(f)
        con.execute(f"DROP INDEX {index_mod.COOP_INDEX}")
        con.commit()
        con.close()
    old = index_mod.Index(two.path)
    assert old._coop == set()
    for artist in ("Tester", "Other Guy", "Stranger"):
        assert old.artist_songs(artist) == _old_artist_songs(old, artist), artist


# ---- small ones ----

def test_files_in_asks_the_collections_own_catalog_and_finds_what_the_view_did(two):
    old = "SELECT path, md5, size, hidden FROM files WHERE collection = ?{} ORDER BY id"
    for c, extra, args in (("modland", "", ()), ("chipland", "", ()),
                           ("modland", " AND format = ?", ("Protracker",)),
                           ("modland", " AND format = ? AND artist = ?", ("TFMX", "Tester"))):
        want = [(p, m if len(m) == 32 else "", s, h)
                for p, m, s, h in two._all(old.format(extra), (c, *args))]
        got = two.files_in(c, *args)
        assert got == want and got, (c, args)
    assert two.files_in("nowhere") == []


def test_a_radio_on_a_view_picks_its_songs_by_id(catalog):
    from array import array
    seen = set()
    for _ in range(40):
        song = catalog.random_song(view=("platform", "amiga"))
        assert song is not None and song == catalog.by_path(song.path, song.collection)
        seen.add(song.format)
    (choices,) = catalog._radio_choices.values()
    assert isinstance(choices, array) and choices.typecode == "q"
    want = {f for f, _ in catalog.view_formats("modland", "platform", "amiga")}
    assert seen <= want and len(seen) > 1
    assert catalog._by_id(10 ** 9) is None and catalog._by_id(63) is None


def test_exports_silence_check_and_fade_are_as_they_were():
    import array
    import random
    from bbmp import ending, export
    rnd = random.Random(5)
    for n in (0, 2, 4800 * 2):
        samples = array.array("h", (rnd.randint(-32768, 32767) for _ in range(n)))
        pcm = samples.tobytes()
        assert export._peak(pcm) == ending.peak(array.array("h", pcm))
        for gain in (0.0, 0.33, 0.998):
            import numpy as np
            fast = (np.frombuffer(pcm, np.int16) * gain).astype(np.int16).tobytes()
            assert fast == array.array("h", (int(x * gain) for x in samples)).tobytes()
    assert export._peak(array.array("h", [-32768, 5]).tobytes()) == 1.0


def test_mpris_imports_pygobject_in_its_own_thread(monkeypatch):
    import sys
    from bbmp import mpris
    monkeypatch.setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nowhere")
    monkeypatch.delitem(sys.modules, "gi", raising=False)
    m = mpris.Mpris(player=None, settings=None)
    ran = []
    monkeypatch.setattr(mpris.Mpris, "_run", lambda self: ran.append(True))
    import importlib.util
    has_gi = importlib.util.find_spec("gi") is not None
    assert m.start() == has_gi
    if m._thread is not None:
        m._thread.join(2)
    assert "gi" not in sys.modules and ran == ([True] if has_gi else [])
