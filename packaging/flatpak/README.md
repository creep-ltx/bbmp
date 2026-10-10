<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# The Flatpak

`make.sh` makes `bbmp.flatpak`, one file to install on another Linux machine. Every
replayer, helper and library in it is built **inside the Flatpak SDK**
(org.freedesktop.Sdk 26.08), **for any x86-64** (`-O2 -march=x86-64 -mtune=generic`),
**from bbmp-sources**, an archive of the exact source tarballs the build uses
(https://github.com/creep-ltx/bbmp-sources). Nothing comes from the machine that builds
it, and nothing is downloaded while it builds: flatpak-builder gives the build no network.

    git clone https://github.com/creep-ltx/bbmp-sources
    BBMP_SOURCES=$PWD/bbmp-sources packaging/flatpak/make.sh

bbmp-sources holds only what the packages are built from, so it is also where the source
of everything in a package can be had. Where its archive of a source differs from the
upstream one (a few have files taken out: songs a build does not need, and parts that may
not be passed on, which the player downloads from their upstream when asked), its
`SOURCES.md` says what and why.

## How it is built

- `manifest.py` writes `io.github.bytebandit.bbmp.yml` (don't edit that by hand); `make.sh`
  runs it, links `sources` to bbmp-sources, and runs flatpak-builder (`--jobs=4`).
- **The libraries**, as flatpak-builder modules from their release tarballs: libresidfp
  1.2.2, libsidplayfp 3.1.1, libopenmpt 0.8.9, libgme 0.6.5 (into `/app/lib`), and 7-Zip
  26.03 (`7zz`, as `/app/bin/7z`). zlib, libsamplerate, libpulse and `flac` are the
  runtime's.
- **The helpers**, one module per build script: each module gets `tools/sources.sh`, its
  `tools/build-*.sh`, its `csrc/` folder and the archives of bbmp-sources it needs (listed
  with their sha256 in the manifest), and runs the script with `BBMP_SOURCES` pointing at
  them. The scripts check every archive against bbmp-sources' `sha256sums.txt` and unpack
  it instead of cloning or downloading. Their patches and `modland:` markers are the same as
  in a build from source. Each installs into its own folder of `/app/opt`, so the paths the
  helpers keep (UADE's base folder, sc68's replays) are right inside the Flatpak. UADE and
  sc68 are installed with only the Amiga players and replay routines that may be passed
  on; the player downloads the others on request (`amigaplayers`, `sc68replays`).
- **The Python packages** (textual and what it needs, numpy) are wheels kept in bbmp-sources.
  numpy's wheel is manylinux: it chooses its CPU code when it runs.
- flatpak-builder keeps every module that built (`WORK`, default `~/.cache/bbmp-flatpak`):
  after a change only that module and the ones after it are built again.

## What is in the package

Every replayer is in. KDM, Buzzic, JayTrax, PMD, Jesper Olsen and SCC-Musixx play on
`kdm`, `buz`, `jxs`, `pmdc`, `jo` and `sccmusixx`, written for this player in a clean
room (PMD's chip by ymfm, BSD 3-clause). `make.sh` builds with `BBMP_PUBLIC=1`: a helper
that had to leave an engine out would list it in `/app/opt/<helper>/left-out.txt`, and
the player would say "… is not in this package" for its formats (`pipe.left_out`).
CyberTracker's engine is the player's own code: the tracker it runs is downloaded by the
player, not shipped.

## Checked (1 October 2026)

- **Build:** about 11 minutes from nothing on a desktop PC (four compilers at once); the
  bundle is 49 MB, 175 MB installed.
- **No AVX where nothing checks the CPU first** (`objdump` of every ELF in `/app` for ymm
  and zmm registers): only 7-Zip (its SHA/BLAKE2/LZ code, chosen by
  `CPU_IsSupported_AVX2()`), Furnace (the libFLAC it brings along: its AVX2 files, chosen
  by `FLAC__cpu_info`), and numpy with its OpenBLAS and libgfortran (numpy's CPU
  dispatch, OpenBLAS's DYNAMIC_ARCH, libgfortran's matmul variants). SunVox's libFLAC has
  SSE4 code chosen the same way. Everything else: none.
- **On older CPUs** (qemu-user 11.1, `-cpu Westmere` and `-cpu Nehalem`, the runtime as
  `/usr` and the build as `/app`; a test program with AVX2 does stop there with an illegal
  instruction): every helper played a real song, and libopenmpt, libgme, libsidplayfp,
  SunVox and numpy played or computed in the runtime's Python. No illegal instruction.
  Not run under qemu: what UADE plays (uadecore is started by exec, which qemu-user doesn't
  follow); it has no AVX either. **numpy 2.5's wheel needs SSE4.2 and POPCNT**
  (x86-64-v2): on `-cpu core2duo` importing it stops with an illegal instruction, while
  Python and the helpers run. So the package needs a CPU from 2008 on (Intel Nehalem, AMD
  Bulldozer) as it is; a numpy built in the SDK would lift that.
- **The same sound:** 154 real songs (two of each of 79 replayers, 15 s each) rendered by
  the installed helpers here and by the Flatpak: 141 identical. The others: JayTrax and PMD
  (left out of that build; they are in now, on the clean-room `jxs` and `pmdc`),
  CyberTracker, Onyx, SID and two Ad Lib S3Ms in libopenmpt (these
  differ from run to run here too), and one SunVox song (built from source without SSE3:
  at most 2 in 32768 apart).

## The sandbox inside

Inside the Flatpak bubblewrap can't start, so the replayers are shut in by Landlock
through `bbmp-shut`, which is built with the rest.

## What the Flatpak may reach

Only what the player uses (`finish-args` in manifest.py):

- **the network** (the collections) and **sound** (PulseAudio, which PipeWire answers);
- **the Music folder** (`xdg-music`), read and write: kept songs, exports, and songs of
  your own;
- **the Downloads folder** (`xdg-download:ro`), read only: songs you have just downloaded.

Its settings, catalogs and cache are in `~/.var/app/io.github.bytebandit.bbmp/`, which
a Flatpak has without asking. It can't see the rest of the home folder, and it can't
write to `~/.local/bin` (a program put there would run outside the Flatpak). To play
songs from another folder, or keep music outside the Music folder, give it that folder
yourself:

    flatpak override --user --filesystem=/path/to/your/songs:ro io.github.bytebandit.bbmp
    flatpak override --user --filesystem=/path/to/the/kept/folder io.github.bytebandit.bbmp

With no flatpak installed here, `build-tools.sh` builds ostree, flatpak and
flatpak-builder into `~/.local/opt/flatpak`, without root; `make.sh` uses them.

On the machine that runs it:

    sudo pacman -S flatpak          # once (Arch, CachyOS)
    flatpak --user remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
    flatpak --user install bbmp.flatpak      # fetches the runtime from Flathub too
    flatpak run io.github.bytebandit.bbmp

The app id is a placeholder until the player has a home page.

## In menus and launchers, and the `bbmp` command

- **Menu entry and icon:** `io.github.bytebandit.bbmp.desktop` and
  `io.github.bytebandit.bbmp.svg` (the level bars in the player's meter colours, chosen
  1 October 2026) are installed by the `desktop` module. Flatpak exports
  them on install, so the player shows up in app menus and launchers by itself. The entry
  says `Terminal=true`: the player is a terminal program, so the launcher opens the user's
  terminal for it. (Where Flatpak is installed, `/etc/profile.d/flatpak.sh` puts its exports
  on `XDG_DATA_DIRS`; with only the tools from build-tools.sh, launchers don't see them.)
- **The `bbmp` command:** a Flatpak may not put commands on the path, nor write to
  `~/.local/bin`. At its first start inside the Flatpak the player shows, once, the line
  to type in a terminal that makes `~/.local/bin/bbmp`, a two-line script that runs
  `flatpak run io.github.bytebandit.bbmp "$@"` (src/bbmp/command.py). Later:
  `flatpak run io.github.bytebandit.bbmp command` shows it again.
- *Verified 2026-10-01:* the bundle installs with both exported, and the menu entry's Exec
  is rewritten by Flatpak to `flatpak run ... --command=bbmp`. (Then `command` made the
  script itself; since 8 October 2026 it gives the line instead, not yet tried in an
  installed Flatpak.)
