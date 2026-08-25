;=====================================================================
; PATH.INC - path resolution and the current directory
;
; A path is normalized into an array of 11-byte components (absolute
; paths start empty, relative ones start from the current directory,
; "." and ".." are folded away), then walked from the volume root one
; directory at a time.
;
;   resolve_parent  path -> context = parent dir, name_buf = last
;                   component ([rp_noname]=1 when the path IS a
;                   directory reference like "\" or "..")
;   resolve_dir     path -> context = the directory itself
;   cwd_install     adopt the last resolution as the current directory
;
; Included by DOSMAIN.ASM (after FAT.INC).
;=====================================================================

;=====================================================================
; resolve_core - normalize the path at ESI into rp_names/rp_cnt.
;                A drive prefix (or its absence, meaning the default
;                drive) selects and if necessary mounts that volume,
;                so everything after this operates on the right disk.
;                CF set with AX = error on a bad drive or malformed
;                component.
;=====================================================================
resolve_core:
        pushad
        mov     byte ptr [rp_noname],0
        mov     byte ptr [rp_baddrv],0

;--- drive prefix ----------------------------------------------------
        cmp     byte ptr [esi+1],':'
        jne     rc_nodrive
        mov     al,[esi]
        or      al,20h
        sub     al,'a'
        cmp     al,MAX_DRIVES
        jae     rc_baddrive
        add     esi,2
        jmp     short rc_seldrv
rc_nodrive:
        mov     al,[cur_drive]          ; no prefix: the default drive
rc_seldrv:
        call    vol_select
        jc      rc_baddrive

        ; forget any long components from the previous resolution -
        ; and DISARM any long match a caller armed but never ran, or
        ; a later search would compare against a dead buffer.
        ; Components seeded from the cwd never have long text (the
        ; cwd is stored canonicalized to 8.3 aliases, deliberately).
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        push    ecx
        push    edi
        mov     edi,OFFSET rp_lptr
        xor     eax,eax
        mov     ecx,MAX_DEPTH
rc_zlong:
        mov     dword ptr [edi],eax
        add     edi,4
        loop    rc_zlong
        mov     edi,OFFSET rp_lflag
        mov     ecx,MAX_DEPTH
rc_zflag:
        mov     byte ptr [edi],al
        inc     edi
        loop    rc_zflag
        pop     edi
        pop     ecx

;--- absolute or relative start --------------------------------------
        xor     ecx,ecx                 ; ECX = component count
        cmp     byte ptr [esi],'\'
        je      rc_absolute
        cmp     byte ptr [esi],'/'
        je      rc_absolute

        ; relative: seed with the current directory
        mov     ecx,[cwd_cnt]
        push    esi
        mov     esi,OFFSET cwd_names
        mov     edi,OFFSET rp_names
        push    ecx
        imul    ecx,11
        rep     movsb
        pop     ecx
        pop     esi
        jmp     short rc_components

rc_absolute:
        inc     esi                     ; eat the slash

;--- component loop --------------------------------------------------
rc_components:
        cmp     byte ptr [esi],0
        je      rc_done

        ; "." and ".." get folded here, before 8.3 conversion
        cmp     byte ptr [esi],'.'
        jne     rc_normal
        cmp     byte ptr [esi+1],'.'
        je      rc_dotdot
        ; single "." - must be followed by \ / or end
        mov     al,[esi+1]
        or      al,al
        jz      rc_dot_eat
        cmp     al,'\'
        je      rc_dot_eat
        cmp     al,'/'
        je      rc_dot_eat
        jmp     short rc_normal         ; a name starting with '.'
rc_dot_eat:
        inc     esi
        jmp     short rc_sep
rc_dotdot:
        mov     al,[esi+2]
        or      al,al
        jz      rc_dd_eat
        cmp     al,'\'
        je      rc_dd_eat
        cmp     al,'/'
        je      rc_dd_eat
        jmp     short rc_normal
rc_dd_eat:
        ; ".." AT THE ROOT IS THE ROOT, not an error.  This used to
        ; refuse, on the reasoning that there is nothing above the root
        ; to go to - which is true and is not what DOS does.  "CD .."
        ; in the root of a real DOS quietly stays there, and "DIR ..\"
        ; lists the root, and the reason it matters beyond tidiness is
        ; that a path assembled by a program - a base plus "..\..\x" -
        ; routinely walks up more levels than it walks down, and every
        ; one of those failed here.
        ;
        ; Found by 21h/60h, which is the first call in PM-DOS whose
        ; whole job is to canonicalise a path the caller made up.
        add     esi,2
        or      ecx,ecx
        jz      rc_sep
        dec     ecx
        jmp     short rc_sep

rc_normal:
        cmp     ecx,MAX_DEPTH
        jae     rc_badpath
        mov     edi,ecx
        imul    edi,11
        add     edi,OFFSET rp_names
        mov     [rc_start],esi          ; where this component's text is
        mov     byte ptr [fc_long],0
        call    fn_conv                 ; ESI advances to the separator
        jc      rc_badpath
        ; keep the raw text beside the 8.3 rendering: when fn_conv was
        ; lossy the walk matches it against long names instead, and
        ; the 71h find uses it as the long half of its pattern always.
        ; A pointer into the CALLER's string is enough - it lives for
        ; the whole INT 21h call, which is all a resolution lasts.
        mov     eax,[rc_start]
        mov     [rp_lptr+ecx*4],eax
        mov     eax,esi
        sub     eax,[rc_start]
        cmp     eax,LFN_MAX-1           ; a component longer than a
        ja      rc_badpath              ; whole LFN can never match
        mov     [rp_llen+ecx*4],eax
        mov     al,[fc_long]
        mov     [rp_lflag+ecx],al
        inc     ecx

rc_sep:
        cmp     byte ptr [esi],'\'
        je      rc_eat
        cmp     byte ptr [esi],'/'
        je      rc_eat
        jmp     short rc_components
rc_eat:
        inc     esi
        cmp     byte ptr [esi],0        ; trailing separator: the path
        jne     rc_components           ; names a directory
        mov     byte ptr [rp_noname],1
        jmp     short rc_done

rc_done:
        mov     [rp_cnt],ecx
        popad
        clc
        ret
;--- No such drive letter, or a drive whose volume will not mount -
;--- and after a critical error the user answered "fail", the second
;--- of those is much the commoner.  The flag exists so a caller can
;--- tell "the disk is not there" from "the file is not there": both
;--- came back as "file not found" until v0.20, which is how DIR of
;--- an empty drive used to say "No files found" and then ask about
;--- the same dead disk a second time.
rc_baddrive:
        popad
        mov     byte ptr [rp_baddrv],1
        mov     ax,ERR_BADDRIVE
        stc
        ret
rc_badpath:
        popad
        mov     ax,ERR_NOPATH
        stc
        ret

;=====================================================================
; walk_to - set the directory context by walking the first ECX
;           components of rp_names from the volume root.
;           CF set with AX = ERR_NOPATH when something on the way is
;           missing or not a directory.
;=====================================================================
walk_to:
        pushad
        call    ctx_root
        xor     edx,edx                 ; components consumed
wt_lp:
        cmp     edx,ecx
        jae     wt_done

        push    ecx
        push    edx
        mov     edi,edx
        imul    edi,11
        add     edi,OFFSET rp_names
        cmp     byte ptr [rp_lflag+edx],0       ; 8.3 was lossy here?
        je      wt_nolong
        mov     eax,[rp_lptr+edx*4]             ; arm the long half -
        mov     [srch_long],eax                 ; fat_search consumes it
        mov     eax,[rp_llen+edx*4]
        mov     [srch_llen],eax
        ; ...and ONLY the long half.  The 8.3 rendering of this
        ; component threw characters away, so anything it matches on
        ; the way past is a different directory with a similar name.
        mov     byte ptr [srch_lonly],1
wt_nolong:
        mov     al,[srch_attr]          ; preserve the caller's filter
        push    eax
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        call    fat_search
        jnc     wt_found
        pop     eax
        mov     [srch_attr],al
        pop     edx
        pop     ecx
        popad
        mov     ax,ERR_NOPATH
        stc
        ret
wt_found:
        pop     eax
        mov     [srch_attr],al
        ; canonicalize: whichever name matched, the component becomes
        ; the entry's own 8.3 name.  The cwd and everything stored
        ; downstream stay pure 8.3 - which is how classic 47h keeps
        ; its 64-byte promise no matter what the user typed.
        push    ecx
        push    esi
        push    edi
        mov     ecx,11
        rep     movsb                   ; entry name -> rp_names slot
        pop     edi
        pop     esi
        pop     ecx
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jz      wt_notdir
        call    de_getclus              ; EAX = the subdirectory chain
        or      eax,eax
        jz      wt_isroot               ; ".." stored as 0 = the root
        call    ctx_chain
        jmp     short wt_next
wt_isroot:
        call    ctx_root
wt_next:
        pop     edx
        pop     ecx
        inc     edx
        jmp     short wt_lp
wt_notdir:
        pop     edx
        pop     ecx
        popad
        mov     ax,ERR_NOPATH
        stc
        ret
wt_done:
        popad
        clc
        ret

;=====================================================================
; resolve_parent - ESI = ASCIIZ path.  On success the directory
; context is the path's parent and name_buf holds the final component
; as an 11-byte pattern.  [rp_noname] = 1 when the path referred to a
; directory itself (root, ".." or trailing '\') - name_buf is then
; all '?'.  CF with AX = error otherwise.
;=====================================================================
resolve_parent:
        push    ecx
        push    esi
        push    edi
        call    resolve_core
        jc      rpx_out

        mov     ecx,[rp_cnt]
        or      ecx,ecx
        jz      rpx_asdir               ; no components at all
        cmp     byte ptr [rp_noname],0
        jne     rpx_asdir               ; trailing '\': a directory

        dec     ecx                     ; walk to the parent...
        call    walk_to
        jc      rpx_out

        push    esi                     ; ...and hand back the last name
        mov     esi,[rp_cnt]
        dec     esi
        imul    esi,11
        add     esi,OFFSET rp_names
        mov     edi,OFFSET name_buf
        push    ecx
        mov     ecx,11
        rep     movsb
        pop     ecx
        pop     esi

        ; export the final component's raw text, and when its 8.3
        ; rendering was lossy, ARM the long-name match for the
        ; caller's own fat_search (consume-once - 4Eh disarms it to
        ; keep the classic find pure 8.3)
        push    esi
        mov     esi,[rp_cnt]
        dec     esi
        mov     al,[rp_lflag+esi]
        mov     [rp_flong],al
        mov     eax,[rp_lptr+esi*4]
        mov     [rp_fptr],eax
        mov     eax,[rp_llen+esi*4]
        mov     [rp_flen],eax
        cmp     byte ptr [rp_flong],0
        je      rpx_noarm
        mov     eax,[rp_fptr]
        mov     [srch_long],eax
        mov     eax,[rp_flen]
        mov     [srch_llen],eax
        ; and the 8.3 pattern in name_buf is a lie about this name -
        ; see the long note in fat_search.  It is still filled in,
        ; because a CREATE needs somewhere to start from when it goes
        ; looking for a free ~N alias; it just may not MATCH.
        mov     byte ptr [srch_lonly],1
rpx_noarm:
        pop     esi
        clc
        jmp     short rpx_out

rpx_asdir:
        mov     byte ptr [rp_flong],0
        mov     dword ptr [rp_fptr],0
        mov     dword ptr [rp_flen],0
        mov     ecx,[rp_cnt]
        call    walk_to                 ; context = the directory itself
        jc      rpx_out
        mov     byte ptr [rp_noname],1
        push    edi                     ; name_buf = match-anything
        push    ecx
        mov     edi,OFFSET name_buf
        mov     ecx,11
        mov     al,'?'
rpx_fill:
        mov     [edi],al
        inc     edi
        loop    rpx_fill
        pop     ecx
        pop     edi
        clc
rpx_out:
        pop     edi
        pop     esi
        pop     ecx
        ret

;=====================================================================
; resolve_dir - ESI = ASCIIZ path.  Context becomes the directory the
; whole path names.  CF with AX = error if it is not a directory.
;=====================================================================
resolve_dir:
        push    ecx
        call    resolve_core
        jc      rd_out
        mov     ecx,[rp_cnt]
        call    walk_to
rd_out:
        pop     ecx
        ret

;=====================================================================
; cwd_install - adopt rp_names/rp_cnt (already verified by a walk) as
; the current directory.  EAX = the directory's start cluster (0 for
; the root).  Fails (CF) if the resulting path would not fit in the
; 64-byte buffer DOS promises 47h callers.
;=====================================================================
cwd_install:
        pushad

        ; measure the dotted rendering first
        mov     ecx,[rp_cnt]
        mov     edx,ecx                 ; separators (n-1, roughly) plus
        imul    edx,13                  ; 12 chars max per component
        cmp     edx,63
        ja      ci_toolong

        mov     [cwd_clus],eax
        mov     ecx,[rp_cnt]
        mov     [cwd_cnt],ecx
        imul    ecx,11
        mov     esi,OFFSET rp_names
        mov     edi,OFFSET cwd_names
        rep     movsb
        popad
        clc
        ret
ci_toolong:
        popad
        mov     ax,ERR_NOPATH
        stc
        ret

;=====================================================================
; cwd_string - render the current directory as "DIR1\DIR2" (no drive,
; no leading backslash, DOS 47h style) into the buffer at EDI.
;=====================================================================
cwd_string:
        pushad
        mov     ecx,[cwd_cnt]
        mov     esi,OFFSET cwd_names
        jmp     short names_go

;=====================================================================
; names_string - the same rendering of any component list: ESI -> an
; array of 11-byte 8.3 names, ECX = how many, EDI = where the text
; goes.  21h/60h uses it on rp_names, which after resolve_core IS the
; canonical form of whatever path the caller asked about - with '.'
; and '..' already collapsed and a relative path already seeded from
; the current directory.  That is the whole of TRUENAME's work, done
; by the resolver as a side effect of its ordinary job.
;=====================================================================
names_string:
        pushad
names_go:
        or      ecx,ecx
        jz      cs_term
cs_comp:
        push    ecx
        ; name part
        xor     ecx,ecx
cs_n1:
        cmp     ecx,8
        jae     cs_ext
        mov     al,[esi+ecx]
        cmp     al,' '
        je      cs_ext
        mov     [edi],al
        inc     edi
        inc     ecx
        jmp     short cs_n1
cs_ext:
        cmp     byte ptr [esi+8],' '
        je      cs_comp_done
        mov     byte ptr [edi],'.'
        inc     edi
        mov     ecx,8
cs_e1:
        cmp     ecx,11
        jae     cs_comp_done
        mov     al,[esi+ecx]
        cmp     al,' '
        je      cs_comp_done
        mov     [edi],al
        inc     edi
        inc     ecx
        jmp     short cs_e1
cs_comp_done:
        pop     ecx
        add     esi,11
        dec     ecx
        jz      cs_term
        mov     byte ptr [edi],'\'
        inc     edi
        jmp     short cs_comp
cs_term:
        mov     byte ptr [edi],0
        popad
        ret

;=====================================================================
; Path state
;=====================================================================
rp_cnt      dd  0
rp_noname   db  0
rp_baddrv   db  0                       ; the DRIVE is what failed
            db  0,0
rp_names    db  MAX_DEPTH * 11 dup(0)

; the long-name shadow of rp_names: per component, a pointer into the
; CALLER's path string plus its length, and a flag saying the 8.3
; rendering was lossy (so only the long text can really match it)
rc_start    dd  0                       ; scratch: component text start
rp_lptr     dd  MAX_DEPTH dup(0)
rp_llen     dd  MAX_DEPTH dup(0)
rp_lflag    db  MAX_DEPTH dup(0)
            db  0,0,0
rp_fptr     dd  0                       ; the FINAL component's text...
rp_flen     dd  0                       ; ...its length...
rp_flong    db  0                       ; ...and its was-lossy flag
            db  0,0,0

; (cwd_cnt / cwd_clus / cwd_names live inside vol_state in FAT.INC -
;  each drive keeps its own current directory across vol_select.)
