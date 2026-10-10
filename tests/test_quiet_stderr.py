# SPDX-License-Identifier: GPL-3.0-or-later
"""The screen goes to the terminal, other programs' chatter to the log (the Flatpak
showed nothing at all: Textual draws on sys.__stderr__, which went into the log)."""

import os
import sys

from bbmp.tui import app as tui_app


def test_the_screen_stays_on_the_terminal_and_descriptor_2_goes_to_the_log(tmp_path, monkeypatch):
    log = tmp_path / "bbmp.log"
    monkeypatch.setattr(tui_app, "log_path", lambda: log)
    monkeypatch.setattr(tui_app.paths, "CACHE_DIR", tmp_path)
    read, write = os.pipe()
    saved = os.dup(2)
    os.dup2(write, 2)                      # the "terminal" is this pipe
    real = sys.__stderr__
    try:
        with tui_app.quiet_stderr():
            os.write(2, b"uade says hello\n")          # a helper's chatter
            sys.__stderr__.write("the screen\n")       # where Textual draws
            sys.__stderr__.flush()
        assert sys.__stderr__ is real
    finally:
        os.dup2(saved, 2)
        os.close(saved)
        os.close(write)
    terminal = os.read(read, 4096)
    os.close(read)
    assert terminal == b"the screen\n"
    assert log.read_bytes() == b"uade says hello\n"
