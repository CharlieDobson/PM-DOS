;=====================================================================
; IRQXWAIT.ASM - IRQXWAIT.EXE, the native half of IRQXTEST
;
; IRQXTEST /I leaves a handler on INT 1Ch that types a Z every two
; seconds.  This is a NATIVE program, so while it runs there is no DOS
; box at all - and every Z it reads was typed by real-mode code the
; kernel ran for an interrupt with nobody in a box to take it
; (DOS\IRQX.INC).  Both of the moments that happens at are asked for:
;
;   1  POLLING, for five seconds: a call that does not wait, over and
;      over.  The excursion runs at the end of each call.
;   2  WAITING, for one key: a call that does not come back until
;      there is one.  The excursion runs from the kernel's idle loop -
;      and without it this would wait for ever, so a run that never
;      finishes is this check failing.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_IW     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_IW, DS:_IW, ES:_IW, SS:_IW
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET iw_entry>,<0>,4096

KEY_Z           EQU     2C5Ah
POLL_SECS       EQU     5

iw_entry:
        mov     edx,OFFSET msg_hi
        call    puts

;--- 1: polling ---------------------------------------------------------
        mov     edx,OFFSET msg_t1
        call    puts
        call    secs
        mov     [start],eax
p_loop:
        mov     ax,0F008h               ; is there a key - and do not wait
        mov     dl,[shift]
        mov     dh,1
        int     21h
        mov     [shift],bl
        cmp     al,1
        jne     p_time
        mov     ax,0F007h               ; there is: read it
        int     21h
        cmp     ax,KEY_Z
        jne     p_time
        inc     dword ptr [npoll]
p_time:
        ; THE TIME, ONE TURN IN 256.  2Ch asks the BIOS's clock, from real
        ; mode, and every such call is a window in which the clock's
        ; interrupts reach a handler through the vector table in real mode
        ; rather than as this test means them to - asked on every turn it
        ; was most of them.
        inc     byte ptr [turns]
        jnz     p_loop
        call    secs
        sub     eax,[start]
        jns     p_pos
        add     eax,24 * 3600           ; midnight
p_pos:
        cmp     eax,POLL_SECS
        jb      p_loop
        mov     eax,[npoll]
        call    putdec
        cmp     dword ptr [npoll],1
        jb      t1_bad
        call    good
        jmp     short t2
t1_bad:
        call    bad

;--- 2: waiting ---------------------------------------------------------
t2:
        mov     edx,OFFSET msg_t2
        call    puts
        mov     ax,0F007h               ; until there is a key
        int     21h
        movzx   eax,ax
        push    eax
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     eax
        cmp     ax,KEY_Z
        jne     t2_bad
        call    good
        jmp     short report
t2_bad:
        call    bad

report:
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     failed
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
failed:
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; secs - EAX = seconds since midnight, from 2Ch
;---------------------------------------------------------------------
secs:
        push    ecx
        push    edx
        mov     ah,2Ch
        int     21h
        movzx   eax,ch                  ; hours
        imul    eax,60
        movzx   ecx,cl                  ; minutes
        add     eax,ecx
        imul    eax,60
        movzx   ecx,dh                  ; seconds
        add     eax,ecx
        pop     edx
        pop     ecx
        ret

good:
        push    edx
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
bad:
        push    edx
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     edx
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; putdec - EAX in decimal
putdec:
        pushad
        mov     ecx,10
        xor     ebx,ebx
pd_div:
        xor     edx,edx
        div     ecx
        push    edx
        inc     ebx
        or      eax,eax
        jnz     pd_div
pd_out:
        pop     edx
        add     dl,'0'
        mov     ah,02h
        int     21h
        dec     ebx
        jnz     pd_out
        popad
        ret

;=====================================================================
msg_hi      db  13,10,'IRQXWAIT - keys typed by a resident handler, with no box running'
            db  13,10,13,10,'$'
msg_t1      db  'Zs while polling, 5s ..... $'
msg_t2      db  'and waiting for one ...... $'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'IRQXWAIT: all checks passed',13,10,'$'
msg_fail    db  'IRQXWAIT: FAILURES - see above',13,10,'$'

start       dd  0
npoll       dd  0
shift       db  0
turns       db  0
fails       db  0

_IW     ENDS
        END
