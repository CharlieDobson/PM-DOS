;=====================================================================
; ANSITEST.ASM - ANSITEST.COM, a 16-bit DOS program that puts a console
;                driver's escape sequences on the screen one case at a
;                time and writes down what came of each.
;
;   ANSITEST file           the cases, into file
;
; For each case: mode 3 (which clears the screen), ESC[0m and ESC[?7h
; to start from the driver's defaults, any input left over read away;
; then the case's bytes, through 40h on standard output, INT 29h or
; 09h as the case says; then its keys put into the BIOS's buffer with
; INT 16h AH=05h; then everything standard input has to give read with
; 06h until it says there is no more.  The record is:
;
;   db  'C', case number
;   db  the BIOS's mode, columns, rows-1, cursor column, cursor row
;   db  input bytes (64 at most), then 64 bytes of them
;   4000 bytes of the screen, from B800:0000
;
; and before the first case one record of what INT 2Fh AX=1A00h said
; and what 440Ch CL=7Fh on handle 1 filled in:
;
;   db  'I', AL from 1A00h, CF from 440Ch, AX from 440Ch, 18 bytes of
;   its block
;
; NOTHING IS JUDGED HERE.  The same program runs under MS-DOS with
; Microsoft's ANSI.SYS, under MS-DOS with PM-DOS's, and under PM-DOS,
; and the three files are compared record by record (TOOLS\ANSICMP.PL).
; The first is the answer.  The cases are in the order they are because
; some build on the ones before - a key given a meaning keeps it.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_AT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_AT, DS:_AT, ES:_AT, SS:_AT
        ORG     0

K_40        EQU     0               ; how a case is written
K_29        EQU     1
K_09        EQU     2
IN_MAX      EQU     64
REC_LEN     EQU     2 + 5 + 1 + IN_MAX + 4000

start:
        jmp     main

;--- the cases: how, the text (FFh ends it), the keys (0 ends them) ---
cases       db  K_40
            dw  OFFSET t01, OFFSET knone
            db  K_40
            dw  OFFSET t02, OFFSET knone
            db  K_40
            dw  OFFSET t03, OFFSET knone
            db  K_40
            dw  OFFSET t04, OFFSET knone
            db  K_40
            dw  OFFSET t05, OFFSET knone
            db  K_40
            dw  OFFSET t06, OFFSET knone
            db  K_40
            dw  OFFSET t07, OFFSET knone
            db  K_40
            dw  OFFSET t08, OFFSET knone
            db  K_40
            dw  OFFSET t09, OFFSET knone
            db  K_40
            dw  OFFSET t10, OFFSET knone
            db  K_40
            dw  OFFSET t11, OFFSET knone
            db  K_40
            dw  OFFSET t12, OFFSET knone
            db  K_40
            dw  OFFSET t13, OFFSET knone
            db  K_40
            dw  OFFSET t14, OFFSET knone
            db  K_29
            dw  OFFSET t15, OFFSET knone
            db  K_40
            dw  OFFSET t16, OFFSET knone
            db  K_29
            dw  OFFSET t17, OFFSET knone
            db  K_09
            dw  OFFSET t18, OFFSET knone
            db  K_40
            dw  OFFSET t19, OFFSET knone
            db  K_40
            dw  OFFSET t20, OFFSET knone
            db  K_40
            dw  OFFSET t21, OFFSET k21
            db  K_40
            dw  OFFSET t22, OFFSET k22
            db  K_40
            dw  OFFSET t23, OFFSET k23
            db  K_40
            dw  OFFSET t24, OFFSET k24
            db  K_40
            dw  OFFSET t25, OFFSET knone
            db  K_40
            dw  OFFSET t26, OFFSET knone
            db  K_40
            dw  OFFSET t27, OFFSET knone
            db  K_40
            dw  OFFSET t28, OFFSET knone
            db  K_40
            dw  OFFSET t29, OFFSET knone
            db  K_40
            dw  OFFSET t30, OFFSET k30
            db  0FFh

knone       dw  0
k21         dw  1E41h, 3042h, 0
k22         dw  3B00h, 0
k23         dw  3042h, 1E41h, 0
k24         dw  1E41h, 3042h, 0
k30         dw  48E0h, 4800h, 0

E           EQU     1Bh

; plain text, CR, LF, BS
t01     db  'Hello',13,10,'World',13,10,'AB',8,'C',13,'D',10,'E',0FFh
; colours
t02     db  E,'[1;31;44mBRIGHT',E,'[0m norm ',E,'[4mU',E,'[5mB',E,'[7mR'
        db  E,'[8mH',E,'[0mN',E,'[mM',0FFh
t03     db  E,'[37;40;1;5;7mY',E,'[0;32;45mZ',E,'[99;35mW',E,'[1;;31mA'
        db  E,'[44m',13,10,'blue line',E,'[K',13,10,0FFh
; the cursor
t04     db  E,'[5;10HA',E,'[HB',E,'[fC',E,'[0;0HD',E,'[30;5HE',E,'[5;200HF'
        db  E,'[7;3fG',E,'[25;79HYZ',0FFh
t05     db  E,'[10;10H',E,'[3AA',E,'[100BB',E,'[5CC',E,'[100DD',E,'[CE'
        db  E,'[BF',E,'[0AG',E,'[1;1H',E,'[5AH',0FFh
t06     db  E,'[3;3Hsave',E,'[s',E,'[20;60Hthere',E,'[uback',E,'[u',E,'[2Bdown',0FFh
; erasing
t07     db  'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
        db  13,10,'bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb'
        db  E,'[1;20H',E,'[K',E,'[2;10H',E,'[1K',E,'[3;1Hcccccccccc',E,'[3;5H',E,'[2K'
        db  0FFh
t08     db  'dddddddddddddddddddddddddddddddddddddd',E,'[2;3Heee',E,'[44m',E,'[1J'
        db  'after',0FFh
t09     db  'ffff',E,'[41m',E,'[0Jg',0FFh
; wrapping and scrolling
t10     db  'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx'
        db  'Y',E,'[25;1H',E,'[42m'
        db  'zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'
        db  'last',0FFh
t11     db  E,'[?7l',E,'[10;75H0123456789',E,'[11;1H',E,'[=7h',E,'[11;78HABCD'
        db  E,'[7l',E,'[13;79HPQRS',E,'[?7h',13,10,'on',0FFh
; what is not a sequence
t12     db  E,'[5Xa',E,'Qb',E,E,'[1mc',E,'[2;3Zd',E,'[;H',E,'[=1;32mk'
        db  E,'[?;33ml',E,'[',13,10,'cr',0FFh
t13     db  E,'["abc";1mq',E,'[',27h,'x;y',27h,';36mr',E,'[""4ms',0FFh
t14     db  E,'[300;5Ha',E,'[256;10Hb',E,'[2;266Hc',E,'[3;300Hd',0FFh
; control characters through INT 29h, and a sequence through it
t15     db  1,2,3,4,5,6,11,12,14,15,16,17,18,19,20,21,22,23,24,25,26,28,29
        db  30,31,9,127,255-1,E,'[3;5H',E,'[33mI29',0FFh
; a tab through 40h, which DOS turns into spaces
t16     db  'a',9,'b',9,9,'c',13,10,'1234567',9,'8',0FFh
t17     db  E,'[4;4H',E,'[36mtwenty-nine',13,10,'x',8,8,'y',0FFh
t18     db  E,'[6;6H',E,'[35mnine',13,10,'$'
; the cursor report
t19     db  E,'[10;20H',E,'[6n',0FFh
t20     db  E,'[H',E,'[3;4H',E,'[5n',0FFh
; keys: A types XY and Enter, B does not change
t21     db  E,'[65;"XY";13p',0FFh
; F1 types DIR and Enter
t22     db  E,'[0;59;"DIR";13p',0FFh
; B types Up, the extended key: 0 and 72; A as before
t23     db  E,'[66;0;72p',0FFh
; A alone: its meaning goes.  B keeps its own
t24     db  E,'[65p',0FFh
; video modes
t25     db  E,'[=1h',E,'[2;2H40',0FFh
t26     db  E,'[=3h',E,'[=8h',E,'[=20h',E,'[3;3H80',0FFh
; a sequence too long for anything
t27     db  E,'['
        db  '1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;'
        db  '1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;'
        db  '1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;'
        db  '1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;'
        db  '1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;1;'
        db  '34mlong',E,'[0mend',0FFh
; a key's report and a key's meaning on the same line: the report wins
t28     db  E,'[12;12Hx',E,'[6n',0FFh
; the bottom line, written to the last cell
t29     db  E,'[25;1H',E,'[46m'
        db  '1234567890123456789012345678901234567890123456789012345678901234567890123456789'
        db  'X',0FFh
; /X on: the grey Up keeps its E0; then off again.  Keys: grey Up, Up
t30     db  E,'[1q',E,'[224;72;"G"p',E,'[0;72;"W"p',0FFh

msg_use     db  'ANSITEST file',13,10,'$'
msg_bad     db  'ANSITEST: cannot write the file',13,10,'$'
reset       db  E,'[0m',E,'[?7h'
reset_len   EQU $ - reset
ending      db  E,'[0q',E,'[0;72p',E,'[224;72p',E,'[66p',E,'[0;59p',E,'[0m'
ending_len  EQU $ - ending

handle      dw  0
casenum     db  0
            db  0
blk         db  20 dup (0)
rec         db  REC_LEN dup (0)

main:
        mov     ax,cs
        mov     ds,ax
        mov     es,ax

        ; the file name: the first word of the tail
        mov     si,81h
mn_skip:
        lodsb
        cmp     al,' '
        je      mn_skip
        cmp     al,13
        je      mn_use
        lea     dx,[si-1]
mn_end:
        lodsb
        cmp     al,' '
        ja      mn_end
        mov     byte ptr [si-1],0
        mov     ah,3Ch
        xor     cx,cx
        int     21h
        jc      mn_bad
        mov     [handle],ax

        ; the 'I' record
        mov     ax,1A00h
        int     2Fh
        mov     byte ptr [rec],'I'
        mov     [rec+1],al
        mov     byte ptr [blk],0                ; level 0
        mov     byte ptr [blk+1],0
        mov     word ptr [blk+2],14
        mov     ax,440Ch
        mov     bx,1
        mov     cx,037Fh
        mov     dx,OFFSET blk
        int     21h
        mov     byte ptr [rec+2],0
        jnc     mn_iok
        mov     byte ptr [rec+2],1
mn_iok:
        mov     word ptr [rec+3],ax
        mov     si,OFFSET blk
        mov     di,OFFSET rec+5
        mov     cx,18
        rep     movsb
        mov     dx,OFFSET rec
        mov     cx,23
        call    wr_file

        mov     si,OFFSET cases
mn_case:
        cmp     byte ptr [si],0FFh
        je      mn_done
        push    si
        call    one_case
        pop     si
        add     si,5
        jmp     short mn_case

mn_done:
        ; the meanings given back and the driver put as it was
        mov     ax,0003h
        int     10h
        mov     dx,OFFSET ending
        mov     cx,ending_len
        mov     bx,1
        mov     ah,40h
        int     21h
        call    drain
        mov     bx,[handle]
        mov     ah,3Eh
        int     21h
        mov     ax,4C00h
        int     21h
mn_use:
        mov     dx,OFFSET msg_use
        jmp     short mn_say
mn_bad:
        mov     dx,OFFSET msg_bad
mn_say:
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; one_case - SI -> its row in cases
;---------------------------------------------------------------------
one_case:
        inc     byte ptr [casenum]
        push    si
        mov     ax,0003h
        int     10h
        mov     dx,OFFSET reset
        mov     cx,reset_len
        mov     bx,1
        mov     ah,40h
        int     21h
        call    drain
        pop     si

        ; the text
        mov     al,[si]
        mov     di,[si+1]               ; the text
        push    word ptr [si+3]         ; the keys, for later
        mov     dx,di
        xor     cx,cx
oc_len:
        cmp     byte ptr [di],0FFh
        je      oc_have
        cmp     al,K_09
        jne     oc_ln
        cmp     byte ptr [di],'$'
        je      oc_have
oc_ln:
        inc     di
        inc     cx
        jmp     short oc_len
oc_have:
        cmp     al,K_29
        je      oc_29
        cmp     al,K_09
        je      oc_09
        mov     bx,1
        mov     ah,40h
        int     21h
        jmp     short oc_keys
oc_29:
        mov     si,dx
        jcxz    oc_keys
oc_29lp:
        lodsb
        int     29h
        loop    oc_29lp
        jmp     short oc_keys
oc_09:
        mov     ah,09h
        int     21h

oc_keys:
        pop     si
oc_klp:
        mov     cx,[si]
        jcxz    oc_read
        mov     ah,05h
        int     16h
        add     si,2
        jmp     short oc_klp

        ; what standard input has: 06h until it says nothing
oc_read:
        mov     di,OFFSET rec+8
        xor     bx,bx
oc_rlp:
        mov     ah,06h
        mov     dl,0FFh
        int     21h
        jz      oc_rdone
        cmp     bx,IN_MAX
        jae     oc_rlp
        mov     [di+bx],al
        inc     bx
        jmp     short oc_rlp
oc_rdone:
        mov     [rec+7],bl

        ; the BIOS's view
        mov     byte ptr [rec],'C'
        mov     al,[casenum]
        mov     [rec+1],al
        push    ds
        mov     ax,40h
        mov     ds,ax
        mov     al,ds:[49h]
        mov     ah,ds:[4Ah]
        mov     dl,ds:[84h]
        mov     cx,ds:[50h]
        pop     ds
        mov     [rec+2],al
        mov     [rec+3],ah
        mov     [rec+4],dl
        mov     word ptr [rec+5],cx

        ; and the screen
        push    ds
        mov     ax,0B800h
        mov     ds,ax
        xor     si,si
        mov     di,OFFSET rec + 8 + IN_MAX
        mov     cx,2000
        rep     movsw
        pop     ds
        mov     dx,OFFSET rec
        mov     cx,REC_LEN
        jmp     wr_file

;--- drain - whatever standard input has, thrown away
drain:
        mov     ah,06h
        mov     dl,0FFh
        int     21h
        jnz     drain
        ret

;--- wr_file - CX bytes at DX to the file
wr_file:
        mov     bx,[handle]
        mov     ah,40h
        int     21h
        ret

_AT     ENDS
        END     start
