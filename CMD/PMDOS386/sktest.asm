;=====================================================================
; SKTEST.ASM - SKTEST.COM, a 16-bit DOS program that proves the network
;   redirector's SEEK-FROM-END (INT 2Fh 1121h) under Windows for
;   Workgroups 3.11.  DOS asks the redirector where a file ends only for
;   one opened deny-none or deny-read (RMFILE2.INC fn42), so this opens
;   N:\README.TXT deny-none, seeks to its end (which must be the file's
;   size), then seeks to end-10 and reads the last ten bytes, and writes
;   a 14-byte record to N:\SEEKOUT.BIN for the host to check:
;       [0..3]  the seek-from-end position (little-endian) - must be 57
;       [4..13] README.TXT's last ten bytes
;
;   The open/read and the create/write go through the bridge to the
;   parked kernel's SMBFS; the seek itself RMDOS answers from the entry's
;   size, as the native kernel does (INT21.INC f42_end), so the server's
;   n2_seekend makes no bridge call.  Any failed step exits 1 and leaves
;   SEEKOUT.BIN uncreated, so the file's presence and contents are proof.
;
; Linked with:  wlink form raw bin order clname CODE offset=0x100
;=====================================================================
        .386
_T      SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_T, DS:_T, ES:_T, SS:_T
        ORG     0

start:
        ; open N:\README.TXT, mode = deny-none (40h) OR read (0)
        mov     ax,3D40h
        mov     dx,OFFSET srcname
        int     21h
        jc      fail
        mov     [hsrc],ax

        ; seek to end (origin 2, offset 0) -> DX:AX = the file's end
        mov     bx,[hsrc]
        xor     cx,cx
        xor     dx,dx
        mov     ax,4202h
        int     21h
        jc      failc
        mov     word ptr [result+0],ax
        mov     word ptr [result+2],dx

        ; seek to end-10 (origin 2, offset -10 = 0FFFFFFF6h)
        mov     bx,[hsrc]
        mov     cx,0FFFFh
        mov     dx,0FFF6h
        mov     ax,4202h
        int     21h
        jc      failc

        ; read the ten bytes there into result+4
        mov     bx,[hsrc]
        mov     cx,10
        mov     dx,OFFSET result
        add     dx,4
        mov     ah,3Fh
        int     21h
        jc      failc

        ; close the source
        mov     bx,[hsrc]
        mov     ah,3Eh
        int     21h

        ; create N:\SEEKOUT.BIN and write the 14-byte record
        mov     ah,3Ch
        xor     cx,cx
        mov     dx,OFFSET outname
        int     21h
        jc      fail
        mov     [hout],ax
        mov     bx,[hout]
        mov     cx,14
        mov     dx,OFFSET result
        mov     ah,40h
        int     21h
        mov     bx,[hout]
        mov     ah,3Eh
        int     21h

        mov     ax,4C00h
        int     21h
failc:
        mov     bx,[hsrc]               ; close the source before leaving
        mov     ah,3Eh
        int     21h
fail:
        mov     ax,4C01h
        int     21h

srcname db      'N:\README.TXT',0
outname db      'N:\SEEKOUT.BIN',0
hsrc    dw      0
hout    dw      0
result  db      14 dup(0)

_T      ENDS
        END     start
