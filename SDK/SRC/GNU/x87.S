/*
 * X87.S - the coprocessor instructions MATH.H is built on, for GCC,
 * Clang and DJGPP.  WATCOM\X87.ASM is the same for Open Watcom, where
 * the list of functions is.
 *
 * cdecl: arguments on the stack, the result in ST(0).  long double is
 * the x87's 80-bit format here and takes 12 bytes on the stack, so the
 * floating arguments are at 4(%esp) and 16(%esp), and an int after one
 * at 16(%esp).
 */
#define CAT2( a, b )    a ## b
#define CAT( a, b )     CAT2( a, b )
#define CSYM( name )    CAT( __USER_LABEL_PREFIX__, name )
#define FUNC( name )    .globl CSYM( name ) ; CSYM( name ):

        .text

FUNC( __pm_xsqrt )
        fldt    4(%esp)
        fsqrt
        ret

/* reduce %st(0) by 2 pi until FSIN, FCOS and FPTAN take it */
reduce:
        fldpi
        fadd    %st(0), %st
        fxch    %st(1)
1:      fprem1
        fnstsw  %ax
        testb   $4, %ah
        jnz     1b
        fstp    %st(1)
        ret

FUNC( __pm_xsin )
        fldt    4(%esp)
        fsin
        fnstsw  %ax
        testb   $4, %ah
        jz      1f
        call    reduce
        fsin
1:      ret

FUNC( __pm_xcos )
        fldt    4(%esp)
        fcos
        fnstsw  %ax
        testb   $4, %ah
        jz      1f
        call    reduce
        fcos
1:      ret

FUNC( __pm_xtan )
        fldt    4(%esp)
        fptan
        fnstsw  %ax
        testb   $4, %ah
        jz      1f
        call    reduce
        fptan
1:      fstp    %st(0)                  /* FPTAN pushes a 1 */
        ret

/* sin, cos, tan or 1/tan of (a + b) pi/2, by op 0 to 3: the far
   reduction's r.  op is at 28(%esp), after the two long doubles */
FUNC( __pm_xtrigq )
        fldt    4(%esp)                 /* a */
        fldt    16(%esp)
        faddp                           /* a + b */
        fldpi
        fmulp                           /* (a + b) pi */
        fld1
        fchs
        fxch    %st(1)
        fscale                          /* (a + b) pi/2, -1 */
        fstp    %st(1)
        mov     28(%esp), %eax          /* op */
        cmp     $1, %eax
        je      2f
        ja      3f
        fsin
        ret
2:      fcos
        ret
3:      fptan                           /* 1, tan */
        cmp     $2, %eax
        jne     4f
        fstp    %st(0)
        ret
4:      fdiv    %st(1), %st             /* 1 / tan, tan */
        fstp    %st(1)
        ret

FUNC( __pm_xatan2 )
        fldt    4(%esp)                 /* y */
        fldt    16(%esp)                /* x */
        fpatan
        ret

FUNC( __pm_xyl2x )
        fldt    16(%esp)                /* y */
        fldt    4(%esp)                 /* x */
        fyl2x
        ret

FUNC( __pm_xyl2xp1 )
        fldt    16(%esp)
        fldt    4(%esp)
        fyl2xp1
        ret

/* x^y = 2^(y log2 x) for x > 0, without leaving the registers: the
   64-bit y log2 x would lose its last bits as a double */
FUNC( __pm_xpow )
        fldt    16(%esp)                /* y */
        fldt    4(%esp)                 /* x */
        fyl2x                           /* t = y log2 x */
        fld     %st(0)
        frndint                         /* n, t */
        fxch    %st(1)
        fsub    %st(1), %st             /* f = t - n, n */
        f2xm1
        fld1
        faddp                           /* 2^f, n */
        fscale
        fstp    %st(1)
        ret

FUNC( __pm_x2xm1 )
        fldt    4(%esp)
        f2xm1
        ret

FUNC( __pm_xscale )
        fldt    16(%esp)                /* n */
        fldt    4(%esp)                 /* x */
        fscale
        fstp    %st(1)
        ret

FUNC( __pm_xrndint )
        sub     $4, %esp                /* (%esp) the old control word, */
        fnstcw  (%esp)                  /* 2(%esp) the one for the mode */
        movw    (%esp), %ax
        andb    $0xF3, %ah
        mov     20(%esp), %ecx          /* the mode, after x (+12) and the 4 */
        and     $3, %ecx
        shl     $10, %ecx
        or      %cx, %ax
        movw    %ax, 2(%esp)
        fldcw   2(%esp)
        fldt    8(%esp)
        frndint
        fldcw   (%esp)
        add     $4, %esp
        ret

FUNC( __pm_xprem )
        fldt    16(%esp)                /* y */
        fldt    4(%esp)                 /* x */
1:      fprem
        fnstsw  %ax
        testb   $4, %ah
        jnz     1b
        fstp    %st(1)
        ret

FUNC( __pm_xprem1 )
        fldt    16(%esp)
        fldt    4(%esp)
1:      fprem1
        fnstsw  %ax
        testb   $4, %ah
        jnz     1b
        fstp    %st(1)
        ret
