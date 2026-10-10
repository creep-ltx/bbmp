<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Furnace and the libraries built into it

Furnace by tildearrow and its contributors (https://github.com/tildearrow/furnace,
commit fa9541d) is a program of its own in the packages (`opt/furnace/bin/furnace`), which
`bbmp-furnace` runs without its windows. Most of it is under the GNU General Public
License version 2 or later ([LICENSE.md](LICENSE.md), [GPL-2.0.txt](../GPL-2.0.txt));
its sound chips (in `src/engine/platform/sound`) are under various licences, each stated
in its own files; the two that state none, QSound's (`qsound.c`) and the Watara Supervision's
(`supervision.c`, from Potator), are not built in (`csrc/furnace/no-unlicensed-cores.patch`,
8 October 2026): Furnace plays those two chips silent. Built without ASIO (which would make it GPL 3 only), SDL, PortAudio,
JACK, RtMidi and FreeType. The libraries from its `extern` folder linked into it
(7 October 2026, from its build's link line):

| Part | Licence | Text |
|---|---|---|
| Nuked-OPLL, Nuked-PSG, Nuked-OPL2-Lite (its LICENSE file is the LGPL 2.1, its source says GPL 2 or later), the YM2151, YM2203, YM2414, YM2608, YM3812, YMF262, YMF276 LLE cores (nukeykt) | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| FFTW 3 | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| itcompress (Schism Tracker's, Storlek and Mrs. Brisby) | GPL 2 or later | [GPL-2.0.txt](../GPL-2.0.txt) |
| ESFMu, Nuked-CQM, the opl, opm, opn cores | LGPL 2.1 or later | [LGPL-2.1.txt](../LGPL-2.1.txt) |
| libsndfile (modified) | LGPL 2.1 or later | [LGPL-2.1.txt](../LGPL-2.1.txt) |
| blip_buf (Shay Green) | LGPL 2.1 or later | [LGPL-2.1.txt](../LGPL-2.1.txt) |
| mpg123 | LGPL 2.1 | [mpg123.txt](mpg123.txt), [LGPL-2.1.txt](../LGPL-2.1.txt) |
| LAME | LGPL 2 | [lame-LICENSE.txt](lame-LICENSE.txt), [LGPL-2.0.txt](LGPL-2.0.txt) |
| libFLAC, libogg, libvorbis (Xiph.Org), Opus | BSD 3-clause | [flac.txt](flac.txt), [ogg.txt](ogg.txt), [vorbis.txt](vorbis.txt), [opus.txt](opus.txt) |
| adpcm-xq (David Bryant) | BSD 3-clause | [adpcm-xq.txt](adpcm-xq.txt) |
| SAASound (Dave Hooper, Simon Owen) | BSD 3-clause | [saasound.txt](saasound.txt) |
| emu2413 (Mitsutaka Okazaki) | MIT | [emu2413.txt](emu2413.txt) |
| klattsch-cpp (Tony Gies), pwrnoise (scratchminer) | MIT | [klattsch.txt](klattsch.txt), [pwrnoise.txt](pwrnoise.txt) |
| {fmt} | MIT | [fmt.txt](fmt.txt) |
| vgsound_emu (cam900) | zlib | [vgsound_emu.txt](vgsound_emu.txt) |
| zlib | zlib | [zlib.txt](zlib.txt) |
| adpcm | public domain (Unlicense) | [adpcm.txt](adpcm.txt) |
