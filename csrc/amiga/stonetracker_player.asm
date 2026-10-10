; SPDX-License-Identifier: GPL-3.0-or-later
; StoneTracker player for UADE (a DeliTracker/EaglePlayer style player).
;
; The replay routine is StoneTracker's own: "StonePlayer_Hard.Bin" 1.98 by Michael
; Lavaire, the player "programmed to run with no operating system running" from the
; free edition of StoneTracker 1.26 (Aminet mus/edit/stonefree2.lha, Players.LHS).
; It is not part of this source: the build script fetches it and it is included
; here when this file is assembled. This file is only the glue between UADE and it.
;
; A song is two files: the module "SPM.name" (or "name.spm") and the sample bank
; "SPS.name" (or "name.sps"). The sample bank must not be packed: bbmp-amiga
; unpacks packed ones before it hands them to UADE.
;
;   vasmm68k_mot -Fhunkexe -kick1hunks -nosym -m68020 -I <uade>/amigasrc/score \
;       -I <folder of StonePlayer_Hard.bin> -o StoneTracker stonetracker_player.asm

	include	misc/DeliPlayer.i

; offsets into StonePlayer_Hard.bin (StonePlayer_Bin.I)
spInstallPlayer		equ	$00
spRemovePlayer		equ	$04
spInstallModule		equ	$08
spInitPlayer		equ	$0C
spStartPlayer		equ	$10
spStopPlayer		equ	$14
spSetPlayerPos		equ	$18
spGetPlayerData		equ	$1C
spSetVectors		equ	$24
spSlideSongVolume	equ	$28
spSlideBalance		equ	$2C

MHD_NbSong		equ	36	; in the module's header

	section	Player,code_c

	PLAYERHEADER PlayerTags

; this string and the Creator one are as long as they were when the helper was called
; modland-amiga: their length moves the code after them, and the sound changes a little
	dc.b	'$VER: StoneTracker player for UADE 1.0 (by bbmp-amiga)',0
	even

PlayerTags
	dc.l	DTP_PlayerVersion,1
	dc.l	DTP_PlayerName,Name
	dc.l	DTP_Creator,Creator
	dc.l	DTP_Check2,Check
	dc.l	DTP_ExtLoad,ExtLoad
	dc.l	DTP_SubSongRange,SubSongRange
	dc.l	DTP_InitPlayer,InitPlayer
	dc.l	DTP_EndPlayer,EndPlayer
	dc.l	DTP_InitSound,InitSound
	dc.l	DTP_EndSound,EndSound
	dc.l	DTP_StartInt,StartInt
	dc.l	DTP_StopInt,StopInt
	dc.l	TAG_DONE

Name	dc.b	'StoneTracker',0
Creator	dc.b	'StonePlayer 1.98 by Michael Lavaire,',10
	dc.b	'glue for UADE by bbmp-amiga v1',0
	even

DeliBase	dc.l	0
ModulePtr	dc.l	0
Bank		dc.l	0
Installed	dc.w	0
Ended		dc.w	0
LastPos		dc.l	-1
Server		dc.l	0,0		; an exec Interrupt: node
		dc.b	2,0		; NT_INTERRUPT, priority
		dc.l	Name
		dc.l	0		; is_Data
		dc.l	Watch		; is_Code
PlayerData	ds.b	20		; PD_Song, PD_Position, PD_Line, ...

; ---- is it a StoneTracker module? "SPM" and a version up to 2
Check
	move.l	dtg_ChkData(a5),a0
	moveq	#-1,d0
	cmp.l	#64,dtg_ChkSize(a5)
	blo.s	.no
	cmp.b	#'S',(a0)
	bne.s	.no
	cmp.b	#'P',1(a0)
	bne.s	.no
	cmp.b	#'M',2(a0)
	bne.s	.no
	move.b	3(a0),d1
	beq.s	.no
	cmp.b	#2,d1
	bhi.s	.no
	tst.w	MHD_NbSong(a0)
	beq.s	.no
	moveq	#0,d0
.no	rts

; ---- load the sample bank: "SPM.name" -> "SPS.name", "name.spm" -> "name.sps"
ExtLoad
	move.l	dtg_PathArrayPtr(a5),a0
	clr.b	(a0)
	move.l	dtg_CopyDir(a5),a0
	jsr	(a0)
	move.l	dtg_PathArrayPtr(a5),a0
.end	tst.b	(a0)+
	bne.s	.end
	subq.l	#1,a0
	move.l	a0,a3			; where the file's name starts
	move.l	dtg_CopyFile(a5),a0
	jsr	(a0)
	move.b	(a3),d0
	and.b	#$df,d0
	cmp.b	#'S',d0
	bne.s	.suffix
	move.b	1(a3),d0
	and.b	#$df,d0
	cmp.b	#'P',d0
	bne.s	.suffix
	move.b	2(a3),d0
	and.b	#$df,d0
	cmp.b	#'M',d0
	bne.s	.suffix
	cmp.b	#'.',3(a3)
	bne.s	.suffix
	addq.b	#'S'-'M',2(a3)		; keeps the case
	bra.s	.load
.suffix	move.l	a3,a0
.end2	tst.b	(a0)+
	bne.s	.end2
	subq.l	#2,a0			; the last character
	move.b	(a0),d0
	and.b	#$df,d0
	cmp.b	#'M',d0
	bne.s	.fail
	addq.b	#'S'-'M',(a0)
.load	move.l	dtg_LoadFile(a5),a0
	jmp	(a0)
.fail	moveq	#-1,d0
	rts

SubSongRange
	moveq	#1,d0
	moveq	#1,d1
	move.l	ModulePtr(pc),a0
	move.w	MHD_NbSong(a0),d1
	rts

InitPlayer
	lea	DeliBase(pc),a0
	move.l	a5,(a0)
	moveq	#0,d0
	move.l	dtg_GetListData(a5),a0
	jsr	(a0)
	lea	ModulePtr(pc),a1
	move.l	a0,(a1)
	moveq	#1,d0
	move.l	dtg_GetListData(a5),a0
	jsr	(a0)
	lea	Bank(pc),a1
	move.l	a0,(a1)
	move.l	a0,d0
	beq.s	.fail
	cmp.b	#'S',(a0)		; "SPS" and not packed
	bne.s	.fail
	cmp.b	#'P',1(a0)
	bne.s	.fail
	cmp.b	#'S',2(a0)
	bne.s	.fail
	btst	#0,4(a0)
	bne.s	.fail
	move.l	dtg_AudioAlloc(a5),a0
	jsr	(a0)
	tst.l	d0
	bne.s	.fail

	movem.l	d2-d7/a2-a6,-(sp)
	bsr	SoftAckFirst
	lea	StonePlayer,a4
	lea	ChipBuffer,a0
	sub.l	a1,a1			; the vector base
	moveq	#3,d0			; AttnFlags: 68010 and 68020
	jsr	spInstallPlayer(a4)
	tst.b	d0
	bne.s	.failed
	lea	Installed(pc),a0
	st	(a0)
	move.l	ModulePtr(pc),a0
	move.l	Bank(pc),a1
	sub.l	a2,a2
	jsr	spInstallModule(a4)
	tst.b	d0
	bne.s	.failed
	movem.l	(sp)+,d2-d7/a2-a6
	moveq	#0,d0
	rts
.failed	movem.l	(sp)+,d2-d7/a2-a6
.fail	moveq	#-1,d0
	rts

; StonePlayer's level 1 (SOFT) interrupt handler looks at its job flags for the last
; time and only then clears its own request in INTREQ. A job that a higher interrupt
; posts in between is lost with that request, and the music stops for good. Here the
; loaded copy of the routine is changed so that the handler clears its request first
; and then looks at the flags. Only the shipped bytes are changed; any other version
; of the routine is left alone.
; Exactly 14 instructions run when it changes the bytes, and this code fills 64 bytes
; up to EndPlayer: the routine's place in memory and the time this takes both decide
; the sound, which is known exactly for this timing and place.
SoftAckFirst
	lea	StonePlayer+$F3E(pc),a0
	cmp.l	#$4A2D00B4,(a0)		; tst.b $B4(a5)
	bne.s	.keep
	cmp.l	#$66D633FC,4(a0)	; bne back to $F1A / move.w #4,...
	bne.s	.keep
	cmp.l	#$000400DF,8(a0)
	bne.s	.keep
	cmp.w	#$F09C,12(a0)		; ...$DFF09C
	bne.s	.keep
	move.l	#$33FC0004,(a0)+	; move.w #4,$DFF09C first,
	move.l	#$00DFF09C,(a0)+
	move.l	#$4A2D00B4,(a0)+	; then tst.b $B4(a5)
	move.w	#$66CE,(a0)		; and bne back to $F1A
.keep	rts

EndPlayer
	move.w	Installed(pc),d0
	beq.s	.not
	movem.l	d2-d7/a2-a6,-(sp)
	lea	StonePlayer,a4
	jsr	spRemovePlayer(a4)
	movem.l	(sp)+,d2-d7/a2-a6
	lea	Installed(pc),a0
	clr.w	(a0)
.not	move.l	dtg_AudioFree(a5),a0
	jmp	(a0)

InitSound
	movem.l	d2-d7/a2-a6,-(sp)
	lea	Ended(pc),a0
	clr.w	(a0)
	lea	StonePlayer,a4
	jsr	spInitPlayer(a4)
	moveq	#64,d0
	moveq	#0,d1
	jsr	spSlideSongVolume(a4)
	move.l	#128,d0
	moveq	#0,d1
	jsr	spSlideBalance(a4)
	lea	Vectors(pc),a0
	jsr	spSetVectors(a4)
	moveq	#0,d0
	move.w	dtg_SndNum(a5),d0
	moveq	#0,d1
	moveq	#0,d2
	moveq	#0,d3
	jsr	spSetPlayerPos(a4)
	; forget where the song has been, and watch it fifty times a second
	lea	Visited,a0
	move.w	#8192/4-1,d0
.clear	clr.l	(a0)+
	dbf	d0,.clear
	lea	LastPos(pc),a0
	moveq	#-1,d0
	move.l	d0,(a0)
	lea	Server(pc),a1
	moveq	#5,d0			; INTB_VERTB
	move.l	4.w,a6
	jsr	-168(a6)		; AddIntServer
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; The end of a song that jumps back instead of ending: the song comes to a position
; where it has been before. (A song that runs off its last position is reported by
; the replay routine itself.)
Watch
	movem.l	d1-d7/a0-a6,-(sp)
	move.w	Ended(pc),d0
	bne.s	.done
	lea	StonePlayer,a4
	lea	PlayerData(pc),a0
	jsr	spGetPlayerData(a4)
	lea	PlayerData(pc),a0
	tst.b	10(a0)			; PD_InPlay
	beq.s	.done
	moveq	#0,d0
	move.w	2(a0),d0		; PD_Position
	lea	LastPos(pc),a1
	cmp.l	(a1),d0
	beq.s	.done
	move.l	d0,(a1)
	move.l	d0,d1
	lsr.l	#3,d1
	and.w	#8191,d1
	lea	Visited,a1
	bset	d0,(a1,d1.w)
	beq.s	.done
	bsr	SongEnd
.done	movem.l	(sp)+,d1-d7/a0-a6
	moveq	#0,d0
	rts

EndSound
	movem.l	d2-d7/a2-a6,-(sp)
	lea	StonePlayer,a4
	moveq	#1,d0			; cut the audio channels
	jsr	spStopPlayer(a4)
	movem.l	(sp)+,d2-d7/a2-a6
	rts

StartInt
	movem.l	d2-d7/a2-a6,-(sp)
	lea	StonePlayer,a4
	jsr	spStartPlayer(a4)
	movem.l	(sp)+,d2-d7/a2-a6
	rts

StopInt
	movem.l	d2-d7/a2-a6,-(sp)
	lea	StonePlayer,a4
	moveq	#1,d0
	jsr	spStopPlayer(a4)
	movem.l	(sp)+,d2-d7/a2-a6
	rts

; called by the replay routine, from its interrupt, when the song has ended or
; goes on with another song of the module
SongEnd
	lea	Ended(pc),a0
	tst.w	(a0)
	bne.s	.done
	st	(a0)
	move.l	DeliBase(pc),a5
	move.l	dtg_SongEnd(a5),a0
	jsr	(a0)
.done	rts

Vectors
	dc.l	0			; VL_UserData
	dc.l	0			; VL_ID
	dc.l	SongEnd			; VL_SongEnd
	dc.l	0			; VL_EndFadeVolSong
	dc.l	0			; VL_EndFadeBalance
	dc.l	0			; VL_EndFadeVolFx

	cnop	0,4
StonePlayer
	incbin	StonePlayer_Hard.bin

	section	Buffer,bss_c
ChipBuffer
	ds.b	4140+20
Visited
	ds.b	8192
