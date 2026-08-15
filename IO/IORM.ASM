;=====================================================================
; IORM.ASM - PMIO.SYS real-mode stub
;
; This is the first half of PMIO.SYS.  It is linked as a standalone
; raw binary of exactly RMSTUB_SIZE (4K) bytes at segment-relative
; offset 0, and is concatenated with IOPM.BIN to form PMIO.SYS.
;
; The boot sector loads the whole thing at IO_LOAD_SEG:0000, which is
; NOT where any of it ends up.  Moving it is this file's first job:
; the stub copies itself down to IO_SEG, and once unreal mode is
; running it blits its protected-mode half above 1MB to IO_PM_BASE.
; That is what buys the DOS box its 624K, and it keeps the knowledge
; here rather than in 512 bytes of boot sector that cannot afford it.
;
; It has two entry points:
;
;   offset 0000h  rm_main   - cold start, called by the boot sector
;                             in real mode with DL = BIOS drive.
;   offset 0010h  pm2rm     - the protected-mode -> real-mode thunk,
;                             entered by a far JMP to SEL_CODE16:0010.
;
; It also owns the GDT (at offset GDT_OFF) and the global variables
; block (at offset GV_OFF), both of which stay resident forever.
;
; Linked with:  wlink form raw bin option offset=0
;=====================================================================
        .386P

        INCLUDE PMDOS.INC

; The BPB the boot sector left at 0000:7C00, and the buffers the file
; loader works through, are all described by FATLOAD.INC / PMDOS.INC.

_RM     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RM, DS:_RM, ES:NOTHING, SS:NOTHING
        ORG     0

;=====================================================================
; Fixed entry vectors
;=====================================================================
        jmp     rm_main                 ; offset 0000h

        ORG     0010h
;---------------------------------------------------------------------
; pm2rm - 32-bit protected mode to real mode and back.
;
; Entered by a far JMP SEL_CODE16:0010h with interrupts already
; disabled.  Expects:
;   RMFRAME       filled in with the register values to hand the BIOS
;   RMR_INTNO     the interrupt number to invoke
;   RMRET         a 16:32 far pointer to resume at
;   RMSAVE_ESP    the caller's 32-bit ESP (restored by the caller)
; On return RMFRAME holds the registers the BIOS gave back.
;---------------------------------------------------------------------
pm2rm:
        mov     ax,SEL_DATA16
        mov     ds,ax
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     sp,RM_STACK_TOP         ; a stack that is legal in 64K

        ; PAGING OFF FIRST, AND IT IS ITS OWN WRITE TO CR0.  Protected
        ; mode cannot be left with paging on, and the manual asks for
        ; PG to be cleared before PE rather than both at once.  The
        ; code doing it has to be identity-mapped, which everything
        ; here is - the map is the identity everywhere except the
        ; sixteen pages of the A20 alias, and those are 1MB up.
        ; Unconditional: clearing a bit that is already clear costs a
        ; write to CR0 on a machine that is about to do two more.
        mov     eax,cr0
        and     eax,7FFFFFFFh
        mov     cr0,eax

        mov     eax,cr0                 ; leave protected mode
        and     al,0FEh
        mov     cr0,eax

        db      0EAh                    ; JMP FAR IO_SEG:rm_land
        dw      OFFSET rm_land
        dw      IO_SEG

;---------------------------------------------------------------------
rm_land:
        xor     ax,ax                   ; genuine real mode again
        mov     ds,ax
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     sp,RM_STACK_TOP
        lidt    fword ptr cs:[rm_idtr]  ; put the BIOS IVT back
        call    pic_to_rm               ; BIOS expects IRQs at 08h/70h

        ; Patch the INT opcode from the frame (DS = 0 addresses RMFRAME)
        push    es
        mov     bx,IO_SEG
        mov     es,bx
        mov     al,byte ptr ds:[RMR_INTNO]
        mov     byte ptr es:[rm_intop + 1],al
        mov     ax,word ptr ds:[RMR_ES]
        mov     word ptr es:[t_es],ax
        mov     ax,word ptr ds:[RMR_DS]
        mov     word ptr es:[t_ds],ax
        pop     es

        mov     edi,dword ptr ds:[RMR_EDI]
        mov     esi,dword ptr ds:[RMR_ESI]
        mov     ebp,dword ptr ds:[RMR_EBP]
        mov     ebx,dword ptr ds:[RMR_EBX]
        mov     edx,dword ptr ds:[RMR_EDX]
        mov     ecx,dword ptr ds:[RMR_ECX]
        mov     eax,dword ptr ds:[RMR_EAX]
        mov     ds,word ptr cs:[t_ds]
        mov     es,word ptr cs:[t_es]
        sti
rm_intop:
        int     0                       ; <-- opcode patched above
        cli

        ; Capture the results using CS overrides, since DS/ES now hold
        ; whatever the BIOS left behind.
        mov     dword ptr cs:[t_eax],eax
        mov     word ptr cs:[t_es],es
        mov     word ptr cs:[t_ds],ds
        pushf
        pop     ax
        mov     word ptr cs:[t_flg],ax

        xor     ax,ax
        mov     ds,ax
        mov     dword ptr ds:[RMR_EDI],edi
        mov     dword ptr ds:[RMR_ESI],esi
        mov     dword ptr ds:[RMR_EBP],ebp
        mov     dword ptr ds:[RMR_EBX],ebx
        mov     dword ptr ds:[RMR_EDX],edx
        mov     dword ptr ds:[RMR_ECX],ecx
        mov     eax,dword ptr cs:[t_eax]
        mov     dword ptr ds:[RMR_EAX],eax
        mov     ax,word ptr cs:[t_flg]
        movzx   eax,ax
        mov     dword ptr ds:[RMR_FLAGS],eax
        mov     ax,word ptr cs:[t_es]
        mov     word ptr ds:[RMR_ES],ax
        mov     ax,word ptr cs:[t_ds]
        mov     word ptr ds:[RMR_DS],ax

        ; Back into protected mode
        cli
        call    pic_to_pm               ; IRQs back out of the exception range
        lgdt    fword ptr cs:[gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax

        db      0EAh                    ; JMP FAR SEL_CODE16:back16
        dw      OFFSET back16
        dw      SEL_CODE16

back16:
        mov     ax,SEL_DATA16           ; base = IO_BASE, reaches our vars
        mov     ds,ax

        ; And paging back on, if it was on when we came down here.  The
        ; reload of CR3 is not redundant - it flushes a TLB that has
        ; been living in real mode - and the jump after it is the
        ; prefetch flush the manual asks for.
        mov     eax,dword ptr ds:[GVO_PGDIR]
        or      eax,eax
        jz      no_paging
        mov     cr3,eax
        mov     eax,cr0
        or      eax,80000000h
        mov     cr0,eax
        jmp     pg_flush
pg_flush:
no_paging:
        lidt    fword ptr [idtr_pm]

        mov     ax,SEL_DATA32
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     ds,ax

        ; JMP FAR DWORD PTR ES:[RMRET]  -- 16:32 indirect, hand encoded
        ; 26      ES: segment override
        ; 66      operand size 32 (m16:32)
        ; FF /5   far jump indirect
        ; 2E      mod=00 reg=101 rm=110 -> disp16 follows
        db      26h,66h,0FFh,2Eh
        dw      RMRET

;=====================================================================
; pic_to_rm / pic_to_pm - shuffle the 8259 vector bases between the
; BIOS layout (08h/70h) and the protected-mode layout (20h/28h).
;
; Every real-mode BIOS call runs with the BIOS layout so the floppy
; driver sees its timer tick and IRQ 6 completion interrupt; protected
; mode needs the IRQs clear of the CPU exception vectors.
;
; pic_to_rm unmasks timer, keyboard, cascade, floppy and the RTC for
; the BIOS; pic_to_pm restores the kernel's own mask set.
;=====================================================================
pic_to_rm:
        mov     bh,08h                  ; master base
        mov     bl,70h                  ; slave base
        mov     ch,0B8h                 ; master mask: IRQ 0,1,2,6 live
        mov     cl,0FEh                 ; slave mask:  IRQ 8 (RTC) live
        jmp     short pic_prog
pic_to_pm:
        mov     bh,IRQ_BASE
        mov     bl,IRQ_BASE2
        mov     ch,0FCh                 ; timer and keyboard only
        mov     cl,0FFh
pic_prog:
        mov     al,11h                  ; ICW1
        out     PIC1_CMD,al
        jmp     short $+2
        out     PIC2_CMD,al
        jmp     short $+2
        mov     al,bh                   ; ICW2 - vector bases
        out     PIC1_DATA,al
        jmp     short $+2
        mov     al,bl
        out     PIC2_DATA,al
        jmp     short $+2
        mov     al,4                    ; ICW3
        out     PIC1_DATA,al
        jmp     short $+2
        mov     al,2
        out     PIC2_DATA,al
        jmp     short $+2
        mov     al,1                    ; ICW4
        out     PIC1_DATA,al
        jmp     short $+2
        out     PIC2_DATA,al
        jmp     short $+2
        mov     al,ch                   ; masks
        out     PIC1_DATA,al
        jmp     short $+2
        mov     al,cl
        out     PIC2_DATA,al
        ret

;=====================================================================
; rm_main - cold start.  CS = IO_SEG, DL = BIOS boot drive.
;=====================================================================
rm_main:
        cli
        mov     ax,cs
        mov     ds,ax
        xor     ax,ax
        mov     ss,ax
        mov     sp,RM_STACK_TOP
        sti

        mov     [v_bootdrv],dl          ; DS = CS, so this works wherever
                                        ; we happen to have been loaded

;--- get out of the transient program area ----------------------------
; Everything above this point is position independent - a near jump,
; CS-relative data, nothing else - because we are still running at
; IO_LOAD_SEG, where the boot sector dropped us, and the code was
; assembled for IO_SEG.  Copy ourselves down and jump to the copy.
;
; A hundred kilobytes of conventional memory rides on these fifteen
; instructions: staying at 20000h would put the stub squarely in the
; middle of what a DOS program wants to be one contiguous block.
        mov     bx,cs
        mov     ax,IO_SEG
        cmp     ax,bx
        je      rm_placed               ; already there - nothing to do
        mov     es,ax
        xor     si,si
        xor     di,di
        mov     cx,RMSTUB_SIZE / 2
        cld
        rep     movsw
        db      0EAh                    ; JMP FAR IO_SEG:rm_placed
        dw      OFFSET rm_placed
        dw      IO_SEG
rm_placed:
        mov     ax,cs                   ; DS still names the old copy
        mov     ds,ax

        ; A release build says NOTHING in real mode.  The stub's whole
        ; job is to get to protected mode, and the message that matters
        ; is the one printed on the other side, on a screen con_init
        ; has just cleared - so anything said here is a line the user
        ; watches scroll away.  The fatal paths below still speak: a
        ; machine that never reaches protected mode has no other voice.
IFDEF PMDOS_DEBUG
        mov     si,OFFSET msgBanner
        call    rputs
ENDIF

;--- refuse to run on anything below a 386 ---------------------------
        call    check386
        jnc     cpu_ok
        mov     si,OFFSET msg386
        jmp     rm_fatal
cpu_ok:

;--- A20 --------------------------------------------------------------
        call    enable_a20
        jnc     a20_ok
        mov     si,OFFSET msgA20
        jmp     rm_fatal
a20_ok:

;--- memory sizing ----------------------------------------------------
        call    get_e820
        call    get_extmem

;--- unreal mode, so we can write above 1M while still in real mode ---
        call    go_unreal

;--- and move our own protected-mode half up there --------------------
; It arrived directly behind us at IO_PM_LOAD because that is how the
; boot sector reads a file.  From here on it lives at IO_PM_BASE, and
; the far JMP at the end of this routine goes there.
;
; Source and destination both go through FS: unreal mode widened that
; one descriptor and no other, so DS still cannot reach past 64K and
; a plain "mov eax,[esi]" with ESI = 22000h would read the wrong
; sixteen bytes rather than fault - the worst possible failure.
;
; Blitting a fixed IO_PM_MAX rather than the real length copies some
; trailing rubbish, which costs nothing; the alternative is teaching
; two boot sectors to pass a file size down.
        mov     esi,IO_PM_LOAD
        mov     edi,IO_PM_BASE
        mov     cx,IO_PM_MAX / 4
rm_pmblit:
        mov     eax,fs:[esi]
        mov     fs:[edi],eax
        add     esi,4
        add     edi,4
        loop    rm_pmblit

;--- load PMDOS.SYS to DOS_BASE --------------------------------------
IFDEF PMDOS_DEBUG
        mov     si,OFFSET msgLoadDos
        call    rputs
ENDIF
        call    load_dos
IFDEF PMDOS_DEBUG
        mov     si,OFFSET msgOK
        call    rputs
ENDIF

;--- into protected mode for good -------------------------------------
        cli
        lgdt    fword ptr [gdtr]
        mov     eax,cr0
        or      eax,1
        mov     cr0,eax

        db      66h,0EAh                ; JMP FAR SEL_CODE32:IO_PM_BASE
        dd      IO_PM_BASE
        dw      SEL_CODE32

;=====================================================================
; check386 - CF set if this is a 286 or older.
;
; On an 8086 bits 12-15 of FLAGS are always set; on a 286 in real mode
; they are always clear.  Only a 386+ lets us change them.
;=====================================================================
check386:
        pushf
        pop     ax
        mov     cx,ax
        and     ax,0FFFh
        push    ax
        popf
        pushf
        pop     ax
        and     ax,0F000h
        cmp     ax,0F000h
        je      c386_no                 ; stuck set -> 8086/8088
        or      cx,0F000h
        push    cx
        popf
        pushf
        pop     ax
        and     ax,0F000h
        jz      c386_no                 ; stuck clear -> 286
        clc
        ret
c386_no:
        stc
        ret

;=====================================================================
; enable_a20 - try the fast gate first, then the keyboard controller.
;              CF set if A20 could not be enabled.
;=====================================================================
enable_a20:
        call    test_a20
        jnc     a20_done

        in      al,92h                  ; PS/2 fast A20
        or      al,2
        and     al,0FEh                 ; never touch the reset bit
        out     92h,al
        call    test_a20
        jnc     a20_done

        call    kbc_wait                ; 8042 route
        mov     al,0D1h
        out     64h,al
        call    kbc_wait
        mov     al,0DFh
        out     60h,al
        call    kbc_wait
        call    test_a20
a20_done:
        ret

kbc_wait:
        push    cx
        mov     cx,0FFFFh
kbcw1:
        in      al,64h
        test    al,2
        jz      kbcw2
        loop    kbcw1
kbcw2:
        pop     cx
        ret

;---------------------------------------------------------------------
; test_a20 - CF clear if the 1M wrap is gone.  Compares 0000:0500
;            against FFFF:0510 (which alias when A20 is off).
;---------------------------------------------------------------------
test_a20:
        push    ds
        push    es
        push    ax
        push    bx
        push    di

        xor     ax,ax
        mov     ds,ax
        mov     bx,0500h                ; 0000:0500
        mov     ax,0FFFFh
        mov     es,ax
        mov     di,0510h                ; FFFF:0510 - the same byte if
                                        ; the 1M wrap is still in effect
        mov     ax,[bx]                 ; stash both words
        push    ax
        mov     ax,es:[di]
        push    ax

        mov     word ptr [bx],1234h
        mov     word ptr es:[di],4321h
        mov     ax,[bx]                 ; AX = probe result

        pop     bx                      ; put the originals back
        mov     es:[di],bx
        pop     bx
        push    ax
        mov     ax,bx
        mov     bx,0500h
        mov     [bx],ax
        pop     ax

        cmp     ax,4321h                ; did the high write alias down?
        pop     di
        pop     bx
        pop     ax                      ; POP leaves the flags alone
        pop     es
        pop     ds
        je      ta20_off
        clc
        ret
ta20_off:
        stc
        ret

;=====================================================================
; get_e820 - collect the INT 15h/E820 memory map at linear MEMMAP.
;=====================================================================
get_e820:
        push    es
        xor     ax,ax
        mov     es,ax
        mov     di,MEMMAP
        xor     ebx,ebx
        mov     dword ptr [v_e820cnt],0
e820lp:
        mov     eax,0E820h
        mov     edx,534D4150h           ; 'SMAP'
        mov     ecx,24
        int     15h
        jc      e820end
        cmp     eax,534D4150h
        jne     e820end
        or      ecx,ecx
        jz      e820nxt
        inc     dword ptr [v_e820cnt]
        add     di,24
e820nxt:
        cmp     dword ptr [v_e820cnt],MEMMAP_MAX
        jae     e820end
        or      ebx,ebx
        jnz     e820lp
e820end:
        pop     es
        ret

;=====================================================================
; get_extmem - extended memory size in KB into v_memkb, and the
;              usable top of memory into v_memtop.
;=====================================================================
get_extmem:
        push    es
        xor     ecx,ecx
        xor     edx,edx
        mov     ax,0E801h
        int     15h
        jc      ext_88
        ; some BIOSes answer in CX/DX rather than AX/BX
        jcxz    ext_ax
        mov     ax,cx
        mov     bx,dx
ext_ax:
        movzx   eax,ax                  ; KB between 1M and 16M
        movzx   ebx,bx                  ; 64K blocks above 16M
        shl     ebx,6                   ; -> KB
        add     eax,ebx
        jmp     short ext_store

ext_88:
        mov     ah,88h                  ; ancient fallback, caps at 64M
        int     15h
        jc      ext_none
        movzx   eax,ax
        jmp     short ext_store
ext_none:
        xor     eax,eax

ext_store:
        mov     [v_memkb],eax
        shl     eax,10                  ; KB -> bytes
        ; HMA_BASE, NOT DOS_BASE.  Extended memory starts at 1MB, and
        ; this said DOS_BASE only because the kernel used to be loaded
        ; there - the two constants had the same value and one of them
        ; was standing in for the other.  Moving the kernel up 64K to
        ; free the HMA made v_memtop 64K too high, which put the top of
        ; the arena past the end of RAM: the end-of-arena header was
        ; written into nothing, read back as rubbish, and every walk of
        ; the chain ran off the end.  Harmless-looking until paging
        ; arrived, at which point running off the end is a page fault.
        add     eax,HMA_BASE
        mov     [v_memtop],eax
        pop     es
        ret

;=====================================================================
; go_unreal - give FS a 4G limit so real-mode code can store above 1M.
;=====================================================================
go_unreal:
        push    eax
        cli
        lgdt    fword ptr [gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short gu1               ; flush the prefetch queue
gu1:
        mov     ax,SEL_DATA32
        mov     fs,ax                   ; descriptor cache keeps limit 4G
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short gu2
gu2:
        sti
        pop     eax
        ret

;=====================================================================
; load_dos - find PMDOS.SYS in the boot volume's root directory and
;            copy it to DOS_BASE, following its cluster chain.
;
; The chain walking, the FAT12/16/32 arithmetic and the BIOS reads all
; come from FATLOAD.INC, the same code the hard-disk stage 2 uses to
; load us.  Nothing on any volume has to be contiguous.
;
; Clusters are staged through XFER_BUF (below 1M, where the BIOS can
; reach) and then blitted above 1M through the unreal-mode FS
; descriptor, which has to be re-armed after every INT 13h because
; some BIOSes restore FS from the real-mode value on the way out.
;=====================================================================
load_dos:
        push    es

        mov     al,[v_bootdrv]
        mov     [fl_drive],al
        call    fl_init

        mov     si,OFFSET dosname
        call    fl_find
        jc      ld_missing

        mov     eax,[fl_size]
        mov     [v_dossize],eax

        mov     edi,DOS_BASE            ; FS-relative destination
        mov     eax,[fl_clus]
ld_loop:
        cmp     eax,2
        jb      ld_done
        cmp     eax,[fl_eoc]
        jae     ld_done

        push    eax
        call    fl_c2l                  ; EAX = the cluster's first sector
        mov     [ld_lba],eax
        mov     eax,[fl_spc]
        mov     [ld_left],eax

;--- a cluster AT A TIME WAS THE BUG ---------------------------------
; This read fl_spc sectors into XFER_BUF, which is XFER_SIZE - 8K -
; and said nothing about what happens when a cluster is bigger than
; that.  Nothing did, for eight releases, because MKDISK could only
; build 1K-cluster images and every test disk had one.  A volume with
; 16K clusters (which is what FORMAT lays down on anything over about
; 512MB) overran the staging buffer by 8K into the memory just above
; it, and PM-DOS booted off a kernel image that had been written over
; while it was being loaded: exception 0E at DOS_BASE+15, every time.
;
; The cluster is now moved in XFER_SIZE-sized pieces, so the buffer
; bounds the transfer rather than the cluster size doing it - the same
; rule bio_loop in BLOCK.INC already followed.
;---------------------------------------------------------------------
ld_chunk:
        mov     ecx,[ld_left]
        cmp     ecx,XFER_SIZE / 512
        jbe     ld_cap
        mov     ecx,XFER_SIZE / 512
ld_cap:
        mov     [ld_this],ecx
        mov     eax,[ld_lba]
        push    es
        mov     bx,XFER_SEG             ; stage it below 1M
        mov     es,bx
        xor     bx,bx
        call    fl_read
        pop     es
        jc      ld_diskerr

        call    go_unreal               ; INT 13h may have cost us FS

        mov     ecx,[ld_this]           ; blit the piece past 1M
        shl     ecx,7                   ; sectors * 512 / 4 = dwords
        push    ds
        mov     ax,XFER_SEG
        mov     ds,ax
        xor     si,si
ld_blit:
        lodsd
        mov     fs:[edi],eax
        add     edi,4
        loop    ld_blit
        pop     ds

        mov     eax,[ld_this]
        add     [ld_lba],eax
        sub     [ld_left],eax
        jnz     ld_chunk

IFDEF PMDOS_DEBUG
        mov     al,'.'                  ; crude progress indicator
        call    rputc
ENDIF
        pop     eax
        call    fl_next
        jmp     short ld_loop

ld_done:
        pop     es
        ret

ld_diskerr:
        pop     eax
        mov     si,OFFSET msgDisk
        jmp     rm_fatal
ld_missing:
        mov     si,OFFSET msgNoDos
        jmp     rm_fatal

;=====================================================================
        INCLUDE FATLOAD.INC

;=====================================================================
; Console helpers (BIOS teletype - real mode only)
;=====================================================================
rputs:
        push    ax
        push    si
rputs1:
        lodsb
        or      al,al
        jz      rputs2
        call    rputc
        jmp     short rputs1
rputs2:
        pop     si
        pop     ax
        ret

rputc:
        push    ax
        push    bx
        mov     ah,0Eh
        mov     bx,0007h
        int     10h
        pop     bx
        pop     ax
        ret

rm_fatal:
        call    rputs
        mov     si,OFFSET msgHalt
        call    rputs
rmf1:
        hlt
        jmp     short rmf1

;=====================================================================
; Messages and scratch
;=====================================================================
IFDEF PMDOS_DEBUG
; No version number here: this file cannot see PMDOS_MINOR as text, so
; a literal one only ever goes stale (it sat at v0.11 through four
; releases).  The kernel banner carries the version; this says who is
; speaking.
msgBanner   db  13,10,'PM-DOS PMIO.SYS',13,10,0
ENDIF
msg386      db  'A 386 or better is required',0
msgA20      db  'Cannot enable A20',0
msgNoDos    db  'PMDOS.SYS not found',0
msgDisk     db  'Disk error',0
IFDEF PMDOS_DEBUG
msgLoadDos  db  'Loading PMDOS.SYS',0
msgOK       db  ' ok',13,10,0
ENDIF
msgHalt     db  13,10,'System halted.',13,10,0

dosname     db  'PMDOS   SYS'   ; 8.3 padded to exactly 11 bytes

t_es        dw  0                       ; thunk register staging
t_ds        dw  0
t_flg       dw  0
t_eax       dd  0

ld_lba      dd  0                       ; load_dos: the piece being moved
ld_left     dd  0                       ; sectors of this cluster to go
ld_this     dd  0                       ; sectors in the piece in hand

; (the loader's own state lives in FATLOAD.INC)

;=====================================================================
; GDT.  Lives inside the resident stub so it stays valid forever.
;=====================================================================
        ORG     GDT_OFF
gdt_start:
        ; 0000 - null
        dq      0

        ; 0008 - SEL_CODE32: base 0, limit 4G, 32-bit, execute/read
        dw      0FFFFh, 0000h
        db      00h, 9Ah, 0CFh, 00h

        ; 0010 - SEL_DATA32: base 0, limit 4G, 32-bit, read/write
        dw      0FFFFh, 0000h
        db      00h, 92h, 0CFh, 00h

        ; 0018 - SEL_CODE16: base IO_BASE, limit 64K, 16-bit, exec/read
        dw      0FFFFh, (IO_BASE AND 0FFFFh)
        db      ((IO_BASE SHR 16) AND 0FFh), 9Ah, 00h, ((IO_BASE SHR 24) AND 0FFh)

        ; 0020 - SEL_DATA16: base IO_BASE, limit 64K, 16-bit, read/write
        dw      0FFFFh, (IO_BASE AND 0FFFFh)
        db      ((IO_BASE SHR 16) AND 0FFh), 92h, 00h, ((IO_BASE SHR 24) AND 0FFh)

        ; 0028 - SEL_TSS: available 386 TSS.  Byte granular, because the
        ; I/O permission bitmap makes it 8300 bytes and a 4K-granular
        ; limit could not describe that.  IOPM fills it in and LTRs it.
        dw      (TSS_LIMIT AND 0FFFFh), (TSS_BASE AND 0FFFFh)
        db      ((TSS_BASE SHR 16) AND 0FFh), 89h, ((TSS_LIMIT SHR 16) AND 0Fh), ((TSS_BASE SHR 24) AND 0FFh)

        ; 0030 - SEL_LDT: the DPMI client's local descriptor table.
        ; Present here rather than built at run time because the GDT
        ; is in the stub and the stub is read-only once PM starts -
        ; the TABLE it points at is what gets filled in, per client.
        dw      (DPMI_LDT_LIMIT AND 0FFFFh), (DPMI_LDT AND 0FFFFh)
        db      ((DPMI_LDT SHR 16) AND 0FFh), 82h, ((DPMI_LDT_LIMIT SHR 16) AND 0Fh), ((DPMI_LDT SHR 24) AND 0FFh)

        ; 0038 - SEL_PROG_CS and 0040 - SEL_PROG_DS: the running native
        ; program's code and data, DPL 3, based at its slot with a limit
        ; of its size.  They start EMPTY and not present, because until
        ; a program has been loaded there is no slot to describe and a
        ; plausible-looking descriptor over somebody else's memory is
        ; the wrong kind of default.  r3_setdesc fills them in.
        dq      0
        dq      0
gdt_end:

gdtr:
        dw      GDT_ENTRIES * 8 - 1
        dd      GDT_BASE

idtr_pm:                                ; the 32-bit IDT built by IOPM
        dw      IDT_LIMIT
        dd      IDT_BASE

rm_idtr:                                ; the BIOS interrupt vector table
        dw      03FFh
        dd      0

;=====================================================================
; Globals, at a fixed offset so protected-mode code can find them.
;=====================================================================
        ORG     GV_OFF
v_bootdrv  db  0
            db  0,0,0
v_dossize  dd  0
v_memkb    dd  0
v_e820cnt  dd  0
v_memtop   dd  0

;=====================================================================
; Pad the stub to exactly RMSTUB_SIZE so IOPM.BIN lands at IO_PM_BASE.
;=====================================================================
        ORG     RMSTUB_SIZE - 1
        db      0

_RM     ENDS
        END
