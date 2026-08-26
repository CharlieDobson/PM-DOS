;=====================================================================
; CFGTEST.ASM - CFGTEST.COM, what CONFIG.SYS actually did.
;
; Four statements stopped being decoration in v0.39 and this is how
; they are measured - from inside a DOS box, through the published
; interfaces, by a program that was not there when any of them ran.
;
; It expects the CONFIG.SYS this kernel ships with:
;
;   FILES=40           more handles than the old fixed table had
;   LASTDRIVE=E        five letters, and never fewer than really exist
;   SET PMDOSTEST=...  the master environment, reaching a grandchild
;   INSTALLHIGH=...    a TSR loaded before the shell existed
;
; THE ENVIRONMENT CHECK IS THE LONG ONE and is worth naming: the value
; is put in the kernel's master environment by CONFIG.SYS, copied into
; the interpreter's slot behind its PSP, read out of there by the
; interpreter into its own block, and copied into conventional memory
; again for this program's DOS box.  Four copies and three address
; spaces, and if any of them is missing the variable simply is not
; here - which is exactly how it failed the first time, silently.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CF     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CF, DS:_CF, ES:_CF, SS:_CF
        ORG     0

MPX_ID          EQU     0C0h            ; TSRTEST's multiplex identifier
WANT_LASTDRV    EQU     5               ; LASTDRIVE=E - or the number of
                                        ; block devices, if that is more
WANT_FILES      EQU     30              ; FILES=40, less what is in use.
                                        ; The OLD table could not reach
                                        ; thirty however hard it was asked

start:
        mov     dx,OFFSET msg_hi
        call    puts
        mov     word ptr [resptr],OFFSET results

;=====================================================================
; 1 - LASTDRIVE, out of the list of lists.  ES:BX points at the
;     structure's second byte, so the field at 23h is at BX+21h - the
;     one offset in DOS that is quoted both ways round and is wrong
;     half the time.
;
;     AND THE ANSWER IS NOT THE NUMBER IN CONFIG.SYS.  LASTDRIVE says
;     how far a program should bother looking for drives, so it can
;     never be fewer than there really are - a machine with seven
;     volumes and LASTDRIVE=E would otherwise be hiding two of its own
;     disks from anything that asked politely.  Real DOS raises it for
;     the same reason, and the block-device count next door in the
;     same structure is what to raise it to.
;
;     So the test is the RULE and not a number: five, or the number of
;     block devices if that is more.  Written the other way it passed
;     on the floppy machine and failed on the one with a disk in it.
;=====================================================================
        mov     dx,OFFSET msg_t1
        call    puts
        mov     ah,52h
        int     21h
        mov     al,es:[bx+20h]          ; block devices
        mov     [nblock],al
        mov     al,es:[bx+21h]          ; ...and LASTDRIVE beside them
        mov     [lastdrv],al
        call    put2
        mov     dx,OFFSET msg_want
        call    puts

        mov     al,WANT_LASTDRV
        cmp     al,[nblock]
        jae     t1_want
        mov     al,[nblock]
t1_want:
        mov     [wantdrv],al
        call    put2
        mov     al,[lastdrv]
        cmp     al,[wantdrv]
        jne     t1bad
        call    good
        jmp     short t2
t1bad:
        call    bad
t2:

;=====================================================================
; 2 - FILES, counted rather than asked for.  There is no field that
;     publishes it, so the honest measurement is to open handles until
;     the kernel says no and put them all back.
;
;     The number is not exact on purpose: the interpreter may hold a
;     batch file open while this runs, and one handle either way is
;     not what is being tested.  What is being tested is that the
;     table grew - thirty is past anything the old thirty-two-entry
;     table could ever have handed out.
;=====================================================================
        mov     dx,OFFSET msg_t2
        call    puts
        xor     si,si                   ; handles taken
t2_open:
        cmp     si,MAXH
        jae     t2_full
        mov     dx,OFFSET fname
        xor     cx,cx
        mov     ah,3Ch                  ; create/truncate: always succeeds
        int     21h                     ; until the table is full
        jc      t2_full
        mov     bx,si
        shl     bx,1
        mov     [handles+bx],ax
        inc     si
        jmp     short t2_open
t2_full:
        mov     [nopen],si

        xor     di,di                   ; ...and give every one of them back
t2_close:
        cmp     di,si
        jae     t2_closed
        mov     bx,di
        shl     bx,1
        mov     bx,[handles+bx]
        mov     ah,3Eh
        int     21h
        inc     di
        jmp     short t2_close
t2_closed:
        mov     dx,OFFSET fname
        mov     ah,41h                  ; and the file it made
        int     21h

        mov     ax,[nopen]
        call    put4
        mov     dx,OFFSET msg_atleast
        call    puts
        mov     al,WANT_FILES
        call    put2
        cmp     word ptr [nopen],WANT_FILES
        jb      t2bad
        call    good
        jmp     short t3
t2bad:
        call    bad
t3:

;=====================================================================
; 3 - the environment, four copies from where CONFIG.SYS put it
;=====================================================================
        mov     dx,OFFSET msg_t3
        call    puts
        mov     ax,ds:[2Ch]             ; a .COM starts with DS = ES = its
                                        ; own PSP, and PSP+2Ch is the
                                        ; environment segment
        or      ax,ax
        jz      t3bad
        mov     es,ax
        xor     di,di
t3_str:
        cmp     byte ptr es:[di],0
        je      t3bad                   ; the empty string ends the block
        mov     si,OFFSET want_name
        push    di
t3_cmp:
        mov     al,[si]
        or      al,al
        jz      t3_hit
        cmp     al,es:[di]
        jne     t3_next
        inc     si
        inc     di
        jmp     short t3_cmp
t3_next:
        pop     di
t3_skip:
        mov     al,es:[di]
        inc     di
        or      al,al
        jnz     t3_skip
        jmp     short t3_str
t3_hit:
        pop     ax                      ; drop the saved start: found it
        mov     si,OFFSET want_value
t3_val:
        mov     al,[si]
        cmp     al,es:[di]
        jne     t3bad
        or      al,al
        jz      t3_ok
        inc     si
        inc     di
        jmp     short t3_val
t3_ok:
        call    good
        jmp     short t4
t3bad:
        call    bad
t4:

;=====================================================================
; 4 - and the TSR that CONFIG.SYS installed, answering for itself.
;     It was loaded before the interpreter existed, in a DOS box that
;     is long gone, and this is a different box again.
;
;     SKIPPED RATHER THAN FAILED when there is no TSR there, because
;     the INSTALLHIGH= line that loads it is commented out in the
;     shipped CONFIG.SYS - a program that prints a banner would break
;     the silence that file is meant to boot in.  A test that reports
;     a failure for a line the user chose not to write is a test that
;     gets ignored, and an ignored test is worse than no test.
;=====================================================================
        mov     dx,OFFSET msg_t4
        call    puts
        mov     ah,MPX_ID
        mov     al,0
        int     2Fh
        cmp     al,0FFh
        jne     t4none
        call    good
        jmp     short done
t4none:
        call    skip
done:
        mov     dx,OFFSET msg_res
        call    puts
        mov     dx,OFFSET results
        call    puts
        mov     dx,OFFSET msg_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts

        mov     al,[fails]
        mov     ah,4Ch
        int     21h

;---------------------------------------------------------------------
good:
        push    dx
        mov     dl,'.'
        call    tally
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dl,'X'
        call    tally
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret
skip:
        push    dx
        mov     dl,'-'
        call    tally
        mov     dx,OFFSET msg_skip
        call    puts
        pop     dx
        ret
tally:
        push    bx
        mov     bx,[resptr]
        mov     [bx],dl
        inc     bx
        mov     byte ptr [bx],'$'
        mov     [resptr],bx
        pop     bx
        ret

puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
        call    put2
        ret
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
; data
;=====================================================================
MAXH        EQU 64

lastdrv     db  0
nblock      db  0
wantdrv     db  0
            db  0
nopen       dw  0
fails       db  0
            db  0
resptr      dw  0
handles     dw  MAXH dup (0)

fname       db  'CFGTEST.TMP',0
want_name   db  'PMDOSTEST=',0
want_value  db  'hello from config',0

msg_hi      db  'CFGTEST - what CONFIG.SYS actually did',13,10,13,10,'$'
msg_t1      db  ' 1 LASTDRIVE= reaches the list of lists  ','$'
msg_t2      db  ' 2 FILES= handles opened at once  ','$'
msg_t3      db  ' 3 SET= reached a program two levels on  ','$'
msg_t4      db  ' 4 INSTALL= TSR is still answering       ','$'
msg_want    db  ' want ','$'
msg_atleast db  ' want at least ','$'
msg_ok      db  '  ok',13,10,'$'
msg_bad     db  '  FAILED',13,10,'$'
msg_skip    db  '  not installed',13,10,'$'
msg_res     db  13,10,'results: ','$'
msg_fails   db  '   failures: ','$'
msg_crlf    db  13,10,'$'
results     db  16 dup (0)

_CF     ENDS
        END     start
