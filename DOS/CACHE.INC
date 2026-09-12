;=====================================================================
; CACHE.INC - the disk cache
;
; A sector cache underneath disk_read and disk_write, keyed by the
; PHYSICAL drive and the absolute LBA - not by drive letter and not by
; cluster.  That is the level the kernel already speaks at the bottom
; of CRITERR.INC, and it is the only level at which the question
; "somebody just wrote sector N of disk 80h, what do we hold that is
; now wrong?" has an answer: several volumes share a disk, a guest's
; INT 13h addresses the disk and not the letter, and INT 25h/26h are
; handed a letter and immediately turn it into a disk.
;
; It is NOT the same thing as the caches in FAT.INC.  Those are the
; filesystem's working set - the FAT, the directory sector being
; walked, the cluster being read - one of each, per volume, and they
; exist so that the filesystem does not re-read what it is in the
; middle of using.  This is a general store of SECTORS, shared by
; every volume on every disk, and it exists so that the disk is not
; spoken to twice for the same sector.  The two stack: a FAT sector
; the filesystem drops still sits here, and comes back without a
; transfer.
;
;---------------------------------------------------------------------
; WHAT IT CACHES, AND WHAT IT DELIBERATELY DOES NOT
;
; Blocks of eight sectors - four kilobytes - with a valid bit and a
; dirty bit per sector.  A miss on a block nothing is held for reads
; the WHOLE block, which is the read-ahead and is nearly free: one
; command instead of one, for eight sectors instead of one, and the
; seventh sector of a directory is usually wanted right after the
; first.  A block that is only partly present is filled a run at a
; time, so a sector already held is never re-read - and, more to the
; point, a DIRTY sector is never overwritten by the older copy still
; on the disk.
;
; TRANSFERS OF CC_BIGXFER SECTORS OR MORE GO STRAIGHT PAST.  A
; sixteen-kilobyte cluster is file data: it is read once, it would
; evict every piece of metadata in the cache to hold something nobody
; will ask for again, and it is already one efficient transfer.  The
; cache is for the small I/O - the FAT, directories, boot sectors,
; partition tables, the same eight sectors of a batch file read again
; on the next line - which is where the arm movement actually is.  A
; big READ still flushes anything dirty underneath it first, and a big
; WRITE still drops what we hold of those sectors, or the cache would
; be describing a disk that has moved on without it.
;
;---------------------------------------------------------------------
; WRITE CACHING, AND WHEN IT IS OVER
;
; Lazy writing is on by default for FIXED DISKS ONLY, and the deal is
; that every dirty sector is on the medium before the user can type
; the next command.  dos_commit is what makes that true, and it is
; called from three places: INT 21h AH=0Dh, which is the function DOS
; has always had for exactly this and which COMMAND.COM now calls
; before it prints a prompt; INT 21h AH=4Ch, a program ending; and
; INT 2Fh AX=4A10h BX=1, which is what a SMARTDRV-aware program calls.
; So the window in which unwritten data exists is one command long.
; Turning the machine off in the middle of a command can lose that
; command's writes; turning it off at the prompt cannot.
;
; FLOPPIES, CD-ROMS AND LOADED BLOCK DRIVERS ARE READ-CACHED ONLY.
; The argument is the same for all three and it is about what the
; kernel can see.  A floppy can be swapped between two commands and
; nothing tells us; an ATAPI drive reports a disc change only as a
; failure on the next command it is given; a loaded block driver is
; somebody else's code and may be a RAM disk, a network redirector or
; a compressed volume keeping state of its own.  Deferring a write to
; any of them means holding data for a medium that may not be there,
; so their writes go out immediately and only the read copy is kept.
; Their blocks are also dropped at every commit point, which bounds a
; stale read to inside one command - the same window the write cache
; runs in, and the most this kernel can promise without a disk-change
; line it does not have.
;
; VERIFY DEFEATS LAZY WRITING ON PURPOSE.  21h/2Eh asks for a write to
; be read back and compared, and there is nothing to read back until
; the sector has been written - so with VERIFY on, every write is a
; write-through and dio_verify sees the medium it was asked about.
;
;---------------------------------------------------------------------
; MEMORY, AND GIVING IT BACK
;
; The cache buys memory from the arena in chunks of CC_CHUNKBLKS
; blocks, and it can hand chunks back.  It starts at the minimum,
; grows towards the maximum on a miss while there is room to spare,
; and - this is the part that matters - arena_alloc asks it for memory
; back before it reports that there is none.  So a program that wants
; sixteen megabytes gets them: the cache shrinks to its floor, dirty
; blocks flushed on the way out, and an allocation that would have
; failed succeeds.  The floor is never crossed, because a cache that
; can be reduced to nothing is one that quietly stops existing on the
; machine that needed it most.
;
; CACHE= in CONFIG.SYS sets the two bounds and the lazy-write switch:
;
;       CACHE=max[,min[,ON|OFF]]        sizes in kilobytes
;       CACHE=OFF                       no cache at all
;
;---------------------------------------------------------------------
; RE-ENTRANCY, AND THE TWO WAYS A DIRTY BLOCK GOES OUT
;
; cache_io is called FROM dio_try, which keeps the transfer it may
; have to retry in the dio_ statics.  Anything this file does that
; ends up back in disk_write overwrites them, and the outer retry then
; repeats the wrong transfer.  So there are two write-back paths and
; they are not interchangeable:
;
;   cache_wbsilent  from inside cache_io - the watermark flush and the
;                   reclaim.  Goes to dio_raw, raises nothing, and on
;                   failure leaves the block dirty and sets
;                   cache_error.
;   cache_wbloud    from cache_commit, which runs at the top of an INT
;                   21h and is inside nothing.  Goes through
;                   disk_wrsure, so a failure raises the ordinary
;                   critical error with Abort, Retry, Fail.
;
; A sector that will not write therefore stays dirty, is tried again,
; and the user is told about it at the commit - which is the same
; place they are told about everything else that went wrong during the
; command.  cache_busy makes every nested call fall straight through
; to the drive, so neither path can re-enter this file.
;
; THE SWEEPS THAT DROP BLOCKS WALK THE CHUNKS AND NOT THE LRU LIST,
; and that is not a style choice: cache_unhook MOVES a block to the
; LRU end, so a walk that reads the next pointer after unhooking has
; had the list rewritten underneath it.  Sweeps that only write blocks
; back leave the order alone and do walk the list, oldest first,
; because the oldest dirty block is the one whose coalescing is over.
;
; Included by DOSMAIN.ASM, after CRITERR.INC.
;=====================================================================

CC_BLKSECS      EQU     8               ; sectors in one cache block
CC_BLKSIZE      EQU     CC_BLKSECS * 512
CC_KEYMASK      EQU     0FFFFFFF8h      ; LBA -> block key.  Spelled out
                                        ; rather than derived, and asserted
IF CC_BLKSECS - 8
.ERR    ; CC_KEYMASK is written out for CC_BLKSECS = 8
ENDIF

CC_CHUNKBLKS    EQU     16              ; blocks bought in one arena block
CC_CHUNKDATA    EQU     CC_CHUNKBLKS * CC_BLKSIZE       ; 64K of payload
CC_HASHSIZE     EQU     512             ; buckets - a power of two
CC_HASHMASK     EQU     CC_HASHSIZE - 1

; A transfer of this many sectors or more is not cache traffic.  Eight
; kilobytes is above every piece of metadata this kernel moves - the
; largest is a FAT12 FAT at sixteen sectors, which is exactly two
; blocks and is cached - and below every cluster worth calling data.
CC_BIGXFER      EQU     17

; What the arena must still be able to offer AFTER the cache has taken
; another chunk.  Growth is supposed to use memory nobody wants; this
; is the line between that and taking memory somebody is about to.
CC_RESERVE      EQU     00100000h       ; one megabyte

; Cache drive ids.  0-1 and 80h-81h are BIOS drive numbers and E0h
; upwards are DEVLOAD's pseudo-drives; this is the range for ATAPI
; units, which have no drive number of their own by design and so are
; given one no BIOS and no driver can collide with.
CC_CDBASE       EQU     0F0h
CC_UNITS        EQU     32              ; rows in cache_drvst

; Defaults, worked out from the size of the arena.  A machine with
; less than CC_ARENAMIN of it gets no cache at all: the whole point is
; to spend memory nobody else needs, and on a four-megabyte 386 there
; is no such memory.
CC_ARENAMIN     EQU     00200000h       ; 2M of arena
CC_MAXDIV       EQU     8               ; ceiling: an eighth of the arena
CC_MAXFLOOR     EQU     00040000h       ; ...but at least 256K
CC_MAXCAP       EQU     00800000h       ; ...and never more than 8M
CC_MINDIV       EQU     4               ; floor: a quarter of the ceiling
CC_MINFLOOR     EQU     00010000h       ; ...but at least 64K
CC_MINCAP       EQU     00080000h       ; ...and never more than 512K

; Sizes CACHE= will take, so that a typo cannot ask for a cache larger
; than the machine or small enough to be pure overhead.
CC_USERMIN      EQU     00010000h       ; 64K
CC_USERMAX      EQU     04000000h       ; 64M

; Dirty blocks that start a background flush, and how far down it
; goes.  Fractions of the cache rather than counts, so the numbers
; mean the same thing at 256K as at 8M.
CC_HIDIV        EQU     2               ; flush at half the cache dirty
CC_LODIV        EQU     8               ; ...down to an eighth

; One block descriptor.
CBLK_KEY        EQU     00h             ; dd - block-aligned LBA
CBLK_DRV        EQU     04h             ; db - cache drive id
CBLK_VALID      EQU     05h             ; db - one bit per sector held
CBLK_DIRTY      EQU     06h             ; db - one bit per sector owed
CBLK_FLAGS      EQU     07h             ; db - CBF_*
CBLK_DATA       EQU     08h             ; dd - the CC_BLKSIZE payload
CBLK_HNEXT      EQU     0Ch             ; dd - hash chain, 0 = end
CBLK_HPREV      EQU     10h             ; dd - 0 = it is a bucket head
CBLK_LNEXT      EQU     14h             ; dd - towards the LRU end
CBLK_LPREV      EQU     18h             ; dd - towards the MRU end
CBLK_HIDX       EQU     1Ch             ; dd - which bucket holds it
CBLK_LEN        EQU     20h

CBF_INUSE       EQU     01h             ; the key means something

; One chunk: a header, CC_CHUNKBLKS descriptors, then the payload.
CCHK_NEXT       EQU     00h             ; dd - the next chunk, 0 = last
CCHK_DESC       EQU     04h             ; dd - the first descriptor
CCHK_DATA       EQU     08h             ; dd - the payload
CCHK_LEN        EQU     10h

CC_CHUNKBYTES   EQU     CCHK_LEN + CC_CHUNKBLKS * CBLK_LEN + CC_CHUNKDATA

; Per-unit permission, which the SMARTDRV API can narrow.
CCM_READ        EQU     01h
CCM_WRITE       EQU     02h

;=====================================================================
; cache_init - defaults out of the size of the arena, and the first
;              chunk.  Called from dos_entry after arena_init and
;              before anything reads a sector.
;
; CONFIG.SYS runs much later and may replace both bounds; that is
; cfg_cache's business and it calls cache_setsize.  The cache exists
; before then because CONFIG.SYS is itself read off the disk, and
; there is no reason for the one file every boot reads to be the one
; file that goes uncached.
;=====================================================================
cache_init:
        pushad
        mov     edi,OFFSET cache_hash   ; a raw binary has no BSS: the
        xor     eax,eax                 ; table holds whatever the loader
        mov     ecx,CC_HASHSIZE         ; left in memory until it is told
        rep     stosd                   ; otherwise

        mov     edi,OFFSET cache_drvst
        mov     ecx,CC_UNITS
        mov     al,CCM_READ OR CCM_WRITE        ; what a drive may do is
        rep     stosb                           ; then narrowed by
                                                ; cache_mode, which knows
                                                ; what kind of drive it is
        mov     dword ptr [cache_chunks],0
        mov     dword ptr [cache_mru],0
        mov     dword ptr [cache_lru],0
        mov     dword ptr [cache_nblk],0
        mov     dword ptr [cache_used],0
        mov     dword ptr [cache_curb],0
        mov     dword ptr [cache_dirty],0
        mov     dword ptr [cache_hitl],0
        mov     dword ptr [cache_hith],0
        mov     dword ptr [cache_missl],0
        mov     dword ptr [cache_missh],0
        mov     byte ptr [cache_error],0
        mov     byte ptr [cache_busy],0
        mov     byte ptr [cache_lazy],1

        ; How big is the arena?  arena_top is the first byte past it.
        mov     eax,[arena_top]
        sub     eax,ARENA_BASE
        mov     [cache_arena],eax
        cmp     eax,CC_ARENAMIN
        jb      cci_none

        xor     edx,edx
        mov     ecx,CC_MAXDIV
        div     ecx
        cmp     eax,CC_MAXFLOOR
        jae     cci_maxlo
        mov     eax,CC_MAXFLOOR
cci_maxlo:
        cmp     eax,CC_MAXCAP
        jbe     cci_maxhi
        mov     eax,CC_MAXCAP
cci_maxhi:
        mov     [cache_maxb],eax

        xor     edx,edx
        mov     ecx,CC_MINDIV
        div     ecx
        cmp     eax,CC_MINFLOOR
        jae     cci_minlo
        mov     eax,CC_MINFLOOR
cci_minlo:
        cmp     eax,CC_MINCAP
        jbe     cci_minhi
        mov     eax,CC_MINCAP
cci_minhi:
        mov     [cache_minb],eax

        mov     byte ptr [cache_on],1
        call    cache_tomin
        popad
        ret

cci_none:
        ; Not enough memory for the cache to be worth having, and the
        ; way it says so is by not existing: every entry point tests
        ; cache_on first, and the machine then behaves exactly as it
        ; did before this file.
        mov     byte ptr [cache_on],0
        mov     dword ptr [cache_maxb],0
        mov     dword ptr [cache_minb],0
        popad
        ret

;---------------------------------------------------------------------
; cache_tomin - buy chunks until the cache is at least cache_minb, or
;               until the arena says no.  Failing is not an error: a
;               smaller cache is still a cache.
;---------------------------------------------------------------------
cache_tomin:
        push    eax
ctm_lp:
        mov     eax,[cache_curb]
        cmp     eax,[cache_minb]
        jae     ctm_out
        call    cache_addchunk
        jnc     ctm_lp
ctm_out:
        pop     eax
        ret

;=====================================================================
; cache_setsize - CONFIG.SYS has spoken.  EAX = max bytes, EDX = min
;                 bytes, CL = 1 for lazy writing, CH nonzero to turn
;                 the whole thing off.
;
; Everything held is committed and forgotten first: the sizes are
; about to change underneath it, and a CACHE= line after a DEVICE=
; line has had that driver's own I/O in the cache.
;=====================================================================
cache_setsize:
        pushad
        mov     [ccs_max],eax
        mov     [ccs_min],edx
        mov     [ccs_lazy],cl
        mov     [ccs_off],ch

        call    cache_commit                    ; whatever is owed, now
        call    cache_dropall                   ; then a clean slate
        call    cache_freeall

        cmp     byte ptr [ccs_off],0
        je      css_on
        mov     byte ptr [cache_on],0
        popad
        ret
css_on:
        mov     eax,[ccs_max]
        mov     edx,[ccs_min]
        cmp     edx,eax                         ; a floor above the ceiling
        jbe     css_order                       ; is a typo, not a request
        mov     edx,eax
css_order:
        mov     [cache_maxb],eax
        mov     [cache_minb],edx
        mov     al,[ccs_lazy]
        mov     [cache_lazy],al
        mov     byte ptr [cache_on],1
        call    cache_tomin
        popad
        ret

;=====================================================================
; cache_addchunk - one more CC_CHUNKBLKS blocks, out of the arena.
;                  CF set if they could not be had.
;
; The new blocks go on at the LRU end rather than the MRU end: they
; hold nothing, so they are exactly what the next miss should take,
; and putting them anywhere else means evicting a block with data in
; it while empty ones sit in the list.
;=====================================================================
cache_addchunk:
        pushad
        cmp     byte ptr [cache_busy],0
        jne     cac_no                          ; never from inside a
                                                ; reclaim, which is the
                                                ; opposite errand
        mov     eax,[cache_curb]
        add     eax,CC_CHUNKDATA
        cmp     eax,[cache_maxb]
        ja      cac_no                          ; that would be too much

        ; DO NOT TAKE THE LAST OF THE ARENA.  The cache is meant to be
        ; living off memory nobody wants; growing into the last
        ; megabyte turns it into the thing that made a program fail to
        ; load, and the program cannot ask for the memory back until
        ; it has already failed.
        ; cache_busy is set for the WHOLE arena conversation and not
        ; just the allocation.  arena_alloc asks the cache for memory
        ; back when it fails, and both calls below can fail: without
        ; the flag, a cache deciding whether it could grow would first
        ; shrink itself in order to answer the question.
        mov     byte ptr [cache_busy],1
        call    arena_avail                     ; -> EAX = largest free
        cmp     eax,CC_CHUNKBYTES + CC_RESERVE
        jb      cac_busyno
        mov     ebx,CC_CHUNKBYTES
        call    arena_alloc
        mov     byte ptr [cache_busy],0
        jc      cac_no

        ; header, descriptors, payload - in that order, one allocation
        mov     esi,eax                         ; ESI = the chunk header
        lea     edi,[esi+CCHK_LEN]              ; EDI = first descriptor
        mov     [esi+CCHK_DESC],edi
        mov     eax,edi
        add     eax,CC_CHUNKBLKS * CBLK_LEN
        mov     [esi+CCHK_DATA],eax             ; EAX = first payload block
        mov     ebx,[cache_chunks]
        mov     [esi+CCHK_NEXT],ebx
        mov     [cache_chunks],esi

        mov     ecx,CC_CHUNKBLKS
cac_blk:
        mov     dword ptr [edi+CBLK_KEY],0
        mov     byte ptr [edi+CBLK_DRV],0
        mov     byte ptr [edi+CBLK_VALID],0
        mov     byte ptr [edi+CBLK_DIRTY],0
        mov     byte ptr [edi+CBLK_FLAGS],0
        mov     [edi+CBLK_DATA],eax
        mov     dword ptr [edi+CBLK_HNEXT],0
        mov     dword ptr [edi+CBLK_HPREV],0
        mov     dword ptr [edi+CBLK_HIDX],0
        mov     ebp,edi
        call    cache_lruadd_old                ; empty: take me first
        add     eax,CC_BLKSIZE
        add     edi,CBLK_LEN
        inc     dword ptr [cache_nblk]
        loop    cac_blk

        add     dword ptr [cache_curb],CC_CHUNKDATA
        popad
        clc
        ret
cac_busyno:
        mov     byte ptr [cache_busy],0
cac_no:
        popad
        stc
        ret

;=====================================================================
; cache_reclaim - EBX = bytes somebody else needs.  Hand back whole
;                 chunks until that much has been released or the
;                 floor is reached.  CF clear if anything was freed.
;
; THIS IS WHAT MAKES THE CACHE POLITE, and it is the reason the cache
; is allowed to grow large in the first place: memory it is holding is
; memory it will give up.  arena_alloc calls it on the path where the
; answer would otherwise be "no memory", and then walks the arena
; again.
;
; Dirty blocks in a chunk being released are written out first, not
; dropped, because they are the only copy.  A write that fails leaves
; cache_error set and the data goes with the chunk - the one place in
; this file where that is true, and accepted because the alternative
; is refusing memory to a program on a machine whose disk has already
; stopped working.
;=====================================================================
cache_reclaim:
        cmp     byte ptr [cache_on],0
        je      crc_no
        cmp     byte ptr [cache_busy],0
        jne     crc_no                          ; already inside ourselves
        pushad
        mov     [ccr_want],ebx
        mov     dword ptr [ccr_got],0
        mov     byte ptr [cache_busy],1
crc_lp:
        mov     eax,[ccr_got]
        cmp     eax,[ccr_want]
        jae     crc_done
        mov     eax,[cache_curb]
        cmp     eax,CC_CHUNKDATA
        jb      crc_done
        sub     eax,CC_CHUNKDATA
        cmp     eax,[cache_minb]
        jb      crc_done                        ; the floor is the floor
        call    cache_dropchunk
        jc      crc_done
        add     dword ptr [ccr_got],CC_CHUNKBYTES
        jmp     short crc_lp
crc_done:
        mov     byte ptr [cache_busy],0
        cmp     dword ptr [ccr_got],0
        je      crc_none
        popad
        clc
        ret
crc_none:
        popad
crc_no:
        stc
        ret

;---------------------------------------------------------------------
; cache_dropchunk - release the newest chunk.  CF set if there is
;                   none.  cache_busy is the caller's to set.
;
; The newest rather than the emptiest: the list is singly linked from
; the head, every chunk is the same size, and which one goes matters
; not at all once its blocks have been unhooked.
;---------------------------------------------------------------------
cache_dropchunk:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    ebp
        mov     esi,[cache_chunks]
        or      esi,esi
        jz      cdc_none

        mov     ebp,[esi+CCHK_DESC]
        mov     ecx,CC_CHUNKBLKS
cdc_blk:
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cdc_clean
        call    cache_wbsilent                  ; the only copy - write it
cdc_clean:
        call    cache_unhook                    ; out of the hash and the LRU
        call    cache_lrudel                    ; ...and out of the list for
                                                ; good: the memory is going
        add     ebp,CBLK_LEN
        dec     dword ptr [cache_nblk]
        loop    cdc_blk

        mov     eax,[esi+CCHK_NEXT]
        mov     [cache_chunks],eax
        mov     edx,esi
        call    arena_free
        sub     dword ptr [cache_curb],CC_CHUNKDATA
        pop     ebp
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
cdc_none:
        pop     ebp
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; cache_freeall - every chunk, unconditionally.  Nothing is written
;                 back: cache_setsize commits before it calls this.
;---------------------------------------------------------------------
cache_freeall:
        push    eax
        mov     byte ptr [cache_busy],1
cfa_lp:
        call    cache_dropchunk
        jnc     cfa_lp
        mov     byte ptr [cache_busy],0
        mov     dword ptr [cache_curb],0
        mov     dword ptr [cache_nblk],0
        mov     dword ptr [cache_used],0
        mov     dword ptr [cache_chunks],0
        mov     dword ptr [cache_mru],0
        mov     dword ptr [cache_lru],0
        mov     dword ptr [cache_dirty],0
        pop     eax
        ret

;=====================================================================
; The LRU list.  MRU at cache_mru, LRU at cache_lru, doubly linked so
; that a block can be lifted out of the middle in constant time -
; which is what every hit does.  LNEXT runs towards the LRU end and
; LPREV towards the MRU end.
;=====================================================================

; cache_lruadd - EBP to the MRU end.
cache_lruadd:
        push    eax
        mov     eax,[cache_mru]
        mov     [ebp+CBLK_LNEXT],eax
        mov     dword ptr [ebp+CBLK_LPREV],0
        or      eax,eax
        jz      cla_first
        mov     [eax+CBLK_LPREV],ebp
        jmp     short cla_head
cla_first:
        mov     [cache_lru],ebp
cla_head:
        mov     [cache_mru],ebp
        pop     eax
        ret

; cache_lruadd_old - EBP to the LRU end, which is where a block
; holding nothing belongs: it is what the next miss should take.
cache_lruadd_old:
        push    eax
        mov     eax,[cache_lru]
        mov     [ebp+CBLK_LPREV],eax
        mov     dword ptr [ebp+CBLK_LNEXT],0
        or      eax,eax
        jz      clo_first
        mov     [eax+CBLK_LNEXT],ebp
        jmp     short clo_tail
clo_first:
        mov     [cache_mru],ebp
clo_tail:
        mov     [cache_lru],ebp
        pop     eax
        ret

; cache_lrudel - EBP out of the list.
cache_lrudel:
        push    eax
        push    edx
        mov     eax,[ebp+CBLK_LPREV]
        mov     edx,[ebp+CBLK_LNEXT]
        or      eax,eax
        jz      cld_washead
        mov     [eax+CBLK_LNEXT],edx
        jmp     short cld_tail
cld_washead:
        cmp     [cache_mru],ebp                 ; ...or is simply not in it
        jne     cld_tail
        mov     [cache_mru],edx
cld_tail:
        or      edx,edx
        jz      cld_wastail
        mov     [edx+CBLK_LPREV],eax
        jmp     short cld_out
cld_wastail:
        cmp     [cache_lru],ebp
        jne     cld_out
        mov     [cache_lru],eax
cld_out:
        mov     dword ptr [ebp+CBLK_LNEXT],0
        mov     dword ptr [ebp+CBLK_LPREV],0
        pop     edx
        pop     eax
        ret

; cache_touch - EBP has just been used.
cache_touch:
        cmp     [cache_mru],ebp
        je      cto_out
        call    cache_lrudel
        call    cache_lruadd
cto_out:
        ret

;=====================================================================
; The hash.  EAX = block key, DL = cache drive id -> EAX = bucket.
;
; The key is a multiple of eight, so the low three bits carry nothing
; and go first.  The two folds bring the high bits of a large disk
; down into the index; the drive is multiplied by an odd number so
; that the same sector on two disks cannot land in the same bucket by
; construction.
;=====================================================================
cache_hashof:
        push    ecx
        shr     eax,3
        mov     ecx,eax
        shr     ecx,9
        xor     eax,ecx
        mov     ecx,eax
        shr     ecx,18
        xor     eax,ecx
        movzx   ecx,dl
        imul    ecx,37
        add     eax,ecx
        and     eax,CC_HASHMASK
        pop     ecx
        ret

;---------------------------------------------------------------------
; cache_find - AL = drive, EBX = block key.  EBP = the descriptor,
;              CF set if it is not held.  Does NOT touch the LRU
;              order; cache_get does that once it knows what it has.
;---------------------------------------------------------------------
cache_find:
        push    eax
        push    edx
        mov     dl,al
        mov     eax,ebx
        call    cache_hashof
        mov     ebp,[cache_hash+eax*4]
cfi_lp:
        or      ebp,ebp
        jz      cfi_no
        cmp     [ebp+CBLK_KEY],ebx
        jne     cfi_next
        cmp     [ebp+CBLK_DRV],dl
        je      cfi_yes
cfi_next:
        mov     ebp,[ebp+CBLK_HNEXT]
        jmp     short cfi_lp
cfi_yes:
        pop     edx
        pop     eax
        clc
        ret
cfi_no:
        pop     edx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; cache_hashadd / cache_hashdel - EBP into or out of its bucket.
;---------------------------------------------------------------------
cache_hashadd:
        push    eax
        push    edx
        mov     dl,[ebp+CBLK_DRV]
        mov     eax,[ebp+CBLK_KEY]
        call    cache_hashof
        mov     [ebp+CBLK_HIDX],eax
        mov     edx,[cache_hash+eax*4]
        mov     [ebp+CBLK_HNEXT],edx
        mov     dword ptr [ebp+CBLK_HPREV],0
        or      edx,edx
        jz      cha_first
        mov     [edx+CBLK_HPREV],ebp
cha_first:
        mov     [cache_hash+eax*4],ebp
        pop     edx
        pop     eax
        ret

cache_hashdel:
        push    eax
        push    edx
        mov     eax,[ebp+CBLK_HPREV]
        mov     edx,[ebp+CBLK_HNEXT]
        or      eax,eax
        jz      chd_washead
        mov     [eax+CBLK_HNEXT],edx
        jmp     short chd_next
chd_washead:
        mov     eax,[ebp+CBLK_HIDX]
        mov     [cache_hash+eax*4],edx
chd_next:
        or      edx,edx
        jz      chd_out
        mov     eax,[ebp+CBLK_HPREV]
        mov     [edx+CBLK_HPREV],eax
chd_out:
        mov     dword ptr [ebp+CBLK_HNEXT],0
        mov     dword ptr [ebp+CBLK_HPREV],0
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; cache_unhook - EBP stops describing anything.  Out of the hash,
;                dirty accounting undone, and back to the LRU end
;                where an empty block belongs.  The BLOCK survives; it
;                is the KEY that is forgotten.
;
; Preserves every register, ECX and ESI included, because the sweeps
; that call it are counting with them.
;---------------------------------------------------------------------
cache_unhook:
        cmp     byte ptr [ebp+CBLK_FLAGS],0
        je      cuh_off
        call    cache_hashdel
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cuh_clean
        cmp     dword ptr [cache_dirty],0
        je      cuh_clean
        dec     dword ptr [cache_dirty]
cuh_clean:
        mov     byte ptr [ebp+CBLK_FLAGS],0
        cmp     dword ptr [cache_used],0
        je      cuh_off
        dec     dword ptr [cache_used]
cuh_off:
        mov     byte ptr [ebp+CBLK_VALID],0
        mov     byte ptr [ebp+CBLK_DIRTY],0
        mov     dword ptr [ebp+CBLK_KEY],0
        mov     byte ptr [ebp+CBLK_DRV],0
        call    cache_lrudel
        call    cache_lruadd_old
        ret

;=====================================================================
; cache_get - AL = drive, EBX = block key.  EBP = a descriptor holding
;             that block, CF set if there is no room for one.
;
; A hit goes to the MRU end and comes back as it stands, valid bits
; and all.  A miss takes the least recently used CLEAN block, growing
; the cache first if that is allowed.
;
; DIRTY BLOCKS ARE NEVER EVICTED HERE, and that is what keeps this
; routine out of disk_write.  cache_io runs the watermark flush before
; it gets this far, so under any ordinary load there is a clean block
; waiting; a cache that has somehow become entirely dirty simply says
; no, and the transfer goes to the disk uncached - which is correct
; and costs nothing but the cache.
;=====================================================================
cache_get:
        call    cache_find
        jnc     cg_hit
        cmp     dword ptr [cache_nblk],0
        je      cg_none

        ; Room to grow?  Only when every block already holds
        ; something, and THAT TEST IS THE CHEAP ONE ON PURPOSE.
        ; cache_addchunk has to ask arena_avail whether the memory can
        ; be spared, and arena_avail walks the whole arena; asking on
        ; every miss would put a walk of the memory map in front of
        ; every sector this machine reads.  A cache with an empty
        ; block in it does not need another one, so the question is
        ; only worth asking sixteen times per chunk - once each time
        ; the cache genuinely fills up.
        push    eax                             ; AL is the drive and EBX the
        push    ebx                             ; key, and both have to
                                                ; survive the question
        mov     eax,[cache_used]
        cmp     eax,[cache_nblk]
        jb      cg_nogrow
        call    cache_addchunk
cg_nogrow:
        pop     ebx
        pop     eax

        ; The victim: the oldest clean block.  Sixteen steps up the
        ; list is a bound and not a policy - if that many in a row are
        ; dirty then the watermark flush has not kept up, and refusing
        ; is better than writing from in here.
        push    ecx
        mov     ebp,[cache_lru]
        mov     ecx,16
cg_scan:
        or      ebp,ebp
        jz      cg_noroom
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cg_take
        mov     ebp,[ebp+CBLK_LPREV]
        loop    cg_scan
cg_noroom:
        pop     ecx
        jmp     short cg_none
cg_take:
        pop     ecx
        call    cache_unhook                    ; whatever it used to be
        mov     [ebp+CBLK_KEY],ebx
        mov     [ebp+CBLK_DRV],al
        mov     byte ptr [ebp+CBLK_VALID],0
        mov     byte ptr [ebp+CBLK_DIRTY],0
        mov     byte ptr [ebp+CBLK_FLAGS],CBF_INUSE
        inc     dword ptr [cache_used]
        call    cache_hashadd
        call    cache_lrudel
        call    cache_lruadd
        clc
        ret
cg_hit:
        call    cache_touch
        clc
        ret
cg_none:
        stc
        ret

;=====================================================================
; cache_mode - AL = cache drive id.  AL = the CCM_* bits that apply,
;              CF set if this drive is not cached at all.
;
; Two things are combined here: what the KIND of drive allows, which
; is fixed and argued at the top of this file, and what the SMARTDRV
; API has been told about this particular unit, which lives in
; cache_drvst.  A drive outside every known range is refused rather
; than guessed at.
;=====================================================================
cache_mode:
        push    ebx
        push    edx
        call    cache_unitof                    ; -> EBX = cache_drvst row
        jc      cmo_no
        mov     dl,[cache_drvst+ebx]
        test    dl,CCM_READ                     ; read caching off means the
        jz      cmo_no                          ; drive is not cached at all

        cmp     al,2
        jb      cmo_read                        ; a floppy
        cmp     al,80h
        jb      cmo_no                          ; 02h-7Fh is nothing
        cmp     al,82h
        jb      cmo_fixed                       ; the two hard disks
        jmp     short cmo_read                  ; a driver unit or an ATAPI
                                                ; one, both read-only
cmo_fixed:
        cmp     byte ptr [cache_lazy],0
        je      cmo_read
        mov     al,dl                           ; whatever the API allows
        pop     edx
        pop     ebx
        clc
        ret
cmo_read:
        mov     al,CCM_READ
        pop     edx
        pop     ebx
        clc
        ret
cmo_no:
        pop     edx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; cache_unitof - AL = cache drive id -> EBX = its row in cache_drvst.
;                CF set if it has none.  Preserves everything else.
;
;   0,1        -> 0,1        floppies
;   80h,81h    -> 2,3        hard disks
;   E0h..EFh   -> 4..19      loaded block driver units
;   F0h..F7h   -> 20..27     ATAPI units
;---------------------------------------------------------------------
cache_unitof:
        movzx   ebx,al
        cmp     bl,2
        jb      cuo_ok
        cmp     bl,80h
        jb      cuo_no
        cmp     bl,82h
        jae     cuo_hi
        sub     ebx,80h - 2
        jmp     short cuo_ok
cuo_hi:
        cmp     bl,DL_UNIT_BASE
        jb      cuo_no
        cmp     bl,CC_CDBASE
        jae     cuo_cd
        sub     ebx,DL_UNIT_BASE - 4
        jmp     short cuo_ok
cuo_cd:
        sub     ebx,CC_CDBASE - 20
cuo_ok:
        cmp     ebx,CC_UNITS
        jae     cuo_no
        clc
        ret
cuo_no:
        stc
        ret

;=====================================================================
; cache_io - the whole of what disk_read and disk_write see.
;
;   AL = drive   EBX = LBA   CX = sector count   EDI = buffer
;   EDX = IOS_RDBLK or IOS_WRBLK
;
;   CF set on failure with AH = the status byte, exactly as dio_raw.
;
; EVERY REGISTER BUT AH COMES BACK, AND THE WRAPPER IS WHY THIS FILE
; HAS ONE.  dio_raw, which this routine replaced in dio_try, preserves
; everything: the block layer runs inside a pushad and the service
; table entries are called through a pointer.  disk_read and
; disk_write inherited that and their callers rely on it - fat_flush
; counts the FAT copies it still owes in ESI ACROSS the call, and
; fat_secflush does the same.  Putting a routine underneath them that
; works in ESI turns that loop into one that never ends and writes the
; FAT across the whole disk.
;
; It is the identical bug the note above dio_verify describes, one
; level further down and arrived at from the other direction - that
; one was a push somebody forgot to add, this one a whole file that
; assumed the contract was "clobber what you like".  The clue was the
; same shape too: the machine booted, the first file written to the
; root came out empty, and the batch file being read at the time
; simply stopped, because its directory and its FAT had been rewritten
; underneath it.
;
; The inputs are latched in the cch_ statics regardless, so the body
; is free with the registers inside the wrapper - which is what makes
; the loops below readable.
;=====================================================================
cache_io:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        call    cache_iobody
        pop     ebp                     ; POP does not touch the flags, so
        pop     edi                     ; the CF this call is about survives
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

cache_iobody:
        cmp     byte ptr [cache_on],0
        je      cio_raw
        cmp     byte ptr [cache_busy],0
        jne     cio_raw
        test    cx,cx
        jz      cio_raw

        mov     [cch_drv],al
        mov     [cch_lba],ebx
        movzx   ecx,cx
        mov     [cch_cnt],ecx
        mov     [cch_buf],edi
        mov     byte ptr [cch_wr],0
        cmp     edx,IOS_WRBLK
        jne     cio_dir
        mov     byte ptr [cch_wr],1
cio_dir:
        call    cache_mode
        jc      cio_again                       ; cache_mode had AL
        mov     [cch_mode],al

        ; The background flush, and it happens HERE - before any block
        ; of this transfer has been touched, while nothing of ours is
        ; half finished.  See the header: the one thing this file must
        ; never do is go back through disk_write while dio_try is
        ; holding a transfer in the dio_ statics.
        call    cache_watermark

        mov     ecx,[cch_cnt]
        cmp     ecx,CC_BIGXFER
        jae     cio_bulk
        cmp     byte ptr [cch_wr],0
        jne     cio_write
        jmp     cio_read

;--- not cache traffic.  Keep the cache honest and stand aside -------
cio_bulk:
        mov     al,[cch_drv]
        mov     ebx,[cch_lba]
        mov     ecx,[cch_cnt]
        cmp     byte ptr [cch_wr],0
        je      cio_bulkrd
        call    cache_droprange         ; about to be overwritten wholesale
        jmp     short cio_again
cio_bulkrd:
        call    cache_flushrange        ; so that the read sees what we owe

;--- straight to the drive, with the transfer rebuilt from the statics
cio_again:
        mov     al,[cch_drv]
        mov     ebx,[cch_lba]
        mov     ecx,[cch_cnt]
        mov     edi,[cch_buf]
        mov     edx,IOS_RDBLK
        cmp     byte ptr [cch_wr],0
        je      cio_raw
        mov     edx,IOS_WRBLK
cio_raw:
        jmp     dio_raw                 ; its CF and AH are the answer

;=====================================================================
; The read path
;=====================================================================
cio_read:
        mov     eax,[cch_lba]
        mov     [cch_p],eax
        mov     eax,[cch_cnt]
        mov     [cch_left],eax
        mov     eax,[cch_buf]
        mov     [cch_ptr],eax
crd_lp:
        cmp     dword ptr [cch_left],0
        je      crd_ok
        call    cache_span
        mov     al,[cch_drv]
        mov     ebx,[cch_key]
        call    cache_get
        jc      crd_direct
        mov     [cch_blk],ebp

        ; is all of it here already?
        movzx   eax,byte ptr [ebp+CBLK_VALID]
        mov     edx,[cch_mask]
        and     eax,edx
        cmp     eax,edx
        je      crd_hit
        call    cache_fill
        jc      crd_fail
        call    cache_stmiss
        jmp     short crd_copy
crd_hit:
        call    cache_sthit
crd_copy:
        mov     ebp,[cch_blk]
        mov     esi,[ebp+CBLK_DATA]
        mov     eax,[cch_off]
        shl     eax,9
        add     esi,eax
        mov     edi,[cch_ptr]
        mov     ecx,[cch_n]
        shl     ecx,7                   ; sectors -> dwords
        rep     movsd
        jmp     short crd_step

;--- no block to be had: this span goes straight to the drive --------
crd_direct:
        call    cache_stmiss
        mov     ebx,[cch_p]
        mov     ecx,[cch_n]
        mov     edi,[cch_ptr]
        mov     al,[cch_drv]
        call    cache_rdmedia
        jc      crd_fail
crd_step:
        mov     eax,[cch_n]
        add     [cch_p],eax
        sub     [cch_left],eax
        shl     eax,9
        add     [cch_ptr],eax
        jmp     crd_lp
crd_ok:
        xor     ah,ah
        clc
        ret
crd_fail:
        stc
        ret

;---------------------------------------------------------------------
; cache_fill - the block in cch_blk is missing some of the sectors
;              this span wants.  Get them.  CF set, AH = status, if
;              the drive would not.
;
; An EMPTY block is filled WHOLE, and that is the read-ahead: one
; command for eight sectors, of which seven were not asked for and six
; will be wanted within the next few calls.  It can legitimately fail
; - the last block of a disk is not eight sectors long - so failing
; here is not an error, it is the signal to go and fetch only what was
; actually asked for.
;
; A block that already holds something is filled a RUN at a time over
; the requested range only, skipping what is present.  That is not an
; optimisation: a sector that is present may be DIRTY, and reading
; over the top of it would put the disk's older copy back.
;---------------------------------------------------------------------
cache_fill:
        mov     ebp,[cch_blk]
        cmp     byte ptr [ebp+CBLK_VALID],0
        jne     cfl_runs

        mov     ebx,[cch_key]
        mov     ecx,CC_BLKSECS
        mov     edi,[ebp+CBLK_DATA]
        mov     al,[cch_drv]
        call    cache_rdmedia
        jc      cfl_runs
        mov     ebp,[cch_blk]
        mov     byte ptr [ebp+CBLK_VALID],0FFh
        clc
        ret

;--- one run of missing sectors at a time ----------------------------
cfl_runs:
        ; An ATAPI unit cannot do this: its block is four of our
        ; sectors and it will not read one.  The whole-block read
        ; above is the only shape that fits one, so its failure is the
        ; answer rather than the start of something smaller.
        cmp     byte ptr [cch_drv],CC_CDBASE
        jae     cfl_fail

        mov     eax,[cch_off]
        mov     [cfl_i],eax
        add     eax,[cch_n]
        mov     [cfl_end],eax
cfl_lp:
        mov     ebp,[cch_blk]
        mov     eax,[cfl_i]
        cmp     eax,[cfl_end]
        jae     cfl_ok
        mov     ecx,eax
        mov     edx,1
        shl     edx,cl
        test    [ebp+CBLK_VALID],dl
        jz      cfl_gap
        inc     dword ptr [cfl_i]
        jmp     short cfl_lp

cfl_gap:
        mov     [cfl_run],eax                   ; first missing sector
        xor     edx,edx                         ; and how many follow it
cfl_grow:
        cmp     eax,[cfl_end]
        jae     cfl_got
        push    edx
        mov     ecx,eax
        mov     edx,1
        shl     edx,cl
        test    [ebp+CBLK_VALID],dl
        pop     edx                             ; POP leaves the flags alone
        jnz     cfl_got
        inc     eax
        inc     edx
        jmp     short cfl_grow
cfl_got:
        mov     [cfl_len],edx
        mov     ebx,[cch_key]
        add     ebx,[cfl_run]
        mov     ecx,[cfl_len]
        mov     edi,[ebp+CBLK_DATA]
        mov     eax,[cfl_run]
        shl     eax,9
        add     edi,eax
        mov     al,[cch_drv]
        call    cache_rdmedia
        jc      cfl_fail

        ; the run is present now: exactly those bits
        mov     ebp,[cch_blk]
        mov     ecx,[cfl_len]
        mov     eax,1
        shl     eax,cl
        dec     eax
        mov     ecx,[cfl_run]
        shl     eax,cl
        or      [ebp+CBLK_VALID],al
        mov     eax,[cfl_run]
        add     eax,[cfl_len]
        mov     [cfl_i],eax
        jmp     cfl_lp
cfl_ok:
        clc
        ret
cfl_fail:
        stc
        ret

;=====================================================================
; The write path
;=====================================================================
cio_write:
        mov     eax,[cch_lba]
        mov     [cch_p],eax
        mov     eax,[cch_cnt]
        mov     [cch_left],eax
        mov     eax,[cch_buf]
        mov     [cch_ptr],eax
crw_lp:
        cmp     dword ptr [cch_left],0
        je      crw_ok
        call    cache_span
        mov     al,[cch_drv]
        mov     ebx,[cch_key]
        call    cache_get
        jc      crw_through             ; no block: straight to the drive
        mov     [cch_blk],ebp

        ; the cache copy first, whichever way the sector then goes
        mov     edi,[ebp+CBLK_DATA]
        mov     eax,[cch_off]
        shl     eax,9
        add     edi,eax
        mov     esi,[cch_ptr]
        mov     ecx,[cch_n]
        shl     ecx,7
        rep     movsd
        mov     ebp,[cch_blk]
        mov     eax,[cch_mask]
        or      [ebp+CBLK_VALID],al

        test    byte ptr [cch_mode],CCM_WRITE
        jz      crw_through             ; a read-cached drive
        cmp     byte ptr [verify_on],0
        jne     crw_through             ; 2Eh wants something to read back

        cmp     byte ptr [ebp+CBLK_DIRTY],0
        jne     crw_wasdirty
        inc     dword ptr [cache_dirty]
crw_wasdirty:
        mov     eax,[cch_mask]
        or      [ebp+CBLK_DIRTY],al
        jmp     short crw_step

;--- the medium now, and the copy we hold is therefore clean ---------
crw_through:
        mov     ebx,[cch_p]
        mov     ecx,[cch_n]
        mov     edi,[cch_ptr]
        mov     edx,IOS_WRBLK
        mov     al,[cch_drv]
        call    dio_raw
        jc      crw_fail

        ; The block may or may not still be held, and if it is it may
        ; have owed these sectors before this call - a drive whose
        ; write caching was turned off by the API while dirty blocks
        ; were outstanding is exactly that case.
        push    eax
        push    ebx
        mov     al,[cch_drv]
        mov     ebx,[cch_key]
        call    cache_find
        pop     ebx
        pop     eax
        jc      crw_step
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      crw_step                ; nothing was owed anyway
        mov     eax,[cch_mask]
        not     eax
        and     [ebp+CBLK_DIRTY],al
        jnz     crw_step                ; still owes the rest
        cmp     dword ptr [cache_dirty],0
        je      crw_step
        dec     dword ptr [cache_dirty]
crw_step:
        mov     eax,[cch_n]
        add     [cch_p],eax
        sub     [cch_left],eax
        shl     eax,9
        add     [cch_ptr],eax
        jmp     crw_lp
crw_ok:
        xor     ah,ah
        clc
        ret

; THE WRITE DID NOT HAPPEN AND WE ARE HOLDING WHAT WAS TO BE WRITTEN.
; Keeping it would mean a later read of those sectors answering with
; bytes the disk has never seen - the program is told its write failed
; and then shown the new data anyway, which is a worse outcome than
; the failure.  So the span is forgotten and the next read goes to the
; medium.
;
; EXCEPT WHERE IT IS DIRTY, and that exception is not an edge case
; being tidied away: a sector can be owed from an EARLIER lazy write
; and be passing through the write-through path now because something
; turned lazy writing off in between.  Dropping it there would throw
; away a write the filesystem believes it has made, silently.  Dirty
; implies valid, so the mask is narrowed to the clean sectors and the
; rest are left for the commit to argue with.
crw_fail:
        push    eax
        push    ebx
        push    edx
        mov     al,[cch_drv]
        mov     ebx,[cch_key]
        call    cache_find
        jc      crw_fdone
        mov     eax,[cch_mask]
        movzx   edx,byte ptr [ebp+CBLK_DIRTY]
        not     edx
        and     eax,edx
        not     eax
        and     [ebp+CBLK_VALID],al
crw_fdone:
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; cache_span - from cch_p and cch_left, which block this part of the
;              transfer is in and how much of it belongs to that
;              block.  Fills cch_key, cch_off, cch_n and the sector
;              mask cch_mask.  Preserves all.
;=====================================================================
cache_span:
        push    eax
        push    ecx
        push    edx
        mov     eax,[cch_p]
        mov     edx,eax
        and     eax,CC_KEYMASK
        mov     [cch_key],eax
        sub     edx,eax
        mov     [cch_off],edx                   ; 0..CC_BLKSECS-1
        mov     ecx,CC_BLKSECS
        sub     ecx,edx
        cmp     ecx,[cch_left]
        jbe     csp_n
        mov     ecx,[cch_left]
csp_n:
        mov     [cch_n],ecx
        mov     eax,1
        shl     eax,cl
        dec     eax
        mov     ecx,edx
        shl     eax,cl
        mov     [cch_mask],eax
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; cache_rdmedia - AL = cache drive, EBX = LBA, ECX = sectors,
;                 EDI = buffer.  CF and AH as the driver gave them.
;
; The one place that knows an ATAPI unit is not a block drive.  Its
; blocks are 2048 bytes, so the sector number and the count are
; divided by four on the way through - which is exact for every
; request this file makes of one, because a cache block is eight
; sectors and eight sectors is two CD blocks, aligned.
;=====================================================================
cache_rdmedia:
        cmp     al,CC_CDBASE
        jae     crm_cd
        mov     edx,IOS_RDBLK
        jmp     dio_raw
crm_cd:
        sub     al,CC_CDBASE
        shr     ebx,2
        shr     ecx,2
        jz      crm_bad
        call    dword ptr ds:[IOS_CDREAD]
        jc      crm_bad
        xor     ah,ah
        clc
        ret
crm_bad:
        mov     ah,0Ch                  ; "general failure", as the block
        stc                             ; layer spells it
        ret

;=====================================================================
; cache_cdread - AL = ATAPI unit, EBX = 2048-byte block, EDI = buffer.
;                One block.  CF set if the drive would not.
;
; This is what ISO9660.INC calls instead of IOS_CDREAD, and it is the
; whole of the optical side: a CD block is four of our sectors, a
; cache block is two CD blocks, and a miss therefore reads two and
; keeps the one nobody asked for.  On a disc that is almost entirely
; sequential reading that is the right guess nearly every time.
;
; Never written to - see the header - so a disc change costs only the
; blocks, which iso_chkmedia drops through cache_dropdrv.
;=====================================================================
cache_cdread:
        cmp     byte ptr [cache_on],0
        je      ccd_pass
        cmp     byte ptr [cache_busy],0
        jne     ccd_pass
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        push    ebp
        movzx   eax,al
        add     eax,CC_CDBASE
        mov     [cch_drv],al
        call    cache_mode
        jc      ccd_pop
        mov     [cch_mode],al
        shl     ebx,2                           ; CD block -> our sectors
        mov     [cch_lba],ebx
        mov     dword ptr [cch_cnt],4
        mov     [cch_buf],edi
        mov     byte ptr [cch_wr],0
        call    cache_watermark
        call    cio_read
        pop     ebp                             ; POP leaves CF alone, which
        pop     edi                             ; is how the answer survives
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret
ccd_pop:
        pop     ebp
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
ccd_pass:
        mov     ecx,1
        call    dword ptr ds:[IOS_CDREAD]
        ret

;=====================================================================
; cache_wbsilent - EBP is dirty.  Write it out with dio_raw, raising
;                  nothing.  On failure the block stays dirty and
;                  cache_error is set, so the next commit tries again
;                  and complains properly.
;
; The dirty sectors go out in RUNS, and that coalescing is most of
; what the write cache is for: a FAT sector touched forty times during
; a copy is written once, and eight consecutive dirty sectors leave as
; one four-kilobyte transfer instead of eight.
;=====================================================================
cache_wbsilent:
        pushad
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cwb_out
        xor     esi,esi                         ; ESI = sector index
cwb_lp:
        cmp     esi,CC_BLKSECS
        jae     cwb_done
        mov     ecx,esi
        mov     eax,1
        shl     eax,cl
        test    [ebp+CBLK_DIRTY],al
        jnz     cwb_run
        inc     esi
        jmp     short cwb_lp
cwb_run:
        mov     edx,esi                         ; EDX = first of the run
        xor     edi,edi                         ; EDI = how long it is
cwb_grow:
        cmp     esi,CC_BLKSECS
        jae     cwb_go
        mov     ecx,esi
        mov     eax,1
        shl     eax,cl
        test    [ebp+CBLK_DIRTY],al
        jz      cwb_go
        inc     esi
        inc     edi
        jmp     short cwb_grow
cwb_go:
        push    esi
        push    edx
        push    edi
        mov     ebx,[ebp+CBLK_KEY]
        add     ebx,edx
        mov     ecx,edi
        mov     edi,[ebp+CBLK_DATA]
        mov     eax,edx
        shl     eax,9
        add     edi,eax
        mov     al,[ebp+CBLK_DRV]
        mov     edx,IOS_WRBLK
        call    dio_raw
        pop     edi
        pop     edx
        pop     esi
        jc      cwb_bad

        mov     ecx,edi                         ; clear exactly that run
        mov     eax,1
        shl     eax,cl
        dec     eax
        mov     ecx,edx
        shl     eax,cl
        not     eax
        and     [ebp+CBLK_DIRTY],al
        jmp     short cwb_lp
cwb_bad:
        mov     byte ptr [cache_error],1
        jmp     short cwb_out
cwb_done:
        cmp     dword ptr [cache_dirty],0
        je      cwb_out
        dec     dword ptr [cache_dirty]
cwb_out:
        popad
        ret

;=====================================================================
; cache_wbloud - EBP is dirty and we are NOT inside dio_try.  Write it
;                through disk_wrsure, so a failure raises the ordinary
;                critical error and the user is asked Abort, Retry,
;                Fail about the sector that would not write.
;
; disk_wrsure rather than disk_write because disk_write refuses when
; the CURRENT volume is an ISO one - a statement about the volume the
; filesystem happens to be standing on, not about the drive this block
; belongs to.  A commit can perfectly well happen while the current
; drive is the CD-ROM, and dirty blocks on the hard disk must not
; quietly stop being written because of where the user last typed a CD
; command.
;=====================================================================
cache_wbloud:
        pushad
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cwl_out
        xor     esi,esi
cwl_lp:
        cmp     esi,CC_BLKSECS
        jae     cwl_done
        mov     ecx,esi
        mov     eax,1
        shl     eax,cl
        test    [ebp+CBLK_DIRTY],al
        jnz     cwl_run
        inc     esi
        jmp     short cwl_lp
cwl_run:
        mov     edx,esi
        xor     edi,edi
cwl_grow:
        cmp     esi,CC_BLKSECS
        jae     cwl_go
        mov     ecx,esi
        mov     eax,1
        shl     eax,cl
        test    [ebp+CBLK_DIRTY],al
        jz      cwl_go
        inc     esi
        inc     edi
        jmp     short cwl_grow
cwl_go:
        push    esi
        push    edx
        push    edi
        mov     ebx,[ebp+CBLK_KEY]
        add     ebx,edx
        mov     ecx,edi
        mov     edi,[ebp+CBLK_DATA]
        mov     eax,edx
        shl     eax,9
        add     edi,eax
        mov     al,[ebp+CBLK_DRV]
        call    disk_wrsure
        pop     edi
        pop     edx
        pop     esi
        jc      cwl_bad

        mov     ecx,edi
        mov     eax,1
        shl     eax,cl
        dec     eax
        mov     ecx,edx
        shl     eax,cl
        not     eax
        and     [ebp+CBLK_DIRTY],al
        jmp     short cwl_lp
cwl_bad:
        ; The handler said Fail, or Abort came back as one.  The
        ; sector is not on the disk and there is nothing further to be
        ; done about it here; the block stays dirty so that the next
        ; commit says so again.
        mov     byte ptr [cache_error],1
        jmp     short cwl_out
cwl_done:
        cmp     dword ptr [cache_dirty],0
        je      cwl_out
        dec     dword ptr [cache_dirty]
cwl_out:
        popad
        ret

;=====================================================================
; cache_watermark - too much held and not written?  Write some of it.
;
; Called at the top of every cache_io, which is the only moment in a
; transfer when nothing of ours is half finished.  It flushes from the
; LRU end, because the oldest dirty block is the one least likely to
; be written to again and therefore the one whose coalescing is over.
;=====================================================================
cache_watermark:
        pushad
        mov     eax,[cache_nblk]
        or      eax,eax
        jz      cwm_out
        xor     edx,edx
        mov     ecx,CC_HIDIV
        div     ecx
        cmp     [cache_dirty],eax
        jb      cwm_out

        mov     eax,[cache_nblk]
        xor     edx,edx
        mov     ecx,CC_LODIV
        div     ecx
        mov     [cwm_low],eax
        mov     ebp,[cache_lru]
cwm_lp:
        or      ebp,ebp
        jz      cwm_out
        mov     eax,[cache_dirty]
        cmp     eax,[cwm_low]
        jbe     cwm_out
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cwm_next
        call    cache_wbsilent          ; preserves EBP, and moves nothing
cwm_next:
        mov     ebp,[ebp+CBLK_LPREV]
        jmp     short cwm_lp
cwm_out:
        popad
        ret

;=====================================================================
; cache_commit - every dirty sector in the cache goes to its medium,
;                properly, with critical errors raised.
;
; MUST NOT be called from inside dio_try - see the header.  The
; callers are dos_commit, cache_setsize and the SMARTDRV API, and all
; of them run at the top of a DOS call.
;=====================================================================
cache_commit:
        cmp     byte ptr [cache_on],0
        je      cco_out
        cmp     dword ptr [cache_dirty],0
        je      cco_clean
        pushad
        mov     byte ptr [cache_busy],1         ; the write-back must not
        mov     byte ptr [cache_error],0        ; come back through here
        mov     ebp,[cache_lru]
cco_lp:
        or      ebp,ebp
        jz      cco_done
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cco_next
        call    cache_wbloud            ; preserves EBP, and moves nothing
cco_next:
        mov     ebp,[ebp+CBLK_LPREV]
        jmp     short cco_lp
cco_done:
        mov     byte ptr [cache_busy],0
        popad
cco_clean:
        ; REMOVABLE MEDIA ARE FORGOTTEN AT EVERY COMMIT.  A floppy can
        ; be swapped between two commands and nothing tells us; an
        ; ATAPI drive says so only as a failure on the next command it
        ; is given.  Dropping what we hold of them here bounds a stale
        ; read to inside one command, which is the same window the
        ; write cache runs in and is the most this kernel can promise
        ; without a disk-change line it does not have.
        call    cache_droprem
cco_out:
        ret

;=====================================================================
; The sweeps that FORGET blocks.  Every one of them walks the chunk
; arrays rather than the LRU list, because cache_unhook moves a block
; to the LRU end and a list walk that read its next pointer afterwards
; would have been rewritten underneath.
;=====================================================================

;---------------------------------------------------------------------
; cache_droprem - forget every clean block belonging to a drive whose
;                 medium can be changed behind our back.  A dirty one
;                 is left alone: the commit that precedes this has
;                 already tried to write it, and forgetting it would
;                 throw the only copy away.
;---------------------------------------------------------------------
cache_droprem:
        cmp     byte ptr [cache_on],0
        je      cpr_out
        pushad
        mov     esi,[cache_chunks]
cpr_chunk:
        or      esi,esi
        jz      cpr_done
        mov     ebp,[esi+CCHK_DESC]
        mov     ecx,CC_CHUNKBLKS
cpr_blk:
        cmp     byte ptr [ebp+CBLK_FLAGS],0
        je      cpr_next
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        jne     cpr_next
        mov     al,[ebp+CBLK_DRV]
        cmp     al,2
        jb      cpr_drop                        ; a floppy
        cmp     al,CC_CDBASE
        jb      cpr_next
cpr_drop:
        call    cache_unhook
cpr_next:
        add     ebp,CBLK_LEN
        loop    cpr_blk
        mov     esi,[esi+CCHK_NEXT]
        jmp     short cpr_chunk
cpr_done:
        popad
cpr_out:
        ret

;---------------------------------------------------------------------
; cache_dropall - forget everything, writing nothing.  For the paths
;                 that have already committed.
;---------------------------------------------------------------------
cache_dropall:
        pushad
        mov     esi,[cache_chunks]
cda_chunk:
        or      esi,esi
        jz      cda_done
        mov     ebp,[esi+CCHK_DESC]
        mov     ecx,CC_CHUNKBLKS
cda_blk:
        call    cache_unhook
        add     ebp,CBLK_LEN
        loop    cda_blk
        mov     esi,[esi+CCHK_NEXT]
        jmp     short cda_chunk
cda_done:
        mov     dword ptr [cache_dirty],0
        popad
        ret

;---------------------------------------------------------------------
; cache_dropdrv - AL = cache drive id.  Forget everything belonging to
;                 it, writing nothing back.  For a medium that HAS
;                 been changed: what we hold describes a disc or a
;                 diskette that is no longer in the machine, and
;                 writing any of it back would be writing one disc's
;                 sectors onto another.
;---------------------------------------------------------------------
cache_dropdrv:
        cmp     byte ptr [cache_on],0
        je      cdd_out
        pushad
        mov     [cfr_drv],al
        mov     esi,[cache_chunks]
cdd_chunk:
        or      esi,esi
        jz      cdd_done
        mov     ebp,[esi+CCHK_DESC]
        mov     ecx,CC_CHUNKBLKS
cdd_blk:
        cmp     byte ptr [ebp+CBLK_FLAGS],0
        je      cdd_next
        mov     al,[cfr_drv]
        cmp     [ebp+CBLK_DRV],al
        jne     cdd_next
        call    cache_unhook
cdd_next:
        add     ebp,CBLK_LEN
        loop    cdd_blk
        mov     esi,[esi+CCHK_NEXT]
        jmp     short cdd_chunk
cdd_done:
        popad
cdd_out:
        ret

;---------------------------------------------------------------------
; cache_cddrop - AL = ATAPI unit.  The same thing for a disc.
;---------------------------------------------------------------------
cache_cddrop:
        push    eax
        movzx   eax,al
        add     eax,CC_CDBASE
        call    cache_dropdrv
        pop     eax
        ret

;---------------------------------------------------------------------
; cache_droprange - AL = drive, EBX = first LBA, ECX = sectors.
;                   Forget every block overlapping that range WITHOUT
;                   writing any of it back.  Preserves all.
;
; This is the after-somebody-else's-write call, and the discarding is
; the whole point: those sectors now hold what the other writer put
; there, and putting our copy back over the top would undo their work
; with data we know to be stale.  It is the same argument vol_syncdrv
; mode 1 makes one level up.
;---------------------------------------------------------------------
cache_droprange:
        cmp     byte ptr [cache_on],0
        je      cdg_out
        pushad
        mov     [cfr_drv],al
        mov     [cfr_lo],ebx
        add     ecx,ebx
        mov     [cfr_hi],ecx
        mov     esi,[cache_chunks]
cdg_chunk:
        or      esi,esi
        jz      cdg_done
        mov     ebp,[esi+CCHK_DESC]
        mov     ecx,CC_CHUNKBLKS
cdg_blk:
        call    cache_inrange
        jc      cdg_next
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cdg_drop

        ; A BLOCK CAN STRADDLE THE EDGE OF THE RANGE, and the two
        ; halves of it need opposite treatment.  Sectors INSIDE the
        ; range have been superseded by whatever the other writer put
        ; there, so our dirty copy of them is obsolete and is
        ; discarded - that is the whole point of this routine.
        ; Sectors OUTSIDE it are still the only copy in the machine,
        ; and dropping the block without writing them would lose a
        ; write the filesystem believes it has made.  Eight sectors
        ; either side of a boundary is not a rare case: a bulk write
        ; starts wherever the caller's cluster starts.
        call    cache_rangemask
        not     al
        and     [ebp+CBLK_DIRTY],al
        jz      cdg_undirty
        call    cache_wbsilent          ; what is left is owed
        jmp     short cdg_drop
cdg_undirty:
        cmp     dword ptr [cache_dirty],0
        je      cdg_drop
        dec     dword ptr [cache_dirty]
cdg_drop:
        call    cache_unhook
cdg_next:
        add     ebp,CBLK_LEN
        loop    cdg_blk
        mov     esi,[esi+CCHK_NEXT]
        jmp     short cdg_chunk
cdg_done:
        popad
cdg_out:
        ret

;---------------------------------------------------------------------
; cache_flushrange - AL = drive, EBX = first LBA, ECX = sectors.
;                    Anything dirty in that range goes out, silently.
;                    Preserves all.
;
; Silently because every caller is about to do I/O of its own to those
; sectors and will report its own errors; a block that will not write
; stays dirty and cache_error is set for the next commit.  This one
; moves nothing, so it walks the chunks only for symmetry with the
; drops beside it.
;---------------------------------------------------------------------
cache_flushrange:
        cmp     byte ptr [cache_on],0
        je      cfr_out
        cmp     dword ptr [cache_dirty],0
        je      cfr_out
        pushad
        mov     [cfr_drv],al
        mov     [cfr_lo],ebx
        add     ecx,ebx
        mov     [cfr_hi],ecx
        mov     esi,[cache_chunks]
cfr_chunk:
        or      esi,esi
        jz      cfr_done
        mov     ebp,[esi+CCHK_DESC]
        mov     ecx,CC_CHUNKBLKS
cfr_blk:
        cmp     byte ptr [ebp+CBLK_DIRTY],0
        je      cfr_next
        call    cache_inrange
        jc      cfr_next
        call    cache_wbsilent
cfr_next:
        add     ebp,CBLK_LEN
        loop    cfr_blk
        mov     esi,[esi+CCHK_NEXT]
        jmp     short cfr_chunk
cfr_done:
        popad
cfr_out:
        ret

;---------------------------------------------------------------------
; cache_inrange - CF clear if EBP is a live block on cfr_drv whose
;                 eight sectors overlap [cfr_lo, cfr_hi).
;---------------------------------------------------------------------
cache_inrange:
        push    eax
        cmp     byte ptr [ebp+CBLK_FLAGS],0
        je      cir_no
        mov     al,[cfr_drv]
        cmp     [ebp+CBLK_DRV],al
        jne     cir_no
        mov     eax,[ebp+CBLK_KEY]
        cmp     eax,[cfr_hi]
        jae     cir_no                          ; starts after it ends
        add     eax,CC_BLKSECS
        cmp     eax,[cfr_lo]
        jbe     cir_no                          ; ends before it starts
        pop     eax
        clc
        ret
cir_no:
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; cache_rangemask - AL = the bits of EBP's block that lie inside
;                   [cfr_lo, cfr_hi).  cache_inrange has already said
;                   they overlap, so the answer is never zero.
;---------------------------------------------------------------------
cache_rangemask:
        push    ebx
        push    ecx
        push    edx
        mov     ebx,[ebp+CBLK_KEY]
        mov     edx,[cfr_lo]                    ; first sector inside
        sub     edx,ebx
        jnc     crk_lo
        xor     edx,edx                         ; the range starts below us
crk_lo:
        cmp     edx,CC_BLKSECS
        jb      crk_lo2
        mov     edx,CC_BLKSECS
crk_lo2:
        mov     ecx,[cfr_hi]                    ; one past the last
        sub     ecx,ebx
        cmp     ecx,CC_BLKSECS
        jbe     crk_hi
        mov     ecx,CC_BLKSECS                  ; ...or it runs past us
crk_hi:
        sub     ecx,edx
        jbe     crk_none
        mov     eax,1
        shl     eax,cl
        dec     eax
        mov     ecx,edx
        shl     eax,cl
        jmp     short crk_out
crk_none:
        xor     eax,eax
crk_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; The statistics the SMARTDRV interface reports.  Two 64-bit counters
; kept as pairs of dwords, because that is the shape DX:BX and DI:SI
; hand back and because a machine that has been up for a day overflows
; thirty-two bits of hits.
;=====================================================================
cache_sthit:
        add     dword ptr [cache_hitl],1
        adc     dword ptr [cache_hith],0
        ret
cache_stmiss:
        add     dword ptr [cache_missl],1
        adc     dword ptr [cache_missh],0
        ret

;=====================================================================
; dos_commit - INT 21h AH=0Dh, and everything else that means "the
;              disk should now say what the filesystem says".
;
; TWO LAYERS, IN ORDER.  The filesystem's own caches are flushed first
; - the FAT, the directory sector, whatever fat_flush is holding for
; each mounted volume - and those flushes turn into writes that land
; in THIS cache.  Committing this cache first and the filesystem
; second would leave the filesystem's writes sitting here unwritten,
; which is the same bug one layer down.
;
; It runs before every command prompt, which is the promise made at
; the top of this file, and from 4Ch, which is where a program that
; wrote a file and exited becomes a file on the disk.
;=====================================================================
dos_commit:
        pushad

        ; Every mounted volume, by the DISK it lives on.  vol_syncdrv
        ; already walks every volume on a disk and flushes the mounted
        ; ones, so this only has to name each disk once; the inner
        ; scan is what stops a disk with four letters on it being
        ; flushed four times over.
        movzx   ecx,byte ptr [drv_count]
        or      ecx,ecx
        jz      dcm_cache
        xor     ebx,ebx                         ; EBX = drive letter
dcm_one:
        mov     eax,ebx
        imul    eax,DRV_LEN
        add     eax,OFFSET drv_tbl
        test    byte ptr [eax+DRV_FLAGS],DRVF_PRESENT
        jz      dcm_next
        test    byte ptr [eax+DRV_FLAGS],DRVF_CDROM
        jnz     dcm_next                        ; nothing is ever owed to one
        mov     dl,[eax+DRV_BIOS]

        xor     esi,esi                         ; done this disk already?
dcm_dup:
        cmp     esi,ebx
        jae     dcm_new
        mov     eax,esi
        imul    eax,DRV_LEN
        add     eax,OFFSET drv_tbl
        test    byte ptr [eax+DRV_FLAGS],DRVF_PRESENT
        jz      dcm_dupnext
        test    byte ptr [eax+DRV_FLAGS],DRVF_CDROM
        jnz     dcm_dupnext
        cmp     [eax+DRV_BIOS],dl
        je      dcm_next                        ; yes - an earlier letter
dcm_dupnext:
        inc     esi
        jmp     short dcm_dup
dcm_new:
        push    ecx
        push    ebx
        mov     al,dl
        mov     ah,0                            ; mode 0: flush, not forget
        xor     ebx,ebx
        mov     ecx,0FFFFFFFFh
        call    vol_syncdrv
        pop     ebx
        pop     ecx
dcm_next:
        inc     ebx
        dec     ecx
        jnz     dcm_one

dcm_cache:
        call    cache_commit
        popad
        ret

;=====================================================================
; cache_int2F - INT 2Fh AX=4A10h, the SMARTDRV multiplex.
;
; CF clear if the call was ours, CF set to let it go down the chain.
;
; WHY THE KERNEL ANSWERS THIS AT ALL.  It is the same argument
; MSCDEX.INC makes about AH=15h: what programs need is the API, not
; the program.  SMARTDRV.EXE's own start-up makes this call and exits
; if somebody answers, so answering it stops a second cache being
; loaded on top of this one - two caches holding two versions of one
; sector being precisely the failure this file exists to avoid.
; Windows asks as well, and so does every disk utility that wants to
; know whether writes are being held before it touches a disk.
;
; The numbers come out of S:\DOS6SRC\dev\smartdrv\int2f.asm, which is
; SMARTDRV 4.0 itself.  The version reported is 4.00 because that is
; the version this interface belongs to; a program that reads 4.00 and
; then finds a working cache has been told the truth about the only
; thing it asked.
;
;   BX=0  installation check and statistics
;   BX=1  commit everything now
;   BX=2  commit and invalidate
;   BX=3  per-drive read/write caching, get or set
;   BX=4  element size and count
;   BX=6  stop caching a drive
;
; BX=5 and BX=7 are NOT answered and go down the chain.  Both hand the
; caller something about a DEVICE DRIVER - the double-buffering
; driver, and the original block device a cached drive was stacked on
; top of - and there is neither here: this cache is not a driver
; stacked on a driver, it is the block layer itself.  A synthetic
; answer would invite a program to call a strategy routine that does
; not exist, which is the reasoning that leaves MSCDEX 1501h chained.
;
; THE WHOLE BODY RUNS INSIDE PUSHAD.  It is called from vdm_i2F's
; chain, where the kernel's own registers belong to the V86 loop, and
; the answer it has to give back is one bit; carrying that bit out in
; a byte rather than in the flags is what makes the pushad possible.
;=====================================================================
CC_SIG          EQU     0BABEh                  ; BAMBI_SIGNATURE
CC_VER          EQU     0400h                   ; and its version, BCD

cache_int2F:
        pushad
        mov     byte ptr [c2f_ours],0
        mov     ax,word ptr [vdm_ctx+V86C_EAX]
        cmp     ax,4A10h
        jne     c2f_out
        cmp     byte ptr [cache_on],0
        je      c2f_out                         ; no cache, no interface -
                                                ; and then a real SMARTDRV
                                                ; may load, which is right
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        cmp     ebx,0
        je      c2f_stats
        cmp     ebx,1
        je      c2f_commit
        cmp     ebx,2
        je      c2f_reinit
        cmp     ebx,3
        je      c2f_drive
        cmp     ebx,4
        je      c2f_info
        cmp     ebx,6
        je      c2f_nocache
c2f_out:
        popad
        cmp     byte ptr [c2f_ours],0
        je      c2f_chain
        clc
        ret
c2f_chain:
        stc
        ret

;--- BX=0: AX=BABEh, DX:BX hits, DI:SI misses, CX dirty, BP version --
c2f_stats:
        mov     eax,[cache_hitl]
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     eax,[cache_hith]
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        mov     eax,[cache_missl]
        mov     word ptr [vdm_ctx+V86C_ESI],ax
        mov     eax,[cache_missh]
        mov     word ptr [vdm_ctx+V86C_EDI],ax
        mov     eax,[cache_dirty]
        mov     word ptr [vdm_ctx+V86C_ECX],ax
        mov     word ptr [vdm_ctx+V86C_EBP],CC_VER
        jmp     c2f_sig

;--- BX=1: everything owed, now --------------------------------------
c2f_commit:
        call    dos_commit
        jmp     c2f_sig

;--- BX=2: commit and then forget ------------------------------------
; The resize half of this call is deliberately not honoured, exactly
; as it is not in SMARTDRV 4.0 itself - its source says so, and says
; why.  Here the reason is different and simpler: the size of this
; cache is the arena's business and CACHE='s, and a program re-sizing
; it behind the user's back is undoing a CONFIG.SYS line.
c2f_reinit:
        call    dos_commit
        call    cache_dropall
        jmp     c2f_sig

;--- BX=4: CX element size, AX blocks, BX queue, DX blocks under Win --
c2f_info:
        mov     word ptr [vdm_ctx+V86C_ECX],CC_BLKSIZE
        mov     eax,[cache_nblk]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        mov     byte ptr [c2f_ours],1
        jmp     c2f_out

;--- BX=3 and BX=6: one drive's caching ------------------------------
; BP is the unit.  SMARTDRV counted block-device units; the only
; numbering a program on THIS machine can obtain is the drive letter,
; and drv_tbl turns one into the physical drive the cache keys on.  DL
; comes back in SMARTDRV's own spelling - bit 7 set means read caching
; is off, bit 6 set means write caching is off, FFh means there is no
; such drive - so a program that knows the interface reads the answer
; correctly without knowing anything about PM-DOS.
c2f_nocache:
        mov     dl,2                            ; "disable read caching",
        jmp     short c2f_drv2                  ; which disables both
c2f_drive:
        mov     dl,byte ptr [vdm_ctx+V86C_EDX]
c2f_drv2:
        movzx   ebp,word ptr [vdm_ctx+V86C_EBP]
        cmp     ebp,MAX_DRIVES
        jae     c2f_nodrv
        mov     eax,ebp
        imul    eax,DRV_LEN
        add     eax,OFFSET drv_tbl
        test    byte ptr [eax+DRV_FLAGS],DRVF_PRESENT
        jz      c2f_nodrv
        test    byte ptr [eax+DRV_FLAGS],DRVF_CDROM
        jz      c2f_notcd
        movzx   eax,byte ptr [eax+DRV_BIOS]     ; an ATAPI unit number, which
        add     eax,CC_CDBASE                   ; is not a drive number
        jmp     short c2f_haveid
c2f_notcd:
        movzx   eax,byte ptr [eax+DRV_BIOS]
c2f_haveid:
        mov     [c2f_id],al
        push    edx
        call    cache_unitof                    ; -> EBX = the cache_drvst row
        pop     edx
        jc      c2f_nodrv

        cmp     dl,1
        je      c2f_rdon
        cmp     dl,2
        je      c2f_rdoff
        cmp     dl,3
        je      c2f_wron
        cmp     dl,4
        je      c2f_wroff
        jmp     short c2f_drvst                 ; 0, or anything else: report
c2f_rdon:
        or      byte ptr [cache_drvst+ebx],CCM_READ
        jmp     short c2f_drvst
c2f_rdoff:
        call    dos_commit                      ; what is owed goes out before
        mov     byte ptr [cache_drvst+ebx],0    ; the drive stops being cached
        jmp     short c2f_drvst
c2f_wron:
        or      byte ptr [cache_drvst+ebx],CCM_WRITE
        jmp     short c2f_drvst
c2f_wroff:
        call    dos_commit
        and     byte ptr [cache_drvst+ebx],NOT CCM_WRITE

c2f_drvst:
        ; THE ANSWER IS THE EFFECTIVE ONE AND NOT THE PERMISSION.
        ; cache_drvst is what the API has been TOLD about this unit;
        ; cache_mode is what actually happens to it, which also
        ; depends on what kind of drive it is and on whether lazy
        ; writing is on at all.  A program asking whether A: is
        ; write-cached wants to hear no, and hearing yes because
        ; nobody has explicitly disabled it would be a lie it acts on.
        mov     al,[c2f_id]
        call    cache_mode
        jc      c2f_nobits
        mov     ah,al
        jmp     short c2f_bits
c2f_nobits:
        xor     ah,ah
c2f_bits:
        xor     edx,edx
        test    ah,CCM_READ
        jnz     c2f_rdis
        or      dl,80h
c2f_rdis:
        test    ah,CCM_WRITE
        jnz     c2f_wdis
        or      dl,40h
c2f_wdis:
        mov     byte ptr [vdm_ctx+V86C_EDX],dl
        jmp     short c2f_sig
c2f_nodrv:
        mov     byte ptr [vdm_ctx+V86C_EDX],0FFh
c2f_sig:
        mov     word ptr [vdm_ctx+V86C_EAX],CC_SIG
        mov     byte ptr [c2f_ours],1
        jmp     c2f_out

;=====================================================================
; Data
;=====================================================================
cache_on    db  0                       ; is there a cache at all
cache_lazy  db  1                       ; write caching on fixed disks
cache_busy  db  0                       ; a nested call goes straight to
                                        ; the drive - see the header
cache_error db  0                       ; a write-back failed and the
                                        ; block is still dirty
c2f_id      db  0                       ; the drive 4A10h/3 is asking about
c2f_ours    db  0                       ; the one bit cache_int2F carries
                                        ; out through its pushad
            db  0,0                     ; six bytes above, two here: the
                                        ; dwords below start aligned
cache_maxb  dd  0                       ; the ceiling, in bytes
cache_minb  dd  0                       ; the floor, which reclaiming will
                                        ; not cross
cache_curb  dd  0                       ; payload bytes held right now
cache_arena dd  0                       ; how big the arena was at boot
cache_nblk  dd  0                       ; blocks in the cache
cache_used  dd  0                       ; ...of which this many hold a key,
                                        ; which is the cheap test for "is
                                        ; it worth asking to grow"
cache_dirty dd  0                       ; ...of which this many owe a write

cache_chunks dd 0                       ; the arena blocks, newest first
cache_mru   dd  0
cache_lru   dd  0

cache_hitl  dd  0                       ; the SMARTDRV statistics, 64-bit
cache_hith  dd  0                       ; because they come back as register
cache_missl dd  0                       ; pairs and because they overflow
cache_missh dd  0

; Per-unit permission, indexed by cache_unitof.  Opened by cache_init
; and narrowed by INT 2Fh AX=4A10h BX=3.
cache_drvst db  CC_UNITS dup(0)

; cache_io's transfer, and the span of it being worked on.  Statics
; rather than registers for the same reason bio_ and dio_ are: the
; loop calls out to the drive, which owns every register it likes.
cch_drv     db  0
cch_wr      db  0
cch_mode    db  0
            db  0
cch_lba     dd  0
cch_cnt     dd  0
cch_buf     dd  0
cch_p       dd  0                       ; the sector the loop is at
cch_left    dd  0
cch_ptr     dd  0
cch_key     dd  0                       ; this span's block
cch_off     dd  0                       ; ...and where in it
cch_n       dd  0
cch_mask    dd  0                       ; the sector bits it covers
cch_blk     dd  0                       ; the descriptor

cfl_i       dd  0                       ; cache_fill's walk
cfl_end     dd  0
cfl_run     dd  0
cfl_len     dd  0

cfr_drv     db  0                       ; the sweeps' arguments
            db  0,0,0
cfr_lo      dd  0
cfr_hi      dd  0

ccr_want    dd  0                       ; cache_reclaim's
ccr_got     dd  0
cwm_low     dd  0                       ; cache_watermark's target

ccs_max     dd  0                       ; cache_setsize's arguments, which
ccs_min     dd  0                       ; outlive the pushad that saved the
ccs_lazy    db  0                       ; registers they arrived in
ccs_off     db  0
            db  0,0

; The bucket heads, and the largest single piece of state in the file
; at two kilobytes.  Zero in the image AND zeroed again by cache_init,
; which is not superstition: cache_setsize can run the whole thing
; down and build it again, and the second time there is no image.
cache_hash  dd  CC_HASHSIZE dup(0)
