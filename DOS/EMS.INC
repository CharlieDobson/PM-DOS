;=====================================================================
; EMS.INC - expanded memory: INT 67h, LIM EMS 4.0
;
; This is EMM386's job, done by the kernel instead of by a driver, and
; the reason is the same one XMS.INC gives for HIMEM: a real EMM386's
; two jobs are to get the machine into V86 mode and to own the page
; tables, and it can do neither here because both were done at boot.
; PM-DOS has been paging since v0.29 and has run its guests under a
; V86 monitor since v0.30.  What is left of EMM386 once those two are
; taken away is the API, and the API is this file.
;
; WHAT EXPANDED MEMORY IS, since it stopped being a thing anyone has
; to know: a 64K PAGE FRAME at some address in upper memory, divided
; into four 16K WINDOWS, each of which can be pointed at any 16K page
; of a pool that may be many megabytes.  A real-mode program addresses
; the frame with an ordinary far pointer and changes what is behind it
; by asking the manager.  In 1985 that meant a card with bank-switch
; registers on it; in 1988 it meant a 386 pretending to be one.
;
; A WINDOW IS FOUR PAGE TABLE ENTRIES AND A CR3 RELOAD.  That is the
; whole of the mechanism, and it is pg_map with a different caller.
; The hard part of every other EMS implementation - persuading the
; hardware that memory it has never decoded is now at D000h - was
; solved here in v0.38 for upper memory and this file spends the same
; solution a second time.
;
; THE FRAME AND THE UMBs ARE COMPETING FOR ONE RESOURCE, and it is
; worth being explicit about that rather than discovering it.  Both
; want 16K pages of C0000h-EFFFFh that nothing decodes; both find them
; with pg_umbprobe, which is a measurement of the hardware and must be
; made exactly once.  So EMS= runs FIRST, takes its 64K out of
; umb_state with umb_reserve, and leaves umb_init to lay its chain
; over what is left.  A frame carved out of the UMB pool afterwards
; would have been worse in two ways: nothing guarantees 64K of it is
; contiguous once programs have been loaded high, and a UMB that has
; been handed to a program is a page this file must never remap.
;
; ONE POOL, THREE DOORS.  The pages behind a handle come out of the
; same arena that 48h and XMS allocate from, for the reason XMS.INC
; states about itself: two doors onto one pool is correct and two
; pools is a bug waiting for whoever opens both.  A handle's pages are
; one CONTIGUOUS arena block, page aligned, so logical page N is base
; plus N times 16K and there is one rounding per handle rather than
; one per page.
;
; HOW A PROGRAM FINDS IT.  It reads the segment of the INT 67h vector
; and looks for the eight characters EMMXXXX0 at offset 0Ah of that
; segment, which is where a character device driver keeps its name.
; So a paragraph of conventional memory is dressed up as one - see
; ems_hdr - and the vector points into it.  That is the detection the
; LIM specification recommends, and the alternative (opening the
; device by name) is served too, through dev_tab.
;
; The header is deliberately NOT linked into the device chain.  A
; program that found it there would be entitled to call its strategy
; and interrupt entries with a request packet, and there is no
; real-mode code behind them to fill one in - so being findable that
; way would be a promise this file cannot keep.  Not being in the
; chain is merely a gap; being in it and answering with an untouched
; status word would be a lie.
;
;---------------------------------------------------------------------
; LIM 4.0, AND WHAT IT ADDED.  v0.41 stopped at 3.2 on the argument
; that a manager claiming 4.0 will be asked for everything 4.0 has,
; by programs that will not take an error for an answer.  So 4.0 is
; here whole - 4Fh to 5Dh, every subfunction - rather than the cheap
; half of it:
;
;   4Fh  a partial page map, saved and restored by segment
;   50h  several windows mapped in one call
;   51h  a handle grown or shrunk, its contents kept
;   52h  handle attributes: volatile, which is all a RAM pool can be
;   53h  a handle's eight-byte name, and 54h, the directory of them
;   55h  map and jump, 56h map and call: an overlay manager's far
;        JMP and far CALL, with the windows changed on the way
;   57h  a region moved or exchanged, up to a megabyte, between any
;        two of conventional memory and the pages of a handle
;   58h  where the windows are; 59h, what the "hardware" is like
;   5Ah  a handle of standard or raw pages, which may hold none
;   5Bh  alternate map register sets - none, and the context save
;        area that replaces them
;   5Ch  warm boot, which has nothing to prepare
;   5Dh  the OS/E functions locked behind an access key
;
; HANDLE 0 IS THE SYSTEM'S, as 4.0 says it is: always open, owning no
; pages unless somebody gives it some, never ended by a program
; exiting, and counted by 4Bh.  Every other handle is the index of its
; row, so handles start at 1 exactly as they did under 3.2.
;
; THE ONE THING 4.0 ALLOWS AND THIS DOES NOT DO is map pages OUTSIDE
; the frame - into conventional memory, so that Windows 3.0 real mode
; or DESQview could give each program a whole bank of its own.  58h
; lists the four windows and nothing else, which the specification
; permits and which is what DOSBox's 4.0 manager has always done.  The
; reason is not the page tables, which would take a conventional page
; as readily as a frame page; it is ata_dmaok, and anything else in the
; kernel that takes a linear address below A0000h to be the physical
; one.  A guest-made hole in that rule is a disk transfer landing in
; the wrong memory.
;
; Included by DOSMAIN.ASM, after VDM.INC: it reads and writes the
; guest's registers out of vdm_ctx, as every other service for a
; 16-bit caller does.
;=====================================================================

; The device header a program finds by reading the vector.  DEVH_LEN
; is 12h - one past the eight-byte name - and that byte is where the
; vector's OFFSET points, so that anything which reflects INT 67h
; rather than trapping it lands on an IRET instead of on the name.
; The byte after it is the HLT that 56h's called routine returns to.
EMSH_IRET       EQU     DEVH_LEN
EMSH_RET        EQU     DEVH_LEN + 1

;---------------------------------------------------------------------
; One row of the handle table.
;
; EH_RAW is what arena_alloc handed back and EH_BASE is that rounded
; up to a page; both are kept because the first is what arena_free
; must be given and the second is what every map call uses.  Freeing
; the rounded one would hand the arena an address that is not the
; start of any block it knows about.  A handle with no pages has no
; block, and both are zero.
;---------------------------------------------------------------------
EH_FLAGS        EQU     0               ; dd - 0 free, 1 allocated
EH_PAGES        EQU     4               ; dd - logical pages it owns
EH_BASE         EQU     8               ; dd - flat, page aligned: logical 0
EH_RAW          EQU     12              ; dd - what arena_alloc handed back
EH_OWN          EQU     16              ; dd - the owner id that took it
EH_SAVED        EQU     20              ; dd - a page map is parked here
EH_SMAP         EQU     24              ; and this is where it is parked
EH_NAME         EQU     EH_SMAP + EMS_MAPLEN    ; 8 bytes, 53h's
EH_LEN          EQU     EH_NAME + 8

EMS_MAXFN       EQU     1Dh             ; 5Dh is the last one 4.0 defines;
                                        ; anything above it gets 84h

;---------------------------------------------------------------------
; 55h's and 56h's structures, as the specification lays them out.  A
; map list is pairs of words: a logical page, then a physical page
; number or a segment, as AL said.
;---------------------------------------------------------------------
EMJ_TARGET      EQU     0               ; dd - where to go, offset first
EMJ_LEN         EQU     4               ; db - entries in the list
EMJ_MAP         EQU     5               ; dd - the list
EMJ_SIZE        EQU     9

EMC_TARGET      EQU     0               ; dd - what to call
EMC_NEWLEN      EQU     4               ; db
EMC_NEWMAP      EQU     5               ; dd - the windows while it runs
EMC_OLDLEN      EQU     9               ; db
EMC_OLDMAP      EQU     0Ah             ; dd - the windows when it returns
EMC_SIZE        EQU     0Eh             ; and eight reserved bytes that
                                        ; this manager does not read

; What 56h leaves on the guest's stack.  The first two words are the
; far return address the called routine's RETF takes, and they lead
; to the HLT at EMSH_RET; everything after them is what that HLT needs
; to finish the call.  The old map is kept as four window states, not
; as the caller's list - see ems_listmap.
ECF_RETIP       EQU     0
ECF_RETCS       EQU     2
ECF_SIG         EQU     4               ; dw - EMS_CALLSIG, or it is not ours
ECF_OLD         EQU     6               ; dd x4 - a state per window
ECF_IP          EQU     ECF_OLD + EMS_WINDOWS * 4       ; where 56h was called
ECF_CS          EQU     ECF_IP + 2
.ERRNZ  ECF_CS + 2 - EMS_CALLSTK, <EMS_CALLSTK does not match 56h's frame>

EMS_CALLSIG     EQU     'MC'

; 57h's descriptor: a length, then the source and the destination as
; seven bytes each.
EMX_LEN         EQU     0               ; dd - bytes, a megabyte at most
EMX_SRC         EQU     4
EMX_DST         EQU     0Bh
EMX_SIZE        EQU     12h
EMXR_TYPE       EQU     0               ; db - 0 conventional, 1 expanded
EMXR_HANDLE     EQU     1               ; dw - expanded only
EMXR_OFF        EQU     3               ; dw - into the segment or the page
EMXR_SEGPG      EQU     5               ; dw - a segment, or a logical page

; ...and what ems_region makes of one side of it.
ERG_FLAT        EQU     0               ; dd - where it is, flat
ERG_TYPE        EQU     4               ; dd - 0 conventional, 1 expanded
ERG_HANDLE      EQU     8               ; dd - expanded: which handle
ERG_FROM        EQU     12              ; dd - expanded: bytes into it
ERG_SIZE        EQU     16

ems_have    dd  0                       ; there is a manager
ems_wanted  dd  0                       ; EMS= asked for one
ems_wantseg dd  0                       ; ...at this segment, 0 = anywhere
ems_frame   dd  0                       ; the page frame, as a segment
ems_flin    dd  0                       ; ...and as a linear address
ems_total   dd  0                       ; pages the manager could ever hand
                                        ; out, measured once at init
ems_hdrseg  dd  0                       ; the EMMXXXX0 paragraph

; What each window is showing: the handle in the low word and the
; logical page in the high one, which makes the live map and the 4Eh
; array the same sixteen bytes and the copy between them a REP MOVSD.
; EMS_UNMAPPED is FFFFFFFFh, and FFFFh is a handle nobody has.
ems_map     dd  EMS_WINDOWS dup (EMS_UNMAPPED)

ems_tmap    dd  EMS_WINDOWS dup (0)     ; a map on its way into the windows
ems_cold    dd  EMS_WINDOWS dup (0)     ; 56h's old map, being built
ems_pwin    dd  EMS_WINDOWS dup (0)     ; 4Fh's windows, in the caller's order
ems_pmap    dd  (EMS_PMAPLEN + 3) / 4 dup (0)   ; 4Fh's array, copied in

ems_how     dd  0                       ; 50h/55h/56h: 0 physical page
                                        ; numbers, 1 segments
ems_osoff   dd  0                       ; 5Dh switched the OS/E functions off
ems_key     dd  0                       ; 5Dh's access key
ems_haskey  dd  0                       ; ...and whether one is out
ems_keyown  dd  0                       ; ...and whose
ems_alt     dd  0                       ; 5Bh's context save area, as the
                                        ; guest's far pointer; 0 = none
ems_altown  dd  0                       ; ...and whose memory that is

ems_csp     dd  0                       ; 56h: the frame's SP
ems_cst     dd  0                       ; ...and the status coming back
ems_xchg    dd  0                       ; 57h: 1 exchange, 0 move
ems_mst     dd  0                       ; ...and the status, if it worked
ems_mx      dd  (EMX_SIZE + 3) / 4 dup (0)      ; 57h's descriptor, copied in
ems_rsrc    dd  ERG_SIZE / 4 dup (0)
ems_rdst    dd  ERG_SIZE / 4 dup (0)

; The handle table is below the arena and not in the file - see
; KBSS_BASE in PMDOS.INC.  Nothing in it has a value until ems_init
; opens handle 0, and dos_entry has zeroed it long before that.
_DOS    ENDS
_KBSS   SEGMENT
.ERRNZ  ($ - kbss_start) AND 3, <ems_htab in _KBSS is not on a dword>
ems_htab    db  EMS_MAXH * EH_LEN dup (?)
_KBSS   ENDS
_DOS    SEGMENT

;=====================================================================
; ems_init - find a page frame, publish the manager, and reserve the
;            frame out of upper memory.  Called from cfg_upper when
;            EMS= asked for one, and BEFORE umb_init.
;
; CF set if there is no frame to be had or no conventional memory to
; put the header in; ems_have says so afterwards.
;
; EMS= IS THE ESCAPE HATCH for exactly the reason DOS=UMB is, and it
; has one more step than DOS=UMB does: the frame is chosen by writing
; to an address and seeing whether anything stores it, so EMS=NO takes
; the guess away entirely and EMS=nnnn replaces it with an answer.
;=====================================================================
ems_init:
        pushad
        cmp     dword ptr [ems_have],0
        jne     ei_already

        call    umb_probe                       ; once, and possibly already
        call    ems_findframe                   ; -> EAX = linear base
        jc      ei_none

        ; THE HEADER FIRST, because it is the half that can fail on a
        ; machine with no conventional memory left, and failing after
        ; the frame is reserved would cost the user 64K of upper memory
        ; to provide a manager that does not exist.
        push    eax
        call    ems_hdr
        pop     eax
        jc      ei_none

        mov     [ems_flin],eax
        shr     eax,4
        mov     [ems_frame],eax

        mov     eax,[ems_flin]
        mov     ecx,EMS_FRAMESZ
        call    umb_reserve                     ; and umb_init never sees it

        call    ems_avail                       ; what the arena could give
        mov     [ems_total],eax                 ; today is what 42h calls the
                                                ; system's total from now on

        ; Handle 0, the system's: open, empty, and owned by the kernel,
        ; so that no program's exit sweep ever reaches it.
        mov     dword ptr [ems_htab+EH_FLAGS],1
        mov     dword ptr [ems_have],1
ei_already:
        popad
        clc
        ret
ei_none:
        popad
        stc
        ret

;=====================================================================
; ems_findframe - EAX = the linear base of 64K of upper memory that
;                 nothing decodes.  CF if there is none.
;
; EMS=nnnn SHORT-CIRCUITS THE WHOLE SEARCH, and deliberately does not
; consult the probe at all.  Choosing a frame automatically means
; measuring the hardware, the measurement can be wrong in both
; directions on a machine nobody here has, and a user who names an
; address has said what they want done.  cfg_ems has already checked
; the only things that are not a matter of opinion - that 64K of frame
; fits between C000h and F000h on a 16K boundary - so there is nothing
; left here to disagree with.  It is the same escape hatch DOS=UMB is,
; one step finer.
;
; Otherwise: HIGHEST FIRST, and 64K aligned.  The alignment is
; convention rather than a rule - the specification asks only for 16K
; - but every EMM ever shipped put its frame at C000h, D000h or E000h,
; and there is software that assumes the frame paragraph looks like
; one of those.  Highest first because the space left over stays in
; one piece: taking the top 64K of the region leaves the rest
; contiguous, where taking a frame out of the middle would split one
; UMB run into two and cost a filler block to describe the gap.
;=====================================================================
ems_findframe:
        push    ecx
        push    edx
        mov     eax,[ems_wantseg]
        or      eax,eax
        jz      ems_search
        shl     eax,4                   ; the user's, as a linear address
        pop     edx
        pop     ecx
        clc
        ret

ems_search:
        mov     eax,UMB_SCAN_HI
eff_down:
        cmp     eax,UMB_SCAN_LO + EMS_FRAMESZ
        jb      eff_none
        sub     eax,EMS_FRAMESZ
        mov     edx,eax
        sub     edx,UMB_SCAN_LO
        shr     edx,12                          ; the first page's index
        mov     ecx,EMS_FRAMEPG
eff_page:
        cmp     byte ptr [umb_state+edx],UMBP_EMPTY
        jne     eff_down                        ; a ROM, or RAM somebody may
        inc     edx                             ; be using: try lower down
        loop    eff_page
        pop     edx
        pop     ecx
        clc
        ret
eff_none:
        pop     edx
        pop     ecx
        stc
        ret

;=====================================================================
; ems_hdr - the paragraphs a program finds when it reads the vector.
;
; A CHARACTER DEVICE HEADER with EMMXXXX0 in its name field, one IRET
; past the end of it for the vector to point at, and one HLT after
; that for 56h.  It is system memory (owner 8) like the XMS callback
; paragraph, so no exit sweep takes it and it lives as long as the
; machine.
;
; The name goes into dev_tab as well, and only here: the slot is FFh
; bytes until this runs, which is a name no path component can be, so
; a machine without EMS cannot be told it has some by opening a file.
;=====================================================================
ems_hdr:
        push    eax
        push    ebx
        push    edi
        mov     ebx,2                           ; 12h of header, an IRET and
        call    lm_sysalloc                     ; a HLT
        jc      ehd_out
        movzx   edi,ax
        mov     [ems_hdrseg],edi
        shl     edi,4

        mov     dword ptr [edi+DEVH_NEXT],-1    ; and not linked in: see the
                                                ; file header for why
        mov     word ptr [edi+DEVH_ATTR],DEV_ATTR_CHAR
        mov     word ptr [edi+DEVH_STRAT],EMSH_IRET
        mov     word ptr [edi+DEVH_INTR],EMSH_IRET
        mov     dword ptr [edi+DEVH_NAME],'XMME'        ; 'EMMX'
        mov     dword ptr [edi+DEVH_NAME+4],'0XXX'      ; 'XXX0'
        mov     byte ptr [edi+EMSH_IRET],0CFh
        mov     byte ptr [edi+EMSH_RET],0F4h

        ; The vector.  vdm_stubs has already pointed 67h at its own
        ; IRET byte, which is what a machine with no EMS should have
        ; there; this replaces it with a segment that has a name in it.
        mov     eax,[ems_hdrseg]
        shl     eax,16
        mov     ax,EMSH_IRET
        mov     dword ptr ds:[67h * 4],eax

        mov     edi,OFFSET ems_devname
        mov     dword ptr [edi],'XMME'
        mov     dword ptr [edi+4],'0XXX'
        clc
ehd_out:
        pop     edi
        pop     ebx
        pop     eax
        ret

;=====================================================================
; ems_avail - EAX = how many 16K pages could be allocated right now.
;
; THE LARGEST FREE BLOCK AND NOT THE TOTAL FREE SPACE, because a
; handle's pages are one contiguous run: what a program can actually
; get is what will fit in one block, and reporting the sum of the
; fragments would be a number no allocation could ever reach.  The
; page of slack every handle pays for its alignment comes off first,
; for the same reason.
;
; Capped at the LIM 4.0 ceiling of 32MB, which is a statement about
; the specification rather than about this machine: the registers
; could carry more, and programs written against 4.0 do not expect to
; be offered it.
;=====================================================================
ems_avail:
        push    edx
        call    arena_avail
        cmp     eax,EMS_PGSIZE + 1000h
        jb      eav_none
        sub     eax,1000h
        shr     eax,14                          ; ...in 16K pages
        cmp     eax,EMS_MAXPAGES
        jbe     eav_out
        mov     eax,EMS_MAXPAGES
eav_out:
        pop     edx
        ret
eav_none:
        xor     eax,eax
        pop     edx
        ret

;=====================================================================
; ems_call - serve one INT 67h out of vdm_ctx.  AH selects, as the
;            specification says, and AH is also the answer: there is
;            no carry flag in this API and every function reports its
;            status in the same register it was asked in.
;
; A subroutine rather than a stretch of vdm_loop, for the reason
; vdm_do21 is one: a DEVICE= driver's INIT makes these calls too, out
; of an excursion that has no program and no box around it, and
; RAMDRIVE.SYS /A is exactly the driver that would.
;=====================================================================
ems_call:
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX+1]
        sub     eax,40h                         ; below 40h wraps, and the
        cmp     eax,EMS_MAXFN                   ; unsigned compare catches it
        ja      ef_e_func
        jmp     dword ptr [ems_tbl+eax*4]

;---------------------------------------------------------------------
; ems_done - AH = the status.  Write it back and return to whoever
;            called ems_call, which is the VM loop or the excursion.
;---------------------------------------------------------------------
ems_done:
        mov     byte ptr [vdm_ctx+V86C_EAX+1],ah
        ret

;--- the shared failures.  One label per code, so that reading a
;--- handler tells you what it decided rather than what it stored.
ef_e_func:      mov     ah,EMSE_BADFUNC
                jmp     ems_done
ef_e_handle:    mov     ah,EMSE_BADHANDLE
                jmp     ems_done
ef_e_ctx:       mov     ah,EMSE_MAPCTX
                jmp     ems_done
ef_e_many:      mov     ah,EMSE_TOOMANY
                jmp     ems_done
ef_e_zero:      mov     ah,EMSE_ZERO
                jmp     ems_done
ef_e_log:       mov     ah,EMSE_BADLOG
                jmp     ems_done
ef_e_phys:      mov     ah,EMSE_BADPHYS
                jmp     ems_done
ef_e_saved:     mov     ah,EMSE_SAVED
                jmp     ems_done
ef_e_nosave:    mov     ah,EMSE_NOTSAVED
                jmp     ems_done
ef_e_sub:       mov     ah,EMSE_BADSUB
                jmp     ems_done
ef_e_attr:      mov     ah,EMSE_BADATTR
                jmp     ems_done
ef_e_nofeat:    mov     ah,EMSE_NOFEAT
                jmp     ems_done
ef_e_toobig:    mov     ah,EMSE_TOOBIG
                jmp     ems_done
ef_e_convexp:   mov     ah,EMSE_CONVEXP
                jmp     ems_done
ef_e_xchgovl:   mov     ah,EMSE_XCHGOVL
                jmp     ems_done
ef_e_noalt:     mov     ah,EMSE_NOALT
                jmp     ems_done
ef_e_noname:    mov     ah,EMSE_NONAME
                jmp     ems_done
ef_e_dup:       mov     ah,EMSE_DUPNAME
                jmp     ems_done
ef_e_corrupt:   mov     ah,EMSE_CORRUPT
                jmp     ems_done
ef_e_denied:    mov     ah,EMSE_DENIED
                jmp     ems_done

; A POINTER THAT REACHES THE KERNEL, and 80h is an invention: the LIM
; specification has no code for it because on a real machine there is
; nothing above the manager to protect.  A guest's ES:DI stops at
; 10FFEFh and PMDOS.SYS starts at 110000h, so the pointer alone is
; harmless - but the pointer PLUS A LENGTH is not, and 4Dh writes up
; to 256 bytes and 57h a megabyte.  That is the same hole v0.37 found
; in the device buffers and buf_ok is the same guard that closed it.
; "The manager has broken" is a fatal answer, which is the right size
; of answer for a program that has just asked us to write into the
; kernel.
ef_e_ptr:       mov     ah,EMSE_SOFT
                jmp     ems_done

;---------------------------------------------------------------------
; The dispatch table.  40h to 5Dh, and the two holes in it are real:
; 49h and 4Ah were "get page map" and "set page map" in EMS 3.0, and
; 3.2 withdrew both.  4.0 left them reserved.
;---------------------------------------------------------------------
ems_tbl dd  ef_40,     ef_41,     ef_42,     ef_43
        dd  ef_44,     ef_45,     ef_46,     ef_47
        dd  ef_48,     ef_e_func, ef_e_func, ef_4B
        dd  ef_4C,     ef_4D,     ef_4E,     ef_4F
        dd  ef_50,     ef_51,     ef_52,     ef_53
        dd  ef_54,     ef_55,     ef_56,     ef_57
        dd  ef_58,     ef_59,     ef_5A,     ef_5B
        dd  ef_5C,     ef_5D

;=====================================================================
; 40h - is the manager there and working?
;=====================================================================
ef_40:
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 41h - BX = the page frame's segment
;=====================================================================
ef_41:
        mov     edx,[ems_frame]
        mov     word ptr [vdm_ctx+V86C_EBX],dx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 42h - BX = pages free, DX = pages the system has
;
; DX is the number measured at init and never moves; BX moves with the
; arena, which is what makes the pair mean anything.  A program that
; asks twice and sees BX fall has watched somebody else allocate.
;=====================================================================
ef_42:
        call    ems_avail
        mov     word ptr [vdm_ctx+V86C_EBX],ax  ; STORED BEFORE AH is touched
        mov     edx,[ems_total]
        mov     word ptr [vdm_ctx+V86C_EDX],dx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 43h - BX pages, please.  DX = the handle they belong to.
;
; NONE IS STILL AN ERROR HERE, in 4.0 as in 3.2.  A handle with no
; pages is legal now, but the way to ask for one is 5Ah, and a 3.2
; program that asks 43h for nothing has made a mistake it would want
; to hear about.
;=====================================================================
ef_43:
        movzx   ecx,word ptr [vdm_ctx+V86C_EBX]
        or      ecx,ecx
        jz      ef_e_zero
        call    ems_alloc                       ; -> EDX, or AH = why not
        jc      ems_done
        mov     word ptr [vdm_ctx+V86C_EDX],dx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 44h - AL = window 0-3, BX = logical page, DX = handle.
;
; BX = FFFFh unmaps the window, which 4.0 made official.
;=====================================================================
ef_44:
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        cmp     eax,EMS_WINDOWS
        jae     ef_e_phys
        mov     ebx,eax                         ; EBX = the window from here
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        movzx   ecx,word ptr [vdm_ctx+V86C_EBX]
        cmp     ecx,0FFFFh
        je      e44_off
        cmp     ecx,[esi+EH_PAGES]
        jae     ef_e_log
        call    ems_window
        mov     ah,EMSE_OK
        jmp     ems_done
e44_off:
        call    ems_unwin
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 45h - give the handle in DX back.
;
; A HANDLE WITH A PARKED PAGE MAP CANNOT BE DEALLOCATED, and 86h is
; what says so.  The rule looks pedantic and is not: a program that
; saved the map and never restored it has left the manager holding
; state on its behalf, and letting the handle go would strand it.
; Real EMM386 refuses in the same place with the same code.
;
; HANDLE 0 GIVES BACK ITS PAGES AND STAYS OPEN, which is the 4.0 rule
; for the system's handle: a program cannot close it, only empty it.
;=====================================================================
ef_45:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        cmp     dword ptr [esi+EH_SAVED],0
        jne     ef_e_ctx
        or      edx,edx
        jz      e45_sys
        call    ems_release
        mov     ah,EMSE_OK
        jmp     ems_done
e45_sys:
        xor     ecx,ecx                         ; down to nothing, which
        call    ems_resize                      ; cannot fail
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 46h - AL = the version, in BCD
;=====================================================================
ef_46:
        mov     byte ptr [vdm_ctx+V86C_EAX],EMS_VERSION
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 47h / 48h - park the page map under a handle, and put it back.
;
; This is how a TSR that pops up over an EMS program borrows the
; windows: save under its own handle, map what it wants, restore.  The
; parked copy lives in the handle's own row, which is why a row is as
; long as it is.
;=====================================================================
ef_47:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        cmp     dword ptr [esi+EH_SAVED],0
        jne     ef_e_saved
        push    esi
        lea     edi,[esi+EH_SMAP]
        mov     esi,OFFSET ems_map
        mov     ecx,EMS_WINDOWS
        cld
        rep     movsd
        pop     esi
        mov     dword ptr [esi+EH_SAVED],1
        mov     ah,EMSE_OK
        jmp     ems_done

ef_48:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        cmp     dword ptr [esi+EH_SAVED],0
        je      ef_e_nosave
        mov     dword ptr [esi+EH_SAVED],0      ; spent whether or not the
        add     esi,EH_SMAP                     ; windows come back: a map
        call    ems_setmap                      ; that cannot be restored is
        jc      ef_e_ctx                        ; not one to try twice
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 4Bh - BX = how many handles are open, the system's among them
;=====================================================================
ef_4B:
        call    ems_count
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 4Ch - BX = how many pages the handle in DX owns
;=====================================================================
ef_4C:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        mov     eax,[esi+EH_PAGES]
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 4Dh - ES:DI gets a handle and a page count for every open handle,
;       BX = how many were written.
;=====================================================================
ef_4D:
        call    ems_esdi                        ; -> EDI, flat
        mov     eax,edi
        mov     ecx,EMS_MAXH * 4                ; the worst case, which is
        call    buf_ok                          ; the only one worth checking
        jc      ef_e_ptr

        xor     ebx,ebx                         ; the row, and the handle
        xor     ecx,ecx                         ; how many written
        mov     esi,OFFSET ems_htab
e4D_one:
        cmp     ebx,EMS_MAXH
        jae     e4D_done
        cmp     dword ptr [esi+EH_FLAGS],0
        je      e4D_next
        mov     [edi],bx
        mov     eax,[esi+EH_PAGES]
        mov     [edi+2],ax
        add     edi,4
        inc     ecx
e4D_next:
        add     esi,EH_LEN
        inc     ebx
        jmp     short e4D_one
e4D_done:
        mov     word ptr [vdm_ctx+V86C_EBX],cx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 4Eh - the whole page map, in and out.  AL selects:
;
;   0  read it into ES:DI
;   1  write it from DS:SI
;   2  both, in that order: the caller gets the map it is replacing
;   3  AL = how many bytes the array is
;
; THE SHAPE OF THE ARRAY IS THE MANAGER'S OWN BUSINESS - the
; specification says only that it is opaque and that subfunction 3
; says how big - which is why 4Eh cannot be used to move a map between
; two different managers, and why ours is simply the live map copied.
;=====================================================================
ef_4E:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,0
        je      e4E_get
        cmp     al,1
        je      e4E_set
        cmp     al,2
        je      e4E_both
        cmp     al,3
        je      e4E_size
        jmp     ef_e_sub

e4E_size:
        mov     byte ptr [vdm_ctx+V86C_EAX],EMS_MAPLEN
        mov     ah,EMSE_OK
        jmp     ems_done
e4E_get:
        call    ems_mapout
        jc      ems_done                        ; AH is already the reason
        mov     ah,EMSE_OK
        jmp     ems_done
e4E_set:
        call    ems_mapin
        jc      ems_done
        mov     ah,EMSE_OK
        jmp     ems_done
e4E_both:
        call    ems_mapout                      ; the old one goes out first:
        jc      ems_done                        ; 4Eh/02 is "hand me what is
        call    ems_mapin                       ; there and then take this"
        jc      ems_done
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 4Fh - part of the page map, named by segment.  AL selects:
;
;   0  DS:SI -> a count and that many window segments; the state of
;      just those windows goes to ES:DI
;   1  DS:SI -> what 0 wrote; those windows go back to it
;   2  BX = how many windows; AL = how many bytes 0 will write
;
; The array is a word count and then, per window, its segment and the
; 4Eh dword for it - so a restore needs nothing but the array, and a
; window that was not saved is not touched.  A partial map is what an
; interrupt handler saves when it only means to borrow one window.
;=====================================================================
ef_4F:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        or      al,al
        jz      e4F_get
        cmp     al,1
        je      e4F_set
        cmp     al,2
        jne     ef_e_sub
        movzx   ecx,word ptr [vdm_ctx+V86C_EBX]
        cmp     ecx,EMS_WINDOWS
        ja      ef_e_phys                       ; more windows than there are
        imul    eax,ecx,EMS_PMAPENT
        add     eax,2
        mov     byte ptr [vdm_ctx+V86C_EAX],al
        mov     ah,EMSE_OK
        jmp     ems_done

e4F_get:
        call    ems_dssi
        mov     eax,esi
        mov     ecx,2
        call    buf_ok
        jc      ef_e_ptr
        movzx   ecx,word ptr [esi]
        cmp     ecx,EMS_WINDOWS
        ja      ef_e_corrupt
        lea     ecx,[ecx*2+2]
        call    buf_ok
        jc      ef_e_ptr
        movzx   ecx,word ptr [esi]

        ; EVERY SEGMENT IS A WINDOW BEFORE ANYTHING IS WRITTEN, and the
        ; windows are kept here rather than read again: ES:DI is allowed
        ; to be the same memory as DS:SI.
        xor     edx,edx
e4F_chk:
        cmp     edx,ecx
        jae     e4F_write
        movzx   eax,word ptr [esi+edx*2+2]
        call    ems_winseg                      ; -> EBX
        jc      ef_e_phys
        mov     [ems_pwin+edx*4],ebx
        inc     edx
        jmp     short e4F_chk
e4F_write:
        call    ems_esdi
        imul    eax,ecx,EMS_PMAPENT
        add     eax,2
        push    ecx
        mov     ecx,eax
        mov     eax,edi
        call    buf_ok
        pop     ecx
        jc      ef_e_ptr
        mov     [edi],cx
        add     edi,2
        xor     edx,edx
e4F_put:
        cmp     edx,ecx
        jae     e4F_ok
        mov     ebx,[ems_pwin+edx*4]
        mov     eax,ebx
        shl     eax,10                          ; 400h paragraphs a window
        add     eax,[ems_frame]
        mov     [edi],ax
        mov     eax,[ems_map+ebx*4]
        mov     [edi+2],eax
        add     edi,EMS_PMAPENT
        inc     edx
        jmp     short e4F_put
e4F_ok:
        mov     ah,EMSE_OK
        jmp     ems_done

; A RESTORE IS COPIED IN AND CHECKED WHOLE BEFORE A WINDOW MOVES, for
; the reason ems_setmap gives: the array may be sitting in one of the
; windows it is about to change.
e4F_set:
        call    ems_dssi
        mov     eax,esi
        mov     ecx,2
        call    buf_ok
        jc      ef_e_ptr
        movzx   ecx,word ptr [esi]
        cmp     ecx,EMS_WINDOWS
        ja      ef_e_corrupt
        imul    ecx,ecx,EMS_PMAPENT
        add     ecx,2
        call    buf_ok
        jc      ef_e_ptr
        mov     edi,OFFSET ems_pmap
        cld
        rep     movsb
        movzx   ecx,word ptr [ems_pmap]

        mov     esi,OFFSET ems_pmap + 2
        xor     edx,edx
e4F_vchk:
        cmp     edx,ecx
        jae     e4F_apply
        movzx   eax,word ptr [esi]
        call    ems_winseg
        jc      ef_e_corrupt
        mov     eax,[esi+2]
        call    ems_entryok
        jc      ef_e_corrupt
        add     esi,EMS_PMAPENT
        inc     edx
        jmp     short e4F_vchk
e4F_apply:
        mov     esi,OFFSET ems_pmap + 2
        xor     edx,edx
e4F_one:
        cmp     edx,ecx
        jae     e4F_ok
        movzx   eax,word ptr [esi]
        call    ems_winseg                      ; -> EBX, checked above
        mov     eax,[esi+2]
        call    ems_mapone
        add     esi,EMS_PMAPENT
        inc     edx
        jmp     short e4F_one

;=====================================================================
; 50h - several windows at once.  AL = 0 if the list names windows by
;       physical page number, 1 if by segment; DX = the handle; CX =
;       how many pairs; DS:SI -> the pairs, logical page first.
;
; A logical page of FFFFh unmaps its window, as in 44h.  Nothing moves
; unless every pair is good - see ems_listmap.
;=====================================================================
ef_50:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,1
        ja      ef_e_sub
        movzx   eax,al
        mov     [ems_how],eax
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        mov     ebp,esi
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        call    ems_dssi
        mov     edi,OFFSET ems_tmap
        call    ems_listmap
        jc      ems_done
        mov     esi,OFFSET ems_tmap
        call    ems_state
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 51h - the handle in DX, made BX pages long.  What it already holds
;       is kept, up to the new length.
;
; The pages stay ONE BLOCK, which is the design and what makes 57h a
; memmove - so growing either takes the free space after the block or
; moves the whole handle somewhere bigger.  See ems_resize.  A window
; showing a page that has gone comes down; one showing a page that
; moved follows it.
;
; BX is left as it came on a refusal, which is DOSBox's answer to a
; question the specification does not settle, and on success it is
; already the new length.
;=====================================================================
ef_51:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        movzx   ecx,word ptr [vdm_ctx+V86C_EBX]
        cmp     ecx,[ems_total]
        ja      ef_e_many
        call    ems_resize
        jc      ems_done
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 52h - handle attributes.  AL selects:
;
;   0  AL = the handle's: 0 volatile, 1 non-volatile
;   1  BL = what it should be
;   2  AL = what this manager can keep: 0, volatile only
;
; NON-VOLATILE MEANT "STILL THERE AFTER A WARM BOOT", which a card with
; its own memory could promise and a pool in the arena cannot: the
; arena is rebuilt from nothing every time the kernel starts.  So
; every handle is volatile, asking for the other kind is 91h, and
; asking for a kind the specification never defined is 90h.
;=====================================================================
ef_52:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,2
        je      e52_cap
        ja      ef_e_sub
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        cmp     al,1
        je      e52_set
        mov     byte ptr [vdm_ctx+V86C_EAX],0
        mov     ah,EMSE_OK
        jmp     ems_done
e52_set:
        mov     al,byte ptr [vdm_ctx+V86C_EBX]
        cmp     al,1
        ja      ef_e_attr
        je      ef_e_nofeat
        mov     ah,EMSE_OK
        jmp     ems_done
e52_cap:
        mov     byte ptr [vdm_ctx+V86C_EAX],0
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 53h - a handle's name.  AL = 0 copies the eight bytes to ES:DI, 1
;       sets them from DS:SI.  DX = the handle.
;
; A NAME BELONGS TO ONE HANDLE, and giving a second handle a name that
; is taken is A1h - except the empty name, eight zero bytes, which is
; what every handle starts with and which any number of them may have.
; Freeing a handle clears its name with the rest of its row.
;=====================================================================
ef_53:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,1
        ja      ef_e_sub
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        cmp     al,1
        je      e53_set
        call    ems_esdi
        mov     eax,edi
        mov     ecx,8
        call    buf_ok
        jc      ef_e_ptr
        mov     eax,[esi+EH_NAME]
        mov     [edi],eax
        mov     eax,[esi+EH_NAME+4]
        mov     [edi+4],eax
        mov     ah,EMSE_OK
        jmp     ems_done
e53_set:
        mov     ebx,esi                         ; EBX = the row being named
        call    ems_dssi
        mov     eax,esi
        mov     ecx,8
        call    buf_ok
        jc      ef_e_ptr
        mov     eax,[esi]
        mov     ecx,[esi+4]
        mov     edi,eax
        or      edi,ecx
        jz      e53_store                       ; no name at all
        call    ems_byname                      ; -> ESI = the row with it
        jc      e53_store
        cmp     esi,ebx
        jne     ef_e_dup                        ; ...and it is another handle
e53_store:
        mov     [ebx+EH_NAME],eax
        mov     [ebx+EH_NAME+4],ecx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 54h - the handle directory.  AL selects:
;
;   0  ES:DI gets ten bytes per open handle, its number and its name;
;      AL = how many
;   1  DS:SI -> a name; DX = the handle that has it
;   2  BX = how many handles the manager has
;
; SEARCHING FOR NO NAME IS A1h, not A0h.  Every unnamed handle has the
; empty name, so "which handle is called nothing" has many answers,
; and the specification gives that question a code of its own.
;=====================================================================
ef_54:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        or      al,al
        jz      e54_dir
        cmp     al,1
        je      e54_find
        cmp     al,2
        jne     ef_e_sub
        mov     word ptr [vdm_ctx+V86C_EBX],EMS_MAXH
        mov     ah,EMSE_OK
        jmp     ems_done

e54_dir:
        call    ems_count
        mov     ebx,eax                         ; BL = AL on the way out
        imul    ecx,eax,10
        call    ems_esdi
        mov     eax,edi
        call    buf_ok
        jc      ef_e_ptr
        xor     edx,edx
        mov     esi,OFFSET ems_htab
e54_one:
        cmp     dword ptr [esi+EH_FLAGS],0
        je      e54_next
        mov     [edi],dx
        mov     eax,[esi+EH_NAME]
        mov     [edi+2],eax
        mov     eax,[esi+EH_NAME+4]
        mov     [edi+6],eax
        add     edi,10
e54_next:
        add     esi,EH_LEN
        inc     edx
        cmp     edx,EMS_MAXH
        jb      e54_one
        mov     byte ptr [vdm_ctx+V86C_EAX],bl
        mov     ah,EMSE_OK
        jmp     ems_done

e54_find:
        call    ems_dssi
        mov     eax,esi
        mov     ecx,8
        call    buf_ok
        jc      ef_e_ptr
        mov     eax,[esi]
        mov     ecx,[esi+4]
        mov     edx,eax
        or      edx,ecx
        jz      ef_e_dup
        call    ems_byname                      ; -> EDX = the handle
        jc      ef_e_noname
        mov     word ptr [vdm_ctx+V86C_EDX],dx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 55h - map some pages and jump.  AL = 0 physical page numbers, 1
;       segments; DX = the handle; DS:SI -> where to go and the list.
;
; AN OVERLAY MANAGER'S FAR JMP: the code it is jumping to is in the
; page it is mapping, so the two have to happen as one call.  This one
; is cheap, because INT 67h is trapped rather than reflected and the
; guest's CS:IP is a pair of fields in vdm_ctx: change them and the
; monitor resumes the guest at the target.  The target sees every
; register and flag the caller had, except AH, which is 0.
;=====================================================================
ef_55:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,1
        ja      ef_e_sub
        movzx   eax,al
        mov     [ems_how],eax
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        mov     ebp,esi
        call    ems_dssi
        mov     eax,esi
        mov     ecx,EMJ_SIZE
        call    buf_ok
        jc      ef_e_ptr
        mov     ebx,[esi+EMJ_TARGET]
        movzx   ecx,byte ptr [esi+EMJ_LEN]
        mov     eax,[esi+EMJ_MAP]
        call    ems_far
        mov     esi,eax
        mov     edi,OFFSET ems_tmap
        call    ems_listmap
        jc      ems_done
        mov     esi,OFFSET ems_tmap
        call    ems_state

        movzx   eax,bx
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        shr     ebx,16
        mov     dword ptr [vdm_ctx+V86C_CS],ebx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 56h - map some pages and call.  AL = 0 or 1 as for 55h, or 2 to ask
;       how much of the caller's stack this takes (BX).  DX = the
;       handle; DS:SI -> the target, the map to call with, and the map
;       to come back to.
;
; THE FAR CALL HALF IS 55h.  The return is the part with something to
; it: when the target's RETF comes back, the old map has to go in
; before the caller sees its next instruction - so the RETF is made to
; land on our HLT at EMSH_RET, and everything that HLT needs to finish
; the job is on the guest's own stack underneath the return address
; (ECF_*).  NOTHING IS KEPT IN THE KERNEL BETWEEN THE CALL AND THE
; RETURN, which is what makes these nest - a target may call 56h
; itself - and what makes a target that never returns cost nothing.
;
; THE OLD MAP IS READ AT THE CALL, NOT AT THE RETURN.  The list the
; caller pointed at may be in a page the new map is about to take
; away, and the specification's own errors for a bad old map are
; errors of the call.  It goes on the stack as four window states; a
; handle the target has freed by the time it returns is 83h then.
;
; The order matters because only the last step changes anything: old
; map checked, room on the stack checked, new map checked and put in,
; and only then the frame written and CS:IP moved.
;=====================================================================
ef_56:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,2
        je      e56_size
        ja      ef_e_sub
        movzx   eax,al
        mov     [ems_how],eax
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        mov     ebp,esi                         ; EBP = the row from here
        call    ems_dssi
        mov     eax,esi
        mov     ecx,EMC_SIZE
        call    buf_ok
        jc      ef_e_ptr
        mov     ebx,esi                         ; EBX = the structure

        movzx   ecx,byte ptr [ebx+EMC_OLDLEN]
        mov     eax,[ebx+EMC_OLDMAP]
        call    ems_far
        mov     esi,eax
        mov     edi,OFFSET ems_cold
        call    ems_listmap
        jc      ems_done

        movzx   eax,word ptr [vdm_ctx+V86C_ESP]
        cmp     eax,EMS_CALLSTK
        jb      ef_e_ptr                        ; it would wrap the segment
        sub     eax,EMS_CALLSTK
        mov     [ems_csp],eax
        mov     ecx,dword ptr [vdm_ctx+V86C_SS]
        shl     ecx,4
        add     eax,ecx
        mov     ecx,EMS_CALLSTK
        call    buf_ok
        jc      ef_e_ptr
        push    eax                             ; the frame, flat

        movzx   ecx,byte ptr [ebx+EMC_NEWLEN]
        mov     eax,[ebx+EMC_NEWMAP]
        call    ems_far
        mov     esi,eax
        mov     edi,OFFSET ems_tmap
        call    ems_listmap
        pop     edi                             ; EDI = the frame
        jc      ems_done
        mov     ebx,[ebx+EMC_TARGET]            ; before the frame can land
                                                ; on the structure
        mov     esi,OFFSET ems_tmap
        call    ems_state

        mov     word ptr [edi+ECF_RETIP],EMSH_RET
        mov     eax,[ems_hdrseg]
        mov     word ptr [edi+ECF_RETCS],ax
        mov     word ptr [edi+ECF_SIG],EMS_CALLSIG
        mov     esi,OFFSET ems_cold
        push    edi
        add     edi,ECF_OLD
        mov     ecx,EMS_WINDOWS
        cld
        rep     movsd
        pop     edi
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        mov     word ptr [edi+ECF_IP],ax
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        mov     word ptr [edi+ECF_CS],ax

        mov     eax,[ems_csp]
        mov     word ptr [vdm_ctx+V86C_ESP],ax
        movzx   eax,bx
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        shr     ebx,16
        mov     dword ptr [vdm_ctx+V86C_CS],ebx
        mov     ah,EMSE_OK
        jmp     ems_done

e56_size:
        mov     word ptr [vdm_ctx+V86C_EBX],EMS_CALLSTK
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; ems_iscb - CF clear if the guest is stopped on the HLT a 56h target
;            returns to.  Everything preserved.
;=====================================================================
ems_iscb:
        push    eax
        cmp     dword ptr [ems_have],0
        je      eic_no
        mov     eax,[ems_hdrseg]
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     eic_no
        cmp     dword ptr [vdm_ctx+V86C_EIP],EMSH_RET
        jne     eic_no
        pop     eax
        clc
        ret
eic_no:
        pop     eax
        stc
        ret

;=====================================================================
; ems_callret - the second half of 56h: the target has returned.  Put
;               the old map back and resume the caller after its INT,
;               with AH saying how that went.
;
; CF set if the stack under the HLT is not one of our frames, which
; means the guest got here some way other than a RETF from a 56h
; target - and then there is no caller to resume and the box is lost.
;=====================================================================
ems_callret:
        movzx   eax,word ptr [vdm_ctx+V86C_ESP]
        sub     eax,ECF_SIG                     ; the RETF took the first
        jb      ecr_bad                         ; four bytes
        cmp     eax,10000h - EMS_CALLSTK
        ja      ecr_bad
        mov     [ems_csp],eax
        mov     ecx,dword ptr [vdm_ctx+V86C_SS]
        shl     ecx,4
        add     eax,ecx
        mov     edi,eax                         ; EDI = the frame, flat
        mov     ecx,EMS_CALLSTK
        call    buf_ok
        jc      ecr_bad
        cmp     word ptr [edi+ECF_SIG],EMS_CALLSIG
        jne     ecr_bad

        mov     dword ptr [ems_cst],EMSE_OK
        xor     ebx,ebx
ecr_one:
        mov     eax,[edi+ECF_OLD+ebx*4]
        cmp     eax,EMS_KEEP
        je      ecr_next
        call    ems_entryok
        jc      ecr_gone
        call    ems_mapone
        jmp     short ecr_next
ecr_gone:
        ; Freed or shrunk while the target ran.  That window is left
        ; as the target had it, and the caller is told which went.
        mov     edx,eax
        and     edx,0FFFFh
        call    ems_hrow
        mov     dword ptr [ems_cst],EMSE_BADHANDLE
        jc      ecr_next
        mov     dword ptr [ems_cst],EMSE_BADLOG
ecr_next:
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      ecr_one

        movzx   eax,word ptr [edi+ECF_IP]
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        movzx   eax,word ptr [edi+ECF_CS]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        mov     eax,[ems_csp]
        add     eax,EMS_CALLSTK
        mov     word ptr [vdm_ctx+V86C_ESP],ax
        mov     al,byte ptr [ems_cst]
        mov     byte ptr [vdm_ctx+V86C_EAX+1],al
        clc
        ret
ecr_bad:
        stc
        ret

;=====================================================================
; 57h - move (AL = 0) or exchange (AL = 1) a region.  DS:SI -> the
;       descriptor: a length, then each side as a type (0
;       conventional, 1 expanded), a handle, an offset and a segment
;       or logical page.
;
; ONE BLOCK PER HANDLE IS WHAT MAKES THIS SHORT.  An expanded region
; that crosses pages is still one run of memory, so each side becomes
; a flat address and a length, and the whole call is a memmove or a
; swap.  Most of what follows is the specification's list of reasons
; to refuse, which are worth getting right because programs test for
; them:
;
;   96h  more than a megabyte
;   98h  a type that is neither
;   A2h  a conventional side that runs past the megabyte
;   83h, 95h, 8Ah, 93h  an expanded side's handle, offset, first page,
;        and last byte
;   94h  a conventional side running through a window that is
;        showing the expanded side, which a flat comparison cannot see
;   97h  an exchange whose two sides overlap
;   92h  a move whose two sides overlap in one handle - done, as a
;        memmove does it, and reported, because the source is now
;        partly gone
;=====================================================================
ef_57:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,1
        ja      ef_e_sub
        movzx   eax,al
        mov     [ems_xchg],eax
        mov     dword ptr [ems_mst],EMSE_OK
        call    ems_dssi
        mov     eax,esi
        mov     ecx,EMX_SIZE
        call    buf_ok
        jc      ef_e_ptr
        mov     edi,OFFSET ems_mx
        cld
        rep     movsb

        mov     ecx,dword ptr [ems_mx+EMX_LEN]
        cmp     ecx,100000h
        ja      ef_e_toobig
        mov     esi,OFFSET ems_mx + EMX_SRC
        mov     edi,OFFSET ems_rsrc
        call    ems_region
        jc      ems_done
        mov     esi,OFFSET ems_mx + EMX_DST
        mov     edi,OFFSET ems_rdst
        call    ems_region
        jc      ems_done

        mov     eax,dword ptr [ems_rsrc+ERG_TYPE]
        cmp     eax,dword ptr [ems_rdst+ERG_TYPE]
        je      e57_same

        ; ONE OF EACH: the conventional side may be a window.
        mov     esi,OFFSET ems_rsrc
        mov     edi,OFFSET ems_rdst
        or      eax,eax
        jz      e57_alias
        xchg    esi,edi
e57_alias:
        call    ems_alias
        jc      ef_e_convexp
        jmp     short e57_go

e57_same:
        or      eax,eax
        jz      e57_conv
        mov     eax,dword ptr [ems_rsrc+ERG_HANDLE]
        cmp     eax,dword ptr [ems_rdst+ERG_HANDLE]
        jne     e57_go                          ; two handles, two blocks
        mov     eax,dword ptr [ems_rsrc+ERG_FROM]
        mov     edx,dword ptr [ems_rdst+ERG_FROM]
        call    ems_overlap
        jnc     e57_go
        cmp     dword ptr [ems_xchg],0
        jne     ef_e_xchgovl
        mov     dword ptr [ems_mst],EMSE_MOVEOVL
        jmp     short e57_go
e57_conv:
        mov     eax,dword ptr [ems_rsrc+ERG_FLAT]
        mov     edx,dword ptr [ems_rdst+ERG_FLAT]
        call    ems_overlap
        jnc     e57_go
        cmp     dword ptr [ems_xchg],0
        jne     ef_e_xchgovl

e57_go:
        mov     esi,dword ptr [ems_rsrc+ERG_FLAT]
        mov     edi,dword ptr [ems_rdst+ERG_FLAT]
        cmp     dword ptr [ems_xchg],0
        jne     e57_swap
        call    ems_copy
        jmp     short e57_out
e57_swap:
        call    ems_swap
e57_out:
        mov     ah,byte ptr [ems_mst]
        jmp     ems_done

;---------------------------------------------------------------------
; ems_region - ESI -> one side of 57h's descriptor, EDI -> the ERG_
;              record to fill, ECX = the length.  CF set: AH = why it
;              is refused.  ECX, ESI and EDI survive.
;---------------------------------------------------------------------
ems_region:
        push    ebx
        push    edx
        push    esi
        movzx   eax,byte ptr [esi+EMXR_TYPE]
        cmp     eax,1
        ja      rgn_type
        mov     [edi+ERG_TYPE],eax
        je      rgn_exp

        movzx   eax,word ptr [esi+EMXR_SEGPG]
        shl     eax,4
        movzx   edx,word ptr [esi+EMXR_OFF]
        add     eax,edx
        mov     [edi+ERG_FLAT],eax
        lea     edx,[eax+ecx]
        cmp     edx,100000h
        ja      rgn_wrap
        call    buf_ok
        jc      rgn_ptr
        jmp     short rgn_ok

rgn_exp:
        movzx   edx,word ptr [esi+EMXR_HANDLE]
        mov     [edi+ERG_HANDLE],edx
        movzx   ebx,word ptr [esi+EMXR_OFF]
        movzx   eax,word ptr [esi+EMXR_SEGPG]
        call    ems_hrow                        ; -> ESI = the row
        jc      rgn_handle
        cmp     ebx,EMS_PGSIZE
        jae     rgn_off
        cmp     eax,[esi+EH_PAGES]
        jae     rgn_log
        shl     eax,14
        add     eax,ebx                         ; bytes into the handle
        mov     [edi+ERG_FROM],eax
        lea     edx,[eax+ecx]
        mov     ebx,[esi+EH_PAGES]
        shl     ebx,14
        cmp     edx,ebx
        ja      rgn_end
        add     eax,[esi+EH_BASE]
        mov     [edi+ERG_FLAT],eax
rgn_ok:
        pop     esi
        pop     edx
        pop     ebx
        clc
        ret

rgn_type:       mov     ah,EMSE_BADTYPE
                jmp     short rgn_fail
rgn_wrap:       mov     ah,EMSE_WRAP
                jmp     short rgn_fail
rgn_ptr:        mov     ah,EMSE_SOFT
                jmp     short rgn_fail
rgn_handle:     mov     ah,EMSE_BADHANDLE
                jmp     short rgn_fail
rgn_off:        mov     ah,EMSE_BADOFF
                jmp     short rgn_fail
rgn_log:        mov     ah,EMSE_BADLOG
                jmp     short rgn_fail
rgn_end:        mov     ah,EMSE_PASTEND
rgn_fail:
        pop     esi
        pop     edx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; ems_alias - ESI -> the conventional side, EDI -> the expanded one,
;             ECX = the length.  CF set if the conventional side runs
;             through a window that is showing any of the expanded
;             side's pages.  Everything preserved.
;
; THE ONE OVERLAP A FLAT COMPARISON CANNOT SEE.  Through a window, D000:0
; and the handle's page are the same memory at two different addresses,
; so a memmove that looked only at the addresses would copy a region
; over itself in the wrong order.  Page granular, which is the most a
; window can say.
;---------------------------------------------------------------------
ems_alias:
        pushad
        or      ecx,ecx
        jz      als_no
        mov     ebp,[esi+ERG_FLAT]
        xor     ebx,ebx
als_one:
        mov     eax,ebx
        shl     eax,14
        add     eax,[ems_flin]                  ; the window's first byte
        lea     edx,[eax+EMS_PGSIZE]
        cmp     ebp,edx
        jae     als_next                        ; the region starts above it
        lea     edx,[ebp+ecx]
        cmp     eax,edx
        jae     als_next                        ; ...or ends below it
        mov     eax,[ems_map+ebx*4]
        cmp     eax,EMS_UNMAPPED
        je      als_next
        movzx   edx,ax
        cmp     edx,[edi+ERG_HANDLE]
        jne     als_next
        shr     eax,16
        shl     eax,14                          ; the page, as bytes into it
        mov     edx,[edi+ERG_FROM]
        add     edx,ecx
        cmp     eax,edx
        jae     als_next                        ; past the expanded side
        add     eax,EMS_PGSIZE
        cmp     [edi+ERG_FROM],eax
        jae     als_next                        ; ...or before it
        popad
        stc
        ret
als_next:
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      als_one
als_no:
        popad
        clc
        ret

;---------------------------------------------------------------------
; ems_overlap - EAX and EDX = where two runs of ECX bytes start.  CF
;               set if they share a byte.  Everything preserved.
;---------------------------------------------------------------------
ems_overlap:
        or      ecx,ecx
        jz      ovl_no
        push    eax
        push    edx
        cmp     eax,edx
        jbe     ovl_ord
        xchg    eax,edx                         ; EAX the lower
ovl_ord:
        sub     edx,eax
        cmp     edx,ecx
        pop     edx
        pop     eax
        jb      ovl_yes
ovl_no:
        clc
        ret
ovl_yes:
        stc
        ret

;---------------------------------------------------------------------
; ems_copy - ECX bytes from ESI to EDI, as a memmove: overlap is safe
;            in either direction.  Everything preserved.
;
; Downwards when the destination starts inside the source, which is
; the only case where a forward copy reads what it has already
; written.  XMS's 0Bh does the same with STD and interrupts on, so
; this is not the first.
;---------------------------------------------------------------------
ems_copy:
        pushad
        cld
        cmp     esi,edi
        jae     cpy_up
        lea     eax,[esi+ecx]
        cmp     edi,eax
        jae     cpy_up
        std
        lea     esi,[esi+ecx-1]
        lea     edi,[edi+ecx-1]
        mov     eax,ecx
        and     ecx,3                           ; the odd bytes at the top
        rep     movsb
        mov     ecx,eax
        shr     ecx,2
        sub     esi,3                           ; and the dwords below them,
        sub     edi,3                           ; from the last one down
        rep     movsd
        cld
        popad
        ret
cpy_up:
        mov     eax,ecx
        shr     ecx,2
        rep     movsd
        mov     ecx,eax
        and     ecx,3
        rep     movsb
        popad
        ret

;---------------------------------------------------------------------
; ems_swap - exchange ECX bytes at ESI with ECX bytes at EDI, which do
;            not overlap.  Everything preserved.
;---------------------------------------------------------------------
ems_swap:
        pushad
        mov     edx,ecx
        shr     ecx,2
        jz      swp_bytes
swp_dw:
        mov     eax,[esi]
        mov     ebx,[edi]
        mov     [edi],eax
        mov     [esi],ebx
        add     esi,4
        add     edi,4
        dec     ecx
        jnz     swp_dw
swp_bytes:
        mov     ecx,edx
        and     ecx,3
        jz      swp_done
swp_b:
        mov     al,[esi]
        mov     bl,[edi]
        mov     [edi],al
        mov     [esi],bl
        inc     esi
        inc     edi
        dec     ecx
        jnz     swp_b
swp_done:
        popad
        ret

;=====================================================================
; 58h - the mappable physical pages.  AL = 0 writes a segment and a
;       page number for each to ES:DI, in address order; both forms
;       answer CX = how many.
;
; Four, and they are the frame - see the file header for why there
; is nothing in conventional memory.
;=====================================================================
ef_58:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        or      al,al
        jz      e58_list
        cmp     al,1
        je      e58_count
        jmp     ef_e_sub
e58_list:
        call    ems_esdi
        mov     eax,edi
        mov     ecx,EMS_WINDOWS * 4
        call    buf_ok
        jc      ef_e_ptr
        mov     eax,[ems_frame]
        xor     ebx,ebx
e58_one:
        mov     [edi],ax
        mov     [edi+2],bx
        add     eax,EMS_PGSIZE / 16
        add     edi,4
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      e58_one
e58_count:
        mov     word ptr [vdm_ctx+V86C_ECX],EMS_WINDOWS
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 59h - what the hardware is like.  AL = 0 writes five words to ES:DI
;       (an OS/E function, so 5Dh can lock it); AL = 1 is BX = raw
;       pages free and DX = raw pages in all.
;
; THE HARDWARE IS PAGE TABLES, so the answers are short: a raw page is
; 16K like a standard one, there are no alternate register sets and
; no DMA register sets, and a context save area is 4Eh's array.
;=====================================================================
ef_59:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,1
        je      e59_raw
        ja      ef_e_sub
        cmp     dword ptr [ems_osoff],0
        jne     ef_e_denied
        call    ems_esdi
        mov     eax,edi
        mov     ecx,10
        call    buf_ok
        jc      ef_e_ptr
        mov     word ptr [edi],EMS_PGSIZE / 16  ; a raw page, in paragraphs
        mov     word ptr [edi+2],0              ; alternate register sets
        mov     word ptr [edi+4],EMS_MAPLEN     ; a context save area
        mov     word ptr [edi+6],0              ; DMA register sets
        mov     word ptr [edi+8],0              ; ...and how DMA works with
        mov     ah,EMSE_OK                      ; them, which is moot
        jmp     ems_done
e59_raw:
        call    ems_avail
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     edx,[ems_total]
        mov     word ptr [vdm_ctx+V86C_EDX],dx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 5Ah - BX pages, standard (AL = 0) or raw (AL = 1), which here are the
;       same 16K.  DX = the handle.  Unlike 43h, BX may be 0.
;=====================================================================
ef_5A:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,1
        ja      ef_e_sub
        movzx   ecx,word ptr [vdm_ctx+V86C_EBX]
        call    ems_alloc
        jc      ems_done
        mov     word ptr [vdm_ctx+V86C_EDX],dx
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 5Bh - alternate map register sets.  An OS/E function, all of it.
;
;   0  BL = the set in use; with set 0, ES:DI = the context save area
;      5Bh/01 named, now holding the current map, or 0:0
;   1  BL = the set to use; with set 0, ES:DI = a context save area to
;      take the map from and to remember, or 0:0 to forget it
;   2  DX = how big a context save area is
;   3  allocate a set, 5 a DMA set: BL = 0, there are none
;   4  free a set, 6 and 7 DMA on a set, 8 free a DMA set: set 0 only
;
; A CARD WITH SPARE REGISTER BANKS COULD SWITCH WHOLE MAPS IN ONE OUT,
; and that is what an alternate set was.  Without them the
; specification falls back to set 0 and a save area in the caller's
; memory, which costs a copy each way and is exactly what a multitasker
; switching programs needs - so that is what is here, and every other
; set number is 9Ch.
;=====================================================================
ef_5B:
        cmp     dword ptr [ems_osoff],0
        jne     ef_e_denied
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        cmp     eax,8
        ja      ef_e_sub
        jmp     dword ptr [e5B_tbl+eax*4]

e5B_tbl dd  e5B_get,   e5B_set,   e5B_size,  e5B_none
        dd  e5B_zero,  e5B_none,  e5B_zero,  e5B_zero
        dd  e5B_zero

e5B_get:
        mov     byte ptr [vdm_ctx+V86C_EBX],0
        mov     eax,[ems_alt]
        mov     word ptr [vdm_ctx+V86C_EDI],ax
        shr     eax,16
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        mov     eax,[ems_alt]
        or      eax,eax
        jz      e5B_ok
        call    ems_far
        mov     edi,eax
        mov     ecx,EMS_MAPLEN
        call    buf_ok
        jc      ef_e_ptr
        mov     esi,OFFSET ems_map
        mov     ecx,EMS_WINDOWS
        cld
        rep     movsd
e5B_ok:
        mov     ah,EMSE_OK
        jmp     ems_done

e5B_set:
        cmp     byte ptr [vdm_ctx+V86C_EBX],0
        jne     ef_e_noalt
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        shl     eax,16
        mov     ax,word ptr [vdm_ctx+V86C_EDI]
        or      eax,eax
        jz      e5B_forget
        mov     ebx,eax
        call    ems_far
        mov     esi,eax
        mov     ecx,EMS_MAPLEN
        call    buf_ok
        jc      ef_e_ptr
        call    ems_setmap
        jc      ef_e_corrupt
        mov     [ems_alt],ebx
        mov     eax,dword ptr [cur_own]
        mov     [ems_altown],eax
        jmp     e5B_ok
e5B_forget:
        mov     [ems_alt],eax
        jmp     e5B_ok

e5B_size:
        mov     word ptr [vdm_ctx+V86C_EDX],EMS_MAPLEN
        jmp     e5B_ok

e5B_none:
        mov     byte ptr [vdm_ctx+V86C_EBX],0
        jmp     e5B_ok

e5B_zero:
        cmp     byte ptr [vdm_ctx+V86C_EBX],0
        jne     ef_e_noalt
        jmp     e5B_ok

;=====================================================================
; 5Ch - get ready for a warm boot.  There is nothing to get ready: the
;       kernel starts the arena again from nothing, and the windows go
;       with it.
;=====================================================================
ef_5C:
        mov     ah,EMSE_OK
        jmp     ems_done

;=====================================================================
; 5Dh - the OS/E functions on (AL = 0) or off (AL = 1), and the key
;       handed back (AL = 2).  BX:CX = the access key.
;
; THE FIRST CALL GETS THE KEY rather than giving one: whoever switches
; the functions first is told a number, and from then on only that
; number switches them again.  2 gives the key back, switches the
; functions on, and leaves the next caller to be told a new one.  It is
; how an operating system kept 59h/00 and 5Bh to itself while it ran
; programs that might call them.
;
; A key whose program has ended goes with it - see ems_relown - since
; nobody is left who could give it back.
;=====================================================================
ef_5D:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,2
        je      e5D_return
        ja      ef_e_sub
        cmp     dword ptr [ems_haskey],0
        je      e5D_first
        call    ems_keyok
        jc      ef_e_denied
        jmp     short e5D_set
e5D_first:
        call    ems_newkey
        mov     [ems_key],eax
        mov     dword ptr [ems_haskey],1
        mov     ecx,dword ptr [cur_own]
        mov     [ems_keyown],ecx
        mov     word ptr [vdm_ctx+V86C_ECX],ax
        shr     eax,16
        mov     word ptr [vdm_ctx+V86C_EBX],ax
e5D_set:
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        mov     [ems_osoff],eax
        mov     ah,EMSE_OK
        jmp     ems_done
e5D_return:
        cmp     dword ptr [ems_haskey],0
        je      e5D_open
        call    ems_keyok
        jc      ef_e_denied
e5D_open:
        xor     eax,eax
        mov     [ems_haskey],eax
        mov     [ems_osoff],eax
        mov     ah,EMSE_OK
        jmp     ems_done

;--- ems_keyok - CF unless the guest's BX:CX is the key.
ems_keyok:
        push    eax
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        shl     eax,16
        mov     ax,word ptr [vdm_ctx+V86C_ECX]
        cmp     eax,[ems_key]
        pop     eax
        jne     eko_no
        clc
        ret
eko_no:
        stc
        ret

;--- ems_newkey - EAX = a key, never 0.  The tick count stirred with
;--- the last key, so that two keys asked for in one tick differ.
ems_newkey:
        mov     eax,dword ptr ds:[BDA_TICKS]
        add     eax,[ems_key]
        imul    eax,eax,41C64E6Dh
        add     eax,3039h
        or      eax,eax
        jnz     enk_out
        inc     eax
enk_out:
        ret

;=====================================================================
; ems_esdi / ems_dssi - a guest's far pointer, flattened.
; ems_far            - EAX = a far pointer as stored in memory,
;                      segment high; EAX = it flattened.
;
; No guard here and none needed here: the guard is buf_ok and it wants
; a length, which only the caller knows.
;=====================================================================
ems_esdi:
        push    eax
        mov     edi,dword ptr [vdm_ctx+V86C_ES]
        shl     edi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EDI]
        add     edi,eax
        pop     eax
        ret

ems_dssi:
        push    eax
        mov     esi,dword ptr [vdm_ctx+V86C_DS]
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_ESI]
        add     esi,eax
        pop     eax
        ret

ems_far:
        push    edx
        movzx   edx,ax
        shr     eax,16
        shl     eax,4
        add     eax,edx
        pop     edx
        ret

;---------------------------------------------------------------------
; ems_mapout - the live map to the guest's ES:DI.
; ems_mapin  - the guest's DS:SI becomes the live map.
;
; CF set means it did not happen, AND AH IS ALREADY THE CODE TO
; REPORT, because there are two ways to fail here and the difference
; matters to the caller: a pointer this manager will not read is 80h
; and a map it will not honour is 86h.  Returning the reason in the
; register that carries it anyway is cheaper than a flag the caller
; has to decode, and it is why both callers can jump straight to
; ems_done.  Every other register survives.
;---------------------------------------------------------------------
ems_mapout:
        pushad
        call    ems_esdi
        mov     eax,edi
        mov     ecx,EMS_MAPLEN
        call    buf_ok
        jc      emp_ptr
        mov     esi,OFFSET ems_map
        mov     ecx,EMS_WINDOWS
        cld
        rep     movsd
        popad
        clc
        ret

ems_mapin:
        pushad
        call    ems_dssi
        mov     eax,esi
        mov     ecx,EMS_MAPLEN
        call    buf_ok
        jc      emp_ptr
        call    ems_setmap
        jc      emp_ctx
        popad
        clc
        ret

emp_ptr:
        popad
        mov     ah,EMSE_SOFT
        stc
        ret
emp_ctx:
        popad
        mov     ah,EMSE_MAPCTX
        stc
        ret

;=====================================================================
; ems_setmap - ESI -> EMS_MAPLEN bytes in the manager's own format.
;              Make the windows say that.  CF if any entry names a
;              handle that is not open or a page that handle has not
;              got, and NOTHING IS CHANGED BEFORE THAT IS KNOWN -
;              because a half-applied page map is a program looking at
;              two different pictures at once, and it would have no
;              way to tell.  Everything preserved.
;
; COPIED IN FIRST, because the guest is allowed to keep its saved map
; in expanded memory: if the array is in window 2 and the map moves
; window 2, reading the rest of it afterwards reads somebody else's
; page.
;=====================================================================
ems_setmap:
        pushad
        mov     edi,OFFSET ems_tmap
        mov     ecx,EMS_WINDOWS
        cld
        rep     movsd

        xor     ebx,ebx
esm_check:
        mov     eax,[ems_tmap+ebx*4]
        call    ems_entryok
        jc      esm_bad
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      esm_check

        xor     ebx,ebx
esm_one:
        mov     eax,[ems_tmap+ebx*4]
        call    ems_mapone
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      esm_one
        popad
        clc
        ret
esm_bad:
        popad
        stc
        ret

;=====================================================================
; ems_listmap - a 4.0 map list made into a state per window.
;
;   ESI -> the list, flat: pairs of words, logical page then physical
;          page number or segment as [ems_how] says
;   ECX =  how many pairs
;   EDX =  the handle, EBP -> its row
;   EDI -> four dwords to fill
;
; Each window comes out as EMS_KEEP if the list does not name it,
; EMS_UNMAPPED if its last pair says FFFFh, or the 4Eh dword for the
; page its last pair names.  CF set: AH = 80h, 8Bh or 8Ah, and the
; four dwords mean nothing.  Everything else preserved.
;
; WHY A STATE AND NOT THE LIST.  Applying pairs one at a time reads the
; list while the windows move, and the list is allowed to be in a
; window - 50h's own pairs can be in the page they are taking away.
; Four windows means the last pair for each one is the only one that
; matters, so the whole list reduces to four answers before any window
; changes, and nothing changes at all if any pair is bad.  56h keeps
; its old map this way too, which is why its stack frame is a fixed
; size however long the caller's list was.
;=====================================================================
ems_listmap:
        pushad
        push    ecx
        push    edi
        mov     eax,EMS_KEEP
        mov     ecx,EMS_WINDOWS
        cld
        rep     stosd
        pop     edi
        pop     ecx

        mov     eax,esi
        push    ecx
        shl     ecx,2
        call    buf_ok
        pop     ecx
        jc      lmp_ptr
lmp_one:
        or      ecx,ecx
        jz      lmp_ok
        movzx   eax,word ptr [esi+2]
        call    ems_phys                        ; -> EBX = the window
        jc      lmp_phys
        movzx   eax,word ptr [esi]
        cmp     eax,0FFFFh
        je      lmp_off
        cmp     eax,[ebp+EH_PAGES]
        jae     lmp_log
        shl     eax,16
        or      eax,edx
        jmp     short lmp_set
lmp_off:
        mov     eax,EMS_UNMAPPED
lmp_set:
        mov     [edi+ebx*4],eax
        add     esi,4
        dec     ecx
        jmp     short lmp_one
lmp_ok:
        popad
        clc
        ret
lmp_ptr:
        popad
        mov     ah,EMSE_SOFT
        stc
        ret
lmp_phys:
        popad
        mov     ah,EMSE_BADPHYS
        stc
        ret
lmp_log:
        popad
        mov     ah,EMSE_BADLOG
        stc
        ret

;---------------------------------------------------------------------
; ems_state - ESI -> four window states that ems_listmap has already
;             checked.  Make the windows say them.  Everything
;             preserved.
;---------------------------------------------------------------------
ems_state:
        push    eax
        push    ebx
        xor     ebx,ebx
est_one:
        mov     eax,[esi+ebx*4]
        cmp     eax,EMS_KEEP
        je      est_next
        call    ems_mapone
est_next:
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      est_one
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; ems_phys   - EAX = a physical page number or a segment, as [ems_how]
;              says.  EBX = its window, CF if it is not one.
; ems_winseg - EAX = a segment.  EBX = the window that starts there,
;              CF if none does.
;
; A SEGMENT INSIDE A WINDOW IS NOT THAT WINDOW.  58h published four
; segments and those are the ones that name windows; D100h is a
; mistake, and answering it with window 0 would hide the mistake.
;---------------------------------------------------------------------
ems_phys:
        cmp     dword ptr [ems_how],0
        jne     ems_winseg
        cmp     eax,EMS_WINDOWS
        jae     eph_bad
        mov     ebx,eax
        clc
        ret
eph_bad:
        stc
        ret

ems_winseg:
        mov     ebx,eax
        sub     ebx,[ems_frame]
        jb      eph_bad
        test    ebx,(EMS_PGSIZE / 16) - 1
        jnz     eph_bad
        shr     ebx,10
        cmp     ebx,EMS_WINDOWS
        jae     eph_bad
        clc
        ret

;---------------------------------------------------------------------
; ems_entryok - EAX = a window's state as the 4Eh array has it.  CF
;               if it names a handle that is not open, or a page that
;               handle has not got.  Unmapped is a state.  Everything
;               preserved.
; ems_mapone  - EBX = a window, EAX = a state ems_entryok has passed.
;               The window shows it.  Everything preserved.
;---------------------------------------------------------------------
ems_entryok:
        cmp     eax,EMS_UNMAPPED
        je      eok_yes
        push    ecx
        push    edx
        push    esi
        movzx   edx,ax
        mov     ecx,eax
        shr     ecx,16
        call    ems_hrow
        jc      eok_no
        cmp     ecx,[esi+EH_PAGES]
        jae     eok_no
        pop     esi
        pop     edx
        pop     ecx
eok_yes:
        clc
        ret
eok_no:
        pop     esi
        pop     edx
        pop     ecx
        stc
        ret

ems_mapone:
        cmp     eax,EMS_UNMAPPED
        je      ems_unwin
        push    ecx
        push    edx
        push    esi
        movzx   edx,ax
        mov     ecx,eax
        shr     ecx,16
        call    ems_hrow
        call    ems_window
        pop     esi
        pop     edx
        pop     ecx
        ret

;=====================================================================
; ems_window - EBX = window 0-3, EDX = handle number, ESI = its row,
;              ECX = logical page.  Point the window at the page.
;              Everything preserved.
;
; FOUR PAGE TABLE ENTRIES AND A CR3 RELOAD, which is the whole of what
; a bank-switching card did with its registers.  The page frame is
; below 1MB and is never handed to hardware, which is the condition
; PAGING.INC states for a hole in the identity map: the ATA driver
; takes the frame by PIO, and a box's 8237 is given the bounce buffer
; instead (VDMA.INC) - which is the job EMM386 needed virtual DMA
; services for.
;=====================================================================
ems_window:
        pushad
        mov     eax,[esi+EH_BASE]
        mov     edi,ecx
        shl     edi,14
        add     eax,edi                         ; the physical 16K
        push    eax
        mov     eax,ebx
        shl     eax,14
        add     eax,[ems_flin]                  ; the linear window
        pop     edx
        mov     ecx,EMS_PGPAGES
        call    dword ptr ds:[IOS_PGMAP]
        popad

        push    eax
        mov     eax,ecx                         ; and the record of it, which
        shl     eax,16                          ; is what 47h and 4Eh copy
        or      eax,edx
        mov     [ems_map+ebx*4],eax
        pop     eax
        ret

;=====================================================================
; ems_unwin - EBX = window 0-3.  Point it back at itself.  Everything
;             preserved.
;
; WHICH IS NOT NOTHING, and is exactly right: the frame was chosen
; because nothing decodes it, so a window pointing at its own address
; is a window with no memory behind it.  Reads come back FFh and
; writes go nowhere, which is what an unmapped EMS window does on a
; machine with a real card in it.
;=====================================================================
ems_unwin:
        pushad
        mov     eax,ebx
        shl     eax,14
        add     eax,[ems_flin]
        mov     edx,eax
        mov     ecx,EMS_PGPAGES
        call    dword ptr ds:[IOS_PGMAP]
        popad
        mov     dword ptr [ems_map+ebx*4],EMS_UNMAPPED
        ret

;---------------------------------------------------------------------
; ems_cutwin - EDX = a handle, ECX = its first page that is going.
;              Every window showing that page or a later one comes
;              down.
; ems_rewin  - EDX = a handle, ESI = its row, whose pages have moved.
;              Every window showing one of them is pointed at where it
;              is now.
;
; Both preserve everything.
;---------------------------------------------------------------------
ems_cutwin:
        push    eax
        push    ebx
        xor     ebx,ebx
ecw_one:
        mov     eax,[ems_map+ebx*4]
        cmp     ax,dx
        jne     ecw_next
        shr     eax,16
        cmp     eax,ecx
        jb      ecw_next
        call    ems_unwin
ecw_next:
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      ecw_one
        pop     ebx
        pop     eax
        ret

ems_rewin:
        push    eax
        push    ebx
        push    ecx
        xor     ebx,ebx
erw_one:
        mov     eax,[ems_map+ebx*4]
        cmp     ax,dx
        jne     erw_next
        shr     eax,16
        mov     ecx,eax
        call    ems_window
erw_next:
        inc     ebx
        cmp     ebx,EMS_WINDOWS
        jb      erw_one
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; ems_alloc - ECX = pages, which may be none.  CF clear: EDX = the new
;             handle.  CF set: AH = why not.
;
; The order of the refusals is the order the specification puts them
; in, and it matters: "more than the system has" and "more than are
; free" are different answers to different questions and a program
; that gets the second one will try again later.
;=====================================================================
ems_alloc:
        cmp     ecx,[ems_total]
        ja      eac_many
        call    ems_avail
        cmp     ecx,eax
        ja      eac_avail
        call    ems_newh                        ; -> EAX = row index, ESI = row
        jc      eac_handles
        push    eax
        or      ecx,ecx
        jz      eac_empty
        mov     ebx,ecx
        shl     ebx,14                          ; the pages, in bytes
        add     ebx,1000h                       ; and room to align the front
        call    arena_alloc
        jc      eac_nomem
        mov     [esi+EH_RAW],eax                ; what arena_free will want
        add     eax,0FFFh
        and     eax,NOT 0FFFh
        mov     [esi+EH_BASE],eax               ; and what every map uses
eac_empty:
        mov     [esi+EH_PAGES],ecx
        mov     eax,dword ptr [cur_own]         ; whose it is, so that the end
        mov     [esi+EH_OWN],eax                ; of one program does not
                                                ; strand another's handles
        mov     dword ptr [esi+EH_FLAGS],1
        pop     edx                             ; the row IS the handle
        clc
        ret
eac_nomem:
        pop     eax                             ; the row was never marked
eac_avail:                                      ; taken, so there is no undo
        mov     ah,EMSE_NOTAVAIL
        stc
        ret
eac_many:
        mov     ah,EMSE_TOOMANY
        stc
        ret
eac_handles:
        mov     ah,EMSE_NOHANDLES
        stc
        ret

;=====================================================================
; ems_resize - ESI = a handle's row, EDX = the handle, ECX = how many
;              pages it should own.  CF set: AH = 88h, and the handle
;              is as it was.
;
; FEWER: the windows onto the pages going come down, and the block
; shrinks where it stands, which arena_resize cannot refuse.  None at
; all gives the block back.
;
; MORE: the block grows where it stands if the arena has free space
; right after it, which is the usual case for a handle grown soon
; after it was made.  Otherwise the whole handle MOVES - a new block,
; the pages copied, the old block freed, and every window that was
; showing one of its pages pointed at the page's new home.  Moving
; rather than chaining a second block is what keeps a handle one run,
; and one run is what makes 57h and 4.0's ceiling cheap.
;
; WHOSE THE MEMORY IS stays the row's, not the caller's: arena_alloc
; stamps a block with whoever is running, and a TSR's handle grown
; while some other program has the machine must not end with it.
;=====================================================================
ems_resize:
        pushad
        mov     [ems_rsh],edx
        mov     eax,[esi+EH_PAGES]
        cmp     ecx,eax
        je      rsz_ok
        jb      rsz_less
        cmp     dword ptr [esi+EH_RAW],0
        je      rsz_new

        mov     ebx,ecx
        shl     ebx,14
        add     ebx,[esi+EH_BASE]
        sub     ebx,[esi+EH_RAW]                ; the block, to its new end
        mov     edx,[esi+EH_RAW]
        call    arena_resize
        jnc     rsz_set                         ; it grew where it stood

        mov     ebx,ecx
        shl     ebx,14
        add     ebx,1000h
        call    arena_alloc
        jc      rsz_nomem
        call    ems_blkown
        mov     edx,eax                         ; the new block, as allocated
        add     eax,0FFFh
        and     eax,NOT 0FFFh                   ; ...and as the windows want it
        push    ecx
        push    esi
        mov     edi,eax
        mov     ecx,[esi+EH_PAGES]
        shl     ecx,12                          ; 4K dwords to a 16K page
        mov     esi,[esi+EH_BASE]
        cld
        rep     movsd
        pop     esi
        pop     ecx
        push    edx
        mov     edx,[esi+EH_RAW]
        call    arena_free
        pop     dword ptr [esi+EH_RAW]
        mov     [esi+EH_BASE],eax
        mov     [esi+EH_PAGES],ecx
        mov     edx,[ems_rsh]
        call    ems_rewin
        jmp     short rsz_ok

rsz_new:
        mov     ebx,ecx
        shl     ebx,14
        add     ebx,1000h
        call    arena_alloc
        jc      rsz_nomem
        call    ems_blkown
        mov     [esi+EH_RAW],eax
        add     eax,0FFFh
        and     eax,NOT 0FFFh
        mov     [esi+EH_BASE],eax
rsz_set:
        mov     [esi+EH_PAGES],ecx
rsz_ok:
        popad
        clc
        ret
rsz_nomem:
        popad
        mov     ah,EMSE_NOTAVAIL
        stc
        ret

rsz_less:
        mov     edx,[ems_rsh]
        call    ems_cutwin
        or      ecx,ecx
        jz      rsz_none
        mov     ebx,ecx
        shl     ebx,14
        add     ebx,[esi+EH_BASE]
        sub     ebx,[esi+EH_RAW]
        mov     edx,[esi+EH_RAW]
        call    arena_resize
        jmp     short rsz_set
rsz_none:
        mov     edx,[esi+EH_RAW]
        call    arena_free
        xor     eax,eax
        mov     [esi+EH_RAW],eax
        mov     [esi+EH_BASE],eax
        jmp     short rsz_set

ems_rsh     dd  0                       ; the handle, across the arena calls

;--- ems_blkown - EAX = a block arena_alloc has just handed back, ESI =
;--- the row it is for.  The block's owner becomes the row's.
ems_blkown:
        push    edx
        mov     edx,[esi+EH_OWN]
        mov     [eax-MCB_LEN+MCB_OWNER],edx
        pop     edx
        ret

;=====================================================================
; ems_hrow - EDX = a handle number as a program sees it.  ESI = its
;            row, CF if there is no such handle.  Everything else
;            preserved.
;=====================================================================
ems_hrow:
        cmp     edx,EMS_MAXH
        jae     ehr_bad
        push    eax
        imul    eax,edx,EH_LEN
        add     eax,OFFSET ems_htab
        mov     esi,eax
        pop     eax
        cmp     dword ptr [esi+EH_FLAGS],0
        je      ehr_bad
        clc
        ret
ehr_bad:
        stc
        ret

;=====================================================================
; ems_newh - CF clear: EAX = a free row's index, ESI = the row.  The
;            row is NOT marked taken, so a caller that fails after
;            this has nothing to undo.  Row 0 is never free.
;=====================================================================
ems_newh:
        mov     eax,1
        mov     esi,OFFSET ems_htab + EH_LEN
enh_one:
        cmp     eax,EMS_MAXH
        jae     enh_none
        cmp     dword ptr [esi+EH_FLAGS],0
        je      enh_got
        add     esi,EH_LEN
        inc     eax
        jmp     short enh_one
enh_got:
        clc
        ret
enh_none:
        stc
        ret

;=====================================================================
; ems_count - EAX = how many handles are open.
;=====================================================================
ems_count:
        push    ecx
        push    esi
        xor     eax,eax
        xor     ecx,ecx
        mov     esi,OFFSET ems_htab
ecn_one:
        cmp     ecx,EMS_MAXH
        jae     ecn_out
        cmp     dword ptr [esi+EH_FLAGS],0
        je      ecn_next
        inc     eax
ecn_next:
        add     esi,EH_LEN
        inc     ecx
        jmp     short ecn_one
ecn_out:
        pop     esi
        pop     ecx
        ret

;---------------------------------------------------------------------
; ems_byname - EAX and ECX = eight bytes of name.  CF clear: ESI = the
;              open row that has it, EDX = its handle.  EAX and ECX
;              preserved.
;---------------------------------------------------------------------
ems_byname:
        xor     edx,edx
        mov     esi,OFFSET ems_htab
ebn_one:
        cmp     dword ptr [esi+EH_FLAGS],0
        je      ebn_next
        cmp     [esi+EH_NAME],eax
        jne     ebn_next
        cmp     [esi+EH_NAME+4],ecx
        je      ebn_got
ebn_next:
        add     esi,EH_LEN
        inc     edx
        cmp     edx,EMS_MAXH
        jb      ebn_one
        stc
        ret
ebn_got:
        clc
        ret

;=====================================================================
; ems_release - EDX = handle, ESI = its row.  Windows down, memory
;               back to the arena, row clear.
;
; ems_forget is the same without the memory, and it exists because the
; owner sweep runs AFTER arena_freeown has already taken every block
; that program owned: freeing it a second time would hand the arena an
; address that is now inside somebody else's block.
;=====================================================================
ems_release:
        push    edx
        mov     edx,[esi+EH_RAW]
        or      edx,edx
        jz      erel_none
        call    arena_free
erel_none:
        pop     edx
        ; and on into ems_forget

ems_forget:
        pushad
        xor     ebx,ebx
efg_win:
        cmp     ebx,EMS_WINDOWS
        jae     efg_clear
        mov     eax,[ems_map+ebx*4]
        cmp     ax,dx                           ; the handle is the low word
        jne     efg_next
        call    ems_unwin
efg_next:
        inc     ebx
        jmp     short efg_win
efg_clear:
        mov     edi,esi
        xor     eax,eax
        mov     ecx,EH_LEN / 4
        cld
        rep     stosd
        popad
        ret

;=====================================================================
; ems_relown - the handles belonging to the program that just ended.
;
; BY OWNER, for the reason xms_relown gives: a box holds a program and
; its children, and a TSR outlives the box entirely - so its handles
; have to outlive it too.  Called from vdm_cleanup, immediately after
; arena_freeown and xms_relown, and the order is what makes ems_forget
; the right half of ems_release to use.  Row 0 is the system's and is
; never swept.
;
; TWO MORE THINGS A PROGRAM CAN LEAVE WITH THE MANAGER: 5Bh's context
; save area, which is a pointer into the program's own memory and
; would have 5Bh/00 writing into whoever is loaded there next, and
; 5Dh's key, which nobody is left to give back.
;=====================================================================
ems_relown:
        pushad
        cmp     dword ptr [ems_have],0
        je      erl_out
        mov     edi,dword ptr [cur_own]
        mov     ebx,1
erl_one:
        cmp     ebx,EMS_MAXH
        jae     erl_more
        mov     esi,ebx
        imul    esi,esi,EH_LEN
        add     esi,OFFSET ems_htab
        cmp     dword ptr [esi+EH_FLAGS],0
        je      erl_next
        cmp     [esi+EH_OWN],edi
        jne     erl_next
        mov     edx,ebx                         ; the handle number
        call    ems_forget
erl_next:
        inc     ebx
        jmp     short erl_one
erl_more:
        cmp     dword ptr [ems_alt],0
        je      erl_key
        cmp     [ems_altown],edi
        jne     erl_key
        mov     dword ptr [ems_alt],0
erl_key:
        cmp     dword ptr [ems_haskey],0
        je      erl_out
        cmp     [ems_keyown],edi
        jne     erl_out
        xor     eax,eax
        mov     [ems_haskey],eax
        mov     [ems_osoff],eax
erl_out:
        popad
        ret
