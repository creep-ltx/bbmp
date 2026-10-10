# SPDX-License-Identifier: GPL-3.0-or-later
"""What a song's own file says it plays on, read from the first bytes of it: the system a
Deflemask song was made for, the chips a VGM file drives, the NES expansion chips a
FamiTracker song uses, the SID a C64 tune names.
For the Platform and Chip views, song by song (platforms.py, the catalogs' chips
table): a Deflemask song is the Mega Drive's or the Game Boy's, not every system
Deflemask knows. tools/song_chips.py reads them for a whole catalog.

A platform is written "platform:<id>" in the chips table, a chip as its id.
"""

from __future__ import annotations

import gzip
import io
import zlib

HEAD = 4096     # bytes of a file that are enough (VGZ and DMF unpacked from them)

# Deflemask's system byte (Furnace's src/engine/sysDef.cpp, id_DMF) -> platform, chips
DMF_SYSTEMS: dict[int, tuple[str, tuple[str, ...]]] = {
    0x02: ("megadrive", ("ym2612", "sn76489")),
    0x42: ("megadrive", ("ym2612", "sn76489")),           # extended channel 3
    0x03: ("mastersystem", ("sn76489",)),
    0x43: ("mastersystem", ("sn76489", "opll")),         # with the FM unit
    0x04: ("gb", ("gbapu",)),
    0x05: ("pcengine", ("huc6280",)),
    0x06: ("nes", ("nesapu",)),
    0x46: ("nes", ("nesapu", "nesexp")),                  # with the VRC7
    0x86: ("nes", ("nesapu", "nesexp")),                  # Famicom Disk System
    0x47: ("c64", ("sid", "sid6581")),
    0x07: ("c64", ("sid", "sid8580")),
    0x08: ("arcade", ("ym2151", "segapcm")),              # DefleCade
    0x09: ("neogeo", ("opnb",)),
    0x49: ("neogeo", ("opnb",)),
    0x0A: ("msx", ("ay", "scc")),
}

# A VGM header's clock fields (VGM 1.71 layout) -> chip; a clock of 0 is no chip
VGM_CLOCKS: dict[int, str] = {
    0x0C: "sn76489", 0x10: "opll", 0x2C: "ym2612", 0x30: "ym2151", 0x38: "segapcm",
    0x40: "rf5c68", 0x44: "opn", 0x48: "opna", 0x4C: "opnb", 0x50: "opl", 0x54: "opl",
    0x58: "opl", 0x5C: "opl", 0x60: "opl4", 0x6C: "rf5c68", 0x74: "ay", 0x80: "gbapu",
    0x84: "nesapu", 0x90: "okim", 0x98: "okim", 0x9C: "scc", 0xA4: "huc6280",
    0xB0: "pokey", 0xB4: "qsound", 0xB8: "scsp", 0xC0: "wsc", 0xC4: "vsu", 0xC8: "saa1099",
    0xCC: "es5503",
}


def _unpacked(head: bytes, packer: str) -> bytes:
    """The start of a packed file, from its first bytes."""
    try:
        if packer == "gzip":
            return gzip.GzipFile(fileobj=io.BytesIO(head)).read1(HEAD)
        return zlib.decompressobj().decompress(head, HEAD)
    except (OSError, EOFError, zlib.error):
        # damaged, or cut short: take what came out before that
        try:
            return zlib.decompressobj(-15).decompress(head[2:], HEAD) if packer == "zlib" else b""
        except zlib.error:
            return b""


def dmf(head: bytes) -> tuple[str, ...]:
    """A Deflemask song's platform and chips, from its first bytes; () if not known."""
    raw = _unpacked(head, "zlib")
    if not raw.startswith(b".DelekDefleMask.") or len(raw) < 18:
        return ()
    found = DMF_SYSTEMS.get(raw[17])
    if found is None:
        return ()
    platform, chips = found
    return (f"platform:{platform}", *chips)


def vgm(head: bytes) -> tuple[str, ...]:
    """The chips a VGM or VGZ file drives, from its first bytes; () if not a VGM."""
    raw = head if head.startswith(b"Vgm ") else _unpacked(head, "gzip")
    if not raw.startswith(b"Vgm ") or len(raw) < 0x40:
        return ()
    version = int.from_bytes(raw[8:12], "little")
    start = 0x40
    if version >= 0x150:
        offset = int.from_bytes(raw[0x34:0x38], "little")
        start = 0x34 + offset if offset else 0x40
    out: list[str] = []
    for at, chip in VGM_CLOCKS.items():
        if at + 4 > min(start, len(raw)):
            continue
        clock = int.from_bytes(raw[at:at + 4], "little") & 0x3FFFFFFF
        if clock and chip not in out:
            out.append(chip)
    return tuple(out)


def ftm(head: bytes) -> tuple[str, ...]:
    """A FamiTracker song's chips: the NES's own, and the expansion chips (VRC6, VRC7,
    FDS, MMC5, N163, 5B) where its PARAMS block names any (FamiTracker's
    CFamiTrackerDoc: the block's first byte, from its version 2); () if not known."""
    if not head.startswith(b"FamiTracker Module") or len(head) < 22:
        return ()
    at = 22                                   # the name, and the file's version
    while at + 24 <= len(head):
        block = head[at:at + 16].rstrip(b"\0")
        version = int.from_bytes(head[at + 16:at + 20], "little")
        size = int.from_bytes(head[at + 20:at + 24], "little")
        if block == b"PARAMS":
            if version < 2 or at + 25 > len(head):
                return ("nesapu",)
            return ("nesapu", "nesexp") if head[at + 24] & 0x3F else ("nesapu",)
        if not block or size > len(head):
            return ()
        at += 24 + size
    return ()


# A PSID or RSID header's SID model (bits 4-5 of the flags at 0x76, version 2 and up)
SID_MODELS = {1: ("sid6581",), 2: ("sid8580",), 3: ("sid6581", "sid8580")}


def sid(head: bytes) -> tuple[str, ...]:
    """The SID a C64 tune was written for, from its header; () if it doesn't say."""
    if len(head) < 0x78 or head[:4] not in (b"PSID", b"RSID"):
        return ()
    if int.from_bytes(head[4:6], "big") < 2:
        return ()
    return SID_MODELS.get((int.from_bytes(head[0x76:0x78], "big") >> 4) & 3, ())


def read(fmt: str, head: bytes) -> tuple[str, ...]:
    """What the song says of itself, for the formats that say it."""
    if head[:4] in (b"PSID", b"RSID"):
        return sid(head)
    if fmt == "Deflemask":
        return dmf(head)
    if fmt == "FamiTracker":
        return ftm(head)
    if fmt.startswith("Video Game Music"):
        return vgm(head)
    return ()


def formats() -> tuple[str, ...]:
    """The formats read song by song (besides HVSC's SID models, tools/sid_chips.py)."""
    return ("Deflemask", "FamiTracker", "Video Game Music")


def says(fmt: str, path: str) -> bool:
    """Is it a song whose file says what it plays on?"""
    return fmt.startswith(formats()) or path.lower().endswith(".sid")


def head(path: str) -> bytes:
    """The first HEAD bytes of a file of Modland's (HVSC's tunes are there too, below
    HVSC/), from the first server that gives them: a Range request, not the file."""
    import urllib.parse
    import urllib.request
    from .collection import modland
    why: Exception | None = None
    for server in modland.SERVERS:
        url = server + modland.MODULES + "/" + urllib.parse.quote(path)
        request = urllib.request.Request(url, headers={
            "User-Agent": "bbmp", "Range": f"bytes=0-{HEAD - 1}"})
        try:
            with urllib.request.urlopen(request, timeout=30) as r:
                return r.read(HEAD)
        except OSError as e:
            why = e
    raise OSError(f"{path}: {why}")


NEW_AT_MOST = 3000      # new songs read at one update; the rest wait for the next


def of_new(paths_formats: list[tuple[str, str]], host_prefix: str = "",
           progress=lambda s: None) -> list[tuple[str, str]]:
    """(path, chip) of songs that are new to the catalog, read from their first bytes.
    `host_prefix`: where the collection's files are on Modland ("HVSC/" for HVSC's)."""
    from concurrent.futures import ThreadPoolExecutor
    wanted = [(p, f) for p, f in paths_formats if says(f, p)][:NEW_AT_MOST]
    if not wanted:
        return []
    progress(f"Reading what {len(wanted):,} new songs play on...")

    def one(song: tuple[str, str]) -> list[tuple[str, str]]:
        path, fmt = song
        try:
            return [(path, chip) for chip in read(fmt, head(host_prefix + path))]
        except OSError:
            return []
    with ThreadPoolExecutor(6) as pool:
        return [row for rows in pool.map(one, wanted) for row in rows]
