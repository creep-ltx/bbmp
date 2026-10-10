# SPDX-License-Identifier: GPL-3.0-or-later
"""Songs inside archives: zip, 7z, LHA and RAR, played as files on this computer."""

import shutil
import subprocess
import zipfile

import pytest

from bbmp import archives, paths
from bbmp.index import Index


@pytest.fixture(autouse=True)
def cache(tmp_path, monkeypatch):
    monkeypatch.setattr(paths, "CACHE_DIR", tmp_path / "cache")


def _crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def make_lha(path, files: dict[str, bytes]) -> None:
    """An LHA archive of stored (-lh0-) files, level 0 headers, as old Amiga ones are."""
    out = b""
    for name, data in files.items():
        raw = name.replace("/", "\\").encode()
        body = (b"-lh0-" + len(data).to_bytes(4, "little") + len(data).to_bytes(4, "little")
                + (0x5B2A0000).to_bytes(4, "little") + bytes([0x20, 0, len(raw)]) + raw
                + _crc16(data).to_bytes(2, "little"))
        out += bytes([len(body), sum(body) & 0xFF]) + body + data
    path.write_bytes(out + b"\0")


def make_zip(tmp_path, mod_bytes):
    z = tmp_path / "pack.zip"
    with zipfile.ZipFile(z, "w") as f:
        f.writestr("Tester/beep.mod", mod_bytes)
        f.writestr("readme.txt", "hello")
        f.writestr("../evil.mod", mod_bytes)            # an archive is other people's
        f.writestr("/etc/evil2.mod", mod_bytes)
    return z


def test_a_zip_is_read_and_unpacked_and_nothing_leads_out(tmp_path, mod_bytes):
    z = make_zip(tmp_path, mod_bytes)
    assert sorted(n for n, _ in archives.members(z)) == ["Tester/beep.mod", "readme.txt"]
    folder = archives.unpacked(z)
    assert (folder / "Tester/beep.mod").read_bytes() == mod_bytes
    assert not (tmp_path / "evil.mod").exists() and not list(tmp_path.rglob("evil*"))
    assert archives.unpacked(z) == folder                      # once
    song = archives.join(z, "Tester/beep.mod")
    assert archives.split(song) == (z, "Tester/beep.mod")
    assert archives.file_of(song) == folder / "Tester/beep.mod"
    assert archives.split(str(tmp_path / "plain.mod")) is None


@pytest.mark.skipif(not shutil.which("7z"), reason="needs 7z")
@pytest.mark.parametrize("kind", [".7z", ".lha"])
def test_7z_and_lha_are_unpacked_in_the_sandbox(tmp_path, mod_bytes, kind):
    src = tmp_path / "src/TFMX"
    src.mkdir(parents=True)
    (src / "mdat.level1").write_bytes(b"TFMX-SONG " + bytes(600))
    (src / "smpl.level1").write_bytes(bytes(100))
    (src / "beep.mod").write_bytes(mod_bytes)
    packed = tmp_path / f"songs{kind}"
    if kind == ".lha":
        make_lha(packed, {f"TFMX/{f.name}": f.read_bytes() for f in sorted(src.iterdir())})
    else:
        subprocess.run(["7z", "a", "-bd", str(packed), "TFMX"], cwd=src.parent, check=True,
                       capture_output=True)
    names = sorted(n for n, _ in archives.members(packed))
    assert names == ["TFMX/beep.mod", "TFMX/mdat.level1", "TFMX/smpl.level1"]
    folder = archives.unpacked(packed)
    # the song and the file it needs beside it
    assert (folder / "TFMX/smpl.level1").is_file() and (folder / "TFMX/mdat.level1").is_file()


def test_songs_in_an_archive_are_songs(tmp_path, mod_bytes, catalog):
    z = make_zip(tmp_path, mod_bytes)
    songs = catalog.archive_songs(z)
    assert [(s.title, s.format, s.album) for s in songs] == [("beep", "Protracker", "pack")]
    assert songs[0].local and songs[0].path == archives.join(z.resolve(), "Tester/beep.mod")
    assert catalog.local_song(songs[0].path) == songs[0]


def test_a_song_in_an_archive_plays(tmp_path, mod_bytes, catalog, fetcher):
    import time
    from bbmp.audio import NullOutput
    from bbmp.player import Player
    from bbmp.settings import Settings
    z = make_zip(tmp_path, mod_bytes)
    started = []
    p = Player(catalog, fetcher, NullOutput(), settings=Settings(level=False),
               on_song=lambda s: started.append(s.title))
    p.play_list(catalog.archive_songs(z))
    end = time.time() + 10
    while time.time() < end and not (started and p.status().state == "stopped"):
        time.sleep(0.05)
    p.shutdown()
    assert started == ["beep"] and not p.status().error


def test_archives_unpacked_long_ago_go_first(tmp_path, mod_bytes):
    import os
    olds = []
    for i in range(3):
        z = tmp_path / f"pack{i}.zip"
        with zipfile.ZipFile(z, "w") as f:
            f.writestr("song.mod", mod_bytes + bytes(1000))
        olds.append(archives.unpacked(z))
        os.utime(olds[-1], (1000 + i, 1000 + i))
    archives.trim(keep_bytes=len(mod_bytes) * 2 + 2500, keep=olds[0])
    assert olds[0].is_dir() and olds[2].is_dir() and not olds[1].exists()
