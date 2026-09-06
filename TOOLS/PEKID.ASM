;=====================================================================
; PEKID.ASM - PEKID.COM, a child that says so BEFORE it asks DOS
;
; PARKEXEC's other children - ARGS.COM, HELLOX.EXE - announce
; themselves with INT 21h AH=09h, which makes "the loader jumped into
; the program" and "the program's first DOS call was served" the same
; observation.  When neither happens there is nothing to tell them
; apart, and they are different faults in different files.
;
; So this one writes STRAIGHT TO THE TEXT PAGE first, in four
; instructions, with no interrupt, no stack and no DOS:
;
;   *K   appears  the far jump landed and the child is executing
;   *KD  appears  ...and RMDOS served an INT 21h for it as well
;   nothing       the jump did not arrive, and the loader is the fault
;
; The marker goes on the BOTTOM line, which is below anything the
; trace has printed and stays put when the screen scrolls under it.
;
; Exits with 5, so the parent's 4Dh check is the same one it makes of
; ARGS.COM and means the same thing.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_PK     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_PK, DS:_PK, ES:_PK, SS:_PK
        ORG     0

MARK    EQU     24 * 160                ; row 24, column 0

start:
;--- before anything else, and deliberately not through DOS ----------
        mov     ax,0B800h
        mov     es,ax
        mov     word ptr es:[MARK],0F2Ah        ; '*'
        mov     word ptr es:[MARK+2],0F4Bh      ; 'K'

;--- an interrupt that is NOT RMDOS's, as a control ------------------
; If the BIOS answers and RMDOS does not, then the child is running
; with a working stack and a working interrupt table and the fault is
; in one DOS's front door - which is a much smaller place to look than
; "the child does not run".
        mov     ax,0E54h                ; teletype 'T'
        mov     bx,0007h
        int     10h
        mov     ax,0B800h
        mov     es,ax
        mov     word ptr es:[MARK+4],0F54h      ; 'T'

;--- what is the child's INT 21h actually going to do? ---------------
; RMDOS says the vector is its own and the parent's calls prove it was
; when the parent made them.  Neither is a statement about what is in
; the table NOW, after a PSP was built and 64K of arena was handed out
; - and the child's INT 21h goes wherever this word says, not wherever
; the loader believes it points.
        xor     ax,ax
        mov     ds,ax
        mov     bx,[21h*4]                      ; offset
        mov     dx,[21h*4 + 2]                  ; segment
        mov     ax,0B800h
        mov     es,ax
        mov     ax,dx
        mov     di,MARK + 12
        call    hex4
        mov     ax,bx
        mov     di,MARK + 22
        call    hex4

;--- now the part that needs a DOS underneath it ---------------------
        push    cs
        pop     ds
        mov     dx,OFFSET msg
        mov     ah,09h
        int     21h

        mov     ax,0B800h                       ; INT 21h is entitled to
        mov     es,ax                           ; have kept nothing
        mov     word ptr es:[MARK+6],0F44h      ; 'D'

        mov     ax,4C05h
        int     21h

;--- AX in hex to ES:DI, four cells, no BIOS and no DOS --------------
hex4:
        push    cx
        mov     cx,4
h4_one:
        rol     ax,4
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     h4_dig
        add     al,7
h4_dig:
        mov     ah,0Fh
        mov     es:[di],ax
        add     di,2
        pop     ax
        loop    h4_one
        pop     cx
        ret

msg     db  'PEKID: the child is running and DOS answered it.',13,10,'$'

_PK     ENDS
        END     start
