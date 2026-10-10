# SPDX-License-Identifier: GPL-3.0-or-later
"""Music kept on this computer: a song, an album or game, an artist, a format, a whole
collection, a playlist or the favorites, downloaded once and played from the disk from
then on, without the collection's servers. What the cache has is taken from there and
not downloaded again. A playlist that is kept is kept as it changes: songs added to it
are fetched, songs taken out of it go (unless something else kept has them).

Fetching can be kept to the night, and to a speed. All of HVSC comes as the one archive
HVSC publishes of itself (85 MB, where its 61,000 files are 353 MB).

What is kept is written down in a database of its own, in the folder the files are
in, so the folder can be moved to another disk as it is:

    scopes   what the user asked to keep
    files    what is there, with the checksum and size it had when it was fetched

The catalog says what a collection has now. Set against `files`, it says what is
missing, what the collection has changed since, and what it no longer has, without
a file being read or a server being asked.
"""

from __future__ import annotations

import errno
import fcntl
import os
import queue
import shutil
import sqlite3
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

from . import collection as collections_, units
from .fetch import Fetcher, link_or_copy, md5_of
from .index import Index, display_name

WORKERS = 4          # files fetched at a time, each from another server where there are several
SPARE = 500_000_000  # bytes of the disk that are left free
GIVE_UP = 8          # files in a row that no server could be reached for

SCHEMA = """
CREATE TABLE IF NOT EXISTS scopes(collection TEXT, format TEXT, artist TEXT, path TEXT,
                                  album TEXT, list TEXT, added INTEGER,
                                  PRIMARY KEY (collection, format, artist, path, album, list));
CREATE TABLE IF NOT EXISTS files(collection TEXT, path TEXT, md5 TEXT, size INTEGER,
                                 PRIMARY KEY (collection, path)) WITHOUT ROWID;
"""


# The name of a playlist that is kept, by its id ("favorites", "playlist:3"); the Keeper
# that has the library says.
LIST_NAMES: Callable[[str], str] = lambda which: which


@dataclass(frozen=True)
class Scope:
    """What is kept: all of a collection, one of its formats, an artist's songs in a
    format, an album or game of theirs, one song; or a playlist or the favorites."""
    collection: str
    format: str = ""
    artist: str = ""
    path: str = ""      # of the one song
    album: str = ""     # the album or game (Song.album)
    list: str = ""      # "favorites", or "playlist:<id>"

    @classmethod
    def of(cls, song) -> "Scope":
        """The one song."""
        return cls(song.collection, song.format, song.artist, song.path, song.album)

    @classmethod
    def album_of(cls, song) -> "Scope":
        """The album or game a song is of."""
        return cls(song.collection, song.format, song.artist, album=song.album)

    @classmethod
    def playlist(cls, which: str) -> "Scope":
        return cls("", list=which)

    @property
    def name(self) -> str:
        if self.list:
            return LIST_NAMES(self.list)
        where = collections_.name(self.collection)
        shown = self.format.removeprefix(f"{where} - ")
        if self.path:
            title = display_name(self.path.rpartition("/")[2])
            return title if self.artist in ("", "- unknown") else f"{title} by {self.artist}"
        if self.album:
            parts = self.album.split("/")
            album = "/".join(parts[1:] if parts[0].startswith("coop-") else parts) or self.album
            by = "" if self.artist in ("", "- unknown") else f" by {self.artist}"
            return f"{album}{by} on {where}"
        if self.artist:
            return f"{self.artist} in {shown} on {where}"
        return f"{shown} on {where}" if self.format else f"all of {where}"

    def holds(self, other: "Scope") -> bool:
        """Is `other` a part of this?"""
        if self.list or other.list:
            return self == other
        return (self.collection == other.collection
                and self.format in ("", other.format)
                and self.artist in ("", other.artist)
                and self.album in ("", other.album)
                and self.path in ("", other.path))


@dataclass(frozen=True)
class File:
    collection: str     # the one whose servers have it: for HVSC's tunes, Modland
    path: str
    md5: str
    size: int

    @property
    def key(self) -> str:
        return f"{self.collection}:{self.path}"


@dataclass
class Plan:
    """What is to be done for the kept music to be what the catalog says."""
    fetch: list[File] = field(default_factory=list)
    move: list[tuple[File, File]] = field(default_factory=list)    # it is there, under an old path
    remove: list[File] = field(default_factory=list)               # nothing kept has it any more
    # kept, and no longer in the catalog: left where it is. A catalog that lost songs
    # (a collection took them away, or a bad list was read) never takes music away
    stale: list[File] = field(default_factory=list)
    files: int = 0       # that are wanted
    bytes: int = 0

    @property
    def fetch_bytes(self) -> int:
        return sum(f.size for f in self.fetch)

    @property
    def done(self) -> bool:
        return not (self.fetch or self.move or self.remove)


@dataclass
class State:
    scope: Scope
    files: int          # it has in the catalog
    bytes: int
    there: int          # of them, on this computer
    there_bytes: int

    @property
    def complete(self) -> bool:
        return self.there >= self.files

    @property
    def line(self) -> str:
        if self.complete:
            return f"{self.scope.name}  ·  {units.size(self.bytes) or 'nothing'}  ·  all there"
        return (f"{self.scope.name}  ·  {units.size(self.there_bytes) or '0'} of "
                f"{units.size(self.bytes)}  ·  {self.files - self.there:,} files not there yet")


@dataclass
class Progress:
    files: int = 0          # to fetch
    bytes: int = 0
    done: int = 0           # fetched, or found on this computer
    done_bytes: int = 0
    failed: list[tuple[str, str]] = field(default_factory=list)    # (path, why)
    moved: int = 0
    removed: int = 0
    stale: int = 0          # kept files the catalog no longer has, left as they are
    stopped: str = ""       # why it ended before its end, if it did
    stage: str = ""         # what is being done, where it isn't fetching file by file
    seconds: float = 0.0    # it has been fetching

    @property
    def left(self) -> str:
        """About how long it takes from here: "3 h 20 min", or "" when that can't be said."""
        if self.seconds < 5 or not self.done_bytes or self.done_bytes >= self.bytes:
            return ""
        minutes = (self.bytes - self.done_bytes) / (self.done_bytes / self.seconds) / 60
        if minutes < 1:
            return "under a minute"
        hours, minutes = divmod(round(minutes), 60)
        return f"{hours} h {minutes} min" if hours else f"{minutes} min"

    @property
    def waiting(self) -> bool:
        """Did it end because it may not fetch at this hour?"""
        return self.stopped == WAITS

    @property
    def line(self) -> str:
        return (f"{self.done:,} of {self.files:,} files  ·  "
                f"{units.size(self.done_bytes) or '0'} of {units.size(self.bytes)}"
                + (f"  ·  about {self.left} left" if self.left else ""))


class Busy(OSError):
    """Another player is fetching into the same folder."""


class Outside(OSError):
    """A file whose path in the catalog leads out of the kept folder ("..", "/..."):
    nothing is written or taken away there."""


class Keeper:
    def __init__(self, index: Index, fetcher: Fetcher | None = None, folder: Path | None = None,
                 library=None):
        """`library`: the favorites and playlists, for keeping them."""
        global LIST_NAMES
        self.index = index
        self.fetcher = fetcher or Fetcher(checksum=index.md5)
        self.folder = Path(folder) if folder is not None else self.fetcher.kept_dir
        self.library = library
        self._con: sqlite3.Connection | None = None
        self._lock = threading.Lock()
        self._wanted: dict[Scope, list[File]] = {}
        self._rate = threading.Lock()
        self._speed = 0             # bytes a second at the most, 0 for no limit
        self._allowed: Callable[[], bool] | None = None
        self._tokens = 0.0          # bytes the speed limit lets through now
        self._filled = 0.0          # when they were counted
        if library is not None:
            LIST_NAMES = self.list_name

    def list_name(self, which: str) -> str:
        if which == "favorites":
            return "your favorites"
        if which.startswith("playlist:") and self.library is not None:
            wanted = int(which.split(":", 1)[1])
            for pid, name, _ in self.library.playlists():
                if pid == wanted:
                    return f"the playlist {name}"
        return "a playlist that is gone"

    # ---- the database ----

    @property
    def db_path(self) -> Path:
        return self.folder / "kept.db"

    def _db(self, create: bool = False) -> sqlite3.Connection | None:
        if self._con is None:
            if not create and not self.db_path.exists():
                return None
            self.folder.mkdir(parents=True, exist_ok=True)
            con = sqlite3.connect(self.db_path, check_same_thread=False, timeout=30)
            try:
                con.executescript(SCHEMA)
                columns = [c[1] for c in con.execute("PRAGMA table_info(scopes)")]
                if "list" not in columns:
                    # from before albums and playlists (and before single songs) were
                    # kept; all at once or not at all, so that nothing kept is lost
                    path = "path" if "path" in columns else "''"
                    con.executescript(
                        "BEGIN; ALTER TABLE scopes RENAME TO scopes_before;" + SCHEMA
                        + f"INSERT INTO scopes SELECT collection, format, artist, {path}, '',"
                          " '', added FROM scopes_before; DROP TABLE scopes_before; COMMIT;")
            except BaseException:
                if con.in_transaction:
                    con.rollback()
                con.close()
                raise
            self._con = con
        return self._con

    def close(self) -> None:
        with self._lock:
            if self._con is not None:
                self._con.close()
                self._con = None

    def path(self, file: File) -> Path:
        """Where the file is kept. Raises Outside for a path that would lead out of
        the folder: a catalog is other people's, and is not trusted with the disk."""
        where = collections_.folder(file.collection)
        parts = file.path.split("/")
        if (file.path.startswith("/") or ".." in parts or "" in parts
                or not where or where in (".", "..") or "/" in where):
            raise Outside(f"{file.collection}:{file.path!r} leads out of the kept folder")
        return self.folder / where / file.path

    # ---- what is kept ----

    def scopes(self) -> list[Scope]:
        with self._lock:
            con = self._db()
            if con is None:
                return []
            rows = con.execute("SELECT collection, format, artist, path, album, list FROM scopes"
                               " ORDER BY list = '', path != '', collection,"
                               " format COLLATE NOCASE, artist COLLATE NOCASE, album, path"
                               " COLLATE NOCASE").fetchall()
        return [Scope(*row) for row in rows]

    def kept(self, scope: Scope) -> Scope | None:
        """The thing kept that `scope` is, or is a part of."""
        return next((s for s in self.scopes() if s.holds(scope)), None)

    def add(self, scope: Scope) -> None:
        with self._lock:
            con = self._db(create=True)
            con.execute("INSERT OR IGNORE INTO scopes VALUES (?, ?, ?, ?, ?, ?, ?)",
                        (scope.collection, scope.format, scope.artist, scope.path,
                         scope.album, scope.list, int(time.time())))
            con.commit()

    def drop(self, scope: Scope) -> int:
        """Stop keeping it, and take away the files nothing else kept has. Gives the
        bytes that freed."""
        with self._lock:
            con = self._db()
            if con is None:
                return 0
            con.execute("DELETE FROM scopes WHERE collection = ? AND format = ? AND artist = ?"
                        " AND path = ? AND album = ? AND list = ?",
                        (scope.collection, scope.format, scope.artist, scope.path, scope.album,
                         scope.list))
            con.commit()
        plan = self.plan()
        self._remove(plan.remove)
        return sum(f.size for f in plan.remove)

    def have(self) -> dict[tuple[str, str], File]:
        with self._lock:
            con = self._db()
            if con is None:
                return {}
            rows = con.execute("SELECT collection, path, md5, size FROM files").fetchall()
        return {(c, p): File(c, p, md5, size) for c, p, md5, size in rows}

    def size(self) -> int:
        """Bytes of kept music on this computer."""
        with self._lock:
            con = self._db()
            if con is None:
                return 0
            return con.execute("SELECT coalesce(sum(size), 0) FROM files").fetchone()[0]

    # ---- what the catalog says ----

    def wanted(self, scope: Scope) -> list[File]:
        """The files of a scope, with the files its songs need to play."""
        if scope.list:
            return self._list(scope)            # a list changes: never kept for later
        if scope in self._wanted:
            return self._wanted[scope]
        if scope.path:
            self._wanted[scope] = self._song(scope)
            return self._wanted[scope]
        rows = self.index.files_in(scope.collection, scope.format, scope.artist, scope.album)
        hosted = scope.collection in collections_.hosted()
        out: dict[tuple[str, str], File] = {}
        for path, md5, size, _ in rows:
            collection = scope.collection
            if hosted:      # the host has the file, under the host's path
                collection, path = collections_.split(collections_.key(collection, path))
            out[(collection, path)] = File(collection, path, md5, size)
        if scope.artist:
            # and the files that songs share across artists (a format's folder of samples)
            needed: dict[tuple[str, str], None] = {}
            for path, _, _, hidden in rows:
                if not hidden:
                    for k in self.index.companions(collections_.key(scope.collection, path)):
                        needed[collections_.split(k)] = None
            missing: dict[str, list[str]] = {}
            for collection, path in needed:
                if (collection, path) not in out:
                    missing.setdefault(collection, []).append(path)
            for collection, paths_ in missing.items():
                for path, md5, size in self.index.file_facts(collection, paths_):
                    out[(collection, path)] = File(collection, path, md5, size)
        out = list(out.values())
        self._wanted[scope] = out
        return out

    def _song(self, scope: Scope) -> list[File]:
        """The one song's file and the files it needs."""
        key = collections_.key(scope.collection, scope.path)
        song = self.index.resolve(key)
        if song is None:
            # The collection has it no longer, or has it elsewhere. A song the user
            # chose is not taken away for that: what is here stays.
            mine = self.have().get(collections_.split(key))
            return [mine] if mine is not None else []
        out = []
        for k in (key, *self.index.companions(key)):
            collection, path = collections_.split(k)
            out += [File(collection, p, md5, size)
                    for p, md5, size in self.index.file_facts(collection, [path])]
        return out

    def _file(self, key: str) -> File | None:
        """A file of a collection by its key, as it is fetched: under the key's own
        collection and path (a hosted collection's under its host's)."""
        collection, path = collections_.split(key)
        where = self.index._locate(key)
        found = self.index.file_facts(*where[:1], [where[1]])
        if not found:
            return None
        _, md5, size = found[0]
        return File(collection, path, md5, size)

    def _list_keys(self, scope: Scope) -> list[str]:
        """The songs a playlist or the favorites have, as they are saved."""
        which = scope.list
        if self.library is None:
            return []
        if which == "favorites":
            return sorted(self.library.favorite_keys())
        if which.startswith("playlist:"):
            return self.library.playlist_keys(int(which.split(":", 1)[1]))
        return []

    def _list(self, scope: Scope) -> list[File]:
        """The files of the songs a playlist or the favorites have now, with the files
        they need. Files on this computer are there already."""
        if self.library is None:
            return []
        songs = [s for k in self._list_keys(scope) if (s := self.index.resolve(k))]
        out: dict[tuple[str, str], File] = {}
        for song in songs:
            if song.local:
                continue
            for k in (song.key, *self.index.companions(song.key)):
                f = self._file(k)
                if f is not None:
                    out[(f.collection, f.path)] = f
        return list(out.values())

    def size_of(self, scope: Scope) -> tuple[int, int]:
        """(files, bytes) of a scope."""
        files = self.wanted(scope)
        return len(files), sum(f.size for f in files)

    def plan(self, more: Iterable[Scope] = ()) -> Plan:
        """What is missing, changed or no longer wanted. `more`: as if these were kept too."""
        wanted: dict[tuple[str, str], File] = {}
        for scope in (*self.scopes(), *more):
            for f in self.wanted(scope):
                wanted[(f.collection, f.path)] = f
        have = self.have()
        plan = Plan(files=len(wanted), bytes=sum(f.size for f in wanted.values()))
        unsure = self._unsure()
        gone = {k: f for k, f in have.items() if k not in wanted and not unsure(f)}
        by_md5 = {f.md5: f for f in gone.values() if f.md5}
        for k, f in wanted.items():
            mine = have.get(k)
            if mine is not None and _same(mine, f):
                continue
            old = by_md5.get(f.md5) if f.md5 and mine is None else None
            if old is not None and old.collection == f.collection:
                plan.move.append((old, f))      # the collection moved it; it is here already
                del by_md5[f.md5], gone[(old.collection, old.path)]
            else:
                plan.fetch.append(f)
        # what nothing kept wants any more goes (a song taken out of a playlist, a
        # thing no longer kept); what the catalog no longer has stays: a catalog that
        # lost its songs took the music kept of them away (7 October 2026)
        listed = self._listed(list(gone.values()))
        plan.remove = [f for k, f in gone.items() if k in listed]
        plan.stale = [f for k, f in gone.items() if k not in listed]
        plan.fetch.sort(key=lambda f: (f.collection, f.path))
        return plan

    def _listed(self, files: list[File]) -> set[tuple[str, str]]:
        """(collection, path) of the files the catalog has, as a hosted collection's
        are found (Index._locate)."""
        where: dict[str, dict[str, tuple[str, str]]] = {}
        for f in files:
            c, path = self.index._locate(collections_.key(f.collection, f.path))
            where.setdefault(c, {})[path] = (f.collection, f.path)
        out = set()
        for c, by_path in where.items():
            for path, _, _ in self.index.file_facts(c, list(by_path)):
                out.add(by_path[path])
        return out

    def _unsure(self) -> Callable[[File], bool]:
        """Which files it can't be known now whether anything kept wants: those are
        never taken away. A collection that is turned off, or whose catalog didn't
        load, has no files in the catalog, and a song of a list that the catalog
        doesn't find (it moved, or its collection is off) has none either; neither
        means that the user no longer wants them."""
        loaded = set(self.index.ids)
        if not loaded:
            return lambda f: True
        hosts = {c.hosted_by for i in loaded if (c := collections_.get(i)) and c.hosted_by}
        off: set[str] = set()
        songs: set[tuple[str, str]] = set()
        for scope in self.scopes():
            if scope.list:
                if self.library is None:
                    return lambda f: True
                for k in self._list_keys(scope):
                    if self.index.resolve(k) is None:
                        songs.add(collections_.split(k))
            elif scope.collection not in loaded:
                off.add(scope.collection)
                c = collections_.get(scope.collection)
                if c is not None and c.hosted_by:
                    off.add(c.hosted_by)     # its files are kept under its host's name

        def unsure(f: File) -> bool:
            return (f.collection in off or (f.collection, f.path) in songs
                    or (f.collection not in loaded and f.collection not in hosts))
        return unsure

    def states(self) -> list[State]:
        have = self.have()
        out = []
        for scope in self.scopes():
            files = self.wanted(scope)
            there = [f for f in files
                     if (mine := have.get((f.collection, f.path))) and _same(mine, f)]
            out.append(State(scope, len(files), sum(f.size for f in files),
                             len(there), sum(f.size for f in there)))
        return out

    # ---- the disk ----

    def free(self) -> int:
        """Bytes free on the disk the folder is on."""
        at = self.folder
        while not at.exists() and at != at.parent:
            at = at.parent
        return shutil.disk_usage(at).free

    def room(self, plan: Plan) -> tuple[bool, int, int]:
        """(does it fit, bytes to fetch, bytes free). Some of the disk is left free."""
        need, free = plan.fetch_bytes, self.free()
        return need + SPARE <= free, need, free

    @staticmethod
    def is_in(folder: Path) -> bool:
        """Does the folder have kept music, with its database?"""
        return (Path(folder).expanduser() / "kept.db").is_file()

    def move_to(self, folder: Path) -> int:
        """Move what is kept, and its database, to another folder. Gives the bytes
        that moved. On the same disk that takes a moment; to another disk everything
        is copied first, and the old folder is emptied when the copy is complete.

        Raises Busy while music is fetched, and OSError with the reason when the
        folder can't be used; everything is then where it was.
        """
        new = Path(folder).expanduser().absolute()
        old = self.folder.absolute()
        if new == old:
            return 0
        if self.is_in(new):
            raise OSError(f"{new} has kept music of its own")
        if not self.db_path.exists():           # nothing is kept: the new folder it is
            new.mkdir(parents=True, exist_ok=True)
            self.folder = self.fetcher.kept_dir = new
            return 0
        moved = self.size()
        with self._lock:
            names = [collections_.folder(c) for (c,) in self._db().execute(
                "SELECT DISTINCT collection FROM files")]
        things = [old / c for c in names if (old / c).is_dir()] + [self.db_path]
        for thing in things[:-1]:
            if new.is_relative_to(thing):
                raise OSError(f"{new} is inside the kept music")
        new.mkdir(parents=True, exist_ok=True)
        for thing in things:
            if (new / thing.name).exists():
                raise OSError(f"{new} has a {thing.name} already")
        with open(old / ".fetching", "w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                raise Busy("music is being fetched into the folder") from None
            self.close()
            if _same_disk(old, new):
                done = []
                try:
                    for thing in things:
                        os.rename(thing, new / thing.name)
                        done.append(thing)
                except BaseException:
                    for thing in reversed(done):        # as it was
                        os.rename(new / thing.name, thing)
                    raise
            else:
                free = shutil.disk_usage(new).free
                if moved + SPARE > free:
                    raise OSError(f"there is not room: {units.size(moved)} are kept, and the "
                                  f"disk {new} is on has {units.size(free) or 'nothing'} free")
                try:
                    for thing in things:
                        if thing.is_dir():
                            shutil.copytree(thing, new / thing.name)
                        else:
                            shutil.copy2(thing, new / thing.name)
                except BaseException:
                    for thing in things:        # as it was
                        shutil.rmtree(new / thing.name, ignore_errors=True)
                        (new / thing.name).unlink(missing_ok=True)
                    raise
                for thing in things:
                    shutil.rmtree(thing, ignore_errors=True)
                    thing.unlink(missing_ok=True)
        (old / ".fetching").unlink(missing_ok=True)
        try:
            old.rmdir()         # if nothing else is in it
        except OSError:
            pass
        self.folder = self.fetcher.kept_dir = new
        return moved

    def check(self) -> int:
        """Look whether the files are there, with the sizes they came with, and forget
        the ones that aren't (deleted by hand, say). Gives how many were forgotten.
        A file a stat each; while music is fetched into the folder it isn't looked
        at, and 0 is given."""
        have = self.have()
        if not have:
            return 0
        with open(self.folder / ".fetching", "w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                return 0        # files are on their way: it is looked at another time
            lost = []
            for f in have.values():
                try:
                    if self.path(f).stat().st_size != f.size:
                        lost.append(f)
                except OSError:
                    lost.append(f)
            self._forget(lost)
        return len(lost)

    def _forget(self, files: list[File]) -> None:
        if not files:
            return
        with self._lock:
            con = self._db(create=True)
            con.executemany("DELETE FROM files WHERE collection = ? AND path = ?",
                            ((f.collection, f.path) for f in files))
            con.commit()

    def _record(self, files: list[File]) -> None:
        if not files:
            return
        with self._lock:
            con = self._db(create=True)
            con.executemany("INSERT OR REPLACE INTO files VALUES (?, ?, ?, ?)",
                            ((f.collection, f.path, f.md5, f.size) for f in files))
            con.commit()

    def _remove(self, files: list[File]) -> None:
        """Take the files away, and the folders that are empty then; never anything
        outside the kept folder."""
        folders = set()
        for f in files:
            try:
                local = self.path(f)
            except Outside:
                continue        # it is only forgotten
            local.unlink(missing_ok=True)
            folders.add(local.parent)
        self._forget(files)
        for d in sorted(folders, key=lambda p: len(p.parts), reverse=True):
            try:
                while (d != self.folder and d.is_relative_to(self.folder)
                       and not any(d.iterdir())):
                    d.rmdir()
                    d = d.parent
            except OSError:
                pass

    # ---- fetching ----

    def run(self, plan: Plan | None = None,
            progress: Callable[[Progress], None] = lambda p: None,
            stop: threading.Event | None = None, workers: int = WORKERS,
            speed: int = 0, allowed: Callable[[], bool] | None = None) -> Progress:
        """Do what the plan says. It can be stopped at any file and taken up again:
        what was fetched is written down as it comes. `speed`: bytes a second at the
        most, 0 for no limit; `allowed()`: whether fetching may go on now (the hours)."""
        stop = stop or threading.Event()
        self._speed, self._allowed = speed, allowed
        self._tokens, self._filled = 0.0, time.monotonic()
        self.folder.mkdir(parents=True, exist_ok=True)
        with open(self.folder / ".fetching", "w") as lock:
            try:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                raise Busy("another player is fetching into this folder") from None
            plan = plan if plan is not None else self.plan()
            state = Progress(files=len(plan.fetch), bytes=plan.fetch_bytes)
            ok, need, free = self.room(plan)
            if not ok:
                state.stopped = (f"There is not room for it: {units.size(need)} are to be "
                                 f"fetched, and the disk {self.folder} is on has "
                                 f"{units.size(free) or 'nothing'} free.")
                return state
            self._move(plan.move, state)
            self._remove(plan.remove)
            state.removed, state.stale = len(plan.remove), len(plan.stale)
            progress(state)
            files = plan.fetch
            if files and not self._may():
                state.stopped = WAITS
                progress(state)
                return state
            if files and not stop.is_set():
                files = self._from_hvsc_archive(files, state, progress, stop)
            if files and not stop.is_set():
                self._fetch(files, state, progress, stop, max(1, workers))
            return state

    def _may(self) -> bool:
        """May fetching go on now (the hours Settings gives)?"""
        return self._allowed is None or self._allowed()

    def _move(self, moves: list[tuple[File, File]], state: Progress) -> None:
        moved = []
        for old, new in moves:
            try:
                dest = self.path(new)       # Outside: it isn't moved, nor fetched
                dest.parent.mkdir(parents=True, exist_ok=True)
                os.replace(self.path(old), dest)
            except OSError:
                continue            # it is fetched the next time
            moved.append((old, File(new.collection, new.path, new.md5, old.size)))
        self._record([new for _, new in moved])
        self._remove([old for old, _ in moved])
        state.moved = len(moved)

    def _one(self, fetcher: Fetcher, f: File, stop: threading.Event) -> File:
        """Bring one file to its place, from the folder itself, the cache or a server."""
        dest = self.path(f)
        cached = fetcher.cache_dir / f.collection / f.path
        for there in (dest, cached):
            try:
                size = there.stat().st_size
            except OSError:
                continue
            # the checksum decides; the size only where the collection gives no checksum
            if md5_of(there) == f.md5.lower() if f.md5 else size == f.size:
                if there is cached:
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    link_or_copy(cached, dest)
                return File(f.collection, f.path, f.md5, dest.stat().st_size)
        self._slow_down(f.size, stop)
        if stop.is_set():
            raise InterruptedError("stopped")
        fetcher.fetch_to(f.key, dest)
        return File(f.collection, f.path, f.md5, dest.stat().st_size)

    def _slow_down(self, size: int, stop: threading.Event) -> None:
        """Keep to the speed limit, over all the files fetched at once: a bucket that
        fills at the speed, up to a second of it, and that each file takes its bytes
        from before it is fetched. A file waits until the bucket has had them, so
        the workers take turns and none of them gets ahead."""
        speed = self._speed
        if not speed or size <= 0:
            return
        with self._rate:
            now = time.monotonic()
            self._tokens = min(float(speed), self._tokens + (now - self._filled) * speed)
            self._filled = now
            self._tokens -= size
            wait = -self._tokens / speed
        if wait > 0:
            stop.wait(wait)

    ARCHIVE_FROM = 1000     # HVSC's files to fetch from which its archive is fetched instead

    def _from_hvsc_archive(self, files: list[File], state: Progress, progress,
                           stop: threading.Event) -> list[File]:
        """Many of HVSC's tunes to fetch: fetch HVSC's archive of itself, and take them
        from it, each checked against the catalog. Gives the files still to fetch."""
        start = "HVSC/"
        tunes = [f for f in files if f.collection == "modland" and f.path.startswith(start)]
        seven = shutil.which("7z") or shutil.which("7za")
        if len(tunes) < self.ARCHIVE_FROM or seven is None:
            return files
        work = self.folder / ".hvsc-archive"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        packed = work / "hvsc.7z"
        state.stage = "HVSC's archive"
        progress(state)
        try:
            import urllib.request
            url = _hvsc_archive_url()
            request = urllib.request.Request(url, headers={"User-Agent": "bbmp"})
            from . import capped
            got = 0
            with urllib.request.urlopen(request, timeout=60) as r, open(packed, "wb") as out:
                while chunk := r.read(1 << 20):
                    got += len(chunk)
                    if got > capped.ARCHIVE:
                        raise capped.TooBig("HVSC's archive is larger than it can be")
                    if not self._may():
                        state.stopped = WAITS
                        stop.set()
                    if stop.is_set():
                        return files
                    out.write(chunk)
                    self._slow_down(len(chunk), stop)
            unpacked = work / "unpacked"
            unpacked.mkdir()
            from . import sandbox
            # as the replayers are: shut in, with their memory limit and environment
            seven_x = sandbox.popen(
                sandbox.wrap([seven, "x", "-y", "-bd", f"-o{unpacked}", str(packed)], packed,
                             writable=[unpacked]),
                stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                seven_x.wait(timeout=1800)
            except subprocess.TimeoutExpired:
                seven_x.kill()
                seven_x.wait()
                raise OSError("7z took too long to unpack it") from None
            if seven_x.returncode != 0:
                raise OSError("7z couldn't unpack it")
            packed.unlink(missing_ok=True)
            root = unpacked / "C64Music"
            taken, fresh = set(), []
            for f in tunes:
                if stop.is_set():
                    break
                try:
                    dest = self.path(f)     # Outside: fetched on its own, and refused there
                    there = root / f.path[len(start):]
                    if (md5_of(there) == f.md5.lower()) if f.md5 else there.is_file():
                        dest.parent.mkdir(parents=True, exist_ok=True)
                        os.replace(there, dest)
                        fresh.append(File(f.collection, f.path, f.md5, dest.stat().st_size))
                        taken.add((f.collection, f.path))
                        state.done += 1
                        state.done_bytes += f.size
                except OSError:
                    continue            # not in the archive, or another version: fetched
                if len(fresh) >= 500:
                    self._record(fresh)
                    fresh = []
                    progress(state)
            self._record(fresh)
            progress(state)
            return [f for f in files if (f.collection, f.path) not in taken]
        except Exception as e:          # the archive can't be had: file by file, as ever
            state.failed.append(("HVSC's archive", str(e)))
            return files
        finally:
            state.stage = ""
            shutil.rmtree(work, ignore_errors=True)

    def _fetch(self, files: list[File], state: Progress, progress, stop: threading.Event,
               workers: int) -> None:
        jobs: queue.Queue = queue.Queue()
        results: queue.Queue = queue.Queue()
        for f in files:
            jobs.put(f)

        def work(n: int) -> None:
            fetcher = self.fetcher.clone(n)
            try:
                while not stop.is_set():
                    if not self._may():
                        state.stopped = WAITS
                        stop.set()
                        return
                    try:
                        f = jobs.get_nowait()
                    except queue.Empty:
                        return
                    try:
                        results.put((f, self._one(fetcher, f, stop), None))
                    except InterruptedError:
                        return          # stopped while it waited for the speed limit
                    except Exception as e:      # told to the user, file by file
                        results.put((f, None, e))
            finally:
                fetcher.close()
                results.put(None)

        threads = [threading.Thread(target=work, args=(n,), name=f"keep-{n}", daemon=True)
                   for n in range(min(workers, len(files)))]
        for t in threads:
            t.start()
        running, fresh, unreachable, said = len(threads), [], 0, time.monotonic()
        began = said
        # Stopped, the files on their way are waited for (and written down): the lock
        # on the folder is held until no worker writes into it any more.
        while running:
            try:
                got = results.get(timeout=0.5)
            except queue.Empty:
                continue
            if got is None:
                running -= 1
                continue
            f, there, error = got
            if error is None:
                fresh.append(there)
                state.done += 1
                state.done_bytes += f.size
                unreachable = 0
            elif isinstance(error, Outside):
                state.failed.append((f.path, "its path leads out of the folder"))
            elif isinstance(error, FileNotFoundError):
                state.failed.append((f.path, "no server has it"))
                unreachable = 0
            else:
                state.failed.append((f.path, str(error)))
                if isinstance(error, OSError) and error.errno in (errno.ENOSPC, errno.EDQUOT):
                    state.stopped = "The disk is full."
                    stop.set()
                unreachable += 1
                if unreachable >= GIVE_UP and not state.stopped:
                    state.stopped = f"The servers can't be reached ({error})."
                    stop.set()
            state.seconds = time.monotonic() - began
            if len(fresh) >= 50 or time.monotonic() - said > 1:
                self._record(fresh)
                fresh, said = [], time.monotonic()
                progress(state)
        for t in threads:
            t.join()
        self._record(fresh)
        if stop.is_set() and not state.stopped and state.done + len(state.failed) < state.files:
            state.stopped = "Stopped. It goes on from here the next time."
        progress(state)


WAITS = "Waiting for the night, as Settings says."     # Progress.stopped, fetching not begun


def _hvsc_archive_url() -> str:
    """Where HVSC's newest archive of itself is, as its site says."""
    import json
    import urllib.request
    request = urllib.request.Request("https://hvsc.c64.org/api/v1/version/7z",
                                     headers={"User-Agent": "bbmp"})
    with urllib.request.urlopen(request, timeout=30) as r:
        from . import capped
        return json.loads(capped.read(r, 1 << 20))["complete"]["url"]


def _same_disk(a: Path, b: Path) -> bool:
    return os.stat(a).st_dev == os.stat(b).st_dev


def _same(mine: File, theirs: File) -> bool:
    """Is what is here what the catalog has?"""
    if mine.md5 and theirs.md5:
        return mine.md5.lower() == theirs.md5.lower()
    return not theirs.size or mine.size == theirs.size
