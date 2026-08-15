;=====================================================================
; SORT.ASM - SORT.COM, the classic line sorter, an EXTERNAL command
;
;   SORT            sort standard input:  DIR | SORT,  SORT < F
;   SORT file       sort the named file (quotes around a long name
;                   are fine)
;   /R              descending
;   /+n             compare from column n
;
; Reads everything, splits it into lines, sorts them case-
; insensitively (as DOS did), writes them to standard output.  The
; classic one managed 64K; the transient slot affords 256K of text
; and 8192 lines, all of it in the D32 BSS so the .COM on disk stays
; small - the loader zeroes it into place.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

TEXT_MAX        EQU     40000h          ; 256K of line text
LINES_MAX       EQU     8192
FNAME_MAX       EQU     80

_SORT   SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_SORT, DS:_SORT, ES:_SORT, SS:_SORT
        ORG     0

;=====================================================================
; D32 header - text and pointer arrays live in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET so_entry>,<TEXT_MAX + (LINES_MAX * 4)>,4096

;=====================================================================
so_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [txt_base],eax
        add     eax,TEXT_MAX
        mov     [ptr_base],eax

;--- the PSP tail: /R, /+n, an optional input filename ---------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
so_arg:
        or      ecx,ecx
        jz      so_argdone
        mov     al,[esi]
        cmp     al,0Dh
        je      so_argdone
        cmp     al,' '
        je      so_argstep
        cmp     al,9
        je      so_argstep
        cmp     al,'/'
        je      so_slash
        jmp     short so_fname
so_argstep:
        inc     esi
        dec     ecx
        jmp     short so_arg

so_slash:
        inc     esi
        dec     ecx
        jz      so_argdone
        mov     al,[esi]
        cmp     al,'r'
        je      so_rev
        cmp     al,'R'
        je      so_rev
        cmp     al,'+'
        je      so_col
        inc     esi                     ; unknown switch: ignored
        dec     ecx
        jmp     short so_arg
so_rev:
        mov     byte ptr [rev_flag],1
        inc     esi
        dec     ecx
        jmp     short so_arg
so_col:
        inc     esi                     ; past the '+'
        dec     ecx
        xor     eax,eax
so_cdig:
        or      ecx,ecx
        jz      so_chave
        movzx   edx,byte ptr [esi]
        sub     edx,'0'
        cmp     edx,9
        ja      so_chave
        imul    eax,10
        add     eax,edx
        inc     esi
        dec     ecx
        jmp     short so_cdig
so_chave:
        or      eax,eax                 ; columns are 1-based
        jz      so_arg
        dec     eax
        mov     [cmp_col],eax
        jmp     short so_arg

so_fname:
        xor     ah,ah                   ; AH = saw a leading quote
        cmp     al,'"'
        jne     so_fcp
        inc     ah
        inc     esi
        dec     ecx
so_fcp:
        mov     edi,OFFSET fname
        xor     edx,edx
so_flp:
        or      ecx,ecx
        jz      so_fend
        mov     al,[esi]
        cmp     al,0Dh
        je      so_fend
        cmp     al,'"'
        je      so_fend
        or      ah,ah
        jnz     so_fkeep
        cmp     al,' '
        je      so_fend
        cmp     al,9
        je      so_fend
so_fkeep:
        cmp     edx,FNAME_MAX - 1
        jae     so_fskip
        mov     [edi],al
        inc     edi
        inc     edx
so_fskip:
        inc     esi
        dec     ecx
        jmp     short so_flp
so_fend:
        mov     byte ptr [edi],0
        or      edx,edx
        jz      so_argdone
        mov     edx,OFFSET fname
        mov     ax,3D00h
        int     21h
        jc      so_cant
        movzx   eax,ax
        mov     [in_hdl],eax
so_argdone:

;--- slurp the whole input -------------------------------------------
        xor     edi,edi                 ; EDI = bytes so far
so_read:
        mov     ecx,TEXT_MAX - 1        ; one byte stays zero, so a
        sub     ecx,edi                 ; final line with no newline
        jz      so_split                ; always finds its terminator
        mov     ebx,[in_hdl]
        mov     edx,[txt_base]
        add     edx,edi
        mov     ah,3Fh
        int     21h
        jc      so_split
        or      eax,eax
        jz      so_split
        add     edi,eax
        jmp     short so_read

;--- carve it into NUL-terminated lines ------------------------------
so_split:
        mov     esi,[txt_base]
        mov     edx,esi
        add     edx,edi                 ; EDX = end of the text
        xor     ebx,ebx                 ; EBX = line count
so_line:
        cmp     esi,edx
        jae     so_sorted0
        cmp     byte ptr [esi],1Ah      ; ^Z ends a text file
        je      so_sorted0
        cmp     ebx,LINES_MAX
        jae     so_sorted0
        mov     eax,[ptr_base]          ; this line starts here
        mov     [eax+ebx*4],esi
        inc     ebx
so_scan:
        cmp     esi,edx
        jae     so_lend
        mov     al,[esi]
        cmp     al,13
        je      so_cr
        cmp     al,10
        je      so_lf
        cmp     al,1Ah
        je      so_ctlz
        inc     esi
        jmp     short so_scan
so_cr:
        mov     byte ptr [esi],0
        inc     esi
        cmp     esi,edx
        jae     so_line
        cmp     byte ptr [esi],10       ; CR LF is one ending
        jne     so_line
        inc     esi
        jmp     short so_line
so_lf:
        mov     byte ptr [esi],0
        inc     esi
        jmp     short so_line
so_ctlz:
        mov     byte ptr [esi],0        ; terminate the final line...
        mov     edx,esi                 ; ...and stop scanning
        jmp     short so_line
so_lend:
        jmp     short so_line           ; the text end terminates it
                                        ; (the BSS byte there is zero)

;--- insertion sort on the pointer array -----------------------------
so_sorted0:
        mov     [nlines],ebx
        cmp     ebx,2
        jb      so_out
        mov     ecx,1
so_outer:
        cmp     ecx,[nlines]
        jae     so_out
        mov     ebp,[ptr_base]
        mov     eax,[ebp+ecx*4]         ; the line being placed
        mov     edx,ecx
so_inner:
        or      edx,edx
        jz      so_place
        mov     esi,[ebp+edx*4-4]       ; the line above it
        mov     edi,eax
        call    so_cmp                  ; CF: above sorts AFTER ours
        jnc     so_place
        mov     esi,[ebp+edx*4-4]
        mov     [ebp+edx*4],esi
        dec     edx
        jmp     short so_inner
so_place:
        mov     [ebp+edx*4],eax
        inc     ecx
        jmp     short so_outer

;--- write the lines out, forward or backward ------------------------
so_out:
        cmp     dword ptr [nlines],0
        je      so_done
        xor     ecx,ecx                 ; ascending cursor...
        mov     edx,[nlines]
        dec     edx                     ; ...or descending
so_wlp:
        mov     ebp,[ptr_base]
        cmp     byte ptr [rev_flag],0
        je      so_wfwd
        mov     esi,[ebp+edx*4]
        jmp     short so_wr
so_wfwd:
        mov     esi,[ebp+ecx*4]
so_wr:
        call    so_puts
        inc     ecx
        dec     edx
        cmp     ecx,[nlines]
        jb      so_wlp
so_done:
        mov     ax,4C00h
        int     21h

so_cant:
        mov     edx,OFFSET msg_nofile
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; so_cmp - CF set when the line at ESI sorts AFTER the line at EDI.
;          Case-insensitive from column [cmp_col]; a line shorter
;          than the column counts as empty, as DOS had it.
;---------------------------------------------------------------------
so_cmp:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     ecx,[cmp_col]
sc_col:
        or      ecx,ecx
        jz      sc_lp
        cmp     byte ptr [esi],0
        je      sc_c2
        inc     esi
sc_c2:
        cmp     byte ptr [edi],0
        je      sc_c3
        inc     edi
sc_c3:
        dec     ecx
        jmp     short sc_col
sc_lp:
        mov     al,[esi]
        mov     ah,[edi]
        call    sc_fold
        xchg    al,ah
        call    sc_fold
        cmp     ah,al                   ; AH = ours, AL = theirs
        ja      sc_after
        jb      sc_before
        or      ah,ah                   ; equal - both ended?
        jz      sc_before               ; equal lines keep their order
        inc     esi
        inc     edi
        jmp     short sc_lp
sc_after:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret
sc_before:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret

sc_fold:
        cmp     al,'a'
        jb      sf_out
        cmp     al,'z'
        ja      sf_out
        sub     al,20h
sf_out:
        ret

;---------------------------------------------------------------------
; so_puts - the ASCIIZ line at ESI to stdout, CR LF after it
;---------------------------------------------------------------------
so_puts:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     edi,esi
sp_len:
        cmp     byte ptr [edi],0
        je      sp_have
        inc     edi
        jmp     short sp_len
sp_have:
        mov     ecx,edi
        sub     ecx,esi
        jz      sp_nl
        mov     ebx,1
        mov     edx,esi
        mov     ah,40h
        int     21h
sp_nl:
        mov     ebx,1
        mov     ecx,2
        mov     edx,OFFSET crlf
        mov     ah,40h
        int     21h
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; Data
;=====================================================================
in_hdl      dd  0                       ; stdin unless a file was named
txt_base    dd  0
ptr_base    dd  0
nlines      dd  0
cmp_col     dd  0
rev_flag    db  0

crlf        db  13,10
msg_nofile  db  'File not found',13,10,'$'
fname       db  FNAME_MAX dup(0)

img_end:                                ; the BSS begins here

_SORT   ENDS
        END
