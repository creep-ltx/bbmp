# SPDX-License-Identifier: GPL-3.0-or-later
"""Pick the right replayer for a song.

Each song of the big libraries is played by a process of its own (remote.py), which
imports this package first: so it imports little, and the helper programs' side
(pipe.py, with the sandbox and the downloads) only when it is asked for. With all of
it, a replayer's process took 48 ms to start, 37 of them imports; slim, 23 ms
(11 October 2026).
"""

from __future__ import annotations

from pathlib import Path
from typing import Callable

from .base import RATE, Decoder, Details, NeedsDownload, TrackInfo, Unsupported
from ..companions import SUBFORMAT_DIRS
from .formats import BACKEND_BY_FORMAT, UNSUPPORTED_FORMATS

# The formats of collections other than Modland's, which the census (of Modland) does
# not know: ZX-Art's kinds of Spectrum music, tried one song each (29 Sep 2026), its
# CHIPNSFX songs and snapshots (1 Oct 2026), and ASMA's TIATracker songs. Older
# catalogs call CHIPNSFX "Spectrum - CHP" and TIATracker "Theta Music Composer".
BACKEND_BY_FORMAT = {**BACKEND_BY_FORMAT, **{
    f"Spectrum - {kind}": "zxtune" for kind in (
        "Digital Studio", "Digital Music Maker", "Extreme Tracker", "SQ Digital Tracker",
        "Sample Tracker", "Multitrack", "ProDigiTracker", "PSG", "ASC Sound Master 0",
        "General Sound")}, "Spectrum - Furnace": "furnace", "Spectrum - VGM": "vgm",
    "Spectrum - CHIPNSFX": "chipnsfx", "Spectrum - CHP": "chipnsfx",
    # ASMA's Atari 2600 songs, on the tracker's own code (bbmp-tiatracker, 1 Oct 2026)
    "TIATracker": "tiatracker", "Theta Music Composer": "tiatracker",
    # ZXTune finds the tracker module in a snapshot's memory (18 of ZX-Art's 31 .sna);
    # tapes, disks and snapshots of music that is code of its own it can't play
    "Spectrum - Snapshot": "zxtune",
    # ordinary audio files (MP3, Ogg, Opus, FLAC, WAV, AIFF), for collections that have
    # them (Scene.org's music); not the user's own music folder
    "Audio file": "sndfile"}


def _openmpt(path: Path, lengths) -> Decoder:
    from .openmpt import OpenMPT
    return OpenMPT(path)


def _gme(path: Path, lengths) -> Decoder:
    from .gme import GME
    return GME(path)


def _uade(path: Path, lengths) -> Decoder:
    from .uade import UADE
    return UADE(path)


def _sid(path: Path, lengths) -> Decoder:
    from .sid import SID
    return SID(path, lengths)


def _sndfile(path: Path, lengths) -> Decoder:
    from .sndfile import AudioFile
    return AudioFile(path)


def _sunvox(path: Path, lengths) -> Decoder:
    from .sunvox import SunVox
    return SunVox(path)


def _pipe(backend: str) -> Callable[[Path, list], Decoder]:
    def open_(path: Path, lengths) -> Decoder:
        from .pipe import PipeDecoder
        return PipeDecoder(backend, path)
    return open_


def _cym(path: Path, lengths) -> Decoder:
    from .cym import CYM
    return CYM(path)


def _polly(path: Path, lengths) -> Decoder:
    from .polly import Polly
    from ..fetch import Fetcher
    return Polly(path, lengths, _openers()["sidplayfp"], lambda key: Fetcher().get(key))


def _apart(backend: str, local: Callable[[Path, list], Decoder]) -> Callable[[Path, list], Decoder]:
    """The library in a sandboxed process of its own (remote.py), where the sandbox
    works; in the player's process where it doesn't."""
    def open_(path: Path, lengths) -> Decoder:
        from . import remote
        if not remote.use_remote():
            return local(path, lengths)
        if backend == "sidplayfp":
            from .sid import build_shim
            build_shim()            # compiled here: the sandbox has nothing to write to
        return remote.Remote(backend, path, list(lengths))
    return open_


LOCAL_OPENERS: dict[str, Callable[[Path, list], Decoder]] = {
    "openmpt": _openmpt, "uade": _uade, "gme": _gme, "sidplayfp": _sid, "sunvox": _sunvox,
    "sndfile": _sndfile}


def _openers() -> dict[str, Callable[[Path, list], Decoder]]:
    """OPENERS: every replayer by its name, made when first asked for."""
    made = globals().get("OPENERS")
    if made is None:
        from .pipe import HELPERS
        made = globals().setdefault("OPENERS", {
            **{b: _apart(b, f) for b, f in LOCAL_OPENERS.items()},
            **{b: _pipe(b) for b in HELPERS},
            "cym": _cym, "polly": _polly})
    return made


def __getattr__(name: str):
    """OPENERS, and the helper programs' HELPERS, OWN and left_out, when first asked."""
    if name == "OPENERS":
        return _openers()
    if name in ("HELPERS", "OWN", "left_out"):
        from . import pipe
        return getattr(pipe, name)
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")

# Tried in this order when a format has no known backend, or its backend fails.
FALLBACK = ("openmpt", "uade", "gme", "sidplayfp", "zxtune", "vgm", "sc68")

# When one of the player's own replayers refuses a song, it says why: a machine the
# song needs is missing, the file is damaged. Then nothing else is tried, because
# some replayers find something to play in any file, and it would not be the song.
# The exceptions are replayers that really know the same format.
ALSO = {"musicline": ("uade",), "jesperolsen": ("uade",)}

# Formats played by turning the song into another kind of file first (a CYM log into
# VGM, a Polly Tracker module into a C64 tune). Nothing else is tried either: other
# replayers find something to play in the file, and it isn't the song. The census
# doesn't try them on other formats (any file makes some kind of CYM log).
MADE_INTO = ("cym", "polly")

# How long a song plays, and where the player stops one: ending.py, one rule.


def _parent(fmt: str) -> str:
    """'Video Game Music - Nintendo Virtual Boy' -> 'Video Game Music', and
    'HVSC - Musicians', which is how HVSC's own collection files its songs, -> 'HVSC'."""
    top = fmt.split(" - ", 1)[0]
    known = top in SUBFORMAT_DIRS or top in BACKEND_BY_FORMAT or top in UNSUPPORTED_FORMATS
    return top if known and fmt not in BACKEND_BY_FORMAT else fmt


def backend_for(fmt: str) -> str | None:
    """The replayer that plays a format first, if the survey found one."""
    return BACKEND_BY_FORMAT.get(fmt) or BACKEND_BY_FORMAT.get(_parent(fmt))


def is_supported(fmt: str) -> bool:
    """`fmt` is a song's format as shown in the browser (Song.format). Not one whose
    replayer this package was built without (left_out)."""
    from .pipe import left_out
    if (first := backend_for(fmt)) and left_out(first):
        return False
    if fmt in BACKEND_BY_FORMAT:
        return True
    return fmt not in UNSUPPORTED_FORMATS and _parent(fmt) not in UNSUPPORTED_FORMATS


# Files on disk come without Modland's folders, so the extension is all there is to go
# on. bbmp-vgm knows almost every sound chip; Game Music Emu plays the ones it
# doesn't know as silence.
LOCAL_BACKENDS = {".vgm": "vgm", ".vgz": "vgm"}


def open_song(path: Path, fmt: str, lengths: list[float] = (), local: bool = False) -> Decoder:
    """Open a song file with the backend that plays its format (Song.format)."""
    from .pipe import OWN, left_out
    from . import sc68_amiga
    OPENERS = _openers()
    first = BACKEND_BY_FORMAT.get(fmt) or BACKEND_BY_FORMAT.get(_parent(fmt))
    if local:
        first = LOCAL_BACKENDS.get(path.suffix.lower(), first)
    why = left_out(first) if first else None
    if why:                 # this package was built without it: no other replayer plays it
        raise Unsupported(why)
    if first in OWN or first in MADE_INTO:
        order = [first, *ALSO.get(first, ())]
    else:
        order = ([first] if first else []) + [b for b in FALLBACK if b != first]
    errors = []
    # A replayer that would play the song with a part that is downloaded on request and
    # isn't there: the others are still tried (a file under the wrong name may be another
    # replayer's), and if none plays it, what is said is that part's advice.
    needed: NeedsDownload | None = None
    if first == "sc68" and sc68_amiga.amiga_song(path) is not None:
        try:
            return sc68_amiga.open_amiga(path, OPENERS["uade"])
        except NeedsDownload as e:
            needed = needed or e
            errors.append(f"uade: {e}")
        except (Unsupported, OSError) as e:
            errors.append(f"uade: {e}")
    for name in order:
        try:
            return OPENERS[name](path, list(lengths))
        except NeedsDownload as e:
            needed = needed or e
            errors.append(f"{name}: {e}")
            continue
        except (Unsupported, OSError) as e:
            errors.append(f"{name}: {e}")
        if name == first:
            # a file a little damaged on the archive, which the replayer takes mended
            from .mend import open_mended
            mended = open_mended(path, OPENERS[name], lengths)
            if mended is not None:
                return mended
    if needed is not None:
        raise needed
    if first in OWN or first in MADE_INTO:
        raise Unsupported(errors[0].split(": ", 1)[1])     # its own reason, as it says it
    raise Unsupported("no replayer could play this file (" + "; ".join(errors) + ")")


__all__ = ["RATE", "Decoder", "Details", "NeedsDownload", "TrackInfo", "Unsupported", "open_song",
           "is_supported",
           "backend_for",
           ]
