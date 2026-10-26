/*
 * STRING.H - XARC's C library: memory and string functions (LIBC.C).
 * stricmp and the like fold only A-Z, which is what DOS file names
 * compare by.
 */
#ifndef _XA_STRING_H
#define _XA_STRING_H

#include <stddef.h>

void  *memcpy( void *dst, const void *src, size_t len );
void  *memmove( void *dst, const void *src, size_t len );
void  *memset( void *dst, int val, size_t len );
int    memcmp( const void *lhs, const void *rhs, size_t len );
void  *memchr( const void *buf, int ch, size_t len );

size_t strlen( const char *text );
char  *strcpy( char *dst, const char *src );
char  *strncpy( char *dst, const char *src, size_t max );
char  *strcat( char *dst, const char *src );
char  *strncat( char *dst, const char *src, size_t max );
int    strcmp( const char *lhs, const char *rhs );
int    strncmp( const char *lhs, const char *rhs, size_t max );
int    stricmp( const char *lhs, const char *rhs );
int    strnicmp( const char *lhs, const char *rhs, size_t max );
char  *strchr( const char *text, int ch );
char  *strrchr( const char *text, int ch );
char  *strstr( const char *hay, const char *needle );
char  *strdup( const char *text );
char  *strupr( char *text );
char  *strlwr( char *text );

#define _stricmp    stricmp
#define _strnicmp   strnicmp
#define _strdup     strdup
#define _strupr     strupr
#define _strlwr     strlwr

#endif
