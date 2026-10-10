#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""gt1_patch.py <play.inc>...: make GoatTracker 1.53's play routine in C (cut out of
goattrk.c by build-misc.sh) write what its C64 play routine (player1.s) writes.

The editor's C routine is a copy of the C64 routine that differs in its details; the
C64 programs made with the editor (and the songs in HVSC) have the C64 routine's
values. Every change here was found by comparing the two, register by register.
Each is checked to apply exactly once, so another version of the source stops the
build instead of being patched wrongly."""
import sys

PATCHES = [
    # A note's pulse width: the instrument's byte times 16 (the C64 routine stores it
    # with its nybbles swapped); the editor added the byte's high nybble again.
    ("cptr->pulse = ((iptr->pulse & 0xfe) << 4) | (iptr->pulse >> 4);",
     "cptr->pulse = (iptr->pulse & 0xfe) << 4;"),
    # Pulse modulation: the step's lowest bit is not used (it is the hard restart
    # flag), the high byte is not cut to 4 bits, the limits are compared with the
    # high byte only (their upper 4 bits), and a speed of 0 changes nothing.
    ("""      if (!cptr->pulsedir)
      {
        unsigned newpulse = cptr->pulse;
        newpulse += iptr->pulseadd;
        cptr->pulse = newpulse & 0xfff;
        if ((cptr->pulse >> 4) >= iptr->pulselimithigh)
          cptr->pulsedir = 0x01;
      }
      else
      {
        unsigned newpulse = cptr->pulse;
        newpulse -= iptr->pulseadd;
        cptr->pulse = newpulse & 0xfff;
        if ((cptr->pulse >> 4) < iptr->pulselimitlow)
          cptr->pulsedir = 0x00;
      }""",
     """      if (iptr->pulseadd & 0xfe)
      {
        unsigned speed = iptr->pulseadd & 0xfe;
        unsigned lo = cptr->pulse & 0xfe, hi = cptr->pulse >> 8;
        if (!cptr->pulsedir)
        {
          lo += speed;
          hi = (hi + (lo >> 8)) & 0xff;
          lo &= 0xff;
          cptr->pulsedir = hi >= (unsigned)(iptr->pulselimithigh >> 4);
        }
        else
        {
          unsigned borrow = lo < speed;
          lo = (lo - speed) & 0xff;
          hi = (hi - borrow) & 0xff;
          cptr->pulsedir = hi >= (unsigned)(iptr->pulselimitlow >> 4);
        }
        cptr->pulse = (hi << 8) | lo;
      }"""),
    # The pulse's low byte keeps the direction of the modulation in its lowest bit,
    # and the C64 routine writes it to the SID as it is.
    ("sidreg[0x2 + 7*c] = cptr->pulse & 0xfe;",
     "sidreg[0x2 + 7*c] = (cptr->pulse & 0xfe) | cptr->pulsedir;"),
    # An arpeggio (command 0) on a row's first frame steps on as the fast one does,
    # whatever its speed: the C64 routine enters it there with the note byte ($ff)
    # in the accumulator, whose top bit it takes for the speed.
    ("void playroutine(void)\n{",
     "static int gtw_arptick0, gtw_fromtick0;\n\nvoid playroutine(void)\n{"),
    ("""        if ((cptr->newnote >= 0x80) && (!cptr->wavetable))
          goto ARPEGGIO;""",
     """        if ((cptr->newnote >= 0x80) && (!cptr->wavetable))
        {
          gtw_arptick0 = 1;
          goto ARPEGGIO;
        }"""),
    ("""      ARPEGGIO:
      if (cptr->cmddata)""",
     """      ARPEGGIO:
      gtw_fromtick0 = gtw_arptick0;
      gtw_arptick0 = 0;
      if (cptr->cmddata)"""),
    ("""        if (cptr->cmddata >= 0x80)
        {
          cptr->arpcount++;
        }
        else cptr->arpcount += 2;""",
     """        if ((cptr->cmddata >= 0x80) && (!gtw_fromtick0))
        {
          cptr->arpcount++;
        }
        else cptr->arpcount += 2;"""),
    # (the stereo editor's second SID; only in its source)
    ("sidreg2[0x2 + 7*c-21] = cptr->pulse & 0xfe;",
     "sidreg2[0x2 + 7*c-21] = (cptr->pulse & 0xfe) | cptr->pulsedir;", "stereo"),
]


def main():
    for path in sys.argv[1:]:
        text = open(path).read()
        for old, new, *only in PATCHES:
            n = text.count(old)
            if only and "stereo" in only and "sidreg2" not in text:
                continue
            if n != 1:
                sys.exit("%s: a patch applies %d times, not once:\n%s" % (path, n, old.splitlines()[0]))
            text = text.replace(old, new)
        open(path, "w").write(text)


main()
