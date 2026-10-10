bbmp-vgm: VGM and S98 files for the ByteBandit Music Player
===========================================================

This program plays a VGM or S98 file (gzip-packed .vgz too) and writes the sound to its
output, for the ByteBandit Music Player (bbmp). The player only runs it and reads its
output.

The player of the files is bbmp's own: reading them, the commands, timing, loops, data
blocks and DAC streams, the chips' setup and the mixing were written from the published
format documents only (the VGM specification 1.71/1.72 and the GD3 1.00 specification of
vgmrips.net, the S98 v2 draft by Mamiya and the S98 v3 specification of 2006-05-15). The
sound chips are emulated by the licensed cores of libvgm. The program as a whole is under
the GNU General Public License version 2 only (GPL-2.0.txt), because libvgm's OPL4 core
(ymf278b.c, a port of openMSX's) is GPL 2 only; bbmp's own files are GPL 2 or later.

Build: tools/build-vgm.sh (gcc, g++, zlib; iconv, which glibc has built in).

Where each part comes from
--------------------------

Written for bbmp, GPL-2.0 or later:

  bbmp-vgm.cpp     the program: the line of JSON, then the sound, 20 ms at a time, round
                   and round, until 10 seconds past the song's length
  songs.cpp        reading VGM and S98 files and playing their commands
  chips.cpp        starting the chips' cores and passing them the writes
  mixer.cpp        bringing each core's output to the output rate, and mixing
  emu_support.c    the three functions the cores call outside themselves (logging,
                   panning, finding a core's function)

The sound chips' cores of libvgm by Valley Bell (https://github.com/ValleyBell/libvgm),
as ZXTune carries it (https://github.com/vitamin-caig/zxtune, commit e36902f, folder
3rdparty/vgm, emu/cores), unchanged, by the licence each file states:

  the MAME cores: c140, c219, c352,     BSD-3-Clause (NOTICES.txt)
  c6280_mame, es5503, gb, iremga20,
  k051649, k053260, k054539, multipcm,
  okiadpcm, okim6258, okim6295, pokey,
  qsound_mame, rf5c68, saa1099_mame,
  scsp, scspdsp, segapcm, sn76496,
  upd7759, x1_010, ymf271, ymz280b
  ym2151, fmopn, fmopl (MAME)           GPL-2.0 or later (GPL-2.0.txt); ymdeltat.c
                                        states nothing and belongs with fmopl/fmopn
  vsu (Mednafen), pwm (Gens/GS)         GPL-2.0 or later (GPL-2.0.txt)
  adlibemu_opl2/3 (DOSBox's AdLibEmu)   LGPL-2.1 or later (LGPL-2.1.txt; stated in
                                        adlibemu_opl_inc.c, which both include)
  emu2149, emu2413 (Mitsutaka Okazaki), MIT (NOTICES.txt; emu2149 and emu2413 state it in
  mikey (laoo)                          their own repositories, digital-sound-antiques)
  ymf278b.c (OPL4, after openMSX)       GPL-2.0 only (GPL-2.0.txt)
  np_nes_apu, np_nes_dmc, np_nes_fds    ported from NSFPlay (Brezza, Brad Smith), whose
                                        author lets anyone reuse its code

The cores are driven through their own interface: libvgm's headers EmuStructs.h,
SoundDevs.h, EmuCores.h, EmuHelper.h, snddef.h, RatioCntr.h, logging.h, panning.h and
the cores' own headers.

Not built in: libvgm's player and glue (player/, utils/, SoundEmu.c, Resampler.c,
panning.c, dac_control.c, the chips' *intf.c), which state no licence; and the cores
that state none: sn76489.c, ym2612.c, scd_pcm.c, Ootake_PSG.c, qsound_ctr.c,
saa1099_vb.c and ws_audio.c. Every chip but the WonderSwan and the ES5505/ES5506 has a
licensed core here; VGMs of those two play without them.

Until 7 October 2026 libvgm (its player too) was part of bbmp-zxtune, then of an earlier
bbmp-vgm. This player was made to sound as that one did: of the 440 VGM and S98 songs of
the player's format census, 424 come out byte for byte the same over their first 60
seconds.

Where it sounds different from the earlier player (checked 7 October 2026):
  - 10 of 102 Mega Drive, 32X and Mega CD logs (all VGM 1.50): the earlier player handled the
    YM2612's frequency latch as older emulators did for some 1.50 files; nothing in the
    files says which, so here the chip behaves as the real one;
  - 3 of 9 Virtual Boy songs and the X68000 song: a write now and then lands one core
    sample apart (-30 to -100 dB);
  - 1 of 40 S98 songs and 1 AY song: small differences after a while;
  - a seek starts from the same place, but chips' running state after it may differ by
    a sample;
  - it plays more: S98's AY-3-8910 device (silent before), VGMs with a WonderSwan chip
    in them (refused before; the WonderSwan stays silent), and damaged files end where
    they stop making sense instead of reading on.
