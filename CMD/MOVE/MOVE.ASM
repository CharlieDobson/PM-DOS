;=====================================================================
; MOVE.ASM - MOVE.EXE, move files, and rename a file or a directory
;
;   MOVE [/Y | /-Y] source[,source...] destination
;
;   MOVE A.TXT B.TXT          rename it
;   MOVE *.TXT \DOCS          move them into a directory
;   MOVE A.TXT B.TXT \DOCS    two sources, one destination
;   MOVE C:\WORK D:\ARCHIVE   ...and what this one will NOT do; see below
;   MOVE OLDDIR NEWDIR        rename a directory
;
;   /Y    do not ask before replacing a file at the destination
;   /-Y   ask, which is the default and is what /Y turns off
;
; ONE CALL DOES THE WHOLE JOB WHEN BOTH ENDS ARE ON ONE VOLUME.  The
; kernel's 21h/56h is a rename that moves: it copies the directory
; entry into the new parent and tombstones the old one, so the file's
; clusters are never read, let alone written.  A 600MB file moves
; across a disk in the time it takes to write two directory sectors.
; THAT is what makes MOVE worth having as well as COPY, and it is why
; the cross-volume path below is the fallback and not the method.
;
; WHEN THE ENDS ARE ON DIFFERENT VOLUMES there is no such trick: the
; bytes have to travel.  56h says so precisely - ERR_NOTSAME, 11h -
; and MOVE then copies and deletes.  THE ORDER IS THE WHOLE SAFETY
; ARGUMENT: the source is deleted only after the copy has been written,
; stamped, closed and had its attributes set.  Anything that goes wrong
; before that removes the PARTIAL DESTINATION and leaves the source
; alone, because a half-moved file is worse than an unmoved one.
;
; A DIRECTORY CAN BE RENAMED AND NOT MOVED, and that is the kernel's
; rule rather than a shortcut here: a directory's ".." points at its
; parent, so handing it a new parent means rewriting the entry inside
; it, and 56h refuses (ERR_ACCESS) rather than orphan it.  Moving a
; tree from one place to another is copying, which is XCOPY's job and
; not this program's.  The message says which of the two happened.
;
; AN EXPLICIT NAME MEANS WHAT IT SAYS; A WILDCARD MEANS THE ORDINARY
; FILES.  "MOVE SUB NEWSUB" moves a directory because the user named
; one; "MOVE *.* \DOCS" does not sweep up subdirectories, hidden files
; and system files, because nobody typing a star is asking for those.
; The two rules have the same shape and are applied in one place, in
; mv_collect's attribute mask.
;
; THE NAMES ARE COLLECTED BEFORE ANYTHING IS MOVED.  A find walks a
; directory by slot index, and moving a file rewrites the directory
; underneath the walk - so enumerating and moving in one loop skips
; files, and which ones it skips depends on where the new entries
; land.  Collect, close the search, then move.  A pattern that matches
; more names than the pool holds is done in passes: everything moved
; has left the directory, so the next pass finds the next batch.
;
; LONG NAMES SURVIVE where the kernel has them.  714Eh returns a file's
; real name and 56h is LFN-transparent, so "MOVE *.TXT \DOCS" keeps
; "Quarterly Report.txt" spelt that way.  With LFN=OFF in CONFIG.SYS
; the 71h family is absent by design and MOVE falls back to 4Eh/4Fh
; and 8.3 names, which is the right answer on a machine that has
; declared it has no long names.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

; Up here, all of them: wasm cannot use a forward-referenced EQU as an
; immediate and the uses start hundreds of lines above the data.
BUF_SIZE        EQU     8000h           ; 32K, the cross-volume copy
PATH_MAX        EQU     LFN_MAX         ; 260, including the NUL
TAIL_MAX        EQU     160             ; the PSP tail is 127 and a bit
ITEMS_MAX       EQU     16              ; source items on one line
NAMES_MAX       EQU     512             ; names collected in one pass
NAMETEXT_MAX    EQU     16384           ; and the text of them

; The 318-byte record 714Eh fills in.  Win95's WIN32_FIND_DATA, which
; is what the API promises; fn714E in DOS\INT21.INC fills exactly these
; fields and zeroes the rest.
FDR_ATTR        EQU     000h            ; dd
FDR_TIME        EQU     004h            ; dd - DOS date high, time low
FDR_SIZE        EQU     020h            ; dd
FDR_LONG        EQU     02Ch            ; ASCIIZ - the 8.3 when there is
                                        ; no long name
FDR_SHORT       EQU     130h            ; ASCIIZ 8.3
FDR_LEN         EQU     13Eh

; ...and the DTA 4Eh fills in, for the machine with LFN=OFF.
DTA_ATTR        EQU     15h             ; db
DTA_TIME        EQU     16h             ; dw
DTA_DATE        EQU     18h             ; dw
DTA_SIZE        EQU     1Ah             ; dd
DTA_NAME        EQU     1Eh             ; ASCIIZ 8.3
DTA_LEN         EQU     128

_MOVE   SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MOVE, DS:_MOVE, ES:_MOVE, SS:_MOVE
        ORG     0

;=====================================================================
; D32 header - the name pool and the copy buffer live in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET mv_entry>,<NAMETEXT_MAX + (NAMES_MAX * 4) + BUF_SIZE>,4096

;=====================================================================
mv_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [name_txt],eax
        add     eax,NAMETEXT_MAX
        mov     [name_ptr],eax
        add     eax,NAMES_MAX * 4
        mov     [copy_buf],eax

        mov     edx,OFFSET dta          ; our own, before any find
        mov     eax,1A00h
        int     21h

        call    mv_parse
        jc      mv_usage

        cmp     byte ptr [opt_help],0
        jne     mv_help

;--- two items at the very least: something, and somewhere ------------
        cmp     dword ptr [nitems],2
        jb      mv_usage

        call    mv_lfnprobe
        call    mv_dsttype

; SEVERAL SOURCES NEED A DIRECTORY TO PUT THEM IN, and this is the one
; place that can be said before any damage is done.  Without it
; "MOVE A.TXT B.TXT C.TXT" would move A onto C, then B onto C, and the
; user would be left with one file where they had three.  The same
; check for several names from ONE wildcard is in mv_doitem, where the
; count is not known until the directory has been read.
        cmp     dword ptr [nitems],2
        jbe     mv_dstok
        cmp     byte ptr [dst_isdir],0
        jne     mv_dstok
        mov     edx,OFFSET msg_needdir
        call    puts
        jmp     mv_bad
mv_dstok:

;--- every source item, in the order it was written -------------------
        mov     dword ptr [item_ix],0
mv_item:
        mov     eax,[item_ix]
        inc     eax
        cmp     eax,[nitems]            ; the last item is the
        jae     mv_report               ; destination, not a source
        mov     eax,[item_ix]
        mov     esi,[items + eax*4]
        call    mv_doitem
        inc     dword ptr [item_ix]
        jmp     short mv_item

;--- how it went ------------------------------------------------------
mv_report:
        mov     eax,[moved]
        or      eax,eax
        jz      mv_nosay
        call    putdec
        mov     edx,OFFSET msg_moved
        call    puts
mv_nosay:
        cmp     byte ptr [failed],0
        jne     mv_bad
        mov     eax,4C00h
        int     21h

mv_bad:
        mov     eax,4C01h
        int     21h

mv_help:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C00h
        int     21h

mv_usage:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C01h
        int     21h

;=====================================================================
; mv_lfnprobe - is the long-name API here at all?
;
; ONE QUESTION ASKED ONCE.  71A0h is the cheapest of the family and
; answers AX = 7100h with carry when LFN=OFF in CONFIG.SYS takes the
; whole family away - which is the documented signature every LFN-aware
; program tests, and the reason it exists.  Asking per file would be
; the same answer eight hundred times.
;=====================================================================
mv_lfnprobe:
        push    eax
        push    ecx
        push    edi
        mov     byte ptr [use_lfn],0
        mov     edi,OFFSET fs_name
        mov     ecx,32
        mov     eax,71A0h
        int     21h
        jc      mvl_out
        mov     byte ptr [use_lfn],1
mvl_out:
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; mv_dsttype - is the destination a place to put files, or a name to
;              give one?
;
; THREE ANSWERS AND THEY ARE ASKED IN ORDER OF CERTAINTY.  A trailing
; backslash and a bare drive letter are the user saying "a directory"
; outright, and they are believed even when nothing is there yet.  Only
; when neither is written is the disk asked, and a name that does not
; exist is a name to create - which is what makes "MOVE A.TXT B.TXT"
; a rename rather than an error.
;=====================================================================
mv_dsttype:
        push    eax
        push    ecx
        push    edx
        push    esi
        mov     byte ptr [dst_isdir],0

        mov     esi,OFFSET dst_path
        call    mv_len                  ; ECX = its length
        jecxz   mvd_out                 ; an empty destination is caught
                                        ; by the parser, not here
        mov     al,[esi+ecx-1]
        cmp     al,'\'
        je      mvd_yes
        cmp     al,'/'
        je      mvd_yes
        cmp     ecx,2                   ; "D:" is D:'s current directory
        jne     mvd_ask
        cmp     byte ptr [esi+1],':'
        je      mvd_yes

mvd_ask:
        mov     edx,OFFSET dst_path
        mov     eax,4300h
        int     21h
        jc      mvd_out                 ; nothing there: a name to make
        test    cl,ATTR_DIR
        jz      mvd_out
mvd_yes:
        mov     byte ptr [dst_isdir],1
mvd_out:
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; mv_doitem - ESI -> one source item, which may hold wildcards.
;=====================================================================
mv_doitem:
        push    esi
        mov     [item_ptr],esi

        ; the directory half, kept for building each full source path
        call    mv_split

        ; an explicit name may be a directory, may be hidden and may be
        ; a system file, because the user named it.  A wildcard gets the
        ; ordinary files only.  (See the header: one rule, one place.)
        mov     esi,[item_ptr]
        call    mv_haswild
        mov     byte ptr [it_wild],0
        mov     byte ptr [it_mask],0
        jnc     mvi_mask
        mov     byte ptr [it_wild],1
mvi_mask:
        cmp     byte ptr [it_wild],0
        jne     mvi_first
        mov     byte ptr [it_mask],ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR

mvi_first:
        mov     byte ptr [it_any],0

mvi_pass:
        call    mv_collect              ; -> [n_names], [trunc]
        cmp     dword ptr [n_names],0
        jne     mvi_have

        ; nothing.  On the FIRST pass that is "no such file"; on a
        ; later one it is simply the end, because the pass before it
        ; moved the last of them out of the directory.
        cmp     byte ptr [it_any],0
        jne     mvi_out
        mov     edx,OFFSET msg_cannot
        call    puts
        mov     edx,[item_ptr]
        call    putz
        mov     edx,OFFSET msg_colon
        call    puts
        mov     edx,OFFSET msg_e2
        call    puts
        call    crlf
        mov     byte ptr [failed],1
        jmp     short mvi_out

mvi_have:
        mov     byte ptr [it_any],1

        ; ONE DESTINATION NAME CANNOT TAKE TWO FILES.  The count is
        ; only knowable here, after the directory has been read, which
        ; is why this check cannot live beside its twin in mv_entry.
        cmp     byte ptr [dst_isdir],0
        jne     mvi_go
        cmp     dword ptr [n_names],1
        jbe     mvi_go
        mov     edx,OFFSET msg_needdir
        call    puts
        mov     byte ptr [failed],1
        jmp     short mvi_out

mvi_go:
        mov     dword ptr [pass_moved],0
        mov     dword ptr [name_ix],0
mvi_one:
        mov     eax,[name_ix]
        cmp     eax,[n_names]
        jae     mvi_passend
        mov     edx,[name_ptr]
        mov     esi,[edx + eax*4]
        call    mv_one
        inc     dword ptr [name_ix]
        jmp     short mvi_one

mvi_passend:
        ; another pass only when the pool filled up AND this one made
        ; progress.  Without the second half, a directory where every
        ; move fails would be enumerated for ever.
        cmp     byte ptr [trunc],0
        je      mvi_out
        cmp     dword ptr [pass_moved],0
        je      mvi_out
        jmp     mvi_pass

mvi_out:
        pop     esi
        ret

;=====================================================================
; mv_collect - read the directory once and write every matching name
;              into the pool.  [n_names] out, [trunc] set if the pool
;              filled before the directory ran out.
;
; The search is CLOSED before this returns.  Nothing is moved while a
; find is open - see the header - and the LFN find is handle-based with
; only four handles in the kernel, so leaving one open is a resource
; leak as well as a correctness problem.
;=====================================================================
mv_collect:
        pushad
        mov     dword ptr [n_names],0
        mov     byte ptr [trunc],0
        mov     eax,[name_txt]
        mov     [txt_next],eax

        cmp     byte ptr [use_lfn],0
        je      mvc_short

;--- the long-name find ----------------------------------------------
        mov     edx,[item_ptr]
        mov     edi,OFFSET find_rec
        movzx   ecx,byte ptr [it_mask]  ; CL allowable, CH required
        mov     eax,714Eh
        int     21h
        jc      mvc_done                ; no match at all
        movzx   ebx,ax                  ; the find handle
mvc_llp:
        mov     esi,OFFSET find_rec + FDR_LONG
        mov     eax,dword ptr [find_rec + FDR_ATTR]
        call    mv_take
        jc      mvc_lclose              ; the pool is full
        push    ebx
        mov     edi,OFFSET find_rec
        mov     eax,714Fh
        int     21h
        pop     ebx
        jnc     mvc_llp
mvc_lclose:
        mov     eax,71A1h
        int     21h
        jmp     short mvc_done

;--- and the 8.3 one, for a machine that has said it has no others ---
mvc_short:
        mov     edx,[item_ptr]
        movzx   ecx,byte ptr [it_mask]
        mov     eax,4E00h
        int     21h
        jc      mvc_done
mvc_slp:
        mov     esi,OFFSET dta + DTA_NAME
        movzx   eax,byte ptr [dta + DTA_ATTR]
        call    mv_take
        jc      mvc_done                ; 4Eh has no handle to close -
                                        ; its state is the DTA
        mov     eax,4F00h
        int     21h
        jnc     mvc_slp

mvc_done:
        popad
        ret

;---------------------------------------------------------------------
; mv_take - ESI -> a name, EAX = its attributes.  Add it to the pool.
;           CF set when the pool is full, which is not an error: it is
;           the signal to stop reading and start moving.
;---------------------------------------------------------------------
mv_take:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        test    al,ATTR_VOLUME          ; a label is not a file
        jnz     mvt_skip

        ; "." and ".." are the directory itself and its parent, and a
        ; find returns both.  Moving either is not a thing that can be
        ; meant, and 56h would answer something unhelpful about it.
        cmp     byte ptr [esi],'.'
        jne     mvt_keep
        cmp     byte ptr [esi+1],0
        je      mvt_skip
        cmp     byte ptr [esi+1],'.'
        jne     mvt_keep
        cmp     byte ptr [esi+2],0
        je      mvt_skip

mvt_keep:
        mov     eax,[n_names]
        cmp     eax,NAMES_MAX
        jae     mvt_full
        call    mv_len                  ; ECX = length, ESI unchanged
        mov     edi,[txt_next]
        mov     edx,[name_txt]
        add     edx,NAMETEXT_MAX
        sub     edx,edi                 ; EDX = room left in the text
        cmp     ecx,edx
        jae     mvt_full                ; ">=": the NUL needs a byte too

        mov     edx,[name_ptr]          ; remember where it goes
        mov     [edx + eax*4],edi
        inc     dword ptr [n_names]
        inc     ecx                     ; copy the NUL as well
        rep     movsb
        mov     [txt_next],edi
mvt_skip:
        clc
        jmp     short mvt_out
mvt_full:
        mov     byte ptr [trunc],1
        stc
mvt_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; mv_one - ESI -> one bare name found in the source directory.  Build
;          both paths, make sure the move is a thing that can be done,
;          and do it.
;=====================================================================
mv_one:
        pushad
        mov     [one_name],esi

        ; source = the directory half of what was typed, plus the name
        ; the directory actually holds.  NOT what was typed: the typed
        ; half may be a pattern, and on an LFN volume the name found is
        ; the file's real one.
        mov     esi,OFFSET src_dir
        mov     edx,[one_name]
        mov     edi,OFFSET src_path
        call    mv_join
        jc      mvo_toolong

        ; destination: into the directory under its own name, or onto
        ; the single name the user gave
        cmp     byte ptr [dst_isdir],0
        je      mvo_asname
        mov     esi,OFFSET dst_path
        mov     edx,[one_name]
        mov     edi,OFFSET tgt_path
        call    mv_join
        jc      mvo_toolong
        jmp     short mvo_have
mvo_asname:
        mov     esi,OFFSET dst_path
        mov     edi,OFFSET tgt_path
        call    mv_copyz
mvo_have:

        ; A FILE CANNOT BE MOVED ONTO ITSELF, and the only honest way
        ; to ask is to have the kernel resolve both.  "A.TXT" and
        ; ".\A.TXT" and "C:\WORK\A.TXT" are one file spelt three ways,
        ; and comparing what was typed would say they are three.  60h
        ; is exactly this question and exists for callers like this one.
        ; If it cannot answer for either side the test is skipped
        ; rather than guessed: the move below will fail honestly.
        mov     esi,OFFSET src_path
        mov     edi,OFFSET true_src
        mov     eax,6000h
        int     21h
        jc      mvo_notself
        mov     esi,OFFSET tgt_path
        mov     edi,OFFSET true_tgt
        mov     eax,6000h
        int     21h
        jc      mvo_notself
        mov     esi,OFFSET true_src
        mov     edi,OFFSET true_tgt
        call    mv_streq
        jnz     mvo_notself
        mov     edx,OFFSET msg_self
        call    puts
        mov     byte ptr [failed],1
        jmp     mvo_out
mvo_notself:

        ; is there already something at the destination?
        mov     edx,OFFSET tgt_path
        mov     eax,4300h
        int     21h
        jc      mvo_move                ; nothing there: straight on
        mov     [tgt_attr],cx

        test    cl,ATTR_DIR             ; a directory is not something
        jz      mvo_exists              ; to be replaced by a file, and
        mov     edx,OFFSET msg_cannot   ; deleting it to make room is
        call    putsrc                  ; not a decision MOVE gets to
        mov     edx,OFFSET msg_edir     ; make on anybody's behalf
        call    puts
        call    crlf
        mov     byte ptr [failed],1
        jmp     mvo_out

mvo_exists:
        call    mv_ask                  ; CF set = leave it alone
        jnc     mvo_clear
        jmp     mvo_out
mvo_clear:
        ; A READ-ONLY FILE IS NOT DELETED BEHIND THE USER'S BACK: the
        ; prompt above has already been answered, so the attribute is
        ; cleared to carry out an instruction that has been given, not
        ; to get around one.  With /Y the instruction was given on the
        ; command line.
        mov     cx,[tgt_attr]
        and     cl,NOT ATTR_READONLY
        movzx   ecx,cx
        mov     edx,OFFSET tgt_path
        mov     eax,4301h
        int     21h
        mov     edx,OFFSET tgt_path
        mov     eax,4100h
        int     21h
        jnc     mvo_move
        call    mvo_why                 ; could not clear the way
        jmp     mvo_out

;--- the move itself --------------------------------------------------
mvo_move:
        mov     edx,OFFSET src_path
        mov     edi,OFFSET tgt_path
        mov     eax,5600h
        int     21h
        jnc     mvo_done

        movzx   eax,ax
        cmp     eax,ERR_NOTSAME         ; different volumes: the bytes
        je      mvo_across              ; have to travel after all

        ; A DIRECTORY REFUSED IS WORTH ITS OWN SENTENCE.  "Access
        ; denied" about a directory sends the reader looking for a
        ; permission that does not exist here; what actually happened
        ; is that its ".." cannot follow it.
        cmp     eax,ERR_ACCESS
        jne     mvo_err
        mov     edx,OFFSET src_path
        mov     eax,4300h
        int     21h
        jc      mvo_err2
        test    cl,ATTR_DIR
        jz      mvo_err2
        mov     edx,OFFSET msg_cannot
        call    putsrc
        mov     edx,OFFSET msg_emoved
        call    puts
        call    crlf
        mov     byte ptr [failed],1
        jmp     short mvo_out
mvo_err2:
        mov     eax,ERR_ACCESS
mvo_err:
        call    mvo_why
        jmp     short mvo_out

mvo_across:
        call    mv_across
        jc      mvo_out
mvo_done:
        mov     edx,OFFSET src_path
        call    putz
        mov     edx,OFFSET msg_arrow
        call    puts
        mov     edx,OFFSET tgt_path
        call    putz
        call    crlf
        inc     dword ptr [moved]
        inc     dword ptr [pass_moved]
        jmp     short mvo_out

mvo_toolong:
        mov     edx,OFFSET msg_cannot
        call    putsrc
        mov     edx,OFFSET msg_elong
        call    puts
        call    crlf
        mov     byte ptr [failed],1
mvo_out:
        popad
        ret

;---------------------------------------------------------------------
; mvo_why - EAX = a DOS error.  "Cannot move <source>: <reason>".
;---------------------------------------------------------------------
mvo_why:
        push    edx
        push    eax
        mov     edx,OFFSET msg_cannot
        call    putsrc
        pop     eax
        call    mv_why
        call    crlf
        mov     byte ptr [failed],1
        pop     edx
        ret

;---------------------------------------------------------------------
; putsrc - EDX -> a lead-in; then the source path and a colon.
;---------------------------------------------------------------------
putsrc:
        call    puts
        mov     edx,OFFSET src_path
        call    putz
        mov     edx,OFFSET msg_colon
        call    puts
        ret

;=====================================================================
; mv_across - the same move when the two ends are on different
;             volumes: copy, stamp, close, then delete the source.
;             CF set if it did not happen.
;
; EVERY FAILURE PATH REMOVES THE PARTIAL DESTINATION.  A file that
; exists at the new place with half its bytes in it is worse than one
; that never arrived, because the next thing to read it will not know.
; The source is untouched until the copy is complete, so the answer to
; any failure here is "nothing moved", which is a state the user can
; act on.
;=====================================================================
mv_across:
        pushad
        mov     dword ptr [src_h],-1
        mov     dword ptr [tgt_h],-1

        ; a directory's bytes are its directory entries, and copying
        ; those would make a second directory pointing at the first
        ; one's clusters.  A tree that has to cross a volume is a
        ; copy of every file in it, which is XCOPY and not this.
        mov     edx,OFFSET src_path
        mov     eax,4300h
        int     21h
        jc      mva_openerr
        mov     [src_attr],cx
        test    cl,ATTR_DIR
        jz      mva_open
        mov     edx,OFFSET msg_cannot
        call    putsrc
        mov     edx,OFFSET msg_edirvol
        call    puts
        call    crlf
        mov     byte ptr [failed],1
        jmp     mva_fail

mva_open:
        mov     edx,OFFSET src_path
        mov     eax,3D00h               ; read
        int     21h
        jc      mva_openerr
        movzx   eax,ax
        mov     [src_h],eax

        ; the stamp is read from the OPEN source, before anything is
        ; written, because a copy that carries the original's date is
        ; the whole difference between a move and a new file
        mov     ebx,[src_h]
        mov     eax,5700h
        int     21h
        jc      mva_mkerr
        mov     [src_time],cx
        mov     [src_date],dx

        ; created with no attributes at all: a read-only file cannot be
        ; written to, so the real ones go on at the end
        xor     ecx,ecx
        mov     edx,OFFSET tgt_path
        mov     eax,3C00h
        int     21h
        jc      mva_mkerr
        movzx   eax,ax
        mov     [tgt_h],eax

mva_lp:
        mov     ebx,[src_h]
        mov     ecx,BUF_SIZE
        mov     edx,[copy_buf]
        mov     eax,3F00h
        int     21h
        jc      mva_ioerr
IFDEF MV_DEBUG
        call    mvd_read                ; what came back, and what is
ENDIF                                   ; actually in the buffer
        or      eax,eax
        jz      mva_end                 ; end of the file
        mov     ecx,eax
        mov     ebx,[tgt_h]
        mov     edx,[copy_buf]
        push    ecx
        mov     eax,4000h
        int     21h
        pop     ecx
        jc      mva_ioerr
        cmp     eax,ecx                 ; a short write IS the disk
        jb      mva_full                ; filling up; 40h says so by
        jmp     short mva_lp            ; writing less, not by failing

mva_end:
        mov     ebx,[tgt_h]             ; the stamp, while it is open
        mov     cx,[src_time]
        mov     dx,[src_date]
        mov     eax,5701h
        int     21h

        mov     ebx,[tgt_h]
        mov     eax,3E00h
        int     21h
        mov     dword ptr [tgt_h],-1
        jc      mva_ioerr
        mov     ebx,[src_h]
        mov     eax,3E00h
        int     21h
        mov     dword ptr [src_h],-1

        movzx   ecx,word ptr [src_attr] ; now the attributes can go on
        and     cl,NOT ATTR_DIR
        mov     edx,OFFSET tgt_path
        mov     eax,4301h
        int     21h

        ; and only now the source.  Its read-only bit is cleared first
        ; for the same reason as the destination's: the instruction has
        ; been given and this is carrying it out.
        movzx   ecx,word ptr [src_attr]
        and     cl,NOT (ATTR_READONLY OR ATTR_DIR)
        mov     edx,OFFSET src_path
        mov     eax,4301h
        int     21h
        mov     edx,OFFSET src_path
        call    del_perm                ; not kept: it is safe at the
        jc      mva_delerr              ; other end

        popad
        clc
        ret

;---------------------------------------------------------------------
; del_perm - delete the file at EDX and do not let the kernel keep it
;            (UNDELETE=ON).  What MOVE deletes is either at the other
;            end already or was never whole, so a kept copy would only
;            take up space.  A kernel with no F1h AL=19h says "invalid
;            function" and gets a plain 41h.  CF with AX as 41h gives.
;---------------------------------------------------------------------
del_perm:
        push    edx
        mov     eax,0F119h
        int     21h
        pop     edx
        jnc     dpm_out
        cmp     ax,ERR_FUNC
        stc
        jne     dpm_out
        mov     eax,4100h
        int     21h
dpm_out:
        ret

;--- everything below leaves the source where it was -----------------
mva_full:
        mov     eax,ERR_DISKFULL
        jmp     short mva_say
mva_ioerr:
        mov     eax,ERR_GENERAL
        jmp     short mva_say
mva_mkerr:
        movzx   eax,ax
        jmp     short mva_say
mva_openerr:
        movzx   eax,ax
mva_say:
        call    mvo_why
mva_fail:
        mov     ebx,[tgt_h]             ; close what is open...
        cmp     ebx,-1
        je      mva_f2
        mov     eax,3E00h
        int     21h
        mov     edx,OFFSET tgt_path     ; ...and take the half-written
        call    del_perm                ; destination away with it
mva_f2:
        mov     ebx,[src_h]
        cmp     ebx,-1
        je      mva_f3
        mov     eax,3E00h
        int     21h
mva_f3:
        popad
        stc
        ret

; The copy worked and the source would not go.  THE FILE IS AT BOTH
; ENDS NOW and saying so is the only useful thing left to do - the
; destination is good, so removing it would throw away the work, and
; keeping quiet would leave a duplicate nobody knows about.
mva_delerr:
        movzx   eax,ax
        push    eax
        mov     edx,OFFSET msg_copied
        call    putsrc
        pop     eax
        call    mv_why
        call    crlf
        mov     byte ptr [failed],1
        popad
        stc
        ret

;=====================================================================
; mv_ask - something is already at the destination.  CF set = leave it
;          alone.
;
; THE PROMPT IS THE DEFAULT AND /Y IS THE OVERRIDE, which is the other
; way round from COPY in this shell - COPY overwrites without asking
; and always has.  The asymmetry is deliberate and is about what is
; lost: a COPY that overwrites destroys one file, and a MOVE that
; overwrites destroys one file AND takes away the only other copy of
; the one that replaced it.
;
; The key is read with 0Ch/01h - flush, then read - so that a keystroke
; typed before the question was asked cannot answer it.
;=====================================================================
mv_ask:
        push    eax
        push    edx
        cmp     byte ptr [opt_yes],0
        jne     mvk_yes
        cmp     byte ptr [all_yes],0
        jne     mvk_yes

        mov     edx,OFFSET msg_over
        call    puts
        mov     edx,OFFSET tgt_path
        call    putz
        mov     edx,OFFSET msg_over2
        call    puts
mvk_key:
        mov     eax,0C01h
        int     21h
        cmp     al,'a'
        jb      mvk_kup
        cmp     al,'z'
        ja      mvk_kup
        sub     al,20h
mvk_kup:
        cmp     al,'Y'
        je      mvk_ky
        cmp     al,'N'
        je      mvk_kn
        cmp     al,'A'
        je      mvk_ka
        cmp     al,27                   ; Escape is No, as it is
        je      mvk_kn                  ; everywhere else
        jmp     short mvk_key
mvk_ka:
        mov     byte ptr [all_yes],1
mvk_ky:
        call    putc                    ; echo the answer, then a line
        call    crlf
        jmp     short mvk_yes
mvk_kn:
        call    putc
        call    crlf
        pop     edx
        pop     eax
        stc
        ret
mvk_yes:
        pop     edx
        pop     eax
        clc
        ret

;=====================================================================
; The command line.
;
; Switches anywhere, items in order, the last item the destination.
; Commas and blanks both separate, because "MOVE A.TXT,B.TXT \DOCS" is
; how MOVE has always been written and a comma is a DOS delimiter
; anyway.  A quoted item keeps its blanks, which is what makes a long
; name typeable.
;=====================================================================
mv_parse:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi

        ; the tail, copied out so it can be cut into NUL-terminated
        ; pieces - the PSP is not ours to rewrite
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        cmp     ecx,TAIL_MAX - 1
        jbe     mp_len
        mov     ecx,TAIL_MAX - 1
mp_len:
        mov     edi,OFFSET line
        jecxz   mp_lend
mp_lcp:
        mov     al,[esi]
        cmp     al,0Dh
        je      mp_lend
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     mp_lcp
mp_lend:
        mov     byte ptr [edi],0

        mov     esi,OFFSET line
        mov     dword ptr [nitems],0

mp_tok:
        mov     al,[esi]
        or      al,al
        jz      mp_done
        cmp     al,' '
        je      mp_skip
        cmp     al,9
        je      mp_skip
        cmp     al,','
        je      mp_skip
        cmp     al,'/'
        je      mp_sw
        jmp     short mp_item
mp_skip:
        inc     esi
        jmp     short mp_tok

;--- /Y, /-Y and /? ---------------------------------------------------
mp_sw:
        inc     esi
        mov     ah,0                    ; AH = a '-' came between
        mov     al,[esi]
        cmp     al,'-'                  ; "/-Y" - the default, said out
        jne     mp_swc                  ; loud, which is how a COPYCMD
        mov     ah,1                    ; habit is written down
        inc     esi
        mov     al,[esi]
mp_swc:
        or      al,al
        jz      mp_bad
        inc     esi
        cmp     al,'a'
        jb      mp_swu
        cmp     al,'z'
        ja      mp_swu
        sub     al,20h
mp_swu:
        cmp     al,'Y'
        jne     mp_sw2
        ; WHICH SPELLING WON IS DECIDED BY WHICH CAME LAST, because a
        ; line that says both means the second one.  "/-Y" clears it.
        ; The flag is carried in AH rather than read back out of the
        ; line: "/Y" and "/-Y" end at the same place and differ two
        ; characters back, and counting backwards to find that out is
        ; how an off-by-one turns /-Y into /Y silently.
        mov     byte ptr [opt_yes],1
        or      ah,ah
        jz      mp_tok
        mov     byte ptr [opt_yes],0
        jmp     short mp_tok
mp_sw2:
        cmp     al,'?'
        jne     mp_bad
        mov     byte ptr [opt_help],1
        jmp     short mp_tok

;--- an item, quoted or not -------------------------------------------
mp_item:
        mov     eax,[nitems]
        cmp     eax,ITEMS_MAX
        jae     mp_bad
        xor     ah,ah                   ; AH = inside quotes
        cmp     byte ptr [esi],'"'
        jne     mp_ihave
        mov     ah,1
        inc     esi
mp_ihave:
        mov     [items + eax*4],esi     ; it starts where it starts: the
        inc     dword ptr [nitems]      ; terminator is written in place
mp_ich:
        mov     al,[esi]
        or      al,al
        jz      mp_iend
        cmp     al,'"'
        je      mp_iquote
        or      ah,ah
        jnz     mp_istep                ; quoted: blanks belong to it
        cmp     al,' '
        je      mp_icut
        cmp     al,9
        je      mp_icut
        cmp     al,','
        je      mp_icut
mp_istep:
        inc     esi
        jmp     short mp_ich
mp_iquote:
        mov     byte ptr [esi],0        ; the closing quote ends it
        inc     esi
        jmp     short mp_tok
mp_icut:
        mov     byte ptr [esi],0
        inc     esi
        jmp     mp_tok
mp_iend:
        jmp     short mp_done

mp_done:
        ; the last item is where things are going
        mov     eax,[nitems]
        cmp     eax,2
        jb      mp_ok                   ; mv_entry says what is missing
        dec     eax
        mov     esi,[items + eax*4]
        mov     edi,OFFSET dst_path
        call    mv_copyz
mp_ok:
        clc
        jmp     short mp_out
mp_bad:
        stc
mp_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; Path and string helpers.
;=====================================================================

;--- mv_split - [item_ptr] -> "DIR\SUB\*.TXT"; src_dir <- "DIR\SUB\".
; The separator is KEPT, so mv_join adds nothing and a root-relative
; "\*.TXT" stays rooted.  A drive letter counts as a separator: "D:*.C"
; means D:'s current directory and "D:\*.C" means its root, and the two
; must not be allowed to turn into each other.
mv_split:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     esi,[item_ptr]
        xor     edx,edx                 ; EDX = characters before the name
        xor     ecx,ecx
ms_lp:
        mov     al,[esi+ecx]
        or      al,al
        jz      ms_end
        inc     ecx
        cmp     al,'\'
        je      ms_mark
        cmp     al,'/'
        je      ms_mark
        cmp     al,':'
        jne     ms_lp
ms_mark:
        mov     edx,ecx
        jmp     short ms_lp
ms_end:
        mov     edi,OFFSET src_dir
        mov     ecx,edx
        jecxz   ms_term
        rep     movsb
ms_term:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- mv_join - ESI -> a directory, EDX -> a name, EDI -> the answer.
; CF set if it will not fit in PATH_MAX.
mv_join:
        push    eax
        push    ecx
        push    esi
        push    edi
        xor     ecx,ecx                 ; ECX = what has been written
mj_dir:
        mov     al,[esi]
        or      al,al
        jz      mj_sep
        cmp     ecx,PATH_MAX - 2
        jae     mj_full
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        jmp     short mj_dir
mj_sep:
        jecxz   mj_name                 ; no directory at all
        mov     al,[edi-1]              ; one separator, never two
        cmp     al,'\'
        je      mj_name
        cmp     al,'/'
        je      mj_name
        cmp     al,':'
        je      mj_name
        cmp     ecx,PATH_MAX - 2
        jae     mj_full
        mov     byte ptr [edi],'\'
        inc     edi
        inc     ecx
mj_name:
        mov     esi,edx
mj_ncp:
        mov     al,[esi]
        or      al,al
        jz      mj_end
        cmp     ecx,PATH_MAX - 2
        jae     mj_full
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        jmp     short mj_ncp
mj_end:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret
mj_full:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;--- mv_copyz - ESI -> ASCIIZ, EDI -> a PATH_MAX buffer
mv_copyz:
        push    eax
        push    ecx
        push    esi
        push    edi
        xor     ecx,ecx
mz_lp:
        mov     al,[esi]
        or      al,al
        jz      mz_end
        cmp     ecx,PATH_MAX - 1
        jae     mz_end
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        jmp     short mz_lp
mz_end:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;--- mv_len - ECX = the length of the ASCIIZ at ESI, ESI kept
mv_len:
        push    esi
        xor     ecx,ecx
ml_lp:
        cmp     byte ptr [esi],0
        je      ml_out
        inc     esi
        inc     ecx
        jmp     short ml_lp
ml_out:
        pop     esi
        ret

;--- mv_haswild - CF set if the ASCIIZ at ESI holds a * or a ?
mv_haswild:
        push    eax
        push    esi
mw_lp:
        mov     al,[esi]
        or      al,al
        jz      mw_no
        cmp     al,'*'
        je      mw_yes
        cmp     al,'?'
        je      mw_yes
        inc     esi
        jmp     short mw_lp
mw_no:
        pop     esi
        pop     eax
        clc
        ret
mw_yes:
        pop     esi
        pop     eax
        stc
        ret

;--- mv_streq - ZF set if the strings at ESI and EDI are the same
; name.  CASE-INSENSITIVE, because a FAT volume's names are: "A.TXT"
; and "a.txt" are one file, and a comparison that says otherwise would
; let MOVE delete a file in order to move it onto itself.
mv_streq:
        push    eax
        push    ecx
        push    esi
        push    edi
me_lp:
        mov     al,[esi]
        mov     cl,[edi]
        cmp     al,'a'
        jb      me_a
        cmp     al,'z'
        ja      me_a
        sub     al,20h
me_a:
        cmp     cl,'a'
        jb      me_b
        cmp     cl,'z'
        ja      me_b
        sub     cl,20h
me_b:
        cmp     al,cl
        jne     me_out                  ; ZF clear: different
        or      al,al
        jz      me_out                  ; ZF set at a shared NUL
        inc     esi
        inc     edi
        jmp     short me_lp
me_out:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; mv_why - EAX = a DOS error; say what it means HERE.  The generic
;          words are not wrong, they are just not useful: "access
;          denied" is true of a read-only file, of a file somebody has
;          open and of a directory that is not empty, and the reader
;          has to be told which of those this program just met.
;=====================================================================
mv_why:
        push    edx
        cmp     eax,ERR_NOFILE
        jne     mvw_3
        mov     edx,OFFSET msg_e2
        jmp     short mvw_say
mvw_3:
        cmp     eax,ERR_NOPATH
        jne     mvw_5
        mov     edx,OFFSET msg_e3
        jmp     short mvw_say
mvw_5:
        cmp     eax,ERR_ACCESS
        jne     mvw_8
        mov     edx,OFFSET msg_e5
        jmp     short mvw_say
mvw_8:
        cmp     eax,ERR_NOMEM
        jne     mvw_15
        mov     edx,OFFSET msg_e8
        jmp     short mvw_say
mvw_15:
        cmp     eax,ERR_BADDRIVE
        jne     mvw_70
        mov     edx,OFFSET msg_e15
        jmp     short mvw_say
mvw_70:
        cmp     eax,ERR_DISKFULL
        jne     mvw_80
        mov     edx,OFFSET msg_e70
        jmp     short mvw_say
mvw_80:
        cmp     eax,ERR_EXISTS
        jne     mvw_1f
        mov     edx,OFFSET msg_e80
        jmp     short mvw_say
mvw_1f:
        cmp     eax,ERR_GENERAL
        jne     mvw_num
        mov     edx,OFFSET msg_e31
        jmp     short mvw_say
mvw_num:
        push    eax
        mov     edx,OFFSET msg_enum
        call    puts
        pop     eax
        call    putdec
        pop     edx
        ret
mvw_say:
        call    puts
        pop     edx
        ret

;=====================================================================
; Output.  Through 21h/02h and 21h/09h, like any program.
;=====================================================================
puts:
        push    eax
        mov     eax,0900h
        int     21h
        pop     eax
        ret

;--- putz - the same for ASCIIZ.  A PATH MAY CONTAIN A '$' and 21h/09h
;--- would stop there, which is exactly the bug that cut PROMPT's help
;--- screen in half in COMMAND.COM.  Every path this program prints
;--- goes through here.
putz:
        push    eax
        push    esi
        mov     esi,edx
pz_lp:
        mov     al,[esi]
        or      al,al
        jz      pz_out
        call    putc
        inc     esi
        jmp     short pz_lp
pz_out:
        pop     esi
        pop     eax
        ret

putc:
        push    eax
        push    edx
        movzx   edx,al
        mov     eax,0200h
        int     21h
        pop     edx
        pop     eax
        ret

crlf:
        push    eax
        mov     al,13
        call    putc
        mov     al,10
        call    putc
        pop     eax
        ret

;--- putdec - EAX in decimal, no padding
putdec:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     edi,OFFSET num_buf + 15
        mov     byte ptr [edi],'$'
        xor     ecx,ecx
pd_div:
        xor     edx,edx
        mov     ebx,10
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        inc     ecx
        or      eax,eax
        jnz     pd_div
        mov     edx,edi
        call    puts
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

IFDEF MV_DEBUG
;=====================================================================
; BUILD move - what the cross-volume copy actually got, as opposed to
; what it asked for.  THE PROGRAM PRINTS WHAT IT RECEIVED, which is
; the only kind of evidence that settles a pointer question: MOUNT's
; "no such file" was diagnosed this way and no other way worked.
;=====================================================================
mvd_read:
        pushad
        mov     edx,OFFSET mvd_m1
        call    puts
        call    putdec                  ; the count 3Fh returned
        mov     edx,OFFSET mvd_m2
        call    puts
        mov     eax,[copy_buf]          ; where we told it to put them
        call    mvd_hex
        mov     edx,OFFSET mvd_m3
        call    puts
        mov     esi,[copy_buf]          ; and what is there now
        mov     ecx,8
mvd_rl:
        movzx   eax,byte ptr [esi]
        call    mvd_hex2
        mov     al,' '
        call    putc
        inc     esi
        dec     ecx
        jnz     mvd_rl
        call    crlf
        popad
        ret

;--- EAX in hex, eight nibbles or two
mvd_hex:
        push    ecx
        mov     ecx,32
        jmp     short mvd_hgo
mvd_hex2:
        push    ecx
        mov     ecx,8
mvd_hgo:
        push    eax
        push    ebx
        mov     ebx,eax
mvd_hl:
        sub     ecx,4
        mov     eax,ebx
        shr     eax,cl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     mvd_hp
        add     al,7
mvd_hp:
        call    putc
        or      ecx,ecx
        jnz     mvd_hl
        pop     ebx
        pop     eax
        pop     ecx
        ret

mvd_m1      db  '[read=$'
mvd_m2      db  ' buf=$'
mvd_m3      db  ' bytes=$'
ENDIF

;=====================================================================
; State
;=====================================================================
opt_yes     db  0                       ; /Y
opt_help    db  0
use_lfn     db  0
dst_isdir   db  0
all_yes     db  0                       ; "All" was answered to a prompt
failed      db  0                       ; the exit code
trunc       db  0                       ; the name pool filled up
it_wild     db  0
it_mask     db  0
it_any      db  0                       ; this item matched something
            db  0                       ; pad to a dword

nitems      dd  0
item_ix     dd  0
item_ptr    dd  0
one_name    dd  0
n_names     dd  0
name_ix     dd  0
moved       dd  0
pass_moved  dd  0
src_h       dd  0
tgt_h       dd  0
name_txt    dd  0                       ; the BSS, carved up at entry
name_ptr    dd  0
txt_next    dd  0
copy_buf    dd  0
items       dd  ITEMS_MAX dup(0)

src_attr    dw  0
tgt_attr    dw  0
src_time    dw  0
src_date    dw  0

line        db  TAIL_MAX dup(0)
src_dir     db  PATH_MAX dup(0)
src_path    db  PATH_MAX dup(0)
tgt_path    db  PATH_MAX dup(0)
dst_path    db  PATH_MAX dup(0)
true_src    db  144 dup(0)
true_tgt    db  144 dup(0)
fs_name     db  32 dup(0)
num_buf     db  16 dup(0)
dta         db  DTA_LEN dup(0)
find_rec    db  FDR_LEN dup(0)

;=====================================================================
; Messages
;=====================================================================
msg_arrow   db  ' => $'
msg_colon   db  ': $'
msg_moved   db  ' file(s) moved',13,10,'$'
msg_cannot  db  'Cannot move $'
msg_copied  db  'Copied, but could not remove $'
msg_self    db  'File cannot be moved onto itself',13,10,'$'
msg_needdir db  'The destination has to be a directory when more than '
            db  'one file is moved',13,10,'$'
msg_over    db  'Replace $'
msg_over2   db  ' (Yes/No/All)? $'

msg_e2      db  'no such file$'
msg_e3      db  'no such directory$'
msg_e5      db  'access denied - it is read-only, open, or a directory '
            db  'that is not empty$'
msg_e8      db  'not enough memory$'
msg_e15     db  'no such drive$'
msg_e31     db  'general failure$'
msg_e70     db  'the disk is full$'
msg_e80     db  'something of that name is already there$'
msg_edir    db  'there is a directory of that name at the destination$'
msg_edirvol db  'a directory cannot be moved to another drive - XCOPY '
            db  'copies a tree$'
msg_emoved  db  'a directory can be RENAMED but not moved to another '
            db  'directory,',13,10,'       because its ".." would be '
            db  'left pointing at the old parent$'
msg_elong   db  'the resulting path would be too long$'
msg_enum    db  'error $'

msg_usage   db  'Moves files and renames files and directories.',13,10,13,10
            db  'To move one or more files:',13,10
            db  'MOVE [/Y | /-Y] [drive:][path]filename1[,...] destination',13,10,13,10
            db  'To rename a directory:',13,10
            db  'MOVE [/Y | /-Y] [drive:][path]dirname1 dirname2',13,10,13,10
            db  '  [drive:][path]filename1 Specifies the location and name of the file',13,10
            db  '                          or files you want to move.',13,10
            db  '  destination             Specifies the new location of the file. Destination',13,10
            db  '                          can consist of a drive letter and colon, a directory',13,10
            db  '                          name, or a combination. If you are moving only one',13,10
            db  '                          file, you can also include a filename if you want',13,10
            db  '                          to rename the file when you move it.',13,10
            db  '  [drive:][path]dirname1  Specifies the directory you want to rename.',13,10
            db  '  dirname2                Specifies the new name of the directory.',13,10,13,10
            db  '  /Y                      Suppresses prompting to confirm creation of a directory',13,10
            db  '                          or overwriting of the destination.',13,10
            db  '  /-Y                     Causes prompting to confirm creation of a directory',13,10
            db  '                          or overwriting of the destination.',13,10,13,10
            db  'The switch /Y may be present in the COPYCMD environment variable.',13,10
            db  'This may be overridden with /-Y on the command line.',13,10,'$'

img_end:                                ; the BSS begins here

_MOVE   ENDS
        END
