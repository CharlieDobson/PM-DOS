;=====================================================================
; DBGSINK.INC - a debug channel that is not the screen.
;
; WHY THIS EXISTS.  Every probe built into this kernel before it was a
; probe that CHANGED WHAT IT WAS MEASURING, and the Windows hang found
; three of them in one afternoon:
;
;   * V86_TRACE and V86_WATCH print through con_str from inside an
;     interrupt.  That costs enough per event that the guest runs
;     measurably fewer instructions per tick - and WIN.COM reached its
;     logo under the loud watchdog and stopped before it without one.
;     The instrument decided the answer.
;   * V86_WATCHQ writes sixteen bytes into the text page, which is
;     cheap enough - but a guest that has taken the screen into a
;     graphics mode has taken the text page with it.  Drawing the same
;     bits into A0000 came out unreadable, because the VGA's write
;     mode, map mask and set/reset belong to the GUEST at that instant.
;   * V86_NOMODE keeps the screen readable by swallowing the guest's
;     mode set - and then the guest runs against a video state it does
;     not believe in, so anything it does next is a fact about a
;     machine that does not exist.
;
; AND THE PRINTER WAS TRIED FIRST, which is why this file was called
; DBGLPT.INC for an afternoon.  Six OUTs a byte to a parallel port,
; with 86Box's Generic Text Printer catching them on the host: a
; channel the guest is not using and cannot corrupt.  It never
; delivered a byte.  Neither did the machine's own BIOS through INT
; 17h, which is the reference implementation and settled that the
; fault was not in the driver - 86Box drops lpt1_enabled out of the
; configuration file every time it rewrites it on exit, so the port
; was disabled on every run after the first.  Fighting an emulator's
; configuration is not debugging a kernel.
;
; SO THE SINK IS THE DISK, which this kernel already drives itself and
; has to get right anyway.  The trace accumulates in RAM and is
; written, in one transfer, to the sectors between the master boot
; record and the first partition - LBA 1 upwards, which no filesystem
; owns and no boot code reads.  Nothing is formatted, nothing is
; allocated, and no file is opened, so it works with the DOS layer
; wedged, which is the state it exists to report on.  On the host the
; bytes are read straight out of the disk image; on real hardware they
; are read straight off the disk.
;
; AND THE RECORDING IS SEPARATE FROM THE WRITING.  A guest loading
; Windows faults thousands of times a second, so the recorder writes
; EIGHT BYTES INTO A RING BUFFER - no I/O, a dozen instructions - and
; the ring is formatted and written out only once the guest has
; ALREADY stopped, when there is nothing left to perturb.
;=====================================================================

DBG_RING        EQU     512             ; entries, a power of two
DBG_ENT         EQU     16              ; dw ip, dw cs, dw ax, db vec, db flg,
                                        ; dd the four bytes at CS:IP
DBG_MASK        EQU     DBG_RING - 1

DBGE_IP         EQU     0
DBGE_CS         EQU     2
DBGE_AX         EQU     4
DBGE_VEC        EQU     6
DBGE_FLG        EQU     7
DBGE_OP         EQU     8
DBGE_BX         EQU     12
DBGE_DX         EQU     14

DBG_NAMES       EQU     8               ; EXEC names kept, most recent last
DBG_NAMELEN     EQU     32

DBG_BUFSEC      EQU     62              ; 31K of text - LBA 1 to 62 is the
                                        ; whole gap between the master boot
                                        ; record and the first partition at
                                        ; 63, and it is empty in a fresh
                                        ; image.  62 and not 64: a trace that
                                        ; runs into the partition boot record
                                        ; destroys the disk it is reporting on.
DBG_BUFLEN      EQU     DBG_BUFSEC * 512
DBG_LBA         EQU     1               ; between the MBR and the first
                                        ; partition, which starts at 63

;---------------------------------------------------------------------
; dbg_init - start the buffer.  There is no hardware to find.
;---------------------------------------------------------------------
dbg_init:
        pushad
        mov     dword ptr ds:[dbg_head],0
        mov     dword ptr ds:[dbg_count],0
        mov     dword ptr ds:[dbg_nhead],0
        mov     dword ptr ds:[dbg_blen],0
        mov     dword ptr ds:[dbg_age],0
        mov     byte ptr ds:[dbg_dumped],0
        popad
        ret

;---------------------------------------------------------------------
; dbg_out - AL into the buffer.  Silently drops anything past the end,
;           because a trace that overruns its own sectors and takes
;           the partition table with it is worse than a short trace.
;---------------------------------------------------------------------
dbg_out:
        push    edi
        push    eax
        mov     edi,dword ptr ds:[dbg_blen]
        cmp     edi,DBG_BUFLEN
        jae     dbg_out9
        add     edi,OFFSET dbg_buf
        mov     [edi],al
        inc     dword ptr ds:[dbg_blen]
dbg_out9:
        pop     eax
        pop     edi
        ret

;---------------------------------------------------------------------
; dbg_flush - the buffer to the disk, in one transfer.
;
; Drive 80h, because the machine this is for boots from it.  blk_write
; goes to the kernel's own ATA driver, which is port I/O and nothing
; else - no BIOS, no real-mode thunk, no DOS - and that is exactly
; what makes it safe to call with a guest hung underneath.
;---------------------------------------------------------------------
dbg_flush:
        pushad
        mov     al,80h
        mov     ebx,DBG_LBA
        mov     cx,DBG_BUFSEC
        mov     edi,OFFSET dbg_buf
        call    blk_write
        popad
        ret

;---------------------------------------------------------------------
; dbg_str - DS:ESI, ASCIIZ.
;---------------------------------------------------------------------
dbg_str:
        push    eax
        push    esi
dbg_str1:
        lodsb
        test    al,al
        jz      dbg_str9
        call    dbg_out
        jmp     short dbg_str1
dbg_str9:
        pop     esi
        pop     eax
        ret

dbg_crlf:
        push    eax
        mov     al,13
        call    dbg_out
        mov     al,10
        call    dbg_out
        pop     eax
        ret

dbg_space:
        push    eax
        mov     al,' '
        call    dbg_out
        pop     eax
        ret

;---------------------------------------------------------------------
; dbg_hex8 / dbg_hex16 / dbg_hex32 - AL, AX, EAX in hex.
;---------------------------------------------------------------------
dbg_hex32:
        push    eax
        shr     eax,16
        call    dbg_hex16
        pop     eax
        call    dbg_hex16
        ret

dbg_hex16:
        push    eax
        shr     eax,8
        call    dbg_hex8
        pop     eax
        call    dbg_hex8
        ret

dbg_hex8:
        push    eax
        push    ecx
        mov     ecx,eax
        shr     al,4
        call    dbg_nib
        mov     eax,ecx
        call    dbg_nib
        pop     ecx
        pop     eax
        ret

dbg_nib:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     dbg_nib1
        add     al,7
dbg_nib1:
        jmp     dbg_out

;---------------------------------------------------------------------
; dbg_rec - record one guest fault.  EBP = the fault frame.
;
; This is the hot path: it runs on every trap the guest takes, which
; while Windows is loading is thousands a second.  So it does no I/O,
; makes no calls, and touches nothing but the ring.
;---------------------------------------------------------------------
dbg_rec:
        push    eax
        push    ebx
        push    edx
        push    edi
        mov     edi,dword ptr ds:[dbg_head]
        and     edi,DBG_MASK
        imul    edi,DBG_ENT
        add     edi,OFFSET dbg_ringbuf
        mov     eax,[ebp+FR_EIP]
        mov     word ptr [edi+DBGE_IP],ax
        mov     ebx,[ebp+FR_CS]
        mov     word ptr [edi+DBGE_CS],bx
        mov     eax,[ebp+PA_EAX]
        mov     word ptr [edi+DBGE_AX],ax

        ; BX AND DX AS WELL, because on a DOS call AX names the
        ; function and the OTHER two name what it is being asked
        ; about.  "IOCTL 4408" is not a fact until it says which
        ; drive, and a call that blocks blocks on the argument.
        mov     eax,[ebp+PA_EBX]
        mov     word ptr [edi+DBGE_BX],ax
        mov     eax,[ebp+PA_EDX]
        mov     word ptr [edi+DBGE_DX],ax

        ; WHICH INTERRUPT, not which fault.  With IOPL 0 every INT the
        ; guest executes arrives here as a general protection fault, so
        ; recording the fault vector writes 0D into all five hundred
        ; entries and answers nothing.  The vector is in the
        ; instruction: CD nn.  Anything else keeps its fault vector and
        ; stores the OPCODE beside it, which is how an IN, an OUT and a
        ; HLT tell themselves apart afterwards.
        and     ebx,0FFFFh
        shl     ebx,4
        movzx   edx,word ptr [ebp+FR_EIP]
        add     ebx,edx
        ; FOUR BYTES, NOT ONE.  One byte cannot tell F3 6E - a REP
        ; OUTSB - from any other instruction beginning F3, and a
        ; string I/O instruction is exactly the kind a V86 monitor
        ; forgets to implement and then emulates for ever without
        ; advancing anything.  Four bytes covers a prefix, an opcode,
        ; a modrm and a displacement.
        mov     eax,[ebx]
        mov     [edi+DBGE_OP],eax
        movzx   edx,byte ptr [ebx]
        cmp     dl,0CDh
        jne     dbg_rec1
        movzx   eax,byte ptr [ebx+1]
        mov     byte ptr [edi+DBGE_VEC],al
        mov     byte ptr [edi+DBGE_FLG],0CDh
        jmp     short dbg_rec2
dbg_rec1:
        mov     eax,[ebp+FR_VEC]
        mov     byte ptr [edi+DBGE_VEC],al
        mov     byte ptr [edi+DBGE_FLG],dl
dbg_rec2:
        inc     dword ptr ds:[dbg_head]
        inc     dword ptr ds:[dbg_count]

        ; The guest is still asking the machine things, so the
        ; watchdog's clock goes back to zero.  It only fires when this
        ; line has not run for five seconds.
        mov     dword ptr ds:[v86_watch],0

        ; ...and if that was EXEC, the name as well.  Rare enough that
        ; reading the guest's own memory here costs nothing over a boot.
        cmp     byte ptr [edi+DBGE_FLG],0CDh
        jne     dbg_rec9
        cmp     byte ptr [edi+DBGE_VEC],21h
        jne     dbg_rec9
        mov     eax,[ebp+PA_EAX]
        cmp     ah,4Bh
        jne     dbg_rec9
        call    dbg_exec
dbg_rec9:
        pop     edi
        pop     edx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; dbg_exec - record the name a guest just asked EXEC to load.
;            EBP = the fault frame; the guest's DS:DX points at it.
;
; WHICH PROGRAM IS RUNNING is the first question about a hang and the
; ring cannot answer it: a ring of addresses says where, and the
; segment a program happens to be loaded at says nothing about which
; program it is.  WIN.COM runs DOSX.EXE, DOSX runs KRNL286 - and
; "the last name that was loaded" separates "it never got there" from
; "it got there and stopped".
;
; Only called for INT 21h AH=4Bh, so the cost of reading the guest's
; memory is paid a handful of times in a boot.  Linear IS physical
; here, so the guest's far pointer is just an address.
;---------------------------------------------------------------------
dbg_exec:
        pushad
        mov     esi,[ebp+FR_DS]
        and     esi,0FFFFh
        shl     esi,4
        movzx   eax,word ptr [ebp+PA_EDX]
        add     esi,eax
        mov     edi,dword ptr ds:[dbg_nhead]
        and     edi,DBG_NAMES - 1
        imul    edi,DBG_NAMELEN
        add     edi,OFFSET dbg_nametab
        mov     ecx,DBG_NAMELEN - 1
dbg_ex1:
        lodsb
        test    al,al
        jz      dbg_ex2
        stosb
        loop    dbg_ex1
dbg_ex2:
        xor     al,al
        stosb
        inc     dword ptr ds:[dbg_nhead]
        popad
        ret

;---------------------------------------------------------------------
; dbg_dump - print the ring, oldest first, and the EXEC names.
;
; Called once, from the watchdog, when the guest has already stopped.
; Nothing here needs to be quick.
;---------------------------------------------------------------------
dbg_dump:
        pushad
        mov     esi,OFFSET dbg_mhead
        call    dbg_str
        mov     esi,OFFSET dbg_mcount
        call    dbg_str
        mov     eax,dword ptr ds:[dbg_count]
        call    dbg_hex32
        mov     esi,OFFSET dbg_mage
        call    dbg_str
        mov     eax,dword ptr ds:[dbg_age]
        call    dbg_hex32
        call    dbg_crlf

        ; The names first: they are the shape of the run, and the ring
        ; is the last inch of it.
        mov     esi,OFFSET dbg_mnames
        call    dbg_str
        mov     ecx,dword ptr ds:[dbg_nhead]
        cmp     ecx,DBG_NAMES
        jbe     dbg_dn0
        mov     ecx,DBG_NAMES
dbg_dn0:
        test    ecx,ecx
        jz      dbg_dn3
        mov     ebx,dword ptr ds:[dbg_nhead]
        sub     ebx,ecx
dbg_dn1:
        mov     esi,ebx
        and     esi,DBG_NAMES - 1
        imul    esi,DBG_NAMELEN
        add     esi,OFFSET dbg_nametab
        mov     al,' '
        call    dbg_out
        call    dbg_str
        call    dbg_crlf
        inc     ebx
        dec     ecx
        jnz     dbg_dn1
dbg_dn3:

        ; ...and now the ring.
        mov     esi,OFFSET dbg_mring
        call    dbg_str
        mov     ecx,dword ptr ds:[dbg_count]
        cmp     ecx,DBG_RING
        jbe     dbg_dr0
        mov     ecx,DBG_RING
dbg_dr0:
        test    ecx,ecx
        jz      dbg_dr9
        mov     ebx,dword ptr ds:[dbg_head]
        sub     ebx,ecx
        xor     edi,edi                 ; how many on this line
dbg_dr1:
        push    ecx
        mov     esi,ebx
        and     esi,DBG_MASK
        imul    esi,DBG_ENT
        add     esi,OFFSET dbg_ringbuf
        movzx   eax,byte ptr [esi+DBGE_VEC]
        call    dbg_hex8
        mov     al,'/'
        call    dbg_out
        movzx   eax,word ptr [esi+DBGE_CS]
        call    dbg_hex16
        mov     al,':'
        call    dbg_out
        movzx   eax,word ptr [esi+DBGE_IP]
        call    dbg_hex16
        mov     al,'.'
        call    dbg_out
        movzx   eax,word ptr [esi+DBGE_AX]
        call    dbg_hex16
        mov     al,'='
        call    dbg_out
        mov     eax,[esi+DBGE_OP]
        call    dbg_hex32
        mov     al,'b'
        call    dbg_out
        movzx   eax,word ptr [esi+DBGE_BX]
        call    dbg_hex16
        mov     al,'d'
        call    dbg_out
        movzx   eax,word ptr [esi+DBGE_DX]
        call    dbg_hex16
        pop     ecx
        inc     edi
        cmp     edi,2
        jb      dbg_dr2
        xor     edi,edi
        call    dbg_crlf
        jmp     short dbg_dr3
dbg_dr2:
        call    dbg_space
dbg_dr3:
        inc     ebx
        dec     ecx
        jnz     dbg_dr1
dbg_dr9:
        call    dbg_crlf
        mov     esi,OFFSET dbg_mtail
        call    dbg_str
        call    dbg_flush
dbg_dmp9:
        popad
        ret

dbg_head        dd      0
dbg_count       dd      0
dbg_nhead       dd      0
dbg_blen        dd      0
dbg_age         dd      0
dbg_dumped      db      0
dbg_mstop       db      13,10,'STOPPED, mode ',0
dbg_mhead       db      13,10,'== PM-DOS guest trace ==',13,10,0
dbg_mcount      db      'traps ',0
dbg_mage        db      '  ticks ',0
dbg_mnames      db      'EXEC:',13,10,0
dbg_mring       db      'RING (vec/cs:ip.ax), oldest first:',13,10,0
dbg_mtail       db      '== end ==',13,10,0
dbg_nametab     db      DBG_NAMES * DBG_NAMELEN dup (0)
dbg_ringbuf     db      DBG_RING * DBG_ENT dup (0)
dbg_buf         db      DBG_BUFLEN dup (0)

; AND A GUARD, BECAUSE THIS FILE ALREADY FELL INTO IT ONCE.  The
; buffer was called dbg_ring, the count of entries in it was called
; DBG_RING, and wasm FOLDS CASE - so they were one symbol, the EQU
; won, and "add edi,OFFSET dbg_ring" assembled to "add edi,512" with
; no warning of any kind.  Every guest trap then wrote eight bytes
; over the interrupt vector table, and the machine reset so fast that
; it never printed anything to say so.
;
; There is no assembly-time check for it, and that was tried: OFFSET of
; a relocatable label is not a constant to wasm, so IF cannot weigh it.
; The rule has to be kept by naming instead - no constant in this file
; may spell a label with the case changed.

