/* ==========================================================================
 * TLSHS.H - TLS 1.2 client handshake + application data for NETSPLOR.
 * Win32s / MSVC 2.2 + Open Watcom 1.9 (portable C89).
 * ==========================================================================
 * Implements a TLS 1.2 client for the single suite
 * TLS_RSA_WITH_AES_128_CBC_SHA (0x002F): ClientHello .. Finished, RSA key
 * exchange (PKCS#1 v1.5), key schedule via the TLS 1.2 PRF, and CBC record
 * protection.  Trust-on-first-use (see X509.C) - no chain validation yet.
 *
 * I/O-agnostic: the caller supplies blocking send/recv callbacks.  A blocking
 * socket drives it today (testable against openssl s_server); the same engine
 * can later be pumped by the async NST_STREAM once made resumable.
 * ========================================================================== */
#ifndef NETSPLOR_TLSHS_H
#define NETSPLOR_TLSHS_H

#include "rng.h"

/* Blocking I/O callbacks.
 *   send: write up to 'len' bytes; return bytes written (>0) or <0 on error.
 *   recv: read up to 'len' bytes; return >0 bytes, 0 on close, <0 on error.  */
typedef int (*tls_send_fn)(void *ctx, const unsigned char *buf, int len);
typedef int (*tls_recv_fn)(void *ctx, unsigned char *buf, int len);

typedef struct tls_conn tls_conn;

/* Allocate / free a connection (rng must outlive it). */
tls_conn *tls_new(nst_rng *rng);
void      tls_free(tls_conn *c);

/* Configure certificate validation, applied during the handshake:
 *   host   : hostname to match (SAN/CN); NULL/"" skips the host check.
 *   now14  : current UTC time "YYYYMMDDHHMMSS"; NULL/"" skips the date check.
 *   ca_der : a trusted CA certificate (DER) the leaf must be signed by;
 *            NULL skips signature verification (trust-on-first-use).
 * If never called, the handshake performs no certificate validation.
 */
void tls_set_verify(tls_conn *c, const char *host, const char *now14,
                    const unsigned char *ca_der, int ca_len);

/* Reason a handshake failed certificate validation (0 if not applicable). */
int  tls_verify_error(tls_conn *c);

/* Run the full client handshake with blocking callbacks (standalone / tests).
   Internally drives the resumable engine below.  0 on success, <0 on error. */
int tls_client_handshake(tls_conn *c, const char *host,
                         tls_send_fn send, tls_recv_fn recv, void *ioctx);

/* --------------------------------------------------------------------------
 * Resumable handshake engine - no blocking, driven by the caller's event
 * loop (used by the browser's async transport).  Usage:
 *   tls_hs_start(c, host);
 *   loop:  drain tls_hs_out_ptr()/consume() to the socket;
 *          st = tls_hs_pump(c);
 *          if DONE -> switch to app data (any tls_hs_leftover() first);
 *          if WANT_READ -> recv bytes, tls_hs_feed(), repeat;
 *          if ERROR -> abort.
 * ------------------------------------------------------------------------ */
#define TLS_HS_WANT_READ  0
#define TLS_HS_DONE       1
#define TLS_HS_ERROR    (-1)

void tls_hs_start(tls_conn *c, const char *host);          /* begin (queues CH on 1st pump) */
int  tls_hs_feed(tls_conn *c, const unsigned char *data, int len); /* add inbound bytes */
int  tls_hs_pump(tls_conn *c);                             /* advance; WANT_READ/DONE/ERROR */
int  tls_hs_out_ptr(tls_conn *c, unsigned char **ptr);     /* pending outbound: len + ptr */
void tls_hs_out_consume(tls_conn *c, int n);               /* mark n outbound bytes sent */
int  tls_hs_pending_output(tls_conn *c);
int  tls_hs_leftover(tls_conn *c, unsigned char **ptr);    /* inbound bytes past Finished */

/* --------------------------------------------------------------------------
 * Introspection - read-only views of the negotiated connection, for hosts
 * (e.g. NSTLS32.DLL) that do not have the tls_conn definition in scope.
 * The suite/version accessors return 0 until the handshake completes.
 * ------------------------------------------------------------------------ */
int tls_get_suite(const tls_conn *c);          /* IANA cipher suite number   */
int tls_get_version(const tls_conn *c);        /* 0x0303 = TLS 1.2           */
int tls_is_established(const tls_conn *c);
int tls_get_cert_count(const tls_conn *c);     /* certs the peer presented   */
/* Copy chain[index] (0 = leaf) into 'out'.  Pass out=NULL to query the size.
   Returns the DER length, 0 if no such cert, -1 if 'outcap' is too small. */
int tls_get_cert(const tls_conn *c, int index, unsigned char *out, int outcap);

/* Application data over the established connection (blocking callbacks). */
int tls_write_app(tls_conn *c, tls_send_fn send, void *ioctx,
                  const unsigned char *data, int len);
int tls_read_app(tls_conn *c, tls_recv_fn recv, void *ioctx,
                 unsigned char *out, int outcap);   /* >0, 0=closed, <0=err */

/* --------------------------------------------------------------------------
 * Buffer-oriented app-data crypto (for driving TLS from an async record
 * pump - see NETFETCH's TLS stream).  No I/O; caller owns the socket.
 * ------------------------------------------------------------------------ */
#define TLS_APP_MAX  16384          /* max plaintext fragment per record */

/* Encrypt one app-data fragment into a complete record.  Returns record
   length written to 'out', or -1. */
int tls_app_encrypt(tls_conn *c, const unsigned char *in, int len,
                    unsigned char *out, int outcap);

/* Encrypt one fragment as an arbitrary content type (tls_app_encrypt is fixed
   at TLS_CT_APPLICATION_DATA).  Used to emit a protected close_notify alert.
   Returns the record length, or -1. */
int tls_app_encrypt_type(tls_conn *c, int content_type,
                         const unsigned char *in, int len,
                         unsigned char *out, int outcap);

/* Decrypt one complete record.  Returns plaintext length (>=0) and sets
   *content_type, or -1 on MAC/parse failure. */
int tls_app_decrypt(tls_conn *c, const unsigned char *rec, int reclen,
                    unsigned char *out, int outcap, int *content_type);

#endif /* NETSPLOR_TLSHS_H */
