;=====================================================================
; LOLTEST.ASM - LOLTEST.COM, the calls that hand back an address
;
; An ordinary 16-bit DOS program, so every call it makes goes through
; the VDM bridge as well as through the kernel - which is the whole
; point, because everything it tests lived in the bridge and was
; invisible from the 32-bit side.
;
; THE THEME IS POINTERS.  52h, 1Bh, 1Ch and 63h all answer an ADDRESS,
; and an address that means something to this kernel - flat, 32-bit,
; above 1MB - means nothing whatever to a real-mode program.  52h was
; where that was found; the others are the same shape and are checked
; here so they cannot become the same bug.  38h is the reverse case, a
; pointer going IN, and 67h and 5Ch answer no pointer at all and are
; here because a program refused either of them stops dead.
;
;   21h/52h    the list of lists.  A guest used to get the bottom half
;              of a flat kernel pointer with ES left at whatever it
;              happened to be, so the first-MCB word it read was a
;              stranger's memory and the arena walk that followed
;              never terminated.  That is a hang with no exception
;              behind it - Quarterdeck's Manifest at its splash
;              screen - and a hang is exactly what a test cannot
;              report, so THE WALK HERE IS BOUNDED.  It counts blocks
;              and gives up rather than hanging the way the bug did.
;
;   PUSHF/POPF the monitor kept IOPL and NT out of the flags a guest
;              RUNS under, which is right, and out of the flags a
;              guest READS BACK, which was not: every 286-versus-386
;              test ever written sets bits 12 to 14 with POPF and
;              looks for them with PUSHF.  A 486 answered "286", and
;              Windows for Workgroups 3.11 said so out loud.
;
; Every check prints "ok" or "FAILED", so the whole run is one screen
; that either has no capitals in it or does.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_LT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_LT, DS:_LT, ES:_LT, SS:_LT
        ORG     0

MCB_MAX EQU     512                     ; blocks before we call it runaway
DEV_MAX EQU     64                      ; and device headers likewise

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; 52h - is the pointer even in the machine?
;
; The failure being tested for does not return an error, it returns a
; plausible-looking pointer into nowhere, so "did it set carry" proves
; nothing.  What can be checked is that ES lands in memory a real-mode
; program can address AND that the structure behind it holds up - so
; this first test is only the cheap half, and the arena walk below is
; the one that would have caught the bug.
;=====================================================================
        mov     dx,OFFSET t_call
        call    puts
        mov     ah,52h
        int     21h
        jc      lol_no
        mov     [lol_seg],es
        mov     [lol_off],bx

        mov     ax,es
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,bx
        call    put4

        ; Below A000h is the real-mode part of the machine.  A flat
        ; kernel pointer truncated to sixteen bits could land here by
        ; chance, which is why this is the weak test and not the proof.
        mov     ax,[lol_seg]
        cmp     ax,0A000h
        jae     lol_no
        cmp     bx,2                    ; [BX-2] has to be inside it
        jb      lol_no
        call    good
        jmp     short lol_mcb
lol_no:
        call    bad
        jmp     cpu_test                ; nothing below this can run

;=====================================================================
; [ES:BX-2] - the first memory control block, and the walk from it.
;
; THE ONE FIELD IN THE STRUCTURE THAT IS A CONTRACT.  It has meant the
; first MCB in every DOS since 2.0, and a memory-mapping tool making
; this call wants it and nothing else.
;=====================================================================
lol_mcb:
        mov     dx,OFFSET t_mcb
        call    puts
        mov     es,[lol_seg]
        mov     bx,[lol_off]
        mov     ax,es:[bx-2]
        mov     [mcb_first],ax
        call    put4
        or      ax,ax
        jz      mcb_bad                 ; segment 0 is the vector table
        cmp     ax,0A000h
        jae     mcb_bad
        call    good
        jmp     short mcb_walk
mcb_bad:
        call    bad
        jmp     lastdrv

;=====================================================================
; The arena, block by block, BOUNDED.
;
; A good chain is 'M' blocks ending in one 'Z'.  Three ways it can be
; wrong and all three are checked, because the bug produced the third:
; a signature that is neither letter, a segment that wraps past FFFFh,
; or a chain that simply goes on longer than the machine has room for.
; The old code met none of them - it met random bytes that happened to
; say 'M' often enough to keep walking forever.
;=====================================================================
mcb_walk:
        mov     dx,OFFSET t_walk
        call    puts
        mov     ax,[mcb_first]
        xor     cx,cx
w_next:
        mov     es,ax
        cmp     byte ptr es:[0],'Z'
        je      w_last
        cmp     byte ptr es:[0],'M'
        jne     w_broken
        inc     cx
        cmp     cx,MCB_MAX
        jae     w_runaway
        mov     bx,es:[3]               ; paragraphs, not counting the header
        inc     bx                      ; which is one more
        add     ax,bx
        jc      w_broken                ; walked off the top of memory
        jmp     short w_next

w_last:
        inc     cx
        mov     ax,cx
        call    put4
        mov     dx,OFFSET msg_blocks
        call    puts
        call    good
        jmp     short lastdrv
w_broken:
        mov     dx,OFFSET msg_broken
        call    puts
        call    bad
        jmp     short lastdrv
w_runaway:
        mov     dx,OFFSET msg_runaway
        call    puts
        call    bad

;=====================================================================
; The two drive counts, which are the fields most likely to be stale
; rather than wrong: LASTDRIVE is fixed at build time, but the block
; device count is written per call precisely because a volume can be
; mounted after boot.
;=====================================================================
lastdrv:
        mov     dx,OFFSET t_drives
        call    puts
        mov     es,[lol_seg]
        mov     bx,[lol_off]
        mov     al,es:[bx+20h]          ; block devices
        call    put2
        mov     dl,'/'
        call    putc
        mov     al,es:[bx+21h]          ; LASTDRIVE
        call    put2
        mov     al,es:[bx+21h]
        or      al,al
        jz      drv_bad
        cmp     al,26
        ja      drv_bad
        mov     ah,es:[bx+20h]
        or      ah,ah
        jz      drv_bad                 ; booted, so there is at least one
        cmp     ah,al
        ja      drv_bad                 ; more drives than letters for them
        call    good
        jmp     short devchain
drv_bad:
        call    bad

;=====================================================================
; The device chain, walked from its HEAD and BOUNDED.
;
; The head is the NUL header inside the list of lists at +22h, not the
; CON pointer - that is where DOS anchored the chain and where every
; device walker starts.  Each name is printed as it is passed, so the
; output is the driver list a tool like MSD would show, and a wrong
; NEXT pointer shows up as garbage rather than as a silent miscount.
;
; An unterminated chain hangs a walker exactly the way an unterminated
; arena hangs a memory mapper, so the count is capped and reported.
;=====================================================================
devchain:
        mov     dx,OFFSET t_dev
        call    puts
        mov     ax,[lol_seg]            ; the head is IN the structure,
        mov     dx,[lol_off]            ; at BX+22h
        add     dx,22h
        mov     [d_seg],ax
        mov     [d_off],dx
        xor     cx,cx

d_next:
        mov     es,[d_seg]
        mov     bx,[d_off]
        inc     cx
        cmp     cx,DEV_MAX
        jae     d_runaway
        push    cx

        mov     cx,8                    ; the name, as the walker sees it
d_name:
        mov     dl,es:[bx+0Ah]
        call    putc
        inc     bx
        loop    d_name
        mov     dl,' '
        call    putc
        pop     cx

        mov     bx,[d_off]
        mov     ax,es:[bx]              ; the next header, offset first
        mov     dx,es:[bx+2]
        cmp     ax,0FFFFh
        je      d_end
        mov     [d_off],ax
        mov     [d_seg],dx
        jmp     short d_next

d_end:
        mov     ax,cx
        call    put4
        mov     dx,OFFSET msg_devs
        call    puts
        cmp     cx,4                    ; NUL, CON, AUX, PRN
        jne     d_bad
        call    good
        jmp     short d_done
d_bad:
        call    bad
        jmp     short d_done
d_runaway:
        mov     dx,OFFSET msg_runaway
        call    puts
        call    bad

; ALL THREE PATHS ARRIVE HERE.  The two ordinary ones used to jump
; to cpu_test instead, which is past every check below - so 1Bh, 1Ch,
; 38h, 67h, 5Ch, 63h, 6Ch and 65h ran ONLY when the device chain
; failed to terminate, which it never does.  Eight checks that
; printed nothing and were counted as neither passing nor failing.
d_done:

;=====================================================================
; 1Bh - allocation information, and the SECOND pointer this bridge
; hands back.  Same hazard as 52h and the same test: DS:BX has to land
; somewhere a real-mode program can read, and the byte there has to be
; a media descriptor rather than whatever was lying around.
;
; DS is moved by this call, which is why it is pushed and popped
; around it - the program's own data segment is the thing at risk.
;=====================================================================
        mov     dx,OFFSET t_alloc
        call    puts
        push    ds
        mov     ah,1Bh
        int     21h
        mov     cs:[a_spc],al
        mov     cs:[a_bps],cx
        mov     cs:[a_clus],dx
        mov     dl,byte ptr [bx]        ; the media byte itself
        mov     cs:[a_media],dl
        mov     ax,ds
        mov     cs:[a_seg],ax
        mov     cs:[a_off],bx
        pop     ds

        mov     ax,[a_seg]
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,[a_off]
        call    put4
        mov     dx,OFFSET msg_media
        call    puts
        mov     al,[a_media]
        call    put2

        ; F0h and F8h-FFh are the media bytes DOS ever wrote.  Anything
        ; else means the pointer went somewhere that is not a BPB.
        mov     ax,[a_seg]              ; addressable at all, first
        or      ax,ax
        jz      alloc_bad               ; segment 0 is the vector table
        cmp     ax,0A000h
        jae     alloc_bad
        mov     al,[a_media]
        cmp     al,0F0h
        je      alloc_ok
        cmp     al,0F8h
        jb      alloc_bad
alloc_ok:
        call    good
        jmp     short alloc_geom
alloc_bad:
        call    bad

;--- and the numbers that came with it --------------------------------
alloc_geom:
        mov     dx,OFFSET t_geom
        call    puts
        mov     al,[a_spc]
        call    put2
        mov     dx,OFFSET msg_x
        call    puts
        mov     ax,[a_bps]
        call    put4
        mov     dx,OFFSET msg_over
        call    puts
        mov     ax,[a_clus]
        call    put4
        mov     al,[a_spc]              ; a cluster is a power of two
        or      al,al                   ; and never zero
        jz      geom_bad
        cmp     word ptr [a_bps],512
        jne     geom_bad
        cmp     word ptr [a_clus],0
        je      geom_bad
        call    good
        jmp     short ctry
geom_bad:
        call    bad

;=====================================================================
; 38h - country information.  The buffer is the CALLER's, so this one
; tests the bridge in the other direction: DS:DX going in rather than
; coming back.  A wrong translation writes 34 bytes somewhere else.
;=====================================================================
ctry:
        mov     dx,OFFSET t_ctry
        call    puts
        mov     word ptr [cbuf],0AA55h  ; poison, so "untouched" shows
        mov     ah,38h
        xor     al,al
        mov     dx,OFFSET cbuf
        int     21h
        jc      ctry_bad
        mov     dl,[cbuf+9]             ; the decimal separator
        call    putc
        mov     dl,[cbuf+0Bh]           ; and the date separator
        call    putc
        cmp     word ptr [cbuf],0AA55h
        je      ctry_bad                ; nothing was written
        cmp     byte ptr [cbuf+9],'.'
        jne     ctry_bad
        call    good
        jmp     short handles
ctry_bad:
        call    bad

;=====================================================================
; 67h and 5Ch - the two that succeed on purpose.  What is being tested
; is that they do NOT return "invalid function", because a program
; refused either one stops for a reason that does not exist here.
;=====================================================================
handles:
        mov     dx,OFFSET t_67
        call    puts
        mov     ah,67h
        mov     bx,60
        int     21h
        jc      h67_bad
        call    good
        jmp     short locktest
h67_bad:
        call    bad
locktest:
        mov     dx,OFFSET t_5c
        call    puts
        mov     ah,3Ch                  ; a file to lock
        xor     cx,cx
        mov     dx,OFFSET n_temp
        int     21h
        jc      locktest_bad
        mov     [h_tmp],ax
        mov     bx,ax
        mov     ax,5C00h                ; lock 16 bytes at 0
        xor     cx,cx
        xor     dx,dx
        xor     si,si
        mov     di,16
        int     21h
        jc      locktest_close
        mov     ax,5C01h                ; and unlock them again
        mov     bx,[h_tmp]
        xor     cx,cx
        xor     dx,dx
        xor     si,si
        mov     di,16
        int     21h
        jc      locktest_close
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_temp
        mov     ah,41h
        int     21h
        call    good
        jmp     short dbcs
locktest_close:
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_temp
        mov     ah,41h
        int     21h
locktest_bad:
        call    bad

;=====================================================================
; 63h - the DBCS table, and the THIRD pointer.  This one answers in
; DS:SI rather than DS:BX, which is the only thing separating it from
; 1Bh as far as the bridge is concerned - and getting that wrong puts
; the segment on one register and the offset on another.
;=====================================================================
dbcs:
        mov     dx,OFFSET t_dbcs
        call    puts
        push    ds
        xor     al,al
        mov     ah,63h
        int     21h
        jc      dbcs_pop
        mov     ax,ds
        mov     cs:[a_seg],ax
        mov     cs:[a_off],si
        mov     ax,word ptr [si]        ; the terminator IS the table
        mov     cs:[a_bps],ax
        pop     ds
        mov     ax,[a_seg]
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,[a_off]
        call    put4
        mov     ax,[a_seg]
        or      ax,ax
        jz      dbcs_bad
        cmp     ax,0A000h
        jae     dbcs_bad
        cmp     word ptr [a_bps],0      ; no lead-byte ranges
        jne     dbcs_bad
        call    good
        jmp     short extopen
dbcs_pop:
        pop     ds
dbcs_bad:
        call    bad

;=====================================================================
; 6Ch - extended open, and the one thing 3Dh and 3Ch cannot say
; between them: which of create and open actually happened.  Three
; calls on the same name, and CX has to give three different answers.
;=====================================================================
extopen:
        mov     dx,OFFSET t_6c
        call    puts
        mov     dx,OFFSET n_temp        ; make sure it is not there
        mov     ah,41h
        int     21h

        mov     ax,6C00h                ; create if missing
        mov     bx,0002h                ; read/write
        xor     cx,cx
        mov     dx,0010h
        mov     si,OFFSET n_temp
        int     21h
        jc      x6c_bad
        mov     [h_tmp],ax
        mov     al,cl
        call    put2
        cmp     cx,3                    ; 3 = created
        jne     x6c_close
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h

        mov     ax,6C00h                ; now it exists: open it
        mov     bx,0002h
        xor     cx,cx
        mov     dx,0011h                ; open-if-exists + create-if-not
        mov     si,OFFSET n_temp
        int     21h
        jc      x6c_bad
        mov     [h_tmp],ax
        mov     al,cl
        call    put2
        cmp     cx,1                    ; 1 = opened in place
        jne     x6c_close
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h

        mov     ax,6C00h                ; and truncate it
        mov     bx,0002h
        xor     cx,cx
        mov     dx,0012h                ; truncate-if-exists
        mov     si,OFFSET n_temp
        int     21h
        jc      x6c_bad
        mov     [h_tmp],ax
        mov     al,cl
        call    put2
        cmp     cx,2                    ; 2 = truncated
        jne     x6c_close
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET n_temp
        mov     ah,41h
        int     21h
        call    good
        jmp     short ctryext
x6c_close:
        mov     bx,[h_tmp]
        mov     ah,3Eh
        int     21h
x6c_bad:
        mov     dx,OFFSET n_temp
        mov     ah,41h
        int     21h
        call    bad

;=====================================================================
; 65h - the subfunctions that DO something rather than hand back a
; table.  20h and 23h take a character and no pointer at all, 22h
; takes a string in DS:DX, and 01h a buffer in ES:DI - three different
; answers to "which register is an address", from one function number.
; Getting that wrong is silent: a character becomes an address.
;=====================================================================
ctryext:
        mov     dx,OFFSET t_65cap
        call    puts
        mov     ax,6520h                ; capitalise one character
        mov     dl,'a'
        int     21h
        jc      x65_capbad
        mov     bl,dl                   ; BEFORE printing it: put1 puts each
                                        ; digit in DL to call 02h with, so the
                                        ; answer is gone by the time put2
                                        ; returns and the check below read the
                                        ; last character printed instead
        mov     al,dl
        call    put2
        cmp     bl,'A'
        jne     x65_capbad

        mov     ax,6522h                ; and a whole ASCIZ string
        mov     dx,OFFSET capbuf
        int     21h
        jc      x65_capbad
        cmp     byte ptr [capbuf],'P'   ; checked BEFORE printing, because
        jne     x65_capbad              ; printing has to overwrite the
        cmp     byte ptr [capbuf+5],'S' ; terminator to do it at all
        jne     x65_capbad
        mov     byte ptr [capbuf+6],'$' ; 22h stops at a zero and 09h stops
        mov     dx,OFFSET capbuf        ; at a dollar - six characters with
        call    puts                    ; two different ends
        call    good
        jmp     short x65_yn
x65_capbad:
        call    bad

x65_yn:
        mov     dx,OFFSET t_65yn
        call    puts
        mov     ax,6523h
        mov     dl,'y'
        int     21h
        jc      x65_ynbad
        cmp     ax,1                    ; yes
        jne     x65_ynbad
        mov     ax,6523h
        mov     dl,'n'
        int     21h
        cmp     ax,0                    ; no
        jne     x65_ynbad
        mov     ax,6523h
        mov     dl,'q'
        int     21h
        cmp     ax,2                    ; neither
        jne     x65_ynbad
        call    good
        jmp     short x65_info
x65_ynbad:
        call    bad

x65_info:
        mov     dx,OFFSET t_65info
        call    puts
        mov     word ptr [cbuf+3],0AA55h
        mov     ax,6501h
        mov     bx,0FFFFh               ; the current country
        mov     cx,41
        mov     di,OFFSET cbuf
        push    es
        push    ds
        pop     es                      ; ES:DI, and a .COM has DS = ES
        int     21h
        pop     es
        jc      x65_infobad
        mov     ax,word ptr [cbuf+3]
        call    put4
        mov     dl,'/'
        call    putc
        mov     ax,word ptr [cbuf+5]
        call    put4
        cmp     byte ptr [cbuf],1       ; the info id
        jne     x65_infobad
        cmp     word ptr [cbuf+3],1     ; country - the United States
        jne     x65_infobad
        cmp     word ptr [cbuf+5],437   ; and the codepage
        jne     x65_infobad
        call    good
        jmp     short cpu_test
x65_infobad:
        call    bad

;=====================================================================
; The CPU, detected the way DOS software detects it.
;
; This is verbatim the sequence WIN.COM uses, and the reason it is
; worth having in a test at all is that it is not a question about the
; CPU - the CPU is whatever it is - it is a question about whether the
; monitor lets a guest read back a flag it just wrote.
;
;   8086:  bits 12-15 of FLAGS are always SET and cannot be cleared
;   286:   they are always CLEAR in real mode and cannot be set
;   386+:  12, 13 and 14 are writable, so they stick
;=====================================================================
cpu_test:
        mov     dx,OFFSET t_cpu
        call    puts

        pushf
        pop     ax
        mov     cx,ax                   ; keep the real ones
        and     ax,0FFFh                ; try to CLEAR 12-15
        push    ax
        popf
        pushf
        pop     ax
        and     ax,0F000h
        cmp     ax,0F000h
        je      cpu_8086                ; they came back set: 8086

        or      cx,0F000h               ; try to SET them
        push    cx
        popf
        pushf
        pop     ax
        and     ax,0F000h
        push    ax
        call    put4                    ; and show what actually stuck
        pop     ax
        or      ax,ax
        jz      cpu_286                 ; nothing stuck: 286

        mov     dx,OFFSET msg_386
        call    puts
        call    good
        jmp     short nt_test
cpu_8086:
        mov     dx,OFFSET msg_8086
        call    puts
        call    bad
        jmp     short nt_test
cpu_286:
        mov     dx,OFFSET msg_286
        call    puts
        call    bad

;=====================================================================
; NT on its own, because it is the bit with a reason behind it.
;
; NT must never be set in the flags the guest RUNS under - an IRET
; with NT set is a task switch and the guest would take the monitor's
; TSS with it - so the right behaviour is the split one: it reads back
; because the guest wrote it, and it is not really there.  A test can
; only see the first half; the second half is that this program still
; has an IRET-driven interrupt or two ahead of it and does not die.
;=====================================================================
nt_test:
        mov     dx,OFFSET t_nt
        call    puts
        pushf
        pop     cx
        or      cx,4000h                ; NT
        push    cx
        popf
        pushf
        pop     ax
        test    ax,4000h
        jz      nt_no
        call    good
        jmp     short nt_clear
nt_no:
        call    bad
nt_clear:
        pushf                           ; and put it back, so nothing
        pop     ax                      ; downstream inherits it
        and     ax,NOT 4000h
        push    ax
        popf

;=====================================================================
; And that the DOS box is still alive after all of that.
;=====================================================================
        mov     dx,OFFSET t_alive
        call    puts
        mov     ah,30h
        int     21h
        call    good

        mov     dx,OFFSET msg_bye
        call    puts
        cmp     byte ptr [fails],0
        je      done_ok
        mov     dx,OFFSET msg_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET msg_crlf
        call    puts
        mov     ax,4C01h
        int     21h
done_ok:
        mov     ax,4C00h
        int     21h

;=====================================================================
; helpers
;=====================================================================
good:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        inc     byte ptr [fails]
        pop     dx
        ret

puts:
        push    ax
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
put4:                                   ; AX in hex
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:                                   ; AL in hex
        push    ax
        shr     al,4
        call    put1
        pop     ax
        push    ax
        and     al,0Fh
        call    put1
        pop     ax
        ret
put1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
; data
;=====================================================================
msg_hi      db  13,10,'LOLTEST - the list of lists, and the flags a guest reads back',13,10,'$'
msg_bye     db  'LOLTEST done.',13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_fails   db  'checks failed: $'
msg_blocks  db  ' blocks, ending in Z$'
msg_devs    db  ' headers, terminated$'
msg_broken  db  ' chain broken$'
msg_runaway db  ' NEVER TERMINATED$'
msg_nodev   db  'no device chain$'
msg_8086    db  ' = 8086$'
msg_286     db  ' = 286$'
msg_386     db  ' = 386 or better$'

t_call      db  '  52h list of lists at $'
t_mcb       db  '  first MCB at [BX-2] is $'
t_walk      db  '  the arena walks: $'
t_drives    db  '  block devices / LASTDRIVE $'
t_dev       db  '  device chain: $'
t_alloc     db  '  1Bh media byte at $'
t_geom      db  '  1Bh geometry $'
t_ctry      db  '  38h country separators $'
t_67        db  '  67h set handle count is not refused$'
t_5c        db  '  5Ch lock and unlock a region$'
t_dbcs      db  '  63h DBCS table at $'
t_6c        db  '  6Ch create/open/truncate say $'
t_65cap     db  '  65h capitalises $'
t_65yn      db  '  65h knows yes from no$'
t_65info    db  '  65h country/codepage $'
msg_media   db  ', which is $'
msg_x       db  ' x $'
msg_over    db  ' bytes, $'
t_cpu       db  '  PUSHF/POPF says the CPU is $'
t_nt        db  '  NT reads back after POPF sets it$'
t_alive     db  '  and the box survived it$'

fails       db  0
lol_seg     dw  0
lol_off     dw  0
mcb_first   dw  0
d_seg       dw  0
d_off       dw  0

; The 1Bh/63h answers are stashed through CS because DS is what those
; calls move - reading them back through DS would read the structure
; the call just pointed at, not this program's variables.
a_seg       dw  0
a_off       dw  0
a_bps       dw  0
a_clus      dw  0
a_spc       db  0
a_media     db  0
h_tmp       dw  0
n_temp      db  'LOLTEST.$$$',0
capbuf      db  'pm-dos',0              ; 22h capitalises this in place
            db  0,0
cbuf        db  48 dup(0)               ; 41 for 65h/01h, rounded up

_LT     ENDS
        END
