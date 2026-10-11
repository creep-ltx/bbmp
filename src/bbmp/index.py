# SPDX-License-Identifier: GPL-3.0-or-later
"""The catalogs: each collection's file list in a searchable SQLite database of its
own, <id>.db in the catalog folder, read together as if they were one (Index)."""

from __future__ import annotations

import json
import os
import random
from array import array
import re
import sqlite3
import tempfile
import threading
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Callable, Iterable

from . import collection as collections_, companions, known_chips, paths, platforms
from .collection import DEFAULT, LOCAL, Listing

VERSION = "6"       # of a catalog's layout; a catalog with another one is made anew
                    # (6: more companion files and sound effects hidden, 2 Oct 2026)
OLD_VERSION = "4"   # the one catalog for every collection there was before, split when met
KEEP_DAYS = 60      # of what the collections changed
# the kinds of change that are shown, of songs that are there ("deleted" ones are not)
SHOWN_CHANGES = ("added", "updated", "moved")
# An index of the songs filed in Modland's "coop-Name" folders (6,566 of its files): an
# artist's page found them by going through every file, 33 ms; with it 0.3 ms (11
# October 2026). Catalogs made before it don't have it, and are asked as before.
COOP_INDEX = "files_coop"
# how catalogs from before read Modland's moves: a line each, the path the song had
RAW_MOVES = ("moved", "renamed", "to", "as")


def read_only(db: Path) -> str:
    """The URI that opens a database read-only, whatever its path has in it (#, ?, %)."""
    return Path(db).absolute().as_uri() + "?mode=ro"


@dataclass(frozen=True)
class Song:
    id: int
    path: str       # where it is in its collection, e.g. "AHX/Pink/inside.ahx"; for a local file, on disk
    format: str     # browse category, e.g. "Protracker" or "Video Game Music - Sega Megadrive"
    artist: str
    album: str
    name: str
    collection: str = DEFAULT
    size: int = 0   # of the file in bytes, without the files it needs; 0 when not known
    # the other collections that have the same file, where a list of every collection
    # shows it once (under `collection`, the first of them)
    also: tuple[str, ...] = ()

    @property
    def key(self) -> str:
        """What the song is saved as: "modland:AHX/Pink/inside.ahx", or a local file's path."""
        return collections_.key(self.collection, self.path)

    @property
    def title(self) -> str:
        return display_name(self.name)

    @property
    def local(self) -> bool:
        """A file on this computer rather than a song in a collection."""
        return self.collection == LOCAL

    @property
    def partner(self) -> str:
        """The co-composer, for songs in one of Modland's "coop-Name" folders."""
        first = self.album.split("/", 1)[0]
        return first[5:] if first.startswith("coop-") else ""

    @property
    def album_name(self) -> str:
        """The album or game without the coop folder."""
        parts = self.album.split("/")
        return "/".join(parts[1:] if parts[0].startswith("coop-") else parts)


def display_name(name: str) -> str:
    """'mdat.turrican 2' -> 'turrican 2', 'blues jive.mod' -> 'blues jive'."""
    low = name.lower()
    for prefix in ("mdat.", "jpn.", "sjs.", "max.", "uds.", "sdr.", "dns.", "tpu.", "thm.",
                   "mfp.", "ash.", "cust."):
        if low.startswith(prefix):
            return name[len(prefix):]
    if "." in name:
        stem, ext = name.rsplit(".", 1)
        if stem and len(ext) <= 8 and " " not in ext:
            return stem
    return name


SCHEMA = """
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
-- hosted: another collection has all of this one's songs (HVSC, which Modland has)
-- bytes: of all its files; dated: when the collection made its list
CREATE TABLE collections(id TEXT PRIMARY KEY, name TEXT, songs INTEGER, built INTEGER,
                         hosted INTEGER, bytes INTEGER, dated INTEGER);
CREATE TABLE files(
    id INTEGER PRIMARY KEY,
    collection TEXT NOT NULL,
    path TEXT NOT NULL,
    format TEXT NOT NULL,
    artist TEXT NOT NULL,
    album TEXT NOT NULL,
    name TEXT NOT NULL,
    hidden INTEGER NOT NULL,
    md5 TEXT NOT NULL,
    size INTEGER NOT NULL,
    UNIQUE(collection, path)
);
CREATE INDEX files_browse ON files(format, artist, hidden);
-- collection '' counts a format across every collection
-- bytes: of all the format's files, also the ones that belong to songs
CREATE TABLE formats(collection TEXT, format TEXT, songs INTEGER, artists INTEGER,
                     bytes INTEGER, PRIMARY KEY (collection, format));
-- what the collections say has changed of late: day is "2026-09-28", kind is
-- "added", "updated", "deleted", "moved" (path: where it is now) or what else the
-- collection calls it
CREATE TABLE changes(collection TEXT, day TEXT, kind TEXT, path TEXT);
CREATE INDEX changes_day ON changes(collection, day);
CREATE TABLE lengths(collection TEXT, path TEXT, lengths TEXT,
                     PRIMARY KEY (collection, path)) WITHOUT ROWID;
CREATE TABLE extensions(ext TEXT PRIMARY KEY, format TEXT) WITHOUT ROWID;
-- what songs say they play on, where their files were read (platforms.SONG_CHIPS)
CREATE TABLE chips(collection TEXT, path TEXT, chip TEXT,
                   PRIMARY KEY (collection, path, chip)) WITHOUT ROWID;
-- what the collection's keepers wrote about a song (HVSC's STIL)
CREATE TABLE notes(collection TEXT, path TEXT, text TEXT,
                   PRIMARY KEY (collection, path)) WITHOUT ROWID;
CREATE VIRTUAL TABLE fts USING fts5(
    name, artist, album, format,
    content='files', content_rowid='id',
    tokenize="unicode61 remove_diacritics 2"
);
"""


# words for one part of a song in a search: "artist:jester", "format:ahx"
SEARCH_FIELDS = {"artist": "artist", "by": "artist", "format": "format", "title": "name",
                 "album": "album", "game": "album"}
_TERM = re.compile(r'(-?)(?:(\w+):)?(?:"([^"]*)"?|(\S+))')


_VIEW_TERM = re.compile(r'(?:^|(?<=\s))(-?)(platform|chip):(?:"([^"]*)"?|(\S+))', re.I)


def view_terms(text: str) -> tuple[str, list[tuple[str, str | None, bool]]]:
    """The `platform:` and `chip:` of a search, taken out of it: (what is left, [(kind,
    the platform's or chip's id or None when there is no such one, left out)])."""
    terms = [(kind.lower(), platforms.find(kind.lower(), quoted or bare), minus == "-")
             for minus, kind, quoted, bare in _VIEW_TERM.findall(text)]
    return _VIEW_TERM.sub(" ", text).strip(), terms


def search_understood(text: str) -> bool:
    """Is there something to search for: words, or a platform or chip it knows?"""
    rest, terms = view_terms(text)
    return bool(search_query(rest)) or (bool(terms) and all(t[1] for t in terms))


def search_query(text: str) -> str:
    """What the user typed, as a query of the catalogs' full-text search.

    Every word must be there, as the start of a word: "jest" finds Jester. Words can
    be kept to one part of a song: artist:jester, format:ahx, title:elysium, album:,
    and "quotes" hold words together: artist:"purple motion". A minus in front leaves
    out what has it: jester -remix. "" when nothing is left to search for.
    """
    wanted, unwanted = [], []
    for minus, field, quoted, bare in _TERM.findall(text):
        column = SEARCH_FIELDS.get((field or "").lower())
        value = quoted if quoted else bare
        if field and column is None:        # "word:thing" that is no field: both words
            value = f"{field} {value}"
        words = re.findall(r"\w+", value)
        if not words:
            continue
        phrase = " AND ".join(f'"{w}"*' for w in words)
        term = f"{column} : ({phrase})" if column else f"({phrase})"
        (unwanted if minus else wanted).append(term)
    if not wanted:
        return ""
    return " AND ".join(wanted) + "".join(f" NOT {t}" for t in unwanted)


def file_keys(name: str) -> list[str]:
    """How a file name can identify its format: 'x.mod' -> ['.mod'], 'mdat.x' -> ['mdat.', ...].

    Amiga names put the type first ("mdat.turrican"), most others last ("song.xm").
    """
    low = name.lower()
    if "." not in low:
        return []
    keys = []
    ext, prefix = low.rsplit(".", 1)[1], low.split(".", 1)[0]
    if ext and len(ext) <= 10 and " " not in ext:
        keys.append("." + ext)
    if prefix and len(prefix) <= 5 and " " not in prefix:
        keys.append(prefix + ".")
    return keys


def _extension_formats(rows: Iterable[tuple[str, str]]) -> dict[str, str]:
    """For every extension and name prefix, the format that uses it most."""
    counts: dict[str, dict[str, int]] = {}
    total = 0
    for name, fmt in rows:
        total += 1
        for key in file_keys(name):
            by_format = counts.setdefault(key, {})
            by_format[fmt] = by_format.get(fmt, 0) + 1
    # An extension counts when a format really uses it, not for one stray file. (In a
    # catalog of a few songs, as in the tests, every file counts.)
    need_ext, need_prefix = (3, 10) if total >= 1000 else (1, 1)
    out = {}
    for key, by_format in counts.items():
        fmt, n = max(by_format.items(), key=lambda kv: kv[1])
        if n >= (need_ext if key.startswith(".") else need_prefix):
            out[key] = fmt
    return out


def _catalogs(folder: Path) -> dict[str, Path]:
    """The collections that have a catalog in the folder, in the layout this version
    reads: id -> file."""
    out = {}
    if not folder.is_dir():
        return out
    for f in sorted(folder.glob("*.db")):
        try:
            con = sqlite3.connect(read_only(f), uri=True)
            try:
                row = con.execute("SELECT value FROM meta WHERE key = 'version'").fetchone()
            finally:
                con.close()
        except sqlite3.Error:
            continue
        if row is not None and row[0] == VERSION:
            out[f.stem] = f
    return out


def _kept(folder: Path) -> dict[str, int]:
    """The collections that have a catalog, and when each was built."""
    out = {}
    for c, f in _catalogs(folder).items():
        con = sqlite3.connect(read_only(f), uri=True)
        try:
            out.update(con.execute("SELECT id, built FROM collections"))
        finally:
            con.close()
    return out


def last_changes(folder: Path | None = None) -> dict[str, str]:
    """For every collection with a catalog, the last day it has changes of."""
    folder = folder if folder is not None else paths.CATALOG_DIR
    out = {}
    for c, f in _catalogs(folder).items():
        con = sqlite3.connect(read_only(f), uri=True)
        try:
            (day,) = con.execute("SELECT max(day) FROM changes").fetchone()
        finally:
            con.close()
        if day:
            out[c] = day
    return out


class Shrank(ValueError):
    """A collection's new list has far fewer songs than its catalog: an error page or an
    empty answer read as a list, most likely. The catalog is kept as it was."""


def _too_small(new: int, old: int) -> bool:
    """Is a list of `new` songs, where the catalog had `old`, one not to believe? None
    at all, or under half of a catalog of 100 songs or more. No collection has ever
    lost half its songs at once; an error page read as a list has (7 October 2026)."""
    return old > 0 and (new == 0 or (old >= 100 and new < old // 2))


def build(listings: dict[str, Listing], folder: Path | None = None,
          progress: Callable[[str], None] = lambda s: None,
          built: dict[str, int] | None = None, failed: dict[str, str] | None = None) -> int:
    """Build the catalogs of collections from their listings, by their ids: a file each,
    <id>.db in the folder. Each replaces the one before it in one step when it is
    complete, and keeps the changes that one had. Gives the number of songs built.
    `built`: when the listings were downloaded, where that was not now.
    `failed`: at an update, a list far smaller than the catalog (_too_small) leaves the
    catalog as it was, and the collection's id is put here with why.
    """
    folder = folder if folder is not None else paths.CATALOG_DIR
    folder.mkdir(parents=True, exist_ok=True)
    for c, listing in listings.items():
        progress(f"Building the catalog of {collections_.name(c)}...")
        try:
            _build_one(c, listing, folder / f"{c}.db", (built or {}).get(c) or int(time.time()),
                       guard=failed is not None)
        except Shrank as e:
            failed[c] = str(e)
            progress(f"{collections_.name(c)}: {e}")
    ix = Index(folder)
    count = ix.song_count()
    # what the first look at the platforms and chips counts, counted now and kept
    # beside the catalogs (the artists of each format were, by Index)
    for c in ix.ids:
        ix._chip_counts(c)
    ix._said_formats()
    ix.close()
    progress(f"Catalogs ready: {count:,} songs.")
    return count


def _old_songs(db_path: Path) -> int:
    """The songs a catalog has; 0 where there is none, or it can't be read."""
    if not db_path.is_file():
        return 0
    try:
        con = sqlite3.connect(read_only(db_path), uri=True)
        try:
            (n,) = con.execute("SELECT count(*) FROM files WHERE hidden = 0").fetchone()
        finally:
            con.close()
        return n
    except sqlite3.Error:
        return 0


def _build_one(c: str, listing: Listing, db_path: Path, when: int, guard: bool = False) -> int:
    # a name of its own, which no other build uses, next to the catalog it replaces
    fd, name = tempfile.mkstemp(prefix=f".{c}-", suffix=".tmp", dir=db_path.parent)
    os.close(fd)
    tmp = Path(name)
    tmp.chmod(0o644)        # as a catalog is; mkstemp makes it the owner's alone
    con = sqlite3.connect(tmp)
    try:
        songs = _fill(c, listing, con, db_path, when)
        con.close()
        if guard and _too_small(songs, old := _old_songs(db_path)):
            raise Shrank(f"its list had {songs:,} songs where the catalog has {old:,}, "
                         "so the catalog was kept as it was")
        os.replace(tmp, db_path)
    except BaseException:
        con.close()
        tmp.unlink(missing_ok=True)
        raise
    return songs


def _changes(rows: Iterable[tuple[str, str, str]]) -> list[tuple[str, str, str]]:
    """What a collection says has changed, where a catalog from before has Modland's
    moves as the lines of its changelog ("moved", "to", "as", "renamed"), which name
    where a song was: those are let go, and the rest kept."""
    rows = list(rows)
    if any(kind in ("to", "as", "renamed") for _, kind, _ in rows):
        rows = [r for r in rows if r[1] not in RAW_MOVES]
    return rows


def _added_again(con: sqlite3.Connection, since: str | None) -> None:
    """Modland takes a folder away and puts it back when it has a new release of what
    is in it: on 2026-09-07 it deleted HVSC's 47,947 tunes, on the 8th it added 48,395.
    A file that was deleted and added again is not new but updated; and not even that
    where it was added on a day the catalog before (attached as "old") had no changes
    of yet, from `since` on, and that catalog had the same file."""
    con.execute("CREATE TEMP TABLE gone(path TEXT PRIMARY KEY) WITHOUT ROWID")
    con.execute("INSERT OR IGNORE INTO gone SELECT path FROM changes WHERE kind = 'deleted'")
    if since is not None:
        con.execute("DELETE FROM changes WHERE kind = 'added' AND day >= ? AND path IN ("
                    "SELECT f.path FROM gone g JOIN files f ON f.path = g.path"
                    " JOIN old.files o ON o.path = f.path AND o.md5 = f.md5 WHERE f.md5 != '')",
                    (since,))
    con.execute("UPDATE changes SET kind = 'updated' WHERE kind = 'added' AND path IN gone")
    con.execute("DROP TABLE gone")


def _fill(c: str, listing: Listing, con: sqlite3.Connection, db_path: Path, when: int) -> int:
    con.executescript(SCHEMA)
    con.executemany(
        "INSERT OR IGNORE INTO files(collection, path, format, artist, album, name,"
        " hidden, md5, size) VALUES (?,?,?,?,?,?,?,?,?)",
        ((c, e.path, e.format, e.artist, e.album, e.name, int(e.hidden), e.md5, e.size)
         for e in listing.entries))
    con.executemany("INSERT OR IGNORE INTO lengths VALUES (?, ?, ?)",
                    ((c, path, lengths) for path, lengths in listing.lengths))
    con.executemany("INSERT OR IGNORE INTO chips VALUES (?, ?, ?)",
                    ((c, path, chip) for path, chip in listing.chips))
    con.executemany("INSERT OR IGNORE INTO notes VALUES (?, ?, ?)",
                    ((c, path, text) for path, text in listing.notes or ()))
    # what the program knows of the songs' chips (known_chips.py), where it is the same file
    con.execute("CREATE TEMP TABLE shipped(path TEXT, md5 TEXT, chip TEXT)")
    con.executemany("INSERT INTO shipped VALUES (?, ?, ?)", known_chips.of(c))
    con.execute("INSERT OR IGNORE INTO chips SELECT ?, s.path, s.chip FROM shipped s"
                " JOIN files f ON f.path = s.path AND f.md5 = s.md5", (c,))
    con.execute("DROP TABLE shipped")
    # the changes that are new, and the ones the catalog had, for KEEP_DAYS days back
    con.executemany("INSERT INTO changes VALUES (?, ?, ?, ?)",
                    ((c, day, kind, path) for day, kind, path in _changes(listing.changes)))
    newest = None        # the first day of the changes the listing has, with a catalog before
    if db_path.exists():
        try:
            con.execute("ATTACH ? AS old", (str(db_path),))
            (first,) = con.execute("SELECT coalesce(min(day), '9') FROM changes").fetchone()
            old = con.execute("SELECT day, kind, path FROM old.changes WHERE day < ?",
                              (first,)).fetchall()
            con.executemany("INSERT INTO changes VALUES (?, ?, ?, ?)",
                            ((c, day, kind, path) for day, kind, path in _changes(old)))
            # and its files, to tell a file that was added again (_added_again)
            con.execute("SELECT path, md5 FROM old.files LIMIT 0").fetchall()
            newest = first
        except sqlite3.Error:
            pass            # an old catalog of another layout: its changes are let go
        try:
            # what the songs said of their chips, where the file is still the same one
            con.execute("INSERT OR IGNORE INTO chips SELECT ?, o.path, o.chip FROM old.chips o"
                        " JOIN old.files w ON w.path = o.path"
                        " JOIN files f ON f.path = o.path AND f.md5 = w.md5", (c,))
        except sqlite3.Error:
            pass            # a catalog from before songs' chips were kept
        if listing.notes is None:
            try:            # the notes weren't had this time: the ones there were
                con.execute("INSERT OR IGNORE INTO notes SELECT ?, path, text FROM old.notes",
                            (c,))
            except sqlite3.Error:
                pass
    _added_again(con, newest)
    con.commit()
    if db_path.exists():
        try:
            con.execute("DETACH old")
        except sqlite3.Error:
            pass
    days = [d for (d,) in con.execute("SELECT DISTINCT day FROM changes ORDER BY day DESC")]
    if len(days) > KEEP_DAYS:
        con.execute("DELETE FROM changes WHERE day < ?", (days[KEEP_DAYS - 1],))
    con.execute("INSERT INTO fts(rowid, name, artist, album, format)"
                " SELECT id, name, artist, album, format FROM files WHERE hidden = 0")
    # songs and artists are counted without the files that belong to songs, bytes with
    con.execute("INSERT INTO formats SELECT collection, format, sum(hidden = 0),"
                " count(DISTINCT CASE WHEN hidden = 0 THEN artist END), sum(size)"
                " FROM files GROUP BY format HAVING sum(hidden = 0) > 0")
    con.execute("CREATE INDEX files_artist ON files(artist)")
    # the songs in Modland's "coop-Name" folders, which artist_songs looks through
    con.execute(f"CREATE INDEX {COOP_INDEX} ON files(album) WHERE album LIKE 'coop-%'")
    con.execute("CREATE INDEX files_name ON files(name COLLATE NOCASE)")
    con.executemany("INSERT INTO extensions VALUES (?, ?)", _extension_formats(
        con.execute("SELECT name, format FROM files WHERE hidden = 0")).items())
    (songs,) = con.execute("SELECT count(*) FROM files WHERE hidden = 0").fetchone()
    (size,) = con.execute("SELECT coalesce(sum(size), 0) FROM files").fetchone()
    con.execute("INSERT INTO collections VALUES (?, ?, ?, ?, ?, ?, ?)",
                (c, collections_.name(c), songs, when, 0, size, listing.dated or 0))
    con.execute("INSERT INTO meta VALUES ('version', ?)", (VERSION,))
    con.commit()
    return songs


def remove(collection: str, folder: Path | None = None) -> None:
    """Take a collection's catalog away (the collection is turned off)."""
    folder = folder if folder is not None else paths.CATALOG_DIR
    (folder / f"{collection}.db").unlink(missing_ok=True)


def set_hosts(chosen: Iterable[str]) -> None:
    """A hosted collection is made from its host's list when the host is chosen too
    (the list is had anyway), else from a list of its own."""
    chosen = set(chosen)
    for c in collections_.known():
        if c.hosted_by:
            c.via_host = c.hosted_by in chosen


def update(progress: Callable[[str], None] = lambda s: None, only: Iterable[str] = (),
           db_path: Path | None = None, chosen: Iterable[str] | None = None,
           failed: dict[str, str] | None = None) -> int:
    """Download the file lists of the collections named (or of every one that has a
    catalog) and build their catalogs from them, in the catalog folder `db_path`.
    `chosen`: the collections the user has (if not said: the ones with a catalog and
    the ones named), which says where a hosted collection's list comes from.

    A collection that can't be reached keeps the catalog it has; `failed` is given
    its id and why. Raises OSError when that leaves nothing to build.
    """
    db_path = db_path if db_path is not None else paths.CATALOG_DIR
    folder = db_path
    have = _kept(folder)
    only = set(only) or set(have) or {c.id for c in collections_.known()}
    set_hosts(set(have) | only if chosen is None else chosen)
    wanted = [c for c in collections_.known() if c.id in only]
    since = last_changes(folder)
    for c in wanted:
        if hasattr(c, "since"):
            c.since = since.get(c.id, "")       # only the changes after it are asked for
        c.catalog_dir = folder
    failed = failed if failed is not None else {}
    listings = {}
    try:
        for c in wanted:
            try:
                listings[c.id] = c.listing(progress)
            except Exception as e:
                # whatever one collection's answer does (cut short, changed, not what
                # was asked for), the others are updated: it stopped them all before
                failed[c.id] = str(e) or type(e).__name__
                if c.id in have:
                    progress(f"Can't reach {c.name}; keeping its songs as they were.")
        if not listings and (failed or not wanted):
            raise OSError("; ".join(f"{collections_.name(c)}: {why}" for c, why in failed.items())
                          or "no such collection")
        for c in wanted:
            if c.id in listings and since.get(c.id):
                listings[c.id] = _new_chips(c.id, listings[c.id], progress)
        return build(listings, folder, progress, failed=failed)
    finally:
        for c in wanted:
            c.done()


def _new_chips(c: str, listing: Listing, progress: Callable[[str], None]) -> Listing:
    """At an update: what the songs that are new say they play on (a new HVSC's tunes
    their SID), read from their first bytes. The ones that came before were read by
    tools/sid_chips.py and tools/song_chips.py and come with the player (known_chips)."""
    if c not in ("modland", "hvsc"):
        return listing
    listing = replace(listing, entries=list(listing.entries), changes=list(listing.changes))
    added = {path for _, kind, path in listing.changes if kind in ("added", "updated")}
    new = [(e.path, e.format) for e in listing.entries if e.path in added]
    from . import song_chips
    found = song_chips.of_new(new, "HVSC/" if c == "hvsc" else "", progress)
    return replace(listing, chips=[*listing.chips, *found]) if found else listing


def move_old_catalog(folder: Path | None = None, old: Path | None = None,
                     progress: Callable[[str], None] = lambda s: None) -> bool:
    """Before there was a catalog per collection, there was one for all of them. Split
    it into the new ones, once, and take it away. Says whether there was one."""
    folder = folder if folder is not None else paths.CATALOG_DIR
    old = old if old is not None else paths.INDEX_DB
    if not old.is_file() or _catalogs(folder):
        return False
    from . import snapshot
    try:
        con = sqlite3.connect(read_only(old), uri=True)
        (version,) = con.execute("SELECT value FROM meta WHERE key = 'version'").fetchone()
        con.close()
    except (sqlite3.Error, TypeError):
        version = None
    if version == OLD_VERSION:
        progress("Making a catalog for each collection...")
        listings, built = snapshot.listings_of_old(old)
        build({c: l for c, l in listings.items() if collections_.get(c) is not None},
              folder, progress, built=built)
    old.unlink(missing_ok=True)
    return version == OLD_VERSION


class Index:
    """Read-only queries against the catalogs, as if they were one.

    The catalog of each collection is a file of its own (<id>.db in the folder). They
    are opened together: the tables of each are joined into views of the same names,
    so a query sees every collection, and a file's id is made unique by the catalog it
    is in (its id there * 64 + the catalog's number).

    Where a method takes `collection`, "" means every collection. Lists of songs from
    every collection show a file that two collections have only once. It is the same
    file when the checksums the collections publish are the same, never by its name.
    A collection whose host has a catalog too is left out of them altogether.
    """

    _SONG = "SELECT id, path, format, artist, album, name, collection, size, md5 FROM files"

    def __init__(self, db_path: Path | None = None):
        db_path = db_path if db_path is not None else paths.CATALOG_DIR
        self.path = db_path
        # only the collections the player knows
        found = {c: f for c, f in _catalogs(db_path).items() if collections_.get(c) is not None}
        order = [c.id for c in collections_.known()]
        self.ids = sorted(found, key=lambda c: (order.index(c) if c in order else len(order), c))
        self.ids = self.ids[:11]         # SQLite opens 11 files together at the most
        self._catalog_files = [found[c] for c in self.ids]
        self.schemas = ["main" if k == 0 else f"c{k}" for k in range(len(self.ids))]
        # a collection whose host is here is hosted; alone, it stands on its own
        self._hosted = [c for c in self.ids if (col := collections_.get(c)) is not None
                        and col.hosted_by and col.hosted_by in self.ids]
        self._stamp = self._stamp_now()     # the catalog files as they were opened
        self._kept_lock = threading.Lock()  # of the counts kept beside the catalogs
        self._chips: set[str] | None = None
        self._notes: list[str] | None = None
        self._coop: set[str] = set()        # the catalogs with COOP_INDEX
        self._said: set[tuple[str, str]] | None = None     # formats whose songs say (_by_songs)
        self._radio_choices: dict[tuple, array] = {}     # song ids (_by_id)
        # how many songs a radio picks from, counted once (catalogs that change are
        # opened as a new Index, which counts again)
        self._radio_counts: dict[tuple, int] = {}
        self.con = self._connect()
        # The screen, the workers and the player thread ask the catalogs at the same
        # time. With one connection a song's start waited behind whatever query ran:
        # a search 94 ms, the platform counts 335 ms, keep's plan of Modland seconds
        # (11 October 2026). A question takes a connection that is free, and another is
        # opened, up to CONNECTIONS, when none is: read-only ones run side by side.
        self._lock = threading.Condition()
        self._cons = [self.con]             # every connection, and the free ones
        self._free = [self.con]
        self._opening = 0
        self._changed = False               # the catalog files were replaced: no more opened
        self._closed = False
        self._extensions: dict[str, str] | None = None
        self._md5_kept: dict[str, bool] = {}       # catalog -> has it checksums (_has_md5)
        # where music is kept on this computer (keep.py), if the player has said
        self.kept_dir: Path | None = None

    CONNECTIONS = 3

    def _stamp_now(self) -> list | None:
        """(name, time, size) of each catalog file: what the counts kept beside them
        were counted from."""
        try:
            return [[f.name, (st := f.stat()).st_mtime_ns, st.st_size] for f in self._catalog_files]
        except OSError:
            return None

    def _connect(self) -> sqlite3.Connection:
        """A connection to every catalog, with the views that join them."""
        if self.ids:
            con = sqlite3.connect(read_only(self._catalog_files[0]), uri=True,
                                  check_same_thread=False)
            for k, f in enumerate(self._catalog_files[1:], 1):
                con.execute(f"ATTACH ? AS c{k}", (read_only(f),))
        else:
            con = sqlite3.connect(":memory:", check_same_thread=False)
        try:
            self._views(con)
            con.commit()        # no transaction is left open by the temp tables
        except BaseException:
            con.close()
            raise
        return con

    def _views(self, con: sqlite3.Connection) -> None:
        if not self.ids:
            con.executescript(SCHEMA.replace("CREATE TABLE ", "CREATE TEMP TABLE ")
                              .replace("CREATE INDEX ", "-- ")
                              .replace("CREATE VIRTUAL TABLE fts", "CREATE VIRTUAL TABLE temp.fts"))
            self._chips, self._notes, self._coop = set(), [], set()
            return
        s = self.schemas

        def union(sql: str) -> str:
            return " UNION ALL ".join(sql.format(s=schema, k=k) for k, schema in enumerate(s))
        con.execute("CREATE TEMP VIEW files AS " + union(
            "SELECT id * 64 + {k} AS id, collection, path, format, artist, album, name, hidden,"
            " md5, size FROM {s}.files"))
        for table in ("lengths", "changes", "extensions"):
            con.execute(f"CREATE TEMP VIEW {table} AS " + union(f"SELECT * FROM {{s}}.{table}"))
        # songs' chips, from the catalogs that have them (older ones don't)
        if self._chips is None:
            self._chips = {schema for schema in s if con.execute(
                f"SELECT 1 FROM {schema}.sqlite_master WHERE name = 'chips'").fetchone()}
            self._notes = [schema for schema in s if con.execute(
                f"SELECT 1 FROM {schema}.sqlite_master WHERE name = 'notes'").fetchone()]
            self._coop = {schema for schema in s if con.execute(
                f"SELECT 1 FROM {schema}.sqlite_master WHERE name = ?", (COOP_INDEX,)).fetchone()}
        having = [schema for schema in s if schema in self._chips]
        con.execute("CREATE TEMP VIEW notes AS " + (" UNION ALL ".join(
            f"SELECT collection, path, text FROM {schema}.notes" for schema in self._notes)
            or "SELECT '' AS collection, '' AS path, '' AS text WHERE 0"))
        con.execute("CREATE TEMP VIEW chips AS " + (" UNION ALL ".join(
            f"SELECT collection, path, chip FROM {schema}.chips" for schema in having)
            or "SELECT '' AS collection, '' AS path, '' AS chip WHERE 0"))
        hosted = ", ".join(f"'{c}'" for c in self._hosted) or "''"
        con.execute("CREATE TEMP VIEW collections AS " + union(
            "SELECT id, name, songs, built, id IN (" + hosted + ") AS hosted, bytes, dated"
            " FROM {s}.collections"))
        own_ids = [c for c in self.ids if c not in self._hosted]
        own = [schema for c, schema in zip(self.ids, s) if c not in self._hosted]
        # an artist that two collections have in a format is one artist of it: they are
        # counted once, when the catalogs are new, and kept beside them (it took 630 ms
        # when the catalogs weren't in memory yet, at the first start after a boot; 11
        # October 2026)
        counted = self._kept("format-artists " + ",".join(own_ids), lambda: con.execute(
            "SELECT format, count(*) FROM ("
            + " UNION ".join(f"SELECT DISTINCT format, artist FROM {schema}.files"
                             " WHERE hidden = 0" for schema in own)
            + ") GROUP BY format").fetchall())
        con.execute("CREATE TEMP TABLE all_artists(format TEXT PRIMARY KEY, artists INTEGER)")
        con.executemany("INSERT INTO all_artists VALUES (?, ?)", counted)
        con.execute("CREATE TEMP VIEW formats AS " + union(
            "SELECT collection, format, songs, artists, bytes FROM {s}.formats")
            + " UNION ALL SELECT '', f.format, sum(f.songs), a.artists, sum(f.bytes) FROM ("
            + " UNION ALL ".join(f"SELECT * FROM {schema}.formats" for schema in own)
            + ") f JOIN all_artists a ON a.format = f.format GROUP BY f.format")

    def _kept(self, key: str, count: Callable[[], object], file: str = "counts.json",
              section: str = "counts"):
        """What `count` gives, kept in a file beside the catalogs and counted again
        when they change (their names, times and sizes). As JSON: tuples come back
        as lists."""
        cache = self.path / file
        if self._stamp is None or not self.ids:
            return count()
        with self._kept_lock:
            try:
                kept = json.loads(cache.read_text())
            except (OSError, ValueError):
                kept = {}
            if isinstance(kept, dict) and kept.get("stamp") == self._stamp \
                    and isinstance(kept.get(section), dict) and key in kept[section]:
                return kept[section][key]
        value = count()
        value = json.loads(json.dumps(value))       # as it comes back from the file
        with self._kept_lock:
            if self._stamp_now() != self._stamp:
                return value        # the catalogs were replaced: these counts are of old ones
            try:
                kept = json.loads(cache.read_text())
            except (OSError, ValueError):
                kept = {}
            if not isinstance(kept, dict) or kept.get("stamp") != self._stamp \
                    or not isinstance(kept.get(section), dict):
                kept = {"stamp": self._stamp, section: {}}
            kept[section][key] = value
            try:
                tmp = cache.with_name(f"{cache.name}.{os.getpid()}.part")
                tmp.write_text(json.dumps(kept))
                os.replace(tmp, cache)
            except OSError:
                pass
        return value

    def _only(self, collection: str, prefix: str = "") -> tuple[str, tuple]:
        """The part of a query that keeps to a collection, or to every collection that
        isn't hosted by another."""
        if collection:
            return f" AND {prefix}collection = ?", (collection,)
        if not self._hosted:
            return "", ()
        return f" AND {prefix}collection NOT IN ({_marks(self._hosted)})", tuple(self._hosted)

    def _all(self, sql: str, args: tuple = ()) -> list[tuple]:
        con = self._take()
        try:
            return con.execute(sql, args).fetchall()
        finally:
            self._give(con)

    def _take(self) -> sqlite3.Connection:
        """A connection no other thread is using: a free one, a new one, or the next
        to be let go."""
        with self._lock:
            while True:
                if self._closed:
                    return self.con         # closed: asking it raises, as it always did
                if self._free:
                    return self._free.pop()
                if not self._changed and len(self._cons) + self._opening < self.CONNECTIONS:
                    self._opening += 1
                    break
                self._lock.wait()
        con = None
        try:
            # only to the files the first connection has: an update replaces them (and a
            # new Index is opened then), and this one's answers must agree with its own
            if self._stamp_now() == self._stamp:
                con = self._connect()
                if self._stamp_now() != self._stamp:
                    con.close()
                    con = None
        except sqlite3.Error:
            con = None
        with self._lock:
            self._opening -= 1
            if con is not None and not self._closed:
                self._cons.append(con)
                return con
            if con is not None:
                con.close()
            self._changed = True
            self._lock.notify_all()
        return self._take()

    def _give(self, con: sqlite3.Connection) -> None:
        with self._lock:
            if con not in self._free:
                self._free.append(con)
            self._lock.notify_all()

    def close(self) -> None:
        """Let go of the catalog files (an update has put new ones in their place).
        The Index can't be asked anything after it; closing it again does nothing.
        Waits for the questions being answered."""
        with self._lock:
            self._closed = True
            while len(self._free) < len(self._cons) or self._opening:
                self._lock.wait()
            for con in self._cons:
                con.close()

    def _songs(self, sql: str, args: tuple, once: bool = False) -> list[Song]:
        """The songs a query finds. With `once`, a file that an earlier row has from
        another collection is left out, and that row says the other collection has it
        too (Song.also)."""
        return self._merge(((song, md5) for *song, md5 in self._all(sql, args)), once)

    def _merge(self, rows, once: bool) -> list[Song]:
        songs: list[Song] = []
        seen: dict[str, tuple[str, list[int]]] = {}       # checksum -> (collection, rows)
        for song, md5 in rows:
            collection = song[6]
            if once and md5 and md5 in seen and seen[md5][0] != collection:
                for i in seen[md5][1]:
                    if collection not in songs[i].also:
                        songs[i] = replace(songs[i], also=songs[i].also + (collection,))
                continue
            if once and md5:
                seen.setdefault(md5, (collection, []))[1].append(len(songs))
            songs.append(Song(*song))
        if once and self._hosted:
            # a hosted collection has its host's files: the host's rows say so
            for i, s in enumerate(songs):
                for guest in self._hosted:
                    g = collections_.get(guest)
                    if g.hosted_by == s.collection and g.guest_path(s.path) is not None:
                        songs[i] = replace(songs[i], also=songs[i].also + (guest,))
        return songs

    def _locate(self, key: str) -> tuple[str, str]:
        """(collection, path) where the catalogs have a song. A hosted collection's
        songs are saved under their host's key; without the host's catalog they are
        found in the hosted collection's own."""
        collection, path = collections_.split(key)
        if collection == LOCAL or collection in self.ids:
            return collection, path
        for c in self.ids:
            guest = collections_.get(c)
            if guest is not None and guest.hosted_by == collection:
                inside = guest.guest_path(path)
                if inside is not None:
                    return c, inside
        return collection, path

    @staticmethod
    def exists(db_path: Path | None = None) -> bool:
        """Is there a catalog of at least one collection, in the layout this version reads?"""
        db_path = db_path if db_path is not None else paths.CATALOG_DIR
        return bool(_catalogs(db_path))

    def collections(self) -> list[tuple[str, str, int, int]]:
        """(id, name, songs, when its list was downloaded) of the collections that have
        a catalog."""
        return self._all("SELECT id, name, songs, built FROM collections"
                         " ORDER BY name COLLATE NOCASE")

    def age_days(self) -> float:
        """The age of the oldest file list in the catalogs."""
        built = [b for _, _, _, b in self.collections()]
        return (time.time() - min(built)) / 86400 if built else 1e9

    def about(self, collection: str) -> dict:
        """What the catalog knows of a collection: songs, bytes, built (when its list
        was downloaded), dated (when the collection made it)."""
        rows = self._all("SELECT songs, bytes, built, dated FROM collections WHERE id = ?",
                         (collection,))
        return dict(zip(("songs", "bytes", "built", "dated"), rows[0])) if rows else {}

    # ---- sizes: of all the files, also the ones that belong to songs ----

    def size(self, collection: str = "") -> int:
        return sum(self.format_sizes(collection).values())

    def format_sizes(self, collection: str = "") -> dict[str, int]:
        return dict(self._all("SELECT format, bytes FROM formats WHERE collection = ?",
                              (collection,)))

    # ---- platforms and chips (platforms.py): by format, or song by song ----

    def _said_formats(self) -> set[tuple[str, str]]:
        """(collection, format) of the formats whose songs the catalogs have read.

        Here and in view_formats and view_counts the songs' chips are joined to their
        files from the chips' side, on (collection, path), the files' key: joined on
        the path alone SQLite went through all of Modland's 516k files. CROSS JOIN keeps
        it in that order. A platform page: 248 ms -> 2 ms; the counts 375 -> 110 ms
        (11 October 2026)."""
        if self._said is None:
            said = set()
            for c, schema in zip(self.ids, self.schemas):
                if schema in self._chips:
                    said |= {(c, f) for (f,) in self._kept(f"said {c}", lambda: self._all(
                        f"SELECT DISTINCT f.format FROM {schema}.chips s CROSS JOIN {schema}.files f"
                        " ON f.collection = s.collection AND f.path = s.path"))}
            self._said = said
        return self._said

    def _chip_counts(self, collection: str) -> list[tuple[str, str, int]]:
        """(format, chip, songs) of what a collection's songs say they play on. Counted
        once for the catalogs and kept beside them: 110 ms for them all (11 October
        2026). What the counts mean (platforms.py) is worked out from them each time."""
        schema = self._schema(collection)
        if schema not in self._chips:
            return []
        return [tuple(r) for r in self._kept(f"chip-counts {collection}", lambda: self._all(
            f"SELECT f.format, s.chip, count(*) FROM {schema}.chips s CROSS JOIN {schema}.files f"
            " ON f.collection = s.collection AND f.path = s.path WHERE f.hidden = 0"
            " GROUP BY f.format, s.chip"))]

    def _by_songs(self, collection: str, fmt: str, kind: str, id_: str) -> bool:
        return (platforms.decided_by_songs(fmt, kind, id_)
                and (collection, fmt) in self._said_formats())

    def _on(self, collection: str, fmt: str, view) -> tuple[str, tuple]:
        """The condition for the songs of a format that are on a view's platform or chip,
        where the songs say it one by one; none where the format says it for all."""
        if not view or not collection or not self._by_songs(collection, fmt, *view):
            return "", ()
        return (" AND (collection, path) IN (SELECT collection, path FROM chips"
                " WHERE chip = ?)", (platforms.value(*view),))

    def _asked(self, collection: str) -> list[tuple[str, str]]:
        """(collection, schema) of the catalogs a question goes to: the collection's, or
        those of every collection that isn't hosted by another."""
        return [(c, schema) for c, schema in zip(self.ids, self.schemas)
                if (c == collection if collection else c not in self._hosted)]

    def _on_in(self, schema: str, collection: str, fmt: str, view) -> tuple[str, tuple]:
        """_on, for a question to one catalog."""
        if not view or not collection or not self._by_songs(collection, fmt, *view):
            return "", ()
        return (f" AND path IN (SELECT path FROM {schema}.chips WHERE chip = ?)",
                (platforms.value(*view),))

    def _schema(self, collection: str) -> str | None:
        return dict(zip(self.ids, self.schemas)).get(collection)

    def view_formats(self, collection: str, kind: str, id_: str) -> list[tuple[str, int]]:
        """(format, songs) of a collection on a platform or chip: whole formats, and of
        the formats whose songs say it, the songs that do."""
        schema = self._schema(collection)
        said = dict(self._all(
            f"SELECT f.format, count(*) FROM {schema}.chips s CROSS JOIN {schema}.files f"
            " ON f.collection = s.collection AND f.path = s.path WHERE s.chip = ? AND f.hidden = 0"
            " GROUP BY f.format", (platforms.value(kind, id_),))) if schema in self._chips else {}
        out = []
        for fmt, songs, _ in self.formats(collection):
            if self._by_songs(collection, fmt, kind, id_):
                if said.get(fmt):
                    out.append((fmt, said[fmt]))
            elif platforms.matches(fmt, collection, kind, id_):
                out.append((fmt, songs))
        return out

    def view_counts(self, collection: str, kind: str) -> dict[str, int]:
        """Songs of a collection on each platform, or on each chip."""
        out: dict[str, int] = {}
        for fmt, songs, _ in self.formats(collection):
            for id_ in platforms.of(fmt, collection)[0 if kind == "platform" else 1]:
                if not self._by_songs(collection, fmt, kind, id_):
                    out[id_] = out.get(id_, 0) + songs
        for fmt, chip, n in self._chip_counts(collection):
            is_platform = chip.startswith("platform:")
            if is_platform != (kind == "platform"):
                continue
            id_ = chip.removeprefix("platform:")
            if self._by_songs(collection, fmt, kind, id_):
                out[id_] = out.get(id_, 0) + n
        return out

    def artist_sizes(self, fmt: str, collection: str = "", view=None) -> dict[str, int]:
        sizes: dict[str, int] = {}
        for c, schema in self._asked(collection):
            on, more = self._on_in(schema, c, fmt, view)
            for artist, n in self._all(f"SELECT artist, sum(size) FROM {schema}.files"
                                       f" WHERE format = ?{on} GROUP BY artist", (fmt, *more)):
                sizes[artist] = sizes.get(artist, 0) + n
        return sizes

    def files_in(self, collection: str, fmt: str = "", artist: str = "",
                 album: str = "") -> list[tuple[str, str, int, int]]:
        """(path, checksum, bytes, does it belong to a song) of every file of a
        collection, of one of its formats, of an artist in a format, or of an album or
        game of theirs."""
        # asked of the collection's own catalog, which has its files only: so read in
        # the order they are kept, or by the format's index. Through the view of them
        # all, keep's plan of all of Modland took 660 ms, 240 ms like this, holding a
        # connection all the while (11 October 2026)
        schema = self._schema(collection)
        if schema is None:
            return []
        where, args = "1", []
        for column, value in (("format", fmt), ("artist", artist), ("album", album)):
            if value:
                where += f" AND {column} = ?"
                args.append(value)
        return [(path, md5 if len(md5) == 32 else "", size, hidden) for path, md5, size, hidden
                in self._all(f"SELECT path, md5, size, hidden FROM {schema}.files WHERE {where}"
                             " ORDER BY id", tuple(args))]

    def file_facts(self, collection: str, paths_: list[str]) -> list[tuple[str, str, int]]:
        """(path, checksum, bytes) of files by their paths."""
        out = []
        for i in range(0, len(paths_), 500):
            some = paths_[i:i + 500]
            out += [(path, md5 if len(md5) == 32 else "", size) for path, md5, size in self._all(
                "SELECT path, md5, size FROM files WHERE collection = ?"
                f" AND path IN ({_marks(some)})", (collection, *some))]
        return out

    def size_with_companions(self, song: Song) -> int:
        """What is downloaded to play the song."""
        if song.local:
            return 0
        keys = [collections_.split(k)[1] for k in self.companions(song.key)]
        collection = collections_.split(song.key)[0]
        extra = sum(n for (n,) in self._all(
            f"SELECT size FROM files WHERE collection = ? AND path IN ({_marks(keys)})",
            (collection, *keys))) if keys else 0
        return song.size + extra

    # ---- what is new ----

    def change_days(self, collection: str = "", moved: bool = False) -> list[tuple]:
        """(day, songs added, songs updated), the newest day first; with `moved`, also
        the songs moved or renamed: (day, added, updated, moved). Only songs that are
        there count: a day of deleted songs only is not one."""
        where, args = self._only(collection, "c.")
        # counting takes a quarter of a second on a fast computer, more on a slow one,
        # and the answer changes only with the catalogs: it is kept beside them
        rows = self._kept(collection or "*", lambda: self._all(
            "SELECT c.day, sum(c.kind = 'added'), sum(c.kind = 'updated'),"
            " sum(c.kind = 'moved') FROM changes c JOIN files f ON f.collection = c.collection"
            f" AND f.path = c.path AND f.hidden = 0 WHERE c.kind IN ({_marks(SHOWN_CHANGES)}){where}"
            " GROUP BY c.day ORDER BY c.day DESC", (*SHOWN_CHANGES, *args)),
            file="change-days.json", section="days")
        rows = [tuple(r) for r in rows]
        return rows if moved else [row[:3] for row in rows]

    def changed(self, day: str, collection: str = "") -> list[tuple[str, Song]]:
        """(what happened: "added", "updated" or "moved", the song) for the songs that
        changed on a day and are there."""
        where, args = self._only(collection, "f.")
        rows = self._all(
            "SELECT c.kind, f.id, f.path, f.format, f.artist, f.album, f.name, f.collection,"
            " f.size FROM changes c JOIN files f ON f.collection = c.collection"
            f" AND f.path = c.path AND f.hidden = 0 WHERE c.day = ?"
            f" AND c.kind IN ({_marks(SHOWN_CHANGES)}){where}"
            " ORDER BY f.format, f.artist, f.name", (day, *SHOWN_CHANGES, *args))
        return [(kind, Song(*song)) for kind, *song in rows]

    def song_count(self, collection: str = "") -> int:
        return sum(n for _, n, _ in self.formats(collection))

    # ---- browsing ----

    def formats(self, collection: str = "") -> list[tuple[str, int, int]]:
        """(format, songs, artists), alphabetical."""
        return self._all("SELECT format, songs, artists FROM formats WHERE collection = ?"
                         " ORDER BY format COLLATE NOCASE", (collection,))

    def artists(self, fmt: str, collection: str = "", view=None) -> list[tuple[str, int]]:
        """(artist, songs) in a format; `view` (kind, id): only the songs on it.
        Each catalog is asked on its own and the counts added up here: through the
        merged view SQLite can't use the index's order, 10 times slower (55 ms for
        Protracker, 5 ms like this; 3 October 2026)."""
        counts: dict[str, int] = {}
        for c, schema in self._asked(collection):
            on, more = self._on_in(schema, c, fmt, view)
            for artist, n in self._all(
                    f"SELECT artist, count(*) FROM {schema}.files WHERE format = ?"
                    f" AND hidden = 0{on} GROUP BY artist", (fmt, *more)):
                counts[artist] = counts.get(artist, 0) + n
        return sorted(counts.items(), key=lambda a: nocase(a[0]))

    def all_artists(self, collection: str = "") -> list[tuple[str, int]]:
        """(artist, songs) in every format, A to Z; counted once for each catalog that
        is opened (the catalogs don't change while open)."""
        kept = self.__dict__.setdefault("_all_artists", {})
        if collection not in kept:
            counts: dict[str, int] = {}
            for _, schema in self._asked(collection):
                for artist, n in self._all(f"SELECT artist, count(*) FROM {schema}.files"
                                           " WHERE hidden = 0 GROUP BY artist"):
                    counts[artist] = counts.get(artist, 0) + n
            kept[collection] = sorted(counts.items(), key=lambda a: nocase(a[0]))
        return kept[collection]

    def songs(self, fmt: str, artist: str, collection: str = "", view=None) -> list[Song]:
        where, args = self._only(collection)
        on, more = self._on(collection, fmt, view)
        songs = self._songs(f"{self._SONG} WHERE format = ? AND artist = ? AND hidden = 0{where}"
                            f"{on} ORDER BY id", (fmt, artist, *args, *more), once=not collection)
        return sorted(songs, key=lambda s: (s.album.lower(), s.title.lower()))

    def formats_on(self, kind: str, wanted: str, collection: str = "") -> dict[str, list[str]]:
        """The formats of each collection whose songs are on the platform, or play on
        the chip (platforms.py): {collection: [format, ...]}, the collections that have
        none left out."""
        out = {}
        for c in self.ids:
            if collection and c != collection:
                continue
            found = [f for f, _ in self.view_formats(c, kind, wanted)]
            if found:
                out[c] = found
        return out

    def _searched(self, text: str, collection: str):
        """What search and search_count ask each catalog: (full-text query or "", and by
        collection the condition its songs are to meet, with its arguments), or None
        when nothing is asked."""
        rest, terms = view_terms(text)
        match = search_query(rest)
        if (not match and not terms) or any(t[1] is None for t in terms):
            return None
        asked: dict[str, tuple[str, tuple]] = {}
        for c, schema in zip(self.ids, self.schemas):
            if (collection and c != collection) or (not collection and c in self._hosted):
                continue
            sql, args, out = "", (), False
            for kind, id_, minus in terms:
                whole, songs = [], []
                for fmt, _, _ in self.formats(c):
                    if self._by_songs(c, fmt, kind, id_):
                        songs.append(fmt)
                    elif platforms.matches(fmt, c, kind, id_):
                        whole.append(fmt)
                parts, more = [], ()
                if whole:
                    parts.append(f"f.format IN ({_marks(whole)})")
                    more += tuple(whole)
                if songs:
                    parts.append(f"(f.format IN ({_marks(songs)}) AND f.path IN"
                                 f" (SELECT path FROM {schema}.chips WHERE chip = ?))")
                    more += (*songs, platforms.value(kind, id_))
                if not parts and not minus:
                    out = True              # none of its songs is on it
                    break
                if parts:
                    sql += f" AND {'NOT ' if minus else ''}({' OR '.join(parts)})"
                    args += more
            if not out:
                asked[c] = (sql, args)
        return match, asked

    def search(self, text: str, limit: int = 5000, collection: str = "") -> list[Song]:
        """Songs whose title, artist, album or format has the words (see search_query),
        and that are on the platform or chip that `platform:` and `chip:` say. Each
        catalog has its own full-text index: they are asked one by one, and the
        answers put in order of how well they match."""
        asked = self._searched(text, collection)
        if asked is None:
            return []
        match, asked = asked
        rows = []
        for k, (c, schema) in enumerate(zip(self.ids, self.schemas)):
            if c not in asked:
                continue
            on, more = asked[c]
            if match:
                rows += self._all(
                    f"SELECT f.id * 64 + {k}, f.path, f.format, f.artist, f.album, f.name,"
                    f" f.collection, f.size, f.md5, t.rank FROM {schema}.fts(?) AS t"
                    f" JOIN {schema}.files f ON f.id = t.rowid WHERE 1{on}"
                    " ORDER BY t.rank LIMIT ?", (match, *more, limit))
            else:                           # a platform or a chip alone: its songs by name
                rows += self._all(
                    f"SELECT f.id * 64 + {k}, f.path, f.format, f.artist, f.album, f.name,"
                    f" f.collection, f.size, f.md5, 0 FROM {schema}.files f"
                    f" WHERE f.hidden = 0{on} ORDER BY f.name COLLATE NOCASE LIMIT ?",
                    (*more, limit))
        rows.sort(key=lambda r: r[-1])
        return self._merge(((song, md5) for *song, md5, _ in rows[:limit]), not collection)

    def search_count(self, text: str, collection: str = "") -> int:
        """How many songs a search finds, without a limit (a file that two collections
        have counts twice)."""
        asked = self._searched(text, collection)
        if asked is None:
            return 0
        match, asked = asked
        n = 0
        for c, schema in zip(self.ids, self.schemas):
            if c not in asked:
                continue
            on, more = asked[c]
            if match:
                n += self._all(f"SELECT count(*) FROM {schema}.fts(?) AS t JOIN {schema}.files f"
                               f" ON f.id = t.rowid WHERE 1{on}", (match, *more))[0][0]
            else:
                n += self._all(f"SELECT count(*) FROM {schema}.files f WHERE f.hidden = 0{on}",
                               more)[0][0]
        return n

    def view_condition(self, kind: str, id_: str, collection: str = "") -> tuple[str, tuple]:
        """" AND ..." for the files of the songs on a platform or chip (the files view's
        columns), in one collection or all of them; with its arguments."""
        whole, by_songs = [], []
        for c in self.ids:
            if (collection and c != collection) or (not collection and c in self._hosted):
                continue
            for fmt, _, _ in self.formats(c):
                if self._by_songs(c, fmt, kind, id_):
                    by_songs.append((c, fmt))
                elif platforms.matches(fmt, c, kind, id_):
                    whole.append((c, fmt))
        parts, args = [], ()
        if whole:
            parts.append(f"(collection, format) IN (VALUES {', '.join('(?, ?)' for _ in whole)})")
            args += tuple(x for pair in whole for x in pair)
        if by_songs:
            parts.append(f"((collection, format) IN (VALUES {', '.join('(?, ?)' for _ in by_songs)})"
                         " AND (collection, path) IN (SELECT collection, path FROM chips"
                         " WHERE chip = ?))")
            args += (*(x for pair in by_songs for x in pair), platforms.value(kind, id_))
        if not parts:
            return " AND 0", ()
        return f" AND ({' OR '.join(parts)})", args

    def random_song(self, fmt: str | None = None, artist: str | None = None,
                    playable: Callable[[Song], bool] | None = None, tries: int = 200,
                    collection: str = "", view: tuple[str, str] | None = None) -> Song | None:
        """A random song, optionally inside one format, artist or collection, and on the
        platform or chip of a view."""
        where, args = ["hidden = 0"], []
        for column, value in (("format", fmt), ("artist", artist)):
            if value:
                where.append(f"{column} = ?")
                args.append(value)
        kept, which = self._only(collection)
        cond = " AND ".join(where) + kept
        args += which
        if view:
            on, more = self.view_condition(*view, collection)
            cond += on
            args += more
        # which songs there are to pick from is found once for a radio, and a song is
        # picked from them at once: counting through the catalog for every song took a
        # fifth of a second, in the thread that plays
        # (in a view; without one, a song is taken at a random place of the catalog,
        # and the list of every song would take 50 MB)
        if view:
            key = (fmt, artist, collection, view)
            choices = self._radio_choices.get(key)
            if choices is None:
                # by their ids, 8 bytes each: as (collection, path) a platform's 100k
                # songs took 20 MB (11 October 2026)
                choices = array("q", (i for (i,) in self._all(
                    f"SELECT id FROM files WHERE {cond}", tuple(args))))
                self._radio_choices = {key: choices}        # one radio at a time
            for _ in range(tries if choices else 0):
                song = self._by_id(random.choice(choices))
                if song and (playable is None or playable(song)):
                    return song
            return None
        key = (fmt, artist, collection)
        n = self._radio_counts.get(key)
        if n is None:
            (n,) = (self._all(f"SELECT count(*) FROM files WHERE {cond}", tuple(args))
                    or [(0,)])[0]
            self._radio_counts[key] = n = n or 0
        if not n:
            return None
        for _ in range(tries):
            songs = self._songs(f"{self._SONG} WHERE {cond} LIMIT 1 OFFSET ?",
                                (*args, random.randrange(n)))
            if songs and (playable is None or playable(songs[0])):
                return songs[0]
        return None

    # ---- one song ----

    def _by_id(self, id_: int) -> Song | None:
        """A song by its id in the merged files view (its id in its catalog * 64 + the
        catalog's number), asked of its own catalog by its key."""
        rowid, k = divmod(id_, 64)
        if not 0 <= k < len(self.schemas):
            return None
        found = self._songs(f"SELECT id * 64 + {k}, path, format, artist, album, name, collection,"
                            f" size, md5 FROM {self.schemas[k]}.files WHERE id = ?", (rowid,))
        return found[0] if found else None

    def by_path(self, path: str, collection: str = DEFAULT) -> Song | None:
        found = self._songs(f"{self._SONG} WHERE collection = ? AND path = ?", (collection, path))
        return found[0] if found else None

    def resolve(self, key: str) -> Song | None:
        """The song that was saved as `key`, in a collection or on this computer; as
        lists of every collection show it, with the other collections that have it
        (Song.also)."""
        collection, path = self._locate(key)
        if collection == LOCAL:
            return self.local_song(path)
        rows = self._all(f"{self._SONG} WHERE collection = ? AND path = ?", (collection, path))
        if not rows:
            return None
        md5 = rows[0][-1]
        if collection not in self._hosted and md5:
            # asked of each other catalog on its own: of them all, it would go through
            # Modland's half a million files for a song of Modland's
            for k, (c, schema) in enumerate(zip(self.ids, self.schemas)):
                if c != collection and c not in self._hosted and self._has_md5(schema):
                    rows += self._all(
                        f"SELECT id * 64 + {k}, path, format, artist, album, name, collection,"
                        f" size, md5 FROM {schema}.files WHERE md5 = ? AND hidden = 0"
                        " ORDER BY id", (md5,))
        return self._merge(((song, md5) for *song, md5 in rows), once=True)[0]

    def _has_md5(self, schema: str) -> bool:
        """Does a catalog have checksums at all? ASMA's, ZX-Art's, Keygenmusic's and
        SNDH's have none, and asking one of them for a checksum went through all its
        files for nothing: 300 songs of History took 299 ms, 4 ms without (11 October
        2026). Found once for each catalog that is opened."""
        if schema not in self._md5_kept:
            self._md5_kept[schema] = bool(self._all(
                f"SELECT 1 FROM {schema}.files WHERE length(md5) = 32 LIMIT 1"))
        return self._md5_kept[schema]

    def copies(self, song: Song) -> list[Song]:
        """The same song in the other collections that have it (the ones Song.also
        names), each as that collection has it."""
        out = []
        md5 = self.md5(song.key)
        for c in song.also:
            guest = collections_.get(c)
            if guest is not None and guest.hosted_by == song.collection:
                path = guest.guest_path(song.path)
                found = self.by_path(path, c) if path is not None else None
                out += [found] if found else []
            elif md5:
                out += self._songs(f"{self._SONG} WHERE collection = ? AND md5 = ? AND hidden = 0"
                                   " ORDER BY id", (c, md5))
        return out

    def moved(self, key: str, md5: str) -> str | None:
        """Where the song saved as `key`, with this checksum, is now in its collection,
        when the collection has moved or renamed it: its new key, if there is one place."""
        collection, _ = self._locate(key)
        if collection == LOCAL or collection not in self.ids or len(md5) != 32:
            return None
        found = self._songs(f"{self._SONG} WHERE collection = ? AND md5 = ? AND hidden = 0",
                            (collection, md5))
        return found[0].key if len(found) == 1 else None

    def note(self, song: Song) -> str:
        """What the collection's keepers wrote about a song (HVSC's STIL), or ""; a tune
        of HVSC's has its note whether it is come by as HVSC's or as Modland's."""
        collection, path = song.collection, song.path
        guest = collections_.get("hvsc")
        if guest is not None and collection == guest.hosted_by:
            inside = guest.guest_path(path)
            if inside is not None:
                collection, path = "hvsc", inside
        for schema in self._notes:
            found = self._all(f"SELECT text FROM {schema}.notes WHERE collection = ?"
                              " AND path = ?", (collection, path))
            if found:
                return found[0][0]
        return ""

    def md5(self, key: str) -> str:
        """The checksum the collection's file list has for a file ("" if it has none)."""
        collection, path = self._locate(key)
        row = (self._all("SELECT md5 FROM files WHERE collection = ? AND path = ?",
                         (collection, path)) or [("",)])[0]
        return row[0] if len(row[0]) == 32 else ""

    def lengths(self, key: str) -> list[float]:
        """How long each subsong plays, in seconds, where the collection says (else empty)."""
        collection, path = self._locate(key)
        row = (self._all("SELECT lengths FROM lengths WHERE collection = ? AND path = ?",
                         (collection, path)) or [None])[0]
        out = []
        for t in (row[0].split() if row else ()):
            m, _, sec = t.partition(":")
            try:
                out.append(int(m) * 60 + float(sec.split("(")[0]))
            except ValueError:
                out.append(0.0)
        return out

    def companions(self, key: str) -> list[str]:
        """The keys of the other files that must sit next to a song for it to play."""
        collection, path = self._locate(key)
        if collection == LOCAL:
            return []       # they are next to it already
        folder, _, name = path.rpartition("/")
        song = self.by_path(path, collection)
        family = song.format.split(" - ", 1)[0] if song else ""
        rows = self._all(
            "SELECT path, hidden FROM files WHERE collection = ? AND path > ? AND path < ?",
            (collection, folder + "/", folder + "/\uffff"))
        whole = family in companions.WHOLE_FOLDER
        found = [p for p, hidden in rows if p != path and (
            companions.goes_with(name, p[len(folder) + 1:], family) if hidden
            else whole and "/" not in p[len(folder) + 1:])]
        if family in companions.FORMAT_WIDE and song is not None:
            found += [p for (p,) in self._all(
                "SELECT path FROM files WHERE collection = ? AND format = ? AND hidden = 1"
                " AND path != ?", (collection, song.format, path)) if p not in found]
        shared = companions.SHARED_FOLDERS.get(family)
        if shared:
            there = f"{path.split('/', 1)[0]}/{shared}/"
            found += [p for (p,) in self._all(
                "SELECT path FROM files WHERE collection = ? AND hidden = 1 AND path > ?"
                " AND path < ?", (collection, there, there + "\uffff"))]
        return [collections_.key(collection, p) for p in found]

    def relocate(self, key: str) -> Song | None:
        """Where a song went that is no longer where `key` says, if that is beyond doubt.

        Collections rename folders, and Modland gives "- unknown" songs their artist
        once it is known. A song counts as found when exactly one file in the collection
        has the same name and it is by the same artist, or the old path named no artist
        and the format is the same.
        """
        collection, path = collections_.split(key)
        source = collections_.get(collection)
        if source is None or path.count("/") < 2:
            return None
        try:
            old = source.parse(path)
        except (NotImplementedError, ValueError, IndexError):
            return None
        rows = self._songs(f"{self._SONG} WHERE name = ? COLLATE NOCASE AND hidden = 0"
                           " AND collection = ?", (old.name, collection))
        same_artist = [s for s in rows if s.artist == old.artist]
        if len(same_artist) == 1:
            return same_artist[0]
        if old.artist == "- unknown":
            same_format = [s for s in rows if s.format == old.format]
            if len(same_format) == 1:
                return same_format[0]
        return None

    # ---- artists and albums ----

    def artist_songs(self, artist: str) -> list[Song]:
        """Everything by an artist, in every format and collection, including what
        they co-wrote."""
        args = (f"coop-{artist}", "coop-" + _like(artist) + "/%")
        rows = []
        for c, schema in self._asked(""):
            k = self.ids.index(c)
            song = (f"SELECT id * 64 + {k}, path, format, artist, album, name, collection, size,"
                    f" md5 FROM {schema}.files")
            if schema in self._coop:
                # by artist, and among the coop folders' songs (the index has them)
                rows += self._all(
                    f"{song} WHERE hidden = 0 AND artist = ? UNION {song} INDEXED BY {COOP_INDEX}"
                    " WHERE album LIKE 'coop-%' AND hidden = 0"
                    " AND (album = ? OR album LIKE ? ESCAPE '\\')", (artist, *args))
            else:
                rows += self._all(f"{song} WHERE hidden = 0 AND (artist = ? OR album = ?"
                                  " OR album LIKE ? ESCAPE '\\')", (artist, *args))
        rows.sort(key=lambda r: r[0])
        songs = self._merge(((song, md5) for *song, md5 in rows), once=True)
        return sorted(songs, key=lambda s: (s.format.lower(), s.album_name.lower(),
                                                  s.title.lower()))

    def album_songs(self, song: Song) -> list[Song]:
        """The songs filed under the same album or game as `song`."""
        songs = self._songs(f"{self._SONG} WHERE format = ? AND artist = ? AND album = ?"
                            " AND collection = ? AND hidden = 0",
                            (song.format, song.artist, song.album, song.collection))
        return sorted(songs, key=lambda s: s.title.lower())

    # ---- files on this computer ----

    def extension_formats(self) -> dict[str, str]:
        """'.mod' -> 'Protracker', 'mdat.' -> 'TFMX', ... from what the catalogs' formats use."""
        if self._extensions is None:
            # Several catalogs may name one extension: ZX-Art files .bbsong under "Spectrum",
            # Modland under "Beepola". The catalogs come in the collections' order (Modland
            # first) and the first one stays, unless a later one names a format with a
            # replayer of its own where the earlier one doesn't.
            from .backends.formats import BACKEND_BY_FORMAT
            known: dict[str, str] = {}
            for ext, fmt in self._all("SELECT ext, format FROM extensions"):
                had = known.get(ext)
                if had is None or (fmt in BACKEND_BY_FORMAT and had not in BACKEND_BY_FORMAT):
                    known[ext] = fmt
            self._extensions = known
        return self._extensions

    def guess_format(self, name: str) -> str | None:
        known = self.extension_formats()
        return next((known[k] for k in file_keys(name) if k in known), None)

    def local_song(self, path: str | Path) -> Song | None:
        """A Song for a music file on disk, or None if it doesn't look like one. A song
        inside an archive is "<archive>#<name inside>" (archives.py)."""
        from . import archives
        inside = archives.split(str(path))
        if inside is not None:
            archive, name = inside
            return next((s for s in self.archive_songs(archive)
                         if s.path == archives.join(archive.resolve(), name)), None)
        path = Path(path).expanduser().resolve()
        if self.kept_dir is not None and path.is_relative_to(self.kept_dir.resolve()):
            # a kept file is the collection's song, with its favorites and its history
            folder, *rest = path.relative_to(self.kept_dir.resolve()).parts or ("",)
            collection = collections_.by_folder(folder)
            if collection is not None and rest:
                found = self._songs(f"{self._SONG} WHERE collection = ? AND path = ?"
                                    " AND hidden = 0", (collection, "/".join(rest)))
                return found[0] if found and path.is_file() else None
        fmt = self.guess_format(path.name)
        if fmt is None or not path.is_file():
            return None
        if companions.is_companion(fmt.split(" - ", 1)[0], path.name):
            return None
        return Song(-1, str(path), fmt, "", path.parent.name, path.name, LOCAL,
                    path.stat().st_size)

    def archive_songs(self, archive: str | Path) -> list[Song]:
        """The songs inside an archive (archives.py), as songs on this computer."""
        from . import archives
        archive = Path(archive).expanduser().resolve()
        out = []
        for name, size in sorted(archives.members(archive), key=lambda m: m[0].lower()):
            base = name.rsplit("/", 1)[-1]
            fmt = self.guess_format(base)
            if fmt is None or companions.is_companion(fmt.split(" - ", 1)[0], base):
                continue
            out.append(Song(-1, archives.join(archive, name), fmt, "", archive.stem, base,
                            LOCAL, size))
        return out

    def local_songs(self, folder: str | Path, recursive: bool = False) -> list[Song]:
        folder = Path(folder).expanduser()
        files = folder.rglob("*") if recursive else folder.iterdir()
        songs = [s for f in sorted(files, key=lambda f: str(f).lower()) if (s := self.local_song(f))]
        return songs


_ASCII_LOWER = str.maketrans("ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz")


def nocase(text: str) -> str:
    """A sort key in the order of SQLite's COLLATE NOCASE: only A-Z fold."""
    return text.translate(_ASCII_LOWER)


def _marks(values: list) -> str:
    return ", ".join("?" for _ in values)


def _like(text: str) -> str:
    return text.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")
