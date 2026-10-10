bbmp-rv: Retrovert's playback plugins and an iXalance player, for the ByteBandit Music Player
=========================================================================================

This program plays a song and writes the sound to its output, for the ByteBandit Music
Player (bbmp). The player only runs it and reads its output. It loads one of Retrovert's
playback plugins (Klystrack), or plays an iXalance song (.ixs) with its own engine,
which is built in.

It is a program of its own, under the MIT licence (LICENSE), with nothing under the GNU
GPL in it: it links only the C and C++ runtime libraries and zlib, and the plugin is
built from the parts below.

Build: tools/build-retrovert.sh (cmake, g++, zlib).

Where each part comes from
--------------------------

Retrovert, Daniel Collin's player, https://github.com/RetrovertApp, one repository per
plugin:

  playback-klystrack, commit aad080d: klystron, the sound engine of Klystrack by Tero
  Lindeman (kometbomb), MIT (klystrack-LICENSE.txt); the plugin's own code is MIT too.

  The plugin interface (the include/retrovert headers), RetrovertApp, MIT
  (retrovert-LICENSE.txt; LICENSE.retrovert-api here).

zlib (Jean-loup Gailly and Mark Adler, the zlib licence), the system's, linked to unpack
iXalance songs.

Written for bbmp, under the MIT licence (LICENSE):

  bbmp-rv.cpp        the host: loads the plugin (or starts the iXalance engine), opens
                     the song, writes one line of JSON about it and then the sound as
                     raw stereo
  ixs_engine.cpp,    the iXalance engine: both of the format's sample generators, its
  ixs_engine.h       tracker and mixer, at 44100 Hz. Written in a clean room, from a
                     description of the format and of what the earlier player did; it
                     gives the same sound sample for sample. Shortcut Software's player,
                     and Juergen Wothke's iXalance plugin that bbmp used before, are not
                     used and nothing of them is in this program.

iXalance: running it
--------------------

  bbmp-rv ixalance <file.ixs> <track> <samplerate> [start-ms]

The first argument "ixalance" (or any path whose file name is ixalance_playback.so, the
plugin this replaces; no such file is needed) selects the built-in engine.

Seven of the 24 known songs also use three short drum sounds that came with Shortcut
Software's player (909OH.WAV, 909SD.WAV, CRASH.WAV). They are not part of this program.
Put them in a folder and name it in BBMP_IXS_DATA; each file is checked by its size and
SHA-256. A song that needs one that is not there (or not the right one) is refused with
a line that says so; the other songs play without the folder.

  BBMP_IXS_DATA=<folder>  the folder with 909OH.WAV, 909SD.WAV and CRASH.WAV
  BBMP_IXS_DUMP=<prefix>  also writes <prefix>.eng.raw (the engine's own 44100 Hz
                          output), <prefix>.samples (the samples the generators made)
                          and <prefix>.ticks (the channels' state on every tick)

The engine is exact (the same output as its reference recordings) when built without
fused multiply-add (-ffp-contract=off) and run with glibc's sin, pow and fmod.
