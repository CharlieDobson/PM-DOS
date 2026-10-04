;=====================================================================
; HWPROBE.ASM - HWPROBE.EXE, what did the boot-time probes see?
;
; A mouse or a CD-ROM drive that PM-DOS did not find has left nothing
; in the machine to ask about, so the two probes write down what they
; read as they go (IO\PROBELOG.INC) and this prints it: the bytes the
; keyboard controller and the mouse answered with, and for each of the
; eight IDE positions how far IDENTIFY got, the status register at
; every step, and the signature the device left.  Between the two,
; what ISA Plug and Play found: an IDE port on a sound card is one the
; kernel has to switch on before there is anything to probe.  Then the
; SCSI adapters and every device that answered on each (21h/F1h
; AL=1Ch), which is the SCSI probe's report.  Last, what ACPI gave for
; switching the power off (21h/F1h AL=1Eh, PWF_QUERY): the ports
; SHUTDOWN writes and the sleep type it writes into them.
;
; It tests nothing and cannot fail.  What it is for is a machine that
; is somewhere else: the screen it prints is the report.
;
;   HWPROBE             to the screen
;   HWPROBE > file      ...or kept
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_HP     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_HP, DS:_HP, ES:_HP, SS:_HP
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET hp_entry>,<0>,4096

hp_entry:
        ; /? is the only argument there is
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
hp_arg:
        or      ecx,ecx
        jz      hp_go
        cmp     word ptr [esi],'?/'
        je      hp_help
        inc     esi
        dec     ecx
        jmp     short hp_arg
hp_help:
        mov     edx,OFFSET msg_help
        call    puts
        mov     ax,4C00h
        int     21h

hp_go:
        mov     edx,OFFSET msg_hi
        call    puts
        mov     edi,OFFSET logbuf
        mov     eax,0F115h
        int     21h
        jnc     hp_have
        mov     edx,OFFSET msg_old
        call    puts
        mov     ax,4C01h
        int     21h
hp_have:

;=====================================================================
; the mouse
;=====================================================================
        mov     edx,OFFSET msg_mouse
        call    puts
        mov     edx,OFFSET msg_mnone
        cmp     byte ptr [logbuf+PLM_HW],MS_PS2
        jne     hp_mser
        mov     edx,OFFSET msg_mps2
hp_mser:
        cmp     byte ptr [logbuf+PLM_HW],MS_SERIAL
        jne     hp_mwhat
        mov     edx,OFFSET msg_mcom
hp_mwhat:
        call    puts

        mov     edx,OFFSET msg_mstage
        call    puts
        mov     dl,byte ptr [logbuf+PLM_STAGE]
        call    putc
        mov     edx,OFFSET msg_mtries
        call    puts
        movzx   eax,byte ptr [logbuf+PLM_TRIES]
        call    phex2
        mov     edx,OFFSET msg_mequip
        call    puts
        movzx   eax,byte ptr [logbuf+PLM_EQUIP]
        call    phex2
        call    crlf

        mov     edx,OFFSET msg_mcmd
        call    puts
        mov     ebx,PLM_CMD0
        mov     ah,01h
        call    mfield
        mov     edx,OFFSET msg_moff
        call    puts
        mov     ebx,PLM_CMDOFF
        mov     ah,02h
        call    mfield
        mov     edx,OFFSET msg_mon
        call    puts
        mov     ebx,PLM_CMDON
        mov     ah,04h
        call    mfield
        mov     edx,OFFSET msg_mtest
        call    puts
        mov     ebx,PLM_TEST
        mov     ah,08h
        call    mfield
        call    crlf

        mov     edx,OFFSET msg_mack
        call    puts
        mov     ebx,PLM_ACK
        mov     ah,10h
        call    mfield
        mov     edx,OFFSET msg_mbat
        call    puts
        mov     ebx,PLM_BAT
        mov     ah,20h
        call    mfield
        mov     edx,OFFSET msg_mid
        call    puts
        mov     ebx,PLM_ID
        mov     ah,40h
        call    mfield
        mov     edx,OFFSET msg_men
        call    puts
        mov     ebx,PLM_ENABLE
        mov     ah,80h
        call    mfield
        call    crlf

;=====================================================================
; ISA Plug and Play
;=====================================================================
        mov     edx,OFFSET msg_pnp
        call    puts
        cmp     byte ptr [logbuf+PLP_FIRST+PLP_STAGE],'N'
        je      hp_pnone
        cmp     byte ptr [logbuf+PLP_FIRST+PLP_STAGE],0
        jne     hp_pcards
hp_pnone:
        mov     edx,OFFSET msg_pnone
        call    puts
        jmp     hp_pdone
hp_pcards:
        movzx   eax,byte ptr [logbuf+PLP_FIRST+PLP_CARDS]
        call    phex2
        mov     edx,OFFSET msg_prdp
        call    puts
        movzx   eax,word ptr [logbuf+PLP_FIRST+PLP_RDP]
        call    phex3
        mov     edx,OFFSET msg_mstage
        call    puts
        mov     dl,byte ptr [logbuf+PLP_FIRST+PLP_STAGE]
        call    putc
        call    crlf
        cmp     byte ptr [logbuf+PLP_FIRST+PLP_STAGE],'I'
        je      hp_pdone

        mov     edx,OFFSET msg_pide
        call    puts
        ; three letters in two bytes, five bits each, then two bytes
        ; as they are
        movzx   ebx,byte ptr [logbuf+PLP_FIRST+PLP_VENDOR]
        shl     ebx,8
        mov     bl,byte ptr [logbuf+PLP_FIRST+PLP_VENDOR+1]
        mov     ecx,10
hp_pname:
        mov     edx,ebx
        shr     edx,cl
        and     dl,1Fh
        add     dl,40h
        call    putc
        sub     ecx,5
        jns     hp_pname
        movzx   eax,byte ptr [logbuf+PLP_FIRST+PLP_VENDOR+2]
        call    phex2
        movzx   eax,byte ptr [logbuf+PLP_FIRST+PLP_VENDOR+3]
        call    phex2
        mov     edx,OFFSET msg_pcard
        call    puts
        movzx   eax,byte ptr [logbuf+PLP_FIRST+PLP_CSN]
        call    phex2
        mov     edx,OFFSET msg_pdev
        call    puts
        movzx   eax,byte ptr [logbuf+PLP_FIRST+PLP_LD]
        call    phex2
        mov     edx,OFFSET msg_pwas
        call    puts
        movzx   eax,word ptr [logbuf+PLP_FIRST+PLP_OLD]
        call    phex3
        mov     edx,OFFSET msg_pnow
        call    puts
        movzx   eax,word ptr [logbuf+PLP_FIRST+PLP_BASE]
        call    phex3
        mov     dl,'/'
        call    putc
        movzx   eax,word ptr [logbuf+PLP_FIRST+PLP_CTL]
        call    phex3
        call    crlf
hp_pdone:

;=====================================================================
; the eight IDE positions
;=====================================================================
        mov     edx,OFFSET msg_ide
        call    puts
        xor     ebp,ebp
hp_dev:
        cmp     ebp,8
        jae     hp_done
        mov     esi,ebp
        imul    esi,PLI_LEN
        add     esi,OFFSET logbuf + PLI_FIRST

        mov     edx,OFFSET msg_pad
        call    puts
        movzx   eax,word ptr [esi+PLI_BASE]
        call    phex3
        mov     edx,OFFSET msg_master
        test    ebp,1
        jz      hp_which
        mov     edx,OFFSET msg_slave
hp_which:
        call    puts

        mov     dl,byte ptr [esi+PLI_STAGE]
        or      dl,dl
        jnz     hp_stage
        mov     dl,'-'
hp_stage:
        call    putc
        mov     edx,OFFSET msg_flags
        call    puts
        movzx   eax,byte ptr [esi+PLI_FLAGS]
        call    phex2

        mov     edx,OFFSET msg_status
        call    puts
        mov     ebx,PLI_ST0
        mov     ah,01h
        call    ifield
        mov     dl,' '
        call    putc
        mov     ebx,PLI_ST1
        mov     ah,02h
        call    ifield
        mov     dl,' '
        call    putc
        mov     ebx,PLI_ST2
        mov     ah,04h
        call    ifield
        mov     dl,' '
        call    putc
        mov     ebx,PLI_ST3
        mov     ah,10h
        call    ifield

        mov     edx,OFFSET msg_sig
        call    puts
        test    byte ptr [esi+PLI_SEEN],08h
        jz      hp_nosig
        movzx   eax,byte ptr [esi+PLI_SIG+1]
        call    phex2
        movzx   eax,byte ptr [esi+PLI_SIG]
        call    phex2
        mov     edx,OFFSET msg_err
        call    puts
        movzx   eax,byte ptr [esi+PLI_ERR]
        call    phex2
        jmp     short hp_what
hp_nosig:
        mov     edx,OFFSET msg_dash4
        call    puts
        mov     edx,OFFSET msg_err
        call    puts
        mov     edx,OFFSET msg_dash2
        call    puts
hp_what:
        mov     edx,OFFSET msg_isdisk
        test    byte ptr [esi+PLI_FLAGS],ATDF_PRESENT
        jnz     hp_say
        mov     edx,OFFSET msg_iscd
        test    byte ptr [esi+PLI_FLAGS],ATDF_ATAPI
        jnz     hp_say
        mov     edx,OFFSET msg_crlf
hp_say:
        call    puts
        inc     ebp
        jmp     hp_dev

hp_done:
        mov     edx,OFFSET msg_key
        call    puts

;=====================================================================
; the SCSI adapters, and every device that answered INQUIRY on each -
; of any type, so that a disk on the bus shows as well as a CD-ROM.
; 21h/F1h AL=1Ch; a kernel without it answers with the carry, which
; reads as "none".
;=====================================================================
        mov     edx,OFFSET msg_scsi
        call    puts
        xor     ebx,ebx                 ; EBX = the adapter
hs_ad:
        mov     edx,ebx
        mov     edi,OFFSET hba
        mov     eax,0F11Ch + (SCF_ADAPTER SHL 16)
        int     21h
        jc      hs_adend
        mov     edx,OFFSET msg_pad
        call    puts
        mov     eax,ebx
        call    pdec
        mov     edx,OFFSET msg_pad
        call    puts
        mov     esi,OFFSET hba + HBA_NAME
        mov     ecx,16
hs_name:
        mov     dl,[esi]
        call    putc
        inc     esi
        loop    hs_name
        movzx   eax,word ptr [hba+HBA_BASE]
        or      eax,eax
        jz      hs_noport
        mov     edx,OFFSET msg_sport
        call    puts
        call    phex4
hs_noport:
        mov     al,[hba+HBA_IRQ]
        cmp     al,0FFh
        je      hs_noirq
        mov     edx,OFFSET msg_sirq
        call    puts
        movzx   eax,al
        call    pdec
hs_noirq:
        mov     al,[hba+HBA_DMA]
        cmp     al,0FFh
        je      hs_nodma
        mov     edx,OFFSET msg_sdma
        call    puts
        movzx   eax,al
        call    pdec
hs_nodma:
        mov     edx,OFFSET msg_sid
        call    puts
        movzx   eax,byte ptr [hba+HBA_ID]
        call    pdec
        cmp     byte ptr [hba+HBA_KIND],SHK_ASPI
        jne     hs_adeol
        mov     edx,OFFSET msg_saspi
        call    puts
hs_adeol:
        call    crlf

        ; what is on it
        mov     byte ptr [hs_seen],0
        xor     ebp,ebp                 ; EBP = the device row
hs_dev:
        mov     edx,ebp
        mov     edi,OFFSET sdv
        mov     eax,0F11Ch + (SCF_DEVICE SHL 16)
        int     21h
        jc      hs_devend
        cmp     [sdv+SDV_HBA],bl
        jne     hs_devnext
        mov     byte ptr [hs_seen],1
        mov     edx,OFFSET msg_sdid
        call    puts
        movzx   eax,byte ptr [sdv+SDV_TARGET]
        call    pdec
        cmp     byte ptr [sdv+SDV_LUN],0
        je      hs_nolun
        mov     edx,OFFSET msg_slun
        call    puts
        movzx   eax,byte ptr [sdv+SDV_LUN]
        call    pdec
hs_nolun:
        mov     dl,' '
        call    putc
        call    putc
        call    hs_type
        mov     esi,OFFSET sdv + SDV_INQ       ; vendor, product, revision,
        mov     ecx,8                           ; a blank between each
        call    hs_chars
        mov     ecx,16
        call    hs_chars
        mov     ecx,4
        call    hs_chars
        test    byte ptr [sdv+SDV_FLAGS],SDF_DUP
        jz      hs_devdone
        mov     edx,OFFSET msg_sdup
        call    puts
hs_devdone:
        call    crlf
hs_devnext:
        inc     ebp
        jmp     hs_dev
hs_devend:
        cmp     byte ptr [hs_seen],0
        jne     hs_adnext
        mov     edx,OFFSET msg_snone
        call    puts
hs_adnext:
        inc     ebx
        jmp     hs_ad
hs_adend:
        or      ebx,ebx
        jnz     hs_out
        mov     edx,OFFSET msg_snoad
        call    puts
hs_out:

;=====================================================================
; ACPI, as SHUTDOWN would use it: where the tables are, the ports it
; writes, and the sleep type it writes into them, with where in the
; DSDT that came from.  21h/F1h AL=1Eh with PWF_QUERY, which switches
; nothing off; the registers come back on "no soft-off" too.
;=====================================================================
        mov     edx,OFFSET msg_acpi
        call    puts
        mov     eax,0F11Eh + (PWF_QUERY SHL 16)
        int     21h
        mov     [a_eax],eax
        mov     [a_ebx],ebx
        mov     [a_ecx],ecx
        mov     [a_edx],edx
        mov     [a_esi],esi
        mov     [a_edi],edi
        jnc     ha_rsdp
        mov     edx,OFFSET msg_anone
        cmp     ax,ERR_NOACPI
        je      ha_say
        mov     edx,OFFSET msg_aold             ; a kernel without AL=1Eh
        cmp     ax,ERR_NOSOFTOFF
        jne     ha_say
        mov     edx,OFFSET msg_anooff
        call    puts
        jmp     short ha_regs
ha_say:
        call    puts
        jmp     ha_exit
ha_rsdp:
        mov     edx,OFFSET msg_arsdp
        call    puts
        mov     eax,[a_eax]
        call    phex8
        call    crlf
ha_regs:
        mov     edx,OFFSET msg_acnt
        call    puts
        movzx   eax,word ptr [a_ebx]
        call    aport
        mov     edx,OFFSET msg_anow
        call    puts
        movzx   eax,word ptr [a_ecx+2]
        call    phex4
        mov     edx,OFFSET msg_acntb
        call    puts
        movzx   eax,word ptr [a_ebx+2]
        call    aport
        call    crlf
        mov     edx,OFFSET msg_aevt
        call    puts
        movzx   eax,word ptr [a_esi]
        call    aport
        mov     edx,OFFSET msg_atmr
        call    puts
        movzx   eax,word ptr [a_esi+2]
        call    aport
        call    crlf
        mov     edx,OFFSET msg_asmi
        call    puts
        movzx   eax,word ptr [a_edx]
        call    aport
        mov     edx,OFFSET msg_aen
        call    puts
        movzx   eax,byte ptr [a_edx+2]
        call    phex2
        mov     edx,OFFSET msg_adis
        call    puts
        movzx   eax,byte ptr [a_edx+3]
        call    phex2
        call    crlf
        mov     eax,[a_edi]
        or      eax,eax
        jnz     ha_s5
        mov     edx,OFFSET msg_anos5
        call    puts
        jmp     short ha_exit
ha_s5:
        mov     edx,OFFSET msg_as5
        call    puts
        call    phex8
        mov     edx,OFFSET msg_atypes
        call    puts
        movzx   eax,byte ptr [a_ecx]
        call    pdec
        mov     dl,' '
        call    putc
        movzx   eax,byte ptr [a_ecx+1]
        call    pdec
        call    crlf
ha_exit:
        mov     ax,4C00h
        int     21h

; aport - EAX = a port in four hex digits, or "none" for 0
aport:
        or      eax,eax
        jz      ap_none
        jmp     phex4
ap_none:
        push    edx
        mov     edx,OFFSET msg_anoport
        call    puts
        pop     edx
        ret

;---------------------------------------------------------------------
; hs_chars - ECX characters from ESI, and a blank after them
;---------------------------------------------------------------------
hs_chars:
        mov     dl,[esi]
        call    putc
        inc     esi
        loop    hs_chars
        mov     dl,' '
        call    putc
        ret

;---------------------------------------------------------------------
; hs_type - the device type in [sdv+SDV_TYPE], as a word, in a column
;---------------------------------------------------------------------
hs_type:
        movzx   eax,byte ptr [sdv+SDV_TYPE]
        and     eax,1Fh
        cmp     eax,8
        jbe     ht_named
        mov     edx,OFFSET msg_tother
        call    puts
        call    phex2
        mov     edx,OFFSET msg_pad
        call    puts
        ret
ht_named:
        imul    eax,eax,10
        lea     edx,[msg_types+eax]
        call    puts
        ret

;---------------------------------------------------------------------
; mfield - the mouse log's byte at EBX, or "--" if bit AH of PLM_SEEN
;          says it was never read
;---------------------------------------------------------------------
mfield:
        test    byte ptr [logbuf+PLM_SEEN],ah
        jz      fld_none
        movzx   eax,byte ptr [logbuf+ebx]
        call    phex2
        ret

; ifield - the same for the device record at ESI
ifield:
        test    byte ptr [esi+PLI_SEEN],ah
        jz      fld_none
        movzx   eax,byte ptr [esi+ebx]
        call    phex2
        ret
fld_none:
        push    edx
        mov     edx,OFFSET msg_dash2
        call    puts
        pop     edx
        ret

;=====================================================================
; Output helpers
;=====================================================================
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

putc:
        push    eax
        mov     ah,02h
        int     21h
        pop     eax
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    puts
        pop     edx
        ret

; pdec - EAX in decimal, no padding
pdec:
        pushad
        mov     edi,OFFSET numbuf + 11
        mov     byte ptr [edi],'$'
        mov     ecx,10
pd_lp:
        xor     edx,edx
        div     ecx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        call    puts
        popad
        ret

; phex2 / phex3 / phex4 / phex8 - the low two, three, four or all eight
; hex digits of EAX
phex8:
        push    ecx
        mov     ecx,8
        jmp     short ph_go
phex4:
        push    ecx
        mov     ecx,4
        shl     eax,16
        jmp     short ph_go
phex3:
        push    ecx
        mov     ecx,3
        shl     eax,20
        jmp     short ph_go
phex2:
        push    ecx
        mov     ecx,2
        shl     eax,24
ph_go:
        push    edx
ph_lp:
        rol     eax,4
        mov     dl,al
        and     dl,0Fh
        add     dl,'0'
        cmp     dl,'9'
        jbe     ph_put
        add     dl,7
ph_put:
        call    putc
        dec     ecx
        jnz     ph_lp
        pop     edx
        pop     ecx
        ret

;=====================================================================
msg_hi      db  'HWPROBE - what the boot probes saw',13,10,'$'
msg_help    db  'Shows what PM-DOS read from the mouse port, from Plug and Play',13,10
            db  'cards and from each IDE position while it was starting, and',13,10
            db  'the SCSI adapters it found, with what answered on each, and',13,10
            db  'what ACPI gives SHUTDOWN to switch the power off with.',13,10,13,10
            db  '  HWPROBE [> file]',13,10,13,10
            db  'Numbers are hexadecimal, except IRQ, DMA and SCSI IDs.',13,10
            db  '-- was never read.',13,10,'$'
msg_old     db  'This kernel keeps no probe log.',13,10,'$'

msg_mouse   db  'Mouse: ','$'
msg_mnone   db  'none found',13,10,'$'
msg_mps2    db  'PS/2',13,10,'$'
msg_mcom    db  'serial',13,10,'$'
msg_mstage  db  '  stage ','$'
msg_mtries  db  '  resets ','$'
msg_mequip  db  '  equipment ','$'
msg_mcmd    db  '  command byte ','$'
msg_moff    db  '  after A7 ','$'
msg_mon     db  '  after A8 ','$'
msg_mtest   db  '  A9 ','$'
msg_mack    db  '  reset ','$'
msg_mbat    db  '  self-test ','$'
msg_mid     db  '  id ','$'
msg_men     db  '  enable ','$'

msg_pnp     db  13,10,'Plug and Play: ','$'
msg_pnone   db  'no cards',13,10,'$'
msg_prdp    db  ' cards  read at ','$'
msg_pide    db  '  IDE on ','$'
msg_pcard   db  '  card ','$'
msg_pdev    db  '  device ','$'
msg_pwas    db  '  was ','$'
msg_pnow    db  '  now ','$'

msg_ide     db  13,10,'IDE:     stage flags  status           signature',13,10,'$'
msg_pad     db  '  ','$'
msg_master  db  ' m  ','$'
msg_slave   db  ' s  ','$'
msg_flags   db  '     ','$'
msg_status  db  '     ','$'
msg_sig     db  '      ','$'
msg_err     db  ' err ','$'
msg_dash2   db  '--','$'
msg_dash4   db  '----','$'
msg_isdisk  db  '  disk',13,10,'$'
msg_iscd    db  '  CD-ROM',13,10,'$'
msg_key     db  13,10,'status: before, after select, after IDENTIFY, after IDENTIFY PACKET',13,10,'$'
msg_crlf    db  13,10,'$'

msg_scsi    db  13,10,'SCSI:',13,10,'$'
msg_snoad   db  '  no adapters',13,10,'$'
msg_sport   db  '  port ','$'
msg_sirq    db  '  IRQ ','$'
msg_sdma    db  '  DMA ','$'
msg_sid     db  '  own ID ','$'
msg_saspi   db  '  (ASPI)','$'
msg_sdid    db  '      ID ','$'
msg_slun    db  ' LUN ','$'
msg_snone   db  '      nothing answered',13,10,'$'
msg_sdup    db  '  (also seen another way)','$'
msg_tother  db  'type ','$'
msg_types   db  'disk     $'
            db  'tape     $'
            db  'printer  $'
            db  'processor$'
            db  'WORM     $'
            db  'CD-ROM   $'
            db  'scanner  $'
            db  'optical  $'
            db  'changer  $'

msg_acpi    db  13,10,'ACPI:',13,10,'$'
msg_anone   db  '  not found',13,10,'$'
msg_aold    db  '  this kernel cannot say',13,10,'$'
msg_anooff  db  '  found, with no soft-off',13,10,'$'
msg_arsdp   db  '  RSDP at ','$'
msg_acnt    db  '  PM1a control ','$'
msg_anow    db  ', now ','$'
msg_acntb   db  '   PM1b control ','$'
msg_aevt    db  '  PM1a event ','$'
msg_atmr    db  '   timer ','$'
msg_asmi    db  '  SMI command ','$'
msg_aen     db  '   enable ','$'
msg_adis    db  '   disable ','$'
msg_as5     db  '  \_S5_ at ','$'
msg_atypes  db  ', sleep types ','$'
msg_anos5   db  '  \_S5_ not found',13,10,'$'
msg_anoport db  'none','$'

hs_seen     db  0
            db  0,0,0
a_eax       dd  0                       ; what F1h AL=1Eh reported
a_ebx       dd  0
a_ecx       dd  0
a_edx       dd  0
a_esi       dd  0
a_edi       dd  0
numbuf      db  12 dup(0)
hba         db  HBA_LEN dup(0)
sdv         db  SDV_LEN dup(0)
logbuf      db  PLOG_LEN dup(0)

_HP     ENDS
        END
