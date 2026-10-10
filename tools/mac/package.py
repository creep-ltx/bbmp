#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Make bbmp-mac: the player as a folder that runs on a Mac with nothing installed.

    tools/mac/package.py [--out dist]

Run on the Mac that built the replayers (tools/mac/build-all.sh; the GitHub workflow
does both). The folder holds a Python of its own (python-build-standalone) with
Textual and numpy, the player, the replayers from ~/.local/opt, and the libraries
they use from Homebrew, rewritten to load from inside the folder: the Mac it is given
to has no Homebrew. Every program and library changed is signed again (ad hoc), which
an Apple Silicon Mac needs to run it at all. The result is
dist/bbmp-mac-<date>-<arch>.tar.gz."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "src"))

PYTHON = "3.12"
# the libraries the player loads itself (ctypes): Homebrew's, copied in
LIBRARIES = ("openmpt", "gme", "sidplayfp", "samplerate", "sndfile")
HOMEBREW = ("/opt/homebrew/", "/usr/local/")


def run(*cmd: str, **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, check=True, text=True, capture_output=True, **kw)


def python_standalone(dest: Path) -> str:
    """Unpack the newest python-build-standalone for this Mac into dest; its name."""
    arch = "aarch64" if platform.machine() == "arm64" else "x86_64"
    api = "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest"
    req = urllib.request.Request(api, headers={"Accept": "application/vnd.github+json"})
    if os.environ.get("GITHUB_TOKEN"):
        req.add_header("Authorization", f"Bearer {os.environ['GITHUB_TOKEN']}")
    release = json.load(urllib.request.urlopen(req))
    want = f"-{arch}-apple-darwin-install_only.tar.gz"
    asset = next(a for a in release["assets"]
                 if a["name"].startswith(f"cpython-{PYTHON}.") and a["name"].endswith(want))
    tgz = dest.parent / asset["name"]
    urllib.request.urlretrieve(asset["browser_download_url"], tgz)
    with tarfile.open(tgz) as t:
        t.extractall(dest.parent, filter="tar")
    (dest.parent / "python").rename(dest)
    tgz.unlink()
    return asset["name"]


def is_macho(p: Path) -> bool:
    if p.is_symlink() or not p.is_file():
        return False
    with open(p, "rb") as f:
        magic = f.read(4)
    return magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xce\xfa\xed\xfe")


def own_name(p: Path) -> str | None:
    """A library's own install name (otool -L lists it first, as if it were a need)."""
    lines = run("otool", "-D", str(p)).stdout.splitlines()[1:]
    return lines[0].strip() if lines else None


def dependencies(p: Path) -> list[str]:
    lines = run("otool", "-L", str(p)).stdout.splitlines()[1:]
    own = own_name(p)
    return [d for d in (line.strip().split(" (")[0] for line in lines) if d != own]


def rpaths(p: Path) -> list[str]:
    out, lines = [], run("otool", "-l", str(p)).stdout.splitlines()
    for i, line in enumerate(lines):
        if "cmd LC_RPATH" in line:
            out.append(lines[i + 2].strip().split(" ")[1])
    return out


def where(dep: str, origin: Path) -> Path | None:
    """The file a Homebrew dependency names, as it is on this Mac."""
    if dep.startswith(HOMEBREW):
        return Path(dep)
    if dep.startswith("@loader_path/"):
        return (origin.parent / dep[len("@loader_path/"):]).resolve()
    if dep.startswith("@rpath/"):
        for r in rpaths(origin):
            r = r.replace("@loader_path", str(origin.parent))
            if (Path(r) / dep[len("@rpath/"):]).exists():
                return (Path(r) / dep[len("@rpath/"):]).resolve()
    return None


def relocate(bundle: Path, libdir: Path, built: Path,
             copied: dict[Path, Path] | None = None) -> int:
    """Copy every Homebrew library a program or library in `bundle` uses into libdir,
    make each one ask for its copies by @rpath, point what names the replayers' own
    folder on the Mac that built them (`built`, copied as `bundle`) at the copy beside
    it, and sign each again. `copied`, if given, gets each copy and where it came from."""
    origin: dict[Path, Path] = {}      # a copied library: where it came from
    todo = [p for p in bundle.rglob("*") if is_macho(p)]
    done, changed = set(), 0
    while todo:
        f = todo.pop()
        if f in done:
            continue
        done.add(f)
        src = origin.get(f, f)
        if own_name(f):                     # a library: named by @rpath, not by where it was
            run("install_name_tool", "-id", f"@rpath/{f.name}", str(f))
        # search paths that name the build folder (-Wl,-rpath,$ADPLUG/lib): its copy here,
        # relative to the file. The Mac it is given to has no such folder, and here the
        # sandbox wouldn't let a replayer read it.
        for r in rpaths(f):
            if r == str(built) or r.startswith(f"{built}/"):
                there = bundle / Path(r).relative_to(built)
                new = f"@loader_path/{os.path.relpath(there, f.parent)}"
                if new in rpaths(f):        # already there: the old one only goes
                    run("install_name_tool", "-delete_rpath", r, str(f))
                else:
                    run("install_name_tool", "-rpath", r, new, str(f))
                changed += 1
        for dep in dependencies(f):
            if dep.startswith(("/usr/lib/", "/System/")):
                continue
            if dep.startswith(f"{built}/"):
                there = bundle / Path(dep).relative_to(built)
                if not there.exists():
                    raise SystemExit(f"{f} needs {dep}, which isn't in the package")
                run("install_name_tool", "-change", dep,
                    f"@loader_path/{os.path.relpath(there, f.parent)}", str(f))
                changed += 1
                continue
            real = where(dep, src)
            if real is None or not str(real).startswith(HOMEBREW) and real not in origin.values():
                if dep.startswith(HOMEBREW):
                    raise SystemExit(f"{f} needs {dep}, which isn't there")
                continue
            copy = libdir / Path(dep).name
            if not copy.exists():
                shutil.copy2(real, copy)
                copy.chmod(0o755)
                origin[copy] = real
                if copied is not None:
                    copied[copy] = real
                run("install_name_tool", "-id", f"@rpath/{copy.name}", str(copy))
                todo.append(copy)
            run("install_name_tool", "-change", dep, f"@rpath/{copy.name}", str(f))
            rel = os.path.relpath(libdir, f.parent)
            if f"@loader_path/{rel}" not in rpaths(f):
                run("install_name_tool", "-add_rpath", f"@loader_path/{rel}", str(f))
            changed += 1
        run("codesign", "--force", "--sign", "-", str(f))
    return changed


# the files of a Homebrew package (its "keg", Cellar/<name>/<version>) that are its
# licence: Homebrew installs them at the top of the keg
LICENCE_FILES = ("LICENSE", "LICENCE", "COPYING", "COPYRIGHT", "NOTICE", "UNLICENSE")


def keg(path: Path) -> Path | None:
    """The Homebrew keg a file belongs to (its real place under Cellar), or None."""
    parts = path.resolve().parts
    if "Cellar" in parts:
        i = parts.index("Cellar")
        if len(parts) > i + 2:
            return Path(*parts[:i + 3])
    return None


def copy_licences(libraries: dict[str, Path], dest: Path) -> None:
    """Copy the licence files of the Homebrew packages the copied libraries come from
    (library name -> where it was copied from) into dest/<package>/, with a README.txt
    of what is whose. Never stops the packaging: what can't be found is written down."""
    try:
        dest.mkdir(parents=True, exist_ok=True)
        whose: dict[str, tuple[Path, list[str]]] = {}
        unknown = []
        for name, src in sorted(libraries.items()):
            k = keg(src)
            if k is None:
                unknown.append(name)
            else:
                whose.setdefault(k.parent.name, (k, []))[1].append(name)
        lines = ["The libraries this folder brings along from Homebrew (opt/lib), and the",
                 "folder here with the licence files of the Homebrew package each comes from.",
                 ""]
        for package, (k, names) in sorted(whose.items()):
            files = [f for f in k.iterdir()
                     if f.is_file() and f.name.upper().startswith(LICENCE_FILES)]
            for f in files:
                (dest / package).mkdir(exist_ok=True)
                shutil.copyfile(f, dest / package / f.name)
            note = "" if files else " (Homebrew keeps no licence file for it; see its source)"
            lines.append(f"{package}/ (Homebrew {package} {k.name}): {', '.join(names)}{note}")
            if not files:
                print(f"  ! no licence file in {k}", flush=True)
        if unknown:
            lines += ["", "Not from a Homebrew package as far as could be told: " + ", ".join(unknown)]
        (dest / "README.txt").write_text("\n".join(lines) + "\n")
        print(f"  licences of {len(whose)} Homebrew packages in {dest.name}/", flush=True)
    except Exception as e:              # a licence that can't be copied mustn't stop the build
        print(f"  ! the Homebrew licences were not all copied: {e}", flush=True)


LAUNCHER = """#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Starts ByteBandit Music Player (bbmp-mac).
HERE="$(cd "$(dirname "$0")" && pwd -P)"
export BBMP_OPT="$HERE/opt"
export BBMP_BUNDLE="$HERE"                # shown to the replayers in their sandbox
export PYTHONPATH="$HERE/app"
export PYTHONNOUSERSITE=1
export PATH="$HERE/python/bin:$PATH"
[ -n "$SSL_CERT_FILE" ] || export SSL_CERT_FILE=/etc/ssl/cert.pem
exec "$HERE/python/bin/python3" -m bbmp "$@"
"""

README = """ByteBandit Music Player for the Mac (bbmp-mac), a test version
==================================================================

A player for tracker and chip music, in the Terminal. It plays about 540,000 songs
from Modland, HVSC and other collections, downloading each as you play it.

To start it, open Terminal and run:

    cd ~/Downloads/bbmp-mac        (or wherever you unpacked it)
    xattr -dr com.apple.quarantine .
    ./bbmp

The xattr line is needed once: this test version isn't signed by Apple, so macOS
would otherwise refuse to run its programs. It only removes the "downloaded from the
internet" mark from this folder.

The keys: arrows and Enter to browse, / to search, Space to pause, ? for all of them.
If a song sounds wrong, press ! and say what; `./bbmp wrong` lists what you noted.

It needs a Mac with {arch}, macOS 13 or newer.
Built {date} from {commit}. Python: {python}.
"""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(ROOT / "dist"))
    args = ap.parse_args()
    from bbmp import paths
    from bbmp.backends import sid

    arch = platform.machine()
    out = Path(args.out)
    bundle = out / "bbmp-mac"
    shutil.rmtree(bundle, ignore_errors=True)
    bundle.mkdir(parents=True)

    print("python ...", flush=True)
    py_name = python_standalone(bundle / "python")
    run(str(bundle / "python/bin/python3"), "-m", "pip", "install", "--quiet",
        "--no-warn-script-location", "textual", "numpy")

    print("the player ...", flush=True)
    shutil.copytree(ROOT / "src/bbmp", bundle / "app/bbmp",
                    ignore=shutil.ignore_patterns("__pycache__"))

    print("the replayers ...", flush=True)
    shutil.copytree(paths.OPT, bundle / "opt", symlinks=True,
                    ignore=shutil.ignore_patterns(".mac-built", "include", "*.a", "*.la"))
    libdir = bundle / "opt/lib"
    libdir.mkdir(exist_ok=True)
    shim = bundle / "opt/sidshim" / f"libsidshim{paths.SO}"
    shim.parent.mkdir(exist_ok=True)
    run(*sid.shim_command(shim))
    prefix = run("brew", "--prefix").stdout.strip()
    copied: dict[Path, Path] = {}      # every Homebrew library copied: where it came from
    for name in LIBRARIES:
        lib = Path(prefix) / "lib" / f"lib{name}.dylib"
        if lib.exists():
            copied[libdir / lib.name] = lib
            shutil.copy2(lib.resolve(), libdir / lib.name)
            (libdir / lib.name).chmod(0o755)
            run("install_name_tool", "-id", f"@rpath/{lib.name}", str(libdir / lib.name))
        else:
            print(f"  lib{name} isn't installed (brew); its formats won't play")

    print("libraries ...", flush=True)
    n = relocate(bundle / "opt", libdir, paths.OPT.resolve(), copied)
    outside = (*HOMEBREW, f"{paths.OPT.resolve()}/", str(Path.home()))
    left = [f"{f}: {d}" for f in (bundle / "opt").rglob("*") if is_macho(f)
            for d in (*dependencies(f), *rpaths(f)) if d.startswith(outside)]
    if left:
        raise SystemExit("still points outside the package:\n  " + "\n  ".join(left[:20]))
    print(f"  {n} references made to point inside the folder")

    (bundle / "bbmp").write_text(LAUNCHER)
    (bundle / "bbmp").chmod(0o755)
    commit = run("git", "-C", str(ROOT), "rev-parse", "--short", "HEAD").stdout.strip()
    date = datetime.date.today().isoformat()
    (bundle / "README.txt").write_text(README.format(
        arch="an Apple processor (M1 or newer)" if arch == "arm64" else "an Intel processor",
        date=date, commit=commit, python=py_name))
    shutil.copy2(ROOT / "COPYING", bundle / "COPYING")       # the GPL
    shutil.copytree(ROOT / "licences", bundle / "licences", dirs_exist_ok=True)  # the parts' own
    # and those of the Homebrew libraries it brings along
    copy_licences({c.name: src for c, src in copied.items()}, bundle / "licences/system")

    tgz = out / f"bbmp-mac-{date}-{arch}.tar.gz"
    with tarfile.open(tgz, "w:gz") as t:
        t.add(bundle, arcname="bbmp-mac")
    print(f"made {tgz} ({tgz.stat().st_size / 1e6:.0f} MB)")


if __name__ == "__main__":
    main()
