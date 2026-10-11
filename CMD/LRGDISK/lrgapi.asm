;=====================================================================
; LRGAPI.ASM - LRGSTAT's calls to LRGDISK.386, for a Windows program
; built small model: the entry INT 2Fh AX=1684h gives for LRGDISK's
; device number, and the two things the driver answers there
; (lrgdisk.asm, ld_api).  C names and C calls: arguments on the stack,
; taken off by the caller; SI, DI, BP, DS and ES kept.
;
; Assembles with wasm and with MASM 6.1x.
;=====================================================================
        .386
LD_DEVICE_ID    EQU     4C44h           ; lrgdisk.asm's

_TEXT   SEGMENT WORD USE16 PUBLIC 'CODE'
        ASSUME  cs:_TEXT

        PUBLIC  _LrgEntry
        PUBLIC  _LrgVersion
        PUBLIC  _LrgAccount

;---------------------------------------------------------------------
; unsigned long LrgEntry( void ) - the entry, or 0: no LRGDISK with an
; API is loaded, or Windows is not in 386 enhanced mode
;---------------------------------------------------------------------
_LrgEntry       PROC    NEAR
        push    di
        push    es
        xor     di,di
        mov     es,di
        mov     ax,1684h
        mov     bx,LD_DEVICE_ID
        int     2Fh
        mov     ax,di
        mov     dx,es
        pop     es
        pop     di
        ret
_LrgEntry       ENDP

;---------------------------------------------------------------------
; unsigned LrgVersion( unsigned long entry, unsigned far *size ) -
; LRGDISK's version, and the bytes in its account into *size; 0 if the
; driver refused
;---------------------------------------------------------------------
_LrgVersion     PROC    NEAR
        push    bp
        mov     bp,sp
        push    di
        push    es
        xor     ax,ax
        call    dword ptr [bp+4]
        jc      lv_no
        les     di,dword ptr [bp+8]
        mov     es:[di],bx
        jmp     short lv_out
lv_no:
        xor     ax,ax
lv_out:
        pop     es
        pop     di
        pop     bp
        ret
_LrgVersion     ENDP

;---------------------------------------------------------------------
; unsigned LrgAccount( unsigned long entry, void far *buf, unsigned
; size ) - the account into buf, as much as fits; the bytes given, 0
; if the driver refused
;---------------------------------------------------------------------
_LrgAccount     PROC    NEAR
        push    bp
        mov     bp,sp
        push    di
        push    es
        les     di,dword ptr [bp+8]
        mov     cx,[bp+12]
        mov     ax,1
        call    dword ptr [bp+4]
        mov     ax,cx
        jnc     la_out
        xor     ax,ax
la_out:
        pop     es
        pop     di
        pop     bp
        ret
_LrgAccount     ENDP

_TEXT   ENDS
        END
