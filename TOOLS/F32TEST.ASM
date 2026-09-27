;=====================================================================
; F32TEST.ASM - F32TEST.COM, the rest of the FAT32 API family.
;
; 7303h - extended free space - went in with v0.24 because DIR needed
; numbers wider than 36h can hold.  The other three sat on the roadmap
; as "the rest of the family":
;
;   7302h  get the extended drive parameter block
;   7304h  set DPB to use for formatting
;   7305h  extended absolute disk read and write
;
; They are one family because they are all the FAT32 widening of
; something that used to be sixteen bits: a DPB whose cluster count
; overflowed, and an INT 25h/26h whose sector number did.
;
; AS OF v0.60 THEY ARE DOS 7.1'S CALLS AND NOT PM-DOS'S VERSIONS OF
; THEM.  7302h and 7304h take the drive in DL, 7302h fills in DOS
; 7.1's own 61-byte block, 7304h takes the FAT32 API's seven-dword
; request, and 7305h fails with the extended errors 13h-1Fh rather than
; the two classic codes it used to use for everything.  Every check
; below is written against what a program built for Windows 95 expects,
; and checks the block's facts against 7303h's, which is a different
; call reading different variables.
;
; Thirteen checks:
;
;   1  7302h answers for DL = 0, the default drive
;   2  ...with the sector size 7303h reports
;   3  ...and the cluster size 7303h reports
;   4  ...and a cluster count that agrees with 7303h
;   5  ...and its length word, and a chain pointer that says "none"
;   6  a short buffer is refused with 18h and left untouched
;   7  DL = the drive by number names the same drive
;   8  a pointer into the guard page is refused, not faulted
;   9  7304h reports the active-FAT flags it is asked for
;  10  ...and refuses to set them to something the volume is not
;  11  7305h reads sector 0 and finds a BPB of the reported size
;  12  ...and refuses a packet without CX = FFFFh
;  13  ...and a sector past the end is error 1Bh, not 08h
;
; Check 8 is the kernel bug CHKDSK found: a small number where a
; pointer goes used to become a flat address in the slot's first page,
; which is not present, and the KERNEL took the page fault.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_F3     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_F3, DS:_F3, ES:_F3, SS:_F3
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET f3_entry>,<0>,4096

; Up here rather than beside the buffer, because wasm cannot size a
; forward-referenced EQU used as an immediate and check 6 uses both
; of these three hundred lines before the data section.
SMALL           EQU     12              ; a deliberately short buffer
GUARD           EQU     16              ; ...and the bytes behind it

f3_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        ; the current drive, named the way 7303h wants it and numbered
        ; the way 7302h, 7304h and 7305h want it
        mov     ah,19h
        int     21h
        mov     [curdrv],al
        add     al,'A'
        mov     [drvpath],al

;=====================================================================
; 1 - 7302h answers for the default drive.
;=====================================================================
        mov     edx,OFFSET msg_t1
        call    puts
        xor     edx,edx                 ; DL = 0: the default drive
        call    getdpb
        jc      t1_bad
        call    good
        jmp     short t2
t1_bad:
        call    bad
        mov     edx,OFFSET msg_no02
        call    puts
        jmp     wrapup
t2:

;--- 7303h, to have something independent to check against -----------
        mov     edx,OFFSET drvpath
        mov     edi,OFFSET frec
        mov     ecx,F73_SIZE
        mov     ax,7303h
        int     21h
        mov     byte ptr [have73],1
        jnc     t2_have
        mov     byte ptr [have73],0
t2_have:

;=====================================================================
; 2 - the sector size, against 7303h's.  It was "512" until v0.60;
;     a volume can now have 1024-, 2048- or 4096-byte logical sectors
;     and both calls must say the same one.
;=====================================================================
        mov     edx,OFFSET msg_t2
        call    puts
        cmp     byte ptr [have73],0
        je      t2_bad
        movzx   eax,word ptr [dpbbuf+F72_DPB+EDPB_BPS]
        cmp     eax,dword ptr [frec+F73_BPS]
        jne     t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad
t3:

;=====================================================================
; 3 - the cluster size, against 7303h's.
;=====================================================================
        mov     edx,OFFSET msg_t3
        call    puts
        cmp     byte ptr [have73],0
        je      t3_bad
        movzx   eax,byte ptr [dpbbuf+F72_DPB+EDPB_SPCM1]
        inc     eax
        cmp     eax,dword ptr [frec+F73_SPC]
        jne     t3_bad
        call    good
        jmp     short t4
t3_bad:
        call    bad
t4:

;=====================================================================
; 4 - and the cluster count.  The DPB carries the HIGHEST cluster
;     NUMBER and 7303h carries HOW MANY there are, and cluster
;     numbering starts at two - so the two differ by one, and a test
;     that did not know that would be testing its own arithmetic.
;=====================================================================
        mov     edx,OFFSET msg_t4
        call    puts
        cmp     byte ptr [have73],0
        je      t4_bad
        mov     eax,dword ptr [dpbbuf+F72_DPB+EDPB_MAXCLUS]
        dec     eax
        cmp     eax,dword ptr [frec+F73_TOTCL]
        jne     t4_bad
        call    good
        jmp     short t5
t4_bad:
        call    bad
t5:

;=====================================================================
; 5 - the length word DOS puts in front of the block, and the chain
;     pointer that would have to lie: FFFF:FFFF, the sentinel fn52
;     uses, which a walker tests before it dereferences anything.
;=====================================================================
        mov     edx,OFFSET msg_t5
        call    puts
        cmp     word ptr [dpbbuf+F72_LEN],EDPB_LEN
        jne     t5_bad
        cmp     dword ptr [dpbbuf+F72_DPB+EDPB_NEXT],0FFFFFFFFh
        jne     t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad
t6:

;=====================================================================
; 6 - a buffer shorter than the block is REFUSED - 18h, bad request
;     length, which is DOS 7.1's answer - and not a byte of it is
;     written.  A half-filled block would read as zeroes for geometry.
;=====================================================================
        mov     edx,OFFSET msg_t6
        call    puts
        mov     edi,OFFSET smallbuf
        mov     ecx,SMALL + GUARD
        mov     al,0CCh
        push    edi
        rep     stosb
        pop     edi
        xor     edx,edx
        mov     ecx,SMALL
        mov     ax,7302h
        int     21h
        jnc     t6_bad
        cmp     ax,ERR_BADLEN
        jne     t6_bad
        mov     ecx,SMALL + GUARD       ; untouched, all of it
t6_guard:
        cmp     byte ptr [smallbuf+ecx-1],0CCh
        jne     t6_bad
        loop    t6_guard
        call    good
        jmp     short t7
t6_bad:
        call    bad
t7:

;=====================================================================
; 7 - DL names a drive by NUMBER, 1 = A:.  The same drive asked for
;     that way comes back with its own number in the block.
;=====================================================================
        mov     edx,OFFSET msg_t7
        call    puts
        movzx   edx,byte ptr [curdrv]
        inc     edx
        call    getdpb
        jc      t7_bad
        mov     al,[dpbbuf+F72_DPB+EDPB_DRIVE]
        cmp     al,[curdrv]
        jne     t7_bad
        call    good
        jmp     short t8
t7_bad:
        call    bad
t8:

;=====================================================================
; 8 - a pointer into the guard page.  EDI = 1 is a small number where
;     a buffer goes, and it has to come back "access denied" with this
;     program still running to say so.  Before v0.60 the kernel
;     translated it into the slot's first page, which is not present,
;     and faulted in the middle of the call.
;=====================================================================
        mov     edx,OFFSET msg_t8
        call    puts
        xor     edx,edx
        mov     edi,1
        mov     ecx,F72_SIZE
        mov     ax,7302h
        int     21h
        jnc     t8_bad
        cmp     ax,ERR_ACCESS
        jne     t8_bad
        call    good
        jmp     short t9
t8_bad:
        call    bad
t9:

;=====================================================================
; 9 - 7304h, function 4: get the active FAT and mirroring flags.  A
;     VAL1 of FFFFFFFFh asks and changes nothing; the answer is in
;     VAL2, and on FAT12 and FAT16 it is zero - no flags at all.
;=====================================================================
        mov     edx,OFFSET msg_t9
        call    puts
        call    sdp_clear
        mov     dword ptr [sdp+SDP_FUNC],SDPF_ACTFAT
        mov     dword ptr [sdp+SDP_VAL1],0FFFFFFFFh
        mov     dword ptr [sdp+SDP_VAL2],12345678h
        call    setdpb
        jc      t9_bad
        cmp     dword ptr [sdp+SDP_VAL2],12345678h
        je      t9_bad                  ; nothing was reported
        mov     eax,dword ptr [sdp+SDP_VAL2]
        mov     [actflags],eax
        call    good
        jmp     short t10
t9_bad:
        call    bad
t10:

;=====================================================================
; 10 - and a SET to something the volume is not is refused.  PM-DOS
;      reads a volume's shape from its BPB and cannot be told another;
;      a "set" that said yes and changed nothing would be worse.
;=====================================================================
        mov     edx,OFFSET msg_t10
        call    puts
        call    sdp_clear
        mov     dword ptr [sdp+SDP_FUNC],SDPF_ACTFAT
        mov     eax,[actflags]
        xor     eax,81h                 ; mirroring flipped, and another FAT
        mov     dword ptr [sdp+SDP_VAL1],eax
        call    setdpb
        jnc     t10_bad
        cmp     ax,ERR_BADDATA
        jne     t10_bad
        call    good
        jmp     short t11
t10_bad:
        call    bad
t11:

;=====================================================================
; 11 - 7305h reads.  Sector 0 of a mounted volume is its boot record,
;      which is recognisable without knowing anything else about the
;      disk: it ends in AA55h, and its sector size is the one 7302h
;      reported - so the buffer is sized for the largest there is.
;=====================================================================
        mov     edx,OFFSET msg_t11
        call    puts
        movzx   eax,byte ptr [curdrv]
        inc     eax                     ; 7305h speaks 1-based drives
        mov     [pkt_drv],eax

        mov     dword ptr [pkt+DSKIO_START],0
        mov     word ptr [pkt+DSKIO_COUNT],1
        mov     dword ptr [pkt+DSKIO_BUF],OFFSET secbuf
        mov     ebx,OFFSET pkt
        mov     edx,[pkt_drv]
        mov     ecx,0FFFFh
        xor     esi,esi                 ; read
        mov     ax,7305h
        int     21h
        jc      t11_bad
        cmp     word ptr [secbuf+510],0AA55h
        jne     t11_bad
        mov     ax,word ptr [secbuf+BPB_BPS]
        cmp     ax,word ptr [dpbbuf+F72_DPB+EDPB_BPS]
        jne     t11_bad
        call    good
        jmp     short t12
t11_bad:
        call    bad
t12:

;=====================================================================
; 12 - and CX must be FFFFh.  It is not a formality: INT 25h's own
;      escape for a volume too big to address the old way was ALSO a
;      count of FFFFh, so the value is what tells this call from a
;      program that has confused two APIs.
;=====================================================================
        mov     edx,OFFSET msg_t12
        call    puts
        mov     dword ptr [pkt+DSKIO_START],0
        mov     word ptr [pkt+DSKIO_COUNT],1
        mov     dword ptr [pkt+DSKIO_BUF],OFFSET secbuf
        mov     ebx,OFFSET pkt
        mov     edx,[pkt_drv]
        mov     ecx,1                   ; not FFFFh
        xor     esi,esi
        mov     ax,7305h
        int     21h
        jnc     t12_bad
        call    good
        jmp     short t13
t12_bad:
        call    bad
t13:

;=====================================================================
; 13 - a sector past the end of the volume is SECTOR NOT FOUND, which
;      as an extended error is 1Bh.  It came back 08h - "insufficient
;      memory" in that numbering - and every other failure came back
;      0Bh, so LABEL on a write-protected disk said "General failure".
;=====================================================================
        mov     edx,OFFSET msg_t13
        call    puts
        mov     dword ptr [pkt+DSKIO_START],0FFFFFFF0h
        mov     word ptr [pkt+DSKIO_COUNT],1
        mov     dword ptr [pkt+DSKIO_BUF],OFFSET secbuf
        mov     ebx,OFFSET pkt
        mov     edx,[pkt_drv]
        mov     ecx,0FFFFh
        xor     esi,esi
        mov     ax,7305h
        int     21h
        jnc     t13_bad
        cmp     ax,1Bh
        jne     t13_bad
        call    good
        jmp     short wrapup
t13_bad:
        call    bad

;=====================================================================
wrapup:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     wr_fail
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
wr_fail:
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; getdpb - DL = the drive (0 = default) -> the whole block in dpbbuf.
;          CF as the call gave it.
;---------------------------------------------------------------------
getdpb:
        push    ecx
        push    edi
        mov     edi,OFFSET dpbbuf
        mov     ecx,F72_SIZE
        mov     ax,7302h
        int     21h
        pop     edi
        pop     ecx
        ret

;---------------------------------------------------------------------
; sdp_clear / setdpb - a 7304h request for the current drive, built in
;                      sdp.  CF and AX as the call gave them.
;---------------------------------------------------------------------
sdp_clear:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET sdp
        xor     eax,eax
        mov     ecx,SDP_LEN / 4
        rep     stosd
        mov     dword ptr [sdp+SDP_SIZE],SDP_LEN
        pop     edi
        pop     ecx
        pop     eax
        ret

setdpb:
        push    ecx
        push    edx
        push    edi
        movzx   edx,byte ptr [curdrv]
        inc     edx
        mov     edi,OFFSET sdp
        mov     ecx,SDP_LEN
        mov     ax,7304h
        int     21h
        pop     edi
        pop     edx
        pop     ecx
        ret

;=====================================================================
good:
        push    edx
        mov     dl,'.'
        call    tally
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
bad:
        push    edx
        mov     dl,'X'
        call    tally
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     edx
        ret
tally:
        push    ebx
        mov     ebx,[resptr]
        mov     [ebx],dl
        inc     ebx
        mov     byte ptr [ebx],'$'
        mov     [resptr],ebx
        pop     ebx
        ret

puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

;=====================================================================

msg_hi      db  'F32TEST - 7302h, 7304h and 7305h, as DOS 7.1 has them',13,10,13,10,'$'
msg_t1      db  ' 1 7302h answers for the default drive    ','$'
msg_t2      db  ' 2 ...sector size agrees with 7303h       ','$'
msg_t3      db  ' 3 ...cluster size agrees with 7303h      ','$'
msg_t4      db  ' 4 ...cluster count agrees with 7303h     ','$'
msg_t5      db  ' 5 ...length word, and no DPB chain       ','$'
msg_t6      db  ' 6 a short buffer: 18h, and untouched     ','$'
msg_t7      db  ' 7 DL = the drive number names it         ','$'
msg_t8      db  ' 8 a guard-page pointer is refused        ','$'
msg_t9      db  ' 9 7304h reports the active-FAT flags     ','$'
msg_t10     db  '10 ...and refuses to change them          ','$'
msg_t11     db  '11 7305h reads sector 0, and it is a BPB  ','$'
msg_t12     db  '12 ...and insists on CX = FFFFh           ','$'
msg_t13     db  '13 ...and past the end is error 1Bh       ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_no02    db  13,10,'  7302h is not answered - nothing else here can run.',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'F32TEST: all checks passed',13,10,'$'
msg_fail    db  'F32TEST: FAILURES',13,10,'$'

drvpath     db  'X:\',0
have73      db  0
fails       db  0
curdrv      db  0
            db  0
pkt_drv     dd  0
actflags    dd  0
resptr      dd  OFFSET results
results     db  16 dup('$')
dpbbuf      db  F72_SIZE + 16 dup(0)
smallbuf    db  SMALL + GUARD dup(0)
frec        db  F73_SIZE dup(0)
pkt         db  DSKIO_LEN dup(0)
            db  0,0
sdp         db  SDP_LEN dup(0)
secbuf      db  4096 dup(0)

_F3     ENDS
        END
