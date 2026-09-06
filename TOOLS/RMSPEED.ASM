;=====================================================================
; RMSPEED.ASM - RMSPEED.COM, how long a DOS call actually takes.
;
; WHY THIS EXISTS.  Windows 3.1 standard mode runs on PM-DOS as of
; v0.54 and reaches Program Manager, and Program Manager comes up
; empty: the shell walks its program items for their icons and does
; not finish.  The trace build says why that reading was wrong.  It
; got 114 DOS calls into a Windows start-up in four hundred seconds -
; three and a half seconds a call - and the calls it did make are
; ordinary: open MOUSE.INI, seek to the end, seek back, read it,
; close, ask for the current directory, and again for SYSTEM.INI.
; Nothing is stuck.  Everything is SLOW, and an icon walk that costs a
; few thousand DOS calls at that rate is a walk that never ends inside
; a screenshot.
;
; So the question is no longer "what is Program Manager waiting for".
; It is "what does one DOS call cost, and where does the time go", and
; that is a question a program can answer in one boot without an
; emulator screenshot or a disk trace.
;
; THE SAME PROGRAM RUNS ON BOTH SIDES.  Started from the command line
; it measures PM-DOS's own 16-bit DOS in the VDM; started through
; PARKTEST it measures RMDOS, the real-mode DOS the kernel leaves
; behind, on bare metal.  Two numbers from one binary, and the
; difference between them is the whole answer to whether this is
; RMDOS's problem or the machine's.
;
; TICKS, NOT MICROSECONDS.  INT 1Ah counts at 18.2 Hz, which is coarse
; and is the only clock every DOS in this project already serves.  The
; loops below are sized so that the coarse clock is not the limit.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_RS     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RS, DS:_RS, ES:_RS, SS:_RS
        ORG     0

; AND THE NAMES ARE NOT n_ini AND N_INI.  wasm folds case, so a label
; called n_ini and a constant called N_INI are ONE SYMBOL, the EQU
; wins, and "mov dx,OFFSET n_ini" quietly assembles to "mov dx,20".
; The program then opened a file at offset 20 of its own PSP and
; reported "file not found, code 2" from BOTH DOSes, which reads
; exactly like a path-walking bug in the thing being measured.
; RMDBG.INC has the same note about rdbg_2fn next to RDBG_2FN; this is
; the second time and it cost a boot.
N_NULL  EQU     20000                    ; AH=30h, which touches no disk
N_I13   EQU     2000                    ; the same sector, through the BIOS
N_OPEN  EQU     200                     ; open + close, which walks a path
N_INI   EQU     100                     ; the whole of what Windows does
                                        ; to read a profile file

start:
        mov     dx,OFFSET m_hi
        call    puts

;--- a call that cannot touch the disk -------------------------------
; AH=30h is answered out of a byte in the DOS's own data, so what this
; measures is the cost of ARRIVING: the interrupt, the frame, the
; dispatch and the way back.  Under Windows there is a mode switch in
; there as well, which this program cannot see - but if the floor is
; already high, nothing above it can be low.
        mov     dx,OFFSET m_null
        call    puts
        call    ticks
        push    ax
        mov     cx,N_NULL
rs_null:
        push    cx
        mov     ah,30h
        int     21h
        pop     cx
        loop    rs_null
        call    ticks
        pop     bx
        sub     ax,bx
        mov     bx,N_NULL
        call    report

;--- and one sector off the disk, through the BIOS -------------------
; THE FLOOR UNDER EVERYTHING ELSE.  RMDOS reads ONE SECTOR PER CALL TO
; THE BIOS, deliberately and with a comment saying that what it costs
; is speed on a path that runs for a few seconds in the life of a
; machine.  That was true when the only thing on the other side of the
; park was a test program.  It is not true now: it is the path Windows
; runs on for its whole life, and an open walks a directory a sector
; at a time before the file is even reached.  So the number that
; matters is what one of those calls costs, and it is asked here of
; the BIOS directly, with no DOS in the way.
;
; A DIFFERENT SECTOR EVERY TIME.  Reading the same one two thousand
; times measures the emulator's cache and nothing else; walking up the
; track makes the drive - or 86Box's model of a year-2000 7200rpm one,
; which is what this VM is configured with - do the work a directory
; scan makes it do.
        mov     dx,OFFSET m_i13
        call    puts
        call    ticks
        push    ax
        mov     cx,N_I13
        mov     word ptr [i13sec],1
rs_i13:
        push    cx
        mov     ax,0201h                ; read, one sector
        mov     cx,word ptr [i13sec]    ; cylinder 0, sector 1..63
        mov     dx,0080h                ; head 0, drive 80h
        mov     bx,OFFSET buf
        push    es
        push    cs
        pop     es
        int     13h
        pop     es
        inc     word ptr [i13sec]
        cmp     word ptr [i13sec],63
        jbe     rs_i13next
        mov     word ptr [i13sec],1
rs_i13next:
        pop     cx
        loop    rs_i13
        call    ticks
        pop     bx
        sub     ax,bx
        mov     bx,N_I13
        call    report

;--- open and close, which is a path walk each time ------------------
        mov     dx,OFFSET m_open
        call    puts
        call    ticks
        push    ax
        mov     cx,N_OPEN
rs_open:
        push    cx
        mov     dx,OFFSET f_sysini
        mov     ax,3D00h
        int     21h
        jc      rs_openbad
        mov     bx,ax
        mov     ah,3Eh
        int     21h
        pop     cx
        loop    rs_open
        call    ticks
        pop     bx
        sub     ax,bx
        mov     bx,N_OPEN
        call    report
        jmp     short rs_ini
rs_openbad:
        mov     [errax],ax              ; the code, before puts loses it
        pop     cx
        pop     bx
        mov     dx,OFFSET m_nofile
        call    puts
        mov     ax,[errax]
        call    put5
        mov     dx,OFFSET m_crlf
        call    puts
        jmp     rs_done

;--- and the whole of a profile read, which is what Windows does -----
; Open, seek to the end for the size, seek back to the start, read the
; lot, close.  Straight out of the trace: it is what KERNEL's profile
; code does to SYSTEM.INI and MOUSE.INI, over and over, and it is the
; unit the icon walk is made of.
rs_ini:
        mov     dx,OFFSET m_ini
        call    puts
        call    ticks
        push    ax
        mov     cx,N_INI
rs_iniloop:
        push    cx
        mov     dx,OFFSET f_sysini
        mov     ax,3D00h
        int     21h
        jc      rs_inibad
        mov     [hnd],ax
        mov     bx,ax
        mov     ax,4202h
        xor     cx,cx
        xor     dx,dx
        int     21h
        mov     bx,[hnd]
        mov     ax,4200h
        xor     cx,cx
        xor     dx,dx
        int     21h
        mov     bx,[hnd]
        mov     cx,2000h
        mov     dx,OFFSET buf
        mov     ah,3Fh
        int     21h
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        pop     cx
        loop    rs_iniloop
        call    ticks
        pop     bx
        sub     ax,bx
        mov     bx,N_INI
        call    report
        jmp     short rs_done
rs_inibad:
        pop     cx
        pop     bx
        mov     dx,OFFSET m_nofile
        call    puts

rs_done:
        mov     dx,OFFSET m_bye
        call    puts
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; report - AX = ticks, BX = how many of whatever it was.  Prints the
;          ticks and then milliseconds each, which is the number that
;          means something: a tick is 54.925 ms, and the rounding to
;          55 is smaller than anything this is going to be read for.
;---------------------------------------------------------------------
report:
        push    ax
        push    bx
        push    dx
        push    ax
        call    put5
        mov     dx,OFFSET m_ticks
        call    puts
        pop     ax
        mov     dx,55                   ; ticks -> milliseconds
        mul     dx                      ; DX:AX = the whole run in ms
        div     bx                      ; AX = ms each; BX is small, so
                                        ; the quotient cannot overflow
        call    put5
        mov     dx,OFFSET m_each
        call    puts
        pop     dx
        pop     bx
        pop     ax
        ret

ticks:
        push    cx
        push    dx
        xor     ah,ah
        int     1Ah
        mov     ax,dx
        pop     dx
        pop     cx
        ret

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

;--- AX in decimal, five columns, leading blanks ---------------------
put5:
        pusha
        mov     di,OFFSET num + 4
        mov     bx,10
        mov     cx,5
p5_dig:
        xor     dx,dx
        div     bx
        add     dl,'0'
        mov     [di],dl
        dec     di
        loop    p5_dig
        mov     di,OFFSET num
        mov     cx,4
p5_blank:
        cmp     byte ptr [di],'0'
        jne     p5_out
        mov     byte ptr [di],' '
        inc     di
        loop    p5_blank
p5_out:
        mov     dx,OFFSET num
        mov     ah,09h
        int     21h
        popa
        ret

;---------------------------------------------------------------------
hnd         dw  0
i13sec      dw  1
errax       dw  0
num         db  '00000$'

m_hi        db  'RMSPEED - what does a DOS call cost here?',13,10,'$'
m_null      db  '20000 x AH=30h       $'
m_i13       db  ' 2000 x INT 13h read $'
m_open      db  '  200 x open+close   $'
m_ini       db  '  100 x profile read $'
m_ticks     db  ' ticks, $'
m_each      db  ' ms each',13,10,'$'
m_nofile    db  'C:\WINDOWS\SYSTEM.INI would not open, code ','$'
m_crlf      db  13,10,'$'
m_bye       db  'done.',13,10,'$'

f_sysini       db  'C:\WINDOWS\SYSTEM.INI',0

buf         db  8192 dup (0)

_RS     ENDS
        END     start
