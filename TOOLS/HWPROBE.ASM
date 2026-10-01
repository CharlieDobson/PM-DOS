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
; kernel has to switch on before there is anything to probe.
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
        mov     ax,4C00h
        int     21h

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

; phex2 / phex3 - the low two or three hex digits of EAX
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
            db  'cards and from each IDE position while it was starting.',13,10,13,10
            db  '  HWPROBE [> file]',13,10,13,10
            db  'Numbers are hexadecimal.  -- was never read.',13,10,'$'
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

logbuf      db  PLOG_LEN dup(0)

_HP     ENDS
        END
