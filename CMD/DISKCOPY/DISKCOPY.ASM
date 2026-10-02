;=====================================================================
; DISKCOPY.ASM - DISKCOPY.COM, a diskette copied track for track
;
;   DISKCOPY [drive1: [drive2:]] [/1] [/V] [/M]
;
; MS-DOS 6.22's DISKCOPY - its prompts, its messages, its errorlevels -
; with the copy done in ONE PASS whatever the machine: the whole source
; is read into memory, and then the whole of it is written.  With one
; drive that is one swap of disks where DOS asked for three or four,
; and once it is in memory it can be written to as many disks as are
; wanted ("Do you wish to write another duplicate"), as 6.22 offered
; when it could hold a disk in a file on the hard disk.  Here it is
; held in extended memory the kernel keeps for the program (see
; INC\DISKIMG.INC); a machine without enough of it is copied in
; passes, as DOS did.
;
;   /1   side 0 only
;   /V   every track is read back and compared once written
;   /M   6.22's "memory only": the image stays in the program's own
;        heap (48h).  That meant passes while a slot was 512K; 48h
;        grows the slot now, so on most machines it is one pass
;
; THE TARGET IS FORMATTED WHEN IT HAS TO BE.  Its boot sector is read
; before anything is written: a disk of the same shape is written as
; it is; one with no format, or another format of the same kind of
; disk, is formatted first ("Formatting while copying"); a disk of
; another density - a 1.44M disk formatted as one, under a 720K
; source - is refused, as DOS refused it, because the format would not
; be reliable.  A write that fails on a disk that looked formatted
; formats from that track on and tries again.
;
; THE COPY GETS A SERIAL NUMBER OF ITS OWN, as DOS has given every copy
; since 4.0, when the source's boot sector has an extended BPB.  So
; DISKCOMP of a disk and its copy finds track 0 different, which is
; also what DOS did.
;
; Errorlevels, 6.22's: 0 copied, 1 a read or write error (the copy is
; finished as well as it can be), 2 ^C, 3 a source that could not be
; read, 4 the command line, the drives, memory, or disks that do not
; go together.  They are the last copy's when more than one is made.
;
; Linked with:  wlink form raw bin option offset=0x2000
;=====================================================================
        .386
        INCLUDE PMDOS.INC

_DCOPY  SEGMENT USE32 PUBLIC 'CODE'
        ASSUME  CS:_DCOPY, DS:_DCOPY, ES:_DCOPY, SS:_DCOPY
        ORG     0

;=====================================================================
; D32 header - two track buffers in the BSS
;=====================================================================
        D32_HEAD SLOT_IMAGE,<OFFSET dc_entry>,<DI_BSS>,8192

; the engine first: its equates are used below, and wasm will not take
; an EQU it has not met yet
        INCLUDE DISKIMG.INC

;=====================================================================
dc_entry:
        call    di_init
        call    di_parse
        call    di_drives

dc_again:
        mov     byte ptr [dc_rc],DI_RC_OK
        mov     byte ptr [dc_wrerr],0
        mov     byte ptr [dc_fmt],0
        mov     byte ptr [dc_ser],0

        mov     edx,OFFSET m_insrc
        mov     al,[di_l1]
        call    say_drive
        cmp     byte ptr [di_one],0
        jne     dc_key
        mov     edx,OFFSET m_instgt
        mov     al,[di_l2]
        call    say_drive
dc_key:
        call    di_anykey

;--- what is being copied --------------------------------------------
        mov     al,[di_bios1]
        call    di_shape
        jc      dc_badsrc
        mov     ah,[di_type2]           ; and can the other drive take it?
        call    di_fits
        jc      dc_incompat
        mov     al,[sw_one]             ; /1: one side
        mov     [di_sides],al
        mov     byte ptr [di_nsec],0
        call    di_layout

        mov     edx,OFFSET m_copying
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

        call    di_imgopen
        jc      dc_nomem
        mov     dword ptr [dc_t0],0
        mov     byte ptr [dc_first],1

;--- a pass: as much of the source as the image holds ----------------
dc_pass:
        mov     eax,[g_tracks]
        sub     eax,[dc_t0]
        cmp     eax,[di_cap]
        jbe     dc_n
        mov     eax,[di_cap]
dc_n:
        mov     [dc_n1],eax
        cmp     byte ptr [di_one],0
        je      dc_rd
        mov     edx,OFFSET m_reading
        call    di_puts
dc_rd:
        call    dc_rdpass
        jc      dc_badsrc

        cmp     byte ptr [di_one],0
        je      dc_wrgo
        mov     edx,OFFSET m_instgt
        mov     al,[di_l2]
        call    say_drive
        call    di_anykey
dc_wrsay:
        cmp     byte ptr [di_one],0
        je      dc_wrgo
        mov     edx,OFFSET m_writing
        call    di_puts
dc_wrgo:
        cmp     byte ptr [dc_first],0
        je      dc_wrpass1
        mov     byte ptr [dc_first],0
        call    dc_target
        jc      dc_incompat
dc_wrpass1:
        call    dc_wrpass
        jc      dc_ended

        mov     eax,[dc_n1]
        add     [dc_t0],eax
        mov     eax,[dc_t0]
        cmp     eax,[g_tracks]
        jae     dc_whole
        cmp     byte ptr [di_one],0     ; more to come: the source again
        je      dc_pass
        mov     edx,OFFSET m_insrc
        mov     al,[di_l1]
        call    say_drive
        call    di_anykey
        jmp     dc_pass

;--- all of it written.  With one drive and all of it in memory, more
;--- copies of it cost a disk each
dc_whole:
        cmp     byte ptr [di_one],0
        je      dc_done
        mov     eax,[di_cap]
        cmp     eax,[g_tracks]
        jb      dc_done
        mov     edx,OFFSET m_dup
        call    di_yesno
        cmp     al,'Y'
        jne     dc_done
        mov     edx,OFFSET m_instgt
        mov     al,[di_l2]
        call    say_drive
        call    di_anykey
        mov     dword ptr [dc_t0],0
        mov     byte ptr [dc_first],1
        mov     byte ptr [dc_fmt],0
        jmp     dc_wrsay

dc_done:
        cmp     byte ptr [dc_wrerr],0
        je      dc_noerr
        mov     edx,OFFSET m_mayunus
        call    di_puts
dc_noerr:
        cmp     byte ptr [dc_ser],0
        je      dc_another
        mov     edx,OFFSET m_serial
        call    di_puts
        mov     eax,[dc_serno]
        shr     eax,16
        call    di_phex4
        mov     al,'-'
        call    di_pchar
        mov     eax,[dc_serno]
        call    di_phex4
        call    di_crlf
        jmp     short dc_another

;--- the ways a copy stops short -------------------------------------
dc_badsrc:
        mov     edx,OFFSET m_badsrc
        call    di_puts
        mov     byte ptr [dc_rc],DI_RC_HARD
        jmp     short dc_ended
dc_incompat:
        mov     edx,OFFSET m_incompat
        call    di_puts
        cmp     byte ptr [dc_rc],DI_RC_OK
        jne     dc_ended
        mov     byte ptr [dc_rc],DI_RC_INIT
dc_ended:
        mov     edx,OFFSET m_ended
        call    di_puts
        cmp     byte ptr [dc_rc],DI_RC_OK
        jne     dc_another
        mov     byte ptr [dc_rc],DI_RC_SOFT

dc_another:
        call    di_imgclose
        mov     edx,OFFSET m_another
        call    di_yesno
        cmp     al,'Y'
        je      dc_again
        mov     al,[dc_rc]
        jmp     di_exit

dc_nomem:
        mov     edx,OFFSET m_nomem
        call    di_puts
        mov     al,DI_RC_INIT
        jmp     di_exit

;=====================================================================
; dc_rdpass - [dc_n1] tracks from [dc_t0] into the image.  CF if track
;             0 cannot be read: there is no copying a disk without it.
;=====================================================================
dc_rdpass:
        mov     al,[di_bios1]
        call    di_settle
        mov     dword ptr [dc_i],0
drp_lp:
        mov     eax,[dc_i]
        cmp     eax,[dc_n1]
        jae     drp_out
        mov     edx,[dc_t0]
        add     edx,eax
        mov     [dc_t],edx
        mov     al,[di_bios1]
        xor     ah,ah
        mov     edi,[di_trk]
        call    di_track
        jnc     drp_got
        cmp     dword ptr [dc_t],0
        je      drp_bad
        mov     edx,OFFSET m_rderr
        mov     al,[di_l1]
        mov     ebx,[dc_t]
        call    say_trackerr
        mov     byte ptr [dc_rc],DI_RC_SOFT
        mov     edi,[di_trk]            ; what could not be read goes
        mov     ecx,[di_tbytes]         ; over as zeroes
        shr     ecx,2
        xor     eax,eax
        rep     stosd
drp_got:
        cmp     dword ptr [dc_t],0
        jne     drp_keep
        call    dc_serial
drp_keep:
        mov     eax,[dc_i]
        mov     esi,[di_trk]
        call    di_imgput
        inc     dword ptr [dc_i]
        jmp     short drp_lp
drp_out:
        clc
        ret
drp_bad:
        stc
        ret

;---------------------------------------------------------------------
; dc_serial - the boot sector in [di_trk] gets a serial number of its
;             own if it has a place for one: an extended BPB.  The
;             clock makes it, as FORMAT makes one.
;---------------------------------------------------------------------
dc_serial:
        mov     esi,[di_trk]
        cmp     byte ptr [esi+26h],29h
        jne     dse_out
        mov     ah,2Ch
        int     21h
        mov     ebx,ecx
        shl     ebx,16
        mov     bx,dx
        mov     ah,2Ah
        int     21h
        shl     ecx,16
        mov     cx,dx
        add     ebx,ecx
        mov     esi,[di_trk]
        mov     [esi+27h],ebx
        mov     [dc_serno],ebx
        mov     byte ptr [dc_ser],1
dse_out:
        ret

;=====================================================================
; dc_target - the disk about to be written: as it is, formatted first,
;             or refused (CF) because it is another kind of disk.
;=====================================================================
dc_target:
        movzx   edx,byte ptr [di_bios2]
        mov     eax,(FDF_FORGET SHL 16) OR 0F114h
        int     21h
        mov     al,[di_bios2]
        xor     ah,ah
        xor     ebx,ebx
        mov     ecx,1
        mov     edi,[di_trk2]
        call    di_io
        jc      dtg_format              ; it will not read: no format
        mov     esi,[di_trk2]
        call    di_bpbof
        jc      dtg_format              ; no BPB: nothing to keep
        cmp     eax,[g_spt]
        jne     dtg_other
        cmp     ebx,[g_heads]
        jne     dtg_other
        cmp     ecx,[g_total]
        jne     dtg_other
        clc                             ; the same shape: written as is
        ret
dtg_other:
        ; another format.  The same kind of disk takes the source's;
        ; another density is not formatted over
        call    di_rowof
        cmp     al,0FFh
        je      dtg_format
        movzx   esi,byte ptr [g_row]
        cmp     esi,0FFh
        je      dtg_format              ; refused below if it matters
        movzx   eax,al
        mov     al,[di_sizes+eax*DIS_LEN+DIS_MEDIUM]
        cmp     al,[di_sizes+esi*DIS_LEN+DIS_MEDIUM]
        jne     dtg_no
dtg_format:
        cmp     byte ptr [g_row],0FFh   ; only a standard shape can be
        je      dtg_no                  ; laid down
        mov     edx,OFFSET m_fmting
        call    di_puts
        xor     eax,eax
        call    dc_format
        clc
        ret
dtg_no:
        stc
        ret

;=====================================================================
; dc_format - EAX = a track.  Every track from that one's cylinder to
;             the end, both sides, laid down and verified (FDF_TRACK),
;             in the shape of di_sizes row [g_row].  A track that will
;             not format is left to its write to report.
;=====================================================================
dc_format:
        mov     byte ptr [dc_fmt],1
        call    di_trkpos
        mov     [df_cyl],ecx
dfm_begin:
        movzx   edx,byte ptr [di_bios2]
        mov     cl,byte ptr [g_spt]
        mov     ch,byte ptr [g_cyls]
        movzx   esi,byte ptr [g_row]
        movzx   ebx,byte ptr [di_sizes+esi*DIS_LEN+DIS_MEDIUM]
        mov     eax,(FDF_BEGIN SHL 16) OR 0F114h
        int     21h
        jnc     dfm_open
        cmp     al,80h                  ; nothing in the drive
        jne     dfm_out                 ; anything else: the writes will
        call    dfm_notrdy              ; have to say what
        jmp     short dfm_begin
dfm_open:
        mov     al,[di_bios2]
        mov     [di_fend],al
dfm_cyl:
        mov     eax,[df_cyl]
        cmp     eax,[g_cyls]
        jae     dfm_end
        mov     dword ptr [df_head],0
dfm_head:
        mov     eax,[df_head]
        cmp     eax,[g_heads]
        jae     dfm_nextcyl
dfm_try:
        call    di_chkbrk
        movzx   edx,byte ptr [di_bios2]
        mov     ch,byte ptr [df_cyl]
        mov     dh,byte ptr [df_head]
        mov     cl,byte ptr [g_spt]
        mov     eax,(FDF_TRACK SHL 16) OR 0F114h
        int     21h
        jnc     dfm_nexthead
        cmp     al,03h                  ; write protected
        jne     dfm_nwp
        mov     edx,OFFSET di_m_wprot
        call    di_puts
        mov     edx,OFFSET di_m_correct
        call    di_puts
        call    di_getkey
        jmp     short dfm_try
dfm_nwp:
        cmp     al,80h
        jne     dfm_nexthead            ; a bad track: its write will say
        call    dfm_notrdy
        jmp     short dfm_try
dfm_nexthead:
        inc     dword ptr [df_head]
        jmp     short dfm_head
dfm_nextcyl:
        inc     dword ptr [df_cyl]
        jmp     short dfm_cyl
dfm_end:
        movzx   edx,byte ptr [di_bios2]
        mov     eax,(FDF_END SHL 16) OR 0F114h
        int     21h
        mov     byte ptr [di_fend],0FFh
        mov     al,[di_bios2]           ; and the block layer is told the
        call    di_settle               ; shape the disk now has
dfm_out:
        ret

dfm_notrdy:
        mov     edx,OFFSET di_m_notrdy
        call    di_puts
        mov     edx,OFFSET di_m_correct
        call    di_puts
        jmp     di_getkey

;=====================================================================
; dc_wrpass - the image's [dc_n1] tracks onto the target from [dc_t0].
;             CF if track 0 could not be written: the copy has no
;             boot sector and no FAT, and is not a disk at all.
;=====================================================================
dc_wrpass:
        mov     al,[di_bios2]
        call    di_settle
        mov     dword ptr [dc_i],0
dwp_lp:
        mov     eax,[dc_i]
        cmp     eax,[dc_n1]
        jae     dwp_out
        mov     edi,[di_trk]
        call    di_imgget
        mov     edx,[dc_t0]
        add     edx,eax
        mov     [dc_t],edx
        call    dc_wrtrack
        jc      dwp_bad
        inc     dword ptr [dc_i]
        jmp     short dwp_lp
dwp_out:
        clc
        ret
dwp_bad:
        mov     edx,OFFSET m_unusable   ; errorlevel 1, the write error's
        call    di_puts
        stc
        ret

;---------------------------------------------------------------------
; dc_wrtrack - track [dc_t], held in [di_trk].  A failure on a disk
;              that has not been formatted here is a reason to format
;              it; on one that has, it is reported.  CF only for a
;              track 0 that will not take.
;---------------------------------------------------------------------
dc_wrtrack:
        mov     al,[di_bios2]
        mov     ah,1
        mov     edx,[dc_t]
        mov     edi,[di_trk]
        call    di_track
        jc      dwt_fail
        cmp     byte ptr [sw_ver],0
        je      dwt_ok
        mov     al,[di_bios2]           ; /V: read it back, compare
        xor     ah,ah
        mov     edx,[dc_t]
        mov     edi,[di_trk2]
        call    di_track
        jc      dwt_fail
        mov     esi,[di_trk]
        mov     edi,[di_trk2]
        mov     ecx,[di_tbytes]
        shr     ecx,2
        repe    cmpsd
        jne     dwt_fail
dwt_ok:
        clc
        ret
dwt_fail:
        cmp     byte ptr [dc_fmt],0
        jne     dwt_bad
        cmp     byte ptr [g_row],0FFh
        je      dwt_bad                 ; no format to lay down
        mov     edx,OFFSET m_fmting
        call    di_puts
        mov     eax,[dc_t]
        call    dc_format
        jmp     short dc_wrtrack
dwt_bad:
        mov     edx,OFFSET m_wrerr
        mov     al,[di_l2]
        mov     ebx,[dc_t]
        call    say_trackerr
        mov     byte ptr [dc_wrerr],1
        mov     byte ptr [dc_rc],DI_RC_SOFT
        cmp     dword ptr [dc_t],0
        jne     dwt_ok
        stc
        ret

;=====================================================================
; Messages with a part of their own in them
;=====================================================================
; say_drive - EDX -> "...drive ", AL = the letter: and ":" CR LF
say_drive:
        call    di_puts
        call    di_pchar
        mov     edx,OFFSET m_colcrlf
        jmp     di_puts

; say_trackerr - EDX -> "...on drive ", AL = the letter, EBX = the
;                track: "A: Side 0, track 5"
say_trackerr:
        push    ebx
        call    di_puts
        call    di_pchar
        mov     edx,OFFSET m_side
        call    di_puts
        pop     eax
        call    di_trkpos               ; ECX = cylinder, EDX = side
        push    ecx
        mov     eax,edx
        call    di_pdec
        mov     edx,OFFSET m_track
        call    di_puts
        pop     eax
        call    di_pdec
        jmp     di_crlf

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
            db  'V'
            dd  OFFSET sw_ver
            db  'M'
            dd  OFFSET p_slotonly
            db  0
p_slotonly  db  0                       ; /M
sw_one      db  0                       ; /1
sw_ver      db  0                       ; /V

dc_rc       db  0
dc_wrerr    db  0                       ; a track could not be written
dc_fmt      db  0                       ; the target has been formatted
dc_first    db  0                       ; the target not yet looked at
dc_ser      db  0                       ; a new serial number was made
            db  0,0,0
dc_serno    dd  0
dc_t0       dd  0                       ; the first track of this pass
dc_n1       dd  0                       ; and how many it has
dc_i        dd  0
dc_t        dd  0
df_cyl      dd  0
df_head     dd  0

p_usage     db  'Do not specify filename(s)',13,10
            db  'Command Format: DISKCOPY [drive1: [drive2:]] [/1] [/V] [/M]',13,10,0
p_baddrv    db  13,10,'Invalid drive specification.',13,10
            db  'Specified drive does not exist or is non-removable. ',13,10,0

m_insrc     db  13,10,'Insert SOURCE diskette in drive ',0
m_instgt    db  13,10,'Insert TARGET diskette in drive ',0
m_colcrlf   db  ':',13,10,0
m_copying   db  13,10,'Copying ',0
m_tracks    db  ' tracks, ',0
m_spt       db  ' sectors per track, ',0
m_sides     db  ' side(s)',13,10,0
m_reading   db  13,10,'Reading from source diskette . . .',13,10,0
m_writing   db  13,10,'Writing to target diskette . . .',13,10,0
m_fmting    db  13,10,'Formatting while copying',13,10,0
m_dup       db  13,10,'Do you wish to write another duplicate of this disk (Y/N)? ',0
m_another   db  13,10,'Copy another diskette (Y/N)? ',0
m_serial    db  13,10,'Volume Serial Number is ',0
m_rderr     db  13,10,'Unrecoverable read error on drive ',0
m_wrerr     db  13,10,'Unrecoverable write error on drive ',0
m_side      db  ': Side ',0
m_track     db  ', track ',0
m_mayunus   db  13,10,'Target diskette may be unusable',13,10,0
m_unusable  db  13,10,'Target diskette unusable',13,10,0
m_badsrc    db  13,10,'SOURCE diskette bad or incompatible',0
m_incompat  db  13,10,'Drive types or diskette types not compatible',13,10,0
m_ended     db  13,10,'Copy process ended',13,10,0
m_nomem     db  13,10,'Insufficient memory',13,10,0
m_help      db  'Copies one diskette onto another, track for track.',13,10,13,10
            db  'DISKCOPY [drive1: [drive2:]] [/1] [/V] [/M]',13,10,13,10
            db  '  /1   Copies side 0 only.',13,10
            db  '  /V   Reads each track back after writing it, to check it.',13,10
            db  '  /M   Keeps the copy in conventional memory, which takes',13,10
            db  '       more than one pass for most diskettes.',13,10,13,10
            db  'Both diskettes have to be of the same type.  drive1 and drive2',13,10
            db  'may be the same drive: the source is read in one go, and then',13,10
            db  'written to as many diskettes as you like.',13,10,0

img_end:                                ; the BSS begins here

_DCOPY  ENDS
        END
