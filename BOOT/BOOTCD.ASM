;=====================================================================
; BOOTCD.ASM - PM-DOS CD-ROM loader (El Torito, no emulation)
;
; The BIOS reads this from the disc's boot catalog to 0000:7C00 and
; jumps to it with DL = the drive number it gave the CD (E0h and up on
; most BIOSes).  The drive is read with INT 13h AH=42h in 2048-byte
; blocks; there is no diskette or hard disk image anywhere.
;
; The job: find PMIO.SYS and PMDOS.SYS in the root of the disc, load
; PMIO.SYS at 2000:0000 as every other loader does, put PMDOS.SYS at
; DOS_BASE itself, and tell PMIO so.  PMIO cannot read the disc: the
; loader it carries for PMDOS.SYS (FATLOAD.INC) walks a FAT, and its
; 4K stub has no room left for a second one.  So this loader does the
; part PMIO would have done, and writes into PMIO's own globals - the
; copy at 2000:0F00, before PMIO has run - that it did:
;
;   GV_DOSSIZE   the size of PMDOS.SYS, which IORM's load_dos would
;                have written
;   GV_CDBOOT    1: PMDOS.SYS is in place, load_dos does nothing
;   GV_CDSTAMP   the volume's creation stamp, out of its primary
;                volume descriptor.  This is how PMDOS knows which CD
;                drive it was started from: the BIOS's drive number
;                means nothing to the kernel's own ATAPI driver, and
;                the drive whose disc has the same stamp is the one.
;
; ABOVE 1MB WITHOUT A20 OR UNREAL MODE.  PMDOS.SYS goes to DOS_BASE in
; 32K pieces through INT 15h AH=87h, which every AT BIOS has and which
; deals with the A20 gate itself.  IORM turns A20 on afterwards as it
; always does.
;
; A BIOS MAY LOAD LESS THAN THE CATALOG ASKS FOR.  The catalog says four
; 512-byte sectors - this whole file - and some BIOSes read one.  So
; the first 512 bytes do not depend on the rest: they read the file's
; block again, from the boot information table MKSETUP fills in at
; offset 8 (the layout mkisofs uses for -boot-info-table), and copy
; what the BIOS left out.
;
; Memory, all below the PMIO image at 20000h except the staging buffer:
;   0000:7C00  this loader (2048 bytes at most)
;   0000:9000  one 2048-byte block: the descriptors, the root directory
;   2000:0000  PMIO.SYS
;   6000:0000  PMDOS.SYS, 32K at a time, on its way to DOS_BASE
;
; Linked with:  wlink form raw bin option offset=0
; All data references go through DS = 07C0h, as in BOOT.ASM.
;=====================================================================
        .386

        INCLUDE PMDOS.INC

LOADSEG         EQU     2000h           ; PMIO.SYS -> 2000:0000
BUFSEG          EQU     0900h           ; one block -> 0000:9000
STAGESEG        EQU     6000h           ; PMDOS.SYS on its way up
STAGEBLKS       EQU     16              ; 32K a piece
BLKSIZE         EQU     2048
PMIO_MAXBLK     EQU     (STAGESEG - LOADSEG) / (BLKSIZE / 16)
DOS_MAXSIZE     EQU     DOS_LIMIT - DOS_BASE

; a directory record (ISO 9660 9.1)
DR_LEN          EQU     0
DR_EXTENT       EQU     2               ; both-endian, little half first
DR_SIZE         EQU     10
DR_FLAGS        EQU     25
DR_NAMELEN      EQU     32
DR_NAME         EQU     33
DRF_DIR         EQU     02h

PVD_ROOT        EQU     156             ; the root's directory record
PVD_STAMP       EQU     CDSTAMP_PVD     ; creation date and time

_CD     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CD, DS:_CD, ES:NOTHING
        ORG     0

start:
        jmp     short begin
        nop
        db      5 dup(0)

; The boot information table, offset 8.  MKSETUP writes it; a disc
; made without it has bi_file = 0 and relies on the BIOS loading all
; four sectors.
bi_pvd          dd      0               ; block of the primary descriptor
bi_file         dd      0               ; block of this file
bi_length       dd      0               ; its length in bytes
bi_csum         dd      0
bi_resv         db      40 dup(0)

;---------------------------------------------------------------------
begin:
        cli
        xor     ax,ax
        mov     ss,ax
        mov     sp,7C00h
        db      0EAh                    ; JMP FAR 07C0:here - some BIOSes
        dw      OFFSET here             ; start us at 0000:7C00 and some
        dw      07C0h                   ; at 07C0:0000
here:
        mov     ax,cs
        mov     ds,ax
        sti
        cld
        mov     [drive],dl

        ; A DRIVE NUMBER BELOW 80h IS NOT A CD's, and some early El
        ; Torito BIOSes start the loader with DL left as it was.  The
        ; BIOS's status call says which drive it is reading the disc
        ; from: the packet goes in the block buffer, which is free.
        cmp     dl,80h
        jae     drive_ok
        push    ds
        mov     ax,BUFSEG
        mov     ds,ax
        xor     si,si
        mov     byte ptr [si],13h       ; the packet's size
        mov     ax,4B01h
        int     13h
        mov     al,[si+ET_MEDIA]
        mov     dl,[si+ET_DRIVE]
        pop     ds
        jc      drive_ok                ; no answer: DL is all there is
        test    al,0Fh
        jnz     drive_ok                ; emulating something: not us
        mov     [drive],dl
drive_ok:

        ; the rest of this file, in case the BIOS read less of it
        mov     eax,[bi_file]
        or      eax,eax
        jz      whole
        mov     cx,1
        mov     bx,BUFSEG
        call    rdblk
        jc      e_disk
        push    ds
        push    es
        mov     ax,ds
        mov     es,ax
        mov     ax,BUFSEG
        mov     ds,ax
        mov     si,512
        mov     di,si
        mov     cx,(BLKSIZE - 512) / 2
        rep     movsw
        pop     es
        pop     ds
whole:
        jmp     part2

;---------------------------------------------------------------------
; rdblk - EAX = first block, CX = how many, BX = segment (offset 0).
;         CF on failure.  Three tries: a drive that has only just
;         spun up can refuse the first.
;---------------------------------------------------------------------
rdblk:
        push    dx
        push    si
        push    di
        mov     di,3
rb_try:
        mov     word ptr [pk_count],cx
        mov     word ptr [pk_off],0
        mov     word ptr [pk_seg],bx
        mov     dword ptr [pk_lba],eax
        mov     si,OFFSET pk_dap
        mov     dl,[drive]
        push    eax
        mov     ah,42h
        int     13h
        pop     eax
        jnc     rb_out
        dec     di
        jz      rb_fail
        push    ax
        xor     ah,ah                   ; reset, then again
        mov     dl,[drive]
        int     13h
        pop     ax
        jmp     short rb_try
rb_fail:
        stc
rb_out:
        pop     di
        pop     si
        pop     dx
        ret

;---------------------------------------------------------------------
e_disk:
        mov     si,OFFSET msgDisk
fatal:
        call    puts
        mov     si,OFFSET msgHalt
        call    puts
        xor     ax,ax
        int     16h                     ; wait for a key ...
        int     19h                     ; ... and try booting again

puts:
        push    ax
        push    bx
        mov     bx,0007h
pu_lp:
        lodsb
        or      al,al
        jz      pu_done
        mov     ah,0Eh
        int     10h
        jmp     short pu_lp
pu_done:
        pop     bx
        pop     ax
        ret

pk_dap          db      10h, 0
pk_count        dw      0
pk_off          dw      0
pk_seg          dw      0
pk_lba          dd      0, 0

drive           db      0
msgDisk         db      13,10,'PM-DOS: CD-ROM read error',0
msgHalt         db      13,10,'Press a key to restart',13,10,0

p1_end:
.ERRE   (p1_end - start) LE 510, <part 1 of the CD loader is past its 510 bytes>
        ORG     510
        db      55h, 0AAh               ; not looked at by an El Torito
                                        ; BIOS; marks where the part that
                                        ; must stand alone ends

;=====================================================================
; Everything from here on may not have been loaded by the BIOS.
;=====================================================================
part2:
        ; THE PRIMARY VOLUME DESCRIPTOR, from block 16 on: type 1 and
        ; "CD001".  A disc whose boot record or supplementary descriptor
        ; came first is still read; a set terminator ends the search.
        mov     eax,16
pvd_lp:
        mov     cx,1
        mov     bx,BUFSEG
        call    rdblk
        jc      e_disk
        push    es
        mov     bx,BUFSEG
        mov     es,bx
        cmp     dword ptr es:[1],30304443h ; "CD00"
        jne     pvd_bad
        cmp     byte ptr es:[5],'1'
        jne     pvd_bad
        mov     bl,es:[0]
        cmp     bl,1
        je      pvd_found
        cmp     bl,0FFh
        je      pvd_bad
        pop     es
        inc     eax
        cmp     eax,16 + 32
        jb      pvd_lp
        jmp     short e_nopvd
pvd_bad:
        pop     es
e_nopvd:
        mov     si,OFFSET msgNoIso
        jmp     fatal

pvd_found:
        ; the root directory, and the stamp PMDOS will look for
        mov     eax,es:[PVD_ROOT + DR_EXTENT]
        mov     [root_blk],eax
        mov     eax,es:[PVD_ROOT + DR_SIZE]
        mov     [root_len],eax
        mov     si,PVD_STAMP
        mov     di,OFFSET stamp
        mov     cx,CDSTAMP_LEN
pvd_st:
        mov     al,es:[si]
        mov     [di],al
        inc     si
        inc     di
        loop    pvd_st
        pop     es

;--- PMIO.SYS ---------------------------------------------------------
        mov     si,OFFSET nameIo
        call    find
        jc      e_noio
        or      eax,eax
        jz      e_noio
        cmp     eax,PMIO_MAXBLK * BLKSIZE
        ja      e_bigio
        call    blocks                  ; EAX bytes -> ECX blocks
        mov     eax,[f_blk]
        mov     bx,LOADSEG
io_lp:
        mov     dx,cx
        cmp     dx,STAGEBLKS
        jbe     io_this
        mov     dx,STAGEBLKS
io_this:
        push    cx
        mov     cx,dx
        call    rdblk
        pop     cx
        jc      e_disk
        movzx   edx,dx
        add     eax,edx
        shl     dx,7                    ; blocks * 2048 / 16 = paragraphs
        add     bx,dx
        shr     dx,7
        sub     cx,dx
        jnz     io_lp

;--- PMDOS.SYS, through the staging buffer ----------------------------
        mov     si,OFFSET nameDos
        call    find
        jc      e_nodos
        or      eax,eax
        jz      e_nodos
        cmp     eax,DOS_MAXSIZE
        ja      e_bigdos
        mov     [dos_size],eax
        call    blocks
        mov     eax,[f_blk]
        mov     dword ptr [dest],DOS_BASE
dos_lp:
        mov     dx,cx
        cmp     dx,STAGEBLKS
        jbe     dos_this
        mov     dx,STAGEBLKS
dos_this:
        push    cx
        mov     cx,dx
        mov     bx,STAGESEG
        call    rdblk
        pop     cx
        jc      e_disk
        movzx   edx,dx
        add     eax,edx
        call    moveup                  ; DX blocks from the stage to [dest]
        jc      e_move
        sub     cx,dx
        jnz     dos_lp

;--- tell PMIO, in its own globals, and go ----------------------------
        mov     ax,LOADSEG
        mov     es,ax
        mov     eax,[dos_size]
        mov     dword ptr es:[GV_DOSSIZE - IO_BASE],eax
        mov     byte ptr es:[GVO_CDBOOT],1
        mov     si,OFFSET stamp
        mov     di,GVO_CDSTAMP
        mov     cx,CDSTAMP_LEN
        rep     movsb

        mov     dl,[drive]
        db      0EAh                    ; JMP FAR LOADSEG:0000
        dw      0
        dw      LOADSEG

e_noio:
        mov     si,OFFSET msgNoIo
        jmp     fatal
e_nodos:
        mov     si,OFFSET msgNoDos
        jmp     fatal
e_bigio:
        mov     si,OFFSET msgBigIo
        jmp     fatal
e_bigdos:
        mov     si,OFFSET msgBigDos
        jmp     fatal
e_move:
        mov     si,OFFSET msgMove
        jmp     fatal

;---------------------------------------------------------------------
; blocks - EAX = a size in bytes -> CX = the 2048-byte blocks it takes.
;          The callers have already held EAX well under 64K blocks.
;---------------------------------------------------------------------
blocks:
        add     eax,BLKSIZE - 1
        shr     eax,11
        mov     cx,ax
        ret

;---------------------------------------------------------------------
; find - DS:SI -> a name ("PMIO.SYS", NUL-terminated, upper case).
;        -> CF clear, EAX = its size, [f_blk] = its first block.
;
; Every block of the root is read and every record in it looked at.
; A record never crosses a block; a length byte of 0 means the rest
; of the block is padding.  The name on the disc carries ";1" after
; it, so a match is the whole name followed by ';' or by the end.
; Directories are passed over: a directory named PMIO.SYS is not it.
;---------------------------------------------------------------------
find:
        push    bx
        push    cx
        push    dx
        push    di
        push    es
        mov     eax,[root_blk]
        mov     [f_next],eax
        mov     eax,[root_len]
        mov     [f_left],eax
fd_blk:
        cmp     dword ptr [f_left],0
        je      fd_none
        mov     eax,[f_next]
        mov     cx,1
        mov     bx,BUFSEG
        call    rdblk
        jc      fd_none
        inc     dword ptr [f_next]
        mov     eax,[f_left]
        sub     eax,BLKSIZE
        jae     fd_left
        xor     eax,eax
fd_left:
        mov     [f_left],eax

        mov     ax,BUFSEG
        mov     es,ax
        xor     di,di                   ; DI = this record
fd_rec:
        cmp     di,BLKSIZE - DR_NAME
        jae     fd_blk
        movzx   bx,byte ptr es:[di+DR_LEN]
        or      bx,bx
        jz      fd_blk                  ; the block's padding
        cmp     bx,DR_NAME
        jb      fd_blk                  ; too short to be a record
        mov     ax,di
        add     ax,bx
        cmp     ax,BLKSIZE
        ja      fd_blk                  ; runs off the block: damaged
        test    byte ptr es:[di+DR_FLAGS],DRF_DIR
        jnz     fd_skip
        movzx   cx,byte ptr es:[di+DR_NAMELEN]
        mov     ax,cx
        add     ax,DR_NAME
        cmp     ax,bx
        ja      fd_skip                 ; the name runs past the record

        push    si
        push    di
        add     di,DR_NAME
fd_cmp:
        lodsb
        or      al,al
        jz      fd_end
        jcxz    fd_diff                 ; the disc's name is shorter
        cmp     al,es:[di]
        jne     fd_diff
        inc     di
        dec     cx
        jmp     short fd_cmp
fd_end:
        jcxz    fd_same                 ; exactly the name
        cmp     byte ptr es:[di],';'
        je      fd_same                 ; the name, then its version
fd_diff:
        pop     di
        pop     si
fd_skip:
        add     di,bx
        jmp     short fd_rec
fd_same:
        pop     di
        pop     si
        mov     eax,es:[di+DR_EXTENT]
        mov     [f_blk],eax
        mov     eax,es:[di+DR_SIZE]
        clc
        jmp     short fd_out
fd_none:
        stc
fd_out:
        pop     es
        pop     di
        pop     dx
        pop     cx
        pop     bx
        ret

;---------------------------------------------------------------------
; moveup - DX = blocks at STAGESEG:0000 -> [dest], which moves on.
;          CF if INT 15h AH=87h failed.
;
; The descriptor table AH=87h wants: two null entries, the source, the
; destination, and two the BIOS fills in.  Bases are 24 bits wide, and
; the fourth byte of each is the high byte a 386 BIOS also reads; both
; addresses here are below 16MB.
;---------------------------------------------------------------------
moveup:
        push    eax
        push    bx
        push    cx
        push    dx
        push    si
        push    es
        mov     eax,STAGESEG * 16
        mov     word ptr [gdt_src+2],ax
        shr     eax,16
        mov     byte ptr [gdt_src+4],al
        mov     byte ptr [gdt_src+7],ah
        mov     eax,[dest]
        mov     word ptr [gdt_dst+2],ax
        shr     eax,16
        mov     byte ptr [gdt_dst+4],al
        mov     byte ptr [gdt_dst+7],ah
        mov     cx,dx
        shl     cx,10                   ; blocks * 2048 / 2 = words
        movzx   eax,dx
        shl     eax,11
        add     [dest],eax
        mov     ax,ds
        mov     es,ax
        mov     si,OFFSET gdt
        mov     ah,87h
        int     15h
        pop     es
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     eax
        ret

;=====================================================================
nameIo          db      'PMIO.SYS',0
nameDos         db      'PMDOS.SYS',0
msgNoIso        db      13,10,'PM-DOS: the disc has no ISO 9660 volume',0
msgNoIo         db      13,10,'PM-DOS: PMIO.SYS not found',0
msgNoDos        db      13,10,'PM-DOS: PMDOS.SYS not found',0
msgBigIo        db      13,10,'PM-DOS: PMIO.SYS is too large',0
msgBigDos       db      13,10,'PM-DOS: PMDOS.SYS is too large',0
msgMove         db      13,10,'PM-DOS: cannot copy to extended memory',0

gdt             db      16 dup(0)
gdt_src         dw      0FFFFh, 0
                db      0, 93h, 0, 0
gdt_dst         dw      0FFFFh, 0
                db      0, 93h, 0, 0
                db      16 dup(0)

root_blk        dd      0
root_len        dd      0
f_blk           dd      0
f_next          dd      0
f_left          dd      0
dos_size        dd      0
dest            dd      0
stamp           db      CDSTAMP_LEN dup(0)

;---------------------------------------------------------------------
; One block: the catalog asks the BIOS for four sectors, and part 1
; reads one block back.  Anything past 2048 bytes is never loaded.
;---------------------------------------------------------------------
cd_end:
.ERRE   (cd_end - start) LE BLKSIZE, <the CD loader no longer fits in one block>

_CD     ENDS
        END     start
