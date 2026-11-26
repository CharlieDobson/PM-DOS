;=====================================================================
; SCRBACK.ASM - SCRBACK.COM: a box the kernel ends from a graphics
;               screen leaves a text screen behind it.
;
; When the kernel ends a program in a box - a fault, a HLT with
; interrupts off, a DPMI client's fault - it says why on the screen,
; and the console writes characters into the text page.  In a graphics
; mode nothing shows that page: the user saw the program's last picture
; and no message.  The kernel now sets the text mode the program
; started on first (DOS\VDM.INC, vdm_scrtext).  Each case ends its
; whole box, this program with it, so the check is a second run:
;
;   SCRBACK /G   mode 13h through the BIOS, then a store into the
;                kernel's read-only low pages, as VECBACK /G: a fault
;   SCRBACK /H   mode 13h, then a HLT with interrupts off: no report,
;                and the shell's prompt is next
;   SCRBACK /R   the BIOS left in mode 3 and the VGA's Graphics
;                Controller set to graphics by its own register, which
;                the BIOS is never told of; then the store
;   SCRBACK /D   mode 13h, then a #GP in protected mode as a DPMI
;                client with no handler for it: a HLT at ring 3
;   SCRBACK /V   run straight after one of those: the BIOS says a text
;                mode, and on a VGA the Graphics Controller says so too.
;                The mode and its rows are printed: the text mode is the
;                one the program started on, 50 lines included.
;
;   SCRBACK /T   a text screen with palette entry 1 changed, the old
;                value kept in SCRBACK.DAT; then the store
;   SCRBACK /K   straight after /T: the text screen was left alone -
;                palette entry 1 is still as /T set it, which a mode set
;                would have undone.  The entry is put back from the file
;                and the file is deleted.
;
; And a program that ends ITSELF in a graphics mode, which the kernel
; leaves alone, as MS-DOS does: the shell puts the screen back before
; its next prompt instead (21h/F0h AL=0Eh, from CMD.EXE and COMMAND.COM):
;
;   SCRBACK /N   mode 13h, then 4Ch
;   SCRBACK /W   the next line of the batch file: the screen is still in
;                mode 13h, because nothing between two programs puts it
;                back.  Then SCRBACK.KEY is written - "SCRBACK /V" and
;                "EXIT" - for a shell to read at its prompt:
;                CMD < SCRBACK.KEY, COMMAND < SCRBACK.KEY.  /V, run by
;                the shell, finds the text mode the prompt put back.
;
; A case still running after the instruction that should have ended
; the box says so, sets mode 3 and ends.  So does /V when the screen
; is not in a text mode, so that the tests after it can be read.  On a
; machine with no VGA there is nothing to try.  The exit code is the
; number of failures.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_SB     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SB, DS:_SB, ES:_SB, SS:_SB
        ORG     0

STACK_BYTES     EQU     512
LOW_STORE       EQU     11BDh           ; where the store goes, as VECBACK's
GC_IDX          EQU     3CEh            ; the VGA's Graphics Controller
GC_MISC         EQU     6               ; ...register 6, bit 0: graphics
PAL_ENTRY       EQU     1               ; the palette entry /T changes
MARK_R          EQU     3Fh             ; ...and what it changes it to
MARK_G          EQU     00h
MARK_B          EQU     3Fh
BDA_SEG         EQU     40h
BDA_ROWS        EQU     84h             ; the rows on the screen, less one
KEYLEN          EQU     18              ; keytext's bytes (checked at the end)

start:
;--- a letter in the tail ----------------------------------------------
        mov     si,81h
        mov     cl,byte ptr ds:[80h]
        xor     ch,ch
        jcxz    usage
arg_scan:
        mov     al,byte ptr [si]
        cmp     al,' '
        je      arg_skip
        cmp     al,'/'
        je      arg_skip
        cmp     al,'-'
        jne     arg_got
arg_skip:
        inc     si
        loop    arg_scan
usage:
        mov     dx,OFFSET m_usage
        call    puts
        mov     ax,4C01h
        int     21h
arg_got:
        and     al,0DFh
        cmp     al,'G'
        je      case_g
        cmp     al,'H'
        je      case_h
        cmp     al,'R'
        je      case_r
        cmp     al,'D'
        je      case_d
        cmp     al,'T'
        je      case_t
        cmp     al,'V'
        je      case_v
        cmp     al,'K'
        je      case_k
        cmp     al,'N'
        je      case_n
        cmp     al,'W'
        je      case_w
        jmp     usage

;=====================================================================
; The cases that end the box
;=====================================================================

;--- G: mode 13h, then a fault -------------------------------------------
case_g:
        mov     dx,OFFSET m_g
        call    puts
        call    set13
        jc      no_vga
        call    store
        jmp     still

;--- H: mode 13h, then a HLT with interrupts off --------------------------
case_h:
        mov     dx,OFFSET m_h
        call    puts
        call    set13
        jc      no_vga
        cli
        hlt
        sti
        jmp     still

;--- R: graphics by the register, the BIOS still saying mode 3 -----------
case_r:
        mov     dx,OFFSET m_r
        call    puts
        call    is_vga
        jc      no_vga
        mov     dx,GC_IDX
        mov     al,GC_MISC
        out     dx,al
        inc     dx                      ; the data port
        in      al,dx
        or      al,1
        out     dx,al
        call    store
        jmp     still

;--- D: mode 13h, then a DPMI client's #GP --------------------------------
case_d:
        mov     dx,OFFSET m_d
        call    puts
        call    shrink                  ; room for the host's data
        mov     ax,1687h
        int     2Fh
        or      ax,ax
        jnz     cd_none
        mov     word ptr [dp_entry],di
        mov     word ptr [dp_entry+2],es
        push    cs
        pop     es
        mov     bx,si                   ; the host's paragraphs
        or      bx,bx
        jz      cd_mode
        mov     ah,48h
        int     21h
        jc      cd_nomem
        mov     word ptr [dp_seg],ax
cd_mode:
        call    set13
        jc      no_vga
        mov     es,word ptr [dp_seg]
        xor     ax,ax                   ; a 16-bit client
        call    dword ptr [dp_entry]
        jc      cd_entry
        ; Protected mode, DS this segment's data selector: a HLT, which
        ; at ring 3 is a #GP.  Not a read through a null selector:
        ; DOSBox-X does not check segments and reads address 0.
        hlt
        jmp     still
cd_none:
        mov     dx,OFFSET m_nodpmi
        call    puts
        jmp     finish
cd_nomem:
        mov     dx,OFFSET m_dpmimem
        call    fail
        jmp     finish
cd_entry:
        push    cs
        pop     es
        call    set3
        mov     dx,OFFSET m_dpmient
        call    fail
        jmp     finish

;--- T: a text screen, with one palette entry changed ---------------------
case_t:
        mov     dx,OFFSET m_t
        call    puts
        call    is_vga
        jc      no_vga
        mov     ax,1015h                ; the entry as it is: DH red,
        mov     bx,PAL_ENTRY            ; CH green, CL blue
        int     10h
        mov     byte ptr [pal_old],dh
        mov     byte ptr [pal_old+1],ch
        mov     byte ptr [pal_old+2],cl
        mov     dx,OFFSET datname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      ct_nofile
        mov     bx,ax
        mov     dx,OFFSET pal_old
        mov     cx,3
        mov     ah,40h
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      ct_nofile
        cmp     ax,3
        jne     ct_nofile
        mov     ax,1010h
        mov     bx,PAL_ENTRY
        mov     dh,MARK_R
        mov     ch,MARK_G
        mov     cl,MARK_B
        int     10h
        call    store
        jmp     still
ct_nofile:
        mov     dx,OFFSET m_nowrite
        call    fail
        jmp     finish

;=====================================================================
; A program that ends itself in a graphics mode
;=====================================================================

;--- N: mode 13h, then 4Ch ------------------------------------------------
case_n:
        mov     dx,OFFSET m_n
        call    puts
        call    set13
        jc      no_vga
        jmp     finish

;--- W: still mode 13h; and the key file for the shell after this --------
case_w:
        mov     dx,OFFSET m_w
        call    puts
        call    is_vga
        jc      no_vga
        mov     ah,0Fh
        int     10h
        and     al,7Fh
        mov     dx,OFFSET m_still13
        cmp     al,13h
        jne     cw_moved
        call    pass
        jmp     short cw_key
cw_moved:
        call    fail
cw_key:
        mov     dx,OFFSET keyname
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      cw_nofile
        mov     bx,ax
        mov     dx,OFFSET keytext
        mov     cx,KEYLEN
        mov     ah,40h
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      cw_nofile
        cmp     ax,KEYLEN
        je      finish
cw_nofile:
        mov     dx,OFFSET m_nokey
        call    fail
        jmp     finish

; still - the box did not end: say so, and leave a text screen
still:
        call    set3
        mov     dx,OFFSET m_still
        call    fail
        jmp     finish

no_vga:
        mov     dx,OFFSET m_novga
        call    puts
        jmp     finish

;=====================================================================
; The checks, a run later
;=====================================================================

;--- V: a text screen -----------------------------------------------------
case_v:
        mov     dx,OFFSET m_v
        call    puts
        call    check_text
        jmp     finish

;--- K: the text screen as /T left it -------------------------------------
case_k:
        mov     dx,OFFSET datname
        mov     ax,3D00h
        int     21h
        jc      ck_noread
        mov     bx,ax
        mov     dx,OFFSET pal_old
        mov     cx,3
        mov     ah,3Fh
        int     21h
        pushf
        push    ax
        mov     ah,3Eh
        int     21h
        pop     ax
        popf
        jc      ck_noread
        cmp     ax,3
        jne     ck_noread
        mov     dx,OFFSET datname
        mov     ah,41h
        int     21h
        mov     dx,OFFSET m_k
        call    puts
        call    check_text
        call    is_vga
        jc      finish
        mov     ax,1015h
        mov     bx,PAL_ENTRY
        int     10h
        cmp     dh,MARK_R
        jne     ck_reset
        cmp     ch,MARK_G
        jne     ck_reset
        cmp     cl,MARK_B
        jne     ck_reset
        mov     dx,OFFSET m_kept
        call    pass
        jmp     ck_back
ck_reset:
        mov     dx,OFFSET m_kept
        call    fail
ck_back:
        mov     ax,1010h
        mov     bx,PAL_ENTRY
        mov     dh,byte ptr [pal_old]
        mov     ch,byte ptr [pal_old+1]
        mov     cl,byte ptr [pal_old+2]
        int     10h
        jmp     finish
ck_noread:
        mov     dx,OFFSET m_noread
        call    fail

finish:
        mov     dx,OFFSET m_done
        call    puts
        mov     ax,word ptr [nfail]
        call    putdec
        mov     dx,OFFSET m_fails
        call    puts
        mov     al,byte ptr [nfail]
        mov     ah,4Ch
        int     21h

;=====================================================================
; Helpers
;=====================================================================

; check_text - a line naming the mode and its rows, then: the BIOS
;              says a text mode, and on a VGA so does the Graphics
;              Controller.  A screen that fails either is set to mode 3.
check_text:
        mov     byte ptr [need3],0
        mov     dx,OFFSET m_mode
        call    puts
        mov     ah,0Fh
        int     10h                     ; AL = the mode
        and     al,7Fh
        mov     byte ptr [cur_mode],al
        call    putbyte
        mov     dx,OFFSET m_rows
        call    puts
        push    es
        mov     ax,BDA_SEG
        mov     es,ax
        mov     al,byte ptr es:[BDA_ROWS]
        pop     es
        xor     ah,ah
        inc     ax
        call    putdec
        mov     dx,OFFSET m_crlf
        call    puts
        mov     dx,OFFSET m_istext
        mov     al,byte ptr [cur_mode]
        cmp     al,3
        jbe     cx_text
        cmp     al,7
        je      cx_text
        call    fail
        mov     byte ptr [need3],1
        jmp     cx_gc
cx_text:
        call    pass
cx_gc:
        call    is_vga
        jc      cx_novga
        mov     dx,GC_IDX
        mov     al,GC_MISC
        out     dx,al
        inc     dx
        in      al,dx
        mov     dx,OFFSET m_gctext
        test    al,1
        jnz     cx_gcgfx
        call    pass
        jmp     cx_fix
cx_gcgfx:
        call    fail
        mov     byte ptr [need3],1
        jmp     cx_fix
cx_novga:
        mov     dx,OFFSET m_noask
        call    puts
cx_fix:
        cmp     byte ptr [need3],0
        je      cx_out
        call    set3
cx_out:
        ret

; is_vga - CF clear if INT 10h AX=1A00h names a VGA as the active
;          display (7: with a mono monitor, 8: with a color one)
is_vga:
        push    ax
        push    bx
        mov     ax,1A00h
        int     10h
        cmp     al,1Ah
        jne     iv_no
        cmp     bl,7
        jb      iv_no
        cmp     bl,8
        ja      iv_no
        pop     bx
        pop     ax
        clc
        ret
iv_no:
        pop     bx
        pop     ax
        stc
        ret

; set13 - mode 13h through the BIOS; CF, and nothing set, with no VGA
set13:
        call    is_vga
        jc      s13_out
        mov     ax,0013h
        int     10h
        clc
s13_out:
        ret

; set3 - mode 3 through the BIOS
set3:
        mov     ax,0003h
        int     10h
        ret

; store - a byte of the kernel's low pages written back to itself
;         through a segment of 0: a fault in a box.  An XCHG, which
;         the monitor does not step over.
store:
        push    ds
        xor     ax,ax
        mov     ds,ax
        mov     al,byte ptr ds:[LOW_STORE]
        xchg    al,byte ptr ds:[LOW_STORE]
        pop     ds
        ret

; shrink - this program down to its image and a stack, which moves to
;          the top of what is left.  ES = the PSP.
shrink:
        pop     dx                      ; the way back, off the old stack
        mov     bx,OFFSET prog_end + STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,OFFSET prog_end + STACK_BYTES
        sti
        jmp     dx

; pass / fail - DX = the case's line; a line either way
pass:
        push    dx
        mov     dx,OFFSET m_ok
        call    puts
        pop     dx
        call    puts
        ret
fail:
        push    dx
        mov     dx,OFFSET m_fail
        call    puts
        pop     dx
        call    puts
        inc     word ptr [nfail]
        ret

; puts - the ASCIIZ at DX, to standard output
puts:
        push    ax
        push    bx
        push    cx
        push    si
        mov     si,dx
        xor     cx,cx
pu_len:
        cmp     byte ptr [si],0
        je      pu_go
        inc     si
        inc     cx
        jmp     pu_len
pu_go:
        mov     bx,1
        mov     ah,40h
        int     21h
        pop     si
        pop     cx
        pop     bx
        pop     ax
        ret

; putbyte - AL as two hex digits
putbyte:
        push    ax
        push    cx
        push    dx
        mov     ah,al
        mov     cx,2
pb_one:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     pb_dig
        add     al,7
pb_dig:
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     ax
        loop    pb_one
        pop     dx
        pop     cx
        pop     ax
        ret

; putdec - AX in decimal
putdec:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
pd_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     pd_div
pd_out:
        pop     dx
        add     dl,'0'
        mov     ah,02h
        int     21h
        loop    pd_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; Data
;=====================================================================
m_usage     db  'SCRBACK /G, /H, /R, /D, /T or /N ends the box or itself; /V, /K or /W looks at the screen afterwards',13,10,0
m_n         db  'SCRBACK /N: mode 13h, then 4Ch',13,10,0
m_w         db  'SCRBACK /W: the screen after a program ended itself in mode 13h',13,10,0
m_still13   db  'mode 13h is still set: nothing between two programs put the screen back',13,10,0
m_nokey     db  'SCRBACK.KEY could not be written',13,10,0
m_g         db  'SCRBACK /G: mode 13h, then a store into the kernel',39,'s pages',13,10,0
m_h         db  'SCRBACK /H: mode 13h, then a HLT with interrupts off',13,10,0
m_r         db  'SCRBACK /R: the VGA set to graphics by its own register, then a store',13,10,0
m_d         db  'SCRBACK /D: mode 13h, then a HLT in protected mode as a DPMI client',13,10,0
m_t         db  'SCRBACK /T: a text screen with palette entry 1 changed, then a store',13,10,0
m_v         db  'SCRBACK /V: the screen after the box ended',13,10,0
m_k         db  'SCRBACK /K: the text screen after the box ended',13,10,0
m_mode      db  '        mode ',0
m_rows      db  'h, rows ',0
m_istext    db  'the BIOS says a text mode',13,10,0
m_gctext    db  'the VGA',39,'s Graphics Controller says a text mode',13,10,0
m_kept      db  'palette entry 1 is as /T set it: no mode was set',13,10,0
m_still     db  'the box did not end',13,10,0
m_novga     db  '        no VGA: nothing to try',13,10,0
m_noask     db  '        no VGA to ask',13,10,0
m_nodpmi    db  '        no DPMI host: nothing to try',13,10,0
m_dpmimem   db  'no memory for the DPMI host',39,'s data',13,10,0
m_dpmient   db  'the DPMI host did not take the program into protected mode',13,10,0
m_nowrite   db  'SCRBACK.DAT could not be written',13,10,0
m_noread    db  'SCRBACK.DAT could not be read: run SCRBACK /T first',13,10,0
m_ok        db  '  ok    ',0
m_fail      db  '  FAIL  ',0
m_crlf      db  13,10,0
m_done      db  'SCRBACK: ',0
m_fails     db  ' failed',13,10,0
datname     db  'SCRBACK.DAT',0
keyname     db  'SCRBACK.KEY',0
keytext     db  'SCRBACK /V',13,10,'EXIT',13,10
keyend      LABEL BYTE
.ERRNZ (keyend - keytext) - KEYLEN, <KEYLEN is not keytext's length>

dp_entry    dd  0
nfail       dw  0
dp_seg      dw  0
pal_old     db  3 dup(0)
cur_mode    db  0
need3       db  0
prog_end    LABEL BYTE

_SB     ENDS
        END     start
