/*
 * SOCKLIB.H - the shared runtime for the PM-DOS network clients
 *
 * FTP, TELNET, WGET and SNTP are each a small C program over this: the
 * socket API on 21h/F1h AL=26h (INC\SOCKET.H) wrapped so the waiting
 * and the timeouts live here and not in every client, plus the console,
 * the clock and the command tail they all need.  Linked behind
 * CMD\NET\NETSTART.ASM with CMD\NET\SOCKLIB.C.
 *
 * A TIMEOUT is in hundredths of a second (21h/2Ch counts ticks on
 * PM-DOS); 0 means do not wait, just do what can be done at once.  An
 * ADDRESS is a u32, most significant octet first (INC\SOCKET.H).
 */
#ifndef SOCKLIB_H
#define SOCKLIB_H

#include "ndrv.h"
#include "socket.h"

#define SL_TRUE   1
#define SL_FALSE  0
#ifndef NULL
#define NULL      ((void *)0)
#endif

void __cdecl int21( REGS *regs );

/* ---- the clock, hundredths of a second ---- */
u32  sl_now( void );
u32  sl_since( u32 start );        /* hundredths since sl_now() returned start */
void sl_now14( char *out );        /* the date and time as "YYYYMMDDHHMMSS" + NUL (out >= 15) */

/* ---- the console ---- */
void sl_putc( char ch );
void sl_puts( const char *text );
void sl_putu( u32 value );         /* decimal */
void sl_putip( u32 ip );
void sl_flush( void );
int  sl_kbhit( void );             /* nonzero if a key is waiting */
int  sl_getkey( void );            /* a key, blocking; 0x100|scan for an extended one */

/* ---- the command tail: argv[0] is the first word after the program ---- */
int  sl_args( char ***argv );      /* returns the count */

/* ---- the environment ---- */
const char *sl_getenv( const char *name );    /* a variable's value, or NULL */

/* ---- small helpers ---- */
u32  sl_strlen( const char *s );
void sl_strcpy( char *to, const char *from, u32 max );
int  sl_strcmp( const char *a, const char *b );
int  sl_strcasecmp( const char *a, const char *b );
u32  sl_atou( const char *s );
void sl_memcpy( void *to, const void *from, u32 len );
void sl_memzero( void *to, u32 len );

/* ---- the interface ---- */
int  sl_ifstat( SOCK_IF *info );   /* 1 ok */
int  sl_up( u32 ms );              /* wait until the interface has an address; 1 ok */

/* ---- names and echoes ---- */
u32  sl_resolve( const char *name, u32 ms );          /* an address, or 0 */
int  sl_ping( u32 ip, u16 seq, u32 *rttms, u32 ms );  /* 1 on a reply */

/* ---- TCP ---- */
int  sl_open( u32 type );                             /* a handle (>=1), or 0 */
void sl_sclose( int handle );
int  sl_connect( int handle, u32 ip, u16 port, u32 ms );  /* 1 established */
int  sl_state( int handle, SOCK_STAT *st );               /* the SS_* state */
int  sl_send( int handle, const void *data, u32 len, u32 ms );  /* bytes sent, <len on timeout */
int  sl_recv( int handle, void *buf, u32 max );           /* at once: bytes, 0 none */
int  sl_recvwait( int handle, void *buf, u32 max, u32 ms );    /* bytes; 0 = closed or timed out */
int  sl_recvline( int handle, char *buf, int max, u32 ms );    /* a CRLF line, NUL-ended; -1 closed */
int  sl_closed( int handle );      /* 1 once the peer has sent FIN and nothing waits */

/* ---- TCP server ---- */
int  sl_listen( int handle, u16 port );
int  sl_accept( int handle, u32 *peer_ip, u16 *peer_port, u32 ms );  /* a new handle, or 0 */

/* ---- UDP ---- */
int  sl_dgram_send( int handle, u32 ip, u16 port, const void *data, u32 len );
int  sl_dgram_recv( int handle, u32 *from_ip, u16 *from_port, void *buf, u32 max, u32 ms );

#endif /* SOCKLIB_H */
