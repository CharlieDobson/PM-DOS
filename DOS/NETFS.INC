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
; net_drive_file - net_drive_num for an open or a create: CF set as
;                  well when the path names a device.  "> NUL" and
;                  "> PRN" with a share the current drive are the
;                  device, as on any drive (dev_find: any directory,
;                  any extension), and not a file of that name on the
;                  server; the local open and create take devices
;                  first.  ESI is preserved.
;---------------------------------------------------------------------
net_drive_file:
        call    net_drive_num
        jc      ndf_out
        push    eax
        call    dev_find                        ; CF clear: a device
        pop     eax
        cmc
ndf_out:
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
;               without the prefix, into the request's NR_PATH, from
;               the share's root (net_putpath).  Preserves the general
;               registers.
;---------------------------------------------------------------------
net_setpath:
        pushad
        mov     edi,OFFSET net_req + NR_PATH
        mov     ecx,NET_PATH_MAX
        call    net_putpath
        popad
        ret

;---------------------------------------------------------------------
; net_putpath - ESI -> a path, EDI -> where it goes, ECX = the room
;               there.  Copied without its "X:" prefix and otherwise as
;               typed: THE DRIVER KEEPS THE DRIVE'S CURRENT DIRECTORY
;               (3Bh, net_chdir) and takes a relative path from it, in
;               the server's long names, which the kernel's 8.3 cwd
;               could not hold.  Preserves the general registers.
;---------------------------------------------------------------------
net_putpath:
        pushad
        cmp     byte ptr [esi+1],':'
        jne     npp_copy
        add     esi,2
npp_copy:
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
        mov     dword ptr [net_req+NR_SPARE],1  ; long names, matched and back
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
;                714Eh's own fill uses (DOS attribute and DOS date/time).
;                The long name is the one the driver put in NR_PATH, the
;                8.3 one in NR_NAME.  Preserves the general registers.
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
        lea     esi,[net_req+NR_PATH]           ; long name at +2Ch: the
        cmp     byte ptr [esi],0                ; driver's, or the 8.3 one
        jne     nlf_long
        lea     esi,[net_req+NR_NAME]
nlf_long:
        add     edi,2Ch
        mov     ecx,LFN_MAX                     ; the record's 260 bytes
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
        mov     edi,OFFSET net_req + NR_SHARE   ; the new name -> NR_SHARE
        mov     ecx,NR_USER - NR_SHARE
        call    net_putpath
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        jmp     i21_ok
nren_notsame:
        mov     ax,ERR_NOTSAME
        jmp     i21_fail

;=====================================================================
; net_info - 36h on a net drive.  AL = the net drive.  The server's
;            figures (net_size) as 32K clusters - 64 sectors of 512 -
;            in 36h's 16-bit registers, so a share over 2GB says 2GB,
;            as on MS-DOS 7; 7303h (net_f73) gives the whole figure.
;            A server that will not say gets 65,535 clusters, all free.
;=====================================================================
net_info:
        call    net_size
        jc      ni_guess
        mov     word ptr [ebp+F_EAX],64         ; sectors per cluster
        mov     word ptr [ebp+F_ECX],512        ; bytes per sector
        mov     eax,dword ptr [net_tsec]
        mov     edx,dword ptr [net_tsec+4]
        call    ni_clus
        mov     word ptr [ebp+F_EDX],ax         ; total clusters
        mov     eax,dword ptr [net_fsec]
        mov     edx,dword ptr [net_fsec+4]
        call    ni_clus
        mov     word ptr [ebp+F_EBX],ax         ; free clusters
        jmp     i21_ok
ni_guess:
        mov     word ptr [ebp+F_EAX],64
        mov     word ptr [ebp+F_ECX],512
        mov     word ptr [ebp+F_EDX],0FFFFh
        mov     word ptr [ebp+F_EBX],0FFFFh
        jmp     i21_ok

; EDX:EAX = 512-byte sectors -> EAX = 32K clusters, at most 0FFFFh
ni_clus:
        shrd    eax,edx,6
        shr     edx,6
        or      edx,edx
        jnz     nic_full
        cmp     eax,0FFFFh
        jbe     nic_out
nic_full:
        mov     eax,0FFFFh
nic_out:
        ret

;---------------------------------------------------------------------
; net_f73 - 7303h's figures for a net drive.  AL = the drive.  Fills
;           f73_rec's sizes from the server (net_size): a cluster of
;           one sector, so clusters x sectors x bytes is the server's
;           own figure to the byte, and 512 bytes a sector - or larger
;           ones when the share's count of them would not fit in 32
;           bits, as on a share of more than 2TB.  CF
;           when the server would not say (f73_rec untouched).
;           Preserves the general registers.
;---------------------------------------------------------------------
net_f73:
        pushad
        call    net_size
        jc      nf73_fail
        mov     eax,dword ptr [net_tsec]
        mov     edx,dword ptr [net_tsec+4]
        mov     esi,dword ptr [net_fsec]
        mov     edi,dword ptr [net_fsec+4]
        mov     ebx,512                         ; EBX = bytes per sector
nf73_fit:
        mov     ecx,edx
        or      ecx,edi
        jz      nf73_fits                       ; both counts in 32 bits
        cmp     ebx,32768
        jae     nf73_full
        shl     ebx,1                           ; sectors twice the size,
        shrd    eax,edx,1                       ; half as many
        shr     edx,1
        shrd    esi,edi,1
        shr     edi,1
        jmp     short nf73_fit
nf73_full:
        or      edx,edx                         ; no sector size is enough:
        jz      nf73_tok                        ; the most 32 bits can say
        mov     eax,0FFFFFFFFh
nf73_tok:
        or      edi,edi
        jz      nf73_fits
        mov     esi,0FFFFFFFFh
nf73_fits:
        mov     dword ptr [f73_rec+F73_SPC],1
        mov     dword ptr [f73_rec+F73_BPS],ebx
        mov     dword ptr [f73_rec+F73_TOTSEC],eax
        mov     dword ptr [f73_rec+F73_FREESEC],esi
        mov     dword ptr [f73_rec+F73_TOTCL],eax
        mov     dword ptr [f73_rec+F73_TOTAU],eax
        mov     dword ptr [f73_rec+F73_FREECL],esi
        mov     dword ptr [f73_rec+F73_FREEAU],esi
        popad
        clc
        ret
nf73_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; net_size - AL = a net drive.  Ask the driver (REDIR_INFO) and leave
;            the share's size and free space in net_tsec and net_fsec,
;            as 64-bit counts of 512-byte sectors.  CF when the server
;            would not say.  Preserves the general registers.
;---------------------------------------------------------------------
net_size:
        pushad
        mov     ebx,REDIR_INFO
        call    net_req_begin                   ; AL = drive
        call    net_fire
        jc      nsz_fail
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     nsz_fail
        mov     eax,[net_req+NR_COUNT]          ; sectors per unit
        mul     dword ptr [net_req+NR_RESULT]   ; x bytes per sector
        jc      nsz_fail                        ; a unit over 4GB: not a share
        or      eax,eax
        jz      nsz_fail
        mov     ebx,eax                         ; EBX = bytes per unit
        mov     esi,[net_req+NR_SIZELO]
        mov     edi,[net_req+NR_SIZEHI]
        call    nsz_conv
        mov     dword ptr [net_tsec],eax
        mov     dword ptr [net_tsec+4],edx
        mov     esi,[net_req+NR_POSLO]
        mov     edi,[net_req+NR_POSHI]
        call    nsz_conv
        mov     dword ptr [net_fsec],eax
        mov     dword ptr [net_fsec+4],edx
        popad
        clc
        ret
nsz_fail:
        popad
        stc
        ret

; EDI:ESI = units, EBX = bytes per unit -> EDX:EAX = 512-byte sectors,
; at the most a 64-bit count of bytes can say.  ECX is lost.
nsz_conv:
        mov     eax,edi
        mul     ebx                             ; the high units' bytes
        or      edx,edx
        jnz     nszc_full
        mov     ecx,eax
        mov     eax,esi
        mul     ebx                             ; the low units' bytes
        add     edx,ecx
        jc      nszc_full
        shrd    eax,edx,9
        shr     edx,9
        ret
nszc_full:
        mov     eax,0FFFFFFFFh
        mov     edx,007FFFFFh                   ; (2^64 - 1) / 512
        ret

;=====================================================================
; net_attr - 43h/7143h on a net drive.  AL = the net drive, [f43_sub]
;            the subfunction, the path at F_EDX.  Reading asks the
;            server (REDIR_GETATTR) and answers CX.  Setting them is
;            not in the redirector yet and is refused as a server
;            would refuse it, rather than said to have worked.
;=====================================================================
net_attr:
        cmp     byte ptr [f43_sub],0
        jne     nat_set
        mov     ebx,REDIR_GETATTR
        call    net_req_begin                   ; AL = drive
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        movzx   eax,byte ptr [net_req+NR_ATTR]
        mov     word ptr [ebp+F_ECX],ax
        jmp     i21_ok
nat_set:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

;=====================================================================
; net_chdir - 3Bh on a net drive.  AL = the net drive, the path at
;             F_EDX.  THE DRIVER KEEPS THE CURRENT DIRECTORY (REDIR_
;             CHDIR): it walks the path on the server a component at a
;             time, by long or short names, and keeps it in both forms -
;             the long one for paths and 7147h, the 8.3 one for 47h.
;             With LFN=OFF a component must be an 8.3 name, as on any
;             drive: resolve_core is asked, and refuses a long one.
;=====================================================================
net_chdir:
        mov     [net_cddrv],al
        cmp     byte ptr [lfn_on],0
        jne     ncd_ask
        mov     esi,[ebp+F_EDX]
        call    resolve_core                    ; only to read the names
        jc      ncd_fail
        mov     ecx,[rp_cnt]
        xor     edx,edx
ncd_long:
        cmp     edx,ecx
        jae     ncd_ask
        cmp     byte ptr [rp_lflag+edx],0
        jne     ncd_nopath
        inc     edx
        jmp     short ncd_long
ncd_ask:
        mov     al,[net_cddrv]
        mov     ebx,REDIR_CHDIR
        call    net_req_begin
        mov     esi,[ebp+F_EDX]
        call    net_setpath
        cmp     byte ptr [lfn_on],0
        jne     ncd_fire
        mov     dword ptr [net_req+NR_SPARE],1  ; its 8.3 form within 63
ncd_fire:
        call    net_fire
        jc      net_err_general
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     net_err_status
        jmp     i21_ok
ncd_nopath:
        mov     ax,ERR_NOPATH
ncd_fail:
        jmp     i21_fail

;---------------------------------------------------------------------
; net_text - AL = a net drive, EBX = REDIR_GETCWD or REDIR_CANON, EDX =
;            NR_SPARE (0 the 8.3 form, 1 the long one), ESI -> a path
;            for CANON.  Asks the driver; the answer is in net_req's
;            NR_PATH, share-relative with no leading backslash.  CF with
;            AX = the error.  Preserves the other general registers.
;---------------------------------------------------------------------
net_text:
        push    edi
        call    net_req_begin
        mov     [net_req+NR_SPARE],edx
        cmp     ebx,REDIR_CANON
        jne     ntx_fire
        call    net_setpath
ntx_fire:
        call    net_fire
        jc      ntx_gen
        mov     eax,[net_req+NR_STATUS]
        or      eax,eax
        jnz     ntx_err
        pop     edi
        clc
        ret
ntx_gen:
        mov     eax,ERR_GENERAL
ntx_err:
        pop     edi
        stc
        ret

;---------------------------------------------------------------------
; net_textout - copy net_req's NR_PATH to EDI, which has ECX bytes (the
;               NUL included).  CF with AX = ERR_NOPATH when it does
;               not fit, which 47h's 64 bytes are the usual reason for.
;               Preserves the general registers but EAX.
;---------------------------------------------------------------------
net_textout:
        push    ecx
        push    esi
        push    edi
        mov     esi,OFFSET net_req + NR_PATH
nto_lp:
        or      ecx,ecx
        jz      nto_long
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      nto_done
        inc     esi
        inc     edi
        dec     ecx
        jmp     short nto_lp
nto_done:
        pop     edi
        pop     esi
        pop     ecx
        clc
        ret
nto_long:
        pop     edi
        pop     esi
        pop     ecx
        mov     ax,ERR_NOPATH
        stc
        ret

;=====================================================================
; net_getcwd - 47h and 7147h on a net drive.  AL = the drive, EDX = 0
;              for 47h (the 8.3 form, into 64 bytes) or 1 for 7147h
;              (the long form, into 260).  The caller's buffer at F_ESI
;              is checked here.  DOS style: no drive, no backslash.
;=====================================================================
net_getcwd:
        mov     ecx,64
        or      edx,edx
        jz      ngc_room
        mov     ecx,LFN_MAX
ngc_room:
        push    eax
        mov     eax,[ebp+F_ESI]
        call    cbuf_ok
        pop     eax
        jc      buf_deny
        push    ecx
        xor     edx,1                           ; GETCWD's NR_SPARE: 1 = 8.3
        mov     ebx,REDIR_GETCWD
        call    net_text
        pop     ecx
        jc      i21_fail
        mov     edi,[ebp+F_ESI]
        call    net_textout
        jc      i21_fail
        jmp     i21_ok

;=====================================================================
; net_canon - 60h and 7160h on a net drive.  AL = the drive, ESI -> the
;             path, EDX = 0 the 8.3 form (60h, 7160h CL=0 and 1) or 1
;             the long form (7160h CL=2), ECX = the caller's room at
;             F_EDI, already checked.  The answer is "X:\" and the
;             driver's text: intermediates looked up on the server, a
;             final name that is not there as typed.
;=====================================================================
net_canon:
        push    eax
        push    ecx
        mov     ebx,REDIR_CANON
        call    net_text
        pop     ecx
        pop     edx                             ; DL = the drive
        jc      i21_fail
        mov     edi,[ebp+F_EDI]
        add     dl,'A'
        mov     [edi],dl
        mov     byte ptr [edi+1],':'
        mov     byte ptr [edi+2],'\'
        add     edi,3
        sub     ecx,3
        call    net_textout
        jc      i21_fail
        jmp     i21_ok

;=====================================================================
; fF1_netuse - 21h/F1h AL=27h: NET USE, for a native NET.EXE.  Bits
;              16-23 of EAX = NETUSE_*, EDI -> a netuse_req of ECX bytes
;              which the kernel checks.  Native programs only.
;=====================================================================
fF1_netuse:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     i21_unsup
        mov     eax,[ebp+F_EAX]
        shr     eax,16
        cmp     al,NETUSE_PWL
        je      fnu_pwl
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
        cmp     eax,[drv_ceil]                  ; past LASTDRIVE= is no letter
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
        jz      fnu_smb2
        or      dword ptr [net_req+NR_SPARE],1
fnu_smb2:
        ; NUF_SAVED: the password from the password list (NET LOGON),
        ; which the driver holds - NR_SPARE bit 1
        test    dword ptr [edi+NU_FLAGS],NUF_SAVED
        jz      fnu_fire
        or      dword ptr [net_req+NR_SPARE],2
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
        ; which step failed and what the server said, for NET.EXE to
        ; name (REDIR.H MNT_*)
        mov     edi,[ebp+F_EDI]
        mov     eax,[net_req+NR_COUNT]
        mov     [edi+NU_STEP],eax
        mov     eax,[net_req+NR_RESULT]
        mov     [edi+NU_SRVSTATUS],eax
        mov     eax,[net_req+NR_STATUS]
        mov     [edi+NU_STATUS],eax
        mov     ax,word ptr [net_req+NR_STATUS]
        jmp     i21_fail
fnu_nodrv:
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_STATUS],ERR_GENERAL
        mov     dword ptr [edi+NU_STEP],MNT_NODRIVER
        mov     dword ptr [edi+NU_SRVSTATUS],0
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
        ; NOT WHILE AN IMAGE IS MOUNTED FROM IT.  The image reads through
        ; a handle on this drive's session (IMAGE.INC, IMF_NET), and the
        ; session goes with the letter.  ERR_ACCESS, which NET.EXE says
        ; in those words.
        mov     al,[net_nudrive]
        call    img_netbusy
        jc      fnu_imgbusy
        mov     al,[net_nudrive]
        mov     ebx,REDIR_UMOUNT
        call    net_req_begin
        call    net_fire                        ; best effort
        mov     al,[net_nudrive]
        call    drv_slot
        mov     byte ptr [ebx+DRV_FLAGS],0
        mov     byte ptr [ebx+DRV_TYPE],0
        mov     byte ptr [ebx+DRV_BIOS],0
        ; AND FORGET THE LETTER'S MOUNT, whichever volume is current.  Only
        ; the current one's was cleared, so a letter unmapped while another
        ; was current kept fat_ok and vol_fs = FS_NET with no row behind
        ; them: DIR of it said "Volume in drive P" and walked a share as
        ; FAT, where it should have said the drive is not there.
        mov     al,[net_nudrive]
        call    vol_swap                        ; its state, no mount
        mov     byte ptr [fat_ok],0
        xor     eax,eax
        mov     edi,[ebp+F_EDI]
        mov     [edi+NU_STATUS],eax
        jmp     i21_ok
fnu_notmapped:
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_STATUS],ERR_BADDRIVE
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail
fnu_imgbusy:
        mov     edi,[ebp+F_EDI]
        mov     dword ptr [edi+NU_STATUS],ERR_ACCESS
        mov     ax,ERR_ACCESS
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

;--- NETUSE_PWL: the password list (NET LOGON) -----------------------
; The whole block goes to the redirector, which keeps the list: the
; kernel checks that it is the program's to write and passes its flat
; address, and the driver answers inside it (REDIR.H pwl_req).  Nothing
; here reads it, so nothing here holds a copy.
fnu_pwl:
        mov     eax,[ebp+F_EDI]
        mov     ecx,PWLR_SIZE
        call    cbuf_ok
        jc      buf_deny
        mov     al,0FFh                         ; no drive
        mov     ebx,REDIR_PWL
        call    net_req_begin
        mov     eax,[ebp+F_EDI]                 ; flat, checked
        mov     [net_req+NR_BUFPTR],eax
        mov     dword ptr [net_req+NR_COUNT],PWLR_SIZE
        call    net_fire
        jc      fnu_pwlnone
        cmp     dword ptr [net_req+NR_STATUS],0
        jne     fnu_pwlnone                     ; a driver without the list
        jmp     i21_ok
fnu_pwlnone:
        mov     ax,ERR_GENERAL                  ; no redirector to keep one
        jmp     i21_fail

;=====================================================================
; Data
;=====================================================================
net_nudrive db  0
net_lfndrv  db  0
net_cddrv   db  0                       ; net_chdir: the drive
            db  0
net_tmph    dw  0
            dw  0
net_cl_esi  dd  0
net_fh      dd  0                       ; the find handle being filled
net_fhidx   dd  0                       ; ...and its index (the 714Eh handle)
net_tsec    dd  0,0                     ; net_size: the share's size and its
net_fsec    dd  0,0                     ; free space, in 512-byte sectors

; The request block.  One, in the kernel's data: a net file operation
; runs to completion inside one INT 21h call and never nests, so a
; single block is enough and it is never an interrupt's.
net_req     dd  (NR_SIZE / 4) dup(0)
