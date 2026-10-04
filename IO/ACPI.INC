;=====================================================================
; ACPI.INC - switching the machine off
;
; A MACHINE WITH AN ATX SUPPLY CAN SWITCH ITSELF OFF, AND ACPI IS HOW
; IT IS ASKED.  The firmware leaves tables in memory: a pointer, the
; RSDP, in the BIOS's own area; a list of tables it points at; and on
; that list the FADT, which says which ports the power management
; registers answer at, and the DSDT, the firmware's description of the
; machine in its own bytecode, AML.  Switching off is two numbers out
; of the DSDT written into one of those registers:
;
;   - \_S5_ is a package holding two sleep types, one for each of the
;     PM1a and PM1b control registers.  S5 is soft-off.
;   - each type goes into bits 10-12 (SLP_TYP) of its register, and
;     writing bit 13 (SLP_EN) with it enters that state.
;
; THE DSDT IS SEARCHED, NOT RUN.  Running AML takes an interpreter as
; big as this kernel.  \_S5_ is a named constant in practically every
; DSDT there is - Name (_S5_, Package () {...}) - and a constant is a
; fixed run of bytes that can be found and read: NameOp, the name,
; PackageOp, a length, a count and the integers.  A firmware that
; works it out in a Method has no such run, and ACPF_FIND then says
; there is no way to switch this machine off.  The SSDTs are searched
; after the DSDT, for a firmware that keeps it in one of those.
;
; ACPI MODE FIRST.  A firmware that boots in legacy mode does its own
; power management through SMIs and leaves SCI_EN clear; the FADT's
; ACPI_ENABLE byte written to its SMI_CMD port hands the registers
; over.  Linux does that before anything else, and this does it just
; before the write - and gives them back through ACPI_DISABLE if the
; machine is still running afterwards, so a failed attempt leaves the
; firmware in the mode it was found in.  A firmware that never sets
; SCI_EN is written to anyway: on most chipsets the control register
; works in either mode.
;
; INTERRUPTS ARE OFF FROM THE MODE CHANGE TO THE END.  In ACPI mode
; the chipset raises the SCI for its own events, and nothing here
; answers it.  The clock does not tick either, so the waits are timed
; by ACPI's power management timer, 3.579545 MHz, which every FADT
; names - and by reads of port 80h in one that does not.
;
; THE TABLES CAN BE ANYWHERE BELOW 4GB, and usually sit just above
; the memory E820 calls usable: past GV_MEMTOP, so possibly in a
; directory entry nothing has mapped.  Each one is asked for through
; pg_mapphys before it is read.  ACPI 2.0 points at tables with 64
; bits, and a table above 4GB is taken to be not there.
;
; Included by IOPM.ASM.
;=====================================================================

; Signatures, as the dwords they are in memory.
RSDP_SIG1       EQU     20445352h       ; 'RSD '
RSDP_SIG2       EQU     20525450h       ; 'PTR '
RSDT_SIG        EQU     54445352h       ; 'RSDT'
XSDT_SIG        EQU     54445358h       ; 'XSDT'
FADT_SIG        EQU     50434146h       ; 'FACP'
DSDT_SIG        EQU     54445344h       ; 'DSDT'
SSDT_SIG        EQU     54445353h       ; 'SSDT'
S5_NAME         EQU     5F35535Fh       ; '_S5_'

; The Root System Description Pointer: on a 16-byte boundary, in the
; first kilobyte of the EBDA or anywhere from E0000h to FFFFFh.
RSDP_REV        EQU     0Fh             ; db - 0 for ACPI 1.0, 2 from 2.0
RSDP_RSDT       EQU     10h             ; dd - the RSDT: 32-bit pointers
RSDP_XSDT       EQU     18h             ; dq - the XSDT: 64-bit ones
RSDP_V1LEN      EQU     20              ; what the checksum covers in 1.0

; Every table starts with the same 36 bytes.
SDT_SIGN        EQU     00h             ; 4 bytes
SDT_LEN         EQU     04h             ; dd - the whole table
SDT_HDRLEN      EQU     36
SDT_MAX         EQU     100000h         ; longer than this is not a table

; The Fixed ACPI Description Table, as much of it as is read here.
FADT_DSDT       EQU     40              ; dd
FADT_SMICMD     EQU     48              ; dd - a port; 0 = always in ACPI mode
FADT_ACPION     EQU     52              ; db - written there to enter it
FADT_ACPIOFF    EQU     53              ; db - and to leave it
FADT_PM1AEVT    EQU     56              ; dd - ports, 0 = none
FADT_PM1BEVT    EQU     60
FADT_PM1ACNT    EQU     64
FADT_PM1BCNT    EQU     68
FADT_PMTMR      EQU     76
FADT_FLAGS      EQU     112             ; dd
FADT_XDSDT      EQU     140             ; dq - ACPI 2.0 on
FADT_XPM1AEVT   EQU     148             ; a GAS each
FADT_XPM1BEVT   EQU     160
FADT_XPM1ACNT   EQU     172
FADT_XPM1BCNT   EQU     184
FADT_XPMTMR     EQU     208

FADTF_TMR32     EQU     100h            ; TMR_VAL_EXT: 32 timer bits, not 24

; A Generic Address Structure, which is how ACPI 2.0 names a register.
GAS_SPACE       EQU     0               ; db - 1 = I/O ports
GAS_ADDR        EQU     4               ; dq
GAS_LEN         EQU     12

; The PM1 registers.  Status is the first half of an event block.
PM1_SCIEN       EQU     0001h           ; control: ACPI mode
PM1_SLPTYP      EQU     1C00h           ; control: the state to enter
PM1_SLPEN       EQU     2000h           ; control: ...enter it now
PM1_KEEP        EQU     0C3FFh          ; control: everything else
PM1_STSALL      EQU     8731h           ; status: every fixed event - the
                                        ; timer, bus master, global lock,
                                        ; both buttons, the RTC alarm and
                                        ; wake - cleared by writing a 1

; The AML that \_S5_ is made of.
AML_NAMEOP      EQU     08h
AML_ROOT        EQU     5Ch             ; '\'
AML_PACKAGE     EQU     12h
AML_ZERO        EQU     00h
AML_ONE         EQU     01h
AML_BYTE        EQU     0Ah
AML_WORD        EQU     0Bh
AML_DWORD       EQU     0Ch
AML_ONES        EQU     0FFh

; How long the firmware is given: to enter ACPI mode, and to switch
; the power off once told to.
PWR_WAIT        EQU     3 * 3579545     ; three seconds of the PM timer
PWR_SPINS       EQU     3000000         ; ...or of port 80h reads

;=====================================================================
; acpi_svc - IOS_ACPI.  AL = an ACPF_* function.
;=====================================================================
acpi_svc:
        cmp     al,ACPF_FIND
        je      acpi_find
        cmp     al,ACPF_OFF
        je      acpi_off
        cmp     al,ACPF_REPORT
        je      acpi_report
        stc
        ret

;=====================================================================
; acpi_find - ACPF_FIND.  CF clear if the power can be switched off,
;             with the registers and the sleep types in pwv_*.  CF and
;             AL = ACPE_NONE or ACPE_NOOFF if it cannot.  Everything
;             else preserved.
;=====================================================================
acpi_find:
        pushad
        xor     eax,eax                         ; nothing found yet, for
        mov     [pwv_rsdp],eax                  ; acpi_report to say
        mov     [pwv_smicmd],eax
        mov     [pwv_pm1acnt],eax
        mov     [pwv_pm1bcnt],eax
        mov     [pwv_pm1aevt],eax
        mov     [pwv_pm1bevt],eax
        mov     [pwv_tmr],eax
        mov     [pwv_s5at],eax
        mov     [pwv_slpa],ax
        mov     [pwv_slpb],ax
        mov     [pwv_on],al
        mov     [pwv_off],al
        call    pwr_rsdp                        ; -> ESI, or CF
        jc      pwf_none
        mov     [pwv_rsdp],esi

;--- the list of tables: the RSDT, or from 2.0 on the XSDT ------------
        mov     byte ptr [pwv_shift],2
        mov     edx,RSDT_SIG
        mov     ebx,[esi+RSDP_RSDT]
        or      ebx,ebx
        jnz     pwf_list
        cmp     byte ptr [esi+RSDP_REV],2
        jb      pwf_none
        cmp     dword ptr [esi+RSDP_XSDT+4],0
        jne     pwf_none
        mov     byte ptr [pwv_shift],3
        mov     edx,XSDT_SIG
        mov     ebx,[esi+RSDP_XSDT]
pwf_list:
        call    pwt_table                       ; -> ECX = its length
        jc      pwf_none
        cmp     [ebx+SDT_SIGN],edx
        jne     pwf_none
        mov     [pwv_list],ebx
        mov     cl,[pwv_shift]                  ; entries, not bytes
        mov     eax,[ebx+SDT_LEN]
        sub     eax,SDT_HDRLEN
        shr     eax,cl
        mov     [pwv_count],eax

;--- the FADT ---------------------------------------------------------
        mov     eax,FADT_SIG
        xor     edx,edx
        call    pws_scan                        ; -> EBX, ECX, or CF
        jc      pwf_none

        mov     eax,[ebx+FADT_SMICMD]
        cmp     eax,10000h
        jb      pwf_smi
        xor     eax,eax
pwf_smi:
        mov     [pwv_smicmd],eax
        mov     al,[ebx+FADT_ACPION]
        mov     [pwv_on],al
        mov     al,[ebx+FADT_ACPIOFF]
        mov     [pwv_off],al

        mov     eax,[ebx+FADT_PM1ACNT]
        mov     edx,FADT_XPM1ACNT
        call    pwq_port
        mov     [pwv_pm1acnt],eax
        mov     eax,[ebx+FADT_PM1BCNT]
        mov     edx,FADT_XPM1BCNT
        call    pwq_port
        mov     [pwv_pm1bcnt],eax
        mov     eax,[ebx+FADT_PM1AEVT]
        mov     edx,FADT_XPM1AEVT
        call    pwq_port
        mov     [pwv_pm1aevt],eax
        mov     eax,[ebx+FADT_PM1BEVT]
        mov     edx,FADT_XPM1BEVT
        call    pwq_port
        mov     [pwv_pm1bevt],eax
        mov     eax,[ebx+FADT_PMTMR]
        mov     edx,FADT_XPMTMR
        call    pwq_port
        mov     [pwv_tmr],eax

        mov     dword ptr [pwv_tmrmask],00FFFFFFh
        cmp     ecx,FADT_FLAGS + 4
        jb      pwf_t24
        test    dword ptr [ebx+FADT_FLAGS],FADTF_TMR32
        jz      pwf_t24
        mov     dword ptr [pwv_tmrmask],0FFFFFFFFh
pwf_t24:

;--- no PM1a control register, no way to say "off" --------------------
        cmp     dword ptr [pwv_pm1acnt],0
        je      pwf_nooff

;--- \_S5_: in the DSDT, else in an SSDT ------------------------------
        mov     eax,[ebx+FADT_DSDT]
        or      eax,eax
        jnz     pwf_dsdt
        cmp     ecx,FADT_XDSDT + 8
        jb      pwf_ssdt
        cmp     dword ptr [ebx+FADT_XDSDT+4],0
        jne     pwf_ssdt
        mov     eax,[ebx+FADT_XDSDT]
pwf_dsdt:
        mov     ebx,eax
        call    pwt_table
        jc      pwf_ssdt
        cmp     dword ptr [ebx+SDT_SIGN],DSDT_SIG
        jne     pwf_ssdt
        call    pw5_find
        jnc     pwf_found

pwf_ssdt:
        xor     edx,edx
pwf_snext:
        mov     eax,SSDT_SIG
        call    pws_scan                        ; EDX moves past each
        jc      pwf_nooff
        call    pw5_find
        jc      pwf_snext

pwf_found:
        popad
        clc
        ret
pwf_nooff:
        popad
        mov     al,ACPE_NOOFF
        stc
        ret
pwf_none:
        popad
        mov     al,ACPE_NONE
        stc
        ret

;=====================================================================
; acpi_off - ACPF_OFF.  Switch the power off.  Returns, with CF and
;            everything preserved, only if the machine is still running
;            when the wait is over - or if acpi_find finds nothing.
;=====================================================================
acpi_off:
        pushad
        pushfd
        call    acpi_find                       ; again: nothing here relies
        jc      pwo_fail                        ; on a caller having asked
        cli
        mov     byte ptr [pwv_entered],0

;--- into ACPI mode, unless the firmware is in it already -------------
        mov     edx,[pwv_pm1acnt]
        in      ax,dx
        test    ax,PM1_SCIEN
        jnz     pwo_mode
        mov     edx,[pwv_smicmd]
        or      edx,edx
        jz      pwo_mode
        mov     al,[pwv_on]
        or      al,al
        jz      pwo_mode
        out     dx,al
        mov     byte ptr [pwv_entered],1
        mov     ebx,PM1_SCIEN
        call    pww_wait                        ; CF: the bit never came on,
pwo_mode:                                       ; and the write is made anyway

;--- nothing pending that the machine might wake up for ---------------
        mov     ax,PM1_STSALL
        mov     edx,[pwv_pm1aevt]
        or      edx,edx
        jz      pwo_stsb
        out     dx,ax
pwo_stsb:
        mov     edx,[pwv_pm1bevt]
        or      edx,edx
        jz      pwo_type
        out     dx,ax

;--- the sleep type on its own, then again with SLP_EN ----------------
; Two writes, as Linux makes them: some chipsets latch the type and
; act on the enable, and want them in that order.
pwo_type:
        mov     edx,[pwv_pm1acnt]
        in      ax,dx
        mov     [pwv_olda],ax
        and     ax,PM1_KEEP
        or      ax,[pwv_slpa]
        mov     [pwv_newa],ax
        out     dx,ax
        mov     edx,[pwv_pm1bcnt]
        or      edx,edx
        jz      pwo_en
        in      ax,dx
        mov     [pwv_oldb],ax
        and     ax,PM1_KEEP
        or      ax,[pwv_slpb]
        mov     [pwv_newb],ax
        out     dx,ax
pwo_en:
        mov     edx,[pwv_pm1acnt]
        mov     ax,[pwv_newa]
        or      ax,PM1_SLPEN
        out     dx,ax
        mov     edx,[pwv_pm1bcnt]
        or      edx,edx
        jz      pwo_wait
        mov     ax,[pwv_newb]
        or      ax,PM1_SLPEN
        out     dx,ax

;--- the power should go before this wait is over ----------------------
pwo_wait:
        xor     ebx,ebx
        call    pww_wait

;--- STILL RUNNING: put back what was changed -------------------------
        mov     edx,[pwv_pm1acnt]
        mov     ax,[pwv_olda]
        out     dx,ax
        mov     edx,[pwv_pm1bcnt]
        or      edx,edx
        jz      pwo_legacy
        mov     ax,[pwv_oldb]
        out     dx,ax
pwo_legacy:
        cmp     byte ptr [pwv_entered],0
        je      pwo_fail
        mov     edx,[pwv_smicmd]
        mov     al,[pwv_off]
        out     dx,al
pwo_fail:
        popfd
        popad
        stc
        ret

;=====================================================================
; acpi_report - ACPF_REPORT.  acpi_find, then what it found in the
;               registers PMDOS.INC lists - on CF as well, as far as it
;               got, with AL = the reason in place of the RSDP.
;
; FOR A MACHINE THAT WILL NOT SWITCH OFF.  What the write was, where it
; went and which bytes of the DSDT it came from are the three things
; that say why, and none of them can be seen from ring 3.  Reading the
; PM1a control register is the one thing this does to the hardware.
;=====================================================================
acpi_report:
        call    acpi_find
        pushfd
        push    eax                             ; AL = the reason, on CF
        mov     ebx,[pwv_pm1bcnt]
        shl     ebx,16
        or      ebx,[pwv_pm1acnt]
        xor     ecx,ecx
        mov     edx,[pwv_pm1acnt]
        or      edx,edx
        jz      pwp_types
        in      ax,dx
        mov     cx,ax
        shl     ecx,16
pwp_types:
        mov     ax,[pwv_slpb]
        shr     ax,10
        mov     ch,al
        mov     ax,[pwv_slpa]
        shr     ax,10
        mov     cl,al
        movzx   edx,byte ptr [pwv_off]
        shl     edx,8
        mov     dl,[pwv_on]
        shl     edx,16
        or      edx,[pwv_smicmd]
        mov     esi,[pwv_tmr]
        shl     esi,16
        or      esi,[pwv_pm1aevt]
        mov     edi,[pwv_s5at]
        pop     eax
        popfd
        jc      pwp_out
        mov     eax,[pwv_rsdp]
pwp_out:
        ret

;---------------------------------------------------------------------
; pwr_rsdp - find the RSDP -> ESI.  CF if there is none.
;
; The EBDA's first kilobyte, then the BIOS area.  A copy is believed
; only if its first 20 bytes add up to zero, which is what tells an
; RSDP from the same eight characters in somebody's message text.
;---------------------------------------------------------------------
pwr_rsdp:
        movzx   esi,word ptr ds:[BDA_EBDA]
        shl     esi,4
        cmp     esi,80000h                      ; an EBDA is in the last
        jb      pwr_bios                        ; 128K of conventional
        cmp     esi,0A0000h                     ; memory, or there is none
        jae     pwr_bios
        mov     ecx,1024 / 16
        call    pwr_scan
        jnc     pwr_out
pwr_bios:
        mov     esi,0E0000h
        mov     ecx,20000h / 16
        call    pwr_scan
pwr_out:
        ret

pwr_scan:
        cmp     dword ptr [esi],RSDP_SIG1
        jne     pwr_next
        cmp     dword ptr [esi+4],RSDP_SIG2
        jne     pwr_next
        push    ecx
        xor     eax,eax
        mov     ecx,RSDP_V1LEN
pwr_sum:
        add     al,[esi+ecx-1]
        loop    pwr_sum
        pop     ecx
        or      al,al
        jz      pwr_found
pwr_next:
        add     esi,16
        loop    pwr_scan
        stc
        ret
pwr_found:
        clc
        ret

;---------------------------------------------------------------------
; pwt_table - EBX = a table's physical address -> ECX = its length,
;             and all of it mapped.  CF if it cannot be, or the length
;             is not one a table has.  EAX is lost.
;
; 32 bytes past the end are mapped as well, so that pw5_find can read
; a malformed \_S5_ at the very end of a table without faulting.
;---------------------------------------------------------------------
pwt_table:
        or      ebx,ebx
        jz      pwt_bad
        mov     ecx,SDT_HDRLEN
        call    pg_mapphys
        jc      pwt_out
        mov     ecx,[ebx+SDT_LEN]
        cmp     ecx,SDT_HDRLEN
        jb      pwt_bad
        cmp     ecx,SDT_MAX
        ja      pwt_bad
        add     ecx,32
        call    pg_mapphys
        lea     ecx,[ecx-32]                    ; LEA: the carry is the answer
pwt_out:
        ret
pwt_bad:
        stc
        ret

;---------------------------------------------------------------------
; pws_scan - EAX = a signature, EDX = the first entry of the RSDT or
;            XSDT to look at -> EBX = the next table with that
;            signature, ECX = its length, EDX = the entry after it.
;            CF if there is none.  ESI preserved.
;---------------------------------------------------------------------
pws_scan:
        push    esi
pws_lp:
        cmp     edx,[pwv_count]
        jae     pws_none
        mov     cl,[pwv_shift]
        mov     esi,edx
        shl     esi,cl
        add     esi,[pwv_list]
        inc     edx
        cmp     byte ptr [pwv_shift],3
        jne     pws_low
        cmp     dword ptr [esi+SDT_HDRLEN+4],0
        jne     pws_lp                          ; above 4GB
pws_low:
        mov     ebx,[esi+SDT_HDRLEN]
        push    eax
        call    pwt_table
        pop     eax
        jc      pws_lp
        cmp     [ebx+SDT_SIGN],eax
        jne     pws_lp
        pop     esi
        clc
        ret
pws_none:
        pop     esi
        stc
        ret

;---------------------------------------------------------------------
; pwq_port - one FADT register as a port number.  EAX = its ACPI 1.0
;            field, EDX = where the 2.0 field for it is, EBX = the
;            FADT, ECX = its length -> EAX = the port, 0 for none.
;
; The 1.0 field wins when it is filled in.  The 2.0 one is only read
; when it is in the table and names an I/O port; one in memory is a
; register this does not write.
;---------------------------------------------------------------------
pwq_port:
        or      eax,eax
        jnz     pwq_check
        lea     eax,[edx+GAS_LEN]
        cmp     ecx,eax
        mov     eax,0                           ; MOV: the flags are the CMP's
        jb      pwq_out
        cmp     byte ptr [ebx+edx+GAS_SPACE],1
        jne     pwq_out
        cmp     dword ptr [ebx+edx+GAS_ADDR+4],0
        jne     pwq_out
        mov     eax,[ebx+edx+GAS_ADDR]
pwq_check:
        cmp     eax,10000h
        jb      pwq_out
        xor     eax,eax                         ; past the last port there is
pwq_out:
        ret

;---------------------------------------------------------------------
; pw5_find - EBX = a DSDT or SSDT, ECX = its length -> the two sleep
;            types of its \_S5_ in pwv_slpa and pwv_slpb, shifted into
;            place.  CF if it has no \_S5_ that is a constant package.
;            Everything preserved.
;
;   08h ['\'] '_S5_' 12h PkgLength NumElements int int ...
;
; A PkgLength is one to four bytes, and bits 7-6 of the first say how
; many follow it.  The integers are Zero, One, Ones, or a byte, word or
; dword after its prefix.  A package of one is taken to mean the same
; type for both registers: PM1b is rare, and when there is none the
; second type is never written anywhere.
;---------------------------------------------------------------------
pw5_find:
        pushad
        lea     esi,[ebx+SDT_HDRLEN]
        lea     edi,[ebx+ecx]                   ; EDI = the end
pw5_lp:
        lea     eax,[esi+4]
        cmp     eax,edi
        ja      pw5_none
        cmp     dword ptr [esi],S5_NAME
        jne     pw5_next
        cmp     byte ptr [esi-1],AML_NAMEOP
        je      pw5_named
        cmp     byte ptr [esi-1],AML_ROOT
        jne     pw5_next
        cmp     byte ptr [esi-2],AML_NAMEOP
        jne     pw5_next
pw5_named:
        cmp     byte ptr [esi+4],AML_PACKAGE
        jne     pw5_next
        movzx   eax,byte ptr [esi+5]
        shr     eax,6
        lea     edx,[esi+6+eax]                 ; EDX -> NumElements
        movzx   ecx,byte ptr [edx]
        inc     edx
        jecxz   pw5_next
        call    pwi_int
        jc      pw5_next
        mov     ebp,eax                         ; EBP = PM1a's type
        cmp     ecx,1
        je      pw5_got
        call    pwi_int
        jc      pw5_next
pw5_got:
        cmp     edx,edi                         ; read past the end: not
        ja      pw5_next                        ; what it looked like
        and     eax,7
        shl     eax,10
        mov     [pwv_slpb],ax
        and     ebp,7
        shl     ebp,10
        mov     [pwv_slpa],bp
        mov     [pwv_s5at],esi
        popad
        clc
        ret
pw5_next:
        inc     esi
        jmp     pw5_lp
pw5_none:
        popad
        stc
        ret

;---------------------------------------------------------------------
; pwi_int - EDX -> an AML integer constant -> EAX = its value, EDX past
;           it.  CF if it is something else.
;---------------------------------------------------------------------
pwi_int:
        movzx   eax,byte ptr [edx]
        inc     edx
        cmp     al,AML_ZERO                     ; Zero and One are their own
        je      pwi_ok                          ; values, and Ones is all
        cmp     al,AML_ONE                      ; ones: as a sleep type, 7
        je      pwi_ok
        cmp     al,AML_ONES
        je      pwi_ok
        cmp     al,AML_BYTE
        je      pwi_byte
        cmp     al,AML_WORD
        je      pwi_word
        cmp     al,AML_DWORD
        je      pwi_dword
        stc
        ret
pwi_byte:
        movzx   eax,byte ptr [edx]
        inc     edx
        jmp     short pwi_ok
pwi_word:
        movzx   eax,word ptr [edx]
        add     edx,2
        jmp     short pwi_ok
pwi_dword:
        mov     eax,[edx]
        add     edx,4
pwi_ok:
        clc
        ret

;---------------------------------------------------------------------
; pww_wait - wait until every bit in EBX is set in the PM1a control
;            register, or for PWR_WAIT if EBX is 0.  CF if the time ran
;            out first.  Everything preserved.
;
; The PM timer counts up and wraps at 24 bits, or 32 with TMR_VAL_EXT:
; the time gone is the sum of the differences between readings, each
; masked, so a wrap in the middle costs nothing.
;---------------------------------------------------------------------
pww_wait:
        pushad
        mov     edx,[pwv_tmr]
        or      edx,edx
        jz      pww_spin
        in      eax,dx
        mov     esi,eax                         ; ESI = the last reading
        xor     edi,edi                         ; EDI = ticks gone by
pww_lp:
        call    pwb_bits
        jnz     pww_done
        mov     edx,[pwv_tmr]
        in      eax,dx
        mov     ecx,eax
        sub     eax,esi
        mov     esi,ecx
        and     eax,[pwv_tmrmask]
        add     edi,eax
        cmp     edi,PWR_WAIT
        jb      pww_lp
        popad
        stc
        ret
pww_spin:
        mov     edi,PWR_SPINS
pww_slp:
        call    pwb_bits
        jnz     pww_done
        in      al,80h
        dec     edi
        jnz     pww_slp
        popad
        stc
        ret
pww_done:
        popad
        clc
        ret

; pwb_bits - ZF clear if every bit of EBX is set in PM1a's control
; register; ZF set if one is not, or if EBX is 0.  EAX and EDX are lost.
pwb_bits:
        or      ebx,ebx
        jz      pwb_out
        mov     edx,[pwv_pm1acnt]
        in      ax,dx
        and     ax,bx
        cmp     ax,bx
        jne     pwb_no
        or      ebx,ebx                         ; ZF clear: EBX is not 0
        ret
pwb_no:
        cmp     eax,eax                         ; ZF set
pwb_out:
        ret

;---------------------------------------------------------------------
; What acpi_find found
;---------------------------------------------------------------------
pwv_rsdp        dd      0
pwv_s5at        dd      0               ; \_S5_ in its table, 0 = not found
pwv_list        dd      0               ; the RSDT or XSDT
pwv_count       dd      0               ; ...and its entries
pwv_smicmd      dd      0               ; ports, 0 = none
pwv_pm1acnt     dd      0
pwv_pm1bcnt     dd      0
pwv_pm1aevt     dd      0
pwv_pm1bevt     dd      0
pwv_tmr         dd      0
pwv_tmrmask     dd      0
pwv_slpa        dw      0               ; SLP_TYP for PM1a, in bits 10-12
pwv_slpb        dw      0               ; ...and for PM1b
pwv_olda        dw      0               ; the control registers as found
pwv_oldb        dw      0
pwv_newa        dw      0               ; ...and with the type in them
pwv_newb        dw      0
pwv_on          db      0               ; ACPI_ENABLE
pwv_off         db      0               ; ACPI_DISABLE
pwv_shift       db      0               ; 2 for the RSDT, 3 for the XSDT
pwv_entered     db      0               ; acpi_off put the firmware into
                                        ; ACPI mode, and owes it legacy
