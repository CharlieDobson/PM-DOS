/*
 * SCANDISK.H - what the parts of SCANDISK share.
 *
 *   MAIN.C     the command line, the drives, the order of work, the
 *              exit code
 *   INI.C      SCANDISK.INI
 *   VOL.C      a volume: its geometry, its sectors, its FAT, directories
 *              read into memory
 *   LFN.C      long file name entries (CHKDSK's)
 *   CHECK.C    the media descriptor, the FAT copies, the directory tree
 *   FILESYS.C  lost clusters and cross-linked files
 *   SURFACE.C  the surface scan, and moving data out of a bad cluster
 *   UNDO.C     the Undo file: what a repair wrote over, and /UNDO
 *   FRAG.C     /FRAGMENT
 *   PROBLEM.C  a problem found: what to do about it, the log, the counts
 *   TEXT.C     every sentence the program says
 *   SCREEN.C   ScanDisk's screens: the front end   }
 *   UI.C       buttons, boxes, a window of text    } not in the
 *   SCR.C      the screen in memory, keys, mouse   } host build
 *   GFX.C      that screen in a graphics mode      }
 *   HOSTFE.C   the front end as lines of text: the host build only
 *   RT.C       the runtime instead of a C library
 *   SYS_D32.C  every call to the kernel
 *   DOSINT.ASM startup and the interrupt thunks;  JMP.ASM  setjmp
 */
#ifndef SCANDISK_H
#define SCANDISK_H

#include "types.h"
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* directory entries                                                   */
/* ------------------------------------------------------------------ */
#define DE_NAME      0
#define DE_ATTR     11
#define DE_NTRES    12
#define DE_CTENTH   13
#define DE_CTIME    14
#define DE_CDATE    16
#define DE_ADATE    18
#define DE_CLUSHI   20
#define DE_TIME     22
#define DE_DATE     24
#define DE_CLUSLO   26
#define DE_SIZE     28
#define DE_LEN      32

#define A_RDONLY  0x01
#define A_HIDDEN  0x02
#define A_SYSTEM  0x04
#define A_VOLID   0x08
#define A_DIR     0x10
#define A_ARCH    0x20
#define A_LFN     0x0F          /* RDONLY|HIDDEN|SYSTEM|VOLID */
#define IS_LFN( ent ) (((ent)[DE_ATTR] & 0x3F) == A_LFN)

#define DEL_MARK  0xE5

/* cluster map flags (vol.map, one byte per cluster) */
#define MF_USED    0x01         /* claimed by a file or directory          */
#define MF_SEEN    0x02         /* lost chain scan: visited                */
#define MF_BAD     0x04         /* marked bad in the FAT                   */
#define MF_LOST    0x08         /* allocated but claimed by nothing        */
#define MF_CROSS   0x10         /* claimed more than once                  */
#define MF_CUR     0x40         /* claimed by the chain being walked now   */
#define MF_HEAD    0x80         /* lost chain scan: first cluster of chain */

/* ------------------------------------------------------------------ */
/* what was asked for                                                  */
/* ------------------------------------------------------------------ */
enum { MODE_ASK, MODE_CHECKONLY, MODE_AUTOFIX, MODE_CUSTOM };

/* what SCANDISK.INI says to do about a kind of problem */
enum { IA_PROMPT, IA_FIX, IA_QUIT, IA_SKIP, IA_DELETE, IA_SAVE };

enum { TRI_AUTO, TRI_ON, TRI_OFF };
enum { SURF_NEVER, SURF_ALWAYS, SURF_PROMPT };
enum { LOG_OFF, LOG_APPEND, LOG_OVERWRITE };

/* the [CUSTOM] settings that name a kind of problem */
enum {
    CAT_MEDIA,                  /* FAT_Media     */
    CAT_OKAY,                   /* Okay_Entries  */
    CAT_CHAIN,                  /* Bad_Chain     */
    CAT_CROSS,                  /* Crosslinks    */
    CAT_MISMATCH,               /* Mismatch_FAT  */
    CAT_BADCLUS,                /* Bad_Clusters  */
    CAT_BADENT,                 /* Bad_Entries   */
    CAT_LOST,                   /* LostClust     */
    CAT_COUNT,
    CAT_NONE = -1               /* nothing can be done about it */
};

typedef struct {
    int          mode;               /* MODE_* */
    int          nosave;             /* /NOSAVE */
    int          surface;            /* /SURFACE */
    int          nosummary;          /* /NOSUMMARY */
    int          graphics;           /* /G: the screens in a graphics mode */
    /* [ENVIRONMENT] */
    int          mouse;
    int          timeout;            /* ScanTimeOut */
    unsigned int passes;             /* NumPasses */
    int          labelcheck;
    int          lfncheck;
    /* [CUSTOM] */
    int          drive_summary;      /* TRI_* */
    int          all_summary;
    int          cust_surface;       /* SURF_* */
    int          save_log;           /* LOG_* */
    int          undo_prompt;
    int          action[CAT_COUNT];  /* IA_* */
} OPTIONS;

extern OPTIONS opt;

void  ini_defaults( void );
int   ini_load( void );         /* 1 = a SCANDISK.INI was read */

/* ------------------------------------------------------------------ */
/* a volume                                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    int      drive;             /* 0 = A:                                  */
    int      fattype;           /* 12, 16 or 32                            */
    uint32_t bps;               /* bytes per sector                        */
    uint32_t spc;               /* sectors per cluster                     */
    uint32_t bpc;               /* bytes per cluster                       */
    uint32_t rsvd;              /* reserved sectors                        */
    uint32_t nfats;
    uint32_t fatsz;             /* sectors per FAT                         */
    uint32_t root_ents;         /* FAT12/16 root directory entries         */
    uint32_t root_sec;          /* FAT12/16 first root directory sector    */
    uint32_t root_secs;         /* FAT12/16 root directory sectors         */
    uint32_t root_clus;         /* FAT32 first root directory cluster      */
    uint32_t data_sec;          /* first sector of cluster 2               */
    uint32_t tot_sec;
    uint32_t nclus;             /* number of data clusters                 */
    uint32_t maxclus;           /* highest valid cluster number (nclus+1)  */
    uint32_t eoc_min;           /* entries >= this end a chain             */
    uint32_t bad_val;           /* bad cluster marker                      */
    uint32_t eoc_val;           /* value written to end a chain            */
    int      mirror;            /* FAT32: all FATs kept in step            */
    uint32_t active_fat;        /* FAT32: the FAT used when they are not   */
    uint32_t fsinfo_sec;        /* FAT32: FSInfo sector, 0 = none          */
    uint32_t backup_sec;        /* FAT32: the backup boot sector, 0 = none */
    int      has_serial;
    uint32_t serial;
    uint8_t  media;             /* the BPB's media descriptor              */
    int      boot_ok;           /* the boot sector itself gave the BPB     */
    char     label[12];         /* the BPB's label, or ""                  */
    uint8_t *fat;               /* the FAT, whole (FAT12/16; NULL on FAT32) */
    uint32_t fatbytes;          /* the part of a FAT that describes clusters */
    uint8_t *map;               /* one flag byte per cluster               */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)
#define IS_EOC( val )   ((val) >= vol.eoc_min)

/* written into a cluster nothing used: the Undo file need not keep
   what was there */
#define WR_NOUNDO       0x100

int      vol_open( int drive );            /* 0 = a FAT volume is there */
void     vol_close( void );
void     bpb_from_dpb( const uint8_t *dpb, uint8_t *sector );   /* for SYS_D32.C */
uint32_t clus_lba( uint32_t clus );
int      read_secs( uint32_t lba, uint32_t count, void *buf );  /* 0 = read */
int      write_secs( uint32_t lba, uint32_t count, const void *buf, int kind );
int      fat_load( uint32_t copy );        /* the working FAT, from that copy */
uint32_t fat_get( uint32_t clus );
void     fat_set( uint32_t clus, uint32_t val );
int      fat_commit( void );               /* what changed, to every copy kept */
int      fat_rewrite( void );              /* the whole FAT, to every copy kept */
void     fat_forget( void );               /* changes not yet written are dropped */
uint32_t fat_free_after( uint32_t clus );  /* a cluster nothing uses, 0 = none */
uint32_t fat_count_free( void );
void     fsinfo_update( uint32_t free_clusters );

/* a FAT copy looked at on its own, a piece at a time */
typedef struct {
    int          unreadable;    /* a sector of it would not read */
    unsigned int invalid;       /* entries that point nowhere */
} FATSTAT;
int   fat_compare( FATSTAT stat[], uint32_t *differing ); /* 0 = it was done */

/* A directory read whole into memory.  A FAT12/16 root has fixed_root
   set; everything else is a list of clusters. */
typedef struct {
    int          fixed_root;
    U32LIST      clus;
    unsigned int nent;          /* 32-byte entries in data */
    uint8_t     *data;
} DIRBUF;

int      dir_load_root( DIRBUF *dir );
int      dir_load_list( DIRBUF *dir, const U32LIST *clus );
int      dir_load_chain( DIRBUF *dir, uint32_t first );
int      dir_extend( DIRBUF *dir );                         /* FAT32: one more cluster */
void     dir_free( DIRBUF *dir );
int      dir_write_entry( DIRBUF *dir, unsigned int idx );  /* the sector it is in */
uint32_t entry_first( const uint8_t *ent );
void     entry_set_first( uint8_t *ent, uint32_t clus );
void     entry_stamp( uint8_t *ent );

/* LFN.C */
#define LFN_MAX 255
typedef struct {
    int          active;        /* collecting a run of LFN entries    */
    int          ok;            /* run is consistent so far           */
    unsigned int start;         /* index of the first entry of run    */
    int          expect;        /* next sequence number expected      */
    uint8_t      sum;           /* checksum every entry must carry    */
    uint16_t     name[LFN_MAX + 14];
    int          len;           /* characters (from first entry)      */
} LFNRUN;

uint8_t lfn_checksum( const uint8_t *sfn );
void    lfn_begin( LFNRUN *run );
int     lfn_add( LFNRUN *run, const uint8_t *ent, unsigned int idx ); /* 0 = previous run broken */
int     lfn_match( LFNRUN *run, const uint8_t *sfn );                 /* complete & valid for sfn */
void    lfn_oem( const LFNRUN *run, char *out );                      /* LFN_MAX+1 bytes */
void    sfn_name( const uint8_t *ent, char *out );                    /* "NAME.EXT", 13 bytes */
int     sfn_is_dot( const uint8_t *ent );                             /* 1 = ".", 2 = ".." */
int     wild_match( const char *pat, const char *name );
int     fcb_match( const char *pat, const uint8_t *sfn );

/* ------------------------------------------------------------------ */
/* the checks                                                          */
/* ------------------------------------------------------------------ */
enum { PH_MEDIA, PH_FAT, PH_DIRS, PH_FILESYS, PH_SURFACE, PH_COUNT };

typedef struct {
    unsigned int hid_cnt;
    uint32_t     hid_clus;
    unsigned int dir_cnt;
    uint32_t     dir_clus;
    unsigned int fil_cnt;
    uint32_t     fil_clus;
    uint32_t     root_clus;        /* FAT32 root directory clusters   */
    uint32_t     bad_clus;         /* marked bad in the FAT           */
    uint32_t     used_clus;        /* the FAT's own count of what is in use */
    unsigned int orph_cnt;         /* lost chains made into files      */
    uint32_t     orph_clus;
    uint32_t     lost_kept;        /* lost clusters left as they were  */
    unsigned int found, fixed;     /* problems, and the ones put right */
    int          ftrunc;           /* part of the tree could not be read */
    int          logical_done;     /* the four checks ran to their end */
    int          surface;          /* 0 not run, 1 finished, 2 left early */
    uint32_t     surf_examined, surf_bad;
} STATS;

extern STATS st;

/* a file or directory whose chain runs into another's */
typedef struct {
    uint32_t     dir_first;     /* the directory the entry is in: its first
                                   cluster, 0 for the root */
    unsigned int idx;           /* the entry */
    uint32_t     clus;          /* the first cluster shared */
    uint32_t     prev;          /* the one before it in this chain, 0 = the
                                   entry itself points at it */
    int          is_dir;
    char        *path;
} CROSSREC;

extern CROSSREC    *cross_list;
extern unsigned int cross_count;

void  check_media( void );
void  check_fats( void );
void  check_dirs( void );
void  check_filesys( void );
void  check_reset( void );              /* between one drive and the next */

/* who has a cluster: found by walking the tree again */
typedef struct {
    int          found;
    uint32_t     dir_first;     /* as CROSSREC's */
    unsigned int idx;
    uint32_t     prev;          /* 0 = the first cluster of the chain */
    int          is_dir;
    int          is_root;       /* the cluster is the FAT32 root's own */
    char        *path;          /* xfree it */
} OWNER;

void  owner_find( uint32_t clus, OWNER *own );
int   dir_load_first( DIRBUF *dir, uint32_t first ); /* 0 = the root */
/* the entry's first cluster, the ".." of its children when it is a
   directory: everything that named "from" names "to" */
int   owner_repoint( const OWNER *own, uint32_t from, uint32_t to );

void  surface_scan( void );

/* ------------------------------------------------------------------ */
/* a problem                                                           */
/* ------------------------------------------------------------------ */
enum {
    PB_MEDIA,                   /* the media descriptor is wrong            */
    PB_FATREAD,                 /* a FAT copy cannot be read                */
    PB_FATMISMATCH,             /* the FAT copies differ                    */
    PB_NAME,                    /* a name with characters a name cannot have */
    PB_ATTR,                    /* attributes that mean nothing             */
    PB_SIZE,                    /* a size that disagrees with the chain     */
    PB_DOTS,                    /* a directory's "." or ".." is wrong       */
    PB_LFN,                     /* long name entries that belong to nothing */
    PB_LABEL,                   /* a volume label with invalid characters   */
    PB_CHAIN,                   /* a chain with a link that goes nowhere    */
    PB_FIRST,                   /* a file whose first cluster is not one    */
    PB_BADDIR,                  /* a directory that is not a directory      */
    PB_CROSS,                   /* two files in the same clusters           */
    PB_LOST,                    /* clusters in use by nothing               */
    PB_BADCLUS,                 /* a cluster that cannot be read            */
    PB_SYSAREA,                 /* a sector of the system area cannot be read */
    PB_DIRREAD,                 /* a directory cannot be read               */
    PB_COUNT
};

enum { CH_FIX, CH_DELETE, CH_SAVEDEL, CH_NONE };
enum { ACT_SKIP, ACT_FIX, ACT_DELETE, ACT_SAVE };

/* a problem has been found: say so, and -> what is to be done (ACT_*) */
int   __cdecl pb_found( int kind, const char *fmt, ... );
void  pb_fixed( void );                             /* it was done */
void  __cdecl pb_failed( const char *fmt, ... );    /* it could not be, and why */
int   pb_choices( int kind );
int   pb_category( int kind );
const char *pb_info( int kind );                    /* "More Info" */
const char *phase_name( int phase );
const char *phase_info( int phase );

/* why a check stops early */
enum {
    Q_USER = 1,                 /* Exit, or ^C */
    Q_CUSTOM,                   /* SCANDISK.INI said Quit */
    Q_DRIVE,                    /* this drive cannot be checked any further */
    Q_NOMEM
};
void  scan_quit( int why );
void  scan_tick( void );        /* a safe place to stop: is one asked for? */

/* the log: its lines fit the window ScanDisk shows the log in, and one
   that begins with '\1' is a heading there */
#define LOG_WIDTH       64
extern TEXT log_text;
void  __cdecl log_line( const char *fmt, ... );
void  log_para( const char *text );     /* wrapped, indented */
int   log_save( int append, char *where );      /* 0 = saved; where it went */

/* ------------------------------------------------------------------ */
/* the Undo file                                                       */
/* ------------------------------------------------------------------ */
int   undo_create( int drive );         /* SCANDISK.UND in its root; 0 = made */
int   undo_active( void );
/* "count" sectors at "lba" are about to become "after" */
int   undo_note( uint32_t lba, uint32_t count, const void *after );
void  undo_close( void );
int   undo_restore( int drive );        /* /UNDO: an exit code */
void  undo_want( void );                /* before the first repair of a drive:
                                           ask whether there is to be one */
void  undo_drive_done( void );
void  undo_avoid( int drive );          /* that drive is about to be checked */

/* FRAG.C */
int   frag_report( int drive, const char *spec );   /* an exit code */

/* ------------------------------------------------------------------ */
/* the front end: ScanDisk's screens (SCREEN.C).  The host build has   */
/* none and says the same things as lines of text (HOSTFE.C).          */
/* ------------------------------------------------------------------ */
void  fe_start( void );
void  fe_finish( void );
void  fe_drive_begin( void );
void  fe_phase( int phase );            /* PH_COUNT: the checks are over */
/* how far a check has got - and the place a check is left from: total
   0 is "nothing new, but is anyone asking to stop?" */
void  fe_progress( unsigned int done, unsigned int total );
int   fe_problem( int kind, const char *text );     /* ACT_*; Exit does not return */
/* news of a problem nobody is being asked about: -1 = it was found,
   0 = it is being left, 1 = it was repaired, 2 = the repair failed */
void  fe_outcome( int news, const char *text );
int   fe_ask_surface( void );           /* 1 = yes */
int   fe_ask_undo( int have_a, int have_b );        /* 0 = A:, 1 = B:, -1 = none */
void  fe_message( const char *text );   /* something that has to be said */
int   fe_confirm( const char *title, const char *text );    /* 1 = yes */
void  fe_busy( const char *text );      /* a job that is not a check: what it
                                           is, and fe_progress() says how far */
void  fe_surface_begin( void );
void  fe_surface_at( uint32_t clus, uint32_t count, int bad );
void  fe_drive_end( int more );         /* the summary; more = work follows */
void  fe_all_end( int drives, unsigned int found, unsigned int fixed );

/* the summary of a drive, as lines: the log's, and the screen's */
void  summary_text( TEXT *text );
const char *drive_verdict( void );
char  *fmt_date( uint16_t dos_date, char *buf );
char  *fmt_time( uint16_t dos_time, char *buf );

extern COUNTRY country;
extern const char help_text[];

void  scandisk_main( void );    /* called by each SYS_*.C entry point */
void  say( const char *text );  /* a line on standard output: no screen is up */
void  leave( int code );        /* the screen given back, the drive let go, and out */

#endif
