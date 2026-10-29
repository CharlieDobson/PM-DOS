;=====================================================================
; AHCITEST.ASM - AHCITEST.EXE, what did the kernel make of the SATA
;                ports?
;
; Prints the report IO\AHCI.INC keeps (21h/F1h AL=2Ah): the adapter,
; its registers as the BIOS left them, which of 80h and 81h the BIOS
; could read before the kernel took anything, and for each port how
; far it got and what the device said.  A disk that is still being
; read through the BIOS shows here as the stage it stopped at.
;
; It tests nothing and cannot fail.  It is for a machine that is
; somewhere else: the screen it prints is the report.
;
;   AHCITEST            to the screen
;   AHCITEST > file     ...or kept
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_AT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_AT, DS:_AT, ES:_AT, SS:_AT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET at_entry>,<0>,4096

at_entry:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
at_arg:
        or      ecx,ecx
        jz      at_go
        cmp     word ptr [esi],'?/'
        je      at_help
        inc     esi
        dec     ecx
        jmp     short at_arg
at_help:
        mov     edx,OFFSET msg_help
        call    puts
        mov     ax,4C00h
        int     21h

at_go:
        ; the thousands separator, the country's or a comma
        mov     byte ptr [sep],','
        mov     ax,3800h
        mov     edx,OFFSET ctry
        int     21h
        jc      at_sep
        mov     al,[ctry+7]
        or      al,al
        jz      at_sep
        mov     [sep],al
at_sep:
        mov     edi,OFFSET rpt
        mov     eax,0F12Ah
        int     21h
        jnc     at_have
        mov     edx,OFFSET msg_old
        call    puts
        mov     ax,4C01h
        int     21h
at_have:
        cmp     byte ptr [rpt+AHR_STATE],AHS_NONE
        jne     at_found
        mov     edx,OFFSET msg_none
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; the adapter
;=====================================================================
at_found:
        mov     edx,OFFSET msg_bus
        call    puts
        mov     eax,dword ptr [rpt+AHR_PCI]
        shr     eax,16
        call    phex2
        mov     edx,OFFSET msg_dev
        call    puts
        mov     eax,dword ptr [rpt+AHR_PCI]
        shr     eax,11
        and     eax,1Fh
        call    phex2
        mov     edx,OFFSET msg_fn
        call    puts
        mov     eax,dword ptr [rpt+AHR_PCI]
        shr     eax,8
        and     eax,7
        call    pdec
        mov     edx,OFFSET msg_id
        call    puts
        mov     eax,dword ptr [rpt+AHR_ID]
        call    phex4
        mov     dl,':'
        call    putc
        mov     eax,dword ptr [rpt+AHR_ID]
        shr     eax,16
        call    phex4
        call    crlf

        cmp     byte ptr [rpt+AHR_STATE],AHS_NOREGS
        je      at_state
        mov     edx,OFFSET msg_abar
        call    puts
        mov     eax,dword ptr [rpt+AHR_ABAR]
        call    phex8
        mov     edx,OFFSET msg_ver
        call    puts
        mov     eax,dword ptr [rpt+AHR_VS]
        shr     eax,16
        call    pdec
        mov     dl,'.'
        call    putc
        movzx   eax,byte ptr [rpt+AHR_VS+1]     ; 1.3 is 00010300h, and
        call    pdec                            ; 1.3.1 is 00010301h
        movzx   eax,byte ptr [rpt+AHR_VS]
        or      eax,eax
        jz      at_vdone
        mov     dl,'.'
        call    putc
        call    pdec
at_vdone:
        call    crlf
        mov     edx,OFFSET msg_cap
        call    puts
        mov     eax,dword ptr [rpt+AHR_CAP]
        call    phex8
        mov     edx,OFFSET msg_ghc
        call    puts
        mov     eax,dword ptr [rpt+AHR_GHC]
        call    phex8
        mov     edx,OFFSET msg_pi
        call    puts
        mov     eax,dword ptr [rpt+AHR_PI]
        call    phex8
        mov     edx,OFFSET msg_cap2
        call    puts
        mov     eax,dword ptr [rpt+AHR_CAP2]
        call    phex8
        mov     edx,OFFSET msg_bohc
        call    puts
        mov     eax,dword ptr [rpt+AHR_BOHC]
        call    phex8
        call    crlf

at_state:
        mov     edx,OFFSET msg_state
        call    puts
        movzx   eax,byte ptr [rpt+AHR_STATE]
        cmp     eax,AHS_NOAE
        jbe     at_stknown
        xor     eax,eax
at_stknown:
        mov     edx,dword ptr [states+eax*4]
        call    puts
        cmp     byte ptr [rpt+AHR_STATE],AHS_STARTED
        jne     at_nounits
        mov     edx,OFFSET msg_inuse
        call    puts
        movzx   eax,byte ptr [rpt+AHR_UNITS]
        call    pdec
        mov     edx,OFFSET msg_cdinuse
        call    puts
        xor     eax,eax                         ; the ports that are CD-ROM
        movzx   ecx,byte ptr [rpt+AHR_NREC]     ; units, counted
        mov     esi,OFFSET rpt + AHR_PORTS
at_cdcount:
        jecxz   at_cdcounted
        test    byte ptr [esi+AHP_FLAGS],AHPF_CD
        jz      at_cdnext
        inc     eax
at_cdnext:
        add     esi,AHP_LEN
        dec     ecx
        jmp     short at_cdcount
at_cdcounted:
        call    pdec
at_nounits:
        call    crlf

        ; what the BIOS read before anything was taken
        cmp     byte ptr [rpt+AHR_STATE],AHS_FOUND
        jbe     at_ports
        xor     ebx,ebx
at_bios:
        mov     edx,OFFSET msg_bios
        call    puts
        lea     eax,[ebx+80h]
        call    phex2
        mov     edx,OFFSET msg_h
        call    puts
        bt      dword ptr [rpt+AHR_BIOSOK],ebx
        jc      at_bread
        mov     edx,OFFSET msg_bnot
        call    puts
        jmp     short at_bnext
at_bread:
        mov     edx,OFFSET msg_bread
        call    puts
        mov     eax,dword ptr [rpt+AHR_BTOT0+ebx*4]
        or      eax,eax
        jz      at_bnosize
        mov     edx,OFFSET msg_comma
        call    puts
        call    pgrp
        mov     edx,OFFSET msg_sectors
        call    puts
at_bnosize:
        call    crlf
at_bnext:
        inc     ebx
        cmp     ebx,2
        jb      at_bios

;=====================================================================
; the ports
;=====================================================================
at_ports:
        movzx   ecx,byte ptr [rpt+AHR_NREC]
        or      ecx,ecx
        jz      at_done
        mov     edx,OFFSET msg_phead
        call    puts
        mov     esi,OFFSET rpt + AHR_PORTS
at_port:
        push    ecx
        mov     edx,OFFSET msg_port
        call    puts
        movzx   eax,byte ptr [esi+AHP_NUM]
        call    pdec
        mov     edx,OFFSET msg_colon
        call    puts
        call    pstage
        test    byte ptr [esi+AHP_FLAGS],AHPF_OWNED
        jz      at_nodrive
        mov     edx,OFFSET msg_as
        call    puts
        movzx   eax,byte ptr [esi+AHP_DRIVE]
        call    phex2
        mov     dl,'h'
        call    putc
at_nodrive:
        call    crlf

        test    byte ptr [esi+AHP_FLAGS],AHPF_CD
        jz      at_disk
        mov     edx,OFFSET msg_pad              ; a CD-ROM drive: its name,
        call    puts                            ; and how its data comes
        call    pmodel
        mov     edx,OFFSET msg_bypio
        test    byte ptr [esi+AHP_FLAGS],AHPF_PDMA
        jz      at_cdhow
        mov     edx,OFFSET msg_bydma
at_cdhow:
        call    puts
        call    crlf
        jmp     short at_nomodel
at_disk:
        cmp     dword ptr [esi+AHP_TOTAL],0
        je      at_nomodel
        mov     edx,OFFSET msg_pad
        call    puts
        call    pmodel
        mov     edx,OFFSET msg_comma
        call    puts
        mov     eax,dword ptr [esi+AHP_TOTAL]
        call    pgrp
        mov     edx,OFFSET msg_sectors
        call    puts
        test    byte ptr [esi+AHP_FLAGS],AHPF_LBA48
        jz      at_no48
        mov     edx,OFFSET msg_lba48
        call    puts
at_no48:
        call    crlf
at_nomodel:

        mov     edx,OFFSET msg_ssts
        call    puts
        mov     eax,dword ptr [esi+AHP_SSTS]
        call    phex8
        mov     edx,OFFSET msg_sig
        call    puts
        mov     eax,dword ptr [esi+AHP_SIG]
        call    phex8
        mov     edx,OFFSET msg_tfd
        call    puts
        mov     eax,dword ptr [esi+AHP_TFD]
        call    phex8
        mov     edx,OFFSET msg_match
        call    puts
        movzx   eax,byte ptr [esi+AHP_MATCH]
        call    phex2
        call    crlf
        mov     edx,OFFSET msg_bclb
        call    puts
        mov     eax,dword ptr [esi+AHP_BCLB]
        call    phex8
        mov     edx,OFFSET msg_bcmd
        call    puts
        mov     eax,dword ptr [esi+AHP_BCMD]
        call    phex8
        mov     edx,OFFSET msg_flags
        call    puts
        movzx   eax,byte ptr [esi+AHP_FLAGS]
        call    phex2
        call    crlf

        cmp     byte ptr [esi+AHP_ERRS],0
        je      at_noerr
        mov     edx,OFFSET msg_errs
        call    puts
        movzx   eax,byte ptr [esi+AHP_ERRS]
        call    pdec
        mov     edx,OFFSET msg_is
        call    puts
        mov     eax,dword ptr [esi+AHP_IS]
        call    phex8
        mov     edx,OFFSET msg_serr
        call    puts
        mov     eax,dword ptr [esi+AHP_SERR]
        call    phex8
        call    crlf
at_noerr:
        add     esi,AHP_LEN
        pop     ecx
        dec     ecx
        jnz     at_port
at_done:
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; pstage - ESI = a port's record: its stage, in words
;---------------------------------------------------------------------
pstage:
        push    eax
        push    edx
        mov     al,byte ptr [esi+AHP_STAGE]
        mov     edx,OFFSET stages
ps_lp:
        cmp     byte ptr [edx],0
        je      ps_odd
        cmp     al,byte ptr [edx]
        je      ps_hit
ps_skip:
        inc     edx
        cmp     byte ptr [edx],'$'
        jne     ps_skip
        inc     edx
        jmp     short ps_lp
ps_hit:
        inc     edx
        call    puts
        pop     edx
        pop     eax
        ret
ps_odd:
        mov     edx,OFFSET msg_stage
        call    puts
        mov     dl,al
        call    putc
        pop     edx
        pop     eax
        ret

;---------------------------------------------------------------------
; pmodel - ESI = a port's record: the model, trailing blanks dropped
;---------------------------------------------------------------------
pmodel:
        pushad
        lea     ebx,[esi+AHP_MODEL]
        mov     ecx,ATA_MODEL_LEN
pm_trim:
        cmp     byte ptr [ebx+ecx-1],' '
        je      pm_short
        cmp     byte ptr [ebx+ecx-1],0
        jne     pm_print
pm_short:
        loop    pm_trim
        jmp     short pm_out
pm_print:
        mov     dl,byte ptr [ebx]
        cmp     dl,' '
        jb      pm_dot
        cmp     dl,7Eh
        jbe     pm_put
pm_dot:
        mov     dl,'.'
pm_put:
        call    putc
        inc     ebx
        loop    pm_print
pm_out:
        popad
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
        mov     edi,OFFSET numbuf + 15
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

; pgrp - EAX in decimal, in groups of three with the country's separator
pgrp:
        pushad
        mov     edi,OFFSET numbuf + 15
        mov     byte ptr [edi],'$'
        mov     ecx,10
        xor     ebx,ebx                         ; EBX = digits so far
pg_lp:
        cmp     ebx,3
        jne     pg_digit
        dec     edi
        mov     dl,[sep]
        mov     [edi],dl
        xor     ebx,ebx
pg_digit:
        xor     edx,edx
        div     ecx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        inc     ebx
        or      eax,eax
        jnz     pg_lp
        mov     edx,edi
        call    puts
        popad
        ret

; phex2 / phex4 / phex8 - the low two, four or all eight hex digits of EAX
phex8:
        push    ecx
        mov     ecx,8
        jmp     short ph_go
phex4:
        push    ecx
        mov     ecx,4
        shl     eax,16
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
msg_help    db  'Shows what PM-DOS found on the AHCI (SATA) adapter and how far',13,10
            db  'it got with each port.',13,10,13,10
            db  '  AHCITEST [> file]',13,10,13,10
            db  'Numbers are hexadecimal, except sector counts and port numbers.',13,10,'$'
msg_old     db  'This kernel keeps no AHCI report.',13,10,'$'
msg_none    db  'No AHCI adapter found.',13,10,'$'

msg_bus     db  'AHCI adapter: bus ','$'
msg_dev     db  ' device ','$'
msg_fn      db  ' function ','$'
msg_id      db  ', ID ','$'
msg_abar    db  '  registers at ','$'
msg_ver     db  ', version ','$'
msg_cap     db  '  CAP ','$'
msg_ghc     db  '  GHC ','$'
msg_pi      db  '  PI ','$'
msg_cap2    db  '  CAP2 ','$'
msg_bohc    db  '  BOHC ','$'
msg_state   db  '  State: ','$'
msg_inuse   db  ', disks in use: ','$'
msg_cdinuse db  ', CD-ROM drives in use: ','$'
msg_bydma   db  ', blocks by DMA','$'
msg_bypio   db  ', by PIO','$'
msg_bios    db  '  BIOS drive ','$'
msg_h       db  'h: ','$'
msg_bnot    db  'not read',13,10,'$'
msg_bread   db  'sector 0 read','$'
msg_comma   db  ', ','$'
msg_sectors db  ' sectors','$'
msg_phead   db  13,10,'$'
msg_port    db  'Port ','$'
msg_colon   db  ': ','$'
msg_as      db  ', drive ','$'
msg_pad     db  '  ','$'
msg_lba48   db  ', LBA48','$'
msg_ssts    db  '  SSTS ','$'
msg_sig     db  '  SIG ','$'
msg_tfd     db  '  TFD ','$'
msg_match   db  '  matched ','$'
msg_bclb    db  '  BIOS list ','$'
msg_bcmd    db  '  BIOS CMD ','$'
msg_flags   db  '  flags ','$'
msg_errs    db  '  failed commands ','$'
msg_is      db  '  last IS ','$'
msg_serr    db  '  SERR ','$'
msg_stage   db  'stage ','$'
msg_crlf    db  13,10,'$'

st_none     db  'none','$'
st_found    db  'found, not started (AHCI=OFF or safe mode)','$'
st_started  db  'started','$'
st_noregs   db  'its registers could not be reached','$'
st_nohand   db  'the BIOS would not hand it over','$'
st_noae     db  'not in AHCI mode, left to the BIOS','$'
states      dd  OFFSET st_none, OFFSET st_found, OFFSET st_started
            dd  OFFSET st_noregs, OFFSET st_nohand, OFFSET st_noae

; a stage letter, then what it means
stages      db  'n','no device','$'
            db  'u','unknown device, left to the BIOS','$'
            db  'p','CD-ROM or other packet device, left to the BIOS','$'
            db  'c','CD-ROM drive in use','$'
            db  'e','CD-ROM or other packet device, left to the BIOS: it is reading a disk image from a CD','$'
            db  'q','packet device that is not a CD-ROM drive, left to the BIOS','$'
            db  'w','packet device, the port could not be taken from the BIOS','$'
            db  'k','packet device, IDENTIFY PACKET DEVICE failed','$'
            db  'y','CD-ROM drive that stopped answering, given back to the BIOS','$'
            db  'l','CD-ROM drive not taken back after Windows, left to the BIOS','$'
            db  'f','disk, not taken','$'
            db  't','disk, the port could not be taken from the BIOS','$'
            db  'i','disk, IDENTIFY failed','$'
            db  'z','disk with sectors that are not 512 bytes, left to the BIOS','$'
            db  'r','disk, sector 0 could not be read','$'
            db  'm','disk that matches no BIOS drive, left to the BIOS','$'
            db  'o','disk in use','$'
            db  'x','disk that stopped answering, given back to the BIOS','$'
            db  'j','disk not taken back after Windows, left to the BIOS','$'
            db  0

sep         db  ','
            db  0,0,0
numbuf      db  16 dup(0)
ctry        db  34 dup(0)
            db  0,0
rpt         db  AHR_LEN dup(0)

_AT     ENDS
        END
