;=====================================================================
; TESTBLK.ASM - TESTBLK.SYS, a block device driver: a small RAM disk.
;
; The block half of DEVICE= is a different job from the character half,
; and this is the thing to try it against the way TESTDEV.SYS was for
; the character half.  It is a real DOS block driver, written the way
; real ones are, and it knows nothing about the DOS calling it being in
; protected mode.
;
; ONE UNIT, 64 SECTORS OF 512 BYTES - 32K, formatted FAT12 by INIT
; itself so that there is a filesystem there the moment DOS looks.
; Small on purpose: it is resident conventional memory, and the point
; is to prove the path rather than to be useful.
;
; WHAT A BLOCK DRIVER OWES DOS, and all of it is in here:
;
;   INIT (0)        the unit count, where the resident part ends, and a
;                   pointer to an array of BPB pointers - one per unit.
;                   DOS reads the BPB to size the drive letter it is
;                   about to create.
;   MEDIA CHECK (1) "has the disk been swapped?"  A RAM disk answers
;                   "not changed" and always can, which is what makes
;                   fixed media the easy case.
;   BUILD BPB (2)   hand back the BPB again.  DOS asks after a media
;                   change it has just been told did not happen, and
;                   answering is cheaper than arguing.
;   READ (4)        a memcpy between the caller's buffer and the disk
;   WRITE (8,9)     image behind this code, in either direction.
;
; THE IMAGE HAS TO BE BELOW endres AND THAT IS THE ONE THING TO GET
; RIGHT.  A driver tells DOS where its resident part ends and every
; paragraph above that goes back to the arena - so a RAM disk whose
; storage sat past the mark would be handed to the next program that
; asked for memory, and would then be quietly full of it.  The first
; draft of this file had it the wrong way round.
;
; Linked with:  wlink form raw bin order clname CODE
;=====================================================================
        .386
_TB     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_TB, DS:_TB, ES:_TB, SS:_TB
        ORG     0

SECSIZE         EQU     512
NSECS           EQU     64              ; 32K
SPC             EQU     1               ; sectors per cluster
NRESV           EQU     1               ; the boot sector
NFATS           EQU     1
ROOTENTS        EQU     32              ; two sectors of directory
FATSECS         EQU     1
MEDIA           EQU     0F8h            ; fixed disk

;--- the device header.  BLOCK: bit 15 CLEAR -------------------------
; The name field of a block header is not a name: the first byte is the
; unit count and the rest is ignored.  DOS reads the real count out of
; the INIT packet afterwards anyway, because INIT is where a driver
; finds out how much hardware it actually has.
header:
        dd      0FFFFFFFFh
        dw      0000h                   ; block, and NO bit 1: this driver
                                        ; does not understand 32-bit sector
                                        ; numbers and 64 sectors never needs
                                        ; one.  Claiming it would be a lie
                                        ; the kernel checks for.
        dw      strat
        dw      intr
        db      1                       ; units
        db      '       '

reqoff  dw      0
reqseg  dw      0
iowrite db      0                       ; which way this transfer goes
imgbase dw      0                       ; the image's base paragraph

;=====================================================================
; strategy - remember ES:BX and go away
;=====================================================================
strat:
        mov     word ptr cs:[reqoff],bx
        mov     word ptr cs:[reqseg],es
        retf

;=====================================================================
; interrupt - do it
;=====================================================================
intr:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    ds
        push    es
        push    cs
        pop     ds
        ASSUME  DS:_TB

        les     bx,dword ptr [reqoff]
        mov     al,es:[bx+2]

        cmp     al,0
        je      do_init
        cmp     al,1
        je      do_media
        cmp     al,2
        je      do_bpb
        cmp     al,4
        je      do_read
        cmp     al,8
        je      do_write
        cmp     al,9
        je      do_write

;--- everything else: done, and nothing happened ----------------------
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; INIT.  Format the disk, then say what we keep and where the BPBs are.
;=====================================================================
do_init:
        ; WHERE THE IMAGE IS, ONCE.  wasm will not divide OFFSET image by
        ; sixteen - it is a relocatable and not a constant - so the base
        ; paragraph is worked out here, at the one moment CS is known to
        ; be right, and everything else reads it.
        ; SI AND NOT BX.  ES:BX is the request packet and stays the
        ; request packet for the whole of this routine - borrowing BX
        ; for two instructions here made INIT answer with a unit count
        ; written somewhere in the code segment, and the kernel refused
        ; the driver for having no units.
        mov     ax,OFFSET image
        shr     ax,4
        mov     si,cs
        add     ax,si
        mov     [imgbase],ax

        call    format

        mov     ax,cs                   ; the array holds a FAR pointer and
        mov     word ptr [bpbtab+2],ax  ; only INIT knows what CS is

        mov     byte ptr es:[bx+0Dh],1          ; one unit
        mov     ax,OFFSET endres                ; ...and this much kept
        mov     es:[bx+0Eh],ax
        mov     ax,cs
        mov     es:[bx+10h],ax
        mov     ax,OFFSET bpbtab                ; the array of BPB pointers
        mov     es:[bx+12h],ax
        mov     ax,cs
        mov     es:[bx+14h],ax
        mov     word ptr es:[bx+3],0100h

        mov     dx,OFFSET msg_hello
        mov     ah,09h
        int     21h
        jmp     intr_out

;=====================================================================
; MEDIA CHECK.  1 means "not changed", and a RAM disk can always say so.
;=====================================================================
do_media:
        mov     byte ptr es:[bx+0Eh],1
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; BUILD BPB.  Hand back the one we already have.
;=====================================================================
do_bpb:
        mov     ax,OFFSET bpb
        mov     es:[bx+12h],ax
        mov     ax,cs
        mov     es:[bx+14h],ax
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

;=====================================================================
; READ and WRITE.
;
; THE IMAGE IS ADDRESSED THROUGH A SEGMENT THAT MOVES, not an offset
; that grows: 64 sectors is only 32K today but the arithmetic that
; would break at 128 is the arithmetic every real block driver had to
; get right, and doing it properly here costs two instructions.
; Sector N starts at paragraph (image/16 + N*32) with offset zero.
;=====================================================================
do_read:
        mov     byte ptr [iowrite],0
        jmp     short do_io
do_write:
        mov     byte ptr [iowrite],1
do_io:
        mov     ax,es:[bx+14h]          ; start sector
        cmp     ax,NSECS
        jae     io_bad
        mov     dx,es:[bx+12h]          ; sectors wanted
        or      dx,dx
        jz      io_none

        mov     di,ax                   ; would it run off the end?
        add     di,dx
        cmp     di,NSECS
        jbe     io_fits
        mov     dx,NSECS                ; a short transfer, reported as one
        sub     dx,ax
io_fits:
        push    dx                      ; how many we will really move
        push    es
        push    bx                      ; the packet, to answer in

        mov     si,ax                   ; the image segment for this sector
        shl     si,5                    ; 512 bytes is 32 paragraphs
        mov     ax,[imgbase]
        add     ax,si

        mov     di,es:[bx+0Eh]          ; the caller's buffer
        mov     cx,es:[bx+10h]
        mov     bx,dx
        shl     bx,8                    ; sectors -> words

        cmp     byte ptr [iowrite],0
        jne     io_wr

        mov     ds,ax                   ; read: image -> caller
        xor     si,si
        mov     es,cx
        mov     cx,bx
        rep     movsw
        jmp     short io_fin
io_wr:
        mov     ds,cx                   ; write: caller -> image
        mov     si,di
        mov     es,ax
        xor     di,di
        mov     cx,bx
        rep     movsw
io_fin:
        push    cs
        pop     ds
        pop     bx
        pop     es
        pop     dx
        mov     es:[bx+12h],dx
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out

io_none:
        mov     word ptr es:[bx+12h],0
        mov     word ptr es:[bx+3],0100h
        jmp     intr_out
io_bad:
        mov     word ptr es:[bx+12h],0
        mov     word ptr es:[bx+3],8108h        ; done, error, sector not found
        jmp     intr_out

intr_out:
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        retf

;=====================================================================
; format - lay a FAT12 filesystem over the image, once, at INIT.
;
; A SECTOR AT A TIME, THROUGH A MOVING SEGMENT, for the same reason
; the transfer above does: the image is bigger than an offset at 128
; sectors and this loop does not have to be revisited when it is.
;=====================================================================
format:
        pushad
        push    es

        mov     ax,[imgbase]
        mov     dx,NSECS
fmt_zero:
        mov     es,ax
        xor     di,di
        mov     cx,SECSIZE / 2
        push    ax
        xor     ax,ax
        rep     stosw
        pop     ax
        add     ax,SECSIZE / 16
        dec     dx
        jnz     fmt_zero

;--- the boot sector: a jump, an OEM name and a BPB ------------------
        mov     ax,[imgbase]
        mov     es,ax
        xor     di,di
        mov     byte ptr es:[di],0EBh   ; a JMP, because everything looks
        mov     byte ptr es:[di+1],3Ch  ; for one
        mov     byte ptr es:[di+2],90h
        mov     si,OFFSET oemname
        mov     di,3
        mov     cx,8
        rep     movsb
        mov     si,OFFSET bpb
        mov     di,0Bh
        mov     cx,BPBLEN
        rep     movsb
        mov     word ptr es:[1FEh],0AA55h

;--- the FAT: the media byte and two more FFs ------------------------
        mov     di,NRESV * SECSIZE
        mov     byte ptr es:[di],MEDIA
        mov     byte ptr es:[di+1],0FFh
        mov     byte ptr es:[di+2],0FFh

;--- and a volume label, so that DIR has something to say ------------
        mov     di,(NRESV + NFATS * FATSECS) * SECSIZE
        mov     si,OFFSET vollabel
        mov     cx,11
        rep     movsb
        mov     byte ptr es:[di],08h    ; attribute: volume label

        pop     es
        popad
        ret

;=====================================================================
; data - ALL of it, the image included, below endres
;=====================================================================
bpbtab      dw  bpb, 0                  ; one far pointer; INIT fills the
                                        ; segment in
bpb:
            dw  SECSIZE                 ; +00 bytes per sector
            db  SPC                     ; +02 sectors per cluster
            dw  NRESV                   ; +03 reserved sectors
            db  NFATS                   ; +05 FATs
            dw  ROOTENTS                ; +06 root entries
            dw  NSECS                   ; +08 total sectors
            db  MEDIA                   ; +0A media descriptor
            dw  FATSECS                 ; +0B sectors per FAT
            dw  8                       ; +0D sectors per track
            dw  1                       ; +0F heads
            dd  0                       ; +11 hidden sectors
            dd  0                       ; +15 total sectors, 32-bit
BPBLEN      EQU $ - bpb

oemname     db  'PMDOS1.0'
vollabel    db  'TESTBLK    '
msg_hello   db  'TESTBLK.SYS installed - a 32K RAM disk',13,10,'$'

            align   16
image       db  NSECS * SECSIZE dup (0)

endres:

_TB     ENDS
        END     header
