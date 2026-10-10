#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build Psycle's native plugins as shared libraries for bbmp-psycle.

    build_plugins.py <root> <out> <compat>

<root> has psycle-plugins, psycle-helpers, universalis, diversalis and
external-packages/stk-4.5.0 of Psycle's sources. What a plugin is made of is read
from its Visual Studio project (psycle-plugins/msvc/*.msvc-2022.vcxproj): the name of
the project is the name of the DLL that songs ask for, and its list of sources and
definitions is the one the Windows plugins are built from. The libraries go to <out>
as <name>.so, the name in lower case.

Plugins that can't be built are told on stderr and left out; the helper then treats
them as it treats a plugin that doesn't exist. The exit status is 0 when at least
the plugins in REQUIRED were built.
"""

import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

# Not plugins, or not for here: the SoundFont player needs FluidSynth and a SoundFont
# file that no song brings along, and the MIDI output plugin sends to MIDI devices.
SKIP = {"fluidsynth", "fs-sf2-player", "ymidi", "dsp", "blwtbl"}
# Projects that are libraries of other plugins: their sources become part of those.
LIBRARIES = {"dsp", "blwtbl"}
REQUIRED = {"arguru-synth-2f", "blitz12", "jmdrum", "pooplog-fm-laboratory"}
# Plugins that are built a second time as they were before Psycle's revision of its
# plugins in 2010, for the songs of before: <folder>/<name>.so, from the project
# <name> with a definition more.
BEFORE = {"before-2010/eq3": ("eq3", "BBMP_EQ3_BEFORE_2010")}


def project(path):
    text = open(path, encoding="utf-8-sig", errors="replace").read()
    if path.endswith(".vcproj"):        # Visual Studio 2008 and older
        sources = [s.replace("\\", "/") for s in re.findall(r'RelativePath="([^"]+\.(?:cpp|c|cc))"', text, re.I)]
        found_defines = re.findall(r'PreprocessorDefinitions="([^"]*)"', text)
        uses = [os.path.basename(r.replace("\\", "/")).split(".")[0].lower()
                for r in re.findall(r'RelativePathToProject="([^"]+)"', text)]
    else:
        sources = [s.replace("\\", "/") for s in re.findall(r'<ClCompile Include="([^"]+)"', text)]
        found_defines = re.findall(r"<PreprocessorDefinitions>([^<]*)</PreprocessorDefinitions>", text)
        uses = [os.path.basename(r.replace("\\", "/")).split(".msvc")[0].lower()
                for r in re.findall(r'<ProjectReference Include="([^"]+)"', text)]
    defines = set()
    for found in found_defines:
        for d in found.split(";"):
            if d and not d.startswith("%") and d not in ("NDEBUG", "_DEBUG", "WIN32", "_WINDOWS", "_USRDLL"):
                defines.add(d)
    return sources, sorted(defines), uses


def main():
    root, out, compat = (os.path.abspath(a) for a in sys.argv[1:4])
    suffix = sys.argv[4] if len(sys.argv) > 4 else ".msvc-2022.vcxproj"
    msvc = os.path.join(root, "psycle-plugins", "msvc")
    obj = os.path.join(out, ".obj")
    os.makedirs(obj, exist_ok=True)
    stk = os.path.join(root, "external-packages", "stk-4.5.0")
    flags = ["-std=gnu++14", "-O2", "-fPIC", "-w", "-fpermissive", "-fno-gnu-unique", "-DNDEBUG",
             "-D__LITTLE_ENDIAN__", "-include", os.path.join(compat, "plugin.hpp"), "-I" + compat,
             "-I" + os.path.join(root, "psycle-plugins", "src"),
             "-I" + os.path.join(root, "psycle-helpers", "src"),
             "-I" + os.path.join(root, "universalis", "src"),
             "-I" + os.path.join(root, "diversalis", "src"),
             "-I" + os.path.join(stk, "include"), "-I" + os.path.join(stk, "modland")]
    if sys.platform == "darwin":    # clang: no GNU unique symbols, narrowing let by, Apple's sincos
        flags.remove("-fno-gnu-unique")
        flags += ["-Wno-c++11-narrowing", "-include", os.path.join(compat, "darwin.h")]
    flags += os.environ.get("BBMP_PSYCLE_CXXFLAGS", "").split()
    # <stk/Stk.h> is what the plugins include
    os.makedirs(os.path.join(stk, "modland"), exist_ok=True)
    link = os.path.join(stk, "modland", "stk")
    if not os.path.islink(link):
        os.symlink(os.path.join(stk, "include"), link)

    projects = {}
    for name in sorted(os.listdir(msvc)):
        if name.endswith(suffix):
            projects[name.split(".msvc")[0].lower()] = project(os.path.join(msvc, name))

    def place(source):
        path = os.path.normpath(os.path.join(msvc, source))
        # the projects have STK's sources next to its headers, and older ones an older STK
        path = re.sub(r"/stk-[0-9.]+/(?:stk|src)/", "/stk-4.5.0/src/", path)
        return path

    helpers = os.path.join(out, ".obj", "libhelpers.a")

    for variant, (name, define) in BEFORE.items():
        if name in projects:
            sources, defines, uses = projects[name]
            projects[variant] = (sources, defines + [define], uses)
            os.makedirs(os.path.join(out, os.path.dirname(variant)), exist_ok=True)

    def build(name):
        sources, defines, uses = projects[name]
        sources = list(sources)
        for lib in uses:
            if lib in LIBRARIES:
                sources += projects[lib][0]
                # a library of a library: the wave tables use the dsp library
                for lib2 in projects[lib][2]:
                    if lib2 in LIBRARIES:
                        sources += projects[lib2][0]
        seen, objects = set(), []
        where = os.path.join(obj, name)
        os.makedirs(where, exist_ok=True)
        for source in sources:
            path = place(source)
            if path in seen:
                continue
            seen.add(path)
            o = os.path.join(where, re.sub(r"[^A-Za-z0-9]+", "_", os.path.relpath(path, root)) + ".o")
            done = subprocess.run(["g++", *flags, *("-D" + d for d in defines), "-c", path, "-o", o],
                                  capture_output=True, text=True, errors="replace")
            if done.returncode:
                errors = [l for l in done.stderr.splitlines() if "error" in l][:3]
                return name, "\n    ".join([os.path.relpath(path, root)] + errors)
            objects.append(o)
        target = os.path.join(out, name + ".so")
        own = [] if sys.platform == "darwin" else ["-Wl,-Bsymbolic", "-Wl,--no-undefined"]
        done = subprocess.run(["g++", "-shared", "-o", target, *objects, helpers,
                               *own, "-lm", "-lpthread"],
                              capture_output=True, text=True, errors="replace")
        if done.returncode:
            errors = [l for l in done.stderr.splitlines() if "undefined" in l or "error" in l][:3]
            return name, "\n    ".join(["linking"] + errors)
        return name, ""

    names = [n for n in projects if n not in SKIP]
    # at most JOBS compilers at once (default 4; one per core filled the memory once)
    with ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS") or 4)) as pool:
        results = list(pool.map(build, names))
    built = [n for n, why in results if not why]
    for n, why in results:
        if why:
            print(f"not built: {n}: {why}", file=sys.stderr)
    print(f"{len(built)} of {len(names)} plugins built", file=sys.stderr)
    return 0 if REQUIRED <= set(built) else 1


if __name__ == "__main__":
    sys.exit(main())
