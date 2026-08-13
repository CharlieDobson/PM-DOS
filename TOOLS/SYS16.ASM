;=====================================================================
; SYS16.ASM - SYS16.COM, make a PM-DOS boot disk from ordinary DOS
;
;   SYS16 d:        transfer the PM-DOS system to drive d:
;
; The same three jobs as the native CMD\SYS.ASM - boot sector, stage 2
; and the three system files - done by a real-mode 16-bit .COM, so a
; PM-DOS disk can be built on any DOS machine instead of only on a
; machine that already has one.
;
; One thing cannot be done the same way.  SYS.COM carries no boot
; image at all: it runs ON a PM-DOS disk, so the boot code it needs is
; sitting in its own sector 0 and it just reads it.  Here there is no
; PM-DOS disk to read - that a PM-DOS disk does not exist yet is the
; entire reason this program exists - so all three boot sectors and
; stage 2 are BUILT INTO the file, turned into DB statements by
; TOOLS\BIN2INC at build time.  SYS16.COM is therefore only as current
; as the build that produced it; rebuild it when the boot code moves.
;
; The FAT type decides which of the three gets used, and it is worked
; out here from the TARGET's own BPB by the usual cluster-count rule,
; because there is no PM-DOS kernel around to answer 21h/F1h AL=1.
; The target keeps that BPB: only the code around it is replaced.
; Anything the target's own FORMAT put on the disk - FATs, root,
; FSInfo - is left alone, exactly as SYS.COM leaves it.
;
; Unlike SYS.COM this will take any drive letter, not just A: to D:.
; PM-DOS itself only has four drives, but the disk being built does
; not care which letter the machine building it happened to use.
;
; Needs DOS 3.0+ and a 386.  Neither is a restriction: PM-DOS needs a
; 386 to run at all, and the 32-bit registers are what make the
; cluster arithmetic below short enough to read.
;
; Run it from real DOS.  Writing sector 0 from inside Windows needs a
; volume lock this program does not take, and Windows will refuse the
; write rather than corrupt anything - it fails safely, but it fails.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;
; ORDER, not the "option offset=" the rest of this project uses: wlink
; rounds THAT up to the next 4K, so a .COM asking for 100h is built
; for 1000h, DOS loads it at 100h anyway, and every absolute address
; in it points 3840 bytes too high.  It assembles, it links, and it
; runs into nothing.  Every other offset here is already 4K-aligned,
; which is why the rounding has never shown itself before.
;=====================================================================
        .386
        INCLUDE PMDOS.INC

COPYBUF_SIZE    EQU     4000h           ; 16K file-copy buffer
S2_BYTES        EQU     S2_SECTORS * 512

_SYS16  SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_SYS16, DS:_SYS16, ES:_SYS16, SS:_SYS16
        ORG     0                       ; the linker puts us at 100h

;=====================================================================
sy_entry:
        cld
        mov     ax,OFFSET img_end       ; scratch above the image - a
        mov     [tgt_sec],ax            ; .COM owns the whole segment,
        add     ax,512                  ; and the top of it is still
        mov     [work_sec],ax           ; a long way below SP
        add     ax,512
        mov     [copy_buf],ax

;--- the target drive from the tail ----------------------------------
        mov     si,PSP_TAIL
        mov     cl,[si]
        xor     ch,ch
        inc     si
sy_skip:
        or      cx,cx                   ; not JCXZ: the error messages
        jz      sy_usage                ; are too far away to reach
        mov     al,[si]
        cmp     al,' '
        je      sy_step
        cmp     al,9
        jne     sy_arg
sy_step:
        inc     si
        dec     cx
        jmp     short sy_skip
sy_arg:
        cmp     al,'/'                  ; /? and anything else switch-like
        je      sy_usage
        cmp     cx,2
        jb      sy_usage
        cmp     byte ptr [si+1],':'
        jne     sy_usage
        cmp     al,'a'
        jb      sy_up
        cmp     al,'z'
        ja      sy_up
        sub     al,20h
sy_up:
        cmp     al,'A'
        jb      sy_baddrv
        cmp     al,'Z'
        ja      sy_baddrv
        mov     [tgt_letter],al
        mov     [tgt_path],al           ; patch the path template
        mov     [mt_drv],al             ; and the message
        sub     al,'A'
        mov     [tgt_log],al

        mov     si,OFFSET msg_banner
        call    sy_puts
        call    sy_srcdir               ; where our own files live

;--- say WHERE the system files are being looked for -----------------
; The commonest way for this program to fail is for it to be run from
; the very floppy it is transferring to, at which point it hunts for
; PMIO.SYS on the target and finds the target does not have one yet.
; That is invisible unless the deduced directory is printed, so it is
; printed - before anything is written, while the answer is still
; useful.
        mov     si,OFFSET msg_source
        call    sy_puts
        cmp     word ptr [src_len],0
        je      sy_srccur
        mov     si,OFFSET src_dir
        mov     di,OFFSET src_path
        mov     cx,[src_len]
        rep     movsb
        mov     byte ptr [di],0
        mov     si,OFFSET src_path
        jmp     short sy_srcshow
sy_srccur:
        mov     si,OFFSET msg_curdir
sy_srcshow:
        call    sy_puts
        mov     si,OFFSET msg_crlf
        call    sy_puts

;--- the target's boot sector, and what its BPB says it is -----------
        mov     bx,[tgt_sec]
        xor     dx,dx
        mov     cx,1
        call    ab_read
        jc      sy_ioerr
IFDEF SYS16_DEBUG
        call    sy_dump
ENDIF
        call    sy_fattype              ; sets [fat_type], [type_name]
        jc      sy_badbpb
        call    sy_agree                ; ...and the host DOS must read
        jc      sy_disagree             ; the disk the same way we do

        mov     si,OFFSET msg_target
        call    sy_puts
        mov     si,[type_name]
        call    sy_puts

;--- pick the matching boot code and give it the target's BPB --------
        mov     si,OFFSET img_fat12
        mov     cx,3Eh - 0Bh            ; BPB span: through the FAT16
        cmp     byte ptr [fat_type],12  ; EBPB, or the longer FAT32 one
        je      sy_haveimg
        mov     si,OFFSET img_fat16
        cmp     byte ptr [fat_type],16
        je      sy_haveimg
        mov     si,OFFSET img_fat32
        mov     cx,5Ah - 0Bh
sy_haveimg:
        push    cx
        mov     di,[work_sec]
        mov     cx,512
        rep     movsb
        pop     cx

        mov     si,[tgt_sec]            ; the target's own numbers back
        add     si,0Bh                  ; over the boot code
        mov     di,[work_sec]
        add     di,0Bh
        rep     movsb

;--- room for stage 2, checked BEFORE a byte is written --------------
; A disk whose reserved area is one sector has nowhere to put the
; FAT-walking loader, and writing it anyway would land on the first
; FAT.  The classic "No room for system on destination disk" is that
; check, and it has to happen while the disk is still intact.
;---------------------------------------------------------------------
        cmp     byte ptr [fat_type],12
        je      sy_roomok               ; floppies boot in one sector
        mov     si,[tgt_sec]
        movzx   eax,word ptr [si+0Eh]   ; the target's reserved count
        mov     edx,S2_START16 + S2_SECTORS
        cmp     byte ptr [fat_type],32
        jne     sy_room
        mov     edx,S2_START32 + S2_SECTORS
sy_room:
        cmp     eax,edx
        jb      sy_noroom
sy_roomok:

;--- down it goes ----------------------------------------------------
        mov     bx,[work_sec]
        xor     dx,dx
        mov     cx,1
        call    ab_write
        jc      sy_ioerr

        cmp     byte ptr [fat_type],32  ; FAT32 keeps a backup copy,
        jne     sy_nobackup             ; wherever its BPB says
        mov     si,[tgt_sec]
        mov     dx,[si+32h]
        or      dx,dx
        jz      sy_nobackup
        mov     bx,[work_sec]
        mov     cx,1
        call    ab_write
        jc      sy_ioerr
sy_nobackup:

;--- stage 2, hard disks only ----------------------------------------
        cmp     byte ptr [fat_type],12
        je      sy_files
        mov     dx,S2_START16
        cmp     byte ptr [fat_type],32
        jne     sy_s2have
        mov     dx,S2_START32
sy_s2have:
        mov     bx,OFFSET img_s2        ; written straight out of our
        mov     cx,S2_SECTORS           ; own data, padding and all
        call    ab_write
        jc      sy_ioerr

sy_files:
        mov     si,OFFSET msg_boot
        call    sy_puts

;--- the system files ------------------------------------------------
        mov     si,OFFSET name_io
        mov     byte ptr [sys_attr],ATTR_READONLY OR ATTR_HIDDEN OR ATTR_SYSTEM
        call    sy_copy
        mov     si,OFFSET name_dos
        call    sy_copy
        mov     si,OFFSET name_cmd
        mov     byte ptr [sys_attr],ATTR_ARCHIVE
        call    sy_copy

        mov     si,OFFSET msg_done
        call    sy_puts
        mov     ax,4C00h
        int     21h

;--- errors ----------------------------------------------------------
sy_usage:
        mov     si,OFFSET msg_usage
        jmp     short sy_die
sy_baddrv:
        mov     si,OFFSET msg_baddrv
        jmp     short sy_die
sy_badbpb:
        mov     si,OFFSET msg_badbpb
        jmp     short sy_die
sy_noroom:
        mov     si,OFFSET msg_noroom
        jmp     short sy_die
sy_ioerr:
        mov     si,OFFSET msg_ioerr
        jmp     short sy_die
sy_disagree:
        mov     si,OFFSET msg_disagree
sy_die:
        call    sy_perr
        mov     ax,4C01h
        int     21h

;=====================================================================
; sy_fattype - the target's boot sector is in [tgt_sec]; work out
;              which FAT it holds and set [fat_type] and [type_name].
;              CF if the BPB is not one this program can use.
;
; The cluster count is the only thing that decides a FAT type - not
; the media byte, not the size of the disk, and not whatever string
; is sitting in the EBPB, which is a comment.  Same rule the kernel
; uses in FAT.INC, and the same one FORMAT used to build the disk.
;=====================================================================
sy_fattype:
        mov     si,[tgt_sec]
        cmp     word ptr [si+0Bh],512   ; the kernel assumes 512 too
        jne     sf_bad
        movzx   ecx,byte ptr [si+0Dh]   ; sectors per cluster
        or      ecx,ecx
        jz      sf_bad
        cmp     byte ptr [si+10h],0     ; number of FATs
        je      sf_bad
        cmp     word ptr [si+0Eh],0     ; reserved sectors
        je      sf_bad

        movzx   eax,word ptr [si+11h]   ; root entries -> sectors
        shl     eax,5
        add     eax,511
        shr     eax,9
        mov     ebx,eax

        movzx   eax,word ptr [si+16h]   ; sectors per FAT, 16 or 32 bit
        or      eax,eax
        jnz     sf_fatsz
        mov     eax,[si+24h]
sf_fatsz:
        movzx   edx,byte ptr [si+10h]
        imul    eax,edx
        add     ebx,eax                 ; FATs + root
        movzx   eax,word ptr [si+0Eh]
        add     ebx,eax                 ; + reserved = the system area

        movzx   eax,word ptr [si+13h]   ; total sectors, 16 or 32 bit
        or      eax,eax
        jnz     sf_total
        mov     eax,[si+20h]
sf_total:
        cmp     eax,ebx
        jbe     sf_bad
        sub     eax,ebx                 ; data sectors
        xor     edx,edx
        div     ecx                     ; -> clusters

        mov     byte ptr [fat_type],12
        mov     word ptr [type_name],OFFSET msg_fat12
        cmp     eax,4085
        jb      sf_ok
        mov     byte ptr [fat_type],16
        mov     word ptr [type_name],OFFSET msg_fat16
        cmp     eax,65525
        jb      sf_ok
        mov     byte ptr [fat_type],32
        mov     word ptr [type_name],OFFSET msg_fat32
sf_ok:
        clc
        ret
sf_bad:
        stc
        ret

;=====================================================================
; sy_agree - does the host DOS read this volume the way its BPB says?
;            CF if it does not.
;
; It is not a silly question.  MS-DOS 6.0 refuses a FAT16 BPB with
; nine reserved sectors - which is exactly what a PM-DOS hard disk
; has, because stage 2 lives in them - calls the volume a "probable
; non-DOS disk", and quietly invents a geometry of its own: CHKDSK
; reports half the clusters at twice the size.  Sectors still go
; where they are asked to, because INT 25h/26h count from the start
; of the volume, so the boot sector and stage 2 would land correctly
; - and then the three files would be written into a directory PM-DOS
; will never look at, on a disk that boots as far as "missing command
; interpreter".  Better to refuse than to hand back that disk.
;
; INT 21h/36h is the cheapest way to ask, because the answer comes
; back in the DOS driver's own terms rather than the BPB's.
;=====================================================================
sy_agree:
        push    bx
        push    dx
        mov     ah,36h
        mov     dl,[tgt_log]
        inc     dl                      ; 36h counts drives from 1
        int     21h
        cmp     ax,0FFFFh               ; not a drive DOS will discuss
        je      sa_bad
        mov     si,[tgt_sec]
        cmp     cx,word ptr [si+0Bh]    ; bytes per sector
        jne     sa_bad
        movzx   bx,byte ptr [si+0Dh]    ; sectors per cluster
        cmp     ax,bx
        jne     sa_bad
        pop     dx
        pop     bx
        clc
        ret
sa_bad:
        pop     dx
        pop     bx
        stc
        ret

;=====================================================================
; ab_read / ab_write - DX = first logical sector, CX = sectors,
;                      BX -> buffer, [tgt_log] = drive.  CF on error.
;
; INT 25h and 26h come in two shapes.  The original addresses sectors
; with a 16-bit DX and cannot speak to a volume of more than 65535 of
; them; DOS 4.0 added a packet whose sector number is 32 bits wide.
; Which one a given DOS will accept for a given volume is not worth
; predicting, so the first call tries the old form and, if it is
; refused, switches to the packet and stays there.  Every sector this
; program touches is in the first 17 of the volume, so the choice is
; about what DOS will allow, never about what it can reach.
;
; Both forms leave the CALLER'S flags on the stack - a 1983 calling
; convention that outlived its reason.  Rather than pop the word and
; trust every DOS and every emulator to have left exactly one, SP is
; saved across the call and put back: the carry is carried over the
; top of that in AX, because MOV SP does not touch the flags and SBB
; AX,AX / ADD AX,1 turns a carry into a register and back again.
;=====================================================================
ab_read:
        mov     byte ptr [ab_wr],0
        jmp     short ab_go
ab_write:
        mov     byte ptr [ab_wr],1
ab_go:
        mov     [ab_lba],dx
        mov     [ab_cnt],cx
        mov     [ab_buf],bx
        mov     byte ptr [ab_again],0
IFDEF SYS16_FAKEIO
        jmp     ab_fake                 ; see the note at ab_fake
ENDIF
ab_try:
        mov     [ab_sp],sp
        cmp     byte ptr [ab_ext],0
        jne     ab_packet

        mov     al,[tgt_log]            ; the original form
        mov     bx,[ab_buf]
        mov     cx,[ab_cnt]
        mov     dx,[ab_lba]
        cmp     byte ptr [ab_wr],0
        je      ab_c25
        int     26h
        jmp     short ab_done
ab_c25:
        int     25h
        jmp     short ab_done

ab_packet:
        mov     ax,[ab_lba]             ; the DOS 4.0 packet
        mov     word ptr [ab_pkt],ax
        mov     word ptr [ab_pkt+2],0
        mov     ax,[ab_cnt]
        mov     word ptr [ab_pkt+4],ax
        mov     ax,[ab_buf]
        mov     word ptr [ab_pkt+6],ax
        mov     word ptr [ab_pkt+8],ds
        mov     al,[tgt_log]
        mov     cx,0FFFFh
        mov     bx,OFFSET ab_pkt
        cmp     byte ptr [ab_wr],0
        je      ab_p25
        int     26h
        jmp     short ab_done
ab_p25:
        int     25h

ab_done:
        sbb     ax,ax                   ; the carry into a register,
        mov     sp,[ab_sp]              ; the stack back to our own,
        add     ax,1                    ; and the carry out again
        jnc     ab_ok
        cmp     byte ptr [ab_again],0
        jne     ab_bad
        cmp     byte ptr [ab_ext],0
        jne     ab_bad
        mov     byte ptr [ab_ext],1     ; refused - try the packet once
        mov     byte ptr [ab_again],1
        jmp     short ab_try
ab_ok:
        clc
        ret
ab_bad:
        stc
        ret

IFDEF SYS16_FAKEIO
;---------------------------------------------------------------------
; ab_fake - the volume's first sectors in an ordinary file, SECTORS.BIN
;
; Built only with -dSYS16_FAKEIO, and only because DOSBox 0.74's INT
; 25h is a stub: it clears carry and leaves the buffer exactly as it
; found it, so under DOSBox this program can never get past reading
; the target's BPB.  Swapping the two INT calls for file I/O runs
; every other line - the FAT-type rule, the splice, the room check,
; the stage-2 placement, the copies - against a sector image that can
; then be checked byte for byte on the host.  The two INT calls
; themselves are the one part of this program a real DOS has to test.
;---------------------------------------------------------------------
ab_fake:
        mov     dx,OFFSET fake_name
        mov     ax,3D02h
        int     21h
        jc      ab_bad
        mov     bx,ax
        mov     ax,[ab_lba]             ; sector -> byte offset
        mov     dx,512
        mul     dx
        mov     cx,dx
        mov     dx,ax
        mov     ax,4200h
        int     21h
        jc      af_shut
        mov     ax,[ab_cnt]
        mov     dx,512
        mul     dx
        mov     cx,ax
        mov     dx,[ab_buf]
        mov     ah,3Fh
        cmp     byte ptr [ab_wr],0
        je      af_go
        mov     ah,40h
af_go:
        int     21h
af_shut:
        pushf
        mov     ah,3Eh                  ; BX is still the handle: DOS
        int     21h                     ; hands back everything but AX
        popf
        jc      ab_bad
        clc
        ret
fake_name   db  'SECTORS.BIN',0
ENDIF

;=====================================================================
; sy_srcdir - the directory SYS16.COM itself was loaded from, which is
;             where PMIO.SYS and friends are expected to be sitting.
;
; DOS 3.0 put the program's own path at the end of its environment,
; and it is the only way a .COM can find out where it came from - the
; command tail does not carry it.  On anything older, or if the
; environment looks wrong, the prefix stays empty and the files are
; looked for in the current directory, which is where sy_copy falls
; back to in any case.
;=====================================================================
sy_srcdir:
        push    es
        mov     word ptr [src_len],0
        mov     ah,30h
        int     21h
        cmp     al,3
        jb      sd_out

        mov     bx,2Ch                  ; PSP: environment segment
        mov     es,[bx]
        mov     ax,es
        or      ax,ax
        jz      sd_out
        xor     di,di
sd_scan:
        cmp     byte ptr es:[di],0      ; the double zero that ends it
        jne     sd_next
        cmp     byte ptr es:[di+1],0
        je      sd_found
sd_next:
        inc     di
        cmp     di,7FF0h                ; a runaway environment is not
        jb      sd_scan                 ; worth chasing off the end
        jmp     short sd_out
sd_found:
        add     di,4                    ; past both zeros and the count

        xor     bx,bx                   ; BX = length kept so far
        xor     cx,cx                   ; CX = length up to the last \
sd_copy:
        mov     al,es:[di]
        or      al,al
        jz      sd_cut
        mov     [src_dir+bx],al
        inc     di
        inc     bx
        cmp     al,'\'
        je      sd_mark
        cmp     al,':'
        jne     sd_more
sd_mark:
        mov     cx,bx                   ; keep the separator itself
sd_more:
        cmp     bx,80
        jb      sd_copy
        xor     cx,cx                   ; too long to be a real path
sd_cut:
        mov     [src_len],cx
sd_out:
        pop     es
        ret

;=====================================================================
; sy_copy - SI -> a bare 8.3 name.  Copy it from our own directory to
;           the root of the target and give it [sys_attr].  Any
;           failure ends the program: a half-transferred system is
;           worse than none, and there is nothing useful to carry on
;           with once a system file is missing.
;=====================================================================
sy_copy:
        mov     [cp_name],si
        ; The prefix lives in src_dir and the candidate path is built
        ; fresh in src_path every time.  It used to be built ONCE, in
        ; place, and the current-directory fallback below overwrote the
        ; whole of src_path with the bare name - so the file after the
        ; first one to fall back was looked for at "<prefix bytes of the
        ; PREVIOUS name><this name>", a path that cannot exist.  It then
        ; fell back itself and worked, which is exactly why it survived:
        ; the bug repaired its own symptom one file late.
        mov     si,OFFSET src_dir       ; our directory...
        mov     di,OFFSET src_path
        mov     cx,[src_len]
        rep     movsb
        mov     si,[cp_name]            ; ...+ the name
        call    sy_strcpy
        mov     si,[cp_name]
        mov     di,OFFSET tgt_path + 3  ; the target's root + the name
        call    sy_strcpy

        mov     si,OFFSET msg_file      ; "  PMIO.SYS"
        call    sy_puts
        mov     si,[cp_name]
        call    sy_puts
        mov     si,OFFSET msg_crlf
        call    sy_puts

        mov     dx,OFFSET src_path
        mov     ax,3D00h
        int     21h
        jnc     sc_got
        cmp     word ptr [src_len],0
        je      sc_nosrc
        mov     si,[cp_name]            ; not beside us - try the
        mov     di,OFFSET src_path      ; current directory
        call    sy_strcpy
        mov     dx,OFFSET src_path
        mov     ax,3D00h
        int     21h
        jc      sc_nosrc
sc_got:
        mov     [src_hdl],ax

        mov     dx,OFFSET tgt_path      ; a previous system's file may
        xor     cx,cx                   ; be sitting there read-only
        mov     ax,4301h
        int     21h                     ; failure just means "new"

        mov     dx,OFFSET tgt_path
        xor     cx,cx
        mov     ah,3Ch
        int     21h
        jc      sc_nodst
        mov     [tgt_hdl],ax

sc_pump:
        mov     bx,[src_hdl]
        mov     cx,COPYBUF_SIZE
        mov     dx,[copy_buf]
        mov     ah,3Fh
        int     21h
        jc      sc_iofail
        or      ax,ax
        jz      sc_done
        mov     cx,ax
        mov     bx,[tgt_hdl]
        mov     dx,[copy_buf]
        mov     ah,40h
        int     21h
        jc      sc_iofail
        cmp     ax,cx
        jb      sc_full
        jmp     short sc_pump

sc_done:
        mov     bx,[tgt_hdl]
        mov     ah,3Eh
        int     21h
        mov     bx,[src_hdl]
        mov     ah,3Eh
        int     21h
        mov     dx,OFFSET tgt_path      ; the promised attributes
        movzx   cx,byte ptr [sys_attr]
        mov     ax,4301h
        int     21h
        ret

;---------------------------------------------------------------------
; The failure paths.  AX is captured FIRST at every one of them: after
; a failed INT 21h it holds the DOS error code, and the very next
; thing each of these does is close handles, which destroys it.  Three
; different failures used to print two nearly identical messages and
; no code at all, so "it would not copy PMIO.SYS" could mean the
; source was not found, the target could not be created, or the disk
; filled up - and there was no way to tell them apart from the output.
;---------------------------------------------------------------------
sc_full:
        mov     [err_ax],ax             ; bytes actually written, here
        mov     si,OFFSET msg_full
        jmp     short sc_shut
sc_iofail:
        mov     [err_ax],ax
        mov     si,OFFSET msg_wrfail
sc_shut:
        push    si
        mov     bx,[tgt_hdl]
        mov     ah,3Eh
        int     21h
        mov     bx,[src_hdl]
        mov     ah,3Eh
        int     21h
        pop     si
        jmp     short sc_die
sc_nodst:
        mov     [err_ax],ax
        mov     si,OFFSET msg_crfail
        mov     bx,[src_hdl]
        mov     ah,3Eh
        int     21h
        jmp     short sc_die
sc_nosrc:
        mov     [err_ax],ax
        mov     si,OFFSET msg_nosrc
sc_die:
        call    sy_perr
        mov     si,[cp_name]
        call    sy_perr
        mov     si,OFFSET msg_errcode
        call    sy_perr
        mov     al,byte ptr [err_ax+1]
        call    sy_hexe
        mov     al,byte ptr [err_ax]
        call    sy_hexe
        mov     si,OFFSET msg_crlf
        call    sy_perr
        mov     si,OFFSET msg_lastpath
        call    sy_perr
        mov     si,OFFSET src_path
        call    sy_perr
        mov     si,OFFSET msg_crlf
        call    sy_perr
        mov     si,OFFSET msg_tgtpath
        call    sy_perr
        mov     si,OFFSET tgt_path
        call    sy_perr
        mov     si,OFFSET msg_crlf
        call    sy_perr
        mov     ax,4C01h
        int     21h

;---------------------------------------------------------------------
; sy_puts / sy_perr - the ASCIIZ string at SI to stdout / stderr
;---------------------------------------------------------------------
sy_perr:
        mov     word ptr [ps_hdl],2
        jmp     short sy_puts0
sy_puts:
        mov     word ptr [ps_hdl],1
sy_puts0:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     dx,si
        xor     cx,cx
sp_len:
        cmp     byte ptr [si],0
        je      sp_go
        inc     si
        inc     cx
        jmp     short sp_len
sp_go:
        jcxz    sp_out
        mov     bx,[ps_hdl]
        mov     ah,40h
        int     21h
sp_out:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

IFDEF SYS16_DEBUG
;---------------------------------------------------------------------
; sy_dump - the first 24 bytes of the sector buffer, in hex.  Built
;           only with -dSYS16_DEBUG, because when a boot sector will
;           not parse the first question is always whether it was ever
;           read - and INT 25h is perfectly capable of saying yes and
;           leaving the buffer exactly as it found it.
;---------------------------------------------------------------------
sy_dump:
        push    cx
        push    si
        mov     si,[tgt_sec]
        mov     cx,24
sd_lp:
        mov     al,[si]
        inc     si
        push    si
        call    sy_hex
        pop     si
        loop    sd_lp
        mov     si,OFFSET msg_crlf
        call    sy_puts
        pop     si
        pop     cx
        ret

sy_hex:
        push    ax
        mov     ah,al
        shr     al,4
        call    sy_nyb
        mov     [hexbuf],al
        mov     al,ah
        and     al,0Fh
        call    sy_nyb
        mov     [hexbuf+1],al
        mov     si,OFFSET hexbuf
        call    sy_puts
        pop     ax
        ret
sy_nyb:
        add     al,'0'
        cmp     al,'9'
        jbe     sy_n1
        add     al,7
sy_n1:
        ret
hexbuf      db  '00 ',0
ENDIF

;---------------------------------------------------------------------
; sy_hexe - AL as two hex digits on STDERR.  A separate routine from
;           the SYS16_DEBUG sy_hex above on purpose: this one is
;           always built, goes to the same handle as the error text it
;           is embedded in, and has no trailing space.
;---------------------------------------------------------------------
sy_hexe:
        push    ax
        push    si
        mov     ah,al
        shr     al,4
        call    sy_nybe
        mov     [ehexbuf],al
        mov     al,ah
        call    sy_nybe
        mov     [ehexbuf+1],al
        mov     si,OFFSET ehexbuf
        call    sy_perr
        pop     si
        pop     ax
        ret
sy_nybe:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     sy_ne1
        add     al,7
sy_ne1:
        ret
ehexbuf     db  '00',0

;---------------------------------------------------------------------
; sy_strcpy - the ASCIIZ string at SI to DI, terminator included
;---------------------------------------------------------------------
sy_strcpy:
        push    ax
sy_sc1:
        mov     al,[si]
        mov     [di],al
        inc     si
        inc     di
        or      al,al
        jnz     sy_sc1
        pop     ax
        ret

;=====================================================================
; Data
;=====================================================================
tgt_sec     dw  0                       ; scratch carve-outs
work_sec    dw  0
copy_buf    dw  0
src_hdl     dw  0
tgt_hdl     dw  0
cp_name     dw  0
src_len     dw  0
type_name   dw  0
ps_hdl      dw  1
ab_lba      dw  0
ab_cnt      dw  0
ab_buf      dw  0
ab_sp       dw  0
ab_wr       db  0
ab_ext      db  0                       ; 0 = the DOS 3 form still works
ab_again    db  0
ab_pkt      dd  0                       ; DOS 4 packet: sector,
            dw  0                       ; count,
            dd  0                       ; buffer
err_ax      dw  0                       ; the failing call's AX, kept
tgt_log     db  0
tgt_letter  db  0
fat_type    db  0
sys_attr    db  0

name_io     db  'PMIO.SYS',0
name_dos    db  'PMDOS.SYS',0
name_cmd    db  'COMMAND.COM',0
tgt_path    db  'A:\'
            db  16 dup(0)
src_dir     db  100 dup(0)              ; the directory SYS16 came from
src_path    db  100 dup(0)              ; that, then the name, rebuilt
                                        ; for every file

msg_banner  db  'SYS16 - PM-DOS system transfer',13,10,0
msg_target  db  'Target '
mt_drv      db  'A: is ',0
msg_fat12   db  'FAT12',13,10,0
msg_fat16   db  'FAT16',13,10,0
msg_fat32   db  'FAT32',13,10,0
msg_boot    db  '  boot sector',13,10,0
msg_file    db  '  ',0
msg_crlf    db  13,10,0
msg_done    db  'System transferred',13,10,0
msg_usage   db  'Usage: SYS16 d:',13,10
            db  'Transfers PM-DOS to drive d: from the directory '
            db  'SYS16.COM was run from.',13,10,0
msg_baddrv  db  'Invalid drive specification',13,10,0
msg_badbpb  db  'Not a disk this can write to - format it first',13,10,0
msg_noroom  db  'No room for system on destination disk',13,10,0
msg_disagree db 'This DOS does not read the disk the way its own BPB '
            db  'describes it,',13,10
            db  'and would write the system files where PM-DOS could '
            db  'not find them.',13,10,0
msg_ioerr   db  'Disk error writing the system areas',13,10,0
msg_source  db  'System files taken from ',0
msg_curdir  db  'the current directory',0
msg_nosrc   db  'Cannot find ',0
msg_crfail  db  'Cannot create on the target: ',0
msg_wrfail  db  'Cannot write ',0
msg_full    db  'Disk full writing ',0
msg_errcode db  ' - DOS error ',0
msg_lastpath db '  source tried: ',0
msg_tgtpath db  '  target tried: ',0

;=====================================================================
; The boot code, generated from the linked binaries by BIN2INC.  Stage
; 2 is padded out to the whole reserved area it will be written into:
; the loader is under a sector and a half of real code, but it is
; copied a sector at a time and the rest of the sectors have to be
; something.  Zeros is what MKDISK leaves there.
;=====================================================================
        INCLUDE BOOTIMG.INC
            db  S2_BYTES - img_s2_len dup(0)

img_end:                                ; scratch begins here

_SYS16  ENDS
        END
