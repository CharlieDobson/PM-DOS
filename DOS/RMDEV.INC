;=====================================================================
; RMDEV.INC - the drivers CONFIG.SYS loaded, spoken to the way DOS
;             speaks to them
;
; A DEVICE= line is obeyed by the kernel, at boot: the driver is read
; into conventional memory, its INIT runs, and its header goes into the
; chain behind NUL (DEVLOAD.INC).  None of that is undone by the
; handover.  The driver is real-mode code at a real address, and its
; header is in the chain this DOS publishes, because rd_chain made the
; kernel's chain this DOS's (RMINFO.INC).  What was missing was any way
; to TALK to it - a name that found it, a handle that pointed at it,
; and a request packet.
;
; WHY IT MATTERS: WINDOWS FOR WORKGROUPS 3.11.  Its file system manager,
; IFSMGR.386, opens the device IFS$HLP$ in its real-mode start-up,
; reads eight bytes from it with 4402h, and loads only if they are
; IFSHLP.SYS's signature and an entry point it then calls.  IFSHLP.SYS
; is a CONFIG.SYS driver.  Until this file, the open failed, IFSMGR
; declined, and Windows ran without it - which also meant without
; 32-bit file access.
;
; THE RULES ARE MS-DOS'S (DOS6SRC dos/disk.asm, READDEV and WRTDEV): a
; device in raw mode takes a whole transfer in one request; a cooked
; one is read a byte at a time up to a CR or a ^Z, and written a byte
; at a time up to a ^Z, and its SFT keeps an "input not at end" bit
; that a ^Z clears and any write sets again.  OPEN and CLOSE are sent
; when the header asks for them.  A driver's error fails the call with
; DOS's own mapping of its code (13h and up); there is no INT 24h for
; it here - Windows owns the screen, and a DOS box's handler would be
; answering about somebody else's printer.
;
; THE PACKET IS ON THIS DOS'S STACK, built fresh for each request, so
; a driver that calls DOS, which calls a driver, does not write over
; the request it is in the middle of.  ES:BX is therefore the stack
; segment, which in the HMA is FFFFh: a driver reaches it the way it
; reaches any other far pointer, and the A20 gate is open for as long
; as this DOS is up there (RMDOS.INC).
;=====================================================================

;--- the request packet ----------------------------------------------
RQ_LEN          EQU 00h                 ; db  its length
RQ_UNIT         EQU 01h                 ; db
RQ_CMD          EQU 02h                 ; db  DVC_*
RQ_STATUS       EQU 03h                 ; dw  DVS_*, written by the driver
RQ_HDRLEN       EQU 0Dh                 ; the header is the whole of an open,
                                        ; a close and the two status calls
RQ_BUFOFF       EQU 0Eh                 ; dw } read, write, IOCTL in and out
RQ_BUFSEG       EQU 10h                 ; dw }
RQ_COUNT        EQU 12h                 ; dw  wanted, and then moved
RQ_RWLEN        EQU 16h
RQ_GCAT         EQU 0Dh                 ; db  generic IOCTL: the category,
RQ_GMINOR       EQU 0Eh                 ; db  the function,
RQ_GSI          EQU 0Fh                 ; dw  the caller's SI and DI,
RQ_GDI          EQU 11h                 ; dw
RQ_GPKT         EQU 13h                 ; dd  and its parameter block
RQ_GLEN         EQU 17h
RQ_UNITB        EQU 01h                 ; db  block: which of the driver's units
RQ_MEDIA        EQU 0Dh                 ; db  block: the media descriptor
RQ_SECTOR       EQU 14h                 ; dw  block: the first sector, or FFFFh
RQ_HUGE         EQU 1Ah                 ; dd  ...and then it is here (DOS 4)
RQ_BLKLEN       EQU 1Eh
RQ_ROOM         EQU 20h                 ; reserved on the stack for any of them

DVC_IOCTLIN     EQU 03h
DVC_READ        EQU 04h
DVC_INSTAT      EQU 06h
DVC_WRITE       EQU 08h
DVC_OUTSTAT     EQU 0Ah
DVC_IOCTLOUT    EQU 0Ch
DVC_OPEN        EQU 0Dh
DVC_CLOSE       EQU 0Eh
DVC_GENIOCTL    EQU 13h

DVS_BUSY        EQU 0200h
DVS_ERROR       EQU 8000h

DVA_IOCTL       EQU 4000h               ; the header's attribute word
DVA_OCRM        EQU 0800h
DVA_GENIOCTL    EQU 0040h
DVA_BIG         EQU 0002h               ; block: takes RQ_HUGE

DIF_EOF         EQU 40h                 ; SFT_DEVINFO: input NOT at its end
DIF_RAW         EQU 20h
DIF_NUL         EQU 04h

RD_CHAINMAX     EQU 32                  ; headers a walk of the chain will
                                        ; follow - see rd_chain, RMINFO.INC
RD_UNLINKMAX    EQU 4                   ; links rd_chain may have to undo

;=====================================================================
; drv_call - ES:BX = a request packet, FS:DI = the device header.
;            Strategy, then interrupt.  -> AX = the status word, CF if
;            the driver set its error bit.  Nothing else changes.
;
; Both entries are far calls into the header's segment, made through
; far pointers on the stack - not through a variable, for the same
; reason the packet is on the stack.  A driver is entitled to change
; any register but SS and SP, and some do; everything is put back.
;=====================================================================
drv_call:
        pushad
        push    ds
        push    es
        push    fs
        push    gs
        push    fs
        push    word ptr fs:[di+8]      ; the interrupt entry, at SP+4
        push    fs
        push    word ptr fs:[di+6]      ; the strategy entry, at SP
        mov     bp,sp
        call    dword ptr ss:[bp]
        mov     bp,sp                   ; SP is what it was; BP may not be
        call    dword ptr ss:[bp+4]
        add     sp,8
        pop     gs
        pop     fs
        pop     es
        pop     ds
        popad
        mov     ax,word ptr es:[bx+RQ_STATUS]
        test    ax,DVS_ERROR            ; clears the carry...
        jz      dc_out
        stc                             ; ...and this sets it
dc_out:
        ret

;--- rq_zero - zero the packet room at SS:BP.  Everything preserved.
rq_zero:
        push    ax
        push    cx
        push    di
        push    es
        mov     ax,ss
        mov     es,ax
        mov     di,bp
        mov     cx,RQ_ROOM / 2
        xor     ax,ax
        cld
        rep     stosw
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

;=====================================================================
; drv_xfer - SI = the SFT of a loaded device, AL = the command (read,
;            write, IOCTL in or out), ES:DI = the caller's buffer, CX =
;            the count.  -> CX = what the driver moved, AX = its status,
;            CF if it failed.  BX DX SI DI ES are kept.
;=====================================================================
drv_xfer:
        push    bx
        push    dx
        push    di
        push    es
        push    fs
        push    bp
        sub     sp,RQ_ROOM
        mov     bp,sp
        call    rq_zero
        mov     byte ptr [bp+RQ_LEN],RQ_RWLEN
        mov     byte ptr [bp+RQ_CMD],al
        mov     word ptr [bp+RQ_BUFOFF],di
        mov     word ptr [bp+RQ_BUFSEG],es
        mov     word ptr [bp+RQ_COUNT],cx
        lfs     di,dword ptr [si+SFT_DEVPTR]
        mov     bx,ss
        mov     es,bx
        mov     bx,bp
        call    drv_call
        mov     word ptr [rd_drvst],ax  ; MOVs and LEA: the carry survives
        mov     cx,word ptr [bp+RQ_COUNT]
        lea     sp,[bp+RQ_ROOM]
        pop     bp
        pop     fs
        pop     es
        pop     di
        pop     dx
        pop     bx
        ret

;=====================================================================
; drv_short - SI = the SFT of a loaded device, AL = a command that is
;             the header alone: open, close, input or output status.
;             -> AX = the status, CF if the driver failed.
;=====================================================================
drv_short:
        push    bx
        push    di
        push    es
        push    fs
        push    bp
        sub     sp,RQ_ROOM
        mov     bp,sp
        call    rq_zero
        mov     byte ptr [bp+RQ_LEN],RQ_HDRLEN
        mov     byte ptr [bp+RQ_CMD],al
        lfs     di,dword ptr [si+SFT_DEVPTR]
        mov     bx,ss
        mov     es,bx
        mov     bx,bp
        call    drv_call
        lea     sp,[bp+RQ_ROOM]
        pop     bp
        pop     fs
        pop     es
        pop     di
        pop     bx
        ret

;--- drv_attr - SI = the SFT of a loaded device -> AX = its header's
;--- attribute word.  Everything else kept.
drv_attr:
        push    di
        push    es
        les     di,dword ptr [si+SFT_DEVPTR]
        mov     ax,word ptr es:[di+4]
        pop     es
        pop     di
        ret

;--- drv_err - AX = a status word with the error bit set -> AX = the
;--- DOS error for it: the driver's own code, 0-0Fh, is DOS's 13h-22h.
drv_err:
        and     ax,000Fh
        add     ax,13h
        ret

;=====================================================================
; drv_find - is the eight-character name in rd_devname a character
;            driver in the chain?  -> CF clear and [rd_drvhdr] = its
;            header, or CF set.  Everything preserved.
;
; THE CHAIN, NOT A TABLE, AND THE CHAIN FIRST: a loaded driver named
; CON or PRN is found before this DOS's own, which is the whole point
; of loading one.  The walk stops where RMDOS's own headers begin;
; dev_name's table answers for those, and for the LPTn and COMn names
; that have no header of their own here.
;=====================================================================
drv_find:
        push    ax
        push    cx
        push    si
        push    di
        push    es
        mov     es,[rd_lowseg]
        les     si,dword ptr es:[RL_NULDEV]
        mov     cx,RD_CHAINMAX
dfd_one:
        cmp     si,0FFFFh
        je      dfd_no
        cmp     si,RL_DEVS
        jne     dfd_try
        mov     ax,es
        cmp     ax,[rd_lowseg]
        je      dfd_no                  ; RMDOS's own CON: the end of theirs
dfd_try:
        test    word ptr es:[si+4],8000h
        jz      dfd_next                ; a block driver has no name
        push    cx
        push    si
        lea     di,[si+0Ah]
        mov     si,OFFSET rd_devname
        mov     cx,8
        cld
        repe    cmpsb                   ; ZF set only if all eight agree
        pop     si
        pop     cx
        je      dfd_hit
dfd_next:
        les     si,dword ptr es:[si]
        loop    dfd_one
dfd_no:
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        stc
        ret
dfd_hit:
        mov     word ptr [rd_drvhdr],si
        mov     word ptr [rd_drvhdr+2],es
        pop     es
        pop     di
        pop     si
        pop     cx
        pop     ax
        clc
        ret

;=====================================================================
; drv_setsft - SI = an SFT being made for the driver at [rd_drvhdr].
;              Its header, its name, and the device information word
;              DOS builds from the header's attributes: the high byte
;              as the header has it (IOCTL, output until busy, open and
;              close), "a device, input not at its end", and the low
;              five bits - standard in and out, NUL, clock, INT 29h.
;=====================================================================
drv_setsft:
        push    ax
        push    cx
        push    si
        push    di
        push    ds
        push    es
        mov     ax,word ptr [rd_drvhdr]
        mov     word ptr [si+SFT_DEVPTR],ax
        mov     ax,word ptr [rd_drvhdr+2]
        mov     word ptr [si+SFT_DEVPTR+2],ax
        les     di,dword ptr [si+SFT_DEVPTR]
        mov     ax,word ptr es:[di+4]
        and     ax,0FF1Fh
        or      al,80h OR DIF_EOF
        mov     word ptr [si+SFT_DEVINFO],ax
        ; the name, as the header spells it, and a blank extension
        push    ds
        push    es
        pop     ds                      ; DS:SI = the header's name
        pop     es                      ; ES:DI = the SFT's
        xchg    si,di
        add     si,0Ah
        add     di,SFT_NAME
        mov     cx,8
        cld
        rep     movsb
        mov     al,' '
        mov     cx,3
        rep     stosb
        pop     es
        pop     ds
        pop     di
        pop     si
        pop     cx
        pop     ax
        ret

;=====================================================================
; drv_open / drv_close - SI = the SFT of a loaded device.  The header
;                        decides whether the driver hears about it.
;                        drv_open: CF and AX = the error if it refused.
;=====================================================================
drv_open:
        call    drv_attr
        test    ax,DVA_OCRM
        jz      dop_ok
        mov     al,DVC_OPEN
        call    drv_short
        jnc     dop_ok
        call    drv_err
        stc
        ret
dop_ok:
        clc
        ret

drv_close:
        push    ax
        call    drv_attr
        test    ax,DVA_OCRM
        jz      dcl_out
        mov     al,DVC_CLOSE
        call    drv_short               ; a refusal to close is not one
dcl_out:                                ; anybody can do anything about
        pop     ax
        ret

;=====================================================================
; drv_read - SI = the SFT, ES:DI = the buffer, ECX = the count.
;            -> ECX = what was read; CF and AX = the error if the
;            driver failed.
;=====================================================================
drv_read:
        push    dx
        push    di
        test    byte ptr [si+SFT_DEVINFO],DIF_EOF
        jz      drd_none                ; a ^Z has been read: the end
        test    byte ptr [si+SFT_DEVINFO],DIF_NUL
        jnz     drd_none
        jcxz    drd_none
        test    byte ptr [si+SFT_DEVINFO],DIF_RAW
        jz      drd_cooked
        mov     al,DVC_READ             ; raw: all of it in one request
        call    drv_xfer
        jc      drd_err
        movzx   ecx,cx
        jmp     short drd_out
drd_cooked:
        xor     dx,dx                   ; DX = moved so far
drd_byte:
        push    cx
        mov     cx,1
        mov     al,DVC_READ
        call    drv_xfer
        mov     ax,cx
        pop     cx
        jc      drd_err2
        or      ax,ax
        jz      drd_cookdone            ; nothing to give: as far as it goes
        mov     al,es:[di]
        inc     di
        inc     dx
        cmp     al,1Ah
        je      drd_eof
        cmp     al,13
        je      drd_cookdone
        loop    drd_byte
drd_cookdone:
        movzx   ecx,dx
        jmp     short drd_out
drd_eof:
        and     byte ptr [si+SFT_DEVINFO],NOT DIF_EOF
        jmp     short drd_cookdone
drd_none:
        xor     ecx,ecx
drd_out:
        pop     di
        pop     dx
        clc
        ret
drd_err2:
        mov     ax,word ptr [rd_drvst]
drd_err:
        call    drv_err
        pop     di
        pop     dx
        stc
        ret

;=====================================================================
; drv_write - SI = the SFT, ES:DI = the buffer, ECX = the count.
;             -> ECX = what was written; CF and AX = the error if the
;             driver failed.
;=====================================================================
drv_write:
        push    dx
        push    di
        or      byte ptr [si+SFT_DEVINFO],DIF_EOF       ; input may go on
        jcxz    dwr_none
        test    byte ptr [si+SFT_DEVINFO],DIF_NUL
        jnz     dwr_all
        test    byte ptr [si+SFT_DEVINFO],DIF_RAW
        jz      dwr_cooked
        mov     al,DVC_WRITE
        call    drv_xfer
        jc      dwr_err
        movzx   ecx,cx
        jmp     short dwr_out
dwr_cooked:
        xor     dx,dx
dwr_byte:
        cmp     byte ptr es:[di],1Ah    ; a ^Z ends it, unwritten
        je      dwr_cookdone
        push    cx
        mov     cx,1
        mov     al,DVC_WRITE
        call    drv_xfer
        mov     ax,cx
        pop     cx
        jc      dwr_err2
        or      ax,ax
        jz      dwr_cookdone
        inc     di
        inc     dx
        loop    dwr_byte
dwr_cookdone:
        movzx   ecx,dx
        jmp     short dwr_out
dwr_all:
        movzx   ecx,cx
        jmp     short dwr_out
dwr_none:
        xor     ecx,ecx
dwr_out:
        pop     di
        pop     dx
        clc
        ret
dwr_err2:
        mov     ax,word ptr [rd_drvst]
dwr_err:
        call    drv_err
        pop     di
        pop     dx
        stc
        ret

;=====================================================================
; drv_coninit - a driver CONFIG.SYS loaded named CON, that says it is
;               standard output, becomes this DOS's console: everything
;               con_out writes goes through it (drv_putc), and rd_sysinit
;               points the list of lists' console at it, as DOS does.
;               Called from rd_sysinit after rd_chain.  -> [rd_condrv],
;               0 if there is none.  Everything preserved.
;
; ANSI.SYS is that driver, and it is the reason: a DOS box under
; Windows prints through this DOS, and an escape sequence written there
; reached the BIOS's teletype as text.  Only the OUTPUT goes through
; it.  The keyboard is still read through the BIOS (con_in), so a key
; ANSI.SYS was told to reassign is not reassigned under Windows.
;=====================================================================
drv_coninit:
        pusha
        push    es
        mov     dword ptr [rd_condrv],0
        mov     si,OFFSET rdc_name
        mov     di,OFFSET rd_devname
        push    cs
        pop     es
        mov     cx,8
        cld
        rep     movsb
        call    drv_find
        jc      dci_out
        les     di,dword ptr [rd_drvhdr]
        test    word ptr es:[di+4],0002h        ; standard output
        jz      dci_out
        mov     eax,dword ptr [rd_drvhdr]
        mov     dword ptr [rd_condrv],eax
dci_out:
        pop     es
        popa
        ret
rdc_name    db  'CON     '

;=====================================================================
; drv_putc - DL = a character, written to the console driver.  A
;            request a byte, with the byte just past the packet.
;            Everything preserved; there is no error to give anybody.
;=====================================================================
drv_putc:
        pusha
        push    es
        push    fs
        sub     sp,RQ_ROOM + 2
        mov     bp,sp
        call    rq_zero
        mov     byte ptr [bp+RQ_ROOM],dl
        mov     byte ptr [bp+RQ_LEN],RQ_RWLEN
        mov     byte ptr [bp+RQ_CMD],DVC_WRITE
        lea     ax,[bp+RQ_ROOM]
        mov     word ptr [bp+RQ_BUFOFF],ax
        mov     word ptr [bp+RQ_BUFSEG],ss
        mov     word ptr [bp+RQ_COUNT],1
        lfs     di,dword ptr cs:[rd_condrv]
        mov     ax,ss
        mov     es,ax
        mov     bx,bp
        call    drv_call
        add     sp,RQ_ROOM + 2
        pop     fs
        pop     es
        popa
        ret

;=====================================================================
; rd_drvraw - rd_raw for a unit of a block driver CONFIG.SYS loaded:
;             EAX = the first sector, from the unit's sector 0, ECX =
;             how many, ES:BX = the buffer, [rd_rw], and rd_xfer's
;             copies of the volume (rd_dhdr, rd_dunit, rd_dmedia).  CF
;             on error.  Every register kept, as rd_raw keeps them.
;
; THE KERNEL LOADED IT, GAVE ITS UNITS LETTERS AND READ THEM THROUGH
; IT (DEVLOAD.INC, the block half), and until 2026-10-06 told this DOS
; the letters were not there: a RAM disk made in CONFIG.SYS vanished
; under Windows.  The driver is still resident, its header still in
; the chain; what was missing was a DOS that would send it the packet.
;
; In runs that do not cross 64K in memory, as rd_raw cuts its own and
; as DOS hands a driver its transfers, and with the DOS 4 sector number
; past 65535 - which only a driver with the bit in its header gets.
;=====================================================================
rd_drvraw:
        push    es
        pushad
rdd_one:
        or      ecx,ecx
        jz      rdd_ok
        ; ES:BX normalised, and the sectors that fit before 64K
        push    eax
        mov     ax,es
        movzx   eax,ax
        shl     eax,4
        movzx   edx,bx
        add     eax,edx
        mov     edx,eax
        shr     edx,4
        mov     es,dx
        mov     edx,eax
        and     dx,0Fh
        mov     bx,dx
        and     eax,0FFFFh
        neg     eax
        add     eax,10000h
        shr     eax,9
        jnz     rdd_fit
        inc     eax
rdd_fit:
        cmp     eax,ecx
        jbe     rdd_fit2
        mov     eax,ecx
rdd_fit2:
        cmp     eax,127
        jbe     rdd_fit3
        mov     eax,127
rdd_fit3:
        mov     edx,eax                 ; EDX = this run
        pop     eax

        call    rdd_req                 ; EAX first, DX count, ES:BX
        jc      rdd_fail
        add     eax,edx
        sub     ecx,edx
        shl     dx,9
        add     bx,dx                   ; past what moved; renormalised
        jmp     short rdd_one           ; at the top of the next run
rdd_ok:
        popad
        pop     es
        clc
        ret
rdd_fail:
        popad
        pop     es
        stc
        ret

;--- rdd_req - one packet: EAX = the first sector, DX = how many, ES:BX
;--- = the buffer.  CF if the driver failed or moved short.  Everything
;--- preserved.
rdd_req:
        pushad
        push    es
        push    fs
        mov     si,es                   ; the buffer's segment, for later
        sub     sp,RQ_ROOM
        mov     bp,sp
        call    rq_zero
        mov     byte ptr [bp+RQ_LEN],RQ_BLKLEN
        mov     cl,[rd_dunit]
        mov     byte ptr [bp+RQ_UNITB],cl
        mov     byte ptr [bp+RQ_CMD],DVC_READ
        cmp     byte ptr [rd_rw],0
        je      rdq_rd
        mov     byte ptr [bp+RQ_CMD],DVC_WRITE
rdq_rd:
        mov     cl,[rd_dmedia]
        mov     byte ptr [bp+RQ_MEDIA],cl
        mov     word ptr [bp+RQ_BUFOFF],bx
        mov     word ptr [bp+RQ_BUFSEG],si
        mov     word ptr [bp+RQ_COUNT],dx
        lfs     di,dword ptr [rd_dhdr]
        cmp     eax,0FFFFh
        jb      rdq_word
        test    word ptr fs:[di+4],DVA_BIG
        jz      rdq_bad                 ; past what this driver can name
        mov     word ptr [bp+RQ_SECTOR],0FFFFh
        mov     dword ptr [bp+RQ_HUGE],eax
        jmp     short rdq_go
rdq_word:
        mov     word ptr [bp+RQ_SECTOR],ax
rdq_go:
        mov     ax,ss
        mov     es,ax
        mov     bx,bp
        call    drv_call
        jc      rdq_bad
        cmp     word ptr [bp+RQ_COUNT],dx
        jne     rdq_bad
        add     sp,RQ_ROOM
        pop     fs
        pop     es
        popad
        clc
        ret
rdq_bad:
        add     sp,RQ_ROOM
        pop     fs
        pop     es
        popad
        stc
        ret

rd_drvhdr   dd  0                       ; the header drv_find found
rd_drvst    dw  0                       ; and the last status a byte got
rd_condrv   dd  0                       ; a loaded CON this DOS writes to
