;=====================================================================
; RMNET.INC - a redirector's drives and files: INT 2Fh 11xxh
;
; A DRIVE LETTER CAN BELONG TO SOMEBODY ELSE.  DOS has always let a
; program take one away from it - a network client, MSCDEX, and in
; Windows for Workgroups 3.11 IFSMGR.386, which is how 32-bit file
; access works.  IFSMGR claims a hard disk by setting bits 15 and 14 of
; its current directory structure's flags (curdir_isnet, curdir_inuse),
; and from then on DOS does nothing for that drive itself: every call
; that names it - open, create, delete, find, rename, the directories,
; the attributes, EXEC's own reads - goes to INT 2Fh with AH=11h, and
; every file opened there is the redirector's, so every call on its
; handle goes there too.  IFSHLP.SYS hooks that interrupt in each
; virtual machine and passes the call up to IFSMGR, which does it in
; protected mode through VFAT.
;
; WHAT THE REDIRECTOR IS GIVEN is MS-DOS's own working state, in the
; swappable data area at the offsets DOS 5 has them (RMINFO.INC, SD_*):
; the name in OPENBUF and its offset in WFP_START, the drive's structure
; in THISCDS, the file table entry in THISSFT and ES:DI, the search
; attribute, the DTA - and for an open or a create one more word, pushed
; just before the INT.  This file fills in exactly what MS-DOS 5 fills
; in for each call, in the order it does it, out of DOS6SRC dos/open.asm,
; create.asm, close.asm, disk.asm, dircall.asm, isearch.asm, search.asm,
; finfo.asm, dinfo.asm, delete.asm, rename.asm and handle.asm.  A
; redirector written against MS-DOS reads nothing else.
;
; THE FILE TABLE ENTRY IS THE REDIRECTOR'S once it has opened it.  This
; DOS allocates it and the handle that points at it, as DOS does, and
; the redirector fills it in - its own pointer where a local file's
; device pointer goes, its own data where this DOS keeps a local file's
; cluster - so from then on only DOS's fields of it mean anything here,
; and the first thing every handle call asks is whether the entry is a
; redirector's (sft_isnet, RMFILE2.INC).
;
; WHAT IS NOT HERE: names that begin "\\" (RMDOS has no UNC names), the
; redirector's first look at every name (1123h, which only redirected
; printers use), the redirection list (5Fh), redirected printers, and
; a current directory structure whose root is longer than "D:\" (a
; network share's).  None of them is how a drive is taken from DOS.
;=====================================================================

;=====================================================================
; net_drive - AL = a drive (0 = A).  NZ if a redirector has it - its
;             current directory structure says so - and then the SDA's
;             THISCDS names that structure, as TransPath leaves it.
;             ZF for one of this DOS's own.  Every register is kept.
;=====================================================================
net_drive:
        push    di
        push    es
        cmp     al,byte ptr cs:[rd_ncds]
        jae     ndr_local
        mov     es,word ptr cs:[rd_lowseg]
        movzx   di,al
        imul    di,di,CDS_LEN
        add     di,RL_CDS
        test    byte ptr es:[di+CDS_FLAGS+1],NC_ISNET
        jz      ndr_out                 ; ZF: this DOS's own
        mov     word ptr es:[RL_SDA + SD_THISCDS],di    ; MOVs keep the NZ
        mov     word ptr es:[RL_SDA + SD_THISCDS + 2],es
        jmp     short ndr_out
ndr_local:
        cmp     al,al                   ; ZF: no such letter is anybody's
ndr_out:
        pop     es
        pop     di
        ret

;=====================================================================
; cds_inuse - AL = a drive (0 = A).  NZ if its current directory
;             structure is in use (curdir_inuse, 4000h), which a drive
;             of this DOS's own and a redirector's both are; ZF for a
;             letter nobody has, or one past LASTDRIVE.  Every register
;             is kept.  DOS's GetThisDrv, which is what 0Eh and 29h ask.
;=====================================================================
cds_inuse:
        push    di
        push    es
        cmp     al,byte ptr cs:[rd_ncds]
        jae     ciu_none
        mov     es,word ptr cs:[rd_lowseg]
        movzx   di,al
        imul    di,di,CDS_LEN
        test    byte ptr es:[RL_CDS + di + CDS_FLAGS + 1],40h
        jmp     short ciu_out
ciu_none:
        cmp     al,al                   ; ZF
ciu_out:
        pop     es
        pop     di
        ret

;=====================================================================
; net_canon - ES:SI -> the caller's path.  rd_canon spells it, and then:
;
;               CF          AX = why it is not a path at all
;               NC, ZF      AL = a drive of this DOS's own: go on
;               NC, NZ      AL = a redirector's drive.  The SDA has the
;                           name and THISCDS, as MS-DOS's TransPath
;                           leaves them for INT 2Fh 11xxh
;
;             Every register but AX is kept.  DS must be ours.
;
; The spelling is the same either way - upper case, 8.3 components, "."
; and ".." resolved, "*" made into question marks, the current
; directory put in front of a relative name - because TransPath makes
; it the same either way: Canonicalize runs before DOS looks at whose
; the drive is.
;=====================================================================
net_canon:
        call    rd_canon                ; -> rd_canbuf, AL = drive
        jc      ncn_bad
        call    net_drive
        jz      ncn_out                 ; NC and ZF: ours
        call    net_name1
        test    sp,sp                   ; NC and NZ: the redirector's
ncn_out:
        ret
ncn_bad:
        mov     ax,ERR_NOPATH           ; whatever rd_canon said, as rd_path
        ret                             ; has always answered it (CF still set)

;--- net_nometa - CF if rd_canbuf has a wildcard in it.  Open, create,
;--- delete, CHDIR and the attribute calls refuse one before any
;--- redirector is asked (DOS's cMeta test).  Registers kept.
net_nometa:
        push    si
        mov     si,OFFSET rd_canbuf
nnw_loop:
        cmp     byte ptr [si],0
        je      nnw_none
        cmp     byte ptr [si],'?'
        je      nnw_meta
        inc     si
        jmp     short nnw_loop
nnw_meta:
        pop     si
        stc
        ret
nnw_none:
        pop     si
        clc
        ret

;--- net_name1 - rd_canbuf into the SDA as its first name, which
;--- WFP_START then names; CURR_DIR_END is FFFFh, "the current directory
;--- is not in it", which a redirector reads only to tell error 3 from
;--- error 16.  net_name2 - as a rename's second name, which REN_WFP
;--- names.  Every register is kept; the flags are not.
net_name1:
        pusha
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     di,RL_SDA + SD_FN1
        mov     word ptr es:[RL_SDA + SD_WFP],di
        mov     word ptr es:[RL_SDA + SD_CURDIR],0FFFFh
        jmp     short nnm_copy
net_name2:
        pusha
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     di,RL_SDA + SD_FN2
        mov     word ptr es:[RL_SDA + SD_RENWFP],di
nnm_copy:
        mov     si,OFFSET rd_canbuf
        cld
nnm_loop:
        lodsb
        stosb
        or      al,al
        jnz     nnm_loop
        pop     es
        popa
        ret

;--- net_sattr - AL = the search attribute MS-DOS would have set for the
;--- call (SATTRIB): 16h for open, rename and the attribute calls, 06h
;--- for create and delete, the caller's CL for a search.
net_sattr:
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     byte ptr es:[RL_SDA + SD_SATTR],al
        pop     es
        ret

;=====================================================================
; net_call - AX = the redirector's function, 11xxh, with ES:DI, CX and
;            the SDA as that function wants them, and [net_parm] the
;            word MS-DOS pushes for it.  -> CF and AX as the redirector
;            answered, and BX CX DX SI DI ES as it left them.  DS kept.
;
; THE WORD IS PUSHED HERE, last thing before the INT, because that is
; where the redirector looks for it: MS-DOS does PUSH AX, MOV AX,11xxh,
; INT 2Fh, POP BX, and IFSHLP.SYS takes it from SS:SP+8 inside its own
; handler - the slot just above the interrupt's three words, with its
; own BX pushed under them.  Anything between the push and the INT
; would move it.  Calls with no word to pass push one all the same;
; nothing reads it.
;
; THE PROCESS goes into the SDA first, CurrentPDB and PROC_ID both,
; because the PSP this call came in with may not be the one it is for
; now (an EXEC between, a process ending) and the redirector reads it
; there.  DS is the DOS data segment, as it is in MS-DOS.
;=====================================================================
net_call:
        push    ds
        push    ax
        mov     ds,word ptr cs:[rd_lowseg]
        mov     ax,word ptr cs:[rd_psp]
        mov     word ptr ds:[RL_SDA + SD_PSP],ax
        mov     word ptr ds:[RL_SDA + SD_PROCID],ax
        pop     ax
        push    word ptr cs:[net_parm]
        int     2Fh
        pop     ds                      ; the word, thrown away - POP leaves
        pop     ds                      ; the carry where the INT put it
        ret

;=====================================================================
; net_user - AX = 111Eh or 111Fh: an INT 21h call the redirector
;            answers whole (5F00h-5F06h, 5E02h-5E05h).  BP = the
;            frame.  Ends the call: CF and AX as the redirector left
;            them, and the caller's registers as it wrote them.
;
; MS-DOS MAKES THE CALL WITH THE CALLER'S OWN REGISTERS - DS:SI and
; ES:DI at the names, BX the type, CX the caller's word - and the
; caller's AX pushed just before the INT, as for every 11xxh.  It reads
; nothing back but the carry and AX: the redirector writes what the
; caller gets - 5F02h's BX and CX, say - into the caller's register
; frame, which the SDA's user_SP and user_SS point at, in MS-DOS's
; order (user_environ, DOS6SRC inc/dossym.inc).  This DOS's frame is
; in PUSHA order, so one in MS-DOS's is built on the stack for the
; call, the SDA pointed at it, and what is in it afterwards is the
; caller's.
;=====================================================================
UE_AX           EQU     00h
UE_BX           EQU     02h
UE_CX           EQU     04h
UE_DX           EQU     06h
UE_SI           EQU     08h
UE_DI           EQU     0Ah
UE_BP           EQU     0Ch
UE_DS           EQU     0Eh
UE_ES           EQU     10h
UE_IP           EQU     12h
UE_CS           EQU     14h
UE_F            EQU     16h
UE_LEN          EQU     18h

net_user:
        mov     [net_fn],ax
        push    bp
        sub     sp,UE_LEN
        mov     di,sp                   ; SS = DS = this segment
        mov     ax,[bp+F_AX]
        mov     [di+UE_AX],ax
        mov     ax,[bp+F_BX]
        mov     [di+UE_BX],ax
        mov     ax,[bp+F_CX]
        mov     [di+UE_CX],ax
        mov     ax,[bp+F_DX]
        mov     [di+UE_DX],ax
        mov     ax,[bp+F_SI]
        mov     [di+UE_SI],ax
        mov     ax,[bp+F_DI]
        mov     [di+UE_DI],ax
        mov     ax,[bp+F_BP]
        mov     [di+UE_BP],ax
        mov     ax,[bp+F_DS]
        mov     [di+UE_DS],ax
        mov     ax,[bp+F_ES]
        mov     [di+UE_ES],ax
        mov     ax,[bp+F_IP]
        mov     [di+UE_IP],ax
        mov     ax,[bp+F_CS]
        mov     [di+UE_CS],ax
        mov     ax,[bp+F_FL]
        mov     [di+UE_F],ax
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     word ptr es:[RL_SDA + SD_USERSP],di
        mov     word ptr es:[RL_SDA + SD_USERSS],ss
        mov     ax,word ptr cs:[rd_psp] ; the process, as net_call has it
        mov     word ptr es:[RL_SDA + SD_PSP],ax
        mov     word ptr es:[RL_SDA + SD_PROCID],ax
        pop     es
        push    di                      ; the frame, for afterwards
        mov     bx,[bp+F_BX]
        mov     cx,[bp+F_CX]
        mov     dx,[bp+F_DX]
        mov     si,[bp+F_SI]
        mov     di,[bp+F_DI]
        mov     es,[bp+F_ES]
        push    word ptr [bp+F_AX]      ; the caller's AX, last before the INT
        mov     ax,[net_fn]
        mov     ds,[bp+F_DS]
        int     2Fh
        pop     ds                      ; the word - POP keeps the carry
        push    cs
        pop     ds
        pop     di                      ; the frame
        mov     bp,[di+UE_LEN]          ; and ours, under it
        jc      nus_fail
        mov     ax,[di+UE_AX]
        mov     [bp+F_AX],ax
        mov     ax,[di+UE_BX]
        mov     [bp+F_BX],ax
        mov     ax,[di+UE_CX]
        mov     [bp+F_CX],ax
        mov     ax,[di+UE_DX]
        mov     [bp+F_DX],ax
        mov     ax,[di+UE_SI]
        mov     [bp+F_SI],ax
        mov     ax,[di+UE_DI]
        mov     [bp+F_DI],ax
        mov     ax,[di+UE_DS]
        mov     [bp+F_DS],ax
        mov     ax,[di+UE_ES]
        mov     [bp+F_ES],ax
        add     sp,UE_LEN
        pop     bp
        jmp     i21_ok
nus_fail:
        add     sp,UE_LEN
        pop     bp
        jmp     i21_fail

;--- net_pcall - a call about a name or a drive: ES:DI = the THISCDS
;--- net_canon or net_drive left, as DOS's TestNet loads it, and then
;--- net_call.  Registers as net_call leaves them.
net_pcall:
        mov     di,word ptr cs:[rd_lowseg]
        mov     es,di
        mov     di,word ptr es:[RL_SDA + SD_THISCDS]
        jmp     net_call

;--- net_simple - the end of a call that has nothing to give back but
;--- whether it worked: AX = the function, the SDA filled in.
net_simple:
        call    net_pcall
        jc      nsi_fail
        jmp     i21_ok
nsi_fail:
        jmp     i21_fail

;--- net_sft - SI = a redirector's file table entry, AX = the function.
;--- ES:DI and the SDA's THISSFT both name the entry, as DOS passes it.
;--- -> CF, AX, CX and DX as the redirector answered; BX, SI, DI, ES and
;--- DS kept.
net_sft:
        push    bx
        push    si
        push    di
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     word ptr es:[RL_SDA + SD_THISSFT],si
        mov     word ptr es:[RL_SDA + SD_THISSFT + 2],cs
        push    cs
        pop     es
        mov     di,si
        call    net_call
        pop     es
        pop     di
        pop     si
        pop     bx
        ret

;=====================================================================
; net_open - AX = 1116h to open or 1117h to create, [rd_opmode] = the
;            mode to keep in the entry, [net_parm] = the word the
;            redirector is pushed, the name in the SDA (net_canon).
;            -> BX = the new handle; CF and AX = why not.  SI, DI and
;            the rest are kept.
;
; MS-DOS's AccessFile, step for step (DOS6SRC dos/file.asm): a file
; table entry, marked busy; a handle, written to point at it before the
; call is made; the entry's number, the handle and where the handle is,
; into the SDA; the mode into the entry, with the no-inherit bit taken
; out of it; and then the redirector fills the entry in.  DOS takes the
; busy mark off afterwards and only then sets sf_no_inherit, so a
; redirector that writes the whole flags word does not lose it.  A
; failure gives back the entry and the handle.
;=====================================================================
net_open:
        push    cx
        push    dx
        push    si
        push    di
        push    es
        mov     [net_fn],ax
        call    jft_alloc               ; -> BX, a free handle
        jc      nop_nohandles
        mov     [rd_ophnd],bx
        call    sft_alloc               ; -> SI, cleared; AL = its number
        jc      nop_nohandles
        mov     [rd_opidx],al
        mov     word ptr [si+SFT_REF],0FFFFh    ; sf_busy, until it is open
        mov     ax,[rd_opmode]
        and     ax,007Fh
        mov     [si+SFT_MODE],ax

        mov     es,[rd_psp]
        call    jft_ptr                 ; -> ES:DI the table
        add     di,bx                   ; ES:DI = this handle's byte
        mov     al,[rd_opidx]
        mov     es:[di],al
        push    fs
        mov     fs,word ptr cs:[rd_lowseg]
        movzx   ax,al
        mov     word ptr fs:[RL_SDA + SD_SFN],ax
        mov     word ptr fs:[RL_SDA + SD_JFN],bx
        mov     word ptr fs:[RL_SDA + SD_PJFN],di
        mov     word ptr fs:[RL_SDA + SD_PJFN + 2],es
        pop     fs

        mov     ax,[net_fn]
        call    net_sft                 ; ES:DI, THISSFT = the entry
        jc      nop_fail
        mov     word ptr [si+SFT_REF],1
        test    byte ptr [rd_opmode],80h
        jz      nop_inherit
        or      byte ptr [si+SFT_DEVINFO+1],NF_NOINH
nop_inherit:
        mov     bx,[rd_ophnd]
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        clc
        ret
nop_fail:
        mov     word ptr [si+SFT_REF],0
        push    ax
        mov     es,[rd_psp]
        call    jft_ptr
        mov     bx,[rd_ophnd]
        mov     byte ptr es:[bx+di],0FFh
        pop     ax
        jmp     short nop_out
nop_nohandles:
        mov     ax,ERR_NOHANDLES
nop_out:
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        stc
        ret

;=====================================================================
; net_xfer - SI = a redirector's file, AX = 1108h to read or 1109h to
;            write, ES:DI = the buffer, CX = the count.  -> CX = what
;            moved; CF and AX if the redirector failed.  Every other
;            register is kept, all thirty-two bits.
;
; THE BUFFER GOES TO IT IN THE SDA'S DTA, as MS-DOS's $READ and $WRITE
; put it there: the offset brought under 16 and the rest added to the
; segment (Align_Buffer), and the count cut to what is left of that
; segment (SETUP).  THISDPB is the entry's device pointer and BYTPOS
; its position, which SETUP also leaves.  The caller's own DTA is put
; back in the SDA by sda_sync on the way out of the call.
;=====================================================================
net_xfer:
        push    ebx
        push    edx
        push    di
        push    es
        mov     dx,di
        shr     dx,4
        mov     bx,es
        add     bx,dx
        and     di,000Fh
        mov     dx,di
        add     dx,cx
        jnc     nxf_fits
        mov     cx,di                   ; 10000h less the offset
        neg     cx
nxf_fits:
        push    es
        mov     es,word ptr cs:[rd_lowseg]
        mov     word ptr es:[RL_SDA + SD_DTA],di
        mov     word ptr es:[RL_SDA + SD_DTA + 2],bx
        mov     edx,dword ptr [si+SFT_POS]
        mov     dword ptr es:[RL_SDA + SD_BYTPOS],edx
        mov     edx,dword ptr [si+SFT_DEVPTR]
        mov     dword ptr es:[RL_SDA + SD_THISDPB],edx
        pop     es
        call    net_sft
        pop     es
        pop     di
        pop     edx
        pop     ebx
        ret

;--- net_rwfn - 3Fh and 40h, on a redirector's file: AX = 1108h or
;--- 1109h, SI = the entry, BP the frame.  DOS checks the mode it was
;--- opened in first, and a read of nothing is not even passed on; a
;--- write of nothing is, because it truncates.
net_rwfn:
        mov     dl,byte ptr [si+SFT_MODE]
        and     dl,03h
        cmp     ax,1108h
        jne     nrw_write
        cmp     dl,01h                  ; opened to write only
        je      nrw_denied
        cmp     word ptr [bp+F_CX],0
        jne     nrw_go
        mov     word ptr [bp+F_AX],0
        jmp     i21_ok
nrw_write:
        or      dl,dl                   ; opened to read only
        jz      nrw_denied
nrw_go:
        mov     cx,[bp+F_CX]
        mov     es,[bp+F_DS]
        mov     di,[bp+F_DX]
        call    net_xfer
        jc      nrw_fail
        mov     [bp+F_AX],cx
        jmp     i21_ok
nrw_denied:
        mov     ax,ERR_ACCESS
nrw_fail:
        jmp     i21_fail

;=====================================================================
; net_readall - SI = a redirector's file, ES:DI = where, ECX = how many
;               bytes, which may be more than a segment.  -> ECX = what
;               arrived; CF if the redirector failed.  For EXEC, whose
;               image can be bigger than a 16-bit count.  Other registers
;               kept.
;=====================================================================
net_readall:
        push    eax
        push    ebx
        push    edx
        push    di
        push    es
        xor     ebx,ebx                 ; what has arrived so far
nra_more:
        or      ecx,ecx
        jz      nra_done
        mov     edx,ecx                 ; what is still wanted
        mov     ax,di                   ; the pointer brought under 16 again,
        shr     ax,4                    ; so that a 32K piece cannot wrap
        mov     cx,es
        add     cx,ax
        mov     es,cx
        and     di,000Fh
        mov     ecx,edx
        cmp     ecx,8000h
        jbe     nra_piece
        mov     ecx,8000h
nra_piece:
        push    cx
        mov     ax,1108h
        call    net_xfer                ; -> CX
        pop     ax                      ; what was asked
        jc      nra_err
        movzx   ecx,cx
        add     ebx,ecx
        add     di,cx
        sub     edx,ecx
        cmp     cx,ax
        jb      nra_done                ; short: the end of the file
        mov     ecx,edx
        jmp     short nra_more
nra_done:
        mov     ecx,ebx
        pop     es
        pop     di
        pop     edx
        pop     ebx
        pop     eax
        clc
        ret
nra_err:
        mov     ecx,ebx
        pop     es
        pop     di
        pop     edx
        pop     ebx
        pop     eax
        stc
        ret

;=====================================================================
; net_find - AX = 111Bh (find first, the name and SATTRIB already in
;            the SDA) or 111Ch (find next, the search's twenty-one
;            bytes already in SEARCHBUF).  Answers the caller's DTA and
;            ends the INT 21h call.
;
; $FIND_FIRST and $FIND_NEXT point the DTA at SEARCHBUF in the SDA for
; the length of the redirector's call, and the redirector leaves there
; twenty-one bytes of its own search state - the first with bit 7 set,
; which is how 4Fh knows later whose search it is - and then the
; directory entry it found.  DOS copies the state to the caller's DTA
; and turns the entry into the rest of it: attribute, time, date, size
; and the name as text (FindSet and PackName in DOS6SRC dos/search.asm).
;=====================================================================
net_find:
        mov     di,word ptr cs:[rd_lowseg]
        mov     es,di
        mov     word ptr es:[RL_SDA + SD_DTA],RL_SDA + SD_SRCH
        mov     word ptr es:[RL_SDA + SD_DTA + 2],es
        mov     di,RL_SDA + SD_SRCH     ; ES:DI = the DTA for find next, as
        cmp     ax,111Ch                ; DOS_SEARCH_NEXT loads it, and the
        je      nfd_call                ; CDS for find first (TestNet's)
        mov     di,word ptr es:[RL_SDA + SD_THISCDS]
nfd_call:
        call    net_call
        jc      nfd_fail
        push    ds
        mov     es,word ptr cs:[rd_dtaseg]
        mov     di,word ptr cs:[rd_dtaoff]
        mov     ds,word ptr cs:[rd_lowseg]
        mov     si,RL_SDA + SD_SRCH
        mov     cx,21
        cld
        rep     movsb                   ; the search, for 4Fh
        mov     al,[si+DE_ATTR]         ; and DS:SI is the entry
        stosb
        mov     ax,[si+DE_TIME]
        stosw
        mov     ax,[si+DE_DATE]
        stosw
        mov     ax,[si+DE_SIZE]
        stosw
        mov     ax,[si+DE_SIZE+2]
        stosw
        call    net_pack
        pop     ds
        jmp     i21_ok
nfd_fail:
        jmp     i21_fail

;--- net_findnext - 4Fh, for a search a redirector started (the DTA's
;--- first byte has bit 7 set): its twenty-one bytes back into
;--- SEARCHBUF, as $FIND_NEXT copies them, and 111Ch.  Ends the call.
net_findnext:
        push    ds
        mov     es,word ptr cs:[rd_lowseg]
        mov     di,RL_SDA + SD_SRCH
        mov     si,word ptr cs:[rd_dtaoff]
        mov     ds,word ptr cs:[rd_dtaseg]
        mov     cx,21
        cld
        rep     movsb
        pop     ds
        mov     ax,111Ch
        jmp     net_find

;=====================================================================
; net_chdir - 3Bh, the name a redirector's (net_canon).  It says whether
;             the directory is there (1105h), and then the whole name,
;             "D:\DIR", becomes the drive's text - which is all $CHDIR
;             does for a network drive: the text from WFP_START, and the
;             cluster word left as the redirector keeps it (DOS6SRC
;             dos/path.asm).  The name has to fit the structure, 67 bytes
;             with its NUL, and may not hold a wildcard.  Ends the call.
;=====================================================================
net_chdir:
        call    net_nometa
        jc      ncd_nopath
        mov     si,OFFSET rd_canbuf
        mov     cx,67
ncd_len:
        cmp     byte ptr [si],0
        je      ncd_fits
        inc     si
        loop    ncd_len
ncd_nopath:
        mov     ax,ERR_NOPATH
        jmp     i21_fail
ncd_fits:
        mov     ax,1105h
        call    net_pcall
        jc      ncd_fail
        push    ds
        mov     ax,word ptr cs:[rd_lowseg]
        mov     ds,ax
        mov     es,ax
        mov     di,word ptr ds:[RL_SDA + SD_THISCDS]
        mov     si,word ptr ds:[RL_SDA + SD_WFP]
        cld
ncd_copy:
        lodsb
        stosb
        or      al,al
        jnz     ncd_copy
        pop     ds
        jmp     i21_ok
ncd_fail:
        jmp     i21_fail

;--- net_pack - DS:SI -> an entry's eleven-byte name, ES:DI -> where the
;--- text goes: "NAME.EXT", the blanks dropped, and a NUL.  DOS's
;--- PackName.  SI, DI and AX, CX are lost.
net_pack:
        mov     cx,8
npk_base:
        lodsb
        cmp     al,' '
        je      npk_baseend
        stosb
        loop    npk_base
        jmp     short npk_ext
npk_baseend:
        dec     cx
        add     si,cx                   ; past the rest of the blanks
npk_ext:
        cmp     word ptr [si],'  '
        jne     npk_dot
        cmp     byte ptr [si+2],' '
        je      npk_end
npk_dot:
        mov     al,'.'
        stosb
        mov     cx,3
npk_extc:
        lodsb
        cmp     al,' '
        je      npk_end
        stosb
        loop    npk_extc
npk_end:
        xor     al,al
        stosb
        ret

;=====================================================================
; net_dinfo - the redirector's drive named by the SDA's THISCDS: how big
;             it is.  -> AL = sectors per cluster, AH = media byte, BX =
;             clusters in all, CX = bytes per sector, DX = clusters free;
;             CF if it would not say.  ES:DI is the structure, as DOS's
;             DISK_INFO passes it.
;=====================================================================
net_dinfo:
        push    di
        push    es
        mov     ax,110Ch
        call    net_pcall
        pop     es
        pop     di
        ret

;=====================================================================
; Data
;=====================================================================
net_parm    dw  0                       ; the word pushed for the redirector
net_fn      dw  0                       ; net_open's function, across the
                                        ; allocations
net_media   db  0                       ; 1Bh/1Ch's media byte, for a
                                        ; redirector's drive
