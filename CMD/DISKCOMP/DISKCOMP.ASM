;=====================================================================
; DISKCOMP.ASM - DISKCOMP.COM, two diskettes compared track for track
;
;   DISKCOMP [drive1: [drive2:]] [/1] [/8]
;
; MS-DOS 6.22's DISKCOMP - its prompts, its messages, its errorlevels -
; done in ONE PASS: the whole first disk is read into memory and the
; second is compared with it as it is read, so with one drive there is
; one swap of disks where DOS asked for two or more.  The image is held
; where INC\DISKIMG.INC keeps it, in extended memory the kernel holds
; for the program; a machine without enough of it is compared in
; passes, as DOS did.
;
;   /1   side 0 only
;   /8   the first eight sectors of each track only
;
; Every track that differs is named ("Compare error on side 0, track
; 5") and the comparison carries on to the end; "Compare OK" only if
; none did and every track could be read.  A disk and its DISKCOPY
; differ on track 0, where the copy was given a serial number of its
; own - DOS's DISKCOMP said so too.
;
; Errorlevels, 6.22's: 0 the same, 1 different, 2 ^C, 3 a track that
; could not be read, 4 the command line, the drives, memory, or disks
; of two different shapes.  The last comparison's when there are more.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_DCOMP  SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_DCOMP, DS:_DCOMP, ES:_DCOMP, SS:_DCOMP
        ORG     0

;=====================================================================
; D32 header - two track buffers in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET dk_entry>,<DI_BSS>,8192

; the engine first: its equates are used below, and wasm will not take
; an EQU it has not met yet
        INCLUDE DISKIMG.INC

;=====================================================================
dk_entry:
        call    di_init
        call    di_parse
        call    di_drives

dk_again:
        mov     byte ptr [dk_rc],DI_RC_OK
        mov     byte ptr [dk_diff],0
        mov     byte ptr [dk_rderr],0
        mov     edi,OFFSET dk_bad
        mov     ecx,DI_MAXTRK
        xor     al,al
        rep     stosb

        mov     edx,OFFSET m_in1
        mov     al,[di_l1]
        call    say_drive
        cmp     byte ptr [di_one],0
        jne     dk_key
        mov     edx,OFFSET m_in2
        mov     al,[di_l2]
        call    say_drive
dk_key:
        call    di_anykey

;--- the first disk's shape is the one compared ----------------------
        mov     al,[di_bios1]
        call    di_shape
        jc      dk_bad1
        mov     ah,[di_type2]
        call    di_fits
        jc      dk_incompat
        mov     al,[sw_one]
        mov     [di_sides],al
        xor     al,al
        cmp     byte ptr [sw_eight],0
        je      dk_nsec
        mov     al,8
dk_nsec:
        mov     [di_nsec],al
        call    di_layout

        mov     edx,OFFSET m_comparing
        call    di_puts
        mov     eax,[g_cyls]
        call    di_pdec
        mov     edx,OFFSET m_tracks
        call    di_puts
        movzx   eax,byte ptr [di_nsec]
        call    di_pdec
        mov     edx,OFFSET m_spt
        call    di_puts
        movzx   eax,byte ptr [di_sides]
        call    di_pdec
        mov     edx,OFFSET m_sides
        call    di_puts

        cmp     byte ptr [di_one],0     ; two drives: the second disk is
        jne     dk_mem                  ; already there to be looked at
        call    dk_second
        jc      dk_ended
dk_mem:
        call    di_imgopen
        jc      dk_nomem
        mov     dword ptr [dk_t0],0
        mov     byte ptr [dk_first],1

;--- a pass: as much of the first disk as the image holds, and the
;--- same tracks of the second compared with it
dk_pass:
        mov     eax,[g_tracks]
        sub     eax,[dk_t0]
        cmp     eax,[di_cap]
        jbe     dk_n
        mov     eax,[di_cap]
dk_n:
        mov     [dk_n1],eax
        call    dk_rdpass

        cmp     byte ptr [di_one],0
        je      dk_cmp
        mov     edx,OFFSET m_in2
        mov     al,[di_l2]
        call    say_drive
        call    di_anykey
        cmp     byte ptr [dk_first],0
        je      dk_cmp
        call    dk_second
        jc      dk_ended
dk_cmp:
        mov     byte ptr [dk_first],0
        call    dk_cmppass

        mov     eax,[dk_n1]
        add     [dk_t0],eax
        mov     eax,[dk_t0]
        cmp     eax,[g_tracks]
        jae     dk_whole
        cmp     byte ptr [di_one],0
        je      dk_pass
        mov     edx,OFFSET m_in1
        mov     al,[di_l1]
        call    say_drive
        call    di_anykey
        jmp     dk_pass

dk_whole:
        cmp     byte ptr [dk_rderr],0
        jne     dk_rc3
        cmp     byte ptr [dk_diff],0
        jne     dk_rc1
        mov     edx,OFFSET m_ok
        call    di_puts
        jmp     short dk_another
dk_rc3:
        mov     byte ptr [dk_rc],DI_RC_HARD
        jmp     short dk_another
dk_rc1:
        mov     byte ptr [dk_rc],DI_RC_SOFT
        jmp     short dk_another

;--- the ways a comparison stops short -------------------------------
dk_bad1:
        mov     edx,OFFSET m_bad1
        call    di_puts
        mov     byte ptr [dk_rc],DI_RC_HARD
        jmp     short dk_ended
dk_incompat:
        mov     edx,OFFSET m_incompat
        call    di_puts
        mov     byte ptr [dk_rc],DI_RC_INIT
dk_ended:
        mov     edx,OFFSET m_ended
        call    di_puts
        cmp     byte ptr [dk_rc],DI_RC_OK
        jne     dk_another
        mov     byte ptr [dk_rc],DI_RC_SOFT

dk_another:
        call    di_imgclose
        mov     edx,OFFSET m_another
        call    di_yesno
        cmp     al,'Y'
        je      dk_again
        mov     al,[dk_rc]
        jmp     di_exit

dk_nomem:
        mov     edx,OFFSET m_nomem
        call    di_puts
        mov     al,DI_RC_INIT
        jmp     di_exit

;=====================================================================
; dk_second - the second disk has to be the first one's shape.  CF,
;             with what was wrong already said and [dk_rc] set, if it
;             is not.  g_* are the first disk's again afterwards.
;=====================================================================
dk_second:
        mov     esi,OFFSET g_spt        ; g_spt..g_total, then g_row
        mov     edi,OFFSET dk_keep
        mov     ecx,4
        rep     movsd
        mov     al,[g_row]
        mov     [dk_keeprow],al

        mov     al,[di_bios2]
        call    di_shape
        jnc     dk2_shaped
        or      ax,ax                   ; it read, and says nothing: it is
        jnz     dk2_bad                 ; compared as the first disk's
        call    dk2_back                ; shape, as 6.22 compared a blank
        clc                             ; disk - track by track, all
        ret                             ; different
dk2_shaped:
        mov     esi,OFFSET g_spt
        mov     edi,OFFSET dk_keep
        mov     ecx,4
        repe    cmpsd
        jne     dk2_other
        call    dk2_back
        clc
        ret
dk2_bad:
        call    dk2_back
        mov     edx,OFFSET m_bad2
        call    di_puts
        mov     byte ptr [dk_rc],DI_RC_INIT
        stc
        ret
dk2_other:
        call    dk2_back
        mov     edx,OFFSET m_incompat
        call    di_puts
        mov     byte ptr [dk_rc],DI_RC_INIT
        stc
        ret
dk2_back:
        mov     esi,OFFSET dk_keep
        mov     edi,OFFSET g_spt
        mov     ecx,4
        rep     movsd
        mov     al,[dk_keeprow]
        mov     [g_row],al
        ret

;=====================================================================
; dk_rdpass - [dk_n1] tracks of the first disk from [dk_t0] into the
;             image.  One that will not read is said so and marked,
;             and is not compared.
;=====================================================================
dk_rdpass:
        mov     al,[di_bios1]
        call    di_settle
        mov     dword ptr [dk_i],0
krp_lp:
        mov     eax,[dk_i]
        cmp     eax,[dk_n1]
        jae     krp_out
        mov     edx,[dk_t0]
        add     edx,eax
        mov     [dk_t],edx
        mov     al,[di_bios1]
        xor     ah,ah
        mov     edi,[di_trk]
        call    di_track
        jnc     krp_got
        mov     al,[di_l1]
        call    dk_rdfail
krp_got:
        mov     eax,[dk_i]
        mov     esi,[di_trk]
        call    di_imgput
        inc     dword ptr [dk_i]
        jmp     short krp_lp
krp_out:
        ret

;=====================================================================
; dk_cmppass - the same tracks of the second disk, each compared with
;              the image as it comes in
;=====================================================================
dk_cmppass:
        mov     al,[di_bios2]
        call    di_settle
        mov     dword ptr [dk_i],0
kcp_lp:
        mov     eax,[dk_i]
        cmp     eax,[dk_n1]
        jae     kcp_out
        mov     edx,[dk_t0]
        add     edx,eax
        mov     [dk_t],edx
        mov     al,[di_bios2]
        xor     ah,ah
        mov     edi,[di_trk2]
        call    di_track
        jnc     kcp_got
        mov     al,[di_l2]
        call    dk_rdfail
        jmp     short kcp_next
kcp_got:
        mov     edx,[dk_t]
        cmp     byte ptr [dk_bad+edx],0
        jne     kcp_next                ; the first disk's would not read
        mov     eax,[dk_i]
        mov     edi,[di_trk]
        call    di_imgget
        mov     esi,[di_trk]
        mov     edi,[di_trk2]
        mov     ecx,[di_tbytes]
        shr     ecx,2
        repe    cmpsd
        je      kcp_next
        mov     byte ptr [dk_diff],1
        mov     edx,OFFSET m_cmperr
        call    di_puts
        mov     eax,[dk_t]
        call    say_where
kcp_next:
        inc     dword ptr [dk_i]
        jmp     short kcp_lp
kcp_out:
        ret

; dk_rdfail - AL = the letter, track [dk_t] would not read
dk_rdfail:
        mov     edx,OFFSET m_rderr
        call    di_puts
        call    di_pchar
        mov     edx,OFFSET m_colcrlf
        call    di_puts
        mov     eax,[dk_t]
        mov     byte ptr [dk_bad+eax],1
        mov     byte ptr [dk_rderr],1
        ; fall into

; say_where - EAX = a track: "side 0, track 5" and CR LF
say_where:
        call    di_trkpos               ; ECX = cylinder, EDX = side
        push    ecx
        push    edx
        mov     edx,OFFSET m_side
        call    di_puts
        pop     eax
        call    di_pdec
        mov     edx,OFFSET m_track
        call    di_puts
        pop     eax
        call    di_pdec
        jmp     di_crlf

; say_drive - EDX -> "...drive ", AL = the letter: and ":" CR LF
say_drive:
        call    di_puts
        call    di_pchar
        mov     edx,OFFSET m_colcrlf
        jmp     di_puts

;=====================================================================
; /?
;=====================================================================
p_help:
        mov     edx,OFFSET m_help
        call    di_puts
        mov     ax,4C00h
        int     21h

;=====================================================================
; Data
;=====================================================================
; for DISKIMG.INC
p_swtab     db  '1'
            dd  OFFSET sw_one
            db  '8'
            dd  OFFSET sw_eight
            db  0
p_slotonly  db  0                       ; never: no /M here
sw_one      db  0                       ; /1
sw_eight    db  0                       ; /8

dk_rc       db  0
dk_diff     db  0                       ; a track differed
dk_rderr    db  0                       ; a track would not read
dk_first    db  0                       ; the second disk not looked at
dk_keeprow  db  0
            db  0,0,0
dk_keep     dd  4 dup(0)                ; the first disk's g_*
dk_t0       dd  0
dk_n1       dd  0
dk_i        dd  0
dk_t        dd  0
dk_bad      db  DI_MAXTRK dup(0)        ; tracks of the first that failed

p_usage     db  'Do not specify filename(s)',13,10
            db  'Command format: DISKCOMP [drive1: [drive2:]] [/1] [/8]',13,10,0
p_baddrv    db  13,10,'Invalid drive specification',13,10
            db  'Specified drive does not exist',13,10
            db  'or is non-removable',13,10,0

m_in1       db  13,10,'Insert FIRST diskette in drive ',0
m_in2       db  13,10,'Insert SECOND diskette in drive ',0
m_colcrlf   db  ':',13,10,0
m_comparing db  13,10,'Comparing ',0
m_tracks    db  ' tracks',13,10,0
m_spt       db  ' sectors per track, ',0
m_sides     db  ' side(s)',13,10,0
m_cmperr    db  13,10,'Compare error on',13,10,0
m_rderr     db  13,10,'Unrecoverable read error on drive ',0
m_side      db  'side ',0
m_track     db  ', track ',0
m_ok        db  13,10,'Compare OK',13,10,0
m_another   db  13,10,'Compare another diskette (Y/N) ?',0
m_bad1      db  13,10,'FIRST diskette bad or incompatible',13,10,0
m_bad2      db  13,10,'SECOND diskette bad or incompatible',13,10,0
m_incompat  db  13,10,'Drive types or diskette types',13,10
            db  'not compatible',13,10,0
m_ended     db  13,10,'Compare process ended',13,10,0
m_nomem     db  13,10,'Insufficient memory',13,10,0
m_help      db  'Compares two diskettes, track for track.',13,10,13,10
            db  'DISKCOMP [drive1: [drive2:]] [/1] [/8]',13,10,13,10
            db  '  /1   Compares side 0 only.',13,10
            db  '  /8   Compares only the first eight sectors of each track.',13,10,13,10
            db  'drive1 and drive2 may be the same drive: the first diskette is',13,10
            db  'read in one go, and the second compared with it.',13,10,0

img_end:                                ; the BSS begins here

_DCOMP  ENDS
        END
