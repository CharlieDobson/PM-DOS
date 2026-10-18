/*
 * REDIR.H - the contract between the kernel's network-drive branches
 * and an NDTF_REDIR driver (DRV\SMBFS).
 *
 * The native kernel has no redirector and no CDS (ISO9660.INC says
 * why): a drive letter is a volume, and which filesystem it is is the
 * two-valued vol_fs (FS_FAT, FS_ISO) - now three, with FS_NET.  A net
 * letter's files are on an SMB server, so the file functions that take
 * a path or a handle test the letter first (NET_DRV_TYPE in its
 * drv_tbl row) and, when it is a net one, fill the block below and call
 * the one loaded NDTF_REDIR driver's CALL entry: EBX = a REDIR_*
 * function, EDI -> the block, ECX = its length.  The driver does the
 * SMB2/3 conversation (CMD\NET\SMB) over the TCP/IP stack it reaches
 * through NDS_SOCKCALL, and answers in the block's out fields.
 *
 * The driver's nd_call returns 0 whenever the request was carried out
 * at all - success AND an ordinary "file not found"; `status` holds the
 * DOS result (0, or an ERR_* the kernel puts in AX).  It returns an
 * ERR_* itself only when the call could not be made (no session).
 *
 * Every number here is INC\REDIR.INC's; a change there is a change
 * here.  Needs INC\NDRV.H before it for u8/u16/u32.
 */
#ifndef REDIR_H
#define REDIR_H

/* THE FUNCTIONS - EBX for the driver's CALL entry */
#define REDIR_MOUNT      1     /* connect + authenticate + tree-connect */
#define REDIR_UMOUNT     2     /* drop the session for this drive */
#define REDIR_FINDFIRST  3     /* list a directory; answer the first match */
#define REDIR_FINDNEXT   4     /* the next match of the cached listing */
#define REDIR_OPEN       5     /* open (and optionally create) a file */
#define REDIR_READ       6     /* read count bytes at pos into bufptr */
#define REDIR_WRITE      7     /* write count bytes at pos from bufptr */
#define REDIR_CLOSE      8     /* close an open handle */
#define REDIR_INFO       9     /* the share's free space, for 36h */
#define REDIR_DELETE     10    /* delete a file */
#define REDIR_MKDIR      11    /* make a directory */
#define REDIR_RMDIR      12    /* remove an empty directory */
#define REDIR_RENAME     13    /* rename/move within the share: path -> share */

/* RESUMABLE EXECUTION, for the WfW bridge (PMDOS.386).  The native kernel
   calls a REDIR_* op and it runs to completion, because its socket waits
   spin with interrupts on and the tick pumps the stack between polls.  The
   bridge cannot: its window runs with interrupts off, so a spin there would
   hang.  So the bridge ORs REDIR_RESUME into the op and polls: the driver
   runs the op on a worker fibre that yields whenever a socket wait has no
   data yet, answering REDIR_PENDING in `status`; the caller lets Windows run
   (where the tick and the card interrupt advance the parked stack) and calls
   again with the same op until `status` is no longer REDIR_PENDING.  One op
   is resumable at a time, which is all a redirector ever has outstanding. */
#define REDIR_RESUME   0x1000  /* OR into the op: run resumably on the fibre */
#define REDIR_PENDING  0xFF    /* status: not finished this slice; poll again */

/* THE REQUEST BLOCK - fixed layout, shared with INC\REDIR.INC.  All the
   u32 fields first so the offsets are easy to fill from assembly. */
typedef struct {
    u32  op;             /* REDIR_* (also in EBX) */
    u32  drive;          /* the net drive, 0 = A */
    u32  handle;         /* in/out: the driver's open-file handle */
    u32  status;         /* out: 0, or a DOS error (PMDOS.INC ERR_*) */
    u32  pos_lo, pos_hi; /* in: the byte offset for read/write */
    u32  count;          /* in: bytes for read/write; FINDNEXT: the index */
    u32  result;         /* out: bytes moved / new handle / listing id */
    u32  size_lo, size_hi;/* out: file size (OPEN) or entry size (FIND) */
    u32  attr;           /* out: the entry's DOS attribute byte */
    u32  time_date;      /* out: (date << 16) | time, DOS format */
    u32  bufptr;         /* in: flat data buffer for READ/WRITE */
    u32  gen;            /* in: the listing id for FINDNEXT */
    u32  freeclus;       /* out: free "clusters" for INFO */
    u32  spare;
    char path[260];      /* in: share-relative path (dir, for FIND; old name for RENAME) */
    char pattern[16];    /* in: the DOS wildcard pattern, for FINDFIRST */
    char name[16];       /* out: the entry's 8.3 name, ASCIIZ (FIND) */
    char server[64];     /* in, for MOUNT */
    char share[80];      /* in, for MOUNT; also RENAME's new name (a DOS path fits) */
    char user[64];
    char pass[64];
    char domain[64];
} redir_req;

/* 21h/F1h AL=27h - NET USE, for a native NET.EXE.  EAX bits 16-23 =
   NETUSE_*, EDI -> a netuse_req the kernel has checked, ECX = its
   length.  The kernel marks the drv_tbl row and drives REDIR_MOUNT /
   REDIR_UMOUNT; the program never touches the driver itself. */
#define NETUSE_MAP    0      /* map req.drive to \\server\share */
#define NETUSE_UNMAP  1      /* unmap req.drive */
#define NETUSE_QUERY  2      /* fill req from req.drive */

#define NUF_INUSE     0x01   /* QUERY: the drive is a mapped net drive */
#define NUF_SMB1      0x02   /* MAP: use the down-level SMB1 client, not SMB2 */

typedef struct {
    u32  drive;          /* 0 = A */
    u32  status;         /* out: 0, or a DOS error (ERR_*) */
    u32  flags;          /* out: NUF_* for QUERY */
    char server[64];
    char share[80];
    char user[64];
    char pass[64];
    char domain[64];
} netuse_req;

#endif /* REDIR_H */
