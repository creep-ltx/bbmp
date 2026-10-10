# SPDX-License-Identifier: GPL-3.0-or-later
"""ASMA, the Atari SAP Music Archive (asma.atari.org): the music of the Atari 8-bit
computers, about 6,400 tunes, twice what Modland has of them.

Its site invites people to download the songs one by one, or all of them as one zip,
and its web player loads everything it knows from one file, /asmadb/asmadb.js: a
JavaScript object whose "fileInfos" list has, for every song, its path below /asma/,
title, author ("Real Name (Handle)"), year, size, subsongs and whether it is stereo.
The songs are SAP files (the POKEY chip's music with its 6502 player), played by
ZXTune as Modland's are; 70 are TIATracker songs (.ttt, JSON), Atari 2600 music made
with Kylearan's tracker (older catalogs call them "Theta Music Composer").

In the player a song is filed under the format Modland gives SAP files, "Slight Atari
Player", then under its composer's handle.
"""

from __future__ import annotations

import email.utils
import json
import re
import time
import urllib.parse
import urllib.request
from typing import Iterable

from .. import capped
from .base import Collection, Entry, Listing, Progress, Transport, safe_path
from .keygenmusic import _paths_before
from .transports import HttpTransport, mirrored

SITE = "https://asma.atari.org"
MUSIC = "/asma/"
DB = "/asmadb/asmadb.js"
FORMATS = {"sap": "Slight Atari Player", "ttt": "TIATracker"}


def _open(path: str, timeout: int = 60, head: bool = False):
    request = urllib.request.Request(SITE + path, headers={"User-Agent": "bbmp"},
                                     method="HEAD" if head else "GET")
    return urllib.request.urlopen(request, timeout=timeout)


def _array(js: str, name: str) -> list:
    """The JSON list after `"name":` in the site's JavaScript."""
    at = js.find(f'"{name}"')
    if at < 0:
        return []
    start = js.find("[", at)
    depth, quoted, escaped = 0, False, False
    for i in range(start, len(js)):
        ch = js[i]
        if quoted:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == '"':
                quoted = False
        elif ch == '"':
            quoted = True
        elif ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth == 0:
                return json.loads(js[start:i + 1])
    return []


def artist_of(author: str) -> str:
    """"Dominique Berger (StaxX)" -> "StaxX"; "<?> (Aki)" -> "Aki"; "Jakub Husak" as it
    is; "<?>" -> "- unknown", as Modland says it. Several, "A (x) & B (y)", -> "x & y"."""
    people = []
    for one in re.split(r"\s+&\s+", author.strip()):
        one = one.replace("<?>", "").strip()        # "<?> (MusicMarius) <?>" too
        handle = re.search(r"\(([^()]+)\)\s*$", one)
        name = (handle.group(1) if handle else one).strip()
        if name:
            people.append(name)
    return " & ".join(people) or "- unknown"


def entry(info: dict) -> Entry | None:
    path = str(info.get("filePath") or "")
    if not safe_path(path):
        return None
    ext = path.rsplit(".", 1)[-1].lower() if "." in path else ""
    title = str(info.get("title") or "").strip() or path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
    size = info.get("fileSize")
    return Entry(path, FORMATS.get(ext, "Slight Atari Player"),
                 artist_of(str(info.get("author") or "")), "", f"{title}.{ext}", False, "",
                 int(size) if isinstance(size, int) and size >= 0 else 0)


def read_list(js: bytes, dated: int = 0, before: Iterable[str] = ()) -> Listing:
    """The site's asmadb.js; `before`: the paths the catalog had, to tell what is new."""
    entries, seen = [], set()
    for info in _array(js.decode("utf-8", "replace"), "fileInfos"):
        e = entry(info) if isinstance(info, dict) else None
        if e is None or e.path in seen:
            continue
        seen.add(e.path)
        entries.append(e)
    before = set(before)
    today = time.strftime("%Y-%m-%d")
    changes = [(today, "added", e.path) for e in entries if before and e.path not in before]
    return Listing(entries, (), changes, dated)


def _dated(response) -> int:
    stamp = response.headers.get("Last-Modified", "")
    try:
        return int(email.utils.parsedate_to_datetime(stamp).timestamp()) if stamp else 0
    except (TypeError, ValueError):
        return 0


class Asma(Collection):
    id = "asma"
    name = "ASMA"
    home = SITE
    about = ("the Atari 8-bit computers' music (asma.atari.org): about 6,400 SAP tunes "
             "of the POKEY chip, twice Modland's. Its catalog is a 2.2 MB list")
    servers = (SITE + MUSIC,)

    def latest(self) -> int:
        with _open(DB, 20, head=True) as r:
            return _dated(r)

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        progress("Downloading ASMA's list of songs...")
        with _open(DB, 120) as r:
            dated, js = _dated(r), capped.read(r)
        return read_list(js, dated, _paths_before(self.id, self.catalog_dir))

    def parse(self, path: str) -> Entry:
        found = entry({"filePath": path})
        return found or Entry(path, "Slight Atari Player", "- unknown", "", path)

    def transport(self) -> Transport:
        return mirrored(self.id, MUSIC, [HttpTransport(SITE + MUSIC.rstrip("/"))])

    def url(self, path: str) -> str:
        return SITE + urllib.parse.quote(MUSIC + path)

    def path_in(self, url: str) -> str | None:
        if "asma.atari.org" not in url:
            return None
        at = url.find(MUSIC)
        return urllib.parse.unquote(url[at + len(MUSIC):]) if at >= 0 else None
