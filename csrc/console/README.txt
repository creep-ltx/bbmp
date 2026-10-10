bbmp-console: WonderSwan and PlayStation SPU rips for the ByteBandit Music Player
================================================================================

This program plays Bandai WonderSwan rips (.wsr) and PlayStation SPU logs (.spu) and
writes the sound to its output, for the ByteBandit Music Player (bbmp). The player only
runs it and reads its output.

Licence: the program as a whole is under the GNU General Public License version 2 only
(licences/GPL-2.0.txt in the player's source), because four of Mednafen's WonderSwan
files that are built into it (wswan/main.cpp, memory.cpp, gfx.cpp, tcache.cpp) are
"GPL version 2" with no "or later". bbmp's own files here are GPL 2 or later, so they
can go with those (and stay usable in GPL 3 programs on their own).

Build: tools/build-console.sh (gcc, g++; iconv, which glibc has built in).

Where each part comes from
--------------------------

Written for bbmp, GPL-2.0 or later (none of it taken from elsewhere):

  bbmp-console.cpp   the program: the line of JSON, then the sound
  engine.h           what an engine offers to the program
  wsr_engine.cpp     the WonderSwan engine: finding the songs, their loops and ends,
                     skipping the time a song waits for the next interrupt
  spu_engine.cpp     the SPU engine: reading the dump and log, and playing the log
  resample.h         a windowed-sinc resampler for the SPU's 44100 Hz

Mednafen's WonderSwan emulation, as rePlayer (Arnaud Neny,
https://github.com/arnaud-neny/rePlayer, commit e3bf02c,
source/Replays/WonderSwan/mednafen) carries it after foo_input_wsr, unchanged:

  wswan/main.cpp, memory.cpp, gfx.cpp,    GPL 2 only (Dox; Mednafen Team;
  tcache.cpp                              Alex Marshall)
  wswan/sound.cpp, interrupt.cpp,         GPL 2 or later (Mednafen Team)
  error.cpp, and the headers error.h,
  math_ops.h, wswan/sound.h, rtc.h, ...
  wswan/v30mz.cpp (the NEC V30MZ CPU)     "may be used for purposes both commercial and
                                          noncommercial if you give the author, Bryan
                                          McPhail, a small credit": this is that credit
  sound/Blip_Buffer.cpp (Shay Green)      LGPL 2.1 or later

Headers without a notice of their own are under that folder's COPYING (GPL 2).
rePlayer's own wsr_player.cpp is not built in.

The P.E.Op.S. PlayStation SPU (Pete Bernert; gauss_i.h Chris Moeller), from the Audio
Overload SDK (https://github.com/nmlgc/aosdk, commit e359a6e, eng_psf/peops), unchanged:
GPL 2 or later (its License.txt is the GPL 2). Only the SPU is built, not the SDK's
player for the format. The SPU's code includes two of the SDK's headers, ao.h and
corlett.h (type and function declarations only), which the SDK's license.txt puts under
its BSD licence (R. Belmont and Richard Bannister); none of the SDK's MAME-licensed
(non-commercial) files is built in.

The C and C++ libraries, libm and iconv are the system's (glibc, libstdc++).
