# SPDX-License-Identifier: GPL-3.0-or-later
"""User settings, saved to disk the moment they change."""

from __future__ import annotations

import json
import os
import threading
from dataclasses import asdict, dataclass, field, fields
from pathlib import Path

from . import paths

try:
    _MUSIC = "~/" + str(paths.MUSIC_DIR.relative_to(Path.home()))
except ValueError:
    _MUSIC = str(paths.MUSIC_DIR)

CACHE_LIMITS_MB = (500, 1000, 2000, 5000, 10000, 0)   # 0 = no limit


@dataclass
class Settings:
    volume: int = 80
    visualizer: str | None = None     # one of visuals.MODES, or None
    subsongs: str = "all"             # all: play every subsong like an album; one: first only
    repeat: str = "off"               # off | all | one
    shuffle: bool = False
    cache_limit_mb: int = 2000        # 0 = no limit
    resume: bool = True               # reopen the last queue and position on start
    music_dir: str = _MUSIC           # the folder the Files tab shows
    export_format: str = "flac"       # flac | mp3 | ogg | opus | wav
    theme: str = "bytebandit"         # the player's own, an Amiga one or any of Textual's
    collection: str = ""              # the collection to browse and search; "" is all of them
    view: str = ""                    # browse by "" (collections), platform:<id> or chip:<id>
    browse: list = field(default_factory=list)   # where Browse is: [[kind, value], ...]
    asked_downloads: bool = False     # the downloads were offered once, at the first start
    asked_command: bool = False       # in a Flatpak: the bbmp command was offered once
    level: bool = True                # play every song at the same loudness
    demo_effects: bool = False        # v cycles through the demo effects too
    keys: dict = field(default_factory=dict)   # other keys, by the bindings' ids
    collections: list | None = None   # the ones the user has; None: not chosen yet
    update_every: str = "week"        # day | week | 2weeks | month | never (catalogs.EVERY)
    last_update: int = 0              # when the catalogs were last updated (seconds since 1970)
    keep_when: str = "any"            # when saved music is fetched: any | night (00-07)
    keep_speed: int = 0               # KB a second at the most; 0: no limit
    keep_dir: str = ""                # where kept music is; "" is its folder in the music folder
    mirror: str = ""                  # a home mirror's address, asked before the collections' servers

    @classmethod
    def load(cls, path: Path = paths.SETTINGS) -> "Settings":
        s = cls()
        s._path = path
        try:
            data = json.loads(path.read_text())
        except (OSError, ValueError):
            return s
        if not isinstance(data, dict):      # null, [], a number: the defaults
            return s
        defaults = cls()
        for f in fields(cls):
            if f.name not in data:
                continue
            value = _of_kind(data[f.name], f.type)
            if value is not _WRONG:         # one of the wrong kind keeps its default
                object.__setattr__(s, f.name, value)
        return s

    def update(self, **changes) -> None:
        """Change settings and save them straight away."""
        for k, v in changes.items():
            if k not in {f.name for f in fields(self)}:
                raise KeyError(k)
            setattr(self, k, v)
        self.save()

    def save(self) -> None:
        # Only settings that came from a file are written back (tests use plain Settings()).
        path = getattr(self, "_path", None)
        if path is None:
            return
        with _save_lock:
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                # its own name: two players saving at once don't write into one file
                tmp = path.with_name(f"{path.stem}.{os.getpid()}.{threading.get_ident()}.tmp")
                tmp.write_text(json.dumps(asdict(self), indent=2) + "\n")
                os.replace(tmp, path)
            except OSError:
                pass   # settings are a convenience; never let saving break playback


_save_lock = threading.Lock()

_WRONG = object()
_KINDS = {"bool": bool, "int": int, "float": float, "str": str, "list": list, "dict": dict,
          "None": type(None)}


def _of_kind(value, annotation):
    """`value` as a setting of that type ("int", "str | None", ...), or _WRONG: a hand-
    edited settings.json may hold anything, and the player must still start."""
    names = [n.strip() for n in str(annotation).split("|")]
    if any(n not in _KINDS for n in names):
        return value                # a kind not checked here
    for n in names:
        kind = _KINDS[n]
        if kind is int and isinstance(value, float) and value.is_integer():
            return int(value)
        if kind is float and isinstance(value, int) and not isinstance(value, bool):
            return float(value)
        if isinstance(value, kind) and not (kind is int and isinstance(value, bool)):
            return value
    return _WRONG
