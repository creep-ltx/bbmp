# SPDX-License-Identifier: GPL-3.0-or-later
"""The `bbmp` command for a player installed as a Flatpak, or run as an AppImage.

A Flatpak may not put commands on the system's path when it is installed: it is started
with `flatpak run io.github.bytebandit.bbmp`. Nor may it write to ~/.local/bin (a program
put there runs outside the Flatpak, so that permission would be a way out of it). So
inside a Flatpak the player gives, once at its first start and with `bbmp command`, the
line to type that makes the command: a two-line script in ~/.local/bin.

An AppImage is one file that can be anywhere (the Downloads folder): the player makes
the command itself, a script that starts that file, where it was when the command was
made ($APPIMAGE). A file of that name that the player didn't make is never touched.
"""

from __future__ import annotations

import os
import shlex
import shutil
from pathlib import Path

APP_ID = "io.github.bytebandit.bbmp"
MARK = "# made by ByteBandit Music Player"
SCRIPT = f"""#!/bin/sh
{MARK}: starts the player from its Flatpak
exec flatpak run {APP_ID} "$@"
"""


def in_flatpak() -> bool:
    return bool(os.environ.get("FLATPAK_ID")) or Path("/.flatpak-info").exists()


def appimage() -> str:
    """The AppImage file the player runs from, or ""."""
    return os.environ.get("APPIMAGE", "")


def packaged() -> bool:
    return in_flatpak() or bool(appimage())


def script() -> str:
    if appimage():
        # quoted for the shell: a file name with $(...), ` or " in it is only a name
        return (f'#!/bin/sh\n{MARK}: starts the player from its AppImage\n'
                f'exec {shlex.quote(appimage())} "$@"\n')
    return SCRIPT


def line() -> str:
    """What to type in a terminal to make the command (from inside a Flatpak, which
    may not make it itself)."""
    return ("mkdir -p ~/.local/bin && printf '%s\\n' '#!/bin/sh' "
            f"'{MARK}: starts the player from its Flatpak' "
            f"'exec flatpak run {APP_ID} \"$@\"' > ~/.local/bin/bbmp && "
            "chmod +x ~/.local/bin/bbmp")


def how_to_start() -> str:
    return appimage() or f"flatpak run {APP_ID}"


def target() -> Path:
    return Path.home() / ".local" / "bin" / "bbmp"


def ours(path: Path) -> bool:
    try:
        return MARK in path.read_text(errors="replace")[:400]
    except OSError:
        return False


def should_offer(asked: bool) -> bool:
    """Offer it at the first start: in a Flatpak or an AppImage, not asked before, and
    no bbmp there."""
    return packaged() and not asked and not target().exists()


def make() -> str:
    """Make the command; says in plain words what was done, or why not."""
    path = target()
    if in_flatpak() and not appimage():
        return ("A Flatpak can't add commands by itself. To start the player by typing bbmp, "
                f"type this once in a terminal (outside the player):\n\n{line()}\n\n"
                f"Until then: {how_to_start()}")
    if (path.exists() or path.is_symlink()) and not ours(path):
        return (f"{path} is there already and isn't the player's: it was left alone. "
                f"Start the player with: {how_to_start()}")
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        # written beside it and put in its place: a link there is replaced, not followed
        tmp = path.with_name(f".{path.name}.{os.getpid()}.tmp")
        try:
            tmp.write_text(script())
            tmp.chmod(0o755)
            os.replace(tmp, path)
        finally:
            tmp.unlink(missing_ok=True)
    except OSError as e:
        return f"The bbmp command could not be made in {path.parent}: {e.strerror or e}"
    said = f"Made {path}: type bbmp in a terminal to start the player."
    if not packaged() and shutil.which("bbmp") not in (None, str(path)):
        said += " (Another bbmp comes first on your path.)"
    if appimage():
        said += f" It starts {appimage()}: if you move that file, make the command again."
    if str(path.parent) not in os.environ.get("PATH", "").split(":") and not in_flatpak():
        said += f" {path.parent} isn't on your path yet: add it in your shell's settings."
    elif in_flatpak():
        said += (f" If your terminal doesn't find it, {path.parent} isn't on your path: "
                 "add it in your shell's settings.")
    return said
