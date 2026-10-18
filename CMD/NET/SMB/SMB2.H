/* ==========================================================================
 * SMB2.H - a small SMB2/3 client engine: connect, authenticate (NTLMv2),
 * tree-connect and list a directory.  Transport-agnostic (send/recv
 * callbacks, like the TLS core), so the same engine is proved on the host
 * over Winsock and then run on PM-DOS over SOCKLIB.  C89.
 * ==========================================================================
 * Dialects offered: 2.0.2, 2.1, 3.0, 3.0.2, 3.1.1 - the server picks.  Auth
 * is raw NTLMSSP/NTLMv2 (no SPNEGO wrapper, which Windows accepts).  This
 * first cut does not sign; it is for a server that does not require signing.
 * ========================================================================== */
#ifndef PMDOS_SMB2_H
#define PMDOS_SMB2_H

/* blocking transport: send writes all 'len' (returns len, or <0); recv reads
   up to 'len' (returns >0 bytes, 0 on close, <0 on error). */
typedef int ( *smb_send_fn )( void *ctx, const unsigned char *buf, int len );
typedef int ( *smb_recv_fn )( void *ctx, unsigned char *buf, int len );

#define SMB2_SENDBUF   4096
#define SMB2_RECVBUF   32768

typedef struct {
    smb_send_fn    send;
    smb_recv_fn    recv;
    void          *ioctx;

    unsigned long  seed;                /* PRNG state for challenges/GUID */
    unsigned long  msgid_lo, msgid_hi;  /* next MessageId */
    unsigned char  session_id[8];
    unsigned long  tree_id;
    int            dialect;             /* negotiated DialectRevision */
    int            signing_required;

    /* signing: the pre-auth hash (3.1.1), the keys, and the mode */
    unsigned char  preauth[64];
    unsigned char  session_key[16];     /* ExportedSessionKey */
    unsigned char  signing_key[16];
    int            signing_mode;        /* 0 none, 2 HMAC-SHA256, 3 AES-CMAC */
    int            signing_active;

    /* raw NTLM messages, kept for the MIC */
    unsigned char  ntlm_negotiate[64];
    int            ntlm_negotiate_len;
    unsigned char  ntlm_challenge[1024];
    int            ntlm_challenge_len;

    unsigned char  sendbuf[SMB2_SENDBUF];
    unsigned char  recvbuf[SMB2_RECVBUF];
    int            recvlen;

    unsigned long  last_status;         /* NTSTATUS of the last response */
    const char    *stage;               /* where we are, for error reports */
} smb2_client;

/* Connect TCP must already be done; these drive the SMB2 conversation over
   the callbacks.  Return 0 on success, <0 on failure (cl->stage and
   cl->last_status say what and where). */
int  smb2_connect( smb2_client *cl, smb_send_fn send, smb_recv_fn recv, void *ioctx,
                   unsigned long seed,
                   const char *server, const char *share,
                   const char *user, const char *password, const char *domain );

/* Call 'emit' for each directory entry under the connected share root. */
int  smb2_list( smb2_client *cl, const char *pattern,
                void ( *emit )( void *user, const char *name, int isdir, unsigned long size ),
                void *user );

/* A richer directory entry, for a redirector that needs the DOS stamp and
   attribute byte as well as the name.  'name' is the server's name with
   its UTF-16LE truncated to bytes (ASCII in practice); 'attr' is the
   Windows FILE_ATTRIBUTE_* word (its low bits are the DOS ones); the
   times are Windows FILETIME (100ns since 1601). */
typedef struct {
    char          name[260];
    unsigned long attr;
    unsigned long size_lo, size_hi;
    unsigned long wtime_lo, wtime_hi;    /* LastWriteTime */
} smb2_dirent;

/* List 'dir' (share-relative, backslash, no leading slash; "" = the share
   root), calling 'emit' for each entry.  'pattern' is the server-side
   match ("*" for all; a redirector matches DOS wildcards itself).  The
   directory handle is opened and closed here.  0, or <0 on failure. */
int  smb2_list_dir( smb2_client *cl, const char *dir, const char *pattern,
                    void ( *emit )( void *user, const smb2_dirent *entry ),
                    void *user );

/* An open remote file: the SMB2 FileId and the size the CREATE reported. */
typedef struct {
    unsigned char file_id[16];
    unsigned long size_lo, size_hi;
    unsigned long attr;                 /* FILE_ATTRIBUTE_* from the CREATE */
} smb2_fh;

/* Open/create a file (not the share root) by its share-relative path
   (backslash-separated, no leading backslash).  'write' asks for write
   access; 'create' creates it if absent (else it must exist).  0 and fills
   'fh', or <0. */
int  smb2_open_file( smb2_client *cl, const char *path, int write, int create, smb2_fh *fh );

/* The same, but with the SMB2 CreateDisposition spelled out: 1 FILE_OPEN
   (must exist), 3 FILE_OPEN_IF (open or create), 5 FILE_OVERWRITE_IF
   (create or truncate-to-empty).  A redirector uses 5 for a DOS create. */
int  smb2_open_disp( smb2_client *cl, const char *path, int write,
                     unsigned long disposition, smb2_fh *fh );

/* Read/write at an absolute 64-bit offset.  Return bytes moved; a read of 0
   is end of file; <0 is an error. */
int  smb2_read( smb2_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
                unsigned char *buf, int len );
int  smb2_write( smb2_client *cl, const smb2_fh *fh, unsigned long off_lo, unsigned long off_hi,
                 const unsigned char *buf, int len );

/* Close a file opened with smb2_open_file. */
int  smb2_close_file( smb2_client *cl, const smb2_fh *fh );

/* Delete a file, make a directory, remove an (empty) directory.  Each is
   a CREATE with the right options (delete-on-close, or FILE_CREATE of a
   directory) and a close.  0, or <0 on failure. */
int  smb2_delete( smb2_client *cl, const char *path );
int  smb2_mkdir( smb2_client *cl, const char *path );
int  smb2_rmdir( smb2_client *cl, const char *path );
int  smb2_rename( smb2_client *cl, const char *oldpath, const char *newpath );

void smb2_disconnect( smb2_client *cl );

int  smb2_dialect( const smb2_client *cl );     /* negotiated DialectRevision */
const char *smb2_stage( const smb2_client *cl );
unsigned long smb2_status( const smb2_client *cl );

#endif /* PMDOS_SMB2_H */
