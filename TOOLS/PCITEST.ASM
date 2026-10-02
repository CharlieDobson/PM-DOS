;=====================================================================
; PCITEST.ASM - PCITEST.EXE, what is on the PCI bus, and does the
;               kernel say the same as the bus does?
;
; 21h/F1h AL=12h is the kernel's account of the machine's buses: what
; pci_scan found at boot, the registers of any of it, how much a base
; address register decodes, which interrupt line a card was given and
; whether that line is level-triggered.  Every answer it gives about
; configuration space can be checked, because configuration space is
; two ports and neither of them is one the monitor keeps: this program
; asks the kernel and then asks the bus, and the two have to agree.
;
; It lists what it finds, which is the other half of its use - it is
; the only thing on a PM-DOS disk that says what cards are in the
; machine.
;
; Eleven checks:
;
;   1  the kernel answers, and says whether there is a bus at all -
;      without one there is nothing here to test, and the program
;      says so rather than failing
;   2  every function it lists is there when the bus is asked, with
;      the same identity and the same class
;   3  ...and the list ends where it says it does
;   4  each of them is found again by vendor and device, the second
;      of two alike as the second
;   5  and by class
;   6  a register read a byte, a word or a dword at a time is the
;      same register; a word that would not fit is refused
;   7  a byte written is the byte read back, and its neighbours in
;      the same dword are as they were
;   8  a base address register's size is a power of two, its base is
;      a multiple of that, and measuring it changed nothing
;   9  a card's memory can be mapped, where it is
;  10  the kernel's own memory is where it says it is, and an address
;      nothing is mapped at is reported as that
;  11  the timer's line is the kernel's, line 5 is not, and there is
;      no line 16
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_PT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_PT, DS:_PT, ES:_PT, SS:_PT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET pt_entry>,<0>,4096

CFG_ADDR        EQU     0CF8h
CFG_DATA        EQU     0CFCh
MAXLIST         EQU     64
NOWHERE         EQU     3FC00000h       ; the last 4MB of the first
                                        ; gigabyte, on a machine whose
                                        ; memory ends below it - see
                                        ; check 10 for the others

; The function goes in the third byte of EAX: AH is F1h and AL is 12h
BUS     MACRO   fn
        mov     eax,0F112h + (fn SHL 16)
        int     21h
        ENDM

pt_entry:
        mov     edx,OFFSET msg_hi
        call    puts

;=====================================================================
; 1 - is there a bus?
;=====================================================================
        mov     edx,OFFSET msg_t1
        call    puts
        BUS     BUS_PCIHERE
        jc      t1_bad
        mov     [hasbus],eax
        mov     [lastbus],ebx
        mov     [nfuncs],ecx
        mov     [elcr],edx
        call    good
        jmp     short t1_done
t1_bad:
        call    bad
        jmp     finish
t1_done:
        cmp     dword ptr [hasbus],0
        jne     t1_bus
        mov     dl,'-'
        call    tally
        mov     byte ptr [skipped],1
        mov     edx,OFFSET msg_nobus
        call    puts
        jmp     t11
t1_bus:
        mov     edx,OFFSET msg_buses
        call    puts
        mov     eax,[lastbus]
        inc     eax
        call    pdec
        mov     edx,OFFSET msg_fns
        call    puts
        mov     eax,[nfuncs]
        call    pdec
        mov     edx,OFFSET msg_level
        call    puts
        mov     eax,[elcr]
        call    phex4
        mov     edx,OFFSET msg_crlf
        call    puts

;=====================================================================
; 2 - the list, against the bus
;=====================================================================
        xor     esi,esi
t2_lp:
        cmp     esi,[nfuncs]
        jae     t2_end
        cmp     esi,MAXLIST
        jae     t2_end
        push    esi
        BUS     BUS_PCIENUM
        pop     esi
        jc      t2_wrong
        mov     [dev_addr + esi*4],ebx
        mov     [dev_id + esi*4],eax
        mov     [dev_class + esi*4],ecx

        push    eax
        push    ecx
        call    showdev
        pop     ecx
        pop     eax

        xor     dl,dl                   ; and what the bus says it is
        call    direct
        cmp     edi,eax
        jne     t2_wrong
        mov     dl,08h
        call    direct
        cmp     edi,ecx
        jne     t2_wrong
        inc     esi
        jmp     short t2_lp
t2_wrong:
        mov     byte ptr [wrong],1
t2_end:
        mov     edx,OFFSET msg_t2
        call    puts
        call    verdict

;=====================================================================
; 3 - and no further
;=====================================================================
        mov     edx,OFFSET msg_t3
        call    puts
        mov     esi,[nfuncs]
        BUS     BUS_PCIENUM
        jnc     t3_bad
        call    good
        jmp     t4
t3_bad:
        call    bad

;=====================================================================
; 4 - found again by what it is called
;=====================================================================
t4:
        xor     esi,esi
t4_lp:
        call    listed
        jae     t4_end
        mov     eax,[dev_id + esi*4]
        mov     edi,OFFSET dev_id
        call    nth                     ; ECX = how many before it are alike
        push    esi
        mov     esi,ecx
        movzx   ecx,ax                  ; the vendor
        mov     edx,eax
        shr     edx,16                  ; and the device
        BUS     BUS_PCIID
        pop     esi
        jc      t4_wrong
        cmp     ebx,[dev_addr + esi*4]
        jne     t4_wrong
        inc     esi
        jmp     short t4_lp
t4_wrong:
        mov     byte ptr [wrong],1
t4_end:
        mov     edx,OFFSET msg_t4
        call    puts
        call    verdict

;=====================================================================
; 5 - and by what it is
;=====================================================================
        xor     esi,esi
t5_lp:
        call    listed
        jae     t5_end
        mov     eax,[dev_class + esi*4]
        shr     eax,8
        mov     edi,OFFSET dev_class
        call    nthclass
        push    esi
        mov     esi,ecx
        mov     ecx,eax
        BUS     BUS_PCICLASS
        pop     esi
        jc      t5_wrong
        cmp     ebx,[dev_addr + esi*4]
        jne     t5_wrong
        inc     esi
        jmp     short t5_lp
t5_wrong:
        mov     byte ptr [wrong],1
t5_end:
        mov     edx,OFFSET msg_t5
        call    puts
        call    verdict

;=====================================================================
; 6 - one register, three widths
;=====================================================================
        mov     edx,OFFSET msg_t6
        call    puts
        mov     ebx,[dev_addr]
        mov     edx,0400h               ; four bytes at 0
        BUS     BUS_PCIREAD
        jc      t6_bad
        cmp     eax,[dev_id]
        jne     t6_bad
        mov     ebx,[dev_addr]
        mov     edx,0202h               ; two at 2: the device
        BUS     BUS_PCIREAD
        jc      t6_bad
        mov     ecx,[dev_id]
        shr     ecx,16
        cmp     eax,ecx
        jne     t6_bad
        mov     ebx,[dev_addr]
        mov     edx,010Bh               ; one at 0Bh: the class
        BUS     BUS_PCIREAD
        jc      t6_bad
        mov     ecx,[dev_class]
        shr     ecx,24
        cmp     eax,ecx
        jne     t6_bad
        mov     ebx,[dev_addr]
        mov     edx,0203h               ; two at 3, which is three bytes
        BUS     BUS_PCIREAD             ; of one dword and one of the next
        jnc     t6_bad
        call    good
        jmp     t7
t6_bad:
        call    bad

;=====================================================================
; 7 - a byte written, and only that byte
;
; The interrupt line register is the one to write: it is a byte the
; BIOS leaves a note in and no hardware reads, so changing it for a
; moment changes nothing.  The three bytes beside it in the same dword
; - the pin and two timers - are read-only, and must not move.
;=====================================================================
t7:
        mov     edx,OFFSET msg_t7
        call    puts
        xor     esi,esi
t7_find:
        call    listed
        jae     t7_none
        mov     ebx,[dev_addr + esi*4]
        BUS     BUS_PCIIRQ
        jc      t7_next
        or      ah,ah                   ; a pin: the register is there
        jnz     t7_have
t7_next:
        inc     esi
        jmp     short t7_find
t7_none:
        mov     dl,'-'
        call    tally
        mov     edx,OFFSET msg_skip
        call    puts
        jmp     t8
t7_have:
        mov     ebx,[dev_addr + esi*4]
        mov     [wdev],ebx
        mov     dl,3Ch
        call    direct
        mov     [wold],edi

        mov     ecx,edi
        xor     cl,05h
        and     ecx,0FFh
        mov     [wnew],ecx
        mov     ebx,[wdev]
        mov     edx,013Ch
        BUS     BUS_PCIWRITE
        jc      t7_bad
        mov     ebx,[wdev]
        mov     dl,3Ch
        call    direct
        mov     [wseen],edi

        mov     ecx,[wold]              ; and back as it was
        and     ecx,0FFh
        mov     ebx,[wdev]
        mov     edx,013Ch
        BUS     BUS_PCIWRITE
        jc      t7_bad
        mov     ebx,[wdev]
        mov     dl,3Ch
        call    direct
        cmp     edi,[wold]
        jne     t7_bad

        mov     eax,[wseen]
        mov     ecx,[wold]
        xor     eax,ecx
        and     eax,0FFFFFF00h          ; the neighbours
        jnz     t7_bad
        mov     eax,[wseen]
        and     eax,0FFh
        cmp     eax,[wnew]
        jne     t7_bad
        call    good
        jmp     t8
t7_bad:
        call    bad

;=====================================================================
; 8 - what each card decodes
;=====================================================================
t8:
        mov     byte ptr [wrong],0
        xor     esi,esi
t8_dev:
        call    listed
        jae     t8_end
        mov     dword ptr [barno],0
t8_bar:
        mov     ebx,[dev_addr + esi*4]
        mov     dl,04h                  ; the command register, and the
        call    direct                  ; register itself, as they are now
        mov     [cmdwas],edi
        mov     edx,[barno]
        shl     edx,2
        add     edx,10h
        call    direct
        mov     [barwas],edi

        mov     ebx,[dev_addr + esi*4]
        mov     edx,[barno]
        push    esi
        BUS     BUS_PCIBAR
        pop     esi
        jc      t8_nextdev              ; it has no such register
        mov     [barbase],eax
        mov     [barsize],ecx
        mov     [barflags],edx
        mov     [barhigh],edi

        mov     ebx,[dev_addr + esi*4]
        mov     edx,[barno]
        shl     edx,2
        add     edx,10h
        call    direct
        cmp     edi,[barwas]
        jne     t8_wrong
        mov     dl,04h
        call    direct
        xor     edi,[cmdwas]
        and     edi,0FFFFh
        jnz     t8_wrong

        test    dword ptr [barflags],BARF_NONE
        jnz     t8_nextbar
        call    showbar
        mov     eax,[barsize]
        or      eax,eax
        jz      t8_wrong
        mov     ecx,eax
        dec     ecx
        test    eax,ecx                 ; a power of two has one bit
        jnz     t8_wrong
        test    dword ptr [barbase],ecx ; and the base is a multiple of it
        jnz     t8_wrong

        ; the first one that is memory, above the first megabyte and
        ; somewhere the BIOS put it, is the one check 9 maps
        test    dword ptr [barflags],BARF_IO OR BARF_64
        jnz     t8_nextbar
        cmp     dword ptr [barbase],PG_MAXLIN
        jb      t8_nextbar
        cmp     dword ptr [mapbase],0
        jne     t8_nextbar
        mov     eax,[barbase]
        mov     [mapbase],eax
        mov     eax,[barsize]
        mov     [mapsize],eax
        jmp     short t8_nextbar
t8_wrong:
        mov     byte ptr [wrong],1
t8_nextbar:
        test    dword ptr [barflags],BARF_64
        jz      t8_one
        inc     dword ptr [barno]       ; the other half of a 64-bit one
t8_one:
        inc     dword ptr [barno]
        cmp     dword ptr [barno],6
        jb      t8_bar
t8_nextdev:
        inc     esi
        jmp     t8_dev
t8_end:
        mov     edx,OFFSET msg_t8
        call    puts
        call    verdict

;=====================================================================
; 9 - a card's memory, mapped where it is
;=====================================================================
        mov     edx,OFFSET msg_t9
        call    puts
        cmp     dword ptr [mapbase],0
        jne     t9_have
        mov     dl,'-'
        call    tally
        mov     edx,OFFSET msg_skip
        call    puts
        jmp     t10
t9_have:
        mov     ebx,[mapbase]
        mov     ecx,[mapsize]
        BUS     BUS_MAPPHYS
        jc      t9_bad
        cmp     eax,[mapbase]
        jne     t9_bad
        mov     ebx,[mapbase]           ; and the page tables agree
        mov     ecx,[mapsize]
        BUS     BUS_PHYSOF
        jc      t9_bad
        cmp     eax,[mapbase]
        jne     t9_bad
        cmp     ecx,[mapsize]
        jne     t9_bad
        call    good
        mov     edx,OFFSET msg_mapped
        call    puts
        mov     eax,[mapbase]
        call    phex8
        mov     edx,OFFSET msg_for
        call    puts
        mov     eax,[mapsize]
        shr     eax,10
        call    pdec
        mov     edx,OFFSET msg_k
        call    puts
        jmp     t10
t9_bad:
        call    bad

;=====================================================================
; 10 - where memory is
;=====================================================================
t10:
        mov     edx,OFFSET msg_t10
        call    puts
        mov     ebx,IO_PM_BASE          ; PMIO itself: where it says
        mov     ecx,8000h
        BUS     BUS_PHYSOF
        jc      t10_bad
        cmp     eax,IO_PM_BASE
        jne     t10_bad
        cmp     ecx,8000h
        jne     t10_bad

        ; AND AN ADDRESS NOTHING MAPS IS NOT ANYWHERE.  Which address
        ; that is depends on the machine now that every byte of memory
        ; is mapped: NOWHERE on one that ends below it, and otherwise
        ; the first 4MB past the end of memory.  The tables for memory
        ; above 256MB sit just above the arena and are less than 4MB,
        ; so 8MB past the arena's end, rounded down, is past them.
        mov     ax,0F104h               ; EBX = the first byte past the arena
        int     21h
        jc      t10_bad
        add     ebx,00800000h
        jc      t10_bad
        and     ebx,NOT 003FFFFFh
        cmp     ebx,NOWHERE
        jae     t10_past
        mov     ebx,NOWHERE
t10_past:
        mov     ecx,1000h
        BUS     BUS_PHYSOF
        jnc     t10_bad
        call    good
        jmp     t11
t10_bad:
        call    bad

;=====================================================================
; 11 - whose line is whose
;=====================================================================
t11:
        mov     edx,OFFSET msg_t11
        call    puts
        xor     edx,edx
        BUS     BUS_IRQSTAT
        jc      t11_bad
        test    eax,IRQF_KERNEL
        jz      t11_bad
        mov     edx,5
        BUS     BUS_IRQSTAT
        jc      t11_bad
        test    eax,IRQF_KERNEL
        jnz     t11_bad
        mov     edx,16
        BUS     BUS_IRQSTAT
        jnc     t11_bad
        call    good
        jmp     t11_show
t11_bad:
        call    bad
t11_show:
        call    showlines

finish:
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET results
        call    puts
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        je      fin_ok
        mov     edx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h
fin_ok:
        cmp     byte ptr [skipped],0
        je      fin_pass
        mov     edx,OFFSET msg_skipped
        call    puts
        mov     ax,4C00h
        int     21h
fin_pass:
        mov     edx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; direct - EBX = a device, DL = a register -> EDI = the dword the BUS
;          gives.  The address port is put back as it was found, the
;          way the kernel does it and for the same reason.
;=====================================================================
direct:
        push    eax
        push    ecx
        push    edx
        movzx   ecx,dl
        and     ecx,0FCh
        or      ecx,ebx
        or      ecx,80000000h
        mov     dx,CFG_ADDR
        in      eax,dx
        push    eax
        mov     eax,ecx
        out     dx,eax
        mov     dx,CFG_DATA
        in      eax,dx
        mov     edi,eax
        pop     eax
        mov     dx,CFG_ADDR
        out     dx,eax
        pop     edx
        pop     ecx
        pop     eax
        ret

;--- listed - ESI = an index.  "Below" if it is one of the functions
;--- this program has a record of.
listed:
        push    eax
        mov     eax,[nfuncs]
        cmp     eax,MAXLIST
        jbe     ls_cmp
        mov     eax,MAXLIST
ls_cmp:
        cmp     esi,eax
        pop     eax
        ret

;--- nth - EAX = a value, EDI -> the table it is entry ESI of -> ECX =
;--- how many entries before that one hold the same.
nth:
        push    edx
        xor     ecx,ecx
        xor     edx,edx
nth_lp:
        cmp     edx,esi
        jae     nth_out
        cmp     [edi + edx*4],eax
        jne     nth_next
        inc     ecx
nth_next:
        inc     edx
        jmp     short nth_lp
nth_out:
        pop     edx
        ret

;--- nthclass - the same for a class, which is the top three bytes of
;--- its entry: EAX = class, subclass and prog-if.
nthclass:
        push    edx
        push    ebx
        xor     ecx,ecx
        xor     edx,edx
ntc_lp:
        cmp     edx,esi
        jae     ntc_out
        mov     ebx,[edi + edx*4]
        shr     ebx,8
        cmp     ebx,eax
        jne     ntc_next
        inc     ecx
ntc_next:
        inc     edx
        jmp     short ntc_lp
ntc_out:
        pop     ebx
        pop     edx
        ret

;=====================================================================
; showdev - EBX = address, EAX = identity, ECX = class: one line of
;           the list, with the interrupt it was given.
;=====================================================================
showdev:
        pushad
        mov     [line_id],eax
        mov     [line_class],ecx
        mov     [line_addr],ebx
        mov     edx,OFFSET msg_sp
        call    puts
        mov     eax,[line_addr]
        shr     eax,16
        call    phex2
        mov     dl,':'
        call    putc
        mov     eax,[line_addr]
        shr     eax,11
        and     eax,1Fh
        call    phex2
        mov     dl,'.'
        call    putc
        mov     eax,[line_addr]
        shr     eax,8
        and     eax,7
        add     al,'0'
        mov     dl,al
        call    putc
        mov     edx,OFFSET msg_sp2
        call    puts
        mov     eax,[line_id]
        call    phex4                   ; the vendor
        mov     dl,':'
        call    putc
        mov     eax,[line_id]
        shr     eax,16
        call    phex4
        mov     edx,OFFSET msg_class
        call    puts
        mov     eax,[line_class]
        shr     eax,24
        call    phex2
        mov     eax,[line_class]
        shr     eax,16
        call    phex2
        mov     eax,[line_class]
        shr     eax,8
        call    phex2
        mov     ebx,[line_addr]
        BUS     BUS_PCIIRQ
        jc      sd_eol
        or      ah,ah
        jz      sd_eol
        mov     [line_irq],eax
        mov     [line_lvl],edx
        mov     edx,OFFSET msg_irq
        call    puts
        movzx   eax,byte ptr [line_irq]
        call    pdec
        mov     edx,OFFSET msg_pin
        call    puts
        mov     dl,byte ptr [line_irq + 1]
        add     dl,'A' - 1
        call    putc
        cmp     dword ptr [line_lvl],0
        je      sd_eol
        mov     edx,OFFSET msg_lvl
        call    puts
sd_eol:
        mov     edx,OFFSET msg_crlf
        call    puts
        popad
        ret

;=====================================================================
; showbar - the register in barno, from barbase, barsize and barflags
;=====================================================================
showbar:
        pushad
        mov     edx,OFFSET msg_bar
        call    puts
        mov     eax,[barno]
        add     al,'0'
        mov     dl,al
        call    putc
        mov     edx,OFFSET msg_mem
        test    dword ptr [barflags],BARF_IO
        jz      sb_kind
        mov     edx,OFFSET msg_io
sb_kind:
        call    puts
        test    dword ptr [barflags],BARF_64
        jz      sb_base
        mov     eax,[barhigh]
        call    phex8
sb_base:
        mov     eax,[barbase]
        call    phex8
        mov     edx,OFFSET msg_size
        call    puts
        mov     eax,[barsize]
        call    phex8
        test    dword ptr [barflags],BARF_PREF
        jz      sb_eol
        mov     edx,OFFSET msg_pref
        call    puts
sb_eol:
        mov     edx,OFFSET msg_crlf
        call    puts
        popad
        ret

;=====================================================================
; showlines - the sixteen lines, the ones with something to say
;=====================================================================
showlines:
        pushad
        xor     esi,esi
sl_lp:
        mov     edx,esi
        BUS     BUS_IRQSTAT
        jc      sl_next
        mov     [line_irq],eax
        mov     [line_id],ebx
        mov     [line_class],ecx
        mov     [line_lvl],edx
        test    eax,NOT IRQF_OPEN
        jnz     sl_show
        or      ebx,ebx
        jz      sl_next
sl_show:
        mov     edx,OFFSET msg_line
        call    puts
        mov     eax,esi
        call    pdec
        mov     dl,':'
        call    putc
        mov     edi,OFFSET flagnames
        mov     ecx,1
sl_flag:
        test    dword ptr [line_irq],ecx
        jz      sl_noflag
        mov     edx,edi
        call    puts
sl_noflag:
        add     edi,FLAGNAME
        shl     ecx,1
        cmp     ecx,100h
        jb      sl_flag
        mov     edx,OFFSET msg_taken
        call    puts
        mov     eax,[line_id]
        call    pdec
        mov     edx,OFFSET msg_tobox
        call    puts
        mov     eax,[line_class]
        call    pdec
        mov     edx,OFFSET msg_lost
        call    puts
        mov     eax,[line_lvl]
        call    pdec
        mov     edx,OFFSET msg_crlf
        call    puts
sl_next:
        inc     esi
        cmp     esi,16
        jb      sl_lp
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

phex8:
        push    eax
        push    ecx
        mov     ecx,8
        jmp     short ph_go
phex4:
        push    eax
        push    ecx
        shl     eax,16
        mov     ecx,4
        jmp     short ph_go
phex2:
        push    eax
        push    ecx
        shl     eax,24
        mov     ecx,2
ph_go:
        push    edx
ph_lp:
        rol     eax,4
        push    eax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     ph_d
        add     al,7
ph_d:
        mov     dl,al
        call    putc
        pop     eax
        dec     ecx
        jnz     ph_lp
        pop     edx
        pop     ecx
        pop     eax
        ret

pdec:
        pushad
        mov     edi,OFFSET decbuf + 11
        mov     byte ptr [edi],'$'
        mov     ebx,10
pd_lp:
        xor     edx,edx
        div     ebx
        add     dl,'0'
        dec     edi
        mov     [edi],dl
        or      eax,eax
        jnz     pd_lp
        mov     edx,edi
        call    puts
        popad
        ret

;=====================================================================
verdict:                                ; by what the loop above left
        cmp     byte ptr [wrong],0
        mov     byte ptr [wrong],0
        jne     bad
good:
        push    edx
        mov     dl,'.'
        call    tally
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
bad:
        push    edx
        mov     dl,'X'
        call    tally
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     edx
        ret
tally:
        push    ebx
        mov     ebx,[resptr]
        mov     [ebx],dl
        inc     ebx
        mov     byte ptr [ebx],'$'
        mov     [resptr],ebx
        pop     ebx
        ret

;=====================================================================
msg_hi      db  'PCITEST - what is on the PCI bus?',13,10,13,10,'$'
msg_t1      db  ' 1 the kernel answers                     ','$'
msg_t2      db  ' 2 the bus says the same                  ','$'
msg_t3      db  ' 3 and the list ends there                ','$'
msg_t4      db  ' 4 each is found by vendor and device     ','$'
msg_t5      db  ' 5 and by class                           ','$'
msg_t6      db  ' 6 a register in three widths             ','$'
msg_t7      db  ' 7 a byte written, and only that byte     ','$'
msg_t8      db  ' 8 what each card decodes                 ','$'
msg_t9      db  ' 9 a card''s memory can be mapped          ','$'
msg_t10     db  '10 memory is where it is said to be       ','$'
msg_t11     db  '11 whose interrupt line is whose          ','$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_skip    db  'skipped',13,10,'$'
msg_nobus   db  13,10,'  This machine has no PCI bus.',13,10,13,10,'$'
msg_buses   db  '    buses: ','$'
msg_fns     db  ', functions: ','$'
msg_level   db  ', level-triggered lines: ','$'
msg_sp      db  '    ','$'
msg_sp2     db  '  ','$'
msg_class   db  '  class ','$'
msg_irq     db  '  IRQ ','$'
msg_pin     db  ' pin ','$'
msg_lvl     db  ' level','$'
msg_bar     db  '      register ','$'
msg_mem     db  ': memory ','$'
msg_io      db  ': ports  ','$'
msg_size    db  ' size ','$'
msg_pref    db  ' prefetchable','$'
msg_mapped  db  '    mapped ','$'
msg_for     db  ' for ','$'
msg_k       db  'K',13,10,'$'
msg_line    db  '    line ','$'
msg_taken   db  ' taken ','$'
msg_tobox   db  ', to a box ','$'
msg_lost    db  ', unclaimed ','$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_skipped db  'PCITEST: no PCI bus on this machine to test',13,10,'$'
msg_pass    db  'PCITEST: all checks passed',13,10,'$'
msg_fail    db  'PCITEST: FAILURES',13,10,'$'

; The IRQF_* flags in bit order, eight bytes a name
FLAGNAME        EQU     8
flagnames   db  ' kernel$'
            db  ' driver$'
            db  ' box   $'
            db  ' owed  $'
            db  ' served$'
            db  ' level $'
            db  ' open  $'
            db  ' DEAD  $'

hasbus      dd  0
lastbus     dd  0
nfuncs      dd  0
elcr        dd  0
barno       dd  0
barbase     dd  0
barhigh     dd  0
barsize     dd  0
barflags    dd  0
barwas      dd  0
cmdwas      dd  0
mapbase     dd  0
mapsize     dd  0
wdev        dd  0
wold        dd  0
wnew        dd  0
wseen       dd  0
line_addr   dd  0
line_id     dd  0
line_class  dd  0
line_irq    dd  0
line_lvl    dd  0
wrong       db  0
skipped     db  0
fails       db  0
            db  0
decbuf      db  12 dup(0)
resptr      dd  OFFSET results
results     db  16 dup('$')
dev_addr    dd  MAXLIST dup(0)
dev_id      dd  MAXLIST dup(0)
dev_class   dd  MAXLIST dup(0)

_PT     ENDS
        END
