;=====================================================================
; XTRACE.INC - BUILD exittrace: where the way back from Windows stops.
;
; Charlie's 486 never gets the prompt back when Windows exits - a
; blinking cursor and nothing else - and no rig here does it: QEMU and
; DOSBox-X come home every time.  So the machine that hangs has to say
; where, and the only thing it can say it with is the screen.
;
; Each step of the road home writes one letter, white on red, at its
; own place on the top row, so a photograph of the hang reads as the
; letters that are there and the first one that is not:
;
;   col 50  W   RMDOS: Windows' exit broadcast, INT 2Fh 1606h
;       51  X   RMDOS: the program that parked has ended (term_home)
;       52  G   RMDOS: rd_goback, the excursion is over
;       53  J   RMDOS: every hook undone, the jump to PMIO next
;       54  a   PMIO:  rm_unpark, in real mode
;       55  b   PMIO:  the drives told to be quiet again
;       56  c   PMIO:  A20 on ("!" here: it would not come on)
;       57  K   PMDOS: back in protected mode, after IOS_PARK
;       58  L   PMDOS: the DOS data segment put back
;       59  M   PMDOS: the arena joined to upper memory again
;       60  V   PMDOS: the vectors, and the mouse restarted
;       61  C   PMDOS: the sector cache dropped
;       62  A.. PMDOS: the drive being mounted again, then "-"
;       63  R   PMDOS: every drive mounted again
;       64  E   PMDOS: the low memory given back
;       65  U   PMDOS: the cache given its memory back
;       66  Z   PMDOS: Windows' loader is ended, and WIN.COM runs on
;                      in its box
;       68  T   PMDOS: WIN.COM's box is over
;       69  Q   PMDOS: a program is waiting for a key: the prompt
;
; WIN.COM sets the screen's mode again as it leaves, which wipes the
; row: after a good exit only T and Q are left there, and a hang
; before WIN.COM runs again leaves everything up to where it stopped.
;
; And a CPU fault in real mode, which is what QEMU could never show:
; a real 386 or 486 faults on a word read at offset FFFFh where QEMU
; reads on (MSD's AARD check, 2026-10-09), and in real mode the fault
; goes through the IVT to vector 0Dh - which is IRQ 5's - whose handler
; returns to the same instruction, for ever.  RMDOS takes 06h, 0Ch and
; 0Dh while it is the DOS (xt_hook) and, for one that is not an
; interrupt request, writes the vector, CS:IP and the registers on the
; second row and stops:
;
;   F000D 15D4 0100 A1FFFFB8 0000 FFFE ... 15D4 FFFE
;   F, the vector, CS, IP, four bytes at CS:IP, then AX BX CX DX SI DI
;   BP DS ES SS SP as they were at the fault
;
; (that one is BAD0D.COM - MOV AX,[FFFFh] - run by PARKTEST under
; DOSBox-X, which does check a real-mode limit for that form).
;
; Nothing here is in a release build.
;=====================================================================

; 16-bit: the letter at that column of row 0.  Every register kept.
; The letter goes in as its code - 057h, not 'W' - because wasm takes
; the quotes off a character handed to a macro and then looks for a
; symbol called W.
XTMARK16 MACRO col, chr
        push    ax
        push    di
        push    es
        mov     ax,0B800h
        mov     es,ax
        mov     di,(col) * 2
        mov     ax,4F00h + (chr)
        mov     es:[di],ax
        pop     es
        pop     di
        pop     ax
        ENDM

; 32-bit, with a flat DS: the same.  Flags and registers kept.
XTMARK32 MACRO col, chr
        mov     word ptr ds:[0B8000h + (col) * 2],4F00h + (chr)
        ENDM

XT_COLDRV       EQU     62              ; the drive being remounted
