;=====================================================================
; RMTEST.ASM - RMTEST.COM, a DOS program with no PM-DOS underneath it
;
; This is the only program in the tree that does not run on PM-DOS.
; PM-DOS loads it, parks - leaves protected mode for good - and RMDOS
; starts it in real mode on a machine where the 32-bit kernel is
; present in memory and not running.  Every INT 21h it makes is
; answered by RMDOS.SYS.
;
; So what it is really testing is the handover: that the mode switch
; left a coherent real-mode machine behind, that the DOS in it works,
; and - by returning at all - that PM-DOS can be picked back up.
;
; It says what it finds through 09h, because that is the only way
; anything can be reported from down here: the screen is the only
; thing both sides of the switch agree about.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_RT     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_RT, DS:_RT, ES:_RT, SS:_RT
        ORG     0

start:
        mov     dx,OFFSET m_hi
        call    puts

;--- 30h: is anything answering at all? ------------------------------
        mov     dx,OFFSET t_ver
        call    puts
        mov     ah,30h
        int     21h
        push    ax
        call    put2                    ; the major
        mov     dl,'.'
        call    putc
        pop     ax
        mov     al,ah
        call    put2
        call    ok

;--- 19h / 0Eh: the drive letters ------------------------------------
        mov     dx,OFFSET t_drv
        call    puts
        mov     ah,19h
        int     21h
        add     al,'A'
        mov     dl,al
        call    putc
        mov     dl,':'
        call    putc
        mov     dl,' '
        call    putc
        mov     ah,0Eh
        mov     dl,0
        int     21h
        call    put2
        mov     dx,OFFSET m_letters
        call    puts
        call    ok

;--- 34h and 5D06h: the InDOS flag is the SDA's second byte ----------
; The same check WINTEST makes of the kernel, made of RMDOS, and for
; the same reason: a TSR finds the critical-error flag by subtracting
; one from what 34h answered, and that only works if 34h is pointing
; into a real structure rather than at a loose byte.
        mov     dx,OFFSET t_sda
        call    puts
        mov     ah,34h
        int     21h                     ; ES:BX
        mov     ax,es
        mov     [in_seg],ax
        mov     [in_off],bx

        push    ds
        mov     ax,5D06h
        int     21h                     ; DS:SI
        mov     ax,ds
        mov     bx,si
        pop     ds
        mov     [sda_seg],ax
        mov     [sda_off],bx
        mov     [sda_cx],cx

        mov     ax,[sda_seg]
        call    put4
        mov     dl,':'
        call    putc
        mov     ax,[sda_off]
        call    put4

        ; compared as twenty-bit addresses, because seg:off is not
        ; unique and two pairs naming one byte would pass a register
        ; comparison and prove nothing
        movzx   eax,word ptr [sda_seg]
        shl     eax,4
        movzx   edx,word ptr [sda_off]
        add     eax,edx
        inc     eax
        movzx   ebx,word ptr [in_seg]
        shl     ebx,4
        movzx   edx,word ptr [in_off]
        add     ebx,edx
        cmp     eax,ebx
        jne     bad
        call    ok

;--- 52h: the list of lists, and the arena behind it ------------------
        mov     dx,OFFSET t_lol
        call    puts
        mov     ah,52h
        int     21h
        mov     ax,es
        cmp     ax,0A000h
        jae     bad
        mov     ax,es:[bx-2]            ; the first MCB
        mov     [mcb1],ax
        call    put4
        or      ax,ax
        jz      bad
        cmp     ax,0A000h
        jae     bad
        call    ok

;--- and the chain really walks --------------------------------------
        mov     dx,OFFSET t_walk
        call    puts
        mov     bx,[mcb1]
        xor     cx,cx
w_loop:
        inc     cx
        cmp     cx,512
        ja      bad                     ; a chain that never ends is the
                                        ; failure this test exists for
        mov     es,bx
        mov     al,es:[0]
        cmp     al,'Z'
        je      w_end
        cmp     al,'M'
        jne     bad
        mov     ax,es:[3]
        add     bx,ax
        inc     bx
        jmp     short w_loop
w_end:
        mov     ax,cx
        call    put4
        mov     dx,OFFSET m_blocks
        call    puts
        call    ok

;--- 48h/49h: memory really is allocatable ---------------------------
        mov     dx,OFFSET t_mem
        call    puts
        mov     bx,64                   ; a kilobyte
        mov     ah,48h
        int     21h
        jc      bad
        mov     [blk],ax
        mov     es,ax
        mov     word ptr es:[0],0AA55h
        cmp     word ptr es:[0],0AA55h
        jne     bad
        mov     es,[blk]
        mov     ah,49h
        int     21h
        jc      bad
        call    ok

;--- 36h: does the volume mount at all? ------------------------------
; Between "the disk cannot be read" and "the directory search is
; wrong" there is a whole filesystem, and this check is the fence: it
; needs the boot sector, the BPB and the FAT, and no directory.
        mov     dx,OFFSET t_free
        call    puts
        mov     ah,36h
        mov     dl,0
        int     21h
        cmp     ax,0FFFFh
        je      bad
        push    bx
        call    put4                    ; sectors per cluster
        mov     dl,'/'
        call    putc
        pop     ax
        call    put4                    ; free clusters
        call    ok

;--- 3Dh/3Fh/3Eh: a file, off a real disk, through RMDOS's own FAT ---
; The only check here that touches a disk, and the one that would find
; a FAT walk that is wrong: it opens the kernel's own image and looks
; at the first two bytes, which are known.
        mov     dx,OFFSET t_file
        call    puts
        mov     dx,OFFSET n_pmio
        mov     ax,3D00h
        int     21h
        jc      f_bad
        mov     [hnd],ax
        mov     bx,ax
        mov     cx,16
        mov     dx,OFFSET buf
        mov     ah,3Fh
        int     21h
        jc      f_bad
        push    ax
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        pop     ax
        cmp     ax,16
        jne     f_bad
        mov     al,[buf]
        call    put2
        mov     al,[buf+1]
        call    put2
        ; PMIO.SYS begins with the stub's jump, E9 - the same three
        ; bytes IORM.ASM puts at offset zero.
        cmp     byte ptr [buf],0E9h
        jne     f_bad
        call    ok
        jmp     short done
f_bad:
        push    ax
        mov     dx,OFFSET m_nofile
        call    puts
        pop     ax
        call    put4
        mov     dx,OFFSET m_crlf
        call    puts
        inc     byte ptr [fails]

done:
        mov     dx,OFFSET m_crlf
        call    puts
        mov     al,[fails]
        or      al,al
        jz      d_allok
        mov     dx,OFFSET m_fails
        call    puts
        mov     al,[fails]
        call    put2
        mov     dx,OFFSET m_crlf
        call    puts
        jmp     short d_bye
d_allok:
        mov     dx,OFFSET m_allok
        call    puts
d_bye:
        mov     dx,OFFSET m_bye
        call    puts
        mov     al,[fails]
        mov     ah,4Ch
        int     21h                     ; ...and RMDOS gives the machine back

bad:
        mov     dx,OFFSET m_bad
        call    puts
        inc     byte ptr [fails]
        ret

ok:
        mov     dx,OFFSET m_ok
        call    puts
        ret

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
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
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
        jbe     p1a
        add     al,7
p1a:
        mov     dl,al
        call    putc
        ret

;=====================================================================
m_hi        db  13,10,'RMTEST - a DOS program with no PM-DOS underneath it',13,10,'$'
m_bye       db  'RMTEST done - handing the machine back.',13,10,'$'
m_ok        db  ' ok',13,10,'$'
m_bad       db  ' FAILED',13,10,'$'
m_crlf      db  13,10,'$'
m_fails     db  'checks failed: $'
m_allok     db  'all checks passed.',13,10,'$'
m_letters   db  ' letters$'
m_blocks    db  ' blocks, ending in Z$'
m_nofile    db  ' FAILED, error ','$'

t_ver       db  '  30h says DOS $'
t_drv       db  '  19h/0Eh current drive $'
t_sda       db  '  5D06h swappable data area at $'
t_lol       db  '  52h first MCB is $'
t_walk      db  '  and the arena walks: $'
t_mem       db  '  48h/49h a kilobyte, written and freed$'
t_free      db  '  36h free space (spc/free) ','$'
t_file      db  '  3Dh/3Fh PMIO.SYS starts $'

n_pmio      db  '\PMIO.SYS',0

fails       db  0
in_seg      dw  0
in_off      dw  0
sda_seg     dw  0
sda_off     dw  0
sda_cx      dw  0
mcb1        dw  0
blk         dw  0
hnd         dw  0
buf         db  32 dup(0)

_RT     ENDS
        END
