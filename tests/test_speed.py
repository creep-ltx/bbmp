# SPDX-License-Identifier: GPL-3.0-or-later
"""What makes a song start sooner: the fastest mirror first, a silent one passed over
quickly, companion files fetched side by side, a song made ready once, user.db in
write-ahead mode."""

import socket
import sqlite3
import threading
import time
from pathlib import Path

import pytest

from bbmp import collection as collections_
from bbmp.audio import NullOutput
from bbmp.backends.base import RATE, Decoder, TrackInfo
from bbmp.collection import Collection, Transport
from bbmp.collection import transports as tr
from bbmp.collection.base import Entry, Listing
from bbmp.collection.transports import HttpTransport, Mirrors
from bbmp.fetch import Fetcher
from bbmp.library import Library
from bbmp.player import Player
from bbmp.settings import Settings


# ---- mirrors ----

def test_modland_asks_the_fastest_mirror_first_before_any_is_measured():
    order = collections_.get("modland").transport().order()
    assert str(order[0]).startswith("https://modland.antarctica.no")


class Timed(Transport):
    def __init__(self, name, waited, has=True):
        self.name, self.waited_for, self.has = name, waited, has
        self.waited = None
        self.asked = 0

    def download(self, path, dest):
        self.asked += 1
        self.waited = self.waited_for
        if not self.has:
            raise FileNotFoundError(path)
        dest.write_bytes(self.name.encode())

    def __str__(self):
        return f"timed://{self.name}-{id(self)}"


def test_the_fastest_lately_is_asked_first(tmp_path):
    slow, fast = Timed("slow", 4.0), Timed("fast", 0.1)
    mirrors = Mirrors([slow, fast])
    mirrors.download("a", tmp_path / "a")           # slow is asked first: nothing known yet
    assert (tmp_path / "a").read_bytes() == b"slow"
    tr._note_speed(str(fast), 0.1)                  # heard from elsewhere (another file)
    mirrors.download("b", tmp_path / "b")
    assert (tmp_path / "b").read_bytes() == b"fast" and mirrors.order()[0] is fast
    # a clone told to ask another server first does so
    mirrors.first = 0
    assert mirrors.order()[0] is slow


def _silent_server(answer_after: float | None):
    """A server that takes connections and says nothing (or answers late)."""
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    sock.listen()
    held = []

    def serve():
        while True:
            try:
                con, _ = sock.accept()
            except OSError:
                return
            held.append(con)
            if answer_after is not None:
                def late(c=con):
                    time.sleep(answer_after)
                    try:
                        c.recv(65536)
                        c.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\nlate")
                    except OSError:
                        pass
                threading.Thread(target=late, daemon=True).start()
    threading.Thread(target=serve, daemon=True).start()
    return sock, f"http://127.0.0.1:{sock.getsockname()[1]}", held


def test_a_silent_mirror_is_passed_over_quickly(tmp_path, monkeypatch):
    import http.server
    monkeypatch.setattr(tr, "FIRST_BYTE", 0.3)
    (tmp_path / "srv").mkdir()
    (tmp_path / "srv/song.mod").write_bytes(b"the song")
    handler = lambda *a, **k: http.server.SimpleHTTPRequestHandler(   # noqa: E731
        *a, directory=str(tmp_path / "srv"), **k)
    good = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    good.RequestHandlerClass.log_message = lambda *a: None
    threading.Thread(target=good.serve_forever, daemon=True).start()
    sock, silent, _ = _silent_server(None)
    try:
        mirrors = Mirrors([HttpTransport(silent), HttpTransport(f"http://127.0.0.1:{good.server_port}")])
        began = time.monotonic()
        mirrors.download("song.mod", tmp_path / "got")
        assert time.monotonic() - began < 1.5               # not the thirty seconds of before
        assert (tmp_path / "got").read_bytes() == b"the song"
        assert mirrors.order()[-1].base == silent           # and is asked last from now on
    finally:
        sock.close()
        good.shutdown()
        good.server_close()


def test_when_no_mirror_answers_quickly_they_are_given_longer(tmp_path, monkeypatch):
    monkeypatch.setattr(tr, "FIRST_BYTE", 0.2)
    sock, late, _ = _silent_server(0.6)
    try:
        Mirrors([HttpTransport(late)]).download("x", tmp_path / "got")
        assert (tmp_path / "got").read_bytes() == b"late"
    finally:
        sock.close()


# ---- companion files ----

class Slow(Transport):
    """Every file takes a moment; counts how many are fetched at once."""
    now = most = 0
    lock = threading.Lock()
    asked: list[str] = []

    def download(self, path, dest):
        with Slow.lock:
            Slow.now += 1
            Slow.most = max(Slow.most, Slow.now)
            Slow.asked.append(path)
        time.sleep(0.15)
        dest.write_bytes(path.encode())
        with Slow.lock:
            Slow.now -= 1


class Slowland(Collection):
    id = "slowland"
    name = "Slowland"

    def parse(self, path):
        return Entry(path, "Protracker", "A", "", path)

    def listing(self, progress=lambda s: None):
        return Listing([])

    def transport(self):
        return Slow()


@pytest.fixture
def slowland():
    Slow.now = Slow.most = 0
    Slow.asked = []
    source = collections_.register(Slowland())
    yield source
    collections_.unregister(source.id)


def test_companion_files_are_fetched_four_at_a_time(tmp_path, slowland):
    fetcher = Fetcher(tmp_path / "cache", kept_dir=tmp_path / "kept")
    companions = [f"slowland:song/smp{i}.ins" for i in range(12)]
    began = time.monotonic()
    got = fetcher.get("slowland:song/song.mod", companions)
    took = time.monotonic() - began
    assert got.read_bytes() == b"song/song.mod"
    assert all((tmp_path / f"cache/slowland/song/smp{i}.ins").exists() for i in range(12))
    assert Slow.most == 4
    assert took < 13 * 0.15 / 2                     # one at a time took 13 x 0.15 s
    fetcher.close()


def test_two_fetchers_that_want_one_file_download_it_once(tmp_path, slowland):
    fetcher = Fetcher(tmp_path / "cache", kept_dir=tmp_path / "kept")
    other = fetcher.clone()
    threads = [threading.Thread(target=f.get, args=("slowland:a/b.mod",)) for f in (fetcher, other)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert Slow.asked == ["a/b.mod"]


def test_a_cache_that_cant_be_trimmed_is_not_looked_through_after_every_song(tmp_path, slowland):
    fetcher = Fetcher(tmp_path / "cache", kept_dir=tmp_path / "kept")
    fetcher.get("slowland:a/b.mod")
    limit = 5                                         # bytes: the one file is over it
    assert fetcher.over_limit(limit)
    fetcher.trim(limit, keep={"slowland:a/b.mod"})    # it is kept: nothing goes
    assert not fetcher.over_limit(limit)
    fetcher._added = 30_000_000                       # until much more has come
    assert fetcher.over_limit(limit)


# ---- the player ----

class Tone(Decoder):
    backend = "fake"

    def __init__(self, seconds=3.0):
        self.info = TrackInfo(subsongs=1, duration=seconds)
        self.left = int(seconds * RATE)
        self.closed = False

    def read(self, frames):
        n = min(frames, self.left)
        self.left -= n
        return (8000).to_bytes(2, "little", signed=True) * 2 * n

    def close(self):
        self.closed = True


def test_a_song_being_made_ready_is_waited_for_not_started_again(catalog, fetcher, monkeypatch):
    opened = []

    def slow_open(path, fmt, lengths=(), local=False):
        if opened:
            time.sleep(0.8)                 # the next song takes its time to open
        opened.append(path.name)
        return Tone(5.0)
    monkeypatch.setattr("bbmp.player.open_song", slow_open)
    p = Player(catalog, fetcher, NullOutput(realtime=True), settings=Settings(level=False))
    p.play_list(catalog.songs("Protracker", "Tester"))
    end = time.time() + 5
    while time.time() < end and p.status().state != "playing":
        time.sleep(0.01)
    time.sleep(0.2)                         # the next one is being made ready now
    p.next()
    end = time.time() + 5
    while time.time() < end and not (p.status().song and p.status().song.title == "beep two"
                                     and p.status().state == "playing"):
        time.sleep(0.01)
    p.shutdown()
    assert opened == ["beep.mod", "beep two.mod"]       # opened once, not twice


def test_prefetch_puts_the_song_in_the_cache(catalog, fetcher):
    p = Player(catalog, fetcher, NullOutput(), settings=Settings(level=False))
    song = catalog.songs("Protracker", "Tester")[1]
    assert not fetcher.is_cached(song.key)
    p.prefetch(song)
    end = time.time() + 3
    while time.time() < end and not fetcher.is_cached(song.key):
        time.sleep(0.02)
    p.shutdown()
    assert fetcher.is_cached(song.key)


def test_a_resumed_queue_is_put_round_the_paused_song(catalog, fetcher, monkeypatch):
    monkeypatch.setattr("bbmp.player.open_song", lambda *a, **k: Tone(5.0))
    p = Player(catalog, fetcher, NullOutput(), settings=Settings(level=False))
    songs = catalog.songs("Protracker", "Tester")
    p.restore([songs[1]], 0)
    p.set_queue(songs, 1, [songs[1].key])
    p.set_queue(songs[:1], 0, ["not what is there"])      # the user has moved on: ignored
    end = time.time() + 3
    while time.time() < end and p.status().queue_len != len(songs):
        time.sleep(0.02)
    st = p.status()
    p.shutdown()
    assert st.state == "paused" and st.queue_pos == 1 and st.song.key == songs[1].key


# ---- user.db ----

def test_user_db_is_written_ahead(tmp_path):
    lib = Library(tmp_path / "user.db")
    assert lib.con.execute("PRAGMA journal_mode").fetchone()[0] == "wal"
    lib.con.execute("INSERT INTO favorites VALUES ('modland:a/b.mod', 1)")
    lib.con.commit()
    # another connection (another player, a backup) sees what was written
    other = sqlite3.connect(tmp_path / "user.db")
    assert other.execute("SELECT count(*) FROM favorites").fetchone()[0] == 1


def test_the_meter_takes_no_more_room_after_hours():
    from bbmp import loudness
    m = loudness.Meter()
    tone = b"".join(int(8000 * ((i % 100) / 50 - 1)).to_bytes(2, "little", signed=True) * 2
                    for i in range(RATE))
    for _ in range(30):
        m.feed(tone)
    size = m._count.nbytes + m._power.nbytes
    for _ in range(30):
        m.feed(tone)
    assert m._count.nbytes + m._power.nbytes == size and m.loudness() is not None
