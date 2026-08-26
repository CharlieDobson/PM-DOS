;=====================================================================
; MEM.ASM - MEM.EXE, the memory report, an EXTERNAL command
;
;   MEM             the summary table
;   MEM /D          plus every block in the DOS arena
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
;   21h/F1h AL=4    extended memory size, as the BIOS reported it
;   21h/F1h AL=8    one block of one arena, by index
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

;--- what a block's signature means, as ARENA.INC lays it out --------
MCB_LEN         EQU     16              ; the header, which is overhead
SIG_USED        EQU     'M'
SIG_FREE        EQU     'Z'
SIG_END         EQU     'E'

BLOCKS_MAX      EQU     2000            ; refuse to loop forever on a
                                        ; corrupted chain

_MEM    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MEM, DS:_MEM, ES:_MEM, SS:_MEM
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET mem_entry>,<0>,4096

;=====================================================================
mem_entry:
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
        je      me_usage
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

        ; the kernel and the two program slots: fixed, and all in use
        mov     edx,OFFSET msg_row_sys
        call    print
        mov     eax,ARENA_BASE - HMA_BASE
        shr     eax,10
        push    eax
        call    col_kb                  ; total
        pop     eax
        call    col_kb                  ; used - all of it
        xor     eax,eax
        call    col_kb                  ; free - none
        call    crlf

        ; conventional memory - what a 16-bit DOS program can have
        mov     edx,OFFSET msg_row_conv
        call    print
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
        call    print
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

        ; the arena, as the block chain actually stands
        mov     edx,OFFSET msg_row_arena
        call    print
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
        call    print
        ; The three rows above, added up - and they have to add up, or
        ; the table is worse than no table at all.
        mov     eax,[a_total]
        add     eax,[c_total]
        add     eax,[u_total]
        add     eax,ARENA_BASE - HMA_BASE
        shr     eax,10
        call    col_kb
        mov     eax,[a_used]
        add     eax,[c_used]
        add     eax,[u_used]
        add     eax,ARENA_BASE - HMA_BASE
        shr     eax,10
        call    col_kb
        mov     eax,[a_free]
        add     eax,[c_free]
        add     eax,[u_free]
        shr     eax,10
        call    col_kb
        call    crlf
        call    crlf

        ; The BIOS's own figure, which is not the same number and is
        ; not supposed to be: it counts every byte above 1M, including
        ; the megabyte the kernel and the slots are sitting in.
        mov     edx,OFFSET msg_xms
        call    print
        mov     eax,[x_memkb]
        call    pdec_c
        mov     edx,OFFSET msg_kb
        call    print

        mov     edx,OFFSET msg_largest
        call    print
        mov     eax,[a_largest]
        shr     eax,10
        call    pdec_c
        mov     edx,OFFSET msg_kb
        call    print

        mov     edx,OFFSET msg_blocks
        call    print
        mov     eax,[n_used]
        call    pdec_c
        mov     edx,OFFSET msg_inuse
        call    print
        mov     eax,[n_free]
        call    pdec_c
        mov     edx,OFFSET msg_free
        call    print

        cmp     byte ptr [want_list],0
        je      me_done
        call    list_blocks
        call    list_dos
me_done:
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
        je      ld_used
        cmp     byte ptr [free_only],0
        je      ld_show
        jmp     ld_next                 ; /F wants the free ones...
ld_used:
        cmp     byte ptr [free_only],0
        jne     ld_next                 ; ...and not the others
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

;---------------------------------------------------------------------
; pdec_c  - EAX in decimal with thousands commas
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
        mov     byte ptr [edi],','
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
            db  0,0
x_memkb     dd  0
blk         db  F8_LEN dup(0)       ; one block, as F1h AL=8 answers
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
u_largest   dd  0
ld_region   db  0
            db  0,0,0
free_only_pad db 0
numbuf      db  24 dup(0)
hexbuf      db  12 dup(0)

;---------------------------------------------------------------------
; The table.  Written with counted runs rather than typed spaces so
; that the columns are provably where the numbers land: the label
; field is LABEL_W wide, and col_kb prints two spaces and an 11-wide
; number after it.
;---------------------------------------------------------------------
LABEL_W         EQU     22              ; the row-label field
NUM_W           EQU     11              ; each number column

msg_head1   db  'Memory Type',(LABEL_W - 11 + 2 + NUM_W - 5) dup(' '),'Total'
            db  (2 + NUM_W - 4) dup(' '),'Used'
            db  (2 + NUM_W - 4) dup(' '),'Free',13,10,'$'
msg_head2   db  16 dup('-'),(LABEL_W - 16 + 2) dup(' '),NUM_W dup('-')
            db  2 dup(' '),NUM_W dup('-')
            db  2 dup(' '),NUM_W dup('-'),13,10,'$'

msg_row_sys   db 'Kernel and slots',(LABEL_W - 16) dup(' '),'$'
msg_row_conv  db 'Conventional',(LABEL_W - 12) dup(' '),'$'
msg_row_upper db 'Upper',(LABEL_W - 5) dup(' '),'$'
msg_row_arena db 'DOS arena',(LABEL_W - 9) dup(' '),'$'
msg_row_tot   db 'Total memory',(LABEL_W - 12) dup(' '),'$'

msg_xms     db  13,10,'Extended memory reported by the BIOS: ','$'
msg_largest db  'Largest free arena block:             ','$'
msg_blocks  db  'Arena blocks:                         ','$'
msg_inuse   db  ' in use, ','$'
msg_free    db  ' free',13,10,'$'
msg_kb      db  ' KB',13,10,'$'

;--- the block list: 8 hex, 2, 15 for the size, 2, 6 for the owner ---
msg_bhead1  db  'Address',6 dup(' '),'Size (bytes)',3 dup(' '),'Owner'
            db  '  Status',13,10,'$'
msg_bhead2  db  8 dup('-'),2 dup(' '),15 dup('-'),2 dup(' '),6 dup('-')
            db  2 dup(' '),6 dup('-'),13,10,'$'
msg_used    db  '  in use',13,10,'$'
msg_dhead1  db  'Segment',2 dup(' '),'Size (bytes)',2 dup(' '),' Owner'
            db  '  Area          Status',13,10,'$'
msg_dhead2  db  4 dup('-'),5 dup(' '),12 dup('-'),2 dup(' '),6 dup('-')
            db  2 dup(' '),12 dup('-'),2 dup('-'),13,10,'$'
msg_aconv   db  'conventional','$'
msg_aupper  db  'upper       ','$'
msg_dnoown  db  5 dup(' '),'-','$'
msg_isfree  db  '  free',13,10,'$'
msg_noowner db  5 dup(' '),'-','$'
msg_2sp     db  '  ','$'
msg_crlf    db  13,10,'$'

msg_badsw   db  'Invalid switch',13,10,'$'
msg_usage   db  'Displays the amount of used and free memory.',13,10,13,10
            db  '  MEM [/D | /F]',13,10,13,10
            db  '  /D   list every block, in both arenas',13,10
            db  '  /F   list the free blocks only',13,10,'$'

_MEM    ENDS
        END
