# SPDX-License-Identifier: GPL-3.0-or-later
"""The player's own files when they hold something unexpected, and background jobs
that can't close the player (8 October 2026)."""

import json
import types

import pytest

from bbmp.settings import Settings


@pytest.mark.parametrize("text", ["null", "[]", "3", '"x"'])
def test_settings_that_are_not_an_object_give_the_defaults(tmp_path, text):
    path = tmp_path / "settings.json"
    path.write_text(text)
    s = Settings.load(path)
    assert s.volume == Settings().volume and s.theme == Settings().theme


def test_settings_of_the_wrong_kind_keep_their_default(tmp_path):
    path = tmp_path / "settings.json"
    path.write_text(json.dumps({"volume": "loud", "shuffle": 1, "browse": None,
                                "keys": [], "visualizer": None, "collections": ["modland"],
                                "theme": "nord", "keep_speed": 3.0}))
    s = Settings.load(path)
    d = Settings()
    assert (s.volume, s.shuffle, s.browse, s.keys) == (d.volume, d.shuffle, d.browse, d.keys)
    assert s.visualizer is None and s.collections == ["modland"] and s.theme == "nord"
    assert s.keep_speed == 3 and isinstance(s.keep_speed, int)


def _app_like(tmp_path):
    from bbmp.tui.app import ModlandApp
    fake = types.SimpleNamespace(_session_path=tmp_path / "session.json", _resuming=None)
    fake.player = types.SimpleNamespace(session=lambda: {"queue": ["modland:a"], "pos": 0,
                                                         "subsong": 0, "seconds": 1.5})
    return ModlandApp, fake


def test_the_session_is_written_whole_or_not_at_all(tmp_path):
    App, fake = _app_like(tmp_path)
    App._save_session(fake)
    assert json.loads(fake._session_path.read_text())["queue"] == ["modland:a"]
    assert [p.name for p in tmp_path.iterdir()] == ["session.json"]      # no temp file left


@pytest.mark.parametrize("text", ["null", "[]", '{"queue": [1], "pos": 0}'])
def test_a_strange_session_is_not_resumed(tmp_path, text):
    App, fake = _app_like(tmp_path)
    fake._session_path.write_text(text)
    fake.index = types.SimpleNamespace(resolve=lambda key: pytest.fail("not resumed"))
    App._resume(fake)


def test_a_screens_background_job_is_guarded_too(tmp_path):
    from bbmp.tui.guard import guarded
    told = []
    app = types.SimpleNamespace(error_log=tmp_path / "errors.log",
                                notify=lambda text, **k: told.append(text),
                                call_from_thread=lambda f, *a, **k: f(*a, **k))

    class Screen:
        def __init__(self):
            self.app = app

        @guarded("reading what is being saved")
        def job(self):
            raise ValueError("a changed format")
    Screen().job()
    assert told and "a changed format" in told[0]
    assert "a changed format" in app.error_log.read_text()
