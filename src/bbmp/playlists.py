# SPDX-License-Identifier: GPL-3.0-or-later
"""Reading .m3u playlists.

A list names songs by links into a collection, by their keys, or as files on this
computer. Importing one means finding each of those songs in the catalog.
"""

from __future__ import annotations

import os
import urllib.parse
from pathlib import Path

from . import collection as collections_
from .collection import LOCAL
from .collection.modland import list_name
from .index import Index


def parse_m3u(text: str, base: Path | None = None) -> list[str]:
    """The keys of the songs an .m3u file names.

    `base` is the folder of a list that is a file on this computer: names in it that
    aren't whole paths are files from there. Whatever isn't a file is taken as a song
    in a collection.
    """
    out = []
    for line in text.splitlines():
        line = line.strip().lstrip("\ufeff")
        if not line or line.startswith("#"):
            continue
        if "://" in line and not line.lower().startswith("file:"):
            for source in collections_.known():
                path = source.path_in(line)
                if path is not None:
                    out.append(collections_.key(source.id, path))
                    break
            continue
        if line.lower().startswith("file:"):
            # file:///home/..., file://localhost/home/...; another computer's isn't had
            url = urllib.parse.urlsplit(line)
            if url.netloc not in ("", "localhost"):
                continue
            line = urllib.parse.unquote(url.path)
        line = line.replace("\\", "/")       # a list made on Windows
        path = Path(line).expanduser()
        if path.is_absolute() or base is None:
            out.append(os.path.normpath(path))
        elif (base / path).exists() or collections_.split(line)[1].count("/") < 2:
            out.append(os.path.normpath(base / path))
        else:
            out.append(line)
    return out


PLAYLIST_SUFFIXES = (".m3u", ".m3u8")


def is_playlist_file(path: Path) -> bool:
    return path.suffix.lower() in PLAYLIST_SUFFIXES


def read_playlist_file(path: Path) -> list[str]:
    """The songs a playlist file on this computer names. Raises OSError if it can't be read.

    Paths in the file that aren't absolute are taken from the file's own folder.
    """
    raw = Path(path).expanduser().read_bytes()
    try:
        text = raw.decode("utf-8-sig")
    except UnicodeDecodeError:      # older playlists are in the local 8-bit encoding
        text = raw.decode("cp1252", "replace")
    return parse_m3u(text, base=Path(path).expanduser().resolve().parent)


def import_file(path: Path, index: Index, library, name: str | None = None) -> tuple[int, str, int, int]:
    """Make a playlist from a file. Returns (playlist id, its name, songs found, songs not found)."""
    found, missing = resolve(read_playlist_file(path), index)
    if not found:
        raise ValueError("none of the songs in that file could be found")
    playlist = library.create_playlist(name or list_name(Path(path).name), found)
    return playlist, library.playlist_name(playlist), len(found), missing


def resolve(keys: list[str], index: Index) -> tuple[list[str], int]:
    """The songs that can be found, by the keys they have now, and how many can't.

    Old lists name songs where they were years ago; see Index.relocate().
    """
    found = []
    for k in keys:
        song = index.resolve(k)
        if song is None and collections_.split(k)[0] != LOCAL:
            song = index.relocate(k)
        if song is not None and song.key not in found:
            found.append(song.key)
    return found, len(keys) - len(found)
