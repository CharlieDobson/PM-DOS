/* ==========================================================================
 * SMB1.H - a small SMB1 (CIFS, "NT LM 0.12") client engine for the
 * down-level peers SMB2 does not reach: Win9x/ME, NT4, OS/2 LAN Server,
 * Samba in NT1 mode and WfW with the NT-compatible redirector.
 * ==========================================================================
 * Transport is NetBIOS-over-TCP (port 139): a session request opens the
 * NetBIOS session, then SMB messages ride the 4-byte NBT length header.
 * Authentication is the old LM + NTLMv1 challenge-response (SMBCRYPT's
 * des/lm_hash/desl over the server's 8-byte challenge) - no SPNEGO, no
 * signing, OEM (8-bit) strings, NT status codes.
 *
 * The engine is transport-agnostic (the same send/recv callbacks as
 * SMB2.H) and answers in the SAME smb2_fh / smb2_dirent shapes, so the
 * kernel redirector (DRV\SMBFS) drives SMB1 and SMB2 through one code
 * path, choosing by the mount's protocol.  C89.
 * ========================================================================== */
#ifndef PMDOS_SMB1_H
#define PMDOS_SMB1_H

#include "smb2.h"       /* smb_send_fn, smb_recv_fn, smb2_fh, smb2_dirent */

#define SMB1_SENDBUF   8192
#define SMB1_RECVBUF   33280     /* a shade over 32K of data + headers */

typedef struct {
    smb_send_fn    send;
    smb_recv_fn    recv;
    void          *ioctx;

    unsigned long  seed;              /* PRNG for the NetBIOS CALLING scope */
    unsigned int   tree_id;           /* TID of the connected share */
    unsigned int   uid;               /* UID from session setup */
    unsigned int   mid;               /* next multiplex id */
    unsigned long  capabilities;      /* the server's NEGOTIATE capabilities */
    unsigned char  challenge[8];      /* the server's 8-byte encryption key */
    int            max_buffer;        /* the server's MaxBufferSize */

    unsigned long  last_status;       /* NTSTATUS of the last response */
    const char    *stage;             /* where we are, for error reports */

    unsigned char  sendbuf[SMB1_SENDBUF];
    unsigned char  recvbuf[SMB1_RECVBUF];
    int            recvlen;
} smb1_client;

/* our NetBIOS name, sent as the CALLING name of the session request:
   "PMDOS" unless the program sets it (SMBFS takes TCPIP.SYS /NAME=) */
extern char smb1_calling[16];

/* Open the NetBIOS session, negotiate NT LM 0.12, authenticate and
   tree-connect \\server\share.  The TCP connection must be made already.
   0 on success, <0 on failure (cl->stage / cl->last_status say what). */
int  smb1_connect( smb1_client *cl, smb_send_fn send, smb_recv_fn recv, void *ioctx,
                   unsigned long seed,
                   const char *server, const char *share,
                   const char *user, const char *password, const char *domain );

/* List 'dir' (share-relative, backslash, "" = the share root); 'emit' is
   called for each entry.  The directory is searched whole (FIND_FIRST2 +
   FIND_NEXT2 looped here).  0, or <0 on failure. */
int  smb1_list_dir( smb1_client *cl, const char *dir, const char *pattern,
                    void ( *emit )( void *user, const smb2_dirent *entry ),
                    void *user );

/* Open/create a file by share-relative path.  'write' asks for write
   access; 'disposition' is the SMB2 value the redirector already uses:
   1 FILE_OPEN (must exist), 3 FILE_OPEN_IF (open or create), 5
   FILE_OVERWRITE_IF (create or truncate).  Fills 'fh' (FID in file_id[0..1]). */
int  smb1_open_disp( smb1_client *cl, const char *path, int write,
                     unsigned long disposition, smb2_fh *fh );

int  smb1_read( smb1_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
                unsigned char *buf, int len );
int  smb1_write( smb1_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
                 const unsigned char *buf, int len );
int  smb1_close_file( smb1_client *cl, const smb2_fh *fh );

int  smb1_delete( smb1_client *cl, const char *path );
int  smb1_mkdir( smb1_client *cl, const char *path );
int  smb1_rmdir( smb1_client *cl, const char *path );
int  smb1_rename( smb1_client *cl, const char *oldpath, const char *newpath );

void smb1_disconnect( smb1_client *cl );

const char   *smb1_stage( const smb1_client *cl );
unsigned long smb1_status( const smb1_client *cl );

#endif /* PMDOS_SMB1_H */
