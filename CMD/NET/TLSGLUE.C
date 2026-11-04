/*
 * TLSGLUE.C - the seam between WGET's socket and the NSTLS TLS core
 *
 * The core (CMD\NET\TLS) is transport-free: it asks for bytes through a
 * send and a recv callback.  Here those callbacks are the stack's
 * sockets (SOCKLIB), and the blocking handshake/app-data wrappers
 * (tls_client_handshake, tls_write_app, tls_read_app) are driven over
 * them.  One connection at a time, which is all WGET needs.
 */
#include "socklib.h"
#include "tlsglue.h"
#include "tlshs.h"
#include "rng.h"
#include <stdlib.h>

static int io_send( void *ctx, const unsigned char *buf, int len )
{
    return sl_send( (int)ctx, buf, (u32)len, 4000 );
}

static int io_recv( void *ctx, unsigned char *buf, int len )
{
    int got = sl_recvwait( (int)ctx, buf, (u32)len, 4000 );
    return got;                         /* 0 (closed or long silence) is fatal to the caller */
}

static nst_rng rng;                     /* lives as long as the connection */

/* A WHOLE RECORD'S PLAINTEXT, KEPT HERE.  tls_read_app decrypts one record
   into the buffer it is given, and a record holds up to 16K: WGET's 4K
   buffer refused every bigger one, and the fetch ended as "HTTP 0" (found
   2026-11-03 on s_server's 4.2K status page; nginx sends 16K records).
   The room covers TLS 1.3's content type byte and padding after the data,
   and it comes from the TLS heap, so the program file does not carry it. */
#define PLAIN_MAX  ( TLS_APP_MAX + 256 )

typedef struct {
    tls_conn *conn;
    int       handle;
    u8       *plain;                    /* the record being handed out (PLAIN_MAX) */
    int       ppos, plen;
} GLUE;

static GLUE glue;

static int verr;                        /* the X509_ERR_* from the last tls_open */

int tls_verify_reason( void )
{
    return verr;
}

void *tls_open( int handle, const char *host, const unsigned char *ca, int calen,
                int insecure, int *outcome )
{
    char now14[15];
    int  verifying = ( ca != (const unsigned char *)0 && calen > 0 && !insecure );

    tls_close( &glue );                 /* one connection at a time: drop a stale one */
    verr = 0;
    rng_init( &rng );
    glue.handle = handle;
    glue.ppos = glue.plen = 0;
    glue.plain = (u8 *)malloc( PLAIN_MAX );
    if ( glue.plain == (u8 *)0 ) {
        if ( outcome ) *outcome = TLSV_NOCONNECT;
        return (void *)0;
    }
    glue.conn = tls_new( &rng );
    if ( glue.conn == (tls_conn *)0 ) {
        free( glue.plain );
        glue.plain = (u8 *)0;
        if ( outcome ) *outcome = TLSV_NOCONNECT;
        return (void *)0;
    }
    if ( verifying ) {
        sl_now14( now14 );
        tls_set_verify( glue.conn, host, now14, ca, calen );
    }
    if ( tls_client_handshake( glue.conn, host, io_send, io_recv, (void *)handle ) != 0 ) {
        verr = tls_verify_error( glue.conn );
        tls_free( glue.conn );
        glue.conn = (tls_conn *)0;
        free( glue.plain );
        glue.plain = (u8 *)0;
        if ( outcome ) *outcome = verr ? TLSV_BADCERT : TLSV_NOCONNECT;
        return (void *)0;
    }
    if ( outcome ) *outcome = verifying ? TLSV_VERIFIED : TLSV_UNVERIFIED;
    return &glue;
}

int tls_send( void *conn, const void *data, u32 len )
{
    GLUE *g = (GLUE *)conn;
    return tls_write_app( g->conn, io_send, (void *)g->handle, (const unsigned char *)data, (int)len );
}

int tls_recv( void *conn, void *buf, u32 max )
{
    GLUE *g = (GLUE *)conn;
    u8 *to = (u8 *)buf;
    int n, i;

    if ( g->ppos >= g->plen ) {             /* the last record is all handed out */
        n = tls_read_app( g->conn, io_recv, (void *)g->handle, g->plain, PLAIN_MAX );
        if ( n <= 0 ) return n;
        g->ppos = 0;
        g->plen = n;
    }
    n = g->plen - g->ppos;
    if ( (u32)n > max ) n = (int)max;
    for ( i = 0; i < n; i++ ) to[i] = g->plain[g->ppos + i];
    g->ppos += n;
    return n;
}

void tls_close( void *conn )
{
    GLUE *g = (GLUE *)conn;
    if ( g->conn ) { tls_free( g->conn ); g->conn = (tls_conn *)0; }
    if ( g->plain ) { free( g->plain ); g->plain = (u8 *)0; }
}
