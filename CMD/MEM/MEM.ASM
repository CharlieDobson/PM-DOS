;=====================================================================
; MEM.ASM - MEM.EXE, the memory report, an EXTERNAL command
;
;   MEM             the summary table
;   MEM /D          plus every block in the DOS arena, and the 32-bit
;                   drivers DEVICE= loaded
;   MEM /F          plus the FREE blocks only
;
; MEM was an external in MS-DOS and is an external here.  It was
; briefly an internal in COMMAND.COM, which was the wrong place twice
; over: it made the interpreter carry a report nobody needs at the
; prompt, and it hid the fact that everything MEM does it does through
; the ordinary API - which is the interesting part, because a report
; that has to reach inside the kernel to work is a report that proves
; the kernel has no way to ask.
;
; What it asks:
;   21h/38h         the country's thousands separator
;   21h/F1h AL=4    extended memory size, as the BIOS reported it
;   21h/F1h AL=8    one block of one arena, by index
;   21h/F1h AL=9    the EMS page frame and page counts, which it has
;                   to ask for rather than call INT 67h: expanded
;                   memory is served to V86 guests and this is a
;                   native program at ring 3
;   21h/F1h AL=25h  the 32-bit drivers: NDF_COUNT, NDF_INFO for each
;                   one's name, image and kind, NDF_MEM for what it
;                   allocated after it was loaded
;
; IT USED TO READ THE HEADERS ITSELF, from the pointer 21h/52h hands
; out, which is exactly what MEM did under real DOS and worked here for
; the same reason: the program and the kernel shared one address space.
; They stopped sharing it when native programs moved to ring 3, and the
; fix is the argument this program was written to make in the first
; place - a report that has to reach inside the kernel is a report
; proving the kernel has no way to ask.  Now it asks.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC
        INCLUDE NDRV.INC                ; the 32-bit drivers' row

;--- what a block's signature means, as ARENA.INC lays it out --------
MCB_LEN         EQU     16              ; the header, which is overhead
SIG_USED        EQU     'M'
SIG_FREE        EQU     'Z'
SIG_END         EQU     'E'

BLOCKS_MAX      EQU     2000            ; refuse to loop forever on a
                                        ; corrupted chain

;--- the table's columns ----------------------------------------------
; The label field is LABEL_W wide (print_pad fills it), and col_kb
; prints two spaces and a NUM_W number after it, so the columns are
; provably where the numbers land.
LABEL_W         EQU     22              ; the row-label field
NUM_W           EQU     11              ; each number column

; The lines under the table: a label this wide puts a NUM_W number
; flush with the table's last column, which is where every figure on
; the screen ends.
LINE_W          EQU     LABEL_W + 2 * (2 + NUM_W) + 2

_MEM    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MEM, DS:_MEM, ES:_MEM, SS:_MEM
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET mem_entry>,<MEM_BSS>,4096

;=====================================================================
mem_entry:
        ; THE COUNTRY'S THOUSANDS SEPARATOR, from 38h, as CMD.EXE asks
        ; for DIR's numbers: COUNTRY= decides, and without one the
        ; kernel answers for the United States and the comma stays.
        mov     edx,OFFSET ctry_buf
        mov     ax,3800h
        int     21h
        jc      me_nosep
        mov     al,byte ptr [ctry_buf+7]
        or      al,al
        jz      me_nosep
        cmp     al,'$'                  ; 09h would end the number there
        je      me_nosep
        mov     [num_sep],al
me_nosep:

        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
me_arg:
        or      ecx,ecx
        jz      me_argdone
        mov     al,[esi]
        cmp     al,0Dh
        je      me_argdone
        cmp     al,'/'
        je      me_slash
        cmp     al,'-'
        je      me_slash
        inc     esi
        dec     ecx
        jmp     short me_arg
me_slash:
        inc     esi
        dec     ecx
        jz      me_argdone
        mov     al,[esi]
        call    upcase
        cmp     al,'D'
        je      me_sw_d
        cmp     al,'F'
        je      me_sw_f
        cmp     al,'?'
        je      me_help
        jmp     me_badsw                ; the whole report sits in between
me_sw_d:
        mov     byte ptr [want_list],1
        jmp     short me_argnext
me_sw_f:
        mov     byte ptr [want_list],1
        mov     byte ptr [free_only],1
me_argnext:
        inc     esi
        dec     ecx
        jmp     short me_arg
me_argdone:

;=====================================================================
; The summary.
;
; Sizes are in KB and rounded DOWN, so the columns can be trusted not
; to add up to more memory than the machine has - which is the failure
; mode that makes a memory report useless.
;=====================================================================
        call    survey

        mov     edx,OFFSET msg_head1
        call    print
        mov     edx,OFFSET msg_head2
        call    print

        ; conventional memory - what a 16-bit DOS program can have
        mov     edx,OFFSET msg_row_conv
        call    row_label
        mov     eax,[c_total]
        shr     eax,10
        call    col_kb
        mov     eax,[c_used]
        shr     eax,10
        call    col_kb
        mov     eax,[c_free]
        shr     eax,10
        call    col_kb
        call    crlf

        ; upper memory, when there is any
        cmp     dword ptr [u_total],0
        je      no_upper
        mov     edx,OFFSET msg_row_upper
        call    row_label
        mov     eax,[u_total]
        shr     eax,10
        call    col_kb
        mov     eax,[u_used]
        shr     eax,10
        call    col_kb
        mov     eax,[u_free]
        shr     eax,10
        call    col_kb
        call    crlf
no_upper:

        ; the kernel and the two program slots, and the page tables at
        ; the top - OS Reserved: all of it in use
        mov     edx,OFFSET msg_row_sys
        call    row_label
        mov     eax,[x_osrsv]
        shr     eax,10
        push    eax
        call    col_kb                  ; total
        pop     eax
        call    col_kb                  ; used - all of it
        xor     eax,eax
        call    col_kb                  ; free - none
        call    crlf

        ; the arena, as the block chain actually stands
        mov     edx,OFFSET msg_row_arena
        call    row_label
        mov     eax,[a_total]
        shr     eax,10
        call    col_kb
        mov     eax,[a_used]
        shr     eax,10
        call    col_kb
        mov     eax,[a_free]
        shr     eax,10
        call    col_kb
        call    crlf

        mov     edx,OFFSET msg_head2
        call    print
        mov     edx,OFFSET msg_row_tot
        call    row_label
        ; The three rows above, added up - and they have to add up, or
        ; the table is worse than no table at all.
        mov     eax,[a_total]
        add     eax,[c_total]
        add     eax,[u_total]
        add     eax,[x_osrsv]
        shr     eax,10
        call    col_kb
        mov     eax,[a_used]
        add     eax,[c_used]
        add     eax,[u_used]
        add     eax,[x_osrsv]
        shr     eax,10
        call    col_kb
        mov     eax,[a_free]
        add     eax,[c_free]
        add     eax,[u_free]
        shr     eax,10
        call    col_kb
        call    crlf
        call    crlf

        ; THE LINES UNDER THE TABLE SHARE ITS RIGHT EDGE.  Each is a
        ; label padded to LINE_W, a number right-aligned in NUM_W, and
        ; a unit after it, so every figure on the screen ends in the
        ; same column as the table's Free column and the unit hangs
        ; off it.  They used to be three blocks with three edges - the
        ; numbers left-aligned after a padded label here, right-aligned
        ; to a column of their own for EMS, and the frame somewhere
        ; else again - which made the report hard to read on a real
        ; screen.
        ;
        ; The BIOS's own figure is not the table's total and is not
        ; supposed to be: it counts every byte above 1M, including the
        ; megabyte the kernel and the slots are sitting in.
        mov     edx,OFFSET msg_xms
        mov     eax,[x_memkb]
        call    line_kb

        mov     edx,OFFSET msg_largest
        mov     eax,[a_largest]
        shr     eax,10
        call    line_kb

        mov     edx,OFFSET msg_blocks
        mov     eax,[n_used]
        call    line_num

        mov     edx,OFFSET msg_bfree
        mov     eax,[n_free]
        call    line_num

        ; Expanded memory, and only when there is any.  MEM.EXE is a
        ; native program at ring 3 and cannot make an INT 67h call, so
        ; the kernel is asked instead - see 21h/F1h AL=9.
        mov     eax,0F109h
        int     21h
        or      eax,eax
        jz      me_noems
        mov     [e_frame],ebx
        mov     [e_total],ecx
        mov     [e_free],edx
        call    crlf
        mov     edx,OFFSET msg_ems
        mov     eax,[e_total]
        shl     eax,4                   ; 16K pages, in KB
        call    line_kb

        mov     edx,OFFSET msg_emsfr
        mov     eax,[e_free]
        shl     eax,4
        call    line_kb

        ; The frame is a segment and prints as one - four hex digits,
        ; right-aligned to the same edge as the numbers above it.
        mov     edx,OFFSET msg_emspf
        mov     ebx,LINE_W + NUM_W - 4
        call    print_pad
        mov     eax,[e_frame]
        call    puthex16
        call    crlf
me_noems:

        cmp     byte ptr [want_list],0
        je      me_done
        call    list_blocks
        call    list_dos
        cmp     byte ptr [free_only],0
        jne     me_done                 ; a driver is never free
        call    list_drv
me_done:
        mov     ax,4C00h
        int     21h

; /? IS AN ANSWER AND NOT A MISTAKE, so it does not share an exit
; code with an unrecognised switch, which is what it used to do.
me_help:
        mov     edx,OFFSET msg_usage
        call    print
        mov     ax,4C00h
        int     21h

me_badsw:
        mov     edx,OFFSET msg_badsw
        call    print
me_usage:
        mov     edx,OFFSET msg_usage
        call    print
        mov     ax,4C01h
        int     21h

;=====================================================================
; survey - walk the arena once and total it up.
;
; Everything the summary prints comes from this walk, so the totals
; and the block list below can never disagree with each other.
;=====================================================================
survey:
        mov     ax,0F104h               ; extended memory, in KB
        int     21h
        jc      sv_noxms
        mov     [x_memkb],eax

        ; AND WHAT LIES PAST THE ARENA'S END, which is OS Reserved too:
        ; the page tables for memory above 256MB sit at the top of
        ; memory with the arena stopping below them (PAGING.INC,
        ; pg_hiram).  EBX is the arena's end.  Counted in KB so that
        ; the top of a machine near 4GB cannot overflow.
        add     eax,HMA_BASE / 1024     ; the top of memory, in KB
        shr     ebx,10
        sub     eax,ebx
        jbe     sv_noxms                ; nothing past it
        shl     eax,10
        add     [x_osrsv],eax
sv_noxms:

        xor     ebx,ebx                 ; block zero of the kernel's arena
sv_lp:
        mov     dl,F8_ARENA
        call    get_block
        jc      sv_out
        mov     eax,dword ptr [blk+F8_SIG]
        cmp     eax,SIG_END
        je      sv_out
        cmp     eax,SIG_USED
        je      sv_used

        inc     dword ptr [n_free]
        mov     eax,dword ptr [blk+F8_SIZE]
        add     [a_free],eax
        cmp     eax,[a_largest]
        jbe     sv_next
        mov     [a_largest],eax
        jmp     short sv_next
sv_used:
        inc     dword ptr [n_used]
        mov     eax,dword ptr [blk+F8_SIZE]
        add     [a_used],eax
sv_next:
        ; the header is overhead, and belongs in "used" either way
        add     dword ptr [a_used],MCB_LEN
        inc     ebx
        cmp     ebx,BLOCKS_MAX
        jb      sv_lp
sv_out:
        mov     eax,[a_used]
        add     eax,[a_free]
        mov     [a_total],eax

;--- and the conventional arena the DOS box runs in -------------------
; A separate chain in a separate place with a separate header format -
; sixteen bytes, 'M' or 'Z', a length in paragraphs - which MEM used to
; read where it lay, being a flat 32-bit program with the run of the
; machine.  It is not one any more; both walks go through the same call
; and the kernel is what knows the difference between the two shapes.
        xor     ebx,ebx
cv_lp:
        mov     dl,F8_CONV
        call    get_block
        jc      cv_out
        mov     eax,dword ptr [blk+F8_SIZE]
        cmp     dword ptr [blk+F8_OWNER],0
        je      cv_free
        add     [c_used],eax
        jmp     short cv_next
cv_free:
        add     [c_free],eax
        cmp     eax,[c_largest]
        jbe     cv_next
        mov     [c_largest],eax
cv_next:
        add     dword ptr [c_used],RMCB_SIZE    ; the header, as above
        inc     ebx
        cmp     ebx,BLOCKS_MAX
        jb      cv_lp
cv_out:
        mov     eax,[c_used]
        add     eax,[c_free]
        mov     [c_total],eax

;--- and upper memory, which is the same chain past umb_head ----------
; A THIRD PASS AND NOT A THIRD ARENA.  The kernel classifies, because
; the boundary is a word in the DOS data segment and this program runs
; in a segment of its own; all that arrives here is "the blocks in the
; region I asked about".  When DOS=UMB was never given there are none,
; u_total stays at zero, and the row is not printed - a report with an
; empty Upper line would be a report inviting the question.
        xor     ebx,ebx
uv_lp:
        mov     dl,F8_UPPER
        call    get_block
        jc      uv_out
        mov     eax,dword ptr [blk+F8_SIZE]
        cmp     dword ptr [blk+F8_OWNER],0
        je      uv_free
        add     [u_used],eax
        jmp     short uv_next
uv_free:
        add     [u_free],eax
        cmp     eax,[u_largest]
        jbe     uv_next
        mov     [u_largest],eax
uv_next:
        add     dword ptr [u_used],RMCB_SIZE    ; the header, as above
        inc     ebx
        cmp     ebx,BLOCKS_MAX
        jb      uv_lp
uv_out:
        mov     eax,[u_used]
        add     eax,[u_free]
        mov     [u_total],eax
        ret

;---------------------------------------------------------------------
; get_block - EBX = which block, DL = which arena.  Fills [blk].
;             CF when there is no such block.
;
; 21h/F1h AL=8, which exists because a program cannot read either arena
; for itself now that it runs at ring 3 in a segment of its own.  The
; walk is by INDEX rather than by following the chain, so a damaged
; chain stops the kernel's walk rather than spinning this one.
;---------------------------------------------------------------------
get_block:
        push    eax
        push    ebx
        push    edi
        movzx   edx,dl
        mov     edi,OFFSET blk
        mov     ax,0F108h
        int     21h
        pop     edi
        pop     ebx
        pop     eax
        ret

;=====================================================================
; list_dos - the chain a DOS program lives in, block by block, with
;            the region each block is in.
;
; THE ARENA LISTING ABOVE CANNOT ANSWER THE QUESTION LOADHIGH RAISES.
; It walks the kernel's own heap above 2MB, which is where native
; programs live and where nothing a DEVICEHIGH or a LOADHIGH touches
; ever goes.  "Did it load high?" is a question about this chain, and
; before there were two regions in it there was nothing to ask.
;
; Conventional and upper are one chain and the kernel is what knows
; where the join is, so this is two walks of one chain rather than one
; walk with a comparison - see F8_UPPER.  Printing them in order keeps
; the addresses ascending, which is what makes a listing readable.
;=====================================================================
list_dos:
        call    crlf
        mov     edx,OFFSET msg_dhead1
        call    print
        mov     edx,OFFSET msg_dhead2
        call    print

        mov     byte ptr [ld_region],F8_CONV
        call    ld_walk
        mov     byte ptr [ld_region],F8_UPPER
        call    ld_walk
        ret

ld_walk:
        xor     ebx,ebx
ld_lp:
        mov     dl,[ld_region]
        call    get_block
        jc      ld_out
        cmp     dword ptr [blk+F8_SIG],SIG_USED
        jne     ld_show                 ; a free block, which /F wants too
        cmp     byte ptr [free_only],0
        jne     ld_next                 ; /F asked for the free ones only
ld_show:
        push    ebx

        ; the DATA, which is a paragraph past the header and is the
        ; segment the program was handed - the number it would print
        ; if you asked it where it was
        mov     eax,dword ptr [blk+F8_ADDR]
        shr     eax,4
        inc     eax
        call    puthex16
        mov     edx,OFFSET msg_2sp
        call    print

        mov     eax,dword ptr [blk+F8_SIZE]
        mov     ebx,12
        call    pdec_cw
        mov     edx,OFFSET msg_2sp
        call    print

        mov     eax,dword ptr [blk+F8_OWNER]
        or      eax,eax
        jnz     ld_owner
        mov     edx,OFFSET msg_dnoown
        call    print
        jmp     short ld_area
ld_owner:
        mov     ebx,6
        call    pdec_cw
ld_area:
        mov     edx,OFFSET msg_2sp
        call    print
        mov     edx,OFFSET msg_aconv
        cmp     byte ptr [ld_region],F8_CONV
        je      ld_parea
        mov     edx,OFFSET msg_aupper
ld_parea:
        call    print

        cmp     dword ptr [blk+F8_SIG],SIG_USED
        je      ld_pused
        mov     edx,OFFSET msg_isfree
        call    print
        pop     ebx
        jmp     ld_next
ld_pused:
        mov     edx,OFFSET msg_used
        call    print
        pop     ebx
ld_next:
        inc     ebx
        cmp     ebx,BLOCKS_MAX
        jb      ld_lp
ld_out:
        ret

;--- puthex16 - AX, four digits.  A segment is four and printing eight
;--- of them puts four zeroes in front of every line for no reason.
puthex16:
        pushad
        mov     edi,OFFSET hexbuf
        mov     ecx,4
        shl     eax,16
p16_lp:
        rol     eax,4
        mov     dl,al
        and     dl,0Fh
        add     dl,'0'
        cmp     dl,'9'
        jbe     p16_dig
        add     dl,7
p16_dig:
        mov     [edi],dl
        inc     edi
        dec     ecx
        jnz     p16_lp
        mov     byte ptr [edi],'$'
        mov     edx,OFFSET hexbuf
        call    print
        popad
        ret

;=====================================================================
; list_blocks - the arena, block by block.
;
; The owner is the OWNER ID the block was allocated under, not a PSP
; address: PM-DOS tags memory with a monotonic id because a nested
; program shares its parent's PSP address.  Id 1 is the shell, and a
; block owned by 0 is the kernel's own.
;=====================================================================
list_blocks:
        call    crlf
        mov     edx,OFFSET msg_bhead1
        call    print
        mov     edx,OFFSET msg_bhead2
        call    print

        xor     ebx,ebx
lb_lp:
        mov     dl,F8_ARENA
        call    get_block
        jc      lb_out
        mov     eax,dword ptr [blk+F8_SIG]
        cmp     eax,SIG_END
        je      lb_out
        cmp     eax,SIG_USED
        je      lb_used

        push    ebx
        call    lb_common
        mov     edx,OFFSET msg_noowner  ; a free block has no owner
        call    print
        mov     edx,OFFSET msg_isfree
        call    print
        pop     ebx
        jmp     short lb_next
lb_used:
        cmp     byte ptr [free_only],0
        jne     lb_next                 ; /F asked for the free ones only
        push    ebx
        call    lb_common
        mov     eax,dword ptr [blk+F8_OWNER]
        mov     ebx,6
        call    pdec_cw
        mov     edx,OFFSET msg_used
        call    print
        pop     ebx
lb_next:
        inc     ebx
        cmp     ebx,BLOCKS_MAX
        jb      lb_lp
lb_out:
        ret

; lb_common - the address and size columns, which both rows share.
lb_common:
        mov     eax,dword ptr [blk+F8_ADDR]
        add     eax,MCB_LEN             ; the payload, which is what the
        call    puthex32                ; program was actually handed
        mov     edx,OFFSET msg_2sp
        call    print
        mov     eax,dword ptr [blk+F8_SIZE]
        mov     ebx,15
        call    pdec_cw
        mov     edx,OFFSET msg_2sp
        call    print
        ret

;=====================================================================
; list_drv - the 32-bit drivers DEVICE= loaded, one line each.
;
; THEIR MEMORY IS IN THE ARENA LISTING ALREADY, as blocks the kernel
; owns (owner 0), and nothing there says which driver a block is
; for.  So each driver is named here with the two figures the kernel
; keeps for it: the image, which is the block at the same address in
; the arena listing, and what the driver asked for after it was loaded
; - a RAM disk's sectors, a card's buffers, a stack's sockets - which
; is often far the larger.  With no driver loaded nothing is printed,
; as with upper memory.
;=====================================================================
list_drv:
        mov     eax,(NDF_COUNT SHL 16) OR 0F125h
        int     21h
        jc      lv_out                  ; a kernel without the call
        or      ecx,ecx
        jz      lv_out
        mov     [dv_count],ecx

        call    crlf
        mov     edx,OFFSET msg_vhead1
        call    print
        mov     edx,OFFSET msg_vhead2
        call    print

        xor     ebx,ebx
lv_lp:
        cmp     ebx,[dv_count]
        jae     lv_out
        cmp     ebx,256
        jae     lv_out                  ; DL carries the row, and no more
        push    ebx
        mov     edx,ebx
        mov     edi,OFFSET dv_row
        mov     eax,(NDF_INFO SHL 16) OR 0F125h
        int     21h
        jc      lv_gone

        call    lv_name
        mov     edx,OFFSET msg_2sp
        call    print
        mov     eax,dword ptr [dv_row+ND_BASE]
        call    puthex32
        mov     edx,OFFSET msg_2sp
        call    print
        mov     eax,dword ptr [dv_row+ND_SIZE]
        mov     ebx,13
        call    pdec_cw
        mov     edx,OFFSET msg_2sp
        call    print

        mov     edx,[esp]               ; the row again
        mov     eax,(NDF_MEM SHL 16) OR 0F125h
        int     21h
        jc      lv_nomem                ; a kernel from before NDF_MEM
        mov     eax,ecx
        mov     ebx,17
        call    pdec_cw
        jmp     short lv_kind
lv_nomem:
        mov     edx,OFFSET msg_vnomem
        call    print
lv_kind:
        mov     edx,OFFSET msg_2sp
        call    print
        call    lv_type
        call    print
        call    crlf
        pop     ebx
        inc     ebx
        jmp     lv_lp
lv_gone:
        pop     ebx
lv_out:
        ret

;--- lv_name - the row's name, eight characters.  It comes from the
;--- driver's own table, so a control character or a '$' (which would
;--- end the string early for 09h) is shown as '?'.
lv_name:
        pushad
        mov     esi,OFFSET dv_row + ND_NAME
        mov     edi,OFFSET dv_name
        mov     ecx,8
lvn_lp:
        mov     al,[esi]
        cmp     al,' '
        jb      lvn_odd
        cmp     al,'$'
        jne     lvn_put
lvn_odd:
        mov     al,'?'
lvn_put:
        mov     [edi],al
        inc     esi
        inc     edi
        dec     ecx
        jnz     lvn_lp
        mov     byte ptr [edi],'$'
        mov     edx,OFFSET dv_name
        call    print
        popad
        ret

;--- lv_type - EDX -> what kind of driver the row's flags say it is,
;--- the first of these it has.  A driver with no flag the kernel knows
;--- of is "other" (NDTEST.SYS is one).
lv_type:
        mov     eax,dword ptr [dv_row+ND_FLAGS]
        mov     edx,OFFSET msg_kdisk
        test    eax,NDTF_DISK
        jnz     lvt_out
        mov     edx,OFFSET msg_kredir
        test    eax,NDTF_REDIR
        jnz     lvt_out
        mov     edx,OFFSET msg_kstack
        test    eax,NDTF_STACK
        jnz     lvt_out
        mov     edx,OFFSET msg_knic
        test    eax,NDTF_NIC
        jnz     lvt_out
        mov     edx,OFFSET msg_kother
lvt_out:
        ret

;=====================================================================
; Output helpers
;=====================================================================

; print - EDX -> a '$'-terminated string
print:
        push    eax
        mov     ah,09h
        int     21h
        pop     eax
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    print
        pop     edx
        ret

upcase:
        cmp     al,'a'
        jb      uc_out
        cmp     al,'z'
        ja      uc_out
        sub     al,20h
uc_out:
        ret

; col_kb - two spaces, then EAX right-aligned in a NUM_W column
col_kb:
        push    eax
        mov     edx,OFFSET msg_2sp
        call    print
        pop     eax
        mov     ebx,NUM_W
        call    pdec_cw
        ret

; line_kb  - the label at EDX (LINE_W wide), EAX right-aligned in
;            NUM_W, ' KB' and a new line
; line_num - the same without the unit
line_kb:
        call    line_fig
        mov     edx,OFFSET msg_kb
        call    print
        ret
line_num:
        call    line_fig
        call    crlf
        ret
line_fig:
        mov     ebx,LINE_W
        call    print_pad
        mov     ebx,NUM_W
        call    pdec_cw
        ret

; row_label - the table row's name at EDX, padded to LABEL_W
row_label:
        mov     ebx,LABEL_W
                                        ; and on into print_pad
; print_pad - EDX -> a '$'-terminated string, printed and then blanks
;             up to EBX columns.  Everything preserved.
;
; THE LABELS ARE STORED BARE AND PADDED HERE.  They used to carry their
; padding as counted runs of blanks, which kept the columns provable
; and cost nearly 300 bytes of spaces - the cluster that decided
; whether MEM.EXE fitted on the boot diskette.  The column is just as
; provable counted at run time.
print_pad:
        pushad
        call    print
        mov     edi,edx
pp_len:
        cmp     byte ptr [edi],'$'
        je      pp_got
        inc     edi
        jmp     short pp_len
pp_got:
        sub     edi,edx                 ; the label's length
        sub     ebx,edi
        jle     pp_out
        mov     edx,OFFSET msg_1sp
pp_blank:
        call    print
        dec     ebx
        jnz     pp_blank
pp_out:
        popad
        ret

;---------------------------------------------------------------------
; pdec_c  - EAX in decimal, the thousands grouped with num_sep (the
;           country's separator, 38h)
; pdec_cw - the same, right-aligned in a field EBX wide
;
; Built backwards into a buffer, which is the only way to know where
; the commas go before you know how many digits there are.
;---------------------------------------------------------------------
pdec_c:
        mov     ebx,0
pdec_cw:
        pushad
        mov     edi,OFFSET numbuf + 23
        mov     byte ptr [edi],'$'
        xor     ebp,ebp                 ; digits since the last comma
        mov     ecx,10
pc_lp:
        xor     edx,edx
        div     ecx
        dec     edi
        add     dl,'0'
        mov     [edi],dl
        inc     ebp
        or      eax,eax
        jz      pc_pad
        cmp     ebp,3
        jne     pc_lp
        dec     edi
        mov     dl,[num_sep]            ; the digit is stored: DL is free
        mov     [edi],dl
        xor     ebp,ebp
        jmp     short pc_lp

pc_pad:
        ; EBX = field width; pad with blanks on the left
        mov     eax,OFFSET numbuf + 23
        sub     eax,edi                 ; characters produced
        sub     ebx,eax
        jle     pc_say
pc_pad2:
        dec     edi
        mov     byte ptr [edi],' '
        dec     ebx
        jnz     pc_pad2
pc_say:
        mov     edx,edi
        call    print
        popad
        ret

; puthex32 - EAX as eight hex digits
puthex32:
        pushad
        mov     edi,OFFSET hexbuf
        mov     ecx,8
ph_lp:
        rol     eax,4
        mov     dl,al
        and     dl,0Fh
        add     dl,'0'
        cmp     dl,'9'
        jbe     ph_dig
        add     dl,7
ph_dig:
        mov     [edi],dl
        inc     edi
        dec     ecx
        jnz     ph_lp
        mov     byte ptr [edi],'$'
        mov     edx,OFFSET hexbuf
        call    print
        popad
        ret

;=====================================================================
; Data
;=====================================================================
want_list   db  0
free_only   db  0
num_sep     db  ','                     ; the thousands separator, 38h's
            db  0
x_memkb     dd  0
x_osrsv     dd  ARENA_BASE - HMA_BASE   ; OS Reserved: below the arena,
                                        ; and survey adds what is above it
a_total     dd  0
a_used      dd  0
a_free      dd  0
a_largest   dd  0
n_used      dd  0
n_free      dd  0
c_total     dd  0                   ; the conventional arena, below 1M
c_used      dd  0
c_free      dd  0
c_largest   dd  0
u_total     dd  0                   ; upper memory, when DOS=UMB
u_used      dd  0                   ; built any
u_free      dd  0
e_frame     dd  0                   ; expanded memory, when EMS=
e_total     dd  0                   ; found a page frame
e_free      dd  0
u_largest   dd  0
ld_region   db  0
            db  0,0,0
free_only_pad db 0
dv_count    dd  0                   ; 32-bit drivers loaded

;--- the table: see LABEL_W at the top --------------------------------
msg_head1   db  'Memory Type',(LABEL_W - 11 + 2 + NUM_W - 8) dup(' '),'Total KB'
            db  (2 + NUM_W - 7) dup(' '),'Used KB'
            db  (2 + NUM_W - 7) dup(' '),'Free KB',13,10,'$'
msg_head2   db  16 dup('-'),(LABEL_W - 16 + 2) dup(' '),NUM_W dup('-')
            db  2 dup(' '),NUM_W dup('-')
            db  2 dup(' '),NUM_W dup('-'),13,10,'$'

msg_row_sys   db 'OS Reserved','$'
msg_row_conv  db 'Conventional','$'
msg_row_upper db 'Upper','$'
msg_row_arena db 'XMS pool','$'
msg_row_tot   db 'Total memory','$'

msg_xms     db  'Extended memory reported by the BIOS','$'
msg_largest db  'Largest free XMS block','$'
msg_blocks  db  'XMS blocks in use','$'
msg_bfree   db  'XMS blocks free','$'
msg_kb      db  ' KB',13,10,'$'
msg_ems     db  'Expanded memory (EMS)','$'
msg_emsfr   db  'Free expanded memory','$'
msg_emspf   db  'EMS page frame','$'

;--- the block list: 8 hex, 2, 15 for the size, 2, 6 for the owner ---
msg_bhead1  db  'Address',6 dup(' '),'Size (bytes)',3 dup(' '),'Owner'
            db  '  Status',13,10,'$'
msg_bhead2  db  8 dup('-'),2 dup(' '),15 dup('-'),2 dup(' '),6 dup('-')
            db  2 dup(' '),6 dup('-'),13,10,'$'
msg_used    db  '  in use',13,10,'$'
msg_dhead1  db  'Segment',2 dup(' '),'Size (bytes)',2 dup(' '),' Owner'
            db  '  Area          Status',13,10,'$'
msg_dhead2  db  4 dup('-'),5 dup(' '),12 dup('-'),2 dup(' '),6 dup('-')
            db  2 dup(' '),12 dup('-'),2 dup(' '),6 dup('-'),13,10,'$'
;--- the drivers: 8 for the name, 2, 8 hex, 2, 13 and 17 for the two
;--- sizes (a 32-bit count with its commas is 13), 2, 12 for the type
msg_vhead1  db  'Driver',4 dup(' '),'Address',3 dup(' '),'Image (bytes)'
            db  2 dup(' '),'Allocated (bytes)',2 dup(' '),'Type',13,10,'$'
msg_vhead2  db  8 dup('-'),2 dup(' '),8 dup('-'),2 dup(' '),13 dup('-')
            db  2 dup(' '),17 dup('-'),2 dup(' '),12 dup('-'),13,10,'$'
msg_vnomem  db  16 dup(' '),'-','$'
msg_kdisk   db  'disk','$'
msg_kredir  db  'redirector','$'
msg_kstack  db  'TCP/IP stack','$'
msg_knic    db  'network card','$'
msg_kother  db  'other','$'
msg_aconv   db  'conventional','$'
msg_aupper  db  'upper       ','$'
msg_dnoown  db  5 dup(' '),'-','$'
msg_isfree  db  '  free',13,10,'$'
msg_noowner db  5 dup(' '),'-','$'
msg_1sp     db  ' ','$'
msg_2sp     db  '  ','$'
msg_crlf    db  13,10,'$'

msg_badsw   db  'Invalid switch',13,10,'$'
msg_usage   db  'Displays the amount of used and free memory in your system.',13,10,13,10
            db  'MEM [/DEBUG | /FREE]',13,10,13,10
            db  '  /DEBUG or /D     Displays every memory block in both arenas, with',13,10
            db  '                   its owner and status, and the 32-bit drivers.',13,10
            db  '  /FREE or /F      Displays only the free memory blocks.',13,10,'$'

;---------------------------------------------------------------------
; The BSS: buffers the loader zeroes past the image, so that they take
; no room in the file.  MEM_BSS in the header is their length.
;---------------------------------------------------------------------
img_end:
blk         EQU     img_end             ; one block, as F1h AL=8 answers
numbuf      EQU     blk + F8_LEN        ; 24: a number, built backwards
hexbuf      EQU     numbuf + 24         ; 12: eight hex digits and a '$'
dv_row      EQU     hexbuf + 12         ; a 32-bit driver, as NDF_INFO
                                        ; answers
dv_name     EQU     dv_row + ND_LEN     ; 12: its name, '$'-ended
ctry_buf    EQU     dv_name + 12        ; 34: 38h's country block
MEM_BSS     EQU     F8_LEN + 24 + 12 + ND_LEN + 12 + 34

_MEM    ENDS
        END
