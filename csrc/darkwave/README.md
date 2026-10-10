<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# bbmp-darkwave: where its code comes from

DarkWave Studio 4.1.6, Copyright (C) 2004-2012 Daniel Werner
(http://www.experimentalscene.com/), was published with its source under the GNU General
Public License, version 3 or later. `tools/build-darkwave.sh` fetches that source (kept by
the Wayback Machine) and builds its engine with the windows taken out.

- `studio.cpp`, `studio.h`: the studio (loader, cables, patterns, sequence) ported from
  DarkWave Studio's own code, changed for bbmp in 2026. GPL 3 or later.
- `darkwave-4.1.6-headless.patch`: the changes made to DarkWave Studio's source. GPL 3 or later.
- `compat/`: headers that stand in for DarkWave Studio's windows and for Windows itself, so
  that the engine builds without them. GPL 3 or later.
- `bbmp-darkwave.cpp`: written for bbmp. GPL-3.0-or-later.

The licence is in [COPYING](../../COPYING) at the top of the repository.
