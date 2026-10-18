;=====================================================================
; BA2API.ASM - BA2API.COM, a 16-bit DOS program that drives PMDOS.386's
;              PDAPI_REDIR V86 API from inside Windows for Workgroups
;              3.11 enhanced mode: a resumable SMB MOUNT, then a
;              FINDFIRST of the share's root, on the parked kernel's
;              SMBFS redirector, through the bridge.
;
; It is BA2b's analogue of BRGTEST (B1).  BRGTEST proved a V86 program
; can reach the bridge for an instant call (NIC_INFO); this proves it
; for a RESUMABLE one.  A redirector op is many round trips, and the
; bridge window runs interrupts off, so the driver cannot wait inside
; it - it runs on a worker fibre and yields when a socket wait has no
; data.  The API runs ONE slice per call and answers REDIR_PENDING
; until the op finishes; this program polls, and between polls RELEASES
; its time slice (INT 2Fh 1680h) so Windows and the bridge's async tick
; run and advance the parked stack.  So the listing comes back with
; Windows never paused - which is what RMDOS's INT 2Fh 11xx server will
; do for a DOS box's DIR on a kernel net drive (BA2b-2).
;
; The VxD traces each finished op on port E9h (the headless channel);
; this program drives and prints its own progress to PRN.  It reaches
; the VxD the way any V86 program reaches a VxD: INT 2Fh AX=1684h with
; the device id in BX hands back a far call-in address in ES:DI.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_T      SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_T, DS:_T, ES:_T, SS:_T
        ORG     0

PMDOS_DEVICE_ID EQU     504Dh
PDAPI_REDIR     EQU     1

; the redir_req (INC\REDIR.INC), the fields this test fills and reads
NR_OP           EQU     0
NR_DRIVE        EQU     4
NR_STATUS       EQU     12
NR_ATTR         EQU     40
NR_SPARE        EQU     60
NR_PATH         EQU     64
NR_SERVER       EQU     356
NR_SHARE        EQU     420
NR_USER         EQU     500
NR_PASS         EQU     564
NR_SIZE         EQU     692

REDIR_MOUNT     EQU     1
REDIR_FINDFIRST EQU     3
REDIR_PENDING   EQU     0FFh
REDIR_SRCH      EQU     16h             ; a find's search attribute (dirs too)
PD_SELFDRIVE    EQU     13              ; N:, the kernel's net-drive slot

SETTLE_TICKS    EQU     36              ; ~2s to let DHCP bind before the first try
RETRY_TICKS     EQU     18              ; ~1s between mount retries
MOUNT_TRIES     EQU     12
POLL_MAX        EQU     60000           ; a wedged op gives up, it does not hang

start:
        mov     dx,OFFSET msg_hi
        call    puts

        ; the VxD's V86 API entry (0:0 if there is no WIN386/VxD under us)
        xor     di,di
        mov     es,di
        mov     ax,1684h
        mov     bx,PMDOS_DEVICE_ID
        int     2Fh
        mov     ax,es
        or      ax,di
        jnz     have_api
        mov     dx,OFFSET msg_noapi
        call    puts
        jmp     done
have_api:
        mov     word ptr [api_ent],di
        mov     word ptr [api_ent+2],es

        ; let DHCP bind the interface before the first mount attempt
        mov     cx,SETTLE_TICKS
        call    wait_ticks

        ;--- MOUNT, retried while the stack is still coming up ----------
        mov     cx,MOUNT_TRIES
mount_try:
        push    cx
        call    build_mount
        call    poll_op                 ; -> AX = final NR_STATUS
        pop     cx
        or      ax,ax
        jz      mounted
        push    cx
        mov     cx,RETRY_TICKS
        call    wait_ticks
        pop     cx
        loop    mount_try
        mov     dx,OFFSET msg_mfail
        call    puts
        jmp     done
mounted:
        mov     dx,OFFSET msg_mok
        call    puts

        ;--- FINDFIRST on the share root -------------------------------
        call    build_find
        call    poll_op                 ; -> AX = final NR_STATUS
        or      ax,ax
        jnz     find_fail
        mov     dx,OFFSET msg_fok
        call    puts
        jmp     done
find_fail:
        mov     dx,OFFSET msg_ffail
        call    puts
done:
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; poll_op - call PDAPI_REDIR on [buf] until NR_STATUS != REDIR_PENDING,
;           releasing the time slice between polls so Windows and the
;           bridge's tick run.  -> AX = the final NR_STATUS (the pending
;           value if it gave up).  Registers but AX are kept.
;---------------------------------------------------------------------
poll_op:
        push    cx
        push    dx
        mov     cx,POLL_MAX
po_again:
        push    cx
        push    cs
        pop     es
        mov     dx,OFFSET buf
        mov     ax,PDAPI_REDIR
        call    dword ptr [api_ent]     ; -> AX = NR_STATUS, CF clear
        pop     cx
        cmp     ax,REDIR_PENDING
        jne     po_done
        push    ax
        mov     ax,1680h                ; release this VM's time slice
        int     2Fh
        pop     ax
        loop    po_again
po_done:
        pop     dx
        pop     cx
        ret

;---------------------------------------------------------------------
; wait_ticks - CX = BIOS ticks (18.2/s) to wait, releasing the time
;              slice between reads.  40:6Ch advances under WfW.  All
;              registers kept.
;---------------------------------------------------------------------
wait_ticks:
        push    eax
        push    ebx
        push    ecx
        push    es
        movzx   ecx,cx
        xor     ax,ax
        mov     es,ax
        mov     ebx,dword ptr es:[46Ch]         ; start tick
wt_loop:
        mov     ax,1680h
        int     2Fh
        mov     eax,dword ptr es:[46Ch]
        sub     eax,ebx
        cmp     eax,ecx
        jb      wt_loop
        pop     es
        pop     ecx
        pop     ebx
        pop     eax
        ret

;---------------------------------------------------------------------
; build_mount / build_find - fill [buf] with a request.  The numeric
; fields are dwords; zero_buf clears the whole block first.
;---------------------------------------------------------------------
build_mount:
        call    zero_buf
        mov     dword ptr [buf+NR_OP],REDIR_MOUNT
        mov     dword ptr [buf+NR_DRIVE],PD_SELFDRIVE
        mov     dword ptr [buf+NR_SPARE],1              ; SMB1 (NR_SPARE picks it)
        mov     si,OFFSET s_srv
        mov     di,OFFSET buf+NR_SERVER
        call    copyz
        mov     si,OFFSET s_shr
        mov     di,OFFSET buf+NR_SHARE
        call    copyz
        mov     si,OFFSET s_usr
        mov     di,OFFSET buf+NR_USER
        call    copyz
        mov     si,OFFSET s_pwd
        mov     di,OFFSET buf+NR_PASS
        call    copyz
        ret

build_find:
        call    zero_buf
        mov     dword ptr [buf+NR_OP],REDIR_FINDFIRST
        mov     dword ptr [buf+NR_DRIVE],PD_SELFDRIVE
        mov     dword ptr [buf+NR_ATTR],REDIR_SRCH
        mov     si,OFFSET s_pat
        mov     di,OFFSET buf+NR_PATH
        call    copyz
        ret

;--- zero_buf - clear [buf] (NR_SIZE bytes).  Registers kept. ---------
zero_buf:
        push    ax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di,OFFSET buf
        mov     cx,NR_SIZE
        xor     al,al
        cld
        rep     stosb
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

;--- copyz - copy the ASCIIZ string DS:SI -> DS:DI.  Registers kept. --
copyz:
        push    si
        push    di
        push    ax
cz_lp:
        mov     al,[si]
        mov     [di],al
        inc     si
        inc     di
        or      al,al
        jnz     cz_lp
        pop     ax
        pop     di
        pop     si
        ret

;--- puts - print the $-terminated string at DS:DX.  Registers kept. --
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

;--- data ------------------------------------------------------------
api_ent     dd  0

s_srv       db  '10.0.2.200',0
s_shr       db  'PMSHARE',0
s_usr       db  'pmdostest',0
s_pwd       db  'PmdosSmb!2026',0
s_pat       db  '*',0

msg_hi      db  'BA2API: PMDOS.386 redirector API test',13,10,'$'
msg_noapi   db  'BA2API: no bridge API (INT 2Fh 1684h gave 0:0)',13,10,'$'
msg_mok     db  'BA2API: mounted',13,10,'$'
msg_mfail   db  'BA2API: mount failed',13,10,'$'
msg_fok     db  'BA2API: findfirst ok',13,10,'$'
msg_ffail   db  'BA2API: findfirst failed',13,10,'$'

buf         db  NR_SIZE dup(0)
_T      ENDS
        END     start
