;=====================================================================
; ISO9660.INC - reading a CD-ROM, in the kernel rather than through a
;               redirector
;
; This is the second filesystem PM-DOS has, and the first one that is
; not FAT.  (There is a third now - UDF.INC, for a disc with no
; ISO9660 on it at all - and it is reached through this file, at the
; end of the mount below, and answers through this file's iso_entry.)
; It reads through IOS_CDREAD, which indexes ATAPI units
; rather than BIOS drives, for the reason written down at that
; equate: a packet device has no drive number and its block is 2048
; bytes, so a CD arriving down the ordinary block path would read a
; quarter of what was asked for and report success.
;
; WHY THERE IS NO REDIRECTOR HERE.  The DOS way to reach a CD is
; MSCDEX, which is not a block driver but a client of the INT 2Fh
; AH=11h redirector interface - and a redirector plants itself in the
; CDS array, which this kernel deliberately does not have (see
; GLOL_CDS in PMDOS.INC: inventing plausible structures is worse than
; saying no, because a tool would follow them).  PM-DOS has its own
; 32-bit block layer and a volume table whose founding idea is already
; that a letter is a volume rather than a disk, so it reads the disc
; itself.  What that gives up is network drives and the MSCDEX API.
;
; WHAT ISO9660 IS, in the two facts that shape the code:
;
;   EVERY MULTI-BYTE NUMBER IS STORED TWICE, little-endian and then
;   big-endian, so that either kind of machine could read the disc
;   without swapping.  A dword field is eight bytes and a word field
;   is four.  This reader takes the little-endian half and steps over
;   the other, which is why the offsets below look oddly spaced.
;
;   A DIRECTORY IS A FLAT RUN OF VARIABLE-LENGTH RECORDS, each one
;   starting with its own length, packed into 2048-byte blocks and
;   NEVER CROSSING ONE.  A zero length byte means "no more records in
;   this block" and the walk moves to the next block rather than
;   stopping - which is the one thing about the format that a walker
;   written against a FAT directory gets wrong.
;
; Included by DOSMAIN.ASM, after FAT.INC.
;=====================================================================

ISO_PVDBLK      EQU     16              ; where the descriptors start - and
                                        ; it is sixteen blocks INTO A
                                        ; SESSION rather than sixteen blocks
                                        ; into the disc.  On a disc with one
                                        ; session those are the same number,
                                        ; which is why this was a constant
                                        ; for as long as it was
ISO_MAXDESC     EQU     32              ; how far to look for the PVD

; The table of contents, which is how the sessions are found.
ISO_TOCMAX      EQU     804             ; 4 + 100 * 8: every track a disc can
                                        ; have, and the header in front
ISO_TOC_LEN     EQU     0               ; word BE - how many bytes follow
ISO_TOC_FIRST   EQU     2               ; byte - the first track
ISO_TOC_LAST    EQU     3               ; byte - and the last
ISO_TOC_DESC    EQU     4               ; the track descriptors start here
ISO_TD_LEN      EQU     8               ; and each one is eight bytes:
ISO_TD_ADRCTL   EQU     1               ; byte - ADR in the high nibble,
                                        ; CONTROL in the low one
ISO_TD_TRACK    EQU     2               ; byte - which track
ISO_TD_LBA      EQU     4               ; dword BE - where it starts
ISO_TRK_LEADOUT EQU     0AAh            ; the run-out, which is not a track
ISO_CTL_DATA    EQU     04h             ; in CONTROL: this track holds data
                                        ; rather than music

; THE PACKET COMMANDS PM-DOS BUILDS FOR ITSELF, handed to IOS_CDPKT.
; PMIO knows the ones a block device needs - ready, capacity, read;
; these are the ones a CD needs in order to be a CD, and they live on
; this side because the code that knows WHY it is asking is here.
; MSCD001.INC builds from the same set.
CDOP_TESTRDY    EQU     000h
CDOP_STARTSTOP  EQU     01Bh            ; eject, and close the tray again
CDOP_PREVENT    EQU     01Eh            ; lock it, so it cannot be opened
CDOP_SEEK       EQU     02Bh
CDOP_SUBCHAN    EQU     042h            ; where the head is, whether the
                                        ; drive is playing, and the disc's
                                        ; catalogue number
CDOP_READTOC    EQU     043h            ; the tracks, and with them the
                                        ; sessions
CDOP_PLAYMSF    EQU     047h
CDOP_PAUSE      EQU     04Bh            ; ...and resume, on a bit in it
CDOP_STOPPLAY   EQU     04Eh
CDOP_MODESEL    EQU     055h            ; the audio volume, out
CDOP_MODESNS    EQU     05Ah            ; ...and back
CDOP_READCD     EQU     0BEh            ; the frame as it is recorded, all
                                        ; 2352 bytes of it - see cdd_readraw

; Volume descriptor
ISO_VD_TYPE     EQU     0               ; byte - 1 = primary, 255 = end
ISO_VD_ID       EQU     1               ; 5 bytes - "CD001"
ISO_VD_VER      EQU     6               ; byte - 1
ISO_VDT_PRIMARY EQU     1
ISO_VDT_SUPP    EQU     2               ; supplementary: this is where
                                        ; Joliet lives
ISO_VDT_END     EQU     255

; Primary volume descriptor
ISO_PVD_VOLID   EQU     40              ; 32 bytes, space padded
ISO_PVD_SIZE    EQU     80              ; dword pair - blocks on the volume
ISO_PVD_BLKSIZE EQU     128             ; word pair - logical block size
ISO_PVD_ROOT    EQU     156             ; a directory record, 34 bytes
ISO_SVD_ESC     EQU     88              ; supplementary: the escape sequence

; The three identifier fields at the end of a primary descriptor, each
; 37 bytes and space-padded.  Nothing in the reader wants them - they
; name files rather than describing the volume - but the MSCDEX API has
; a call for each, so they are part of the descriptor's layout and
; belong here with the rest of it.  Counted off the run of 128-byte
; identifiers that precedes them: volume set at 190, publisher at 318,
; data preparer at 446, application at 574, and these three after it.
ISO_PVD_COPYR   EQU     702             ; 37 bytes - copyright file id
ISO_PVD_ABSTR   EQU     739             ; 37 bytes - abstract file id
ISO_PVD_BIBLIO  EQU     776             ; 37 bytes - bibliographic file id
ISO_PVD_IDLEN   EQU     37
                                        ; that says which character set - see
                                        ; iso_joliet

; Directory record
ISO_DR_LEN      EQU     0               ; byte - the whole record
ISO_DR_XALEN    EQU     1               ; byte - extended attributes first
ISO_DR_EXTENT   EQU     2               ; dword pair - first block
ISO_DR_SIZE     EQU     10              ; dword pair - bytes of data
ISO_DR_DATE     EQU     18              ; 7 bytes
ISO_DR_FLAGS    EQU     25              ; byte
ISO_DR_UNIT     EQU     26
ISO_DR_GAP      EQU     27
ISO_DR_VOLSEQ   EQU     28              ; word pair
ISO_DR_IDLEN    EQU     32              ; byte
ISO_DR_ID       EQU     33              ; that many bytes

ISO_FL_HIDDEN   EQU     01h
ISO_FL_DIR      EQU     02h
ISO_FL_ASSOC    EQU     04h
ISO_FL_MULTI    EQU     80h             ; not the last extent of the file

; The two identifiers that are not text: a one-byte name of 00h is
; "this directory" and 01h is "the parent".
ISO_ID_DOT      EQU     00h
ISO_ID_DOTDOT   EQU     01h

;=====================================================================
; State
;=====================================================================
iso_unit     db 0                       ; which ATAPI unit is mounted
iso_ok       db 0                       ; a primary descriptor was parsed
iso_jol      db 0                       ; the root in use is a Joliet one
iso_bufok    db 0                       ; iso_buf holds iso_bufblk
iso_dloaded  db 0                       ; ...and it is the walk's block
iso_bufblk   dd 0FFFFFFFFh
iso_lbsize   dd 0                       ; the disc's logical block size.
                                        ; NOT iso_blksize, which would differ
                                        ; from the equate ISO_BLKSIZE only in
                                        ; case - and wasm folds case, so they
                                        ; would be ONE symbol and the constant
                                        ; would win in silence.  NOTES v0.48
                                        ; records this trap; this release
                                        ; walked into it again anyway
iso_volsize  dd 0                       ; blocks on the volume
iso_rootext  dd 0                       ; root directory, first block
iso_rootlen  dd 0                       ; ...and its length in bytes

iso_base     dd 0                       ; the LBA the mounted volume's
                                        ; descriptors were found at: 0 on a
                                        ; disc with one session, the last
                                        ; session's track on a disc with
                                        ; more.  THE EXTENTS INSIDE ARE NOT
                                        ; RELATIVE TO IT - see iso_findbase
iso_rr       db 0                        ; the primary tree carries SUSP, so
                                        ; the real names are behind the
                                        ; records rather than in them
iso_skp      db 0                        ; ...and this many bytes at the
                                        ; front of every system use area
                                        ; belong to somebody else
iso_rrnm     db 0                        ; the last record walked had a name
iso_rrre     db 0                        ; ...was a relocated directory
iso_rrlen    dd 0                        ; ...how long its name was
iso_rrcl     dd 0                        ; ...where its children really are
iso_rrpl     dd 0                        ; ...and where its parent really is

iso_dext     dd 0                       ; the walk: block being read
iso_dleft    dd 0                       ; bytes of directory still to go
iso_doff     dd 0                       ; offset inside the block

i83_len      dd 0                       ; iso_mk83 while it works: the name
i83_dot      dd 0                       ; length, the offset just past its
i83_num      dd 0                       ; last dot, and the number iso_alias
                                        ; is about to write
i83_okchars  db '!#$%&()-@^_{}~'       ; the punctuation an 8.3 name is
             db 27h,60h                 ; allowed.  The apostrophe and the
                                        ; backtick will not go inside a
                                        ; quoted string here, so they are
                                        ; written as the bytes they are
I83_OKCOUNT  EQU $ - i83_okchars

iso_lastrec  db 255 dup (0)             ; the last record iso_entry built
                                        ; from, whole - MSCDEX's 150Fh
                                        ; wants the parts FAT has no room
                                        ; for
             db 0
iso_mrec     db 255 dup (0)             ; a file written in several pieces,
                                        ; as one record - see iso_joinext.
                                        ; 255 is the largest a record may be
                                        ; and the length is a byte, so this
                                        ; cannot be overrun by a bad disc
             db 0                       ; and a pad, to keep what follows
                                        ; on an even address
iso_rootrec  db 34 dup (0)              ; the root's own directory record
iso_volid    db 33 dup (0)              ; volume name, NUL terminated
iso_fatent   db 32 dup (0)              ; an ISO record wearing FAT's shape -
                                        ; see iso_entry, which is the whole
                                        ; reason a CD can be a drive letter
iso_buf      db ISO_BLKSIZE dup (0)     ; the one cached block
iso_cebuf    db ISO_BLKSIZE dup (0)     ; a Rock Ridge continuation area.
                                        ; It cannot share the cache above:
                                        ; the block being continued FROM is
                                        ; the directory the walk is in the
                                        ; middle of
iso_toc      db ISO_TOCMAX dup (0)      ; the table of contents
cd_cdb       db 12 dup (0)              ; a command block, built here and
                                        ; run by IOS_CDPKT

;=====================================================================
; iso_rdblk - read one logical block into iso_buf.
;             EAX = block.  CF on failure.
;
; ONE BLOCK IS CACHED, and it is worth saying why that is enough here
; where it would not be for FAT: a directory walk reads a block and
; then works through sixty or so records inside it, so the hit rate on
; a single block is already most of what a cache can win.
;=====================================================================
iso_rdblk:
        push    ebx
        push    ecx
        push    edi
        cmp     eax,dword ptr [iso_bufblk]
        jne     irb_load
        cmp     byte ptr [iso_bufok],0
        jne     irb_hit
irb_load:
        mov     byte ptr [iso_bufok],0
        mov     ebx,eax
        mov     al,byte ptr [iso_unit]
        mov     ecx,1
        mov     edi,OFFSET iso_buf
        call    cache_cdread            ; the disk cache, which for an
                                        ; optical drive is read-only and
                                        ; reads two blocks where one was
                                        ; asked for - see CACHE.INC.  It
                                        ; falls through to IOS_CDREAD when
                                        ; there is no cache, so this is the
                                        ; same call it always was
        jc      irb_changed
        mov     dword ptr [iso_bufblk],ebx
        mov     byte ptr [iso_bufok],1
irb_hit:
        pop     edi
        pop     ecx
        pop     ebx
        clc
        ret
irb_changed:
        ; A FAILED READ IS HOW A DISC CHANGE ARRIVES.  There is no
        ; change LINE on an ATAPI drive the way there is on a floppy
        ; controller - the drive reports "medium may have changed" as a
        ; sense condition on the next command, and the command fails.
        ; So the trigger has to be the failure itself: throw the cached
        ; block away and, more importantly, unmount the volume, so that
        ; the next thing to touch this letter mounts whatever is in the
        ; drive NOW rather than carrying on with the extents of a disc
        ; that has been taken out.
        ;
        ; It is deliberately not clever.  A read can also fail because
        ; the tray is open or the disc is scratched, and re-mounting is
        ; the right answer to all three: the only cost of being wrong
        ; is one extra mount.
        mov     byte ptr [iso_ok],0
        mov     byte ptr [fat_ok],0     ; ...and vol_select will re-mount
        mov     byte ptr [iso_bufok],0
        mov     dword ptr [iso_bufblk],0FFFFFFFFh
        mov     al,byte ptr [iso_unit]  ; and the block cache goes with it,
        call    cache_cddrop            ; for the same reason iso_buf does
irb_bad:
        pop     edi
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; iso_forget - throw the cached block away.  Anything that changes
;              which disc is in the drive has to call this.
;=====================================================================
iso_forget:
        mov     byte ptr [iso_bufok],0
        mov     dword ptr [iso_bufblk],0FFFFFFFFh
        ret

;=====================================================================
; cd_cdbzero - blank the command block this file builds into.  All
;              registers preserved.
;=====================================================================
cd_cdbzero:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET cd_cdb
        xor     eax,eax
        mov     ecx,12
        cld
        rep     stosb
        pop     edi
        pop     ecx
        pop     eax
        ret

;=====================================================================
; cd_readtoc - the table of contents of the unit in AL, into iso_toc.
;              CF set if the drive would not give one.  All registers
;              preserved otherwise.
;
; Format 0, which is every track on the disc: each one says where it
; starts and whether it holds data or music.
;=====================================================================
cd_readtoc:
        pushad
        push    eax
        call    cd_cdbzero
        pop     eax
        mov     byte ptr [cd_cdb+0],CDOP_READTOC
        mov     byte ptr [cd_cdb+6],0           ; from the first track
        mov     byte ptr [cd_cdb+7],(ISO_TOCMAX SHR 8) AND 0FFh
        mov     byte ptr [cd_cdb+8],ISO_TOCMAX AND 0FFh
        mov     ah,0                            ; the data phase receives
        mov     esi,OFFSET cd_cdb
        mov     edi,OFFSET iso_toc
        mov     ecx,ISO_TOCMAX
        call    dword ptr ds:[IOS_CDPKT]
        jc      crt_bad
        popad
        clc
        ret
crt_bad:
        popad
        stc
        ret

;=====================================================================
; iso_findbase - AL = unit, EDI = a 2048-byte scratch buffer.
;                -> EAX = the block the newest volume descriptors
;                start at, which is zero on an ordinary disc.
;                It cannot fail: a disc whose table of contents says
;                nothing useful is read exactly the way it always was.
;
; MULTI-SESSION, AND WHY THE ANSWER IS NOT "ASK FOR THE LAST SESSION".
; READ TOC format 1 exists to say where the last session begins, and
; it is the call every book names - this kernel's own notes named it
; too.  The trouble is what a drive that does not think in sessions
; answers: format 1 says one session, track one, which is the FIRST
; session, and it says it without any way to tell that apart from a
; disc that really has one.  86Box answers exactly that for a disc it
; reports three tracks for in format 0.  A reader built on format 1
; alone is therefore right on paper and wrong on the machine it is
; being tested on, which is the worst of both.
;
; So the question is asked the way it is actually meant: WHERE IS THE
; NEWEST ISO9660 VOLUME ON THIS DISC.  Walk the tracks backwards, skip
; the ones carrying music, and take the first one that has CD001
; sixteen blocks in.  On a plain data disc that is track one and the
; answer is zero - the same read every disc got before today.  On a
; disc appended to twice it is the last session's track.  On a CD
; Extra, whose first session is audio and whose block 16 is therefore
; music, it is the data track that a reader starting at 16 could not
; see at all.
;
; It is also the more honest question in a way format 1 is not: a last
; session that holds no filesystem - a disc closed with an audio
; session, say - is skipped rather than mounted as nothing.
;=====================================================================
iso_findbase:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     [ifb_unit],al
        mov     [ifb_buf],edi
        mov     dword ptr [ifb_best],0

        call    cd_readtoc
        jc      ifb_out                 ; no contents: it starts at zero

        movzx   ecx,byte ptr [iso_toc+ISO_TOC_LEN]
        shl     ecx,8
        mov     cl,byte ptr [iso_toc+ISO_TOC_LEN+1]
        cmp     ecx,2
        jbe     ifb_out                 ; the two track-number bytes only
        sub     ecx,2
        shr     ecx,3                   ; -> how many descriptors
        jz      ifb_out
        cmp     ecx,100
        jbe     ifb_have
        mov     ecx,100                 ; more than a disc can hold
ifb_have:
        mov     esi,OFFSET iso_toc + ISO_TOC_DESC
        mov     eax,ecx
        dec     eax
        imul    eax,ISO_TD_LEN
        add     esi,eax                 ; the LAST one, and work backwards
ifb_try:
        cmp     byte ptr [esi+ISO_TD_TRACK],ISO_TRK_LEADOUT
        je      ifb_next
        test    byte ptr [esi+ISO_TD_ADRCTL],ISO_CTL_DATA
        jz      ifb_next                ; music: no filesystem in it

        movzx   eax,byte ptr [esi+ISO_TD_LBA+0]
        shl     eax,8
        mov     al,byte ptr [esi+ISO_TD_LBA+1]
        shl     eax,8
        mov     al,byte ptr [esi+ISO_TD_LBA+2]
        shl     eax,8
        mov     al,byte ptr [esi+ISO_TD_LBA+3]
        mov     [ifb_lba],eax

        add     eax,ISO_PVDBLK
        mov     ebx,eax
        push    ecx
        push    esi
        mov     al,[ifb_unit]
        mov     ecx,1
        mov     edi,[ifb_buf]
        call    dword ptr ds:[IOS_CDREAD]
        pop     esi
        pop     ecx
        jc      ifb_next                ; a track that will not read is not
                                        ; the one we are looking for
        mov     edi,[ifb_buf]
        cmp     byte ptr [edi+ISO_VD_ID+0],'C'
        jne     ifb_next
        cmp     byte ptr [edi+ISO_VD_ID+1],'D'
        jne     ifb_next
        cmp     byte ptr [edi+ISO_VD_ID+2],'0'
        jne     ifb_next
        cmp     byte ptr [edi+ISO_VD_ID+3],'0'
        jne     ifb_next
        cmp     byte ptr [edi+ISO_VD_ID+4],'1'
        jne     ifb_next
        mov     eax,[ifb_lba]
        mov     [ifb_best],eax
        jmp     short ifb_out
ifb_next:
        sub     esi,ISO_TD_LEN
        dec     ecx
        jnz     ifb_try
ifb_out:
        mov     eax,[ifb_best]
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret
ifb_unit db 0
ifb_lba  dd 0
ifb_best dd 0
ifb_buf  dd 0

;=====================================================================
; iso_rrcheck - does this disc's primary tree carry Rock Ridge?
;
; SUSP says so in ONE place and it has to be exactly there: the FIRST
; system use entry of the "." record of the ROOT directory, with the
; signature SP and the two check bytes BEh EFh behind it.  That
; position is the whole protocol's handshake, and a writer that puts
; anything in front of it has written a disc nobody reads as Rock
; Ridge - which is worth knowing from both ends, because TOOLS\MKCD.PL
; did exactly that on its first run and bsdtar duly showed the mangled
; 8.3 names.
;
; LEN_SKP, the last byte of the entry, is how many bytes at the front
; of every OTHER system use area belong to something else.  It is
; almost always zero and costs nothing to honour.
;=====================================================================
iso_rrcheck:
        pushad
        mov     byte ptr [iso_rr],0
        mov     byte ptr [iso_skp],0
        mov     eax,[iso_rootext]
        call    iso_rdblk
        jc      irc_out

        ; record zero is the root's own "." entry
        movzx   eax,byte ptr [iso_buf+ISO_DR_LEN]
        cmp     eax,34
        jb      irc_out
        movzx   ebx,byte ptr [iso_buf+ISO_DR_IDLEN]
        cmp     ebx,1
        jne     irc_out
        ; A PAD BYTE FOLLOWS AN IDENTIFIER OF EVEN LENGTH, and this
        ; identifier is one byte long - so there is none.  The first
        ; version of this added one anyway and looked for SP one byte
        ; late, which is the whole of why the first Rock Ridge disc
        ; read back its mangled 8.3 names: the handshake is at a fixed
        ; place, and being one byte out is the same as it not being
        ; there at all.  iso_susp had the test the right way round,
        ; which is what made the mistake invisible - the walker was
        ; correct and was never asked to walk.
        test    bl,1
        jnz     irc_odd
        inc     ebx
irc_odd:
        add     ebx,ISO_DR_ID
        lea     ecx,[ebx+7]
        cmp     ecx,eax
        ja      irc_out                 ; no room for an SP entry at all
        cmp     byte ptr [iso_buf+ebx+0],'S'
        jne     irc_out
        cmp     byte ptr [iso_buf+ebx+1],'P'
        jne     irc_out
        cmp     byte ptr [iso_buf+ebx+4],0BEh
        jne     irc_out
        cmp     byte ptr [iso_buf+ebx+5],0EFh
        jne     irc_out
        mov     al,byte ptr [iso_buf+ebx+6]
        mov     byte ptr [iso_skp],al
        mov     byte ptr [iso_rr],1
irc_out:
        popad
        ret

;=====================================================================
; iso_mount - AL = ATAPI unit.  Find the primary volume descriptor and
;             remember the root directory.  CF set if the disc is not
;             ISO9660 or will not read.
;
; The descriptors are a list starting at block 16 and ending with a
; type-255 record, so this walks it rather than assuming the primary
; is first - it usually is, and a disc with a boot record in front of
; it is entirely legal.
;=====================================================================
iso_mount:
        pushad
        mov     byte ptr [iso_unit],al
        mov     byte ptr [iso_ok],0
        mov     byte ptr [udf_on],0     ; whatever the LAST disc was, this
                                        ; mount decides again: a UDF disc
                                        ; swapped for an ISO9660 one would
                                        ; otherwise be read by the wrong
                                        ; half of the reader
        call    iso_forget

        ; ASK WHETHER THERE IS A DISC BEFORE READING ONE, and wait for
        ; it if the answer is "nearly".  IOS_CDINFO is TEST UNIT READY
        ; with retries followed by READ CAPACITY, and the retries are
        ; the point: a drive whose disc has just been swapped spends a
        ; second or two answering "becoming ready", and a mount that
        ; went straight to block 16 read nothing and reported an
        ; unreadable disc - which is what a swap looked like from the
        ; outside until this line was here.
        movzx   eax,byte ptr [iso_unit]
        call    dword ptr ds:[IOS_CDINFO]
        jc      imo_bad

        ; WHATEVER CHANGE WAS PENDING, THIS MOUNT IS THE ANSWER TO IT.
        ; The ready check above is a command like any other, so it may
        ; well be the one the drive reported a change on; asking now
        ; and throwing the answer away is what stops the next DOS call
        ; deciding the disc moved and mounting it all over again.
        movzx   eax,byte ptr [iso_unit]
        mov     ah,CDL_VOL
        call    dword ptr ds:[IOS_CDCHG]

        ; WHERE THE DESCRIPTORS ARE.  On a disc with one session that
        ; is block 16 and always was; on a disc with more it is sixteen
        ; blocks into the newest one - see iso_findbase.
        mov     al,byte ptr [iso_unit]
        mov     edi,OFFSET iso_buf
        call    iso_findbase
        mov     dword ptr [iso_base],eax
        call    iso_forget              ; that search read through the
                                        ; buffer without going through the
                                        ; cache that describes it

        mov     ecx,ISO_MAXDESC
        mov     eax,[iso_base]
        add     eax,ISO_PVDBLK
imo_next:
        push    ecx
        push    eax
        call    iso_rdblk
        pop     eax
        pop     ecx
        jc      imo_bad

        ; "CD001" or this is not a descriptor at all
        cmp     byte ptr [iso_buf+ISO_VD_ID+0],'C'
        jne     imo_bad
        cmp     byte ptr [iso_buf+ISO_VD_ID+1],'D'
        jne     imo_bad
        cmp     byte ptr [iso_buf+ISO_VD_ID+2],'0'
        jne     imo_bad
        cmp     byte ptr [iso_buf+ISO_VD_ID+3],'0'
        jne     imo_bad
        cmp     byte ptr [iso_buf+ISO_VD_ID+4],'1'
        jne     imo_bad

        mov     bl,byte ptr [iso_buf+ISO_VD_TYPE]
        cmp     bl,ISO_VDT_PRIMARY
        je      imo_found
        cmp     bl,ISO_VDT_END
        je      imo_bad                 ; the list ended without one
        inc     eax
        dec     ecx
        jnz     imo_next
        jmp     short imo_bad

imo_found:
        ; the logical block size, which this reader requires to be the
        ; 2048 that IOS_CDREAD moves - a disc with any other is legal
        ; and is refused honestly rather than read wrongly
        movzx   ebx,word ptr [iso_buf+ISO_PVD_BLKSIZE]
        cmp     ebx,ISO_BLKSIZE
        jne     imo_bad
        mov     dword ptr [iso_lbsize],ebx

        mov     ebx,dword ptr [iso_buf+ISO_PVD_SIZE]
        mov     dword ptr [iso_volsize],ebx

        ; the root directory record, copied out whole: the block it
        ; sits in is about to be replaced by the first directory read
        mov     esi,OFFSET iso_buf + ISO_PVD_ROOT
        mov     edi,OFFSET iso_rootrec
        mov     ecx,34
        cld
        rep     movsb

        mov     eax,dword ptr [iso_rootrec+ISO_DR_EXTENT]
        mov     dword ptr [iso_rootext],eax
        mov     eax,dword ptr [iso_rootrec+ISO_DR_SIZE]
        mov     dword ptr [iso_rootlen],eax

        ; and the volume name, trimmed of the spaces it is padded with
        mov     esi,OFFSET iso_buf + ISO_PVD_VOLID
        mov     edi,OFFSET iso_volid
        mov     ecx,32
        rep     movsb
        mov     byte ptr [edi],0
        mov     ecx,32
imo_trim:
        dec     edi
        cmp     byte ptr [edi],' '
        jne     imo_trimmed
        mov     byte ptr [edi],0
        loop    imo_trim
imo_trimmed:
        call    iso_rrcheck             ; are the real names behind the
                                        ; records, Unix fashion?
        call    iso_joliet              ; ...or in a second tree, Windows
                                        ; fashion, which wins if it is there
        mov     byte ptr [iso_ok],1
        popad
        clc
        ret

imo_bad:
        ; NOT ISO9660 - AND THAT IS NOT THE SAME THING AS NOT A DISC.
        ; A DVD authored without an ISO9660 bridge has no CD001
        ; anywhere on it: what it has is UDF, whose own recognition
        ; sequence is in the very blocks just read.  This is the only
        ; place UDF is tried, and it is tried LAST, so that every disc
        ; that ever worked here is still read by the code that read it
        ; before.
        mov     al,byte ptr [iso_unit]
        mov     ebx,[iso_base]
        call    udf_mount
        jc      imo_reallybad
        mov     byte ptr [iso_ok],1
        popad
        clc
        ret
imo_reallybad:
        popad
        stc
        ret

;=====================================================================
; THE DIRECTORY WALK
;
; iso_diropen  EAX = extent, EDX = length in bytes.  Starts a walk.
; iso_dirnext  -> ESI = the next record, CF set when there are none.
;
; The state is one set of globals, so one walk at a time - which is
; all the caller below needs.  When this grows a find-first/find-next
; API the state goes in the DTA, for the reason v0.58 wrote down at
; length.
;=====================================================================
iso_diropen:
        mov     dword ptr [iso_dext],eax
        mov     dword ptr [iso_dleft],edx
        mov     dword ptr [iso_doff],0
        mov     byte ptr [iso_dloaded],0
        ret

;=====================================================================
; iso_dirnext - the next record, with a file that was written in more
;               than one piece handed back as ONE.
;
; ISO_FL_MULTI SAYS "THIS IS NOT THE LAST EXTENT OF THIS FILE".  A file
; too big for one extent - the size field is 32 bits, so over 4GB - is
; written as several directory records in a row, all with the same
; name, the flag set on every one but the last.  A walker that does not
; know this lists the file once per extent and reads only the first
; piece of it, which is the kind of wrong that looks like a corrupt
; disc rather than like a missing feature.
;
; THE RECORDS ARE CONSECUTIVE AND THAT IS WHAT IDENTIFIES THEM, not
; their names: the standard says the pieces of one file follow each
; other, so the rule "keep going while the last record had the flag"
; needs no name comparison and cannot be fooled by two files whose
; names match after a Joliet or Rock Ridge rewrite.
;=====================================================================
iso_dirnext:
        call    iso_dirone
        jc      idn_nomore
        test    byte ptr [esi+ISO_DR_FLAGS],ISO_FL_MULTI
        jz      idn_single
        call    iso_joinext             ; -> ESI = one record for all of it
idn_single:
        clc
        ret
idn_nomore:
        stc
        ret

iso_dirone:
        push    eax
        push    ebx
idn_block:
        cmp     dword ptr [iso_dleft],0
        jbe     idn_end

        ; make sure the block the offset is in is the one in the buffer
        cmp     byte ptr [iso_dloaded],0
        jne     idn_have
        mov     eax,dword ptr [iso_dext]
        call    iso_rdblk
        jc      idn_end
        mov     byte ptr [iso_dloaded],1
idn_have:
        mov     ebx,dword ptr [iso_doff]
        cmp     ebx,ISO_BLKSIZE
        jae     idn_advance

        ; A ZERO LENGTH BYTE IS "NOTHING MORE IN THIS BLOCK", NOT "THE
        ; END".  Records never straddle a block, so the tail of one is
        ; padded with zeros and the walk carries on in the next.
        movzx   eax,byte ptr [iso_buf+ebx+ISO_DR_LEN]
        or      eax,eax
        jz      idn_advance

        ; step past it for next time, then hand this one back
        add     dword ptr [iso_doff],eax
        lea     esi,[iso_buf+ebx]
        pop     ebx
        pop     eax
        clc
        ret

idn_advance:
        ; on to the next block of the directory
        mov     eax,ISO_BLKSIZE
        cmp     dword ptr [iso_dleft],eax
        jbe     idn_end
        sub     dword ptr [iso_dleft],eax
        inc     dword ptr [iso_dext]
        mov     dword ptr [iso_doff],0
        mov     byte ptr [iso_dloaded],0
        jmp     short idn_block

idn_end:
        mov     dword ptr [iso_dleft],0
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; iso_joinext - ESI = a record whose file continues in the records
;               after it.  -> ESI = one record describing the whole
;               file, with the extents behind it consumed.
;
; THE RECORD IS COPIED OUT FIRST because the walk below reads further
; blocks of the directory, and the block this record is sitting in is
; the one that gets thrown away to do it.
;
; THE SIZE IS ONLY ADDED UP WHILE THE PIECES ARE CONTIGUOUS, and that
; is a limit stated rather than hidden.  Everything above this layer
; reads a file by starting at the extent in the entry and running
; forward - the same way it reads a contiguous FAT file - so a total
; that included a piece somewhere else on the disc would be a promise
; the read path cannot keep, and the file would read as whatever
; happened to be recorded after the first piece.  An authoring tool
; writes the pieces in order and adjacent, so the common case adds up
; in full; a disc that does not gets the size of the run that DOES
; start at the extent, which reads short and reads TRUE.
;=====================================================================
iso_joinext:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    edi

        movzx   ecx,byte ptr [esi+ISO_DR_LEN]
        mov     edi,OFFSET iso_mrec
        cld
        rep     movsb
        mov     esi,OFFSET iso_mrec

        mov     edx,dword ptr [esi+ISO_DR_SIZE] ; the total so far
        mov     eax,edx
        add     eax,ISO_BLKSIZE - 1
        shr     eax,11                          ; bytes -> blocks, rounded
        mov     ebx,dword ptr [esi+ISO_DR_EXTENT]
        add     ebx,eax                         ; where the next piece has
        mov     edi,1                           ; to start to be one run
ijx_next:
        test    byte ptr [esi+ISO_DR_FLAGS],ISO_FL_MULTI
        jz      ijx_done                        ; that record was the last
        call    iso_dirone
        jc      ijx_done                        ; the directory ended in the
        or      edi,edi                         ; middle of the file
        jz      ijx_next                        ; past the break: consume
        cmp     ebx,dword ptr [esi+ISO_DR_EXTENT]
        jne     ijx_gap
        mov     eax,dword ptr [esi+ISO_DR_SIZE]
        add     edx,eax
        jc      ijx_huge                        ; over 4GB, which is a size
        add     eax,ISO_BLKSIZE - 1             ; DOS has no field for
        shr     eax,11
        add     ebx,eax
        jmp     short ijx_next
ijx_huge:
        ; THE FILE IS BIGGER THAN THE FIELD THAT REPORTS IT.  Every
        ; size DOS hands a program is 32 bits - in the directory entry,
        ; in 42h's seek and in 3Fh's count - so there is no honest
        ; number to give.  The largest one there is says "as much as
        ; this can say", and the read path stops there rather than
        ; wrapping round to a small file.
        mov     edx,0FFFFFFFFh
ijx_gap:
        xor     edi,edi
        jmp     short ijx_next
ijx_done:
        mov     esi,OFFSET iso_mrec
        mov     dword ptr [esi+ISO_DR_SIZE],edx
        and     byte ptr [esi+ISO_DR_FLAGS],0FFh - ISO_FL_MULTI
                                        ; the flag goes with the pieces it
                                        ; described: what is handed back is
                                        ; one whole file.  Written as a
                                        ; subtraction because NOT on a byte
                                        ; is a 32-bit number to this
                                        ; assembler and does not fit
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; THE NAME OF A THING ON A DISC, and the two shapes DOS wants it in.
;
; A directory record carries ONE identifier, and which alphabet it is
; written in depends on which tree this is.  The ISO9660 tree spells
; names in d-characters - upper case, digits and underscore - with a
; ";1" version suffix nobody wants to see.  The Joliet tree spells the
; same files in UCS-2, BIG-endian, two bytes to a character, with the
; case the author typed and no useful length limit.
;
; EVERYTHING ABOVE dir_seek WANTS BOTH HALVES OF A VFAT NAME: eleven
; bytes in the directory entry, and a long name for the 71xxh calls
; and for matching what the user typed.  So the name is read ONCE, in
; full, into lfn_buf - and the 8.3 field is made FROM THAT rather than
; from the disc.  That is the order VFAT itself works in, and it is
; why the two can never disagree about what the file is called.
;
;   iso_rawname   the identifier as text, into lfn_buf
;   iso_mk83      lfn_buf -> eleven bytes, saying what was lost
;   iso_alias     the ~N form, for when something was
;   iso_dosname   all three at once, which is what iso_entry calls
;
; [iso_lname] is the answer to "does this entry have a long name",
; and lfnw_seal reads it in place of a fragment chain: on a CD there
; are no 0Fh entries to collect, but the QUESTION the rest of the
; kernel asks is the same one, so it is answered in the same place.
;=====================================================================

;=====================================================================
; iso_susp - read the system use area of the record at ESI.
;
; Fills lfn_buf with the Rock Ridge name when there is one - saying so
; in [iso_rrnm], with its length in [iso_rrlen] - and collects the
; three markers that say the tree was rearranged to fit inside
; ISO9660's eight levels: [iso_rrcl], [iso_rrre], [iso_rrpl].
; Everything preserved.
;
; WHERE THE AREA IS: after the identifier, plus a pad byte when the
; identifier's length is EVEN, plus whatever LEN_SKP reserved.  Each
; entry is a two-letter signature, its own length and a version byte,
; and that shape is the whole point of the format - a reader steps
; over everything it has never heard of rather than giving up on the
; disc.
;
; A NAME TOO LONG FOR THE RECORD IS NOT AN EDGE CASE, it is the case
; Rock Ridge exists for.  A record may not exceed 255 bytes, so a long
; name arrives as several NM entries - and the last of them is
; commonly not in the record at all but in a CONTINUATION AREA
; elsewhere on the disc, named by a CE entry.  Following one needs a
; buffer that is not the one the directory walk is using, which is
; what iso_cebuf is for.
;
; THE SIGNATURES ARE COMPARED A BYTE AT A TIME on purpose.  A
; two-character constant in this assembler is a number with the first
; character in the HIGH half, and two bytes read from the disc put it
; in the low one - so the obvious word compare is the wrong way round
; and looks perfectly correct.
;=====================================================================
iso_susp:
        pushad
        mov     dword ptr [iso_rrlen],0
        mov     byte ptr [iso_rrnm],0
        mov     byte ptr [iso_rrre],0
        mov     dword ptr [iso_rrcl],0
        mov     dword ptr [iso_rrpl],0
        mov     dword ptr [isp_celen],0
        mov     dword ptr [isp_hops],0

        movzx   eax,byte ptr [esi+ISO_DR_IDLEN]
        lea     ebx,[eax+ISO_DR_ID]
        test    al,1
        jnz     isp_odd
        inc     ebx                     ; an even identifier is padded
isp_odd:
        movzx   eax,byte ptr [iso_skp]
        add     ebx,eax
        movzx   edx,byte ptr [esi+ISO_DR_LEN]
        add     edx,esi                 ; EDX = one past this record
        add     ebx,esi                 ; EBX = the next entry to look at

isp_walk:
        lea     eax,[ebx+4]
        cmp     eax,edx
        ja      isp_done                ; no room for another entry
        movzx   eax,byte ptr [ebx+2]    ; how long this one says it is
        cmp     eax,4
        jb      isp_done                ; a length of zero would never end
        lea     ecx,[ebx+eax]
        cmp     ecx,edx
        ja      isp_done                ; it runs off the end of the area

        cmp     byte ptr [ebx+0],'N'
        jne     isp_notnm
        cmp     byte ptr [ebx+1],'M'
        je      isp_nm
isp_notnm:
        cmp     byte ptr [ebx+0],'C'
        jne     isp_notc
        cmp     byte ptr [ebx+1],'E'
        je      isp_ce
        cmp     byte ptr [ebx+1],'L'
        je      isp_cl
isp_notc:
        cmp     byte ptr [ebx+0],'P'
        jne     isp_notpl
        cmp     byte ptr [ebx+1],'L'
        je      isp_pl
isp_notpl:
        cmp     byte ptr [ebx+0],'R'
        jne     isp_next
        cmp     byte ptr [ebx+1],'E'
        je      isp_re
isp_next:
        add     ebx,eax
        jmp     short isp_walk

isp_nm:
        ; Bits 1 and 2 say "this entry is the . record" and "this one
        ; is the ..", which are not names at all; bit 0 only says
        ; another NM follows, and they are appended in the order they
        ; appear whether it is set or not.
        push    eax
        mov     cl,byte ptr [ebx+4]
        test    cl,06h
        jnz     isp_nmdone
        lea     esi,[ebx+5]
        mov     ecx,eax
        sub     ecx,5
        call    isp_add
isp_nmdone:
        pop     eax
        jmp     short isp_next

isp_ce:
        ; Block, offset and length, each stored twice - the
        ; little-endian half is the one this kernel reads, as
        ; everywhere else in the format.
        push    eax
        mov     eax,dword ptr [ebx+4]
        mov     [isp_ceblk],eax
        mov     eax,dword ptr [ebx+12]
        mov     [isp_ceoff],eax
        mov     eax,dword ptr [ebx+20]
        mov     [isp_celen],eax
        pop     eax
        jmp     short isp_next
isp_cl:
        push    eax
        mov     eax,dword ptr [ebx+4]
        mov     [iso_rrcl],eax
        pop     eax
        jmp     short isp_next
isp_pl:
        push    eax
        mov     eax,dword ptr [ebx+4]
        mov     [iso_rrpl],eax
        pop     eax
        jmp     short isp_next
isp_re:
        mov     byte ptr [iso_rrre],1
        jmp     short isp_next

isp_done:
        cmp     dword ptr [isp_celen],0
        je      isp_out
        mov     eax,[isp_hops]
        inc     eax
        mov     [isp_hops],eax
        cmp     eax,4                   ; continuations may legally chain,
        ja      isp_out                 ; and a LOOP of them must not hang
                                        ; a directory listing
        mov     ebx,[isp_ceblk]
        mov     al,byte ptr [iso_unit]
        mov     ecx,1
        mov     edi,OFFSET iso_cebuf
        call    dword ptr ds:[IOS_CDREAD]
        jc      isp_out
        mov     ebx,[isp_ceoff]
        cmp     ebx,ISO_BLKSIZE
        jae     isp_out
        mov     edx,[isp_celen]
        add     edx,ebx
        cmp     edx,ISO_BLKSIZE
        jbe     isp_ceok
        mov     edx,ISO_BLKSIZE         ; an area cannot run past its block
isp_ceok:
        mov     dword ptr [isp_celen],0 ; consumed; another CE may set it
        add     ebx,OFFSET iso_cebuf
        add     edx,OFFSET iso_cebuf
        jmp     isp_walk
isp_out:
        mov     eax,[iso_rrlen]
        or      eax,eax
        jz      isp_noname
        mov     byte ptr [lfn_buf+eax],0
        mov     byte ptr [iso_rrnm],1
isp_noname:
        popad
        ret

;--- isp_add - ESI = bytes, ECX = how many, appended to lfn_buf in the
;--- form a DOS long name can hold.
;
; A byte with the top bit set is the first of a UTF-8 character on any
; disc written this century, and the bytes following it belong to the
; same character - so the sequence is DECODED and the character it
; spells looked up in codepage 437, which has a good many of them.
; What 437 cannot spell becomes ONE underscore for the whole character
; rather than one per byte, and so does a stray high byte from an older
; disc that is not UTF-8 at all - the same answer arrived at for two
; different reasons.
isp_add:
        push    eax
        push    ebx
        push    edi
isa_lp:
        or      ecx,ecx
        jz      isa_out
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,80h
        jb      isa_low
        call    isa_utf8                ; -> EBX = the code point, or -1
        cmp     ebx,0FFFFh
        ja      isa_wide                ; not UTF-8, or outside UCS-2
        mov     eax,ebx
        call    iso_cp437
        jnc     isa_put
isa_wide:
        mov     al,'_'
        jmp     short isa_put
isa_low:
        call    iso_lfnchar
isa_put:
        mov     edi,[iso_rrlen]
        cmp     edi,LFN_MAX - 1
        jae     isa_lp
        mov     byte ptr [lfn_buf+edi],al
        inc     edi
        mov     [iso_rrlen],edi
        jmp     short isa_lp
isa_out:
        pop     edi
        pop     ebx
        pop     eax
        ret

;--- isa_utf8 - AL = a byte with the top bit set, ESI/ECX = what
;--- follows it.  EBX = the code point it begins, or FFFFFFFFh when the
;--- bytes are not a UTF-8 character - and either way the continuation
;--- bytes are consumed, so a name is never turned into a row of
;--- underscores by one bad byte at the front of it.
;
; ONLY THE LEAD BYTE SAYS HOW LONG THE CHARACTER IS.  110xxxxx is two
; bytes, 1110xxxx three, 11110xxx four, and 10xxxxxx is a continuation
; that cannot start one.  A sequence that runs off the end of the field
; or whose continuation bytes are not continuation bytes is not UTF-8
; at all: an older disc's byte in somebody else's codepage, and this
; reader has no way to know whose.
isa_utf8:
        push    edx
        movzx   ebx,al
        cmp     al,0C0h
        jb      isu_bad                 ; a continuation byte on its own
        cmp     al,0E0h
        jae     isu_three
        and     ebx,1Fh
        mov     edx,1
        jmp     short isu_take
isu_three:
        cmp     al,0F0h
        jae     isu_four
        and     ebx,0Fh
        mov     edx,2
        jmp     short isu_take
isu_four:
        cmp     al,0F8h
        jae     isu_bad
        and     ebx,07h
        mov     edx,3
isu_take:
        or      edx,edx
        jz      isu_out
        or      ecx,ecx
        jz      isu_bad
        mov     al,[esi]
        and     al,0C0h
        cmp     al,80h
        jne     isu_bad
        movzx   eax,byte ptr [esi]
        and     eax,3Fh
        shl     ebx,6
        or      ebx,eax
        inc     esi
        dec     ecx
        dec     edx
        jmp     short isu_take
isu_bad:
        ; eat whatever continuation bytes follow, so that one character
        ; the reader cannot spell costs one underscore and not several
        or      ecx,ecx
        jz      isu_none
        mov     al,[esi]
        and     al,0C0h
        cmp     al,80h
        jne     isu_none
        inc     esi
        dec     ecx
        jmp     short isu_bad
isu_none:
        mov     ebx,0FFFFFFFFh
isu_out:
        pop     edx
        ret

;--- iso_lfnchar - AL in place: what a DOS long name may hold.  The
;--- Joliet path has the same problem and answers it in iso_lchar,
;--- which works on UCS-2 characters; this one works on bytes and has
;--- a second job, because a Rock Ridge name is a UNIX name and may
;--- legally contain the characters DOS keeps for its own syntax.
iso_lfnchar:
        cmp     al,20h
        jb      ilf_bad
        cmp     al,7Fh
        jae     ilf_bad
        cmp     al,'"'
        je      ilf_bad
        cmp     al,'*'
        je      ilf_bad
        cmp     al,'/'
        je      ilf_bad
        cmp     al,':'
        je      ilf_bad
        cmp     al,'<'
        je      ilf_bad
        cmp     al,'>'
        je      ilf_bad
        cmp     al,'?'
        je      ilf_bad
        cmp     al,'|'
        je      ilf_bad
        cmp     al,5Ch                  ; the backslash, written as its byte
        je      ilf_bad                 ; for the same reason i83_okchars
        ret                             ; spells two of its characters out
ilf_bad:
        mov     al,'_'
        ret

isp_ceblk   dd 0
isp_ceoff   dd 0
isp_celen   dd 0
isp_hops    dd 0

;=====================================================================
; iso_rawname - ESI = a directory record.  The identifier as text in
;               lfn_buf, ASCIIZ, -> ECX = its length.
;
; The version suffix goes.  ";1" is part of the stored name on every
; conforming disc and no DOS program has ever wanted it, and Joliet
; discs carry it too - encoded, so the test has to be for a UCS-2
; semicolon rather than for a byte.
;=====================================================================
iso_rawname:
        push    eax
        push    ebx
        push    edx
        push    edi
        mov     edi,OFFSET lfn_buf
        xor     edx,edx                 ; EDX = characters written

        ; THE SYSTEM USE AREA IS READ FIRST, and for every record
        ; including "." and "..".  A name is only one of the things in
        ; it: a ".." carries the parent link that says where a
        ; relocated directory really came from, and that has to be
        ; collected whether or not the entry has a name of its own.
        mov     byte ptr [iso_rrnm],0
        mov     byte ptr [iso_rrre],0
        mov     dword ptr [iso_rrcl],0
        mov     dword ptr [iso_rrpl],0
        cmp     byte ptr [iso_rr],0
        je      irn_nosusp
        cmp     byte ptr [iso_jol],0
        jne     irn_nosusp              ; a Joliet tree carries no SUSP, and
                                        ; the names in it are already real
        call    iso_susp
irn_nosusp:
        movzx   ecx,byte ptr [esi+ISO_DR_IDLEN]
        or      ecx,ecx
        jz      irn_end
        lea     ebx,[esi+ISO_DR_ID]

        ; "." and ".." are ONE BYTE even on a Joliet disc - they are
        ; not encoded - so this test comes before the alphabet does
        cmp     ecx,1
        jne     irn_wide
        mov     al,[ebx]
        cmp     al,ISO_ID_DOT
        jne     irn_notdot
        mov     byte ptr [edi],'.'
        mov     edx,1
        jmp     short irn_term          ; AND NOT irn_end: see the strip
irn_notdot:                             ; there, which would take these
        cmp     al,ISO_ID_DOTDOT        ; two apart - ".." came out "."
        jne     irn_wide                ; and "." came out empty, which
        mov     word ptr [edi],'..'     ; iso_alias then called "~1"
        mov     edx,2
        jmp     short irn_term

irn_wide:
        ; ROCK RIDGE FIRST, WHEN THE DISC HAS IT.  The identifier in
        ; the record is the mangled 8.3 name the authoring tool had to
        ; invent; the real one is behind it, and iso_susp has already
        ; put it in lfn_buf.  There is no version suffix to strip and
        ; no trailing dot to trim, because it was never an ISO9660
        ; name at all - so this goes straight to the terminator.
        cmp     byte ptr [iso_rrnm],0
        je      irn_notrr
        mov     edx,[iso_rrlen]
        jmp     irn_term
irn_notrr:
        cmp     byte ptr [iso_jol],0
        je      irn_narrow
        shr     ecx,1                   ; bytes -> UCS-2 characters
        jz      irn_end
irn_wlp:
        mov     al,[ebx+1]              ; big-endian: the low half
        mov     ah,[ebx]                ; ...and the high one
        add     ebx,2
        cmp     ax,';'
        je      irn_end
        call    iso_lchar
        cmp     edx,LFN_MAX - 1
        jae     irn_wnext
        mov     [edi+edx],al
        inc     edx
irn_wnext:
        dec     ecx
        jnz     irn_wlp
        jmp     short irn_end

irn_narrow:
        mov     al,[ebx]
        inc     ebx
        cmp     al,';'
        je      irn_end
        cmp     edx,LFN_MAX - 1
        jae     irn_nnext
        mov     [edi+edx],al
        inc     edx
irn_nnext:
        dec     ecx
        jnz     irn_narrow

irn_end:
        ; A FILE WITH NO EXTENSION IS STORED WITH THE DOT STILL ON IT.
        ; ISO9660 writes "READ.;1" for a file called READ, because the
        ; separator is not optional in the grammar - so one trailing
        ; dot is punctuation rather than part of the name.
        or      edx,edx
        jz      irn_term
        cmp     byte ptr [edi+edx-1],'.'
        jne     irn_term
        dec     edx
irn_term:
        mov     byte ptr [edi+edx],0
        mov     ecx,edx
        pop     edi
        pop     edx
        pop     ebx
        pop     eax
        ret

;--- iso_lchar - AX = a UCS-2 character -> AL = what a DOS long name
;--- can hold.  THE CASE IS KEPT, unlike in the 8.3 field: the long
;--- name is the real name and 71xxh hands it back as it was written.
;--- Anything codepage 437 cannot spell becomes '_', the way DOS has
;--- always rendered what it could not print.
iso_lchar:
        or      ah,ah
        jnz     ilc_wide
        cmp     al,20h
        jb      ilc_bad
        cmp     al,7Eh
        ja      ilc_wide
        ret
ilc_wide:
        call    iso_cp437
        jnc     ilc_out
ilc_bad:
        mov     al,'_'
ilc_out:
        ret

;=====================================================================
; iso_cp437 - AX = a Unicode code point -> AL = the byte codepage 437
;             spells it with.  CF set when 437 has no such character.
;
; A JOLIET NAME IS UCS-2 AND A ROCK RIDGE NAME IS UTF-8, and both of
; them can say things this machine's screen can show.  Folding
; everything above 7Eh to '_' threw away the half that WAS spellable:
; a disc authored in French listed "caf_.txt" on a machine with an é
; in its character generator, on a DOS whose own filenames have always
; been allowed to hold one.
;
; THE TABLE IS THE ANSWER AND A COMPARISON IS NOT.  There is no
; arithmetic between Unicode and 437 above 7Eh - the 128 characters are
; four alphabets, a currency set and the box drawing, in the order IBM
; put them in a ROM in 1981 - so the mapping is written out and looked
; up.  128 words is 256 bytes and a name is a few characters long, so
; the scan costs less than the code a binary search would need.
;
; It is deliberately EXACT and not approximate: 'é' is 82h because 437
; has 'é' at 82h, and 'ę' is not in 437 at all and stays an underscore.
; An approximate table - stripping accents to get a letter - would make
; two different files on the same disc list under one name.
;=====================================================================
cp437_uni   dw 0C7h,0FCh,0E9h,0E2h,0E4h,0E0h,0E5h,0E7h      ; 80h
            dw 0EAh,0EBh,0E8h,0EFh,0EEh,0ECh,0C4h,0C5h      ; 88h
            dw 0C9h,0E6h,0C6h,0F4h,0F6h,0F2h,0FBh,0F9h      ; 90h
            dw 0FFh,0D6h,0DCh,0A2h,0A3h,0A5h,20A7h,0192h    ; 98h
            dw 0E1h,0EDh,0F3h,0FAh,0F1h,0D1h,0AAh,0BAh      ; A0h
            dw 0BFh,2310h,0ACh,0BDh,0BCh,0A1h,0ABh,0BBh     ; A8h
            dw 2591h,2592h,2593h,2502h,2524h,2561h,2562h,2556h
            dw 2555h,2563h,2551h,2557h,255Dh,255Ch,255Bh,2510h
            dw 2514h,2534h,252Ch,251Ch,2500h,253Ch,255Eh,255Fh
            dw 255Ah,2554h,2569h,2566h,2560h,2550h,256Ch,2567h
            dw 2568h,2564h,2565h,2559h,2558h,2552h,2553h,256Bh
            dw 256Ah,2518h,250Ch,2588h,2584h,258Ch,2590h,2580h
            dw 03B1h,0DFh,0393h,03C0h,03A3h,03C3h,0B5h,03C4h ; E0h
            dw 03A6h,0398h,03A9h,03B4h,221Eh,03C6h,03B5h,2229h
            dw 2261h,0B1h,2265h,2264h,2320h,2321h,0F7h,2248h ; F0h
            dw 0B0h,2219h,0B7h,221Ah,207Fh,0B2h,25A0h,0A0h

iso_cp437:
        push    ecx
        push    edi
        mov     edi,OFFSET cp437_uni
        mov     ecx,128
        cld
        repne   scasw
        jne     icp_none
        sub     edi,OFFSET cp437_uni
        shr     edi,1                   ; bytes -> entries, and the scan
        add     edi,80h - 1             ; stepped PAST the one it matched
        mov     eax,edi
        pop     edi
        pop     ecx
        clc
        ret
icp_none:
        pop     edi
        pop     ecx
        stc
        ret

;=====================================================================
; iso_mk83 - lfn_buf -> the eleven bytes at EDI.
;            AL = 0 the field IS the name, 1 only the case differs,
;            2 something had to be thrown away.  EDI preserved; EAX
;            is the answer.
;
; THE LAST DOT IS THE ONE THAT COUNTS.  "Read Me First.v2.txt" has no
; 8.3 spelling at all, and the rule VFAT settled on is to take the
; final dot as the extension and alias everything else - which is what
; a user expects, because the extension is the part they think of as
; the file's kind.
;=====================================================================
iso_mk83:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        mov     ebp,edi
        mov     al,' '                  ; blank the field first
        mov     ecx,11
        cld
        rep     stosb
        mov     edi,ebp

        mov     esi,OFFSET lfn_buf
        xor     bh,bh                   ; BH = the worst loss so far
        xor     edx,edx                 ; EDX = offset past the last dot,
                                        ; 0 = there is no dot
        xor     ecx,ecx                 ; ECX = length
im8_scan:
        mov     al,[esi+ecx]
        or      al,al
        jz      im8_scanned
        cmp     al,'.'
        jne     im8_nodot
        or      edx,edx
        jz      im8_firstdot
        mov     bh,2                    ; a second dot: no 8.3 spelling
im8_firstdot:
        lea     edx,[ecx+1]
im8_nodot:
        inc     ecx
        jmp     short im8_scan
im8_scanned:
        mov     [i83_len],ecx
        mov     [i83_dot],edx

;--- the base: everything before the last dot, or all of it ----------
        or      edx,edx
        je      im8_havebase
        mov     ecx,edx
        dec     ecx                     ; the dot itself is not in it
im8_havebase:
        xor     bl,bl                   ; BL = characters written
im8_blp:
        or      ecx,ecx
        jz      im8_based
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,' '                  ; A SPACE IS NOT SUBSTITUTED BUT
        je      im8_bskip               ; DROPPED, which is what VFAT does
        cmp     al,'.'                  ; and what makes "Program Files"
        je      im8_bskip               ; come out PROGRA~1 and not
        call    iso_83char              ; PROGRA_1
        cmp     ah,bh
        jbe     im8_bkeep
        mov     bh,ah
im8_bkeep:
        cmp     bl,8
        jae     im8_btrunc
        mov     [edi],al
        inc     edi
        inc     bl
        jmp     short im8_blp
im8_btrunc:
        mov     bh,2                    ; more than eight
        jmp     short im8_blp
im8_bskip:
        mov     bh,2
        jmp     short im8_blp

im8_based:
        or      bl,bl
        jnz     im8_ext
        mov     bh,2                    ; nothing left to put in it

;--- and the extension, three bytes into the field -------------------
im8_ext:
        mov     edx,[i83_dot]
        or      edx,edx
        jz      im8_done
        mov     esi,OFFSET lfn_buf
        add     esi,edx
        mov     ecx,[i83_len]
        sub     ecx,edx
        lea     edi,[ebp+8]
        xor     bl,bl
im8_elp:
        or      ecx,ecx
        jz      im8_done
        mov     al,[esi]
        inc     esi
        dec     ecx
        cmp     al,' '
        je      im8_eskip
        call    iso_83char
        cmp     ah,bh
        jbe     im8_ekeep
        mov     bh,ah
im8_ekeep:
        cmp     bl,3
        jae     im8_etrunc
        mov     [edi],al
        inc     edi
        inc     bl
        jmp     short im8_elp
im8_etrunc:
        mov     bh,2
        jmp     short im8_elp
im8_eskip:
        mov     bh,2
        jmp     short im8_elp

im8_done:
        movzx   eax,bh
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;--- iso_83char - AL = one character of a name -> AL = what may stand
;--- for it in an 8.3 field, AH = 0 it already did, 1 the case was
;--- folded, 2 it had to be replaced.
iso_83char:
        xor     ah,ah
        cmp     al,'a'
        jb      i8c_notlower
        cmp     al,'z'
        ja      i8c_notlower
        sub     al,20h
        mov     ah,1
        ret
i8c_notlower:
        cmp     al,'A'
        jb      i8c_notalpha
        cmp     al,'Z'
        jbe     i8c_ok
i8c_notalpha:
        cmp     al,'0'
        jb      i8c_punct
        cmp     al,'9'
        jbe     i8c_ok
i8c_punct:
        push    ecx
        push    edi
        mov     edi,OFFSET i83_okchars
        mov     ecx,I83_OKCOUNT
        cld
        repne   scasb
        pop     edi
        pop     ecx
        je      i8c_ok
        mov     al,'_'
        mov     ah,2
i8c_ok:
        ret

;=====================================================================
; iso_alias - EDI = the eleven-byte field, EBX = this record's ordinal
;             in its directory.  Rewrite the base as the ~N form.
;             Everything preserved.
;
; THE NUMBER IS THE RECORD'S POSITION, and that is the one decision
; here worth defending.  VFAT numbers its aliases 1, 2, 3 by searching
; the directory for what is already taken - a whole directory walk for
; every entry listed, to produce a number whose only job is to be
; different from the others.  A read-only filesystem can have that for
; nothing: what an alias has to be is UNIQUE and STABLE, and a
; record's position in its own directory is both.  Two entries can no
; more share a position than they can share a name.
;=====================================================================
iso_alias:
        pushad
        mov     ebp,edi
        mov     eax,ebx
        inc     eax                     ; ~1 reads better than ~0
        cmp     eax,999999
        jbe     ia_cap
        mov     eax,999999              ; a directory with a million
ia_cap:                                 ; entries in it may collide
        mov     [i83_num],eax
        mov     ecx,1                   ; ECX = digits it needs
        mov     edx,10
ia_digits:
        cmp     eax,edx
        jb      ia_havedigits
        inc     ecx
        imul    edx,10
        jmp     short ia_digits
ia_havedigits:
        mov     eax,7
        sub     eax,ecx                 ; EAX = stem characters allowed

        push    ecx                     ; how many are actually there
        mov     ecx,8
        mov     edi,ebp
        xor     edx,edx
ia_count:
        cmp     byte ptr [edi],' '
        je      ia_counted
        inc     edx
        inc     edi
        loop    ia_count
ia_counted:
        pop     ecx
        cmp     edx,eax
        jbe     ia_stem
        mov     edx,eax
ia_stem:
        lea     edi,[ebp+edx]
        mov     al,'~'
        cld
        stosb
        add     edi,ecx                 ; the digits are written backwards,
        push    edi                     ; because division produces them
        mov     eax,[i83_num]           ; that way round
        mov     ebx,10
ia_dig:
        xor     edx,edx
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        loop    ia_dig
        pop     edi

        mov     ecx,ebp                 ; blank whatever is left of the
        add     ecx,8                   ; eight: the old base was longer
        sub     ecx,edi                 ; than the new one
        jbe     ia_out
        mov     al,' '
        rep     stosb
ia_out:
        popad
        ret

;=====================================================================
; iso_dosname - ESI = a directory record, EDI = eleven bytes to fill,
;               EBX = the record's ordinal in its directory.
;
; Fills the 8.3 field, leaves the full name in lfn_buf, and sets
; [iso_lname] to say whether the two differ.  ESI and EDI preserved.
;=====================================================================
iso_dosname:
        pushad
        mov     byte ptr [iso_lname],0
        call    iso_rawname             ; -> lfn_buf, ECX = length

        ; "." and ".." are names in their own right and never long
        or      ecx,ecx
        jz      idsn_real
        cmp     ecx,2
        ja      idsn_real
        cmp     byte ptr [lfn_buf],'.'
        jne     idsn_real
        cmp     ecx,1
        je      idsn_dots
        cmp     byte ptr [lfn_buf+1],'.'
        jne     idsn_real
idsn_dots:
        push    ecx
        push    edi
        mov     al,' '
        mov     ecx,11
        cld
        rep     stosb
        pop     edi
        pop     ecx
        mov     byte ptr [edi],'.'
        cmp     ecx,2
        jne     idsn_out
        mov     byte ptr [edi+1],'.'
        jmp     short idsn_out

idsn_real:
        call    iso_mkboth
idsn_out:
        popad
        ret

;=====================================================================
; iso_mkboth - lfn_buf holds a name, ECX its length, EDI the eleven
;              bytes to fill and EBX the record's ordinal.  Make the
;              8.3 field from the name and say whether the two differ.
;
; Its own routine because UDF.INC does the same thing to a name it
; spelled out of a different structure: reading the name is what the
; two filesystems disagree about, and everything after that is the
; same question - what may a DOS program be told this file is called.
;=====================================================================
iso_mkboth:
        call    iso_mk83                ; AL = 0 exact, 1 case, 2 lost
        or      al,al
        jz      imb_out                 ; the field IS the name
        mov     byte ptr [iso_lname],1
        cmp     al,2
        jb      imb_out                 ; only the case differs, and an
        call    iso_alias               ; alias would be a worse name
imb_out:
        ret
;=====================================================================
; iso_joliet - look for a Joliet tree and use it instead.
;
; Joliet is a SUPPLEMENTARY volume descriptor - type 2 - whose escape
; sequence says UCS-2 and whose directory tree is a parallel copy of
; the same files under their real names.  A disc that has one is
; usually a disc whose ISO9660 names have been mangled into 8.3, so
; taking the Joliet root gives better names for exactly the discs that
; need them.
;
; THE THREE ESCAPE SEQUENCES ARE THE WHOLE TEST: %/@, %/C and %/E, at
; offset 88, are UCS-2 levels 1, 2 and 3.  Anything else in a
; supplementary descriptor is some other character set and is left
; alone.
;
; If none is found the primary root stays, which is why this can fail
; silently: a disc without Joliet is not a broken disc.
;=====================================================================
iso_joliet:
        pushad
        mov     byte ptr [iso_jol],0
        mov     ecx,ISO_MAXDESC
        mov     eax,[iso_base]          ; the same session the primary came
        add     eax,ISO_PVDBLK          ; out of, and not the first one
ijo_next:
        push    ecx
        push    eax
        call    iso_rdblk
        pop     eax
        pop     ecx
        jc      ijo_out

        cmp     byte ptr [iso_buf+ISO_VD_ID+0],'C'
        jne     ijo_out
        cmp     byte ptr [iso_buf+ISO_VD_TYPE],ISO_VDT_END
        je      ijo_out
        cmp     byte ptr [iso_buf+ISO_VD_TYPE],ISO_VDT_SUPP
        jne     ijo_step

        ; %/@, %/C or %/E and nothing else
        cmp     byte ptr [iso_buf+ISO_SVD_ESC+0],'%'
        jne     ijo_step
        cmp     byte ptr [iso_buf+ISO_SVD_ESC+1],'/'
        jne     ijo_step
        mov     bl,byte ptr [iso_buf+ISO_SVD_ESC+2]
        cmp     bl,'@'
        je      ijo_take
        cmp     bl,'C'
        je      ijo_take
        cmp     bl,'E'
        jne     ijo_step

ijo_take:
        ; the same fields, in the same places, describing the other tree
        movzx   ebx,word ptr [iso_buf+ISO_PVD_BLKSIZE]
        cmp     ebx,ISO_BLKSIZE
        jne     ijo_step                ; not one this reader can move
        mov     esi,OFFSET iso_buf + ISO_PVD_ROOT
        mov     edi,OFFSET iso_rootrec
        mov     ecx,34
        cld
        rep     movsb
        mov     eax,dword ptr [iso_rootrec+ISO_DR_EXTENT]
        mov     dword ptr [iso_rootext],eax
        mov     eax,dword ptr [iso_rootrec+ISO_DR_SIZE]
        mov     dword ptr [iso_rootlen],eax
        mov     byte ptr [iso_jol],1
        jmp     short ijo_out

ijo_step:
        inc     eax
        dec     ecx
        jnz     ijo_next
ijo_out:
        popad
        ret

;=====================================================================
; iso_volstate - fill in the vol_state fields that describe a volume's
;                SHAPE, so that the calls which report one do not each
;                need to know what filesystem they are looking at.
;
; 36h wants sectors per cluster, bytes per sector and a free count;
; DIR wants a total.  Answering them from these fields rather than
; branching in each caller keeps the ISO knowledge in this file.
;
; A 2048-byte block is presented as four 512-byte sectors, which is
; what every DOS program's arithmetic expects, and FREE SPACE IS ZERO
; because it genuinely is: a program that asks how much room is left
; on a CD and is told a large number will try to use it.
;=====================================================================
iso_volstate:
        push    eax
        mov     dword ptr [fat_spc],4           ; 2048 / 512
        mov     dword ptr [fat_rootent],0       ; no fixed root area
        mov     dword ptr [fat_rootclus],0
        mov     dword ptr [fat_nfats],0
        mov     dword ptr [fat_spf],0
        mov     dword ptr [fat_hint],0
        mov     byte ptr [fat_type],0           ; not 12, 16 or 32
        mov     byte ptr [fat_media],0F8h
        mov     eax,[iso_volsize]
        mov     [fat_totclus],eax               ; blocks, and a block is
        mov     dword ptr [fat_eoc],0FFFFFFFFh  ; this volume's cluster
        mov     dword ptr [fat_eocv],0FFFFFFFFh
        shl     eax,2                           ; blocks -> 512-byte sectors
        mov     [fat_totsec],eax
        pop     eax
        ret

;=====================================================================
; iso_dirlen - dc_isoext names a directory; set dc_isolen to its real
;              length.
;
; A FAT directory ends when its cluster chain does, so nothing has to
; know how long it is in advance.  AN ISO DIRECTORY DOES NOT: its
; length is a number, and the only place that number is written down
; for a subdirectory is the parent's record for it - which the caller
; here no longer has, because all it was given was an extent.
;
; The disc records it a second time, and that is the way out: THE
; FIRST RECORD OF EVERY DIRECTORY IS ITS OWN "." ENTRY, and that entry
; carries the directory's size.  So read one block, take the size out
; of record zero, and the walk knows where to stop.
;=====================================================================
iso_dirlen:
        cmp     byte ptr [udf_on],0
        jne     udf_dirlen              ; where the answer is a read away
        pushad
        mov     eax,dword ptr [dc_isoext]
        call    iso_rdblk
        jc      idl_out
        ; record zero must be the "." entry: one byte of identifier,
        ; and that byte zero.  A disc that does not do this is
        ; malformed, and one block is a safer answer than a number
        ; taken from whatever is actually there.
        cmp     byte ptr [iso_buf+ISO_DR_LEN],34
        jb      idl_out
        cmp     byte ptr [iso_buf+ISO_DR_IDLEN],1
        jne     idl_out
        cmp     byte ptr [iso_buf+ISO_DR_ID],ISO_ID_DOT
        jne     idl_out
        mov     eax,dword ptr [iso_buf+ISO_DR_SIZE]
        or      eax,eax
        jz      idl_out
        mov     dword ptr [dc_isolen],eax
idl_out:
        popad
        ret

;=====================================================================
; iso_entry - the whole reason a CD can be a drive letter.
;
; EBX = which record of the current directory (0, 1, 2...).
; Returns ESI -> A THIRTY-TWO BYTE FAT DIRECTORY ENTRY that describes
; it, CF set when there is no such record.
;
; THE TRANSLATION IS THE DESIGN.  Everything above the directory layer
; in this kernel - walk_to, resolve_parent, 4Eh/4Fh, open, the LFN
; wrapper - consumes a pointer to a FAT directory entry and reads
; DE_ATTR, DE_CLUSLO, DE_SIZE off it.  Rather than teach all of that
; about ISO9660, ISO9660 answers in their shape: the extent goes where
; the start cluster goes, the flags become DOS attributes, and the
; identifier becomes an 8.3 name.  One routine is filesystem-specific
; and the rest of the kernel never finds out.
;
; What it costs is that the "cluster" in a synthesised entry is an ISO
; BLOCK NUMBER, not a FAT cluster, so anything that follows it has to
; know which volume it came off - which is what vol_fs is for.
;=====================================================================
iso_entry:
        ; AND IT MAY NOT BE ISO9660 AT ALL.  A disc with UDF on it and
        ; no CD001 anywhere is mounted by UDF.INC, which answers in
        ; this same buffer and this same shape - so the branch is here,
        ; at the one door everything above comes through, and not at
        ; twenty places that would each have to remember.
        cmp     byte ptr [udf_on],0
        jne     udf_entry

        push    eax
        push    ecx
        push    edx
        push    edi
        push    ebp

        ; THE ROOT IS INDEXED DIFFERENTLY FROM EVERY OTHER DIRECTORY,
        ; and both differences are there to make a CD look like the FAT
        ; volume the rest of the kernel expects.
        ;
        ; Entry zero of the root is the VOLUME LABEL.  ISO9660 keeps
        ; the volume name in the primary descriptor, not in a directory
        ; - but FAT keeps it in a root-directory entry with
        ; ATTR_VOLUME, and that is where COMMAND.COM's DIR looks for
        ; it, with a 4Eh search.  Synthesising it here means the label
        ; is found by the code that already knows how to find one, and
        ; fat_search's existing rule keeps it out of ordinary searches.
        ;
        ; And the root's "." and ".." records are SKIPPED, because a
        ; FAT root has neither and a listing that shows them in the
        ; root looks wrong to anyone who knows DOS.  A subdirectory
        ; keeps its own, exactly as FAT does.
        mov     ebp,ebx
        cmp     byte ptr [dc_isroot],0
        je      ien_walk
        or      ebp,ebp
        jz      ien_label
        dec     ebp
        add     ebp,2                   ; past "." and ".."
ien_walk:
        ; walk to the EBPth record.  A directory is a flat run of
        ; variable-length records, so there is no way to index one
        ; except by counting from the front; the block cache underneath
        ; makes that cheap enough within a block.
        mov     eax,dword ptr [dc_isoext]
        mov     edx,dword ptr [dc_isolen]
        call    iso_diropen
        mov     ecx,ebp
ien_skip:
        call    iso_dirnext
        jc      ien_none
        or      ecx,ecx
        jz      ien_got
        dec     ecx
        jmp     short ien_skip

ien_got:
        ; THE RECORD ITSELF IS KEPT, and not only what FAT's shape can
        ; carry.  MSCDEX's 150Fh hands a program the ISO directory
        ; entry - the extended attribute length, the interleave, the
        ; volume set, the version after the ';' and the system use area
        ; - and none of that survives the translation into thirty-two
        ; bytes.  One copy here is what lets that call be answered out
        ; of THIS walk instead of out of a second reader that would
        ; eventually disagree with it.
        push    ecx
        push    esi
        push    edi
        movzx   ecx,byte ptr [esi+ISO_DR_LEN]
        mov     edi,OFFSET iso_lastrec
        cld
        rep     movsb
        pop     edi
        pop     esi
        pop     ecx

        ; ESI is the ISO record; build the FAT entry from it
        mov     edi,OFFSET iso_fatent
        push    esi
        push    edi
        xor     al,al                   ; a clean entry every time: the
        mov     ecx,32                  ; caller may read fields this
        cld                             ; record does not set
        rep     stosb
        pop     edi
        pop     esi

        push    ebx
        mov     ebx,ebp                 ; the record ordinal, for the alias
        call    iso_dosname             ; the eleven bytes, and lfn_buf
        pop     ebx

        ; attributes.  A CD is read-only by construction, and that is
        ; not decoration: it is what stops a program opening a file on
        ; it for writing and being told yes.
        mov     al,ATTR_READONLY
        test    byte ptr [esi+ISO_DR_FLAGS],ISO_FL_DIR
        jz      ien_notdir
        or      al,ATTR_DIR
ien_notdir:
        test    byte ptr [esi+ISO_DR_FLAGS],ISO_FL_HIDDEN
        jz      ien_notdhid
        or      al,ATTR_HIDDEN
ien_notdhid:
        mov     byte ptr [edi+DE_ATTR],al

        ; the extent, into the two halves of the start cluster
        mov     eax,dword ptr [esi+ISO_DR_EXTENT]
        mov     word ptr [edi+DE_CLUSTER],ax
        shr     eax,16
        mov     word ptr [edi+DE_CLUSHI],ax

        mov     eax,dword ptr [esi+ISO_DR_SIZE]
        mov     dword ptr [edi+DE_SIZE],eax

        ; ROCK RIDGE MAY HAVE MOVED WHAT THIS RECORD POINTS AT.
        ; ISO9660 allows eight levels of directory and no more, so a
        ; deeper one is stored elsewhere with markers left behind: CL
        ; says "the directory you wanted is over there" on a record
        ; that otherwise looks like a file, RE marks the copy that was
        ; moved so it is not listed in both places, and PL on a ".."
        ; says where the parent really is - without which CD .. out of
        ; a relocated directory walks into the holding pen instead of
        ; back where it came from.
        cmp     byte ptr [iso_rr],0
        je      ien_norr
        mov     eax,[iso_rrcl]
        or      eax,eax
        jz      ien_nocl
        mov     word ptr [edi+DE_CLUSTER],ax
        shr     eax,16
        mov     word ptr [edi+DE_CLUSHI],ax
        or      byte ptr [edi+DE_ATTR],ATTR_DIR
        mov     dword ptr [edi+DE_SIZE],0
ien_nocl:
        mov     eax,[iso_rrpl]
        or      eax,eax
        jz      ien_nopl
        mov     word ptr [edi+DE_CLUSTER],ax
        shr     eax,16
        mov     word ptr [edi+DE_CLUSHI],ax
ien_nopl:
        cmp     byte ptr [iso_rrre],0
        je      ien_norr
        ; HIDDEN THE WAY A DELETED FAT ENTRY IS HIDDEN, which costs
        ; nothing and teaches nobody anything new: every walker above
        ; this layer already skips an entry whose first byte is E5h.
        mov     byte ptr [edi],0E5h
ien_norr:

        call    iso_stamp               ; DE_DATE / DE_TIME

        mov     esi,edi
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
;--- the synthetic volume label, entry zero of the root --------------
ien_label:
        call    iso_label
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret

;=====================================================================
; iso_label - the volume name, as the root-directory entry with
;             ATTR_VOLUME that DIR looks for.  ESI = iso_fatent.
;
; FAT KEEPS THE LABEL IN A DIRECTORY ENTRY AND NEITHER OF THESE TWO
; FILESYSTEMS DOES - ISO9660 keeps it in the primary descriptor and
; UDF in the logical volume descriptor - so both of them arrive here,
; where one is made out of iso_volid.  Synthesising it means the label
; is found by the code that already knows how to find one.
;=====================================================================
iso_label:
        push    eax
        push    ecx
        push    edi
        mov     byte ptr [iso_lname],0  ; a label is never a long name
        mov     edi,OFFSET iso_fatent
        push    edi
        xor     al,al
        mov     ecx,32
        cld
        rep     stosb
        pop     edi

        ; eleven bytes, space padded, out of the descriptor's name
        mov     esi,OFFSET iso_volid
        mov     ecx,11
ienl_copy:
        mov     al,[esi]
        or      al,al
        jz      ienl_pad
        call    iso_upcase
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     ienl_copy
        jmp     short ienl_done
ienl_pad:
        mov     al,' '
        rep     stosb
ienl_done:
        mov     esi,OFFSET iso_fatent
        mov     byte ptr [esi+DE_ATTR],ATTR_VOLUME
        pop     edi
        pop     ecx
        pop     eax
        ret

ien_none:
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; iso_stamp - the record at ESI has a seven-byte recording time; DOS
;             wants two packed words.  EDI = the FAT entry.
;
; The seven bytes are year-1900, month, day, hour, minute, second and
; a quarter-hour offset from GMT, all binary.  The offset is ignored:
; DOS timestamps have never carried a timezone and inventing one here
; would make the same disc read differently in two places.
;=====================================================================
iso_stamp:
        push    eax
        push    ebx
        push    ecx

        movzx   eax,byte ptr [esi+ISO_DR_DATE+0]        ; year - 1900
        add     eax,1900
        sub     eax,1980                ; ...and DOS counts from 1980
        jns     ist_year
        xor     eax,eax                 ; a disc predating DOS: clamp
ist_year:
        cmp     eax,127
        jbe     ist_yok
        mov     eax,127
ist_yok:
        shl     eax,9
        movzx   ebx,byte ptr [esi+ISO_DR_DATE+1]        ; month
        and     ebx,0Fh
        shl     ebx,5
        or      eax,ebx
        movzx   ebx,byte ptr [esi+ISO_DR_DATE+2]        ; day
        and     ebx,1Fh
        or      eax,ebx
        mov     word ptr [edi+DE_DATE],ax

        movzx   eax,byte ptr [esi+ISO_DR_DATE+3]        ; hour
        and     eax,1Fh
        shl     eax,11
        movzx   ebx,byte ptr [esi+ISO_DR_DATE+4]        ; minute
        and     ebx,3Fh
        shl     ebx,5
        or      eax,ebx
        movzx   ebx,byte ptr [esi+ISO_DR_DATE+5]        ; second, in twos
        shr     ebx,1
        and     ebx,1Fh
        or      eax,ebx
        mov     word ptr [edi+DE_TIME],ax

        pop     ecx
        pop     ebx
        pop     eax
        ret

;--- iso_upcase - AL in place.  A disc conforming to the standard is
;--- upper case already; one burned by a tool that did not care is not.
iso_upcase:
        cmp     al,'a'
        jb      iuc_out
        cmp     al,'z'
        ja      iuc_out
        sub     al,20h
iuc_out:
        ret

IFDEF ISO_DEBUG
;=====================================================================
; iso_selftest - mount the first CD in the machine and read its root
;                directory out loud.  BUILD iso.
;
; The same shape of proof as ATAPI's: each line can only be printed if
; everything under it worked, so a volume name says the descriptor
; walk found a primary and parsed it, and a list of names says the
; directory walk crossed records, blocks and the both-endian fields
; between them.
;=====================================================================
iso_selftest:
        pushad
        mov     esi,OFFSET msgIsoTry
        call    dword ptr ds:[IOS_CONSTR]

        ; A MARKER PER STEP, because the first run of this stopped with
        ; nothing on the screen at all and "it hung somewhere in here"
        ; is not a measurement.  One character costs nothing and turns
        ; a hang into a location.
        mov     al,'1'
        call    dword ptr ds:[IOS_CONOUT]
        xor     al,al
        mov     ecx,0
        call    dword ptr ds:[IOS_CDINFO]
        jc      ist_nodrive
        mov     al,'2'
        call    dword ptr ds:[IOS_CONOUT]

        xor     al,al
        call    iso_mount
        jc      ist_nodisc
        mov     al,'3'
        call    dword ptr ds:[IOS_CONOUT]

        mov     esi,OFFSET msgIsoVol
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET iso_volid
        call    dword ptr ds:[IOS_CONSTR]

        mov     esi,OFFSET msgIsoRoot
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr [iso_rootext]
        mov     edx,dword ptr [iso_rootlen]
        call    iso_diropen
        mov     ebp,12                  ; enough to show it is walking
ist_loop:
        call    iso_dirnext
        jc      ist_endlist
        mov     edi,OFFSET iso_n83
        mov     ebx,ebp
        call    iso_dosname
        mov     byte ptr [iso_n83+11],0
        push    esi
        mov     esi,OFFSET iso_n83
        call    dword ptr ds:[IOS_CONSTR]
        pop     esi
        mov     esi,OFFSET msgIsoBar
        call    dword ptr ds:[IOS_CONSTR]
        dec     ebp
        jnz     ist_loop
ist_endlist:
        mov     esi,OFFSET msgIsoEol
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret
ist_nodisc:
        mov     esi,OFFSET msgIsoNoDisc
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret
ist_nodrive:
        or      ecx,ecx
        jnz     ist_nodisc              ; a drive, but it would not answer
        mov     esi,OFFSET msgIsoNoDrv
        call    dword ptr ds:[IOS_CONSTR]
        popad
        ret

msgIsoTry     db 'ISO9660: ',0
msgIsoVol     db 'volume "',0
msgIsoRoot    db '" root: ',0
msgIsoBar     db ' ',0
msgIsoNoDisc  db 'no readable disc',13,10,0
msgIsoNoDrv   db 'no CD-ROM drive',13,10,0
msgIsoEol       db 13,10,0
iso_n83       db 12 dup (0)
ENDIF
