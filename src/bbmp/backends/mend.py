# SPDX-License-Identifier: GPL-3.0-or-later
"""Songs a little damaged on the archive, mended so that their replayer takes them.

Some files are fine but for a detail the replayer checks strictly and the music never
uses: a file that lost its last bytes (the end of a name, of an unused table), numbers
in parts of the file the player never reads. Their replayer refuses them whole. Here a
copy is made with that detail put right, and the song is played from the copy. Only
what the replay does not depend on is changed; nothing is guessed about the music.

Tried only when the format's replayer refuses the file as it is (open_song). Found on
Modland's files, 2 October 2026:

- HivelyTracker / AHX (.hvl, .ahx): the last instrument name has lost its closing
  zero byte; ZXTune reads the names and gives up. (Syphus's "meltwater (10ch)", "top
  gun anthem (2nd version)".)
- Fast Tracker (.ftc): the file ends a few bytes inside its last ornament ("sanxion").
- Slight Atari Player (.sap): the last block of the program ends a few bytes before the
  block says ("rage against 16-bit").
- Sound Tracker 1.1 (.st11): numbers ZXTune's check doesn't allow and the replay never
  uses: a pattern's rows past the song's pattern length, high bits of a sample's tone
  shift and noise. And a sample loop that starts past the sample's 32 steps, which is
  taken as no loop. (disco, bilin3, housem, odzial, nszz.)
"""

from __future__ import annotations

import tempfile
from pathlib import Path
from typing import Callable

from .base import Decoder, Details, TrackInfo, Unsupported

MAX_SIZE = 16 << 20         # none of these formats has bigger files


def _names_closed(data: bytes) -> bytes | None:
    """HVL and AHX: the title and the instruments' names, each ending in a zero byte,
    are the end of the file. Put back the zeros the file lost."""
    if data[:3] not in (b"HVL", b"THX") or len(data) < 16:
        return None
    names = data[4] << 8 | data[5]
    if not 16 <= names <= len(data):
        return None
    missing = data[12] + 1 - data[names:].count(0)       # the title and each instrument
    return data + bytes(missing) if 0 < missing <= 64 else None


# A Fast Tracker object (sample or ornament) is at most 3 bytes and 256 steps of 5 bytes:
# that many zeros past the end are enough for any that was cut, and the replayer reads
# no further than the object's own length.
FTC_PAD = 3 + 256 * 5


def _ftc_cut(data: bytes) -> bytes | None:
    """Fast Tracker: the file ends inside its last sample or ornament."""
    return data + bytes(FTC_PAD) if data.startswith(b"Module: ") else None


def _sap_cut(data: bytes) -> bytes | None:
    """SAP: text lines, then the program as Atari binary blocks (start and end address,
    then the bytes). A last block that ends early is filled up with zeros."""
    at = data.find(b"\xff\xff")
    if not data.startswith(b"SAP") or at < 0:
        return None
    at += 2
    while at + 4 <= len(data):
        if data[at:at + 2] == b"\xff\xff":
            at += 2
            continue
        start, end = data[at] | data[at + 1] << 8, data[at + 2] | data[at + 3] << 8
        if end < start:
            return None
        at += 4 + end - start + 1
    missing = at - len(data)
    return data + bytes(missing) if 0 < missing <= 0x10000 else None


# Sound Tracker 1.1 (ZXTune's "Sound Tracker v1.x", uncompiled): 15 samples of 130 bytes
# (32 volumes, 32 noise bytes, 32 tone shifts of 16 bits, loop start and length), 256
# positions, the length, 17 ornaments, the delay, the pattern length, then patterns of
# 64 rows of three channels of three bytes. Modland's files have a 56-byte "ZXAYST11"
# header before it.
ST_SAMPLES, ST_SAMPLE = 15, 130
ST_PATTERN_LENGTH = 3008
ST_PATTERNS = 3009
ST_PATTERN, ST_ROW = 576, 9


def _st11(data: bytes) -> bytes | None:
    """Sound Tracker 1.1: what ZXTune checks and the replay doesn't use, put right."""
    base = 56 if data.startswith(b"ZXAYST11") else 0
    if len(data) < base + ST_PATTERNS + ST_PATTERN:
        return None
    out = bytearray(data)
    for s in range(ST_SAMPLES):
        at = base + s * ST_SAMPLE
        for i in range(32):
            out[at + 32 + i] &= 0xDF            # noise: bit 5 means nothing
            out[at + 64 + 2 * i + 1] &= 0x1F    # tone shift: the replay uses 13 bits
        loop, length = at + 128, at + 129
        if out[loop] > 31:                      # past the sample's end: no loop
            out[loop], out[length] = 0, 0
        out[length] = min(out[length], 31)      # the loop ends at the sample's end anyway
    rows = out[base + ST_PATTERN_LENGTH]
    if not 1 <= rows <= 64:
        return None
    first = base + ST_PATTERNS
    for p in range((len(out) - first) // ST_PATTERN):
        at = first + p * ST_PATTERN
        out[at + rows * ST_ROW:at + ST_PATTERN] = bytes(ST_PATTERN - rows * ST_ROW)
    return bytes(out) if out != data else None


MENDERS: dict[str, Callable[[bytes], bytes | None]] = {
    ".hvl": _names_closed, ".ahx": _names_closed, ".ftc": _ftc_cut, ".sap": _sap_cut,
    ".st11": _st11}


def mended(path: Path) -> bytes | None:
    """The file put right, or None when there is nothing to mend (or no rule for it)."""
    mend = MENDERS.get(path.suffix.lower())
    if mend is None:
        return None
    try:
        if path.stat().st_size > MAX_SIZE:
            return None
        data = path.read_bytes()
    except OSError:
        return None
    return mend(data)


class Mended(Decoder):
    """A song played from a mended copy, in a folder of its own that goes when the song
    is closed. Everything else is the replayer's."""

    def __init__(self, inner: Decoder, tmp: tempfile.TemporaryDirectory):
        self._inner, self._tmp = inner, tmp
        self.backend = inner.backend

    @property
    def info(self) -> TrackInfo:
        return self._inner.info

    @property
    def never_ends(self) -> bool:
        return self._inner.never_ends

    def read(self, frames: int) -> bytes:
        return self._inner.read(frames)

    def select_subsong(self, n: int) -> None:
        self._inner.select_subsong(n)

    def seek(self, seconds: float) -> bool:
        return self._inner.seek(seconds)

    def channel_levels(self) -> list[float] | None:
        return self._inner.channel_levels()

    def details(self) -> Details:
        return self._inner.details()

    def loop_forever(self, on: bool) -> bool:
        return self._inner.loop_forever(on)

    def pattern_view(self, rows: int) -> tuple[int, list[str]] | None:
        return self._inner.pattern_view(rows)

    def close(self) -> None:
        self._inner.close()
        if self._tmp is not None:
            self._tmp.cleanup()
            self._tmp = None


def open_mended(path: Path, opener: Callable[[Path, list], Decoder],
                lengths: list[float]) -> Decoder | None:
    """The song from a mended copy, with `opener`; None when there's nothing to mend or
    the replayer refuses the copy too."""
    data = mended(path)
    if data is None:
        return None
    tmp = tempfile.TemporaryDirectory(prefix="bbmp-mended-")
    copy = Path(tmp.name) / path.name
    try:
        copy.write_bytes(data)
        return Mended(opener(copy, list(lengths)), tmp)
    except (Unsupported, OSError):
        tmp.cleanup()
        return None
