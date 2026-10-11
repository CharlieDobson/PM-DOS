;=====================================================================
; D32STUB.ASM - D32STUB.EXE, the MS-DOS program in front of a D32 image
;
; A D32 command is two programs in one file: this one, and behind it
; the 32-bit image that PM-DOS loads.  Real MS-DOS sees 'MZ', loads
; what the header describes - these 128 bytes and no more, however
; large the file is - and runs it.  It says what the file needs and
; ends with errorlevel 1.  PM-DOS reads the dword at 3Ch, finds 'CD'
; where it points, and never executes a byte of this.
;
; TOOLS\D32BIND puts it in front of every command BUILD.CMD ships, and
; writes the real offset at 3Ch when it does.  A program that is to
; run on both systems names its own 16-bit .EXE to D32BIND in place of
; this one.
;
; THE HEADER IS WRITTEN OUT BY HAND and the file is linked as a flat
; binary, so that its layout is the one the kernel is told about in
; PMDOS.INC and not whatever a linker's "system dos" happens to emit:
; no fixups, the fixup table's offset at 40h (which is what says there
; IS a 3Ch), and the code straight after the header.
;
; The stack is the paragraph count at 0Ah, above the load module: 64
; bytes of image and 256 of stack, so SS:SP is 0000:0140h from the
; load segment.
;
; Linked with:  wlink form raw bin option offset=0x0
;=====================================================================
        .8086

STUB_HDR        EQU     40h             ; the header: four paragraphs
STUB_LEN        EQU     80h             ; all of it, and where the image goes

_STUB   SEGMENT USE16 PUBLIC 'CODE'
        ASSUME  CS:_STUB, DS:_STUB
        ORG     0

stub_start:
        db      'M', 'Z'
        dw      STUB_LEN                ; 02h bytes in the last page
        dw      1                       ; 04h pages, that one included
        dw      0                       ; 06h fixups: none
        dw      STUB_HDR / 16           ; 08h header, in paragraphs
        dw      10h                     ; 0Ah paragraphs past the image: the stack
        dw      0FFFFh                  ; 0Ch ...and wanted, as every linker says
        dw      0                       ; 0Eh SS, from the load segment
        dw      STUB_LEN - STUB_HDR + 100h      ; 10h SP
        dw      0                       ; 12h checksum
        dw      0                       ; 14h IP
        dw      0                       ; 16h CS, from the load segment
        dw      STUB_HDR                ; 18h the fixup table: 40h, so 3Ch counts
        dw      0                       ; 1Ah overlay number
        db      32 dup(0)               ; 1Ch reserved
        dd      STUB_LEN                ; 3Ch where the D32 image starts

; CS is the load segment and DS is the PSP, as for any .EXE.
stub_code:
        push    cs
        pop     ds
        mov     dx,OFFSET stub_msg - STUB_HDR
        mov     ah,09h
        int     21h
        mov     ax,4C01h
        int     21h

stub_msg        db      13,10,'This application requires 32-bit DOS mode.',13,10,'$'

        ORG     STUB_LEN - 1
        db      0
_STUB   ENDS
        END
