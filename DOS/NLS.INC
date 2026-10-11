;=====================================================================
; NLS.INC - the country: COUNTRY.SYS, and what 38h, 65h and 66h answer
;           from once CONFIG.SYS has named one
;
; THE KERNEL KNOWS ONE COUNTRY AND READS THE REST.  The United States
; and code page 437 are built in, as they are in MS-DOS, and they are
; what a machine with no COUNTRY= line has.  Every other country is a
; row in COUNTRY.SYS: its 34 bytes of 38h, the capitals of 80h-FFh,
; the collating weights and the letters for yes and no.  The file is
; in MS-DOS's own layout (CSYS_ in PMDOS.INC), so either DOS's file
; does.
;
; AND THERE IS NO NLSFUNC.  MS-DOS loaded the country it booted with
; from sysinit and needed a resident program for anything asked about
; another one afterwards - 38h for a country that is not the current
; one, 6501h, CHCP.  That program existed because DOS could not open a
; file from inside a DOS call.  This kernel can, so the three ask
; nls_load and nls_load reads the file.
;
; WHAT IS NOT DONE WITH IT.  The tables go where 65h's callers look
; (GNLS_ in PMDOS.INC) and 6520h-6522h answer from them.  File names
; are not put through them: a name is upper-cased in ASCII and a byte
; from 80h up is stored as it was typed, as before.  And nothing here
; changes what the screen shows - a code page is a set of tables and
; a keyboard layout, and the font is the video card's.
;
; Included by DOSMAIN.ASM.
;=====================================================================

; what nls_load says went wrong
NLE_NOFILE      EQU     1               ; the file would not open
NLE_BAD         EQU     2               ; it is not a COUNTRY.SYS
NLE_NOPAIR      EQU     3               ; no such country and code page in it

NLH_INFO        EQU     01h             ; what a pair's tables had: the
NLH_UCASE       EQU     02h             ; country block, the capitals,
NLH_COLLATE     EQU     04h             ; the weights, yes and no
NLH_YESNO       EQU     08h

CTRY_PATHMAX    EQU     80

;=====================================================================
; nls_load - EAX = a country code, EDX = a code page or 0 for the first
;            one the file has for that country, CL = 1 to make the pair
;            the one the system uses, 0 to read it and no more.
;
; -> CF clear: the pair's tables are in nls_s* - and are the system's
;    too, if that was asked.  CF set: EAX = an NLE_* reason, and
;    nothing has changed.
;
; Read into the staging area whole before anything is committed, so
; that a file with half a country in it leaves the one in use alone.
;=====================================================================
nls_load:
        pushad
        mov     dword ptr [nls_wcode],eax
        mov     dword ptr [nls_wpage],edx
        mov     byte ptr [nls_commit],cl
        mov     byte ptr [nls_have],0

        ; until COUNTRY= names a file it is COUNTRY.SYS where the
        ; system's other files are - beside the interpreter, or in the
        ; root of the drive the system started from (sys_open).  Looked
        ; for each time, and not remembered: the '?' stays, and says no
        ; file has been named.
        mov     dword ptr [nls_err],NLE_NOFILE
        cmp     byte ptr [ctry_path],'?'
        jne     nl_named
        mov     esi,OFFSET ctry_path + 3
        xor     al,al
        call    sys_open
        jc      nl_fail
        jmp     nl_opened
nl_named:
        mov     edx,OFFSET ctry_path
        mov     eax,3D00h
        call    k21
        jc      nl_fail
        movzx   eax,ax
nl_opened:
        mov     dword ptr [nls_hnd],eax

;--- the header: FFh,'COUNTRY', and where the list of pairs is --------
        mov     dword ptr [nls_err],NLE_BAD
        xor     ecx,ecx
        mov     edx,CSYS_HDRLEN
        call    nl_readat
        jc      nl_close
        cmp     dword ptr [nls_buf],554F43FFh           ; FFh,'COU'
        jne     nl_close
        cmp     dword ptr [nls_buf+4],5952544Eh         ; 'NTRY'
        jne     nl_close
        cmp     byte ptr [nls_buf+CSYS_TYPE],1
        jne     nl_close
        mov     ecx,dword ptr [nls_buf+CSYS_LIST]
        mov     edx,2
        call    nl_readat
        jc      nl_close
        movzx   ebx,word ptr [nls_buf]          ; how many pairs
        cmp     ebx,CSYS_MAXLIST
        ja      nl_close
        add     ecx,2

;--- the pair -------------------------------------------------------
nl_pair:
        or      ebx,ebx
        jz      nl_nopair
        mov     edx,CSE_SIZE
        call    nl_readat
        jc      nl_close
        movzx   eax,word ptr [nls_buf+CSE_COUNTRY]
        cmp     eax,dword ptr [nls_wcode]
        jne     nl_nextpair
        movzx   eax,word ptr [nls_buf+CSE_PAGE]
        cmp     dword ptr [nls_wpage],0         ; any page: the first is the
        je      nl_found                        ; country's own
        cmp     eax,dword ptr [nls_wpage]
        je      nl_found
nl_nextpair:
        movzx   eax,word ptr [nls_buf+CSE_LEN]
        lea     ecx,[ecx+eax+2]
        dec     ebx
        jmp     nl_pair
nl_nopair:
        mov     dword ptr [nls_err],NLE_NOPAIR
        jmp     nl_close

;--- its tables, one at a time ----------------------------------------
nl_found:
        mov     word ptr [nls_spage],ax
        mov     eax,dword ptr [nls_wcode]
        mov     word ptr [nls_scode],ax
        mov     ecx,dword ptr [nls_buf+CSE_TABLES]
        mov     edx,2
        call    nl_readat
        jc      nl_close
        movzx   ebx,word ptr [nls_buf]
        cmp     ebx,CSYS_MAXTAB
        ja      nl_close
        add     ecx,2
nl_tab:
        or      ebx,ebx
        jz      nl_tabsdone
        mov     edx,CSB_SIZE
        call    nl_readat
        jc      nl_close
        movzx   eax,word ptr [nls_buf+CSB_LEN]
        lea     eax,[ecx+eax+2]
        push    eax                             ; where the next entry is
        movzx   eax,word ptr [nls_buf+CSB_ID]
        mov     ecx,dword ptr [nls_buf+CSB_DATA]
        call    nl_table
        pop     ecx
        jc      nl_close
        dec     ebx
        jmp     nl_tab
nl_tabsdone:
        test    byte ptr [nls_have],NLH_INFO
        jz      nl_close                        ; a pair with no country block
        mov     dword ptr [nls_err],0           ; is not one

nl_close:
        mov     ebx,dword ptr [nls_hnd]
        mov     eax,3E00h
        call    k21
        cmp     dword ptr [nls_err],0
        jne     nl_fail
        cmp     byte ptr [nls_commit],0
        je      nl_ok
        call    nls_use
nl_ok:
        popad
        clc
        ret
nl_fail:
        popad
        mov     eax,dword ptr [nls_err]
        stc
        ret

;---------------------------------------------------------------------
; nl_readat - ECX = where in the file, EDX = how many bytes, 512 at
;             most -> they are in nls_buf.  CF if the file ends first.
;             Everything preserved.
;---------------------------------------------------------------------
nl_readat:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ebx,dword ptr [nls_hnd]
        mov     eax,4200h               ; k21's 42h takes the offset in ECX
        xor     edx,edx
        call    k21
        jc      nra_bad
        mov     edx,[esp]               ; the count
        mov     ecx,edx
        mov     ebx,dword ptr [nls_hnd]
        mov     edx,OFFSET nls_buf
        mov     eax,3F00h
        call    k21
        jc      nra_bad
        cmp     eax,[esp]
        jne     nra_bad
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
nra_bad:
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; nl_table - EAX = which table (CSI_*), ECX = where it is in the file.
;            The ones the kernel keeps go into the staging area; the
;            rest are passed over.  CF if one it keeps is not sound.
;
; Each is FFh, a seven-letter name, a length, and the bytes.  The name
; is not looked at - MS-DOS's own file calls table 4 "UCASE" in most of
; its entries and "FUCASE" in the rest.
;---------------------------------------------------------------------
nl_table:
        push    esi
        push    edi
        push    edx
        cmp     eax,CSI_INFO
        je      nlt_info
        cmp     eax,CSI_UCASE
        je      nlt_ucase
        cmp     eax,CSI_COLLATE
        je      nlt_collate
        cmp     eax,CSI_YESNO
        je      nlt_yesno
        jmp     nlt_ok                  ; 4, 5 and 7: file names keep the
                                        ; kernel's own rules, and there are
                                        ; no double-byte pages here

nlt_info:
        mov     edx,CSD_DATA + 4 + CTRY_SIZE
        call    nl_readat
        jc      nlt_bad
        cmp     byte ptr [nls_buf],0FFh
        jne     nlt_bad
        cmp     word ptr [nls_buf+CSD_LEN],4 + CTRY_SIZE
        jb      nlt_bad
        mov     esi,OFFSET nls_buf + CSD_DATA + 4       ; past the country
        mov     edi,OFFSET nls_sinfo                    ; and the code page
        mov     ecx,CTRY_SIZE
        rep     movsb
        or      byte ptr [nls_have],NLH_INFO
        jmp     nlt_ok

nlt_ucase:
        mov     edx,CSD_DATA + 128
        call    nl_readat
        jc      nlt_bad
        cmp     word ptr [nls_buf+CSD_LEN],128
        jb      nlt_bad
        mov     esi,OFFSET nls_buf + CSD_DATA
        mov     edi,OFFSET nls_sup
        mov     ecx,128
        rep     movsb
        or      byte ptr [nls_have],NLH_UCASE
        jmp     nlt_ok

nlt_collate:
        mov     edx,CSD_DATA + 256
        call    nl_readat
        jc      nlt_bad
        cmp     word ptr [nls_buf+CSD_LEN],256
        jb      nlt_bad
        mov     esi,OFFSET nls_buf + CSD_DATA
        mov     edi,OFFSET nls_scol
        mov     ecx,256
        rep     movsb
        or      byte ptr [nls_have],NLH_COLLATE
        jmp     nlt_ok

nlt_yesno:
        mov     edx,CSD_DATA + 4
        call    nl_readat
        jc      nlt_ok                  ; MS-DOS 6's file has none, and a
        cmp     word ptr [nls_buf+CSD_LEN],3    ; short one is none
        jb      nlt_ok
        mov     al,byte ptr [nls_buf+CSD_DATA]
        mov     byte ptr [nls_syes],al
        mov     al,byte ptr [nls_buf+CSD_DATA+2]
        mov     byte ptr [nls_sno],al
        or      byte ptr [nls_have],NLH_YESNO
nlt_ok:
        pop     edx
        pop     edi
        pop     esi
        clc
        ret
nlt_bad:
        pop     edx
        pop     edi
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; nls_use - what is staged becomes the country the system answers for.
;           Everything preserved.
;
; The tables go straight into the DOS data segment, where 65h's
; callers were handed pointers to them: a program that asked 6502h
; once and kept the answer reads the new country's capitals, which is
; what MS-DOS's did.  A pair with no table of capitals has the
; identity, and one with no weights keeps the weights there were.
;---------------------------------------------------------------------
nls_use:
        pushad
        mov     esi,OFFSET nls_sinfo
        mov     edi,OFFSET ctry_block
        mov     ecx,CTRY_SIZE
        rep     movsb
        mov     dword ptr [ctry_block+CTRY_CASEMAP],0   ; patched per call
        mov     ax,word ptr [nls_scode]
        mov     word ptr [ctry_code],ax
        mov     ax,word ptr [nls_spage]
        mov     word ptr [ctry_page],ax

        mov     edi,DOSDATA + GNLS_UPCASE + 2
        test    byte ptr [nls_have],NLH_UCASE
        jz      nlu_ident
        mov     esi,OFFSET nls_sup
        mov     ecx,128
        rep     movsb
        jmp     nlu_weights
nlu_ident:
        mov     al,80h
nlu_idlp:
        mov     [edi],al
        inc     edi
        inc     al
        jnz     nlu_idlp
nlu_weights:
        test    byte ptr [nls_have],NLH_COLLATE
        jz      nlu_yesno
        mov     esi,OFFSET nls_scol
        mov     edi,DOSDATA + GNLS_COLLATE + 2
        mov     ecx,256
        rep     movsb
nlu_yesno:
        mov     ax,'N' * 256 + 'Y'
        test    byte ptr [nls_have],NLH_YESNO
        jz      nlu_yn
        mov     al,byte ptr [nls_syes]
        mov     ah,byte ptr [nls_sno]
nlu_yn:
        mov     byte ptr [ctry_yes],al
        mov     byte ptr [ctry_no],ah
        popad
        ret

;---------------------------------------------------------------------
; nls_upchar - AL in upper case: ASCII's, and the country's table for
;              80h and up - the one 6502h hands out.
;---------------------------------------------------------------------
nls_upchar:
        cmp     al,80h
        jae     nuc_high
        jmp     cfg_upcase
nuc_high:
        push    ebx
        movzx   ebx,al
        mov     al,byte ptr ds:[DOSDATA + GNLS_UPCASE + 2 + ebx - 80h]
        pop     ebx
        ret

;---------------------------------------------------------------------
; nls_mapptr - EAX = the far address a 16-bit program calls to have AL
;              upper-cased: the twelve bytes at GNLS_CASEMAP.
;---------------------------------------------------------------------
nls_mapptr:
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GNLS_CASEMAP
        ret

;=====================================================================
; COUNTRY=code[,[page][,file]] - CONFIG.SYS, as MS-DOS takes it.
;
;   COUNTRY=049             Germany, in the first code page the file
;                           has for Germany
;   COUNTRY=049,850         ...in code page 850
;   COUNTRY=049,,C:\DOS\COUNTRY.SYS     ...out of that file
;
; With no file named it is COUNTRY.SYS beside the interpreter, which is
; where the system's files are, or in the root of the drive the system
; started from.  38h, 6501h and CHCP go back to the same file for a
; country or a code page that is not the one in use.
;
; This sat with the statements that do nothing until there were tables
; to load, with a note saying a second country was a data problem and
; not a parsing one.  COUNTRY.SYS is the data.
;=====================================================================
cfg_country:
        call    cfg_num
        jc      cfg_badparm
        or      eax,eax
        jz      cfg_badparm
        mov     dword ptr [nls_ccode],eax
        mov     dword ptr [nls_cpage],0
        mov     byte ptr [nls_newpath],0
        call    cfg_blanks
        cmp     byte ptr [esi],0
        je      ccy_load
        cmp     byte ptr [esi],','
        jne     cfg_badparm
        inc     esi
        call    cfg_blanks
        cmp     byte ptr [esi],0
        je      ccy_load
        cmp     byte ptr [esi],','
        je      ccy_file
        call    cfg_num
        jc      cfg_badparm
        or      eax,eax
        jz      cfg_badparm
        mov     dword ptr [nls_cpage],eax
        call    cfg_blanks
        cmp     byte ptr [esi],0
        je      ccy_load
        cmp     byte ptr [esi],','
        jne     cfg_badparm
ccy_file:
        inc     esi
        call    cfg_blanks
        cmp     byte ptr [esi],0
        je      ccy_load
        mov     edi,OFFSET ctry_path
        xor     ecx,ecx
ccy_name:
        mov     al,[esi]
        cmp     ecx,CTRY_PATHMAX - 1
        jae     cfg_badparm             ; before anything is changed: the
        or      al,al                   ; name goes in below
        jz      ccy_named
        inc     esi
        inc     ecx
        jmp     ccy_name
ccy_named:
        push    esi                     ; the file in use until now, in
        push    ecx                     ; case this one will not do
        mov     esi,OFFSET ctry_path
        mov     edi,OFFSET nls_oldpath
        mov     ecx,CTRY_PATHMAX
        rep     movsb
        pop     ecx
        pop     esi
        mov     byte ptr [nls_newpath],1
        mov     edi,OFFSET ctry_path
        sub     esi,ecx
        inc     ecx                     ; and its zero
        rep     movsb

ccy_load:
        mov     eax,dword ptr [nls_ccode]
        mov     edx,dword ptr [nls_cpage]
        mov     cl,1
        call    nls_load
        jc      ccy_fail
        mov     ax,word ptr [ctry_page]         ; the page the system starts
        mov     word ptr [ctry_syspage],ax      ; in: 6601h's DX
        ret
ccy_fail:
        push    eax
        call    cfg_show_line
        pop     eax
        mov     esi,OFFSET msgCtryNoFile
        cmp     eax,NLE_NOFILE
        je      ccy_say
        mov     esi,OFFSET msgCtryBad
        cmp     eax,NLE_BAD
        je      ccy_say
        mov     esi,OFFSET msgCtryNoPair
ccy_say:
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET ctry_path
        cmp     byte ptr [esi],'?'      ; no file was named: the name alone,
        jne     ccy_which               ; or the one sys_open found
        add     esi,3
        cmp     dword ptr [nls_err],NLE_NOFILE
        je      ccy_which
        mov     esi,OFFSET sys_path
ccy_which:
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
        ; a file that would not do is not the one 38h and CHCP go
        ; back to: the one before it is
        cmp     byte ptr [nls_newpath],0
        je      ccy_out
        mov     esi,OFFSET nls_oldpath
        mov     edi,OFFSET ctry_path
        mov     ecx,CTRY_PATHMAX
        rep     movsb
ccy_out:
        ret

;=====================================================================
; kl_check - the record in kl_buf, ECX bytes of it: is it a layout the
;            keyboard driver can follow without looking where it steps?
;            CF if not.  Everything preserved.
;
; KEYB reads it out of a file and anybody can write a file, and the
; driver walks it from an interrupt: so every count is checked against
; the length here, once, and the driver checks nothing.
;=====================================================================
kl_check:
        pushad
        cmp     word ptr [kl_buf+KBL_SIG],KBL_MAGIC
        jne     klc_bad
        movzx   eax,byte ptr [kl_buf+KBL_NKEYS]
        cmp     eax,KBL_MAXKEYS
        ja      klc_bad
        movzx   edx,byte ptr [kl_buf+KBL_NDEAD]
        cmp     edx,KBL_MAXDEAD
        ja      klc_bad
        imul    ebx,eax,KBLK_LEN
        add     ebx,KBL_KEYS            ; EBX = where the dead keys start
        cmp     ebx,ecx
        ja      klc_bad

        ; a key that says one of its levels is a dead key names one
        ; there is
        mov     esi,OFFSET kl_buf + KBL_KEYS
klc_key:
        or      eax,eax
        jz      klc_dead
        mov     edi,3
klc_level:
        push    ecx
        mov     ecx,edi
        mov     ch,KBLK_DEAD
        shl     ch,cl
        test    byte ptr [esi+KBLK_FLAGS],ch
        pop     ecx
        jz      klc_notdead
        cmp     byte ptr [esi+KBLK_CHARS+edi],dl
        jae     klc_bad
klc_notdead:
        dec     edi
        jns     klc_level
        add     esi,KBLK_LEN
        dec     eax
        jmp     klc_key

        ; and each dead key's pairs end inside the record
klc_dead:
        or      edx,edx
        jz      klc_ok
        lea     eax,[ebx+2]
        cmp     eax,ecx
        ja      klc_bad
        movzx   eax,byte ptr [kl_buf+ebx+1]
        lea     ebx,[ebx+2+eax*2]
        cmp     ebx,ecx
        ja      klc_bad
        dec     edx
        jmp     klc_dead
klc_ok:
        popad
        clc
        ret
klc_bad:
        popad
        stc
        ret

;=====================================================================
; Data
;=====================================================================
ctry_code   dw  CTRY_USA                ; the country in use,
ctry_page   dw  CTRY_CP437              ; its code page, which CHCP can
ctry_syspage dw CTRY_CP437              ; change, and the one it began with
ctry_yes    db  'Y'                     ; what 6523h takes for yes
ctry_no     db  'N'                     ; ...and for no
ctry_path   db  '?:\COUNTRY.SYS',0      ; the file COUNTRY= named; '?', none
            db  CTRY_PATHMAX - 15 dup(0)        ; yet, and nls_load looks for
                                                ; the name after it

nls_wcode   dd  0                       ; the pair nls_load is after
nls_wpage   dd  0
nls_ccode   dd  0                       ; ...and the one COUNTRY= named
nls_cpage   dd  0
nls_hnd     dd  0
nls_err     dd  0
nls_scode   dw  0                       ; the pair that was found
nls_spage   dw  0
nls_commit  db  0
nls_have    db  0                       ; NLH_*: which tables it had
nls_syes    db  0
nls_sno     db  0
nls_newpath db  0                       ; COUNTRY= named a file of its own
            db  0,0,0

msgCtryNoFile   db  'COUNTRY=: cannot open ',0
msgCtryBad      db  'COUNTRY=: not a country information file: ',0
msgCtryNoPair   db  'COUNTRY=: no such country code or code page in ',0

; Below the arena and not in the file - see KBSS_BASE in PMDOS.INC.
_DOS    ENDS
_KBSS   SEGMENT
IF ($ - kbss_start) AND 3
        .ERR    nls_buf in _KBSS is not on a dword
ENDIF
nls_buf     db  512 dup(?)              ; a piece of COUNTRY.SYS
nls_sinfo   db  CTRY_SIZE dup(?)        ; what a pair had: 38h's block,
            db  2 dup(?)
nls_sup     db  128 dup(?)              ; the capitals of 80h-FFh,
nls_scol    db  256 dup(?)              ; and a weight a character
nls_oldpath db  CTRY_PATHMAX dup(?)     ; ctry_path, while COUNTRY= tries
                                        ; another
kl_buf      db  KBL_MAX dup(?)          ; KEYB's layout, which the keyboard
                                        ; driver reads where it lies
IF ($ - kbss_start) AND 3
        .ERR    what follows kl_buf in _KBSS is not on a dword
ENDIF
_KBSS   ENDS
_DOS    SEGMENT
