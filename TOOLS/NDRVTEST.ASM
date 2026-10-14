;=====================================================================
; NDRVTEST.ASM - NDRVTEST.EXE, the 32-bit drivers asked through 21h/F1h
;
; A native program that asks the kernel what DEVICE= loaded above the
; megabyte (21h/F1h AL=25h, INC\NDRV.INC) and then asks NDTEST.SYS,
; which has to be one of them, for its report (TOOLS\NDTEST.INC):
; whether the loader relocated it, what INIT was given, whether the
; block it took holds its pattern, whether the clock reaches it,
; whether the line it hooked is the kernel's now - and then parks the
; kernel, with a 16-bit program on the other side, and asks again to
; see that PARK was called on the way down and on the way back.
;
; Every line is a check, and the last line is how many failed.
; Needs NDTEST.SYS loaded by CONFIG.SYS with /I:11 on its line, which
; is what RUNTESTS.PS1 does, and RMDOS.SYS where the kernel looks for
; it.  The program run on the far side is \TESTS\HELLO.COM, or the
; command tail.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC
        INCLUDE NDRV.INC
        INCLUDE NDTEST.INC

_NT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_NT, DS:_NT, ES:_NT, SS:_NT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET nt_entry>,<0>,4096

nt_entry:
        mov     edx,OFFSET msg_hi
        call    puts
        call    nt_arg

;--- how many ----------------------------------------------------------
        mov     edx,OFFSET msg_count
        call    puts
        mov     eax,(NDF_COUNT SHL 16) OR 0F125h
        int     21h
        jc      nt_nocount
        mov     [count],ecx
        mov     eax,ecx
        call    put4
        mov     edx,OFFSET msg_of
        call    puts
        mov     eax,ND_MAX
        call    put4
        cmp     dword ptr [count],1
        jb      nt_nocount
        call    ok
        jmp     short nt_rows
nt_nocount:
        call    fail
        jmp     nt_sum                  ; nothing else can be asked

;--- row 0 is NDTEST, above the megabyte --------------------------------
nt_rows:
        mov     edx,OFFSET msg_row
        call    puts
        mov     eax,(NDF_INFO SHL 16) OR 0F125h
        xor     edx,edx
        mov     edi,OFFSET row
        int     21h
        jc      nt_rowbad
        mov     esi,OFFSET row + ND_NAME
        mov     edi,OFFSET name_ndtest
        mov     ecx,8
        repe    cmpsb
        jne     nt_rowbad
        cmp     dword ptr [row+ND_BASE],ARENA_BASE
        jb      nt_rowbad
        cmp     dword ptr [row+ND_SIZE],D32H_SIZE + NDT_LEN
        jb      nt_rowbad
        call    ok
        jmp     short nt_norow
nt_rowbad:
        call    fail

;--- a row there is not --------------------------------------------------
nt_norow:
        mov     edx,OFFSET msg_norow
        call    puts
        mov     eax,(NDF_INFO SHL 16) OR 0F125h
        mov     edx,ND_MAX
        mov     edi,OFFSET row
        int     21h
        jnc     nt_norowbad
        call    put4
        cmp     ax,ERR_BADDRIVE
        jne     nt_norowbad
        call    ok
        jmp     short nt_report
nt_norowbad:
        call    fail

;--- the report ----------------------------------------------------------
nt_report:
        mov     edx,OFFSET msg_report
        call    puts
        call    nt_ask
        jc      nt_reportbad
        call    put4
        cmp     eax,NDTR_LEN
        jne     nt_reportbad
        call    ok
        jmp     short nt_reloc
nt_reportbad:
        call    fail
        jmp     nt_sum                  ; the rest reads the report

;--- relocated: the dword the linker wrote against where the code is ---
nt_reloc:
        mov     edx,OFFSET msg_reloc
        call    puts
        mov     eax,[report+NDTR_LINKED]
        cmp     eax,[report+NDTR_RUNNING]
        jne     nt_relocbad
        cmp     eax,ARENA_BASE
        jb      nt_relocbad
        call    ok
        jmp     short nt_hdr
nt_relocbad:
        call    fail

;--- the header's own base and entry, after the relocation ---------------
nt_hdr:
        mov     edx,OFFSET msg_hdr
        call    puts
        mov     eax,[report+NDTR_HDRBASE]
        cmp     eax,[row+ND_BASE]
        jne     nt_hdrbad
        mov     eax,[report+NDTR_HDRENTRY]
        cmp     eax,[report+NDTR_RUNNING]
        jne     nt_hdrbad
        cmp     eax,[row+ND_TABLE]
        jne     nt_hdrbad
        cmp     dword ptr [report+NDTR_NRELOC],2
        jb      nt_hdrbad
        call    ok
        jmp     short nt_init
nt_hdrbad:
        call    fail

;--- what INIT was given --------------------------------------------------
nt_init:
        mov     edx,OFFSET msg_init
        call    puts
        cmp     dword ptr [report+NDTR_SERVICES],0
        je      nt_initbad
        cmp     dword ptr [report+NDTR_ROW],0
        je      nt_initbad
        call    ok
        jmp     short nt_line
nt_initbad:
        call    fail

;--- the line ---------------------------------------------------------------
nt_line:
        mov     edx,OFFSET msg_line
        call    puts
        mov     edx,OFFSET report + NDTR_LINE
        call    putz
        cmp     byte ptr [report+NDTR_LINE],0
        je      nt_linebad
        call    ok
        jmp     short nt_block
nt_linebad:
        call    fail

;--- the block ---------------------------------------------------------------
nt_block:
        mov     edx,OFFSET msg_block
        call    puts
        cmp     dword ptr [report+NDTR_BLOCK],ARENA_BASE
        jb      nt_blockbad
        cmp     dword ptr [report+NDTR_BLOCKOK],1
        jne     nt_blockbad
        call    ok
        jmp     short nt_clock
nt_blockbad:
        call    fail

;--- the clock: hooked, and ticking between two reports -------------------
nt_clock:
        mov     edx,OFFSET msg_clock
        call    puts
        cmp     dword ptr [report+NDTR_TICKHOOK],0
        jne     nt_clockbad
        mov     eax,[report+NDTR_TICKS]
        mov     [ticks1],eax
        call    nt_second               ; a second of the clock
        call    nt_ask
        jc      nt_clockbad
        mov     eax,[report+NDTR_TICKS]
        cmp     eax,[ticks1]
        jbe     nt_clockbad
        call    ok
        jmp     short nt_irq
nt_clockbad:
        call    fail

;--- the line /I: named: hooked, quiet, and wanted by the kernel ----------
nt_irq:
        mov     edx,OFFSET msg_irq
        call    puts
        mov     eax,[report+NDTR_IRQ]
        call    put4
        cmp     eax,15
        ja      nt_irqbad
        cmp     dword ptr [report+NDTR_IRQHOOK],0
        jne     nt_irqbad
        cmp     dword ptr [report+NDTR_IRQS],0
        jne     nt_irqbad
        mov     edx,eax                 ; DL = the line
        mov     eax,(BUS_IRQSTAT SHL 16) OR 0F112h
        int     21h
        jc      nt_irqbad
        test    eax,IRQF_HOOKED
        jz      nt_irqbad
        test    eax,IRQF_OPEN
        jz      nt_irqbad
        call    ok
        jmp     short nt_badfn
nt_irqbad:
        call    fail

;--- an unknown function, and a short buffer, refused ------------------
nt_badfn:
        mov     edx,OFFSET msg_badfn
        call    puts
        mov     eax,(NDF_CALL SHL 16) OR 0F125h
        mov     ebx,NDC_REPORT + 1
        xor     edx,edx
        mov     ecx,NDTR_LEN
        mov     edi,OFFSET report
        int     21h
        jnc     nt_badfnbad
        call    put4
        cmp     ax,ERR_FUNC
        jne     nt_badfnbad
        call    ok
        jmp     short nt_short
nt_badfnbad:
        call    fail
nt_short:
        mov     edx,OFFSET msg_short
        call    puts
        mov     eax,(NDF_CALL SHL 16) OR 0F125h
        mov     ebx,NDC_REPORT
        xor     edx,edx
        mov     ecx,NDTR_LEN - 1
        mov     edi,OFFSET report
        int     21h
        jnc     nt_shortbad
        call    put4
        cmp     ax,ERR_BADDATA
        jne     nt_shortbad
        call    ok
        jmp     short nt_park
nt_shortbad:
        call    fail

;--- the park: PARK with 0 on the way down and 1 on the way back ---------
nt_park:
        mov     edx,OFFSET msg_parking
        call    puts
        mov     edx,[progptr]
        call    putz
        mov     edx,OFFSET msg_crlf
        call    puts
        mov     edx,[progptr]
        mov     ax,0F10Bh
        int     21h
        jc      nt_refused
        mov     edx,OFFSET msg_back
        call    puts
        call    nt_ask
        jc      nt_parkbad
        mov     eax,[report+NDTR_PARKS]
        call    put4
        mov     edx,OFFSET msg_and
        call    puts
        mov     eax,[report+NDTR_UNPARKS]
        call    put4
        cmp     dword ptr [report+NDTR_PARKS],1
        jne     nt_parkbad
        cmp     dword ptr [report+NDTR_UNPARKS],1
        jne     nt_parkbad
        ; the kernel's own count of the same calls, from the row
        mov     eax,(NDF_INFO SHL 16) OR 0F125h
        xor     edx,edx
        mov     edi,OFFSET row
        int     21h
        jc      nt_parkbad
        cmp     dword ptr [row+ND_PARKS],1
        jne     nt_parkbad
        cmp     dword ptr [row+ND_UNPARKS],1
        jne     nt_parkbad
        call    ok
        jmp     short nt_sum
nt_refused:
        mov     [exitcode],eax
        mov     edx,OFFSET msg_refused
        call    puts
        mov     eax,[exitcode]
        call    put4
nt_parkbad:
        call    fail

;--- the sum ------------------------------------------------------------------
nt_sum:
        mov     edx,OFFSET msg_failed
        call    puts
        mov     eax,[failures]
        call    put2
        mov     edx,OFFSET msg_crlf
        call    puts
        cmp     dword ptr [failures],0
        jne     nt_some
        mov     edx,OFFSET msg_allok
        call    puts
        jmp     short nt_done
nt_some:
        mov     edx,OFFSET msg_notok
        call    puts
nt_done:
        mov     edx,OFFSET msg_done
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; nt_ask - NDTEST's report into rep.  CF if the call failed; EAX = what
;          it answered.
;---------------------------------------------------------------------
nt_ask:
        mov     eax,(NDF_CALL SHL 16) OR 0F125h
        mov     ebx,NDC_REPORT
        xor     edx,edx                 ; row 0
        mov     ecx,NDTR_LEN
        mov     edi,OFFSET report
        int     21h
        ret

;=====================================================================
; nt_second - wait for the clock's seconds to change twice: between
;             one and two seconds, however the call landed in one.
;---------------------------------------------------------------------
nt_second:
        push    eax
        push    ebx
        push    ecx
        push    edx
        mov     ah,2Ch
        int     21h
        mov     [secs],dh
        mov     ebx,2                   ; not ECX: 2Ch answers the hour
nts_wait:                               ; and minute in CX
        mov     ah,2Ch
        int     21h
        cmp     dh,[secs]
        je      nts_wait
        mov     [secs],dh
        dec     ebx
        jnz     nts_wait
        pop     edx
        pop     ecx
        pop     ebx
        pop     eax
        ret

;=====================================================================
; nt_arg - the command tail, if there is one, into [progptr].
;---------------------------------------------------------------------
nt_arg:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        or      ecx,ecx
        jz      na_done
        inc     esi
        mov     edi,OFFSET argbuf
na_skip:
        cmp     byte ptr [esi],' '
        jne     na_copy
        inc     esi
        dec     ecx
        jnz     na_skip
        jmp     short na_done
na_copy:
        mov     al,[esi]
        cmp     al,' '
        jbe     na_end
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     na_copy
na_end:
        mov     byte ptr [edi],0
        cmp     edi,OFFSET argbuf
        je      na_done
        mov     dword ptr [progptr],OFFSET argbuf
na_done:
        ret

;=====================================================================
; ok / fail - the verdict on the line being printed.  Everything
;             preserved; fail counts.
;---------------------------------------------------------------------
ok:
        push    edx
        mov     edx,OFFSET msg_ok
        call    puts
        pop     edx
        ret
fail:
        push    edx
        inc     dword ptr [failures]
        mov     edx,OFFSET msg_fail
        call    puts
        pop     edx
        ret

;--- putz - EDX -> an ASCIIZ string -----------------------------------
putz:
        push    eax
        push    edx
        push    esi
        mov     esi,edx
pz_loop:
        mov     al,[esi]
        or      al,al
        jz      pz_done
        mov     dl,al
        mov     ah,02h
        int     21h
        inc     esi
        jmp     short pz_loop
pz_done:
        pop     esi
        pop     edx
        pop     eax
        ret

;--- puts - EDX -> a '$' string ----------------------------------------
puts:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

;--- put8 / put4 / put2 - EAX in hex: eight, four or two digits --------
put8:
        push    ecx
        mov     ecx,8
        push    eax
        jmp     short put_n
put4:
        push    ecx
        mov     ecx,4
        push    eax
        rol     eax,16
        jmp     short put_n
put2:
        push    ecx
        mov     ecx,2
        push    eax
        rol     eax,24
put_n:
        push    edx
        mov     edx,eax
pn_loop:
        rol     edx,4
        mov     al,dl
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     pn_ch
        add     al,7
pn_ch:
        push    ecx
        push    edx
        mov     dl,al
        mov     ah,02h
        int     21h
        pop     edx
        pop     ecx
        loop    pn_loop
        pop     edx
        pop     eax
        pop     ecx
        ret

;=====================================================================
msg_hi      db  'NDRVTEST - the 32-bit drivers DEVICE= loaded, asked through 21h/F1h',13,10,'$'
msg_count   db  '  NDF_COUNT: drivers loaded, of ND_MAX ................ $'
msg_of      db  ' of $'
msg_row     db  '  NDF_INFO: row 0 is NDTEST, above the megabyte ........ $'
msg_norow   db  '  NDF_INFO: a row there is not, refused ................ $'
msg_report  db  '  NDF_CALL: the report, its length ..................... $'
msg_reloc   db  '  relocated: the table as linked, and as found ......... $'
msg_hdr     db  '  the header base and entry, after the relocation ...... $'
msg_init    db  '  INIT was given the services and its row .............. $'
msg_line    db  '  the DEVICE= line: $'
msg_block   db  '  NDS_ALLOC: a block, and its pattern read back ........ $'
msg_clock   db  '  the clock hooked, and ticking ........................ $'
msg_irq     db  '  the line hooked, quiet, and open at the 8259 ......... $'
msg_badfn   db  '  NDF_CALL: an unknown function, refused ............... $'
msg_short   db  '  NDF_CALL: a short buffer, refused .................... $'
msg_parking db  'parking the kernel, to count PARK: $'
msg_back    db  'back: PARK called with 0 and with 1 .................... $'
msg_and     db  ' and $'
msg_refused db  'the kernel refused to park, error $'
msg_failed  db  '  checks failed here: $'
msg_allok   db  'all checks passed.',13,10,'$'
msg_notok   db  'SOME CHECKS FAILED.',13,10,'$'
msg_done    db  'NDRVTEST done',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_fail    db  ' FAIL',13,10,'$'
msg_crlf    db  13,10,'$'

name_ndtest db  'NDTEST  '
progptr     dd  OFFSET progname
progname    db  '\TESTS\HELLO.COM',0
argbuf      db  80 dup(0)
exitcode    dd  0
failures    dd  0
count       dd  0
ticks1      dd  0
secs        db  0
            db  0, 0, 0
row         dd  ND_LEN / 4 dup(0)
report      dd  NDTR_LEN / 4 dup(0)

_NT     ENDS
        END
