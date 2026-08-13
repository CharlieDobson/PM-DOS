;=====================================================================
; I13TEST.ASM - I13TEST.COM, a 16-bit DOS program that goes at the
;               disks the way a disk utility goes at them: INT 13h,
;               underneath the filesystem entirely.
;
; Two questions, and the second is the one that matters.
;
; Does the disk BIOS work?  Geometry, a CHS read, the EDD extensions,
; an extended read of the same sector, and a CHS triple that does not
; exist - which has to be refused rather than quietly wrapped onto the
; next track, because that is how a program finds the end of a disk.
;
; And does it AGREE WITH THE FILESYSTEM?  That is what the last phase
; is for, and it is the reason any of this was written.  The kernel
; caches FAT sectors, whole FAT12 FATs, directory sectors and the last
; cluster it read; a program reaching past all of that to the disk can
; see a stale version of what it wrote, or write something the kernel
; will later overwrite from a cache of its own.  So:
;
;   DOS writes a file and closes it.  The program then finds that
;   file's directory entry BY READING SECTORS - if the kernel had
;   still been holding the directory, it would not be there yet.
;
;   The file is read through DOS, which caches its cluster.  The
;   program overwrites that cluster with INT 13h.  DOS reads the file
;   again: it has to see the new contents, and it can only do that if
;   the raw write dropped the cache that described them.
;
; Nothing is written anywhere except into a file this program created
; itself, one cluster of it, whose address it worked out from the BPB.
;
; The volume is whichever one is current, as long as it is A:, B: or
; C: - a floppy is the whole medium and C: is the first partition of
; the first hard disk, which is enough to find the BPB from.  On any
; other drive the last phase says so and stops.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_I13    SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_I13, DS:_I13, ES:_I13, SS:_I13
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; Which drive, and is it one we can work out a layout for?
;=====================================================================
        mov     ah,19h                          ; the current DOS drive
        int     21h
        mov     [dos_drive],al
        cmp     al,2
        ja      i13_other
        mov     bl,al                           ; A: -> 00h, B: -> 01h
        cmp     al,2
        jb      i13_fd
        mov     bl,80h                          ; C: -> the first hard disk
i13_fd:
        mov     [bios_drive],bl
        jmp     short i13_havedrv
i13_other:
        mov     byte ptr [bios_drive],80h       ; still test the BIOS calls,
        mov     byte ptr [no_layout],1          ; just not the last phase
i13_havedrv:
        mov     dx,OFFSET msg_drv
        call    puts
        movzx   eax,byte ptr [bios_drive]
        call    hex8
        call    crlf

;=====================================================================
; Phase 1 - the geometry, and it has to be usable arithmetic
;=====================================================================
        mov     dx,OFFSET msg_p1
        call    puts

        mov     ah,08h
        mov     dl,[bios_drive]
        int     13h
        jc      geo_bad

        movzx   eax,cl
        and     eax,3Fh                         ; CL 5:0 = sectors per track
        mov     [spt],ax
        or      ax,ax
        jz      geo_bad
        movzx   eax,dh
        inc     eax                             ; DH = LAST head
        mov     [heads],ax
        movzx   eax,ch                          ; CH + CL 7:6 = last cylinder
        movzx   ebx,cl
        shl     ebx,2
        and     ebx,300h
        or      eax,ebx
        inc     eax
        mov     [cyls],ax

        mov     dx,OFFSET msg_geo
        call    puts
        movzx   eax,word ptr [cyls]
        call    dec16
        mov     dx,OFFSET msg_x
        call    puts
        movzx   eax,word ptr [heads]
        call    dec16
        mov     dx,OFFSET msg_x
        call    puts
        movzx   eax,word ptr [spt]
        call    dec16
        call    crlf
        jmp     short phase2
geo_bad:
        mov     dx,OFFSET msg_geobad
        call    puts
        jmp     done

;=====================================================================
; Phase 2 - a CHS read of the first sector, and what is in it
;=====================================================================
phase2:
        mov     dx,OFFSET msg_p2
        call    puts

        xor     eax,eax
        mov     di,OFFSET sect
        call    rd_chs
        jc      p2_bad
        cmp     word ptr [sect+510],0AA55h
        jne     p2_nosig
        mov     dx,OFFSET msg_sig
        call    ok
        jmp     short phase3
p2_nosig:
        ; Not a failure by itself - a floppy's boot sector need not be
        ; signed - so it is reported and the sector is still used.
        mov     dx,OFFSET msg_nosig
        call    puts
        jmp     short phase3
p2_bad:
        mov     dx,OFFSET msg_rdfail
        call    fail
        jmp     done

;=====================================================================
; Phase 3 - the extensions, and the two addressing modes cross-checked
;
; This is the check with teeth in it: the same sector, reached two
; entirely different ways - a packed CHS triple and a 64-bit LBA in a
; packet - has to come back byte for byte identical.  Anything wrong
; with the translation shows up here as a difference and nowhere else
; as anything at all.
;=====================================================================
phase3:
        mov     dx,OFFSET msg_p3
        call    puts

        mov     ah,41h
        mov     bx,55AAh
        mov     dl,[bios_drive]
        int     13h
        jc      p3_noedd
        cmp     bx,0AA55h
        jne     p3_noedd
        test    cl,1                            ; extended disk access?
        jz      p3_noedd
        mov     dx,OFFSET msg_edd
        call    ok

        xor     eax,eax
        mov     di,OFFSET sect2
        call    rd_lba
        jc      p3_extbad

        mov     si,OFFSET sect
        mov     di,OFFSET sect2
        mov     cx,512
        cld
        repe    cmpsb
        jne     p3_differ
        mov     dx,OFFSET msg_same
        call    ok
        jmp     short phase4
p3_differ:
        mov     dx,OFFSET msg_differ
        call    fail
        jmp     short phase4
p3_extbad:
        mov     dx,OFFSET msg_extbad
        call    fail
        jmp     short phase4
p3_noedd:
        mov     dx,OFFSET msg_noedd
        call    puts

;=====================================================================
; Phase 4 - a CHS triple that does not exist
;
; Sector 0 is not a sector: they are numbered from one, and every disk
; BIOS ever built returns an error for it.  A translation that just
; did the arithmetic would read the sector BEFORE the track instead,
; which is a real sector belonging to somebody else.
;=====================================================================
phase4:
        mov     dx,OFFSET msg_p4
        call    puts

        mov     ax,0201h
        mov     cx,0000h                        ; cylinder 0, sector 0
        mov     dh,0
        mov     dl,[bios_drive]
        mov     bx,OFFSET sect2
        int     13h
        jnc     p4_took
        mov     dx,OFFSET msg_ref0
        call    ok
        jmp     short p4_head
p4_took:
        mov     dx,OFFSET msg_took0
        call    fail

p4_head:
        ; and a head the drive has not got
        mov     ax,0201h
        mov     cx,0001h                        ; cylinder 0, sector 1
        mov     dh,0FEh                         ; head 254
        mov     dl,[bios_drive]
        mov     bx,OFFSET sect2
        int     13h
        jnc     p4_tookh
        mov     dx,OFFSET msg_refh
        call    ok
        jmp     short phase5
p4_tookh:
        mov     dx,OFFSET msg_tookh
        call    fail

;=====================================================================
; Phase 5 - the filesystem and the sectors underneath it, made to
;           agree.  See the header.
;=====================================================================
phase5:
        mov     dx,OFFSET msg_p5
        call    puts
        cmp     byte ptr [no_layout],0
        je      p5_layout
        mov     dx,OFFSET msg_nolayout
        call    puts
        jmp     done

;--- where does the volume start? ------------------------------------
p5_layout:
        mov     dword ptr [partlba],0
        cmp     byte ptr [bios_drive],80h
        jb      p5_vbr                          ; a floppy IS the volume

        ; partition table entry 0, out of the sector already read
        mov     eax,dword ptr [sect+1BEh+8]
        mov     [partlba],eax
        or      eax,eax
        jz      p5_nopart

p5_vbr:
        mov     eax,[partlba]
        mov     di,OFFSET sect
        call    rd_lba
        jc      p5_ioerr

        cmp     word ptr [sect+0Bh],512         ; bytes per sector
        jne     p5_odd
        movzx   eax,byte ptr [sect+0Dh]         ; sectors per cluster
        mov     [spc],ax
        or      ax,ax
        jz      p5_odd
        movzx   eax,word ptr [sect+11h]         ; root entries
        mov     [rootent],ax
        or      ax,ax
        jz      p5_fat32                        ; a FAT32 root is a CHAIN,
                                                ; not an area - out of scope
                                                ; for a program this size

        ; root LBA = partition + reserved + FATs
        movzx   eax,word ptr [sect+0Eh]         ; reserved sectors
        movzx   ebx,byte ptr [sect+10h]         ; number of FATs
        movzx   ecx,word ptr [sect+16h]         ; sectors per FAT
        or      ecx,ecx
        jz      p5_fat32
        imul    ebx,ecx
        add     eax,ebx
        add     eax,[partlba]
        mov     [rootlba],eax

        ; and the data area starts after the root directory
        movzx   ebx,word ptr [rootent]
        add     ebx,15
        shr     ebx,4                           ; entries -> sectors, up
        mov     [rootsecs],bx
        add     eax,ebx
        mov     [datalba],eax

;--- DOS writes a file, and closes it --------------------------------
        mov     dx,OFFSET fname
        mov     cx,0
        mov     ah,3Ch
        int     21h
        jc      p5_nofile
        mov     [fhandle],ax

        mov     bx,ax
        mov     dx,OFFSET pattern_a
        mov     cx,512
        mov     ah,40h
        int     21h
        jc      p5_wrfail
        cmp     ax,512
        jne     p5_wrfail

        mov     bx,[fhandle]
        mov     ah,3Eh
        int     21h

;--- and the program finds it by reading the directory ---------------
        call    find_entry
        jc      p5_notfound
        mov     dx,OFFSET msg_seen
        call    ok

        ; the entry's first cluster -> the LBA of its first sector.
        ; SI is already an offset into dirbuf, not an index within it.
        movzx   eax,word ptr [si+26]
        cmp     eax,2
        jb      p5_noclus
        sub     eax,2
        movzx   ebx,word ptr [spc]
        imul    eax,ebx
        add     eax,[datalba]
        mov     [filelba],eax

;--- DOS reads it, which puts the cluster in the kernel's cache ------
        call    read_file
        jc      p5_rdfail
        cmp     byte ptr [fbuf],'A'
        jne     p5_rdfail

;--- the program overwrites that cluster underneath DOS --------------
        mov     eax,[filelba]
        mov     di,OFFSET pattern_b
        call    wr_chs
        jc      p5_rawfail

;--- and DOS has to see it ------------------------------------------
        call    read_file
        jc      p5_rdfail
        cmp     byte ptr [fbuf],'B'
        jne     p5_stale
        mov     dx,OFFSET msg_fresh
        call    ok
        jmp     short p5_tidy
p5_stale:
        mov     dx,OFFSET msg_stale
        call    fail

p5_tidy:
        mov     dx,OFFSET fname
        mov     ah,41h
        int     21h
        jmp     short done

;--- the ways phase 5 can decline rather than fail -------------------
p5_odd:
        mov     dx,OFFSET msg_oddbpb
        call    puts
        jmp     short done
p5_fat32:
        mov     dx,OFFSET msg_isfat32
        call    puts
        jmp     short done
p5_nopart:
        mov     dx,OFFSET msg_nopart
        call    puts
        jmp     short done
p5_ioerr:
        mov     dx,OFFSET msg_vbrbad
        call    fail
        jmp     short done
p5_nofile:
        mov     dx,OFFSET msg_nofile
        call    fail
        jmp     short done
p5_wrfail:
        mov     bx,[fhandle]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET msg_wrfail
        call    fail
        jmp     short done
p5_notfound:
        mov     dx,OFFSET msg_notseen
        call    fail
        jmp     short p5_tidy
p5_noclus:
        mov     dx,OFFSET msg_noclus
        call    fail
        jmp     short p5_tidy
p5_rdfail:
        mov     dx,OFFSET msg_rdback
        call    fail
        jmp     short p5_tidy
p5_rawfail:
        mov     dx,OFFSET msg_rawfail
        call    fail
        jmp     short p5_tidy

done:
        mov     dx,OFFSET msg_done
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; find_entry - walk the root directory with INT 13h looking for the
;              file DOS has just written.  SI = its offset in dirbuf
;              on return, CF set if it is not there.
;
; Reading it off the medium is the whole point: the directory sector
; was written moments ago through the filesystem, and if the kernel
; were still holding it, this is where that would show.
;=====================================================================
find_entry:
        pushad
        mov     eax,[rootlba]
        movzx   ecx,word ptr [rootsecs]
fe_sector:
        push    ecx
        push    eax
        mov     di,OFFSET dirbuf
        call    rd_lba
        pop     eax
        pop     ecx
        jc      fe_no

        push    ecx
        push    eax
        mov     si,OFFSET dirbuf
        mov     cx,16                           ; entries in one sector
fe_entry:
        cmp     byte ptr [si],0                 ; end of the directory
        je      fe_nopop
        cmp     byte ptr [si],0E5h              ; a deleted one
        je      fe_nextentry
        push    cx
        push    si
        mov     di,OFFSET fname83
        mov     cx,11
        cld
        repe    cmpsb
        pop     si
        pop     cx
        je      fe_found
fe_nextentry:
        add     si,32
        loop    fe_entry
        pop     eax
        pop     ecx
        inc     eax
        dec     ecx
        jnz     fe_sector
fe_no:
        popad
        stc
        ret
fe_nopop:
        pop     eax
        pop     ecx
        jmp     short fe_no
fe_found:
        mov     [found_off],si
        pop     eax
        pop     ecx
        popad
        mov     si,[found_off]
        clc
        ret

;=====================================================================
; read_file - open the test file, read its first 512 bytes into fbuf
;             and close it again.  CF on any failure.
;
; Opened afresh each time on purpose: a handle held across the raw
; write would let a file position or a buffer inside DOS answer the
; second read, and the question is what the FILESYSTEM has, not what
; one open handle remembers.
;=====================================================================
read_file:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     dx,OFFSET fname
        mov     ax,3D00h
        int     21h
        jc      rf_out
        mov     bx,ax
        mov     dx,OFFSET fbuf
        mov     cx,512
        mov     ah,3Fh
        int     21h
        pushf
        mov     ah,3Eh
        int     21h
        popf
rf_out:
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; rd_chs / wr_chs - one sector at EAX, through the packed CHS form.
;                   DI = buffer.  CF and AH on failure.
;
;   sector   = lba MOD spt + 1
;   head     = (lba DIV spt) MOD heads
;   cylinder = (lba DIV spt) DIV heads
;
; The same arithmetic the kernel does in the other direction, which is
; why doing it here proves anything.
;=====================================================================
rd_chs:
        mov     byte ptr [chs_fn],02h
        jmp     short chs_go
wr_chs:
        mov     byte ptr [chs_fn],03h
chs_go:
        push    ebx
        push    ecx
        push    edx

        xor     edx,edx
        movzx   ebx,word ptr [spt]
        div     ebx                             ; EAX = track, EDX = sector-1
        inc     edx
        mov     [chs_sec],dl

        xor     edx,edx
        movzx   ebx,word ptr [heads]
        div     ebx                             ; EAX = cylinder, EDX = head
        mov     [chs_head],dl
        cmp     eax,1024
        jae     chs_range
        mov     [chs_cyl],ax

        mov     cl,byte ptr [chs_cyl+1]
        shl     cl,6
        or      cl,[chs_sec]
        mov     ch,byte ptr [chs_cyl]
        mov     dh,[chs_head]
        mov     dl,[bios_drive]
        mov     ah,[chs_fn]
        mov     al,1
        mov     bx,di
        int     13h

        pop     edx
        pop     ecx
        pop     ebx
        ret
chs_range:
        pop     edx
        pop     ecx
        pop     ebx
        mov     ah,0FFh
        stc
        ret

;=====================================================================
; rd_lba - one sector at EAX into DI, through a disk address packet.
;=====================================================================
rd_lba:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    si

        mov     word ptr [dap],0010h            ; size, and one reserved byte
        mov     word ptr [dap+2],1              ; sectors
        mov     word ptr [dap+4],di             ; buffer offset
        mov     word ptr [dap+6],ds             ; ...and segment
        mov     dword ptr [dap+8],eax           ; LBA, low half
        mov     dword ptr [dap+12],0            ; ...and high

        mov     si,OFFSET dap
        mov     ah,42h
        mov     dl,[bios_drive]
        int     13h

        pop     si
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; Output helpers
;=====================================================================
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        call    puts
        ret

fail:
        push    dx
        mov     dx,OFFSET msg_fail
        call    puts
        pop     dx
        call    puts
        ret

crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret

; EAX in hex, two digits
hex8:
        push    eax
        push    ecx
        push    edx
        mov     ecx,2
        rol     eax,24
h8_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        cmp     al,10
        jb      h8_dig
        add     al,7
h8_dig:
        add     al,'0'
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     eax
        loop    h8_lp
        pop     edx
        pop     ecx
        pop     eax
        ret

; EAX as unsigned decimal
dec16:
        pushad
        mov     ecx,0
        mov     ebx,10
d16_div:
        xor     edx,edx
        div     ebx
        push    edx
        inc     ecx
        or      eax,eax
        jnz     d16_div
d16_out:
        pop     eax
        add     al,'0'
        mov     dl,al
        mov     ah,02h
        int     21h
        loop    d16_out
        popad
        ret

;=====================================================================
msg_hi      db  13,10,'I13TEST - the disk BIOS a DOS box sees',13,10,'$'
msg_drv     db  '  BIOS drive: $'
msg_p1      db  '  AH=08h - the geometry:',13,10,'$'
msg_geo     db  '    $'
msg_x       db  ' x $'
msg_geobad  db  '    FAIL the drive has no geometry - stopping',13,10,'$'
msg_p2      db  '  AH=02h - the first sector, by CHS:',13,10,'$'
msg_sig     db  'read, and it ends AA55',13,10,'$'
msg_nosig   db  '    read, but it is not a signed sector',13,10,'$'
msg_rdfail  db  'the read failed',13,10,'$'
msg_p3      db  '  AH=41h/42h - the extensions:',13,10,'$'
msg_edd     db  'present, and they do extended access',13,10,'$'
msg_noedd   db  '    the extensions are not there',13,10,'$'
msg_same    db  'LBA 0 and CHS 0/0/1 are the same sector',13,10,'$'
msg_differ  db  'the two addressing modes disagree',13,10,'$'
msg_extbad  db  'the extended read failed',13,10,'$'
msg_p4      db  '  and a CHS triple that does not exist:',13,10,'$'
msg_ref0    db  'sector 0 refused',13,10,'$'
msg_took0   db  'sector 0 accepted - it read something else',13,10,'$'
msg_refh    db  'head 254 refused',13,10,'$'
msg_tookh   db  'head 254 accepted - it read something else',13,10,'$'
msg_p5      db  '  and does it agree with the filesystem?',13,10,'$'
msg_seen    db  'a file DOS just closed is on the medium',13,10,'$'
msg_notseen db  'the file is not in the directory on the disk',13,10,'$'
msg_fresh   db  'a raw write went through the cache, not round it',13,10,'$'
msg_stale   db  'DOS answered from a cache of what USED to be there',13,10,'$'
msg_nolayout db '    not A:, B: or C: - no layout to work from',13,10,'$'
msg_isfat32 db  '    FAT32: its root is a chain, not an area',13,10,'$'
msg_oddbpb  db  '    the BPB is not one this test can read',13,10,'$'
msg_nopart  db  '    partition 1 is empty',13,10,'$'
msg_vbrbad  db  'the boot record would not read',13,10,'$'
msg_nofile  db  'DOS would not create the test file',13,10,'$'
msg_wrfail  db  'DOS would not write the test file',13,10,'$'
msg_noclus  db  'the directory entry has no first cluster',13,10,'$'
msg_rdback  db  'DOS would not read the test file back',13,10,'$'
msg_rawfail db  'the raw write failed',13,10,'$'
msg_done    db  'I13TEST done',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'
msg_crlf    db  13,10,'$'

fname       db  'I13WR.TXT',0
fname83     db  'I13WR   TXT'

dos_drive   db  0
bios_drive  db  80h
no_layout   db  0
chs_fn      db  0
chs_sec     db  0
chs_head    db  0
chs_cyl     dw  0
spt         dw  0
heads       dw  0
cyls        dw  0
spc         dw  0
rootent     dw  0
rootsecs    dw  0
fhandle     dw  0
found_off   dw  0
partlba     dd  0
rootlba     dd  0
datalba     dd  0
filelba     dd  0

dap         db  16 dup(0)
pattern_a   db  512 dup('A')
pattern_b   db  512 dup('B')
sect        db  512 dup(0)
sect2       db  512 dup(0)
dirbuf      db  512 dup(0)
fbuf        db  512 dup(0)

_I13    ENDS
        END
