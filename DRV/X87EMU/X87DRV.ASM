;=====================================================================
; X87DRV.ASM - X87EMU.SYS, the coprocessor emulator, as a 32-bit driver
;
;   DEVICE=X87EMU.SYS
;
; A 386 without a 387 and a 486SX run no floating-point instruction at
; all.  With this driver loaded, a native program's are carried out by
; the emulator in X87EMU.C, X87OPS.C, X87FN.C and X87NUM.C (X87EMU.H
; says how); DOS boxes and DPMI clients are left to the hardware and
; their own emulators, as they always were (IO\FPU.INC).
;
; THIS FILE IS THE DRIVER AROUND THE C: the D32 header, the driver
; table, INIT, and fpu_uok, which the C calls by name.  INIT hands the
; kernel the C's two entries (NDS_FPUEMU, which is FPU_SETEMU in
; IO\FPU.INC); from then on PMIO calls x87_trap on every coprocessor
; instruction a native program executes, and x87_init when a program
; is to start with a fresh coprocessor.  The kernel keeps the state
; (fpu_emst), so the driver has nothing to do at a park or a restart.
;
; On a machine WITH a coprocessor the kernel says no and INIT refuses,
; WITHOUT A WORD, so the 20K is given back and the screen says nothing:
; a CONFIG.SYS that names this driver can be used on any machine, and
; the ones PM-DOS ships and Setup writes all do.  The x87emu kernel (BUILD x87emu) reports no
; coprocessor whatever is fitted, which is how the emulator is tested.
;
; It was linked into PMIO.SYS for a day, and took PMIO's protected-mode
; half from 81K to 101K of the 110K the stub then copied up; most
; machines have a coprocessor and never run a line of it.
;
; Linked with:  wlink form raw bin option offset=0x0, and again with
; offset=0x10000000; TOOLS\D32DRV.EXE makes the two into X87EMU.SYS.
; This object comes first, so the header is at the start of the image.
;=====================================================================
        .386P
        INCLUDE PMDOS.INC
        INCLUDE NDRV.INC

        EXTRN   x87_trap:NEAR           ; X87EMU.C: the kernel calls these
        EXTRN   x87_init:NEAR
        PUBLIC  fpu_uok                 ; ...and the C calls this

_X87    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_X87, DS:_X87, ES:_X87, SS:_X87
        ORG     0

image:
        D32_DRVHEAD <OFFSET drvtab>, 0

;--- the driver table: INIT and nothing else --------------------------
drvtab:
        dd      NDT_SIGVAL
        dw      NDT_VERSION
        dw      0                       ; none of the kinds NDTF_ names
        db      'X87EMU  '
        dd      OFFSET xe_init
        dd      0                       ; PARK: the state is the kernel's
        dd      0                       ; HALT: nothing is running
        dd      0                       ; CALL: nothing to ask
IF ($ - drvtab) NE NDT_LEN
        .ERR    <X87EMU: the driver table is not NDT_LEN bytes>
ENDIF

;=====================================================================
; fpu_uok - the kernel's, through the address NDS_FPUEMU put in uok_vec:
;           EAX = a linear address, ECX = a length -> EAX = 1 if the
;           program could write all of it, 0 if not.  Nothing else is
;           changed.  X87EMU.H declares it to the C.
;=====================================================================
fpu_uok:
        jmp     dword ptr [uok_vec]

;=====================================================================
; INIT - ESI -> the services, EDI -> the row, EDX -> the line.  The
;        line says nothing this driver needs.
;
; NDS_FPUEMU IS THE LAST THING ASKED: once the kernel has the entries
; it calls them for good, so nothing may refuse after it says yes.  The
; kernel writes uok_vec itself, before it lets x87_trap be called.
;=====================================================================
xe_init:
        mov     [services],esi
        cmp     word ptr [esi+NDS_VER],6        ; NDS_VERSION 6 brought it
        jb      xi_old
        mov     edx,OFFSET x87_trap
        mov     ecx,OFFSET x87_init
        mov     ebx,OFFSET uok_vec
        call    dword ptr [esi+NDS_FPUEMU]
        jc      xi_no
        ret                             ; CF clear: installed

xi_no:
        cmp     eax,FSE_X87             ; a coprocessor: not needed, and
        je      xi_quiet                ; nothing is said (see above)
        mov     esi,OFFSET msg_have
        cmp     eax,FSE_HAVE
        je      xi_say
xi_old:
        mov     esi,OFFSET msg_old
xi_say:
        mov     edx,[services]
        call    dword ptr [edx+NDS_PRINT]
        stc
        ret
xi_quiet:
        mov     eax,NDI_QUIET           ; refused, and the kernel's
        stc                             ; message is not wanted either
        ret

;=====================================================================
; Messages and state
;=====================================================================
msg_have   db  'X87EMU.SYS is already loaded',13,10,0
msg_old     db  'X87EMU.SYS needs a newer PM-DOS kernel',13,10,0

services    dd  0
uok_vec     dd  0                       ; the kernel's fpu_uok, which
                                        ; the kernel writes here

_X87    ENDS
        END
