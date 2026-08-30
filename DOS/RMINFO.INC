;=====================================================================
; RMINFO.INC - the calls that only answer, and the structures they
;              answer with
;
; Nothing here touches a disk.  What it does do is publish the shapes
; a DOS is expected to have: the swappable data area, the list of
; lists, the country block.  Windows reads all three within a second
; of starting, and a DOS that has them wrong is a DOS Windows declines
; to run on rather than one it runs on badly.
;=====================================================================

;--- the swappable data area, laid out as MS-DOS lays it out.  Same
;--- fields, same order, same reasons as the kernel's own - see the
;--- SDA block in PMDOS.INC, which took its layout from the DOS 6
;--- source and this one copies.
SD_ERRMODE      EQU 00h
SD_INDOS        EQU 01h
SD_WPERR        EQU 02h
SD_LOCUS        EQU 03h
SD_EXTERR       EQU 04h
SD_ACTION       EQU 06h
SD_CLASS        EQU 07h
SD_ERRPTR       EQU 08h
SD_DTA          EQU 0Ch
SD_PSP          EQU 10h
SD_CONCSP       EQU 12h
SD_EXITCODE     EQU 14h
SD_DRIVE        EQU 16h
SD_BREAK        EQU 17h
SD_CPSW         EQU 18h
SD_CPSWSAVE     EQU 19h
SD_LEN          EQU 1Ah

;--- the list of lists, in the shape a real-mode program walks -------
LL_MCBSEG       EQU 00h                 ; dw, and it is at [ES:BX-2]
LL_PTR          EQU 02h                 ; where ES:BX points
LL_DPB          EQU 02h                 ; dd
LL_SFT          EQU 06h                 ; dd
LL_CLOCK        EQU 0Ah                 ; dd
LL_CON          EQU 0Eh                 ; dd
LL_MAXSEC       EQU 12h                 ; dw
LL_BUFFERS      EQU 14h                 ; dd
LL_CDS          EQU 18h                 ; dd
LL_FCB          EQU 1Ch                 ; dd
LL_PROTFCB      EQU 20h                 ; dw
LL_NBLOCK       EQU 22h                 ; db
LL_LASTDRV      EQU 23h                 ; db
LL_NUL          EQU 24h                 ; the NUL header, and the chain head
LL_SIZE         EQU 24h + 18 + 1

;=====================================================================
; rd_sysinit - build the published structures.
;=====================================================================
rd_sysinit:
        pusha
        push    es
        mov     ax,cs
        mov     es,ax

        mov     di,OFFSET rd_sda
        mov     cx,SD_LEN
        xor     al,al
        cld
        rep     stosb
        mov     byte ptr [rd_sda + SD_WPERR],0FFh       ; as MS-DOS does:
                                                        ; drive zero is real

        mov     di,OFFSET rd_lol
        mov     cx,LL_SIZE
        xor     al,al
        rep     stosb

        mov     ax,[h_mcbhead]
        mov     word ptr [rd_lol + LL_MCBSEG],ax
        mov     dword ptr [rd_lol + LL_DPB],0FFFFFFFFh
        mov     dword ptr [rd_lol + LL_SFT],0FFFFFFFFh
        mov     dword ptr [rd_lol + LL_CLOCK],0FFFFFFFFh
        mov     dword ptr [rd_lol + LL_BUFFERS],0FFFFFFFFh
        mov     dword ptr [rd_lol + LL_CDS],0FFFFFFFFh
        mov     dword ptr [rd_lol + LL_FCB],0FFFFFFFFh
        mov     word ptr [rd_lol + LL_MAXSEC],SECSIZE
        mov     al,[h_ndrv]
        mov     [rd_lol + LL_NBLOCK],al
        mov     byte ptr [rd_lol + LL_LASTDRV],LASTDRIVE

        ; The CON device the list of lists points at is the NUL header
        ; that follows it, which is the head of the chain - the same
        ; single real terminated entry PM-DOS publishes, and for the
        ; same reason: a walker starts here and has to be able to stop.
        mov     ax,cs
        mov     word ptr [rd_lol + LL_CON + 2],ax
        mov     word ptr [rd_lol + LL_CON],OFFSET rd_lol + LL_NUL

        mov     di,OFFSET rd_lol
        add     di,LL_NUL
        mov     dword ptr [di],0FFFFFFFFh       ; no next device
        mov     word ptr [di+4],8004h           ; character, NUL
        mov     word ptr [di+6],OFFSET rd_devret
        mov     word ptr [di+8],OFFSET rd_devret
        mov     dword ptr [di+0Ah],'LUN '
        mov     dword ptr [di+0Eh],'    '

        ; Every drive starts at its own root, with an empty path.
        mov     di,OFFSET rd_cds
        mov     cx,LASTDRIVE * CDS_LEN
        xor     al,al
        rep     stosb

        pop     es
        popa
        ret

rd_devret:
        retf                            ; what both driver entries point at

;=====================================================================
; 2Ah - the date -> CX = year, DH = month, DL = day, AL = weekday
; 2Ch - the time -> CH:CL = hours:minutes, DH:DL = seconds:hundredths
;
; Both out of the CMOS through the BIOS, because there is no clock
; driver here and inventing one would mean keeping a tick count that
; nothing increments.
;=====================================================================
fn2A:
        mov     ah,04h
        int     1Ah                     ; CH=century CL=year DH=month DL=day
        jc      f2A_none
        mov     al,ch
        call    bcd2bin
        mov     bl,al
        mov     al,cl
        call    bcd2bin
        mov     bh,al
        movzx   ax,bl
        mov     cx,100
        mul     cx
        movzx   bx,bh
        add     ax,bx
        mov     [bp+F_CX],ax
        mov     al,dh
        call    bcd2bin
        mov     byte ptr [bp+F_DX+1],al
        mov     al,dl
        call    bcd2bin
        mov     byte ptr [bp+F_DX],al
        mov     byte ptr [bp+F_AX],0    ; the weekday, which nothing checks
        jmp     i21_ok
f2A_none:
        mov     word ptr [bp+F_CX],1980
        mov     word ptr [bp+F_DX],0101h
        mov     byte ptr [bp+F_AX],2
        jmp     i21_ok

fn2C:
        mov     ah,02h
        int     1Ah                     ; CH=hours CL=minutes DH=seconds
        jc      f2C_none
        mov     al,ch
        call    bcd2bin
        mov     byte ptr [bp+F_CX+1],al
        mov     al,cl
        call    bcd2bin
        mov     byte ptr [bp+F_CX],al
        mov     al,dh
        call    bcd2bin
        mov     byte ptr [bp+F_DX+1],al
        mov     byte ptr [bp+F_DX],0
        jmp     i21_ok
f2C_none:
        mov     word ptr [bp+F_CX],0
        mov     word ptr [bp+F_DX],0
        jmp     i21_ok

;--- 2Bh set date, 2Dh set time.  Refused rather than pretended: the
;--- excursion is minutes long and a program that sets the clock here
;--- and finds it unset afterwards is worse off than one told no.
fn2B:
fn2D:
        mov     byte ptr [bp+F_AX],0FFh
        jmp     i21_ok

bcd2bin:
        push    cx
        mov     ah,al
        and     al,0Fh
        shr     ah,4
        mov     cl,10
        push    ax
        mov     al,ah
        mul     cl
        pop     cx
        and     cx,0Fh
        add     al,cl
        pop     cx
        ret


;=====================================================================
; 2Eh - set the verify flag.  AL = 0 off, 1 on.
;
; Remembered and reported and NOT acted on, which is what it amounts
; to here: verifying a write means reading the sector back, and every
; write in this DOS already goes through one BIOS call per sector on a
; path that runs for minutes.  A program that turns it on and reads it
; back through 54h gets a consistent answer.
;=====================================================================
fn2E:
        mov     al,byte ptr [bp+F_AX]
        mov     [rd_verify],al
        jmp     i21_ok
;=====================================================================
; 25h - set the vector in AL to DS:DX
; 35h - and read it back into ES:BX
;=====================================================================
fn25:
        push    es
        movzx   di,byte ptr [bp+F_AX]
        shl     di,2
        xor     ax,ax
        mov     es,ax
        mov     ax,[bp+F_DX]
        cli
        mov     es:[di],ax
        mov     ax,[bp+F_DS]
        mov     es:[di+2],ax
        sti
        pop     es
        jmp     i21_ok

fn35:
        push    es
        movzx   di,byte ptr [bp+F_AX]
        shl     di,2
        xor     ax,ax
        mov     es,ax
        mov     ax,es:[di]
        mov     [bp+F_BX],ax
        mov     ax,es:[di+2]
        mov     [bp+F_ES],ax
        pop     es
        jmp     i21_ok

;=====================================================================
; 30h - the version.  Six point twenty-two, which is what PM-DOS says
;       and what everything this is standing in for was written
;       against.  BH is the OEM; FFh is "not one of the ones with a
;       name", which is the truthful answer.
;=====================================================================
fn30:
        mov     word ptr [bp+F_AX],DOSVER_MINOR * 256 + DOSVER_MAJOR
        mov     word ptr [bp+F_BX],0FF00h
        mov     word ptr [bp+F_CX],0
        jmp     i21_ok

;=====================================================================
; 33h - the odds and ends DOS filed under one number
;=====================================================================
fn33:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jz      f33_get
        cmp     al,1
        je      f33_set
        cmp     al,5
        je      f33_boot
        cmp     al,6
        je      f33_true
        jmp     i21_unsup
f33_get:
        mov     al,[rd_brkchk]
        mov     byte ptr [bp+F_DX],al
        jmp     i21_ok
f33_set:
        mov     al,byte ptr [bp+F_DX]
        mov     [rd_brkchk],al
        jmp     i21_ok
f33_boot:
        mov     al,[h_bootdrv]
        inc     al                      ; 33h/05h counts A as one
        mov     byte ptr [bp+F_DX],al
        jmp     i21_ok
f33_true:
        mov     word ptr [bp+F_BX],DOSVER_MINOR * 256 + DOSVER_MAJOR
        mov     word ptr [bp+F_DX],0
        jmp     i21_ok

;=====================================================================
; 34h - the address of the InDOS flag.
;
; And it is the SECOND BYTE OF THE SWAPPABLE DATA AREA, exactly as it
; is in the kernel this is standing in for, so a TSR that finds the
; critical-error flag by subtracting one gets the right byte.
;=====================================================================
fn34:
        mov     ax,cs
        mov     [bp+F_ES],ax
        mov     word ptr [bp+F_BX],OFFSET rd_sda + SD_INDOS
        jmp     i21_ok

;=====================================================================
; 37h - the switch character.  A slash, and it cannot be set.
;=====================================================================
fn37:
        mov     al,byte ptr [bp+F_AX]
        or      al,al
        jnz     f37_other
        mov     byte ptr [bp+F_DX],'/'
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok
f37_other:
        cmp     al,1
        je      f37_ok
        jmp     i21_unsup
f37_ok:
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok

;=====================================================================
; 38h - country information.  One country, the United States, which is
;       the one PM-DOS answers too.
;=====================================================================
fn38:
        mov     ax,[bp+F_DX]
        cmp     ax,0FFFFh
        je      f38_set
        push    es
        mov     es,[bp+F_DS]
        mov     di,ax
        mov     si,OFFSET rd_country
        mov     cx,34
        push    ds
        push    cs
        pop     ds
        cld
        rep     movsb
        pop     ds
        pop     es
        mov     word ptr [bp+F_BX],1    ; the country code
        jmp     i21_ok
f38_set:
        jmp     i21_ok                  ; there is only one to set it to

;=====================================================================
; 52h - the list of lists.  ES:BX, and the word BELOW it is the first
;       memory control block, which is the one field here that has
;       meant the same thing in every DOS since 2.0 and the one a
;       memory tool is really after.
;=====================================================================
fn52:
        mov     ax,cs
        mov     [bp+F_ES],ax
        mov     word ptr [bp+F_BX],OFFSET rd_lol + LL_PTR
        jmp     i21_ok

;=====================================================================
; 54h - the verify flag
;=====================================================================
fn54:
        mov     al,[rd_verify]
        mov     byte ptr [bp+F_AX],al
        jmp     i21_ok

;=====================================================================
; 59h - the last error, with its class, action and locus.
;
; Derived from the code rather than stored, exactly as the kernel does
; it - which is why there is a table here and not three variables.
;=====================================================================
fn59:
        mov     ax,[rd_lasterr]
        mov     [bp+F_AX],ax
        mov     si,OFFSET rd_errtab
f59_scan:
        movzx   cx,byte ptr [si]
        or      cx,cx
        jz      f59_unknown
        cmp     cx,ax
        je      f59_have
        add     si,4
        jmp     short f59_scan
f59_unknown:
        mov     si,OFFSET rd_errunk
f59_have:
        mov     bh,[si+1]
        mov     bl,[si+2]
        mov     [bp+F_BX],bx
        mov     ch,[si+3]
        mov     byte ptr [bp+F_CX+1],ch
        jmp     i21_ok

rd_errtab:
        db  ERR_FUNC,      07h, 05h, 01h
        db  ERR_NOFILE,    08h, 03h, 02h
        db  ERR_NOPATH,    08h, 03h, 02h
        db  ERR_NOHANDLES, 01h, 04h, 01h
        db  ERR_ACCESS,    03h, 04h, 02h
        db  ERR_BADHANDLE, 07h, 04h, 01h
        db  ERR_BADMCB,    04h, 05h, 05h
        db  ERR_NOMEM,     01h, 04h, 05h
        db  ERR_BADBLOCK,  07h, 05h, 05h
        db  ERR_EXEFMT,    09h, 04h, 01h
        db  ERR_BADDRIVE,  08h, 03h, 02h
        db  ERR_EXISTS,    0Ch, 04h, 02h
        db  0
rd_errunk:
        db  0, 0Dh, 07h, 01h

;=====================================================================
; 5Dh - and the one subfunction of it that matters here.
;
;   AL=06h  the swappable data area -> DS:SI, CX its length, DX how
;           much of it must always be swapped
;
; The consumer this exists for is Windows' own DOSMGR, which asks for
; this within moments of starting so it can give every virtual machine
; its own copy of DOS's per-process state.  A DOS that cannot answer
; is a DOS Windows will not instance, and one DOS box's current
; directory becomes every DOS box's current directory.
;=====================================================================
fn5D:
        mov     al,byte ptr [bp+F_AX]
        cmp     al,06h
        je      f5D_sda
        jmp     i21_unsup
f5D_sda:
        call    sda_sync
        mov     ax,cs
        mov     [bp+F_DS],ax
        mov     word ptr [bp+F_SI],OFFSET rd_sda
        mov     word ptr [bp+F_CX],SD_LEN
        mov     word ptr [bp+F_DX],SD_LEN
        jmp     i21_ok

;--- sda_sync - the fields whose master copy is elsewhere ------------
sda_sync:
        push    ax
        mov     ax,[rd_psp]
        mov     word ptr [rd_sda + SD_PSP],ax
        mov     ax,[rd_dtaoff]
        mov     word ptr [rd_sda + SD_DTA],ax
        mov     ax,[rd_dtaseg]
        mov     word ptr [rd_sda + SD_DTA + 2],ax
        mov     al,[rd_drive]
        mov     [rd_sda + SD_DRIVE],al
        mov     ax,[rd_lasterr]
        mov     word ptr [rd_sda + SD_EXTERR],ax
        mov     al,[rd_indos]
        mov     [rd_sda + SD_INDOS],al
        mov     al,[rd_errmode]
        mov     [rd_sda + SD_ERRMODE],al
        pop     ax
        ret

;=====================================================================
; 63h - the double-byte character set table.  There is none, and a
;       pointer to a zero word is how DOS says so.
;=====================================================================
fn63:
        mov     ax,cs
        mov     [bp+F_DS],ax
        mov     word ptr [bp+F_SI],OFFSET rd_dbcs
        mov     byte ptr [bp+F_AX],0
        jmp     i21_ok

;=====================================================================
; 65h - extended country information, and the two subfunctions of it
;       that do something rather than hand back a table.
;=====================================================================
fn65:
        mov     al,byte ptr [bp+F_AX]
        cmp     al,20h
        je      f65_upchar
        cmp     al,21h
        je      f65_upmem
        cmp     al,22h
        je      f65_upasciz
        jmp     i21_unsup
f65_upchar:
        mov     al,byte ptr [bp+F_DX]
        call    rd_upcase
        mov     byte ptr [bp+F_DX],al
        jmp     i21_ok
f65_upmem:
        push    es
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        mov     cx,[bp+F_CX]
        jcxz    f65_memdone
f65_memloop:
        mov     al,es:[di]
        call    rd_upcase
        mov     es:[di],al
        inc     di
        loop    f65_memloop
f65_memdone:
        pop     es
        jmp     i21_ok
f65_upasciz:
        push    es
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
f65_azloop:
        mov     al,es:[di]
        or      al,al
        jz      f65_azdone
        call    rd_upcase
        mov     es:[di],al
        inc     di
        jmp     short f65_azloop
f65_azdone:
        pop     es
        jmp     i21_ok

;=====================================================================
; INT 24h - the default critical error handler.
;
; It FAILS the call rather than asking.  A handler that prompted would
; be writing "Abort, Retry, Fail?" onto a screen that belongs to
; Windows, in a video mode that may not have text in it, and waiting
; for a keystroke from a keyboard Windows has taken over.  A program
; with its own handler installs one and never reaches this.
;=====================================================================
i24:
        mov     al,3                    ; fail
        iret

;=====================================================================
; INT 2Fh - the multiplex.
;
; AH=43h IS THE ONE THAT MATTERS.  Windows in 386 enhanced mode does
; not ask whether there is extended memory - it requires a driver, and
; AL=00h answering anything but 80h ends the conversation.  It answers
; 80h now, and AL=10h hands back the far pointer every XMS client
; calls from then on.  The driver behind it is RMXMS.INC.
;=====================================================================
i2F:
        cmp     ah,11h
        je      i2F_none                ; the network redirector: none
        cmp     ah,43h
        je      i2F_xms
        cmp     ax,1600h
        je      i2F_nowin
        cmp     ax,160Ah
        je      i2F_iret
        cmp     ax,4680h
        je      i2F_iret
        cmp     ah,12h
        je      i2F_none                ; DOS's own internal services
        iret
i2F_none:
        mov     al,0
        iret
i2F_xms:
        cmp     al,00h
        je      i2F_xmshere
        cmp     al,10h
        je      i2F_xmsaddr
        iret
i2F_xmshere:
        mov     al,80h                  ; an extended memory manager IS
        iret                            ; installed - see RMXMS.INC
i2F_xmsaddr:
        push    cs
        pop     es
        mov     bx,OFFSET rx_entry
        iret
i2F_nowin:
        mov     al,0                    ; 386 enhanced mode is not running -
        iret                            ; not yet, which is the point
i2F_iret:
        iret

;=====================================================================
; Data
;=====================================================================
rd_sda      db  SD_LEN dup(0)
rd_lol      db  LL_SIZE dup(0)
rd_dbcs     dw  0

rd_country:
        dw  0                           ; date format, month first
        db  '$',0,0,0,0                 ; currency
        db  ',',0                       ; thousands
        db  '.',0                       ; decimal
        db  '-',0                       ; date separator
        db  ':',0                       ; time separator
        db  0                           ; currency format
        db  2                           ; digits after the point
        db  0                           ; twelve-hour clock
        dd  0                           ; the case-map call, which is none
        db  ',',0                       ; list separator
        db  10 dup(0)
