;=====================================================================
; CSOCK.ASM - CSOCK.COM, a 16-bit DOS program that drives PMDOS.386's
;             PDAPI_SOCK V86 API from inside Windows for Workgroups
;             3.11 enhanced mode: it opens a TCP socket on the parked
;             kernel's TCP/IP stack, connects to an HTTP peer, sends a
;             GET and receives the reply - all through the bridge.
;
; It is Stage C's analogue of BA2API (which proved PDAPI_REDIR before
; the RMDOS redirector server used it): this proves the socket API
; surface and the poll/yield loop that WINSOCK.DLL will use.  Every
; socket op is instant (the stack never blocks), so one bridge window
; does each; the WAITING - for the handshake to finish, for data to
; arrive - is here, polling SOCK_STATE / SOCK_RECV and releasing the
; time slice (INT 2Fh 1680h) between polls so Windows and the bridge's
; async tick run and advance the parked stack.  So the fetch completes
; with Windows never paused.
;
; The VxD traces each completed socket op on port E9h (the headless
; channel), and a receive shows its first bytes as text - so a healthy
; run shows "sock fn=1 ans=<handle>", "...fn=3..." (connect), "...fn=6
; ans=<n>" (the GET sent), "...fn=7 ans=<n> data=HTTP/1.0 200 ..." (the
; reply received through the bridge), "...fn=2" (closed).
;
; Reaches the VxD the standard way: INT 2Fh AX=1684h, BX=504Dh hands
; back a far call-in address in ES:DI.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_T      SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_T, DS:_T, ES:_T, SS:_T
        ORG     0

PMDOS_DEVICE_ID EQU     504Dh
PDAPI_SOCK      EQU     3

; the socket API (INC\SOCKET.H)
SOCK_OPEN       EQU     1
SOCK_CLOSE      EQU     2
SOCK_CONNECT    EQU     3
SOCK_SEND       EQU     6
SOCK_RECV       EQU     7
SOCK_STATE      EQU     8

SOCK_STREAM     EQU     1

SS_ESTABLISHED  EQU     4
SS_ERROR        EQU     6

; SOCK_IO header: handle(4) ip(4) port(2) spare(2) len(4) = 16
SIO_HANDLE      EQU     0
SIO_IP          EQU     4
SIO_PORT        EQU     8
SIO_LEN         EQU     12
SIO_HDR         EQU     16

; SOCK_STAT: handle(4) state(4) rxready(4) txfree(4) err(4) = 20
SST_STATE       EQU     4
SST_LEN         EQU     20

; the HTTP peer the rig forwards to the host (winrun -HttpPeer): the
; guest address 10.0.2.200, port 80 (a dword, most significant octet
; first: 0A 00 02 C8 = 10.0.2.200)
PEER_IP         EQU     0A0002C8h
PEER_PORT       EQU     80

SETTLE_TICKS    EQU     54              ; ~3s for DHCP to bind before connect
CONN_TRIES      EQU     40000           ; connect poll cap (yields between)
RECV_TRIES      EQU     40000           ; receive poll cap (yields between)

RECV_ROOM       EQU     512             ; bytes of room per receive

start:
        mov     dx,OFFSET msg_hi
        call    puts

        ; the VxD's V86 API entry (0:0 if no WIN386/VxD under us)
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

        ; let DHCP bind the interface before connecting
        mov     cx,SETTLE_TICKS
        call    wait_ticks

        ;--- open a TCP socket -----------------------------------------
        call    build_open
        mov     bx,SOCK_OPEN
        mov     cx,4                            ; sizeof SOCK_OPENREQ
        call    sock_call                       ; -> AX = handle, CF on error
        jc      s_fail
        or      ax,ax
        jz      s_fail
        mov     [sockh],ax
        mov     dx,OFFSET msg_open
        call    puts

        ;--- connect to the HTTP peer ----------------------------------
        call    build_conn
        mov     bx,SOCK_CONNECT
        mov     cx,12                           ; sizeof SOCK_CONNREQ
        call    sock_call
        jc      s_fail

        ; poll the state until established (or error / give up)
        mov     cx,CONN_TRIES
cn_poll:
        push    cx
        call    build_stat
        mov     bx,SOCK_STATE
        mov     cx,SST_LEN
        call    sock_call
        pop     cx
        mov     ax,word ptr [buf+SST_STATE]
        cmp     ax,SS_ESTABLISHED
        je      connected
        cmp     ax,SS_ERROR
        je      s_fail
        call    yield
        loop    cn_poll
        jmp     s_fail                          ; handshake did not finish
connected:
        mov     dx,OFFSET msg_conn
        call    puts

        ;--- send the HTTP GET -----------------------------------------
        call    build_send                      ; -> CX = total length
        mov     bx,SOCK_SEND
        call    sock_call                       ; CX already set by build_send
        jc      s_fail
        mov     dx,OFFSET msg_sent
        call    puts

        ;--- receive the reply -----------------------------------------
        mov     cx,RECV_TRIES
rv_poll:
        push    cx
        call    build_recv
        mov     bx,SOCK_RECV
        mov     cx,SIO_HDR + RECV_ROOM
        call    sock_call                       ; -> AX = bytes given (0 none)
        pop     cx
        or      ax,ax
        jnz     got_data                        ; the VxD traced the bytes as text
        ; none yet: is the socket still alive?
        push    cx
        call    build_stat
        mov     bx,SOCK_STATE
        mov     cx,SST_LEN
        call    sock_call
        pop     cx
        mov     ax,word ptr [buf+SST_STATE]
        cmp     ax,SS_ESTABLISHED
        ja      rv_dead                         ; CLOSING/ERROR and nothing left
        call    yield
        loop    rv_poll
        jmp     rv_dead                         ; gave up waiting
got_data:
        mov     dx,OFFSET msg_recv
        call    puts
rv_dead:
        ;--- close -----------------------------------------------------
        call    build_close
        mov     bx,SOCK_CLOSE
        mov     cx,4
        call    sock_call
        mov     dx,OFFSET msg_closed
        call    puts
        jmp     done
s_fail:
        mov     dx,OFFSET msg_fail
        call    puts
done:
        mov     ax,4C00h
        int     21h

;---------------------------------------------------------------------
; sock_call - one PDAPI_SOCK call on [buf].  BX = the SOCK_* function,
;             CX = the request length.  Retries on the bridge-busy
;             answer (AX=0FFFFh + CF), yielding between.  -> AX = the
;             answer, CF = the socket op's carry.  Keeps BX/CX/DX.
;---------------------------------------------------------------------
sock_call:
        push    bx
        push    cx
        push    dx
        push    si
        mov     si,256                          ; busy-retry cap
sc_again:
        push    cs
        pop     es
        mov     dx,OFFSET buf
        mov     ax,PDAPI_SOCK
        call    dword ptr [api_ent]             ; AX=answer, CF=carry; AX=FFFFh+CF busy
        jnc     sc_done
        cmp     ax,0FFFFh
        jne     sc_done                         ; a real socket error: carry out
        ; bridge busy this instant: yield and retry
        call    yield
        dec     si
        jnz     sc_again
        stc                                     ; gave up: report an error
sc_done:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

;--- yield - release this VM's time slice so Windows and the tick run -
yield:
        push    ax
        mov     ax,1680h
        int     2Fh
        pop     ax
        ret

;---------------------------------------------------------------------
; wait_ticks - CX = BIOS ticks (18.2/s) to wait, yielding between
;              reads.  40:6Ch advances under WfW.  Keeps all registers.
;---------------------------------------------------------------------
wait_ticks:
        push    eax
        push    ebx
        push    ecx
        push    es
        movzx   ecx,cx
        xor     ax,ax
        mov     es,ax
        mov     ebx,dword ptr es:[46Ch]
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
; the request builders - fill [buf].  Each clears the fields it uses;
; the block is big enough for any of them.
;---------------------------------------------------------------------
build_open:
        mov     dword ptr [buf+0],SOCK_STREAM
        ret

build_conn:
        movzx   eax,word ptr [sockh]
        mov     dword ptr [buf+0],eax           ; handle
        mov     dword ptr [buf+4],PEER_IP       ; ip
        mov     word ptr [buf+8],PEER_PORT      ; port
        mov     word ptr [buf+10],0             ; spare
        ret

build_stat:
        movzx   eax,word ptr [sockh]
        mov     dword ptr [buf+0],eax
        mov     dword ptr [buf+SST_STATE],0
        ret

build_send:
        ; SOCK_IO header, then the request bytes
        movzx   eax,word ptr [sockh]
        mov     dword ptr [buf+SIO_HANDLE],eax
        mov     dword ptr [buf+SIO_IP],0        ; TCP ignores ip/port here
        mov     word ptr [buf+SIO_PORT],0
        mov     word ptr [buf+SIO_PORT+2],0
        mov     cx,req_len
        mov     dword ptr [buf+SIO_LEN],ecx     ; len = the request size (ecx high is 0)
        ; copy the request after the header
        mov     si,OFFSET http_req
        mov     di,OFFSET buf + SIO_HDR
        mov     cx,req_len
        cld
        push    ds
        pop     es
        rep     movsb
        mov     cx,SIO_HDR + req_len            ; total length for the call
        ret

build_recv:
        movzx   eax,word ptr [sockh]
        mov     dword ptr [buf+SIO_HANDLE],eax
        mov     dword ptr [buf+SIO_IP],0
        mov     word ptr [buf+SIO_PORT],0
        mov     word ptr [buf+SIO_PORT+2],0
        mov     dword ptr [buf+SIO_LEN],RECV_ROOM  ; room available
        ret

build_close:
        movzx   eax,word ptr [sockh]
        mov     dword ptr [buf+0],eax
        ret

;--- puts - print the $-terminated string at DS:DX.  Keeps registers. -
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret

;--- data ------------------------------------------------------------
api_ent     dd  0
sockh       dw  0

http_req    db  'GET / HTTP/1.0',13,10,'Host: pmdos',13,10,13,10
req_len     EQU $ - http_req

msg_hi      db  'CSOCK: PMDOS.386 socket API test',13,10,'$'
msg_noapi   db  'CSOCK: no bridge API (INT 2Fh 1684h gave 0:0)',13,10,'$'
msg_open    db  'CSOCK: socket opened',13,10,'$'
msg_conn    db  'CSOCK: connected',13,10,'$'
msg_sent    db  'CSOCK: request sent',13,10,'$'
msg_recv    db  'CSOCK: reply received',13,10,'$'
msg_closed  db  'CSOCK: closed',13,10,'$'
msg_fail    db  'CSOCK: FAILED',13,10,'$'

buf         db  (SIO_HDR + RECV_ROOM + 16) dup(0)
_T      ENDS
        END     start
