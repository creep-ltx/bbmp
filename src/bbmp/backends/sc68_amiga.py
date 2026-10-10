# SPDX-License-Identifier: GPL-3.0-or-later
"""SC68 files that hold Amiga music.

An SC68 file is a container: besides Atari ST music it can hold an Amiga song
together with the name of the replay routine sc68 should run it on ("sidmon1",
"soundfx", "deltamusic2"). sc68 has only some of those routines, and without the
one named it stays silent. UADE has the Amiga players themselves: the song data is
taken out of the container and given to UADE.
"""

from __future__ import annotations

import re
import struct
import tempfile
from pathlib import Path

from .. import paths
from ..download_pins import SC68_REPLAYS as DOWNLOADED_REPLAYS
from .base import Unsupported

SC68_REPLAYS = paths.CHIP_PREFIX.parent / "sc68/share/sc68/Replay"
# the names UADE knows the players by, where they differ from sc68's
PREFIXES = {"sidmon1": "sid1", "sidmon2": "sid2", "soundfx": "sfx", "deltamusic2": "dm2",
            "deltamusic1": "dm1", "futurecomposer": "fc", "soundmon": "bp"}
# the name of a replay routine, as sc68 names them: it becomes part of a file name
_REPLAY_NAME = re.compile(r"[A-Za-z0-9_.+-]{1,64}")


def _chunks(data: bytes) -> list[tuple[bytes, bytes]]:
    at = data.find(b"SC68", 16)
    if not data.startswith(b"SC68 Music-file") or at < 0:
        return []
    out, at = [], at + 8
    while at + 8 <= len(data):
        tag = data[at:at + 4]
        (size,) = struct.unpack_from("<I", data, at + 4)
        out.append((tag, data[at + 8:at + 8 + size]))
        at += 8 + size
        if tag == b"SCEF":
            break
    return out


def amiga_song(path: Path, track: int = 0) -> tuple[str, bytes] | None:
    """(replay routine, song data) of a track whose routine sc68 doesn't have."""
    try:
        chunks = _chunks(path.read_bytes())
    except OSError:
        return None
    replay, n = "", -1
    for tag, value in chunks:
        if tag == b"SCMU":
            n += 1
            replay = ""
        elif tag == b"SCRE":
            replay = value.rstrip(b"\0").decode("latin-1").strip()
            if not _REPLAY_NAME.fullmatch(replay):
                return None     # no name of a routine: a path ("../..") or garbage
        elif tag == b"SCDA" and replay and n in (track, -1):
            # one of sc68's own: installed, or one of those downloaded on request
            # (sc68replays), which sc68 plays or, missing, refuses with advice. By the
            # name as it is written, as the files' names were looked for before (an Amiga
            # routine named "JamCrackerPro" goes to UADE, though sc68 has jamcrackerpro)
            if (SC68_REPLAYS / f"{replay}.bin").exists() or replay in DOWNLOADED_REPLAYS:
                return None
            return replay, value
    return None


def open_amiga(path: Path, opener) -> object:
    """The Amiga song of an SC68 file, opened with `opener` (UADE's) from a
    temporary file named as UADE's player for it expects."""
    found = amiga_song(path)
    if found is None:
        raise Unsupported("not Amiga music that sc68 lacks the replay routine of")
    replay, data = found
    tmp = tempfile.TemporaryDirectory(prefix="modland-sc68-")
    file = Path(tmp.name) / f"{PREFIXES.get(replay, replay)}.{path.stem}"
    file.write_bytes(data)
    try:
        decoder = opener(file, [])
    except BaseException:
        tmp.cleanup()
        raise
    close = decoder.close

    def close_and_clean() -> None:        # the temporary file goes with the decoder
        close()
        tmp.cleanup()
    decoder.close = close_and_clean
    return decoder
