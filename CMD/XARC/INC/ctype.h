/*
 * CTYPE.H - XARC's C library: character classes, for the 128 ASCII
 * characters only.  A byte above 127 is in none of the classes and is
 * not changed by toupper or tolower - code-page letters are left as
 * they are, which is what a DOS file name wants.
 */
#ifndef _XA_CTYPE_H
#define _XA_CTYPE_H

int isalpha( int ch );
int isdigit( int ch );
int isalnum( int ch );
int isspace( int ch );
int isupper( int ch );
int islower( int ch );
int isxdigit( int ch );
int isprint( int ch );
int iscntrl( int ch );
int ispunct( int ch );
int toupper( int ch );
int tolower( int ch );

#endif
