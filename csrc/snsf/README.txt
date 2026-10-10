bbmp-snsf: Super Nintendo songs (SNSF, MINISNSF) for the ByteBandit Music Player
==============================================================================

This program plays a Super Nintendo song and writes the sound to its output, for the
ByteBandit Music Player (bbmp). The player only runs it and reads its output.

It is a program of its own because most of it is Snes9x, which may be used, copied and
passed on for non-commercial purposes only (snes9x-license.txt). Those terms can't be
combined with the GNU GPL, under which the player is, so nothing under the GPL is in
this program, and the files written for it are under the MIT licence (LICENSE).

Build: tools/build-snsf.sh (g++, C++17, zlib).

Where each part comes from
--------------------------

snsf9x, Snes9x trimmed for sound by loveemu, https://github.com/loveemu/snsf9x, commit
128cb0d. It has no licence file of its own; it is Snes9x's code, so Snes9x's licence
(snes9x/docs/snes9x-license.txt, installed as snes9x-license.txt) applies to it:

  snsf9x/SNESSystem.cpp                     the system around the emulation
  snes9x/{cpu,cpuexec,cpuops,dma,globals,   Snes9x's CPU, memory, DMA, PPU, SA-1 and
  memmap,ppu,sa1,sdd1}.cpp, snes9x/apu/     S-DD1, and its sound unit's glue. Unchanged.
  apu.cpp

snes_spc 0.9.0 by Shay Green (blargg), the sound chip, as Snes9x carries it, under the
GNU Lesser GPL version 2.1 or later (snes9x/apu/license.txt, installed as
snes_spc-LGPL-2.1.txt): snes9x/apu/SNES_SPC*.cpp, SPC_DSP.cpp, SPC_Filter.cpp. Unchanged.
Its full source is published with the player's, so it can be changed and linked again.

psflib by Christopher Snowhill, MIT (psflib-LICENSE.txt), commit 4d957c2 of
https://github.com/kode54/psflib (the copy in kode54/deadbeef_hq): reads the SNSF file
and the libraries it names. Unchanged.

zlib (the system's), which psflib unpacks the songs with.

Written for bbmp, under the MIT licence (LICENSE):

  bbmp-snsf.cpp      the program: reads the song's tags, plays it for its length and
                     fades it out, as raw stereo after one line of JSON
  snsf_engine.cpp/.h loads the song's ROM and SRAM through psflib and runs snsf9x

Until 7 October 2026 this was part of bbmp-chip; the 40 songs the player's format
census plays render byte for byte the same in both.
