/*
 * DEFRAG.H - what the parts of DEFRAG share.
 *
 *   MAIN.C     the command line, the order of work, the exit code
 *   VOL.C      a volume: its geometry, its sectors, its FAT, directories
 *              read into memory (SCANDISK's)
 *   TREE.C     every file and directory of a drive, who has each
 *              cluster, and whether the file system is sound
 *   DIRS.C     a directory put in order: sorted, its deleted entries
 *              squeezed out
 *   MOVE.C     clusters moved, safely: the one place the disk is changed
 *   OPTIM.C    where everything is to go, and the going there
 *   TEXT.C     every sentence the program says
 *   SCREEN.C   DEFRAG's screen: the map, the boxes        }
 *   UI.C       a menu, buttons, lists, a window of text   } not in the
 *   SCR.C      the screen in memory, keys, mouse          } host build
 *   GFX.C      that screen in a graphics mode             }
 *   HOSTFE.C   the front end as lines of text: the host build only
 *   RT.C       the runtime instead of a C library
 *   SYS_D32.C  every call to the kernel
 *   DOSINT.ASM startup and the interrupt thunks;  JMP.ASM  setjmp
 */
#ifndef DEFRAG_H
#define DEFRAG_H

#include "types.h"
#include "rt.h"
#include "sys.h"

/* ------------------------------------------------------------------ */
/* directory entries                                                   */
/* ------------------------------------------------------------------ */
#define DE_NAME      0
#define DE_ATTR     11
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

/* ------------------------------------------------------------------ */
/* what was asked for                                                  */
/* ------------------------------------------------------------------ */
enum { METHOD_FULL, METHOD_UNFRAG, METHOD_NONE };   /* NONE: only ever a
                                                       recommendation */
enum { SORT_NONE, SORT_NAME, SORT_EXT, SORT_DATE, SORT_SIZE };

typedef struct {
    int  drive;                 /* 0 = A:, -1 = none named */
    int  method;                /* METHOD_FULL or METHOD_UNFRAG */
    int  unattended;            /* /F or /U given: no questions, and out
                                   when the drive is done */
    int  sort;                  /* SORT_* */
    int  descending;
    int  hidden;                /* /H: hidden files are moved too */
    int  restart;               /* /B: restart the machine afterwards */
    int  graphics;              /* /G: the screen in a graphics mode */
} OPTIONS;

extern OPTIONS opt;

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
    uint8_t *fat;               /* the FAT, whole                          */
    uint32_t fatbytes;          /* the part of a FAT that describes clusters */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)
#define IS_EOC( val )   ((val) >= vol.eoc_min)

int      vol_open( int drive );         /* 0 = a FAT volume is there */
void     vol_close( void );
void     bpb_from_dpb( const uint8_t *dpb, uint8_t *sector );   /* for SYS_D32.C */
uint32_t clus_lba( uint32_t clus );
int      read_secs( uint32_t lba, uint32_t count, void *buf );  /* 0 = read */
int      write_secs( uint32_t lba, uint32_t count, const void *buf, int kind );
int      fat_load( void );              /* 0 = read; -1 = it would not read */
uint32_t fat_get( uint32_t clus );
void     fat_set( uint32_t clus, uint32_t val );
int      fat_commit( void );            /* what changed, to every copy kept */
uint32_t fat_differing( void );         /* sectors in which a copy disagrees
                                           with the one in memory */
void     fsinfo_update( uint32_t free_clusters, uint32_t next_free );

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
void     dir_free( DIRBUF *dir );
int      dir_write_entry( DIRBUF *dir, unsigned int idx );  /* the sector it is in */
int      dir_write_all( DIRBUF *dir );                      /* every sector, first to last */
/* sector "sec" of the directory, from "image" and not from its own copy */
int      dir_write_sec( DIRBUF *dir, const uint8_t *image, unsigned int sec );
uint32_t entry_first( const uint8_t *ent );
void     entry_set_first( uint8_t *ent, uint32_t clus );

/* ------------------------------------------------------------------ */
/* the tree: every file and directory, and who has each cluster        */
/* ------------------------------------------------------------------ */
#define NF_DIR      0x01
#define NF_FIXED    0x02        /* not to be moved: the map's X */
#define NF_FRAG     0x04        /* in more than one piece */
#define NF_PLACED   0x08        /* where it is going to stay */

#define N_NONE      0           /* nobody: a cluster nothing owns */
#define N_ROOT      1           /* the root directory */

typedef struct {
    unsigned int parent;        /* the directory it is in: a node */
    unsigned int ent;           /* its 8.3 entry in that directory */
    uint32_t     first;         /* its first cluster, 0 = it has none */
    uint32_t     nclus;
    uint32_t     target;        /* where it is to start, 0 = nowhere new */
    unsigned int dir;           /* NF_DIR: which of dirs[] */
    uint8_t      flags;
    uint8_t      attr;
} NODE;

typedef struct {
    unsigned int  node;
    DIRBUF        buf;
    unsigned int *who;          /* for each entry: the node it is the 8.3
                                   entry of, or N_NONE */
    unsigned int  dead;         /* deleted entries ahead of the end mark */
} DIRINFO;

extern NODE         *nodes;
extern unsigned int  nnodes;
extern DIRINFO      *dirs;
extern unsigned int  ndirs;
extern unsigned int *own;       /* for each cluster: the node that has it */
extern uint32_t     *prev;      /* ...and the cluster before it in that
                                   node's chain, 0 = it is the first */

/* what is wrong with a drive: bits, and how much of each */
#define FSE_FATDIFF     0x01    /* the copies of the FAT differ */
#define FSE_CHAIN       0x02    /* a chain that goes nowhere a chain can */
#define FSE_CROSS       0x04    /* two files in the same cluster */
#define FSE_LOST        0x08    /* clusters in use by nothing */
#define FSE_SIZE        0x10    /* a size that disagrees with the chain */
#define FSE_DIR         0x20    /* a directory that is not laid out as one */

typedef struct {
    int          errors;        /* FSE_* */
    uint32_t     lost;          /* lost clusters */
    unsigned int cross;         /* cross-linked files */
    unsigned int files, folders; /* what there is */
    uint32_t     used, free, bad, fixed; /* clusters */
    unsigned int frag_items;    /* files and directories in pieces */
    uint32_t     movable;       /* clusters of what may be moved */
    uint32_t     gaps;          /* free clusters below the last one in use */
    uint32_t     top;           /* the last cluster in use */
} DRIVESTAT;

extern DRIVESTAT dst;

int   tree_read( void );        /* 0 = read; DFE_* when it could not be */
void  tree_free( void );
void  tree_stats( void );       /* dst, from the tree as it stands now */
void  tree_unplace( void );     /* a new run: nothing is in its final place */
int   tree_percent( void );     /* how much of the drive is not fragmented */
int   tree_recommend( void );   /* METHOD_* */
const char *node_name( unsigned int node, char *buf ); /* "NAME.EXT", 13 bytes */

/* ------------------------------------------------------------------ */
/* the map: the drive as blocks of clusters                            */
/* ------------------------------------------------------------------ */
typedef struct {
    uint32_t used;              /* clusters of the block in use */
    uint32_t fixed;             /* ...by something that is not moved */
    uint32_t bad;
    uint32_t done;              /* ...in their final place */
} BLOCK;

extern BLOCK       *blocks;
extern unsigned int nblocks;
extern uint32_t     per_block;

void  map_build( unsigned int cells ); /* a map of at most that many blocks */
void  map_count( uint32_t clus, int used, int fixed, int done ); /* +1 or -1 each */
#define BLOCK_OF( clus ) (((clus) - 2) / per_block)

/* ------------------------------------------------------------------ */
/* the work                                                            */
/* ------------------------------------------------------------------ */

/* why a run stops: the exit codes are MS-DOS DEFRAG's */
enum {
    DFE_OK      = 0,
    DFE_INTERNAL = 1,           /* something that cannot be */
    DFE_NOFREE  = 2,            /* not one free cluster to work with */
    DFE_USER    = 3,            /* stopped by whoever is at the keyboard */
    DFE_GENERAL = 4,
    DFE_READ    = 5,            /* a cluster would not read */
    DFE_WRITE   = 6,            /* a cluster would not write */
    DFE_ALLOC   = 7,            /* the file system is not sound */
    DFE_MEMORY  = 8,
    DFE_NOMEM   = 9             /* not enough memory for this drive */
};

/* DIRS.C: a directory put in order, in memory only */
int   dir_arrange( unsigned int dirno, int sort );  /* 1 = it has to be written now */
int   dir_check( unsigned int dirno, int sort );    /* 1 = arranging would change it */
void  dir_children( unsigned int dirno, int sort, U32LIST *out ); /* in that order */
void  dir_forget( void );

/* what dir_arrange last changed, for whoever has to write it */
typedef struct {
    unsigned int dirno;
    int          sorted;        /* not only squeezed: entries went both ways */
    unsigned int old_end, new_end; /* entries in use, before and after */
    uint8_t     *old;           /* the directory as it was */
} ARRANGED;
extern ARRANGED arranged;

/* MOVE.C: 0, or DFE_READ / DFE_WRITE with nothing changed */
int      move_run( unsigned int node, uint32_t before, const uint32_t *src, uint32_t count, uint32_t dst );
int      move_dir( unsigned int dirno, uint32_t dst );  /* a whole directory, from memory */
int      dir_rewrite( unsigned int dirno );             /* where it is */
uint32_t move_limit( void );                            /* the most clusters in one move */
void     move_done( void );
extern uint32_t move_error_clus;                        /* the cluster a failure was at */

int   optimize( void );         /* DFE_* */
int   optimize_needed( void );  /* 1 = the chosen method has work to do */

/* ------------------------------------------------------------------ */
/* the front end: DEFRAG's screen (SCREEN.C).  The host build has none */
/* and says the same things as lines of text (HOSTFE.C).               */
/* ------------------------------------------------------------------ */
enum { ACT_IDLE, ACT_READ, ACT_WRITE, ACT_SORT, ACT_FAT };

/* what the Optimize menu answers */
enum { GO_BEGIN = 1, GO_DRIVE, GO_METHOD, GO_SORT, GO_LEGEND, GO_ABOUT, GO_EXIT };

/* what is asked when a drive is done */
enum { DONE_ANOTHER, DONE_CONFIGURE, DONE_EXIT };

void  fe_start( void );
void  fe_finish( void );
void  fe_suspend( void );                            /* the screen given back while
                                                        another program runs... */
void  fe_resume( void );                             /* ...and taken again */
void  fe_drive( int loaded );                        /* a drive has been read: its map;
                                                        0 = there is none any more */
void  fe_reading( const char *what );                /* the tree is being read: a
                                                        directory's name, or NULL */
void  fe_run( int running );                         /* a drive is being optimized */
/* a move: the clusters being read or written, 0 clusters for neither */
void  fe_activity( int what, uint32_t clus, uint32_t count );
void  fe_progress( uint32_t done, uint32_t total );  /* clusters put where they go */
int   fe_stop_asked( void );                         /* 1 = stop here */
int   fe_log_drive( void );                          /* test mode: the drive the log is
                                                        on, which cannot be optimized; -1 */

int   fe_menu( void );                      /* the Optimize menu: a GO_* */
int   fe_pick_drive( int now );             /* a drive, or -1 */
int   fe_pick_method( void );               /* sets opt.method; 1 = it was chosen */
void  fe_pick_sort( void );                 /* sets opt.sort, opt.descending */
void  fe_legend( void );
void  fe_about( void );
int   fe_recommend( int percent, int method );  /* 1 = optimize, 0 = configure */
int   fe_fs_error( const char *text );      /* 1 = run ScanDisk, 0 = leave */
void  fe_message( const char *text, int error );    /* something that has to
                                                       be said, and seen */
void  fe_finished( void );                  /* a run is over */
int   fe_done( void );                      /* a DONE_* */

extern COUNTRY country;
extern const char help_text[];

void  defrag_main( void );      /* called by each SYS_*.C entry point */
void  say( const char *text );  /* a line on standard output: no screen is up */
void  leave( int code );        /* the screen and the drive given back, and out */

#endif
