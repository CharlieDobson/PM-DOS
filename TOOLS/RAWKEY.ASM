;=====================================================================
; RAWKEY.ASM - RAWKEY.EXE, the keyboard's scan codes for a native
;              program: 21h/F0h AL=0Fh and AL=10h
;
; A game has to know when a key comes up again, and every other key
; call gives characters.  AL=0Fh turns the keyboard over to the
; program, scan code by scan code, and AL=10h reads what has come
; (INT21.INC fF0_raw, KEYBD.INC kbr_put).  The keys are pressed here
; with 21h/F1h AL=23h, as KBLTEST presses its own, so every code goes
; in through kb_byte exactly as the keyboard's would.
;
; What this checks -
;
;   the codes come back as they were sent, every one: the E0h in front
;   of a grey key and the code with bit 7 set for a key let go
;
;   no key is made of any of them while it is on
;
;   THE RING HOLDS 127.  That many come back whole and in order; of
;   one more, the last is dropped and the 127 are still right; and the
;   ring takes codes again afterwards.  A read with less room than
;   there are codes leaves the rest for the next read
;
;   room that is not the program's is refused
;
;   turned off, the keyboard makes keys again - and it is turned off
;   for a program that ends without doing so, by 4Ch or by a fault:
;
;   RAWKEY L    turn it on and leave
;   RAWKEY F    turn it on and store through a null pointer
;   RAWKEY C    after either: a key typed is a key
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

RING_HOLDS      EQU     127             ; KBR_SIZE - 1 (IO\KEYBD.INC)
ROOM_BYTES      EQU     256

_MN     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MN, DS:_MN, ES:_MN, SS:_MN
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET mn_entry>,<0>,4096

INJ     MACRO   code
        mov     dl,code
        mov     eax,0F123h
        int     21h
        ENDM

EXPECT  MACRO   got, want
        mov     eax,got
        mov     ebx,want
        call    chk
        ENDM

SAY     MACRO   txt
        mov     edx,OFFSET txt
        call    puts
        ENDM

mn_entry:
        mov     esi,SLOT_PSP + 81h
tail_sp:
        lodsb
        cmp     al,' '
        je      tail_sp
        and     al,0DFh
        cmp     al,'L'
        je      run_leave
        cmp     al,'F'
        je      run_fault
        cmp     al,'C'
        je      run_check

        SAY     msg_hi

;--- on -----------------------------------------------------------------
; The carry is set on the way in: a kernel without the call answers
; "invalid function" and leaves it set.
        SAY     t_on
        call    raw_on
        EXPECT  eax,0
        call    verdict

        SAY     t_none
        mov     ecx,16
        call    rd
        EXPECT  eax,0
        call    verdict

;--- every byte, as it came ----------------------------------------------
        SAY     t_keys
        INJ     1Eh                     ; A down
        INJ     9Eh                     ; A up
        INJ     0E0h                    ; the grey up arrow, down
        INJ     48h
        INJ     0E0h                    ; ...and up
        INJ     0C8h
        mov     ecx,16
        call    rd
        EXPECT  eax,6
        EXPECT  <dword ptr [room]>,48E09E1Eh
        movzx   esi,word ptr [room + 4]
        EXPECT  esi,0C8E0h
        call    verdict

        SAY     t_nokey
        mov     ah,0Bh                  ; a character waiting?
        int     21h
        movzx   esi,al
        EXPECT  esi,0
        call    verdict

;--- the ring's limit ----------------------------------------------------
        SAY     t_full
        xor     ebx,ebx
        mov     ecx,RING_HOLDS
        call    inj_run
        mov     ecx,ROOM_BYTES
        call    rd
        EXPECT  eax,RING_HOLDS
        xor     ebx,ebx
        mov     ecx,RING_HOLDS
        call    chk_run
        EXPECT  eax,0
        call    verdict

        SAY     t_over
        xor     ebx,ebx
        mov     ecx,RING_HOLDS + 1
        call    inj_run
        mov     ecx,ROOM_BYTES
        call    rd
        EXPECT  eax,RING_HOLDS
        xor     ebx,ebx
        mov     ecx,RING_HOLDS
        call    chk_run
        EXPECT  eax,0
        mov     ecx,ROOM_BYTES
        call    rd
        EXPECT  eax,0
        call    verdict

        SAY     t_again
        INJ     02h
        mov     ecx,ROOM_BYTES
        call    rd
        EXPECT  eax,1
        movzx   esi,byte ptr [room]
        EXPECT  esi,02h
        call    verdict

        SAY     t_short
        xor     ebx,ebx
        mov     ecx,100
        call    inj_run
        mov     ecx,40
        call    rd
        EXPECT  eax,40
        xor     ebx,ebx
        mov     ecx,40
        call    chk_run
        EXPECT  eax,0
        mov     ecx,ROOM_BYTES
        call    rd
        EXPECT  eax,60
        mov     ebx,40
        mov     ecx,60
        call    chk_run
        EXPECT  eax,0
        call    verdict

        SAY     t_zero
        INJ     03h
        xor     ecx,ecx
        call    rd
        EXPECT  eax,0
        mov     ecx,1
        call    rd
        EXPECT  eax,1
        movzx   esi,byte ptr [room]
        EXPECT  esi,03h
        call    verdict

;--- room that is not the program's ---------------------------------------
        SAY     t_outside
        INJ     04h
        mov     edi,0FFFFF000h
        mov     ecx,16
        mov     eax,0F010h
        int     21h
        sbb     edx,edx                 ; -1: refused
        movzx   esi,ax
        EXPECT  edx,-1
        EXPECT  esi,5
        mov     ecx,ROOM_BYTES          ; and the code is still there
        call    rd
        EXPECT  eax,1
        call    verdict

;--- off -----------------------------------------------------------------
        SAY     t_off
        INJ     05h                     ; one left unread: it goes with the ring
        xor     ebx,ebx
        mov     eax,0F00Fh
        int     21h
        call    a_key
        call    verdict

        movzx   eax,byte ptr [fails]
        push    eax
        SAY     msg_bye
        pop     eax
        mov     ah,4Ch
        int     21h

;=====================================================================
run_leave:
        call    raw_on
        INJ     1Eh                     ; a key only the ring sees
        SAY     msg_left
        mov     eax,4C00h
        int     21h

run_fault:
        call    raw_on
        INJ     1Eh
        SAY     msg_fault
        xor     eax,eax
        mov     [eax],eax               ; the first page is nobody's
        mov     eax,4C01h               ; not reached
        int     21h

run_check:
        SAY     t_back
        call    a_key
        call    verdict
        movzx   eax,byte ptr [fails]
        mov     ah,4Ch
        int     21h

;=====================================================================
; raw_on - the scan codes for this program.  EAX = 0 if the kernel has
;          the call, -1 if not.
;=====================================================================
raw_on:
        mov     ebx,1
        mov     eax,0F00Fh
        stc
        int     21h
        sbb     eax,eax
        ret

;=====================================================================
; rd - ECX = room -> EAX = the codes read into [room]
;=====================================================================
rd:
        mov     edi,OFFSET room
        mov     eax,0F010h
        int     21h
        ret

;=====================================================================
; a_key - with the keyboard making keys: A is pressed and let go, a
;         character is waiting, it is 'a', and the ring has nothing.
;         The character is read only if one is there: 07h waits.
;=====================================================================
a_key:
        INJ     1Eh
        INJ     9Eh
        mov     ah,0Bh
        int     21h
        movzx   esi,al
        EXPECT  esi,0FFh
        cmp     esi,0FFh
        jne     ak_ring
        mov     ah,07h
        int     21h
        movzx   esi,al
        EXPECT  esi,61h                 ; 'a'
ak_ring:
        mov     ecx,16
        call    rd
        EXPECT  eax,0
        ret

;=====================================================================
; gen - EAX = n -> AL = the nth code of the run: the twelve keys 02h to
;       0Dh (1 to =) going down, then the same twelve coming up, and
;       round again.  None of them is a shift key.
;=====================================================================
gen:
        push    ebx
        push    edx
        xor     edx,edx
        mov     ebx,12
        div     ebx
        add     dl,2
        test    al,1
        jz      gen_out
        or      dl,80h
gen_out:
        mov     al,dl
        pop     edx
        pop     ebx
        ret

;=====================================================================
; inj_run - ECX codes of the run pressed, from number EBX
;=====================================================================
inj_run:
        pushad
ir_next:
        mov     eax,ebx
        call    gen
        mov     dl,al
        mov     eax,0F123h
        int     21h
        inc     ebx
        dec     ecx
        jnz     ir_next
        popad
        ret

;=====================================================================
; chk_run - [room] against ECX codes of the run from number EBX.
;           EAX = how many are not the run's.
;=====================================================================
chk_run:
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     esi,OFFSET room
        xor     edx,edx
cr_next:
        mov     eax,ebx
        call    gen
        cmp     al,[esi]
        je      cr_same
        inc     edx
cr_same:
        inc     esi
        inc     ebx
        dec     ecx
        jnz     cr_next
        mov     eax,edx
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; helpers - chk keeps the first difference on a line, verdict reports
;=====================================================================
chk:
        cmp     eax,ebx
        je      chk_out
        cmp     byte ptr [miss],0
        jne     chk_out
        mov     byte ptr [miss],1
        mov     [miss_got],eax
        mov     [miss_want],ebx
        mov     al,[chkn]
        mov     [miss_n],al
chk_out:
        inc     byte ptr [chkn]
        ret

verdict:
        pushad
        cmp     byte ptr [miss],0
        jne     vd_bad
        SAY     msg_ok
        jmp     short vd_reset
vd_bad:
        SAY     msg_chk
        movzx   eax,byte ptr [miss_n]
        call    puthex
        SAY     msg_got
        mov     eax,[miss_got]
        call    puthex
        SAY     msg_want
        mov     eax,[miss_want]
        call    puthex
        SAY     msg_bad
        inc     byte ptr [fails]
vd_reset:
        mov     byte ptr [miss],0
        mov     byte ptr [chkn],0
        popad
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; EAX in hex, eight digits
puthex:
        pushad
        mov     edi,OFFSET numbuf
        mov     ecx,8
ph_one:
        rol     eax,4
        mov     bl,al
        and     bl,0Fh
        add     bl,'0'
        cmp     bl,'9'
        jbe     ph_dig
        add     bl,7
ph_dig:
        mov     [edi],bl
        inc     edi
        loop    ph_one
        mov     byte ptr [edi],'$'
        mov     edx,OFFSET numbuf
        call    puts
        popad
        ret

;=====================================================================
msg_hi      db  13,10,'RAWKEY - the keyboard''s scan codes for a native program',13,10,'$'
msg_bye     db  'RAWKEY done.',13,10,'$'
msg_left    db  'RAWKEY L: the scan codes left on',13,10,'$'
msg_fault   db  'RAWKEY F: the scan codes on, and a fault',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_chk     db  ' #$'
msg_got     db  ' got $'
msg_want    db  ' want $'

t_on        db  '  0Fh on: the call is there$'
t_none      db  '  10h with nothing typed gives none$'
t_keys      db  '  a key down and up, a grey key with its E0h$'
t_nokey     db  '  and no key is made of them$'
t_full      db  '  127 codes fill the ring and come back in order$'
t_over      db  '  of 128 the last is dropped and 127 are whole$'
t_again     db  '  the emptied ring takes codes again$'
t_short     db  '  a short read leaves the rest for the next$'
t_zero      db  '  room for none takes none$'
t_outside   db  '  room outside the program is refused$'
t_off       db  '  0Fh off: a key is a key again, the ring empty$'
t_back      db  '  the keyboard came back when the program ended$'

fails       db  0
miss        db  0
chkn        db  0
miss_n      db  0
miss_got    dd  0
miss_want   dd  0
numbuf      db  12 dup (0)
room        db  ROOM_BYTES dup (0)

_MN     ENDS
        END
