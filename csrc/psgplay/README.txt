bbmp-psgplay: Atari ST SNDH tunes for the ByteBandit Music Player
================================================================

This program plays one tune of an SNDH file and writes the sound to its output, for the
ByteBandit Music Player (bbmp). The player only runs it and reads its output.

It is a program of its own, under the GNU General Public License version 2 only
(GPL-2.0.txt), because it is built on psgplay, which is under the GPL version 2 only and
so can't be built into the player itself (GPL version 3).

Build: tools/build-psgplay.sh (gcc, g++, make).

Where each part comes from
--------------------------

psgplay by Fredrik Noring, https://github.com/frno7/psgplay, commit 6f1c39f, with its
submodules (cf2149, cf68901, cf300588, toslibc). Its library, libpsgplay.a, as its
Makefile builds it, unchanged:

  lib/atari, lib/psgplay, lib/ice,        the Atari ST machine, the SNDH tags, the ICE
  lib/internal, lib/cf2149, lib/cf68901,  unpacker, the YM2149, MFP and sound chips:
  lib/cf300588, system/unix               GPL-2.0 only (GPL-2.0.txt)
  lib/m68k (Musashi, Karl Stenerud)       the 68000: MIT (MIT.txt)
  lib/toslibc (its unicode tables)        LGPL-2.1 (LGPL-2.1.txt)

Written for bbmp, GPL-2.0 only:

  bbmp-psgplay.cpp   the program: unpacks an ICE-packed file with room on both sides
                     (psgplay's unpacker doesn't check where it reads), reads the tags,
                     plays the tune and turns it 12 dB down, as raw stereo after one
                     line of JSON

Until 7 October 2026 this was part of bbmp-chip; the 40 tunes the player's format census
plays (their first two tracks) render byte for byte the same in both.
