;=====================================================================
; DOSMAIN.ASM - PMDOS.SYS, the 32-bit DOS kernel
;
; Loaded whole at DOS_BASE by PMIO's real-mode stub and entered flat
; at its first byte with interrupts running and the PMIO service
; table live.  Provides:
;
;   - a first-fit memory arena above ARENA_BASE
;   - a read-only FAT12 filesystem on the boot drive
;   - an INT 21h API (see INT21.INC for the register conventions)
;   - a built-in command shell, pending a real COMMAND.COM
;
; Everything PMDOS knows about the hardware comes through the PMIO
; service table at IOSVC - it never touches ports or the BIOS itself.
;
; Linked with:  wlink form raw bin option offset=0x100000
;=====================================================================
        .386P

        INCLUDE PMDOS.INC

_DOS    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_DOS, DS:_DOS, ES:_DOS, SS:_DOS
        ORG     0

;=====================================================================
; Entry - first instruction of PMDOS.SYS
;=====================================================================
dos_entry:
IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgBanner
        call    dword ptr ds:[IOS_CONSTR]
ENDIF

        ; The two DOS status flags, before anything can raise either.
        ; They live in the BIOS data area, which the BIOS left holding
        ; whatever it liked, and CONFIG.SYS makes DOS calls long before
        ; the shell exists.
        mov     byte ptr ds:[DOS_INDOS],0
        mov     byte ptr ds:[DOS_ERRFLG],0

        call    arena_init
        call    fat_init                ; per-drive state, out of the arena
        call    lm_init                 ; the DOS arena below 1MB
        call    xms_init                ; and the XMS callback in it
        call    vdm_stubs               ; and an IRET for the DOS vectors
                                        ; a DOS box reflects through
        call    vdm_lolinit             ; and a 52h list of lists a 16-bit
                                        ; program can actually follow
        call    i13_count_drives        ; what the BIOS counted at power-on
        call    dpmi_init               ; and the protected-mode interface
        call    sft_init

        ; Tell PMIO where to go when a ring-0 fault has been reported.
        ; THAT NOW MEANS THE KERNEL'S OWN BUG AND NOTHING ELSE: native
        ; programs moved to ring 3 in this release, so their faults
        ; arrive at pm3_fault with the machine still intact, and this
        ; hook is left for the case it was always really about - a
        ; fault in PM-DOS itself.  It still unwinds rather than halting,
        ; because a development machine that comes back to a prompt is
        ; worth more than one that does not.
        mov     edi,OFFSET dos_fault
        call    dword ptr ds:[IOS_EXCHOOK]

        ; And where to go when RING-3 code faults: a DPMI client or a
        ; native program, which pm3_dispatch tells apart.
        mov     edi,OFFSET pm3_dispatch
        call    dword ptr ds:[IOS_PM3HOOK]

;=====================================================================
; The DOS vectors, BEFORE the first sector is read.  Mounting the boot
; volume can fail, and failing at a disk is now a call to INT 24h -
; which before this point is PMIO's do-nothing gate, an IRETD that
; returns whatever happened to be in AL.  Nought is "ignore", so the
; kernel would have carried on with an unread sector.  Anything that
; touches a disk belongs after this block.
;=====================================================================
        mov     al,20h                  ; classic terminate vector
        mov     edi,OFFSET int20_entry
        call    dword ptr ds:[IOS_SETVEC]
        mov     al,21h                  ; and the API itself
        mov     edi,OFFSET int21_entry
        call    dword ptr ds:[IOS_SETVEC]
        mov     al,22h                  ; terminate address: ending up here
        mov     edi,OFFSET int20_entry   ; ends the program, which is what
        call    dword ptr ds:[IOS_SETVEC]       ; 22h has always meant
        mov     al,23h                  ; CTRL-C: terminate by default
        mov     edi,OFFSET int23_default
        call    dword ptr ds:[IOS_SETVEC]
        mov     al,24h                  ; critical error: fail, until
                                        ; something installs a prompt
        mov     edi,OFFSET int24_default
        call    dword ptr ds:[IOS_SETVEC]
        mov     al,25h                  ; absolute disk read...
        mov     edi,OFFSET int25_entry
        call    dword ptr ds:[IOS_SETVEC]
        mov     al,26h                  ; ...and write
        mov     edi,OFFSET int26_entry
        call    dword ptr ds:[IOS_SETVEC]

        ; Every volume on every disk, before any of them is mounted:
        ; a drive letter is an index into that table now and not a sum
        ; on a BIOS drive number.
        call    disk_scan

        mov     al,byte ptr ds:[GV_BOOTDRV]
        call    bios2log                ; the letter the boot disk got
        mov     [cur_vol],al            ; the boot volume owns vol_state
        mov     [cur_drive],al
        add     al,'A'                  ; the shell reloads off the boot
        mov     [shell_name],al         ; drive no matter where cwd sits
        call    fat_mount
        jnc     mount_ok
        mov     esi,OFFSET msgNoMount
        call    dword ptr ds:[IOS_CONSTR]
        jmp     short no_fs
mount_ok:
IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgMounted
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET msgFat12
        cmp     byte ptr [fat_type],12
        je      mount_say
        mov     esi,OFFSET msgFat16
        cmp     byte ptr [fat_type],16
        je      mount_say
        mov     esi,OFFSET msgFat32
mount_say:
        call    dword ptr ds:[IOS_CONSTR]
ENDIF
no_fs:

;=====================================================================
; CONFIG.SYS, once, before the interpreter exists - a SHELL= line
; chooses which interpreter that is, so nothing later could act on it.
; AUTOEXEC.BAT then follows from the shell itself, in that order.
;
; Deliberately ABOVE the shell_restart label: reloading the shell must
; not re-run the system configuration.
;=====================================================================
IFDEF VDM_TEST
        call    vdm_selftest
ENDIF
        call    config_run

;=====================================================================
; shell_restart - (re)load the command interpreter into the shell slot
; and run it.  fn4C jumps back here whenever the shell terminates.
;=====================================================================
shell_restart:
        mov     esp,KSTACK_TOP          ; fresh kernel stack
        sti

        ; Nothing is in progress here by definition - this is the top
        ; of the machine - and every unwind that reaches this label
        ; threw away INT 21h frames whose decrements will now never
        ; run.  So the depth is not adjusted, it is ASSERTED.
        mov     byte ptr ds:[DOS_INDOS],0
        mov     byte ptr ds:[DOS_ERRFLG],0

        mov     dword ptr ds:[cur_psp],SHELL_PSP
        xor     eax,eax                 ; depth zero: the interpreter's own
        call    r3_useslot              ; slot, and its two descriptors
        mov     edx,OFFSET shell_name
        call    exec_load
        jc      no_shell

        ; "/P" marks the permanent shell, exactly as SHELL= does in a
        ; real CONFIG.SYS: it is what tells COMMAND.COM to run
        ; AUTOEXEC.BAT and to ignore EXIT.
        mov     esi,OFFSET shell_tail
        call    build_psp
        mov     dword ptr ds:[exec_depth],0

        ; A fresh shell gets a fresh identity.  Nothing else can own
        ; anything here - every program is gone, and 4Ch swept its
        ; handles and memory on the way out - so the counter restarts.
        mov     dword ptr ds:[cur_own],1
        mov     dword ptr ds:[next_own],2
        mov     dword ptr ds:[alloc_strat],0    ; and a fresh 58h strategy

        ; The interpreter is a program like any other and starts the
        ; way any other one does: at ring 3, in its own slot, with the
        ; PSP offset on its stack so that a RET lands on the INT 20h
        ; there and brings us back to this label.
        ;
        ; It is the one program that starts with the whole of the
        ; ring-0 stack in front of it, because nothing is waiting
        ; underneath it - that is what makes it the shell.  Every other
        ; program inherits a level from the 4Bh that started it.
        mov     dword ptr ds:[r3_esp0],R3_KSTACK
        mov     eax,dword ptr ds:[exec_entry]
        jmp     r3_start

no_shell:
        mov     esi,OFFSET msgNoShell
        call    dword ptr ds:[IOS_CONSTR]
        call    dword ptr ds:[IOS_CONIN]        ; any key ...
        jmp     shell_restart                   ; ... and try again

;=====================================================================
; dos_fault - IOS_EXCHOOK.  A ring-0 fault has happened and PMIO has
;             already said what it was.  Get back to a prompt.
;
; This is the recovery a native 32-bit program never had.  It cannot
; be gentle: the faulting code was in the kernel's own segment, we are
; on whatever stack it faulted on, and the fault frame is still up
; there.  So the machine state that says "a program is running" is
; asserted rather than unwound, and then the program is ENDED THROUGH
; THE ORDINARY EXIT.
;
; That last part is what makes it worth doing properly.  Jumping
; straight to shell_restart also gets a prompt back, but it reloads
; the interpreter - which re-runs AUTOEXEC.BAT and loses whatever
; batch file was in progress, so a crash in one utility restarts the
; session.  4Ch already knows how to hand a slot back to the program
; that is waiting in its EXEC, and that program is the shell.  So a
; faulting program terminates the way any other program terminates,
; with errorlevel 255, and the batch file it was called from carries
; on to the next line.
;
; A fault in the SHELL ITSELF has no waiting parent, and 4Ch already
; does the right thing with that: exec_depth is zero, so it takes the
; f4C_shell path and reloads the interpreter after all.
;
; What this does NOT do is pretend the file system is unharmed.  A
; fault in the middle of a directory update leaves what it leaves.
; The dying program's handles are left for 4Ch to close, because that
; is where a half-written file gets its length and its directory entry
; - but a fault inside the FAT code can perfectly well make that close
; fail or write nonsense, and there is no way from here to tell.
;=====================================================================
dos_fault:
        cli
        mov     ax,SEL_DATA32           ; the fault may have come from
        mov     ds,ax                   ; anywhere at all
        mov     es,ax
        mov     fs,ax
        mov     gs,ax
        mov     ss,ax
        mov     esp,KSTACK_TOP
        cld

        ; No DPMI client, no critical error, no call in progress.
        ; Every one of these is a flag that some unwind would normally
        ; have cleared on its way out, and any of them left set would
        ; make the exit below take a different path.  exec_depth is
        ; deliberately NOT touched: it is the count 4Ch needs to find
        ; the waiting program; neither is vdm_busy, for the reason at
        ; the bottom of this routine.
        mov     dword ptr ds:[dpmi_active],0
        mov     dword ptr ds:[dpmi_rmact],0
        mov     byte ptr ds:[crit_busy],0
        mov     byte ptr ds:[DOS_ERRFLG],0
        mov     byte ptr ds:[DOS_INDOS],0
        xor     eax,eax
        lldt    ax                      ; a client's LDT is not a thing now

        ; ...and no handler of the program's is running either, which
        ; is the same kind of fact one level up: r3_thunk parks a frame
        ; for the IRET that ends a ring-3 handler, and a fault takes
        ; that IRET away.  The ring-0 stack pointer goes back to the
        ; top with it.
        call    r3_abandon

        ; THE GUARD MAY HAVE BEEN STOOD DOWN WHEN THE FAULT ARRIVED.
        ; v86_run, the real-mode thunk and ivt_set all disarm it and
        ; re-arm on the way out, and a fault takes the way out away -
        ; so without this, one recovered crash inside a DOS box turns
        ; the null guard off for the rest of the session, silently.
        mov     al,1
        call    dword ptr ds:[IOS_NULLTRAP]

        ; A RAW CONSOLE OUTLIVES THE PROGRAM THAT ASKED FOR IT.  4401h
        ; sets bit 5 on the SFT, and the SFT for handle 0 belongs to
        ; the shell, not to the program that borrowed it - so a
        ; full-screen program killed between "raw on" and "raw off"
        ; leaves COMMAND.COM reading unedited single characters
        ; forever.  The whole point of this recovery is to buy back a
        ; usable prompt, and a console nobody can type at is not one.
        ; Every device handle goes back to cooked; a file has no such
        ; flag to clear, so testing SFTF_DEVICE would only cost time.
        mov     ebx,OFFSET sft_tbl
        mov     ecx,MAX_HANDLES
df_cook:
        and     dword ptr [ebx+SFT_FLAGS],NOT SFTF_RAW
        add     ebx,SFT_LEN
        loop    df_cook

        call    dword ptr ds:[IOS_EXCCLEAR]     ; the next one is catchable

        ; WITH A DOS BOX RUNNING, THE THING THAT DIED IS THE BOX.  A
        ; 16-bit EXEC does not nest: the guest lives below 1MB, nothing
        ; above it is disturbed, and vdm_exec RETURNS to the 4Bh that
        ; called it - so that path pushes no nesting record and leaves
        ; exec_depth alone.  4Ch would therefore find a depth of zero,
        ; conclude that the INTERPRETER was what faulted, and reload it
        ; - AUTOEXEC.BAT round again, and whatever batch file was
        ; running gone, because one 16-bit utility crashed.  That is
        ; the complaint v0.30 fixed for a 32-bit program without
        ; noticing that a DOS box is the other half of it.
        ;
        ; vdm_abort is the unwind built for exactly this shape: it is
        ; what a ^C and an Abort at the critical-error prompt already
        ; use, and it does not care what stack it is standing on - it
        ; longjmps to the one vdm_run started with, sweeps the guest's
        ; handles and memory, and lets the shell's 4Bh return with the
        ; failure.  4Dh will say the box ended on a system error, which
        ; is the nearest true thing DOS has a code for: there is no
        ; "the program faulted", because under real DOS the machine
        ; simply stopped.
        cmp     dword ptr ds:[vdm_busy],0
        je      df_notbox
        mov     eax,VDX_CRITERR
        jmp     vdm_abortc

df_notbox:
        ; 4Ch reads the exit code out of a frame, and the frame this
        ; fault came in on is not one - so it gets a manufactured one
        ; with 255 in it.  Nothing else in 4Ch looks at the frame until
        ; it has replaced ESP with the waiting program's own.
        mov     ebp,OFFSET fault_frame
        mov     dword ptr [ebp+F_EAX],255
        jmp     fn4C

;=====================================================================
        INCLUDE ARENA.INC
        INCLUDE FAT.INC
        INCLUDE CRITERR.INC
        INCLUDE PATH.INC
        INCLUDE INT21.INC
        INCLUDE CONFIG.INC
        INCLUDE LOWMEM.INC
        INCLUDE XMS.INC
        INCLUDE VDM.INC
        INCLUDE INT13.INC               ; after VDM.INC: it works on vdm_ctx
                                        ; and ends its calls at vdm_loop
        INCLUDE DPMI.INC                ; and after both: the DPMI host
                                        ; borrows the DOS box's own V86 loop
        INCLUDE RING3.INC               ; last: it sorts ring-3 faults into
                                        ; the DPMI host's and its own, and
                                        ; borrows the bridge's pointer table

;=====================================================================
; Data
;=====================================================================
cur_drive   db  0                       ; logical drive (0 = A:) - a LETTER, not a disk

; The frame dos_fault hands 4Ch.  A whole one, not just the dword 4Ch
; reads out of it, because the alternative is a routine that works
; until somebody adds a second field to the top of 4Ch.
fault_frame db  F_ESP3 + 8 dup(0)

; Both are buffers, not constants: a CONFIG.SYS SHELL= line replaces
; the interpreter AND its tail, and "/P" being absent is a meaningful
; choice rather than an oversight - it is how you ask for a shell that
; skips AUTOEXEC.BAT and that EXIT can terminate.
shell_name  db  'A:\COMMAND.COM',0      ; drive letter patched at boot
            db  SHELLNAME_MAX - 15 dup(0)
shell_tail  db  3,' /P',0Dh             ; DOS tail: length, text, CR
            db  SHELLTAIL_MAX - 5 dup(0)

IFDEF PMDOS_DEBUG
msgBanner   db  13,10
            db  'PMDOS.SYS - PM-DOS kernel version 0.29',13,10
            db  'Copyright (c) 2026',13,10,0
msgMounted  db  'Boot volume mounted: ',0
msgFat12    db  'FAT12',13,10,0
msgFat16    db  'FAT16',13,10,0
msgFat32    db  'FAT32',13,10,0
ENDIF
msgNoMount  db  'WARNING: cannot mount the boot volume - no filesystem',13,10,0
msgNoVolMem db  'no arena for the per-drive volume state',0
msgNoShell  db  13,10,'Bad or missing command interpreter'
            db  13,10,'Insert a system diskette and press any key',13,10,0

_DOS    ENDS
        END
