# SPDX-License-Identifier: GPL-3.0-or-later
"""Polly Tracker (Aleksi Eeben, 2005): four channels of samples on a Commodore 64.

The tracker's own module-to-SID tool makes a tune of its player (962 bytes, loaded at
$0801) followed by the module as it is saved; four of Modland's five modules are in
HVSC made so, and the players in them differ only in byte 43, the high byte of where
the module ends. The player is not part of this program: it
is taken from HVSC's Polly_Rock.sid, which is downloaded like any other tune, and the
song is put behind it and played by libsidplayfp.
"""

from __future__ import annotations

import hashlib
import tempfile
from pathlib import Path

from .base import Decoder, Unsupported

SOURCE = "HVSC/DEMOS/UNKNOWN/Polly_Rock.sid"
SOURCE_MD5 = "bfe033969b4a5d1374cfe788904f10af"
HEADER = 0x7C           # PSID/RSID version 2
PLAYER = 962            # the load address $0801 and the player
LOAD = 0x0801
END_HIGH = 43           # in the player: the high byte of the module's last address
NAME = 0x16             # the header's three 32-byte texts: name, author, released


def _player(fetch) -> bytes:
    """The header and player of the tune at SOURCE."""
    try:
        data = fetch(SOURCE).read_bytes()
    except (OSError, EOFError) as e:
        raise Unsupported(f"Polly Tracker's player comes from HVSC's Polly_Rock.sid, "
                          f"which couldn't be had: {e}") from None
    if hashlib.md5(data).hexdigest() != SOURCE_MD5:
        raise Unsupported("HVSC's Polly_Rock.sid is not the file it should be")
    return data[:HEADER + PLAYER]


def tune(module: bytes, title: str, player: bytes) -> bytes:
    """An RSID of the player and the module."""
    end = LOAD + PLAYER - 2 + len(module) - 1
    if not module or end > 0xFFFF:
        raise Unsupported("not a Polly Tracker module: it doesn't fit into the C64's memory")
    out = bytearray(player + module)
    out[HEADER + END_HIGH] = end >> 8
    out[NAME:NAME + 96] = bytes(96)
    out[NAME:NAME + 32] = title.encode("latin-1", "replace")[:31].ljust(32, b"\0")
    return bytes(out)


class Polly(Decoder):
    """libsidplayfp playing the module made into a tune, in a folder that goes with it."""

    def __init__(self, path: Path, lengths, open_sid, fetch):
        module = path.read_bytes()[:0x10000]
        self._tmp = tempfile.TemporaryDirectory(prefix="bbmp-polly-")
        sid = Path(self._tmp.name) / (path.stem + ".sid")
        try:
            sid.write_bytes(tune(module, path.stem, _player(fetch)))
            self._sid = open_sid(sid, lengths)
        except Exception:
            self._tmp.cleanup()
            raise
        self.backend = "polly"
        self.info = self._sid.info
        self.info.format = "Polly Tracker"

    def read(self, frames: int) -> bytes:
        return self._sid.read(frames)

    def seek(self, seconds: float) -> bool:
        return self._sid.seek(seconds)

    def select_subsong(self, n: int) -> None:
        self._sid.select_subsong(n)

    def details(self):
        return self._sid.details()

    def channel_levels(self):
        return self._sid.channel_levels()

    def loop_forever(self, on: bool) -> bool:
        return self._sid.loop_forever(on)

    def close(self) -> None:
        self._sid.close()
        if self._tmp is not None:
            self._tmp.cleanup()
            self._tmp = None
