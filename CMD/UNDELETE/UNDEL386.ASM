;=====================================================================
; UNDEL386.ASM - UNDELETE.386, delete protection while Windows runs
;
; UNDELETE=ON is the kernel's (DOS\RECYCLE.INC), and the kernel is
; parked while Windows 3.1 or Windows for Workgroups runs in 386
; enhanced mode.  A file deleted then goes to RMDOS, or to VFAT with
; 32-bit file access, and neither keeps anything.  This driver does what
; rcy_delete does, from in front of both of them:
;
;       [386Enh]
;       device=C:\UNDELETE.386
;
; WHERE IT SITS.  In two places, in front of everything else:
;
;   - the V86 INT 21h chain, which a DOS program's delete reaches, and
;     a Windows program's too when Windows sends it down to DOS.  The
;     chain runs the last hook first, and the hook goes in at
;     Init_Complete, after IFSMGR has put in its own (ud_hookall)
;   - protected-mode INT 21h, the default vector of every VM.  With
;     32-bit file access IFSMGR answers a Windows program's delete there
;     and it never reaches V86 at all (ud_pm21)
;
; WHAT IT DOES WITH ONE.  It stops the VM where it is and makes ordinary
; DOS calls in it, nested, through the same chain: find the file, make
; \RECYCLED, read and write UNDELETE.DAT, and rename the file into
; \RECYCLED.  The caller then gets "done" and the real delete never
; runs.  Because they are ordinary calls they go through IFSMGR and
; VFAT's cache or RMDOS as the VM's own would, and there is no second
; view of the disk to keep in step.  The directory, the index and the
; names are the kernel's (INC\PMDOS.INC, DELETE PROTECTION), so
; UNDELETE.EXE reads what is written here without knowing the difference.
;
; WHAT IS NOT KEPT is what the kernel does not keep, and Windows' own
; scratch files: *.TMP, *.$$$, *.SWP, names that start with ~, anything
; inside \RECYCLED, anything on a network drive, a read-only file (DOS
; refuses to delete it), and a file bigger than the allowance.  When
; keeping fails for ANY reason the call simply goes on, and the program
; gets the delete it asked for.
;
; AN FCB DELETE (13h) is done here whole (ud_fcb): its wildcards are
; walked with 4Eh/4Fh, each file is kept or deleted by 41h, and AL is
; answered as DOS answers it.  Only a network drive's goes on to DOS.
;
; A DISK THAT REFUSES.  Nothing here may stop to ask the user, because
; the question would be asked in a VM holding the critical section, and
; the dialog may need a VM that is waiting for it.  So INT 24h is
; answered "fail" during the nested calls, keeping fails, and the delete
; that follows asks the question in the ordinary way, once.
;
; ONE AT A TIME.  The low block and the index are shared by every VM, so
; the whole of a delete is inside the VMM's critical section.  That also
; keeps a DOS box in the background running until it has finished.
;
; Standard mode loads no virtual devices, so this does nothing there.
;
; An LE VxD, laid out as LRGDISK.386 and PMDOS.386 are.  No VMM.INC: the
; numbers are the DDK's, and the service ordinals were counted from its
; VMM.INC.  Linked with: wlink format windows vxd, see BUILD.CMD
;=====================================================================
        .386p
        .model  flat

_LTEXT  SEGMENT DWORD USE32 PUBLIC 'CODE'
_LTEXT  ENDS
_LDATA  SEGMENT DWORD USE32 PUBLIC 'CODE'
_LDATA  ENDS
_ITEXT  SEGMENT DWORD USE32 PUBLIC 'ICODE'
_ITEXT  ENDS
_IDATA  SEGMENT DWORD USE32 PUBLIC 'ICODE'
_IDATA  ENDS

_LGROUP GROUP   _LTEXT, _LDATA
_IGROUP GROUP   _ITEXT, _IDATA

VXDCALL MACRO   svc
        int     20h
        dd      svc
        ENDM

;--- the VMM --------------------------------------------------------
S_ALLOC_PMCB    EQU     0001000Ch       ; Allocate_PM_Call_Back
S_MAP_FLAT      EQU     0001001Ch
S_BEGIN_CRIT    EQU     0001001Fh
S_END_CRIT      EQU     00010020h
S_HOOK_V86      EQU     00010041h
S_GET_PMVEC     EQU     00010044h
S_SET_PMVEC     EQU     00010045h
S_SIM_IRET      EQU     00010047h
S_SIM_FARJMP    EQU     00010049h
S_BEGIN_NEST    EQU     00010082h       ; Begin_Nest_V86_Exec
S_EXEC_INT      EQU     00010084h
S_END_NEST      EQU     00010086h
S_SAVE_CLIENT   EQU     0001008Dh
S_REST_CLIENT   EQU     0001008Eh
S_ALLOC_GV86    EQU     000100A8h       ; C-callable: nBytes, flags
GVDA_PARAALIGN  EQU     00000004h
GVDA_ZEROINIT   EQU     00000200h
BLOCK_SVC_INTS  EQU     1
BLOCK_ENA_INTS  EQU     4

DEVICE_INIT     EQU     1
INIT_COMPLETE   EQU     2
SYS_VM_TERM     EQU     4
VM_NOT_EXEC     EQU     0Bh
UNDEF_ORDER     EQU     80000000h

; Client_Reg_Struc
C_EDI           EQU     00h
C_ESI           EQU     04h
C_EBX           EQU     10h
C_EDX           EQU     14h
C_ECX           EQU     18h
C_EAX           EQU     1Ch
C_EFLAGS        EQU     2Ch
C_ES            EQU     38h
C_DS            EQU     3Ch
CLIENT_SIZE     EQU     6Ch
V86_MASK        EQU     00020000h       ; Client_EFlags: in V86 mode
V86_TOP         EQU     110000h         ; the end of a VM's 1MB and HMA

; the VM's control block
CB_VMSTATUS     EQU     00h             ; CB_VM_Status
VMSTAT_USE32    EQU     80h             ; VMStat_PM_Use32

;--- what the kernel leaves in low memory (INC\PMDOS.INC) ------------
GV_BASE         EQU     1F00h
KL_BRGSIG       EQU     GV_BASE + 0E0h
KL_UNDEL        EQU     GV_BASE + 0E9h
BRG_SIGVAL      EQU     42475250h

;--- UNDELETE.DAT (INC\PMDOS.INC, DELETE PROTECTION) -----------------
UNDEL_PCT       EQU     10
UDH_VERSION     EQU     08h
UDH_RECLEN      EQU     0Ah
UDH_NEXTSEQ     EQU     0Ch
UDR_FLAGS       EQU     00h
UDR_ATTR        EQU     01h
UDR_SEQ         EQU     04h
UDR_SIZE        EQU     08h
UDR_DTIME       EQU     10h
UDR_DDATE       EQU     12h
UDR_STORED      EQU     14h
UDR_SHORT       EQU     20h
UDR_DIR         EQU     40h
UDR_DIRMAX      EQU     260
UDR_LONG        EQU     UDR_DIR + UDR_DIRMAX
UDR_LONGMAX     EQU     268
UDR_LEN         EQU     UDR_LONG + UDR_LONGMAX
UDRF_USED       EQU     01h

ATTR_RDONLY     EQU     01h
ATTR_HIDDEN     EQU     02h
ATTR_SYSTEM     EQU     04h
ATTR_VOLUME     EQU     08h
ATTR_DIR        EQU     10h
ATTR_FIND       EQU     27h             ; files of every kind
ATTR_KEEPDIR    EQU     16h             ; \RECYCLED: hidden, system
ATTR_INDEX      EQU     26h             ; UNDELETE.DAT: and archive

ERR_NOFILE      EQU     2

DTA_ATTR        EQU     15h
DTA_SIZE        EQU     1Ah
DTA_NAME        EQU     1Eh

;--- the block below 1MB that every nested call works from -----------
; It is every VM's memory, taken before any of them starts, so it is
; kept small: one record at a time, and only the header's first 16 bytes.
UB_PATH         EQU     000h            ; the caller's name          128
UB_TRUE         EQU     080h            ; its true name              128
UB_DEST         EQU     100h            ; a name in \RECYCLED         64
UB_BIN          EQU     140h            ; "X:\RECYCLED"               16
UB_INDEX        EQU     150h            ; "X:\RECYCLED\UNDELETE.DAT"  32
UB_DTA          EQU     170h            ; the file's find             48
UB_DTA2         EQU     1A0h            ; an FCB delete's walk        48
UB_FCB          EQU     1D0h            ; the caller's FCB            48
UB_PAT          EQU     200h            ; "D:NAME????.EXT"            16
UB_HDR          EQU     210h            ; the index's header          16
UB_REC          EQU     220h            ; the record being written
UB_SCAN         EQU     UB_REC + UDR_LEN        ; a record being read
SCAN_RECS       EQU     1
UB_END          EQU     UB_SCAN + SCAN_RECS * UDR_LEN
UB_SIZE         EQU     ( UB_END + 15 ) AND 0FFF0h

PATH_MAX        EQU     128
FCB_MAX         EQU     44              ; an extended FCB: 7 + 37
FCB_MIN         EQU     12              ; the drive and the name

IFDEF UD_TRACE
UDT     MACRO   letter
        push    eax
        push    edx
        mov     dx,0E9h
        mov     al,letter
        out     dx,al
        pop     edx
        pop     eax
        ENDM
ELSE
UDT     MACRO   letter
        ENDM
ENDIF

;=====================================================================
; LOCKED DATA
;=====================================================================
_LDATA  SEGMENT
        PUBLIC  UNDELETE_DDB

UNDELETE_DDB    dd      0                       ; DDB_Next
                dw      030Ah                   ; for Windows 3.1 and up
                dw      0                       ; no device ID: no API
                db      1, 0                    ; version 1.0
                dw      0
                db      'UNDELETE'
                dd      UNDEF_ORDER             ; after IFSMGR
                dd      OFFSET FLAT:ud_control
                dd      0, 0
                dd      0, 0
                dd      0
                dd      0, 0

ud_pg           dd      0                       ; the low block, linear
ud_seg          dd      0                       ; ...and as a segment
ud_owner        dd      0                       ; the VM doing a delete
ud_h            dd      -1                      ; the index's handle
ud_dtaseg       dd      0                       ; the caller's DTA
ud_dtaoff       dd      0
ud_drive        dd      0                       ; 0 = A:
ud_namep        dd      0                       ; in UB_TRUE: the last part
ud_extp         dd      0                       ; ...its extension, or 0
ud_clb          dd      0                       ; bytes in a cluster
ud_free         dd      0                       ; clusters free,
ud_bin          dd      0                       ; ...in kept files,
ud_need         dd      0                       ; ...and in this one
ud_nrec         dd      0                       ; records, header included
ud_old          dd      0                       ; the oldest kept file's
ud_oldseq       dd      0
ud_slot         dd      0                       ; where the new record goes
ud_seq          dd      0
ud_tries        dd      0
ud_idx          dd      0                       ; the walk's record number
ud_batch        dd      0
ud_pidx         dd      0                       ; the record being purged
ud_pmcb         dd      0                       ; our protected-mode INT 21h
ud_pmoldcs      dd      0                       ; and the one before it
ud_pmoldip      dd      0
ud_fcblen       dd      0                       ; an FCB delete: bytes copied,
ud_fattr        dd      0                       ; the search attribute,
ud_fdrive       dd      0                       ; the drive, 1 = A:,
ud_fcbp         dd      0                       ; the FCB proper, linear,
ud_fdtaseg      dd      0                       ; and the caller's DTA
ud_fdtaoff      dd      0
ud_ok           db      0                       ; PM-DOS, with UNDELETE=ON
ud_on           db      0                       ; ready, and Windows is up
ud_dtaset       db      0                       ; our DTA is in place
ud_kept         db      0
ud_mode         db      0                       ; 0 = 41h, 1 = 13h
ud_handled      db      0                       ; 13h: answered here,
ud_fcbal        db      0                       ; ...with this AL
ud_fhit         db      0                       ; ...having deleted one,
ud_fro          db      0                       ; ...or met a read-only one
ud_c24          db      0                       ; ud_i24 answered Fail
                db      0, 0

ud_skipext      db      'TMP',0, '$$$',0, 'SWP',0, 0,0,0,0
ud_binname      db      'RECYCLED'
ud_binslash     db      'RECYCLED\'
ud_indexname    db      '\UNDELETE.DAT',0
ud_magic        db      'PMUNDEL',1Ah

ud_save         db      CLIENT_SIZE + 20 dup(0) ; the caller's registers
_LDATA  ENDS

;=====================================================================
; LOCKED CODE
;=====================================================================
_LTEXT  SEGMENT
        ASSUME  ds:FLAT, es:FLAT, ss:FLAT

ud_control:
        cmp     eax,DEVICE_INIT
        je      uc_init
        cmp     eax,INIT_COMPLETE
        je      uc_ready
        cmp     eax,SYS_VM_TERM
        je      uc_stop
        cmp     eax,VM_NOT_EXEC
        je      uc_vmgone
        clc
        ret
uc_init:
        jmp     ud_devinit
uc_ready:
        jmp     ud_hookall
uc_out:
        clc
        ret
uc_stop:
        mov     byte ptr [ud_on],0      ; Windows is on its way out, and
        clc                             ; its swap file goes as it leaves
        ret
uc_vmgone:
        cmp     ebx,[ud_owner]          ; closed in the middle of a delete
        jne     uc_out
        mov     dword ptr [ud_owner],0
        clc
        ret

;---------------------------------------------------------------------
; ud_i24 - the V86 INT 24h chain.  During a nested call the answer is
;          "fail", where DOS allows that; see the top of this file.
;---------------------------------------------------------------------
ud_i24:
        cmp     ebx,[ud_owner]
        jne     u24_chain
        test    byte ptr [ebp+C_EAX+1],08h      ; may this one fail?
        jz      u24_chain
        mov     byte ptr [ebp+C_EAX],3
        mov     byte ptr [ud_c24],1             ; and the FCB walk is told
        clc
        ret
u24_chain:
        stc
        ret

;---------------------------------------------------------------------
; ud_i21 - the V86 INT 21h chain.  EBX = the VM, EBP -> its registers.
;          CF set sends the call on; CF clear means it has been answered.
;
; Nothing has been pushed for the INT yet - the end of the chain does
; that - so an answered call needs only its carry cleared.
;---------------------------------------------------------------------
ud_i21:
        cmp     byte ptr [ebp+C_EAX+1],13h
        je      ud_try13                ; answered in AL, no carry
        cmp     byte ptr [ebp+C_EAX+1],41h
        jne     ui_chain
        call    ud_try
        jc      ui_ret
        and     byte ptr [ebp+C_EFLAGS],NOT 1
ui_ret:
        ret

;---------------------------------------------------------------------
; ud_pm21 - protected-mode INT 21h, a callback in every VM's vector
;           table in front of whatever was there.  With 32-bit file
;           access that is IFSMGR, which answers a Windows program's
;           delete itself and never sends it down to V86.  Here the INT
;           has pushed its frame, so an answered call is an IRET, and any
;           other goes on to the old vector as if this had not been here.
;---------------------------------------------------------------------
ud_pm21:
        cmp     byte ptr [ebp+C_EAX+1],13h
        je      up_13
        cmp     byte ptr [ebp+C_EAX+1],41h
        jne     up_chain
        call    ud_try
        jc      up_chain
        VXDCALL S_SIM_IRET
        and     byte ptr [ebp+C_EFLAGS],NOT 1
        ret
up_13:
        call    ud_try13
        jc      up_chain
        VXDCALL S_SIM_IRET
        ret
up_chain:
        mov     ecx,[ud_pmoldcs]
        mov     edx,[ud_pmoldip]
        VXDCALL S_SIM_FARJMP
        ret

;---------------------------------------------------------------------
; ud_try - the delete in the caller's DS:DX: keep it if it can be kept.
;          EBX = the VM, EBP -> its registers.  CF clear if it was
;          kept and the caller is to be told it is gone; CF set if the
;          call is to go on and delete it.
;
; ud_try13 - the same for 13h, whose DS:DX is an FCB.  CF clear if the
;          whole delete was done here and AL set as DOS would set it;
;          CF set if it is to go on to DOS untouched (see ud_fcb).
;---------------------------------------------------------------------
ud_try13:
        mov     ecx,FCB_MAX
        mov     al,1
        jmp     short ut_go
ud_try:
        mov     ecx,PATH_MAX
        xor     al,al
ut_go:
        cmp     byte ptr [ud_on],0
        je      ui_chain
        cmp     ebx,[ud_owner]          ; one of our own calls - and it
        je      ui_chain                ; must not touch ud_mode: the FCB
        mov     [ud_mode],al            ; walk's own 41h comes this way

        pushad
        mov     ecx,BLOCK_SVC_INTS OR BLOCK_ENA_INTS
        VXDCALL S_BEGIN_CRIT
        mov     ebx,[esp+10h]           ; the VM, from the PUSHAD
        mov     ebp,[esp+08h]
        mov     [ud_owner],ebx
        mov     byte ptr [ud_kept],0
        mov     byte ptr [ud_handled],0
        mov     byte ptr [ud_c24],0

        ; THE NAME OR THE FCB, while DS:DX is still the caller's.  A V86
        ; caller's megabyte is all there; a protected-mode caller's
        ; segment may end anywhere, and a page past it may be no page at
        ; all, so the copy stops at its limit.
        mov     ecx,[esp+18h]           ; PATH_MAX or FCB_MAX
        test    dword ptr [ebp+C_EFLAGS],V86_MASK
        jz      ui_pm
        movzx   eax,word ptr [ebp+C_DS] ; V86: no further than the end of
        shl     eax,4                   ; the megabyte and the HMA above it
        movzx   edx,word ptr [ebp+C_EDX]
        add     eax,edx
        mov     edx,V86_TOP
        sub     edx,eax
        jbe     ui_done
        cmp     edx,ecx
        jae     ui_map
        mov     ecx,edx
        jmp     short ui_map
ui_pm:
        movzx   eax,word ptr [ebp+C_DS]
        verr    ax
        jnz     ui_done
        lsl     edx,eax                 ; the segment's last byte
        jnz     ui_done
        mov     eax,[ebp+C_EDX]
        test    byte ptr [ebx+CB_VMSTATUS],VMSTAT_USE32
        jnz     ui_off32
        movzx   eax,ax
ui_off32:
        sub     edx,eax                 ; bytes after the first one
        jb      ui_done
        cmp     edx,ecx
        jae     ui_map
        lea     ecx,[edx+1]
ui_map:
        push    ecx
        mov     ax,(C_DS SHL 8) OR C_EDX
        VXDCALL S_MAP_FLAT
        pop     ecx
        cmp     eax,-1
        je      ui_done
        mov     esi,eax
        cmp     byte ptr [ud_mode],0
        je      ui_name

        ; an FCB is bytes, NULs and all: as much of one as there is
        cmp     ecx,FCB_MIN
        jb      ui_done
        mov     [ud_fcblen],ecx
        mov     edi,[ud_pg]
        add     edi,UB_FCB
        cld
        rep     movsb
        mov     edi,OFFSET FLAT:ud_save
        VXDCALL S_SAVE_CLIENT
        VXDCALL S_BEGIN_NEST
        call    ud_fcb
        jmp     short ui_nested
ui_name:
        mov     edi,[ud_pg]
        add     edi,UB_PATH
ui_copy:
        mov     al,[esi]
        mov     [edi],al
        or      al,al
        jz      ui_named
        inc     esi
        inc     edi
        loop    ui_copy
        jmp     short ui_done           ; longer than DOS takes
ui_named:
IFDEF UD_TRACE
        mov     esi,[ud_pg]
        call    ud_tstr
ENDIF
        mov     edi,OFFSET FLAT:ud_save
        VXDCALL S_SAVE_CLIENT
        VXDCALL S_BEGIN_NEST
        call    ud_keep
ui_nested:
        VXDCALL S_END_NEST
        mov     esi,OFFSET FLAT:ud_save
        VXDCALL S_REST_CLIENT
ui_done:
        mov     dword ptr [ud_owner],0
        VXDCALL S_END_CRIT
        cmp     byte ptr [ud_mode],0
        je      ui_res41
        cmp     byte ptr [ud_handled],0
        je      ui_no
        mov     al,[ud_fcbal]           ; the FCB's answer, in the caller's
        mov     byte ptr [ebp+C_EAX],al ; AL now its registers are back
        jmp     short ui_yes
ui_res41:
        cmp     byte ptr [ud_kept],0
        je      ui_no
ui_yes:
        popad
        clc
        ret
ui_no:
        popad
ui_chain:
        stc
        ret

;---------------------------------------------------------------------
; ud_dos - INT 21h in the VM.  EAX = its AX; DS and ES are the low block.
;          -> EAX = the AX it answered, CF = its carry.  Only EBP is
;          preserved.
;---------------------------------------------------------------------
ud_dos:
        mov     ecx,[ud_seg]
        mov     word ptr [ebp+C_DS],cx
        mov     word ptr [ebp+C_ES],cx
        mov     word ptr [ebp+C_EAX],ax
        and     dword ptr [ebp+C_EFLAGS],NOT 401h       ; carry, direction
        mov     eax,21h
        VXDCALL S_EXEC_INT
        movzx   eax,word ptr [ebp+C_EAX]
        test    byte ptr [ebp+C_EFLAGS],1
        jnz     ud_cy
        clc
        ret
ud_cy:
        stc
        ret

IFDEF UD_TRACE
;--- ud_tstr - a new line and the string at ESI, on port E9h
ud_tstr:
        pushad
        mov     dx,0E9h
        mov     al,13
        out     dx,al
        mov     al,10
        out     dx,al
uts_lp:
        mov     al,[esi]
        or      al,al
        jz      uts_out
        out     dx,al
        inc     esi
        jmp     short uts_lp
uts_out:
        mov     al,32
        out     dx,al
        popad
        ret
ENDIF

;--- ud_path - EDX = a name in the low block, for the AX in EAX --------
ud_path:
        mov     dword ptr [ebp+C_EDX],edx
        jmp     ud_dos

;--- ud_seek - the index, to the byte EAX.  CF on an error ------------
ud_seek:
        mov     edx,eax
        shr     eax,16
        mov     dword ptr [ebp+C_ECX],eax
        movzx   edx,dx
        mov     dword ptr [ebp+C_EDX],edx
        mov     eax,[ud_h]
        mov     dword ptr [ebp+C_EBX],eax
        mov     eax,4200h
        jmp     ud_dos

;--- ud_read / ud_write - ECX bytes at the page's offset EDX, all of
;--- them or CF.  At the index's current position.
ud_read:
        mov     eax,3F00h
        jmp     short ud_xfer
ud_write:
        mov     eax,4000h
ud_xfer:
        mov     dword ptr [ebp+C_ECX],ecx
        mov     dword ptr [ebp+C_EDX],edx
        mov     edx,[ud_h]
        mov     dword ptr [ebp+C_EBX],edx
        push    ecx
        call    ud_dos
        pop     ecx
        jc      ux_out
        cmp     eax,ecx
        je      ux_out                  ; equal: the carry is clear
        stc
ux_out:
        ret

;--- ud_recpos - seek to record EAX -----------------------------------
ud_recpos:
        imul    eax,eax,UDR_LEN
        jmp     ud_seek

;--- ud_clus - EAX = bytes -> EAX = clusters, rounded up --------------
ud_clus:
        xor     edx,edx
        div     dword ptr [ud_clb]
        or      edx,edx
        jz      ucl_out
        inc     eax
ucl_out:
        ret

;--- ud_want - EAX = clusters kept -> EAX = the free clusters there must
;--- be for that to be within the allowance, as rcy_want.
ud_want:
        mov     ecx,100 - UNDEL_PCT
        mul     ecx
        mov     ecx,UNDEL_PCT
        div     ecx
        or      edx,edx
        jz      uw_out
        inc     eax
uw_out:
        ret

;---------------------------------------------------------------------
; ud_keep - keep the file named at UB_PATH.  Sets [ud_kept] if it now
;           lives in \RECYCLED.  In every other case nothing has been
;           lost, and the caller lets the delete go ahead.
;---------------------------------------------------------------------
ud_keep:
        cld
        mov     dword ptr [ud_h],-1
        mov     byte ptr [ud_dtaset],0

        ; Not from inside DOS: these calls would run on its stack.
        mov     eax,3400h
        call    ud_dos
        mov     ax,(C_ES SHL 8) OR C_EBX
        VXDCALL S_MAP_FLAT
        cmp     eax,-1
        je      uk_out
        cmp     byte ptr [eax],0
        jne     uk_out

        ; ITS ONE TRUE NAME.  "X:\..." or it is nothing kept here: a
        ; device answers "X:/NAME" and a network path "\\SERVER\...".
        mov     dword ptr [ebp+C_ESI],UB_PATH
        mov     dword ptr [ebp+C_EDI],UB_TRUE
        mov     eax,6000h
        call    ud_dos
        jc      uk_out
        mov     esi,[ud_pg]
        add     esi,UB_TRUE
        movzx   eax,byte ptr [esi]
        sub     eax,'A'
        cmp     eax,26
        jae     uk_out
        mov     [ud_drive],eax
        cmp     word ptr [esi+1],5C3Ah  ; ":\"
        jne     uk_out

        ; the last part of it, and that part's extension
        lea     edi,[esi+3]
        mov     [ud_namep],edi
        mov     dword ptr [ud_extp],0
        mov     ecx,PATH_MAX - 3
uk_tail:
        mov     al,[edi]
        or      al,al
        jz      uk_tailed
        inc     edi
        cmp     al,'\'
        jne     uk_notsl
        mov     [ud_namep],edi
        mov     dword ptr [ud_extp],0
        jmp     short uk_tnext
uk_notsl:
        cmp     al,'.'
        jne     uk_tnext
        mov     [ud_extp],edi
uk_tnext:
        loop    uk_tail
        jmp     uk_out                  ; no end to it
uk_tailed:

        ; Windows' own scratch files start with a tilde.  One of them, a
        ; bare "~" in TEMP, is deleted while Windows still has it open.
        mov     edx,[ud_namep]
        cmp     byte ptr [edx],'~'
        je      uk_out

        ; extensions that are never kept
        mov     edx,[ud_extp]
        or      edx,edx
        jz      uk_extok
        mov     eax,[edx]               ; three letters and the NUL
        mov     edi,OFFSET FLAT:ud_skipext
uk_extlp:
        cmp     dword ptr [edi],0
        je      uk_extok
        cmp     eax,[edi]
        je      uk_out
        add     edi,4
        jmp     short uk_extlp
uk_extok:

        ; from inside \RECYCLED?
        lea     esi,[esi+3]
        mov     edi,OFFSET FLAT:ud_binslash
        mov     ecx,9
        repe    cmpsb
        je      uk_out

        ; a network drive has no \RECYCLED of ours
        mov     eax,[ud_drive]
        inc     eax
        mov     dword ptr [ebp+C_EBX],eax
        mov     eax,4409h
        call    ud_dos
        jc      uk_out
        test    byte ptr [ebp+C_EDX+1],10h
        jnz     uk_out

        ; THE FILE ITSELF, through a DTA of ours
        mov     eax,2F00h
        call    ud_dos
        movzx   eax,word ptr [ebp+C_ES]
        mov     [ud_dtaseg],eax
        movzx   eax,word ptr [ebp+C_EBX]
        mov     [ud_dtaoff],eax
        mov     edx,UB_DTA
        mov     eax,1A00h
        call    ud_path
        mov     byte ptr [ud_dtaset],1

        mov     dword ptr [ebp+C_ECX],ATTR_FIND
        mov     edx,UB_TRUE
        mov     eax,4E00h
        call    ud_path
        jc      uk_out
        mov     esi,[ud_pg]
        test    byte ptr [esi+UB_DTA+DTA_ATTR],ATTR_RDONLY
        jnz     uk_out                  ; DOS will refuse it

        ; the drive's cluster size and free space
        mov     eax,[ud_drive]
        inc     eax
        mov     dword ptr [ebp+C_EDX],eax
        mov     eax,3600h
        call    ud_dos
        cmp     ax,0FFFFh
        je      uk_out
        movzx   ecx,word ptr [ebp+C_ECX]
        imul    eax,ecx
        or      eax,eax
        jz      uk_out
        mov     [ud_clb],eax
        movzx   eax,word ptr [ebp+C_EBX]
        mov     [ud_free],eax
        mov     esi,[ud_pg]
        mov     eax,[esi+UB_DTA+DTA_SIZE]
        call    ud_clus
        mov     [ud_need],eax

        call    ud_names
        call    ud_opendir
        jc      uk_out
        call    ud_openidx
        jc      uk_out
        call    ud_scan
        jc      uk_out

        ;--- THE ALLOWANCE, as rcy_delete has it ----------------------
        mov     eax,[ud_bin]
        add     eax,[ud_need]
        call    ud_want
        cmp     eax,[ud_free]
        jbe     uk_fits

        ; would it fit with nothing else kept?
        mov     eax,[ud_need]
        call    ud_want
        mov     edx,[ud_free]
        add     edx,[ud_bin]
        cmp     eax,edx
        ja      uk_out
uk_purge:
        mov     eax,[ud_old]
        or      eax,eax
        jz      uk_out
        call    ud_purge
        jc      uk_out
        add     [ud_free],eax
        call    ud_scan
        jc      uk_out
        mov     eax,[ud_bin]
        add     eax,[ud_need]
        call    ud_want
        cmp     eax,[ud_free]
        ja      uk_purge
uk_fits:
        call    ud_record
        jc      uk_out

        ;--- A NAME NOBODY HAS ----------------------------------------
        mov     dword ptr [ud_tries],64
uk_name:
        dec     dword ptr [ud_tries]
        jz      uk_out
        call    ud_mkstored
        inc     dword ptr [ud_seq]
        mov     edx,UB_DEST
        mov     eax,4300h
        call    ud_path
        jnc     uk_name

        ; the record and the next number first, as the kernel does: a
        ; record whose file never came is harmless, and a file with no
        ; record is space nobody can see
        mov     esi,[ud_pg]
        mov     eax,[ud_seq]
        mov     [esi+UB_HDR+UDH_NEXTSEQ],eax
        xor     eax,eax
        call    ud_seek
        jc      uk_out
        mov     ecx,UDH_NEXTSEQ + 4
        mov     edx,UB_HDR
        call    ud_write
        jc      uk_out
        mov     eax,[ud_slot]
        call    ud_recpos
        jc      uk_out
        mov     ecx,UDR_LEN
        mov     edx,UB_REC
        call    ud_write
        jc      uk_out
        call    ud_close

        ;--- THE MOVE -------------------------------------------------
        mov     dword ptr [ebp+C_EDI],UB_DEST
        mov     edx,UB_TRUE
        mov     eax,5600h
        call    ud_path
        jc      uk_undo
        mov     byte ptr [ud_kept],1
        UDT     4Bh                     ; K
        jmp     short uk_out

        ; It would not go.  The record goes instead, and DOS deletes the
        ; file as it would have without any of this.
uk_undo:
        UDT     75h                     ; u
        mov     edx,UB_INDEX
        mov     eax,3D02h
        call    ud_path
        jc      uk_out
        mov     [ud_h],eax
        mov     eax,[ud_slot]
        call    ud_recpos
        jc      uk_out
        mov     esi,[ud_pg]
        mov     byte ptr [esi+UB_REC+UDR_FLAGS],0
        mov     ecx,1
        mov     edx,UB_REC
        call    ud_write

uk_out:
        call    ud_close
        cmp     byte ptr [ud_dtaset],0
        je      uk_ret
        mov     eax,[ud_dtaseg]         ; the caller's DTA, back
        mov     ecx,eax
        mov     edx,[ud_dtaoff]
        mov     dword ptr [ebp+C_EDX],edx
        mov     word ptr [ebp+C_DS],cx
        mov     word ptr [ebp+C_EAX],1A00h
        mov     eax,21h
        VXDCALL S_EXEC_INT
uk_ret:
        ret

;--- ud_close - the index, if it is open ------------------------------
ud_close:
        mov     eax,[ud_h]
        cmp     eax,-1
        je      ucs_out
        mov     dword ptr [ebp+C_EBX],eax
        mov     dword ptr [ud_h],-1
        mov     eax,3E00h
        call    ud_dos
ucs_out:
        ret

;---------------------------------------------------------------------
; ud_fcb - 13h, an FCB delete, done here whole: each file the FCB
;          matches in its drive's current directory is kept, or deleted
;          by 41h when it cannot be, exactly as DOS's $FCB_Delete would
;          match and delete them - read-only files passed over,
;          directories and the volume label never matched, hidden and
;          system files only by an extended FCB's attribute.  AL is 0
;          if anything went, FFh if nothing did.
;
; Sets [ud_handled] and [ud_fcbal] when it has answered.  A network
; drive, or a call from inside DOS, is left to DOS: [ud_handled] stays 0
; and nothing has been touched.
;
; The walk has a DTA of its own, UB_DTA2.  ud_keep takes whatever DTA is
; current, uses UB_DTA, and puts the current one back - which is this
; walk's, so 4Fh carries on where it was.
;---------------------------------------------------------------------
ud_fcb:
        cld
        mov     eax,3400h               ; not from inside DOS
        call    ud_dos
        mov     ax,(C_ES SHL 8) OR C_EBX
        VXDCALL S_MAP_FLAT
        cmp     eax,-1
        je      ufc_ret
        cmp     byte ptr [eax],0
        jne     ufc_ret

        ; extended or not
        mov     esi,[ud_pg]
        add     esi,UB_FCB
        xor     ecx,ecx
        cmp     byte ptr [esi],0FFh
        jne     ufc_std
        cmp     dword ptr [ud_fcblen],7 + FCB_MIN
        jb      ufc_ret
        mov     cl,[esi+6]
        add     esi,7
ufc_std:
        and     cl,ATTR_HIDDEN OR ATTR_SYSTEM
        mov     [ud_fattr],ecx
        mov     [ud_fcbp],esi

        ; the drive, 1 = A:, and none of ours if it is a network's
        movzx   eax,byte ptr [esi]
        or      eax,eax
        jnz     ufc_drv
        mov     eax,1900h
        call    ud_dos
        movzx   eax,al
        inc     eax
ufc_drv:
        cmp     eax,26
        ja      ufc_ret
        mov     [ud_fdrive],eax
        mov     dword ptr [ebp+C_EBX],eax
        mov     eax,4409h
        call    ud_dos
        jc      ufc_ret
        test    byte ptr [ebp+C_EDX+1],10h
        jnz     ufc_ret

        ; "D:NAME????.EXT", as DOS's TransFCB makes it
        mov     edi,[ud_pg]
        add     edi,UB_PAT
        mov     eax,[ud_fdrive]
        add     al,'A' - 1
        mov     ah,':'
        stosw
        mov     esi,[ud_fcbp]
        inc     esi
        mov     ecx,8
        call    ud_fpart
        mov     esi,[ud_fcbp]
        cmp     byte ptr [esi+9],' '
        je      ufc_noext
        mov     al,'.'
        stosb
        add     esi,9
        mov     ecx,3
        call    ud_fpart
ufc_noext:
        mov     byte ptr [edi],0

        ; the walk's own DTA
        mov     eax,2F00h
        call    ud_dos
        movzx   eax,word ptr [ebp+C_ES]
        mov     [ud_fdtaseg],eax
        movzx   eax,word ptr [ebp+C_EBX]
        mov     [ud_fdtaoff],eax
        mov     edx,UB_DTA2
        mov     eax,1A00h
        call    ud_path

        mov     byte ptr [ud_fhit],0
        mov     byte ptr [ud_fro],0
        mov     byte ptr [ud_fcbal],0FFh
        mov     eax,[ud_fattr]
        mov     dword ptr [ebp+C_ECX],eax
        mov     edx,UB_PAT
        mov     eax,4E00h
        call    ud_path
        jc      ufc_end
ufc_one:
        mov     esi,[ud_pg]
        mov     al,[esi+UB_DTA2+DTA_ATTR]
        test    al,ATTR_DIR OR ATTR_VOLUME
        jnz     ufc_next

        ; "D:NAME.EXT", in the drive's current directory
        lea     edi,[esi+UB_PATH]
        mov     eax,[ud_fdrive]
        add     al,'A' - 1
        mov     ah,':'
        stosw
        lea     esi,[esi+UB_DTA2+DTA_NAME]
        mov     ecx,12
ufc_name:
        lodsb
        or      al,al
        jz      ufc_named
        stosb
        loop    ufc_name
ufc_named:
        mov     byte ptr [edi],0

        ; A read-only file is passed over, as DOS_Delete passes it over.
        ; Its name is kept, in place of the pattern the walk has done
        ; with, for the error at the end.
        mov     esi,[ud_pg]
        test    byte ptr [esi+UB_DTA2+DTA_ATTR],ATTR_RDONLY
        jz      ufc_rw
        lea     edi,[esi+UB_PAT]
        add     esi,UB_PATH
        mov     ecx,16
        rep     movsb
        mov     byte ptr [ud_fro],1
        jmp     short ufc_next
ufc_rw:
        mov     byte ptr [ud_kept],0
        call    ud_keep
        cmp     byte ptr [ud_kept],0
        jne     ufc_hit
        cmp     byte ptr [ud_c24],0
        jne     ufc_crit                ; the disk refused: see ufc_crit
        mov     edx,UB_PATH             ; not kept: deleted, as it would
        mov     eax,4100h               ; have been (our own call, so the
        call    ud_path                 ; hook lets it through)
        jnc     ufc_hit
        cmp     byte ptr [ud_c24],0
        jne     ufc_crit
        jmp     short ufc_next
ufc_hit:
        mov     byte ptr [ud_fhit],1
        mov     byte ptr [ud_fcbal],0
ufc_next:
        mov     eax,4F00h
        call    ud_dos
        jnc     ufc_one
ufc_end:
        ; NOTHING WENT: 59h is to say why, as it does after DOS's own -
        ; "access denied" if a read-only file matched, "file not found"
        ; if nothing did.  One more call that fails for that reason and
        ; changes nothing leaves DOS's own error behind: 41h on the
        ; read-only file, which DOS refuses, or 4300h on "D:?", which
        ; names no file.  (Not on the pattern: "HID.F4" from a plain FCB
        ; matches nothing, and is still a hidden file 4300h would find.)
        cmp     byte ptr [ud_fhit],0
        jne     ufc_answered
        mov     edx,UB_PAT
        mov     eax,4100h
        cmp     byte ptr [ud_fro],0
        jne     ufc_why
        mov     esi,[ud_pg]
        mov     word ptr [esi+UB_PAT+2],003Fh   ; "?" and the NUL
        mov     eax,4300h
ufc_why:
        call    ud_path
ufc_answered:
        mov     byte ptr [ud_handled],1
ufc_dtaback:
        mov     eax,[ud_fdtaseg]        ; the caller's DTA, back
        mov     ecx,eax
        mov     edx,[ud_fdtaoff]
        mov     dword ptr [ebp+C_EDX],edx
        mov     word ptr [ebp+C_DS],cx
        mov     word ptr [ebp+C_EAX],1A00h
        mov     eax,21h
        VXDCALL S_EXEC_INT
ufc_ret:
        ret

        ; THE DISK REFUSED A WRITE, and ud_i24 answered Fail for it so that
        ; nobody was asked about a file being kept.  But the program is
        ; owed the question about its own delete - MS-DOS would put it to
        ; its INT 24h handler.  If nothing has gone yet, the whole call
        ; goes on to DOS untouched (ud_handled stays 0), and DOS asks.  If
        ; something has, the answer is already AL = 0 - "one or more went"
        ; - and the rest are left where they are, as a disk that refuses
        ; would have left them.
ufc_crit:
        cmp     byte ptr [ud_fhit],0
        jne     ufc_answered
        jmp     short ufc_dtaback

;--- ud_fpart - ECX bytes at ESI to EDI, less their trailing spaces ---
ud_fpart:
        jecxz   ufp_out
        cmp     byte ptr [esi+ecx-1],' '
        jne     ufp_copy
        dec     ecx
        jmp     short ud_fpart
ufp_copy:
        rep     movsb
ufp_out:
        ret

;---------------------------------------------------------------------
; ud_names - "X:\RECYCLED" and "X:\RECYCLED\UNDELETE.DAT"
;---------------------------------------------------------------------
ud_names:
        mov     edi,[ud_pg]
        add     edi,UB_BIN
        call    ud_binpath
        mov     byte ptr [edi],0
        mov     edi,[ud_pg]
        add     edi,UB_INDEX
        call    ud_binpath
        mov     esi,OFFSET FLAT:ud_indexname
        mov     ecx,14
        rep     movsb
        ret

;--- ud_binpath - "X:\RECYCLED" at EDI, which is left past it ---------
ud_binpath:
        mov     eax,[ud_drive]
        add     al,'A'
        mov     ah,':'
        stosw
        mov     al,'\'
        stosb
        mov     esi,OFFSET FLAT:ud_binname
        mov     ecx,8
        rep     movsb
        ret

;---------------------------------------------------------------------
; ud_opendir - \RECYCLED is there and is a directory, made if need be,
;              hidden and system like the kernel's.  CF if it cannot be.
;---------------------------------------------------------------------
ud_opendir:
        mov     edx,UB_BIN
        mov     eax,4300h
        call    ud_path
        jc      uod_make
        test    byte ptr [ebp+C_ECX],ATTR_DIR
        jnz     uod_ok
        stc                             ; a FILE called RECYCLED
        ret
uod_make:
        mov     edx,UB_BIN
        mov     eax,3900h
        call    ud_path
        jc      uod_out
        mov     dword ptr [ebp+C_ECX],ATTR_KEEPDIR
        mov     edx,UB_BIN
        mov     eax,4301h
        call    ud_path
uod_ok:
        clc
uod_out:
        ret

;---------------------------------------------------------------------
; ud_openidx - UNDELETE.DAT, open read/write, made if it is not there,
;              with its header read into UB_HDR.  One that is not ours
;              is left alone: CF.
;---------------------------------------------------------------------
ud_openidx:
        mov     edx,UB_INDEX
        mov     eax,3D02h
        call    ud_path
        jnc     uoi_have
        cmp     eax,ERR_NOFILE
        stc
        jne     uoi_ret
        mov     dword ptr [ebp+C_ECX],ATTR_INDEX
        mov     edx,UB_INDEX
        mov     eax,3C00h
        call    ud_path
        jc      uoi_ret
uoi_have:
        mov     [ud_h],eax

        ; how long is it?
        xor     eax,eax
        mov     dword ptr [ebp+C_ECX],eax
        mov     dword ptr [ebp+C_EDX],eax
        mov     eax,[ud_h]
        mov     dword ptr [ebp+C_EBX],eax
        mov     eax,4202h
        call    ud_dos
        jc      uoi_ret
        cmp     word ptr [ebp+C_EDX],0
        jne     uoi_read
        cmp     eax,UDR_LEN
        jae     uoi_read

        ; A new index gets its header, a whole record long.  It is made
        ; in the record's buffer, which is not in use yet; only its first
        ; 16 bytes are read back and kept, into UB_HDR.
        mov     edi,[ud_pg]
        add     edi,UB_REC
        push    edi
        xor     eax,eax
        mov     ecx,UDR_LEN / 4
        rep     stosd
        pop     edi
        mov     esi,OFFSET FLAT:ud_magic
        mov     ecx,8
        push    edi
        rep     movsb
        pop     edi
        mov     word ptr [edi+UDH_VERSION],1
        mov     word ptr [edi+UDH_RECLEN],UDR_LEN
        mov     dword ptr [edi+UDH_NEXTSEQ],1
        xor     eax,eax
        call    ud_seek
        jc      uoi_ret
        mov     ecx,UDR_LEN
        mov     edx,UB_REC
        call    ud_write
        jc      uoi_ret
uoi_read:
        xor     eax,eax
        call    ud_seek
        jc      uoi_ret
        mov     ecx,UDH_NEXTSEQ + 4
        mov     edx,UB_HDR
        call    ud_read
        jc      uoi_ret
        mov     esi,OFFSET FLAT:ud_magic
        mov     edi,[ud_pg]
        add     edi,UB_HDR
        mov     ecx,8
        repe    cmpsb
        jne     uoi_bad
        mov     esi,[ud_pg]
        cmp     word ptr [esi+UB_HDR+UDH_RECLEN],UDR_LEN
        jne     uoi_bad
        mov     eax,[esi+UB_HDR+UDH_NEXTSEQ]
        mov     [ud_seq],eax
        clc
uoi_ret:
        ret
uoi_bad:
        stc
        ret

;---------------------------------------------------------------------
; ud_scan - the head of every record, as rcy_scan:
;   ud_bin = clusters the kept files hold, ud_old = the record of the
;   one deleted first (0 for none), ud_slot = where a new record goes.
;---------------------------------------------------------------------
ud_scan:
        xor     eax,eax
        mov     dword ptr [ebp+C_ECX],eax
        mov     dword ptr [ebp+C_EDX],eax
        mov     eax,[ud_h]
        mov     dword ptr [ebp+C_EBX],eax
        mov     eax,4202h
        call    ud_dos
        jc      usc_ret
        movzx   edx,word ptr [ebp+C_EDX]
        shl     edx,16
        or      eax,edx                 ; DX:AX, the index's length
        xor     edx,edx
        mov     ecx,UDR_LEN
        div     ecx
        mov     [ud_nrec],eax
        xor     eax,eax
        mov     [ud_bin],eax
        mov     [ud_old],eax
        mov     [ud_slot],eax
        mov     dword ptr [ud_oldseq],-1
        mov     dword ptr [ud_idx],1
        mov     eax,1
        call    ud_recpos
        jc      usc_ret
usc_batch:
        mov     ecx,[ud_nrec]
        sub     ecx,[ud_idx]
        jbe     usc_done
        cmp     ecx,SCAN_RECS
        jbe     usc_some
        mov     ecx,SCAN_RECS
usc_some:
        mov     [ud_batch],ecx
        imul    ecx,ecx,UDR_LEN
        mov     edx,UB_SCAN
        call    ud_read
        jc      usc_ret
        mov     esi,[ud_pg]
        add     esi,UB_SCAN
usc_rec:
        test    byte ptr [esi+UDR_FLAGS],UDRF_USED
        jnz     usc_used
        cmp     dword ptr [ud_slot],0
        jne     usc_next
        mov     eax,[ud_idx]
        mov     [ud_slot],eax
        jmp     short usc_next
usc_used:
        mov     eax,[esi+UDR_SIZE]
        call    ud_clus
        add     [ud_bin],eax
        mov     eax,[esi+UDR_SEQ]
        cmp     eax,[ud_oldseq]
        jae     usc_next
        mov     [ud_oldseq],eax
        mov     eax,[ud_idx]
        mov     [ud_old],eax
usc_next:
        inc     dword ptr [ud_idx]
        add     esi,UDR_LEN
        dec     dword ptr [ud_batch]
        jnz     usc_rec
        jmp     usc_batch
usc_done:
        cmp     dword ptr [ud_slot],0
        jne     usc_ok
        mov     eax,[ud_nrec]
        mov     [ud_slot],eax
usc_ok:
        clc
usc_ret:
        ret

;---------------------------------------------------------------------
; ud_purge - the kept file of record EAX goes for good, and its record
;            with it.  -> EAX = the clusters that freed, CF on an error.
;---------------------------------------------------------------------
ud_purge:
        mov     [ud_pidx],eax
        call    ud_recpos
        jc      upg_ret
        mov     ecx,UDR_STORED + 11
        mov     edx,UB_SCAN
        call    ud_read
        jc      upg_ret
        mov     esi,[ud_pg]
        add     esi,UB_SCAN + UDR_STORED
        call    ud_mkdest
        mov     edx,UB_DEST
        mov     eax,4100h
        call    ud_path
        mov     edx,0                   ; nothing freed if it was gone
        jnc     upg_freed
        cmp     eax,ERR_NOFILE
        je      upg_rec
        stc
        ret
upg_freed:
        mov     esi,[ud_pg]
        mov     eax,[esi+UB_SCAN+UDR_SIZE]
        call    ud_clus
        mov     edx,eax
upg_rec:
        push    edx
        mov     eax,[ud_pidx]
        call    ud_recpos
        jc      upg_pop
        mov     esi,[ud_pg]
        mov     byte ptr [esi+UB_SCAN+UDR_FLAGS],0
        mov     ecx,1
        mov     edx,UB_SCAN
        call    ud_write
upg_pop:
        pop     eax
upg_ret:
        ret

;---------------------------------------------------------------------
; ud_mkdest - UB_DEST = "X:\RECYCLED\NAME.EXT" from the 11 bytes at ESI
;---------------------------------------------------------------------
ud_mkdest:
        push    esi
        mov     edi,[ud_pg]
        add     edi,UB_DEST
        call    ud_binpath
        pop     esi
        mov     al,'\'
        stosb
        mov     ecx,8
umd_base:
        lodsb
        cmp     al,' '
        je      umd_bsk
        stosb
umd_bsk:
        loop    umd_base
        cmp     byte ptr [esi],' '
        je      umd_end
        mov     al,'.'
        stosb
        mov     ecx,3
umd_ext:
        lodsb
        cmp     al,' '
        je      umd_esk
        stosb
umd_esk:
        loop    umd_ext
umd_end:
        xor     al,al
        stosb
        ret

;---------------------------------------------------------------------
; ud_mkstored - the record's stored name from [ud_seq] and the file's
;               extension: 'D', seven hex digits, the extension.  Into
;               UDR_SEQ, UDR_STORED and UB_DEST.
;---------------------------------------------------------------------
ud_mkstored:
        mov     ebx,[ud_pg]
        lea     edi,[ebx+UB_REC+UDR_STORED]
        mov     edx,[ud_seq]
        mov     [ebx+UB_REC+UDR_SEQ],edx
        mov     al,'D'
        stosb
        shl     edx,4                   ; seven digits: the top nibble goes
        mov     ecx,7
ums_dig:
        rol     edx,4
        mov     al,dl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ums_put
        add     al,'A' - '9' - 1
ums_put:
        stosb
        loop    ums_dig

        mov     eax,20202020h           ; the extension, space-padded
        mov     esi,[ud_extp]
        or      esi,esi
        jz      ums_noext
        mov     ecx,3
        push    edi
ums_ext:
        mov     al,[esi]
        or      al,al
        jz      ums_extend
        mov     [edi],al
        inc     esi
        inc     edi
        loop    ums_ext
ums_extend:
        jecxz   ums_extdone
ums_pad:
        mov     byte ptr [edi],' '
        inc     edi
        loop    ums_pad
ums_extdone:
        pop     edi
        jmp     short ums_named
ums_noext:
        mov     [edi],ax
        mov     [edi+2],al
ums_named:
        lea     esi,[ebx+UB_REC+UDR_STORED]
        jmp     ud_mkdest

;---------------------------------------------------------------------
; ud_record - everything in the record except its number and stored
;             name: flags, attributes, size, when, the 8.3 name, the
;             directory, and the long name if the file has one.
;---------------------------------------------------------------------
ud_record:
        mov     edi,[ud_pg]
        add     edi,UB_REC
        xor     eax,eax
        mov     ecx,UDR_LEN / 4
        rep     stosd

        mov     ebx,[ud_pg]
        mov     byte ptr [ebx+UB_REC+UDR_FLAGS],UDRF_USED
        mov     al,[ebx+UB_DTA+DTA_ATTR]
        mov     [ebx+UB_REC+UDR_ATTR],al
        mov     eax,[ebx+UB_DTA+DTA_SIZE]
        mov     [ebx+UB_REC+UDR_SIZE],eax

        ; the name the find gave, 8.3 with its dot
        lea     esi,[ebx+UB_DTA+DTA_NAME]
        lea     edi,[ebx+UB_REC+UDR_SHORT]
        mov     ecx,12
urc_short:
        lodsb
        or      al,al
        jz      urc_shdone
        stosb
        loop    urc_short
urc_shdone:

        ; "\DIR\SUB": what is between the drive and the last part, and
        ; "\" alone for the root
        lea     esi,[ebx+UB_TRUE+2]
        lea     edi,[ebx+UB_REC+UDR_DIR]
        mov     ecx,[ud_namep]
        dec     ecx                     ; -> the '\' in front of the name
        sub     ecx,esi
        jnz     urc_dir
        mov     ecx,1                   ; the root: keep its one '\'
urc_dir:
        cmp     ecx,UDR_DIRMAX - 1
        jae     urc_bad
        rep     movsb

        ; when: DOS's own date and time
        mov     eax,2C00h
        call    ud_dos
        movzx   eax,byte ptr [ebp+C_ECX+1]      ; hours
        shl     eax,6
        movzx   ecx,byte ptr [ebp+C_ECX]        ; minutes
        or      eax,ecx
        shl     eax,5
        movzx   ecx,byte ptr [ebp+C_EDX+1]      ; seconds
        shr     ecx,1
        or      eax,ecx
        mov     ebx,[ud_pg]
        mov     [ebx+UB_REC+UDR_DTIME],ax
        mov     eax,2A00h
        call    ud_dos
        movzx   eax,word ptr [ebp+C_ECX]
        sub     eax,1980
        shl     eax,4
        movzx   ecx,byte ptr [ebp+C_EDX+1]      ; month
        or      eax,ecx
        shl     eax,5
        movzx   ecx,byte ptr [ebp+C_EDX]        ; day
        or      eax,ecx
        mov     ebx,[ud_pg]
        mov     [ebx+UB_REC+UDR_DDATE],ax

        ; The long name, which the rename is about to lose.  RMDOS reads
        ; it off the disk; a DOS without the call leaves the field empty.
        mov     dword ptr [ebp+C_EDI],UB_REC + UDR_LONG
        mov     edx,UB_TRUE
        mov     eax,0F12Ah
        call    ud_path
        mov     ebx,[ud_pg]
        jnc     urc_long
        mov     byte ptr [ebx+UB_REC+UDR_LONG],0 ; pieces of one, if any
urc_long:
        mov     byte ptr [ebx+UB_REC+UDR_LONG+UDR_LONGMAX-1],0
        clc
        ret
urc_bad:
        stc
        ret
_LTEXT  ENDS

;=====================================================================
; INIT CODE - goes when Windows has started
;=====================================================================
_ITEXT  SEGMENT
        ASSUME  ds:FLAT, es:FLAT, ss:FLAT

;---------------------------------------------------------------------
; ud_devinit - nothing is hooked unless this is PM-DOS with UNDELETE=ON.
;              On another DOS, or with UNDELETE=OFF, the driver sits
;              here doing nothing, and Windows starts as usual.
;---------------------------------------------------------------------
ud_devinit:
        pushad
        mov     eax,KL_BRGSIG
        cmp     dword ptr [eax],BRG_SIGVAL
        jne     udi_out
        mov     eax,KL_UNDEL
        cmp     byte ptr [eax],0
        je      udi_out
        mov     byte ptr [ud_ok],1

        xor     edx,edx                 ; protected mode's way in
        mov     esi,OFFSET FLAT:ud_pm21
        VXDCALL S_ALLOC_PMCB
        jc      udi_out
        mov     [ud_pmcb],eax
udi_out:
        popad
        clc
        ret

;---------------------------------------------------------------------
; ud_hookall - Init_Complete.  THE HOOKS GO IN NOW, NOT AT DEVICE_INIT.
;
; The V86 chain runs the last hook first, and IFSMGR puts its own in
; front of INT 21h after Device_Init, so a hook made then never sees a
; delete that 32-bit file access takes.  At Init_Complete every device
; has initialized and this driver, with no init order, is called last.
;
; The protected-mode vector goes in now as well: set before Sys_VM_Init,
; it is the default for every VM.  What was there - IFSMGR's, or the
; VMM's own reflection to V86 - is where every other call goes on to.
;
; THE LOW BLOCK is taken now too, and by the paragraph, not the page.
; It comes out of every VM's conventional memory, and the global V86
; data area is handed out in pages that small blocks share: taken last,
; as VNETBIOS takes its buffers, it goes into room another driver's page
; already has, where a page of its own cost every DOS box 4K.
;---------------------------------------------------------------------
ud_hookall:
        cmp     byte ptr [ud_ok],0
        je      uha_out
        pushad
        push    GVDA_PARAALIGN OR GVDA_ZEROINIT
        push    UB_SIZE
        VXDCALL S_ALLOC_GV86
        add     esp,8
        or      eax,eax
        jz      uha_none
        cmp     eax,100000h - UB_SIZE
        ja      uha_none
        mov     [ud_pg],eax
        shr     eax,4
        mov     [ud_seg],eax

        mov     eax,21h
        mov     esi,OFFSET FLAT:ud_i21
        VXDCALL S_HOOK_V86
        mov     eax,24h
        mov     esi,OFFSET FLAT:ud_i24
        VXDCALL S_HOOK_V86

        cmp     dword ptr [ud_pmcb],0
        je      uha_nopm
        mov     eax,21h
        VXDCALL S_GET_PMVEC
        movzx   ecx,cx
        mov     [ud_pmoldcs],ecx
        mov     [ud_pmoldip],edx
        mov     edx,[ud_pmcb]
        mov     ecx,edx
        shr     ecx,16
        movzx   edx,dx
        mov     eax,21h
        VXDCALL S_SET_PMVEC
uha_nopm:
        popad
        mov     byte ptr [ud_on],1
uha_out:
        clc
        ret
uha_none:
        popad                           ; no block: nothing hooked, and
        clc                             ; Windows starts as usual
        ret
_ITEXT  ENDS

        END
