# SPDX-License-Identifier: GPL-3.0-or-later
"""Small boxes in the middle of the screen: busy, ask, pick, confirm."""

from __future__ import annotations

from rich.text import Text
from textual import on
from textual.app import ComposeResult
from textual.binding import Binding
from textual.containers import Vertical
from textual.screen import ModalScreen
from textual.widgets import Input, OptionList, Static
from textual.widgets.option_list import Option


class Busy(ModalScreen):
    """Shown while the catalog downloads."""

    DEFAULT_CSS = """
    Busy { align: center middle; }
    Busy > Static { width: 60; height: auto; padding: 1 2; border: round $accent; background: $panel; }
    """

    def __init__(self, text: str):
        super().__init__()
        self._text = text

    def compose(self) -> ComposeResult:
        yield Static(self._text, id="busy-text", markup=False)

    def say(self, text: str) -> None:
        self.query_one("#busy-text", Static).update(text)


class Dialog(ModalScreen):
    """A small box in the middle of the screen. Esc closes it."""

    DEFAULT_CSS = """
    Dialog { align: center middle; }
    Dialog > Vertical { width: 76; max-width: 96%; height: auto; max-height: 80%; padding: 1 2;
                        border: round $accent; background: $panel; }
    Dialog OptionList { height: auto; max-height: 20; border: none; }
    Dialog .hint { color: $text-muted; margin-top: 1; }
    """
    BINDINGS = [Binding("escape", "cancel", show=False)]

    def action_cancel(self) -> None:
        self.dismiss(None)


class Ask(Dialog):
    """Asks for one line of text; gives None when cancelled or left empty."""

    def __init__(self, prompt: str, value: str = "", replace: bool = True, empty: bool = False):
        """`replace`: the value is selected, so typing replaces it; otherwise typing adds to it.
        `empty`: Enter on nothing gives "" (only Esc gives None)."""
        super().__init__()
        self._prompt, self._value, self._replace = prompt, value, replace
        self._empty = empty

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Static(self._prompt, markup=False)
            yield Input(value=self._value, id="ask", select_on_focus=self._replace)
            yield Static("Enter: OK    Esc: cancel", classes="hint", markup=False)

    @on(Input.Submitted)
    def _done(self, event: Input.Submitted) -> None:
        self.dismiss(event.value.strip() or ("" if self._empty else None))


class Pick(Dialog):
    """Picks one of a list; gives the option's id, or None."""

    def __init__(self, title: str, options: list[tuple[str, str]]):
        super().__init__()
        self._title, self._options = title, options
        self.preview = None          # called with the id of each option the cursor lands on
        self.start: str | None = None   # the option to begin on

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Static(self._title, markup=False)
            yield OptionList(*(Option(label, id=key) for key, label in self._options), id="pick", markup=False)
            yield Static("Enter: choose    Esc: cancel", classes="hint", markup=False)

    def on_mount(self) -> None:
        keys = [key for key, _ in self._options]
        self.query_one("#pick", OptionList).highlighted = (
            keys.index(self.start) if self.start in keys else 0)

    @on(OptionList.OptionHighlighted)
    def _moved(self, event: OptionList.OptionHighlighted) -> None:
        if self.preview is not None and event.option_id:
            self.preview(event.option_id)

    @on(OptionList.OptionSelected)
    def _done(self, event: OptionList.OptionSelected) -> None:
        self.dismiss(event.option_id)


class Confirm(Dialog):
    """A yes-or-no question; gives True only for Enter."""

    BINDINGS = [Binding("escape", "cancel", show=False), Binding("enter", "yes", show=False)]

    def __init__(self, question: str):
        super().__init__()
        self._question = question

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Static(self._question, markup=False)
            yield Static("Enter: yes    Esc: no", classes="hint", markup=False)

    def action_yes(self) -> None:
        self.dismiss(True)


class ChooseCollections(Dialog):
    """Which collections to have: Enter ticks one or not, "Done" takes the choice.
    Gives the ids chosen, or None when cancelled."""

    DEFAULT_CSS = """
    ChooseCollections > Vertical { width: 90; max-width: 96%; }
    """

    def __init__(self, title: str, offered: list[tuple[str, str, str]], ticked: list[str],
                 esc: str = "cancel"):
        """`offered`: (id, name, what it is). `esc`: what Esc does, as the hint says it."""
        super().__init__()
        self._title, self._offered = title, offered
        self._ticked = set(ticked)
        self._esc = esc

    def compose(self) -> ComposeResult:
        with Vertical():
            yield Static(self._title, markup=False)
            yield OptionList(id="choose", markup=False)
            yield Static(f"Enter: tick or untick    Enter on Done: take it    Esc: {self._esc}",
                         classes="hint", markup=False)

    def on_mount(self) -> None:
        self._fill(0)

    def _fill(self, at: int) -> None:
        ol = self.query_one("#choose", OptionList)
        ol.clear_options()
        for id_, name, about in self._offered:
            mark = "✓" if id_ in self._ticked else " "
            ol.add_option(Option(Text.assemble((f"{mark} {name}", "bold"), f":  {about}"), id=id_))
        done = "Done" if self._ticked else "Done (tick at least one)"
        ol.add_option(Option(done, id="\x00done"))
        ol.highlighted = at

    @on(OptionList.OptionSelected, "#choose")
    def _chosen(self, event: OptionList.OptionSelected) -> None:
        if event.option_id == "\x00done":
            if self._ticked:
                self.dismiss([i for i, _, _ in self._offered if i in self._ticked])
            return
        self._ticked ^= {event.option_id}
        self._fill(event.option_index)
