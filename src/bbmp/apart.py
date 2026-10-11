# SPDX-License-Identifier: GPL-3.0-or-later
"""Work that takes much memory, done in a Python process of its own, so that the
memory goes back when it ends.

The catalog update read Modland's half a million files into Python objects in the
player's own process: 678 MB at its peak, and 282 MB of it kept for as long as the
player ran, since freed memory scattered among the player's other objects is not
given back (7 and 11 October 2026). Done in a process of its own, all of it goes.

The player starts `python -m bbmp.apart` and writes it what to do as a line of JSON:
the function ("module:name") and its arguments. The process calls it with a
`progress` that sends each message back as a line of JSON, and sends the answer, or
the error, last. Should the player end meanwhile, the process ends too.
"""

from __future__ import annotations

import importlib
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path
from typing import Callable


class Failed(RuntimeError):
    """The work raised something other than an OSError, as the process told it."""


class NotStarted(OSError):
    """The process could not be started (no Python to start it with)."""


def run(target: str, kwargs: dict, progress: Callable[[str], None] = lambda s: None):
    """Call target(progress=..., **kwargs) in a process of its own and give back what
    it gave (as JSON gives it back). An OSError in it is an OSError here; anything
    else, Failed. Raises OSError when the process can't be started or ends without
    an answer."""
    src = str(Path(__file__).resolve().parents[1])
    env = {**os.environ, "PYTHONPATH": os.pathsep.join(
        p for p in (src, os.environ.get("PYTHONPATH", "")) if p)}
    with tempfile.TemporaryFile() as err:
        try:
            proc = subprocess.Popen([sys.executable, "-m", "bbmp.apart"], stdin=subprocess.PIPE,
                                    stdout=subprocess.PIPE, stderr=err, env=env)
        except OSError as e:
            raise NotStarted(str(e)) from None
        answer = None
        try:
            proc.stdin.write((json.dumps({"target": target, "kwargs": kwargs}) + "\n").encode())
            proc.stdin.close()
            for line in proc.stdout:
                try:
                    said = json.loads(line)
                except ValueError:
                    continue
                if not isinstance(said, dict):
                    continue
                if "say" in said:
                    progress(str(said["say"]))
                elif "result" in said or "error" in said:
                    answer = said
        finally:
            proc.stdout.close()
            if proc.poll() is None and answer is None:
                proc.kill()
            proc.wait()
        if answer is None:
            err.seek(0)
            lines = err.read().decode(errors="replace").strip().splitlines()
            raise OSError(f"its process ended without an answer ({lines[-1] if lines else proc.returncode})")
    if "error" in answer:
        raise (OSError if answer.get("kind") == "OSError" else Failed)(str(answer["error"]))
    return answer["result"]


def _orphaned(parent: int) -> None:
    """End this process when the player that started it is gone."""
    while True:
        time.sleep(1)
        if os.getppid() != parent:
            os._exit(1)


def main() -> int:
    parent = os.getppid()
    threading.Thread(target=_orphaned, args=(parent,), daemon=True).start()
    # what the work prints can't break the answers: they go out on a copy of stdout
    out = os.fdopen(os.dup(1), "w")
    os.dup2(2, 1)
    sys.stdout = sys.stderr
    lock = threading.Lock()

    def send(message: dict) -> None:
        with lock:
            out.write(json.dumps(message) + "\n")
            out.flush()
    asked = json.loads(sys.stdin.readline())
    module, _, name = asked["target"].partition(":")
    try:
        work = getattr(importlib.import_module(module), name)
        result = work(progress=lambda s: send({"say": str(s)}), **asked["kwargs"])
    except OSError as e:
        send({"error": str(e) or type(e).__name__, "kind": "OSError"})
        return 1
    except Exception as e:
        send({"error": str(e) or type(e).__name__, "kind": type(e).__name__})
        return 1
    send({"result": result})
    return 0


if __name__ == "__main__":
    sys.exit(main())
