# SPDX-License-Identifier: GPL-3.0-or-later
"""What a collection is: an archive of music with a file list and a way to download.

Modland is one. Another one is a module with a Collection subclass in it, registered
in collection/__init__.py; docs/collections.md walks through writing one.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable

Progress = Callable[[str], None]


def safe_path(path: str) -> bool:
    """Is it a path inside a collection? A song's path becomes a file's place on this
    computer, so a line of a server's list that names "../x", "/x" or "a//b" is not
    taken."""
    parts = path.split("/")
    return bool(path) and not ({"", ".", ".."} & set(parts)) and "\0" not in path


@dataclass(frozen=True)
class Entry:
    """One file in a collection."""
    path: str            # where it is in the collection; with the collection's id, its identity
    format: str          # the category it is browsed under, which also picks the replayer
    artist: str
    album: str
    name: str            # the file name as shown; the title is worked out from it
    hidden: bool = False  # belongs to a song (samples, instruments) and isn't one itself
    md5: str = ""
    size: int = 0        # in bytes; 0 when the collection doesn't say


@dataclass
class Listing:
    """Everything a collection has, as downloaded by Collection.listing()."""
    entries: Iterable[Entry]
    # (path, "3:55.594 1:01.288"): how long each subsong plays, for songs that loop forever
    lengths: Iterable[tuple[str, str]] = field(default_factory=tuple)
    # (day as "2026-09-28", kind, path): what the collection says has changed of late.
    # The kind is "added", "updated", "deleted", or "moved" (moved or renamed; the path
    # is the one it has now), or what else the collection calls it
    changes: Iterable[tuple[str, str, str]] = field(default_factory=tuple)
    dated: int = 0       # when the collection made the list (seconds since 1970); 0: unknown
    # (path, chip): what a song says it plays on, where the files were read (platforms.py's
    # SONG_CHIPS: a SID tune names its 6581 or 8580)
    chips: Iterable[tuple[str, str]] = field(default_factory=tuple)
    # (path, text): what the collection's keepers wrote about a song (HVSC's STIL:
    # its title, the tune it covers, the composer's own words); None: not had this time
    notes: Iterable[tuple[str, str]] | None = None


class Transport:
    """An open connection to a collection's server. Used by one thread at a time."""

    def download(self, path: str, dest: Path) -> None:
        """Write the file at `path` in the collection to `dest`.

        Raises FileNotFoundError when the server has no such file, OSError when it
        can't be reached.
        """
        raise NotImplementedError

    def close(self) -> None:
        pass


class Collection:
    id = ""      # lower-case letters and digits; part of every saved song and the cache folder
    name = ""    # as shown to the user
    home = ""    # its web page
    servers: tuple[str, ...] = ()   # where its files are, for showing: the first is asked first
    about = ""   # a line for choosing it: what it has, how big it is
    # The folder its music is in when the user downloads it to keep, in the music
    # folder where a file manager shows it. Its name if nothing is said. It can't
    # change later.
    folder = ""

    # A collection can be hosted by another: HVSC is a collection of its own, and
    # Modland has all of it in its HVSC folder. Such a collection is browsed and
    # searched in its own layout, while each of its songs is the host's song: saved
    # under the host's key, downloaded from the host, cached once. It needs no
    # transport. In lists of every collection its songs are left out, since the host
    # has them all.
    hosted_by = ""

    def host_path(self, path: str) -> str:
        """Where the host has the file that is at `path` here."""
        raise NotImplementedError

    def guest_path(self, host_path: str) -> str | None:
        """The other way: where a file of the host's is here, if it is."""
        return None

    # Set before listing(): whether the host is chosen too. Then the collection is
    # made from the host's list, which is had anyway; alone, from a list of its own.
    via_host = True

    # Set before listing(): the folder the collection's catalog is in, where the
    # collection can see what it had before. None: the player's own.
    catalog_dir: Path | None = None

    def listing(self, progress: Progress = lambda s: None) -> Listing:
        """Download the collection's file list."""
        raise NotImplementedError

    def done(self) -> None:
        """An update is over: let go of what was kept for it (lists that the collections
        of one update share)."""

    def latest(self) -> int:
        """When the collection's newest file list was made, asked of its server now
        (seconds since 1970). 0 when the collection can't say. Raises OSError when it
        can't be reached."""
        return 0

    def parse(self, path: str) -> Entry:
        """What a path says on its own: format, artist, album and name.

        Old playlists name songs that have moved since, and those paths aren't in the
        file list any more.
        """
        raise NotImplementedError

    def transport(self) -> Transport:
        raise NotImplementedError

    def url(self, path: str) -> str:
        """The link to a file, for playlists and the desktop's "now playing"."""
        raise NotImplementedError

    def path_in(self, url: str) -> str | None:
        """The path a link points at, if it is a link into this collection."""
        return None

    # ---- playlists the collection publishes itself ----

    has_lists = False

    def published_lists(self, transport: Transport) -> list[tuple[str, str]]:
        """(what read_list() needs, the name to show) for every list."""
        return []

    def read_list(self, transport: Transport, ref: str) -> str:
        """The text of one list, in .m3u form."""
        raise NotImplementedError
