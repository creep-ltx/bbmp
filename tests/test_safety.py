# SPDX-License-Identifier: GPL-3.0-or-later
"""The safety fixes of 8 October 2026 (the final audit): the warning when no sandbox
works, and the helpers' limits (memory, no core dumps, no preexec_fn) and reaping."""

import subprocess
import sys

import pytest

from bbmp import sandbox
from bbmp.settings import Settings


# ---- no sandbox: said plainly ----

def test_no_sandbox_is_a_plain_warning(monkeypatch):
    monkeypatch.setattr(sandbox, "_check", lambda: ("", "bubblewrap (bwrap) is not installed"))
    monkeypatch.delenv("BBMP_SANDBOX", raising=False)
    text = sandbox.warning()
    assert "without a sandbox" in text and "bubblewrap" in text
    assert "bwrap) is not installed" in text            # and why
    monkeypatch.setattr(sandbox, "_check", lambda: ("bwrap", ""))
    assert sandbox.warning() == ""


def test_the_player_warns_once_at_start_when_no_sandbox_works(catalog, fetcher, tmp_path,
                                                              monkeypatch):
    from test_tui import make_app, run, settle
    monkeypatch.setattr(sandbox, "warning", lambda: "Warning: the replayers run without a sandbox")

    async def go():
        app = make_app(catalog, fetcher, tmp_path, settings=Settings(resume=False))
        async with app.run_test(size=(130, 44), notifications=True) as pilot:
            assert await settle(pilot, lambda: any("without a sandbox" in n.message
                                                   for n in app._notifications))
            await pilot.pause(0.3)
            assert sum("without a sandbox" in n.message for n in app._notifications) == 1
    run(go())


# ---- the helpers' limits ----

def test_a_helper_leaves_no_core_dump_and_is_started_without_preexec_fn(monkeypatch):
    seen = {}
    real = subprocess.Popen

    def popen(cmd, **kwargs):
        seen.update(kwargs)
        return real(cmd, **kwargs)
    monkeypatch.setattr(subprocess, "Popen", popen)
    p = sandbox.popen([sys.executable, "-c", "import resource; "
                       "print(*resource.getrlimit(resource.RLIMIT_CORE), "
                       "*resource.getrlimit(resource.RLIMIT_DATA))"],
                      stdout=subprocess.PIPE)
    out = p.communicate(timeout=30)[0].split()
    assert "preexec_fn" not in seen
    assert out[:2] == [b"0", b"0"]
    if sys.platform != "darwin":
        assert int(out[2]) == sandbox.MEMORY_LIMIT


def test_a_helper_that_is_not_there_is_an_error_at_once():
    with pytest.raises(FileNotFoundError):
        sandbox.popen(["/nonexistent/bbmp-helper"], stdout=subprocess.PIPE)


def test_a_killed_library_process_is_reaped():
    from bbmp.backends.remote import Remote
    r = Remote.__new__(Remote)
    r._proc = subprocess.Popen(["sleep", "30"])
    r._kill()
    assert r._proc.returncode is not None             # waited for: no zombie
