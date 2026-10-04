;=====================================================================
; DMATEST.ASM - DMATEST.COM, does the 8237 write where a DOS box said?
;
; A driver programs the DMA controller with segment times sixteen.  In
; conventional memory that is the truth; in upper memory and in the
; expanded memory page frame the page tables say otherwise, and the
; controller - which knows nothing of page tables - used to write into
; whatever is really at that number.  IO\VDMA.INC stands in for the
; controller's ports and puts that right.  This asks:
;
;   1  CHANNEL 3's registers read back as they were written - the
;      address the box gave, not the one the controller was given.
;   2  CHANNEL 5's likewise: a word channel, page bit 0 its own.
;   3  a Sound Blaster at 220h, for the rest.  DOSBox-X answers the
;      card's recording with silence - 80h - written by the DMA
;      controller, which is a device writing memory where it was told.
;   4  RECORDED INTO CONVENTIONAL MEMORY: every byte is 80h.  This is
;      the control; it worked before.
;   5  RECORDED INTO UPPER MEMORY: every byte is 80h.  Before, none was.
;      And the channel's current address, read afterwards, is the
;      box's buffer plus the recording - not wherever the controller
;      was really sent.
;   6  RECORDED ACROSS TWO EXPANDED MEMORY PAGES mapped out of order,
;      so that the buffer is in two pieces: every byte is 80h, which
;      only the bounce buffer can manage.
;   7  A RECORDING AIMED AT THE KERNEL never happens.
;
; Checks with nothing to run on say so and are skipped.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_DT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_DT, DS:_DT, ES:_DT, SS:_DT
        ORG     0

SB_BASE         EQU     220h
SB_RESET        EQU     SB_BASE + 6
SB_READ         EQU     SB_BASE + 0Ah
SB_WRITE        EQU     SB_BASE + 0Ch
SB_STATUS       EQU     SB_BASE + 0Eh
REC_LEN         EQU     512

start:
        mov     dx,OFFSET msg_hi
        call    puts
        call    find_umb

;=====================================================================
; 1 - channel 3, a byte channel
;=====================================================================
        mov     dx,OFFSET t_ch3
        call    puts
        mov     eax,[umblin]
        or      eax,eax
        jnz     c1_have
        call    cbuf_lin
c1_have:
        mov     [prog],eax
        cli
        mov     al,07h                  ; masked: nothing moves
        out     0Ah,al
        out     0Ch,al
        mov     al,4Bh                  ; single, the device reads, channel 3
        out     0Bh,al
        mov     eax,[prog]
        out     06h,al
        mov     al,ah
        out     06h,al
        shr     eax,16
        out     82h,al
        mov     al,0FFh
        out     07h,al
        mov     al,01h
        out     07h,al
        out     0Ch,al
        in      al,06h
        mov     ah,al
        in      al,06h
        xchg    al,ah
        mov     [got],ax
        in      al,82h
        mov     [gotpg],al
        out     0Ch,al
        in      al,07h
        mov     ah,al
        in      al,07h
        xchg    al,ah
        mov     [gotcnt],ax
        sti
        mov     ax,[got]
        call    put4
        mov     dl,'/'
        call    putc
        mov     al,[gotpg]
        call    put2
        mov     dl,'/'
        call    putc
        mov     ax,[gotcnt]
        call    put4
        mov     eax,[prog]
        cmp     [got],ax
        jne     c1_bad
        shr     eax,16
        cmp     [gotpg],al
        jne     c1_bad
        cmp     word ptr [gotcnt],01FFh
        jne     c1_bad
        call    ok
        jmp     short c2
c1_bad:
        call    bad

;=====================================================================
; 2 - channel 5, a word channel
;=====================================================================
c2:
        mov     dx,OFFSET t_ch5
        call    puts
        cli
        mov     al,05h
        out     0D4h,al
        out     0D8h,al
        mov     al,49h                  ; single, the device reads, channel 5
        out     0D6h,al
        mov     eax,[prog]
        shr     eax,1
        out     0C4h,al
        mov     al,ah
        out     0C4h,al
        mov     eax,[prog]
        shr     eax,16
        and     al,0FEh
        out     8Bh,al
        mov     al,0FFh
        out     0C6h,al
        xor     al,al
        out     0C6h,al
        out     0D8h,al
        in      al,0C4h
        mov     ah,al
        in      al,0C4h
        xchg    al,ah
        mov     [got],ax
        in      al,8Bh
        mov     [gotpg],al
        out     0D8h,al
        in      al,0C6h
        mov     ah,al
        in      al,0C6h
        xchg    al,ah
        mov     [gotcnt],ax
        sti
        mov     ax,[got]
        call    put4
        mov     dl,'/'
        call    putc
        mov     al,[gotpg]
        call    put2
        mov     dl,'/'
        call    putc
        mov     ax,[gotcnt]
        call    put4
        mov     eax,[prog]
        shr     eax,1
        cmp     [got],ax
        jne     c2_bad
        mov     eax,[prog]
        shr     eax,16
        and     al,0FEh
        cmp     [gotpg],al
        jne     c2_bad
        cmp     word ptr [gotcnt],00FFh
        jne     c2_bad
        call    ok
        jmp     short c3
c2_bad:
        call    bad

;=====================================================================
; 3 - is there a card to drive
;=====================================================================
c3:
        mov     dx,OFFSET t_sb
        call    puts
        call    dsp_reset
        jnc     c3_have
        mov     dx,OFFSET msg_nosb
        call    puts
        jmp     report
c3_have:
        mov     dx,OFFSET msg_ok
        call    puts

;=====================================================================
; 4 - into conventional memory
;=====================================================================
        mov     dx,OFFSET t_conv
        call    puts
        call    cbuf_lin
        call    rec_check

;=====================================================================
; 5 - into upper memory
;=====================================================================
        mov     dx,OFFSET t_umb
        call    puts
        mov     eax,[umblin]
        or      eax,eax
        jnz     c5_have
        mov     dx,OFFSET msg_noumb
        call    puts
        jmp     short c6
c5_have:
        mov     dx,OFFSET msg_at
        call    puts
        mov     eax,[umbphys]
        call    put8
        mov     dx,OFFSET msg_sp
        call    puts
        mov     eax,[umblin]
        call    rec_check

        ; and where the controller has got to is told in the box's terms:
        ; its own buffer, a recording further on
        mov     dx,OFFSET t_umbcur
        call    puts
        cli
        out     0Ch,al
        in      al,02h
        mov     ah,al
        in      al,02h
        xchg    al,ah
        mov     [got],ax
        in      al,83h
        mov     [gotpg],al
        sti
        mov     ax,[got]
        call    put4
        mov     dl,'/'
        call    putc
        mov     al,[gotpg]
        call    put2
        mov     eax,[umblin]
        add     eax,REC_LEN
        cmp     [got],ax
        jne     c5b_bad
        mov     eax,[umblin]
        shr     eax,16
        cmp     [gotpg],al
        jne     c5b_bad
        call    ok
        jmp     short c6
c5b_bad:
        call    bad

;=====================================================================
; 6 - across two expanded memory pages, mapped out of order
;=====================================================================
c6:
        mov     dx,OFFSET t_ems
        call    puts
        call    ems_open
        jnc     c6_have
        mov     dx,OFFSET msg_noems
        call    puts
        jmp     short c7
c6_have:
        movzx   eax,word ptr [emsframe]
        shl     eax,4
        add     eax,4000h - REC_LEN / 2 ; straddling the two
        call    rec_check
        call    ems_close

;=====================================================================
; 7 - into the kernel: refused
;=====================================================================
c7:
        mov     dx,OFFSET t_kernel
        call    puts
        mov     eax,00110000h           ; where PMDOS.SYS is
        mov     cx,256
        call    sb_rec
        jnc     c7_bad                  ; it happened
        call    dsp_reset
        call    ok
        jmp     short report
c7_bad:
        call    bad

report:
        mov     al,05h                  ; channels 1, 3 and 5 masked again
        out     0Ah,al
        mov     al,07h
        out     0Ah,al
        mov     al,05h
        out     0D4h,al
        mov     dx,OFFSET msg_crlf
        call    puts
        cmp     byte ptr [fails],0
        jne     failed
        mov     dx,OFFSET msg_pass
        call    puts
        mov     ax,4C00h
        int     21h
failed:
        mov     dx,OFFSET msg_fail
        call    puts
        mov     ax,4C01h
        int     21h

;=====================================================================
; rec_check - EAX = a linear buffer of REC_LEN bytes.  Fill it with
;             zeroes, record into it, and say how many bytes are 80h.
;=====================================================================
rec_check:
        mov     [rlin],eax
        push    es
        mov     ebx,eax
        shr     ebx,4
        mov     es,bx
        and     ax,0Fh
        mov     di,ax
        push    di
        mov     cx,REC_LEN
        xor     al,al
        cld
        rep     stosb
        pop     di
        pop     es
        mov     eax,[rlin]
        mov     cx,REC_LEN
        call    sb_rec
        jc      rc_none
        push    es
        mov     ebx,[rlin]
        shr     ebx,4
        mov     es,bx
        mov     di,word ptr [rlin]
        and     di,0Fh
        mov     cx,REC_LEN
        xor     bx,bx
rc_count:
        cmp     byte ptr es:[di],80h
        jne     rc_not
        inc     bx
rc_not:
        inc     di
        loop    rc_count
        pop     es
        mov     ax,bx
        call    put4
        cmp     bx,REC_LEN
        jne     rc_bad
        call    ok
        ret
rc_none:
        mov     dx,OFFSET msg_notc
        call    puts
rc_bad:
        call    bad
        ret

;=====================================================================
; sb_rec - EAX = linear, CX = bytes: the card records that many through
;          DMA channel 1.  CF if the controller never reached the end.
;=====================================================================
sb_rec:
        mov     [rlin],eax
        mov     [rlen],cx
        cli
        mov     al,05h                  ; channel 1 masked
        out     0Ah,al
        out     0Ch,al
        mov     al,45h                  ; single, the device writes, channel 1
        out     0Bh,al
        mov     eax,[rlin]
        out     02h,al
        mov     al,ah
        out     02h,al
        shr     eax,16
        out     83h,al
        mov     ax,[rlen]
        dec     ax
        out     03h,al
        mov     al,ah
        out     03h,al
        in      al,08h                  ; nothing left over
        mov     al,01h
        out     0Ah,al                  ; live
        sti
        mov     al,40h                  ; about 11kHz
        call    dsp_out
        mov     al,0A5h
        call    dsp_out
        mov     al,24h                  ; eight bits in, single cycle
        call    dsp_out
        mov     ax,[rlen]
        dec     ax
        call    dsp_out
        mov     al,ah
        call    dsp_out

        push    es
        mov     ax,40h
        mov     es,ax
        mov     bx,es:[6Ch]
        add     bx,18                   ; a second
sr_wait:
        in      al,08h
        test    al,02h                  ; channel 1 has reached its end
        jnz     sr_done
        mov     ax,es:[6Ch]
        cmp     ax,bx
        jne     sr_wait
        pop     es
        mov     al,05h
        out     0Ah,al
        stc
        ret
sr_done:
        pop     es
        mov     al,05h
        out     0Ah,al
        mov     dx,SB_STATUS            ; and the card's interrupt dealt with
        in      al,dx
        clc
        ret

;---------------------------------------------------------------------
; dsp_reset - CF if no DSP answers at SB_BASE
;---------------------------------------------------------------------
dsp_reset:
        mov     dx,SB_RESET
        mov     al,1
        out     dx,al
        mov     cx,100
srs_pause:
        in      al,dx
        loop    srs_pause
        xor     al,al
        out     dx,al
        mov     cx,0FFFFh
srs_wait:
        mov     dx,SB_STATUS
        in      al,dx
        test    al,80h
        jnz     srs_read
        loop    srs_wait
        stc
        ret
srs_read:
        mov     dx,SB_READ
        in      al,dx
        cmp     al,0AAh
        jne     srs_no
        clc
        ret
srs_no:
        stc
        ret

dsp_out:
        push    cx
        push    dx
        push    ax
        mov     dx,SB_WRITE
        mov     cx,0FFFFh
dso_wait:
        in      al,dx
        test    al,80h
        jz      dso_go
        loop    dso_wait
dso_go:
        pop     ax
        out     dx,al
        pop     dx
        pop     cx
        ret

;=====================================================================
; find_umb - a block in upper memory, and where VDS says it really is
;=====================================================================
find_umb:
        mov     ax,5800h
        int     21h
        mov     [strat],ax
        mov     ax,5802h
        int     21h
        mov     [linked],al
        mov     ax,5803h
        mov     bx,1
        int     21h
        mov     ax,5801h
        mov     bx,40h                  ; first fit, upper memory only
        int     21h
        mov     ah,48h
        mov     bx,1024 / 16
        int     21h
        jc      fu_restore
        cmp     ax,0A000h
        jb      fu_low                  ; not up there after all
        mov     [umbseg],ax
fu_restore:
        mov     ax,5801h
        mov     bx,[strat]
        int     21h
        mov     ax,5803h
        movzx   bx,byte ptr [linked]
        int     21h
        cmp     word ptr [umbseg],0
        je      fu_out
        movzx   eax,word ptr [umbseg]
        shl     eax,4
        call    nocross
        mov     [umblin],eax
        mov     dword ptr [dds],REC_LEN
        mov     dword ptr [dds+4],eax
        mov     word ptr [dds+8],0
        mov     ax,8103h
        xor     dx,dx
        mov     di,OFFSET dds
        int     4Bh
        jc      fu_out
        mov     eax,dword ptr [dds+12]
        mov     [umbphys],eax
fu_out:
        ret
fu_low:
        mov     es,ax
        mov     ah,49h
        int     21h
        push    cs
        pop     es
        jmp     short fu_restore

;---------------------------------------------------------------------
; cbuf_lin - EAX = a REC_LEN window of cbuf that crosses no 64K line
; nocross  - EAX = the start of 1K -> the same, for any such block
;---------------------------------------------------------------------
cbuf_lin:
        mov     ax,cs
        movzx   eax,ax
        shl     eax,4
        add     eax,OFFSET cbuf
nocross:
        push    edx
        mov     edx,eax
        and     edx,0FFFFh
        add     edx,REC_LEN
        cmp     edx,10000h
        jbe     nc_ok
        add     eax,REC_LEN
nc_ok:
        pop     edx
        ret

;=====================================================================
; ems_open - two pages of expanded memory, the second mapped first.
;            CF if there is none.
;=====================================================================
ems_open:
        mov     ax,3567h
        int     21h
        mov     di,0Ah
        mov     si,OFFSET emmname
        mov     cx,8
        cld
        repe    cmpsb
        push    cs
        pop     es
        jne     eo_none
        mov     ah,41h
        int     67h
        or      ah,ah
        jnz     eo_none
        mov     [emsframe],bx
        mov     ah,43h
        mov     bx,2
        int     67h
        or      ah,ah
        jnz     eo_none
        mov     [emshandle],dx
        mov     ax,4400h                ; logical 1 in the frame's page 0
        mov     bx,1
        int     67h
        or      ah,ah
        jnz     eo_close
        mov     ax,4401h                ; and logical 0 in page 1
        xor     bx,bx
        mov     dx,[emshandle]
        int     67h
        or      ah,ah
        jnz     eo_close
        clc
        ret
eo_close:
        call    ems_close
eo_none:
        stc
        ret

ems_close:
        mov     ah,45h
        mov     dx,[emshandle]
        int     67h
        ret

;=====================================================================
; helpers
;=====================================================================
ok:
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
put8:                                   ; EAX in hex
        push    eax
        shr     eax,16
        call    put4
        pop     eax
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
        push    dx
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     put1a
        add     al,7
put1a:
        mov     dl,al
        call    putc
        pop     dx
        ret

;=====================================================================
; data
;=====================================================================
prog        dd  0
rlin        dd  0
umblin      dd  0
umbphys     dd  0
dds         db  16 dup(0)
rlen        dw  0
got         dw  0
gotcnt      dw  0
strat       dw  0
umbseg      dw  0
emsframe    dw  0
emshandle   dw  0
gotpg       db  0
linked      db  0
fails       db  0
emmname     db  'EMMXXXX0'

msg_hi      db  13,10,'DMATEST - does the 8237 write where a DOS box said?'
            db  13,10,13,10,'$'
t_ch3       db  'channel 3 reads back ... $'
t_ch5       db  'channel 5 reads back ... $'
t_sb        db  'Sound Blaster at 220h .. $'
t_conv      db  'recorded, conventional . $'
t_umb       db  'recorded, upper memory . $'
t_umbcur    db  'and its address after .. $'
t_ems       db  'recorded, two EMS pages  $'
t_kernel    db  'recorded, into kernel .. $'
msg_at      db  'at $'
msg_sp      db  ' $'
msg_nosb    db  'none (the rest is skipped)',13,10,'$'
msg_noumb   db  'no upper memory (skipped)',13,10,'$'
msg_noems   db  'no expanded memory (skipped)',13,10,'$'
msg_notc    db  'never finished$'
msg_ok      db  ' OK',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_crlf    db  13,10,'$'
msg_pass    db  'DMATEST: all checks passed',13,10,'$'
msg_fail    db  'DMATEST: FAILURES - see above',13,10,'$'

cbuf        db  1024 dup(0)

_DT     ENDS
        END     start
