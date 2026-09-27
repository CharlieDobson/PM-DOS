;
; STUB.ASM - real-mode stub for LABEL.EXE (the native 32-bit DOS build).
;
; A 32-bit DOS loads the LE image that follows this stub.  Anything
; that runs the file as a plain MS-DOS program gets this message
; instead.  Open Watcom WASM, linked with "format dos".
;
        .8086
_TEXT   segment byte public 'CODE'
        assume  cs:_TEXT, ds:_TEXT
start:
        push    cs
        pop     ds
        mov     dx,offset msg
        mov     ah,9
        int     21h
        mov     ax,4CFFh
        int     21h
msg     db      'This program requires a 32-bit DOS.',13,10,'$'
_TEXT   ends

_STACK  segment para stack 'STACK'
        db      128 dup(?)
_STACK  ends

        end     start
