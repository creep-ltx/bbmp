# SPDX-License-Identifier: GPL-3.0-or-later
"""ZX-Art (zxart.ee): the ZX Spectrum's music, about 29,700 tunes (AY trackers, beeper,
digital), each with its composer, year, the party it took part in, its rating and its
length.

ZX-Art has a JSON API for programs, which music players use (ZXTune for Android):
/api/language:eng/export:zxMusic/start:N/limit:1000 gives a page of tunes (id, title,
authorIds, type, year, time, originalUrl, ...) and export:author the people (id,
title, realName). Its robots.txt keeps crawlers from /api and /file; a player that
fetches the tune a user picks is no crawler, and the catalog is read once a
week at the most, a request a second, as "bbmp". A tune's file is at
/file/id:<id>/filename:<name>; in the player its path is "<id>/<name>".

A tune is filed under the format Modland gives its kind ("PT3" is "Spectrum - Pro
Tracker 3"), so the same replayer plays it, then under its composer.
"""

from __future__ import annotations

import json
import time
import urllib.parse
import urllib.request
from typing import Iterable

from .. import capped
from .base import Collection, Entry, Listing, Progress, Transport, safe_path
from .keygenmusic import _paths_before
from .transports import HttpTransport, Mirrors

SITE = "https://zxart.ee"
API = "/api/language:eng/export:{what}/start:{start}/limit:{limit}"
PAGE = 1000          # the most the API gives in one answer
PAUSE = 1.0          # seconds between requests: the catalog is read gently

# ZX-Art's kind of tune -> the format Modland files it under (its replayer, its
# platform); kinds Modland has no folder for get a "Spectrum - " name of their own
TYPES = {
    "PT3": "Spectrum - Pro Tracker 3", "TS": "Spectrum - Pro Tracker 3",
    "PT2": "Spectrum - Pro Tracker 2", "PT1": "Spectrum - Pro Tracker 1",
    "STC": "Spectrum - ST Song Compiler", "ST1": "Spectrum - Sound Tracker 1.1",
    "ST3": "Spectrum - Sound Tracker 1.3", "STP": "Spectrum - Sound Tracker Pro",
    "SQT": "Spectrum - SQ Tracker", "ASC": "Spectrum - ASC Sound Master",
    "PSC": "Spectrum - Pro Sound Creator", "PSM": "Spectrum - Pro Sound Maker",
    "GTR": "Spectrum - Global Tracker", "FTC": "Spectrum - Fast Tracker",
    "FLS": "Spectrum - Flash Tracker", "CHI": "Spectrum - Chip Tracker",
    "VTX": "Spectrum - Vortex", "ZXS": "Spectrum - ZXS",
    "FXM": "Spectrum - Fuxoft AY Language", "AY": "AY Emul", "YM": "YM",
    "TFD": "TFM Music Maker", "TFC": "TFM Music Maker", "TFE": "TFM Music Maker",
    "TF0": "TFM Music Maker", "AKS": "ArkosTracker", "COP": "Sam Coupe COP",
    "DST": "Spectrum - Digital Studio", "DMM": "Spectrum - Digital Music Maker",
    "ET1": "Spectrum - Extreme Tracker", "SQD": "Spectrum - SQ Digital Tracker",
    "STR": "Spectrum - Sample Tracker", "MTC": "Spectrum - Multitrack",
    "PDT": "Spectrum - ProDigiTracker", "PSG": "Spectrum - PSG",
    "AS0": "Spectrum - ASC Sound Master 0", "FUR": "Spectrum - Furnace",
    "VGM": "Spectrum - VGM", "MOD": "Spectrum - General Sound", "CHP": "Spectrum - CHIPNSFX",
}
SNAPSHOTS = {"sna", "z80", "tap", "tzx", "scl", "trd"}  # the whole computer's memory, or its tape or disk
SOUND = {"MP3", "OGG", "WAV", "FLAC"}       # recordings, not chip music: left out


def _get(path: str, timeout: int = 120) -> bytes:
    request = urllib.request.Request(SITE + path, headers={"User-Agent": "bbmp"})
    with urllib.request.urlopen(request, timeout=timeout) as r:
        return capped.read(r)


def _page(what: str, start: int, limit: int = PAGE) -> tuple[list[dict], int]:
    """A page of the API's list of tunes or authors, and how many there are in all."""
    answer = json.loads(_get(API.format(what=what, start=start, limit=limit)))
    if answer.get("responseStatus") != "success":
        raise OSError(f"ZX-Art's API said {answer.get('responseStatus')!r}")
    found = (answer.get("responseData") or {}).get(what) or []
    return [x for x in found if isinstance(x, dict)], int(answer.get("totalAmount") or 0)


def _all(what: str, progress: Progress) -> list[dict]:
    out, start, total = [], 0, None
    while total is None or start < total:
        found, total = _page(what, start)
        out += found
        start += PAGE
        progress(f"ZX-Art: {min(start, total):,} of {total:,} {'tunes' if what == 'zxMusic' else 'people'}...")
        if start < total:
            time.sleep(PAUSE)
    return out


def _seconds(text: str) -> str:
    """"3:14.88" as the catalog keeps lengths ("3:14.880"); "" when not said."""
    try:
        minutes, _, seconds = text.strip().rpartition(":")
        total = int(minutes or 0) * 60 + float(seconds)
    except ValueError:
        return ""
    return f"{int(total // 60)}:{total % 60:06.3f}" if total > 0 else ""


def entry(tune: dict, people: dict[int, str]) -> Entry | None:
    kind = str(tune.get("type") or "").upper()
    name = urllib.parse.unquote(str(tune.get("originalFileName") or ""))
    if kind in SOUND or not tune.get("originalUrl") or not name:
        return None
    path = f"{int(tune['id'])}/{name}"
    if not safe_path(path):
        return None
    artists = [people.get(a) or "" for a in tune.get("authorIds") or [] if isinstance(a, int)]
    artist = " & ".join(a for a in artists if a) or "- unknown"
    title = str(tune.get("title") or "").strip() or name.rsplit(".", 1)[0]
    ext = name.rsplit(".", 1)[-1].lower() if "." in name else kind.lower()
    if not kind and ext.upper() in TYPES:
        kind = ext.upper()      # ZX-Art names no kind for some (113 in 2026): .fxm, .fls, .vtx
    fmt = ("Spectrum - Snapshot" if ext in SNAPSHOTS
           else TYPES.get(kind, f"Spectrum - {kind}" if kind else "Spectrum"))
    return Entry(path, fmt, artist, "", f"{title}.{ext}")


def read_list(tunes: Iterable[dict], people: Iterable[dict], dated: int = 0,
              before: Iterable[str] = ()) -> Listing:
    names = {int(p["id"]): str(p.get("title") or "").strip()
             for p in people if isinstance(p.get("id"), int)}
    entries, lengths, seen = [], [], set()
    for tune in tunes:
        if not isinstance(tune.get("id"), int):
            continue
        e = entry(tune, names)
        if e is None or e.path in seen:
            continue
        seen.add(e.path)
        entries.append(e)
        if length := _seconds(str(tune.get("time") or "")):
            lengths.append((e.path, length))
    before = set(before)
    today = time.strftime("%Y-%m-%d")
    changes = [(today, "added", e.path) for e in entries if before and e.path not in before]
    return Listing(entries, lengths, changes, dated)


class ZxArt(Collection):
    id = "zxart"
    name = "ZX-Art"
    home = SITE
    about = ("the ZX Spectrum's music (zxart.ee): about 29,700 tunes, AY trackers, beeper "
             "and digital, with composers, parties and ratings. Its catalog is read from "
             "its API, about 50 requests")
    servers = (SITE + "/file/",)

    def latest(self) -> int:
        found, _ = _page("zxMusic", 0, 1)
        return max((int(t.get("dateModified") or 0) for t in found), default=0)

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        progress("Downloading ZX-Art's list of tunes...")
        tunes = _all("zxMusic", progress)
        time.sleep(PAUSE)
        people = _all("author", progress)
        dated = max((int(t.get("dateModified") or 0) for t in tunes), default=0)
        return read_list(tunes, people, dated, _paths_before(self.id, self.catalog_dir))

    def parse(self, path: str) -> Entry:
        return Entry(path, "Spectrum", "- unknown", "", path.rsplit("/", 1)[-1])

    def transport(self) -> Transport:
        return Mirrors([_Files()])

    def url(self, path: str) -> str:
        tune, _, name = path.partition("/")
        return f"{SITE}/file/id:{tune}/filename:{urllib.parse.quote(name)}"

    def path_in(self, url: str) -> str | None:
        if "zxart.ee/file/" not in url:
            return None
        parts = dict(p.split(":", 1) for p in url.split("/file/", 1)[1].split("/") if ":" in p)
        if "id" not in parts or "filename" not in parts:
            return None
        return f"{parts['id']}/{urllib.parse.unquote(parts['filename'])}"


class _Files(HttpTransport):
    """ZX-Art's files: "<id>/<name>" is at /file/id:<id>/filename:<name>."""

    def __init__(self):
        super().__init__(SITE + "/file")

    def url(self, path: str) -> str:
        tune, _, name = path.partition("/")
        return f"{self.base}/id:{tune}/filename:{urllib.parse.quote(name)}"
