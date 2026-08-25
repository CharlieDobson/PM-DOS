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

;--- walk it a line at a time ----------------------------------------
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
cfg_none:
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
        call    cfg_show_line
        mov     esi,OFFSET msgCfgBadCmd
        call    dword ptr ds:[IOS_CONSTR]
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
; than in BUFFERS, has no FCBs, and is flat and unpaged, which makes
; DOS=HIGH,UMB a statement about a memory model that does not exist
; here.  They are advisory in a real CONFIG.SYS too.
;
; DEVICE= and DEVICEHIGH= are in this list for now and will not stay:
; they are what the 16-bit VDM will need, and the drivers named on
; those lines are exactly what it will be asked to load.
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
; DEVICE=path [options] - load a character device driver.
;
; DEVICEHIGH= is the same statement here, and that is not a shortcut:
; a real DOS with no upper-memory provider loads a DEVICEHIGH driver
; low and says nothing about it, because the statement asks for a
; preference rather than a guarantee.  PM-DOS has no UMBs to offer
; yet, so it gives the same answer for the same reason.
;
; A FAILURE IS A MESSAGE AND NOT A HALT.  A CONFIG.SYS naming a driver
; that is not there is the commonest CONFIG.SYS there is - the line
; survives the uninstall of what it loaded - and a DOS that refuses to
; boot over it would be a DOS nobody could get back into.
;---------------------------------------------------------------------
cfg_device:
        call    dl_load
        jnc     cdv_out
        push    eax
        call    cfg_show_line
        pop     eax
        cmp     eax,DLERR_BLOCK
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
        mov     esi,OFFSET msgDevBlock
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
        dd      OFFSET cfgn_devhigh, OFFSET cfg_device
        dd      OFFSET cfgn_lfn,     OFFSET cfg_lfn
        dd      OFFSET cfgn_dpmi,    OFFSET cfg_dpmi
        dd      OFFSET cfgn_nulltrp, OFFSET cfg_nulltrap

;--- the inherited set: recognised, and doing nothing ----------------
        dd      OFFSET cfgn_rem,     OFFSET cfg_rem
        dd      OFFSET cfgn_buffers, OFFSET cfg_ignore
        dd      OFFSET cfgn_files,   OFFSET cfg_ignore
        dd      OFFSET cfgn_lastdrv, OFFSET cfg_ignore
        dd      OFFSET cfgn_fcbs,    OFFSET cfg_ignore
        dd      OFFSET cfgn_stacks,  OFFSET cfg_ignore
        dd      OFFSET cfgn_country, OFFSET cfg_ignore
        dd      OFFSET cfgn_switches,OFFSET cfg_ignore
        dd      OFFSET cfgn_numlock, OFFSET cfg_ignore
        dd      OFFSET cfgn_dos,     OFFSET cfg_ignore
        dd      OFFSET cfgn_drivparm,OFFSET cfg_ignore
        dd      OFFSET cfgn_install, OFFSET cfg_ignore
        dd      OFFSET cfgn_insthigh,OFFSET cfg_ignore
        dd      OFFSET cfgn_set,     OFFSET cfg_ignore
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

;=====================================================================
; Data
;=====================================================================
cfg_name    db  'A:\CONFIG.SYS',0       ; drive letter patched at boot
cfg_val     dd  0
cfg_tmp     dd  0
break_flag  db  0

msgCrLf         db  13,10,0
msgCfgBadCmd    db  'Unrecognized command in CONFIG.SYS',13,10,0
msgDevNoFile    db  'Bad or missing device driver',13,10,0
msgDevNoMem     db  'Not enough memory for device driver',13,10,0
msgDevBlock     db  'Block device drivers are not supported yet',13,10,0
msgDevInit      db  'Device driver failed to initialise',13,10,0
msgDevFull      db  'Too many device drivers',13,10,0
msgCfgBadVal    db  'Invalid parameter in CONFIG.SYS',13,10,0
IFDEF PMDOS_DEBUG
msgCfgFound     db  'Processing CONFIG.SYS',13,10,0
msgCfgLine      db  'CONFIG: ',0
msgCfgInert     db  '        (recognised, no effect)',13,10,0
ENDIF

cfg_key     db  CFG_KEY_MAX dup(0)
cfg_line_buf db CFG_LINE_MAX dup(0)
cfg_buf     db  CFG_BUF_SIZE + 1 dup(0)
