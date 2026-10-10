# SPDX-License-Identifier: GPL-3.0-or-later
"""keygenmusic.tk: the music of keygens, cracks and trainers, about 4,350 tracker
modules (XM, MOD, IT, S3M) from some 190 release groups.

The site's own player loads its whole library from one file, /kgm/lib.txt: a JSON list
with, for every song, its path, the release group, what was cracked, and the module's
title. /kgm/ver.txt says when that list last changed (milliseconds since 1970). The
songs are files below /kgm/, laid out by group. The list has no sizes and no
checksums: a song's size is known once it is fetched, and a song that is in Modland
too shows twice in lists of every collection.

In the player a song is filed under its tracker's format (Fasttracker 2, Protracker,
Impulsetracker, Screamtracker 3), which picks the replayer, then under its group.
"""

from __future__ import annotations

import json
import time
import urllib.parse
import urllib.request
from typing import Iterable

from .. import capped
from .base import Collection, Entry, Listing, Progress, Transport, safe_path
from .transports import HttpTransport, mirrored

SITE = "https://keygenmusic.tk"
MUSIC = "/kgm/"
LIB = MUSIC + "lib.txt"
VERSION = MUSIC + "ver.txt"
FORMATS = {"xm": "Fasttracker 2", "mod": "Protracker", "it": "Impulsetracker",
           "s3m": "Screamtracker 3"}


def _get(path: str, timeout: int = 60) -> bytes:
    request = urllib.request.Request(SITE + path, headers={"User-Agent": "bbmp"})
    with urllib.request.urlopen(request, timeout=timeout) as r:
        return capped.read(r)


def parse(path: str, name: str = "", group: str = "") -> Entry:
    """"TSRh/TSRh - Opus Creator 9.01 kg.xm": filed under its tracker's format, then its
    group; `name` is what was cracked and `group` who did it, where the list says (the
    site keeps some groups' songs in folders for all, "!Others" and "!FromUsers")."""
    folder, _, file = path.partition("/")
    ext = file.rsplit(".", 1)[-1].lower() if "." in file else ""
    shown = f"{name}.{ext}" if name and ext else file
    if not group:
        group = folder if not folder.startswith("!") else file.split(" - ", 1)[0]
    return Entry(path, FORMATS.get(ext, "Fasttracker 2"), group or "- unknown", "", shown)


def read_list(raw: bytes, dated: int = 0, before: Iterable[str] = ()) -> Listing:
    """The site's lib.txt; `before`: the paths the catalog had, to tell what is new."""
    songs = json.loads(raw.decode("utf-8", "replace"))
    entries, seen = [], set()
    for song in songs:
        path = str(song.get("path", ""))
        if not path.startswith(MUSIC):
            continue
        path = path[len(MUSIC):]
        if path in seen or not safe_path(path):
            continue
        seen.add(path)
        entries.append(parse(path, str(song.get("sn") or "").strip(),
                             str(song.get("rg") or "").strip()))
    before = set(before)
    today = time.strftime("%Y-%m-%d")
    changes = [(today, "added", e.path) for e in entries if before and e.path not in before]
    return Listing(entries, (), changes, dated)


class KeygenMusic(Collection):
    id = "keygenmusic"
    name = "Keygen Music"
    home = SITE
    about = ("the music of keygens, cracks and trainers (keygenmusic.tk): about 4,350 "
             "tracker modules from 190 groups. Its catalog is a 0.8 MB list")
    servers = (SITE + MUSIC,)

    def latest(self) -> int:
        try:
            return int(_get(VERSION, 20).strip()) // 1000
        except ValueError:
            return 0

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        progress("Downloading keygenmusic.tk's list of songs...")
        raw = _get(LIB, 120)
        return read_list(raw, self.latest(), _paths_before(self.id, self.catalog_dir))

    def parse(self, path: str) -> Entry:
        return parse(path)

    def transport(self) -> Transport:
        return mirrored(self.id, MUSIC, [HttpTransport(SITE + MUSIC.rstrip("/"))])

    def url(self, path: str) -> str:
        return SITE + urllib.parse.quote(MUSIC + path)

    def path_in(self, url: str) -> str | None:
        """A link to a song: to its file, or the site's own "#track=" link to it."""
        if "keygenmusic" not in url:
            return None
        for mark in ("#track=", MUSIC):
            at = url.find(mark)
            if at >= 0:
                return urllib.parse.unquote(url[at + len(mark):])
        return None


def _paths_before(collection: str, folder=None) -> list[str]:
    """The songs the collection's catalog in the folder had, to tell what is new."""
    import sqlite3
    from .. import paths
    from ..index import read_only
    db = (folder if folder is not None else paths.CATALOG_DIR) / f"{collection}.db"
    if not db.is_file():
        return []
    try:
        con = sqlite3.connect(read_only(db), uri=True)
        try:
            return [p for (p,) in con.execute("SELECT path FROM files")]
        finally:
            con.close()
    except sqlite3.Error:
        return []
