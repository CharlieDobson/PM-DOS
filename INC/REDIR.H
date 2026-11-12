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
#define REDIR_INFO       9     /* the share's size, for 36h and 7303h - see below */
#define REDIR_DELETE     10    /* delete a file */
#define REDIR_MKDIR      11    /* make a directory */
#define REDIR_RMDIR      12    /* remove an empty directory */
#define REDIR_RENAME     13    /* rename/move within the share: path -> share */
#define REDIR_ISDIR      14    /* is path a directory?  status 0, or ERR_NOPATH */
#define REDIR_GETATTR    15    /* path's DOS attribute byte, in attr, for 4300h */
#define REDIR_CHDIR      16    /* make path the drive's current directory (3Bh);
                                  spare 1: LFN=OFF, its 8.3 form within 63 */
#define REDIR_GETCWD     17    /* the current directory -> path (47h spare 1, 8.3;
                                  7147h spare 0, long) */
#define REDIR_CANON      18    /* path as truename gives it -> path (60h spare 0,
                                  8.3; 7160h CL=2 spare 1, long) */
#define REDIR_PWL        19    /* the password list: bufptr -> a pwl_req of
                                  count bytes (below), status in the pwl_req */

/* THE DRIVER KEEPS EACH DRIVE'S CURRENT DIRECTORY, and a path that does
   not start with a backslash is taken from it.  Every path handed over is
   share-relative, "X:" taken off.  The answers of GETCWD and CANON have
   no leading backslash.  A FINDFIRST with spare 1 is 714Eh's: the pattern
   is matched against long names as well, and each entry's long name comes
   back in path. */

/* REDIR_INFO answers in the server's own allocation units, which the
   kernel fits to whichever call asked: size_lo/size_hi = the units in all
   (64 bits), pos_lo/pos_hi = the units still free, count = sectors per
   unit, result = bytes per sector.  status is not 0 when the server
   would not say. */

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

/* A REDIR_MOUNT that fails says where: `count` = a MNT_* step and
   `result` = the server's status for it (an NTSTATUS, or LAN Manager's
   (code << 16) | class), 0 when the server never answered.  NET USE on
   the 5x86 against a real server said only "could not be made"
   (2026-11-01). */
#define MNT_NONAME     1       /* the server's name was not found */
#define MNT_CONNECT    2       /* nothing answered on TCP port 445 or 139 */
#define MNT_DIALECT    3       /* no SMB dialect agreed (negotiate) */
#define MNT_NETBIOS    4       /* the NetBIOS session was refused (port 139) */
#define MNT_LOGON      5       /* the user was not accepted (session setup) */
#define MNT_SHARE      6       /* the share was refused (tree connect) */
#define MNT_NODRIVER   7       /* no redirector driver is loaded (kernel) */
#define MNT_MEMORY     8       /* the driver had no memory for the session */
#define MNT_NOSAVED    9       /* NUF_SAVED, and the unlocked password list has
                                  no password for that domain or server */

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
#define NETUSE_PWL    3      /* the password list: EDI -> a pwl_req (below),
                                ECX = its size; the kernel hands the whole
                                block to the redirector (REDIR_PWL) */

#define NUF_INUSE     0x01   /* QUERY: the drive is a mapped net drive */
#define NUF_SMB1      0x02   /* MAP: use the down-level SMB1 client, not SMB2 */
#define NUF_SAVED     0x04   /* MAP: the password - and the user, when none is
                                given - from the unlocked password list, for
                                the domain or, with none, the server */

typedef struct {
    u32  drive;          /* 0 = A */
    u32  status;         /* out: 0, or a DOS error (ERR_*) */
    u32  flags;          /* out: NUF_* for QUERY */
    char server[64];
    char share[80];
    char user[64];
    char pass[64];
    char domain[64];
    u32  step;           /* out, MAP failed: the MNT_* step */
    u32  srvstatus;      /* out, MAP failed: the server's status, or 0 */
} netuse_req;

/* THE PASSWORD LIST (NET LOGON).  A user's saved passwords and remembered
   connections are a file, NAME.PWL beside NET.EXE, encrypted with a key made
   from the list's own password.  NET.EXE reads and writes the file and asks
   for the passwords; SMBFS.SYS does everything else.  Once unlocked, the
   list and its key stay in SMBFS's memory until NET LOGOFF (PWL_LOCK) or a
   restart, and a NUF_SAVED map takes its password from there.  No PWL_*
   call ever hands a saved password, or the key, back to a program: every
   change comes back as the whole file, encrypted (data/len), for NET.EXE to
   write.  DRV\SMBFS\PWL.C has the file format. */
#define PWL_FILE_MAX   12288   /* the largest file: header, entries, tag */
#define PWL_CREDS      24      /* saved passwords in one list */
#define PWL_MAPS       26      /* remembered connections, one a letter */

#define PWL_STATUS     1       /* out: flags 1 if a list is unlocked, owner,
                                  index = passwords saved, drive = connections */
#define PWL_CREATE     2       /* in: owner, pass, seed.  A new empty list,
                                  unlocked; out: the file */
#define PWL_UNLOCK     3       /* in: owner, pass, the file.  Unlock it */
#define PWL_LOCK       4       /* forget the unlocked list (NET LOGOFF) */
#define PWL_ADDCRED    5       /* in: target, user, secret, seed; out: the file */
#define PWL_DELCRED    6       /* in: target (user, if given); out: the file */
#define PWL_ADDMAP     7       /* in: drive, flags, server, share, user, domain,
                                  seed; out: the file */
#define PWL_DELMAP     8       /* in: drive, seed; out: the file */
#define PWL_GETCRED    9       /* in: index; out: target, user - never secret */
#define PWL_GETMAP     10      /* in: index; out: drive, flags, server, share,
                                  user, domain */
#define PWL_CHANGE     11      /* in: owner, pass (the old), newpass, the file,
                                  seed.  Unlocked with the new; out: the file */

#define PWLE_OK        0
#define PWLE_LOCKED    1       /* no list is unlocked */
#define PWLE_BADPASS   2       /* the password does not open the file - or the
                                  file was changed, which GCM cannot tell apart */
#define PWLE_BADFILE   3       /* not a password list this version can read */
#define PWLE_FULL      4       /* no room for another entry */
#define PWLE_NOMORE    5       /* GETCRED/GETMAP: past the last one */
#define PWLE_NOTFOUND  6       /* DELCRED/DELMAP: nothing matched */
#define PWLE_MEMORY    7       /* the driver had no memory for the list */
#define PWLE_OWNER     8       /* the file is another user's list */
#define PWLE_ARGS      9       /* a field the call needs is empty or too long */

typedef struct {
    u32  op;             /* PWL_* */
    u32  status;         /* out: PWLE_* */
    u32  index;          /* in: GETCRED/GETMAP; out STATUS: passwords saved */
    u32  drive;          /* ADDMAP/DELMAP/GETMAP: the letter, 0 = A;
                            out STATUS: connections remembered */
    u32  flags;          /* ADDMAP/GETMAP: NUF_SMB1; out STATUS: 1 unlocked */
    u32  len;            /* in: bytes of the file in data; out: of the new one */
    u32  spare[2];
    u8   seed[64];       /* in: noise for the driver's random numbers - when
                            the keys were pressed, read off a running count */
    char owner[32];      /* the list's user name */
    char pass[64];       /* the list's password (CREATE, UNLOCK; CHANGE's old) */
    char newpass[64];    /* CHANGE: the new password */
    char target[64];     /* a saved password's domain - or server, when the
                            connection names no domain */
    char user[64];
    char secret[64];     /* ADDCRED: the password to save.  Never filled in */
    char server[64];     /* a remembered connection */
    char share[80];
    char domain[64];
    u8   data[PWL_FILE_MAX];     /* the file, in and out */
} pwl_req;

#endif /* REDIR_H */
