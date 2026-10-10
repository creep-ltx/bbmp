# SPDX-License-Identifier: GPL-3.0-or-later
"""The page of what is being saved and exported: how far it has come, what failed,
and what is saved. t opens it, Esc closes it."""

from __future__ import annotations

import time
from dataclasses import dataclass, field

from rich.text import Text
from textual import on, work
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Vertical, VerticalScroll
from textual.screen import ModalScreen
from textual.widgets import OptionList, Static
from textual.widgets.option_list import Option

from .. import theme, units
from .guard import guarded


@dataclass
class ExportJob:
    """Songs being exported, or the last ones that were."""
    name: str               # of the folder they go to; "" for one song
    of: int
    done: int = 0
    song: str = ""          # the one being exported now
    written: int = 0        # files
    failed: list = field(default_factory=list)      # (song, why)
    folder: str = ""
    running: bool = True
    stopped: bool = False
    began: float = field(default_factory=time.monotonic)

    @property
    def left(self) -> str:
        took = time.monotonic() - self.began
        if not self.running or self.done < 1 or took < 5:
            return ""
        minutes = (self.of - self.done) * took / self.done / 60
        return "under a minute" if minutes < 1 else f"{round(minutes)} min"


class Activity(ModalScreen):
    """What is being saved and exported."""

    DEFAULT_CSS = """
    Activity { align: center middle; }
    Activity > Vertical { width: 96; max-width: 98%; height: 90%; padding: 1 2;
                          border: round $accent; background: $panel; }
    Activity VerticalScroll { height: 1fr; }
    Activity OptionList { height: auto; max-height: 12; border: none; margin-top: 1; }
    Activity .hint { color: $text-muted; margin-top: 1; }
    """
    BINDINGS = [Binding("escape,t", "close", show=False)]

    def __init__(self):
        super().__init__()
        self._states: list = []
        self._actions: list[tuple[str, str]] = []
        self._checked = False       # whether the files were looked at, on opening
        self._said = Text()         # what the page shows
        self._said_lines = None     # and in how many lines, how wide

    def compose(self) -> ComposeResult:
        with Vertical():
            with VerticalScroll():
                yield Static(id="activity-body", markup=False)
            yield OptionList(id="activity-actions", markup=False)
            yield Static("Enter does what is chosen    Esc or t: close", classes="hint", markup=False)

    def on_mount(self) -> None:
        self._show()
        self._read_states()
        self.set_interval(0.5, self._show)
        self.set_interval(5, self._read_states)
        self.query_one("#activity-actions").focus()

    def action_close(self) -> None:
        self.dismiss()

    @work(thread=True, exclusive=True, group="activity-states")
    @guarded("reading what is being saved")
    def _read_states(self) -> None:
        keeper = self.app.keeper
        if keeper is not None and not self._checked:
            # files deleted by hand are not shown as there (a stat a file, once)
            keeper.check()
            self._checked = True
        states = keeper.states() if keeper is not None else []
        self.app.call_from_thread(self._got_states, states)

    def _got_states(self, states) -> None:
        self._states = states
        self._show()

    def _show(self) -> None:
        app = self.app
        accent, bad, dim = theme.current.accent, theme.current.bad, "dim"
        t = Text()
        t.append("Saving on this computer\n", style=f"bold {accent}")
        p = app._keep_progress
        s = app.settings
        if s.keep_when == "night" or s.keep_speed:
            t.append("  " + ("at night (00-07)" if s.keep_when == "night" else "at any time")
                     + (f", {units.size(s.keep_speed * 1024)}/s at the most" if s.keep_speed
                        else "") + " (Settings)\n", style=dim)
        if app._keeping and p is not None and p.stage:
            t.append(f"  Fetching {p.stage}, then taking the tunes out of it: {p.line}\n")
        elif app._keeping and p is not None:
            t.append(f"  Fetching: {p.line}\n")
        elif app._keeping:
            t.append("  Looking at what is to be fetched...\n")
        elif not app._keep_allowed() and (
                (p is not None and p.waiting)
                or (self._states and not all(s.complete for s in self._states))):
            t.append(f"  {app._keep_wait_text()}\n")
        elif p is not None and p.stopped:
            t.append(f"  {p.stopped}\n",
                     style="" if p.waiting or p.stopped.startswith("Stopped") else bad)
        elif p is not None:
            t.append(f"  Done: {p.done:,} files fetched ({units.size(p.done_bytes) or 'nothing'})"
                     + (f", {p.moved:,} moved" if p.moved else "")
                     + (f", {p.removed:,} taken away" if p.removed else "") + ".\n")
        else:
            t.append("  Nothing is being fetched.\n", style=dim)
        if p is not None and p.failed:
            t.append(f"  {len(p.failed):,} files could not be fetched; they are tried the "
                     "next time:\n", style=bad)
            for path, why in p.failed[-5:]:
                t.append(f"    {path}  ({why})\n", style=dim)
        if self._states:
            t.append("\n  What is saved, in ")
            t.append(units.folder(app.keeper.folder), style=dim)
            t.append(":\n")
            for state in self._states:
                mark = "✓" if state.complete else "…"
                t.append(f"    {mark} {state.line.replace(state.scope.name, app._scope_name(state.scope), 1)}\n",
                         style="" if state.complete else theme.current.warn)
        elif app._kept:
            t.append("\n  Reading what is saved...\n", style=dim)
        else:
            t.append("  Nothing is saved yet: s saves a song, an artist, a format or a "
                     "collection.\n", style=dim)

        t.append("\nExporting\n", style=f"bold {accent}")
        job = app._export_job
        if job is None:
            t.append("  Nothing is being exported: e exports a song, a playlist, an artist "
                     "or the queue.\n", style=dim)
        else:
            what = job.name or "one song"
            if job.running:
                t.append(f"  {what}: {job.done:,} of {job.of:,} songs  ·  {job.written:,} files"
                         + (f"  ·  about {job.left} left" if job.left else "") + "\n")
                if job.song:
                    t.append(f"  Now: {job.song}\n", style=dim)
            else:
                t.append(f"  {what}: {'stopped, ' if job.stopped else 'done, '}"
                         f"{job.written:,} files" + (f" in {job.folder}" if job.folder else "")
                         + "\n")
            if job.failed:
                t.append(f"  {len(job.failed):,} songs could not be exported:\n", style=bad)
                for song, why in job.failed[-5:]:
                    t.append(f"    {song.title}  ({why})\n", style=dim)
        # twice a second: drawn only when it says something else, and laid out only
        # when it has another number of lines (its height follows them)
        if t != self._said:
            body = self.query_one("#activity-body", Static)
            width = body.content_size.width
            lines = (len(t.wrap(self.app.console, width)), width) if width > 0 else None
            body.update(t, layout=lines is None or lines != self._said_lines)
            self._said, self._said_lines = t, lines
        self._fill_actions()

    def _fill_actions(self) -> None:
        app = self.app
        actions = []
        if app._keeping:
            actions.append(("stop_saving", "Stop fetching for now (it goes on at the next start)"))
        elif app._kept and self._states and not all(s.complete for s in self._states):
            actions.append(("go_on_saving", "Fetch what is not there yet"))
        if app._exporting:
            actions.append(("stop_export", "Stop exporting"))
        for i, state in enumerate(self._states):
            actions.append((f"drop:{i}", f"Stop keeping {app._scope_name(state.scope)}"))
        if actions == self._actions:
            return
        self._actions = actions
        ol = self.query_one("#activity-actions", OptionList)
        at = ol.highlighted
        ol.clear_options()
        ol.add_options([Option(label, id=key) for key, label in actions])
        ol.display = bool(actions)
        if actions:
            ol.highlighted = min(at or 0, len(actions) - 1)

    @on(OptionList.OptionSelected, "#activity-actions")
    def _chosen(self, event: OptionList.OptionSelected) -> None:
        app, key = self.app, event.option_id
        if key == "stop_saving":
            app._keep_stop.set()
        elif key == "go_on_saving":
            app._keep_fetch(asked=True)
        elif key == "stop_export":
            app._export_stop.set()
        elif key and key.startswith("drop:"):
            index = int(key[5:])
            if index < len(self._states):
                app._ask_drop(self._states[index].scope)
        self._show()
