#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write what the catalogs on this computer know of songs' chips into
src/bbmp/data/chips.xz, which comes with the program (known_chips.py).

    tools/make_chips.py

Run tools/sid_chips.py and tools/song_chips.py first: they read the files and note
the chips in the catalogs.
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from bbmp import index as index_mod, known_chips, paths, units  # noqa: E402


def main() -> int:
    n = known_chips.write(known_chips.HERE, index_mod._catalogs(paths.CATALOG_DIR))
    print(f"{known_chips.HERE}: {n:,} songs' chips, {units.size(known_chips.HERE.stat().st_size)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
