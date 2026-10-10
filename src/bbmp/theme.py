# SPDX-License-Identifier: GPL-3.0-or-later
"""The colours the player draws with, taken from the active Textual theme.

Textual themes colour the widgets (borders, tabs, lists). What the player draws itself
(level meters, the spectrum, numbers on the Stats tab) takes its colours from here, so
it follows the theme too.
"""

from __future__ import annotations

from dataclasses import dataclass

from rich.color import Color, ColorParseError
from textual.theme import Theme

DEFAULT_THEME = "bytebandit"

# The player's own look: near-black blue-grey, one amber accent for what is chosen and
# what moves, blue for values. Frames are faint until a list has the keys.
BYTEBANDIT = Theme(
    name="bytebandit",
    dark=True,
    background="#0E1014", surface="#14171D", panel="#1B1F27",
    foreground="#D5DAE1",
    primary="#F0A63A", secondary="#7AA7F0", accent="#F0A63A",
    success="#5CC98B", warning="#F0C03A", error="#E8606F",
    variables={
        "block-cursor-background": "#4A3714", "block-cursor-foreground": "#FFD08A",
        "block-cursor-text-style": "bold",
        "block-cursor-blurred-background": "#262B35", "block-cursor-blurred-foreground": "#D5DAE1",
        "block-hover-background": "#1B1F27",
        "border": "#F0A63A", "border-blurred": "#2C313C",
        "footer-background": "#14171D", "footer-foreground": "#8A93A3",
        "footer-key-foreground": "#F0A63A", "footer-key-background": "#14171D",
        "footer-description-foreground": "#8A93A3", "footer-description-background": "#14171D",
        "footer-item-background": "#14171D",
        "input-selection-background": "#F0A63A 40%", "input-cursor-background": "#F0A63A",
        "scrollbar": "#2C313C", "scrollbar-hover": "#4A5263", "scrollbar-active": "#F0A63A",
        "scrollbar-background": "#0E1014", "scrollbar-corner-color": "#0E1014",
        "modland-primary": "#7AA7F0", "modland-accent": "#F0A63A",
        "modland-bars": "#2FA69A,#5CC98B,#B5D25A,#F0C03A,#F0A63A,#E8606F",
    },
)


@dataclass(frozen=True)
class Palette:
    accent: str = "#ffa62b"     # progress bar, big numbers, bars on the Stats tab
    primary: str = "#4aa3ff"    # keys, values, the oscilloscope trace
    good: str = "#4ebf71"       # level meters: quiet
    warn: str = "#fea62b"       # loud
    bad: str = "#e0566f"        # at the limit; also hearts and errors
    text: str = "#e0e0e0"       # peak markers
    bars: tuple[str, ...] = ()  # colours for the level bars, bottom to top; () = good/warn/bad
    blend: bool = True          # shade between those colours, or step from one to the next

    def bar(self, height: float) -> str:
        """The colour of a level bar at this height (0 = bottom, 1 = top)."""
        if self.bars and self.blend:
            return _blend(self.bars, height)
        if self.bars:
            return self.bars[min(len(self.bars) - 1, int(max(0.0, height) * len(self.bars)))]
        return self.good if height < 0.55 else self.warn if height < 0.8 else self.bad


current = Palette()


def _colour(variables: dict, name: str, fallback: str) -> str:
    value = str(variables.get(name) or "")
    try:
        Color.parse(value)
    except ColorParseError:
        return fallback
    return value


def _blend(stops: tuple[str, ...], where: float) -> str:
    """The colour at `where` (0..1) along the stops, blended between neighbours."""
    where = min(1.0, max(0.0, where)) * (len(stops) - 1)
    i = min(int(where), len(stops) - 2) if len(stops) > 1 else 0
    if len(stops) == 1:
        return stops[0]
    a, b = Color.parse(stops[i]).get_truecolor(), Color.parse(stops[i + 1]).get_truecolor()
    t = where - i
    return "#%02x%02x%02x" % tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def _stops(value: str) -> tuple[str, ...]:
    stops = tuple(s.strip() for s in str(value or "").split(",") if s.strip())
    try:
        for s in stops:
            Color.parse(s)
    except ColorParseError:
        return ()
    return stops


def from_variables(variables: dict) -> Palette:
    d = Palette()
    bars = str(variables.get("modland-bars", ""))
    steps = str(variables.get("modland-bars-mode", "")) == "steps"
    # a theme can name the two text colours itself, when its primary and accent are
    # meant for backgrounds and would be hard to read as text
    variables = {**variables, "accent": variables.get("modland-accent") or variables.get("accent"),
                 "primary": variables.get("modland-primary") or variables.get("primary")}
    return Palette(bars=_stops(bars), blend=not steps,
                   accent=_colour(variables, "accent", d.accent),
                   primary=_colour(variables, "primary", d.primary),
                   good=_colour(variables, "success", d.good),
                   warn=_colour(variables, "warning", d.warn),
                   bad=_colour(variables, "error", d.bad),
                   text=_colour(variables, "foreground", d.text))


def use(variables: dict) -> Palette:
    """Make the theme with these CSS variables the one the player draws with."""
    global current
    current = from_variables(variables)
    return current
