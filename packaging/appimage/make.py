#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make bbmp-x86_64.AppImage from the Flatpak's build: the same replayers, helpers and
Python packages (packaging/flatpak, built in the Flatpak SDK for any x86-64), with what
they need of the Flatpak's runtime (Python 3.14, the C library and the other libraries)
brought along. It runs on Linux systems older than the runtime: nothing of the system
is used but the kernel, the terminal and the sound server.

    packaging/appimage/make.py [--app DIR] [--runtime DIR] [--out FILE]

How the programs find what they bring along. Every program in it asks for the C
library's loader at one fixed place, /tmp/.bbmp-ld-<version>/ (an AppImage is mounted
somewhere new each time it runs, so the path can't point into it), which AppRun fills
with a copy of the loader. Each program and library then finds the libraries beside it
by a search path relative to itself ($ORIGIN). So a program keeps knowing where it is
(several helpers find their data beside themselves), and nothing is put in the
environment that the system's own programs (a browser, bubblewrap) would pick up.

Needs patchelf and appimagetool (TOOLS, default ~/.cache/bbmp-appimage/tools; see
packaging/appimage/README.md).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP_ID = "io.github.bytebandit.bbmp"
WORK = Path(os.environ.get("WORK") or Path.home() / ".cache/bbmp-appimage")
TOOLS = Path(os.environ.get("TOOLS") or WORK / "tools")
RUNTIME = (Path.home() / ".local/share/flatpak/runtime/org.freedesktop.Platform"
           / "x86_64/26.08/active/files")
PY = "python3.14"
LOADER = "ld-linux-x86-64.so.2"
# What of the runtime's Python is left out: tests, Tk, IDLE (about half its size)
PY_SKIP = {"test", "tests", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3",
           "__phello__"}
# Libraries the player loads by name (ctypes), which no program lists as needed
DLOPENED = ["libpulse-simple.so.0"]
# Programs of the runtime the player starts: flac for exports
RUNTIME_BIN = ["flac"]
# GCC's runtime libraries: the runtime has no licence folder for them
GCC_RUNTIME = ("libgcc_s.", "libstdc++.")
# The runtime's licence folders (share/licenses/freedesktop-sdk/<package>), for the
# libraries and programs taken from it: a library's name without "lib" and ".so...",
# where it isn't the folder's name or "lib" + that name
LICENCE_OF = {
    "c": "glibc", "dl": "glibc", "m": "glibc", "mvec": "glibc", "pthread": "glibc",
    "resolv": "glibc", "rt": "glibc", "ld-linux-x86-64": "glibc", "util": "glibc",
    "asound": "alsa-lib", "bz2": "bzip2", "crypto": "openssl", "ssl": "openssl",
    "exslt": "libxslt", "FLAC": "libflac", "flac": "libflac", "icudata": "icu",
    "icuuc": "icu", "icui18n": "icu", "lzma": "xz", "ncursesw": "ncurses",
    "panelw": "ncurses", "tinfow": "ncurses", "formw": "ncurses", "menuw": "ncurses",
    "pulse-simple": "libpulse", "pulse": "libpulse", "pulsecommon": "libpulse",
    "python3": "python3", "sqlite3": "sqlite", "vorbisfile": "libvorbis",
    "vorbisenc": "libvorbis", "z": "zlib-ng", "uuid": "util-linux", "mpdec": "python3",
    "sndfile": "sndfile", "dbus-1": "dbus", "systemd": "systemd", "cap": "libcap",
}


def run(*cmd, **kw) -> subprocess.CompletedProcess:
    return subprocess.run([str(c) for c in cmd], check=True, text=True, **kw)


def is_elf(path: Path) -> bool:
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def glibc_version(runtime: Path) -> str:
    libc = runtime / "lib/x86_64-linux-gnu/libc.so.6"
    found = re.search(rb"GNU C Library [^\n]*version (\d+\.\d+)", libc.read_bytes())
    return found.group(1).decode() if found else "x"


def needed(elf: Path, search: list[Path], loader: Path) -> list[Path]:
    """The libraries a program or library needs, as the runtime's loader finds them."""
    out = subprocess.run([str(loader), "--list", "--library-path",
                          ":".join(map(str, search)), str(elf)],
                         capture_output=True, text=True)
    libs = []
    for line in out.stdout.splitlines():
        m = re.match(r"\s*(\S+) => (/\S+)", line)
        if m:
            libs.append(Path(m.group(2)))
        elif "not found" in line:
            print(f"  ! {elf}: {line.strip()}", file=sys.stderr)
    return libs


def licence_folder(name: str, root: Path) -> Path | None:
    """The runtime's licence folder for a library or program of it, or None."""
    stem = re.sub(r"\.so(\.[\d.]*)?$", "", name)
    stem = re.sub(r"^lib", "", stem)
    bare = re.sub(r"[\d.]+$", "", stem)          # python3.14 -> python, gdbm -> gdbm
    for cand in (LICENCE_OF.get(stem), LICENCE_OF.get(bare), stem, f"lib{stem}", bare,
                 f"lib{bare}", f"{bare}3"):
        if cand and (root / cand).is_dir():
            return root / cand
    return None


def copy_licences(names: list[str], runtime: Path, dest: Path) -> list[str]:
    """Copy the licence folders of the runtime's libraries and programs (by file name)
    into dest/<package>, following the runtime's links (which point at /usr, the
    runtime's files), and write dest/README.txt of what is whose. Returns the names
    no licence folder was found for; a missing file is reported, not fatal."""
    root = runtime / "share/licenses/freedesktop-sdk"
    if not root.is_dir():
        print(f"  ! no licence folders in {root}", file=sys.stderr)
        return sorted(names)
    dest.mkdir(parents=True, exist_ok=True)
    whose: dict[str, list[str]] = {}
    missing = []
    for name in sorted(set(names)):
        folder = licence_folder(name, root)
        if folder is None:
            if not name.startswith(GCC_RUNTIME):
                missing.append(name)
            continue
        whose.setdefault(folder.name, []).append(name)
    for package in whose:
        for f in sorted((root / package).rglob("*")):
            if f.is_dir():
                continue
            real = f
            if f.is_symlink():
                target = os.readlink(f)
                real = (runtime / target[len("/usr/"):] if target.startswith("/usr/")
                        else (f.parent / target))
            out = dest / package / f.relative_to(root / package)
            try:
                out.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(real, out)
            except OSError as e:
                print(f"  ! licence file {f} not copied: {e}", file=sys.stderr)
    lines = ["The libraries and programs of the Freedesktop runtime "
             "(org.freedesktop.Platform) that this AppImage brings along, and the folder "
             "here with their licences, as the runtime has them.", ""]
    lines += [f"{p}/: {', '.join(n)}" for p, n in sorted(whose.items())]
    gcc = sorted(n for n in names if n.startswith(GCC_RUNTIME))
    if gcc:
        lines += ["", f"{', '.join(gcc)}: GCC's runtime libraries, under the GPL 3 with the "
                  "GCC Runtime Library Exception (https://www.gnu.org/licenses/gcc-exception-3.1.html), "
                  "which lets them go with any program; the GPL 3 is COPYING of the player."]
    if missing:
        lines += ["", "No licence folder found in the runtime for: " + ", ".join(missing)]
    (dest / "README.txt").write_text("\n".join(lines) + "\n")
    return missing


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--app", type=Path, default=Path.home() / ".cache/bbmp-flatpak/build/files",
                    help="the Flatpak's /app, as flatpak-builder left it")
    ap.add_argument("--runtime", type=Path, default=RUNTIME)
    ap.add_argument("--out", type=Path, default=HERE / "bbmp-x86_64.AppImage")
    a = ap.parse_args()
    patchelf, appimagetool = TOOLS / "bin/patchelf", TOOLS / "appimagetool"
    for tool in (patchelf, appimagetool):
        if not tool.exists():
            sys.exit(f"{tool} is missing: see packaging/appimage/README.md")
    if not (a.app / "opt").is_dir() or not (a.runtime / f"bin/{PY}").exists():
        sys.exit(f"no Flatpak build in {a.app}, or no runtime in {a.runtime}: "
                 "run packaging/flatpak/make.sh first")

    version = glibc_version(a.runtime)
    interp_dir = f"/tmp/.bbmp-ld-{version}"
    appdir = WORK / "AppDir"
    if appdir.exists():
        shutil.rmtree(appdir)
    usr_lib = appdir / "usr/lib"
    usr_lib.mkdir(parents=True)

    print("copying the Flatpak's /app and the runtime's Python")
    shutil.copytree(a.app, appdir / "app", symlinks=True,
                    ignore=shutil.ignore_patterns("manifest.json"))
    (appdir / "usr/bin").mkdir()
    shutil.copy2(a.runtime / f"bin/{PY}", appdir / f"usr/bin/{PY}")
    os.symlink(PY, appdir / "usr/bin/python3")
    for name in RUNTIME_BIN:
        shutil.copy2(a.runtime / "bin" / name, appdir / "usr/bin" / name)
    shutil.copytree(a.runtime / f"lib/{PY}", appdir / f"usr/lib/{PY}", symlinks=True,
                    ignore=lambda d, names: [n for n in names if n in PY_SKIP])

    print("finding the libraries everything needs")
    rt_lib = a.runtime / "lib/x86_64-linux-gnu"
    loader = rt_lib / LOADER
    app_libs = sorted({p.parent for p in (appdir / "app").rglob("*.so*") if is_elf(p)})
    search = [*app_libs, rt_lib, a.runtime / "lib"]
    elves = [p for p in appdir.rglob("*") if p.is_file() and not p.is_symlink() and is_elf(p)]
    todo = list(elves) + [rt_lib / n for n in DLOPENED]
    seen: set[Path] = set()
    while todo:
        elf = todo.pop()
        for lib in needed(elf, search, loader):
            real = lib.resolve()
            if real.is_relative_to(appdir) or real in seen:
                continue
            seen.add(real)
            dest = usr_lib / lib.name
            if not dest.exists():
                shutil.copy2(real, dest)
                todo.append(dest)
    for name in DLOPENED:
        if not (usr_lib / name).exists():
            shutil.copy2((rt_lib / name).resolve(), usr_lib / name)
    for name in (LOADER, "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0",
                 "librt.so.1", "libresolv.so.2"):
        if (rt_lib / name).exists() and not (usr_lib / name).exists():
            shutil.copy2((rt_lib / name).resolve(), usr_lib / name)
    print(f"  {len(list(usr_lib.glob('*.so*')))} libraries from the runtime")

    print("their licences, as the runtime has them")
    system = appdir / f"app/share/licenses/{APP_ID}/licences/system"
    missing = copy_licences([p.name for p in usr_lib.glob("*.so*")] + [PY, *RUNTIME_BIN],
                            a.runtime, system)
    print(f"  {len(list(system.iterdir())) - 1} licence folders in {system.relative_to(appdir)}")
    if missing:
        print(f"  ! no licence folder for: {', '.join(missing)}", file=sys.stderr)

    print(f"pointing every program at {interp_dir}/{LOADER} and its libraries")
    lib_dirs = [usr_lib, *app_libs]          # app_libs are inside the AppDir already
    patched = 0
    for elf in [p for p in appdir.rglob("*") if p.is_file() and not p.is_symlink() and is_elf(p)]:
        if elf.name in (LOADER, "libc.so.6"):
            continue
        head = subprocess.run([str(patchelf), "--print-interpreter", str(elf)],
                              capture_output=True, text=True)
        if head.returncode == 0 and head.stdout.strip():
            run(patchelf, "--set-interpreter", f"{interp_dir}/{LOADER}", elf)
        old = subprocess.run([str(patchelf), "--print-rpath", str(elf)],
                             capture_output=True, text=True).stdout.strip()
        keep = [p for p in old.split(":") if p.startswith("$ORIGIN")]
        rel = [f"$ORIGIN/{os.path.relpath(d, elf.parent)}" for d in lib_dirs]
        # DT_RPATH, not RUNPATH: it holds for what the program's libraries need too
        run(patchelf, "--force-rpath", "--set-rpath",
            ":".join(dict.fromkeys(keep + rel)), elf)
        patched += 1
    print(f"  {patched} programs and libraries")

    print("AppRun, the menu entry and the icon")
    shutil.copy2(HERE / "AppRun", appdir / "AppRun")
    (appdir / "AppRun").chmod(0o755)
    text = (HERE / "AppRun").read_text()
    (appdir / "AppRun").write_text(text.replace("@GLIBC@", version))
    desktop = (HERE.parent / "flatpak" / f"{APP_ID}.desktop").read_text()
    desktop = re.sub(r"(?m)^Exec=.*$", "Exec=bbmp", desktop)
    (appdir / f"{APP_ID}.desktop").write_text(desktop)
    shutil.copy2(HERE.parent / "flatpak" / f"{APP_ID}.svg", appdir / f"{APP_ID}.svg")

    print(f"packing {a.out}")
    env = {**os.environ, "ARCH": "x86_64", "APPIMAGE_EXTRACT_AND_RUN": "1"}
    run(appimagetool, "--no-appstream", appdir, a.out, env=env)
    print(f"made {a.out} ({a.out.stat().st_size / 1e6:.0f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
