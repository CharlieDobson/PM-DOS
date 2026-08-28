;=====================================================================
; PCI.INC - configuration space, mechanism 1
;
; The smallest thing that deserves to be called PCI support: find a
; device by what it IS, read and write its configuration registers,
; and nothing else.  There is no resource assignment here and there
; never will be - the BIOS did that at POST, and a DOS that started
; moving devices around would be taking a decision it has no way to
; get right.
;
; WHAT IT IS FOR, TODAY, IS THE BUS-MASTER IDE REGISTERS.  A PCI IDE
; controller keeps its command block where it always was, at 1F0h and
; 170h, but its DMA engine is at an address only configuration space
; knows - BAR4 - so ATA.INC cannot find it by probing.  That is the
; whole reason this file exists, and it is written as a general
; facility rather than four lines inside ATA.INC because the next two
; things that want it (ATAPI, and anything on the network side) will
; want exactly this and nothing more.
;
; MECHANISM 1 ONLY.  There were two ways of reaching configuration
; space in the early days and the other one - mechanism 2, a window
; at C000h-CFFFh - was gone from new chipsets by 1994 and deprecated
; by the specification itself.  A machine that has only mechanism 2
; gets the same answer here as a machine with no PCI at all, which is
; "no bus master", which is PIO, which is what it was doing anyway.
;
; BUS 0 ONLY, and that is a decision rather than an omission: PM-DOS
; probes exactly two ATA channels, at the two legacy addresses, and a
; controller behind a PCI-PCI bridge is not going to be answering at
; 1F0h.  If the drives were found by the legacy probe then the
; controller is on the root bus.
;
; Included by IOPM.ASM, before ATA.INC.
;=====================================================================

PCI_CFG_ADDR    EQU     0CF8h
PCI_CFG_DATA    EQU     0CFCh

; Configuration registers, by offset.  A read is always the whole
; dword the port gives, so these are all multiples of four and the
; caller shifts out the field it wants.
PCIR_VENDOR     EQU     00h             ; vendor:device
PCIR_COMMAND    EQU     04h             ; command:status
PCIR_CLASS      EQU     08h             ; revision, prog-if, subclass, class
PCIR_HDRTYPE    EQU     0Ch             ; cache line, latency, header, BIST
PCIR_BAR0       EQU     10h
PCIR_BAR4       EQU     20h

PCIC_IOEN       EQU     0001h           ; command: respond to I/O cycles
PCIC_MEMEN      EQU     0002h           ; ...and to memory ones
PCIC_BUSMASTER  EQU     0004h           ; ...and may drive the bus itself

PCI_MULTIFN     EQU     00800000h       ; header type bit 7, in the dword

;=====================================================================
; pci_init - is there a mechanism 1 at all?
;
; The test the specification gives: write the enable bit to the
; address port and read it back.  A machine with no PCI has nothing
; decoding CF8h, so the read comes back as a floating bus or as
; whatever some other port left there, and either way it is not what
; was written.  The pair has to be written and read without an
; interrupt in between, because anything else that touched CF8h in
; the gap would be the thing answered.
;=====================================================================
pci_init:
        pushad
        mov     byte ptr [pci_ok],0
        pushfd
        cli
        mov     eax,80000000h
        mov     dx,PCI_CFG_ADDR
        out     dx,eax
        in      eax,dx
        popfd
        cmp     eax,80000000h
        jne     pini_none
        mov     byte ptr [pci_ok],1
pini_none:
        popad
        ret

;=====================================================================
; pci_read - EBX = device, AL = register offset -> EAX = the dword.
;
; A device is carried as the packed address field the port wants,
; less the enable bit: bus in 23:16, device in 15:11, function in
; 10:8.  Passing it around in that form means the caller never has to
; take it apart and put it back together.
;=====================================================================
pci_read:
        push    ecx
        push    edx
        movzx   ecx,al
        and     ecx,0FCh
        or      ecx,ebx
        or      ecx,80000000h
        pushfd
        cli
        mov     eax,ecx
        mov     dx,PCI_CFG_ADDR
        out     dx,eax
        mov     dx,PCI_CFG_DATA
        in      eax,dx
        popfd
        pop     edx
        pop     ecx
        ret

;=====================================================================
; pci_write - EBX = device, AL = register offset, ECX = the dword.
;=====================================================================
pci_write:
        push    eax
        push    ecx
        push    edx
        movzx   eax,al
        and     eax,0FCh
        or      eax,ebx
        or      eax,80000000h
        pushfd
        cli
        mov     dx,PCI_CFG_ADDR
        out     dx,eax
        mov     eax,ecx
        mov     dx,PCI_CFG_DATA
        out     dx,eax
        popfd
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; pci_find - CH = class, CL = subclass.  -> EBX = the first device on
;            bus 0 that matches, CF clear.
;
; THE FIRST ONE, and there is no way to ask for the second: a machine
; with two IDE controllers has four channels and this kernel probes
; two.  When there is a caller that can use a second device this grows
; a resume argument; inventing one now would be inventing a caller.
;
; Functions 1 to 7 are only looked at when function 0 says it is a
; multi-function device, which is the rule - a single-function device
; is entitled to answer on every function and would otherwise be
; found eight times.
;=====================================================================
pci_find:
        push    eax
        push    ecx
        push    edx
        push    esi
        push    edi
        cmp     byte ptr [pci_ok],0
        je      pf_none
        movzx   esi,cx                  ; ESI = class:subclass wanted
        xor     edx,edx                 ; EDX = device number
pf_dev:
        cmp     edx,32
        jae     pf_none
        xor     edi,edi                 ; EDI = function number
pf_fn:
        mov     ebx,edx
        shl     ebx,11
        mov     ecx,edi
        shl     ecx,8
        or      ebx,ecx

        mov     al,PCIR_VENDOR
        call    pci_read
        cmp     ax,0FFFFh               ; nothing answering
        jne     pf_live
        or      edi,edi
        jz      pf_nextdev              ; and nothing at function 0 means
        jmp     short pf_nextfn         ; the socket is empty
pf_live:
        mov     al,PCIR_CLASS
        call    pci_read
        shr     eax,16                  ; AX = class:subclass
        cmp     ax,si
        je      pf_hit

        or      edi,edi                 ; only function 0 can say whether
        jnz     pf_nextfn               ; there are others
        mov     al,PCIR_HDRTYPE
        call    pci_read
        test    eax,PCI_MULTIFN
        jz      pf_nextdev
pf_nextfn:
        inc     edi
        cmp     edi,8
        jb      pf_fn
pf_nextdev:
        inc     edx
        jmp     short pf_dev

pf_hit:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        clc
        ret
pf_none:
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        stc
        ret

;=====================================================================
; State
;=====================================================================
pci_ok      db  0                       ; mechanism 1 answered
            db  0
