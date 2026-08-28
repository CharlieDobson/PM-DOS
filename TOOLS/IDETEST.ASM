;=====================================================================
; IDETEST.ASM - IDETEST.EXE, how is this machine moving its disk data,
;               and is it moving it correctly?
;
; v0.43 gave ATA.INC two faster paths than the REP INSW it has always
; used: 32-bit PIO, and bus-master DMA.  Both are decided by probing -
; a word in IDENTIFY, a device in PCI configuration space - and both
; fail in the same nasty way when the probe is wrong, which is not an
; error but WRONG DATA.  So this program has two jobs, and the second
; matters more than the first.
;
; The first is to REPORT: what the controller is, which path each disk
; ended up on, and in which transfer mode.  None of that can be seen
; from a program - a native program runs at ring 3 with no I/O
; privilege and cannot read a controller's registers even to describe
; them - so it is asked for through 21h/F1h AL=0Ah.
;
; The second is to CHECK, and the checks are built round one idea: the
; same bytes read two different ways must agree.  A run of sectors
; read in ONE transfer goes through the widest path the machine has -
; several descriptors, a 64K boundary in the middle of it - while the
; same sectors read ONE AT A TIME go through the narrowest.  If the
; descriptor arithmetic is wrong, or the controller latches half a
; doubleword, the two disagree, and there is no other way to see it
; from up here.
;
; The straddling read is deliberate.  A bus-master descriptor may not
; cross a 64K physical boundary, so ata_prdbuild cuts the buffer at
; every one it spans - and a buffer that never spans one never tests
; that.  The read below is arranged to sit half on either side.
;
; AND ONE CHECK IS A COMPARISON ACROSS BOOTS.  The checksum printed at
; the end is of the same sectors every time: run this with IDEDMA=AUTO
; in CONFIG.SYS and again with IDEDMA=OFF, and the two numbers must
; match.  That is the only test here that compares the fast path
; against something the fast path had no part in producing.
;
; Nine checks:
;
;   1  F1h AL=0Ah answers for the controller
;   2  at least one ATA disk was found - without one there is nothing
;      here to test, and the program says so rather than failing
;   3  every device's flags are self-consistent: no DMA without an
;      engine to do it and a mode to do it in
;   4  a DOS drive letter belongs to one of those disks
;   5  INT 25h reads a sector of it
;   6  the same sector read twice is the same sector
;   7  32 sectors read at once, across a 64K boundary, are the same
;      32 sectors read one at a time
;   8  ...and so are they read into an ODD address, which no bus
;      master can be pointed at, so this one is the PIO path proving
;      the fallback in ata_dmaok is live
;   9  48K written to a file and read back is intact
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_ID     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_ID, DS:_ID, ES:_ID, SS:_ID
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET id_entry>,<0>,4096

ARENA           EQU     0040000h        ; room for the three buffers, with
                                        ; a 64K line inside it to straddle
RUNSECS         EQU     32              ; 16K: two descriptors, always
RUNBYTES        EQU     RUNSECS * 512
FILEBYTES       EQU     49152           ; and a file big enough to be many

id_entry:
        mov     edx,OFFSET msg_hi
        call    puts

        ; the same reason VFYTEST has one: several of these calls are
        ; entitled to fail, and a critical error that reaches COMMAND
        ; stops the machine waiting for a keystroke
        mov     edx,OFFSET my24
        mov     ax,2524h
        int     21h

;=====================================================================
; 1 - the controller
;=====================================================================
        mov     edx,OFFSET msg_t1
        call    puts
        mov     eax,0F10Ah
        mov     edx,0FFh
        int     21h
        jc      t1_bad
        mov     [ndisks],eax
        mov     [pciid],ebx
        mov     [bmbase],ecx
        mov     [ctlflags],edx
        call    good
        jmp     short t1_done
t1_bad:
        call    bad
t1_done:
        call    report

;=====================================================================
; 2 - is there anything of ours on it?
;=====================================================================
        mov     edx,OFFSET msg_t2
        call    puts
        cmp     dword ptr [ndisks],0
        je      t2_none
        call    good
        jmp     short t2_done
t2_none:
        ; NOT A FAILURE.  DOSBox 0.74 has no IDE controller at all and
        ; the whole kernel reaches its disks through the BIOS thunk
        ; there; so does any real machine whose controller this driver
        ; does not recognise.  A test that cannot run is not a test
        ; that failed, and calling it one would mean every DOSBox run
        ; reported a fault in hardware support it is not using.
        mov     dl,'-'
        call    tally
        mov     byte ptr [skipped],1
        mov     edx,OFFSET msg_skip
        call    puts
        mov     edx,OFFSET msg_noide
        call    puts
        jmp     finish
t2_done:

;=====================================================================
; 3 - each device's account of itself hangs together
;
; DMA claimed with no engine, or with no mode selected, would mean the
; driver had talked itself into something; so would 32-bit PIO on a
; device that is not there.
;=====================================================================
        mov     edx,OFFSET msg_t3
        call    puts
        xor     ebx,ebx                 ; EBX = device index
        mov     byte ptr [t3bad],0
t3_lp:
        cmp     ebx,4
        jae     t3_done
        push    ebx
        mov     eax,0F10Ah
        mov     edx,ebx
        int     21h
        mov     [devflags],eax
        mov     [devbm],ebx
        mov     [devmode],ecx
        mov     [devsecs],edx
        pop     ebx

        test    byte ptr [devflags],ATDF_PRESENT
        jz      t3_next
        test    byte ptr [devflags],ATDF_DMA
        jz      t3_next
        cmp     dword ptr [devbm],0     ; DMA with no engine
        je      t3_wrong
        cmp     dword ptr [devmode],0   ; DMA in no mode
        je      t3_wrong
        jmp     short t3_next
t3_wrong:
        mov     byte ptr [t3bad],1
t3_next:
        inc     ebx
        jmp     short t3_lp
t3_done:
        cmp     byte ptr [t3bad],0
        jne     t3_fail
        call    good
        jmp     short t3_shown
t3_fail:
        call    bad
t3_shown:
        call    report_disks

;=====================================================================
; 4 - which drive letter is one of them?
;
; A drive record carries the BIOS number its volume lives on, and the
; ATA driver hands out 80h upwards in probe order, so a drive whose
; number is inside that run is a drive this test can use.  A floppy is
; not one and neither is a loaded block driver's unit.
;=====================================================================
        mov     edx,OFFSET msg_t4
        call    puts
        xor     ebx,ebx
t4_lp:
        cmp     ebx,LASTDRIVE
        jae     t4_no
        mov     edx,ebx                 ; F1h AL=6 is 0-based
        mov     edi,OFFSET drv_rec
        mov     eax,0F106h
        int     21h
        jc      t4_next
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_PRESENT
        jz      t4_next
        test    byte ptr [drv_rec+DRV_FLAGS],DRVF_FLOPPY OR DRVF_DRIVER
        jnz     t4_next
        movzx   eax,byte ptr [drv_rec+DRV_BIOS]
        cmp     eax,80h
        jb      t4_next
        sub     eax,80h
        cmp     eax,[ndisks]
        jae     t4_next
        mov     [tstdrv],ebx
        mov     [tstdisk],eax
        call    good
        mov     al,[drv_rec+DRV_BIOS]
        call    which_drive
        jmp     short t4_done
t4_next:
        inc     ebx
        jmp     short t4_lp
t4_no:
        call    bad
        mov     edx,OFFSET msg_nodrv
        call    puts
        jmp     finish
t4_done:

;--- and the room the reads need -------------------------------------
        mov     ebx,ARENA
        mov     ah,48h
        int     21h
        jnc     mem_ok
        mov     edx,OFFSET msg_nomem
        call    puts
        jmp     finish
mem_ok:
        ; a 64K line with room IN FRONT of it, because half the
        ; straddling read has to sit on the near side
        add     eax,8000h
        add     eax,0FFFFh
        and     eax,0FFFF0000h
        mov     [line],eax
        sub     eax,RUNBYTES / 2
        mov     [bigbuf],eax            ; half on each side of the line
        mov     eax,[line]
        add     eax,0C000h              ; clear of the FILE test, which is
        mov     [refbuf],eax            ; three times the size of the read
        mov     eax,[line]              ; and reaches further past the line
        add     eax,18001h              ; and one no bus master can take
        mov     [oddbuf],eax

;=====================================================================
; 5 - a sector, through INT 25h
;=====================================================================
        mov     edx,OFFSET msg_t5
        call    puts
        mov     ebx,[refbuf]
        xor     edx,edx
        mov     ecx,1
        mov     eax,[tstdrv]
        int     25h
        jc      t5_bad
        call    good
        jmp     short t5_done
t5_bad:
        call    bad
        jmp     finish
t5_done:

;=====================================================================
; 6 - and it reads the same the second time
;=====================================================================
        mov     edx,OFFSET msg_t6
        call    puts
        mov     ebx,[oddbuf]
        xor     edx,edx
        mov     ecx,1
        mov     eax,[tstdrv]
        int     25h
        jc      t6_bad
        mov     esi,[refbuf]
        mov     edi,[oddbuf]
        mov     ecx,512
        call    same
        jc      t6_bad
        call    good
        jmp     short t6_done
t6_bad:
        call    bad
t6_done:

;=====================================================================
; 7 - 32 sectors in one transfer, across a 64K line, against the same
;     32 read one at a time
;
; THIS IS THE ONE THE OTHERS EXIST TO SUPPORT.  One transfer is the
; whole of the fast path: several descriptors, a boundary cut in the
; middle, and the controller rather than the processor doing the
; moving.  One sector at a time is a single descriptor every time, and
; on a machine where DMA has been refused it is not DMA at all.  The
; two must produce identical bytes.
;=====================================================================
        mov     edx,OFFSET msg_t7
        call    puts

        mov     ebx,[refbuf]            ; the slow way first
        xor     esi,esi                 ; ESI = sector
t7_lp:
        cmp     esi,RUNSECS
        jae     t7_slow
        push    esi
        push    ebx
        mov     edx,esi
        mov     ecx,1
        mov     eax,[tstdrv]
        int     25h
        pop     ebx
        pop     esi
        jc      t7_bad
        add     ebx,512
        inc     esi
        jmp     short t7_lp
t7_slow:
        mov     ebx,[bigbuf]            ; and the fast one
        xor     edx,edx
        mov     ecx,RUNSECS
        mov     eax,[tstdrv]
        int     25h
        jc      t7_bad

        mov     esi,[refbuf]
        mov     edi,[bigbuf]
        mov     ecx,RUNBYTES
        call    same
        jc      t7_bad
        call    good
        jmp     short t7_done
t7_bad:
        call    bad
t7_done:

;=====================================================================
; 8 - the same run into an ODD address
;
; A descriptor cannot name an odd byte, so ata_dmaok has to refuse
; this one and the transfer has to happen anyway, by PIO.  A machine
; that gets the right bytes here and the wrong ones above has a bus
; master problem; one that fails here has something worse.
;=====================================================================
        mov     edx,OFFSET msg_t8
        call    puts
        mov     ebx,[oddbuf]
        xor     edx,edx
        mov     ecx,RUNSECS
        mov     eax,[tstdrv]
        int     25h
        jc      t8_bad
        mov     esi,[refbuf]
        mov     edi,[oddbuf]
        mov     ecx,RUNBYTES
        call    same
        jc      t8_bad
        call    good
        jmp     short t8_done
t8_bad:
        call    bad
t8_done:

;=====================================================================
; 9 - and the write path, which is the filesystem's rather than 25h's
;=====================================================================
        mov     edx,OFFSET msg_t9
        call    puts

        mov     al,byte ptr [tstdrv]
        add     al,'A'
        mov     [filename],al

        mov     edi,[bigbuf]            ; something recognisable
        xor     ecx,ecx
t9_fill:
        cmp     ecx,FILEBYTES
        jae     t9_made
        mov     eax,ecx
        xor     al,ah
        add     al,'A'
        mov     [edi+ecx],al
        inc     ecx
        jmp     short t9_fill
t9_made:
        mov     edx,OFFSET filename
        xor     ecx,ecx
        mov     ah,3Ch
        int     21h
        jc      t9_bad
        mov     [filehdl],eax

        mov     ebx,eax
        mov     edx,[bigbuf]
        mov     ecx,FILEBYTES
        mov     ah,40h
        int     21h
        jc      t9_close
        cmp     eax,FILEBYTES
        jne     t9_close

        mov     ebx,[filehdl]
        mov     ah,3Eh
        int     21h
        mov     dword ptr [filehdl],-1

        mov     edx,OFFSET filename
        mov     ax,3D00h
        int     21h
        jc      t9_bad
        mov     [filehdl],eax
        mov     ebx,eax
        mov     edx,[refbuf]
        mov     ecx,FILEBYTES
        mov     ah,3Fh
        int     21h
        jc      t9_close
        cmp     eax,FILEBYTES
        jne     t9_close

        mov     esi,[bigbuf]
        mov     edi,[refbuf]
        mov     ecx,FILEBYTES
        call    same
        jc      t9_close
        mov     byte ptr [t9ok],1
t9_close:
        cmp     dword ptr [filehdl],-1
        je      t9_gone
        mov     ebx,[filehdl]
        mov     ah,3Eh
        int     21h
t9_gone:
        mov     edx,OFFSET filename
        mov     ah,41h
        int     21h
        cmp     byte ptr [t9ok],0
        je      t9_bad
        call    good
        jmp     short t9_done
t9_bad:
        call    bad
t9_done:

;=====================================================================
; and the number to compare against another boot's
;=====================================================================
        mov     ebx,[refbuf]
        xor     edx,edx
        mov     ecx,RUNSECS
        mov     eax,[tstdrv]
        int     25h
        jc      finish
        mov     esi,[refbuf]
        xor     eax,eax
        mov     ecx,RUNBYTES / 4
sum_lp:
        add     eax,[esi]
        rol     eax,1
        add     esi,4
        dec     ecx
        jnz     sum_lp
        mov     [sumval],eax
        mov     edx,OFFSET msg_sum
        call    puts
        mov     eax,[sumval]
        call    phex8
        mov     edx,OFFSET msg_crlf
        call    puts

finish:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        je      fin_ok
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h
fin_ok:
        cmp     byte ptr [skipped],0
        je      fin_pass
        mov     edx,OFFSET msg_skipped
        call    puts
        mov     ax,4C00h
        int     21h
fin_pass:
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; report - what the controller came back as
;=====================================================================
report:
        mov     edx,OFFSET msg_disks
        call    puts
        mov     eax,[ndisks]
        call    pdec
        mov     edx,OFFSET msg_crlf
        call    puts

        test    dword ptr [ctlflags],00010000h
        jz      rep_nopci
        mov     edx,OFFSET msg_pci
        call    puts
        mov     eax,[pciid]
        call    phex8
        mov     edx,OFFSET msg_crlf
        call    puts
        test    dword ptr [ctlflags],00020000h
        jz      rep_nobm
        mov     edx,OFFSET msg_bm
        call    puts
        mov     eax,[bmbase]
        call    phex8
        mov     edx,OFFSET msg_crlf
        call    puts
        jmp     short rep_pol
rep_nobm:
        mov     edx,OFFSET msg_nobm
        call    puts
        jmp     short rep_pol
rep_nopci:
        mov     edx,OFFSET msg_nopci
        call    puts
rep_pol:
        mov     edx,OFFSET msg_pol
        call    puts
        mov     eax,[ctlflags]
        shr     eax,24
        and     eax,0Fh
        call    pdec
        mov     edx,OFFSET msg_pol2
        call    puts
        mov     eax,[ctlflags]
        shr     eax,28
        and     eax,0Fh
        call    pdec
        mov     edx,OFFSET msg_crlf
        call    puts
        ret

;=====================================================================
; report_disks - a line each for the disks that are there
;=====================================================================
report_disks:
        push    ebx
        xor     ebx,ebx
rd_lp:
        cmp     ebx,4
        jae     rd_done
        push    ebx
        mov     eax,0F10Ah
        mov     edx,ebx
        int     21h
        mov     [devflags],eax
        mov     [devbm],ebx
        mov     [devmode],ecx
        mov     [devsecs],edx
        pop     ebx
        test    byte ptr [devflags],ATDF_PRESENT
        jz      rd_next

        mov     edx,OFFSET msg_disk
        mov     eax,ebx
        add     al,'0'
        mov     [msg_disk+9],al
        call    puts

        mov     eax,[devsecs]
        shr     eax,11                  ; sectors -> MB
        call    pdec
        mov     edx,OFFSET msg_mb
        call    puts

        test    byte ptr [devflags],ATDF_DMA
        jz      rd_pio
        mov     edx,OFFSET msg_dma
        call    puts
        mov     eax,[devmode]
        call    phex2
        jmp     short rd_wide
rd_pio:
        mov     edx,OFFSET msg_pio
        call    puts
rd_wide:
        test    byte ptr [devflags],ATDF_PIO32
        jz      rd_eol
        mov     edx,OFFSET msg_32
        call    puts
rd_eol:
        mov     edx,OFFSET msg_crlf
        call    puts
rd_next:
        inc     ebx
        jmp     short rd_lp
rd_done:
        pop     ebx
        ret

which_drive:
        push    eax
        mov     eax,[tstdrv]
        add     al,'A'
        mov     [msg_which+11],al
        mov     edx,OFFSET msg_which
        call    puts
        pop     eax
        ret

;=====================================================================
; same - ESI and EDI for ECX bytes.  CF set if they differ.
;=====================================================================
same:
        push    ecx
        push    esi
        push    edi
        repe    cmpsb
        pop     edi
        pop     esi
        pop     ecx
        je      sm_same
        stc
        ret
sm_same:
        clc
        ret

;=====================================================================
; INT 24h - answer FAIL and say nothing
;=====================================================================
my24:
        mov     al,3
        iret

;=====================================================================
; Output helpers
;=====================================================================
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

phex8:
        push    eax
        push    ecx
        push    edx
        mov     ecx,8
ph8_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph8_d
        add     al,7
ph8_d:
        mov     dl,al
        push    eax
        mov     ah,02h
        int     21h
        pop     eax
        pop     eax
        dec     ecx
        jnz     ph8_lp
        pop     edx
        pop     ecx
        pop     eax
        ret

phex2:
        push    eax
        shl     eax,24
        push    ecx
        push    edx
        mov     ecx,2
ph2_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph2_d
        add     al,7
ph2_d:
        mov     dl,al
        push    eax
        mov     ah,02h
        int     21h
        pop     eax
        pop     eax
        dec     ecx
        jnz     ph2_lp
        pop     edx
        pop     ecx
        pop     eax
        ret

pdec:
        pushad
        mov     edi,OFFSET decbuf + 11
        mov     byte ptr [edi],'$'
        mov     ebx,10
pd_lp:
        xor     edx,edx
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        call    puts
        popad
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

;=====================================================================
msg_hi      db  'IDETEST - how is the disk data moving?',13,10,13,10,'$'
msg_t1      db  ' 1 the controller answers                 ','$'
msg_t2      db  ' 2 there is an ATA disk to test           ','$'
msg_t3      db  ' 3 each disk hangs together               ','$'
msg_t4      db  ' 4 a drive letter belongs to one          ','$'
msg_t5      db  ' 5 INT 25h reads a sector                 ','$'
msg_t6      db  ' 6 and reads it the same twice            ','$'
msg_t7      db  ' 7 32 at once = 32 one at a time          ','$'
msg_t8      db  ' 8 ...and into an odd address too         ','$'
msg_t9      db  ' 9 48K written and read back is intact    ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_disks   db  '    disks: ','$'
msg_pci     db  '    PCI IDE controller: ','$'
msg_nopci   db  '    no PCI: 16-bit PIO unless the drive asks otherwise',13,10,'$'
msg_nobm    db  '    PCI, but no bus-master registers',13,10,'$'
msg_bm      db  '    bus-master registers at ','$'
msg_pol     db  '    IDEDMA policy ','$'
msg_pol2    db  ', IDE32 policy ','$'
msg_disk    db  '    disk N: ','$'
msg_mb      db  'MB, ','$'
msg_dma     db  'bus-master DMA, mode ','$'
msg_pio     db  'PIO','$'
msg_32      db  ' + 32-bit','$'
msg_which   db  '    (it is X:)',13,10,'$'
msg_sum     db  13,10,'Checksum of the first 32 sectors: ','$'
msg_noide   db  13,10,'  No ATA disk was found.  This machine reaches its',13,10
            db  '  disks through the BIOS, and there is nothing here',13,10
            db  '  for this program to test.',13,10,'$'
msg_nodrv   db  13,10,'  No drive letter lives on an ATA disk.',13,10,'$'
msg_nomem   db  13,10,'  Could not allocate the buffers.',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_skip    db  'skipped',13,10,'$'
msg_skipped db  'IDETEST: nothing on this machine to test',13,10,'$'
msg_pass    db  'IDETEST: all checks passed',13,10,'$'
msg_fail    db  'IDETEST: FAILURES',13,10,'$'

filename    db  'X:\IDETEST.TMP',0

ndisks      dd  0
pciid       dd  0
bmbase      dd  0
ctlflags    dd  0
devflags    dd  0
devbm       dd  0
devmode     dd  0
devsecs     dd  0
tstdrv      dd  0
tstdisk     dd  0
line        dd  0
bigbuf      dd  0
refbuf      dd  0
oddbuf      dd  0
sumval      dd  0
filehdl     dd  -1
t3bad       db  0
t9ok        db  0
skipped     db  0
fails       db  0
            db  0
decbuf      db  12 dup(0)
resptr      dd  OFFSET results
results     db  16 dup('$')
drv_rec     db  DRV_LEN dup(0)

_ID     ENDS
        END
