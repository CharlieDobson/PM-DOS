;=====================================================================
; CACHE.ASM - CACHE.EXE, the disk cache's status and controls
;
;   CACHE               what is cached, and how
;   CACHE /S            ...with the element count and the hit rate
;   CACHE /C            write everything the cache is holding
;   CACHE /R            ...and then empty it
;   CACHE /X            no write-behind on any drive
;   CACHE C+ A- D       write caching on for C:, no caching at all
;                       for A:, read caching only for D:
;   CACHE /Q            do the above and print nothing
;
; THIS IS SMARTDRV /S FOR A CACHE THAT IS PART OF THE KERNEL.  The
; report is laid out the way SMARTDRV lays out its own, line for
; line, because that is the report a DOS user knows how to read; the
; switches are the ones of SMARTDRV's that still mean something when
; the cache cannot be loaded, unloaded or given a size from a command
; line.  Its size is CACHE='s business in CONFIG.SYS and the arena's
; at run time, which is why the report shows three sizes where
; SMARTDRV shows two: what the cache holds now, the floor a reclaim
; will not take it below, and the ceiling it may grow to on a miss.
;
; What it asks:
;   21h/F1h AL=0     the kernel's version, for the first line
;   21h/F1h AL=16h   the cache - its status record, a commit, a reset,
;                    and one letter's caching, asked or changed.  The
;                    same questions a SMARTDRV-aware program puts to
;                    INT 2Fh AX=4A10h from a DOS box, which a native
;                    program has no INT 2Fh to ask
;   21h/38h          the thousands separator
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

LETTERS         EQU     26              ; A: to Z:
DOPS_MAX        EQU     32              ; drive requests on one line

; 21h/F1h AL=16h with the CCF_* function in bits 16-23, spelled out
F1_CSTATUS      EQU     (CCF_STATUS SHL 16) OR 0F116h
F1_CCOMMIT      EQU     (CCF_COMMIT SHL 16) OR 0F116h
F1_CRESET       EQU     (CCF_RESET SHL 16) OR 0F116h
F1_CDRIVE       EQU     (CCF_DRIVE SHL 16) OR 0F116h

_DC     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_DC, DS:_DC, ES:_DC, SS:_DC
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET dc_entry>,<0>,4096

;=====================================================================
dc_entry:
        call    get_sep
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi

;---------------------------------------------------------------------
; The command line.  Switches and drive letters in any order; a drive
; letter may carry a colon and then a '+' or a '-'.  Nothing is
; changed until the whole line has been read, so a line with a
; mistake in it does nothing at all.
;---------------------------------------------------------------------
da_next:
        or      ecx,ecx
        jz      da_done
        mov     al,[esi]
        cmp     al,0Dh
        je      da_done
        cmp     al,' '
        je      da_skip
        cmp     al,9
        je      da_skip
        mov     [tok_start],esi
        cmp     al,'/'
        je      da_switch

        ; a drive letter
        call    upcase
        cmp     al,'A'
        jb      da_bad
        cmp     al,'Z'
        ja      da_bad
        sub     al,'A'
        mov     [dop_letter],al
        mov     byte ptr [dop_req],CCD_WROFF    ; a bare letter: reads only
        inc     esi
        dec     ecx
        jz      da_drvgo
        mov     al,[esi]
        cmp     al,':'
        jne     da_sign
        inc     esi
        dec     ecx
        jz      da_drvgo
        mov     al,[esi]
da_sign:
        cmp     al,'+'
        jne     da_minus
        mov     byte ptr [dop_req],CCD_WRON
        jmp     short da_drvend
da_minus:
        cmp     al,'-'
        jne     da_drvsep
        mov     byte ptr [dop_req],CCD_RDOFF
da_drvend:
        inc     esi
        dec     ecx
        jz      da_drvgo
        mov     al,[esi]
da_drvsep:
        call    is_sep                          ; "C+X" is not a request
        jne     da_bad
da_drvgo:
        ; Does the letter name something the cache keys on?  Asked
        ; now, with nothing changed, so that a bad letter stops the
        ; whole line rather than half of it.
        mov     eax,F1_CDRIVE
        mov     dl,[dop_letter]
        mov     dh,CCD_GET
        int     21h
        jc      da_bad
        movzx   ebx,byte ptr [n_dops]
        cmp     ebx,DOPS_MAX
        jae     da_bad
        mov     al,[dop_letter]
        mov     [dops+ebx*2],al
        mov     al,[dop_req]
        mov     [dops+ebx*2+1],al
        inc     byte ptr [n_dops]
        jmp     da_next

da_skip:
        inc     esi
        dec     ecx
        jmp     da_next

da_switch:
        inc     esi
        dec     ecx
        jz      da_bad
        mov     al,[esi]
        call    upcase
        mov     ah,al
        inc     esi
        dec     ecx
        jz      da_swend
        mov     al,[esi]
        call    is_sep                          ; "/SX" is not a switch
        jne     da_bad
da_swend:
        cmp     ah,'?'
        je      da_help
        mov     ebx,OFFSET sw_x
        cmp     ah,'X'
        je      da_swset
        mov     ebx,OFFSET sw_c
        cmp     ah,'C'
        je      da_swset
        mov     ebx,OFFSET sw_r
        cmp     ah,'R'
        je      da_swset
        mov     ebx,OFFSET sw_q
        cmp     ah,'Q'
        je      da_swset
        mov     ebx,OFFSET sw_s
        cmp     ah,'S'
        je      da_swset
        jmp     short da_bad
da_swset:
        mov     byte ptr [ebx],1
        jmp     da_next

da_help:
        mov     edx,OFFSET msg_usage
        call    print
        mov     ax,4C00h
        int     21h

; Invalid parameter - and the parameter, as it was typed, up to the
; blank after it.
da_bad:
        mov     edx,OFFSET msg_badparm
        call    print
        mov     esi,[tok_start]
db_lp:
        mov     al,[esi]
        call    is_sep
        je      db_end
        call    putc
        inc     esi
        jmp     short db_lp
db_end:
        call    crlf
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; Do what the line asked, in SMARTDRV's order: the drives, then /X,
; then the commit or the reset, then the report.
;---------------------------------------------------------------------
da_done:
        call    get_status
        test    byte ptr [cst+CST_FLAGS],CSTF_ON
        jz      no_cache

        movzx   ecx,byte ptr [n_dops]
        xor     ebx,ebx
do_dops:
        jecxz   do_x
        mov     eax,F1_CDRIVE
        mov     dl,[dops+ebx*2]
        mov     dh,[dops+ebx*2+1]
        int     21h
        inc     ebx
        dec     ecx
        jmp     short do_dops

do_x:
        cmp     byte ptr [sw_x],0
        je      do_commit
        mov     ecx,LETTERS
        xor     ebx,ebx
dx_lp:
        mov     eax,F1_CDRIVE
        mov     dl,bl
        mov     dh,CCD_WROFF
        int     21h                             ; a letter that is nothing
        inc     ebx                             ; says so and is not an error
        loop    dx_lp

do_commit:
        cmp     byte ptr [sw_r],0
        je      dc_notr
        mov     eax,F1_CRESET
        int     21h
        jmp     short do_report
dc_notr:
        cmp     byte ptr [sw_c],0
        je      do_report
        mov     eax,F1_CCOMMIT
        int     21h

do_report:
        cmp     byte ptr [sw_q],0
        jne     dc_bye
        call    get_status                      ; after the changes, not before
        call    report
dc_bye:
        mov     ax,4C00h
        int     21h

no_cache:
        cmp     byte ptr [sw_q],0
        jne     dc_bye
        call    banner
        mov     edx,OFFSET msg_nocache
        call    print
        mov     edx,OFFSET msg_forhelp
        call    print
        jmp     short dc_bye

;=====================================================================
; report - the screen SMARTDRV /S prints, from the record in cst.
;=====================================================================
report:
        call    banner

        cmp     byte ptr [sw_s],0
        je      rp_sizes
        mov     edx,OFFSET msg_room
        call    print
        mov     eax,dword ptr [cst+CST_NBLK]
        call    pdec32
        mov     edx,OFFSET msg_elements
        call    print
        mov     eax,dword ptr [cst+CST_ELSIZE]
        call    pdec32
        mov     edx,OFFSET msg_each
        call    print
        mov     edx,OFFSET msg_hits
        call    print
        mov     eax,dword ptr [cst+CST_HITS]
        mov     edx,dword ptr [cst+CST_HITS+4]
        call    pdec64
        mov     edx,OFFSET msg_hits2
        call    print
        mov     edx,OFFSET msg_misses
        call    print
        mov     eax,dword ptr [cst+CST_MISSES]
        mov     edx,dword ptr [cst+CST_MISSES+4]
        call    pdec64
        mov     edx,OFFSET msg_misses2
        call    print

rp_sizes:
        mov     edx,OFFSET msg_size
        call    print
        mov     eax,dword ptr [cst+CST_SIZE]
        call    pdec32
        mov     edx,OFFSET msg_bytes
        call    print
        mov     edx,OFFSET msg_sizemin
        call    print
        mov     eax,dword ptr [cst+CST_MIN]
        call    pdec32
        mov     edx,OFFSET msg_bytes
        call    print
        mov     edx,OFFSET msg_sizemax
        call    print
        mov     eax,dword ptr [cst+CST_MAX]
        call    pdec32
        mov     edx,OFFSET msg_bytes
        call    print
        call    crlf

        ; The table: one row for every letter the cache is caching,
        ; which is SMARTDRV's rule too - a drive turned off with '-'
        ; leaves the table rather than showing a row of noes.
        mov     edx,OFFSET msg_thead
        call    print
        mov     ecx,LETTERS
        xor     ebx,ebx
rp_row:
        mov     eax,F1_CDRIVE
        mov     dl,bl
        mov     dh,CCD_GET
        int     21h
        jc      rp_next
        test    al,CCM_READ
        jz      rp_next
        mov     edx,OFFSET msg_row_rw
        test    al,CCM_WRITE
        jnz     rp_say
        mov     edx,OFFSET msg_row_ro
rp_say:
        mov     al,bl
        add     al,'A'
        mov     [edx+2],al
        call    print
rp_next:
        inc     ebx
        loop    rp_row

        mov     edx,OFFSET msg_behind
        test    byte ptr [cst+CST_FLAGS],CSTF_LAZY
        jnz     rp_lazy
        mov     edx,OFFSET msg_nobehind
rp_lazy:
        call    print
        test    byte ptr [cst+CST_FLAGS],CSTF_ERROR
        jz      rp_noerr
        mov     edx,OFFSET msg_wberr
        call    print
rp_noerr:
        mov     edx,OFFSET msg_forhelp
        call    print
        ret

;---------------------------------------------------------------------
; banner - the first line: whose cache this is, and the kernel's
;          version, which is the cache's version.
;---------------------------------------------------------------------
banner:
        mov     edx,OFFSET msg_banner
        call    print
        mov     ax,0F100h
        int     21h
        movzx   eax,bh
        push    ebx
        call    pdec32
        mov     al,'.'
        call    putc
        pop     ebx
        movzx   eax,bl
        cmp     eax,10
        jae     bn_minor
        push    eax
        mov     al,'0'
        call    putc
        pop     eax
bn_minor:
        call    pdec32
        call    crlf
        call    crlf
        ret

;---------------------------------------------------------------------
; get_status - the record into cst.  A kernel without the call is
;              reported as a machine without a cache, which for this
;              program's purposes it is.
;---------------------------------------------------------------------
get_status:
        mov     eax,F1_CSTATUS
        mov     edi,OFFSET cst
        int     21h
        jnc     gs_out
        mov     dword ptr [cst+CST_FLAGS],0
gs_out:
        ret

;=====================================================================
; Output helpers
;=====================================================================

; print - EDX -> a '$'-terminated string
print:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; putc - AL
putc:
        push    eax
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    print
        pop     edx
        ret

upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,20h
uc_out:
        ret

; is_sep - ZF set if AL ends a parameter
is_sep:
        cmp     al,' '
        je      is_out
        cmp     al,9
        je      is_out
        cmp     al,0Dh
is_out:
        ret

;---------------------------------------------------------------------
; pdec64 - EDX:EAX in decimal with the country's thousands separator
; pdec32 - EAX the same
;
; Built backwards, which is the only way to know where the separators
; go before the digit count is known; the division is the long one,
; high dword first, so that a counter which has gone past four
; thousand million prints as what it is.  No padding: SMARTDRV's
; formatter prints none either, and the spacing is in the messages.
;---------------------------------------------------------------------
pdec32:
        push    edx
        xor     edx,edx
        call    pdec64
        pop     edx
        ret

pdec64:
        pushad
        mov     edi,OFFSET numbuf + 31
        mov     byte ptr [edi],'$'
        xor     ebp,ebp                         ; digits since a separator
        mov     ecx,10
pd_lp:
        push    eax                             ; the low dword
        mov     eax,edx
        xor     edx,edx
        div     ecx                             ; EAX = high quotient
        mov     ebx,eax
        pop     eax
        div     ecx                             ; EDX:EAX = (rem:low) / 10
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        mov     edx,ebx                         ; EDX:EAX = the quotient
        inc     ebp
        mov     ebx,eax
        or      ebx,edx
        jz      pd_done
        cmp     ebp,3
        jne     pd_lp
        dec     edi
        mov     bl,[sep_char]
        mov     [edi],bl
        xor     ebp,ebp
        jmp     short pd_lp
pd_done:
        mov     edx,edi
        call    print
        popad
        ret

;---------------------------------------------------------------------
; get_sep - the thousands separator out of the country information,
;           as SMARTDRV takes it.  A kernel that will not say keeps
;           the comma.
;---------------------------------------------------------------------
get_sep:
        mov     ax,3800h
        mov     edx,OFFSET ctry
        int     21h
        jc      gsp_out
        mov     al,[ctry+7]                     ; the thousands separator
        or      al,al
        jz      gsp_out
        mov     [sep_char],al
gsp_out:
        ret

;=====================================================================
; Data
;=====================================================================
sw_x        db  0
sw_c        db  0
sw_r        db  0
sw_q        db  0
sw_s        db  0
n_dops      db  0
dop_letter  db  0
dop_req     db  0
sep_char    db  ','
            db  0,0,0
tok_start   dd  0
dops        db  DOPS_MAX * 2 dup(0)             ; letter, CCD_* request
cst         db  CST_LEN dup(0)
ctry        db  40 dup(0)
numbuf      db  32 dup(0)

msg_banner  db  'PM-DOS Disk Cache version ','$'
msg_room    db  'Room for    ','$'
msg_elements db ' elements of   ','$'
msg_each    db  ' bytes each',13,10,'$'
msg_hits    db  'There have been    ','$'
msg_hits2   db  ' cache hits',13,10,'$'
msg_misses  db  '    and    ','$'
msg_misses2 db  ' cache misses',13,10,13,10,'$'
msg_size    db  'Cache size:  ','$'
msg_sizemin db  'Smallest cache size:  ','$'
msg_sizemax db  'Largest cache size:  ','$'
msg_bytes   db  ' bytes',13,10,'$'

msg_thead   db  '            Disk Caching Status',13,10
            db  'drive   read cache   write cache   buffering',13,10
            db  44 dup('-'),13,10,'$'
msg_row_rw  db  '  A:       yes           yes          no',13,10,'$'
msg_row_ro  db  '  A:       yes           no           no',13,10,'$'
msg_behind  db  'Write behind data will be committed before command prompt returns.',13,10,'$'
msg_nobehind db 'Write behind caching is off.',13,10,'$'
msg_wberr   db  'A write to the disk failed; the data is still in the cache.',13,10,'$'
msg_forhelp db  13,10,13,10,'For help, type "Cache /?".',13,10,'$'
msg_nocache db  'The disk cache is not running.',13,10,'$'
msg_crlf    db  13,10,'$'

msg_badparm db  'Invalid parameter - ','$'
msg_usage   db  'Displays and controls the disk cache built into PM-DOS.',13,10,13,10
            db  'CACHE [/X] [[drive[+|-]]...] [/C | /R] [/Q | /S]',13,10,13,10
            db  '/X              Disables write-behind caching for all drives.',13,10
            db  'drive           Sets caching options on specific drive(s). The specified',13,10
            db  '                drive(s) will have write-caching disabled unless you add +.',13,10
            db  '+               Enables write-behind caching for the specified drive.',13,10
            db  '-               Disables all caching for the specified drive.',13,10
            db  '/C              Writes all information currently in write-cache to hard disk.',13,10
            db  '/R              Clears the cache and restarts it.',13,10
            db  '/Q              Does not display status information.',13,10
            db  '/S              Displays additional information about the cache',39,'s status.',13,10,13,10
            db  'The size of the cache is set by CACHE= in CONFIG.SYS.',13,10,'$'

_DC     ENDS
        END
