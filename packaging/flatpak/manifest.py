#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the Flatpak's manifest, io.github.bytebandit.bbmp.yml, from the table below.

    packaging/flatpak/manifest.py [-o file]

Every replayer, helper and library is built inside the Flatpak SDK, for any x86-64
(-O2 -march=x86-64 -mtune=generic), from bbmp-sources: a copy of every source at the
version used, which make.sh links here as `sources`. Nothing is downloaded while it
builds (flatpak-builder gives the build no network), and nothing comes from the machine
that builds it.

One module per build script (tools/build-*.sh, run with BBMP_SOURCES pointing at the
module's own copy of the archives it needs), and one per library the helpers and the
player load (libopenmpt, libsidplayfp, libresidfp, libgme) and for 7-Zip and the Python
packages. flatpak-builder keeps each module that built, so a failed build starts again
at the module that failed.

"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

HERE = Path(__file__).resolve().parent
APP = "io.github.bytebandit.bbmp"
SOURCES = "sources"         # make.sh links bbmp-sources here
CFLAGS = "-O2 -march=x86-64 -mtune=generic"

# helper modules: name, script, its prefix in /app/opt, the csrc folders it needs, the
# archives of bbmp-sources it needs. In this order: later ones use earlier ones'
# results (AdPlug and sc68 come with bbmp-chip; UADE before bbmp-amiga; the libraries
# before the helpers that link them).
HELPERS = [
    ("shut", ["build-shut.sh"], "bbmp-shut", ["shut"], []),
    ("gme", ["build-gme.sh"], "gme", ["gme"], ["game-music-emu-f68963b.tar.xz"]),
    ("uade", ["build-uade.sh"], "uade", [], [
        "uade-8d77cd7.tar.xz", "bencodetools-5fa73d3.tar.xz", "libzakalwe-f40771d.tar.xz"]),
    # bbmp-ay-vt2 is built inside ZXTune's tree, so bbmp-ay is built in ZXTune's module, and
    # bbmp-vgm from the libvgm in it (GPL 2 only: a program of its own since 7 Oct 2026)
    ("zxtune-ay", ["build-zxtune.sh", "build-ay.sh", "build-vgm.sh"], None, ["zxtune", "ay", "vgm"], [
        "zxtune-e36902f.tar.xz", "boost-1.92.0-b2-nodocs.tar.xz", "ayumi-07c08b4.tar.xz",
        "arkostracker3-14d975b.tar.xz", "JUCE-2d14a1f.tar.xz", "CPM-0.40.8.cmake"]),
    ("chip", ["build-chip.sh"], "bbmp-chip", ["chip"], [
        "StSound-a27c293.tar.xz", "mdxmini-003531a.tar.xz",
        "libbinio-3baed73.tar.xz", "adplug-1e6133f.tar.xz", "fmplayer-4fa914e.tar.xz",
        "libkss-15fb77f.tar.xz", "eupplayer-8587733.tar.xz", "organya.h-f7a086f.tar.xz",
        "libpxtone-dec7b60.tar.xz", "deadbeef_hq-b92e40f.tar.xz",
        "sc68-2.2.1.tar.gz", "ymfm-81aec25.tar.gz"], []),
    # psgplay (GPL 2 only) and Snes9x (non-commercial) may not be combined with GPL 3 code
    ("psgplay", ["build-psgplay.sh"], "bbmp-psgplay", ["psgplay"], ["psgplay-6f1c39f.tar.xz"]),
    ("snsf", ["build-snsf.sh"], "bbmp-snsf", ["snsf"], ["snsf9x-128cb0d.tar.xz", "deadbeef_hq-b92e40f.tar.xz"]),
    ("furnace", ["build-furnace.sh"], "furnace", ["furnace"], ["furnace-fa9541d.tar.xz"]),
    ("sunvox", ["install-sunvox.sh"], "sunvox", [], ["sunvox_lib-2.1.4d.zip"]),
    ("xmp", ["build-xmp.sh"], "bbmp-xmp", ["xmp"], ["libxmp-1e08121.tar.xz"]),
    ("retrovert", ["build-retrovert.sh"], "bbmp-rv", ["retrovert"], [
        "retrovert-playback-klystrack-aad080d.tar.xz"]),
    ("musicline", ["build-musicline.sh"], "bbmp-musicline", ["musicline"],
     ["nostalgicplayer-musicline-2485bec.tar.xz"]),
    ("fac", ["build-fac.sh"], "bbmp-fac", ["fac"], ["emu8950-fb129b1.tar.xz", "emu2413-2e4f6f9.tar.xz"]),
    ("psycle", ["build-psycle.sh"], "bbmp-psycle", ["psycle"], ["psycle-r12005.tar.xz"]),
    ("iigs", ["build-iigs.sh"], "bbmp-iigs", ["iigs"], []),
    ("nez", ["build-nez.sh"], "bbmp-nez", ["nez"], ["libnezplug-161d6e2.tar.xz"]),
    ("console", ["build-console.sh"], "bbmp-console", ["console"],
     ["rePlayer-e3bf02c.tar.xz", "aosdk-e359a6e.tar.xz"]),
    ("misc", ["build-misc.sh"], "bbmp-misc", ["misc"], [
        "zlib-blast-1.3.1.tar.xz", "GoatTracker_2.77.zip", "GoatTracker_1.53.zip",
        "GoatTracker_1.53_Stereo.zip"]),
    ("adlib", ["build-adlib.sh"], "bbmp-adlib", ["adlib"], []),
    ("pc", ["build-pc.sh"], "bbmp-pc", ["pc"], [
        "mikmod-8de19ee.tar.xz", "PlayerPRO-0a07010.tar.xz", "bmxplay-e77f921.tar.xz"]),
    ("synth", ["build-synth.sh"], "bbmp-synth", ["synth"], [
        "rePlayer-e3bf02c.tar.xz", "openmpt-skm-0eafb12.tar.xz", "skale-skm-loader-openmpt-27dde22.tar.xz"]),
    ("amiga", ["build-amiga.sh"], "bbmp-amiga", ["amiga"], [
        "uade-8d77cd7.tar.xz", "scummvm-maxtrax-e6c3b10.tar.xz",
        "stonefree2.lha", "stonefree2.readme", "vasm2_0f.tar.gz",
        "lhafile-0.3.1.tar.gz", "amitools-0.8.1.tar.gz", "machine68k-0.3.0.tar.gz",
        "setuptools_scm-10.3.4-py3-none-any.whl", "vcs_versioning-2.5.0-py3-none-any.whl",
        "packaging-26.3-py3-none-any.whl"]),
    ("retro", ["build-retro.sh"], "bbmp-retro", ["retro"], [
        "emu2212-ee35dbe.tar.xz", "cc65-5552824.tar.xz", "nedplay.zip", "rgbds-0.6.1.tar.gz",
        "GBSoundSystem-c0c6a6f.tar.xz", "dasm-0927086.tar.xz", "victracker-2.0.tar.gz"]),
    ("darkwave", ["build-darkwave.sh"], "bbmp-darkwave", ["darkwave"], ["DarkWave-Studio-4.1.6-Source.zip"]),
    ("pcmusic", ["build-pcmusic.sh"], "bbmp-pcmusic", ["pcmusic"], []),
    ("piece", ["build-piece.sh"], "bbmp-piece", ["piece"], []),
    ("chipnsfx", ["build-chipnsfx.sh"], "bbmp-chipnsfx", ["chipnsfx"], []),
    ("tiatracker", ["build-tiatracker.sh"], "bbmp-tiatracker", ["tiatracker"], []),
    ("synder", ["build-synder.sh"], "bbmp-synder", ["synder"], []),
    ("t0ast", ["build-t0ast.sh"], "bbmp-t0ast", ["t0ast"], []),
    ("berotracker", ["build-berotracker.sh"], "bbmp-berotracker", ["berotracker"], []),
    ("axs", ["build-axs.sh"], "bbmp-axs", ["axs"], []),
]
PREFIXES = {"build-zxtune.sh": "zxtune", "build-ay.sh": "bbmp-ay", "build-vgm.sh": "bbmp-vgm"}
# other folders of this repository a helper's script reads (bbmp-console installs the GPL 2
# and LGPL 2.1 texts from licences/)
EXTRA_DIRS = {"console": ["licences"]}

WHEELS = [
    "textual-8.2.8-py3-none-any.whl", "rich-15.0.0-py3-none-any.whl",
    "markdown_it_py-4.2.0-py3-none-any.whl", "mdurl-0.1.2-py3-none-any.whl",
    "mdit_py_plugins-0.6.1-py3-none-any.whl", "linkify_it_py-2.2.0-py3-none-any.whl",
    "uc_micro_py-2.0.0-py3-none-any.whl", "pygments-2.21.0-py3-none-any.whl",
    "platformdirs-4.11.12-py3-none-any.whl", "typing_extensions-4.16.0-py3-none-any.whl",
    "numpy-2.5.3-cp314-cp314-manylinux_2_27_x86_64.manylinux_2_28_x86_64.whl",
]


def sums(root: Path) -> dict[str, str]:
    out = {}
    for line in (root / "sha256sums.txt").read_text().splitlines():
        s, name = line.split(None, 1)
        out[name.removeprefix("archives/")] = s
    return out


def archive(name: str, sha: dict[str, str], dest: str = SOURCES + "/archives") -> dict:
    if name not in sha:
        raise SystemExit(f"{name} is not in bbmp-sources' sha256sums.txt")
    return {"type": "file", "path": f"{SOURCES}/archives/{name}", "sha256": sha[name], "dest": dest}


def lib_module(name: str, kind: str, file: str, sha: dict, opts: list[str], **more) -> dict:
    m = {"name": name, "buildsystem": kind, "config-opts": opts,
         "sources": [{"type": "archive", "path": f"{SOURCES}/archives/{file}", "sha256": sha[file]}]}
    m.update(more)
    return m


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", default=str(HERE / f"{APP}.yml"))
    a = ap.parse_args()
    sha = sums(HERE / SOURCES)
    public = "1"
    modules: list[dict] = []

    # --- the libraries: what the player opens itself and the helpers link
    modules.append(lib_module("libresidfp", "autotools", "libresidfp-1.2.2.tar.gz", sha,
                              ["--disable-static"]))
    modules.append(lib_module("libsidplayfp", "autotools", "libsidplayfp-3.1.1.tar.gz", sha,
                              ["--disable-static", "--without-exsid", "--without-usbsid"]))
    modules.append(lib_module("libopenmpt", "autotools", "libopenmpt-0.8.9+release.autotools.tar.gz", sha, [
        "--disable-static", "--disable-openmpt123", "--disable-examples", "--disable-tests",
        "--disable-doxygen-doc", "--without-portaudio", "--without-portaudiocpp",
        "--without-pulseaudio", "--without-sndfile", "--without-flac", "--without-sdl2"]))
    # Game Music Emu as it was released (0.6.5), as the system's is where the player is
    # built from source: bbmp-retro links it. The player's own (build-gme.sh) is in /app/opt/gme.
    modules.append(lib_module("libgme", "cmake-ninja", "libgme-0.6.5-src.tar.gz", sha, [
        "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_SHARED_LIBS=ON", "-DGME_BUILD_EXAMPLES=OFF",
        "-DGME_BUILD_TESTING=OFF", "-DENABLE_UBSAN=OFF", "-DCMAKE_INSTALL_LIBDIR=lib"]))
    # 7-Zip (the player opens archives with 7z): 7zz, all formats in one program, as 7z
    modules.append({
        "name": "7zip", "buildsystem": "simple",
        "sources": [{"type": "archive", "path": f"{SOURCES}/archives/7z2603-src.tar.xz",
                     "sha256": sha["7z2603-src.tar.xz"], "strip-components": 0}],
        "build-commands": [
            # its own -O2, for any x86-64, without -Werror (a newer compiler warns more);
            # DISABLE_RAR=1 leaves out its RAR code (from unRAR, whose licence restricts
            # its use), so RAR archives don't open in the packages
            "make -C CPP/7zip/Bundles/Alone2 -f ../../cmpl_gcc.mak -j4 DISABLE_RAR=1 "
            "MY_ARCH='-march=x86-64 -mtune=generic' CFLAGS_WARN_WALL='-Wall'",
            "install -Dm755 CPP/7zip/Bundles/Alone2/b/g/7zz /app/bin/7zz",
            "ln -sf 7zz /app/bin/7z",
            "install -Dm644 DOC/License.txt /app/share/licenses/7zip/License.txt"]})

    # --- the helpers, each by its own build script
    for name, scripts, prefix, csrc, archives, *private_only in HELPERS:
        if not all((HERE / "../../tools" / s).exists() for s in scripts):
            continue        # not in this copy of the source
        srcs: list[dict] = [{"type": "file", "path": "../../tools/sources.sh", "dest": "tools"}]
        for s in scripts:
            srcs.append({"type": "file", "path": f"../../tools/{s}", "dest": "tools"})
        for c in csrc:
            srcs.append({"type": "dir", "path": f"../../csrc/{c}", "dest": f"csrc/{c}"})
        for d in EXTRA_DIRS.get(name, []):
            srcs.append({"type": "dir", "path": f"../../{d}", "dest": d})
        if archives:
            srcs.append({"type": "file", "path": f"{SOURCES}/sha256sums.txt", "dest": SOURCES})
        srcs += [archive(x, sha) for x in archives]
        cmds = []
        for s in scripts:
            pre = PREFIXES.get(s, prefix)
            cmds.append(f"BBMP_SOURCES=$PWD/{SOURCES} BBMP_SRC=$PWD/work bash tools/{s} /app/opt/{pre}")
        modules.append({"name": f"helper-{name}", "buildsystem": "simple", "sources": srcs,
                        "build-commands": cmds})

    # libsidplayfp's C interface for the player (csrc/sidshim.cpp), built here: the
    # Flatpak has no compiler for the player to build it with at its first start
    modules.append({
        "name": "sidshim", "buildsystem": "simple",
        "sources": [{"type": "file", "path": "../../csrc/sidshim.cpp"}],
        "build-commands": [
            "mkdir -p /app/opt/sidshim",
            f"g++ {CFLAGS} -shared -fPIC -o /app/opt/sidshim/libsidshim.so sidshim.cpp -lsidplayfp"]})

    # --- the player's Python packages, as wheels from bbmp-sources (numpy's picks its
    # CPU code at run time)
    modules.append({
        "name": "python-packages", "buildsystem": "simple",
        "sources": [archive(w, sha, dest="wheels") for w in WHEELS],
        "build-commands": [
            "pip3 install --prefix=/app --no-index --no-cache-dir --find-links wheels "
            + " ".join(w.split("-")[0].replace("_", "-") + "==" + w.split("-")[1] for w in WHEELS)]})

    modules.append({
        "name": "bbmp", "buildsystem": "simple",
        "sources": [{"type": "dir", "path": "../..",
                     "skip": ["packaging", ".git", "tests", "tools", "docs", "screenshots", "csrc"]}],
        "build-commands": [
            "pip3 install --prefix=/app --no-deps --no-build-isolation --no-cache-dir .",
            f"install -Dm644 COPYING /app/share/licenses/{APP}/COPYING",
            f"cp -r licences /app/share/licenses/{APP}/licences"]})

    # in menus and launchers, with its icon: a terminal program (Terminal=true), so the
    # launcher opens a terminal for it
    modules.append({
        "name": "desktop", "buildsystem": "simple",
        "sources": [{"type": "file", "path": f"{APP}.desktop"}, {"type": "file", "path": f"{APP}.svg"}],
        "build-commands": [
            f"install -Dm644 {APP}.desktop /app/share/applications/{APP}.desktop",
            f"install -Dm644 {APP}.svg /app/share/icons/hicolor/scalable/apps/{APP}.svg"]})

    manifest = {
        "app-id": APP,
        "runtime": "org.freedesktop.Platform",
        "runtime-version": "26.08",
        "sdk": "org.freedesktop.Sdk",
        "command": "bbmp",
        "finish-args": [
            # Only what the player uses. Its own settings, catalogs and cache are in
            # ~/.var/app/<id>, which a Flatpak has without asking. Not the whole home
            # folder (with the network, a flaw in the player could send ~/.ssh anywhere),
            # and nothing on the command path (~/.local/bin writable is a way out of the
            # Flatpak): the bbmp command is a line the user types (command.py).
            "--share=network",               # the collections
            "--socket=pulseaudio",           # sound (PipeWire answers it too)
            "--filesystem=xdg-music",        # kept songs and exports, songs of your own
            "--filesystem=xdg-download:ro",  # songs just downloaded, played from there
            "--env=BBMP_OPT=/app/opt",       # where the replayers are
        ],
        "build-options": {
            "cflags": CFLAGS, "cflags-override": True,
            "cxxflags": CFLAGS, "cxxflags-override": True,
            # LIBRARY_PATH: the build scripts link -lopenmpt and the rest without LDFLAGS
            "env": {"BBMP_OPT": "/app/opt", "BBMP_PUBLIC": public, "JOBS": "4",
                    "LIBRARY_PATH": "/app/lib"},
            "strip": True, "no-debuginfo": True,
        },
        "cleanup": ["/include", "/lib/pkgconfig", "/lib/cmake", "/share/man", "/share/doc",
                    "*.la", "*.a", "/opt/*/include", "/opt/*/lib/pkgconfig", "/opt/*/share/man",
                    "/opt/*/share/doc"],
        "modules": modules,
    }
    head = ("# SPDX-License-Identifier: GPL-3.0-or-later\n"
            "# ByteBandit Music Player as a Flatpak. Written by manifest.py (edit that, not this);\n"
            "# make.sh builds it. Every module is built inside the SDK from bbmp-sources, for any x86-64.\n")
    # JSON is YAML; flatpak-builder reads either
    Path(a.o).write_text(head + json.dumps(manifest, indent=2) + "\n")
    print(f"wrote {a.o} ({len(modules)} modules, BBMP_PUBLIC={public})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
