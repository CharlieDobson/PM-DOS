;=====================================================================
; CONFIG.INC - CONFIG.SYS processing
;
; Read and acted on by the KERNEL, once, before the command
; interpreter is loaded - which is the only place it can go.  A
; SHELL= line decides which interpreter to load in the first place,
; so nothing that runs after the shell could honour it.  MS-DOS put
; this in IO.SYS for the same reason, and AUTOEXEC.BAT then follows
; naturally: CONFIG.SYS configures the system, the shell starts, and
; the shell runs AUTOEXEC.BAT.
;
; The file is read through our own INT 21h.  Everything in PM-DOS runs
; at ring 0 through one flat code selector, so an INT from kernel code
; builds exactly the same frame a program's would - no stack switch,
; no special case, and no second copy of the file logic.
;
; Every statement a real DOS understands is RECOGNISED, so that a
; CONFIG.SYS carried over from a real install boots without a word of
; complaint; almost all of them then do nothing, because they describe
; machinery PM-DOS does not have.  Only an unrecognised statement is an
; error - which is what makes a typo in a PM-DOS statement visible.
;
; ADDING A STATEMENT:  add a row to cfg_table and write the handler.
; A handler is called with ESI -> the value text (everything after the
; '=', blanks trimmed off both ends, NUL terminated) and may destroy
; EAX/EBX/ECX/EDX/ESI/EDI.  Statements specific to PM-DOS go in the
; same table as the inherited ones - there is no second mechanism.
;
; Included by DOSMAIN.ASM.
;=====================================================================

CFG_BUF_SIZE    EQU     4096            ; a CONFIG.SYS bigger than this
CFG_LINE_MAX    EQU     160             ; is not a configuration file
CFG_KEY_MAX     EQU     16

; The walks over the file, in order.  See cfg_phaseof for which
; statement belongs to which, and why any of them has to.
CFG_PASS_DOS    EQU     0               ; DOS=, so upper memory exists
CFG_PASS_MAIN   EQU     1               ; everything else, in file order
CFG_PASS_INST   EQU     2               ; INSTALL=, after every driver

;=====================================================================
; config_run - find CONFIG.SYS on the boot drive and act on it.
;              Its absence is not an error and never was.
;=====================================================================
config_run:
        mov     al,[cur_drive]
        add     al,'A'
        mov     [cfg_name],al           ; the drive we booted from

        mov     edx,OFFSET cfg_name
        mov     ax,3D00h
        int     21h
        jc      cfg_none
        movzx   ebx,ax

        mov     ecx,CFG_BUF_SIZE
        mov     edx,OFFSET cfg_buf
        mov     ah,3Fh
        int     21h
        pushfd
        push    eax
        mov     ah,3Eh                  ; close before doing anything
        int     21h
        pop     eax
        popfd
        jc      cfg_none
        or      eax,eax
        jz      cfg_none

        mov     esi,OFFSET cfg_buf      ; NUL-terminate what we read
        add     esi,eax
        mov     byte ptr [esi],0

IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgCfgFound
        call    dword ptr ds:[IOS_CONSTR]
ENDIF

;--- walk it TWICE, and the first walk reads one statement -----------
; DOS=UMB decides whether there is any upper memory for a DEVICEHIGH
; on an earlier line to be loaded into, and a CONFIG.SYS is a
; DESCRIPTION OF A SYSTEM rather than a program: nothing in it is
; written expecting to be read top to bottom, and a user who puts
; DOS=UMB at the bottom has not made a mistake.  So the file is read
; through once for DOS=, upper memory is built, and then it is read
; again for everything else.  MS-DOS makes several passes over it for
; exactly this reason.
        mov     dword ptr [cfg_pass],CFG_PASS_DOS
        call    cfg_walk
        call    cfg_upper
        mov     dword ptr [cfg_pass],CFG_PASS_MAIN
        call    cfg_walk
        mov     dword ptr [cfg_pass],CFG_PASS_INST
        call    cfg_walk
cfg_none:
        ret

;=====================================================================
; cfg_upper - act on EMS= and DOS=UMB, in between the two walks.
;
; EMS FIRST, AND THAT ORDER IS THE WHOLE OF THE ARRANGEMENT BETWEEN
; THEM.  Both want 16K pages of C0000h-EFFFFh that nothing decodes and
; both find them with the same probe, so the page frame is reserved
; out of umb_state before umb_init has laid a chain over anything.
; Reversing these two lines would leave the frame inside a UMB, which
; is a page a program may have been given and which this kernel would
; then remap underneath it.
;
; Silent when it works, because a real DOS says nothing here and the
; shipped CONFIG.SYS is meant to boot without a word.  NOT silent when
; it fails: the user asked for it in writing and did not get it, and a
; machine that quietly loads everything low looks exactly like a
; machine where the statement was never read.
;=====================================================================
cfg_upper:
        cmp     dword ptr [ems_wanted],0
        je      cup_umb
        call    ems_init
        jnc     cup_umb
        mov     esi,OFFSET msgNoEms
        call    dword ptr ds:[IOS_CONSTR]
cup_umb:
        cmp     dword ptr [umb_wanted],0
        je      cup_out
        call    umb_init
        jnc     cup_out
        mov     esi,OFFSET msgNoUmb
        call    dword ptr ds:[IOS_CONSTR]
cup_out:
        ret

;=====================================================================
; cfg_walk - one pass over cfg_buf, a line at a time.
;=====================================================================
cfg_walk:
        mov     esi,OFFSET cfg_buf
cfg_line:
        cmp     byte ptr [esi],0
        je      cfg_done

        ; copy one line out, stopping at CR, LF or Ctrl-Z
        mov     edi,OFFSET cfg_line_buf
        xor     ecx,ecx
cfg_lcp:
        mov     al,[esi]
        or      al,al
        jz      cfg_lend
        cmp     al,1Ah
        jz      cfg_done
        cmp     al,13
        je      cfg_lterm
        cmp     al,10
        je      cfg_lterm
        cmp     ecx,CFG_LINE_MAX - 1
        jae     cfg_lskip
        mov     [edi],al
        inc     edi
        inc     ecx
cfg_lskip:
        inc     esi
        jmp     short cfg_lcp
cfg_lterm:
        inc     esi                     ; CR, CRLF and bare LF all end it
        cmp     al,13
        jne     cfg_lend
        cmp     byte ptr [esi],10
        jne     cfg_lend
        inc     esi
cfg_lend:
        mov     byte ptr [edi],0
        push    esi
        call    cfg_do_line
        pop     esi
        jmp     short cfg_line

cfg_done:
        ret

;=====================================================================
; cfg_do_line - act on the one line in cfg_line_buf.
;=====================================================================
cfg_do_line:
        mov     esi,OFFSET cfg_line_buf
        call    cfg_blanks
        cmp     byte ptr [esi],0
        je      cdl_out
        cmp     byte ptr [esi],';'      ; a comment, by convention
        je      cdl_out
        cmp     byte ptr [esi],'['      ; [menu] / [common] / [block]
        je      cdl_out                 ; multi-config: every block is
                                        ; read, none is chosen from

;--- keyword ---------------------------------------------------------
        mov     edi,OFFSET cfg_key
        xor     ecx,ecx
cdl_key:
        mov     al,[esi]
        or      al,al
        jz      cdl_kend
        cmp     al,'='
        je      cdl_kend
        cmp     al,' '
        je      cdl_kend
        cmp     al,9
        je      cdl_kend
        call    cfg_upcase
        cmp     ecx,CFG_KEY_MAX - 1
        jae     cdl_kskip
        mov     [edi],al
        inc     edi
        inc     ecx
cdl_kskip:
        inc     esi
        jmp     short cdl_key
cdl_kend:
        mov     byte ptr [edi],0
        or      ecx,ecx
        jz      cdl_out

;--- value: past the blanks, past one '=', past the blanks again -----
        call    cfg_blanks
        cmp     byte ptr [esi],'='
        jne     cdl_noeq
        inc     esi
        call    cfg_blanks
cdl_noeq:
        mov     [cfg_val],esi
        call    cfg_trim

;--- dispatch --------------------------------------------------------
        mov     ebx,OFFSET cfg_table
cdl_lp:
        mov     edi,[ebx]
        or      edi,edi
        jz      cdl_unknown
        mov     esi,OFFSET cfg_key
cdl_cmp:
        mov     al,[esi]
        cmp     al,[edi]
        jne     cdl_miss
        or      al,al
        jz      cdl_hit
        inc     esi
        inc     edi
        jmp     short cdl_cmp
cdl_miss:
        add     ebx,8
        jmp     short cdl_lp
cdl_hit:
        ; ONE WALK PER PHASE, AND THE HANDLER SAYS WHICH PHASE IT IS
        ; IN.  A statement acted on out of order is a statement acted
        ; on before the thing it needs exists - DOS=UMB before a
        ; DEVICEHIGH could load into it, every DEVICE= before an
        ; INSTALL= is entitled to the drivers.
        call    cfg_phaseof
        cmp     eax,[cfg_pass]
        jne     cdl_out
cdl_go:
IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgCfgLine
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET cfg_line_buf
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
ENDIF
        mov     esi,[cfg_val]
        call    dword ptr [ebx+4]
cdl_out:
        ret

cdl_unknown:
        cmp     dword ptr [cfg_pass],CFG_PASS_MAIN
        jne     cdl_out                 ; complain once, on the main walk
        call    cfg_show_line
        mov     esi,OFFSET msgCfgBadCmd
        call    dword ptr ds:[IOS_CONSTR]
        ret

;---------------------------------------------------------------------
; cfg_phaseof - EBX -> a cfg_table row.  EAX = the walk that row's
;               statement belongs to.
;
; Kept here as a list of exceptions rather than a column in the table,
; because it IS a list of exceptions: two statements have to be early,
; two have to be late, and the other twenty-odd genuinely do not care.
; A fourth phase goes in as two more lines.
;---------------------------------------------------------------------
cfg_phaseof:
        mov     eax,[ebx+4]
        cmp     eax,OFFSET cfg_dos
        je      cph_first
        cmp     eax,OFFSET cfg_ems      ; for the same reason as DOS=: the
        je      cph_first               ; frame is reserved out of upper
                                        ; memory before the chain is laid
                                        ; over what is left, and both of
                                        ; those happen between the walks
        cmp     eax,OFFSET cfg_install
        je      cph_last
        cmp     eax,OFFSET cfg_insthigh
        je      cph_last
        mov     eax,CFG_PASS_MAIN
        ret
cph_first:
        mov     eax,CFG_PASS_DOS
        ret
cph_last:
        mov     eax,CFG_PASS_INST
        ret

;=====================================================================
; Statement handlers.  ESI -> the value text.
;=====================================================================

;---------------------------------------------------------------------
; SHELL=path [arguments]
;
; The one statement with teeth: it decides which interpreter the
; kernel loads and what tail it gets.  Leaving "/P" off is meaningful
; rather than a mistake - it is how you get an interpreter that does
; not run AUTOEXEC.BAT and that EXIT can terminate.
;---------------------------------------------------------------------
cfg_shell:
        mov     edi,OFFSET shell_name
        xor     ecx,ecx
csh_name:
        mov     al,[esi]
        or      al,al
        jz      csh_nend
        cmp     al,' '
        je      csh_nend
        cmp     al,9
        je      csh_nend
        cmp     ecx,SHELLNAME_MAX - 1
        jae     csh_nskip
        mov     [edi],al
        inc     edi
        inc     ecx
csh_nskip:
        inc     esi
        jmp     short csh_name
csh_nend:
        mov     byte ptr [edi],0
        or      ecx,ecx
        jz      cfg_badparm

        ; and the rest of the line becomes its command tail
        call    cfg_blanks
        mov     edi,OFFSET shell_tail
        xor     ecx,ecx
        cmp     byte ptr [esi],0
        je      csh_tend
        mov     byte ptr [edi+1],' '
        inc     ecx
csh_tcp:
        mov     al,[esi]
        or      al,al
        jz      csh_tend
        cmp     ecx,7Eh
        jae     csh_tend
        mov     [edi+ecx+1],al
        inc     ecx
        inc     esi
        jmp     short csh_tcp
csh_tend:
        mov     [edi],cl
        mov     byte ptr [edi+ecx+1],0Dh
        ret

;---------------------------------------------------------------------
; BREAK=ON|OFF - remembered; there is no Ctrl-C handler to consult it
;                yet, so it is a promise rather than a setting.
;---------------------------------------------------------------------
cfg_break:
        mov     edi,OFFSET cfgw_on
        call    cfg_iskw
        jnc     cbk_on
        mov     edi,OFFSET cfgw_off
        call    cfg_iskw
        jc      cfg_badparm
        mov     byte ptr [break_flag],0
        ret
cbk_on:
        mov     byte ptr [break_flag],1
        ret

;---------------------------------------------------------------------
; Accepted, and deliberately inert.
;
; Every statement a real DOS understands is recognised here and does
; nothing, so that a CONFIG.SYS carried over from a real install
; loads without a word of complaint.  They describe machinery PM-DOS
; does not have and mostly does not want: it caches per volume rather
; than in BUFFERS and has no FCB table to size.  They are advisory in
; a real CONFIG.SYS too.
;
; SEVEN STATEMENTS HAVE LEFT THIS LIST and the way they left it is
; worth keeping in view.  DEVICE= and DEVICEHIGH= sat here "for now"
; with a note saying they would not stay; they went in v0.37 and
; v0.38.  DOS= sat here with a note saying that HIGH,UMB described a
; memory model that did not exist - which was true of a flat unpaged
; kernel and stopped being true when the A20 gate needed page tables.
; SET=, INSTALL=, INSTALLHIGH=, FILES= and LASTDRIVE= went in v0.39.
; A statement parked here is a statement waiting for its machinery,
; and the note beside it is what says which.
;
; WHAT IS LEFT, AND WHY EACH ONE STAYS - because "inert" without a
; reason is indistinguishable from "not done yet", and five of these
; will never be done:
;
;   BUFFERS=    PM-DOS caches per volume, not in a global pool.  There
;               is no number here to set.
;   FCBS=       there is no system FCB table to size: an FCB lives in
;               the CALLER's memory and carries its own index - see
;               FCB.INC, which is the whole argument.
;   STACKS=     interrupt stack switching, which this kernel does in
;               hardware through the TSS.
;   DRIVPARM=   overrides a block driver's BPB.  There is something
;               to override as of v0.40, so this is now real work
;               rather than a statement with no subject.
;   COUNTRY=    would need country tables PM-DOS does not carry.  38h
;               and 65h answer for codepage 437 and say so; a second
;               country is a data problem, not a parsing one.
;   NUMLOCK=    needs the lock keys tracked and their lights driven,
;               and KEYBD.INC deliberately tracks neither - see the
;               note on BDAK_CAPS.  It is a keyboard feature wearing a
;               CONFIG.SYS statement's clothes.
;   SWITCHES=   /F skips a startup delay there is none of, /N disables
;               function keys at boot that are not read, /K forces a
;               conventional keyboard that is the only kind here.
;   the [menu] family - multi-config.  Every block is read and none is
;               chosen from, which is deliberate and documented.
;---------------------------------------------------------------------
cfg_ignore:
IFDEF PMDOS_DEBUG
        mov     esi,OFFSET msgCfgInert
        call    dword ptr ds:[IOS_CONSTR]
ENDIF
        ret

;---------------------------------------------------------------------
; REM - a comment
;---------------------------------------------------------------------
cfg_rem:
        ret

;---------------------------------------------------------------------
; SET name=value - a variable in the MASTER ENVIRONMENT.
;
; THE MASTER ENVIRONMENT IS THE KERNEL'S, and until now there was no
; such thing: COMMAND.COM started with an empty block and put COMSPEC
; in it, and nothing the machine had been configured with could reach
; the shell at all.  That is what SET= in CONFIG.SYS is for - PATH and
; TEMP and PROMPT set before AUTOEXEC.BAT runs, and set for every
; program in the machine rather than for one interpreter.
;
; It goes to the shell the way an environment goes to any program:
; copied into its slot behind its PSP with PSP_ENV pointing at it,
; which is exactly what exec_setenv already does for a child of 4Bh.
; The interpreter reads it there and seeds its own block from it.  A
; second mechanism would have been a second thing to get wrong.
;
; "SET NAME=" with nothing after the equals DELETES the variable, here
; as everywhere else, which is why this is not simply an append.
;---------------------------------------------------------------------
cfg_set:
        call    menv_set
        jc      cfg_badparm
        ret

;---------------------------------------------------------------------
; menv_set - ESI -> "NAME=value", ASCIIZ.  Replaces any entry with the
;            same name; an empty value deletes it.  CF if the name is
;            malformed or the block is full.
;
; The name is UPPERCASED on the way in.  DOS stores them that way and
; every lookup in the machine - COMMAND's env_find, a program's own
; walk of the block - compares them raw, so a lower-case name stored
; here would be a variable nobody could find and nobody could unset.
;---------------------------------------------------------------------
menv_set:
        pushad

;--- copy it to the scratch, uppercasing as far as the '=' -----------
        mov     edi,OFFSET menv_tmp
        xor     ecx,ecx                 ; name length, up to and incl '='
        xor     edx,edx                 ; 0 = still in the name
        xor     ebx,ebx                 ; total length
ms_copy:
        mov     al,[esi]
        or      al,al
        jz      ms_copied
        cmp     ebx,MENV_LINE - 2
        jae     ms_bad
        or      edx,edx
        jnz     ms_store
        cmp     al,'='
        je      ms_iseq
        call    cfg_upcase
        jmp     short ms_store
ms_iseq:
        mov     edx,1                   ; past the equals: value text, as
        lea     ecx,[ebx+1]             ; typed
ms_store:
        mov     [edi],al
        inc     edi
        inc     esi
        inc     ebx
        jmp     short ms_copy
ms_copied:
        mov     byte ptr [edi],0
        or      ecx,ecx
        jz      ms_bad                  ; no '=' at all: not a SET
        cmp     ecx,1
        je      ms_bad                  ; "=value": no name either

;--- take out any entry that already has this name -------------------
        mov     esi,OFFSET master_env
        xor     edx,edx                 ; offset of the entry
ms_find:
        cmp     edx,[master_envlen]
        jae     ms_notthere
        cmp     byte ptr [esi+edx],0
        je      ms_notthere             ; the empty string ends the block
        push    ecx
        push    edx
        mov     edi,OFFSET menv_tmp
        lea     ebx,[esi+edx]
ms_cmp:
        mov     al,[edi]
        cmp     al,[ebx]
        jne     ms_nomatch
        inc     edi
        inc     ebx
        dec     ecx
        jnz     ms_cmp
        pop     edx                     ; matched name and '=' both
        pop     ecx
        jmp     short ms_remove
ms_nomatch:
        pop     edx
        pop     ecx
ms_skip:
        cmp     edx,[master_envlen]
        jae     ms_notthere
        mov     al,[esi+edx]
        inc     edx
        or      al,al
        jnz     ms_skip
        jmp     short ms_find

;--- shift the rest of the block down over it ------------------------
ms_remove:
        push    ecx
        mov     ebx,edx                 ; EBX = where it starts
        mov     edi,edx                 ; EDI = one past its NUL
ms_rlen:
        mov     al,[esi+edi]
        inc     edi
        or      al,al
        jnz     ms_rlen
        mov     ecx,[master_envlen]
        sub     ecx,edi                 ; bytes that follow it
        push    esi
        push    edi
        lea     edx,[esi+ebx]
        add     esi,edi
        mov     edi,edx
        rep     movsb
        pop     edi
        pop     esi
        mov     eax,edi
        sub     eax,ebx                 ; how long the entry was
        sub     [master_envlen],eax
        pop     ecx
        cmp     dword ptr [master_envlen],1
        ja      ms_notthere
        mov     dword ptr [master_envlen],0     ; nothing left: no block
ms_notthere:

;--- "NAME=" with no value has done its whole job by deleting --------
        mov     edi,OFFSET menv_tmp
        cmp     byte ptr [edi+ecx],0
        je      ms_done

;--- append, over the terminator if there is one ---------------------
        mov     edx,[master_envlen]
        or      edx,edx
        jz      ms_at
        dec     edx                     ; write over the empty string
ms_at:
        mov     esi,OFFSET menv_tmp
        mov     edi,OFFSET master_env
        add     edi,edx
ms_app:
        cmp     edx,MENV_MAX - 2
        jae     ms_bad
        mov     al,[esi]
        mov     [edi],al
        inc     esi
        inc     edi
        inc     edx
        or      al,al
        jnz     ms_app
        mov     byte ptr [edi],0        ; and the empty string after it
        inc     edx
        mov     [master_envlen],edx
ms_done:
        popad
        clc
        ret
ms_bad:
        popad
        stc
        ret

;=====================================================================
; cfg_shellenv - hand the master environment to the interpreter.
;                Called from shell_restart, after build_psp.
;
; It goes through exec_env / exec_setenv rather than being copied
; straight into the slot, because that is the one routine that knows
; where a program's environment lives and how PSP_ENV is filled in,
; and there is nothing else running at this point to want the buffer.
; An empty master environment leaves exec_envlen at zero, which is
; exactly the "no environment" case exec_setenv already handles - so a
; CONFIG.SYS with no SET= line leaves the shell exactly as it was.
;=====================================================================
cfg_shellenv:
        pushad
        mov     ecx,[master_envlen]
        mov     [exec_envlen],ecx
        jecxz   cse_out
        mov     esi,OFFSET master_env
        mov     edi,OFFSET exec_env
        rep     movsb
cse_out:
        call    exec_setenv
        popad
        ret

;---------------------------------------------------------------------
; DOS=HIGH|LOW[,UMB|NOUMB] - answered a word at a time.
;
; UMB is the half with teeth.  It builds upper memory - probes what is
; out there, backs the empty pages from the arena, lays an MCB chain
; over the result, and joins that chain onto the conventional one - and
; without it DEVICEHIGH and LOADHIGH mean what they have always meant
; here, which is "load it low and say nothing".  See UMB.INC.
;
; THIS STATEMENT IS READ BEFORE ANY OTHER, whatever line it is on.  A
; CONFIG.SYS that says DEVICEHIGH on line one and DOS=UMB on line ten
; is not unusual and is not wrong - the statements describe a system
; and not a sequence - so config_run walks the file twice and this
; handler is the whole of the first walk.  MS-DOS makes several passes
; for the same reason.
;
; HIGH is accepted and does nothing, and that is not the same kind of
; nothing.  It asks for the kernel to be moved into the HMA, and this
; kernel is 32-bit and lives at 110000h; the 64K at 100000h is the
; guest's, deliberately, and giving it away is what DOS=HIGH would
; mean here.  The answer is no and the statement is not an error.
;---------------------------------------------------------------------
cfg_dos:
cdo_word:
        call    cfg_blanks
        cmp     byte ptr [esi],0
        je      cdo_out
        mov     edi,OFFSET cfgw_umb
        call    cfg_iskw_part
        jnc     cdo_umb
        mov     edi,OFFSET cfgw_noumb
        call    cfg_iskw_part
        jnc     cdo_noumb
        mov     edi,OFFSET cfgw_high
        call    cfg_iskw_part
        jnc     cdo_next
        mov     edi,OFFSET cfgw_low
        call    cfg_iskw_part
        jnc     cdo_next
        jmp     cfg_badparm
cdo_umb:
        mov     dword ptr [umb_wanted],1
        jmp     short cdo_next
cdo_noumb:
        mov     dword ptr [umb_wanted],0
cdo_next:
        call    cfg_blanks
        cmp     byte ptr [esi],','
        jne     cdo_out
        inc     esi
        jmp     short cdo_word
cdo_out:
        ret

;---------------------------------------------------------------------
; cfg_iskw_part - ESI -> text, EDI -> a keyword.  Like cfg_iskw, but
;                 for a word inside a list: it matches a PREFIX and
;                 leaves ESI past it, where cfg_iskw wants the whole
;                 of the value and nothing after it.
;
; "DOS=HIGH,UMB" has to be read a word at a time, and the separator is
; not a terminator to the routine that reads the word.
;---------------------------------------------------------------------
cfg_iskw_part:
        push    eax
        push    ebx
        mov     ebx,esi
ckp_cmp:
        mov     al,[edi]
        or      al,al
        jz      ckp_end
        mov     al,[ebx]
        call    cfg_upcase
        cmp     al,[edi]
        jne     ckp_no
        inc     ebx
        inc     edi
        jmp     short ckp_cmp
ckp_end:
        mov     al,[ebx]                ; and it must end there: UMBFOO is
        or      al,al                   ; not UMB
        jz      ckp_yes
        cmp     al,','
        je      ckp_yes
        cmp     al,' '
        je      ckp_yes
        cmp     al,9
        jne     ckp_no
ckp_yes:
        mov     esi,ebx
        pop     ebx
        pop     eax
        clc
        ret
ckp_no:
        pop     ebx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; FILES=n - how many files the system will have open at once.
;
; IT IS THE SYSTEM FILE TABLE AND NOT A PER-PROGRAM LIMIT, here as in
; real DOS: one table, shared, and a handle is an index into it.  Real
; DOS defaults to eight and everybody writes FILES=30 or FILES=40 to
; get past it; PM-DOS's table is sixty-four entries and the statement
; chooses how much of it to hand out.
;
; A LINE ASKING FOR MORE THAN THERE IS GETS WHAT THERE IS, silently,
; which is what real DOS does with its own ceiling of 255 - and is why
; raising the table to sixty-four mattered more than making the
; statement adjustable.  At thirty-two a FILES=40 line was a promise
; the kernel could not keep and did not say so.  Now the only lines it
; cannot keep are ones no CONFIG.SYS in the world contains.
;
; The low bound is eight, because that is DOS's own floor and three of
; the entries are the standard handles before any program starts.
;---------------------------------------------------------------------
cfg_files:
        call    cfg_num
        jc      cfg_badparm
        cmp     eax,8
        jae     cfi_low
        mov     eax,8
cfi_low:
        cmp     eax,MAX_HANDLES
        jbe     cfi_high
        mov     eax,MAX_HANDLES
cfi_high:
        mov     [files_max],eax
        mov     dword ptr [dos_lol+4],eax       ; and 52h says the same number
        ret

;---------------------------------------------------------------------
; KEYRATE=rate[,delay] - how fast a held-down key repeats.
;
; THE NUMBERS ARE MODE CON'S, not the hardware's.  RATE runs from 1 to
; 32 and counts UPWARDS to faster (32 is thirty characters a second);
; DELAY is 1 to 4 quarter-seconds before the repeating starts.  The
; 8042 wants the rate the other way up in five bits and the delay in
; two, which is a conversion and not a decision, so it happens here
; and the kernel's side of IOS_KBRATE speaks the hardware's units.
;
; It is a CONFIG.SYS statement because the alternative is MODE CON,
; and MODE is a program PM-DOS does not have; it goes with LFN=, DPMI=
; and NULLTRAP= as a machine-wide setting the kernel owns.  Saying
; nothing leaves the rate the BIOS set, which is the right default: a
; statement that is absent should change nothing.
;
; RATE=32 DELAY=1 IS THE BYTE ZERO, and it is legal - it is what "as
; fast as the keyboard goes, after the shortest pause" comes to.  It
; is also the one value DOSBox 0.74 cannot take: that emulator does
; not implement F3h's argument state, reads the zero as a COMMAND
; instead, and stops delivering keystrokes altogether.  Nothing is
; done about it here, because the byte is correct and the emulator is
; not - see the note in DISK\CONFIG.SYS, which says which value to use
; instead when a DOSBox is in the mix.
;---------------------------------------------------------------------
cfg_keyrate:
        call    cfg_num
        jc      cfg_badparm
        or      eax,eax
        jz      cfg_badparm
        cmp     eax,32
        ja      cfg_badparm
        mov     edx,32
        sub     edx,eax                 ; 32 fastest -> 0 fastest

        mov     eax,1                   ; a delay of 250ms unless told
        cmp     byte ptr [esi],','
        jne     ckr_have
        inc     esi
        call    cfg_num
        jc      cfg_badparm
ckr_have:
        or      eax,eax
        jz      cfg_badparm
        cmp     eax,4
        ja      cfg_badparm
        dec     eax
        shl     eax,5
        or      eax,edx
        call    dword ptr ds:[IOS_KBRATE]
        ret

;---------------------------------------------------------------------
; LASTDRIVE=letter - the highest drive letter that can exist.
;
; What a program does with it is size a loop, so the number has to be
; at least the number of drives there really are: a LASTDRIVE below
; the last mounted volume would be a machine hiding one of its own
; disks from anything that asked politely.  Real DOS raises it for the
; same reason.  The ceiling is MAX_DRIVES, which is what the volume
; table was built with at assembly time.
;
; PM-DOS's letters come from scanning partitions rather than from this
; statement, so raising it does not create drives - it says how far a
; program should bother looking, which is all it ever said.
;---------------------------------------------------------------------
cfg_lastdrv:
        mov     al,[esi]
        or      al,al
        jz      cfg_badparm
        call    cfg_upcase
        sub     al,'A' - 1              ; A: is one drive, not none
        jbe     cfg_badparm
        movzx   eax,al
        cmp     eax,MAX_DRIVES
        jbe     cld_ceil
        mov     eax,MAX_DRIVES
cld_ceil:
        movzx   edx,byte ptr ds:[drv_count]
        cmp     eax,edx
        jae     cld_floor
        mov     eax,edx                 ; never fewer than really exist
cld_floor:
        mov     [lastdrive_n],eax
        ret

;---------------------------------------------------------------------
; cfg_num - ESI -> decimal digits.  EAX = the value, CF if there are
;           none or it runs away.  Anything after the digits is
;           ignored, because FILES=30,8 and BUFFERS=30,8 both exist
;           and only the first number was ever ours.
;---------------------------------------------------------------------
cfg_num:
        push    ecx
        push    edx
        xor     eax,eax
        xor     ecx,ecx                 ; digits seen
cn_lp:
        mov     dl,[esi]
        cmp     dl,'0'
        jb      cn_end
        cmp     dl,'9'
        ja      cn_end
        sub     dl,'0'
        imul    eax,10
        cmp     eax,10000
        ja      cn_bad
        movzx   edx,dl
        add     eax,edx
        inc     esi
        inc     ecx
        jmp     short cn_lp
cn_end:
        or      ecx,ecx
        jz      cn_bad
        pop     edx
        pop     ecx
        clc
        ret
cn_bad:
        pop     edx
        pop     ecx
        stc
        ret

;---------------------------------------------------------------------
; INSTALL=path [arguments] - run a program before the shell exists.
;
; What it is FOR is TSRs.  SHARE, FASTOPEN, KEYB, NLSFUNC: programs
; that install themselves and stay, and that a real DOS runs here
; rather than from AUTOEXEC.BAT because they cost less memory when
; nothing else has been loaded yet.  A program that simply runs and
; exits works too and achieves nothing that AUTOEXEC could not.
;
; IT RUNS ON THE THIRD WALK, AFTER EVERY DEVICE= LINE, wherever it
; sits in the file.  That is not tidiness: an INSTALL= program is
; entitled to the drivers, and a real DOS makes the same guarantee by
; running all of these after all of those.  See config_run.
;
; SIXTEEN-BIT PROGRAMS ONLY, and the refusal is not a limitation so
; much as an admission of what a native program is here.  vdm_exec
; sets up a DOS box, runs the guest, and RETURNS - which is what lets
; CONFIG.SYS carry on to the next line - while the native path jumps
; into the program at ring 3 and never comes back to its caller.  A
; native program installed here could not return to finish reading the
; file it was named in, and there is no such thing as a native TSR to
; make it worth solving.
;
; A FAILURE IS A MESSAGE AND NOT A HALT, for the same reason DEVICE=
; gives: a CONFIG.SYS naming a program that has been uninstalled is
; the commonest CONFIG.SYS there is.
;---------------------------------------------------------------------
cfg_insthigh:
        mov     eax,[alloc_strat]
        push    eax
        mov     dword ptr [alloc_strat],40h     ; upper first, then low
        call    cfg_install
        pop     eax
        mov     [alloc_strat],eax
        ret

cfg_install:
        call    cfg_split
        jc      cin_noname

        mov     edx,OFFSET inst_path            ; whose kind of program?
        call    exec_kind
        cmp     eax,EK_16BIT
        jne     cin_native

        mov     edi,OFFSET master_env           ; the environment it gets is
        cmp     dword ptr [master_envlen],0     ; the one SET= built, and it
        jne     cin_env                         ; is the same block the shell
        xor     edi,edi                         ; will get
cin_env:
        mov     edx,OFFSET inst_path
        mov     esi,OFFSET inst_tail
        call    vdm_exec
        jc      cin_failed
        ret

cin_noname:
        call    cfg_show_line
        mov     esi,OFFSET msgInstName
        jmp     short cin_say
cin_native:
        call    cfg_show_line
        mov     esi,OFFSET msgInstNative
        jmp     short cin_say
cin_failed:
        call    cfg_show_line
        mov     esi,OFFSET msgInstFail
cin_say:
        call    dword ptr ds:[IOS_CONSTR]
        ret

;---------------------------------------------------------------------
; cfg_split - ESI -> "path arguments".  Fills inst_path with the path
;             and inst_tail with the rest in DOS command-tail form -
;             a length byte, the text, and a carriage return.
;             CF if there is no path at all.
;
; The tail keeps its leading space, because that is what a program
; parsing its own tail expects to find and what COMMAND.COM builds
; when it starts one.
;---------------------------------------------------------------------
cfg_split:
        push    eax
        push    ecx
        push    esi
        push    edi
        mov     edi,OFFSET inst_path
        xor     ecx,ecx
cs_path:
        mov     al,[esi]
        or      al,al
        jz      cs_pend
        cmp     al,' '
        je      cs_pend
        cmp     al,9
        je      cs_pend
        cmp     ecx,EXEC_PATH_MAX - 1
        jae     cs_pskip
        mov     [edi],al
        inc     edi
        inc     ecx
cs_pskip:
        inc     esi
        jmp     short cs_path
cs_pend:
        mov     byte ptr [edi],0
        or      ecx,ecx
        jz      cs_none

        mov     edi,OFFSET inst_tail
        xor     ecx,ecx
        cmp     byte ptr [esi],0
        je      cs_tend
        mov     byte ptr [edi+1],' '
        inc     ecx
cs_tail:
        mov     al,[esi]
        or      al,al
        jz      cs_tend
        cmp     ecx,7Eh
        jae     cs_tend
        mov     [edi+ecx+1],al
        inc     ecx
        inc     esi
        jmp     short cs_tail
cs_tend:
        mov     [edi],cl
        mov     byte ptr [edi+ecx+1],0Dh
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        clc
        ret
cs_none:
        pop     edi
        pop     esi
        pop     ecx
        pop     eax
        stc
        ret

;---------------------------------------------------------------------
; DEVICE=path [options] - load a character device driver.
;
; DEVICEHIGH= is the same statement with the allocation strategy set
; to "upper first" around it, which is the whole of what loading high
; is - see lm_alloc.  If DOS=UMB never built any upper memory the
; strategy finds none, lm_alloc falls back to conventional, and the
; driver loads low WITHOUT A WORD: the statement asks for a preference
; and not a guarantee, and a real DOS with no upper-memory provider
; gives the same answer for the same reason.
;
; The strategy is put back afterwards whatever happened.  It is a
; setting a program can read and change through 58h, and CONFIG.SYS
; leaving its own value behind in it would be a system that starts up
; differently depending on what was in a file.
;
; A FAILURE IS A MESSAGE AND NOT A HALT.  A CONFIG.SYS naming a driver
; that is not there is the commonest CONFIG.SYS there is - the line
; survives the uninstall of what it loaded - and a DOS that refuses to
; boot over it would be a DOS nobody could get back into.
;---------------------------------------------------------------------
cfg_devhigh:
        mov     eax,[alloc_strat]
        push    eax
        mov     dword ptr [alloc_strat],40h     ; upper first, then low
        call    cfg_device
        pop     eax
        mov     [alloc_strat],eax
        ret

cfg_device:
        call    dl_load
        jnc     cdv_out
        push    eax
        call    cfg_show_line
        pop     eax
        cmp     eax,DLERR_NOUNITS
        je      cdv_block
        cmp     eax,DLERR_NOMEM
        je      cdv_nomem
        cmp     eax,DLERR_FULL
        je      cdv_full
        cmp     eax,DLERR_INIT
        je      cdv_init
        mov     esi,OFFSET msgDevNoFile
        jmp     short cdv_say
cdv_block:
        mov     esi,OFFSET msgDevUnits
        jmp     short cdv_say
cdv_nomem:
        mov     esi,OFFSET msgDevNoMem
        jmp     short cdv_say
cdv_full:
        mov     esi,OFFSET msgDevFull
        jmp     short cdv_say
cdv_init:
        mov     esi,OFFSET msgDevInit
cdv_say:
        call    dword ptr ds:[IOS_CONSTR]
cdv_out:
        ret

;=====================================================================
; PM-DOS's own statements
;=====================================================================

;---------------------------------------------------------------------
; LFN=ON|OFF - long file names.  ON is the default and the statement
; is only needed to turn them off: the 71xxh API then answers
; "unsupported" (AX=7100h, the very answer tools probe for) and the
; classic calls stop matching long names - strict 8.3, as real DOS.
; The escape hatch for a program that cannot cope with names longer
; than the directory entry it expects.
;---------------------------------------------------------------------
cfg_lfn:
        mov     edi,OFFSET cfgw_on
        call    cfg_iskw
        jnc     clf_on
        mov     edi,OFFSET cfgw_off
        call    cfg_iskw
        jc      cfg_badparm
        mov     byte ptr [lfn_on],0
        ret
clf_on:
        mov     byte ptr [lfn_on],1
        ret

;---------------------------------------------------------------------
; EMS=segment | AUTO | NO - expanded memory, and where its page frame
;                           goes.
;
;   EMS=NO      no expanded memory.  Also what an absent statement
;               means, so the line only ever documents the default.
;   EMS=AUTO    on, and the kernel picks the frame.  A bare "EMS="
;               with nothing after it means this too.
;   EMS=D000    on, with the frame at that segment.
;
; A STATEMENT OF ITS OWN RATHER THAN A WORD IN DOS=, and the address
; is why.  DOS= is a list of yes-or-no words about how the machine
; uses memory; "where the page frame goes" is not one of those, and
; hanging a four-digit number off one of the words would have been the
; wrong shape.  Real DOS asks the same question as a parameter on
; DEVICE=EMM386.EXE (FRAME=D000), which is that DOS's way of saying
; the same thing: the manager is a driver there, so its statement is
; DEVICE=.  Here the manager is the kernel, so its statement is EMS=.
;
; AN EXPLICIT SEGMENT OVERRULES THE PROBE, ON PURPOSE.  Choosing the
; frame automatically means writing to an address and seeing whether
; anything stores it, and that measurement can be wrong on hardware
; nobody here has: an adapter with a window and no option ROM in front
; of it looks exactly like shadow RAM, and shadow RAM that is switched
; on over a ROM looks exactly like memory somebody owns.  A user who
; knows their machine can say so, and being overruled is the whole
; point of being able to name an address.  What is NOT overruled is
; the format: the frame is 64K and 16K-aligned because the
; specification says so, and a segment that cannot hold one is a
; mistake rather than an opinion.
;
; The automatic search insists on 64K alignment as well, which is
; convention rather than rule - every EMM ever shipped put its frame
; at C000h, D000h or E000h and there is software that quietly assumes
; it.  An explicit request is held only to the 16K the specification
; asks for, because a user naming C400h has said something deliberate.
;---------------------------------------------------------------------
cfg_ems:
        mov     edi,OFFSET cfgw_no
        call    cfg_iskw
        jnc     cem_off
        mov     edi,OFFSET cfgw_off
        call    cfg_iskw
        jnc     cem_off

        cmp     byte ptr [esi],0        ; "EMS=" and nothing else
        je      cem_auto
        mov     edi,OFFSET cfgw_auto
        call    cfg_iskw
        jnc     cem_auto
        mov     edi,OFFSET cfgw_on
        call    cfg_iskw
        jnc     cem_auto
        mov     edi,OFFSET cfgw_yes
        call    cfg_iskw
        jnc     cem_auto

        call    cfg_hex
        jc      cfg_badparm
        cmp     eax,0C000h              ; it has to be upper memory...
        jb      cem_badseg
        cmp     eax,0E000h              ; ...and 64K of it has to fit
        ja      cem_badseg              ; below the system BIOS
        test    eax,03FFh               ; ...on a 16K boundary
        jnz     cem_badseg
        mov     [ems_wantseg],eax
        mov     dword ptr [ems_wanted],1
        ret

cem_auto:
        mov     dword ptr [ems_wantseg],0
        mov     dword ptr [ems_wanted],1
        ret
cem_off:
        mov     dword ptr [ems_wanted],0
        ret
cem_badseg:
        call    cfg_show_line
        mov     esi,OFFSET msgEmsSeg
        call    dword ptr ds:[IOS_CONSTR]
        ret

;---------------------------------------------------------------------
; cfg_hex - ESI -> text.  EAX = the value, CF if it is not a hex
;           number of one to four digits.
;
; A trailing 'h' is allowed and ignored, because that is how anyone
; who has written assembler spells a segment and "EMS=D000h" should
; not be a syntax error over one character.  Four digits is the limit
; rather than an overflow check: what this reads is a SEGMENT, and a
; fifth digit means the line says something the reader did not mean.
;---------------------------------------------------------------------
cfg_hex:
        push    ebx
        push    ecx
        push    edx
        push    esi
        xor     ebx,ebx                 ; the value so far
        xor     ecx,ecx                 ; digits seen
chx_lp:
        movzx   edx,byte ptr [esi]
        or      dl,dl
        jz      chx_end
        cmp     dl,' '
        je      chx_end
        cmp     dl,9
        je      chx_end
        cmp     dl,'a'
        jb      chx_nolc
        cmp     dl,'z'
        ja      chx_nolc
        sub     dl,20h
chx_nolc:
        cmp     dl,'H'
        jne     chx_digit
        inc     esi                     ; and it has to be the LAST thing
        cmp     byte ptr [esi],0        ; on the line: "D0H00" is not a
        je      chx_end                 ; number with an h in it
        cmp     byte ptr [esi],' '
        je      chx_end
        cmp     byte ptr [esi],9
        je      chx_end
        jmp     short chx_bad
chx_digit:
        cmp     dl,'0'
        jb      chx_bad
        cmp     dl,'9'
        jbe     chx_num
        cmp     dl,'A'
        jb      chx_bad
        cmp     dl,'F'
        ja      chx_bad
        sub     dl,'A' - 10
        jmp     short chx_add
chx_num:
        sub     dl,'0'
chx_add:
        cmp     ecx,4
        jae     chx_bad
        shl     ebx,4
        or      ebx,edx
        inc     ecx
        inc     esi
        jmp     short chx_lp
chx_end:
        or      ecx,ecx
        jz      chx_bad                 ; nothing but an 'h', or nothing
        mov     eax,ebx
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        clc
        ret
chx_bad:
        pop     esi
        pop     edx
        pop     ecx
        pop     ebx
        stc
        ret

;---------------------------------------------------------------------
; DPMI=ON|OFF - the protected-mode interface.
;
; ON is the default.  OFF makes INT 2Fh AX=1687h answer "no host",
; which is the one thing a client checks before it tries anything, so
; a program that can run either way falls back to real mode instead of
; failing - and one that cannot says so itself, which is more use than
; a host that half works.
;
; Worth having as a switch because a DPMI host is a large thing to
; have in the way when the question is whether something else is at
; fault: turning it off puts the machine back to how v0.27 behaved.
;---------------------------------------------------------------------
cfg_dpmi:
        mov     edi,OFFSET cfgw_on
        call    cfg_iskw
        jnc     cdp_on
        mov     edi,OFFSET cfgw_off
        call    cfg_iskw
        jc      cfg_badparm
        mov     byte ptr [dpmi_on],0
        ret
cdp_on:
        mov     byte ptr [dpmi_on],1
        ret

;---------------------------------------------------------------------
; NULLTRAP=ON|OFF - the null-pointer guard.
;
; ON is the default and means the four debug registers watch linear 0
; to 15 for writes, so that the commonest bug in C-shaped code stops
; the program instead of quietly rewriting the first four interrupt
; vectors.  See IO\NULLTRAP.INC, which explains at length why this is
; debug registers and not the guard page it ought to be.
;
; OFF exists for the program that writes low memory on purpose.  There
; is not much such software, but there is some - anything that plants
; its own real-mode vector by hand rather than through 25h - and a
; guard that cannot be turned off is a guard that eventually stops
; somebody using the machine.
;---------------------------------------------------------------------
cfg_nulltrap:
        mov     edi,OFFSET cfgw_on
        call    cfg_iskw
        jnc     cnt_on
        mov     edi,OFFSET cfgw_off
        call    cfg_iskw
        jc      cfg_badparm
        mov     al,0FFh                 ; off, and not to be armed again
        call    dword ptr ds:[IOS_NULLTRAP]
        ret
cnt_on:
        mov     al,1
        call    dword ptr ds:[IOS_NULLTRAP]
        ret

cfg_badparm:
        call    cfg_show_line
        mov     esi,OFFSET msgCfgBadVal
        call    dword ptr ds:[IOS_CONSTR]
        ret

;=====================================================================
; Helpers
;=====================================================================

; cfg_show_line - echo the offending line before complaining about it
cfg_show_line:
        push    esi
        mov     esi,OFFSET cfg_line_buf
        call    dword ptr ds:[IOS_CONSTR]
        mov     esi,OFFSET msgCrLf
        call    dword ptr ds:[IOS_CONSTR]
        pop     esi
        ret

;---------------------------------------------------------------------
; cfg_iskw - CF clear if the word at ESI is the ASCIIZ uppercase
;            keyword at EDI.  Whole word: it must end at a blank, a
;            comma or the end of the value, so PERM does not match
;            PERMANENT.  ESI and EDI come back untouched either way.
;---------------------------------------------------------------------
cfg_iskw:
        push    esi
        push    edi
        push    eax
ck_lp:
        mov     ah,[edi]
        or      ah,ah
        jz      ck_end
        mov     al,[esi]
        call    cfg_upcase
        cmp     al,ah
        jne     ck_no
        inc     esi
        inc     edi
        jmp     short ck_lp
ck_end:
        mov     al,[esi]
        or      al,al
        jz      ck_yes
        cmp     al,' '
        je      ck_yes
        cmp     al,9
        je      ck_yes
        cmp     al,','
        je      ck_yes
ck_no:
        pop     eax
        pop     edi
        pop     esi
        stc
        ret
ck_yes:
        pop     eax
        pop     edi
        pop     esi
        clc
        ret

; cfg_upcase - AL lower -> upper
cfg_upcase:
        cmp     al,'a'
        jb      cu_out
        cmp     al,'z'
        ja      cu_out
        sub     al,20h
cu_out:
        ret

; cfg_blanks - ESI past spaces and tabs
cfg_blanks:
        cmp     byte ptr [esi],' '
        je      cb_step
        cmp     byte ptr [esi],9
        jne     cb_out
cb_step:
        inc     esi
        jmp     short cfg_blanks
cb_out:
        ret

; cfg_trim - cut trailing blanks off the value at [cfg_val]
cfg_trim:
        push    esi
        push    eax
        mov     esi,[cfg_val]
        mov     eax,esi
ct_end:
        cmp     byte ptr [eax],0
        je      ct_back
        inc     eax
        jmp     short ct_end
ct_back:
        cmp     eax,esi
        jbe     ct_out
        cmp     byte ptr [eax-1],' '
        je      ct_cut
        cmp     byte ptr [eax-1],9
        jne     ct_out
ct_cut:
        dec     eax
        mov     byte ptr [eax],0
        jmp     short ct_back
ct_out:
        pop     eax
        pop     esi
        ret

;=====================================================================
; The statement table.  Add PM-DOS's own statements here.
;=====================================================================
cfg_table:
;--- statements PM-DOS acts on ----------------------------------------
        dd      OFFSET cfgn_shell,   OFFSET cfg_shell
        dd      OFFSET cfgn_break,   OFFSET cfg_break

;--- PM-DOS's own -----------------------------------------------------
        dd      OFFSET cfgn_device,  OFFSET cfg_device
        dd      OFFSET cfgn_devhigh, OFFSET cfg_devhigh
        dd      OFFSET cfgn_lfn,     OFFSET cfg_lfn
        dd      OFFSET cfgn_dpmi,    OFFSET cfg_dpmi
        dd      OFFSET cfgn_nulltrp, OFFSET cfg_nulltrap
        dd      OFFSET cfgn_ems,     OFFSET cfg_ems
        dd      OFFSET cfgn_keyrate, OFFSET cfg_keyrate

;--- the inherited set: recognised, and doing nothing ----------------
        dd      OFFSET cfgn_rem,     OFFSET cfg_rem
        dd      OFFSET cfgn_buffers, OFFSET cfg_ignore
        dd      OFFSET cfgn_files,   OFFSET cfg_files
        dd      OFFSET cfgn_lastdrv, OFFSET cfg_lastdrv
        dd      OFFSET cfgn_fcbs,    OFFSET cfg_ignore
        dd      OFFSET cfgn_stacks,  OFFSET cfg_ignore
        dd      OFFSET cfgn_country, OFFSET cfg_ignore
        dd      OFFSET cfgn_switches,OFFSET cfg_ignore
        dd      OFFSET cfgn_numlock, OFFSET cfg_ignore
        dd      OFFSET cfgn_dos,     OFFSET cfg_dos
        dd      OFFSET cfgn_drivparm,OFFSET cfg_ignore
        dd      OFFSET cfgn_install, OFFSET cfg_install
        dd      OFFSET cfgn_insthigh,OFFSET cfg_insthigh
        dd      OFFSET cfgn_set,     OFFSET cfg_set
        dd      OFFSET cfgn_include, OFFSET cfg_ignore
        dd      OFFSET cfgn_menuitem,OFFSET cfg_ignore
        dd      OFFSET cfgn_menudflt,OFFSET cfg_ignore
        dd      OFFSET cfgn_menucolr,OFFSET cfg_ignore
        dd      OFFSET cfgn_submenu, OFFSET cfg_ignore
        dd      OFFSET cfgn_comment, OFFSET cfg_ignore
        dd      OFFSET cfgn_multitrk,OFFSET cfg_ignore
        dd      0, 0

cfgn_shell    db  'SHELL',0
cfgn_break    db  'BREAK',0
cfgn_lfn      db  'LFN',0
cfgn_dpmi     db  'DPMI',0
cfgn_ems      db  'EMS',0
cfgn_keyrate  db  'KEYRATE',0
cfgn_nulltrp  db  'NULLTRAP',0
cfgn_rem      db  'REM',0
cfgn_buffers  db  'BUFFERS',0
cfgn_files    db  'FILES',0
cfgn_lastdrv  db  'LASTDRIVE',0
cfgn_fcbs     db  'FCBS',0
cfgn_stacks   db  'STACKS',0
cfgn_country  db  'COUNTRY',0
cfgn_switches db  'SWITCHES',0
cfgn_numlock  db  'NUMLOCK',0
cfgn_dos      db  'DOS',0
cfgn_drivparm db  'DRIVPARM',0
cfgn_device   db  'DEVICE',0
cfgn_devhigh  db  'DEVICEHIGH',0
cfgn_install  db  'INSTALL',0
cfgn_insthigh db  'INSTALLHIGH',0
cfgn_set      db  'SET',0
cfgn_include  db  'INCLUDE',0
cfgn_menuitem db  'MENUITEM',0
cfgn_menudflt db  'MENUDEFAULT',0
cfgn_menucolr db  'MENUCOLOR',0
cfgn_submenu  db  'SUBMENU',0
cfgn_comment  db  'COMMENT',0
cfgn_multitrk db  'MULTITRACK',0

cfgw_on       db  'ON',0
cfgw_off      db  'OFF',0
cfgw_no       db  'NO',0
cfgw_yes      db  'YES',0
cfgw_auto     db  'AUTO',0
cfgw_umb      db  'UMB',0
cfgw_noumb    db  'NOUMB',0
cfgw_high     db  'HIGH',0
cfgw_low      db  'LOW',0

;=====================================================================
; Data
;=====================================================================
cfg_name    db  'A:\CONFIG.SYS',0       ; drive letter patched at boot
cfg_val     dd  0
cfg_pass    dd  0                       ; which walk over the file this is

MENV_MAX    EQU 1024                    ; the master environment, which is the
MENV_LINE   EQU 160                     ; shell's to start with
master_env  db  MENV_MAX dup(0)
master_envlen dd 0
menv_tmp    db  MENV_LINE dup(0)

inst_path   db  EXEC_PATH_MAX dup(0)    ; what INSTALL= names, and the tail
inst_tail   db  132 dup(0)              ; it is given, in DOS's own form
cfg_tmp     dd  0
break_flag  db  0

msgCrLf         db  13,10,0
msgCfgBadCmd    db  'Unrecognized command in CONFIG.SYS',13,10,0
msgDevNoFile    db  'Bad or missing device driver',13,10,0
msgDevNoMem     db  'Not enough memory for device driver',13,10,0
msgDevUnits     db  'Device driver has no units',13,10,0
msgDevInit      db  'Device driver failed to initialise',13,10,0
msgDevFull      db  'Too many device drivers',13,10,0
msgNoUmb        db  'DOS=UMB: no upper memory is available on this machine',13,10,0
msgNoEms        db  'EMS: no 64K page frame is available on this machine',13,10,0
msgEmsSeg       db  'EMS: the page frame wants a 16K-aligned segment from '
                db  'C000 to E000',13,10,0
msgInstName     db  'INSTALL= names no program',13,10,0
msgInstNative   db  'INSTALL= cannot run a 32-bit program',13,10,0
msgInstFail     db  'Bad or missing INSTALL= program',13,10,0
msgCfgBadVal    db  'Invalid parameter in CONFIG.SYS',13,10,0
IFDEF PMDOS_DEBUG
msgCfgFound     db  'Processing CONFIG.SYS',13,10,0
msgCfgLine      db  'CONFIG: ',0
msgCfgInert     db  '        (recognised, no effect)',13,10,0
ENDIF

cfg_key     db  CFG_KEY_MAX dup(0)
cfg_line_buf db CFG_LINE_MAX dup(0)
cfg_buf     db  CFG_BUF_SIZE + 1 dup(0)
