# SPDX-License-Identifier: GPL-3.0-or-later
"""HVSC, the High Voltage SID Collection: the Commodore 64's music, about 60,000 tunes.

With Modland chosen too, its catalog is made from Modland's file list, which the player
has anyway. Alone, it comes from HVSC's own list of tunes, Songlengths.md5 (5 MB, where
Modland's is 21): every tune with its checksum, the same as Modland's, and how long
each of its subsongs plays. That list has no sizes: they are taken from the catalog
there was, or the one that comes with the player, and a tune newer than those has its
size once it is fetched.

HVSC is a collection of its own, and Modland has all of it in its HVSC folder. So it
is a collection that Modland hosts: it is browsed in HVSC's own layout (MUSICIANS by
musician, GAMES and DEMOS by title), and its tunes are Modland's: a tune is the same
favorite, the same line in the history and the same file in the cache, whichever way
it was come by.
"""

from __future__ import annotations

import time
import urllib.request
from dataclasses import replace
from pathlib import Path
from typing import Iterable

from . import modland
from .. import capped
from .base import Collection, Entry, Listing, Progress, safe_path

FOLDER = "HVSC"                     # where Modland has it
SECTIONS = {"MUSICIANS": "Musicians", "GAMES": "Games", "DEMOS": "Demos"}
DOWNLOAD = "/C64Music/"             # what HVSC's own links have before the path


def parse(path: str, md5: str = "", size: int = 0) -> Entry:
    """MUSICIANS/H/Hubbard_Rob/[folders/]Commando.sid: by musician.
    GAMES/A-F/[folders/]Bionic_Commando.sid and DEMOS/...: by the letters they are under."""
    parts = path.split("/")
    section = SECTIONS.get(parts[0], parts[0].title())
    if parts[0] == "MUSICIANS" and len(parts) >= 4:
        artist, album = modland.hvsc_artist(parts[2]), "/".join(parts[3:-1])
    elif len(parts) >= 3:
        artist, album = parts[1], "/".join(parts[2:-1])
    else:
        artist, album = "- unknown", ""
    hidden = not parts[-1].lower().endswith((".sid", ".mus", ".str"))
    # "HVSC - Musicians": the part before the dash is the format the replayer goes by
    return Entry(path, f"{FOLDER} - {section}", artist, album.replace("_", " "),
                 parts[-1].replace("_", " "), hidden, md5, size)


class Hvsc(Collection):
    id = "hvsc"
    name = "HVSC"
    about = ("the Commodore 64's music: about 61,000 tunes, 353 MB. Comes with Modland's "
             "list; alone, from HVSC's own (5 MB)")
    home = "https://hvsc.c64.org"
    hosted_by = modland.Modland.id

    def host_path(self, path: str) -> str:
        return f"{FOLDER}/{path}"

    def guest_path(self, host_path: str) -> str | None:
        start = FOLDER + "/"
        return host_path[len(start):] if host_path.startswith(start) else None

    since = ""

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        if not self.via_host:
            listing = own_listing(progress, folder=self.catalog_dir)
        else:
            more = modland.sizes_and_changes(progress, self.since)
            listing = read_list(modland.file_list(progress), more["sizes"], more["changes"],
                                more["dated"])
        return replace(listing, notes=stil(progress))

    def done(self) -> None:
        modland.forget()

    def latest(self) -> int:
        if not self.via_host:
            with _ask_own(head=True) as r:
                return modland._dated(r)
        return modland.Modland().latest()

    def parse(self, path: str) -> Entry:
        return parse(path)

    def url(self, path: str) -> str:
        return modland.Modland().url(self.host_path(path))

    def path_in(self, url: str) -> str | None:
        where = url.find(DOWNLOAD)
        if "hvsc" not in url.lower() or where < 0:
            return None
        return url[where + len(DOWNLOAD):]


def read_list(raw: bytes, sizes: bytes | None = None,
              changes: Iterable[tuple[str, str, str]] = (), dated: int = 0) -> Listing:
    """HVSC's part of Modland's file list, and of its sizes and changes."""
    start = FOLDER + "/"
    size_of = {p[len(start):]: n for p, n in modland.read_sizes(sizes).items()
               if p.startswith(start)}

    def entries() -> Iterable[Entry]:
        for line in raw.decode("utf-8", "replace").splitlines():
            md5, _, path = line.partition(" ")
            if path.startswith(start) and safe_path(path):
                yield parse(path[len(start):], md5, size_of.get(path[len(start):], 0))
    ours = [(day, kind, p[len(start):]) for day, kind, p in changes if p.startswith(start)]
    return Listing(entries(), (), ours, dated)


SONGLENGTHS = "/DOCUMENTS/Songlengths.md5"
STIL = "/DOCUMENTS/STIL.txt"


def _ask_own(head: bool = False, timeout: int = 60, what: str = SONGLENGTHS):
    """HVSC's list of tunes (or another of its documents) from the first of its servers
    that answers."""
    why = None
    for server in modland.HVSC_SERVERS:
        try:
            request = urllib.request.Request(server + what, method="HEAD" if head else "GET",
                                             headers={"User-Agent": "bbmp"})
            return urllib.request.urlopen(request, timeout=timeout)
        except (OSError, EOFError) as e:
            why = why or e
    raise OSError(f"no server of HVSC's answers ({why})")


def read_songlengths(text: str) -> list[tuple[str, str, str]]:
    """(path, checksum, lengths) of every tune in Songlengths.md5:
    "; /MUSICIANS/H/Hubbard_Rob/Commando.sid" then "<md5>=3:55.594 1:01.288"."""
    out, path = [], None
    for line in text.splitlines():
        if line.startswith("; /"):
            path = line[3:].strip()
            path = path if safe_path(path) else None
        elif path and "=" in line and not line.startswith(("[", ";")):
            md5, _, lengths = line.partition("=")
            out.append((path, md5.strip().lower(), lengths.strip()))
            path = None
    return out


def _known(folder=None) -> tuple[dict[str, tuple[str, int]], bool]:
    """path -> (checksum, size) of HVSC's tunes as the player knew them: from the
    catalog there is in the folder, else from the one that came with the player; and
    whether it was the catalog."""
    from .. import paths, snapshot
    from ..index import read_only
    folder = folder if folder is not None else paths.CATALOG_DIR
    db = folder / "hvsc.db"
    if db.is_file():
        import sqlite3
        con = sqlite3.connect(read_only(db), uri=True)
        try:
            return {p: (m, n) for p, m, n in con.execute("SELECT path, md5, size FROM files")}, True
        except sqlite3.Error:
            pass
        finally:
            con.close()
    try:
        listings, _ = snapshot.read(snapshot.where())
    except (OSError, ValueError, EOFError):
        return {}, False
    if "hvsc" in listings:
        return {e.path: (e.md5, e.size) for e in listings["hvsc"].entries}, False
    host = listings.get(modland.Modland.id)
    start = FOLDER + "/"
    return {e.path[len(start):]: (e.md5, e.size) for e in (host.entries if host else ())
            if e.path.startswith(start)}, False


def own_listing(progress: Progress = lambda s: None, folder=None) -> Listing:
    """HVSC from its own list. What is new or changed since the catalog there was is
    what the collection added and changed today. Without a catalog before it (the
    first time, or HVSC was made from Modland's list until now) nothing is: every tune
    would be new."""
    progress("Downloading HVSC's list of tunes...")
    with _ask_own(timeout=120) as r:
        dated = modland._dated(r)
        text = capped.read(r).decode("latin-1")
    tunes = read_songlengths(text)
    if not tunes:
        raise ValueError("HVSC's list of tunes is empty")
    known, had_catalog = _known(folder)
    today = time.strftime("%Y-%m-%d")
    changes = []
    if known and had_catalog:
        for path, md5, _ in tunes:
            before = known.get(path)
            if before is None:
                changes.append((today, "added", path))
            elif before[0] and before[0] != md5:
                changes.append((today, "updated", path))
    entries = [parse(path, md5, (known.get(path) or ("", 0))[1]) for path, md5, _ in tunes]
    return Listing(entries, [(path, lengths) for path, _, lengths in tunes], changes, dated)


def read_stil(text: str) -> list[tuple[str, str]]:
    """(path, note) of every tune in HVSC's STIL (SID Tune Information List): a line
    "/MUSICIANS/H/Hubbard_Rob/Commando.sid", then its note (TITLE:, ARTIST:, COMMENT:,
    "(#2)" before a subsong's) up to an empty line. Notes on whole folders are left out."""
    out: list[tuple[str, str]] = []
    path, lines = None, []

    def done() -> None:
        if path and lines:
            out.append((path, "\n".join(lines).rstrip()))
    for line in text.replace("\r", "").splitlines():
        if line.startswith("/"):
            done()
            path = line[1:].strip()
            path = path if path.lower().endswith(".sid") and safe_path(path) else None
            lines = []
        elif not line.strip():
            done()
            path, lines = None, []
        elif path and not line.startswith("#"):
            lines.append(line.rstrip())
    done()
    return out


def stil(progress: Progress = lambda s: None) -> list[tuple[str, str]] | None:
    """HVSC's notes on its tunes (3.7 MB); None when no server gives them: the catalog
    keeps the ones it had."""
    progress("Downloading HVSC's notes on its tunes (STIL)...")
    try:
        with _ask_own(timeout=120, what=STIL) as r:
            raw = capped.read(r)
    except (OSError, EOFError):
        return None
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError:
        text = raw.decode("latin-1")
    return read_stil(text) or None


MODELS = {1: ("sid6581",), 2: ("sid8580",), 3: ("sid6581", "sid8580")}


def sid_chips(folder: Path) -> list[tuple[str, str]]:
    """(path below C64Music, chip) of the tunes in an unpacked HVSC that name the SID they
    were written for: bits 4-5 of the flags at 0x76 of a PSID or RSID header of version
    2 and up say 6581, 8580 or both (0: not said). 50,841 of HVSC 85's 61,157 do."""
    out = []
    for file in sorted(folder.rglob("*.sid")):
        try:
            with open(file, "rb") as f:
                head = f.read(0x78)
        except OSError:
            continue
        if len(head) < 0x78 or head[:4] not in (b"PSID", b"RSID"):
            continue
        if int.from_bytes(head[4:6], "big") < 2:
            continue
        flags = int.from_bytes(head[0x76:0x78], "big")
        path = file.relative_to(folder).as_posix()
        out += [(path, chip) for chip in MODELS.get((flags >> 4) & 3, ())]
    return out
