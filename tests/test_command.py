# SPDX-License-Identifier: GPL-3.0-or-later
"""The bbmp command a Flatpak install makes on request (command.py)."""

import os

from bbmp import command


def _home(tmp_path, monkeypatch, flatpak=True):
    monkeypatch.setenv("HOME", str(tmp_path))
    if flatpak:
        monkeypatch.setenv("FLATPAK_ID", command.APP_ID)
    else:
        monkeypatch.delenv("FLATPAK_ID", raising=False)
    return tmp_path / ".local" / "bin" / "bbmp"


def test_it_is_offered_once_in_a_flatpak_only(tmp_path, monkeypatch):
    _home(tmp_path, monkeypatch, flatpak=True)
    assert command.should_offer(asked=False)
    assert not command.should_offer(asked=True)
    _home(tmp_path, monkeypatch, flatpak=False)
    if not os.path.exists("/.flatpak-info"):
        assert not command.should_offer(asked=False)


def test_in_a_flatpak_the_player_says_the_line_that_makes_the_command(tmp_path, monkeypatch):
    """A Flatpak may not write to ~/.local/bin (a way out of it): the user types one line."""
    import subprocess
    path = _home(tmp_path, monkeypatch)
    said = command.make()
    assert not path.exists() and command.line() in said
    # the line, typed in a shell, makes the script, which starts the Flatpak
    subprocess.run(["sh", "-c", command.line()], check=True, env={**os.environ, "HOME": str(tmp_path)})
    assert os.access(path, os.X_OK) and command.ours(path)
    text = path.read_text()
    assert text.startswith("#!/bin/sh\n")
    assert f'exec flatpak run {command.APP_ID} "$@"' in text


def test_a_bbmp_that_isnt_ours_is_left_alone(tmp_path, monkeypatch):
    path = _home(tmp_path, monkeypatch, flatpak=False)
    monkeypatch.setattr(command, "in_flatpak", lambda: False)
    monkeypatch.setenv("APPIMAGE", "/x/bbmp.AppImage")
    path.parent.mkdir(parents=True)
    path.write_text("#!/bin/sh\necho mine\n")
    said = command.make()
    assert path.read_text() == "#!/bin/sh\necho mine\n"
    assert "left alone" in said


def test_in_an_appimage_the_command_starts_the_appimage(tmp_path, monkeypatch):
    from bbmp import command
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.delenv("FLATPAK_ID", raising=False)
    monkeypatch.setattr(command, "in_flatpak", lambda: False)
    monkeypatch.setenv("APPIMAGE", "/home/someone/Downloads/bbmp-x86_64.AppImage")
    assert command.should_offer(False)
    said = command.make()
    script = (tmp_path / ".local/bin/bbmp").read_text()
    assert 'exec /home/someone/Downloads/bbmp-x86_64.AppImage "$@"' in script
    assert command.ours(tmp_path / ".local/bin/bbmp") and "move that file" in said


def test_an_appimage_name_is_quoted_for_the_shell(tmp_path, monkeypatch):
    import subprocess
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.delenv("FLATPAK_ID", raising=False)
    monkeypatch.setattr(command, "in_flatpak", lambda: False)
    odd = tmp_path / 'it\'s "$(touch pwned)" `touch pwned2`.AppImage'
    odd.write_text("#!/bin/sh\necho started \"$@\"\n")
    odd.chmod(0o755)
    monkeypatch.setenv("APPIMAGE", str(odd))
    command.make()
    done = subprocess.run([str(tmp_path / ".local/bin/bbmp"), "a b"], capture_output=True,
                          text=True, cwd=tmp_path)
    assert done.stdout.strip() == "started a b"
    assert not (tmp_path / "pwned").exists() and not (tmp_path / "pwned2").exists()


def test_a_broken_link_there_is_not_followed(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.setattr(command, "in_flatpak", lambda: False)
    monkeypatch.setenv("APPIMAGE", "/x/bbmp.AppImage")
    path = tmp_path / ".local/bin/bbmp"
    path.parent.mkdir(parents=True)
    path.symlink_to(tmp_path / "elsewhere")
    said = command.make()
    assert "left alone" in said and not (tmp_path / "elsewhere").exists()
