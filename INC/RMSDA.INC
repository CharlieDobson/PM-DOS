;=====================================================================
; RMSDA.INC - the swappable data area's offsets, MS-DOS 5's, for the
;             real-mode side: RMDOS (RMINFO.INC, which publishes the
;             area) and RMCD.SYS (which reads it, as a redirector
;             does).  Separate from RMDOS.INC because the kernel
;             includes that one too, and has an SDA of its own.
;=====================================================================

;--- the swappable data area, laid out as MS-DOS lays it out.  Same
;--- fields, same order, same reasons as the kernel's own - see the
;--- SDA block in PMDOS.INC, which took its layout from the DOS 6
;--- source and this one copies.
SD_ERRMODE      EQU 00h
SD_INDOS        EQU 01h
SD_WPERR        EQU 02h
SD_LOCUS        EQU 03h
SD_EXTERR       EQU 04h
SD_ACTION       EQU 06h
SD_CLASS        EQU 07h
SD_ERRPTR       EQU 08h
SD_DTA          EQU 0Ch
SD_PSP          EQU 10h
SD_CONCSP       EQU 12h
SD_EXITCODE     EQU 14h
SD_DRIVE        EQU 16h
SD_BREAK        EQU 17h
SD_CPSW         EQU 18h
SD_CPSWSAVE     EQU 19h
SD_LEN          EQU 1Ah

;--- ...and the rest of DOS 5's, which only a network redirector reads.
;--- MS-DOS fills these in before each INT 2Fh 11xxh it makes and the
;--- redirector takes its arguments out of them (RMNET.INC).  The
;--- offsets are SWAP_START's in DOS6SRC inc/const2.asm and
;--- inc/ms_data.asm, counted field by field; IFSHLP.SYS reads THISSFT,
;--- THISCDS and WFP_START at exactly these, and the current PSP at 10h.
SD_USERAX       EQU 1Ah                 ; dw  the caller's AX (USER_IN_AX)
SD_PROCID       EQU 1Ch                 ; dw  the process the call is for
SD_FN1          EQU 9Eh                 ; 128 the name (OPENBUF)
SD_FN2          EQU 11Eh                ; 128 a rename's new name (RENBUF)
SD_SRCH         EQU 19Eh                ; 21 + 32: a search's state, then
                                        ; the entry it found (SEARCHBUF)
SD_SATTR        EQU 24Dh                ; db  the search attribute (SATTRIB)
SD_USERSP       EQU 264h                ; dw  where the caller's registers are,
SD_USERSS       EQU 266h                ; dw  MS-DOS's shape (user_SP/user_SS):
                                        ; set only for the calls passed to the
                                        ; redirector whole (RMNET.INC,
                                        ; net_user), which answers through them
SD_THISDPB      EQU 26Ah                ; dd
SD_THISSFT      EQU 27Eh                ; dd  the file table entry
SD_THISCDS      EQU 282h                ; dd  the current directory structure
SD_SFN          EQU 28Ah                ; dw  the entry's number
SD_JFN          EQU 28Ch                ; dw  the handle
SD_PJFN         EQU 28Eh                ; dd  the handle's byte in the table
SD_WFP          EQU 292h                ; dw  the name's offset in this
SD_RENWFP       EQU 294h                ; dw  segment, and the new name's
SD_CURDIR       EQU 296h                ; dw  CURR_DIR_END: FFFFh here
SD_BYTPOS       EQU 2AEh                ; dd  a read or write's position
IF SD_BYTPOS + 4 GT RL_SDALEN
        .ERR    <RMSDA.INC: an SDA field past what RMDOS.INC lays out>
ENDIF
