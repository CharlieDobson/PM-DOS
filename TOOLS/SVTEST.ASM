;=====================================================================
; SVTEST.ASM - SVTEST.COM: what a DOS program is told its DOS is
;
;   SVTEST [/F] [program]
;
; One line, the program's own file name and then three versions:
;
;   SVTEST.COM   30h 6.22  3306h 0.68  PSP 6.22
;
; 30h is the one SETVER changes; 3306h is the one it must not; PSP is
; the word at PSP+40h that the kernel (or RMDOS, under Windows) stamped
; when it loaded this program, and that 30h reads back.  The line above
; is the table SETVER.EXE ships, whose "*" entry is 6.22; with no
; SETVER loaded all three are PM-DOS's own version.  Copied under a
; name the table has - SVKID.COM, with SVKID.COM 5.00 in it - it should
; say 5.00, 0.68 and 5.00.
;
; With a program named it then EXECs it, with an empty tail or with
; /F, which is how the lookup EXEC does is tested from inside a DOS
; box.  /F appends the line to C:\SV.TXT instead of standard output,
; because under RMDOS nothing else can be read headless - a file that
; PM-DOS does not have open is safe to write from there, and FATGET.PL
; reads it back.
;
; WITH NO ENVIRONMENT AT ALL, which is how PARKTEST starts a program on
; RMDOS (PARK.INC builds that PSP with none and an empty tail), it is
; SVTEST /F C:\SVKID.COM: the EXEC is RMDOS's, and so is the lookup.
;
; The exit code is the major version 30h gave, for IF ERRORLEVEL.
;
; Linked with:  wlink form raw bin ... order clname CODE offset=0x100
;=====================================================================
        .386
_SVT    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SVT, DS:_SVT, ES:_SVT, SS:_SVT
        ORG     0

start:
        mov     sp,OFFSET stk_top
        mov     bx,OFFSET prog_end      ; give back what a .COM is handed,
        add     bx,15                   ; or there is nothing to EXEC into
        shr     bx,4
        mov     ah,4Ah
        int     21h

        call    args                    ; [fmode], kid_path
        call    out_open                ; [outh]
        call    put_self
        mov     dx,OFFSET msg_30
        call    put_str
        mov     ax,3000h
        int     21h
        mov     [ver30],ax
        call    put_ver
        mov     dx,OFFSET msg_3306
        call    put_str
        mov     ax,3306h
        int     21h
        mov     ax,bx                   ; BL major, BH minor
        call    put_ver
        mov     dx,OFFSET msg_psp
        call    put_str
        mov     ax,ds:[40h]
        call    put_ver
        mov     dx,OFFSET msg_crlf
        call    put_str

        call    out_close               ; the child appends after this
        cmp     byte ptr [kid_path],0
        je      finish
        mov     word ptr [pb_tail+2],cs
        mov     word ptr [pb_fcb1],5Ch
        mov     word ptr [pb_fcb1+2],cs
        mov     word ptr [pb_fcb2],6Ch
        mov     word ptr [pb_fcb2+2],cs
        mov     [save_sp],sp
        mov     dx,OFFSET kid_path
        mov     bx,OFFSET pblock
        mov     ax,4B00h
        int     21h
        mov     bx,cs                   ; DOS 2 lost these, and nothing
        cli                             ; since has promised them
        mov     ss,bx
        mov     sp,cs:[save_sp]
        sti
        mov     ds,bx
        mov     es,bx
        jnc     finish
        add     al,'0'
        mov     [msg_code],al
        mov     dx,OFFSET msg_exfail
        call    put_str

finish:
        mov     ax,[ver30]
        mov     ah,4Ch
        int     21h

;--- args - /F and a program name out of the tail ---------------------
args:
        cmp     byte ptr ds:[80h],0     ; a tail, even with no environment,
        jne     ar_tail                 ; is a child of this program's: its
        cmp     word ptr ds:[2Ch],0     ; /F, and no child of its own, or
        jne     ar_tail                 ; it would start one for ever
        mov     byte ptr [fmode],1      ; no environment: PARKTEST's PSP
        mov     si,OFFSET str_kid
        mov     di,OFFSET kid_path
ar_kidcopy:
        lodsb
        stosb
        or      al,al
        jnz     ar_kidcopy
        jmp     short ar_kidp
ar_tail:
        mov     si,81h
        movzx   cx,byte ptr ds:[80h]
ar_skip:
        jcxz    ar_done
        lodsb
        dec     cx
        cmp     al,' '
        je      ar_skip
        cmp     al,9
        je      ar_skip
        cmp     al,13
        je      ar_done
        cmp     al,'/'
        jne     ar_word
        jcxz    ar_done
        lodsb                           ; /F - nothing else is taken
        dec     cx
        or      al,20h
        cmp     al,'f'
        jne     ar_skip
        mov     byte ptr [fmode],1
        jmp     short ar_skip
ar_word:
        mov     di,OFFSET kid_path
ar_copy:
        stosb
        cmp     di,OFFSET kid_path + 79
        jae     ar_named
        jcxz    ar_named
        lodsb
        dec     cx
        cmp     al,' '
        ja      ar_copy
ar_named:
        mov     byte ptr [di],0
        cmp     byte ptr [fmode],0
        je      ar_done
ar_kidp:
        mov     si,OFFSET kid_ftail     ; and the child reports to the file too
        mov     word ptr [pb_tail],si
ar_done:
        ret

;--- out_open - [outh] = standard output, or C:\SV.TXT opened at its end -
out_open:
        cmp     byte ptr [fmode],0
        je      oo_out
        mov     dx,OFFSET str_file
        mov     ax,3D02h
        int     21h
        jnc     oo_opened
        mov     dx,OFFSET str_file
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      oo_out                  ; standard output, then
oo_opened:
        mov     [outh],ax
        mov     bx,ax
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
oo_out:
        ret

;--- out_close - and closed again, before a child opens it -----------
out_close:
        mov     bx,[outh]
        cmp     bx,4
        jbe     oc_out
        mov     ah,3Eh
        int     21h
        mov     word ptr [outh],1
oc_out:
        ret

;--- put_self - the file name this program was run as, padded to 12 --
put_self:
        mov     ax,ds:[2Ch]
        or      ax,ax
        jz      ps_none
        mov     es,ax
        xor     di,di
        cmp     byte ptr es:[di],0
        je      ps_end
ps_find:
        cmp     di,7FF0h
        jae     ps_none
        mov     al,es:[di]
        inc     di
        or      al,al
        jnz     ps_find
        cmp     byte ptr es:[di],0
        jne     ps_find
ps_end:
        add     di,3                    ; the zero, and the count
        mov     si,di                   ; the name starts after the last
ps_scan:                                ; \ or :
        mov     al,es:[di]
        inc     di
        or      al,al
        jz      ps_based
        cmp     al,'\'
        je      ps_mark
        cmp     al,':'
        jne     ps_scan
ps_mark:
        mov     si,di
        jmp     short ps_scan
ps_based:
        mov     di,OFFSET line
        mov     cx,12
ps_copy:
        mov     al,es:[si]
        or      al,al
        jz      ps_pad
        mov     [di],al
        inc     si
        inc     di
        loop    ps_copy
ps_pad:
        jcxz    ps_put
        mov     byte ptr [di],' '
        inc     di
        loop    ps_pad
ps_put:
        mov     byte ptr [di],0
        push    ds
        pop     es
        mov     dx,OFFSET line
        jmp     short put_str
ps_none:
        push    ds
        pop     es
        mov     dx,OFFSET msg_noname
        ; falls into put_str

;--- put_str - the ASCIIZ text at DX to [outh] ------------------------
put_str:
        push    si
        mov     si,dx
        xor     cx,cx
pst_len:
        cmp     byte ptr [si],0
        je      pst_put
        inc     si
        inc     cx
        jmp     short pst_len
pst_put:
        mov     bx,[outh]
        mov     ah,40h
        int     21h
        pop     si
        ret

;--- put_ver - AL.AH as n.nn ------------------------------------------
put_ver:
        push    ax
        aam
        add     ax,3030h
        cmp     ah,'0'
        je      pv_one
        mov     [vbuf],ah
        mov     [vbuf+1],al
        mov     byte ptr [vbuf+2],0
        jmp     short pv_major
pv_one:
        mov     [vbuf],al
        mov     byte ptr [vbuf+1],0
pv_major:
        mov     dx,OFFSET vbuf
        call    put_str
        pop     ax
        mov     al,ah
        aam
        add     ax,3030h
        mov     byte ptr [vbuf],'.'
        mov     [vbuf+1],ah
        mov     [vbuf+2],al
        mov     byte ptr [vbuf+3],0
        mov     dx,OFFSET vbuf
        jmp     short put_str

msg_30      db  ' 30h ',0
msg_3306    db  '  3306h ',0
msg_psp     db  '  PSP ',0
msg_crlf    db  13,10,0
msg_noname  db  '(no name)   ',0
msg_exfail  db  'EXEC failed, error '
msg_code    db  '?',13,10,0
str_kid     db  'C:\SVKID.COM',0
str_file    db  'C:\SV.TXT',0

outh        dw  1
fmode       db  0
ver30       dw  0
save_sp     dw  0
pblock      dw  0                       ; the parent's environment
pb_tail     dw  OFFSET kid_tail, 0
pb_fcb1     dw  0, 0
pb_fcb2     dw  0, 0
kid_tail    db  0,13
kid_ftail   db  3,' /F',13
vbuf        db  8 dup(0)
line        db  16 dup(0)
kid_path    db  80 dup(0)
            db  256 dup(0)
stk_top:
prog_end:

_SVT    ENDS
        END     start
