;=====================================================================
; NCR5380.INC - the SCSI port on a Pro Audio Spectrum sound card
;
; Media Vision's Pro Audio Spectrum Plus and 16 carry an NCR 53C80 for
; a CD-ROM, and a 5380 is the simplest SCSI chip there is: it does not
; run commands, it drives the bus's signals and lets the processor do
; the protocol a byte at a time - arbitrate, select, then whatever the
; target asks for next, phase by phase, each byte a REQ/ACK handshake
; done in software.  That is what this file does.
;
; THE PHASES ARE THE TARGET'S TO CHOOSE.  After selection the driver
; only answers: MESSAGE OUT gets IDENTIFY, COMMAND gets the command
; block, DATA IN and DATA OUT move data, STATUS is read and kept,
; MESSAGE IN is read, and COMMAND COMPLETE ends it.  The loop below is
; that list and nothing else.  IDENTIFY does not grant the right to
; disconnect, so the target holds the bus until it has finished.
;
; PSEUDO-DMA FOR BIG READS.  Byte by byte is fine for a status reply
; and slow for a CD's blocks, so the card has a data port of its own
; (base+5C00h) that does the handshake in hardware: the processor reads
; a stream of bytes and the card holds each read until the drive has
; put the next byte on the bus.  It is used for a read's whole data
; phase when that is a whole number of 512-byte pieces - READ(10) and
; READ(12) always are - and the port's ready bit is asked before each
; piece.  Ending a pseudo-DMA transfer partway through a phase loses a
; byte on an NMOS 5380 (Linux's NCR5380.c describes it), which is why
; a transfer that is not all whole pieces goes by hand instead.
;
; THE CARD'S ADDRESS.  The board decodes sixteen bits and puts its
; registers all over the I/O space at fixed offsets from a base, 388h
; unless MVSOUND.SYS moved it: the 5380's eight registers are at
; base+1C00h-1C03h and base+3C00h-3C03h.  A newer board can come up
; with no address at all until one is written to 9A01h, which is done
; only if nothing answered at any of the four bases - writing it on a
; board MVSOUND.SYS has already placed would move the board out from
; under the sound driver.
;
; NO INTERRUPT.  The 5380's are switched off and the card's SCSI IRQ
; is set to none, as Linux does for a board with no free line.
;
; Linux's pas16.c and NCR5380.c were the reference for the hardware,
; and 86Box's emulation of the card for the tests.
;
; Included by IOPM.ASM, after SCSI.INC.
;=====================================================================

; the card's registers, as offsets from its base
PSR_TOCNT       EQU     4000h           ; the pseudo-DMA watchdog's count
PSR_TOSTAT      EQU     4001h           ; ...and its status (01h clears it)
PSR_PDATA       EQU     5C00h           ; pseudo-DMA data
PSR_PSTAT       EQU     5C01h           ; ...bit 7: ready for the next piece
PSR_WAIT        EQU     0BC00h          ; wait states
PSR_SYS4        EQU     8003h           ; drive parameters, 5380 interrupt
PSR_OPMODE      EQU     0EC03h          ; bits 0-1: 11b, a SCSI port fitted
PSR_IOCFG3      EQU     0F002h          ; IRQs: SCSI's in the high nibble
PSR_MASTER      EQU     9A01h           ; NOT from the base: where the
                                        ; base itself is written

; the 5380's registers, by number (pas_regs turns one into a port)
N5R_DATA        EQU     0               ; read: the bus; write: output
N5R_ICR         EQU     1               ; initiator command
N5R_MR          EQU     2               ; mode
N5R_TCR         EQU     3               ; target command: the phase
N5R_CSBS        EQU     4               ; read: bus status; write: select
                                        ; enable
N5R_BSR         EQU     5               ; read: bus and status
N5R_RPI         EQU     7               ; read: reset parity/interrupt;
                                        ; write: start DMA initiator receive

; initiator command bits
N5I_RST         EQU     80h
N5I_AIP         EQU     40h             ; read: arbitration in progress
N5I_LA          EQU     20h             ; read: arbitration lost
N5I_ACK         EQU     10h
N5I_BSY         EQU     08h
N5I_SEL         EQU     04h
N5I_ATN         EQU     02h
N5I_DATA        EQU     01h             ; drive the data bus

; mode bits
N5M_ARB         EQU     01h
N5M_DMA         EQU     02h
N5M_MONBSY      EQU     04h
N5M_PARCHK      EQU     20h             ; check parity: only the probe
                                        ; writes it, to see it read back

; bus status bits
N5S_BSY         EQU     40h
N5S_REQ         EQU     20h
N5S_SEL         EQU     02h

; the phases, as (MSG, C/D, I/O) - bits 4-2 of the bus status, and the
; same three bits are what the target command register wants
N5P_DOUT        EQU     0
N5P_DIN         EQU     1
N5P_CMD         EQU     2
N5P_STAT        EQU     3
N5P_MOUT        EQU     6
N5P_MIN         EQU     7

NCR_MYID        EQU     7               ; the highest ID always wins

; waits, counted in reads of the card - see AHA154X.INC
N5W_ARB         EQU     00040000h       ; for the bus to be won
N5W_SEL         EQU     00040000h       ; 250ms or more for a target
N5W_BYTE        EQU     00100000h       ; REQ to drop after ACK
N5W_IO          EQU     01000000h       ; the target to ask for the next
                                        ; thing: a drive spinning up takes
                                        ; seconds
N5W_RESET       EQU     00040000h       ; reads of port 80h after a reset

;=====================================================================
; pas_probe - every Pro Audio Spectrum SCSI port, registered.  All
;             registers kept.
;=====================================================================
pas_probe:
        pushad
        mov     byte ptr [pas_found],0
        call    pas_bases
        cmp     byte ptr [pas_found],0
        jne     ppr_out
        ; NOTHING ANSWERED: a newer board waits for its address.  Board
        ; 0, at 388h, and then look again
        mov     dx,PSR_MASTER
        mov     al,0BCh
        out     dx,al
        call    io_delay
        mov     al,388h SHR 2
        out     dx,al
        call    io_delay
        mov     dx,388h
        call    pas_one
ppr_out:
        popad
        ret

pas_bases:
        mov     esi,OFFSET pas_bslist
pbs_one:
        movzx   edx,word ptr [esi]
        or      edx,edx
        jz      pbs_out
        call    pas_one
        jc      pbs_next
        mov     byte ptr [pas_found],1
pbs_next:
        add     esi,2
        jmp     pbs_one
pbs_out:
        ret

;=====================================================================
; pas_one - DX = a base.  If a Pro Audio Spectrum with its SCSI port
;           answers there, set it up and register it.  CF if not.  All
;           registers kept.
;=====================================================================
pas_one:
        pushad
        mov     [pas_base],dx
        mov     eax,[scs_nhba]
        cmp     eax,SCSI_MAXHBA
        jae     po1_no

        ; A SCSI PORT FITTED?  OPERATION_MODE_1's low two bits say so,
        ; and a port with nothing behind it reads FFh
        add     dx,PSR_OPMODE
        in      al,dx
        cmp     al,0FFh
        je      po1_no
        and     al,03h
        cmp     al,03h
        jne     po1_no

        ; ...and Media Vision made boards that claim one and have none,
        ; so the 5380's mode register is asked to hold a value, as Linux
        ; asks it
        mov     dx,[pas_base]
        add     dx,PSR_WAIT
        mov     al,01h                  ; one wait state
        out     dx,al
        call    ncr_ports
        mov     bl,N5R_MR
        mov     al,N5M_PARCHK
        call    ncr_wr
        call    ncr_rd
        cmp     al,N5M_PARCHK
        jne     po1_no
        xor     al,al
        call    ncr_wr
        call    ncr_rd
        or      al,al
        jnz     po1_no

        ; SET IT UP, as Linux's init_board does for a board with no IRQ
        mov     dx,[pas_base]
        add     dx,PSR_TOCNT
        mov     al,30h                  ; the pseudo-DMA watchdog
        out     dx,al
        inc     dx                      ; PSR_TOSTAT
        mov     al,01h
        out     dx,al
        mov     dx,[pas_base]
        add     dx,PSR_SYS4
        mov     al,4Dh                  ; drive parameters, no 5380 IRQ
        out     dx,al
        mov     dx,[pas_base]
        add     dx,PSR_IOCFG3
        in      al,dx
        and     al,0Fh                  ; the sound's IRQ stays; SCSI none
        out     dx,al

        ; and the 5380 idle, on an idle bus
        mov     bl,N5R_ICR
        xor     al,al
        call    ncr_wr
        mov     bl,N5R_MR
        call    ncr_wr
        mov     bl,N5R_TCR
        call    ncr_wr
        mov     bl,N5R_CSBS             ; select enable: none
        call    ncr_wr
        mov     bl,N5R_RPI
        call    ncr_rd
        mov     bl,N5R_CSBS
        call    ncr_rd
        test    al,N5S_BSY OR N5S_SEL
        jz      po1_idle
        call    ncr_busreset            ; something is stuck on it
po1_idle:
        mov     edi,OFFSET pas_tmpl
        push    edi
        xor     eax,eax
        mov     ecx,HBA_LEN
        cld
        rep     stosb
        pop     edi
        mov     dword ptr [edi+HBA_EXEC],OFFSET pas_exec
        mov     byte ptr [edi+HBA_KIND],SHK_PAS16
        mov     byte ptr [edi+HBA_ID],NCR_MYID
        mov     ax,[pas_base]
        mov     [edi+HBA_BASE],ax
        mov     byte ptr [edi+HBA_IRQ],0FFh
        mov     byte ptr [edi+HBA_DMA],0FFh
        mov     dword ptr [edi+HBA_MAXX],SCSI_BUFLEN
        push    edi
        add     edi,HBA_NAME
        mov     esi,OFFSET pas_name
        mov     ecx,16
        rep     movsb
        pop     edi
        call    scs_addhba
        jc      po1_no
        popad
        clc
        ret
po1_no:
        popad
        stc
        ret

;---------------------------------------------------------------------
; ncr_ports - the ports of [pas_base]'s 5380 registers 0, 1 and 4,
;             which the byte loops use, worked out once.  All kept.
;---------------------------------------------------------------------
ncr_ports:
        push    eax
        mov     ax,[pas_base]
        add     ax,word ptr [pas_regs+N5R_DATA*2]
        mov     [ncr_pdata],ax
        mov     ax,[pas_base]
        add     ax,word ptr [pas_regs+N5R_ICR*2]
        mov     [ncr_picr],ax
        mov     ax,[pas_base]
        add     ax,word ptr [pas_regs+N5R_CSBS*2]
        mov     [ncr_pcsbs],ax
        pop     eax
        ret

;---------------------------------------------------------------------
; ncr_rd - BL = a 5380 register -> AL.     All else kept.
; ncr_wr - BL = a 5380 register, AL to it.  All kept.
;---------------------------------------------------------------------
ncr_rd:
        push    edx
        movzx   edx,bl
        mov     dx,word ptr [pas_regs+edx*2]
        add     dx,[pas_base]
        in      al,dx
        pop     edx
        ret

ncr_wr:
        push    edx
        movzx   edx,bl
        mov     dx,word ptr [pas_regs+edx*2]
        add     dx,[pas_base]
        out     dx,al
        pop     edx
        ret

;---------------------------------------------------------------------
; ncr_icr - AL to the initiator command register.  All kept.
; ncr_csbs - the bus status -> AL.  All else kept.
;---------------------------------------------------------------------
ncr_icr:
        push    edx
        mov     dx,[ncr_picr]
        out     dx,al
        pop     edx
        ret

ncr_csbs:
        push    edx
        mov     dx,[ncr_pcsbs]
        in      al,dx
        pop     edx
        ret

;---------------------------------------------------------------------
; ncr_busreset - RST on the bus, and time for the drives to come back.
;                All registers kept.
;---------------------------------------------------------------------
ncr_busreset:
        push    eax
        push    ecx
        mov     al,N5I_RST
        call    ncr_icr
        mov     ecx,40                  ; RST is held for 25us at least
nbr_hold:
        in      al,80h
        loop    nbr_hold
        xor     al,al
        call    ncr_icr
        push    ebx
        mov     bl,N5R_MR
        call    ncr_wr
        mov     bl,N5R_TCR
        call    ncr_wr
        mov     bl,N5R_RPI
        call    ncr_rd
        pop     ebx
        mov     ecx,N5W_RESET
nbr_wait:
        in      al,80h
        loop    nbr_wait
        pop     ecx
        pop     eax
        ret

;=====================================================================
; pas_exec - HBA_EXEC.  EBX -> the adapter, EDI -> a request.  CF
;            unless the target said GOOD.  All registers kept.
;=====================================================================
pas_exec:
        pushad
        mov     ax,[ebx+HBA_BASE]
        mov     [pas_base],ax
        call    ncr_ports
        mov     [ncr_srq],edi
        mov     dword ptr [ncr_got],0
        mov     dword ptr [ncr_cmdpos],0
        mov     byte ptr [ncr_stat],0FFh
        mov     byte ptr [ncr_idsent],0
        mov     byte ptr [ncr_pdone],0

        call    ncr_select              ; -> CF, AL = SRH_* on failure
        jc      pex_nosel

;--- the information phases, in whatever order the target asks -------
pex_loop:
        call    ncr_waitreq             ; -> CF, AL = 1 time, 2 bus free
        jc      pex_lost
        call    ncr_csbs
        shr     al,2
        and     al,07h
        mov     [ncr_phase],al
        mov     bl,N5R_TCR              ; the 5380 drives the bus only in
        call    ncr_wr                  ; the phase it has been told
        cmp     al,N5P_DIN
        je      pex_din
        cmp     al,N5P_DOUT
        je      pex_dout
        cmp     al,N5P_CMD
        je      pex_cmd
        cmp     al,N5P_STAT
        je      pex_stat
        cmp     al,N5P_MIN
        je      pex_min
        cmp     al,N5P_MOUT
        je      pex_mout
        jmp     pex_err                 ; 4 and 5 are no phase at all

pex_mout:
        ; IDENTIFY the first time, with the LUN and without leave to
        ; disconnect; after that, a target still asking gets NOP
        mov     al,08h
        cmp     byte ptr [ncr_idsent],0
        jne     pex_mo1
        mov     byte ptr [ncr_idsent],1
        mov     edi,[ncr_srq]
        mov     al,[edi+SRQ_LUN]
        and     al,07h
        or      al,80h
pex_mo1:
        call    ncr_pout
        jc      pex_err
        jmp     pex_loop

pex_cmd:
        mov     edi,[ncr_srq]
        mov     ecx,[ncr_cmdpos]
        inc     dword ptr [ncr_cmdpos]
        xor     al,al                   ; a target wanting more than the
        movzx   edx,byte ptr [edi+SRQ_CDBLEN]   ; block has gets zeros
        cmp     ecx,edx
        jae     pex_c0
        mov     al,byte ptr [edi+SRQ_CDB+ecx]
pex_c0:
        call    ncr_pout
        jc      pex_err
        jmp     pex_loop

pex_din:
        cmp     byte ptr [ncr_pdone],0
        jne     pex_dinpio
        mov     edi,[ncr_srq]
        mov     al,[edi+SRQ_CDB]
        cmp     al,SCSI_READ10
        je      pex_whole
        cmp     al,SCSI_READ12
        jne     pex_dinpio
pex_whole:
        mov     ecx,[edi+SRQ_COUNT]
        sub     ecx,[ncr_got]
        jbe     pex_dinpio
        test    ecx,1FFh
        jnz     pex_dinpio
        mov     byte ptr [ncr_pdone],1
        mov     edi,[edi+SRQ_BUF]
        add     edi,[ncr_got]
        call    pas_pdmain              ; -> EAX = bytes, CF if it stalled
        pushfd
        add     [ncr_got],eax
        popfd
        jc      pex_err
        jmp     pex_loop
pex_dinpio:
        call    ncr_pin
        jc      pex_err
        mov     edi,[ncr_srq]
        mov     ecx,[ncr_got]
        cmp     ecx,[edi+SRQ_COUNT]
        jae     pex_loop                ; more than there is room for
        mov     edx,[edi+SRQ_BUF]
        mov     [edx+ecx],al
        inc     dword ptr [ncr_got]
        jmp     pex_loop

pex_dout:
        mov     edi,[ncr_srq]
        xor     al,al
        mov     ecx,[ncr_got]
        cmp     ecx,[edi+SRQ_COUNT]
        jae     pex_do0                 ; asked for more than there is
        mov     edx,[edi+SRQ_BUF]
        mov     al,[edx+ecx]
        inc     dword ptr [ncr_got]
pex_do0:
        call    ncr_pout
        jc      pex_err
        jmp     pex_loop

pex_stat:
        call    ncr_pin
        jc      pex_err
        mov     [ncr_stat],al
        jmp     pex_loop

pex_min:
        call    ncr_pin
        jc      pex_err
        or      al,al                   ; COMMAND COMPLETE
        jz      pex_complete
        cmp     al,01h                  ; an extended message: a length
        jne     pex_loop                ; and that many bytes, all of them
        call    ncr_pin                 ; ignored.  Every other message -
        jc      pex_err                 ; pointers saved or restored, a
        movzx   ecx,al                  ; reject - changes nothing here
        or      ecx,ecx
        jz      pex_loop
pex_ext:
        call    ncr_pin
        jc      pex_err
        loop    pex_ext
        jmp     pex_loop

pex_complete:
        call    ncr_waitfree            ; the target lets go of the bus
        mov     edi,[ncr_srq]
        mov     al,[ncr_stat]
        mov     [edi+SRQ_TSTAT],al
        mov     byte ptr [edi+SRQ_HSTAT],SRH_OK
        mov     eax,[ncr_got]
        mov     [edi+SRQ_GOT],eax
        cmp     byte ptr [ncr_stat],SCSI_ST_GOOD
        jne     pex_fail
        popad
        clc
        ret

pex_nosel:
        mov     edi,[ncr_srq]
        mov     [edi+SRQ_HSTAT],al
        jmp     short pex_fail
pex_lost:
        ; THE TARGET STOPPED ASKING, and nobody knows what state it is
        ; in: the bus is reset.  ONE THAT LET GO OF THE BUS without
        ; COMMAND COMPLETE has failed the command but left the bus free
        mov     edi,[ncr_srq]
        cmp     al,1
        jne     pex_free
        mov     byte ptr [edi+SRQ_HSTAT],SRH_TIMEOUT
        jmp     short pex_reset
pex_free:
        mov     byte ptr [edi+SRQ_HSTAT],SRH_ERROR
        mov     bl,N5R_MR
        xor     al,al
        call    ncr_wr
        jmp     short pex_fail
pex_err:
        mov     edi,[ncr_srq]
        mov     byte ptr [edi+SRQ_HSTAT],SRH_ERROR
pex_reset:
        mov     bl,N5R_MR
        xor     al,al
        call    ncr_wr
        call    ncr_busreset
pex_fail:
        popad
        stc
        ret

;---------------------------------------------------------------------
; ncr_select - arbitrate, and select the target in [ncr_srq] with ATN,
;              so that the first thing it asks for is a message.  CF
;              and AL = SRH_NODEV if nothing answered, SRH_ERROR if the
;              bus could not be had.
;---------------------------------------------------------------------
ncr_select:
        push    ebx
        push    ecx
        push    edx
        mov     bl,N5R_TCR
        xor     al,al
        call    ncr_wr
        call    ncr_icr
        mov     bl,N5R_DATA
        mov     al,1 SHL NCR_MYID
        call    ncr_wr
        mov     bl,N5R_MR
        mov     al,N5M_ARB
        call    ncr_wr

        ; the 5380 waits for a free bus and puts our ID on it
        mov     bl,N5R_ICR
        mov     ecx,N5W_ARB
nsl_aip:
        call    ncr_rd
        test    al,N5I_AIP
        jnz     nsl_aipok
        dec     ecx
        jnz     nsl_aip
        jmp     short nsl_busy
nsl_aipok:
        in      al,80h                  ; the 2.2us arbitration delay
        in      al,80h
        in      al,80h
        call    ncr_rd
        test    al,N5I_LA
        jnz     nsl_busy                ; somebody else's: not on a bus
                                        ; where this is the only initiator
        mov     al,N5I_SEL OR N5I_BSY   ; won
        call    ncr_icr
        in      al,80h
        in      al,80h

        mov     edi,[ncr_srq]
        mov     cl,[edi+SRQ_TARGET]
        mov     al,1
        shl     al,cl
        or      al,1 SHL NCR_MYID
        mov     bl,N5R_DATA
        call    ncr_wr
        mov     al,N5I_BSY OR N5I_DATA OR N5I_ATN OR N5I_SEL
        call    ncr_icr
        mov     bl,N5R_MR
        xor     al,al
        call    ncr_wr
        mov     bl,N5R_CSBS             ; select enable: no reselection
        call    ncr_wr
        in      al,80h
        mov     al,N5I_DATA OR N5I_ATN OR N5I_SEL       ; let go of BSY
        call    ncr_icr
        in      al,80h

        ; the target answers by raising BSY itself
        mov     ecx,N5W_SEL
nsl_bsy:
        call    ncr_csbs
        test    al,N5S_BSY
        jnz     nsl_got
        dec     ecx
        jnz     nsl_bsy
        xor     al,al                   ; nobody there
        call    ncr_icr
        mov     al,SRH_NODEV
        jmp     short nsl_fail
nsl_got:
        in      al,80h
        mov     al,N5I_ATN              ; SEL and the data go; ATN stays
        call    ncr_icr                 ; for the IDENTIFY
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
nsl_busy:
        mov     bl,N5R_MR
        xor     al,al
        call    ncr_wr
        call    ncr_icr
        mov     al,SRH_ERROR
nsl_fail:
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; ncr_waitreq - until the target raises REQ.  CF and AL = 1 if it
;               never does, AL = 2 if it lets go of the bus instead.
;               Everything else kept.
;---------------------------------------------------------------------
ncr_waitreq:
        push    ecx
        mov     ecx,N5W_IO
nwr_lp:
        call    ncr_csbs
        test    al,N5S_REQ
        jnz     nwr_ok
        test    al,N5S_BSY
        jz      nwr_free
        dec     ecx
        jnz     nwr_lp
        mov     al,1
        pop     ecx
        stc
        ret
nwr_free:
        mov     al,2
        pop     ecx
        stc
        ret
nwr_ok:
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; ncr_waitfree - until BSY goes: the end of a command.  All kept.
;---------------------------------------------------------------------
ncr_waitfree:
        push    eax
        push    ecx
        mov     ecx,N5W_BYTE
nwf_lp:
        call    ncr_csbs
        test    al,N5S_BSY
        jz      nwf_out
        dec     ecx
        jnz     nwf_lp
nwf_out:
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; ncr_reqdrop - until REQ goes, after ACK.  CF if it never does.  All
;               registers kept.
;---------------------------------------------------------------------
ncr_reqdrop:
        push    eax
        push    ecx
        mov     ecx,N5W_BYTE
nrd_lp:
        call    ncr_csbs
        test    al,N5S_REQ
        jz      nrd_ok
        dec     ecx
        jnz     nrd_lp
        pop     ecx
        pop     eax
        stc
        ret
nrd_ok:
        pop     ecx
        pop     eax
        clc
        ret

;---------------------------------------------------------------------
; ncr_inphase - CF clear if the bus is still in [ncr_phase].  The
;               byte about to be handshaken belongs to the phase it
;               was asked for in, or it is not the byte that was meant.
;               All else kept.
;---------------------------------------------------------------------
ncr_inphase:
        push    eax
        call    ncr_csbs
        shr     al,2
        and     al,07h
        cmp     al,[ncr_phase]
        pop     eax
        je      nip_yes
        stc
        ret
nip_yes:
        clc
        ret

;---------------------------------------------------------------------
; ncr_pin - one byte in from the target -> AL, by hand.  CF if the
;           handshake failed.  Everything else kept.
;---------------------------------------------------------------------
ncr_pin:
        push    edx
        call    ncr_waitreq
        jc      npi_bad
        call    ncr_inphase
        jc      npi_bad
        mov     dx,[ncr_pdata]
        in      al,dx
        push    eax
        mov     al,N5I_ACK
        call    ncr_icr
        call    ncr_reqdrop
        pushfd
        xor     al,al
        call    ncr_icr
        popfd
        pop     eax
        jc      npi_bad
        pop     edx
        clc
        ret
npi_bad:
        pop     edx
        stc
        ret

;---------------------------------------------------------------------
; ncr_pout - AL out to the target, by hand.  CF if the handshake
;            failed.  All registers kept.
;
; ATN DROPS HERE.  Every message sent is one byte, so this byte is the
; last of its message, and SCSI wants ATN released before the ACK that
; sends it - the ICR writes below simply leave it out.
;---------------------------------------------------------------------
ncr_pout:
        push    eax
        push    edx
        mov     ah,al
        call    ncr_waitreq
        jc      npo_bad
        call    ncr_inphase
        jc      npo_bad
        mov     dx,[ncr_pdata]
        mov     al,ah
        out     dx,al
        mov     al,N5I_DATA
        call    ncr_icr
        mov     al,N5I_DATA OR N5I_ACK
        call    ncr_icr
        call    ncr_reqdrop
        pushfd
        xor     al,al
        call    ncr_icr
        popfd
        jc      npo_bad
        pop     edx
        pop     eax
        clc
        ret
npo_bad:
        pop     edx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; pas_pdmain - the data-in phase in progress, ECX bytes of it (a
;              multiple of 512) into EDI, through the card's pseudo-DMA
;              port.  -> EAX = bytes read, CF if the drive stopped
;              sending or let go of the bus.  Everything else kept.
;---------------------------------------------------------------------
pas_pdmain:
        push    ebx
        push    ecx
        push    edx
        push    edi
        push    ebp
        xor     ebp,ebp                 ; EBP = bytes read
        mov     bl,N5R_MR
        mov     al,N5M_DMA OR N5M_MONBSY
        call    ncr_wr
        xor     al,al
        call    ncr_icr
        mov     bl,N5R_RPI              ; start DMA initiator receive
        call    ncr_wr
        call    io_delay
pdm_piece:
        mov     ebx,N5W_IO
pdm_wait:
        mov     dx,[pas_base]
        add     dx,PSR_PSTAT
        in      al,dx
        test    al,80h
        jnz     pdm_read
        ; still in the data phase, and still connected?
        call    ncr_csbs
        test    al,N5S_BSY
        jz      pdm_fail
        test    al,N5S_REQ
        jz      pdm_next
        shr     al,2
        and     al,07h
        cmp     al,N5P_DIN
        jne     pdm_stop                ; the target has moved on: short
pdm_next:
        dec     ebx
        jnz     pdm_wait
        jmp     short pdm_fail
pdm_read:
        push    ecx
        mov     dx,[pas_base]
        add     dx,PSR_PDATA
        mov     ecx,512
        cld
        rep     insb
        pop     ecx
        add     ebp,512
        sub     ecx,512
        jnz     pdm_piece
pdm_stop:
        mov     bl,N5R_MR
        xor     al,al
        call    ncr_wr
        mov     eax,ebp
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
pdm_fail:
        mov     bl,N5R_MR
        xor     al,al
        call    ncr_wr
        mov     eax,ebp
        pop     ebp
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;=====================================================================
; Data
;=====================================================================
pas_regs    dw 1C00h, 1C01h, 1C02h, 1C03h       ; the 5380's 0 to 3
            dw 3C00h, 3C01h, 3C02h, 3C03h       ; ...and 4 to 7
pas_bslist  dw 388h, 384h, 38Ch, 288h, 0        ; where a board may be
pas_base    dw 0
ncr_pdata   dw 0                        ; register 0, 1 and 4's ports
ncr_picr    dw 0
ncr_pcsbs   dw 0
ncr_srq     dd 0                        ; the request in hand
ncr_got     dd 0                        ; data bytes moved
ncr_cmdpos  dd 0                        ; command bytes sent
ncr_stat    db 0                        ; the status byte
ncr_phase   db 0
ncr_idsent  db 0
ncr_pdone   db 0                        ; pseudo-DMA used for this one
pas_found   db 0
pas_name    db 'Pro Audio SCSI  '
pas_tmpl    db HBA_LEN dup (0)
