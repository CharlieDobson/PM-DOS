;=====================================================================
; DPMITEST.ASM - DPMITEST.COM, a 16-bit DPMI client.
;
; It starts as an ordinary .COM in a DOS box, finds the host the way
; the specification says to, asks to be put into protected mode, and
; then does from PROTECTED mode every one of the things a real client
; does: descriptors, memory, DOS calls, a simulated real-mode
; interrupt, and a BIOS call that has to be reflected.
;
; A 16-bit client is written almost exactly like a .COM, which is the
; nicest thing about the interface: the selectors it is given have
; bases of CS*16 and DS*16, so every OFFSET in the file still means
; what it meant in real mode and the same instruction encodings run.
; What changes is that DS is a SELECTOR, and handing one to DOS means
; the host has to translate it - which is the half being tested here.
;
; Every check prints OK or FAIL, and the ones after the switch are
; printed BY protected-mode code through INT 21h, so their appearing
; at all is itself the first result.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386

; Up here and not with the other equates at the foot of the file:
; wasm cannot size a forward-referenced EQU used as an immediate, and
; this one is compared against a word a hundred lines before the
; bottom of the file.
CB_SEED         EQU     4321h           ; what real mode hands the callback

_DP     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DP, DS:_DP, ES:_DP, SS:_DP
        ORG     0

start:
        ; GIVE BACK THE MEMORY WE DO NOT NEED, and do it first.
        ;
        ; A .COM is handed every free paragraph of conventional memory
        ; there is, so until it shrinks itself there is nothing left
        ; for anyone else - and phase 5 below asks the host for a
        ; buffer below a megabyte, which is exactly "anyone else".
        ; Every real DPMI client's startup does this; leaving it out
        ; made the host answer 8012h, which is the truthful answer to
        ; a request for memory that has all been given away already.
        mov     ah,4Ah
        mov     bx,100h                         ; 4K is more than enough
        int     21h

        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; Real mode: is there a host, and what is it?
;=====================================================================
        mov     dx,OFFSET msg_p1
        call    puts

        mov     ax,1686h                        ; "am I in protected mode?"
        int     2Fh
        or      ax,ax
        jz      p1_odd                           ; zero would mean yes, here
        mov     dx,OFFSET msg_notpm
        call    ok
        jmp     short p1_host
p1_odd:
        mov     dx,OFFSET msg_saidpm
        call    fail

p1_host:
        mov     ax,1687h
        int     2Fh
        or      ax,ax
        jz      p1_found
        mov     dx,OFFSET msg_nohost
        call    fail
        jmp     done

p1_found:
        mov     [entry_off],di
        mov     [entry_seg],es
        mov     [host_flags],bx
        mov     [host_ver],dx
        mov     [host_para],si

        mov     dx,OFFSET msg_found
        call    ok
        mov     dx,OFFSET msg_ver
        call    puts
        mov     al,byte ptr [host_ver+1]        ; out of the SAVED copy: the
        call    dec8                            ; line above just used DX
        mov     dl,'.'
        call    putc
        mov     al,byte ptr [host_ver]
        call    dec8
        call    crlf

        mov     dx,OFFSET msg_p32
        call    puts
        test    byte ptr [host_flags],1
        jz      p1_no32
        mov     dx,OFFSET msg_yes
        call    puts
        jmp     short p1_priv
p1_no32:
        mov     dx,OFFSET msg_no
        call    puts

;--- the host's private data area, if it wants one -------------------
p1_priv:
        mov     bx,[host_para]
        or      bx,bx
        jz      p1_switch
        mov     ah,48h
        int     21h
        jc      p1_nomem
        mov     es,ax
        jmp     short p1_switch
p1_nomem:
        mov     dx,OFFSET msg_noparas
        call    fail
        jmp     done

;=====================================================================
; The switch itself.  Everything after this instruction runs in
; protected mode, at ring 3, out of a descriptor table the host built.
;=====================================================================
p1_switch:
        mov     dx,OFFSET msg_p2
        call    puts

        mov     ax,0                            ; bit 0 clear: a 16-bit client
        call    dword ptr [entry_off]
        jnc     in_pm

        mov     dx,OFFSET msg_swfail
        call    fail
        jmp     done

;=====================================================================
; PROTECTED MODE.  The first thing printed from here is the answer to
; the only question that matters: can a client still talk to DOS?
;=====================================================================
in_pm:
        mov     [pm_ds],ds                      ; the selectors we were given
        mov     [pm_es],es
        mov     [pm_cs],cs

        mov     dx,OFFSET msg_inpm
        call    ok

;--- and are we REALLY in protected mode? ----------------------------
        smsw    ax
        test    al,1
        jz      pm_notpe
        mov     dx,OFFSET msg_pebit
        call    ok
        jmp     short pm_ask
pm_notpe:
        mov     dx,OFFSET msg_nope
        call    fail

pm_ask:
        mov     ax,1686h                        ; the same question, answered
        int     2Fh                             ; the other way round now
        or      ax,ax
        jnz     pm_askbad
        mov     dx,OFFSET msg_1686
        call    ok
        jmp     short pm_ver
pm_askbad:
        mov     dx,OFFSET msg_1686bad
        call    fail

;=====================================================================
; INT 31h - the interface
;=====================================================================
pm_ver:
        mov     dx,OFFSET msg_p3
        call    puts

        mov     ax,0400h
        int     31h
        jc      pm_verbad
        cmp     ah,0
        jne     pm_verbad
        cmp     al,90
        jne     pm_verbad
        mov     dx,OFFSET msg_v09
        call    ok
        jmp     short pm_inc
pm_verbad:
        mov     dx,OFFSET msg_v09bad
        call    fail

pm_inc:
        mov     ax,0003h
        int     31h
        cmp     ax,8
        jne     pm_incbad
        mov     dx,OFFSET msg_inc
        call    ok
        jmp     short pm_desc
pm_incbad:
        mov     dx,OFFSET msg_incbad
        call    fail

;--- a descriptor of our own, over our own data ----------------------
; Base and limit are set to cover this program's data segment, so a
; write through the new selector has to be visible through DS.  That
; is the check: not that the call succeeded, but that the descriptor
; describes what it was told to.
pm_desc:
        mov     ax,0000h
        mov     cx,1
        int     31h
        jc      pm_descbad
        mov     [mysel],ax

        mov     ax,0006h                        ; what base does DS have?
        mov     bx,[pm_ds]
        int     31h
        jc      pm_descbad
        mov     [ds_base_hi],cx
        mov     [ds_base_lo],dx

        mov     ax,0007h                        ; give ours the same one
        mov     bx,[mysel]
        mov     cx,[ds_base_hi]
        mov     dx,[ds_base_lo]
        int     31h
        jc      pm_descbad

        mov     ax,0008h
        mov     bx,[mysel]
        mov     cx,0
        mov     dx,0FFFFh
        int     31h
        jc      pm_descbad

        mov     ax,0009h                        ; a writable data segment
        mov     bx,[mysel]
        mov     cx,00F2h
        int     31h
        jc      pm_descbad

        mov     word ptr [probe],0
        push    es
        mov     es,[mysel]
        mov     di,OFFSET probe
        mov     word ptr es:[di],1234h
        pop     es
        cmp     word ptr [probe],1234h
        jne     pm_descbad
        mov     dx,OFFSET msg_desc
        call    ok
        jmp     short pm_mem
pm_descbad:
        mov     dx,OFFSET msg_descbad
        call    fail

;--- extended memory, and a descriptor over it -----------------------
pm_mem:
        mov     ax,0501h
        mov     bx,0
        mov     cx,1000h                        ; 4K of it
        int     31h
        jc      pm_membad
        mov     [mem_hi],bx
        mov     [mem_lo],cx
        mov     [mem_hand_hi],si
        mov     [mem_hand_lo],di

        mov     ax,0000h
        mov     cx,1
        int     31h
        jc      pm_membad
        mov     [memsel],ax

        mov     ax,0007h
        mov     bx,[memsel]
        mov     cx,[mem_hi]
        mov     dx,[mem_lo]
        int     31h
        jc      pm_membad
        mov     ax,0008h
        mov     bx,[memsel]
        mov     cx,0
        mov     dx,0FFFh
        int     31h
        jc      pm_membad
        mov     ax,0009h
        mov     bx,[memsel]
        mov     cx,00F2h
        int     31h
        jc      pm_membad

        push    es
        mov     es,[memsel]
        xor     di,di
        mov     word ptr es:[di],0AA55h
        mov     word ptr es:[di+0FFEh],5AA5h
        cmp     word ptr es:[di],0AA55h
        jne     pm_membad2
        cmp     word ptr es:[di+0FFEh],5AA5h
        jne     pm_membad2
        pop     es
        mov     dx,OFFSET msg_mem
        call    ok
        jmp     short pm_free
pm_membad2:
        pop     es
pm_membad:
        mov     dx,OFFSET msg_membad
        call    fail

pm_free:
        mov     ax,0502h
        mov     si,[mem_hand_hi]
        mov     di,[mem_hand_lo]
        int     31h
        jc      pm_freebad
        mov     dx,OFFSET msg_memfree
        call    ok
        jmp     short pm_dos
pm_freebad:
        mov     dx,OFFSET msg_memfreebad
        call    fail

;=====================================================================
; DOS, from protected mode.  Every line printed above already went
; through INT 21h AH=09h with DS holding a SELECTOR, so the string
; pointer has been translated a dozen times by now - this checks a
; call whose ANSWER can be looked at rather than read off the screen.
;=====================================================================
pm_dos:
        mov     dx,OFFSET msg_p4
        call    puts

        mov     ah,30h                          ; the DOS version
        int     21h
        cmp     al,5
        jb      pm_dosbad
        mov     dx,OFFSET msg_dosver
        call    ok
        jmp     short pm_write
pm_dosbad:
        mov     dx,OFFSET msg_dosverbad
        call    fail

;--- a file, written and read back, entirely from protected mode -----
pm_write:
        mov     ah,3Ch
        mov     cx,0
        mov     dx,OFFSET fname
        int     21h
        jc      pm_filebad
        mov     bx,ax
        mov     [fhandle],bx
        mov     ah,40h
        mov     cx,FTEXT_LEN
        mov     dx,OFFSET ftext
        int     21h
        jc      pm_fileclose
        cmp     ax,FTEXT_LEN
        jne     pm_fileclose
        mov     bx,[fhandle]
        mov     ah,3Eh
        int     21h

        mov     ax,3D00h
        mov     dx,OFFSET fname
        int     21h
        jc      pm_filebad
        mov     bx,ax
        mov     [fhandle],bx
        mov     ah,3Fh
        mov     cx,FTEXT_LEN
        mov     dx,OFFSET fbuf
        int     21h
        pushf
        mov     bx,[fhandle]
        mov     ah,3Eh
        int     21h
        popf
        jc      pm_filebad

        mov     si,OFFSET ftext
        mov     di,OFFSET fbuf
        mov     cx,FTEXT_LEN
        push    es
        push    ds
        pop     es
        cld
        repe    cmpsb
        pop     es
        jne     pm_filebad
        mov     dx,OFFSET msg_file
        call    ok
        jmp     short pm_del
pm_fileclose:
        mov     bx,[fhandle]
        mov     ah,3Eh
        int     21h
pm_filebad:
        mov     dx,OFFSET msg_filebad
        call    fail
pm_del:
        mov     ah,41h
        mov     dx,OFFSET fname
        int     21h

;=====================================================================
; 0300h - a real-mode interrupt, run on our behalf
;
; The buffer has to be somewhere real-mode code can see, so it comes
; from 0100h - which hands back a segment for the real-mode side and a
; selector for us, over the same memory.  Writing through one and
; having the other work is the point of the call.
;=====================================================================
pm_sim:
        mov     dx,OFFSET msg_p5
        call    puts

        mov     ax,0100h
        mov     bx,4                            ; 64 bytes is plenty
        int     31h
        jc      pm_simbad
        mov     [dos_seg],ax
        mov     [dos_sel],dx

        ; the string, into the low buffer, through the selector
        push    es
        mov     es,[dos_sel]
        xor     di,di
        mov     si,OFFSET simtext
        mov     cx,SIMTEXT_LEN
        cld
        rep     movsb
        pop     es

        ; a register structure asking for INT 21h AH=09h
        mov     di,OFFSET rmcs
        mov     cx,RMCS_LEN
        xor     al,al
        push    es
        push    ds
        pop     es
        rep     stosb
        pop     es

        mov     word ptr [rmcs+1Ch],0900h       ; EAX = AH 09h
        mov     ax,[dos_seg]
        mov     word ptr [rmcs+24h],ax          ; DS = the low buffer
        mov     word ptr [rmcs+14h],0           ; EDX = offset 0

        push    es
        push    ds
        pop     es
        mov     ax,0300h
        mov     bx,0021h                        ; BL = the vector, BH = 0
        mov     cx,0                            ; no stack words to copy
        mov     di,OFFSET rmcs
        int     31h
        pop     es
        jc      pm_simbad
        mov     dx,OFFSET msg_sim
        call    ok
        jmp     short pm_dosfree
pm_simbad:
        push    ax
        mov     dx,OFFSET msg_simbad
        call    fail
        mov     dx,OFFSET msg_ax
        call    puts
        pop     ax
        call    hex16
        call    crlf

pm_dosfree:
        mov     ax,0101h
        mov     dx,[dos_sel]
        int     31h

;=====================================================================
; A BIOS call from protected mode, which nobody serves and everybody
; reflects.  INT 10h AH=0Fh answers with the video mode in AL and the
; column count in AH - two numbers that cannot both be zero on a
; machine with a screen, which is what makes it checkable.
;=====================================================================
pm_bios:
        mov     dx,OFFSET msg_p6
        call    puts

        mov     ah,0Fh
        int     10h
        or      ah,ah
        jz      pm_biosbad
        mov     [vid_cols],ah
        mov     dx,OFFSET msg_bios
        call    ok
        mov     dx,OFFSET msg_cols
        call    puts
        mov     al,[vid_cols]
        call    dec8
        call    crlf
        jmp     short pm_cb
pm_biosbad:
        mov     dx,OFFSET msg_biosbad
        call    fail

;=====================================================================
; 0303h - a real-mode callback, which is the interface running the
; other way about: real-mode code calling US.
;
; The way to make real-mode code call anything, from up here, is to
; put the callback address in the real-mode interrupt table (0201h)
; and then ask the host to run that interrupt (0300h).  What comes
; back proves three things at once - that the procedure was entered in
; protected mode, that what it wrote into the register structure
; reached real mode, and that it found the far return address on the
; real-mode stack the host handed it, because if it had not, nothing
; would have come back at all.
;=====================================================================
pm_cb:
        mov     dx,OFFSET msg_p7
        call    puts

        push    ds
        push    es
        push    ds
        pop     es                              ; ES:EDI - our structure
        mov     di,OFFSET cb_rmcs
        mov     ax,[pm_cs]                      ; DS:ESI - the procedure, and
        mov     ds,ax                           ; it has to be a CODE selector
        mov     si,OFFSET cb_proc
        mov     ax,0303h
        int     31h
        pop     es
        pop     ds
        jc      pm_cbbad
        mov     [cb_seg],cx
        mov     [cb_off],dx

        ; into the real-mode vector table, where real-mode code will
        ; find it exactly as it finds any other interrupt handler
        mov     ax,0201h
        mov     bl,0F5h
        mov     cx,[cb_seg]
        mov     dx,[cb_off]
        int     31h
        jc      pm_cbbad

        ; both structures clear, and a number for the procedure to
        ; work on
        push    es
        push    ds
        pop     es
        mov     di,OFFSET rmcs
        mov     cx,RMCS_LEN
        xor     al,al
        cld
        rep     stosb
        mov     di,OFFSET cb_rmcs
        mov     cx,RMCS_LEN
        rep     stosb
        pop     es
        mov     word ptr [rmcs+1Ch],CB_SEED     ; EAX, going in
        mov     byte ptr [cb_hits],0

        push    es
        push    ds
        pop     es
        mov     ax,0300h
        mov     bx,00F5h                        ; the vector we just hooked
        xor     cx,cx
        mov     di,OFFSET rmcs
        int     31h
        pop     es
        jc      pm_cbbad

        cmp     byte ptr [cb_hits],1            ; it ran, and ran once
        jne     pm_cbno
        cmp     word ptr [rmcs+10h],CB_SEED + 1 ; EBX, as it left it
        jne     pm_cbno
        mov     dx,OFFSET msg_cb
        call    ok
        jmp     short pm_cbfree
pm_cbno:
        mov     dx,OFFSET msg_cbno
        call    fail
        jmp     short pm_cbfree
pm_cbbad:
        push    ax
        mov     dx,OFFSET msg_cbbad
        call    fail
        mov     dx,OFFSET msg_ax
        call    puts
        pop     ax
        call    hex16
        call    crlf
        jmp     short pm_state

pm_cbfree:
        mov     ax,0304h
        mov     cx,[cb_seg]
        mov     dx,[cb_off]
        int     31h
        jc      pm_cbfbad
        mov     dx,OFFSET msg_cbfree
        call    ok
        jmp     short pm_state
pm_cbfbad:
        mov     dx,OFFSET msg_cbfreebad
        call    fail

;--- 0305h - and the state a mode switch costs, which here is none ---
pm_state:
        mov     ax,0305h
        int     31h
        jc      pm_statebad
        or      ax,ax                   ; zero: no buffer needed, because
        jnz     pm_statebad             ; nothing is lost either way
        mov     dx,OFFSET msg_state
        call    ok
        jmp     done
pm_statebad:
        mov     dx,OFFSET msg_statebad
        call    fail
        jmp     done

;---------------------------------------------------------------------
; cb_proc - the callback procedure.  PROTECTED MODE, ring 3, entered
; from real-mode code that has no idea that is what it did.
;
; DS:ESI is the real-mode stack and ES:EDI the register structure;
; neither is ours, and SS is the host's.  So the only way back to our
; own data is a selector that is still readable, and CS is it - a
; client's code selector is readable and has the same base as its data
; one, so the data selector can be fetched through CS and put in FS.
;
; What it then does is what every callback that hooks an interrupt
; does: take the return address off the real-mode stack, put it in the
; structure as CS:IP, and step SP past it.  The host does not do that
; part, and the specification is explicit that it does not - which is
; the whole reason DS:ESI is a stack pointer.
;---------------------------------------------------------------------
cb_proc:
        mov     ax,word ptr cs:[pm_ds]  ; our own data, through the one
        mov     fs,ax                   ; selector still readable from here
        inc     byte ptr fs:[cb_hits]

        ; something the caller can see afterwards: EAX + 1, into EBX
        mov     eax,dword ptr es:[di+1Ch]
        inc     eax
        mov     dword ptr es:[di+10h],eax

        ; and the return - IP, CS and FLAGS, off the real-mode stack
        mov     ax,word ptr ds:[si]
        mov     word ptr es:[di+2Ah],ax         ; RMCS_IP
        mov     ax,word ptr ds:[si+2]
        mov     word ptr es:[di+2Ch],ax         ; RMCS_CS
        mov     ax,word ptr ds:[si+4]
        mov     word ptr es:[di+20h],ax         ; RMCS_FLAGS
        add     word ptr es:[di+2Eh],6          ; RMCS_SP, past the three
        iret

;=====================================================================
; And out.  A client terminates with the DOS call it always did; the
; host notices and ends the box the client was.
;=====================================================================
done:
        mov     dx,OFFSET msg_done
        call    puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; Output.  All of it goes through INT 21h, in whichever mode we happen
; to be in - which is the whole point of a DPMI host.
;=====================================================================
puts:
        push    ax
        mov     ah,09h
        int     21h
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
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        call    puts
        ret
fail:
        push    dx
        mov     dx,OFFSET msg_fail
        call    puts
        pop     dx
        call    puts
        ret

; AX in hex
hex16:
        push    ax
        push    cx
        push    dx
        mov     cx,4
h16_lp:
        rol     ax,4
        push    ax
        and     al,0Fh
        cmp     al,10
        jb      h16_dig
        add     al,7
h16_dig:
        add     al,'0'
        mov     dl,al
        push    ax
        mov     ah,02h
        int     21h
        pop     ax
        pop     ax
        loop    h16_lp
        pop     dx
        pop     cx
        pop     ax
        ret

; AL as unsigned decimal
dec8:
        push    ax
        push    bx
        push    cx
        push    dx
        movzx   ax,al
        mov     cx,0
        mov     bx,10
d8_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     d8_div
d8_out:
        pop     dx
        add     dl,'0'
        call    putc
        loop    d8_out
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
RMCS_LEN        EQU     32h
FTEXT_LEN       EQU     34
SIMTEXT_LEN     EQU     39

msg_hi      db  13,10,'DPMITEST - a 16-bit DPMI client',13,10,'$'
msg_p1      db  '  real mode - is there a host?',13,10,'$'
msg_notpm   db  '1686h agrees we are not in protected mode',13,10,'$'
msg_saidpm  db  '1686h claims we already are',13,10,'$'
msg_nohost  db  'no DPMI host answered 1687h',13,10,'$'
msg_found   db  'a host answered, with an entry point',13,10,'$'
msg_ver     db  '         it reports DPMI $'
msg_p32     db  '         32-bit clients: $'
msg_yes     db  'yes',13,10,'$'
msg_no      db  'no',13,10,'$'
msg_noparas db  'DOS would not give the host its private data',13,10,'$'
msg_p2      db  '  and into protected mode:',13,10,'$'
msg_swfail  db  'the mode switch refused',13,10,'$'
msg_inpm    db  'DOS still answers - printed by PM code',13,10,'$'
msg_pebit   db  'and CR0 says PE is set, so it really is',13,10,'$'
msg_nope    db  'but CR0 says we are still in real mode',13,10,'$'
msg_1686    db  '1686h agrees, the other way round now',13,10,'$'
msg_1686bad db  '1686h still thinks we are in real mode',13,10,'$'
msg_p3      db  '  INT 31h - descriptors and memory:',13,10,'$'
msg_v09     db  'the host reports version 0.90',13,10,'$'
msg_v09bad  db  'the version is not one this test knows',13,10,'$'
msg_inc     db  'selectors are 8 apart',13,10,'$'
msg_incbad  db  'the selector increment is not 8',13,10,'$'
msg_desc    db  'a descriptor we built addresses what we said',13,10,'$'
msg_descbad db  'the descriptor did not describe the memory',13,10,'$'
msg_mem     db  '4K of extended memory, written at both ends',13,10,'$'
msg_membad  db  'extended memory could not be had or used',13,10,'$'
msg_memfree db  'and given back',13,10,'$'
msg_memfreebad db 'the handle would not free',13,10,'$'
msg_p4      db  '  DOS, from protected mode:',13,10,'$'
msg_dosver  db  '30h answers with a version',13,10,'$'
msg_dosverbad db '30h did not answer sensibly',13,10,'$'
msg_file    db  'a file written and read back, byte for byte',13,10,'$'
msg_filebad db  'the file did not survive the round trip',13,10,'$'
msg_p5      db  '  0300h - a real-mode interrupt on our behalf:',13,10,'$'
msg_sim     db  'INT 21h ran in real mode and came back',13,10,'$'
msg_simbad  db  'the simulated interrupt failed',13,10,'$'
msg_ax      db  '         AX came back $'
msg_p6      db  '  and a BIOS call, reflected:',13,10,'$'
msg_bios    db  'INT 10h AH=0Fh answered',13,10,'$'
msg_biosbad db  'the reflected BIOS call answered nothing',13,10,'$'
msg_cols    db  '         it says the screen is $'
msg_p7      db  '  0303h - and a callback, the other way about:',13,10,'$'
msg_cb      db  'real mode called us, in protected mode',13,10,'$'
msg_cbno    db  'the callback did not run, or lost its answer',13,10,'$'
msg_cbbad   db  'the callback could not be set up',13,10,'$'
msg_cbfree  db  'and the callback address freed',13,10,'$'
msg_cbfreebad db 'the callback address would not free',13,10,'$'
msg_state   db  '0305h: no state to save across a mode switch',13,10,'$'
msg_statebad db '0305h answered something unexpected',13,10,'$'
msg_done    db  'DPMITEST done',13,10,'$'
msg_ok      db  '    OK   $'
msg_fail    db  '    FAIL $'
msg_crlf    db  13,10,'$'

simtext     db  'and this line came out of real mode.',13,10,'$'
ftext       db  'DPMITEST wrote this from ring 3.',13,10
fname       db  'DPMIT.TMP',0

entry_off   dw  0
entry_seg   dw  0
host_flags  dw  0
host_ver    dw  0
host_para   dw  0
pm_ds       dw  0
pm_es       dw  0
pm_cs       dw  0
mysel       dw  0
memsel      dw  0
ds_base_hi  dw  0
ds_base_lo  dw  0
mem_hi      dw  0
mem_lo      dw  0
mem_hand_hi dw  0
mem_hand_lo dw  0
dos_seg     dw  0
dos_sel     dw  0
fhandle     dw  0
vid_cols    db  0
            db  0
probe       dw  0
cb_seg      dw  0                       ; the callback address, as the host
cb_off      dw  0                       ; gave it to us
cb_hits     db  0                       ; how many times the procedure ran
            db  0
rmcs        db  RMCS_LEN dup(0)
cb_rmcs     db  RMCS_LEN dup(0)          ; the callback's own, NOT the one
                                        ; 0300h is using at the time
fbuf        db  FTEXT_LEN dup(0)

_DP     ENDS
        END
