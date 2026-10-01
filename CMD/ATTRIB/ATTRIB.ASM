;=====================================================================
; ATTRIB.ASM - ATTRIB.EXE, file attributes shown and changed
;
;   ATTRIB [+R|-R] [+A|-A] [+S|-S] [+H|-H] [[d:][path]filename] [/S]
;
;   ATTRIB                  every file here, and what it is
;   ATTRIB \DOS\*.*         ...somewhere else
;   ATTRIB +R READ.ME       make it read-only
;   ATTRIB -H -S IO.SYS     take the hidden and system bits off
;   ATTRIB +A *.TXT /S      and everything below here as well
;
; THE FOUR BITS AND NOTHING ELSE.  Read-only, archive, system and
; hidden are what 21h/43h will change; the directory bit and the
; volume bit belong to the filesystem and the kernel puts them back
; behind this program's back (see attr_core in DOS\INT21.INC).  So
; there is no +D and no -V, and a directory named on the command line
; is shown with its own attributes rather than being descended into.
;
; THE COLUMNS ARE DOS 6'S OWN, taken from the bits[]/as[] pair in
; CMD\ATTRIB\ATTRIB.C and not from a screenshot: eight attribute
; columns, of which only four are ever printed -
;
;       bits  80  40  20  10  08  04  02  01
;       char   .   .   A   .   .   S   H   R
;
; - padded to sixteen characters, then four spaces, then the path.
; That is why ATTRIB's output has always looked like it has a hole in
; the middle: columns 1, 2, 4 and 5 are the two reserved bits, the
; directory bit and the volume bit, which DOS decided to reserve space
; for and never print.  Ours reserves the same space, because a batch
; file that reads this output with FOR reads it the same on both.
;
; THE PATH IS FULLY QUALIFIED BEFORE ANYTHING IS PRINTED, which is
; also DOS's behaviour (Make_fspec) and is the only thing that makes a
; /S listing readable: without it every line of a recursive sweep
; would be a bare filename with no indication of which directory it
; came out of.  7160h does it here, or 60h on a machine with LFN=OFF.
;
; A HIDDEN OR SYSTEM FILE IS NOT QUIETLY CHANGED.  "ATTRIB -R *.*" in
; a root directory would otherwise strip the read-only bit off IO.SYS
; and MSDOS.SYS on its way past, so a change that does not NAME the
; hidden or system bit refuses to touch a file that has one, and says
; so: "Not resetting hidden file C:\IO.SYS".  Naming it - "-R -H" -
; is taken as meaning it.  This is DOS 6's rule (messages 216 and 217
; in ATTRIB.SKL) and it is worth having for the same reason it was
; worth having there.
;
; AN EXPLICIT NAME MEANS WHAT IT SAYS; A WILDCARD MEANS THE ORDINARY
; FILES.  "ATTRIB +R SUB" sets the read-only bit on a directory called
; SUB because the user named one; "ATTRIB +R *.*" does not sweep up
; the subdirectories.  The same rule and the same wording as MOVE.
; AND /S COUNTS AS A WILDCARD even when the name is explicit, which
; looks arbitrary until you write the other version down: "ATTRIB +R
; SUB /S" would otherwise set the read-only bit on every directory
; named SUB anywhere on the disk, which nobody typing that means.
; DOS does this in one line - "if (descending) WildCard = TRUE" - and
; so does at_entry.
;
; THE FIND IS CLOSED BEFORE THE RECURSION, and that is the one piece
; of machinery here that is not obvious.  A find cannot be held open
; across a descent: 4Eh/4Fh keeps its whole state in the caller's DTA
; and there is one DTA, while the long-name find is handle-based with
; FIND_MAX - four - handles in the whole kernel, so a tree five deep
; would run out on its fifth level.  So each level READS ITS
; SUBDIRECTORY NAMES INTO A POOL, closes the search, and only then
; descends.  The pool is a stack: a level takes what it needs on the
; way down and gives it back on the way up, so what it has to hold is
; the widest PATH through the tree and not the whole tree.
;
; THE FILES, THOUGH, ARE ACTED ON WHILE THE SEARCH IS OPEN, and that
; is safe here for a reason worth writing down rather than assuming:
; both find-nexts rebuild everything they need before they step.  fn4F
; re-reads the drive, the pattern, the attribute mask and the
; directory context out of the caller's DTA (DOS\INT21.INC), and
; fn714F re-selects the volume and restores the context from its
; handle.  Neither carries live state in a kernel global that 43h
; could tread on - and 43h changes an entry's attribute byte in place,
; so nothing moves in the directory underneath the walk either.  That
; is the difference from MOVE, which had to collect first because
; moving a file DOES rewrite the directory it is walking.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

; Up here, all of them: wasm cannot use a forward-referenced EQU as an
; immediate and the uses start hundreds of lines above the data.
PATH_MAX        EQU     LFN_MAX         ; 260, including the NUL
TAIL_MAX        EQU     160             ; the PSP tail is 127 and a bit
DEPTH_MAX       EQU     32              ; levels /S will descend

; The pool the descent takes its subdirectory names from.  It holds
; one PATH through the tree and not the tree, so these are generous:
; 8000 characters of names is a path of two hundred directories with
; forty-character names, and the kernel refuses a path over 260 bytes
; long before that.
POOL_TEXT       EQU     8000
POOL_PTRS       EQU     512

; ATTRIB's own search mask: hidden, system and subdirectories as well
; as ordinary files.  Read-only and archive are always allowed and
; need not be asked for.  This is DOS's ALL3.
SRCH_ALL        EQU     ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_DIR

; The 318-byte record 714Eh fills in - Win95's WIN32_FIND_DATA, which
; is what the API promises.
FDR_ATTR        EQU     000h            ; dd
FDR_SIZE        EQU     020h            ; dd
FDR_LONG        EQU     02Ch            ; ASCIIZ - the 8.3 when there is
                                        ; no long name
FDR_SHORT       EQU     130h            ; ASCIIZ 8.3
FDR_LEN         EQU     13Eh

; ...and the DTA 4Eh fills in, for the machine with LFN=OFF.
DTA_ATTR        EQU     15h             ; db
DTA_NAME        EQU     1Eh             ; ASCIIZ 8.3
DTA_LEN         EQU     128

_ATTR   SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_ATTR, DS:_ATTR, ES:_ATTR, SS:_ATTR
        ORG     0

;=====================================================================
; D32 header - the name pool lives in the BSS, and the stack is eight
; kilobytes rather than the usual four because at_dir calls itself:
; DEPTH_MAX levels of saved registers and pool marks, with room left
; over for the kernel's own use of our stack on an INT 21h.
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET at_entry>,<POOL_TEXT + (POOL_PTRS * 4)>,8192

;=====================================================================
at_entry:
        mov     eax,OFFSET img_end      ; carve up the BSS
        mov     [pool_txt],eax
        add     eax,POOL_TEXT
        mov     [pool_ptr],eax

        mov     eax,[pool_txt]          ; the pool starts empty
        mov     [txt_next],eax
        mov     dword ptr [pool_n],0

        mov     edx,OFFSET dta          ; our own, before any find
        mov     eax,1A00h
        int     21h

        call    at_parse
        jc      at_usage

        cmp     byte ptr [opt_help],0
        jne     at_help

        call    at_lfnprobe
        call    at_split                ; spec -> spec_dir + spec_name

;--- what a wildcard does to the rules --------------------------------
; Both halves of the "an explicit name means what it says" rule are
; decided here, once, rather than per file: a pattern in the name, or
; /S, and subdirectories stop being things this command touches.
        mov     esi,OFFSET spec_name
        call    at_haswild
        jnc     at_nowild
        mov     byte ptr [wild],1
at_nowild:
        cmp     byte ptr [opt_sub],0
        je      at_qual
        mov     byte ptr [wild],1

;--- the directory the walk starts in, spelt out in full --------------
at_qual:
        call    at_qualify
        jc      at_badpath

        mov     byte ptr [did_one],0
        mov     dword ptr [depth],0
        call    at_dir
        jc      at_bad

        ; Nothing matched and nothing went wrong: DOS turns that into
        ; "file not found", and names what the USER typed rather than
        ; the qualified form, because the qualified form of a name that
        ; does not exist is not a helpful thing to be shown.
        cmp     byte ptr [did_one],0
        jne     at_exit
        mov     edx,OFFSET msg_nofile
        call    puts
        mov     esi,OFFSET spec
        call    putz
        call    crlf
        jmp     at_bad

at_exit:
        mov     eax,4C00h
        int     21h

at_bad:
        mov     eax,4C01h
        int     21h

at_badpath:
        mov     edx,OFFSET msg_nopath
        call    puts
        mov     esi,OFFSET spec
        call    putz
        call    crlf
        jmp     short at_bad

at_help:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C00h
        int     21h

at_usage:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     eax,4C01h
        int     21h

;=====================================================================
; at_dir - do [cur_dir]: everything below it first if /S was asked
;          for, then the files in it that match [spec_name].
;
;          CF set if something went wrong and the sweep should stop.
;          RECURSIVE - see the header for why the find is closed first.
;=====================================================================
at_dir:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        inc     dword ptr [depth]
        cmp     dword ptr [depth],DEPTH_MAX
        ja      ad_deep

        cmp     byte ptr [opt_sub],0
        je      ad_files

;--- the subdirectories, names first and the search shut --------------
        mov     eax,[txt_next]          ; where this level's names start
        push    eax
        mov     eax,[pool_n]
        push    eax

        call    at_collect
        jc      ad_full

        ; [pool_n] now counts this level's names as well as every
        ; level above it; ours are the ones from the saved count up.
        mov     ebx,[esp]               ; EBX = the first index of ours
ad_sub:
        cmp     ebx,[pool_n]
        jae     ad_subdone
        mov     eax,[pool_ptr]
        mov     esi,[eax + ebx*4]

        call    at_push                 ; cur_dir <- cur_dir\name
        jc      ad_subnext              ; too long to name: skip it,
                                        ; having already said so
        push    ebx
        call    at_dir
        pop     ebx
        pushfd
        call    at_popdir
        popfd
        jc      ad_subfail
ad_subnext:
        inc     ebx
        jmp     short ad_sub

ad_subdone:
        pop     eax                     ; give the pool back
        mov     [pool_n],eax
        pop     eax
        mov     [txt_next],eax

;--- and now the files here -------------------------------------------
ad_files:
        call    at_files
        jc      ad_bad

ad_ok:
        dec     dword ptr [depth]
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

ad_subfail:
        pop     eax                     ; the pool, before leaving
        mov     [pool_n],eax
        pop     eax
        mov     [txt_next],eax
        jmp     short ad_bad

ad_full:
        pop     eax
        mov     [pool_n],eax
        pop     eax
        mov     [txt_next],eax
        mov     edx,OFFSET msg_pool
        call    puts
        jmp     short ad_bad

ad_deep:
        ; Not a limit anyone will reach by accident: the kernel refuses
        ; to NAME a path over 260 characters long before a tree gets
        ; thirty-two deep.  It is here so that a directory loop on a
        ; damaged disk stops rather than running the stack out.
        mov     edx,OFFSET msg_deep
        call    puts
ad_bad:
        dec     dword ptr [depth]
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; at_collect - every subdirectory of [cur_dir] into the pool.  The
;              search is CLOSED before this returns; see the header.
;              CF set if the pool filled, which is an error and not a
;              signal: unlike MOVE, nothing ATTRIB does removes a name
;              from a directory, so a second pass would find the same
;              names again and again.
;=====================================================================
at_collect:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     esi,OFFSET cur_dir
        mov     edx,OFFSET str_allfiles
        mov     edi,OFFSET work
        call    at_join
        jc      ac_none                 ; cannot even name the pattern

        cmp     byte ptr [use_lfn],0
        je      ac_short

;--- the long-name find -----------------------------------------------
        mov     edx,OFFSET work
        mov     edi,OFFSET find_rec
        mov     ecx,SRCH_ALL            ; CL allowable, CH required
        mov     eax,714Eh
        int     21h
        jc      ac_none
        movzx   ebx,ax                  ; the find handle
ac_llp:
        mov     esi,OFFSET find_rec + FDR_LONG
        mov     eax,dword ptr [find_rec + FDR_ATTR]
        call    at_take
        jc      ac_lclose               ; the pool is full
        push    ebx
        mov     edi,OFFSET find_rec
        mov     eax,714Fh
        int     21h
        pop     ebx
        jnc     ac_llp
        mov     eax,71A1h
        int     21h
        jmp     ac_none
ac_lclose:
        mov     eax,71A1h
        int     21h
        jmp     ac_full

;--- and the 8.3 one, for a machine that has said it has no others ----
ac_short:
        mov     edx,OFFSET work
        mov     ecx,SRCH_ALL
        mov     eax,4E00h
        int     21h
        jc      ac_none
ac_slp:
        mov     esi,OFFSET dta + DTA_NAME
        movzx   eax,byte ptr [dta + DTA_ATTR]
        call    at_take
        jc      ac_full                 ; 4Eh has no handle to close -
                                        ; its state is the DTA
        mov     eax,4F00h
        int     21h
        jnc     ac_slp

ac_none:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
ac_full:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; at_take - ESI -> a name, EAX = its attributes.  Into the pool if it
;           is a subdirectory worth descending.  CF set when the pool
;           is full.
;---------------------------------------------------------------------
at_take:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        test    al,ATTR_DIR
        jz      at_skip
        test    al,ATTR_VOLUME          ; a label is not a directory,
        jnz     at_skip                 ; and an LFN fragment is neither

        ; "." and ".." are this directory and its parent, and a find
        ; returns both.  Descending into either is how a recursion
        ; never ends.
        cmp     byte ptr [esi],'.'
        jne     at_keep
        cmp     byte ptr [esi+1],0
        je      at_skip
        cmp     byte ptr [esi+1],'.'
        jne     at_keep
        cmp     byte ptr [esi+2],0
        je      at_skip

at_keep:
        mov     eax,[pool_n]
        cmp     eax,POOL_PTRS
        jae     at_tfull
        call    at_len                  ; ECX = length, ESI unchanged
        mov     edi,[txt_next]
        mov     edx,[pool_txt]
        add     edx,POOL_TEXT
        sub     edx,edi                 ; EDX = room left in the text
        cmp     ecx,edx
        jae     at_tfull                ; ">=": the NUL needs a byte too

        mov     edx,[pool_ptr]          ; remember where it goes
        mov     [edx + eax*4],edi
        inc     dword ptr [pool_n]
        inc     ecx                     ; copy the NUL as well
        rep     movsb
        mov     [txt_next],edi
at_skip:
        clc
        jmp     short at_tout
at_tfull:
        stc
at_tout:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; at_files - the files in [cur_dir] that match [spec_name].  Acted on
;            while the search is open, which the header argues is safe
;            here and was not safe in MOVE.
;            CF set if something went wrong and the sweep should stop.
;=====================================================================
at_files:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     esi,OFFSET cur_dir
        mov     edx,OFFSET spec_name
        mov     edi,OFFSET work
        call    at_join
        jc      af_none

        cmp     byte ptr [use_lfn],0
        je      af_short

;--- the long-name find -----------------------------------------------
        mov     edx,OFFSET work
        mov     edi,OFFSET find_rec
        mov     ecx,SRCH_ALL
        mov     eax,714Eh
        int     21h
        jc      af_ffail
        movzx   ebx,ax
af_llp:
        mov     esi,OFFSET find_rec + FDR_LONG
        mov     eax,dword ptr [find_rec + FDR_ATTR]
        call    at_visit
        jc      af_lclose
        push    ebx
        mov     edi,OFFSET find_rec
        mov     eax,714Fh
        int     21h
        pop     ebx
        jnc     af_llp
        mov     eax,71A1h
        int     21h
        jmp     af_none
af_lclose:
        mov     eax,71A1h
        int     21h
        jmp     af_bad

;--- and the 8.3 one --------------------------------------------------
af_short:
        mov     edx,OFFSET work
        mov     ecx,SRCH_ALL
        mov     eax,4E00h
        int     21h
        jc      af_ffail
af_slp:
        mov     esi,OFFSET dta + DTA_NAME
        movzx   eax,byte ptr [dta + DTA_ATTR]
        call    at_visit
        jc      af_bad
        mov     eax,4F00h
        int     21h
        jnc     af_slp

af_ffail:
        ; A find-first that fails is USUALLY just the empty answer -
        ; nothing matched the pattern - and that is not an error.
        ;
        ; IT IS NOT WHERE A MISTYPED DIRECTORY IS CAUGHT, and it
        ; cannot be: this kernel's 4Eh answers ERR_NOFILE for a
        ; directory that does not exist exactly as it does for a name
        ; that matched nothing, and tells only a bad DRIVE apart
        ; (f4E_nf in DOS\INT21.INC).  The first run of this program
        ; proved it - "ATTRIB \NOSUCHDIR\*.*" said "File not found" -
        ; and the check that fixed it is in at_qualify, where the
        ; directory can still be asked about directly.  The two codes
        ; below are what is left that a find CAN say.
        ;
        ; On a /S sweep this can only fire at the top anyway: every
        ; directory below it was handed over by a find that had just
        ; listed it.
        cmp     eax,ERR_NOPATH
        je      af_fsay
        cmp     eax,ERR_BADDRIVE
        jne     af_none
af_fsay:
        call    at_why
        mov     esi,OFFSET cur_dir
        call    putz
        call    crlf
        jmp     short af_bad

af_none:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
af_bad:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; at_visit - ESI -> a found name, EAX = its attributes.  Decide
;            whether it is ours, build its full path, and do it.
;            CF set to stop the sweep.
;---------------------------------------------------------------------
at_visit:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        test    al,ATTR_VOLUME
        jnz     av_skip                 ; a label has no attributes to set

        ; "." and ".." again.  DOS skipped ANY name beginning with a
        ; dot, and could do that safely because an 8.3 name cannot
        ; begin with one - a long name can, and a file called
        ; ".PROFILE" is a real file that ATTRIB should show, so the
        ; test here is the two names it actually means.
        cmp     byte ptr [esi],'.'
        jne     av_notdot
        cmp     byte ptr [esi+1],0
        je      av_skip
        cmp     byte ptr [esi+1],'.'
        jne     av_notdot
        cmp     byte ptr [esi+2],0
        je      av_skip
av_notdot:

        ; A subdirectory is touched only when the user named one
        ; outright - see the header.
        test    al,ATTR_DIR
        jz      av_mine
        cmp     byte ptr [wild],0
        jne     av_skip
av_mine:

        mov     edx,esi
        mov     esi,OFFSET cur_dir
        mov     edi,OFFSET file_path
        call    at_join
        jc      av_long

        mov     byte ptr [did_one],1
        call    at_do
        jc      av_stop
av_skip:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
av_stop:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

av_long:
        ; The path is longer than the kernel will name.  Say which one
        ; rather than stopping: on a /S sweep one unreachable corner
        ; of the tree is no reason to abandon the rest.
        mov     edx,OFFSET msg_toolong
        call    puts
        mov     esi,OFFSET file_path    ; at_join left the truncated path
        call    putz                    ; there, which shows where it got
        call    crlf
        jmp     short av_skip

;=====================================================================
; at_do - [file_path] is a file.  Show its attributes, or change them.
;         CF set to stop the sweep.
;=====================================================================
at_do:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     edx,OFFSET file_path
        mov     eax,4300h
        int     21h
        jc      ad_err
        movzx   eax,cl                  ; EAX = what it has now

        cmp     byte ptr [set_attr],0
        jne     ad_change

;--- showing ----------------------------------------------------------
        call    at_show
        jmp     short ad_done

;--- changing ---------------------------------------------------------
ad_change:
        ; The directory and volume bits are not this program's to set.
        ; The kernel puts them back regardless (attr_core), but they
        ; are cleared here as well so that the two tests below - and
        ; anything read off this value later - are about the four bits
        ; the command is actually made of.
        and     al,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_ARCHIVE

        ; A change that does not name the hidden bit does not touch a
        ; hidden file; same for system.  Hidden is tested first
        ; because DOS tests it first, and a file that is both gets the
        ; hidden message.
        test    al,ATTR_HIDDEN
        jz      ad_nothid
        mov     ah,[pmask]
        or      ah,[mmask]
        test    ah,ATTR_HIDDEN
        jnz     ad_nothid
        mov     edx,OFFSET msg_nohid
        jmp     short ad_refuse
ad_nothid:
        test    al,ATTR_SYSTEM
        jz      ad_set
        mov     ah,[pmask]
        or      ah,[mmask]
        test    ah,ATTR_SYSTEM
        jnz     ad_set
        mov     edx,OFFSET msg_nosys
ad_refuse:
        call    puts
        mov     esi,OFFSET file_path
        call    putz
        call    crlf
        jmp     short ad_done

ad_set:
        mov     ah,[mmask]              ; off first, then on: a bit in
        not     ah                      ; both masks is a syntax error
        and     al,ah                   ; the parser has already refused
        or      al,[pmask]
        movzx   ecx,al
        mov     edx,OFFSET file_path
        mov     eax,4301h
        int     21h
        jc      ad_err

ad_done:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret

ad_err:
        ; DOS stops the whole sweep on the first file it cannot do,
        ; rather than carrying on and reporting at the end, and this
        ; follows it: the common cause is a disk that has gone away or
        ; a share that has said no, and the next nine hundred files
        ; will fail the same way.  The path is named so it is clear
        ; where the sweep got to.
        call    at_why
        mov     esi,OFFSET file_path
        call    putz
        call    crlf
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; at_show - EAX = the attribute byte of [file_path].  One line of it.
;
; The sixteen columns are laid out at attr_str and only ever have
; characters poked into them, so their width cannot drift: DOS's
; bits[]/as[] table says 'A' at column 2, 'S' at 5, 'H' at 6 and 'R'
; at 7, counting from nought, and every other column in the sixteen is
; a space it reserved and never used.  See the header.
;=====================================================================
at_show:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     bl,al                   ; the attributes, out of the way
        mov     edi,OFFSET attr_str     ; sixteen spaces again - only the
        mov     ecx,16                  ; columns, never the four spaces
        mov     al,' '                  ; that follow them
        rep     stosb

        mov     al,bl
        test    al,ATTR_ARCHIVE
        jz      as_nota
        mov     byte ptr [attr_str+2],'A'
as_nota:
        test    al,ATTR_SYSTEM
        jz      as_nots
        mov     byte ptr [attr_str+5],'S'
as_nots:
        test    al,ATTR_HIDDEN
        jz      as_noth
        mov     byte ptr [attr_str+6],'H'
as_noth:
        test    al,ATTR_READONLY
        jz      as_notr
        mov     byte ptr [attr_str+7],'R'
as_notr:
        mov     edx,OFFSET attr_str     ; the sixteen and DOS's four
        call    puts
        mov     esi,OFFSET file_path
        call    putz
        call    crlf

        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; at_push  - cur_dir <- cur_dir + '\' + the name at ESI.
;            CF set (having said so) if it will not fit.
; at_popdir- and back again, to the length at the top of [dir_stack].
;
; ONE BUFFER AND A LENGTH, rather than a path per level: the descent
; is depth-first, so only one path is ever live, and a 260-byte buffer
; per level for thirty-two levels would be eight kilobytes of BSS to
; hold thirty-one copies of a prefix.
;=====================================================================
at_push:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     eax,[dir_sp]
        cmp     eax,DEPTH_MAX
        jae     apu_deep
        push    esi
        mov     esi,OFFSET cur_dir
        call    at_len                  ; ECX = the length to come back to
        pop     esi
        mov     [dir_stack + eax*4],ecx
        inc     dword ptr [dir_sp]

        mov     edx,esi
        mov     esi,OFFSET cur_dir
        mov     edi,OFFSET work
        call    at_join
        jc      apu_long
        mov     esi,OFFSET work
        mov     edi,OFFSET cur_dir
        call    at_copyz
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

apu_long:
        ; Named, and skipped.  THE LENGTH PUSHED ABOVE IS TAKEN BACK
        ; HERE, because the caller's error path jumps straight on to
        ; the next name without calling at_popdir - it has nothing to
        ; pop back to.  A push undone in one of two places is exactly
        ; how a stack ends up one deep for the rest of a run.
        dec     dword ptr [dir_sp]
        mov     edx,OFFSET msg_toolong
        call    puts
        mov     esi,OFFSET work         ; at_join left the truncated path
        call    putz                    ; there, which shows where the
        call    crlf                    ; name stopped fitting
        jmp     short apu_out
apu_deep:
        mov     edx,OFFSET msg_deep
        call    puts
apu_out:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

at_popdir:
        push    eax
        push    ecx
        cmp     dword ptr [dir_sp],0
        je      apd_out
        dec     dword ptr [dir_sp]
        mov     eax,[dir_sp]
        mov     ecx,[dir_stack + eax*4]
        mov     byte ptr [cur_dir + ecx],0
apd_out:
        pop     ecx
        pop     eax
        ret

;=====================================================================
; at_qualify - spec_dir, whatever shape it was written in, becomes the
;              absolute path in cur_dir.
;
; THE KERNEL DOES IT.  7160h with CL=2 renders a path from the volume
; root with each component spelt the way the disk spells it, and 60h
; does the same in 8.3 on a machine with LFN=OFF.  Both accept the
; shapes that matter here without any help: an empty string is the
; current directory, "D:" is D:'s current directory, "\" is a root and
; a trailing separator is harmless, because resolve_core folds "." and
; ".." and drops empty components before either of them renders
; anything.  Neither leaves a trailing backslash except on a root,
; which is exactly what at_join wants.
;=====================================================================
at_qualify:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     esi,OFFSET spec_dir
        mov     edi,OFFSET cur_dir
        cmp     byte ptr [use_lfn],0
        je      aq_short
        mov     ecx,2                   ; the long form
        mov     eax,7160h
        int     21h
        jc      aq_bad
        jmp     short aq_exists
aq_short:
        mov     eax,6000h
        int     21h
        jc      aq_bad

;--- ...AND IT HAS TO BE THERE, which a truename cannot say -----------
; 7160h and 60h CANONICALIZE; they do not look.  Without this,
; "ATTRIB \NOSUCHDIR\*.*" comes back saying it could not find the
; FILES - because the kernel's 4Eh answers ERR_NOFILE for a directory
; that does not exist just as it does for a name that matched nothing,
; and tells only a bad DRIVE apart (f4E_nf in DOS\INT21.INC).  The
; find can therefore never distinguish the two, so the question is
; asked HERE, while the answer is still about the directory.  This is
; what makes ATTRIB's two refusals different sentences, as they are on
; real DOS.
;
; A BARE ROOT IS EXEMPT AND HAS TO BE: "C:\" has no last component for
; 43h to look up, so 43h calls it "not found" and a perfectly good
; root would be refused by the very check meant to catch typos.
aq_exists:
        mov     esi,OFFSET cur_dir
        call    at_len                  ; ECX = its length, ESI kept
        cmp     ecx,3                   ; "C:\" and nothing shorter
        jbe     aq_ok
        mov     edx,OFFSET cur_dir
        mov     eax,4300h
        int     21h
        jc      aq_bad
        test    cl,ATTR_DIR             ; a FILE named as a directory is
        jz      aq_bad                  ; the same mistake

aq_ok:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
aq_bad:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; at_lfnprobe - is the long-name API here at all?
;
; One question asked once.  71A0h is the cheapest of the family and
; answers AX = 7100h with carry when LFN=OFF in CONFIG.SYS takes the
; whole family away, which is the documented signature every LFN-aware
; program tests.  The answer decides three things here and not one:
; which find to use, which name to build paths from, and which of
; 7160h/60h qualifies the starting directory.
;=====================================================================
at_lfnprobe:
        push    eax
        push    ecx
        push    edi
        mov     byte ptr [use_lfn],0
        mov     edi,OFFSET fs_name
        mov     ecx,32
        mov     eax,71A0h
        int     21h
        jc      al_out
        mov     byte ptr [use_lfn],1
al_out:
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; at_parse - take the command tail apart.  CF set if it is nonsense.
;
; '-' IS NOT A SWITCH CHARACTER HERE, and that is the one place ATTRIB
; differs from every other command in this directory.  MOUNT takes
; "-R" as it takes "/R"; ATTRIB cannot, because "-R" is the command's
; whole point.  So a token beginning with '+' or '-' is an attribute
; and a token beginning with '/' is a switch, and neither spelling
; reaches the other.
;=====================================================================
at_parse:
        push    eax
        push    ebx
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
        jbe     ap_len
        mov     ecx,TAIL_MAX - 1
ap_len:
        mov     edi,OFFSET line
        jecxz   ap_lend
ap_lcp:
        mov     al,[esi]
        cmp     al,0Dh
        je      ap_lend
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     ap_lcp
ap_lend:
        mov     byte ptr [edi],0

        mov     esi,OFFSET line

ap_tok:
        mov     al,[esi]
        or      al,al
        jz      ap_done
        cmp     al,' '
        je      ap_skip
        cmp     al,9
        je      ap_skip
        cmp     al,','
        je      ap_skip
        cmp     al,'/'
        je      ap_sw
        cmp     al,'+'
        je      ap_attr
        cmp     al,'-'
        je      ap_attr
        jmp     ap_item
ap_skip:
        inc     esi
        jmp     short ap_tok

;--- +R -R +A -A +S -S +H -H ------------------------------------------
ap_attr:
        mov     ah,0                    ; AH = which mask it goes in
        cmp     al,'-'
        jne     ap_aplus
        mov     ah,1
ap_aplus:
        inc     esi
        mov     al,[esi]
        or      al,al
        jz      ap_bad
        inc     esi

        ; ...and NOTHING after it.  "+RA" is not two attributes, it is
        ; a typo, and reading it as two would be inventing a syntax
        ; that no DOS has: what works here has to work there.
        mov     bl,[esi]
        or      bl,bl
        jz      ap_alone
        cmp     bl,' '
        je      ap_alone
        cmp     bl,9
        je      ap_alone
        cmp     bl,','
        je      ap_alone
        jmp     ap_bad
ap_alone:
        cmp     al,'a'
        jb      ap_aup
        cmp     al,'z'
        ja      ap_aup
        sub     al,20h
ap_aup:
        mov     bl,ATTR_READONLY
        cmp     al,'R'
        je      ap_abit
        mov     bl,ATTR_ARCHIVE
        cmp     al,'A'
        je      ap_abit
        mov     bl,ATTR_SYSTEM
        cmp     al,'S'
        je      ap_abit
        mov     bl,ATTR_HIDDEN
        cmp     al,'H'
        jne     ap_bad

ap_abit:
        ; SAID TWICE IS A SYNTAX ERROR, both ways round: "+R +R" is a
        ; line the user did not mean to type, and "+R -R" is a line
        ; with no answer.  DOS refuses both (Check_parms in ATTRIB.C)
        ; and refusing is better than picking one, because the two
        ; plausible rules - first wins, last wins - disagree.
        or      ah,ah
        jnz     ap_aminus
        test    [pmask],bl
        jnz     ap_bad
        test    [mmask],bl
        jnz     ap_bad
        or      [pmask],bl
        jmp     short ap_aset
ap_aminus:
        test    [mmask],bl
        jnz     ap_bad
        test    [pmask],bl
        jnz     ap_bad
        or      [mmask],bl
ap_aset:
        mov     byte ptr [set_attr],1
        jmp     ap_tok

;--- /S and /? --------------------------------------------------------
ap_sw:
        inc     esi
        mov     al,[esi]
        or      al,al
        jz      ap_bad
        inc     esi
        cmp     al,'a'
        jb      ap_swu
        cmp     al,'z'
        ja      ap_swu
        sub     al,20h
ap_swu:
        cmp     al,'S'
        jne     ap_sw2
        mov     byte ptr [opt_sub],1
        jmp     ap_tok
ap_sw2:
        cmp     al,'?'
        jne     ap_bad
        mov     byte ptr [opt_help],1
        jmp     ap_tok

;--- the filespec, quoted or not --------------------------------------
; Quoted because this kernel has long names and a long name may have a
; space in it, and "ATTRIB +R "Quarterly Report.txt"" is otherwise two
; filespecs, which is an error rather than a file.
ap_item:
        cmp     byte ptr [got_spec],0
        jne     ap_bad                  ; two filespecs is not a question
                                        ; this command knows how to answer
        xor     ah,ah                   ; AH = inside quotes
        cmp     byte ptr [esi],'"'
        jne     ap_ihave
        mov     ah,1
        inc     esi
ap_ihave:
        mov     edi,OFFSET spec
        xor     ecx,ecx
ap_ich:
        mov     al,[esi]
        or      al,al
        jz      ap_iend
        cmp     al,'"'
        je      ap_iquote
        or      ah,ah
        jnz     ap_istep                ; quoted: blanks belong to it
        cmp     al,' '
        je      ap_iend
        cmp     al,9
        je      ap_iend
        cmp     al,','
        je      ap_iend
ap_istep:
        cmp     ecx,PATH_MAX - 1
        jae     ap_bad
        mov     [edi],al
        inc     edi
        inc     ecx
        inc     esi
        jmp     short ap_ich
ap_iquote:
        inc     esi                     ; the closing quote ends it
ap_iend:
        mov     byte ptr [edi],0
        mov     byte ptr [got_spec],1
        jmp     ap_tok

ap_done:
        clc
        jmp     short ap_pout
ap_bad:
        stc
ap_pout:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; at_split - [spec] -> spec_dir + spec_name.
;
; The separator is KEPT with the directory, and a drive letter counts
; as one: "D:*.C" means D:'s current directory and "D:\*.C" means its
; root, and the two must not be allowed to turn into each other.  Same
; split, and the same reason for it, as MOVE's mv_split.
;
; A spec with no name at all - "", "D:", "\DOS\" - means "*.*", which
; is what makes bare ATTRIB list the current directory and is DOS's
; own rule (two separate cases in Check_parms; one here, because the
; empty name is the same thing in both).
;=====================================================================
at_split:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi

        ; No filespec at all IS "*.*", and it becomes "*.*" HERE rather
        ; than further down, so that everything after this - the split,
        ; the wildcard rule, and the name the "file not found" message
        ; quotes back - sees the same string whether the user typed it
        ; or not.  DOS does it in the same place and for the same
        ; reason (Check_parms, before Make_fspec copies the name it
        ; will report).
        cmp     byte ptr [got_spec],0
        jne     as_have
        mov     esi,OFFSET str_allfiles
        mov     edi,OFFSET spec
        call    at_copyz
as_have:

        mov     esi,OFFSET spec
        xor     edx,edx                 ; EDX = characters before the name
        xor     ecx,ecx
as_lp:
        mov     al,[esi+ecx]
        or      al,al
        jz      as_end
        inc     ecx
        cmp     al,'\'
        je      as_mark
        cmp     al,'/'
        je      as_mark
        cmp     al,':'
        jne     as_lp
as_mark:
        mov     edx,ecx
        jmp     short as_lp
as_end:
        push    ecx                     ; the whole length
        mov     edi,OFFSET spec_dir
        mov     ecx,edx
        jecxz   as_dterm
        rep     movsb
as_dterm:
        mov     byte ptr [edi],0

        pop     ecx                     ; the name is what is left
        sub     ecx,edx
        mov     esi,OFFSET spec
        add     esi,edx
        mov     edi,OFFSET spec_name
        jecxz   as_noname
        rep     movsb
        mov     byte ptr [edi],0
        jmp     short as_sout
as_noname:
        mov     esi,OFFSET str_allfiles
        call    at_copyz

as_sout:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; Path and string helpers.  Every one of them leaves every register it
; is not documented to return exactly as it found it - said out loud
; because a helper that quietly eats a register is the bug this kernel
; has caught more often than any other.
;=====================================================================

;--- at_join - ESI -> a directory, EDX -> a name, EDI -> the answer.
; CF set if it will not fit in PATH_MAX.  One separator, never two,
; and none at all after a bare "D:" or a root's backslash.
at_join:
        push    eax
        push    ecx
        push    esi
        push    edi
        xor     ecx,ecx                 ; ECX = what has been written
aj_dir:
        mov     al,[esi]
        or      al,al
        jz      aj_sep
        cmp     ecx,PATH_MAX - 2
        jae     aj_full
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        jmp     short aj_dir
aj_sep:
        jecxz   aj_name                 ; no directory at all
        mov     al,[edi-1]
        cmp     al,'\'
        je      aj_name
        cmp     al,'/'
        je      aj_name
        cmp     al,':'
        je      aj_name
        cmp     ecx,PATH_MAX - 2
        jae     aj_full
        mov     byte ptr [edi],'\'
        inc     edi
        inc     ecx
aj_name:
        mov     esi,edx
aj_ncp:
        mov     al,[esi]
        or      al,al
        jz      aj_end
        cmp     ecx,PATH_MAX - 2
        jae     aj_full
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        jmp     short aj_ncp
aj_end:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret
aj_full:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;--- at_copyz - ESI -> ASCIIZ, EDI -> a PATH_MAX buffer
at_copyz:
        push    eax
        push    ecx
        push    esi
        push    edi
        xor     ecx,ecx
az_lp:
        mov     al,[esi]
        or      al,al
        jz      az_end
        cmp     ecx,PATH_MAX - 1
        jae     az_end
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        jmp     short az_lp
az_end:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        ret

;--- at_len - ECX = the length of the ASCIIZ at ESI, ESI kept
at_len:
        push    esi
        xor     ecx,ecx
al_lp:
        cmp     byte ptr [esi],0
        je      al_lout
        inc     esi
        inc     ecx
        jmp     short al_lp
al_lout:
        pop     esi
        ret

;--- at_haswild - CF set if the ASCIIZ at ESI holds a * or a ?
at_haswild:
        push    eax
        push    esi
aw_lp:
        mov     al,[esi]
        or      al,al
        jz      aw_no
        cmp     al,'*'
        je      aw_yes
        cmp     al,'?'
        je      aw_yes
        inc     esi
        jmp     short aw_lp
aw_no:
        pop     esi
        pop     eax
        clc
        ret
aw_yes:
        pop     esi
        pop     eax
        stc
        ret

;=====================================================================
; at_why - EAX = a DOS error; say what it means HERE, leaving the
;          caller to print the path after it.  The generic words are
;          not wrong, they are just not useful: "access denied" is
;          true of a read-only file, of a file somebody has open and
;          of a disk with the tab off, and the reader has to be told
;          which of those ATTRIB just met.
;=====================================================================
at_why:
        push    eax
        push    edx
        cmp     eax,ERR_NOFILE
        jne     aw_e3
        mov     edx,OFFSET msg_nofile
        jmp     short aw_say
aw_e3:
        cmp     eax,ERR_NOPATH
        jne     aw_e5
        mov     edx,OFFSET msg_nopath
        jmp     short aw_say
aw_e5:
        cmp     eax,ERR_ACCESS
        jne     aw_e15
        mov     edx,OFFSET msg_denied
        jmp     short aw_say
aw_e15:
        cmp     eax,ERR_BADDRIVE
        jne     aw_num
        mov     edx,OFFSET msg_baddrv
aw_say:
        call    puts
        pop     edx
        pop     eax
        ret
aw_num:
        mov     edx,OFFSET msg_enum
        call    puts
        call    putdec
        mov     edx,OFFSET msg_dash
        call    puts
        pop     edx
        pop     eax
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
;--- goes through here, and none of them goes through puts.
putz:
        push    eax
        push    esi
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

;=====================================================================
; State
;=====================================================================
pmask       db  0                       ; bits being turned on
mmask       db  0                       ; and off
set_attr    db  0                       ; either of them was asked for
opt_sub     db  0                       ; /S
opt_help    db  0                       ; /?
got_spec    db  0
wild        db  0                       ; leave subdirectories alone
use_lfn     db  0
did_one     db  0                       ; anything matched at all
            db  0,0,0

depth       dd  0                       ; levels of at_dir below the top
dir_sp      dd  0                       ; and the lengths to come back to
pool_txt    dd  0                       ; the BSS, carved up at entry
pool_ptr    dd  0
txt_next    dd  0
pool_n      dd  0

str_allfiles db '*.*',0

attr_str    db  '                    $' ; the sixteen columns and the
                                        ; four spaces after them, in one
                                        ; string - see at_show

line        db  TAIL_MAX dup(0)
spec        db  PATH_MAX dup(0)
spec_dir    db  PATH_MAX dup(0)
spec_name   db  PATH_MAX dup(0)
cur_dir     db  PATH_MAX dup(0)
file_path   db  PATH_MAX dup(0)
work        db  PATH_MAX dup(0)
dir_stack   dd  DEPTH_MAX dup(0)
dta         db  DTA_LEN dup(0)
find_rec    db  FDR_LEN dup(0)
fs_name     db  36 dup(0)
num_buf     db  16 dup(0)

msg_nofile  db  'File not found - $'
msg_nopath  db  'Path not found - $'
msg_denied  db  'Access denied - $'
msg_baddrv  db  'Invalid drive - $'
msg_enum    db  'Error $'
msg_dash    db  ' - $'
msg_nohid   db  'Not resetting hidden file $'
msg_nosys   db  'Not resetting system file $'
msg_toolong db  'Path too long to name - $'
msg_deep    db  'Directory tree too deep to search',13,10,'$'
msg_pool    db  'Too many subdirectories on one path to search',13,10,'$'

msg_usage   db  'Displays or changes file attributes.',13,10,13,10
            db  'ATTRIB [+R | -R] [+A | -A] [+S | -S] [+H | -H] '
            db  '[drive:][path][filename] [/S]',13,10,13,10
            db  '  +   Sets an attribute.',13,10
            db  '  -   Clears an attribute.',13,10
            db  '  R   Read-only file attribute.',13,10
            db  '  A   Archive file attribute.',13,10
            db  '  S   System file attribute.',13,10
            db  '  H   Hidden file attribute.',13,10
            db  '  [drive:][path][filename]',13,10
            db  '      Specifies file(s) or directory for ATTRIB to process.',13,10
            db  '  /S  Processes files in all directories in the specified path.',13,10,'$'

img_end:                                ; the BSS begins here

_ATTR   ENDS
        END
