;=====================================================================
; CDATEST.ASM - CDATEST.COM: the MSCD001 device, and CD audio, asked
;               the questions a CD player asks.
;
; SIXTEEN-BIT ON PURPOSE, for the same reason CDXTEST is: the MSCDEX
; API is answered out of the V86 monitor's INT 2Fh chain and a 32-bit
; native program cannot reach that path at all.  CDXTEST covers the
; calls that are questions ABOUT the drives; this one covers the calls
; that go TO the device - which is every audio call there is, because
; the audio API is the device interface.
;
; What it checks, in the order a CD player would:
;
;   1501h  the driver list - and the header it points at must really
;          say MSCD001, because a program that finds a driver list
;          with rubbish in it will call the rubbish
;   1510h  IOCTL input 6, the device status
;          IOCTL input 7, the sector size - 2048 on every CD
;          IOCTL input 10, the tracks on this disc
;          IOCTL input 11, each track: where it starts and whether it
;          is music or data
;   PLAY   the first audio track, for a few seconds
;   1510h  IOCTL input 12, the Q channel, TWICE - and the second
;          reading must be LATER than the first.  That is the only
;          check here that cannot be passed by a driver which merely
;          answers politely: the drive has to be really playing
;          for the position to move
;   STOP   ...and IOCTL input 15 must then say paused
;   RESUME ...and the position must move again
;   STOP twice, which really stops
;   THE DEVICE ITSELF: the strategy and interrupt entries out of the
;          header are CALLED DIRECTLY, the way a program that found
;          the driver through 1501h would call them, rather than
;          through 1510h.  Those two entry points are the part of
;          this that is new machinery, and nothing else tests them.
;
; A disc with an audio track is wanted.  TOOLS\MKCD.PL builds one:
; MIXED.CUE is data track first and music after, which is what a game
; with CD music is, and CDEXTRA.CUE is the other way round.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_CX     SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_CX, DS:_CX, ES:_CX, SS:_CX
        ORG     0

start:
        mov     dx,OFFSET msg_hi
        call    puts

;=====================================================================
; The drive letter to work on, out of 150Dh.  1500h first, because a
; machine with no CD driver has nothing to ask.
;=====================================================================
        xor     bx,bx
        mov     ax,1500h
        int     2Fh
        or      bx,bx
        jnz     c_have
        mov     dx,OFFSET msg_nodrv
        call    puts
        jmp     c_bye
c_have:
        mov     bx,OFFSET letters
        mov     ax,150Dh
        int     2Fh
        mov     al,[letters]
        mov     [drive],al
        mov     dx,OFFSET msg_drive
        call    puts
        mov     al,[drive]
        add     al,'A'
        mov     dl,al
        call    putc
        mov     dl,':'
        call    putc
        call    crlf
        call    crlf

;=====================================================================
; 1501h - the driver list, and what it points at.
;
; Five bytes per drive: the subunit, then a far pointer to the device
; header.  THE NAME IS THE CHECK: eight bytes at offset 0Ah of the
; header, and they have to be MSCD001 followed by a space.
;=====================================================================
        mov     dx,OFFSET msg_1501
        call    puts
        mov     bx,OFFSET drvlist
        mov     ax,1501h
        int     2Fh
        jc      c_01bad

        mov     ax,word ptr [drvlist+1]         ; the header's offset
        mov     [hdroff],ax
        mov     ax,word ptr [drvlist+3]         ; ...and its segment
        mov     [hdrseg],ax
        or      ax,ax
        jz      c_01bad

        mov     es,ax
        mov     bx,[hdroff]
        mov     si,OFFSET msg_mscd
        mov     cx,8
        mov     di,bx
        add     di,0Ah                          ; the name field
c_01name:
        mov     al,es:[di]
        cmp     al,[si]
        jne     c_01bad
        inc     si
        inc     di
        loop    c_01name

        mov     ax,es:[bx+6]                    ; the strategy entry
        mov     word ptr [fstrat],ax
        mov     ax,es
        mov     word ptr [fstrat+2],ax
        mov     bx,[hdroff]
        mov     ax,es:[bx+8]                    ; and the interrupt entry
        mov     word ptr [fintr],ax
        mov     ax,es
        mov     word ptr [fintr+2],ax
        push    ds
        pop     es
        call    ok
        jmp     short c_06
c_01bad:
        push    ds
        pop     es
        call    bad
        jmp     c_bye                           ; nothing below works without
                                                ; a device to talk to
;=====================================================================
; IOCTL input 6 - what this device says it can do, and what is in it.
;=====================================================================
c_06:
        mov     dx,OFFSET msg_stat
        call    puts
        mov     al,6
        mov     cx,5
        call    ioctl_in
        jc      c_06bad
        mov     ax,word ptr [cblk+3]
        call    put4
        mov     ax,word ptr [cblk+1]
        call    put4
        call    ok
        jmp     short c_07
c_06bad:
        call    bad

;=====================================================================
; IOCTL input 7 - the sector size, which is 2048 on every CD there has
;                 ever been.  A check that reads back a number we know.
;=====================================================================
c_07:
        mov     dx,OFFSET msg_secsz
        call    puts
        mov     al,7
        mov     cx,4
        call    ioctl_in
        jc      c_07bad
        mov     ax,word ptr [cblk+2]
        call    put5d
        cmp     ax,2048
        jne     c_07bad
        call    ok
        jmp     short c_10
c_07bad:
        call    bad

;=====================================================================
; IOCTL input 10 - the tracks on this disc, and where the music ends.
;=====================================================================
c_10:
        mov     dx,OFFSET msg_disc
        call    puts
        mov     al,10
        mov     cx,7
        call    ioctl_in
        jc      c_10bad
        mov     al,[cblk+1]
        mov     [trk_lo],al
        call    put2d
        mov     dl,'-'
        call    putc
        mov     al,[cblk+2]
        mov     [trk_hi],al
        call    put2d
        mov     dx,OFFSET msg_leadout
        call    puts
        mov     al,[cblk+5]                     ; minutes
        mov     ah,[cblk+4]                     ; seconds
        mov     bl,[cblk+3]                     ; frames
        call    putmsf
        call    ok
        jmp     short c_11
c_10bad:
        call    bad
        jmp     c_done

;=====================================================================
; IOCTL input 11 - each track in turn: where it starts, and whether it
;                  is data or music.  The first audio track found is
;                  the one the play test below uses.
;=====================================================================
c_11:
        mov     al,[trk_lo]
        mov     [trk],al
c_11lp:
        mov     dx,OFFSET msg_track
        call    puts
        mov     al,[trk]
        call    put2d
        mov     dl,' '
        call    putc

        mov     al,11
        mov     cx,7
        mov     bl,[trk]
        mov     [cblk+1],bl                     ; which track is asked about
        call    ioctl_in
        jc      c_11bad

        ; THE STARTING POINT IS A DWORD AT +2 WITH THE FRAME IN THE
        ; LOWEST BYTE, which is the reverse of how anybody says it out
        ; loud - minutes, seconds, frames.  Reading it as if the
        ; minutes came first is how the first run of this asked the
        ; drive to play from an address before the start of the disc
        ; and was quite properly refused.
        mov     al,[cblk+4]             ; minutes
        mov     ah,[cblk+3]             ; seconds
        mov     bl,[cblk+2]             ; frames
        call    putmsf
        mov     dl,' '
        call    putc

        ; the control byte: bit 6 of the high nibble means data
        mov     al,[cblk+6]
        test    al,40h
        jz      c_11audio
        mov     dx,OFFSET msg_data
        call    puts
        jmp     short c_11next
c_11audio:
        mov     dx,OFFSET msg_audio
        call    puts
        cmp     byte ptr [play_trk],0
        jne     c_11next
        mov     al,[trk]                        ; the first one, kept
        mov     [play_trk],al
        mov     al,[cblk+2]
        mov     [play_f],al
        mov     al,[cblk+3]
        mov     [play_s],al
        mov     al,[cblk+4]
        mov     [play_m],al
c_11next:
        call    crlf
        mov     al,[trk]
        inc     al
        mov     [trk],al
        cmp     al,[trk_hi]
        jbe     c_11lp
        jmp     short c_play
c_11bad:
        call    bad

;=====================================================================
; PLAY AUDIO - and then the only check in this program that a merely
;              polite driver cannot pass.
;=====================================================================
c_play:
        call    crlf
        cmp     byte ptr [play_trk],0
        jne     c_playgo
        mov     dx,OFFSET msg_noaudio
        call    puts
        jmp     c_direct
c_playgo:
        mov     dx,OFFSET msg_play
        call    puts
        mov     al,[play_trk]
        call    put2d

        ; the request: Red Book addressing, which is the notation the
        ; track list came back in, and 300 frames - four seconds
        mov     al,132
        call    mkreq
        mov     byte ptr [req+0Dh],1            ; Red Book
        mov     al,[play_f]
        mov     byte ptr [req+0Eh],al
        mov     al,[play_s]
        mov     byte ptr [req+0Fh],al
        mov     al,[play_m]
        mov     byte ptr [req+10h],al
        mov     byte ptr [req+11h],0
        mov     word ptr [req+12h],300
        mov     word ptr [req+14h],0
        call    sendreq
        jc      c_playbad
        call    ok
        jmp     short c_q1
c_playbad:
        call    bad
        jmp     c_direct                ; nothing below this can mean
                                        ; anything if the play was refused
c_q1:
        ; where is the head now, and where is it a moment later?
        mov     dx,OFFSET msg_q1
        call    puts
        call    qchan
        jc      c_qbad
        call    putq
        mov     eax,[q_abs]
        mov     [q_first],eax
        call    ok

        mov     cx,36                           ; about two seconds
        call    waitticks

        mov     dx,OFFSET msg_q2
        call    puts
        call    qchan
        jc      c_qbad
        call    putq
        mov     eax,[q_abs]
        cmp     eax,[q_first]
        jbe     c_qsame
        call    ok
        jmp     short c_busy
c_qsame:
        mov     dx,OFFSET msg_notmoving
        call    puts
        inc     byte ptr [failures]
        jmp     short c_busy
c_qbad:
        call    bad

;--- the busy bit, which is how a program knows the music is still on
c_busy:
        mov     dx,OFFSET msg_busy
        call    puts
        mov     al,6
        mov     cx,5
        call    ioctl_in                        ; any request will do
        mov     ax,word ptr [req+3]
        test    ax,0200h
        jz      c_busybad
        call    ok
        jmp     short c_stop
c_busybad:
        call    bad

;--- STOP, which pauses -----------------------------------------------
c_stop:
        mov     dx,OFFSET msg_stop
        call    puts
        mov     al,133
        call    mkreq
        call    sendreq
        jc      c_stopbad
        mov     al,15
        mov     cx,11
        call    ioctl_in
        jc      c_stopbad
        test    byte ptr [cblk+1],1             ; paused
        jz      c_stopbad
        call    ok
        jmp     short c_resume
c_stopbad:
        call    bad

;--- RESUME, and the position must move again -------------------------
c_resume:
        mov     dx,OFFSET msg_resume
        call    puts
        mov     al,136
        call    mkreq
        call    sendreq
        jc      c_resbad
        call    qchan
        mov     eax,[q_abs]
        mov     [q_first],eax
        mov     cx,36
        call    waitticks
        call    qchan
        mov     eax,[q_abs]
        cmp     eax,[q_first]
        jbe     c_resbad
        call    ok
        jmp     short c_stop2
c_resbad:
        call    bad

;--- and STOP twice, which really stops --------------------------------
c_stop2:
        mov     dx,OFFSET msg_stop2
        call    puts
        mov     al,133
        call    mkreq
        call    sendreq
        mov     al,133
        call    mkreq
        call    sendreq
        mov     al,15
        mov     cx,11
        call    ioctl_in
        jc      c_stop2bad
        test    byte ptr [cblk+1],1
        jnz     c_stop2bad
        call    ok
        jmp     short c_direct
c_stop2bad:
        call    bad

;=====================================================================
; THE DEVICE ITSELF - strategy and interrupt, called the way a program
; that found the driver through 1501h calls them, with no INT 2Fh
; anywhere in it.
;
; This is the part of the release that is new machinery: two far
; entry points in guest memory that fault into a 32-bit kernel and
; come back.  A program doing this is not rare - it is what the
; driver list is FOR - and nothing else here tests that path.
;=====================================================================
c_direct:
        call    crlf
        mov     dx,OFFSET msg_direct
        call    puts
        mov     al,3                            ; IOCTL input
        call    mkreq
        mov     byte ptr [req+1],0              ; subunit 0: no MSCDEX to
                                                ; fill it in on this path
        mov     word ptr [req+0Eh],OFFSET cblk
        mov     ax,ds
        mov     word ptr [req+10h],ax
        mov     word ptr [req+12h],5
        mov     byte ptr [cblk],6               ; the device status again
        mov     bx,OFFSET req
        push    ds
        pop     es                              ; ES:BX = the request
        call    dword ptr [fstrat]
        mov     bx,OFFSET req
        push    ds
        pop     es
        call    dword ptr [fintr]
        mov     ax,word ptr [req+3]
        test    ax,0100h                        ; done
        jz      c_dirbad
        test    ax,8000h                        ; ...and not an error
        jnz     c_dirbad
        call    ok
        jmp     c_upc
c_dirbad:
        call    bad

;=====================================================================
; IOCTL input 14 - the disc's catalogue number.
;
; MOST DISCS HAVE NONE and that is a pass, not a failure: the drive
; says so and the field comes back zero.  What is being tested is that
; the call is ANSWERED - it used to be "unknown command" - and that
; what comes back is packed BCD rather than the ASCII the drive
; returns, because the conversion is the only part of this a driver
; can get wrong while still looking like it works.
;=====================================================================
c_upc:
        call    crlf
        mov     dx,OFFSET msg_upc
        call    puts
        mov     al,14
        mov     cx,10
        call    ioctl_in
        jc      c_upcbad
        cmp     byte ptr [cblk],0Eh             ; the code, echoed back
        jne     c_upcbad
        mov     si,OFFSET cblk + 2              ; seven bytes of it
        mov     cx,7
c_upclp:
        lodsb
        call    put2
        loop    c_upclp
        call    ok
        jmp     short c_raw
c_upcbad:
        call    bad

;=====================================================================
; READ LONG, RAW - and the check is the best one available anywhere in
; this program, because it does not need to trust anything.
;
; The same sector is read TWICE: cooked, which is 2048 bytes of user
; data through the ordinary path, and raw, which is the whole 2352-byte
; frame.  Two things must then be true, and between them they leave a
; wrong answer nowhere to hide:
;
;   the frame begins with the SYNC PATTERN, 00 then ten FFs then 00,
;   which is the one part of a sector that is the same on every disc
;   ever pressed, and
;
;   the 2048 bytes at offset 16 of the frame are the cooked sector,
;   byte for byte.  A driver that quietly served a cooked read when
;   asked for a raw one passes neither test; one that returned the
;   right length of the wrong data passes the first and fails this.
;
; A DRIVE THAT CANNOT DO IT SAYS SO, and that is reported rather than
; counted as a failure: READ CD is not on every drive, and "unknown
; command" from one that lacks it is the truth.
;=====================================================================
c_raw:
        call    crlf
        mov     dx,OFFSET msg_raw
        call    puts

        mov     al,128                          ; read long, cooked
        call    mkreq
        mov     byte ptr [req+0Dh],0            ; addressing: HSG
        mov     word ptr [req+0Eh],OFFSET cook
        mov     ax,ds
        mov     word ptr [req+10h],ax
        mov     word ptr [req+12h],1            ; one sector
        mov     word ptr [req+14h],16           ; block 16: the descriptor,
        mov     word ptr [req+16h],0            ; which every data disc has
        mov     byte ptr [req+18h],0            ; cooked
        call    sendreq
        jc      c_rawbad

        mov     al,128                          ; ...and the same one raw
        call    mkreq
        mov     byte ptr [req+0Dh],0
        mov     word ptr [req+0Eh],OFFSET raw
        mov     ax,ds
        mov     word ptr [req+10h],ax
        mov     word ptr [req+12h],1
        mov     word ptr [req+14h],16
        mov     word ptr [req+16h],0
        mov     byte ptr [req+18h],1            ; raw
        call    sendreq
        jc      c_rawno

        cmp     byte ptr [raw],0                ; the sync pattern
        jne     c_rawbad
        mov     si,OFFSET raw + 1
        mov     cx,10
c_rawsync:
        lodsb
        cmp     al,0FFh
        jne     c_rawbad
        loop    c_rawsync
        cmp     byte ptr [raw+11],0
        jne     c_rawbad

        mov     si,OFFSET raw + 16              ; the user data inside it
        mov     di,OFFSET cook
        push    ds
        pop     es
        mov     cx,2048
        cld
        repe    cmpsb
        jne     c_rawbad
        call    ok
        jmp     short c_0F
c_rawno:
        mov     dx,OFFSET msg_rawno
        call    puts
        jmp     short c_0F
c_rawbad:
        call    bad

;=====================================================================
; 150Fh - the directory entry for a path, which is the one call in the
;         MSCDEX family that asks a FILESYSTEM question.
;
; THE ROOT IS THE PATH ASKED ABOUT because every disc has one, and the
; answer is checkable without knowing what is on this disc: the flags
; byte says directory, and the extent is not zero because block zero
; of a disc is not a directory.
;
; SI IS THE SEGMENT AND DI THE OFFSET, which is the other way round
; from every other call in this family and is exactly the sort of
; thing an implementation gets backwards - so the buffer is handed
; over that way round on purpose.
;=====================================================================
c_0F:
        call    crlf
        mov     dx,OFFSET msg_0F
        call    puts
        mov     di,OFFSET path_root
        mov     si,OFFSET dirent
        call    getdirent
        jc      c_0Fbad
        test    byte ptr [dirent+12h],2         ; it is a directory
        jz      c_0Fbad
        mov     ax,word ptr [dirent+1]
        or      ax,word ptr [dirent+3]
        jz      c_0Fbad                         ; ...at a real extent
        mov     ax,word ptr [dirent+3]
        call    put4
        mov     ax,word ptr [dirent+1]
        call    put4
        call    ok
        jmp     short c_0Fno

;--- ...and a path that is not there has to be REFUSED.  A call that
;--- answers every question is not answering any of them.
c_0Fno:
        mov     dx,OFFSET msg_0Fno
        call    puts
        mov     di,OFFSET path_junk
        mov     si,OFFSET dirent
        call    getdirent
        jnc     c_0Fnobad
        cmp     ax,2                            ; file not found
        jne     c_0Fnobad
        call    ok
        jmp     short c_done
c_0Fnobad:
        call    bad
        jmp     short c_done
c_0Fbad:
        call    bad

c_done:
        call    crlf
        mov     dx,OFFSET msg_done
        call    puts
        movzx   ax,byte ptr [failures]
        call    put2d
        mov     dx,OFFSET msg_fails
        call    puts
c_bye:
        mov     ax,4C00h
        int     21h

;=====================================================================
; mkreq - AL = the command.  Zero the header and fill in the length,
;         the subunit and the command.
;=====================================================================
mkreq:
        push    ax
        push    cx
        push    di
        push    es
        push    ds
        pop     es
        mov     di,OFFSET req
        mov     cx,26
        push    ax
        xor     al,al
        cld
        rep     stosb
        pop     ax
        mov     byte ptr [req],26
        mov     byte ptr [req+1],0
        mov     byte ptr [req+2],al
        pop     es
        pop     di
        pop     cx
        pop     ax
        ret

;=====================================================================
; sendreq - hand the request to the device through 1510h.  CF set if
;           the status came back with the error bit in it.
;=====================================================================
sendreq:
        push    ax
        push    bx
        push    cx
        movzx   cx,byte ptr [drive]
        mov     bx,OFFSET req
        mov     ax,1510h
        int     2Fh
        jc      sr_bad                  ; THE CALL ITSELF WAS REFUSED, which
                                        ; is not the same thing as the
                                        ; request failing - and it leaves
                                        ; the status word exactly as this
                                        ; program set it, so a test that
                                        ; looks only at the status sees a
                                        ; success that never happened.  The
                                        ; first run of this reported "ok"
                                        ; against a buffer full of zeros for
                                        ; precisely that reason.
        mov     ax,word ptr [req+3]
        test    ax,8000h
        jnz     sr_bad
        pop     cx
        pop     bx
        pop     ax
        clc
        ret
sr_bad:
        pop     cx
        pop     bx
        pop     ax
        stc
        ret

;=====================================================================
; getdirent - DI = an ASCIIZ path, SI = the buffer for the answer.
;             CF set with AX = the DOS error when there is no such
;             thing.
;
; The registers this call wants are unlike the rest of the family:
; the DRIVE IS IN CL ALONE because CH carries flags, the path is at
; ES:BX, and the buffer is at SI:DI with SI the SEGMENT.
;=====================================================================
getdirent:
        push    bx
        push    cx
        push    dx
        push    si
        push    di
        push    es
        movzx   cx,byte ptr [drive]             ; CL = drive, CH = no flags
        mov     bx,di                           ; ES:BX = the path
        mov     di,si                           ; DI = the buffer's offset
        mov     si,ds                           ; SI = its segment
        push    ds
        pop     es
        mov     ax,150Fh
        int     2Fh
        pop     es
        pop     di
        pop     si
        pop     dx
        pop     cx
        pop     bx
        ret

;=====================================================================
; ioctl_in - AL = the control code, CX = how many bytes the answer is.
;            The block comes back in cblk.  CF on failure.
;=====================================================================
ioctl_in:
        push    ax
        push    bx
        push    cx
        push    ax
        push    cx
        mov     al,3                            ; IOCTL input
        call    mkreq
        pop     cx
        pop     ax
        mov     byte ptr [cblk],al              ; the code goes in the block
        mov     word ptr [req+0Eh],OFFSET cblk
        mov     ax,ds
        mov     word ptr [req+10h],ax
        mov     word ptr [req+12h],cx
        call    sendreq
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; qchan - the Q channel, into q_m/q_s/q_f, with the absolute position
;         also packed into q_abs so two readings can be compared.
;=====================================================================
qchan:
        push    ax
        push    cx
        push    ebx
        mov     al,12
        mov     cx,11
        call    ioctl_in
        jc      q_bad
        mov     al,[cblk+8]
        mov     [q_m],al
        mov     al,[cblk+9]
        mov     [q_s],al
        mov     al,[cblk+10]
        mov     [q_f],al
        ; THE WHOLE POSITION, IN FRAMES.  Minutes and seconds looked
        ; like enough and are not: a second of waiting moves the head
        ; by a fraction of a second, so two readings inside the same
        ; second compare equal and a drive that IS playing is reported
        ; as stopped.  The first run of this test called a working play
        ; "the position did not move" for exactly that reason - the
        ; screen showed 00:04:23 and 00:04:46 and the comparison could
        ; not see the difference between them.
        movzx   eax,byte ptr [q_m]
        imul    eax,60
        movzx   ebx,byte ptr [q_s]
        add     eax,ebx
        imul    eax,75
        movzx   ebx,byte ptr [q_f]
        add     eax,ebx
        mov     [q_abs],eax
        pop     ebx
        pop     cx
        pop     ax
        clc
        ret
q_bad:
        pop     ebx
        pop     cx
        pop     ax
        stc
        ret

putq:
        push    ax
        push    bx
        mov     al,[q_m]
        mov     ah,[q_s]
        mov     bl,[q_f]
        call    putmsf
        pop     bx
        pop     ax
        ret

;--- putmsf - AL = minutes, AH = seconds, BL = frames ----------------
putmsf:
        push    ax
        push    bx
        push    ax
        call    put2d
        mov     dl,':'
        call    putc
        pop     ax
        mov     al,ah
        call    put2d
        mov     dl,':'
        call    putc
        mov     al,bl
        call    put2d
        pop     bx
        pop     ax
        ret

;=====================================================================
; waitticks - CX ticks of the BIOS clock, which runs at 18.2 a second.
;
; THE GIVE-UP COUNT IS NOT DECORATION.  If the tick were not running
; in this box, a loop waiting for it would hang the test rather than
; fail it, and a test that hangs tells you less than one that fails.
;=====================================================================
waitticks:
        push    ax
        push    bx
        push    cx
        push    dx
        push    si
        mov     si,cx
        xor     ah,ah
        int     1Ah
        mov     bx,dx                           ; the tick we started on
        mov     cx,0FFFFh                       ; and the patience
wt_lp:
        push    cx
        xor     ah,ah
        int     1Ah
        pop     cx
        mov     ax,dx
        sub     ax,bx
        cmp     ax,si
        jae     wt_out
        loop    wt_lp
wt_out:
        pop     si
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; The three verdicts, so a missing check reads as a gap.
;=====================================================================
ok:
        push    dx
        mov     dx,OFFSET msg_ok
        call    puts
        pop     dx
        ret
bad:
        push    dx
        mov     dx,OFFSET msg_bad
        call    puts
        pop     dx
        inc     byte ptr [failures]
        ret

;=====================================================================
; Output helpers, the same ones every 16-bit test in this tree uses.
;=====================================================================
puts:
        push    ax
        mov     ah,09h
        int     21h
        pop     ax
        ret
putc:
        push    ax
        push    dx
        mov     ah,02h
        int     21h
        pop     dx
        pop     ax
        ret
crlf:
        push    dx
        mov     dx,OFFSET msg_crlf
        call    puts
        pop     dx
        ret

;--- AX in hex, four digits ------------------------------------------
put4:
        push    ax
        mov     al,ah
        call    put2
        pop     ax
put2:
        push    ax
        shr     al,4
        call    put1
        pop     ax
put1:
        push    ax
        and     al,0Fh
        add     al,'0'
        cmp     al,'9'
        jbe     p1_go
        add     al,7
p1_go:
        mov     dl,al
        call    putc
        pop     ax
        ret

;--- AL in decimal, two digits ---------------------------------------
put2d:
        push    ax
        push    bx
        push    cx
        push    dx
        movzx   ax,al
        mov     bx,10
        xor     cx,cx
d2_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     d2_div
        cmp     cx,2
        jae     d2_put
        push    cx
        mov     dl,'0'
        call    putc
        pop     cx
d2_put:
        pop     ax
        add     al,'0'
        mov     dl,al
        call    putc
        loop    d2_put
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;--- AX in decimal, up to five digits --------------------------------
put5d:
        push    ax
        push    bx
        push    cx
        push    dx
        mov     bx,10
        xor     cx,cx
d5_div:
        xor     dx,dx
        div     bx
        push    dx
        inc     cx
        or      ax,ax
        jnz     d5_div
d5_put:
        pop     ax
        add     al,'0'
        mov     dl,al
        call    putc
        loop    d5_put
        pop     dx
        pop     cx
        pop     bx
        pop     ax
        ret

;=====================================================================
; Data
;=====================================================================
drive       db  0
trk_lo      db  0
trk_hi      db  0
trk         db  0
play_trk    db  0
play_m      db  0
play_s      db  0
play_f      db  0
failures    db  0
q_m         db  0
q_s         db  0
q_f         db  0
q_abs       dd  0                   ; the head's position, in frames
q_first     dd  0                   ; ...and what it was a moment ago
hdrseg      dw  0
hdroff      dw  0
fstrat      dd  0
fintr       dd  0
letters     db  16 dup(0)
drvlist     db  32 dup(0)
req         db  32 dup(0)
cblk        db  32 dup(0)
path_root   db  '\',0                   ; every disc has one of these
path_junk   db  '\NO SUCH.XYZ',0        ; and no disc has one of these
dirent      db  255 dup(0)              ; what 150Fh writes
cook        db  2048 dup(0)             ; one sector, read the ordinary way
raw         db  2352 dup(0)             ; ...and the frame it came out of

msg_hi      db  'CDATEST - the MSCD001 device and CD audio, from a DOS box'
            db  13,10,13,10,'$'
msg_crlf    db  13,10,'$'
msg_ok      db  ' ok',13,10,'$'
msg_bad     db  ' FAILED',13,10,'$'
msg_nodrv   db  'BX came back zero: no CD-ROM driver is installed.',13,10,'$'
msg_drive   db  'drive: $'
msg_1501    db  '1501h  driver list says MSCD001: $'
msg_mscd    db  'MSCD001 '
msg_stat    db  '15h/06  device status:    $'
msg_secsz   db  '15h/07  sector size:      $'
msg_disc    db  '15h/10  tracks:           $'
msg_leadout db  '  lead-out $'
msg_track   db  '  track $'
msg_data    db  'data$'
msg_audio   db  'audio$'
msg_noaudio db  'no audio track on this disc - play not tested',13,10,'$'
msg_play    db  'play    audio track      $'
msg_q1      db  '15h/12  Q channel:        $'
msg_q2      db  '15h/12  ...a second later:$'
msg_notmoving db ' FAILED - the position did not move',13,10,'$'
msg_busy    db  'status  busy while playing:$'
msg_stop    db  '133     stop, then paused:$'
msg_resume  db  '136     resume, and moving:$'
msg_stop2   db  '133x2   stopped for good: $'
msg_direct  db  'device  strategy+interrupt called directly: $'
msg_upc     db  '15h/14  catalogue number: $'
msg_raw     db  '128     raw frame vs cooked sector: $'
msg_rawno   db  ' this drive has no READ CD - skipped',13,10,'$'
msg_0F      db  '150Fh   the root, extent:  $'
msg_0Fno    db  '150Fh   a path that is not there: $'
msg_done    db  'CDATEST done, $'
msg_fails   db  ' failure(s).',13,10,'$'

_CX     ENDS
        END
