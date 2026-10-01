;=====================================================================
; SHARE.INC - file sharing and record locking, in the kernel
;
; SHARE.EXE was the part of DOS that made the sharing bits of an open
; mean something and made 5Ch lock anything.  Without it a second
; open in deny-write mode succeeded, a locked record was a record
; anyone could write, and INT 2Fh AX=1000h said so - which is why
; Windows, Word, dBASE and every installer that wants to be sure of
; its files asked, and some refused to run without it.  It was a
; program because DOS was a program's worth of memory away from
; having it inside; here it is the forty lines it should have been,
; and CONFIG.SYS says how many locks:
;
;       SHARE=locks[,space]     SHARE.EXE's /L: and /F: - the second
;                               is accepted for the line's sake and
;                               means nothing here, because no names
;                               are stored: a file is known by where
;                               its directory entry is
;       SHARE=ON                on, with SHARE_DFLTLK locks (the default)
;       SHARE=OFF               as DOS without SHARE: every open
;                               succeeds, every lock is granted, and
;                               2Fh/1000h says nobody is home
;
;---------------------------------------------------------------------
; THE RULES ARE SHARE.EXE'S, out of S:\DOS6SRC\cmd\share\gshare.asm,
; and they are three:
;
;   OPEN.  A new open is judged against every existing open of the
;   SAME FILE - the same volume, directory and entry, which is what
;   SFT_DRIVE, SFT_DIRSTART and SFT_DIRENT say - by the table CUCA
;   below, indexed by (sharing mode * 3 + access) of each.  Two
;   compatibility-mode opens never conflict on one machine (SHARE
;   compared user ids, and a machine has one); a compatibility open
;   against any deny mode always does, both ways.  A compatibility
;   read of a READ-ONLY file counts as deny-write read, so that two
;   programs reading a read-only file in compat mode do not fight
;   (GOM).  A conflict is a critical error, 0Dh, "Sharing violation",
;   with Retry and Fail on offer: Retry judges again, anything else
;   fails the open with error 20h.
;
;   LOCK.  5Ch locks a byte range on the file behind a handle.  It is
;   refused, error 21h, if it overlaps ANY lock on the same file,
;   including this handle's own; unlocking must name a lock exactly.
;   A close releases the handle's locks, and a program's end releases
;   every lock it took, on inherited handles included.
;
;   READ AND WRITE.  A transfer that overlaps a lock is refused
;   unless the lock was taken through THIS handle BY THIS program -
;   which is SLE's rule, and is what lets a child lock a range on a
;   handle it inherited and then use it.  The refusal is a critical
;   error, 0Eh, "Lock violation", Retry or Fail; Fail is error 21h.
;
; WHAT IS DELIBERATELY NOT HERE: the retry count and delay of 44h/0Bh
; (a wait for another process on a machine that runs one at a time),
; the MFT of file names (the identity above does the job with no
; string compares), and 2Fh/10xxh beyond the install check (SHARE's
; internal entry points, which the DOS kernel called and this kernel
; is).
;
; Included by DOSMAIN.ASM after JFT.INC: it walks sft_tbl and calls
; crit_raise.
;=====================================================================

; One lock.  LK_SFT is zero when the slot is free, which is what the
; segment starts as.
LK_SFT          EQU     0               ; dd - the SFT entry it was taken on
LK_START        EQU     4               ; dd - first byte
LK_END          EQU     8               ; dd - one past the last, saturated
LK_OWNER        EQU     12              ; dd - cur_own when it was taken
LK_LEN          EQU     16

;=====================================================================
; share_check - may this open happen?  AL = the open mode byte as the
;               program gave it, DL = the file's attribute byte, EBX =
;               its directory index, and [cur_vol], [dc_start] and
;               [dc_isroot] say which directory - the state resolve
;               and fat_search leave, which is why this is called
;               right after them.  CF set and AX = ERR_SHARING if not.
;               Everything but AX preserved.
;
; A refusal goes to INT 24h first, as 0Dh with Retry and Fail on
; offer.  Retry asks the table again, which on this machine changes
; nothing unless a TSR closed something meanwhile - but it is what
; DOS did and a handler that says Retry is not lying to.
;=====================================================================
share_check:
        cmp     byte ptr [share_on],0
        je      shc_ok
        pushad
        mov     [shc_attr],dl
        mov     byte ptr [shc_have],0
        call    share_gom               ; AL -> the mode as judged, and
        call    share_csi               ; kept in shc_new; EAX = its index
        mov     dx,[share_cuca+eax*2]
        mov     [shc_word],dx
shc_again:
        mov     esi,OFFSET sft_tbl
        mov     ebp,MAX_HANDLES
shc_lp:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      shc_next
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     shc_next
        call    share_samefile          ; ESI against the context and EBX
        jne     shc_next

        mov     al,[esi+SFT_SHARE]
        mov     dl,[shc_attr]
        call    share_gom
        mov     ah,al
        and     ah,OM_SHARE             ; old sharing mode
        mov     dl,[shc_new]
        and     dl,OM_SHARE
        or      ah,dl
        jz      shc_next                ; both compat: one machine, one user
        call    share_csi               ; -> EAX = the old one's index
        lea     ecx,[eax+1]             ; bit 0 of the word is unused
        movzx   edx,word ptr [shc_word]
        shr     edx,cl
        test    dl,1
        jnz     shc_conflict
shc_next:
        add     esi,SFT_LEN
        dec     ebp
        jnz     shc_lp
        popad
shc_ok:
        clc
        ret

shc_conflict:
        mov     al,[cur_vol]
        mov     edi,CERR_SHARING
        call    share_raise
        cmp     al,CERR_RETRY
        je      shc_again
        popad
        mov     ax,ERR_SHARING
        stc
        ret

;---------------------------------------------------------------------
; share_samefile - is the open at ESI the file being resolved?  ZF set
;                  if so.  EBX = the directory index; the directory
;                  is the resolver's context.  Preserves all.
;---------------------------------------------------------------------
share_samefile:
        push    eax
        cmp     [esi+SFT_DIRENT],ebx
        jne     ssf_out
        movzx   eax,byte ptr [cur_vol]
        cmp     [esi+SFT_DRIVE],eax
        jne     ssf_out
        mov     eax,[dc_start]
        cmp     [esi+SFT_DIRSTART],eax
        jne     ssf_out
        mov     eax,[esi+SFT_FLAGS]
        and     eax,SFTF_DIRROOT
        cmp     byte ptr [dc_isroot],0
        je      ssf_notroot
        cmp     eax,SFTF_DIRROOT        ; ZF = that open is in the root too
        jmp     short ssf_out
ssf_notroot:
        test    eax,eax                 ; ZF = neither is
ssf_out:
        pop     eax                     ; POP leaves the flags alone
        ret

;---------------------------------------------------------------------
; share_gom - AL = a mode byte, DL = the file's attribute -> AL = the
;             mode as the table judges it (SHARE's GOM): a network
;             FCB is compat; a compat open of a read-only file is
;             deny-write read.  The judged NEW mode is also kept in
;             shc_new the first time through.
;---------------------------------------------------------------------
share_gom:
        push    eax
        and     al,OM_SHARE
        cmp     al,OM_NETFCB
        pop     eax
        jne     sgm_notnet
        and     al,OM_ACCESS            ; compat, same access
sgm_notnet:
        test    al,OM_SHARE
        jnz     sgm_out                 ; not compat: as given
        test    dl,ATTR_READONLY
        jz      sgm_out
        mov     al,OM_DENYWR OR OM_READ
sgm_out:
        cmp     byte ptr [shc_have],0
        jne     sgm_kept
        mov     [shc_new],al
        mov     byte ptr [shc_have],1
sgm_kept:
        ret

;---------------------------------------------------------------------
; share_csi - AL = a judged mode -> EAX = (sharing * 3) + access, the
;             index into share_cuca.  An access of 3 or more is not
;             one, and is judged as read/write.
;---------------------------------------------------------------------
share_csi:
        push    ecx
        movzx   ecx,al
        and     ecx,OM_ACCESS
        cmp     ecx,OM_RDWR
        jbe     scs_acc
        mov     ecx,OM_RDWR
scs_acc:
        movzx   eax,al
        and     eax,OM_SHARE
        shr     eax,4                   ; 0..4
        lea     eax,[eax+eax*2]         ; * 3
        add     eax,ecx
        pop     ecx
        ret

;---------------------------------------------------------------------
; share_raise - a critical error.  AL = the volume, EDI = the code,
;               -> AL = the handler's answer.  Preserves the rest.
;---------------------------------------------------------------------
share_raise:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     ah,CE_FAIL_OK OR CE_RETRY_OK
        call    crit_raise
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; share_lock - 5Ch.  ESI = the SFT entry, AL = 0 lock / 1 unlock,
;              EBX = the first byte, ECX = how many.  CF and AX on a
;              refusal.  Preserves the rest.
;
; SHARE=OFF grants everything, which is the pre-SHARE kernel and what
; a DOS without SHARE.EXE did: the call must not FAIL, because a
; program that locks a record and is refused stops.
;=====================================================================
share_lock:
        cmp     byte ptr [share_on],0
        je      shl_ok
        pushad
        mov     [shl_op],al
        mov     [shl_start],ebx
        add     ebx,ecx
        jnc     shl_endok
        mov     ebx,0FFFFFFFFh          ; past the end of everything
shl_endok:
        mov     [shl_end],ebx
        cmp     byte ptr [shl_op],0
        jne     shl_unlock
        or      ecx,ecx
        jz      shl_done                ; nothing to hold

        ; any lock on this file that overlaps is a refusal - own ones
        ; too, which is SHARE's rule for setting (SLE with Z clear)
        mov     edi,OFFSET share_lk
        mov     ecx,[share_nlk]
        xor     ebp,ebp                 ; EBP = a free slot, if seen
shl_scan:
        cmp     dword ptr [edi+LK_SFT],0
        jne     shl_used
        or      ebp,ebp
        jnz     shl_snext
        mov     ebp,edi
        jmp     short shl_snext
shl_used:
        call    share_samesft           ; ZF = the lock is on this file
        jne     shl_snext
        call    share_overlap           ; CF = it overlaps
        jc      shl_viol
shl_snext:
        add     edi,LK_LEN
        loop    shl_scan
        or      ebp,ebp
        jz      shl_full
        mov     [ebp+LK_SFT],esi
        mov     eax,[shl_start]
        mov     [ebp+LK_START],eax
        mov     eax,[shl_end]
        mov     [ebp+LK_END],eax
        mov     eax,[cur_own]
        mov     [ebp+LK_OWNER],eax
        call    share_count
shl_done:
        popad
shl_ok:
        clc
        ret

shl_unlock:
        mov     edi,OFFSET share_lk
        mov     ecx,[share_nlk]
shl_ulp:
        cmp     [edi+LK_SFT],esi
        jne     shl_unext
        mov     eax,[shl_start]
        cmp     [edi+LK_START],eax
        jne     shl_unext
        mov     eax,[shl_end]
        cmp     [edi+LK_END],eax
        jne     shl_unext
        mov     dword ptr [edi+LK_SFT],0
        call    share_count
        jmp     short shl_done
shl_unext:
        add     edi,LK_LEN
        loop    shl_ulp
shl_viol:
        popad
        mov     ax,ERR_LOCKVIOL
        stc
        ret
shl_full:
        popad
        mov     ax,ERR_SHAREBUF
        stc
        ret

;---------------------------------------------------------------------
; share_samesft - is the lock at EDI on the file the SFT at ESI is
;                 open on?  ZF set if so.  The same handle is the
;                 same file; two handles are compared by where the
;                 directory entry is.  Preserves all.
;---------------------------------------------------------------------
share_samesft:
        push    eax
        push    edx
        mov     edx,[edi+LK_SFT]
        cmp     edx,esi
        je      sss_out
        mov     eax,[edx+SFT_DIRENT]
        cmp     [esi+SFT_DIRENT],eax
        jne     sss_out
        mov     eax,[edx+SFT_DIRSTART]
        cmp     [esi+SFT_DIRSTART],eax
        jne     sss_out
        mov     eax,[edx+SFT_DRIVE]
        cmp     [esi+SFT_DRIVE],eax
        jne     sss_out
        mov     eax,[edx+SFT_FLAGS]
        xor     eax,[esi+SFT_FLAGS]
        test    eax,SFTF_DIRROOT        ; ZF = the same kind of directory
sss_out:
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; share_overlap - does [shl_start, shl_end) meet the lock at EDI?
;                 CF set if so.  Preserves all.
;---------------------------------------------------------------------
share_overlap:
        push    eax
        mov     eax,[shl_start]
        cmp     eax,[edi+LK_END]
        jae     sov_no                  ; we begin after it ends
        mov     eax,[shl_end]
        cmp     eax,[edi+LK_START]
        jbe     sov_no                  ; we end before it begins
        pop     eax
        stc
        ret
sov_no:
        pop     eax
        clc
        ret

;=====================================================================
; share_iochk - a read or a write is about to happen: ESI = the SFT
;               entry, ECX = bytes, from SFT_POS.  CF and AX =
;               ERR_LOCKVIOL if a lock somebody else holds is in the
;               way.  Preserves the rest.
;
; "Somebody else" is SLE's rule: a lock is the caller's own when it
; was taken on this very handle by the program now running, and only
; then.  The same program through another handle is refused, and so
; is a parent using a handle its child locked and then left locked.
;=====================================================================
share_iochk:
        cmp     byte ptr [share_on],0
        je      sio_ok
        jecxz   sio_ok
        cmp     dword ptr [share_held],0
        je      sio_ok                  ; no lock anywhere: the usual case
        pushad
        mov     eax,[esi+SFT_POS]
        mov     [shl_start],eax
        add     eax,ecx
        jnc     sio_end
        mov     eax,0FFFFFFFFh
sio_end:
        mov     [shl_end],eax
sio_again:
        mov     edi,OFFSET share_lk
        mov     ecx,[share_nlk]
sio_lp:
        cmp     dword ptr [edi+LK_SFT],0
        je      sio_next
        call    share_samesft
        jne     sio_next
        cmp     [edi+LK_SFT],esi
        jne     sio_test
        mov     eax,[cur_own]
        cmp     [edi+LK_OWNER],eax
        je      sio_next                ; our own, through this handle
sio_test:
        call    share_overlap
        jc      sio_viol
sio_next:
        add     edi,LK_LEN
        loop    sio_lp
        popad
sio_ok:
        clc
        ret
sio_viol:
        push    edi
        mov     eax,[esi+SFT_DRIVE]      ; the file's volume, which is not
        mov     edi,CERR_LOCK            ; necessarily the current one
        call    share_raise
        pop     edi
        cmp     al,CERR_RETRY
        je      sio_again
        popad
        mov     ax,ERR_LOCKVIOL
        stc
        ret

;=====================================================================
; share_release - the SFT entry at ESI is being freed: its locks go.
; share_relown  - the program EDX is ending: every lock it took goes,
;                 on handles it inherited as much as its own.
; Both preserve everything.
;=====================================================================
share_release:
        pushad
        mov     edi,OFFSET share_lk
        mov     ecx,SHARE_MAXLK         ; the whole table, whatever SHARE=
srl_lp:                                 ; says now: it may have been larger
        cmp     [edi+LK_SFT],esi
        jne     srl_next
        mov     dword ptr [edi+LK_SFT],0
srl_next:
        add     edi,LK_LEN
        loop    srl_lp
        call    share_count
        popad
        ret

share_relown:
        pushad
        mov     edi,OFFSET share_lk
        mov     ecx,SHARE_MAXLK
sro_lp:
        cmp     dword ptr [edi+LK_SFT],0
        je      sro_next
        cmp     [edi+LK_OWNER],edx
        jne     sro_next
        mov     dword ptr [edi+LK_SFT],0
sro_next:
        add     edi,LK_LEN
        loop    sro_lp
        call    share_count
        popad
        ret

; share_count - how many locks are held, into share_held: the cheap
;               test share_iochk makes before it walks anything.
share_count:
        push    eax
        push    ecx
        push    edi
        xor     eax,eax
        mov     edi,OFFSET share_lk
        mov     ecx,SHARE_MAXLK
sct_lp:
        cmp     dword ptr [edi+LK_SFT],0
        je      sct_next
        inc     eax
sct_next:
        add     edi,LK_LEN
        loop    sct_lp
        mov     [share_held],eax
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; share_int2F - INT 2Fh AX=1000h, the SHARE install check.  CF clear
;               if it was ours.  AL = FFh: installed.  With SHARE=OFF
;               the call goes down the chain and a SHARE.EXE may load,
;               which is right - the user said there was no SHARE.
;
; The body runs inside pushad, for cache_int2F's reason.
;=====================================================================
share_int2F:
        pushad
        mov     byte ptr [s2f_ours],0
        cmp     word ptr [vdm_ctx+V86C_EAX],1000h
        jne     s2f_out
        cmp     byte ptr [share_on],0
        je      s2f_out
        mov     byte ptr [vdm_ctx+V86C_EAX],0FFh
        mov     byte ptr [s2f_ours],1
s2f_out:
        popad
        cmp     byte ptr [s2f_ours],0
        je      s2f_chain
        clc
        ret
s2f_chain:
        stc
        ret

;=====================================================================
; The use-conflict table, SHARE's CUCA: row = the NEW open's index,
; bit (old index + 1) set = refuse.  Bit 0 is unused.
;=====================================================================
share_cuca  dw  0FFFFh                  ; compat     read
            dw  0FFFFh                  ; compat     write
            dw  0FFFFh                  ; compat     read/write
            dw  0FFFFh                  ; deny r/w   read
            dw  0FFFFh                  ; deny r/w   write
            dw  0FFFFh                  ; deny r/w   read/write
            dw  0DF7Fh                  ; deny write read
            dw  0DBFFh                  ; deny write write
            dw  0DFFFh                  ; deny write read/write
            dw  0BEFFh                  ; deny read  read
            dw  0B7FFh                  ; deny read  write
            dw  0BFFFh                  ; deny read  read/write
            dw  01C7Fh                  ; deny none  read
            dw  003FFh                  ; deny none  write
            dw  01FFFh                  ; deny none  read/write

;=====================================================================
; Data
;=====================================================================
share_on    db  1                       ; SHARE=OFF clears it
shc_attr    db  0                       ; share_check's file attribute
shc_have    db  0                       ; ...has shc_new been set
shc_new     db  0                       ; ...the new open's judged mode
shl_op      db  0                       ; share_lock's function
s2f_ours    db  0
            db  0,0
shc_word    dw  0                       ; the CUCA row for the new open
            dw  0
shl_start   dd  0                       ; the range being locked, tested
shl_end     dd  0                       ; or unlocked
share_nlk   dd  SHARE_DFLTLK            ; locks SHARE= allows
share_held  dd  0                       ; locks held right now

; The lock table, below the arena and not in the file.
_DOS    ENDS
_KBSS   SEGMENT
share_lk    db  SHARE_MAXLK * LK_LEN dup(?)
IF ($ - kbss_start) AND 3
        .ERR    what follows share_lk in _KBSS is not on a dword
ENDIF
_KBSS   ENDS
_DOS    SEGMENT
