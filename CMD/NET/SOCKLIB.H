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
 * ADDRESS is a uint32_t, most significant octet first (INC\SOCKET.H).
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
uint32_t sl_now( void );
uint32_t sl_since( uint32_t start );  /* hundredths since sl_now() returned start */
void     sl_now14( char *out );       /* the date and time as "YYYYMMDDHHMMSS" + NUL (out >= 15) */

/* ---- the console ---- */
void sl_putc( char ch );
void sl_puts( const char *text );
void sl_putu( unsigned int value ); /* decimal */
void sl_putip( uint32_t ip );
void sl_flush( void );
int  sl_kbhit( void );             /* nonzero if a key is waiting */
int  sl_getkey( void );            /* a key, blocking; 0x100|scan for an extended one */

/* ---- the command tail: argv[0] is the first word after the program ---- */
int  sl_args( char ***argv );      /* returns the count */

/* ---- the environment ---- */
const char *sl_getenv( const char *name );    /* a variable's value, or NULL */

/* ---- small helpers ---- */
unsigned int sl_strlen( const char *s );
void         sl_strcpy( char *to, const char *from, unsigned int max );
int          sl_strcmp( const char *a, const char *b );
int          sl_strcasecmp( const char *a, const char *b );
unsigned int sl_atou( const char *s );
void         sl_memcpy( void *to, const void *from, unsigned int len );
void         sl_memzero( void *to, unsigned int len );

/* ---- the interface ---- */
int  sl_ifstat( SOCK_IF *info );   /* 1 ok */
int  sl_up( unsigned int ms );     /* wait until the interface has an address; 1 ok */

/* ---- names and echoes ---- */
uint32_t sl_resolve( const char *name, unsigned int ms );                        /* an address, or 0 */
int      sl_ping( uint32_t ip, uint16_t seq, uint32_t *rttms, unsigned int ms ); /* 1 on a reply */

/* ---- TCP ---- */
int  sl_open( uint32_t type );                        /* a handle (>=1), or 0 */
void sl_sclose( int handle );
int  sl_connect( int handle, uint32_t ip, uint16_t port, unsigned int ms );      /* 1 established */
int  sl_state( int handle, SOCK_STAT *st );                                      /* the SS_* state */
int  sl_send( int handle, const void *data, unsigned int len, unsigned int ms ); /* bytes sent, <len on timeout */
int  sl_recv( int handle, void *buf, unsigned int max );                         /* at once: bytes, 0 none */
int  sl_recvwait( int handle, void *buf, unsigned int max, unsigned int ms );    /* bytes; 0 = closed or timed out */
int  sl_recvline( int handle, char *buf, int max, unsigned int ms );             /* a CRLF line, NUL-ended; -1 closed */
int  sl_closed( int handle );      /* 1 once the peer has sent FIN and nothing waits */

/* ---- TCP server ---- */
int  sl_listen( int handle, uint16_t port );
int  sl_accept( int handle, uint32_t *peer_ip, uint16_t *peer_port, unsigned int ms ); /* a new handle, or 0 */

/* ---- UDP ---- */
int  sl_dgram_send( int handle, uint32_t ip, uint16_t port, const void *data, unsigned int len );
int  sl_dgram_recv( int handle, uint32_t *from_ip, uint16_t *from_port, void *buf, unsigned int max, unsigned int ms );

#endif /* SOCKLIB_H */
