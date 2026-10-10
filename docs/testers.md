# Trying the player: a guide for testers

Thank you for trying ByteBandit Music Player. It plays tracker and chip music from the
old computers and consoles (Amiga, C64, Atari, ZX Spectrum, Mega Drive, PC and many
more) in a terminal window. It has about 540,000 songs from six collections (Modland,
HVSC, Keygen Music, ASMA, ZX-Art and SNDH), and downloads each song the moment you play it.

What helps most is **listening** and saying what sounds wrong. Most of its replayers
have only been checked by measurement, never by ear.

## What you need

- Linux on an ordinary 64-bit PC (x86-64) with a processor from 2008 or later.
- An internet connection: songs come from the collections' servers.

There are two ways to get it, the same player in both. Take whichever you like.

## The AppImage: one file, nothing to install

The file is named like `bbmp-<date>-x86_64.AppImage`. Make it runnable and start it:

```sh
chmod +x bbmp-*-x86_64.AppImage
./bbmp-*-x86_64.AppImage
```

It brings everything it needs along, so it runs on older systems too (tried on Ubuntu
22.04). If it says it can't mount itself (no FUSE on your system), start it with
`--appimage-extract-and-run` after the file name.

## The Flatpak

If you have Flatpak ([flatpak.org/setup](https://flatpak.org/setup/)): the file is named
like `bbmp-<date>.flatpak`. In a terminal:

```sh
flatpak install --user bbmp-*.flatpak
flatpak run io.github.bytebandit.bbmp
```

Flatpak may ask to fetch the runtime the player needs (the Freedesktop 26.08 platform,
from Flathub); say yes. After that it is in your desktop's menu too.

## The first start

The first time it asks:

1. **Which collections you want.** Enter ticks one; "Done" takes them. Esc takes the
   usual choice. You can change it later in Settings (`0`).
2. Then it gets their catalogs ready, which takes a little while, only the first time.
3. **Whether to make the `bbmp` command.** Say yes, and from then on you can just type
   `bbmp` in a terminal.

## The keys you need

| Key | Does |
|---|---|
| arrows, Enter | move around. In Browse, → or Enter goes into a collection, a format or an artist, ← goes back; Enter on a song plays it |
| `1`–`0` | the tabs: Browse, Search, Queue, Playlists, Files, History, Info, Artist, Stats, Settings |
| `/` | search: a name, an artist, `format:ahx`, `platform:c64` |
| `r` | radio: random songs from the format, artist, platform, chip or collection you are on |
| Ctrl+R | radio from everything |
| Space | pause |
| `n` / `p` | next / previous song |
| `]` / `[` | next / previous subsong (many game tunes have several) |
| `+` / `-` | louder / softer |
| `f` | favorite |
| `i` | what the file says about itself, and how well its format plays |
| **`!`** | **this sounds wrong** (see below) |
| `?` | all the keys |
| `q` | quit |

Ctrl+R plays random songs from everything, which is a good way to hear many
different formats quickly.

## When something sounds wrong: press `!`

While the song is playing, press `!`. The player asks for a word or two: "drums too
loud", "wrong notes", "stops after a minute", "silence", "crackles". Typing nothing is
fine too; Enter notes it, Esc doesn't.

It writes down the song, its format, which replayer played it, the subsong and the
moment you pressed the key. Press it as often as you like.

**To send the list:** in a terminal, type

```sh
bbmp wrong
```

(or `./bbmp-*-x86_64.AppImage wrong`, or `flatpak run io.github.bytebandit.bbmp wrong`).
It shows the list and the file it is in, normally

    ~/.var/app/io.github.bytebandit.bbmp/data/bbmp/sounds-wrong.txt    (the Flatpak)
    ~/.local/share/bbmp/sounds-wrong.txt                               (the AppImage, or built from source)

Send that file to whoever you report to (or attach it to an issue on the project's
page). Delete it afterwards to begin a new list. The player sends nothing anywhere by
itself.

### What is worth noting

- A song that sounds broken: wrong notes, wrong speed, missing instruments, noise,
  clicks, one channel silent, much too loud or too soft.
- A song that stops much too early, or never ends.
- Something you know well that sounds different from how you remember it.

You don't need to know what is wrong. "Sounds odd" is useful.

### Other trouble

If the player stops, freezes or shows an error, write down what you did just before it
happened, and send the output of

```sh
bbmp info
```

with it. That says which version you have and how things are set up on your machine.

## Good to know

- **Some songs don't play.** A song marked `!` in the lists is known not to play. In
  Browse, a struck-out format doesn't play at all, and a format marked `!` plays with
  known faults. `i` says what is known.
- **Some formats need a file** that the player may not ship (marked `+`). "Downloads" and
  "Extra files" in Settings say which, and fetch them where they are allowed to.
- Songs you play are kept in a cache, so they start at once the next time.

## Removing it

The Flatpak:

```sh
flatpak uninstall --user io.github.bytebandit.bbmp
rm -rf ~/.var/app/io.github.bytebandit.bbmp     # its catalogs, cache and settings
rm -f ~/.local/bin/bbmp                          # the command, if you made it
```

The AppImage: delete the file, and its catalogs, cache and settings with

```sh
rm -rf ~/.local/share/bbmp ~/.cache/bbmp ~/.config/bbmp
rm -f ~/.local/bin/bbmp                          # the command, if you made it
```
