;=====================================================================
; UNDELETE.ASM - UNDELETE.EXE, gets deleted files back
;
;   UNDELETE [d:][path][name] [/LIST | /ALL] [/S]
;   UNDELETE [d:] /PURGE
;   UNDELETE [d:] /STATUS
;
; With UNDELETE=ON in CONFIG.SYS (the default) the kernel does not free
; a deleted file.  It moves the directory entry into d:\RECYCLED,
; hidden and system, and writes a record into d:\RECYCLED\UNDELETE.DAT
; saying where the file was and what it was called - see DELETE
; PROTECTION in PMDOS.INC for the format and DOS\RECYCLE.INC for the
; kernel's half.  This is the other half: it reads the records, and
; recovering a file is a rename back to where it came from.
;
; WHICH FILES.  The path names the directory the files were deleted
; from and the name part picks among them, wildcards and all; it is
; matched against the 8.3 name and, when there was one, the long name.
; The default is every file deleted from the current directory, which
; is what "I just deleted that" usually means.  /S takes in the
; directories below it too.
;
; NOTHING IS OVERWRITTEN.  A file whose name has been taken since it
; was deleted comes back with '#' (or the next free character) in place
; of its first letter, and a directory that has gone since is made
; again.
;
; /PURGE empties d:\RECYCLED for good and removes it; the kernel makes
; it again on the next delete.  Deleting anything INSIDE \RECYCLED is
; never kept, which is how this can do it with plain 41h calls.
;
; What it asks:
;   21h/F1h AL=18h   whether deleted files are being kept
;   21h/38h          the date and time layout and the thousands
;                    separator
;   21h/7147h        whether long names are on - a long name is only
;                    given back when they are
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

MATCH_MAX       EQU     8192            ; files one run can sort
TXT_MAX         EQU     320             ; any path this program builds

MODE_RECOVER    EQU     0               ; ask about each one
MODE_LIST       EQU     1
MODE_ALL        EQU     2               ; recover them all, asking nothing
MODE_PURGE      EQU     3
MODE_STATUS     EQU     4

_UD     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_UD, DS:_UD, ES:_UD, SS:_UD
        ORG     0

;=====================================================================
; D32 header - the BSS is the table of matching records, two dwords
; each: the sequence number and the record's index
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET ud_entry>,<MATCH_MAX * 8>,8192

;=====================================================================
ud_entry:
        call    get_country
        call    get_lfn
        call    parse_line              ; stops the program on a bad line
        call    find_target             ; ...and on a bad path
        mov     al,[u_mode]
        cmp     al,MODE_STATUS
        je      do_status
        cmp     al,MODE_PURGE
        je      do_purge
        jmp     do_files

;=====================================================================
; The command line
;=====================================================================
parse_line:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        mov     byte ptr [esi+ecx],0    ; the 0Dh becomes the end
pl_next:
        mov     al,[esi]
        or      al,al
        jz      pl_done
        cmp     al,0Dh
        je      pl_done
        cmp     al,' '
        je      pl_skip
        cmp     al,9
        je      pl_skip
        mov     [u_tok],esi
        cmp     al,'/'
        je      pl_switch

        ; the path, once
        cmp     byte ptr [u_arg],0
        jne     pl_toomany
        mov     edi,OFFSET u_arg
        mov     ecx,TXT_MAX - 1
        cmp     al,'"'                  ; "a long name with spaces"
        jne     pl_pcopy
        inc     esi
pl_qcopy:
        mov     al,[esi]
        or      al,al
        jz      pl_pend
        cmp     al,0Dh
        je      pl_pend
        inc     esi
        cmp     al,'"'
        je      pl_pend
        mov     [edi],al
        inc     edi
        loop    pl_qcopy
        jmp     pl_bad
pl_pcopy:
        mov     al,[esi]
        call    is_end
        je      pl_pend
        cmp     al,'/'
        je      pl_pend
        mov     [edi],al
        inc     edi
        inc     esi
        loop    pl_pcopy
        jmp     pl_bad
pl_pend:
        mov     byte ptr [edi],0
        cmp     byte ptr [u_arg],0
        je      pl_bad                  ; "" names nothing
        jmp     short pl_next
pl_skip:
        inc     esi
        jmp     short pl_next

pl_switch:
        inc     esi
        mov     edi,OFFSET u_word
        mov     ecx,15
pl_wcopy:
        mov     al,[esi]
        call    is_end
        je      pl_wend
        cmp     al,'/'
        je      pl_wend
        call    upcase
        mov     [edi],al
        inc     edi
        inc     esi
        loop    pl_wcopy
        jmp     pl_bad
pl_wend:
        mov     byte ptr [edi],0
        mov     edi,OFFSET sw_names
        xor     ebx,ebx                 ; which one
pl_wfind:
        cmp     byte ptr [edi],0
        je      pl_bad
        push    esi
        push    edi
        mov     esi,OFFSET u_word
        call    str_eq
        pop     edi
        pop     esi
        je      pl_whave
pl_wskip:
        inc     edi
        cmp     byte ptr [edi-1],0
        jne     pl_wskip
        inc     ebx
        jmp     short pl_wfind
pl_whave:
        cmp     ebx,SW_HELP
        je      pl_help
        cmp     ebx,SW_SUBDIRS
        jne     pl_mode
        mov     byte ptr [u_subdirs],1
        jmp     pl_next
pl_mode:
        cmp     byte ptr [u_modeset],0  ; one of the five, and only one
        jne     pl_bad
        mov     byte ptr [u_modeset],1
        mov     al,[sw_modes+ebx]
        mov     [u_mode],al
        jmp     pl_next

pl_done:
        ; /PURGE and /STATUS take a drive, and nothing else
        cmp     byte ptr [u_mode],MODE_PURGE
        jb      pl_ok
        cmp     byte ptr [u_subdirs],0
        jne     pl_badsw
        cmp     byte ptr [u_arg],0
        je      pl_ok
        cmp     byte ptr [u_arg+1],':'
        jne     pl_badarg
        cmp     byte ptr [u_arg+2],0
        je      pl_ok
        cmp     byte ptr [u_arg+2],'\'
        jne     pl_badarg
        cmp     byte ptr [u_arg+3],0
        jne     pl_badarg
pl_ok:
        ret

pl_badsw:
        mov     esi,OFFSET msg_badsw
        jmp     short pl_say
pl_badarg:
        mov     esi,OFFSET msg_driveonly
pl_say:
        call    puts
        mov     ax,4C01h
        int     21h

pl_help:
        mov     esi,OFFSET msg_usage
        call    puts
        mov     ax,4C00h
        int     21h

pl_toomany:
        mov     esi,OFFSET msg_toomany
        jmp     short pl_badtok
pl_bad:
        mov     esi,OFFSET msg_badparm
pl_badtok:
        call    puts
        mov     esi,[u_tok]
pl_btlp:
        mov     al,[esi]
        call    is_end
        je      pl_btend
        call    putc
        inc     esi
        jmp     short pl_btlp
pl_btend:
        call    crlf
        mov     ax,4C01h
        int     21h

;=====================================================================
; find_target - the drive, the directory (canonical, as the kernel
; records it: "\DIR\SUB", 8.3 names, "\" for the root) and the name
; pattern, out of u_arg.
;=====================================================================
find_target:
        mov     esi,OFFSET u_arg
        cmp     byte ptr [esi],0
        je      ft_nodrive
        cmp     byte ptr [esi+1],':'
        jne     ft_nodrive
        mov     al,[esi]
        call    upcase
        sub     al,'A'
        cmp     al,26
        jae     ft_baddrive
        add     esi,2
        jmp     short ft_drive
ft_nodrive:
        mov     ah,19h
        int     21h
ft_drive:
        mov     [u_drv],al
        add     al,'A'
        mov     [u_letter],al

        ; is it a drive at all?  Its current directory says - and that
        ; is kept, to come back to after a visit (ft_canon)
        mov     dl,[u_drv]
        inc     dl
        mov     esi,OFFSET u_savecwd
        mov     ah,47h
        int     21h
        jc      ft_baddrive

        mov     esi,OFFSET u_arg
        cmp     byte ptr [esi+1],':'
        jne     ft_rest
        add     esi,2
ft_rest:
        mov     [u_rest],esi
        mov     edi,OFFSET u_spec       ; the default: everything
        mov     byte ptr [edi],'*'
        mov     byte ptr [edi+1],'.'
        mov     byte ptr [edi+2],'*'
        mov     byte ptr [edi+3],0
        cmp     byte ptr [esi],0
        je      ft_cwd

        ; "d:" + the rest: a directory itself, or a directory and a name?
        mov     edi,OFFSET u_tmp
        call    put_drive
        mov     esi,[u_rest]
        call    str_cpy
        mov     esi,OFFSET u_tmp
        call    str_len
        mov     al,[esi+eax-1]
        cmp     al,'\'
        je      ft_alldir
        cmp     al,'/'
        je      ft_alldir
        mov     edx,OFFSET u_tmp
        mov     eax,4300h
        int     21h
        jc      ft_split
        test    cl,ATTR_DIR
        jz      ft_split
ft_alldir:
        mov     esi,[u_rest]
        mov     edi,OFFSET u_dpart
        call    str_cpy
        jmp     short ft_canon

ft_split:
        ; everything up to the last separator is the directory
        mov     esi,[u_rest]
        mov     ebx,-1
        xor     ecx,ecx
ft_find:
        mov     al,[esi+ecx]
        or      al,al
        jz      ft_found
        cmp     al,'\'
        je      ft_sep
        cmp     al,'/'
        jne     ft_fnext
ft_sep:
        mov     ebx,ecx
ft_fnext:
        inc     ecx
        jmp     short ft_find
ft_found:
        lea     esi,[esi+ebx+1]         ; the name
        mov     edi,OFFSET u_spec
        call    str_cpy
        mov     esi,[u_rest]
        mov     edi,OFFSET u_dpart
        mov     ecx,ebx
        cmp     ebx,-1
        je      ft_nodir
        or      ecx,ecx
        jnz     ft_dcopy
        inc     ecx                     ; "\NAME": the directory is "\"
ft_dcopy:
        rep     movsb
ft_nodir:
        mov     byte ptr [edi],0
        cmp     byte ptr [u_dpart],0
        je      ft_cwd

ft_canon:
        ; THE DIRECTORY AS THE KERNEL WROTE IT DOWN.  Going there and
        ; asking where we are gives exactly that, long names and ".."
        ; and all; a directory that no longer exists cannot be gone to,
        ; and the resolver's own tidying (60h) is the next best thing.
        mov     edi,OFFSET u_tmp
        call    put_drive
        mov     esi,OFFSET u_dpart
        call    str_cpy
        mov     esi,OFFSET u_tmp        ; no trailing separator, except
        call    str_len                 ; on the root itself
        cmp     eax,3
        jbe     ft_go
        mov     bl,[esi+eax-1]
        cmp     bl,'\'
        je      ft_trim
        cmp     bl,'/'
        jne     ft_go
ft_trim:
        mov     byte ptr [esi+eax-1],0
ft_go:
        mov     edx,OFFSET u_tmp
        mov     ah,3Bh
        int     21h
        jc      ft_truename
        call    ft_here                 ; where that is...
        mov     edi,OFFSET u_tmp        ; ...and back to where we were
        call    put_drive
        mov     byte ptr [edi],'\'
        inc     edi
        mov     esi,OFFSET u_savecwd
        call    str_cpy
        mov     edx,OFFSET u_tmp
        mov     ah,3Bh
        int     21h
        jmp     short ft_spec

ft_truename:
        mov     esi,OFFSET u_tmp
        mov     edi,OFFSET u_true
        mov     ah,60h
        int     21h
        jc      ft_badpath
        mov     esi,OFFSET u_true+2
        mov     edi,OFFSET u_dir
        call    str_cpy
        jmp     short ft_spec

ft_cwd:
        call    ft_here
ft_spec:
        ; the name, as an 8.3 pattern for the short names; the text
        ; itself is kept for the long ones
        mov     esi,OFFSET u_spec
        mov     edi,OFFSET u_pat
        call    to_fcb
        ret

; ft_here - u_dir = "\" + the drive's current directory
ft_here:
        mov     byte ptr [u_dir],'\'
        mov     dl,[u_drv]
        inc     dl
        mov     esi,OFFSET u_dir+1
        mov     ah,47h
        int     21h
        ret

ft_baddrive:
        mov     esi,OFFSET msg_baddrive
        jmp     short ft_die
ft_badpath:
        mov     esi,OFFSET msg_badpath
ft_die:
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; /STATUS
;=====================================================================
do_status:
        mov     eax,0F118h
        int     21h
        jc      ds_old
        mov     [u_pct],ecx
        mov     [u_on],al
        mov     esi,OFFSET msg_on
        test    al,UNDF_ON
        jnz     ds_state
        mov     esi,OFFSET msg_off
ds_state:
        call    puts

        call    count_kept              ; -> u_count, u_bytes
        call    drive_line
        test    byte ptr [u_on],UNDF_ON ; the allowance means nothing
        jz      ud_exit0                ; when nothing is being kept
        mov     esi,OFFSET msg_pct1
        call    puts
        mov     eax,[u_pct]
        call    pdec
        mov     esi,OFFSET msg_pct2
        call    puts
        jmp     ud_exit0
ds_old:
        mov     esi,OFFSET msg_nokernel
        call    puts
        mov     ax,4C01h
        int     21h

; drive_line - "Drive C: holds n deleted files, b bytes." from u_count
;              and u_bytes
drive_line:
        mov     esi,OFFSET msg_drive
        call    puts
        mov     al,[u_letter]
        call    putc
        cmp     dword ptr [u_count],0
        jne     dl_some
        mov     esi,OFFSET msg_holdsnone
        call    puts
        ret
dl_some:
        mov     esi,OFFSET msg_holds
        call    puts
        mov     eax,[u_count]
        call    pdec
        mov     esi,OFFSET msg_files1
        cmp     dword ptr [u_count],1
        je      dl_one
        mov     esi,OFFSET msg_filesn
dl_one:
        call    puts
        mov     eax,[u_bytes]
        call    pdec
        mov     esi,OFFSET msg_bytesdot
        call    puts
        ret

; count_kept - how many kept files there are on the drive, and their
;              bytes, counting only those still there
count_kept:
        mov     dword ptr [u_count],0
        mov     dword ptr [u_bytes],0
        call    open_index
        jc      ck_out
        mov     ebx,1
ck_lp:
        cmp     ebx,[u_nrec]
        jae     ck_done
        mov     eax,ebx
        call    read_rec
        jc      ck_done
        test    byte ptr [u_rec+UDR_FLAGS],UDRF_USED
        jz      ck_next
        call    stored_path
        mov     edx,OFFSET u_src
        mov     eax,4300h
        int     21h
        jc      ck_next
        inc     dword ptr [u_count]
        mov     eax,dword ptr [u_rec+UDR_SIZE]
        add     [u_bytes],eax
ck_next:
        inc     ebx
        jmp     short ck_lp
ck_done:
        call    close_index
ck_out:
        ret

;=====================================================================
; /PURGE
;=====================================================================
do_purge:
        call    count_kept

        ; everything in \RECYCLED goes, the index with it
        mov     edx,OFFSET u_dta
        mov     ah,1Ah
        int     21h
        mov     edi,OFFSET u_tmp
        call    put_bin
        mov     esi,OFFSET txt_all
        call    str_cpy
        mov     edx,OFFSET u_tmp
        mov     ecx,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_ARCHIVE
        mov     ah,4Eh
        int     21h
        jc      dp_rmdir
dp_each:
        mov     edi,OFFSET u_tmp
        call    put_bin
        mov     esi,OFFSET u_dta+1Eh
        call    str_cpy
        mov     edx,OFFSET u_tmp
        xor     ecx,ecx
        mov     eax,4301h
        int     21h
        mov     edx,OFFSET u_tmp
        mov     ah,41h
        int     21h
        mov     ah,4Fh
        int     21h
        jnc     dp_each
dp_rmdir:
        mov     edi,OFFSET u_tmp
        call    put_bin
        mov     byte ptr [edi-1],0      ; "d:\RECYCLED"
        mov     edx,OFFSET u_tmp
        mov     ah,3Ah
        int     21h

        cmp     dword ptr [u_count],0
        jne     dp_said
        call    drive_line
        jmp     ud_exit0
dp_said:
        mov     esi,OFFSET msg_drive    ; "Drive C: 3 files, 1,234 bytes
        call    puts                    ;  deleted for good."
        mov     al,[u_letter]
        call    putc
        mov     esi,OFFSET msg_colonsp
        call    puts
        mov     eax,[u_count]
        call    pdec
        mov     esi,OFFSET msg_files1
        cmp     dword ptr [u_count],1
        je      dp_one
        mov     esi,OFFSET msg_filesn
dp_one:
        call    puts
        mov     eax,[u_bytes]
        call    pdec
        mov     esi,OFFSET msg_purged
        call    puts
        jmp     ud_exit0

;=====================================================================
; Listing and recovering
;=====================================================================
do_files:
        call    open_index
        jc      df_none

        ; THE FILES THAT MATCH, in the order they were deleted.  A
        ; record whose file is no longer in \RECYCLED - deleted from it
        ; by hand - is let go on the way past; there is nothing it
        ; could give back.
        xor     ebp,ebp                 ; matches so far
        mov     ebx,1
df_scan:
        cmp     ebx,[u_nrec]
        jae     df_sort
        mov     eax,ebx
        call    read_rec
        jc      df_sort
        test    byte ptr [u_rec+UDR_FLAGS],UDRF_USED
        jz      df_snext
        call    match_rec
        jc      df_snext
        call    stored_path
        mov     edx,OFFSET u_src
        mov     eax,4300h
        int     21h
        jnc     df_there
        mov     eax,ebx
        call    free_rec
        jmp     short df_snext
df_there:
        cmp     ebp,MATCH_MAX
        jae     df_snext
        mov     eax,dword ptr [u_rec+UDR_SEQ]
        mov     [img_end+ebp*8],eax
        mov     [img_end+ebp*8+4],ebx
        inc     ebp
df_snext:
        inc     ebx
        jmp     short df_scan

df_sort:
        mov     [u_nmatch],ebp
        or      ebp,ebp
        jz      df_close
        ; insertion sort on the sequence number
        mov     ecx,1
dso_outer:
        cmp     ecx,ebp
        jae     df_list
        mov     eax,[img_end+ecx*8]
        mov     edx,[img_end+ecx*8+4]
        mov     ebx,ecx
dso_inner:
        or      ebx,ebx
        jz      dso_put
        cmp     [img_end+ebx*8-8],eax
        jbe     dso_put
        mov     esi,[img_end+ebx*8-8]
        mov     [img_end+ebx*8],esi
        mov     esi,[img_end+ebx*8-4]
        mov     [img_end+ebx*8+4],esi
        dec     ebx
        jmp     short dso_inner
dso_put:
        mov     [img_end+ebx*8],eax
        mov     [img_end+ebx*8+4],edx
        inc     ecx
        jmp     short dso_outer

df_list:
        mov     esi,OFFSET msg_listhead
        call    puts
        call    put_where
        call    crlf
        call    crlf

        xor     ecx,ecx
df_each:
        cmp     ecx,[u_nmatch]
        jae     df_summary
        push    ecx
        mov     eax,[img_end+ecx*8+4]
        mov     [u_cur],eax
        call    read_rec
        jc      df_next
        call    print_rec
        mov     eax,dword ptr [u_rec+UDR_SIZE]
        add     [u_bytes],eax
        cmp     byte ptr [u_mode],MODE_LIST
        je      df_next
        cmp     byte ptr [u_mode],MODE_ALL
        je      df_recover
        call    ask_yn                  ; -> AL = 'Y', 'N' or 1Bh
        cmp     al,1Bh
        je      df_stop
        cmp     al,'Y'
        jne     df_next
df_recover:
        call    recover
df_next:
        pop     ecx
        inc     ecx
        jmp     short df_each
df_stop:
        pop     ecx

df_summary:
        call    crlf
        cmp     byte ptr [u_mode],MODE_LIST
        jne     df_done
        mov     eax,[u_nmatch]
        call    pdec
        mov     esi,OFFSET msg_files1
        cmp     dword ptr [u_nmatch],1
        je      dfs_one
        mov     esi,OFFSET msg_filesn
dfs_one:
        call    puts
        mov     eax,[u_bytes]
        call    pdec
        mov     esi,OFFSET msg_bytesdot
        call    puts
        jmp     short df_close
df_done:
        mov     eax,[u_nrecov]
        call    pdec
        mov     esi,OFFSET msg_recov1
        cmp     dword ptr [u_nrecov],1
        je      dfd_one
        mov     esi,OFFSET msg_recovn
dfd_one:
        call    puts
df_close:
        call    close_index
        cmp     dword ptr [u_nmatch],0
        jne     ud_exit0
df_none:
        mov     esi,OFFSET msg_nonefound
        call    puts
        call    put_where
        mov     esi,OFFSET msg_dot
        call    puts
        mov     eax,0F118h              ; and why, if it is that
        int     21h
        jc      ud_exit0
        test    al,UNDF_ON
        jnz     ud_exit0
        mov     esi,OFFSET msg_off
        call    puts
ud_exit0:
        mov     ax,4C00h
        int     21h

; put_where - "C:\DOCS" (and " or below" under /S)
put_where:
        mov     al,[u_letter]
        call    putc
        mov     al,':'
        call    putc
        mov     esi,OFFSET u_dir
        call    puts
        cmp     byte ptr [u_subdirs],0
        je      pw_out
        mov     esi,OFFSET msg_below
        call    puts
pw_out:
        ret

;---------------------------------------------------------------------
; ask_yn - "Recover it (Y/N)?" -> AL = 'Y', 'N', or 1Bh for Esc (which
;          stops the run).  A redirected input that runs dry stops it
;          too, rather than asking for ever.
;---------------------------------------------------------------------
ask_yn:
        mov     esi,OFFSET msg_ask
        call    puts
        mov     ecx,64
ay_lp:
        push    ecx
        mov     ah,08h
        int     21h
        pop     ecx
        call    upcase
        cmp     al,'Y'
        je      ay_have
        cmp     al,'N'
        je      ay_have
        cmp     al,1Bh
        je      ay_esc
        cmp     al,1Ah
        je      ay_esc
        loop    ay_lp
ay_esc:
        push    eax
        call    crlf
        pop     eax
        mov     al,1Bh
        ret
ay_have:
        push    eax
        call    putc
        call    crlf
        pop     eax
        ret

;---------------------------------------------------------------------
; recover - put the file of record [u_cur] (in u_rec) back
;---------------------------------------------------------------------
recover:
        ; the directory, made again if it has gone
        mov     edi,OFFSET u_dst
        call    put_drive
        mov     esi,OFFSET u_rec+UDR_DIR
        call    str_cpy
        call    make_dirs
        jc      rc_nodir

        ; the name: the long one when there is one and long names are
        ; on, the 8.3 one otherwise
        mov     edi,OFFSET u_dst
        mov     esi,edi
        call    str_len
        add     edi,eax
        cmp     byte ptr [edi-1],'\'
        je      rc_sep
        mov     byte ptr [edi],'\'
        inc     edi
rc_sep:
        mov     [u_dname],edi
        mov     esi,OFFSET u_rec+UDR_SHORT
        cmp     byte ptr [u_lfn],0
        je      rc_named
        cmp     byte ptr [u_rec+UDR_LONG],0
        je      rc_named
        mov     esi,OFFSET u_rec+UDR_LONG
rc_named:
        call    str_cpy
        mov     byte ptr [u_renamed],0

        ; taken?  Then '#' or the next free character for its first
        mov     edx,OFFSET u_dst
        mov     eax,4300h
        int     21h
        jc      rc_free
        mov     byte ptr [u_renamed],1
        mov     ebx,OFFSET alt_chars
rc_alt:
        mov     al,[ebx]
        or      al,al
        jz      rc_noname
        mov     edi,[u_dname]
        mov     esi,OFFSET u_rec+UDR_SHORT
        call    str_cpy
        mov     edi,[u_dname]
        mov     [edi],al
        inc     ebx
        mov     edx,OFFSET u_dst
        mov     eax,4300h
        int     21h
        jnc     rc_alt

rc_free:
        call    stored_path
        mov     edx,OFFSET u_src
        mov     edi,OFFSET u_dst
        mov     ah,56h
        int     21h
        jc      rc_failed
        movzx   ecx,byte ptr [u_rec+UDR_ATTR]
        and     ecx,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_ARCHIVE
        mov     edx,OFFSET u_dst
        mov     eax,4301h
        int     21h
        mov     eax,[u_cur]
        call    free_rec
        inc     dword ptr [u_nrecov]
        cmp     byte ptr [u_renamed],0
        je      rc_out
        mov     esi,OFFSET msg_as
        call    puts
        mov     esi,OFFSET u_dst
        call    puts
        call    crlf
rc_out:
        ret

rc_nodir:
        mov     esi,OFFSET msg_nodir
        jmp     short rc_say
rc_noname:
        mov     esi,OFFSET msg_noname
        jmp     short rc_say
rc_failed:
        mov     esi,OFFSET msg_failed
rc_say:
        call    puts
        ret

;---------------------------------------------------------------------
; make_dirs - every directory in u_dst ("d:\A\B") that is missing is
;             made, from the root down.  CF if one cannot be, or a
;             FILE is in the way.
;---------------------------------------------------------------------
make_dirs:
        mov     esi,OFFSET u_dst+3      ; past "d:\"
md_lp:
        cmp     byte ptr [esi],0
        je      md_ok
        mov     edi,esi
md_scan:
        mov     al,[edi]
        or      al,al
        jz      md_cut
        cmp     al,'\'
        je      md_cut
        inc     edi
        jmp     short md_scan
md_cut:
        mov     bl,[edi]
        mov     byte ptr [edi],0
        mov     edx,OFFSET u_dst
        mov     eax,4300h
        int     21h
        jc      md_make
        test    cl,ATTR_DIR
        jz      md_fail
        jmp     short md_on
md_make:
        mov     edx,OFFSET u_dst
        mov     ah,39h
        int     21h
        jc      md_fail
md_on:
        mov     [edi],bl
        or      bl,bl
        jz      md_ok
        lea     esi,[edi+1]
        jmp     short md_lp
md_ok:
        clc
        ret
md_fail:
        mov     [edi],bl
        stc
        ret

;---------------------------------------------------------------------
; print_rec - one line: when it was deleted, its size, where it was
;---------------------------------------------------------------------
print_rec:
        movzx   eax,word ptr [u_rec+UDR_DDATE]
        call    put_date
        mov     al,' '
        call    putc
        call    putc
        movzx   eax,word ptr [u_rec+UDR_DTIME]
        call    put_time
        mov     eax,dword ptr [u_rec+UDR_SIZE]
        mov     ecx,14
        call    pdec_w
        mov     al,' '
        call    putc
        call    putc
        mov     edi,OFFSET u_tmp        ; d:\DIR\name
        call    put_drive
        mov     esi,OFFSET u_rec+UDR_DIR
        call    str_cpy
        cmp     byte ptr [edi-1],'\'
        je      pr_sep
        mov     byte ptr [edi],'\'
        inc     edi
pr_sep:
        mov     esi,OFFSET u_rec+UDR_SHORT
        cmp     byte ptr [u_rec+UDR_LONG],0
        je      pr_name
        mov     esi,OFFSET u_rec+UDR_LONG
pr_name:
        call    str_cpy
        mov     esi,OFFSET u_tmp
        call    puts
        call    crlf
        ret

; put_date - the DOS date in EAX, the way the country setting lays it out
put_date:
        mov     edx,eax
        and     edx,1Fh
        mov     [u_day],edx
        mov     edx,eax
        shr     edx,5
        and     edx,0Fh
        mov     [u_month],edx
        shr     eax,9
        add     eax,1980
        mov     [u_year],eax
        mov     ax,word ptr [ctry]      ; 0 = m d y, 1 = d m y, 2 = y m d
        cmp     ax,1
        je      pd_dmy
        cmp     ax,2
        je      pd_ymd
        mov     eax,[u_month]
        call    pdec2
        call    put_dsep
        mov     eax,[u_day]
        call    pdec2
        call    put_dsep
        mov     eax,[u_year]
        jmp     pyear
pd_dmy:
        mov     eax,[u_day]
        call    pdec2
        call    put_dsep
        mov     eax,[u_month]
        call    pdec2
        call    put_dsep
        mov     eax,[u_year]
        jmp     pyear
pd_ymd:
        mov     eax,[u_year]
        call    pyear
        call    put_dsep
        mov     eax,[u_month]
        call    pdec2
        call    put_dsep
        mov     eax,[u_day]
        jmp     pdec2
put_dsep:
        mov     al,[ctry+0Bh]
        jmp     putc

; put_time - the DOS time in EAX: 24-hour or 12-hour, as the country says
put_time:
        mov     edx,eax
        shr     edx,5
        and     edx,3Fh
        mov     [u_min],edx
        shr     eax,11
        test    byte ptr [ctry+11h],1
        jnz     pt_24
        mov     byte ptr [u_ampm],'a'
        cmp     eax,12
        jb      pt_am
        mov     byte ptr [u_ampm],'p'
        sub     eax,12
pt_am:
        or      eax,eax
        jnz     pt_12
        mov     eax,12
pt_12:
        mov     ecx,2
        call    pdec_w
        call    put_tsep
        mov     eax,[u_min]
        call    pdec2
        mov     al,[u_ampm]
        jmp     putc
pt_24:
        call    pdec2
        call    put_tsep
        mov     eax,[u_min]
        call    pdec2
        mov     al,' '
        jmp     putc
put_tsep:
        mov     al,[ctry+0Dh]
        jmp     putc

;=====================================================================
; The index
;=====================================================================

; open_index - d:\RECYCLED\UNDELETE.DAT, read and write.  u_nrec =
;              the records in it, the header included.  CF if there is
;              none; a file that is not ours stops the program.
open_index:
        mov     edi,OFFSET u_tmp
        call    put_bin
        mov     esi,OFFSET txt_index
        call    str_cpy
        mov     edx,OFFSET u_tmp
        mov     eax,3D02h
        int     21h
        jc      oi_none
        movzx   eax,ax
        mov     [u_handle],eax
        mov     ebx,eax
        xor     ecx,ecx
        mov     eax,4202h               ; its size
        int     21h
        xor     edx,edx
        mov     ecx,UDR_LEN
        div     ecx
        mov     [u_nrec],eax
        or      eax,eax
        jz      oi_bad
        xor     eax,eax
        call    read_rec
        jc      oi_bad
        mov     esi,OFFSET u_rec
        mov     edi,OFFSET txt_magic
        mov     ecx,8
        repe    cmpsb
        jne     oi_bad
        cmp     word ptr [u_rec+UDH_RECLEN],UDR_LEN
        jne     oi_bad
        clc
        ret
oi_none:
        mov     dword ptr [u_handle],-1
        stc
        ret
oi_bad:
        call    close_index
        mov     edi,OFFSET u_tmp        ; "d:\RECYCLED\UNDELETE.DAT is
        call    put_bin                 ;  not an index..."
        mov     esi,OFFSET txt_index
        call    str_cpy
        mov     esi,OFFSET u_tmp
        call    puts
        mov     esi,OFFSET msg_badindex
        call    puts
        mov     ax,4C01h
        int     21h

close_index:
        mov     ebx,[u_handle]
        cmp     ebx,-1
        je      ci_out
        mov     ah,3Eh
        int     21h
        mov     dword ptr [u_handle],-1
ci_out:
        ret

; read_rec - record EAX into u_rec.  CF if it is not all there.
;            Registers bar EAX preserved.
read_rec:
        push    ebx
        push    ecx
        push    edx
        call    seek_rec
        mov     ecx,UDR_LEN
        mov     edx,OFFSET u_rec
        mov     ah,3Fh
        int     21h
        jc      rr_out
        cmp     eax,UDR_LEN
        je      rr_out                  ; CF clear from the compare
        stc
rr_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

; free_rec - record EAX is not a kept file any more
free_rec:
        push    ebx
        push    ecx
        push    edx
        call    seek_rec
        mov     ecx,1
        mov     edx,OFFSET zero_byte
        mov     ah,40h
        int     21h
        pop     edx
        pop     ecx
        pop     ebx
        ret

; seek_rec - the index's position to record EAX; BX = the handle
seek_rec:
        imul    eax,UDR_LEN
        mov     ecx,eax
        mov     ebx,[u_handle]
        mov     eax,4200h
        int     21h
        ret

; stored_path - u_src = "d:\RECYCLED\" + u_rec's stored name, dotted
stored_path:
        push    esi
        push    edi
        push    ecx
        mov     edi,OFFSET u_src
        call    put_bin
        mov     esi,OFFSET u_rec+UDR_STORED
        mov     ecx,8
sp_name:
        lodsb
        cmp     al,' '
        je      sp_nend
        stosb
        loop    sp_name
sp_nend:
        mov     esi,OFFSET u_rec+UDR_STORED+8
        cmp     byte ptr [esi],' '
        je      sp_end
        mov     al,'.'
        stosb
        mov     ecx,3
sp_ext:
        lodsb
        cmp     al,' '
        je      sp_end
        stosb
        loop    sp_ext
sp_end:
        mov     byte ptr [edi],0
        pop     ecx
        pop     edi
        pop     esi
        ret

;---------------------------------------------------------------------
; match_rec - does u_rec's file answer the path given?  CF clear if so.
;             The directory has to be u_dir (or below it, under /S) and
;             the name has to fit the pattern, in 8.3 or in its long
;             form.
;---------------------------------------------------------------------
match_rec:
        push    ebx
        mov     esi,OFFSET u_dir
        mov     edi,OFFSET u_rec+UDR_DIR
mr_dir:
        mov     al,[esi]
        or      al,al
        jz      mr_dend
        call    upcase
        mov     ah,al
        mov     al,[edi]
        call    upcase
        cmp     al,ah
        jne     mr_no
        inc     esi
        inc     edi
        jmp     short mr_dir
mr_dend:
        cmp     byte ptr [edi],0        ; the same directory
        je      mr_name
        cmp     byte ptr [u_subdirs],0
        je      mr_no
        cmp     byte ptr [edi],'\'      ; one below it
        je      mr_name
        cmp     byte ptr [edi-1],'\'    ; below the root
        jne     mr_no
mr_name:
        mov     esi,OFFSET u_rec+UDR_SHORT
        mov     edi,OFFSET u_nfcb
        call    to_fcb
        mov     esi,OFFSET u_pat
        mov     edi,OFFSET u_nfcb
        mov     ecx,11
mr_fcb:
        mov     al,[esi]
        cmp     al,'?'
        je      mr_fnext
        cmp     al,[edi]
        jne     mr_long
mr_fnext:
        inc     esi
        inc     edi
        loop    mr_fcb
        jmp     short mr_yes
mr_long:
        cmp     byte ptr [u_rec+UDR_LONG],0
        je      mr_no
        mov     esi,OFFSET u_spec
        mov     edi,OFFSET u_rec+UDR_LONG
        call    glob
        jnc     mr_yes
mr_no:
        pop     ebx
        stc
        ret
mr_yes:
        pop     ebx
        clc
        ret

;---------------------------------------------------------------------
; glob - does the text at EDI fit the pattern at ESI?  '*' and '?',
;        without regard to case.  CF clear if it does.
;---------------------------------------------------------------------
glob:
        push    ebx
        push    edx
        xor     ebx,ebx                 ; after the last '*': pattern...
        xor     edx,edx                 ; ...and text
gb_lp:
        mov     al,[esi]
        cmp     al,'*'
        jne     gb_notstar
        inc     esi
        mov     ebx,esi
        mov     edx,edi
        jmp     short gb_lp
gb_notstar:
        cmp     byte ptr [edi],0
        je      gb_tend
        cmp     al,'?'
        je      gb_step
        call    upcase
        mov     ah,al
        mov     al,[edi]
        call    upcase
        cmp     al,ah
        jne     gb_back
gb_step:
        inc     esi
        inc     edi
        jmp     short gb_lp
gb_back:
        or      ebx,ebx
        jz      gb_no
        inc     edx                     ; the '*' takes one more character
        mov     edi,edx
        mov     esi,ebx
        jmp     short gb_lp
gb_tend:
        cmp     byte ptr [esi],'*'      ; only stars may be left
        jne     gb_tlast
        inc     esi
        jmp     short gb_tend
gb_tlast:
        cmp     byte ptr [esi],0
        jne     gb_no
        pop     edx
        pop     ebx
        clc
        ret
gb_no:
        pop     edx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; to_fcb - the name at ESI as eleven bytes at EDI, the way a directory
;          entry holds it: upper case, padded with spaces, '*' filling
;          the rest of its part with '?'.
;---------------------------------------------------------------------
to_fcb:
        push    ecx
        push    esi
        push    edi
        mov     ecx,11
        mov     al,' '
        rep     stosb
        pop     edi
        push    edi
        mov     ecx,8                   ; the name part
tf_name:
        mov     al,[esi]
        or      al,al
        jz      tf_done
        cmp     al,'.'
        je      tf_dot
        inc     esi
        cmp     al,'*'
        je      tf_nstar
        jecxz   tf_name                 ; too long: the rest is dropped
        call    upcase
        mov     [edi],al
        inc     edi
        dec     ecx
        jmp     short tf_name
tf_nstar:
        jecxz   tf_name
        mov     byte ptr [edi],'?'
        inc     edi
        loop    tf_nstar
        jmp     short tf_name
tf_dot:
        inc     esi
        pop     edi
        push    edi
        add     edi,8
        mov     ecx,3                   ; the extension
tf_ext:
        mov     al,[esi]
        or      al,al
        jz      tf_done
        inc     esi
        cmp     al,'*'
        je      tf_estar
        jecxz   tf_ext
        call    upcase
        mov     [edi],al
        inc     edi
        dec     ecx
        jmp     short tf_ext
tf_estar:
        jecxz   tf_ext
        mov     byte ptr [edi],'?'
        inc     edi
        loop    tf_estar
        jmp     short tf_ext
tf_done:
        pop     edi
        pop     esi
        pop     ecx
        ret

;=====================================================================
; Small things
;=====================================================================

; get_country - the date and time layout and the separators
get_country:
        mov     ax,3800h
        mov     edx,OFFSET ctry
        int     21h
        jc      gc_dflt
        cmp     byte ptr [ctry+7],0
        jne     gc_out
gc_dflt:
        mov     word ptr [ctry],0
        mov     byte ptr [ctry+7],','
        mov     byte ptr [ctry+0Bh],'-'
        mov     byte ptr [ctry+0Dh],':'
        mov     byte ptr [ctry+11h],0
gc_out:
        ret

; get_lfn - u_lfn = 1 when long names are on (7147h is answered)
get_lfn:
        mov     eax,7147h
        xor     edx,edx
        mov     esi,OFFSET u_true
        stc
        int     21h
        jc      gl_out
        mov     byte ptr [u_lfn],1
gl_out:
        ret

; put_drive - "d:" at EDI, EDI past it
put_drive:
        mov     al,[u_letter]
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        add     edi,2
        mov     byte ptr [edi],0
        ret

; put_bin - "d:\RECYCLED\" at EDI, EDI past it
put_bin:
        call    put_drive
        push    esi
        mov     esi,OFFSET txt_bin
        call    str_cpy
        pop     esi
        ret

; str_cpy - ASCIIZ ESI to EDI; EDI ends on the NUL, ESI past it
str_cpy:
        push    eax
sc_lp:
        mov     al,[esi]
        inc     esi
        mov     [edi],al
        or      al,al
        jz      sc_out
        inc     edi
        jmp     short sc_lp
sc_out:
        pop     eax
        ret

; str_len - EAX = the length of the ASCIIZ at ESI
str_len:
        xor     eax,eax
sl_lp:
        cmp     byte ptr [esi+eax],0
        je      sl_out
        inc     eax
        jmp     short sl_lp
sl_out:
        ret

; str_eq - ZF if the ASCIIZ strings at ESI and EDI are the same
str_eq:
        mov     al,[esi]
        cmp     al,[edi]
        jne     se_out
        or      al,al
        jz      se_out
        inc     esi
        inc     edi
        jmp     short str_eq
se_out:
        ret

; is_end - ZF if AL ends a word on the command line
is_end:
        cmp     al,' '
        je      ie_out
        cmp     al,9
        je      ie_out
        cmp     al,0Dh
        je      ie_out
        or      al,al
ie_out:
        ret

upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,20h
uc_out:
        ret

; puts - the ASCIIZ at ESI to standard output.  Registers preserved.
puts:
        pushad
        call    str_len
        mov     ecx,eax
        mov     edx,esi
        mov     ebx,1
        mov     ah,40h
        int     21h
        popad
        ret

putc:
        pushad
        mov     dl,al
        mov     ah,02h
        int     21h
        popad
        ret

crlf:
        push    esi
        mov     esi,OFFSET txt_crlf
        call    puts
        pop     esi
        ret

; pdec - EAX in decimal, with the country's thousands separator
pdec:
        xor     ecx,ecx
        ; fall through
; pdec_w - ...right-aligned in ECX columns
pdec_w:
        pushad
        mov     edi,OFFSET numbuf + 31
        mov     byte ptr [edi],0
        xor     ebp,ebp                 ; digits since a separator
        mov     ebx,10
pw_lp:
        xor     edx,edx
        div     ebx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        inc     ebp
        or      eax,eax
        jz      pw_done
        cmp     ebp,3
        jne     pw_lp
        cmp     byte ptr [u_plain],0    ; a year: no separator
        jne     pw_lp
        dec     edi
        mov     dl,[ctry+7]
        mov     [edi],dl
        xor     ebp,ebp
        jmp     short pw_lp
pw_done:
        mov     esi,edi
        call    str_len
pw_pad:
        cmp     eax,ecx
        jae     pw_say
        push    eax
        mov     al,' '
        call    putc
        pop     eax
        inc     eax
        jmp     short pw_pad
pw_say:
        call    puts
        popad
        ret

; pyear - EAX in decimal with no thousands separator: 2026, not 2,026
pyear:
        mov     byte ptr [u_plain],1
        call    pdec
        mov     byte ptr [u_plain],0
        ret

; pdec2 - EAX as two digits
pdec2:
        push    eax
        push    edx
        push    ecx
        xor     edx,edx
        mov     ecx,10
        div     ecx
        add     al,'0'
        call    putc
        mov     al,dl
        add     al,'0'
        call    putc
        pop     ecx
        pop     edx
        pop     eax
        ret

;=====================================================================
; Data
;=====================================================================
SW_HELP         EQU     4               ; the index of "?" in sw_names
SW_SUBDIRS      EQU     5               ; ...and of "S"
sw_names    db  'LIST',0, 'ALL',0, 'PURGE',0, 'STATUS',0, '?',0, 'S',0, 0
sw_modes    db  MODE_LIST, MODE_ALL, MODE_PURGE, MODE_STATUS

alt_chars   db  '#%&-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ',0

u_mode      db  MODE_RECOVER
u_modeset   db  0
u_subdirs   db  0
u_lfn       db  0
u_drv       db  0
u_letter    db  'C'
u_renamed   db  0
u_ampm      db  0
u_plain     db  0                       ; pdec_w: no separators
u_on        db  0                       ; F1h AL=18h's UNDF_* bits
zero_byte   db  0
            db  0,0
u_tok       dd  0
u_rest      dd  0
u_dname     dd  0
u_handle    dd  -1
u_nrec      dd  0
u_nmatch    dd  0
u_nrecov    dd  0
u_cur       dd  0
u_count     dd  0
u_bytes     dd  0
u_pct       dd  0
u_day       dd  0
u_month     dd  0
u_year      dd  0
u_min       dd  0

txt_bin     db  '\RECYCLED\',0          ; after "d:"
            db  0
txt_index   db  'UNDELETE.DAT',0
txt_all     db  '*.*',0
txt_magic   db  'PMUNDEL',1Ah
txt_crlf    db  13,10,0

u_word      db  16 dup(0)
u_arg       db  TXT_MAX dup(0)
u_spec      db  TXT_MAX dup(0)
u_dpart     db  TXT_MAX dup(0)
u_dir       db  TXT_MAX dup(0)
u_tmp       db  TXT_MAX dup(0)
u_true      db  TXT_MAX dup(0)
u_src       db  TXT_MAX dup(0)
u_dst       db  TXT_MAX dup(0)
u_savecwd   db  TXT_MAX dup(0)
u_pat       db  12 dup(0)
u_nfcb      db  12 dup(0)
u_dta       db  64 dup(0)
ctry        db  40 dup(0)
numbuf      db  32 dup(0)
u_rec       db  UDR_LEN dup(0)

msg_listhead db 'Deleted files from ',0
msg_below   db  ' and below',0
msg_ask     db  '    Recover it (Y/N)? ',0
msg_as      db  '    The name was taken; recovered as ',0
msg_nodir   db  '    Not recovered: its directory could not be made again.',13,10,0
msg_noname  db  '    Not recovered: no free name for it.',13,10,0
msg_failed  db  '    Not recovered: the rename failed.',13,10,0
msg_recov1  db  ' file recovered.',13,10,0
msg_recovn  db  ' files recovered.',13,10,0
msg_files1  db  ' file, ',0
msg_filesn  db  ' files, ',0
msg_bytesdot db ' bytes.',13,10,0
msg_nonefound db 'No deleted files from ',0
msg_dot     db  '.',13,10,0
msg_on      db  'Deleted files are being kept.',13,10,0
msg_off     db  'Deleted files are not being kept (UNDELETE=OFF in CONFIG.SYS).',13,10,0
msg_drive   db  'Drive ',0
msg_colonsp db  ': ',0
msg_holds   db  ': holds ',0
msg_holdsnone db ': holds no deleted files.',13,10,0
msg_pct1    db  'They may take up to ',0
msg_pct2    db  '% of the free space.',13,10,0
msg_purged  db  ' bytes deleted for good.',13,10,0
msg_nokernel db 'This version of PM-DOS does not keep deleted files.',13,10,0
msg_badindex db ' is not an index UNDELETE can read.',13,10,0
msg_baddrive db 'Invalid drive specification',13,10,0
msg_badpath db  'Invalid path',13,10,0
msg_badparm db  'Invalid parameter - ',0
msg_toomany db  'Too many parameters - ',0
msg_badsw   db  '/S goes with listing and recovering only.',13,10,0
msg_driveonly db '/PURGE and /STATUS take a drive letter only.',13,10,0

msg_usage   db  'Recovers files deleted while PM-DOS was keeping them.',13,10,13,10
            db  'UNDELETE [drive:][path][name] [/LIST | /ALL] [/S]',13,10
            db  'UNDELETE [drive:] /PURGE',13,10
            db  'UNDELETE [drive:] /STATUS',13,10,13,10
            db  '  [drive:][path]  The directory the files were deleted from.',13,10
            db  '                  The current directory if none is given.',13,10
            db  '  name            Which files; wildcards allowed.  All if none.',13,10
            db  '  /LIST           Lists them and recovers nothing.',13,10
            db  '  /ALL            Recovers them all without asking.',13,10
            db  '  /S              Takes in the directories below as well.',13,10
            db  '  /PURGE          Deletes every kept file on the drive for good.',13,10
            db  '  /STATUS         Shows what is being kept.',13,10,13,10
            db  'Deleted files are kept in a hidden RECYCLED directory on their',13,10
            db  'drive, in up to 10% of its free space; the oldest go first when',13,10
            db  'that fills.  *.TMP files are not kept.  A recovered file whose',13,10
            db  'name has been taken since gets # in place of its first letter.',13,10
            db  'UNDELETE=OFF in CONFIG.SYS stops files being kept.',13,10,0

img_end:                                ; the BSS begins here
_UD     ENDS
        END
