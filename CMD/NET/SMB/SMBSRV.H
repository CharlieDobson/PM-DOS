/* ==========================================================================
 * SMBSRV.H - a small SMB1 (CIFS, "NT LM 0.12") file server, so PM-DOS can
 * SHARE its files with down-level peers (and with the PM-DOS SMB1 client).
 * ==========================================================================
 * The protocol core (SMBSRV.C) is transport- and filesystem-agnostic: it
 * drives the SMB conversation over the same smb_send_fn / smb_recv_fn
 * callbacks the clients use, and does every file operation through a
 * srv_vfs vtable.  Two back ends plug in: INT 21h on PM-DOS (SMBSRV.EXE)
 * and the C runtime on the host (SMBSRVHOST.EXE, for proving).
 *
 * It speaks NT LM 0.12 with OEM (8-bit) names and NT status, accepts any
 * client (one share, the configured directory), and serves OPEN/READ/
 * WRITE_ANDX, CLOSE, TRANS2 FIND_FIRST2, the classic DELETE / CREATE and
 * DELETE DIRECTORY, TREE (dis)connect, LOGOFF and ECHO.  C89.
 * ========================================================================== */
#ifndef PMDOS_SMBSRV_H
#define PMDOS_SMBSRV_H

#include "smb2.h"       /* smb_send_fn, smb_recv_fn */

/* one directory entry a back end hands back during a listing */
typedef struct {
    char          name[260];        /* OEM ASCIIZ, the on-disk name */
    unsigned long attr;             /* FILE_ATTRIBUTE_* (low bits = DOS attr) */
    unsigned long size;             /* bytes (low 32) */
    unsigned long ft_lo, ft_hi;     /* LastWriteTime as a Windows FILETIME */
} srv_dirent;

/* the filesystem back end.  Paths are share-relative, backslash-separated,
   with a leading backslash ("\" is the share root); the back end joins them
   with its own root and may reject "..".  A handle is a small non-negative
   int the back end chooses; -1 means failure. */
typedef struct {
    int   ( *vopen )( void *ctx, const char *path, int write, int dispo,
                      unsigned long *size, int *action );   /* dispo 1 open,3 open/create,5 create/trunc */
    int   ( *vread )( void *ctx, int h, unsigned long off, void *buf, int len );
    int   ( *vwrite )( void *ctx, int h, unsigned long off, const void *buf, int len );
    void  ( *vclose )( void *ctx, int h );
    void *( *vfindopen )( void *ctx, const char *dir );     /* NULL if the dir is missing */
    int   ( *vfindnext )( void *ctx, void *it, srv_dirent *de ); /* 1 got one, 0 done */
    void  ( *vfindclose )( void *ctx, void *it );
    int   ( *vmkdir )( void *ctx, const char *path );       /* 0 ok */
    int   ( *vrmdir )( void *ctx, const char *path );
    int   ( *vunlink )( void *ctx, const char *path );
} srv_vfs;

/* Serve one connected client to the end (its disconnect).  'sharename' is
   reported on tree-connect; the back end's ctx carries the served root. */
/* the server's name in SESSION_SETUP's answer: "PMDOS" unless the program
   sets it (SMBSRV.EXE takes TCPIP.SYS /NAME=) */
extern char smbsrv_name[16];

void smbsrv_serve( smb_send_fn send, smb_recv_fn recv, void *ioctx,
                   const srv_vfs *fs, void *fsctx, const char *sharename );

#endif /* PMDOS_SMBSRV_H */
