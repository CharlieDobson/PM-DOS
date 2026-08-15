;=====================================================================
; INT21.INC - the DOS API dispatcher and function handlers
;
; Register conventions are classic INT 21h with the obvious flat-mode
; readings: pointers that DOS took in DS:DX arrive in EDX as flat
; addresses, counts widen to ECX, and 21h/42h takes the whole seek
; offset in ECX and returns it in EAX.  Errors come back as CF set
; with the error code in AX, exactly as always.
;
; Functions with real-mode-segment semantics (26h PSP games, EXEC and
; friends) are not here yet.
;
; Included by DOSMAIN.ASM.
;=====================================================================

; Offsets into the saved register frame built by int21_entry.
F_EDI           EQU     0
F_ESI           EQU     4
F_EBP           EQU     8
F_ESP           EQU     12
F_EBX           EQU     16
F_EDX           EQU     20
F_ECX           EQU     24
F_EAX           EQU     28
F_ES            EQU     36
F_DS            EQU     40
F_EIP           EQU     44
F_CS            EQU     48
F_FLAGS         EQU     52

; And two more that only exist when the caller was at ring 3 - a
; DPMI client.  The CPU pushes its stack pointer and stack selector
; on the way in, because it had to change stacks to get here at all.
F_ESP3          EQU     56
F_SS3           EQU     60

FL_CF           EQU     1
FL_ZF           EQU     40h

; TWO TABLES ARE KEYED ON THIS, NOT ONE.  i21_table below and
; vdm_ptrtab in VDM.INC are both indexed by the function number and
; both bounded by this constant, so raising it without lengthening
; BOTH makes the shorter one read past its end and use whatever
; follows as a handler address or as pointer flags.  That is the bug
; the comment above vdm_ptrtab describes, from the other direction.
I21_MAXFN       EQU     6Dh             ; both tables cover 00h..6Ch

;---------------------------------------------------------------------
; EXEC nesting.  A D32 image is linked for one absolute address, so
; every transient program wants the SAME slot - there is no relocation
; to hand a second one somewhere else.  Nesting is therefore done by
; PARKING the occupant: its whole slot (PSP, image and stack) is
; copied into the arena, the newcomer is loaded over the top, and the
; occupant is put back when the newcomer exits.  One record per level
; describes the program that is waiting.
;---------------------------------------------------------------------
EXEC_MAX        EQU     8               ; nesting levels
EXEC_PATH_MAX   EQU     144
SLOT_SIZE       EQU     PROG_TOP - PROG_PSP     ; the whole transient slot

XN_EBP          EQU     0               ; dd - the waiting program's frame
XN_PSP          EQU     4               ; dd - and its PSP
XN_OWN          EQU     8               ; dd - and its owner id
XN_DTA          EQU     12              ; dd - and its DTA
XN_SAVE         EQU     16              ; dd - its parked slot, 0 = none
XN_INDOS        EQU     20              ; dd - and the InDOS depth it was at
XN_ESP0         EQU     24              ; dd - and the ring-0 stack it makes
                                        ;      its DOS calls on
XN_LEN          EQU     28

;=====================================================================
; int21_entry - every DOS call lands here through the IDT gate.
;=====================================================================
int21_entry:
        ; DS AND ES ARE THE CALLER'S, whoever the caller is: an
        ; interrupt gate does not reload them.  From ring 0 they are
        ; already flat and this costs two pushes; from a DPMI client
        ; they are the client's own selectors, and without this the
        ; first ds:[...] below would read the client's memory instead
        ; of the kernel's.
        ;
        ; THEY GO ON BEFORE THE FRAME POINTER, not after it, and that
        ; is not a matter of taste.  EXEC and the critical-error abort
        ; both come back to a waiting caller by restoring ESP from the
        ; frame it saved and leaving through i21_return - so anything
        ; pushed BELOW the frame is gone by then, and the pops at the
        ; other end would take the general registers instead.  Above
        ; the frame they survive every one of those paths.
        push    ds
        push    es
        push    ebp
        pushad
        mov     ebp,esp
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        cld

        ; DOS IS NOW BUSY, and 34h hands out the address of the byte
        ; that says so.  It counts rather than flags, because these
        ; calls genuinely nest: a critical-error handler and an INT 23h
        ; handler both run with their caller's INT 21h still open.
        inc     byte ptr ds:[DOS_INDOS]

        ; WHOSE CALL IS THIS?  Three kinds of caller reach this label
        ; and the CS says which.  The kernel's own is already flat and
        ; needs nothing.  A native program is ring-3 code in a segment
        ; based at its slot, so its pointers are offsets from that
        ; base.  Anything else at ring 3 is a DPMI client, whose
        ; pointers are selector:offset.
        cmp     word ptr [ebp+F_CS],SEL_CODE32
        je      i21_flat
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        je      i21_native

        cmp     byte ptr [ebp+F_EAX+1],4Ch      ; a client ending IS the DOS
        je      i21_dpmiexit                    ; box ending, and it unwinds
        call    dpmi_xlat                       ; rather than returns
        jc      i21_badsel
        jmp     short i21_flat

;--- a native program -------------------------------------------------
; Its stack pointer is remembered because it is the only place the
; kernel can learn it: a handler this program has hooked runs on that
; stack, below whatever it was using when it called in, exactly as DOS
; has always run one.  The CPU pushed it on the way here.
i21_native:
        mov     eax,[ebp+F_ESP3]
        mov     [r3_lastesp],eax

        ; The trampoline at PSP+54h, which is how a handler that has
        ; finished gets back.  Recognised by the address it calls from,
        ; because the registers are the handler's answer by now and a
        ; function number in AX would have destroyed it.
        cmp     dword ptr [ebp+F_EIP],R3RET_EIP
        je      r3_ret

        call    r3_xlat
        jc      i21_badptr
i21_flat:
        movzx   eax,byte ptr [ebp+F_EAX+1]      ; AH = function
        cmp     eax,71h                         ; long-file-name API
        je      i21_lfn
        cmp     eax,73h                         ; FAT32 extended calls
        je      i21_fat32
        cmp     eax,0F0h                        ; PM-DOS console extension
        je      i21_ext
        cmp     eax,0F1h                        ; PM-DOS kernel information
        je      i21_ext1
        cmp     eax,I21_MAXFN
        jae     i21_badfn
        call    dword ptr [i21_table+eax*4]

i21_return:
        cmp     word ptr [ebp+F_CS],SEL_CODE32
        je      i21_r0
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        je      i21_r3native
        call    dpmi_unxlat             ; the client's own registers back
        jmp     short i21_r0

;--- back into a native program.  The offsets it passed are put back,
;--- and the ring-0 stack pointer is restored to whatever depth the
;--- program is at: the top of the native stack normally, and the
;--- interrupted level's frame while one of its handlers is running.
i21_r3native:
        call    r3_unxlat
        mov     eax,[r3_esp0]
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],eax
i21_r0:
        ; The matching decrement goes HERE rather than at i21_return,
        ; because the bad-selector path skips straight to this label
        ; and would otherwise leave the count one too high for good.
        dec     byte ptr ds:[DOS_INDOS]
        popad
        pop     ebp
        pop     es
        pop     ds
        iretd

i21_badsel:
        mov     word ptr [ebp+F_EAX],ERR_BADDRIVE
        or      dword ptr [ebp+F_FLAGS],FL_CF
        jmp     short i21_r0

;--- a native program passed an offset that is not inside its own slot.
;--- The call has not happened and nothing is half done, so it is
;--- refused rather than being made a fault: a program that gets an
;--- error back can say which call failed, where a dead program cannot.
i21_badptr:
        mov     word ptr [ebp+F_EAX],ERR_ACCESS
        or      dword ptr [ebp+F_FLAGS],FL_CF
        jmp     short i21_r3native

i21_dpmiexit:
        mov     al,byte ptr [ebp+F_EAX]
        jmp     dpmi_exit

i21_lfn:
        call    fn71
        jmp     short i21_return

i21_fat32:
        call    fn73
        jmp     short i21_return

i21_ext:
        call    fnF0
        jmp     short i21_return

i21_ext1:
        call    fnF1
        jmp     short i21_return

i21_badfn:
        call    i21_unsup
        jmp     short i21_return

;=====================================================================
; INT 20h - classic terminate.  The PSP starts with this opcode so a
; plain RET out of a program still ends it.
;=====================================================================
int20_entry:
        push    ds                      ; the same frame int21_entry builds:
        push    es                      ; it leaves through i21_return
        push    ebp
        pushad
        mov     ebp,esp
        mov     ax,SEL_DATA32
        mov     ds,ax
        mov     es,ax
        cld
        inc     byte ptr ds:[DOS_INDOS] ; it leaves through i21_r0 too
        mov     byte ptr [ebp+F_EAX],0  ; exit code 0
        jmp     fn4C

;=====================================================================
; INT 25h / 26h - absolute disk read / write, the DOS raw-sector API.
;
;   AL = logical drive (0 = A:), EDX = first LOGICAL sector of the
;   volume (the partition offset is the kernel's business), ECX =
;   sector count, EBX -> buffer.  CF with AX = error.
;
; Flat deviations, both deliberate: the buffer rides in EBX (the
; flat reading of DS:BX), and the infamous flags-word-left-on-the-
; stack quirk is NOT reproduced - these return by plain IRET like
; every other interrupt here.  SYS and FORMAT are what these exist
; for; a program that can rewrite a boot sector has no need of a
; 45-year-old stack wart on top.
;=====================================================================
int25_entry:
        push    ds                      ; the frame shape is shared, so the
        push    es                      ; F_ pass offsets are too
        push    ebp
        pushad
        mov     ebp,esp
        cld
        call    disk_absio
        jc      i25_fail
        mov     eax,IOS_RDBLK
        call    disk_absgo
        jc      i25_fail
        and     dword ptr [ebp+F_FLAGS],NOT FL_CF
        jmp     short i25_out
i25_fail:
        mov     word ptr [ebp+F_EAX],ax
        or      dword ptr [ebp+F_FLAGS],FL_CF
i25_out:
        popad
        pop     ebp
        pop     es
        pop     ds
        iretd

int26_entry:
        push    ds
        push    es
        push    ebp
        pushad
        mov     ebp,esp
        cld
        call    disk_absio
        jc      i26_fail
        mov     eax,IOS_WRBLK
        call    disk_absgo
        jc      i26_fail
        and     dword ptr [ebp+F_FLAGS],NOT FL_CF
        jmp     short i26_out
i26_fail:
        mov     word ptr [ebp+F_EAX],ax
        or      dword ptr [ebp+F_FLAGS],FL_CF
i26_out:
        popad
        pop     ebp
        pop     es
        pop     ds
        iretd

;---------------------------------------------------------------------
; disk_absio - common validation: AL = logical drive is selected and
;              mounted, EDX + ECX stays inside the volume.  CF with
;              AX = error code.
;---------------------------------------------------------------------
disk_absio:
        mov     al,[ebp+F_EAX]
        cmp     al,MAX_DRIVES
        jae     dab_bad
        call    vol_select
        jc      dab_bad
        mov     eax,[ebp+F_EDX]         ; start + count within bounds
        add     eax,[ebp+F_ECX]
        jc      dab_range
        cmp     eax,[fat_totsec]
        ja      dab_range
        clc
        ret
dab_bad:
        mov     ax,ERR_BADDRIVE
        stc
        ret
dab_range:
        mov     ax,8                    ; classic "sector not found"
        stc
        ret

; disk_absgo - EAX = IOS_RDBLK or IOS_WRBLK; the frame supplies the
;              rest.  CF on I/O failure.
;
; 25h and 26h go round the filesystem by definition - that is what
; they are for - so the filesystem has to be told.  Everything held
; dirty goes out before the transfer, so that a 25h read sees what the
; kernel believes is on the disk; and after a 26h write the caches
; that described those sectors are dropped, because the sectors have
; just become the caller's version and not ours.  Without the second
; half, a program rewriting a directory sector through 26h could watch
; the kernel put the old one back the next time it flushed.
disk_absgo:
        push    eax
        mov     ebx,[ebp+F_EDX]         ; logical -> absolute LBA
        add     ebx,[fat_partlba]
        mov     ecx,[ebp+F_ECX]
        mov     edi,[ebp+F_EBX]         ; the caller's buffer
        mov     al,[fat_drive]
        pop     edx                     ; the service to call

        push    edx                     ; vol_syncdrv swaps volumes about
        push    edi                     ; and restores them; the transfer's
        push    ecx                     ; own arguments are already latched
        push    ebx
        push    eax
        mov     ah,0
        call    vol_syncdrv
        pop     eax
        pop     ebx
        pop     ecx
        pop     edi
        pop     edx

        push    edx
        push    ecx
        push    ebx
        push    eax
        call    dword ptr ds:[edx]
        pop     edx                     ; EDX = the drive, EAX = the status
        pop     ebx
        pop     ecx
        jc      dag_popfail
        pop     eax                     ; the service, again
        cmp     eax,IOS_WRBLK
        jne     dag_ok
        mov     al,dl
        mov     ah,1
        call    vol_syncdrv             ; those sectors are theirs now
dag_ok:
        clc
        ret
dag_popfail:
        pop     eax
dag_fail:
        mov     ax,0Bh                  ; classic "read/write fault"
        stc
        ret

;=====================================================================
; CTRL-C and INT 23h.
;
; A ^C arriving from the KEYBOARD during console input echoes "^C"
; and raises INT 23h.  The default handler below terminates the
; running program through 4Ch - which unwinds every nested frame by
; construction, exactly like any other exit.  A program (COMMAND.COM
; foremost) hooks vector 23h through the ordinary 25h call; a
; handler that IRETs back simply resumes the interrupted read, the
; ^C swallowed.  Input arriving from a FILE is never checked: a
; redirected stdin cannot press anything.
;=====================================================================
ctrl_break:
        push    eax
        mov     al,'^'
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,'C'
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,13
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,10
        call    dword ptr ds:[IOS_CONOUT]
        int     23h
        pop     eax
        ret

int23_default:
        mov     ax,4C00h                ; terminate the program; never
        int     21h                     ; returns - 4Ch owns the unwind

; INT 24h and everything that raises one live in CRITERR.INC, next to
; the sector I/O that finds the trouble.

;---------------------------------------------------------------------
; Outcome helpers.  Handlers end with one of these.
;---------------------------------------------------------------------
i21_ok:
        and     dword ptr [ebp+F_FLAGS],NOT FL_CF
        ret

i21_fail:                               ; AX = error code
        mov     word ptr [ebp+F_EAX],ax
        mov     word ptr [last_err],ax  ; and 59h can ask about it later
        or      dword ptr [ebp+F_FLAGS],FL_CF
        ret

i21_unsup:
        mov     ax,ERR_FUNC
        jmp     short i21_fail

;=====================================================================
; 00h - terminate (treated as 4Ch with code 0)
;=====================================================================
fn00:
        mov     byte ptr [ebp+F_EAX],0
        jmp     fn4C

;=====================================================================
; 01h - console input with echo -> AL.
;
; This and the other CP/M-era console functions go through HANDLES 0
; and 1 rather than straight to the screen and keyboard - that is
; what real DOS does, and it is the whole mechanism of redirection:
; COMMAND points handle 1 at a file and every 02h/09h in every
; program lands there without the program knowing.  A redirected
; stdin that runs out reads as ^Z forever, which is what programs
; historically test for.
;=====================================================================
fn01:
        call    std_in
        jc      f01_eof
        mov     [ebp+F_EAX],ax
        call    std_out                 ; echo follows stdout, so a
        jmp     i21_ok                  ; redirected stdout captures it
f01_eof:
        mov     word ptr [ebp+F_EAX],001Ah
        jmp     i21_ok

;=====================================================================
; 02h - console output, DL = char -> AL = char
;=====================================================================
fn02:
        mov     al,[ebp+F_EDX]
        call    std_out
        mov     [ebp+F_EAX],al
        jmp     i21_ok

;=====================================================================
; 03h / 04h / 05h - the serial port and the printer, a byte at a time.
;
; The three CP/M-era calls that do not go through a handle, and they
; are here because since v0.30 they cost nothing: dev_in and dev_out
; already talk to AUX and PRN, and these are the same two routines with
; the device decided by the function number instead of by a name.
;
; They deliberately do NOT go through the standard handles.  02h writes
; to handle 1 and can be redirected; 05h writes to the printer and
; cannot, which is exactly why a program that means the printer uses
; 05h.
;
; 03h WAITS, as DOS does, and on a machine with no serial port it waits
; for ever - which is authentic and is also why it idles rather than
; spins while it does it.  There is no ^C check in DOS's 03h either.
;=====================================================================
fn03:
        mov     edx,DEV_AUX
        call    dev_in
        jnc     f03_have
        call    dos_idle
        jmp     short fn03
f03_have:
        mov     [ebp+F_EAX],al
        jmp     i21_ok

fn04:
        mov     al,[ebp+F_EDX]
        mov     edx,DEV_AUX
        call    dev_out
        jmp     i21_ok

fn05:
        mov     al,[ebp+F_EDX]
        mov     edx,DEV_PRN
        call    dev_out
        jmp     i21_ok

;=====================================================================
; 2Eh / 54h - the verify flag: read every sector back after writing it.
;
; SET AND REPORTED, AND DELIBERATELY NOT ACTED ON, which is a thing
; worth being explicit about rather than quietly doing.  Verification
; belongs in the block layer, where a write could be read back and
; compared; putting it there is a real piece of work and doubles the
; cost of every write when it is on.
;
; The calls exist anyway because of what a program does when they are
; missing.  VERIFY is a global setting that anything may read and
; almost anything may set - installers set it, batch files set it - and
; 54h answering "invalid function" tells a caller the DOS underneath it
; is broken, which is a much larger claim than "writes are not
; verified".  So the flag is remembered honestly and the roadmap
; carries the other half.
;=====================================================================
fn2E:
        mov     al,[ebp+F_EAX]
        and     al,1
        mov     [verify_on],al
        jmp     i21_ok

fn54:
        movzx   eax,byte ptr [verify_on]
        mov     [ebp+F_EAX],al
        jmp     i21_ok

;=====================================================================
; 06h - direct console I/O.  DL = FFh reads (ZF set if nothing there),
;       anything else writes DL.  A file as stdin never blocks: there
;       is either a byte or the end, which reads as "nothing there".
;=====================================================================
fn06:
        mov     al,[ebp+F_EDX]
        cmp     al,0FFh
        je      f06_in
        call    std_out
        mov     [ebp+F_EAX],al
        jmp     i21_ok
f06_in:
        test    dword ptr [sft_tbl+SFT_FLAGS],SFTF_DEVICE
        jz      f06_file
        cmp     dword ptr [sft_tbl+SFT_DEVID],DEV_CON
        jne     f06_other
        call    dword ptr ds:[IOS_CONSTAT]
        jz      f06_none
        call    dword ptr ds:[IOS_CONIN]
        mov     [ebp+F_EAX],ax
        and     dword ptr [ebp+F_FLAGS],NOT FL_ZF
        jmp     i21_ok
f06_other:                              ; NUL or a serial port: whatever it
        mov     edx,dword ptr [sft_tbl+SFT_DEVID]       ; has, without a wait
        call    dev_in
        jc      f06_none
        mov     [ebp+F_EAX],ax
        and     dword ptr [ebp+F_FLAGS],NOT FL_ZF
        jmp     i21_ok
f06_file:
        call    std_in
        jc      f06_none                ; end of file = nothing there
        mov     [ebp+F_EAX],ax
        and     dword ptr [ebp+F_FLAGS],NOT FL_ZF
        jmp     i21_ok
f06_none:
        mov     byte ptr [ebp+F_EAX],0
        or      dword ptr [ebp+F_FLAGS],FL_ZF
        jmp     i21_ok

;=====================================================================
; 07h / 08h - console input without echo -> AL
;=====================================================================
fn07:
fn08:
        call    std_in
        jnc     f07_have
        mov     ax,001Ah                ; end of file reads as ^Z
f07_have:
        mov     [ebp+F_EAX],ax
        jmp     i21_ok

;=====================================================================
; 09h - print the '$'-terminated string at EDX.
;       To the screen a character at a time as always; to a
;       redirected stdout as ONE block write, because file_wchunk
;       writes its cluster through on every call and DIR's output at
;       a cluster write per character would be felt on real iron.
;=====================================================================
fn09:
        mov     esi,[ebp+F_EDX]
        test    dword ptr [sft_tbl+SFT_LEN+SFT_FLAGS],SFTF_DEVICE
        jz      f09_file
        mov     edx,dword ptr [sft_tbl+SFT_LEN+SFT_DEVID]
f09_lp:
        mov     al,[esi]
        cmp     al,'$'
        je      f09_done
        call    dev_out
        inc     esi
        jmp     short f09_lp
f09_file:
        mov     edi,esi
f09_len:
        cmp     byte ptr [edi],'$'
        je      f09_wr
        inc     edi
        jmp     short f09_len
f09_wr:
        mov     ecx,edi
        sub     ecx,esi
        call    std_write
f09_done:
        mov     byte ptr [ebp+F_EAX],'$'
        jmp     i21_ok

;=====================================================================
; 0Ah - buffered line input.  EDX -> max byte, then count byte, then
;       the characters.  CR terminates and is stored but not counted.
;       From a redirected stdin: one line of the file, no echo and no
;       editing - the CR LF convention is honoured and a bare LF
;       terminates too, so LF-only files still read line by line.
;=====================================================================
fn0A:
        mov     edi,[ebp+F_EDX]
        movzx   ecx,byte ptr [edi]      ; capacity
        or      ecx,ecx
        jz      f0A_done
        dec     ecx                     ; room for the CR
        xor     ebx,ebx                 ; EBX = count so far
        test    dword ptr [sft_tbl+SFT_FLAGS],SFTF_DEVICE
        jz      f0A_file
        cmp     dword ptr [sft_tbl+SFT_DEVID],DEV_CON
        jne     f0A_file                ; only the console has line editing;
                                        ; every other device is a byte source
f0A_key:
        call    dos_conin
        cmp     al,3                    ; ^C breaks line input too
        jne     f0A_nobrk
        call    ctrl_break
        jmp     short f0A_key
f0A_nobrk:
        cmp     al,13
        je      f0A_cr
        cmp     al,8
        je      f0A_bs
        cmp     al,32                   ; drop other control keys
        jb      f0A_key
        cmp     ebx,ecx
        jae     f0A_key                 ; line full - swallow it
        mov     [edi+ebx+2],al
        inc     ebx
        call    dword ptr ds:[IOS_CONOUT]
        jmp     short f0A_key
f0A_bs:
        or      ebx,ebx
        jz      f0A_key
        dec     ebx
        mov     al,8
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,8
        call    dword ptr ds:[IOS_CONOUT]
        jmp     short f0A_key
f0A_cr:
        mov     byte ptr [edi+ebx+2],13
        mov     [edi+1],bl
        mov     al,13
        call    dword ptr ds:[IOS_CONOUT]
f0A_done:
        jmp     i21_ok

;--- stdin is a file --------------------------------------------------
f0A_file:
        call    std_in
        jc      f0A_feol                ; end of file ends the line
        cmp     al,13
        je      f0A_fcr
        cmp     al,10
        je      f0A_feol                ; a bare LF terminates too
        cmp     ebx,ecx
        jae     f0A_file                ; line full - swallow the rest
        mov     [edi+ebx+2],al
        inc     ebx
        jmp     short f0A_file
f0A_fcr:
        call    std_in                  ; swallow the LF of a CR LF pair
        jc      f0A_feol
        cmp     al,10
        je      f0A_feol
        test    dword ptr [sft_tbl+SFT_FLAGS],SFTF_DEVICE
        jnz     f0A_feol                ; a device has no position to rewind
        dec     dword ptr [sft_tbl+SFT_POS]     ; not LF - un-read it
f0A_feol:
        mov     byte ptr [edi+ebx+2],13
        mov     [edi+1],bl
        jmp     i21_ok

;=====================================================================
; 0Bh - console status -> AL = FFh if a key is waiting.  A file as
;       stdin always has one waiting - until it does not.
;=====================================================================
fn0B:
        test    dword ptr [sft_tbl+SFT_FLAGS],SFTF_DEVICE
        jz      f0B_file
        cmp     dword ptr [sft_tbl+SFT_DEVID],DEV_CON
        jne     f0B_dead                ; NUL and a silent port: nothing, and
        call    dword ptr ds:[IOS_CONSTAT]      ; there never will be
        mov     al,0
        jz      f0B_none
        mov     al,0FFh
f0B_none:
        mov     [ebp+F_EAX],al
        jmp     i21_ok
f0B_dead:
        mov     al,0
        jmp     short f0B_none
f0B_file:
        mov     al,0FFh
        mov     edx,dword ptr [sft_tbl+SFT_POS]
        cmp     edx,dword ptr [sft_tbl+SFT_SIZE]
        jb      f0B_none
        mov     al,0
        jmp     short f0B_none

;=====================================================================
; 0Ch - flush keyboard then invoke AL (01,06,07,08).  A file needs no
;       flushing - and draining it would eat the input.
;=====================================================================
fn0C:
        test    dword ptr [sft_tbl+SFT_FLAGS],SFTF_DEVICE
        jz      f0C_empty
        cmp     dword ptr [sft_tbl+SFT_DEVID],DEV_CON
        jne     f0C_empty               ; only the keyboard has a buffer
f0C_drain:
        call    dword ptr ds:[IOS_CONSTAT]
        jz      f0C_empty
        call    dword ptr ds:[IOS_CONIN]
        jmp     short f0C_drain
f0C_empty:
        mov     al,[ebp+F_EAX]          ; the sub-function
        cmp     al,01h
        je      fn01
        cmp     al,06h
        je      fn06
        cmp     al,07h
        je      fn07
        cmp     al,08h
        je      fn08
        mov     byte ptr [ebp+F_EAX],0
        jmp     i21_ok

;=====================================================================
; 0Dh - disk reset (nothing cached that needs it yet)
;=====================================================================
fn0D:
        jmp     i21_ok

;=====================================================================
; 0Eh - select drive DL (0 = A:) -> AL = number of logical drives
;=====================================================================
fn0E:
        mov     al,[ebp+F_EDX]
        cmp     al,MAX_DRIVES
        jae     f0E_out                 ; A: B: C: D:
        call    vol_select              ; mounts it if needed
        jc      f0E_out                 ; no volume - default unchanged
        mov     [cur_drive],al          ; a LETTER; it used to be turned
                                        ; back into a BIOS drive here and
                                        ; converted again at every use
f0E_out:
        mov     byte ptr [ebp+F_EAX],MAX_DRIVES
        jmp     i21_ok

;=====================================================================
; 19h - get current drive -> AL (0 = A:)
;=====================================================================
fn19:
        mov     al,[cur_drive]
        mov     [ebp+F_EAX],al
        jmp     i21_ok

;=====================================================================
; 1Ah - set DTA from EDX
;=====================================================================
fn1A:
        mov     eax,[ebp+F_EDX]
        mov     [dta_ptr],eax
        jmp     i21_ok

;=====================================================================
; 25h - set interrupt vector AL to the handler at EDX
;=====================================================================
fn25:
        mov     al,[ebp+F_EAX]
        mov     edi,[ebp+F_EDX]
        call    set_vec
        jmp     i21_ok

;---------------------------------------------------------------------
; get_vec / set_vec - one place that knows how a vector is stored.
;
;   get_vec:  AL = vector -> EAX = handler address
;   set_vec:  AL = vector,   EDI = handler address
;
; 25h and 35h are the API face of these; the PSP save and restore in
; build_psp and 4Ch are the other caller, and it matters that both go
; through the same two routines - a PSP that saved a vector read a
; different way from the way 25h writes it would restore rubbish.
;
; THERE ARE TWO KINDS OF HANDLER ADDRESS NOW and no tag is needed to
; tell them apart, because the two ranges cannot overlap: a native
; program's handler is an offset in its own slot, so it is below the
; slot size, and every kernel address is above 1MB.  A value that
; could be an offset is one; anything else is a kernel address being
; put back.  That one rule serves 25h, 35h, the PSP save, the PSP
; restore and the kernel's own installation, which is the point - a
; vector that went in one way and came out another is how a restored
; handler ends up pointing at nothing.
;---------------------------------------------------------------------
get_vec:
        push    ecx
        movzx   ecx,al
        mov     eax,[r3vec+ecx*4]       ; the program's own, if it has one
        or      eax,eax
        jnz     gv_out
        mov     eax,ecx
        shl     eax,3
        add     eax,IDT_BASE
        movzx   ecx,word ptr [eax+6]    ; offset 31:16
        shl     ecx,16
        movzx   eax,word ptr [eax]      ; offset 15:0
        or      eax,ecx
gv_out:
        pop     ecx
        ret

set_vec:
        push    eax
        cmp     edi,[r3_size]           ; a slot offset - the program's own
        jb      sv_r3
        call    r3_unhook               ; a kernel address: whatever the
        call    dword ptr ds:[IOS_SETVEC]       ; program had is gone
        pop     eax
        ret
sv_r3:
        call    r3_hook
        pop     eax
        ret

;=====================================================================
; 2Ah - get date -> CX = year, DH = month, DL = day, AL = weekday
;=====================================================================
fn2A:
        mov     ah,04h                  ; RTC read date, BCD answers
        call    rtc_call
        jc      f2A_dflt

        movzx   eax,byte ptr ds:[RMR_ECX+1]     ; century
        call    bcd2bin
        imul    eax,100
        push    eax
        movzx   eax,byte ptr ds:[RMR_ECX]       ; year in century
        call    bcd2bin
        pop     ecx
        add     ecx,eax
        mov     [ebp+F_ECX],cx

        movzx   eax,byte ptr ds:[RMR_EDX+1]     ; month
        call    bcd2bin
        mov     [ebp+F_EDX+1],al
        movzx   eax,byte ptr ds:[RMR_EDX]       ; day
        call    bcd2bin
        mov     [ebp+F_EDX],al
        mov     byte ptr [ebp+F_EAX],0          ; weekday: not computed
        jmp     i21_ok
f2A_dflt:
        mov     word ptr [ebp+F_ECX],1980
        mov     byte ptr [ebp+F_EDX+1],1
        mov     byte ptr [ebp+F_EDX],1
        mov     byte ptr [ebp+F_EAX],2
        jmp     i21_ok

;=====================================================================
; 2Ch - get time -> CH = hour, CL = min, DH = sec, DL = 0
;=====================================================================
fn2C:
        mov     ah,02h                  ; RTC read time, BCD answers
        call    rtc_call
        jc      f2C_dflt

        movzx   eax,byte ptr ds:[RMR_ECX+1]
        call    bcd2bin
        mov     [ebp+F_ECX+1],al        ; hour
        movzx   eax,byte ptr ds:[RMR_ECX]
        call    bcd2bin
        mov     [ebp+F_ECX],al          ; minute
        movzx   eax,byte ptr ds:[RMR_EDX+1]
        call    bcd2bin
        mov     [ebp+F_EDX+1],al        ; second
        mov     byte ptr [ebp+F_EDX],0
        jmp     i21_ok
f2C_dflt:
        mov     word ptr [ebp+F_ECX],0
        mov     word ptr [ebp+F_EDX],0
        jmp     i21_ok

;=====================================================================
; 2Bh - set date.  CX = year, DH = month, DL = day.
;       -> AL = 0 accepted, FFh rejected.
;=====================================================================
fn2B:
        movzx   eax,word ptr [ebp+F_ECX]
        cmp     eax,1980
        jb      f2B_bad
        cmp     eax,2099
        ja      f2B_bad
        mov     al,[ebp+F_EDX+1]        ; month
        dec     al
        cmp     al,12-1
        ja      f2B_bad
        mov     al,[ebp+F_EDX]          ; day
        dec     al
        cmp     al,31-1
        ja      f2B_bad

        ; RMFRAME: INT 1Ah/05h, CH=century CL=year DH=month DL=day, BCD
        movzx   eax,word ptr [ebp+F_ECX]
        mov     ecx,100
        xor     edx,edx
        div     ecx                     ; EAX=century, EDX=year-in-century
        call    bin2bcd
        mov     bh,al                   ; century BCD
        mov     eax,edx
        call    bin2bcd
        mov     bl,al                   ; year BCD
        movzx   eax,byte ptr [ebp+F_EDX+1]
        call    bin2bcd
        mov     ch,al                   ; month BCD
        movzx   eax,byte ptr [ebp+F_EDX]
        call    bin2bcd
        mov     cl,al                   ; day BCD

        push    ecx
        push    ebx
        mov     ah,05h
        call    rtc_prep                ; zero the frame, AH into RMR_EAX
        pop     ebx
        pop     ecx
        mov     byte ptr ds:[RMR_ECX+1],bh
        mov     byte ptr ds:[RMR_ECX],bl
        mov     byte ptr ds:[RMR_EDX+1],ch
        mov     byte ptr ds:[RMR_EDX],cl
        mov     al,1Ah
        call    dword ptr ds:[IOS_REALINT]
        mov     byte ptr [ebp+F_EAX],0
        jmp     i21_ok
f2B_bad:
        mov     byte ptr [ebp+F_EAX],0FFh
        jmp     i21_ok

;=====================================================================
; 2Dh - set time.  CH = hour, CL = minute, DH = second.
;       -> AL = 0 accepted, FFh rejected.
;=====================================================================
fn2D:
        mov     al,[ebp+F_ECX+1]
        cmp     al,23
        ja      f2D_bad
        mov     al,[ebp+F_ECX]
        cmp     al,59
        ja      f2D_bad
        mov     al,[ebp+F_EDX+1]
        cmp     al,59
        ja      f2D_bad

        movzx   eax,byte ptr [ebp+F_ECX+1]
        call    bin2bcd
        mov     bh,al                   ; hours BCD
        movzx   eax,byte ptr [ebp+F_ECX]
        call    bin2bcd
        mov     bl,al                   ; minutes BCD
        movzx   eax,byte ptr [ebp+F_EDX+1]
        call    bin2bcd
        mov     ch,al                   ; seconds BCD

        push    ecx
        push    ebx
        mov     ah,03h                  ; INT 1Ah/03h - set RTC time
        call    rtc_prep
        pop     ebx
        pop     ecx
        mov     byte ptr ds:[RMR_ECX+1],bh
        mov     byte ptr ds:[RMR_ECX],bl
        mov     byte ptr ds:[RMR_EDX+1],ch
        mov     byte ptr ds:[RMR_EDX],0 ; no DST
        mov     al,1Ah
        call    dword ptr ds:[IOS_REALINT]
        mov     byte ptr [ebp+F_EAX],0
        jmp     i21_ok
f2D_bad:
        mov     byte ptr [ebp+F_EAX],0FFh
        jmp     i21_ok

;---------------------------------------------------------------------
; rtc_call - INT 1Ah function AH through the real-mode thunk.
;            CF reflects the BIOS carry.
;---------------------------------------------------------------------
rtc_call:
        call    rtc_prep
        mov     al,1Ah
        call    dword ptr ds:[IOS_REALINT]
        test    dword ptr ds:[RMR_FLAGS],FL_CF
        jz      rtc_ok
        stc
        ret
rtc_ok:
        clc
        ret

rtc_prep:                               ; zero RMFRAME, stage caller's AH
        push    eax
        push    ecx
        push    edi
        mov     edi,RMFRAME
        push    eax
        xor     eax,eax
        mov     ecx,RMFRAME_LEN / 4
        rep     stosd
        pop     eax
        mov     byte ptr ds:[RMR_EAX+1],ah
        pop     edi
        pop     ecx
        pop     eax
        ret

bcd2bin:                                ; AL BCD -> EAX binary
        push    ecx
        movzx   ecx,al
        shr     ecx,4
        imul    ecx,10
        and     eax,0Fh
        add     eax,ecx
        pop     ecx
        ret

bin2bcd:                                ; EAX binary (0..99) -> AL BCD
        push    ecx
        push    edx
        xor     edx,edx
        mov     ecx,10
        div     ecx                     ; EAX = tens, EDX = units
        shl     al,4
        or      al,dl
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; get_dosdate / get_dostime - current RTC reading, DOS-packed.
;   get_dosdate -> AX = (year-1980)<<9 | month<<5 | day
;   get_dostime -> AX = hour<<11 | minute<<5 | second/2
; Fall back to fixed values if the RTC is unreadable.
;---------------------------------------------------------------------
get_dosdate:
        push    ebx
        push    ecx
        push    edx
        mov     ah,04h
        call    rtc_call
        jc      gdd_dflt
        movzx   eax,byte ptr ds:[RMR_ECX+1]     ; century
        call    bcd2bin
        imul    eax,100
        mov     ebx,eax
        movzx   eax,byte ptr ds:[RMR_ECX]       ; year
        call    bcd2bin
        add     ebx,eax                         ; EBX = full year
        sub     ebx,1980
        jb      gdd_dflt
        shl     ebx,9
        movzx   eax,byte ptr ds:[RMR_EDX+1]     ; month
        call    bcd2bin
        shl     eax,5
        or      ebx,eax
        movzx   eax,byte ptr ds:[RMR_EDX]       ; day
        call    bcd2bin
        or      eax,ebx
        jmp     short gdd_out
gdd_dflt:
        mov     eax,((2026-1980) SHL 9) OR (1 SHL 5) OR 1
gdd_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

get_dostime:
        push    ebx
        push    ecx
        push    edx
        mov     ah,02h
        call    rtc_call
        jc      gdt_dflt
        movzx   eax,byte ptr ds:[RMR_ECX+1]     ; hours
        call    bcd2bin
        shl     eax,11
        mov     ebx,eax
        movzx   eax,byte ptr ds:[RMR_ECX]       ; minutes
        call    bcd2bin
        shl     eax,5
        or      ebx,eax
        movzx   eax,byte ptr ds:[RMR_EDX+1]     ; seconds
        call    bcd2bin
        shr     eax,1
        or      eax,ebx
        jmp     short gdt_out
gdt_dflt:
        xor     eax,eax
gdt_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; 2Fh - get DTA -> EBX
;=====================================================================
fn2F:
        mov     eax,[dta_ptr]
        call    i21_toprog              ; back into the caller's own terms
        mov     [ebp+F_EBX],eax
        jmp     i21_ok

;---------------------------------------------------------------------
; i21_toprog - EAX = a flat address the kernel holds.  If the caller is
;              a native ring-3 program, hand back the offset it knows
;              that address by instead.
;
; The three calls that ANSWER with an address rather than being given
; one all need this: 2Fh the DTA, 51h and 62h the PSP.  The pointer
; translation on the way in is a table, because which register holds a
; pointer differs per function; on the way out there is no table to
; consult, because a function that answers with an address says so in
; its own definition.
;
; NOT everything that answers with an address goes through here, and
; the two that do not are deliberate.  34h hands out the address of the
; InDOS flag, which lives in the BIOS data area for the benefit of
; real-mode TSRs and is not in any program's slot; 52h hands out the
; list of lists.  Both are honest about being unreachable, in the one
; way that matters - a program that follows either lands in its own
; guard page and is told so - and the alternative is inventing a copy
; inside the slot for a caller that has no business with either.
;---------------------------------------------------------------------
i21_toprog:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     itp_out
        push    eax
        call    r3_off
        jnc     itp_ok
        pop     eax
        ret
itp_ok:
        add     esp,4
itp_out:
        ret

;=====================================================================
; 30h - get version -> AL = major, AH = minor: 6.22
;                      BH = 03h, the OEM number: PM-DOS
;                      BL:CX = 24-bit user serial number, zero
;
; 6.22 IS A TARGET, NOT A CLAIM ABOUT THIS CODE, and it moved here from
; 5.00 in v0.31 because the target changed rather than because anything
; was fixed.  MS-DOS 6.22 is the last version that was only DOS - 7.x
; is Windows 95's - so it is the high-water mark of the API this kernel
; is trying to be, and the number every version check in the software
; of the period was written against.
;
; What PM-DOS itself is has a call of its own, F100h, and 3306h agrees
; with this one rather than telling a different story; PMDOS.INC has
; the argument.
;
; The OEM byte is the only place identity is asserted at all: this is
; how software asks who made the DOS it is running on, which is
; precisely why the kernel files did not need IBM's names.
;
; 03h because the numbers in circulation (Ralf Brown's list) leave it
; open - 00h IBM, 01h Compaq, 02h MS packaged, 04h AT&T, 05h Zenith,
; 06h HP - and picking one's own is what every clone did: DR-DOS took
; EEh, FreeDOS FDh, PTS-DOS 66h, RxDOS 5Eh.  These were never
; centrally administered, so "unassigned" means "unclaimed in the
; published list" rather than guaranteed unique.
;
; There is no OEM *string* in this call, or anywhere else in the DOS
; API - just this byte and the serial.  The one OEM string a DOS
; system carries is the 8-byte OEM name in a FAT boot sector, and
; ours already says 'PM-DOS  ' (BOOT.ASM's bsOEM; MKDISK writes the
; same into hard-disk volume boot records).
;=====================================================================
PMDOS_OEM       EQU     03h

fn30:
        mov     word ptr [ebp+F_EAX],DOSVER_MINOR * 256 + DOSVER_MAJOR
        mov     dword ptr [ebp+F_EBX],PMDOS_OEM * 256   ; BH = OEM, BL = serial hi
        mov     dword ptr [ebp+F_ECX],0                 ; serial lo
        jmp     i21_ok

;=====================================================================
; 33h - the odds and ends DOS filed under one function.
;
;   AL=00h  get Ctrl-Break checking -> DL (0 off, 1 on)
;   AL=01h  set it from DL
;   AL=05h  get the boot drive      -> DL (1 = A:)
;   AL=06h  get the TRUE version    -> BL = major, BH = minor,
;                                      DL = revision, DH = flags
;
; 06h USED TO BE THE HONEST HALF of the version story: 30h said 5.00
; and this said 0.17, on the reasoning that 3306h exists precisely
; because 30h can be lied to by SETVER.
;
; That was the wrong reading of what 3306h is for.  It reports the
; version SETVER is lying ABOUT - still a DOS version, just the
; un-faked one - and a program that finds 6.22 at one call and 0.31 at
; the other has not found the truth, it has found a system that
; contradicts itself, and the sensible thing for it to do is refuse to
; run.  So the two agree, and PM-DOS's own build number lives at F100h
; where nothing mistakes it for a DOS version.
;
; DH's flags say DOS is neither in ROM (08h) nor in the HMA (10h), both
; of which are true: it is in extended memory, which the flags have no
; way of saying.
;=====================================================================
fn33:
        mov     al,[ebp+F_EAX]
        or      al,al
        jz      f33_getbrk
        cmp     al,1
        je      f33_setbrk
        cmp     al,5
        je      f33_boot
        cmp     al,6
        je      f33_ver
        jmp     i21_unsup

f33_getbrk:
        movzx   eax,byte ptr [break_flag]
        mov     [ebp+F_EDX],eax
        jmp     i21_ok
f33_setbrk:
        mov     al,[ebp+F_EDX]
        and     al,1
        mov     byte ptr [break_flag],al
        jmp     i21_ok
f33_boot:
        mov     al,byte ptr ds:[GV_BOOTDRV]
        call    bios2log
        inc     al                      ; 1 = A:, as this call reports it
        movzx   eax,al
        mov     [ebp+F_EDX],eax
        jmp     i21_ok
f33_ver:
        mov     dword ptr [ebp+F_EBX],DOSVER_MINOR * 256 + DOSVER_MAJOR
        mov     dword ptr [ebp+F_EDX],0 ; revision 0, and no ROM/HMA flags
        jmp     i21_ok

;=====================================================================
; 35h - get interrupt vector AL -> EBX (flat handler address)
;=====================================================================
fn35:
        mov     al,[ebp+F_EAX]
        call    get_vec
        mov     [ebp+F_EBX],eax
        jmp     i21_ok

;=====================================================================
; 36h - get free disk space.  DL = drive (0 = default, 1 = A:).
;       -> AX = sectors/cluster, BX = free clusters,
;          CX = bytes/sector,    DX = total clusters
;=====================================================================
fn36:
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     f36_expl
        mov     al,[cur_drive]
        jmp     short f36_sel
f36_expl:
        dec     al                      ; 1-based -> logical
        cmp     al,MAX_DRIVES
        jae     f36_bad
f36_sel:
        call    vol_select
        jc      f36_bad
        mov     eax,[fat_spc]
        mov     [ebp+F_EAX],ax
        mov     word ptr [ebp+F_ECX],512

        ; DOS returns these in 16-bit registers; huge volumes saturate
        mov     eax,[fat_totclus]
        cmp     eax,0FFFFh
        jbe     f36_tot_ok
        mov     eax,0FFFFh
f36_tot_ok:
        mov     [ebp+F_EDX],ax

        call    fat_freecount
        mov     ebx,eax
        cmp     ebx,0FFFFh
        jbe     f36_free_ok
        mov     ebx,0FFFFh
f36_free_ok:
        mov     [ebp+F_EBX],bx
        jmp     i21_ok
f36_bad:
        mov     word ptr [ebp+F_EAX],0FFFFh     ; DOS: AX=FFFF = bad drive
        jmp     i21_ok

;=====================================================================
; 3Ch - create/truncate file.  EDX -> path, CX = attributes.
;       -> AX = handle (open for write)
;
; 5Bh - create NEW file, same arguments, but fail with "file exists"
;       rather than truncating one that is already there.
;
; The two are one routine and one flag, which is how they differ in
; real DOS too.  5Bh matters far more than its size suggests: it is
; the only way to ask the file system "make this file, but not if it
; would destroy something", and without it a program that wants that
; guarantee has to test with 3Dh and then create - which is a race on
; a real DOS and, more to the point, is what an archiver's "do not
; overwrite" option is built on.  A 5Bh that answers "invalid
; function" sends every one of those programs down a fallback path
; that calls 3Ch, and the file is gone.
;=====================================================================
fn3C:
        mov     byte ptr [create_new],0
        jmp     short f3C_go

fn5B:
        mov     byte ptr [create_new],1
f3C_go:
        mov     esi,[ebp+F_EDX]
        call    dev_find                ; a device is a fine create target -
        jc      f3C_file                ; "> NUL" is a create, and so is
        mov     edx,eax                 ; "> PRN"
        call    sft_new
        jc      f3C_full
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE OR SFTF_DEVICE
        mov     dword ptr [esi+SFT_MODE],1
        mov     [esi+SFT_DEVID],edx
        mov     [ebp+F_EAX],ax
        jmp     i21_ok

f3C_file:
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f3C_deny
        cmp     byte ptr [rp_noname],0
        jne     f3C_deny                ; the path names a directory
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f3C_deny

        mov     byte ptr [srch_attr],ATTR_DIR   ; collide with dirs too
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f3C_new
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jnz     f3C_deny                ; a directory wears that name
        cmp     byte ptr [create_new],0
        jne     f3C_exists              ; 5Bh: that is the whole point

        ; exists: truncate in place
        call    de_getclus
        call    fat_freechain
        xor     eax,eax
        call    de_setclus
        mov     dword ptr [esi+DE_SIZE],0
        jmp     short f3C_stamp

f3C_new:
        cmp     byte ptr [rp_flong],0   ; a name only an LFN can spell
        je      f3C_slot                ; gets a ~N alias and a chain
        mov     esi,[rp_fptr]
        mov     ecx,[rp_flen]
        call    lfn_alias               ; -> name_buf = the unique alias
        jc      f3C_deny
        mov     esi,[rp_fptr]
        mov     ecx,[rp_flen]
        call    lfn_store               ; fragments in; EBX/ESI = slot
        jc      f3C_deny
        jmp     short f3C_fill
f3C_slot:
        call    fat_findfree            ; EBX = slot, ESI -> entry
        jc      f3C_deny
f3C_fill:
        push    ebx
        push    esi
        mov     edi,esi                 ; blank the whole 32-byte entry
        xor     eax,eax
        mov     ecx,8
        rep     stosd
        pop     esi
        mov     edi,esi                 ; write the name in
        push    esi
        mov     ecx,11
        push    edi
        mov     esi,OFFSET name_buf
        rep     movsb
        pop     edi
        pop     esi
        pop     ebx

f3C_stamp:
        mov     al,[ebp+F_ECX]          ; requested attributes
        and     al,27h                  ; RHS + archive only
        or      al,ATTR_ARCHIVE
        mov     [esi+DE_ATTR],al
        call    get_dostime
        mov     [esi+DE_TIME],ax
        call    get_dosdate
        mov     [esi+DE_DATE],ax
        mov     eax,[esi+DE_TIME]       ; the same stamp into the handle,
        mov     [open_stamp],eax        ; so a 57h get straight after a
                                        ; create answers with it
        call    fat_dirupdate
        jc      f3C_deny
        call    fat_flush
        jc      f3C_deny

        mov     edi,ebx                 ; directory index
        call    sft_new                 ; ESI = fresh SFT entry
        jc      f3C_full
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        mov     dword ptr [esi+SFT_MODE],1      ; write
        mov     dword ptr [esi+SFT_CLUSTER],0
        mov     dword ptr [esi+SFT_CURCLUS],0
        mov     dword ptr [esi+SFT_SIZE],0
        mov     dword ptr [esi+SFT_POS],0
        ; ECX, NOT EAX.  AX is the handle sft_new just returned and it
        ; is not stored into the frame until four lines below this one;
        ; loading the stamp through EAX made every 3Ch hand back the
        ; file's packed time and date as its handle, with the carry
        ; flag clear to say it had worked.
        mov     ecx,[open_stamp]
        mov     [esi+SFT_TIME],ecx
        mov     [esi+SFT_DIRENT],edi
        mov     dword ptr [esi+SFT_HANDLE],0
        call    sft_setdir              ; remember which directory
        mov     [ebp+F_EAX],ax
        jmp     i21_ok

;---------------------------------------------------------------------
; sft_setdir - record the current directory context AND the loaded
;              volume in the SFT entry at ESI, so later reads, writes
;              and the close can find their way back to both.
;---------------------------------------------------------------------
sft_setdir:
        push    eax
        mov     eax,[dc_start]
        mov     [esi+SFT_DIRSTART],eax
        and     dword ptr [esi+SFT_FLAGS],NOT SFTF_DIRROOT
        cmp     byte ptr [dc_isroot],0
        je      ssd_out
        or      dword ptr [esi+SFT_FLAGS],SFTF_DIRROOT
ssd_out:
        mov     al,[cur_vol]            ; which VOLUME this handle is on;
        movzx   eax,al                  ; two of them can share a disk
        mov     [esi+SFT_DRIVE],eax
        pop     eax
        ret

;---------------------------------------------------------------------
; sft_seldrv - load the volume the SFT entry at ESI lives on.
;              CF if that volume is gone.  Preserves registers.
;---------------------------------------------------------------------
sft_seldrv:
        push    eax
        mov     eax,[esi+SFT_DRIVE]
        call    vol_select
        pop     eax                     ; flags survive the pop
        ret

f3C_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f3C_full:
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail
f3C_exists:
        mov     ax,ERR_EXISTS
        jmp     i21_fail

;=====================================================================
; 5Ah - create a UNIQUE file.  EDX -> a directory path ending in a
;       backslash, CX = attributes.  The name is appended to that
;       buffer and the handle comes back in AX.
;
; The caller supplies the directory and DOS supplies the name, and the
; name it gets back is as much the result as the handle is - this is
; how a compiler makes its intermediates and how anything that writes
; a file and renames it into place gets somewhere safe to write.
;
; THE BUFFER IS WRITTEN EVEN THOUGH THE CALLER OWNS IT, which is the
; part that trips people: EDX must point at a buffer with 13 bytes
; spare after the path, and DOS says so, and there is nothing here that
; can check it.  The name is eight characters and no extension, as real
; DOS makes them.
;
; Uniqueness is by TESTING, not by hoping the clock is fine-grained
; enough: the tick count seeds it, the counter moves it on, and each
; candidate is looked for before it is created.  Sixty-four tries is a
; great deal more than a system that creates one temporary file at a
; time will ever need, and a bounded loop cannot become a hang.
;=====================================================================
F5A_TRIES       EQU     64

fn5A:
        ; where the generated name goes, and the caller's terminator
        mov     esi,[ebp+F_EDX]
        mov     edi,esi
f5A_end:
        cmp     byte ptr [edi],0
        je      f5A_haveend
        inc     edi
        jmp     short f5A_end
f5A_haveend:
        mov     [f5a_tail],edi

        ; Seed from the tick count.  It only has to differ from the
        ; last one often enough that the search below usually succeeds
        ; first time; the search is what makes it correct.
        call    dword ptr ds:[IOS_TICKS]
        mov     [f5a_seed],eax
        mov     dword ptr [f5a_try],F5A_TRIES

f5A_next:
        mov     edi,[f5a_tail]          ; write eight hex digits
        mov     eax,[f5a_seed]
        mov     ecx,8
f5A_dig:
        rol     eax,4
        push    eax
        and     al,0Fh
        cmp     al,10
        jb      f5A_num
        add     al,'A' - 10
        jmp     short f5A_put
f5A_num:
        add     al,'0'
f5A_put:
        mov     [edi],al
        inc     edi
        pop     eax
        loop    f5A_dig
        mov     byte ptr [edi],0

        ; Is it taken?  Anything that resolves at all - a file OR a
        ; directory - counts as taken, which is why srch_attr lets
        ; directories match.
        mov     esi,[ebp+F_EDX]
        call    dev_deny                ; "C:\NUL" is not somewhere to put
        jnc     f5A_undo                ; a temporary file
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f5A_undo
        cmp     byte ptr [rp_noname],0
        jne     f5A_undo
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f5A_undo
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f5A_free

        inc     dword ptr [f5a_seed]    ; taken: try the next one
        dec     dword ptr [f5a_try]
        jnz     f5A_next
f5A_undo:
        mov     edi,[f5a_tail]          ; leave the caller's buffer as it
        mov     byte ptr [edi],0        ; was found
        mov     ax,ERR_ACCESS
        jmp     i21_fail

f5A_free:
        ; The name is free and the frame's EDX already points at the
        ; whole path, so the ordinary create does the rest.  It will
        ; resolve a second time, which is a few hundred cycles to keep
        ; one copy of the code that makes a file.
        mov     byte ptr [create_new],1
        jmp     f3C_go

;=====================================================================
; 60h - TRUENAME.  ESI -> the path to canonicalise, EDI -> a 128-byte
;       buffer for "D:\DIR\NAME".
;
; The kernel has always been able to do this - it is what the resolver
; does on the way to every open - and 60h is that answer handed back
; instead of used.  resolve_core leaves the whole path in rp_names as
; 8.3 components with '.' and '..' already collapsed and a relative
; path already seeded from the current directory, so the work here is
; the drive letter, a backslash, and the rendering PATH.INC does for
; 47h.
;
; It matters more than a "convenience" call sounds: it is how a program
; turns what a user typed into something it can compare, and every tool
; that decides whether two names are the same file starts here.  A
; batch-file editor or an archiver that cannot ask has to build its own
; and get the same corner cases wrong.
;
; The name does NOT have to exist.  A caller asking where a file it is
; about to create would land is asking a fair question, so a resolve
; failure on the FINAL component is not an error - only a bad drive or
; a missing directory is.
;=====================================================================
fn60:
        mov     esi,[ebp+F_ESI]
        call    resolve_core
        jc      f60_bad

        mov     edi,[ebp+F_EDI]
        mov     al,[cur_vol]            ; whichever volume it resolved on,
        add     al,'A'                  ; which is not always the default
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        mov     byte ptr [edi+2],'\'
        add     edi,3
        mov     esi,OFFSET rp_names
        mov     ecx,[rp_cnt]
        call    names_string
        jmp     i21_ok
f60_bad:
        cmp     byte ptr [rp_baddrv],0
        je      f60_nopath
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail
f60_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail

;=====================================================================
; 68h / 6Ah - commit file.  BX = handle.  Everything written through
;             that handle is on the disk when this returns.
;
; 68h and 6Ah are the same call under two numbers, the way 51h and 62h
; are: 68h came with DOS 3.3 and 6Ah is the DOS 4 name for it.
;
; A program that calls this is one that has decided its data matters
; more than its speed - a database between transactions, an editor
; after a save - and the alternative it falls back on when the call
; fails is CLOSING AND REOPENING THE FILE, which costs a directory
; search and loses the file position.  Cheap to provide and unpleasant
; to be without.
;
; The flush is sft_close's, minus the closing: the same directory entry
; write and the same FAT flush, with the handle left open and usable
; and the size and cluster chain on disk agreeing with what has been
; written.
;=====================================================================
fn68:
        movzx   ebx,word ptr [ebp+F_EBX]
        cmp     ebx,3                   ; the standard handles are devices
        jb      i21_ok                  ; or files nobody else can flush
        call    sft_get
        jc      f68_bad
        call    sft_flush
        jmp     i21_ok
f68_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;=====================================================================
; 41h - delete file.  EDX -> path (no wildcards at this level).
;=====================================================================
fn41:
        mov     esi,[ebp+F_EDX]
        call    dev_deny                ; "DEL NUL" deletes nothing, ever
        jnc     i21_fail
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f41_nofile
        cmp     byte ptr [rp_noname],0
        jne     f41_deny
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f41_deny

        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f41_nofile
        call    lfn_tomb                ; the chain dies with the file
        call    dir_seek                ; back onto OUR sector and entry
        jc      f41_nofile

        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jnz     f41_deny                ; that is a directory - use 3Ah
        test    byte ptr [esi+DE_ATTR],ATTR_READONLY
        jnz     f41_deny

        call    de_getclus
        call    fat_freechain
        mov     byte ptr [esi],0E5h     ; tombstone the entry
        call    fat_dirupdate
        jc      f41_deny
        call    fat_flush
        jc      f41_deny
        mov     dword ptr [clusbuf_clus],-1     ; cached data now stale
        jmp     i21_ok

f41_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f41_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

;=====================================================================
; 39h - MKDIR.  EDX -> path.
;=====================================================================
fn39:
        mov     esi,[ebp+F_EDX]
        call    dev_deny                ; no directory may be called NUL
        jnc     i21_fail
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f39_nopath
        cmp     byte ptr [rp_noname],0
        jne     f39_deny
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f39_deny

        ; name must be entirely unclaimed, file or directory
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jnc     f39_deny

        ; remember the parent for the ".." entry and for the slot
        call    ctx_save
        mov     [mk_par_isroot],al
        mov     [mk_par_start],edx

        cmp     byte ptr [rp_flong],0   ; a long directory name gets a
        je      f39_slot                ; ~N alias and a chain, exactly
        mov     esi,[rp_fptr]           ; like a file's
        mov     ecx,[rp_flen]
        call    lfn_alias
        jc      f39_deny
        mov     esi,[rp_fptr]
        mov     ecx,[rp_flen]
        call    lfn_store
        jc      f39_deny
        jmp     short f39_slotok
f39_slot:
        call    fat_findfree            ; may extend a chain directory
        jc      f39_deny
f39_slotok:
        mov     [mk_slot],ebx

        call    fat_alloc               ; the directory's first cluster
        jc      f39_full
        mov     [mk_clus],eax

        ; build "." and ".." in a zeroed cluster image
        push    eax
        mov     edi,OFFSET clus_buf
        xor     eax,eax
        mov     ecx,CLUSBUF_SIZE / 4
        rep     stosd
        pop     eax
        mov     dword ptr [clusbuf_clus],-1

        mov     edi,OFFSET clus_buf     ; "."  -> itself
        mov     ecx,11
        mov     al,' '
f39_dot1:
        mov     [edi],al
        inc     edi
        loop    f39_dot1
        mov     byte ptr [clus_buf+0],'.'
        mov     byte ptr [clus_buf+DE_ATTR],ATTR_DIR
        mov     esi,OFFSET clus_buf
        mov     eax,[mk_clus]
        call    de_setclus
        call    get_dostime
        mov     [esi+DE_TIME],ax
        call    get_dosdate
        mov     [esi+DE_DATE],ax

        mov     edi,OFFSET clus_buf+32  ; ".." -> the parent
        mov     ecx,11
        mov     al,' '
f39_dot2:
        mov     [edi],al
        inc     edi
        loop    f39_dot2
        mov     byte ptr [clus_buf+32],'.'
        mov     byte ptr [clus_buf+33],'.'
        mov     byte ptr [clus_buf+32+DE_ATTR],ATTR_DIR
        mov     esi,OFFSET clus_buf+32
        xor     eax,eax                 ; parent = root records 0
        cmp     byte ptr [mk_par_isroot],0
        jne     f39_ddclus
        mov     eax,[mk_par_start]
        cmp     byte ptr [fat_type],32  ; FAT32: the root chain IS a
        jne     f39_ddclus              ; cluster, but ".." still says 0
        cmp     eax,[fat_rootclus]
        jne     f39_ddclus
        xor     eax,eax
f39_ddclus:
        call    de_setclus
        push    eax
        mov     ax,word ptr [clus_buf+DE_TIME]
        mov     [esi+DE_TIME],ax
        mov     ax,word ptr [clus_buf+DE_DATE]
        mov     [esi+DE_DATE],ax
        pop     eax

        mov     eax,[mk_clus]
        mov     edi,OFFSET clus_buf
        call    fat_writeclus
        jc      f39_deny

        ; now the entry in the parent
        mov     al,[mk_par_isroot]
        mov     edx,[mk_par_start]
        call    ctx_restore
        mov     ebx,[mk_slot]
        call    dir_seek
        jc      f39_deny

        push    esi
        mov     edi,esi                 ; blank it
        xor     eax,eax
        mov     ecx,8
        rep     stosd
        pop     esi
        push    esi
        mov     edi,esi                 ; name
        mov     ecx,11
        push    esi
        mov     esi,OFFSET name_buf
        rep     movsb
        pop     esi
        pop     esi
        mov     byte ptr [esi+DE_ATTR],ATTR_DIR
        mov     eax,[mk_clus]
        call    de_setclus
        call    get_dostime
        mov     [esi+DE_TIME],ax
        call    get_dosdate
        mov     [esi+DE_DATE],ax

        call    fat_dirupdate
        jc      f39_deny
        call    fat_flush
        jc      f39_deny
        jmp     i21_ok

f39_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail
f39_full:
        mov     ax,ERR_DISKFULL
        jmp     i21_fail
f39_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

;=====================================================================
; 3Ah - RMDIR.  EDX -> path.  The directory must be empty and must
;       not be the current one.
;=====================================================================
fn3A:
        mov     esi,[ebp+F_EDX]
        call    dev_deny
        jnc     i21_fail
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f3A_nopath
        cmp     byte ptr [rp_noname],0
        jne     f3A_deny                ; "\" or "." - never removable
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f3A_deny

        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f3A_nopath
        call    lfn_tomb                ; the chain goes with the dir
        call    dir_seek
        jc      f3A_nopath
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jz      f3A_nopath              ; a file - 41h territory

        call    ctx_save                ; parent context + slot
        mov     [mk_par_isroot],al
        mov     [mk_par_start],edx
        mov     [mk_slot],ebx
        call    de_getclus
        mov     [mk_clus],eax

        cmp     eax,[cwd_clus]          ; not while we stand in it
        je      f3A_deny
        or      eax,eax
        jz      f3A_deny                ; the root itself

        ; must hold nothing but "." and ".."
        call    ctx_chain
        xor     ebx,ebx
f3A_scan:
        call    dir_seek
        jc      f3A_empty               ; ran off the end - empty
        mov     al,[esi]
        or      al,al
        jz      f3A_empty
        cmp     al,0E5h
        je      f3A_skip
        cmp     al,'.'                  ; "." and ".." live here
        je      f3A_skip
        jmp     short f3A_notempty
f3A_skip:
        inc     ebx
        jmp     short f3A_scan

f3A_empty:
        mov     al,[mk_par_isroot]      ; back to the parent
        mov     edx,[mk_par_start]
        call    ctx_restore
        mov     ebx,[mk_slot]
        call    dir_seek
        jc      f3A_deny
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
        jc      f3A_deny
        mov     eax,[mk_clus]
        call    fat_freechain
        call    fat_flush
        jc      f3A_deny
        jmp     i21_ok

f3A_notempty:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f3A_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail
f3A_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

;=====================================================================
; 3Bh - CHDIR.  EDX -> path.
;=====================================================================
fn3B:
        mov     esi,[ebp+F_EDX]
        call    resolve_core
        jc      f3B_fail
        mov     ecx,[rp_cnt]
        call    walk_to                 ; verifies every step is a dir
        jc      f3B_fail

        xor     eax,eax                 ; EAX = the dir's start cluster
        cmp     byte ptr [dc_isroot],0
        jne     f3B_install
        mov     eax,[dc_start]
        cmp     byte ptr [fat_type],32
        jne     f3B_install
        cmp     eax,[fat_rootclus]      ; FAT32 root normalizes to 0
        jne     f3B_install
        xor     eax,eax
f3B_install:
        call    cwd_install
        jc      f3B_fail
        jmp     i21_ok

f3B_nopath:
        mov     ax,ERR_NOPATH
f3B_fail:
        jmp     i21_fail

mk_par_isroot db 0
              db 0,0,0
mk_par_start  dd 0
mk_slot       dd 0
mk_clus       dd 0

;=====================================================================
; 3Dh - open file.  EDX -> path, AL = mode (writes refused).
;       -> AX = handle
;=====================================================================
fn3D:
        mov     al,[ebp+F_EAX]
        and     al,3
        cmp     al,3
        je      f3D_deny                ; 3 is not a valid access mode
        movzx   eax,al
        mov     [open_mode],eax

        ; device name?
        mov     esi,[ebp+F_EDX]
        call    dev_find
        jc      f3D_file
        mov     edx,eax
        call    sft_new
        jc      f3D_full
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE OR SFTF_DEVICE
        mov     ecx,[open_mode]
        mov     [esi+SFT_MODE],ecx
        mov     [esi+SFT_DEVID],edx
        mov     [ebp+F_EAX],ax
        jmp     i21_ok

f3D_file:
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f3D_nofile
        cmp     byte ptr [rp_noname],0
        jne     f3D_nofile
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f3D_nofile

        mov     byte ptr [srch_attr],0
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f3D_nofile

        ; ESI -> live directory entry; grab what we need before the
        ; sector cache gets recycled
        mov     eax,[esi+DE_TIME]       ; time and date are adjacent words,
        mov     [open_stamp],eax        ; and 57h will want both
        call    de_getclus
        mov     ecx,eax
        mov     edx,[esi+DE_SIZE]
        mov     edi,ebx                 ; directory index

        call    sft_new
        jc      f3D_full
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        mov     [esi+SFT_CLUSTER],ecx
        mov     [esi+SFT_CURCLUS],ecx
        mov     [esi+SFT_SIZE],edx
        mov     dword ptr [esi+SFT_POS],0
        mov     ecx,[open_mode]
        mov     [esi+SFT_MODE],ecx
        mov     ecx,[open_stamp]
        mov     [esi+SFT_TIME],ecx      ; ...both, in the one dword
        mov     [esi+SFT_DIRENT],edi
        mov     dword ptr [esi+SFT_HANDLE],0    ; cur cluster index
        call    sft_setdir
        mov     [ebp+F_EAX],ax
        jmp     i21_ok

f3D_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f3D_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f3D_full:
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail

;=====================================================================
; WAITING FOR A KEY, AND INT 28h
;
; DOS issues INT 28h while it has nothing to do but wait for the
; keyboard, and it is the only moment at which a resident program is
; allowed to call DOS back: a TSR that wants to do file work does it
; from this hook, because everywhere else the DOS it would be calling
; is already inside a call of its own.  PRINT's background spooling is
; the canonical use and the reason the hook exists.
;
; SO THE WAIT HAS TO BE OURS, not PMIO's.  IOS_CONIN blocks on a HLT
; inside the keyboard driver, which is the right thing for a driver to
; do and leaves no room for anything to happen in between.  Every DOS
; path that waits for a key now goes through dos_conin, which polls
; IOS_CONSTAT and idles.
;
; WHOSE INT 28h?  This is where PM-DOS differs from a real DOS and the
; difference is worth stating plainly.  The handler a TSR installs is
; REAL-MODE code, reachable only in V86, so INT 28h can only be issued
; while a DOS box is running - and only then is it issued.  A native
; 32-bit program waiting at the prompt (COMMAND.COM, most of the time)
; idles without it, because there is nothing on the other end: a
; 32-bit program cannot go resident here, so a 32-bit INT 28h would be
; a call to an empty room.
;
; A TSR loaded in one DOS box and popped up from ANOTHER is the real
; goal and this is a step towards it, not the whole of it - the rest is
; INT 9 and belongs with the ring-3 work.
;=====================================================================

;---------------------------------------------------------------------
; dos_conin - wait for a key, letting the idle hook run.  AX = the key,
;             exactly as IOS_CONIN would have returned it.
;---------------------------------------------------------------------
dos_conin:
        call    dword ptr ds:[IOS_CONSTAT]
        jnz     dci_have
        call    dos_idle
        jmp     short dos_conin
dci_have:
        call    dword ptr ds:[IOS_CONIN]
        ret

;---------------------------------------------------------------------
; dos_idle - one pass of doing nothing, as usefully as possible.
;---------------------------------------------------------------------
dos_idle:
        call    dos_int28
        jc      di_hlt                  ; nothing was called: sleep instead
        ret                             ; something ran; go round again at
                                        ; once rather than at 18.2Hz
di_hlt:
        sti
        hlt                             ; the timer will do
        ret

;---------------------------------------------------------------------
; dos_int28 - issue INT 28h into the DOS box, if there is one and if
;             this is a safe moment.  CF set = not issued.
;
; The safety rules are DOS's own, and they are why InDOS is a counter:
;
;   InDOS must be exactly 1.  It is 1 because of the very call that is
;   waiting - the 21h/01h or 3Fh we are inside - and anything higher
;   means a critical-error handler or an INT 23h is stacked on top of
;   that, which is not an idle system by any reading.
;
;   The critical-error flag must be clear.  A pop-up during "Abort,
;   Retry, Fail?" would be talking to a DOS that is halfway through
;   failing.
;
;   And no excursion may already be running, because vdm_ctx is a
;   single context and this borrows it.
;---------------------------------------------------------------------
dos_int28:
        cmp     dword ptr ds:[vdm_busy],0
        je      d28_no                  ; no box, no real-mode handler
        cmp     dword ptr ds:[dpmi_rmact],0
        jne     d28_no
        cmp     word ptr ds:[xms_cbseg],0
        je      d28_no
        cmp     byte ptr ds:[DOS_INDOS],1
        jne     d28_no
        cmp     byte ptr ds:[DOS_ERRFLG],0
        jne     d28_no

        pushad
        ; A register structure of our own, filled from the GUEST'S OWN
        ; parked context - every register, not just the stack.
        ;
        ; The temptation is to zero it, on the reasoning that an idle
        ; hook has no arguments.  But INT 28h on a real DOS is an
        ; interrupt taken at a moment when the program was in the
        ; middle of something, so the handler sees whatever was live -
        ; and in particular it sees a plausible DS.  Handing it zeroes
        ; would be handing it a machine state no real DOS ever
        ; produces, and the first TSR to assume something about DS
        ; would fail here and nowhere else.
        mov     edi,OFFSET idle_rmcs
        xor     eax,eax
        mov     ecx,(RMCS_LEN + 3) / 4  ; 32h is not a multiple of four, and
        rep     stosd                   ; idle_rmcs is padded so it can be

        mov     esi,OFFSET vdm_ctx
        mov     edi,OFFSET idle_rmcs
        mov     eax,[esi+V86C_EDI]
        mov     [edi+RMCS_EDI],eax
        mov     eax,[esi+V86C_ESI]
        mov     [edi+RMCS_ESI],eax
        mov     eax,[esi+V86C_EBP]
        mov     [edi+RMCS_EBP],eax
        mov     eax,[esi+V86C_EBX]
        mov     [edi+RMCS_EBX],eax
        mov     eax,[esi+V86C_EDX]
        mov     [edi+RMCS_EDX],eax
        mov     eax,[esi+V86C_ECX]
        mov     [edi+RMCS_ECX],eax
        mov     eax,[esi+V86C_EAX]
        mov     [edi+RMCS_EAX],eax
        mov     eax,[esi+V86C_ES]
        mov     [edi+RMCS_ES],ax
        mov     eax,[esi+V86C_DS]
        mov     [edi+RMCS_DS],ax
        mov     eax,[esi+V86C_FS]
        mov     [edi+RMCS_FS],ax
        mov     eax,[esi+V86C_GS]
        mov     [edi+RMCS_GS],ax
        mov     eax,[esi+V86C_SS]
        mov     [edi+RMCS_SS],ax
        mov     eax,[esi+V86C_ESP]
        mov     [edi+RMCS_SP],ax

        mov     dword ptr ds:[dpmi_rmcs],OFFSET idle_rmcs
        mov     byte ptr ds:[dpmi_rmint],28h
        mov     word ptr ds:[dpmi_rmwords],0
        call    dpmi_simint             ; borrows the box's own V86 loop
        popad
        jc      d28_no
        clc
        ret
d28_no:
        stc
        ret

;=====================================================================
; CHARACTER DEVICES
;
; Until v0.30 there was exactly one, CON, recognised by comparing four
; bytes.  Everything else - NUL, PRN, LPT1, AUX - fell through to the
; file path, so "VER > NUL" CREATED A FILE CALLED NUL and every batch
; file in the world quietly littered the disk it ran from.
;
; A device is now a name looked up in one table, and that lookup has
; to happen at every call that takes a path, not just at open: DEL,
; REN, MD, RD and the attribute calls all have to refuse, or the file
; system gets a chance to invent an entry with a reserved name that
; nothing can afterwards open.
;=====================================================================

;---------------------------------------------------------------------
; dev_find - is the ASCIIZ path at ESI a character device?
;            CF clear: EAX = device id, kind + unit * DEV_UNIT.
;            CF set:   it names a file.  Every register but EAX kept.
;
; DOS matches a device in ANY directory and under ANY extension:
; "NUL", "C:\TEMP\NUL" and "NUL.TXT" are all the bit bucket.  So this
; takes the last path component, stops at the dot, and looks up what
; is left of it.  That rule is the reason no file may be called NUL,
; and it is why the check belongs on the whole path rather than on the
; first three characters of it.
;---------------------------------------------------------------------
dev_find:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        mov     edx,esi                 ; EDX = start of the last component
df_scan:
        mov     al,[esi]
        or      al,al
        jz      df_last
        cmp     al,'\'
        je      df_sep
        cmp     al,'/'
        je      df_sep
        cmp     al,':'
        jne     df_next
df_sep:
        lea     edx,[esi+1]             ; ...moved on by every separator
df_next:
        inc     esi
        jmp     short df_scan

df_last:
        mov     edi,OFFSET dev_name     ; blank-padded to eight, as the
        mov     eax,20202020h           ; table entries are
        mov     [edi],eax
        mov     [edi+4],eax
        xor     ecx,ecx
df_copy:
        mov     al,[edx]
        or      al,al
        jz      df_look
        cmp     al,'.'
        je      df_look                 ; the extension is not part of it
        cmp     ecx,8
        jae     df_no                   ; longer than eight: not a device
        cmp     al,'a'
        jb      df_up
        cmp     al,'z'
        ja      df_up
        sub     al,20h
df_up:
        mov     [edi+ecx],al
        inc     ecx
        inc     edx
        jmp     short df_copy

df_look:
        or      ecx,ecx
        jz      df_no                   ; an empty component names nothing
        mov     esi,OFFSET dev_tab
df_try:
        cmp     dword ptr [esi],0       ; a zero name ends the table
        je      df_no
        push    esi
        mov     edi,OFFSET dev_name
        mov     ecx,8
        repe    cmpsb
        pop     esi
        je      df_hit
        add     esi,DEVT_LEN
        jmp     short df_try
df_hit:
        mov     eax,[esi+8]
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
df_no:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; dev_path - the guard every path-taking call wears.  CF set (and the
;            path is an ordinary file) means carry on; CF clear means
;            the caller should fail with the code in AX.
;
; ERR_ACCESS for the calls that would MODIFY the name (delete, rename,
; make and remove directory) and ERR_NOFILE for the ones that merely
; look it up, which is what MS-DOS answers: "ATTRIB NUL" there says
; file not found, and "DEL NUL" says access denied.
;---------------------------------------------------------------------
dev_deny:                               ; -> ERR_ACCESS
        call    dev_find
        jc      dp_file
        mov     ax,ERR_ACCESS
        clc
        ret
dev_hide:                               ; -> ERR_NOFILE
        call    dev_find
        jc      dp_file
        mov     ax,ERR_NOFILE
        clc
        ret
dp_file:
        stc
        ret

;---------------------------------------------------------------------
; dev_out - write AL to the device whose id is in EDX.  All registers
;           preserved.  There is no error return, and there is nowhere
;           for one to go: the DOS character-output calls this serves
;           have never had one.
;---------------------------------------------------------------------
dev_out:
        cmp     dl,DEV_CON
        je      do_con
        cmp     dl,DEV_PRN
        je      do_prn
        cmp     dl,DEV_AUX
        je      do_aux
do_nul:
        ret                             ; DEV_NUL, and anything unknown
do_con:
        jmp     dword ptr ds:[IOS_CONOUT]

do_prn:                                 ; INT 17h AH=00h - print AL on DX
        push    eax
        push    edx
        call    dev_prep
        mov     byte ptr ds:[RMR_EAX],al
        mov     byte ptr ds:[RMR_EDX],dh        ; the unit becomes the port
        mov     al,17h
        call    dword ptr ds:[IOS_REALINT]
        pop     edx
        pop     eax
        ret

do_aux:                                 ; INT 14h AH=01h - send AL on DX
        push    eax
        push    edx
        call    dev_prep
        mov     byte ptr ds:[RMR_EAX],al
        mov     byte ptr ds:[RMR_EAX+1],01h
        mov     byte ptr ds:[RMR_EDX],dh
        mov     al,14h
        call    dword ptr ds:[IOS_REALINT]
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; dev_in - read one byte from the device whose id is in EDX.
;          -> AL, AH = 0.  CF set at end of input.
;          Every register but EAX preserved.
;
; NUL and the printer are write-only, so a read of either is an
; immediate end of file - which is exactly the answer that makes
; "PROG < NUL" behave, and the reason DOS programs test for it.
;
; The serial port does NOT block.  A DOS box waiting forever on a port
; with nothing attached would be indistinguishable from a hung kernel,
; and there is no way out of it from the keyboard, so a port with no
; byte ready reads as end of file too.
;---------------------------------------------------------------------
dev_in:
        cmp     dl,DEV_CON
        je      di_con
        cmp     dl,DEV_AUX
        je      di_aux
di_eof:
        stc                             ; DEV_NUL, DEV_PRN, anything unknown
        ret
di_con:
        call    dos_conin
        clc
        ret
di_aux:
        push    edx
        call    dev_prep                ; INT 14h AH=03h - line status
        mov     byte ptr ds:[RMR_EAX+1],03h
        mov     byte ptr ds:[RMR_EDX],dh
        mov     al,14h
        call    dword ptr ds:[IOS_REALINT]
        test    byte ptr ds:[RMR_EAX+1],1       ; data ready?
        jz      di_none
        call    dev_prep                ; INT 14h AH=02h - receive
        mov     byte ptr ds:[RMR_EAX+1],02h
        mov     byte ptr ds:[RMR_EDX],dh
        mov     al,14h
        call    dword ptr ds:[IOS_REALINT]
        test    byte ptr ds:[RMR_EAX+1],80h     ; AH bit 7 = the read failed
        jnz     di_none
        movzx   eax,byte ptr ds:[RMR_EAX]
        pop     edx
        clc
        ret
di_none:
        pop     edx
        stc
        ret

;---------------------------------------------------------------------
; dev_prep - zero the thunk's register frame.  EAX and the frame are
;            the only things it touches.
;---------------------------------------------------------------------
dev_prep:
        push    ecx
        push    edi
        push    eax
        mov     edi,RMFRAME
        xor     eax,eax
        mov     ecx,RMFRAME_LEN / 4
        rep     stosd
        pop     eax
        pop     edi
        pop     ecx
        ret

;---------------------------------------------------------------------
; sft_dev - EDX = the device id of the SFT entry at ESI.  Only
;             meaningful when SFTF_DEVICE is set; the standard handles
;             start as DEV_CON, which is id zero, so a slot that has
;             never been told otherwise IS the console.
;---------------------------------------------------------------------
sft_dev:
        mov     edx,[esi+SFT_DEVID]
        ret

;--- the table.  Eight blank-padded characters, then the id ----------
DEVT_LEN        EQU     12
dev_tab:
        db  'CON     '
        dd  DEV_CON
        db  'NUL     '
        dd  DEV_NUL
        db  'PRN     '
        dd  DEV_PRN
        db  'LPT1    '
        dd  DEV_PRN
        db  'LPT2    '
        dd  DEV_PRN + 1 * DEV_UNIT
        db  'LPT3    '
        dd  DEV_PRN + 2 * DEV_UNIT
        db  'AUX     '
        dd  DEV_AUX
        db  'COM1    '
        dd  DEV_AUX
        db  'COM2    '
        dd  DEV_AUX + 1 * DEV_UNIT
        db  'COM3    '
        dd  DEV_AUX + 2 * DEV_UNIT
        db  'COM4    '
        dd  DEV_AUX + 3 * DEV_UNIT
        db  0,0,0,0,0,0,0,0             ; CLOCK$ is deliberately absent:
        dd  0                           ; half of it would be a lie, and
                                        ; 2Ah-2Dh are the whole of the truth
dev_name    db  '        '

;=====================================================================
; 3Eh - close handle BX
;=====================================================================
fn3E:
        movzx   ebx,word ptr [ebp+F_EBX]
        cmp     ebx,3                   ; the standard handles stay open
        jb      f3E_ok
        call    sft_get
        jc      f3E_bad
        call    sft_close               ; flushes size/cluster/FAT if dirty
f3E_ok:
        jmp     i21_ok
f3E_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;---------------------------------------------------------------------
; sft_flush - put the SFT entry at ESI on the disk: the size, the first
;             cluster, the stamp and the FAT.  The handle stays open
;             and stays usable, and the DIRTY bit is cleared because it
;             is no longer true.
;
; This is the whole of what a close does apart from freeing the slot,
; which is why 68h and 3Eh are one routine with a flag: two copies of
; "write the directory entry back" would be two places for the stamp
; rule to be got wrong.
;---------------------------------------------------------------------
sft_flush:
        pushad
        mov     byte ptr [sc_keepopen],1
        jmp     short sc_body

;---------------------------------------------------------------------
; sft_close - close the SFT entry at ESI, writing back the directory
;             entry and the FAT if the file was written to.
;---------------------------------------------------------------------
sft_close:
        pushad
        mov     byte ptr [sc_keepopen],0
sc_body:
        test    dword ptr [esi+SFT_FLAGS],SFTF_DIRTY
        jz      sc_clear
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     sc_clear
        call    sft_seldrv              ; back onto the file's volume
        jc      sc_clear                ; volume gone - drop the entry

        mov     edi,esi                 ; EDI = the SFT entry

        ; restore the directory the entry lives in
        xor     al,al
        test    dword ptr [edi+SFT_FLAGS],SFTF_DIRROOT
        jz      sc_ctx
        mov     al,1
sc_ctx:
        mov     edx,[edi+SFT_DIRSTART]
        call    ctx_restore

        mov     ebx,[edi+SFT_DIRENT]
        call    dir_seek                ; ESI -> directory entry
        jc      sc_clear
        mov     eax,[edi+SFT_SIZE]
        mov     [esi+DE_SIZE],eax
        mov     eax,[edi+SFT_CLUSTER]
        call    de_setclus

        ; The stamp is "now" unless 57h said otherwise, and that
        ; exception is the whole point of 57h: an archiver restoring a
        ; file sets the original date on the handle and closing it must
        ; not throw that away in favour of the wall clock.
        test    dword ptr [edi+SFT_FLAGS],SFTF_SETDT
        jnz     sc_keepdt
        call    get_dostime
        mov     [esi+DE_TIME],ax
        call    get_dosdate
        mov     [esi+DE_DATE],ax
        jmp     short sc_stamped
sc_keepdt:
        mov     eax,[edi+SFT_TIME]      ; time and date in one dword
        mov     [esi+DE_TIME],eax
sc_stamped:
        call    fat_dirupdate
        call    fat_flush
        mov     esi,edi
sc_clear:
        cmp     byte ptr [sc_keepopen],0
        je      sc_free
        ; A commit, not a close.  The entry keeps everything it had
        ; except the claim that it has unwritten data in it.
        and     dword ptr [esi+SFT_FLAGS],NOT SFTF_DIRTY
        popad
        ret
sc_free:
        mov     dword ptr [esi+SFT_FLAGS],0
        popad
        ret

sc_keepopen db  0                       ; sft_flush, rather than sft_close
            db  0,0,0

;=====================================================================
; 3Fh - read.  BX = handle, ECX = count, EDX -> buffer.
;       -> EAX = bytes actually read
;=====================================================================
fn3F:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f3F_bad

        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     f3F_dev

;--- plain file -------------------------------------------------------
        call    sft_seldrv              ; the file's volume, not the cwd's
        jc      f3F_ioerr
        mov     ecx,[ebp+F_ECX]         ; bytes wanted
        mov     edi,[ebp+F_EDX]         ; destination
        xor     edx,edx                 ; EDX = bytes delivered

f3F_more:
        or      ecx,ecx
        jz      f3F_eof
        mov     eax,[esi+SFT_SIZE]
        sub     eax,[esi+SFT_POS]
        jbe     f3F_eof                 ; at or past end of file

        push    ecx
        push    edx
        push    edi
        call    file_chunk              ; -> EAX = chunk copied (<= ECX)
        pop     edi
        pop     edx
        pop     ecx
        jc      f3F_ioerr
        or      eax,eax
        jz      f3F_eof
        add     edi,eax
        add     edx,eax
        sub     ecx,eax
        jmp     short f3F_more

f3F_eof:
        mov     [ebp+F_EAX],edx
        jmp     i21_ok
f3F_ioerr:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f3F_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- a device ---------------------------------------------------------
f3F_dev:
        call    sft_dev
        cmp     dl,DEV_CON
        je      f3F_con
        mov     ecx,[ebp+F_ECX]
        mov     edi,[ebp+F_EDX]
        xor     ebx,ebx                 ; bytes delivered
f3Fo_lp:
        cmp     ebx,ecx
        jae     f3Fo_done
        call    dev_in
        jc      f3Fo_done               ; end of input, and NUL is always
        mov     [edi+ebx],al            ; at the end of its input
        inc     ebx
        jmp     short f3Fo_lp
f3Fo_done:
        mov     [ebp+F_EAX],ebx
        jmp     i21_ok

;--- the console: one line, CR turned into CR/LF ----------------------
; Unless 44h/01h put the handle in RAW mode, in which case none of
; that: the caller gets exactly the bytes it asked for, as typed, with
; no echo, no backspace handling, no ^C and no line to wait for.  That
; is what raw mode IS, and it is why an editor asks for it.
f3F_con:
        mov     ecx,[ebp+F_ECX]
        mov     edi,[ebp+F_EDX]
        xor     edx,edx
        test    dword ptr [esi+SFT_FLAGS],SFTF_RAW
        jnz     f3F_conraw
f3Fd_key:
        cmp     edx,ecx
        jae     f3Fd_done
        call    dos_conin
        cmp     al,3                    ; ^C breaks a device read
        jne     f3Fd_nobrk
        call    ctrl_break
        jmp     short f3Fd_key
f3Fd_nobrk:
        cmp     al,13
        je      f3Fd_cr
        cmp     al,8
        je      f3Fd_bs
        mov     [edi+edx],al
        inc     edx
        call    dword ptr ds:[IOS_CONOUT]
        jmp     short f3Fd_key
f3Fd_bs:
        or      edx,edx
        jz      f3Fd_key
        dec     edx
        mov     al,8
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        mov     al,8
        call    dword ptr ds:[IOS_CONOUT]
        jmp     short f3Fd_key
f3Fd_cr:
        mov     byte ptr [edi+edx],13
        inc     edx
        mov     al,13
        call    dword ptr ds:[IOS_CONOUT]
        cmp     edx,ecx
        jae     f3Fd_done
        mov     byte ptr [edi+edx],10
        inc     edx
        mov     al,10
        call    dword ptr ds:[IOS_CONOUT]
f3Fd_done:
        mov     [ebp+F_EAX],edx
        jmp     i21_ok

;--- and the same console with the cooking turned off -----------------
; A zero ASCII code is an extended key - a function or cursor key -
; and raw mode is the mode in which a program wants those.  DOS
; reports one as two bytes, a zero followed by the scan code, and the
; second byte is delivered on the NEXT read if this one has no room
; left, which is why the pending byte is a variable and not a jump.
f3F_conraw:
        or      ecx,ecx
        jz      f3Fd_done
f3Fr_key:
        cmp     edx,ecx
        jae     f3Fd_done
        cmp     byte ptr [con_pending],0
        je      f3Fr_read
        mov     al,[con_pending]
        mov     byte ptr [con_pending],0
        jmp     short f3Fr_store
f3Fr_read:
        call    dos_conin               ; idles for the DOS box while it waits
        or      al,al
        jnz     f3Fr_store
        mov     [con_pending],ah        ; extended: the scan code follows
f3Fr_store:
        mov     [edi+edx],al
        inc     edx
        jmp     short f3Fr_key

;---------------------------------------------------------------------
; file_chunk - move bytes out of the cluster under SFT_POS.
;
;   ESI = SFT entry, EDI = destination, ECX = bytes still wanted
;   -> EAX = bytes copied (bounded by cluster edge and EOF), CF on I/O
;      error.  Advances SFT_POS.
;---------------------------------------------------------------------
file_chunk:
        push    ebx
        push    ecx
        push    edx
        push    edi

        ; which cluster of the chain does POS sit in?
        mov     eax,[esi+SFT_POS]
        xor     edx,edx
        mov     ebx,[fat_spc]
        shl     ebx,9                   ; cluster size in bytes
        div     ebx                     ; EAX = cluster index, EDX = offset

        ; walk there, restarting if we are already past it
        cmp     eax,[esi+SFT_HANDLE]    ; SFT_HANDLE = cached chain index
        jae     fc_forward
        push    ecx
        mov     ecx,[esi+SFT_CLUSTER]
        mov     [esi+SFT_CURCLUS],ecx
        pop     ecx
        mov     dword ptr [esi+SFT_HANDLE],0
fc_forward:
        push    eax
fc_walk:
        mov     ebx,[esi+SFT_HANDLE]
        cmp     ebx,[esp]
        jae     fc_there
        push    eax
        mov     eax,[esi+SFT_CURCLUS]
        call    fat_next
        mov     [esi+SFT_CURCLUS],eax
        cmp     eax,[fat_eoc]
        jae     fc_chain_end
        pop     eax
        inc     dword ptr [esi+SFT_HANDLE]
        jmp     short fc_walk
fc_chain_end:
        pop     eax
        pop     eax
        xor     eax,eax                 ; chain shorter than SIZE claims
        jmp     short fc_out
fc_there:
        pop     eax

        ; make sure that cluster is in clus_buf
        mov     eax,[esi+SFT_CURCLUS]
        cmp     eax,[clusbuf_clus]
        je      fc_cached
        mov     edi,OFFSET clus_buf
        call    fat_readclus
        jc      fc_ioerr
        mov     [clusbuf_clus],eax
fc_cached:

        ; how much can this one cluster give us?
        mov     ebx,[fat_spc]
        shl     ebx,9
        sub     ebx,edx                 ; room left in the cluster
        mov     eax,[esi+SFT_SIZE]
        sub     eax,[esi+SFT_POS]       ; room left in the file
        cmp     ebx,eax
        jbe     fc_lim1
        mov     ebx,eax
fc_lim1:
        cmp     ebx,ecx                 ; and what the caller wanted
        jbe     fc_lim2
        mov     ebx,ecx
fc_lim2:

        ; copy it out
        pop     edi                     ; the caller's destination
        push    edi
        push    esi
        mov     ecx,ebx
        lea     esi,[clus_buf+edx]
        rep     movsb
        pop     esi

        add     [esi+SFT_POS],ebx
        mov     eax,ebx
fc_out:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
fc_ioerr:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; 40h - write.  BX = handle, ECX = count, EDX -> data.
;       ECX = 0 truncates the file at the current position (DOS rule).
;       -> EAX = bytes written (less than ECX means the disk filled).
;=====================================================================
fn40:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f40_bad

        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jz      f40_file

;--- a device ---------------------------------------------------------
; Every byte is reported written whatever the device did with it,
; which is the right answer for all four: the screen and the bit
; bucket cannot refuse, and a printer or a serial line that is not
; listening has no way to say so through this call.
        call    sft_dev
        mov     esi,[ebp+F_EDX]
        mov     ecx,[ebp+F_ECX]
        or      ecx,ecx
        jz      f40_devdone
f40_lp:
        mov     al,[esi]
        call    dev_out
        inc     esi
        dec     ecx
        jnz     f40_lp
f40_devdone:
        mov     eax,[ebp+F_ECX]
        mov     [ebp+F_EAX],eax
        jmp     i21_ok

;--- plain file -------------------------------------------------------
f40_file:
        cmp     dword ptr [esi+SFT_MODE],0
        je      f40_deny                ; opened read-only
        call    sft_seldrv              ; the file's volume, not the cwd's
        jc      f40_ioerr

        mov     ecx,[ebp+F_ECX]
        or      ecx,ecx
        jz      f40_trunc

        mov     edi,[ebp+F_EDX]         ; source data
        xor     edx,edx                 ; bytes done
f40_more:
        or      ecx,ecx
        jz      f40_fin
        push    ecx
        push    edx
        push    edi
        call    file_wchunk             ; -> EAX = chunk written
        pop     edi
        pop     edx
        pop     ecx
        jc      f40_ioerr
        or      eax,eax
        jz      f40_fin                 ; disk full - report the shortfall
        add     edi,eax
        add     edx,eax
        sub     ecx,eax
        jmp     short f40_more
f40_fin:
        or      dword ptr [esi+SFT_FLAGS],SFTF_DIRTY
        mov     [ebp+F_EAX],edx
        jmp     i21_ok

;--- ECX = 0: truncate at POS ----------------------------------------
f40_trunc:
        mov     eax,[esi+SFT_POS]
        cmp     eax,[esi+SFT_SIZE]
        jae     f40_setsz               ; truncating forward = just resize

        or      eax,eax
        jnz     f40_partial
        mov     eax,[esi+SFT_CLUSTER]   ; cut to nothing
        call    fat_freechain
        mov     dword ptr [esi+SFT_CLUSTER],0
        mov     dword ptr [esi+SFT_CURCLUS],0
        mov     dword ptr [esi+SFT_HANDLE],0
        jmp     short f40_setsz

f40_partial:
        ; keep ceil(POS / clustersize) clusters, free the rest
        mov     ebx,[fat_spc]
        shl     ebx,9
        dec     eax
        xor     edx,edx
        div     ebx                     ; EAX = index of the last kept cluster
        mov     ecx,eax
        mov     eax,[esi+SFT_CLUSTER]
f40_walk:
        or      ecx,ecx
        jz      f40_cut
        call    fat_next
        dec     ecx
        jmp     short f40_walk
f40_cut:
        mov     ebx,eax                 ; EBX = last kept cluster
        call    fat_next                ; EAX = first cluster to free
        call    fat_freechain
        mov     eax,ebx
        mov     edx,[fat_eocv]
        call    fat_setnext             ; terminate the kept chain
        mov     eax,[esi+SFT_CLUSTER]
        mov     [esi+SFT_CURCLUS],eax   ; cluster cache is now suspect
        mov     dword ptr [esi+SFT_HANDLE],0
f40_setsz:
        mov     eax,[esi+SFT_POS]
        mov     [esi+SFT_SIZE],eax
        or      dword ptr [esi+SFT_FLAGS],SFTF_DIRTY
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [ebp+F_EAX],0
        jmp     i21_ok

f40_ioerr:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f40_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f40_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

;---------------------------------------------------------------------
; file_wchunk - write bytes into the cluster under SFT_POS.
;
;   ESI = SFT entry, EDI = source, ECX = bytes still wanted
;   -> EAX = bytes written this chunk (0 = disk full), CF = I/O error.
;      Advances SFT_POS, grows SFT_SIZE, extends the chain as needed.
;---------------------------------------------------------------------
file_wchunk:
        push    ebx
        push    ecx
        push    edx
        push    edi

        ; first cluster of a brand-new file?
        cmp     dword ptr [esi+SFT_CLUSTER],0
        jne     fw_havechain
        call    fat_alloc
        jc      fw_full
        mov     [esi+SFT_CLUSTER],eax
        mov     [esi+SFT_CURCLUS],eax
        mov     dword ptr [esi+SFT_HANDLE],0
fw_havechain:

        ; which cluster index does POS live in?
        mov     eax,[esi+SFT_POS]
        xor     edx,edx
        mov     ebx,[fat_spc]
        shl     ebx,9
        div     ebx                     ; EAX = target index, EDX = offset

        cmp     eax,[esi+SFT_HANDLE]
        jae     fw_forward
        push    ecx
        mov     ecx,[esi+SFT_CLUSTER]   ; restart from the front
        mov     [esi+SFT_CURCLUS],ecx
        pop     ecx
        mov     dword ptr [esi+SFT_HANDLE],0
fw_forward:
        push    eax                     ; [esp] = target index
fw_walk:
        mov     ebx,[esi+SFT_HANDLE]
        cmp     ebx,[esp]
        jae     fw_there
        mov     eax,[esi+SFT_CURCLUS]
        call    fat_next
        cmp     eax,[fat_eoc]
        jb      fw_link
        ; chain ends before the target: extend it
        call    fat_alloc
        jc      fw_full_pop
        push    edx
        push    eax
        mov     edx,eax
        mov     eax,[esi+SFT_CURCLUS]
        call    fat_setnext             ; old tail -> new cluster
        pop     eax
        pop     edx
        ; a hole cluster the caller will skip past must read as zeros
        call    fat_zeroclus
fw_link:
        mov     [esi+SFT_CURCLUS],eax
        inc     dword ptr [esi+SFT_HANDLE]
        jmp     short fw_walk
fw_there:
        pop     eax

        ; decide whether this cluster needs a read-modify-write
        mov     ebx,[fat_spc]
        shl     ebx,9
        mov     eax,ebx
        sub     eax,edx                 ; room left in the cluster
        cmp     eax,ecx
        jbe     fw_len_ok
        mov     eax,ecx
fw_len_ok:                              ; EAX = bytes this chunk
        cmp     eax,ebx
        je      fw_overwrite            ; full cluster - no preload needed

        push    eax
        mov     eax,[esi+SFT_CURCLUS]
        cmp     eax,[clusbuf_clus]
        je      fw_preloaded
        push    edi
        mov     edi,OFFSET clus_buf
        call    fat_readclus
        pop     edi
        jc      fw_ioerr_pop
        mov     [clusbuf_clus],eax
fw_preloaded:
        pop     eax
        jmp     short fw_copy

fw_overwrite:
        push    eax
        mov     eax,[esi+SFT_CURCLUS]
        mov     [clusbuf_clus],eax      ; buffer will hold this cluster
        pop     eax

fw_copy:
        ; splice the caller's bytes into clus_buf at EDX, write it out
        push    esi
        push    ecx
        push    eax
        mov     esi,edi                 ; source = caller
        lea     edi,[clus_buf+edx]
        mov     ecx,eax
        rep     movsb
        pop     eax
        pop     ecx
        pop     esi

        push    eax
        mov     eax,[esi+SFT_CURCLUS]
        mov     edi,OFFSET clus_buf
        call    fat_writeclus
        pop     eax
        jc      fw_ioerr

        add     [esi+SFT_POS],eax
        mov     ebx,[esi+SFT_POS]
        cmp     ebx,[esi+SFT_SIZE]
        jbe     fw_out
        mov     [esi+SFT_SIZE],ebx
fw_out:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret

fw_full_pop:
        pop     eax
fw_full:
        xor     eax,eax                 ; 0 bytes = disk full
        jmp     short fw_out
fw_ioerr_pop:
        pop     eax
fw_ioerr:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; 42h - lseek.  BX = handle, AL = origin, ECX = offset (signed for
;       origins 1 and 2).  -> EAX = new position.
;=====================================================================
fn42:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f42_bad
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     f42_bad

        mov     eax,[ebp+F_ECX]
        mov     cl,[ebp+F_EAX]          ; origin
        or      cl,cl
        jz      f42_set
        cmp     cl,1
        je      f42_cur
        cmp     cl,2
        je      f42_end
        mov     ax,ERR_FUNC
        jmp     i21_fail
f42_cur:
        add     eax,[esi+SFT_POS]
        jmp     short f42_set
f42_end:
        add     eax,[esi+SFT_SIZE]
f42_set:
        test    eax,80000000h           ; refuse negative positions
        jnz     f42_range
        mov     [esi+SFT_POS],eax
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
f42_range:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f42_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;=====================================================================
; 44h - IOCTL.
;
; Until v0.31 this answered AL=0 and returned "invalid function" for
; every other subfunction, and that was the worst-behaved gap left in
; the API - not because the calls are important individually, but
; because of HOW they fail.  A program that asks 4401h to put the
; console in raw mode and is refused does not stop; it carries on with
; a console that is still cooked, and every key it reads afterwards has
; been line-edited and echoed behind its back.  The bug shows up as a
; keyboard that behaves strangely in one program, which is about as far
; from "IOCTL subfunction 01h is missing" as a symptom can get.
;
; Implemented here:
;   00h  get device information             (BX = handle -> DX)
;   01h  set it                             (BX = handle, DX)
;   06h  get input status                   (BX -> AL = FFh ready)
;   07h  get output status                  (BX -> AL = FFh ready)
;   08h  is the drive removable             (BL = drive -> AX)
;   09h  is the drive remote                (BL = drive -> DX)
;   0Bh  set sharing retry count            (accepted, no-op)
;   0Eh  get logical drive map              (BL = drive -> AL = 0)
;
; Still refused: 02h-05h and 0Ch/0Dh, the generic IOCTL read and write
; pairs.  Those pass a driver-defined request packet to a driver, and
; PM-DOS has no installable drivers to pass one to - "invalid function"
; is the truthful answer rather than a gap.
;=====================================================================
fn44:
        mov     al,[ebp+F_EAX]
        or      al,al
        jz      f44_get
        cmp     al,01h
        je      f44_set
        cmp     al,06h
        je      f44_instat
        cmp     al,07h
        je      f44_outstat
        cmp     al,08h
        je      f44_removbl
        cmp     al,09h
        je      f44_remote
        cmp     al,0Bh
        je      f44_retry
        cmp     al,0Eh
        je      f44_drvmap
        jmp     f44_unsup

;--- 00h: what kind of handle is this? --------------------------------
f44_get:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f44_bad
        mov     dx,0                    ; plain file on drive 0
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jz      f44_have

        ; A character device, and WHICH one matters: bit 1 is "this is
        ; the console output", bit 2 is NUL, bit 6 is "not at end of
        ; file".  A program that pipes to NUL and asks about it is
        ; entitled to be told, and MORE-like filters read bit 1 to
        ; decide whether they are talking to a screen.
        mov     dx,80C0h                ; character device, data available
        mov     eax,[esi+SFT_DEVID]
        cmp     al,DEV_CON
        je      f44_con
        cmp     al,DEV_NUL
        je      f44_nul
        jmp     short f44_raw           ; PRN and AUX carry neither bit
f44_con:
        mov     dx,80D3h                ; CON, in and out
        jmp     short f44_raw
f44_nul:
        mov     dx,80C4h                ; the bit bucket
f44_raw:
        ; Bit 5 is the one 01h can change, so it has to be REPORTED from
        ; the handle rather than from the device's fixed description -
        ; a program that sets raw mode and reads it back to check is
        ; doing the ordinary thing.
        test    dword ptr [esi+SFT_FLAGS],SFTF_RAW
        jz      f44_have
        or      dx,0020h
f44_have:
        mov     [ebp+F_EDX],dx
        jmp     i21_ok

;--- 01h: raw mode, which is the only bit that means anything here ----
; DH must be zero (DOS says so, and it is how the call is told apart
; from a block-device 01h), and only a character device can be set.
;
; Raw - "binary" - mode means the driver stops interpreting: no line
; editing, no echo, no ^C, and no ^Z ending the input.  For CON that is
; a real behavioural change and 3Fh honours it below; for the other
; devices there was never any cooking to turn off, and the bit is
; remembered so that a 00h read-back agrees with what was set.
f44_set:
        cmp     byte ptr [ebp+F_EDX+1],0
        jne     f44_unsup
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f44_bad
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jz      f44_unsup               ; a file has no device data to set
        and     dword ptr [esi+SFT_FLAGS],NOT SFTF_RAW
        test    byte ptr [ebp+F_EDX],20h
        jz      i21_ok
        or      dword ptr [esi+SFT_FLAGS],SFTF_RAW
        jmp     i21_ok

;--- 06h: is there anything to read? ----------------------------------
; FFh means ready, 00h means not.  For a FILE the answer is "not at the
; end yet", and that is the reading that makes this call useful: it is
; how a filter asks whether its input has run out without consuming a
; byte to find out.
f44_instat:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f44_bad
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jz      f44_infile
        call    sft_dev
        cmp     dl,DEV_CON
        jne     f44_ready               ; NUL is always ready to say "end",
                                        ; and a port is asked when read
        call    dword ptr ds:[IOS_CONSTAT]
        jz      f44_notready
        jmp     short f44_ready
f44_infile:
        mov     eax,[esi+SFT_POS]
        cmp     eax,[esi+SFT_SIZE]
        jb      f44_ready
f44_notready:
        mov     byte ptr [ebp+F_EAX],00h
        jmp     i21_ok
f44_ready:
        mov     byte ptr [ebp+F_EAX],0FFh
        jmp     i21_ok

;--- 07h: can it be written to? ---------------------------------------
; Always, for everything here.  There is no spooler and no driver queue
; to be full, and a disk that is full fails the write rather than
; refusing it in advance.  The handle is still checked, because a
; program is entitled to find out that its handle is bad.
f44_outstat:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f44_bad
        jmp     short f44_ready

;--- 08h: can the media be changed? -----------------------------------
; AX = 0 removable, 1 fixed.  Decided by the BIOS drive number of the
; volume, which is what 80h has meant since the first hard disk, and
; not by the partition or the file system - two volumes on one disk
; give the same answer because they are on the same physical thing.
;
; This is what an installer asks before it says "insert disk 2", and
; what a program that caches a directory asks before it decides it may.
f44_removbl:
        call    f44_drive
        jc      f44_baddrv
        cmp     byte ptr [fat_drive],80h
        jae     f44_fixed
        mov     word ptr [ebp+F_EAX],0
        jmp     i21_ok
f44_fixed:
        mov     word ptr [ebp+F_EAX],1
        jmp     i21_ok

;--- 09h: is the drive somebody else's? -------------------------------
; DX is the device attribute word, and bit 12 is "remote".  Everything
; here is local, so it is clear - but the call still has to VALIDATE
; the drive, because "no" and "there is no such drive" are different
; answers and a program that gets the first for a drive that does not
; exist will go on to use it.
f44_remote:
        call    f44_drive
        jc      f44_baddrv
        mov     dword ptr [ebp+F_EDX],0
        jmp     i21_ok

;--- 0Bh: how hard to try when a file is locked -----------------------
; Accepted and discarded.  PM-DOS is single-tasking and has no record
; locking, so a retry count describes a wait that can never happen -
; but refusing it would fail programs that set one defensively at
; startup and never lock anything.
f44_retry:
        jmp     i21_ok

;--- 0Eh: does this drive answer to another letter? -------------------
; AL = 0 means "it has only this one".  Since v0.26 a letter IS a
; volume and no volume has two letters, so zero is the whole truth.
f44_drvmap:
        call    f44_drive
        jc      f44_baddrv
        mov     byte ptr [ebp+F_EAX],0
        jmp     i21_ok

;---------------------------------------------------------------------
; f44_drive - the drive in BL (0 = default, 1 = A:), selected.
;             CF set if there is no such drive.
;---------------------------------------------------------------------
f44_drive:
        mov     al,[ebp+F_EBX]
        or      al,al
        jnz     f44d_expl
        mov     al,[cur_drive]
        jmp     short f44d_sel
f44d_expl:
        dec     al
        cmp     al,MAX_DRIVES
        jae     f44d_no
f44d_sel:
        call    vol_select
        ret
f44d_no:
        stc
        ret

f44_baddrv:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail
f44_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f44_unsup:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; 45h - DUP.  BX = handle -> AX = a second handle to the same open
;       file.
;
; Handles ARE global-table slots here, so there is nothing to
; refcount: the new handle is a full COPY of the slot, position and
; all, and the pair track the file independently from then on.  That
; is less than DOS promises (its dups share one SFT entry and move
; together) but exactly enough for what DUP exists for in a
; single-tasking DOS: COMMAND parking a standard handle while a
; redirection borrows the slot.  The owner rides along in the copy,
; so the duplicate belongs to whoever opened the original.
;=====================================================================
fn45:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f45_bad
        push    esi                     ; the source slot
        call    sft_new                 ; -> AX = handle, ESI = new slot
        pop     edx
        jc      f45_full
        mov     edi,esi
        mov     esi,edx
        mov     ecx,SFT_LEN / 4
        rep     movsd
        mov     [ebp+F_EAX],ax
        jmp     i21_ok
f45_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f45_full:
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail

;=====================================================================
; 46h - FORCEDUP.  Make handle CX refer to what handle BX refers to,
;       properly closing (and flushing) whatever CX held.  This is
;       the other half of redirection: COMMAND points 0 or 1 at a
;       file, runs the command, and points it back - the close of
;       the file it displaces is what writes the directory entry.
;=====================================================================
fn46:
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        jc      f46_bad
        movzx   ecx,word ptr [ebp+F_ECX]
        cmp     ecx,MAX_HANDLES
        jae     f46_bad
        cmp     ecx,ebx
        je      f46_same
        mov     edx,esi                 ; EDX = the source slot
        mov     esi,ecx
        imul    esi,SFT_LEN
        add     esi,OFFSET sft_tbl      ; ESI = the target slot
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      f46_copy
        call    sft_close               ; flush + free what it held
f46_copy:
        mov     edi,esi
        mov     esi,edx
        mov     ecx,SFT_LEN / 4
        rep     movsd
f46_same:
        jmp     i21_ok
f46_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;=====================================================================
; 47h - get current directory of drive DL (0 = default, 1 = A:) into
;       the 64-byte buffer at ESI (DOS style: no drive, no leading
;       backslash, root = "")
;=====================================================================
fn47:
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     f47_expl
        mov     al,[cur_drive]
        jmp     short f47_sel
f47_expl:
        dec     al
        cmp     al,MAX_DRIVES
        jae     f47_bad
f47_sel:
        call    vol_select
        jc      f47_bad
        mov     edi,[ebp+F_ESI]
        call    cwd_string
        jmp     i21_ok
f47_bad:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;=====================================================================
; 48h - allocate EBX bytes -> EAX = address
;       (flat deviation: bytes, not paragraphs)
;
; WHICH HEAP DEPENDS ON WHO IS ASKING, and it has to.  The kernel
; allocates out of the arena above 2MB, which is where the arena has
; always been; a native program cannot see that memory at all now, so
; an address out of it would be a pointer that faults the first time it
; is used - worse than a refusal, because the program would carry it
; around until the damage was somewhere else.  A program's memory comes
; out of its own slot, between the end of its BSS and its stack.
;
; That is what DOS did with a .COM, which owned its segment and
; allocated from inside it.  It also parks correctly: EXEC copies the
; whole slot away when a program nests, so a program's heap travels
; with it and needs neither an owner nor a sweep.
;=====================================================================
fn48:
        mov     ebx,[ebp+F_EBX]
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        je      f48_slot
        call    arena_alloc
        jc      f48_nomem
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
f48_slot:
        call    r3h_alloc
        jc      f48_nomem
        mov     [ebp+F_EAX],eax
        jmp     i21_ok
f48_nomem:
        mov     [ebp+F_EBX],eax         ; largest possible
        mov     ax,ERR_NOMEM
        jmp     i21_fail

;=====================================================================
; 49h - free the block at EDX
;=====================================================================
fn49:
        mov     edx,[ebp+F_EDX]
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        je      f49_slot
        call    arena_free
        jc      f49_bad
        jmp     i21_ok
f49_slot:
        call    r3h_free
        jc      f49_bad
        jmp     i21_ok
f49_bad:
        mov     ax,ERR_BADMCB
        jmp     i21_fail

;=====================================================================
; 4Ah - resize the block at EDX to EBX bytes
;
; A DOS program's first act is traditionally to shrink its OWN block -
; "4Ah on your PSP" - so that the memory it was handed but does not
; want is available to a child.  There is nothing to shrink here: a
; PM-DOS program owns a fixed 512K slot which is not an arena block at
; all, and EXEC parks that whole slot rather than allocating round it.
; So the call is answered rather than refused, because a program that
; gets an error from it will usually give up on the spot.
;=====================================================================
fn4A:
        mov     edx,[ebp+F_EDX]
        mov     ebx,[ebp+F_EBX]
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        je      f4A_prog
        cmp     edx,[cur_psp]
        je      f4A_slot
        call    arena_resize
        jc      f4A_no
        jmp     i21_ok

;--- from a program, in its own terms: its PSP is at a known offset and
;--- everything else is a block out of its slot heap.
f4A_prog:
        cmp     edx,SLOT_PSP
        je      f4A_whole
        call    r3h_resize
        jc      f4A_no
        jmp     i21_ok
f4A_whole:
        mov     eax,[r3_size]
        cmp     ebx,eax
        ja      f4A_no
        jmp     i21_ok

f4A_slot:
        mov     eax,[edx+PSP_MEMTOP]    ; what the slot actually holds
        sub     eax,edx
        cmp     ebx,eax
        ja      f4A_no                  ; asking for MORE than the slot
        jmp     i21_ok

f4A_no:
        mov     [ebp+F_EBX],eax
        mov     ax,ERR_NOMEM
        jmp     i21_fail

;=====================================================================
; 50h / 51h / 62h - set and get the current PSP.
;
; 51h and 62h are the same call; 51h came first and was undocumented
; for years, 62h is the documented one, and programs use both.  50h
; exists for the debugger-and-TSR pattern of borrowing another
; program's identity for a few calls.
;
; It moves [cur_psp] only.  Ownership of handles and memory is by
; OWNER ID here, so a program cannot acquire another's files by
; pointing 50h at its PSP - which is a happy accident of the design
; rather than a check, but worth knowing.
;=====================================================================
fn50:
        mov     eax,[ebp+F_EBX]
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     f50_flat
        call    r3_flat                 ; the caller means an offset
        jc      f50_bad
f50_flat:
        mov     [cur_psp],eax
        jmp     i21_ok
f50_bad:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

fn51:
fn62:
        mov     eax,[cur_psp]
        call    i21_toprog
        mov     [ebp+F_EBX],eax
        jmp     i21_ok

;=====================================================================
; 52h - "list of lists" -> EBX = kernel information block.
;
; DOS's most-used undocumented call: the internal pointers a tool like
; MEM or DEBUG needs and no documented call provides.  Its one field
; with a fixed meaning across DOS versions is the pointer to the FIRST
; MEMORY BLOCK, which real DOS put in the word just BELOW the returned
; address - so PM-DOS puts the flat equivalent in the dword just below
; it, and MEM.EXE walks the arena from there.  The rest is PM-DOS's
; own shape; matching real DOS's DPB and SFT layout would be a
; fiction, since neither structure exists here.
;=====================================================================
fn52:
        mov     dword ptr [ebp+F_EBX],OFFSET dos_lol
        jmp     i21_ok

;=====================================================================
; 34h - get the address of the InDOS flag -> EBX.
;
; THE CALL A TSR CANNOT DO WITHOUT.  A pop-up wakes on a keystroke or
; a timer tick, which is to say from inside an interrupt, which may
; have arrived while the interrupted program was already somewhere
; inside INT 21h.  DOS is not re-entrant, so the TSR reads this flag
; to find out whether making a DOS call right now would corrupt the
; call already in progress, and waits for a quiet moment if it would.
;
; PM-DOS has been able to make a program resident since v0.25, so
; claiming TSR support without this was claiming half a thing.
;
; A 16-bit guest is answered by the VDM with the same address split
; into a far pointer - see vdm_indos - which is the reason the flag
; lives below 1MB at all.
;=====================================================================
fn34:
        mov     dword ptr [ebp+F_EBX],DOS_INDOS
        jmp     i21_ok

;=====================================================================
; 57h - get or set a file's date and time.  BX = handle.
;
;   AL=00h  get -> CX = time, DX = date
;   AL=01h  set from CX = time, DX = date
;
; Every archiver, every compiler and every MAKE in existence uses
; this: an extractor restores the timestamp it stored, and a build
; tool compares one against another to decide what is out of date.
; Without it PKUNZIP stamps everything with the wall clock and MAKE
; rebuilds the world.
;
; The stamp rides in the handle rather than being read back out of the
; directory on demand, because the directory entry does not yet hold
; what the program just set - the write-back happens at close.
;=====================================================================
fn57:
        mov     al,[ebp+F_EAX]
        cmp     al,1
        ja      f57_badfn
        push    eax
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get
        pop     eax
        jc      f57_bad
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     f57_bad                 ; a device has no timestamp, and
                                        ; DOS calls that a bad handle
        or      al,al
        jnz     f57_set
        movzx   eax,word ptr [esi+SFT_TIME]
        mov     [ebp+F_ECX],ax
        movzx   eax,word ptr [esi+SFT_DATE]
        mov     [ebp+F_EDX],ax
        jmp     i21_ok
f57_set:
        mov     ax,[ebp+F_ECX]
        mov     [esi+SFT_TIME],ax
        mov     ax,[ebp+F_EDX]
        mov     [esi+SFT_DATE],ax
        ; DIRTY as well as SETDT: a handle opened only for reading has
        ; nothing else to write back, and without this the close would
        ; decide there was nothing to do and drop the new stamp.
        or      dword ptr [esi+SFT_FLAGS],SFTF_DIRTY OR SFTF_SETDT
        jmp     i21_ok
f57_bad:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f57_badfn:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; 59h - get extended error information.
;
;   -> AX = the extended error code
;      BH = class, BL = suggested action, CH = locus
;
; What a program calls the moment something fails, to find out what
; actually went wrong and what to do about it - and until now PM-DOS
; answered "invalid function", so every caller learned nothing from a
; failure except that there had been one.
;
; The code is simply the last one i21_fail recorded, which is the one
; place in the kernel every failure passes through.  It is NOT cleared
; by a successful call, exactly as in real DOS: 59h is only ever asked
; immediately after a failure, and clearing on success would mean
; every intervening housekeeping call wiped the answer.
;
; The class, action and locus are a translation of that code rather
; than a second thing to keep in step, so they can never disagree with
; it.  Anything not in the table is class 0Dh (unknown), which is the
; honest answer and is also what DOS returns for its own oddities.
;=====================================================================
fn59:
        movzx   eax,word ptr [last_err]
        mov     [ebp+F_EAX],ax
        mov     esi,OFFSET err_xlat
f59_scan:
        movzx   ecx,byte ptr [esi]
        or      ecx,ecx
        jz      f59_unknown             ; end of table
        cmp     cx,ax
        je      f59_have
        add     esi,4
        jmp     short f59_scan
f59_unknown:
        mov     esi,OFFSET err_unknown
f59_have:
        mov     bh,[esi+1]              ; class
        mov     bl,[esi+2]              ; action
        mov     [ebp+F_EBX],bx
        mov     ch,[esi+3]              ; locus
        mov     byte ptr [ebp+F_ECX+1],ch
        jmp     i21_ok

;--- code, class, action, locus.  A zero code ends it -----------------
; Classes:  01 out of resource  02 temporary  03 authorization
;           04 internal  05 hardware  06 system  07 application
;           08 not found  09 bad format  0A locked  0B media
;           0C already exists  0D unknown
; Actions:  01 retry  02 delayed retry  03 re-enter input
;           04 abort after cleanup  05 immediate abort  06 ignore
;           07 retry after user intervention
; Locus:    01 unknown  02 block device  03 network  04 serial  05 memory
err_xlat:
        db  ERR_FUNC,      07h, 05h, 01h
        db  ERR_NOFILE,    08h, 03h, 02h
        db  ERR_NOPATH,    08h, 03h, 02h
        db  ERR_NOHANDLES, 01h, 04h, 01h
        db  ERR_ACCESS,    03h, 04h, 02h
        db  ERR_BADHANDLE, 07h, 04h, 01h
        db  ERR_BADMCB,    04h, 05h, 05h
        db  ERR_NOMEM,     01h, 04h, 05h
        db  ERR_BADBLOCK,  07h, 05h, 05h
        db  ERR_EXEFMT,    09h, 04h, 01h
        db  ERR_BADDRIVE,  08h, 03h, 02h
        db  ERR_NOTSAME,   07h, 04h, 02h
        db  ERR_BADFUNC,   07h, 05h, 01h
        db  ERR_EXISTS,    0Ch, 04h, 02h
        db  ERR_DISKFULL,  01h, 04h, 02h
        db  0
err_unknown:
        db  0,             0Dh, 05h, 01h

;=====================================================================
; 58h - memory allocation strategy and the UMB link state.
;
;   AL=00h  get strategy      -> AX
;   AL=01h  set strategy from BL
;   AL=02h  get UMB link state -> AL
;   AL=03h  set UMB link state from BL
;
; The strategy is real: arena_alloc honours first / best / last fit.
; The 40h and 80h "high memory" bits are accepted and ignored, which
; is the only honest answer on a machine with no upper memory blocks -
; and the UMB link state is therefore always 0 and refuses to become
; 1.  A program that asks to be put in upper memory gets told there
; is none, rather than being told it succeeded.
;=====================================================================
fn58:
        mov     al,[ebp+F_EAX]
        or      al,al
        jz      f58_getstrat
        cmp     al,1
        je      f58_setstrat
        cmp     al,2
        je      f58_getumb
        cmp     al,3
        je      f58_setumb
        jmp     i21_unsup

f58_getstrat:
        mov     eax,[alloc_strat]
        mov     [ebp+F_EAX],ax
        jmp     i21_ok
f58_setstrat:
        mov     al,[ebp+F_EBX]
        and     al,3                    ; the high-memory bits mean
        cmp     al,3                    ; nothing here
        je      f58_bad
        movzx   eax,al
        mov     [alloc_strat],eax
        jmp     i21_ok
f58_getumb:
        mov     word ptr [ebp+F_EAX],0  ; never linked, there being none
        jmp     i21_ok
f58_setumb:
        cmp     byte ptr [ebp+F_EBX],0
        jne     f58_bad                 ; "link them in" - there are none
        jmp     i21_ok
f58_bad:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;---------------------------------------------------------------------
; The 52h information block.  The dword BELOW the label is the one
; field whose position is a contract - see fn52.
;---------------------------------------------------------------------
            dd  ARENA_BASE              ; [dos_lol-4] = first arena header
dos_lol:
            dd  OFFSET sft_tbl          ; +00  the handle table
            dd  MAX_HANDLES             ; +04  entries in it
            dd  OFFSET arena_top        ; +08  -> top of the arena
            dd  OFFSET cur_psp          ; +0C  -> the current PSP
            dd  PMDOS_MINOR * 256 + PMDOS_MAJOR    ; +10  the TRUE version

;=====================================================================
; 56h - rename / move.  EDX -> old path, EDI -> new path.  The entry
;       moves between directories when the paths call for it.
;=====================================================================
fn56:
        ; Neither end may be a device.  The source because there is
        ; nothing there to move, and the destination because a rename
        ; is the other way to get a reserved name onto a disk.
        mov     esi,[ebp+F_EDX]
        call    dev_deny
        jnc     i21_fail
        mov     esi,[ebp+F_EDI]
        call    dev_deny
        jnc     i21_fail

        ; resolve the old path and copy its whole entry aside
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f56_nofile
        cmp     byte ptr [rp_noname],0
        jne     f56_deny
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f56_deny

        mov     byte ptr [srch_attr],ATTR_DIR   ; files and directories
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f56_nofile
        call    lfn_tomb                ; the OLD name's chain: the new
        call    dir_seek                ; 8.3 name would break its
        jc      f56_nofile              ; checksum into an orphan anyway

        push    esi
        mov     edi,OFFSET ren_entry    ; the 32 bytes, verbatim
        mov     ecx,32
        rep     movsb
        pop     esi
        call    ctx_save                ; where the old entry lives
        mov     [mk_par_isroot],al
        mov     [mk_par_start],edx
        mov     [mk_slot],ebx
        mov     al,[fat_drive]          ; and on which volume
        mov     [ren_drv],al

        ; resolve the new path; its parent may be a different dir
        ; (but must be the same volume - a rename never copies data)
        mov     esi,[ebp+F_EDI]
        call    resolve_parent
        jc      f56_nopath
        mov     al,[fat_drive]
        cmp     al,[ren_drv]
        jne     f56_notsame
        cmp     byte ptr [rp_noname],0
        jne     f56_deny
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f56_deny

        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jnc     f56_exists

        ; moving a directory would orphan its ".." - rename in place only
        test    byte ptr [ren_entry+DE_ATTR],ATTR_DIR
        jz      f56_mv_ok
        call    ctx_save
        cmp     al,[mk_par_isroot]
        jne     f56_deny
        cmp     edx,[mk_par_start]
        jne     f56_deny
f56_mv_ok:
        cmp     byte ptr [rp_flong],0   ; a long NEW name gets a fresh
        je      f56_slot                ; alias and chain in its home
        mov     esi,[rp_fptr]
        mov     ecx,[rp_flen]
        call    lfn_alias
        jc      f56_deny
        mov     esi,[rp_fptr]
        mov     ecx,[rp_flen]
        call    lfn_store
        jc      f56_deny
        jmp     short f56_fill
f56_slot:
        call    fat_findfree            ; slot in the new home
        jc      f56_deny
f56_fill:
        push    esi
        mov     edi,esi
        mov     esi,OFFSET ren_entry    ; entry data, then the new name
        mov     ecx,32
        rep     movsb
        pop     esi
        push    esi
        mov     edi,esi
        mov     esi,OFFSET name_buf
        mov     ecx,11
        rep     movsb
        pop     esi
        call    fat_dirupdate
        jc      f56_deny

        ; tombstone the original
        mov     al,[mk_par_isroot]
        mov     edx,[mk_par_start]
        call    ctx_restore
        mov     ebx,[mk_slot]
        call    dir_seek
        jc      f56_deny
        mov     byte ptr [esi],0E5h
        call    fat_dirupdate
        jc      f56_deny
        call    fat_flush               ; a dir may have grown a cluster
        jc      f56_deny
        jmp     i21_ok

f56_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f56_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail
f56_exists:
        mov     ax,ERR_EXISTS
        jmp     i21_fail
f56_notsame:
        mov     ax,ERR_NOTSAME
        jmp     i21_fail
f56_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail

ren_entry   db  32 dup(0)
ren_drv     db  0
            db  0,0,0

;=====================================================================
; 4Bh - EXEC.  AL = 0, EDX -> path of a program, EBX -> a parameter
;       block: dd environment (a FLAT pointer to back-to-back ASCIIZ
;       "NAME=value" strings ended by an empty one, 0 for none),
;       dd command tail (DOS format: length byte, text, 0Dh; 0 = none).
;
; TWO KINDS OF PROGRAM COME THROUGH HERE, AND THE FILE DECIDES WHICH.
; A D32 image is loaded into the transient slot and run flat, 32-bit,
; the way everything in PM-DOS has been run so far.  Anything else is
; a 16-bit DOS program, and goes to the VDM: conventional memory, a
; real-mode PSP, and V86 mode, with its INT 21h calls served by the
; very handlers this file is made of.  The caller does not know or
; care which - COMMAND.COM asks for HELLO.COM the same way either way,
; which is the whole point of putting the fork here rather than in the
; shell.  The extension decides nothing; the signature does.
;
; When the program terminates, control returns here and this call
; comes back with its exit code available through 4Dh.
;
; A D32 program may EXEC a program, EXEC_MAX deep.  Since every D32
; image is linked for the same slot, the occupant is PARKED in the
; arena and restored on the child's 4Ch - see the nesting note at the
; top.  A 16-bit program needs none of that: its DOS box is below 1MB,
; so nothing above it is disturbed and nothing has to be parked.
;=====================================================================
fn4B:
        cmp     byte ptr [ebp+F_EAX],0
        jne     f4B_badfn

        ; Copy the path and the tail out of the caller NOW.  Both are
        ; pointers into the caller's own memory, and for a nested EXEC
        ; that memory is the slot this call is about to load over.
        mov     esi,[ebp+F_EDX]
        mov     edi,OFFSET exec_path
        mov     ecx,EXEC_PATH_MAX - 1
        call    exec_cpz

        mov     edi,OFFSET exec_tail
        mov     byte ptr [edi],0
        mov     byte ptr [edi+1],0Dh
        mov     ebx,[ebp+F_EBX]
        or      ebx,ebx
        jz      f4B_notail
        mov     esi,[ebx+4]
        call    f4B_inner               ; the block is the caller's, and so
        or      esi,esi                 ; is everything it points at
        jz      f4B_notail
        movzx   ecx,byte ptr [esi]
        cmp     ecx,7Eh
        jbe     f4B_tlen
        mov     ecx,7Eh
f4B_tlen:
        mov     [edi],cl
        inc     edi
        inc     esi
        rep     movsb
        mov     byte ptr [edi],0Dh
f4B_notail:

        ; AND THE ENVIRONMENT, INTO A KERNEL BUFFER, for the same
        ; reason and with a second one on top: a child cannot reach the
        ; block its parent points at even while that parent is intact,
        ; because it is in another slot.  So the environment is COPIED
        ; into the child rather than pointed at - which is exactly what
        ; the VDM already does for a 16-bit guest, and exactly why.
        mov     dword ptr [exec_envlen],0
        mov     ebx,[ebp+F_EBX]
        or      ebx,ebx
        jz      f4B_noenv
        mov     esi,[ebx]               ; parameter block +0
        call    f4B_inner
        or      esi,esi
        jz      f4B_noenv
        call    exec_cpenv
f4B_noenv:

        ; Which kind of program is it?  Ask the file.  EK_NOFILE joins
        ; the D32 path deliberately - see exec_kind.
        mov     edx,OFFSET exec_path
        call    exec_kind
        cmp     eax,EK_16BIT
        je      f4B_vdm

        cmp     dword ptr [exec_depth],EXEC_MAX
        jae     f4B_deep

        ; a record for the program that is about to start waiting
        mov     edi,[exec_depth]
        imul    edi,XN_LEN
        add     edi,OFFSET exec_stk
        mov     [edi+XN_EBP],ebp
        mov     eax,[cur_psp]
        mov     [edi+XN_PSP],eax
        mov     eax,[cur_own]
        mov     [edi+XN_OWN],eax
        mov     eax,[dta_ptr]           ; the child gets its own; this is
        mov     [edi+XN_DTA],eax        ; how the parent gets its back
        movzx   eax,byte ptr ds:[DOS_INDOS]
        mov     [edi+XN_INDOS],eax      ; and its InDOS depth - see below
        mov     dword ptr [edi+XN_SAVE],0

        ; THE RING-0 STACK HAS TO DESCEND WITH THE NESTING, and until it
        ; did, every child overwrote the frame its parent was waiting
        ; in.  Programs used to run at ring 0 and the kernel ran their
        ; DOS calls on their own stacks, so a parent's frame lived in
        ; the parent's slot and a child could not reach it.  At ring 3
        ; every program's DOS calls run on ONE kernel stack, and a child
        ; starting at the top of it writes its first INT 21h frame
        ; exactly where its parent's 4Bh frame is sitting.
        ;
        ; The parent's frame base is EBP and the frame is above it, so
        ; the child gets EBP and works downwards.  The parent's own
        ; level goes in the record for 4Ch to put back.
        mov     eax,[r3_esp0]
        mov     [edi+XN_ESP0],eax
        mov     [r3_esp0],ebp

        ; Park the occupant, if there is one.  At depth 0 the caller is
        ; the shell, which lives in its own slot and is not disturbed.
        cmp     dword ptr [exec_depth],0
        je      f4B_nosave
        mov     ebx,SLOT_SIZE
        call    arena_alloc
        jc      f4B_deep                ; no room to park it: no EXEC
        mov     [edi+XN_SAVE],eax
        push    edi
        mov     esi,PROG_PSP
        mov     edi,eax
        mov     ecx,SLOT_SIZE / 4
        rep     movsd
        pop     edi

f4B_nosave:
        ; Onto the kernel stack before the slot is written: at depth 1
        ; and beyond the caller's own stack is INSIDE it, and loading
        ; the child would saw through the branch we are sitting on.
        ; Everything needed from here on is in kernel memory.
        mov     esp,KSTACK_TOP
        mov     eax,1                   ; the transient slot, whatever the
        call    r3_useslot              ; depth: nesting parks, not relocates
        mov     edx,OFFSET exec_path
        call    exec_load
        jc      f4B_loadfail            ; AX holds the code
        mov     esi,OFFSET exec_tail
        call    build_psp
        call    exec_setenv

        inc     dword ptr [exec_depth]
        mov     eax,[next_own]          ; a fresh identity: handles and
        mov     [cur_own],eax           ; memory taken from here on are
        inc     dword ptr [next_own]    ; the child's, not the parent's
        mov     dword ptr [cur_psp],PROG_PSP

        ; THE CHILD RUNS WITH DOS IDLE, and getting this wrong would
        ; make every TSR useless.  This 4Bh never returns - it jumps
        ; into the child - so its own increment is still standing, and
        ; without this the whole of the child's run would look to a
        ; pop-up like a DOS call in progress.  4Ch puts the parent's
        ; depth back before leaving through the front door.
        mov     byte ptr ds:[DOS_INDOS],0

        mov     eax,[exec_entry]
        jmp     r3_start                ; and away, at ring 3

;--- the load failed with the slot already half overwritten ----------
; Put the occupant back before answering, or a failed EXEC would take
; its caller down with it.
f4B_loadfail:
        movzx   eax,ax
        mov     [exec_err],eax
        mov     eax,[exec_depth]        ; the caller's slot back under its
        call    r3_useslot              ; feet before it is resumed
        mov     edi,[exec_depth]
        imul    edi,XN_LEN
        add     edi,OFFSET exec_stk
        mov     esi,[edi+XN_SAVE]
        or      esi,esi
        jz      f4B_lf_done
        push    edi
        mov     edi,PROG_PSP
        mov     ecx,SLOT_SIZE / 4
        rep     movsd
        pop     edi
        mov     edx,[edi+XN_SAVE]
        call    arena_free
        mov     dword ptr [edi+XN_SAVE],0
f4B_lf_done:
        mov     esp,[edi+XN_EBP]        ; back onto the caller's stack,
        mov     ebp,esp                 ; whose frame is intact again
        mov     eax,[exec_err]
        ; CALL, not JMP: resetting ESP to the frame threw away the
        ; return address int21_entry's dispatch pushed below it, so
        ; i21_fail's RET has nothing to go back to.  Give it one, then
        ; leave through the front door - the same reason 4Ch does this.
        call    i21_fail
        jmp     i21_return

f4B_badfn:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f4B_deep:
        mov     ax,ERR_NOMEM
        jmp     i21_fail

;---------------------------------------------------------------------
; A 16-bit program.  This path RETURNS - which is what makes it so
; much shorter than the one above: the guest runs in conventional
; memory, the caller is untouched in its slot the whole time, and
; when the DOS box is gone there is nothing to put back.
;
; The environment is passed through as the flat pointer it is; the VDM
; makes the guest's own copy below 1MB, because a 16-bit program
; cannot reach the shell's.
;---------------------------------------------------------------------
f4B_vdm:
        xor     edi,edi
        mov     ebx,[ebp+F_EBX]
        or      ebx,ebx
        jz      f4B_vgo
        mov     esi,[ebx]                       ; parameter block +0
        call    f4B_inner                       ; in the caller's terms, and
        mov     edi,esi                         ; the caller may be at ring 3
f4B_vgo:
        mov     edx,OFFSET exec_path
        mov     esi,OFFSET exec_tail
        call    vdm_exec
        jc      f4B_vfail
        mov     al,byte ptr ds:[vdm_exit]       ; 4Dh answers with it
        mov     [exit_code],al
        jmp     i21_ok
f4B_vfail:
        mov     ax,word ptr ds:[vdm_err]
        jmp     i21_fail

;---------------------------------------------------------------------
; exec_kind - EDX -> ASCIIZ path.  EAX = EK_*: whose kind of executable
;             this is, or that there is no file there at all.
;
; A file that will not open is NOT reported here.  It goes down the D32
; path, which is the one that knows how to say "no such program", so
; that exactly one piece of code decides what that means - and so that
; COMMAND trying four extensions in turn does not set a DOS box up and
; tear it down again three times before it finds the right one.
;
; The answer is a value and not a flag because the natural test - CMP
; against the signature - sets the carry flag on its own account, and
; a "could not open" answer riding in CF would be forged by any 16-bit
; program whose first four bytes happen to sort low.
;
; It costs one open per EXEC, and it is worth it: trusting the
; extension is how a loader ends up executing an .EXE header.
;---------------------------------------------------------------------
; The labels below are ek_none/ek_out, NOT ek_nofile: wasm symbols are
; case-insensitive, so a label spelled like one of these equates is the
; equate, and what it reports is "unexpected colon" on the label.
EK_NOFILE       EQU     0
EK_D32          EQU     1
EK_16BIT        EQU     2

exec_kind:
        push    ebx
        push    ecx
        push    edx
        mov     eax,3D00h
        call    k21
        jc      ek_none
        mov     ebx,eax
        mov     dword ptr [exec_sig],0
        mov     dword ptr [exec_sig+4],0
        mov     edx,OFFSET exec_sig
        mov     ecx,8                           ; the jump AND the magic behind
        mov     eax,3F00h                       ; it - see D32H_JMP in PMDOS.INC
        call    k21
        mov     eax,3E00h                       ; EBX is still the handle
        call    k21
        mov     eax,EK_16BIT
        cmp     dword ptr [exec_sig+D32H_MAGIC],D32_MAGIC
        jne     ek_out
        mov     eax,EK_D32
        jmp     short ek_out
ek_none:
        mov     eax,EK_NOFILE
ek_out:
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; f4B_inner - ESI = a pointer read OUT OF the EXEC parameter block.
;             Make it flat if the caller was a native program.
;
; The block itself was translated on the way in, by the same table as
; every other pointer argument; what is inside it was not, because a
; table keyed on a register cannot say anything about memory.  So the
; two pointers the block holds - the environment and the command tail -
; are converted here, one call each, at the point they are read.
;
; ESI comes back zero if it was zero or if it is not an address the
; caller could have meant, which both callers already treat as "not
; supplied".
;---------------------------------------------------------------------
f4B_inner:
        cmp     word ptr [ebp+F_CS],SEL_PROG_CS
        jne     fbi_out
        or      esi,esi
        jz      fbi_out
        push    eax
        mov     eax,esi
        call    r3_flat
        jc      fbi_none
        mov     esi,eax
        pop     eax
        ret
fbi_none:
        xor     esi,esi
        pop     eax
fbi_out:
        ret

;---------------------------------------------------------------------
; exec_cpenv - ESI -> an environment, flat.  Take a copy into kernel
;              memory now, while the caller is still there to have one.
;
; The block is a run of ASCIIZ strings ended by an empty one.  A
; truncated copy would be worse than none, so a run that will not fit
; is refused whole: a program with half an environment finds a PATH
; that names three directories out of four and fails somewhere else.
;---------------------------------------------------------------------
exec_cpenv:
        pushad
        mov     edi,OFFSET exec_env
        xor     ecx,ecx                 ; bytes copied
ece_str:
        cmp     ecx,SLOT_ENVMAX - 2
        jae     ece_toobig
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        inc     ecx
        or      al,al
        jnz     ece_str
        cmp     byte ptr [esi],0        ; an empty string ends the block
        jne     ece_str
        mov     byte ptr [edi],0
        inc     ecx
        mov     [exec_envlen],ecx
        popad
        ret
ece_toobig:
        mov     dword ptr [exec_envlen],0
        popad
        ret

;---------------------------------------------------------------------
; exec_setenv - put the copy into the child's slot, behind its PSP, and
;               point PSP_ENV at it.  Called after build_psp, which has
;               just zeroed the field.
;---------------------------------------------------------------------
exec_setenv:
        pushad
        mov     ecx,[exec_envlen]
        jecxz   ese_out
        mov     edi,[r3_base]
        add     edi,SLOT_ENV
        mov     esi,OFFSET exec_env
        rep     movsb
        mov     edi,[exec_psp]
        mov     dword ptr [edi+PSP_ENV],SLOT_ENV
ese_out:
        popad
        ret

;---------------------------------------------------------------------
; exec_cpz - copy the ASCIIZ at ESI to EDI, at most ECX characters,
;            always terminated.
;---------------------------------------------------------------------
exec_cpz:
        or      ecx,ecx
        jz      ecz_end
        mov     al,[esi]
        or      al,al
        jz      ecz_end
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jmp     short exec_cpz
ecz_end:
        mov     byte ptr [edi],0
        ret

;---------------------------------------------------------------------
; exec_load - load a D32 executable.
;
;   EDX -> ASCIIZ path;  the slot is whichever r3_useslot last chose.
;   On success: image + zeroed BSS in place, the slot heap laid out
;   behind them, [exec_entry] set to the entry OFFSET, CF clear.
;   On failure: CF set, AX = ERR_NOFILE / ERR_EXEFMT / ERR_NOMEM.
;
; Also used directly by DOSMAIN to start the shell.
;---------------------------------------------------------------------
exec_load:
        mov     esi,edx
        call    resolve_parent
        jc      xl_nofile
        cmp     byte ptr [rp_noname],0
        jne     xl_nofile
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      xl_nofile

        mov     byte ptr [srch_attr],0
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      xl_nofile

        call    de_getclus
        mov     ecx,eax
        mov     eax,[esi+DE_SIZE]
        mov     [exec_left],eax
        cmp     eax,D32H_SIZE
        jb      xl_badfmt
        or      ecx,ecx
        jz      xl_badfmt
        mov     [exec_clus],ecx

        ; header check: read the first cluster into clus_buf
        mov     eax,ecx
        mov     edi,OFFSET clus_buf
        call    fat_readclus
        jc      xl_nofile
        mov     dword ptr [clusbuf_clus],-1     ; buffer no longer file cache

        ; EVERY FIELD IN THE HEADER IS A SLOT OFFSET NOW, and the base
        ; is checked against the one offset every D32 program is linked
        ; for rather than against "the slot this is going into".  It is
        ; the same test it always was and it now means something
        ; better: a file either has this kernel's idea of a program's
        ; layout or it is not one of this kernel's programs.  Which
        ; slot it lands in stopped being the file's business when the
        ; segment base became the thing that decides.
        cmp     dword ptr [clus_buf+D32H_MAGIC],D32_MAGIC
        jne     xl_badfmt
        cmp     dword ptr [clus_buf+D32H_BASE],SLOT_IMAGE
        jne     xl_badfmt
        mov     eax,[r3_base]
        add     eax,SLOT_IMAGE
        mov     [exec_cursor],eax       ; flat: the kernel does the blit

        mov     ebx,dword ptr [clus_buf+D32H_ENTRY]
        cmp     ebx,SLOT_IMAGE
        jb      xl_badfmt
        mov     ecx,SLOT_IMAGE
        add     ecx,[exec_left]         ; the offset just past the image
        cmp     ebx,ecx
        jae     xl_badfmt
        mov     [exec_entry],ebx        ; stays an offset: it is a ring-3 EIP

        ; image + bss + the stack the header asks for must fit in the
        ; slot.  D32H_STACK was informational for eight releases and is
        ; load-bearing now: it is what divides the space above the BSS
        ; between the heap 48h allocates from and the stack.
        mov     ebx,ecx
        add     ebx,dword ptr [clus_buf+D32H_BSS]
        mov     [exec_bssend],ebx
        mov     eax,dword ptr [clus_buf+D32H_STACK]
        cmp     eax,1000h               ; never less than 4K, whatever it says
        jae     xl_stkok
        mov     eax,1000h
xl_stkok:
        mov     [exec_stack],eax
        add     ebx,eax
        jc      xl_nomem
        cmp     ebx,[r3_size]
        ja      xl_nomem

;--- stream the file, cluster by cluster ------------------------------
xl_blit:
        mov     eax,[fat_spc]
        shl     eax,9                   ; EAX = cluster size
        cmp     eax,[exec_left]
        jbe     xl_n_ok
        mov     eax,[exec_left]
xl_n_ok:
        push    eax
        mov     esi,OFFSET clus_buf
        mov     edi,[exec_cursor]
        mov     ecx,eax
        rep     movsb
        pop     eax
        add     [exec_cursor],eax
        sub     [exec_left],eax
        jz      xl_loaded

        mov     eax,[exec_clus]         ; next cluster of the chain
        call    fat_next
        cmp     eax,[fat_eoc]
        jae     xl_badfmt               ; chain shorter than the size field
        mov     [exec_clus],eax
        mov     edi,OFFSET clus_buf
        call    fat_readclus
        jc      xl_nofile
        jmp     short xl_blit

xl_loaded:
        ; zero the BSS.  The cursor is flat and the end is an offset,
        ; so one of them has to move; the offset does, because the
        ; kernel is the only thing here that can hold both.
        mov     edi,[exec_cursor]
        mov     ecx,[r3_base]
        add     ecx,[exec_bssend]
        sub     ecx,edi
        jbe     xl_heap
        xor     eax,eax
        rep     stosb
xl_heap:
        ; and lay out what is left: the heap 48h hands out, between the
        ; BSS and the stack reservation at the top of the slot.
        mov     eax,[exec_bssend]
        mov     ebx,[exec_stack]
        call    r3h_init
xl_done:
        clc
        ret

xl_nofile:
        mov     ax,ERR_NOFILE
        stc
        ret
xl_badfmt:
        mov     ax,ERR_EXEFMT
        stc
        ret
xl_nomem:
        mov     ax,ERR_NOMEM
        stc
        ret

;---------------------------------------------------------------------
; build_psp - construct the PSP at [exec_psp].  ESI -> command tail in
;             DOS format (len, text, 0Dh), or 0 for an empty tail.
;
; The three saved vectors are taken LIVE, here, at the moment the child
; is about to start - so what goes into its PSP is whatever the parent
; had installed, and 4Ch putting them back restores the parent's world
; whether or not the child was well behaved about it.
;
; The DTA is set to the DOS default of PSP+80h, which is the command
; tail: a program that does a directory search before it has finished
; reading its own arguments loses them.  That is authentic, it is the
; oldest trap in the DOS book, and programs are written around it -
; every PM-DOS external reads its tail at entry for exactly this
; reason.
;---------------------------------------------------------------------
build_psp:
        pushad
        mov     edi,[r3_base]
        add     edi,SLOT_PSP
        mov     [exec_psp],edi
        push    edi
        xor     eax,eax
        mov     ecx,PSP_SIZE / 4
        rep     stosd
        pop     edi

        mov     word ptr [edi+PSP_INT20],20CDh
        mov     eax,[r3_size]           ; the first byte past the slot, in
        mov     [edi+PSP_MEMTOP],eax    ; the terms the program uses

        ; INT 21h / RET at PSP+50h: the flat reading of DOS's far-call
        ; thunk, for a program that calls the API through its PSP.
        mov     word ptr [edi+PSP_DOSCALL],21CDh
        mov     byte ptr [edi+PSP_DOSCALL+2],0C3h

        ; ...and the bare INT 21h at PSP+54h that a handler's IRET
        ; comes back through.  Two bytes and no function number: see
        ; the note beside PSP_R3RET, and r3_thunk.
        mov     word ptr [edi+PSP_R3RET],21CDh

        mov     al,22h                  ; the vectors, as they stand now
        call    get_vec
        mov     [edi+PSP_TERM],eax
        mov     al,23h
        call    get_vec
        mov     [edi+PSP_CTRLC],eax
        mov     al,24h
        call    get_vec
        mov     [edi+PSP_CRITERR],eax

        ; Who started us.  DOS's idiom is that a program whose parent
        ; field names its own PSP has no parent, and the shell is that
        ; program; a child's parent lives in another slot and there is
        ; no address in this one that names it, so the honest answer is
        ; zero rather than a number pointing at nothing.  Either way
        ; the test everybody makes - "is my parent me?" - still works.
        ; The slot decides, not [cur_psp]: a NESTED program is loaded
        ; into its parent's slot and shares its PSP address, so
        ; comparing the two would tell a program three levels deep that
        ; it was the interpreter.  Only the interpreter runs in the
        ; shell slot, and that is the whole test.
        mov     eax,SLOT_PSP
        cmp     edi,SHELL_PSP
        je      bp_noparent
        xor     eax,eax
bp_noparent:
        mov     [edi+PSP_PARENT],eax

        lea     eax,[edi+PSP_TAIL]      ; the default DTA
        mov     [dta_ptr],eax

        add     edi,PSP_TAIL
        or      esi,esi
        jz      bp_empty
        movzx   ecx,byte ptr [esi]
        cmp     ecx,7Eh
        jbe     bp_len_ok
        mov     ecx,7Eh
bp_len_ok:
        mov     [edi],cl
        inc     edi
        inc     esi
        rep     movsb
        mov     byte ptr [edi],0Dh
        popad
        ret
bp_empty:
        mov     byte ptr [edi],0
        mov     byte ptr [edi+1],0Dh
        popad
        ret

;=====================================================================
; 4Ch - terminate with return code AL.
;
; From a transient program: unwind into the EXEC call that started it -
; its parent's, which is the shell only at depth 1.
; From the shell itself: hand back to the kernel, which reloads it.
;
; Either way the program's HANDLES are closed and its MEMORY is
; reclaimed first.  Both are found by owner id, never by PSP address:
; a nested program is loaded into its parent's slot and shares its PSP
; address, so an address comparison would close the parent's files and
; free the parent's memory while the parent is still waiting to run.
;=====================================================================
fn4C:
        ; WITH A VM RUNNING, THE PROGRAM THIS ENDS IS THE GUEST.  The
        ; guest's own 4Ch never reaches here - the bridge answers that
        ; one - so a 4Ch arriving now came from 32-bit code acting on
        ; the guest's behalf: the INT 23h handler after a ^C, or an
        ; Abort at the critical-error prompt.  Terminating "the current
        ; program" by the ordinary route would end the SHELL, which is
        ; merely waiting in its EXEC call, and take AUTOEXEC.BAT round
        ; again.  vdm_abort ends the DOS box instead and hands the
        ; shell back its EXEC.
        cmp     dword ptr ds:[vdm_busy],0
        jne     f4C_vdm

        mov     al,[ebp+F_EAX]
        mov     [exit_code],al

        ; Lift the saved vectors out of the PSP NOW, while the PSP is
        ; still ours: un-parking below writes the parent's copy of the
        ; whole slot back over it.  They are re-installed at the very
        ; end, once nothing else can disturb them.
        mov     esi,[cur_psp]
        mov     eax,[esi+PSP_TERM]
        mov     [term_vec22],eax
        mov     eax,[esi+PSP_CTRLC]
        mov     [term_vec23],eax
        mov     eax,[esi+PSP_CRITERR]
        mov     [term_vec24],eax

        ; Close what THIS program left open, and only that.  With one
        ; global handle table and no owner, a child's exit would take
        ; the shell's open batch file down with it - which is exactly
        ; what happened the first time CHOICE became an external.
        mov     esi,OFFSET sft_tbl + 3 * SFT_LEN
        mov     ecx,MAX_HANDLES - 3
        mov     edx,[cur_own]
f4C_sweep:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      f4C_next
        cmp     [esi+SFT_OWNER],edx
        jne     f4C_next
        push    edx
        call    sft_close
        pop     edx
f4C_next:
        add     esi,SFT_LEN
        loop    f4C_sweep

        ; ...and reclaim what it allocated and never freed.  DOS always
        ; did this; PM-DOS used to leak every 48h block until reboot.
        mov     eax,[cur_own]
        call    arena_freeown

        ; A HANDLER OF THIS PROGRAM'S MAY BE WHAT CALLED 4Ch, and if so
        ; it is not going back.  COMMAND's INT 23h handler does exactly
        ; this when a ^C arrives with a child running: it terminates the
        ; child from inside the handler rather than returning from it.
        ; The frame r3_thunk parked for the IRET that will now never
        ; happen goes with everything else this unwind discards.
        call    r3_abandon

        cmp     dword ptr [exec_depth],0
        je      f4C_shell

        ; Pop one nesting level: the record at the new depth describes
        ; the program that has been waiting for us.
        dec     dword ptr [exec_depth]
        mov     edi,[exec_depth]
        imul    edi,XN_LEN
        add     edi,OFFSET exec_stk

        ; Onto the kernel stack first.  Un-parking the waiting program
        ; rewrites the whole slot, and nothing inside it may be
        ; depended on across that.  The dying program's own stack is in
        ; there - which is the half that used to matter, when the
        ; kernel ran a program's DOS calls on the program's stack.  It
        ; does not any more, and this stays anyway: a routine that
        ; overwrites half a megabyte should not be standing on any of
        ; it.
        mov     esp,KSTACK_TOP
        mov     eax,[exec_depth]        ; the waiting program's slot, and
        call    r3_useslot              ; its descriptors, back
        mov     esi,[edi+XN_SAVE]
        or      esi,esi
        jz      f4C_noparked            ; depth 1: the shell's own slot
        push    edi
        mov     edi,PROG_PSP
        mov     ecx,SLOT_SIZE / 4
        rep     movsd
        pop     edi
        mov     edx,[edi+XN_SAVE]
        call    arena_free
        mov     dword ptr [edi+XN_SAVE],0
f4C_noparked:
        mov     eax,[edi+XN_PSP]
        mov     [cur_psp],eax
        mov     eax,[edi+XN_OWN]        ; its identity comes back with it
        mov     [cur_own],eax
        mov     eax,[edi+XN_DTA]        ; and its DTA, which the child was
        mov     [dta_ptr],eax           ; free to move
        mov     eax,[edi+XN_INDOS]      ; the depth its own 4Bh was at, so
        mov     byte ptr ds:[DOS_INDOS],al      ; that i21_r0's decrement
                                        ; lands where it should
        mov     eax,[edi+XN_ESP0]       ; and the ring-0 stack level it was
        mov     [r3_esp0],eax           ; making its DOS calls on
        call    term_vectors
        mov     byte ptr [crit_busy],0  ; an abort has finished unwinding
        mov     byte ptr ds:[DOS_ERRFLG],0
        mov     esp,[edi+XN_EBP]        ; its frame is intact again
        mov     ebp,esp
        mov     word ptr [ebp+F_EAX],0
        call    i21_ok
        jmp     i21_return

f4C_vdm:
        ; The lock first, for the same reason cmd_gohome clears it: a
        ; longjmp out of a critical-error handler would carry the
        ; kernel's "a handler is running" flag away with it, after
        ; which every critical error quietly answers itself.
        mov     byte ptr [crit_busy],0
        mov     byte ptr ds:[DOS_ERRFLG],0
        jmp     vdm_abort

f4C_shell:
        ; The shell is going away, and with it any redirection it
        ; never unwound (EXIT > FILE ends the shell INSIDE the
        ; redirected run).  The parked console copies were owned by
        ; the shell and just went in the sweep above; here whatever
        ; is sitting in the standard slots is closed - flushing the
        ; file EXIT redirected into - and the fresh shell starts on
        ; CON, the same state sft_init hands the first one.
        mov     esi,OFFSET sft_tbl
        mov     ecx,3
f4C_std:
        test    dword ptr [esi+SFT_FLAGS],SFTF_DEVICE
        jnz     f4C_stdisdev
        call    sft_close
f4C_stdisdev:
        mov     dword ptr [esi+SFT_FLAGS],SFTF_INUSE OR SFTF_DEVICE
        mov     dword ptr [esi+SFT_DEVID],DEV_CON       ; and back on CON,
                                        ; not on whatever "EXIT > NUL" left
        add     esi,SFT_LEN
        loop    f4C_std
        call    term_vectors            ; the kernel's own handlers back
        mov     byte ptr [crit_busy],0  ; an abort has finished unwinding
        mov     byte ptr ds:[DOS_ERRFLG],0
        jmp     shell_restart           ; in DOSMAIN.ASM

;---------------------------------------------------------------------
; term_vectors - re-install the 22h/23h/24h handlers 4Ch lifted out of
;                the terminating program's PSP.
;
; This is what stops a child that hooked Ctrl-C from leaving its own
; handler pointing into a slot that no longer holds it - the first
; thing the parent's next ^C would jump into.  Preserves everything,
; because both callers are mid-unwind with live pointers in registers.
;---------------------------------------------------------------------
term_vectors:
        pushad
        mov     edi,[term_vec22]
        mov     al,22h
        call    set_vec
        mov     edi,[term_vec23]
        mov     al,23h
        call    set_vec
        mov     edi,[term_vec24]
        mov     al,24h
        call    set_vec
        popad
        ret

;=====================================================================
; 4Dh - get return code of the last 4Ch
;=====================================================================
fn4D:
        movzx   ax,byte ptr [exit_code]
        mov     [ebp+F_EAX],ax
        mov     byte ptr [exit_code],0
        jmp     i21_ok

;=====================================================================
; 4Eh - find first.  EDX -> pattern, CX = attributes (ignored).
;       Fills the DTA; CF/ERR_NOFILE when nothing matches.
;=====================================================================
fn4E:
        mov     esi,[ebp+F_EDX]
        call    resolve_parent          ; sets the context, fills name_buf
        jc      f4E_none                ; ([rp_noname] leaves it all-'?')
        mov     dword ptr [srch_long],0 ; classic find stays pure 8.3 -
                                        ; the DTA can only carry 8.3
                                        ; state between 4Eh and 4Fh

        mov     edi,[dta_ptr]
        mov     al,[cur_vol]            ; the LETTER, not the disk: with
        mov     [edi],al                ; several volumes per disk a BIOS
                                        ; number no longer names one
        push    esi
        push    edi
        mov     esi,OFFSET name_buf     ; DTA+1 = the pattern
        inc     edi
        mov     ecx,11
        rep     movsb
        pop     edi
        pop     esi
        mov     al,[ebp+F_ECX]          ; search attributes
        or      al,SRCH_FILTER          ; a directory search, so hidden
        mov     [edi+0Ch],al            ; and system files stay hidden
        mov     [srch_attr],al
        call    ctx_save                ; the directory being searched
        mov     [edi+13h],al
        mov     [edi+0Fh],edx
        xor     ebx,ebx                 ; start of the directory
        jmp     short find_common

;=====================================================================
; 4Fh - find next, from the state left in the DTA
;=====================================================================
fn4F:
        mov     edi,[dta_ptr]
        mov     al,[edi]                ; DTA+0 = the logical drive 4Eh was on
        call    vol_select
        jc      f4E_none
        mov     al,[edi+0Ch]
        mov     [srch_attr],al
        mov     al,[edi+13h]            ; the directory context
        mov     edx,[edi+0Fh]
        call    ctx_restore
        movzx   ebx,word ptr [edi+0Dh]  ; resume index

find_common:
        mov     edi,[dta_ptr]
        add     edi,1                   ; the stored pattern
        call    fat_search
        jc      f4E_none

        ; EBX = hit index, ESI -> live entry: fill the caller's half
        mov     edi,[dta_ptr]
        inc     ebx
        mov     [edi+0Dh],bx            ; where find-next resumes

        mov     al,[esi+DE_ATTR]
        mov     [edi+15h],al
        mov     ax,[esi+DE_TIME]
        mov     [edi+16h],ax
        mov     ax,[esi+DE_DATE]
        mov     [edi+18h],ax
        mov     eax,[esi+DE_SIZE]
        mov     [edi+1Ah],eax

        ; 11-byte padded name -> ASCIIZ 8.3 at DTA+1Eh
        add     edi,1Eh
        push    ebx
        xor     ebx,ebx

        ; A VOLUME LABEL is not a filename.  It is eleven free-form
        ; bytes with no name/extension split, so it is copied whole and
        ; only right-trimmed - splicing a dot in after the eighth byte
        ; is what turned the label "RENAMED16" into "RENAMED1.6" for
        ; DIR's header line and for VOL.  The test is the same one
        ; fat_search uses to recognise a label, so an LFN fragment
        ; (attribute 0Fh, VOLUME set but DIR too) cannot come in here.
        mov     al,[esi+DE_ATTR]
        and     al,ATTR_VOLUME OR ATTR_DIR
        cmp     al,ATTR_VOLUME
        jne     f4E_n1
f4E_v1:
        cmp     ebx,11
        jae     f4E_vtrim
        mov     al,[esi+ebx]
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short f4E_v1
f4E_vtrim:
        or      ebx,ebx
        jz      f4E_z
        cmp     byte ptr [edi-1],' '
        jne     f4E_z
        dec     edi
        dec     ebx
        jmp     short f4E_vtrim

f4E_n1:                                 ; name part
        cmp     ebx,8
        jae     f4E_dot
        mov     al,[esi+ebx]
        cmp     al,' '
        je      f4E_dot
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short f4E_n1
f4E_dot:
        cmp     byte ptr [esi+8],' '    ; any extension at all?
        je      f4E_z
        mov     byte ptr [edi],'.'
        inc     edi
        mov     ebx,8
f4E_e1:
        cmp     ebx,11
        jae     f4E_z
        mov     al,[esi+ebx]
        cmp     al,' '
        je      f4E_z
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short f4E_e1
f4E_z:
        mov     byte ptr [edi],0
        pop     ebx
        jmp     i21_ok

f4E_none:
        mov     ax,ERR_NOFILE
        cmp     byte ptr [rp_baddrv],0  ; ...unless the DRIVE is what
        je      f4E_nf                  ; could not be reached, which
        mov     ax,ERR_BADDRIVE         ; is a different sentence
f4E_nf:
        jmp     i21_fail

;=====================================================================
; 43h - file attributes.  EDX -> path; AL = 0 reads them into CX,
;       AL = 1 sets them from CX (read-only, hidden, system, archive -
;       a directory keeps being one).  7143h is the same call with the
;       subfunction in BL, so both land on attr_core.  LFN-transparent
;       like every other path taker.
;=====================================================================
fn43:
        mov     al,[ebp+F_EAX]
        jmp     short attr_core
fn7143:
        mov     al,[ebp+F_EBX]
attr_core:
        cmp     al,1
        ja      f43_badfn
        mov     [f43_sub],al
        mov     esi,[ebp+F_EDX]
        call    dev_hide                ; a device has no attributes, and
        jnc     i21_fail                ; MS-DOS says so with "not found"
        mov     esi,[ebp+F_EDX]
        call    resolve_parent
        jc      f43_err                 ; AX = the resolver's verdict
        cmp     byte ptr [rp_noname],0
        jne     f43_nofile
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f43_nofile
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f43_nofile
        cmp     byte ptr [f43_sub],0
        jne     f43_set
        movzx   eax,byte ptr [esi+DE_ATTR]
        mov     word ptr [ebp+F_ECX],ax
        jmp     i21_ok
f43_set:
        mov     al,[ebp+F_ECX]
        and     al,ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM OR ATTR_ARCHIVE
        mov     ah,[esi+DE_ATTR]        ; what 43h cannot touch stays
        and     ah,ATTR_DIR OR ATTR_VOLUME
        or      al,ah
        mov     [esi+DE_ATTR],al
        call    fat_dirupdate
        jc      f43_deny
        jmp     i21_ok
f43_badfn:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f43_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f43_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f43_err:
        jmp     i21_fail

f43_sub     db  0
            db  0,0,0

;=====================================================================
; 71h - the Win95 long-file-name API.
;
;   7139h/3Ah/3Bh/41h/56h  MKDIR, RMDIR, CHDIR, delete, rename: the
;                      register layouts match the classic calls, and
;                      the classic handlers are LFN-transparent, so
;                      these dispatch straight onto them
;   7143h attributes   BL = 0 get -> CX / 1 set from CX, EDX -> path
;   7147h get cwd      DL = drive, ESI -> buffer (long names)
;   714Eh find first   EDX -> pattern path, EDI -> 318-byte record,
;                      CL = allowable attrs, CH = required attrs
;                      -> AX = find handle, CX = 0
;   714Fh find next    BX = handle, EDI -> record
;   7160h truename     CL = 0/1 the 8.3 form, 2 the long form
;   716Ch open/create  BX = mode, CX = attrs, DX = action, ESI -> path
;   71A0h volume info  EDI -> fs-name buffer, ECX = its size
;   71A1h find close   BX = handle
;
; Anything else answers AX = 7100h with CF - the documented "not
; supported" signature LFN-aware programs test.  Unlike 4Eh/4Fh,
; which parks its whole state in the caller's DTA, this find is
; HANDLE-based: the pattern lives in a kernel copy because the
; caller's buffer is long gone by the time find-next runs.  Matching
; is dual-namespace, Win95's way: the 8.3 pattern against the 8.3
; name, and the long pattern against the file's PRIMARY name - its
; LFN when it has one, its 8.3 rendered as text when it does not.
; Times are always returned in DOS date/time form (the SI format
; selector is ignored; our directory entries know nothing else).
;=====================================================================
FH_INUSE        EQU     0               ; db
FH_DRIVE        EQU     1               ; db - logical drive
FH_ISROOT       EQU     2               ; db
FH_ALLOW        EQU     3               ; db - CL of the 714Eh
FH_REQ          EQU     4               ; db - CH of the 714Eh
FH_P83          EQU     5               ; 11 bytes - the 8.3 pattern
FH_NEXT         EQU     16              ; dd - resume index
FH_DIR          EQU     20              ; dd - directory start
FH_PLEN         EQU     24              ; dd - long pattern length
FH_PLONG        EQU     28              ; the long pattern text
FH_SIZE         EQU     288             ; = 28 + LFN_MAX

fn71:
        cmp     byte ptr [lfn_on],0     ; LFN=OFF in CONFIG.SYS: the
        je      f71_unsup               ; whole family is absent - the
                                        ; authentic no-LFN answer
        mov     al,[ebp+F_EAX]
        cmp     al,39h                  ; the classic handlers are
        je      f71_j39                 ; LFN-transparent by design, so
        cmp     al,3Ah                  ; the 71h forms with identical
        je      f71_j3A                 ; register layouts just ARE
        cmp     al,3Bh                  ; them
        je      f71_j3B
        cmp     al,41h
        je      f71_j41
        cmp     al,43h
        je      f71_j43
        cmp     al,47h
        je      f71_j47
        cmp     al,4Eh
        je      fn714E
        cmp     al,4Fh
        je      fn714F
        cmp     al,56h
        je      f71_j56
        cmp     al,60h
        je      f71_j60
        cmp     al,6Ch
        je      f71_j6C
        cmp     al,0A0h
        je      fn71A0
        cmp     al,0A1h
        je      fn71A1
f71_unsup:
        mov     ax,7100h                ; "no such subfunction", the
        jmp     i21_fail                ; signature probes look for

f71_j39:
        jmp     fn39
f71_j3A:
        jmp     fn3A
f71_j3B:
        jmp     fn3B
f71_j41:
        jmp     fn41
f71_j43:
        jmp     fn7143
f71_j47:
        jmp     fn7147
f71_j56:
        jmp     fn56
f71_j60:
        jmp     fn7160
f71_j6C:
        jmp     fn716C

;--- 714Eh find first -------------------------------------------------
fn714E:
        mov     ebx,OFFSET find_tbl     ; a free handle first
        xor     ecx,ecx
f7e_slot:
        cmp     byte ptr [ebx+FH_INUSE],0
        je      f7e_have
        add     ebx,FH_SIZE
        inc     ecx
        cmp     ecx,FIND_MAX
        jb      f7e_slot
        mov     ax,ERR_NOHANDLES
        jmp     i21_fail
f7e_have:
        mov     [f71_hdl],ebx
        mov     [f71_hidx],ecx

        mov     esi,[ebp+F_EDX]
        call    resolve_parent          ; the walk is LFN-transparent
        jc      f71_nofile
        mov     dword ptr [srch_long],0 ; find matches its own way
        cmp     byte ptr [rp_noname],0
        jne     f71_nofile

        mov     ebx,[f71_hdl]
        mov     al,[cur_vol]
        mov     [ebx+FH_DRIVE],al
        call    ctx_save                ; AL = isroot, EDX = start
        mov     [ebx+FH_ISROOT],al
        mov     [ebx+FH_DIR],edx
        mov     al,[ebp+F_ECX]          ; CL = allowable attributes
        mov     [ebx+FH_ALLOW],al
        mov     al,[ebp+F_ECX+1]        ; CH = required attributes
        mov     [ebx+FH_REQ],al
        mov     dword ptr [ebx+FH_NEXT],0

        push    esi                     ; the 8.3 half of the pattern
        push    edi
        mov     esi,OFFSET name_buf
        lea     edi,[ebx+FH_P83]
        mov     ecx,11
        rep     movsb
        pop     edi
        pop     esi

        ; the long half: the final component's raw text.  "*.*" is
        ; spelt "*" - Win95's own rewrite, or nothing dotless would
        ; ever match it.
        xor     ecx,ecx
        mov     esi,[rp_fptr]
        or      esi,esi
        jz      f7e_plset
        mov     ecx,[rp_flen]
        cmp     ecx,3
        jne     f7e_plcopy
        cmp     word ptr [esi],'.*'     ; "*." little-endian
        jne     f7e_plcopy
        cmp     byte ptr [esi+2],'*'
        jne     f7e_plcopy
        mov     ecx,1                   ; "*.*" -> "*"
f7e_plcopy:
        push    ecx
        push    esi
        push    edi
        lea     edi,[ebx+FH_PLONG]
        rep     movsb
        pop     edi
        pop     esi
        pop     ecx
f7e_plset:
        mov     [ebx+FH_PLEN],ecx

        mov     byte ptr [f71_first],1
        jmp     f71_step

;--- 714Fh find next --------------------------------------------------
fn714F:
        movzx   ebx,word ptr [ebp+F_EBX]
        cmp     ebx,FIND_MAX
        jae     f71_badh
        imul    ebx,FH_SIZE
        add     ebx,OFFSET find_tbl
        cmp     byte ptr [ebx+FH_INUSE],0
        je      f71_badh
        mov     [f71_hdl],ebx
        mov     byte ptr [f71_first],0
        mov     al,[ebx+FH_DRIVE]
        call    vol_select
        jc      f71_nomore
        mov     al,[ebx+FH_ISROOT]
        mov     edx,[ebx+FH_DIR]
        call    ctx_restore

;--- the common walk --------------------------------------------------
f71_step:
        call    lfnw_reset
        mov     byte ptr [lfn_ok],0
        mov     edx,[f71_hdl]
        mov     ebx,[edx+FH_NEXT]
f7s_lp:
        call    dir_seek
        jc      f71_nomore
        mov     al,[esi]
        or      al,al
        jz      f71_nomore
        cmp     al,0E5h
        je      f7s_del
        mov     al,[esi+DE_ATTR]
        and     al,ATTR_LFN
        cmp     al,ATTR_LFN
        jne     f7s_real
        call    lfnw_frag
        jmp     f7s_next
f7s_del:
        call    lfnw_reset
        jmp     f7s_next
f7s_real:
        call    lfnw_seal
        ; attribute gate: bits outside allowable (read-only and
        ; archive always are) exclude it; required bits must be on
        mov     edx,[f71_hdl]
        mov     al,[esi+DE_ATTR]
        mov     ah,[edx+FH_ALLOW]
        or      ah,ATTR_READONLY OR ATTR_ARCHIVE
        not     ah
        and     al,ah
        and     al,3Fh
        jnz     f7s_next
        mov     al,[esi+DE_ATTR]
        mov     ah,[edx+FH_REQ]
        and     al,ah
        cmp     al,ah
        jne     f7s_next

        push    edi                     ; the 8.3 pattern first
        lea     edi,[edx+FH_P83]
        call    f71_m83
        pop     edi
        jnc     f7s_hit
        cmp     dword ptr [edx+FH_PLEN],0
        je      f7s_next
        ; the long pattern against the primary name
        push    esi
        push    edi
        cmp     byte ptr [lfn_ok],0
        jne     f7s_lname
        push    edi
        mov     edi,OFFSET f71_name     ; no LFN: its 8.3, as text
        call    name83_str
        pop     edi
        mov     edi,OFFSET f71_name
        jmp     short f7s_lmt
f7s_lname:
        mov     edi,OFFSET lfn_buf
f7s_lmt:
        lea     esi,[edx+FH_PLONG]
        mov     ecx,[edx+FH_PLEN]
        call    lfn_match
        pop     edi
        pop     esi
        jnc     f7s_hit
f7s_next:
        inc     ebx
        jmp     f7s_lp

;--- a hit: marshal the 318-byte record -------------------------------
f7s_hit:
        mov     edi,[ebp+F_EDI]
        push    edi                     ; zero it first
        xor     eax,eax
        mov     ecx,79
        rep     stosd
        stosw
        pop     edi

        movzx   eax,byte ptr [esi+DE_ATTR]
        mov     [edi],eax
        movzx   eax,word ptr [esi+DE_DATE]
        shl     eax,16
        mov     ax,[esi+DE_TIME]        ; date high, time low
        mov     [edi+4],eax             ; created = accessed = written:
        mov     [edi+0Ch],eax           ; FAT keeps only the one stamp
        mov     [edi+14h],eax
        mov     eax,[esi+DE_SIZE]
        mov     [edi+20h],eax

        push    edi                     ; the 8.3 name at +130h
        add     edi,130h
        call    name83_str
        pop     edi

        push    edi                     ; the long name at +2Ch - the
        add     edi,2Ch                 ; 8.3 again when there is none
        cmp     byte ptr [lfn_ok],0
        je      f7s_noln
        push    esi
        mov     esi,OFFSET lfn_buf
f7s_lcp:
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        or      al,al
        jnz     f7s_lcp
        pop     esi
        jmp     short f7s_named
f7s_noln:
        call    name83_str
f7s_named:
        pop     edi

        mov     edx,[f71_hdl]
        inc     ebx
        mov     [edx+FH_NEXT],ebx
        mov     byte ptr [edx+FH_INUSE],1
        cmp     byte ptr [f71_first],0
        je      f7s_done
        mov     eax,[f71_hidx]          ; find-first: hand out the
        mov     word ptr [ebp+F_EAX],ax ; handle
        mov     word ptr [ebp+F_ECX],0  ; no unicode conversions
f7s_done:
        jmp     i21_ok

f71_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f71_nomore:
        cmp     byte ptr [f71_first],0
        je      f71_nm2
        mov     ax,ERR_NOFILE           ; find-first: nothing at all
        jmp     i21_fail
f71_nm2:
        mov     ax,12h                  ; find-next: no more files
        jmp     i21_fail
f71_badh:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail

;--- 71A0h volume information -----------------------------------------
fn71A0:
        mov     edi,[ebp+F_EDI]
        cmp     dword ptr [ebp+F_ECX],4
        jb      f7a0_nofs
        mov     dword ptr [edi],00544146h       ; "FAT",0
f7a0_nofs:
        mov     word ptr [ebp+F_EBX],4002h      ; LFNs live here, and
        mov     word ptr [ebp+F_ECX],255        ; case is preserved
        mov     word ptr [ebp+F_EDX],260
        mov     word ptr [ebp+F_EAX],0
        jmp     i21_ok

;--- 71A1h find close -------------------------------------------------
fn71A1:
        movzx   ebx,word ptr [ebp+F_EBX]
        cmp     ebx,FIND_MAX
        jae     f71_badh
        imul    ebx,FH_SIZE
        add     ebx,OFFSET find_tbl
        mov     byte ptr [ebx+FH_INUSE],0
        jmp     i21_ok

;--- 7147h get current directory, the long form -----------------------
; DL = drive (0 = default, 1 = A:), ESI -> buffer.  47h style: no
; drive, no leading backslash, root = "".  The stored cwd is pure 8.3
; aliases (walk_to canonicalizes, deliberately), so the long rendering
; is DERIVED: walk the path from the root and collect each
; component's primary name.
fn7147:
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     f747_expl
        mov     al,[cur_drive]
        jmp     short f747_sel
f747_expl:
        dec     al
        cmp     al,MAX_DRIVES
        jae     f747_bad
f747_sel:
        call    vol_select
        jc      f747_bad

        ; seed the resolver arrays from the cwd: all 8.3, no long
        ; text, nothing stale left pointing anywhere
        mov     ecx,[cwd_cnt]
        mov     [rp_cnt],ecx
        push    esi
        push    edi
        mov     esi,OFFSET cwd_names
        mov     edi,OFFSET rp_names
        imul    ecx,11
        rep     movsb
        mov     edi,OFFSET rp_lptr
        xor     eax,eax
        mov     ecx,MAX_DEPTH
f747_zp:
        mov     dword ptr [edi],eax
        add     edi,4
        loop    f747_zp
        mov     edi,OFFSET rp_lflag
        mov     ecx,MAX_DEPTH
f747_zf:
        mov     [edi],al
        inc     edi
        loop    f747_zf
        pop     edi
        pop     esi

        mov     eax,[ebp+F_ESI]
        mov     [prd_out],eax
        mov     byte ptr [prd_mode],1
        call    path_render
        jc      f747_np
        jmp     i21_ok
f747_bad:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail
f747_np:
        jmp     i21_fail                ; AX from path_render

;--- 7160h truename ---------------------------------------------------
; CL = 0 canonical, 1 the 8.3 form (the same thing here - canonical
; IS 8.3), 2 the long form.  ESI -> path, EDI -> the caller's buffer.
; Existence is only demanded of the intermediates, as ever with
; truename; a missing or wildcarded final component renders as typed.
fn7160:
        mov     al,[ebp+F_ECX]
        cmp     al,2
        ja      f760_bad
        xor     ah,ah
        cmp     al,2
        jne     f760_m
        mov     ah,1
f760_m:
        mov     [prd_mode],ah
        mov     esi,[ebp+F_ESI]
        call    resolve_core
        jc      f760_err
        mov     edi,[ebp+F_EDI]
        mov     al,[cur_vol]
        add     al,'A'
        mov     [edi],al
        mov     byte ptr [edi+1],':'
        mov     byte ptr [edi+2],'\'
        add     edi,3
        mov     [prd_out],edi
        call    path_render
        jc      f760_err
        jmp     i21_ok
f760_bad:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f760_err:
        jmp     i21_fail

;---------------------------------------------------------------------
; path_render - walk rp_names[0..rp_cnt-1] from the volume root and
; write each component at [prd_out] (advanced as it goes), separated
; by '\', NUL-terminated.  [prd_mode] = 0 renders every component as
; its 8.3 alias, 1 as its primary name (the LFN when it wears one).
; A final component that does not exist, or holds wildcards, renders
; from what was typed instead.  Intermediates must exist as
; directories: CF with AX = ERR_NOPATH otherwise.
;---------------------------------------------------------------------
path_render:
        pushad
        call    ctx_root
        xor     edx,edx
prd_lp:
        cmp     edx,[rp_cnt]
        jae     prd_done
        or      edx,edx                 ; a separator before every
        jz      prd_nosep               ; component but the first
        mov     edi,[prd_out]
        mov     byte ptr [edi],'\'
        inc     edi
        mov     [prd_out],edi
prd_nosep:
        mov     edi,edx
        imul    edi,11
        add     edi,OFFSET rp_names

        mov     eax,[rp_cnt]            ; a wildcarded FINAL component
        dec     eax                     ; is never looked up - truename
        cmp     edx,eax                 ; keeps the wildcards
        jne     prd_mid
        call    chk_wild
        jc      prd_astyped
prd_mid:
        cmp     byte ptr [rp_lflag+edx],0
        je      prd_nolong
        mov     eax,[rp_lptr+edx*4]     ; only the long text can spell
        mov     [srch_long],eax         ; this one - arm the match
        mov     eax,[rp_llen+edx*4]
        mov     [srch_llen],eax
prd_nolong:
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        call    fat_search
        jc      prd_miss

        ; append the primary name in long mode, the alias otherwise
        push    edi
        mov     edi,[prd_out]
        cmp     byte ptr [prd_mode],0
        je      prd_as83
        cmp     byte ptr [lfn_ok],0
        je      prd_as83
        push    esi
        mov     esi,OFFSET lfn_buf
prd_lcp:
        mov     al,[esi]
        or      al,al
        jz      prd_lcpd
        mov     [edi],al
        inc     esi
        inc     edi
        jmp     short prd_lcp
prd_lcpd:
        pop     esi
        jmp     short prd_app
prd_as83:
        call    name83_str              ; ESI = the live entry
prd_a83:
        cmp     byte ptr [edi],0        ; advance over what it wrote
        je      prd_app
        inc     edi
        jmp     short prd_a83
prd_app:
        mov     [prd_out],edi
        pop     edi

        mov     eax,[rp_cnt]            ; done when that was the last
        dec     eax
        cmp     edx,eax
        je      prd_done
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jz      prd_np                  ; a file mid-path
        call    de_getclus
        or      eax,eax
        jz      prd_root
        call    ctx_chain
        jmp     short prd_step
prd_root:
        call    ctx_root
prd_step:
        inc     edx
        jmp     prd_lp

prd_miss:
        mov     eax,[rp_cnt]            ; only the FINAL component may
        dec     eax                     ; be missing
        cmp     edx,eax
        jne     prd_np
prd_astyped:
        ; render the typed text when we still have it, else the pattern
        push    edi                     ; EDI -> the 11-byte pattern
        mov     eax,[rp_lptr+edx*4]
        or      eax,eax
        jnz     prd_typed
        mov     esi,edi
        mov     edi,[prd_out]
        call    name83_str
prd_pad:
        cmp     byte ptr [edi],0
        je      prd_tfin
        inc     edi
        jmp     short prd_pad
prd_typed:
        mov     esi,eax
        mov     ecx,[rp_llen+edx*4]
        mov     edi,[prd_out]
prd_tcp:
        or      ecx,ecx
        jz      prd_tfin
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jmp     short prd_tcp
prd_tfin:
        mov     [prd_out],edi
        pop     edi

prd_done:
        mov     edi,[prd_out]
        mov     byte ptr [edi],0
        popad
        clc
        ret
prd_np:
        popad
        mov     ax,ERR_NOPATH
        stc
        ret

;--- 716Ch open or create, long-name aware ----------------------------
; BX = mode (the low 3 bits honoured), CX = attributes for a creation,
; DX = action: bit 0 open-if-exists, bit 1 truncate-if-exists, bit 4
; create-if-missing.  ESI -> path.  DI (the alias hint) is ignored -
; aliases are the kernel's business here.
; -> AX = handle, CX = 1 opened / 2 truncated / 3 created.
;
; Composed out of 3Dh and 3Ch: the classic handlers are CALLED on the
; current frame - their i21_ok/i21_fail tails just RET back here -
; with the path staged in F_EDX, which is restored afterwards so the
; caller's DX comes home intact.
fn716C:
        mov     al,[ebp+F_EBX]
        and     al,3
        cmp     al,3
        je      f6c_deny                ; 3 is no access mode
        mov     eax,[ebp+F_EDX]
        mov     [f6c_dx],eax

        ; A device short-circuits the whole exists/create/truncate
        ; question: it is always there, it is never truncated, and
        ; 3Dh already knows how to open one.
        mov     esi,[ebp+F_ESI]
        call    dev_find
        jc      f6c_look
        mov     byte ptr [f6c_act],1
        mov     al,[ebp+F_EBX]
        and     al,3
        mov     [ebp+F_EAX],al
        mov     eax,[ebp+F_ESI]
        mov     [ebp+F_EDX],eax
        call    fn3D
        jmp     f6c_fin

f6c_look:
        mov     esi,[ebp+F_ESI]         ; does it exist?
        call    resolve_parent
        jc      f6c_err                 ; AX = the resolver's verdict
        cmp     byte ptr [rp_noname],0
        jne     f6c_deny
        mov     edi,OFFSET name_buf
        call    chk_wild
        jc      f6c_deny
        mov     byte ptr [srch_attr],ATTR_DIR
        xor     ebx,ebx
        mov     edi,OFFSET name_buf
        call    fat_search
        jc      f6c_miss
        test    byte ptr [esi+DE_ATTR],ATTR_DIR
        jnz     f6c_deny                ; a directory wears that name

        test    byte ptr [ebp+F_EDX],2  ; truncate wins over open
        jnz     f6c_trunc
        test    byte ptr [ebp+F_EDX],1
        jz      f6c_exists
        mov     byte ptr [f6c_act],1    ; open in place: 3Dh wants the
        mov     al,[ebp+F_EBX]          ; mode in AL, the path in EDX
        and     al,3
        mov     [ebp+F_EAX],al
        mov     eax,[ebp+F_ESI]
        mov     [ebp+F_EDX],eax
        call    fn3D
        jmp     short f6c_fin
f6c_trunc:
        mov     byte ptr [f6c_act],2
        jmp     short f6c_creat
f6c_miss:
        test    byte ptr [ebp+F_EDX],10h
        jz      f6c_nofile
        mov     byte ptr [f6c_act],3
f6c_creat:
        mov     eax,[ebp+F_ESI]         ; 3Ch takes the path from EDX;
        mov     [ebp+F_EDX],eax         ; F_ECX already holds our CX,
        call    fn3C                    ; which IS the attributes
f6c_fin:
        mov     eax,[f6c_dx]            ; the caller's DX, back in place
        mov     [ebp+F_EDX],eax
        test    dword ptr [ebp+F_FLAGS],FL_CF
        jnz     f6c_out                 ; failed: AX is the error
        movzx   eax,byte ptr [f6c_act]
        mov     word ptr [ebp+F_ECX],ax
        cmp     byte ptr [f6c_act],1    ; a created or truncated handle
        je      f6c_out                 ; honours the asked-for mode
        movzx   ebx,word ptr [ebp+F_EAX]
        call    sft_get
        jc      f6c_out
        movzx   eax,byte ptr [ebp+F_EBX]
        and     eax,3
        mov     [esi+SFT_MODE],eax
f6c_out:
        ret
f6c_exists:
        mov     ax,ERR_EXISTS
        jmp     i21_fail
f6c_nofile:
        mov     ax,ERR_NOFILE
        jmp     i21_fail
f6c_deny:
        mov     ax,ERR_ACCESS
        jmp     i21_fail
f6c_err:
        jmp     i21_fail

;---------------------------------------------------------------------
; f71_m83 - the classic 11-byte compare: EDI = pattern ('?' wild),
;           ESI = name.  CF clear on a match.  Preserves everything.
;---------------------------------------------------------------------
f71_m83:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     ecx,11
f7m_lp:
        mov     al,[edi]
        cmp     al,'?'
        je      f7m_w
        cmp     al,[esi]
        jne     f7m_no
f7m_w:
        inc     esi
        inc     edi
        loop    f7m_lp
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret
f7m_no:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; name83_str - render the 11-byte name at ESI as "NAME.EXT", ASCIIZ,
;              at EDI.  Preserves everything.
;---------------------------------------------------------------------
name83_str:
        push    eax
        push    ebx
        push    esi
        push    edi
        xor     ebx,ebx
n83_n:
        cmp     ebx,8
        jae     n83_dot
        mov     al,[esi+ebx]
        cmp     al,' '
        je      n83_dot
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short n83_n
n83_dot:
        cmp     byte ptr [esi+8],' '
        je      n83_z
        mov     byte ptr [edi],'.'
        inc     edi
        mov     ebx,8
n83_e:
        cmp     ebx,11
        jae     n83_z
        mov     al,[esi+ebx]
        cmp     al,' '
        je      n83_z
        mov     [edi],al
        inc     edi
        inc     ebx
        jmp     short n83_e
n83_z:
        mov     byte ptr [edi],0
        pop     edi
        pop     esi
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; lfn_match - does the long pattern match the name?
;   ESI = pattern text, ECX = its length, EDI = ASCIIZ name
;   '*' = any run; '?' = one character, or none at a dot or the end
;   (and never the dot itself); letters fold case.  CF clear = match.
;   Recursive on '*'.
;---------------------------------------------------------------------
lfn_match:
        or      ecx,ecx
        jz      lm_pend
        mov     al,[esi]
        cmp     al,'*'
        je      lm_star
        cmp     al,'?'
        je      lm_q
        mov     ah,[edi]
        or      ah,ah
        jz      lm_no                   ; name over, pattern is not
        call    fsr_fold
        xchg    al,ah
        call    fsr_fold
        cmp     al,ah
        jne     lm_no
lm_step:
        inc     esi
        inc     edi
        dec     ecx
        jmp     lfn_match
lm_q:
        mov     ah,[edi]
        or      ah,ah
        jz      lm_qnone                ; at the end: matches nothing
        cmp     ah,'.'
        jne     lm_step                 ; ordinary: eat one character
lm_qnone:
        inc     esi                     ; parked at a dot: the '?' just
        dec     ecx                     ; evaporates
        jmp     lfn_match
lm_star:
        inc     esi                     ; past the '*' itself
        dec     ecx
lm_star1:
        push    esi
        push    edi
        push    ecx
        call    lfn_match               ; try every suffix, shortest
        pop     ecx                     ; first
        pop     edi
        pop     esi
        jnc     lm_yes
        cmp     byte ptr [edi],0
        je      lm_no
        inc     edi
        jmp     short lm_star1
lm_pend:
        cmp     byte ptr [edi],0        ; both ended = match
        jne     lm_no
lm_yes:
        clc
        ret
lm_no:
        stc
        ret

; 71h find state
f71_hdl     dd  0                       ; current handle, while working
f71_hidx    dd  0                       ; its index (the AX we hand out)
f71_first   db  0                       ; this walk is a find-FIRST
            db  0,0,0
f71_name    db  16 dup(0)               ; short name rendered as text
find_tbl    db  FIND_MAX * FH_SIZE dup(0)

; 71h write-side state
f6c_dx      dd  0                       ; 716Ch: the caller's DX parked
f6c_act     db  0                       ; ...and the action taken
prd_mode    db  0                       ; path_render: 1 = long names
            db  0,0
prd_out     dd  0                       ; path_render's output cursor

;=====================================================================
; SFT management
;=====================================================================
sft_init:
        push    eax
        push    ecx
        push    edi
        mov     edi,OFFSET sft_tbl
        xor     eax,eax
        mov     ecx,(MAX_HANDLES * SFT_LEN) / 4
        rep     stosd

        ; handles 0,1,2 = CON
        mov     edi,OFFSET sft_tbl
        mov     ecx,3
si_std:
        mov     dword ptr [edi+SFT_FLAGS],SFTF_INUSE OR SFTF_DEVICE
        add     edi,SFT_LEN
        loop    si_std
        pop     edi
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; sft_get - EBX = handle -> ESI = table entry.  CF if bad or unused.
;---------------------------------------------------------------------
sft_get:
        cmp     ebx,MAX_HANDLES
        jae     sg_bad
        mov     esi,ebx
        imul    esi,SFT_LEN
        add     esi,OFFSET sft_tbl
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      sg_bad
        clc
        ret
sg_bad:
        stc
        ret

;---------------------------------------------------------------------
; sft_new - claim a free entry.  -> AX = handle, ESI = entry.  CF if
;           the table is full.
;---------------------------------------------------------------------
sft_new:
        push    ebx
        mov     ebx,3                   ; never hand out the std handles
sn_lp:
        cmp     ebx,MAX_HANDLES
        jae     sn_full
        mov     esi,ebx
        imul    esi,SFT_LEN
        add     esi,OFFSET sft_tbl
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      sn_have
        inc     ebx
        jmp     short sn_lp
sn_have:
        ; stamp the owner NOW, at the one place a handle is ever
        ; claimed, so that termination can tell whose it was
        mov     eax,[cur_own]
        mov     [esi+SFT_OWNER],eax
        mov     ax,bx
        pop     ebx
        clc
        ret
sn_full:
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; std_in - read one byte through handle 0, wherever it points.
;          A device slot is the keyboard (blocks; AX comes back as
;          IOS_CONIN delivers it).  A file slot - stdin redirected -
;          yields the next byte with AH = 0.  CF set at end of file;
;          the keyboard never sets it.  All other registers preserved.
;---------------------------------------------------------------------
std_in:
        test    dword ptr [sft_tbl+SFT_FLAGS],SFTF_DEVICE
        jz      sin_file
        cmp     dword ptr [sft_tbl+SFT_DEVID],DEV_CON
        jne     sin_dev
sin_key:
        call    dos_conin               ; the wait that lets INT 28h out
        cmp     al,3                    ; ^C from the keyboard breaks
        je      sin_break
        clc
        ret
sin_break:
        call    ctrl_break              ; returning means carry on
        jmp     short sin_key
sin_dev:                                ; stdin redirected to NUL or a port
        push    edx
        mov     edx,dword ptr [sft_tbl+SFT_DEVID]
        call    dev_in
        pop     edx                     ; the pop leaves CF alone
        ret
sin_file:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     esi,OFFSET sft_tbl
        call    sft_seldrv
        jc      sin_eof
        mov     eax,[esi+SFT_POS]
        cmp     eax,[esi+SFT_SIZE]
        jae     sin_eof
        mov     edi,OFFSET std_ch
        mov     ecx,1
        call    file_chunk
        jc      sin_eof
        or      eax,eax
        jz      sin_eof
        movzx   eax,byte ptr [std_ch]
        clc
        jmp     short sin_out
sin_eof:
        stc
sin_out:
        pop     edi                     ; pops leave CF alone
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; std_out - write AL through handle 1, wherever it points.  A device
;           slot is the screen; a file slot appends at the file
;           position.  All registers preserved.  A full disk simply
;           drops the byte - the console functions this serves have
;           no error path to report one, and never did.
;---------------------------------------------------------------------
std_out:
        test    dword ptr [sft_tbl+SFT_LEN+SFT_FLAGS],SFTF_DEVICE
        jz      sout_file
        push    edx
        mov     edx,dword ptr [sft_tbl+SFT_LEN+SFT_DEVID]
        call    dev_out
        pop     edx
        ret
sout_file:
        push    ecx
        push    esi
        mov     [std_ch],al
        mov     esi,OFFSET std_ch
        mov     ecx,1
        call    std_write
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; std_write - write ECX bytes at ESI to the FILE in handle 1 (the
;             caller has already checked it is not a device).  Short
;             writes and I/O errors are absorbed, as above.
;             All registers preserved.
;---------------------------------------------------------------------
std_write:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi
        or      ecx,ecx
        jz      sw_done
        mov     edi,esi                 ; EDI = the caller's bytes
        mov     esi,OFFSET sft_tbl + SFT_LEN
        call    sft_seldrv
        jc      sw_done
sw_more:
        or      ecx,ecx
        jz      sw_mark
        push    ecx
        call    file_wchunk             ; -> EAX = bytes taken
        pop     ecx
        jc      sw_done
        or      eax,eax
        jz      sw_mark                 ; disk full - keep what landed
        add     edi,eax
        sub     ecx,eax
        jmp     short sw_more
sw_mark:
        or      dword ptr [sft_tbl+SFT_LEN+SFT_FLAGS],SFTF_DIRTY
sw_done:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; F0h - PM-DOS console extension.
;       AL = 0: clear screen    AL = 1: get cursor -> DH,DL
;       AL = 2: set cursor from DH,DL
;=====================================================================
;=====================================================================
; 73h - the FAT32 extended calls.  Only 7303h, get extended free
;       space, is here: EDX -> any path naming the drive ("C:\" will
;       do), EDI -> the structure, ECX = its size in bytes.
;
; This answers 36h's oldest limitation.  36h hands its cluster counts
; back in 16-bit registers, so it saturates at 65535 and calls a 40MB
; volume 32MB - which is exactly why FORMAT reports numbers it worked
; out itself rather than asking.  Every field here is 32 bits wide.
;
; PM-DOS compresses nothing, so the "without adjustment for
; compression" fields carry the same values as the plain ones, which
; is what a system with no compression is supposed to say.  The record
; is built in full here and then copied out under the caller's own
; length, so a program that offers a short buffer gets a short answer
; instead of a stack full of holes.
;
; Any other 73h subfunction fails with "invalid function", the
; documented way of saying a FAT32 call is not implemented.
;=====================================================================
; (the F73_* record offsets live in PMDOS.INC - programs need them)

fn73:
        mov     al,[ebp+F_EAX]
        cmp     al,03h
        jne     f73_bad

;--- which drive?  The path names it, or the default one does -------
        mov     esi,[ebp+F_EDX]
        or      esi,esi
        jz      f73_dflt
        cmp     byte ptr [esi+1],':'
        jne     f73_dflt
        mov     al,[esi]
        or      al,20h
        sub     al,'a'
        cmp     al,MAX_DRIVES
        jae     f73_bad
        jmp     short f73_sel
f73_dflt:
        mov     al,[cur_drive]
f73_sel:
        call    vol_select
        jc      f73_bad

;--- build the whole record, then hand over as much as was asked ----
        mov     edi,OFFSET f73_rec
        xor     eax,eax
        mov     ecx,F73_SIZE / 4
        rep     stosd

        mov     eax,[fat_spc]
        mov     dword ptr [f73_rec+F73_SPC],eax
        mov     dword ptr [f73_rec+F73_BPS],512
        mov     ebx,eax                 ; EBX = sectors per cluster

        mov     eax,[fat_totclus]
        mov     dword ptr [f73_rec+F73_TOTCL],eax
        mov     dword ptr [f73_rec+F73_TOTAU],eax
        imul    eax,ebx
        mov     dword ptr [f73_rec+F73_TOTSEC],eax

        call    fat_freecount
        mov     dword ptr [f73_rec+F73_FREECL],eax
        mov     dword ptr [f73_rec+F73_FREEAU],eax
        imul    eax,ebx
        mov     dword ptr [f73_rec+F73_FREESEC],eax

        mov     ecx,[ebp+F_ECX]         ; never more than was offered
        cmp     ecx,F73_SIZE
        jbe     f73_len
        mov     ecx,F73_SIZE
f73_len:
        jecxz   f73_short
        mov     esi,OFFSET f73_rec
        mov     edi,[ebp+F_EDI]
        rep     movsb
        jmp     i21_ok
f73_short:
        mov     ax,ERR_FUNC             ; no room for even the version
        jmp     i21_fail
f73_bad:
        mov     ax,ERR_FUNC
        jmp     i21_fail

fnF0:
        mov     al,[ebp+F_EAX]
        or      al,al
        jz      fF0_cls
        cmp     al,1
        je      fF0_get
        cmp     al,2
        je      fF0_set
        cmp     al,3
        je      fF0_size
        jmp     i21_unsup
fF0_cls:
        call    dword ptr ds:[IOS_CLS]
        jmp     i21_ok
fF0_get:
        call    dword ptr ds:[IOS_GETCUR]
        mov     [ebp+F_EDX],dx
        jmp     i21_ok
fF0_set:
        mov     dx,[ebp+F_EDX]
        call    dword ptr ds:[IOS_SETCUR]
        jmp     i21_ok

; AL = 3: how big is the screen NOW - DH = rows, DL = columns.  It has
; to be asked rather than assumed, because a DOS box can change it:
; the answer comes out of the BIOS data area every time.
fF0_size:
        call    dword ptr ds:[IOS_SCRSIZE]
        mov     [ebp+F_EDX],dx
        jmp     i21_ok

;=====================================================================
; F1h - PM-DOS kernel information.
;
;       AL = 0: -> BH = major version, BL = minor version
;       AL = 1: DL = drive (0 = default, 1 = A:)
;               -> AL = the volume's FAT type: 12, 16 or 32
;       AL = 2: DL = drive - remount it, discarding cached state
;       AL = 3: EDX -> buffer for the interpreter's own path
;       AL = 4: -> EAX = extended memory in KB as the BIOS reported it,
;                  EBX = the highest usable linear address
;
; 30h has to keep lying about being DOS 5 so that ordinary programs
; recognise us, which leaves nothing there to report what the kernel
; itself is.  VER asks here instead: COMMAND.COM and PMDOS.SYS are
; separate binaries and can perfectly well be different versions, so
; the interpreter has no business reporting its own number as the
; system's.  AL = 1 is what lets SYS refuse to marry a FAT12 boot
; sector to a FAT32 volume without reimplementing the cluster-count
; rule the kernel already knows.
;=====================================================================
fnF1:
        mov     al,[ebp+F_EAX]
        or      al,al
        jz      fF1_ver
        cmp     al,1
        je      fF1_vol
        cmp     al,2
        je      fF1_rem
        cmp     al,3
        je      fF1_shell
        cmp     al,4
        je      fF1_mem
        cmp     al,5
        je      fF1_crit
        cmp     al,6
        je      fF1_drv
        cmp     al,7
        je      fF1_null
        cmp     al,8
        je      fF1_mcb
        jmp     i21_unsup
fF1_ver:
        mov     word ptr [ebp+F_EBX],PMDOS_MAJOR * 256 + PMDOS_MINOR
        jmp     i21_ok

;--- AL=7: is the null-pointer guard armed? -> AL = 1 if it is.
;--- Worth a call of its own because the guard is four debug registers
;--- and an emulator is entitled not to implement those.  Without this,
;--- a test program that writes through a null pointer and survives
;--- cannot tell "PM-DOS did not arm it" from "the CPU underneath us
;--- ignored it" - and those are a bug and a footnote respectively.
fF1_null:
        call    dword ptr ds:[IOS_NULLGET]
        mov     [ebp+F_EBX],ebx         ; DR7, straight out of the CPU
        mov     [ebp+F_EAX],al
        jmp     i21_ok
fF1_vol:
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     fF1_expl
        mov     al,[cur_drive]
        jmp     short fF1_sel
fF1_expl:
        dec     al
        cmp     al,MAX_DRIVES
        jae     fF1_baddrv
fF1_sel:
        call    vol_select
        jc      fF1_baddrv
        mov     al,[fat_type]
        mov     [ebp+F_EAX],al
        jmp     i21_ok
;--- AL=2: remount drive DL (0 = default, 1 = A:).  For FORMAT: the
;--- volume has been rewritten UNDERNEATH the kernel, so every cached
;--- notion of it is discarded - never flushed, because the dirt
;--- describes a dead filesystem - and the BPB is read afresh.  The
;--- drive's current directory comes back at the root, fat_mount's
;--- own doing.
fF1_rem:
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     fF1_rexp
        mov     al,[cur_drive]
        jmp     short fF1_rsel
fF1_rexp:
        dec     al
        cmp     al,MAX_DRIVES
        jae     fF1_baddrv
fF1_rsel:
        call    vol_select              ; swap its state in (a failed
                                        ; mount still swaps - fine)
        mov     byte ptr [fatsec_dirty],0
        mov     byte ptr [fat12_dirty],0
        mov     dword ptr [fatsec_lba],-1
        mov     dword ptr [sec_lba],-1
        mov     dword ptr [clusbuf_clus],-1
        mov     dword ptr [dc_curord],-1
        mov     byte ptr [fat_ok],0
        call    vol_select              ; and mount it afresh
        jc      fF1_baddrv
        jmp     i21_ok

;--- AL=3: the interpreter's own path, ASCIIZ into the buffer at EDX
;--- (SHELLNAME_MAX bytes is always enough).  COMSPEC has to name the
;--- interpreter that is actually running, and after a CONFIG.SYS
;--- SHELL= line the KERNEL is the only thing that knows which one it
;--- loaded - the shell cannot see its own command line's name.
fF1_shell:
        mov     edi,[ebp+F_EDX]
        mov     esi,OFFSET shell_name
        mov     ecx,SHELLNAME_MAX - 1
fF1_scp:
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      fF1_sdone
        inc     esi
        inc     edi
        dec     ecx
        jnz     fF1_scp
        mov     byte ptr [edi],0        ; terminated even if truncated
fF1_sdone:
        jmp     i21_ok

;--- AL=4: what the machine has, as opposed to what the arena has left.
;--- 36h reports disks and there was never an equivalent for memory:
;--- real DOS's answer was "walk the MCB chain and add it up", which
;--- tells you about the arena and nothing about the machine.  MEM.EXE
;--- prints both and they should agree to within a rounding error.
fF1_mem:
        mov     eax,dword ptr ds:[GV_MEMKB]
        mov     [ebp+F_EAX],eax
        mov     eax,[arena_top]
        mov     [ebp+F_EBX],eax
        jmp     i21_ok

;--- AL=8: a memory arena, one block at a time.  DL = which arena
;--- (0 = the kernel's, above 2MB; 1 = the conventional one a DOS box
;--- lives in), EBX = which block counting from zero, EDI -> a 16-byte
;--- record.  CF when EBX is past the end, so a caller walks until it
;--- is told to stop and cannot loop for ever on a damaged chain.
;---
;--- THIS EXISTS BECAUSE MEM CAN NO LONGER READ EITHER ARENA.  It
;--- walked the headers itself - the kernel's from the pointer 52h
;--- hands out, and the conventional chain where it lies below 1MB -
;--- which is exactly what MEM did under real DOS, and worked here for
;--- the same reason: the program and the kernel shared an address
;--- space.  They do not now.  The fix is the one v0.19 arrived at when
;--- MEM stopped being an internal command: a report that has to reach
;--- inside the kernel is a report proving the kernel has no way to
;--- ask.  It had no way to ask.  Now it has.
;---
;--- THE RECORD IS NOT EITHER HEADER, and that is deliberate.  The two
;--- chains have different shapes - sixteen flat bytes with an owner id
;--- up here, a byte signature and a paragraph count down there - and
;--- handing out both raw would put that difference in every caller.
;--- One shape, and the kernel does the translating.
fF1_mcb:
        mov     ecx,[ebp+F_EBX]         ; which block
        cmp     byte ptr [ebp+F_EDX],0
        jne     fF1_mlow

;--- the kernel's arena ----------------------------------------------
        mov     ebx,ARENA_BASE
fF1_mwalk:
        cmp     ebx,[arena_top]
        jae     fF1_mend
        jecxz   fF1_mgot
        cmp     dword ptr [ebx+MCB_SIG],SIG_END
        je      fF1_mend                ; the terminator ends the walk, and
        dec     ecx                     ; is itself the last block reported
        mov     eax,[ebx+MCB_SIZE]
        add     eax,MCB_LEN
        add     ebx,eax
        jmp     short fF1_mwalk
fF1_mgot:
        mov     eax,[ebx+MCB_SIG]
        mov     dword ptr [f8_rec+F8_SIG],eax
        mov     eax,[ebx+MCB_SIZE]
        mov     dword ptr [f8_rec+F8_SIZE],eax
        mov     eax,[ebx+MCB_OWNER]
        mov     dword ptr [f8_rec+F8_OWNER],eax
        mov     dword ptr [f8_rec+F8_ADDR],ebx
        jmp     short fF1_mout

;--- and the one below 1MB, in the same terms -------------------------
fF1_mlow:
        mov     ebx,V86_TPA
fF1_lwalk:
        cmp     ebx,V86_TPA_TOP
        jae     fF1_mend
        movzx   eax,byte ptr [ebx+RMCB_SIG]
        cmp     al,'M'
        je      fF1_lone
        cmp     al,'Z'
        jne     fF1_mend                ; not a header: the chain is over
fF1_lone:
        jecxz   fF1_lgot
        cmp     al,'Z'
        je      fF1_mend                ; 'Z' is the last one there is
        dec     ecx
        movzx   eax,word ptr [ebx+RMCB_LEN]
        shl     eax,4
        add     ebx,eax
        add     ebx,RMCB_SIZE
        jmp     short fF1_lwalk
fF1_lgot:
        ; DOWN THERE THE SIGNATURE SAYS WHERE THE CHAIN ENDS AND THE
        ; OWNER SAYS WHETHER THE BLOCK IS FREE; up here the signature
        ; says both.  So the record is filled from the owner, and 'Z'
        ; has already done its other job by stopping the walk.
        mov     edx,SIG_USED
        cmp     word ptr [ebx+RMCB_OWN],0
        jne     fF1_lsig
        mov     edx,SIG_FREE
fF1_lsig:
        mov     dword ptr [f8_rec+F8_SIG],edx
        movzx   eax,word ptr [ebx+RMCB_LEN]
        shl     eax,4
        mov     dword ptr [f8_rec+F8_SIZE],eax
        movzx   eax,word ptr [ebx+RMCB_OWN]
        mov     dword ptr [f8_rec+F8_OWNER],eax
        mov     dword ptr [f8_rec+F8_ADDR],ebx

fF1_mout:
        mov     edi,[ebp+F_EDI]
        mov     esi,OFFSET f8_rec
        mov     ecx,F8_LEN / 4
        rep     movsd
        jmp     i21_ok
fF1_mend:
        mov     ax,ERR_NOFILE           ; "there is no such one"
        jmp     i21_fail

;--- AL=5: an INT 24h handler is leaving without returning.
;---
;--- The kernel locks INT 24h against itself while a handler runs, so
;--- that an error raised inside one is failed rather than asked
;--- about, and it unlocks when the handler IRETs.  A handler that
;--- goes home instead - COMMAND longjmps back to its prompt for both
;--- ^C and Abort, because the alternative is reloading the shell off
;--- the floppy that just failed - would leave the lock on for good
;--- and every critical error after it would answer itself.  So it
;--- says so first.  Real DOS's error-mode flag lives in the SDA and
;--- gets put back by hand for exactly the same reason.
fF1_crit:
        mov     byte ptr [crit_busy],0
        mov     byte ptr ds:[DOS_ERRFLG],0

        ; AND THE HANDLER ITSELF IS NOT COMING BACK.  This call already
        ; meant "I am leaving by a route of my own" - it exists because
        ; COMMAND longjmps home from its critical-error handler and its
        ; ^C handler rather than IRETing, and the kernel's error-mode
        ; lock would otherwise be carried away with it.  A ring-3
        ; handler leaving that way strands one more thing: the frame
        ; r3_thunk parked for the IRET that is never going to happen,
        ; and the ring-0 stack pointer aimed just below it.  Both go
        ; back to the top here.
        call    r3_abandon

        ; The InDOS count is set to ONE and not to zero, and the one is
        ; this very call: i21_r0 is about to decrement it on the way
        ; out, so zero here would leave it at 0FFh - a kernel that
        ; believes it is 255 calls deep, which every TSR in the world
        ; would then refuse to pop up over.  Everything the abandoned
        ; handler was inside of is gone; this call is all that is left.
        mov     byte ptr ds:[DOS_INDOS],1
        jmp     i21_ok

;--- AL=6: describe logical drive DL (0 = A:) into the 12-byte record
;--- at EDI - the volume table entry, verbatim.  CX comes back as the
;--- number of letters that exist at all.
;---
;--- It is a diagnostic, and it is here because a partition scan that
;--- cannot be looked at is a partition scan nobody can debug: "DIR E:
;--- says invalid drive" does not distinguish a scan that missed the
;--- partition from a mount that failed on it.
fF1_drv:
        movzx   eax,byte ptr [drv_count]
        mov     [ebp+F_ECX],eax
        mov     al,[ebp+F_EDX]
        cmp     al,MAX_DRIVES
        jae     fF1_baddrv
        call    drv_slot                ; -> EBX
        mov     edi,[ebp+F_EDI]
        mov     esi,ebx
        mov     ecx,DRV_LEN / 4
        rep     movsd
        jmp     i21_ok

fF1_baddrv:
        mov     ax,ERR_BADDRIVE
        jmp     i21_fail

;=====================================================================
; 1Bh - allocation information for the default drive
; 1Ch - the same for the drive in DL
;
;   AL = sectors per cluster        CX = bytes per sector
;   DX = total clusters             DS:BX -> the media descriptor byte
;   AL = FFh means the drive is not there
;
; THE POINTER IS THE INTERESTING PART and it is the 52h lesson again:
; a call that answers an ADDRESS has to answer one its caller can hold,
; and half this kernel's callers are real-mode programs that cannot
; hold anything above 1MB.  So the byte lives in the guest-visible
; block with the list of lists, the flat address of it is what goes
; back in EBX, and vdm_dsptr turns that into DS:BX for a guest without
; needing to know which call it came from.
;
; DS:BX, not ES:BX.  DOS really did move the caller's DS here, which is
; why every program that calls 1Bh pushes DS first.
;=====================================================================
fn1B:
        mov     al,[cur_drive]
        jmp     short f1B_sel
fn1C:
        mov     al,[ebp+F_EDX]
        or      al,al
        jnz     f1C_expl
        mov     al,[cur_drive]
        jmp     short f1B_sel
f1C_expl:
        dec     al                      ; 1-based -> logical
        cmp     al,MAX_DRIVES
        jae     f1B_bad
f1B_sel:
        call    vol_select
        jc      f1B_bad

        ; No low block, no answer.  It is allocated at boot out of
        ; conventional memory and four paragraphs have never failed,
        ; but returning a kernel pointer here would be handing back
        ; exactly the address a 16-bit caller cannot use - which is
        ; the bug this call was written to avoid.
        movzx   edx,word ptr ds:[vdm_lolseg]
        or      edx,edx
        jz      f1B_bad
        shl     edx,4
        mov     al,[fat_media]
        mov     [edx+GLOL_MEDIA],al
        add     edx,GLOL_MEDIA
        mov     [ebp+F_EBX],edx

        mov     eax,[fat_spc]
        mov     byte ptr [ebp+F_EAX],al
        mov     word ptr [ebp+F_ECX],512

        mov     eax,[fat_totclus]       ; 16-bit register, so it saturates
        cmp     eax,0FFFFh              ; exactly as fn36 saturates
        jbe     f1B_tot
        mov     eax,0FFFFh
f1B_tot:
        mov     [ebp+F_EDX],ax
        jmp     i21_ok
f1B_bad:
        mov     byte ptr [ebp+F_EAX],0FFh
        jmp     i21_ok

;=====================================================================
; 38h - country information.  DS:DX -> a 34-byte buffer, BX = country.
;
; ONE COUNTRY, ANSWERED HONESTLY.  PM-DOS has no COUNTRY.SYS and no
; codepage machinery, so it knows the United States and says so: any
; other country asked for is refused rather than answered with US
; punctuation under a foreign code, because a program that formats a
; date from a wrong answer produces a wrong date and never finds out.
;
; The case-map field is a FAR CALL a program may make to uppercase a
; character.  It points at the RETF in the guest block, so the call
; returns having done nothing - which is right for a codepage with no
; case mapping beyond ASCII, and is in any event what a caller can
; survive.  Zero there would be a call into the interrupt table.
;=====================================================================
fn38:
        ; AL = 0 asks about the current country; AL = FFh means the
        ; code is in BX.  Setting a country is the same call with
        ; DX = FFFFh, and there is only one to set.
        mov     al,[ebp+F_EAX]
        or      al,al
        jz      f38_get
        cmp     al,0FFh
        jne     f38_wrong               ; a specific country, and not ours
        mov     ax,word ptr [ebp+F_EBX]
        cmp     ax,CTRY_USA
        jne     f38_wrong
f38_get:
        mov     ax,word ptr [ebp+F_EDX]
        cmp     ax,0FFFFh
        je      f38_set

        mov     edi,[ebp+F_EDX]
        mov     esi,OFFSET ctry_block
        mov     ecx,CTRY_SIZE
        rep     movsb

        ; The case-map far call, patched in rather than stored: the
        ; segment is not known until vdm_lolinit has run.
        mov     edi,[ebp+F_EDX]
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GLOL_RETF
        mov     [edi+CTRY_CASEMAP],eax

        mov     word ptr [ebp+F_EBX],CTRY_USA
        jmp     i21_ok
f38_set:
        mov     word ptr [ebp+F_EBX],CTRY_USA
        jmp     i21_ok
f38_wrong:
        mov     ax,ERR_NOFILE           ; what DOS says for a country it
        jmp     i21_fail                ; has no table for

;=====================================================================
; 5Ch - lock and unlock a region of a file.  AL = 0 lock, 1 unlock.
;
; A STUB THAT SUCCEEDS, AND IT IS NOT A LIE.  Locking exists so two
; programs cannot write the same record at once; PM-DOS runs one
; program at a time, so there is no second party for a lock to exclude
; and every lock would be granted anyway.  What the call must NOT do
; is fail: a database that asks to lock a record and is refused stops,
; and it stops for a conflict that cannot exist here.
;
; The handle is still checked.  Succeeding on a handle the program
; never opened would hide a real bug in it.
;=====================================================================
fn5C:
        mov     al,[ebp+F_EAX]
        cmp     al,1
        ja      f5C_func
        movzx   ebx,word ptr [ebp+F_EBX]
        call    sft_get                 ; CF set = not an open handle
        jc      f5C_handle
        jmp     i21_ok
f5C_handle:
        mov     ax,ERR_BADHANDLE
        jmp     i21_fail
f5C_func:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; 63h - the DBCS lead-byte table.  DS:SI -> it, and the table is a
;       list of start/end byte pairs ending in a zero word.
;
; An empty table is a complete answer, not a refusal: it says every
; byte is a single character, which is true of every codepage this
; kernel has.  Programs call this to find out whether they must walk
; strings two bytes at a time, and "no" is a useful answer.
;
; AL = 1 and AL = 2 are the DOS 4 interim console flag, which nothing
; outside the Far East ever set.
;=====================================================================
fn63:
        mov     al,[ebp+F_EAX]
        or      al,al
        jnz     f63_interim
        movzx   eax,word ptr ds:[vdm_lolseg]
        or      eax,eax
        jz      f63_none
        shl     eax,4
        add     eax,GLOL_DBCS
        mov     [ebp+F_ESI],eax
        mov     byte ptr [ebp+F_EAX],0
        jmp     i21_ok
f63_none:
        mov     ax,ERR_FUNC
        jmp     i21_fail
f63_interim:
        cmp     al,2
        ja      f63_none
        mov     byte ptr [ebp+F_EDX],0  ; the flag, always off
        jmp     i21_ok

;=====================================================================
; 67h - set the maximum handle count.  BX = how many.
;
; SUCCEEDS, ALWAYS, and the reason is a difference in shape rather
; than a shortcut.  In DOS this call grows the per-process handle
; table, whose default 20 is what programs are trying to escape.
; PM-DOS has no per-process table to grow - sft_tbl is global and
; MAX_HANDLES deep - so there is nothing for the call to do and no
; state for it to change.
;
; Refusing would be worse than useless: a program asks for a hundred
; handles at startup as insurance and then opens five.  Failing it
; stops a program that was never going to run out.  One that really
; does open more than MAX_HANDLES is told so at the open, which is
; the same answer DOS gives when the table it just grew fills up.
;=====================================================================
fn67:
        jmp     i21_ok

;=====================================================================
; 6Ch - extended open.  BX = mode, CX = attributes, DX = action,
;       DS:SI -> path.  AL must be 0.
;
; THE SAME CALL AS 716Ch WITH A SHORTER NAME.  The long-filename form
; was written first because that is what the LFN API needed, and it
; takes its arguments in exactly the registers the classic call uses -
; so this is an entry point rather than an implementation.  What 6Ch
; buys is software that predates the LFN API and still wants the one
; thing 3Dh and 3Ch cannot express between them: open-or-create as a
; single decision, with the answer to "which did you do" in CX.
;=====================================================================
fn6C:
        cmp     byte ptr [ebp+F_EAX],0
        jne     f6C_bad                 ; 6C00h is the only form defined
        jmp     fn716C
f6C_bad:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;=====================================================================
; 65h - extended country information.
;
; WHAT IS ANSWERED AND WHAT IS REFUSED, on purpose.  The subfunctions
; divide into two kinds.  The ones that DO something - capitalise a
; character, a string, decide whether a keystroke means yes - are
; answered, because they are small, they are exactly right for a
; single-codepage system, and a program that has to write its own is
; a program that will get a different answer from DOS.
;
; The ones that hand back a TABLE - 02h uppercase, 04h filename
; uppercase, 05h filename terminators, 06h collating sequence, 07h
; DBCS - are refused with "invalid function", and that is a considered
; answer rather than a gap left open.  Each is a far pointer to a
; 258-byte table that would have to live in conventional memory for a
; guest to read it, which is the better part of a kilobyte of the 624K
; a DOS box gets, spent on tables that for codepage 437 say exactly
; what the code in 20h/21h/22h already computes.  A program refused
; one of these falls back to its own, which is the same answer.  When
; something real asks, the place to put them is the guest block.
;=====================================================================
fn65:
        mov     al,[ebp+F_EAX]
        cmp     al,01h
        je      f65_info
        cmp     al,20h
        je      f65_capchar
        cmp     al,21h
        je      f65_capstr
        cmp     al,22h
        je      f65_capasciz
        cmp     al,23h
        je      f65_yesno
        mov     ax,ERR_FUNC             ; the table subfunctions, and
        jmp     i21_fail                ; anything not invented yet

;--- 01h: the country block, with the extended header on the front ---
; ES:DI -> buffer, CX = how big it is.  DOS truncates to the buffer
; rather than failing, which is what lets a DOS 4 program call a DOS 6
; kernel and get the part it understands.
f65_info:
        movzx   ecx,word ptr [ebp+F_ECX]
        cmp     ecx,7                   ; the header is written whole, so a
        jb      f65_small               ; buffer shorter than it is refused
                                        ; rather than half filled
        cmp     ecx,CTRY_SIZE + 7
        jbe     f65_fit
        mov     ecx,CTRY_SIZE + 7
f65_fit:
        mov     edi,[ebp+F_EDI]
        push    ecx

        mov     byte ptr [edi],1        ; info id
        mov     word ptr [edi+1],CTRY_SIZE + 7
        mov     word ptr [edi+3],CTRY_USA
        mov     word ptr [edi+5],437    ; the only codepage here

        ; The country block itself, and then the case-map pointer over
        ; the top of it - same patch as fn38, same reason.
        pop     ecx
        push    edi
        sub     ecx,7
        jbe     f65_hdronly
        add     edi,7
        mov     esi,OFFSET ctry_block
        rep     movsb
f65_hdronly:
        pop     edi
        movzx   ecx,word ptr [ebp+F_ECX]
        cmp     ecx,7 + CTRY_CASEMAP + 4
        jb      f65_done                ; the buffer stopped short of it
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GLOL_RETF
        mov     [edi+7+CTRY_CASEMAP],eax
f65_done:
        jmp     i21_ok
f65_small:
        mov     ax,ERR_FUNC
        jmp     i21_fail

;--- 20h: capitalise the character in DL ------------------------------
; ASCII only, and that is the whole of codepage 437's case mapping
; that DOS itself performed for the United States.  cfg_upcase is
; CONFIG.SYS's own, borrowed rather than written twice: a second
; upcase that drifted from the first would show up as a file the
; parser can open and this call cannot.
f65_capchar:
        mov     al,[ebp+F_EDX]
        call    cfg_upcase
        mov     byte ptr [ebp+F_EDX],al
        jmp     i21_ok

;--- 21h: capitalise CX bytes at DS:DX --------------------------------
f65_capstr:
        movzx   ecx,word ptr [ebp+F_ECX]
        jecxz   f65_strdone
        mov     esi,[ebp+F_EDX]
f65_strloop:
        mov     al,[esi]
        call    cfg_upcase
        mov     [esi],al
        inc     esi
        dec     ecx
        jnz     f65_strloop
f65_strdone:
        jmp     i21_ok

;--- 22h: the same, to the terminating zero ---------------------------
f65_capasciz:
        mov     esi,[ebp+F_EDX]
f65_azloop:
        mov     al,[esi]
        or      al,al
        jz      f65_azdone
        call    cfg_upcase
        mov     [esi],al
        inc     esi
        jmp     short f65_azloop
f65_azdone:
        jmp     i21_ok

;--- 23h: is DL a yes or a no? ----------------------------------------
; -> AX = 0 no, 1 yes, 2 neither.  The one place a country's answer
; really does change the behaviour of ordinary software, which is why
; DOS gave it a call of its own instead of leaving programs to compare
; against 'Y'.  Here the country is the United States and the letters
; are the English ones.
f65_yesno:
        mov     al,[ebp+F_EDX]
        call    cfg_upcase
        cmp     al,'Y'
        je      f65_yes
        cmp     al,'N'
        je      f65_no
        mov     word ptr [ebp+F_EAX],2
        jmp     i21_ok
f65_yes:
        mov     word ptr [ebp+F_EAX],1
        jmp     i21_ok
f65_no:
        mov     word ptr [ebp+F_EAX],0
        jmp     i21_ok

;=====================================================================
; Dispatch table 00h..6Ch
;=====================================================================
i21_table:
        dd      fn00,      fn01,      fn02,      fn03       ; 00-03
        dd      fn04,      fn05,      fn06,      fn07       ; 04-07
        dd      fn08,      fn09,      fn0A,      fn0B       ; 08-0B
        dd      fn0C,      fn0D,      fn0E,      i21_unsup  ; 0C-0F
        dd      i21_unsup, i21_unsup, i21_unsup, i21_unsup  ; 10-13
        dd      i21_unsup, i21_unsup, i21_unsup, i21_unsup  ; 14-17
        dd      i21_unsup, fn19,      fn1A,      fn1B       ; 18-1B
        dd      fn1C,      i21_unsup, i21_unsup, i21_unsup  ; 1C-1F
        dd      i21_unsup, i21_unsup, i21_unsup, i21_unsup  ; 20-23
        dd      i21_unsup, fn25,      i21_unsup, i21_unsup  ; 24-27
        dd      i21_unsup, i21_unsup, fn2A,      fn2B       ; 28-2B
        dd      fn2C,      fn2D,      fn2E,      fn2F       ; 2C-2F
        dd      fn30,      i21_unsup, i21_unsup, fn33       ; 30-33
        dd      fn34,      fn35,      fn36,      i21_unsup  ; 34-37
        dd      fn38,      fn39,      fn3A,      fn3B       ; 38-3B
        dd      fn3C,      fn3D,      fn3E,      fn3F       ; 3C-3F
        dd      fn40,      fn41,      fn42,      fn43       ; 40-43
        dd      fn44,      fn45,      fn46,      fn47       ; 44-47
        dd      fn48,      fn49,      fn4A,      fn4B       ; 48-4B
        dd      fn4C,      fn4D,      fn4E,      fn4F       ; 4C-4F
        dd      fn50,      fn51,      fn52,      i21_unsup  ; 50-53
        dd      fn54,      i21_unsup, fn56,      fn57       ; 54-57
        dd      fn58,      fn59,      fn5A,      fn5B       ; 58-5B
        dd      fn5C,      i21_unsup, i21_unsup, i21_unsup  ; 5C-5F
        dd      fn60,      i21_unsup, fn62,      fn63       ; 60-63
        dd      i21_unsup, fn65,      i21_unsup, fn67       ; 64-67
        dd      fn68,      i21_unsup, fn68,      i21_unsup  ; 68-6B
                                                            ; 6Ah is 68h under
                                                            ; its DOS 4 name;
                                                            ; 6Bh is the one DOS
                                                            ; documented as doing
                                                            ; nothing at all
        dd      fn6C                                        ; 6C

; AND THE TABLE REALLY IS THAT LONG.  i21_entry indexes this with any
; AH below I21_MAXFN, so a table one entry short does not fail at
; assembly or produce a wrong answer - it CALLS whatever dword follows
; it, which here would be the DOS state below.  Counted rather than
; trusted, because the comment on I21_MAXFN is only as good as the two
; tables agreeing with it.
IF ($ - i21_table) NE I21_MAXFN * 4
        .ERR    <i21_table does not cover 00h..I21_MAXFN-1>
ENDIF

;=====================================================================
; DOS state
;=====================================================================
dta_ptr     dd  OFFSET default_dta
default_dta db  43 dup(0)
cur_psp     dd  SHELL_PSP               ; whose handles are being opened

; OWNER IDS.  Handles and memory blocks are tagged with one of these
; rather than with a PSP address, because a nested program is loaded
; into the same slot as its parent and therefore has the SAME PSP
; ADDRESS - so the address cannot tell a child from the parent it
; would otherwise be closing files and freeing memory out from under.
; Monotonic: an id is never reused, so a stale tag can never match a
; live program.
cur_own     dd  1                       ; who is running now
next_own    dd  2                       ; the next id to hand out

; The 22h/23h/24h handlers lifted out of a terminating program's PSP,
; held here for the few instructions between "the PSP is still ours"
; and "the unwind is finished" - see term_vectors.
term_vec22  dd  0
term_vec23  dd  0
term_vec24  dd  0
exit_code   db  0
name_buf    db  11 dup(0)
name2_buf   db  11 dup(0)
open_mode   dd  0
open_stamp  dd  0                       ; the opened file's time and date
last_err    dw  0                       ; what 59h reports; set by i21_fail
            dw  0
con_pending db  0                       ; second byte of an extended key,
                                        ; waiting for the next raw-mode read
create_new  db  0                       ; 3Ch may truncate; 5Bh and 5Ah may not
verify_on   db  0                       ; 2Eh/54h.  See fn2E for why it is a
                                        ; setting and not a behaviour
            dw  0

f5a_tail    dd  0                       ; where 5Ah appends its name
f5a_seed    dd  0
f5a_try     dd  0

; The 38h answer, copied out whole.  The case-map far call is patched
; over on the way out - see fn38 - because the segment it points into
; is not known until the guest block has been allocated.
ctry_block:
            dw  0                       ; mm/dd/yy
            db  '$',0,0,0,0             ; currency symbol
            db  ',',0                   ; thousands
            db  '.',0                   ; decimal
            db  '-',0                   ; date separator
            db  ':',0                   ; time separator
            db  0                       ; symbol leads, no space
            db  2                       ; cents
            db  0                       ; 12-hour
            dd  0                       ; case map - patched per call
            db  ',',0                   ; data list separator
            db  10 dup(0)               ; reserved

; The register structure INT 28h is issued through.  Padded to a
; multiple of four because it is zeroed with STOSD and RMCS_LEN is 32h,
; which is not one - the last two bytes would otherwise be somebody
; else's.
idle_rmcs   db  ((RMCS_LEN + 3) / 4) * 4 dup(0)
clusbuf_clus dd -1
std_ch      db  0                       ; one-byte bounce for std_in/out

; EXEC state
exec_psp    dd  0                       ; the child's PSP, flat
exec_stack  dd  0                       ; the stack its header asked for
exec_entry  dd  0                       ; its entry point, as a slot offset
exec_cursor dd  0
exec_left   dd  0
exec_clus   dd  0
exec_bssend dd  0
exec_bsssz  dd  0                       ; the header's BSS field, for the save
exec_depth  dd  0                       ; how many programs deep we are
exec_err    dd  0                       ; error code across a stack switch

; The path and the command tail are COPIED here before a program is
; loaded.  Both arrive as pointers into the CALLER's memory, and for a
; nested EXEC the caller's memory is the very slot about to be
; overwritten - so reading them later would read the new program.
exec_path   db  EXEC_PATH_MAX dup(0)
exec_tail   db  132 dup(0)
exec_envlen dd  0                       ; and the environment, which has the
exec_env    db  SLOT_ENVMAX dup(0)      ; same problem twice over - see
                                        ; exec_cpenv
exec_sig    dd  0,0                     ; the eight bytes that say whose
                                        ; kind of executable this is: the
                                        ; 16-bit jump, and the magic behind it

; One record per nesting level, describing the program that is WAITING
; - not the one running.
exec_stk    db  EXEC_MAX * XN_LEN dup(0)

f73_rec     db  F73_SIZE dup(0)         ; 7303h's answer, built whole
f8_rec      db  F8_LEN dup(0)           ; and F1h/08's, likewise

sft_tbl     db  MAX_HANDLES * SFT_LEN dup(0)
