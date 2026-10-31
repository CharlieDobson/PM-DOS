/*
 * STDBOOL.H - bool, true and false
 */
#ifndef _STDBOOL_H
#define _STDBOOL_H

#ifndef __cplusplus
#if defined( __GNUC__ ) || (defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 199901L)
#define bool    _Bool
#else
#define bool    unsigned char
#endif
#define true    1
#define false   0
#endif
#define __bool_true_false_are_defined   1

#endif
