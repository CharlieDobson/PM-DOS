;
; X87.ASM - the coprocessor instructions MATH.H is built on, for Open
; Watcom.  WASM or MASM 6.1x.  GNU\X87.S is the same for the others.
;
; Each is __cdecl: arguments on the stack, the result in ST(0), and
; the caller pops.  Under Open Watcom long double is double, so every
; floating argument is 8 bytes.
;
;   __pm_xsqrt( x )             sqrt x
;   __pm_xsin( x ), __pm_xcos( x ), __pm_xtan( x )
;                               reduced by 2 pi first when FSIN says x
;                               is out of its range (2^63)
;   __pm_xtrigq( a, b, op )     sin, cos, tan or 1/tan (op 0-3) of
;                               (a + b) pi/2, a + b in 64 bits
;   __pm_xatan2( y, x )         FPATAN: atan(y / x), in the right quadrant
;   __pm_xyl2x( x, y )          y * log2 x
;   __pm_xyl2xp1( x, y )        y * log2 (1 + x), |x| < 0.29
;   __pm_x2xm1( x )             2^x - 1, |x| <= 1
;   __pm_xscale( x, n )         x * 2^n, n an integer
;   __pm_xrndint( x, mode )     x rounded: mode 0 nearest, 1 down, 2 up,
;                               3 toward zero
;   __pm_xprem( x, y )          FPREM to the end: fmod
;   __pm_xprem1( x, y )         FPREM1 to the end: the IEEE remainder
;
        .386p
        .model  flat


        .code

        public  ___pm_xsqrt
___pm_xsqrt proc near
        fld     qword ptr [esp+4]
        fsqrt
        ret
___pm_xsqrt endp

; reduce ST(0) by 2 pi until FSIN/FCOS/FPTAN take it
reduce  proc    near
        fldpi
        fadd    st,st(0)                ; 2 pi
        fxch    st(1)
rd_more:
        fprem1
        fnstsw  ax
        test    ah,4                    ; C2: not finished
        jnz     rd_more
        fstp    st(1)
        ret
reduce  endp

        public  ___pm_xsin
___pm_xsin proc near
        fld     qword ptr [esp+4]
        fsin
        fnstsw  ax
        test    ah,4
        jz      xsin_done
        call    reduce
        fsin
xsin_done:
        ret
___pm_xsin endp

        public  ___pm_xcos
___pm_xcos proc near
        fld     qword ptr [esp+4]
        fcos
        fnstsw  ax
        test    ah,4
        jz      xcos_done
        call    reduce
        fcos
xcos_done:
        ret
___pm_xcos endp

        public  ___pm_xtan
___pm_xtan proc near
        fld     qword ptr [esp+4]
        fptan
        fnstsw  ax
        test    ah,4
        jz      xtan_done
        call    reduce
        fptan
xtan_done:
        fstp    st(0)                   ; FPTAN pushes a 1
        ret
___pm_xtan endp

; sin, cos, tan or 1/tan of (a + b) pi/2, by op 0 to 3: the far
; reduction's r, whose two parts only make 64 bits once added here
        public  ___pm_xtrigq
___pm_xtrigq proc near
        fld     qword ptr [esp+4]       ; a
        fadd    qword ptr [esp+12]      ; a + b
        fldpi
        fmulp   st(1),st                ; (a + b) pi
        fld1
        fchs
        fxch    st(1)
        fscale                          ; (a + b) pi/2, -1
        fstp    st(1)
        mov     eax,[esp+20]            ; op
        cmp     eax,1
        je      xtq_cos
        ja      xtq_tan
        fsin
        ret
xtq_cos:
        fcos
        ret
xtq_tan:
        fptan                           ; 1, tan
        cmp     eax,2
        jne     xtq_cot
        fstp    st(0)
        ret
xtq_cot:
        fdiv    st,st(1)                ; 1 / tan, tan
        fstp    st(1)
        ret
___pm_xtrigq endp

        public  ___pm_xatan2
___pm_xatan2 proc near
        fld     qword ptr [esp+4]                    ; y
        fld     qword ptr [esp+12]                    ; x
        fpatan
        ret
___pm_xatan2 endp

        public  ___pm_xyl2x
___pm_xyl2x proc near
        fld     qword ptr [esp+12]                    ; y
        fld     qword ptr [esp+4]                    ; x
        fyl2x
        ret
___pm_xyl2x endp

        public  ___pm_xyl2xp1
___pm_xyl2xp1 proc near
        fld     qword ptr [esp+12]
        fld     qword ptr [esp+4]
        fyl2xp1
        ret
___pm_xyl2xp1 endp

; x^y = 2^(y log2 x) for x > 0, without leaving the registers: the
; 64-bit y log2 x would lose its last bits as a double
        public  ___pm_xpow
___pm_xpow proc near
        fld     qword ptr [esp+12]      ; y
        fld     qword ptr [esp+4]       ; x
        fyl2x                           ; t = y log2 x
        fld     st(0)
        frndint                         ; n, t
        fxch    st(1)
        fsub    st,st(1)                ; f = t - n, n
        f2xm1
        fld1
        faddp   st(1),st                ; 2^f, n
        fscale
        fstp    st(1)
        ret
___pm_xpow endp

        public  ___pm_x2xm1
___pm_x2xm1 proc near
        fld     qword ptr [esp+4]
        f2xm1
        ret
___pm_x2xm1 endp

        public  ___pm_xscale
___pm_xscale proc near
        fld     qword ptr [esp+12]                    ; n
        fld     qword ptr [esp+4]                    ; x
        fscale
        fstp    st(1)
        ret
___pm_xscale endp

        public  ___pm_xrndint
___pm_xrndint proc near
        sub     esp,4                   ; [ESP] the old control word,
        fnstcw  word ptr [esp]          ; [ESP+2] the one for the mode
        mov     ax,word ptr [esp]
        and     ah,0F3h
        mov     ecx,[esp+16]            ; the mode, after x (+8) and the 4
        and     ecx,3
        shl     ecx,10
        or      ax,cx
        mov     word ptr [esp+2],ax
        fldcw   word ptr [esp+2]
        fld     qword ptr [esp+8]
        frndint
        fldcw   word ptr [esp]
        add     esp,4
        ret
___pm_xrndint endp

        public  ___pm_xprem
___pm_xprem proc near
        fld     qword ptr [esp+12]                    ; y
        fld     qword ptr [esp+4]                    ; x
xprem_more:
        fprem
        fnstsw  ax
        test    ah,4
        jnz     xprem_more
        fstp    st(1)
        ret
___pm_xprem endp

        public  ___pm_xprem1
___pm_xprem1 proc near
        fld     qword ptr [esp+12]
        fld     qword ptr [esp+4]
xprem1_more:
        fprem1
        fnstsw  ax
        test    ah,4
        jnz     xprem1_more
        fstp    st(1)
        ret
___pm_xprem1 endp

        end
