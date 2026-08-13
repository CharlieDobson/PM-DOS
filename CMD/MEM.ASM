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
;   21h/52h         the kernel information block, whose dword BELOW
;                   the returned pointer is the first arena header -
;                   the one field real DOS also put there, and the
;                   reason MEM and DEBUG could walk MCBs at all
;
; The block walk itself reads kernel-side headers directly, which is
; exactly what MEM always did and the reason 52h exists at all.
;
; Linked with:  wlink form raw bin option offset=0x181000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

;--- the arena header, as ARENA.INC lays it out ----------------------
MCB_SIG         EQU     0
MCB_SIZE        EQU     4
MCB_OWNER       EQU     8
MCB_LEN         EQU     16
SIG_USED        EQU     'M'
SIG_FREE        EQU     'Z'
SIG_END         EQU     'E'

;--- the 52h block ---------------------------------------------------
DL_MCB          EQU     -4              ; dd - first arena header
DL_ARENATOP     EQU     8               ; dd - -> top of the arena

BLOCKS_MAX      EQU     2000            ; refuse to loop forever on a
                                        ; corrupted chain

_MEM    SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_MEM, DS:_MEM, ES:_MEM, SS:_MEM
        ORG     0

;=====================================================================
; D32 header
;=====================================================================
        D32_HEAD PROG_BASE,<OFFSET mem_entry>,<0>,4096

;=====================================================================
mem_entry:
        mov     esi,PROG_PSP + PSP_TAIL
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
        add     eax,ARENA_BASE - HMA_BASE
        shr     eax,10
        call    col_kb
        mov     eax,[a_used]
        add     eax,[c_used]
        add     eax,ARENA_BASE - HMA_BASE
        shr     eax,10
        call    col_kb
        mov     eax,[a_free]
        add     eax,[c_free]
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

        mov     ah,52h                  ; the arena's first header
        int     21h
        jc      sv_nolol
        mov     eax,[ebx+DL_MCB]
        mov     [a_first],eax
sv_nolol:

        mov     esi,[a_first]
        or      esi,esi
        jz      sv_out
        mov     ecx,BLOCKS_MAX
sv_lp:
        mov     eax,[esi+MCB_SIG]
        cmp     eax,SIG_END
        je      sv_out
        cmp     eax,SIG_USED
        je      sv_used
        cmp     eax,SIG_FREE
        jne     sv_out                  ; not a header at all: stop here

        inc     dword ptr [n_free]
        mov     eax,[esi+MCB_SIZE]
        add     [a_free],eax
        cmp     eax,[a_largest]
        jbe     sv_next
        mov     [a_largest],eax
        jmp     short sv_next
sv_used:
        inc     dword ptr [n_used]
        mov     eax,[esi+MCB_SIZE]
        add     [a_used],eax
sv_next:
        ; the header is overhead, and belongs in "used" either way
        add     dword ptr [a_used],MCB_LEN
        add     esi,[esi+MCB_SIZE]
        add     esi,MCB_LEN
        dec     ecx
        jnz     sv_lp
sv_out:
        mov     eax,[a_used]
        add     eax,[a_free]
        mov     [a_total],eax

;--- and the conventional arena the DOS box runs in -------------------
; A separate chain in a separate place with a separate header format:
; sixteen bytes, 'M' or 'Z', length in paragraphs.  MEM is a 32-bit
; flat program so it can simply read it where it lies - no thunk, no
; V86, no segment arithmetic beyond a shift.
        mov     esi,V86_TPA
        mov     ecx,BLOCKS_MAX
cv_lp:
        mov     dl,[esi+RMCB_SIG]       ; kept, because 'Z' is what ends
        cmp     dl,'M'                  ; the walk and ESI has moved on
        je      cv_ok                   ; by the time we test it
        cmp     dl,'Z'
        jne     cv_out                  ; a broken chain: report what we saw
cv_ok:
        movzx   eax,word ptr [esi+RMCB_LEN]
        shl     eax,4
        cmp     word ptr [esi+RMCB_OWN],0
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
        cmp     dl,'Z'
        je      cv_out
        add     esi,eax
        add     esi,RMCB_SIZE
        cmp     esi,V86_TPA_TOP
        jae     cv_out
        dec     ecx
        jnz     cv_lp
cv_out:
        mov     eax,[c_used]
        add     eax,[c_free]
        mov     [c_total],eax
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

        mov     esi,[a_first]
        or      esi,esi
        jz      lb_out
        mov     ecx,BLOCKS_MAX
lb_lp:
        mov     eax,[esi+MCB_SIG]
        cmp     eax,SIG_END
        je      lb_out
        cmp     eax,SIG_USED
        je      lb_used
        cmp     eax,SIG_FREE
        jne     lb_out                  ; not a header: the chain ends here
        push    ecx
        call    lb_common
        mov     edx,OFFSET msg_noowner  ; a free block has no owner
        call    print
        mov     edx,OFFSET msg_isfree
        call    print
        pop     ecx
        jmp     short lb_next
lb_used:
        cmp     byte ptr [free_only],0
        jne     lb_next                 ; /F asked for the free ones only
        push    ecx
        call    lb_common
        mov     eax,[esi+MCB_OWNER]
        mov     ebx,6
        call    pdec_cw
        mov     edx,OFFSET msg_used
        call    print
        pop     ecx
lb_next:
        add     esi,[esi+MCB_SIZE]
        add     esi,MCB_LEN
        dec     ecx
        jnz     lb_lp
lb_out:
        ret

; lb_common - the address and size columns, which both rows share.
lb_common:
        mov     eax,esi
        add     eax,MCB_LEN             ; the payload, which is what the
        call    puthex32                ; program was actually handed
        mov     edx,OFFSET msg_2sp
        call    print
        mov     eax,[esi+MCB_SIZE]
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
a_first     dd  0
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
msg_isfree  db  '  free',13,10,'$'
msg_noowner db  5 dup(' '),'-','$'
msg_2sp     db  '  ','$'
msg_crlf    db  13,10,'$'

msg_badsw   db  'Invalid switch',13,10,'$'
msg_usage   db  'Displays the amount of used and free memory.',13,10,13,10
            db  '  MEM [/D | /F]',13,10,13,10
            db  '  /D   list every block in the DOS arena',13,10
            db  '  /F   list the free blocks only',13,10,'$'

_MEM    ENDS
        END
