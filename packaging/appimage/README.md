<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# The AppImage

`make.py` makes `bbmp-x86_64.AppImage`, one file that runs on its own: download it, make
it runnable (`chmod +x`), start it. Nothing is installed, nothing needs root.

    packaging/flatpak/make.sh          # first: builds everything in the Flatpak SDK
    packaging/appimage/make.py         # then: the AppImage from that build

It is the Flatpak's build, the same replayers, helpers and Python packages, built in
org.freedesktop.Sdk 26.08 for any x86-64 (see ../flatpak/README.md), with what they need
of the Flatpak's runtime brought along: Python 3.14 and its standard library (no tests,
Tk or IDLE), the C library and every other library a program or library in it asks for
(found with the runtime's own loader, `ld-linux --list`), libpulse-simple (the player
loads it by name) and `flac` (for exports).

The licences of what it takes from the runtime go with it: `make.py` copies the
runtime's licence folder (`share/licenses/freedesktop-sdk/<package>`) of each library
and program it brings along into `licences/system/` beside the player's own
(`app/share/licenses/io.github.bytebandit.bbmp/licences`), with a README.txt of which
library is whose. GCC's runtime libraries have no folder there; README.txt names their
licence (GPL 3 with the GCC Runtime Library Exception). A library it can't place is
listed there and by `make.py`, and doesn't stop the build.

## Why it runs on older systems

The runtime's C library is 2.44 (2026), and its Python asks for that version. A program
built against it doesn't start on a system with an older C library (Ubuntu 22.04 has
2.35, Debian 12 2.36). So the AppImage brings its C library along, and every program in
it is made to use it:

- **The loader.** Each program asks for the C library's loader at a fixed place,
  `/tmp/.bbmp-ld-<version>/ld-linux-x86-64.so.2`; AppRun puts a copy of the AppImage's
  loader there at each start. (The AppImage is mounted somewhere new each time, so the
  path can't point into it. AppImages of the same C library version share the copy.)
  /tmp is shared by every user, so AppRun makes the folder 0700 and uses it only when it
  is a real folder of the user's own that nobody else may write to; else another user
  could swap the loader for one of theirs. On a computer where another user has run the
  AppImage first, the folder is theirs, and AppRun says so and stops: the path is written
  into every program when the AppImage is made, so it can't carry the user's number.
- **The libraries.** Each program and library has a search path relative to where it
  is (`$ORIGIN/...`, as DT_RPATH, which also holds for what its libraries need), to the
  AppImage's libraries and the helpers' own.

So the programs are started as they are, not through the loader as a command: they
still know where they are (several helpers find their data beside themselves through
/proc/self/exe), and nothing is put in the environment that the system's programs would
pick up (the browser `w` opens, bubblewrap).

## What else AppRun does

- `BBMP_OPT` to the helpers inside, `PYTHONHOME` and `PYTHONPATH` to its Python.
- `BBMP_BUNDLE`: the AppImage's folder and the loader's, which the replayers' sandbox
  shows them (an AppImage runs from under /tmp, which a sandbox otherwise hides).
- `SSL_CERT_FILE`: the system's certificates, wherever the distribution keeps them.
- `APPIMAGE` (set by the AppImage's runtime): the "make the bbmp command" offer makes
  `~/.local/bin/bbmp` start that file.

## Tools

patchelf (0.18, from github.com/NixOS/patchelf releases) and appimagetool (from
github.com/AppImage/appimagetool releases), in `~/.cache/bbmp-appimage/tools` (`TOOLS`):

    mkdir -p ~/.cache/bbmp-appimage/tools && cd ~/.cache/bbmp-appimage/tools
    curl -sSLo appimagetool https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x appimagetool
    curl -sSL https://github.com/NixOS/patchelf/releases/download/0.18.0/patchelf-0.18.0-x86_64.tar.gz | tar xz

## Known limits

- `/tmp` must allow running programs (not mounted `noexec`), for the loader's copy.
- The AppImage runtime needs FUSE to mount itself; without it, start it with
  `--appimage-extract-and-run`.
