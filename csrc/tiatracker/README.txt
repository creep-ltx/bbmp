bbmp-tiatracker: TIATracker songs (.ttt, Atari 2600 music) for the ByteBandit Music Player
=========================================================================================

This program plays a TIATracker song and writes the sound to its output, for the
ByteBandit Music Player (bbmp). It is a program of its own, under the GNU General
Public License version 2 (license.txt), because most of it is TIATracker's own code,
which is under the GPL version 2 only and so can't be built into the player itself
(GPL version 3). The player only runs it and reads its output.

Build: tools/build-tiatracker.sh (g++, C++17, nothing else).

Where each part comes from
--------------------------

TIATracker, (c) 2016-2017 Andre "Kylearan" Wichmann, GPL-2 ("See the file license.txt";
that file is the GPL version 2). Taken from the copy kept by dionoid,
https://github.com/dionoid/tiatracker, commit f523802160735969f984fc7db01d1ad40e1d5e6d
(21 September 2026); the original is https://bitbucket.org/kylearan/tiatracker.

  track/*.cpp, track/*.h          the song: instruments, percussion, patterns,
                                  sequences, and reading them from the song's JSON.
                                  Unchanged.
  tiasound/tiasound.cpp/.h,       the TIA's waveforms and note names; the pitch guide
  tiasound/pitchguide.h,          headers only because track.h includes them.
  tiasound/instrumentpitchguide.h Unchanged.
  emulation/player.cpp            the replay logic, as the tracker plays a song at 50
                                  or 60 frames a second. Unchanged.
  emulation/player.h              CHANGED for bbmp: three small accessors added
                                  (bbmpPlaying, bbmpSound, bbmpFrame), marked "bbmp:",
                                  so the program can run it a frame at a time and see
                                  when it stops. Nothing else is changed.

Stella, the Atari 2600 emulator, (c) 1995-2015 Bradford W. Mott, Stephen Anthony and
the Stella Team, GPL-2-or-later; as TIATracker carries them:

  emulation/TIASnd.cpp/.h         the TIA's sound. Unchanged.
  emulation/bspf.h                Stella's basic types. Unchanged.

Written for bbmp (GPL-2.0-only, each file says so):

  bbmp-tiatracker.cpp             the program: reads the file, checks what the replay
                                  code would trust, plays the song once from the start
                                  until it ends or comes back to a row it has played,
                                  and mixes the two channels to stereo.
  emulation/SoundSDL2.h           stands in for TIATracker's SDL sound output (which
                                  is Stella's code); it only keeps each frame's TIA
                                  registers. Not Stella's code.
  shim/qtshim.h, shim/qjson.cpp   small stand-ins for the parts of Qt the tracker's
                                  code uses (QString, QList, QMap, QMutex, QObject, the
                                  JSON classes) and a JSON reader. Not Qt's code.
  shim/Q*, shim/SDL.h             one-line headers that include qtshim.h.
  shim/mainwindow.h,              stand in for the tracker's windows: the song file
  shim/instrumentstab.h,          version (1) and the longest names (64), and a place
  shim/percussiontab.h            for the song code's messages.

How it was checked (1 October 2026): the TIA registers this program writes, frame by
frame, are the same as those of TIATracker's unmodified code built with Qt 5, for all
134 different songs tried (the 64 in TIATracker's songs folder, ASMA's 70), over up
to 30 minutes each.
