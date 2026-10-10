# SPDX-License-Identifier: GPL-3.0-or-later
"""CYM: logs of what a Capcom arcade game (CPS-1) wrote to its YM2151, made by the
MAME and Kawaks emulators. Modland files them as "Megadrive CYM", but the chip is the
arcade boards' YM2151 (OPM), not the Mega Drive's.

The file is pairs of register and value; a register of 0 (which the YM2151 doesn't
have) is one frame's wait, 1/60 s, and has no value. Here it is turned into a VGM
file, which bbmp-vgm plays (it knows the YM2151).
"""

from __future__ import annotations

import struct
import tempfile
from pathlib import Path

from .base import Unsupported
from .pipe import PipeDecoder

CLOCK = 3_579_545          # the CPS-1's YM2151
FRAME = 735                # 44100 / 60: VGM counts time in samples at 44.1 kHz
HEADER = 0x100
MAX_FRAMES = 60 * 60 * 60  # an hour; a damaged file can't ask for more


def to_vgm(data: bytes) -> bytes:
    """The CYM log as a VGM 1.51 file for one YM2151."""
    out, frames, i, writes = bytearray(), 0, 0, 0
    while i < len(data) and frames < MAX_FRAMES:
        reg = data[i]
        if reg == 0:
            out.append(0x62)            # wait 735 samples
            frames += 1
            i += 1
        elif i + 1 < len(data):
            out += bytes((0x54, reg, data[i + 1]))
            writes += 1
            i += 2
        else:
            break
    if not writes or not frames:
        raise Unsupported("not a CYM file: no writes to the YM2151, or no time")
    out.append(0x66)
    head = bytearray(HEADER)
    struct.pack_into("<4sIIIIIIII", head, 0, b"Vgm ", HEADER + len(out) - 4, 0x151,
                     0, 0, 0, frames * FRAME, 0, 0)
    struct.pack_into("<I", head, 0x24, 60)               # rate
    struct.pack_into("<I", head, 0x30, CLOCK)            # YM2151
    struct.pack_into("<I", head, 0x34, HEADER - 0x34)    # data offset
    return bytes(head + out)


class CYM(PipeDecoder):
    """The song as VGM, in a folder of its own that goes when the song is closed."""

    def __init__(self, path: Path):
        try:
            vgm = to_vgm(path.read_bytes()[:16 << 20])
        except OSError as e:
            raise Unsupported(str(e)) from None
        self._tmp = tempfile.TemporaryDirectory(prefix="bbmp-cym-")
        out = Path(self._tmp.name) / (path.stem + ".vgm")
        out.write_bytes(vgm)
        try:
            super().__init__("vgm", out)
        except Exception:
            self._tmp.cleanup()
            raise
        self.backend = "cym"
        self.info.format = "CYM (Capcom YM2151 log)"

    def close(self) -> None:
        super().close()
        if self._tmp is not None:
            self._tmp.cleanup()
            self._tmp = None
