# SPDX-License-Identifier: GPL-3.0-or-later
"""Media keys and the desktop's now-playing widget (MPRIS), on a private D-Bus session."""

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

SCRIPT = Path(__file__).with_name("mpris_session.py")


def test_mpris_controls_the_player():
    if not (shutil.which("dbus-run-session") and shutil.which("gdbus")):
        pytest.skip("needs dbus-run-session and gdbus")
    pytest.importorskip("gi")
    out = subprocess.run(["dbus-run-session", "--", sys.executable, str(SCRIPT)],
                         capture_output=True, text=True, timeout=120).stdout
    lines = dict(line.split(" ", 1) for line in out.splitlines() if " " in line)
    assert "done" in out, out
    for check in ("started", "owned", "playing", "length", "metadata", "paused", "resumed", "next",
                  "previous", "seek", "seeked", "openuri", "volume", "loop", "stopped"):
        assert lines.get(check) == "True", f"{check}: {lines.get(check)}\n{out}"
