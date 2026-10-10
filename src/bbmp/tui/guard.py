# SPDX-License-Identifier: GPL-3.0-or-later
"""Background jobs that can't close the player.

Textual closes the whole app when a background job (a worker) meets an error nobody
caught. A job wrapped in `guarded` instead writes what happened to the error log and
shows a plain message; the player goes on (7 October 2026).
"""

from __future__ import annotations

import functools
import time
import traceback
from pathlib import Path

from .. import paths


def log_error(app, what: str) -> Path:
    """Add the error being handled to the app's error log; the log's path."""
    log = Path(getattr(app, "error_log", None) or paths.ERROR_LOG)
    try:
        log.parent.mkdir(parents=True, exist_ok=True)
        with log.open("a", encoding="utf-8") as f:
            f.write(f"--- {time.strftime('%Y-%m-%d %H:%M:%S')} while {what}\n")
            f.write(traceback.format_exc())
    except OSError:
        pass
    return log


def guarded(what: str):
    """Wrap a background job (of the app or a screen, in a thread): an error in it is logged and
    told, "Something went wrong while <what>", instead of closing the player."""
    def wrap(job):
        @functools.wraps(job)
        def run(self, *args, **kwargs):
            try:
                return job(self, *args, **kwargs)
            except Exception as e:
                # a job of the app, or of one of its screens
                app = self if hasattr(self, "call_from_thread") else self.app
                log = log_error(app, what)
                text = (f"Something went wrong while {what}: {str(e) or type(e).__name__}. "
                        f"The details are in {paths.shown(log)}.")
                try:
                    app.call_from_thread(app.notify, text, severity="error", timeout=15)
                except RuntimeError:
                    pass            # the player is closing, or the job ran in its own thread
                return None
        return run
    return wrap
