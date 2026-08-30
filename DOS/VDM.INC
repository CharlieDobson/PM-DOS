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
        mov     eax,edi
        add     eax,RSP_JFT
        mov     [edi+RSP_JFTPTR],eax
        mov     word ptr [edi+RSP_DOSCALL],21CDh
        mov     byte ptr [edi+RSP_DOSCALL+2],0CBh

        mov     ecx,20                          ; every handle closed...
        push    edi
        add     edi,RSP_JFT
        mov     al,0FFh
        rep     stosb
        pop     edi
        mov     word ptr [edi+RSP_JFT],0000h    ; ...except stdin/stdout/
        mov     word ptr [edi+RSP_JFT+2],0201h  ;    stderr/aux/prn
        mov     word ptr [edi+RSP_JFT+4],0403h

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
        call    lm_freeown
        mov     word ptr ds:[vdm_pspseg],0
vl_fail:
        popad
        stc
        ret

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
; 20h-2Fh are DOS's, all of them.  15h is claimed for a handful of
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
        mov     byte ptr [vdm_ctx+V86C_TRAP+4],0FFh     ; 20h-27h
        mov     byte ptr [vdm_ctx+V86C_TRAP+5],0FFh     ; 28h-2Fh

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
; vdm_stubs - give the real-mode IVT somewhere harmless to point.
;
; The monitor reflects an untrapped interrupt through the guest's own
; interrupt vector table at 0:0, and that table is the BIOS's - which
; has never heard of DOS.  Vectors 22h-2Fh belong to DOS, and on a
; machine that has not booted one they hold whatever the BIOS felt
; like leaving: an IRET on the careful BIOSes, nothing at all on the
; rest.  A guest's INT 2Fh would then jump to 0000:0000 and start
; executing the interrupt vector table as code.
;
; So: one IRET byte, in the same paragraph of guest-visible memory the
; XMS callback lives in, and every DOS vector the monitor reflects
; points at it.  A TSR that hooks one of these and chains to what it
; found now finds an IRET rather than a cliff.
;
; ONCE, at boot, and NOT per DOS box.  These vectors are the machine's
; and not the box's - which is exactly what makes a TSR possible - so
; re-planting them when a box starts would quietly unhook every
; resident program the moment the next one ran.
;=====================================================================
vdm_stubs:
        pushad
        movzx   eax,word ptr ds:[xms_cbseg]
        or      eax,eax
        jz      vs_none                         ; no low memory, no stubs
        mov     edi,eax
        shl     edi,4
        mov     byte ptr [edi+VDMCB_IRET],0CFh  ; IRET

        shl     eax,16                          ; the far pointer to it
        mov     ax,VDMCB_IRET
        mov     esi,OFFSET vdm_stublist
vs_one:
        movzx   edx,byte ptr [esi]
        or      dl,dl
        jz      vs_none
        shl     edx,2                           ; the vector's IVT slot
        mov     dword ptr ds:[edx],eax
        inc     esi
        jmp     short vs_one
vs_none:
        popad
        ret

; The DOS vectors the monitor hands to the guest's own table.  21h is
; not here because it is served and never reflected; 25h, 26h and 27h
; likewise.  29h is the fast console write, which DOS provides and
; ANSI.SYS hooks - an IRET loses the character, which is the right
; kind of wrong until there is a device chain to do better.
;
; 67h IS NOT A DOS VECTOR AND IS ON THE LIST ANYWAY.  It belongs to
; expanded memory, and a machine with none leaves it exactly as the
; BIOS did - which on most BIOSes is nothing at all, so a program that
; probes for EMS by CALLING the vector rather than by reading it jumps
; to 0000:0000 and starts executing the interrupt vector table.
; Programs that do this are wrong and they exist.  ems_init overwrites
; the entry with a segment that has a name in it when there is a
; manager to name; until then an IRET is the whole of the right
; answer, and AH comes back unchanged rather than saying "no error".
vdm_stublist    db  22h,23h,24h,28h,29h,2Ah,2Bh,2Ch,2Dh,2Eh,2Fh,67h,0

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

        ; THE ONE FIELD THAT IS A CONTRACT.  The guest's arena starts
        ; at V86_TPA, which is where lm_init put the first header - and
        ; NOT at ARENA_BASE, which is what fn52 publishes to a 32-bit
        ; caller.  Those are two different arenas and this is the whole
        ; reason a guest could not be given the kernel's answer.
        mov     word ptr [edi+GLOL_MCBSEG],V86_TPA_SEG

        ; The structures PM-DOS does not have, said plainly.
        mov     eax,0FFFFFFFFh
        mov     [edi+GLOL_DPB],eax
        mov     [edi+GLOL_BUFFERS],eax
        mov     [edi+GLOL_CDS],eax
        mov     [edi+GLOL_FCB],eax

        ; ...AND ONE THAT IT DOES, because FFFF:FFFF was not an
        ; honest answer for this one - it is a CHAIN HEAD, and a
        ; walker does not read it as "nothing here", it follows it.
        ; One block, terminated the way DOS terminates one, with
        ; MAX_HANDLES entries after it that the zero-fill above has
        ; already made "not open".  See GSFT_ in PMDOS.INC for what
        ; this cost and what it bought.
        mov     dword ptr ds:[GSFT_BASE + GSFT_NEXT],0FFFFFFFFh
        mov     word ptr ds:[GSFT_BASE + GSFT_COUNT],GSFT_ENTRIES
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GSFT_OFF
        mov     [edi+GLOL_SFT],eax

        ; CON points at the real CON header now.  CLOCK$ says FFFF:FFFF
        ; with the rest of what is not here: PM-DOS deliberately has no
        ; clock driver - see dev_tab - and pointing this at some other
        ; device's header would be the exact half-truth that comment
        ; refuses.  Nothing walks the chain from here anyway; walkers
        ; start at NUL, which is what the +22h header is for.
        mov     dword ptr [edi+GLOL_CLOCK],0FFFFFFFFh
        movzx   eax,word ptr ds:[vdm_lolseg]
        shl     eax,16
        mov     ax,GLOL_CON
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
        popad
        ret

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
        call    vdm_push                        ; frame 0: the shell's
        call    vdm_newid

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
        mov     esi,OFFSET vdm_ctx
        call    dword ptr ds:[IOS_V86RUN]
        cmp     eax,V86R_INT
        je      vdm_int
        cmp     eax,V86R_HLT
        je      vdm_halt
        cmp     eax,V86R_FLTLOOP
        je      vdm_fltloop
        jmp     vdm_fault

;---------------------------------------------------------------------
; A HLT, which is either a program that has genuinely stopped or one
; of our own callback bytes being used as a service entry.  The
; address settles it: the callback block is memory we planted and know
; the address of, and no ordinary program executes a HLT at all.
;---------------------------------------------------------------------
; Six things can be at the far end of a HLT now.  Four of them are
; told apart by ADDRESS - the XMS entry, DPMI's mode switch, the pad a
; simulated real-mode interrupt returns to, and one of the real-mode
; callback landing pads - and if it is none of those, the guest's own
; interrupt flag settles the last two: on, and it is a program waiting
; for an interrupt; off, and it is one that has genuinely stopped.
vdm_halt:
        call    xms_iscb
        jnc     vh_xms
        call    dpmi_iscb
        jnc     vh_dpmi
        call    dpmi_isret
        jnc     vh_dpmiret
        call    dpmi_iscall             ; EAX = which callback, if it is one
        jnc     vh_dpmicb

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

;---------------------------------------------------------------------
; vdm_abend - the box ends because something went wrong rather than
;             because a program asked to.  The whole box: a fault at
;             any depth has already lost the machine state its parents
;             were relying on.
;---------------------------------------------------------------------
vdm_abend:
        mov     dword ptr ds:[vdm_exit],0
        mov     dword ptr ds:[vdm_extype],VDX_CRITERR
va_unwind:
        cmp     dword ptr ds:[vdm_depth],1
        jbe     vdm_done
        call    vdm_cleanup
        call    vdm_pop
        jmp     short va_unwind

vdm_int:
        cmp     ebx,20h
        je      vdm_ret0
        cmp     ebx,21h
        je      vdm_dos
        cmp     ebx,27h                         ; terminate and stay resident,
        je      vdm_keep27                      ; the DOS 1 spelling
        cmp     ebx,15h
        je      vdm_i15
        cmp     ebx,2Fh
        je      vdm_i2F
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
; The two vectors PM-DOS claims a PIECE of.  Each handler answers CF
; clear if the subfunction was one of ours and CF set if it was not,
; and "not ours" means exactly that: hand the interrupt to whatever
; the guest has in its own IVT.
;
; Getting this wrong in the obvious direction would be quiet and bad.
; INT 2Fh is the DOS multiplex, hooked by every TSR ever written, and
; INT 15h carries the mouse and the wait services; a monitor that
; claims either vector and swallows what it does not recognise breaks
; all of that without a message.
;---------------------------------------------------------------------
vdm_i15:
        call    xms_int15
        jc      vdm_reflect
        jmp     vdm_loop
vdm_i2F:
        call    xms_int2F
        jnc     vdm_loop
        call    dpmi_int2F              ; 1686h/1687h: is there a host?
        jnc     vdm_loop
        call    win_int2F               ; and the 16xx Windows family,
        jc      vdm_reflect             ; which vetoes ENHANCED mode's
        cmp     byte ptr ds:[win_parkreq],0     ; 1605h and reflects it
        jne     vdm_winpark                     ; anyway - see there
        jmp     vdm_loop

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
; vdm_winpark - Windows in standard mode broadcast 1605h and PM-DOS
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
; THE GUEST IS NOT TOLD, AND CANNOT TELL.  It executed an INT and it
; gets a return from that INT with its registers intact; that the
; return arrives in real mode, on a machine whose DOS is now a
; different and much smaller one, is not a thing an 8086 instruction
; stream can observe.  It observes only what it went looking for: PE
; clear, and a machine it may have.
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
        cmp     al,5Dh
        je      vdm_sda
        cmp     al,1Bh
        je      vdm_dsptr
        cmp     al,1Ch
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
        call    k21u                    ; u for the guest's: see buf_ok
        call    vdm_result
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
vdm_indosptr:
        mov     eax,DOS_INDOS
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
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
        call    k21
        call    vdm_result
        jmp     vdm_loop

; 65h - extended country information, and the clearest case yet of why
; vdm_ptrtab cannot answer for a whole function at once: 01h takes a
; buffer in ES:DI, 21h and 22h take a string in DS:DX, and 20h and 23h
; take a character in DL and no pointer whatever.  One flag byte keyed
; on AH would have to be wrong for at least two of those, and being
; wrong here means either handing the kernel an offset with no segment
; or turning a character into a megabyte-wide address.
vdm_ctryext:
        movzx   eax,word ptr [vdm_ctx+V86C_EAX]
        movzx   ecx,word ptr [vdm_ctx+V86C_ECX]
        movzx   edx,word ptr [vdm_ctx+V86C_EDX]
        movzx   edi,word ptr [vdm_ctx+V86C_EDI]

        cmp     al,01h
        je      vce_esdi
        cmp     al,21h
        je      vce_dsdx
        cmp     al,22h
        je      vce_dsdx
        jmp     short vce_run           ; 20h, 23h and the refused ones

vce_esdi:
        mov     ebx,dword ptr [vdm_ctx+V86C_ES]
        shl     ebx,4
        add     edi,ebx
        jmp     short vce_run
vce_dsdx:
        mov     ebx,dword ptr [vdm_ctx+V86C_DS]
        shl     ebx,4
        add     edx,ebx
vce_run:
        call    k21
        call    vdm_result
        jmp     vdm_loop

; WHICH REGISTER IS DECIDED BEFORE THE CALL AND NOT AFTER.  vdm_result
; writes the ANSWER's AX into the context, so by the time the pointer
; needs splitting there is no function number left to test - reading
; AH there would be reading whatever the call returned.
vdm_dsptr:                                      ; 1Bh, 1Ch - offset in BX
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
        je      vdm_loop                        ; 1Bh/1Ch's bad-drive answer,
                                                ; which is not a carry

        mov     eax,dword ptr ds:[k21_ebx]
        cmp     byte ptr ds:[vdm_dssi],0
        je      vd_split
        mov     eax,dword ptr ds:[k21_esi]
vd_split:
        mov     edx,eax
        shr     edx,4
        and     eax,0Fh
        cmp     byte ptr ds:[vdm_dssi],0
        jne     vd_si
        mov     word ptr [vdm_ctx+V86C_EBX],ax
        jmp     short vd_ds
vd_si:
        mov     word ptr [vdm_ctx+V86C_ESI],ax
vd_ds:
        mov     dword ptr [vdm_ctx+V86C_DS],edx
        jmp     vdm_loop

vdm_lol:
        movzx   eax,word ptr ds:[vdm_lolseg]
        or      eax,eax
        jz      vl_nolol
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
        mov     ecx,dword ptr ds:[files_max]
        cmp     ecx,GSFT_ENTRIES
        jbe     vl_filesok
        mov     ecx,GSFT_ENTRIES
vl_filesok:
        mov     word ptr ds:[GSFT_BASE + GSFT_COUNT],cx
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
        cmp     dword ptr ds:[vdm_depth],VDM_MAXNEST
        jae     vc_deep
        call    vc_args

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
        mov     ax,word ptr ds:[vdm_hdrbuf+EXE_SIG]
        cmp     ax,EXE_MZ
        je      vov_exe
        cmp     ax,EXE_ZM
        je      vov_exe
        cmp     dword ptr ds:[vdm_hdrbuf+D32H_MAGIC],D32_MAGIC
        je      vov_badfmt

;--- a flat overlay: the whole file, where it was asked for ----------
vov_flat:
        mov     ebx,dword ptr ds:[vdm_hdl]
        xor     ecx,ecx
        mov     eax,4200h                       ; unread the sniff
        call    k21
        jc      vov_ioerr
        mov     edx,dword ptr ds:[vov_seg]
        shl     edx,4
        mov     ebx,dword ptr ds:[vdm_hdl]
        mov     ecx,0FFF0h
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
        call    k21
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
        cmp     dword ptr ds:[vdm_depth],1
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
        cmp     dword ptr ds:[vdm_depth],1
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
        jmp     vdm_abend               ; near: the hex helpers below sit
                                        ; between here and it

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
; vdm_abort - kill the guest from outside, wherever it is.
;
; ^C at the keyboard and a critical error answered with Abort both
; arrive as 32-bit code running deep inside a DOS call the guest made:
; ctrl_break and COMMAND's INT 24h handler are several frames above
; the V86 monitor, in the middle of k21, and there is no sensible
; value for that call to return.  So this longjmps: the kernel stack
; goes back to what it was when vdm_run started and the ordinary exit
; path runs from there, closing handles and giving memory back exactly
; as a 4Ch would have.
;
; It is the same manoeuvre COMMAND makes with cmd_gohome, and for the
; same reason - the thing being abandoned is not the caller of this
; code, it is the code itself.
;
; A guest that hooked INT 23h does NOT get to see the ^C.  Real DOS
; would run its handler and carry the interrupted call on if it
; returned; doing that here means running V86 code from inside a
; kernel call the guest is already inside, which the monitor has no
; way to nest.  Documented gap, not an oversight.
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
        cmp     dword ptr ds:[vdm_depth],1
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

        call    vdm_cleanup
        call    vdm_pop                 ; frame 0: cur_own and dta_ptr
                                        ; go back to the 32-bit shell
        mov     word ptr ds:[vdm_pspseg],0
        mov     word ptr ds:[vdm_envseg],0
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
        ; vdm_resident and NOT vdm_extype, which is a different thing
        ; wearing the same value.  vdm_extype is a REPORT - it is what
        ; the next 4Dh will say - and it survives until somebody reads
        ; it, so a child that went resident leaves VDX_RESIDENT sitting
        ; there while its parent runs on.  Deciding whether to free a
        ; program's memory out of that field means the parent's own
        ; exit skips the sweep and leaks everything it had.
        cmp     dword ptr ds:[vdm_resident],0
        jne     vt_resident

        ; Close what the guest left open and take back what it kept.
        mov     esi,OFFSET sft_tbl + 3 * SFT_LEN
        mov     ecx,MAX_HANDLES - 3
        mov     edx,[cur_own]
vt_sweep:
        test    dword ptr [esi+SFT_FLAGS],SFTF_INUSE
        jz      vt_next
        cmp     [esi+SFT_OWNER],edx
        jne     vt_next
        push    edx
        call    sft_close
        pop     edx
vt_next:
        add     esi,SFT_LEN
        loop    vt_sweep

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
        mov     dword ptr ds:[vdm_tailp],esi
        mov     dword ptr ds:[vdm_envp],edi
        mov     word ptr ds:[vdm_parpsp],0      ; started by the 32-bit
                                                ; shell, which has no PSP
                                                ; a real-mode program could
                                                ; be pointed at
        call    vdm_load
        jc      ve_out
        call    vdm_run
        clc
        ret
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
        cmp     bl,I21_MAXFN
        jae     vpf_family
        mov     bl,[vdm_ptrtab+ebx]
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
        db  73h,  02h,  P_DX+P_DI       ; get DPB: drive in, block out
        db  73h,  03h,  P_DX+P_DI       ; free space, the FAT32 form
        db  73h,  04h,  P_DX+P_DI       ; set DPB, the same two pointers
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
                                        ; F0h converts nothing at all: its
                                        ; DX is a row and a column
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
        db  0,    0,    0,    0,    P_SI                      ; 68-6C
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
vdm_resident dd 0                       ; the level now exiting kept its
                                        ; memory - see vdm_cleanup
vdm_esp     dd  0                       ; where vdm_abort unwinds to
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
msgVdmCR    db  13,10,0
IFDEF VDM_DEBUG
msgVdmEntry db  'VDM: entry ',0
msgVdmBytes db  ' bytes ',0
ENDIF
msgVdmKey   db  'VDM: press a key to carry on booting',13,10,0
