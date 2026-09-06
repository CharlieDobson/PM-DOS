;=====================================================================
; HNDTEST.ASM - HNDTEST.COM, how many files can one process open?
;
; WHY THIS EXISTS.  Program Manager comes up with no groups in it, and
; the DOS trace says why: every open of C:\WINDOWS\PROGMAN.INI comes
; back with error 4, too many open files, twenty-four times in a row.
; Windows has eighteen files open by the time the shell starts and the
; per-process table DOS builds in the PSP holds twenty.
;
; The way out of that is INT 21h AH=67h, which asks DOS to MOVE the
; handle table into a block of its own and make it bigger.  RMDOS
; refused it until v0.55 on the grounds that the table cannot grow
; past the PSP - true, and not the point.
;
; This is the unit test for the new one, and it runs on both DOSes:
; from the command line it tests PM-DOS's, through PARKTEST it tests
; RMDOS's.  It asks for sixty handles, prints what the PSP says
; afterwards, and then opens the same file until it is refused.  A
; DOS that answers 67h and then still stops at twenty has not
; implemented it, it has only stopped saying no.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_HT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_HT, DS:_HT, ES:_HT, SS:_HT
        ORG     0

WANT    EQU     60
TRIES   EQU     50

start:
        mov     [pspseg],ds             ; a .COM starts with DS on its PSP
        mov     dx,OFFSET m_hi
        call    puts

;--- what the table looks like before ---------------------------------
        mov     dx,OFFSET m_before
        call    puts
        call    showjft

;--- ask for more -----------------------------------------------------
        mov     dx,OFFSET m_ask
        call    puts
        mov     ax,6700h
        mov     bx,WANT
        int     21h
        jnc     ht_asked
        mov     dx,OFFSET m_refused
        call    puts
        call    put4
        call    crlf
        jmp     short ht_open
ht_asked:
        mov     dx,OFFSET m_granted
        call    puts
ht_open:
        mov     dx,OFFSET m_after
        call    puts
        call    showjft

;--- and then just open the thing until it will not open --------------
        mov     dx,OFFSET m_open
        call    puts
        xor     si,si                   ; how many are open
        mov     cx,TRIES
ht_loop:
        push    cx
        mov     dx,OFFSET f_name
        mov     ax,3D00h
        int     21h
        pop     cx
        jc      ht_stop
        mov     bx,si
        shl     bx,1
        mov     [hnds+bx],ax
        inc     si
        loop    ht_loop
ht_stop:
        mov     [lasterr],ax
        mov     ax,si
        call    put4
        mov     dx,OFFSET m_opened
        call    puts
        mov     ax,[lasterr]
        call    put4
        call    crlf

;--- and give them all back ------------------------------------------
        mov     cx,si
        or      cx,cx
        jz      ht_done
        xor     si,si
ht_close:
        push    cx
        mov     bx,si
        shl     bx,1
        mov     bx,[hnds+bx]
        mov     ah,3Eh
        int     21h
        inc     si
        pop     cx
        loop    ht_close
ht_done:
        mov     dx,OFFSET m_bye
        call    puts
        mov     ax,4C00h
        int     21h

;--- showjft - the three words in the PSP that say where the table is
;--- and how big it is.  Read them all first, THEN print: DS has to be
;--- the PSP for the first and ours for the second, and a routine that
;--- keeps swapping it is a routine nobody can check.
showjft:
        push    ax
        push    ds
        mov     ds,[cs:pspseg]
        mov     ax,ds:[32h]
        mov     cs:[jcount],ax
        mov     ax,ds:[34h]
        mov     cs:[joff],ax
        mov     ax,ds:[36h]
        mov     cs:[jseg],ax
        pop     ds
        mov     ax,[jcount]
        call    put4
        mov     dx,OFFSET m_at
        call    puts
        mov     ax,[jseg]
        call    put4
        mov     dx,OFFSET m_colon
        call    puts
        mov     ax,[joff]
        call    put4
        call    crlf
        pop     ax
        ret
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

crlf:
        push    ax
        push    dx
        mov     dx,OFFSET m_crlf
        mov     ah,09h
        int     21h
        pop     dx
        pop     ax
        ret

put4:
        push    ax
        push    cx
        push    dx
        mov     cx,4
p4_loop:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p4_dig
        add     al,7
p4_dig:
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     ax
        loop    p4_loop
        pop     dx
        pop     cx
        pop     ax
        ret

pspseg      dw  0
jcount      dw  0
joff        dw  0
jseg        dw  0
lasterr     dw  0
hnds        dw  TRIES dup (0)

m_hi        db  'HNDTEST - how many files can one process open?',13,10,'$'
m_before    db  'table before: $'
m_after     db  'table after:  $'
m_at        db  ' entries at $'
m_colon     db  ':$'
m_ask       db  'asking 67h for sixty... $'
m_granted   db  'granted.',13,10,'$'
m_refused   db  'REFUSED, code $'
m_open      db  'opened: $'
m_opened    db  ' files, then error $'
m_bye       db  'done.',13,10,'$'
m_crlf      db  13,10,'$'

f_name      db  'C:\WINDOWS\SYSTEM.INI',0

_HT     ENDS
        END     start
