# SPDX-License-Identifier: GPL-3.0-or-later
"""SNDH (sndh.atari.org): the Atari ST's music, the YM2149 chip's, about 5,900 tunes,
kept by its own keepers and newer than Modland's copy of it.

SNDH publishes all of it once a year as one zip (sndh2026_lf.zip, 100 MB, listed on
/download.php), and has every tune in its open folders too (/sndh/sndh_lf/<composer>/
...). The catalog is read from the zip's own list of files, which is at its end: two
small requests (a zip's central directory, about 0.7 MB), not the 100 MB, and not the
600 folders. The tunes are fetched one by one from the folders.

The zip gives no checksums, which the player tells a song two collections have by; a
tune that Modland has at the same path and size takes Modland's checksum, so it shows
once, as "Modland + SNDH". Few do (none in September 2026): SNDH keeps its tunes
unpacked and fixed, where Modland has older, packed copies (Wings of Death: 43,542
bytes here, 23,396 there), so both are shown, each with its collection. SNDH's layout
is the one Modland copied (SNDH/Mad_Max/Games/Wings_Of_Death.sndh), and tunes are filed
as Modland files them.
"""

from __future__ import annotations

import email.utils
import re
import sqlite3
import struct
import time
import urllib.parse
import urllib.request
from typing import Iterable

from .. import capped
from .base import Collection, Entry, Listing, Progress, Transport, safe_path
from .keygenmusic import _paths_before
from .modland import parse_path
from .transports import HttpTransport, mirrored

SITE = "https://sndh.atari.org"
MUSIC = "/sndh/"
DOWNLOADS = "/download.php"
FOLDER = "sndh_lf/"          # the zip's (and the site's) folder of long file names


def _open(url: str, rng: str = "", head: bool = False, timeout: int = 60):
    headers = {"User-Agent": "bbmp"}
    if rng:
        headers["Range"] = rng
    request = urllib.request.Request(url, headers=headers, method="HEAD" if head else "GET")
    return urllib.request.urlopen(request, timeout=timeout)


def newest_archive() -> str:
    """The address of SNDH's newest yearly archive of everything, as /download.php
    lists them ("files/sndh2026_lf.zip")."""
    with _open(SITE + DOWNLOADS) as r:
        page = capped.read(r).decode("utf-8", "replace")
    years = sorted(set(re.findall(r'files/sndh(\d{4})_lf\.zip', page)))
    if not years:
        raise OSError("SNDH's download page lists no archive")
    return f"{SITE}/files/sndh{years[-1]}_lf.zip"


def _dated(response) -> int:
    stamp = response.headers.get("Last-Modified", "")
    try:
        return int(email.utils.parsedate_to_datetime(stamp).timestamp()) if stamp else 0
    except (TypeError, ValueError):
        return 0


def zip_list(url: str) -> tuple[list[tuple[str, int]], int]:
    """(name, bytes) of the files in a zip on a server, read from its central directory
    with two Range requests; and when the zip was made."""
    with _open(url, head=True) as r:
        size, dated = int(r.headers["Content-Length"]), _dated(r)
    with _open(url, f"bytes={max(0, size - 65536)}-{size - 1}") as r:
        tail = capped.read(r, 1 << 20)
    end = tail.rfind(b"PK\x05\x06")
    if end < 0 or end + 22 > len(tail):
        raise OSError("SNDH's archive has no zip directory at its end")
    cd_size, cd_offset = struct.unpack("<II", tail[end + 12:end + 20])
    if cd_size > 64 << 20:
        raise OSError("SNDH's archive says its list is too big")
    with _open(url, f"bytes={cd_offset}-{cd_offset + cd_size - 1}") as r:
        cd = capped.read(r, 64 << 20)
    out, at = [], 0
    while at + 46 <= len(cd) and cd[at:at + 4] == b"PK\x01\x02":
        usize, nlen, elen, clen = struct.unpack("<IHHH", cd[at + 24:at + 34])
        flags = struct.unpack("<H", cd[at + 8:at + 10])[0]
        raw = cd[at + 46:at + 46 + nlen]
        name = raw.decode("utf-8" if flags & 0x800 else "cp437", "replace")
        out.append((name, usize))
        at += 46 + nlen + elen + clen
    return out, dated


def read_list(files: Iterable[tuple[str, int]], dated: int = 0, before: Iterable[str] = (),
              modland: dict[str, tuple[str, int]] | None = None) -> Listing:
    """The archive's files as the catalog's songs. `modland`: Modland's SNDH files, by
    their path below SNDH/, with their checksum and size."""
    modland = modland or {}
    entries, seen = [], set()
    for name, size in files:
        if not name.startswith(FOLDER) or name.endswith("/"):
            continue
        path = name[len(FOLDER):]
        if not path.lower().endswith(".sndh") or path in seen or not safe_path(path):
            continue
        seen.add(path)
        _, artist, album, title, _ = parse_path("SNDH/" + path)
        md5, theirs = modland.get(path, ("", -1))
        entries.append(Entry(path, "SNDH", artist, album, title, False,
                             md5 if theirs == size else "", size))
    before = set(before)
    today = time.strftime("%Y-%m-%d")
    changes = [(today, "added", e.path) for e in entries if before and e.path not in before]
    return Listing(entries, (), changes, dated)


def _modland_sndh(folder) -> dict[str, tuple[str, int]]:
    """Modland's SNDH files in its catalog, if there is one: path below SNDH/ -> (md5,
    size), to tell the tunes both have."""
    from .. import paths
    from ..index import read_only
    db = (folder if folder is not None else paths.CATALOG_DIR) / "modland.db"
    if not db.is_file():
        return {}
    try:
        con = sqlite3.connect(read_only(db), uri=True)
        try:
            return {p[5:]: (md5, size) for p, md5, size in con.execute(
                "SELECT path, md5, size FROM files WHERE path LIKE 'SNDH/%'") if len(md5) == 32}
        finally:
            con.close()
    except sqlite3.Error:
        return {}


class Sndh(Collection):
    id = "sndh"
    name = "SNDH"
    home = SITE
    about = ("the Atari ST's music (sndh.atari.org): about 5,900 tunes of the YM2149 chip, "
             "newer than Modland's copy. Its catalog is read from its archive's list of "
             "files, 0.7 MB")
    servers = (SITE + MUSIC,)

    def latest(self) -> int:
        with _open(newest_archive(), head=True, timeout=20) as r:
            return _dated(r)

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        progress("Reading the list of SNDH's archive...")
        files, dated = zip_list(newest_archive())
        return read_list(files, dated, _paths_before(self.id, self.catalog_dir),
                         _modland_sndh(self.catalog_dir))

    def parse(self, path: str) -> Entry:
        _, artist, album, title, _ = parse_path("SNDH/" + path)
        return Entry(path, "SNDH", artist, album, title)

    def transport(self) -> Transport:
        return mirrored(self.id, MUSIC + FOLDER, [HttpTransport(SITE + MUSIC + FOLDER.rstrip("/"))])

    def url(self, path: str) -> str:
        return SITE + urllib.parse.quote(MUSIC + FOLDER + path)

    def path_in(self, url: str) -> str | None:
        if "sndh.atari.org" not in url:
            return None
        at = url.find(MUSIC + FOLDER)
        return urllib.parse.unquote(url[at + len(MUSIC + FOLDER):]) if at >= 0 else None
