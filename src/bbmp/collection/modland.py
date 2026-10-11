# SPDX-License-Identifier: GPL-3.0-or-later
"""Modland, ftp.modland.com: about half a million songs in over 400 formats.

Modland publishes every file it has in /pub/documents/allmods_md5.zip as lines of
"<md5> <Format>/<Artist>/[<Album>/]<file>". That is all that browsing and search
need, so nothing is crawled on the server.
"""

from __future__ import annotations

import email.utils
import re
import tempfile
import time
from dataclasses import replace
import urllib.parse
import urllib.request
import zipfile
from typing import Iterable

from .. import capped
from ..companions import BANKS, SHARED_FOLDERS, SUBFORMAT_DIRS, bank_of, is_companion
from .base import Collection, Entry, Listing, Progress, Transport, safe_path
from .transports import Below, FtpTransport, HttpTransport, Mirrors, mirrored

HOST = "ftp.modland.com"
MODULES = "/pub/modules"
LISTS = "/pub/playlists"
LIST = "/pub/documents/allmods_md5.zip"     # "<md5> <path>" for every file
SIZES = "/allmods.zip"                      # "<size><tab><path>" for every file
CHANGELOGS = "/pub/changelogs"              # changelog_2026-09-28.txt, one a day
DAYS = 60                                   # of changes that are kept

# Servers that have Modland. Until the player has heard how fast each one answers
# they are asked in this order, the fastest first as measured on 2 October 2026 (time to
# the first byte of a new connection: antarctica.no 0.07-0.35 s, amigascne 0.15-0.8 s,
# Modland's own 5-8 s, ziphoid 1.4-14 s); after that the fastest lately is asked first
# (see transports.Mirrors). What a server gives is checked against the checksum
# Modland's file list has for the file, so a mirror can't give another file.
SERVERS = (
    "https://modland.antarctica.no",
    "http://ftp.amigascne.org/mirrors/ftp.modland.com",
    "https://ftp.modland.com",
    "https://modland.ziphoid.com",
)
FTP_SERVERS = (("ftp.modland.com", ""), ("ftp.amigascne.org", "/mirrors/ftp.modland.com"))
# HVSC's own servers, for what Modland has of HVSC. They have the newest HVSC, which
# Modland's copy may not be: a tune that was changed since is not taken from them.
HVSC_SERVERS = (
    "https://hvsc.c64.org/download/C64Music",
    "https://hvsc.brona.dk/HVSC/C64Music",
    "https://hvsc.etv.cx/C64Music",
    "https://hvsc.de/download/C64Music",
)
HVSC_SONGLENGTHS_URL = "https://hvsc.c64.org/download/C64Music/DOCUMENTS/Songlengths.md5"

# Files that hold instruments for the songs next to them and no song of their own, and
# games' banks of sound effects (2 October 2026: their channels play no tune, or no
# replayer takes them as a song).
NOT_SONGS = {"MaxTrax/- unknown/mm3-fx.mxtx", "MaxTrax/- unknown/mm3-jaws.mxtx",
             "MaxTrax/Frank Klepacki/kyrandia introinst.mxtx",
             *(f"David Whittaker/David Whittaker/{n}.dw" for n in (
                 "anarchy sfx", "blood money sfx", "leviathan-sfx", "spellbound sfx",
                 "thekillingcloud-sfx", "xenon-sfx")),
             "Rob Hubbard/Rob Hubbard/fxlarge.rh", "Rob Hubbard/Rob Hubbard/fxsmall.rh",
             "Activision Pro/Sean Gugler/d-generation-sfx.avp",
             "Digital Sonix And Chrome/David Hanlon/dragon'sbreath fyfx.dsc",
             "Infogrames/Lauren Elliott/shufflepuck-shuffle.dum",
             "Infogrames/Lauren Elliott/shufflepuck-ringard.dum",
             "Infogrames/Charles Callet/drakkhen (resid).dum",
             "Infogrames/Charles Callet/metal masters (bruits).dum",
             "Infogrames/- unknown/advantage tennis-bruits.dum",
             "Infogrames/Silvain Tintillier/rh-captainbloodfx.dum"}

# Formats whose second folder level is a sub-format or system rather than an artist:
# SUBFORMAT_DIRS, in companions.py.


def parse_path(path: str) -> tuple[str, str, str, str, bool]:
    """Split a Modland path into (format, artist, album, name, hidden)."""
    parts = path.split("/")
    fmt, name = parts[0], parts[-1]
    hidden = is_companion(fmt, name, parts[2:-1]) or path in NOT_SONGS
    if len(parts) >= 3 and SHARED_FOLDERS.get(fmt) == parts[1]:
        hidden = True       # the samples of all of the format's songs
    if fmt == "HVSC":
        # HVSC/MUSICIANS/H/Hubbard_Rob/[sub/]Commando.sid, HVSC/GAMES/A/[..]/x.sid
        if len(parts) >= 5 and parts[1] == "MUSICIANS":
            artist = hvsc_artist(parts[3])
            album = "/".join(parts[4:-1])
        else:
            artist = parts[1].title() if len(parts) >= 3 else "- unknown"
            album = "/".join(parts[3:-1])
        name = name.replace("_", " ")
    elif fmt in SUBFORMAT_DIRS and len(parts) >= 4:
        # Video Game Music/<System>/<Artist>/<Game>/file.vgz, Ad Lib/<Tracker>/<Artist>/file
        fmt = f"{fmt} - {parts[1]}"
        artist = parts[2]
        album = "/".join(parts[3:-1])
    else:
        artist = parts[1] if len(parts) >= 3 else "- unknown"
        album = "/".join(parts[2:-1])
    return fmt, artist, album, name, hidden


def hvsc_artist(d: str) -> str:
    # HVSC names musicians Last_First; show "First Last" when there are exactly two parts.
    bits = d.split("_")
    return f"{bits[1]} {bits[0]}" if len(bits) == 2 else " ".join(bits)


def _entry(path: str, md5: str = "", size: int = 0) -> Entry:
    fmt, artist, album, name, hidden = parse_path(path)
    return Entry(path, fmt, artist, album, name, hidden, md5, size)


def read_sizes(raw: bytes | None) -> dict[str, int]:
    """allmods.txt: "<size><tab><path>" for every file."""
    sizes = {}
    for line in (raw or b"").decode("utf-8", "replace").splitlines():
        size, tab, path = line.partition("\t")
        if tab and size.isdigit():
            sizes[path] = int(size)
    return sizes


def _entries(raw: bytes, sizes: dict[str, int] | None = None) -> Iterable[Entry]:
    sizes = sizes or {}
    lines = [line.partition(" ") for line in raw.decode("utf-8", "replace").splitlines()]
    # what is a wave bank shows only by the song next to it
    folders: dict[str, set[str]] = {}
    for _, _, path in lines:
        if path.startswith(tuple(f + "/" for f in BANKS)):
            folder, _, name = path.rpartition("/")
            folders.setdefault(folder, set()).add(name)
    for md5, _, path in lines:
        if not safe_path(path):
            continue
        entry = _entry(path, md5, sizes.get(path, 0))
        folder, _, name = path.rpartition("/")
        if folder in folders and bank_of(path.split("/", 1)[0], name, folders[folder]):
            entry = replace(entry, hidden=True)
        yield entry


def _lengths(raw: bytes) -> Iterable[tuple[str, str]]:
    """'; /MUSICIANS/H/Hubbard_Rob/Commando.sid' + 'md5=3:55.594 1:01.288' -> (path, lengths)."""
    path = None
    for line in raw.decode("latin-1").splitlines():
        if line.startswith("; /"):
            path = "HVSC" + line[2:].strip()
            path = path if safe_path(path) else None
        elif path and "=" in line:
            yield path, line.split("=", 1)[1].strip()
            path = None


def read_list(raw: bytes, songlengths: bytes | None = None, sizes: bytes | None = None,
              changes: Iterable[tuple[str, str, str]] = (), dated: int = 0) -> Listing:
    """The listing in the contents of allmods_md5.txt and, for the C64 songs Modland
    mirrors from HVSC, HVSC's song length database; allmods.txt has the files' sizes."""
    return Listing(_entries(raw, read_sizes(sizes)),
                   _lengths(songlengths) if songlengths else (), changes, dated)


def read_changelog(day: str, text: str) -> list[tuple[str, str, str]]:
    """A day's changelog: lines of "added: <path>", "updated: <path>", "deleted: <path>".
    A file that moved is "moved: <path>", then "to: <folder>", then "as: <name>" if its
    name changed on the way; one that was renamed is "renamed: <path>", then "to:
    <name>". Either is kept as "moved", with the path the file has now: that is where
    the song is."""
    out = []
    move: list[str] | None = None        # [kind, folder, name, whether "to" came]

    def moved() -> None:
        if move and move[3]:
            path = f"{move[1]}/{move[2]}" if move[1] else move[2]
            if safe_path(path):
                out.append((day, "moved", path))
    for line in text.splitlines():
        kind, colon, path = line.strip().partition(": ")
        kind, path = kind.lower(), path.strip()
        if not (colon and kind.isalpha() and path):
            continue
        if move and kind == "to":
            move[1 if move[0] == "moved" else 2], move[3] = path, "yes"
        elif move and kind == "as" and move[0] == "moved":
            move[2] = path
        else:
            moved()
            move = None
            if kind in ("moved", "renamed"):
                folder, _, name = path.rpartition("/")
                move = [kind, folder, name, ""]
            elif safe_path(path):
                out.append((day, kind, path))
    moved()
    return out


# What one update downloads, kept for the collections that Modland hosts, which are
# made from the same lists, and let go of when the update is over (Modland.done):
# name -> (when it was downloaded, what it is)
_kept: dict[str, tuple[float, object]] = {}
KEEP_SECONDS = 600          # an update takes no longer


def _had(name: str):
    found = _kept.get(name)
    return found[1] if found is not None and time.time() - found[0] < KEEP_SECONDS else None


def _keep(name: str, what):
    _kept[name] = (time.time(), what)
    return what


def forget() -> None:
    """Let go of the lists kept for an update (Modland's is 45 MB, its sizes 31)."""
    _kept.clear()


def _ask(path: str, timeout: int = 60, head: bool = False):
    """Ask Modland's servers for a file of theirs, the next one when one doesn't answer."""
    why = None
    for server in SERVERS:
        try:
            request = urllib.request.Request(server + path, method="HEAD" if head else "GET",
                                             headers={"User-Agent": "bbmp"})
            return urllib.request.urlopen(request, timeout=timeout)
        except (OSError, EOFError) as e:
            why = why or e
    raise OSError(f"no server of Modland's answers ({why})")


def _dated(response) -> int:
    stamp = response.headers.get("Last-Modified", "")
    try:
        return int(email.utils.parsedate_to_datetime(stamp).timestamp()) if stamp else 0
    except (TypeError, ValueError):
        return 0


def sizes_and_changes(progress: Progress = lambda s: None, since: str = "") -> dict:
    """The files' sizes (allmods.txt, as it is), the changes of the days after `since`
    (at most DAYS days), and when the file list was made. What can't be had is left
    out: the catalog does without."""
    sizes = _had("sizes")
    if sizes is None:
        sizes = {"sizes": None, "dated": 0}
        try:
            with _ask(LIST, head=True) as r:
                sizes["dated"] = _dated(r)
        except OSError:
            pass
        progress("Downloading the sizes of Modland's files...")
        try:
            with _ask(SIZES, timeout=120) as r, tempfile.TemporaryFile() as packed:
                capped.copy(r, packed, capped.LIST, "Modland's list of sizes")
                with zipfile.ZipFile(packed) as z:
                    sizes["sizes"] = capped.unzip_first(z, capped.LIST, "Modland's list of sizes")
            _keep("sizes", sizes)       # what failed is asked for again
        except (OSError, EOFError, zipfile.BadZipFile, IndexError):
            progress("Modland's list of sizes can't be had; sizes will not be shown.")
    return {**sizes, "changes": _changes(progress, since)}


def _changes(progress: Progress, since: str) -> list[tuple[str, str, str]]:
    """The changes of the days after `since`, at most DAYS days of them."""
    progress("Downloading what is new on Modland...")
    changes = []
    try:
        days = _had("days")
        if days is None:
            with _ask(CHANGELOGS + "/") as r:
                page = capped.read(r).decode("utf-8", "replace")
            days = _keep("days", sorted(set(re.findall(
                r"changelog_(\d{4}-\d\d-\d\d)\.txt", page)), reverse=True))
        for day in [d for d in days if d > since][:DAYS]:
            text = _had(f"changelog {day}")
            if text is None:
                with _ask(f"{CHANGELOGS}/changelog_{day}.txt") as r:
                    text = _keep(f"changelog {day}", capped.read(r).decode("utf-8", "replace"))
            changes += read_changelog(day, text)
    except (OSError, EOFError):
        progress("Modland's changelogs can't be had.")
    return changes


def file_list(progress: Progress = lambda s: None) -> bytes:
    """Modland's file list, allmods_md5.txt. Collections that Modland hosts are made
    from it too, so it is kept until the update is over."""
    raw = _had("list")
    if raw is not None:
        return raw
    progress("Downloading Modland's file list...")
    raw, why = None, None
    for server in (*SERVERS, *(f"ftp://{host}{root}" for host, root in FTP_SERVERS)):
        try:
            # streamed to a file, then unpacked, each with a ceiling: plain http and ftp
            # servers are asked too, and a zip bomb would take all the memory
            with (urllib.request.urlopen(server + LIST, timeout=120) as r,
                  tempfile.TemporaryFile() as packed):
                capped.copy(r, packed, capped.LIST, "Modland's file list")
                with zipfile.ZipFile(packed) as z:
                    raw = capped.unzip_first(z, capped.LIST, "Modland's file list")
            break
        except (OSError, EOFError, zipfile.BadZipFile, IndexError) as e:
            why = why or e
            progress(f"{server} doesn't answer; asking the next server...")
    if raw is None:
        raise OSError(f"no server of Modland's could give its file list ({why})")
    return _keep("list", raw)


class Modland(Collection):
    id = "modland"
    name = "Modland"
    about = ("tracker modules and computer and console music: about 490,000 songs, "
             "99 GB. Its catalog is made from a 21 MB list")
    home = "https://modland.com"

    def latest(self) -> int:
        with _ask(LIST, timeout=20, head=True) as r:
            return _dated(r)

    since = ""      # the last day the catalog has changes of; the catalog's builder sets it

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        raw = file_list(progress)
        more = sizes_and_changes(progress, self.since)
        progress("Downloading HVSC song lengths...")
        try:
            with urllib.request.urlopen(HVSC_SONGLENGTHS_URL, timeout=120) as r:
                lengths = capped.read(r)
        except OSError:
            lengths = None       # without them C64 tunes get a default length
        return read_list(raw, lengths, more["sizes"], more["changes"], more["dated"])

    def done(self) -> None:
        forget()

    def parse(self, path: str) -> Entry:
        return _entry(path)

    def transport(self) -> Transport:
        return mirrored(self.id, MODULES, [
            *(HttpTransport(server + MODULES) for server in SERVERS),
            *(FtpTransport(host, root + MODULES, top=root) for host, root in FTP_SERVERS),
            *(Below("HVSC", HttpTransport(server)) for server in HVSC_SERVERS)])

    servers = (*SERVERS, *(f"ftp://{host}{root}" for host, root in FTP_SERVERS),
               *HVSC_SERVERS)

    def url(self, path: str) -> str:
        return f"ftp://{HOST}{MODULES}/" + urllib.parse.quote(path)

    def path_in(self, url: str) -> str | None:
        # any mirror of Modland: what follows /pub/modules/ is the path
        where = url.find(MODULES + "/")
        if "://" not in url or where < 0:
            return None
        return urllib.parse.unquote(url[where + len(MODULES) + 1:])

    # Modland's lists are people's favourites, music disks, and the competition entries
    # of demo parties, as .m3u files of links into the archive.
    has_lists = True

    def published_lists(self, transport: Transport) -> list[tuple[str, str]]:
        found: list[tuple[str, str]] = []
        ftp = _ftp(transport)

        def walk(folder: str, label: str, depth: int) -> None:
            for name, is_dir in ftp.list_dir(folder):
                if is_dir and depth < 3:
                    walk(f"{folder}/{name}", f"{label}{list_name(name)} / ", depth + 1)
                elif name.lower().endswith(".m3u"):
                    found.append((f"{folder}/{name}", label + list_name(name)))
        walk(LISTS, "", 0)
        return found

    def read_list(self, transport: Transport, ref: str) -> str:
        return _ftp(transport).read(ref).decode("utf-8", "replace")


def _ftp(transport: Transport) -> FtpTransport:
    """Folders can be listed by FTP only."""
    found = transport.of(FtpTransport) if isinstance(transport, Mirrors) else transport
    if not isinstance(found, FtpTransport):
        raise OSError("Modland's lists are read by FTP, and there is no FTP server")
    return found


def list_name(file_name: str) -> str:
    """'assembly_92.m3u' -> 'Assembly 92'."""
    stem = file_name.rsplit(".", 1)[0] if file_name.lower().endswith((".m3u", ".m3u8")) else file_name
    stem = stem.replace("_", " ").strip()
    return " ".join(w if w.isupper() else w.capitalize() for w in stem.split())
