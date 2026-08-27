;=====================================================================
; EMS.INC - expanded memory: INT 67h, LIM EMS 3.2
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
; WHAT IS DELIBERATELY NOT HERE: LIM 4.0.  46h answers 32h and means
; it.  4.0's additions are mostly about mapping pages OUTSIDE the
; frame - anywhere in conventional memory, on demand, so that Windows
; and DESQview could give a whole DOS box its own bank - and a manager
; that claims 4.0 will be asked for 5Ah, 57h and 58h by programs that
; will not take an error for an answer.  Claiming what is implemented
; is the honest version and every 3.2 program works against it.
;
; Included by DOSMAIN.ASM, after VDM.INC: it reads and writes the
; guest's registers out of vdm_ctx, as every other service for a
; 16-bit caller does.
;=====================================================================

; The device header a program finds by reading the vector.  DEVH_LEN
; is 12h - one past the eight-byte name - and that byte is where the
; vector's OFFSET points, so that anything which reflects INT 67h
; rather than trapping it lands on an IRET instead of on the name.
EMSH_IRET       EQU     DEVH_LEN

;---------------------------------------------------------------------
; One row of the handle table.
;
; EH_RAW is what arena_alloc handed back and EH_BASE is that rounded
; up to a page; both are kept because the first is what arena_free
; must be given and the second is what every map call uses.  Freeing
; the rounded one would hand the arena an address that is not the
; start of any block it knows about.
;---------------------------------------------------------------------
EH_FLAGS        EQU     0               ; dd - 0 free, 1 allocated
EH_PAGES        EQU     4               ; dd - logical pages it owns
EH_BASE         EQU     8               ; dd - flat, page aligned: logical 0
EH_RAW          EQU     12              ; dd - what arena_alloc handed back
EH_OWN          EQU     16              ; dd - the owner id that took it
EH_SAVED        EQU     20              ; dd - a page map is parked here
EH_SMAP         EQU     24              ; and this is where it is parked
EH_LEN          EQU     EH_SMAP + EMS_MAPLEN

EMS_MAXFN       EQU     0Eh             ; 4Eh is the last one 3.2 defines;
                                        ; anything above it gets 84h

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

ems_htab    db  EMS_MAXH * EH_LEN dup (0)

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
; ems_hdr - the paragraph a program finds when it reads the vector.
;
; A CHARACTER DEVICE HEADER with EMMXXXX0 in its name field, and one
; IRET past the end of it for the vector to point at.  It is system
; memory (owner 8) like the XMS callback paragraph, so no exit sweep
; takes it and it lives as long as the machine.
;
; The name goes into dev_tab as well, and only here: the slot is FFh
; bytes until this runs, which is a name no path component can be, so
; a machine without EMS cannot be told it has some by opening a file.
;=====================================================================
ems_hdr:
        push    eax
        push    ebx
        push    edi
        mov     ebx,2                           ; 12h of header and an IRET
        call    lm_sysalloc
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
; Capped at the LIM 3.2 ceiling of 8MB, which is a statement about the
; specification rather than about this machine: page counts travel in
; 16-bit registers and 512 pages is what the API can describe.
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
ef_e_handles:   mov     ah,EMSE_NOHANDLES
                jmp     ems_done
ef_e_ctx:       mov     ah,EMSE_MAPCTX
                jmp     ems_done
ef_e_many:      mov     ah,EMSE_TOOMANY
                jmp     ems_done
ef_e_avail:     mov     ah,EMSE_NOTAVAIL
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

; A POINTER THAT REACHES THE KERNEL, and 80h is an invention: the LIM
; specification has no code for it because on a real machine there is
; nothing above the manager to protect.  A guest's ES:DI stops at
; 10FFEFh and PMDOS.SYS starts at 110000h, so the pointer alone is
; harmless - but the pointer PLUS A LENGTH is not, and 4Dh writes up
; to 128 bytes.  That is the same hole v0.37 found in the device
; buffers and buf_ok is the same guard that closed it.  "The manager
; has broken" is a fatal answer, which is the right size of answer for
; a program that has just asked us to write into the kernel.
ef_e_ptr:       mov     ah,EMSE_SOFT
                jmp     ems_done

;---------------------------------------------------------------------
; The dispatch table.  40h to 4Eh, and the two holes in the middle are
; real: 49h and 4Ah were "get page map" and "set page map" in EMS 3.0
; and the 3.2 specification withdrew both.
;---------------------------------------------------------------------
ems_tbl dd  ef_40,     ef_41,     ef_42,     ef_43
        dd  ef_44,     ef_45,     ef_46,     ef_47
        dd  ef_48,     ef_e_func, ef_e_func, ef_4B
        dd  ef_4C,     ef_4D,     ef_4E

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
; The order of the refusals is the order the specification puts them
; in, and it matters: "more than the system has" and "more than are
; free" are different answers to different questions and a program
; that gets the second one will try again later.
;=====================================================================
ef_43:
        movzx   ecx,word ptr [vdm_ctx+V86C_EBX]
        or      ecx,ecx
        jz      ef_e_zero
        cmp     ecx,[ems_total]
        ja      ef_e_many
        call    ems_avail
        cmp     ecx,eax
        ja      ef_e_avail
        call    ems_newh                        ; -> EAX = row index, ESI = row
        jc      ef_e_handles

        push    eax
        push    ecx
        mov     ebx,ecx
        shl     ebx,14                          ; the pages, in bytes
        add     ebx,1000h                       ; and room to align the front
        call    arena_alloc
        pop     ecx
        jc      e43_nomem
        mov     [esi+EH_RAW],eax                ; what arena_free will want
        add     eax,0FFFh
        and     eax,NOT 0FFFh
        mov     [esi+EH_BASE],eax               ; and what every map uses
        mov     [esi+EH_PAGES],ecx
        mov     eax,dword ptr [cur_own]         ; whose it is, so that the end
        mov     [esi+EH_OWN],eax                ; of one program does not
                                                ; strand another's handles
        mov     dword ptr [esi+EH_FLAGS],1
        pop     eax
        inc     eax                             ; handles start at one: zero
        mov     word ptr [vdm_ctx+V86C_EDX],ax  ; is the system's in 4.0 and
        mov     ah,EMSE_OK                      ; is "no handle" everywhere
        jmp     ems_done
e43_nomem:
        pop     eax                             ; the row was never marked
        jmp     ef_e_avail                      ; taken, so there is no undo

;=====================================================================
; 44h - AL = window 0-3, BX = logical page, DX = handle.
;
; BX = FFFFh unmaps the window, which is a 4.0 spelling this manager
; honours anyway: it costs one comparison, a 3.2 program cannot ask
; for it by accident (no handle has 65535 pages), and without it there
; is no way at all to take a window down again.
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
;=====================================================================
ef_45:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    ems_hrow
        jc      ef_e_handle
        cmp     dword ptr [esi+EH_SAVED],0
        jne     ef_e_ctx
        call    ems_release
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
; parked copy lives in the handle's own row, which is why a row is 40
; bytes rather than 24.
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
; 4Bh - BX = how many handles are open
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

        xor     ebx,ebx                         ; the row
        xor     ecx,ecx                         ; how many written
        mov     esi,OFFSET ems_htab
e4D_one:
        cmp     ebx,EMS_MAXH
        jae     e4D_done
        cmp     dword ptr [esi+EH_FLAGS],0
        je      e4D_next
        mov     eax,ebx
        inc     eax
        mov     [edi],ax
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
; ems_esdi / ems_dssi - a guest's far pointer, flattened.
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
;              way to tell.
;=====================================================================
ems_setmap:
        pushad
        xor     ebx,ebx                         ; pass one: is it all legal?
esm_check:
        cmp     ebx,EMS_WINDOWS
        jae     esm_apply
        mov     eax,[esi+ebx*4]
        cmp     eax,EMS_UNMAPPED
        je      esm_cnext
        movzx   edx,ax
        mov     ecx,eax
        shr     ecx,16
        push    esi
        call    ems_hrow
        jc      esm_cbad
        cmp     ecx,[esi+EH_PAGES]
        pop     esi
        jae     esm_bad
esm_cnext:
        inc     ebx
        jmp     short esm_check
esm_cbad:
        pop     esi
        jmp     short esm_bad

esm_apply:
        xor     ebx,ebx
esm_one:
        cmp     ebx,EMS_WINDOWS
        jae     esm_ok
        mov     eax,[esi+ebx*4]
        cmp     eax,EMS_UNMAPPED
        je      esm_off
        movzx   edx,ax
        mov     ecx,eax
        shr     ecx,16
        push    esi
        call    ems_hrow                        ; checked above; ESI is the
        call    ems_window                      ; row from here to the pop
        pop     esi
        jmp     short esm_next
esm_off:
        call    ems_unwin
esm_next:
        inc     ebx
        jmp     short esm_one
esm_bad:
        popad
        stc
        ret
esm_ok:
        popad
        clc
        ret

;=====================================================================
; ems_window - EBX = window 0-3, EDX = handle number, ESI = its row,
;              ECX = logical page.  Point the window at the page.
;
; FOUR PAGE TABLE ENTRIES AND A CR3 RELOAD, which is the whole of what
; a bank-switching card did with its registers.  The page frame is
; below 1MB and is never handed to hardware, which is the condition
; PAGING.INC states for a hole in the identity map: the ATA driver is
; polled PIO, the floppy path stages through XFER_BUF, and rm_int
; passes registers and that same buffer.  A driver loaded high that
; programs the DMA controller to write into the page frame is the case
; this does not survive - and it is the case EMM386 needed virtual DMA
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
; ems_unwin - EBX = window 0-3.  Point it back at itself.
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

;=====================================================================
; ems_hrow - EDX = a handle number as a program sees it.  ESI = its
;            row, CF if there is no such handle.
;=====================================================================
ems_hrow:
        push    eax
        mov     eax,edx
        dec     eax                             ; handle 1 is row 0, so that
        cmp     eax,EMS_MAXH                    ; handle 0 - which programs
        jae     ehr_bad                         ; use to mean "none" - never
        imul    eax,EH_LEN                      ; belongs to anybody
        add     eax,OFFSET ems_htab
        mov     esi,eax
        cmp     dword ptr [esi+EH_FLAGS],0
        je      ehr_bad
        pop     eax
        clc
        ret
ehr_bad:
        pop     eax
        stc
        ret

;=====================================================================
; ems_newh - CF clear: EAX = a free row's index, ESI = the row.  The
;            row is NOT marked taken, so a caller that fails after
;            this has nothing to undo.
;=====================================================================
ems_newh:
        xor     eax,eax
        mov     esi,OFFSET ems_htab
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
; the right half of ems_release to use.
;=====================================================================
ems_relown:
        pushad
        cmp     dword ptr [ems_have],0
        je      erl_out
        mov     edi,dword ptr [cur_own]
        xor     ebx,ebx
erl_one:
        cmp     ebx,EMS_MAXH
        jae     erl_out
        mov     esi,ebx
        imul    esi,EH_LEN
        add     esi,OFFSET ems_htab
        cmp     dword ptr [esi+EH_FLAGS],0
        je      erl_next
        cmp     [esi+EH_OWN],edi
        jne     erl_next
        mov     edx,ebx
        inc     edx                             ; the handle number
        call    ems_forget
erl_next:
        inc     ebx
        jmp     short erl_one
erl_out:
        popad
        ret
