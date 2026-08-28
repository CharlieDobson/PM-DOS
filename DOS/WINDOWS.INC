;=====================================================================
; WINDOWS.INC - the surface Microsoft Windows expects to find, and the
;               swappable data area that a great deal of other software
;               expects to find for the same reasons.
;
; TWO THINGS LIVE HERE AND THEY ARE THE SAME THING SEEN FROM TWO SIDES.
;
; The first is INT 21h AX=5D06h, the swappable data area: the block of
; DOS's own per-process state that a multitasker has to save and
; restore around a task switch, because DOS keeps one copy of it and
; there is more than one process.  Windows in 386 enhanced mode is the
; famous consumer - its DOSMGR virtual device asks for this the moment
; it starts, and instances it per virtual machine - but it is by no
; means the only one: DESQview asks, network redirectors ask, and any
; TSR that wants DOS's current drive or DTA without disturbing them
; asks.  PM-DOS has had the two flags at the head of this structure
; since it had an INT 24h, and has been unable to publish them as a
; structure because of where they were.  Now it can.
;
; The second is the INT 2Fh multiplex family in the 16xx range, which
; is how Windows and the machine underneath it introduce themselves.
; PM-DOS answers the questions ("is Windows running?" - no) and, for
; the one call that is not a question, REFUSES: AX=1605h is Windows/386
; announcing that it is about to take the machine, and PM-DOS cannot
; let it, because WIN386.EXE is itself a V86 monitor and this kernel is
; already one.
;
; WHY REFUSING IS WORTH CODE.  Before this file, 1605h reached the IRET
; stub the VDM installs on INT 2Fh for a guest, came back with every
; register exactly as WIN386 had set it, and WIN386 read CX - which it
; had not initialised for the purpose of being read - as the verdict on
; whether it could start.  Whatever happened next was not a decision
; anybody had made.  Now it is: CX comes back non-zero, WIN386 stops,
; and the user is told why and what to try instead.  That is the entire
; behavioural change in this file, and it turns an undefined outcome
; into a defined one.
;=====================================================================

;=====================================================================
; win_sdainit - lay out the swappable data area.  Called from DOSMAIN
;               at boot, AFTER vdm_lolinit, which zeroes the whole DOS
;               data segment including this.
;
; The order matters and the assembler cannot check it: vdm_lolinit's
; rep stosd covers DOSDATA_SIZE bytes, and the InDOS flag is now one of
; them.  It is safe only because both run from the init path at ring 0
; with no DOS call in flight - the count it zeroes is already zero.
;=====================================================================
win_sdainit:
        pushad
        mov     edi,SDA_BASE
        xor     eax,eax
        mov     ecx,SDA_LEN
        rep     stosb

        ; MS-DOS initialises exactly one field here to something other
        ; than zero - see CONST2.ASM, where WPErr is declared <-1> -
        ; because drive zero is a real drive and "no write-protect
        ; error yet" therefore cannot be spelled with it.
        mov     byte ptr ds:[SDA_BASE + SDA_WPERR],0FFh

        call    win_sdasync
        popad
        ret

;=====================================================================
; win_sdasync - refresh the fields of the SDA that the kernel keeps its
;               master copy of somewhere else.
;
; Called on the way out of every INT 21h, from i21_r0.
;
; WHY THERE, AND WHY NOT IN THE SETTERS.  The current DTA, PSP and
; drive are changed by 1Ah, 50h and 0Eh, and also by EXEC, by
; termination, by a critical error unwinding a box and by the VDM
; entering and leaving one - seven or eight sites, of which forgetting
; one would produce a structure that is right nearly always.  A
; consumer reads this when InDOS is zero, which is to say just after a
; DOS call returned, so refreshing it as the call leaves gives the
; right answer at the only moment anyone is entitled to look.
;
; The cost is a dozen instructions on the exit path of a call that has
; just done real work, and it buys one site that cannot be missed.
;=====================================================================
win_sdasync:
        pushad

        ; THE CURRENT PROCESS, AS A REAL-MODE PSP SEGMENT, WHICH A
        ; NATIVE PROGRAM DOES NOT HAVE.  A DOS box's PSP is in
        ; conventional memory and converts; a 32-bit program's is at
        ; 140000h or 180000h and there is no segment:offset that names
        ; it.  So it reads 0000 rather than the bottom sixteen bits of
        ; an address above the megabyte, which is a number that means
        ; something else and would be followed.
        xor     eax,eax
        cmp     dword ptr ds:[vdm_depth],0
        je      wss_psp
        movzx   eax,word ptr ds:[vdm_curpsp]
wss_psp:
        mov     word ptr ds:[SDA_BASE + SDA_PSP],ax

        ; The disk transfer address, under the same rule and stored the
        ; way DOS stores it: offset in the low word, segment in the
        ; high one.
        mov     eax,dword ptr ds:[dta_ptr]
        cmp     eax,00100000h
        jb      wss_dta
        xor     eax,eax
wss_dta:
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
        shl     edx,16
        or      eax,edx
        mov     dword ptr ds:[SDA_BASE + SDA_DTA],eax

        mov     al,byte ptr ds:[cur_drive]
        mov     byte ptr ds:[SDA_BASE + SDA_DRIVE],al

        ; The extended error, and its class, action and locus - which
        ; PM-DOS DERIVES from the code rather than storing, so getting
        ; them is a table scan and not a load.  Only when the code has
        ; actually changed: otherwise this would be a scan on the way
        ; out of every DOS call, nearly always arriving at the answer it
        ; arrived at last time.
        movzx   eax,word ptr ds:[last_err]
        cmp     ax,word ptr ds:[SDA_BASE + SDA_EXTERR]
        je      wss_done
        mov     word ptr ds:[SDA_BASE + SDA_EXTERR],ax
        call    err_classify
        mov     byte ptr ds:[SDA_BASE + SDA_CLASS],bh
        mov     byte ptr ds:[SDA_BASE + SDA_ACTION],bl
        mov     byte ptr ds:[SDA_BASE + SDA_LOCUS],ch
wss_done:
        popad
        ret

;=====================================================================
; 5Dh - the server-call family.  PM-DOS answers one of it.
;
;   AL=06h  get the swappable data area -> DS:SI, CX = its length,
;           DX = how much of it must always be swapped
;
; AND THE ANSWER IS SHORT ON PURPOSE.  MS-DOS 5 publishes about 1440
; bytes here and PM-DOS publishes 26.  The 26 are the ones MS-DOS
; itself calls SWAP_START through Swap_Always - the error flags, the
; DTA, the PSP, the drive - which is per-process state and genuinely
; needs instancing.  Everything past that point in MS-DOS is its own
; working storage: device request packets half built, directory-search
; state, the arena walk in progress.  PM-DOS's equivalents are 32-bit
; structures above 1MB, so there is nothing there to publish, and
; publishing 1414 zeroes at the offsets where a consumer expects
; MS-DOS's would be an invitation to read them.
;
; CX AND DX ARE THE SAME NUMBER, which is what "all of it must always
; be swapped" looks like from the caller's side.  MS-DOS computes DX as
; the distance from SWAP_START to Swap_Always; ours has no second half
; for the distance to stop short of.
;
; The rest of the family is NOT here and each absence is deliberate.
; 5D00h-5D05h and 5D07h-5D09h are the network server calls - executing
; a DOS call on behalf of another machine, closing files by user ID,
; printer spooling - and PM-DOS has no redirector for them to be part
; of.  5D0Ah, set extended error information, is the one that could go
; in and has not: it presets the class, action and locus that the NEXT
; 59h will report, and PM-DOS does not store those three - it derives
; them from the error code through err_xlat.  Honouring 5D0Ah therefore
; means giving 59h an override that outranks its own table, which is a
; change to the error model rather than an addition to this file, and
; it can wait for something that actually calls it.
;=====================================================================
fn5D:
        mov     al,byte ptr [ebp+F_EAX]
        cmp     al,06h
        je      f5D_sda
        jmp     i21_unsup

f5D_sda:
        call    win_sdasync             ; it is about to be read by someone
        mov     dword ptr [ebp+F_ESI],SDA_BASE
        mov     dword ptr [ebp+F_ECX],SDA_LEN
        mov     dword ptr [ebp+F_EDX],SDA_LEN
        jmp     i21_ok

;=====================================================================
; win_int2F - the Windows half of the INT 2Fh multiplex, out of a DOS
;             box.  CF clear = answered, CF set = not ours.
;
; Called from vdm_i2F after XMS and DPMI have had their look.
;
; THREE OF THESE FOUR ANSWERS WERE ALREADY RIGHT AND THAT IS NOT A
; REASON TO LEAVE THEM ALONE.  1600h, 160Ah, 4680h and 1680h are all
; specified so that the "no" answer is the registers coming back
; unchanged - AL still 00h, AX still non-zero, AL still 80h - and the
; IRET stub the VDM puts on INT 2Fh for a guest changes nothing, so
; every one of them has been answering correctly by accident since the
; VDM existed.  What claiming them buys is that the answer stops
; depending on the stub still being an IRET, and that a guest TSR which
; hooked 2Fh cannot answer them on PM-DOS's behalf and get it wrong.
;=====================================================================
win_int2F:
        mov     ax,word ptr [vdm_ctx+V86C_EAX]
        cmp     ax,1605h
        je      w2f_start
        cmp     ax,1600h
        je      w2f_none
        cmp     ax,160Ah
        je      w2f_held
        cmp     ax,4680h
        je      w2f_held
        cmp     ax,1680h
        je      w2f_held

        ; 1606h, 1607h, 1608h and 1609h - Windows exiting, calling out
        ; to a named virtual device, finishing its initialisation,
        ; beginning to shut down.  All four are broadcasts addressed to
        ; residents that took part in the startup, and PM-DOS refused to
        ; take part, so they are not ours to swallow: they reflect to
        ; the guest's own chain like any other multiplex call.
        stc
        ret

;--- 1600h: is 386 enhanced mode running?  AL=00h is "no".  It is worth
;--- saying rather than leaving to the stub, because "no" here is what
;--- sends a program that can run either way down its DOS path.
w2f_none:
        mov     byte ptr [vdm_ctx+V86C_EAX],0
        clc
        ret

;--- 160Ah (get version and instance data), 4680h (is Windows 3.0 real
;--- or standard mode running?) and 1680h (release the current time
;--- slice).  All three mean "no" or "nobody is listening" by coming
;--- back exactly as they went in, so the handler is a deliberate
;--- nothing.  Writing the registers would be writing what is there.
w2f_held:
        clc
        ret

;=====================================================================
; 1605h - Windows/386 is starting, and it is asking permission.
;
; This is the one call in the family with teeth.  WIN386.EXE broadcasts
; it before it takes the machine; every resident on the chain gets a
; look, may add instance-data records through the structure at ES:BX,
; and may VETO the whole startup by returning CX non-zero.  PM-DOS
; vetoes.
;
; WHY IT HAS TO.  WIN386.EXE is a V86 monitor: it builds its own page
; tables, its own GDT and IDT, puts DOS in a virtual machine and runs
; the machine from ring 0 itself.  There is exactly one of those seats
; and PM-DOS is in it.  Under MS-DOS the seat is empty, and where a
; monitor is already there - EMM386 - Windows starts only because
; EMM386 answers this call by getting out of protected mode entirely
; and handing the whole machine over, with a real-mode MS-DOS left
; underneath still able to serve INT 21h.  PM-DOS has no real-mode DOS
; to leave underneath: its entire API is 32-bit protected-mode code.
; That is the thing that has to be built before this call can be
; answered any other way, and it is a second DOS, not a shim.
;
; THE VETO IS SET AND THEN THE CALL IS REFLECTED ANYWAY.  Returning CF
; set sends it on to the guest's own INT 2Fh chain, which is correct
; and costs nothing: a broadcast is supposed to reach every resident,
; and a TSR in the box that wanted to see Windows start is entitled to
; see it even though the answer is already no.  Nothing downstream can
; undo the veto - the protocol has no way to clear CX back to zero -
; so setting it first and chaining second is both polite and safe.
;=====================================================================
w2f_start:
        mov     word ptr [vdm_ctx+V86C_ECX],1   ; non-zero: do not start

        cmp     byte ptr ds:[win_said],0
        jne     w2f_saidit
        mov     byte ptr ds:[win_said],1
        push    esi
        mov     esi,OFFSET msgWinEnh
        call    dword ptr ds:[IOS_CONSTR]
        pop     esi
w2f_saidit:
        stc                                     ; and on down the chain
        ret

win_said    db  0                   ; the message is per boot, not per try:
                                    ; WIN.COM will broadcast again if the
                                    ; user runs it again, and saying it
                                    ; twice adds nothing

msgWinEnh   db  13,10
            db  'PM-DOS: 386 enhanced mode cannot start on this kernel.',13,10
            db  'WIN386.EXE is itself a protected-mode monitor, and PM-DOS is'
            db  ' already',13,10
            db  'the one running this machine - there is no second seat to '
            db  'give it.',13,10
            db  'Standard mode - WIN /S - runs as a DPMI client instead, and '
            db  'PM-DOS',13,10
            db  'hosts DPMI.  That is the one to try.',13,10,0
