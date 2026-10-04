;=====================================================================
; VBETEST.ASM - VBETEST.EXE, VBE and the linear frame buffer for a
;               native program (21h/F1h AL=1Fh, DOS\VBE.INC)
;
;   VBETEST         the checks below
;   VBETEST /L      the modes the BIOS lists, one a line
;   VBETEST /S [m]  a picture in mode m (hex), or in the best 640x480
;                   mode with a linear frame buffer, until a key
;   VBETEST /X      for check 11: set a mode and end, leaving it set
;   VBETEST /F      for check 12: set a mode and fault
;   VBETEST /N      for check 8: set mode 13h and end, leaving it set
;
; The checks:
;
;   1  VBF_INFO: 'VESA', a mode list that ends where ECX says, every
;      pointer 0 or inside the block, an OEM string
;   2  VBF_MODE for every listed mode: answered, or refused by the BIOS
;      (ERR_VBEFAIL, counted and said - a BIOS may list what its setup
;      cannot give), and at least one answered.  And one with a linear
;      frame buffer to test with: 640x480 at 8 bits if there is one,
;      any 8-bit one if not, any at all after that
;   3  VBF_SET: EAX = SEL_VBE, its limit is ECX and at least a screen,
;      it is DPL 3 and writable, and 4F03h says the mode is set
;   4  every byte of it written through SEL_VBE and read back
;   5  4F07h through VBF_BIOS: the display start moved down a line, and
;      read back as moved
;   6  VBF_PALETTE: 256 entries set, and read back the same
;   7  refused, with the right error: a buffer outside the slot, and
;      one that runs past its end; a VBE mode without VBM_LFB; a VGA
;      mode with it; a mode the BIOS has not got; VBF_BIOS 05h;
;      function 99h; a palette of no entries, and one past 256
;   8  a child sets mode 13h and ends: this program's mode is back,
;      and SEL_VBE covers this program's frame buffer again
;   9  VBF_SET 13h: SEL_VBE is the 64K at A0000h, written and read back
;  10  VBF_RESTORE: 4F03h says the mode this started in, SEL_VBE is
;      empty, and a second VBF_RESTORE changes nothing
;  11  a child sets a mode and ends without putting it back: the mode
;      this started in is back after it
;  12  a child sets a mode and faults: the same, and it ended as a
;      fault ends a program, with 255.  Its report is on the screen,
;      in text, for a capture to show (r3_fault puts the mode back
;      before it writes it)
;
; EVERYTHING IT SAYS WAITS IN A BUFFER until the screen is text again,
; because printed in a graphics mode it goes nowhere anybody can read -
; unless standard output is a FILE, which can be written in any mode:
; then each check's line goes as soon as it is decided, and the cache
; is written (0Dh), so that a check that hangs the machine leaves the
; lines before it on the disk.  Not in the file's directory entry -
; 68h does nothing for handles 0 to 2, and only a close writes the
; entry - so after a hang the file says 0 bytes and the lines are in a
; lost chain: the CHKDSK the next boot runs makes them FILE0000.CHK.
; (The first Banshee runs left 0 bytes and nothing anywhere.)
; With no VBE at all, 1 says so and nothing else is tried.  With no
; mode that has a linear frame buffer - and VBE 1.x has none, which is
; what most cards' own BIOSes of the period are - 3 to 6, 8, 11 and 12
; are skipped and the rest still run.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

; The calls, all of EAX.
VB_INFO         EQU     0F11Fh + (VBF_INFO SHL 16)
VB_MODE         EQU     0F11Fh + (VBF_MODE SHL 16)
VB_SET          EQU     0F11Fh + (VBF_SET SHL 16)
VB_RESTORE      EQU     0F11Fh + (VBF_RESTORE SHL 16)
VB_PALETTE      EQU     0F11Fh + (VBF_PALETTE SHL 16)
VB_GETMODE      EQU     0F11Fh + (VBF_BIOS SHL 16) + 03000000h
VB_BANK         EQU     0F11Fh + (VBF_BIOS SHL 16) + 05000000h
VB_START        EQU     0F11Fh + (VBF_BIOS SHL 16) + 07000000h
VB_NOSUCH       EQU     0F11Fh + 00990000h

; 4F01h's colour fields, which only the picture needs
VT_RSIZE        EQU     1Fh
VT_RPOS         EQU     20h
VT_GSIZE        EQU     21h
VT_GPOS         EQU     22h
VT_BSIZE        EQU     23h
VT_BPOS         EQU     24h

CHILD_X         EQU     42
CHILD_N         EQU     43

; The buffers, past the end of the image: D32_HEAD zeroes them.
BS_OUT          EQU     0               ; what is said, until the end
BS_OUTLEN       EQU     4096
BS_INFO         EQU     BS_OUT + BS_OUTLEN
BS_MI           EQU     BS_INFO + VBI_LEN
BS_TMI          EQU     BS_MI + VMI_LEN ; the mode being tested
BS_PAL          EQU     BS_TMI + VMI_LEN
BS_PAL2         EQU     BS_PAL + 1024
BS_LINE         EQU     BS_PAL2 + 1024  ; one scan line of the picture
BS_LINELEN      EQU     8192
BS_LEN          EQU     BS_LINE + BS_LINELEN

_VT     SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_VT, DS:_VT, ES:_VT, SS:_VT
        ORG     0

        D32_HEAD SLOT_IMAGE,<OFFSET vt_entry>,<BS_LEN>,8192

vt_entry:
        cld
        mov     dword ptr [vt_outp],OFFSET vt_bss + BS_OUT
        mov     eax,4400h                       ; is standard output a
        mov     ebx,1                           ; device, or a file?
        int     21h
        jc      ve_held
        test    dl,80h
        jnz     ve_held
        mov     byte ptr [vt_direct],1
ve_held:
        mov     esi,SLOT_PSP + PSP_TAIL
        movzx   ecx,byte ptr [esi]
        inc     esi
        call    skipblank
        or      ecx,ecx
        jz      vt_checks
        cmp     byte ptr [esi],'/'
        jne     vt_usage
        cmp     ecx,2
        jb      vt_usage
        mov     al,byte ptr [esi+1]
        add     esi,2
        sub     ecx,2
        and     al,0DFh
        cmp     al,'L'
        je      vt_list
        cmp     al,'S'
        je      vt_show
        cmp     al,'X'
        je      vt_childx
        cmp     al,'F'
        je      vt_childf
        cmp     al,'N'
        je      vt_childn
vt_usage:
        mov     edx,OFFSET msg_usage
        call    puts
        mov     al,1
        jmp     vt_quit

;--- AL = the exit code, once everything said has been written --------
vt_quit:
        push    eax
        call    flush
        pop     eax
        mov     ah,4Ch
        int     21h

;=====================================================================
; The checks
;=====================================================================
vt_checks:
        mov     edx,OFFSET msg_hi
        call    puts

;--- 1: VBF_INFO ------------------------------------------------------
        mov     edx,OFFSET msg_t1
        call    puts
        call    getinfo
        jnc     t1_have
        cmp     ax,ERR_NOVBE
        jne     t1_bad
        mov     edx,OFFSET msg_novbe
        call    puts
        mov     al,2
        jmp     vt_quit
t1_have:
        mov     edi,OFFSET vt_bss + BS_INFO
        cmp     dword ptr [edi+VBI_SIG],41534556h       ; 'VESA'
        jne     t1_bad
        mov     ecx,dword ptr [vt_nmodes]
        or      ecx,ecx
        jz      t1_bad
        lea     eax,[edi+VBI_RESERVED]
        cmp     dword ptr [edi+VBI_MODES],eax
        jne     t1_bad
        cmp     word ptr [eax+ecx*2],0FFFFh
        jne     t1_bad
        mov     esi,OFFSET t1_ptrs
t1_ptr:
        movzx   ebx,byte ptr [esi]
        or      ebx,ebx
        jz      t1_ptrsok
        mov     eax,dword ptr [edi+ebx]
        or      eax,eax
        jz      t1_pnext
        sub     eax,edi
        cmp     eax,VBI_LEN
        jae     t1_bad
t1_pnext:
        inc     esi
        jmp     t1_ptr
t1_ptrsok:
        mov     eax,dword ptr [edi+VBI_OEMSTR]
        or      eax,eax
        jz      t1_bad
        cmp     byte ptr [eax],0
        je      t1_bad
        call    good
        ; what it is, for whoever reads the output
        mov     edx,OFFSET msg_vbe
        call    puts
        movzx   eax,byte ptr [vt_ver+1]
        call    putdec
        mov     dl,'.'
        call    putc
        movzx   eax,byte ptr [vt_ver]
        call    putdec
        mov     edx,OFFSET msg_comma
        call    puts
        movzx   eax,word ptr [edi+VBI_MEMORY]
        shl     eax,6
        call    putdec
        mov     edx,OFFSET msg_kcomma
        call    puts
        mov     eax,dword ptr [vt_nmodes]
        call    putdec
        mov     edx,OFFSET msg_modes
        call    puts
        mov     esi,dword ptr [edi+VBI_OEMSTR]
        call    putz
        call    crlf
        cmp     word ptr [vt_ver],0200h
        jae     t2
        ; VBE 1.x is the BIOS's age and not a failure: there is nothing
        ; linear to test, and the rest still can be
        mov     edx,OFFSET msg_vbe1
        call    puts
        mov     byte ptr [vt_nolfb],1
        jmp     t2

t1_bad:
        call    bad
        mov     al,1
        jmp     vt_quit                         ; nothing else can be asked

;--- 2: VBF_MODE for every mode, and one to test with -----------------
t2:
        mov     edx,OFFSET msg_t2
        call    puts
        ; A BIOS may list a mode the card or its monitor setting cannot
        ; give, and answer 4F01h for it with a refusal rather than a
        ; mode with its "supported" bit clear: ATI's Mach64 lists twenty
        ; and describes the five at 640x480.  So a refusal is counted
        ; and said, and only one that is not the BIOS's (ERR_VBEFAIL),
        ; or a list with nothing the BIOS will describe, fails.
        mov     esi,dword ptr [vt_bss + BS_INFO + VBI_MODES]
        xor     ebx,ebx                         ; EBX = refused
t2_each:
        movzx   ecx,word ptr [esi]
        cmp     ecx,0FFFFh
        je      t2_all
        mov     eax,VB_MODE
        mov     edi,OFFSET vt_bss + BS_MI
        int     21h
        jnc     t2_next
        cmp     ax,ERR_VBEFAIL
        jne     t2_bad
        inc     ebx
t2_next:
        add     esi,2
        jmp     t2_each
t2_all:
        cmp     ebx,dword ptr [vt_nmodes]
        jae     t2_bad
        mov     dword ptr [vt_refused],ebx
        xor     bl,bl                           ; the checks' choice
        call    pickmode
        jc      t2_none
        call    good
        call    t2_refused
        mov     edx,OFFSET msg_testing
        call    puts
        call    putmode
        call    crlf
        jmp     t3
t2_none:
        call    good
        call    t2_refused
        cmp     byte ptr [vt_nolfb],0           ; 1 has said why already
        jne     t3
        mov     edx,OFFSET msg_nolfb
        call    puts
        mov     byte ptr [vt_nolfb],1
        jmp     t3
t2_bad:
        call    bad
        jmp     t3
; "the BIOS will not describe n of its m modes", if there were any
t2_refused:
        mov     eax,dword ptr [vt_refused]
        or      eax,eax
        jz      t2r_out
        mov     edx,OFFSET msg_refn
        call    puts
        call    putdec
        mov     edx,OFFSET msg_refof
        call    puts
        mov     eax,dword ptr [vt_nmodes]
        call    putdec
        mov     edx,OFFSET msg_refm
        call    puts
t2r_out:
        ret

;--- 3: VBF_SET with the linear frame buffer --------------------------
t3:
        mov     edx,OFFSET msg_t3
        call    puts
        call    getmode
        mov     dword ptr [vt_start],eax        ; the mode to come back to
        cmp     byte ptr [vt_nolfb],0
        jne     t3_skip
        movzx   ebx,word ptr [vt_tmode]
        or      ebx,VBM_LFB
        mov     eax,VB_SET
        int     21h
        jc      t3_bad
        mov     byte ptr [vt_set],1
        cmp     eax,SEL_VBE
        jne     t3_bad
        mov     dword ptr [vt_size],ecx
        mov     dword ptr [vt_pitch],edx
        or      edx,edx
        jz      t3_bad
        movzx   eax,word ptr [vt_bss + BS_TMI + VMI_YRES]
        imul    eax,edx
        cmp     ecx,eax
        jb      t3_bad
        call    selinfo
        jc      t3_bad
        inc     ecx
        cmp     ecx,dword ptr [vt_size]
        jne     t3_bad
        cmp     dl,DACC_DATA
        jne     t3_bad
        mov     eax,SEL_VBE
        verw    ax
        jnz     t3_bad
        call    getmode
        jc      t3_bad
        movzx   ebx,word ptr [vt_tmode]
        cmp     eax,ebx
        jne     t3_bad
        call    good
        jmp     t4
t3_skip:
        call    skipped
        jmp     t4
t3_bad:
        call    bad

;--- 4: every byte of the frame buffer, through SEL_VBE ---------------
t4:
        mov     edx,OFFSET msg_t4
        call    puts
        cmp     byte ptr [vt_set],0
        je      t4_skip
        mov     ecx,dword ptr [vt_size]
        call    fbfill
        jc      t4_bad
        call    good
        jmp     t5
t4_skip:
        call    skipped
        jmp     t5
t4_bad:
        call    bad

;--- 5: 4F07h moves the display start ---------------------------------
t5:
        mov     edx,OFFSET msg_t5
        call    puts
        cmp     byte ptr [vt_set],0
        je      t5_skip
        mov     eax,VB_START
        xor     ebx,ebx                         ; BL = 0: set it
        xor     ecx,ecx                         ; pixel 0
        mov     edx,1                           ; of line 1
        int     21h
        jc      t5_bad
        mov     eax,VB_START
        mov     ebx,1                           ; BL = 1: where is it?
        int     21h
        jc      t5_bad
        cmp     dx,1
        jne     t5_bad
        or      cx,cx
        jnz     t5_bad
        mov     eax,VB_START
        xor     ebx,ebx
        xor     ecx,ecx
        xor     edx,edx
        int     21h
        jc      t5_bad
        call    good
        jmp     t6
t5_skip:
        call    skipped
        jmp     t6
t5_bad:
        call    bad

;--- 6: the palette, there and back -----------------------------------
t6:
        mov     edx,OFFSET msg_t6
        call    puts
        cmp     byte ptr [vt_set],0
        je      t6_skip
        mov     edi,OFFSET vt_bss + BS_PAL
        xor     ecx,ecx
t6_make:
        mov     al,cl                           ; blue
        and     al,3Fh
        mov     byte ptr [edi+ecx*4],al
        mov     al,cl                           ; green
        shr     al,2
        mov     byte ptr [edi+ecx*4+1],al
        mov     al,cl                           ; red
        not     al
        shr     al,2
        mov     byte ptr [edi+ecx*4+2],al
        mov     byte ptr [edi+ecx*4+3],0
        inc     ecx
        cmp     ecx,256
        jb      t6_make
        mov     eax,VB_PALETTE
        xor     ebx,ebx                         ; set
        mov     ecx,256
        xor     edx,edx
        int     21h
        jc      t6_bad
        mov     eax,VB_PALETTE
        mov     ebx,1                           ; read
        mov     ecx,256
        xor     edx,edx
        mov     edi,OFFSET vt_bss + BS_PAL2
        int     21h
        jc      t6_bad
        mov     esi,OFFSET vt_bss + BS_PAL
        mov     edi,OFFSET vt_bss + BS_PAL2
        mov     ecx,256
t6_cmp:
        mov     eax,dword ptr [esi]
        xor     eax,dword ptr [edi]
        and     eax,00FFFFFFh                   ; the fourth is padding
        jnz     t6_bad
        add     esi,4
        add     edi,4
        loop    t6_cmp
        call    good
        jmp     t7
t6_skip:
        call    skipped
        jmp     t7
t6_bad:
        call    bad

;--- 7: what must be refused ------------------------------------------
t7:
        mov     edx,OFFSET msg_t7
        call    puts
        mov     word ptr [vt_want],ERR_ACCESS
        mov     eax,VB_INFO
        mov     edi,0FFFFF000h                  ; nowhere in the slot
        call    try
        jne     t7_bad
        mov     eax,VB_INFO
        mov     edi,dword ptr ds:[SLOT_PSP + PSP_MEMTOP]
        sub     edi,100h                        ; in it, and running out
        call    try
        jne     t7_bad
        mov     word ptr [vt_want],ERR_BADDATA
        mov     eax,VB_SET
        movzx   ebx,word ptr [vt_tmode]         ; a VBE mode, not linear
        cmp     byte ptr [vt_nolfb],0
        je      t7_vbe
        mov     ebx,101h
t7_vbe:
        call    try
        jne     t7_bad
        mov     eax,VB_SET
        mov     ebx,13h OR VBM_LFB              ; a VGA mode, linear
        call    try
        jne     t7_bad
        mov     eax,VB_SET
        mov     ebx,VBM_LFB OR 0800h            ; a CRTC block it has not got
        call    try
        jne     t7_bad
        mov     eax,VB_PALETTE
        xor     ebx,ebx
        xor     ecx,ecx                         ; no entries
        xor     edx,edx
        mov     edi,OFFSET vt_bss + BS_PAL
        call    try
        jne     t7_bad
        mov     eax,VB_PALETTE
        xor     ebx,ebx
        mov     ecx,100
        mov     edx,200                         ; to entry 299
        call    try
        jne     t7_bad
        mov     word ptr [vt_want],ERR_VBEFAIL
        mov     eax,VB_SET
        mov     ebx,1FFh OR VBM_LFB             ; not in any list
        call    try
        jne     t7_bad
        mov     word ptr [vt_want],ERR_FUNC
        mov     eax,VB_BANK
        call    try
        jne     t7_bad
        mov     eax,VB_NOSUCH
        call    try
        jne     t7_bad
        ; and nothing refused has changed the screen
        cmp     byte ptr [vt_set],0
        je      t7_ok
        call    getmode
        movzx   ebx,word ptr [vt_tmode]
        cmp     eax,ebx
        jne     t7_bad
t7_ok:
        call    good
        jmp     t8
t7_bad:
        call    bad

;--- 8: a child's mode, and this one's back after it ------------------
t8:
        mov     edx,OFFSET msg_t8
        call    puts
        cmp     byte ptr [vt_set],0
        je      t8_skip
        mov     dword ptr [vt_xtail],OFFSET tail_n
        call    child
        jc      t8_bad
        cmp     al,CHILD_N
        jne     t8_bad
        call    getmode
        movzx   ebx,word ptr [vt_tmode]
        cmp     eax,ebx
        jne     t8_bad
        call    selinfo
        jc      t8_bad
        inc     ecx
        cmp     ecx,dword ptr [vt_size]
        jne     t8_bad
        cmp     dl,DACC_DATA
        jne     t8_bad
        call    good
        jmp     t9
t8_skip:
        call    skipped
        jmp     t9
t8_bad:
        call    bad

;--- 9: VGA mode 13h, and its window ----------------------------------
t9:
        mov     edx,OFFSET msg_t9
        call    puts
        mov     eax,VB_SET
        mov     ebx,13h
        int     21h
        jc      t9_bad
        mov     byte ptr [vt_set],1
        cmp     eax,SEL_VBE
        jne     t9_bad
        cmp     ecx,10000h
        jne     t9_bad
        call    selinfo
        jc      t9_bad
        cmp     ecx,0FFFFh
        jne     t9_bad
        cmp     dl,DACC_DATA
        jne     t9_bad
        call    getmode
        cmp     eax,13h
        jne     t9_bad
        mov     ecx,320 * 200
        call    fbfill
        jc      t9_bad
        call    good
        jmp     t10
t9_bad:
        call    bad

;--- 10: VBF_RESTORE --------------------------------------------------
t10:
        mov     edx,OFFSET msg_t10
        call    puts
        mov     eax,VB_RESTORE
        int     21h
        jc      t10_bad
        call    getmode
        cmp     eax,dword ptr [vt_start]
        jne     t10_bad
        call    selinfo
        jc      t10_bad
        cmp     dl,0F6h                         ; empty: expand-down
        jne     t10_bad
        mov     eax,VB_RESTORE
        int     21h
        jc      t10_bad
        call    getmode
        cmp     eax,dword ptr [vt_start]
        jne     t10_bad
        call    good
        jmp     t11
t10_bad:
        call    bad

;--- 11: a child that leaves a mode set -------------------------------
t11:
        mov     edx,OFFSET msg_t11
        call    puts
        cmp     byte ptr [vt_nolfb],0
        jne     t11_skip
        mov     dword ptr [vt_xtail],OFFSET tail_x
        call    child
        jc      t11_bad
        cmp     al,CHILD_X
        jne     t11_bad
        call    getmode
        cmp     eax,dword ptr [vt_start]
        jne     t11_bad
        call    selinfo
        jc      t11_bad
        cmp     dl,0F6h
        jne     t11_bad
        call    good
        jmp     t12
t11_skip:
        call    skipped
        jmp     t12
t11_bad:
        call    bad

;--- 12: a child that faults in a mode it set -------------------------
t12:
        mov     edx,OFFSET msg_t12
        call    puts
        cmp     byte ptr [vt_nolfb],0
        jne     t12_skip
        mov     dword ptr [vt_xtail],OFFSET tail_f
        call    child
        jc      t12_bad
        cmp     al,0FFh                         ; what a fault ends with
        jne     t12_bad
        call    getmode
        cmp     eax,dword ptr [vt_start]
        jne     t12_bad
        call    selinfo
        jc      t12_bad
        cmp     dl,0F6h
        jne     t12_bad
        call    good
        jmp     t_end
t12_skip:
        call    skipped
        jmp     t_end
t12_bad:
        call    bad

;--- the result -------------------------------------------------------
t_end:
        ; Whatever happened above, the screen is put back before the
        ; result is written: a check that failed may have left a mode.
        mov     eax,VB_RESTORE
        int     21h
        mov     edx,OFFSET msg_res
        call    puts
        mov     edx,OFFSET vt_results
        call    puts
        call    crlf
        mov     edx,OFFSET msg_pass
        xor     al,al
        cmp     byte ptr [vt_fails],0
        je      t_say
        mov     edx,OFFSET msg_fail
        mov     al,1
t_say:
        push    eax
        call    puts
        pop     eax
        jmp     vt_quit

;=====================================================================
; VBETEST /X, /F and /N - the children.  Each sets a mode, says
; nothing, and ends without VBF_RESTORE - /F by a fault, through a null
; pointer: 4Ch and the fault's report are what is being tested.
;=====================================================================
vt_childx:
        call    childset
        jc      vc_fail
        mov     ax,4C00h + CHILD_X
        int     21h
vt_childf:
        call    childset
        jc      vc_fail
        mov     dword ptr ds:[0],0
        jmp     vc_fail                         ; not reached
vt_childn:
        mov     eax,VB_SET
        mov     ebx,13h
        int     21h
        jc      vc_fail
        mov     ax,4C00h + CHILD_N
        int     21h
vc_fail:
        mov     ax,4C01h
        int     21h

;--- childset - the checks' mode, set.  CF if it could not be. --------
childset:
        call    getinfo
        jc      cs_out
        xor     bl,bl
        call    pickmode
        jc      cs_out
        movzx   ebx,word ptr [vt_tmode]
        or      ebx,VBM_LFB
        mov     eax,VB_SET
        int     21h
cs_out:
        ret

;=====================================================================
; VBETEST /L - the modes
;=====================================================================
vt_list:
        call    getinfo
        jnc     vl_have
        mov     edx,OFFSET msg_novbe2
        call    puts
        mov     al,2
        jmp     vt_quit
vl_have:
        mov     edx,OFFSET msg_vbe2
        call    puts
        movzx   eax,byte ptr [vt_ver+1]
        call    putdec
        mov     dl,'.'
        call    putc
        movzx   eax,byte ptr [vt_ver]
        call    putdec
        mov     edx,OFFSET msg_comma
        call    puts
        movzx   eax,word ptr [vt_bss + BS_INFO + VBI_MEMORY]
        shl     eax,6
        call    putdec
        mov     edx,OFFSET msg_kcolon
        call    puts
        mov     esi,dword ptr [vt_bss + BS_INFO + VBI_OEMSTR]
        or      esi,esi
        jz      vl_noname
        call    putz
vl_noname:
        call    crlf
        call    crlf
        mov     esi,dword ptr [vt_bss + BS_INFO + VBI_MODES]
vl_each:
        movzx   ecx,word ptr [esi]
        cmp     ecx,0FFFFh
        je      vl_done
        add     esi,2
        push    esi
        mov     dl,' '
        call    putc
        mov     eax,ecx
        mov     cl,4
        call    puthex
        mov     edx,OFFSET msg_h2
        call    puts
        movzx   ecx,word ptr [esi-2]
        mov     eax,VB_MODE
        mov     edi,OFFSET vt_bss + BS_MI
        int     21h
        jc      vl_refused
        mov     edi,OFFSET vt_bss + BS_MI
        test    word ptr [edi+VMI_ATTR],VMA_HW
        jz      vl_nohw
        test    word ptr [edi+VMI_ATTR],VMA_GRAPHICS
        jnz     vl_gfx
        mov     edx,OFFSET msg_text
        call    puts
        movzx   eax,word ptr [edi+VMI_XRES]
        call    putdec
        mov     dl,'x'
        call    putc
        movzx   eax,word ptr [edi+VMI_YRES]
        call    putdec
        jmp     vl_eol
vl_gfx:
        movzx   eax,word ptr [edi+VMI_XRES]
        call    putdec
        mov     dl,'x'
        call    putc
        movzx   eax,word ptr [edi+VMI_YRES]
        call    putdec
        mov     dl,'x'
        call    putc
        movzx   eax,byte ptr [edi+VMI_BPP]
        call    putdec
        test    word ptr [edi+VMI_ATTR],VMA_LFB
        jz      vl_banked
        mov     edx,OFFSET msg_lfbat
        call    puts
        mov     eax,dword ptr [edi+VMI_PHYS]
        mov     cl,8
        call    puthex
        mov     dl,'h'
        call    putc
        jmp     vl_eol
vl_banked:
        mov     edx,OFFSET msg_banked
        call    puts
        jmp     vl_eol
vl_nohw:
        mov     edx,OFFSET msg_nohw
        call    puts
        jmp     vl_eol
vl_refused:
        mov     edx,OFFSET msg_refused
        call    puts
vl_eol:
        call    crlf
        pop     esi
        jmp     vl_each
vl_done:
        xor     al,al
        jmp     vt_quit

;=====================================================================
; VBETEST /S [mode] - a picture, until a key
;
; Drawn the way a program should draw through SEL_VBE: a scan line at
; a time in its own memory, then REP MOVSB to ES:EDI.  A gradient of
; red across and green down with blue the other way, inside a white
; frame - if the frame's sides are straight, the pitch is right, and
; if the colours run smoothly, the masks are.  An 8-bit mode gets a
; palette of 3-3-2 colours to show the same picture in.
;=====================================================================
vt_show:
        call    skipblank
        xor     ebx,ebx                         ; the mode, if one is named
        xor     edx,edx                         ; and how many digits
vs_hex:
        jecxz   vs_parsed
        mov     al,byte ptr [esi]
        cmp     al,' '
        je      vs_parsed
        cmp     al,0Dh
        je      vs_parsed
        sub     al,'0'
        cmp     al,9
        jbe     vs_digit
        and     al,0DFh                         ; 'a'-'0' folds to 'A'-'0'
        sub     al,'A' - '0' - 10
        cmp     al,10
        jb      vs_badarg
        cmp     al,15
        ja      vs_badarg
vs_digit:
        shl     ebx,4
        movzx   eax,al
        or      ebx,eax
        inc     edx
        inc     esi
        dec     ecx
        jmp     vs_hex
vs_badarg:
        jmp     vt_usage
vs_parsed:
        push    ebx
        push    edx
        call    getinfo
        pop     edx
        pop     ebx
        jnc     vs_info
        mov     edx,OFFSET msg_novbe2
        call    puts
        mov     al,2
        jmp     vt_quit
vs_info:
        or      edx,edx
        jnz     vs_named
        mov     bl,1                            ; the picture's choice
        call    pickmode
        jnc     vs_chosen
        mov     edx,OFFSET msg_noshow
        call    puts
        mov     al,1
        jmp     vt_quit
vs_named:
        mov     word ptr [vt_tmode],bx
        mov     ecx,ebx
        mov     eax,VB_MODE
        mov     edi,OFFSET vt_bss + BS_MI
        int     21h
        jc      vs_cannot
        mov     bl,1
        call    score
        cmp     eax,-1
        je      vs_cannot
        mov     esi,OFFSET vt_bss + BS_MI
        mov     edi,OFFSET vt_bss + BS_TMI
        mov     ecx,VMI_LEN / 4
        rep     movsd
vs_chosen:
        movzx   ebx,word ptr [vt_tmode]
        or      ebx,VBM_LFB
        mov     eax,VB_SET
        int     21h
        jc      vs_cannot
        mov     dword ptr [vt_pitch],edx
        cmp     byte ptr [vt_bss + BS_TMI + VMI_BPP],8
        jne     vs_draw
        call    pal332

vs_draw:
        movzx   eax,byte ptr [vt_bss + BS_TMI + VMI_BPP]
        add     eax,7
        shr     eax,3
        mov     dword ptr [vt_bytes],eax
        xor     eax,eax
        mov     dword ptr [vt_y],eax
vs_line:
        mov     eax,dword ptr [vt_y]
        cmp     ax,word ptr [vt_bss + BS_TMI + VMI_YRES]
        jae     vs_drawn
        call    drawline
        mov     eax,SEL_VBE
        mov     es,ax
        mov     edi,dword ptr [vt_y]
        imul    edi,dword ptr [vt_pitch]
        mov     esi,OFFSET vt_bss + BS_LINE
        movzx   ecx,word ptr [vt_bss + BS_TMI + VMI_XRES]
        imul    ecx,dword ptr [vt_bytes]
        rep     movsb
        push    ds
        pop     es
        inc     dword ptr [vt_y]
        jmp     vs_line
vs_drawn:
        mov     eax,0F007h                      ; wait for a key
        int     21h
        mov     eax,VB_RESTORE
        int     21h
        mov     edx,OFFSET msg_shown
        call    puts
        call    putmode
        call    crlf
        xor     al,al
        jmp     vt_quit
vs_cannot:
        mov     eax,VB_RESTORE
        int     21h
        mov     edx,OFFSET msg_cannot
        call    puts
        mov     al,1
        jmp     vt_quit

;---------------------------------------------------------------------
; drawline - the scan line [vt_y] of the picture, into BS_LINE.
;---------------------------------------------------------------------
drawline:
        pushad
        mov     edi,OFFSET vt_bss + BS_LINE
        xor     esi,esi                         ; ESI = x
dl_px:
        movzx   eax,word ptr [vt_bss + BS_TMI + VMI_XRES]
        cmp     esi,eax
        jae     dl_out
        ; the frame
        or      esi,esi
        jz      dl_white
        dec     eax
        cmp     esi,eax
        je      dl_white
        mov     eax,dword ptr [vt_y]
        or      eax,eax
        jz      dl_white
        movzx   edx,word ptr [vt_bss + BS_TMI + VMI_YRES]
        dec     edx
        cmp     eax,edx
        je      dl_white
        ; red = x * 256 / width
        mov     eax,esi
        shl     eax,8
        xor     edx,edx
        movzx   ecx,word ptr [vt_bss + BS_TMI + VMI_XRES]
        div     ecx
        mov     dword ptr [vt_r],eax
        mov     ebx,255
        sub     ebx,eax
        mov     dword ptr [vt_b],ebx
        ; green = y * 256 / height
        mov     eax,dword ptr [vt_y]
        shl     eax,8
        xor     edx,edx
        movzx   ecx,word ptr [vt_bss + BS_TMI + VMI_YRES]
        div     ecx
        mov     dword ptr [vt_g],eax
        jmp     dl_pack
dl_white:
        mov     eax,255
        mov     dword ptr [vt_r],eax
        mov     dword ptr [vt_g],eax
        mov     dword ptr [vt_b],eax
dl_pack:
        call    pack                            ; -> EAX
        mov     ecx,dword ptr [vt_bytes]
dl_byte:
        stosb
        shr     eax,8
        loop    dl_byte
        inc     esi
        jmp     dl_px
dl_out:
        popad
        ret

;--- pack - vt_r, vt_g, vt_b (0-255) -> EAX = the pixel ---------------
pack:
        push    ebx
        push    ecx
        cmp     byte ptr [vt_bss + BS_TMI + VMI_BPP],8
        jne     pk_direct
        mov     eax,dword ptr [vt_r]            ; 3-3-2, as pal332 made it
        and     eax,0E0h
        mov     ebx,dword ptr [vt_g]
        and     ebx,0E0h
        shr     ebx,3
        or      eax,ebx
        mov     ebx,dword ptr [vt_b]
        shr     ebx,6
        or      eax,ebx
        jmp     pk_out
pk_direct:
        xor     eax,eax
        mov     ebx,dword ptr [vt_r]
        mov     ch,byte ptr [vt_bss + BS_TMI + VT_RSIZE]
        mov     cl,byte ptr [vt_bss + BS_TMI + VT_RPOS]
        call    pk_one
        mov     ebx,dword ptr [vt_g]
        mov     ch,byte ptr [vt_bss + BS_TMI + VT_GSIZE]
        mov     cl,byte ptr [vt_bss + BS_TMI + VT_GPOS]
        call    pk_one
        mov     ebx,dword ptr [vt_b]
        mov     ch,byte ptr [vt_bss + BS_TMI + VT_BSIZE]
        mov     cl,byte ptr [vt_bss + BS_TMI + VT_BPOS]
        call    pk_one
pk_out:
        pop     ecx
        pop     ebx
        ret
; EBX = 0-255, CH = the field's bits, CL = where it starts: into EAX
pk_one:
        push    ecx
        mov     cl,8
        sub     cl,ch
        shr     ebx,cl
        pop     ecx
        shl     ebx,cl
        or      eax,ebx
        ret

;--- pal332 - an 8-bit mode's palette, 3 bits red, 3 green, 2 blue ----
pal332:
        pushad
        mov     edi,OFFSET vt_bss + BS_PAL
        xor     ecx,ecx
p3_each:
        mov     eax,ecx
        and     eax,3
        imul    eax,21                          ; blue, 0 to 63
        mov     byte ptr [edi+ecx*4],al
        mov     eax,ecx
        shr     eax,2
        and     eax,7
        imul    eax,9                           ; green
        mov     byte ptr [edi+ecx*4+1],al
        mov     eax,ecx
        shr     eax,5
        imul    eax,9                           ; red
        mov     byte ptr [edi+ecx*4+2],al
        mov     byte ptr [edi+ecx*4+3],0
        inc     ecx
        cmp     ecx,256
        jb      p3_each
        mov     eax,VB_PALETTE
        xor     ebx,ebx
        mov     ecx,256
        xor     edx,edx
        int     21h
        popad
        ret

;=====================================================================
; Shared
;=====================================================================

;--- getinfo - VBF_INFO into BS_INFO.  CF and AX = the error. ---------
getinfo:
        push    edi
        mov     eax,VB_INFO
        mov     edi,OFFSET vt_bss + BS_INFO
        int     21h
        pop     edi
        jc      gi_out
        mov     word ptr [vt_ver],ax
        mov     dword ptr [vt_nmodes],ecx
gi_out:
        ret

;--- getmode - EAX = the mode 4F03h says is set, its number only.  CF
;--- if it would not say.
getmode:
        push    ebx
        mov     eax,VB_GETMODE
        int     21h
        jc      gm_out
        movzx   eax,bx
        and     eax,VBM_MODE
gm_out:
        pop     ebx
        ret

;--- selinfo - ECX = SEL_VBE's limit, DL = its access byte.  CF if the
;--- processor will not say.  EDX's other bytes trashed.
selinfo:
        push    eax
        mov     eax,SEL_VBE
        lsl     ecx,eax
        jnz     sl_bad
        lar     edx,eax
        jnz     sl_bad
        shr     edx,8
        pop     eax
        clc
        ret
sl_bad:
        pop     eax
        stc
        ret

;--- fbfill - ECX = bytes: each dword of SEL_VBE written with its own
;--- offset inverted, then read back.  CF if one came back different.
fbfill:
        pushad
        mov     eax,SEL_VBE
        mov     es,ax
        shr     ecx,2
        mov     edx,ecx
        xor     edi,edi
ff_wr:
        mov     eax,edi
        not     eax
        stosd
        dec     ecx
        jnz     ff_wr
        mov     ecx,edx
        xor     edi,edi
ff_rd:
        mov     eax,edi
        not     eax
        scasd
        jne     ff_bad
        dec     ecx
        jnz     ff_rd
        push    ds
        pop     es
        popad
        clc
        ret
ff_bad:
        push    ds
        pop     es
        popad
        stc
        ret

;--- try - INT 21h with the registers as they are.  ZF if it failed with
;--- the error in vt_want.
try:
        int     21h
        jnc     try_no
        cmp     ax,word ptr [vt_want]
        ret
try_no:
        or      esp,esp                         ; not zero: ZF clear
        ret

;--- child - this program again, with the tail at [vt_xtail].  -> AL =
;--- its exit code; CF if it could not be run.
child:
        mov     eax,dword ptr [vt_xtail]
        mov     dword ptr [xblock+4],eax
        mov     edx,OFFSET path_self
        mov     ebx,OFFSET xblock
        mov     ax,4B00h
        int     21h
        jc      ch_out
        mov     ah,4Dh
        int     21h
        clc
ch_out:
        ret

;---------------------------------------------------------------------
; pickmode - BL = 0 for the checks' choice, 1 for the picture's.  The
;            listed mode that suits best into vt_tmode, its 4F01h block
;            into BS_TMI.  CF if none suits at all.
;---------------------------------------------------------------------
pickmode:
        pushad
        mov     byte ptr [vt_pref],bl
        mov     dword ptr [vt_best],-1
        mov     word ptr [vt_tmode],0FFFFh
        mov     esi,dword ptr [vt_bss + BS_INFO + VBI_MODES]
pm_each:
        movzx   ecx,word ptr [esi]
        cmp     ecx,0FFFFh
        je      pm_done
        add     esi,2
        mov     eax,VB_MODE
        mov     edi,OFFSET vt_bss + BS_MI
        int     21h
        jc      pm_each
        mov     bl,byte ptr [vt_pref]
        call    score
        cmp     eax,dword ptr [vt_best]
        jle     pm_each
        mov     dword ptr [vt_best],eax
        mov     word ptr [vt_tmode],cx
        push    esi
        push    ecx
        mov     esi,OFFSET vt_bss + BS_MI
        mov     edi,OFFSET vt_bss + BS_TMI
        mov     ecx,VMI_LEN / 4
        rep     movsd
        pop     ecx
        pop     esi
        jmp     pm_each
pm_done:
        cmp     dword ptr [vt_best],-1
        je      pm_none
        popad
        clc
        ret
pm_none:
        popad
        stc
        ret

;--- score - BS_MI's mode for BL's purpose -> EAX, higher is better, -1
;--- not at all.  A graphics mode with a linear frame buffer, packed or
;--- direct colour, at 8, 15, 16, 24 or 32 bits; the picture's must fit
;--- BS_LINE.
score:
        push    ecx
        push    edx
        mov     eax,-1
        mov     dx,word ptr [vt_bss + BS_MI + VMI_ATTR]
        and     dx,VMA_HW OR VMA_GRAPHICS OR VMA_LFB
        cmp     dx,VMA_HW OR VMA_GRAPHICS OR VMA_LFB
        jne     sc_out
        mov     dl,byte ptr [vt_bss + BS_MI + VMI_MODEL]
        cmp     dl,4
        je      sc_model
        cmp     dl,6
        jne     sc_out
sc_model:
        movzx   edx,byte ptr [vt_bss + BS_MI + VMI_BPP]
        cmp     edx,8
        je      sc_bpp
        cmp     edx,15
        je      sc_bpp
        cmp     edx,16
        je      sc_bpp
        cmp     edx,24
        je      sc_bpp
        cmp     edx,32
        jne     sc_out
sc_bpp:
        movzx   ecx,word ptr [vt_bss + BS_MI + VMI_XRES]
        add     edx,7
        shr     edx,3
        imul    ecx,edx                         ; ECX = one line's bytes
        movzx   edx,byte ptr [vt_bss + BS_MI + VMI_BPP]
        xor     eax,eax
        cmp     word ptr [vt_bss + BS_MI + VMI_XRES],640
        jne     sc_size
        cmp     word ptr [vt_bss + BS_MI + VMI_YRES],480
        jne     sc_size
        mov     eax,100
sc_size:
        cmp     bl,0
        jne     sc_show
        cmp     edx,8                           ; the checks: 8 bits first,
        jne     sc_out                          ; then 640x480
        add     eax,200
        jmp     sc_out
sc_show:
        cmp     ecx,BS_LINELEN                  ; the picture: 640x480, as
        ja      sc_never                        ; many bits as there are
        add     eax,edx
        jmp     sc_out
sc_never:
        mov     eax,-1
sc_out:
        pop     edx
        pop     ecx
        ret

;---------------------------------------------------------------------
; Saying things, into BS_OUT until flush
;---------------------------------------------------------------------
good:
        push    edx
        mov     dl,'.'
        call    tally
        mov     edx,OFFSET msg_ok
        call    puts
        call    written
        pop     edx
        ret
bad:
        push    edx
        mov     dl,'X'
        call    tally
        mov     edx,OFFSET msg_bad
        call    puts
        inc     byte ptr [vt_fails]
        call    written
        pop     edx
        ret
skipped:
        push    edx
        mov     dl,'-'
        call    tally
        mov     edx,OFFSET msg_skip
        call    puts
        call    written
        pop     edx
        ret
tally:
        push    ebx
        mov     ebx,dword ptr [vt_resptr]
        mov     byte ptr [ebx],dl
        inc     ebx
        mov     byte ptr [ebx],'$'
        mov     dword ptr [vt_resptr],ebx
        pop     ebx
        ret

;--- putmode - "0101h: 640x480, 8 bits" for vt_tmode and BS_TMI ------
putmode:
        pushad
        movzx   eax,word ptr [vt_tmode]
        mov     cl,4
        call    puthex
        mov     edx,OFFSET msg_h
        call    puts
        movzx   eax,word ptr [vt_bss + BS_TMI + VMI_XRES]
        call    putdec
        mov     dl,'x'
        call    putc
        movzx   eax,word ptr [vt_bss + BS_TMI + VMI_YRES]
        call    putdec
        mov     edx,OFFSET msg_comma
        call    puts
        movzx   eax,byte ptr [vt_bss + BS_TMI + VMI_BPP]
        call    putdec
        mov     edx,OFFSET msg_bits
        call    puts
        popad
        ret

;--- puts - EDX -> '$'-terminated ------------------------------------
puts:
        push    edx
        push    eax
pts_ch:
        mov     al,byte ptr [edx]
        cmp     al,'$'
        je      pts_out
        push    edx
        mov     dl,al
        call    putc
        pop     edx
        inc     edx
        jmp     pts_ch
pts_out:
        pop     eax
        pop     edx
        ret

;--- putz - ESI -> zero-terminated, at most 64 -------------------------
putz:
        pushad
        mov     ecx,64
pz_ch:
        lodsb
        or      al,al
        jz      pz_out
        mov     dl,al
        call    putc
        loop    pz_ch
pz_out:
        popad
        ret

crlf:
        push    edx
        mov     edx,OFFSET msg_crlf
        call    puts
        pop     edx
        ret

;--- putc - DL into the buffer, which is written out when it fills ----
putc:
        push    eax
        mov     eax,dword ptr [vt_outp]
        cmp     eax,OFFSET vt_bss + BS_OUT + BS_OUTLEN
        jb      pc_room
        call    flush
        mov     eax,dword ptr [vt_outp]
pc_room:
        mov     byte ptr [eax],dl
        inc     eax
        mov     dword ptr [vt_outp],eax
        pop     eax
        ret

;--- written - a check's line is decided: out now, if that is safe ----
written:
        cmp     byte ptr [vt_direct],0
        je      wr_held
        call    flush
        push    eax
        mov     ah,0Dh                          ; and onto the disk: the
        int     21h                             ; cache holds it otherwise
        pop     eax
wr_held:
        ret

;--- flush - the buffer, to standard output --------------------------
flush:
        pushad
        mov     ecx,dword ptr [vt_outp]
        mov     edx,OFFSET vt_bss + BS_OUT
        sub     ecx,edx
        jz      fl_out
        mov     ebx,1
        mov     ah,40h
        int     21h
fl_out:
        mov     dword ptr [vt_outp],OFFSET vt_bss + BS_OUT
        popad
        ret

;--- putdec - EAX in decimal ------------------------------------------
putdec:
        pushad
        mov     ecx,10
        xor     ebx,ebx
pd_div:
        xor     edx,edx
        div     ecx
        push    edx
        inc     ebx
        or      eax,eax
        jnz     pd_div
pd_out:
        pop     edx
        add     dl,'0'
        call    putc
        dec     ebx
        jnz     pd_out
        popad
        ret

;--- puthex - EAX in CL hex digits ------------------------------------
puthex:
        pushad
        movzx   ecx,cl
        mov     ebx,ecx
        shl     ebx,2
        push    ecx
        mov     ecx,32
        sub     ecx,ebx
        shl     eax,cl                          ; the digits to the top
        pop     ecx
ph_dig:
        rol     eax,4
        mov     dl,al
        and     dl,0Fh
        add     dl,'0'
        cmp     dl,'9'
        jbe     ph_put
        add     dl,7
ph_put:
        call    putc
        loop    ph_dig
        popad
        ret

;--- skipblank - past blanks in the tail at ESI, ECX left -------------
skipblank:
        jecxz   sb_out
        cmp     byte ptr [esi],' '
        je      sb_next
        cmp     byte ptr [esi],9
        jne     sb_out
sb_next:
        inc     esi
        dec     ecx
        jmp     skipblank
sb_out:
        ret

;=====================================================================
; Data
;=====================================================================
msg_hi      db  'VBETEST - VBE and the linear frame buffer, for a native program',13,10,13,10,'$'
msg_t1      db  ' 1 VBF_INFO: the VESA block and its mode list ......... ','$'
msg_t2      db  ' 2 VBF_MODE for every mode, and one to test with ...... ','$'
msg_t3      db  ' 3 VBF_SET: SEL_VBE, its limit, and the mode set ...... ','$'
msg_t4      db  ' 4 all of video memory, written and read back ......... ','$'
msg_t5      db  ' 5 4F07h moves the display start, and says so ......... ','$'
msg_t6      db  ' 6 VBF_PALETTE: 256 entries set and read back ......... ','$'
msg_t7      db  ' 7 bad requests refused, with the right errors ........ ','$'
msg_t8      db  ' 8 a child',39,'s mode 13h, and this one',39,'s back after it ... ','$'
msg_t9      db  ' 9 VGA mode 13h: the 64K window at A0000h ............. ','$'
msg_t10     db  '10 VBF_RESTORE: the first mode back, SEL_VBE empty .... ','$'
msg_t11     db  '11 a child leaves a mode set: the first one back ...... ','$'
msg_t12     db  '12 a child faults in its mode: the first one back ..... ','$'
msg_vbe     db  '     VBE ','$'
msg_comma   db  ', ','$'
msg_kcomma  db  'K, ','$'
msg_kcolon  db  'K: ','$'
msg_modes   db  ' modes: ','$'
msg_testing db  '     testing mode ','$'
msg_nolfb   db  '     no mode with a linear frame buffer to test',13,10,'$'
msg_refn    db  '     the BIOS will not describe ','$'
msg_refof   db  ' of its ','$'
msg_refm    db  ' modes',13,10,'$'
msg_vbe1    db  '     VBE 1.x: no linear frame buffer before VBE 2.0',13,10,'$'
msg_novbe   db  'no VBE',13,10,13,10,'VBETEST: this BIOS has no VBE, so nothing was tested',13,10,'$'
msg_novbe2  db  'This BIOS has no VBE.',13,10,'$'
msg_vbe2    db  'VBE ','$'
msg_h       db  'h: ','$'
msg_h2      db  'h  ','$'
msg_bits    db  ' bits','$'
msg_text    db  'text ','$'
msg_lfbat   db  '  frame buffer at ','$'
msg_banked  db  '  banked only','$'
msg_nohw    db  'not on this hardware','$'
msg_refused db  'the BIOS will not describe it','$'
msg_noshow  db  'No mode with a linear frame buffer to show.',13,10,'$'
msg_cannot  db  'That mode cannot be shown.',13,10,'$'
msg_shown   db  'Shown: mode ','$'
msg_skip    db  'skipped',13,10,'$'
msg_ok      db  'ok',13,10,'$'
msg_bad     db  'FAILED',13,10,'$'
msg_res     db  13,10,'Result: ','$'
msg_crlf    db  13,10,'$'
msg_pass    db  'VBETEST: all checks passed',13,10,'$'
msg_fail    db  'VBETEST: FAILURES',13,10,'$'
msg_usage   db  'Tests VBE and the linear frame buffer.',13,10,13,10
            db  'VBETEST [/L | /S [mode]]',13,10,13,10
            db  '  /L    Lists the modes the BIOS has.',13,10
            db  '  /S    Shows a picture until a key is pressed, in the mode given',13,10
            db  '        (in hex) or in the best 640x480 mode.',13,10,'$'

; the far pointers 1 checks, as offsets in the block
t1_ptrs     db  VBI_OEMSTR, VBI_MODES, VBI_VENDOR, VBI_PRODUCT, VBI_PRODREV, 0

path_self   db  'C:\TESTS\VBETEST.EXE',0
xblock      dd  0                       ; no environment
            dd  0                       ; the tail: vt_xtail
tail_x      db  3,' /X',0Dh
tail_f      db  3,' /F',0Dh
tail_n      db  3,' /N',0Dh

vt_outp     dd  0
vt_resptr   dd  OFFSET vt_results
vt_xtail    dd  0
vt_nmodes   dd  0
vt_refused  dd  0
vt_start    dd  0
vt_size     dd  0
vt_pitch    dd  0
vt_best     dd  0
vt_bytes    dd  0
vt_y        dd  0
vt_r        dd  0
vt_g        dd  0
vt_b        dd  0
vt_ver      dw  0
vt_tmode    dw  0FFFFh
vt_want     dw  0
vt_fails    db  0
vt_nolfb    db  0
vt_set      db  0
vt_pref     db  0
vt_direct   db  0                       ; standard output is a file
vt_results  db  16 dup('$')

vt_bss      LABEL   BYTE

_VT     ENDS
        END
