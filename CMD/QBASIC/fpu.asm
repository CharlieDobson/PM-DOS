;
; FPU.ASM - the coprocessor's own instructions for QBASIC, which links
; no C library.  Open Watcom WASM (and MASM 6.1x).
;
; Every routine is __cdecl: a double argument is eight bytes on the
; stack, and a double result comes back in ST(0), as Watcom's code
; compiled with -fpi87 expects of a function declared __cdecl.
;
; THE CONTROL WORD is 037Fh throughout - every exception masked, 64-bit
; precision, rounding to the nearest - and a routine that needs another
; rounding sets it, does its one instruction, and puts it back.  The
; interpreter finds an overflow by looking at what came out (QB.H's
; DBL_BAD), not by taking an exception: a program at ring 3 has nowhere
; to take one to.
;
; __CHP is the routine Watcom's code calls before FISTP when C converts
; a double to an integer: ST(0) is cut towards zero, in place.  It is
; the only thing the compiler wants from a library, so it is here.
;
        .386
        .model  flat

        .data
cw_near         dw      037Fh           ; masked, 64-bit, to the nearest
cw_down         dw      077Fh           ; ...towards minus infinity
cw_chop         dw      0F7Fh           ; ...towards zero
one             dq      1.0

; Watcom's -fpi87 code names __8087, the C library's note of which
; coprocessor there is; QBASIC will not start without one (MAIN.C)
                public  __8087
__8087          db      3               ; a 387 or better

        .code

; void fp_init( void ) - a coprocessor in the state the rest assumes
        public  _fp_init
_fp_init proc   near
        fninit
        fldcw   cw_near
        ret
_fp_init endp

; double fp_floor( double v )
        public  _fp_floor
_fp_floor proc  near
        fld     qword ptr [esp+4]
        fldcw   cw_down
        frndint
        fldcw   cw_near
        ret
_fp_floor endp

; double fp_trunc( double v )
        public  _fp_trunc
_fp_trunc proc  near
        fld     qword ptr [esp+4]
        fldcw   cw_chop
        frndint
        fldcw   cw_near
        ret
_fp_trunc endp

; double fp_round( double v ) - to the nearest, a half to the even one,
; which is what CINT and CLNG do
        public  _fp_round
_fp_round proc  near
        fld     qword ptr [esp+4]
        frndint
        ret
_fp_round endp

; double fp_powi( double base, int n ) - base^n for a whole n, multiplied
; out in the coprocessor's 64 bits, the bits of n from the lowest, and
; 1 over that for a negative n.  QBasic's own ^ does this: 10 ^ 39 comes
; out a hair under 1D+39, as QBasic shows it.
        public  _fp_powi
_fp_powi proc   near
        mov     ecx,[esp+12]            ; n
        fld1                            ; the result so far
        fld     qword ptr [esp+4]       ; base, result
        mov     edx,ecx
        or      edx,edx
        jns     pw_loop
        neg     edx
pw_loop:
        or      edx,edx
        jz      pw_done
        test    edx,1
        jz      pw_square
        fmul    st(1),st                ; result times base
pw_square:
        shr     edx,1
        jz      pw_done
        fmul    st,st                   ; base squared
        jmp     pw_loop
pw_done:
        fstp    st                      ; the result
        or      ecx,ecx
        jns     pw_ret
        fdivr   one                     ; 1 / result
pw_ret:
        ret
_fp_powi endp

; WINDOW's sums, worked in the coprocessor's 64 bits as QBasic's are: a
; double in C would round between the steps, and a point that QBasic
; puts at 159 (50 * 3.19, a hair under 159.5) would land on 160.

; double fp_scale( double a, double b, double f ) - (a - b) * f
        public  _fp_scale
_fp_scale proc  near
        fld     qword ptr [esp+4]
        fsub    qword ptr [esp+12]
        fmul    qword ptr [esp+20]
        ret
_fp_scale endp

; double fp_scale_round( double a, double b, double f ) - (a - b) * f
; to the nearest whole number
        public  _fp_scale_round
_fp_scale_round proc near
        fld     qword ptr [esp+4]
        fsub    qword ptr [esp+12]
        fmul    qword ptr [esp+20]
        frndint
        ret
_fp_scale_round endp

; double fp_offset( double a, double b, double g ) - a + b * g
        public  _fp_offset
_fp_offset proc near
        fld     qword ptr [esp+12]
        fmul    qword ptr [esp+20]
        fadd    qword ptr [esp+4]
        ret
_fp_offset endp

; The trigonometric instructions take arguments below 2^63 only, and
; say so in C2 when one is not: it is brought into range by the
; remainder after 2*pi, as many times as that takes.
reduce  proc    near                    ; ST(0) = the argument
        fldpi
        fadd    st,st                   ; 2*pi, x
        fxch
red_more:
        fprem1
        fnstsw  ax
        sahf
        jp      red_more                ; C2 set: not finished
        fstp    st(1)
        ret
reduce  endp

        public  _fp_sin
_fp_sin proc    near
        fld     qword ptr [esp+4]
        fsin
        fnstsw  ax
        sahf
        jnp     sin_done
        call    reduce
        fsin
sin_done:
        ret
_fp_sin endp

        public  _fp_cos
_fp_cos proc    near
        fld     qword ptr [esp+4]
        fcos
        fnstsw  ax
        sahf
        jnp     cos_done
        call    reduce
        fcos
cos_done:
        ret
_fp_cos endp

; FPTAN leaves the tangent and then 1.0 on top of it
        public  _fp_tan
_fp_tan proc    near
        fld     qword ptr [esp+4]
        fptan
        fnstsw  ax
        sahf
        jnp     tan_done
        call    reduce
        fptan
tan_done:
        fstp    st(0)
        ret
_fp_tan endp

        public  _fp_atn
_fp_atn proc    near
        fld     qword ptr [esp+4]
        fld1
        fpatan
        ret
_fp_atn endp

; 2^ST(0), for any ST(0): F2XM1 takes only -1..1, so the whole part goes
; to FSCALE and the fraction to F2XM1
exp2    proc    near
        fld     st                      ; x, x
        frndint                         ; n, x
        fsub    st(1),st                ; n, f
        fxch                            ; f, n
        f2xm1                           ; 2^f - 1, n
        fld1
        faddp   st(1),st                ; 2^f, n
        fscale                          ; 2^f * 2^n, n
        fstp    st(1)
        ret
exp2    endp

; double fp_exp( double v ) - e^v = 2^(v * log2(e))
        public  _fp_exp
_fp_exp proc    near
        fld     qword ptr [esp+4]
        fldl2e
        fmulp   st(1),st
        call    exp2
        ret
_fp_exp endp

; double fp_log( double v ) - ln(v) = ln(2) * log2(v); v > 0
        public  _fp_log
_fp_log proc    near
        fldln2
        fld     qword ptr [esp+4]
        fyl2x
        ret
_fp_log endp

        public  _fp_sqrt
_fp_sqrt proc   near
        fld     qword ptr [esp+4]
        fsqrt
        ret
_fp_sqrt endp

; double fp_pow( double base, double power ) - base > 0: 2^(power *
; log2(base)).  The signs and the zeros are FP's caller's (NUM.C).
        public  _fp_pow
_fp_pow proc    near
        fld     qword ptr [esp+12]      ; power
        fld     qword ptr [esp+4]       ; base, power
        fyl2x                           ; power * log2(base)
        call    exp2
        ret
_fp_pow endp

; __CHP - ST(0) cut towards zero, in place; every register kept
        public  __CHP
__CHP   proc    near
        fldcw   cw_chop
        frndint
        fldcw   cw_near
        ret
__CHP   endp

        end
