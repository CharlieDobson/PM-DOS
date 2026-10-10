;=====================================================================
; LRGTEST.ASM - LRGTEST.COM, LRGDISK.386's real-mode start-up run
;               under plain DOS
;
;   LRGTEST [file]      file defaults to LRGDISK.386 in the current
;                       directory
;
; Everything LRGDISK.386 decides about the disks it decides in real
; mode, before Windows starts (CMD\LRGDISK\LRGDISK.ASM): it reads
; sectors through the BIOS, sees how the BIOS addressed the drive, reads
; them again itself and compares.  This runs those bytes - the real-mode
; object out of the .386 file itself, not a copy of its source - with
; this program standing in for WIN386's loader, so a machine can be
; asked "would LRGDISK take this disk, and how?" in seconds, without
; Windows.
;
; The stand-in loader: the object is read into memory of its own at a
; paragraph, entered at the file's EIP with DS = ES = its segment, AX =
; 030Bh (Windows for Workgroups 3.11), BX = 0 and SI = the environment;
; ECX points at a service routine that answers every SYSTEM.INI boolean
; "yes", so 32BitDiskAccess is on and LRGDISKREPORT is on - the report
; goes to the screen and to LRGDISK.TXT in the boot drive's root.  The
; start-up ends with a near RET, as WIN386 calls it; the return address
; pushed for it is a RETF placed just past the object, which brings it
; back here.  Then the answer: AX and the reference data in EDX.
;
; Linked with:  wlink form raw bin ... offset=0x100
;=====================================================================
        .386
_LT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_LT, DS:_LT, ES:_LT, SS:_LT
        ORG     0

start:
        mov     sp,OFFSET stack_top
        mov     dx,OFFSET m_hello
        call    say

        ; the file name: the command tail's first word, or LRGDISK.386
        mov     si,81h
        mov     di,OFFSET fname
lt_skip:
        lodsb
        cmp     al,' '
        je      lt_skip
        cmp     al,9
        je      lt_skip
        cmp     al,13
        je      lt_default
lt_copy:
        stosb
        lodsb
        cmp     al,' '
        je      lt_named
        cmp     al,9
        je      lt_named
        cmp     al,13
        jne     lt_copy
lt_named:
        mov     byte ptr [di],0
lt_default:

        mov     ax,3D00h
        mov     dx,OFFSET fname
        int     21h
        jnc     lt_open
        mov     dx,OFFSET m_noopen
        jmp     fail
lt_open:
        mov     [handle],ax

        ; the MZ header's pointer to the LE header, and the LE header
        xor     eax,eax
        call    seek
        mov     cx,40h
        mov     dx,OFFSET hdr
        call    read
        jc      bad
        cmp     word ptr [hdr],'ZM'
        jne     bad
        mov     eax,dword ptr [hdr+3Ch]
        mov     [le_off],eax
        call    seek
        mov     cx,0C4h
        mov     dx,OFFSET hdr
        call    read
        jc      bad
        cmp     word ptr [hdr],'EL'
        jne     bad

        ; the start object's entry in the object table
        mov     eax,dword ptr [hdr+18h]         ; the EIP's object
        or      eax,eax
        jz      bad
        dec     eax
        imul    eax,eax,24
        add     eax,dword ptr [hdr+40h]
        add     eax,[le_off]
        mov     ebx,dword ptr [hdr+1Ch]         ; EIP
        mov     [eip],bx
        call    seek
        mov     cx,24
        mov     dx,OFFSET obj
        call    read
        jc      bad
        test    word ptr [obj+8],2000h          ; must be 16-bit
        jnz     bad
        mov     eax,dword ptr [obj]             ; its size
        cmp     eax,0FF00h
        ja      bad
        mov     [osize],ax

        ; memory for it: past this program, from a paragraph
        mov     ax,OFFSET prog_end
        add     ax,15
        shr     ax,4
        mov     bx,cs
        add     ax,bx
        mov     [oseg],ax
        mov     bx,ax                           ; the end of it must be
        add     bx,1000h                        ; inside what DOS gave us
        cmp     bx,word ptr ds:[2]
        jbe     lt_room
        mov     dx,OFFSET m_nomem
        jmp     fail
lt_room:
        ; and the rest given back to DOS: the start-up asks DOS for a
        ; block of its own, as it does under WIN386
        push    ax
        mov     ax,cs
        sub     bx,ax
        mov     ah,4Ah
        int     21h
        pop     ax
        ; zeroed - the pages in the file stop at the last byte written
        push    es
        mov     es,ax
        xor     di,di
        mov     cx,8000h
        xor     ax,ax
        rep     stosw
        pop     es

        ; and its pages read in: object page N is the file's data page
        ; N, page size each, the module's last one shorter
        mov     eax,dword ptr [obj+0Ch]         ; its first page
        mov     [pgnum],eax
        mov     word ptr [where],0
        movzx   ecx,word ptr [osize]
        mov     [left],ecx
lt_page:
        cmp     dword ptr [left],0
        je      lt_loaded
        mov     eax,[pgnum]
        dec     eax
        imul    eax,dword ptr [hdr+28h]
        add     eax,dword ptr [hdr+80h]
        call    seek
        mov     ecx,dword ptr [hdr+28h]
        mov     eax,[pgnum]
        cmp     eax,dword ptr [hdr+14h]
        jne     lt_full
        mov     ecx,dword ptr [hdr+2Ch]         ; the module's last page
lt_full:
        cmp     ecx,[left]
        jbe     lt_part
        mov     ecx,[left]
lt_part:
        sub     [left],ecx
        push    ds
        mov     dx,[where]
        add     [where],cx
        mov     ax,[oseg]
        mov     bx,[handle]
        mov     ds,ax
        mov     ah,3Fh
        int     21h
        pop     ds
        jc      bad
        inc     dword ptr [pgnum]
        jmp     lt_page
lt_loaded:
        mov     ah,3Eh
        mov     bx,[handle]
        int     21h

        ; the RETF the start-up's RET comes back through
        push    es
        mov     es,[oseg]
        mov     bx,[osize]
        mov     byte ptr es:[bx],0CBh
        pop     es

        mov     dx,OFFSET m_run
        call    say

        ; and in, as WIN386 goes in
        mov     word ptr [svc],OFFSET loader
        mov     word ptr [svc+2],cs
        mov     ecx,dword ptr [svc]
        mov     si,word ptr ds:[2Ch]            ; the environment
        mov     ax,[oseg]
        mov     word ptr [target+2],ax
        mov     ax,[eip]
        mov     word ptr [target],ax
        push    cs
        push    OFFSET back
        push    word ptr [osize]                ; the near RET's target
        mov     ax,[oseg]
        mov     ds,ax
        mov     es,ax
        mov     ax,030Bh
        xor     bx,bx
        xor     edx,edx
        jmp     dword ptr cs:[target]
back:
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     [r_ax],ax
        mov     [r_edx],edx
        mov     dx,OFFSET m_ax
        call    say
        movzx   eax,word ptr [r_ax]
        mov     cl,4
        call    hexout
        mov     dx,OFFSET m_edx
        call    say
        mov     eax,[r_edx]
        mov     cl,8
        call    hexout
        mov     dx,OFFSET m_crlf
        call    say
        cmp     word ptr [r_ax],0       ; loading: a byte a unit
        jne     lt_end
        mov     dx,OFFSET m_units
        call    say
        xor     bx,bx
lt_unit:
        mov     dx,OFFSET m_unit
        call    say
        mov     eax,ebx
        mov     cl,1
        call    hexout
        mov     dx,OFFSET m_uflags
        call    say
        mov     eax,[r_edx]
        mov     cx,bx
        shl     cx,3
        shr     eax,cl
        and     eax,0FFh
        mov     cl,2
        call    hexout
        mov     dx,OFFSET m_udisk
        call    say
        mov     eax,[r_edx]
        mov     cx,bx
        shl     cx,3
        shr     eax,cl
        shr     eax,5
        and     eax,3
        add     eax,80h
        mov     cl,2
        call    hexout
        mov     dx,OFFSET m_crlf
        call    say
        inc     bx
        cmp     bx,4
        jb      lt_unit
lt_end:
        push    cs
        pop     es
        mov     ax,4C00h
        int     21h

bad:
        mov     dx,OFFSET m_bad
fail:
        call    say
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; loader - the stand-in for WIN386's real-mode service entry: every
; Get_Profile_Boolean (AX=3) says yes, anything else no
;---------------------------------------------------------------------
loader:
        xor     cx,cx
        cmp     ax,3
        jne     ld_out
        inc     cx
ld_out:
        retf

;---------------------------------------------------------------------
; seek (EAX = file offset), read (CX bytes to DS:DX; CF if short),
; say (DX -> $-text), hexout (EAX, CL digits)
;---------------------------------------------------------------------
seek:
        push    bx
        mov     dx,ax
        shr     eax,16
        mov     cx,ax
        mov     ax,4200h
        mov     bx,[handle]
        int     21h
        pop     bx
        ret

read:
        push    bx
        push    cx
        mov     ah,3Fh
        mov     bx,[handle]
        int     21h
        pop     cx
        jc      rd_out
        cmp     ax,cx
rd_out:
        pop     bx
        ret

say:
        mov     ah,9
        int     21h
        ret

hexout:
        movzx   cx,cl
ho_digit:
        push    cx
        push    eax
        dec     cx
        shl     cx,2
        shr     eax,cl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ho_put
        add     al,'A'-'0'-10
ho_put:
        mov     dl,al
        mov     ah,2
        int     21h
        pop     eax
        pop     cx
        loop    ho_digit
        ret

;---------------------------------------------------------------------
m_hello db      'LRGTEST - LRGDISK.386', 27h, 's real-mode start-up under '
        db      'DOS', 13, 10, '$'
m_noopen db     'Cannot open the file.', 13, 10, '$'
m_bad   db      'That is not a VxD with a 16-bit start-up object.', 13, 10, '$'
m_nomem db      'Not enough memory.', 13, 10, '$'
m_run   db      '---', 13, 10, '$'
m_ax    db      '--- AX=$'
m_edx   db      ' EDX=$'
m_crlf  db      13, 10, '$'
m_units db      'a byte a unit: bit 0 ours, 1 LBA, 2 LBA48, 3 something else, 4 DMA, 5-6 the BIOS disk', 13, 10, '$'
m_unit  db      'unit $'
m_uflags db     ': $'
m_udisk db      ' disk $'

fname   db      'LRGDISK.386', 0
        db      80 dup (0)
handle  dw      0
le_off  dd      0
eip     dw      0
osize   dw      0
oseg    dw      0
where   dw      0
pgnum   dd      0
left    dd      0
svc     dd      0
target  dd      0
r_ax    dw      0
r_edx   dd      0
obj     db      24 dup (0)
hdr     db      0C4h dup (0)
        db      512 dup (0)
stack_top LABEL WORD
prog_end LABEL BYTE
_LT     ENDS
        END     start
