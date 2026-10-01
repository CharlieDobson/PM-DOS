;=====================================================================
; ISAPNP.INC - ISA Plug and Play, as much of it as finds a CD-ROM
;
; A SOUND CARD'S IDE CONNECTOR IS NOT THERE UNTIL SOMETHING SAYS WHERE.
; A Plug and Play card comes out of reset answering at no port at all.
; On a machine whose BIOS knows nothing about such cards - a 486 with
; ISA slots - it is a program run from CONFIG.SYS that gives the card
; its addresses, and PM-DOS started from a floppy has run no such
; program.  The drive on that connector is then behind a port that
; does not exist, and ata_init can ask 1E8h all day.
;
; So this does the one part of that program's job the kernel needs:
; find the cards, find an IDE controller among their logical devices,
; and switch it on at an address ata_init looks at.  NOTHING ELSE ON
; THE CARD IS TOUCHED.  The audio, the game port and the wavetable
; stay as they were found, which on such a machine is off.
;
; THE PROTOCOL, from the Plug and Play ISA Specification 1.0a:
;
;   - three ports.  279h takes a register number, A79h takes a value
;     for it, and values are read back at a port that is OURS TO
;     CHOOSE, between 203h and 3FFh, and told to the cards.
;   - a card listens to none of it until it has seen 32 particular
;     bytes written to 279h: pnp_key.
;   - cards are told apart by ISOLATION.  Every card without a number
;     shifts its 72-bit identifier out a bit at a time, a one as 55h
;     then AAh and a zero as nothing at all; a card that is sending a
;     zero and sees another's one drops out.  The one left at the end
;     is given a number and the rest go round again.
;   - a card with a number can be woken by it, and then its resource
;     data - what devices it has and where each may live - is read a
;     byte at a time, and its devices are configured through registers
;     30h upwards.
;
; THE READ PORT MAY COLLIDE with something on the machine, and the
; only way to know is that isolation finds nothing sensible there, so
; pnp_rdps is a list and each is tried until one finds a card.  A
; machine with no such card costs eight bit-times at each: no vendor's
; name starts with a zero byte, so eight zeroes is nobody.
;
; NO INTERRUPT IS GIVEN TO THE CONTROLLER.  ATA.INC polls, with nIEN
; set at the drive, so a line would be something to collide on and
; nothing to use.
;
; THE CARD NUMBERS ARE HANDED OUT AGAIN, which undoes a Plug and Play
; BIOS's numbering and none of its work: a device the BIOS switched on
; stays on and stays where it was put.
;
; Interrupts are off when this runs and the clock is not ticking, so
; the pauses the specification asks for are counted in reads of port
; 80h - a microsecond or so each on an ISA bus, and more is harmless.
;
; Included by IOPM.ASM, after PROBELOG.INC and before ATA.INC, whose
; ata_ports it reads.
;=====================================================================

PNP_ADDR        EQU     0279h           ; write: a register's number
PNP_WDATA       EQU     0A79h           ; write: a value for it

; Card-level registers
PNP_R_RDP       EQU     00h             ; where values are read, bits 9-2
PNP_R_ISOL      EQU     01h             ; serial isolation
PNP_R_CTRL      EQU     02h             ; PNP_C_*
PNP_R_WAKE      EQU     03h             ; wake the card with this number
PNP_R_DATA      EQU     04h             ; the next byte of resource data
PNP_R_STATUS    EQU     05h             ; bit 0 = ...and it is ready
PNP_R_CSN       EQU     06h             ; the card's number
PNP_R_LDN       EQU     07h             ; which logical device 30h on is

; Logical device registers
PNP_R_ACT       EQU     30h             ; bit 0 = the device is on
PNP_R_IO0       EQU     60h             ; first port range, high byte first
PNP_R_IO1       EQU     62h             ; second
PNP_R_IRQ0      EQU     70h             ; first interrupt, 0 = none
PNP_R_IRQ1      EQU     72h             ; second

PNP_C_WAITKEY   EQU     02h             ; stop listening until the key
PNP_C_RESETCSN  EQU     04h             ; every card's number back to 0

; Small resource items, by tag
PNP_T_LD        EQU     2               ; a logical device, and its ID
PNP_T_COMPAT    EQU     3               ; an ID it is compatible with
PNP_T_DEP       EQU     6               ; one choice of resources begins
PNP_T_ENDDEP    EQU     7               ; the choices end
PNP_T_IO        EQU     8               ; a port range
PNP_T_FIXED     EQU     9               ; a port range at one address
PNP_T_END       EQU     15

PNP_MAXCARD     EQU     8               ; cards given a number
PNP_MAXIDE      EQU     2               ; IDE controllers remembered
PNP_MAXRES      EQU     4096            ; bytes of one card's resources read

PNP_T250        EQU     500             ; reads of port 80h: 250 microseconds
PNP_T1MS        EQU     2000            ; a millisecond
PNP_T2MS        EQU     4000
PNP_RESWAIT     EQU     20000           ; polls before a resource byte is
                                        ; given up on

; An IDE controller that was found
PNF_CSN         EQU     0               ; db - its card
PNF_LD          EQU     1               ; db - its logical device
PNF_OK          EQU     2               ; db - the channels it may be put at,
                                        ;      a bit each, as ata_ports counts
PNF_VENDOR      EQU     4               ; dd - the card's ID
PNF_LEN         EQU     8

;=====================================================================
; pnp_init - switch on any IDE controller an ISA Plug and Play card
;            has.  Before blk_init, so that ata_init finds the drive.
;=====================================================================
pnp_init:
        pushad
        mov     byte ptr [plog+PLP_FIRST+PLP_STAGE],'N'
        mov     byte ptr [pnp_nfound],0
        mov     dword ptr [pnp_used],0

        xor     ebx,ebx
pin_port:
        mov     ax,word ptr [pnp_rdps+ebx*2]
        or      ax,ax
        jz      pin_done
        mov     word ptr [pnp_rdp],ax
        call    pnp_isolate
        cmp     byte ptr [pnp_cards],0
        jne     pin_cards
        inc     ebx
        jmp     short pin_port

pin_cards:
        mov     ax,word ptr [pnp_rdp]
        mov     word ptr [plog+PLP_FIRST+PLP_RDP],ax
        mov     al,byte ptr [pnp_cards]
        mov     byte ptr [plog+PLP_FIRST+PLP_CARDS],al
        mov     byte ptr [plog+PLP_FIRST+PLP_STAGE],'I'

        mov     bl,1                            ; what each card has
pin_scan:
        cmp     bl,byte ptr [pnp_cards]
        ja      pin_set
        call    pnp_scan
        inc     bl
        jmp     short pin_scan

pin_set:
        xor     ebx,ebx                         ; and the controllers among it
pin_each:
        cmp     bl,byte ptr [pnp_nfound]
        jae     pin_done
        lea     esi,[pnp_found+ebx*8]
        call    pnp_setide
        inc     ebx
        jmp     short pin_each

pin_done:
        mov     ax,(PNP_C_WAITKEY SHL 8) OR PNP_R_CTRL
        call    pnp_wr
        popad
        ret

;---------------------------------------------------------------------
; pnp_wr - register AL takes the value AH.  Everything kept.
;---------------------------------------------------------------------
pnp_wr:
        push    edx
        mov     dx,PNP_ADDR
        out     dx,al
        mov     dx,PNP_WDATA
        xchg    al,ah
        out     dx,al
        xchg    al,ah
        pop     edx
        ret

;---------------------------------------------------------------------
; pnp_rd - AL = register -> AL = its value
;---------------------------------------------------------------------
pnp_rd:
        push    edx
        mov     dx,PNP_ADDR
        out     dx,al
        mov     dx,word ptr [pnp_rdp]
        in      al,dx
        pop     edx
        ret

;---------------------------------------------------------------------
; pnp_rd16 - AL = the register of a high byte -> EAX = the word
;---------------------------------------------------------------------
pnp_rd16:
        push    ecx
        mov     cl,al
        call    pnp_rd
        mov     ch,al
        mov     al,cl
        inc     al
        call    pnp_rd
        mov     ah,ch
        movzx   eax,ax
        pop     ecx
        ret

;---------------------------------------------------------------------
; pnp_pause - ECX reads of port 80h.  Everything kept.
;---------------------------------------------------------------------
pnp_pause:
        push    eax
        push    ecx
ppa_lp:
        in      al,80h
        dec     ecx
        jnz     ppa_lp
        pop     ecx
        pop     eax
        ret

;---------------------------------------------------------------------
; pnp_key - the 32 bytes that make the cards listen.  Two zeroes put
;           each card's copy of the sequence back at its start; the
;           sequence is a shift register's, from 6Ah.
;---------------------------------------------------------------------
pnp_key:
        push    eax
        push    ecx
        push    edx
        mov     dx,PNP_ADDR
        xor     al,al
        out     dx,al
        out     dx,al
        mov     al,6Ah
        mov     ecx,32
pky_lp:
        out     dx,al
        mov     ah,al                           ; bit 7 of the next is bit 0
        shr     ah,1                            ; of this one against bit 1
        xor     ah,al
        and     ah,1
        shl     ah,7
        shr     al,1
        or      al,ah
        dec     ecx
        jnz     pky_lp
        pop     edx
        pop     ecx
        pop     eax
        ret

;=====================================================================
; pnp_isolate - number the cards, reading at [pnp_rdp].
;               [pnp_cards] = how many were found.
;=====================================================================
pnp_isolate:
        push    eax
        push    ebx
        push    ecx
        push    edx
        push    esi
        mov     byte ptr [pnp_cards],0

        mov     ax,(PNP_C_WAITKEY SHL 8) OR PNP_R_CTRL
        call    pnp_wr
        call    pnp_key
        mov     ax,(PNP_C_RESETCSN SHL 8) OR PNP_R_CTRL
        call    pnp_wr
        mov     ecx,PNP_T2MS
        call    pnp_pause
        mov     ax,(PNP_C_WAITKEY SHL 8) OR PNP_R_CTRL
        call    pnp_wr
        call    pnp_key

pis_next:
        mov     ax,PNP_R_WAKE                   ; Wake[0]: every card that
        call    pnp_wr                          ; has no number yet
        mov     ax,word ptr [pnp_rdp]
        shr     ax,2
        mov     ah,al
        mov     al,PNP_R_RDP
        call    pnp_wr
        mov     ecx,PNP_T1MS
        call    pnp_pause
        mov     al,PNP_R_ISOL
        mov     dx,PNP_ADDR
        out     dx,al
        call    pnp_pause

        ; 64 bits of identifier.  BL is the checksum as it is worked
        ; out, BH is set once any bit has been a one
        mov     dx,word ptr [pnp_rdp]
        mov     bl,6Ah
        xor     bh,bh
        mov     esi,64
pis_id:
        call    pnp_isobit
        or      bh,al
        mov     ah,bl
        shr     ah,1
        xor     ah,bl
        xor     ah,al
        and     ah,1
        shl     ah,7
        shr     bl,1
        or      bl,ah
        cmp     esi,57                          ; the eighth bit
        jne     pis_idnext
        or      bh,bh
        jz      pis_end                         ; nobody is sending
pis_idnext:
        dec     esi
        jnz     pis_id

        ; and the eight of the checksum the card worked out
        xor     bh,bh
        xor     esi,esi
pis_sum:
        call    pnp_isobit
        mov     ecx,esi
        shl     al,cl
        or      bh,al
        inc     esi
        cmp     esi,8
        jb      pis_sum

        or      bl,bl
        jz      pis_end
        cmp     bl,bh                           ; two cards talking at once,
        jne     pis_end                         ; or something else at the
                                                ; read port
        inc     byte ptr [pnp_cards]
        mov     ah,byte ptr [pnp_cards]
        mov     al,PNP_R_CSN
        call    pnp_wr
        mov     ecx,PNP_T250
        call    pnp_pause
        cmp     byte ptr [pnp_cards],PNP_MAXCARD
        jb      pis_next
pis_end:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; pnp_isobit - one bit of isolation.  DX = the read port -> AL = 1 if
;              the two reads were 55h and AAh, 0 if not.
;---------------------------------------------------------------------
pnp_isobit:
        push    ecx
        mov     ecx,PNP_T250
        in      al,dx
        mov     ah,al
        call    pnp_pause
        in      al,dx
        call    pnp_pause
        pop     ecx
        cmp     ax,55AAh
        sete    al
        ret

;---------------------------------------------------------------------
; pnp_resbyte - the next byte of the woken card's resource data -> AL.
;               CF set if the card never offered one.
;---------------------------------------------------------------------
pnp_resbyte:
        push    ecx
        mov     ecx,PNP_RESWAIT
prb_wait:
        mov     al,PNP_R_STATUS
        call    pnp_rd
        test    al,1
        jnz     prb_have
        call    io_delay
        dec     ecx
        jnz     prb_wait
        pop     ecx
        stc
        ret
prb_have:
        mov     al,PNP_R_DATA
        call    pnp_rd
        pop     ecx
        clc
        ret

;---------------------------------------------------------------------
; pnp_next - pnp_resbyte, for as long as [pnp_left] lasts.  A read
;            port with nothing behind it is always ready and always
;            FFh, which is an item 65,535 bytes long.
;---------------------------------------------------------------------
pnp_next:
        cmp     dword ptr [pnp_left],0
        je      pnx_out
        dec     dword ptr [pnp_left]
        jmp     short pnp_resbyte
pnx_out:
        stc
        ret

;=====================================================================
; pnp_scan - read card BL's resource data and remember each IDE
;            controller in it.
;
; A device's ports come as descriptors, in the order its registers
; take them: the first is 60h's and the second is 62h's.  They may be
; inside DEPENDENT FUNCTIONS, which are whole alternative sets - this
; address with that one, or these two instead.  A channel is one the
; device may be put at if ONE set has its command block in the first
; descriptor and its control port in the second.
;=====================================================================
pnp_scan:
        pushad
        mov     byte ptr [pnp_csn],bl
        mov     ah,bl
        mov     al,PNP_R_WAKE
        call    pnp_wr
        mov     dword ptr [pnp_left],PNP_MAXRES

        mov     edi,OFFSET pnp_item             ; the identifier again,
        mov     ecx,9                           ; nine bytes of it
psc_id:
        call    pnp_next
        jc      psc_out
        mov     byte ptr [edi],al
        inc     edi
        dec     ecx
        jnz     psc_id
        mov     eax,dword ptr [pnp_item]
        mov     dword ptr [pnp_vendor],eax
        mov     byte ptr [pnp_ld],0FFh          ; no device yet
        mov     byte ptr [pnp_iside],0

psc_tag:
        call    pnp_next
        jc      psc_close
        test    al,80h
        jnz     psc_large

        mov     bl,al
        shr     bl,3                            ; BL = the tag
        movzx   ecx,al
        and     ecx,7                           ; ECX = bytes that follow
        xor     eax,eax
        mov     dword ptr [pnp_item],eax
        mov     dword ptr [pnp_item+4],eax
        mov     edi,OFFSET pnp_item
        jecxz   psc_got
psc_body:
        call    pnp_next
        jc      psc_close
        mov     byte ptr [edi],al
        inc     edi
        dec     ecx
        jnz     psc_body
psc_got:
        cmp     bl,PNP_T_END
        je      psc_close
        cmp     bl,PNP_T_LD
        je      psc_ld
        cmp     bl,PNP_T_COMPAT
        je      psc_compat
        cmp     bl,PNP_T_DEP
        je      psc_dep
        cmp     bl,PNP_T_ENDDEP
        je      psc_enddep
        cmp     bl,PNP_T_IO
        je      psc_io
        cmp     bl,PNP_T_FIXED
        je      psc_fixed
        jmp     short psc_tag

        ; a large item: a name, or memory.  Nothing here wants one
psc_large:
        call    pnp_next
        jc      psc_close
        movzx   ecx,al
        call    pnp_next
        jc      psc_close
        mov     ch,al
        or      ecx,ecx
        jz      psc_tag
psc_skip:
        call    pnp_next
        jc      psc_close
        dec     ecx
        jnz     psc_skip
        jmp     psc_tag

psc_ld:
        call    psc_endld                       ; the device before it
        inc     byte ptr [pnp_ld]
        xor     eax,eax
        mov     dword ptr [pnp_bbase],eax       ; four masks
        mov     dword ptr [pnp_ioidx],eax       ; two counts, a flag, PNF_OK
        mov     byte ptr [pnp_iside],al
psc_compat:
        mov     eax,dword ptr [pnp_item]
        mov     esi,OFFSET pnp_ideids
psc_which:
        cmp     dword ptr [esi],0
        je      psc_tag
        cmp     eax,dword ptr [esi]
        je      psc_isit
        add     esi,4
        jmp     short psc_which
psc_isit:
        mov     byte ptr [pnp_iside],1
        jmp     psc_tag

psc_dep:
        cmp     byte ptr [pnp_indep],0
        je      psc_first
        call    psc_endblk
psc_first:
        mov     byte ptr [pnp_indep],1
psc_again:
        mov     ax,word ptr [pnp_fbase]         ; what came before the
        mov     word ptr [pnp_bbase],ax         ; choices is in each of them
        mov     al,byte ptr [pnp_fixidx]
        mov     byte ptr [pnp_ioidx],al
        jmp     psc_tag

psc_enddep:
        call    psc_endblk
        mov     byte ptr [pnp_indep],0
        jmp     short psc_again

psc_io:
        movzx   eax,word ptr [pnp_item+1]       ; lowest base
        movzx   edx,word ptr [pnp_item+3]       ; highest
        movzx   ecx,byte ptr [pnp_item+5]       ; how far apart
        mov     bl,byte ptr [pnp_item+6]        ; how many ports
        jmp     short psc_range
psc_fixed:
        movzx   eax,word ptr [pnp_item]
        and     eax,03FFh
        mov     edx,eax
        xor     ecx,ecx
        mov     bl,byte ptr [pnp_item+2]
psc_range:
        mov     byte ptr [pnp_len],bl
        call    pnp_chans                       ; -> AL, AH
        cmp     byte ptr [pnp_ioidx],0
        jne     psc_second
        or      byte ptr [pnp_bbase],al
        jmp     short psc_count
psc_second:
        cmp     byte ptr [pnp_ioidx],1
        jne     psc_count
        or      byte ptr [pnp_bctl],ah
psc_count:
        inc     byte ptr [pnp_ioidx]
        cmp     byte ptr [pnp_indep],0
        jne     psc_tag
        mov     ax,word ptr [pnp_bbase]
        mov     word ptr [pnp_fbase],ax
        mov     al,byte ptr [pnp_ioidx]
        mov     byte ptr [pnp_fixidx],al
        jmp     psc_tag

psc_close:
        call    psc_endld
psc_out:
        popad
        ret

; psc_endblk - one set of resources has ended
psc_endblk:
        push    eax
        mov     al,byte ptr [pnp_bbase]
        and     al,byte ptr [pnp_bctl]
        or      byte ptr [pnp_ok],al
        pop     eax
        ret

; psc_endld - and so has a device
psc_endld:
        push    eax
        push    edi
        cmp     byte ptr [pnp_ld],0FFh
        je      pel_out
        cmp     byte ptr [pnp_iside],0
        je      pel_out
        call    psc_endblk
        movzx   eax,byte ptr [pnp_nfound]
        cmp     eax,PNP_MAXIDE
        jae     pel_out
        lea     edi,[pnp_found+eax*8]
        mov     al,byte ptr [pnp_csn]
        mov     byte ptr [edi+PNF_CSN],al
        mov     al,byte ptr [pnp_ld]
        mov     byte ptr [edi+PNF_LD],al
        mov     al,byte ptr [pnp_ok]
        mov     byte ptr [edi+PNF_OK],al
        mov     eax,dword ptr [pnp_vendor]
        mov     dword ptr [edi+PNF_VENDOR],eax
        inc     byte ptr [pnp_nfound]
pel_out:
        pop     edi
        pop     eax
        ret

;---------------------------------------------------------------------
; pnp_chans - which channels a port descriptor allows.
;             EAX = lowest base, EDX = highest, ECX = how far apart,
;             [pnp_len] = ports.  -> AL = a bit for each channel whose
;             command block it can be, AH = whose control port.
;             Never the first channel, which is the board's.
;---------------------------------------------------------------------
pnp_chans:
        push    ebx
        push    ecx
        push    esi
        push    edi
        push    ebp
        mov     esi,eax
        mov     edi,ecx
        or      edi,edi
        jnz     pch_apart
        inc     edi
pch_apart:
        xor     ebx,ebx
        mov     ecx,1
pch_lp:
        movzx   ebp,word ptr [ata_ports+ecx*4]
        call    pnp_inrange
        jc      pch_ctl
        cmp     byte ptr [pnp_len],8
        jb      pch_ctl
        bts     ebx,ecx
pch_ctl:
        movzx   ebp,word ptr [ata_ports+ecx*4+2]
        call    pnp_inrange
        jc      pch_next
        cmp     byte ptr [pnp_len],0
        je      pch_next
        lea     eax,[ecx+8]
        bts     ebx,eax
pch_next:
        inc     ecx
        cmp     ecx,ATA_MAXCHAN
        jb      pch_lp
        mov     eax,ebx
        pop     ebp
        pop     edi
        pop     esi
        pop     ecx
        pop     ebx
        ret

; pnp_inrange - CF clear if EBP is a base from ESI to EDX, EDI apart
pnp_inrange:
        push    eax
        push    edx
        cmp     ebp,esi
        jb      pir_no
        cmp     ebp,edx
        ja      pir_no
        mov     eax,ebp
        sub     eax,esi
        xor     edx,edx
        div     edi
        or      edx,edx
        jnz     pir_no
        pop     edx
        pop     eax
        clc
        ret
pir_no:
        pop     edx
        pop     eax
        stc
        ret

;=====================================================================
; pnp_setide - switch on the controller at ESI (PNF_*).
;
; One that is on already, at a channel ata_init looks at, is left as
; it is: a BIOS put it there.  Anything else is put at the first of
; pnp_order's channels that its resource data allows, that no other
; controller was given, and that nothing answers at now.
;=====================================================================
pnp_setide:
        pushad
        mov     ah,byte ptr [esi+PNF_CSN]
        mov     al,PNP_R_WAKE
        call    pnp_wr
        mov     ah,byte ptr [esi+PNF_LD]
        mov     al,PNP_R_LDN
        call    pnp_wr

        mov     al,PNP_R_IO0
        call    pnp_rd16
        mov     ebx,eax                         ; BX = its command block
        mov     al,PNP_R_IO1
        call    pnp_rd16
        mov     edx,eax                         ; DX = its control port
        mov     word ptr [pnp_old],0
        mov     al,PNP_R_ACT
        call    pnp_rd
        test    al,1
        jz      pse_pick
        mov     word ptr [pnp_old],bx

        xor     ecx,ecx
pse_where:
        cmp     bx,word ptr [ata_ports+ecx*4]
        jne     pse_wnext
        cmp     dx,word ptr [ata_ports+ecx*4+2]
        je      pse_kept
pse_wnext:
        inc     ecx
        cmp     ecx,ATA_MAXCHAN
        jb      pse_where
        jmp     short pse_pick
pse_kept:
        bts     dword ptr [pnp_used],ecx
        mov     al,'K'
        jmp     pse_log

pse_pick:
        mov     ax,PNP_R_ACT                    ; off, while it is moved
        call    pnp_wr
        mov     edi,OFFSET pnp_order
pse_try:
        movzx   ecx,byte ptr [edi]
        inc     edi
        cmp     cl,0FFh
        je      pse_none
        movzx   eax,byte ptr [esi+PNF_OK]
        bt      eax,ecx
        jnc     pse_try
        bt      dword ptr [pnp_used],ecx
        jc      pse_try
        call    pnp_chanfree
        jc      pse_try

        movzx   ebx,word ptr [ata_ports+ecx*4]
        movzx   edx,word ptr [ata_ports+ecx*4+2]
        mov     al,PNP_R_IO0
        mov     ah,bh
        call    pnp_wr
        mov     al,PNP_R_IO0 + 1
        mov     ah,bl
        call    pnp_wr
        mov     al,PNP_R_IO1
        mov     ah,dh
        call    pnp_wr
        mov     al,PNP_R_IO1 + 1
        mov     ah,dl
        call    pnp_wr
        mov     ax,PNP_R_IRQ0                   ; no interrupt
        call    pnp_wr
        mov     ax,PNP_R_IRQ1
        call    pnp_wr
        mov     ax,(1 SHL 8) OR PNP_R_ACT
        call    pnp_wr
        bts     dword ptr [pnp_used],ecx

        mov     ecx,ebx                         ; and what the card says it
        mov     ebp,edx                         ; has now
        mov     al,PNP_R_IO0
        call    pnp_rd16
        mov     ebx,eax
        mov     al,PNP_R_IO1
        call    pnp_rd16
        mov     edx,eax
        cmp     ebx,ecx
        jne     pse_bad
        cmp     edx,ebp
        jne     pse_bad
        mov     al,PNP_R_ACT
        call    pnp_rd
        test    al,1
        jz      pse_bad
        mov     al,'+'
        jmp     short pse_log
pse_bad:
        mov     al,'V'
        jmp     short pse_log
pse_none:
        xor     ebx,ebx
        xor     edx,edx
        mov     al,'T'

pse_log:
        cmp     byte ptr [plog+PLP_FIRST+PLP_STAGE],'+'
        je      pse_out                         ; one that worked is not
        cmp     byte ptr [plog+PLP_FIRST+PLP_STAGE],'K' ; written over
        je      pse_out
        mov     byte ptr [plog+PLP_FIRST+PLP_STAGE],al
        mov     word ptr [plog+PLP_FIRST+PLP_BASE],bx
        mov     word ptr [plog+PLP_FIRST+PLP_CTL],dx
        mov     ax,word ptr [pnp_old]
        mov     word ptr [plog+PLP_FIRST+PLP_OLD],ax
        mov     eax,dword ptr [esi+PNF_VENDOR]
        mov     dword ptr [plog+PLP_FIRST+PLP_VENDOR],eax
        mov     al,byte ptr [esi+PNF_CSN]
        mov     byte ptr [plog+PLP_FIRST+PLP_CSN],al
        mov     al,byte ptr [esi+PNF_LD]
        mov     byte ptr [plog+PLP_FIRST+PLP_LD],al
pse_out:
        popad
        ret

;---------------------------------------------------------------------
; pnp_chanfree - CF set if an IDE interface answers at channel ECX
;                already.  Two of its registers are latches: what is
;                written to them is read back.  The second write is
;                what makes an empty bus fail the test - a bus nobody
;                drives tends to read as the last thing driven on it,
;                and that is then AAh where 55h is looked for.
;---------------------------------------------------------------------
pnp_chanfree:
        push    eax
        push    edx
        mov     dx,word ptr [ata_ports+ecx*4]
        add     dx,ATA_SECCNT
        mov     al,55h
        out     dx,al
        inc     dx                              ; ATA_LBA0
        mov     al,0AAh
        out     dx,al
        dec     dx
        in      al,dx
        cmp     al,55h
        jne     pcf_free
        inc     dx
        in      al,dx
        cmp     al,0AAh
        jne     pcf_free
        pop     edx
        pop     eax
        stc
        ret
pcf_free:
        pop     edx
        pop     eax
        clc
        ret

;=====================================================================
; Data
;=====================================================================

; Where values might be read.  Clear of the game port, the sound card,
; the network card, both printer ports and all four serial ones
pnp_rdps        dw      020Bh, 0213h, 021Bh, 0273h, 026Bh
                dw      02A3h, 02B3h, 034Bh, 035Bh, 03A3h
                dw      0

; What an IDE controller calls itself, as the four bytes are read
pnp_ideids      dd      0006D041h               ; PNP0600, any of them
                dd      11208C0Eh               ; CTL2011, Creative's
                dd      0

; Channels, as ata_ports counts them, in the order they are offered:
; 1E8h, 168h, and 170h if the board has no second channel of its own
pnp_order       db      2, 3, 1, 0FFh

pnp_rdp         dw      0
pnp_old         dw      0
pnp_used        dd      0               ; a bit for each channel given out
pnp_left        dd      0
pnp_vendor      dd      0
pnp_cards       db      0
pnp_nfound      db      0
pnp_csn         db      0
pnp_ld          db      0
pnp_iside       db      0
pnp_len         db      0

; psc_ld clears these as two doublewords, and psc_again and psc_count
; copy the masks a pair at a time: THE ORDER IS PART OF THE CODE
pnp_bbase       db      0               ; this set: command block
pnp_bctl        db      0               ;           control port
pnp_fbase       db      0               ; the same, before any set began
pnp_fctl        db      0
pnp_ioidx       db      0               ; descriptors so far in this set
pnp_fixidx      db      0               ; ...and before any set
pnp_indep       db      0               ; 1 = inside a set
pnp_ok          db      0               ; PNF_OK as it builds

pnp_item        db      12 dup(0)       ; an item's bytes, or the identifier
pnp_found       db      PNP_MAXIDE * PNF_LEN dup(0)
