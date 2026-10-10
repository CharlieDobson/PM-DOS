;=====================================================================
; VDM.INC - the DOS side of the virtual DOS machine
;
; PMIO's monitor knows how to run 16-bit code and nothing about DOS;
; this is where the two meet.  The pattern is the exit loop: run the
; guest, get handed the interrupt it executed, answer it, run it again.
;
; THE IMPORTANT DECISION HERE is that a V86 guest's INT 21h is served
; by the kernel's OWN handlers - the same i21_table the 32-bit side
; dispatches through.  DOS is not reimplemented for 16-bit callers.
; All that stands between them is a translation layer: the guest says
; DS:DX and the kernel wants one flat address, so the bridge multiplies
; the segment by sixteen and hands the same handler the same request.
; That is why opening a file, reading it, DIR-style searches and the
; long-name calls all worked the day the bridge did - they are the
; code that was already there and already tested.
;
; A handful of functions cannot be passed through, and they are the
; ones where "a program" means something different on each side:
;   48h/49h/4Ah  memory - the guest's arena is the DOS one below 1MB
;                (LOWMEM.INC), not the kernel's 32-bit arena
;   00h/4Ch/31h  terminate - ends the VM, not a 32-bit program
;   62h          get PSP - the guest's PSP is a real-mode segment
;   1Ah/2Fh      the DTA is a far pointer to the guest
;   4Bh          EXEC - a child runs in the same box, on a nesting
;                stack; the kernel's own 4Bh is for 32-bit programs
;   4Dh          the child's exit code, not the 32-bit side's
;   31h/27h      terminate and stay resident - the one exit that must
;                NOT give the memory back
;   25h/35h      vectors - the guest means the real-mode IVT at 0:0,
;                the kernel means a protected-mode IDT gate
;   42h          seek - CX:DX in and DX:AX out are each one 32-bit
;                number split across two registers
;
; NESTING, in one paragraph.  There is one live set of variables -
; vdm_ctx, vdm_pspseg, vdm_curpsp, cur_own, dta_ptr - and they always
; describe the program that is actually running.  A child pushes the
; parent's copies onto vdm_stack and overwrites them; the child's exit
; pops them back and lets the parent's 4Bh return.  The exit loop below
; never left, so nothing recursed and the kernel stack never grew:
; termination at depth 1 ends the box, and at any greater depth it is
; just a return.
;
; Included by DOSMAIN.ASM.
;=====================================================================

;=====================================================================
; k21 - call the kernel's own INT 21h handler from kernel code.
;
; AH selects, the other registers are as that function wants them,
; and every pointer is FLAT.  Returns with the result registers in
; k21_e?? and CF meaning what it always means.
;
; The handlers read and write their caller's registers through a frame
; at EBP, so the frame is what has to be manufactured: pushad's eight
; dwords, the saved EBP, then EIP/CS/FLAGS.  Forty-eight bytes of
; stack and the whole DOS API is callable from anywhere.
;=====================================================================
; k21u is the same door with a label on it: the registers below came
; from a DOS BOX, so the pointers in them are the guest's rather than
; the kernel's.  It matters to exactly one thing - buf_ok, which has
; to refuse a guest a buffer inside PMDOS.SYS and hand the kernel the
; same buffer without complaint - and the answer travels in the frame
; rather than in a global because these calls NEST: a guest's 4Bh runs
; exec_kind, which calls k21 for itself while the guest's own call is
; still open.  A flag would have to be saved and put back at every one
; of those points; a frame field is already per-call, and stacks.
k21u:
        push    ebp
        mov     ebp,1
        jmp     short k21_body
k21:
        push    ebp
        xor     ebp,ebp
k21_body:
        push    ebx
        push    ecx
        push    edx
        push    esi
        push    edi

        push    0                       ; F_FLAGS
        push    SEL_CODE32              ; F_CS - the kernel's own, so that
                                        ; anything asking "was this a DPMI
                                        ; client?" gets the right answer
        push    ebp                     ; F_EIP - 0 from the kernel, 1 from
                                        ; a DOS box.  A manufactured frame
                                        ; has no instruction pointer to put
                                        ; here, so the field was spare
        push    0                       ; F_DS  } the two an interrupt gate
        push    0                       ; F_ES  } does not reload, and which
                                        ; a manufactured frame has no caller
                                        ; to take them from
        push    0                       ; the saved EBP slot
        push    eax                     ; F_EAX
        push    ecx                     ; F_ECX
        push    edx                     ; F_EDX
        push    ebx                     ; F_EBX
        push    0                       ; F_ESP
        push    0                       ; F_EBP
        push    esi                     ; F_ESI
        push    edi                     ; F_EDI
        mov     ebp,esp
        cld
        inc     byte ptr ds:[DOS_INDOS] ; a guest's call is a DOS call

        ; A DOS PROGRAM'S HANDLES ARE ITS OWN TABLE'S, and the kernel's
        ; are slots: JFT.INC turns the one into the other on the way in
        ; and back on the way out, and does dup, close and 67h itself.
        ; Only for k21u - the kernel's own k21 calls use slots.
        cmp     dword ptr [ebp+F_EIP],1
        jne     k21_disp
        cmp     dword ptr ds:[vdm_gframe],-1    ; vdm_do21's own call: this
        jne     k21_jft                         ; is the frame a ^C in it
        mov     dword ptr ds:[vdm_gframe],ebp   ; abandons - see vdm_ctrlc
k21_jft:
        call    jft_pre
        jc      k21_done                ; done there, answer and all
k21_disp:

        ; The SAME dispatch as int21_entry, and it has to be: this is
        ; the door a DOS box comes in through, and any family missing
        ; from here is a family that answers "invalid function" to a
        ; guest and works perfectly from 32-bit code.  73h, F0h and F1h
        ; were missing, which is why a guest could not move the cursor
        ; through the kernel, ask for free space the FAT32 way, or read
        ; the volume table - three calls that all appeared to exist.
        movzx   eax,byte ptr [ebp+F_EAX+1]
        cmp     eax,71h
        je      k21_lfn
        cmp     eax,73h
        je      k21_fat32
        cmp     eax,0F0h
        je      k21_ext
        cmp     eax,0F1h
        je      k21_ext1
        cmp     eax,I21_MAXFN
        jae     k21_bad
        call    dword ptr [i21_table+eax*4]
        jmp     short k21_done
k21_lfn:
        call    fn71
        jmp     short k21_done
k21_fat32:
        call    fn73
        jmp     short k21_done
k21_ext:
        call    fnF0
        jmp     short k21_done
k21_ext1:
        call    fnF1
        jmp     short k21_done
k21_bad:
        call    i21_unsup
k21_done:
        call    jft_post                ; a no-op unless jft_pre asked
        dec     byte ptr ds:[DOS_INDOS]
        mov     eax,[ebp+F_EAX]
        mov     dword ptr ds:[k21_eax],eax
        mov     eax,[ebp+F_EBX]
        mov     dword ptr ds:[k21_ebx],eax
        mov     eax,[ebp+F_ECX]
        mov     dword ptr ds:[k21_ecx],eax
        mov     eax,[ebp+F_EDX]
        mov     dword ptr ds:[k21_edx],eax
        mov     eax,[ebp+F_ESI]
        mov     dword ptr ds:[k21_esi],eax
        mov     eax,[ebp+F_EDI]
        mov     dword ptr ds:[k21_edi],eax
        mov     eax,[ebp+F_FLAGS]
        mov     dword ptr ds:[k21_flg],eax

        add     esp,56                  ; the twelve dwords of frame plus the
                                        ; two segment slots above it
        pop     edi
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        pop     ebp
        mov     eax,dword ptr ds:[k21_eax]
        test    dword ptr ds:[k21_flg],FL_CF
        jnz     k21_cf
        clc
        ret
k21_cf:
        stc
        ret

;=====================================================================
; vdm_psp - build a real-mode PSP at segment AX, owning DX paragraphs.
;
; The fields that matter are the ones programs actually read: the
; INT 20h opcode at the front (so a .COM that RETs still terminates),
; the top of memory, the parent, the saved vectors, the environment,
; and the command tail with its two parsed FCBs.  The rest is
; furniture, and it is here because a program that pokes at 5Ch
; expecting an FCB should find zeros rather than somebody else's data.
;
; The saved vectors are taken from the REAL-MODE IVT, not from the
; kernel's IDT: they are what the guest will put back if it ever runs
; its own INT 22h/23h/24h restore, and a guest means the vectors the
; monitor reflects through.
;=====================================================================
vdm_psp:
        pushad
        movzx   edi,ax
        shl     edi,4                           ; flat address of the PSP
        push    edi
        mov     ecx,RSP_SIZE / 4                ; a clean slate first
        push    edi
        pop     ebx
        xor     eax,eax
        push    edi
        rep     stosd
        pop     edi

        mov     word ptr [edi+RSP_INT20],20CDh  ; INT 20h
        movzx   eax,dx                          ; top of our memory,
        mov     ebx,edi                         ; as a paragraph
        shr     ebx,4
        add     eax,ebx
        mov     [edi+RSP_MEMTOP],ax
        mov     byte ptr [edi+RSP_DISP],0CBh    ; a bare RETF: the 5Ch
                                                ; far-call entry is a
                                                ; museum piece, but it
                                                ; must not execute rubbish
        ; The parent, which is zero for the program the shell started -
        ; a 32-bit shell is not a real-mode PSP and there is nothing
        ; honest to put here - and the caller's PSP for an EXEC'd
        ; child.  Programs read it: an overlay manager walks the chain,
        ; and a TSR uses it to tell "loaded from the command line" from
        ; "loaded by an installer".
        mov     ax,word ptr ds:[vdm_parpsp]
        mov     [edi+RSP_PARENT],ax
        mov     ax,word ptr ds:[vdm_envseg]
        mov     [edi+RSP_ENV],ax
        mov     word ptr [edi+RSP_JFTLEN],20

        mov     eax,dword ptr ds:[22h*4]        ; the guest's own vectors,
        mov     [edi+RSP_TERM],eax              ; out of the IVT the monitor
        mov     eax,dword ptr ds:[23h*4]        ; reflects through
        mov     [edi+RSP_CTRLC],eax
        mov     eax,dword ptr ds:[24h*4]
        mov     [edi+RSP_CRITERR],eax
        ; THE TABLE'S ADDRESS IS A FAR POINTER, the PSP's segment and
        ; 18h.  It used to be the flat address stored as a dword, which
        ; reads back as 0000:flat and so pointed at the right byte only
        ; while the PSP sat below 64K; above it the offset wrapped.
        ; COMMAND.COM follows this pointer in its critical-error handler,
        ; MS-DOS's and PM-DOS's alike.
        mov     eax,edi
        shl     eax,12                          ; flat >> 4 = the segment,
        mov     ax,RSP_JFT                      ; into the high word
        mov     [edi+RSP_JFTPTR],eax
        mov     word ptr [edi+RSP_DOSCALL],21CDh
        mov     byte ptr [edi+RSP_DOSCALL+2],0CBh

        ; The handles: the parent's, or CMD.EXE's five - see JFT.INC.
        call    jft_inherit
        mov     byte ptr ds:[vdm_jftmade],1     ; vl_undo gives them back

;--- the command tail, and the two FCBs parsed out of it -------------
        mov     byte ptr [edi+RSP_TAIL],0       ; an empty tail, until
        mov     byte ptr [edi+RSP_TAIL+1],0Dh   ; there is one to copy
        mov     esi,dword ptr ds:[vdm_tailp]
        or      esi,esi
        jz      vp_notail
        movzx   ecx,byte ptr [esi]
        cmp     ecx,7Eh                         ; 126 and the CR fill 80h-FFh
        jbe     vp_tlen
        mov     ecx,7Eh
vp_tlen:
        push    edi
        add     edi,RSP_TAIL
        mov     [edi],cl
        inc     edi
        inc     esi
        rep     movsb
        mov     byte ptr [edi],0Dh
        pop     edi
vp_notail:
        call    vdm_fcbs

;--- and the environment block belongs to us now ---------------------
; It was allocated before the program's own block and owns itself; now
; that there is a PSP, hand it over, so that the exit sweep takes it
; back without the guest having to free it.
        movzx   eax,word ptr ds:[vdm_envseg]
        or      eax,eax
        jz      vp_noenv
        mov     edx,edi
        shr     edx,4                           ; the PSP, as a segment
        call    lm_setown
vp_noenv:
        pop     edi
        popad
        ret

;=====================================================================
; vdm_fcbs - EDI -> the PSP.  Parse the first two arguments of the
;            command tail into the FCBs at 5Ch and 6Ch and leave the
;            two result codes in [vdm_fcbax], which is what the guest
;            finds in AX when it starts.
;
; This is furniture from 1981 that programs still read: an .EXE built
; by any Microsoft language checks AL for FFh to reject "PROG X:" when
; X: is not a drive, and a good deal of software written for FCBs
; never parses its own tail at all.  The codes are DOS's: FFh if the
; argument named a drive that does not exist, 01h if it holds a
; wildcard, 00h otherwise.
;=====================================================================
vdm_fcbs:
        pushad
        lea     esi,[edi+RSP_TAIL+1]            ; past the length byte
        push    edi
        add     edi,RSP_FCB1
        call    vdm_fcb1
        mov     byte ptr ds:[vdm_fcbax],al
        pop     edi
        add     edi,RSP_FCB2
        call    vdm_fcb1
        mov     byte ptr ds:[vdm_fcbax+1],al
        popad
        ret

;---------------------------------------------------------------------
; vdm_fcb1 - parse one argument at ESI into the FCB at EDI, advancing
;            ESI past it.  AL = the result code.
;---------------------------------------------------------------------
vdm_fcb1:
        push    ebx
        push    ecx
        push    edx
        push    edi
        xor     ebx,ebx                         ; the result code so far

        mov     byte ptr [edi],0                ; drive: the default one
        push    edi
        inc     edi
        mov     ecx,11                          ; name and extension, blank
        mov     al,' '                          ; padded, as an FCB wants
        rep     stosb
        ; SIXTEEN BYTES, not the thirty-seven an open FCB occupies: the
        ; two in a PSP are sixteen apart and the second one ends four
        ; bytes short of the command tail.  Zeroing a whole FCB here
        ; would wipe the tail it was parsed from.
        mov     ecx,4
        xor     al,al
        rep     stosb
        pop     edi

vf_skip:
        mov     al,[esi]                        ; leading whitespace is not
        cmp     al,' '                          ; part of the argument
        je      vf_step
        cmp     al,9
        jne     vf_drive
vf_step:
        inc     esi
        jmp     short vf_skip

;--- "X:" in front of it ---------------------------------------------
vf_drive:
        cmp     byte ptr [esi+1],':'
        jne     vf_name
        mov     al,[esi]
        call    vdm_upper
        sub     al,'A'
        jb      vf_baddrv
        cmp     al,MAX_DRIVES
        jae     vf_baddrv
        inc     al                              ; 1 = A:, DOS's numbering
        mov     [edi],al
        add     esi,2
        jmp     short vf_name
vf_baddrv:
        mov     bl,0FFh                         ; there is no such drive
        add     esi,2

;--- the name, then the extension ------------------------------------
vf_name:
        push    edi
        inc     edi
        mov     ecx,8
        call    vdm_fcbpart
        pop     edi
        cmp     byte ptr [esi],'.'
        jne     vf_done
        inc     esi
        push    edi
        add     edi,9
        mov     ecx,3
        call    vdm_fcbpart
        pop     edi
vf_done:
        mov     al,bl
        pop     edi
        pop     edx
        pop     ecx
        pop     ebx
        ret

;---------------------------------------------------------------------
; vdm_fcbpart - copy up to ECX characters of a name from ESI to EDI,
;               uppercased, stopping at a separator.  '*' fills the
;               rest of the field with '?', which is what DOS's own
;               parser does and why "*.*" arrives as eleven of them.
;               BL collects the result code.
;---------------------------------------------------------------------
vdm_fcbpart:
        or      ecx,ecx
        jz      vfp_eat
        mov     al,[esi]
        call    vdm_sep
        jc      vfp_end
        cmp     al,'*'
        je      vfp_star
        cmp     al,'?'
        jne     vfp_ch
        or      bl,bl                           ; a wildcard, unless the
        jnz     vfp_ch                          ; drive was already wrong
        mov     bl,1
vfp_ch:
        call    vdm_upper
        mov     [edi],al
        inc     edi
        inc     esi
        dec     ecx
        jmp     short vdm_fcbpart
vfp_star:
        or      bl,bl
        jnz     vfp_sfill
        mov     bl,1
vfp_sfill:
        mov     al,'?'
        rep     stosb
        inc     esi
        ; and everything up to the separator goes with it
vfp_eat:
        mov     al,[esi]
        call    vdm_sep                         ; '.' is one of them, so an
        jc      vfp_end                         ; extension still gets its turn
        inc     esi
        jmp     short vfp_eat
vfp_end:
        ret

;---------------------------------------------------------------------
; vdm_sep - CF set if AL ends a filename argument.  The list is DOS's,
;           and it is longer than it looks: everything that could ever
;           separate two things on a command line is in it.
;---------------------------------------------------------------------
vdm_sep:
        cmp     al,0
        je      vsp_yes
        cmp     al,0Dh
        je      vsp_yes
        cmp     al,' '
        je      vsp_yes
        cmp     al,9
        je      vsp_yes
        cmp     al,'.'
        je      vsp_yes
        cmp     al,':'
        je      vsp_yes
        cmp     al,';'
        je      vsp_yes
        cmp     al,','
        je      vsp_yes
        cmp     al,'='
        je      vsp_yes
        cmp     al,'+'
        je      vsp_yes
        cmp     al,'/'
        je      vsp_yes
        cmp     al,'"'
        je      vsp_yes
        cmp     al,'['
        je      vsp_yes
        cmp     al,']'
        je      vsp_yes
        cmp     al,'|'
        je      vsp_yes
        cmp     al,'<'
        je      vsp_yes
        cmp     al,'>'
        je      vsp_yes
        clc
        ret
vsp_yes:
        stc
        ret

vdm_upper:
        cmp     al,'a'
        jb      vu_out
        cmp     al,'z'
        ja      vu_out
        sub     al,20h
vu_out:
        ret

;=====================================================================
; vdm_env - build the guest's environment block in conventional memory.
;
; A DOS program is handed its environment as a block of its own that
; it is free to walk: the strings back to back, each NUL-terminated,
; an empty string to end them, and then - since DOS 3.0 - a word 0001h
; followed by the full path of the program itself.  That path is where
; argv[0] comes from in every C runtime of the era and how a program
; finds the files it shipped with, so it is worth getting right: the
; path is put through truename rather than copied as typed, because
; "PROG" typed at the prompt has to arrive as C:\SOMEWHERE\PROG.COM.
;
; The 32-bit side keeps its environment as flat text (COMMAND.COM's
; env_blk) and the parameter block hands over a flat pointer to it;
; the guest's copy is made here, below 1MB, where a 16-bit program can
; reach it.  The two are copies, not one block seen twice - which is
; also true of real DOS, and is why SET inside a program never changes
; the shell's environment.
;
; Failing to build one is NOT fatal: a program with no environment is
; a program from DOS 1, and there were plenty.
;=====================================================================
VDM_ENVMAX      EQU     8192            ; a runaway block is not copied

vdm_env:
        pushad
        mov     word ptr ds:[vdm_envseg],0

;--- the program's own path, fully qualified -------------------------
        mov     esi,dword ptr ds:[vdm_pathp]
        mov     edi,OFFSET vdm_envpath
        mov     byte ptr [edi],0
        xor     ecx,ecx                         ; CL = 0: the canonical form
        mov     eax,7160h
        call    k21
        jnc     ve_named
        mov     esi,dword ptr ds:[vdm_pathp]    ; truename can refuse a path
        mov     edi,OFFSET vdm_envpath          ; the open is about to refuse
        mov     ecx,EXEC_PATH_MAX - 1           ; too; use it as typed and
        call    exec_cpz                        ; let that be the error
ve_named:

;--- how much is there? ----------------------------------------------
        xor     ecx,ecx
        mov     esi,dword ptr ds:[vdm_envp]
        or      esi,esi
        jz      ve_sized
ve_str:
        cmp     byte ptr [esi],0                ; the empty string ends them
        je      ve_sized
ve_ch:
        inc     esi
        inc     ecx
        cmp     ecx,VDM_ENVMAX
        jae     ve_sized
        cmp     byte ptr [esi],0
        jne     ve_ch
        inc     esi                             ; the string's own terminator
        inc     ecx
        jmp     short ve_str
ve_sized:
        mov     dword ptr ds:[vdm_envstr],ecx

        mov     esi,OFFSET vdm_envpath
        call    vdm_slen
        add     ecx,eax
        add     ecx,4                           ; the empty string, the word
        mov     ebx,ecx                         ; 0001h, and the path's NUL
        add     ebx,15
        shr     ebx,4
        call    lm_alloc
        jc      ve_none
        mov     word ptr ds:[vdm_envseg],ax

;--- and write it ----------------------------------------------------
        movzx   edi,ax
        shl     edi,4
        mov     esi,dword ptr ds:[vdm_envp]
        mov     ecx,dword ptr ds:[vdm_envstr]
        jecxz   ve_noset
        rep     movsb
ve_noset:
        xor     al,al
        stosb                                   ; the empty string
        mov     ax,1                            ; how many things follow
        stosw
        mov     esi,OFFSET vdm_envpath
ve_path:
        lodsb
        stosb
        or      al,al
        jnz     ve_path
ve_none:
        popad
        ret

;---------------------------------------------------------------------
; vdm_slen - EAX = the length of the ASCIIZ string at ESI, terminator
;            not counted.  ESI is left where it was.
;---------------------------------------------------------------------
vdm_slen:
        push    esi
        xor     eax,eax
vs_go:
        cmp     byte ptr [esi],0
        je      vs_end
        inc     esi
        inc     eax
        jmp     short vs_go
vs_end:
        pop     esi
        ret

;=====================================================================
; vdm_load - EDX -> an ASCIIZ path.  Load the program and leave vdm_ctx
;            ready to run.  CF on any failure.
;
; WHICH KIND OF PROGRAM IT IS COMES OUT OF THE FILE, NOT THE NAME.  DOS
; has always worked that way and it matters: LINK will happily produce
; an .EXE called SOMETHING.COM, several utilities shipped exactly like
; that, and a loader that trusts the extension runs the MZ header as
; code.  So: read the first 28 bytes, look for the signature, and let
; the answer decide.  The extension is a hint for humans.
;=====================================================================
vdm_load:
        pushad
        mov     byte ptr ds:[vdm_jftmade],0     ; no handles taken yet
        mov     word ptr ds:[vdm_pspseg],0      ; so a failure before the
        mov     word ptr ds:[vdm_envseg],0      ; allocations does not hand
                                                ; the cleanup a stale segment
        mov     word ptr ds:[vdm_fcbax],0
        mov     word ptr ds:[vdm_err],ERR_NOFILE
        mov     dword ptr ds:[vdm_pathp],edx

        ; The environment FIRST, and the order matters: a .COM is given
        ; every free paragraph there is, so anything allocated after it
        ; would find nothing left.
        call    vdm_env

        mov     edx,dword ptr ds:[vdm_pathp]
        mov     eax,3D00h                       ; open for reading
        call    k21
        jc      vl_undo
        mov     dword ptr ds:[vdm_hdl],eax

        mov     ebx,eax
        mov     edx,OFFSET vdm_hdrbuf
        mov     ecx,EXE_HDRLEN
        mov     eax,3F00h
        call    k21
        jc      vl_close
        cmp     eax,EXE_HDRLEN
        jb      vl_com                          ; too short to be an .EXE
        mov     ax,word ptr ds:[vdm_hdrbuf+EXE_SIG]
        cmp     ax,EXE_MZ
        je      vl_exe
        cmp     ax,EXE_ZM
        je      vl_exe

;=====================================================================
; A .COM is the easy one: no header, no relocation, it just goes down
; at PSP:0100 and gets the whole block.  DOS handed a .COM every free
; paragraph it had, and programs rely on that - it is where they put
; their own heap - so this does too.
;=====================================================================
vl_com:
        mov     ebx,dword ptr ds:[vdm_hdl]      ; unread the sniff
        xor     ecx,ecx
        mov     eax,4200h
        call    k21
        jc      vl_close

        ; HOW SMALL IS TOO SMALL?  The PSP, the image, and enough stack
        ; for the program to make one call - which is what MS-DOS has
        ; always insisted on before it will load a .COM at all, and
        ; which nothing here had to ask while there was only one place
        ; to put it.  lm_allocbig then hands over the largest block in
        ; the region the strategy names, so a .COM still gets every
        ; paragraph of wherever it lands.
        mov     ebx,dword ptr ds:[vdm_hdl]
        xor     ecx,ecx
        mov     eax,4202h                       ; seek to the end: the size
        call    k21
        jc      vl_close
        ; EAX IS THE WHOLE OF IT.  42h hands a DOS program back DX:AX
        ; and hands the kernel back a flat 32-bit position in EAX -
        ; see fn42 - so recombining a DX that nothing wrote is how the
        ; size of every .COM in the machine became a large number and
        ; every one of them stopped loading.
        add     eax,15
        shr     eax,4
        add     eax,10h + 10h                   ; the PSP, and 256 bytes of
        mov     esi,eax                         ; stack behind it

        mov     ebx,dword ptr ds:[vdm_hdl]      ; and back to the start
        xor     ecx,ecx
        mov     eax,4200h
        call    k21
        jc      vl_close

        mov     ebx,esi
        call    lm_allocbig
        jc      vl_nomem
vl_got:
        mov     word ptr ds:[vdm_pspseg],ax

        movzx   edx,ax                          ; how big did we get?
        dec     edx
        shl     edx,4
        movzx   edx,word ptr [edx+RMCB_LEN]
        mov     word ptr ds:[vdm_paras],dx
        call    vdm_psp                         ; AX = seg, DX = paragraphs

        movzx   eax,word ptr ds:[vdm_pspseg]    ; and the block wears the
        mov     esi,dword ptr ds:[vdm_pathp]    ; program.s name, as DOS 4
        call    lm_setname                      ; onwards has done

        movzx   edi,word ptr ds:[vdm_pspseg]    ; read the image in behind it
        shl     edi,4
        add     edi,100h
        mov     edx,edi
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,0FF00h                      ; a .COM cannot exceed 64K
        mov     eax,3F00h
        call    k21
        jc      vl_close

        mov     ebx,dword ptr ds:[vdm_hdl]      ; done with the file
        mov     eax,3E00h
        call    k21

;--- the context a .COM expects --------------------------------------
        mov     edi,OFFSET vdm_ctx
        xor     eax,eax
        mov     ecx,V86C_SIZE / 4
        rep     stosd

        movzx   eax,word ptr ds:[vdm_pspseg]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        mov     dword ptr [vdm_ctx+V86C_DS],eax
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        mov     dword ptr [vdm_ctx+V86C_SS],eax
        mov     dword ptr [vdm_ctx+V86C_EIP],100h
        mov     dword ptr [vdm_ctx+V86C_FLAGS],EFL_IF

        ; The stack goes at the top of the block, or the top of the
        ; segment if the block is bigger than one - and a zero word is
        ; pushed so that a .COM ending in RET lands on the PSP's INT 20h.
        movzx   eax,word ptr ds:[vdm_paras]
        shl     eax,4
        add     eax,100h - 2                    ; PSP included
        cmp     eax,0FFFEh
        jbe     vl_sp
        mov     eax,0FFFEh
vl_sp:
        mov     dword ptr [vdm_ctx+V86C_ESP],eax
        movzx   edi,word ptr ds:[vdm_pspseg]
        shl     edi,4
        add     edi,eax
        mov     word ptr [edi],0

        ; AL and AH are the two FCB parse codes, from the command tail
        movzx   eax,word ptr ds:[vdm_fcbax]
        mov     dword ptr [vdm_ctx+V86C_EAX],eax

        ; 20h-2Fh belong to us; everything else reflects to the BIOS
        call    vdm_traps
        popad
        clc
        ret

vl_nomem:
        mov     word ptr ds:[vdm_err],ERR_NOMEM
;---------------------------------------------------------------------
; Every way out that failed comes through here, and all of them have
; the file open: a load that gives up without closing it leaks a
; handle per attempt, and COMMAND tries four extensions before it
; decides a name is not a program.
;---------------------------------------------------------------------
vl_close:
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3E00h
        call    k21
vl_undo:
        ; The environment goes back by ADDRESS and it goes back FIRST,
        ; while it is still a block of its own.  Once vdm_psp has run
        ; it belongs to the PSP, and the owner sweep below would leave
        ; this code freeing something that is no longer there.
        movzx   eax,word ptr ds:[vdm_envseg]
        or      eax,eax
        jz      vl_u1
        call    lm_free
vl_u1:
        mov     word ptr ds:[vdm_envseg],0
        movzx   eax,word ptr ds:[vdm_pspseg]
        or      eax,eax
        jz      vl_fail
        cmp     byte ptr ds:[vdm_jftmade],0
        je      vl_nojft
        call    jft_exit                ; the handles vdm_psp inherited -
                                        ; a PSP that never runs never exits
vl_nojft:
        call    lm_freeown
        mov     word ptr ds:[vdm_pspseg],0
vl_fail:
        popad
        stc
        ret

;=====================================================================
; vl_winres - the program vl_exe is about to load is named at
;             [vdm_pathp].  If it is WIN386.EXE, reserve the segment
;             RMDOS.SYS will park in FIRST, so that it lies below the
;             block WIN386 is given.
;
; WHY THERE IS NOTHING FREE WHEN WINDOWS ASKS.  WIN386.EXE is linked
; with MAXALLOC = FFFFh, so it gets every paragraph in the box - and
; keeps them: its INT 2Fh 1605h comes with nothing free, the same thing
; v0.49 learned of DOSX's 4Ah.  DOSX asks for a fixed 14E8h and leaves
; the rest, which is why standard mode never met this.  A real DOS has
; nothing to load at that moment, its EMM386 being resident; PM-DOS has
; RMDOS, and the HMA will not do for this customer: V86MMGR takes XMS
; over by planting a breakpoint on the driver's entry, which has to be
; below 1MB, and KRNL386 finds the file table by scanning memory below
; 512K.  Parked in the HMA, Windows says "Cannot install the 386
; enhanced mode XMS driver" and leaves.
;
; WHY BELOW WIN386 AND NOT ABOVE.  Handing WIN386 less than everything
; and parking RMDOS in what was left was tried first, and Windows then
; stopped with "Insufficient memory or address space to initialize
; Windows in 386 enhanced mode".  V86MMGR divides conventional memory
; at WIN386's own PSP: everything below it was there before Windows and
; is GLOBAL, one copy seen by every virtual machine; everything from
; the PSP up is LOCAL, and each machine gets its own.  A DOS above that
; line is a DOS per machine, each with its own file table and its own
; idea of the disk, which is not a thing V86MMGR will set up.  A real
; DOS is below the line because it was loaded first, and this puts
; RMDOS's segment there for the same reason: it is allocated before
; WIN386's block, first fit, so WIN386 lands above it.
;
; The block is owned by the program loading WIN386 - WIN.COM - and
; wears RMDOS's name.  park_loadrm takes it if it is still that block;
; if the park never comes, WIN.COM's exit frees it like any other.  The
; name is the whole of the test for WIN386.EXE, which is also how
; Windows recognises EMM386, SMARTDRV and the rest of its own.
;=====================================================================
vl_winres:
        push    eax
        push    ebx
        push    esi
        push    edi
        mov     esi,dword ptr ds:[vdm_pathp]
        or      esi,esi
        jz      vwr_out
        mov     ebx,esi                         ; the last component
vwr_scan:
        mov     al,[esi]
        or      al,al
        jz      vwr_name
        inc     esi
        cmp     al,'\'
        je      vwr_sep
        cmp     al,'/'
        je      vwr_sep
        cmp     al,':'
        jne     vwr_scan
vwr_sep:
        mov     ebx,esi
        jmp     short vwr_scan
vwr_name:
        mov     esi,OFFSET vwr_win386
vwr_cmp:
        mov     al,[ebx]
        cmp     al,'a'
        jb      vwr_up
        cmp     al,'z'
        ja      vwr_up
        sub     al,20h
vwr_up:
        cmp     al,[esi]
        jne     vwr_out
        or      al,al
        jz      vwr_match
        inc     ebx
        inc     esi
        jmp     short vwr_cmp
vwr_match:
        ; RMDOS's 64K, and RMCD.SYS's room after it when there is a
        ; CD-ROM for it to serve (PARK.INC, park_loadcd) - in the same
        ; block, so that it too is below WIN386 and global to every
        ; virtual machine, as MSCDEX loaded before Windows was.  Without
        ; the room for both, RMDOS's alone.
        mov     ebx,PARK_RMPARAS
        call    park_cdfind
        jc      vwr_alloc
        add     ebx,PARK_CDPARAS
        call    lm_alloc
        jnc     vwr_have
        mov     ebx,PARK_RMPARAS
vwr_alloc:
        call    lm_alloc                        ; -> AX = the segment
        jc      vwr_out                         ; no room: the park will say
vwr_have:
        mov     word ptr ds:[park_resseg],ax
        movzx   edi,ax
        dec     edi
        shl     edi,4                           ; its header
        push    ax
        mov     ax,word ptr ds:[vdm_curpsp]     ; the parent's, so that the
        mov     word ptr [edi+RMCB_OWN],ax      ; parent's exit frees it
        pop     ax
        mov     esi,OFFSET vwr_rmname
        call    lm_setname                      ; and MEM says whose it is
vwr_out:
        pop     edi
        pop     esi
        pop     ebx
        pop     eax
        ret

vwr_win386  db  'WIN386.EXE',0
vwr_rmname  db  'RMDOS.SYS',0

;=====================================================================
; The .EXE path: a header, a load module, and a table of fixups.
;
; The whole job is arithmetic and one loop.  Work out how much of the
; file is program, find somewhere to put it, read it in, then walk the
; fixup table adding the load segment to every segment word the linker
; could not know the value of.  CS:IP and SS:SP in the header are also
; relative and get the same treatment.
;
; The load module starts one paragraph past the PSP - PSP + 10h - and
; that is the value every fixup is relative to.  Get that wrong by a
; paragraph and the program runs for a while and then dies somewhere
; unrelated, which is the worst kind of wrong.
;=====================================================================
vl_exe:
;--- how big is the program? -----------------------------------------
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_CP]
        shl     eax,9                           ; pages of 512
        movzx   edx,word ptr ds:[vdm_hdrbuf+EXE_CBLP]
        or      edx,edx
        jz      vx_whole                        ; 0 = the last page is full
        sub     eax,512
        add     eax,edx
vx_whole:
        movzx   edx,word ptr ds:[vdm_hdrbuf+EXE_CPARHDR]
        shl     edx,4
        mov     dword ptr ds:[vdm_hdrsz],edx
        sub     eax,edx                         ; the load module proper
        jbe     vx_badexe                       ; header claims to be all of it
        mov     dword ptr ds:[vdm_imgsz],eax
        add     eax,15
        shr     eax,4
        mov     dword ptr ds:[vdm_imgpar],eax

;--- and where does it go? -------------------------------------------
        mov     ebx,eax
        add     ebx,10h                         ; the PSP in front of it
        movzx   ecx,word ptr ds:[vdm_hdrbuf+EXE_MINALLOC]
        add     ebx,ecx
        mov     dword ptr ds:[vdm_minpar],ebx

        movzx   ecx,word ptr ds:[vdm_hdrbuf+EXE_MAXALLOC]
        add     ecx,eax
        add     ecx,10h
        cmp     ecx,0FFFFh                      ; a real-mode block cannot
        jbe     vx_ask                          ; be bigger than 1MB anyway
        mov     ecx,0FFFFh
vx_ask:
        call    vl_winres                       ; WIN386.EXE: RMDOS's segment
        mov     ebx,ecx
        call    lm_alloc
        jnc     vx_got
        ; MAXALLOC is 0FFFFh in nearly every .EXE ever linked, so that
        ; first ask nearly always fails and BX comes back holding the
        ; largest run there is.  Take it, if the program can live in it.
        ; ...and the largest run IN THE REGION THE STRATEGY NAMES, which
        ; stopped being one number the moment there were two regions.
        ; See lm_allocbig: asking globally is what would have made every
        ; DEVICEHIGH and every LOADHIGH load low without a word.
        mov     ebx,dword ptr ds:[vdm_minpar]
        call    lm_allocbig
        jc      vx_nomem
vx_got:
        mov     word ptr ds:[vdm_pspseg],ax
        add     ax,10h
        mov     word ptr ds:[vdm_loadseg],ax

        movzx   edx,word ptr ds:[vdm_pspseg]    ; how big did we get?
        dec     edx
        shl     edx,4
        movzx   edx,word ptr [edx+RMCB_LEN]
        mov     word ptr ds:[vdm_paras],dx
        movzx   eax,word ptr ds:[vdm_pspseg]
        call    vdm_psp                         ; AX = seg, DX = paragraphs

        movzx   eax,word ptr ds:[vdm_pspseg]    ; and the block wears the
        mov     esi,dword ptr ds:[vdm_pathp]    ; program.s name, as DOS 4
        call    lm_setname                      ; onwards has done

;--- read the load module in -----------------------------------------
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,dword ptr ds:[vdm_hdrsz]
        mov     eax,4200h                       ; past the header
        call    k21
        jc      vl_close

        movzx   edx,word ptr ds:[vdm_loadseg]
        shl     edx,4
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,dword ptr ds:[vdm_imgsz]    ; may be well over 64K, and
        mov     eax,3F00h                       ; the kernel's read is 32-bit
        call    k21
        jc      vl_close

;--- and relocate it -------------------------------------------------
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_CRLC]
        mov     dword ptr ds:[vdm_relcnt],eax
        or      eax,eax
        jz      vx_ready                        ; a .EXE with no fixups is
                                                ; unusual but perfectly legal
        movzx   ecx,word ptr ds:[vdm_hdrbuf+EXE_LFARLC]
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,4200h
        call    k21
        jc      vl_close

vx_batch:
        mov     ecx,dword ptr ds:[vdm_relcnt]
        or      ecx,ecx
        jz      vx_ready
        cmp     ecx,VDM_RELBAT
        jbe     vx_some
        mov     ecx,VDM_RELBAT
vx_some:
        mov     dword ptr ds:[vdm_relthis],ecx
        shl     ecx,2                           ; four bytes an entry
        mov     edx,OFFSET vdm_relbuf
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3F00h
        call    k21
        jc      vl_close

        mov     esi,OFFSET vdm_relbuf
        mov     ecx,dword ptr ds:[vdm_relthis]
vx_one:
        movzx   eax,word ptr [esi+2]            ; the fixup's own segment,
        movzx   edx,word ptr ds:[vdm_loadseg]   ; itself relative to the load
        add     eax,edx
        shl     eax,4
        movzx   edx,word ptr [esi]              ; and its offset within that
        add     eax,edx
        mov     dx,word ptr ds:[vdm_loadseg]
        add     word ptr [eax],dx               ; the fixup itself
        add     esi,4
        loop    vx_one

        mov     eax,dword ptr ds:[vdm_relthis]
        sub     dword ptr ds:[vdm_relcnt],eax
        jmp     vx_batch

;--- the context a .EXE expects --------------------------------------
vx_ready:
        mov     ebx,dword ptr ds:[vdm_hdl]      ; done with the file
        mov     eax,3E00h
        call    k21

        mov     edi,OFFSET vdm_ctx
        xor     eax,eax
        mov     ecx,V86C_SIZE / 4
        rep     stosd

        movzx   eax,word ptr ds:[vdm_pspseg]    ; DS and ES address the PSP,
        mov     dword ptr [vdm_ctx+V86C_DS],eax ; which is where the command
        mov     dword ptr [vdm_ctx+V86C_ES],eax ; tail and the FCBs are

        movzx   edx,word ptr ds:[vdm_loadseg]
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_CS]
        add     eax,edx
        and     eax,0FFFFh
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_IP]
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_SS]
        add     eax,edx
        and     eax,0FFFFh
        mov     dword ptr [vdm_ctx+V86C_SS],eax
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_SP]
        mov     dword ptr [vdm_ctx+V86C_ESP],eax

        mov     dword ptr [vdm_ctx+V86C_FLAGS],EFL_IF
        movzx   eax,word ptr ds:[vdm_fcbax]     ; the two FCB parse codes
        mov     dword ptr [vdm_ctx+V86C_EAX],eax
        call    vdm_traps
        popad
        clc
        ret

vx_badexe:
        mov     word ptr ds:[vdm_err],ERR_EXEFMT
        jmp     vl_close

vx_nomem:
        mov     word ptr ds:[vdm_pspseg],0      ; the program never got a block
        mov     word ptr ds:[vdm_err],ERR_NOMEM
        jmp     vl_close

;=====================================================================
; vdm_traps - which interrupt vectors the monitor is to hand back to
;             us rather than reflect into the guest's own IVT.
;
; NOT 20h-2Fh, WHICH ARE DOS'S AND GO THROUGH THE TABLE LIKE DOS'S.
; They used to be trapped here, all sixteen, and served before the
; guest's table was ever looked at - so a program that hooked INT 21h
; or INT 2Fh, which is what a TSR is, was never called.  The table
; points at the DOS vector stubs instead (vdm_stubs), and the monitor
; hands the kernel a HLT at the end of the chain.  15h is claimed for a handful of
; subfunctions only - AH=88h and AH=87h, the pre-XMS way of getting at
; extended memory, which the real BIOS would answer honestly and
; disastrously, since the memory it would offer is the memory this
; kernel is running in.  Everything else on both vectors is reflected
; straight back; see vdm_reflect.
;
; 13h is claimed OUTRIGHT, for the drives this kernel manages.  The
; disks belong to the kernel - it drives IDE controllers itself and it
; caches what it reads off them - so a guest reaching past it to the
; BIOS was reading and writing sectors behind the filesystem's back.
; The drives it does NOT manage are handed back one call at a time,
; inside the handler, where the drive number is known.
;=====================================================================
vdm_traps:
        mov     byte ptr [vdm_ctx+V86C_TRAP+2],28h      ; 13h and 15h: bits
                                                        ; 3 and 5 of the byte
                                                        ; covering 10h-17h
IFDEF V86_NOMODE
        or      byte ptr [vdm_ctx+V86C_TRAP+2],1        ; ...and 10h, to hold
                                                ; the screen in text mode
ENDIF
        mov     byte ptr [vdm_ctx+V86C_TRAP+4],0        ; 20h-27h: the table's
        mov     byte ptr [vdm_ctx+V86C_TRAP+5],0        ; 28h-2Fh: the same

        ; 67h, and ONLY WHEN THERE IS A MANAGER TO ANSWER IT.  Claiming
        ; the vector on a machine with no expanded memory would mean
        ; answering as though there were one, and the honest reply to
        ; "is EMS here" is the IRET vdm_stubs plants: a program reads
        ; the vector's segment, fails to find EMMXXXX0 in it, and
        ; concludes correctly.  Bit 7 of byte 12: eight vectors to a
        ; byte, and 67h is 103.
        mov     byte ptr [vdm_ctx+V86C_TRAP+12],0
        cmp     dword ptr [ems_have],0
        je      vt_noems
        mov     byte ptr [vdm_ctx+V86C_TRAP+12],80h
vt_noems:
        ret

;=====================================================================
; vdm_stubs - the real-mode IVT's DOS vectors, and where they point.
;
; The monitor reflects an untrapped interrupt through the guest's own
; interrupt vector table at 0:0, and that table is the BIOS's - which
; has never heard of DOS.  Vectors 20h-2Fh belong to DOS, and on a
; machine that has not booted one they hold whatever the BIOS felt
; like leaving: an IRET on the careful BIOSes, nothing at all on the
; rest.
;
; So they point at the DOS vector stubs (DVS_ in PMDOS.INC): a HLT for
; each vector the kernel serves - 20h, 21h, 23h, 25h, 26h, 27h, 29h
; and 2Fh - and an IRET for the rest.  A program that hooks one and
; chains to what it found reaches the kernel through the HLT, exactly
; as it reached DOS; one that hooks nothing reaches it the same way,
; one step sooner.  vdm_dvs is what the HLT arrives at.
;
; ONCE, at boot, and NOT per DOS box.  These vectors are the machine's
; and not the box's - which is exactly what makes a TSR possible - so
; re-planting them when a box starts would quietly unhook every
; resident program the moment the next one ran.  park_restvecs puts
; them back after Windows, and plants only the STUBS again (dvs_plant),
; because RMDOS writes jumps into those.
;
; 67h IS NOT A DOS VECTOR AND IS POINTED AT AN IRET ANYWAY.  It belongs
; to expanded memory, and a machine with none leaves it exactly as the
; BIOS did - which on most BIOSes is nothing at all, so a program that
; probes for EMS by CALLING the vector rather than by reading it jumps
; to 0000:0000 and starts executing the interrupt vector table.
; Programs that do this are wrong and they exist.  ems_init overwrites
; the entry with a segment that has a name in it when there is a
; manager to name; until then an IRET is the whole of the right
; answer, and AH comes back unchanged rather than saying "no error".
;=====================================================================
vdm_stubs:
        pushad
        movzx   eax,word ptr ds:[xms_cbseg]
        or      eax,eax
        jz      vs_none                         ; no low memory, no stubs
        mov     edi,eax
        shl     edi,4
        mov     byte ptr [edi+VDMCB_IRET],0CFh  ; IRET, for 67h
        shl     eax,16
        mov     ax,VDMCB_IRET
        mov     dword ptr ds:[67h*4],eax

        cmp     word ptr ds:[dvs_seg],0         ; the stubs' own paragraphs,
        jne     vs_have                         ; owner 8 like the callback
        mov     ebx,DVS_PARAS                   ; block, bought once
        call    lm_sysalloc
        jc      vs_none
        mov     word ptr ds:[dvs_seg],ax
vs_have:
        call    dvs_plant
        movzx   eax,word ptr ds:[dvs_seg]
        shl     eax,16                          ; seg:0, the stub for 20h
        mov     edi,20h * 4
        mov     ecx,DVS_COUNT
vs_one:
        mov     dword ptr ds:[edi],eax
        add     eax,DVS_LEN
        add     edi,4
        loop    vs_one
vs_none:
        popad
        ret

;---------------------------------------------------------------------
; mouse_boot - the INT 33h stub's paragraphs, bought once like the DOS
;              vectors' and for the same reason, and handed to PMIO's
;              mouse driver with the three things it cannot find out
;              for itself: where a DPMI client's INT 33h still goes, and
;              which native program is running and whether it is inside
;              a handler of the kernel's.  A failed allocation still
;              gives native programs their mouse; only boxes go without.
;---------------------------------------------------------------------
mouse_boot:
        pushad
        mov     ebx,MS_PARAS
        call    lm_sysalloc
        jnc     mb_have
        xor     eax,eax
mb_have:
        mov     edi,OFFSET dst_33
        mov     esi,OFFSET r3_depth
        mov     edx,OFFSET r3_calldep
        call    dword ptr ds:[IOS_MSINIT]
        popad
        ret

;---------------------------------------------------------------------
; bus_boot - the same again for INT 4Bh, where a driver asks PMIO what
;            a buffer's physical address is (IO\VDS.INC), and the two
;            things PMIO has to be told about the DPMI host before it
;            can hand a client its card's interrupt (IO\VPIC.INC).
;---------------------------------------------------------------------
bus_boot:
        pushad
        mov     ebx,BUS_PARAS
        call    lm_sysalloc
        jnc     bb_have
        xor     eax,eax
bb_have:
        mov     ebx,OFFSET dpmi_pmvec
        mov     ecx,OFFSET dpmi_active
        mov     edx,OFFSET dpmi_is32
        call    dword ptr ds:[IOS_BUSINIT]
        mov     dword ptr ds:[dpmi_pvif],eax    ; and what PMIO has to tell us
        popad
        ret

;---------------------------------------------------------------------
; dvs_plant - write the sixteen stubs and the ^C pad.  Everything
;             preserved.
;---------------------------------------------------------------------
dvs_plant:
        pushad
        movzx   edi,word ptr ds:[dvs_seg]
        or      edi,edi
        jz      dvp_none
        shl     edi,4
        mov     esi,OFFSET dvs_served
        mov     ecx,DVS_COUNT
dvp_one:
        mov     dword ptr [edi],909090CFh       ; IRET, and NOPs to the end
        mov     byte ptr [edi+4],90h
        cmp     byte ptr [esi],0
        je      dvp_next
        mov     word ptr [edi],0CFF4h           ; HLT, then an IRET no one reaches
dvp_next:
        inc     esi
        add     edi,DVS_LEN
        loop    dvp_one
        mov     byte ptr [edi],0F4h             ; DVS_CCPAD
dvp_none:
        popad
        ret

; Which of 20h-2Fh the kernel answers: a HLT.  The rest are an IRET.
; 22h is an address and never called; 24h unhooked is the kernel's own
; question (crit_hooked); 28h and 2Ah-2Eh have nothing behind them on
; DOS either unless a program hooks them.
dvs_served  db  1, 1, 0, 1, 0, 1, 1, 1          ; 20h - 27h
            db  0, 1, 0, 0, 0, 0, 0, 1          ; 28h - 2Fh

;---------------------------------------------------------------------
; dvs_which - is the context standing on one of the stubs' HLTs?  CF
;             clear if so, with EAX = the vector, or 0 for the ^C pad.
;---------------------------------------------------------------------
dvs_which:
        push    ecx
        push    edx
        movzx   eax,word ptr ds:[dvs_seg]
        or      eax,eax
        jz      dvw_no
        cmp     dword ptr [vdm_ctx+V86C_CS],eax
        jne     dvw_no
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        cmp     eax,DVS_CCPAD
        je      dvw_pad
        ja      dvw_no
        xor     edx,edx
        mov     ecx,DVS_LEN
        div     ecx
        or      edx,edx
        jnz     dvw_no                           ; not the first byte of one
        cmp     byte ptr [dvs_served+eax],0
        je      dvw_no                           ; an IRET, which cannot halt
        add     eax,20h
        pop     edx
        pop     ecx
        clc
        ret
dvw_pad:
        xor     eax,eax
        pop     edx
        pop     ecx
        clc
        ret
dvw_no:
        pop     edx
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; vdm_cpush - AX onto the guest's stack, through vdm_ctx.
; vdm_cpop  - EAX = a word off it.
; vdm_ciret - an IRET done to vdm_ctx: IP, CS and FLAGS off the stack.
;
; A stub is reached by an INT, or by a hook's PUSHF / CALL FAR, and
; either way the caller's return frame is on top of the guest's stack.
; Taking it off FIRST leaves the context exactly as the monitor used to
; hand it over when it trapped the INT - IP past the instruction, the
; caller's own flags - so everything that served a trapped INT 21h
; serves one that came through the table, unchanged.
;---------------------------------------------------------------------
vdm_cpush:
        push    ebx
        push    edx
        mov     edx,dword ptr [vdm_ctx+V86C_ESP]
        sub     dx,2
        mov     dword ptr [vdm_ctx+V86C_ESP],edx
        movzx   ebx,dx
        mov     edx,dword ptr [vdm_ctx+V86C_SS]
        shl     edx,4
        add     ebx,edx
        mov     [ebx],ax
        pop     edx
        pop     ebx
        ret

vdm_cpop:
        push    ebx
        push    edx
        mov     edx,dword ptr [vdm_ctx+V86C_ESP]
        movzx   ebx,dx
        add     dx,2
        mov     dword ptr [vdm_ctx+V86C_ESP],edx
        mov     edx,dword ptr [vdm_ctx+V86C_SS]
        shl     edx,4
        add     ebx,edx
        movzx   eax,word ptr [ebx]
        pop     edx
        pop     ebx
        ret

vdm_ciret:
        push    eax
        call    vdm_cpop
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        call    vdm_cpop
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        call    vdm_cpop
        mov     dword ptr [vdm_ctx+V86C_FLAGS],eax
        pop     eax
        ret

;=====================================================================
; vdm_lolinit - build the list of lists a 16-bit guest can follow.
;
; Below 1MB because a real-mode program cannot hold a pointer to
; anywhere else, and out of the DOS arena marked owner 8 - the same
; way xms_init pays for its callback paragraph.  No PSP is ever 8, so
; the exit sweeps walk past it and it lives as long as the machine.
; Four paragraphs is the whole cost, and MEM reporting them gone is
; the truth: real DOS spends conventional memory on this structure too.
;
; ONCE, at boot.  Nothing in it changes per box, and the two drive
; counts that CAN change are written by vdm_lol when it is asked.
;
; See the GLOL_ block in PMDOS.INC for what each field says and why
; four of the pointers deliberately say nothing at all.
;=====================================================================
vdm_lolinit:
        pushad
        ; IT IS NOT TAKEN OUT OF THE ARENA ANY MORE.  This used to call
        ; lm_sysalloc, which put the list of lists five paragraphs
        ; ABOVE the first MCB - inside the memory it describes, which
        ; no real DOS does and which MEM sizes by subtracting.  See
        ; DOSDATA in PMDOS.INC.  A fixed address below the arena is
        ; also one less thing that can fail at boot.
        mov     ax,DOSDATA_SEG
        mov     word ptr ds:[vdm_lolseg],ax
        mov     edi,DOSDATA

        ; ZERO IT FIRST.  Everything below writes the fields PM-DOS
        ; means to publish, and what is left over used to be whatever
        ; the paragraph held at boot - which a program reading a fixed
        ; offset in the DOS data segment reads as an answer.  That is
        ; not a hypothetical: UMB_HEAD at 8Ch is exactly such a field,
        ; and MEM believed it.
        push    edi
        push    ecx
        xor     eax,eax
        mov     ecx,DOSDATA_SIZE / 4
        rep     stosd
        pop     ecx
        pop     edi

        ; DOS 5's version word at offset 4, which SHARE and VSHARE read
        ; before they believe anything else here - see DOSDATA.
        mov     word ptr [edi+GLOL_DOSVER],1

        ; THE ONE FIELD THAT IS A CONTRACT.  The guest's arena starts
        ; at V86_TPA, which is where lm_init put the first header - and
        ; NOT at ARENA_BASE, which is what fn52 publishes to a 32-bit
        ; caller.  Those are two different arenas and this is the whole
        ; reason a guest could not be given the kernel's answer.
        mov     word ptr [edi+GLOL_MCBSEG],V86_TPA_SEG

        ; The structures PM-DOS does not have, said plainly - and the
        ; drive parameter blocks, which it has once vdm_dpbfresh has
        ; looked at the volumes, on the first 52h.
        mov     eax,0FFFFFFFFh
        mov     [edi+GLOL_DPB],eax
        mov     [edi+GLOL_BUFFERS],eax
        mov     [edi+GLOL_FCB],eax

        ; ...AND ONE IT HAS NOW: the current directory array, whose
        ; entries vdm_cdsfresh writes whenever 52h is asked.  It said
        ; FFFF:FFFF until 2026-10-06, and Windows for Workgroups' Setup
        ; read its current directories out of whatever was there and
        ; found SUBST and JOIN in force - and stopped to ask about them.
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GCDS_OFF
        mov     [edi+GLOL_CDS],eax

        ; ...AND ONE THAT IT DOES, because FFFF:FFFF was not an
        ; honest answer for this one - it is a CHAIN HEAD, and a
        ; walker does not read it as "nothing here", it follows it.
        ; Two blocks, the second terminated the way DOS terminates one,
        ; with MAX_HANDLES entries between them that the zero-fill above
        ; has already made "not open".  See GSFT_ in PMDOS.INC for what
        ; this cost and what it bought; vdm_lolfresh shares FILES= out
        ; between the two.
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GSFT2_OFF
        mov     dword ptr ds:[GSFT_BASE + GSFT_NEXT],eax
        mov     word ptr ds:[GSFT_BASE + GSFT_COUNT],GSFT_N1
        mov     dword ptr ds:[GSFT2_BASE + GSFT_NEXT],0FFFFFFFFh
        mov     word ptr ds:[GSFT2_BASE + GSFT_COUNT],GSFT_N2
        mov     ax,GSFT_OFF
        mov     [edi+GLOL_SFT],eax

        ; CON points at the real CON header now.  CLOCK$ says FFFF:FFFF
        ; with the rest of what is not here: PM-DOS deliberately has no
        ; clock driver - see dev_tab - and pointing this at some other
        ; device's header would be the exact half-truth that comment
        ; refuses.  Nothing walks the chain from here anyway; walkers
        ; start at NUL, which is what the +22h header is for.
        ;
        ; ...AND IT DOES NOW.  It said GLOL_CON, which is this field's
        ; own offset, so the console device was a far pointer to
        ; itself until the layout moved under it (2026-10-06).
        mov     dword ptr [edi+GLOL_CLOCK],0FFFFFFFFh
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GLOL_HCON
        mov     [edi+GLOL_CON],eax

        mov     word ptr [edi+GLOL_MAXSEC],512
        mov     word ptr [edi+GLOL_PROTFCB],0
        mov     byte ptr [edi+GLOL_NBLOCK],0    ; vdm_lol fills all three of
        mov     byte ptr [edi+GLOL_LASTDRV],MAX_DRIVES
        mov     byte ptr [edi+GLOL_NJOIN],0

        ; THE DOS 5 TAIL.  Four of these are "nothing here", said at
        ; the offset a reader will look at rather than left as whatever
        ; the device headers used to spell there; three are answers
        ; PM-DOS has always known and had nowhere to put.
        mov     word ptr [edi+GLOL_SPECIAL],0
        mov     dword ptr [edi+GLOL_SETVER],0FFFFFFFFh
        mov     word ptr [edi+GLOL_CPFIX],0
        mov     word ptr [edi+GLOL_A20PSP],0

        ; NO DISK BUFFERS, AND THAT IS THE TRUE ANSWER RATHER THAN A
        ; MODEST ONE.  A DOS buffer is a 528-byte sector cache in
        ; conventional memory that BUFFERS= sizes; PM-DOS caches whole
        ; clusters and FAT windows above 1MB instead, so a program
        ; costing the arena out finds none here because none of it is
        ; down there to find.  Saying "20" would spend 10K of somebody
        ; else's arithmetic on memory that is not missing.
        mov     word ptr [edi+GLOL_NBUF],0
        mov     word ptr [edi+GLOL_NLOOK],0

        ; The boot drive as DOS numbers it - 1 = A - from the BIOS
        ; number PMIO booted with.  A hard disk is C: because that is
        ; where its first partition mounts.
        movzx   eax,byte ptr ds:[GV_BOOTDRV]
        cmp     al,80h
        jb      vli_bootfd
        sub     al,80h - 2
vli_bootfd:
        inc     al
        mov     [edi+GLOL_BOOTDRV],al

        mov     byte ptr [edi+GLOL_DWMOVE],1    ; a 386 is the entry price

        ; Extended memory in KB, saturated, because the field is a
        ; word and the machines this runs on are bigger than one - the
        ; same clamp xms_cap16 makes for the same reason.
        mov     eax,dword ptr ds:[GV_MEMKB]
        cmp     eax,0FFFFh
        jbe     vli_extok
        mov     eax,0FFFFh
vli_extok:
        mov     [edi+GLOL_EXTMEM],ax

        ; WHERE UPPER MEMORY BEGINS, WHICH IS NOWHERE.  FFFFh is DOS's
        ; own "not initialised", and no segment can be at or above it,
        ; so every block in the chain is conventional memory.  See the
        ; comment on GLOL_UMBHEAD: this single word is what MS-DOS
        ; 6.22's MEM.EXE classifies the entire arena by.
        mov     word ptr [edi+GLOL_UMBHEAD],0FFFFh

        mov     byte ptr [edi+GLOL_RETF],0CBh   ; RETF

        ; The odds and ends the other pointer-answering calls hand
        ; back.  The media byte is written per call by fn1B; the DBCS
        ; table is a zero word and never changes, because "no lead
        ; bytes" is the whole of what it has to say.
        mov     byte ptr [edi+GLOL_MEDIA],0
        mov     word ptr [edi+GLOL_DBCS],0

        ; And the device chain, built from a table so that four headers
        ; cannot drift apart.  NUL is first because the list of lists
        ; anchors there and because DEVICE= will insert behind it.
        mov     esi,OFFSET vdm_devlist
vli_dev:
        movzx   edx,word ptr [esi]              ; where this header goes
        cmp     dx,0FFFFh
        je      vli_devdone
        add     edx,edi

        movzx   eax,word ptr [esi+2]            ; and where the next one is
        cmp     ax,0FFFFh
        je      vli_devlast
        movzx   ecx,word ptr ds:[vdm_lolseg]
        shl     ecx,16
        or      eax,ecx                         ; a far pointer to it
        jmp     short vli_devnext
vli_devlast:
        mov     eax,0FFFFFFFFh                  ; the chain ends here
vli_devnext:
        mov     [edx+DEVH_NEXT],eax

        mov     ax,[esi+4]
        mov     [edx+DEVH_ATTR],ax

        ; Both driver entries are FAR CALL targets and both point at
        ; the same RETF.  PM-DOS's devices are 32-bit code inside the
        ; kernel, not real-mode drivers taking request packets, so
        ; there is nothing here to call - and a return that does
        ; nothing is the only honest thing to put at an address that
        ; has to exist.  Walkers READ these; nothing calls them.
        mov     word ptr [edx+DEVH_STRAT],GLOL_RETF
        mov     word ptr [edx+DEVH_INTR],GLOL_RETF

        push    esi
        add     esi,6                           ; the eight-byte name
        lea     ebx,[edx+DEVH_NAME]
        mov     ecx,8
vli_name:
        mov     al,[esi]
        mov     [ebx],al
        inc     esi
        inc     ebx
        dec     ecx
        jnz     vli_name
        pop     esi

        add     esi,VDMDEV_LEN
        jmp     short vli_dev
vli_devdone:

        ; THE COUNTRY TABLES - see GNLS_ in PMDOS.INC for why they are
        ; here at all.  Uppercase first, and it is the identity: the
        ; kernel's own 20h-22h upper-case ASCII and leave 80h-FFh as they
        ; are, and a table that disagreed with them would make FIND /I
        ; and 6520h two different answers to one question.
        mov     edi,DOSDATA + GNLS_UPCASE
        mov     word ptr [edi],128
        add     edi,2
        mov     al,80h
vli_up:
        mov     [edi],al
        inc     edi
        inc     al
        jnz     vli_up

        ; The filename terminators, exactly as MS-DOS 6 lays them out.
        mov     esi,OFFSET vdm_nlsterm
        mov     edi,DOSDATA + GNLS_TERM
        mov     ecx,GNLS_TERMLEN
        rep     movsb

        ; The collating sequence: a character's weight is itself, except
        ; that a small letter weighs what its capital does and CP437's
        ; accented letters weigh what their base letters do.  That is why
        ; DOS's SORT has always been case-blind, and SORT is what reads
        ; this.
        mov     edi,DOSDATA + GNLS_COLLATE
        mov     word ptr [edi],256
        add     edi,2
        xor     eax,eax
vli_col:
        mov     dl,al
        cmp     al,'a'
        jb      vli_colacc
        cmp     al,'z'
        ja      vli_colacc
        sub     dl,20h
        jmp     short vli_colput
vli_colacc:
        cmp     al,80h
        jb      vli_colput
        cmp     al,80h + GNLS_ACCENTS - 1
        ja      vli_colput
        mov     dl,byte ptr [vdm_nlsacc + eax - 80h]
vli_colput:
        mov     [edi],dl
        inc     edi
        inc     al
        jnz     vli_col

        ; THE CASE MAP: what the FAR CALL in the country block reaches.
        ; A program hands it a character in AL from 80h up and gets the
        ; capital back, out of the table above - which is the United
        ; States' until COUNTRY= puts another country's there, and so
        ; this needs writing only once.
        ;
        ;       cmp al,80h / jb done / push bx
        ;       mov bx,GNLS_UPCASE + 2 - 80h / xlat cs:[bx]
        ;       pop bx / done: retf
        mov     edi,DOSDATA + GNLS_CASEMAP
        mov     dword ptr [edi],0772803Ch
        mov     byte ptr [edi+4],53h
        mov     byte ptr [edi+5],0BBh
        mov     word ptr [edi+6],GNLS_UPCASE + 2 - 80h
        mov     dword ptr [edi+8],0CB5BD72Eh

        ; AND WHERE INT 09h ENDS IN A DOS BOX UNDER KEYB: a HLT the
        ; keyboard driver answers (kb_hlt, IO\KEYBD.INC).  Nothing
        ; points at it until KEYB loads a layout.
        mov     dword ptr ds:[DOSDATA + GNLS_KBSTUB],9090CFF4h
        mov     byte ptr ds:[DOSDATA + GNLS_KBSTUB + 4],90h

        popad
        ret
IF GNLS_MAPLEN NE 12
        .ERR    <vdm_lolinit writes a twelve-byte case map>
ENDIF
IF GNLS_KBSTUB LT GNLS_CASEMAP + GNLS_MAPLEN
        .ERR    <GNLS: the INT 09h stub is on top of the case map>
ENDIF
IF GNLS_END LT GNLS_KBSTUB + GNLS_STUBLEN
        .ERR    <GNLS: the INT 09h stub runs past the end of the tables>
ENDIF

; What 6505h hands back, byte for byte MS-DOS 6's: the table's length,
; three bytes of permissible range (01h, lowest, highest), an excluded
; range (00h, 00h-20h), and the characters that end a file name.
vdm_nlsterm:
        dw      22
        db      01h, 00h, 0FFh
        db      00h, 00h, 20h
        db      02h, 14
        db      '."/\[]:|<>+=;,'

; The collating weight of 80h-A5h, CP437's accented letters and its
; currency signs: each letter sorts with its base letter, each currency
; sign with '$'.
vdm_nlsacc:
        db      'CUEAAAACEEEIIIAAEAAOOOUUYOU$$$$$AIOUNN'

; The chain, in DOS's own order: NUL, CON, AUX, PRN.  Each row is
; where the header goes, where the NEXT one is, the attribute word,
; and the eight-byte name.  FFFFh in the second column means the chain
; ends; FFFFh in the first ends the table.
;
; This is the order MS-DOS used and it is worth keeping even though
; nothing depends on it yet: software that expects NUL first and CON
; second is software that was written against DOS, and matching it
; costs nothing.
VDMDEV_LEN      EQU     14              ; two words, attributes, eight bytes

vdm_devlist:
        dw  GLOL_HNUL, GLOL_HCON
        dw  DEV_ATTR_NUL
        db  'NUL     '
        dw  GLOL_HCON, GLOL_HAUX
        dw  DEV_ATTR_CON
        db  'CON     '
        dw  GLOL_HAUX, GLOL_HPRN
        dw  DEV_ATTR_AUX
        db  'AUX     '
        dw  GLOL_HPRN, 0FFFFh
        dw  DEV_ATTR_PRN
        db  'PRN     '
        dw  0FFFFh

;=====================================================================
; vdm_push - put the running program's identity on the nesting stack.
; vdm_pop  - take it off again.
;
; What a "program" is here is five things: the V86 context, the PSP and
; environment segments, the block size, and the owner id the kernel
; charges its files and memory to.  Between a push and its pop the
; singletons belong to somebody else entirely.
;
; The push at depth 0 saves an empty context and a PSP that has already
; been replaced by the load - neither is ever looked at.  What it saves
; that MATTERS is cur_own and dta_ptr, which belong to the 32-bit shell
; and have to come back exactly.
;=====================================================================
vdm_push:
        pushad
        mov     eax,dword ptr ds:[vdm_depth]
        cmp     eax,VDM_MAXNEST
        jae     vpu_full
        imul    eax,VF_LEN
        add     eax,OFFSET vdm_stack
        mov     edi,eax

        push    edi                             ; the context, whole
        mov     esi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd
        pop     edi

        mov     eax,dword ptr [cur_own]
        mov     [edi+VF_OWN],eax
        mov     eax,dword ptr [dta_ptr]
        mov     [edi+VF_DTA],eax
        mov     ax,word ptr ds:[vdm_pspseg]
        mov     [edi+VF_PSPSEG],ax
        mov     ax,word ptr ds:[vdm_envseg]
        mov     [edi+VF_ENVSEG],ax
        mov     ax,word ptr ds:[vdm_paras]
        mov     [edi+VF_PARAS],ax
        mov     ax,word ptr ds:[vdm_curpsp]
        mov     [edi+VF_CURPSP],ax
        mov     ax,word ptr ds:[vdm_parpsp]
        mov     [edi+VF_PARPSP],ax

        ; And its floating-point state, which is in the registers the
        ; next program is about to have (FPSTATE.INC).
        mov     eax,dword ptr ds:[vdm_depth]
        add     eax,FPS_BOX
        call    fps_keep

        inc     dword ptr ds:[vdm_depth]
        popad
        clc
        ret
vpu_full:
        popad
        stc
        ret

vdm_pop:
        pushad
        ; One level is leaving, so whatever it decided about its own
        ; memory is settled.  Cleared here rather than where it is set
        ; because a resident program takes the exit that SKIPS the
        ; sweep, and this is the one instruction both exits pass.
        mov     dword ptr ds:[vdm_resident],0
        dec     dword ptr ds:[vdm_depth]
        mov     eax,dword ptr ds:[vdm_depth]
        imul    eax,VF_LEN
        add     eax,OFFSET vdm_stack
        mov     esi,eax

        push    esi
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd
        pop     esi

        mov     eax,[esi+VF_OWN]
        mov     dword ptr [cur_own],eax
        mov     eax,[esi+VF_DTA]
        mov     dword ptr [dta_ptr],eax
        mov     ax,[esi+VF_PSPSEG]
        mov     word ptr ds:[vdm_pspseg],ax
        mov     ax,[esi+VF_ENVSEG]
        mov     word ptr ds:[vdm_envseg],ax
        mov     ax,[esi+VF_PARAS]
        mov     word ptr ds:[vdm_paras],ax
        mov     ax,[esi+VF_CURPSP]
        mov     word ptr ds:[vdm_curpsp],ax
        mov     ax,[esi+VF_PARPSP]
        mov     word ptr ds:[vdm_parpsp],ax

        mov     eax,dword ptr ds:[vdm_depth]    ; and its floating-point
        add     eax,FPS_BOX                     ; state, whatever the one
        call    fps_back                        ; that is leaving left
        popad
        ret

;=====================================================================
; vdm_newid - give the just-loaded program an identity of its own.
;
; A fresh owner id, so that the files it opens and the memory it takes
; are its own to lose - without this they would belong to whoever
; started it and outlive it.  And a DTA at its own PSP+80h, the DOS
; default: otherwise the guest's directory searches would fill in the
; DTA of its caller, which for the usual one is COMMAND.COM in the
; middle of a FOR loop.
;=====================================================================
vdm_newid:
        pushad
        mov     dword ptr ds:[vdm_resident],0   ; a program that has just
                                                ; started has not kept
                                                ; anything yet
        mov     eax,dword ptr [next_own]
        mov     dword ptr [cur_own],eax
        inc     eax
        mov     dword ptr [next_own],eax

        movzx   eax,word ptr ds:[vdm_pspseg]
        mov     word ptr ds:[vdm_curpsp],ax     ; what 51h/62h answer, until
                                                ; a program moves it with 50h
        shl     eax,4
        add     eax,RSP_TAIL
        mov     dword ptr [dta_ptr],eax
        popad
        ret

;=====================================================================
; vdm_run - run the loaded guest until it terminates.
;=====================================================================
vdm_run:
        pushad
        call    vdm_newid                       ; (vdm_exec pushed the frame
                                                ; this run goes back to)

        mov     dword ptr ds:[vdm_exit],0
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        mov     dword ptr ds:[vdm_busy],1
        call    xms_a20reset                    ; a fresh box, a shut gate
        call    dword ptr ds:[IOS_KBBOX]        ; ...and no scan codes left
                                                ; over from the last one

        ; Where to unwind to if the guest has to be killed from
        ; underneath - see vdm_abort.  The InDOS depth is part of that
        ; picture: an abort can happen with a guest's k21 call open,
        ; and its decrement will never run.
        mov     dword ptr ds:[vdm_esp],esp
        movzx   eax,byte ptr ds:[DOS_INDOS]
        mov     dword ptr ds:[vdm_indos],eax

        ; AND THE BOX RUNS WITH DOS IDLE.  We are here from inside the
        ; shell's own 4Bh, which is still open and will stay open for
        ; as long as the guest lives - so without this every program
        ; in a DOS box would run with InDOS reading 1, and a TSR would
        ; conclude that DOS was permanently busy and never pop up.
        ; The 32-bit EXEC path zeroes it for the same reason; this is
        ; the same rule for the other kind of child.  vdm_done and
        ; vdm_abort put the shell's depth back.
        mov     byte ptr ds:[DOS_INDOS],0

IFDEF VDM_DEBUG
        mov     esi,OFFSET msgVdmEntry
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        call    vdm_hex4
        mov     al,':'
        call    dword ptr ds:[IOS_CONOUT]
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        call    vdm_hex4
        mov     esi,OFFSET msgVdmBytes
        call    dword ptr ds:[IOS_CONSTR]
        mov     edi,dword ptr [vdm_ctx+V86C_CS]
        shl     edi,4
        add     edi,dword ptr [vdm_ctx+V86C_EIP]
        mov     ecx,6
vdd_one:
        push    ecx
        movzx   eax,byte ptr [edi]
        call    vdm_hex8
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        inc     edi
        pop     ecx
        loop    vdd_one
        mov     esi,OFFSET msgVdmCR
        call    dword ptr ds:[IOS_CONSTR]
ENDIF

vdm_loop:
        ; A DRIVER'S CALL, NOT THE BOX'S.  dl_excur hands the DOS calls
        ; that have a V86 answer of their own - a vector, a pointer into
        ; DOS's data - to the handlers below, which all end by coming
        ; back here; this sends them back to the excursion instead.
        cmp     byte ptr ds:[vdm_inexc],0
        jne     dlx_back21
        mov     esi,OFFSET vdm_ctx
        call    dword ptr ds:[IOS_V86RUN]
        cmp     byte ptr [cg_inflight],0        ; a critical error's handler is
        jne     vdm_cgchk                       ; running: has it gone home?
vdm_cgback:
        cmp     eax,V86R_INT
        je      vdm_int
        cmp     eax,V86R_HLT
        je      vdm_halt
        cmp     eax,V86R_FLTLOOP
        je      vdm_fltloop
        jmp     vdm_fault

;---------------------------------------------------------------------
; vdm_cgchk - a box's INT 24h handler is running on the frame crit_guest
;             built, and the guest has just come back to the monitor
;             for something.  If its stack is back at the caller's SP,
;             the handler did not IRET onto the landing pad at all: it
;             dropped the frame and returned INTO THE PROGRAM, with the
;             error in AX, as the C library's _hardretn does and as
;             MS-DOS allows.  The program is where its handler put it,
;             so that is where the box goes on from - and the DOS call
;             the error was raised in is abandoned, as it is on MS-DOS.
;
; Only at the depth the error was raised at: a program the handler
; started would be measured against a stack that is not its own.
;---------------------------------------------------------------------
vdm_cgchk:
        push    eax
        mov     eax,dword ptr ds:[vdm_depth]
        cmp     eax,dword ptr ds:[dpmi_rmdepth]
        jne     vcg_not
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        cmp     eax,[cg_ss]
        jne     vcg_not
        movzx   eax,word ptr [vdm_ctx+V86C_ESP]
        cmp     eax,[cg_sp]
        jb      vcg_not                         ; still below: still inside
        pop     eax
        jmp     crit_direct
vcg_not:
        pop     eax
        jmp     vdm_cgback

;---------------------------------------------------------------------
; crit_direct - the handler returned straight into the program.  The
;               simulated interrupt is over without its HLT, the DOS
;               call that raised the error is over without its answer,
;               and the kernel stack above the box loop's anchor is all
;               of it: unwind there, as vdm_abort would, and go on.
;---------------------------------------------------------------------
crit_direct:
        mov     byte ptr [cg_inflight],0
        mov     dword ptr ds:[dpmi_rmact],0     ; as dpmi_rmdone leaves them
        mov     eax,dword ptr ds:[dpmi_rmesp0]
        mov     dword ptr ds:[TSS_BASE + TSS_ESP0],eax
        mov     byte ptr [crit_busy],0          ; as crit_raise leaves them
        mov     byte ptr ds:[DOS_ERRFLG],0
        mov     byte ptr ds:[DOS_INDOS],0       ; and the call is not in DOS
        mov     esp,dword ptr ds:[vdm_esp]
        jmp     vdm_loop

;---------------------------------------------------------------------
; A HLT, which is either a program that has genuinely stopped or one
; of our own callback bytes being used as a service entry.  The
; address settles it: the callback block is memory we planted and know
; the address of, and no ordinary program executes a HLT at all.
;---------------------------------------------------------------------
; Eight things can be at the far end of a HLT now.  Six of them are
; told apart by ADDRESS - the XMS entry, DPMI's mode switch, the pad a
; simulated real-mode interrupt returns to, one of the real-mode
; callback landing pads, either entry point of the MSCD001 device, and
; the byte an EMS 56h target returns to - and if it is none of those,
; the guest's own interrupt flag settles the last two: on, and it is a
; program waiting for an interrupt; off, and it is one that has
; genuinely stopped.
vdm_halt:
        mov     esi,OFFSET vdm_ctx      ; the mouse's INT 33h stub, which
        movzx   eax,word ptr ds:[vdm_pspseg]    ; PMIO answers itself; the
        or      eax,MSTOK_BOX                   ; program is named so that
        call    dword ptr ds:[IOS_MSHLT]        ; its handler can be put
        jnc     vdm_loop                        ; away when it ends
        call    dvs_which               ; a DOS vector's stub, or the ^C pad:
        jnc     vdm_dvs                 ; EAX says which
        call    xms_iscb
        jnc     vh_xms
        call    dpmi_iscb
        jnc     vh_dpmi
        call    dpmi_isret
        jnc     vh_dpmiret
        call    dpmi_iscall             ; EAX = which callback, if it is one
        jnc     vh_dpmicb
        call    dpmi_israwpm            ; 0306h's way back to protected
        jnc     vh_rawpm                ; mode

        call    cdd_iscb                ; EAX = which entry, if it is one of
        jnc     vh_cdd                  ; the MSCD001 device's two
        call    ems_iscb                ; or the RETF of a routine EMS 56h
        jnc     vh_ems                  ; called

        ; A SIXTH, and it is not a stopped program at all: a HLT WITH
        ; INTERRUPTS ON is a program waiting for one.  That is the
        ; canonical way to wait for a key without spinning, and until
        ; IRQ 1 could be delivered into a box there was nothing to wait
        ; FOR - so ending the box was as good an answer as any.  Now
        ; there is, and killing the guest at the exact moment its
        ; keystroke was about to arrive would be a poor joke.
        ;
        ; The flags are the guest's own virtual ones, so this asks what
        ; the program believes rather than what the CPU is doing; the
        ; guest never really runs with interrupts off.  HLT with them
        ; off is still the end, and it is the honest end: an 8086 that
        ; halts with no way to be interrupted has genuinely stopped.
        ;
        ; What it costs is that a program which halts forever with
        ; interrupts on now hangs its box instead of abending it.  That
        ; is what the hardware does, Ctrl-Alt-Del still works because
        ; the kernel owns IRQ 1, and V86_WATCH is the build that says
        ; where a guest has got stuck.
        test    dword ptr [vdm_ctx+V86C_FLAGS],EFL_IF
        jz      vdm_abend
        sti
        hlt                             ; until the next interrupt - the
                                        ; timer at worst, 18.2 times a second
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        inc     ax                      ; over the HLT, which the monitor
        mov     dword ptr [vdm_ctx+V86C_EIP],eax        ; leaves standing on
        jmp     vdm_loop                                ; it
vh_xms:
        call    xms_call
        jmp     vdm_loop
vh_dpmi:
        jmp     dpmi_switch
vh_dpmiret:
        jmp     dpmi_rmdone
vh_dpmicb:
        jmp     dpmi_docall
vh_rawpm:
        jmp     dpmi_rawpm
vh_cdd:
        call    cdd_call
        jmp     vdm_loop
vh_ems:
        call    ems_callret             ; CF: no 56h frame under it, and
        jc      vdm_abend               ; so no caller to go back to
        jmp     vdm_loop

;---------------------------------------------------------------------
; vdm_abend - the box ends because something went wrong rather than
;             because a program asked to.  The whole box: a fault at
;             any depth has already lost the machine state its parents
;             were relying on.
;---------------------------------------------------------------------
vdm_abend:
        cmp     byte ptr ds:[irx_busy],0        ; a handler an excursion ran:
        jne     irx_lost                        ; the excursion ends, not a box
        mov     dword ptr ds:[vdm_exit],0
        mov     dword ptr ds:[vdm_extype],VDX_CRITERR
va_unwind:
        call    vdm_outer
        jbe     vdm_done
        call    vdm_cleanup
        call    vdm_pop
        jmp     short va_unwind

;---------------------------------------------------------------------
; vdm_dvs - the guest reached the HLT in one of the DOS vector stubs:
;           EAX = the vector, or 0 for the ^C pad.  See vdm_stubs.
;
; The caller's return frame is on the guest's stack.  For all but 25h
; and 26h it comes off first, as the handler's IRET would have taken
; it, and then the vector is served as a trapped one always was.
;---------------------------------------------------------------------
vdm_dvs:
        or      eax,eax
        jz      vdm_ccret               ; an INT 23h handler, come back
        cmp     eax,21h
        je      vdm_s21
        cmp     eax,2Fh
        je      vdm_s2F
        cmp     eax,29h
        je      vdm_s29
        cmp     eax,25h
        je      vdm_s25
        cmp     eax,26h
        je      vdm_s26
        cmp     eax,20h
        je      vdm_s20
        cmp     eax,27h
        je      vdm_s27
        cmp     eax,23h
        je      vdm_s23
        jmp     vdm_abend               ; a HLT dvs_which should not pass

vdm_s21:
        call    vdm_ciret
        jmp     vdm_dos
vdm_s20:
        call    vdm_ciret
        cmp     byte ptr ds:[irx_busy],0        ; nothing to end in an
        jne     vdm_loop                        ; excursion (IRQX.INC)
        jmp     vdm_ret0
vdm_s27:                                ; terminate and stay resident, the
        call    vdm_ciret               ; DOS 1 spelling
        cmp     byte ptr ds:[irx_busy],0
        jne     vdm_loop
        jmp     vdm_keep27

;--- 2Fh: the multiplex.  Whatever was in front of the stub has already
;--- had its turn, and a subfunction none of ours answers simply comes
;--- back - which is what DOS's own INT 2Fh does at the end of the chain.
vdm_s2F:
        call    vdm_ciret
        call    dvs_do2F
        cmp     byte ptr ds:[irx_busy],0        ; nothing parks from inside
        je      vs2f_park                       ; an interrupt handler
        mov     byte ptr ds:[win_parkreq],0
vs2f_park:
        cmp     byte ptr ds:[win_parkreq],0
        jne     vdm_winpark
        jmp     vdm_loop

;--- 29h: the fast console write.  To the console and nowhere else - it
;--- is how a program writes past its own redirection - with every
;--- register kept, as DOS keeps them.  This was an IRET, which lost the
;--- character.
vdm_s29:
        call    vdm_ciret
        call    dvs_do29
        jmp     vdm_loop

;--- 23h: the end of the ^C chain.  Nothing in front of the stub kept the
;--- program alive, and the default ^C handler - COMMAND.COM's, on DOS -
;--- ends it.  Only it: its parent's 4Bh returns with 4Dh saying why.
vdm_s23:
        call    vdm_ciret
        mov     eax,VDX_CTRLC
        jmp     vdm_killprog

;--- 25h / 26h: absolute disk read and write.  THE FLAGS STAY ON THE
;--- STACK: these two return with RETF, not IRET, and every caller since
;--- 1981 POPFs the word they leave behind.  The flags the caller is
;--- handed are the ones it pushed, with the answer in the carry.
vdm_s25:
        xor     edx,edx
        jmp     short vdm_sabs
vdm_s26:
        mov     edx,1
vdm_sabs:
        call    vdm_cretf
        call    dvs_doabs
        jmp     vdm_loop

;--- vdm_cretf - the half of an IRET INT 25h and 26h do: IP and CS off
;--- the guest's stack, and FLAGS read but left there.
vdm_cretf:
        push    eax
        call    vdm_cpop
        mov     dword ptr [vdm_ctx+V86C_EIP],eax
        call    vdm_cpop
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        call    vdm_cpop                ; a look at the flags...
        mov     dword ptr [vdm_ctx+V86C_FLAGS],eax
        sub     word ptr [vdm_ctx+V86C_ESP],2   ; ...which stay where they are
        pop     eax
        ret

vdm_int:
        cmp     ebx,15h
        je      vdm_i15
IFDEF V86_NOMODE
        cmp     ebx,10h
        je      vdm_i10
ENDIF
        cmp     ebx,67h                         ; expanded memory, and the bit
        je      vdm_i67                         ; is only set when there is any
        cmp     ebx,13h                         ; the disk BIOS, served out
        jne     vdm_notours                     ; of the kernel's own block
        jmp     vdm_i13                         ; layer - see INT13.INC
vdm_notours:
        jmp     vdm_reflect                     ; 22h-2Eh: the guest's own

;---------------------------------------------------------------------
; INT 15h, the vector PM-DOS claims a PIECE of.  The handler answers CF
; clear if the subfunction was one of ours and CF set if it was not,
; and "not ours" means exactly that: hand the interrupt to whatever
; the guest has in its own IVT.  INT 15h carries the mouse and the
; wait services, and a monitor that claimed the vector and swallowed
; what it did not recognise would break all of that without a message.
;
; INT 2Fh used to be claimed the same way.  It is not claimed now: it
; goes through the guest's table like DOS's, and dvs_do2F is what the
; end of the chain reaches.
;---------------------------------------------------------------------
vdm_i15:
        mov     esi,OFFSET vdm_ctx      ; C2h, the BIOS's mouse: refused while
        call    dword ptr ds:[IOS_MSI15]        ; the kernel drives it
        jnc     vdm_loop
        call    xms_int15
        jc      vdm_reflect
        jmp     vdm_loop

;---------------------------------------------------------------------
; dvs_do2F - the kernel's own INT 2Fh subfunctions, for the guest in
;            vdm_ctx.  Each answers CF clear if it was one of its own;
;            anything else is left as it came, which at the end of the
;            chain is the whole of the right answer.
;---------------------------------------------------------------------
dvs_do2F:
        call    dos12_int2F             ; AH=12h: DOS's own services
        jnc     d2f_out
        call    xms_int2F
        jnc     d2f_out
        call    dpmi_int2F              ; 1686h/1687h: is there a host?
        jnc     d2f_out
        call    cdx_int2F               ; AH=15h: the MSCDEX API, which
                                        ; this kernel answers itself - see
                                        ; MSCDEX.INC for why there is no
                                        ; MSCDEX.EXE to answer it
        jnc     d2f_out
        call    cache_int2F             ; AX=4A10h: the SMARTDRV API, for
                                        ; the same reason one level along -
                                        ; the kernel HAS a disk cache, so
                                        ; SMARTDRV.EXE must be able to find
                                        ; out and not load a second one
        jnc     d2f_out
        call    share_int2F             ; AX=1000h: SHARE is installed, and
                                        ; SHARE.EXE must be told so
        jnc     d2f_out
        call    win_int2F               ; and the 16xx Windows family, whose
                                        ; 1605h asks for a park in either
                                        ; mode: the caller looks at
                                        ; win_parkreq
d2f_out:
        ret

;=====================================================================
; dos12_int2F - INT 2Fh AH=12h, the services DOS offers the programs
;               that live close to it.  CF clear if it was one of them.
;
; MS-DOS'S OWN COMMAND.COM CALLS THESE, and one of them decides whether
; it can take an answer.  It upper-cases every Y, N, A, R, I and F it
; reads with 1213h, and with nothing behind 1213h the character came
; back as 13h - AL, from the AX the call was made with - so "Terminate
; batch job (Y/N)?" and "Abort, Retry, Fail?" asked again whatever was
; typed, for ever.  Nobody had seen it because until the DOS vectors
; went through the guest's table, a ^C never reached COMMAND.COM's
; handler to ask.
;
; DOS's own contract (mscode.asm, DispatchDOS): the routine is called
; with AX replaced by the word the caller pushed before the INT, and it
; leaves by RETF 2, so the flags it sets are the caller's.  The pushed
; word is under the IRET frame - which vdm_ciret has already taken off
; - so it is the word at SS:SP now, and the caller discards it.
;
; The ones here are the ones that need nothing of DOS's insides, which
; is every one a program outside DOS has a use for: install check,
; path separators, a character out, the packed date, and the string
; routines with DOS's own rules - upper case, '/' the same as '\'.
; The rest name DOS's buffers, SFTs and CDSs, which are not DOS's shape
; here; they go on down the chain and come back unanswered.
;=====================================================================
dos12_int2F:
        cmp     byte ptr [vdm_ctx+V86C_EAX+1],12h
        jne     d12_no
        pushad
        mov     esi,dword ptr [vdm_ctx+V86C_SS]         ; the pushed word
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_ESP]
        movzx   eax,word ptr [esi+eax]
        mov     [d12_arg],eax
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,00h
        je      d12_install
        cmp     al,04h
        je      d12_pathchr
        cmp     al,05h
        je      d12_out
        cmp     al,0Dh
        je      d12_date
        cmp     al,11h
        je      d12_strcpy
        cmp     al,12h
        je      d12_strlen
        cmp     al,13h
        je      d12_ucase
        cmp     al,14h
        je      d12_pointcmp
        cmp     al,1Ah
        je      d12_drvtext
        cmp     al,1Eh
        je      d12_strcmp
        cmp     al,21h
        je      d12_truename
        cmp     al,25h
        je      d12_dstrlen
        popad
d12_no:
        stc
        ret
d12_done:
        popad
        clc
        ret

;--- 00h: DOS is here
d12_install:
        mov     byte ptr [vdm_ctx+V86C_EAX],0FFh
        jmp     d12_done

;--- 04h: is AL a path separator?  '/' becomes '\', ZF says either
d12_pathchr:
        mov     eax,[d12_arg]
        call    d12_sep
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        call    d12_zf
        jmp     d12_done

;--- 05h: the pushed character, to standard output
d12_out:
        mov     edx,[d12_arg]
        mov     eax,0200h
        call    k21u                    ; the guest's own handle 1
        jmp     d12_done

;--- 0Dh: AX = today, DX = now, the way a directory entry holds them
d12_date:
        mov     eax,2A00h
        call    k21
        mov     eax,dword ptr ds:[k21_ecx]      ; CX = year
        sub     eax,1980
        shl     eax,9
        movzx   ebx,byte ptr ds:[k21_edx+1]     ; DH = month
        shl     ebx,5
        or      eax,ebx
        movzx   ebx,byte ptr ds:[k21_edx]       ; DL = day
        or      eax,ebx
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        mov     eax,2C00h
        call    k21
        movzx   eax,byte ptr ds:[k21_ecx+1]     ; CH = hours
        shl     eax,11
        movzx   ebx,byte ptr ds:[k21_ecx]       ; CL = minutes
        shl     ebx,5
        or      eax,ebx
        movzx   ebx,byte ptr ds:[k21_edx+1]     ; DH = seconds, in twos
        shr     ebx,1
        or      eax,ebx
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        jmp     d12_done

;--- 11h: DS:SI to ES:DI, upper-cased, '/' made '\'; both left past
;--- their NULs
d12_strcpy:
        call    d12_dssi
        mov     ebx,esi
        call    d12_esdi
d12_cpy:
        mov     al,[ebx]
        inc     ebx
        call    d12_up
        call    d12_sep
        mov     [edi],al
        inc     edi
        inc     word ptr [vdm_ctx+V86C_ESI]
        inc     word ptr [vdm_ctx+V86C_EDI]
        or      al,al
        jnz     d12_cpy
        jmp     d12_done

;--- 12h: CX = the length of the string at ES:DI, its NUL included
;--- 25h: the same, at DS:SI
d12_strlen:
        call    d12_esdi
        jmp     short d12_len
d12_dstrlen:
        call    d12_dssi
        mov     edi,esi
d12_len:
        xor     ecx,ecx
d12_lenl:
        inc     ecx
        cmp     byte ptr [edi],0
        je      d12_lend
        inc     edi
        cmp     ecx,0FFFFh
        jb      d12_lenl
d12_lend:
        mov     word ptr [vdm_ctx+V86C_ECX],cx
        jmp     d12_done

;--- 13h: the pushed character, upper case, in AL
d12_ucase:
        mov     eax,[d12_arg]
        call    d12_up
        mov     byte ptr [vdm_ctx+V86C_EAX],al
        jmp     d12_done

;--- 14h: ZF if DS:SI and ES:DI are the same pointer
d12_pointcmp:
        mov     eax,dword ptr [vdm_ctx+V86C_ESI]
        cmp     ax,word ptr [vdm_ctx+V86C_EDI]
        jne     d12_pcset
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        cmp     ax,word ptr [vdm_ctx+V86C_ES]
d12_pcset:
        call    d12_zf
        jmp     d12_done

;--- 1Ah: a drive letter at DS:SI?  AL = 0 if none, else 1 for A: and
;--- SI past the colon - and FFh if the "letter" was not one
d12_drvtext:
        call    d12_dssi
        xor     eax,eax
        cmp     byte ptr [esi],0
        je      d12_dtset
        cmp     byte ptr [esi+1],':'
        jne     d12_dtset
        mov     al,[esi]
        or      al,20h
        sub     al,'a' - 1
        add     word ptr [vdm_ctx+V86C_ESI],2
        or      al,al
        jnz     d12_dtset
        mov     al,0FFh
d12_dtset:
        mov     byte ptr [vdm_ctx+V86C_EAX],al
        jmp     d12_done

;--- 1Eh: ZF if DS:SI and ES:DI name the same thing, DOS's way: case
;--- does not count and nor does which slash
d12_strcmp:
        call    d12_dssi
        call    d12_esdi
d12_cmpl:
        mov     al,[esi]
        inc     esi
        call    d12_up
        call    d12_sep
        mov     ah,al
        mov     al,[edi]
        inc     edi
        call    d12_up
        call    d12_sep
        cmp     ah,al
        jne     d12_cmpset
        or      al,al
        jnz     d12_cmpl
d12_cmpset:
        call    d12_zf
        jmp     d12_done

;--- 21h: 60h by another name
d12_truename:
        mov     byte ptr [vdm_ctx+V86C_EAX+1],60h
        mov     al,60h
        call    vdm_do21                ; and AX and CF are its answer
        jmp     d12_done

; the character in AL: '/' becomes '\', and ZF says it is either
d12_sep:
        cmp     al,'/'
        jne     d12_sepb
        mov     al,'\'
        ret
d12_sepb:
        cmp     al,'\'
        ret

; the character in AL, upper case: ASCII's, and the country table's
; for 80h and up - the one 6520h answers from
d12_up:
        cmp     al,80h
        jae     d12_uphi
        cmp     al,'a'
        jb      d12_upout
        cmp     al,'z'
        ja      d12_upout
        sub     al,20h
d12_upout:
        ret
d12_uphi:
        push    ebx
        movzx   ebx,al
        mov     al,byte ptr ds:[DOSDATA + GNLS_UPCASE + 2 + ebx - 80h]
        pop     ebx
        ret

; the context's ZF from the flags as they stand
d12_zf:
        lahf
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT 40h
        test    ah,40h
        jz      d12_zfout
        or      dword ptr [vdm_ctx+V86C_FLAGS],40h
d12_zfout:
        ret

; DS:SI and ES:DI, flat
d12_dssi:
        mov     esi,dword ptr [vdm_ctx+V86C_DS]
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_ESI]
        add     esi,eax
        ret
d12_esdi:
        mov     edi,dword ptr [vdm_ctx+V86C_ES]
        shl     edi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EDI]
        add     edi,eax
        ret

d12_arg     dd  0

;---------------------------------------------------------------------
; dvs_do29 - INT 29h: AL to the console, every register kept.
;---------------------------------------------------------------------
dvs_do29:
        pushad
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        call    dword ptr ds:[IOS_CONOUT]
        popad
        ret

;---------------------------------------------------------------------
; dvs_doabs - INT 25h (EDX = 0) or 26h (EDX = 1) for the guest in
;             vdm_ctx: AL = the drive, and either CX sectors from DX
;             into DS:BX, or CX = FFFFh and DS:BX -> the packet a
;             volume over 32MB needs (dd first sector, dw count, dd
;             buffer).  CF and AX in the context, DOS's way: AH the
;             driver's status, AL the INT 24h code.
;
; The kernel's own 25h and 26h do the transfer - called from kernel
; code, they take a flat buffer and trust it, so the buffer is measured
; here first, against the volume's own sector size.
;---------------------------------------------------------------------
dvs_doabs:
        pushad
        mov     [dva_write],edx
        mov     ebx,dword ptr [vdm_ctx+V86C_DS]         ; DS:BX, flat
        shl     ebx,4
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        add     ebx,eax
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        cmp     ecx,0FFFFh
        jne     dva_regs
        mov     edx,[ebx]                               ; the packet
        movzx   ecx,word ptr [ebx+4]
        mov     esi,[ebx+6]
        call    vdm_farflat
        mov     ebx,eax
dva_regs:
        mov     [dva_start],edx
        mov     [dva_count],ecx
        mov     [dva_buf],ebx

        ; the buffer, in the volume's own sectors - which vol_select
        ; is what knows; a drive it will not select is left for the
        ; transfer itself to refuse, with the error DOS would give
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        cmp     al,MAX_DRIVES
        jae     dva_go
        call    vol_select
        jc      dva_go
        mov     eax,[dva_count]
        cmp     eax,100000h
        jae     dva_go                  ; too many: 25h's own range check
        movzx   ecx,byte ptr [fat_lshift]
        add     ecx,9
        shl     eax,cl
        mov     ecx,eax
        mov     eax,[dva_buf]
        call    buf_ok
        jc      dva_deny
dva_go:
        mov     al,byte ptr [vdm_ctx+V86C_EAX]
        mov     edx,[dva_start]
        mov     ecx,[dva_count]
        mov     ebx,[dva_buf]
        cmp     dword ptr [dva_write],0
        jne     dva_wr
        int     25h
        jmp     short dva_done
dva_wr:
        int     26h
dva_done:
        jnc     dva_ok
dva_fail:
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        popad
        ret
dva_ok:
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        popad
        ret
dva_deny:
        mov     ax,010Ch                ; "general failure", and a status
        jmp     short dva_fail          ; of "bad command": it was

dva_write   dd  0
dva_start   dd  0
dva_count   dd  0
dva_buf     dd  0

;--- INT 67h: the whole vector, and every subfunction of it.  Unlike
;--- 15h and 2Fh there is nothing on 67h that belongs to anyone else:
;--- it was allocated to expanded memory and to nothing else, and a
;--- manager that hands part of it back would be handing it to the
;--- IRET it replaced.
vdm_i67:
        call    ems_call
        jmp     vdm_loop

IFDEF V86_NOMODE
;--- INT 10h AH=00h, swallowed.  A DEBUG SWITCH AND NOTHING ELSE ----
; Every trace this monitor has writes to the text page, and a guest
; that sets a graphics mode takes the screen away from the thing
; watching it - at exactly the moment the watching matters.  Windows
; puts up its logo and then hangs: with this the logo never happens,
; the guest carries on believing it did, and the trace stays
; readable.  Nothing else about the call is faked, so a guest that
; then WRITES to A0000 writes to memory nobody is displaying.
vdm_i10:
        cmp     byte ptr [vdm_ctx+V86C_EAX+1],0
        jne     vdm_reflect
        jmp     vdm_loop
ENDIF

vdm_reflect:
        mov     eax,ebx
        mov     esi,OFFSET vdm_ctx
        call    dword ptr ds:[IOS_V86INT]
        jmp     vdm_loop

;=====================================================================
; vdm_winpark - Windows broadcast 1605h, in either mode, and PM-DOS
;               said yes.  Now it has to mean it.
;
; WHY SAYING YES IS NOT ENOUGH, and why this is a handover rather than
; a permission.  DOSX.EXE is a DOS extender: before it switches the
; machine into protected mode it checks whether anybody else already
; has, with SMSW, and the PE bit answers for the whole machine.  Under
; this kernel it reads back set - because it IS set, the guest is in
; V86 mode inside a protected-mode monitor - and DOSX will not switch a
; machine that is already switched.  It looks for the two ways a
; resident monitor is supposed to let it in, VCPI on INT 67h and DPMI
; on INT 2Fh AX=1687h, and takes neither: it never asks for DPMI at
; all, and PM-DOS has no VCPI.  So it prints "Cannot start Windows in
; Standard Mode" and leaves.
;
; There is no answer to that from inside protected mode.  The bit it
; is reading is the truth.  The only way to make the answer no is to
; stop being in protected mode - which is what parking is, and what
; RMDOS.SYS is underneath it.
;
; WIN386.EXE, THE ENHANCED-MODE HALF, wants more than a real-mode
; machine: it is a V86 monitor, and it wants the ring-0 seat, its own
; page tables, GDT and IDT.  The same park gives it those - the seat
; is empty once PM-DOS has left it - and what WIN386 then asks of the
; DOS underneath (the head of RMINFO.INC) is RMDOS's to answer, not
; ours.  Where the two modes differ for PM-DOS is rm_park and rmp_back
; (RMCALL.INC): WIN386 loads system registers DOSX never touches, and
; the unpark has to put every one of them back.
;
; THE GUEST IS NOT TOLD, AND CANNOT TELL.  It executed an INT and it
; gets a return from that INT with its registers intact; that the
; return arrives in real mode, on a machine whose DOS is now a
; different and much smaller one, is not a thing an 8086 instruction
; stream can observe.  It observes only what it went looking for: PE
; clear and a machine it may have, or a seat with nobody in it.
;=====================================================================
vdm_winpark:
        mov     byte ptr ds:[win_parkreq],0
        call    park_fromv86
        xor     esi,esi
        call    park_run
        jc      vwp_failed

        ; Windows has ended and the machine is ours again.  From this
        ; box's point of view the program that broadcast 1605h has
        ; simply finished, so finish it the way it would have finished
        ; itself - 4Ch, carrying the code RMDOS brought back - and let
        ; the ordinary exit path unwind it.  Its parent's EXEC returns,
        ; and nothing in the box has to know where it has been.
IFDEF EXIT_TRACE
        XTMARK32 66,05Ah                ; 'Z'
ENDIF
        mov     byte ptr [vdm_ctx+V86C_EAX],al
        jmp     vdm_term

;--- The excursion did not happen: no RMDOS.SYS, no room for it, or a
;--- file still open.  Say so, because the guest is about to act on a
;--- permission that has just been withdrawn - and then withdraw it
;--- properly, by vetoing after all.  A veto is the one answer that is
;--- always safe: Windows unwinds and the machine is exactly as it was.
;--- ...and it says WHICH of the three, because the three have nothing
;--- in common from the user's side: one is a missing file, one is a
;--- full machine, and one is something the user did a moment ago and
;--- can undo.  park_run already distinguishes them in EAX and the
;--- first version of this handler threw that away.
vwp_failed:
        push    esi
        push    eax
        mov     esi,OFFSET msgWinPark
        call    dword ptr ds:[IOS_CONSTR]
        pop     eax
        mov     esi,OFFSET msgWinPMem
        cmp     eax,ERR_ACCESS
        jne     vwp_why2
        mov     esi,OFFSET msgWinPFile
        jmp     short vwp_say
vwp_why2:
        cmp     eax,ERR_NOFILE
        jne     vwp_why3
        mov     esi,OFFSET msgWinPNoRM
        jmp     short vwp_say
vwp_why3:
        cmp     eax,ERR_BADDATA
        jne     vwp_say
        mov     esi,OFFSET msgWinPBad
vwp_say:
        call    dword ptr ds:[IOS_CONSTR]
        pop     esi
        mov     word ptr [vdm_ctx+V86C_ECX],1
        jmp     vdm_loop

;---------------------------------------------------------------------
; INT 21h from the guest
;---------------------------------------------------------------------
vdm_dos:
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX+1]
        cmp     byte ptr ds:[irx_busy],0        ; a handler an excursion ran
        jne     irx_dos                         ; (IRQX.INC) has no program
vdm_dos_go:
        cmp     al,4Ch
        je      vdm_term
        cmp     al,00h
        je      vdm_ret0
        cmp     al,31h
        je      vdm_keep
        cmp     al,4Dh
        je      vdm_getcode
        cmp     al,48h
        je      vdm_alloc
        cmp     al,49h
        je      vdm_dealloc
        cmp     al,4Ah
        je      vdm_setblock
vdm_dos_v86:                             ; dl_excur comes in here
        cmp     al,62h
        je      vdm_getpsp
        cmp     al,51h                          ; 51h and 62h are the same
        je      vdm_getpsp                      ; call; 51h was the one DOS
        cmp     al,50h                          ; never wrote down
        je      vdm_setpsp
        cmp     al,2Fh
        je      vdm_getdta
        cmp     al,34h
        je      vdm_indosptr
        cmp     al,52h
        je      vdm_lol
        cmp     al,0Eh
        je      vdm_seldrv
        cmp     al,5Dh
        je      vdm_sda
        cmp     al,1Bh
        je      vdm_dsptr
        cmp     al,1Ch
        je      vdm_dsptr
        cmp     al,1Fh
        je      vdm_dsptr
        cmp     al,32h
        je      vdm_dsptr
        cmp     al,63h
        je      vdm_dsptrsi
        cmp     al,38h
        je      vdm_country
        cmp     al,65h
        je      vdm_ctryext
        cmp     al,4Bh
        je      vdm_child
        cmp     al,25h
        je      vdm_setvec
        cmp     al,35h
        je      vdm_getvec
        cmp     al,42h
        je      vdm_seek

;--- everything else goes to the kernel's own handler ----------------
        call    vdm_do21
        jmp     vdm_loop

;=====================================================================
; vdm_do21 - service one INT 21h out of vdm_ctx.  AL = the function,
;            which the caller has already read out of the context to
;            decide that this call is not one of the special cases
;            above.  Returns with the answer written back into the
;            context.
;
; It is a subroutine rather than a stretch of vdm_loop because a DOS
; box is no longer the only thing that makes 16-bit DOS calls: a
; DEVICE= driver's INIT routine makes them too, from an excursion into
; V86 that has no program, no PSP and no box around it, and it would
; be absurd for the driver that loads it to own a second copy of the
; flattening, the 29h rule and the pointer guard.
;=====================================================================
vdm_do21:
        ; 29h IS THE ONLY DOS CALL THAT ANSWERS IN A POINTER REGISTER.
        ; It leaves SI past the text it parsed, and this bridge is
        ; built on the assumption that pointers only ever travel
        ; inwards - it flattens them on the way in and never looks at
        ; them again, because until the FCB API went in there was
        ; nothing to look at.  Handing the guest the kernel's flat
        ; address there would send it walking off above 1MB with it,
        ; so the ANSWER is carried out as a distance rather than an
        ; address: see p29_adv, which is the one thing 29h publishes
        ; that its own registers cannot.
        mov     byte ptr ds:[vdm_p29],0
        cmp     al,29h
        jne     vd_not29
        mov     byte ptr ds:[vdm_p29],1
vd_not29:
        mov     ah,byte ptr [vdm_ctx+V86C_EAX]  ; AL: the subfunction, for the
        call    vdm_flags                       ; families that need it
        mov     byte ptr ds:[vdm_pf],bl

        movzx   eax,word ptr [vdm_ctx+V86C_EDX]         ; DX, maybe DS:DX
        test    byte ptr ds:[vdm_pf],P_DX
        jz      vd_nodx
        mov     edx,dword ptr [vdm_ctx+V86C_DS]
        shl     edx,4
        add     eax,edx
vd_nodx:
        mov     dword ptr ds:[vdm_adx],eax

        movzx   eax,word ptr [vdm_ctx+V86C_ESI]         ; SI, maybe DS:SI
        test    byte ptr ds:[vdm_pf],P_SI
        jz      vd_nosi
        mov     edx,dword ptr [vdm_ctx+V86C_DS]
        shl     edx,4
        add     eax,edx
vd_nosi:
        mov     dword ptr ds:[vdm_asi],eax

        movzx   eax,word ptr [vdm_ctx+V86C_EDI]         ; DI, maybe ES:DI
        test    byte ptr ds:[vdm_pf],P_DI
        jz      vd_nodi
        mov     edx,dword ptr [vdm_ctx+V86C_ES]
        shl     edx,4
        add     eax,edx
vd_nodi:
        mov     dword ptr ds:[vdm_adi],eax

        movzx   eax,word ptr [vdm_ctx+V86C_EBX]         ; BX, maybe ES:BX
        test    byte ptr ds:[vdm_pf],P_BX
        jz      vd_nobx
        mov     edx,dword ptr [vdm_ctx+V86C_ES]
        test    byte ptr ds:[vdm_pf],P_BXD
        jz      vd_bxes
        mov     edx,dword ptr [vdm_ctx+V86C_DS]
vd_bxes:
        shl     edx,4
        add     eax,edx
vd_nobx:
        mov     ebx,eax

        ; THE POINTER HALF of what buf_ok does for lengths, and the
        ; cheaper half: a guest's segment:offset cannot reach
        ; PMDOS.SYS at all, but it reaches the kernel's low
        ; scaffolding without trying.  0000:0600 is the thunk's
        ; register frame; 0000:0800 is the real-mode stack the kernel
        ; is standing on while it services this very call; and every
        ; DOS program that has ever zeroed a segment register by
        ; accident has pointed somewhere in there.  One byte is the
        ; right measure here - what the call then DOES with the
        ; pointer is measured where the length is known.
        call    vdm_ptrchk
        jc      vd_denied

        mov     edx,dword ptr ds:[vdm_adx]
        mov     esi,dword ptr ds:[vdm_asi]
        mov     edi,dword ptr ds:[vdm_adi]
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]

        ; DX AND BX COME BACK AS THE GUEST LEFT THEM, unless the call
        ; answered in them.  vdm_result copies the kernel's EDX and EBX
        ; out whole, and for a call that took DS:DX that is the FLAT
        ; address - so the guest's DX came back as the low word of
        ; DS*16+DX.  DOS preserves DX across 3Fh and 40h and MS-DOS's own
        ; SORT counts on it (ADD DX,AX after every read): it read the
        ; right bytes and then believed its text ran on for kilobytes of
        ; whatever the buffer had held, and sorted that too.  A handler
        ; that ANSWERS in DX or BX writes something other than the flat
        ; pointer it was given, which is how the two are told apart.
        ; On the stack rather than in cells: a guest's critical-error
        ; handler makes DOS calls while this one is still open.
        push    dword ptr ds:[vdm_pf]
        push    dword ptr [vdm_ctx+V86C_EDX]    ; the guest's own DX...
        push    dword ptr [vdm_ctx+V86C_EBX]    ; ...and BX
        push    edx                             ; and what was passed
        push    ebx
        push    dword ptr ds:[vdm_gframe]       ; and which call is open, for
        mov     dword ptr ds:[vdm_gframe],-1    ; a ^C in this one: k21u
        call    k21u                    ; u for the guest's: see buf_ok
        pop     dword ptr ds:[vdm_gframe]       ; writes its frame in
        call    vdm_result
        pop     ebx
        pop     edx
        pop     esi                     ; the guest's BX
        pop     edi                     ; the guest's DX
        pop     ecx                     ; CL = the pointer flags
        test    cl,P_DX
        jz      vd_dxok
        cmp     edx,dword ptr ds:[k21_edx]
        jne     vd_dxok                 ; it answered in DX
        mov     word ptr [vdm_ctx+V86C_EDX],di
vd_dxok:
        test    cl,P_BX
        jz      vd_bxok
        cmp     ebx,dword ptr ds:[k21_ebx]
        jne     vd_bxok
        mov     word ptr [vdm_ctx+V86C_EBX],si
vd_bxok:
        cmp     byte ptr ds:[vdm_p29],0
        je      vd_done
        mov     eax,dword ptr ds:[p29_adv]      ; how far the parse got, as a
        add     word ptr [vdm_ctx+V86C_ESI],ax  ; distance from where it began
vd_done:
        ret

vd_denied:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_ACCESS
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        ret

;---------------------------------------------------------------------
; vdm_ptrchk - every pointer this call flattened, measured against the
;              kernel's own memory.  CF if any of them is in it.
;              EBX holds the flattened BX; the other three are in the
;              vdm_a?? cells, which is why this cannot simply loop.
;---------------------------------------------------------------------
vdm_ptrchk:
        push    eax
        push    ecx
        mov     ecx,1
        test    byte ptr ds:[vdm_pf],P_DX
        jz      vpc_nodx
        mov     eax,dword ptr ds:[vdm_adx]
        call    buf_ok
        jc      vpc_bad
vpc_nodx:
        test    byte ptr ds:[vdm_pf],P_SI
        jz      vpc_nosi
        mov     eax,dword ptr ds:[vdm_asi]
        call    buf_ok
        jc      vpc_bad
vpc_nosi:
        test    byte ptr ds:[vdm_pf],P_DI
        jz      vpc_nodi
        mov     eax,dword ptr ds:[vdm_adi]
        call    buf_ok
        jc      vpc_bad
vpc_nodi:
        test    byte ptr ds:[vdm_pf],P_BX
        jz      vpc_good
        mov     eax,ebx
        call    buf_ok
        jc      vpc_bad
vpc_good:
        pop     ecx
        pop     eax
        clc
        ret
vpc_bad:
        pop     ecx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; vdm_result - the kernel's answer, back into the guest's registers.
;              Sixteen bits of each, and the carry flag, which for
;              most of the DOS API is the whole return value.
;---------------------------------------------------------------------
vdm_result:
        mov     eax,dword ptr ds:[k21_eax]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        mov     eax,dword ptr ds:[k21_ebx]
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     eax,dword ptr ds:[k21_ecx]
        mov     word ptr [vdm_ctx+V86C_ECX],ax
        mov     eax,dword ptr ds:[k21_edx]
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        test    dword ptr ds:[k21_flg],FL_CF
        jz      vr_nocf
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
vr_nocf:
        ret

;--- the calls that cannot be passed through -------------------------
; 48h - BX paragraphs.
;
; AND THE BLOCK BELONGS TO THE PROGRAM THAT ASKED FOR IT, which is the
; whole of what this line does and the whole of what was missing.
; lm_alloc leaves a block owning itself; the exit sweep frees by PSP;
; so every allocation a DOS box ever made stood there until the
; machine was switched off.  Nothing complained, because a leak is not
; an error message - it is a largest-free-block that keeps getting
; smaller.  MSD takes two of these and MEM afterwards reported 604K
; free with the biggest run 309K, which is the shape a leak makes.
;
; The CURRENT PSP, not the one the box was started with, because that
; is DOS's own rule and a TSR allocating on somebody's behalf relies
; on it.  In every ordinary case they are the same value.
vdm_alloc:
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        call    lm_alloc
        jc      va_fail
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        mov     dx,word ptr ds:[vdm_curpsp]
        call    lm_setown
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop
va_fail:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_NOMEM
        mov     word ptr [vdm_ctx+V86C_EBX],bx
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

vdm_dealloc:                                    ; 49h - ES = the block
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        call    lm_free
        jc      vf_fail
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop
vf_fail:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_BADBLOCK
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

vdm_setblock:                                   ; 4Ah - ES, BX paragraphs
        mov     eax,dword ptr [vdm_ctx+V86C_ES]
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        call    lm_resize
        jc      vsb_fail
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop
vsb_fail:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_NOMEM
        mov     word ptr [vdm_ctx+V86C_EBX],bx
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

vdm_getpsp:                                     ; 62h/51h - BX = our PSP
        movzx   eax,word ptr ds:[vdm_curpsp]
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

; 50h - set the current PSP.  It moves what 51h/62h answer and NOTHING
; else: the block this VM will give back at the end is the one it was
; given, not whatever a program has since decided to call its own.  A
; TSR swapping PSPs must not be able to make the cleanup free somebody
; else's memory.
vdm_setpsp:
        mov     eax,dword ptr [vdm_ctx+V86C_EBX]
        mov     word ptr ds:[vdm_curpsp],ax
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

vdm_getdta:                                     ; 2Fh - ES:BX = the DTA
        mov     eax,dword ptr [dta_ptr]         ; flat, back to a far pointer
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     dword ptr [vdm_ctx+V86C_ES],edx
        jmp     vdm_loop

; 34h - ES:BX = the InDOS flag.  A guest can only hold an address as a
; segment and an offset, which is the entire reason the flag lives at
; DOS_INDOS in the BIOS data area rather than in the kernel's data
; with everything else it belongs with.  0040:00F7 is what a TSR sees.
;
; ...IN THE DOS DATA SEGMENT NOW, AND AS DOS SAYS IT: the segment is
; the DOS data segment's own and the offset is the flag's in it,
; 0400:0321, not the same byte normalised to 0432:0001.  A program that
; takes ES from here as the DOS data segment - IFSMGR.386 adds DOS 5's
; offsets to it - is entitled to (2026-10-06; dd_split).
vdm_indosptr:
        mov     eax,DOS_INDOS
        call    dd_split                        ; -> DX:AX
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        mov     dword ptr [vdm_ctx+V86C_ES],edx
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

; 52h - ES:BX = the list of lists.  DOS's most-used undocumented call,
; and the one this bridge could NOT pass through to the kernel: fn52
; answers a flat pointer to dos_lol, vdm_result would hand the guest
; its bottom sixteen bits with ES left at whatever it happened to be,
; and the program would read a first-MCB segment out of a stranger's
; memory.  A memory-mapping tool then walks 'M' blocks from a random
; segment, never meets a 'Z', and never faults either, because every
; byte of it is mapped - so it wraps the 20-bit address space until
; somebody reaches for the power switch.  That is a hang with no
; exception behind it, which is exactly what Quarterdeck's Manifest
; did at its splash screen.
;
; The two drive counts and LASTDRIVE are filled HERE rather than in
; vdm_lolinit, because a volume can be mounted after boot and a count
; copied once would be a lie the moment it was - and because
; vdm_lolinit runs at boot, BEFORE CONFIG.SYS has been read at all, so
; a LASTDRIVE= written there would be the default every time.
; 1Bh, 1Ch and 63h - the calls that answer a FLAT pointer in a
; register the guest reads as DS:something.  The kernel's handlers
; deliberately keep what they point at inside the guest block, so all
; three need the same three lines: run the call, then split the flat
; address it answered back into a segment and an offset.
;
; ONE HANDLER FOR THREE CALLS because the difference between them is
; which register the offset goes in, and that is decided by AH.  What
; they share - a pointer below 1MB, and DS moved rather than ES - is
; the whole of what the bridge has to do.
;
; DS IS THE CALLER'S AND WE ARE MOVING IT.  That is what DOS did, and
; a program that calls one of these has already pushed DS or does not
; want it back.
; 38h - country information.  DS:DX is a buffer, EXCEPT when DX is
; FFFFh, which means "set" and is a sentinel rather than an address.
;
; That is why it cannot go in vdm_ptrtab: a blanket P_DX would flatten
; the sentinel into DS*16+FFFF, which is a perfectly ordinary address
; and no longer FFFFh, so the kernel would take a set for a get and
; write 34 bytes over the end of the guest's segment.  The same trap
; vdm_subptr's comment describes for 71h/47h, one function lower.
vdm_country:
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        cmp     edx,0FFFFh
        je      vco_run                         ; leave it alone
        mov     eax,dword ptr [vdm_ctx+V86C_DS]
        shl     eax,4
        add     edx,eax
vco_run:
        ; ALL FOUR, INCLUDING THE ONE THIS CALL HAS NO USE FOR.
        ; vdm_result copies EAX, EBX, ECX and EDX back into the guest,
        ; so a register this does not LOAD is not "left alone" - it is
        ; handed back holding whatever the kernel last had in it.  CX
        ; is neither an input to 38h nor an output from it, and real
        ; DOS preserves it; ours was returning zero, because the
        ; kernel's own `rep movsb` copying the country block is the
        ; last thing to touch ECX before the frame is built.
        ;
        ; That cost MS-DOS 6.22's MEM.EXE its numbers.  Its message
        ; service formats every figure it prints by pushing digits and
        ; COUNTING THEM IN CX, which it does not zero first - and it
        ; calls 38h for the thousands separator on the way.  So the
        ; count arrived poisoned, the digit collector popped two words
        ; that were never digits, and MEM printed 1,703,7.. for 17,037
        ; and then flooded the screen.
        ;
        ; The general rule, and it is worth more than the fix: A
        ; REGISTER THE SPECIFICATION CALLS UNDEFINED IS STILL THE SAME
        ; VALUE EVERY TIME ON REAL DOS, and somebody has built on it.
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        ; ...and the same rule for DX, which went in as the flat address
        ; of the buffer and came back out as its low word.  38h answers
        ; in BX and the buffer; DX is the guest's.
        push    dword ptr [vdm_ctx+V86C_EDX]
        call    k21
        call    vdm_result
        pop     eax
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        jmp     vdm_loop

; 65h - extended country information, and the clearest case yet of why
; vdm_ptrtab cannot answer for a whole function at once: 01h takes a
; buffer in ES:DI, 21h and 22h take a string in DS:DX, and 20h and 23h
; take a character in DL and no pointer whatever.  One flag byte keyed
; on AH would have to be wrong for at least two of those, and being
; wrong here means either handing the kernel an offset with no segment
; or turning a character into a megabyte-wide address.
vdm_ctryext:
        ; BX AS WELL, since 2026-10-10: 6501h takes a code page in it
        ; and a country in DX, and until COUNTRY.SYS there was one of
        ; each and nothing read them.  It was scratch here, and came
        ; back to the guest as part of ES times sixteen.
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        movzx   edi,word ptr [vdm_ctx+V86C_EDI]

        cmp     al,01h
        je      vce_esdi
        cmp     al,02h                  ; the tables: five bytes at ES:DI
        jb      vce_nottab
        cmp     al,07h
        jbe     vce_esdi
vce_nottab:
        cmp     al,21h
        je      vce_dsdx
        cmp     al,22h
        je      vce_dsdx
        cmp     al,0A1h                 ; and DOS 7's names for the two
        je      vce_dsdx
        cmp     al,0A2h
        je      vce_dsdx
        jmp     short vce_run           ; 20h, 23h and anything unknown

vce_esdi:
        mov     esi,dword ptr [vdm_ctx+V86C_ES]
        shl     esi,4
        add     edi,esi
        jmp     short vce_run
vce_dsdx:
        mov     esi,dword ptr [vdm_ctx+V86C_DS]
        shl     esi,4
        add     edx,esi
        ; 21h and 22h answer in the string, not in DX - which comes back
        ; as the guest left it, for the reason vdm_do21 gives
        push    dword ptr [vdm_ctx+V86C_EDX]
        call    k21
        call    vdm_result
        pop     eax
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        jmp     vdm_loop
vce_run:
        call    k21
        call    vdm_result
        jmp     vdm_loop

; WHICH REGISTER IS DECIDED BEFORE THE CALL AND NOT AFTER.  vdm_result
; writes the ANSWER's AX into the context, so by the time the pointer
; needs splitting there is no function number left to test - reading
; AH there would be reading whatever the call returned.
vdm_dsptr:                                      ; 1Bh, 1Ch, 1Fh, 32h - in BX
        mov     byte ptr ds:[vdm_dssi],0
        jmp     short vd_go
; 5D06h - ES:BX would be wrong here and DS:SI is right: the swappable
; data area is answered in the SAME register pair 63h uses, and for the
; same reason 1Bh, 1Ch and 63h needed a handler at all - fn5D hands
; back a FLAT address in ESI and a guest cannot hold one.  So this is
; vdm_dsptrsi with a different door on it.
;
; Only AL=06h.  The rest of the 5Dh family is refused by the kernel
; (see fn5D), and refusing it through the ordinary bridge keeps the
; error the kernel's rather than making up a second one here.
vdm_sda:
        cmp     byte ptr [vdm_ctx+V86C_EAX],06h
        je      vdm_dsptrsi
        call    vdm_do21
        jmp     vdm_loop

vdm_dsptrsi:                                    ; 63h - offset in SI
        mov     byte ptr ds:[vdm_dssi],1
vd_go:
        ; All four, for the reason vdm_country gives at length: what is
        ; not loaded comes back to the guest as the kernel's leftovers.
        ; 1Bh and 1Ch answer IN CX, so the handler overwrites it - but
        ; 63h does not, and a guest calling 63h was having its CX taken
        ; away by a routine that had no interest in it.
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        call    k21
        call    vdm_result
        test    dword ptr ds:[k21_flg],FL_CF
        jnz     vdm_loop                        ; it failed, and the pointer
                                                ; in it means nothing
        cmp     byte ptr [vdm_ctx+V86C_EAX],0FFh
        je      vdm_loop                        ; 1Bh, 1Ch, 1Fh and 32h's
                                                ; bad-drive answer, which is
                                                ; not a carry

        mov     eax,dword ptr ds:[k21_ebx]
        cmp     byte ptr ds:[vdm_dssi],0
        je      vd_split
        mov     eax,dword ptr ds:[k21_esi]
vd_split:
        call    dd_split
        cmp     byte ptr ds:[vdm_dssi],0
        jne     vd_si
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        jmp     short vd_ds
vd_si:
        mov     word ptr [vdm_ctx+V86C_ESI],ax
vd_ds:
        mov     dword ptr [vdm_ctx+V86C_DS],edx
        jmp     vdm_loop

;---------------------------------------------------------------------
; vdm_seldrv - 0Eh, select the default drive, for a DOS box: a letter
;              that names a drive becomes the default WITHOUT A LOOK AT
;              THE DISK, as MS-DOS's $SET_DEFAULT_DRIVE does it.
;
; The kernel's fn0E mounts the volume first, and leaves the default
; alone if it will not mount - which is what CMD.EXE's "A:" is built
; on, and it stays that way for the native shell.  A DOS program was
; never promised it.  MSD walks every letter with 0Eh and 19h to see
; which exist, and on an empty diskette drive the mount was a critical
; error, which MSD's own handler noted and answered and which then
; ended MSD /P's report at Disk Drives with "Error Writing" (2026-10-09).
; Under MS-DOS the disk is read when something reads it, and a
; program's INT 24h handler hears about it then; that is so here now,
; and 16-bit COMMAND.COM's prompt asks 47h, as MS-DOS's does.
;---------------------------------------------------------------------
vdm_seldrv:
        mov     al,byte ptr [vdm_ctx+V86C_EDX]  ; DL, 0 = A:
        cmp     al,MAX_DRIVES
        jae     vsl_out
        call    drv_slot                        ; AL -> EBX, the row
        test    byte ptr [ebx+DRV_FLAGS],DRVF_PRESENT
        jz      vsl_out
        mov     byte ptr ds:[cur_drive],al
vsl_out:
        mov     byte ptr [vdm_ctx+V86C_EAX],MAX_DRIVES  ; as fn0E answers
        jmp     vdm_loop

vdm_lol:
        call    vdm_lolfresh
        jc      vl_nolol
        mov     word ptr [vdm_ctx+V86C_EBX],GLOL_PTR
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop
vl_nolol:
        ; No low memory at boot, so there is nothing to point at.  CF
        ; set is not an answer real DOS ever gives to 52h, but a caller
        ; that checks it is stopped, and one that does not is no worse
        ; off than it was before this handler existed.
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;---------------------------------------------------------------------
; vdm_lolfresh - the list of lists a guest is shown, brought up to date
;                -> EAX = its segment (GLOL_PTR into it is what 52h
;                hands out).  CF if there is none.  A DPMI client's 52h
;                is shown the same one (dpmi_post21).
;---------------------------------------------------------------------
;---------------------------------------------------------------------
; dd_split - EAX = a flat address below the megabyte -> DX = a segment
;            and AX = an offset for a guest to hold it by.  Inside the
;            DOS data segment the segment is DOSDATA_SEG and the offset
;            the address's place in it, as MS-DOS answers 34h, 5D06h,
;            1Bh, 1Ch and 63h; anywhere else, the paragraph and the
;            remainder.  The high halves of EAX and EDX come back 0.
;---------------------------------------------------------------------
dd_split:
        cmp     eax,DOSDATA
        jb      dds_para
        cmp     eax,DOSDATA + DOSDATA_SIZE
        jae     dds_para
        sub     eax,DOSDATA
        mov     edx,DOSDATA_SEG
        ret
dds_para:
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
        ret

vdm_lolfresh:
        movzx   eax,word ptr ds:[vdm_lolseg]
        or      eax,eax
        jz      vlf_none
        push    ecx
        push    edx
        push    edi
        mov     edi,eax
        shl     edi,4
        mov     dl,byte ptr ds:[drv_count]
        mov     byte ptr [edi+GLOL_NBLOCK],dl
        ; DL AND NOT EAX.  EAX is the list-of-lists SEGMENT here and is
        ; handed to the guest two lines further down - the third time
        ; this release that a register looked free and already held the
        ; answer.  The symptom was a guest reading its LASTDRIVE out of
        ; whatever segment this number happened to name.
        mov     dl,byte ptr [lastdrive_n]       ; and LASTDRIVE=, for the same
                                                ; reason: CONFIG.SYS is read
                                                ; long after vdm_lolinit ran

        ; ...RAISED AGAIN HERE, because a DRIVE CAN APPEAR AFTER THE
        ; STATEMENT DID.  cfg_lastdrv already clamps the number up to
        ; the drives that exist when the line is read - but a
        ; DEVICE= block driver further down the file adds a letter
        ; after that, and a CONFIG.SYS with LASTDRIVE= above its
        ; DEVICE= lines is the ordinary way round.  The rule is "never
        ; fewer than there really are", and the only place that can be
        ; true at the moment somebody asks is the moment somebody asks.
        cmp     dl,byte ptr ds:[drv_count]
        jae     vl_lastok
        mov     dl,byte ptr ds:[drv_count]
vl_lastok:
        mov     byte ptr [edi+GLOL_LASTDRV],dl

        ; AND THE FILE COUNT, for the same reason the two above are
        ; refreshed here: FILES= is read out of CONFIG.SYS long after
        ; vdm_lolinit built the block, and the number a guest adds up
        ; walking this chain is the number it decides with.  Windows
        ; wants thirty.
        ; ...IN ECX, BECAUSE EAX IS STILL THE ANSWER.  The comment
        ; twenty lines above says exactly this about DL, and this
        ; code was written under it and used EAX anyway.  The
        ; symptom was a guest reading its list of lists at
        ; 0028:0002 - forty, which is what FILES= says.
        ;
        ; TWO BLOCKS, and the second is only in the chain when the
        ; first cannot hold them all: a block with no entries in it is
        ; not a thing DOS ever made, and a walker is entitled to stop
        ; at one or divide by it.
        mov     ecx,dword ptr ds:[files_max]
        cmp     ecx,GSFT_ENTRIES
        jbe     vl_filesok
        mov     ecx,GSFT_ENTRIES
vl_filesok:
        mov     edx,ecx
        cmp     edx,GSFT_N1
        jbe     vl_files1
        mov     edx,GSFT_N1
vl_files1:
        mov     word ptr ds:[GSFT_BASE + GSFT_COUNT],dx
        sub     ecx,edx
        mov     word ptr ds:[GSFT2_BASE + GSFT_COUNT],cx
        mov     edx,0FFFFFFFFh
        jz      vl_files2               ; SUB's zero: nothing for block 2
        movzx   edx,word ptr ds:[vdm_lolseg]
        shl     edx,16
        mov     dx,GSFT2_OFF
vl_files2:
        mov     dword ptr ds:[GSFT_BASE + GSFT_NEXT],edx

        ; AND EVERY DRIVE'S CURRENT DIRECTORY, for the same reason as
        ; the three above: it changes, and this is when it is looked at.
        call    vdm_cdsfresh
        ; ...and its drive parameter block, which the list of lists'
        ; first field and each current directory's 45h point at.
        call    vdm_dpbfresh
        pop     edi
        pop     edx
        pop     ecx
        clc
        ret
vlf_none:
        stc
        ret

;---------------------------------------------------------------------
; vdm_cdsfresh - the current directory array, rewritten from what the
;                kernel knows now.  Everything preserved.
;
; MS-DOS 4'S ENTRY, 58h BYTES, AND THE FIELDS A PROGRAM READS ARE DOS'S:
; the path at 0 as DOS keeps it - "C:\DOS\BIN", drive and all - the
; flags word at 43h, the drive parameter block at 45h, the directory's
; start cluster at 49h, and at 4Fh the offset of the backslash that
; begins the path, which is 2 because there is no SUBST here to move
; it.  Every letter up to LASTDRIVE gets "X:\", as DOS's SYSINIT gives
; them; the ones with a volume behind them say so with 4000h, physical,
; and the rest say nothing with 0.  No JOIN, no SUBST, no network bit -
; which is what Windows for Workgroups' Setup was asking.
;
; THE DPB POINTER is the drive's slot in GDPB_ (vdm_dpbfresh) when it
; has one, and FFFF:FFFF for a CD-ROM, as MSCDEX's letters have none.
; It said FFFF:FFFF for every drive until 2026-10-09; see GDPB_ in
; PMDOS.INC for what that cost.
;
; Each drive's own directory, not the current drive's only.  The kernel
; keeps every volume's current directory in that volume's state, the
; live one in vol_state and the rest in their slots (FAT.INC,
; vol_swap), so nothing has to be selected - and nothing is, because
; selecting a floppy drive is a disk access.
;
; Refreshed on 52h, which is when a program finds the array.  RMDOS
; starts each drive where this left it (RMINFO.INC, rd_sysinit).
;---------------------------------------------------------------------
vdm_cdsfresh:
        pushad
        xor     ebx,ebx                         ; the drive
vcf_one:
        mov     edi,ebx
        imul    edi,GCDS_LEN
        add     edi,GCDS_BASE
        push    edi
        xor     eax,eax
        mov     ecx,GCDS_LEN / 4
        rep     stosd
        pop     edi
        mov     al,'A'
        add     al,bl
        mov     [edi],al
        mov     word ptr [edi+1],'\:'           ; ":\" as it lies in memory
        mov     dword ptr [edi+GCDS_DPB],0FFFFFFFFh
        mov     word ptr [edi+GCDS_BACK],2

        movzx   eax,byte ptr ds:[drv_count]
        cmp     ebx,eax
        jae     vcf_next
        mov     eax,ebx
        imul    eax,DRV_LEN
        test    byte ptr [drv_tbl + eax + DRV_FLAGS],DRVF_PRESENT
        jz      vcf_next
        mov     word ptr [edi+GCDS_FLAGS],GCDSF_PHYS
        mov     eax,ebx
        imul    eax,DRV_LEN
        test    byte ptr [drv_tbl + eax + DRV_FLAGS],DRVF_CDROM
        jz      vcf_notcd
        mov     word ptr [edi+GCDS_FLAGS],GCDSF_CDROM
vcf_notcd:
        call    vdm_dpbstate                    ; a drive parameter block?
        jc      vcf_nodpb
        mov     eax,ebx
        imul    eax,GDPB_LEN
        add     eax,(DOSDATA_SEG SHL 16) + GDPB_OFF
        mov     [edi+GCDS_DPB],eax
vcf_nodpb:

        ; this drive's state: the live one, or its slot
        mov     esi,OFFSET vol_state
        cmp     bl,byte ptr ds:[cur_vol]
        je      vcf_state
        mov     esi,ebx
        imul    esi,VOL_STATE_SIZE
        add     esi,[vol_slotp]
vcf_state:
        mov     eax,[esi + (cwd_clus - vol_state)]
        mov     [edi+GCDS_CLUS],eax
        mov     ecx,[esi + (cwd_cnt - vol_state)]
        cmp     ecx,MAX_DEPTH
        ja      vcf_next                        ; a slot never filled in
        add     esi,cwd_names - vol_state
        push    edi
        add     edi,3                           ; after "X:\"
        call    names_string
        pop     edi
vcf_next:
        inc     ebx
        cmp     ebx,LASTDRIVE
        jb      vcf_one
        popad
        ret

;---------------------------------------------------------------------
; vdm_dpbfresh - the drive parameter blocks, written from what each
;                volume's state says now, and chained in drive order
;                from the list of lists' first field.  Everything
;                preserved.
;
; See GDPB_ in PMDOS.INC.  The same states vdm_cdsfresh reads, and
; nothing is selected, so nothing is read off a disk: a drive nobody
; has read yet gets a block that says so (vdm_dpbone).  Every letter
; keeps its own slot and only the ones with a block are linked; no
; drive with one leaves the list of lists' FFFF:FFFF as it was.
;---------------------------------------------------------------------
vdm_dpbfresh:
        pushad
        mov     ecx,DOSDATA + GLOL_DPB          ; where the next link goes
        xor     ebx,ebx                         ; the drive
vdf_one:
        call    vdm_dpbstate                    ; -> ESI
        jc      vdf_next
        mov     edi,ebx
        imul    edi,GDPB_LEN
        add     edi,GDPB_BASE
        call    vdm_dpbone
        mov     eax,ebx
        imul    eax,GDPB_LEN
        add     eax,(DOSDATA_SEG SHL 16) + GDPB_OFF
        mov     [ecx],eax
        lea     ecx,[edi+EDPB_NEXT]
vdf_next:
        inc     ebx
        cmp     ebx,LASTDRIVE
        jb      vdf_one
        mov     dword ptr [ecx],0FFFFFFFFh      ; and there the chain ends
        popad
        ret

;---------------------------------------------------------------------
; vdm_dpbstate - EBX = a drive -> ESI = its volume state, the live one
;                or its slot.  CF if the letter gets no drive parameter
;                block: no volume behind it, or a CD-ROM or a mounted
;                ISO image, which have none under MSCDEX either.  EAX
;                is destroyed.
;---------------------------------------------------------------------
vdm_dpbstate:
        movzx   eax,byte ptr ds:[drv_count]
        cmp     ebx,eax
        jae     vds_none
        mov     eax,ebx
        imul    eax,DRV_LEN
        test    byte ptr [drv_tbl + eax + DRV_FLAGS],DRVF_PRESENT
        jz      vds_none
        test    byte ptr [drv_tbl + eax + DRV_FLAGS],DRVF_CDROM
        jnz     vds_none
        mov     esi,OFFSET vol_state
        cmp     bl,byte ptr ds:[cur_vol]
        je      vds_state
        mov     esi,ebx
        imul    esi,VOL_STATE_SIZE
        add     esi,[vol_slotp]
vds_state:
        cmp     byte ptr [esi + (fat_ok - vol_state)],0
        je      vds_ok                          ; not read yet
        cmp     byte ptr [esi + (vol_fs - vol_state)],FS_FAT
        jne     vds_none
vds_ok:
        clc
        ret
vds_none:
        stc
        ret

;---------------------------------------------------------------------
; vdm_dpbone - EBX = a drive, ESI = its volume state, EDI = its slot:
;              MS-DOS 4's drive parameter block for it, every number
;              in the volume's own LOGICAL sectors as 7302h's are.
;              Registers preserved; the next pointer is the caller's.
;
; A volume that has not been mounted is a drive nobody has read yet,
; and its block says that and no more: 512-byte sectors, and FFh in
; the access byte, which is how DOS marks a block it must build again
; before it believes it.  32h mounts the volume first, so it never
; hands one of these out.
;
; Sixteen-bit fields saturate at FFFFh, as 1Bh's cluster count does.
; FAT32's sectors per FAT is ZERO here, as in 7302h's block, which is
; how a reader of this layout tells that the rest is in 7302h's.  The
; free count is FFFFh, "not known", which DOS's own block says until
; something has counted - counting is a walk of the whole FAT, and
; this is written on every 52h.  The driver is FFFF:FFFF: the volumes
; are served by the kernel, and no header in the chain is theirs.
;---------------------------------------------------------------------
vdm_dpbone:
        push    eax
        push    ecx
        push    edi
        xor     eax,eax
        mov     ecx,GDPB_LEN
        rep     stosb
        pop     edi
        mov     [edi+EDPB_DRIVE],bl
        mov     [edi+EDPB_UNIT],bl
        mov     word ptr [edi+EDPB_BPS],512
        mov     dword ptr [edi+EDPB_DRIVER],0FFFFFFFFh
        mov     byte ptr [edi+EDPB_ACCESS],0FFh
        mov     word ptr [edi+EDPB_FREE16],0FFFFh
        cmp     byte ptr [esi + (fat_ok - vol_state)],0
        je      vdo_out
        mov     byte ptr [edi+EDPB_ACCESS],0

        ; CL is the volume's sector size as a shift from 512 all the way
        ; down: everything in the state counts in 512-byte units
        movzx   ecx,byte ptr [esi + (fat_lshift - vol_state)]
        mov     eax,512
        shl     eax,cl
        mov     [edi+EDPB_BPS],ax
        mov     eax,[esi + (fat_spc - vol_state)]
        shr     eax,cl
        dec     eax
        mov     [edi+EDPB_SPCM1],al
        mov     ch,0                    ; and as a shift: a cluster is a
vdo_shift:                              ; power of two sectors, so the bits
        or      al,al                   ; of one less are the shift
        jz      vdo_shifted
        shr     al,1
        inc     ch
        jmp     short vdo_shift
vdo_shifted:
        mov     [edi+EDPB_SHIFT],ch

        mov     eax,[esi + (fat_rsvd - vol_state)]
        shr     eax,cl
        mov     [edi+EDPB_RSVD],ax
        mov     eax,[esi + (fat_nfats - vol_state)]
        mov     [edi+EDPB_NFATS],al
        mov     eax,[esi + (fat_rootent - vol_state)]
        mov     [edi+EDPB_ROOTENT],ax
        mov     eax,[esi + (fat_datalba - vol_state)]
        sub     eax,[esi + (fat_partlba - vol_state)]
        shr     eax,cl
        call    vdo_sat
        mov     [edi+EDPB_DATA16],ax
        mov     eax,[esi + (fat_totclus - vol_state)]
        inc     eax                     ; the HIGHEST number, and they
        call    vdo_sat                 ; start at two
        mov     [edi+EDPB_MAXCL16],ax
        cmp     byte ptr [esi + (fat_type - vol_state)],32
        je      vdo_fat32
        mov     eax,[esi + (fat_spf - vol_state)]
        shr     eax,cl
        mov     [edi+EDPB_SPF16],ax
        mov     eax,[esi + (fat_rootlba - vol_state)]
        sub     eax,[esi + (fat_partlba - vol_state)]
        shr     eax,cl
        mov     [edi+EDPB_ROOT16],ax
vdo_fat32:
        mov     al,[esi + (fat_media - vol_state)]
        mov     [edi+EDPB_MEDIA],al
        mov     eax,[esi + (fat_hint - vol_state)]
        call    vdo_sat
        mov     [edi+EDPB_HINT16],ax
vdo_out:
        pop     ecx
        pop     eax
        ret

vdo_sat:
        cmp     eax,0FFFFh
        jbe     vdo_satok
        mov     eax,0FFFFh
vdo_satok:
        ret

;=====================================================================
; 4Bh - EXEC.  A guest starting another program.
;
; DS:DX names the file and ES:BX points at the parameter block, whose
; four fields are an environment segment, a command tail and two FCBs.
; The child is loaded into the SAME conventional memory as its parent,
; because that is the only memory a real-mode program has, and it runs
; on the same exit loop one level deeper.
;
; WHY A CHILD USUALLY FAILS TO LOAD WITHOUT 4Ah FIRST, and why that is
; correct rather than a bug to work around: a .COM is handed every free
; paragraph in the machine, so a .COM that has not shrunk itself with
; 4Ah has nothing left to give a child.  Real DOS behaves identically
; and every program that EXECs knows to shrink first.  Nothing here
; special-cases it; the memory manager simply says no.
;
; AL selects: 00h load and execute, 01h load and hand back the entry
; point, 03h load an overlay at a segment the caller names.  All three
; are here now; 01h and 03h are vc_load1 and vc_overlay below, and the
; three of them share the path-copying above and nothing else, because
; the three of them ARE three different jobs.
;=====================================================================
vdm_child:
        cmp     byte ptr [vdm_ctx+V86C_EAX],3
        je      vc_overlay
        cmp     byte ptr [vdm_ctx+V86C_EAX],1
        je      vc_load1
        cmp     byte ptr [vdm_ctx+V86C_EAX],0
        jne     vc_badfn

        ; EVERYTHING WRITTEN SO FAR GOES OUT BEFORE A PROGRAM STARTS -
        ; see fn4B, which says why.  Load-only and overlays run nothing
        ; and are left alone.
        call    dos_commit

        cmp     dword ptr ds:[vdm_depth],VDM_MAXNEST
        jae     vc_deep
        call    vc_args

        ; A 32-BIT PROGRAM, asked for by a 16-bit one - see vdm_d32.
        ; Asked of the file, as 4Bh always asks it: the extension says
        ; nothing, and a D32 command has an MS-DOS program in front of
        ; its image that would otherwise load here and print that it
        ; needs 32-bit DOS.
        mov     edx,OFFSET vdm_cpath
        call    exec_kind
        cmp     eax,EK_D32
        je      vdm_d32

;--- from here the parent is on the stack and the child is live -------
        call    vdm_push
        jc      vc_deep
        mov     ax,word ptr ds:[vdm_curpsp]     ; the child's parent is
        mov     word ptr ds:[vdm_parpsp],ax     ; whoever called 4Bh

        mov     eax,dword ptr ds:[vdm_ctail]
        mov     dword ptr ds:[vdm_tailp],eax
        mov     eax,dword ptr ds:[vdm_cenv]
        mov     dword ptr ds:[vdm_envp],eax
        mov     edx,OFFSET vdm_cpath
        call    vdm_load
        jc      vc_failed

        ; The caller's own FCBs, over the two the tail was parsed into.
        ; A null pointer means it had none to give and the parsed pair
        ; stand, which is what a program that passes 0000:0000 wants.
        movzx   edi,word ptr ds:[vdm_pspseg]
        shl     edi,4
        mov     esi,dword ptr ds:[vdm_cfcb1]
        lea     edx,[edi+RSP_FCB1]
        call    vdm_cpfcb
        mov     esi,dword ptr ds:[vdm_cfcb2]
        lea     edx,[edi+RSP_FCB2]
        call    vdm_cpfcb

        call    vdm_newid
        jmp     vdm_loop

vc_failed:
        ; The load undid its own allocations; this puts the parent back
        ; where it was and hands it the failure through 4Bh.
        call    vdm_pop
        movzx   eax,word ptr ds:[vdm_err]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop
vc_badfn:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_FUNC
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop
vc_deep:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_NOMEM
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;=====================================================================
; vdm_d32 - a DOS program has asked 4Bh for a 32-bit one.
;
; THE NATIVE BRIDGE.  Until now this could not happen: the D32 image's
; MZ stub loaded as a DOS program and said it needed 32-bit DOS, so
; COMMAND.COM could not run MORE, SORT, ATTRIB or CMD.EXE - every
; external PM-DOS has - and neither could any program that shells out.
;
; THE 32-BIT EXEC IS THE ORDINARY ONE, fn4B, and nothing in it had to
; change.  What it needs is a caller: a native program's INT 21h frame,
; which it records as the frame to come back to and which the child's
; 4Ch unwinds to and leaves through i21_return.  So the frame is built
; here, as int21_entry would build it for kernel code - SEL_CODE32 in
; the CS slot, which also tells fn4B the pointers are flat - with its
; return address at vdm_d32back.  The child's IRET home lands there,
; on this same stack, level with the box's loop again.
;
; WHILE THE CHILD RUNS, THE BOX IS NOT RUNNING, and vdm_busy says so:
; every "is a guest in the middle of this" question in the kernel - 4Ch
; ending the guest, a critical error going to the guest's INT 24h, ^C
; going to its INT 23h, a fault ending the box - is about the program
; that is running, and that is the 32-bit one.  If IT starts a DOS
; program, vdm_exec runs that one on top of this box (see there).
;
; ITS STANDARD HANDLES ARE THE DOS PROGRAM'S.  A 32-bit program reads
; slot 0 and writes slot 1; a DOS program's 0 and 1 are entries in its
; own table, and COMMAND.COM's "MORE < FILE" put a file in its entry
; 0.  So the slots the DOS program's 0-2 name are copied into slots
; 0-2 for the run, and afterwards what the 32-bit program did to them -
; how far it read, what it appended - is copied back, and slots 0-2
; are what they were.  See d32_swapin and d32_swapout.
;=====================================================================
D32S_SLOTS      EQU     0               ; 3 bytes: the slot handle 0-2 was
                                        ; swapped for, FFh = not swapped
D32S_SFT        EQU     4               ; slots 0-2 as they were
D32S_LEN        EQU     D32S_SFT + 3 * SFT_LEN

vdm_d32:
        sub     esp,D32S_LEN            ; the swap's own record, on this
        mov     ebx,esp                 ; stack, so bridges can nest
        call    d32_swapin
        mov     dword ptr ds:[vdm_busy],0

        mov     eax,dword ptr ds:[vdm_cenv]     ; fn4B's parameter block:
        mov     dword ptr ds:[d32_pblk],eax     ; the environment and the
        mov     eax,dword ptr ds:[vdm_ctail]    ; tail, flat - it copies
        mov     dword ptr ds:[d32_pblk+4],eax   ; both before anything else

        push    EFL_IF OR 2             ; F_FLAGS
        push    SEL_CODE32              ; F_CS - the kernel's: flat pointers
        push    OFFSET vdm_d32back      ; F_EIP
        push    SEL_DATA32              ; F_DS
        push    SEL_DATA32              ; F_ES
        push    0                       ; the saved EBP
        push    4B00h                   ; F_EAX
        push    0                       ; F_ECX
        push    OFFSET vdm_cpath        ; F_EDX
        push    OFFSET d32_pblk         ; F_EBX
        push    0                       ; F_ESP
        push    0                       ; F_EBP
        push    0                       ; F_ESI
        push    0                       ; F_EDI
        mov     ebp,esp
        inc     byte ptr ds:[DOS_INDOS] ; as int21_entry does; i21_return
        call    fn4B                    ; takes it off.  A 4Bh that fails
        jmp     i21_return              ; returns here; one that works
                                        ; comes back through the child's 4Ch

;---------------------------------------------------------------------
; vdm_d32back - the 32-bit program's 4Bh is over.  ESP is the swap
;               record again; the flags and AX are the 4Bh's answer.
;---------------------------------------------------------------------
vdm_d32back:
        pushfd
        push    eax
        lea     ebx,[esp+8]
        call    d32_swapout
        mov     dword ptr ds:[vdm_busy],1
        pop     eax
        popfd
        jc      vd32_fail
        movzx   eax,byte ptr [exit_code]        ; 4Dh answers with it
        mov     dword ptr ds:[vdm_exit],eax
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        mov     word ptr [vdm_ctx+V86C_EAX],0
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        add     esp,D32S_LEN
        jmp     vdm_loop
vd32_fail:
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        add     esp,D32S_LEN
        jmp     vdm_loop

;---------------------------------------------------------------------
; d32_swapin - EBX -> a swap record.  The DOS program's handles 0-2,
;              into slots 0-2.
; d32_swapout - the same record: back again.
;
; A slot the DOS program's handle names that is itself one of 0-2 is
; read from the copy taken first, so a program that has swapped its
; own 0 and 1 is not half-copied over itself; the copy back skips it,
; since that slot is live under its own number the whole time.
;---------------------------------------------------------------------
d32_swapin:
        pushad
        mov     dword ptr [ebx+D32S_SLOTS],0FFFFFFFFh
        mov     esi,OFFSET sft_tbl      ; slots 0-2 aside, all three
        lea     edi,[ebx+D32S_SFT]
        mov     ecx,3 * SFT_LEN
        rep     movsb
        call    jft_where               ; -> ESI, ECX: the program's table
        jc      d32i_out
        xor     edx,edx
d32i_one:
        cmp     edx,3
        jae     d32i_out
        cmp     edx,ecx
        jae     d32i_out
        movzx   eax,byte ptr [esi+edx]
        cmp     eax,edx
        je      d32i_next                ; already where a 32-bit program looks
        cmp     eax,MAX_HANDLES
        jae     d32i_next                ; closed: slot i stays as it is
        mov     byte ptr [ebx+D32S_SLOTS+edx],al
        pushad
        cmp     eax,3                   ; the source: one of 0-2, from the
        jb      d32i_saved               ; copy; any other, from the table
        imul    eax,SFT_LEN
        lea     esi,[sft_tbl+eax]
        jmp     short d32i_copy
d32i_saved:
        imul    eax,SFT_LEN
        lea     esi,[ebx+D32S_SFT+eax]
d32i_copy:
        imul    edx,SFT_LEN
        lea     edi,[sft_tbl+edx]
        mov     ecx,SFT_LEN
        rep     movsb
        popad
d32i_next:
        inc     edx
        jmp     short d32i_one
d32i_out:
        popad
        ret

d32_swapout:
        pushad
        xor     edx,edx
d32o_one:
        cmp     edx,3
        jae     d32o_out
        movzx   eax,byte ptr [ebx+D32S_SLOTS+edx]
        cmp     eax,0FFh
        je      d32o_next
        pushad
        cmp     eax,3
        jb      d32o_restore
        mov     esi,edx                 ; what the program did to the file
        imul    esi,SFT_LEN             ; goes back to the DOS program's
        add     esi,OFFSET sft_tbl      ; slot...
        imul    eax,SFT_LEN
        lea     edi,[sft_tbl+eax]
        mov     ecx,SFT_LEN
        rep     movsb
d32o_restore:
        mov     eax,edx                 ; ...and slot i is what it was
        imul    eax,SFT_LEN
        lea     esi,[ebx+D32S_SFT+eax]
        lea     edi,[sft_tbl+eax]
        mov     ecx,SFT_LEN
        rep     movsb
        popad
d32o_next:
        inc     edx
        jmp     short d32o_one
d32o_out:
        popad
        ret

d32_pblk    dd  0, 0

;---------------------------------------------------------------------
; vc_args - everything AL=0 and AL=1 take out of the guest before the
;           load starts: the path, the command tail, the two FCBs and
;           the environment, all of them flattened and copied into the
;           kernel.
;
; COPIED, AND THAT IS THE POINT.  Every one of these is a pointer into
; the guest's own memory, and the loader is about to allocate in that
; memory and read a file into it.  A tail still being read out of the
; place the child has just been loaded over is the kind of bug that
; shows up as one program's arguments appearing inside another.
;---------------------------------------------------------------------
vc_args:
        pushad
        mov     esi,dword ptr [vdm_ctx+V86C_DS]
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        add     esi,eax
        mov     edi,OFFSET vdm_cpath
        mov     ecx,EXEC_PATH_MAX - 1
        call    exec_cpz

        ; The parameter block
        mov     ebx,dword ptr [vdm_ctx+V86C_ES]
        shl     ebx,4
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        add     ebx,eax

        mov     esi,[ebx+EP_TAIL]               ; the tail, seg:off -> flat
        call    vdm_farflat
        mov     dword ptr ds:[vdm_ctail],eax
        mov     esi,[ebx+EP_FCB1]
        call    vdm_farflat
        mov     dword ptr ds:[vdm_cfcb1],eax
        mov     esi,[ebx+EP_FCB2]
        call    vdm_farflat
        mov     dword ptr ds:[vdm_cfcb2],eax

        ; The environment.  Zero means inherit, and what gets inherited
        ; is the STRINGS - vdm_env copies them and appends the child's
        ; own path, so the child gets its own block and its own name in
        ; it, exactly as DOS 3 did.
        movzx   eax,word ptr [ebx+EP_ENV]
        or      eax,eax
        jnz     vca_given
        movzx   eax,word ptr ds:[vdm_envseg]    ; still the parent's
vca_given:
        or      eax,eax
        jz      vca_flat
        shl     eax,4
vca_flat:
        mov     dword ptr ds:[vdm_cenv],eax
        popad
        ret

;=====================================================================
; 4Bh AL=1 - LOAD BUT DO NOT EXECUTE.
;
; The program is loaded exactly as AL=0 loads one - its own block, its
; own PSP, its own environment, its own copy of the command tail - and
; then nothing happens.  The parameter block comes back with the entry
; point (CS:IP) and the initial stack (SS:SP) written into its last
; two fields, and the CALLER decides what to do next.  What a caller
; does next is single-step it, which is what this function is for and
; very nearly all it has ever been used for.
;
; THE CALLER'S CONTEXT IS WHAT HAS TO BE PROTECTED, and it is the only
; hard part.  vdm_load writes the loaded program's registers straight
; into vdm_ctx, which for AL=0 is exactly right - the box is about to
; become that program - and for AL=1 is the caller's live context
; being overwritten by a program that is not going to run.  So the
; whole frame is parked first and put back afterwards, in the shape
; vdm_push and vdm_pop already agreed on, and the entry point is read
; out of vdm_ctx in between.
;
; THE CHILD'S MEMORY IS NOT FREED and no nesting level is used.  There
; is no child as far as the box is concerned: there is a caller, and
; some memory with a program in it that the caller now owns the
; problem of.  Real DOS says the same thing in the same words - the
; documentation's warning that the loaded program's memory is the
; caller's to release is the whole of the contract - and it is why the
; child's PSP is handed back rather than kept.
;=====================================================================
vc_load1:
        call    vc_args                 ; path, tail, FCBs, environment

        mov     esi,OFFSET vdm_ctx      ; park the caller, whole
        mov     edi,OFFSET vdm_lframe
        mov     ecx,V86C_SIZE / 4
        rep     movsd
        mov     edi,OFFSET vdm_lframe
        mov     eax,dword ptr [cur_own]
        mov     [edi+VF_OWN],eax
        mov     eax,dword ptr [dta_ptr]
        mov     [edi+VF_DTA],eax
        mov     ax,word ptr ds:[vdm_pspseg]
        mov     [edi+VF_PSPSEG],ax
        mov     ax,word ptr ds:[vdm_envseg]
        mov     [edi+VF_ENVSEG],ax
        mov     ax,word ptr ds:[vdm_paras]
        mov     [edi+VF_PARAS],ax
        mov     ax,word ptr ds:[vdm_curpsp]
        mov     [edi+VF_CURPSP],ax
        mov     ax,word ptr ds:[vdm_parpsp]
        mov     [edi+VF_PARPSP],ax

        mov     ax,word ptr ds:[vdm_curpsp]     ; the loaded program's
        mov     word ptr ds:[vdm_parpsp],ax     ; parent is whoever asked
        mov     eax,dword ptr ds:[vdm_ctail]
        mov     dword ptr ds:[vdm_tailp],eax
        mov     eax,dword ptr ds:[vdm_cenv]
        mov     dword ptr ds:[vdm_envp],eax
        mov     edx,OFFSET vdm_cpath
        call    vdm_load
        jc      vl1_failed

        ; The two far pointers, out of the context the load just built
        ; and before it is thrown away.
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        shl     eax,16
        mov     ax,word ptr [vdm_ctx+V86C_ESP]
        mov     dword ptr ds:[vl1_ss],eax
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        shl     eax,16
        mov     ax,word ptr [vdm_ctx+V86C_EIP]
        mov     dword ptr ds:[vl1_cs],eax
        movzx   eax,word ptr ds:[vdm_pspseg]
        mov     dword ptr ds:[vl1_psp],eax

        call    vl1_unpark

        ; ...and into the caller's parameter block, which is where it
        ; expects to find them.  The block is re-derived rather than
        ; remembered: the load may have moved memory about, and the
        ; caller's ES:BX is what the caller said.
        mov     ebx,dword ptr [vdm_ctx+V86C_ES]
        shl     ebx,4
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        add     ebx,eax
        mov     eax,dword ptr ds:[vl1_ss]
        mov     dword ptr [ebx+EP_SSSP],eax
        mov     eax,dword ptr ds:[vl1_cs]
        mov     dword ptr [ebx+EP_CSIP],eax

        ; AX = the child's PSP, which DOS leaves there and a debugger
        ; reads to find the program it has just been handed.
        mov     eax,dword ptr ds:[vl1_psp]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

vl1_failed:
        movzx   eax,word ptr ds:[vdm_err]
        mov     dword ptr ds:[vl1_psp],eax
        call    vl1_unpark
        mov     eax,dword ptr ds:[vl1_psp]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;--- put the caller back exactly as it was ---------------------------
vl1_unpark:
        pushad
        mov     esi,OFFSET vdm_lframe
        mov     edi,OFFSET vdm_ctx
        mov     ecx,V86C_SIZE / 4
        rep     movsd
        mov     esi,OFFSET vdm_lframe
        mov     eax,[esi+VF_OWN]
        mov     dword ptr [cur_own],eax
        mov     eax,[esi+VF_DTA]
        mov     dword ptr [dta_ptr],eax
        mov     ax,[esi+VF_PSPSEG]
        mov     word ptr ds:[vdm_pspseg],ax
        mov     ax,[esi+VF_ENVSEG]
        mov     word ptr ds:[vdm_envseg],ax
        mov     ax,[esi+VF_PARAS]
        mov     word ptr ds:[vdm_paras],ax
        mov     ax,[esi+VF_CURPSP]
        mov     word ptr ds:[vdm_curpsp],ax
        mov     ax,[esi+VF_PARPSP]
        mov     word ptr ds:[vdm_parpsp],ax
        popad
        ret

;=====================================================================
; 4Bh AL=3 - LOAD AN OVERLAY.
;
; The smallest of the three and the least like the other two: no
; memory is allocated, no PSP is built, no environment is copied and
; nothing is executed.  The caller has already got the memory - it is
; the caller's own block - and says where in it the file is to go.
; The parameter block is two words: the segment to load at, and the
; relocation factor to add to every fixup.
;
; A .COM overlay is a straight read.  A .EXE one is a read and the
; same fixup walk vl_exe does, with the factor the caller gave instead
; of a load segment the loader chose - which is the entire difference
; between an overlay and a program, and is why the two words are the
; only parameters there are.
;
; A D32 IMAGE CANNOT BE AN OVERLAY and is refused as a bad format.
; The roadmap suspected this function might never be implementable and
; that suspicion was about the wrong caller: a D32 image is linked for
; one absolute address, so it cannot be loaded into somebody else's
; memory at somebody else's address - but a DOS BOX has neither of
; those problems, and an overlay is a sixteen-bit idea from beginning
; to end.  Every program that ever used one was sixteen-bit.
;=====================================================================
vc_overlay:
        mov     esi,dword ptr [vdm_ctx+V86C_DS]
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EDX]
        add     esi,eax
        mov     edi,OFFSET vdm_cpath
        mov     ecx,EXEC_PATH_MAX - 1
        call    exec_cpz

        mov     ebx,dword ptr [vdm_ctx+V86C_ES]
        shl     ebx,4
        movzx   eax,word ptr [vdm_ctx+V86C_EBX]
        add     ebx,eax
        movzx   eax,word ptr [ebx+OVL_SEG]
        mov     dword ptr ds:[vov_seg],eax
        movzx   eax,word ptr [ebx+OVL_RELOC]
        mov     dword ptr ds:[vov_fix],eax

        mov     edx,OFFSET vdm_cpath
        mov     eax,3D00h
        call    k21
        jc      vov_nofile
        mov     dword ptr ds:[vdm_hdl],eax

        mov     ebx,eax
        mov     edx,OFFSET vdm_hdrbuf
        mov     ecx,EXE_HDRLEN
        mov     eax,3F00h
        call    k21
        jc      vov_ioerr
        cmp     eax,EXE_HDRLEN
        jb      vov_flat                        ; too short to be an .EXE
        call    d32_find                        ; an image, bare or behind an
        jnc     vov_badfmt                      ; MS-DOS program: not an overlay
        mov     ax,word ptr ds:[vdm_hdrbuf+EXE_SIG]
        cmp     ax,EXE_MZ
        je      vov_exe
        cmp     ax,EXE_ZM
        je      vov_exe

;--- a flat overlay: the whole file, where it was asked for ----------
vov_flat:
        mov     ebx,dword ptr ds:[vdm_hdl]      ; how much of it there is,
        xor     ecx,ecx                         ; for the check below
        mov     eax,4202h
        call    k21
        jc      vov_ioerr
        cmp     eax,0FFF0h
        jbe     vov_flen
        mov     eax,0FFF0h
vov_flen:
        mov     dword ptr ds:[vdm_imgsz],eax
        mov     ebx,dword ptr ds:[vdm_hdl]
        xor     ecx,ecx
        mov     eax,4200h                       ; unread the sniff
        call    k21
        jc      vov_ioerr
        call    vov_where                       ; somewhere it may go?
        jc      vov_deny
        mov     edx,dword ptr ds:[vov_seg]
        shl     edx,4
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,dword ptr ds:[vdm_imgsz]
        mov     eax,3F00h
        call    k21
        jc      vov_ioerr
        jmp     vov_done

;--- an .EXE overlay: the load module, then its fixups ---------------
vov_exe:
        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_CP]
        shl     eax,9
        movzx   edx,word ptr ds:[vdm_hdrbuf+EXE_CBLP]
        or      edx,edx
        jz      vov_whole
        sub     eax,512
        add     eax,edx
vov_whole:
        movzx   edx,word ptr ds:[vdm_hdrbuf+EXE_CPARHDR]
        shl     edx,4
        mov     dword ptr ds:[vdm_hdrsz],edx
        sub     eax,edx
        jbe     vov_badfmt
        mov     dword ptr ds:[vdm_imgsz],eax
        call    vov_where
        jc      vov_deny

        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,dword ptr ds:[vdm_hdrsz]
        mov     eax,4200h
        call    k21
        jc      vov_ioerr
        mov     edx,dword ptr ds:[vov_seg]
        shl     edx,4
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,dword ptr ds:[vdm_imgsz]
        mov     eax,3F00h
        call    k21
        jc      vov_ioerr

        movzx   eax,word ptr ds:[vdm_hdrbuf+EXE_CRLC]
        mov     dword ptr ds:[vdm_relcnt],eax
        or      eax,eax
        jz      vov_done
        movzx   ecx,word ptr ds:[vdm_hdrbuf+EXE_LFARLC]
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,4200h
        call    k21
        jc      vov_ioerr
vov_batch:
        mov     ecx,dword ptr ds:[vdm_relcnt]
        or      ecx,ecx
        jz      vov_done
        cmp     ecx,VDM_RELBAT
        jbe     vov_some
        mov     ecx,VDM_RELBAT
vov_some:
        mov     dword ptr ds:[vdm_relthis],ecx
        shl     ecx,2
        mov     edx,OFFSET vdm_relbuf
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3F00h
        call    k21
        jc      vov_ioerr
        mov     esi,OFFSET vdm_relbuf
        mov     ecx,dword ptr ds:[vdm_relthis]
vov_one:
        ; THE FIXUP IS FOUND AT THE LOAD SEGMENT AND ADJUSTED BY THE
        ; RELOCATION FACTOR, and those are two different numbers here
        ; where for a program they are one.  An overlay is read into
        ; the caller's memory at one place and may be meant to run
        ; addressed from another; the caller knows which and says so
        ; in the second word of the block.
        movzx   eax,word ptr [esi+2]
        mov     edx,dword ptr ds:[vov_seg]
        add     eax,edx
        shl     eax,4
        movzx   edx,word ptr [esi]
        add     eax,edx
        mov     dx,word ptr ds:[vov_fix]
        add     word ptr [eax],dx
        add     esi,4
        loop    vov_one
        mov     eax,dword ptr ds:[vdm_relthis]
        sub     dword ptr ds:[vdm_relcnt],eax
        jmp     vov_batch

vov_done:
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3E00h
        call    k21
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

vov_ioerr:
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3E00h
        call    k21
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_ACCESS
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop
vov_badfmt:
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3E00h
        call    k21
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_EXEFMT
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop
vov_nofile:
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_NOFILE
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop
vov_deny:
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     eax,3E00h
        call    k21
        mov     word ptr [vdm_ctx+V86C_EAX],ERR_ACCESS
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;---------------------------------------------------------------------
; vov_where - CF if the overlay, [vdm_imgsz] bytes at [vov_seg]:0,
;             would land on the kernel's own memory.
;
; The caller names the address, and nothing looked at it: 4B03h with
; a parameter block of zeroes - which is what EXECTST handed it, from
; the days when it expected 4B03h to be refused - read ARGS.COM over
; the interrupt vector table and on into the thunk's frame and the
; real-mode stack the kernel stands on at 0600h, and the machine went
; round in circles.  Real DOS would have done the first half of that
; too; the second half is PM-DOS's to refuse, the way buf_ok refuses
; it for every other buffer a guest names.
;---------------------------------------------------------------------
vov_where:
        ; BELOW THE ARENA IS NOBODY'S TO LOAD INTO: the vector table,
        ; the BIOS's data, DOS's own, and the kernel's scaffolding.  An
        ; overlay goes into memory its caller owns, and a DOS program owns
        ; nothing under the first MCB.
        cmp     dword ptr ds:[vov_seg],V86_TPA_SEG
        jb      vw_no
        push    eax
        push    ecx
        mov     eax,dword ptr ds:[vov_seg]
        shl     eax,4
        mov     ecx,dword ptr ds:[vdm_imgsz]
        call    buf_ok
        pop     ecx
        pop     eax
        ret
vw_no:
        stc
        ret

;---------------------------------------------------------------------
; vdm_farflat - ESI holds a real-mode far pointer as one dword.  EAX
;               comes back as the flat address, or zero if the pointer
;               was null, which callers treat as "there is none".
;---------------------------------------------------------------------
vdm_farflat:
        or      esi,esi
        jnz     vff_real
        xor     eax,eax
        ret
vff_real:
        mov     eax,esi
        shr     eax,16                          ; the segment
        shl     eax,4
        movzx   esi,si                          ; and the offset
        add     eax,esi
        ret

;---------------------------------------------------------------------
; vdm_cpfcb - copy the sixteen bytes of an FCB from ESI to EDX, unless
;             ESI is zero.  Sixteen and not twelve: the drive byte, the
;             name and extension, and the four DOS zeroes behind them.
;---------------------------------------------------------------------
vdm_cpfcb:
        or      esi,esi
        jz      vcf_none
        push    ecx
        push    edi
        mov     edi,edx
        mov     ecx,4
        rep     movsd
        pop     edi
        pop     ecx
vcf_none:
        ret

;---------------------------------------------------------------------
; 4Dh - how did the last program end?  AL is its exit code and AH says
;       whether it ended, was killed by ^C or a critical error, or went
;       resident.  DOS answers once and then forgets, so a second call
;       reports nothing, and programs depend on that.
;
; It cannot go to the kernel's own 4Dh: that answers with exit_code,
; which is where a 32-bit program's result goes.  The number a guest
; wants is the one its own child left behind.
;---------------------------------------------------------------------
vdm_getcode:
        mov     eax,dword ptr ds:[vdm_exit]
        mov     ah,byte ptr ds:[vdm_extype]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        mov     dword ptr ds:[vdm_exit],0
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

;---------------------------------------------------------------------
; ivt_set - write the real-mode IVT slot for vector AL with EDX, WITH
;           THE NULL GUARD STOOD DOWN ACROSS THE STORE.  Preserves
;           every register.
;
; VECTORS 0-3 ARE THE SIXTEEN BYTES THE NULL GUARD WATCHES, and that
; is not an awkward coincidence to be worked around - it is the shape
; of the problem.  The guard cannot be a page, because page zero is
; load-bearing here (NULLTRAP.INC argues it at length), so it is four
; debug registers on linear 0, 4, 8 and 12; and linear 0 to 15 IS the
; first four entries of the real-mode interrupt vector table.  A DOS
; program hooking INT 0 before it divides - which is what every CPU
; prober does - is therefore INDISTINGUISHABLE AT THE HARDWARE LEVEL
; from a write through a null pointer.
;
; v0.31 half-knew this: v86_run disarms the guard, which covers a
; guest that writes the table with its own MOV.  What it does not
; cover is the guest ASKING DOS to do it.  21h/25h is served by the
; kernel, with the guard armed, so the store landed here and the
; program was killed for something entirely legitimate.  That is what
; happened to QCONFIG.EXE on the first real machine PM-DOS ever ran
; on, and it is the first time the guard fired at all - no emulator
; implements data breakpoints, so eleven days of testing could not
; have found it.
;
; DPMI's 0201h sets a real-mode vector too and had exactly the same
; hole, which is why this is a routine and not two fixes.
;---------------------------------------------------------------------
ivt_set:
        pushad
        movzx   ebx,al
        shl     ebx,2
        xor     al,al
        call    dword ptr ds:[IOS_NULLTRAP]     ; stand down...
        mov     dword ptr ds:[ebx],edx
        mov     al,1
        call    dword ptr ds:[IOS_NULLTRAP]     ; ...and back up
        popad
        ret

;---------------------------------------------------------------------
; 25h and 35h - set and get an interrupt vector.
;
; THESE MUST NOT GO TO THE KERNEL'S OWN HANDLERS, and it would be an
; expensive mistake to let them.  The kernel's 25h installs a 32-bit
; protected-mode IDT gate; a guest calling it would point a gate at a
; real-mode segment:offset pair and the next interrupt of that number
; would jump into nowhere with the machine in ring 0.
;
; What a guest means by a vector is a slot in the real-mode IVT at
; 0:0, which is exactly what the monitor reflects untrapped interrupts
; through - so that is where these two read and write.  Hooking INT
; 24h, 1Bh, 23h or the timer is ordinary behaviour for a DOS program
; and it has to work.
;---------------------------------------------------------------------
vdm_setvec:                                     ; 25h - AL = vector, DS:DX
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        mov     edx,dword ptr [vdm_ctx+V86C_DS]
        shl     edx,16
        mov     dx,word ptr [vdm_ctx+V86C_EDX]
        call    ivt_set
        jmp     vdm_loop

vdm_getvec:                                     ; 35h - AL = vector, -> ES:BX
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        shl     eax,2
        mov     eax,dword ptr ds:[eax]
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        shr     eax,16
        mov     dword ptr [vdm_ctx+V86C_ES],eax
        jmp     vdm_loop

;---------------------------------------------------------------------
; 42h - seek.  CX:DX is ONE 32-bit offset and DX:AX is one 32-bit
;              answer, so neither end fits the generic path: that would
;              pass CX alone as the offset and return the low half of
;              the position twice.  A file over 64K would seek to the
;              wrong place and read the wrong bytes, silently.
;---------------------------------------------------------------------
vdm_seek:
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        shl     ecx,16
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        or      ecx,edx
        movzx   ebx,word ptr [vdm_ctx+V86C_EBX]
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]
        call    k21u                    ; the guest's: BX is its handle
        jc      vsk_fail
        mov     eax,dword ptr ds:[k21_eax]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        shr     eax,16
        mov     word ptr [vdm_ctx+V86C_EDX],ax
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop
vsk_fail:
        mov     eax,dword ptr ds:[k21_eax]
        mov     word ptr [vdm_ctx+V86C_EAX],ax
        or      dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jmp     vdm_loop

;=====================================================================
; Ways out.  There are three, and the difference between them is what
; happens to the program's memory.
;
;   4Ch / 00h / INT 20h   it ends and everything goes back
;   31h / INT 27h         it ends and its memory STAYS
;   a fault, ^C, Abort    it is killed and everything goes back
;
; and each of them then asks the same second question: is this the
; outermost program in the box, or a child with a parent waiting?
;=====================================================================
vdm_term:                                       ; 4Ch - AL is the code
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        mov     dword ptr ds:[vdm_exit],eax
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        jmp     short vdm_exitprog

vdm_ret0:                                       ; 00h and INT 20h
        mov     dword ptr ds:[vdm_exit],0
        mov     dword ptr ds:[vdm_extype],VDX_NORMAL
        ; fall through

;---------------------------------------------------------------------
; vdm_exitprog - the program is finished with.  Give its memory and
;                its handles back, then either end the box or let the
;                parent's 4Bh return.
;---------------------------------------------------------------------
vdm_exitprog:
        xor     eax,eax                 ; (ending, not staying)
        call    vdm_rmleave             ; a DPMI client ending from real
        jnc     vep_go                  ; mode: back to the box's loop
        mov     esp,eax
vep_go:
        ; A PROGRAM THAT IS ITS OWN PARENT DOES NOT END.  DOS's rule -
        ; reset_environment in msctrlc.asm - leaves its memory and its
        ; files alone and sends it to its terminate address instead,
        ; and it is how COMMAND.COM survives a ^C or an Abort in one of
        ; its own commands: it makes itself its own parent at start-up
        ; and points PSP:0Ah at the code that puts its prompt back.
        ; Without this, MS-DOS's COMMAND.COM ended its whole box.
        call    vdm_selfparent
        jnc     vdm_loop
        call    vdm_outer
        jbe     vdm_done                        ; the outermost: the box ends
        call    vdm_cleanup
        jmp     short vdm_toparent

;---------------------------------------------------------------------
; 31h and INT 27h - terminate and stay resident.
;
; THE WHOLE POINT IS THE MEMORY THAT DOES NOT COME BACK, and it is the
; one exit that must not run the sweep: not the block, not the
; environment, not the files it left open, and not the extended memory
; it holds.  All of it belongs to code that is still there.
;
; And it really is still there afterwards.  The conventional arena and
; the real-mode IVT are the machine's, not the box's - a TSR loaded in
; one DOS box is still resident, with its vectors still hooked, when
; the next box starts, and the monitor reflects an untrapped interrupt
; straight into it.  That is what makes this worth having rather than
; a way of ending a program slowly.
;
; 31h takes paragraphs in DX; INT 27h takes a byte offset, because it
; is four years older and predates anyone thinking in paragraphs.
;---------------------------------------------------------------------
vdm_keep:                                       ; 31h - AL code, DX paras
        movzx   eax,byte ptr [vdm_ctx+V86C_EAX]
        mov     dword ptr ds:[vdm_exit],eax
        movzx   ebx,word ptr [vdm_ctx+V86C_EDX]
        jmp     short vdm_stay

vdm_keep27:                                     ; INT 27h - DX = bytes
        mov     dword ptr ds:[vdm_exit],0
        movzx   ebx,word ptr [vdm_ctx+V86C_EDX]
        add     ebx,15
        shr     ebx,4

vdm_stay:
        mov     dword ptr ds:[vdm_extype],VDX_RESIDENT
        mov     dword ptr ds:[vdm_resident],1   ; for this level's exit only:
                                                ; vdm_pop clears it again
        ; Never below the PSP itself.  A program asking to keep nothing
        ; is asking for its own PSP to be handed to somebody else while
        ; its interrupt handlers are still pointing into it.
        cmp     ebx,RSP_SIZE / 10h
        jae     vs_size
        mov     ebx,RSP_SIZE / 10h
vs_size:
        movzx   eax,word ptr ds:[vdm_pspseg]
        or      eax,eax
        jz      vs_noblk
        call    lm_resize                       ; shrink to what it keeps;
                                                ; a refusal leaves it whole,
                                                ; which is safe and wasteful
vs_noblk:
        call    vdm_termvecs                    ; DOS puts them back for a
                                                ; TSR too - its handlers are
                                                ; hooked by vector, not by PSP
        mov     eax,1                           ; staying
        call    vdm_rmleave                     ; (see vdm_exitprog)
        jnc     vsk_go
        mov     esp,eax
vsk_go:
        call    vdm_outer
        jbe     vdm_done                        ; the box ends, the program
                                                ; stays in low memory
        ; fall through

;---------------------------------------------------------------------
; vdm_toparent - pop a level and let the waiting 4Bh return.  DOS
;                reports success in the usual way: AX is meaningless
;                and the carry flag is clear.
;---------------------------------------------------------------------
vdm_toparent:
        call    vdm_pop
        mov     word ptr [vdm_ctx+V86C_EAX],0
        and     dword ptr [vdm_ctx+V86C_FLAGS],NOT FL_CF
        jmp     vdm_loop

;---------------------------------------------------------------------
; vdm_rmleave - the running program is ending (EAX = 0) or going
;               resident (EAX = 1), and it may be a DPMI client's.  CF
;               with EAX = the box loop's ESP if the caller is to stand
;               there first.
;
; A CLIENT'S PROGRAM ENDING ENDS THE CLIENT, and its 4Ch may come from
; real mode - after a raw switch (0306h), or inside a simulated
; interrupt - as well as from protected mode (dpmi_exit).  GOING
; RESIDENT DOES NOT: Borland's RTM is a DPMI client that ends with 31h
; from real mode and is called back through INT 2Fh, whose handler
; switches into the client again.  A resident client goes when its box
; does (vdm_done).
;
; Either way a simulated interrupt in flight is over: it ran on the
; stack the client's INT 31h came in on, and vdm_done returns from the
; box loop's level, not from inside that.
;---------------------------------------------------------------------
vdm_rmleave:
        cmp     dword ptr ds:[dpmi_active],0
        je      vrl_no
        push    edx
        mov     edx,dword ptr ds:[vdm_depth]
        cmp     edx,dword ptr ds:[dpmi_depth]
        pop     edx
        jne     vrl_no                  ; a program it started, or its parent
        or      eax,eax
        jnz     vrl_stay
        call    dpmi_drop               ; (and dpmi_rmact with it)
        jmp     vrl_esp
vrl_stay:
        cmp     dword ptr ds:[dpmi_rmact],0
        je      vrl_no
        mov     dword ptr ds:[dpmi_rmact],0
vrl_esp:
        mov     eax,dword ptr ds:[vdm_esp]
        stc
        ret
vrl_no:
        clc
        ret
;---------------------------------------------------------------------
; vdm_fltloop - the guest's own handler for an exception is not getting
;               past the instruction that caused it.
;
; The monitor gave up rather than reflect the same fault a thirty-third
; time, and the distinction is worth a line of its own: the box is
; being ended because it had stopped, not because the kernel could not
; cope.  On real hardware this is the machine locking up, and the
; reason nobody ever sees the message is that there is nobody left to
; print it.
;---------------------------------------------------------------------
vdm_fltloop:
        ; The coprocessor's error is exception 16 to the processor and
        ; INT 75h to the program, and it is the program's handler that
        ; is being named.  "Vector 10" would be the video BIOS.
        cmp     ebx,16
        jne     vfl_say
        mov     ebx,FPU_VEC
vfl_say:
        push    ebx
        mov     esi,OFFSET msgVdmLoop
        call    dword ptr ds:[IOS_CONSTR]
        pop     eax
        call    vdm_hex8
        mov     esi,OFFSET msgVdmAt
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        call    vdm_hex4
        mov     al,':'
        call    dword ptr ds:[IOS_CONOUT]
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        call    vdm_hex4
        mov     esi,OFFSET msgVdmCR
        call    dword ptr ds:[IOS_CONSTR]
        jmp     vdm_abend

vdm_fault:
        ; Say WHICH fault and WHERE.  "The guest faulted" on its own
        ; names a symptom and nothing else, and a DOS box has exactly
        ; one way of telling anybody anything.
        ; EBX, NOT EAX.  EAX is the REASON the monitor came back and
        ; V86R_FAULT happens to be 3, so printing it under the heading
        ; "vector" reported every fault in the world as a breakpoint -
        ; and sent a good hour looking for a CCh byte that was never
        ; there.  The vector is the second return value.
        push    ebx
        mov     esi,OFFSET msgVdmFault
        call    dword ptr ds:[IOS_CONSTR]
        pop     eax
        call    vdm_hex8
        mov     esi,OFFSET msgVdmAt
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        call    vdm_hex4
        mov     al,':'
        call    dword ptr ds:[IOS_CONOUT]
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        call    vdm_hex4
        mov     esi,OFFSET msgVdmCR2    ; and where it was reaching, which
        call    dword ptr ds:[IOS_CONSTR]       ; for a page fault is the
        mov     eax,cr2                         ; only useful half
        call    vdm_hex32
        mov     esi,OFFSET msgVdmCR
        call    dword ptr ds:[IOS_CONSTR]
        ; AND A SECOND LINE WITH THE STACK AND THE CODE.  Where the guest
        ; was is one thing; how it got there is the four words at its
        ; stack pointer - a far return address, an interrupt frame - and
        ; what it was doing is the six bytes at CS:IP.  A report from
        ; real hardware is a photograph of the screen, and these are
        ; what the photograph needs to have in it (MSD, 2026-10-08).
        mov     esi,OFFSET msgVdmStk
        call    dword ptr ds:[IOS_CONSTR]
        mov     eax,dword ptr [vdm_ctx+V86C_SS]
        call    vdm_hex4
        mov     al,':'
        call    dword ptr ds:[IOS_CONOUT]
        mov     eax,dword ptr [vdm_ctx+V86C_ESP]
        call    vdm_hex4
        mov     esi,dword ptr [vdm_ctx+V86C_SS]
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_ESP]
        add     esi,eax
        mov     ecx,12
vf_stack:
        push    ecx
        push    esi
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        movzx   eax,word ptr [esi]
        push    esi
        call    vdm_hex4
        pop     esi
        pop     ecx
        add     esi,2
        loop    vf_stack
        mov     esi,OFFSET msgVdmCode
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,dword ptr [vdm_ctx+V86C_CS]
        shl     esi,4
        movzx   eax,word ptr [vdm_ctx+V86C_EIP]
        add     esi,eax
        mov     ecx,6
vf_code:
        push    ecx
        push    esi
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        movzx   eax,byte ptr [esi]
        push    esi
        call    vdm_hex8
        pop     esi
        pop     ecx
        inc     esi
        loop    vf_code
        mov     esi,OFFSET msgVdmCR
        call    dword ptr ds:[IOS_CONSTR]
        ; AND THE REGISTERS, a third line: which call the guest was in
        ; the middle of is AX, and the rest say what it was holding.
        mov     esi,OFFSET msgVdmRegs
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET vf_order
        mov     ecx,10
vf_regs:
        push    ecx
        push    esi
        mov     al,' '
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        push    esi
        mov     al,[esi]
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        push    esi
        mov     al,[esi+1]
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        push    esi
        mov     al,'='
        call    dword ptr ds:[IOS_CONOUT]
        pop     esi
        movzx   eax,byte ptr [esi+2]
        mov     eax,dword ptr [vdm_ctx+eax]
        push    esi
        call    vdm_hex4
        pop     esi
        pop     ecx
        add     esi,3
        loop    vf_regs
        mov     esi,OFFSET msgVdmExc
        call    dword ptr ds:[IOS_CONSTR]
        movzx   eax,byte ptr ds:[irx_busy]
        call    vdm_hex8
        mov     esi,OFFSET msgVdmCR
        call    dword ptr ds:[IOS_CONSTR]
        jmp     vdm_abend               ; near: the hex helpers below sit
                                        ; between here and it

vf_order    db  'ax',V86C_EAX, 'bx',V86C_EBX, 'cx',V86C_ECX, 'dx',V86C_EDX
            db  'si',V86C_ESI, 'di',V86C_EDI, 'bp',V86C_EBP, 'ds',V86C_DS
            db  'es',V86C_ES, 'fl',V86C_FLAGS

;---------------------------------------------------------------------
; vdm_hex4 / vdm_hex8 - EAX in hex, four or two digits.  The kernel has
; no formatted output of its own; every other message it prints is a
; fixed string.
;---------------------------------------------------------------------
vdm_hex32:
        push    eax
        shr     eax,16
        call    vdm_hex4
        pop     eax
        jmp     short vdm_hex4
vdm_hex4:
        push    eax
        shr     eax,8
        call    vdm_hex8
        pop     eax
vdm_hex8:
        push    eax
        shr     al,4
        call    vdm_hex1
        pop     eax
        push    eax
        and     al,0Fh
        call    vdm_hex1
        pop     eax
        ret
vdm_hex1:
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     vdh_out
        add     al,7
vdh_out:
        push    eax
        call    dword ptr ds:[IOS_CONOUT]
        pop     eax
        ret

;=====================================================================
; vdm_ctrlc - a ^C, typed during a DOS call a guest made.  Called by
;             ctrl_break with a box running, after the "^C" is echoed.
;
; DOS's ANSWER, NOT THE KERNEL'S.  DOS abandons the call, calls the
; program's INT 23h with the registers it made the call with, and then
; looks at how the handler came back: an IRET (or RETF 2) means carry
; on, and the call is made again from the start; a RETF with the carry
; set means end the program.  A handler that never comes back - the
; one COMMAND.COM runs at its own prompt, which puts its stack back and
; jumps to the top of its loop - is allowed that too.  This used to
; end the whole box whatever the program had asked for, which is how
; MS-DOS's COMMAND.COM, whose ^C handler is the whole of how it keeps
; its prompt, went down with the first one.
;
; So the call is abandoned the way vdm_abort abandons one - the kernel
; stack goes back to the box's loop - and then the guest is set up to
; make the INT 23h itself: its INT 21h frame is put back on its stack,
; where the stub found it, and an interrupt frame that returns to the
; ^C pad goes on top.  The pad is vdm_ccret.
;
; RETURNS when the ^C cannot be handed over - a critical error or a
; DPMI client or a driver's INIT has the context, or the call was not
; one the box's loop made - and the caller's INT 23h ends the program
; instead, through vdm_killprog.
;=====================================================================
vdm_ctrlc:
        mov     eax,dword ptr ds:[vdm_gframe]
        or      eax,eax
        jz      vcc_no
        cmp     eax,-1
        je      vcc_no                  ; asked for, never made: not a call
        cmp     byte ptr [crit_busy],0
        jne     vcc_no
        cmp     dword ptr [dl_depth],0
        jne     vcc_no
        ; A DPMI CLIENT'S OWN CALLS ARE NOT A GUEST'S, but a program a
        ; client started through 0300h is: it runs in the simulated
        ; interrupt's loop, a level deeper, and its ^C is its own.
        call    vdm_anchor
        jc      vcc_anchor
        cmp     dword ptr ds:[dpmi_rmact],0
        jne     vcc_no
        cmp     dword ptr ds:[dpmi_active],0
        jne     vcc_no
vcc_anchor:
        mov     [vcc_esp],eax
        mov     eax,dword ptr ds:[vdm_gframe]
        cmp     word ptr ds:[dvs_seg],0
        je      vcc_no

        ; The call goes where it stands.  What its JFT bracket aimed the
        ; standard handles at goes back first, from its own frame.
        mov     ebp,eax
        call    jft_post
        cli
        mov     esp,[vcc_esp]           ; the loop the program runs in
        mov     byte ptr ds:[DOS_INDOS],0       ; the box runs with DOS idle
        sti
        mov     dword ptr ds:[vdm_gframe],0

        ; The INT 21h frame, back on the guest's stack...
        mov     eax,dword ptr [vdm_ctx+V86C_FLAGS]
        call    vdm_cpush
        mov     eax,dword ptr [vdm_ctx+V86C_CS]
        call    vdm_cpush
        mov     eax,dword ptr [vdm_ctx+V86C_EIP]
        call    vdm_cpush

        ; ...where the handler's IRET will leave the stack, so the pad
        ; can tell an IRET from a RETF...
        mov     eax,dword ptr ds:[vdm_depth]
        mov     edx,dword ptr [vdm_ctx+V86C_SS]
        mov     dword ptr [vdm_ccss+eax*4],edx
        mov     edx,dword ptr [vdm_ctx+V86C_ESP]
        mov     dword ptr [vdm_ccsp+eax*4],edx

        ; ...and INT 23h, taken from the pad.
        movzx   eax,word ptr ds:[dvs_seg]
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        mov     dword ptr [vdm_ctx+V86C_EIP],DVS_CCPAD
        mov     esi,OFFSET vdm_ctx
        mov     al,23h
        call    dword ptr ds:[IOS_V86INT]
        jmp     vdm_loop
vcc_no:
        ret

;---------------------------------------------------------------------
; vdm_ccret - the guest's INT 23h handler has come back, to the pad.
;
; Level with where it was called from: an IRET, or a RETF 2 - carry on.
; Anywhere else on that stack: a RETF, with the flags it was called
; with still on it, and the carry flag it came back with is the answer.
; That is DOS's own test, word for word (CNTCHAND in msctrlc.asm: any
; SP but the one it saved means "pop those flags").  On another stack
; altogether there is nothing to pop, and it is taken as a carry-on.
;
; CARRYING ON IS THE CALL AGAIN, with the registers the handler left:
; DOS runs whatever function is in AH when it comes back, and a handler
; is allowed to change it.  The frame it goes back through is the one
; vdm_ctrlc put back, so the stub's own path takes it from there.
;---------------------------------------------------------------------
vdm_ccret:
        mov     eax,dword ptr ds:[vdm_depth]
        mov     edx,dword ptr [vdm_ctx+V86C_SS]
        cmp     edx,dword ptr [vdm_ccss+eax*4]
        jne     vcr_again
        mov     edx,dword ptr [vdm_ctx+V86C_ESP]
        cmp     dx,word ptr [vdm_ccsp+eax*4]
        je      vcr_again
        call    vdm_cpop                ; the flags the RETF left
        test    dword ptr [vdm_ctx+V86C_FLAGS],FL_CF
        jz      vcr_again
        mov     eax,VDX_CTRLC
        jmp     vdm_killprog
vcr_again:
        jmp     vdm_s21

;=====================================================================
; vdm_killprog - end the running program, and only it, from wherever
;                the kernel is.  EAX = how it ended: VDX_CTRLC or
;                VDX_CRITERR.
;
; DOS's ^C and Abort end the CURRENT process: its parent's 4Bh returns,
; 4Dh there says why, and the parent decides - COMMAND.COM asks
; whether to end the batch file.  This used to be vdm_abort, which ends
; every program in the box: a ^C in a program COMMAND.COM had started
; took COMMAND.COM with it.  At the top of the box the two are the
; same thing, and the box ends.
;
; The kernel stack goes back to the box's loop, for vdm_abort's reason:
; the call being abandoned is several frames deep, and none of them
; has a sensible value to return.
;=====================================================================
vdm_killprog:
        cmp     byte ptr ds:[irx_busy],0        ; see vdm_abend
        jne     irx_lost
        mov     dword ptr ds:[vdm_abrt],eax
        call    vdm_anchor
        jc      vkp_inner
        cli
        mov     esp,dword ptr ds:[vdm_esp]
        mov     byte ptr ds:[DOS_INDOS],0       ; the box runs idle; vdm_done
        sti                                     ; puts the shell's depth back
        mov     byte ptr [crit_busy],0
        mov     byte ptr ds:[DOS_ERRFLG],0
        mov     dword ptr ds:[vdm_gframe],0
        mov     dword ptr ds:[dpmi_rmact],0     ; a simulated interrupt in
                                                ; flight has gone with the stack
        mov     ax,0100h                        ; the standard handles are
        call    std_set                         ; CMD.EXE's between calls
        cmp     dword ptr ds:[dpmi_active],0
        je      vkp_v86
        call    dpmi_drop                       ; a DPMI client IS its program
vkp_v86:
        mov     dword ptr ds:[vdm_exit],0
        mov     eax,dword ptr ds:[vdm_abrt]
        mov     dword ptr ds:[vdm_extype],eax
        jmp     vdm_exitprog

; A CHILD A SIMULATED INTERRUPT STARTED - a DOS extender's spawn - ends
; into that interrupt's own loop, which goes on to finish the 4Bh the
; extender asked for.  Going to the box's loop instead threw away the
; simulated interrupt and the DPMI client with it, and the box ended.
vkp_inner:
        cli
        mov     esp,eax
        mov     byte ptr ds:[DOS_INDOS],0
        sti
        mov     byte ptr [crit_busy],0
        mov     byte ptr ds:[DOS_ERRFLG],0
        mov     dword ptr ds:[vdm_gframe],0
        mov     ax,0100h
        call    std_set
        jmp     short vkp_v86

;---------------------------------------------------------------------
; vdm_anchor - EAX = the ESP of the loop the running program is in.
;              That is the box's (vdm_esp) - unless a simulated
;              interrupt is in flight and the program is deeper than the
;              level it was made from, in which case the program was
;              started inside it and runs in its loop (dpmi_rmesp), and
;              CF is set to say so.
;---------------------------------------------------------------------
vdm_anchor:
        mov     eax,dword ptr ds:[vdm_esp]
        cmp     dword ptr ds:[dpmi_rmact],0
        je      van_box
        push    edx
        mov     edx,dword ptr ds:[vdm_depth]
        cmp     edx,dword ptr ds:[dpmi_rmdepth]
        pop     edx
        jbe     van_box
        mov     eax,dword ptr ds:[dpmi_rmesp]
        stc
        ret
van_box:
        clc
        ret

vcc_esp     dd  0

;---------------------------------------------------------------------
; vdm_outer - flags as CMP would set them for "vdm_depth against the
;             level this run's outermost program is on": BE means that
;             program is the one ending, and the run ends with it.  The
;             level is vdm_base + 1, and vdm_base is 0 unless a box is
;             running on top of another (vdm_exec).  Preserves EAX.
;---------------------------------------------------------------------
vdm_outer:
        push    eax
        mov     eax,dword ptr ds:[vdm_base]
        inc     eax
        cmp     dword ptr ds:[vdm_depth],eax
        pop     eax
        ret

;---------------------------------------------------------------------
; vdm_termvecs - INT 22h, 23h and 24h back to what the ending program's
;                PSP saved.  Everything preserved.
;---------------------------------------------------------------------
vdm_termvecs:
        pushad
        movzx   esi,word ptr ds:[vdm_pspseg]
        or      esi,esi
        jz      vtv_out
        shl     esi,4
        mov     edx,[esi+RSP_TERM]
        mov     al,22h
        call    ivt_set
        mov     edx,[esi+RSP_CTRLC]
        mov     al,23h
        call    ivt_set
        mov     edx,[esi+RSP_CRITERR]
        mov     al,24h
        call    ivt_set
vtv_out:
        popad
        ret

;---------------------------------------------------------------------
; vdm_selfparent - CF clear if the program ending is its own parent,
;                  in which case it has been sent to its terminate
;                  address with its vectors put back, and nothing else
;                  about it has changed.  See vdm_exitprog.
;---------------------------------------------------------------------
vdm_selfparent:
        push    eax
        push    esi
        movzx   esi,word ptr ds:[vdm_pspseg]
        or      esi,esi
        jz      vsp_no
        mov     eax,esi
        shl     esi,4
        cmp     word ptr [esi+RSP_PARENT],ax
        jne     vsp_no
        call    vdm_termvecs
        mov     eax,[esi+RSP_TERM]
        movzx   esi,ax
        mov     dword ptr [vdm_ctx+V86C_EIP],esi
        shr     eax,16
        mov     dword ptr [vdm_ctx+V86C_CS],eax
        mov     dword ptr [vdm_ctx+V86C_FLAGS],EFL_IF
        pop     esi
        pop     eax
        clc
        ret
vsp_no:
        pop     esi
        pop     eax
        stc
        ret

;=====================================================================
; vdm_abort - kill the whole box from outside, wherever it is.
;
; What is left for it is a ring-0 fault taken while a box is running
; (dos_fault, through vdm_abortc).  ^C and Abort used to come here too
; and now go to vdm_killprog, which ends the one program they belong
; to - and ^C goes to the guest's own INT 23h first (vdm_ctrlc), which
; is what "a guest that hooked INT 23h does not get to see the ^C", the
; gap this comment used to document, was asking for.
;
; The mechanism is the same longjmp: the fault arrives as 32-bit code
; running deep inside something, there is no sensible value for any of
; it to return, so the kernel stack goes back to what it was when
; vdm_run started and the ordinary exit path runs from there, closing
; handles and giving memory back exactly as a 4Ch would have.  It is
; the same manoeuvre COMMAND makes with cmd_gohome, and for the same
; reason - the thing being abandoned is not the caller of this code,
; it is the code itself.
;=====================================================================
; vdm_abortc - the same thing for a killer that is not a ^C.  EAX = the
;              VDX_ code 4Dh is to report.  dos_fault uses it: a ring-0
;              fault taken while a box is running ends the box, and the
;              alternative - terminating "the current program" - reloads
;              the interpreter, because a 16-bit EXEC has no depth.
;=====================================================================
vdm_abortc:
        mov     dword ptr ds:[vdm_abrt],eax
        jmp     short vdm_abort0
vdm_abort:
        mov     dword ptr ds:[vdm_abrt],VDX_CTRLC
vdm_abort0:
        cli
        mov     esp,dword ptr ds:[vdm_esp]
        mov     eax,dword ptr ds:[vdm_indos]    ; the depth vdm_run was at
        mov     byte ptr ds:[DOS_INDOS],al
        sti
        mov     dword ptr ds:[vdm_exit],0
        mov     eax,dword ptr ds:[vdm_abrt]
        mov     dword ptr ds:[vdm_extype],eax

        ; ^C KILLS THE WHOLE BOX, not the innermost program.  A guest
        ; three EXECs deep is still one thing the user started and one
        ; thing they are trying to stop, and leaving the parents
        ; running would hand control back to a program that thinks its
        ; child returned normally.  So unwind the lot, innermost first,
        ; which is the order that frees the memory in.
va_level:
        call    vdm_outer
        jbe     vdm_done
        call    vdm_cleanup
        call    vdm_pop
        jmp     short va_level
        ; fall through into vdm_done for the outermost

;=====================================================================
; vdm_done - the last program in the box has finished.  Sweep it up,
;            put the shell's identity back, and return to whoever
;            called vdm_run.
;=====================================================================
vdm_done:
        ; FIRST, before anything that can fail: the VM is over.  The
        ; sweep below flushes the guest's files, a flush can raise a
        ; critical error, and Abort at that prompt comes back here -
        ; which with the flag still set would be a loop rather than an
        ; exit.
        mov     dword ptr ds:[vdm_busy],0

        ; AND NO SIMULATED INTERRUPT IS STILL IN FLIGHT, whatever the
        ; flag says.  dpmi_simint sets it and dpmi_rmdone clears it,
        ; which covers every case where the guest comes back - but a
        ; guest CAN end without coming back: an INT 24h handler that
        ; answers by calling 4Ch, or a ^C during an idle hook.  The
        ; flag would then be left standing and refuse every simulated
        ; interrupt in every box after this one, for the lifetime of
        ; the machine.  The box is over; nothing can still be in it.
        mov     dword ptr ds:[dpmi_rmact],0
        ; AND A DPMI CLIENT THAT WENT RESIDENT (vdm_rmleave) GOES WITH
        ; ITS BOX: there is one descriptor table, and the next box's
        ; client wants it.
        cmp     dword ptr ds:[dpmi_active],0
        je      vd_nodpmi
        call    dpmi_drop
vd_nodpmi:
        mov     dword ptr ds:[vdm_gframe],0     ; nor any call a ^C could find

        ; AND THE STANDARD HANDLES ARE CMD.EXE'S AGAIN.  A guest's
        ; character call aims std_in and std_out at its own 0 and 1 for
        ; as long as the call lasts (JFT.INC) - and a ^C in the middle
        ; of one leaves through here without ever finishing it.
        mov     ax,0100h
        call    std_set

        call    vdm_cleanup
        call    vdm_pop                 ; the frame vdm_exec pushed: cur_own
                                        ; and dta_ptr go back to the 32-bit
                                        ; program that started the box
IFDEF EXIT_TRACE
        cmp     byte ptr ds:[GV_PARK + GVP_XT],0
        je      vd_noxt                 ; the box WIN.COM ran in is over
        XTMARK32 68,054h                ; 'T'
vd_noxt:
ENDIF
        cmp     dword ptr ds:[vdm_depth],0
        jne     vd_outer                ; ...and a box underneath keeps its
        mov     word ptr ds:[vdm_pspseg],0      ; guest's PSP - see vdm_exec
        mov     word ptr ds:[vdm_envseg],0
vd_outer:
        mov     eax,dword ptr ds:[vdm_indos]    ; the shell's 4Bh is still
        mov     byte ptr ds:[DOS_INDOS],al      ; open and about to return
        popad
        ret

;=====================================================================
; vdm_cleanup - give back everything the running program owns.
;
; Called for one level at a time, so it works on the singletons and
; the caller pops afterwards.  A program that went resident owns its
; memory still and is skipped entirely - see vdm_stay.
;=====================================================================
vdm_cleanup:
        pushad
        ; INT 22h, 23h and 24h go back to what the program's PSP saved,
        ; which is what they were when it started - DOS does this on
        ; every way out, a TSR's included.  This did not, so a program
        ; that hooked INT 24h and ended left the vector pointing into
        ; memory that had just been freed, and its parent's next disk
        ; error jumped there.
        call    vdm_termvecs

        ; vdm_resident and NOT vdm_extype, which is a different thing
        ; wearing the same value.  vdm_extype is a REPORT - it is what
        ; the next 4Dh will say - and it survives until somebody reads
        ; it, so a child that went resident leaves VDX_RESIDENT sitting
        ; there while its parent runs on.  Deciding whether to free a
        ; program's memory out of that field means the parent's own
        ; exit skips the sweep and leaks everything it had.
        cmp     dword ptr ds:[vdm_resident],0
        jne     vt_resident

        ; Its mouse handlers, and its cursor if it reset the driver and
        ; left it showing - a TSR's stay, which is why this is below the
        ; test and not above it.
        movzx   eax,word ptr ds:[vdm_pspseg]
        or      eax,MSTOK_BOX
        call    dword ptr ds:[IOS_MSEXIT]

        ; Its handle table first: every entry let go, and a file nobody
        ; else's table names is closed with it (JFT.INC).
        call    jft_exit

        ; Then what the guest left open outside that table - an FCB's
        ; file has no handle - and take back what it kept.  NOT a slot
        ; some other table still names: a child's handle inherited by a
        ; TSR it started belongs to the TSR now, as it did in DOS.
        mov     esi,OFFSET sft_tbl + 3 * SFT_LEN
        mov     ecx,MAX_HANDLES - 3
        mov     edx,[cur_own]
        mov     ebx,3                   ; the slot ESI is
vt_sweep:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      vt_next
        cmp     [esi+SFT_OWNER],edx
        jne     vt_next
        cmp     byte ptr [jft_ref+ebx],0
        jne     vt_next
        push    ebx
        push    edx
        call    sft_close
        pop     edx
        pop     ebx
vt_next:
        add     esi,SFT_LEN
        inc     ebx
        loop    vt_sweep
        call    find_freeown            ; its open 714Eh searches too

        ; The environment goes with it: it was handed to the PSP when
        ; the PSP was built, so one sweep takes both.
        movzx   eax,word ptr ds:[vdm_pspseg]
        or      eax,eax
        jz      vt_nomem
        call    lm_freeown
vt_nomem:

        ; And the extended memory it took through XMS, which lives in
        ; the 32-bit arena and would otherwise be the one kind of
        ; memory a DOS box could leak: fn4C sweeps the arena for a D32
        ; program, and a guest never goes through fn4C.
        mov     eax,[cur_own]
        call    arena_freeown
        call    xms_relown
        call    ems_relown              ; AFTER arena_freeown, which has
                                        ; already taken the pages: this
                                        ; drops the windows and the rows
                                        ; and deliberately frees nothing
vt_resident:
        ; AND WHAT IT WROTE IS ON THE DISK, as fn4C makes sure of for a
        ; D32 program - and a guest never goes through fn4C, so until
        ; this line a DOS program that wrote a file and exited had only
        ; written it into the cache, and a program after it that hung
        ; or was switched off took the file with it.  After the handle
        ; sweep, for fn4C's reason: closing is what writes a file's
        ; length and its directory entry.  A program going resident
        ; comes here too, and is committed as well - it has ended as
        ; far as anybody waiting for its output is concerned.
        call    dos_commit
        popad
        ret

;=====================================================================
; vdm_exec - run a 16-bit program.  This is what 4Bh calls when the
;            file it was pointed at turns out not to be one of ours.
;
;   EDX -> ASCIIZ path
;   ESI -> the command tail, DOS format (length, text, 0Dh), 0 for none
;   EDI -> the environment strings, flat, 0 for none
;
; CF set if it could not be loaded, with the reason in [vdm_err]; the
; guest's exit code is in [vdm_exit] either way.
;
; Note what this DOES NOT do that the 32-bit path has to: no parking,
; no depth counting, no stack switch.  A DOS box lives below 1MB, so
; loading one disturbs nothing above it, and the caller is still
; standing exactly where it was when the guest terminates.  Nesting a
; VM inside a VM is the case that is refused, and it is refused at the
; guest's own 4Bh.
;=====================================================================
vdm_exec:
        cmp     dword ptr ds:[vdm_busy],0
        jne     ve_busy

        ; A BOX MAY BE WAITING UNDERNEATH THIS ONE.  A 32-bit program a
        ; DOS program started (vdm_d32) runs with vdm_busy clear and the
        ; box it came from suspended in its 4Bh - and when THAT program
        ; runs a DOS program, the new one goes on top of the same box, a
        ; level deeper, with the suspended guest's state pushed like any
        ; parent's.  The anchors a run keeps in globals - where to unwind
        ; to, the InDOS depth, and the level its outermost program is on
        ; - are this run's now and the outer run's again afterwards.
        ;
        ; THE PUSH COMES BEFORE THE LOAD, because the load writes the new
        ; program's context and PSP over the live ones.  It used to come
        ; after it, in vdm_run, which was harmless while "the live ones"
        ; were only ever a finished box's leftovers.
        push    dword ptr ds:[vdm_base]
        push    dword ptr ds:[vdm_esp]
        push    dword ptr ds:[vdm_indos]
        mov     eax,dword ptr ds:[vdm_depth]
        mov     dword ptr ds:[vdm_base],eax
        call    vdm_push                        ; the frame whoever is live
        jc      ve_full                         ; goes back to at the end
        mov     dword ptr ds:[vdm_tailp],esi
        mov     dword ptr ds:[vdm_envp],edi
        mov     word ptr ds:[vdm_parpsp],0      ; started by a 32-bit
                                                ; program, which has no PSP
                                                ; a real-mode program could
                                                ; be pointed at
        call    vdm_load
        jc      ve_undo
        call    vdm_run
        pop     dword ptr ds:[vdm_indos]
        pop     dword ptr ds:[vdm_esp]
        pop     dword ptr ds:[vdm_base]
        clc
        ret
ve_undo:
        call    vdm_pop
        jmp     short ve_back
ve_full:
        mov     word ptr ds:[vdm_err],ERR_NOMEM
ve_back:
        pop     dword ptr ds:[vdm_indos]
        pop     dword ptr ds:[vdm_esp]
        pop     dword ptr ds:[vdm_base]
ve_out:
        stc
        ret
ve_busy:
        ; One BOX at a time, which is a different statement from one
        ; program at a time: a guest's own 4Bh nests inside the box it
        ; is already in, and never comes through here.  What this
        ; refuses is a second box started from outside, which would
        ; take over vdm_esp and leave the first one with no way home.
        mov     word ptr ds:[vdm_err],ERR_NOMEM
        stc
        ret

;=====================================================================
; vdm_selftest - run the two guests off the boot disk: a flat .COM and
;                a relocated .EXE.  Both, because they exercise almost
;                disjoint halves of the loader.
;
; NOT called vdm_test: that is the name of the build define, and wasm
; symbols are case-insensitive, so the label collides with it and the
; error you get is "unexpected colon" on the label itself.
;=====================================================================
vdm_selftest:
        pushad
        mov     esi,OFFSET msgVdmStart
        call    dword ptr ds:[IOS_CONSTR]
        mov     edx,OFFSET vdm_hello
        call    vst_one

        mov     esi,OFFSET msgVdmStartX
        call    dword ptr ds:[IOS_CONSTR]
        mov     edx,OFFSET vdm_hellox
        call    vst_one

        mov     esi,OFFSET msgVdmKey
        call    dword ptr ds:[IOS_CONSTR]
        call    dword ptr ds:[IOS_CONIN]
        popad
        ret

vst_one:
        xor     esi,esi                 ; no tail and no environment: this
        xor     edi,edi                 ; runs before there is a shell to
        call    vdm_exec                ; have either
        jc      vst_bad
        mov     esi,OFFSET msgVdmDone
        call    dword ptr ds:[IOS_CONSTR]
        ret
vst_bad:
        mov     esi,OFFSET msgVdmNo
        call    dword ptr ds:[IOS_CONSTR]
        ret

;=====================================================================
; vdm_flags - AL = the function (AH of the guest's call).
;             BL = which of its registers hold segment:offset pointers
;             that have to be flattened before the kernel sees them.
;
; The table below stops at 62h because the kernel's own dispatch table
; does, and for a long time this code indexed it with AH regardless -
; so every call ABOVE 62h read a byte from beyond the end of it and
; used whatever it found as pointer flags.  The families up there are
; not decorative: 71h is the whole long-filename API, 73h is FAT32
; free space, F0h and F1h are the kernel's own.  A stray P_DX in that
; garbage turns a drive number into a megabyte-wide address, and a
; stray zero where a pointer was needed hands the kernel an offset
; with no segment.  It was found by a console test whose cursor call
; came back with the cursor somewhere else entirely.
;
; Those families also cannot be described by AH alone: 71h/47h takes a
; DRIVE in DL and a buffer in DS:SI, while 71h/4Eh takes a path in
; DS:DX - the same AH, opposite meanings.  So they get a second table,
; keyed on the subfunction, and everything not in it converts nothing.
;=====================================================================
; AL = the function (the caller's AH), AH = the subfunction (its AL).
; Both are passed in rather than read out of vdm_ctx, because the DPMI
; bridge asks the same question about a protected-mode client and its
; registers are in an interrupt frame, not in a V86 context.
vdm_flags:
        push    ecx
        push    esi
        mov     cl,ah                           ; the subfunction, kept
        movzx   ebx,al
        ; 0Ch IS THE ONE CALL BELOW THE TABLE'S TOP WHOSE POINTER DEPENDS
        ; ON ITS AL: 0C0Ah flushes the keyboard and then reads a line
        ; into DS:DX, as 0Ah does, and every other 0Ch has no pointer at
        ; all - so a DX full of whatever must not be flattened for them.
        cmp     bl,0Ch
        jne     vpf_ioctl
        xor     bl,bl
        cmp     cl,0Ah
        jne     vpf_out
        mov     bl,P_DX
        jmp     short vpf_out
        ; ...AND 44h IS THE OTHER.  02h, 03h and 0Ch hand a loaded
        ; driver a buffer in DS:DX; 00h and 01h carry a device word in
        ; DX, and the drive calls a drive in BL, neither of them an
        ; address (2026-10-06, when the first three reached a driver).
        ; 0Dh's block is in DS:DX too, with the drive in BL.
vpf_ioctl:
        cmp     bl,44h
        jne     vpf_table
        xor     bl,bl
        cmp     cl,02h
        je      vpf_iodx
        cmp     cl,03h
        je      vpf_iodx
        cmp     cl,0Dh
        je      vpf_iodx
        cmp     cl,0Ch
        jne     vpf_out
vpf_iodx:
        mov     bl,P_DX
        jmp     short vpf_out
vpf_table:
        cmp     bl,I21_MAXFN
        jae     vpf_family
        mov     bl,byte ptr [vdm_ptrtab+ebx]
        jmp     short vpf_out

vpf_family:
        mov     esi,OFFSET vdm_subptr
vpf_scan:
        mov     bl,[esi]
        or      bl,bl
        jz      vpf_none
        cmp     bl,al
        jne     vpf_next
        cmp     byte ptr [esi+1],cl
        je      vpf_hit
vpf_next:
        add     esi,3
        jmp     short vpf_scan
vpf_hit:
        mov     bl,[esi+2]
        jmp     short vpf_out
vpf_none:
        xor     bl,bl
vpf_out:
        pop     esi
        pop     ecx
        ret

;--- AH, AL, and what is a pointer in that particular pair -----------
; The terminator is a zero AH, which is safe: function 0 is below
; I21_MAXFN and never reaches this table.
vdm_subptr:
        db  71h,  39h,  P_DX            ; MkDir
        db  71h,  3Ah,  P_DX            ; RmDir
        db  71h,  3Bh,  P_DX            ; ChDir
        db  71h,  41h,  P_DX            ; Delete
        db  71h,  43h,  P_DX            ; Get/Set attributes
        db  71h,  47h,  P_SI            ; GetCurDir - DL is a DRIVE here,
                                        ; and flattening it would make a
                                        ; drive number out of a segment
        db  71h,  4Eh,  P_DX+P_DI       ; FindFirst: path in, record out
        db  71h,  4Fh,  P_DI            ; FindNext
        db  71h,  56h,  P_DX+P_DI       ; Rename
        db  71h,  60h,  P_SI+P_DI       ; TrueName
        db  71h,  6Ch,  P_SI            ; extended open
        db  71h, 0A0h,  P_DX+P_DI       ; volume information
                                        ; A1h - FindClose - is BX only
        db  73h,  02h,  P_DI            ; get DPB: DL is a DRIVE, as DOS 7
                                        ; has it - the block is the pointer
        db  73h,  03h,  P_DX+P_DI       ; free space, the FAT32 form: this
                                        ; one DOES name its drive by a path
        db  73h,  04h,  P_DI            ; set DPB: a drive in DL again
        db  73h,  05h,  P_BX+P_BXD      ; absolute I/O - and the pointer
                                        ; INSIDE the packet is the one thing
                                        ; here that no table can convert.
                                        ; f7305 does it, and is the only
                                        ; handler in the kernel that has to
                                        ; know what kind of caller it has
        db  0F1h, 03h,  P_DX            ; the interpreter's own path
        db  0F1h, 06h,  P_DI            ; the volume table
        db  0F1h, 08h,  P_DI            ; and one block of the arena
        db  0F1h, 0Bh,  P_DX            ; the program to run parked
        db  0F1h, 0Ch,  P_DI            ; one physical disk's description
        db  0F1h, 0Dh,  P_DI            ; and its raw sectors, read...
        db  0F1h, 0Eh,  P_DI            ; ...and written.  P_DI ALONE: EBX
                                        ; is an LBA in both of those and
                                        ; flattening it would add a slot base
                                        ; to a sector number
        db  0F1h, 0Fh,  P_DX            ; the image file to mount.  NOT BX:
                                        ; BL is a drive letter and BH a flag,
                                        ; which is the same trap as 0Dh's LBA
                                        ; and 71h/47h's drive number - a
                                        ; NUMBER in a register a table might
                                        ; otherwise assume is an address
                                        ; (10h takes BL alone and needs no row)
        db  0F1h, 11h,  P_DI            ; the mount report's buffer
        db  0F1h, 15h,  P_DI            ; and the probe log's.  14h has no
                                        ; row: a drive, a track and a head
                                        ; are all numbers
        db  0F1h, 16h,  P_DI            ; the cache's status record - for
                                        ; CCF_STATUS only; the other three
                                        ; carry numbers, and r3_pflags
                                        ; answers this one by hand so that
                                        ; a stray EDI on one of them is not
                                        ; refused as a bad pointer
        db  0F1h, 17h,  P_DI            ; one CD-ROM drive's record
        db  0F1h, 19h,  P_DX            ; a file to delete and not keep
                                        ; (18h carries nothing)
        db  0F1h, 1Ch,  P_DI            ; a SCSI adapter's, or a device's
        db  0F1h, 21h,  P_SI            ; the registers a BIOS call is given
                                        ; and gives back.  20h's pointer
                                        ; depends on its function, so
                                        ; r3_pflags answers that one by hand
        db  0F0h, 04h,  P_SI           ; cells onto the screen - EBX is a
                                        ; CELL NUMBER, not a pointer
        db  0F0h, 05h,  P_DI            ; and back off it
                                        ; F0h's other calls convert nothing:
                                        ; DX is a row and a column, or the
                                        ; shift state
        db  0

;=====================================================================
; Which register holds a pointer, for each DOS function.
;
; THE FCB ROWS WENT IN WITH THE FCB API and every one of them is a
; DS:DX - an FCB is always addressed that way, which is the one mercy
; of an API designed before anybody had thought about parameter
; conventions.  29h is the exception and could not be otherwise: it
; reads a string through DS:SI and writes the FCB through ES:DI, so it
; is the only call here with two pointers in different segments.
;
; 00h to 6Ah, which is exactly the range of the kernel's own dispatch
; table - vdm_flags bounds-checks against the same constant, and
; anything above it is the business of vdm_subptr.
;
; THE TWO MUST BE EXTENDED TOGETHER.  v0.27 found this table being
; indexed past its end for every call above 62h, which is a read of
; whatever data follows it interpreted as "which register is a
; pointer" - so a guest's INT 21h could have a register flattened that
; was never an address.  The bound is one constant for exactly that
; reason, and a new function at the top of the dispatch table needs a
; row here in the same commit.
;=====================================================================
vdm_ptrtab:
        ;    0     1     2     3     4     5     6     7
        ;    8     9     A     B     C     D     E     F
        db  0,    0,    0,    0,    0,    0,    0,    0       ; 00-07
        db  0,    P_DX, P_DX, 0,    0,    0,    0,    P_DX    ; 08-0F
        db  P_DX, P_DX, P_DX, P_DX, P_DX, P_DX, P_DX, P_DX    ; 10-17
        db  0,    0,    P_DX, 0,    0,    0,    0,    0       ; 18-1F
        db  0,    P_DX, P_DX, P_DX, P_DX, 0,    0,    P_DX    ; 20-27
        db  P_DX, P_SI+P_DI,  0,    0,    0,    0,    0,    0 ; 28-2F
                                                              ; 29h: the text
                                                              ; in DS:SI, the
                                                              ; FCB in ES:DI
        db  0,    0,    0,    0,    0,    0,    0,    0       ; 30-37
        db  0,    P_DX, P_DX, P_DX, P_DX, P_DX, 0,    P_DX    ; 38-3F
        db  P_DX, P_DX, 0,    P_DX, 0,    0,    0,    P_SI    ; 40-47
        db  0,    0,    0,    P_DX+P_BX,  0,    0,    P_DX, 0 ; 48-4F
        db  0,    0,    0,    0,    0,    0,    P_DX+P_DI, 0  ; 50-57
        db  0,    0,    P_DX, P_DX, 0,    0,    0,    0       ; 58-5F
        db  P_SI+P_DI,  0,    0,    0,    0,    0,    0,    0 ; 60-67
        ; 65h is 0 here and handled by vdm_country's neighbour instead:
        ; its subfunctions disagree about which register is a pointer,
        ; which is the one thing a table keyed on AH alone cannot say.
        db  0,    P_DX, 0,    0,    P_SI                      ; 68-6C
                                                              ; 69h: the serial
                                                              ; block, DS:DX
                                                              ; 6Ch: the path,
                                                              ; as 716Ch takes it

; The other half of the invariant I21_MAXFN states.  vdm_flags indexes
; this with the same AH the kernel dispatches on, so a table shorter
; than the constant reads pointer flags out of whatever follows - the
; exact failure the comment above describes, and one that shows up as
; a wrong address rather than as a crash.
IF ($ - vdm_ptrtab) NE I21_MAXFN
        .ERR    <vdm_ptrtab does not cover 00h..I21_MAXFN-1>
ENDIF

;=====================================================================
; Data
;=====================================================================
vdm_p29     db  0                       ; this call was 21h/29h
vdm_inexc   db  0                       ; a driver excursion's INT 21h is
                                        ; being served by the box's handlers:
                                        ; vdm_loop goes back to dl_excur
            db  0,0,0
k21_eax     dd  0
k21_ebx     dd  0
k21_ecx     dd  0
k21_edx     dd  0
k21_esi     dd  0
k21_edi     dd  0
k21_flg     dd  0

vdm_hdl     dd  0
vdm_exit    dd  0                       ; the last program's exit code...
vdm_extype  dd  0                       ; ...and how it ended, for 4Dh
vdm_abrt    dd  0                       ; which of those an abort is to be
vdm_indos   dd  0                       ; the InDOS depth vdm_run started at
vdm_busy    dd  0                       ; a VM is running: ^C, a critical
                                        ; error and 4Ch all mean the guest
vdm_depth   dd  0                       ; programs live in the box, 0 = none
vdm_base    dd  0                       ; the level this run began above: 0,
                                        ; or an outer box's depth - vdm_exec
vdm_resident dd 0                       ; the level now exiting kept its
                                        ; memory - see vdm_cleanup
vdm_esp     dd  0                       ; where vdm_abort unwinds to
vdm_gframe  dd  0                       ; the k21 frame of the guest call the
                                        ; box's loop has open, for vdm_ctrlc:
                                        ; -1 while vdm_do21 asks k21u for it
vdm_ccss    dd  (VDM_MAXNEST + 1) dup(0)    ; where each level's INT 23h
vdm_ccsp    dd  (VDM_MAXNEST + 1) dup(0)    ; handler was called from
dvs_seg     dw  0                       ; the DOS vector stubs - vdm_stubs
            dw  0
vdm_tailp   dd  0                       ; the caller's command tail
vdm_envp    dd  0                       ; and its environment, flat
vdm_pathp   dd  0                       ; the program's path, as given
vdm_envstr  dd  0                       ; bytes of environment strings

; A guest's 4Bh, taken out of its memory before the load can move
; anything.  One set is enough: a load runs to completion before the
; program it loaded can start one of its own.
vdm_ctail   dd  0
vdm_cenv    dd  0
vdm_cfcb1   dd  0
vdm_cfcb2   dd  0

vdm_err     dw  ERR_NOFILE              ; why the last load failed
vdm_fcbax   dw  0                       ; the two FCB codes, guest AX
vdm_pspseg  dw  0
vdm_lolseg  dw  0                       ; the guest's list of lists, below 1MB
vdm_jftmade db  0                       ; vdm_psp built a table - see vl_undo
vdm_curpsp  dw  0                       ; what 51h/62h answer - 50h moves
                                        ; this and not the block we free
vdm_parpsp  dw  0                       ; what goes at PSP+16h
vdm_envseg  dw  0
vdm_paras   dw  0
            dw  0
vdm_cpath   db  EXEC_PATH_MAX dup(0)
vdm_stack   db  (VDM_MAXNEST * VF_LEN) dup(0)

; 4Bh AL=1's parking space: one frame, in the shape vdm_push writes,
; holding the CALLER while the loader writes over the live context
; with a program that is not going to run.  One is enough - the load
; is finished before the caller can start another.
vdm_lframe  db  VF_LEN dup(0)
vl1_ss      dd  0                       ; and what the load turned out to
vl1_cs      dd  0                       ; be, read out before the parking
vl1_psp     dd  0                       ; space is put back

vov_seg     dd  0                       ; 4Bh AL=3: where the overlay goes
vov_fix     dd  0                       ; and what to add to its fixups
vdm_loadseg dw  0                       ; PSP + 10h: what every fixup is
            dw  0                       ; relative to

;--- .EXE loading ----------------------------------------------------
; Fixups are read a batch at a time rather than one at a time: a linker
; can emit thousands of them, and every read goes down through the FAT
; layer, so four bytes a call would make loading a large program take
; visibly longer than it should.
VDM_RELBAT  EQU 128                     ; entries per read - half a K

vdm_hdrsz   dd  0                       ; header size in bytes
vdm_imgsz   dd  0                       ; the load module, in bytes
vdm_imgpar  dd  0                       ; ...and rounded up to paragraphs
vdm_minpar  dd  0                       ; the smallest block it can live in
vdm_relcnt  dd  0                       ; fixups still to do
vdm_relthis dd  0                       ; fixups in the batch in hand
vdm_envpath db  EXEC_PATH_MAX dup(0)    ; the program's own path, as the
                                        ; tail of its environment block
vdm_hdrbuf  db  EXE_HDRLEN dup(0)
            db  4 dup(0)                ; keep the fixup buffer aligned
vdm_relbuf  db  VDM_RELBAT * 4 dup(0)
vdm_pf      db  0
vdm_dssi    db  0                       ; the pointer call answers in SI, not
                                        ; BX - decided before k21 runs
            db  0,0                     ; align the pointer temporaries
vdm_adx     dd  0                       ; the flat arguments, worked out
vdm_asi     dd  0                       ; before any of them is loaded -
vdm_adi     dd  0                       ; each needs DS or ES in a register

vdm_ctx     db  V86C_SIZE dup(0)

; No drive letter: the self-test has to work whichever volume the
; machine booted from.  Hard-coding A: made it fail on a hard-disk
; boot and look like a loader bug when it was a path bug.
vdm_hello   db  '\HELLO.COM',0
vdm_hellox  db  '\HELLOX.EXE',0
msgVdmStart db  'VDM: loading HELLO.COM into a DOS box',13,10,0
msgVdmStartX db 'VDM: loading HELLOX.EXE into a DOS box',13,10,0
msgVdmDone  db  'VDM: the guest terminated normally',13,10,0
msgVdmNo    db  'VDM: could not load it',13,10,0
msgVdmFault db  'VDM: the guest faulted, vector ',0
msgVdmAt    db  ' at ',0
msgVdmLoop  db  'VDM: the guest is stuck on its own handler for vector ',0
msgVdmCR2   db  ' cr2 ',0
msgVdmStk   db  '     ss:sp ',0
msgVdmCode  db  '  code',0
msgVdmRegs  db  '    ',0
msgVdmExc   db  ' excursion=',0
msgVdmCR    db  13,10,0
IFDEF VDM_DEBUG
msgVdmEntry db  'VDM: entry ',0
msgVdmBytes db  ' bytes ',0
ENDIF
msgVdmKey   db  'VDM: press a key to carry on booting',13,10,0
