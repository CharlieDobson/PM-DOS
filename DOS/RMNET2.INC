;=====================================================================
; RMNET2.INC - the network redirector SERVER: INT 2Fh AH=11h answered
;              for the kernel's net drives, through the WfW bridge
;
; RMNET.INC is the CLIENT: RMDOS's INT 21h side turns a call on a net
; drive into INT 2Fh AH=11h, with MS-DOS 5's working state in the
; swappable data area.  Something has to be on the far end of that INT,
; and for the kernel's NET USE letters that something is here.
;
; THE PARKED KERNEL STILL HAS THE SHARE.  A letter the kernel had a
; NET USE on (DRV\SMBFS) was marked a net drive at the park (PARK.INC,
; RMDF_NET) and its current directory structure a redirector's here
; (RMINFO.INC, the net pass in rd_sysinit) - in use, redirected, bit 7,
; the way MSCDEX marks the CD-ROM, which is the mark Windows for
; Workgroups leaves to the DOS underneath.  Its SMB session is the
; parked kernel's, kept alive by the bridge's async tick; this server
; reaches it by reading the SDA MS-DOS filled in, packing the kernel's
; own redirector request block (INC\REDIR.INC, the NR_* fields), and
; handing it to PMDOS.386's V86 API - sub-function PDAPI_REDIR, which
; runs one resumable slice through the bridge and answers the request's
; status.  REDIR_PENDING means "not finished this slice": the server
; releases the virtual machine's time slice (INT 2Fh 1680h) so Windows
; and the tick advance the parked stack, then asks again - exactly the
; poll-and-yield loop BA2API.COM proved (BA2b-1).  Windows is never
; paused for it.
;
; RMCD.ASM is the template for the INT 2Fh dispatch and the SEARCHBUF
; layout; NETFS.INC (the kernel's own redirector client) is the field-
; by-field map between a DOS call and the request block.  What is here
; now (BA2b-2a) is enough for DIR: find first/next, disk info, and a
; directory check.  Open/read for TYPE, writes and the rest follow.
;
; Included by RMDOS.ASM after RMINFO.INC (which has the SDA offsets and
; calls n2_serve from i2F_net) and RMNET.INC.
;=====================================================================

;--- the request block (INC\REDIR.INC), the fields this server uses ---
NR_OP           EQU     0
NR_DRIVE        EQU     4
NR_HANDLE       EQU     8               ; the driver's open-file handle, in and out
NR_STATUS       EQU     12
NR_POSLO        EQU     16              ; a read's or write's file position
NR_POSHI        EQU     20              ; ...its high dword (0 for files < 4GB)
NR_COUNT        EQU     24              ; a find's next index / a read or write's count
NR_RESULT       EQU     28              ; bytes a read moved
NR_SIZELO       EQU     32
NR_ATTR         EQU     40
NR_TIMEDATE     EQU     44              ; (date << 16) | time
NR_BUFPTR       EQU     48              ; a read's destination, a flat linear address
NR_GEN          EQU     52              ; a find's listing id, in and out
NR_SPARE        EQU     60              ; open: 0 = open existing (no create)
NR_PATH         EQU     64              ; char[260]
NR_NAME         EQU     340             ; char[16], the 8.3 name a find returns
NR_SHARE        EQU     420             ; char[80], MOUNT's share - reused for
                                        ; RENAME's new name (a DOS path fits)
NR_SIZE         EQU     692

REDIR_FINDFIRST EQU     3
REDIR_FINDNEXT  EQU     4
REDIR_OPEN      EQU     5
REDIR_READ      EQU     6
REDIR_WRITE     EQU     7
REDIR_CLOSE     EQU     8
REDIR_DELETE    EQU     10
REDIR_MKDIR     EQU     11
REDIR_RMDIR     EQU     12
REDIR_RENAME    EQU     13
REDIR_RESUME    EQU     1000h
REDIR_PENDING   EQU     0FFh
N2_SRCH_FILTER  EQU     80h             ; SMBFS: a directory search (native's SRCH_FILTER)
N2_DATASIZE     EQU     512             ; the read bounce buffer, in RMDOS's own segment
                                        ; (below 1MB, identity-mapped, so SMBFS can write
                                        ; it in the bridge window); a read is chunked to it
                                        ; - small, as RMDOS's whole image must fit one 64K
                                        ; segment (PARK_RMPARAS); raise it for speed later

;--- PMDOS.386's V86 API ---------------------------------------------
N2_DEVID        EQU     504Dh           ; the VxD's device id ("PM")
N2_PDAPI_REDIR  EQU     1               ; the redirector sub-function
N2_POLLMAX      EQU     60000           ; a wedged op gives up, it does not hang

;--- the 21-byte search state this server keeps in SEARCHBUF ----------
; SS_DRIVE is 80h + the one-based letter, as RMDOS reads it to know a
; redirector's search (cd_mine's cdm_next); the rest is ours to resume
; the listing - the driver's listing id and the next index.
N2S_DRIVE       EQU     0               ; db  81h + letter
N2S_GEN         EQU     1               ; dd  the listing id (NR_GEN)
N2S_IDX         EQU     5               ; dd  the next index (NR_COUNT)
N2S_LEN         EQU     21

N2_ERR_NOMORE   EQU     18              ; no more files (DOS's own)

;=====================================================================
; n2_serve - INT 2Fh AH=11h, AL != 0, jumped to from i2F_net.  The
; caller's registers are live and the interrupt frame [IP][CS][FLAGS]
; is on the stack; DS is the caller's.  If the call is one of ours it
; is answered and returned with RETF 2 (CF and AX the result, as MS-DOS
; returns a redirector call); if not, it is failed ERR_FUNC the way
; i2F_net did before, so a redirector drive nobody serves is unchanged.
;=====================================================================
n2_serve:
        sti                             ; the op waits on the network; the
        cld                             ; interrupt was taken with these off
        push    ds
        push    es
        push    cs
        pop     ds                      ; DS = ours; ES set per handler
        cmp     al,1Bh
        je      n2sv_cds
        cmp     al,0Ch
        je      n2sv_cds
        cmp     al,05h
        je      n2sv_cds
        cmp     al,16h
        je      n2sv_cds                ; open: by THISCDS (the SFT is not filled yet)
        cmp     al,17h
        je      n2sv_cds                ; create: likewise
        cmp     al,1Ch
        je      n2sv_next
        cmp     al,06h
        je      n2sv_sft                ; close: by the SFT's signature
        cmp     al,08h
        je      n2sv_sft                ; read:  by the SFT's signature
        cmp     al,09h
        je      n2sv_sft                ; write: by the SFT's signature
        cmp     al,21h
        je      n2sv_sft                ; seek from end: by the SFT's signature
        cmp     al,13h
        je      n2sv_cds                ; delete: by THISCDS (a path, not a file)
        cmp     al,03h
        je      n2sv_cds                ; make directory
        cmp     al,01h
        je      n2sv_cds                ; remove directory
        cmp     al,11h
        je      n2sv_cds                ; rename: by THISCDS (two paths)
        jmp     n2_notours              ; a redirector call we do not serve
n2sv_cds:
        call    n2_cdsletter            ; ZF + n2_letter if THISCDS is ours
        jnz     n2_notours
        jmp     n2_dispatch
n2sv_next:
        call    n2_nextletter           ; ZF + n2_letter if SS_DRIVE (ES:DI) is ours
        jnz     n2_notours
        jmp     n2_dispatch
n2sv_sft:
        call    n2_sftmine              ; ZF + n2_letter if ES:DI is one of our files
        jnz     n2_notours
n2_dispatch:
        cmp     al,1Bh
        je      n2_findfirst
        cmp     al,1Ch
        je      n2_findnext
        cmp     al,0Ch
        je      n2_diskinfo
        cmp     al,16h
        je      n2_open
        cmp     al,17h
        je      n2_open
        cmp     al,06h
        je      n2_close
        cmp     al,08h
        je      n2_read
        cmp     al,09h
        je      n2_write
        cmp     al,21h
        je      n2_seekend
        cmp     al,13h
        je      n2_delete
        cmp     al,03h
        je      n2_mkdir
        cmp     al,01h
        je      n2_rmdir
        cmp     al,11h
        je      n2_rename
        jmp     n2_chdir                ; al = 05h

n2_notours:
        mov     ax,ERR_FUNC
n2_fail:                                ; AX = the error already
        pop     es
        pop     ds
        stc
        retf    2
n2_ok:
        pop     es
        pop     ds
        clc
        retf    2

;---------------------------------------------------------------------
; 1Bh FIND FIRST - the SDA's name (WFP_START) and search attribute
; (SATTRIB), the first match into SEARCHBUF (as DOS points the DTA).
;---------------------------------------------------------------------
n2_findfirst:
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_FINDFIRST
        ; the path, after the "X:" prefix, out of the SDA
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     si,word ptr es:[RL_SDA+SD_WFP]
        add     si,2                            ; past "N:"
        mov     di,OFFSET n2_req + NR_PATH
        mov     cx,260
nff_path:
        mov     al,es:[si]
        mov     [di],al
        or      al,al
        jz      nff_pathok
        inc     si
        inc     di
        loop    nff_path
        mov     byte ptr [di],0
nff_pathok:
        movzx   eax,byte ptr es:[RL_SDA+SD_SATTR]
        pop     es
        or      al,N2_SRCH_FILTER               ; a directory search, as native net_find
        mov     dword ptr [n2_req+NR_ATTR],eax
        call    n2_pdapi
        jc      nff_nofile
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     nff_nofile
        call    n2_fillsrch
        jmp     n2_ok
nff_nofile:
        mov     ax,ERR_NOFILE
        jmp     n2_fail

;---------------------------------------------------------------------
; 1Ch FIND NEXT - ES:DI = SEARCHBUF, the 21-byte state the client copied
; back (our listing id and next index in it).
;---------------------------------------------------------------------
n2_findnext:
        mov     eax,dword ptr es:[di+N2S_GEN]
        mov     [n2_savegen],eax
        mov     eax,dword ptr es:[di+N2S_IDX]
        test    eax,eax                                 ; no listing is that long
        js      nfn_nomore
        mov     [n2_saveidx],eax
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_FINDNEXT
        mov     eax,[n2_savegen]
        mov     dword ptr [n2_req+NR_GEN],eax
        mov     eax,[n2_saveidx]
        mov     dword ptr [n2_req+NR_COUNT],eax
        call    n2_pdapi
        jc      nfn_nomore
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     nfn_nomore
        call    n2_fillsrch
        jmp     n2_ok
nfn_nomore:
        mov     ax,N2_ERR_NOMORE
        jmp     n2_fail

;---------------------------------------------------------------------
; 0Ch DISK INFO - a share has no geometry, so a large plausible one, as
; the kernel's own net_info answers it (NETFS.INC): DIR turns it into a
; free-bytes figure and nothing else reads it.
;---------------------------------------------------------------------
n2_diskinfo:
        mov     al,64                   ; sectors per cluster
        mov     ah,0F8h                 ; media
        mov     bx,0FFFFh               ; total clusters
        mov     cx,512                  ; bytes per sector
        mov     dx,0FFFFh               ; free clusters
        jmp     n2_ok

;---------------------------------------------------------------------
; 05h CHDIR - is the SDA's name a directory?  For now the answer is yes:
; DIR of the root does not chdir, and the root is always there.  A real
; check (a find of the name) comes with the sub-directory work.
;---------------------------------------------------------------------
n2_chdir:
        jmp     n2_ok

;---------------------------------------------------------------------
; 1113h DELETE / 1103h MAKE DIRECTORY / 1101h REMOVE DIRECTORY - a call
; that names a path and gives back nothing but whether it worked.  The
; SDA's WFP_START names it (the client's net_canon put the canonicalized
; name there, THISCDS the drive); the server copies the path, past its
; "X:" prefix, and runs the one op the driver does for it.  The driver
; answers 0 or an access error, which becomes the DOS error in AX.
;---------------------------------------------------------------------
n2_delete:
        mov     dword ptr [n2_pathfn],REDIR_DELETE
        jmp     n2_pathop
n2_mkdir:
        mov     dword ptr [n2_pathfn],REDIR_MKDIR
        jmp     n2_pathop
n2_rmdir:
        mov     dword ptr [n2_pathfn],REDIR_RMDIR
n2_pathop:
        call    n2_reqzero
        mov     eax,[n2_pathfn]
        mov     dword ptr [n2_req+NR_OP],eax
        call    n2_setpath
        call    n2_pdapi
        jc      n2po_fail
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     n2po_status
        jmp     n2_ok
n2po_status:
        mov     ax,word ptr [n2_req+NR_STATUS]
        jmp     n2_fail
n2po_fail:
        mov     ax,ERR_ACCESS                   ; the bridge was not there
        jmp     n2_fail

;---------------------------------------------------------------------
; 1111h RENAME - the SDA names both: the old name at WFP_START (SD_WFP)
; and the new one at REN_WFP (SD_RENWFP), each "D:\NAME".  The old goes
; in NR_PATH and the new in NR_SHARE (REDIR.H reuses the MOUNT-only share
; field for it - a DOS path fits its 80 bytes), both past the "X:" prefix.
; One REDIR_RENAME op; the driver moves it within the share.
;---------------------------------------------------------------------
n2_rename:
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_RENAME
        call    n2_setpath                      ; SD_WFP+2 -> NR_PATH (old)
        ; the new name: SD_RENWFP+2 -> NR_SHARE
        push    si
        push    di
        push    cx
        push    ax
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     si,word ptr es:[RL_SDA+SD_RENWFP]
        add     si,2                            ; past "N:"
        mov     di,OFFSET n2_req + NR_SHARE
        mov     cx,79                           ; room for the NUL in char[80]
nrn_copy:
        mov     al,es:[si]
        or      al,al
        jz      nrn_term
        mov     [di],al
        inc     si
        inc     di
        dec     cx
        jnz     nrn_copy
nrn_term:
        mov     byte ptr [di],0
        pop     es
        pop     ax
        pop     cx
        pop     di
        pop     si
        call    n2_pdapi
        jc      nrn_nobridge
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     nrn_status
        jmp     n2_ok
nrn_status:
        mov     ax,word ptr [n2_req+NR_STATUS]
        jmp     n2_fail
nrn_nobridge:
        mov     ax,ERR_ACCESS
        jmp     n2_fail

;---------------------------------------------------------------------
; n2_setpath - copy the SDA's path (WFP_START), past its "X:" prefix,
; into n2_req+NR_PATH, bounded to the field.  DS = ours.  Registers kept.
;---------------------------------------------------------------------
n2_setpath:
        push    si
        push    di
        push    cx
        push    ax
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     si,word ptr es:[RL_SDA+SD_WFP]
        add     si,2                            ; past "N:"
        mov     di,OFFSET n2_req + NR_PATH
        mov     cx,259                          ; room for the NUL in char[260]
nsp_copy:
        mov     al,es:[si]
        or      al,al
        jz      nsp_term
        mov     [di],al
        inc     si
        inc     di
        dec     cx
        jnz     nsp_copy
nsp_term:
        mov     byte ptr [di],0
        pop     es
        pop     ax
        pop     cx
        pop     di
        pop     si
        ret

;---------------------------------------------------------------------
; 1116h OPEN / 1117h CREATE - ES:DI = a fresh SFT (RMDOS allocated it,
; the mode set, busy).  The SDA's WFP_START names the file.  Open it on
; the server and fill the SFT: our signature in SFT_DEVPTR (so read and
; close know it is ours), the driver's handle in SFT_START, and the size,
; time and attribute the driver returned.  3Dh opens an existing file
; (NR_SPARE 0); 3Ch/5Bh create or truncate it (NR_SPARE 1), which is the
; only difference between the two on the wire.
;---------------------------------------------------------------------
n2_open:
        mov     byte ptr [n2_crdisp],0
        cmp     al,17h
        jne     n2o_nocreate
        mov     byte ptr [n2_crdisp],1                  ; create/truncate
n2o_nocreate:
        mov     word ptr [n2_sftptr],di
        mov     word ptr [n2_sftptr+2],es
        movzx   eax,word ptr es:[di+SFT_MODE]
        and     eax,3
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_OPEN
        mov     dword ptr [n2_req+NR_COUNT],eax         ; access
        movzx   eax,byte ptr [n2_crdisp]
        mov     dword ptr [n2_req+NR_SPARE],eax         ; disposition
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     si,word ptr es:[RL_SDA+SD_WFP]
        add     si,2                                    ; past "N:"
        mov     di,OFFSET n2_req + NR_PATH
        mov     cx,260
n2o_path:
        mov     al,es:[si]
        mov     [di],al
        or      al,al
        jz      n2o_pathok
        inc     si
        inc     di
        loop    n2o_path
        mov     byte ptr [di],0
n2o_pathok:
        pop     es
        call    n2_pdapi
        jc      n2o_fail
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     n2o_fail
        les     di,dword ptr [n2_sftptr]
        movzx   ax,byte ptr [n2_letter]                 ; the net letter in bits 0-5, so
        or      ax,8040h                                ; read and close recover the drive;
        mov     word ptr es:[di+SFT_DEVINFO],ax         ; bit 15 set, bit 7 clear = sft_isnet
        mov     word ptr es:[di+SFT_DEVPTR],OFFSET n2_sig
        mov     ax,cs
        mov     word ptr es:[di+SFT_DEVPTR+2],ax
        mov     eax,dword ptr [n2_req+NR_HANDLE]
        mov     dword ptr es:[di+SFT_START],eax         ; the driver's handle
        mov     eax,dword ptr [n2_req+NR_SIZELO]
        mov     dword ptr es:[di+SFT_SIZE],eax
        mov     dword ptr es:[di+SFT_POS],0
        mov     eax,dword ptr [n2_req+NR_TIMEDATE]
        mov     word ptr es:[di+SFT_TIME],ax
        shr     eax,16
        mov     word ptr es:[di+SFT_DATE],ax
        mov     al,byte ptr [n2_req+NR_ATTR]
        mov     byte ptr es:[di+SFT_ATTR],al
        jmp     n2_ok
n2o_fail:
        mov     ax,ERR_NOFILE
        jmp     n2_fail

;---------------------------------------------------------------------
; 1108h READ - ES:DI = the SFT, the SDA's DTA = where the data goes,
; BYTPOS = the file position, CX = the count.  Read it from the server a
; bounce-buffer-full at a time into RMDOS's own low (identity-mapped)
; buffer, copy each chunk into the caller's buffer, advance the SFT's
; position, and answer CX = what was read.
;---------------------------------------------------------------------
n2_read:
        mov     word ptr [n2_sftptr],di
        mov     word ptr [n2_sftptr+2],es
        mov     eax,dword ptr es:[di+SFT_START]
        mov     [n2_handle],eax
        movzx   eax,cx
        mov     [n2_total],eax
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     eax,dword ptr es:[RL_SDA+SD_BYTPOS]
        mov     [n2_pos],eax
        mov     ax,word ptr es:[RL_SDA+SD_DTA]
        mov     word ptr [n2_dstptr],ax
        mov     ax,word ptr es:[RL_SDA+SD_DTA+2]
        mov     word ptr [n2_dstptr+2],ax
        pop     es
        mov     dword ptr [n2_done],0
nrd_loop:
        mov     eax,[n2_total]
        sub     eax,[n2_done]
        jz      nrd_end
        cmp     eax,N2_DATASIZE
        jbe     nrd_chunk
        mov     eax,N2_DATASIZE
nrd_chunk:
        mov     [n2_chunk],eax
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_READ
        mov     eax,[n2_handle]
        mov     dword ptr [n2_req+NR_HANDLE],eax
        mov     eax,[n2_pos]
        add     eax,[n2_done]
        mov     dword ptr [n2_req+NR_POSLO],eax
        mov     eax,[n2_chunk]
        mov     dword ptr [n2_req+NR_COUNT],eax
        xor     eax,eax                                 ; the bounce buffer, flat linear
        mov     ax,cs
        shl     eax,4
        add     eax,OFFSET n2_data
        mov     dword ptr [n2_req+NR_BUFPTR],eax
        call    n2_pdapi
        jc      nrd_end
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     nrd_end
        mov     eax,dword ptr [n2_req+NR_RESULT]
        or      eax,eax
        jz      nrd_end
        push    es                                      ; copy the chunk to the caller
        mov     es,word ptr [n2_dstptr+2]
        mov     di,word ptr [n2_dstptr]
        add     di,word ptr [n2_done]
        mov     si,OFFSET n2_data
        mov     cx,ax                                   ; the bytes read (<= N2_DATASIZE)
        cld
        rep     movsb
        pop     es
        add     [n2_done],eax
        mov     ebx,[n2_chunk]
        cmp     eax,ebx
        jb      nrd_end                                 ; short: end of file
        jmp     nrd_loop
nrd_end:
        les     di,dword ptr [n2_sftptr]
        mov     eax,[n2_done]
        add     dword ptr es:[di+SFT_POS],eax
        mov     cx,word ptr [n2_done]
        jmp     n2_ok

;---------------------------------------------------------------------
; 1109h WRITE - ES:DI = the SFT, the SDA's DTA = the source, BYTPOS the
; file position, CX the count (the mirror of read).  The source goes to
; the server a bounce-buffer-full at a time: each chunk is copied from
; the caller's buffer into RMDOS's own low (identity-mapped) buffer,
; which SMBFS reads in the bridge window.  The SFT's position advances,
; its size grows past the old end, and CX answers what was written - a
; count short of what was asked is how DOS learns the disk is full.
;---------------------------------------------------------------------
n2_write:
        mov     word ptr [n2_sftptr],di
        mov     word ptr [n2_sftptr+2],es
        mov     eax,dword ptr es:[di+SFT_START]
        mov     [n2_handle],eax
        movzx   eax,cx
        mov     [n2_total],eax
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     eax,dword ptr es:[RL_SDA+SD_BYTPOS]
        mov     [n2_pos],eax
        mov     ax,word ptr es:[RL_SDA+SD_DTA]
        mov     word ptr [n2_srcptr],ax
        mov     ax,word ptr es:[RL_SDA+SD_DTA+2]
        mov     word ptr [n2_srcptr+2],ax
        pop     es
        mov     dword ptr [n2_done],0
nwr_loop:
        mov     eax,[n2_total]
        sub     eax,[n2_done]
        jz      nwr_end
        cmp     eax,N2_DATASIZE
        jbe     nwr_chunk
        mov     eax,N2_DATASIZE
nwr_chunk:
        mov     [n2_chunk],eax
        ; this chunk from the caller's buffer into the bounce buffer; the
        ; our-segment reads take a CS override, so the order in which DS
        ; is reloaded to the source segment does not matter
        push    ds
        push    es
        mov     si,word ptr cs:[n2_srcptr]
        add     si,word ptr cs:[n2_done]
        mov     cx,word ptr cs:[n2_chunk]
        mov     di,OFFSET n2_data
        push    cs
        pop     es                                      ; ES:DI = ours:n2_data
        mov     ds,word ptr cs:[n2_srcptr+2]            ; DS:SI = the caller's buffer
        cld
        rep     movsb
        pop     es
        pop     ds
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_WRITE
        mov     eax,[n2_handle]
        mov     dword ptr [n2_req+NR_HANDLE],eax
        mov     eax,[n2_pos]
        add     eax,[n2_done]
        mov     dword ptr [n2_req+NR_POSLO],eax
        mov     eax,[n2_chunk]
        mov     dword ptr [n2_req+NR_COUNT],eax
        xor     eax,eax                                 ; the bounce buffer, flat linear
        mov     ax,cs
        shl     eax,4
        add     eax,OFFSET n2_data
        mov     dword ptr [n2_req+NR_BUFPTR],eax
        call    n2_pdapi
        jc      nwr_end
        cmp     dword ptr [n2_req+NR_STATUS],0
        jne     nwr_end
        mov     eax,dword ptr [n2_req+NR_RESULT]
        or      eax,eax
        jz      nwr_end
        mov     ebx,[n2_chunk]
        cmp     eax,ebx
        jbe     nwr_okcnt
        mov     eax,ebx                                 ; never more than was sent
nwr_okcnt:
        add     [n2_done],eax
        cmp     eax,ebx
        jb      nwr_end                                 ; short: the disk is full
        jmp     nwr_loop
nwr_end:
        les     di,dword ptr [n2_sftptr]
        mov     eax,[n2_done]
        add     dword ptr es:[di+SFT_POS],eax
        mov     eax,dword ptr es:[di+SFT_POS]           ; grow the file past the old end
        cmp     eax,dword ptr es:[di+SFT_SIZE]
        jbe     nwr_sized
        mov     dword ptr es:[di+SFT_SIZE],eax
nwr_sized:
        mov     cx,word ptr [n2_done]
        jmp     n2_ok

;---------------------------------------------------------------------
; 1106h CLOSE - ES:DI = the SFT.  Close the driver's handle, then drop
; the reference count, freeing the entry at nought (the redirector's
; job: MS-DOS hands a network close over whole - RMFILE2.INC, srl_net).
;---------------------------------------------------------------------
n2_close:
        mov     word ptr [n2_sftptr],di
        mov     word ptr [n2_sftptr+2],es
        ; DROP ONE REFERENCE AND CLOSE THE SERVER FILE ONLY ON THE LAST ONE.
        ; A DUP shares the entry and the one server handle (COMMAND redirects
        ; a command's output by opening the file, FORCEDUP'ing it onto stdout
        ; and closing the original); if this closed the server file on that
        ; first close, the surviving handle's writes would fail with "invalid
        ; handle".  MS-DOS hands every net close here whole (DOS6SRC
        ; dos/close.asm), so the count is ours to keep.
        dec     word ptr es:[di+SFT_REF]
        jg      ncl_done                        ; others still hold it: keep open
        mov     word ptr es:[di+SFT_REF],0
        mov     eax,dword ptr es:[di+SFT_START]
        call    n2_reqzero
        mov     dword ptr [n2_req+NR_OP],REDIR_CLOSE
        mov     dword ptr [n2_req+NR_HANDLE],eax
        call    n2_pdapi
ncl_done:
        jmp     n2_ok

;---------------------------------------------------------------------
; 1121h SEEK FROM END - ES:DI = the SFT, CX:DX = the signed offset from
; the end.  DOS asks the redirector this only for a net file opened deny-
; none or deny-read, where another writer might have moved the end since
; it was opened (RMFILE2.INC fn42).  The native kernel answers a seek
; from end straight from the entry's size (INT21.INC f42_end), with no
; re-query, so this does the same: the end is SFT_SIZE, the new position
; is SFT_SIZE + the offset, and DX:AX answers it.  No bridge call - the
; size is the kernel's own field, kept current by open and by n2_write.
; (An authoritative re-stat for a concurrent outside writer would need a
; new driver op - REDIR.H has none - and is deferred.)
;---------------------------------------------------------------------
n2_seekend:
        movzx   eax,cx
        shl     eax,16
        mov     ax,dx                           ; EAX = CX:DX, the offset
        add     eax,dword ptr es:[di+SFT_SIZE]  ; measured from the end
        mov     dword ptr es:[di+SFT_POS],eax   ; the new position
        mov     edx,eax
        shr     edx,16                          ; DX:AX = the new position
        jmp     n2_ok

;---------------------------------------------------------------------
; n2_sftmine - ES:DI = a file table entry.  ZF if it is one of ours
; (SFT_DEVPTR is our signature) and then n2_letter = its drive.  AX kept.
;---------------------------------------------------------------------
n2_sftmine:
        push    ax
        cmp     word ptr es:[di+SFT_DEVPTR],OFFSET n2_sig
        jne     n2sm_no
        mov     ax,cs
        cmp     word ptr es:[di+SFT_DEVPTR+2],ax
        jne     n2sm_no
        mov     ax,word ptr es:[di+SFT_DEVINFO]
        and     al,3Fh
        mov     [n2_letter],al
        pop     ax
        cmp     al,al                   ; ZF: ours
        ret
n2sm_no:
        pop     ax
        or      sp,sp                   ; NZ
        ret

;---------------------------------------------------------------------
; n2_fillsrch - a successful find's answer into SEARCHBUF: the 21-byte
; state (so find-next can resume), then the entry (name, attribute,
; time, date, size) where the client reads it (RMNET.INC, net_find).
; DS = ours, n2_req holds the driver's answer.  Registers but flags kept.
;---------------------------------------------------------------------
n2_fillsrch:
        pusha
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     di,RL_SDA + SD_SRCH
        mov     al,[n2_letter]
        add     al,81h
        mov     es:[di+N2S_DRIVE],al
        mov     eax,dword ptr [n2_req+NR_GEN]
        mov     dword ptr es:[di+N2S_GEN],eax
        mov     eax,dword ptr [n2_req+NR_COUNT]
        mov     dword ptr es:[di+N2S_IDX],eax
        ; the entry, after the 21-byte state
        add     di,N2S_LEN
        push    di
        mov     si,OFFSET n2_req + NR_NAME
        call    n2_tofcb                        ; DS:SI 8.3 ASCIIZ -> ES:DI 11 bytes
        pop     di
        mov     al,byte ptr [n2_req+NR_ATTR]
        mov     es:[di+DE_ATTR],al
        mov     eax,dword ptr [n2_req+NR_TIMEDATE]
        mov     es:[di+DE_TIME],ax
        shr     eax,16
        mov     es:[di+DE_DATE],ax
        mov     eax,dword ptr [n2_req+NR_SIZELO]
        mov     dword ptr es:[di+DE_SIZE],eax
        pop     es
        popa
        ret

;---------------------------------------------------------------------
; n2_tofcb - DS:SI = a NUL-terminated 8.3 name (upper case, as SMBFS
; returns it), ES:DI = eleven bytes: "NAME    EXT", blank padded.
; Registers kept.
;---------------------------------------------------------------------
n2_tofcb:
        push    si
        push    di
        push    cx
        push    ax
        push    bx
        mov     bx,di                   ; the field's start
        push    di
        mov     al,' '
        mov     cx,11
        cld
        rep     stosb
        pop     di
        ; "." and ".." are literal in the name field, not a base and
        ; extension - net_pack turns them back into "." and ".."
        cmp     byte ptr [si],'.'
        jne     ntf_normal
        mov     byte ptr es:[di],'.'
        cmp     byte ptr [si+1],'.'
        jne     ntf_done
        mov     byte ptr es:[di+1],'.'
        jmp     short ntf_done
ntf_normal:
        mov     cx,8
ntf_base:
        mov     al,[si]
        or      al,al
        jz      ntf_done
        cmp     al,'.'
        je      ntf_dot
        mov     es:[di],al
        inc     si
        inc     di
        loop    ntf_base
ntf_skip:                               ; past eight: on to the dot
        mov     al,[si]
        or      al,al
        jz      ntf_done
        cmp     al,'.'
        je      ntf_dot
        inc     si
        jmp     short ntf_skip
ntf_dot:
        inc     si                      ; past the '.'
        lea     di,[bx+8]               ; the extension's place
        mov     cx,3
ntf_ext:
        mov     al,[si]
        or      al,al
        jz      ntf_done
        mov     es:[di],al
        inc     si
        inc     di
        loop    ntf_ext
ntf_done:
        pop     bx
        pop     ax
        pop     cx
        pop     di
        pop     si
        ret

;---------------------------------------------------------------------
; n2_cdsletter - ZF if the SDA's THISCDS is one of our net letters, and
; then n2_letter = the letter (0 = A).  AX preserved.  For the calls
; that name a drive or a path (DOS's TestNet loads THISCDS).
;---------------------------------------------------------------------
n2_cdsletter:
        push    ax
        push    dx
        push    es
        mov     es,word ptr [rd_lowseg]
        mov     ax,word ptr es:[RL_SDA+SD_THISCDS+2]
        cmp     ax,word ptr [rd_lowseg]
        jne     n2cl_no
        mov     ax,word ptr es:[RL_SDA+SD_THISCDS]
        sub     ax,RL_CDS
        xor     dx,dx
        mov     dl,CDS_LEN
        div     dl                      ; AL = letter, AH = remainder
        or      ah,ah
        jnz     n2cl_no
        mov     [n2_letter],al
        call    n2_isnet                ; AL = letter -> ZF if ours
        jnz     n2cl_no
        pop     es
        pop     dx
        pop     ax
        cmp     al,al                   ; ZF: ours
        ret
n2cl_no:
        pop     es
        pop     dx
        pop     ax
        or      sp,sp                   ; NZ
        ret

;---------------------------------------------------------------------
; n2_nextletter - ZF if the find-next search at ES:DI is one of ours
; (its SS_DRIVE is 81h + a net letter), and then n2_letter = the letter.
; AX preserved.
;---------------------------------------------------------------------
n2_nextletter:
        push    ax
        mov     al,es:[di+N2S_DRIVE]
        sub     al,81h
        jc      n2nl_no
        cmp     al,26
        jae     n2nl_no
        mov     [n2_letter],al
        call    n2_isnet
        jnz     n2nl_no
        pop     ax
        cmp     al,al                   ; ZF
        ret
n2nl_no:
        pop     ax
        or      sp,sp                   ; NZ
        ret

;---------------------------------------------------------------------
; n2_isnet - AL = a letter (0 = A).  ZF if that letter is one the park
; marked a net drive (rd_netmap).  AL kept; a pop leaves the test's ZF.
;---------------------------------------------------------------------
n2_isnet:
        push    ecx
        push    edx
        movzx   ecx,al
        mov     edx,1
        shl     edx,cl
        test    dword ptr [rd_netmap],edx
        pop     edx
        pop     ecx
        jz      n2in_no
        cmp     al,al                   ; the bit was set: ours
        ret
n2in_no:
        or      sp,sp                   ; NZ
        ret

;---------------------------------------------------------------------
; n2_reqzero - clear n2_req and set NR_DRIVE from n2_letter.  The caller
; sets NR_OP.  Registers kept.
;---------------------------------------------------------------------
n2_reqzero:
        push    eax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di,OFFSET n2_req
        mov     cx,NR_SIZE
        xor     al,al
        cld
        rep     stosb
        movzx   eax,byte ptr [n2_letter]
        mov     dword ptr [n2_req+NR_DRIVE],eax
        pop     es
        pop     di
        pop     cx
        pop     eax
        ret

;=====================================================================
; n2_pdapi - run [n2_req] to completion through PMDOS.386's V86 API.
; The op is in NR_OP.  -> CF clear and NR_STATUS = the result (0 = ok);
; CF set when the bridge API is not there (so the caller fails the call).
;
; The API runs ONE resumable slice a call and answers NR_STATUS in AX;
; REDIR_PENDING means poll again, and between polls this releases the
; virtual machine's time slice so Windows and the bridge's async tick
; advance the parked stack (BA2API.COM proved this loop, BA2b-1).  The
; entry is got once, lazily - WIN386 is not up when RMDOS starts, only
; when a box first reaches here - and WIN386 answers INT 2Fh 1684h and
; 1680h itself, above RMDOS's own INT 2Fh hook, so neither re-enters it.
;=====================================================================
n2_pdapi:
        cmp     word ptr [n2_apiseg],0
        jne     n2p_have
        cmp     byte ptr [n2_apitried],0
        jne     n2p_noapi
        mov     byte ptr [n2_apitried],1
        push    es
        push    di
        xor     di,di
        mov     es,di
        mov     ax,1684h
        mov     bx,N2_DEVID
        int     2Fh                     ; -> ES:DI, WIN386 answers it
        mov     ax,es
        or      ax,di
        jz      n2p_noapi_pop
        mov     word ptr [n2_apioff],di
        mov     word ptr [n2_apiseg],es
        pop     di
        pop     es
n2p_have:
        push    cx
        mov     cx,N2_POLLMAX
n2p_poll:
        push    cx
        push    es
        push    cs
        pop     es
        mov     dx,OFFSET n2_req
        mov     ax,N2_PDAPI_REDIR
        call    dword ptr [n2_apient]   ; -> AX = NR_STATUS, CF clear
        pop     es
        pop     cx
        cmp     ax,REDIR_PENDING
        jne     n2p_done
        mov     ax,1680h                ; release this VM's time slice
        int     2Fh
        loop    n2p_poll
n2p_done:
        pop     cx
        clc
        ret
n2p_noapi_pop:
        pop     di
        pop     es
n2p_noapi:
        stc
        ret

;=====================================================================
; Data - one request block (a redirector call runs to completion inside
; one INT 2Fh and never nests here), the cached API entry, and scratch.
;=====================================================================
n2_letter   db  0
n2_apitried db  0
n2_apient   label dword
n2_apioff   dw  0
n2_apiseg   dw  0
n2_savegen  dd  0
n2_saveidx  dd  0
n2_sig      dd  0                       ; a net file's SFT_DEVPTR points here - the
                                        ; address is the signature, not the value
n2_sftptr   dd  0                       ; the SFT a handle call is about, far
n2_dstptr   dd  0                       ; a read's destination, far
n2_srcptr   dd  0                       ; a write's source (the caller's DTA), far
n2_pathfn   dd  0                       ; a path op's REDIR_* function
n2_crdisp   db  0                       ; open (0) or create/truncate (1)
n2_handle   dd  0                       ; the driver's open-file handle
n2_pos      dd  0                       ; a read's start position
n2_total    dd  0                       ; bytes a read asked for
n2_done     dd  0                       ; ...and moved so far
n2_chunk    dd  0                       ; this slice's byte count
n2_req      db  NR_SIZE dup(0)
n2_data     db  N2_DATASIZE dup(0)      ; the read bounce buffer (1:1 low memory)
