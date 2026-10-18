/*
 * TLSGLUE.H - WGET's door to the TLS port (CMD\NET\TLS)
 *
 * tls_open does the handshake on an already-connected socket and gives
 * back an opaque connection; tls_send/tls_recv carry the application
 * data; tls_close ends it.  All over the vendored NSTLS core.
 */
#ifndef TLSGLUE_H
#define TLSGLUE_H

#include "ndrv.h"

/* what tls_open did about the server's certificate (its *outcome) */
#define TLSV_VERIFIED    0      /* chain, host and dates checked against the bundle */
#define TLSV_UNVERIFIED  1      /* connected, certificate NOT checked (no bundle, or /K) */
#define TLSV_BADCERT     2      /* handshake refused: the certificate did not check out */
#define TLSV_NOCONNECT   3      /* the handshake failed for a reason other than the cert */

/* Open a TLS connection on an already-connected socket.
 *   host     : the server name, matched against the certificate and sent as SNI.
 *   ca/calen : the trust bundle (concatenated DER roots), or NULL to not verify.
 *   insecure : nonzero skips verification even when a bundle is given.
 *   outcome  : receives a TLSV_* code (may be NULL).
 * Returns the connection, or NULL if the handshake failed (outcome says why). */
void *tls_open( int handle, const char *host, const unsigned char *ca, int calen,
                int insecure, int *outcome );
int   tls_verify_reason( void );   /* the X509_ERR_* behind a TLSV_BADCERT, else 0 */

int   tls_send( void *conn, const void *data, u32 len );    /* bytes, or <0 */
int   tls_recv( void *conn, void *buf, u32 max );           /* bytes, 0 at end, <0 error */
void  tls_close( void *conn );

#endif /* TLSGLUE_H */
