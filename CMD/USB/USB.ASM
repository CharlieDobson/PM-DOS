;=====================================================================
; USB.ASM - USB.EXE, what is on the USB bus
;
;   USB             the controllers, the devices on them, and the disk
;                   units with their drive letters
;
; The kernel drives the controllers itself (IO\USB.INC) and there is
; no driver to load and nothing that prints when a device is plugged
; in: a stick simply answers at its letter.  This is the one place to
; see what the kernel found - which controller, which port, what the
; device calls itself, and which letter a disk has - and the first
; thing to run when a device is not where it should be.
;
; What it asks:
;   21h/F1h AL=24h  USBF_HC, USBF_DEVICE, USBF_UNIT: the kernel's
;                   tables, as they stand
;   21h/F1h AL=6    the volume table, letter by letter: a DRVF_USB
;                   row's DRV_BIOS is the unit's drive number
;   21h/38h         the country's thousands separator
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

LETTERS         EQU     26

_USB    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_USB, DS:_USB, ES:_USB, SS:_USB
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET usb_entry>,<0>,4096

usb_entry:
;--- /? is the only argument there is ---------------------------------
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
ua_lp:
        or      ecx,ecx
        jz      ua_done
        mov     al,[esi]
        cmp     al,0Dh
        je      ua_done
        cmp     al,' '
        je      ua_next
        cmp     al,9
        je      ua_next
        cmp     al,'/'
        jne     ua_bad
        cmp     byte ptr [esi+1],'?'
        je      ua_help
ua_bad:
        mov     edx,OFFSET msg_badparm
        call    print
        mov     ax,4C01h
        int     21h
ua_help:
        mov     edx,OFFSET msg_usage
        call    print
        mov     ax,4C00h
        int     21h
ua_next:
        inc     esi
        dec     ecx
        jmp     short ua_lp
ua_done:

;--- the thousands separator, for the sizes ---------------------------
        mov     edx,OFFSET ctry
        mov     ax,3800h
        int     21h
        jc      ua_nosep
        mov     al,[ctry+7]
        or      al,al
        jz      ua_nosep
        mov     [sep],al
ua_nosep:

;=====================================================================
; The controllers
;=====================================================================
        xor     edx,edx
        mov     edi,OFFSET hc
        mov     eax,(USBF_HC SHL 16) OR 0F124h
        int     21h
        mov     [n_hc],ecx
        or      ecx,ecx
        jnz     uc_some
        mov     edx,OFFSET msg_none
        call    print
        mov     ax,4C01h
        int     21h
uc_some:
        mov     edx,OFFSET msg_hchead
        call    print
        xor     ebx,ebx
uc_one:
        cmp     ebx,[n_hc]
        jae     uc_done
        mov     edx,ebx
        mov     edi,OFFSET hc
        mov     eax,(USBF_HC SHL 16) OR 0F124h
        int     21h
        jc      uc_next
        mov     edx,OFFSET msg_2sp
        call    print
        lea     eax,[ebx+1]
        call    pdec
        mov     edx,OFFSET msg_2sp
        call    print
        movzx   eax,byte ptr [hc+USBH_KIND]
        call    kindname
        mov     edx,OFFSET msg_at
        call    print
        mov     eax,dword ptr [hc+USBH_BASE]
        cmp     byte ptr [hc+USBH_KIND],USBK_UHCI
        jne     uc_mem
        call    phex16
        jmp     short uc_based
uc_mem:
        call    phex32
uc_based:
        mov     edx,OFFSET msg_h
        call    print
        cmp     byte ptr [hc+USBH_KIND],USBK_EHCI
        je      uc_ehci
        mov     edx,OFFSET msg_comma
        call    print
        movzx   eax,byte ptr [hc+USBH_NPORTS]
        call    pdec
        mov     edx,OFFSET msg_ports
        call    print
        test    byte ptr [hc+USBH_FLAGS],USBHF_OWNED
        jnz     uc_owned
        mov     edx,OFFSET msg_notdriven
        call    print
        jmp     short uc_eol
uc_owned:
        test    byte ptr [hc+USBH_FLAGS],USBHF_LEGACY
        jz      uc_eol
        mov     edx,OFFSET msg_legacy
        call    print
        jmp     short uc_eol
uc_ehci:
        mov     edx,OFFSET msg_ehci
        test    byte ptr [hc+USBH_FLAGS],USBHF_OWNED
        jnz     uc_ehcisay
        mov     edx,OFFSET msg_ehcioff
uc_ehcisay:
        call    print
uc_eol:
        call    crlf
uc_next:
        inc     ebx
        jmp     uc_one
uc_done:

;=====================================================================
; The devices
;=====================================================================
        mov     byte ptr [any_dev],0
        xor     ebx,ebx
ud_one:
        cmp     ebx,USB_MAXDEV
        jae     ud_done
        mov     edx,ebx
        mov     edi,OFFSET dev
        mov     eax,(USBF_DEVICE SHL 16) OR 0F124h
        int     21h
        jc      ud_next
        cmp     byte ptr [dev+USBD_STATE],USBS_FREE
        je      ud_next
        cmp     byte ptr [any_dev],0
        jne     ud_row
        mov     byte ptr [any_dev],1
        mov     edx,OFFSET msg_devhead
        call    print
ud_row:
        ; where: controller:port, or hub slot's port
        mov     edx,OFFSET msg_2sp
        call    print
        mov     dword ptr [col],0
        cmp     byte ptr [dev+USBD_PARENT],0FFh
        jne     ud_onhub
        movzx   eax,byte ptr [dev+USBD_HC]
        inc     eax
        call    pdecc
        mov     edx,OFFSET msg_colon
        call    printc
        movzx   eax,byte ptr [dev+USBD_PORT]
        call    pdecc
        jmp     short ud_where
ud_onhub:
        mov     edx,OFFSET msg_hubword
        call    printc
        movzx   eax,byte ptr [dev+USBD_PARENT]
        inc     eax
        call    pdecc
        mov     edx,OFFSET msg_colon
        call    printc
        movzx   eax,byte ptr [dev+USBD_PORT]
        call    pdecc
ud_where:
        mov     ecx,9
        call    padto
        ; what
        movzx   eax,byte ptr [dev+USBD_CLASS]
        cmp     byte ptr [dev+USBD_STATE],USBS_READY
        je      ud_class
        mov     eax,5                   ; not in service: say so instead
        cmp     byte ptr [dev+USBD_STATE],USBS_DEAD
        je      ud_class
        mov     eax,6
ud_class:
        mov     edx,[classnames+eax*4]
        call    printc
        mov     ecx,20
        call    padto
        ; the speed
        mov     edx,OFFSET msg_full
        test    byte ptr [dev+USBD_FLAGS],USBDF_LOWSPEED
        jz      ud_speed
        mov     edx,OFFSET msg_low
ud_speed:
        call    printc
        mov     ecx,32
        call    padto
        ; its name, or its numbers
        cmp     byte ptr [dev+USBD_NAME],0
        je      ud_ids
        mov     esi,OFFSET dev + USBD_NAME
        mov     ecx,32
        call    trimz
        call    putn
        jmp     short ud_eol
ud_ids:
        movzx   eax,word ptr [dev+USBD_VID]
        call    phex16
        mov     edx,OFFSET msg_colon
        call    print
        movzx   eax,word ptr [dev+USBD_PID]
        call    phex16
ud_eol:
        call    crlf
ud_next:
        inc     ebx
        jmp     ud_one
ud_done:
        cmp     byte ptr [any_dev],0
        jne     ud_had
        mov     edx,OFFSET msg_nodev
        call    print
ud_had:

;=====================================================================
; The disk units
;=====================================================================
        mov     edx,OFFSET msg_unithead
        call    print
        xor     ebx,ebx
uu_one:
        cmp     ebx,USB_MAXUNITS
        jae     uu_done
        mov     edx,ebx
        mov     edi,OFFSET model
        mov     eax,(USBF_UNIT SHL 16) OR 0F124h
        int     21h
        jc      uu_done
        mov     [uflags],al
        mov     [usecs],ebx
        mov     [ualias],ch             ; the BIOS drive it stands in for
        mov     ebx,edx
        ; the letter
        mov     edx,OFFSET msg_2sp
        call    print
        mov     eax,ebx
        call    pdec
        mov     edx,OFFSET msg_2sp
        call    print
        cmp     byte ptr [ualias],0
        je      uu_ownletter
        mov     al,[ualias]             ; the BIOS drive's first letter
        mov     byte ptr [lo_any],1
        call    letterof
        mov     byte ptr [lo_any],0
        jc      uu_ownletter            ; no letters there: the USB one
        jmp     short uu_hasletter
uu_ownletter:
        lea     eax,[ebx+USB_DRV0]
        call    letterof                ; -> AL, or CF
        jc      uu_noletter
uu_hasletter:
        add     al,'A'
        mov     [msg_letter],al
        mov     edx,OFFSET msg_letter
        call    print
        jmp     short uu_what
uu_noletter:
        mov     edx,OFFSET msg_noletter
        call    print
uu_what:
        test    byte ptr [uflags],USBUF_ATTACHED
        jz      uu_empty
        test    byte ptr [uflags],USBUF_NOTDISK
        jnz     uu_notdisk
        mov     esi,OFFSET model
        mov     ecx,24
        call    trim
        call    putn
        test    byte ptr [uflags],USBUF_READY
        jz      uu_unasked
        mov     edx,OFFSET msg_2sp
        call    print
        mov     eax,[usecs]
        shr     eax,11                  ; sectors -> megabytes
        call    pgroup
        mov     edx,OFFSET msg_mb
        call    print
        test    byte ptr [uflags],USBUF_WRPROT
        jz      uu_eol
        mov     edx,OFFSET msg_wprot
        call    print
        jmp     short uu_eol
uu_unasked:
        mov     edx,OFFSET msg_unasked
        call    print
        jmp     short uu_eol
uu_notdisk:
        mov     edx,OFFSET msg_notdisk
        call    print
        jmp     short uu_eol
uu_empty:
        mov     edx,OFFSET msg_empty
        call    print
uu_eol:
        call    crlf
        inc     ebx
        jmp     uu_one
uu_done:
        mov     ax,4C00h
        int     21h

;=====================================================================
; letterof - AL = a unit's drive number -> AL = its letter (0 = A), or
;            CF.  21h/F1h AL=6, row by row.
;=====================================================================
letterof:
        push    ebx
        push    ecx
        push    edx
        push    edi
        mov     [lo_want],al            ; not in a register: the call below
        xor     ebx,ebx                 ; answers ECX = the row count
lo_lp:
        cmp     ebx,LETTERS
        jae     lo_no
        mov     edx,ebx
        mov     edi,OFFSET drv
        mov     eax,0F106h
        int     21h
        jc      lo_no
        mov     al,[drv+DRV_FLAGS]
        test    al,DRVF_PRESENT
        jz      lo_next
        cmp     byte ptr [lo_any],0     ; any row, for a BIOS drive's
        jne     lo_cmp
        test    al,DRVF_USB
        jz      lo_next
lo_cmp:
        mov     al,[lo_want]
        cmp     [drv+DRV_BIOS],al
        je      lo_hit
lo_next:
        inc     ebx
        jmp     short lo_lp
lo_hit:
        mov     al,bl
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
lo_no:
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;--- kindname - EAX = USBK_* -> its name printed
kindname:
        push    edx
        mov     edx,OFFSET msg_uhci
        cmp     al,USBK_UHCI
        je      kn_say
        mov     edx,OFFSET msg_ohci
        cmp     al,USBK_OHCI
        je      kn_say
        mov     edx,OFFSET msg_ehcik
kn_say:
        call    print
        pop     edx
        ret

;=====================================================================
; Output
;=====================================================================
print:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

; printc - print, counting the characters into col
printc:
        push    eax
        push    edx
pc_lp:
        cmp     byte ptr [edx],'$'
        je      pc_out
        inc     dword ptr [col]
        inc     edx
        jmp     short pc_lp
pc_out:
        pop     edx
        pop     eax
        jmp     print

putc:
        push    eax
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     eax
        ret

; padto - blanks until col reaches ECX, and one at least
padto:
        push    eax
        mov     al,' '
        call    putc
        inc     dword ptr [col]
pt_lp:
        cmp     dword ptr [col],ecx
        jae     pt_out
        call    putc
        inc     dword ptr [col]
        jmp     short pt_lp
pt_out:
        pop     eax
        ret

; putn - ECX characters at ESI
putn:
        push    ecx
        push    esi
pn_lp:
        jecxz   pn_out
        mov     al,[esi]
        call    putc
        inc     esi
        dec     ecx
        jmp     short pn_lp
pn_out:
        pop     esi
        pop     ecx
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    print
        pop     edx
        ret

; trim  - ESI -> ECX bytes, space padded -> ECX = the length without
;         the trailing blanks
; trimz - the same for an ASCIIZ string of at most ECX bytes
trimz:
        push    esi
        push    edx
        xor     edx,edx
tz_lp:
        cmp     edx,ecx
        jae     tz_end
        cmp     byte ptr [esi+edx],0
        je      tz_end
        inc     edx
        jmp     short tz_lp
tz_end:
        mov     ecx,edx
        pop     edx
        pop     esi
trim:
        jecxz   tr_out
        cmp     byte ptr [esi+ecx-1],' '
        ja      tr_out
        dec     ecx
        jmp     short trim
tr_out:
        ret

; pdecc - pdec, counting the digits into col
pdecc:
        push    eax
        push    ecx
        push    edx
        mov     ecx,10
pdc_lp:
        inc     dword ptr [col]
        xor     edx,edx
        div     ecx
        or      eax,eax
        jnz     pdc_lp
        pop     edx
        pop     ecx
        pop     eax
        ; and on into pdec, which prints it

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
        call    print
        popad
        ret

; pgroup - EAX in decimal with the country's separator every three
pgroup:
        pushad
        mov     edi,OFFSET numbuf + 15
        mov     byte ptr [edi],'$'
        mov     ecx,10
        xor     ebx,ebx                 ; digits so far
pg_lp:
        or      ebx,ebx
        jz      pg_digit
        push    eax
        mov     eax,ebx
        xor     edx,edx
        push    ecx
        mov     ecx,3
        div     ecx
        pop     ecx
        or      edx,edx
        pop     eax
        jnz     pg_digit
        dec     edi
        mov     dl,[sep]
        mov     [edi],dl
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
        call    print
        popad
        ret

; phex16 / phex32 - AX / EAX in hexadecimal
phex32:
        push    ecx
        mov     ecx,8
        jmp     short ph_go
phex16:
        push    ecx
        mov     ecx,4
        rol     eax,16
ph_go:
        push    eax
        push    edx
        mov     edx,OFFSET numbuf
ph_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_dig
        add     al,7
ph_dig:
        mov     [edx],al
        pop     eax
        inc     edx
        loop    ph_lp
        mov     byte ptr [edx],'$'
        mov     edx,OFFSET numbuf
        call    print
        pop     edx
        pop     eax
        pop     ecx
        ret

;=====================================================================
; Data
;=====================================================================
n_hc        dd  0
col         dd  0
usecs       dd  0
uflags      db  0
any_dev     db  0
sep         db  ','
            db  0
hc          db  USBH_LEN dup(0)
dev         db  USBD_LEN dup(0)
drv         db  DRV_LEN dup(0)
lo_want     db  0                       ; the drive number letterof looks for
lo_any      db  0                       ; ...on any row, not only USB rows
ualias      db  0                       ; the BIOS drive a unit stands in for
model       db  24 dup(0)
ctry        db  40 dup(0)
numbuf      db  16 dup(0)

classnames  dd  OFFSET msg_other, OFFSET msg_kbd, OFFSET msg_mouse
            dd  OFFSET msg_disk, OFFSET msg_hub, OFFSET msg_dead
            dd  OFFSET msg_settingup

msg_usage   db  'Lists the USB controllers, the devices on them and the disk units.',13,10
            db  13,10
            db  'USB',13,10,'$'
msg_badparm db  'Invalid parameter',13,10,'$'
msg_none    db  'No USB controller.',13,10,'$'
msg_hchead  db  'USB controllers:',13,10,'$'
msg_devhead db  13,10,'Devices:',13,10,'$'
msg_nodev   db  13,10,'No USB device is plugged in.',13,10,'$'
msg_unithead db 13,10,'Disk units:',13,10,'$'
msg_uhci    db  'UHCI$'
msg_ohci    db  'OHCI$'
msg_ehcik   db  'EHCI$'
msg_at      db  ' at $'
msg_h       db  'h$'
msg_comma   db  ', $'
msg_ports   db  ' ports$'
msg_notdriven db ', not driven (USB=OFF)$'
msg_legacy  db  ', taken from the BIOS$'
msg_ehci    db  ': ports handed down to its UHCI or OHCI$'
msg_ehcioff db  ', not touched$'
msg_2sp     db  '  $'
msg_colon   db  ':$'
msg_hubword db  'hub $'
msg_other   db  'other$'
msg_kbd     db  'keyboard$'
msg_mouse   db  'mouse$'
msg_disk    db  'disk$'
msg_hub     db  'hub$'
msg_dead    db  'failed$'
msg_settingup db 'setting up$'
msg_full    db  'full speed$'
msg_low     db  'low speed$'
msg_letter  db  'D:  $'
msg_noletter db '--  $'
msg_empty   db  'no disk$'
msg_notdisk db  'not a disk this kernel can use$'
msg_unasked db  '  (not asked yet)$'
msg_mb      db  ' MB$'
msg_wprot   db  '  write protected$'
msg_crlf    db  13,10,'$'

_USB    ENDS
        END
