# SPDX-License-Identifier: GPL-3.0-or-later
"""The collections the player knows, and how a saved song names its collection.

A song is saved (in favorites, playlists, the play history, the queue) as its key:
"modland:AHX/Pink/inside.ahx". A file on this computer is saved as its path,
"/home/me/Music/song.mod". A key without a collection in front is from Modland,
which is how songs were saved before there were collections.

HVSC's songs are Modland's, since Modland hosts HVSC: "hvsc:MUSICIANS/H/Hubbard_Rob/
Commando.sid" names the song that is saved as "modland:HVSC/MUSICIANS/H/Hubbard_Rob/
Commando.sid".
"""

from __future__ import annotations

import re

from .base import Collection, Entry, Listing, Progress, Transport
from .hvsc import Hvsc
from .asma import Asma
from .keygenmusic import KeygenMusic
from .sndh import Sndh
from .zxart import ZxArt
from .modland import Modland

LOCAL = "local"         # not a collection: where files on this computer say they are from
DEFAULT = "modland"

_ID = re.compile(r"[a-z][a-z0-9]*")
_KEY = re.compile(r"([a-z][a-z0-9]*):(.*)", re.S)
_registry: dict[str, Collection] = {}


def register(collection: Collection) -> Collection:
    if not _ID.fullmatch(collection.id) or collection.id == LOCAL:
        raise ValueError(f"{collection.id!r} can't be a collection's id")
    _registry[collection.id] = collection
    return collection


def unregister(id: str) -> None:
    _registry.pop(id, None)


def get(id: str) -> Collection | None:
    return _registry.get(id)


def known() -> list[Collection]:
    return list(_registry.values())


def name(id: str) -> str:
    """A collection's name as shown; its id if the player doesn't know it (any more)."""
    found = _registry.get(id)
    return found.name if found else id


def folder(id: str) -> str:
    """The folder a collection's music is in when it is kept on this computer."""
    found = _registry.get(id)
    return (found.folder or found.name).replace("/", "-") if found else id


def by_folder(name: str) -> str | None:
    """The collection whose kept music is in a folder of this name."""
    return next((c for c in _registry if folder(c) == name), None)


def key(collection: str, path: str) -> str:
    """What a song is saved as. A song of a collection that another one hosts is the
    host's song, and is saved as that."""
    if collection == LOCAL:
        return path
    found = _registry.get(collection)
    if found is not None and found.hosted_by:
        return key(found.hosted_by, found.host_path(path))
    return f"{collection}:{path}"


def hosted() -> list[str]:
    """The collections that another one hosts."""
    return [c.id for c in _registry.values() if c.hosted_by]


def split(key: str) -> tuple[str, str]:
    """(collection, path) of a saved song."""
    if key.startswith("/"):
        return LOCAL, key
    found = _KEY.fullmatch(key)
    if found:
        return found.group(1), found.group(2)
    return DEFAULT, key


register(Modland())
register(Hvsc())
register(KeygenMusic())
register(Asma())
register(ZxArt())
register(Sndh())

__all__ = ["Collection", "Entry", "Listing", "Progress", "Transport", "LOCAL", "DEFAULT",
           "register", "unregister", "get", "known", "name", "key", "split", "hosted", "folder",
           "by_folder"]
