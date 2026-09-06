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
        mov     [sp0],sp
        mov     dx,OFFSET m_hi
        call    puts

;--- 30h: is anything answering at all? ------------------------------
chk_ver:
        mov     word ptr [resume],OFFSET chk_drv
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
chk_drv:
        mov     word ptr [resume],OFFSET chk_sda
        mov     dx,OFFSET t_drv
        call    puts
        mov     ah,19h
        int     21h
        mov     [curdrv],al             ; keep it: 0Eh is about to need it
        add     al,'A'
        mov     dl,al
        call    putc
        mov     dl,':'
        call    putc
        mov     dl,' '
        call    putc

        ; 0Eh IS SELECT DISK.  It answers "how many drive letters are
        ; there", which is what this check wants, but it does it by
        ; CHANGING TO the drive in DL first - so asking with DL=0 was
        ; asking to be moved to A:, and this test then spent the rest
        ; of its run on an empty floppy: 36h failed, \PMIO.SYS came
        ; back "path not found", and RMDOS looked like it could not
        ; read a disk.  Ask about the drive we are already on.
        mov     ah,0Eh
        mov     dl,[curdrv]
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
chk_sda:
        mov     word ptr [resume],OFFSET chk_lol
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
chk_lol:
        mov     word ptr [resume],OFFSET chk_walk
        mov     dx,OFFSET t_lol
        call    puts
        mov     ah,52h
        int     21h

        ; WHERE A DOS IS ALLOWED TO PUBLISH ITS STRUCTURES.  Below
        ; A000h, or in the HMA - segment FFFFh - because a DOS that
        ; has loaded itself high keeps its list of lists up there with
        ; the rest of itself, which is what DOS=HIGH has always done
        ; and what RMDOS does since v0.50.  A000h to FFFEh is video
        ; memory and ROM and is nobody's data.
        ;
        ; This check used to be the one comparison, and it turned a
        ; DOS that had moved somewhere legitimate into a FAILED - and
        ; then skipped every test after it, which is how a working
        ; file system came to look like a broken list of lists.
        mov     ax,es
        cmp     ax,0FFFFh               ; the HMA - RM_HMA_SEG, spelled out
        je      t52_where               ; because this file deliberately
                                        ; includes none of the kernel's
                                        ; headers: it is a DOS program and
                                        ; it should know only what one knows
        cmp     ax,0A000h
        jae     bad
t52_where:
        mov     ax,es:[bx-2]            ; the first MCB
        mov     [mcb1],ax
        call    put4
        or      ax,ax
        jz      bad
        cmp     ax,0A000h
        jae     bad
        call    ok

;--- and the chain really walks --------------------------------------
chk_walk:
        mov     word ptr [resume],OFFSET chk_mem
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
chk_mem:
        mov     word ptr [resume],OFFSET chk_free
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
chk_free:
        mov     word ptr [resume],OFFSET chk_file
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
chk_file:
        mov     word ptr [resume],OFFSET done
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

;--- 3Dh on a DEVICE, which is not the same question as 3Dh on a file
;
; THIS IS THE CHECK THAT WAS NOT HERE.  Every program in this suite
; INHERITS its handles - stdin, stdout and stderr arrive already
; pointing at CON - so nothing had ever asked RMDOS to open a device
; BY NAME, and it could not: sub_open went straight to the directory,
; "CON" is not in one, and 3Dh answered "file not found".  Windows'
; own kernel opens CON ten times as it starts, got ten refusals, and
; gave up with a message about the DOS version that sent this release
; looking in entirely the wrong place.
;
; 44h AL=00 is asked as well as 3Dh, because a handle that opens and
; is not a character device is a different bug from one that does not
; open, and only the second one has a symptom.
chk_dev:
        mov     word ptr [resume],OFFSET done
        mov     dx,OFFSET t_dev
        call    puts
        mov     dx,OFFSET n_con
        mov     ax,3D02h
        int     21h
        jc      dv_bad
        mov     [hnd],ax
        mov     bx,ax
        mov     ax,4400h                ; IOCTL: get device information
        int     21h
        push    dx
        mov     bx,[hnd]
        mov     ah,3Eh
        int     21h
        pop     dx
;
; AND DX IS SAVED ACROSS THE PRINTING, which is not fussiness.  put4
; ends in putc, and putc passes the character to DOS in DL - so
; printing the answer destroys half of it.  This check failed on its
; first run for exactly that reason, while the thing it was written to
; test was working: the fifth release running in which a return value
; already in a register was destroyed by an instruction that read as
; housekeeping.
        push    dx
        mov     ax,dx
        call    put4
        pop     dx
        test    dx,80h                  ; bit 7: it is a character device
        jz      dv_bad
        call    ok
        jmp     short done
dv_bad:
        mov     dx,OFFSET m_nodev
        call    puts
        inc     byte ptr [fails]
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

; A FAILED CHECK ABANDONS ITSELF AND NOTHING ELSE.
;
; This used to print the failure and RET - and it is JUMPED to, not
; called, so the RET unwound straight out of the run and every check
; after the first failure was skipped in silence.  One broken thing
; hid the state of the seven behind it, which is how a working file
; system came to look like a broken list of lists.
;
; Each check names its successor in [resume] before it starts, so
; failing means going on to the next one.  SP is put back because the
; jump here can come from the middle of a check that had pushed.
bad:
        mov     dx,OFFSET m_bad
        call    puts
        inc     byte ptr [fails]
        mov     sp,[sp0]
        jmp     word ptr [resume]

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
m_nodev     db  ' FAILED - could not open CON, or it is not a device',13,10,'$'
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
t_dev       db  '  3Dh/44h CON is a device $'
n_con       db  'CON',0

n_pmio      db  '\PMIO.SYS',0

fails       db  0
resume      dw  0                  ; the check after the one running
sp0         dw  0                  ; SP as the program was entered
in_seg      dw  0
in_off      dw  0
sda_seg     dw  0
sda_off     dw  0
sda_cx      dw  0
curdrv      db  0                  ; 0Eh selects, so ask about this one
mcb1        dw  0
blk         dw  0
hnd         dw  0
buf         db  32 dup(0)

_RT     ENDS
        END
