# ByteBandit Music Player

ByteBandit Music Player (`bbmp`) browses, searches and plays music from
[Modland](https://modland.com/), the huge archive of tracker modules and
computer/console music, and from HVSC (C64), Keygen Music, ASMA (Atari 8-bit), ZX-Art
(ZX Spectrum) and SNDH (Atari ST), straight from the terminal: about 540,000 songs. Songs are downloaded when you play them and cached, so it feels like streaming.

Trying it from a Flatpak or AppImage package? [docs/testers.md](docs/testers.md) is
the short guide: installing it, the keys you need, and reporting what sounds wrong.

## Setting it up

It runs on Linux. The player is Python; its replayers are built from source on your
computer, into `~/.local/opt` (their sources go to `~/.local/src`). Only the system
packages below need root.

### What you need

- Python 3.11 or newer, with `venv`; Textual and numpy (put in a venv, below)
- a C and C++ compiler, make, CMake, pkg-config, git, curl, unzip, patch, perl,
  Subversion and bison
- bubblewrap (`bwrap`): the replayers run shut in by it, without network or your files
- 7-Zip (`7z`): songs in 7z, LHA and RAR archives
- the development files of libopenmpt, libgme, libsidplayfp, libsamplerate, libpulse,
  zlib and Boost
- if you like: the freetype, X11 and ALSA development files (Arkos Tracker 3 songs),
  flac or ffmpeg (FLAC export), PyGObject (media keys)

Arch:

```sh
sudo pacman -S --needed base-devel cmake pkgconf git curl unzip patch perl subversion \
  bison bubblewrap 7zip python python-numpy libopenmpt libgme libsidplayfp \
  libsamplerate libpulse zlib boost freetype2 libx11 alsa-lib flac
```

Debian and Ubuntu (on an older release without the `7zip` package: `p7zip-full`):

```sh
sudo apt install build-essential cmake pkg-config git curl unzip patch perl subversion \
  bison bubblewrap 7zip python3 python3-venv libopenmpt-dev libgme-dev \
  libsidplayfp-dev libsamplerate0-dev libpulse-dev zlib1g-dev libboost-dev \
  libfreetype-dev libx11-dev libasound2-dev flac
```

Fedora (on an older release without the `7zip` package: `p7zip p7zip-plugins`):

```sh
sudo dnf install gcc gcc-c++ make cmake pkgconf-pkg-config git curl unzip patch perl \
  subversion bison bubblewrap 7zip python3 libopenmpt-devel game-music-emu-devel \
  libsidplayfp-devel libsamplerate-devel pulseaudio-libs-devel zlib-devel boost-devel \
  freetype-devel libX11-devel alsa-lib-devel flac
```

Other distributions have the same things under similar names; `tools/setup.sh --check`
says what is still missing, with the package names for these three.

### Installing

```sh
git clone https://github.com/creep-ltx/bbmp.git
cd bbmp
python3 -m venv --system-site-packages .venv    # the player's own Python packages
.venv/bin/pip install textual numpy
tools/setup.sh --check    # only reports what is there and what is missing
tools/setup.sh            # builds the replayers that are missing and makes the catalogs
```

The venv is there because Textual is not packaged everywhere and pip won't install into
the system's Python on Debian and Ubuntu; `./bbmp` and `tools/setup.sh` use `.venv` by
themselves when it is there. Building every replayer takes a while (it runs four
compilers at once; `JOBS=2 tools/setup.sh` for fewer). A replayer that fails to build
is reported, and only the formats it plays are missing; `tools/setup.sh` again tries
the missing ones.

## Running it

```sh
./bbmp          # the TUI, from the folder you cloned it into
```

Put it in your PATH if you like:

```sh
ln -s "$PWD/bbmp" ~/.local/bin/bbmp
```

The first start asks which of the six collections you want and makes
their catalogs. `u` in the TUI or `bbmp update` updates them later, and they update by
themselves as often as Settings says.

### Keys

Shift gives the bigger or related thing: `r` radio, `R` repeat; `l` add to a
playlist, `L` the queue as one. The name in brackets is what `keys` in the settings
file calls the key (see below).

| Key | Does |
|---|---|
| Space | pause / resume (`pause`) |
| x | stop; Shift+Space too, where the terminal sends it (`stop`) |
| n / p | next / previous song (`next`, `prev`) |
| ] / [ | next / previous subsong (`subsong_next`, `subsong_prev`) |
| . / , | seek ±10 s (`seek_forward`, `seek_back`) |
| + / - | volume (`volume_up`, `volume_down`) |
| r | radio: endless random songs from the selected format, artist, platform, chip or collection (`radio`) |
| Ctrl+R | radio from everything; in a platform or chip view, from all of that: random Amiga songs (`radio_all`) |
| R | repeat: off, the whole queue, the current song (`repeat`) |
| o | on and on: the song that plays goes on (a SID tune loops, a song that ends starts again) until another song starts; for this song only, so playlists are not changed (`forever`) |
| S | shuffle the queue on/off (`shuffle`) |
| v | visualiser, in turn: spectrum analyser, gradient spectrum (smooth bars shaded bottom to top), mirrored spectrum, spectrogram (frequency over time), oscilloscope, channel scopes (one small scope per tracker channel; their shapes are guessed from the notes, as the replayers give each channel's loudness but not its sound), channel bars, tracker pattern, off; the demo effects (copper bars, starfield, plasma and a sine scroller with the song's name, twenty seconds each, moving with the music) too when "Demo effects with v" is on in Settings. A song without channels or a pattern skips those (`visuals`) |
| / | search: words, and `artist:jester`, `format:ahx`, `title:`, `album:`, `platform:amiga`, `chip:sid`, `"two words"`, `-leave-out`; Enter, Down or Esc leave the box for the results (with none, for the tab bar) (`search`) |
| 1–0 | Browse, Search, Queue, Playlists, Files, History, Info, Artist, Stats, Settings (`tab_browse` … `tab_settings`) |
| → / ← | in Browse: into the entry under the cursor (Enter too), and back (`open_copies`, `close_copies`) |
| c | in Browse: the first list, All and the collections; in Search: search the next collection, or all (`collection`) |
| b | in Browse: the platforms (Amiga, C64, Mega Drive...); again: the sound chips (SID, Paula, YM2612...) (`view`) |
| g | go to the song that plays, in Browse (`go_to_playing`) |
| m | more by this artist: every song, format and collaboration (`artist`) |
| A | the album or game the song belongs to (`album`) |
| i | what the file says about itself: comments, instrument and sample names (`info`) |
| w / d | on the artist page: look the artist up on the web / on Demozoo (`web`, `demozoo`) |
| u | update the catalogs (`update`) |
| Enter | play from the selected song onwards (in the queue: jump to it) |
| → / ← | on a song marked ▸, which more than one collection has: one row for each collection, to play that one's copy; ← hides them, and among Browse's songs goes back (`open_copies`, `close_copies`) |
| a | add the selected song to the end of the queue; on an artist, all their songs (`enqueue`) |
| N | play it next: right after the song that plays (`play_next`) |
| f | favorite on/off (`favorite`) |
| ! | this sounds wrong: notes the song that plays, where in it and which replayer, with a word or two, in a list to send (`sounds_wrong`; see below) |
| l | add the song to a playlist (`add_to_playlist`) |
| L | save the queue as a playlist (`queue_to_playlist`) |
| Delete | take the song out of the playlist; on a playlist's name, delete the playlist (`remove`) |
| F2 | rename the playlist (`rename`) |
| Shift+Up / Shift+Down | move the song up or down in the playlist; also `K` / `J` (`move_up`, `move_down`) |
| s | save on this computer: the song, its artist, the format or the whole collection (`save`; see below) |
| e | export as FLAC, MP3, Ogg, Opus or WAV: the song, a playlist, an artist, the queue (`export`) |
| t | what is being saved and exported: how far, what failed, what is saved; stop or go on (`activity`) |
| ? | help (`help`) |
| q | quit (`quit`) |

Any key can be changed in `~/.config/bbmp/settings.json`, by the names in
brackets. Several keys for one thing are separated by commas:

```json
"keys": {"save": "d", "move_up": "shift+up,k", "move_down": "shift+down,j"}
```

### Playlists

The Playlists tab (`4`) lists your favorites and your playlists. `l` on any song adds
it to a playlist, `L` saves the whole queue as one, and `a` on a playlist's name adds
all of it to the queue. `F2` renames a playlist, and Shift+Up and Shift+Down (or `K`
and `J`) move the selected song up and down in it.

"Import a playlist file" reads an `.m3u` or `.m3u8` file from this computer. Its lines
can be links to Modland or one of its mirrors, Modland paths
(`Protracker/Jester/elysium.mod`), or files on this computer (relative to where the
list is). Enter on such a file in the Files tab does
the same, and so does the command line:

```sh
bbmp import ~/Music/lists/amiga-classics.m3u
bbmp import old.m3u --name "Party 1994"
bbmp playlists                 # your playlists and how many songs each has
```

"Import one of Modland's lists" brings in the playlists Modland publishes: people's
favourites, music disks, and the competition entries of demo parties such as Assembly
92 and The Gathering 91. Those lists name songs where they were years ago. A song
that has moved is found again only when there is no doubt: exactly one file with the
same name, by the same artist, or a song that had no artist then and has one now. The
rest are left out, and the import says how many.

Songs in playlists are never removed from the cache, like favorites.

### Exporting

`e` writes music as files that any player plays, to
`~/Music/ByteBandit Music Player/Exports`, tagged with title, artist, album and format. It
offers the song, the playlist on show, the artist's songs in the format, everything
by the artist, and the queue. Several songs go into a folder of their own named after
the list, numbered in their order (`Queue/01 - Jester - elysium.mp3`); songs that
can't be played are passed over and told. It runs behind what you do, and `e` again
has "Stop exporting".

With "Same loudness for all songs" on (Settings), every exported file is levelled to the
loudness the player plays at, measured on the whole song, so a playlist on a phone needs
no hand on the volume either. With the setting off, or with `bbmp export --no-level`,
files are as loud as the replayer plays them.
"Export songs as" in Settings has the kind:

| | |
|---|---|
| FLAC | the song as it is, about half the size of WAV |
| MP3 | plays everywhere; about 190 kbit/s |
| Ogg Vorbis | about 190 kbit/s |
| Opus | the smallest; about 128 kbit/s |
| WAV | the song as it is, and large |

WAV needs nothing. The others are written by `ffmpeg` (FLAC also by `flac`), and the
list says which of them this computer can write.
 A file with subsongs becomes one file
per subsong; silent subsongs are left out. It renders as fast as the replayer runs,
and playback carries on meanwhile.

```sh
bbmp export "Protracker/Jester/elysium.mod"
bbmp export ~/Music/AHX/Pink --format mp3 -o /tmp/pink
bbmp export --playlist "Chips" --format opus    # a playlist, into Exports/Chips
bbmp export --artist Jester                     # everything by an artist
bbmp export jester elysium            # search words work when only one song matches
```

### How long a song plays

One rule, for playing and exporting (`src/bbmp/ending.py`): a song that ends by itself
ends there; a song that loops is played once through, fading into the repeat where its
replayer can; only the songs that never end by themselves (SID tunes, SNDH, ZXTune's
formats, VGM and S98) are stopped by the player, at their known length (HVSC's list, the file's
own) or after 3 minutes, with a 4-second fade. Silence is never played at a song's
start or end. Nothing plays longer than an hour.

### Files on this computer

```sh
bbmp ~/Music/AHX            # open the player and play a folder (and its subfolders)
bbmp song.mod other.xm         # or single files
bbmp pack.zip mods.lha      # or the songs inside archives: zip, 7z, LHA, RAR
bbmp play ~/Music/AHX       # the same without the TUI
```

The Files tab (`5`) shows your music folder (`~/Music`, or `music_dir` in the settings
file). It lists folders and the files a replayer knows, and Enter plays a file and
queues the rest of its folder. → opens a folder (on an open one, goes into it), ← closes
it (on a file or a closed folder, goes up to its folder), and Enter opens and closes. The format is worked out from the file name, using the
extensions Modland's formats use. Local files can be favorites, show up in history and
statistics, and come back after a restart, like songs from Modland.

Archives are shown too (zip, 7z, LHA and RAR: Aminet's LHA files, a VGMRips pack you
downloaded), and Enter plays the songs in one. The archive is unpacked once, whole,
into the cache (`~/.cache/bbmp/unpacked`), so that songs find the files they need
beside them; 7z, LHA and RAR are unpacked by the 7z program, shut in as the replayers
are. The album page (`A`) of such a song is its archive. The Flatpak and the AppImage
carry a 7-Zip without RAR (its RAR code comes with a licence restriction), so there RAR
archives don't open; none of the collections has one, and a system's 7z opens them when
the player is installed from source.

### Artist and album pages

`m` on any song opens its artist: how many songs they have on Modland, in which
formats, their albums and games, who they wrote songs with, and how much you have
played them. Enter on a name under "Worked with" opens that person's page. `A` opens
the album or game a song belongs to.

There are no biographies, on purpose. Scene handles are shared and reused (Modland
itself needs names like "Jazz (NL)"), so any text matched to an artist by name would
often describe someone else. Everything on the page is counted from Modland's
catalog or from your own history. `w` opens a web search for the artist and `d`
searches Demozoo, in your browser, and what you find there is yours to judge.

### Song info

`i` shows what the playing file says about itself, exactly as it is in the file:
title, author, the tracker it was made with, channel and pattern counts, game and
copyright tags, the ripper, the song message, and instrument and sample names, which
is where tracker musicians wrote their greetings.

### Media keys and the desktop

The player speaks MPRIS, so the keyboard's play/pause, next and previous keys, the
desktop's media widget and `playerctl` all control it and show the current song.

### Gapless playback and seeking

While a song plays, the player decides what comes next and opens it in the
background, so one song follows the other without a gap, also with repeat, shuffle
and radio. A song that ends by itself plays to its last sample; only skipping cuts
the sound.

Seeking works in every format. Where a replayer can't jump (the Amiga and C64
emulators), the emulated machine runs at full speed to the position: about a tenth of
a second per minute for Amiga songs, about two seconds per minute for the C64.
SunVox songs jump to the line before the position. After a jump backwards, notes
that started before that line are not sounding yet, so the first seconds can be
thinner than they are when the song plays through.

### The same loudness for every song

Replayers differ in how loud they play, and songs do too: between the quietest and
the loudest it is more than 20 dB. The player measures how loud a song is and plays it
louder or softer by what it is off, so that you set the volume once.

Loudness is measured as broadcasters measure it (ITU-R BS.1770, in LUFS), which takes
into account that the ear hears some pitches better than others and doesn't count
silent passages. The target is −18 LUFS. A song's loudness is known after it has
played once, and is kept. The first time it is measured on the song's first second
before it starts, and the level follows the measurement up or down for the next five
seconds; after that, if the song turns out louder than its beginning, it is turned
down gently while it plays, and never up again. A quiet song is made louder by
12 dB at most, and nothing is made to clip.

The Info tab shows a song's loudness and by how much it is played louder or softer.
"Same loudness for all songs" in Settings switches it off. Exported files are levelled
the same way, measured on the whole song (see Exporting); `bbmp export --no-level`
leaves them as the replayer plays them.

### Songs known not to play

In a format that plays, a song here and there still fails. The player marks such a
song with `!` in its lists, and the Info tab says why. It knows them from two places:
the census, which tried up to 40 songs of every format (218 songs, shipped with the
player), and this computer, where a song that fails to open or stays silent for 30
seconds is noted as it happens. Radio passes them over. A song played on purpose is
tried again, and loses its mark the moment it plays: a new replayer, or a file put in
the extras folder, may have mended it. A server that doesn't answer is not held
against the song.

### Subsongs

Many files hold several songs: a C64 tune with 19 pieces, a Game Boy file with a
whole soundtrack. Every replayer is treated the same way. The file's own start
song plays first, then the others in order, like an album. Subsongs that stay
silent are skipped at once, and silence before a song's first note isn't played.
`]` and `[` step between subsongs by hand. Settings has a switch to play only one
subsong per file instead.

### Settings

The Settings tab (`0`) changes a setting with Enter. Everything is saved the
moment it changes, including the volume and the visualiser you last used.

| Setting | Choices |
|---|---|
| Songs with subsongs | play them all, or the first one only |
| Repeat | off, the whole queue, the current song |
| Shuffle the queue | on, off |
| Same loudness for all songs | on, off |
| Reopen where I left off | the queue, song and position come back paused on the next start |
| Browse and search | every collection, or only one (shown when you have more than one) |
| Visualizer | one of the nine visualisers (see `v`), or off |
| Demo effects with v | `v` shows the demo effects too (off at first) |
| Theme | the colours of the whole player, shown as you move through the list |
| Home mirror | the address of your own copy of the collections, asked first (empty by default) |
| Cache size limit | 0.5, 1, 2, 5, 10 GB, or no limit (default 2 GB) |
| Clear the cache now | |

When the cache goes over its limit, the songs you played longest ago are deleted
first. Favorites and the songs in the queue are never deleted.

**Home mirror.** If you keep a copy of the collections on your own network (a NAS, a
Raspberry Pi), type its address under "Home mirror", for example
`http://192.168.1.10:8088`, and songs come from it first instead of the archives'
servers. `BBMP_MIRROR=<address>` does the same for one run and counts instead of the
setting (`BBMP_MIRROR=` turns it off). The mirror has each collection in a folder of
its id, laid out as the collection's own server: `modland/pub/modules/...` (HVSC is in
there, under `HVSC/`), `asma/asma/...`, `sndh/sndh/sndh_lf/...`,
`keygenmusic/kgm/...`. ZX-Art isn't mirrored and comes from its site. A file the
mirror doesn't have comes from the archive's servers, and what it gives is checked
against the catalog like any server's. Away from home, when it can't be reached, it is
waited for once (3 seconds) and then asked last until the player is started again.

### Themes

"Theme" in Settings lists 26 themes. Moving through the list shows each one at once,
Enter keeps it, Esc goes back to the one you had. The visualisers, level meters and
the Stats tab take their colours from the theme too.

The default, **bytebandit**, is the player's own: near-black, one amber colour for what
is chosen and what moves, faint lines between the lists and a brighter one over the list
that has the keys. Four are Amiga themes:

| Theme | Looks like |
|---|---|
| amiga-workbench-1.3 | Workbench 1.x: blue, white, black and orange, and nothing in between |
| amiga-workbench-3.1 | Workbench 2 and 3: grey, black, white and blue |
| amiga-copper | a demo: black, with level bars shaded like copper bars |
| amiga-guru-meditation | the red on black of the system's crash alert |

The others come with Textual: textual-dark (the default before October 2026), nord, gruvbox, dracula,
tokyo-night, monokai, catppuccin, solarized, rose-pine and more, dark and light.

### What is known about a format

Formats differ in how well they play, and the player says what is known. In Browse,
the format list marks a format with `✓` when its replayer was compared with the
original and is the same, and with `!` when it plays with faults or gaps; a format
that doesn't play is struck out. The line under the lists says what is known about
the format under the cursor or the one one is in: how many of its songs play, what was checked, what is wrong
or missing, or why it can't be played. The Info tab says the same for the song that
is playing, and a song that can't be played says why.

```sh
bbmp formats skale renoise     # the formats with that in their name, with notes
bbmp formats --notes           # every format with its notes
```

[docs/formats.md](docs/formats.md) has the whole list. It is made from a census of
up to 40 songs of every format (`tools/format_census.py`) and from notes on what was
checked (`docs/format-notes.toml`).

### Downloads

Some formats are played with a file of the program their songs were made with: Sound
Club's songs with its instrument bank, CyberTracker's by the C64 tracker itself, MuSICA's,
MPK's and OPLLDRV's (MSX) with those programs' sound drivers. Their
makers give these files out for everyone to download, under terms that don't allow
passing them on as part of something else. So the player doesn't come with them, and
downloads them from their makers when you say so.

It asks once, at the first start. After that they are under "Downloads" in Settings,
and:

```sh
bbmp downloads                 # what there is, whose it is, where it is from
bbmp download all              # or: bbmp download soundclub
bbmp download --remove cybertracker
```

| Download | Size | Used by | Without it |
|---|---|---|---|
| `soundclub` | 2.9 MB | Sound Club, Sound Club 2 | only about a quarter of the songs play |
| `cybertracker` | 79 KB | Cybertracker C64 | its songs can't be played |
| `msxdrivers` | 49 KB | MuSICA, MPK, OPLLDRV | their songs can't be played (other MSX songs play) |
| `amigaplayers` | 0.7 MB | most Amiga formats (UADE's players: AHX, TFMX, Sonic Arranger, IFF-SMUS, the game composers' formats, ...) | those songs can't be played; SoundMon, MusicMaker, Art Of Noise, Pretracker, FredMon, SidMon 1, Maniacs Of Noise, Music Assembler, Medley and PokeyNoise play without it |
| `sc68replays` | 0.7 MB | SC68 (sc68's replay routines) | the SC68 songs that name a routine instead of carrying their own can't be played (about a third) |
| `beepoladata` | 1.1 MB | Beepola (the Spectrum 48K ROM, Shiru's Phaser1 drum samples, a table of Savage's player) | the Beepola songs whose engine reads one of them can't be played |
| `ixalancesamples` | 0.5 MB | iXalance (three drum samples of Shortcut Software's player) | the 7 iXalance songs that use one of them can't be played; the other 17 play |
| `organyabank` | 0.4 MB | Organya, Organya 2 (Studio Pixel's 100 waveforms and OrgMaker's drums, as organya.h gives them) | Organya songs can't be played where the helper has no bank of its own (a build from source installs one) |

What is downloaded is checked against a checksum before anything of it is kept.

### Extra files

A few formats are played with a piece of the machine they are from: the drum sounds
in a sound chip's ROM, a console's BIOS, a game's instrument sounds. Those belong to
someone else, so the player doesn't download them. If you have one, put it into the
extras folder under the name given here, and the player finds it:

    ~/.local/share/bbmp/extras/

| File | Size | Used by | Without it |
|---|---|---|---|
| `ym2608_adpcm_rom.bin` | 8,192 bytes | PMD, FMP, S98 and VGM files, Furnace songs (the YM2608: NEC PC-98 and others) | songs that use the chip's built-in drums play without them |
| `coleco.rom` | 8,192 bytes | SGC | the ColecoVision songs that call the BIOS are refused (2 of 386); Master System and Game Gear songs don't need it |
| `mario-paint-notes.wav` (or `notes.wav`) | 26,936,466 bytes | Shroom | Shroom's songs can't be played |
| `kernal.bin` | 8,192 bytes | C64 tunes (HVSC, RealSID) | tunes written in BASIC are silent, and some that call the C64's system |
| `basic.bin` | 8,192 bytes | C64 tunes | the same; it is used with the KERNAL |
| `chargen.bin` | 4,096 bytes | C64 tunes | a few tunes that read the character set play wrong |
| `hebios.bin` | 524,288 bytes | Playstation Sound Format, Playstation 2 Sound Format | PlayStation and PlayStation 2 songs can't be played |

- `ym2608_adpcm_rom.bin` is a dump of the YM2608 chip's rhythm ROM; emulators of the
  PC-98 use the same file under the same name. It is also found in
  `~/.local/share/98fmplayer/`.
- `coleco.rom` is a dump of the ColecoVision's BIOS; emulators of the console use
  the same file.
- `kernal.bin`, `basic.bin` and `chargen.bin` are the Commodore 64's three ROMs. C64
  emulators such as VICE come with them, under names like `kernal-901227-03.bin`,
  which the player takes too.
- `notes.wav` is in the data folder of Mariopants 1.1 by Brad Smith, a program that
  plays Mario Paint's songs.
- `hebios.bin` (or `scph10000_he.bin`) is the PlayStation 2's BIOS in the trimmed form
  that Highly Experimental, the PlayStation player, runs: made from a dump of the BIOS
  of your own PlayStation 2 (SCPH-10000, the first Japanese model) with `mkhebios`, the
  tool that comes with Highly Experimental's source (and ZXTune's). It is Sony's, so no
  package of the player carries it. Only the known good image is taken (checked by its
  SHA-256); the full 4 MB dump is not.

The player says when a file is needed. A format that can't play without one has a
`+` in the format list, and the line under the lists names the file, its size and
the folder. A song of such a format says the same when you try to play it.
"Extra files" in Settings lists them all, and so does:

```sh
bbmp extras
```

A file with the right name and another size is not taken: the player says what it
found. `BBMP_EXTRAS` names another folder. The player's own variables (`BBMP_EXTRAS`,
`BBMP_KEPT` and the rest) are still read by their old names, `MODLAND_EXTRAS` and so on,
when the new one isn't set.

### Collections

There are six collections: Modland; HVSC, the High Voltage SID Collection of
Commodore 64 music; Keygen Music, the music of keygens, cracks and trainers from
[keygenmusic.tk](https://keygenmusic.tk) (about 4,350 tracker modules from some 450
release groups, filed under their tracker's format and then their group); ASMA, the
[Atari SAP Music Archive](https://asma.atari.org) of Atari 8-bit music (about 6,400
tunes of the POKEY chip, twice Modland's, filed under their composer); [ZX-Art](https://zxart.ee), the ZX Spectrum's music (about 29,500 tunes: AY trackers,
beeper, digital, TurboSound FM, with their lengths, filed under Modland's format names
and then their composer); and [SNDH](https://sndh.atari.org), the Atari ST's music
(about 5,900 tunes, newer and unpacked versions of what Modland has). Settings or
`bbmp collections` choose which ones you have. HVSC is a collection of its own that Modland hosts, so the player
shows it both ways: as a format in Modland, and as a collection in HVSC's own layout
(Musicians, Games, Demos). A tune is the same favorite either way. The player is built
for more collections: the catalog, the cache, search, playlists, favorites and history
all know which collection a song is from, and a playlist can mix songs from any of
them with files on this computer.
[docs/collections.md](docs/collections.md) says how one is added.

#### Choosing collections

You have the collections you choose, at the first start and later in Settings
("Collections I have") or with `bbmp collections --use modland hvsc`. Each has a
catalog of its own, so a collection you don't have costs nothing: it isn't downloaded,
updated or asked about. Turning one off takes its catalog away; your saved music,
favorites and playlists stay, and its songs come back when you choose it again.

HVSC can be had on its own. With Modland, its catalog comes from Modland's file list,
which is had anyway. Alone, it comes from HVSC's own list (5 MB, where Modland's is
21), and its tunes from HVSC's own servers.

#### Updates

`u` updates the catalogs of the collections you have. They also update by themselves,
behind what you do, as often as "Update automatically" in Settings says: every day,
week (the default), two weeks or month, or never. `bbmp collections --every month`
sets it from the command line.

A collection has several servers. The player asks the fastest one first, and when
that doesn't answer within 3 seconds or doesn't have a file, the next: Modland's own
server, three mirrors of it, and for C64 tunes HVSC's own servers too. The song the
cursor rests on in a list is downloaded in the background, so it starts at once when
chosen. What a server gives is checked
against the checksum Modland's file list has for the file, and a server that gives
another file is passed over. `bbmp collections` lists the servers.

Browse's first list has All and each collection, with its own formats, artists,
platforms and news; in Search, `c` switches between them. Lists of songs from
several collections show where a song is from. A file that two collections have shows
once, as "▸ title … Modland + Keygen Music", which is decided by checksum and never by
name; → opens it to a row for each collection, and Enter plays the copy chosen. A song
one collection has in several places shows in all of them.

#### Browsing

Browse works as the file managers ranger and yazi do, in three columns: the list you
came from, the list you are in, and a look at what the entry under the cursor holds.
→ or Enter goes into it, ← goes back. The first list is where the music comes from:
**All**, and each collection (Modland, HVSC, ZX-Art...). Each of them holds the same
ways to browse it:

| Entry | What is in it |
|---|---|
| Formats | the formats, then a format's artists, then their songs |
| Platforms | the machines (Amiga, C64, Mega Drive...), each one's formats, artists, songs |
| Chips | the sound chips (SID, Paula, YM2612, OPL...), the same way |
| Artists | everyone, A to Z, with everything they made in every format |
| New | what has been added or changed, by day |

With only one collection, the first list is these ways.

Among the songs, the column on the right says what the song under the cursor is. A
narrow terminal leaves out the column you came from. Browse opens where it was left.

#### Sizes, and what is new

The catalog knows the size of every file, from the list of sizes Modland publishes,
so nothing has to be fetched to tell how big something is. Browse shows it at every
step: the whole collection in the title of the list of formats, a format in the title
of the list of artists, an artist above the songs, and each song in its own column.

Modland publishes a note each day of what it added and changed. The catalog keeps the
last 60 days of them. New, in All and in each collection in Browse, shows them day by
day, and the songs of a day play like any other list. HVSC comes out as a whole a
couple of times a year, so its days are few and large.

At the first start the player fetches the lists of the collections you choose,
fresh from them (Modland's is 21 MB), and makes its catalogs from them. What the
lists don't say, which chip each song is for, comes with the player
(`tools/make_chips.py` writes it).

When it starts, the player asks the server how old its list of files is. If the list
is newer than the catalog, it says so, and `u` brings the catalog up to date.

#### Keeping music on this computer

`s` saves music on this computer, to play it without the servers: the song under the
cursor, its album or game, its artist, the whole format, or the whole collection; in
the Playlists tab, the playlist on show or your favorites, which are then kept as they
change (a song added is fetched, a song taken out goes). It is one key for all
of them, since saving, keeping and downloading are one thing. The list says how large each
is and how much room the disk has, and nothing is begun that doesn't fit. What the
cache has already is taken from there and not downloaded again. The files
are fetched behind what you do, a few at a time and from several servers, and you can
go on listening. `⤓` marks what is kept.

What is kept is played from the disk, and its servers are not asked. The cache's
size limit doesn't touch it. Stopping the player stops the fetching, and the next
start goes on where it was.

When the catalog is updated, the player sets what is kept against it: what the
collection added is fetched, what it changed is fetched again, what it moved is moved
here too without a download, and what it no longer has stays where it is: kept music is
only taken away when you stop keeping it. A list that comes back empty, or with far
fewer songs than before, is not believed; the catalog stays as it was and is updated
again later. The checksums in the collection's list decide what is the same, so no file
has to be read for it.

`s` on something saved stops keeping it and frees the room. "Saved on this computer"
in Settings lists it all.

Kept music is in your music folder, in `~/Music/ByteBandit Music Player`, in a folder for
every collection, laid out as on its server (`Modland/Protracker/Jester/...`), so a
file manager finds it and
any other program can play it. The Files tab shows it too, and a song played from
there is the collection's song, with its favorites and its history.

"Folder for saved music" in Settings changes that: to the player's own folder, out of
sight, or to any folder you name, on another disk for one. What is kept
moves there with its database: in a moment on the same disk, by copying to another
one, and only if there is room. A folder that has kept music already is taken as it
is.

```sh
bbmp keep                          # fetch what is missing of what is kept, and show it
bbmp keep Protracker/Jester/elysium.mod    # keep a song
bbmp keep modland TFMX "Chris Huelsbeck" "Turrican 2"   # an album or game
bbmp keep --playlist Chips                 # a playlist, kept as it changes
bbmp keep --playlist favorites
bbmp keep hvsc                     # keep a whole collection
bbmp keep modland Protracker       # a format
bbmp keep modland Protracker Jester
bbmp keep --stop modland Protracker Jester
bbmp keep --check                  # look whether every file is still there
bbmp keep --folder ~/Music/ByteBandit\ Music\ Player    # have it there, and move what is kept
```

"Fetch saved music" in Settings keeps the fetching to the night (midnight to seven),
and "Fetching speed" to a speed, so that it doesn't take the line when you need it.
All of HVSC comes as the one archive HVSC publishes of itself (85 MB, fetched in
seconds), not as its 61,000 files; every tune in it is checked against the catalog, and
one it has in another version is fetched on its own.

All of Modland is 99 GB in half a million files, and took some six hours when it
was measured: the servers give about 4 MB a second together. `bbmp keep` in a
terminal of its own is the way to do that; it can be stopped with Ctrl-C and goes on
from there the next time.

```sh
bbmp collections               # the collections: songs, size, age of the list, cache use
bbmp new                       # what the collections added and changed, day by day
bbmp new -d 7 -c modland       # the last week of one collection
bbmp update modland            # update one collection's catalog
bbmp search hubbard -c hvsc
bbmp radio -c hvsc             # random C64 tunes
```

### Statistics

The Stats tab (`9`) shows plays, listening time, plays per day, and your most
played songs, artists and formats, for this week, month, year, or all time. A
play counts once you have listened for 20 seconds, so skipping through songs
doesn't fill the charts.

### Command line

```sh
bbmp search turrican           # print the songs that match (✗ = format can't be played)
bbmp play "AHX/Pink/inside.ahx"
bbmp play jester stardust      # plays the search results
bbmp radio AHX                 # random AHX forever
bbmp radio Protracker Jester   # random songs by one artist
bbmp formats                   # every format, song counts, which ones play
bbmp info                      # catalogs, cache, sandbox, and which replayers are there
bbmp wrong                     # the songs noted with ! as sounding wrong, and where that list is
bbmp command                   # as an AppImage: make the bbmp command (~/.local/bin/bbmp); as a Flatpak: show the line that makes it
```

### When a song sounds wrong

Press `!` while it plays. The player asks for a word or two ("drums too loud",
"stops after a minute"; nothing is fine too) and adds the song to a plain text
list: when, which song, its format, the replayer, the subsong and the moment in it.
`bbmp wrong` shows the list and where the file is (`~/.local/share/bbmp/sounds-wrong.txt`;
in the Flatpak `~/.var/app/io.github.bytebandit.bbmp/data/bbmp/sounds-wrong.txt`).
Send that file to whoever makes the player; delete it to begin a new list. The
player sends nothing by itself.

## How it plays everything

| Backend | Plays |
|---|---|
| [libopenmpt](https://lib.openmpt.org/) | Protracker, Fasttracker, Impulse/Scream Tracker, OctaMED, and most other trackers |
| [UADE](https://zakalwe.fi/uade/) | Amiga formats: AHX, TFMX, Future Composer, SoundMon, Delitracker customs, ..., plus Quartet ST and Hippel ST (runs the original replayers on an emulated Amiga; most of those players with the `amigaplayers` download) |
| [Game Music Emu](https://github.com/libgme/game-music-emu) | NES, SNES SPC, Game Boy, PC Engine, Atari 8-bit SAP, Mega Drive VGM/GYM, Master System, BBC Micro, Spectrum AY |
| [libsidplayfp](https://github.com/libsidplayfp/libsidplayfp) | C64 SID (HVSC), with song lengths from HVSC's database |
| [ZXTune](https://github.com/vitamin-caig/zxtune) | Spectrum trackers, Nintendo DS (2SF), GBA (GSF), PlayStation 1/2 (PSF), N64 (USF), Saturn (SSF), Dreamcast (DSF), V2 |
| bbmp-vgm, the player's own, with [libvgm](https://github.com/ValleyBell/libvgm)'s sound chip cores | PC-98 S98, Virtual Boy / ColecoVision / Vectrex / MSX / X68000 / Spectrum VGM, and VGM files of your own (almost every chip) |
| [StSound](https://github.com/arnaud-carre/StSound) | Atari ST YM |
| [psgplay](https://github.com/frno7/psgplay) | Atari ST SNDH |
| [sc68](http://sc68.atari.org/) | SC68 (Atari ST / Amiga; the replay routines some songs name with the `sc68replays` download) |
| [AdPlug](https://github.com/adplug/adplug) | AdLib (CMF, ROL, HSC, RAD, D00, A2M, LDS, IMF, ...) through the Nuked OPL3 emulator |
| [mdxmini](https://github.com/mistydemeo/mdxmini) | Sharp X68000 MDX |
| [98fmplayer](https://github.com/takamichih/fmplayer) | NEC PC-98 FMP |
| [libkss](https://github.com/digital-sound-antiques/libkss) | MSX: KSS, MGSDRV, MoonBlaster; MuSICA, OPLLDRV and MPK with the `msxdrivers` download |
| [EUPPlayer](https://github.com/tjhayasaka/eupplayer) | Fujitsu FM Towns Euphony |
| [Furnace](https://github.com/tildearrow/furnace) | Deflemask, FamiTracker |
| [libpxtone](https://github.com/Wohlstand/libpxtone) | Piston Collage (pxtone) |
| [organya.h](https://github.com/Strultz/organya.h) | Organya (Cave Story) |
| [Highly Quixotic](https://github.com/kode54/Highly_Quixotic) | Capcom QSound (QSF) |
| [snsf9x](https://github.com/loveemu/snsf9x) | Super Nintendo (SNSF) |
| [SunVox library](https://warmplace.ru/soft/sunvox/sunvox_lib.php) | SunVox |
| [libxmp](https://github.com/libxmp/libxmp) | Acorn Archimedes: Archimedes Tracker, Coconizer; Megatracker |
| [Retrovert](https://github.com/RetrovertApp)'s Klystrack plugin (iXalance is the player's own engine in the same helper) | Klystrack |
| Replayers of the player's own ([docs/replayers.md](docs/replayers.md)) | Musicline Editor, FAC SoundTracker, Psycle, SoundSmith, Noise Tracker GS, SGC, Game Boy GBR, WonderSwan, SPU, EdLib, GoatTracker, ProTrekkr, JayTrax, Skale Tracker, Ken's Digital Music, Buzzic, SBStudio, MikMod UNITRK, PlayerPro, DSIK, Cubic Tiny XM, Buzz, MaxTrax, Zoundmonitor, Jesper Olsen, Stonetracker, SCC-Musixx, NerdTracker 2, Cybertracker C64, Gameboy Tracker, Onyx, Westwood SND, Picatune, TSS, Beepola, Flash Tracker, Starkos, Arkos Tracker, Fuxoft AY Language, AY Amadeus, Vortex Tracker II, PMD (NEC PC-98, on ymfm), PiECE, MONOTONE, DigiTrekker, DarkWave Studio, Ad Lib formats AdPlug lacks, iXalance |

`src/bbmp/backends/formats.py` records which backend plays which Modland
format. It is generated by `tools/format_census.py`, which downloads up to 40 songs of
every format, finds the backend that plays them, and counts how many of them play.

libopenmpt, libgme and libsidplayfp come from the system packages. The rest are
built from source into `~/.local/opt` (nothing needs root):

| Script | Builds |
|---|---|
| `tools/build-uade.sh` | UADE |
| `tools/build-zxtune.sh` | `bbmp-zxtune`, a small helper built inside the ZXTune source tree |
| `tools/build-vgm.sh` | `bbmp-vgm`, the player's own VGM and S98 player with libvgm's licensed sound chip cores (as ZXTune's sources carry them), a program of its own (libvgm's OPL4 is GPL 2 only) |
| `tools/build-chip.sh` | `bbmp-chip`, one helper with StSound, sc68, AdPlug, mdxmini, pmdc (the player's own PMD driver, on ymfm), 98fmplayer, libkss, EUPPlayer, organya.h, libpxtone and Highly Quixotic |
| `tools/build-psgplay.sh` | `bbmp-psgplay`, psgplay for SNDH, a program of its own (psgplay is GPL 2 only) |
| `tools/build-snsf.sh` | `bbmp-snsf`, snsf9x for SNSF, a program of its own (Snes9x is for non-commercial use only) |
| `tools/build-furnace.sh` | a headless Furnace and its `bbmp-furnace` wrapper |
| `tools/build-gme.sh` | Game Music Emu with the player's fix for PC Engine songs |
| `tools/install-sunvox.sh` | the prebuilt SunVox library |
| `tools/build-xmp.sh` | libxmp and its helper `bbmp-xmp` |
| `tools/build-retrovert.sh` | Retrovert's iXalance and Klystrack plugins, and `bbmp-rv`, the helper that loads them |
| `tools/build-musicline.sh`, `build-fac.sh`, `build-psycle.sh`, `build-iigs.sh`, `build-nez.sh`, `build-console.sh`, `build-misc.sh`, `build-pc.sh`, `build-synth.sh`, `build-amiga.sh`, `build-retro.sh`, `build-ay.sh`, `build-adlib.sh`, `build-darkwave.sh`, `build-pcmusic.sh`, `build-piece.sh`, `build-chipnsfx.sh`, `build-tiatracker.sh`, `build-synder.sh`, `build-t0ast.sh`, `build-berotracker.sh`, `build-axs.sh` | the player's own replayers, one helper each |
| `tools/build-shut.sh` | `bbmp-shut`, which shuts a replayer in by Landlock where bubblewrap can't start (see below) |
| `tools/setup.sh` | all of the above, skipping what is already built |

The helpers run as separate processes and stream raw audio over a pipe, so an
emulator that crashes on a bad file only ends that song. `tools/try_helper.py` runs a
helper on the songs of a format and reports which play, which are silent and which
fail.

Retrovert is Daniel Collin's player, which has every replayer as a plugin behind one
C interface. `bbmp-rv` is a host for that interface, so other plugins of
Retrovert's can be added by building them. Its iXalance plugin is for non-commercial
use only (CC BY-NC-SA 4.0).

PC-98 FMP, PMD, S98 and VGM songs and Furnace songs that use the YM2608 sound chip's
built-in drums need a dump of the YM2608 rhythm ROM, `ym2608_adpcm_rom.bin` (8192 bytes), in
the extras folder (see "Extra files" above; `~/.local/share/98fmplayer/` is looked in
too). Without it those songs play without drums. The SID backend
compiles a small C++ shim (`csrc/sidshim.cpp`) with g++ the first time it is needed.

## The replayers are shut in

Every replayer is old code reading files from the internet, so every one runs shut
in: the helper programs, and the big libraries too (libopenmpt, UADE, Game Music Emu,
libsidplayfp and SunVox), each in a process of its own that the player talks to over
a pipe. The sandbox is [bubblewrap](https://github.com/containers/bubblewrap), the one
Flatpak uses: no network, the system read-only, the home folder hidden but for the
replayers, the song's folder and the one above it, the collections' files, your saved
music and the extra files, and a /tmp of its own. A replayer that crashes ends its
song and nothing else. Each may take at most 2 GB of memory.

Where bubblewrap can't start (inside a Flatpak, which gives no user namespaces), the
replayers are shut in by [Landlock](https://docs.kernel.org/userspace-api/landlock.html)
instead, through the small launcher `bbmp-shut` (`tools/build-shut.sh`): each may read
the system's programs and libraries and the same folders as under bubblewrap, write
only into a temporary folder of its own (gone when it ends), and open no socket (a
seccomp filter, on x86-64 and AArch64; Landlock also refuses TCP). Unlike bubblewrap
it can still see the names of files it can't open. What Landlock refuses grows with
the kernel:

| Landlock ABI | Linux | Refused besides |
|---|---|---|
| 1 | 5.13 | reading, writing, running and making files outside its folders |
| 2 | 5.19 | moving files from one folder to another |
| 3 | 6.2 | truncating files outside its folders |
| 4 | 6.7 | TCP (seccomp refuses every socket on any kernel) |
| 5 | 6.10 | ioctls on devices |
| 6 | 6.12 | signals and abstract Unix sockets to processes outside |

If no sandbox works (no bubblewrap and no Landlock), the player says so when it starts,
and the replayers then run with your own rights.

The Flatpak may use the network, sound, your Music folder and (read only) your Downloads
folder. To play songs from another folder:
`flatpak override --user --filesystem=/path/to/songs:ro io.github.bytebandit.bbmp`.
| 9 | | Unix sockets by path outside (the desktop's D-Bus, PulseAudio), which seccomp keeps it from anyway |

On a kernel without Landlock, or without `bbmp-shut`, the replayers run as before.
`bbmp info` says which sandbox is on and what it refuses. `BBMP_SANDBOX=landlock`
uses Landlock where bubblewrap would work too (for testing it), and `BBMP_SANDBOX=0`
turns the sandbox off for looking into a replayer.

## Where things are kept

| What | Where |
|---|---|
| The catalogs, one per collection (SQLite, full-text search) | `~/.local/share/bbmp/catalogs/<collection>.db` |
| Favorites, playlists and every play (history, statistics) | `~/.local/share/bbmp/user.db` |
| Exported songs | `~/Music/ByteBandit Music Player/Exports/` |
| The queue and position to resume | `~/.local/share/bbmp/session.json` |
| Settings | `~/.config/bbmp/settings.json` |
| The cache: songs that were played (a folder per collection, laid out as on its server). It is the player's own business, with a size limit | `~/.cache/bbmp/files/<collection>/` |
| Kept music, laid out the same, and the database of what is kept (`kept.db`) | `~/Music/ByteBandit Music Player/`, or the folder set in Settings, or `BBMP_KEPT` |
| Log (stderr from the replayers) | `~/.cache/bbmp/bbmp.log` |

## Demo video

`tools/record_demo.py demo.mp4` runs the TUI headlessly through a scripted tour
(browse, search, four of the nine visualisers, radio), capturing the screen and the audio
it plays, and assembles an MP4 with ffmpeg. Nothing plays on the speakers.
`tools/record_demo.py --tour tour.mp4` records the format tour instead: 17 machines
from 32-channel XMs to the Virtual Boy, X68000 and Sam Coupé, with captions.

`tools/record_overview.py overview.mp4` records the overview: the features (browse,
search, subsongs, favorites and queue, radio, statistics, settings) and ten machines
in two and a half minutes, with a caption band under the terminal, title and end
cards that show the spectrum of the music playing, and the loudness of the different
replayers evened out. Settings, statistics and favorites used in the recording live
in a temporary folder.

## Tests

```sh
python3 -m pytest              # includes a few downloads from Modland
python3 -m pytest -m "not network"
```

The TUI tests drive the real app headlessly with Textual's pilot, and audio goes
to a null output, so the test suite makes no sound.

## Licence

ByteBandit Music Player is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version. It is distributed
in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See [COPYING](COPYING).

That is the player's own code. The replayers are programs of their own that the player
starts, and each keeps the licence of what it is made from (GPL, LGPL, BSD, MIT and others;
`bbmp replayers` and the Info tab name them, and [docs/replayers.md](docs/replayers.md)
has the details). The source of every replayer, with the changes made to it, is published
with the player. The player is free and will never be sold.

The licence texts and credits of every part are in [licences/](licences/README.md),
which every package carries. Among them:

Powered by SunVox (modular synth & tracker)
Copyright (c) 2008 - 2026, Alexander Zolotov <nightradio@gmail.com>, WarmPlace.ru

(SunVox's other texts: [licences/sunvox/](licences/sunvox/).)
