<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# The licences of the parts the replayers are built from

The player's own code is GPL-3.0-or-later ([COPYING](../COPYING)). The replayers are
programs of their own, built from the player's code and from the free software below. The
packages carry this folder, so every notice those licences ask for goes with the program.
[docs/replayers.md](../docs/replayers.md) says which replayer uses what.

Where a part is under the GPL or LGPL, the table says which versions: "GPL 2 or later"
can go into a GPL 3 program, "GPL 2 only" can't, which is why the four GPL-2-only parts
(psgplay, TIATracker, libvgm and Mednafen's WonderSwan) are in programs of their own under the GPL 2.

| Part | Licence | Text | In |
|---|---|---|---|
| Arkos Tracker 3 (Julien Nevo) | MIT | [arkostracker3.txt](arkostracker3.txt) | bbmp-ay-at3 |
| Arkos Tracker 3's LZH unpacker (ST-Sound's, Arnaud Carré; after Haruhiko Okumura and Kerwin F. Medina) | BSD-style | [arkostracker3-lzh.txt](arkostracker3-lzh.txt) | bbmp-ay-at3 |
| JUCE 7 (Raw Material Software): only its juce_core, juce_events and juce_audio_basics modules, which are under the ISC licence | ISC | [juce-LICENSE.md](juce-LICENSE.md) | bbmp-ay-at3 |
| Ayumi (Peter Sovietov) | MIT | [ayumi.txt](ayumi.txt) | bbmp-ay |
| Ay_Emul's Flash Tracker and FXM players (Sergey Bulba), ported | free to use, crediting the author | [ay_emul.txt](ay_emul.txt) | bbmp-ay |
| ZXTune (Vitamin) and what of its `3rdparty` folder is built in | LGPL 3; the programs as a whole GPL 3 | [zxtune/](zxtune/README.md) | bbmp-zxtune, bbmp-ay-vt2 |
| bbmp-vgm: the player's own VGM and S98 player, with the sound chip cores of libvgm (Valley Bell) as ZXTune's sources carry them | bbmp-vgm's own files GPL 2 or later; the cores: the OPL4 (after openMSX) **GPL 2 only**, MAME's YM2151, OPN and OPL, Mednafen's VSU and Gens' PWM GPL 2 or later, DOSBox's AdLibEmu LGPL 2.1 or later, emu2149, emu2413 and mikey MIT, MAME's others BSD 3-clause, NSFPlay's NES cores reusable by its author's word. libvgm's player and glue, which state no licence, are not in it (since 7 October 2026) | [GPL-2.0.txt](GPL-2.0.txt), [LGPL-2.1.txt](LGPL-2.1.txt), [libvgm-notices.txt](libvgm-notices.txt) | bbmp-vgm, a program of its own under the GPL 2 only |
| emu2212, emu2413, emu8950 (Mitsutaka Okazaki) | MIT | [emu2212.txt](emu2212.txt), [emu2413.txt](emu2413.txt), [emu8950.txt](emu8950.txt) | bbmp-retro (emu2212), bbmp-fac (emu2413, emu8950), bbmp-chip (inside libkss) |
| libkss (Mitsutaka Okazaki) | ISC | [libkss.md](libkss.md) | bbmp-chip |
| ymfm (Aaron Giles) | BSD 3-clause | [ymfm.txt](ymfm.txt) | bbmp-chip |
| 98fmplayer (Takamichi Horikawa) | BSD 2-clause | [fmplayer.txt](fmplayer.txt) | bbmp-chip |
| organya.h (Strultz): its code. Its sound bank (`default.wdb`: Studio Pixel's 100 waveforms and OrgMaker's 42 drum sounds) has no terms of its own and is to be downloaded (`bbmp download organyabank`), not shipped (below) | zlib | [organya.txt](organya.txt) | bbmp-chip |
| psflib (Christopher Snowhill) | MIT | [psflib.txt](psflib.txt) | bbmp-chip, bbmp-snsf |
| QSoundCore (Highly Quixotic) | GPL 3 | [qsoundcore.txt](qsoundcore.txt) | bbmp-chip |
| StSound (Arnaud Carré) | MIT | [stsound.txt](stsound.txt) | bbmp-chip |
| libpxtone (Pixel) | free to use and change | [libpxtone.txt](libpxtone.txt) | bbmp-chip |
| sc68 (Benjamin Gerard) | GPL 2 or later; installed with its own six replay stubs only (below) | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-chip |
| mdxmini | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-chip |
| EUPPlayer | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-chip |
| AdPlug, libbinio | LGPL 2.1 or later | [LGPL-2.1.txt](LGPL-2.1.txt) | bbmp-chip, bbmp-misc |
| psgplay (Fredrik Noring) | **GPL 2 only**; its 68000 (Musashi) MIT, its toslibc tables LGPL 2.1 | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-psgplay, a program of its own under the GPL 2 only |
| snsf9x (loveemu), which is Snes9x | Snes9x's licence: non-commercial use only | [snes9x.txt](snes9x.txt) | bbmp-snsf, a program of its own with no GPL code in it |
| snes_spc (Shay Green), as Snes9x carries it | LGPL 2.1 or later | [LGPL-2.1.txt](LGPL-2.1.txt) | bbmp-snsf |
| bbmp-snsf's own files (the player's authors) | MIT | [bbmp-snsf.txt](bbmp-snsf.txt) | bbmp-snsf |
| GBSoundSystem | MIT | [gbsoundsystem.txt](gbsoundsystem.txt) | bbmp-retro |
| VIC-TRACKER's player (Daniel Kahlin) | BSD | [vic-tracker.txt](vic-tracker.txt) | bbmp-retro |
| T'SoundSystem (Takashi Toyoshima), ported | BSD 3-clause | [tss.txt](tss.txt) | bbmp-retro |
| NerdTracker II's replay routine (Michel Iwaniec, Bananmos) | giftware, **non-commercial use only** | [nerdtracker2.txt](nerdtracker2.txt) | bbmp-retro |
| Mariopants (Brad Smith), its format and way of playing, and its note recordings | free to change and pass on, crediting the author | [mariopants.txt](mariopants.txt) | bbmp-retro |
| BoyScout (Christer Andersson): its songs played as its GBA library plays them | its terms of usage | [boyscout.txt](boyscout.txt) | bbmp-retro |
| Game Music Emu (Shay Green; libgme), built with its Nuked YM2612 | LGPL 2.1 or later | [LGPL-2.1.txt](LGPL-2.1.txt) | the player, bbmp-retro |
| ProTrekkr (Franck Charlet) | BSD 2-clause | [protrekkr.txt](protrekkr.txt) | bbmp-synth |
| libpac (Thomas Pfaff) | ISC | [libpac.txt](libpac.txt) | bbmp-synth |
| Skale Tracker loader (Luke Preiner) | BSD 2-clause | [skale-skm-loader.txt](skale-skm-loader.txt) | bbmp-synth |
| mda VST plug-ins (Paul Kellett) | MIT | [mda-vst.txt](mda-vst.txt) | bbmp-synth |
| libopenmpt (OpenMPT Project) | BSD 3-clause | [libopenmpt.txt](libopenmpt.txt) | bbmp-synth, bbmp-retro, bbmp-pc, bbmp-xmp, the player |
| libmikmod | LGPL 2 or later | [LGPL-2.1.txt](LGPL-2.1.txt) | bbmp-pc |
| PlayerPRO (Antoine Rosset) | public domain | [playerpro.txt](playerpro.txt) | bbmp-pc |
| libxmp (Claudio Matsuoka and Hipolito Carraro Jr) | MIT | [libxmp.txt](libxmp.txt) | bbmp-xmp |
| The Ensoniq 5503 after MAME (R. Belmont) | BSD 3-clause | [mame-es5503.txt](mame-es5503.txt) | bbmp-iigs |
| NostalgicPlayer's Musicline tables (Thomas Neumann) | MIT | [nostalgicplayer.txt](nostalgicplayer.txt) | bbmp-musicline |
| The Musicline Editor replay routine (Conny Cyreus, Christian Cyreus, John Carehag, Jimmy Fredriksson), ported | LGPL 2.1 or later | [LGPL-2.1.txt](LGPL-2.1.txt) | bbmp-musicline |
| WonderSwan replayer (Arnaud Nény) | free to use | [wonderswan.txt](wonderswan.txt) | bbmp-console |
| Mednafen's WonderSwan: its wswan/main.cpp, memory.cpp, gfx.cpp and tcache.cpp; its sound.cpp, interrupt.cpp and error.cpp; Blip_Buffer (Shay Green); the V30MZ CPU (Bryan McPhail) | main, memory, gfx, tcache **GPL 2 only**; sound, interrupt, error GPL 2 or later; Blip_Buffer LGPL 2.1 or later; the V30MZ free to use with a credit to Bryan McPhail (this is it) | [GPL-2.0.txt](GPL-2.0.txt), [LGPL-2.1.txt](LGPL-2.1.txt) | bbmp-console, a program of its own under the GPL 2 only |
| P.E.Op.S. PlayStation sound, from the Audio Overload SDK (with two of the SDK's headers, BSD) | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-console |
| bbmp-console's own files (the player's authors) | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-console |
| libnezplug | public domain or MIT | [libnezplug.txt](libnezplug.txt) | bbmp-nez |
| GoatTracker 1 and 2 (Lasse Öörni) | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-misc |
| libsidplayfp, reSID, reSIDfp | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | the player, bbmp-misc, bbmp-retro |
| UADE | the GPL (its UAE files name no version, so any may be chosen); its sound core LGPL; the 18 Amiga players it is installed with each under their own terms (below) | [GPL-2.0.txt](GPL-2.0.txt), [LGPL-2.1.txt](LGPL-2.1.txt) | the player, bbmp-amiga |
| bencodetools (Heikki Orsila) | BSD 2-clause | [bencodetools.txt](bencodetools.txt) | UADE |
| libzakalwe | BSD 2-clause | [libzakalwe.txt](libzakalwe.txt) | UADE |
| ScummVM's MaxTrax player and Paula | GPL 3 or later | [COPYING](../COPYING) | bbmp-amiga |
| TIATracker (Andre "Kylearan" Wichmann) | **GPL 2 only** | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-tiatracker, a program of its own under the GPL 2 only |
| Stella's TIA sound (Bradford W. Mott, Stephen Anthony and the Stella Team) | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-tiatracker |
| CHIPNSFX (César Nicolás-González, CNGSoft) | GPL 3 or later | [COPYING](../COPYING) | bbmp-chipnsfx |
| Psycle (its player and native plugins) | GPL 2 or later | [GPL-2.0.txt](GPL-2.0.txt) | bbmp-psycle |
| The Synthesis ToolKit (Perry R. Cook and Gary P. Scavone), in Psycle's plugins | MIT-style | [stk.txt](stk.txt) | bbmp-psycle |
| DarkWave Studio (Daniel Werner) | GPL 3 or later | [COPYING](../COPYING) | bbmp-darkwave |
| Furnace (tildearrow) and the libraries built into it | GPL 2 or later; its libraries their own | [furnace/](furnace/README.md) | furnace, run by bbmp-furnace |
| The SunVox library (Alexander Zolotov) | free to use, with the credit below and its four texts | [sunvox/](sunvox/LICENSE.txt) | the player (sunvox.so) |
| The iXalance engine (`csrc/retrovert/ixs_engine.cpp`, the player's authors; written in a clean room from a description of the format, since 7 October 2026; Juergen Wothke's iXalance plugin, CC BY-NC-SA 4.0, is no longer used) | MIT | [bbmp-rv.txt](bbmp-rv.txt) | bbmp-rv |
| zlib (Jean-loup Gailly and Mark Adler), the system's, to unpack iXalance songs | zlib | the system's | bbmp-rv |
| Klystrack's klystron (Tero Lindeman) and its Retrovert plugin | MIT | [klystrack.txt](klystrack.txt) | bbmp-rv |
| Retrovert's plugin interface (RetrovertApp) | MIT | [retrovert-api.txt](retrovert-api.txt) | bbmp-rv |
| bbmp-rv's own files (the player's authors) | MIT | [bbmp-rv.txt](bbmp-rv.txt) | bbmp-rv, a program of its own with no GPL code in it |
| MGSDRV (Ain, Gigamix): "MGSDRV (C) Ain./Gigamix" | free to embed unchanged (libkss unmodified counts as unchanged), with this credit, no fee | [mgsdrv.txt](mgsdrv.txt) | bbmp-chip, through libkss |
| MoonBlaster 1.4's replayer (Moonsoft) | freeware since 3 June 2012, with its source | [moonblaster.txt](moonblaster.txt) | bbmp-chip, through libkss |
| StoneTracker's replay routine (Emmanuel Marty, Michael Lavaire) | the archive may be passed on with its readme; not open source | [stonetracker.txt](stonetracker.txt) | bbmp-amiga (in `share/stonetracker/`, with the archive and readme whole) |
| sse2neon 1.8.0 (SSE2NEON Contributors) | MIT | [sse2neon.txt](sse2neon.txt) | bbmp-darkwave, the Mac (ARM) build only |
| 7-Zip (Igor Pavlov), built without its RAR code (`DISABLE_RAR=1`) | GNU LGPL 2.1 or later; its LZFSE and Zstandard decoders BSD 3-clause, its XXH64 BSD 2-clause; some files public domain. Its RAR code, the only part under the unRAR restriction, is not built in | its own License.txt, installed with it (`share/licenses/7zip/`) | the Flatpak and the AppImage (`7zz`, to open archives) |

The GPL 2 and LGPL 2.1 texts are [GPL-2.0.txt](GPL-2.0.txt) and
[LGPL-2.1.txt](LGPL-2.1.txt); the GPL 3 is [COPYING](../COPYING).

**SunVox:** Powered by SunVox (modular synth & tracker)
Copyright (c) 2008 - 2026, Alexander Zolotov <nightradio@gmail.com>, WarmPlace.ru.
Its other texts, which its licence asks to go with it, are in [sunvox/](sunvox/):
[sundog.txt](sunvox/sundog.txt) (the SunDog engine, MIT),
[libflac.txt](sunvox/libflac.txt) (libFLAC, BSD 3-clause) and
[tremor.txt](sunvox/tremor.txt) (Tremor, BSD 3-clause).

psgplay (GPL 2 only) and Snes9x (non-commercial) may not be in one program with GPL 3
code, so since 7 October 2026 they are helpers of their own, `bbmp-psgplay` and
`bbmp-snsf`, each with its licences and a README.txt of where each part comes from in its
`share/` folder; so is `bbmp-rv` (MIT, with Klystrack's plugin and the player's own
iXalance engine). ZXTune's sources
bring UnRAR (whose licence forbids using it to remake RAR's compressor) and vgmstream
(whose `relic_mixfft.c` is for non-commercial use only): `bbmp-zxtune` and `bbmp-ay-vt2`
are built without them (`csrc/zxtune/no-rar-vgmstream.patch`), so neither is in those two
helpers. Nor is unRAR's code in the packages: the 7-Zip that the Flatpak and the AppImage carry to
open archives is built without its RAR decoders (since 8 October 2026), which 7-Zip's
licence puts under the same unRAR restriction; so RAR archives don't open there. Nor Sony's
PlayStation 2 BIOS, which ZXTune builds into its PSF and PSF2 player (Highly
Experimental) as a trimmed image: `csrc/zxtune/no-ps2-bios.patch` leaves it out of
`bbmp-zxtune` and `bbmp-ay-vt2` (since 8 October 2026), and no package or download of
the player carries it; who has a PlayStation 2 provides the image made from their own
console's BIOS in the extras folder (`bbmp extras`). Nor Z80~Ex (GPL 2 only), which only ZXTune's AY Emul player used
(`csrc/zxtune/no-z80ex.patch`; Game Music Emu plays AY Emul songs), nor libvgm
(`csrc/zxtune/no-libvgm.patch`): its OPL4 is GPL 2 only and some of its chips state no
licence, so VGM and S98 are played by `bbmp-vgm`, a program of its own under the GPL 2
only: a VGM and S98 player written for bbmp from the formats' published documents, with
libvgm's licensed chip cores only (none of libvgm's player or glue, which state no
licence, and none of the cores that state none: Maxim's SN76489, Gens' YM2612 and
RF5C164, Ootake's PC Engine, superctr's QSound, Valley Bell's SAA1099 and the
WonderSwan; other cores play all those chips but the WonderSwan), with its licences,
notices and a README.txt in `share/bbmp-vgm/`.

Four of Mednafen's WonderSwan files (`wswan/main.cpp`, `memory.cpp`, `gfx.cpp`,
`tcache.cpp`) are GPL 2 only, so bbmp-console, which builds them in, is a program of its
own under the GPL 2 only, as bbmp-vgm is: its own files are GPL 2 or later (since
8 October 2026), and every other part of it is GPL 2 or later, LGPL 2.1 or later, BSD or
free with a credit. `csrc/console/README.txt` says where each part comes from.

The packages made as an AppImage and for the Mac also carry libraries of the system they
were made on (the C library, Python's, sound, compression); their licence files are in
`licences/system/` of those packages.

MGSDRV: "MGSDRV (C) Ain./Gigamix". It is embedded unchanged, as its terms ask
(https://gigamix.hatenablog.com/entry/mgsdrv/, quoted in [mgsdrv.txt](mgsdrv.txt); they
name libkss, used unmodified, as an allowed way to embed it). MoonBlaster 1.4 was made
freeware by Moonsoft (https://www.msx.org/node/39448, quoted in
[moonblaster.txt](moonblaster.txt)). Both are Z80 programs that libkss runs on its
emulated MSX, as data inside bbmp-chip, not code linked with the GPL parts of it; they
stay there by decision, since their terms allow embedding them unchanged.

libkss can build in four more MSX drivers, whose terms don't allow passing them on:
MuSICA's (kinrou5, Keiichi Kuroda), MPK's (mpk106 and mpk103, K-KAZ) and OPLLDRV's
(opx4kss, Mikasen, with Ring's OPLLDriver). bbmp-chip is built without them
(`tools/build-chip.sh` checks it), so no package carries them. The player downloads them
when asked (`bbmp download msxdrivers`, or Downloads in Settings) from libkss's
kss-drivers repository, checked against a SHA-256, and they stay on that computer.

UADE plays Amiga songs by running each format's own replay routine (an Eagleplayer) on
an emulated Amiga. Its `players/` folder holds 176 of them, under "various different
licenses and quite a many different copyright holders" (UADE's COPYING). Only the 18
whose authors' written terms allow passing them on are installed with UADE
(`tools/build-uade.sh` keeps them and fails if any other is left; `tools/build-amiga.sh`
checks bbmp-amiga's players the same way):

| Players | Terms |
|---|---|
| Fred, JochenHippel_UADE, ManiacsOfNoise, MusicAssembler, SIDMon1.0 | UADE's own re-codings of the replays: GNU LGPL (Heikki Orsila, UADE) |
| MusiclineEditor | GNU LGPL |
| PreTracker | LGPL glue (Arnaud Neny) around PreTracker 1.5's replay as Pink/Abyss give it out with the editor |
| ArtOfNoise-4V, ArtOfNoise-8V | the sources are freeware (Bastian Spiegel); the StoneCracker decruncher in them free in non-commercial productions (Marcus Ottosson) |
| MED, Octa-MED, Octa-MED-Ss | "permitted for any free programm" (RBF Software): non-commercial, and run, not linked |
| Medley | free to be used, with his name mentioned (Paul van der Valk) |
| MusicMaker-4V, MusicMaker-8V | BSD (Thomas Winischhofer; the only text is UADE's note, [musicmaker.txt](musicmaker.txt)) |
| Pokeynoise | freeware, source included (Achim Haertel) |
| SoundMon2.0, SoundMon2.2 | given into the public domain (Brian Postma) |

The other 158 (ripped from games and editors and adapted by Wanted Team, DEFECT,
Delirium and others, or given out without written terms) and the `ENV:` files only they
read are not in any package. The player downloads them when asked (`bbmp download
amigaplayers`, or Downloads in Settings) from UADE's own repository on GitLab, at the
commit the helpers are built from, each checked against a SHA-256, and they stay on that
computer.

sc68 comes with 84 replay routines that SC68 files may name instead of carrying their
own code. Only its own six stubs (dwaga, mon, none, note, note2, timerc, by Benjamin
Gerard, under sc68's GPL) are installed (`tools/build-chip.sh` checks it). The other 78,
ripped from games, editors and demos and given to sc68 without written terms, are
downloaded when asked (`bbmp download sc68replays`) from sc68 2.2.1 on SourceForge, each
checked against a SHA-256.

Beepola's engines (Chris Cowley's player and the engines he took from others: SFX, The
Music Box, The Music Studio, Savage, Phaser1, Plip Plop, ROM Beep) are played by a
clean-room rewrite. Three pieces of data they read are fetched, not shipped: the ZX
Spectrum 48K ROM (Amstrad's), Shiru's Phaser1 drum samples (1 KB) and 136 bytes of the
tables of Jason C. Brooke's Savage player. The player downloads them when asked (`bbmp
download beepoladata`, or Downloads in Settings) from Beepola's own repository on GitHub
(atsidaev/beepola, at a fixed commit), cuts them out of the engines' memory images there,
checks each against a SHA-256, and they stay on that computer.

Organya's sound bank, `default.wdb` (436 KB), is what organya.h plays Organya songs
with: 100 waveforms (a version of Studio Pixel's Wave100, close to but not the same as the
`Wave.dat` in OrgMaker's source) and 43 drum sounds, of which 37 are byte for byte and 4
are cut-short copies of the drum WAVs in OrgMaker's source, and 2 are not found there.
OrgMaker 2's source, with its `Wave.dat` and 42 drum WAVs, was released under the BSD
3-clause licence (Rxo Inverse, Studio Pixel, 2018; https://github.com/shbow/organya), but
the bank as organya.h has it comes with no terms of its own (organya.h's zlib licence is
for its code). So it is not to be shipped: the player downloads it when asked (`bbmp
download organyabank`, or Downloads in Settings) from organya.h's repository on GitHub
(Strultz/organya.h, at a fixed commit), checks it against a SHA-256, and hands it to
bbmp-chip (`BBMP_ORGANYA_BANK`). Until bbmp-chip reads that and the package build stops
installing its own copy, `tools/build-chip.sh` still installs one beside the helper.

The iXalance engine is the player's own. Three short drum samples it reads for 7 of
the 24 songs (`909OH.WAV`, `909SD.WAV`, `CRASH.WAV`, 23 KB together) are Shortcut
Software's, from their iXalance player; they are fetched, not shipped. The player
downloads them when asked (`bbmp download ixalancesamples`, or Downloads in Settings)
from the source of the earlier iXalance plugin on GitHub (RetrovertApp/playback-ixalance,
at a fixed commit), reads them out of the C arrays in `src/WaveLibrary.c` there, checks
each against a SHA-256, and they stay on that computer.
