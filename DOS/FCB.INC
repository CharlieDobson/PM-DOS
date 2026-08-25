;=====================================================================
; FCB.INC - the file control block API: 0Fh-17h, 21h-24h, 27h-29h
;
; The oldest half of the DOS file interface, and the last one PM-DOS
; did not have.  A program written before 1983 - and a surprising
; number written long after, because the API never went away - does
; not know what a handle is: it hands DOS a 37-byte structure of its
; own, DOS fills the rest of it in, and every later call finds the
; file by being given that same structure back.
;
; THE WHOLE DIFFICULTY IS THAT THE STRUCTURE IS THE CALLER'S.  A
; handle is a number the kernel chose and can look up in a table it
; owns; an FCB is memory a program allocated, may copy, may hand to a
; child, may forget about entirely, and will certainly still be
; holding when the kernel has long since given the table entry to
; somebody else.  So the kernel cannot simply put a pointer in it and
; trust what comes back.  What it puts there instead is an INDEX AND
; A CLAIM ID (see SFT_STAMP in PMDOS.INC), and when the id does not
; match, the file is opened again from the name the FCB is still
; carrying.  That is not a workaround for having a small table - real
; DOS does exactly the same thing, and its FCBS= statement exists to
; size the pool this recycling runs over.  A program that opens forty
; FCBs and closes none of them is not misbehaving; it is using the
; API the way the API was meant to be used, and a kernel that refuses
; the thirty-third is a kernel it does not run on.
;
; WHAT IS DELIBERATELY NOT HERE:
;
;   - A read from an FCB open on CON gives raw characters rather than
;     an edited line.  The handle API's 3Fh has the line editor and
;     this does not; a CP/M-era program reading the console through
;     an FCB gets what INT 21h/07h would give it.  Stated because it
;     is a choice, not an oversight - the editor is a hundred lines
;     inside 3Fh's console branch and nothing has ever asked for it
;     through this door.
;
;   - The 05h-means-E5h name convention is honoured where it matters
;     (a search compares the FCB's bytes against the directory's, and
;     both hold 05h, so they match) and NOT where it does not: 0Fh and
;     16h render the FCB back into a path for the resolver, and a name
;     whose first byte is 05h renders as a control character the path
;     parser will refuse.  A file whose real name begins with E5h can
;     therefore be found and deleted through an FCB but not opened
;     through one.  It costs a line to say so and would cost a
;     special case in the resolver to fix, for a filename nobody has.
;
; Included by DOSMAIN.ASM LAST, after RING3.INC: fcb_room asks how big
; the running program's slot is, and r3_base is declared in there.
;=====================================================================

; Where a rename FCB keeps the second name.  There is no separate
; structure for it - 17h simply reads eleven more bytes out of the
; middle of the FCB, at the offset the file size and the date would
; be in an open one.
FCB_NEWNAME     EQU     11h

; The search continuation record 11h writes into the caller's FCB, on
; top of the name it was just given.  REAL DOS DOES THIS TOO and it is
; not tidiness: 12h is handed the FCB and nothing else, so the only
; place the state can live is inside it.  A program that keeps its
; pattern somewhere else and re-fills the FCB between calls is a
; program that searches the first file for ever, on any DOS.
FSC_DRV         EQU     01h             ; db - the drive being searched
FSC_PAT         EQU     02h             ; 11 bytes - the pattern
FSC_IDX         EQU     0Dh             ; dw - the next entry to look at
FSC_ATTR        EQU     0Fh             ; db - the search attribute
FSC_ROOT        EQU     10h             ; db - directory context: is it the root
FSC_START       EQU     11h             ; dd - directory context: where it starts

;=====================================================================
; 0Fh - open a file (or a device) through an FCB
;
; There is no access mode in an FCB, so everything is opened for both
; reading and writing, which is what DOS has always done here.
;=====================================================================
fn0F:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_mkpath
        jc      f0F_bad
        mov     eax,2                   ; read and write
        call    fcb_openx               ; -> AX = index, ESI = the entry
        jc      f0F_bad
        movzx   ebx,ax
        mov     edi,esi
        call    fcb_adopt               ; the FCB remembers which entry...
        call    fcb_fill                ; ...and what is in the file
        jmp     fcb_ok
f0F_bad:
        jmp     fcb_fail

;=====================================================================
; 10h - close an FCB
;
; The close has to find the file even when the entry it was open on
; has since been recycled, because closing is exactly when the size
; and the timestamp are written back.  fcb_bind will re-open it if
; that has happened, which costs a directory search and gets the
; right answer instead of a fast wrong one.
;=====================================================================
fn10:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_bind
        jc      f10_bad
        mov     esi,edi
        call    sft_close
        call    fcb_unbind
        jmp     fcb_ok
f10_bad:
        jmp     fcb_fail

;=====================================================================
; 11h - find first, 12h - find next
;
; The answer goes to the DTA as an UNOPENED FCB: the drive byte and
; then the directory entry itself, all thirty-two bytes of it, which
; is why a program can hand the result straight back to 0Fh.  An
; extended FCB in, an extended FCB out - seven bytes of hat first, and
; the search attribute is what makes a volume label or a directory
; findable at all.
;=====================================================================
fn11:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_setdir              ; the drive's current directory
        jc      f11_none
        call    fcb_pat                 ; fq_name = the pattern
        mov     al,[fq_attr]
        or      al,SRCH_FILTER          ; a DIRECTORY search: hidden and
        mov     [srch_attr],al          ; system stay out unless asked for
        xor     ebx,ebx                 ; from the top
        jmp     short f11_go

fn12:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        mov     esi,[fq_ptr]
        movzx   eax,byte ptr [esi+FSC_DRV]
        mov     [fq_drv],al
        dec     eax                     ; the record keeps it 1-based
        call    vol_select
        jc      f11_none

        push    esi                     ; the pattern 11h left behind
        add     esi,FSC_PAT
        mov     edi,OFFSET fq_name
        mov     ecx,11
        rep     movsb
        pop     esi

        mov     al,[esi+FSC_ATTR]
        mov     [srch_attr],al
        mov     al,[esi+FSC_ROOT]       ; and the directory it was in
        mov     edx,[esi+FSC_START]
        call    ctx_restore
        movzx   ebx,word ptr [esi+FSC_IDX]

f11_go:
        mov     dword ptr [srch_long],0 ; an FCB cannot spell a long name,
        mov     byte ptr [srch_lonly],0 ; so this search is pure 8.3
        mov     edi,OFFSET fq_name
        call    fat_search
        jc      f11_none

        ; ESI -> the live entry inside the sector cache and EBX is its
        ; index.  Copy it out before anything else touches the disk.
        push    ebx
        mov     edi,OFFSET fq_ent
        mov     ecx,32
        rep     movsb
        pop     ebx
        inc     ebx                     ; where find-next picks up

        ; the continuation record, into the caller's FCB
        mov     esi,[fq_ptr]
        mov     al,[fq_drv]
        mov     [esi+FSC_DRV],al
        mov     [esi+FSC_IDX],bx
        mov     al,[srch_attr]
        mov     [esi+FSC_ATTR],al
        call    ctx_save                ; AL = is-root, EDX = where
        mov     [esi+FSC_ROOT],al
        mov     [esi+FSC_START],edx
        push    esi
        mov     edi,esi
        add     edi,FSC_PAT
        mov     esi,OFFSET fq_name
        mov     ecx,11
        rep     movsb
        pop     esi

        ; and the answer, into the DTA
        mov     edi,[dta_ptr]
        cmp     byte ptr [fq_isx],0
        je      f11_plain
        mov     byte ptr [edi],FCBX_SIG
        mov     dword ptr [edi+1],0     ; five unused bytes...
        mov     byte ptr [edi+5],0
        mov     al,[fq_ent+DE_ATTR]     ; ...then what was actually found
        mov     [edi+FCBX_ATTR],al
        add     edi,FCBX_LEN
f11_plain:
        mov     al,[fq_drv]
        mov     [edi],al
        inc     edi
        mov     esi,OFFSET fq_ent
        mov     ecx,32
        rep     movsb
        jmp     fcb_ok

f11_none:
        mov     ax,ERR_NOFILE
        jmp     fcb_fail

;=====================================================================
; 13h - delete every file matching the FCB
;
; The FCB API's delete takes wildcards where the handle API's 41h
; refuses them, and that is not an inconsistency: 41h is the newer
; call and DOS moved the expansion out into the shell.  Here it has to
; happen in the kernel, because the program passing "*.BAK" has no
; find-first to expand it with.
;=====================================================================
fn13:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_setdir
        jc      f13_none
        call    fcb_pat
        mov     al,[fq_attr]
        or      al,SRCH_FILTER
        mov     [srch_attr],al
        mov     byte ptr [fq_hit],0
        xor     ebx,ebx
f13_lp:
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        mov     edi,OFFSET fq_name
        call    fat_search
        jc      f13_end

        ; THE CHECKS COME BEFORE THE TOMBSTONING, and 41h has them the
        ; other way round: it calls lfn_tomb and only then discovers
        ; the entry is a directory or read-only, by which time the long
        ; name is already gone from a file that is still there.  Same
        ; two calls, opposite order, and only one of the orders can be
        ; undone.
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jnz     f13_next
        test    byte ptr [esi+DE_ATTR],ATTR_READONLY
        jnz     f13_next

        call    lfn_tomb                ; the long name dies with the file
        call    dir_seek                ; and lfn_tomb moved the cache
        jc      f13_end
        call    de_getclus
        call    fat_freechain
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
        jc      f13_end
        call    fat_flush
        jc      f13_end
        mov     dword ptr [clusbuf_clus],-1     ; cached data now stale
        mov     byte ptr [fq_hit],1
f13_next:
        inc     ebx                     ; the entry is E5h now, so the scan
        jmp     short f13_lp            ; can only move forwards
f13_end:
        cmp     byte ptr [fq_hit],0
        je      f13_none
        jmp     fcb_ok
f13_none:
        mov     ax,ERR_NOFILE
        jmp     fcb_fail

;=====================================================================
; 14h - sequential read       15h - sequential write
; 21h - random read           22h - random write
; 27h - random block read     28h - random block write
;
; Six entry points, one engine, and the differences between them are
; three bits: where the position comes from, which way the bytes go,
; and whether CX holds a record count.  Real DOS merged them the same
; way for the same reason - the arithmetic around the transfer is far
; longer than the transfer, and six copies of it would be six places
; for the extent/record bookkeeping to drift.
;=====================================================================
fn14:
        mov     al,FIOP_READ
        jmp     short fcb_io
fn15:
        mov     al,0
        jmp     short fcb_io
fn21:
        mov     al,FIOP_READ OR FIOP_RANDOM
        jmp     short fcb_io
fn22:
        mov     al,FIOP_RANDOM
        jmp     short fcb_io
fn27:
        mov     al,FIOP_READ OR FIOP_RANDOM OR FIOP_BLOCK
        jmp     short fcb_io
fn28:
        mov     al,FIOP_RANDOM OR FIOP_BLOCK
        jmp     short fcb_io

;---------------------------------------------------------------------
; fcb_io - the whole of FCB record I/O.
;
; The order of operations below is real DOS's, step for step, because
; the order is observable: the extent and record fields are written
; back BEFORE the transfer as well as after it, a random call leaves
; them pointing at the record it just did, a block call also updates
; the random record field, and a write stamps the FCB with the time
; whether or not anything reached the disk.  Programs depend on all
; four - they are how a program that mixes sequential and random calls
; on the same FCB knows where it is.
;---------------------------------------------------------------------
fcb_io:
        mov     [fq_op],al
        mov     byte ptr [fq_err],FIOR_OK
        mov     dword ptr [fq_sft],0
        mov     edx,[ebp+F_EDX]
        call    fcb_get

        mov     ecx,1                   ; one record unless CX says more
        test    byte ptr [fq_op],FIOP_BLOCK
        jz      fio_one
        movzx   ecx,word ptr [ebp+F_ECX]
fio_one:
        mov     [fq_cnt],ecx

        call    fcb_recsz               ; EBX = record size, never zero
        mov     [fq_recsz],ebx

        call    fcb_getext              ; where a sequential call is
        test    byte ptr [fq_op],FIOP_RANDOM
        jz      fio_havepos
        call    fcb_getrr               ; where a random one says to go
fio_havepos:
        mov     [fq_recpos],eax
        call    fcb_setext              ; DOS writes it back here, before
                                        ; the transfer, for all six calls

        mul     ebx                     ; EDX:EAX = the byte position
        or      edx,edx
        jnz     fio_dead                ; past 4GB: there is no such byte
        mov     [fq_bpos],eax

        mov     eax,[fq_cnt]            ; and how many bytes are wanted
        mul     ebx
        or      edx,edx
        jz      fio_fits
        mov     eax,-1                  ; more than 4GB asked for; the trim
fio_fits:                               ; below is about to cut it down
        mov     [fq_bytes],eax

        ; WILL IT FIT WHERE THE DTA IS?  Real DOS asks whether the
        ; transfer would run off the end of the DTA's 64K segment and
        ; trims it to whole records if so, answering 2.  PM-DOS has no
        ; segment to run off, so the question becomes the one that
        ; actually matters here - whether it would run off the end of
        ; the program's own memory - plus the API's own 64K ceiling,
        ; which is kept because a 16-bit program asking for 100 records
        ; of 1000 bytes has already made an arithmetic mistake and DOS
        ; has always told it so.
        call    fcb_room                ; -> EAX, and EBX survives
        cmp     [fq_bytes],eax
        jbe     fio_go
        mov     byte ptr [fq_err],FIOR_TRIM
        xor     edx,edx
        div     ebx                     ; whole records only
        mov     [fq_cnt],eax
        mul     ebx
        mov     [fq_bytes],eax
fio_go:
        mov     dword ptr [fq_done],0
        cmp     dword ptr [fq_bytes],0
        jne     fio_bind
        cmp     byte ptr [fq_err],FIOR_TRIM
        je      fio_tail                ; no room for even one record, so
                                        ; the file is not touched at all

fio_bind:
        call    fcb_bind                ; -> EDI = the table entry
        jc      fio_dead
        mov     [fq_sft],edi
        mov     eax,[fq_bpos]
        mov     [edi+SFT_POS],eax

        test    dword ptr [edi+SFT_FLAGS],SFTF_DEVICE
        jnz     fio_dev

        mov     esi,edi
        mov     ecx,[fq_bytes]
        mov     edi,[dta_ptr]
        test    byte ptr [fq_op],FIOP_READ
        jz      fio_write
        call    file_read
        jc      fio_dead
        mov     [fq_done],edx
        jmp     short fio_after
fio_write:
        or      ecx,ecx
        jz      fio_wtrunc
        call    file_write
        jc      fio_dead
        mov     [fq_done],edx
        jmp     short fio_after

;--- 28h with CX = 0: the file becomes exactly as long as the random
;--- record field says.  The one call in this API that changes a file
;--- without moving a byte, and the only way a pre-handle program can
;--- truncate one at all.
fio_wtrunc:
        call    sft_seldrv
        jc      fio_dead
        call    file_trunc
        jmp     short fio_after

;--- a character device.  Position means nothing, end of file means
;--- nothing, and a device that gives back less than was asked for is
;--- not failing - it is just a device.
fio_dev:
        mov     esi,edi
        call    sft_dev                 ; DL = which one
        mov     ecx,[fq_bytes]
        mov     edi,[dta_ptr]
        xor     ebx,ebx
        test    byte ptr [fq_op],FIOP_READ
        jz      fio_dwr
fio_drd:
        cmp     ebx,ecx
        jae     fio_ddone
        call    dev_in
        jc      fio_ddone               ; end of input
        mov     [edi+ebx],al
        inc     ebx
        jmp     short fio_drd
fio_dwr:
        cmp     ebx,ecx
        jae     fio_ddone
        mov     al,[edi+ebx]
        call    dev_out
        inc     ebx
        jmp     short fio_dwr
fio_ddone:
        mov     [fq_done],ebx

fio_after:
        ; The size goes back into the FCB after every transfer, because
        ; the FCB is where it will still be when the table entry has
        ; been given to somebody else.
        mov     edi,[fq_sft]
        mov     esi,[fq_ptr]
        mov     eax,[edi+SFT_SIZE]
        mov     [esi+FCB_FILSIZ],eax
        test    byte ptr [fq_op],FIOP_READ
        jnz     fio_tail
        call    get_dostime             ; a write stamps the FCB, exactly as
        mov     [esi+FCB_FTIME],ax      ; the close will stamp the directory
        call    get_dosdate
        mov     [esi+FCB_FDATE],ax

fio_tail:
        mov     eax,[fq_done]
        xor     edx,edx
        div     dword ptr [fq_recsz]    ; EAX = whole records, EDX = a part
        mov     ecx,eax                 ; ECX = what to report in CX
        add     eax,[fq_recpos]
        mov     [fq_recpos],eax

        cmp     ecx,[fq_cnt]
        je      fio_whole
        test    byte ptr [fq_op],FIOP_READ
        jnz     fio_seteof              ; short read = end of file
        mov     edi,[fq_sft]            ; short write = disk full, unless
        or      edi,edi                 ; it was a device, where it never
        jz      fio_seteof              ; meant anything and 2.0 programs
        test    dword ptr [edi+SFT_FLAGS],SFTF_DEVICE    ; rely on that
        jnz     fio_whole
fio_seteof:
        mov     byte ptr [fq_err],FIOR_EOF
fio_whole:
        or      edx,edx
        jz      fio_setpos
        inc     dword ptr [fq_recpos]   ; a part-record still moves the
        test    byte ptr [fq_op],FIOP_READ      ; position past it
        jz      fio_setpos
        inc     ecx                     ; and on a read it counts as one
        mov     byte ptr [fq_err],FIOR_PART
        push    ecx                     ; the rest of the record is zeroed:
        mov     ecx,[fq_recsz]          ; a program reading the last record
        sub     ecx,edx                 ; of a file gets a record, not the
        mov     edi,[dta_ptr]           ; leftovers of whatever was in the
        add     edi,[fq_done]           ; DTA before
        xor     al,al
        rep     stosb
        pop     ecx

fio_setpos:
        mov     eax,[fq_recpos]
        test    byte ptr [fq_op],FIOP_RANDOM
        jz      fio_ext                 ; sequential: always
        test    byte ptr [fq_op],FIOP_BLOCK
        jz      fio_rr                  ; single random: leave them alone
fio_ext:
        mov     ebx,[fq_recsz]
        call    fcb_setext
fio_rr:
        test    byte ptr [fq_op],FIOP_BLOCK
        jz      fio_out
        mov     eax,[fq_recpos]
        mov     ebx,[fq_recsz]
        call    fcb_setrr
fio_out:
        test    byte ptr [fq_op],FIOP_BLOCK
        jz      fio_ret
        mov     word ptr [ebp+F_ECX],cx ; records actually transferred
fio_ret:
        mov     al,[fq_err]
        mov     byte ptr [ebp+F_EAX],al
        jmp     i21_ok

;--- the file could not be reached at all: no records, and the answer
;--- DOS gives for it, which is the same one it gives for end of file.
;--- A program that wants to know why asks 59h.
fio_dead:
        mov     byte ptr [fq_err],FIOR_EOF
        mov     dword ptr [fq_done],0
        xor     ecx,ecx
        jmp     short fio_out

;=====================================================================
; 16h - create a file through an FCB
;
; An existing file is truncated, which is what this call has always
; done and is why it is not the same as 0Fh.  An extended FCB's
; attribute byte comes along, so a program can make a hidden or
; system file the old way.
;=====================================================================
fn16:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_mkpath
        jc      f16_bad
        xor     ecx,ecx
        cmp     byte ptr [fq_isx],0
        je      f16_plain
        movzx   ecx,byte ptr [fq_attr]
f16_plain:
        mov     byte ptr [create_new],0
        call    fcb_createx
        jc      f16_bad
        movzx   ebx,ax
        mov     edi,esi
        call    fcb_adopt
        call    fcb_fill
        jmp     fcb_ok
f16_bad:
        jmp     fcb_fail

;=====================================================================
; 17h - rename every file matching the FCB
;
; The FCB carries both names: the pattern at the front and the new
; name eleven bytes further in.  A '?' in the NEW name means "keep
; whatever this file had here", which is how "REN *.TXT *.BAK" works
; when the shell hands the job down to this call - so the target name
; can only be worked out one file at a time.
;
; The rename is done IN PLACE, over the eleven bytes of the directory
; entry, and not through 56h's move-the-entry machinery.  Two reasons:
; there is no long name to invent for an 8.3 target, and an entry that
; stays where it is keeps the forward scan below honest - a renamed
; entry that moved could be found again further down and renamed
; twice.
;=====================================================================
fn17:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_setdir
        jc      f17_none
        call    fcb_pat                 ; fq_name = the pattern to find

        mov     esi,[fq_ptr]            ; fq_new = the pattern to become
        add     esi,FCB_NEWNAME
        mov     edi,OFFSET fq_new
        mov     ecx,11
        rep     movsb

        mov     byte ptr [fq_hit],0
        mov     byte ptr [fq_bad],0
        xor     ebx,ebx
f17_lp:
        mov     al,[fq_attr]
        or      al,SRCH_FILTER
        mov     [srch_attr],al
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        mov     edi,OFFSET fq_name
        call    fat_search
        jc      f17_end

        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jnz     f17_next

        ; build this file's target name: the new pattern, with '?'
        ; taking whatever the old name has in that position
        push    ebx
        mov     edi,OFFSET fq_targ
        xor     ecx,ecx
f17_bld:
        cmp     ecx,11
        jae     f17_built
        mov     al,[fq_new+ecx]
        cmp     al,'?'
        jne     f17_keep
        mov     al,[esi+ecx]
f17_keep:
        mov     [edi+ecx],al
        inc     ecx
        jmp     short f17_bld
f17_built:

        ; is the target name already taken?  The search runs over the
        ; same directory from the top, so a rename to the file's own
        ; name finds itself and is refused - which is what DOS does.
        mov     al,ATTR_DIR OR SRCH_FILTER
        mov     [srch_attr],al
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        xor     ebx,ebx
        mov     edi,OFFSET fq_targ
        call    fat_search
        pop     ebx
        jnc     f17_taken

        call    dir_seek                ; back onto our own entry
        jc      f17_end
        call    lfn_tomb                ; an 8.3 rename orphans any long
        call    dir_seek                ; name: its checksum no longer
        jc      f17_end                 ; matches the entry it belongs to
        push    ebx
        mov     edi,esi
        mov     esi,OFFSET fq_targ
        mov     ecx,11
        rep     movsb
        pop     ebx
        call    fat_dirupdate
        jc      f17_end
        call    fat_flush
        jc      f17_end
        mov     byte ptr [fq_hit],1
        jmp     short f17_next
f17_taken:
        mov     byte ptr [fq_bad],1     ; DOS renames what it can and then
                                        ; says something went wrong
f17_next:
        inc     ebx
        jmp     short f17_lp
f17_end:
        cmp     byte ptr [fq_bad],0
        jne     f17_none
        cmp     byte ptr [fq_hit],0
        je      f17_none
        jmp     fcb_ok
f17_none:
        mov     ax,ERR_NOFILE
        jmp     fcb_fail

;=====================================================================
; 23h - file size, in records
;
; The FCB is UNOPENED and the record size field is the question: the
; answer is how many records of that size the file takes, rounded up,
; left in the random record field.  A record size of zero means 128,
; the same default an open would have written.
;=====================================================================
fn23:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_setdir
        jc      f23_none
        call    fcb_pat
        mov     al,[fq_attr]
        or      al,SRCH_FILTER
        mov     [srch_attr],al
        mov     dword ptr [srch_long],0
        mov     byte ptr [srch_lonly],0
        xor     ebx,ebx
        mov     edi,OFFSET fq_name
        call    fat_search
        jc      f23_none
        mov     eax,[esi+DE_SIZE]
        mov     [fq_bytes],eax          ; out of the cache before anything
                                        ; else can recycle it
        call    fcb_recsz               ; EBX = the record size
        mov     eax,[fq_bytes]
        add     eax,ebx                 ; round up...
        jc      f23_huge
        dec     eax                     ; ...and a size of zero stays zero
        xor     edx,edx
        div     ebx
        call    fcb_setrr
        jmp     fcb_ok
f23_huge:
        mov     eax,-1                  ; a file this big cannot exist on a
        call    fcb_setrr               ; FAT volume, but the arithmetic
        jmp     fcb_ok                  ; must not wrap if one ever does
f23_none:
        mov     ax,ERR_NOFILE
        jmp     fcb_fail

;=====================================================================
; 24h - set the random record field from the current position
;
; The other direction of the same arithmetic every read and write
; does, exposed so that a program can switch from sequential to random
; access without working it out itself.  It cannot fail.
;=====================================================================
fn24:
        mov     edx,[ebp+F_EDX]
        call    fcb_get
        call    fcb_recsz               ; EBX, for the four-byte rule
        call    fcb_getext
        call    fcb_setrr
        jmp     fcb_ok

;=====================================================================
; 29h - parse a filename into an FCB
;
;   DS:SI -> the text, ES:DI -> the FCB, AL = what to do about the
;   fields the text does not mention:
;       bit 0 - skip leading separators
;       bit 1 - keep the drive field if the text names no drive
;       bit 2 - keep the name field if the text names no file
;       bit 3 - keep the extension likewise
;   -> AL = 0 (a plain name), 1 (it has wildcards) or FFh (bad drive),
;      SI advanced past what was parsed.
;
; This is the routine that makes the rest of the API usable, and it is
; the one worth getting exactly right, because every DOS program that
; ever read a command line called it and inherited its quirks.  Two in
; particular are reproduced deliberately:
;
;   A NAME LONGER THAN THE FIELD IS NOT AN ERROR AND NOT A STOP.  DOS
;   keeps scanning and throws the extra characters away, so
;   "LONGFILENAME.TXT" parses to LONGFILE.TXT with SI left after the
;   whole thing.  The DOS 6 source calls this a "horrible bug" in a
;   comment and then ships it, because by 1993 far too much depended
;   on it.  A parser that stopped at the eighth character would leave
;   SI in the middle of a name and every caller would then see the
;   remainder as the next argument.
;
;   '*' FILLS THE REST OF THE FIELD WITH '?' AND NOTHING ELSE.  That
;   is why "*.*" is eleven question marks, why "A*B.TXT" loses the B,
;   and why the wildcard flag in AL comes back set for both.
;=====================================================================
fn29:
        mov     al,[ebp+F_EAX]
        mov     [fq_pflag],al
        mov     byte ptr [fq_amb],0
        mov     byte ptr [fq_bad],0
        mov     esi,[ebp+F_ESI]
        mov     edi,[ebp+F_EDI]
        mov     [fq_pstart],esi         ; where the text began, so that the
                                        ; answer can be handed back as a
                                        ; distance as well as an address -
                                        ; see p29_adv

        ; the fields the caller did not ask us to leave alone
        test    byte ptr [fq_pflag],02h
        jnz     f29_keepdrv
        mov     byte ptr [edi+FCB_DRIVE],0
f29_keepdrv:
        test    byte ptr [fq_pflag],04h
        jnz     f29_keepnam
        push    edi
        add     edi,FCB_NAME
        mov     ecx,8
        mov     al,' '
        rep     stosb
        pop     edi
f29_keepnam:
        test    byte ptr [fq_pflag],08h
        jnz     f29_keepext
        push    edi
        add     edi,FCB_EXT
        mov     ecx,3
        mov     al,' '
        rep     stosb
        pop     edi
f29_keepext:
        ; DOS zeroes the two words past the extension whatever the
        ; flags say - the current block and the record size - so that
        ; a freshly parsed FCB is one an open can be handed.
        mov     word ptr [edi+FCB_EXTENT],0
        mov     word ptr [edi+FCB_RECSIZ],0

        test    byte ptr [fq_pflag],01h
        jz      f29_blanks
        call    f29_scanb               ; blanks and tabs first...
        mov     al,[esi]
        call    f29_issep               ; ...then ONE separator, if the
        jne     f29_noscan              ; next thing is one
        inc     esi
f29_blanks:
        call    f29_scanb
f29_noscan:

        ; a drive letter?
        cmp     byte ptr [esi+1],':'
        jne     f29_name
        mov     al,[esi]
        call    f29_upper
        sub     al,'@'                  ; A: = 1, as an FCB counts them
        jbe     f29_baddrv
        cmp     al,LASTDRIVE
        ja      f29_baddrv
        mov     [edi+FCB_DRIVE],al
        add     esi,2
        jmp     short f29_name
f29_baddrv:
        mov     byte ptr [fq_bad],1
        mov     byte ptr [edi+FCB_DRIVE],0FFh
        add     esi,2

f29_name:
        push    edi
        add     edi,FCB_NAME
        mov     ecx,8
        call    f29_field
        pop     edi
        cmp     byte ptr [esi],'.'
        jne     f29_done
        inc     esi                     ; eat the dot
        push    edi
        add     edi,FCB_EXT
        mov     ecx,3
        call    f29_field
        pop     edi
f29_done:
        mov     [ebp+F_ESI],esi         ; where the scan stopped...
        sub     esi,[fq_pstart]         ; ...and how far that is from where
        mov     [p29_adv],esi           ; it started, for the bridges
        mov     al,[fq_amb]
        cmp     byte ptr [fq_bad],0
        je      f29_al
        mov     al,0FFh
f29_al:
        mov     byte ptr [ebp+F_EAX],al
        jmp     i21_ok

;--- f29_field - copy up to ECX characters into the field at EDI,
;--- stopping at a delimiter.  See the two quirks in the header.
f29_field:
        push    ebx
f29_flp:
        mov     al,[esi]
        or      al,al
        jz      f29_fout
        call    f29_isdelim
        je      f29_fout
        inc     esi
        or      ecx,ecx
        jz      f29_flp                 ; field full: keep eating, store
                                        ; nothing.  This is the quirk.
        cmp     al,'*'
        je      f29_star
        call    f29_upper
        cmp     al,'?'
        jne     f29_store
        mov     byte ptr [fq_amb],1
f29_store:
        mov     [edi],al
        inc     edi
        dec     ecx
        jmp     short f29_flp
f29_star:
        mov     byte ptr [fq_amb],1
        mov     al,'?'
f29_fill:
        mov     [edi],al
        inc     edi
        dec     ecx
        jnz     f29_fill
        jmp     short f29_flp
f29_fout:
        pop     ebx
        ret

;--- f29_scanb - eat blanks and tabs
f29_scanb:
        mov     al,[esi]
        cmp     al,' '
        je      f29_seat
        cmp     al,9
        jne     f29_sout
f29_seat:
        inc     esi
        jmp     short f29_scanb
f29_sout:
        ret

;--- f29_upper - AL to upper case, letters only
f29_upper:
        cmp     al,'a'
        jb      f29_uout
        cmp     al,'z'
        ja      f29_uout
        sub     al,20h
f29_uout:
        ret

;--- f29_isdelim - ZF set if AL ends a filename field.  The set is
;--- DOS's: the characters a filename may not contain, plus the
;--- control characters, plus the blank.
f29_isdelim:
        cmp     al,20h
        jb      f29_dyes                ; every control character
        push    ecx
        push    edi
        mov     edi,OFFSET f29_delims
        mov     ecx,f29_ndelim
        repne   scasb
        pop     edi
        pop     ecx
        ret
f29_dyes:
        cmp     al,al                   ; ZF
        ret

;--- f29_issep - ZF set if AL is one of the "one time only" separators
;--- that bit 0 of the flags asks to be skipped.  A shorter list, and
;--- a different one: '.' ends a name but does not separate arguments.
f29_issep:
        push    ecx
        push    edi
        mov     edi,OFFSET f29_seps
        mov     ecx,f29_nsep
        repne   scasb
        pop     edi
        pop     ecx
        ret

f29_delims  db  ' ','"','+',',','.','/',':',';','<','=','>','[','\',']','|'
f29_ndelim  EQU $ - f29_delims
f29_seps    db  '+',',',':',';','<','=','>','|'
f29_nsep    EQU $ - f29_seps

;=====================================================================
; The plumbing every one of them stands on
;=====================================================================

;---------------------------------------------------------------------
; fcb_get - EDX -> whatever the caller passed.  Sorts out whether it
;           is an extended FCB, remembers the attribute byte if it is,
;           and leaves the PLAIN FCB in ESI and in [fq_ptr].
;---------------------------------------------------------------------
fcb_get:
        mov     [fq_user],edx
        mov     byte ptr [fq_isx],0
        mov     byte ptr [fq_attr],0
        mov     esi,edx
        cmp     byte ptr [esi],FCBX_SIG
        jne     fcb_gplain
        mov     byte ptr [fq_isx],1
        mov     al,[esi+FCBX_ATTR]
        mov     [fq_attr],al
        add     esi,FCBX_LEN
fcb_gplain:
        mov     [fq_ptr],esi
        ret

;---------------------------------------------------------------------
; fcb_mkpath - render the FCB as "D:NAME.EXT" in fq_path and hand it
;              back in ESI, which is what the resolver wants.  CF with
;              AX = error if the drive byte names no drive.
;
; The rendering is the exact inverse of what 29h produced, so a name
; that came in through 29h comes out unchanged.  A name a program
; built by hand with a blank in the middle does not - the blank ends
; the field here, as it ends it everywhere else in DOS.
;---------------------------------------------------------------------
fcb_mkpath:
        push    eax
        push    ecx
        push    edx
        push    edi
        mov     esi,[fq_ptr]
        mov     edi,OFFSET fq_path
        movzx   eax,byte ptr [esi+FCB_DRIVE]
        or      al,al
        jnz     fcb_mletter
        mov     al,[cur_drive]          ; 0 means "whatever is current",
        inc     al                      ; and an FCB counts from 1
fcb_mletter:
        cmp     al,LASTDRIVE
        ja      fcb_mbad
        add     al,'A'-1
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        add     edi,2
        add     esi,FCB_NAME
        mov     ecx,8
        call    fcb_mfield
        mov     esi,[fq_ptr]
        add     esi,FCB_EXT
        cmp     byte ptr [esi],' '
        je      fcb_mend
        mov     byte ptr [edi],'.'
        inc     edi
        mov     ecx,3
        call    fcb_mfield
fcb_mend:
        mov     byte ptr [edi],0
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        mov     esi,OFFSET fq_path
        clc
        ret
fcb_mbad:
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        mov     ax,ERR_BADDRIVE
        stc
        ret

;--- fcb_mfield - ECX bytes from ESI to EDI, stopping at a blank
fcb_mfield:
        push    ebx
        xor     ebx,ebx
fcb_mflp:
        cmp     ebx,ecx
        jae     fcb_mfout
        mov     al,[esi+ebx]
        cmp     al,' '
        je      fcb_mfout
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short fcb_mflp
fcb_mfout:
        pop     ebx
        ret

;---------------------------------------------------------------------
; fcb_setdir - select the FCB's drive and put the directory context on
;              that drive's current directory, which is the only
;              directory an FCB can name: there is no path in one.
;              [fq_drv] gets the drive, 1-based and resolved, because
;              a search has to remember which drive it was on and "0
;              means the default" stops being true the moment the
;              default moves.
;---------------------------------------------------------------------
fcb_setdir:
        push    esi
        push    edi
        push    ecx
        push    edx
        mov     esi,[fq_ptr]
        movzx   eax,byte ptr [esi+FCB_DRIVE]
        or      al,al
        jnz     fcb_sdhave
        mov     al,[cur_drive]
        inc     al
fcb_sdhave:
        cmp     al,LASTDRIVE
        ja      fcb_sdbad
        mov     [fq_drv],al
        add     al,'A'-1
        mov     byte ptr [fq_path],al
        mov     byte ptr [fq_path+1],':'
        mov     byte ptr [fq_path+2],0
        mov     esi,OFFSET fq_path      ; a drive and nothing else: the
        call    resolve_dir             ; resolver walks to its current
        jc      fcb_sdbad               ; directory and stops there
        mov     al,[fq_drv]
        pop     edx
        pop     ecx
        pop     edi
        pop     esi
        clc
        ret
fcb_sdbad:
        pop     edx
        pop     ecx
        pop     edi
        pop     esi
        mov     ax,ERR_BADDRIVE
        stc
        ret

;---------------------------------------------------------------------
; fcb_pat - the FCB's eleven name bytes, as a search pattern.  '?' is
;           already the wildcard the directory matcher wants and '*'
;           never gets this far: 29h turned it into question marks at
;           parse time, which is where DOS turns it into them too.
;---------------------------------------------------------------------
fcb_pat:
        push    esi
        push    edi
        push    ecx
        mov     esi,[fq_ptr]
        add     esi,FCB_NAME
        mov     edi,OFFSET fq_name
        mov     ecx,11
        rep     movsb
        pop     ecx
        pop     edi
        pop     esi
        ret

;---------------------------------------------------------------------
; fcb_recsz - EBX = the FCB's record size, with the zero case turned
;             into 128 AND WRITTEN BACK, because DOS writes it back
;             and a program can see that it did.
;---------------------------------------------------------------------
fcb_recsz:
        push    esi
        mov     esi,[fq_ptr]
        movzx   ebx,word ptr [esi+FCB_RECSIZ]
        or      ebx,ebx
        jnz     fcb_rsout
        mov     ebx,128
        mov     word ptr [esi+FCB_RECSIZ],bx
fcb_rsout:
        pop     esi
        ret

;---------------------------------------------------------------------
; fcb_getext / fcb_setext - the sequential position, which DOS keeps
;             in two fields for reasons that were good in 1981: a
;             block of 128 records, and the record within it.  The
;             record byte holds seven bits and the eighth bit of it
;             belongs to the block number, which is why this is a
;             shift and not a pair of loads.
;---------------------------------------------------------------------
fcb_getext:                             ; -> EAX = record number
        push    esi
        push    edx
        mov     esi,[fq_ptr]
        movzx   eax,word ptr [esi+FCB_EXTENT]
        shl     eax,7
        movzx   edx,byte ptr [esi+FCB_NR]
        and     dl,7Fh
        add     eax,edx
        pop     edx
        pop     esi
        ret

fcb_setext:                             ; EAX = record number
        push    esi
        push    eax
        push    edx
        mov     esi,[fq_ptr]
        mov     edx,eax
        and     dl,7Fh
        mov     [esi+FCB_NR],dl
        shr     eax,7
        mov     [esi+FCB_EXTENT],ax
        pop     edx
        pop     eax
        pop     esi
        ret

;---------------------------------------------------------------------
; fcb_getrr / fcb_setrr - the random record field, which is four bytes
;             or three depending on the record size.  That rule is not
;             a mistake anybody made: the field was three bytes until
;             DOS 2, the fourth byte was added where it could not
;             break anything, and "record size under 64" is where it
;             could not.  EBX = the record size on both.
;---------------------------------------------------------------------
fcb_getrr:                              ; -> EAX = the random record
        push    esi
        mov     esi,[fq_ptr]
        mov     eax,[esi+FCB_RR]
        cmp     ebx,64
        jb      fcb_grout
        and     eax,00FFFFFFh
fcb_grout:
        pop     esi
        ret

fcb_setrr:                              ; EAX = the random record
        push    esi
        push    eax
        mov     esi,[fq_ptr]
        mov     [esi+FCB_RR],ax
        shr     eax,16
        mov     [esi+FCB_RR+2],al
        cmp     ebx,64
        jae     fcb_srout
        mov     [esi+FCB_RR+3],ah
fcb_srout:
        pop     eax
        pop     esi
        ret

;---------------------------------------------------------------------
; fcb_room - how many bytes may be moved at the DTA.  EAX; everything
;            else preserved.  See the long note in fcb_io for what
;            this is standing in for.
;---------------------------------------------------------------------
fcb_room:
        push    ecx
        push    edx
        mov     eax,10000h              ; the API's own ceiling
        mov     edx,[dta_ptr]
        mov     ecx,[r3_base]           ; is the DTA inside the running
        or      ecx,ecx                 ; program's slot?  A DOS box's is
        jz      fcb_rmout               ; not, and is bounded below instead
        cmp     edx,ecx
        jb      fcb_rmout
        add     ecx,[r3_size]
        cmp     edx,ecx
        jae     fcb_rmout
        sub     ecx,edx
        cmp     eax,ecx
        jbe     fcb_rmout
        mov     eax,ecx

;--- AND THE SAME QUESTION buf_ok ANSWERS FOR 3Fh, asked here instead
;--- because this routine exists to TRIM rather than to refuse: a
;--- block read that runs out of room is a short read, which the API
;--- has a way of saying, and 27h answering "1 record" is a better
;--- answer than 27h answering "access denied".
;---
;--- A DTA at FFFF:FFF0 is sixteen bytes from PMDOS.SYS, and a block
;--- read of 128-byte records crosses that without any part of the
;--- copy loop noticing.  A DTA down in the real-mode stack is the
;--- other half of it, and 1Ah will set one anywhere at all.
fcb_rmout:
        cmp     edx,DOS_BASE
        jae     fcb_rmk                 ; a slot's DTA; bounded above already
        mov     ecx,DOS_BASE
        sub     ecx,edx
        cmp     eax,ecx
        jbe     fcb_rmk
        mov     eax,ecx
fcb_rmk:
        mov     ecx,eax
        mov     eax,edx
        call    cbuf_ok                 ; preserves both, so EAX comes back
        mov     eax,ecx                 ; as the DTA and ECX as the room
        jnc     fcb_rmdone
        xor     eax,eax                 ; no room at all: fcb_io then leaves
fcb_rmdone:                             ; the file untouched and says so
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; fcb_bind - hand back the table entry this FCB's file is open on, in
;            EDI, opening it again if the entry has been recycled.
;            CF with AX = error if the file cannot be reached at all.
;
; THE COMPARISON IS THE WHOLE ROUTINE.  An index alone proves nothing
; - index seven is index seven whoever is in it - so the FCB carries
; the claim id the entry had when it was opened and the owner it was
; opened for, and all three have to agree before the cached binding is
; believed.  Any disagreement is not an error: it means the entry has
; been reused since, and the file is simply opened again from the name
; the FCB is still carrying.
;---------------------------------------------------------------------
fcb_bind:
        push    eax
        push    ebx
        push    ecx
        push    esi
        mov     esi,[fq_ptr]
        movzx   eax,byte ptr [esi+FCBR_SFN]

        ; AND AN FCB FULL OF ZEROES MUST NOT LOOK LIKE A BINDING.  It
        ; is the commonest FCB there is - a program clears a buffer and
        ; fills in the name - and index zero is a real, in-use entry:
        ; it is the standard input.  The stamp would have agreed with
        ; it too, because the three standard entries are planted by
        ; sft_init and never go through sft_new, so nothing ever gave
        ; them one and theirs is zero as well.  A freshly cleared FCB
        ; therefore bound itself to CON, and the first record read
        ; waited for somebody to type ninety-six bytes.  Two tests are
        ; cheaper than one: the standard handles are never an FCB's,
        ; and a claim id of zero was never handed out.
        cmp     eax,3
        jb      fcb_reopen
        cmp     eax,MAX_HANDLES
        jae     fcb_reopen
        mov     edi,eax
        imul    edi,SFT_LEN
        add     edi,OFFSET sft_tbl
        test    dword ptr [edi+SFT_FLAGS],SFTF_INUSE
        jz      fcb_reopen
        movzx   eax,word ptr [esi+FCBR_STAMP]
        or      eax,eax
        jz      fcb_reopen
        cmp     eax,[edi+SFT_STAMP]
        jne     fcb_reopen
        mov     eax,[esi+FCBR_OWN]
        cmp     eax,[edi+SFT_OWNER]
        jne     fcb_reopen
        call    fcb_touch               ; still ours, and now the most
        jmp     short fcb_bgood         ; recently used of the FCBs

fcb_reopen:
        call    fcb_mkpath
        jc      fcb_bbad
        mov     eax,2
        call    fcb_openx
        jc      fcb_bbad
fcb_bopened:
        movzx   ebx,ax
        mov     edi,esi                 ; open_core hands back the entry
        call    fcb_adopt
fcb_bgood:
        pop     esi
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
fcb_bbad:
        pop     esi
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; fcb_openx / fcb_createx - open_core and create_core with the pool's
;             recycling behind them: when the table is full, close the
;             FCB that has waited longest and try once more.
;
;   fcb_openx    ESI -> path, EAX = mode
;   fcb_createx  ESI -> path, ECX = attributes, [create_new] set
;   both -> AX = index, ESI = the entry, or CF with AX = error
;
; EVERY DOOR INTO THE TABLE NEEDS THIS, not just the re-open.  The
; first version had the eviction inside fcb_bind alone, on the
; reasoning that recycling is what a stale FCB needs - and the
; thirty-third 0Fh in a row then failed with "too many open files",
; which is precisely the case the recycling exists to prevent.  Real
; DOS calls its LRUFcb from the open and the create as well as from
; the regeneration, for the same reason.
;---------------------------------------------------------------------
fcb_openx:
        mov     [fq_opath],esi
        mov     [fq_omode],eax
        call    open_core
        jnc     fcb_oxout
        cmp     ax,ERR_NOHANDLES
        jne     fcb_oxout
        call    fcb_evict
        jc      fcb_oxfull
        mov     esi,[fq_opath]
        mov     eax,[fq_omode]
        call    open_core
fcb_oxout:
        ret
fcb_oxfull:
        mov     ax,ERR_NOHANDLES
        stc
        ret

fcb_createx:
        mov     [fq_opath],esi
        mov     [fq_omode],ecx
        call    create_core
        jnc     fcb_cxout
        cmp     ax,ERR_NOHANDLES
        jne     fcb_cxout
        call    fcb_evict
        jc      fcb_oxfull
        mov     esi,[fq_opath]
        mov     ecx,[fq_omode]
        mov     byte ptr [create_new],0
        call    create_core
fcb_cxout:
        ret

;---------------------------------------------------------------------
; fcb_adopt - EDI = a table entry, EBX = its index.  Stamp the FCB
;             with everything needed to recognise it again.
;---------------------------------------------------------------------
fcb_adopt:
        push    eax
        push    esi
        mov     esi,[fq_ptr]
        mov     [esi+FCBR_SFN],bl
        mov     eax,[edi+SFT_STAMP]
        mov     [esi+FCBR_STAMP],ax
        mov     eax,[edi+SFT_OWNER]
        mov     [esi+FCBR_OWN],eax
        mov     eax,[edi+SFT_DRIVE]
        inc     eax
        mov     [esi+FCBR_DRV],al
        call    fcb_touch
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
; fcb_unbind - this FCB no longer has a file.  Only the close needs
;              it; everything else leaves the binding in place on
;              purpose, so the next call finds it still there.
;---------------------------------------------------------------------
fcb_unbind:
        push    esi
        mov     esi,[fq_ptr]
        mov     byte ptr [esi+FCBR_SFN],FCBR_NONE
        pop     esi
        ret

;---------------------------------------------------------------------
; fcb_touch - EDI = a table entry: mark it the most recently used of
;             the FCB entries.  Zero means "not an FCB's" and is
;             skipped on the wrap, because a handle must never look
;             like the oldest FCB.
;---------------------------------------------------------------------
fcb_touch:
        push    eax
        mov     eax,[fq_lru]
        inc     eax
        jnz     fcb_tok
        inc     eax
fcb_tok:
        mov     [fq_lru],eax
        mov     [edi+SFT_FCBLRU],eax
        pop     eax
        ret

;---------------------------------------------------------------------
; fcb_evict - close the FCB entry that has waited longest, freeing a
;             table slot.  CF if there is not one - which means every
;             entry in the table is a real handle and the caller is
;             simply out of files.
;---------------------------------------------------------------------
fcb_evict:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        xor     edx,edx                 ; the candidate, 0 = none yet
        mov     eax,-1                  ; and its age
        mov     esi,OFFSET sft_tbl
        mov     ecx,MAX_HANDLES
fcb_evlp:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      fcb_evnext
        mov     ebx,[esi+SFT_FCBLRU]
        or      ebx,ebx
        jz      fcb_evnext              ; a handle, not an FCB
        cmp     ebx,eax
        jae     fcb_evnext
        mov     eax,ebx
        mov     edx,esi
fcb_evnext:
        add     esi,SFT_LEN
        loop    fcb_evlp
        or      edx,edx
        jz      fcb_evnone
        mov     esi,edx
        call    sft_close               ; writes the size and stamp back,
                                        ; then frees the entry
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        clc
        ret
fcb_evnone:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; fcb_fill - EDI = the entry just opened or created: put what an open
;            is supposed to put in the caller's FCB.
;
; The record size becomes 128 and the current block becomes zero.  The
; CURRENT RECORD BYTE IS DELIBERATELY LEFT ALONE, because DOS leaves
; it alone: the documented sequence is "open, then zero the record
; yourself", and a program that sets it before the open would find its
; setting thrown away by a kernel that was tidier than the one it was
; written for.
;---------------------------------------------------------------------
fcb_fill:
        push    eax
        push    esi
        mov     esi,[fq_ptr]
        mov     word ptr [esi+FCB_RECSIZ],128
        mov     word ptr [esi+FCB_EXTENT],0
        mov     eax,[edi+SFT_SIZE]
        mov     [esi+FCB_FILSIZ],eax
        ; NOT one dword: the table keeps time then date, the FCB keeps
        ; date then time, and the two fields are adjacent in both.
        mov     ax,[edi+SFT_TIME]
        mov     [esi+FCB_FTIME],ax
        mov     ax,[edi+SFT_DATE]
        mov     [esi+FCB_FDATE],ax
        test    dword ptr [edi+SFT_FLAGS],SFTF_DEVICE
        jnz     fcb_flout               ; a device has no drive to report
        mov     eax,[edi+SFT_DRIVE]
        inc     eax
        mov     [esi+FCB_DRIVE],al
fcb_flout:
        pop     esi
        pop     eax
        ret

;---------------------------------------------------------------------
; How an FCB call answers.  There is no carry flag in this API and no
; error code either - a byte of AL is the whole vocabulary - so the
; failure path still records the reason where 59h can find it, which
; is the only way a program can ask what went wrong.
;---------------------------------------------------------------------
fcb_ok:
        mov     byte ptr [ebp+F_EAX],0
        jmp     i21_ok

fcb_fail:                               ; AX = the reason, for 59h
        mov     word ptr [last_err],ax
        mov     byte ptr [ebp+F_EAX],0FFh
        jmp     i21_ok

;=====================================================================
; State.  One FCB call at a time, like every other INT 21h here.
;=====================================================================
fq_user     dd  0                       ; what the caller passed
fq_ptr      dd  0                       ; and the plain FCB inside it
fq_isx      db  0                       ; it was an extended one
fq_attr     db  0                       ; and this was its attribute byte
fq_drv      db  0                       ; the drive, 1-based and resolved
fq_hit      db  0                       ; a wildcard call did something
fq_bad      db  0                       ; ...and something else went wrong
fq_pflag    db  0                       ; 29h's flag bits
fq_amb      db  0                       ; 29h saw a wildcard
            db  0
fq_op       db  0                       ; which of the six I/O calls
fq_err      db  0                       ; the answer being assembled
            db  0,0
fq_sft      dd  0                       ; the entry it is running on
fq_cnt      dd  0                       ; records asked for
fq_recsz    dd  0
fq_recpos   dd  0                       ; in records
fq_bpos     dd  0                       ; the same, in bytes
fq_bytes    dd  0                       ; how many to move
fq_done     dd  0                       ; how many moved
fq_lru      dd  0                       ; the FCB clock
fq_pstart   dd  0                       ; where 29h's text began
fq_opath    dd  0                       ; what fcb_openx is retrying
fq_omode    dd  0
fq_path     db  20 dup(0)               ; "D:NAME.EXT"
fq_name     db  11 dup(0)               ; the 8.3 pattern
fq_new      db  11 dup(0)               ; 17h's replacement pattern
fq_targ     db  11 dup(0)               ; and what it comes to for one file
            db  0
fq_ent      db  32 dup(0)               ; a directory entry, copied out
