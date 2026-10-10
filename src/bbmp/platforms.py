# SPDX-License-Identifier: GPL-3.0-or-later
"""The machines music was made for, and the sound chips it plays on: for the views
Platform and Chip, and search's `platform:` and `chip:`.

What a song is comes from its format (platform_table.py: TFMX is the Amiga's and
Paula's, SNDH the Atari ST's and the YM2149's), from the collection where that says
more (Keygen Music is PC music, also in Amiga formats), and later from the song itself
where its file says (a SID tune names the 6581 or the 8580).
"""

from __future__ import annotations

import re

# id -> name, in the order they are listed: computers, then consoles, then the rest
PLATFORMS: dict[str, str] = {
    "amiga": "Amiga",
    "atarist": "Atari ST",
    "c64": "Commodore 64",
    "zx": "ZX Spectrum",
    "cpc": "Amstrad CPC",
    "msx": "MSX",
    "pc": "PC",
    "mac": "Macintosh",
    "archimedes": "Acorn Archimedes",
    "bbc": "BBC Micro",
    "atari8": "Atari 8-bit",
    "atari2600": "Atari 2600",
    "vic20": "VIC-20",
    "plus4": "Commodore 16 and Plus/4",
    "samcoupe": "Sam Coupé",
    "iigs": "Apple IIgs",
    "x68000": "Sharp X68000",
    "pc98": "NEC PC-98 and PC-88",
    "fmtowns": "FM Towns",
    "nes": "NES and Famicom",
    "gb": "Game Boy",
    "gba": "Game Boy Advance",
    "snes": "Super Nintendo",
    "n64": "Nintendo 64",
    "nds": "Nintendo DS",
    "virtualboy": "Virtual Boy",
    "megadrive": "Mega Drive and Genesis",
    "mastersystem": "Master System",
    "gamegear": "Game Gear",
    "sg1000": "SG-1000 and SC-3000",
    "megacd": "Mega CD",
    "32x": "32X",
    "saturn": "Saturn",
    "dreamcast": "Dreamcast",
    "psx": "PlayStation",
    "ps2": "PlayStation 2",
    "pcengine": "PC Engine",
    "wonderswan": "WonderSwan",
    "coleco": "ColecoVision",
    "vectrex": "Vectrex",
    "piece": "P/ECE",
    "neogeo": "Neo Geo",
    "arcade": "Arcade",
}

# id -> name. A family where the members sound alike (the AY-3-8910 and the YM2149,
# the OPL chips); samples played by the processor, as PC trackers do, are no chip.
CHIPS: dict[str, str] = {
    "paula": "Paula (Amiga)",
    "sid": "SID (6581 and 8580)",
    "sid6581": "SID 6581 (the first C64s)",
    "sid8580": "SID 8580 (the later C64s)",
    "ay": "AY-3-8910 and YM2149",
    "ym2612": "YM2612 (OPN2)",
    "ym2151": "YM2151 (OPM)",
    "opn": "YM2203 (OPN)",
    "opna": "YM2608 (OPNA)",
    "opnb": "YM2610 (OPNB)",
    "opl": "OPL, OPL2 and OPL3 (Ad Lib, Sound Blaster)",
    "opl4": "OPL4 (MoonSound)",
    "opll": "YM2413 (OPLL, MSX-MUSIC)",
    "sn76489": "SN76489 (PSG)",
    "nesapu": "NES APU (2A03)",
    "nesexp": "NES expansion chips (VRC6, VRC7, N163, FDS, MMC5, 5B)",
    "gbapu": "Game Boy APU",
    "gbadirect": "Game Boy Advance Direct Sound",
    "spc700": "SPC700 (Super Nintendo)",
    "huc6280": "HuC6280 (PC Engine)",
    "scc": "Konami SCC",
    "pokey": "POKEY (Atari 8-bit)",
    "tia": "TIA (Atari 2600)",
    "saa1099": "SAA1099 (Sam Coupé)",
    "ted": "TED (Commodore 16 and Plus/4)",
    "vic": "VIC (VIC-20)",
    "beeper": "Beeper (ZX Spectrum 48K)",
    "vidc": "VIDC (Archimedes)",
    "es5503": "Ensoniq DOC (Apple IIgs)",
    "rf5c68": "RF5C68 (Mega CD, FM Towns)",
    "segapcm": "Sega PCM",
    "qsound": "Capcom QSound",
    "okim": "OKI ADPCM (MSM6258, MSM6295)",
    "scsp": "SCSP (Saturn)",
    "aica": "AICA (Dreamcast)",
    "spu": "SPU (PlayStation)",
    "spu2": "SPU2 (PlayStation 2)",
    "ndssound": "Nintendo DS sound",
    "vsu": "VSU (Virtual Boy)",
    "wsc": "WonderSwan sound",
    "pcspeaker": "PC speaker",
}

# A collection that says more than the format: keygens run on PCs, whatever tracker
# their music was made with.
COLLECTIONS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    "keygenmusic": (("pc",), ()),
}

KINDS = {"platform": PLATFORMS, "chip": CHIPS}

# Chips that each song says for itself, not its format: the catalog has them song by
# song (its chips table), where the files were read (HVSC's tunes name their SID).
SONG_CHIPS = ("sid6581", "sid8580")


def decided_by_songs(fmt: str, kind: str, id_: str) -> bool:
    """Is it the songs of a format that say whether they are on a platform or chip, one
    by one (song_chips.py), rather than the format for all of them? A Deflemask song is
    the Mega Drive's or the Game Boy's; a VGM file names its chips; a C64 tune its SID.
    Where the catalog has not read a format's songs, the format decides after all."""
    return (id_ in SONG_CHIPS or fmt == "Deflemask"
            or (kind == "chip" and fmt.startswith("Video Game Music"))
            or (fmt == "FamiTracker" and id_ == "nesexp"))


def value(kind: str, id_: str) -> str:
    """How a platform or chip is written in the catalogs' chips table."""
    return f"platform:{id_}" if kind == "platform" else id_

# other names people use
ALIASES: dict[str, dict[str, str]] = {
    "platform": {"genesis": "megadrive", "spectrum": "zx", "speccy": "zx", "zxspectrum": "zx",
                 "st": "atarist", "ste": "atarist", "falcon": "atarist", "commodore": "c64",
                 "famicom": "nes", "gameboy": "gb", "gbc": "gb", "gameboycolor": "gb",
                 "superfamicom": "snes", "sfc": "snes", "ps1": "psx", "psone": "psx",
                 "sms": "mastersystem", "turbografx": "pcengine", "tg16": "pcengine",
                 "pce": "pcengine", "dos": "pc", "windows": "pc", "acorn": "archimedes",
                 "apple": "iigs", "x68k": "x68000", "pc88": "pc98", "c16": "plus4",
                 "plus4": "plus4", "atari800": "atari8", "xl": "atari8", "xe": "atari8",
                 "2600": "atari2600", "vcs": "atari2600", "atarivcs": "atari2600",
                 "coupe": "samcoupe", "n64": "n64", "ds": "nds", "megacd": "megacd",
                 "segacd": "megacd", "segagenesis": "megadrive", "macintosh": "mac",
                 "mvs": "neogeo", "aes": "neogeo"},
    "chip": {"ym2149": "ay", "ay38910": "ay", "psg": "sn76489", "sn": "sn76489",
             "adlib": "opl", "opl2": "opl", "opl3": "opl", "ym3812": "opl", "ymf262": "opl",
             "ym3526": "opl", "y8950": "opl", "ym2413": "opll", "msxmusic": "opll",
             "moonsound": "opl4", "ymf278": "opl4", "opn2": "ym2612", "opm": "ym2151",
             "ym2203": "opn", "ym2608": "opna", "ym2610": "opnb", "2a03": "nesapu",
             "2a07": "nesapu", "vrc6": "nesexp", "vrc7": "nesexp", "n163": "nesexp",
             "fds": "nesexp", "mmc5": "nesexp", "6581": "sid6581", "8580": "sid8580",
             "spc": "spc700", "sdsp": "spc700", "msm6295": "okim", "msm6258": "okim"},
}


def _table() -> dict[str, tuple[tuple[str, ...], tuple[str, ...]]]:
    try:
        from .platform_table import FORMATS
    except ImportError:
        return {}
    return FORMATS


# A collection of one machine's music: its formats are that machine's, whatever else
# they are used for (ZX-Art's Arkos Tracker songs are Spectrum music, Arkos' are mostly
# the CPC's); but for the formats of another machine it has too (its SAM Coupé tunes).
MACHINES: dict[str, str] = {"zxart": "zx"}


def of(fmt: str, collection: str = "") -> tuple[tuple[str, ...], tuple[str, ...]]:
    """(platforms, chips) of a format as a collection has it; empty when not known."""
    if collection in COLLECTIONS:
        return COLLECTIONS[collection]
    if collection in MACHINES:
        found = _table().get(fmt) or _table().get(fmt.split(" - ", 1)[0]) or ((), ())
        if found[0] and found[0][0] != "cpc" and MACHINES[collection] not in found[0]:
            return found            # another machine's music it has too (SAM Coupé)
        return (MACHINES[collection],), found[1] or ("ay",)
    table = _table()
    found = table.get(fmt)
    if found is None and " - " in fmt:
        found = table.get(fmt.split(" - ", 1)[0])
    return found or ((), ())


def matches(fmt: str, collection: str, kind: str, wanted: str) -> bool:
    """Is a format of a collection on the platform, or does it play on the chip?"""
    platforms, chips = of(fmt, collection)
    return wanted in (platforms if kind == "platform" else chips)


def find(kind: str, text: str) -> str | None:
    """The platform or chip the user means: "amiga", "c64", "commodore 64", "atari st",
    "sid", "ym2612", "opl"... by its id, or by the start of its name, spaces and case
    not counting. None when nothing is meant."""
    names = KINDS[kind]
    want = _plain(text)
    if not want:
        return None
    if want in names:
        return want
    if want in ALIASES[kind]:
        return ALIASES[kind][want]
    for id_, name in names.items():
        parts = re.split(r"[(),/]| and ", name)
        if _plain(name).startswith(want) or any(_plain(p).startswith(want) for p in parts):
            return id_
    return None


def _plain(text: str) -> str:
    return "".join(ch for ch in text.lower() if ch.isalnum())


def name(kind: str, id_: str) -> str:
    return KINDS[kind].get(id_, id_)
