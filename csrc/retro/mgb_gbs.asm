; SPDX-License-Identifier: GPL-3.0-or-later
; bbmp-retro, Gameboy Tracker engine: the two entry points of the GBS image that
; the helper builds around the SoundSystem driver (github.com/gbdev/GBSoundSystem).
;
; The helper puts the song in bank 1 and up. Bank 1 starts with four words that say
; where the song is: instrument table, its bank, order table, its bank.

SECTION "modland reserved", ROM0[$0000]
	ds	$400, $FF		; a GBS image starts at $0400; keep the linker out of here

SECTION "modland entry", ROM0[$0400]
GbsInit::
	jp	Init
GbsPlay::
	jp	SoundSystem_Process

Init:
	call	SoundSystem_Init
	call	ReadHeader		; de = instrument table, bc = its bank
	call	Music_PrepareInst
	call	ReadHeader
	ld	a, [hl+]		; hl is left at the order table's entry
	ld	e, a
	ld	a, [hl+]
	ld	d, a
	ld	a, [hl+]
	ld	c, a
	ld	b, [hl]
	jp	Music_Play

; the song's header is at the start of bank 1
ReadHeader:
	ld	a, 1
	ld	[$2000], a
	ld	hl, $4000
	ld	a, [hl+]
	ld	e, a
	ld	a, [hl+]
	ld	d, a
	ld	a, [hl+]
	ld	c, a
	ld	a, [hl+]
	ld	b, a
	ret

; Notes above B-5 are past the driver's table of 72 frequencies; the tracker plays
; them all the same, with what follows its own table in memory. Songs have such
; notes (C-6), so the same words follow the table here: mgb_gbs.link puts this
; section right behind the driver's table.
SECTION "modland past the table", ROM0
	dw	$0005, $0040, $0000, $0000, $0008, $0002
