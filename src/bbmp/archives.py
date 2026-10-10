# SPDX-License-Identifier: GPL-3.0-or-later
"""Songs inside archives: zip, 7z, LHA and RAR, as music comes on the Amiga (Aminet's
LHA files), from VGMRips (a zip a game) and JoshW (a 7z a game), or as the user
downloaded it.

A song inside an archive is written "<archive>#<name inside>". The first time one of
its songs is played the archive is unpacked whole, into a folder of the cache of its
own, so that the files a song needs beside it (TFMX's smpl. with its mdat., a PSF's
library) are there too; from then on its songs are files like any other. Zip files
are read by Python; the other kinds by 7z, shut in like a replayer (sandbox.py),
since archives come from the internet too.
"""

from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
import threading
import zipfile
from pathlib import Path, PurePosixPath

from . import paths, sandbox

KINDS = (".zip", ".7z", ".lha", ".lzh", ".rar")
MARK = "#"                      # between the archive and the name inside it
LIMIT = 1 << 30                 # bytes unpacked from one archive at the most
_lock = threading.Lock()


class ArchiveError(OSError):
    """The archive can't be read, or holds something it may not (".." in a name)."""


def is_archive(path: str | Path) -> bool:
    return Path(path).suffix.lower() in KINDS


def split(path: str) -> tuple[Path, str] | None:
    """(the archive, the name inside it) of "<archive>#<name>", or None for a plain
    path. The archive is the longest part that is an archive file."""
    at = path.find(MARK)
    while at != -1:
        outer = Path(path[:at])
        if is_archive(outer) and outer.is_file():
            return outer, path[at + 1:]
        at = path.find(MARK, at + 1)
    return None


def join(archive: Path, name: str) -> str:
    return f"{archive}{MARK}{name}"


def _safe(name: str) -> str | None:
    """The name inside as a relative path, or None where it would lead out (an archive
    is other people's)."""
    name = name.replace("\\", "/")
    parts = PurePosixPath(name).parts
    if not parts or name.startswith("/") or any(p in ("..", "") for p in parts) \
            or ":" in parts[0]:
        return None
    return "/".join(parts)


def members(archive: Path) -> list[tuple[str, int]]:
    """(name, bytes) of the files in an archive, the ones with a safe name."""
    if archive.suffix.lower() == ".zip":
        try:
            with zipfile.ZipFile(archive) as z:
                return [(n, i.file_size) for i in z.infolist()
                        if not i.is_dir() and (n := _safe(i.filename))]
        except (zipfile.BadZipFile, OSError, ValueError) as e:
            raise ArchiveError(f"{archive.name} can't be read: {e}") from None
    out, name, size, folder = [], None, 0, False
    for line in _seven(["l", "-slt", "-ba", str(archive)], archive).splitlines() + [""]:
        key, _, value = line.partition(" = ")
        if key == "Path":
            name = value
        elif key == "Size":
            size = int(value) if value.isdigit() else 0
        elif key == "Folder":
            folder = value == "+"
        elif key == "Attributes" and value.startswith("D"):
            folder = True
        elif not line.strip() and name is not None:
            if not folder and (safe := _safe(name)):
                out.append((safe, size))
            name, size, folder = None, 0, False
    return out


def _seven(args: list[str], archive: Path, writable: list[Path] = ()) -> str:
    seven = shutil.which("7z") or shutil.which("7za") or shutil.which("7zz")
    if seven is None:
        raise ArchiveError(f"{archive.suffix} files need the 7z program (7-Zip)")
    cmd = sandbox.wrap([seven, *args], archive, writable=list(writable))
    try:
        proc = sandbox.popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                             stdin=subprocess.DEVNULL)
        out, _ = proc.communicate(timeout=300)
    except (OSError, subprocess.SubprocessError) as e:
        raise ArchiveError(f"{archive.name} can't be read: {e}") from None
    if proc.returncode not in (0, 1):          # 1: warnings, what could be read was
        raise ArchiveError(f"{archive.name} can't be read (7z said {proc.returncode})")
    return out.decode("utf-8", "replace")


def folder_of(archive: Path) -> Path:
    """Where an archive is unpacked: one folder for each archive as it is now."""
    st = archive.stat()
    tag = hashlib.sha1(f"{archive.resolve()}\0{st.st_size}\0{st.st_mtime_ns}".encode())
    return paths.CACHE_DIR / "unpacked" / tag.hexdigest()[:20]


def unpacked(archive: Path) -> Path:
    """The archive unpacked, whole (once; later calls find it there)."""
    folder = folder_of(archive)
    with _lock:
        if (folder / ".complete").exists():
            return folder
        wanted = members(archive)
        if sum(size for _, size in wanted) > LIMIT:
            raise ArchiveError(f"{archive.name} unpacks to more than {LIMIT >> 20} MB")
        tmp = folder.with_name(folder.name + ".part")
        shutil.rmtree(tmp, ignore_errors=True)
        tmp.mkdir(parents=True)
        try:
            if archive.suffix.lower() == ".zip":
                _unzip(archive, tmp)
            else:
                _seven(["x", "-y", "-bd", f"-o{tmp}", str(archive)], archive, [tmp])
            _leave_out_links(tmp)
            (tmp / ".complete").touch()
            shutil.rmtree(folder, ignore_errors=True)
            os.replace(tmp, folder)
        except BaseException:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
    return folder


def _unzip(archive: Path, into: Path) -> None:
    written = 0
    with zipfile.ZipFile(archive) as z:
        for info in z.infolist():
            name = _safe(info.filename)
            if info.is_dir() or name is None:
                continue
            dest = into / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            with z.open(info) as src, open(dest, "wb") as out:
                while chunk := src.read(1 << 20):
                    written += len(chunk)
                    if written > LIMIT:     # more than the archive said it holds
                        raise ArchiveError(f"{archive.name} unpacks to too much")
                    out.write(chunk)


def _leave_out_links(folder: Path) -> None:
    """A link in an archive could point anywhere on this computer: none are kept."""
    for p in folder.rglob("*"):
        if p.is_symlink():
            p.unlink()


def file_of(path: str) -> Path:
    """The file on disk for a song: itself, or its copy unpacked from its archive."""
    inside = split(path)
    if inside is None:
        return Path(path)
    archive, name = inside
    if _safe(name) is None:
        raise ArchiveError(f"{name!r} is no name inside an archive")
    found = unpacked(archive) / _safe(name)
    if not found.is_file():
        raise FileNotFoundError(f"{archive.name} has no {name}")
    return found


def trim(keep_bytes: int = 2_000_000_000, keep: Path | None = None) -> None:
    """Take away the archives unpacked longest ago, beyond what may be kept; never
    `keep` (the one a song plays from)."""
    root = paths.CACHE_DIR / "unpacked"
    if not root.is_dir():
        return
    try:
        if keep is not None and keep.is_dir():
            os.utime(keep)                  # the one in use is the newest
        folders = sorted((p for p in root.iterdir() if p.is_dir() and p.suffix != ".part"),
                         key=lambda p: p.stat().st_mtime, reverse=True)
        total = 0
        for f in folders:
            total += sum(x.stat().st_size for x in f.rglob("*") if x.is_file())
            if total > keep_bytes and f != keep:
                with _lock:
                    shutil.rmtree(f, ignore_errors=True)
    except OSError:
        pass
