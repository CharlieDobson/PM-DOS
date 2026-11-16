;=====================================================================
; PCI.INC - configuration space, mechanisms 1 and 2
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
; BOTH MECHANISMS.  There were two ways of reaching configuration space
; in the early days.  Mechanism 1 is an address at CF8h and the data at
; CFCh, and is what every board since about 1995 has.  Mechanism 2 is
; a window: CF8h names a function and opens the window, CFAh names the
; bus, and the window at C000h-CFFFh is 256 ports for each of sixteen
; devices.  The specification gave mechanism 2 up, but many 1993-94
; Pentium and PCI 486 boards use nothing else, the owner's VM's board
; (86Box's pb520r) among them.  This file used to see no PCI at all on
; such a board.
;
; Mechanism 1 is asked first and used if it answers.  Some chipsets
; can do both, with a register at CFBh choosing between them; the
; choice is left as the BIOS made it, because the BIOS's own PCI calls,
; which DOS programs and Windows make, use that mechanism and no other.
;
; A DEVICE IS CARRIED THE SAME WAY FOR BOTH (pci_read, below), so
; nothing outside this file knows which mechanism is in use.  Under
; mechanism 2 there is no device 16 to 31: one reads as an empty socket
; and is never written.
;
; BUS 0 ONLY - FOR pci_find, and that is a decision rather than an
; omission: PM-DOS probes exactly two ATA channels, at the two legacy
; addresses, and a controller behind a PCI-PCI bridge is not going to
; be answering at 1F0h.  If the drives were found by the legacy probe
; then the controller is on the root bus.
;
; EVERYTHING ELSE SEES EVERY BUS.  pci_scan walks the machine once, at
; boot, following each bridge to the buses behind it, and keeps what it
; found in a table (pci_table) that the searches read - by position, by
; class, by vendor and device, each with an index so that the second
; card of a kind can be asked for as well as the first.  That is the
; part the first version of this file said would come with its first
; caller: an AGP card is on bus 1, and a machine with two network cards
; has a second one.
;
; STILL NO RESOURCE ASSIGNMENT.  A base address register is read and
; measured (pcx_bar) and never moved.
;
; Included by IOPM.ASM, before ATA.INC.
;=====================================================================

PCI_CFG_ADDR    EQU     0CF8h
PCI_CFG_DATA    EQU     0CFCh
PCI_CSE         EQU     0CF8h                   ; mechanism 2: the enable
                                                ; register, a byte - key in
                                                ; 7:4, function in 3:1
PCI_FORWARD     EQU     0CFAh                   ; mechanism 2: the bus
PCI_WINDOW      EQU     0C000h                  ; mechanism 2: a device to
                                                ; each 256 ports
PCI_M2KEY       EQU     0F0h                    ; any key but 0 opens it

PCI_MAXFN       EQU     64                      ; functions pci_scan keeps
PCT_ADDR        EQU     0                       ; dd - bus, device, function
PCT_ID          EQU     4                       ; dd - device:vendor
PCT_CLASS       EQU     8                       ; dd - class, subclass,
                                                ;      prog-if, revision
PCT_LEN         EQU     12
PCIR_BUSES      EQU     18h                     ; a bridge's: primary,
                                                ; secondary, subordinate
PCIR_INTR       EQU     3Ch                     ; line, pin, and two timers

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
; pci_init - is there a mechanism 1, or failing that a mechanism 2?
;
; Mechanism 1 by the test the specification gives: write the enable
; bit to the address port and read it back.  A machine with no PCI has
; nothing decoding CF8h, so the read comes back as a floating bus or
; as whatever some other port left there, and either way it is not
; what was written.  The pair has to be written and read without an
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
        jne     pini_two
        mov     byte ptr [pci_ok],1
        jmp     short pini_scan
pini_two:
        call    pci_try2
pini_scan:
        call    pci_scan
        popad
        ret

;=====================================================================
; pci_try2 - is there a mechanism 2?  Asked only when mechanism 1 did
;            not answer, so on every 386 and 486 without PCI as well.
;            -> pci_ok = 2 if there is.
;
; ITS TWO REGISTERS ARE ASKED FIRST, AND THE WINDOW ONLY AFTER.  The
; enable register (CF8h) and the bus register (CFAh) are bytes that
; keep what is written to them.  Both are written with zeros and read
; back, then the bus register with a pattern - harmless, with the
; enable register at zero nothing is open.  A machine without PCI
; reads back a floating bus and stops there.  That order matters
; because on a plain ISA machine the window's ports need not be empty:
; anything that decodes only ten bits of address answers C008h as
; 008h, and on an AT that can be the DMA controller's status register,
; which a read clears.
;
; Then the window is opened and bus 0 looked at.  The answer is yes
; only if something there is plainly part of a PCI machine: a host
; bridge, a VGA card, or a device made by Intel or Compaq.  Linux asks
; the same before it uses either mechanism.
;=====================================================================
pci_try2:
        pushfd
        cli
        xor     al,al
        mov     dx,PCI_CSE
        out     dx,al
        mov     dx,PCI_FORWARD
        out     dx,al
        mov     dx,PCI_CSE
        in      al,dx
        mov     ah,al
        mov     dx,PCI_FORWARD
        in      al,dx
        or      al,ah
        jnz     pt2_none                ; DX = PCI_FORWARD, from here
        mov     al,5Ah
        out     dx,al
        in      al,dx
        mov     ah,al
        xor     al,al
        out     dx,al
        cmp     ah,5Ah
        jne     pt2_none
        popfd

        mov     byte ptr [pci_ok],2     ; so that pci_read uses the window
        xor     ebx,ebx                 ; bus 0, device 0, function 0
pt2_dev:
        mov     al,PCIR_VENDOR
        call    pci_read
        cmp     ax,0FFFFh
        je      pt2_next
        cmp     ax,8086h                ; Intel
        je      pt2_yes
        cmp     ax,0E11h                ; Compaq
        je      pt2_yes
        mov     al,PCIR_CLASS
        call    pci_read
        shr     eax,16                  ; AX = class:subclass
        cmp     ax,0600h                ; a host bridge
        je      pt2_yes
        cmp     ax,0300h                ; a VGA card
        je      pt2_yes
pt2_next:
        add     ebx,1 SHL 11            ; the next device, function 0
        cmp     ebx,16 SHL 11
        jb      pt2_dev
        mov     byte ptr [pci_ok],0
pt2_yes:
        ret
pt2_none:
        popfd
        ret

;=====================================================================
; pci_read - EBX = device, AL = register offset -> EAX = the dword.
;
; A device is carried as the packed address field the port wants,
; less the enable bit: bus in 23:16, device in 15:11, function in
; 10:8.  Passing it around in that form means the caller never has to
; take it apart and put it back together.
;
; THE ADDRESS PORT IS PUT BACK AS IT WAS FOUND.  A DOS box reaches
; configuration space through the same two ports, in two instructions,
; and an interrupt can arrive between them; so can a call from one.
; Whatever ran in the gap used to leave its own address behind, and
; the box's read then answered for a device it had not named.  Under
; mechanism 2 the same goes for both of its registers (pm2_open).
;=====================================================================
pci_read:
        cmp     byte ptr [pci_ok],2
        je      pci_read2
        push    ecx
        push    edx
        movzx   ecx,al
        and     ecx,0FCh
        or      ecx,ebx
        or      ecx,80000000h
        pushfd
        cli
        mov     dx,PCI_CFG_ADDR
        in      eax,dx
        push    eax
        mov     eax,ecx
        out     dx,eax
        mov     dx,PCI_CFG_DATA
        in      eax,dx
        mov     ecx,eax
        pop     eax
        mov     dx,PCI_CFG_ADDR
        out     dx,eax
        mov     eax,ecx
        popfd
        pop     edx
        pop     ecx
        ret

;--- the same through mechanism 2's window
pci_read2:
        push    ecx
        push    edx
        pushfd
        cli
        call    pm2_open
        jc      prd2_none
        and     dl,0FCh                 ; the whole dword
        in      eax,dx
        call    pm2_close
        popfd
        pop     edx
        pop     ecx
        ret
prd2_none:
        or      eax,-1                  ; what an empty socket reads as
        popfd
        pop     edx
        pop     ecx
        ret

;=====================================================================
; pci_write - EBX = device, AL = register offset, ECX = the dword.
;=====================================================================
pci_write:
        cmp     byte ptr [pci_ok],2
        je      pci_write2
        push    eax
        push    ecx
        push    edx
        push    esi
        movzx   esi,al
        and     esi,0FCh
        or      esi,ebx
        or      esi,80000000h
        pushfd
        cli
        mov     dx,PCI_CFG_ADDR
        in      eax,dx
        push    eax
        mov     eax,esi
        out     dx,eax
        mov     eax,ecx
        mov     dx,PCI_CFG_DATA
        out     dx,eax
        pop     eax
        mov     dx,PCI_CFG_ADDR
        out     dx,eax
        popfd
        pop     esi
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- the same through mechanism 2's window
pci_write2:
        push    ecx
        push    edx
        push    esi
        mov     esi,ecx                 ; ESI = what to write
        pushfd
        cli
        call    pm2_open
        jc      pwr2_none
        and     dl,0FCh
        xchg    eax,esi
        out     dx,eax
        xchg    eax,esi
        call    pm2_close
pwr2_none:
        popfd
        pop     esi
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; pm2_open - mechanism 2, interrupts off.  EBX = device, AL = register.
;            -> DX = that register's port in the window, CH and CL =
;            the enable and bus registers as they were found, for
;            pm2_close.  CF, and nothing touched, if the device is one
;            the window has no room for.
;
; THE WINDOW HAS SIXTEEN DEVICES.  Four bits of the port name the
; device, so device 16 would land on device 0.  A device number of 16
; or more reads as an empty socket and is never written.
;
; BOTH REGISTERS ARE PUT BACK AS THEY WERE FOUND (pm2_close), for the
; reason pci_read gives: a DOS box, or the BIOS's PCI call inside one,
; may be between opening the window and using it.
;---------------------------------------------------------------------
pm2_open:
        test    ebx,8000h               ; bit 4 of the device
        jnz     pm2_none
        push    eax
        movzx   edx,al                  ; the register, to the byte
        mov     eax,ebx
        shr     eax,3                   ; the device, from 15:11 to 11:8
        and     eax,0F00h
        or      edx,eax
        or      edx,PCI_WINDOW
        push    edx
        mov     dx,PCI_CSE
        in      al,dx
        mov     ch,al
        mov     dx,PCI_FORWARD
        in      al,dx
        mov     cl,al
        mov     eax,ebx
        shr     eax,7                   ; the function, from 10:8 to 3:1
        and     al,0Eh
        or      al,PCI_M2KEY
        mov     dx,PCI_CSE
        out     dx,al
        mov     eax,ebx
        shr     eax,16                  ; the bus
        mov     dx,PCI_FORWARD
        out     dx,al
        pop     edx
        pop     eax
        clc
        ret
pm2_none:
        stc
        ret

;--- pm2_close - CH and CL as pm2_open gave them
pm2_close:
        push    eax
        push    edx
        mov     al,cl
        mov     dx,PCI_FORWARD
        out     dx,al
        mov     al,ch
        mov     dx,PCI_CSE
        out     dx,al
        pop     edx
        pop     eax
        ret

;=====================================================================
; pcx_rd - EBX = device, DL = register, DH = 1, 2 or 4 bytes -> EAX.
;          CF if that many bytes do not fit at that offset.
;
; A register is a byte, a word or a dword at any offset that holds it,
; and the port gives dwords: the whole one is read and the part asked
; for is cut out of it.
;=====================================================================
pcx_rd:
        push    ecx
        call    pcx_fits
        jc      pxr_no
        mov     al,dl
        call    pci_read
        mov     cl,dl
        and     cl,3
        shl     cl,3
        shr     eax,cl
        cmp     dh,4
        je      pxr_ok
        cmp     dh,2
        je      pxr_word
        movzx   eax,al
        jmp     short pxr_ok
pxr_word:
        movzx   eax,ax
pxr_ok:
        pop     ecx
        clc
        ret
pxr_no:
        pop     ecx
        stc
        ret

;=====================================================================
; pcx_wr - the same, and ECX = what to write.
;
; NOT read, change and write back.  The status register, and others
; like it, clear a bit when a ONE is written to it, so writing back
; what was read wipes every flag that happened to be set.  The data
; port is four ports, one a byte of the register, and a write of the
; right width to the right one of them touches nothing else.  The
; window of mechanism 2 is a port for every byte of every register, so
; there the register's own port is written.
;=====================================================================
pcx_wr:
        push    eax
        push    edx
        push    esi
        call    pcx_fits
        jc      pxw_no
        cmp     byte ptr [pci_ok],2
        je      pxw_two
        movzx   esi,dl
        and     esi,0FCh
        or      esi,ebx
        or      esi,80000000h
        pushfd
        cli
        push    edx
        mov     dx,PCI_CFG_ADDR
        in      eax,dx
        xchg    eax,esi                 ; ESI = what was there
        out     dx,eax
        pop     edx
        push    edx
        mov     al,dl
        and     eax,3
        mov     dl,dh                   ; DL = the width
        add     eax,PCI_CFG_DATA
        xchg    eax,edx                 ; DX = the port, AL = the width
        cmp     al,4
        je      pxw_dword
        cmp     al,2
        je      pxw_word
        mov     al,cl
        out     dx,al
        jmp     short pxw_done
pxw_word:
        mov     ax,cx
        out     dx,ax
        jmp     short pxw_done
pxw_dword:
        mov     eax,ecx
        out     dx,eax
pxw_done:
        pop     edx
        mov     eax,esi
        push    edx
        mov     dx,PCI_CFG_ADDR
        out     dx,eax
        pop     edx
        popfd
        pop     esi
        pop     edx
        pop     eax
        clc
        ret
pxw_two:
        push    ecx
        mov     esi,ecx                 ; ESI = what to write
        mov     al,dl                   ; AL = the register,
        mov     ah,dh                   ; AH = the width
        pushfd
        cli
        call    pm2_open                ; -> DX = the register's own port
        jc      pxw2_none
        cmp     ah,4
        je      pxw2_dword
        cmp     ah,2
        je      pxw2_word
        mov     eax,esi
        out     dx,al
        jmp     short pxw2_done
pxw2_word:
        mov     eax,esi
        out     dx,ax
        jmp     short pxw2_done
pxw2_dword:
        mov     eax,esi
        out     dx,eax
pxw2_done:
        call    pm2_close
pxw2_none:
        popfd
        pop     ecx
        pop     esi
        pop     edx
        pop     eax
        clc
        ret
pxw_no:
        pop     esi
        pop     edx
        pop     eax
        stc
        ret

;--- pcx_fits - DL = offset, DH = width.  CF if it is not 1, 2 or 4
;--- bytes, or runs out of the dword it starts in.
pcx_fits:
        push    eax
        mov     al,dl
        and     al,3
        cmp     dh,1
        je      pxf_ok
        cmp     dh,2
        jne     pxf_four
        cmp     al,2
        jbe     pxf_ok
        jmp     short pxf_no
pxf_four:
        cmp     dh,4
        jne     pxf_no
        or      al,al
        jnz     pxf_no
pxf_ok:
        pop     eax
        clc
        ret
pxf_no:
        pop     eax
        stc
        ret

;=====================================================================
; pci_scan - what is on every bus, into pci_table.  Once, at boot.
;
; Bus 0 is walked first.  A bridge on it says which buses are behind it
; - the highest of them is its subordinate number - and the walk goes
; on until it has done the highest bus any bridge it met has named.
; The numbers are the BIOS's: it assigned them at POST, and a bridge it
; left unnumbered leads to a bus nothing can be reached on anyway.
;=====================================================================
pci_scan:
        pushad
        mov     dword ptr [pci_count],0
        mov     dword ptr [pci_lastbus],0
        cmp     byte ptr [pci_ok],0
        je      pcs_done
        xor     esi,esi                 ; ESI = bus
pcs_bus:
        xor     edx,edx                 ; EDX = device
pcs_dev:
        xor     edi,edi                 ; EDI = function
pcs_fn:
        mov     ebx,esi
        shl     ebx,16
        mov     eax,edx
        shl     eax,11
        or      ebx,eax
        mov     eax,edi
        shl     eax,8
        or      ebx,eax

        mov     al,PCIR_VENDOR
        call    pci_read
        cmp     ax,0FFFFh
        jne     pcs_live
        or      edi,edi
        jz      pcs_nextdev
        jmp     pcs_nextfn
pcs_live:
        mov     ebp,eax
        mov     al,PCIR_CLASS
        call    pci_read
        mov     ecx,dword ptr [pci_count]
        cmp     ecx,PCI_MAXFN
        jae     pcs_full
        imul    ecx,ecx,PCT_LEN
        add     ecx,OFFSET pci_table
        mov     dword ptr [ecx+PCT_ADDR],ebx
        mov     dword ptr [ecx+PCT_ID],ebp
        mov     dword ptr [ecx+PCT_CLASS],eax
        inc     dword ptr [pci_count]
pcs_full:
        mov     al,PCIR_HDRTYPE
        call    pci_read
        mov     ecx,eax
        shr     eax,16
        and     al,7Fh
        cmp     al,1                    ; a PCI-PCI bridge
        je      pcs_bridge
        cmp     al,2                    ; a CardBus one keeps its bus
        jne     pcs_leaf                ; numbers in the same place
pcs_bridge:
        mov     al,PCIR_BUSES
        call    pci_read
        shr     eax,16
        movzx   eax,al
        cmp     eax,dword ptr [pci_lastbus]
        jbe     pcs_leaf
        mov     dword ptr [pci_lastbus],eax
pcs_leaf:
        or      edi,edi                 ; only function 0 can say whether
        jnz     pcs_nextfn              ; there are others
        test    ecx,PCI_MULTIFN
        jz      pcs_nextdev
pcs_nextfn:
        inc     edi
        cmp     edi,8
        jb      pcs_fn
pcs_nextdev:
        inc     edx
        cmp     edx,32
        jb      pcs_dev
        inc     esi
        cmp     esi,dword ptr [pci_lastbus]
        jbe     pcs_bus
pcs_done:
        popad
        ret

;=====================================================================
; pcx_nth - SI = n -> ESI -> the nth function pci_scan found.  CF if
;           there are not that many.
;=====================================================================
pcx_nth:
        movzx   esi,si
        cmp     esi,dword ptr [pci_count]
        jae     pxn_no
        imul    esi,esi,PCT_LEN
        add     esi,OFFSET pci_table
        clc
        ret
pxn_no:
        stc
        ret

;=====================================================================
; pcx_match - the nth function whose field at offset EDI of its table
;             entry, under the mask EDX, is ECX.  SI = n.
;             -> EBX = its address, ESI -> its entry.  CF if there is
;             no nth one.
;
; One search for both questions: a class is the top three bytes of
; PCT_CLASS, and a vendor and device are all of PCT_ID.
;=====================================================================
pcx_match:
        push    eax
        push    ebp
        movzx   ebp,si                  ; how many matches to pass over
        mov     esi,OFFSET pci_table
        mov     eax,dword ptr [pci_count]
pxm_one:
        or      eax,eax
        jz      pxm_no
        mov     ebx,dword ptr [esi+edi]
        and     ebx,edx
        cmp     ebx,ecx
        jne     pxm_next
        or      ebp,ebp
        jz      pxm_hit
        dec     ebp
pxm_next:
        add     esi,PCT_LEN
        dec     eax
        jmp     short pxm_one
pxm_hit:
        mov     ebx,dword ptr [esi+PCT_ADDR]
        pop     ebp
        pop     eax
        clc
        ret
pxm_no:
        pop     ebp
        pop     eax
        stc
        ret

;=====================================================================
; pcx_bar - EBX = device, DL = which base address register, 0 to 5.
;           -> EAX = the base, EDI = its high half if it is a 64-bit
;              one, ECX = how much it decodes, EDX = BARF_* flags.
;           CF if that device has no such register.
;
; THE SIZE IS MEASURED THE ONLY WAY THERE IS: all ones are written to
; the register, the bits that stayed zero are the ones the device does
; not decode, and what was there is put back.  For as long as that
; takes the register holds an address that is not the device's, so the
; device is told to stop answering first - the two enable bits in its
; command register - and interrupts are off, because what is being
; measured may be the display or the disk controller.
;
; A register that reads back zero decodes nothing, and that is said
; (BARF_NONE) rather than reported as a base of zero and a size of
; four gigabytes.
;=====================================================================
pcx_bar:
        push    ebx
        push    esi
        push    ebp
        movzx   esi,dl
        mov     al,PCIR_HDRTYPE
        call    pci_read
        shr     eax,16
        and     al,7Fh
        mov     ecx,6                   ; an ordinary device has six,
        or      al,al
        jz      pxb_count
        mov     ecx,2                   ; a bridge two,
        cmp     al,1
        je      pxb_count
        xor     ecx,ecx                 ; and anything else none
pxb_count:
        cmp     esi,ecx
        jae     pxb_no
        shl     esi,2
        add     esi,PCIR_BAR0           ; ESI = the register

        pushfd
        cli
        mov     eax,esi
        call    pci_read
        mov     ebp,eax                 ; EBP = what it holds
        mov     al,PCIR_COMMAND
        call    pci_read
        movzx   edi,ax                  ; EDI = the command register.  The
                                        ; status half is written as zeros
                                        ; both times: ones would clear it
        mov     ecx,edi
        and     ecx,NOT (PCIC_IOEN OR PCIC_MEMEN)
        mov     al,PCIR_COMMAND
        call    pci_write
        mov     ecx,0FFFFFFFFh
        mov     eax,esi
        call    pci_write
        mov     eax,esi
        call    pci_read
        mov     edx,eax                 ; EDX = which bits it has
        mov     ecx,ebp
        mov     eax,esi
        call    pci_write
        mov     ecx,edi
        mov     al,PCIR_COMMAND
        call    pci_write
        popfd

        xor     edi,edi
        test    ebp,1
        jnz     pxb_io
        mov     ecx,edx
        and     ecx,0FFFFFFF0h
        mov     eax,ebp
        and     eax,0FFFFFFF0h
        mov     edx,ebp
        and     edx,0Eh
        xor     ebp,ebp                 ; EBP = the flags, from here
        test    dl,08h
        jz      pxb_nopref
        or      ebp,BARF_PREF
pxb_nopref:
        and     dl,06h
        cmp     dl,02h
        jne     pxb_not1m
        or      ebp,BARF_LOW
pxb_not1m:
        cmp     dl,04h
        jne     pxb_size
        or      ebp,BARF_64
        push    eax
        lea     eax,[esi+4]
        call    pci_read
        mov     edi,eax
        pop     eax
        jmp     short pxb_size
pxb_io:
        ; SIXTEEN BITS OF IT, whatever the rest reads as.  There are
        ; 64K ports on this kind of machine and a port address is never
        ; wider; the top half of the register is zeros on most devices,
        ; ones on some, and on a VIA 686B's IDE function a mixture that
        ; measures as a sixteen-megabyte block of ports.
        mov     ecx,edx
        and     ecx,0000FFFCh
        jz      pxb_ioset               ; no bits at all: nothing there
        or      ecx,0FFFF0000h
pxb_ioset:
        mov     eax,ebp
        and     eax,0000FFFCh
        mov     ebp,BARF_IO
pxb_size:
        or      ecx,ecx
        jnz     pxb_some
        or      ebp,BARF_NONE
        xor     eax,eax
        jmp     short pxb_out
pxb_some:
        not     ecx
        inc     ecx
pxb_out:
        mov     edx,ebp
        pop     ebp
        pop     esi
        pop     ebx
        clc
        ret
pxb_no:
        pop     ebp
        pop     esi
        pop     ebx
        stc
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
pci_ok      db  0                       ; the mechanism that answered,
                                        ; 1 or 2; 0 if neither did
            db  0
            db  0,0
pci_count   dd  0                       ; functions in pci_table
pci_lastbus dd  0                       ; the highest bus any bridge named
pci_table   db  PCI_MAXFN * PCT_LEN dup(0)
