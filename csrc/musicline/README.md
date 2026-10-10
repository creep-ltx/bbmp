<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# bbmp-musicline: where its code comes from

- **The replay routine** (`mline.h`, `mline_load.cpp`, `mline_play.cpp`) is a port of the
  original Amiga Musicline Editor replayer, Copyright (C) John Carehag; its authors include
  Conny Cyreus, Christian Cyreus, John Carehag and Jimmy Fredriksson. They released it
  under the GNU Lesser General Public License (mlineplayer102.asm in UADE,
  amigasrc/players/other/musicline_editor). The licence, version 2.1 as UADE carries it,
  is in [COPYING.LGPL](COPYING.LGPL).
- **NostalgicPlayer's C# port** of the same routine, Copyright (c) 2023 Thomas Neumann,
  MIT licence, helped with the port. `gen_tables.py` turns its tables into a header at build
  time. Its licence is in [LICENSE.NostalgicPlayer](LICENSE.NostalgicPlayer).
- **The rest** (`bbmp-musicline.cpp`, `gen_tables.py`) is written for bbmp, GPL-3.0-or-later.
