#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""On macOS: run a few programs shut in as the replayers are, and say how each went.
With the system's log of what the sandbox refused, this shows what a profile lacks."""

import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "src"))
from bbmp import paths, sandbox  # noqa: E402

print("mode:", sandbox.mode() or "none", "-", sandbox.protection())
song = Path(tempfile.mkdtemp()) / "probe" / "song.mod"
song.parent.mkdir(parents=True)
song.write_bytes(b"probe" * 100)
tries = {"cat": ["/bin/cat", str(song)],
         "python": [sys.executable, "-c", "print('python runs')"],
         "network refused": ["/usr/bin/curl", "-sS", "-m", "5", "https://example.com"],
         "home refused": ["/bin/ls", str(Path.home())]}
for exe in sorted((paths.OPT).glob("bbmp-*/bin/bbmp-*"))[:6]:
    tries[exe.name] = [str(exe), str(song)]
for name, cmd in tries.items():
    done = subprocess.run(sandbox.wrap(cmd, song), capture_output=True, text=True, timeout=30)
    print(f"--- {name}: exit {done.returncode}")
    for line in (done.stdout.strip().splitlines()[:2] + done.stderr.strip().splitlines()[-3:]):
        print("   ", line[:200])
