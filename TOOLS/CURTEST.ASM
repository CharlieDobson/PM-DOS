;=====================================================================
; CURTEST.ASM - CURTEST.COM, a 16-bit DOS program that asks whether
;               the kernel's console and the machine's BIOS agree
;               about where the cursor is.
;
; They have to, and until now they did not.  The console kept the
; cursor in variables of its own and wrote characters straight into
; video memory; a DOS box's INT 10h goes to the real BIOS, which keeps
; it in the BIOS data area at 0040:0050 and moves the hardware cursor
; to match.  Two records of one thing, neither reading the other.
;
; So this program interleaves them deliberately and looks at the
; RESULT ON THE SCREEN rather than at what either side claims:
;
;   the BIOS puts the cursor somewhere.  DOS prints a character.  Is
;   the character in that cell?  (Does the kernel ADOPT the position?)
;
;   ...and where is the cursor afterwards, according to the BIOS?
;   (Does the kernel PUBLISH the position?)
;
;   then the BIOS teletypes a character and DOS prints another.  Are
;   they side by side, in that order?  (Do they agree WHILE ALTERNATING,
;   which is what a real program printing through both actually does?)
;
; Every check reads the character cell out of video memory itself, so
; a wrong answer cannot be a matter of opinion.  All of the probing
; happens before anything is printed, because printing the results
; would move the very cursor being measured.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CUR    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CUR, DS:_CUR, ES:_CUR, SS:_CUR
        ORG     0

TEST_ROW        EQU     12
TEST_COL        EQU     30

start:
;--- the screen's own idea of its width, and where its memory is -----
        mov     ax,40h
        mov     es,ax
        mov     ax,es:[4Ah]                     ; columns
        or      ax,ax
        jnz     have_cols
        mov     ax,80
have_cols:
        mov     [cols],ax
        mov     ax,0B800h
        cmp     byte ptr es:[49h],7             ; mode 7 is the mono card
        jne     have_seg
        mov     ax,0B000h
have_seg:
        mov     [vseg],ax

;=====================================================================
; Probe 1 - the BIOS moves the cursor, DOS writes a character.
;=====================================================================
        mov     ah,02h                          ; INT 10h: set cursor
        mov     bh,0
        mov     dh,TEST_ROW
        mov     dl,TEST_COL
        int     10h

        mov     ah,02h                          ; INT 21h: write 'X'
        mov     dl,'X'
        int     21h

        mov     dh,TEST_ROW                     ; what is in that cell?
        mov     dl,TEST_COL
        call    peek
        mov     [got1],al

        mov     ah,03h                          ; and where is the cursor?
        mov     bh,0
        int     10h
        mov     [row1],dh
        mov     [col1],dl

;=====================================================================
; Probe 2 - and now alternating, on the same line.
;
; The BIOS teletypes 'Y' where it left off, then DOS prints 'Z'.  If
; the two halves are keeping separate cursors, the second character
; lands on top of the first, or a column away from it, or back at the
; start of a line nobody is writing to.
;=====================================================================
        mov     ah,0Eh                          ; INT 10h: teletype 'Y'
        mov     al,'Y'
        mov     bh,0
        mov     bl,7
        int     10h

        mov     ah,02h                          ; INT 21h: write 'Z'
        mov     dl,'Z'
        int     21h

        mov     dh,TEST_ROW
        mov     dl,TEST_COL + 1
        call    peek
        mov     [got2],al
        mov     dh,TEST_ROW
        mov     dl,TEST_COL + 2
        call    peek
        mov     [got3],al

        mov     ah,03h
        mov     bh,0
        int     10h
        mov     [row2],dh
        mov     [col2],dl

;=====================================================================
; Probe 3 - the other direction: DOS moves the cursor through its own
;           call, and the BIOS has to see it there.
;
; PM-DOS's cursor call is 21h/F0h AL=2, which is the kernel's own and
; not a DOS standard - but the BIOS reading it back through INT 10h is
; entirely standard, and that is the half being tested.
;=====================================================================
        mov     ax,0F002h
        mov     dh,TEST_ROW + 2
        mov     dl,TEST_COL
        int     21h

        mov     ah,03h
        mov     bh,0
        int     10h
        mov     [row3],dh
        mov     [col3],dl

;=====================================================================
; Nothing is measured after this point, so it is safe to print.
;=====================================================================
        mov     ah,02h
        mov     bh,0
        mov     dh,0
        mov     dl,0
        int     10h
        mov     ax,0F000h                       ; clear the evidence off
        int     21h                             ; the screen

        mov     dx,OFFSET msg_hi
        call    puts

;--- probe 1 ---------------------------------------------------------
        mov     dx,OFFSET msg_p1
        call    puts
        cmp     byte ptr [got1],'X'
        jne     p1_bad
        mov     dx,OFFSET msg_p1ok
        call    ok
        jmp     short p1_cur
p1_bad:
        mov     dx,OFFSET msg_p1bad
        call    fail
p1_cur:
        cmp     byte ptr [row1],TEST_ROW
        jne     p1_cbad
        cmp     byte ptr [col1],TEST_COL + 1
        jne     p1_cbad
        mov     dx,OFFSET msg_p1cok
        call    ok
        jmp     short probe2
p1_cbad:
        mov     dx,OFFSET msg_p1cbad
        call    fail
        call    saywhere1

;--- probe 2 ---------------------------------------------------------
probe2:
        mov     dx,OFFSET msg_p2
        call    puts
        cmp     byte ptr [got2],'Y'
        jne     p2_bad
        cmp     byte ptr [got3],'Z'
        jne     p2_bad
        mov     dx,OFFSET msg_p2ok
        call    ok
        jmp     short p2_cur
p2_bad:
        mov     dx,OFFSET msg_p2bad
        call    fail
        mov     dx,OFFSET msg_p2saw
        call    puts
        mov     al,[got2]
        call    putc
        mov     al,[got3]
        call    putc
        call    crlf
p2_cur:
        cmp     byte ptr [col2],TEST_COL + 3
        jne     p2_cbad
        mov     dx,OFFSET msg_p2cok
        call    ok
        jmp     short probe3
p2_cbad:
        mov     dx,OFFSET msg_p2cbad
        call    fail

;--- probe 3 ---------------------------------------------------------
probe3:
        mov     dx,OFFSET msg_p3
        call    puts
        cmp     byte ptr [row3],TEST_ROW + 2
        jne     p3_bad
        cmp     byte ptr [col3],TEST_COL
        jne     p3_bad
        mov     dx,OFFSET msg_p3ok
        call    ok
        jmp     short cdone
p3_bad:
        mov     dx,OFFSET msg_p3bad
        call    fail

cdone:
        mov     dx,OFFSET msg_done
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; peek - the character in the cell at DH,DL, out of video memory.
;=====================================================================
peek:
        push    bx
        push    cx
        push    es
        movzx   ax,dh
        movzx   cx,byte ptr [cols]
        imul    ax,cx
        movzx   cx,dl
        add     ax,cx
        shl     ax,1
        mov     bx,ax
        mov     ax,[vseg]
        mov     es,ax
        mov     al,es:[bx]
        pop     es
        pop     cx
        pop     bx
        ret

saywhere1:
        mov     dx,OFFSET msg_saw
        call    puts
        movzx   eax,byte ptr [row1]
        call    dec8
        mov     dx,OFFSET msg_comma
        call    puts
        movzx   eax,byte ptr [col1]
        call    dec8
        call    crlf
        ret

;=====================================================================
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        call    puts
        ret
fail:
        push    dx
        mov     dx,OFFSET msg_fail
        call    puts
        pop     dx
        call    puts
        ret
dec8:
        pushad
        mov     ecx,0
        mov     ebx,10
d8_div:
        xor     edx,edx
        div     ebx
        push    edx
        inc     ecx
        or      eax,eax
        jnz     d8_div
d8_out:
        pop     eax
        add     al,'0'
        call    putc
        loop    d8_out
        popad
        ret

;=====================================================================
msg_hi      db  13,10,'CURTEST - one cursor, or two?',13,10,'$'
msg_p1      db  '  the BIOS moves it, DOS writes:',13,10,'$'
msg_p1ok    db  'the character landed where the BIOS left the cursor',13,10,'$'
msg_p1bad   db  'DOS wrote somewhere else entirely',13,10,'$'
msg_p1cok   db  'and the BIOS can see where DOS left it',13,10,'$'
msg_p1cbad  db  'but the BIOS still thinks it is elsewhere',13,10,'$'
msg_saw     db  '         it says row $'
msg_comma   db  ', column $'
msg_p2      db  '  and alternating, on one line:',13,10,'$'
msg_p2ok    db  'INT 10h then INT 21h wrote YZ, side by side',13,10,'$'
msg_p2bad   db  'the two halves overwrote each other',13,10,'$'
msg_p2saw   db  '         the two cells hold: $'
msg_p2cok   db  'and both of them moved the cursor on',13,10,'$'
msg_p2cbad  db  'but the cursor did not end up past them',13,10,'$'
msg_p3      db  '  and DOS moves it, the BIOS reads it back:',13,10,'$'
msg_p3ok    db  'the same position, both ways round',13,10,'$'
msg_p3bad   db  'the BIOS reads back a different position',13,10,'$'
msg_done    db  'CURTEST done',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'
msg_crlf    db  13,10,'$'

cols        dw  80
vseg        dw  0B800h
got1        db  0
got2        db  0
got3        db  0
row1        db  0
col1        db  0
row2        db  0
col2        db  0
row3        db  0
col3        db  0

_CUR    ENDS
        END
