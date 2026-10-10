# SPDX-License-Identifier: GPL-3.0-or-later
"""UADE: the Amiga formats (AHX, TFMX, Future Composer, SoundMon, customs, ...).

UADE emulates an Amiga (68000 + Paula) and runs the original Eagleplayer
replay routines in a helper process. Companion files (smpl.*, instruments)
are loaded by UADE from the song's own folder, which is why the cache mirrors
the Modland folder layout.
"""

from __future__ import annotations

import ctypes as C
import os
import re
import tempfile
from pathlib import Path

from .. import companions, downloads, paths
from .base import (FRAME_BYTES, RATE, Decoder, Details, NeedsDownload, TrackInfo, Unsupported,
                   load_library)

PATH_MAX = 4096


class _Subsongs(C.Structure):
    _fields_ = [("cur", C.c_int), ("min", C.c_int), ("def_", C.c_int), ("max", C.c_int)]


class _Detection(C.Structure):
    _fields_ = [("custom", C.c_int), ("content", C.c_int), ("ext", C.c_char * 16),
                ("ep", C.c_void_p)]


class _SongInfo(C.Structure):
    _fields_ = [("subsongs", _Subsongs), ("detectioninfo", _Detection),
                ("modulebytes", C.c_size_t), ("modulemd5", C.c_char * 33),
                ("duration", C.c_double), ("subsongbytes", C.c_int64), ("songbytes", C.c_int64),
                ("modulefname", C.c_char * PATH_MAX), ("playerfname", C.c_char * PATH_MAX),
                ("formatname", C.c_char * 256), ("modulename", C.c_char * 256),
                ("playername", C.c_char * 256)]


# enum uade_option values from uade.h (UC_NO_OPTION = 0x1000, then counting up)
UC_BASE_DIR = 0x1000 + 1
UC_FREQUENCY = 0x1000 + 10
UC_IGNORE_PLAYER_CHECK = 0x1000 + 14
UC_NO_EP_END = 0x1000 + 21
UC_ONE_SUBSONG = 0x1000 + 23
UC_PANNING_VALUE = 0x1000 + 25
UC_SILENCE_TIMEOUT_VALUE = 0x1000 + 29
UC_SUBSONG_TIMEOUT_VALUE = 0x1000 + 31
UC_UADECORE_FILE = 0x1000 + 33

_lib = None


def available() -> bool:
    try:
        lib()
        return True
    except OSError:
        return False


def lib():
    global _lib
    if _lib is None:
        L = load_library("uade", paths=(paths.UADE_PREFIX / f"lib/libuade{paths.SO}",))
        L.uade_new_config.restype = C.c_void_p
        L.uade_config_set_option.argtypes = [C.c_void_p, C.c_int, C.c_char_p]
        L.uade_new_state.restype = C.c_void_p
        L.uade_new_state.argtypes = [C.c_void_p]
        L.uade_play.argtypes = [C.c_char_p, C.c_int, C.c_void_p]
        L.uade_read.restype = C.c_ssize_t
        L.uade_read.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p]
        L.uade_get_song_info.restype = C.POINTER(_SongInfo)
        L.uade_get_song_info.argtypes = [C.c_void_p]
        L.uade_stop.argtypes = [C.c_void_p]
        L.uade_cleanup_state.argtypes = [C.c_void_p]
        L.uade_get_sampling_rate.argtypes = [C.c_void_p]
        L.uade_song_info.argtypes = [C.c_char_p, C.c_size_t, C.c_char_p, C.c_int]
        _lib = L
    return _lib


def _s(b: bytes) -> str:
    return b.decode("latin-1").strip()


class UADE(Decoder):
    backend = "uade"

    def __init__(self, path: Path):
        self._path = os.fsencode(path)
        self._state = None
        self._state = _new_state()
        self._no_end = False
        self._buf = C.create_string_buffer(0)
        self._playing = False
        self._tmp: tempfile.TemporaryDirectory | None = None
        suffix = path.suffix.lower()
        fitted = (_ymst_folder(path) if suffix == ".ymst"
                  else _infogrames_folder(path) if suffix == ".dum"
                  else _smus_folder(path) if suffix == ".smus"
                  else _relabelled_copy(path) or _unfilled_copy(path))
        if fitted is not None:
            self._tmp, self._path = fitted[0], os.fsencode(fitted[1])
        try:
            self._start(-1)
        except NeedsDownload:
            self.close()
            raise
        except Unsupported:
            for renamed in _renamed_copies(path):
                if self._tmp is not None:
                    self._tmp.cleanup()     # the last try's folder, which didn't play either
                self._tmp, self._path = renamed[0], os.fsencode(renamed[1])
                try:
                    self._start(-1)
                    break
                except NeedsDownload:
                    self.close()
                    raise
                except Unsupported:
                    continue
            else:
                if not self._forced(path):
                    self.close()
                    raise

    def _forced(self, path: Path) -> bool:
        """The last try: the song played by the player its name gives, unchecked."""
        if path.name.split(".", 1)[0].lower() not in _FORCED:
            return False
        if self._tmp is not None:
            self._tmp.cleanup()
            self._tmp = None
        lib().uade_cleanup_state(self._state)
        self._state = None
        self._state = _new_state(forced=True)
        self._path = os.fsencode(path)
        try:
            self._start(-1)
        except NeedsDownload:
            self.close()
            raise
        except Unsupported:
            return False
        return True

    def _ended_early(self) -> bool:
        """DeltaMusic 2's replayer says some songs have ended while their music goes on
        (Sanxion's loader music after 30 ms, Zaxxon after 7 s, 12 of Modland's 98): its
        channels loop each on their own. A song said to end in its first EARLY_END
        seconds plays on from there with the replayer's word left unheard, as a song
        that never ends (the player stops it with a fade, or its silence does)."""
        if (self._no_end or self._pos >= EARLY_END * RATE
                or self.info.extra.get("player") not in _NO_END_PLAYERS):
            return False
        at, info = self._pos, self.info
        L = lib()
        if self._playing:
            L.uade_stop(self._state)
            self._playing = False
        L.uade_cleanup_state(self._state)
        self._state = None
        self._state = _new_state(no_end=True)
        self._no_end = True
        try:
            self._start(self._min + info.subsong)
        except Unsupported:
            return False
        self.info, self.never_ends = info, True
        scratch = C.create_string_buffer(RATE // 10 * FRAME_BYTES)
        while self._pos < at:
            n = L.uade_read(scratch, min(len(scratch), (at - self._pos) * FRAME_BYTES), self._state)
            if n <= 0:
                return False
            self._pos += n // FRAME_BYTES
        return True

    def _start(self, subsong: int) -> None:
        L = lib()
        if self._playing:
            L.uade_stop(self._state)
            self._playing = False
        r = L.uade_play(self._path, subsong, self._state)
        if r <= 0:
            _refuse_missing_player(L.uade_get_song_info(self._state).contents)
            raise Unsupported("UADE doesn't recognise this file")
        self._playing = True
        self._pos = 0
        si = L.uade_get_song_info(self._state).contents
        sub = si.subsongs
        self.info = TrackInfo(
            title=_s(si.modulename), format=_s(si.formatname) or _s(si.playername),
            duration=si.duration or None,
            subsongs=max(1, sub.max - sub.min + 1), subsong=sub.cur - sub.min,
            extra={"player": _s(si.playername)})
        self._min = sub.min

    def select_subsong(self, n: int) -> None:
        self._start(self._min + n)

    def seek(self, seconds: float) -> bool:
        """The emulated Amiga runs at full speed to the position (backwards: from the start).

        UADE has a seek call of its own, but it doesn't land where it is asked to.
        """
        if not self._playing:
            return False
        target = int(max(0.0, seconds) * RATE)
        if target < self._pos:
            try:
                self._start(self._min + self.info.subsong)
            except Unsupported:
                return False
        scratch = C.create_string_buffer(65536)
        while self._pos < target:
            want = min(len(scratch), (target - self._pos) * FRAME_BYTES)
            n = lib().uade_read(scratch, want, self._state)
            if n <= 0:
                break        # the song ends before that position
            self._pos += n // FRAME_BYTES
        return True

    def details(self) -> Details:
        d = Details()
        d.fact("Title", self.info.title)
        d.fact("Format", self.info.format)
        d.fact("Amiga replayer", self.info.extra.get("player"))
        buf = C.create_string_buffer(16384)
        if lib().uade_song_info(buf, len(buf), self._path, 0) == 0:   # UADE_MODULE_INFO
            lines = buf.value.decode("latin-1").splitlines()
            skip = ("UADE2 MODINFO", "File name:", "File length:", "File prefix:")
            d.text("Module information", [ln for ln in lines if not ln.startswith(skip)])
        return d

    def read(self, frames: int) -> bytes:
        if not self._playing:
            return b""
        if len(self._buf) < frames * FRAME_BYTES:
            self._buf = C.create_string_buffer(frames * FRAME_BYTES)
        n = lib().uade_read(self._buf, frames * FRAME_BYTES, self._state)
        if n <= 0 and self._ended_early():
            n = lib().uade_read(self._buf, frames * FRAME_BYTES, self._state)
        if n <= 0:
            return b""
        self._pos += n // FRAME_BYTES
        return self._buf.raw[:n]

    def close(self) -> None:
        if self._state:
            lib().uade_cleanup_state(self._state)
            self._state = None
            self._playing = False
        if self._tmp is not None:
            self._tmp.cleanup()
            self._tmp = None


def _refuse_missing_player(si: _SongInfo) -> None:
    """A song UADE took for a player that isn't there (the players whose terms don't
    allow passing them on are downloaded on request): say which, and what to do. The
    patched libuade (tools/build-uade.sh) names the player it couldn't load."""
    wanted = os.fsdecode(si.playerfname)
    if not wanted or os.path.exists(wanted):
        return
    said = f"UADE's Amiga player {os.path.basename(wanted)} is not there"
    raise NeedsDownload(downloads.with_advice(said, downloads.advice("uade", "Amiga player")))


def _new_state(no_end: bool = False, forced: bool = False):
    L = lib()
    # where the players downloaded on request are (libuade looks there when a player
    # isn't in its own folder)
    extra = downloads.env("uade").get("BBMP_UADE_PLAYERS")
    if extra:
        os.environ["BBMP_UADE_PLAYERS"] = extra
    else:
        os.environ.pop("BBMP_UADE_PLAYERS", None)
    cfg = L.uade_new_config()
    # Panning 0.4 mixes some of each Amiga channel into the other side
    # (0 = hard Amiga stereo, 1 = mono), like openmpt's stereo separation.
    # its data and core where UADE_PREFIX is, not where it was built (a package)
    for opt, val in ((UC_BASE_DIR, str(paths.UADE_PREFIX / "share/uade")),
                     (UC_UADECORE_FILE, str(paths.UADE_PREFIX / "lib/uade/uadecore")),
                     (UC_FREQUENCY, str(RATE)), (UC_ONE_SUBSONG, None),
                     (UC_PANNING_VALUE, "0.4"),
                     (UC_SUBSONG_TIMEOUT_VALUE, "600"), (UC_SILENCE_TIMEOUT_VALUE, "10")):
        L.uade_config_set_option(cfg, opt, val.encode() if val else None)
    if no_end:
        L.uade_config_set_option(cfg, UC_NO_EP_END, None)
    if forced:
        L.uade_config_set_option(cfg, UC_IGNORE_PLAYER_CHECK, None)
    state = L.uade_new_state(cfg)
    # libuade copies the config into the state; the malloc'd config is small and
    # there is no uade_free_config in the public API.
    if not state:
        raise Unsupported("UADE failed to start")
    return state


# Replayers (UADE's names) whose word that a song has ended is not taken when it
# comes within the song's first EARLY_END seconds.
_NO_END_PLAYERS = {"DeltaMusic 2.0"}
EARLY_END = 60

# Name prefixes of players that play songs their check turns away: Ashley Hogg's
# player doesn't know the replay code of Stuntman Seymour (one instruction more at its
# start) and plays its nine songs all the same.
_FORCED = {"ash"}


# Eagleplayers find songs by a name prefix ("qts.song"), while Modland names most
# files with an extension ("song.4v"). Songs UADE doesn't recognise get another
# try under the prefixed name, in a temporary folder of symlinks.
# extension -> (player prefix, {companion extension: companion prefix})
_RENAMES = {
    "4v": ("qts", {"set": "smp"}),   # Quartet ST
    "soc": ("hst", {}),              # Jochen Hippel ST COSO
    "sog": ("hst", {}),              # Jochen Hippel ST: the songs without replay code
}
# and after that under these: one "David Whittaker" song is Jason Brooke's format
_ALSO_AS = {"dw": "jcb"}


_YM_REPLAY = re.compile(rb"[Yy][Mm]\.([\x21-\x7e][\x20-\x7e]{0,40}?)[ _]?[Rr][Ee][Pp][Ll][Aa][Yy]")


def _replay_key(name: str) -> str:
    """'YM.Toki_replay', 'ym.toki_replay' and 'toki - replay.ymst' are all 'toki'."""
    low = name.lower().lstrip("'")
    low = low.removesuffix(".ymst").removeprefix("ym.")
    low = low.replace("replay", "")
    return re.sub(r"[^a-z0-9]", "", low)


def _ymst_folder(path: Path) -> tuple[tempfile.TemporaryDirectory, Path] | None:
    """A YMST song runs on a replay routine it names inside ("YM.Toki_replay"). Put
    the song in a folder of links with the routine under that name. The routines are
    anywhere below the format's folder, under Modland's names."""
    try:
        wanted = _YM_REPLAY.search(path.read_bytes()[:4096])
    except OSError:
        return None
    if wanted is None:
        return None
    name = wanted.group(0).decode("latin-1")
    root = next((p for p in path.parents if p.name == "YMST"), path.parent)
    key = _replay_key(name)
    found = [f for f in root.rglob("*") if "replay" in f.name.lower() and f.is_file()
             and _replay_key(f.name) == key]
    if not found:
        return None
    tmp = tempfile.TemporaryDirectory(prefix="modland-uade-")
    folder = Path(tmp.name)
    (folder / path.name).symlink_to(path)
    (folder / name).symlink_to(found[0])
    return tmp, folder / path.name


def _infogrames_folder(path: Path) -> tuple[tempfile.TemporaryDirectory, Path] | None:
    """An Infogrames song "bob4e.dum" without a "bob4e.ins" plays with the instruments
    whose name begins its own ("bob4.ins"): give it those under its name."""
    stem = path.name.rsplit(".", 1)[0]
    if (path.parent / f"{stem}.ins").exists():
        return None
    siblings = [f.name for f in path.parent.iterdir()]
    own = next((n for n in siblings if n.lower() == f"{stem}.ins".lower()), None)
    bank = own or companions.prefix_bank(path.name, siblings, "ins")
    if bank is None:
        return None
    tmp = tempfile.TemporaryDirectory(prefix="modland-uade-")
    folder = Path(tmp.name)
    (folder / path.name).symlink_to(path)
    (folder / f"{stem}.ins").symlink_to(path.parent / bank)
    return tmp, folder / path.name


def _relabelled_copy(path: Path) -> tuple[tempfile.TemporaryDirectory, Path] | None:
    """Two formats are a known one under another label, which UADE doesn't take:
    SoundMon 1 ("BPSM" at 26) is SoundMon 2's layout without synth tables, so "V.2"
    and a table count of 0; Digital Mugician 3 (" MUGICIAN3/SOFTEYES 1990", Moon
    Child's title theme) is Mugician 2's layout. A copy with the label changed."""
    try:
        with open(path, "rb") as f:
            head = f.read(30)
    except OSError:
        return None
    if head[26:30] == b"BPSM":
        at, label = 26, b"V.2\0"
    elif head.startswith(b" MUGICIAN3/SOFTEYES"):
        at, label = 9, b"2"
    else:
        return None
    data = bytearray(path.read_bytes())
    data[at:at + len(label)] = label
    tmp = tempfile.TemporaryDirectory(prefix="modland-uade-")
    out = Path(tmp.name) / path.name
    out.write_bytes(data)
    return tmp, out


def _linked_folder(path: Path) -> tuple[tempfile.TemporaryDirectory, Path]:
    """A temporary folder with links to everything next to the song but the song."""
    path = path.absolute()
    tmp = tempfile.TemporaryDirectory(prefix="modland-uade-")
    folder = Path(tmp.name)
    for f in path.parent.iterdir():
        if f.name != path.name:
            (folder / f.name).symlink_to(f)
    return tmp, folder / path.name


def _unfilled_copy(path: Path) -> tuple[tempfile.TemporaryDirectory, Path] | None:
    """Seven Maximum Effect songs on Modland ("MAX.AlloAllo level1", "MAX.DinosaurDetective
    intro") have a fill byte where their tables have zeros: "---\x0f---\x14" for
    00 00 00 0f 00 00 00 14. The fill stands before a byte other than zero, the song's
    own bytes of that value before a zero; a copy with the fill made zero plays."""
    if not path.name.lower().startswith("max."):
        return None
    try:
        with open(path, "rb") as f:
            head = f.read(8)
    except OSError:
        return None
    if len(head) < 8 or head[3] != 0x0F or head[7] != 0x14:
        return None
    fill = head[0] or head[4]
    filled = [part for part in (head[0:3], head[4:7]) if part != bytes(3)]
    if not fill or not filled or any(part != bytes([fill]) * 3 for part in filled):
        return None
    data = bytearray(path.read_bytes())
    for i in range(len(data)):
        if data[i] == fill and (i + 1 == len(data) or data[i + 1] != 0):
            data[i] = 0
    tmp, song = _linked_folder(path)
    song.write_bytes(data)
    return tmp, song


def _smus_instruments(data: bytes) -> list[str]:
    """The instrument names an IFF-SMUS song asks for (its INS1 chunks)."""
    names, i = [], 12
    if data[:4] != b"FORM" or data[8:12] != b"SMUS":
        return names
    while i + 8 <= len(data):
        size = int.from_bytes(data[i + 4:i + 8], "big")
        if data[i:i + 4] == b"INS1" and size > 4:
            names.append(data[i + 12:i + 8 + size].split(b"\0")[0].decode("latin-1"))
        i += 8 + size + (size & 1)
    return names


# A sampled instrument of Sonix ("SampledSound", its sample's name at 0x44, the
# envelope full on): what stands in for an .instr Modland lacks.
_SAMPLED = (b"SampledSound".ljust(0x20, b"\0") + bytes(4) + b"\0" * 0x20
            + bytes(0x20) + bytes(4) + b"\0\xff" * 6 + b"\0\x98" * 3 + bytes(6))


def _stand_in(name: str) -> bytes:
    data = bytearray(_SAMPLED)
    data[0x44:0x44 + 20] = name.encode("latin-1")[:19].ljust(20, b"\0")
    return bytes(data)


def _smus_folder(path: Path) -> tuple[tempfile.TemporaryDirectory, Path] | None:
    """Sonix plays an IFF-SMUS song only when every instrument it names is in its
    Instruments folder. Some folders on Modland lack one (wildman's "Bpart"), or have
    an instrument's sample without its .instr ("bass.ss" of Gilsons Green Grape). Then
    the song gets a folder where the instrument is made of its sample, or else is a
    silent one: the song plays without that part rather than not at all."""
    path = path.absolute()
    try:
        wanted = _smus_instruments(path.read_bytes())
    except OSError:
        return None
    dirs = [d for d in path.parent.iterdir() if d.is_dir() and d.name.lower() in companions.DIRS]
    have = {f.name.lower(): f for d in dirs for f in d.iterdir()}
    missing = [n for n in wanted if n and "/" not in n and f"{n.lower()}.instr" not in have]
    samples = sorted(f for n, f in have.items() if n.endswith(".ss"))
    if not missing or not samples:
        return None
    tmp, song = _linked_folder(path)
    folder = song.parent
    inst = folder / (dirs[0].name if dirs else "Instruments")
    if dirs:
        (folder / dirs[0].name).unlink()
    inst.mkdir()
    for d in dirs:
        for f in d.iterdir():
            if not (inst / f.name).exists():
                (inst / f.name).symlink_to(f)
    song.symlink_to(path)
    for n in missing:
        (inst / f"{n}.instr").write_bytes(_stand_in(n))
        if f"{n.lower()}.ss" not in have:
            # silence: another sample's header (its lengths and octaves), then zeros
            silent = samples[0].read_bytes()
            (inst / f"{n}.ss").write_bytes(silent[:0x3E] + bytes(len(silent) - 0x3E))
    return tmp, song


def _renamed_copies(path: Path):
    """The song under each name it is tried as, one after the other."""
    ext = path.name.rpartition(".")[2].lower()
    for prefix in ((None, _ALSO_AS[ext]) if ext in _ALSO_AS else (None,)):
        made = _renamed_copy(path, prefix)
        if made is not None:
            yield made


def _renamed_copy(path: Path, prefix: str | None = None
                  ) -> tuple[tempfile.TemporaryDirectory, Path] | None:
    stem, dot, ext = path.name.rpartition(".")
    if not dot or not stem or len(ext) > 6:
        return None
    usual, companions = _RENAMES.get(ext.lower(), (ext.lower(), {}))
    prefix = prefix or usual
    tmp = tempfile.TemporaryDirectory(prefix="modland-uade-")
    folder = Path(tmp.name)
    siblings = list(path.parent.iterdir())
    own = {fext.lower() for f in siblings for fstem, _, fext in [f.name.rpartition(".")]
           if fstem.lower() == stem.lower()}
    for f in siblings:
        fstem, _, fext = f.name.rpartition(".")
        if f == path:
            name = f"{prefix}.{stem}"
        elif fstem.lower() == stem.lower() and fext.lower() in companions:
            name = f"{companions[fext.lower()]}.{stem}"
        elif (fext.lower() in companions and fext.lower() not in own
              and sum(1 for g in siblings if g.name.lower().endswith("." + fext.lower())) == 1):
            # the song has no file of its own of this kind; the folder has one for all
            name = f"{companions[fext.lower()]}.{stem}"
        else:
            name = f.name   # instrument folders etc. stay reachable under their own names
        try:
            (folder / name).symlink_to(f)
        except FileExistsError:
            pass
    return tmp, folder / f"{prefix}.{stem}"
