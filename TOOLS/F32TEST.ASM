;=====================================================================
; F32TEST.ASM - F32TEST.COM, the rest of the FAT32 API family.
;
; 7303h - extended free space - went in with v0.24 because DIR needed
; numbers wider than 36h can hold.  The other three sat on the roadmap
; as "the rest of the family":
;
;   7302h  get the extended drive parameter block
;   7304h  set it
;   7305h  extended absolute disk read and write
;
; They are one family because they are all the FAT32 widening of
; something that used to be sixteen bits: a DPB whose cluster count
; overflowed, and an INT 25h/26h whose sector number did.
;
; WHAT 7302h CAN HONESTLY ANSWER is the interesting part and worth
; stating.  PM-DOS has no drive parameter blocks - fn52's list of
; lists says FFFF:FFFF where the DPB chain would be, on purpose,
; because a pointer into a structure that does not exist is an
; invitation to walk it.  This call is the other direction: it COPIES
; a block into the caller's buffer, so there is nothing to follow and
; nothing that outlives the call, and every field in it is a fact
; about the mounted volume that the kernel already knows.  The tests
; below check those facts against the ones 7303h reports, which is a
; different call reading different variables.
;
; Ten checks:
;
;   1  7302h answers at all
;   2  ...with 512-byte sectors
;   3  ...and a cluster size that agrees with 7303h
;   4  ...and a cluster count that agrees with 7303h
;   5  ...and a chain pointer that says "no such thing"
;   6  a short buffer is refused rather than overrun
;   7  7304h accepts the DPB the kernel just gave out
;   8  ...and refuses one that has been altered
;   9  7305h reads sector 0 and finds a BPB
;  10  ...and refuses a packet without CX = FFFFh
;
; Checks 7 and 8 are what "set" means here.  PM-DOS takes its idea of
; a volume's shape from the BPB and cannot be told otherwise - FORMAT
; works from the BPB too, and F1h AL=2 exists to say "read it again" -
; so the only truthful answers are "that is already what I believe"
; and "I cannot believe that", and the call gives both.
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

        ; the current drive, named the way the 73h family wants it
        mov     ah,19h
        int     21h
        add     al,'A'
        mov     [drvpath],al

;=====================================================================
; 1 - 7302h answers.
;=====================================================================
        mov     edx,OFFSET msg_t1
        call    puts
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

;=====================================================================
; 2 - 512-byte sectors, which is the one number in the block that
;     this kernel has no other opinion about.
;=====================================================================
        mov     edx,OFFSET msg_t2
        call    puts
        cmp     word ptr [dpbbuf+F72_DPB+DPB_BPS],512
        jne     t2_bad
        call    good
        jmp     short t3
t2_bad:
        call    bad
t3:

;--- 7303h, to have something independent to check against -----------
        mov     edx,OFFSET drvpath
        mov     edi,OFFSET frec
        mov     ecx,F73_SIZE
        mov     ax,7303h
        int     21h
        mov     byte ptr [have73],1
        jnc     t3_have
        mov     byte ptr [have73],0
t3_have:

;=====================================================================
; 3 - the cluster size, against 7303h's.
;=====================================================================
        mov     edx,OFFSET msg_t3
        call    puts
        cmp     byte ptr [have73],0
        je      t3_skip
        movzx   eax,byte ptr [dpbbuf+F72_DPB+DPB_SPCM1]
        inc     eax
        cmp     eax,dword ptr [frec+F73_SPC]
        jne     t3_bad
        call    good
        jmp     short t4
t3_skip:
        call    bad
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
        mov     eax,dword ptr [dpbbuf+F72_DPB+DPB_MAXCLUS]
        dec     eax
        cmp     eax,dword ptr [frec+F73_TOTCL]
        jne     t4_bad
        call    good
        jmp     short t5
t4_bad:
        call    bad
t5:

;=====================================================================
; 5 - the chain pointer says there is no chain.  This is the field
;     that would have to lie, and it refuses instead - the same
;     sentinel fn52 uses, and a walker tests for it before it
;     dereferences anything.
;=====================================================================
        mov     edx,OFFSET msg_t5
        call    puts
        cmp     dword ptr [dpbbuf+F72_DPB+DPB_NEXT],0FFFFFFFFh
        jne     t5_bad
        call    good
        jmp     short t6
t5_bad:
        call    bad
t6:

;=====================================================================
; 6 - a buffer with no room in it is refused, and a buffer with SOME
;     room gets that much and not a byte more.  The second half is
;     what a fixed-size answer copied into a caller-sized buffer gets
;     wrong, and it gets it wrong by overwriting something.
;=====================================================================
        mov     edx,OFFSET msg_t6
        call    puts
        mov     edi,OFFSET smallbuf
        mov     ecx,SMALL + GUARD
        mov     al,0CCh
        push    edi
        rep     stosb
        pop     edi
        mov     edx,OFFSET drvpath
        mov     ecx,SMALL
        mov     ax,7302h
        int     21h
        jc      t6_bad
        mov     ecx,GUARD               ; nothing past the length given
t6_guard:
        cmp     byte ptr [smallbuf+SMALL+ecx-1],0CCh
        jne     t6_bad
        loop    t6_guard

        mov     edx,OFFSET drvpath      ; and none at all is an error
        xor     ecx,ecx
        mov     ax,7302h
        int     21h
        jnc     t6_bad
        call    good
        jmp     short t7
t6_bad:
        call    bad
t7:

;=====================================================================
; 7 - 7304h accepts the block 7302h just handed out, because that
;     block describes the volume the kernel has mounted.
;=====================================================================
        mov     edx,OFFSET msg_t7
        call    puts
        call    getdpb
        jc      t7_bad
        mov     edx,OFFSET drvpath
        mov     edi,OFFSET dpbbuf
        mov     ecx,F72_SIZE
        mov     ax,7304h
        int     21h
        jc      t7_bad
        call    good
        jmp     short t8
t7_bad:
        call    bad
t8:

;=====================================================================
; 8 - and refuses one that has been changed.  A "set" that accepted
;     anything would be a "set" that did nothing and said otherwise.
;=====================================================================
        mov     edx,OFFSET msg_t8
        call    puts
        mov     ax,word ptr [dpbbuf+F72_DPB+DPB_BPS]
        xor     ax,0FFFFh
        mov     word ptr [dpbbuf+F72_DPB+DPB_BPS],ax
        mov     edx,OFFSET drvpath
        mov     edi,OFFSET dpbbuf
        mov     ecx,F72_SIZE
        mov     ax,7304h
        int     21h
        jnc     t8_bad
        call    good
        jmp     short t9
t8_bad:
        call    bad
t9:

;=====================================================================
; 9 - 7305h reads.  Sector 0 of a mounted volume is its boot record,
;     which is recognisable without knowing anything else about the
;     disk: it ends in AA55h and its sector size is 512.
;=====================================================================
        mov     edx,OFFSET msg_t9
        call    puts
        mov     ah,19h
        int     21h
        movzx   eax,al
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
        jc      t9_bad
        cmp     word ptr [secbuf+510],0AA55h
        jne     t9_bad
        cmp     word ptr [secbuf+BPB_BPS],512
        jne     t9_bad
        call    good
        jmp     short t10
t9_bad:
        call    bad
t10:

;=====================================================================
; 10 - and CX must be FFFFh.  It is not a formality: INT 25h's own
;      escape for a volume too big to address the old way was ALSO a
;      count of FFFFh, so the value is what tells this call from a
;      program that has confused two APIs.
;=====================================================================
        mov     edx,OFFSET msg_t10
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
        jnc     t10_bad
        call    good
        jmp     short wrapup
t10_bad:
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
; getdpb - the whole block, into dpbbuf.  CF as the call gave it.
;---------------------------------------------------------------------
getdpb:
        push    ecx
        push    edx
        push    edi
        mov     edx,OFFSET drvpath
        mov     edi,OFFSET dpbbuf
        mov     ecx,F72_SIZE
        mov     ax,7302h
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

msg_hi      db  'F32TEST - 7302h, 7304h and 7305h',13,10,13,10,'$'
msg_t1      db  ' 1 7302h answers                          ','$'
msg_t2      db  ' 2 ...with 512-byte sectors               ','$'
msg_t3      db  ' 3 ...cluster size agrees with 7303h      ','$'
msg_t4      db  ' 4 ...cluster count agrees with 7303h     ','$'
msg_t5      db  ' 5 ...and the DPB chain says "none"       ','$'
msg_t6      db  ' 6 a short buffer is filled, not overrun  ','$'
msg_t7      db  ' 7 7304h accepts the volume as it is      ','$'
msg_t8      db  ' 8 ...and refuses it as it is not         ','$'
msg_t9      db  ' 9 7305h reads sector 0, and it is a BPB  ','$'
msg_t10     db  '10 ...and insists on CX = FFFFh           ','$'
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
            db  0,0
pkt_drv     dd  0
resptr      dd  OFFSET results
results     db  16 dup('$')
dpbbuf      db  F72_SIZE + 16 dup(0)
smallbuf    db  SMALL + GUARD dup(0)
frec        db  F73_SIZE dup(0)
pkt         db  DSKIO_LEN dup(0)
secbuf      db  512 dup(0)

_F3     ENDS
        END
