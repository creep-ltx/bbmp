# SPDX-License-Identifier: GPL-3.0-or-later
"""Where things live on disk (XDG locations)."""

import os
import sys
from pathlib import Path

NAME = "ByteBandit Music Player"
ID = "bbmp"              # the command, and the folders' name
OLD_ID = "modland-player"   # what the folders were called before the player had its name


def env(name: str) -> str | None:
    """The environment variable BBMP_<name>, or else MODLAND_<name>: what it was called
    before the player had its name, so that what a user set then still counts."""
    value = os.environ.get(f"BBMP_{name}")
    return value if value is not None else os.environ.get(f"MODLAND_{name}")


def shown(path: Path | str) -> str:
    """A path as the user is shown it: their home folder as ~."""
    path, home = Path(path), Path.home()
    return f"~/{path.relative_to(home)}" if path.is_relative_to(home) and path != home else str(path)


def _xdg(var: str, fallback: str) -> Path:
    return Path(os.environ.get(var) or Path.home() / fallback) / ID


def _old(var: str, fallback: str) -> Path:
    return Path(os.environ.get(var) or Path.home() / fallback) / OLD_ID


DATA_DIR = _xdg("XDG_DATA_HOME", ".local/share")
CACHE_DIR = _xdg("XDG_CACHE_HOME", ".cache")
CONFIG_DIR = _xdg("XDG_CONFIG_HOME", ".config")

CATALOG_DIR = DATA_DIR / "catalogs"   # a catalog for each collection, made from its file list
INDEX_DB = DATA_DIR / "index.db"      # the one catalog there was before: split when met
USER_DB = DATA_DIR / "user.db"        # favorites, playlists and plays, never rebuilt
SESSION = DATA_DIR / "session.json"   # the queue and position, for resuming
SETTINGS = CONFIG_DIR / "settings.json"
FILES_DIR = CACHE_DIR / "files"       # downloaded songs: <collection>/<the path it has there>
NATIVE_DIR = DATA_DIR / "native"      # compiled helper libraries (the SID shim)
ERROR_LOG = DATA_DIR / "errors.log"  # what went wrong in a background job, for a bug report


def _music_dir() -> Path:
    """The user's music folder: what the desktop says it is (it has another name in
    other languages), else ~/Music."""
    try:
        for line in (CONFIG_DIR.parent / "user-dirs.dirs").read_text().splitlines():
            name, _, value = line.strip().partition("=")
            value = value.strip().strip('"')
            if name == "XDG_MUSIC_DIR" and value:
                value = value.replace("$HOME", str(Path.home()))
                if value.startswith("/") and Path(value) != Path.home():
                    return Path(value)
    except OSError:
        pass
    return Path.home() / "Music"


MUSIC_DIR = _music_dir()

# What the user downloads to keep, a song or a whole collection, and the database of
# it: in the music folder, where a file manager finds it, in a folder for every
# collection. The cache is not there: it is the player's own business. The player's
# own folder is the other place Settings offers for kept music.
KEPT_DIR = MUSIC_DIR / "ByteBandit Music Player"
OWN_KEPT_DIR = DATA_DIR / "kept"


def kept_dir(setting: str = "") -> Path:
    """Where kept music is: what BBMP_KEPT says, else the setting, else KEPT_DIR.
    Music that was kept in the player's own folder is still kept there."""
    named = env("KEPT") or setting
    if named:
        return Path(named).expanduser()
    if (OWN_KEPT_DIR / "kept.db").is_file() and not (KEPT_DIR / "kept.db").is_file():
        return OWN_KEPT_DIR
    return KEPT_DIR


EXPORT_DIR = KEPT_DIR / "Exports"      # songs written as FLAC or WAV

EXTRAS_DIR = DATA_DIR / "extras"       # files only the user can supply: ROMs, sample sets
RHYTHM_ROM = Path.home() / ".local/share/98fmplayer/ym2608_adpcm_rom.bin"   # where 98fmplayer looks
SHROOM_NOTES = DATA_DIR / "shroom-notes.bin"   # Mario Paint's instruments as Shroom's replayer reads them

# what a shared library's name ends in here
SO = ".dylib" if sys.platform == "darwin" else ".so"

# Where the replayers are built: ~/.local/opt, or BBMP_OPT (the Flatpak's /app/opt)
OPT = Path(env("OPT") or Path.home() / ".local/opt")
UADE_PREFIX = Path(env("UADE_PREFIX") or OPT / "uade")
ZXTUNE_PREFIX = Path(env("ZXTUNE_PREFIX") or OPT / "zxtune")
CHIP_PREFIX = Path(env("CHIP_PREFIX") or OPT / "bbmp-chip")
FURNACE_PREFIX = Path(env("FURNACE_PREFIX") or OPT / "furnace")
# Game Music Emu with the player's fix (tools/build-gme.sh); else the system's
GME_PREFIX = Path(env("GME_PREFIX") or OPT / "gme")


def helper_prefix(name: str) -> Path:
    """Where the helper bbmp-<name> is installed: OPT/bbmp-<name>, or what
    BBMP_<NAME>_PREFIX says."""
    return Path(env(f"{name.upper()}_PREFIX") or OPT / f"bbmp-{name}")


XMP_PREFIX = helper_prefix("xmp")
RV_PREFIX = helper_prefix("rv")


OLD_COPY = ".from-modland-player"     # what an old file is called where the new one has its name


def move_old_folders() -> list[str]:
    """The player was called Modland Player, and kept its things in folders of that
    name. Move them to the new ones, once, where the new one isn't there yet. Gives
    what was moved.

    Where the new folder is there already, what it doesn't have is moved into it.
    A file both have (a user.db, a settings.json, kept.db) is not merged: the new one
    is used, and the old one is put next to it as "user.db.from-modland-player", so
    that nothing the user made is lost and it can be had back by hand. A folder both
    have, and what the caches both have, are left in the old folder, as before."""
    moved = []
    pairs = [(_old("XDG_DATA_HOME", ".local/share"), DATA_DIR),
             (_old("XDG_CONFIG_HOME", ".config"), CONFIG_DIR),
             (_old("XDG_CACHE_HOME", ".cache"), CACHE_DIR),
             (MUSIC_DIR / "Modland Player", KEPT_DIR)]
    cache = pairs[2][0]
    for old, new in pairs:
        if not old.is_dir() or old.is_symlink():
            continue
        try:
            if not new.exists():
                new.parent.mkdir(parents=True, exist_ok=True)
                old.rename(new)
                moved.append(f"{old} -> {new}")
                continue
            # the new folder is there already (something made it first): move what it
            # doesn't have, and leave the old folder with what is left
            for item in old.iterdir():
                to = new / item.name
                if to.exists() and old != cache and item.is_file():
                    to = new / (item.name + OLD_COPY)
                if not to.exists():
                    item.rename(to)
                    moved.append(f"{item} -> {to}")
            if not any(old.iterdir()):
                old.rmdir()
        except OSError:
            pass            # another disk: left where it is
    return moved
