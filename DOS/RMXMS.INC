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
RX_PSLIST       EQU     40              ; dirty pages rx_poolscan names

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
RXE_NOUMB       EQU     0B0h
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
        xor     ax,ax
        mov     es,ax
        mov     ax,word ptr [rx_oldi15]
        mov     word ptr es:[15h*4],ax
        mov     ax,word ptr [rx_oldi15 + 2]
        mov     word ptr es:[15h*4 + 2],ax
        pop     es
        pop     ax
        ret

;=====================================================================
; INT 15h - AH=88h only, and everything else goes where it was going.
;=====================================================================
i15:
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

;--- rx_owner - EAX inside an allocated block?  CF + ECX = its end ---
rx_owner:
        push    edx
        push    esi
        push    edi
        mov     si,OFFSET rx_handles
        mov     dx,RX_MAXH
ro_loop:
        cmp     word ptr [si+RXH_FLAGS],1
        jne     ro_next
        mov     ecx,[si+RXH_ADDR]
        cmp     eax,ecx
        jb      ro_next
        call    rx_foot
        add     ecx,edi
        cmp     eax,ecx
        jb      ro_hit
ro_next:
        add     si,RXH_LEN
        dec     dx
        jnz     ro_loop
        pop     edi
        pop     esi
        pop     edx
        clc
        ret
ro_hit:
        pop     edi
        pop     esi
        pop     edx
        stc
        ret

;--- rxp_next - ECX = the lowest block start above EAX, or the top ----
rxp_next:
        push    ebx
        push    edx
        push    esi
        mov     ecx,[rx_end]
        mov     si,OFFSET rx_handles
        mov     bx,RX_MAXH
rnx_loop:
        cmp     word ptr [si+RXH_FLAGS],1
        jne     rnx_next
        mov     edx,[si+RXH_ADDR]
        cmp     edx,eax
        jbe     rnx_next
        cmp     edx,ecx
        jae     rnx_next
        mov     ecx,edx
rnx_next:
        add     si,RXH_LEN
        dec     bx
        jnz     rnx_loop
        pop     esi
        pop     edx
        pop     ebx
        ret

;--- rx_scan - EDX = bytes wanted, 0 to only measure -----------------
; CF clear: EAX = where it goes.  CF set: it does not fit.
; EBX = the largest gap in the pool, either way.
rx_scan:
        push    ecx
        push    esi
        push    edi
        xor     ebx,ebx
        mov     eax,[rx_base]
rxs_loop:
        cmp     eax,[rx_end]
        jae     rxs_none
        call    rx_owner
        jc      rxs_skip
        call    rxp_next
        mov     edi,ecx
        sub     edi,eax                 ; the gap in front of us
        cmp     edi,ebx
        jbe     rxs_nomax
        mov     ebx,edi
rxs_nomax:
        or      edx,edx
        jz      rxs_step
        cmp     edi,edx
        jb      rxs_step
        pop     edi
        pop     esi
        pop     ecx
        clc
        ret
rxs_step:
        mov     eax,ecx
        jmp     short rxs_loop
rxs_skip:
        mov     eax,ecx
        add     eax,RX_GRAN - 1
        and     eax,NOT (RX_GRAN - 1)
        jmp     short rxs_loop
rxs_none:
        pop     edi
        pop     esi
        pop     ecx
        stc
        ret

;--- rx_stats - EAX = the largest free block, EDX = all of it, bytes -
rx_stats:
        push    ebx
        push    ecx
        push    esi
        push    edi
        xor     edx,edx
        call    rx_scan
        mov     eax,ebx

        ; REPORTED ROUNDED DOWN TO A PAGE, because that is what can
        ; actually be handed out: a request is rounded UP to one, so a
        ; largest-free-block that is not a whole number of pages is a
        ; number no caller can successfully ask for.
        and     eax,NOT (RX_GRAN - 1)
        mov     edx,[rx_end]
        sub     edx,[rx_base]
        mov     si,OFFSET rx_handles
        mov     cx,RX_MAXH
rt_loop:
        cmp     word ptr [si+RXH_FLAGS],1
        jne     rt_next
        call    rx_foot
        sub     edx,edi
rt_next:
        add     si,RXH_LEN
        loop    rt_loop
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        ret

;=====================================================================
; rx_entry - the far call every XMS client makes.
;
; A program finds this through INT 2Fh AX=4310h and then never uses an
; interrupt again, which is why the entry point has to be sixteen-bit
; code at a real-mode address - here, trivially, because all of RMDOS
; is.  The kernel's own XMS host has to go to some trouble over the
; same requirement; see the note on the HLT trick in XMS.INC.
;
; The XMS specification says a driver preserves every register it does
; not return a value in, so the five that no function returns are
; saved and restored around the lot.
;=====================================================================
RX_MAXFN        EQU     12h
RX_REV          EQU     0046h           ; the driver revision 00h reports,
                                        ; which is PM-DOS's own version

rx_entry:
        push    ds
        push    es
        push    si
        push    di
        push    bp

        cmp     ah,0Bh
        jne     rx_nodesc

        ; The move descriptor, copied in WHILE DS:SI STILL MEANS WHAT
        ; THE CALLER MEANT.  Everything below this line reaches this
        ; file's data through DS, and a routine that keeps the caller's
        ; DS and then calls anything is the bug rd_path had.
        push    ax
        push    cx
        mov     ax,cs
        mov     es,ax
        mov     di,OFFSET rx_desc
        mov     cx,XM_SIZE / 2
        cld
        rep     movsw
        pop     cx
        pop     ax
rx_nodesc:
        mov     si,cs
        mov     ds,si
        mov     es,si
IFDEF DBG_LPT
        call    rdbg_xin                ; the question, held until the answer
ENDIF
        cmp     ah,RX_MAXFN
        ja      rx_high
        movzx   si,ah
        shl     si,1
        jmp     word ptr [rx_tab + si]

;--- the XMS 3.0 additions, which are the same questions in EAX -----
rx_high:
        cmp     ah,88h
        je      rxf88
        cmp     ah,89h
        je      rxf89
        cmp     ah,8Eh
        je      rxf8E
        cmp     ah,8Fh
        je      rxf8F
        mov     bl,RXE_NOTIMPL
        jmp     short rxr_err

rxr_ok:
        mov     ax,1
        jmp     short rx_ret
rxr_err:                                 ; BL = why
        xor     ax,ax
rx_ret:
IFDEF DBG_LPT
        call    rdbg_xout               ; ...and both, recorded together
ENDIF
        pop     bp
        pop     di
        pop     si
        pop     es
        pop     ds
        retf

;=====================================================================
; 00h - what this driver is.
;
; AX = 0300h is the SPECIFICATION it implements, BX its own revision,
; and DX whether an HMA exists - which depends on where this image
; parked, and is worked out rather than assumed.  See rxf01.
;=====================================================================
rxf00:
        mov     bx,RX_REV
        xor     dx,dx
        test    byte ptr [h_flags],RMHF_HIGH
        jnz     f00_nohma
        inc     dx                      ; low: the HMA is the guest's
f00_nohma:
        mov     ax,0300h
        jmp     rx_ret

;=====================================================================
; 01h / 02h - the HMA.
;
; IT DEPENDS ON WHERE THIS IMAGE IS, and until v0.53 it did not: the
; answer was a flat "no HMA exists", written when RMDOS always parked
; into the HMA and was therefore standing in the only 64K it could
; have handed out.  RMDOS parks LOW by default now - the Windows
; kernel scans for the SFT only as far as 80000h and cannot find a DOS
; above it - so on the ordinary path the HMA is empty and this was
; refusing memory that nobody was using.
;
; And it is refusing it to the one program that most wants it.  The
; XMS log of a standard-mode start reads:
;
;       AH=01 req HMA  DX=FFFF  ->  AX=0000  FAILED BL=90
;
; DX=FFFFh is the specification's "I am an application, not a TSR, so
; the HMAMIN threshold does not apply to me" - which is DOSX asking
; for the whole of it, exactly as it would of HIMEM.
;
; The 64K IS FREE, and by construction rather than by luck: PMDOS.INC
; leaves 100000h-10FFEFh empty on purpose and loads the kernel at
; 110000h instead, precisely so that a guest's FFFF:xxxx means what it
; says.  So when this image is low, the HMA is the guest's to ask for,
; and when it is high the old answer is still the right one - it is
; this driver's own code up there.
;
; A20 is the caller's business, as it always is: the HMA does not
; exist as an address until the gate is open, and 03h/05h are how a
; program says so.
;=====================================================================
rxf01:
        test    byte ptr [h_flags],RMHF_HIGH
        jnz     f01_nohma               ; we are standing in it
        cmp     byte ptr [rx_hmaown],0
        jne     f01_inuse
        cmp     dx,0FFFFh
        je      f01_grant               ; an application takes all of it
        cmp     dx,RX_HMAMIN
        jb      f01_toosmall
f01_grant:
        mov     byte ptr [rx_hmaown],1
        jmp     rxr_ok
f01_nohma:
        mov     bl,RXE_NOHMA
        jmp     rxr_err
f01_inuse:
        mov     bl,RXE_HMAINUSE
        jmp     rxr_err
f01_toosmall:
        mov     bl,RXE_HMAMIN
        jmp     rxr_err

rxf02:
        test    byte ptr [h_flags],RMHF_HIGH
        jnz     f01_nohma
        cmp     byte ptr [rx_hmaown],0
        je      f02_notours
        mov     byte ptr [rx_hmaown],0
        jmp     rxr_ok
f02_notours:
        mov     bl,RXE_HMANOTUSED
        jmp     rxr_err

;=====================================================================
; 03h-07h - the A20 gate.
;
; GLOBAL and LOCAL are two different questions and the specification
; means both.  A global enable is a program saying "leave it open"; a
; local one is a program saying "I need it for the next few
; instructions" and pairs with a local disable.  So local enables are
; COUNTED and a global disable will not close a gate somebody is
; standing in - which is the one place these five functions can do
; real damage, because the program holding it open is reading memory
; that stops existing the moment the gate shuts.
;=====================================================================
rxf03:
        call    rx_a20raise
        mov     al,1
        jmp     short rx_a20done
rxf04:
        cmp     word ptr [rx_a20cnt],0
        jne     rxf04_held
        call    rx_a20lower
        xor     al,al
        jmp     short rx_a20done
rxf04_held:
        mov     bl,RXE_A20STILL
        jmp     rxr_err

rxf05:
        inc     word ptr [rx_a20cnt]
        call    rx_a20raise
        mov     al,1
        jmp     short rx_a20done
rxf06:
        cmp     word ptr [rx_a20cnt],0
        je      rxf06_none
        dec     word ptr [rx_a20cnt]
        mov     al,1
        jnz     rx_a20done              ; somebody else still holds it
        call    rx_a20lower
        xor     al,al
        jmp     short rx_a20done
rxf06_none:
        mov     bl,RXE_A20
        jmp     rxr_err

; AL = THE STATE WE ASKED FOR, and every one of these ends by LOOKING
; rather than by assuming the hardware did what it was told.  A
; machine whose gate this driver cannot move is a machine where 03h
; has to FAIL - not one where it reports success and the caller then
; reads the wrong megabyte.
rx_a20done:
        mov     [rx_a20want],al
        ; ...unless the answer was "shut it" and we are living up
        ; there, in which case the gate is deliberately still open and
        ; saying so would fail a call the caller can do nothing about.
        ; A wired-open gate is a configuration, not an error.
        or      al,al
        jnz     rxa_check
        test    byte ptr [h_flags],RMHF_HIGH
        jz      rxa_check
        jmp     rxr_ok
rxa_check:
        call    rx_a20test
        cmp     al,[rx_a20want]
        jne     rxa_bad
        jmp     rxr_ok
rxa_bad:
        mov     bl,RXE_A20
        jmp     rxr_err

;--- 07h - is it open?  AX = 1 or 0 and BL = 0 either way -----------
rxf07:
        call    rx_a20test
        xor     bl,bl
        jmp     rx_ret

;=====================================================================
; rx_alloc - EDX = kilobytes wanted.
;            CF clear: BX = the handle.  CF set: BL = why not.
;
; DESTROYS EBX, which is the answer.  Said here because the last time
; a routine in this tree answered in a register, a later edit saved
; and restored it and every caller got its own value back.
;=====================================================================
rx_alloc:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        mov     ecx,edx                 ; the size, as asked for
        cmp     ecx,400000h             ; four gigabytes of kilobytes
        jae     ra_nomem
        call    rx_newh                 ; -> SI, DI
        jc      ra_nohandle
        mov     edx,ecx
        shl     edx,10
        add     edx,RX_GRAN - 1
        and     edx,NOT (RX_GRAN - 1)
        jz      ra_zero                 ; a handle for no memory is legal
        call    rx_scan
        jc      ra_nomem
        jmp     short ra_got
ra_zero:
        mov     eax,[rx_base]
ra_got:
        mov     word ptr [si+RXH_FLAGS],1
        mov     [si+RXH_KB],ecx
        mov     [si+RXH_ADDR],eax
        mov     word ptr [si+RXH_LOCK],0
        mov     bx,di
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
ra_nohandle:
        mov     bl,RXE_NOHANDLE
        jmp     short ra_bad
ra_nomem:
        mov     bl,RXE_NOMEM
ra_bad:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; 08h / 88h - how much is there?
;
; 08h answers in kilobytes in sixteen bits, which saturates at 64MB
; minus a kilobyte and has done since 1988.  88h is the same question
; asked in EAX, and is the reason this driver reports version 3.00
; rather than 2.00.
;=====================================================================
rxf08:
        call    rx_stats                ; EAX largest, EDX total, in bytes
        shr     eax,10
        shr     edx,10
        cmp     eax,0FFFFh
        jbe     f08_cap1
        mov     eax,0FFFFh
f08_cap1:
        cmp     edx,0FFFFh
        jbe     f08_cap2
        mov     edx,0FFFFh
f08_cap2:
        xor     bl,bl
        or      ax,ax
        jnz     f08_out
        mov     bl,RXE_NOMEM
f08_out:
        jmp     rx_ret

rxf88:
        call    rx_stats
        shr     eax,10
        shr     edx,10
        mov     ecx,[rx_end]
        or      ecx,ecx
        jz      f88_none
        dec     ecx                     ; the highest address, not the top
f88_none:
        xor     bl,bl
        or      eax,eax
        jnz     f88_out
        mov     bl,RXE_NOMEM
f88_out:
        jmp     rx_ret

;--- 09h / 89h - allocate.  DX or EDX = KB, DX = the handle back ----
rxf09:
        movzx   edx,dx
        jmp     short rxf09_common
rxf89:
                                        ; 89h: EDX is already the size
rxf09_common:
        call    rx_alloc
        jc      f09_bad
        mov     dx,bx
        xor     bl,bl
        jmp     rxr_ok
f09_bad:
        jmp     rxr_err

;--- 0Ah - free.  A locked block is somebody's DMA target -----------
rxf0A:
        mov     bx,dx
        call    rx_find
        jc      f0A_bad
        cmp     word ptr [si+RXH_LOCK],0
        jne     f0A_locked
        mov     word ptr [si+RXH_FLAGS],0
        jmp     rxr_ok
f0A_bad:
        mov     bl,RXE_BADHANDLE
        jmp     rxr_err
f0A_locked:
        mov     bl,RXE_LOCKED
        jmp     rxr_err

;=====================================================================
; 0Ch / 0Dh - lock and unlock.
;
; A LOCK IS A PROMISE NOT TO MOVE THE BLOCK, and it is answered with
; the block's PHYSICAL address, because the caller is about to give
; that number to something that does not go through the processor at
; all - a disk controller, or a page table.  This driver never moves a
; block behind its owner's back, so the promise costs nothing to keep
; and the count exists to stop 0Ah and 0Fh from breaking it.
;
; The number is the truth here only because PM-DOS's page map is the
; identity below its own ceiling, so a linear address IS a physical
; one.  A manager that paged would have to pin the pages first.
;=====================================================================
rxf0C:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        cmp     word ptr [si+RXH_LOCK],0FFh
        jae     f0C_toomany
        inc     word ptr [si+RXH_LOCK]
        mov     eax,[si+RXH_ADDR]
        mov     bx,ax
        shr     eax,16
        mov     dx,ax
        jmp     rxr_ok
f0C_bad:
        mov     bl,RXE_BADHANDLE
        jmp     rxr_err
f0C_toomany:
        mov     bl,RXE_LOCKCOUNT
        jmp     rxr_err

rxf0D:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        cmp     word ptr [si+RXH_LOCK],0
        je      f0D_notlocked
        dec     word ptr [si+RXH_LOCK]
        jmp     rxr_ok
f0D_notlocked:
        mov     bl,RXE_NOTLOCKED
        jmp     rxr_err

;--- 0Eh / 8Eh - what is this handle? -------------------------------
rxf0E:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        mov     eax,[si+RXH_KB]
        cmp     eax,0FFFFh
        jbe     f0E_fits
        mov     eax,0FFFFh
f0E_fits:
        mov     dx,ax
        call    rx_freeh                ; -> CX
        mov     ax,word ptr [si+RXH_LOCK]
        mov     bh,al
        mov     bl,cl
        jmp     rxr_ok

rxf8E:
        mov     bx,dx
        call    rx_find
        jc      f0C_bad
        mov     edx,[si+RXH_KB]
        call    rx_freeh
        mov     ax,word ptr [si+RXH_LOCK]
        mov     bh,al
        mov     bl,cl
        jmp     rxr_ok

;--- 10h/11h/12h - upper memory, which came from EMM386 and not -----
;    from HIMEM.  See the head of this file.
rxf10:
        xor     dx,dx                   ; and none available, either
        mov     bl,RXE_NOUMB
        jmp     rxr_err
rxf11:
        mov     bl,RXE_BADUMB
        jmp     rxr_err
rxf12:
        mov     bl,RXE_NOUMB
        jmp     rxr_err

;=====================================================================
; 0Fh / 8Fh - reallocate.
;
; Shrinking is a change to one field.  Growing takes the block OUT of
; the map before looking for room, so that the scan can offer the
; space the block is already standing in - which is where a block that
; grows almost always ends up, and a scan that could not see it would
; move a megabyte for nothing.  If it does have to move, the contents
; go with it: a realloc that loses the data is not a realloc.
;=====================================================================
rxf0F:
        movzx   ebx,bx
        jmp     short rx_realloc
rxf8F:
        jmp     short rx_realloc        ; EBX is already the whole size

rx_realloc:
        mov     [rx_newkb],ebx
        mov     bx,dx
        call    rx_find
        jc      f0F_badh
        mov     [rx_rec],si
        cmp     word ptr [si+RXH_LOCK],0
        jne     f0F_locked

        call    rx_foot                 ; -> EDI, what it occupies now
        mov     [rx_oldfoot],edi
        mov     edx,[rx_newkb]
        cmp     edx,400000h
        jae     f0F_nomem
        shl     edx,10
        add     edx,RX_GRAN - 1
        and     edx,NOT (RX_GRAN - 1)
        cmp     edx,edi
        jbe     f0F_fits

        mov     word ptr [si+RXH_FLAGS],0
        call    rx_scan
        mov     si,[rx_rec]
        mov     word ptr [si+RXH_FLAGS],1
        jc      f0F_nomem
        cmp     eax,[si+RXH_ADDR]
        je      f0F_fits

        mov     [rx_newaddr],eax
        mov     ecx,[si+RXH_ADDR]
        mov     esi,ecx
        mov     edi,[rx_newaddr]
        mov     ecx,[rx_oldfoot]
        call    rx_domove
        mov     si,[rx_rec]
        mov     eax,[rx_newaddr]
        mov     [si+RXH_ADDR],eax
f0F_fits:
        mov     si,[rx_rec]
        mov     eax,[rx_newkb]
        mov     [si+RXH_KB],eax
        jmp     rxr_ok
f0F_badh:
        mov     bl,RXE_BADHANDLE
        jmp     rxr_err
f0F_locked:
        mov     bl,RXE_LOCKED
        jmp     rxr_err
f0F_nomem:
        mov     bl,RXE_NOMEM
        jmp     rxr_err

;=====================================================================
; 0Bh - move a block, which is the function this whole file is built
; around and the only one a real HIMEM needs protected mode for.
;
; Either end may be a handle with an offset into it, or handle zero
; with a real-mode SEGMENT:OFFSET in the same dword - which is how a
; program moves data between conventional and extended memory, and
; why this driver has to understand both kinds of address.
;=====================================================================
rxf0B:
        mov     ecx,dword ptr [rx_desc + XM_LEN]
        test    ecx,1
        jnz     f0B_badlen
        or      ecx,ecx
        jz      xmv_none                ; nothing to do, done successfully

        mov     bx,word ptr [rx_desc + XM_SRCH]
        mov     edx,dword ptr [rx_desc + XM_SRCO]
        call    rx_resolve
        jc      f0B_srcbad
        mov     [rx_msrc],eax

        mov     bx,word ptr [rx_desc + XM_DSTH]
        mov     edx,dword ptr [rx_desc + XM_DSTO]
        call    rx_resolve
        jc      f0B_dstbad

        mov     edi,eax
        mov     esi,[rx_msrc]
        mov     ecx,dword ptr [rx_desc + XM_LEN]
        call    rx_domove
        jc      f0B_failed
xmv_none:
        jmp     rxr_ok
f0B_badlen:
        mov     bl,RXE_BADLEN
        jmp     rxr_err
f0B_srcbad:
        mov     bl,RXE_BADSRCH
        or      al,al
        jz      f0B_srcout
        mov     bl,RXE_BADSRCO
f0B_srcout:
        jmp     rxr_err
f0B_dstbad:
        mov     bl,RXE_BADDSTH
        or      al,al
        jz      f0B_dstout
        mov     bl,RXE_BADDSTO
f0B_dstout:
        jmp     rxr_err
f0B_failed:
        mov     bl,RXE_PARITY
        jmp     rxr_err

;---------------------------------------------------------------------
; rx_resolve - BX = handle, EDX = offset, ECX = length.
;              CF clear: EAX = the flat address.
;              CF set:   AL = 0 the handle is wrong, 1 the offset is.
;---------------------------------------------------------------------
rx_resolve:
        push    ecx
        push    edi
        push    esi
        or      bx,bx
        jnz     rr_handle

        ; Handle zero: the dword is a real-mode pointer, and this is
        ; the one place in the driver where a segment still means
        ; sixteen times something.
        mov     eax,edx
        shr     eax,16
        shl     eax,4
        movzx   edx,dx
        add     eax,edx
        jmp     short rr_ok
rr_handle:
        call    rx_find
        jc      rr_badh
        mov     eax,[si+RXH_KB]
        shl     eax,10
        mov     edi,edx
        add     edi,ecx                 ; the far end of what was asked for
        jc      rr_bado                 ; which must not have wrapped
        cmp     edi,eax
        ja      rr_bado
        mov     eax,[si+RXH_ADDR]
        add     eax,edx
rr_ok:
        pop     esi
        pop     edi
        pop     ecx
        clc
        ret
rr_badh:
        mov     al,0
        jmp     short rr_bad
rr_bado:
        mov     al,1
rr_bad:
        pop     esi
        pop     edi
        pop     ecx
        stc
        ret

;=====================================================================
; rx_domove - ESI, EDI, ECX.  CF set if it could not be done.
;
; The gate has to be open for the length of it, whatever the caller's
; A20 state was, because at least one end of the move is above the
; megabyte by definition - and it has to go back to what it was
; afterwards, because a program that had A20 shut and finds it open
; is a program reading somebody else's memory.
;=====================================================================
rx_domove:
        or      ecx,ecx
        jz      dm_nothing
        pushf
        cli
        call    rx_a20test
        push    ax                      ; how we found the gate
        call    rx_a20raise

IFDEF RMDOS_I15MOVE
        call    rx_move15               ; forced, so that the path a
ELSE                                    ; monitor would take gets run
        smsw    ax
        test    al,1                    ; PE set: protected mode is not
        jnz     dm_notours              ; ours to leave
        call    rx_moveflat
        jmp     short dm_gate
dm_notours:
        call    rx_move15
ENDIF
dm_gate:
        mov     byte ptr [rx_mverr],0
        jnc     dm_kept
        mov     byte ptr [rx_mverr],1
dm_kept:
        pop     ax
        or      al,al
        jnz     dm_wasopen
        call    rx_a20lower
dm_wasopen:
        popf                            ; and CF is the CALLER'S again,
        cmp     byte ptr [rx_mverr],0   ; which is why the answer waited
        je      dm_nothing              ; in memory to be put back here
        stc
        ret
dm_nothing:
        clc
        ret

;=====================================================================
; rx_moveflat - the real thing: leave real mode, load two descriptors
; that cover the whole address space, come back, and copy.
;
; THE SEGMENT REGISTERS KEEP WHAT THEY WERE GIVEN.  A 386 loads a
; descriptor into a hidden cache when a segment register is written
; and uses the cache, not the descriptor, until it is written again -
; so a DS loaded in protected mode with a four-gigabyte limit still
; has one after the processor goes back to real mode.  The copy below
; is ordinary real-mode code addressing the whole machine, and this is
; the same trick every HIMEM has used since 1988.
;
; The rules it lives under: interrupts are already off, nothing may
; reload DS or ES until the copy is finished, and nothing in between
; may touch this file's own data - because DS no longer reaches it.
;=====================================================================
rx_moveflat:
        push    ds
        push    es
        push    ebp

        ; Which way to copy, decided here because deciding it later
        ; would mean reading a variable we can no longer address.
        xor     ebp,ebp                 ; 0 = forwards
        cmp     edi,esi
        jbe     mf_dir
        mov     eax,esi
        add     eax,ecx
        cmp     edi,eax
        jae     mf_dir
        mov     ebp,1                   ; they overlap and the destination
mf_dir:                                 ; is the higher: copy downwards

        db      66h                     ; so that all thirty-two bits of
        lgdt    fword ptr cs:[rx_gdtr]  ; the base are loaded
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short mf_pe
mf_pe:
        mov     ax,8                    ; the flat data descriptor
        mov     ds,ax
        mov     es,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short mf_rm
mf_rm:
        or      ebp,ebp
        jnz     mf_backward

        mov     edx,ecx
        and     edx,3
        shr     ecx,2
        or      ecx,ecx
        jz      mf_tail
mf_dwords:
        mov     eax,ds:[esi]
        mov     es:[edi],eax
        add     esi,4
        add     edi,4
        dec     ecx
        jnz     mf_dwords
mf_tail:
        mov     ecx,edx
        or      ecx,ecx
        jz      mf_end
mf_bytes:
        mov     al,ds:[esi]
        mov     es:[edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     mf_bytes
        jmp     short mf_end

mf_backward:
        add     esi,ecx
        add     edi,ecx
mf_bloop:
        dec     esi
        dec     edi
        mov     al,ds:[esi]
        mov     es:[edi],al
        dec     ecx
        jnz     mf_bloop
mf_end:
        ; and these two POPs are what puts the sixty-four kilobyte
        ; limits back - loading a segment register in real mode
        ; rewrites the cache it was carrying
        pop     ebp
        pop     es
        pop     ds
        clc
        ret

IFDEF XMSPOOLSTAMP
;=====================================================================
; rx_pooltest - is the memory at the address this driver reports the
;               same memory it actually handed over?
;
; THE QUESTION THE FAULT LEFT BEHIND.  Windows 3.1 standard mode dies
; on an IRET inside DOSX with the client's own stack selector loaded,
; and the stack behind that selector reads as zeros - before DOSX has
; written anything.  The selector's base is memory DOSX got from this
; driver through DPMI 0501h, so either the kernel's copy into it never
; landed, or THE ADDRESS THIS DRIVER REPORTS IS NOT THE MEMORY IT GAVE
; AWAY.  The second is ours to get wrong, so it is the one to rule out
; first, and it can be ruled out here without Windows in the room.
;
; The test is the only one that distinguishes the two: every page in
; the pool is stamped with ITS OWN LINEAR ADDRESS and then read back.
; A pool that answers correctly everywhere is a pool of distinct,
; present, correctly-addressed memory.  A pool where page X reads back
; the stamp of page Y is aliased, and the difference X-Y names the
; address line that is not getting through - A20 at 100000h, or a chip
; select the emulated 440BX never decodes, or a bug of ours.  A pool
; that reads back nothing at all is not there.
;
; It runs where the answer counts: after the park, in real mode, with
; PM-DOS's page tables gone, which is exactly the machine Windows will
; see.  A20 is forced open for the duration and put back afterwards,
; because half the pool has bit 20 set and testing it through a shut
; gate would only rediscover the gate.
;
; Stamping destroys the pool's contents, which is why this is DBG_LPT
; only and why it runs from rx_init - before any handle exists, so
; there is nothing in there to destroy.
;=====================================================================
rx_pooltest:
        pushad
        pushf
        cli
        mov     dword ptr [rx_ptbad],0FFFFFFFFh
        mov     dword ptr [rx_ptval],0
        mov     byte ptr [rx_ptflg],0

        call    rx_a20test
        or      al,al
        jz      pt_shut
        or      byte ptr [rx_ptflg],1   ; how the gate was found, because a
pt_shut:                                ; gate that was shut explains a lot
        call    rx_a20raise

        ; LEAVING REAL MODE HAS TO BE OURS TO DO.  The same test
        ; rx_domove makes, for the same reason: under a monitor CR0
        ; belongs to somebody else, and there is no INT 15h service
        ; that would do this one.
        smsw    ax
        test    al,1
        jz      pt_run
        or      byte ptr [rx_ptflg],2
        jmp     short pt_gate
pt_run:
        mov     esi,[rx_base]
        mov     ebp,[rx_end]
        call    rx_ptflat
        mov     [rx_ptbad],edi
        mov     [rx_ptval],edx
        cmp     edi,0FFFFFFFFh
        jne     pt_gate
        mov     [rx_ptval],ecx          ; nothing wrong: say how much was
pt_gate:                                ; checked, so that a silent pass
        test    byte ptr [rx_ptflg],1   ; is still a measurement
        jnz     pt_wasopen
        call    rx_a20lower
pt_wasopen:
        popf
        popad
        ret

;---------------------------------------------------------------------
; rx_ptflat - the two passes, in unreal mode.  ESI = base, EBP = end.
;
;   EDI = the first address that lied, or FFFFFFFFh
;   EDX = what it said instead
;   ECX = pages checked
;
; Under the same rules as rx_moveflat: interrupts already off, and
; nothing between the two segment loads may touch this file's data,
; because DS no longer reaches it.  Both ends of the pool arrive in
; registers for that reason.
;---------------------------------------------------------------------
rx_ptflat:
        push    ds
        push    es
        push    ebx
        push    esi
        push    ebp

        db      66h
        lgdt    fword ptr cs:[rx_gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short pf_pe
pf_pe:
        mov     ax,8                    ; the flat data descriptor
        mov     ds,ax
        mov     es,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short pf_rm
pf_rm:
        ; EVERY DWORD, not one per page.  A stamp every 4096 bytes
        ; answers "is this page distinct memory", which was the
        ; aliasing question and is now settled; it cannot answer "did
        ; anything write HERE", because a stack 0030h into a page is
        ; nowhere near the one word being watched.  Thirty-two million
        ; stamps take a few seconds of emulated time and turn any read
        ; of extended memory, at any offset, into an oracle: the dword
        ; either says its own address or says somebody wrote it.
        mov     ebx,esi                 ; pass one
        xor     ecx,ecx
pf_stamp:
        cmp     ebx,ebp
        jae     pf_verify
        mov     ds:[ebx],ebx
        add     ebx,4
        inc     ecx
        jmp     short pf_stamp
pf_verify:
        mov     ebx,esi                 ; pass two
pf_check:
        cmp     ebx,ebp
        jae     pf_ok
        mov     eax,ds:[ebx]
        cmp     eax,ebx
        jne     pf_bad
        add     ebx,4
        jmp     short pf_check
pf_bad:
        mov     edi,ebx
        mov     edx,eax
        jmp     short pf_out
pf_ok:
        mov     edi,0FFFFFFFFh
        xor     edx,edx
pf_out:
        pop     ebp                     ; and these put the real-mode
        pop     esi                     ; limits back
        pop     ebx
        pop     es
        pop     ds
        ret

;=====================================================================
; rx_poolscan - and afterwards, WHICH PAGES LOST THEIR STAMP.
;
; The stamps rx_pooltest leaves are only half an instrument.  The other
; half is asking, once the excursion is over, which of them are gone -
; because a page that no longer holds its own address is a page
; something WROTE TO, and the set of those pages is a map of every byte
; of extended memory Windows actually used.
;
; That map is the question the standard-mode fault reduces to.  DOSX
; hands KRNL386 a block out of this pool and the kernel repoints its
; own stack selector at it; the memory behind that selector reads back
; its stamp, untouched, so the kernel's copy went SOMEWHERE ELSE.  A
; count of nothing says the DPMI memory path never wrote at all; a
; count confined to the low block says the high one cannot be reached.
;
; Called from rd_goback under RMDOS_FREEZE, when there is nothing left
; to disturb.
;=====================================================================
rx_poolscan:
        pushad
        pushf
        cli
        call    rx_a20test
        push    ax
        call    rx_a20raise
        smsw    ax
        test    al,1
        jnz     ps_gate
        mov     esi,[rx_base]
        mov     ebp,[rx_end]
        call    rx_psflat
        mov     [rx_psn],ecx
        mov     [rx_psfirst],edi
        mov     [rx_pslast],edx

        ; AND THE ADDRESSES THEMSELVES, which is the map.  One scan per
        ; entry, restarting a page past the last one found: twenty-four
        ; passes over 32000 pages is nothing next to a boot, and it
        ; needs no register inside the unreal loop to hold a table
        ; pointer - there are none spare in there, and DS is flat, so
        ; the loop could not reach this data anyway.
        mov     bx,OFFSET rx_pslist
        mov     cx,RX_PSLIST
        mov     esi,[rx_base]
psl_loop:
        push    cx
        push    bx
        mov     ebp,[rx_end]
        call    rx_psflat
        pop     bx
        pop     cx
        cmp     edi,0FFFFFFFFh
        je      ps_gate
        mov     [bx],edi
        add     bx,4
        mov     esi,edi
        add     esi,1000h
        loop    psl_loop
ps_gate:
        pop     ax
        or      al,al
        jnz     ps_wasopen
        call    rx_a20lower
ps_wasopen:
        popf
        popad
        ret

;--- the scan itself, unreal, under rx_ptflat's rules ----------------
; ESI = base, EBP = end.  ECX = pages written, EDI = the first of them,
; EDX = the last.
rx_psflat:
        push    ds
        push    es
        push    ebp
        push    esi

        db      66h
        lgdt    fword ptr cs:[rx_gdtr]
        mov     eax,cr0
        or      al,1
        mov     cr0,eax
        jmp     short ps_pe
ps_pe:
        mov     ax,8
        mov     ds,ax
        mov     es,ax
        mov     eax,cr0
        and     al,0FEh
        mov     cr0,eax
        jmp     short ps_rm
ps_rm:
        mov     ebx,esi
        xor     ecx,ecx
        mov     edi,0FFFFFFFFh
        xor     edx,edx
ps_loop:
        cmp     ebx,ebp
        jae     ps_out
        mov     esi,ebx                 ; ESI is free once EBX has the
ps_page:                                ; base: every dword of this page
        mov     eax,ds:[esi]
        cmp     eax,esi
        jne     ps_dirty
        add     esi,4
        test    si,0FFFh                ; a page ends when the low twelve
        jnz     ps_page                 ; bits come back to zero
        jmp     short ps_next
ps_dirty:
        inc     ecx
        mov     edx,ebx                 ; the last page seen, always
        cmp     edi,0FFFFFFFFh
        jne     ps_next
        mov     edi,ebx                 ; and the first, once
ps_next:
        add     ebx,1000h
        jmp     short ps_loop
ps_out:
        pop     esi
        pop     ebp
        pop     es
        pop     ds
        ret

;=====================================================================
; rx_psshow - the three numbers onto the bottom line of the screen.
;
; Written straight into the text page rather than through anything,
; because by the time this runs the excursion has ended and there is no
; DOS underneath it - and because the line above is a fault report this
; must not scroll away.
;=====================================================================
rx_psshow:
        pushad
        push    es
        mov     ax,0B800h
        mov     es,ax
        mov     di,24*160
        mov     eax,[rx_psn]
        call    rx_pshex
        add     di,4
        mov     eax,[rx_psfirst]
        call    rx_pshex
        add     di,4
        mov     eax,[rx_pslast]
        call    rx_pshex

        ; the map itself, eight to a row, on the three rows above
        mov     si,OFFSET rx_pslist
        mov     cx,RX_PSLIST
        mov     di,19*160
        xor     bp,bp
pss_loop:
        mov     eax,[si]
        or      eax,eax
        jz      pss_done
        call    rx_pshex
        add     di,2
        add     si,4
        inc     bp
        cmp     bp,8
        jb      pss_next
        xor     bp,bp
        add     di,160
        sub     di,8*18                 ; back to column 0 of the next row
pss_next:
        loop    pss_loop
pss_done:
        pop     es
        popad
        ret

;--- EAX in hex at ES:DI, bright white, DI left after it ------------
rx_pshex:
        push    ecx
        push    bx
        mov     cx,8
psh_l:
        rol     eax,4
        mov     bl,al
        and     bl,0Fh
        add     bl,'0'
        cmp     bl,'9'
        jbe     psh_dig
        add     bl,7
psh_dig:
        mov     byte ptr es:[di],bl
        mov     byte ptr es:[di+1],1Fh
        add     di,2
        loop    psh_l
        pop     bx
        pop     ecx
        ret
ENDIF

;=====================================================================
; rx_move15 - the same move, asked of the machine instead of done.
;
; INT 15h AH=87h is the BIOS's own block move: it makes the switch
; itself, from a descriptor table the caller fills in.  It is here for
; the case where LEAVING REAL MODE IS NOT OURS TO DO - once Windows is
; running this driver's code is inside a virtual 8086 machine and CR0
; belongs to somebody else.  SMSW answers that question without being
; privileged: PE is clear in real mode and set under a monitor.
;
; Taken rather than refusing, because a monitor that puts a DOS box in
; V86 mode virtualises 87h - it is how everything moved extended
; memory before XMS was written, so everything that virtualises DOS
; handles it.
;=====================================================================
rx_move15:
        push    ds
        push    es
        push    ebp
mv15_loop:
        or      ecx,ecx
        jz      mv15_done
        mov     ax,cs                   ; each pass, because the BIOS is
        mov     ds,ax                   ; entitled to both of these
        mov     es,ax
        mov     eax,ecx
        cmp     eax,10000h              ; 8000h words is all 87h takes
        jbe     mv15_size
        mov     eax,10000h
mv15_size:
        mov     ebp,eax

        mov     bx,OFFSET rx_gdt87 + 10h
        call    mv15_desc
        push    esi
        mov     esi,edi
        mov     bx,OFFSET rx_gdt87 + 18h
        call    mv15_desc
        pop     esi

        push    ecx
        push    esi
        mov     ecx,ebp
        shr     ecx,1
        mov     si,OFFSET rx_gdt87
        mov     ah,87h
        int     15h
        pop     esi
        pop     ecx
        jc      mv15_fail
        sub     ecx,ebp
        add     esi,ebp
        add     edi,ebp
        jmp     short mv15_loop
mv15_done:
        pop     ebp
        pop     es
        pop     ds
        clc
        ret
mv15_fail:
        pop     ebp
        pop     es
        pop     ds
        stc
        ret

; BX -> a descriptor, ESI = its base, EBP = its length
mv15_desc:
        push    eax
        mov     eax,ebp
        dec     eax
        mov     [bx],ax                 ; limit
        mov     eax,esi
        mov     [bx+2],ax               ; base 15-0
        shr     eax,16
        mov     [bx+4],al               ; base 23-16
        mov     byte ptr [bx+5],93h     ; present, writable data
        mov     word ptr [bx+6],0
        mov     [bx+7],ah               ; base 31-24, which the first BIOS
        pop     eax                     ; to define this ignored
        ret

;=====================================================================
; Data
;=====================================================================
rx_base     dd  0                       ; the pool PM-DOS handed over
rx_end      dd  0
IFDEF XMSPOOLSTAMP
rx_ptbad    dd  0FFFFFFFFh              ; rx_pooltest: the first address that
rx_ptval    dd  0                       ; lied, what it said, and how it was
rx_ptflg    db  0                       ; found - see rdbg_xpt
rx_psn      dd  0                       ; rx_poolscan: pages that lost
rx_psfirst  dd  0                       ; their stamp, and the first and
rx_pslast   dd  0                       ; last of them
rx_pslist   dd  RX_PSLIST dup (0)        ; and where the first of them are
ENDIF
rx_hmaown   db  0                       ; the HMA, when this image is low
rx_a20cnt   dw  0                       ; local enables outstanding
rx_a20meth  db  0                       ; 0 none, 1 the 8042, 2 port 92h
rx_a20want  db  0
rx_mverr    db  0
rx_oldi15   dd  0
rx_msrc     dd  0
rx_rec      dw  0
rx_newkb    dd  0
rx_newaddr  dd  0
rx_oldfoot  dd  0
rx_desc     db  XM_SIZE dup(0)

; The descriptor table the block move switches through.  Two entries:
; the null descriptor the processor insists on, and one that covers
; the whole address space.
rx_gdtr     dw  rx_gdt_end - rx_gdt - 1
            dd  0                       ; its linear address, from rx_init

rx_gdt:
            dd  0, 0                    ; the null descriptor
            dw  0FFFFh                  ; limit 15-0
            dw  0                       ; base 15-0
            dw  9200h                   ; base 23-16, present writable data
            dw  00CFh                   ; limit 19-16, 4K granular, 32-bit
rx_gdt_end:

; And the one INT 15h AH=87h wants, in the BIOS's own layout: null,
; the table's own descriptor, source, destination, and two the BIOS
; fills in for itself.
rx_gdt87    db  48 dup(0)

rx_handles  db  (RX_MAXH * RXH_LEN) dup(0)

rx_tab:
        dw  OFFSET rxf00                ; get version
        dw  OFFSET rxf01                ; request the HMA
        dw  OFFSET rxf02                ; release the HMA
        dw  OFFSET rxf03                ; global enable A20
        dw  OFFSET rxf04                ; global disable A20
        dw  OFFSET rxf05                ; local enable A20
        dw  OFFSET rxf06                ; local disable A20
        dw  OFFSET rxf07                ; query A20
        dw  OFFSET rxf08                ; query free extended memory
        dw  OFFSET rxf09                ; allocate
        dw  OFFSET rxf0A                ; free
        dw  OFFSET rxf0B                ; move
        dw  OFFSET rxf0C                ; lock
        dw  OFFSET rxf0D                ; unlock
        dw  OFFSET rxf0E                ; get handle information
        dw  OFFSET rxf0F                ; reallocate
        dw  OFFSET rxf10                ; request a UMB
        dw  OFFSET rxf11                ; release a UMB
        dw  OFFSET rxf12                ; reallocate a UMB
