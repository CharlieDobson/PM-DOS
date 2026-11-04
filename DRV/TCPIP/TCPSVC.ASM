;=====================================================================
; TCPSVC.ASM - two kernel services for TCPIP.SYS that NDSTART.ASM does
; not bridge: NDS_READFILE, which reads TCPIP.INI and the trust bundle
; at INIT, and NDS_STAMP, the date and time a certificate is checked
; against.
;
; They are here and not in NDSTART.ASM because every C driver links
; NDSTART, and a bridge there would change every one of them for a
; service only this one calls.  NDSTART makes its services pointer
; public for this.  Same seam as NDSTART: C calls in, cdecl, and the
; kernel's register contract out (INC\NDRV.INC).
;=====================================================================
        .386p
        .model  flat

        include pmdos.inc
        include ndrv.inc

        extrn   nds_services:dword

        .code

        public  _nds_readfile, _nds_stamp

;--- int nds_readfile( const char *path, void *buf, u32 room, u32 *got,
;---                   u32 *size )
;--- 0, *got = bytes read and *size = the file's whole size; else ERR_*.
;--- ERR_FUNC from a kernel older than NDS_VERSION 7, or after INIT
_nds_readfile:
        push    ebx
        push    esi
        push    edi
        mov     eax,[nds_services]
        cmp     word ptr [eax+NDS_VER],7        ; NDS_VERSION 7 brought it
        jb      nrf_old
        mov     edx,[esp+16]                    ; path
        mov     edi,[esp+20]                    ; buf
        mov     ecx,[esp+24]                    ; room
        call    dword ptr [eax+NDS_READFILE]    ; -> EAX got, ECX size
        jc      nrf_err
        mov     edx,[esp+28]
        mov     [edx],eax                       ; *got
        mov     edx,[esp+32]
        mov     [edx],ecx                       ; *size
        xor     eax,eax
        jmp     short nrf_out
nrf_old:
        mov     eax,ERR_FUNC
        jmp     short nrf_out
nrf_err:
        movzx   eax,ax
nrf_out:
        pop     edi
        pop     esi
        pop     ebx
        ret

;--- u32 nds_stamp( void ) - now, DOS-packed: the date in the high word,
;--- the time in the low; 0 from a kernel older than NDS_VERSION 4
_nds_stamp:
        mov     edx,[nds_services]
        xor     eax,eax
        cmp     word ptr [edx+NDS_VER],4        ; NDS_VERSION 4 brought it
        jb      nst_out
        call    dword ptr [edx+NDS_STAMP]
nst_out:
        ret

        end
