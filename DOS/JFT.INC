;=====================================================================
; JFT.INC - a DOS program's own handle table, and the reference counts
;           that make its handles DOS handles.
;
; IN DOS A HANDLE IS AN INDEX INTO THE PROGRAM'S OWN TABLE, the job file
; table in its PSP, and what that table holds is an index into the one
; system file table.  Two levels, and the second is SHARED: a DUP is a
; second entry pointing at the same open file, so the two advance one
; file position; a child inherits its parent's entries, so a closed
; handle in the child is a reference dropped and not a file closed; and
; a program can only ever name the files in its own table.
;
; PM-DOS HAD ONE LEVEL until v0.60: a handle was a slot in the kernel's
; table, for every program, and 45h/46h COPIED a slot rather than
; sharing it.  Native programs are written for that and keep it.  DOS
; programs are not, and MS-DOS 6.22's own COMMAND.COM is what showed
; how far apart the two are.  After every command it closes handles 5
; to 19 - its own table's, which DOS keeps empty - and here those were
; the kernel's slots: the one holding CMD.EXE's saved standard output
; among them.  CMD.EXE could not put its redirection back, and wrote a
; file size that lost everything the DOS program had appended.
;
; So a DOS program - V86 code in a box, or a DPMI client, which is the
; same process in protected mode - now has the table its PSP has always
; had room for, and this file is what reads it:
;
;   jft_pre / jft_post   around every INT 21h a guest makes: a handle
;                        in BX becomes its slot on the way in and is put
;                        back on the way out; a slot a create or open
;                        hands back becomes the program's lowest free
;                        handle, which is what DOS gave; and 3Eh, 45h,
;                        46h and 67h are done here outright, because
;                        they ARE the table
;   jft_inherit          a new PSP's table: the parent's entries, less
;                        any opened no-inherit, or for the first program
;                        of a box CMD.EXE's three and AUX and PRN
;   jft_exit             a program's references, given back when it ends
;
; jft_ref counts the table entries that point at each slot.  A slot is
; closed when its count falls to zero - EXCEPT slots 0-2, which are the
; standard handles and are never a guest's to close.
;
; The PSP that counts is vdm_curpsp, the one 51h/62h answer, because it
; is DOS's rule: a TSR that switches PSP with 50h to do its file I/O is
; asking to use the table of the PSP it switched to.
;=====================================================================

JFT_STD         EQU     3               ; slots 0-2: the standard handles,
                                        ; never closed here - as 3Eh keeps them
JFT_MAXLEN      EQU     255             ; the most entries a table is taken at

; The per-call record, in the frame's F_ESP slot: popad never reads
; that one back, and a frame is per call, so these nest where a global
; could not - a guest's critical-error handler makes DOS calls while
; the call that failed is still open.  Low word: the guest's handle.
; High word: what jft_post has to do.
JOP_NONE        EQU     0
JOP_RESTBX      EQU     1               ; BX was a handle: put it back
JOP_NEWH        EQU     2               ; AX will be a new slot: make it a handle
JOP_STD         EQU     3               ; a character call: std_set put back
                                        ; (low word = the old AL, AH for it)

;=====================================================================
; jft_where - the current guest PSP's table.  ESI = its flat address,
;             ECX = how many entries.  CF if there is no guest table to
;             use: no box, no PSP (a DEVICE= driver's INIT has neither),
;             or a PSP whose table is not believable.
;=====================================================================
jft_where:
        push    eax
        push    edx
        cmp     dword ptr ds:[vdm_busy],0
        je      jw_none
        movzx   eax,word ptr ds:[vdm_curpsp]
        or      eax,eax
        jz      jw_none
        shl     eax,4
        movzx   ecx,word ptr [eax+RSP_JFTLEN]
        or      ecx,ecx
        jz      jw_none
        cmp     ecx,JFT_MAXLEN
        jbe     jw_len
        mov     ecx,JFT_MAXLEN
jw_len:
        movzx   esi,word ptr [eax+RSP_JFTPTR+2] ; segment
        shl     esi,4
        movzx   edx,word ptr [eax+RSP_JFTPTR]   ; offset
        add     esi,edx
        lea     edx,[esi+ecx]
        cmp     edx,110000h                     ; a real-mode address or
        ja      jw_none                         ; it is not one
        pop     edx
        pop     eax
        clc
        ret
jw_none:
        pop     edx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; jft_h2s - EBX = a guest handle, ESI/ECX = its table.  EBX = the slot
;           it names; CF if it names nothing.
;---------------------------------------------------------------------
jft_h2s:
        cmp     ebx,ecx
        jae     jh_bad
        movzx   ebx,byte ptr [esi+ebx]
        cmp     ebx,MAX_HANDLES
        jae     jh_bad                  ; FFh - closed - lands here too
        push    eax
        mov     eax,ebx
        imul    eax,SFT_LEN
        test    dword ptr [sft_tbl+eax+SFT_FLAGS],SFTF_INUSE
        pop     eax
        jz      jh_bad
        clc
        ret
jh_bad:
        stc
        ret

;---------------------------------------------------------------------
; jft_free - ESI/ECX = a table.  EAX = its lowest free entry; CF if it
;            is full.  Lowest, because that is the number DOS hands out
;            and programs have been caught assuming the first file they
;            open is handle 5.
;---------------------------------------------------------------------
jft_free:
        xor     eax,eax
jf_loop:
        cmp     eax,ecx
        jae     jf_full
        cmp     byte ptr [esi+eax],0FFh
        je      jf_got
        inc     eax
        jmp     short jf_loop
jf_got:
        clc
        ret
jf_full:
        stc
        ret

;---------------------------------------------------------------------
; jft_release - EBX = a slot one table entry has stopped naming.  The
;               last reference closes it, unless it is a standard one.
;               Everything preserved.
;---------------------------------------------------------------------
jft_release:
        pushad
        cmp     ebx,MAX_HANDLES
        jae     jr_out
        cmp     byte ptr [jft_ref+ebx],0
        je      jr_last                 ; (a count that was never taken)
        dec     byte ptr [jft_ref+ebx]
        jnz     jr_out
jr_last:
        cmp     ebx,JFT_STD
        jb      jr_out
        mov     eax,ebx
        imul    eax,SFT_LEN
        test    dword ptr [sft_tbl+eax+SFT_FLAGS],SFTF_INUSE
        jz      jr_out
        mov     eax,3E00h               ; the kernel's own close, on the
        call    k21                     ; slot - k21 frames nest
jr_out:
        popad
        ret

;=====================================================================
; jft_pre - EBP = the frame of a guest's INT 21h, before the handler.
;           CF clear: go on and dispatch it.  CF set: it has been done
;           here, answer and all, and must not be dispatched.
;=====================================================================
jft_pre:
        mov     dword ptr [ebp+F_ESP],JOP_NONE
        pushad
        call    jft_where
        jc      jp_pass
        mov     al,byte ptr [ebp+F_EAX+1]       ; AH
        mov     dl,byte ptr [ebp+F_EAX]         ; AL
        cmp     al,3Eh
        je      jp_close
        cmp     al,45h
        je      jp_dup
        cmp     al,46h
        je      jp_dup2
        cmp     al,67h
        je      jp_count
        cmp     al,01h                          ; the character functions:
        jb      jp_notchar                      ; they read handle 0 and
        cmp     al,0Ch                          ; write handle 1, whatever
        jbe     jp_char                         ; this program's 0 and 1 are
jp_notchar:

        ; a handle in BX
        cmp     al,3Fh
        je      jp_bx
        cmp     al,40h
        je      jp_bx
        cmp     al,42h
        je      jp_bx
        cmp     al,57h
        je      jp_bx
        cmp     al,5Ch
        je      jp_bx
        cmp     al,68h
        je      jp_bx
        cmp     al,6Ah
        je      jp_bx
        cmp     al,44h
        jne     jp_not44
        mov     edi,OFFSET jft_ioctlh           ; the IOCTLs that take a
jp_44:                                          ; handle, not a drive
        cmp     byte ptr [edi],0FFh
        je      jp_pass
        cmp     dl,[edi]
        je      jp_bx
        inc     edi
        jmp     short jp_44
jp_not44:
        cmp     al,71h
        jne     jp_not71
        cmp     dl,0A6h                         ; get file info by handle
        je      jp_bx
        cmp     dl,6Ch                          ; long-name open
        je      jp_create
        jmp     short jp_pass
jp_not71:
        ; a new handle in AX afterwards
        cmp     al,3Ch
        je      jp_create
        cmp     al,3Dh
        je      jp_create
        cmp     al,5Ah
        je      jp_create
        cmp     al,5Bh
        je      jp_create
        cmp     al,6Ch
        je      jp_create
jp_pass:
        popad
        clc
        ret

jp_bx:
        movzx   ebx,word ptr [ebp+F_EBX]
        push    ebx
        call    jft_h2s
        pop     eax
        jc      jp_badh
        or      eax,JOP_RESTBX SHL 16           ; remember the guest's BX...
        mov     dword ptr [ebp+F_ESP],eax
        mov     word ptr [ebp+F_EBX],bx         ; ...and hand over the slot
        popad
        clc
        ret

jp_create:
        mov     dword ptr [ebp+F_ESP],JOP_NEWH SHL 16
        popad
        clc
        ret

;--- 01h-0Ch: aim std_in and std_out at this program's 0 and 1 ---------
; What they pointed at before is written down to go back afterwards.
; A handle that names nothing leaves that side where it was.
jp_char:
        movzx   eax,word ptr [std_islot]        ; std_islot, std_oslot
        or      eax,JOP_STD SHL 16
        mov     dword ptr [ebp+F_ESP],eax
        mov     edx,eax                         ; DL/DH: the current pair
        xor     ebx,ebx
        call    jft_h2s
        jc      jpc_out
        mov     dl,bl
jpc_out:
        mov     ebx,1
        call    jft_h2s
        jc      jpc_set
        mov     dh,bl
jpc_set:
        mov     eax,edx
        call    std_set
        popad
        clc
        ret

;--- 3Eh: the entry goes, and the slot if nothing else names it -------
jp_close:
        movzx   ebx,word ptr [ebp+F_EBX]
        mov     edx,ebx
        call    jft_h2s
        jc      jp_badh
        mov     byte ptr [esi+edx],0FFh
        call    jft_release
        jmp     jp_ok

;--- 45h: a second entry naming the same slot -------------------------
jp_dup:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    jft_h2s
        jc      jp_badh
        call    jft_free
        jc      jp_full
        mov     [esi+eax],bl
        inc     byte ptr [jft_ref+ebx]
        mov     word ptr [ebp+F_EAX],ax
        jmp     jp_ok

;--- 46h: CX names what BX names; what CX named is let go first --------
jp_dup2:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    jft_h2s
        jc      jp_badh
        movzx   edx,word ptr [ebp+F_ECX]
        cmp     edx,ecx
        jae     jp_badh
        movzx   eax,byte ptr [esi+edx]
        cmp     eax,ebx
        je      jp_ok                   ; already the same file
        mov     [esi+edx],bl
        inc     byte ptr [jft_ref+ebx]
        cmp     eax,MAX_HANDLES
        jae     jp_ok                   ; CX was closed: nothing to let go
        mov     ebx,eax
        call    jft_release
        jmp     jp_ok

;--- 67h: a bigger table, in the program's own memory -----------------
; Never a smaller one - DOS refused that while handles were open above
; the new size, and succeeding without shrinking is the same answer to
; every program that asks for insurance.  And not refused for want of
; memory either, though DOS would have: a .COM asks at start-up while
; it still owns every paragraph, and it is asking for insurance - the
; kernel always answered 67h yes, and a program that really does open
; more than twenty is told so at the open that does not fit.  The old table's entries are
; copied and the rest are closed; an old table that was not the PSP's
; own twenty bytes was 67h's before, and goes back.
jp_count:
        movzx   ebx,word ptr [ebp+F_EBX]
        cmp     ebx,ecx
        jbe     jp_ok
        cmp     ebx,JFT_MAXLEN
        jbe     jp_cnt
        mov     ebx,JFT_MAXLEN
jp_cnt:
        mov     [jft_newlen],ebx
        add     ebx,15
        shr     ebx,4
        call    lm_alloc                ; -> AX = the segment; ECX and
        jc      jp_ok                   ; ESI (the old table) survive it
        mov     word ptr [jft_newseg],ax
        mov     dx,word ptr ds:[vdm_curpsp]
        call    lm_setown
        movzx   edi,ax
        shl     edi,4
        push    ecx
        rep     movsb                   ; the old entries
        pop     ecx
        neg     ecx
        add     ecx,[jft_newlen]
        mov     al,0FFh
        rep     stosb                   ; and the new ones closed
        ; the old table goes back if an earlier 67h made it
        movzx   edx,word ptr ds:[vdm_curpsp]
        shl     edx,4
        movzx   eax,word ptr [edx+RSP_JFTPTR+2]
        cmp     ax,word ptr ds:[vdm_curpsp]
        je      jp_cntown
        cmp     word ptr [edx+RSP_JFTPTR],0
        jne     jp_cntown
        call    lm_free
jp_cntown:
        movzx   edx,word ptr ds:[vdm_curpsp]
        shl     edx,4
        mov     ebx,[jft_newlen]
        mov     word ptr [edx+RSP_JFTLEN],bx
        mov     word ptr [edx+RSP_JFTPTR],0
        mov     ax,word ptr [jft_newseg]
        mov     word ptr [edx+RSP_JFTPTR+2],ax
        jmp     jp_ok

jp_ok:
        and     dword ptr [ebp+F_FLAGS],NOT FL_CF
        popad
        stc
        ret
jp_badh:
        mov     word ptr [ebp+F_EAX],ERR_BADHANDLE
        jmp     jp_err
jp_full:
        mov     word ptr [ebp+F_EAX],ERR_NOHANDLES
        jmp     jp_err
jp_nomem:
        mov     word ptr [ebp+F_EAX],ERR_NOMEM
jp_err:
        or      dword ptr [ebp+F_FLAGS],FL_CF
        popad
        stc
        ret

; the 44h subfunctions whose BX is a handle
jft_ioctlh      db      00h, 01h, 02h, 03h, 06h, 07h, 0Ah, 0Ch, 10h, 0FFh

;=====================================================================
; jft_post - EBP = the same frame, after the handler.  What jft_pre
;            wrote down is done now.  Everything preserved.
;=====================================================================
jft_post:
        pushad
        mov     eax,dword ptr [ebp+F_ESP]
        mov     edx,eax
        shr     edx,16
        cmp     edx,JOP_RESTBX
        je      jq_restbx
        cmp     edx,JOP_NEWH
        je      jq_newh
        cmp     edx,JOP_STD
        jne     jq_out
        call    std_set                         ; AX: the pair it was
jq_out:
        mov     dword ptr [ebp+F_ESP],JOP_NONE
        popad
        ret
jq_restbx:
        mov     word ptr [ebp+F_EBX],ax
        jmp     short jq_out

jq_newh:
        test    dword ptr [ebp+F_FLAGS],FL_CF
        jnz     jq_out                  ; it failed: there is no slot
        movzx   ebx,word ptr [ebp+F_EAX]
        cmp     ebx,MAX_HANDLES
        jae     jq_out
        call    jft_where
        jc      jq_out
        call    jft_free
        jc      jq_full
        mov     [esi+eax],bl
        inc     byte ptr [jft_ref+ebx]
        mov     word ptr [ebp+F_EAX],ax
        jmp     short jq_out
jq_full:
        ; No room in the program's table: the file it just opened is
        ; closed again and the open fails, exactly as DOS fails it.
        call    jft_release
        mov     word ptr [ebp+F_EAX],ERR_NOHANDLES
        or      dword ptr [ebp+F_FLAGS],FL_CF
        jmp     short jq_out

;=====================================================================
; jft_inherit - EDI = a new PSP, flat; [vdm_parpsp] = the program that
;               is starting it, or 0 for the first program of a box.
;               Its twenty-entry table, filled in.
;
; A CHILD INHERITS WHAT ITS PARENT HAS OPEN, which is how redirection
; survives an EXEC: the parent points 1 at a file, and the child's 1 is
; that file.  A file opened with the no-inherit bit (80h in the open
; mode) stays behind.  More than twenty entries are never passed on -
; the child's table is twenty long until it asks 67h for more.
;
; The first program of a box has no DOS parent, and gets what CMD.EXE
; has - its three standard handles, slots 0 to 2 - and AUX and PRN.
;=====================================================================
jft_inherit:
        pushad
        mov     ebx,edi
        lea     edi,[ebx+RSP_JFT]
        mov     ecx,20
        mov     al,0FFh
        rep     stosb
        movzx   eax,word ptr ds:[vdm_parpsp]
        or      eax,eax
        jz      ji_top
        push    ebx
        mov     ecx,eax
        shl     ecx,4
        movzx   esi,word ptr [ecx+RSP_JFTPTR+2]
        shl     esi,4
        movzx   edx,word ptr [ecx+RSP_JFTPTR]
        add     esi,edx
        movzx   ecx,word ptr [ecx+RSP_JFTLEN]
        pop     ebx
        cmp     ecx,20
        jbe     ji_len
        mov     ecx,20
ji_len:
        lea     edx,[esi+ecx]
        cmp     edx,110000h
        ja      ji_out                  ; a parent with no believable table
        xor     edx,edx
ji_one:
        cmp     edx,ecx
        jae     ji_out
        movzx   eax,byte ptr [esi+edx]
        cmp     eax,MAX_HANDLES
        jae     ji_next
        push    eax
        imul    eax,SFT_LEN
        test    dword ptr [sft_tbl+eax+SFT_FLAGS],SFTF_INUSE
        jz      ji_skip
        test    byte ptr [sft_tbl+eax+SFT_MODE],80h
        jnz     ji_skip                 ; opened no-inherit
        pop     eax
        mov     byte ptr [ebx+RSP_JFT+edx],al
        inc     byte ptr [jft_ref+eax]
        jmp     short ji_next
ji_skip:
        pop     eax
ji_next:
        inc     edx
        jmp     short ji_one
ji_top:
        xor     eax,eax
ji_std:
        mov     byte ptr [ebx+RSP_JFT+eax],al
        inc     byte ptr [jft_ref+eax]
        inc     eax
        cmp     eax,JFT_STD
        jb      ji_std

        ; 3 AND 4 ARE AUX AND PRN, OPENED FOR THIS BOX.  They are not
        ; slots 3 and 4: PM-DOS keeps three standard slots and no more
        ; (3Eh protects 0-2), so 3 and 4 are ordinary slots that may
        ; hold anybody's file - the box used to be handed them anyway,
        ; and a guest writing to its printer through handle 4 wrote
        ; into whatever file that was.  jft_exit closes these again.
        mov     edx,OFFSET jft_auxnm
        mov     edi,3
        call    ji_dev
        mov     edx,OFFSET jft_prnnm
        mov     edi,4
        call    ji_dev
ji_out:
        popad
        ret

; ji_dev - open the device named at EDX and make it entry EDI of the
;          table at EBX (the PSP).  A device that will not open leaves
;          the entry closed.
ji_dev:
        push    ebx
        mov     eax,3D02h
        call    k21
        pop     ebx
        jc      jd_out
        movzx   eax,ax
        cmp     eax,MAX_HANDLES
        jae     jd_out
        mov     byte ptr [ebx+RSP_JFT+edi],al
        mov     byte ptr [jft_ref+eax],1
jd_out:
        ret

jft_auxnm       db      'AUX',0
jft_prnnm       db      'PRN',0

;=====================================================================
; jft_exit - the program ending is [vdm_pspseg]'s: every entry in its
;            table is let go.  A slot nobody else names closes, which is
;            the sweep DOS does at termination.
;=====================================================================
jft_exit:
        pushad
        movzx   eax,word ptr ds:[vdm_pspseg]
        or      eax,eax
        jz      jx_out
        shl     eax,4
        movzx   ecx,word ptr [eax+RSP_JFTLEN]
        cmp     ecx,JFT_MAXLEN
        jbe     jx_len
        mov     ecx,JFT_MAXLEN
jx_len:
        movzx   esi,word ptr [eax+RSP_JFTPTR+2]
        shl     esi,4
        movzx   edx,word ptr [eax+RSP_JFTPTR]
        add     esi,edx
        lea     edx,[esi+ecx]
        cmp     edx,110000h
        ja      jx_out
        xor     edx,edx
jx_one:
        cmp     edx,ecx
        jae     jx_out
        movzx   ebx,byte ptr [esi+edx]
        cmp     ebx,0FFh
        je      jx_next
        mov     byte ptr [esi+edx],0FFh
        call    jft_release
jx_next:
        inc     edx
        jmp     short jx_one
jx_out:
        popad
        ret

jft_newlen      dd      0
jft_newseg      dd      0
jft_ref         db      MAX_HANDLES dup(0)
