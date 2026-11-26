;=====================================================================
; RMXMS.INC - the extended memory manager RMDOS provides
;
; This is HIMEM.SYS's job done for real, in real mode, by sixteen-bit
; code - which is the opposite of what DOS\XMS.INC does for the
; kernel's own guests and is worth saying plainly, because the two
; files answer the same interface and share not one line.
;
; XMS.INC serves a V86 guest FROM protected mode: the machine is
; already in it, extended memory already belongs to the arena, and a
; block move is a REP MOVSD between two flat addresses.  Here there is
; no protected mode, nothing owns extended memory, and everything
; HIMEM ever had to do the hard way has to be done the hard way: the
; A20 gate through the hardware that owns it, and a block move by
; leaving real mode and coming back.
;
; WHY IT HAS TO EXIST.  Windows in 386 enhanced mode does not ask
; whether there is extended memory - it requires an XMS driver, gets
; all of its memory through one, and stops with "You must have an XMS
; driver" if INT 2Fh AX=4300h does not answer 80h.  v0.45 answered 0,
; which was true and which ended the conversation.
;
;---------------------------------------------------------------------
; WHERE THE MEMORY COMES FROM, WHICH IS THE PART THAT IS NOT LIKE
; HIMEM AT ALL.
;
; A real HIMEM asks the BIOS how much extended memory the machine has
; and takes all of it, because on a real DOS nothing else has any
; claim.  Here almost all of it is spoken for: PMDOS.SYS is at
; 100000h, the arena above it has live blocks in it, and the excursion
; has to come back to every one of them.  Extended memory as the BIOS
; describes it is not RMDOS's to give away.
;
; So it is GIVEN a range.  Before parking, PM-DOS allocates the free
; part of its own arena to itself and passes the block over in
; RMH_XMSBASE/RMH_XMSLEN; park_release gives it back.  Everything
; handed out here comes out of memory the kernel has already agreed to
; lose for the duration, and no XMS handle in the machine can name a
; byte the kernel is standing on.
;
; THE ADDRESSES ARE PHYSICAL BECAUSE PM-DOS'S MAP IS THE IDENTITY.
; Function 0Ch has to answer "where is this block, really", and that
; question has a truthful answer here only because linear and physical
; are the same number in this kernel.  It is the same property that
; let v0.43 hand a bus-master controller the caller's own pointer.
;
;---------------------------------------------------------------------
; WHAT IT OFFERS AND WHAT IT DOES NOT, AND WHY EACH IS A DECISION.
;
;   THE HMA, AND ONLY WHEN THIS IMAGE IS NOT IN IT.  That used to be
;   never, and the flat refusal that went with it outlived its reason.
;   RMDOS parks LOW by default since v0.53 - the Windows kernel scans
;   for the SFT only as far as 80000h and cannot find a DOS above it -
;   and 100000h to 10FFEFh is then empty and belongs to the guest.
;   PMDOS.INC leaves it empty deliberately, loading the kernel at
;   110000h instead, so offering it is not scavenging: it is handing
;   over a range reserved for exactly this.  Parked HIGH the old
;   answer stands and 01h still says "no HMA", because then the
;   driver really is standing in the only 64K it could offer.
;
;   UMBs.  10h answers "no UMBs available", which is what HIMEM alone
;   answers on every machine - upper memory came from EMM386, not from
;   HIMEM.  PM-DOS's own upper memory is already linked into the MCB
;   chain RMDOS adopted, so it is reachable through 48h, and a second
;   door onto the same pool is a bug waiting for whoever opens both.
;
; Included by RMDOS.ASM.  Real mode throughout.
;=====================================================================

RX_MAXH         EQU     32              ; handles, HIMEM's own default
RX_HMAMIN       EQU     0                       ; HIMEM s /HMAMIN, and zero is
                                                ; its default: a TSR asking for
                                                ; any amount at all is served
; A PAGE - AND THE KILOBYTE WAS TRIED AND MEASURED, so it need not be
; tried again.  Rounding the size up to a page leaves a hole: on a
; 128MB machine Windows 3.1's extender asks twice, the first answer
; saturates the 16-bit KB field at 65535K, and a 65535K block rounded
; up to a page ends 1024 bytes before the next one begins.  HIMEM
; counts in kilobytes throughout and would have handed out two blocks
; that touch, so the extender's arena would be one region where ours
; is two - which looked like exactly the kind of difference that makes
; a block MOVE instead of growing in place.
;
; It is not what standard mode dies of.  With RX_GRAN at 400h the two
; blocks do touch, every address downstream shifts by the missing
; kilobyte - the extender's first allocation lands at 04298C30 rather
; than 04299030 - AND THE FAULT IS THE SAME FAULT, same selector, same
; instruction, same untouched memory behind it.  So the page stays,
; for the reason it was chosen: the customer is a pager.
RX_GRAN         EQU     1000h           ; every block starts on a page
RX_PSMAX        EQU     40              ; dirty pages rx_poolscan names - not
                                        ; RX_PSLIST: wasm folds case, and that
                                        ; name is the table rx_pslist

RXH_FLAGS       EQU     0               ; dw - 0 free, 1 allocated
RXH_KB          EQU     2               ; dd - size as the owner asked for it
RXH_ADDR        EQU     6               ; dd - flat = physical
RXH_LOCK        EQU     10              ; dw - lock count
RXH_LEN         EQU     12

; The error codes, from the XMS 3.0 specification.  RXE_ rather than
; XSE_ because the kernel's XMS.INC uses that prefix for the same
; numbers, wasm folds case, and one of these files reaching the wrong
; assembly is a mistake worth making impossible to miss.
RXE_NOTIMPL     EQU     80h
RXE_VDISK       EQU     81h
RXE_A20         EQU     82h
RXE_NOHMA       EQU     90h
RXE_HMAINUSE    EQU     91h
RXE_HMAMIN      EQU     92h
RXE_HMANOTUSED  EQU     93h
RXE_A20STILL    EQU     94h
RXE_NOMEM       EQU     0A0h
RXE_NOHANDLE    EQU     0A1h
RXE_BADHANDLE   EQU     0A2h
RXE_BADSRCH     EQU     0A3h
RXE_BADSRCO     EQU     0A4h
RXE_BADDSTH     EQU     0A5h
RXE_BADDSTO     EQU     0A6h
RXE_BADLEN      EQU     0A7h
RXE_OVERLAP     EQU     0A8h
RXE_PARITY      EQU     0A9h
RXE_NOTLOCKED   EQU     0AAh
RXE_LOCKED      EQU     0ABh
RXE_LOCKCOUNT   EQU     0ACh
RXE_LOCKFAIL    EQU     0ADh
RXE_NOUMB       EQU     0B1h            ; "no UMBs are available" - B0h is
                                        ; "a smaller one is", with DX its
                                        ; size, and B0h with DX=0 was what
                                        ; Windows got here until v0.63
RXE_UMBTOOBIG   EQU     0B1h
RXE_BADUMB      EQU     0B2h

; The move descriptor, which the caller passes in DS:SI
XM_LEN          EQU     0               ; dd - bytes, and it must be even
XM_SRCH         EQU     4               ; dw - handle, 0 = the offset is seg:off
XM_SRCO         EQU     6               ; dd
XM_DSTH         EQU     10
XM_DSTO         EQU     12
XM_SIZE         EQU     16

;=====================================================================
; rx_init - take the range PM-DOS gave us and find the A20 gate.
;
; Called from rd_entry before anything else can ask for memory.
;=====================================================================
rx_init:
        pusha
        push    es

        ; The pool, as handed over.  Both ends are already aligned -
        ; PM-DOS rounded the base up to a page and the length down to a
        ; kilobyte - so there is nothing to do here but believe it.
        mov     eax,dword ptr [h_xmsbase]
        mov     [rx_base],eax
        add     eax,dword ptr [h_xmslen]

        ; NOT CAPPED, AND THE CAP WAS NEVER THE ANSWER - THIS COMMENT
        ; HAS NOW BEEN WRONG TWICE AND BOTH ERRORS HAD ONE CAUSE.
        ;
        ; Windows 3.1 standard mode dies on this kernel: DOSX carves
        ; KRNL386's memory out of what this host offers and then fails
        ; with "Standard Mode: Fault in MS-DOS Extender".  Uncapped on a
        ; 128MB machine the extender's second block lands at 04298000h -
        ; 69MB - which a 24-bit descriptor base cannot reach, so capping
        ; the pool looked obvious.  Capping made WIN.COM refuse EARLIER,
        ; at 16, 32 and 64MB alike, and that was written up here as "the
        ; small pool is what it dislikes".
        ;
        ; BOTH READINGS WERE ARTEFACTS OF A BUG SOMEWHERE ELSE.  WIN.COM
        ; allocates XMS from PM-DOS own host BEFORE it parks; the park
        ; then swapped that host for this one and left the arena still
        ; allocated, so the pool handed over was EMPTY.  "Refused" never
        ; meant "the cap is bad", it meant "there is nothing here".
        ; park_xmsreclaim in PARK.INC fixes that, and every one of the
        ; three lines of measurement that used to sit here re-reads
        ; differently now.  With it in place:
        ;
        ;   128MB, no cap   pool 128240K at 00283000 - DOSX takes 65535K
        ;                   (the 16-bit KB field saturates), locks it at
        ;                   00298000, then takes the remaining 62620K
        ;                   and locks THAT at 04298000 - 69MB
        ;   128MB, cap 16MB pool  13812K at 00283000 - DOSX takes one
        ;                   13728K block at 00298000, ending at exactly
        ;                   01000000h.  Contiguous, locked, entirely
        ;                   inside 24 bits: an ordinary standard-mode
        ;                   machine by any measure
        ;   16MB machine    pool  13600K, same shape
        ;
        ; AND ALL THREE FAULT IDENTICALLY.  Same CS:IP, same selector,
        ; same SI/DI/BP/SP; only the requested limit and one selector
        ; move, and both of those track the pool size, so they are
        ; downstream of the fault rather than upstream of it.  Where the
        ; extender memory sits is therefore NOT the cause, and the cap
        ; is an instrument, not a fix.
        ;
        ; WHAT THE FAULT ACTUALLY IS, read out of the extender itself.
        ; DOSX protected-mode code segment begins at file offset 3AC2h
        ; in DOSX.EXE - see TOOLS\UNDIS.PL, which is how it was found and
        ; how to disassemble any of it again.  Its INT 31h dispatch table
        ; (file 6542h, [function][handler] pairs) puts Set Segment Limit
        ; at code 2C3B and ONE shared return path at 2B92..2B9A:
        ;
        ;   2B92  and byte ptr [bp+14h],0FEh   ; success: clear caller CF
        ;   2B96  cli
        ;   2B97  call 18F9h                   ; unwind to the client
        ;   2B9A  IRET                         ; <== the faulting insn
        ;
        ; IP=2B9A is that IRET, and every DPMI call in the system returns
        ; through it.  So the call SUCCEEDS and the extender dies on the
        ; way back.
        ;
        ; Patching DOSX in the disk image - never the master - and using
        ; its own fault printer as the readout, four runs settle it.  The
        ; selector being resized is 027F, and it is the selector the
        ; client is running on as SS:
        ;
        ;   descriptor 027F before: base 04299030, limit 0000FFFF,
        ;   access F3 (present, DPL3, data, EXPAND-UP, writable), byte 6
        ;   = 00 (byte granular, 16-bit).  The base is KRNL386 own block,
        ;   1030h into the XMS block DOSX locked - and base15:0 = 9030 is
        ;   the DI=9030 that shows up in every fault dump and was once
        ;   mistaken for a DPMI handle.
        ;
        ;   write nothing at all  -> the IRET survives
        ;   write the SAME value  -> the IRET survives
        ;   write a LARGER limit  -> IRET faults at 2B9A
        ;   write a SMALLER limit -> IRET faults at 2B9A
        ;
        ; So it is not the act of writing, not the 64K boundary, and not
        ; grow-versus-shrink: it is CHANGING the limit of the selector the
        ; client is standing on.  DOSX unwind reloads SS from that
        ; descriptor, so the IRET pops against the new limit.
        ;
        ; AND WHAT THE KERNEL IS DOING, read out of KRNL386.EXE.  It is
        ; an NE file; segment 1 is CODE at file offset 1620h, so a
        ; segment offset is file 1620h + offset.  BEF9 is one of the 74
        ; INT 31h sites in that segment, which confirms the call site
        ; independently of the breadcrumbing that first found it:
        ;
        ;   BED3  mov ax,0501h / int 31h    allocate a block of EAX bytes
        ;   BEF4  call 22D3h                give the selector that base
        ;   BEFD  dec eax                   ...so the limit is SIZE-1
        ;   BF08  test cx,0FFF0h            >= 1MB?  then page-granular
        ;   BF14  mov bx,si                 (this is why SI = BX = 027F)
        ;   BF16  mov ax,0008h / int 31h
        ;
        ; EAX is a memory-derived size plus 8080h - a heap clamped to a
        ; 32K floor.  Roughly free-KB times eight: with 65535K reported
        ; it comes to 512K, and 512K + 8080h = 88080h, whose limit is
        ; the 0008807F seen in the dump.  NOTE THAT A REAL HIMEM ON A
        ; 128MB MACHINE SATURATES THAT SAME 16-BIT FIELD, so it would
        ; compute the same number.  The size alone does not distinguish
        ; this host from a real one, and an earlier note here that said
        ; otherwise was guessing.
        ;
        ; The important find is 22D3h -> 2291h, because it explains why
        ; no DPMI 0007 Set Segment Base is ever issued for 027F - probed
        ; for, at every RPL, and it simply is not there:
        ;
        ;   2291  mov bx,[bp+4]        the selector
        ;         lsl ecx,ebx          ITS CURRENT LIMIT
        ;         shr ecx,16 / inc ecx
        ;         mov di,cx            DI = (limit>>16)+1 - 64K blocks
        ;         mov ds,cs:[0032]     the kernel OWN LDT alias
        ;         and bl,0F8h
        ;   22B5  mov [bx+2],dx        base15:0     ) written STRAIGHT
        ;         mov [bx+4],cl        base23:16    ) into the LDT,
        ;         mov [bx+7],ch        base31:24    ) no DPMI involved
        ;         lea bx,[bx+8] / inc cx / dec di / jnz
        ;
        ; So the kernel edits descriptors itself, and the LIMIT it asks
        ; DPMI to set is really the LENGTH OF A SELECTOR ARRAY: raising
        ; 027F to 88080h bytes makes nine consecutive LDT entries - 027F,
        ; 0287 ... 02BF - part of that one segment, and the next pass
        ; through 2291 will write all nine.
        ;
        ; THE LIMIT WAS NEVER THE CAUSE.  That reading is withdrawn.  It
        ; came from patching the 0008 handler and watching whether the
        ; IRET survived, and it does not survive the obvious control:
        ; DOSX's own fault printer dumps EVERY register, not the three
        ; the earlier runs quoted, and it says
        ;
        ;   Fault in MS-DOS Extender.
        ;   EC=0000 CS=0053 IP=2B9A AX=0008 BX=027F CX=0008 DX=807F
        ;   SI=027F DI=9030 BP=01DC DS=0317 ES=0317 SS=027F SP=01A6
        ;
        ; SS=027F.  The selector whose limit KRNL386 is asking DOSX to
        ; set is the CLIENT'S OWN STACK, and the IRET at 2B9A pops its
        ; frame from it.  Probing DOSX's shared return path - patch the
        ; 0008 handler, call the unwind at 18F9, then read the pending
        ; frame off the restored client stack - the word the IRET is
        ; about to take for IP reads 0000.  On the earlier, healthy 0008
        ; call for selector 0357 the same probe reads BC43.  IP=0000 with
        ; CS=0000 is a return through a NULL selector, which is #GP(0):
        ; EC=0000 exactly as printed.  Nothing about the limit is wrong.
        ;
        ; AND IT IS ALREADY WRONG BEFORE DOSX WRITES ANYTHING.  Reading
        ; the client's saved SP through a FRESHLY LOADED 027F at the top
        ; of the handler, before the limit is touched:
        ;
        ;   EC=FFFC CS=0053 IP=2C4F AX=027F BX=01A6 CX=FFFC DX=0000
        ;
        ; BX is the client's saved SP, DS was 027F, and the stack reads
        ; zero.  The descriptor itself already carries base ..29:9030,
        ; access F3, before the call - probed the same way.  So 027F
        ; points at memory that is blank, while the client goes on
        ; running from the descriptor the CPU cached when SS was last
        ; loaded.  DOSX's unwind reloads SS (mov ss,[24C0] at the tail of
        ; the exit helper) and the client's stack vanishes underneath it.
        ; KRNL386 repointed 027F itself, through its own LDT alias at
        ; cs:[0032], immediately before the 0008 call - see the routine
        ; at seg1:2291 - so this is stock Windows doing a stock thing.
        ;
        ; IT IS NOT A PM-DOS QUESTION.  The paragraph that used to stand
        ; here said the block behind 04299030 was blank and offered two
        ; candidates - the kernel's copy never landed there, or the
        ; address this driver reports for a locked block is not the
        ; memory it handed over.  Both have now been measured, and the
        ; second is wrong.
        ;
        ; THE POOL IS HONEST.  rx_pooltest, below, stamps every dword of
        ; the pool with its own linear address and reads them all back,
        ; in real mode after the park, with the page tables gone and A20
        ; forced open - the machine Windows will actually see.  On a
        ; 128MB machine that is 32 million stamps across 128240K and
        ; every one of them answers correctly.  There is no aliasing,
        ; nothing missing, and a linear address in what we hand out IS
        ; the physical memory we hand out.
        ;
        ; AND THE STAMPS ARE THE INSTRUMENT, not just the test.  Left in
        ; place they turn any read of extended memory into an oracle,
        ; because a dword either says its own address or says somebody
        ; wrote it.  Two things fall straight out:
        ;
        ;   - the fault changes from EC=0000 to EC=8DD8, and 8DD8 is the
        ;     low word of the stamp at the exact address the IRET pops
        ;     CS from.  The client's stack frame is not there.  Nothing
        ;     ever wrote it.  Zeros could have been anything; a stamp
        ;     cannot.
        ;
        ;   - rx_poolscan, at rd_goback under RMDOS_FREEZE, then names
        ;     every page that lost its stamp.  Over an entire standard-
        ;     mode start that is TWENTY-SEVEN PAGES out of 32060:
        ;
        ;        00283000..00298000   the extender's own 84K block, all
        ;                             of it, and this is the only thing
        ;                             in the machine that looks used
        ;        04297000             the last page of the 65535K block
        ;        04298000 04299000    the first pages of the 62620K one
        ;        04321000             the last page of the 544K block
        ;                             the 0008 call is sizing
        ;        07FBE000             the last page of the pool
        ;
        ; First page and last page of each region: that is a SIZING
        ; PROBE, not a copy and not a kernel being loaded.  128MB of
        ; extended memory has been allocated, locked, measured at both
        ; ends and then not used for anything.
        ;
        ; SO WHAT DIES IS THIS.  KRNL386 is running from conventional
        ; memory - selector 0317, base 00010320, which is exactly where
        ; RMDOS's EXEC put segment 1 of KRNL386.EXE (file 1620h into an
        ; image loaded at 0EF0:0000, and its MZ header asks for 73K,
        ; which is what it was given).  It takes a 544K block out of the
        ; extender at 04299030, points a second selector 0357 at the
        ; same base - and 0357's access byte still reads F2, ACCESSED
        ; CLEAR, so that alias has never once been loaded into a segment
        ; register.  It then writes 027F's base to the same address with
        ; its own hands through the LDT alias at cs:[0032], and asks
        ; DPMI 0008 for the matching limit.  027F is its stack.  Nothing
        ; was copied, through 0357 or anything else, and the descriptor
        ; is otherwise perfect: base 04299030 with all thirty-two bits
        ; present (byte 7 reads 04 - the 24-bit truncation this file
        ; once suspected does not happen), access F3, limit 0.
        ;
        ; 027F is also not part of a selector array: the descriptor
        ; after it, 0287, has base 0 and accessed clear, and the one
        ; before, 0277, has an unrelated conventional base and is in
        ; use.  It is a single live selector whose base moved out from
        ; under a stack that was never brought with it.
        ;
        ; AND IT WAS A PM-DOS QUESTION AFTER ALL - the paragraph that
        ; used to stand here said it was not.  v0.54 has Windows 3.1
        ; standard mode reaching Program Manager with a stock DOSX and
        ; nothing patched, and neither fix is in this file.
        ;
        ; The copy does not happen because THERE IS NO COPY.  KRNL386
        ; is not moving a stack; it is creating its global heap, and
        ; the selector it repoints is meant to be a scratch one whose
        ; old contents nobody wants (seg1:BDF3, which allocates from
        ; DPMI 0501 and then calls SetSelectorBase on the selector it
        ; was given).  That selector came back from DPMI 0002, Segment
        ; To Descriptor, and DOSX serves 0002 by SEARCHING THE LDT for
        ; a descriptor that already has the right base, a limit of
        ; FFFF and access F2 - and handing that one back rather than
        ; making a new one (the search is at DOSX code 20E7, reached
        ; from nothing but the 0002 handler).  The client stack fits
        ; that description exactly.  So Windows asked for a descriptor
        ; onto its own free memory, was given its own stack, and threw
        ; the stack away.
        ;
        ; What put the stack there was RMDOS.  KRNL386.EXE has MAXALLOC
        ; zero, which in real DOS means LOAD HIGH: the largest block
        ; there is, the PSP at the bottom of it and the image at the
        ; top, with the free run in between.  RMDOS loaded it low and
        ; gave it nothing beyond the image, so the paragraph Windows
        ; asked 0002 about landed on top of a descriptor it was
        ; already using.  fn4B honours MAXALLOC zero now.  The other
        ; fix was the environment: see ep_envcopy.
        ;
        ; The instruments are XMSPOOLSTAMP (rx_pooltest, rx_poolscan)
        ; and RMDOS_FREEZE, which stops the machine at rd_goback instead
        ; of unparking so the screen keeps whatever the excursion left
        ; on it - without it the fault report survives about half a
        ; second and a once-a-second poll caught it once in twelve boot
        ; loops.  BUILD.CMD stamp / freeze / stampfrz / lptfz / lptfrz
        ; / lptq, which is the trace that never writes during the run.
        ; TOOLS\UNDIS.PL is how DOSX and KRNL386 were read and
        ; TOOLS\DOSXPOKE.PL is how the probes went into HD16.IMG, never
        ; into the Windows master.  Nothing here is evidence about pool
        ; SIZE; the cap experiments were measuring the wrong thing.
        ; There is still no real-DOS control on this machine.
        ; See NOTES.TXT, v0.53 and v0.54.
IFDEF XMSPOOLCAP
        ; Kept, off by default, because it is how the middle line above
        ; was measured and it will be needed again the moment the small
        ; pool works.
        cmp     eax,1000000h
        jbe     rxi_nocap
        mov     eax,1000000h
rxi_nocap:
ENDIF
        mov     [rx_end],eax
IFDEF DBG_LPT
        call    rdbg_xpool              ; what this host was actually given
ENDIF

        ; every handle free
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rx_handles
        mov     cx,RX_MAXH * RXH_LEN
        xor     al,al
        cld
        rep     stosb

        ; The GDT the block move uses, at its linear address.  CS is
        ; FFFFh when RMDOS is high, and this arithmetic is right for
        ; that too - FFFF0h plus an offset of at least RM_ORG is at or
        ; above the megabyte, which is where the table actually is.
        ; Either way it fits in the twenty-four bits an unprefixed
        ; LGDT would load even if the prefix were forgotten.
        xor     eax,eax
        mov     ax,cs
        shl     eax,4
        add     eax,OFFSET rx_gdt
        mov     dword ptr [rx_gdtr + 2],eax

        ; INT 15h AH=88h has to stop telling the truth.  It is the
        ; other way a program can find extended memory, and a program
        ; that finds it that way will use memory this driver has
        ; already promised to somebody - or memory the kernel is in.
        ; Every HIMEM since the first one has hooked this vector for
        ; exactly that reason.
        xor     ax,ax
        mov     es,ax
        mov     ax,word ptr es:[15h*4]
        mov     word ptr [rx_oldi15],ax
        mov     ax,word ptr es:[15h*4 + 2]
        mov     word ptr [rx_oldi15 + 2],ax
        cli
        mov     word ptr es:[15h*4],OFFSET i15
        mov     word ptr es:[15h*4 + 2],cs
        sti

        ; THE GATE - AND WHETHER IT IS OURS TO TOUCH AT ALL.
        ;
        ; Living in the HMA settles both questions before they are
        ; asked.  The detect probe works by SHUTTING the gate to see
        ; whether it moves, and from up here that instruction is the
        ; last one this driver would ever execute; and the answer it
        ; would reach - "nothing this driver does moves it" - is the
        ; one we already know, because rx_a20set will not let anything
        ; move it.  So say so and skip the probe.
        test    byte ptr [h_flags],RMHF_HIGH
        jz      rxi_lowgate
        mov     byte ptr [rx_a20meth],0
        mov     word ptr [rx_a20cnt],0
        jmp     short rxi_gated
rxi_lowgate:
        call    rx_a20detect

        ; Low, XMS starts with the gate shut and no enables
        ; outstanding, so that the driver's idea of A20 and the
        ; hardware's agree from the first call.  PM-DOS parked with it
        ; open because PM-DOS lives above the megabyte; nothing down
        ; here needs it, and the unpark forces it back on rather than
        ; trusting what it finds.
        mov     word ptr [rx_a20cnt],0
        call    rx_a20lower
IFDEF RMDOS_E9
        call    e9_a20init              ; which method, and did it shut
ENDIF
rxi_gated:

IFDEF XMSPOOLSTAMP
        ; LAST, because it is the only thing in rx_init that needs the
        ; A20 gate to be in a state anyone can name.  See rx_pooltest:
        ; it answers whether the addresses this driver is about to hand
        ; out are the memory it was given.
        ;
        ; AND IT LEAVES THE STAMPS BEHIND, which is the second reason to
        ; have it.  Every page in the pool holding its own address turns
        ; any read of extended memory into an oracle: a word read back
        ; through somebody else's descriptor either says where it really
        ; came from, or says that something has since written there.
        ; Zeros on a fresh machine say neither, so this is switched on
        ; separately from the trace and can be built into a release-speed
        ; kernel with -dXMSPOOLSTAMP.
        call    rx_pooltest
IFDEF DBG_LPT
        call    rdbg_xpt
ENDIF
ENDIF

        pop     es
        popa
        ret

;=====================================================================
; rx_done - put INT 15h back before the machine changes hands.
;
; RMDOS's whole image is about to be freed and handed to the next
; program that asks for memory, and a vector pointing into it is a
; call into whatever lands there.  PM-DOS only issues INT 15h at boot,
; so nothing would have noticed today - which is the reason to fix it
; today rather than on the release that adds the second caller.  It is
; the same reason park_recover calls vdm_stubs.
;
; Interrupts are already off: rd_goback turned them off on its way in.
;=====================================================================
rx_done:
        push    ax
        push    es
        cmp     word ptr [rx_oldi15 + 2],0
        je      rxd_never               ; rx_init never ran: a header this
                                        ; image refused, going straight back
        xor     ax,ax
        mov     es,ax
        mov     ax,word ptr [rx_oldi15]
        mov     word ptr es:[15h*4],ax
        mov     ax,word ptr [rx_oldi15 + 2]
        mov     word ptr es:[15h*4 + 2],ax
rxd_never:
        pop     es
        pop     ax
        ret

;=====================================================================
; INT 15h - AH=88h only, and everything else goes where it was going.
;=====================================================================
i15:
IFDEF RMDOS_E9
        call    e9_i15
ENDIF
        cmp     ah,88h
        je      i15_ext
        jmp     dword ptr cs:[rx_oldi15]
i15_ext:
        xor     ax,ax                   ; no extended memory this way
        push    bp
        mov     bp,sp
        and     word ptr [bp+6],0FFFEh  ; and CF clear, which the IRET
        pop     bp                      ; would otherwise put back
        iret

;=====================================================================
; The A20 gate.
;
; TWO WAYS TO MOVE IT AND NO WAY TO KNOW WHICH IN ADVANCE.  The
; original is a spare output pin on the keyboard controller, which is
; where IBM found a pin going spare in 1984; the other is bit 1 of
; port 92h, which is where every chipset since has put it because
; going through the 8042 costs tens of microseconds.  Machines exist
; with one, with the other, with both, and with neither - a gate wired
; permanently open is a real configuration.
;
; So the driver does not decide from a model byte.  It moves the gate
; and looks: writes a byte at 0000:0500 and reads at FFFF:0510, which
; is the same byte if the address wraps at the megabyte and a
; different one if it does not.  0500h is DOS's own scratch byte and
; the pair PM-DOS's boot-time check uses, chosen because a test that
; scribbles on the interrupt vector table to prove a point takes the
; machine with it.
;
;---------------------------------------------------------------------
; AND WHEN RMDOS IS IN THE HMA, THE GATE DOES NOT MOVE AT ALL.
;
; The gate is what makes the HMA exist.  With A20 off, segment FFFFh
; addresses 00000h-0FFEFh instead of 100000h-10FFEFh, so the very next
; instruction after an OUT that shut it would be fetched from the
; interrupt vector table - by a driver whose own code, data, stack and
; caller are all in the sixty-four kilobytes that just stopped
; existing.  There is no recovering from it and no way to test for it
; afterwards, because there is no afterwards.
;
; So when RMHF_HIGH is set the gate is nailed open: rx_a20set refuses
; to close it, which chocks every path at once - the detect probe, the
; two disable functions, and the block move's habit of putting the
; gate back the way it found it - and the disable functions report
; success anyway.  That is not a lie invented for this case: a gate
; wired permanently open is a real configuration, HIMEM has always had
; to present it, and every program that will ever ask this driver has
; a code path for it.  It is also exactly what a real DOS does when it
; loads itself high, and for the identical reason.
;
; The low path - the fallback when the image did not fit up there -
; leaves all of this alone, and it is the path the PARKWIN and RMTEST
; tests take from a prompt, so the ordinary gate code still gets run.
;=====================================================================

;--- rx_a20test - AL = 1 if the gate is open, 0 if it wraps ----------
rx_a20test:
        push    bx
        push    cx
        push    ds
        push    es
        push    si
        push    di
        pushf
        cli
        xor     ax,ax
        mov     ds,ax
        mov     si,500h
        dec     ax
        mov     es,ax                   ; FFFF
        mov     di,510h

        mov     bl,[si]                 ; keep both bytes
        mov     bh,es:[di]
        mov     byte ptr [si],00h
        mov     byte ptr es:[di],0FFh
        mov     cl,[si]                 ; and read the low one back
        mov     [si],bl                 ; put them back before deciding
        mov     es:[di],bh

        xor     ax,ax
        cmp     cl,0FFh
        je      a20t_out                ; the high write reached it: wrapped
        inc     ax
a20t_out:
        popf
        pop     di
        pop     si
        pop     es
        pop     ds
        pop     cx
        pop     bx
        ret

;--- the keyboard controller ----------------------------------------
rx_kbwait:
        push    ax
        push    cx
        xor     cx,cx
kbw_1:  in      al,64h
        test    al,2                    ; input buffer still full?
        jz      kbw_out
        loop    kbw_1
kbw_out:
        pop     cx
        pop     ax
        ret

; AL = 0DFh to open the gate, 0DDh to shut it
rx_kbgate:
        push    ax
        call    rx_kbwait
        mov     al,0D1h                 ; write output port
        out     64h,al
        call    rx_kbwait
        pop     ax
        out     60h,al
        call    rx_kbwait
        mov     al,0FFh                 ; a NULL command, to flush the
        out     64h,al                  ; controller before we look
        call    rx_kbwait
        ret

;--- port 92h, and the bit in it that must never be touched ----------
; BIT 0 OF PORT 92h IS FAST RESET.  Writing it restarts the machine.
; So this reads the port, changes bit 1, and masks bit 0 off on the
; way out whatever it was - which is not defensive, it is the only
; correct way to write this port.
rx_fastgate:                            ; AH = 2 to open, 0 to shut
        in      al,92h
        and     al,NOT 2
        or      al,ah
        and     al,0FEh
        out     92h,al
        ret

;--- rx_a20set - AH = 2 open / 0 shut, by whichever way works -------
; THE ONE CHOKE POINT.  Every route to the hardware comes through
; here, which is why the refusal is here and not at the five places
; that would each have needed their own.
rx_a20set:
        push    ax
        or      ah,ah                   ; asking to SHUT it?
        jnz     a20s_move
        test    byte ptr [h_flags],RMHF_HIGH
        jnz     a20s_none               ; not while we are living in it
a20s_move:
        cmp     byte ptr [rx_a20meth],1
        jne     a20s_fast
        mov     al,0DDh
        or      ah,ah
        jz      a20s_kb
        mov     al,0DFh
a20s_kb:
        call    rx_kbgate
        pop     ax
        ret
a20s_fast:
        cmp     byte ptr [rx_a20meth],2
        jne     a20s_none
        call    rx_fastgate
a20s_none:
        pop     ax
        ret

rx_a20raise:
        push    ax
        mov     ah,2
        call    rx_a20set
        pop     ax
        ret

rx_a20lower:
        push    ax
        xor     ah,ah
        call    rx_a20set
        pop     ax
        ret

;--- rx_a20detect - which of the two moves this machine's gate ------
; Tries each by asking for the state the gate is NOT in.  A method
; that cannot move it is not this machine's method, and if neither
; can, the gate is wired open and 03h has nothing to do.
rx_a20detect:
        push    ax
        push    bx
        call    rx_a20test
        mov     bl,al                   ; what it was
        mov     byte ptr [rx_a20meth],1
        call    rx_a20try
        jnc     a20d_out
        mov     byte ptr [rx_a20meth],2
        call    rx_a20try
        jnc     a20d_out
        mov     byte ptr [rx_a20meth],0
a20d_out:
        mov     ah,2                    ; and leave it as we found it
        or      bl,bl
        jnz     a20d_put
        xor     ah,ah
a20d_put:
        call    rx_a20set
        pop     bx
        pop     ax
        ret

; BL = the current state.  CF clear if [rx_a20meth] moved the gate.
rx_a20try:
        push    ax
        mov     ah,2                    ; ask for the opposite
        or      bl,bl
        jz      a20y_set
        xor     ah,ah
a20y_set:
        call    rx_a20set
        call    rx_a20test
        cmp     al,bl
        je      a20y_no                 ; nothing happened
        pop     ax
        clc
        ret
a20y_no:
        pop     ax
        stc
        ret

;=====================================================================
; The pool, and the handles that carve it up.
;
; First fit over a table of thirty-two records, which is the whole
; algorithm.  With that many handles a walk costs nothing and there is
; no free list to keep in step with anything - the allocated blocks
; ARE the map, and the gaps between them are what is left.
;
; Every block starts on a page boundary.  HIMEM aligns to a kilobyte
; because that is the unit XMS counts in, but the customer here builds
; page tables out of what it is given, and a block starting halfway
; through a page is a page it cannot use.
;=====================================================================

;--- rx_find - BX = handle -> SI = its record, CF if it is not one ---
rx_find:
        push    ax
        push    dx
        mov     ax,bx
        dec     ax
        cmp     ax,RX_MAXH
        jae     rf_bad
        mov     dx,RXH_LEN
        mul     dx
        mov     si,ax
        add     si,OFFSET rx_handles
        cmp     word ptr [si+RXH_FLAGS],1
        jne     rf_bad
        pop     dx
        pop     ax
        clc
        ret
rf_bad:
        pop     dx
        pop     ax
        stc
        ret

;--- rx_newh - SI = a free record, DI = its handle number, CF if none-
rx_newh:
        push    cx
        mov     si,OFFSET rx_handles
        mov     di,1
        mov     cx,RX_MAXH
rn_loop:
        cmp     word ptr [si+RXH_FLAGS],0
        je      rn_got
        add     si,RXH_LEN
        inc     di
        loop    rn_loop
        pop     cx
        stc
        ret
rn_got:
        pop     cx
        clc
        ret

;--- rx_freeh - CX = how many handles are still free -----------------
rx_freeh:
        push    si
        mov     si,OFFSET rx_handles
        xor     cx,cx
        push    dx
        mov     dx,RX_MAXH
rfh_loop:
        cmp     word ptr [si+RXH_FLAGS],0
        jne     rfh_next
        inc     cx
rfh_next:
        add     si,RXH_LEN
        dec     dx
        jnz     rfh_loop
        pop     dx
        pop     si
        ret

;--- rx_foot - SI = a record -> EDI = the bytes it occupies ----------
; What a block COSTS, which is what it was given rounded up to a page,
; not what its owner asked for.  Free space measured in what people
; asked for is free space that is not there.
rx_foot:
        mov     edi,[si+RXH_KB]
        shl     edi,10
        add     edi,RX_GRAN - 1
        and     edi,NOT (RX_GRAN - 1)
        ret

;--- THE REST IS IN RMXMS2.INC (2026-11-21): rx_owner, rx_scan, rx_stats, rx_entry, the calls
;--- and the block move.  The file is cut here so that this part can be in front
;--- of RMDOS's stack slots and that part behind them - RMSTK.INC says
;--- why the slots are pinned, and RMDOS.ASM includes the three.
