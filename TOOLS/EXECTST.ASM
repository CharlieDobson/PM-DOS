;=====================================================================
; EXECTST.ASM - EXECTST.COM, a 16-bit DOS program that starts another
;               16-bit DOS program.
;
; The thing being tested is INT 21h/4Bh from inside a DOS box: a guest
; asking the kernel to load a second guest into the same conventional
; memory, run it, and come back here with its exit code.
;
; IT SHRINKS ITSELF FIRST, AND THAT IS NOT A FORMALITY.  A .COM is
; handed every free paragraph in the machine, so a .COM that has not
; called 4Ah has nothing left to give a child and 4Bh fails with "not
; enough memory" - on real DOS exactly as here.  Every program that
; EXECs does this; the interesting part of the test is that the
; failure mode is the same one, for the same reason.
;
; The child is ARGS.COM, which exits with the first digit in its tail,
; so the exit code coming back through 4Dh is checkable rather than
; just present.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_EXT    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_EXT, DS:_EXT, ES:_EXT, SS:_EXT
        ORG     0

start:
;--- what were we given? ---------------------------------------------
; DS is the PSP on entry to a .COM and stays it, so the tail is just
; [80h].  A digit means "you are a level of a recursion test"; a W
; means "run a child that waits, so a human can press ^C at it"; and
; nothing means the ordinary test.
        mov     al,byte ptr [80h]
        or      al,al
        jz      main_test
        mov     si,81h                          ; the tail text starts here
        mov     cx,7Eh
arg_scan:
        mov     al,[si]
        cmp     al,' '                          ; and the switch character
        je      arg_skip                        ; is as skippable as the
        cmp     al,'/'                          ; space in front of it -
        je      arg_skip                        ; "EXECTST /F" is a tail of
        cmp     al,'-'                          ; THREE characters, and
        jne     arg_got                         ; starting at 82h lands on
arg_skip:                                       ; the slash rather than
        inc     si                              ; past it
        loop    arg_scan
        jmp     short main_test
arg_got:
        ; Upper case, but only a letter: masking 20h off unconditionally
        ; turns '0' into DLE and the recursion levels stop being digits.
        cmp     al,'a'
        jb      arg_upper
        cmp     al,'z'
        ja      arg_upper
        sub     al,20h
arg_upper:
        mov     byte ptr [arg],al
        cmp     al,'W'
        je      wait_mode
        cmp     al,'F'
        je      fault_parent
        cmp     al,'X'
        je      fault_child
        cmp     al,'0'
        jb      main_test
        cmp     al,'9'
        ja      main_test
        jmp     deep_level

main_test:
        mov     dx,OFFSET msg_hi
        mov     ah,09h
        int     21h

;--- give back everything past our own end ---------------------------
; BX is paragraphs to keep, counted from the PSP.  OFFSET prog_end is
; already measured from the PSP and not from the code - the link puts
; this segment at 0100h - so the PSP is in the number and must not be
; added again.  ES still holds the PSP segment, which is what 4Ah
; wants to be pointed at.
        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        jnc     shrunk
        mov     dx,OFFSET msg_noshrink
        call    say_fail
        jmp     bail
shrunk:
        ; and move our own stack inside what we kept, before anything
        ; else is allowed to allocate over where it used to be
        cli
        mov     ax,cs
        mov     ss,ax
        ; Two instructions, and it has to be two: wasm reads
        ; "OFFSET prog_end + STACK_BYTES" as the OFFSET of an
        ; expression and refuses it, parenthesised or not.
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti
        mov     dx,OFFSET msg_shrunk
        call    say_ok

;--- the environment we were given, so we can hand it on -------------
        mov     ax,cs
        mov     ds,ax
        mov     es,ax

;=====================================================================
; The parameter block.  Four fields: an environment segment (zero
; means "give the child mine"), a far pointer to the command tail, and
; two far pointers to FCBs the child will find at 5Ch and 6Ch.
;=====================================================================
        mov     word ptr [pb_env],0             ; inherit
        mov     word ptr [pb_tail],OFFSET tail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch          ; our own, which are empty -
        mov     word ptr [pb_fcb1+2],cs         ; what almost every program
        mov     word ptr [pb_fcb2],6Ch          ; that EXECs passes
        mov     word ptr [pb_fcb2+2],cs

        mov     dx,OFFSET msg_run
        mov     ah,09h
        int     21h

        mov     dx,OFFSET child                 ; DS:DX -> the program
        mov     bx,OFFSET pb_env                ; ES:BX -> the block
        mov     ax,4B00h
        int     21h
        ; DS and ES back BEFORE anything reads them.  Real DOS returns
        ; from 4Bh with every register but CS:IP and SS:SP undefined,
        ; and a program that assumes otherwise works until the day it
        ; runs somewhere that takes the specification literally.
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      exec_failed

;--- back, and this line proves it -----------------------------------
        mov     dx,OFFSET msg_back
        call    say_ok

;--- 4Dh: what did it exit with? -------------------------------------
        mov     ah,4Dh
        int     21h
        mov     word ptr [code_ax],ax

        mov     dx,OFFSET msg_code
        mov     ah,09h
        int     21h
        mov     al,byte ptr [code_ax+1]         ; AH - how it ended
        call    put2
        mov     dl,'/'
        mov     ah,02h
        int     21h
        mov     al,byte ptr [code_ax]           ; AL - the code itself
        call    put2
        call    crlf

        cmp     word ptr [code_ax],0005h        ; ARGS 5 exits with 5,
        je      code_ok                         ; normally
        mov     dx,OFFSET msg_codebad
        call    say_fail
        jmp     short did_code
code_ok:
        mov     dx,OFFSET msg_codeok
        call    say_ok
did_code:

;--- and a second 4Dh must report nothing ----------------------------
; DOS answers once and forgets.  A program that polls it would
; otherwise see the same child exit over and over.
        mov     ah,4Dh
        int     21h
        or      ax,ax
        jz      again_ok
        mov     dx,OFFSET msg_againbad
        call    say_fail
        jmp     short did_again
again_ok:
        mov     dx,OFFSET msg_againok
        call    say_ok
did_again:

;--- a child that does not exist has to fail cleanly -----------------
        mov     word ptr [pb_tail],OFFSET emptytail
        mov     dx,OFFSET nochild
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        jnc     miss_bad
        cmp     ax,2                            ; file not found
        jne     miss_bad
        mov     dx,OFFSET msg_missok
        call    say_ok
        jmp     short did_miss
miss_bad:
        mov     dx,OFFSET msg_missbad
        call    say_fail
did_miss:

;--- and so does a function we do not provide ------------------------
        mov     dx,OFFSET child
        mov     bx,OFFSET pb_env
        mov     ax,4B03h                        ; overlay load
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        jnc     ovl_bad
        mov     dx,OFFSET msg_ovlok
        call    say_ok
        jmp     short did_ovl
ovl_bad:
        mov     dx,OFFSET msg_ovlbad
        call    say_fail
did_ovl:

;=====================================================================
; And now depth.  One level of nesting proves the stack is pushed and
; popped; it does not prove the stack is INDEXED, and an arithmetic
; error there shows up at level three and not before.
;
; So: run ourselves with "3", which runs itself with "2", and so on to
; "0", which exits with a distinctive code.  Every level checks what
; came back, so the answer only reaches here if all four returned to
; the right parent in the right order.
;=====================================================================
        mov     dx,OFFSET msg_deep
        mov     ah,09h
        int     21h

        mov     byte ptr [deeptail+2],'3'
        mov     word ptr [pb_tail],OFFSET deeptail
        mov     dx,OFFSET self
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      deep_bad
        mov     ah,4Dh
        int     21h
        cmp     ax,DEEP_CODE
        jne     deep_bad
        mov     dx,OFFSET msg_deepok
        call    say_ok
        jmp     bail
deep_bad:
        mov     dx,OFFSET msg_deepbad
        call    say_fail
        jmp     bail

;=====================================================================
; A level of the recursion.  Prints its depth, runs the next one down,
; checks what it exited with, and passes the same code up.
;=====================================================================
deep_level:
        mov     dx,OFFSET msg_level
        mov     ah,09h
        int     21h
        mov     dl,byte ptr [arg]
        mov     ah,02h
        int     21h
        call    crlf

        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti

        mov     al,byte ptr [arg]
        cmp     al,'0'
        je      deep_bottom

        dec     al                              ; one level further down
        mov     byte ptr [deeptail+2],al
        mov     word ptr [pb_env],0
        mov     word ptr [pb_tail],OFFSET deeptail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     dx,OFFSET self
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        jc      deep_fail
        mov     ah,4Dh
        int     21h
        cmp     ax,DEEP_CODE
        jne     deep_fail
deep_bottom:
        mov     ax,4C00h + DEEP_CODE
        int     21h
deep_fail:
        mov     ax,4C00h                        ; anything but DEEP_CODE
        int     21h

;=====================================================================
; A child that waits for a key, so that ^C can be pressed while this
; program is two levels deep.  Nothing here checks anything - the
; check is MEM afterwards, because an abort that unwinds only the
; innermost level leaves the outer one's memory allocated forever.
;=====================================================================
;=====================================================================
; /F and /X - the abnormal way out, two levels down.
;
; /F shrinks itself and EXECs "/X", which executes an invalid opcode.
; The box dies from underneath BOTH of them, and the point of the test
; is what MEM says afterwards: an unwind that cleans up only the
; program that faulted leaves its parent's block allocated with
; nothing left alive to free it, and conventional memory quietly
; shrinks every time it happens.
;
; This is the deterministic half of the same machinery ^C uses.  ^C
; arrives from the keyboard and cannot be scripted; the unwind loop it
; runs is the one exercised here.
;=====================================================================
fault_parent:
        mov     dx,OFFSET msg_fparent
        mov     ah,09h
        int     21h
        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti
        mov     word ptr [pb_env],0
        mov     word ptr [pb_tail],OFFSET faulttail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     dx,OFFSET self
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        ; Not reached: the child takes the box with it.  If this ever
        ; prints, the fault was survivable and the test is wrong about
        ; what it is testing.
        push    cs
        pop     ds
        mov     dx,OFFSET msg_fsurvived
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

fault_child:
        mov     dx,OFFSET msg_fchild
        mov     ah,09h
        int     21h
        db      0FFh,0FFh                       ; not an instruction
        mov     ax,4C00h
        int     21h

wait_mode:
        mov     dx,OFFSET msg_wait
        mov     ah,09h
        int     21h
        mov     bx,OFFSET prog_end
        add     bx,STACK_BYTES + 15
        shr     bx,4
        mov     ah,4Ah
        int     21h
        cli
        mov     ax,cs
        mov     ss,ax
        mov     sp,OFFSET prog_end
        add     sp,STACK_BYTES
        sti
        mov     word ptr [pb_env],0
        mov     word ptr [pb_tail],OFFSET waittail
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     dx,OFFSET child
        mov     bx,OFFSET pb_env
        mov     ax,4B00h
        int     21h
        push    cs
        pop     ds
        push    cs
        pop     es
        mov     dx,OFFSET msg_waitback
        mov     ah,09h
        int     21h
        mov     ax,4C00h
        int     21h

exec_failed:
        mov     word ptr [code_ax],ax
        mov     dx,OFFSET msg_execbad
        call    say_fail
        mov     dx,OFFSET msg_err
        mov     ah,09h
        int     21h
        mov     ax,word ptr [code_ax]
        call    put4
        call    crlf

bail:
        mov     dx,OFFSET msg_bye
        mov     ah,09h
        int     21h
        mov     al,byte ptr [failed]
        mov     ah,4Ch
        int     21h

;--- helpers ----------------------------------------------------------
say_ok:
        push    dx
        mov     dx,OFFSET msg_ok
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
        ret
say_fail:
        mov     byte ptr [failed],1
        push    dx
        mov     dx,OFFSET msg_fail
        mov     ah,09h
        int     21h
        pop     dx
        mov     ah,09h
        int     21h
        ret

put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
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
        jbe     p1_out
        add     al,7
p1_out:
        mov     dl,al
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        ret
crlf:
        mov     dl,13
        mov     ah,02h
        int     21h
        mov     dl,10
        mov     ah,02h
        int     21h
        ret

;--- data -------------------------------------------------------------
STACK_BYTES     EQU     512

DEEP_CODE       EQU     7               ; what the bottom of the recursion
                                        ; exits with, chosen to be nothing
                                        ; any other path would return

child       db  'ARGS.COM',0
self        db  'EXECTST.COM',0
nochild     db  'NOSUCH.COM',0
tail        db  2,' 5',0Dh
emptytail   db  0,0Dh
deeptail    db  2,' 0',0Dh              ; the digit is patched
waittail    db  3,' /W',0Dh
faulttail   db  3,' /X',0Dh
arg         db  0

pb_env      dw  0
pb_tail     dd  0
pb_fcb1     dd  0
pb_fcb2     dd  0

code_ax     dw  0
failed      db  0

msg_hi      db  'EXECTST.COM - a DOS program starting a DOS program',13,10,'$'
msg_shrunk  db  'shrank to our own size, so there is room for a child',13,10,'$'
msg_noshrink db 'could not shrink with 4Ah',13,10,'$'
msg_run     db  '  running ARGS.COM 5 as a child:',13,10,'$'
msg_back    db  'control came back to us',13,10,'$'
msg_code    db  '  4Dh says (type/code) $'
msg_codeok  db  'the child exited 5, normally',13,10,'$'
msg_codebad db  'that is not what the child exited with',13,10,'$'
msg_againok db  'a second 4Dh reports nothing, as DOS does',13,10,'$'
msg_againbad db 'a second 4Dh repeated the exit code',13,10,'$'
msg_missok  db  'a child that does not exist fails with error 2',13,10,'$'
msg_missbad db  'a missing child did not fail properly',13,10,'$'
msg_ovlok   db  '4B03h (overlay) is refused rather than half done',13,10,'$'
msg_ovlbad  db  '4B03h claimed to work',13,10,'$'
msg_execbad db  'EXEC refused to run the child',13,10,'$'
msg_err     db  '  error $'
msg_deep    db  '  and four levels deep, to test the nesting stack:',13,10,'$'
msg_level   db  '    level $'
msg_deepok  db  'the bottom level''s code came all the way back up',13,10,'$'
msg_deepbad db  'a level of the recursion did not return properly',13,10,'$'
msg_fparent db  'EXECTST /F - running a child that will fault',13,10,'$'
msg_fchild  db  'EXECTST /X - about to execute an invalid opcode',13,10,'$'
msg_fsurvived db 'the fault did not end the box',13,10,'$'
msg_wait    db  'EXECTST /W - running a child that waits, for ^C',13,10,'$'
msg_waitback db 'the child returned without being interrupted',13,10,'$'
msg_bye     db  'EXECTST done',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'

prog_end    label byte

_EXT    ENDS
        END
