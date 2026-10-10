#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write what each replayer is made of, its versions and licences, for the player to show
(src/bbmp/data/replayers.json), and list what there is to look into about licences.

    tools/replayer_facts.py [path to bbmp-sources]      (default: bbmp-sources beside this repository)

The sources' versions and licences come from bbmp-sources/sources.json (bbmp-sources is
an archive of the exact source tarballs the build uses); what each replayer is made of is below.
"own" is code written for this player; its terms are the player's, or the terms of the
code it was ported from, which the note says.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parents[1]
OUT = HERE / "src/bbmp/data/replayers.json"

# engine -> (name shown, [sources in sources.json], what the player's own part is, terms
# where the sources don't say them or the whole has other terms than its parts)
ENGINES: dict[str, tuple[str, list[str], str, str]] = {
    "openmpt": ("libopenmpt", ["libopenmpt"], "", ""),
    "uade": ("UADE", ["uade", "bencodetools", "libzakalwe"], "", ""),
    "gme": ("Game Music Emu", ["libgme"], "one fault mended (PC Engine)", ""),
    "sidplayfp": ("libsidplayfp", ["libsidplayfp", "libresidfp"], "", ""),
    "sunvox": ("SunVox library", ["sunvox_lib"], "",
               "free to use, with its credit: Powered by SunVox (modular synth & tracker), "
               "Copyright (c) 2008 - 2026, Alexander Zolotov, WarmPlace.ru; "
               "its SunDog engine MIT, its libFLAC and Tremor BSD (licences/sunvox)"),
    "zxtune": ("ZXTune", ["zxtune"], "", ""),
    "vgm": ("bbmp-vgm", ["zxtune"], "the player's own VGM and S98 player (written from the formats' "
            "published documents), with libvgm's licensed sound chip cores (Valley Bell; "
            "ZXTune's sources carry them, 3rdparty/vgm)",
            "GPL-2.0 only (libvgm's OPL4 core, after openMSX; a program of its own, bbmp-vgm, "
            "whose own files are GPL 2 or later; none of libvgm's player or glue is in it)"),
    "ym": ("StSound", ["StSound"], "", ""),
    "sndh": ("psgplay", ["psgplay"], "",
             "GPL-2.0 only (psgplay's licence; a program of its own, bbmp-psgplay)"),
    "adplug": ("AdPlug", ["adplug", "libbinio"], "", ""),
    "mdx": ("mdxmini", ["mdxmini"], "", ""),
    "pmdc": ("PMD driver on ymfm", ["ymfm"],
             "written for this player from a description of PMD's behaviour (clean room); "
             "the YM2608 by ymfm", ""),
    "fmp": ("fmplayer", ["fmplayer"], "", ""),
    "kss": ("libkss", ["libkss"], "", ""),
    "eup": ("EUP player", ["eupplayer"], "", ""),
    "organya": ("organya.h", ["organya.h"], "", ""),
    "pxtone": ("pxtone", ["libpxtone"], "", ""),
    "qsf": ("Highly Quixotic", ["deadbeef_hq"], "", ""),
    "snsf": ("snsf9x", ["snsf9x"], "",
             "non-commercial use only (Snes9x's licence; a program of its own, bbmp-snsf, "
             "with no GPL code in it)"),
    "sc68": ("sc68", ["sc68"], "", ""),
    "furnace": ("Furnace", ["furnace"], "a helper that streams Furnace's rendering", ""),
    "xmp": ("libxmp", ["libxmp"], "", ""),
    "ixalance": ("iXalance replayer", [],
                 "written for this player from a description of the format and of how the earlier "
                 "player sounds (clean room); the three drum samples a few songs use are fetched "
                 "separately", "MIT (in bbmp-rv, with no GPL code in it)"),
    "klystrack": ("klystrack (Retrovert)", ["retrovert-playback-klystrack"], "", ""),
    "musicline": ("Musicline Player, ported", ["nostalgicplayer-musicline"],
                  "a C++ port of the original Amiga routine (LGPL, as in UADE's sources)",
                  "LGPL (a derived work of the original routine)"),
    "fac": ("FAC SoundTracker replayer", ["emu8950", "emu2413"],
            "written for this player in a clean room, from a description of the format only; "
            "the same chip writes as before in all 644 songs", ""),
    "psycle": ("Psycle", ["psycle"], "", "GPL 2 or later; some plugins have terms of their own"),
    "darkwave": ("DarkWave Studio 4.1.6, ported", [],
                 "its studio (project loader, cables, patterns, sequence) ported to C++ without "
                 "its windows; its machines (ES-CoreMachines) built from its source with the "
                 "windows taken out; no VST plug-ins",
                 "GPL 3 or later (DarkWave Studio's source, Daniel Werner 2004-2012)"),
    "soundsmith": ("SoundSmith replayer", [],
                   "written for this player in a clean room, from a description of the formats only, "
                   "with MTPng's effect rules for MegaTracker; the same chip writes as before in every song",
                   "GPL 3 or later (MTPng's rules: GPL 2 or later)"),
    "noisetracker": ("Noise Tracker GS replayer", [],
                     "written for this player in a clean room, from a description of the format only; "
                     "the same chip writes as before in every song", "GPL 3 or later"),
    "sgc": ("libnezplug", ["libnezplug"], "a patch to find a song's end", ""),
    "gbr": ("libnezplug", ["libnezplug"], "a patch to find a song's end", ""),
    "wsr": ("Mednafen's WonderSwan (via rePlayer)", ["rePlayer"], "",
            "GPL-2.0 only (four of Mednafen's WonderSwan files; a program of its own, bbmp-console, "
            "whose own files are GPL 2 or later)"),
    "spu": ("P.E.Op.S. SPU (Audio Overload SDK)", ["aosdk"], "a player for the log",
            "GPL 2 or later; in bbmp-console, a program of its own under the GPL-2.0 only "
            "(Mednafen's WonderSwan files are GPL 2 only)"),
    "edlib": ("AdPlug, with EdLib unpacked", ["adplug", "libbinio", "zlib-blast"],
              "a converter to D00, written for this player in a clean room, from a description "
              "of the format only; the same D00 files as before in every song", ""),
    "pixelpainters": ("Pixel Painters player", ["adplug"],
                      "written for this player in a clean room, from a description of the formats only; "
                      "checked against the music routines of Electranoid and Xatax in every song", ""),
    "d01": ("EdLib D01 player", ["adplug"],
            "written for this player in a clean room, from a description of the format only; "
            "checked against JCH's player 1.03 with samples (SBL-0103.P01) in every song", ""),
    "lem": ("Lemmings' Ad Lib player", ["adplug"],
            "written for this player in a clean room, from a description of the format only; "
            "checked against the Sound Images driver of 1991 in Lemmings' ADLIB.DAT in every song", ""),
    "fmtracker": ("FM Tracker player", ["adplug"],
                  "written for this player in a clean room, from a description of the format only; "
                  "checked against FM Tracker Player 1.26 (FMTP.EXE, Sagitta Software) in every song", ""),
    "samurai": ("First Samurai's Ad Lib player", ["adplug"],
                "written for this player in a clean room, from a description of the format only; "
                "the same OPL writes as the original in all songs", ""),
    "shadowlands": ("Shadowlands' Ad Lib player", ["adplug"],
                    "written for this player in a clean room, from a description of the format only; "
                    "the same OPL writes as the original in all songs", ""),
    "vibrants": ("JO's Ad Lib player (Vibrants)", ["adplug"],
                 "written for this player in a clean room, from a description of the format only; "
                 "the same OPL writes as the original in all songs", ""),
    "sig": ("Sound Images Generation 2 player", ["adplug"],
            "written for this player in a clean room, from a description of the format only; "
            "checked against Tony Williams' driver (in Kwik Snax for DOS) in every song", ""),
    "goattracker": ("GoatTracker's play routines",
                    ["GoatTracker_2.77", "GoatTracker_1.53", "GoatTracker_1.53_Stereo"], "", ""),
    "soundclub": ("Sound Club replayer", ["SoundClub"],
                  "written for this player in a clean room, from a description of the formats only; "
                  "the same sound as before in every song; the instrument bank and FM waveforms are "
                  "Sound Club's", ""),
    "monotone": ("MONOTONE's player, ported", [],
                 "a C++ port of MONOTONE's player and PC speaker output",
                 "public domain (MONOTONE is under The Unlicense)"),
    "digitrekker": ("DigiTrekker replayer", [],
                    "written for this player from DigiTrekker's format document", ""),
    "protrekkr": ("ProTrekkr (via rePlayer)", ["rePlayer"], "", "BSD 2-clause"),
    "kdm": ("Ken's Digital Music player", [],
            "written for this player from a description of the format's behaviour (clean room)",
            ""),
    "buz": ("Buzzic player", [],
            "written for this player from a description of the formats' behaviour (clean room)",
            ""),
    "jxs": ("JayTrax player", [],
            "written for this player from a description of the format's behaviour (clean room)",
            ""),
    "sbstudio": ("libpac (via rePlayer)", ["rePlayer"], "", "ISC"),
    "skale": ("OpenMPT with a Skale loader", ["openmpt-skm", "skale-skm-loader-openmpt"], "", ""),
    "unitrk": ("libmikmod", ["mikmod"], "", "LGPL 2.1"),
    "playerpro": ("PlayerPRO's driver", ["PlayerPRO"], "", ""),
    "dsm": ("libopenmpt, the song rewritten", ["libopenmpt"], "a converter to DSIK", ""),
    "mxm": ("libopenmpt, the song rewritten", ["libopenmpt"], "a converter to XM", ""),
    "buzz": ("BmxPlay", ["bmxplay"], "", "public domain"),
    "maxtrax": ("ScummVM's MaxTrax player", ["scummvm-maxtrax"], "one fault mended", ""),
    "zoundmonitor": ("UADE", ["uade"], "puts the samples where the player looks", ""),
    "jesperolsen": ("Jesper Olsen driver on ScummVM's Paula", ["scummvm-maxtrax", "uade"],
                    "written for this player from a description of the games' drivers (clean "
                    "room); songs with their own routine play through UADE", ""),
    "stonetracker": ("StoneTracker's replay routine, on UADE", ["uade", "stonefree2"], "",
                     "the routine: free to pass on with its whole archive and readme, which the "
                     "package carries (share/stonetracker); not open source"),
    "sccmusixx": ("SCC-Musixx replayer on emu2212", ["emu2212"],
                  "written for this player from a description of the tracker's replay "
                  "behaviour (clean room)", ""),
    "ned": ("NerdTracker 2's routine, on Game Music Emu", ["nedplay", "cc65", "libgme"], "",
            "the routine: giftware, free for any non-commercial use (NerdTracker 2's home page)"),
    "mgb": ("GBSoundSystem, on Game Music Emu", ["GBSoundSystem", "rgbds", "libgme"],
            "the song compiler, written for this player in a clean room, from a description of "
            "the format only; the same GBS image as before for every song", ""),
    "cybertracker": ("CyberTracker itself, emulated", ["cybertracker", "libresidfp"],
                     "a 6502 emulation that runs it", "the tracker: no terms stated"),
    "tss": ("TSS, ported", ["tss"], "a port of toyoshim's TSS", ""),
    "victracker": ("VIC-TRACKER's player, emulated", ["victracker", "dasm"],
                   "a 6502 and VIC sound emulation that run it", ""),
    "boyscout": ("BoyScout player", [],
                 "written after BoyScout's GBA playback library, with a GBA sound unit",
                 "the library: free to use in any project"),
    "vgemm": ("VGM Music Maker player, on Game Music Emu", ["libgme"],
              "written after the editor's VGM export, observed write for write", ""),
    "wwsnd": ("Westwood SND player", [], "written after ScummVM's PC speaker driver",
              "GPL 3 or later"),
    "onyx": ("libopenmpt, the song rewritten", ["libopenmpt"],
             "a converter to a module, written for this player in a clean room, from a description "
             "of the format only; the same module and sound as before in every song", ""),
    "picatune": ("Picatune player", [],
                 "written for this player in a clean room, from a description of the format only; "
                 "the same sound as before, bit for bit, in every song; checked against the "
                 "programs Picatune exports", ""),
    "shroom": ("Mario Paint player", [], "made from Mariopants 1.1 by Brad Smith, changed",
               "Mariopants: reuse allowed, credit and changes marked"),
    "beepola": ("Beepola's eight beeper engines", [],
                "written for this player in a clean room, from a description of the engines' "
                "behaviour only; the same sound as before, sample for sample, in every song; "
                "three pieces of data (the 48K ROM, Shiru's drum samples, 136 bytes of Savage) "
                "are downloaded from Beepola's repository on request, not shipped", ""),
    "strc": ("Sound Tracker (Raxoft compiler) player", ["ayumi"],
             "written for this player in a clean room, from a description of the format only; "
             "the same registers and sound as before", ""),
    "fls": ("Flash Tracker player, ported", ["ayumi"], "a port of Ay_Emul's player",
            "Ay_Emul: free to use with reference to its author"),
    "fxm": ("Fuxoft AY Language, ported", ["ayumi"], "a port of Ay_Emul's interpreter",
            "Ay_Emul: free to use with reference to its author"),
    "amad": ("AY Amadeus, ported", ["ayumi"], "a port of Ay_Emul's interpreter",
             "Ay_Emul: free to use with reference to its author"),
    "sks": ("Arkos Tracker 3's player", ["arkostracker3", "JUCE"], "", ""),
    "aks": ("Arkos Tracker 3's player", ["arkostracker3", "JUCE"], "", ""),
    "sndfile": ("libsndfile, with libsamplerate", [], "",
                "the system's libraries: libsndfile LGPL 2.1, libsamplerate BSD 2-clause"),
    "piece": ("P/ECE music driver replayer", [],
              "written for this player from the P/ECE SDK's format documents and measurements "
              "of the driver's sound; the drums are made anew", ""),
    "vt2": ("ZXTune, the song rewritten", ["zxtune"], "the text rewritten as ZXTune reads it", ""),
    "chipnsfx": ("CHIPNSFX's player", [],
                 "the tracker's own code (CHIPNSFX.C, César Nicolás-González, 2024-12-31 build, "
                 "kept unchanged in csrc/chipnsfx) without its user interface",
                 "GPL-3.0-or-later (CHIPNSFX's own licence)"),
    "tiatracker": ("TIATracker's player", [],
                   "the tracker's own song and replay code (Andre Wichmann's TIATracker, the copy "
                   "in github.com/dionoid/tiatracker, commit f523802) with Stella's TIA sound, kept "
                   "in csrc/tiatracker, three accessors added to player.h (marked); Qt replaced by small stand-ins "
                   "written for this player",
                   "GPL-2.0 (TIATracker's own licence; a separate program)"),
    "synder": ("Synder SNG replayer", [],
               "written for this player from a description of PlaySNG's behaviour (clean room)", ""),
    "t0ast": ("T0AST replayer", [],
              "written for this player from a description of the tracker's behaviour (clean room)", ""),
    "berotracker": ("BeRoTracker replayer", [],
                    "written for this player from a description of the tracker's behaviour (clean room)", ""),
    "axs": ("AXS replayer", [],
            "written for this player from a description of how AXS's Winamp plug-in sounds (clean room)", ""),
    "cym": ("bbmp-vgm, the log made into VGM", ["zxtune"], "the CYM log written as a VGM file, "
            "played by bbmp-vgm (the player's own VGM player, libvgm's sound chip cores)",
            "GPL-2.0 only (libvgm's OPL4 core; a program of its own, bbmp-vgm)"),
    "polly": ("Polly Tracker's player on libsidplayfp", ["libsidplayfp", "libresidfp"],
              "the module put behind Polly Tracker's C64 player, which is taken from HVSC's "
              "Polly_Rock.sid when it is played (it is not part of the player)",
              ""),
}


# the player's own code (decided 1 October 2026; the text is COPYING)
OWN_LICENCE = "GPL-3.0-or-later (the player's own code)"


def main() -> int:
    sources_dir = Path(sys.argv[1] if len(sys.argv) > 1 else
                       Path(__file__).resolve().parents[2] / "bbmp-sources")
    sources = {s["name"]: s for s in json.loads((sources_dir / "sources.json").read_text())}
    out, questions = {}, []
    for engine, (name, parts, own, terms) in sorted(ENGINES.items()):
        made = []
        for part in parts:
            s = sources.get(part)
            if s is None:
                questions.append(f"{engine}: {part} is not in sources.json")
                continue
            licence = s.get("licence") or s.get("licence_found") or ""
            if (not licence or "none found" in licence or "state none" in licence) and not terms:
                questions.append(f"{engine}: no licence found in {part}")
            made.append({"name": part, "version": s.get("version") or "",
                         "url": s.get("url") or "", "licence": licence})
        if not parts and not terms:
            terms = OWN_LICENCE          # written for the player: the player's own licence
        out[engine] = {"name": name, "own": own, "terms": terms, "made_of": made}
    OUT.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
    print(f"wrote {len(out)} replayers to {OUT.relative_to(HERE)}")
    if questions:
        print("\nTo look into:")
        print("\n".join(f"  {q}" for q in questions))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
