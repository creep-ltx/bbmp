# SPDX-License-Identifier: GPL-3.0-or-later
"""Amiga themes.

The two Workbench themes use the system's default four-colour palettes: Workbench 1.x
is blue $05A, white $FFF, black $002 and orange $F80; Workbench 2 and 3 are grey $AAA,
black $000, white $FFF and blue $68B. Guru Meditation is the red-on-black of the
system's crash alert. Copper is a demo-scene look: black, with the level bars shaded
like copper bars, a colour change on every line.

A theme can give the level bars colours of their own in the "modland-bars" variable,
from the bottom of a bar to its top. They are shaded into each other, or, with
"modland-bars-mode" set to "steps", used as they are: a four-colour Workbench has no
shades in between.
"modland-accent" and "modland-primary" name the colours for text the player draws:
Textual rounds the colours it derives, and the palette is meant to be exact.
"""

from __future__ import annotations

from textual.theme import Theme

BARS = "modland-bars"


def _bars(*colours: str) -> str:
    return ",".join(colours)


WORKBENCH_13 = Theme(
    name="amiga-workbench-1.3",
    dark=True,
    background="#0055AA", surface="#0055AA", panel="#003F88",
    foreground="#FFFFFF",
    primary="#FF8800", secondary="#FFFFFF", accent="#FF8800",
    success="#FFFFFF", warning="#FF8800", error="#000022",
    variables={
        "block-cursor-background": "#FF8800", "block-cursor-foreground": "#000022",
        "block-cursor-text-style": "bold",
        "block-cursor-blurred-background": "#003F88", "block-cursor-blurred-foreground": "#FFFFFF",
        "block-hover-background": "#1166BB",
        "border": "#FF8800", "border-blurred": "#FFFFFF",
        "footer-background": "#FFFFFF", "footer-foreground": "#0055AA",
        "footer-key-foreground": "#000022", "footer-key-background": "#FFFFFF",
        "footer-description-foreground": "#0055AA", "footer-description-background": "#FFFFFF",
        "footer-item-background": "#FFFFFF",
        "input-selection-background": "#FF8800 60%", "input-cursor-background": "#FF8800",
        "scrollbar": "#FFFFFF", "scrollbar-hover": "#FF8800", "scrollbar-active": "#FF8800",
        "scrollbar-background": "#003F88", "scrollbar-corner-color": "#003F88",
        "modland-primary": "#FF8800", "modland-accent": "#FF8800",
        BARS: _bars("#FFFFFF", "#FFFFFF", "#FFFFFF", "#FFFFFF", "#FF8800", "#FF8800", "#000022"),
        "modland-bars-mode": "steps",
    },
)

WORKBENCH_31 = Theme(
    name="amiga-workbench-3.1",
    dark=False,
    background="#AAAAAA", surface="#AAAAAA", panel="#959595",
    foreground="#000000",
    primary="#6688BB", secondary="#FFFFFF", accent="#FFFFFF",
    success="#6688BB", warning="#FFFFFF", error="#000000",
    variables={
        "block-cursor-background": "#6688BB", "block-cursor-foreground": "#FFFFFF",
        "block-cursor-text-style": "none",
        "block-cursor-blurred-background": "#959595", "block-cursor-blurred-foreground": "#000000",
        "block-hover-background": "#B8B8B8",
        "border": "#FFFFFF", "border-blurred": "#000000",
        "footer-background": "#6688BB", "footer-foreground": "#FFFFFF",
        "footer-key-foreground": "#FFFFFF", "footer-key-background": "#6688BB",
        "footer-description-foreground": "#000000", "footer-description-background": "#6688BB",
        "footer-item-background": "#6688BB",
        "input-selection-background": "#6688BB 60%", "input-cursor-background": "#000000",
        "input-cursor-foreground": "#FFFFFF",
        "scrollbar": "#6688BB", "scrollbar-hover": "#FFFFFF", "scrollbar-active": "#FFFFFF",
        "scrollbar-background": "#959595", "scrollbar-corner-color": "#959595",
        "modland-primary": "#000000", "modland-accent": "#FFFFFF",
        BARS: _bars("#6688BB", "#6688BB", "#6688BB", "#6688BB", "#FFFFFF", "#FFFFFF", "#000000"),
        "modland-bars-mode": "steps",
    },
)

GURU = Theme(
    name="amiga-guru-meditation",
    dark=True,
    background="#000000", surface="#000000", panel="#220000",
    foreground="#FF0000",
    primary="#FF0000", secondary="#AA0000", accent="#FF0000",
    success="#AA0000", warning="#FF0000", error="#FF6655",
    variables={
        "block-cursor-background": "#FF0000", "block-cursor-foreground": "#000000",
        "block-cursor-text-style": "bold",
        "block-cursor-blurred-background": "#440000", "block-cursor-blurred-foreground": "#FF0000",
        "block-hover-background": "#330000",
        "border": "#FF0000", "border-blurred": "#880000",
        "footer-background": "#000000", "footer-foreground": "#FF0000",
        "footer-key-foreground": "#FF6655", "footer-key-background": "#000000",
        "footer-description-foreground": "#CC0000", "footer-description-background": "#000000",
        "footer-item-background": "#000000",
        "input-selection-background": "#FF0000 50%", "input-cursor-background": "#FF0000",
        "scrollbar": "#AA0000", "scrollbar-hover": "#FF0000", "scrollbar-active": "#FF0000",
        "scrollbar-background": "#1A0000", "scrollbar-corner-color": "#1A0000",
        "modland-primary": "#FF0000", "modland-accent": "#FF0000",
        BARS: _bars("#660000", "#880000", "#AA0000", "#CC0000", "#EE0000", "#FF2211", "#FF6655"),
    },
)

COPPER = Theme(
    name="amiga-copper",
    dark=True,
    background="#000000", surface="#05050F", panel="#11112A",
    foreground="#DDE4FF",
    primary="#55AAFF", secondary="#AA66FF", accent="#FFAA00",
    success="#33DD88", warning="#FFCC00", error="#FF4466",
    variables={
        "block-cursor-background": "#55AAFF", "block-cursor-foreground": "#000000",
        "block-cursor-text-style": "bold",
        "block-cursor-blurred-background": "#1B2A55", "block-cursor-blurred-foreground": "#DDE4FF",
        "block-hover-background": "#11112A",
        "border": "#FFAA00", "border-blurred": "#3344AA",
        "footer-background": "#11112A", "footer-foreground": "#DDE4FF",
        "footer-key-foreground": "#FFAA00", "footer-key-background": "#11112A",
        "footer-description-foreground": "#DDE4FF", "footer-description-background": "#11112A",
        "footer-item-background": "#11112A",
        "input-selection-background": "#55AAFF 45%", "input-cursor-background": "#FFAA00",
        "scrollbar": "#3344AA", "scrollbar-hover": "#55AAFF", "scrollbar-active": "#FFAA00",
        "scrollbar-background": "#05050F", "scrollbar-corner-color": "#05050F",
        "modland-primary": "#55AAFF", "modland-accent": "#FFAA00",
        # a copper list's worth of colours, one change per line, from the bottom up
        BARS: _bars("#2200AA", "#0044FF", "#0099FF", "#00DDCC", "#22EE55", "#CCEE00",
                    "#FFCC00", "#FF8800", "#FF3300", "#FF0077", "#EE44FF"),
    },
)

THEMES = (WORKBENCH_13, WORKBENCH_31, COPPER, GURU)
