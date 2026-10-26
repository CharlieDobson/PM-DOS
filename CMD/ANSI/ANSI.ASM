;=====================================================================
; ANSI.ASM - ANSI.SYS, the console driver that reads escape sequences
;
; DEVICE=C:\PMDOS\ANSI.SYS [/X] [/K] [/R] puts this in front of the
; console.  What a program writes to the screen through DOS is then
; read for ESC [ sequences - move the cursor, clear, change colour,
; set a video mode, report where the cursor is, give a key a new
; meaning - and everything else is printed as it is, control
; characters included.  It is MS-DOS 6.22's ANSI.SYS again, quirks and
; all, because programs and ANSI pictures were made against that one:
;
;   ESC[r;cH ESC[r;cf   the cursor to row r, column c (from 1).  A row
;                       off the bottom moves nothing; a column past the
;                       edge stops at the edge
;   ESC[nA B C D        up, down, right, left n (1 if none), never off
;                       the screen
;   ESC[s  ESC[u        keep the cursor's place, and go back to it
;   ESC[2J              clear the screen and home the cursor - and so
;                       does every other ESC[nJ, as in DOS's
;   ESC[K               clear to the end of the line, whatever the n
;   ESC[n;...m          colours: 0 normal, 1 bright, 4 underline (blue
;                       on a colour screen), 5 blink, 7 reverse, 8 hidden,
;                       30-37 the letters, 40-47 the background
;   ESC[=nh ESC[=nl     n = 0-6 or 13-19 sets that video mode, whichever
;                       letter ends it; n = 7 turns line wrap on (h) or
;                       off (l).  The = and ? are allowed and ignored
;   ESC[6n              the cursor's place, typed back in as ESC[r;cR
;                       and a CR
;   ESC[k;...p          key k now types the rest: numbers and quoted
;                       strings, "0;59" for F1.  k alone takes a meaning
;                       away again
;   ESC[0q ESC[1q       /X off and on
;
; A final byte no sequence uses is printed, and an ESC followed by
; anything but [ is dropped and the byte printed: both are what DOS's
; driver did, and both show up in pictures made on it.
;
; THE SWITCHES.  /X keeps the grey keys of a 101-key keyboard apart
; from their white twins when keys are given new meanings; /K reads the
; keyboard as an 84-key one; /R sends everything through the video
; BIOS, where a screen reader listens for it.  /S and /SCREENSIZE (the
; BIOS's row count put back to 25) and /L (accepted and ignored, as in
; 6.22) are taken as well, for CONFIG.SYS files that name them.
;
; THE SCREEN IS WRITTEN DIRECTLY in the text modes and through the BIOS
; in the rest, and the BIOS data area is the record of where the cursor
; is: every request starts from it and leaves it right, so a program
; that moves the cursor with INT 10h between two writes, and the
; kernel's own console, agree with this driver.  DOS's driver called
; the BIOS twice for every character; this one calls it twice for every
; request.
;
; THE KEYBOARD is read through INT 16h, with key meanings and the ESC[6n
; report put in front of it.  Under PM-DOS the kernel reads the console
; through this driver only when the driver says a key is there, and
; nothing here may wait in the ROM - a BIOS that halts the processor to
; wait would halt it inside the kernel's call - so a read with nothing
; to read polls instead.
;
; THE BREAK KEY.  Under MS-DOS this driver is the console and marks the
; key itself (INT 1Bh), as DOS's own console did.  PM-DOS keeps that
; mark in the kernel, which reads it before it asks a driver for a
; character, so there the vector is left alone.
;
; ALSO: INT 29h, the fast console write, comes here; INT 2Fh AX=1A00h
; answers FFh, which is how a program asks whether ANSI.SYS is in; and
; the generic IOCTL 440Ch, functions 7Fh and 5Fh, reads and sets the
; display mode - MODE CON LINES= - directly or through INT 2Fh AX=1A01h.
;
; 8086 CODE, so that it runs on any machine MS-DOS does.  The key table,
; the parameters of the sequence being read and the driver's own stack
; are not in the file: they lie over the INIT code, past endres, and
; INIT reports the end of them as the end of the driver.
;
; Linked with:  wlink form raw bin option offset=0 ... order clname CODE
;=====================================================================
        .8086

AN_ESC      EQU 1Bh
KEY_MAX     EQU 512             ; the key table, bytes
PRM_MAX     EQU 240             ; parameter bytes in one sequence
INFO_LEN    EQU 64              ; INT 10h AH=1Bh's state block
STK_LEN     EQU 512             ; the driver's own stack
SW_MAX      EQU 12              ; letters kept of a switch's name

; past endres, and none of it in the file
BSS_KEY     EQU 0
BSS_PRM     EQU BSS_KEY + KEY_MAX
BSS_INFO    EQU BSS_PRM + PRM_MAX + 2
BSS_STK     EQU BSS_INFO + INFO_LEN + STK_LEN
BSS_LEN     EQU BSS_STK

; the request packet
RQ_CMD      EQU 02h
RQ_STAT     EQU 03h
RQ_UNITS    EQU 0Dh             ; INIT
RQ_END      EQU 0Eh             ; INIT: the first byte not kept
RQ_ARG      EQU 12h             ; INIT: the DEVICE= line
RQ_ERRMSG   EQU 17h             ; INIT: FFFFh for "Error in CONFIG.SYS"
RQ_MEDIA    EQU 0Dh             ; non-destructive read: the byte
RQ_BUF      EQU 0Eh             ; read and write
RQ_COUNT    EQU 12h
RQ_GMINOR   EQU 0Eh             ; generic IOCTL: the function
RQ_GPKT     EQU 13h             ; and its parameter block

ST_DONE     EQU 0100h
ST_BUSY     EQU 0200h
ST_ERRDONE  EQU 8100h           ; error, done; the low byte says which
ST_UNKNOWN  EQU 8103h           ; "unknown command"
CMD_COUNT   EQU 20              ; 0 to 13h

; the generic IOCTL's errors
GE_FUNC     EQU 1               ; a block this driver cannot read
GE_UNSUP    EQU 10              ; this adapter cannot do that

; the video BIOS's data, at 40:0
B_MODE      EQU 49h
B_COLS      EQU 4Ah
B_PGOFF     EQU 4Eh
B_CURS      EQU 50h
B_PAGE      EQU 62h
B_ROWS      EQU 84h
B_KBF3      EQU 96h

; adapters, one bit each so that a mode table row can list several
VC_CGA      EQU 01h
VC_MDA      EQU 02h
VC_EGAC     EQU 04h             ; an EGA on a colour screen
VC_EGAM     EQU 08h             ; ...and on a mono one
VC_MCGA     EQU 10h
VC_VGA      EQU 20h
VC_EGAUP    EQU VC_EGAC OR VC_EGAM OR VC_MCGA OR VC_VGA

; a mode table row
MT_MODE     EQU 0
MT_KIND     EQU 1               ; 1 text, 2 graphics
MT_COLORS   EQU 2
MT_WIDE     EQU 4               ; pixels, FFFFh in text
MT_HIGH     EQU 6
MT_COLS     EQU 8
MT_ROWS     EQU 10              ; FFFFh: as many as the BIOS has set
MT_CARDS    EQU 12              ; VC_* bits that have the mode
MT_LEN      EQU 13

_AN     SEGMENT WORD PUBLIC 'CODE'
        ASSUME  CS:_AN, DS:_AN, ES:NOTHING, SS:NOTHING
        ORG     0

;--- the device header: the console, input and output, the fast write
;--- through INT 29h, IOCTL and the generic IOCTL
header:
        dd      0FFFFFFFFh
        dw      0C053h
        dw      OFFSET strat
        dw      OFFSET intr
        db      'CON     '

;=====================================================================
; RESIDENT DATA, here at the top because wasm settles a symbol's type
; at its first mention and a forward reference gets it guessed
;=====================================================================
reqptr      dd  0
vstate      dw  OFFSET s_norm           ; where the next byte goes
prm_i       dw  0                       ; the parameter being filled
key_used    dw  0                       ; table bytes in use, before the 0
pend_ptr    dw  0                       ; input owed: a key's meaning or
pend_cnt    dw  0                       ; the ESC[6n report
sv_cur      dw  0                       ; ESC[s: column, row
sv_ss       dw  0
sv_sp       dw  0
p_key       dw  0                       ; where the areas past endres are
p_prm       dw  0
p_info      dw  0
p_stk       dw  0
old2f       dd  0
inq         db  0                       ; the quote a string opened with
attr        db  07h
wrapoff     db  0                       ; ESC[7l: the cursor waits at the edge
altah       db  0                       ; the second byte of an extended key
sw_x        db  0
sw_k        db  0
sw_r        db  0
sw_s        db  0
ext16       db  0                       ; INT 16h AH=10h and 11h are there
pmdos       db  0
vclass      db  0                       ; VC_*
intens      db  0                       ; what 5Fh last set, for an EGA
in_drv      db  0                       ; calls into this driver, nested
ini_done    db  0
i29_ch      db  0                       ; INT 29h's character
gs_want     db  0                       ; 5Fh: the intensity bit asked for

; the screen, as the BIOS data area had it at the start of this request
v_col       db  0                       ; column then row: one word, as
v_row       db  0                       ; INT 10h AH=02h takes them in DX
v_mode      db  0
v_page      db  0
v_text      db  0                       ; 1: write video memory directly
v_gfx       db  0                       ; 1: a mode with no attribute bytes
v_maxcol    db  0
            db  0
v_cols      dw  0
v_rows      dw  0
v_pgoff     dw  0
v_seg       dw  0
v_crtc      dw  0

report      db  AN_ESC,'[000;000R',13   ; ESC[6n's answer, built in place

;--- what each request is
cmdtab      dw  OFFSET c_init           ; 00  (c_unk once INIT has run)
            dw  OFFSET c_done           ; 01  media check
            dw  OFFSET c_done           ; 02  build BPB
            dw  OFFSET c_unk            ; 03  IOCTL input
            dw  OFFSET c_read           ; 04
            dw  OFFSET c_peek           ; 05  non-destructive read
            dw  OFFSET c_done           ; 06  input status: always ready
            dw  OFFSET c_flush          ; 07
            dw  OFFSET c_write          ; 08
            dw  OFFSET c_write          ; 09  write with verify
            dw  OFFSET c_done           ; 0A  output status
            dw  OFFSET c_unk            ; 0B
            dw  OFFSET c_unk            ; 0C  IOCTL output
            dw  OFFSET c_unk            ; 0D
            dw  OFFSET c_unk            ; 0E
            dw  OFFSET c_unk            ; 0F
            dw  OFFSET c_unk            ; 10
            dw  OFFSET c_unk            ; 11
            dw  OFFSET c_unk            ; 12
            dw  OFFSET c_gen            ; 13  generic IOCTL

;--- the final bytes of a sequence, and what each does
seqtab      db  'A'
            dw  OFFSET a_cuu
            db  'B'
            dw  OFFSET a_cud
            db  'C'
            dw  OFFSET a_cuf
            db  'D'
            dw  OFFSET a_cub
            db  'H'
            dw  OFFSET a_cup
            db  'J'
            dw  OFFSET a_ed
            db  'K'
            dw  OFFSET a_el
            db  'R'
            dw  OFFSET a_nop            ; a report coming back: nothing
            db  'f'
            dw  OFFSET a_cup
            db  'h'
            dw  OFFSET a_sm
            db  'l'
            dw  OFFSET a_rm
            db  'm'
            dw  OFFSET a_sgr
            db  'n'
            dw  OFFSET a_dsr
            db  'p'
            dw  OFFSET a_key
            db  'q'
            dw  OFFSET a_extkey
            db  's'
            dw  OFFSET a_scp
            db  'u'
            dw  OFFSET a_rcp
            db  0

;--- ESC[n m: the number, and what it does to the attribute - ANDed
;--- with the first mask, then ORed with the second.  The ANSI order of
;--- the colours (red, green, yellow, blue...) is not the adapter's
;--- (blue, green, cyan, red...), hence the second column
sgrtab      db  0,  00h, 07h
            db  1,  0FFh, 08h
            db  4,  0F8h, 01h
            db  5,  0FFh, 80h
            db  7,  0F8h, 70h
            db  8,  88h, 00h
            db  30, 0F8h, 00h
            db  31, 0F8h, 04h
            db  32, 0F8h, 02h
            db  33, 0F8h, 06h
            db  34, 0F8h, 01h
            db  35, 0F8h, 05h
            db  36, 0F8h, 03h
            db  37, 0F8h, 07h
            db  40, 8Fh, 00h
            db  41, 8Fh, 40h
            db  42, 8Fh, 20h
            db  43, 8Fh, 60h
            db  44, 8Fh, 10h
            db  45, 8Fh, 50h
            db  46, 8Fh, 30h
            db  47, 8Fh, 70h
            db  0FFh

;--- the display modes the generic IOCTL knows: mode, kind, colours,
;--- pixels across and down, columns, rows, and which adapters have it.
;--- The 40- and 80-column colour modes come before their grey twins so
;--- that a request that names only a size gets the colour one.
modetab     db  1, 1
            dw  16, 0FFFFh, 0FFFFh, 40, 0FFFFh
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  0, 1
            dw  16, 0FFFFh, 0FFFFh, 40, 0FFFFh
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  3, 1
            dw  16, 0FFFFh, 0FFFFh, 80, 0FFFFh
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  2, 1
            dw  16, 0FFFFh, 0FFFFh, 80, 0FFFFh
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  7, 1
            dw  0, 0FFFFh, 0FFFFh, 80, 0FFFFh
            db  VC_MDA OR VC_EGAM OR VC_VGA
            db  4, 2
            dw  4, 320, 200, 40, 25
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  5, 2
            dw  2, 320, 200, 40, 25
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  6, 2
            dw  2, 640, 200, 80, 25
            db  VC_CGA OR VC_EGAC OR VC_MCGA OR VC_VGA
            db  0Dh, 2
            dw  16, 320, 200, 40, 25
            db  VC_EGAC OR VC_VGA
            db  0Eh, 2
            dw  16, 640, 200, 80, 25
            db  VC_EGAC OR VC_VGA
            db  0Fh, 2
            dw  0, 640, 350, 80, 25
            db  VC_EGAM OR VC_VGA
            db  10h, 2
            dw  16, 640, 350, 80, 25
            db  VC_EGAC OR VC_VGA
            db  11h, 2
            dw  2, 640, 480, 80, 30
            db  VC_MCGA OR VC_VGA
            db  12h, 2
            dw  16, 640, 480, 80, 30
            db  VC_VGA
            db  13h, 2
            dw  256, 320, 200, 40, 25
            db  VC_MCGA OR VC_VGA
modetab_end:

;=====================================================================
; strategy - ES:BX is the request packet.  Keep it.
;=====================================================================
strat:
        mov     word ptr cs:[reqptr],bx
        mov     word ptr cs:[reqptr+2],es
        retf

;=====================================================================
; interrupt - do the request.
;
; ON THIS DRIVER'S OWN STACK, because the caller's may be a few hundred
; bytes - PM-DOS gives a driver 256 - and a mode set through the video
; BIOS can take most of that.  Not for INIT, whose code that stack lies
; over, and not for a call that arrives inside another one, which is
; already on it.
;=====================================================================
intr:
        pushf
        cli
        push    ax                      ; on the caller's stack
        xor     ax,ax                   ; AX = 0: stay on it
        cmp     byte ptr cs:[in_drv],0
        jne     in_go
        cmp     byte ptr cs:[ini_done],0
        je      in_go
        mov     word ptr cs:[sv_ss],ss
        mov     word ptr cs:[sv_sp],sp
        mov     ax,cs
        mov     ss,ax
        mov     sp,word ptr cs:[p_stk]
        mov     ax,1
in_go:
        push    ax                      ; whether to go back to the other
        inc     byte ptr cs:[in_drv]
        sti
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        push    cs
        pop     ds
        cld

        les     bx,dword ptr [reqptr]
        mov     al,es:[bx+RQ_CMD]
        cmp     al,CMD_COUNT
        jae     in_unk
        xor     ah,ah
        mov     si,ax
        shl     si,1
        call    word ptr cmdtab[si]     ; -> AX = the status
        jmp     short in_stat
in_unk:
        mov     ax,ST_UNKNOWN
in_stat:
        les     bx,dword ptr [reqptr]
        mov     es:[bx+RQ_STAT],ax

        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        cli
        dec     byte ptr cs:[in_drv]
        pop     ax
        or      ax,ax
        jz      in_back
        mov     ss,word ptr cs:[sv_ss]
        mov     sp,word ptr cs:[sv_sp]
in_back:
        pop     ax
        popf
        retf

c_done:
        mov     ax,ST_DONE
        ret
c_unk:
        mov     ax,ST_UNKNOWN
        ret

;=====================================================================
; WRITE - every byte through the escape reader, the screen as the BIOS
; data area has it before the first and the cursor put back after the
; last.
;=====================================================================
c_write:
        les     bx,dword ptr [reqptr]
        mov     cx,es:[bx+RQ_COUNT]
        jcxz    cw_out
        les     si,dword ptr es:[bx+RQ_BUF]
        call    vsync
cw_lp:
        mov     al,es:[si]
        inc     si
        push    cx
        push    si
        push    es
        call    vchar
        pop     es
        pop     si
        pop     cx
        loop    cw_lp
        call    vpub
cw_out:
        mov     ax,ST_DONE
        ret

;=====================================================================
; INT 29h - the fast console write: AL to the screen, every register
; kept.  DOS uses it for the console when the header says it may, and
; programs use it to write past their own redirection.
;=====================================================================
i29:
        pushf
        cli
        mov     byte ptr cs:[i29_ch],al ; before AX is needed for the flag
        push    ax
        xor     ax,ax
        cmp     byte ptr cs:[in_drv],0
        jne     i29_go
        cmp     byte ptr cs:[ini_done],0
        je      i29_go
        mov     word ptr cs:[sv_ss],ss
        mov     word ptr cs:[sv_sp],sp
        mov     ax,cs
        mov     ss,ax
        mov     sp,word ptr cs:[p_stk]
        mov     ax,1
i29_go:
        push    ax
        inc     byte ptr cs:[in_drv]
        mov     al,byte ptr cs:[i29_ch]
        sti
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        push    cs
        pop     ds
        cld
        push    ax
        call    vsync
        pop     ax
        call    vchar
        call    vpub
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        cli
        dec     byte ptr cs:[in_drv]
        pop     ax
        or      ax,ax
        jz      i29_back
        mov     ss,word ptr cs:[sv_ss]
        mov     sp,word ptr cs:[sv_sp]
i29_back:
        pop     ax
        popf
        iret

;=====================================================================
; vchar - AL through the escape reader.  DS = this segment.  Anything
;         but DS, SS and SP may change.
;
; The reader is a state: what to do with the next byte.  s_norm prints,
; s_esc has seen an ESC, s_parm is inside ESC[ collecting numbers, and
; s_quote is inside a quoted string, whose characters are parameters
; too - one each, which is how a key is given a word to type.
;=====================================================================
vchar:
        jmp     word ptr [vstate]

s_norm:
        cmp     al,AN_ESC
        jne     put_char
        mov     word ptr [vstate],OFFSET s_esc
        ret

s_esc:
        cmp     al,'['
        je      se_csi
        mov     word ptr [vstate],OFFSET s_norm
        jmp     short s_norm
se_csi:
        mov     word ptr [vstate],OFFSET s_parm
        mov     word ptr [prm_i],0
        mov     bx,[p_prm]
        mov     word ptr [bx],0         ; the first and the one after it
        ret

s_parm:
        cmp     al,';'
        je      prm_new
        cmp     al,'0'
        jb      sp_other
        cmp     al,'9'
        ja      sp_other
        ; ONE BYTE A NUMBER, and what passes 255 wraps, as it did in
        ; DOS's driver.  No sequence wants a bigger one.
        sub     al,'0'
        mov     bx,[p_prm]
        add     bx,[prm_i]
        xchg    al,[bx]
        mov     ah,10
        mul     ah
        add     [bx],al
        ret
sp_other:
        cmp     al,'='
        je      sp_ret
        cmp     al,'?'
        je      sp_ret
        cmp     al,'"'
        je      sp_quote
        cmp     al,27h
        je      sp_quote
        jmp     do_seq
sp_quote:
        mov     [inq],al
        mov     word ptr [vstate],OFFSET s_quote
sp_ret:
        ret

s_quote:
        cmp     al,[inq]
        je      sq_end
        mov     bx,[p_prm]
        add     bx,[prm_i]
        mov     [bx],al
        jmp     short prm_new
sq_end:
        ; Every character took a slot and opened the next; the closing
        ; quote gives the last one back.  An empty string gives back the
        ; one the ; before it opened, so it vanishes - but never past
        ; the first.
        cmp     word ptr [prm_i],0
        je      sq_state
        dec     word ptr [prm_i]
sq_state:
        mov     word ptr [vstate],OFFSET s_parm
        ret

;--- prm_new - the next parameter, at zero, and the one after it too.
;--- A sequence longer than the room keeps writing its last one.
prm_new:
        mov     bx,[prm_i]
        cmp     bx,PRM_MAX - 1
        jae     pn_full
        inc     bx
        mov     [prm_i],bx
pn_full:
        add     bx,[p_prm]
        mov     word ptr [bx],0
        ret

;---------------------------------------------------------------------
; do_seq - AL is the byte that ends a sequence.  CX = the first number,
;          or 1 when it is 0 or missing, which is what the movements
;          count by; DL = the first number as it is.
;---------------------------------------------------------------------
do_seq:
        mov     word ptr [vstate],OFFSET s_norm
        mov     bx,OFFSET seqtab
ds_lp:
        mov     ah,[bx]
        or      ah,ah
        jz      ds_none
        cmp     ah,al
        je      ds_hit
        add     bx,3
        jmp     short ds_lp
ds_none:
        jmp     put_char                ; no such sequence: the byte prints
ds_hit:
        mov     si,[p_prm]
        mov     dl,[si]
        xor     dh,dh
        mov     cx,dx
        or      cx,cx
        jnz     ds_go
        inc     cx
ds_go:
        jmp     word ptr [bx+1]

a_nop:
        ret

;--- up, down, right, left: CX steps, stopping at the edge ------------
a_cuu:
        mov     al,[v_row]
        xor     ah,ah
        sub     ax,cx
        jnb     acu_set
        xor     ax,ax
acu_set:
        mov     [v_row],al
        ret

a_cud:
        mov     al,[v_row]
        xor     ah,ah
        add     ax,cx
        mov     dx,[v_rows]
        dec     dx
        cmp     ax,dx
        jbe     acd_set
        mov     ax,dx
acd_set:
        mov     [v_row],al
        ret

a_cuf:
        mov     al,[v_col]
        xor     ah,ah
        add     ax,cx
        mov     dl,[v_maxcol]
        xor     dh,dh
        cmp     ax,dx
        jbe     acf_set
        mov     ax,dx
acf_set:
        mov     [v_col],al
        ret

a_cub:
        mov     al,[v_col]
        xor     ah,ah
        sub     ax,cx
        jnb     acb_set
        xor     ax,ax
acb_set:
        mov     [v_col],al
        ret

;--- ESC[r;cH: a row past the bottom is ignored altogether, a column
;--- past the edge is the edge - DOS's two rules, not one
a_cup:
        cmp     cx,[v_rows]
        ja      acp_out
        mov     ax,cx
        dec     ax
        mov     [v_row],al
        mov     al,[si+1]               ; the column, 0 being 1
        or      al,al
        jz      acp_col
        dec     al
acp_col:
        cmp     al,[v_maxcol]
        jbe     acp_set
        mov     al,[v_maxcol]
acp_set:
        mov     [v_col],al
acp_out:
        ret

a_scp:
        mov     ax,word ptr [v_col]
        mov     [sv_cur],ax
        ret

a_rcp:
        mov     ax,[sv_cur]
        mov     word ptr [v_col],ax
        jmp     cur_clamp               ; the screen may be smaller now

a_ed:
        mov     word ptr [v_col],0
        jmp     clr_screen

a_el:
        jmp     clr_eol

;--- ESC[...m: each number in turn, through sgrtab ---------------------
a_sgr:
        mov     cx,[prm_i]
        inc     cx
asg_lp:
        lodsb
        mov     bx,OFFSET sgrtab
asg_find:
        mov     ah,[bx]
        cmp     ah,0FFh
        je      asg_next
        cmp     ah,al
        je      asg_hit
        add     bx,3
        jmp     short asg_find
asg_hit:
        mov     al,[attr]
        and     al,[bx+1]
        or      al,[bx+2]
        mov     [attr],al
asg_next:
        loop    asg_lp
        ret

;--- ESC[6n, and any other number before n: the report, owed as input
a_dsr:
        push    ds
        pop     es
        mov     di,OFFSET report
        mov     ax,AN_ESC + ('[' SHL 8)
        stosw
        mov     al,[v_row]
        call    rep_num
        mov     al,';'
        stosb
        mov     al,[v_col]
        call    rep_num
        mov     ax,'R' + (13 SHL 8)
        stosw
        mov     ax,di
        sub     ax,OFFSET report
        mov     [pend_cnt],ax
        mov     word ptr [pend_ptr],OFFSET report
        ret

;--- rep_num - AL counted from 0, at ES:DI counted from 1: two digits,
;--- or three past 99
rep_num:
        xor     ah,ah
        inc     ax
        cmp     ax,100
        jb      rn_two
        mov     bl,100
        div     bl
        add     al,'0'
        stosb
        mov     al,ah
        xor     ah,ah
rn_two:
        mov     bl,10
        div     bl
        add     ax,'00'
        stosw
        ret

;--- ESC[=nh and ESC[=nl: a video mode either way, or line wrap --------
a_sm:
        xor     cl,cl
        jmp     short asm_go
a_rm:
        mov     cl,1
asm_go:
        mov     al,dl
        cmp     al,7
        je      asm_wrap
        jb      asm_mode
        cmp     al,13
        jb      asm_out
        cmp     al,19
        ja      asm_out
asm_mode:
        xor     ah,ah
        int     10h
        jmp     vsync                   ; a different screen now
asm_wrap:
        mov     [wrapoff],cl
asm_out:
        ret

;--- ESC[0q and ESC[1q: /X off and on ---------------------------------
a_extkey:
        cmp     dl,1
        ja      aek_out
        mov     [sw_x],dl
aek_out:
        ret

;=====================================================================
; put_char - AL onto the screen at the cursor.  CR, LF, BS and BEL do
;            what they do; every other byte is a character, the control
;            characters' shapes included, as DOS's driver had it - DOS
;            itself turns a TAB into spaces before it gets here.
;=====================================================================
put_char:
        cmp     al,13
        je      pc_cr
        cmp     al,10
        je      pc_lf
        cmp     al,8
        je      pc_bs
        cmp     al,7
        je      pc_bel
        call    cell_put
        mov     al,[v_col]
        cmp     al,[v_maxcol]
        jae     pc_edge
        inc     al
        mov     [v_col],al
        ret
pc_edge:
        ; PAST THE LAST COLUMN THE LINE ENDS AT ONCE, scrolling if it
        ; was the last line: an 80-character line moves the cursor to
        ; the next, as pictures made for DOS's driver expect.  With
        ; ESC[7l the cursor stays and the next character overwrites.
        cmp     byte ptr [wrapoff],0
        jne     pc_out
        mov     byte ptr [v_col],0
        jmp     short pc_lf
pc_cr:
        mov     byte ptr [v_col],0
pc_out:
        ret
pc_bs:
        cmp     byte ptr [v_col],0
        je      pc_out
        dec     byte ptr [v_col]
        ret
pc_bel:
        mov     ax,0E07h                ; the BIOS's teletype beeps
        mov     bh,[v_page]
        mov     bl,[attr]
        and     bl,7
        int     10h
        ret
pc_lf:
        mov     al,[v_row]
        xor     ah,ah
        inc     ax
        cmp     ax,[v_rows]
        jae     scroll_up               ; the last row: it stays the last
        mov     [v_row],al
        ret

;---------------------------------------------------------------------
; cell_put - AL at the cursor, in the current attribute
;---------------------------------------------------------------------
cell_put:
        cmp     byte ptr [v_text],0
        je      cp_bios
        call    cell_addr
        mov     ah,[attr]
        stosw
        ret
cp_bios:
        push    ax
        call    bios_cur
        pop     ax
        mov     ah,09h
        mov     bh,[v_page]
        mov     bl,[attr]
        mov     cx,1
        int     10h
        ret

;--- cell_addr - ES:DI = the cursor's cell in video memory.  AX kept.
cell_addr:
        push    ax
        push    dx
        mov     al,[v_row]
        xor     ah,ah
        mul     word ptr [v_cols]
        mov     dl,[v_col]
        xor     dh,dh
        add     ax,dx
        shl     ax,1
        add     ax,[v_pgoff]
        mov     di,ax
        mov     es,[v_seg]
        pop     dx
        pop     ax
        ret

;---------------------------------------------------------------------
; scroll_up - the page up one row, the last row blanked in the current
;             attribute.  In the BIOS's modes it is the BIOS's line feed
;             from the last row, which is how DOS's driver scrolled them.
;---------------------------------------------------------------------
scroll_up:
        cmp     byte ptr [v_text],0
        je      su_bios
        mov     ax,[v_rows]
        dec     ax
        mul     word ptr [v_cols]
        mov     cx,ax                   ; the words that move
        mov     bx,[v_cols]             ; and the words in a row
        mov     di,[v_pgoff]
        mov     si,di
        add     si,bx
        add     si,bx
        mov     ah,[attr]
        mov     al,' '
        mov     dx,ax
        mov     ax,[v_seg]
        mov     es,ax
        push    ds
        mov     ds,ax
        rep     movsw
        mov     cx,bx
        mov     ax,dx
        rep     stosw
        pop     ds
        ret
su_bios:
        mov     dh,byte ptr [v_rows]
        dec     dh
        mov     dl,[v_col]
        mov     bh,[v_page]
        mov     ah,02h
        int     10h
        mov     ax,0E0Ah
        mov     bh,[v_page]
        mov     bl,[attr]
        and     bl,7
        int     10h
        ret

;---------------------------------------------------------------------
; clr_screen / clr_eol - the page, or the cursor to the end of its row,
;             blanked in the current attribute (in the BIOS's modes, in
;             its colour 0, which is what a graphics screen blanks to)
;---------------------------------------------------------------------
clr_screen:
        cmp     byte ptr [v_text],0
        je      cs_bios
        mov     ax,[v_cols]
        mul     word ptr [v_rows]
        mov     cx,ax
        mov     di,[v_pgoff]
        mov     es,[v_seg]
        mov     ah,[attr]
        mov     al,' '
        rep     stosw
        ret
cs_bios:
        xor     cx,cx
        mov     dh,byte ptr [v_rows]
        dec     dh
        jmp     short ce_bios

clr_eol:
        cmp     byte ptr [v_text],0
        je      ce_rowbios
        mov     al,[v_maxcol]
        sub     al,[v_col]
        xor     ah,ah
        inc     ax
        mov     cx,ax
        call    cell_addr
        mov     ah,[attr]
        mov     al,' '
        rep     stosw
        ret
ce_rowbios:
        mov     cx,word ptr [v_col]     ; CL = column, CH = row
        mov     dh,ch
ce_bios:
        mov     dl,[v_maxcol]
        mov     bh,[attr]
        cmp     byte ptr [v_gfx],0
        je      ce_attr
        xor     bh,bh
ce_attr:
        mov     ax,0600h
        int     10h
        ret

;=====================================================================
; vsync - the screen as the BIOS data area has it NOW: mode, size,
;         page, and where that page's cursor is.
;
; CHECKED RATHER THAN BELIEVED, as the kernel's console checks it: a
; program can write any of these, and a nonsense column count would
; send the writes off the end of the adapter's memory and into its
; ROM's shadow.  The page has to fit in the 32K a text mode has.
;=====================================================================
vsync:
        push    es
        mov     ax,40h
        mov     es,ax
        mov     al,es:[B_MODE]
        and     al,7Fh
        mov     [v_mode],al
        mov     ax,es:[B_COLS]
        or      ah,ah
        jnz     vs_c80
        or      al,al
        jnz     vs_cols
vs_c80:
        mov     ax,80
vs_cols:
        mov     [v_cols],ax
        mov     al,es:[B_ROWS]          ; one short of the rows, and 0
        xor     ah,ah                   ; on a BIOS that never sets it
        or      al,al
        jz      vs_r25
        cmp     al,0FFh
        je      vs_r25
        inc     ax
        jmp     short vs_rows
vs_r25:
        mov     ax,25
vs_rows:
        mov     [v_rows],ax
        mov     al,es:[B_PAGE]
        and     al,7
        mov     [v_page],al
        mov     ax,es:[B_PGOFF]
        mov     [v_pgoff],ax
        mov     bl,[v_page]
        xor     bh,bh
        shl     bx,1
        mov     ax,es:[bx+B_CURS]
        mov     word ptr [v_col],ax
        pop     es

        ; which memory, and whether it can be written as cells
        mov     word ptr [v_seg],0B800h
        mov     word ptr [v_crtc],3D4h
        mov     byte ptr [v_gfx],1
        mov     al,[v_mode]
        cmp     al,7
        jne     vs_col
        mov     word ptr [v_seg],0B000h
        mov     word ptr [v_crtc],3B4h
        jmp     short vs_istext
vs_col:
        cmp     al,3
        ja      vs_kind
vs_istext:
        mov     byte ptr [v_gfx],0
vs_kind:
        mov     al,[v_gfx]
        xor     al,1
        cmp     byte ptr [sw_r],0
        je      vs_txt
        xor     al,al                   ; /R: all of it through the BIOS
vs_txt:
        mov     [v_text],al

        ; and the page has to fit
        mov     ax,[v_cols]
        mul     word ptr [v_rows]
        shl     ax,1
        rcl     dx,1
        or      dx,dx
        jnz     vs_bad
        cmp     ax,8000h
        ja      vs_bad
        add     ax,[v_pgoff]
        jc      vs_pg0
        cmp     ax,8000h
        jbe     vs_ok
vs_pg0:
        mov     word ptr [v_pgoff],0
        jmp     short vs_ok
vs_bad:
        mov     word ptr [v_cols],80
        mov     word ptr [v_rows],25
        mov     word ptr [v_pgoff],0
vs_ok:
        mov     al,byte ptr [v_cols]
        dec     al
        mov     [v_maxcol],al
        ; fall through

;--- cur_clamp - the cursor onto the screen
cur_clamp:
        mov     al,[v_row]
        xor     ah,ah
        cmp     ax,[v_rows]
        jb      cc_row
        mov     al,byte ptr [v_rows]
        dec     al
        mov     [v_row],al
cc_row:
        mov     al,[v_col]
        cmp     al,[v_maxcol]
        jbe     cc_col
        mov     al,[v_maxcol]
        mov     [v_col],al
cc_col:
        ret

;=====================================================================
; vpub - the cursor back where everybody looks for it: the BIOS data
;        area, and the CRT controller, which counts cells from the start
;        of the adapter's memory and not from the start of the page.
;        In the BIOS's modes, the BIOS does both.
;=====================================================================
vpub:
        cmp     byte ptr [v_text],0
        je      bios_cur
        push    es
        mov     ax,40h
        mov     es,ax
        mov     bl,[v_page]
        xor     bh,bh
        shl     bx,1
        mov     ax,word ptr [v_col]
        mov     es:[bx+B_CURS],ax
        pop     es
        mov     al,[v_row]
        xor     ah,ah
        mul     word ptr [v_cols]
        mov     bl,[v_col]
        xor     bh,bh
        add     ax,bx
        mov     bx,[v_pgoff]
        shr     bx,1
        add     bx,ax
        mov     dx,[v_crtc]
        mov     al,0Eh
        out     dx,al
        inc     dx
        mov     al,bh
        out     dx,al
        dec     dx
        mov     al,0Fh
        out     dx,al
        inc     dx
        mov     al,bl
        out     dx,al
        ret

bios_cur:
        mov     ah,02h
        mov     bh,[v_page]
        mov     dx,word ptr [v_col]
        int     10h
        ret

;=====================================================================
; THE KEYBOARD
;
; What a READ hands out, in order: the second byte of an extended key
; already half read; the rest of a key's new meaning, or of the ESC[6n
; report; and then the BIOS's next key, looked up in the table first.
;
; A KEY IN THE TABLE is a row: its length (counting itself), the key -
; one byte for a character, two (0 and the scan code) for a key that
; has none - and the bytes it types instead.  A 0 in those bytes is an
; extended key too, with the scan code after it.
;
; THE GREY KEYS of a 101-key keyboard come from the BIOS as E0h and
; the scan code.  Without /X they are taken for their white twins
; before anything else happens; with /X they are looked up as
; themselves (ESC[224;72p) and only then folded, so a program reads
; them as DOS has always delivered them.
;=====================================================================

;--- kb_ask - ZF clear and AX = the BIOS's next key, left there; ZF
;--- set if there is none
kb_ask:
        mov     ah,01h
        cmp     byte ptr [ext16],0
        je      kba_go
        mov     ah,11h
kba_go:
        int     16h
        ret

;--- kb_take - the BIOS's next key -> AX.  Under PM-DOS it is asked for
;--- only once one is known to be there, and is waited for here if not.
kb_take:
        cmp     byte ptr [pmdos],0
        je      kbt_read
kbt_poll:
        call    kb_ask
        jz      kbt_poll
kbt_read:
        mov     ah,00h
        cmp     byte ptr [ext16],0
        je      kbt_go
        mov     ah,10h
kbt_go:
        int     16h
        ret

;--- kb_cook - AX = a key as the BIOS gave it -> as it is looked up.
;--- Ctrl+PrtSc is ^P, as DOS's own console has it.
kb_cook:
        cmp     ax,7200h
        jne     kbc_x
        mov     al,10h
        ret
kbc_x:
        cmp     byte ptr [sw_x],0
        jne     kbc_out
kb_fold:
        cmp     al,0E0h                 ; E0h and a scan code: the grey key
        jne     kbc_out                 ; reads as its white twin.  E0h
        or      ah,ah                   ; with none is the character
        jz      kbc_out
        xor     al,al
kbc_out:
        ret

;---------------------------------------------------------------------
; key_find - AX = a key.  ZF set and BX = its row, CX = the row's
;            length, if the table has one; ZF clear if not.
;---------------------------------------------------------------------
key_find:
        mov     bx,[p_key]
kf_lp:
        mov     cl,[bx]
        xor     ch,ch
        jcxz    kf_no
        cmp     al,0
        je      kf_word
        cmp     al,0E0h
        jne     kf_byte
        cmp     byte ptr [sw_x],0
        je      kf_byte
kf_word:
        cmp     ax,word ptr [bx+1]
        jmp     short kf_test
kf_byte:
        cmp     al,[bx+1]
kf_test:
        je      kf_out
        add     bx,cx
        jmp     short kf_lp
kf_no:
        inc     cx                      ; ZF clear
kf_out:
        ret

;--- key_defn - BX = a key's row, AX = the key -> SI, CX = what it types
key_defn:
        mov     cl,[bx]
        xor     ch,ch
        lea     si,[bx+2]
        sub     cx,2
        cmp     al,0
        je      kd_word
        cmp     al,0E0h
        jne     kd_out
        cmp     byte ptr [sw_x],0
        je      kd_out
kd_word:
        inc     si
        dec     cx
kd_out:
        ret

;--- key_owe - AX = a key the BIOS has just given and the table has a
;--- meaning for: the meaning is owed as input now
key_owe:
        call    key_defn
        mov     [pend_ptr],si
        mov     [pend_cnt],cx
        ret

;---------------------------------------------------------------------
; key_peek - CF clear and AL = the byte a READ would give next, which
;            stays where it is; CF set if there is none.
;
; A KEY WITH A MEANING IS TAKEN FROM THE BIOS HERE, and its meaning
; becomes owed input, so that what this says is there is exactly what
; the read that follows gets - including when the meaning is nothing at
; all, which is looked past.
;---------------------------------------------------------------------
key_peek:
        mov     al,[altah]
        or      al,al
        jnz     kp_have
        cmp     word ptr [pend_cnt],0
        je      kp_kbd
        mov     si,[pend_ptr]
        mov     al,[si]
        or      al,al
        jnz     kp_have
        cmp     word ptr [pend_cnt],2   ; a 0 is a key only with a scan
        jb      kp_drop1                ; code after it, and 0;0 is none
        cmp     byte ptr [si+1],0
        jne     kp_have
        add     word ptr [pend_ptr],2
        sub     word ptr [pend_cnt],2
        jmp     short key_peek
kp_drop1:
        inc     word ptr [pend_ptr]
        dec     word ptr [pend_cnt]
        jmp     short key_peek
kp_kbd:
        call    kb_ask
        jz      kp_none
        or      ax,ax
        jnz     kp_key
        call    kb_take                 ; the Break key's leftover
        jmp     short key_peek
kp_key:
        call    kb_cook
        call    key_find
        jnz     kp_plain
        push    ax
        call    kb_take
        pop     ax
        call    key_owe
        jmp     short key_peek
kp_plain:
        cmp     byte ptr [sw_x],0
        je      kp_have
        call    kb_fold
kp_have:
        clc
        ret
kp_none:
        stc
        ret

;---------------------------------------------------------------------
; key_get - the next byte of input -> AL, waiting for a key if need be.
;---------------------------------------------------------------------
key_get:
        mov     al,[altah]
        or      al,al
        jz      kg_pend
        mov     byte ptr [altah],0
        ret
kg_pend:
        cmp     word ptr [pend_cnt],0
        je      kg_kbd
        mov     si,[pend_ptr]
        lodsb
        dec     word ptr [pend_cnt]
        or      al,al
        jnz     kg_took
        cmp     word ptr [pend_cnt],0
        je      kg_skip                 ; a 0 with nothing after it
        mov     ah,[si]
        inc     si
        dec     word ptr [pend_cnt]
        mov     [pend_ptr],si
        or      ah,ah
        jz      key_get                 ; 0;0 types nothing
        mov     [altah],ah
        xor     al,al
        ret
kg_skip:
        mov     [pend_ptr],si
        jmp     short key_get
kg_took:
        mov     [pend_ptr],si
        ret
kg_kbd:
        call    kb_take
        or      ax,ax
        jz      key_get                 ; the Break key's leftover
        call    kb_cook
        call    key_find
        jnz     kg_plain
        call    key_owe
        jmp     short key_get
kg_plain:
        cmp     byte ptr [sw_x],0
        je      kg_ret
        call    kb_fold
kg_ret:
        or      al,al
        jnz     kg_out
        mov     [altah],ah              ; the scan code, at the next read
kg_out:
        ret

;=====================================================================
; READ, NON-DESTRUCTIVE READ, FLUSH
;=====================================================================
c_read:
        les     bx,dword ptr [reqptr]
        mov     cx,es:[bx+RQ_COUNT]
        jcxz    cr_out
        les     di,dword ptr es:[bx+RQ_BUF]
cr_lp:
        push    cx
        push    di
        push    es
        call    key_get
        pop     es
        pop     di
        pop     cx
        stosb
        loop    cr_lp
cr_out:
        mov     ax,ST_DONE
        ret

c_peek:
        call    key_peek
        jc      cpk_none
        les     bx,dword ptr [reqptr]
        mov     es:[bx+RQ_MEDIA],al
        mov     ax,ST_DONE
        ret
cpk_none:
        mov     ax,ST_DONE OR ST_BUSY
        ret

c_flush:
        mov     byte ptr [altah],0
        mov     word ptr [pend_cnt],0
cf_lp:
        call    kb_ask
        jz      cf_out
        call    kb_take
        jmp     short cf_lp
cf_out:
        mov     ax,ST_DONE
        ret

;---------------------------------------------------------------------
; a_key - ESC[k;...p: the parameters are the key and what it types.
;         An old meaning goes first, whether or not there is a new one;
;         one that would not fit is ignored, as DOS ignored it.
;---------------------------------------------------------------------
a_key:
        mov     si,[p_prm]
        mov     ax,word ptr [si]        ; the key, one byte or two
        call    key_find
        jnz     ak_new

        ; THE OLD ROW OUT, and the table closed up behind it.  Owed
        ; input may be part of what moves, so it is dropped, as DOS's
        ; driver dropped it.
        mov     word ptr [pend_cnt],0
        mov     di,bx
        mov     si,bx
        add     si,cx
        mov     ax,[p_key]
        add     ax,[key_used]
        inc     ax                      ; the 0 at the end moves too
        sub     ax,si
        push    cx
        mov     cx,ax
        push    ds
        pop     es
        rep     movsb
        pop     cx
        sub     [key_used],cx
ak_new:
        mov     cx,[prm_i]
        inc     cx                      ; the parameters
        mov     si,[p_prm]
        mov     dx,1                    ; how many of them are the key
        cmp     byte ptr [si],0
        je      ak_two
        cmp     byte ptr [si],0E0h
        jne     ak_len
        cmp     byte ptr [sw_x],0
        je      ak_len
ak_two:
        inc     dx
ak_len:
        cmp     cx,dx
        jbe     ak_out                  ; the key alone: no new meaning
        mov     ax,cx
        inc     ax                      ; and the length byte
        mov     dx,[key_used]
        add     dx,ax
        cmp     dx,KEY_MAX - 1          ; with the 0 after it, it must fit
        ja      ak_out
        push    ds
        pop     es
        mov     di,[p_key]
        add     di,[key_used]
        stosb
        mov     si,[p_prm]
        rep     movsb
        mov     byte ptr [di],0
        mov     [key_used],dx
ak_out:
        ret

;=====================================================================
; THE GENERIC IOCTL - 440Ch on the console, CL = 7Fh to read the
; display's mode and 5Fh to set it, ES:DI -> the block:
;
;   +0  db  0 (the only level)       +8   dw  colours (0 for mono)
;   +1  db  0                        +10  dw  pixels across  } FFFFh in
;   +2  dw  14, the bytes after it   +12  dw  pixels down    } text
;   +4  dw  1 = background intensity +14  dw  columns
;           in place of blinking     +16  dw  rows
;   +6  db  1 text, 2 graphics
;   +7  db  0
;
; MODE CON LINES=43 is this call: 25, 43 or 50 lines on a VGA, 25 or 43
; on an EGA, 25 on anything older.
;=====================================================================
c_gen:
        les     bx,dword ptr [reqptr]
        mov     al,es:[bx+RQ_GMINOR]
        cmp     al,7Fh
        je      cg_go
        cmp     al,5Fh
        jne     c_unk
cg_go:
        les     di,dword ptr es:[bx+RQ_GPKT]
        call    gen_do
        jc      cg_err
        mov     ax,ST_DONE
        ret
cg_err:
        or      ax,ST_ERRDONE
        ret

;--- gen_do - AL = 7Fh or 5Fh, ES:DI -> the block.  CF and AX = the
;--- error, or AX = 0.
gen_do:
        cmp     al,7Fh
        je      gen_get
        cmp     al,5Fh
        je      gen_set
        mov     ax,GE_FUNC
        stc
        ret

gen_get:
        cmp     byte ptr es:[di],0
        jne     gg_bad
        cmp     word ptr es:[di+2],14
        jb      gg_bad
        mov     byte ptr es:[di+1],0
        mov     ah,0Fh
        int     10h
        and     al,7Fh
        mov     si,OFFSET modetab
gg_find:
        cmp     si,OFFSET modetab_end
        jae     gg_unsup
        cmp     al,[si+MT_MODE]
        je      gg_have
        add     si,MT_LEN
        jmp     short gg_find
gg_have:
        mov     word ptr es:[di+2],14
        call    intens_get
        mov     es:[di+4],ax
        mov     al,[si+MT_KIND]
        mov     es:[di+6],al
        mov     byte ptr es:[di+7],0
        mov     ax,[si+MT_COLORS]
        mov     es:[di+8],ax
        mov     ax,[si+MT_WIDE]
        mov     es:[di+10],ax
        mov     ax,[si+MT_HIGH]
        mov     es:[di+12],ax
        mov     ax,[si+MT_COLS]
        mov     es:[di+14],ax
        mov     ax,[si+MT_ROWS]
        cmp     ax,0FFFFh
        jne     gg_rows
        mov     ax,25
        test    byte ptr [vclass],VC_EGAC OR VC_EGAM OR VC_VGA
        jz      gg_rows
        push    es
        mov     ax,40h
        mov     es,ax
        mov     al,es:[B_ROWS]
        pop     es
        xor     ah,ah
        inc     ax
gg_rows:
        mov     es:[di+16],ax
        xor     ax,ax
        clc
        ret
gg_bad:
        mov     ax,GE_FUNC
        stc
        ret
gg_unsup:
        mov     ax,GE_UNSUP
        stc
        ret

;--- intens_get - SI = the mode's row -> AX = 1 if the background is
;--- bright rather than blinking.  A VGA is asked (INT 10h AH=1Bh); an
;--- EGA or an MCGA is what 5Fh last made it; anything older, and
;--- mono, blinks.
intens_get:
        xor     ax,ax
        cmp     byte ptr [si+MT_MODE],7
        je      ig_out
        test    byte ptr [vclass],VC_EGAUP
        jz      ig_out
        mov     al,[intens]
        test    byte ptr [vclass],VC_VGA
        jz      ig_out
        push    es
        push    di
        push    ds
        pop     es
        mov     di,[p_info]
        mov     ah,1Bh
        xor     bx,bx
        int     10h
        xor     ax,ax
        test    byte ptr es:[di+2Dh],20h
        jnz     ig_back
        inc     ax
ig_back:
        pop     di
        pop     es
ig_out:
        ret

gen_set:
        cmp     byte ptr es:[di],0
        jne     gs_bad
        cmp     word ptr es:[di+2],14
        jne     gs_bad
        mov     ax,es:[di+4]
        test    ax,0FFFEh
        jnz     gs_bad
        mov     [gs_want],al            ; before a BIOS call can move ES:DI
        test    al,1
        jz      gs_find
        test    byte ptr [vclass],VC_EGAUP
        jz      gs_bad
gs_find:
        mov     si,OFFSET modetab
gs_try:
        cmp     si,OFFSET modetab_end
        jae     gs_unsup
        mov     al,[vclass]
        test    al,[si+MT_CARDS]
        jz      gs_next
        mov     al,es:[di+6]
        cmp     al,[si+MT_KIND]
        jne     gs_next
        cmp     byte ptr es:[di+7],0
        jne     gs_next
        mov     ax,es:[di+8]
        cmp     ax,[si+MT_COLORS]
        jne     gs_next
        cmp     byte ptr [si+MT_KIND],1
        je      gs_text
        mov     ax,es:[di+10]
        cmp     ax,[si+MT_WIDE]
        jne     gs_next
        mov     ax,es:[di+12]
        cmp     ax,[si+MT_HIGH]
        jne     gs_next
        jmp     short gs_have
gs_text:
        mov     ax,es:[di+14]
        cmp     ax,[si+MT_COLS]
        je      gs_have
gs_next:
        add     si,MT_LEN
        jmp     short gs_try

gs_have:
        cmp     byte ptr [si+MT_KIND],1
        jne     gs_mode                 ; a graphics mode is only a mode
        ; THE ROWS: a VGA chooses its scan lines before the mode is set,
        ; and 43 or 50 is the 8x8 font afterwards
        mov     ax,es:[di+16]
        xor     dx,dx                   ; DL = scan lines, DH = small font
        cmp     ax,25
        je      gs_r25
        test    byte ptr [vclass],VC_VGA
        jnz     gs_rvga
        test    byte ptr [vclass],VC_EGAC OR VC_EGAM
        jz      gs_unsup
        cmp     ax,43
        jne     gs_unsup
        mov     dx,0100h
        jmp     short gs_rows
gs_rvga:
        mov     dx,0101h                ; 350 lines and the small font
        cmp     ax,43
        je      gs_scan
        mov     dl,2                    ; 400 lines
        cmp     ax,50
        je      gs_scan
        jmp     short gs_unsup
gs_r25:
        test    byte ptr [vclass],VC_VGA
        jz      gs_rows
        mov     dl,2
gs_scan:
        push    dx
        mov     ah,12h
        mov     bl,30h
        mov     al,dl
        int     10h
        pop     dx
gs_rows:
        push    dx
        mov     al,[si+MT_MODE]
        xor     ah,ah
        int     10h
        pop     dx
        or      dh,dh
        jz      gs_flags
        mov     ax,1112h
        xor     bl,bl
        int     10h
        mov     ax,1103h
        xor     bl,bl
        int     10h
        jmp     short gs_flags
gs_mode:
        mov     al,[si+MT_MODE]
        xor     ah,ah
        int     10h
gs_flags:
        test    byte ptr [vclass],VC_EGAUP
        jz      gs_ok
        cmp     byte ptr [si+MT_MODE],7
        je      gs_ok
        mov     bl,[gs_want]
        mov     [intens],bl
        xor     bl,1                    ; BL = 1: blink
        mov     ax,1003h
        int     10h
gs_ok:
        xor     ax,ax
        clc
        ret
gs_bad:
        mov     ax,GE_FUNC
        stc
        ret
gs_unsup:
        mov     ax,GE_UNSUP
        stc
        ret

;=====================================================================
; INT 2Fh, AH = 1Ah - this driver's own:
;   AL = 00h  is ANSI.SYS here?  -> AL = FFh
;   AL = 01h  the generic IOCTL by another road: CL = 7Fh or 5Fh, DS:DX
;             -> the block.  CF and AX = the error, or AX = 011Ah
;   AL = 02h  DS:DX -> 3 bytes: level 1 asks whether /L is in force,
;             answered in the third, and it never is
; Everything else goes on to whoever had the vector before.
;=====================================================================
i2f:
        cmp     ah,1Ah
        jne     i2f_chain
        or      al,al
        jz      i2f_inst
        cmp     al,2
        jbe     i2f_ours
i2f_chain:
        jmp     dword ptr cs:[old2f]
i2f_inst:
        mov     al,0FFh
        iret
i2f_ours:
        sti
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    bp
        push    ds
        push    es
        push    ax
        push    ds
        pop     es
        mov     di,dx                   ; ES:DI = the block
        push    cs
        pop     ds
        cld
        cmp     al,1
        jne     i2f_info
        mov     al,cl
        call    gen_do
        jmp     short i2f_ret
i2f_info:
        cmp     byte ptr es:[di],1
        jne     i2f_ok
        mov     byte ptr es:[di+2],0
i2f_ok:
        xor     ax,ax
        clc
i2f_ret:
        pop     bx                      ; the caller's AX
        jc      i2f_err
        mov     ax,bx
        xchg    ah,al                   ; "ANSI.SYS answered"
i2f_err:
        pop     es
        pop     ds
        pop     bp
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        retf    2                       ; with this carry, not the caller's

;--- INT 1Bh, under DOSes other than PM-DOS: the Break key is a ^C at
;--- the next read, as DOS's own console made it
i1b:
        mov     byte ptr cs:[altah],3
        iret

i2f_iret:
        iret                            ; INT 2Fh's end, if nobody had it

;=====================================================================
; Everything past here is INIT, and the key table, the parameters and
; the stack take its place once it has run.  The byte at endres is the
; key table's first, and the 0 that ends it - so it is in the file.
;=====================================================================
endres:
        db      0

;=====================================================================
; INIT - read the switches, look at the adapter and the keyboard, take
;        the vectors, and say where the driver ends.
;=====================================================================
c_init:
        les     bx,dword ptr [reqptr]
        push    ds
        lds     si,dword ptr es:[bx+RQ_ARG]
        call    parse_args
        pop     ds
        jnc     ci_ok

        ; REFUSED: no memory kept and an error in the status, which is
        ; how a driver says it did not install.  The FFFFh asks MS-DOS
        ; to say which CONFIG.SYS line it was; PM-DOS says it anyway.
        les     bx,dword ptr [reqptr]
        mov     word ptr es:[bx+RQ_END],0
        mov     es:[bx+RQ_END+2],cs
        mov     byte ptr es:[bx+RQ_UNITS],0
        mov     word ptr es:[bx+RQ_ERRMSG],0FFFFh
        mov     ax,ST_UNKNOWN
        ret

ci_ok:
        mov     ax,OFFSET endres
        mov     [p_key],ax
        add     ax,BSS_PRM
        mov     [p_prm],ax
        add     ax,BSS_INFO - BSS_PRM
        mov     [p_info],ax
        add     ax,BSS_STK - BSS_INFO
        mov     [p_stk],ax

        mov     ax,40h
        mov     es,ax
        cmp     byte ptr [sw_s],0
        je      ci_kbd
        mov     byte ptr es:[B_ROWS],24
ci_kbd:
        test    byte ptr es:[B_KBF3],10h        ; a 101-key keyboard, and
        jz      ci_dos                          ; INT 16h AH=10h for it
        cmp     byte ptr [sw_k],0
        jne     ci_dos
        mov     byte ptr [ext16],1
ci_dos:
        ; PM-DOS answers 21h/F100h with its version in BX; a DOS that
        ; does not know the call leaves BX as it was
        mov     ax,0F100h
        mov     bx,0FFFFh
        int     21h
        cmp     bx,0FFFFh
        je      ci_card
        mov     byte ptr [pmdos],1
ci_card:
        call    vdetect

        ; THE VECTORS LAST, once the driver is ready for the calls
        mov     ax,352Fh
        int     21h
        mov     word ptr [old2f],bx
        mov     word ptr [old2f+2],es
        mov     ax,es
        or      ax,bx
        jnz     ci_2f
        mov     word ptr [old2f],OFFSET i2f_iret
        mov     word ptr [old2f+2],cs
ci_2f:
        mov     dx,OFFSET i2f
        mov     ax,252Fh
        int     21h
        mov     dx,OFFSET i29
        mov     ax,2529h
        int     21h
        cmp     byte ptr [pmdos],0
        jne     ci_end
        mov     dx,OFFSET i1b
        mov     ax,251Bh
        int     21h

ci_end:
        mov     word ptr [cmdtab],OFFSET c_unk  ; INIT is not here to run twice
        les     bx,dword ptr [reqptr]
        mov     ax,OFFSET endres
        add     ax,BSS_LEN
        mov     word ptr es:[bx+RQ_END],ax
        mov     es:[bx+RQ_END+2],cs
        mov     byte ptr es:[bx+RQ_UNITS],0
        mov     byte ptr [ini_done],1
        mov     ax,ST_DONE
        ret

;---------------------------------------------------------------------
; vdetect - which adapter: VGA or MCGA by INT 10h AX=1A00h, an EGA by
;           AH=12h BL=10h, and otherwise a mono or a colour card by the
;           equipment word.  -> [vclass]
;---------------------------------------------------------------------
vdetect:
        mov     ax,1A00h
        int     10h
        cmp     al,1Ah
        jne     vd_ega
        mov     al,VC_VGA
        cmp     bl,7
        je      vd_set
        cmp     bl,8
        je      vd_set
        mov     al,VC_MCGA
        cmp     bl,0Bh
        je      vd_set
        cmp     bl,0Ch
        je      vd_set
vd_ega:
        mov     ah,12h
        mov     bl,10h
        int     10h
        cmp     bl,10h
        je      vd_old
        mov     al,VC_EGAC
        or      bh,bh
        jz      vd_set
        mov     al,VC_EGAM
        jmp     short vd_set
vd_old:
        int     11h
        and     al,30h
        cmp     al,30h
        mov     al,VC_MDA
        je      vd_set
        mov     al,VC_CGA
vd_set:
        mov     [vclass],al
        ret

;---------------------------------------------------------------------
; parse_args - DS:SI -> the DEVICE= line: the driver's own path, then
;              switches.  CF, with the reason printed, on one this
;              driver does not know or a word that is not a switch.
;---------------------------------------------------------------------
parse_args:
pa_lead:
        lodsb
        cmp     al,' '
        je      pa_lead
        cmp     al,9
        je      pa_lead
pa_path:
        call    pa_isend
        je      pa_ok
        cmp     al,' '
        je      pa_gap
        cmp     al,9
        je      pa_gap
        cmp     al,'/'
        je      pa_sw                   ; ANSI.SYS/X
        lodsb
        jmp     short pa_path
pa_gap:
        lodsb
pa_next:
        cmp     al,' '
        je      pa_gap
        cmp     al,9
        je      pa_gap
        cmp     al,','
        je      pa_gap
        call    pa_isend
        je      pa_ok
        cmp     al,'/'
        je      pa_sw
        jmp     pa_word

;--- a switch: its letters, folded, into swbuf
pa_sw:
        xor     cx,cx
psw_lp:
        lodsb
        cmp     al,'a'
        jb      psw_up
        cmp     al,'z'
        ja      psw_up
        sub     al,20h
psw_up:
        cmp     al,'A'
        jb      psw_end
        cmp     al,'Z'
        ja      psw_end
        cmp     cx,SW_MAX
        jae     psw_lp                  ; a long one is no switch of ours
        mov     bx,cx
        mov     byte ptr cs:swbuf[bx],al
        inc     cx
        jmp     short psw_lp
psw_end:
        mov     byte ptr cs:[swlen],cl
        push    ax                      ; what ended it
        mov     di,OFFSET sw_x
        mov     bx,OFFSET swn_x
        call    pa_is
        je      psw_set
        mov     di,OFFSET sw_k
        mov     bx,OFFSET swn_k
        call    pa_is
        je      psw_set
        mov     di,OFFSET sw_r
        mov     bx,OFFSET swn_r
        call    pa_is
        je      psw_set
        mov     di,OFFSET sw_s
        mov     bx,OFFSET swn_s
        call    pa_is
        je      psw_set
        mov     bx,OFFSET swn_ss
        call    pa_is
        je      psw_set
        mov     di,OFFSET swl_dummy     ; /L: taken, and nothing more
        mov     bx,OFFSET swn_l
        call    pa_is
        je      psw_set
        pop     ax
        mov     dx,OFFSET msg_badsw
        call    pa_say
        call    pa_swname
        jmp     short pa_crlf
psw_set:
        mov     byte ptr cs:[di],1
        pop     ax
        jmp     pa_next

;--- a word that is not a switch
pa_word:
        push    ax
        mov     dx,OFFSET msg_toomany
        call    pa_say
        pop     ax
paw_lp:
        mov     dl,al
        mov     ah,02h
        int     21h
        lodsb
        cmp     al,' '
        je      pa_crlf
        cmp     al,9
        je      pa_crlf
        cmp     al,'/'
        je      pa_crlf
        call    pa_isend
        jne     paw_lp
pa_crlf:
        mov     dx,OFFSET msg_crlf
        call    pa_say
        stc
        ret
pa_ok:
        clc
        ret

;--- pa_isend - ZF set if AL ends the line
pa_isend:
        cmp     al,13
        je      pie_out
        cmp     al,10
        je      pie_out
        or      al,al
pie_out:
        ret

;--- pa_is - ZF set if swbuf holds the switch whose name, length first,
;--- is at CS:BX
pa_is:
        push    cx
        push    si
        mov     cl,cs:[bx]
        cmp     cl,byte ptr cs:[swlen]
        jne     pis_out
        xor     ch,ch
        xor     si,si
pis_lp:
        mov     al,cs:swbuf[si]
        cmp     al,cs:[bx+si+1]
        jne     pis_out
        inc     si
        loop    pis_lp
pis_out:
        pop     si
        pop     cx
        ret

;--- pa_say - the $-ended string at CS:DX
pa_say:
        push    ds
        push    cs
        pop     ds
        mov     ah,09h
        int     21h
        pop     ds
        ret

;--- pa_swname - "/" and the switch as it was read
pa_swname:
        mov     dl,'/'
        mov     ah,02h
        int     21h
        xor     bx,bx
psn_lp:
        cmp     bl,byte ptr cs:[swlen]
        jae     psn_out
        mov     dl,byte ptr cs:swbuf[bx]
        mov     ah,02h
        int     21h
        inc     bx
        jmp     short psn_lp
psn_out:
        ret

swn_x       db  1,'X'
swn_k       db  1,'K'
swn_r       db  1,'R'
swn_s       db  1,'S'
swn_ss      db  10,'SCREENSIZE'
swn_l       db  1,'L'
swlen       db  0
swl_dummy   db  0
swbuf       db  SW_MAX dup (0)
msg_badsw   db  'ANSI.SYS: invalid switch - $'
msg_toomany db  'ANSI.SYS: too many parameters - $'
msg_crlf    db  13,10,'$'

_AN     ENDS
        END     header
