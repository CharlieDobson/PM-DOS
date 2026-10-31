/*
 * FLOAT.H - the floating-point types
 *
 * long double is the x87's 80-bit format under the GNU compilers and
 * the same as double under Open Watcom.
 */
#ifndef _FLOAT_H
#define _FLOAT_H

#define FLT_RADIX       2
#define FLT_ROUNDS      1
#define FLT_EVAL_METHOD 2               /* the x87 works in long double */
#define DECIMAL_DIG     21

#define FLT_MANT_DIG    24
#define FLT_DIG         6
#define FLT_MIN_EXP     (-125)
#define FLT_MIN_10_EXP  (-37)
#define FLT_MAX_EXP     128
#define FLT_MAX_10_EXP  38
#define FLT_MAX         3.40282346638528859812e+38F
#define FLT_EPSILON     1.1920928955078125e-07F
#define FLT_MIN         1.17549435082228750797e-38F
#define FLT_TRUE_MIN    1.40129846432481707092e-45F
#define FLT_DECIMAL_DIG 9

#define DBL_MANT_DIG    53
#define DBL_DIG         15
#define DBL_MIN_EXP     (-1021)
#define DBL_MIN_10_EXP  (-307)
#define DBL_MAX_EXP     1024
#define DBL_MAX_10_EXP  308
#define DBL_MAX         1.79769313486231570815e+308
#define DBL_EPSILON     2.22044604925031308085e-16
#define DBL_MIN         2.22507385850720138309e-308
#define DBL_TRUE_MIN    4.94065645841246544177e-324
#define DBL_DECIMAL_DIG 17

#if defined( __SIZEOF_LONG_DOUBLE__ ) && __SIZEOF_LONG_DOUBLE__ > 8
#define LDBL_MANT_DIG   64
#define LDBL_DIG        18
#define LDBL_MIN_EXP    (-16381)
#define LDBL_MIN_10_EXP (-4931)
#define LDBL_MAX_EXP    16384
#define LDBL_MAX_10_EXP 4932
#define LDBL_MAX        1.18973149535723176502e+4932L
#define LDBL_EPSILON    1.08420217248550443401e-19L
#define LDBL_MIN        3.36210314311209350626e-4932L
#define LDBL_TRUE_MIN   3.64519953188247460253e-4951L
#define LDBL_DECIMAL_DIG 21
#else
#define LDBL_MANT_DIG   DBL_MANT_DIG
#define LDBL_DIG        DBL_DIG
#define LDBL_MIN_EXP    DBL_MIN_EXP
#define LDBL_MIN_10_EXP DBL_MIN_10_EXP
#define LDBL_MAX_EXP    DBL_MAX_EXP
#define LDBL_MAX_10_EXP DBL_MAX_10_EXP
#define LDBL_MAX        DBL_MAX
#define LDBL_EPSILON    DBL_EPSILON
#define LDBL_MIN        DBL_MIN
#define LDBL_TRUE_MIN   DBL_TRUE_MIN
#define LDBL_DECIMAL_DIG DBL_DECIMAL_DIG
#endif

#endif
