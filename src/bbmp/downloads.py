# SPDX-License-Identifier: GPL-3.0-or-later
"""Downloads: what some formats need and the player may fetch but not bring along.

Some replayers work with a file of the program their songs were made with: its
instrument bank, its disk image. Those files are given out by their makers for
everyone to download, under terms that don't allow passing them on as part of
something else. So the player doesn't come with them. It offers to download them from
where their makers keep them, at the first start and under Downloads in Settings, and
it says what each one is and whose.

(Files that are nobody's to give out, such as ROMs, are another matter: see extras.py.)
"""

from __future__ import annotations

import hashlib
import io
import os
import re
import shutil
import struct
import tarfile
import tempfile
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from . import paths
from .download_pins import (AMIGA_PLAYERS, BEEPOLA_DATA, IXS_LIBRARY, IXS_LIBRARY_FILE,
                            SC68_REPLAYS)

# The Modland formats whose songs UADE plays with a player that is downloaded on request:
# those of the census songs refused without "amigaplayers" (7 October 2026; all of each
# format's songs but Jesper Olsen's, whose own replayer plays most).
AMIGA_FORMATS = (
    "AHX", "AM Composer", "AProSys", "Actionamics", "Activision Pro", "Anders Oland",
    "Arpeggiator", "Art And Magic", "Ashley Hogg", "Beathoven Synthesizer", "Ben Daglish",
    "Ben Daglish SID", "Core Design", "CustomMade", "Darius Zendeh", "Dave Lowe",
    "Dave Lowe New", "David Hanney", "David Whittaker", "Delta Music", "Delta Music 2",
    "Delta Packer", "Desire", "Digital Mugician", "Digital Mugician 2", "Digital Mugician 3",
    "Digital Sonix And Chrome", "Digital Sound Studio", "Dirk Bialluch", "Dynamic Synthesizer",
    "EarAche", "Electronic Music System", "Electronic Music System v6", "Fashion Tracker",
    "Follin Player II", "Forgotten Worlds", "Fred Gray", "Future Composer BSI", "Future Player",
    "GlueMon", "Hippel", "Hippel 7V", "Hippel COSO", "Hippel ST", "Hippel ST COSO",
    "Howie Davies", "IFF-SMUS", "InStereo!", "InStereo! 2.0", "Infogrames", "JamCracker",
    "Janko Mrsic-Flogel", "Jason Brooke", "Jason Page", "Jason Page Old", "Jeroen Tel",
    "Jesper Olsen", "Kris Hatlelid", "Leggless Music Editor", "Lionheart", "MCMD",
    "MIDI-Loriciel", "Magnetic Fields Packer", "Mark Cooksey", "Mark Cooksey Old", "Mark II",
    "Maximum Effect", "Mike Davies", "NovoTrade Packer", "OctaMED MMDC", "Paul Robotham",
    "Paul Shields", "Paul Summers", "Peter Verswyvelen", "Pierre Adane Packer", "Powertracker",
    "Professional Sound Artists", "Quadra Composer", "Quartet PSG", "Quartet ST",
    "Richard Joseph", "Riff Raff", "Rob Hubbard", "Rob Hubbard ST", "Ron Klaren", "SCUMM",
    "Sean Connolly", "Sean Conran", "SidMon 2", "Silmarils", "Sonic Arranger", "Sound Images",
    "Sound Master", "Sound Master II v1", "Sound Master II v3", "Sound Programming Language",
    "SoundControl", "SoundFactory", "SoundPlayer", "Special FX", "Special FX ST",
    "Speedy A1 System", "Speedy System", "Steve Barrett", "SunTronic", "SynTracker",
    "Synth Dream", "Synth Pack", "Synthesis", "TFMX", "TFMX ST", "The Musical Enlightenment",
    "Thomas Hermann", "Tomy Tracker", "Unique Development", "Voodoo Supreme Synthesizer",
    "Wally Beben", "YMST", "Zoundmonitor",
)


@dataclass(frozen=True)
class Download:
    id: str
    name: str                   # what it is called
    what: str                   # what it is
    formats: tuple[str, ...]    # the formats that use it
    required: bool              # True: without it no song of these formats plays
    without: str                # what happens without it
    url: str
    size: int                   # of what is downloaded, in bytes
    sha256: str                 # of what is downloaded
    files: tuple[str, ...]      # what is kept of it
    terms: str                  # whose it is, and why the player doesn't come with it
    backend: str = ""           # the helpers that use it (space between), when that isn't its id
    asks: str = ""              # with backend: what the helper says when a song needs it
    songs: str = ""             # the songs that need it, said shortly ("": its formats' songs)


DOWNLOADS = (
    Download("soundclub", "Sound Club's instruments",
             "the instrument bank and the FM waveforms of Sound Club for Windows 2.03",
             ("Sound Club", "Sound Club 2"), False,
             "only the songs that have all their instruments in them play, about a quarter",
             "http://www.bluemoon.ee/history/scwin/scw203.zip", 2900977,
             "b371ac8ea07c8fa80e4727b9c0347a18304c00b1917c4d4b9103d97e1aa70dd0",
             ("sclub.bnk", "scwaves.bin"),
             "BlueMoon Software's, who give the program out for free. It may be distributed "
             "whole and unchanged only, so its files can't be part of the player"),
    Download("cybertracker", "CyberTracker 1.01",
             "the C64 tracker's disk image; the tracker itself is what plays its songs",
             ("Cybertracker C64",), True,
             "Cybertracker's songs can't be played",
             "https://csdb.dk/getinternalfile.php/63969/cybertracker1_01-d64.zip", 79006,
             "cdecc06385ebb5cd1d941889cae8c1ecf33e72965116db652a4e43c50d4040fe",
             ("cybertracker101.d64",),
             "No Name's (CyberBrain, 2001), who released it on the Commodore 64 Scene "
             "Database without saying on what terms"),
    Download("msxdrivers", "MSX drivers",
             "the MSX sound drivers of MuSICA (kinrou5), MPK (1.03 and 1.06) and OPLLDRV "
             "(opx4kss), from libkss's kss-drivers",
             ("MuSICA", "MPK", "OPLLDRV"), True,
             "MuSICA, MPK and OPLLDRV songs can't be played (MGSDRV, MoonBlaster and KSS "
             "songs play without it)",
             "https://codeload.github.com/digital-sound-antiques/kss-drivers/tar.gz/"
             "e8032da38ea8439caca699954fce82b5050bf86f", 49303,
             "533cc994c0b4b85bcecedd98351a981204779833d89d49577e619d3d5685d47e",
             ("kinrou5.bin", "mpk106.bin", "mpk103.bin", "opx4kss.bin"),
             "Keiichi Kuroda's (MuSICA), K-KAZ's (MPK), and Mikasen's and Ring's (OPLLDRV). "
             "They are given out for free, but on no terms that allow passing them on",
             backend="kss", asks="driver ("),
    Download("amigaplayers", "UADE's Amiga players",
             "the Amiga replay routines (Eagleplayers) UADE plays most Amiga formats with, "
             "from UADE's own source on GitLab: all but the 18 whose terms allow passing "
             "them on, which come with the player",
             AMIGA_FORMATS, False,
             "the songs of most Amiga formats can't be played (AHX, TFMX, Sonic Arranger, "
             "the game composers' formats, ...); SoundMon, MusicMaker, Art Of Noise, "
             "Pretracker, FredMon, SidMon 1 and a few more play without it",
             "https://gitlab.com/uade-music-player/uade/-/archive/"
             "8d77cd773e10d33de44f2b6c52201bb54732b701/"
             "uade-8d77cd773e10d33de44f2b6c52201bb54732b701.tar.gz?path=players", 725776,
             "22bbd1b731ffff21fd8639186cd44debb27265b8d7af4a15fb9faaf45a43b73d",
             tuple(AMIGA_PLAYERS),
             "Ripped from games and editors and adapted by Wanted Team, DEFECT, Delirium and "
             "others, or given out by their authors without written terms. UADE carries them "
             "as \"certainly distributable\", but on no terms that allow passing them on",
             backend="uade zoundmonitor jesperolsen stonetracker", asks="Amiga player",
             songs="most Amiga songs"),
    Download("sc68replays", "sc68's replay routines",
             "the Atari ST replay routines that some SC68 files name instead of carrying "
             "their own, from sc68 2.2.1 on SourceForge: all but sc68's own six",
             ("SC68",), False,
             "the SC68 songs that name such a routine can't be played (about a third); "
             "those that carry their own code play",
             "https://downloads.sourceforge.net/project/sc68/sc68/2.2.1/sc68-2.2.1.tar.gz",
             745407, "d7371f0f406dc925debf50f64df1f0700e1d29a8502bb170883fc41cc733265f",
             tuple(f"Replay/{n}.bin" for n in SC68_REPLAYS),
             "Ripped from games, editors and demos (many of them Nicolas Pomar\u00e8de's YMST "
             "wrappers of game drivers) and given to sc68 without written terms",
             backend="sc68", asks="replay routine", songs="many SC68 songs"),
    Download("beepoladata", "Beepola's data",
             "three pieces some of Beepola's engines read: the ZX Spectrum 48K ROM (noise "
             "drums, the ROM Beep engine), "
             "Shiru's Phaser1 drum samples and a table of Savage's player, cut out of the "
             "memory images in Beepola's own source on GitHub",
             ("Beepola",), False,
             "the Beepola songs whose engine reads one of them can't be played",
             "https://codeload.github.com/atsidaev/beepola/tar.gz/"
             "4145966ce0e6d96eeed57b522882915fbad23244", 1130448,
             "f5e2804c0fa5a809b6e01b69da69db01b77477bceb03509ad1c2c3211b56df16",
             tuple(BEEPOLA_DATA),
             "Amstrad's (the ROM), Shiru's (the drum samples, given out with Phaser1's "
             "source without a licence) and Jason C. Brooke's (Savage's player, taken "
             "from his game). Beepola carries them, but on no terms that allow passing "
             "them on in the player",
             backend="beepola", asks="Beepola's data"),
    Download("ixalancesamples", "iXalance's library samples",
             "three short drum sounds that came with Shortcut Software's iXalance player "
             "(a TR-909 open hi-hat and snare, a crash cymbal), from the source of the "
             "earlier iXalance plugin on GitHub",
             ("Ixalance",), False,
             "the 7 iXalance songs that use one of them can't be played; the other 17 play",
             "https://codeload.github.com/RetrovertApp/playback-ixalance/tar.gz/"
             "9b474c80ce1e6f6b77d0290fd51958b3663ba8af", 450764,
             "e80d049337ba8787cc7a049b3b2bd5005bd67cfc21ed06ae05c31aa9d24ea492",
             tuple(IXS_LIBRARY),
             "Shortcut Software's, who gave them out with their player. The plugin's source "
             "carries them, but on no terms that allow passing them on in the player",
             backend="ixalance", asks="iXalance's library samples",
             songs="some iXalance songs"),
    Download("organyabank", "Organya's sound bank",
             "the 100 waveforms and 42 drum sounds Organya songs are played with (Studio "
             "Pixel's Wave100 and OrgMaker's drums, as one file, default.wdb), from "
             "organya.h's repository on GitHub",
             ("Organya", "Organya 2"), False,
             "Organya songs can't be played where the helper doesn't have a bank of its own "
             "(the packages)",
             "https://raw.githubusercontent.com/Strultz/organya.h/"
             "f7a086fc00f21de5e1c410d38d7412627b8d2cf0/default.wdb", 436212,
             "cd538281bbbc110a4ca1b9321b1452b46b39a24048fa760e1e09ca36bf0eec88",
             ("default.wdb",),
             "Studio Pixel's sounds, put into one file by Strultz. organya.h's zlib licence "
             "is for its code; the bank comes with no terms of its own",
             backend="organya", asks="Organya soundbank", songs="Organya songs"),
)


def folder(download: Download | None = None) -> Path:
    """Where downloads are kept; BBMP_DOWNLOADS names another folder."""
    root = Path(paths.env("DOWNLOADS") or paths.DATA_DIR / "downloads").expanduser()
    return root / download.id if download else root


def get(id: str) -> Download:
    return next(d for d in DOWNLOADS if d.id == id)


def installed(download: Download) -> bool:
    return all((folder(download) / f).is_file() for f in download.files)


def advice(backend: str, said: str = "") -> str:
    """What to do when a replayer's download isn't there: "" when it is, or there is none.
    A download only some of a helper's songs need (its backend is set) is advised when
    what the helper said asks for it."""
    d = next((d for d in DOWNLOADS if d.id == backend
              or (backend in d.backend.split() and d.asks and d.asks in said)), None)
    if d is None or installed(d):
        return ""
    return (f"To play {used_for(d)}, the player needs {d.name}. "
            f"Download it in Settings \u2192 Downloads (or: bbmp download {d.id}).")


def used_for(d: Download) -> str:
    """The songs it is for, said shortly: "Sound Club and Sound Club 2 songs"."""
    return d.songs or f"{' and '.join(d.formats)} songs"


def with_advice(said: str, advice: str) -> str:
    """A helper's refusal and the advice, as one message."""
    if not advice:
        return said
    return f"{said.rstrip()}{'' if said.rstrip().endswith(('.', '!', '?')) else '.'} {advice}"


def for_format(fmt: str) -> list[Download]:
    family = fmt.split(" - ", 1)[0]
    return [d for d in DOWNLOADS if fmt in d.formats or family in d.formats]


def missing(fmt: str) -> list[Download]:
    """The downloads a format can't do without and that aren't there."""
    return [d for d in for_format(fmt) if d.required and not installed(d)]


def describe(download: Download) -> str:
    text = f"{download.name}: {download.what} ({download.size / 1e6:.1f} MB to download). "
    if installed(download):
        return text + "It is there."
    return (text + f"It is not there; without it {download.without}. "
            "Downloads in Settings fetches it.")


# ---- fetching ----

def _soundclub(data: bytes, dest: Path) -> None:
    """The package is a list of files (data.idx: 64 bytes each, the name and at 0x34 the
    size) and the files one after the other (data.001, data.002, ...). The bank is one
    of them as it is; the waveforms are a resource of scw.exe."""
    z = zipfile.ZipFile(io.BytesIO(data))
    names = {n.lower(): n for n in z.namelist()}
    idx = z.read(names["data.idx"])
    parts = sorted(n for n in names if n.startswith("data.") and n[5:].isdigit())
    blob = b"".join(z.read(names[n]) for n in parts)
    files, pos = {}, 0
    for i in range(len(idx) // 64):
        entry = idx[i * 64:(i + 1) * 64]
        name = entry[:52].split(b"\0")[0].decode("latin-1").lower()
        (size,) = struct.unpack_from("<I", entry, 0x34)
        files[name] = blob[pos:pos + size]
        pos += size
    bank, waves = files["sclub.bnk"], _resource(files["scw.exe"], 10, 0x71)
    if bank[:3] != b"BNK" or len(waves) != 22 * 4096:
        raise ValueError("the package is not the one that was expected")
    (dest / "sclub.bnk").write_bytes(bank)
    (dest / "scwaves.bin").write_bytes(waves)


def _resource(exe: bytes, type_id: int, name_id: int) -> bytes:
    """A resource of a Windows program, by the numbers of its type and name."""
    (pe,) = struct.unpack_from("<I", exe, 0x3C)
    (sections,) = struct.unpack_from("<H", exe, pe + 6)
    (optional,) = struct.unpack_from("<H", exe, pe + 20)
    at = pe + 24 + optional
    for _ in range(sections):
        _, va, _, start = struct.unpack_from("<IIII", exe, at + 8)
        if exe[at:at + 8].rstrip(b"\0") == b".rsrc":
            def entries(off: int):
                named, numbered = struct.unpack_from("<HH", exe, start + off + 12)
                for k in range(named + numbered):
                    yield struct.unpack_from("<II", exe, start + off + 16 + k * 8)
            for tid, toff in entries(0):
                for nid, noff in entries(toff & 0x7FFFFFFF) if tid == type_id else ():
                    for _, loff in entries(noff & 0x7FFFFFFF) if nid == name_id else ():
                        rva, size = struct.unpack_from("<II", exe, start + loff)
                        return exe[rva - va + start:rva - va + start + size]
        at += 40
    raise ValueError("the program has no such resource")


def _cybertracker(data: bytes, dest: Path) -> None:
    z = zipfile.ZipFile(io.BytesIO(data))
    image = next(n for n in z.namelist() if n.lower().endswith(".d64"))
    (dest / "cybertracker101.d64").write_bytes(z.read(image))


# The drivers are C arrays in kss-drivers (kinrou5.h: "0xFE,0x00,..." after a comment);
# kept as the bytes they are, which bbmp-chip hands to libkss. Their SHA-256 as the
# arrays in libkss's own build (the same commit) have them, so they play the same.
MSX_DRIVERS = {
    "kinrou5": "a888018b8e117de9806bc8c6b8f2d69e7414d9ebb97427c2f8a038e828d21d2b",
    "mpk106": "e6ac92e4635dc078004a897827f62090318b9422ebf0493ef092b22b95ed5484",
    "mpk103": "3ed454f8674d16d8f94cc70df508a7220251e158f37cdc1ae843129d45f2df1d",
    "opx4kss": "0203487cbafbec8d41cc24021ec577ee93acf788f04736d774d1aaca55332a58",
}


def c_array(text: str) -> bytes:
    """The bytes of a C array's initialiser: its 0x.. numbers, comments left out."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    return bytes(int(n, 16) for n in re.findall(r"0[xX]([0-9a-fA-F]{1,2})\b", text))


def _msxdrivers(data: bytes, dest: Path) -> None:
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        headers = {Path(m.name).name: m for m in tar.getmembers() if m.isfile()}
        for name, sha in MSX_DRIVERS.items():
            driver = c_array(tar.extractfile(headers[name + ".h"]).read().decode("latin-1"))
            if hashlib.sha256(driver).hexdigest() != sha:
                raise ValueError(f"{name}.h is not the driver that was expected")
            (dest / f"{name}.bin").write_bytes(driver)


def _pinned(tar: tarfile.TarFile, members: dict[str, tarfile.TarInfo], pins: dict[str, str],
            dest: Path, where=lambda name: name) -> None:
    """Each pinned file out of the archive, checked, kept under `where(name)`."""
    for name, sha in pins.items():
        data = tar.extractfile(members[name]).read()
        if hashlib.sha256(data).hexdigest() != sha:
            raise ValueError(f"{name} is not the file that was expected")
        out = dest / where(name)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)


def _amigaplayers(data: bytes, dest: Path) -> None:
    """UADE's players/ folder at the commit the helpers are built from; kept as it is
    there (ENV/ beside the players: UADE looks for a player's ENV: files in its folder)."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        members = {m.name.split("/players/", 1)[1]: m for m in tar.getmembers()
                   if m.isfile() and "/players/" in m.name}
        _pinned(tar, members, AMIGA_PLAYERS, dest)


def _sc68replays(data: bytes, dest: Path) -> None:
    """sc68's data/Replay, kept as Replay/ (sc68 looks in <folder>/Replay/<name>.bin)."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        members = {Path(m.name).name[:-4]: m for m in tar.getmembers()
                   if m.isfile() and "/data/Replay/" in m.name and m.name.endswith(".bin")}
        _pinned(tar, members, SC68_REPLAYS, dest, lambda name: f"Replay/{name}.bin")


def _beepoladata(data: bytes, dest: Path) -> None:
    """The three pieces, cut out of the engines' memory images in Beepola's source."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        members = {m.name.split("/", 1)[1]: m for m in tar.getmembers()
                   if m.isfile() and "/" in m.name}
        for name, (image, at, size, sha) in BEEPOLA_DATA.items():
            piece = tar.extractfile(members[image]).read()[at:at + size]
            if hashlib.sha256(piece).hexdigest() != sha:
                raise ValueError(f"{image} does not hold the {name} that was expected")
            (dest / name).write_bytes(piece)


def c_bytes(text: str, name: str) -> bytes:
    """The bytes of the C array `name` (its numbers, decimal or 0x..), comments left out."""
    text = re.sub(r"/\*.*?\*/|//[^\n]*", "", text, flags=re.S)
    m = re.search(rf"\b{re.escape(name)}\s*\[[^\]]*\]\s*=\s*\{{([^}}]*)\}}", text)
    if m is None:
        raise ValueError(f"there is no {name}")
    return bytes(int(n, 0) for n in re.findall(r"\b(0[xX][0-9a-fA-F]+|\d+)\b", m.group(1)))


def _organyabank(data: bytes, dest: Path) -> None:
    """The bank is one file, kept as it is (its SHA-256 was checked)."""
    (dest / "default.wdb").write_bytes(data)


def _ixalancesamples(data: bytes, dest: Path) -> None:
    """The three samples, as C arrays in the old iXalance plugin's source."""
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:gz") as tar:
        member = next(m for m in tar.getmembers()
                      if m.isfile() and m.name.split("/", 1)[-1] == IXS_LIBRARY_FILE)
        text = tar.extractfile(member).read().decode("latin-1")
    for name, (array, size, sha) in IXS_LIBRARY.items():
        sample = c_bytes(text, array)
        if len(sample) != size or hashlib.sha256(sample).hexdigest() != sha:
            raise ValueError(f"{array} is not the {name} that was expected")
        (dest / name).write_bytes(sample)


_UNPACK: dict[str, Callable[[bytes, Path], None]] = {
    "soundclub": _soundclub, "cybertracker": _cybertracker, "msxdrivers": _msxdrivers,
    "amigaplayers": _amigaplayers, "sc68replays": _sc68replays, "beepoladata": _beepoladata,
    "ixalancesamples": _ixalancesamples, "organyabank": _organyabank}


def install(download: Download, progress: Callable[[str], None] = lambda s: None) -> None:
    """Download it and keep what is needed of it. Raises OSError when it can't be
    reached, ValueError when what arrives is not what was expected."""
    progress(f"Downloading {download.name}...")
    request = urllib.request.Request(download.url, headers={"User-Agent": "bbmp"})
    with urllib.request.urlopen(request, timeout=60) as r:
        data = r.read(download.size * 4 + 1_000_000)
    if hashlib.sha256(data).hexdigest() != download.sha256:
        raise ValueError(f"what {download.url} gave is not the file that was expected")
    dest = folder(download)
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(tempfile.mkdtemp(prefix=f".{download.id}-", dir=dest.parent))
    try:
        try:
            _UNPACK[download.id](data, tmp)
        except (KeyError, StopIteration, zipfile.BadZipFile, struct.error,
                tarfile.TarError) as e:
            raise ValueError(f"{download.name} could not be unpacked: {e}") from None
        shutil.rmtree(dest, ignore_errors=True)
        tmp.chmod(0o755)
        tmp.rename(dest)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    progress(f"{download.name} is there.")


def remove(download: Download) -> None:
    shutil.rmtree(folder(download), ignore_errors=True)


def env(backend: str) -> dict[str, str]:
    """For the helpers: where the downloads are."""
    out = {}
    if backend == "soundclub" and installed(get("soundclub")):
        out["BBMP_SOUNDCLUB_DATA"] = str(folder(get("soundclub")))
    elif backend == "cybertracker" and installed(get("cybertracker")):
        out["BBMP_CYBERTRACKER_DISK"] = str(folder(get("cybertracker")) / "cybertracker101.d64")
    elif backend == "kss" and installed(get("msxdrivers")):
        out["BBMP_MSX_DRIVERS"] = str(folder(get("msxdrivers")))
    elif backend in get("amigaplayers").backend.split() and installed(get("amigaplayers")):
        out["BBMP_UADE_PLAYERS"] = str(folder(get("amigaplayers")))
    elif backend == "sc68" and installed(get("sc68replays")):
        out["BBMP_SC68_REPLAYS"] = str(folder(get("sc68replays")))
    elif backend == "beepola" and installed(get("beepoladata")):
        out["BBMP_BEEPOLA_DATA"] = str(folder(get("beepoladata")))
    elif backend == "ixalance" and installed(get("ixalancesamples")):
        out["BBMP_IXS_DATA"] = str(folder(get("ixalancesamples")))
    elif backend == "organya" and installed(get("organyabank")):
        out["BBMP_ORGANYA_BANK"] = str(folder(get("organyabank")) / "default.wdb")
    return out
