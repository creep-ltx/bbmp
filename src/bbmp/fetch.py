# SPDX-License-Identifier: GPL-3.0-or-later
"""Download songs from their collections into a local cache.

The cache has a folder for every collection, and below it the same layout as the
collection has on its server. One connection per collection is kept open and reused.

Music the user downloads to keep (keep.py) is in the music folder, in a folder for
every collection by its name, laid out the same. A song that is there is played from
there, and the cache's limit is not its business.
"""

from __future__ import annotations

import contextlib
import hashlib
import os
import queue
import shutil
import threading
import time
from pathlib import Path
from typing import Iterator

from . import collection as collections_, paths, psf
from .collection import DEFAULT, LOCAL, Transport
from .collection.transports import Mirrors
from .settings import Settings

LAYOUT = "2"     # 1: Modland's folders directly in the cache; 2: a folder per collection
# A file used or fetched as late as this many seconds ago is not trimmed away: it may be
# on its way to the player, or a companion of a song that is being fetched.
RECENT = 60
# Companion files are downloaded this many at a time, each on a connection of its own
# (some formats have dozens of them: Zoundmonitor's and YMST's songs 57).
PARALLEL = 4
# When the cache can't be brought under its limit (what is left is kept, queued or new),
# it is looked through again only after this much more was downloaded, or this long.
RETRIM_BYTES = 20_000_000
RETRIM_SECONDS = 600


def md5_of(file: Path) -> str:
    digest = hashlib.md5()
    with open(file, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def link_or_copy(src: Path, dest: Path) -> None:
    """Have the file at `dest` too, without a second copy on the disk where the
    file system can do that."""
    tmp = dest.with_name(dest.name + ".part")
    tmp.unlink(missing_ok=True)
    try:
        os.link(src, tmp)
    except OSError:
        shutil.copy2(src, tmp)
    os.replace(tmp, dest)


class Fetcher:
    def __init__(self, cache_dir: Path = paths.FILES_DIR, checksum=None,
                 kept_dir: Path | None = None):
        """`checksum(key)` gives the MD5 a file should have, where the catalog knows it
        (Index.md5). What a server gives is checked against it."""
        self.cache_dir = cache_dir
        self.checksum = checksum
        self.kept_dir = (Path(kept_dir) if kept_dir is not None
                         else paths.kept_dir(Settings.load().keep_dir))
        self._ask_first = 0      # of a collection's servers
        self._transports: dict[str, Transport] = {}
        self._in_use: dict[str, threading.Lock] = {}     # a collection's, while it downloads
        self._closing: set[int] = set()     # transports in use when close() was called
        self._lock = threading.Lock()       # of the two dicts, held only to look in them
        self._trim_lock = threading.Lock()
        self._known_total: int | None = None
        self._added = 0
        self._stuck: float | None = None    # when a trim last couldn't get under the limit
        # files being downloaded, by where they go: shared with clones, so that two
        # fetchers that want the same file download it once (the second waits)
        self._inflight: dict[Path, threading.Event] = {}
        self._inflight_lock = threading.Lock()
        self._root = self       # the fetcher whose cache size counts what clones download
        self._adopt_old_layout()

    def _adopt_old_layout(self) -> None:
        """A cache from before there were collections holds Modland's folders: move
        them into the folder for Modland."""
        marker = self.cache_dir / ".layout"
        try:
            if marker.exists() or not self.cache_dir.is_dir():
                self.cache_dir.mkdir(parents=True, exist_ok=True)
                marker.write_text(LAYOUT + "\n")
                return
            old = [f for f in self.cache_dir.iterdir()]
            if old:
                home = self.cache_dir / DEFAULT
                staging = self.cache_dir / ".moving"
                staging.mkdir(exist_ok=True)
                for f in old:
                    if f != staging:
                        f.rename(staging / f.name)
                staging.rename(home)
            marker.write_text(LAYOUT + "\n")
        except OSError:
            pass     # a cache that can't be written to fails when a song is downloaded

    def _places(self, key: str) -> tuple[Path, Path]:
        """Where a file of a collection is when it is kept, and when it is cached."""
        collection, path = collections_.split(key)
        parts = path.split("/")
        if path.startswith("/") or ".." in parts or "" in parts:
            raise FileNotFoundError(f"{path!r} is not a path inside a collection")
        return (self.kept_dir / collections_.folder(collection) / path,
                self.cache_dir / collection / path)

    def local_path(self, key: str) -> Path:
        """Where the file is on this computer, or will be once it is downloaded."""
        collection, path = collections_.split(key)
        if collection == LOCAL:
            return Path(path)
        kept, cached = self._places(key)
        return kept if kept.exists() else cached

    def is_cached(self, key: str) -> bool:
        return self.local_path(key).exists()

    def is_kept(self, key: str) -> bool:
        return collections_.split(key)[0] != LOCAL and self._places(key)[0].exists()

    def get(self, key: str, companions: list[str] = ()) -> Path:
        """Make sure a song and its companion files are on this computer; return the
        song's file.

        The companions are put where the song is, in the kept music or in the cache:
        a replayer looks for them next to the song.
        """
        if collections_.split(key)[0] == LOCAL:
            return Path(key)
        now = time.time()
        kept = self._places(key)[0].exists()
        missing: list[tuple[str, Path]] = []
        for k in (key, *companions):
            local, other = self._places(k) if kept else self._places(k)[::-1]
            if local.exists():
                if not kept:
                    try:
                        os.utime(local, (now, now))   # the modification time marks "last used"
                    except OSError:
                        pass
            elif other.exists():
                local.parent.mkdir(parents=True, exist_ok=True)
                link_or_copy(other, local)
            else:
                missing.append((k, local))
        self._download_all(missing)
        if not kept:
            for _, local in missing:
                with contextlib.suppress(OSError):
                    self._root._added += local.stat().st_size
        song = self._places(key)[0 if kept else 1]
        psf.fit_libs(song)          # libraries under the names the song asks for
        return song

    def _download_all(self, files: list[tuple[str, Path]]) -> None:
        """Download (key, where to) files: one by one on this fetcher's connection when
        there are few, PARALLEL at a time when there are more, each further one on a
        connection of its own. The first error is raised once all have stopped."""
        if len(files) <= 1:
            for k, local in files:
                self._download(k, local)
            return
        jobs: queue.Queue = queue.Queue()
        for job in files:
            jobs.put(job)
        errors: list[BaseException] = []

        def work(fetcher: "Fetcher") -> None:
            try:
                while not errors:
                    try:
                        k, local = jobs.get_nowait()
                    except queue.Empty:
                        return
                    try:
                        fetcher._download(k, local)
                    except BaseException as e:      # noqa: BLE001 - given to the caller
                        errors.append(e)
            finally:
                if fetcher is not self:
                    fetcher.close()
        helpers = [self.clone() for _ in range(min(PARALLEL, len(files)) - 1)]
        threads = [threading.Thread(target=work, args=(f,), name="fetch-companions",
                                    daemon=True) for f in helpers]
        for t in threads:
            t.start()
        work(self)
        for t in threads:
            t.join()
        if errors:
            raise errors[0]

    def fetch_to(self, key: str, dest: Path) -> None:
        """Download a file of a collection to a place of the caller's choice."""
        self._download(key, dest)

    def clone(self, n: int = 0) -> "Fetcher":
        """A fetcher of the same files with connections of its own. Of a collection's
        servers it asks the n-th first, so that several fetchers at work ask several
        servers."""
        other = Fetcher(self.cache_dir, self.checksum, self.kept_dir)
        other._ask_first = n
        other._inflight, other._inflight_lock = self._inflight, self._inflight_lock
        other._root = self._root
        return other

    def trim(self, limit_bytes: int, keep: set[str] = frozenset()) -> int:
        """Delete the least recently used files until the cache fits in limit_bytes.

        The songs in `keep` (favorites, the queue and their companion files, by key) are
        never deleted, nor what is being downloaded or was used in the last minute.
        Returns the number of bytes freed. A limit of 0 means no limit.
        """
        if limit_bytes <= 0:
            return 0
        with self._trim_lock:
            files = self._measured()
            total = sum(size for _, size, _ in files)
            self._added = 0
            self._known_total = total
            self._stuck = None
            if total <= limit_bytes:
                return 0
            keep_files = set()
            for k in keep:
                try:
                    keep_files.add(self.local_path(k))
                except FileNotFoundError:
                    pass
            freed, recent = 0, time.time() - RECENT
            for used, size, f in sorted(files):
                if total - freed <= limit_bytes:
                    break
                if f in keep_files or used > recent:
                    continue
                try:
                    f.unlink()
                    freed += size
                except OSError:
                    pass
            # remove folders that are now empty
            for d in sorted({f.parent for _, _, f in files}, key=lambda p: len(p.parts), reverse=True):
                try:
                    while d != self.cache_dir and d.exists() and not any(d.iterdir()):
                        d.rmdir()
                        d = d.parent
                except OSError:
                    pass            # a song arrived in it meanwhile, or it went already
            self._known_total = total - freed
            if self._known_total > limit_bytes:
                # what is left can't go now: not looked through again after every song
                self._stuck = time.time()
            return freed

    def _measured(self, root: Path | None = None) -> list[tuple[float, int, Path]]:
        """(last used, size, path) of each file in the cache (or under root). A file
        that goes while it is looked at (a download replaced, a trim elsewhere) is left
        out: measuring it closed the player (7 October 2026)."""
        out = []
        for f in self._files():
            if root is not None and not f.is_relative_to(root):
                continue
            try:
                st = f.stat()
            except FileNotFoundError:
                continue
            out.append((st.st_mtime, st.st_size, f))
        return out

    def _files(self) -> Iterator[Path]:
        for f in self.cache_dir.rglob("*"):
            if f.is_file() and not f.name.endswith(".part") and f.parent != self.cache_dir:
                yield f

    def over_limit(self, limit_bytes: int) -> bool:
        """Cheap check whether trim() has work to do (the first call scans the cache)."""
        if limit_bytes <= 0:
            return False
        if self._known_total is None:
            self._known_total = self.cache_size()
        if (self._stuck is not None and self._added < RETRIM_BYTES
                and time.time() - self._stuck < RETRIM_SECONDS):
            return False        # the last trim couldn't do more, and little has come since
        return self._known_total + self._added > limit_bytes

    def cache_size(self, collection: str = "") -> int:
        """Bytes in the cache, of every collection or of one."""
        root = self.cache_dir / collection if collection else self.cache_dir
        # not while a trim deletes: the two take turns
        with self._trim_lock:
            return sum(size for _, size, _ in self._measured(root))

    @contextlib.contextmanager
    def transport(self, collection: str) -> Iterator[Transport]:
        """The open connection to a collection's server, for one thread at a time: a
        thread that wants it while another downloads waits for that one. The other
        collections, and quitting, don't wait.

        After an error the connection is closed, and the next use opens a new one.
        """
        source = collections_.get(collection)
        if source is None:
            raise FileNotFoundError(f"this player doesn't know the collection {collection!r}")
        with self._lock:
            in_use = self._in_use.setdefault(collection, threading.Lock())
        with in_use:
            with self._lock:
                transport = self._transports.get(collection)
                if transport is None:
                    transport = self._transports[collection] = source.transport()
                    if isinstance(transport, Mirrors) and transport.transports and self._ask_first:
                        transport.first = self._ask_first % len(transport.transports)
            try:
                yield transport
            except (OSError, EOFError):
                with self._lock:
                    if self._transports.get(collection) is transport:
                        del self._transports[collection]
                    self._closing.discard(id(transport))
                transport.close()
                raise
            finally:
                with self._lock:
                    closing = id(transport) in self._closing
                    self._closing.discard(id(transport))
                if closing:
                    transport.close()

    def _download(self, key: str, dest: Path | None = None) -> None:
        dest = dest if dest is not None else self._places(key)[1]
        while True:
            with self._inflight_lock:
                busy = self._inflight.get(dest)
                if busy is None:
                    done = self._inflight[dest] = threading.Event()
                    break
            busy.wait()             # another fetcher downloads it: its file is used
            if dest.exists():
                return
        try:
            self._download_now(key, dest)
        finally:
            with self._inflight_lock:
                del self._inflight[dest]
            done.set()

    def _download_now(self, key: str, dest: Path) -> None:
        collection, path = collections_.split(key)
        dest.parent.mkdir(parents=True, exist_ok=True)
        tmp = dest.with_name(dest.name + ".part")
        expected = None
        if self.checksum is not None:
            try:
                expected = self.checksum(key)
            except Exception:
                expected = None

        def right(file: Path) -> bool:
            return not expected or md5_of(file) == expected.lower()
        try:
            with self.transport(collection) as transport:
                if isinstance(transport, Mirrors):
                    transport.download(path, tmp, right)
                else:
                    transport.download(path, tmp)
        except BaseException:
            tmp.unlink(missing_ok=True)
            tmp.with_name(tmp.name + ".other").unlink(missing_ok=True)
            raise
        os.replace(tmp, dest)

    def close(self) -> None:
        """Close the connections. One that a download is using is closed when that
        download is over, and not waited for."""
        with self._lock:
            idle, busy = [], []
            for collection, transport in self._transports.items():
                lock = self._in_use.get(collection)
                if lock is not None and not lock.acquire(blocking=False):
                    busy.append(transport)
                    continue
                idle.append(transport)
                if lock is not None:
                    lock.release()
            self._closing.update(id(t) for t in busy)
            self._transports.clear()
        for transport in idle:
            transport.close()
