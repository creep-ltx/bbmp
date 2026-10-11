# SPDX-License-Identifier: GPL-3.0-or-later
"""Shared test fixtures: a tiny generated module and a catalog that points at it."""

from __future__ import annotations

import os
import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from bbmp import collection as collections_, index as index_mod  # noqa: E402
from bbmp.collection import Collection, Entry, Listing, Transport, hvsc, modland  # noqa: E402
from bbmp.fetch import Fetcher  # noqa: E402


def make_mod(rows_with_notes: int = 64) -> bytes:
    """A valid 4-channel ProTracker module: one pattern, a looping square wave on C-3."""
    title = b"test beep".ljust(20, b"\0")
    wave = bytes([0x40] * 16 + [0xC0] * 16)
    samples = struct.pack(">22sHBBHH", b"square", len(wave) // 2, 0, 64, 0, len(wave) // 2)
    samples += b"\0" * 30 * 30
    orders = bytes([1, 127]) + bytes(128)
    pattern = bytearray(64 * 4 * 4)
    period = 214  # C-3
    for row in range(0, min(64, rows_with_notes), 8):
        o = row * 16
        pattern[o:o + 4] = bytes([(1 & 0xF0) | (period >> 8), period & 0xFF, (1 & 0x0F) << 4, 0])
    return title + samples + orders + b"M.K." + bytes(pattern) + wave


SONGS = [
    "Protracker/Tester/beep.mod",
    "Protracker/Tester/beep two.mod",
    "Protracker/Other Guy/coop-Tester/duet.mod",
    "Fasttracker 2/Tester/not really an xm.xm",
    "TFMX/Tester/mdat.level1",
    "TFMX/Tester/smpl.level1",
    "TFMX/Tester/mdat.level2",
    "TFMX/Tester/smpl.level2",
    "Richard Joseph/Tester/game.sng",
    "Richard Joseph/Tester/game.ins",
    "IFF-SMUS/Tester/Album/song.smus",
    "IFF-SMUS/Tester/Album/Instruments/bass.instr",
    "HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid",
    "HVSC/GAMES/A-F/Bionic.sid",
    "Video Game Music/Sega Megadrive/Yuzo Koshiro/Streets of Rage/01 title.vgz",
    "Playstation Sound Format/Someone/Game/01.minipsf",
    "Playstation Sound Format/Someone/Game/game.psflib",
    "Renoise Old/Someone/opening.rns",
    "MDX/Someone/Game/track1.mdx",
    "MDX/Someone/Game/drums.pdx",
    "Ad Lib/Visual Composer/Someone/song.rol",
    "Ad Lib/Visual Composer/Someone/standard.bnk",
    "SGC/Someone/Game/Game.sgc",
    "SGC/Someone/Game/01 Title Screen.m3u",
    "SGC/Someone/Game/02 Stage 1.m3u",
    "SGC/Someone/Other Game [SMS].sgc",
    "Protracker/Tester/List/a list.m3u",
    "SoundSmith/Someone/Breathless",
    "SoundSmith/Someone/Breathless.W",
    "SoundSmith/Someone/FTA.Song (stripped)",
    "SoundSmith/Someone/FTA.Song (stripped).W",
    "SoundSmith/Someone/Lonely.W",             # a bank without its song stays a file of its own
    "FAC SoundTracker/Someone/Disk/First.MUS",
    "FAC SoundTracker/Someone/Disk/Second.MUS",
    "FAC SoundTracker/Someone/Disk/DRUMS1.SM1",
    "FAC SoundTracker/Someone/Disk/DRUMS1.SM2",
    "FAC SoundTracker/Someone/Disk/First.SM1",
    "Ad Lib/Ken's AdLib Music/Someone/song.ksm",
    "Ad Lib/Ken's AdLib Music/Someone/insts.dat",
    "Ken's Digital Music/Someone/song.kdm",
    "Ken's Digital Music/Someone/waves.kwv",
    "Zoundmonitor/Someone/tune.sng",
    "Zoundmonitor/Samples/perco",
    "Zoundmonitor/Samples/tom4",
    "MaxTrax/Someone/game (intro).mxtx",
    "MaxTrax/Someone/game (level 1).mxtx",
    "PokeyNoise/Someone/pn.tetris",
    "PokeyNoise/Someone/pn.tetris.info",
    "Synth Pack/Someone/Game/title.osp",
    "Synth Pack/Someone/Game/smp.set",
    "Shroom/Someone/tune.sho",
]

# how many of them are songs and not files that belong to one
MODLAND_COUNT = sum(1 for e in modland.read_list(
    "\n".join(f"{i:032x} {p}" for i, p in enumerate(SONGS)).encode()).entries if not e.hidden)
HVSC_COUNT = sum(1 for p in SONGS if p.startswith("HVSC/"))

SONGLENGTHS = b"""[Database]
; /MUSICIANS/H/Hubbard_Rob/Commando.sid
6d019ecba831a9f853675aac29a61c10=3:55.594 1:01.288 0:06
"""


@pytest.fixture(scope="session")
def mod_bytes() -> bytes:
    return make_mod()


@pytest.fixture
def catalog(tmp_path) -> index_mod.Index:
    db = tmp_path / "index.db"
    index_mod.build({"modland": modland_listing()}, db)
    return index_mod.Index(db)


RAW_LIST = "\n".join(f"{i:032x} {p}" for i, p in enumerate(SONGS)).encode()


# every file is 1000 bytes and ten more than the one before it
SIZES = "\n".join(f"{1000 + 10 * i}\t{p}" for i, p in enumerate(SONGS)).encode()
CHANGES = [("2026-09-27", "added", "Protracker/Tester/beep two.mod"),
           ("2026-09-27", "updated", "TFMX/Tester/mdat.level1"),
           ("2026-09-27", "added", "TFMX/Tester/smpl.level1"),          # belongs to a song
           ("2026-09-20", "added", "Protracker/Tester/beep.mod"),
           ("2026-09-20", "added", "Protracker/Gone/removed since.mod"),
           ("2026-09-20", "added", "HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid")]
DATED = 1790000000


def modland_listing() -> Listing:
    return modland.read_list(RAW_LIST, SONGLENGTHS, SIZES, CHANGES, DATED)


@pytest.fixture
def hosted(tmp_path) -> index_mod.Index:
    """A catalog of Modland and HVSC, which Modland hosts."""
    db = tmp_path / "index.db"
    index_mod.build({"modland": modland_listing(),
                     "hvsc": hvsc.read_list(RAW_LIST, SIZES, CHANGES, DATED)}, db)
    return index_mod.Index(db)


@pytest.fixture(autouse=True)
def no_downloads(monkeypatch, tmp_path):
    """Modland's file list is the tests' own, whoever asks for it, and the extras
    folder is an empty one: what is on this computer is not the tests' business."""
    monkeypatch.setattr(modland, "file_list", lambda progress=None: RAW_LIST)
    # catalogs are built here, where the tests stand in for the servers, not in a
    # process of their own
    from bbmp import catalogs
    monkeypatch.setattr(catalogs, "APART", False)
    # the tests know Modland and HVSC (and Chipland, their own); a test that wants
    # Keygen Music, ASMA, ZX-Art or SNDH, whose listings would ask their sites, registers it itself
    from bbmp import collection as registry
    monkeypatch.setattr(registry, "_registry", {k: v for k, v in registry._registry.items()
                                                if k not in ("keygenmusic", "asma", "zxart", "sndh")})
    monkeypatch.setattr(modland, "sizes_and_changes", lambda progress=None, since="": {
        "sizes": SIZES, "changes": [], "dated": DATED})
    for k in [k for k in os.environ if k.startswith("MODLAND_")]:   # the old names still count
        monkeypatch.delenv(k)
    monkeypatch.setenv("BBMP_MIRROR", "")      # a mirror set outside isn't the tests'
    monkeypatch.setenv("BBMP_EXTRAS", str(tmp_path / "extras"))
    monkeypatch.setenv("BBMP_DOWNLOADS", str(tmp_path / "downloads"))
    monkeypatch.setenv("BBMP_KEPT", str(tmp_path / "kept"))
    monkeypatch.setenv("BBMP_SNAPSHOT", str(tmp_path / "no snapshot"))
    monkeypatch.setenv("BBMP_CHIPS", str(tmp_path / "no chips"))
    from bbmp import song_chips

    def no_heads(path):
        raise OSError("the tests don't read songs' first bytes from Modland")
    monkeypatch.setattr(song_chips, "head", no_heads)
    from bbmp import extras
    monkeypatch.setattr(extras, "EXTRAS", tuple(
        extras.Extra(e.id, e.names, e.size, e.what, e.formats, e.required, e.without,
                     e.source, sha256=e.sha256) for e in extras.EXTRAS))   # and nowhere else


class LocalFetcher(Fetcher):
    """A fetcher whose 'server' is a dict of Modland's files by path, so tests never
    touch the network. Other collections go through their own transports."""

    def __init__(self, cache_dir: Path, server: dict[str, bytes]):
        super().__init__(cache_dir)
        self.server = server
        self.downloads: list[str] = []

    def _download(self, key: str, dest: Path | None = None) -> None:
        collection, path = collections_.split(key)
        if collection != "modland":
            return super()._download(key, dest)
        if path not in self.server:
            raise FileNotFoundError(path)
        self.downloads.append(path)
        dest = dest if dest is not None else self._places(key)[1]
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(self.server[path])

    def clone(self, n: int = 0) -> "LocalFetcher":
        other = LocalFetcher(self.cache_dir, self.server)
        other.downloads = self.downloads        # one list of what was asked for
        other.kept_dir = self.kept_dir
        return other


# ---- a second collection, to test that more than one works ----

CHIPLAND_SONGS = [
    "tunes/Tester/beep.mod",              # the same artist and title as a Modland song
    "tunes/Tester/Disk One/only here.mod",
    "tunes/Stranger/far away.mod",
]


class DictTransport(Transport):
    def __init__(self, files: dict[str, bytes], log: list[str]):
        self.files, self.log, self.closed = files, log, False

    def download(self, path: str, dest: Path) -> None:
        if path not in self.files:
            raise FileNotFoundError(path)
        self.log.append(path)
        dest.write_bytes(self.files[path])

    def close(self) -> None:
        self.closed = True


class Chipland(Collection):
    """Laid out as tunes/<Artist>/[<Album>/]<file>, all of it Protracker."""
    id = "chipland"
    name = "Chipland"
    home = "https://chipland.example"

    def __init__(self, files: dict[str, bytes]):
        self.files = files
        self.downloads: list[str] = []
        self.reachable = True

    def parse(self, path: str) -> Entry:
        parts = path.split("/")
        return Entry(path, "Protracker", parts[1], "/".join(parts[2:-1]), parts[-1],
                     md5=self.MD5.get(path, ""))

    # tunes/Tester/beep.mod is the very file Modland has as Protracker/Tester/beep.mod
    MD5 = {"tunes/Tester/beep.mod": f"{0:032x}", "tunes/Stranger/far away.mod": "f" * 32}

    def listing(self, progress=lambda s: None) -> Listing:
        if not self.reachable:
            raise OSError("chipland.example is down")
        return Listing([self.parse(p) for p in self.files])

    def transport(self) -> Transport:
        return DictTransport(self.files, self.downloads)

    def url(self, path: str) -> str:
        return "https://chipland.example/files/" + path

    def path_in(self, url: str) -> str | None:
        start = "https://chipland.example/files/"
        return url[len(start):] if url.startswith(start) else None


@pytest.fixture
def chipland(mod_bytes) -> Chipland:
    """The second collection, known to the player for as long as the test runs."""
    source = collections_.register(Chipland({p: mod_bytes for p in CHIPLAND_SONGS}))
    yield source
    collections_.unregister(source.id)


@pytest.fixture
def two(tmp_path, chipland) -> index_mod.Index:
    """A catalog of Modland and Chipland."""
    db = tmp_path / "index.db"
    index_mod.build({"modland": modland_listing(), "chipland": chipland.listing()}, db)
    return index_mod.Index(db)


@pytest.fixture
def fetcher(tmp_path, mod_bytes) -> LocalFetcher:
    server = {p: mod_bytes for p in SONGS if p.endswith(".mod")}
    return LocalFetcher(tmp_path / "cache", server)
