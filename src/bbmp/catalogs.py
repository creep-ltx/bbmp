# SPDX-License-Identifier: GPL-3.0-or-later
"""Which collections the user has, and keeping their catalogs.

Each chosen collection has a catalog of its own (index.py). At the first start the
user chooses; later, Settings. A chosen collection without a catalog gets one: from
the catalogs that come with the player where they have it, else from the collection.
A collection that is turned off has its catalog removed; saved music, favorites and
playlists stay.

Catalogs are updated when the user asks (u, `bbmp update`), and by themselves when
the oldest is older than the setting says: every day, week, two weeks or month, or
never. One update at a time: while the catalogs are being made or updated, by this
player or by another one (`bbmp update` while the player runs), another update is
not begun but raises Busy.
"""

from __future__ import annotations

import contextlib
import fcntl
import os
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterable, Iterator

from . import apart, collection as collections_, index as index_mod, paths
from .settings import Settings

# The catalogs are built in a process of their own (apart.py), which gives all its
# memory back when it ends: built in the player's, it peaked at 678 MB and kept 282 MB
# (7 and 11 October 2026). The tests build them here, where they can stand in for the
# collections' servers.
APART = True

DEFAULT = ["modland", "hvsc"]       # what is ticked at the first start

# how often the catalogs update by themselves: setting -> (days, as shown)
EVERY = {"day": (1, "every day"), "week": (7, "every week"), "2weeks": (14, "every two weeks"),
         "month": (30, "every month"), "never": (None, "never: only when asked")}

Progress = Callable[[str], None]


class Busy(Exception):
    """The catalogs are being updated already, by this player or another."""


@dataclass
class Updated:
    """What an update did."""
    songs: int = 0          # in the catalogs now
    # the collections that could not be updated, and why; they keep the catalog they had
    failed: dict[str, str] = field(default_factory=dict)

    def why(self) -> str:
        """The collections that failed, and why, as a line to show."""
        return "; ".join(f"{collections_.name(c)}: {why}" for c, why in self.failed.items())


_lock = threading.RLock()       # of this process; the thread that has it may go on
_held = threading.local()       # how deep this thread is in its update, and the file


@contextlib.contextmanager
def updating(folder: Path) -> Iterator[None]:
    """Hold the catalogs while they are made: against the other threads of this
    process, and against other processes by a lock on a file in the catalog folder.
    Raises Busy when another has them."""
    if not _lock.acquire(blocking=False):
        raise Busy("the catalogs are being updated already")
    try:
        depth = getattr(_held, "depth", 0)
        if depth == 0:
            folder.mkdir(parents=True, exist_ok=True)
            fd = os.open(folder / ".updating", os.O_RDWR | os.O_CREAT, 0o644)
            try:
                fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError:
                os.close(fd)
                raise Busy("the catalogs are being updated already, by another bbmp") from None
            _held.fd = fd
            # what a build left that was stopped short (the player was killed)
            for tmp in folder.glob(".*.tmp*"):
                tmp.unlink(missing_ok=True)
        _held.depth = depth + 1
        try:
            yield
        finally:
            _held.depth = depth
            if depth == 0:
                os.close(_held.fd)      # which lets go of the lock
    finally:
        _lock.release()


def chosen(settings: Settings, folder: Path | None = None) -> list[str]:
    """The collections the user has: as chosen, or, from before there was a choice, the
    ones that have a catalog, or else the default."""
    folder = folder if folder is not None else paths.CATALOG_DIR
    if settings.collections is not None:
        return [c for c in settings.collections if collections_.get(c) is not None]
    found = index_mod._catalogs(folder)
    have = [c.id for c in collections_.known() if c.id in found]     # in the player's order
    return have or list(DEFAULT)


def first_start(settings: Settings, folder: Path | None = None) -> bool:
    """Is there nothing chosen and no catalog, so that the user is to be asked?"""
    folder = folder if folder is not None else paths.CATALOG_DIR
    return (settings.collections is None and not index_mod._catalogs(folder)
            and not paths.INDEX_DB.is_file())


def needs_work(settings: Settings, folder: Path | None = None) -> bool:
    """Is there an old catalog to split, or a chosen collection without a catalog?"""
    folder = folder if folder is not None else paths.CATALOG_DIR
    have = set(index_mod._catalogs(folder))
    return paths.INDEX_DB.is_file() or not set(chosen(settings, folder)) <= have


def prepare(settings: Settings, progress: Progress = lambda s: None,
            folder: Path | None = None) -> None:
    """Get the chosen collections' catalogs ready: the old single catalog split, and
    the missing ones made from the collections themselves (their lists, fetched now).
    Raises Busy when an update is at work."""
    folder = folder if folder is not None else paths.CATALOG_DIR
    with updating(folder):
        _prepare(settings, progress, folder)


def _prepare(settings: Settings, progress: Progress, folder: Path) -> None:
    index_mod.move_old_catalog(folder, paths.INDEX_DB, progress)
    ids = chosen(settings, folder)
    if settings.collections is None:
        settings.update(collections=ids)
    index_mod.set_hosts(ids)
    missing = [c for c in ids if c not in index_mod._catalogs(folder)]
    if not missing:
        return
    _build(progress, missing, folder, ids, {})


def _build(progress: Progress, only: list[str], folder: Path, chosen: list[str],
           failed: dict[str, str]) -> int:
    """index.update, in a process of its own where it can be started. Gives the songs
    in the catalogs; the collections that failed, and why, are put in `failed`."""
    if APART:
        try:
            done = apart.run("bbmp.catalogs:_build_here",
                             {"only": list(only), "folder": str(folder), "chosen": list(chosen)},
                             progress)
            failed.update({str(c): str(why) for c, why in done["failed"].items()})
            return int(done["songs"])
        except apart.NotStarted:
            pass            # built here, then
    try:
        return index_mod.update(progress, only=only, db_path=folder, chosen=chosen,
                                failed=failed)
    finally:
        _give_back_memory()


def _build_here(progress: Progress, only: list[str], folder: str, chosen: list[str]) -> dict:
    """What _build does, in the process apart.py starts."""
    failed: dict[str, str] = {}
    songs = index_mod.update(progress, only=only, db_path=Path(folder), chosen=chosen,
                             failed=failed)
    return {"songs": songs, "failed": failed}


def _give_back_memory() -> None:
    """Hand the memory Python has freed back to the system, where the C library can
    (glibc; elsewhere nothing happens): after an update built in this process, 282 MB
    stayed taken (11 October 2026)."""
    import ctypes
    import ctypes.util
    try:
        ctypes.CDLL(ctypes.util.find_library("c") or "libc.so.6").malloc_trim(0)
    except (OSError, AttributeError):
        pass


def choose(settings: Settings, ids: list[str], progress: Progress = lambda s: None,
           folder: Path | None = None) -> list[str]:
    """The user has chosen these collections: remove the catalogs of the others, and
    make the ones that are missing. Gives the ones turned off. Raises Busy when an
    update is at work."""
    folder = folder if folder is not None else paths.CATALOG_DIR
    with updating(folder):
        return _choose(settings, ids, progress, folder)


def _choose(settings: Settings, ids: list[str], progress: Progress, folder: Path) -> list[str]:
    before = set(chosen(settings, folder))
    ids = [c for c in ids if collections_.get(c) is not None]
    off = [c for c in index_mod._catalogs(folder) if c not in ids]
    for c in off:
        index_mod.remove(c, folder)
    # a hosted collection whose host comes or goes is made anew the other way: from
    # its host's list, or from its own (which also has what the host's kept for it)
    for c in ids:
        host = collections_.get(c).hosted_by
        if host and (host in before) != (host in ids):
            index_mod.remove(c, folder)
    settings.update(collections=ids)
    _prepare(settings, progress, folder)
    return off


def update(settings: Settings, progress: Progress = lambda s: None,
           only: Iterable[str] = (), folder: Path | None = None) -> Updated:
    """Update the catalogs of the chosen collections, or of the ones of them named: a
    collection that isn't chosen is not updated. A hosted collection comes along with
    its host, since its list is had anyway.

    A collection that can't be updated keeps the catalog it had, and is in what is
    given back (Updated.failed); the time of the last update is set only when none
    failed, so that an update that is due is tried again. Raises OSError when none
    could be updated, Busy when an update is at work already.
    """
    folder = folder if folder is not None else paths.CATALOG_DIR
    with updating(folder):
        ids = chosen(settings, folder)
        wanted = [c for c in (only or ids) if c in ids]
        for c in collections_.known():
            if c.hosted_by in wanted and c.id in ids and c.id not in wanted:
                wanted.append(c.id)
        if not wanted:
            return Updated()
        done = Updated()
        done.songs = _build(progress, wanted, folder, ids, done.failed)
        if not done.failed:
            settings.update(last_update=int(time.time()))
        return done


def due(settings: Settings, folder: Path | None = None) -> bool:
    """Is it time to update by itself?"""
    folder = folder if folder is not None else paths.CATALOG_DIR
    days = EVERY.get(settings.update_every, EVERY["week"])[0]
    if days is None:
        return False
    built = [b for c, b in index_mod._kept(folder).items() if c in chosen(settings, folder)]
    if not built:
        return False
    last = max(min(built), settings.last_update or 0)
    return time.time() - last >= days * 86400 - 3600      # an hour's leeway


def summary(ids: Iterable[str]) -> str:
    names = [collections_.name(c) for c in ids]
    return ", ".join(names) if names else "none"
