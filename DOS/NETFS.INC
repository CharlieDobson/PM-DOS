;=====================================================================
; NETFS.INC - the network-drive file branches
;
; A drive letter whose drv_tbl row carries NET_DRV_TYPE is a share on
; an SMB server, reached through the one loaded NDTF_REDIR driver
; (DRV\SMBFS).  The native kernel has no redirector and no CDS
; (ISO9660.INC says why): a letter is a volume and vol_fs says which
; filesystem it is.  So the INT 21h file functions test the letter at
; their entry and, for a net one, route the request here instead of to
; fat_search and the cluster layer.  Everything SMB - the session, the
; names, the bytes - is the driver's; this file marshals a request
; block (INC\REDIR.INC) and copies the result into the caller's frame.
;
; THE PATH WORK IS THE DRIVER'S TOO.  The kernel strips the "X:" drive
; prefix and hands the rest over as typed; the driver folds "." and
; ".." , turns '/' into '\' and, for a find, splits the directory from
; the wildcard.  Keeping it in C keeps the delicate string handling out
; of the kernel and where it can be read.
;
; Included by DOSMAIN.ASM after NDLOAD.INC (nd_find_flag / nd_call_row)
; and INT21.INC (i21_ok / i21_fail / buf_deny / the F_* frame).
;=====================================================================

;---------------------------------------------------------------------
; net_drive_present - AL = a drive letter (0 = A).  CF clear if that
;                     letter is a present network drive.  Preserves all
;                     the general registers.
;---------------------------------------------------------------------
net_drive_present:
        push    ebx
        cmp     al,MAX_DRIVES
        jae     ndp_no
        call    drv_slot                        ; EBX -> the row, AL kept
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      ndp_no
        cmp     byte ptr [ebx+DRV_TYPE],NET_DRV_TYPE
        jne     ndp_no
        pop     ebx
        clc
        ret
ndp_no:
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; net_drive_num - ESI -> an ASCIIZ path.  CF clear with AL = the net
;                 drive the path names (its "X:" prefix, or the default
;                 drive) when that drive is a network one; CF set
;                 otherwise.  ESI is preserved.
;---------------------------------------------------------------------
net_drive_num:
        push    esi
        cmp     byte ptr [esi+1],':'
        jne     ndn_default
        mov     al,[esi]
        or      al,20h
        sub     al,'a'
        jmp     short ndn_check
ndn_default:
        mov     al,[cur_drive]
ndn_check:
        call    net_drive_present               ; CF + AL
        pop     esi
        ret

;---------------------------------------------------------------------
; net_copyz - copy an ASCIIZ string, bounded.  ESI -> source, EDI ->
;             destination, ECX = the destination's capacity (with the
;             NUL).  Stops at the source NUL or capacity.  Preserves the
;             general registers; the destination is always terminated.
;---------------------------------------------------------------------
net_copyz:
        pushad
        or      ecx,ecx
        jz      ncz_done
        dec     ecx                             ; room for the NUL
ncz_lp:
        or      ecx,ecx
        jz      ncz_term
        mov     al,[esi]
        or      al,al
        jz      ncz_term
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jmp     short ncz_lp
ncz_term:
        mov     byte ptr [edi],0
ncz_done:
        popad
        ret

;---------------------------------------------------------------------
; net_req_begin - EBX = a REDIR_* op, AL = the net drive.  Zero the
;                 request block and set its op and drive.  Preserves the
;                 general registers.
;---------------------------------------------------------------------
net_req_begin:
        pushad
        mov     edi,OFFSET net_req
        movzx   edx,al                          ; keep the drive
        xor     eax,eax
        mov     ecx,NR_SIZE / 4
        cld
        rep     stosd
        mov     [net_req+NR_OP],ebx
        mov     [net_req+NR_DRIVE],edx
        popad
        ret

;---------------------------------------------------------------------
; net_setpath - ESI -> a path (maybe with an "X:" prefix).  Copy it,
;               without the prefix, into the request's NR_PATH.
;               Preserves the general registers.
;---------------------------------------------------------------------
net_setpath:
        pushad
        cmp     byte ptr [esi+1],':'
        jne     nsp_go
        add     esi,2
nsp_go:
        mov     edi,OFFSET net_req + NR_PATH
        mov     ecx,NET_PATH_MAX
        call    net_copyz
        popad
        ret

;---------------------------------------------------------------------
; net_fire - call the one loaded NDTF_REDIR driver with the op in
;            NR_OP.  CF clear when the call was made (NR_STATUS then
;            holds the DOS result); CF set when no redirector is loaded
;            or it has no CALL entry.  Preserves the general registers.
;---------------------------------------------------------------------
; INTERRUPTS MUST BE ON while the driver runs.  Its SMB conversation
; waits on the network card's receive interrupt and on the clock tick
; (for its timeouts), and INT 21h is entered through an interrupt gate
; with them off.  Left off, the first socket wait spins forever: no
; frame is taken from the card and the tick count never moves.  So the
; caller's flags are saved, interrupts enabled for the call, and the
; flags put back - the driver is a long operation like a driver INIT,
; which the kernel also runs with interrupts on.
net_fire:
        pushad
        pushfd
        sti
        mov     eax,NDTF_REDIR
        xor     edx,edx
        call    nd_find_flag                    ; ESI -> the redirector's row
        jc      nf_fail
        mov     ebx,[net_req+NR_OP]
        mov     edi,OFFSET net_req
        mov     ecx,NR_SIZE
        call    nd_call_row                     ; -> EAX / CF, results in net_req
        jc      nf_fail
        popfd
        popad
        clc
        ret
nf_fail:
        popfd
        popad
        stc
        ret

;---------------------------------------------------------------------
; Common error tails - set AX and fail.
;---------------------------------------------------------------------
net_err_general:
        mov     ax,ERR_GENERAL
        jmp     i21_fail
net_err_status:
        mov     ax,word ptr [net_req+NR_STATUS]
        jmp     i21_fail

;=====================================================================
; net_open - 3Dh/3Ch on a net drive.  AL = the net drive; the path is
;            at F_EDX and the access mode is in F_EAX's low byte.  CL
;            bit 0 (from the caller) says create-if-absent.  Fills a
;            new SFT and answers the handle, or fails.
;=====================================================================
net_open:
        mov     ebx,REDIR_OPEN
        call    net_req_begin                   ; AL = drive
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        movzx   eax,byte ptr [ebp+F_EAX]        ; access: 0 read 1 write 2 both
        and     eax,3
        mov     [net_req+NR_COUNT],eax         ; NR_SPARE stays 0: 3Dh opens
                                                ; an existing file, never creates
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        call    sft_new                         ; -> AX = handle, ESI = entry
        jc      no_nohandles
        mov     [net_tmph],ax
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE OR SFTF_NET
        mov     eax,[net_req+NR_HANDLE]
        mov     [esi+SFT_CLUSTER],eax            ; the driver's open-file handle
        mov     eax,[net_req+NR_SIZELO]
        mov     [esi+SFT_SIZE],eax
        mov     dword ptr [esi+SFT_POS],0
        mov     dword ptr [esi+SFT_CURCLUS],0
        mov     dword ptr [esi+SFT_DIRENT],0
        mov     eax,[net_req+NR_DRIVE]
        mov     [esi+SFT_DRIVE],eax
        movzx   eax,byte ptr [ebp+F_EAX]
        and     eax,3
        mov     [esi+SFT_MODE],eax
        movzx   eax,byte ptr [ebp+F_EAX]
        mov     [esi+SFT_SHARE],eax
        mov     eax,[net_req+NR_TIMEDATE]
        mov     [esi+SFT_TIME],eax
        movzx   eax,word ptr [net_tmph]
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
no_nohandles:
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail

;=====================================================================
; net_create - 3Ch/5Bh/716Ch create on a net drive.  AL = the net
;              drive; the path is at F_EDX.  Creates (or truncates) the
;              file on the server and hands back a read/write handle.
;=====================================================================
net_create:
        mov     ebx,REDIR_OPEN
        call    net_req_begin                   ; AL = drive
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        mov     dword ptr [net_req+NR_COUNT],2   ; access = read/write
        mov     dword ptr [net_req+NR_SPARE],1   ; disposition = create/truncate
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        call    sft_new                         ; -> AX = handle, ESI = entry
        jc      no_nohandles
        mov     [net_tmph],ax
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE OR SFTF_NET
        mov     eax,[net_req+NR_HANDLE]
        mov     [esi+SFT_CLUSTER],eax
        mov     eax,[net_req+NR_SIZELO]
        mov     [esi+SFT_SIZE],eax
        mov     dword ptr [esi+SFT_POS],0
        mov     dword ptr [esi+SFT_CURCLUS],0
        mov     dword ptr [esi+SFT_DIRENT],0
        mov     eax,[net_req+NR_DRIVE]
        mov     [esi+SFT_DRIVE],eax
        mov     dword ptr [esi+SFT_MODE],2
        mov     dword ptr [esi+SFT_SHARE],0
        mov     eax,[net_req+NR_TIMEDATE]
        mov     [esi+SFT_TIME],eax
        movzx   eax,word ptr [net_tmph]
        mov     [ebp+F_EAX],eax
        jmp     i21_ok

;=====================================================================
; net_lopen - 716Ch (LFN open/create) on a net drive.  AL = the net
;             drive; F_ESI = path, F_EBX = access mode, F_EDX = action
;             (bit 0 open existing, bit 1 truncate, bit 4 create).  The
;             action becomes an SMB disposition; CX comes back 1 opened
;             or 2 created, as 716Ch promises.
;=====================================================================
net_lopen:
        mov     ebx,REDIR_OPEN
        call    net_req_begin                   ; AL = drive
        mov     esi,[ebp+F_ESI]
        call    net_setpath
        movzx   eax,byte ptr [ebp+F_EBX]
        and     eax,3
        mov     [net_req+NR_COUNT],eax           ; access
        mov     dword ptr [net_req+NR_SPARE],0   ; default FILE_OPEN
        movzx   edx,byte ptr [ebp+F_EDX]
        test    dl,2                             ; truncate -> OVERWRITE_IF
        jz      nlo_t1
        mov     dword ptr [net_req+NR_SPARE],1
        jmp     short nlo_fire
nlo_t1:
        test    dl,10h                           ; create if missing -> OPEN_IF
        jz      nlo_fire
        mov     dword ptr [net_req+NR_SPARE],2
nlo_fire:
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        call    sft_new
        jc      no_nohandles
        mov     [net_tmph],ax
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE OR SFTF_NET
        mov     eax,[net_req+NR_HANDLE]
        mov     [esi+SFT_CLUSTER],eax
        mov     eax,[net_req+NR_SIZELO]
        mov     [esi+SFT_SIZE],eax
        mov     dword ptr [esi+SFT_POS],0
        mov     dword ptr [esi+SFT_CURCLUS],0
        mov     dword ptr [esi+SFT_DIRENT],0
        mov     eax,[net_req+NR_DRIVE]
        mov     [esi+SFT_DRIVE],eax
        movzx   eax,byte ptr [ebp+F_EBX]
        and     eax,3
        mov     [esi+SFT_MODE],eax
        mov     dword ptr [esi+SFT_SHARE],0
        mov     eax,[net_req+NR_TIMEDATE]
        mov     [esi+SFT_TIME],eax
        movzx   eax,word ptr [net_tmph]
        mov     [ebp+F_EAX],eax
        mov     word ptr [ebp+F_ECX],1          ; opened
        cmp     dword ptr [net_req+NR_SPARE],0
        je      nlo_done
        mov     word ptr [ebp+F_ECX],2          ; created
nlo_done:
        jmp     i21_ok

;=====================================================================
; net_read - 3Fh on a net handle.  ESI -> the SFT entry (SFTF_NET).
;            The destination (F_EDX) has already been bounds-checked by
;            3Fh, the count is F_ECX.  Answers F_EAX = bytes delivered.
;=====================================================================
net_read:
        mov     ebx,REDIR_READ
        mov     eax,[esi+SFT_DRIVE]
        push    esi
        call    net_req_begin                   ; AL = drive
        pop     esi
        mov     eax,[esi+SFT_CLUSTER]
        mov     [net_req+NR_HANDLE],eax
        mov     eax,[esi+SFT_POS]
        mov     [net_req+NR_POSLO],eax
        mov     dword ptr [net_req+NR_POSHI],0
        mov     eax,[ebp+F_ECX]
        mov     [net_req+NR_COUNT],eax
        mov     eax,[ebp+F_EDX]
        mov     [net_req+NR_BUFPTR],eax
        push    esi
        call    net_fire
        pop     esi
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        mov     eax,[net_req+NR_RESULT]        ; bytes read
        add     [esi+SFT_POS],eax
        mov     [ebp+F_EAX],eax
        jmp     i21_ok

;=====================================================================
; net_write - 40h on a net handle.  ESI -> the SFT entry.  The source
;             (F_EDX) has been bounds-checked by 40h, the count F_ECX.
;             A zero count truncates at the position; the driver does
;             that.  Answers F_EAX = bytes written.
;=====================================================================
net_write:
        mov     ebx,REDIR_WRITE
        mov     eax,[esi+SFT_DRIVE]
        push    esi
        call    net_req_begin
        pop     esi
        mov     eax,[esi+SFT_CLUSTER]
        mov     [net_req+NR_HANDLE],eax
        mov     eax,[esi+SFT_POS]
        mov     [net_req+NR_POSLO],eax
        mov     dword ptr [net_req+NR_POSHI],0
        mov     eax,[ebp+F_ECX]
        mov     [net_req+NR_COUNT],eax
        mov     eax,[ebp+F_EDX]
        mov     [net_req+NR_BUFPTR],eax
        push    esi
        call    net_fire
        pop     esi
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        mov     eax,[net_req+NR_RESULT]        ; bytes written
        cmp     eax,[net_req+NR_COUNT]         ; never more than was asked
        jbe     nw_counted
        mov     eax,[net_req+NR_COUNT]
nw_counted:
        add     [esi+SFT_POS],eax
        ; a write past the old size grows the file
        mov     edx,[esi+SFT_POS]
        cmp     edx,[esi+SFT_SIZE]
        jbe     nw_sized
        mov     [esi+SFT_SIZE],edx
nw_sized:
        mov     [ebp+F_EAX],eax
        jmp     i21_ok

;=====================================================================
; net_close - CLOSE a net handle on the server and free the SFT entry.
;             ESI -> the SFT entry.  Called from 3Eh and from sft_close
;             (process termination).  Preserves the general registers;
;             the entry is freed (SFT_FLAGS = 0) either way.
;=====================================================================
net_close:
        pushad
        mov     ebx,REDIR_CLOSE
        mov     eax,[esi+SFT_DRIVE]
        mov     [net_cl_esi],esi
        call    net_req_begin
        mov     esi,[net_cl_esi]
        mov     eax,[esi+SFT_CLUSTER]
        mov     [net_req+NR_HANDLE],eax
        call    net_fire
        mov     esi,[net_cl_esi]
        mov     dword ptr [esi+SFT_FLAGS],0
        popad
        ret

;=====================================================================
; net_find - 4Eh on a net drive.  AL = the net drive.  The DTA is set
;            (4Eh checked dta_ok); the path is at F_EDX and the search
;            attributes in F_ECX.  Lists the directory through the
;            driver and fills the DTA with the first match.
;=====================================================================
net_find:
        mov     ebx,REDIR_FINDFIRST
        call    net_req_begin                   ; AL = drive
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        movzx   eax,byte ptr [ebp+F_ECX]
        or      al,SRCH_FILTER                  ; a directory search, as fn4E's
        mov     [net_req+NR_ATTR],eax          ; own path marks it (hidden and
                                                ; system stay out unless asked)
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     nf_status
        call    net_fill_dta
        jmp     i21_ok
nf_status:
        ; "Nothing matched" is the driver's ERR_NOFILE and 4Eh's
        ; ERR_NOMORE, as fn4E says it on a local drive.  714Eh shares
        ; the driver's answer and keeps the 2 (nlf_nomatch).
        cmp     dword ptr [net_req+NR_STATUS],ERR_NOFILE
        jne     net_err_status
        mov     ax,ERR_NOMORE
        jmp     i21_fail

;=====================================================================
; net_findnext - 4Fh on a net drive.  AL = the net drive, EDI = the
;                DTA, which holds the listing id and the next index.
;=====================================================================
net_findnext:
        mov     ebx,REDIR_FINDNEXT
        call    net_req_begin                   ; AL = drive
        mov     edi,[dta_ptr]
        movzx   eax,word ptr [edi+0Dh]          ; the next index
        mov     [net_req+NR_COUNT],eax
        mov     eax,[edi+0Fh]                   ; the listing id
        mov     [net_req+NR_GEN],eax
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_findnext_none
        call    net_fill_dta
        jmp     i21_ok
net_findnext_none:
        mov     ax,ERR_NOMORE
        jmp     i21_fail

;---------------------------------------------------------------------
; net_fill_dta - copy the entry the driver returned in net_req into the
;                caller's DTA, in the classic 4Eh/4Fh layout, and store
;                the continuation (drive, listing id, next index) in the
;                record's reserved bytes.  Preserves the general
;                registers.
;---------------------------------------------------------------------
net_fill_dta:
        pushad
        mov     edi,[dta_ptr]
        mov     eax,[net_req+NR_DRIVE]         ; +00 : the drive (as classic)
        mov     [edi],al
        mov     eax,[net_req+NR_GEN]           ; +0F : the listing id
        mov     [edi+0Fh],eax
        mov     eax,[net_req+NR_COUNT]         ; +0D : the next index
        mov     [edi+0Dh],ax
        mov     eax,[net_req+NR_ATTR]          ; +15 : attributes
        mov     [edi+15h],al
        mov     eax,[net_req+NR_TIMEDATE]      ; +16 : time, +18 : date
        mov     [edi+16h],ax
        shr     eax,16
        mov     [edi+18h],ax
        mov     eax,[net_req+NR_SIZELO]        ; +1A : size
        mov     [edi+1Ah],eax
        add     edi,1Eh                         ; +1E : the 8.3 name, ASCIIZ
        mov     esi,OFFSET net_req + NR_NAME
        mov     ecx,12                          ; 12 + the NUL: the record ends at +2A
nfd_nm:
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      nfd_done
        inc     esi
        inc     edi
        dec     ecx
        jnz     nfd_nm
        mov     byte ptr [edi],0
nfd_done:
        popad
        ret

;=====================================================================
; net_lfn_find - 714Eh on a net drive.  AL = the net drive; the path is
;                at F_EDX, CL = allowable attrs, CH = required, and the
;                318-byte record goes to F_EDI (714E checked it).  The
;                driver lists the directory and matches the pattern; the
;                find is handle-based, so a find handle (the same table
;                4Eh's LFN find uses) carries the listing id and the
;                resume index.  Answers the handle in AX.
;=====================================================================
net_lfn_find:
        mov     [net_lfndrv],al
        mov     ebx,[find_tblp]                 ; a free handle
        or      ebx,ebx
        jz      nlf_nohandles
        xor     ecx,ecx
nlf_slot:
        cmp     byte ptr [ebx+FH_INUSE],0
        je      nlf_have
        add     ebx,FH_SIZE
        inc     ecx
        cmp     ecx,FIND_MAX
        jb      nlf_slot
nlf_nohandles:
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail
nlf_have:
        mov     [net_fh],ebx
        mov     [net_fhidx],ecx
        mov     eax,[cur_own]
        mov     [ebx+FH_OWNER],eax
        mov     al,[net_lfndrv]
        mov     [ebx+FH_DRIVE],al

        mov     al,[net_lfndrv]
        mov     ebx,REDIR_FINDFIRST
        call    net_req_begin
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        movzx   eax,byte ptr [ebp+F_ECX]        ; CL = allowable attributes
        or      al,ATTR_DIR OR SRCH_FILTER      ; a dir search; dirs allowed -
        mov     [net_req+NR_ATTR],eax           ; DIR's own /A filter narrows it
        call    net_fire
        jc      nlf_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     nlf_nomatch
        mov     ebx,[net_fh]
        mov     eax,[net_req+NR_GEN]
        mov     [ebx+FH_DIR],eax                ; the listing id
        mov     eax,[net_req+NR_COUNT]
        mov     [ebx+FH_NEXT],eax               ; the resume index
        mov     byte ptr [ebx+FH_INUSE],1
        call    net_lfn_fill
        mov     eax,[net_fhidx]
        mov     word ptr [ebp+F_EAX],ax
        mov     word ptr [ebp+F_ECX],0
        jmp     i21_ok
nlf_nomatch:
        mov     ax,word ptr [net_req+NR_STATUS] ; nothing matched / no path
        jmp     i21_fail
nlf_general:
        mov     ax,ERR_GENERAL
        jmp     i21_fail

;=====================================================================
; net_lfn_next - 714Fh on a net find handle.  EBX -> the handle entry,
;                AL = its (net) drive.  The record goes to F_EDI.
;=====================================================================
net_lfn_next:
        mov     [net_fh],ebx
        mov     al,[ebx+FH_DRIVE]
        mov     ebx,REDIR_FINDNEXT
        call    net_req_begin                   ; AL = drive
        mov     ebx,[net_fh]
        mov     eax,[ebx+FH_NEXT]
        mov     [net_req+NR_COUNT],eax
        mov     eax,[ebx+FH_DIR]                ; the listing id
        mov     [net_req+NR_GEN],eax
        call    net_fire
        jc      nln_nomore
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     nln_nomore
        mov     ebx,[net_fh]
        mov     eax,[net_req+NR_COUNT]
        mov     [ebx+FH_NEXT],eax
        call    net_lfn_fill
        jmp     i21_ok
nln_nomore:
        mov     ax,12h                          ; no more files
        jmp     i21_fail

;---------------------------------------------------------------------
; net_lfn_fill - fill the 318-byte LFN find record at F_EDI from the
;                entry the driver returned in net_req, in the same shape
;                714Eh's own fill uses (DOS attribute and DOS date/time;
;                the long name is the 8.3 name until v1 tracks LFNs).
;                Preserves the general registers.
;---------------------------------------------------------------------
net_lfn_fill:
        pushad
        mov     edi,[ebp+F_EDI]
        push    edi
        xor     eax,eax
        mov     ecx,79
        cld
        rep     stosd
        stosw                                   ; 79*4 + 2 = 318, zeroed
        pop     edi
        mov     eax,[net_req+NR_ATTR]
        mov     [edi],eax
        mov     eax,[net_req+NR_TIMEDATE]       ; (date << 16) | time
        mov     [edi+4],eax
        mov     [edi+0Ch],eax
        mov     [edi+14h],eax
        mov     eax,[net_req+NR_SIZELO]
        mov     [edi+20h],eax
        lea     esi,[net_req+NR_NAME]           ; 8.3 name at +130h
        push    edi
        add     edi,130h
        mov     ecx,14                          ; the short-name slot
        call    net_copyz
        pop     edi
        lea     esi,[net_req+NR_NAME]           ; long name at +2Ch
        add     edi,2Ch
        mov     ecx,16                          ; no more than NR_NAME holds
        call    net_copyz
        popad
        ret

;=====================================================================
; net_pathop - a path-only operation (delete, mkdir, rmdir) on a net
;              drive.  EBX = the REDIR_* op, AL = the net drive, the path
;              is at F_EDX.
;=====================================================================
net_pathop:
        call    net_req_begin                   ; EBX = op, AL = drive
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        jmp     i21_ok

net_delete:                                     ; AL = drive
        mov     ebx,REDIR_DELETE
        jmp     net_pathop
net_mkdir:
        mov     ebx,REDIR_MKDIR
        jmp     net_pathop
net_rmdir:
        mov     ebx,REDIR_RMDIR
        jmp     net_pathop

;=====================================================================
; net_rename - 56h on a net drive.  AL = the old name's net drive; the
;              old name is at F_EDX and the new one at F_EDI.  Both go
;              to the driver past their "X:" prefixes, the old in
;              NR_PATH and the new in NR_SHARE (REDIR.H gives the
;              MOUNT-only field to RENAME), as RMNET2.INC's 1111h does
;              for a DOS box.  56h used to take a net drive down the FAT
;              path, where every rename on a share was "file not found".
;              A new name on another drive is refused as on a disk: a
;              rename never copies.
;=====================================================================
net_rename:
        mov     dl,al                           ; the old name's drive
        mov     esi,[ebp+F_EDI]
        call    net_drive_num                   ; AL = the new name's, if a net one
        jc      nren_notsame
        cmp     al,dl
        jne     nren_notsame
        mov     ebx,REDIR_RENAME
        call    net_req_begin                   ; EBX = op, AL = drive
        mov     esi,[ebp+F_EDX]
        call    net_setpath                     ; the old name -> NR_PATH
        mov     esi,[ebp+F_EDI]
        cmp     byte ptr [esi+1],':'
        jne     nren_new
        add     esi,2
nren_new:
        mov     edi,OFFSET net_req + NR_SHARE   ; the new name -> NR_SHARE
        mov     ecx,NR_USER - NR_SHARE
        call    net_copyz
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        jmp     i21_ok
nren_notsame:
        mov     ax,ERR_NOTSAME
        jmp     i21_fail

;=====================================================================
; net_info - 36h on a net drive.  AL = the net drive.  A share has no
;            geometry, so report a large, plausible one; DIR turns it
;            into a free-bytes figure and nothing else reads it.
;=====================================================================
net_info:
        mov     word ptr [ebp+F_EAX],64         ; sectors per cluster
        mov     word ptr [ebp+F_ECX],512        ; bytes per sector
        mov     word ptr [ebp+F_EDX],0FFFFh     ; total clusters
        mov     word ptr [ebp+F_EBX],0FFFFh     ; free clusters
        jmp     i21_ok

;=====================================================================
; fF1_netuse - 21h/F1h AL=27h: NET USE, for a native NET.EXE.  Bits
;              16-23 of EAX = NETUSE_*, EDI -> a netuse_req of ECX bytes
;              which the kernel checks.  Native programs only.
;=====================================================================
fF1_netuse:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     i21_unsup
        mov     eax,[ebp+F_EDI]
        mov     ecx,NU_SIZE
        call    cbuf_ok
        jc      buf_deny
        mov     edi,[ebp+F_EDI]                 ; flat, checked
        mov     eax,[ebp+F_EAX]
        shr     eax,16
        movzx   eax,al
        cmp     eax,NETUSE_MAP
        je      fnu_map
        cmp     eax,NETUSE_UNMAP
        je      fnu_unmap
        cmp     eax,NETUSE_QUERY
        je      fnu_query
        jmp     i21_unsup

;--- NETUSE_MAP: \\server\share -> a drive letter -------------------
fnu_map:
        mov     eax,[edi+NU_DRIVE]
        cmp     eax,MAX_DRIVES
        jae     fnu_baddrv
        mov     [net_nudrive],al
        call    drv_slot                        ; EBX -> the row (AL in)
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      fnu_build
        cmp     byte ptr [ebx+DRV_TYPE],NET_DRV_TYPE
        jne     fnu_inuse                       ; a local drive holds the letter
fnu_build:
        ; the credentials, from the program's buffer into the request
        mov     al,[net_nudrive]
        mov     ebx,REDIR_MOUNT
        call    net_req_begin
        mov     edi,[ebp+F_EDI]
        lea     esi,[edi+NU_SERVER]
        mov     edi,OFFSET net_req + NR_SERVER
        mov     ecx,64
        call    net_copyz
        mov     edi,[ebp+F_EDI]
        lea     esi,[edi+NU_SHARE]
        mov     edi,OFFSET net_req + NR_SHARE
        mov     ecx,80
        call    net_copyz
        mov     edi,[ebp+F_EDI]
        lea     esi,[edi+NU_USER]
        mov     edi,OFFSET net_req + NR_USER
        mov     ecx,64
        call    net_copyz
        mov     edi,[ebp+F_EDI]
        lea     esi,[edi+NU_PASS]
        mov     edi,OFFSET net_req + NR_PASS
        mov     ecx,64
        call    net_copyz
        mov     edi,[ebp+F_EDI]
        lea     esi,[edi+NU_DOMAIN]
        mov     edi,OFFSET net_req + NR_DOMAIN
        mov     ecx,64
        call    net_copyz
        ; the protocol: NUF_SMB1 picks the down-level SMB1 client, else
        ; SMB2/3.  net_req_begin zeroed NR_SPARE (= SMB2) already.
        mov     edi,[ebp+F_EDI]
        test    dword ptr [edi+NU_FLAGS],NUF_SMB1
        jz      fnu_fire
        mov     dword ptr [net_req+NR_SPARE],1
fnu_fire:
        call    net_fire
        jc      fnu_nodrv
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     fnu_mapfail
        ; the session is up: mark the drive row
        mov     al,[net_nudrive]
        call    drv_slot
        mov     byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        mov     byte ptr [ebx+DRV_TYPE],NET_DRV_TYPE
        mov     al,[net_nudrive]
        mov     [ebx+DRV_BIOS],al
        mov     byte ptr [ebx+DRV_PIDX],0FFh
        mov     dword ptr [ebx+DRV_START],0
        mov     dword ptr [ebx+DRV_SECS],0
        ; raise the drive high-water mark if this letter is past it
        movzx   eax,byte ptr [net_nudrive]
        inc     eax
        cmp     al,[drv_count]
        jbe     fnu_counted
        mov     [drv_count],al
fnu_counted:
        xor     eax,eax
        mov     edi,[ebp+F_EDI]
        mov     [edi+NU_STATUS],eax
        jmp     i21_ok

fnu_mapfail:
        mov     eax,[net_req+NR_STATUS]
        mov     edi,[ebp+F_EDI]
        mov     [edi+NU_STATUS],eax
        mov     ax,word ptr [net_req+NR_STATUS]
        jmp     i21_fail
fnu_nodrv:
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_STATUS],ERR_GENERAL
        mov     ax,ERR_GENERAL
        jmp     i21_fail
fnu_inuse:
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_STATUS],85    ; ERROR_ALREADY_ASSIGNED
        mov     ax,85
        jmp     i21_fail
fnu_baddrv:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;--- NETUSE_UNMAP: take the letter back -----------------------------
fnu_unmap:
        mov     eax,[edi+NU_DRIVE]
        cmp     eax,MAX_DRIVES
        jae     fnu_baddrv
        mov     [net_nudrive],al
        call    drv_slot
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      fnu_notmapped
        cmp     byte ptr [ebx+DRV_TYPE],NET_DRV_TYPE
        jne     fnu_notmapped
        mov     al,[net_nudrive]
        mov     ebx,REDIR_UMOUNT
        call    net_req_begin
        call    net_fire                        ; best effort
        mov     al,[net_nudrive]
        call    drv_slot
        mov     byte ptr [ebx+DRV_FLAGS],0
        mov     byte ptr [ebx+DRV_TYPE],0
        mov     byte ptr [ebx+DRV_BIOS],0
        ; drop any cached mount of that letter so a later re-map is fresh
        mov     al,[net_nudrive]
        cmp     al,[cur_vol]
        jne     fnu_um_ok
        mov     byte ptr [fat_ok],0
fnu_um_ok:
        xor     eax,eax
        mov     edi,[ebp+F_EDI]
        mov     [edi+NU_STATUS],eax
        jmp     i21_ok
fnu_notmapped:
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_STATUS],ERR_BADDRIVE
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;--- NETUSE_QUERY: is this letter a net drive? ----------------------
fnu_query:
        mov     eax,[edi+NU_DRIVE]
        cmp     eax,MAX_DRIVES
        jae     fnu_baddrv
        mov     [net_nudrive],al
        mov     dword ptr [edi+NU_FLAGS],0
        mov     dword ptr [edi+NU_STATUS],0
        call    net_drive_present               ; AL in, CF out
        jc      fnu_q_done
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_FLAGS],NUF_INUSE
fnu_q_done:
        jmp     i21_ok

;=====================================================================
; Data
;=====================================================================
net_nudrive db  0
net_lfndrv  db  0
            db  0,0
net_tmph    dw  0
            dw  0
net_cl_esi  dd  0
net_fh      dd  0                       ; the find handle being filled
net_fhidx   dd  0                       ; ...and its index (the 714Eh handle)

; The request block.  One, in the kernel's data: a net file operation
; runs to completion inside one INT 21h call and never nests, so a
; single block is enough and it is never an interrupt's.
net_req     dd  (NR_SIZE / 4) dup(0)
