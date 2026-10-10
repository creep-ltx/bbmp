# SPDX-License-Identifier: GPL-3.0-or-later
"""Your own data: favorites, playlists, and every play, for history and statistics.

Kept apart from the catalog so catalog updates never touch it. Songs are stored as
their keys ("modland:AHX/Pink/inside.ahx", or a local file's path), in the columns
called path.
"""

from __future__ import annotations

import sqlite3
import threading
import time
from pathlib import Path

from . import paths
from .collection import DEFAULT
from .index import Index, Song


class Library:
    def __init__(self, db_path: Path = paths.USER_DB):
        db_path.parent.mkdir(parents=True, exist_ok=True)
        self.con = sqlite3.connect(db_path, check_same_thread=False)
        self._lock = threading.Lock()
        # Write-ahead: a change is written once and not synced to the disk each time
        # (4.3 ms a commit before, a few per song, in the thread that plays). After a
        # crash of the computer the last changes may be lost, never the file. While the
        # player runs, recent changes are in user.db-wal next to it: copy both, or
        # checkpoint() first. (On a file system without shared memory it stays as it was.)
        try:
            self.con.execute("PRAGMA journal_mode=WAL")
            self.con.execute("PRAGMA synchronous=NORMAL")
        except sqlite3.DatabaseError:
            pass
        self.con.executescript("""
            CREATE TABLE IF NOT EXISTS favorites(path TEXT PRIMARY KEY, added INTEGER);
            CREATE TABLE IF NOT EXISTS plays(
                id INTEGER PRIMARY KEY, path TEXT NOT NULL, format TEXT NOT NULL,
                artist TEXT NOT NULL, title TEXT NOT NULL, started INTEGER NOT NULL,
                seconds REAL NOT NULL DEFAULT 0);
            CREATE INDEX IF NOT EXISTS plays_started ON plays(started);
            CREATE TABLE IF NOT EXISTS playlists(
                id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, created INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS loudness(
                path TEXT NOT NULL, subsong INTEGER NOT NULL, lufs REAL NOT NULL,
                measured INTEGER NOT NULL, PRIMARY KEY (path, subsong));
            CREATE TABLE IF NOT EXISTS trouble(
                path TEXT PRIMARY KEY, kind TEXT NOT NULL, note TEXT NOT NULL,
                noted INTEGER NOT NULL);
            CREATE TABLE IF NOT EXISTS checksums(path TEXT PRIMARY KEY, md5 TEXT NOT NULL);
            CREATE TABLE IF NOT EXISTS playlist_songs(
                playlist INTEGER NOT NULL, pos INTEGER NOT NULL, path TEXT NOT NULL,
                PRIMARY KEY (playlist, pos));
        """)
        self._migrate_history()
        self._name_collections()

    def _migrate_history(self) -> None:
        """Older versions kept only (path, time) in a 'history' table."""
        with self._lock, self.con:
            if not self.con.execute("SELECT 1 FROM sqlite_master WHERE name = 'history'").fetchone():
                return
            for path, played in self.con.execute("SELECT path, played FROM history").fetchall():
                parts = path.split("/")
                self.con.execute(
                    "INSERT INTO plays(path, format, artist, title, started, seconds)"
                    " VALUES (?, ?, ?, ?, ?, 0)",
                    (path, parts[0], parts[1] if len(parts) > 2 else "", parts[-1], played))
            self.con.execute("DROP TABLE history")

    def _name_collections(self) -> None:
        """Before there were collections every song was from Modland, and was saved
        without saying so."""
        with self._lock, self.con:
            if self.con.execute("PRAGMA user_version").fetchone()[0] >= 1:
                return
            for table in ("favorites", "plays", "playlist_songs"):
                self.con.execute(
                    f"UPDATE {table} SET path = '{DEFAULT}:' || path"
                    f" WHERE path NOT LIKE '/%' AND path NOT LIKE '{DEFAULT}:%'")
            self.con.execute("PRAGMA user_version = 1")

    def is_favorite(self, path: str) -> bool:
        with self._lock:
            return self.con.execute("SELECT 1 FROM favorites WHERE path = ?", (path,)).fetchone() is not None

    def favorite_keys(self) -> set[str]:
        with self._lock:
            return {p for (p,) in self.con.execute("SELECT path FROM favorites")}

    def toggle_favorite(self, path: str) -> bool:
        """Returns True if the song is now a favorite."""
        with self._lock, self.con:
            if self.con.execute("DELETE FROM favorites WHERE path = ?", (path,)).rowcount:
                return False
            self.con.execute("INSERT INTO favorites VALUES (?, ?)", (path, int(time.time())))
            return True

    def favorites(self, index: Index) -> list[Song]:
        with self._lock:
            rows = self.con.execute("SELECT path FROM favorites ORDER BY added DESC").fetchall()
        return [s for (p,) in rows if (s := index.resolve(p))]

    # ---- playlists ----

    def playlists(self) -> list[tuple[int, str, int]]:
        """(id, name, number of songs), by name."""
        with self._lock:
            return self.con.execute(
                "SELECT p.id, p.name, count(s.path) FROM playlists p"
                " LEFT JOIN playlist_songs s ON s.playlist = p.id"
                " GROUP BY p.id ORDER BY p.name COLLATE NOCASE").fetchall()

    def playlist_id(self, name: str) -> int | None:
        with self._lock:
            row = self.con.execute("SELECT id FROM playlists WHERE name = ? COLLATE NOCASE",
                                   (name.strip(),)).fetchone()
        return row[0] if row else None

    def create_playlist(self, name: str, paths: list[str] = ()) -> int:
        """A new playlist. A taken name gets a number: "Chips", "Chips 2", ...

        The name is looked at and taken in one statement, so that two made at once
        (another window, another player) never get the same one."""
        base = name.strip() or "Playlist"
        name, n = base, 1
        with self._lock, self.con:
            while True:
                try:
                    cur = self.con.execute(
                        "INSERT INTO playlists(name, created) SELECT ?, ? WHERE NOT EXISTS"
                        " (SELECT 1 FROM playlists WHERE name = ? COLLATE NOCASE)",
                        (name, int(time.time()), name))
                except sqlite3.IntegrityError:
                    cur = None
                if cur is not None and cur.rowcount == 1:
                    break
                n += 1
                name = f"{base} {n}"
        self.add_to_playlist(cur.lastrowid, list(paths))
        return cur.lastrowid

    def delete_playlist(self, playlist: int) -> None:
        with self._lock, self.con:
            self.con.execute("DELETE FROM playlist_songs WHERE playlist = ?", (playlist,))
            self.con.execute("DELETE FROM playlists WHERE id = ?", (playlist,))

    def rename_playlist(self, playlist: int, name: str) -> bool:
        name = name.strip()
        if not name or self.playlist_id(name) not in (None, playlist):
            return False
        with self._lock, self.con:
            self.con.execute("UPDATE playlists SET name = ? WHERE id = ?", (name, playlist))
        return True

    def add_to_playlist(self, playlist: int, paths: list[str]) -> int:
        """Append songs; ones already in the playlist are left out. Returns how many were added."""
        with self._lock, self.con:
            have = {p for (p,) in self.con.execute(
                "SELECT path FROM playlist_songs WHERE playlist = ?", (playlist,))}
            (last,) = self.con.execute("SELECT coalesce(max(pos), -1) FROM playlist_songs"
                                       " WHERE playlist = ?", (playlist,)).fetchone()
            added = 0
            for path in paths:
                if path in have:
                    continue
                have.add(path)
                last += 1
                added += 1
                self.con.execute("INSERT INTO playlist_songs VALUES (?, ?, ?)", (playlist, last, path))
        return added

    def remove_from_playlist(self, playlist: int, path: str) -> None:
        with self._lock, self.con:
            self.con.execute("DELETE FROM playlist_songs WHERE playlist = ? AND path = ?",
                             (playlist, path))

    def move_in_playlist(self, playlist: int, path: str, delta: int) -> int | None:
        """Move a song up (delta -1) or down (+1). Returns its new place, or None if it can't move."""
        with self._lock, self.con:
            paths_ = [p for (p,) in self.con.execute(
                "SELECT path FROM playlist_songs WHERE playlist = ? ORDER BY pos", (playlist,))]
            if path not in paths_:
                return None
            old = paths_.index(path)
            new = old + delta
            if not 0 <= new < len(paths_):
                return None
            paths_.insert(new, paths_.pop(old))
            self.con.execute("DELETE FROM playlist_songs WHERE playlist = ?", (playlist,))
            self.con.executemany("INSERT INTO playlist_songs VALUES (?, ?, ?)",
                                 [(playlist, i, p) for i, p in enumerate(paths_)])
        return new

    def playlist_name(self, playlist: int) -> str:
        with self._lock:
            row = self.con.execute("SELECT name FROM playlists WHERE id = ?", (playlist,)).fetchone()
        return row[0] if row else ""

    def playlist_keys(self, playlist: int) -> list[str]:
        with self._lock:
            return [p for (p,) in self.con.execute(
                "SELECT path FROM playlist_songs WHERE playlist = ? ORDER BY pos", (playlist,))]

    def playlist_songs(self, playlist: int, index: Index) -> list[Song]:
        return [s for p in self.playlist_keys(playlist) if (s := index.resolve(p))]

    def protected_keys(self) -> set[str]:
        """Songs the cache must keep: favorites and everything in a playlist."""
        with self._lock:
            return ({p for (p,) in self.con.execute("SELECT path FROM favorites")}
                    | {p for (p,) in self.con.execute("SELECT path FROM playlist_songs")})

    def relocate(self, index: Index) -> list[tuple[str, str]]:
        """Favorites and playlist songs that the collection has moved or renamed are
        found again, by their checksum, and saved under their new key. The checksum of
        each is noted while the catalog finds it. Gives (old key, new key) of the ones
        that moved."""
        with self._lock:
            noted = dict(self.con.execute("SELECT path, md5 FROM checksums").fetchall())
        new_sums: dict[str, str] = {}
        moved: list[tuple[str, str]] = []
        for key in sorted(self.protected_keys()):
            if key.startswith("/"):
                continue                    # a file on this computer
            if index.resolve(key) is not None:
                md5 = index.md5(key)
                if md5 and noted.get(key) != md5:
                    new_sums[key] = md5
            elif key in noted:
                found = index.moved(key, noted[key])
                if found is not None:
                    moved.append((key, found))
        with self._lock, self.con:
            self.con.executemany("INSERT OR REPLACE INTO checksums VALUES (?, ?)",
                                 new_sums.items())
            for old, new in moved:
                if self.con.execute("SELECT 1 FROM favorites WHERE path = ?", (new,)).fetchone():
                    self.con.execute("DELETE FROM favorites WHERE path = ?", (old,))
                else:
                    self.con.execute("UPDATE favorites SET path = ? WHERE path = ?", (new, old))
                self.con.execute("UPDATE playlist_songs SET path = ? WHERE path = ?", (new, old))
                self.con.execute("UPDATE OR IGNORE loudness SET path = ? WHERE path = ?", (new, old))
                self.con.execute("UPDATE checksums SET path = ? WHERE path = ?", (new, old))
        return moved

    # ---- loudness ----

    def loudness(self, key: str, subsong: int = 0) -> float | None:
        """A song's loudness in LUFS, if it was played before."""
        with self._lock:
            row = self.con.execute("SELECT lufs FROM loudness WHERE path = ? AND subsong = ?",
                                   (key, subsong)).fetchone()
        return row[0] if row else None

    def trouble(self) -> dict[str, tuple[str, str]]:
        """Songs that failed on this computer: key -> (kind, why)."""
        with self._lock:
            return {p: (k, n) for p, k, n in
                    self.con.execute("SELECT path, kind, note FROM trouble")}

    def note_trouble(self, key: str, kind: str, note: str) -> None:
        with self._lock, self.con:
            self.con.execute("INSERT OR REPLACE INTO trouble VALUES (?, ?, ?, ?)",
                             (key, kind, note, int(time.time())))

    def clear_trouble(self, key: str) -> None:
        with self._lock, self.con:
            self.con.execute("DELETE FROM trouble WHERE path = ?", (key,))

    def set_loudness(self, key: str, subsong: int, lufs: float) -> None:
        with self._lock, self.con:
            self.con.execute("INSERT OR REPLACE INTO loudness VALUES (?, ?, ?, ?)",
                             (key, subsong, lufs, int(time.time())))

    # ---- plays ----

    def start_play(self, song: Song) -> int:
        """Record that a song started; returns the play's id for finish_play()."""
        with self._lock, self.con:
            cur = self.con.execute(
                "INSERT INTO plays(path, format, artist, title, started) VALUES (?, ?, ?, ?, ?)",
                (song.key, song.format, song.artist, song.title, int(time.time())))
            return cur.lastrowid

    def finish_play(self, play_id: int, seconds: float) -> None:
        with self._lock, self.con:
            self.con.execute("UPDATE plays SET seconds = ? WHERE id = ?", (seconds, play_id))

    def history(self, index: Index, limit: int = 300) -> list[Song]:
        with self._lock:
            rows = self.con.execute("SELECT path, max(started) m FROM plays GROUP BY path"
                                    " ORDER BY m DESC LIMIT ?", (limit,)).fetchall()
        return [s for (p, _) in rows if (s := index.resolve(p))]

    def artist_stats(self, artist: str) -> dict:
        """Your own listening to one artist."""
        with self._lock:
            plays, seconds, songs = self.con.execute(
                "SELECT count(*), coalesce(sum(seconds), 0), count(DISTINCT path) FROM plays"
                " WHERE artist = ? AND seconds >= ?", (artist, self.COUNTED_SECONDS)).fetchone()
            top = self.con.execute(
                "SELECT title, count(*) n FROM plays WHERE artist = ? AND seconds >= ?"
                " GROUP BY path ORDER BY n DESC, max(started) DESC LIMIT 3",
                (artist, self.COUNTED_SECONDS)).fetchall()
        return {"plays": plays, "seconds": seconds, "songs": songs, "top": top}

    # ---- statistics ----

    # A play counts once you've listened this long; quick skips don't.
    COUNTED_SECONDS = 20

    def stats(self, since: int = 0, top: int = 10) -> dict:
        """Listening statistics for plays started at or after `since` (a Unix time)."""
        counted = "started >= ? AND seconds >= ?"
        args = (since, self.COUNTED_SECONDS)
        with self._lock:
            q = self.con.execute
            plays, seconds, songs = q(
                f"SELECT count(*), coalesce(sum(seconds), 0), count(DISTINCT path) FROM plays"
                f" WHERE {counted}", args).fetchone()
            first = q("SELECT min(started) FROM plays").fetchone()[0]
            top_songs = q(
                f"SELECT path, title, artist, format, count(*) n, sum(seconds) s FROM plays"
                f" WHERE {counted} GROUP BY path ORDER BY n DESC, s DESC LIMIT ?", (*args, top)).fetchall()
            top_artists = q(
                f"SELECT artist, count(*) n, sum(seconds) s FROM plays WHERE {counted}"
                f" AND artist NOT IN ('', '- unknown') GROUP BY artist ORDER BY n DESC, s DESC LIMIT ?",
                (*args, top)).fetchall()
            top_formats = q(
                f"SELECT format, count(*) n, sum(seconds) s FROM plays WHERE {counted}"
                f" GROUP BY format ORDER BY n DESC, s DESC LIMIT ?", (*args, top)).fetchall()
            per_day = q(
                f"SELECT date(started, 'unixepoch', 'localtime') d, count(*) n, sum(seconds) s"
                f" FROM plays WHERE {counted} GROUP BY d ORDER BY d", args).fetchall()
        return {"plays": plays, "seconds": seconds, "songs": songs, "first": first,
                "top_songs": top_songs, "top_artists": top_artists, "top_formats": top_formats,
                "per_day": per_day}
