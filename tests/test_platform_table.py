# SPDX-License-Identifier: GPL-3.0-or-later
"""The table of what platform and sound chips each format is (platform_table.py)."""

import re
from pathlib import Path

from bbmp.platform_table import FORMATS
from bbmp.platforms import CHIPS, PLATFORMS

SOURCE = Path(__file__).resolve().parents[1] / "src" / "bbmp" / "platform_table.py"


def lookup(fmt: str):
    """A format, or the family before its " - " (as the views look it up)."""
    if fmt in FORMATS:
        return FORMATS[fmt]
    return FORMATS.get(fmt.split(" - ", 1)[0]) if " - " in fmt else None


def test_every_entry_is_well_formed_and_uses_known_ids():
    for fmt, entry in FORMATS.items():
        assert isinstance(entry, tuple) and len(entry) == 2, fmt
        platforms, chips = entry
        assert isinstance(platforms, tuple) and isinstance(chips, tuple), fmt
        assert platforms, f"{fmt} has no platform"
        assert all(p in PLATFORMS for p in platforms), (fmt, platforms)
        assert all(c in CHIPS for c in chips), (fmt, chips)
        assert len(set(platforms)) == len(platforms) and len(set(chips)) == len(chips), fmt


def test_sorted_by_name():
    names = list(FORMATS)
    assert names == sorted(names, key=str.casefold)


def test_every_line_says_how_sure_it_is():
    text = SOURCE.read_text()
    body = text[text.index("FORMATS"):]
    marks = re.findall(r"  # (?:sure\b|checked: |guess: )", body)
    assert len(marks) == len(FORMATS)


def test_families_are_there_as_fallbacks():
    for family in ("Ad Lib", "Spectrum", "Video Game Music", "HVSC"):
        assert family in FORMATS
    assert lookup("HVSC - Musicians") == (("c64",), ("sid",))
    assert lookup("Ad Lib - Something New") == (("pc",), ("opl",))
    assert lookup("Spectrum - Something New") == (("zx",), ("ay",))
    assert lookup("Nothing Like It") is None


def test_samples_of_what_formats_are():
    expected = {
        "Protracker": (("amiga",), ("paula",)),
        "TFMX": (("amiga",), ("paula",)),
        "AHX": (("amiga",), ("paula",)),
        "Hippel ST": (("atarist",), ("ay",)),
        "Rob Hubbard ST": (("atarist",), ("ay",)),
        "SNDH": (("atarist",), ("ay",)),
        "Fasttracker 2": (("pc",), ()),
        "Impulsetracker": (("pc",), ()),
        "Organya": (("pc",), ()),
        "Ad Lib - HSC AdLib Composer": (("pc",), ("opl",)),
        "Spectrum - Vortex Tracker II": (("zx",), ("ay",)),
        "Beepola": (("zx",), ("beeper",)),
        "GoatTracker 2": (("c64",), ("sid",)),
        "HVSC": (("c64",), ("sid",)),
        "Nintendo Sound Format": (("nes",), ("nesapu", "nesexp")),
        "Gameboy Sound System": (("gb",), ("gbapu",)),
        "Nintendo SPC": (("snes",), ("spc700",)),
        "HES": (("pcengine",), ("huc6280",)),
        "MDX": (("x68000",), ("ym2151", "okim")),
        "Capcom Q-Sound Format": (("arcade",), ("qsound",)),
        "Video Game Music - Sega Megadrive": (("megadrive",), ("ym2612", "sn76489")),
        "Vic-Tracker": (("vic20",), ("vic",)),
        "SoundSmith": (("iigs",), ("es5503",)),
    }
    for fmt, want in expected.items():
        assert lookup(fmt) == want, fmt


def test_formats_on_more_than_one_machine():
    assert lookup("YM")[0][0] == "atarist" and {"cpc", "zx"} <= set(lookup("YM")[0])
    assert set(lookup("SC68")[0]) == {"atarist", "amiga"}
    assert {"msx", "mastersystem", "gamegear"} <= set(lookup("KSS")[0])
    assert "megadrive" in lookup("Deflemask")[0] and "nes" in lookup("Deflemask")[0]
    assert lookup("Ben Daglish SID") == (("amiga",), ("paula",))   # the Amiga port of his C64 routine
    assert lookup("Gameboy Sound Format")[0] == ("gba",)            # GSF is the Game Boy Advance's


def test_famitracker_songs_say_their_expansion_chips():
    from bbmp import song_chips

    def module(expansion: int, params_version: int = 6) -> bytes:
        block = b"PARAMS".ljust(16, b"\0") + params_version.to_bytes(4, "little")
        data = bytes([expansion]) + bytes(20)
        return (b"FamiTracker Module" + (0x440).to_bytes(4, "little")
                + block + len(data).to_bytes(4, "little") + data)
    assert song_chips.ftm(module(0)) == ("nesapu",)
    assert song_chips.ftm(module(1)) == ("nesapu", "nesexp")      # VRC6
    assert song_chips.ftm(module(0x10)) == ("nesapu", "nesexp")   # N163
    assert song_chips.ftm(module(1, params_version=1)) == ("nesapu",)
    assert song_chips.ftm(b"FTMN" + bytes(100)) == ()             # Face The Music
    assert song_chips.read("FamiTracker", module(4)) == ("nesapu", "nesexp")
