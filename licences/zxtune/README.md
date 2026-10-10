<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# ZXTune and what it brings along, as bbmp-zxtune and bbmp-ay-vt2 are built

ZXTune by Vitamin (vitamin-caig, https://github.com/vitamin-caig/zxtune, commit e36902f)
is under the GNU Lesser General Public License version 3 ([LGPL-3.0.md](LGPL-3.0.md);
the GPL 3 it builds on is [COPYING](../../COPYING)). `bbmp-zxtune` and `bbmp-ay-vt2` are
programs built inside ZXTune's tree with its libraries and the libraries below from its
`3rdparty` folder. Two of those, Highly Experimental and Highly Theoretical, are GPL 3, so
the programs as a whole are under the GPL version 3 (the player's own files in them,
`csrc/zxtune/main.cpp` and `csrc/ay/vt2/main.cpp`, are GPL-3.0-or-later).

What of `3rdparty` is in the programs was read from their debug information (the source
files compiled into them), 7 October 2026. UnRAR and vgmstream are not in them
(`csrc/zxtune/no-rar-vgmstream.patch`), nor Z80~Ex, which is GPL 2 only and only the AY
Emul player used (`csrc/zxtune/no-z80ex.patch`), nor libvgm, whose OPL4 is GPL 2 only and
some of whose chips state no licence (`csrc/zxtune/no-libvgm.patch`; VGM and S98 are played
by `bbmp-vgm`, a program of its own: see [../README.md](../README.md)), nor atrac9, mpg123,
curl, lame, minimp3; zlib is the system's. Nor the trimmed image of Sony's PlayStation 2
BIOS that ZXTune builds into Highly Experimental (`scph10000_he.inc`): it is left out
(`csrc/zxtune/no-ps2-bios.patch`), and the user provides it in the extras folder.

| Part | Licence | Text |
|---|---|---|
| ZXTune itself | LGPL 3 | [LGPL-3.0.md](LGPL-3.0.md) |
| Highly Experimental, Highly Theoretical (Neill Corlett, kode54's copies) | GPL 3 | [COPYING](../../COPYING) |
| mGBA (Jeffrey Pfau) | MPL 2.0 | [mgba-MPL-2.0.txt](mgba-MPL-2.0.txt) |
| FFmpeg's libavcodec and libavutil, the audio decoders only (built without its GPL parts) | LGPL 2.1 or later | [LGPL-2.1.txt](../LGPL-2.1.txt) |
| Game Music Emu (kode54's fork) | LGPL 2.1 or later; some of its sound chips MAME's, GPL 2 or later | [LGPL-2.1.txt](../LGPL-2.1.txt), [GPL-2.0.txt](../GPL-2.0.txt) |
| snes_spc (Shay Green) | LGPL 2.1 or later | [LGPL-2.1.txt](../LGPL-2.1.txt) |
| libsidplayfp and reSIDfp | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| ASAP (Piotr Fusik) | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| lazyusf2 (Mupen64plus) | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| vio2sf (DeSmuME) | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| libopenmpt (OpenMPT Project) | BSD 3-clause | [openmpt.txt](openmpt.txt) |
| HivelyTracker's replayer (Pete Gordon) | BSD 3-clause | [hvl.txt](hvl.txt) |
| libxmp (Claudio Matsuoka and Hipolito Carraro Jr; ZXTune's copy) | MIT | [xmp.txt](xmp.txt) |
| libFLAC (Xiph.Org) | BSD 3-clause | [flac.txt](flac.txt) |
| libogg, libvorbis (Xiph.Org) | BSD 3-clause | [ogg.txt](ogg.txt), [vorbis.txt](vorbis.txt) |
| Opus | BSD 3-clause | [opus.txt](opus.txt) |
| lhasa (Simon Howard) | ISC | [lhasa.txt](lhasa.txt) |
| LZMA SDK (Igor Pavlov) | public domain | (none needed) |
| SSEQPlayer | WTFPL | [sseqplayer.txt](sseqplayer.txt) |
| V2 synthesizer replayer (Farbrausch) | Artistic License 2.0 | [v2m-Artistic-2.0.txt](v2m-Artistic-2.0.txt) |
| {fmt} (Victor Zverovich) | MIT | [fmt.txt](fmt.txt) |
| Boost's headers (only headers are used) | Boost Software License 1.0 | (needed for source copies only) |
