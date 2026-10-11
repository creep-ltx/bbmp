# SPDX-License-Identifier: GPL-3.0-or-later
"""Files that belong to a song rather than being songs themselves.

A TFMX song is two files ("mdat.x" and "smpl.x"), a PlayStation song needs its
game's library, an IFF-SMUS song its instruments folder. These files are hidden from
lists and downloaded together with the song that needs them. The rules are about the
formats, so they hold for every collection and for files on this computer.
"""

from __future__ import annotations

from typing import Iterable

# Modland's formats whose second folder level is a sub-format or system rather than an
# artist. Here, where nothing is imported, so that a replayer's process can know it
# without the collections' downloading code (collection/modland.py uses it too).
SUBFORMAT_DIRS = {"Video Game Music", "Ad Lib", "Spectrum"}

PREFIXES = ("smpl.", "smp.")
# by format family: Mark Cooksey's "mcr.x" plays with the samples in "mcs.x"
FORMAT_PREFIXES = {"Mark Cooksey Old": ("mcs.",)}
DIRS = {"instruments", "instr", "samples"}
JUNK_EXTS = {"txt", "m3u", "nfo", "diz", "doc", "info", "pdf", "jpg", "png", "gif"}
LIB_EXTS = {"psflib", "psf2lib", "2sflib", "ssflib", "dsflib", "usflib", "gsflib",
            "qsflib", "snsflib", "ncsflib"}
# Shared by every song in their folder: MDX and PC-98 sample banks, AdLib instrument
# banks and patches.
FOLDER_EXTS = LIB_EXTS | {"pdx", "bnk", "003", "pvi", "ppz", "pzi", "ppc", "pps", "p86",
                          "mbk", "fmb", "pmb"}
# by format family (the part of a format's name before " - ")
FORMAT_EXTS = {
    "Richard Joseph": {"ins"},
    "Infogrames": {"ins"},
    "Stereo Sidplayer": {"str"},
    "Quartet ST": {"set"},
    "MoonBlaster": {"mbk"},
    "MoonBlaster (edit mode)": {"mbk"},
    "Stonetracker": {"sps"},
    "FAC SoundTracker": {"sm1", "sm2"},
    "MDX": {"pdx"},
    "Ad Lib": {"agd", "bnk", "003", "ins", "s01"},     # s01: an EdLib D01 song's samples
    "Paul Robotham": {"ssd"},       # "song.dat" plays with "song.ssd", else "mdtest.ssd"
    "MusicMaker V8 Old": {"ip", "l", "n"},     # see NAMED_AFTER
    # each .670 is an older layout of the .c67 next to it, which plays (no replayer
    # here reads .670)
    "Composer 670 (CDFM)": {"670"},
}


# The PSF family: a song names its libraries inside itself (psf.py), and a library can
# have no extension at all ("abe_drv", "driver").
SF_FAMILIES = {"Playstation Sound Format", "Playstation 2 Sound Format", "Saturn Sound Format",
               "Dreamcast Sound Format", "Capcom Q-Sound Format", "Gameboy Sound Format",
               "Nintendo DS Sound Format", "Super Nintendo Sound Format",
               "Ultra64 Sound Format"}


# Formats whose songs may need any of the format's companion files, in any folder:
# a YMST song names the replay routine it runs on ("YM.Toki_replay"), and Modland
# keeps the routines in the folders of whoever wrote them, often under another name
# ("toki - replay.ymst"). All 57 of them are 370 KB.
FORMAT_WIDE = {"YMST"}


def is_replay(family: str, name: str) -> bool:
    """A YMST replay routine rather than a song."""
    return family == "YMST" and "replay" in name.lower()


def ext(name: str) -> str:
    return name.rsplit(".", 1)[1].lower() if "." in name else ""


def is_companion(family: str, name: str, folders: Iterable[str] = ()) -> bool:
    """`folders` are the ones between the artist's folder and the file."""
    low = name.lower()
    e = ext(low)
    if low.startswith(PREFIXES + FORMAT_PREFIXES.get(family, ())):
        return True
    if e in FOLDER_EXTS or e in JUNK_EXTS:
        return True
    if any(f.lower() in DIRS for f in folders):
        return True
    if low in FORMAT_FOLDER_FILES.get(family, ()):
        return True
    if family in SF_FAMILIES and not e:
        return True
    if is_replay(family, name):
        return True
    return e in FORMAT_EXTS.get(family, ())


def stems(name: str) -> set[str]:
    """Names a song and its companions share: 'mdat.x' / 'smpl.x', 'a.sng' / 'a.ins'."""
    low = name.lower()
    out = set()
    if "." in low:
        pre, rest = low.split(".", 1)
        if len(pre) <= 5:
            out.add(rest)
        out.add(low.rsplit(".", 1)[0])
    return out or {low}


# Files that every song in their folder may use, in one format only. A FAC SoundTracker
# song names its drum kit inside the file ("DRUMS1" is DRUMS1.SM1 and DRUMS1.SM2, the
# two halves of the sample memory), and many songs share a kit.
# A Quartet ST song uses the set of samples of the same name, or else the folder's one
# set, SMP.set, which the songs of a disk share.
FORMAT_FOLDER_EXTS = {"FAC SoundTracker": {"sm1", "sm2"}, "Quartet ST": {"set"}}

# Files with a fixed name that every song in their folder needs: Ken Silverman's AdLib
# songs (.ksm) take their instruments from insts.dat, his digital ones (.kdm) their
# waves from waves.kwv.
# Pierre Adane's and Synth Pack's songs take their samples from the smp.set of their
# game's folder, and so do the Hippel ST COSO songs that keep no samples of their own
# (Wings of Death); Synth Dream's ("sdr.x") and Maximum Effect's ("MAX.x") do when they
# have no "smp.x" of their own.
# A Kris Hatlelid song runs on the replay code of its game, the file "songplay" next to
# it; a Paul Robotham song "x.dat" without an "x.ssd" takes its samples from "mdtest.ssd".
FORMAT_FOLDER_FILES = {"Ad Lib": {"insts.dat"}, "Ken's Digital Music": {"waves.kwv"},
                       "Pierre Adane Packer": {"smp.set"}, "Synth Pack": {"smp.set"},
                       "Synth Dream": {"smp.set"}, "Maximum Effect": {"smp.set"},
                       "Kris Hatlelid": {"songplay"}, "Paul Robotham": {"mdtest.ssd"},
                       "Hippel ST COSO": {"smp.set"}}

# Formats with a folder of samples for all their songs, next to the artists' folders:
# a Zoundmonitor song names its samples, and they are in Zoundmonitor/Samples.
SHARED_FOLDERS = {"Zoundmonitor": "Samples"}

# Formats whose songs may take their instruments from another song of their folder:
# MaxTrax, where a game's songs share the instruments one of its files has.
WHOLE_FOLDER = {"MaxTrax"}

# Formats whose songs have a file of the same name plus an extension next to them:
# a SoundSmith song "Breathless" has its instruments in the wave bank "Breathless.W".
# The song's own name can have dots in it ("FTA.Song"), so only a file that has its
# song next to it is a bank.
# A PokeyNoise song has the player's settings for it in its Amiga icon, "pn.song.info".
# A Startrekker AM song's synth instruments are in "song.mod.nt".
# An Audio Sculpture song's synth instruments are in "song.adsc.as".
BANKS = {"SoundSmith": ".w", "PokeyNoise": ".info", "Startrekker AM": ".nt",
         "Audio Sculpture": ".as"}


# Formats whose songs take their instruments from a file whose name begins theirs: an
# Infogrames song "bob4e.dum" plays with "bob4.ins" when it has no "bob4e.ins".
PREFIX_BANKS = {"Infogrames": "ins"}


def prefix_bank(song_name: str, candidates: Iterable[str], kind: str) -> str | None:
    """The file of extension `kind` whose name is the song's, or else the longest that
    begins the song's name."""
    stem = song_name.rsplit(".", 1)[0].lower()
    best = None
    for c in candidates:
        cstem, dot, cext = c.rpartition(".")
        if not dot or cext.lower() != kind or not stem.startswith(cstem.lower()):
            continue
        if best is None or len(cstem) > len(best.rpartition(".")[0]):
            best = c
    return best


def bank_of(family: str, name: str, siblings: Iterable[str]) -> bool:
    """Is `name` the bank of one of the files next to it (`siblings`)?"""
    suffix = BANKS.get(family)
    if not suffix or not name.lower().endswith(suffix):
        return False
    return name[:-len(suffix)] in siblings


# Formats whose songs come with track lists: one .m3u per track, in the song's folder,
# with the track's title and length. An SGC file has the sounds of a whole game in it,
# effects and all, and the lists say which of them are the music.
LISTED = {"SGC"}


# Formats whose songs have files named after them: an old MusicMaker V8 song "x.sdata"
# has its instruments in "x.ip" (and "x.ip.l", "x.ip.n").
NAMED_AFTER = {"MusicMaker V8 Old"}


def goes_with(song_name: str, other: str, family: str = "") -> bool:
    """Does `other`, a hidden file at or below the song's folder (given from there),
    belong to the song? `family` is the song's format family."""
    if family in LISTED and "/" not in other and ext(other) == "m3u":
        return True
    if family in NAMED_AFTER and "/" not in other:
        return other.lower().startswith(song_name.rsplit(".", 1)[0].lower() + ".")
    if family in BANKS:
        return other.lower() == song_name.lower() + BANKS[family]
    if family in PREFIX_BANKS and "/" not in other and ext(other) == PREFIX_BANKS[family]:
        return song_name.rsplit(".", 1)[0].lower().startswith(other.rsplit(".", 1)[0].lower())
    if "/" not in other and ext(other) in FORMAT_FOLDER_EXTS.get(family, ()):
        return True
    if other.lower() in FORMAT_FOLDER_FILES.get(family, ()):
        return True
    if family in SF_FAMILIES and "/" not in other and not ext(other):
        return True
    if "/" in other:
        # files in instrument folders below the song, e.g. Instruments/bass.instr
        return other.split("/", 1)[0].lower() in DIRS
    return ext(other) in FOLDER_EXTS or bool(stems(other) & stems(song_name))
