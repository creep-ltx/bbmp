# SPDX-License-Identifier: GPL-3.0-or-later
"""The Help tab."""

HELP = """\
[b]Playing[/b]
  Space      pause / resume        x  stop (Shift+Space too, where the terminal sends it)
  n / p      next / previous song
  ] / \\[      next / previous subsong
  . / ,      seek forward / back 10 seconds
  + / -      volume
  r          radio: random songs from the selected format or artist
  Ctrl+R     radio from everything (in a platform or chip view: from all of that)
  R          repeat: off, whole queue, current song
  o          on and on: the song that plays goes on (a SID tune loops, a song that
             ends starts again) until another song starts; for this song only
  S          shuffle on/off
  v          visualiser: spectrum, gradient, mirrored, spectrogram, scope,
             channel scopes, channels, pattern, off (demo effects: Settings)

[b]Finding music[/b]
  → / ←      in Browse: into the entry under the cursor (Enter too), and back
             (Esc too).
             First All or a collection, then formats, platforms, chips, artists
             or what is new
  /          search             1-0  switch tabs (5 Files, 7 Info, 0 Settings)
             artist:jester  format:ahx  title:  album:  "two words"  -leave-out
             platform:amiga  chip:sid  (platform:c64, platform:genesis, chip:ym2612...)
             (Esc leaves the search box for the results)
  c          in Browse: All and the collections; in Search: search the next
             collection, or all of them (with more than one)
  b          in Browse: the platforms (Amiga, C64, Mega Drive...); again: the
             sound chips (SID, Paula, YM2612...). A search keeps to the platform
             or chip Browse is in, unless it names a platform: or chip:
  g          go to the song that plays, in Browse
  m          more by this artist: all their songs, formats and collaborations
             (there: w web search, d Demozoo, Enter on "Worked with" to follow)
  A          the album or game this song belongs to
  i          what the file says about itself: comments, instrument names
  u          update the catalogs (the player says at its start when a collection
             has a newer list of its files)

[b]Your music[/b]
  Enter      play from the selected song (in the queue: jump to it)
  → / ←      on a song marked ▸, which more than one collection has: show one row
             for each collection, to play that collection's copy; ← hides them
             (among Browse's songs, ← goes back)
  a          add the selected song (or whole artist) to the end of the queue
  N          play it next: right after the song that plays
  f          favorite on/off (selected song, or the one playing)
  !          this sounds wrong: notes the song that plays, where in it, and a
             word or two, in a list to send (bbmp wrong shows it and where it is)
  l          add the song to a playlist        L  save the queue as a playlist
  Delete     take the song out of the playlist (on a playlist's name: delete it)
  F2         rename the playlist
  Shift+Up / Shift+Down (or K / J)   move the song up or down in the playlist
  s          save on this computer, to play without the servers: the song, the
             artist, the format or the whole collection. It goes to your music
             folder (Settings has the folder), and ⤓ marks what is saved
  e          export as files any player plays: the song, the playlist, the
             artist's songs or the queue, as FLAC, MP3, Ogg, Opus or WAV
             (Settings has the choice), to Exports in the same folder
  t          what is being saved and exported: how far it is, what failed,
             what is saved; stop it or go on
  8          files on this computer (also: bbmp FILE or FOLDER)
  ?          help                  q  quit

  Shift gives the bigger or related thing: r radio, R repeat; l playlist, L
  the queue as one. Every key can be changed: "keys" in settings.json, by the
  names the README lists.

[b]Formats[/b]
  "★ New", first in the list of formats, has what the collection has added or
  changed, by day. The lists say how large things are: a collection, a format,
  an artist's songs, a song.
  In Browse, a format with ✓ was compared with the original replayer and is the
  same; one with ! plays with faults or gaps; one with + needs a file; a
  struck-out one doesn't play. The line under the lists says what is known, and
  the Info tab says it for the song that is playing.
  A song marked ! is known not to play, in a format that otherwise does: the
  census found it, or it failed here. Radio passes it over; played on purpose it
  is tried again, and it loses its mark once it plays.

[b]Downloads and extra files[/b]
  Some formats use a file of the program their songs were made with. The player
  doesn't come with those, and downloads them from their makers when you say so:
  "Downloads" in Settings.
  Others use a ROM or a set of samples that the player can't download for you.
  "Extra files" in Settings lists them: what each is, its name and size, and the
  folder to put it into.
"""
