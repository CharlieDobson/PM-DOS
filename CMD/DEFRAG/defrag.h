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
    int  drive;                 /* 0 = A:                                  */
    int  fattype;               /* 12, 16 or 32                            */
    u32  bps;                   /* bytes per sector                        */
    u32  spc;                   /* sectors per cluster                     */
    u32  bpc;                   /* bytes per cluster                       */
    u32  rsvd;                  /* reserved sectors                        */
    u32  nfats;
    u32  fatsz;                 /* sectors per FAT                         */
    u32  root_ents;             /* FAT12/16 root directory entries         */
    u32  root_sec;              /* FAT12/16 first root directory sector    */
    u32  root_secs;             /* FAT12/16 root directory sectors         */
    u32  root_clus;             /* FAT32 first root directory cluster      */
    u32  data_sec;              /* first sector of cluster 2               */
    u32  tot_sec;
    u32  nclus;                 /* number of data clusters                 */
    u32  maxclus;               /* highest valid cluster number (nclus+1)  */
    u32  eoc_min;               /* entries >= this end a chain             */
    u32  bad_val;               /* bad cluster marker                      */
    u32  eoc_val;               /* value written to end a chain            */
    int  mirror;                /* FAT32: all FATs kept in step            */
    u32  active_fat;            /* FAT32: the FAT used when they are not   */
    u32  fsinfo_sec;            /* FAT32: FSInfo sector, 0 = none          */
    u8  *fat;                   /* the FAT, whole                          */
    u32  fatbytes;              /* the part of a FAT that describes clusters */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)
#define IS_EOC( val )   ((val) >= vol.eoc_min)

int   vol_open( int drive );            /* 0 = a FAT volume is there */
void  vol_close( void );
void  bpb_from_dpb( const u8 *dpb, u8 *sector );    /* for SYS_D32.C */
u32   clus_lba( u32 clus );
int   read_secs( u32 lba, u32 count, void *buf );   /* 0 = read */
int   write_secs( u32 lba, u32 count, const void *buf, int kind );
int   fat_load( void );                 /* 0 = read; -1 = it would not read */
u32   fat_get( u32 clus );
void  fat_set( u32 clus, u32 val );
int   fat_commit( void );               /* what changed, to every copy kept */
u32   fat_differing( void );            /* sectors in which a copy disagrees
                                           with the one in memory */
void  fsinfo_update( u32 free_clusters, u32 next_free );

/* A directory read whole into memory.  A FAT12/16 root has fixed_root
   set; everything else is a list of clusters. */
typedef struct {
    int  fixed_root;
    U32LIST clus;
    u32  nent;                  /* 32-byte entries in data */
    u8  *data;
} DIRBUF;

int   dir_load_root( DIRBUF *dir );
int   dir_load_list( DIRBUF *dir, const U32LIST *clus );
void  dir_free( DIRBUF *dir );
int   dir_write_entry( DIRBUF *dir, u32 idx );  /* the sector it is in */
int   dir_write_all( DIRBUF *dir );             /* every sector, first to last */
/* sector "sec" of the directory, from "image" and not from its own copy */
int   dir_write_sec( DIRBUF *dir, const u8 *image, u32 sec );
u32   entry_first( const u8 *ent );
void  entry_set_first( u8 *ent, u32 clus );

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
    u32  parent;                /* the directory it is in: a node */
    u32  ent;                   /* its 8.3 entry in that directory */
    u32  first;                 /* its first cluster, 0 = it has none */
    u32  nclus;
    u32  target;                /* where it is to start, 0 = nowhere new */
    u32  dir;                   /* NF_DIR: which of dirs[] */
    u8   flags;
    u8   attr;
} NODE;

typedef struct {
    u32    node;
    DIRBUF buf;
    u32   *who;                 /* for each entry: the node it is the 8.3
                                   entry of, or N_NONE */
    u32    dead;                /* deleted entries ahead of the end mark */
} DIRINFO;

extern NODE    *nodes;
extern u32      nnodes;
extern DIRINFO *dirs;
extern u32      ndirs;
extern u32     *own;            /* for each cluster: the node that has it */
extern u32     *prev;           /* ...and the cluster before it in that
                                   node's chain, 0 = it is the first */

/* what is wrong with a drive: bits, and how much of each */
#define FSE_FATDIFF     0x01    /* the copies of the FAT differ */
#define FSE_CHAIN       0x02    /* a chain that goes nowhere a chain can */
#define FSE_CROSS       0x04    /* two files in the same cluster */
#define FSE_LOST        0x08    /* clusters in use by nothing */
#define FSE_SIZE        0x10    /* a size that disagrees with the chain */
#define FSE_DIR         0x20    /* a directory that is not laid out as one */

typedef struct {
    int  errors;                /* FSE_* */
    u32  lost;                  /* lost clusters */
    u32  cross;                 /* cross-linked files */
    u32  files, folders;        /* what there is */
    u32  used, free, bad, fixed;    /* clusters */
    u32  frag_items;            /* files and directories in pieces */
    u32  movable;               /* clusters of what may be moved */
    u32  gaps;                  /* free clusters below the last one in use */
    u32  top;                   /* the last cluster in use */
} DRIVESTAT;

extern DRIVESTAT dst;

int   tree_read( void );        /* 0 = read; DFE_* when it could not be */
void  tree_free( void );
void  tree_stats( void );       /* dst, from the tree as it stands now */
void  tree_unplace( void );     /* a new run: nothing is in its final place */
int   tree_percent( void );     /* how much of the drive is not fragmented */
int   tree_recommend( void );   /* METHOD_* */
const char *node_name( u32 node, char *buf );   /* "NAME.EXT", 13 bytes */

/* ------------------------------------------------------------------ */
/* the map: the drive as blocks of clusters                            */
/* ------------------------------------------------------------------ */
typedef struct {
    u32 used;                   /* clusters of the block in use */
    u32 fixed;                  /* ...by something that is not moved */
    u32 bad;
    u32 done;                   /* ...in their final place */
} BLOCK;

extern BLOCK *blocks;
extern u32    nblocks, per_block;

void  map_build( u32 cells );   /* a map of at most that many blocks */
void  map_count( u32 clus, int used, int fixed, int done );     /* +1 or -1 each */
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
int   dir_arrange( u32 dirno, int sort );   /* 1 = it has to be written now */
int   dir_check( u32 dirno, int sort );     /* 1 = arranging would change it */
void  dir_children( u32 dirno, int sort, U32LIST *out );    /* in that order */
void  dir_forget( void );

/* what dir_arrange last changed, for whoever has to write it */
typedef struct {
    u32  dirno;
    int  sorted;                /* not only squeezed: entries went both ways */
    u32  old_end, new_end;      /* entries in use, before and after */
    u8  *old;                   /* the directory as it was */
} ARRANGED;
extern ARRANGED arranged;

/* MOVE.C: 0, or DFE_READ / DFE_WRITE with nothing changed */
int   move_run( u32 node, u32 before, const u32 *src, u32 count, u32 dst );
int   move_dir( u32 dirno, u32 dst );       /* a whole directory, from memory */
int   dir_rewrite( u32 dirno );             /* where it is */
u32   move_limit( void );                   /* the most clusters in one move */
void  move_done( void );
extern u32 move_error_clus;                 /* the cluster a failure was at */

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
void  fe_suspend( void );                   /* the screen given back while
                                               another program runs... */
void  fe_resume( void );                    /* ...and taken again */
void  fe_drive( int loaded );               /* a drive has been read: its map;
                                               0 = there is none any more */
void  fe_reading( const char *what );       /* the tree is being read: a
                                               directory's name, or NULL */
void  fe_run( int running );                /* a drive is being optimized */
/* a move: the clusters being read or written, 0 clusters for neither */
void  fe_activity( int what, u32 clus, u32 count );
void  fe_progress( u32 done, u32 total );   /* clusters put where they go */
int   fe_stop_asked( void );                /* 1 = stop here */
int   fe_log_drive( void );                 /* test mode: the drive the log is
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
