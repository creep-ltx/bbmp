# SPDX-License-Identifier: GPL-3.0-or-later
"""What platform and chips each format is. See platforms.py for the ids.
Made 2026-09-29 from the project's own notes (docs/replayers.md, docs/format-notes.toml,
the helpers' sources in csrc/), Modland's documents (old_updates.txt, format_documentation/),
the Wanted Team's readmes in UADE's sources, OpenMPT's loaders, the headers of Modland's
files and, for a few, the web; the comment on each line says how sure it is.

A format's name that isn't here is looked up by the part before " - " ("Ad Lib - X" as
"Ad Lib", "HVSC - Musicians" as "HVSC"). Some sounds have no id of their own and are
given none: samples played by the processor (PC trackers, the Atari STE's and Falcon's
DMA sound), the PC speaker, the AWE32's EMU8000, the 32X's PWM, the Neo Geo (as Arcade).
"""

# format -> (platforms, chips); the first platform is the one the format is mostly from
FORMATS: dict[str, tuple[tuple[str, ...], tuple[str, ...]]] = {
    "Ace Tracker": (("atarist",), ()),  # checked: docs/replayers.md, for the Falcon's DSP
    "Actionamics": (("amiga",), ("paula",)),  # checked: UADE player (Amiga)
    "Activision Pro": (("amiga",), ("paula",)),  # sure
    "Ad Lib": (("pc",), ("opl",)),  # sure (family fallback)
    "Ad Lib - ADL": (("pc",), ("opl",)),  # sure
    "Ad Lib - AdLib Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - AdLib Tracker 2": (("pc",), ("opl",)),  # sure
    "Ad Lib - AMusic": (("pc",), ("opl",)),  # sure
    "Ad Lib - AMusic XMS": (("pc",), ("opl",)),  # sure
    "Ad Lib - Apogee": (("pc",), ("opl",)),  # sure
    "Ad Lib - Beni Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - Bob's AdLib Music": (("pc",), ("opl",)),  # sure
    "Ad Lib - Boom Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - Creative Music File": (("pc",), ("opl",)),  # sure
    "Ad Lib - DeFy AdLib Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - Digital FM": (("pc",), ("opl",)),  # sure
    "Ad Lib - DOSBox": (("pc",), ("opl",)),  # sure: DOSBox's OPL captures
    "Ad Lib - Drum Traker": (("pc",), ("opl",)),  # sure
    "Ad Lib - EdLib D00": (("pc",), ("opl",)),  # sure
    "Ad Lib - EdLib D01": (("pc",), ("opl",)),  # sure
    "Ad Lib - EdLib Packed": (("pc",), ("opl",)),  # sure
    "Ad Lib - Exotic AdLib": (("pc",), ("opl",)),  # sure
    "Ad Lib - Extra Simple Music": (("pc",), ("opl",)),  # sure
    "Ad Lib - Faust Music Creator": (("pc",), ("opl",)),  # sure
    "Ad Lib - FM Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - FM-Kingtracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - Herad Music System": (("pc",), ("opl",)),  # sure
    "Ad Lib - HSC AdLib Composer": (("pc",), ("opl",)),  # sure
    "Ad Lib - Johannes Bjerregard Module": (("pc",), ("opl",)),  # sure
    "Ad Lib - Ken's AdLib Music": (("pc",), ("opl",)),  # sure
    "Ad Lib - Loudness Sound System": (("pc",), ("opl",)),  # sure
    "Ad Lib - LucasArts": (("pc",), ("opl",)),  # sure
    "Ad Lib - Martin Fernandez": (("pc",), ("opl",)),  # sure
    "Ad Lib - Master Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - MK-Jamz": (("pc",), ("opl",)),  # sure
    "Ad Lib - Mlat Adlib Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - MPU-401 Trakker": (("pc",), ("opl",)),  # sure
    "Ad Lib - MUS": (("pc",), ("opl",)),  # sure
    "Ad Lib - Palladix": (("pc",), ("opl",)),  # sure
    "Ad Lib - Pixel Painters FMF": (("pc",), ("opl",)),  # sure
    "Ad Lib - Pixel Painters FTF": (("pc",), ("opl",)),  # sure
    "Ad Lib - Raw OPL Capture": (("pc",), ("opl",)),  # sure
    "Ad Lib - Reality AdLib Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - Screamtracker 3 AdLib": (("pc",), ("opl",)),  # sure
    "Ad Lib - Shadowlands": (("pc",), ("opl",)),  # sure
    "Ad Lib - Sierra": (("pc",), ("opl",)),  # sure
    "Ad Lib - SNG Player": (("pc",), ("opl",)),  # sure
    "Ad Lib - Sound Images Generation 2": (("pc",), ("opl",)),  # sure
    "Ad Lib - Sound Interface System": (("pc",), ("opl",)),  # sure
    "Ad Lib - SoundFX Macs Opera": (("pc",), ("opl",)),  # sure
    "Ad Lib - Surprise! AdLib Tracker": (("pc",), ("opl",)),  # sure
    "Ad Lib - Surprise! AdLib Tracker 2.0": (("pc",), ("opl",)),  # sure
    "Ad Lib - Twin TrackPlayer": (("pc",), ("opl",)),  # sure
    "Ad Lib - Ultima 6": (("pc",), ("opl",)),  # sure
    "Ad Lib - Vibrants": (("pc",), ("opl",)),  # sure
    "Ad Lib - Visual Composer": (("pc",), ("opl",)),  # sure
    "Aero Studio": (("pc",), ()),  # checked: Modland, a Windows tracker by IoNeo
    "AHX": (("amiga",), ("paula",)),  # sure
    "All Sound Tracker": (("pc",), ()),  # checked: Modland, DOS, for the AWE32 (EMU8000, no id)
    "AM Composer": (("amiga",), ("paula",)),  # checked: UADE player, Marc Hawlitzeck 1989
    "AND XSynth": (("pc",), ()),  # checked: Modland, a softsynth by AND (and.intercon.ru)
    "Anders Oland": (("amiga",), ("paula",)),  # sure
    "AProSys": (("amiga",), ("paula",)),  # sure
    "Archimedes Tracker": (("archimedes",), ("vidc",)),  # sure
    "ArkosTracker": (("cpc",), ("ay",)),  # checked: Targhan's tracker, from Starkos on the CPC (docs/replayers.md)
    "Arpeggiator": (("amiga",), ("paula",)),  # checked: UADE's Major Tom player (Amiga)
    "Art And Magic": (("amiga",), ("paula",)),  # sure
    "Art Of Noise": (("amiga",), ("paula",)),  # sure
    "Ashley Hogg": (("amiga",), ("paula",)),  # sure
    "Astroidea XMF": (("pc",), ()),  # checked: OpenMPT, DOS game Imperium Galactica and Astroidea demos
    "Asylum": (("pc",), ()),  # sure: DOS games (Crusader)
    "Atari Digi-Mix": (("atarist",), ("ay",)),  # sure
    "Athtune": (("pc",), ()),  # checked: Modland, a Windows console program by Athaudia
    "Audio Sculpture": (("atarist",), ()),  # checked: an ST/STE tracker; samples, no chip id
    "AXS": (("pc",), ()),  # checked: Modland, a Windows tracker
    "AY Amadeus": (("zx",), ("ay",)),  # checked: docs/format-notes.toml, Fuxoft's engine
    "AY Emul": (("zx", "cpc"), ("ay",)),  # checked: catalog, 210 of 1202 are .cpc.emul
    "AY STRC": (("zx",), ("ay",)),  # guess: one song, AY folder, Spectrum naming
    "Beathoven Synthesizer": (("amiga",), ("paula",)),  # sure
    "Beaver Sweeper": (("pc",), ()),  # checked: Modland, a Windows softsynth tracker
    "Beepola": (("zx",), ("beeper",)),  # sure
    "Ben Daglish": (("amiga",), ("paula",)),  # sure
    "Ben Daglish SID": (("amiga",), ("paula",)),  # checked: Wanted Team readme, Amiga version of his C64 routine (Terramex)
    "BeRoTracker": (("pc",), ()),  # sure
    "BoyScout": (("gba",), ("gbapu",)),  # checked: Modland, Windows tracker for the GBA's sound chip
    "BP SoundMon 1": (("amiga",), ("paula",)),  # sure
    "BP SoundMon 2": (("amiga",), ("paula",)),  # sure
    "BP SoundMon 3": (("amiga",), ("paula",)),  # sure
    "Buzz": (("pc",), ()),  # sure
    "Buzzic": (("pc",), ()),  # checked: Modland, Windows 4k synth
    "Buzzic 2": (("pc",), ()),  # checked: Modland, Windows 4k synth
    "Capcom Q-Sound Format": (("arcade",), ("qsound",)),  # sure
    "CBA": (("pc",), ()),  # checked: OpenMPT, the DOS demo Expoze by Chuck Biscuits and Black Artist
    "Chiptracker": (("amiga",), ("paula",)),  # checked: Wanted Team readme, Kris/Anarchy
    "Coconizer": (("archimedes",), ("vidc",)),  # sure
    "Composer 667": (("pc",), ("opl",)),  # checked: OpenMPT loader, an OPL2 format
    "Composer 669": (("pc",), ()),  # sure
    "Composer 670 (CDFM)": (("pc",), ("opl",)),  # checked: OpenMPT loader, samples and OPL2
    "Compoz": (("pc",), ("opl",)),  # checked: Modland, a Sound Blaster tracker for DOS, 1991
    "Core Design": (("amiga",), ("paula",)),  # sure
    "Cubic Tiny XM": (("pc",), ()),  # sure
    "CustomMade": (("amiga",), ("paula",)),  # sure
    "Cybertracker": (("amiga",), ("paula",)),  # guess: one song in IFF chunks; Modland knows nothing of it
    "Cybertracker C64": (("c64",), ("sid",)),  # sure
    "Darius Zendeh": (("amiga",), ("paula",)),  # sure
    "DarkWave Studio": (("pc",), ()),  # sure: Windows
    "Dave Lowe": (("amiga",), ("paula",)),  # sure
    "Dave Lowe New": (("amiga",), ("paula",)),  # sure
    "David Hanney": (("amiga",), ("paula",)),  # sure
    "David Whittaker": (("amiga",), ("paula",)),  # sure
    # system bytes of Modland's 1813 files: Mega Drive 788, Game Boy 332, PC Engine 283,
    # Arcade (YM2151 + SegaPCM) 156, NES 85, Master System 76, C64 82
    "Deflemask": (("megadrive", "gb", "pcengine", "arcade", "nes", "mastersystem", "c64"),
                  ("ym2612", "sn76489", "gbapu", "huc6280", "ym2151", "segapcm", "nesapu", "sid", "opll")),  # checked: every file's system byte
    "Delitracker Custom": (("amiga",), ("paula",)),  # sure
    "Delta Music": (("amiga",), ("paula",)),  # sure
    "Delta Music 2": (("amiga",), ("paula",)),  # sure
    "Delta Packer": (("amiga",), ("paula",)),  # sure
    "Desire": (("amiga",), ("paula",)),  # checked: Wanted Team readme, Amiga games by Dentons
    "Digibooster": (("amiga",), ("paula",)),  # sure
    "Digibooster Pro": (("amiga",), ("paula",)),  # sure
    "Digital Mugician": (("amiga",), ("paula",)),  # sure
    "Digital Mugician 2": (("amiga",), ("paula",)),  # sure
    "Digital Mugician 3": (("amiga",), ("paula",)),  # sure
    "Digital Sonix And Chrome": (("amiga",), ("paula",)),  # sure
    "Digital Sound And Music Interface": (("pc",), ()),  # checked: Modland, DOS, Otto Chrons
    "Digital Sound Interface Kit": (("pc",), ()),  # sure
    "Digital Sound Interface Kit RIFF": (("pc",), ()),  # sure
    "Digital Sound Studio": (("amiga",), ("paula",)),  # sure
    "Digital Symphony": (("archimedes",), ("vidc",)),  # sure
    "Digital Tracker DTM": (("atarist",), ()),  # checked: Modland, an Atari Falcon tracker
    "Digital Tracker MOD": (("atarist",), ()),  # checked: Modland, an Atari Falcon tracker
    "Digitrakker": (("pc",), ()),  # sure
    "Digitrekker": (("pc",), ()),  # checked: its format document (DigiTrekker 3.00, Sound Blaster/GUS); Modland once said Atari
    "Dirk Bialluch": (("amiga",), ("paula",)),  # sure
    "Disorder Tracker 2": (("pc",), ()),  # sure
    "Dreamcast Sound Format": (("dreamcast",), ("aica",)),  # sure
    "DreamStation": (("pc",), ()),  # checked: Modland, a Windows synth tracker
    "DSMI Compact": (("pc",), ()),  # sure
    "Dynamic Studio Professional": (("pc",), ()),  # checked: files begin "DSm\x1a" (DOS), little-endian, OpenMPT; Modland says Amiga
    "Dynamic Synthesizer": (("amiga",), ("paula",)),  # sure
    "EarAche": (("amiga",), ("paula",)),  # sure
    "EasyTrax": (("pc",), ()),  # guess: little-endian file with DOS 8.3 sample names; nothing found on the web
    "Electronic Music System": (("amiga",), ("paula",)),  # sure
    "Electronic Music System v6": (("amiga",), ("paula",)),  # sure
    "Epic Megagames MASI": (("pc",), ()),  # sure
    "Euphony": (("fmtowns",), ("ym2612", "rf5c68")),  # sure
    "Extreme Tracker": (("pc",), ()),  # sure
    "FAC SoundTracker": (("msx",), ("opll", "opl")),  # checked: docs/replayers.md, MSX-MUSIC and MSX-AUDIO
    "Face The Music": (("amiga",), ("paula",)),  # sure
    "FamiTracker": (("nes",), ("nesapu", "nesexp")),  # sure
    "Farandole Composer": (("pc",), ()),  # sure
    "Fashion Tracker": (("amiga",), ("paula",)),  # checked: UADE player, Fashion 1988
    "Fasttracker": (("pc",), ()),  # sure
    "Fasttracker 2": (("pc",), ()),  # sure
    "FM sound driver (FMP)": (("pc98",), ("opn", "opna")),  # sure
    "FM Tracker": (("pc",), ("opl",)),  # checked: OpenMPT loader, Davey W Taylor's FM Tracker
    "Follin Player II": (("amiga",), ("paula",)),  # sure
    "Forgotten Worlds": (("amiga",), ("paula",)),  # sure
    "Fred Gray": (("amiga",), ("paula",)),  # sure
    "FredMon": (("amiga",), ("paula",)),  # sure
    "FuchsTracker": (("amiga",), ("paula",)),  # sure
    "Funktracker": (("pc",), ()),  # sure
    "Future Composer 1.3": (("amiga",), ("paula",)),  # sure
    "Future Composer 1.4": (("amiga",), ("paula",)),  # sure
    "Future Composer BSI": (("amiga",), ("paula",)),  # sure
    "Future Player": (("amiga",), ("paula",)),  # sure
    "Game Music Creator": (("amiga",), ("paula",)),  # sure
    "Gameboy Sound Format": (("gba",), ("gbadirect", "gbapu")),  # checked: the files are .minigsf, Game Boy Advance rips
    "Gameboy Sound System": (("gb",), ("gbapu",)),  # sure
    "Gameboy Sound System GBR": (("gb",), ("gbapu",)),  # sure
    "Gameboy Tracker": (("gb",), ("gbapu",)),  # checked: csrc/retro/gbt_engine.cpp
    "General DigiMusic": (("pc",), ()),  # sure
    "GlueMon": (("amiga",), ("paula",)),  # sure
    "GoatTracker": (("c64",), ("sid",)),  # sure
    "GoatTracker 2": (("c64",), ("sid",)),  # sure
    "Graoumf Tracker": (("atarist",), ()),  # checked: Modland and OpenMPT, Atari Falcon
    "Graoumf Tracker 2": (("atarist", "pc"), ()),  # checked: 59 of 61 files are of the Falcon versions (OpenMPT: version <= 5)
    "GT Game Systems": (("amiga",), ("paula",)),  # checked: Modland, only the Amiga game Carver
    "HES": (("pcengine",), ("huc6280",)),  # sure
    "Hippel": (("amiga",), ("paula",)),  # sure
    "Hippel 7V": (("amiga",), ("paula",)),  # sure
    "Hippel COSO": (("amiga",), ("paula",)),  # sure
    "Hippel ST": (("atarist",), ("ay",)),  # sure
    "Hippel ST COSO": (("atarist",), ("ay",)),  # sure
    "His Master's Noise": (("amiga",), ("paula",)),  # sure
    "HivelyTracker": (("amiga", "pc"), ("paula",)),  # checked: made for Windows, AmigaOS 3.1/4 and AROS; AHX's sound
    "Howie Davies": (("amiga",), ("paula",)),  # sure
    "HVSC": (("c64",), ("sid",)),  # sure (also the fallback for "HVSC - ...")
    "IFF-SMUS": (("amiga",), ("paula",)),  # sure
    "Images Music System": (("amiga",), ("paula",)),  # sure
    "Imago Orpheus": (("pc",), ()),  # sure
    "Impulsetracker": (("pc",), ()),  # sure
    "Infogrames": (("amiga",), ("paula",)),  # checked: UADE player (Amiga)
    "InStereo!": (("amiga",), ("paula",)),  # sure
    "InStereo! 2.0": (("amiga",), ("paula",)),  # sure
    "Ixalance": (("pc",), ()),  # checked: a Win32 player for IT with procedural samples
    "JamCracker": (("amiga",), ("paula",)),  # sure
    "Janko Mrsic-Flogel": (("amiga",), ("paula",)),  # sure
    "Jason Brooke": (("amiga",), ("paula",)),  # sure
    "Jason Page": (("amiga",), ("paula",)),  # sure
    "Jason Page Old": (("amiga",), ("paula",)),  # sure
    "JayTrax": (("pc",), ()),  # checked: a synth tracker for Windows and PocketPC
    "Jeroen Tel": (("amiga",), ("paula",)),  # sure
    "Jesper Olsen": (("amiga",), ("paula",)),  # sure
    "Ken's Digital Music": (("pc",), ()),  # sure
    "Klystrack": (("pc",), ()),  # sure
    "Kris Hatlelid": (("amiga",), ("paula",)),  # sure
    "KSS": (("msx", "mastersystem", "gamegear", "arcade"),
            ("ay", "scc", "opll", "sn76489", "opl")),  # checked: catalog, 19 arcade rips; opl for MSX-AUDIO and YM3526
    "Leggless Music Editor": (("amiga",), ("paula",)),  # sure
    "Lionheart": (("amiga",), ("paula",)),  # sure
    "Liquid Tracker": (("pc",), ()),  # checked: Modland, a DOS tracker
    "Mad Tracker 2": (("pc",), ()),  # sure
    "Magnetic Fields Packer": (("amiga",), ("paula",)),  # sure
    "Maniacs Of Noise": (("amiga",), ("paula",)),  # sure
    "Mark Cooksey": (("amiga",), ("paula",)),  # sure
    "Mark Cooksey Old": (("amiga",), ("paula",)),  # sure
    "Mark II": (("amiga",), ("paula",)),  # sure
    "Maximum Effect": (("amiga",), ("paula",)),  # sure
    "MaxTrax": (("amiga",), ("paula",)),  # sure
    "MCMD": (("amiga",), ("paula",)),  # checked: UADE player; the songs are Amiga crack intros
    "MDX": (("x68000",), ("ym2151", "okim")),  # sure
    "Medley": (("amiga",), ("paula",)),  # sure
    "Megadrive CYM": (("arcade",), ("ym2151",)),  # checked: CYM is a log of a Capcom arcade board's YM2151 (MAME, Kawaks), not the Mega Drive's
    "Megadrive GYM": (("megadrive",), ("ym2612", "sn76489")),  # sure
    "MegaStation": (("pc",), ()),  # guess: zlib-packed song of a modern program, came with other Windows formats
    "MegaStation MIDI": (("pc",), ()),  # guess: as MegaStation
    "Megatracker": (("atarist",), ()),  # checked: Modland, a sample tracker for the Atari Falcon
    "MGSDRV": (("msx",), ("ay", "scc", "opll")),  # sure
    "MIDI-Loriciel": (("amiga", "atarist"), ("paula",)),  # checked: Wanted Team readme, later Amiga and Atari ST games
    "Mike Davies": (("amiga",), ("paula",)),  # sure
    "MikMod UNITRK": (("pc",), ()),  # sure
    "MO3": (("pc",), ()),  # sure
    "Monotone": (("pc",), ("pcspeaker",)),  # sure: a PC speaker tracker
    "MoonBlaster": (("msx",), ("opll", "opl")),  # checked: all files are .MBM/.MBK (MoonBlaster 1.4: MSX-MUSIC, MSX-AUDIO), not OPL4's .MWM
    "MoonBlaster (edit mode)": (("msx",), ("opll", "opl")),  # checked: as MoonBlaster
    "MPK": (("msx",), ("ay", "scc")),  # checked: Modland (K-KAZ's MSX driver), libkss sets the SCC slot
    "MultiMedia Sound": (("amiga",), ("paula",)),  # checked: Wanted Team readme, the SoundFX team's Amiga editor
    "Multitracker": (("pc",), ()),  # sure
    "Music Assembler": (("amiga",), ("paula",)),  # sure
    "Music Editor": (("amiga",), ("paula",)),  # checked: files begin "MED", Teijo Kinnunen's MED 2/3
    "MuSICA": (("msx",), ("ay", "scc", "opll")),  # checked: Modland, ASCII's MSX editor
    "Musicline Editor": (("amiga",), ("paula",)),  # sure
    "MusicMaker V8": (("amiga",), ("paula",)),  # sure
    "MusicMaker V8 Old": (("amiga",), ("paula",)),  # sure
    "MVS Tracker": (("neogeo", "arcade"), ("opnb",)),  # checked: MVSTracker makes Neo Geo FM music
    "MVX Module": (("pc",), ()),  # checked: Modland, Buzz plugins for Gargaj's demos
    "NerdTracker 2": (("nes",), ("nesapu",)),  # sure
    "Nintendo DS Sound Format": (("nds",), ("ndssound",)),  # sure
    "Nintendo Sound Format": (("nes",), ("nesapu", "nesexp")),  # sure
    "Nintendo SPC": (("snes",), ("spc700",)),  # sure
    "Noise Tracker GS 1": (("iigs",), ("es5503",)),  # sure
    "NoiseTrekker": (("pc",), ()),  # sure
    "NoiseTrekker 2": (("pc",), ()),  # sure
    "NovoTrade Packer": (("amiga",), ("paula",)),  # sure
    "Octalyser": (("atarist",), ()),  # checked: Modland, an STE/Falcon tracker
    "OctaMED MMD0": (("amiga",), ("paula",)),  # sure
    "OctaMED MMD1": (("amiga",), ("paula",)),  # sure
    "OctaMED MMD2": (("amiga",), ("paula",)),  # sure
    "OctaMED MMD3": (("amiga",), ("paula",)),  # sure
    "OctaMED MMDC": (("amiga",), ("paula",)),  # sure
    "Oktalyzer": (("amiga",), ("paula",)),  # sure
    "Onyx Music File": (("pc",), ()),  # checked: csrc/retro/jangle_engine.cpp, the MS-DOS music disk Jangle
    "OpenMPT MPTM": (("pc",), ()),  # sure
    "OPLLDRV": (("msx",), ("opll",)),  # checked: Modland, for the FM-PAC's YM2413
    "Organya": (("pc",), ()),  # sure
    "Organya 2": (("pc",), ()),  # sure
    "Paul Robotham": (("amiga",), ("paula",)),  # sure
    "Paul Shields": (("amiga",), ("paula",)),  # sure
    "Paul Summers": (("amiga",), ("paula",)),  # sure
    "Peter Verswyvelen": (("amiga",), ("paula",)),  # sure
    "Picatune": (("pc",), ()),  # sure
    "Picatune2": (("pc",), ()),  # sure
    "PiECE music driver": (("piece",), ()),  # sure
    "Pierre Adane Packer": (("amiga",), ("paula",)),  # checked: Wanted Team readme, Ocean games
    "Piston Collage": (("pc",), ()),  # sure
    "Piston Collage Protected": (("pc",), ()),  # sure
    "PlayerPro": (("mac",), ()),  # checked: PlayerPRO is a Macintosh tracker
    "Playstation 2 Sound Format": (("ps2",), ("spu2",)),  # sure
    "Playstation Sound Format": (("psx",), ("spu",)),  # sure
    "PMD": (("pc98",), ("opn", "opna")),  # sure
    "PokeyNoise": (("atari8",), ("pokey",)),  # checked: UADE readme, Atari 8-bit music (Whittaker, Hubbard, Langston)
    "Pollytracker": (("c64",), ("sid",)),  # checked: Modland and CSDb, a 4-channel sample tracker for the C64
    "Polytracker": (("pc",), ()),  # sure
    "Powertracker": (("amiga",), ("paula",)),  # checked: UADE's Laxity player (Amiga)
    "Pretracker": (("amiga",), ("paula",)),  # sure
    "Professional Sound Artists": (("amiga",), ("paula",)),  # sure
    "Protracker": (("amiga",), ("paula",)),  # sure
    "Protracker IFF": (("amiga",), ("paula",)),  # sure
    "ProTrekkr": (("pc",), ()),  # sure
    "ProTrekkr 2.0": (("pc",), ()),  # sure
    "Psycle": (("pc",), ()),  # sure
    "Pumatracker": (("amiga",), ("paula",)),  # sure
    "Quadra Composer": (("amiga",), ("paula",)),  # sure
    "Quartet PSG": (("atarist",), ("ay",)),  # checked: Wanted Team readme, synthesised on the ST's chip
    "Quartet ST": (("atarist",), ("ay",)),  # checked: Wanted Team readme, Atari ST samples played through the YM2149
    "RamTracker": (("pc",), ()),  # checked: Modland, DOS, for the AWE32 (EMU8000, no id)
    "Real Tracker": (("pc",), ()),  # checked: Modland, a DOS tracker
    "RealSID": (("c64",), ("sid",)),  # sure
    "Renoise": (("pc",), ()),  # sure
    "Renoise Old": (("pc",), ()),  # sure
    "Richard Joseph": (("amiga",), ("paula",)),  # sure
    "Riff Raff": (("amiga",), ("paula",)),  # sure
    "Rob Hubbard": (("amiga",), ("paula",)),  # sure
    "Rob Hubbard ST": (("atarist",), ("ay",)),  # sure
    "Ron Klaren": (("amiga",), ("paula",)),  # sure
    "S98": (("pc98", "fmtowns"), ("opna", "opn", "ym2612")),  # checked: catalog, mostly PC-88/98 and 138 FM Towns songs
    "Sam Coupe COP": (("samcoupe",), ("saa1099",)),  # sure
    "Sam Coupe SNG": (("samcoupe",), ("saa1099",)),  # sure
    "Saturn Sound Format": (("saturn",), ("scsp",)),  # sure
    "SBStudio": (("pc",), ()),  # checked: Modland, DOS (Sound Blaster, GUS)
    "SC68": (("atarist", "amiga"), ("ay", "paula")),  # checked: sc68 plays Atari ST, STE and Amiga rips
    "SCC-Musixx": (("msx",), ("scc",)),  # sure
    "Screamtracker 2": (("pc",), ()),  # sure
    "Screamtracker 3": (("pc",), ()),  # sure
    "SCUMM": (("amiga",), ("paula",)),  # checked: UADE player, LucasArts' Amiga games
    "Sean Connolly": (("amiga",), ("paula",)),  # sure
    "Sean Conran": (("amiga",), ("paula",)),  # sure
    "SGC": (("mastersystem", "gamegear", "coleco"), ("sn76489", "opll")),  # sure: opll for the Japanese FM unit
    "Shroom": (("snes",), ("spc700",)),  # checked: csrc/retro/shroom_engine.cpp, Mario Paint's composer
    "SidMon 1": (("amiga",), ("paula",)),  # sure
    "SidMon 2": (("amiga",), ("paula",)),  # sure
    "Sidplayer": (("c64",), ("sid",)),  # sure
    "Silmarils": (("amiga",), ("paula",)),  # checked: UADE player, Michel Pernot
    "Skale Tracker": (("pc",), ()),  # sure
    "Slight Atari Player": (("atari8",), ("pokey",)),  # sure
    "SNDH": (("atarist",), ("ay",)),  # sure
    "Sonic Arranger": (("amiga",), ("paula",)),  # sure
    "Sound Club": (("pc",), ()),  # checked: docs/replayers.md, Sound Club for Windows
    "Sound Club 2": (("pc",), ()),  # checked: as Sound Club
    "Sound Images": (("amiga",), ("paula",)),  # sure
    "Sound Master": (("amiga",), ("paula",)),  # sure
    "Sound Master II v1": (("amiga",), ("paula",)),  # sure
    "Sound Master II v3": (("amiga",), ("paula",)),  # sure
    "Sound Programming Language": (("amiga",), ("paula",)),  # sure
    "SoundControl": (("amiga",), ("paula",)),  # sure
    "SoundFactory": (("amiga",), ("paula",)),  # sure
    "SoundFX": (("amiga",), ("paula",)),  # sure
    "SoundPlayer": (("amiga",), ("paula",)),  # sure
    "SoundSmith": (("iigs",), ("es5503",)),  # sure
    "Soundtracker": (("amiga",), ("paula",)),  # sure
    "Soundtracker 2.6": (("amiga",), ("paula",)),  # sure
    "Soundtracker Pro II": (("amiga",), ("paula",)),  # sure
    "Special FX": (("amiga",), ("paula",)),  # sure
    "Special FX ST": (("atarist",), ("ay",)),  # sure
    "Spectrum": (("zx",), ("ay",)),  # sure (family fallback)
    "Spectrum - ASC Sound Master": (("zx",), ("ay",)),  # sure
    "Spectrum - Chip Tracker": (("zx",), ()),  # checked: its format document, a 4-channel sample tracker
    "Spectrum - Fast Tracker": (("zx",), ("ay",)),  # sure
    "Spectrum - Flash Tracker": (("zx",), ("ay",)),  # sure
    "Spectrum - Fuxoft AY Language": (("zx",), ("ay",)),  # sure
    "Spectrum - Global Tracker": (("zx",), ("ay",)),  # sure
    "Spectrum - Pro Sound Creator": (("zx",), ("ay",)),  # sure
    "Spectrum - Pro Sound Maker": (("zx",), ("ay",)),  # sure
    "Spectrum - Pro Tracker 1": (("zx",), ("ay",)),  # sure
    "Spectrum - Pro Tracker 2": (("zx",), ("ay",)),  # sure
    "Spectrum - Pro Tracker 3": (("zx",), ("ay",)),  # sure
    "Spectrum - Sound Tracker 1.1": (("zx",), ("ay",)),  # sure
    "Spectrum - Sound Tracker 1.3": (("zx",), ("ay",)),  # sure
    "Spectrum - Sound Tracker Pro": (("zx",), ("ay",)),  # sure
    "Spectrum - Sound Tracker Pro 2": (("zx",), ("ay",)),  # sure
    "Spectrum - SQ Tracker": (("zx",), ("ay",)),  # sure
    "Spectrum - ST Song Compiler": (("zx",), ("ay",)),  # sure
    "Spectrum - Vortex": (("zx",), ("ay",)),  # sure
    "Spectrum - Vortex Tracker II": (("zx",), ("ay",)),  # sure
    "Spectrum - ZXS": (("zx",), ("ay",)),  # checked: files say "SONG BY ST COMPILE"
    "Speedy A1 System": (("amiga",), ("paula",)),  # checked: UADE player, Michael Winterberg
    "Speedy System": (("amiga",), ("paula",)),  # checked: Exotica, an Amiga sound format and editor
    "SPU": (("psx",), ("spu",)),  # sure
    "Starkos": (("cpc",), ("ay",)),  # sure
    "Startrekker AM": (("amiga",), ("paula",)),  # sure
    "Startrekker FLT8": (("amiga",), ("paula",)),  # sure
    "Stereo Sidplayer": (("c64",), ("sid",)),  # sure
    "Steve Barrett": (("amiga",), ("paula",)),  # sure
    "Stonetracker": (("amiga",), ("paula",)),  # sure
    "SunTronic": (("amiga",), ("paula",)),  # sure
    "SunVox": (("pc",), ()),  # sure
    "Super Nintendo Sound Format": (("snes",), ("spc700",)),  # sure
    "SVAr Tracker": (("pc",), ()),  # checked: Modland, a Windows tracker
    "Symphonie": (("amiga",), ("paula",)),  # sure
    "Synder SNG-Player": (("pc",), ("sid",)),  # checked: docs/replayers.md, C64 tunes recorded for a Windows program's own synthesizer
    "Synder SNG-Player Stereo": (("pc",), ("sid",)),  # checked: as Synder SNG-Player
    "Synder Tracker": (("pc",), ("sid",)),  # guess: DaNyL's Windows synth; the files name "SID" but Modland calls it phase distortion
    "Synth Dream": (("amiga",), ("paula",)),  # sure
    "Synth Pack": (("amiga",), ("paula",)),  # sure
    "Synthesis": (("amiga",), ("paula",)),  # sure
    "SynTracker": (("amiga",), ("paula",)),  # sure
    "TCB Tracker": (("atarist",), ("ay",)),  # checked: Wanted Team readme and Modland, Atari ST, samples through the YM2149
    "TFM Music Maker": (("zx",), ("opn",)),  # checked: TurboSound FM, two YM2203 on the Spectrum
    "TFMX": (("amiga",), ("paula",)),  # sure
    "TFMX ST": (("atarist",), ("ay",)),  # sure
    "The 0ok Amazing Synth Tracker": (("pc",), ()),  # checked: Modland, BeRo's Windows softsynth tracker
    "The Musical Enlightenment": (("amiga",), ("paula",)),  # sure
    "Theta Music Composer": (("atari2600",), ("tia",)),  # checked: the name older catalogs give ASMA's TIATracker songs
    "Thomas Hermann": (("amiga",), ("paula",)),  # sure
    "TIATracker": (("atari2600",), ("tia",)),  # checked: ASMA's .ttt files are TIATracker's (Kylearan), not Theta Music Composer's
    "Tomy Tracker": (("amiga",), ("paula",)),  # sure
    "TSS": (("pc",), ()),  # checked: csrc/retro/tss_engine.cpp, T'SoundSystem's own sound modules
    "Tunefish": (("pc",), ()),  # sure
    "Ultra64 Sound Format": (("n64",), ()),  # sure
    "Ultratracker": (("pc",), ()),  # sure
    "Unique Development": (("amiga",), ("paula",)),  # sure
    "Unis 669": (("pc",), ()),  # sure
    "V2": (("pc",), ()),  # sure
    "Velvet Studio": (("pc",), ()),  # sure
    "VGM Music Maker": (("megadrive",), ("ym2612", "sn76489")),  # checked: .vge songs by Shiru and others, a Mega Drive editor
    "Vic-Tracker": (("vic20",), ("vic",)),  # sure
    "Video Game Music": (("megadrive",), ("ym2612", "sn76489")),  # guess: family fallback, most of it is Sega
    "Video Game Music - BBC Micro": (("bbc",), ("sn76489",)),  # sure
    "Video Game Music - Colecovision": (("coleco",), ("sn76489",)),  # sure
    "Video Game Music - MSX1": (("msx",), ("ay", "scc", "opll")),  # sure
    "Video Game Music - MSX2": (("msx",), ("ay", "scc", "opll")),  # sure
    "Video Game Music - Nintendo Virtual Boy": (("virtualboy",), ("vsu",)),  # sure
    "Video Game Music - Sega 32X": (("32x",), ("ym2612", "sn76489")),  # sure: and its PWM, no id
    "Video Game Music - Sega Game Gear": (("gamegear",), ("sn76489",)),  # sure
    "Video Game Music - Sega Master System": (("mastersystem",), ("sn76489", "opll")),  # sure
    "Video Game Music - Sega Mega CD": (("megacd",), ("ym2612", "sn76489", "rf5c68")),  # sure
    "Video Game Music - Sega Megadrive": (("megadrive",), ("ym2612", "sn76489")),  # sure
    "Video Game Music - Sega SC-3000": (("sg1000",), ("sn76489",)),  # sure
    "Video Game Music - Sega SG-1000": (("sg1000",), ("sn76489",)),  # sure
    "Video Game Music - Sharp X68000": (("x68000",), ("ym2151", "okim")),  # sure
    "Video Game Music - Vectrex": (("vectrex",), ("ay",)),  # sure
    "Voodoo Supreme Synthesizer": (("amiga",), ("paula",)),  # sure
    "Wally Beben": (("amiga",), ("paula",)),  # sure
    "Westwood SND": (("pc",), ("pcspeaker",)),  # checked: csrc/retro/wwsnd_engine.cpp, PC speaker
    "WonderSwan": (("wonderswan",), ("wsc",)),  # sure
    "X-Tracker": (("pc",), ()),  # sure
    "YM": (("atarist", "cpc", "zx"), ("ay",)),  # sure
    "YMST": (("atarist",), ("ay",)),  # checked: UADE's YM-2149 player; the songs are Atari ST replays (Hippel, Furniss)
    "Zoundmonitor": (("amiga",), ("paula",)),  # sure
}
