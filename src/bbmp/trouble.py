# SPDX-License-Identifier: GPL-3.0-or-later
"""Songs known not to play, one by one: in formats that play, some songs still fail.

Two sources: the format census, which tried up to 40 songs of every format
(known_trouble.py, shipped with the player), and this computer, which notes a song
that fails to open or stays silent the moment it happens (the library's `trouble`
table). A song that plays after all is taken off the list.

The player marks such songs in its lists, says why in the Info tab, and radio passes
them over. A song played on purpose is still tried: the reason may be gone.
"""

from __future__ import annotations

import threading

from . import known_trouble


class Trouble:
    def __init__(self, library=None, known: dict | None = None):
        self.library = library
        self.known = dict(known_trouble.KNOWN if known is None else known)
        self._here: dict[str, tuple[str, str]] = library.trouble() if library else {}
        self._cleared: set[str] = set()        # census songs that played here after all
        self._lock = threading.Lock()           # the player notes, the screen reads

    def get(self, key: str) -> str | None:
        """Why the song doesn't play, if that is known; None if it isn't."""
        with self._lock:
            found = self._here.get(key) or (self.known.get(key) if key not in self._cleared
                                            else None)
        return found[1] if found else None

    def keys(self) -> set[str]:
        with self._lock:
            return set(self._here) | (set(self.known) - self._cleared)

    def note(self, key: str, kind: str, why: str) -> None:
        """It failed here: kind "failed" or "silent"."""
        with self._lock:
            if self._here.get(key) == (kind, why):
                return
            self._here[key] = (kind, why)
            self._cleared.discard(key)
        if self.library is not None:
            self.library.note_trouble(key, kind, why)

    def played(self, key: str) -> None:
        """It made a sound here: it is not in trouble (any more)."""
        with self._lock:
            was = self._here.pop(key, None) is not None
            if key in self.known:
                self._cleared.add(key)
        if was and self.library is not None:
            self.library.clear_trouble(key)
