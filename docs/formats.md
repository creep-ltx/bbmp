# Formats

Every format in the collections, whether the player plays it, and what is known about how well. Made by `tools/format_status.py` from the census (`tools/format_census.json`) and the notes (`docs/format-notes.toml`); change those and run it again.

Nobody has listened to all of this, and nothing was compared with a real machine. "Plays" means: the replayer gives sound that is neither silence nor noise.

The census tries a sample of each format's songs (40 at the most): Modland's where Modland has the format, else those of the collections that have it. A format that two collections have is counted on Modland's copies. Tried, by collection: Modland (423 formats, 10,422 songs), ASMA (1 format, 40 songs), HVSC (3 formats, 120 songs), ZX-Art (14 formats, 68 songs). ZX-Art's songs (its site isn't mirrored) are five a format at the most; HVSC's are counted both as Modland's format HVSC and as HVSC's own three groups.

| | Formats | Songs |
|---|---|---|
| Verified against the original | 31 | 94,221 |
| Plays, no fault known | 348 | 422,413 |
| Plays with faults or gaps | 37 | 22,356 |
| Switched off | 0 | 0 |
| Doesn't play | 22 | 531 |
| **All** | **438** | **539,521** |

Counting every format by the share of its tried songs that play, about **99.6%** of the 539,521 songs play.

## Verified against the original

The replayer's output, or what it writes to the sound chip, was compared with the original replay routine and is the same. This is as close to certain as it gets without listening on the real machine.

| Format | Songs | Play | Replayer | Notes |
|---|---|---|---|---|
| HVSC | 61,157 | 40 of 40 tried | sidplayfp | A tune is its own original player (the composer's 6502 code), so the C64 emulation was checked: every change libsidplayfp's C64 makes to the SID, against VICE's, the reference C64 emulator, running the same tune (tools/verify/sid.py): 956 of 996 random tunes the same, change for change, in order, and in time within 7 cycles at the median (30 s each, 2 October 2026). Of the rest: 21 are RSID programs playing samples through the volume register, whose timing is to the cycle, and part from VICE after a while; 7 read the SID's oscillator 3 for random numbers, which is the chip emulation's own (reSIDfp is not VICE's reSID); 12 differ otherwise. Tunes for two or three SIDs were not compared. HVSC's own three groups, tried from a copy of HVSC: Musicians, Games and Demos 40 of 40 each. |
| Spectrum - Pro Tracker 3 | 17,439 | 40 of 40 tried | zxtune | Every write to the AY chip, frame by frame, against Vortex Tracker II's own Z80 player (S.V. Bulba) on an emulated Z80, for all 7,376 of Modland's songs: 7,218 of the 7,297 compared the same in every frame (tools/verify). Of the rest: 88 have a channel that plays before it chooses a sample, which Pro Tracker 3 leaves undefined (played with sample 1, as Vortex Tracker's editor does); 16 start a sample or ornament past its end, where the original plays whatever bytes follow for a frame; 11 files are cut short on Modland; 52 differ, mostly in a few frames of a slide. Two faults of ZXTune were found and mended: a note and its ornament, and the channels' envelope offsets, are added in a byte, as in the original. Turbo Sound songs (two chips, 61) were not compared. |
| Spectrum - Pro Tracker 2 | 13,440 | 40 of 40 tried | zxtune | Every write to the AY chip, frame by frame, against Pro Tracker 2.101's own Z80 player (GDC, from a magazine's listing of it) on an emulated Z80, for all 6,243 of Modland's songs: 5,966 the same in every frame (tools/verify). Four faults of ZXTune were found and mended: the volume table is rounded, a pattern plays to its end however long, a 00 in a row is a noise command, and a note past the 96 notes' table plays what the original player's memory has there (the start of its volume table). Of the rest: 195 differ in a few frames where such a note is involved, 42 have a channel that plays before it chooses a sample (undefined in the original), 4 play as S.V. Bulba's player, which mends faults of the original, 1 file is cut short; 35 differ. The original plays a song's first row after its delay less a frame; the player plays it at once. |
| Musicline Editor | 817 | 815 of 817 | musicline | Every write to the sound chip matches the original Amiga routine, in 59 songs with 4 channels. The waves its synthesizer effects make, and songs with 8 channels, were compared by sound only. One file is cut short, one is silent. |
| FAC SoundTracker | 644 | 644 of 644 | fac | Written for this player in a clean room, from a description of the format only; the same chip writes as before in all 644 songs, and the same sound sample for sample (those writes match FAC's own routine in 60 songs on both chips). Open: 50 or 60 Hz, which the files don't say; played at 60. Six songs name a drum kit that is missing and play without drums. |
| Sound Club | 170 | 169 of 170 | soundclub | Written for this player in a clean room, from a description of the format only; the same sound as before in every song, and that was compared sample by sample with the original program's own rendering: all within 0.2%, most within 0.01%. It slides and mixes as the program does (voices mixed in runs, interpolation chosen per block). The last 0.17 seconds differ: the program's export drops its last part-filled buffer. One file is damaged; the original program can't load it either. |
| SCC-Musixx | 97 | 97 of 97 | sccmusixx | A replayer written for this player from a description of the tracker's replay routine (clean room): every write to the chip matches the routine in all 97 songs, and the audio is byte for byte that of the port of the routine it replaced. |
| Spectrum - Fuxoft AY Language | 72 | 39 of 39 | fxm | 27 of 30 songs register for register the same as the original Z80 players in Modland's AY rips. Three songs differ in a few places (notes above the note table). Nine have no rip to compare with. |
| Synder SNG-Player Stereo | 67 | 67 of 67 | synder | As Synder SNG-Player: sample for sample the same as the original program's own recordings of all 133 songs. The original keeps voices 5 to 8 in the same memory as its noise table, which colours the sound of the 6- and 8-voice songs; the player keeps that fault, so they sound as in the original. |
| Synder SNG-Player | 66 | 66 of 66 | synder | Written for this player from a description of the original's behaviour (clean room): sample for sample the same as the original program's own recordings of all 133 songs of both Synder formats. The files are not C64 register dumps but six synthesizer settings per voice per frame, played on the SNG-Player's own small synthesizer at 48000 Hz. Each song plays once to its end. |
| NerdTracker 2 | 54 | 54 of 54 | ned | 8 of 10 songs write for write the same as their composers' own NSF files. Plays at the tracker's 50 Hz, not an American NES's 60. |
| AXS | 26 | 26 of 26 | axs | Written for this player from a description of how AXS's Winamp plug-in sounds (clean room): sample for sample the same as the plug-in's output in all 26 songs, about 126 minutes in all. AXS, Resolution Audio's Windows synthesizer and tracker (1998-2001): seven two-oscillator synthesizers, a drum sampler, a delay and a reverb. One song jumps back for ever; it stops at the length AXS itself gives it. |
| Gameboy Tracker | 25 | 25 of 25 | mgb | Written for this player in a clean room, from a description of the format only; the same GBS image as before for every song; chip writes match the tracker's own driver in 24 of 25 songs. One song (il saint) differs in 46 writes. |
| Sound Club 2 | 25 | 25 of 25 | soundclub | Written for this player in a clean room, from a description of the format only; the same sound as before in every song, and that was compared sample by sample with the original program's own rendering: all within 0.2%. |
| Picatune | 20 | 20 of 20 | picatune | A replayer written for this player in a clean room, from a description of the format only; sample for sample against the 17 programs BeRo exported from his example songs. All 17 the same to the last bit of 16-bit sound (the exported player rounds toward zero, and so does this one now). Plays a semitone lower than 15 of those programs, as the 2007 tracker and his last two exports do. |
| BeRoTracker | 19 | 19 of 19 | berotracker | Written for this player from a description of the tracker's behaviour (clean room): bit for bit the same as BeRoTracker 1.00's own output in all 19 songs, on x86-64. BeRo's Impulse-Tracker-like tracker with a floating-point mixer and built-in effects. Its sound depends on the x87 unit's arithmetic; on other processors than x86 the songs sound the same but are not bit for bit the tracker's. |
| TSS | 14 | 14 of 14 | tss | 10 songs sample for sample the same as the author's JavaScript version. |
| Buzzic 2 | 12 | 12 of 12 | buz | Sample by sample the same as Buzzic 2's own synthesizer (Stan's code), all 12 songs; played by a replayer written for this player from a description of it (clean room). |
| Ad Lib - Pixel Painters FMF | 11 | 11 of 11 | pixelpainters | A replayer written for this player in a clean room, from a description of the formats only: register for register, every tick, as Electranoid's own routines run in an emulated 8086. One song has five silent voices where the game leaves default instruments. |
| Ad Lib - Pixel Painters FTF | 8 | 8 of 8 | pixelpainters | A replayer written for this player in a clean room, from a description of the formats only: register for register, every tick, as Xatax's own routines run in an emulated 8086. Played as on a Sound Blaster Pro 2, with the songs' own OPL3 panning. |
| The 0ok Amazing Synth Tracker | 8 | 8 of 8 | t0ast | Written for this player from a description of the tracker's behaviour (clean room): bit for bit the same as the tracker's own WAV export in all 77 songs tried, on x86-64. T0AST, BeRo's first synthesizer for 64k intros (2004-06). Its sound depends on the x87 unit's arithmetic; on other processors than x86 the songs sound the same but are not bit for bit the tracker's. |
| Digital Sound Interface Kit | 7 | 7 of 7 | dsm | The same waveform as the S3M originals of three songs. |
| Ad Lib - Sound Images Generation 2 | 4 | 4 of 4 | sig | A replayer written for this player in a clean room, from a description of the format only: the chip's registers after every tick as with Tony Williams' driver (from Kwik Snax) run in an emulated 8086. Each file holds a theme and two jingles, played as three tracks. |
| Buzzic | 4 | 4 of 4 | buz | Version 1.0 sample by sample the same as the player Buzzic generates; played by a replayer written for this player from a description of the format (clean room), bit for bit the same as before. The one song of version 1.1 has nothing to compare with. |
| Monotone | 4 | 4 of 4 | monotone | The PC speaker's timer value on every tick, against the tracker's own disk writer (10 songs, about 48,000 ticks). A port of MONOTONE's own player and PC speaker output (public domain). All the same, tick for tick; only where a song ends is chosen differently (the song is played to where it repeats). |
| Ad Lib - EdLib D01 | 3 | 3 of 3 | d01 | A replayer written for this player in a clean room, from a description of the format only: the chip's registers after every tick, and the samples on the same ticks, as JCH's player 1.03 run in an emulated 8086. The samples come from the .s01 beside the song; how loud they are mixed is a judgement (DOSBox's level). |
| Ad Lib - Sound Interface System | 3 | 3 of 3 | lem | A replayer written for this player in a clean room, from a description of the format only: register for register, every tick, as the Sound Images driver in Xmas Lemmings run in an emulated 8086. Played at 70 Hz, the VGA frame rate the game's timer follows. Modland's rip of one song lacks a short rest pattern it shares with another; the player supplies it. |
| Ad Lib - FM Tracker | 2 | 2 of 2 | fmtracker | A replayer written for this player in a clean room, from a description of the format only: the same OPL writes as FM Tracker Player 1.26 (FMTP.EXE) in all songs. Every write to the chip matches FMTP.EXE, run in an emulated DOS PC, in both songs and the tracker's six example songs. Played once, up to where the song goes back to its start. |
| Ad Lib - MUS | 1 | 1 of 1 | samurai | A replayer written for this player in a clean room, from a description of the format only: the same OPL writes as the original in all songs. The file is First Samurai's whole Ad Lib sound module; only its data is read. 4 tunes, each matching the game's own routine write for write. |
| Ad Lib - Shadowlands | 1 | 1 of 1 | shadowlands | A replayer written for this player in a clean room, from a description of the format only: the same OPL writes as the original in all songs. The game's TUNE.CAL, unpacked. Every write to the chip matches the game's driver (3 minutes at 50 Hz). The tune loops back to its seventh order; played once. |
| Ad Lib - Vibrants | 1 | 1 of 1 | vibrants | A replayer written for this player in a clean room, from a description of the format only: the chip's registers the same as with the original after every tick, in all songs. Unchanged registers are written again each tick. An intro, then a loop of 73 seconds, played once (1:42 at 70 Hz). |

## Plays, no fault known

Every song that was tried plays, and nothing is known to be wrong. Most of these are played by libraries that have been in use for years. That is not the same as verified: nobody has compared them with the original for this player.

| Format | Songs | Play | Replayer | Notes |
|---|---|---|---|---|
| Protracker | 81,701 | 40 of 40 tried | openmpt |  |
| Fasttracker 2 | 47,371 | 40 of 40 tried | openmpt |  |
| Nintendo SPC | 36,903 | 40 of 40 tried | gme |  |
| Nintendo DS Sound Format | 30,095 | 40 of 40 tried | zxtune |  |
| Impulsetracker | 25,779 | 40 of 40 tried | openmpt |  |
| Gameboy Sound Format | 23,072 | 40 of 40 tried | zxtune |  |
| Playstation Sound Format | 16,057 | 100 of 100 tried | zxtune | Played by Highly Experimental, which runs the PlayStation 2's BIOS: hebios.bin in the extras folder, made from a dump of your own console's BIOS (bbmp extras). The player doesn't carry it; without it no song plays. With it 100 of 100 tried play (8 October 2026). |
| FM sound driver (FMP) | 11,527 | 40 of 40 tried | fmp |  |
| SNDH | 11,381 | 40 of 40 tried | sndh |  |
| Screamtracker 3 | 11,267 | 40 of 40 tried | openmpt |  |
| Video Game Music - Sega Megadrive | 10,961 | 40 of 40 tried | gme |  |
| Slight Atari Player | 9,565 | 39 of 40 tried | zxtune | One file Modland and ASMA have is a broken rip. One whose last block is a few bytes short is played from a mended copy. |
| Playstation 2 Sound Format | 6,911 | 40 of 40 tried | zxtune | Played by Highly Experimental, which runs the PlayStation 2's BIOS: hebios.bin in the extras folder, made from a dump of your own console's BIOS (bbmp extras). The player doesn't carry it; without it no song plays. With it 40 of 40 tried play (8 October 2026). |
| Spectrum - ST Song Compiler | 6,378 | 40 of 40 tried | zxtune |  |
| S98 | 5,111 | 40 of 40 tried | vgm |  |
| Sidplayer | 5,032 | 40 of 40 tried | sidplayfp |  |
| Nintendo Sound Format | 5,015 | 40 of 40 tried | gme |  |
| YM | 4,974 | 40 of 40 tried | ym |  |
| Dreamcast Sound Format | 4,534 | 37 of 40 tried | zxtune | 62 of Naomi's 1,218 songs are empty tracks of their rips: no music, which ZXTune and the Audio Overload SDK agree on; they are marked. Skies of Arcadia's songs name their libraries with "_" where Modland has "-"; the player allows for it. |
| AY Emul | 4,235 | 40 of 40 tried | gme |  |
| RealSID | 3,540 | 40 of 40 tried | sidplayfp | Tunes written in BASIC need the C64's ROMs (kernal.bin, basic.bin, chargen.bin in the extras folder): with them all 40 tried play (1 October 2026), without them five of the 40 are silent. |
| Spectrum - ASC Sound Master | 3,190 | 40 of 40 tried | zxtune |  |
| OctaMED MMD1 | 2,728 | 40 of 40 tried | openmpt |  |
| Spectrum - Sound Tracker Pro | 2,459 | 40 of 40 tried | zxtune |  |
| Video Game Music - Sega Master System | 2,102 | 40 of 40 tried | gme |  |
| Fasttracker | 2,036 | 40 of 40 tried | openmpt |  |
| MoonBlaster | 1,999 | 40 of 40 tried | kss |  |
| Soundtracker | 1,936 | 40 of 40 tried | openmpt |  |
| Deflemask | 1,813 | 79 of 100 tried | furnace | Furnace. About one in five of Modland's files was once sent as text, which damaged it (every CR LF made LF, and every CR CR changed too). Furnace plays most of them anyway, perhaps with a wrong note or a click where the damage is; 21 of 100 tried it can't. Mending them was tried: 1 of 46 came back whole. |
| FamiTracker | 1,784 | 40 of 40 tried | furnace |  |
| Piston Collage | 1,783 | 39 of 40 tried | pxtone |  |
| SC68 | 1,775 | 100 of 100 tried | sc68 |  |
| Spectrum - SQ Tracker | 1,626 | 40 of 40 tried | zxtune |  |
| PMD | 1,510 | 40 of 40 tried | pmdc | A driver written for this player from a description of PMD's behaviour (clean room): the same YM2608 register writes, at the same moments, as PMDWin in all 1,510 songs; the chip by ymfm. The sound is ymfm's YM2608, not fmgen's: against the old player, loudness within 0.1 dB on average (-3.2 to +1.4 dB in 40 songs), the same timing. The drums need the YM2608 rhythm ROM (an extra file). None of the PCM files the songs name (P86, PPS, PZI) is on Modland, so ADPCM and PPZ8 parts are silent. Songs under a second long no longer play for three minutes. |
| Euphony | 1,499 | 40 of 40 tried | eup |  |
| Stereo Sidplayer | 1,447 | 40 of 40 tried | sidplayfp |  |
| OctaMED MMD0 | 1,441 | 40 of 40 tried | openmpt |  |
| AHX | 1,390 | 40 of 40 tried | uade |  |
| MGSDRV | 1,388 | 40 of 40 tried | kss |  |
| YMST | 1,339 | 100 of 100 tried | uade |  |
| Spectrum - Vortex | 1,007 | 40 of 40 tried | zxtune |  |
| Gameboy Sound System | 918 | 40 of 40 tried | gme |  |
| Digibooster Pro | 845 | 40 of 40 tried | openmpt |  |
| MuSICA | 808 | 40 of 40 tried | kss |  |
| Multitracker | 790 | 40 of 40 tried | openmpt |  |
| IFF-SMUS | 756 | 40 of 40 tried | uade | UADE's Sonix player. Two of Mark Riley's songs that Modland lists are not on its server. wildman plays without its "Bpart" instrument, which Modland lacks, and Gilsons Green Grape's bass is made from its sample (its .instr is missing). |
| OctaMED MMD3 | 660 | 40 of 40 tried | openmpt |  |
| Video Game Music - Sega Game Gear | 652 | 40 of 40 tried | gme |  |
| Spectrum - Sound Tracker Pro 2 | 632 | 40 of 40 tried | zxtune |  |
| Super Nintendo Sound Format | 608 | 40 of 40 tried | snsf |  |
| Quartet ST | 568 | 100 of 100 tried | uade |  |
| Spectrum - Digital Studio | 471 | 5 of 5 tried | zxtune |  |
| Ad Lib - Creative Music File | 459 | 40 of 40 tried | adplug |  |
| Spectrum - Pro Sound Creator | 456 | 40 of 40 tried | zxtune |  |
| Oktalyzer | 449 | 40 of 40 tried | openmpt |  |
| SGC | 386 | 384 of 386 | sgc | Against Game Music Emu's SGC player in 746 songs: 84% alike, 14% roughly, 1% not. Two ColecoVision songs need the ColecoVision BIOS, which the player doesn't have. Songs without a track list have the game's sound effects among their subsongs, and their length is a guess that is wrong for about 1 in 10. |
| OPLLDRV | 373 | 40 of 40 tried | kss |  |
| TFMX | 368 | 40 of 40 tried | uade |  |
| V2 | 367 | 40 of 40 tried | zxtune |  |
| X-Tracker | 366 | 40 of 40 tried | openmpt |  |
| SoundFX | 358 | 40 of 40 tried | openmpt |  |
| Organya | 338 | 40 of 40 tried | organya |  |
| Ad Lib - Raw OPL Capture | 316 | 40 of 40 tried | adplug |  |
| Piston Collage Protected | 315 | 40 of 40 tried | pxtone |  |
| OctaMED MMD2 | 305 | 40 of 40 tried | openmpt |  |
| Capcom Q-Sound Format | 298 | 40 of 40 tried | qsf |  |
| Ad Lib - LucasArts | 284 | 40 of 40 tried | adplug |  |
| Ad Lib - AdLib Tracker 2 | 278 | 40 of 40 tried | adplug |  |
| TFM Music Maker | 266 | 40 of 40 tried | furnace |  |
| Megadrive GYM | 265 | 40 of 40 tried | gme |  |
| Ad Lib - Visual Composer | 247 | 40 of 40 tried | adplug |  |
| SoundSmith | 243 | 243 of 243 | soundsmith | Written for this player in a clean room, from a description of the formats only; the same chip writes as before in every song (243 of 243), and the same sound sample for sample; tempo and notes against MTPng, another player. Never compared with an Apple IIGS or an emulator of one. 33 songs use instruments their wave bank lacks; those notes are silent, as on the IIGS. |
| Ad Lib - HSC AdLib Composer | 236 | 40 of 40 tried | adplug |  |
| Screamtracker 2 | 234 | 40 of 40 tried | openmpt |  |
| BP SoundMon 2 | 214 | 40 of 40 tried | uade |  |
| Art Of Noise | 211 | 40 of 40 tried | uade |  |
| Ad Lib - Apogee | 201 | 40 of 40 tried | adplug |  |
| Ad Lib - Loudness Sound System | 184 | 40 of 40 tried | adplug |  |
| Ad Lib - EdLib Packed | 179 | 92 of 179 | edlib | Converted as the editor converts (the same files as the editor made), by a converter written for this player in a clean room from a description of the format only; played by AdPlug. Every song plays. 87 files are damaged on Modland: they hold pieces of an archive and no song. |
| Future Composer 1.3 | 175 | 40 of 40 tried | openmpt |  |
| Symphonie | 174 | 40 of 40 tried | openmpt |  |
| Composer 669 | 172 | 40 of 40 tried | openmpt |  |
| Digibooster | 168 | 40 of 40 tried | openmpt |  |
| Digital Symphony | 168 | 40 of 40 tried | openmpt |  |
| WonderSwan | 168 | 168 of 168 | wsr | Mednafen's emulation; nothing to compare with. Songs loop; the player finds the loop and plays it once. A file has the game's sound effects among its songs. |
| Spectrum - Pro Tracker 1 | 163 | 40 of 40 tried | zxtune |  |
| Future Composer 1.4 | 152 | 40 of 40 tried | openmpt |  |
| Spectrum - CHIPNSFX | 145 | 145 of 145 | chipnsfx | Played by the tracker's own code (CHIPNSFX.C, GPL-3): the same samples as its WAV export, but for a filter that takes away the steady level its 8-bit sound has. Stereo is the tracker's ABC, a quarter of each side mixed into the other. Older catalogs call it Spectrum - CHP. |
| Ad Lib - AMusic | 143 | 40 of 40 tried | adplug |  |
| Digital Sound And Music Interface | 139 | 40 of 40 tried | openmpt |  |
| Video Game Music - Sega 32X | 136 | 40 of 40 tried | gme |  |
| Music Editor | 132 | 40 of 40 tried | xmp |  |
| Ad Lib - EdLib D00 | 122 | 40 of 40 tried | adplug |  |
| BP SoundMon 3 | 121 | 40 of 40 tried | uade |  |
| David Whittaker | 115 | 40 of 40 tried | uade |  |
| Digital Tracker DTM | 114 | 40 of 40 tried | openmpt |  |
| Octalyser | 111 | 40 of 40 tried | openmpt |  |
| SidMon 2 | 107 | 40 of 40 tried | uade |  |
| Spectrum - Digital Music Maker | 107 | 5 of 5 tried | zxtune |  |
| Beepola | 105 | 105 of 105 | beepola | A clean-room rewrite of Beepola's eight engines, the same as the old Z80 engines sample for sample in all 105 songs. Most songs need three pieces of data the player downloads on request (bbmp download beepoladata); the others play without them. The ROM Beep engine plays two octaves above the other engines, as Beepola and the Spectrum's BEEP do. |
| Ad Lib - Sierra | 104 | 40 of 40 tried | adplug |  |
| His Master's Noise | 104 | 40 of 40 tried | openmpt |  |
| Polytracker | 101 | 40 of 40 tried | openmpt |  |
| Delta Music 2 | 98 | 40 of 40 tried | uade | UADE's DeltaMusic 2.0 player. Its channels loop each on their own, and UADE's player says some songs have ended while the music goes on (12 of the 98, Sanxion's loader after 30 ms): those play on, and the player stops them at 3 minutes with a fade. |
| Hippel ST COSO | 97 | 40 of 40 tried | uade |  |
| Face The Music | 95 | 40 of 40 tried | openmpt |  |
| Spectrum - Flash Tracker | 91 | 66 of 66 | fls | Three songs against their STC versions: same length, same pitch. Played at 50 Hz; a real Pentagon has 48.8, 2.4% slower. |
| Ultratracker | 91 | 40 of 40 tried | openmpt |  |
| Epic Megagames MASI | 90 | 40 of 40 tried | openmpt |  |
| GoatTracker | 90 | 90 of 90 | goattracker | The editors' own play routines; compared with the C64 programs of the same songs in HVSC. Songs in the newest format: 43 of 62 the same as in HVSC. GoatTracker 1 songs agree with their C64 programs where the same play routine made them (the editor's routine differed from the C64's; it is mended at the build). Other HVSC programs come from other versions of the editor. A file doesn't say its speed or SID model; a table has them for 74 songs. |
| MO3 | 89 | 40 of 40 tried | openmpt |  |
| Digital Mugician | 86 | 40 of 40 tried | uade |  |
| Spectrum - Fast Tracker | 81 | 40 of 40 tried | zxtune |  |
| MaxTrax | 79 | 79 of 79 | maxtrax | ScummVM's player with a fault of its tempo changes mended. Three Dune 2 files start with a sound effect of 0.2 seconds. |
| Richard Joseph | 76 | 40 of 40 tried | uade |  |
| Ad Lib - Surprise! AdLib Tracker 2.0 | 75 | 40 of 40 tried | adplug |  |
| Video Game Music - Sega SG-1000 | 74 | 40 of 40 tried | gme |  |
| Spectrum - Sound Tracker 1.1 | 72 | 40 of 40 tried | zxtune |  |
| Noise Tracker GS 1 | 71 | 71 of 71 | noisetracker | Written for this player in a clean room, from a description of the format only; the same chip writes as before in every song (71 of 71), and the same sound sample for sample; tempo against the Amiga original of one song. Pitch could not be settled by the comparison. |
| Video Game Music - BBC Micro | 71 | 40 of 40 tried | gme |  |
| TIATracker | 70 | 70 of 70 | tiatracker | Played by the tracker's own song and replay code (GPL-2, a helper of its own) with Stella's TIA sound: the same TIA registers, frame by frame, as the tracker's unmodified code built with Qt, in all 134 songs tried. Atari 2600 music made with Kylearan's TIATracker (JSON); older catalogs call it Theta Music Composer. Played once to where its loop starts; not compared with the 2600's own player routine. |
| MusicMaker V8 | 69 | 40 of 40 tried | uade |  |
| Starkos | 65 | 65 of 65 | sks | Arkos Tracker 3's own converter and player. |
| General DigiMusic | 64 | 40 of 40 tried | openmpt |  |
| Pretracker | 64 | 40 of 40 tried | uade |  |
| Shroom | 63 | 63 of 63 | shroom | All 63 songs sample for sample the same as Mariopants, another player, set to its timing. The files don't say where a song ends; it plays to its last note. |
| Digitrakker | 61 | 40 of 40 tried | openmpt |  |
| Graoumf Tracker 2 | 61 | 40 of 40 tried | openmpt |  |
| SidMon 1 | 61 | 40 of 40 tried | uade |  |
| Infogrames | 60 | 40 of 40 tried | uade |  |
| Game Music Creator | 59 | 40 of 40 tried | openmpt |  |
| Coconizer | 57 | 57 of 57 | xmp | Libxmp. |
| Spectrum - Pro Sound Maker | 55 | 40 of 40 tried | zxtune |  |
| Video Game Music - MSX1 | 54 | 40 of 40 tried | vgm |  |
| Graoumf Tracker | 52 | 40 of 40 tried | openmpt |  |
| JamCracker | 48 | 40 of 40 tried | uade |  |
| Startrekker AM | 47 | 39 of 40 tried | xmp |  |
| Chiptracker | 45 | 40 of 40 tried | openmpt |  |
| Dave Lowe | 45 | 40 of 40 tried | uade |  |
| Ben Daglish | 44 | 40 of 40 tried | uade |  |
| HivelyTracker | 44 | 40 of 40 tried | zxtune |  |
| Video Game Music - MSX2 | 44 | 40 of 40 tried | vgm |  |
| Ad Lib - ADL | 43 | 40 of 40 tried | adplug |  |
| MultiMedia Sound | 43 | 40 of 40 tried | openmpt |  |
| Pumatracker | 43 | 40 of 40 tried | openmpt |  |
| Spectrum - Chip Tracker | 43 | 21 of 21 tried | zxtune |  |
| Ad Lib - Screamtracker 3 AdLib | 42 | 40 of 40 tried | openmpt |  |
| Asylum | 42 | 40 of 40 tried | openmpt |  |
| Spectrum - Global Tracker | 42 | 38 of 38 tried | zxtune |  |
| Ad Lib - Ken's AdLib Music | 41 | 41 of 41 | adplug | AdPlug. |
| Dave Lowe New | 41 | 40 of 40 tried | uade |  |
| ProTrekkr 2.0 | 41 | 41 of 41 | protrekkr | ProTrekkr's own replayer. |
| Organya 2 | 39 | 39 of 39 | organya |  |
| Spectrum - Sound Tracker 1.3 | 38 | 3 of 3 tried | zxtune |  |
| Farandole Composer | 37 | 37 of 37 | openmpt |  |
| Jesper Olsen | 37 | 37 of 37 | jesperolsen | A replayer written for this player from a description of the games' drivers (clean room): the same Paula register writes, in the same ticks, as the games' own routines under UADE, in all 55 tracks of the 35 LollyPop, Georg Glaxo and Harald Hardtand songs; the two Guldkornsexpressen songs carry their own routine and play through UADE. The sound is ScummVM's Paula (A500 filter, UADE's stereo of 0.4), not UADE's: loudness within 0.2 dB of the old player on 10 tracks. Georg Glaxo's songs are played with that game's driver, known by their contents. |
| Composer 670 (CDFM) | 36 | 36 of 36 | openmpt | Each .670 file is an older layout of the .c67 next to it. The .c67 plays; the .670 files are hidden. |
| Special FX ST | 36 | 36 of 36 | uade |  |
| Synthesis | 36 | 36 of 36 | uade |  |
| JayTrax | 35 | 35 of 35 | jxs | A replayer written for this player from a description of the format (clean room); with BBMP_JXS_TODAY=1 bit for bit the same as the CrossX engine it replaces, on all 67 tracks. Played as the original would sound without three faults of the engine used before: each track from a fresh start (it played every track once silently first, and 23 tracks carried settings from that pass into what was heard), the dyna-morph effect's sine wave made (it was never made: 26 tracks use it), and no leftover memory at a track's start. Not compared with the JayTrax editor itself. |
| Spectrum - ZXS | 35 | 35 of 35 | zxtune |  |
| Spectrum - Extreme Tracker | 34 | 5 of 5 tried | zxtune |  |
| AY Amadeus | 33 | 33 of 33 | fxm | The same engine as Fuxoft AY Language, which is verified. |
| Sound Images | 33 | 33 of 33 | uade |  |
| Cybertracker C64 | 32 | 32 of 32 | cybertracker | The C64 tracker itself on an emulated C64. |
| Stonetracker | 32 | 32 of 32 | stonetracker | The tracker's own replay routine, run by UADE. A song that jumps back to an earlier position as part of its structure would be ended there. |
| Gameboy Sound System GBR | 31 | 31 of 31 | gbr | A file doesn't say how many songs it has or which are music. |
| SoundPlayer | 31 | 31 of 31 | uade |  |
| Special FX | 31 | 31 of 31 | uade |  |
| Digital Sound Studio | 30 | 30 of 30 | uade |  |
| Ad Lib - Bob's AdLib Music | 29 | 29 of 29 | adplug |  |
| Spectrum - Multitrack | 29 | 5 of 5 tried | zxtune |  |
| Ad Lib - Beni Tracker | 28 | 28 of 28 | adplug |  |
| Ad Lib - Surprise! AdLib Tracker | 28 | 28 of 28 | adplug |  |
| AProSys | 28 | 28 of 28 | uade |  |
| Maniacs Of Noise | 28 | 28 of 28 | uade |  |
| Ad Lib - Mlat Adlib Tracker | 26 | 26 of 26 | adplug |  |
| Ad Lib - Twin TrackPlayer | 26 | 26 of 26 | adplug |  |
| Spectrum - PSG | 26 | 5 of 5 tried | zxtune |  |
| Activision Pro | 25 | 25 of 25 | uade |  |
| MikMod UNITRK | 25 | 25 of 25 | unitrk | Against the XM originals of four songs: same length, alike in sound. |
| Unique Development | 25 | 25 of 25 | uade |  |
| Velvet Studio | 25 | 25 of 25 | openmpt |  |
| Digital Sound Interface Kit RIFF | 24 | 24 of 24 | openmpt |  |
| Ixalance | 24 | 24 of 24 | ixalance | The player's own iXalance engine, written in a clean room, the same as the old plugin sample for sample in all 24 songs (7 of them need three library samples the player downloads on request: bbmp download ixalancesamples). |
| Onyx Music File | 24 | 24 of 24 | onyx | Written for this player in a clean room, from a description of the format only; made into a module that libopenmpt plays, the same module and sound as before in all 24 songs. Not compared with the original program. |
| TCB Tracker | 24 | 24 of 24 | openmpt |  |
| Wally Beben | 24 | 24 of 24 | uade |  |
| Maximum Effect | 23 | 23 of 23 | uade | Seven songs are damaged on Modland (a fill byte where their tables have zeros) and play from a copy with the zeros put back. |
| MIDI-Loriciel | 22 | 22 of 22 | uade |  |
| Pierre Adane Packer | 22 | 22 of 22 | uade | UADE's player. |
| Synth Dream | 22 | 21 of 22 | uade | The songs play with their folder's shared smp.set. sdr.nobuddiesland 8 is damaged on Modland (fill bytes where its tables have zeros) and the replayer stops on it. |
| Video Game Music - Sega Mega CD | 22 | 22 of 22 | gme |  |
| Westwood SND | 22 | 22 of 22 | wwsnd | PC speaker sounds, mostly effects. The tick rate is ScummVM's for Eye of the Beholder and assumed for Kyrandia. |
| Ad Lib - Digital FM | 21 | 21 of 21 | adplug |  |
| ArkosTracker | 20 | 2 of 2 | aks | Arkos Tracker 3's own player. ZX-Art has 17 more: 4 of 5 tried play (2 October 2026); the fifth is a bank of sound effects (Chibi Akumas SFX) and gives no sound. |
| Video Game Music - Vectrex | 20 | 20 of 20 | vgm |  |
| Ad Lib - Master Tracker | 19 | 19 of 19 | adplug |  |
| Sound Master II v3 | 19 | 19 of 19 | uade |  |
| Soundtracker 2.6 | 19 | 19 of 19 | openmpt |  |
| Spectrum - General Sound | 19 | 5 of 5 tried | zxtune |  |
| Jason Brooke | 18 | 18 of 18 | uade | One song (pacland) is near silent in all its 10 subsongs. |
| SoundControl | 18 | 18 of 18 | uade |  |
| Ad Lib - MK-Jamz | 17 | 17 of 17 | adplug |  |
| Anders Oland | 17 | 17 of 17 | uade |  |
| Klystrack | 17 | 17 of 17 | klystrack | Retrovert's plugin. |
| PokeyNoise | 17 | 17 of 17 | uade | UADE's player. |
| Quadra Composer | 17 | 17 of 17 | uade |  |
| Synth Pack | 17 | 17 of 17 | uade | UADE's player. |
| GlueMon | 16 | 16 of 16 | uade |  |
| Sound Master | 16 | 16 of 16 | uade |  |
| SynTracker | 16 | 16 of 16 | uade |  |
| Ad Lib - Herad Music System | 15 | 15 of 15 | adplug |  |
| Ad Lib - Palladix | 15 | 15 of 15 | adplug |  |
| Atari Digi-Mix | 15 | 15 of 15 | ym |  |
| Speedy System | 15 | 15 of 15 | uade |  |
| The Musical Enlightenment | 15 | 15 of 15 | uade |  |
| Darius Zendeh | 14 | 14 of 14 | uade |  |
| Electronic Music System | 14 | 14 of 14 | uade |  |
| Fashion Tracker | 14 | 14 of 14 | uade |  |
| NovoTrade Packer | 14 | 14 of 14 | uade |  |
| OctaMED MMDC | 14 | 13 of 14 | uade |  |
| Paul Shields | 14 | 14 of 14 | uade |  |
| ProTrekkr | 14 | 14 of 14 | protrekkr | ProTrekkr's own replayer. |
| Sean Conran | 14 | 14 of 14 | uade |  |
| DSMI Compact | 13 | 13 of 13 | openmpt |  |
| Follin Player II | 13 | 13 of 13 | uade |  |
| Images Music System | 13 | 13 of 13 | openmpt |  |
| Ken's Digital Music | 13 | 13 of 13 | kdm | A replayer written for this player from a description of the format (clean room); with BBMP_KDM_FLAT=1 it is bit for bit the same as Ken Silverman's own C player on all 13 songs. Plays with the vibrato, tremolo and auto-pan of Ken's original DOS player, which the C port the player used before lost (its sine was always 0); 11 of 13 songs use them. A song is cut where it starts again, so its last note doesn't ring out. |
| Liquid Tracker | 13 | 13 of 13 | xmp |  |
| Paul Robotham | 13 | 13 of 13 | uade |  |
| TFMX ST | 13 | 13 of 13 | uade |  |
| Ad Lib - Ultima 6 | 12 | 12 of 12 | adplug |  |
| BoyScout | 12 | 11 of 12 | boyscout | Against BoyScout's own WAV renders (the tracker run in Wine, no sound out): tempo, pitch, notes. Game Boy Advance music. Played as BoyScout's GBA library plays an export. One song on Modland is empty. The tracker mixes its wave and noise channels a little quieter. |
| Kris Hatlelid | 12 | 12 of 12 | uade |  |
| Peter Verswyvelen | 12 | 8 of 12 | uade | The four Ziriax files are damaged on Modland: fill bytes (1A) where their tables have zeros, which can't be told from real data. |
| Rob Hubbard | 12 | 12 of 12 | uade |  |
| Spectrum - SQ Digital Tracker | 12 | 5 of 5 tried | zxtune |  |
| Spectrum - Vortex Tracker II | 12 | 12 of 12 | vt2 | Against ZXTune's own reading of the same songs. The files don't say which chip; YM is used. |
| CBA | 11 | 11 of 11 | openmpt |  |
| Digital Sonix And Chrome | 11 | 11 of 11 | uade |  |
| Protracker IFF | 11 | 11 of 11 | openmpt |  |
| SoundFactory | 11 | 11 of 11 | uade |  |
| Spectrum - Furnace | 11 | 5 of 5 tried | furnace |  |
| Vic-Tracker | 11 | 11 of 11 | victracker | Against the six recordings on the tracker's home page: tempo, pitch (0 cents) and level per octave. VIC-TRACKER's own player (2.0) on an emulated 6502 and VIC-20 sound chip. |
| Video Game Music - Colecovision | 11 | 11 of 11 | vgm |  |
| Video Game Music - Sega SC-3000 | 11 | 11 of 11 | gme |  |
| Ad Lib - SoundFX Macs Opera | 10 | 10 of 10 | adplug |  |
| CustomMade | 10 | 10 of 10 | uade |  |
| Electronic Music System v6 | 10 | 10 of 10 | uade |  |
| Lionheart | 10 | 10 of 10 | uade |  |
| SCUMM | 10 | 10 of 10 | uade |  |
| Zoundmonitor | 10 | 10 of 10 | zoundmonitor | UADE's player. |
| Ad Lib - Exotic AdLib | 9 | 9 of 9 | adplug |  |
| Ad Lib - Johannes Bjerregard Module | 9 | 9 of 9 | adplug |  |
| Arpeggiator | 9 | 9 of 9 | uade |  |
| Astroidea XMF | 9 | 9 of 9 | openmpt |  |
| Delta Music | 9 | 9 of 9 | uade |  |
| Disorder Tracker 2 | 9 | 9 of 9 | openmpt |  |
| Future Player | 9 | 9 of 9 | uade |  |
| Imago Orpheus | 9 | 9 of 9 | openmpt |  |
| Mike Davies | 9 | 9 of 9 | uade |  |
| Music Assembler | 9 | 9 of 9 | uade |  |
| NoiseTrekker 2 | 9 | 9 of 9 | protrekkr | Played 14 dB down, so no song clips. How loud NoiseTrekker 2 itself played is unknown: its source is lost. |
| PlayerPro | 9 | 9 of 9 | playerpro | PlayerPRO's own driver. |
| Ron Klaren | 9 | 9 of 9 | uade | One song (electricity) is misread by UADE as 2,418 subsongs, all silent. |
| Sound Master II v1 | 9 | 9 of 9 | uade |  |
| Sound Programming Language | 9 | 9 of 9 | uade |  |
| Spectrum - ProDigiTracker | 9 | 5 of 5 tried | zxtune |  |
| VGM Music Maker | 9 | 9 of 9 | vgemm | A replayer written after the editor's own VGM export (vgm_mm.exe, observed write for write). Mega Drive music (YM2612 and SN76489). All 9 songs: 99.98% of the chip writes the same as the editor's export, 8 songs entirely; Trinitron Sunset differs in 20 writes (a vibrato after a key-off). Samples within 1 to 3 steps of 8 bits of the editor's resampling. Plays once through the order list. |
| Video Game Music - Nintendo Virtual Boy | 9 | 9 of 9 | vgm |  |
| David Hanney | 8 | 8 of 8 | uade |  |
| Dynamic Studio Professional | 8 | 8 of 8 | openmpt |  |
| EasyTrax | 8 | 8 of 8 | openmpt |  |
| FM Tracker | 8 | 8 of 8 | openmpt |  |
| Jeroen Tel | 8 | 8 of 8 | uade |  |
| Mark Cooksey | 8 | 8 of 8 | uade |  |
| Ad Lib - Faust Music Creator | 7 | 7 of 7 | adplug |  |
| Ad Lib - FM-Kingtracker | 7 | 7 of 7 | adplug |  |
| Digital Mugician 2 | 7 | 7 of 7 | uade |  |
| Funktracker | 7 | 7 of 7 | zxtune |  |
| Jason Page Old | 7 | 7 of 7 | uade |  |
| SunTronic | 7 | 5 of 7 | uade |  |
| Voodoo Supreme Synthesizer | 7 | 7 of 7 | uade |  |
| Ad Lib - AMusic XMS | 6 | 6 of 6 | adplug |  |
| Ad Lib - Boom Tracker | 6 | 6 of 6 | adplug |  |
| Ashley Hogg | 6 | 6 of 6 | uade |  |
| Core Design | 6 | 6 of 6 | uade |  |
| Digital Tracker MOD | 6 | 6 of 6 | openmpt |  |
| Hippel 7V | 6 | 6 of 6 | uade |  |
| InStereo! 2.0 | 6 | 6 of 6 | uade |  |
| Janko Mrsic-Flogel | 6 | 6 of 6 | uade |  |
| Megatracker | 6 | 6 of 6 | xmp | Libxmp. |
| Spectrum - ASC Sound Master 0 | 6 | 5 of 5 tried | zxtune |  |
| Steve Barrett | 6 | 6 of 6 | uade |  |
| AM Composer | 5 | 5 of 5 | uade |  |
| Audio Sculpture | 5 | 5 of 5 | xmp |  |
| Dynamic Synthesizer | 5 | 5 of 5 | uade |  |
| Mark Cooksey Old | 5 | 5 of 5 | uade |  |
| Pollytracker | 5 | 5 of 5 | polly | Polly Tracker's own C64 player, taken from HVSC's Polly_Rock.sid, with the module behind it, played by libsidplayfp: the tunes made so are byte for byte HVSC's for three songs (Backwoods differs in two bytes of HVSC's copy of the song). The player is downloaded from Modland's HVSC folder the first time a Polly Tracker song is played. |
| Spectrum - Sample Tracker | 5 | 5 of 5 | zxtune |  |
| Ad Lib - Extra Simple Music | 4 | 4 of 4 | adplug |  |
| Art And Magic | 4 | 4 of 4 | uade |  |
| Fred Gray | 4 | 4 of 4 | uade |  |
| MCMD | 4 | 4 of 4 | uade |  |
| Medley | 4 | 4 of 4 | uade |  |
| Paul Summers | 4 | 4 of 4 | uade |  |
| Quartet PSG | 4 | 4 of 4 | uade |  |
| Riff Raff | 4 | 4 of 4 | uade |  |
| Ad Lib - DeFy AdLib Tracker | 3 | 3 of 3 | adplug |  |
| Ben Daglish SID | 3 | 3 of 3 | uade |  |
| DarkWave Studio | 3 | 3 of 3 | darkwave | DarkWave Studio's own engine, built from its GPL 3 source (4.1.6), compared with the program's own output in Wine. Three songs, the program's own sample projects: loops of 2 to 6 seconds, played with its Loop to half a minute. Chemistry 101 and 202 match the program sample by sample (correlation 0.998 and 0.997); in 303 the drums are random in the program too, and the bass follows it in pitch, tempo and brightness but not sample by sample. |
| Desire | 3 | 3 of 3 | uade |  |
| Dirk Bialluch | 3 | 3 of 3 | uade |  |
| EarAche | 3 | 3 of 3 | uade |  |
| Extreme Tracker | 3 | 3 of 3 | openmpt |  |
| Howie Davies | 3 | 3 of 3 | uade |  |
| InStereo! | 3 | 3 of 3 | uade |  |
| Leggless Music Editor | 3 | 3 of 3 | uade |  |
| Professional Sound Artists | 3 | 3 of 3 | uade |  |
| Real Tracker | 3 | 3 of 3 | openmpt |  |
| Rob Hubbard ST | 3 | 3 of 3 | uade |  |
| Sean Connolly | 3 | 3 of 3 | uade |  |
| Soundtracker Pro II | 3 | 3 of 3 | openmpt |  |
| Spectrum - VGM | 3 | 3 of 3 | vgm |  |
| Speedy A1 System | 3 | 3 of 3 | uade |  |
| Ad Lib - DOSBox | 2 | 2 of 2 | adplug |  |
| Ad Lib - MPU-401 Trakker | 2 | 2 of 2 | adplug |  |
| Ad Lib - SNG Player | 2 | 2 of 2 | adplug |  |
| Cubic Tiny XM | 2 | 2 of 2 | mxm | Both songs are quiet, which is in the files. |
| Delta Packer | 2 | 2 of 2 | uade |  |
| Digitrekker | 2 | 2 of 2 | digitrekker | DigiTrekker 3.10's own rendering (run in DOSBox): the same length, pitch, tempo and balance. Written from the format's document. DigiTrekker's own output is louder and clips; this one doesn't. Effects the two songs don't use are not checked. |
| FuchsTracker | 2 | 2 of 2 | xmp | Libxmp. |
| Future Composer BSI | 2 | 2 of 2 | uade |  |
| Magnetic Fields Packer | 2 | 2 of 2 | uade |  |
| Powertracker | 2 | 2 of 2 | uade |  |
| Thomas Hermann | 2 | 2 of 2 | uade |  |
| Tomy Tracker | 2 | 2 of 2 | uade |  |
| Unis 669 | 2 | 2 of 2 | openmpt |  |
| Actionamics | 1 | 1 of 1 | uade |  |
| Ad Lib - AdLib Tracker | 1 | 1 of 1 | adplug |  |
| AY STRC | 1 | 1 of 1 | strc | Written for this player in a clean room, from a description of the format only, with Sound Tracker's own note table; the same registers every tick and the same sound as before. Sound Tracker 1.1 songs compiled by Raxoft's compiler for DeliAY. Checked against ZXTune's playing of the same song as a Sound Tracker file: the same length and notes (spectral likeness 0.92). |
| BP SoundMon 1 | 1 | 1 of 1 | uade | SoundMon 1 is SoundMon 2's layout without synthesizer tables; played as that by UADE's SoundMon 2.0 player, under that label. Not compared with SoundMon 1 itself. |
| Composer 667 | 1 | 1 of 1 | openmpt |  |
| Digital Mugician 3 | 1 | 1 of 1 | uade | The song (Moon Child's title theme) is Digital Mugician 2's layout under another label; played by UADE's Mugician II player. Not yet compared with the recording of the game's demo on archive.org. |
| Megadrive CYM | 1 | 1 of 1 | cym | CYM is a log of a Capcom arcade board's YM2151 (from MAME or Kawaks), not Mega Drive music; made into VGM and played by bbmp-vgm. |
| Startrekker FLT8 | 1 | 1 of 1 | openmpt |  |
| Video Game Music - Sharp X68000 | 1 | 1 of 1 | vgm |  |

## Plays with faults or gaps

Something is known to be wrong or missing, or some of the songs don't play.

| Format | Songs | Play | Replayer | Notes |
|---|---|---|---|---|
| MDX | 7,137 | 39 of 40 tried | mdx | Metal Sight's 17 songs are encrypted (their data begins "crypt") and come out silent. 1 of 40 tried is silent. |
| Ultra64 Sound Format | 6,325 | 39 of 40 tried | zxtune | The World Is Not Enough (18 songs) is silent: ZXTune's lazyusf2 gives no sound, with its high-level audio and without (tried 2 October 2026). 1 of 40 tried is silent. |
| Saturn Sound Format | 4,387 | 99 of 100 tried | zxtune | One sound effect of Irem Arcade Classics' rip (tyfsnd-00-27, a second long) is silent. 1 of 100 tried is silent. |
| Sam Coupe COP | 784 | 38 of 40 tried | zxtune | Frantisek Fuka's manic.cop and prince.cop are Z80 player code, not tracker songs: they need an emulated SAM Coupé, which the player doesn't have. 2 of 40 tried can't be played (no replayer could play this file (zxtune: no such track; openmpt: libopenmpt can…t recognise this file; gme: W). |
| Psycle | 535 | 389 of 535 | psycle | Psycle's own player code and 64 of its plugins; song lengths compared with another player. 146 songs are refused: they need VST plugins, or plugins whose source was never published. 119 of those that play miss an effect or a minor instrument and say so. The sound was never compared with Psycle itself; 13 songs clip. |
| Delitracker Custom | 477 | 38 of 40 tried | uade | Marble Madness (6 files) and Archon I and II crash inside UADE. 2 of 40 tried can't be played (no replayer could play this file (uade: UADE doesn…t open Marble Madness - Ultimate.cus; gme: Wrong file type ; no replayer could play this file (uade: UADE doesn…t open Archon.cus; gme: Wrong file type for this emulator; ). |
| HES | 421 | 39 of 40 tried | gme | Bloody Wolf makes no music in any track (Game Music Emu and ZXTune alike). A rip has 256 tracks and its music often starts late (track 43, 128); the player passes the silent ones over in a moment. A rip that names its first track starts there. Game Music Emu is built with a fix of ours: without it, Oboccha Makun's track 199 crashed it. 1 of 40 tried is silent. |
| KSS | 393 | 39 of 40 tried | kss | 1 of 40 tried is noise. |
| Sonic Arranger | 251 | 37 of 40 tried | uade | UADE refuses one song (amber moon26) and plays two silent (laserdance-eddiesreturn, sonic-intro-1). 1 of 40 tried can't be played (no replayer could play this file (uade: UADE doesn…t open amber moonn.sa; gme: Wrong file type for this emulat). 2 of 40 tried are silent. |
| SunVox | 246 | 59 of 60 tried | sunvox | One song (sunvox digital) sounds harsh throughout and is counted as noise; whether it is meant to is not known. 1 of 60 tried is noise. |
| SPU | 164 | 163 of 164 | spu | Against a second emulator in 10 songs. A note that was sounding when the rip was made is missing until it is struck again, and reverb differs between the two emulators (neither is known to be right). One file (Raiden DX 01) is refused: an entry of its log makes no sense. |
| MoonBlaster (edit mode) | 129 | 38 of 40 tried | kss | Two songs (Betaroid, and Ace In Space's level 1 big boss) stay silent in libkss, with their sample kits too. 2 of 40 tried can't be played (opens and gives no sound). |
| Ad Lib - Reality AdLib Tracker | 127 | 39 of 40 tried | adplug | One song (canon in d) is for MIDI instruments only, and the player has no General MIDI synthesizer. 1 of 40 tried is silent. |
| Mad Tracker 2 | 106 | 39 of 40 tried | openmpt | One song (accidental ray of sun, piano mix) needs the commercial VST plug-in The Grand. 1 of 40 tried is silent. |
| Archimedes Tracker | 99 | 97 of 99 | xmp | Libxmp. Two songs are silent. |
| Hippel COSO | 92 | 39 of 40 tried | uade | One song (cryptoburners - pack5) is silent; not looked into. 1 of 40 tried is silent. |
| OpenMPT MPTM | 68 | 39 of 40 tried | openmpt | One song (synthetic journey) needs the Synth1 VST plug-in. 1 of 40 tried is silent. |
| Spectrum - Snapshot | 65 | 18 of 65 | zxtune | Tapes, disks, and music that is a program of its own need an emulated Spectrum, which the player doesn't have; a snapshot is a whole Spectrum's memory, tape or disk. ZXTune finds the tracker song in 18 of the 31 snapshots (.sna); tapes, disks, and music that is a program of its own need an emulated Spectrum, which the player doesn't have. |
| Hippel | 63 | 38 of 40 tried | uade | Two songs (prince, astaroth) have a version of the replay code UADE's player refuses; forced, it plays garbage. 2 of 40 tried can't be played (no replayer could play this file (uade: UADE doesn…t open prince.hip; gme: Wrong file type for this emulator; ; no replayer could play this file (uade: UADE doesn…t open astaroth.hip; gme: Wrong file type for this emulator). |
| FredMon | 59 | 36 of 40 tried | uade | Four files (voodoo, voodoo-2, both fuzzball) have a version of the replay code UADE doesn't know. 4 of 40 tried can't be played (no replayer could play this file (uade: UADE doesn…t open voodoo-n.fred; gme: Wrong file type for this emulato; no replayer could play this file (uade: UADE doesn…t open fuzzball-title.fred; gme: Wrong file type for this e). |
| Buzz | 51 | 10 of 51 | buzz | BmxPlay and its five machines. 41 songs need Buzz machines that exist only as Windows plug-ins. |
| MPK | 51 | 38 of 40 tried | kss | Two songs of Fighters Ragnarok (Prologue, Can you beat me) are counted as noise; they are drum-heavy and probably right, but nobody has listened. 2 of 40 tried are noise. |
| Spectrum | 42 | 3 of 3 tried |  | The 22 E-Tracker songs ZX-Art files here (.etc, ESI's tracker, 1992) play in no replayer the player has. Modland's three (Pro Tracker 2, ASC Sound Master) play. ZX-Art files 113 tunes here whose kind it doesn't name; of 5 tried (2 October 2026) the 3 Fuxoft songs (.fxm, 30 of the 113) play by their own replayer once a catalog update files them by their extension, as it now does with .fxm, .fls and .vtx. The 22 E-Tracker songs (.etc, ESI's tracker, 1992) play in no replayer the player has (ZXTune doesn't know them); 4 Beepola songs (.bbsong, the editor's own files) and a few others are not looked into. |
| Ad Lib - Martin Fernandez | 39 | 38 of 39 | adplug | One file (Viking Child's loader) opens as 0 seconds long and gives no sound in AdPlug; why is not found. 1 of 39 tried can't be played (opens and gives no sound). |
| PiECE music driver | 36 | 36 of 36 | piece | A replayer written anew from the P/ECE SDK's format documents and recordings of the original driver. The drums are made anew: as long and as loud as the driver's at their start, but brighter and decaying differently. Compared with Aquaplus' driver running in the piemu emulator: Final Fantasy's Prelude within 0.1 dB and alike in sound, SAMPLE BGM1 within 1.7 dB; volumes above 127 and v+/v- now as the driver plays them (Tora-H's BGM02 Yuko from 16 dB off to 2 dB). |
| Mark II | 35 | 34 of 35 | uade | One song (my way to the stars) has a version of the replay code that UADE plays silent. 1 of 35 tried is silent. |
| Beathoven Synthesizer | 32 | 31 of 32 | uade | One song (katakis loading v1, an early version 1.00) is silent in UADE. 1 of 32 tried is silent. |
| Jason Page | 32 | 31 of 32 | uade | One file (jpn.road) has another layout (it begins "ROAD"), which no UADE player opens. 1 of 32 tried can't be played (no replayer could play this file (uade: UADE doesn…t open jpn.road; gme: Wrong file type for this emulator; si). |
| Skale Tracker | 30 | 30 of 30 | skale | The effects' mixing checked against Skale's own player (SkalePlayer.dll), level by level. A chorus (3 songs) and a multi-tap delay (1 song) are missing, as in Skale without those closed plug-ins. Its other effects (delay, reverb, stereo) play, through rewritten plug-ins, mixed as Skale mixes them; Freeverb2 is played by Freeverb, its settings guessed. |
| Hippel ST | 21 | 13 of 21 | uade | UADE's Jochen Hippel ST player. 8 of the 14 .sog files (song data without its replay code) come out as the same quiet opening, not their song: UADE's player misreads them. The 7 .hst songs and 6 .sog songs play. |
| Sam Coupe SNG | 15 | 7 of 15 | zxtune | 8 of the 15 (Fuka's, Ziutek's, tetris) are Z80 player code, not tracker songs: they need an emulated SAM Coupé, which the player doesn't have. 8 of 15 tried can't be played (no replayer could play this file (zxtune: no such track; openmpt: libopenmpt can…t recognise this file; gme: W). |
| GoatTracker 2 | 14 | 14 of 14 | goattracker | As GoatTracker. Details of the play routines before 2.2 (pulse speed, filter resonance) may differ: their source isn't to be had, and 13 of the 14 songs are in the early format of version 2 (its wavetable values 08-0F are waveforms; delays from 2.18 on), read so now. |
| Silmarils | 14 | 11 of 14 | uade | The 3 Boston Bomb Club files are song data without their samples, which the game keeps in bostonbombclub.mok; UADE can't put the two together. 3 of 14 tried can't be played (no replayer could play this file (uade: UADE doesn…t open bostonbombclub ingame n.mok; gme: Wrong file type fo, 2 times; no replayer could play this file (uade: UADE doesn…t open bostonbombclub title.mok; gme: Wrong file type for t). |
| SBStudio | 6 | 5 of 6 | sbstudio | Plays without the AWE32's effects. One file is for MIDI instruments and has no sound of its own. |
| Forgotten Worlds | 5 | 1 of 5 | uade | 4 of the 5 are silent: whether UADE's player sounds depends on the file's path and its neighbours, a fault of UADE. 4 of 5 tried are silent. |
| NoiseTrekker | 3 | 3 of 3 | protrekkr | One song still clips, 0.13% of the time, though the songs are played 14 dB down, with NoiseTrekker 1's synths at half volume. How loud NoiseTrekker itself played is unknown: its source is lost. |
| MusicMaker V8 Old | 2 | 1 of 2 | uade | "best of guitars" is a 6-channel song UADE's player won't take; moveback plays with its instruments (.ip). 1 of 2 tried can't be played (no replayer could play this file (uade: UADE doesn…t open best of guitars.sdata; gme: Wrong file type for this). |

## Doesn't play

| Format | Songs | Why |
|---|---|---|
| Renoise | 151 | No replayer here plays Renoise songs: Renoise's sampler and effects would have to be written, which would serve Renoise Old too. Until 2 October 2026 ZXTune seemed to play them, but what it played were the instruments' samples inside the song's zip, one after the other. |
| Renoise Old | 120 | Renoise's effect devices were never published. Of 60 songs looked at, 53 use them and 6 use VST plugins; 1 uses samples only. Possible: old Renoise demos, one for Linux (2.7.2), can still be downloaded to record references; a replayer of the sampler and about ten effects would serve both Renoise formats. |
| Ace Tracker | 57 | For the Atari Falcon's signal processor. Possible: its makers (New Beat) publish the replay routine Replace (68030 and DSP, no source) and the tracker; it would play in an emulated Falcon (Hatari). Replace knows two of the three kinds of song (41 of 57). |
| Picatune2 | 39 | Another, far bigger synthesizer than Picatune's, without published source. Possible: the program (with WAV export) is at its author's site, and much of the synthesizer is in his BR808 (LGPL-3). |
| DreamStation | 28 | Its synthesizer was never published. The freeware program (with WAV export) and a plug-in of it are on archive.org, for a replayer written from what they do. |
| Synder Tracker | 24 | The tracker for the same Windows synthesizer, with its own synthesizer; no source. The program is on Modland. |
| All Sound Tracker | 20 | Drives the AWE32's sampler chip, FM and MIDI together; its source is lost. Possible: its author's description of the format and recordings of songs are on archive.org; songs that use only the FM chip would be quick. |
| Aero Studio | 16 | Its synthesizer (about eight machines) was never published. The program's demo is on Modland; its maker, ioneo AB in Linköping, still exists. |
| Ad Lib - Drum Traker | 15 | Drum Blaster's Drum Traker (Larry Tipton, Epic MegaGames). All 15 songs, and the samples they name, are in Drum Blaster 3.0 shareware; a replayer of its sequencer is to be written. |
| MVX Module | 12 | Only compiled libraries of its player exist (apEx, CC BY-NC 4.0), with all their names in them. Possible: linked into a helper of its own, or a replayer written from them. |
| RamTracker | 9 | A MIDI tracker (General MIDI on an AWE32, GUS or MPU-401). All 9 songs and the program are on archive.org; it needs a General MIDI synthesizer, which the player doesn't have yet. |
| Tunefish | 8 | Tunefish 2; its makers published versions 3 and 4, whose files are different. The Tunefish 2 plug-in, which writes these files, can still be downloaded. |
| GT Game Systems | 7 | The Amiga game Carver (Art Edition, 1991). Its replay routine would have to be taken from the game. |
| MegaStation MIDI | 5 | The MIDI songs of MegaStation (source on Modland); they need a General MIDI synthesizer, which the player doesn't have yet. |
| Beaver Sweeper | 4 | A Windows synthesizer tracker (2005). Its whole source is on Modland, for non-commercial use. |
| Compoz | 4 | DOS, 1991: text that the program turns into CMF, with 16 instruments inside the program, which is on Modland. |
| Athtune | 3 | Tunes with synthesizers written in C#, compiled by the program (.NET). The three songs are its examples and small enough to rewrite by hand. |
| SVAr Tracker | 3 | A host for Windows plug-ins, without source. Its songs are zip files locked with a password; songs that use other people's plug-ins can't be played. |
| MegaStation | 2 | A Windows sample tracker (DJ Hikari, 2000). Its Delphi source, without a licence, is on Modland, for a replayer written from it. |
| MVS Tracker | 2 | Mega Drive and Neo Geo FM music (2006-07). The source of the tracker and of its Z80 driver is published, without a licence. |
| AND XSynth | 1 | One song, of a 64k intro. The synthesizer's Buzz machines are closed and the file format undescribed. |
| Cybertracker | 1 | One song: the demo song of CyberTracker 0.67 for the Amiga (Andreas Öman, 1995), not the C64 tracker. Its layout is known (IFF: the song and 22 samples, 10 channels); how it plays, nothing describes. The program is found, to record a reference in an emulated Amiga. |
