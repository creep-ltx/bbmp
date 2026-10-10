# The player's own replayers

Most formats are played by libraries that exist for them (see the README). For the
formats here there was none that could be used, so each has a helper program of its
own in `csrc/`, built by a script in `tools/`. This page records, for each, where it
comes from, under what terms, how it was checked, and what is known to be wrong.

Nobody has compared any of them by ear with the original machine or program. "Checked"
below means measured against another implementation.

| Helper | Formats | Songs on Modland | Play |
|---|---|---|---|
| `bbmp-musicline` | Musicline Editor | 817 | 815 |
| `bbmp-fac` | FAC SoundTracker | 644 | 644 |
| `bbmp-psycle` | Psycle | 535 | 389 |
| `bbmp-iigs` | SoundSmith, Noise Tracker GS | 243, 71 | 243, 71 |
| `bbmp-nez` | SGC, Gameboy Sound System GBR | 386, 31 | 384, 31 |
| `bbmp-console` | WonderSwan, SPU | 168, 164 | 168, 163 |
| `bbmp-misc` | EdLib Packed, GoatTracker 1 and 2 | 179, 104 | 92, 104 |
| `bbmp-synth` | ProTrekkr and NoiseTrekker (4 formats) | 67 | 67 |
| | JayTrax, Skale Tracker | 35, 30 | 35, 30 |
| | Ken's Digital Music, Buzzic 1 and 2, SBStudio | 13, 16, 6 | 13, 16, 5 |
| `bbmp-pc` | MikMod UNITRK, PlayerPro | 25, 9 | 25, 9 |
| | Digital Sound Interface Kit, Cubic Tiny XM | 7, 2 | 7, 2 |
| | Buzz | 51 | 10 |
| `bbmp-amiga` | MaxTrax, Zoundmonitor | 79, 10 | 79, 10 |
| | Jesper Olsen, Stonetracker | 37, 32 | 37, 32 |
| `bbmp-retro` | SCC-Musixx, NerdTracker 2 | 97, 54 | 97, 54 |
| | Cybertracker C64, Gameboy Tracker, Onyx | 32, 25, 24 | 32, 25, 24 |
| | Westwood SND, Picatune, TSS | 22, 20, 14 | 22, 20, 14 |
| | BoyScout, Vic-Tracker | 12, 11 | 11, 11 |
| | VGM Music Maker | 9 | 9 |
| `bbmp-pcmusic` | Monotone, Digitrekker | 4, 2 | 4, 2 |
| `bbmp-ay` | Beepola, Flash Tracker, Starkos | 105, 66, 65 | 105, 66, 65 |
| | Fuxoft AY Language, AY Amadeus | 39, 33 | 39, 33 |
| | Vortex Tracker II, ArkosTracker, AY STRC | 12, 2, 1 | 12, 2, 1 |
| UADE | Pierre Adane, Synth Pack, PokeyNoise (needed a file each) | 22, 17, 17 | 22, 17, 17 |
| `bbmp-chip` | Ken's AdLib Music (needed its instrument file) | 41 | 41 |
| `bbmp-xmp` | Archimedes Tracker, Coconizer, Megatracker | 99, 57, 6 | 97, 57, 6 |
| `bbmp-adlib` | Pixel Painters FMF and FTF, EdLib D01 | 11, 8, 6 | 11, 8, 6 |
| | Sound Images Generation 2, Sound Interface System | 4, 3 | 4, 3 |
| `bbmp-rv` | iXalance, Klystrack | 24, 17 | 24, 17 |
| `bbmp-chipnsfx` | CHIPNSFX (ZX-Art's songs) | 145 on ZX-Art | 145 |
| `bbmp-tiatracker` | TIATracker (ASMA's Atari 2600 songs) | 70 on ASMA | 70 |
| `bbmp-synder` | Synder SNG-Player, Synder SNG-Player Stereo | 66, 67 | 66, 67 |
| `bbmp-t0ast` | The 0ok Amazing Synth Tracker | 8 | 8 |
| `bbmp-berotracker` | BeRoTracker | 19 | 19 |
| `bbmp-axs` | AXS | 26 | 26 |
| UADE, under a kin's label | SoundMon 1, Digital Mugician 3 | 1, 1 | 1, 1 |
| made into another format | CYM (as VGM), Pollytracker (as a C64 tune) | 1, 5 | 1, 5 |

"Play" is what `tools/try_helper.py` counts on every song Modland has of the format:
the helper gives sound that is neither silence nor noise. For Psycle and EdLib it was
counted by the people who built the helper, on all songs; the others were counted
again after that.

## Musicline Editor

A port to C++ of the original Amiga replay routine (`mlineplayer102.asm`, Musicline
Player 1.15, by Conny Cyréus, Christian Cyréus, John Carehag and Jimmy Fredriksson,
LGPL, as it is in UADE's sources), with the help of NostalgicPlayer's C# port (MIT).
The build fetches one file of tables from NostalgicPlayer. The code in
`csrc/musicline` is a derived work of the LGPL original.

- **Checked:** every write to the Amiga's sound chip, against the original routine
  running in UADE: 1,425,672 of 1,425,672 the same, in 59 songs with 4 channels.
  Tempo, level and song lengths against UADE.
- **Not checked:** the waves the synthesizer effects make (only that the chip is given
  the same ones), and the five 8-channel songs of the test set other than by their
  sound being like UADE's.
- **Differs on purpose:** songs end (UADE plays these for ever); 8 channels are mixed
  cleanly, not with the Amiga's 8-bit tables; no Amiga 500 low-pass filter.
- **Doesn't play:** `Andrew/percubic.ml` (the file is cut short; UADE plays it) and
  `Mark Sheeky/xenex-remix.ml` (silent).

## FAC SoundTracker

A replayer written for this player in a clean room, from a description of the format
only, on the chip emulators emu2413 and emu8950 (MIT) that libkss uses. Versions 1.0,
2.0 and Pro; the drum kits (`<kit>.SM1`, `.SM2`) are found beside the song.

- **Checked:** the same chip writes as before in all 644 songs, register for register
  and tick for tick, and the same sound sample for sample (23.5 hours). Those writes had
  been checked against FAC's own replay routine running on an emulated Z80: the same in
  all 60 test songs, on both chips. Test mode: `BBMP_FAC_LOG=<file>` lists every write.
- **Open:** 50 or 60 Hz. The helper plays at 60; songs written on European machines
  have slower pitch bends at 50, and the file doesn't say (`BBMP_FAC_HZ=50`).
  Version 1.0 songs for MSX-MUSIC use version 2.0's drum numbering, which is a
  judgement. Six songs name a drum kit that is missing and play without drums.
- Other players of the format exist (RoboPlay, FST-Web, rePlayer).

## Psycle

Psycle's own player code (version 1.12.2, revision 12005 of its repository,
GPL 2 or later), built without its windows, and 64 of its native plugins. Some plugins
in Psycle's repository are by third parties under licences of their own, which have
not been read one by one.

- **Plays** 270 songs with every machine there, 85 with effects missing and 34 with
  generators missing that play at most 10% of the notes; those say "Missing: ..." in
  their comment. 146 are refused: they need VST plugins or native plugins whose source
  was never published, or can't be read.
- **Checked:** song lengths against Retrovert's cpsycle. Levels of all songs. Memory
  safety of the song loaders with a sanitizer, on 5500 damaged songs: Psycle's
  loaders trust the file's sizes, counts and tempo, and csrc/psycle/psycle-r12005-damaged.patch
  makes them refuse a song whose numbers the file can't hold (105 real songs play
  byte for byte as before). Playing 3000 damaged songs found one crash left: Pooplog FM
  Laboratory trusts its own saved settings (an oscillator's sync number indexes an array),
  and the helper for that song ends; not fixed, as the plugin's 150 settings have no ranges
  written down.
- **Not checked:** the sound, against Psycle itself. Plugins were changed over the
  years; only the equalizer's change of 2010 is undone for older songs. 13 songs clip.

## SoundSmith and Noise Tracker GS (Apple IIGS)

Replayers written for this player in a clean room, from a description of the formats
only; the same chip writes as before in every song. They play on an emulation of the
Ensoniq 5503 written after MAME's (BSD). Three dialects of SoundSmith: SoundSmith,
MegaTracker, NinjaTracker; a SoundSmith song's wave bank (`<song>.W`) is found beside it.

- **Checked:** the same chip writes as before in all 314 songs, register for register
  and tick for tick, and the same sound sample for sample (15.9 hours); 208 made-up and
  damaged files are played or refused as before. Test mode: `BBMP_IIGS_LOG=<file>`
  lists every write. Before that, tempo and notes had been checked against MTPng
  (SoundSmith) and against the Amiga original of one song (Noise Tracker). Memory
  safety with a sanitizer, on fuzzed songs.
- **Not checked:** anything against a real IIGS or an emulator of one. NinjaTracker's
  10 songs have no reference at all.
- **Terms:** MegaTracker's effect rules are from MTPng (GPL 2 or later), which makes
  the helper GPL.

## SGC and Game Boy GBR

libnezplug (John Regan's library made from NEZplug++, public domain), from a copy of
his repository, which is gone. A patch adds what is needed to find a song's end,
the PAL clock, and cartridge memory, and keeps the program inside the file's data: a
ROM bank the file doesn't have wraps round (libnezplug pointed past its buffer), and a
ColecoVision song is loaded no further than FFFFh (it copied past its buffer).

- **Checked:** SGC against Game Music Emu's SGC player, 746 songs: 84% alike, 14%
  roughly alike, 1% not alike (9 of those not looked into).
- **Not checked:** GBR against anything. Master System songs with FM.
- **Needs:** two of the three ColecoVision songs call the ColecoVision BIOS, which is
  not included: `~/.local/share/bbmp/coleco.rom` (8192 bytes).
- **Subsongs:** an SGC file has a game's sound effects among its songs. Where Modland
  has track lists (110 of 386 files), those name the music. A GBR file doesn't say how
  many songs it has; the helper has a table for Modland's 31 files.

## WonderSwan and SPU

Mednafen's WonderSwan emulation in the form foo_input_wsr uses (four of its files GPL 2
only), and the P.E.Op.S. PlayStation sound emulation from the Audio Overload SDK (GPL 2
or later) with a player for the log written for this player. So `bbmp-console` is a
program of its own under the GPL 2 only; its own files are GPL 2 or later
(`csrc/console/README.txt`).

- **Checked:** SPU against a second emulator, Highly Experimental, in 10 songs.
- **Not checked:** WonderSwan against anything but itself.
- **Known:** SPU reverb differs from the other emulator's, and neither is known to be
  the right one. WonderSwan songs loop; the helper finds the loop and plays it once,
  with a fade.

## EdLib and GoatTracker

EdLib's songs are unpacked (zlib's blast), converted to D00 the way the editor does it,
and played by AdPlug (LGPL). The converter (`csrc/misc/edl_cr.cpp`) was written for this
player in a clean room, from a description of the format only; it gives the same D00
files as before in all 92 songs, and the same sound byte for byte. GoatTracker's songs are played by the editors' own play routines
(GoatTracker 2.77 and 1.53, GPL 2) with reSID.

- **EdLib:** 87 of Modland's 179 files are damaged (they hold pieces of an archive, not
  songs) and are refused. The converter gives the same D00 files as the editor did. A
  damaged song whose D00 would make AdPlug's player loop for ever inside one tick (a
  voice going round its order list without a note) ends at that tick instead.
- **GoatTracker checked:** the values written to the SID, against the C64 programs of
  the same songs in HVSC (a 6502 emulation, call by call). Version 2 songs in the last
  format: 43 of 62 the same.
- **GoatTracker 1 (GTS!):** the editor's play routine in C isn't quite its C64 routine.
  `csrc/misc/gt1_patch.py` changes it (at build time) where they differ: a note's
  pulse width, pulse modulation (step, limits, the direction bit in the low byte), the
  speed of an arpeggio on a row's first frame, and the volume at the start. All 7 tunes
  (5 songs) whose C64 programs were made with the same routine now agree in every
  call as far as it can be heard (before: 5% to 68%). Registers of silent voices still
  differ: the C64 routine reads "instrument 0" out of its own variables. The other
  HVSC programs of GTS! songs were made by other versions (another frequency table:
  1.25 and older) or other routines, and can't agree.
- **GoatTracker 2, GTS2:** the editor up to 2.17 read wavetable values 08-0F as
  waveforms, 2.18 onwards as delays, in the same file format; the editor loads old
  files as they are. The helper reads them as waveforms: 3 of Modland's 13 GTS2 songs
  change, two now agree fully with HVSC (from 26% and 98%). The rest differ in details
  of the old routines (pulse speed, filter resonance) that 2.77's doesn't have, or
  their HVSC programs are of the version 1 songs.
- **GoatTracker open:** a song's file doesn't say at what speed it is played and for
  which SID. The helper has a table for 74 of Modland's songs, made from HVSC.
- **Damaged songs (GoatTracker 2):** the editor's GTS2 import converts rows * 4 rows of each
  pattern, past its end (and past the array for the last of 208); `tools/build-misc.sh`
  patches the loop to the pattern's own rows, which changes no song. A row's instrument
  above 63 (the play routine indexes 64) refuses the song.

## ProTrekkr, JayTrax, Skale and other synth trackers

`bbmp-synth` builds ProTrekkr, Skale and SBStudio from rePlayer's copies of them, with
patches for Linux and for faults in them, and the engines written for this player in a
clean room: `kdm`, `jxs` and `buz`. PMD's `pmdc`, written the same way, is in
`bbmp-chip` (`tools/build-chip.sh`), and is listed here with them.

| Engine | From | Licence |
|---|---|---|
| `protrekkr` | ProTrekkr 2.8.2's replayer and loader | BSD 2-clause |
| `pmdc` | PMD: a driver written for this player from a description of PMD's behaviour (clean room, 1 October 2026), the same register writes as PMDWin in all 1,510 songs; the YM2608 by ymfm | GPL-3.0-or-later; ymfm BSD 3-clause |
| `kdm` | written for this player from a description of the format (clean room, 1 October 2026) | GPL-3.0-or-later |
| `jxs` | JayTrax, written for this player from a description of the format (clean room, 1 October 2026); bit for bit the same as CrossX on all 67 tracks in its test mode, and without CrossX's three faults by default | GPL-3.0-or-later |
| `buz` | Buzzic 1 and 2, written for this player from a description of the formats (clean room, 1 October 2026); bit for bit the same as the old engine on all 16 songs | GPL-3.0-or-later |
| `sbstudio` | libpac 0.9.0 | ISC |
| `skale` | OpenMPT with a loader for Skale's files by lpreiner, which its author calls experimental and written with AI assistance | BSD |

- **Checked:** Buzzic 1.0 sample by sample against the player Buzzic generates. Song
  lengths of ProTrekkr, also across the three versions the same songs exist in. One
  Skale song's length against Skale's own recording. **Skale's effects** against
  Skale's own player (SkalePlayer.dll 0.81 from scene.org, run under Wine with
  plug-ins built for the test): how sends, bus faders, balance and the bus-to-bus
  matrix mix, exactly; mda Delay against the same code in Skale's player; the level
  of the effects next to the dry mix within 0.5 dB for most of the 28 songs.
- **KDM (clean room):** `kdm` was written from a description of the format alone, by
  someone who never saw Ken Silverman's player. With BBMP_KDM_FLAT=1 it is
  bit for bit the same as Ken's C player on all 13 songs; by default it plays the sine
  effects (vibrato, tremolo, auto-pan) of Ken's original DOS player, which the C port
  lost (its sine was always 0): 11 of 13 songs use them. Same pitch and tempo; a few
  songs clip slightly more, as the original's tables swing the volume up to 2x.
- **Not checked:** the sound of ProTrekkr, JayTrax, SBStudio and Buzzic 2 against
  anything. Skale's Freeverb2: a closed plug-in, played with Jezar's Freeverb and
  its parameters guessed (room, damping, predelay (left out), wet, dry, width,
  freeze).
- **Skale effects:** the song is loaded once more for each effect bus, with the
  instruments' sends as their gains (a hook in the loader); the plug-ins are
  rewritten from source: mda Delay, Stereo, Image, ThruZero, RoundPan (MIT) and
  Freeverb (public domain). Songs with effects play at 44.1 kHz, Skale's rate.
  Missing: SChorus (3 songs) and sc Multi Tap Delay (1 song), closed plug-ins; their
  buses stay silent, as in Skale without them.
- **NoiseTrekker:** its songs drive ProTrekkr's mixer 2 to 8 times over full scale.
  NoiseTrekker 1's synth instruments get volume 64 (the loader gave 127; 64 is what
  it gives NoiseTrekker 2 instruments without one), and every NoiseTrekker song's
  master volume is taken down 14 dB. 11 of the 12 songs no longer clip. How loud
  NoiseTrekker itself was is not known: its source is not to be found.
- **Known:** SBStudio plays without the AWE32's effects. KDM songs are cut where they
  start again. Some Skale instruments are louder or softer than in Skale (the
  loader's, up to 4 dB).
- **Doesn't play:** MVX Module (12 songs): only Windows libraries of its player
  exist, no source.
- **Damaged files (review of 2026-09-30):** the replayers read what the files say.
  Checked before they get it, or patched by `tools/build-synth.sh`: ProTrekkr's
  loader reads within the unpacked song, which may be 64 MB (the largest song
  unpacks to 9.5 MB), and refuses a sample longer than what is left of it, a
  song without tempo, patterns the sequence names that aren't there; JayTrax's
  waves, loops, arpeggios, finetunes and notes, its song positions and patterns
  where they are used; libpac's sounds, sheets and instrument numbers, and its
  search for a song's end stops at an hour; waves.kwv of KDM; a Buzzic 1 song
  may be 10 minutes (it is held whole in memory); Skale's effect parameters are
  0 to 1. Every JayTrax song read before its first song position and 28 of 35
  one frame past a sample; those frames are now silence, so 11 songs sound a
  little different (the old values were bytes of the memory allocator).

## MikMod, PlayerPro, DSIK, Cubic Tiny XM, Buzz

| Engine | How | Licence |
|---|---|---|
| `unitrk` | libmikmod 3.3.14 | LGPL 2.1 |
| `playerpro` | PlayerPRO's own driver | public domain |
| `dsm` | rewritten as the later DSIK format, played by libopenmpt | |
| `mxm` | `mxm_engine.cpp`, written for this player in a clean room from a description of the format and of the XM it builds (October 2026); rewritten as XM, played by libopenmpt; the same XM byte for byte (or the same refusal) as the earlier engine for all 3 songs and 168 test files. `BBMP_MXM_XM=<file>` writes the XM it builds there | GPL 3 or later; libopenmpt BSD 3-clause |
| `buzz` | BmxPlay and its five machines | public domain |

- **Checked:** DSIK against the S3M originals of three songs: the same waveform.
  UNITRK against the XM originals of four songs: the same length, alike in sound.
- **Buzz:** 41 of the 51 songs need Buzz machines that exist only as Windows plug-ins
  and are refused by name. The 10 that play are BmxPlay's own demo songs.
- **Damaged files (review of 2026-09-30):** BmxPlay reads what the file says;
  `buzz_engine.c` refuses what would make it read or write outside its tables
  (a second master, machines that feed each other, _xi samples longer than their
  data), and `tools/build-pc.sh` patches three of its machines. PlayerPro's MAD-G
  importer took a finetune of 32 from the stack in `mantra 08 final labyrinth`,
  which sounded different every time it played; now it plays one way. Cubic Tiny
  XM's sample memory is at most 64 KB larger than the file, and samples that would
  add up to far more than it are refused (6,500 damaged files under AddressSanitizer
  and UBSan found nothing, October 2026). PlayerPro's MADK, MADH and MADI
  loaders (no such songs on Modland) are not checked: they trust the file.

## MaxTrax, Zoundmonitor, Jesper Olsen, Stonetracker (Amiga)

| Engine | How | Terms |
|---|---|---|
| `maxtrax` | ScummVM's MaxTrax player, with one fault of it mended (tempo changes started from the wrong tempo) | GPL 3 or later |
| `zoundmonitor` | UADE's player (downloaded on request: `amigaplayers`); the helper puts the song's samples where the player looks for them | as UADE; the player itself has no written terms (not shipped) |
| `jesperolsen` | `jo` (csrc/amiga/jo_engine.cpp), written for this player in a clean room from a description of the games' drivers (LollyPop, Georg Glaxo, Harald Hardtand), on ScummVM's Paula; the two songs that carry their own routine (Guldkornsexpressen) through UADE's player | GPL 3 or later |
| `stonetracker` | StoneTracker's own replay routine, from the free edition of the tracker on Aminet, run by UADE; packed sample banks ("delta Huffman") are unpacked by the helper first, with an unpacker written for this player in a clean room from a description of the format (October 2026) | the archive may be passed on with its readme ("as long as you retain this document"): the package carries both whole, in `share/stonetracker/`; not open source |

- **Checked:** Jesper Olsen: the same Paula register writes as the games' own routines
  under UADE, line for line, in all 55 tracks (the clean-room proof, 1 October 2026);
  106,000 fuzzed songs under AddressSanitizer found nothing. Stonetracker's tempo. That
  every song ends.
- **Not checked:** MaxTrax, Zoundmonitor and Stonetracker against any reference.
- **Mended:** a fault in StoneTracker's routine that made songs stand still for ever
  under UADE is patched in memory when the song is loaded (the glue's correcting code
  was written anew in a clean room from a description of the fault and the change,
  October 2026; it plays all 52 tracks exactly as before, and the new bank unpacker
  gives the same banks as the old one, byte for byte). Five ways a damaged MaxTrax
  file crashed ScummVM's player (none of Modland's songs has them; shown with crafted
  files under AddressSanitizer): a portamento time of 0 (division by zero), a mono
  channel (read past the voices), a patch loaded twice (delete for delete[]), a sample
  longer than 32 KB (an assert of its Paula) and a sample of no octaves (played from an
  empty array; it is made silent).
- **Known:** three of MaxTrax's Dune 2 files start with a sound effect of 0.2 seconds.
  Three Georg Glaxo songs are played with that game's routine, recognised by their
  content.

## SCC-Musixx, NerdTracker 2 and other 8-bit trackers

| Engine | How | Terms |
|---|---|---|
| `sccmusixx` | `smx_engine.cpp`, written for this player in a clean room from a description of the tracker's replay behaviour, on emu2212 | GPL 3 or later; emu2212 MIT |
| `ned` | NerdTracker 2's own NES replay routine with the song, as an NSF, played by Game Music Emu | giftware: "You're entitled to use NT2 & its replay code freely for any non-commercial usage" (its home page, nesdev.org/nt2) |
| `mgb` | `gbt_engine.cpp`, written for this player in a clean room, from a description of the format only; the song compiled into the byte streams the GBSoundSystem driver plays, put with the driver into a GBS image that Game Music Emu plays; the same GBS image as before for every song | GPL 3 or later; GBSoundSystem MIT |
| `cybertracker` | the C64 tracker itself from its disk image, on a 6502 and SID emulation; the engine that loads and starts it was written for this player in a clean room from a description of how to drive the tracker (October 2026); `BBMP_CYBERTRACKER_SIDLOG=<file>` writes every SID write with its cycle there | the engine GPL 3 or later; the tracker "Copyright No Name 2001", no terms (not shipped: the player downloads it) |
| `tss` | a port of toyoshim's TSS | BSD 3-clause |
| `wwsnd` | written after ScummVM's PC speaker driver | GPL 3 or later |
| `onyx` | `jangle_engine.cpp`, written for this player in a clean room, from a description of the format only; the song is made into a module that libopenmpt plays; the same module and, with libopenmpt's dither off, the same sound as before, bit for bit, in all 24 songs | GPL 3 or later; libopenmpt BSD 3-clause |
| `picatune` | `smufi_engine.cpp`, written for this player in a clean room, from a description of the format only; the same sound as before, bit for bit, in all 20 songs; checked against the programs Picatune exports | GPL 3 or later |
| `victracker` | VIC-TRACKER 2.0's own player (assembled with dasm at build time), on a 6502 with a VIC sound emulation written for it | BSD (the tracker); dasm GPL 2 |
| `boyscout` | written after BoyScout 1.0's GBA playback library (BoyScout.c), with a GBA sound unit written for it | the library: free to use in any project |
| `vgemm` | written after VGM Music Maker 1.1's VGM export (vgm_mm.exe -vgm, observed), the log it makes played by Game Music Emu | own code |

- **Checked, write for write against the original:** SCC-Musixx (all 97 songs; the
  clean-room engine of 1 October 2026 also gives the old port's audio byte for byte),
  Gameboy Tracker (24 of 25), NerdTracker 2 (8 of 10 that have an NSF by their
  composer). TSS sample for sample against the author's JavaScript version.
  Picatune sample for sample against the 17 programs BeRo exported from his example
  songs (run in a CPU emulator, sound caught, not played): all 17 the same to the
  last bit of 16-bit sound, given each program's own note base and, for one song,
  the fade-in its program has and its song file doesn't. The player rounds toward
  zero; rounding to nearest had made 6 of them drift away.
- **Known:** Picatune plays notes a semitone lower than 15 of those programs: the
  tracker of 2007, and the two programs BeRo exported last, have the lower pitch.
- **Checked against recordings:** Vic-Tracker against the six MP3s on the tracker's
  home page (five songs by its authors and a compo entry): tempo within 0.1%, pitch
  within 5 cents, the level per octave within 1-2 dB from 62 Hz to 8 kHz. The songs
  end where they go back to their start or repeat step. BoyScout against the WAVs the
  tracker itself renders (BoyScout.exe 1.0 run in Wine, driven by a small program, all
  11 songs, also channel by channel for two of them): tempo the same (the tracker
  counts 60 frames a second, the GBA 59.73), pitch within 5 cents, the level of each
  10 ms of the whole song matching with a correlation of 0.71-0.90 (0.51 for the
  8-second castlevania-boss); the square channels' notes and lengths match sample
  for sample apart from the tracker rounding each period to whole samples.
- **Known:** BoyScout: where the GBA library and the tracker differ (Sound 3's length,
  which the library's second write of the register sets to one second; the library's
  read-modify-write of Sound 1's start bit), the tracker is followed. The tracker
  mixes Sound 3 and 4 some 15-20% quieter than the GBA's channels are here, and plays
  Sound 3 around zero where the GBA's channels are all above it. One song on Modland,
  "inspiration (purple motion)", is an empty song (no patterns).
- **VGM Music Maker** (Shiru, 2011; Mega Drive YM2612 + SN76489), engine `vgemm`: the
  editor itself only exists for Windows and its source is not published, but it
  converts a song to a VGM without a window (`vgm_mm.exe -vgm song.vge`, in Wine): a log
  of every chip write, frame by frame. The engine replays the song the way that export
  does and builds the same kind of VGM in memory for Game Music Emu. Everything it
  knows was measured from the export, on the nine songs and on some 20 test songs made
  for it (every note, every pitch step of slides, every volume at every TL, the PSG
  envelope at every rate and 44 TL/volume pairs, each effect): the F-numbers (a
  semitone table, straight lines between), the PSG periods, the volume column (FM: TL
  = 127 - (127 - TL) x (127 - A[vol]) / 127 on the carriers; PSG: a level of
  4 x vol x (127 - TL) / 127 times the envelope, in 16 steps), a PSG envelope made of
  straight lines (0.0009904 x 1.25^rate a frame), vibrato (a 32-step sine), and the
  export's own habits (only changed registers are written, four key on/off pairs for a
  retrig or delay, at the loop point it forgets what it wrote to the YM2612).
- **Checked, write for write against the export:** all nine songs: 118152 of the 118172
  chip writes the same, in the same frames (eight songs all of them; Trinitron Sunset
  20 differ: a vibrato phase after a key-off, and two sample stops a frame apart).
  Each song ends where the export's VGM ends (the order list played once; the export
  sets a loop point, the player does not loop). Played through Game Music Emu, the
  three songs without samples sound the same as the export's VGM to the bit; with
  samples the difference is 19-38 dB below the music: the samples are resampled
  to the song's PCM quality within 1-3 steps of 8 bits of the export's (its resampler
  is not reproduced exactly), and they start and stop within a few 1/44100 s of its.
- **Not checked:** effects no song uses were checked on test songs only (L, B, E0x-E3x,
  E5x, E6x, EEx, EFx, Axy, Fxy, Q9x, QCx, QDx: all writes the same); V3 files (other
  clocks) were not seen.
- **Cybertracker C64:** the clean-room engine gives the same SID writes, cycle for
  cycle, as the earlier engine in all 37 tracks and 136 runs of test files, with the
  same lengths and header lines; the tracker's own playing is not checked against a
  real C64.
- **Not checked:** Onyx, Westwood SND.
- **Damaged files** (read line by line and fuzzed, 2026-10-01): limits where a small file
  could ask for gigabytes or hours (Westwood SND's walk to the end: 10 minutes; TSS: 16 MB
  of compiled data for all channels; Picatune: 64 levels of XML, 2^27 samples of rows and
  echoes; NerdTracker 2: a halting song past 2^27 frames is streamed; VGM Music Maker:
  32 MB of samples; BoyScout: End within the sequencer's 10000 rows); TSS's envelope
  table and missing numbers no longer read past their ends. No real song changed.
  CyberTracker's and Onyx's renders differ run to run: reSIDfp itself does (a 20-line
  test with fixed writes), and libopenmpt dithers its output with a random seed (Onyx
  measures its loudness without dither, so its gain is the same every run). With
  `BBMP_ONYX_NODITHER=1` dither is off for the song too and every run is the same (the
  test mode the engine is checked in).
- **Known:** NerdTracker 2 plays at 50 Hz. Westwood SND files are PC speaker sounds,
  mostly effects.

## Beepola, Flash Tracker, Starkos, AY STRC and other Spectrum and Amstrad formats

| Engine | How | Terms |
|---|---|---|
| `beepola` | `beepola_engine.cpp`, written for this player in a clean room (7 October 2026) from a description of what Beepola's eight player routines do in time: the song is laid out in memory as Beepola compiles it, and each player is a timed step machine (no Z80 emulation, no memory images) whose speaker changes become samples; the same header, speaker changes and sound as before, bit for bit, in all 105 songs and in 158 of 166 test variants (7 damaged ones are now refused, one longer than 285 s differs in the last bits). Three pieces of data are not part of it: the Spectrum ROM (some drums play its bytes as noise), Phaser1's drum samples and the bytes after the Savage player's frequency table. The player fetches them on first use; the engine reads them from the folder that `BBMP_BEEPOLA_DATA` names (`zx48.rom`, `p1d-samples.bin`, `svg-table-tail.bin`, each checked by size and SHA-256) | GPL 3 or later |
| `fls` | a port of Ay_Emul's Flash Tracker player, on the Ayumi chip emulation | Ay_Emul: free to use with reference to the author; Ayumi MIT |
| `fxm`, `amad` | a port of Ay_Emul's interpreter of Fuxoft's language | the same |
| `sks`, `aks` | Arkos Tracker 3's own loader, player and chip emulation | MIT; it builds with JUCE, which is GPL or commercial |
| `vt2` | the text is rewritten the way ZXTune reads it, and ZXTune plays it | as ZXTune |
| `strc` | `stc_cr.cpp`, written for this player in a clean room, from a description of the format only, with Sound Tracker's table of notes, on Ayumi; the same registers every tick and the same sound as before, bit for bit | GPL 3 or later; Ayumi MIT |

- **Checked:** Fuxoft, against the original Z80 players that Modland's AY rips of the
  same songs hold, register by register: 27 songs the same, 3 differ in a few places,
  9 have no rip to compare with. Flash Tracker: three songs against their STC versions
  (length and pitch). Vortex Tracker II against ZXTune.
- **Not checked:** Beepola and Starkos against any recording.
- **On request:** `bbmp download beepoladata` (or Downloads in Settings) fetches the
  three pieces of data the clean-room Beepola engine reads: the 48K ROM, Shiru's Phaser1
  drum samples and 136 bytes of Savage's tables, cut out of the memory images
  (`p1d.img`, `svg.img`) of Beepola's source at commit 4145966 on GitHub, each checked
  against a SHA-256 (`download_pins.py`). The engine finds them in `BBMP_BEEPOLA_DATA`.
- **Beepola without its data:** a song that needs a missing piece is refused with
  "This song needs Beepola's data (…), which is not here"; the others play as with it.
  Of the 105 songs, 27 need none (all The Music Box, The Music Studio and ROM Beep,
  2 Phaser1 synth drums, 4 Savage, 4 Special FX); 51 Special FX, 3 Phaser1 synth drums
  and 3 Plip Plop need the ROM, the 18 Phaser1 digital drums the samples, 3 Savage the
  table's tail. `BBMP_BEEPOLA_DUMP=<prefix>` writes `<prefix>.edges`, `.ticks`, `.marks`
  and `.layout` (speaker changes, tick times, marks and the end, the laid-out bytes)
  for tests. A few damaged songs that made the old engine run into memory holding no
  program are refused with "the song has notes the engine can't play". The clean-room
  engine was fuzzed under AddressSanitizer and UBSan (7 October 2026): 1500 damaged
  songs with the data and 400 without, nothing found.
- **Checked:** AY STRC ("Mega Mix 1") against ZXTune playing the same song in its
  original Sound Tracker 1.1 file: the same length (107.5 s), no lag, the same notes
  (spectra of 20 ms: median cosine similarity 0.92). The three Plip Plop songs: pitch
  as their notes say (within 0.4 semitone, the engine's own tuning), lengths as rows
  times tempo, drums at their rows, the pitch bend of Konami Pong.
- **Known:** Beepola's sound has the engines' 18 kHz carrier filtered out. The ROM
  Beep engine sounds two octaves above Beepola's other engines, as Beepola itself
  plays it (its note 0 is BEEP 0, middle C); the tones are those of the ROM's BEEP.
- **Doesn't play:** Ace Tracker (57 songs) is for the Atari Falcon's signal processor;
  no replayer or description of it was found.
- **Reviewed** (1 October 2026, code read line by line, damaged copies of the real songs
  run under AddressSanitizer and UBSan, Arkos Tracker and ZXTune without): fixed reads
  past the Spectrum's memory (Fuxoft loaded at $FFFB and up, STRC at $FFFF), a read
  before the file (STRC's author offset), Beepola tempos that overflow, STRC lengths
  of up to 61 days; Arkos Tracker's Starkos reader, which read past the end of a damaged
  file for ever and filled a null instrument, is patched by the build. Every song of
  these formats on Modland renders as before. The clean-room `strc` (4 October 2026) was
  fuzzed again under AddressSanitizer and UBSan.
- **Known:** the engines on Ayumi (fls, fxm, amad, strc) need a sample rate of at least
  about 28 kHz (chip clock / 64; Ayumi refuses lower ones); the player asks for 48 kHz.

## Shroom

Songs made in Mario Paint's composer (63). The engine, written for this player after
Mariopants by Brad Smith, plays them with Mario Paint's instrument sounds, which are
recordings of the game's sounds and Nintendo's. The player doesn't fetch them: the
user puts `notes.wav` of Mariopants 1.1 into the extras folder (see the README).

- **Checked:** all 63 songs sample for sample the same as Mariopants.
- **Terms:** the engine is made from Mariopants 1.1's source (Brad Smith,
  rainwarrior.ca), whose source.txt permits modifying, reusing and redistributing it,
  asking that the author be credited and changes marked; the engine's header does both.

## PiECE (`bbmp-piece`)

A replayer for Aquaplus' P/ECE music driver (36 songs), written anew for this player.
The driver's own source may be used only in programs for the P/ECE, so nothing of it
is in the replayer, and it was not read. What the replayer is made from:

- the format as the P/ECE SDK's documents describe it (musbin.txt, musbin2.txt, the MML
  reference mml.htm) and as the SDK's MML compiler writes it;
- how the driver sounds, measured on its output: the driver was run in the piemu
  emulator (open source) with test songs made for it, and its sound recorded to files.
  Pitch, tempo, volume steps, envelope rates, vibrato, portamento and mixing come from
  those measurements.

- **Checked:** against the driver in the emulator (loudness per 50 ms, mean of the
  differences). Final Fantasy's Prelude within 0.1 dB and alike in sound second by
  second; SAMPLE BGM1 within 1.7 dB; seven of eleven test songs within 0.2 to 1.0 dB,
  the ones with drums or with six parts at once 2.5 to 2.8 dB, the one of held loud
  volumes (V170 and up, which no song uses) 9.9 dB. Volumes
  above 127, the release from them and v+ v- (0xfa) were measured in the review of
  1 October 2026 and fixed: the test song with many volume changes went from 3.7 to
  2.5 dB off, the part of Tora-H's BGM02 Yuko that uses v+ v- from 16.4 to 2.0 dB (the
  only song of the 36 that sounds different). A test here plays an A-4 at 440 Hz for
  its length.
- **Known:** the drums are made anew and differ from the driver's own: the level of
  their first 100 ms is within 3 dB and their length within 15% (at C4, V127), but
  most are brighter (the snares and the clap by 900-1700 Hz of spectral centroid) and
  decay differently, so changing their levels alone did not bring them closer. Also
  measured and left as they are: a release starts one 8 ms frame later in the driver
  (played so, seven test songs and BGM1 and Yuko came closer, one test song and the
  Prelude went further away), decays and sustains
  run about 4% faster there (rates 16, 64 and 96; rate 4 and attack 16 don't move at
  all), a decay to level 0 holds there instead of ending the note, and a loud volume
  held (V170 and up) is the noise of the driver's tables rather than silence. In the
  three real songs recorded from the driver, each of these moves the result by 0.3 dB
  or less.

## Formats that can't be played

22 kinds of song, 531 songs (4 October 2026; Renoise added on 2 October, AXS plays now). For each, a research pass on 1 October
2026 found what exists to play it; `docs/formats.md` says it in a line each.

- **Closed synthesizers**, where the program (or a plug-in of it) can still be had, to
  record references and to write a replayer from what it does: Renoise Old (120 songs;
  53 of 60 looked at use Renoise's built-in effects, 6 use VST; a Linux demo of Renoise
  2.7.2 exists) and Renoise (151; until 2 October 2026 ZXTune "played" them, but what
  it played were the instruments' samples inside the song's zip, one after the other), Picatune2 (39; much of its synthesizer is in BeRo's BR808, LGPL-3),
  DreamStation (28), Aero Studio (16), Tunefish 2 (8), MVX Module (12; compiled
  libraries, CC BY-NC 4.0). (BeRoTracker and AXS were two; they play now, below.)
- **Source published, without a licence or for non-commercial use**: MegaStation (2)
  and its MIDI songs (5), MVS Tracker (2), Beaver Sweeper (4). (The 0ok Amazing Synth
  Tracker was one; it plays now, below.)
- **Program found, replayer to be written**: Drum Traker (15; Drum Blaster 3.0 has all
  the songs and samples), Compoz (4), Athtune (3), Cybertracker for the Amiga (1), GT
  Game Systems (7; the game Carver).
- **Hardware the player doesn't have**: Ace Tracker (57, the Falcon's DSP; its makers'
  replay routine would run in Hatari), All Sound Tracker (20, the AWE32; its author's
  format description is on archive.org) and RamTracker (9, General MIDI).
- **Synder Tracker** (24): the tracker for the synthesizer the Synder SNG files play
  on (those play now, below); its patterns and instruments are not worked out.
- **Not worth it alone**: SVAr Tracker (3, zips locked with a password, songs that may
  use others' plug-ins), AND XSynth (1).

## CHIPNSFX (`bbmp-chipnsfx`)

César Nicolás-González's (CNGSoft) three-channel AY tracker for the CPC, Spectrum and
MSX; its songs are text. ZX-Art has 145 (its "CHP" kind; older catalogs call the format
"Spectrum - CHP"), all CNGSoft's own.

- **Source:** the tracker itself, `CHIPNSFX.C` from `http://cngsoft.no-ip.org/chipnsfx.zip`
  (the 2024-12-31 build), GPL-3.0-or-later, kept unchanged in `csrc/chipnsfx` and built
  without its user interface (`-DDEBUG`). `bbmp-chipnsfx.c` loads the song with the
  tracker's loader and plays it with its sound code, as its WAV export does (with its
  oversampling, `-Q`).
- **Checked:** against the tracker's own WAV export (`-w -s0 -Q`), sample for sample,
  but for a filter that takes away the steady level the 8-bit sound has (0 is silence,
  so all sound is above it). The engine runs at 44,100 Hz; the helper resamples by
  straight lines. All 814 songs in the tracker's package and all 145 of ZX-Art's play
  (12 of the package's are jingles under 2 seconds).
- **Stereo:** the tracker's ABC (A left, B both, C right), a quarter of each side mixed
  into the other, so that a song on channel C alone isn't in one ear only.
- **Damaged songs:** the tracker's loader writes order lines before it counts them and
  copies instrument names into 17 bytes; the helper first cuts a song to 255 orders and
  16-byte names, and refuses a tempo outside 1-1000 Hz and 1-255 ticks a row. Fuzzed
  under AddressSanitizer (1 October 2026); the only finding was the tracker shifting
  negative numbers left in its vibrato, which GCC does as arithmetic.

## TIATracker (`bbmp-tiatracker`)

Andre "Kylearan" Wichmann's tracker for the Atari 2600's sound chip (the TIA: two
channels, 32 pitches each, a handful of waveforms); its songs are JSON (`.ttt`). ASMA has
70 (older catalogs call the format "Theta Music Composer", which it is not).

- **Source:** the tracker's own code, from the copy kept by dionoid
  (github.com/dionoid/tiatracker, commit f523802): the song model and loader
  (`track/`), the replay logic as the tracker plays a song (`emulation/player.cpp`), and
  Stella's TIA sound (`emulation/TIASnd.cpp`). Kept in `csrc/tiatracker`, unchanged but
  for three small accessors in `player.h`, marked. Qt is replaced by small stand-ins
  written for this player (`shim/`), with a JSON reader of its own. `README.txt` there
  says where each file comes from.
- **Licence, and why it is a program of its own:** TIATracker is GPL-2 only (its files
  say "see license.txt", which is the GPL 2, with no "or later"); Stella's part is GPL-2
  or later. GPL-2-only code can't be built into a GPL-3 program, so the helper is a
  separate program under the GPL 2 (its files say `GPL-2.0-only`; `license.txt` and
  `README.txt` are installed beside it). The player only runs it and reads its output,
  as with every helper.
- **How it plays:** once, from the song's start patterns (the tracker's "play from
  start"), at 50 frames a second for PAL songs and 60 for NTSC, with the song's speed,
  funktempo (odd and even rows at different speeds) or speeds set per pattern. It stops
  where the song ends, or where a goto would bring both channels back to a row they
  have played together (that is the length shown); at most 30 minutes. A note the
  tracker can't play (a pause after a drum) stops the song, as in the tracker; one of
  the tracker's example songs does that. The two channels are mixed to stereo, each
  three quarters on its own side and a quarter on the other, and the steady level the
  TIA's sound has (it is all above silence) is taken away.
- **Checked:** the TIA registers the helper writes, frame by frame (`bbmp-tiatracker
  --regs`), against TIATracker's unmodified code built with the real Qt 5 (a test
  program, not kept): the same for all 134 different songs tried (the 64 in the
  tracker's songs folder and ASMA's 70), over up to 30 minutes each. That checks the
  Qt stand-ins and the JSON reader, not the replay logic itself (it is the same code),
  nor the tracker against the 2600's own player routine. All 70 of ASMA's play; of the
  64, all but one empty test song.
- **Damaged songs:** the helper refuses, with the reason, a file over 2 MB (the largest
  song known is 460 KB), a song whose sequences play a pattern that isn't there or has
  no rows, whose notes play an instrument or drum that isn't there or a pitch outside
  0-63, whose slides are larger than 63, whose start patterns aren't in its sequences,
  or whose speeds are outside 1-255; the tracker's own loader already checks envelopes
  and gotos. Every list the tracker's code reads checks its index, so anything missed
  ends with an error. Fuzzed under AddressSanitizer and UBSan (1 October 2026), 24,000
  songs changed at random (bytes flipped, cut short, numbers made huge, JSON members
  deleted, doubled or given another type; about a third still played): the one finding
  was a note's pitch of about -2 billion overflowing the replay code's sum, now refused.

## Synder SNG (`bbmp-synder`)

DaNyL's SNG-Player (Windows, 2008) plays C64 tunes recorded into its own format:
Modland's "Synder SNG-Player" (66 songs, 2 or 3 voices, mono) and "Synder SNG-Player
Stereo" (67, 4, 6 or 8 voices). They are not C64 register dumps: for each voice and
frame the file holds six settings (a pitch in hertz, a noise switch, a place in the
stereo, a waveform shape, a loudness and a low-pass filter), packed with aPLib's LZ
scheme. The program plays them on a small wavetable synthesizer of its own at 48,000 Hz.

- **Source:** written for this player in a clean room (`csrc/synder`, GPL-3.0-or-later):
  from a description of the file format, the unpacking and the synthesizer only, by
  someone who never saw the program's code, the unpacker too; checked against every
  song recorded with the program.
- **Checked:** sample for sample the same as the original program's own recordings of
  all 133 songs (about 388 minutes), and the unpacked frame data byte for byte the same
  for all 133 (`bbmp-synder --ref` and `--unpack`, 1 October 2026).
- **The original's faults, kept:** with more than four voices the program keeps voices 5
  to 8 in the same memory as its noise table. Those voices start from the table's bytes
  instead of silence, and a voice playing noise reads the voices' state as noise. That
  colours the 6- and 8-voice songs (their stereo too), so it is kept; `--clean` keeps
  the voices apart, for comparison only. The program does not clip, it wraps; no song
  comes near that.
- **How it plays:** once, to the end of its last frame (the files give a loop frame; it
  is not used). Other sample rates than 48,000 Hz are resampled.
- **Damaged files:** refused, with the reason: a file over 16 MB, a voice count outside
  1-8, a frame rate outside 1-1000 Hz, a text running past the end, frame data that is
  not a whole number of frames, over 64 MB or over 1,024 times its packed size, a song
  over 30 minutes, and a packed stream that reads past its end, refers to bytes before
  its start or does not unpack to exactly the size the file gives. Synder Tracker songs
  (same extension) and `CYN` files are refused as not supported. Fuzzed under
  AddressSanitizer and UBSan (1 October 2026), 12,000 files changed at random (bytes
  flipped, cut short, header numbers changed, sizes made huge, unpacked frame data
  scrambled in both layouts): nothing found.

## T0AST (`bbmp-t0ast`)

T0AST, "The 0ok Amazing Synth Tracker", is BeRo's (Benjamin Rosseaux's) first tracker
for 64k intros (Windows, 2004-2006). Each of its 16 channels has a small synthesizer of
its own: an oscillator (sine, triangle, square, saws, noise or a 303 voice), a filter, a
vowel filter, distortion and a delay, and channels can feed each other (FM, ring
modulation, chorus). Modland has 8 songs (`.t0ast`).

- **Source:** written for this player in a clean room (`csrc/t0ast`, GPL-3.0-or-later):
  from a description of the file format, the sequencer and the synthesizer only, by
  someone who never saw the tracker's code; checked against references made with the
  tracker's own WAV export.
- **Checked:** bit for bit the same as the tracker's own WAV export, with the same
  length, in all 77 songs tried: Modland's 8, 66 example songs from the tracker's
  archives and 3 test songs made to cover the effects no real song uses (`bbmp-t0ast
  --ref`, 1 October 2026; built with `-O0`, `-O2`, `-O3`, the Flatpak's flags and
  `-march=native`, all exact).
- **The x87 point:** the tracker ran on the x87 unit of the PC, at its extended precision
  and rounding toward zero, and its sound depends on it: the synthesizer's values creep
  by the last bit, and feedback patches grow from it. On x86 (32 and 64 bit) the player
  does the same: `long double` arithmetic, the rounding mode set while the engine runs,
  and the x87 sine, cosine and 2^x instructions. On other processors (ARM) the C
  library's functions stand in. Tried on x86-64 with those functions instead, 65 of the 77
  songs stay bit for bit the same and 10 differ in one to eight samples, more than 150 dB
  below the peak; two drift: Dkamp's `tc` (58 dB below the peak) and `64kext`, a
  feedback patch that grows apart after 158 seconds.
- **How it plays:** once, as long as the tracker's export makes it: until the order
  list has come round (or a position is played a second time). The engine runs at
  44,100 Hz; other rates are resampled. Samples beyond full scale are clipped.
- **Damaged files:** refused, with the reason: a file over 4 MB, no T0AST signature,
  more than 16 channels, an order list longer than 256, pattern data cut short or
  copying rows from outside its pattern, a text running past the end, an instrument
  setting that is not a number or far outside what a song uses, and a song that does
  not end within 30 minutes. Fuzzed under AddressSanitizer and UBSan (float casts
  too) on 1 October 2026: 7,000 files, songs changed at random (bytes changed, cut
  short, settings made huge or not a number, texts made long) and new songs full of
  random effects and instruments, played from several start points at three rates:
  nothing found.

## BeRoTracker (`bbmp-berotracker`)

BeRoTracker is BeRo's (Benjamin Rosseaux's) tracker in the style of Impulse Tracker, with
a mixer in floating point: 8-tap resampling, resonant and biquad channel filters,
envelopes, new-note actions, and a routing graph of built-in effects (delay, stereo
delay, reverb, pitch shifter, bass booster, 10-band equaliser, compressor,
distortion). Modland has 19 songs (`.brt`), all BeRo's own.

- **Source:** written for this player in a clean room (`csrc/berotracker`,
  GPL-3.0-or-later): from a description of the file and the mixer only, by someone who
  never saw the program's code; checked against each song's output from BeRoTracker
  1.00 (Linux, September 2026, a free download).
- **Checked:** bit for bit the same as BeRoTracker 1.00's own output, with the same
  length, in all 19 songs, about 42 minutes in all (`bbmp-berotracker --ref`, 2 October
  2026; built with the build script's flags, with the Flatpak's flags and under
  AddressSanitizer at `-O1`, all exact). Five points were not clear from the description
  and were settled by the recordings: a sample's auto-vibrato deepens by its rate every
  tick; a new note leaves the channel's filter as it was unless the instrument sets one;
  the effects add their tiny anti-denormal constant as an extended number (the voices as
  a single); a tempo change counts at once for the volume ramps of its own tick; and the
  global volume's ramp is stopped by comparing bit patterns as unsigned numbers.
- **The x87 point:** the mixer works in single, double and x87 extended arithmetic, and
  its output depends on all three. On x86 (32 and 64 bit) the player does the same:
  `long double` arithmetic and the x87 sine, cosine, 2^x, logarithm and scaling
  instructions. On other processors the C library stands in. Tried on x86-64 with plain
  double arithmetic in place of extended: 7 of the 19 songs stay bit for bit the same,
  the others differ by at most 0.00001 (about 100 dB below full scale).
- **How it plays:** once, to the end of its order list; the tracker stops there, with no
  tail of echoes. The engine runs at 44,100 Hz, the rate of the recordings; other rates
  are resampled. The output is floating point; it is clipped to full scale and scaled to
  16 bits.
- **Not covered by the songs** (the description leaves them out, so they are guesses or
  left alone): effects the songs don't use (arpeggio, vibrato, tremolo, pattern delay,
  jumps and breaks, tempo slides, most `S` commands, the volume column beyond setting
  the volume, the panning column), tone portamento's slide, random swings of cutoff and
  pitch, LFOs, resonance envelopes and glides, envelope interpolation mode 0, stereo
  sustain loops, other resampler settings, filter modes other than the four described,
  the newer ZIP form of the file, external plug-ins, plug-in parameters other than the
  pitch shifter's, equaliser modes other than ±12 dB, and delays with a tempo of their
  own.
- **Damaged files:** refused, with the reason: no `BRTF` signature (or the ZIP form), a
  part running past the end of the file, no song header, no channels or more than 256,
  an order list longer than 257, a pattern above number 239 or longer than 4,096 rows,
  packed pattern data that refers before its start or unpacks to too much (256 MB in
  all), samples with float data, more than two channels or other than 8 or 16 bits, a
  sample longer than 64 M frames or with an impossible rate, more than 512 MB of
  sample buffers, an envelope with more than 1,024 points or loops outside them, a
  routing graph that refers to parts it does not have, has no single output, uses
  external plug-ins or more than 64 effects, effect settings that are not numbers or
  far outside what a song uses (delays, reverb pre-delays and all-pass steps, pitch,
  bass booster, equaliser bands and widths, compressor times and look-ahead), a song
  with more than two million ticks, and one that does not end within 30 minutes.
  Positions outside a sample's padding and loops shorter than one step are kept
  inside. Fuzzed under AddressSanitizer and UBSan (float casts and division by zero
  too) on 2 October 2026: 7,000 files, 16 of the songs changed at random (bytes and
  numbers in every kind of chunk changed, chunk sizes changed, chunks dropped or doubled,
  files cut short), played at three rates from two start points: nothing found.

## AXS (`bbmp-axs`)

AXS is Resolution Audio's synthesizer and tracker for Windows (the Netherlands,
1998-2001): seven identical two-oscillator synthesizers (twelve waves, sync, ring and
AND modulation, drive, five filter types, three envelopes, two LFOs, glide) and a drum
sampler with settings for each of its 128 notes, a pool of 64 voices, a tempo-synced
delay, a reverb and a tracker of up to 16 tracks, with the sound set by MIDI-style
controllers. The mix is mono, made stereo at the end: the right side is the dry sound
turned over and 10 ms late, with the effects. Modland has 26 songs (`.axs`).

- **Source:** written for this player in a clean room (`csrc/axs`, GPL-3.0-or-later):
  from a description of the file and of how AXS's Winamp plug-in (v1.0 beta, November
  2000) sounds, by someone who never saw the plug-in's code; checked against the
  plug-in's output for every song.
- **Checked:** sample for sample the same as the plug-in's output, with the same length,
  in all 26 songs, about 126 minutes in all (`bbmp-axs --ref`, 4 October 2026; built with
  the build script's flags, the Flatpak's flags, `-O0`, `-O3 -march=native` and with
  clang, all exact). One point was not clear from the description and was settled by the
  recordings: a new voice's noise oscillator holds 0 until its first new random value
  (two songs differed by one step in a few thousand samples before that). The engine is
  integer arithmetic but for the filters (IEEE doubles, rounded to single as the
  original's are) and the soft drive (an arctangent); on x86-64 the C library's tangent
  and arctangent give exactly the original's numbers but for two filter settings, which
  the player has as numbers.
- **The plug-in's quirk, kept:** the smooth triangle wave (wave 6) starts its eight-step
  average from a leftover number, the oscillator's address in the plug-in's memory, so it
  carries a small constant offset that depends on the voice. The player keeps those
  numbers by default; `bbmp-axs --clean` leaves the offset out.
- **How it plays:** once, as the plug-in plays it: to the end of the 576-frame block in
  which the song ends (the tick that ends it still starts the first row's notes, which
  ring into that block). One song (`idea`) jumps back to its start for ever; it stops at
  the length AXS itself gives it (90.4 seconds). The engine runs at 44,100 Hz; other
  rates are resampled.
- **Not covered by the songs** (described, but nothing could check them): the sustain
  pedal, LFO 1 on the volume, reverb presets 0-6 and 9-11, velocities above 127 (held to
  the tables' 128 entries), songs that end inside a jump.
- **Damaged files:** refused, with the reason: no `FORM`/`AXSF` signature, no song header
  or position list, no positions or no blocks, a position that plays a block the file
  doesn't have, a track playing an instrument above 7, a drum note numbered outside
  0-127, a sample of negative or impossible length, a file over 64 MB. A file cut short in
  its last chunk is read as far as it goes, as AXS reads it (two of Modland's songs end
  in the middle of a sample, and the plug-in plays them). Positions outside a sample, and
  table indexes outside the tables (only damaged songs reach them), are held inside.
  Fuzzed under AddressSanitizer and UBSan (float casts and division by zero too) on 4
  October 2026: about 129,000 files in 30 minutes, made from the 26 songs (their samples
  cut short) by changing bytes, chunk names and sizes, cutting and joining files: nothing
  found. Under AddressSanitizer the 26 songs still play exactly as the references.

## Formats played as another

- **BP SoundMon 1** (1 song): SoundMon 2's layout without synthesizer tables. The file
  says "BPSM" where SoundMon 2 says "V.2" and its table count; UADE plays a copy with
  "V.2" and no tables (`uade.py`, `_relabelled_copy`).
- **Digital Mugician 3** (1 song, Moon Child's title theme): Digital Mugician 2's
  layout under the label " MUGICIAN3/SOFTEYES 1990"; UADE's Mugician II player plays a
  copy labelled "MUGICIAN2". The game's demo has a recording of it on archive.org, not
  yet compared.
- **Megadrive CYM** (1 song): not Mega Drive music but a log of a Capcom arcade
  board's YM2151 (from MAME or Kawaks): pairs of register and value, register 0 a
  frame's wait (1/60 s). `backends/cym.py` writes it as a VGM 1.51 file, which
  `bbmp-zxtune` plays.
- **Pollytracker** (5 songs, Aleksi Eeben's 4-channel sample tracker for the C64): the
  tracker's module-to-SID tool makes a tune of its 962-byte player followed by the
  module, and four of the songs are in HVSC made so. The player is not part of this
  program: `backends/polly.py` downloads HVSC's
  `DEMOS/UNKNOWN/Polly_Rock.sid` from Modland (checked by its MD5), puts the module
  behind its player, sets where the module ends (byte 43 of the player), and libsidplayfp
  plays the tune. Three of the tunes so made are byte for byte HVSC's; the fourth,
  Backwoods, differs in two bytes of HVSC's copy of the song.
- **Spectrum - Snapshot** (ZX-Art, 65): ZXTune finds the tracker song in the memory of
  18 of the 31 snapshots; the 31 tapes, 3 disks and 13 snapshots left need an emulated
  Spectrum.

## Songs mended before they play

Some files on Modland are fine but for a detail their replayer checks strictly and the
music never uses. When the format's replayer refuses such a file, `backends/mend.py`
puts the detail right in a copy (in a folder of its own, gone when the song closes)
and the replayer plays the copy (2 October 2026):

- **HivelyTracker, AHX:** the last instrument name lost its closing zero byte, and
  ZXTune gives up reading the names (2 of 40 tried).
- **Spectrum Fast Tracker:** the file ends a few bytes inside its last ornament (1 of 40).
- **Slight Atari Player:** the last block of the program ends a few bytes before the
  block says.
- **Spectrum Sound Tracker 1.1:** rows past the song's pattern length, unused high bits
  of a sample's noise and tone shift, and a sample loop that starts past the sample's 32
  steps (taken as no loop) fail ZXTune's check (5 of 40).

## Checked against the original players: Pro Tracker 2 and 3 (2 October 2026)

`tools/verify/` compares what a replayer sends to the AY chip with what the original Z80
player sends, frame by frame and register by register, over every song Modland has.
`bbmp-zxtune --aydump` writes ZXTune's registers; `ayref` (ayref.c, on superzazu's Z80)
runs the original player; `aycompare.py` compares only what can be heard (a channel's
tone period not while its tone is off, the noise period not while no channel has noise);
`pt3.py` fetches the songs from the home mirror, runs both and sorts what differs;
`get-references.sh` fetches and builds the originals (not shipped).

| | Songs | The same in every frame | Explained | Differ |
|---|---|---|---|---|
| Pro Tracker 3 (Vortex Tracker II's Z80 player) | 7,297 of 7,376 | 7,218 (98.9%) | 27 | 52 |
| Pro Tracker 2 (Pro Tracker 2.101's own player) | 6,243 | 5,966 (95.6%) | 242 | 35 |

- **Faults of ZXTune found and mended** (`csrc/zxtune/*.patch`, applied by
  `build-zxtune.sh`): PT3 adds a note and its ornament in a byte (a sum of 128 or more is
  below the lowest note, not the highest), and the channels' envelope offsets in a byte;
  PT2's volume table is rounded (ZXTune's was a step quieter in places), a pattern plays
  to its end past 64 rows, a 00 inside a row is a noise command with its value, and a
  note past the end of the 96 notes' table plays what the original's memory has there,
  the start of its volume table (762 songs touched it).
- **Explained:** a channel that plays a note before choosing a sample (undefined in the
  originals: their sample pointer is left as it is; ZXTune, Vortex Tracker's editor and
  Ay_Emul use sample 1); a sample or ornament started past its end (the original reads
  the bytes that follow for a frame); files cut short on Modland; PT2 songs where S.V.
  Bulba's player, which mends the original's faults (its tone portamento), plays as
  ZXTune does; PT2 notes past the table in a few frames.
- **Not compared:** Turbo Sound songs (two chips), PT3 files that are a program holding
  the song. The originals start a PT2 song's first row after its delay less a frame;
  ZXTune plays it at once.

## Checked against VICE: C64 tunes (2 October 2026)

An HVSC tune is its own player: the composer's 6502 code, which a C64 emulation runs.
`tools/verify/sid.py` checks the emulation: every change libsidplayfp's C64 makes to the
SID (`sidwrites.cpp`: the registers after each step, time counted by the SID's 192 kHz
output, as `play(n)` counts scheduler events, not cycles) against VICE's `vsid`, the
reference C64 emulator (its "dump" sound device logs each write with its cycle; the Arch
package unpacked into `~/.local/opt/vice`, run in a headless cage, no sound out).

- **1,000 random HVSC tunes, 30 s each:** 956 of 996 the same, change for change and in
  order; in time within 7 cycles at the median, 90% within 60.
- **What differs:** 21 RSID programs playing samples through the volume register (to the
  cycle; they part after 0.001-27 s); 7 tunes that read oscillator 3 for random numbers
  (the chip emulation's own values); 12 others. 4 tunes for two or three SIDs skipped.
- The emulators start differently (VICE's driver waits for the next frame), so time is
  compared from a second in. With the C64's own ROMs in libsidplayfp, the same.

## Amiga songs UADE needed help with (2 October 2026)

What made Modland's songs fail in UADE, and what `uade.py` and `companions.py` do about
it.

- **Files a song loads by a fixed name** go with it and are hidden from the lists: the
  shared `smp.set` of a folder (Synth Dream, Maximum Effect, Hippel ST COSO's Wings of
  Death), Kris Hatlelid's `songplay` (the game's replay code), Paul Robotham's `.ssd`
  samples (else the folder's `mdtest.ssd`), Mark Cooksey's `mcs.*` samples.
- **Fill bytes for zeros:** seven Maximum Effect songs have a fill byte where their
  tables have zeros; a copy with zeros plays.
- **IFF-SMUS** songs whose Instruments folder lacks one instrument (wildman's "Bpart")
  or has its sample without the `.instr` (Gilsons Green Grape's bass) get a stand-in:
  made from the sample, or silent. The song plays without that part rather than not at
  all.
- **Other names:** Hippel ST's `.sog` files are played as `hst.` (6 of the 14 play right;
  8 come out as the same quiet opening, which UADE's player misreads); "feud fake.dw" is
  a Jason Brooke song. Ashley Hogg's Stuntman Seymour plays with the player's check
  skipped (its code has one instruction more than the check expects).
- **DeltaMusic 2's end:** its channels loop each on their own, and UADE's player says a
  song has ended when the music goes on (Sanxion's loader after 30 ms, Zaxxon after
  7 s: 12 of Modland's 98). A song said to end in its first 60 seconds plays on as a
  song that never ends: the player stops it at 3 minutes with a fade.
- **Not songs**, hidden: games' sound-effect banks (6 David Whittaker, 2 Rob Hubbard,
  6 Infogrames, 1 Activision Pro, 1 Digital Sonix) and Composer 670's `.670` files (each
  an older layout of the `.c67` next to it).
- **What UADE can't:** Marble Madness and Archon crash it (Delitracker Custom); FredMon,
  Hippel (2), Mark II, Beathoven and Sonic Arranger songs with replay code versions its
  players don't know; Silmarils' songs whose samples are in another file; SunTronic
  files cut from a bigger dump; Forgotten Worlds, which sounds or not by its path.

## Sound Club

Sound Club for DOS (`.sn`) and for Windows (`.sn2`). Written for this player in a clean
room, from a description of the format only (`csrc/misc/sclub_cr.cpp`, GPL 3 or later).
194 of Modland's 195 songs play.

- **Checked:** the same sound as the replayer before it, sample for sample, in all 195
  songs and 280 test files. That replayer was compared with the original program's own
  WAV export, sample by sample (194 songs): all within 0.2% (the size of the difference
  against the size of the sound); 187 within 0.1%, 73 within 0.01%.
- **How Sound Club mixes**, which the replayer does too because the sound depends on
  it: a voice is mixed in runs that end at the sample's end, at a slide's next change
  or at the end of a block (a step, or the program's buffer of 8192 frames). A slide
  counts down to its next change by the whole run before the run is cut short at the
  sample's end, so over a short loop a slide goes faster. Each run's mixing is picked
  by the step at the block's start: a step below 1 interpolates (16 positions, each at
  its middle) and ignores the step's whole part, so a slide that rises past 1 inside a
  block plays far too low until the block ends; a step of 1 or more doesn't
  interpolate. 8 bit sounds at a step of 1 or more get 128 added to every value (a
  small DC offset per voice). A silent voice (volume 0) is moved on in one go, losing
  the fraction's carry. Voices are added one by one into 16 bits that stop at the
  limits, in the order of their places; a place stays taken after its sound ends until
  its key is let go, and a slide moves that key too.
- **Known:** the original's WAV export leaves out the last part-filled buffer (up to
  0.17 s at the end). One song ("nothing else matters") has a sample with a wrong
  checksum and is refused.
- **Data:** 129 of the songs use the program's instrument bank, and FM instruments
  use its waveforms. The player offers to download Sound Club for Windows 2.03's
  package from its publisher and takes the two files out of it (Downloads in Settings,
  `bbmp download soundclub`). They stay on the computer and are not passed on: the
  package may be distributed only whole and unchanged.
- **Damaged songs:** a packed sample must fit into what is left of the file (a byte a
  sample unpacked, a bit a sample packed; all of Modland's and the bank's do), and a Sound
  Club 2 song has at most 64 instruments (Modland's most), so a small file can't ask for
  hundreds of MB. The engine was fuzzed under AddressSanitizer and UBSan.

## MONOTONE and DigiTrekker

`bbmp-pcmusic`, built by `tools/build-pcmusic.sh` from the player's own code;
nothing is fetched.

**MONOTONE** (Jim Leonard's PC speaker tracker, 4 songs): a port to C++ of the
tracker's own player (`MTSRC/MT_PLAY.PAS`) and PC speaker output (`MT_OUTP.PAS`, as
`MT_EDIT.PAS` sets it up) from github.com/MobyGamer/MONOTONE, which is under The
Unlicense (public domain). The player runs at 60 Hz; the speaker sounds one track at
a time, in turn, one per tick (the first four tracks), starting again with the first
on every row. The tone is the timer chip's square wave (1193182 Hz divided by a whole
number), averaged over each sample.

- **Checked:** the timer divisor sent to the speaker on every tick, against what the
  tracker's own disk writer (Ctrl+W, MONOTONE 2019-10-09, `MT.EXE` run in the emu2 DOS
  emulator) writes: the same on every tick of 10 songs (the 4 on Modland and 6 more
  from the tracker's repository, about 48,000 ticks). `BBMP_MONOTONE_DUMP=<file>` makes
  the helper write the same file.
- **Differs on purpose:** the disk writer stops at a position jump (Bxx) and writes
  one tick of the last pattern's first row after the end; the helper plays the row
  with the jump to its end, follows the jump and ends where the song has been before.
- **Not modelled:** the speaker cone and the PC's case; the square wave is clean.

**DigiTrekker** (Horst Beham's DOS tracker, 1994-95, 2 songs): written for this
player from the format's document (`DTMDOC.TXT` in DigiTrekker 3.10, also in libxmp's
docs as `dtm-1.0.txt`) and the effect list of the tracker's help file. The document
leaves out two things the files show: tracks are numbered from 0 (track 0 is kept
empty), and DigiTrekker 3.00 wrote the `sped` chunk as two bytes (speed, tempo)
followed by garbage.

- **Checked:** against DigiTrekker 3.10's own rendering (its `FILE8S.DRV` output
  driver, 8 bit stereo at 44100 Hz, run in DOSBox 0.74-3): both songs line up from
  start to end (drift 40 ms over 230 s), the same spectrum (log-spectrum correlation
  0.993 and 0.997, no shift in pitch), the same balance of left and right (within
  0.4%), loudness envelopes correlated 0.75-0.99 per 10 s.
- **Differs on purpose:** DigiTrekker's output is about three times as loud and clips
  (4% of all samples of `keepcool.dtm`, more of `demorave.dtm`), which is where most of
  the envelope difference is; the helper mixes with linear interpolation and doesn't
  clip. DigiTrekker plays a song again when it ends; the helper ends it.
- **Not checked, no song uses them:** arpeggio, tremolo, retrigger, pattern loop and
  delay, note delay and cut, glissando, the vibrato waveforms, finetune, 16-bit
  samples.

## DarkWave Studio

`bbmp-darkwave`, built by `tools/build-darkwave.sh`: DarkWave Studio's own engine,
GPL 3 or later. DarkWave Studio (Daniel Werner, experimentalscene.com) published its
source up to 4.1.6 (2012; in the Wayback Machine as `DarkWave-Studio-4.1.6-Source.zip`,
pinned by sha256; the program is freeware without source since). Modland's three songs
are its own sample projects (Chemistry 101, 202 and 303, project format 3) and use only
its own machines: ES BassHead, ES Stomp, ES DGenR8, ES Mixer, ES SpatialVerb and ES
QuadDelay.

- **Made of:** the machines of `ES-CoreMachines.dll` and DarkPlug (their interface),
  built from the source with their windows taken out
  (`csrc/darkwave/darkwave-4.1.6-headless.patch`, which only wraps window code in
  `#ifndef DW_HEADLESS`; `csrc/darkwave/compat/` stands in for `<windows.h>`); the
  studio (project loader, machines' cables, patterns, sequence, the output stage) ported
  from `Studio/*.cpp` in `csrc/darkwave/studio.cpp`, its odd behaviour kept (the last
  block of a sequence is gone through twice; the AUX inputs of the mixer are not cleared
  by the studio). No windows, audio devices, MIDI or VST. A project that needs a machine
  it lacks (a VST plug-in, a later program's machine) is refused with the machine's name.
- **As the program runs it:** 512-sample blocks; the machines' SSE2 code paths (the
  program took them on every x86-64 processor; `AddValueToBlock` adds to half the
  samples there, and that is heard); Microsoft's `rand()`; the machines run for a
  second before the song starts, as they do in the program between loading and Play
  (the drum machine's first beat is too loud without it).
- **Length:** a project doesn't save the Loop button, and the sample projects are one
  to three bars (1.7 to 6 s). A sequence shorter than half a minute is played with the
  studio's own Loop as many times as it takes to fill one (15, 18 and 5 times); the
  "Sequence" fact says so. `BBMP_DARKWAVE_PASSES=<n>` sets the number.
- **Checked:** against DarkWave Studio 4.0.6's own output (the Windows program from
  the Wayback Machine, `DarkWave-Studio-4.0.6.exe`, run in Wine in a headless cage,
  its WaveOut output at 48000 Hz caught by ALSA's file plugin; 4.1.6's program is not
  archived, and 4.0.6's machines differ from 4.1.6's only in the formant filter, which
  none of the songs uses). Where the program's oscillators and LFOs stand when Play is
  pressed depends on how long it ran before; `BBMP_DARKWAVE_IDLE=<blocks>` sets the
  lead-in, and with the lead-in that matches, Chemistry 101 correlates 0.998 sample by
  sample with the program's output (loudness envelope 1.000), 202 0.997 (0.9998), and
  101 played with Loop for 48 s 0.93 (0.999, no drift). Chemistry 303: its drum part's
  envelope 0.998 (the noise is `rand()` seeded by the clock in the program); its bass
  has a 12.6 s LFO on the filter and a detuned second oscillator, and no lead-in tried
  (0 to 1300 blocks) lines both up: its brightness follows the program's through the
  song (spectral centroid within 10%), the envelope correlates 0.95.
- **Not checked:** machines no Modland song uses (ES BassHead Polyphonic; the program's
  other machines, AntiAlias, Crunch, Distort, StereoSplitter and -Joiner, are not built);
  control tracks (property automation in patterns). Of the ten sample projects in the
  4.1.6 source (format 6) the four made of the built machines play (by ear of the numbers
  only: length, level); the other six are refused, naming ES Arpeggiator, ES MIDI Input
  (the studio's own machines, not ported), ES Crunch, ES Distort or ES StereoSplitter.

## Furnace (Deflemask, FamiTracker)

Furnace, built headless from its source, renders a song to a WAV file, and
`bbmp-furnace` sends it on as it is written (a long song takes Furnace 10-20
seconds, longer than the player waits for a first sound). Two patches of ours: the
option `-outrate` (`csrc/furnace/outrate.patch`), since Furnace writes 44100 Hz and
the player plays 48000, which made every song 9 % fast and high until 29 September
2026; and, in the helper, Deflemask files whose zlib checksum is broken are unpacked
without it. About a fifth of Modland's Deflemask files are damaged that way: they were
sent as text once (every CR LF became LF), and most of them can't be mended.

Two of Furnace's sound cores state no licence, QSound's (superctr's `qsound.c`, the one
bbmp-vgm leaves out too) and the Watara Supervision's (`supervision.c`, "a modified version
of Potator's"): since 8 October 2026 they are not built (`csrc/furnace/no-unlicensed-cores.patch`;
the build stops if either is in the program), and Furnace's dummy chip stands in, silent.
Furnace has no other core for either. No song the player finds loses sound: Deflemask
(DMF) and FamiTracker files can't use these chips, and the Furnace files of ZXArt are all
Spectrum songs (checked: 1,807 DMF on Modland by their format, the 17 cached by their
system byte, 5 of ZXArt's 11 .fur by Furnace's own text export). 22 cached Deflemask,
FamiTracker and TFM songs play byte for byte as before.

`tools/check_rates.py` checks every helper for the same fault: the sound it sends
against the length it says.

## Retrovert's plugins and libxmp

`bbmp-rv` loads Retrovert's playback plugins (MIT): Klystrack. libxmp is MIT.

**iXalance** (`.ixs`, engine `ixalance`) is played by an engine built into `bbmp-rv`,
`csrc/retrovert/ixs_engine.cpp`, written for this player in a clean room (7 October
2026) from a description of the format and of how the earlier player (Juergen Wothke's
iXalance plugin for Retrovert, non-commercial, no longer used) sounds: both sample
generators (six-operator FM with filter and reverb; wave table with partials and
detuned voices), the tracker and the fixed-point mixer at 44100 Hz, converted to the
player's rate by `bbmp-rv` as before. It gives the same output, sample for sample, as
the earlier plugin on all 24 songs (with that plugin's reads past sample ends taken as
silence), the same generated samples and the same channel state on every tick, and the
same results on 315 test files (191 the same, 109 refused, 15 free), when built with
`-ffp-contract=off` and run with glibc's `sin`, `pow` and `fmod`. It refuses damaged
files the plugin would have read past or crashed on.

Seven of the songs (caribbean, shortrance, the next e, vixen - heineken, archon,
vectorworld, iron dimension) also use three short drum sounds that came with Shortcut
Software's player: `909OH.WAV`, `909SD.WAV` and `CRASH.WAV`. They are not part of the
program and are provided separately. The engine reads them from the folder named by
`BBMP_IXS_DATA`, checks each by its size and SHA-256, and refuses a song that needs one
it does not find, with one line naming them; the other 17 songs play without the folder.
`bbmp download ixalancesamples` (or Downloads in Settings, or the offer at the first
start) fetches them: the source of the earlier plugin (RetrovertApp/playback-ixalance at
commit 9b474c8 on GitHub, pinned by SHA-256) holds them as the C arrays `WAVE_800`,
`WAVE_801` and `WAVE_802` of `src/WaveLibrary.c`, which are the files' bytes exactly;
each is checked against its SHA-256 (`download_pins.py`), and the player names the folder
in `BBMP_IXS_DATA` (and lets the sandboxed helper read it). A song refused for want of
them gets the advice to download them.
`BBMP_IXS_DUMP=<prefix>` writes the engine's own 44100 Hz output (`.eng.raw`), the
samples it made (`.samples`) and the channels' state on every tick (`.ticks`).
`bbmp-rv ixalance <file> <track> <rate>` selects the built-in engine; so does any first
argument whose file name is `ixalance_playback.so` (the old plugin's name; the file is
not needed and is no longer built).

`bbmp-rv` is a program of its own with nothing under the GPL in it: its own files,
`csrc/retrovert/bbmp-rv.cpp` and the iXalance engine, are under the MIT licence
(`csrc/retrovert/LICENSE`), it links only the C and C++ runtime libraries and zlib, and
`tools/build-retrovert.sh` stops if a GPL file turns up among its sources.
The licences and a README.txt of where each part comes from are installed in
`share/bbmp-rv/` beside it.

## Ad Lib formats AdPlug doesn't play (`bbmp-adlib`)

Replayers written for this player, on AdPlug's Nuked OPL3 (`tools/build-adlib.sh`).
Each was checked by running the original program's own routine in an emulated 8086
(Unicorn) next to ours: the chip's
registers after every tick of the whole song, compared. This is the only check; nobody
has listened to the original programs.

- **Pixel Painters FMF** (Electranoid, Laser Light) and **FTF** (Xatax), engine
  `pixelpainters`. Checked against ENOID.EXE (Electranoid 1.20) and XATAX.EXE (Xatax
  1.00), from their shareware releases: every FMF song against Electranoid's loader, start
  and timer routine, with the FM-Music Functions of Creative's developer kit linked
  into it: the same registers on every tick, in all nine voices. The library puts its
  default instrument in every voice at the start and loads a channel's own only when a
  note starts; the game's start plays note 0 at the channel's velocity, and velocity 0
  is a note off, so a channel of velocity 0 (five in Laser Light's sound effects) keeps
  the default. Every FTF song against Xatax's routines: the same, in all nine voices. FMF plays at 18.2 Hz (the BIOS timer); FTF at
  16.43 Hz (Xatax's own timer, the music every 31st of it; the game can switch to 27
  for 18.9 Hz, which is not used here). FTF is played as on a Sound Blaster Pro 2 or
  later, with the songs' own panning; on an older card the game plays it in mono. The
  games loop their music; here a song plays once. Written for this player in a clean room, from a description of the formats only (`csrc/adlib/ppf_cr.cpp`); checked against the original's register writes in every song.
- **EdLib D01**, engine `d01`: JCH's EdLib songs with a voice of Sound Blaster samples,
  for his player 1.03 (SBL-0103.P01, in AdPlug's library of formats together with the
  three songs Modland has). The `.s01` beside the song holds the samples; without it
  the drums are missing (the header says so). Checked: the three songs against
  SBL-0103.P01: the same registers on every tick and the same samples started on the
  same ticks. AdPlug's D00 player is of a later line of JCH's players and plays these
  songs differently (levels, vibrato), which is why it isn't used. The sample voice is
  mixed as DOSBox mixes a Sound Blaster (the DAC at full scale); how loud it was next
  to the FM on the cards of the time is not known. The Alibi's first subsong holds only
  its drum loop and is left out. Written for this player in a clean room, from a description of the format only (`csrc/adlib/d01_cr.cpp`); checked against the original's register writes in every song.
- **Sound Images Generation 2** (the DOS Dizzy games), engine `sig`: the songs
  of Tony Williams' driver of 1993 (the copy inside Kwik Snax for DOS). A file holds five songs:
  the theme, two jingles and two empty ones, which are left out (three tracks).
  Checked: every song of the four files against the driver: the same registers on
  every tick. The driver runs at the games' 50.16 Hz; a song's speed doesn't depend on
  it. Where a song puts a track on a voice above 8 (Kwik Snax's fourth song does), the
  driver writes its levels to C0 and C8, and so does this. Written for this player in a clean room, from a description of the format only (`csrc/adlib/sig_cr.cpp`); checked against the original's register writes in every song.
  It writes a note's levels also where they have not changed.
- **Sound Interface System** (Modland's name; Xmas Lemmings), engine `lem`: the
  songs of "AdLib Music by Sound Images (c) 1991", the driver in ADLIB.DAT of Lemmings and Xmas
  Lemmings for DOS (packed with the games' own .DAT scheme; `check/lemdat.py` unpacks
  it). Checked: the three songs against the driver: the same registers on every tick.
  The game calls it once a VGA frame (70.09 Hz; its timer is set to the screen's
  retrace). Modland's rip of the second song leaves out a pattern of rests it shares
  with the third (in the driver's memory right after it); the helper puts it back.
  Written for this player in a clean room, from a description of the format only (`csrc/adlib/lem_cr.cpp`); checked against the original's register writes in every song.
- **FM Tracker** (Sagitta Software, Morten Stenshorne), engine `fmtracker`: the
  songs of FM Tracker Player 1.26 (FMTP.EXE, freeware, in fmtrk154.zip with FM Tracker 1.54 and
  FMTRK.DOC, which describes the effects). Checked: Modland's two songs and the six
  example songs of the zip against FMTP.EXE run in a small emulated DOS: the same
  registers on every tick, up to where the song goes back to its start (the player
  plays on; here a song plays once). The player's timer is 1193182/4096 Hz and a song
  plays a tick every 16/its clock speed of them. Written for this player in a clean room, from a description of the format only (`csrc/adlib/fmt_cr.cpp`); checked against the original's register writes in every song.
- **MUS** (Modland's name; First Samurai, Nick Jones), engine `samurai`: the file is
  the game's whole Ad Lib sound module, a program with its music; the replayer reads only
  its data (the song tables, instruments and tracks). It has four tunes (the module's
  sounds 33, 30, 31, 32; the others are the game's effects), four tracks here. 50 Hz.
  A tune ends once every voice it started has gone round once or stopped.
  Written for this player in a clean room, from a description of the format only
  (`csrc/adlib/mus_cr.cpp`); the same OPL writes as the original in all songs: all
  four tunes, on every tick.
- **Shadowlands** (Matt Furniss), engine `shadowlands`: the music of the game (Domark,
  1992). Modland's file is the game's TUNE.CAL unpacked (the game keeps it packed with
  Pack-Ice): a small tracker module for the chip's rhythm mode, six melodic channels and
  five drums. The game's timer runs at 1193182/5D38h Hz (50 Hz). The tune jumps back to
  its seventh order at the end; here it plays once (3:00). Written for this player in a
  clean room, from a description of the format only (`csrc/adlib/cal_cr.cpp`); the same
  OPL writes as the original in all songs: all 9024 ticks of the one tune.
- **Vibrants** (JO, Poul-Jesper Olsen), engine `vibrants`: the music of JO's player in
  the Copper demo (Surprise! Productions, The Party 1992). The song file is the player's
  memory as well as its music (its channels' variables are in it), and holds a silence
  (sound 0) and the tune (sound 1). The demo calls the player 70 times a second. The
  tune plays an intro and then goes round a loop of 5120 ticks; here it ends where the
  player's state comes back to what it was (1:42). The frequency table runs 64 bytes
  past its end, into the song's first bytes, and so here. Written for this player in a
  clean room, from a description of the format only (`csrc/adlib/vib_cr.cpp`); the same
  OPL writes as the original in all songs: the chip's registers hold the same values
  as with the original after every one of the 7150 ticks (this replayer writes every
  register of a sounding voice each tick, also those that have not changed).
- **Drum Traker** (15 songs) can't be played: its songs name instruments of the
  program's own library (CARCRANK, DRUMROK1, ...) and carry none; the program isn't
  to be found.

## Damaged files the libraries trusted (8 October 2026)

The final audit of 7 October fuzzed the helpers' libraries with damaged files. What it found
in third-party code is patched at build time (each patch says what it changes; real songs of
each format play byte for byte as before, but for two Klystrack songs, below):

- **libkss** (`csrc/chip/libkss-opx-size.patch`): an OPX file over 32 KB wrapped the
  converter's 16-bit size and the copy wrote past its buffer. Refused (the largest real OPX
  is 15,794 bytes).
- **StSound** (`csrc/chip/StSound-damaged-files.patch`): every read of the YM, Digi-Mix (MIX1)
  and YM-Tracker loader stays inside the file; the LZH unpacker refuses impossible code
  tables; a tracker note, a YM 2 drum and a loop point stay inside the song.
- **sc68** (`csrc/chip/sc68-chunk-size.patch`): a chunk whose size runs past the file is
  refused. (Two damaged files whose 68000 code never returns still run until the player's
  10-second stall timer ends them.)
- **psgplay's ICE unpacker** (in `csrc/psgplay/bbmp-psgplay.cpp`): the zeros in front of the
  packed data cover the most it can read (5 bytes for every byte it makes).
- **Snes9x** (`csrc/snsf/snsf9x-spc-padding.patch`): 0x400 bytes of padding after the SPC's
  memory instead of 0x100: the DSP read up to 0x2FF bytes past it on every song.
- **HivelyTracker in ZXTune** (`csrc/zxtune/hvl-damaged-files.patch`): wave length,
  waveform, stereo setting, a note and a position jump are kept inside the replayer's tables.
- **klystron, Klystrack's engine** (`csrc/retrovert/klystron-damaged-files.patch`, added to
  the plugin's own patch list by `tools/build-retrovert.sh`): counts and sizes are checked,
  a read past the file gives zeros, a song rate of 0 no longer divides. Its frequency table
  read one entry past its end when a note slid above the top note (2 of the 11 cached songs
  do: `phonkeh.kt`, `smp-dpintro.kt`); that entry held whatever the linker put next (in the
  build of 7 October, the first bytes of the ELF's frame table, a far lower pitch). Now the
  top note holds, so those two songs differ from before there.

Organya's sound bank is no longer installed with `bbmp-chip`: the helper takes the one the
player downloads from `BBMP_ORGANYA_BANK`, and without it refuses an Organya song with
"Organya soundbank missing".

## MSX drivers left out of libkss (`bbmp-chip`)

libkss plays MSX songs by wrapping each in a KSS image with its sound driver, and can
build six drivers in (its kss-drivers repository, C arrays). MGSDRV's and MoonBlaster's
may be passed on and stay built in. MuSICA's (kinrou5), MPK's (mpk106, mpk103: MPK
0.98 to 1.03 songs use mpk103) and OPLLDRV's (opx4kss) may not, so `tools/build-chip.sh`
builds libkss with `-DEXCLUDE_DRIVER_KINROU -DEXCLUDE_DRIVER_MPK106
-DEXCLUDE_DRIVER_OPX4KSS` (MPK106's switch leaves out both MPK drivers) and stops if
`nm` still finds them in the data section.

- **On request:** `bbmp download msxdrivers` (or Downloads in Settings) fetches
  kss-drivers at commit e8032da, checks the archive's SHA-256, turns the four C arrays
  into `kinrou5.bin`, `mpk106.bin`, `mpk103.bin` and `opx4kss.bin`, and checks each of
  those against its SHA-256. The player names that folder to the helper in
  `BBMP_MSX_DRIVERS`.
- **In the helper:** before loading a song, bbmp-chip asks libkss what kind of song it is
  (`KSS_check_type`). A MuSICA, MPK or OPLLDRV song gets its driver from that folder
  (`KSS_set_kinrou` and the like, which keep the arrays' full size, so the KSS image is
  the same as with the driver built in); when it isn't there, the song is refused with one
  line ("MuSICA songs need MuSICA's driver (kinrou5), which is not here") and the player
  adds where to download it. KSS rips carry their own driver and are not affected.
- **Checked (6 October 2026):** 32 songs (6 KSS rips, 3 MGSDRV, 4 MuSICA, 9 MPK of
  versions 0.98, 1.03 and 1.06, 4 OPLLDRV, 6 MoonBlaster), the first 40 seconds of each:
  with the drivers downloaded, all 32 byte for byte the same as the helper with the
  drivers built in; without them, the 17 MuSICA, MPK and OPLLDRV songs refused and the
  other 15 byte for byte the same.

## UADE's Amiga players and sc68's replay routines, downloaded on request

UADE and sc68 play by running each format's original replay routine (68000 code, loaded
and run as data, not linked). Most of those routines were ripped from games, editors
and demos and carry no terms that allow passing them on (the survey of all 176 UADE
players and 84 sc68 routines, 7 October 2026). So:

- **What is installed:** `tools/build-uade.sh` keeps only the 18 UADE players whose
  terms allow it (Fred, JochenHippel_UADE, ManiacsOfNoise, MusicAssembler, SIDMon1.0,
  MusiclineEditor, PreTracker, ArtOfNoise-4V/8V, MED, Octa-MED, Octa-MED-Ss, Medley,
  MusicMaker-4V/8V, Pokeynoise, SoundMon2.0/2.2; see licences/README.md) and UADE's empty
  `S/`, and stops if anything else is left; `tools/build-amiga.sh` checks bbmp-amiga's
  base folder the same way (those plus StoneTracker's). `tools/build-chip.sh` keeps sc68's
  own six stubs (dwaga, mon, none, note, note2, timerc) and checks it.
- **On request:** `bbmp download amigaplayers` fetches UADE's `players/` folder from
  GitLab at the commit the helpers are built from (8d77cd7, 726 KB, archive SHA-256
  pinned) and keeps the other 158 players and the `ENV:` files only they read, each
  checked against its SHA-256 (`src/bbmp/download_pins.py`). `bbmp download sc68replays`
  fetches sc68 2.2.1 from SourceForge (745 KB, pinned) and keeps the other 78 routines
  as `Replay/<name>.bin`, each checked.
- **How the helpers find them:** libuade is patched (`build-uade.sh`) to look for a player
  that isn't in `<base>/players` in the folder `BBMP_UADE_PLAYERS` names; the player sets
  it for its UADE process (`uade.py`, which also lets that sandboxed process read the
  folder) and for bbmp-amiga. A player's `ENV:` files are looked for beside it, so they
  sit in the same folder. bbmp-chip gives `BBMP_SC68_REPLAYS` to sc68 as its user folder,
  which sc68 searches before its own.
- **Without them:** the patched libuade names the player it couldn't load
  (`playerfname`), and the song is refused in one line ("UADE's Amiga player
  SonicArranger is not there") with the advice to download; bbmp-chip refuses an SC68
  song whose routine is in neither folder ("This song needs sc68's replay routine
  mcoder, which is not here"). The other replayers are still tried (a file under the
  wrong format's name may be theirs); when none plays it, that refusal and its advice
  (`NeedsDownload`) is what the player says. An SC68 file naming a routine sc68 is given
  on request stays with sc68 even when it isn't there.
- **Checked (7 October 2026):** the census songs of every format UADE and sc68 play
  (2,623 songs of 135 formats, first 20 seconds), against renders made before the change
  with all players installed: with the downloads, 2,623 of 2,623 the same (2,603 sounds
  byte for byte, the same 20 refusals as before); without them, 487 byte for byte the same
  (SoundMon, MusicMaker, Art Of Noise, Pretracker, FredMon, SidMon 1, Maniacs Of Noise,
  Music Assembler, Medley, PokeyNoise, Delitracker customs, Jesper Olsen's own replayer,
  StoneTracker, and 46 SC68 files: those that carry their own code or use a stub), 7 of
  the old refusals unchanged, 59 played by another replayer instead (all 40 AHX songs by
  ZXTune's HivelyTracker replayer, 17 Quadra Composer by ZXTune, 2 SC68 files by sc68's
  DWaga stub; all with sound), and the other 2,070 refused with the advice (13 of them
  songs that didn't play before either).

## Helpers split off for their licences: SNDH, SNSF, VGM; ZXTune without UnRAR, vgmstream, Z80~Ex, libvgm and Sony's BIOS

Not replayers of the player's own, but helpers whose build changed for their licences on
7 October 2026; how they play did not change.

- **`bbmp-psgplay` (SNDH):** psgplay is GPL 2 only, which may not be in one program with
  GPL 3 code, so it left `bbmp-chip` for a program of its own (`csrc/psgplay`,
  `tools/build-psgplay.sh`). Its one file of the player's, `bbmp-psgplay.cpp`, is the code
  that was in `bbmp-chip.cpp` (the ICE unpacking with room on both sides, the tags, the
  12 dB turn down), now GPL-2.0-only. `share/bbmp-psgplay/` has the licences and a
  README.txt of where each part comes from.
- **`bbmp-snsf` (SNSF):** snsf9x is Snes9x's code, for non-commercial use only, which
  can't go with the GPL at all. `bbmp-snsf` (`csrc/snsf`, `tools/build-snsf.sh`) has no
  GPL code in it: snsf9x, snes_spc (LGPL 2.1 or later), psflib (MIT), zlib, and the
  player's loader and program under the MIT licence. `share/bbmp-snsf/` has Snes9x's
  licence, the others and a README.txt; the build stops if a GPL file is among its sources.
- **ZXTune:** its sources bring UnRAR (its licence forbids using it to remake RAR's
  compressor, a restriction the GPL doesn't allow) and vgmstream (whose `relic_mixfft.c`
  is for non-commercial use only). `csrc/zxtune/no-rar-vgmstream.patch` stops ZXTune
  registering either, and `bbmp-zxtune` and `bbmp-ay-vt2` no longer link them (both
  3.5 MB smaller). The player opens RAR archives with 7-Zip, and none of the formats
  ZXTune plays for it went through vgmstream.
- **ZXTune without Z80~Ex:** ZXTune's AY Emul player runs a song's Z80 code on Z80~Ex,
  which is GPL 2 only and can't go with ZXTune's LGPL 3 or the GPL 3.
  `csrc/zxtune/no-z80ex.patch` stops ZXTune registering that player, and the helpers no
  longer link Z80~Ex, ZXTune's Z80 device or its beeper (read from their debug
  information). AY Emul songs were already played by Game Music Emu (the census: 40 of
  40). Checked against the helpers before, byte for byte, 20 seconds: the census's 1,421
  ZXTune songs and Vortex Tracker II's 12 are the same, except one: the ZX-Art
  Multitrack song "Proof of concept (a bonus track)" (Yerzmyey) has a part for a second
  Spectrum in AY Emul's form, which is now silent (its AY tracker part still plays); the
  other 4 Multitrack songs tried are the same.
- **`bbmp-vgm` (VGM, S98):** libvgm (Valley Bell), which ZXTune played VGM and S98 with,
  can't stay in a ZXTune helper: its OPL4 (`ymf278b.c`, after openMSX) is GPL 2 only, and
  several of its sound chips state no licence at all. `csrc/zxtune/no-libvgm.patch` stops
  ZXTune registering its VGM and S98 player, and the helpers no longer link libvgm (read
  from their debug information; both 1 MB smaller). `bbmp-vgm` (`csrc/vgm`,
  `tools/build-vgm.sh`) plays them instead: libvgm as ZXTune's sources carry it, built
  with ZXTune's flags, in a program of its own under the GPL 2 only, played the way
  ZXTune played it (the same settings, 20 ms at a time, round and round, a little past
  the length). Left out are the chips' files that state no licence: Maxim's SN76489, Gens'
  YM2612 and RF5C164, Ootake's PC Engine, superctr's QSound, Valley Bell's SAA1099 and the
  WonderSwan. Each of those chips but the WonderSwan has a MAME core (BSD 3-clause) beside
  it, which is used instead; libvgm asks for the Gens RF5C164 and the superctr QSound by
  name, so `bbmp-vgm` asks for MAME's. No WonderSwan VGM is in the archives the player
  browses; one of your own is now silent. `share/bbmp-vgm/` has the licences, the MAME and
  MIT notices and a README.txt of where each part comes from. The titles, authors and
  games of S98 files are now read as libvgm reads them (ZXTune ran all the tags into the
  title or game for many); the lengths are the same.
- **Checked (7 October 2026), `bbmp-vgm` against ZXTune with libvgm, the sound byte for
  byte, 20 seconds:** the census's songs of every format libvgm played and of every VGM
  format Game Music Emu plays (VGM files of your own go to `bbmp-vgm`): S98 40, MSX1 40,
  MSX2 40, Vectrex 20, ColecoVision 11, Virtual Boy 9, Spectrum VGM 3, X68000 1, Master
  System 40, SG-1000 40, Game Gear 40, Mega Drive 40, 32X 40, Mega CD 22, BBC Micro 40,
  SC-3000 11, and the CYM song as VGM: all 437 the same, and one SG-1000 song (Gulkave 11,
  a tiny file) that ZXTune refused now plays. The lengths are the same in all. The cores
  that changed are used by no song of these: two VGMs made for the test differ, an SAA1099
  tone (Valley Bell's core against MAME's: the same level, difference -12 dB) and a PC
  Engine tone (Ootake against MAME: MAME's 6.7 dB louder). The rest of ZXTune is as before:
  the other 1,178 census songs of ZXTune and Vortex Tracker II's 12, byte for byte. The
  census: every VGM and S98 format plays as before (S98 40 of 40, MSX1 and MSX2 40, Vectrex
  20, ColecoVision 11, Virtual Boy 9, Spectrum VGM 3, X68000 1, CYM 1, Master System and
  SG-1000 40). Built from bbmp-sources, `bbmp-vgm` is the same bytes as built from the clone.
- **ZXTune without Sony's PlayStation 2 BIOS (8 October 2026):** Highly Experimental,
  ZXTune's PSF and PSF2 player, runs a trimmed image of Sony's SCPH-10000 BIOS (its own
  `mkhebios` makes it: the BIOS's sound and PlayStation 1 modules, with newer IOP
  modules and Highly Experimental's own put in), which ZXTune compiles into the program.
  `csrc/zxtune/no-ps2-bios.patch` reads it at run time instead, from the file
  `BBMP_PS2_BIOS` names; the player names `hebios.bin` of the extras folder when it is
  there and is the known image (SHA-256). Without it a PlayStation song is refused with
  one line that points to `bbmp extras`. The build scripts delete ZXTune's copy
  (`scph10000_he.inc`) from the tree; no 64-byte piece of it, packed or unpacked, is left
  in `bbmp-zxtune` or `bbmp-ay-vt2`. The full 4 MB dump can't be turned into the image by
  the player: `mkhebios` puts in IOP modules of a later BIOS (Sony's, version 2.6) that
  it carries in its source, and those can't be shipped either. **Checked, 20 seconds, byte
  for byte, with the image as the user's file:** the census's 100 PSF and 40 PSF2 songs
  the same as the helper with the image built in; without the file all 140 refused with
  that line; the other 1,097 census songs of ZXTune and Vortex Tracker II's 12 the same.
- **`bbmp-vgm`'s player is the player's own (7 October 2026, later):** libvgm's player
  and glue (player/, utils/, SoundEmu, the resampler, the panning, the DAC stream
  control, the chips' interfaces) state no licence, so none of it is built any more.
  `csrc/vgm` reads VGM (1.00-1.72, GD3) and S98 (v1-v3, its tags) and plays their
  commands, data blocks (also the compressed ones), PCM RAM writes and DAC streams,
  written from the formats' published documents only; only libvgm's licensed chip cores
  are compiled (`tools/build-vgm.sh` lists them with their licences and stops on a core
  that states none). How it should sound was taken from the earlier helper by listening
  to it (renders of test files, never its code): which core plays which chip (EMU2149 for
  the AY and the OPN's SSG, EMU2413, AdLibEmu for the YM3812 and YMF262, NSFPlay for the
  NES), how output is averaged or interpolated to 48 kHz (positions in 1/2048 of a core's
  sample, the sample before a command made on its own), each chip's share of the mix
  (the sum of the shares halves the gain at 0x60 times each power of two; S98 gives every
  chip the same volume, an OPN's SSG 0.8 of it), what happens at a song's end (one sample
  made and dropped, the chips reset, the positions going on) and in a seek (the commands
  run to the position, output starts there afresh). Damaged files are safe: every size
  and offset is checked against the file, a .vgz is unpacked to at most 64 MB (a cut one
  as far as it goes), commands that run past the end end the song, a loop with no wait
  ends, absurd clocks and stream rates are refused, the SCC core's register numbers are
  checked. **Checked against the earlier helper**, the census's 440 VGM and S98 songs,
  the first 60 seconds: 424 byte for byte the same, all headers and lengths the same; the
  16 others are told in csrc/vgm/README.txt and the knowledge notes (some 1.50 Mega Drive
  logs whose YM2612 the old player treated as older emulators did, three Virtual Boy
  songs and the X68000 song where a write lands one core sample apart, one S98 and one AY
  song with small differences).
- **Checked (7 October 2026), the old helpers against the new, byte for byte (the JSON
  line and the sound):** SNDH, the census's 40 songs, tracks 1 and 2, 60 seconds each
  (80 renders); SNSF, the census's 40 songs, 60 seconds; the other eleven engines of
  `bbmp-chip`, 5 songs each, 20 seconds (55); ZXTune, the census's songs of the 53
  formats it plays or was tried on, 20 seconds (1,421, of which 63 refused alike); Vortex
  Tracker II, its 12 songs, 20 seconds. All the same. Built from bbmp-sources,
  `bbmp-psgplay` and `bbmp-snsf` are the same bytes as built from the clones.
