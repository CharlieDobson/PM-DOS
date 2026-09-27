/*
 * CHKDSK.H - shared definitions for CHKDSK/32.
 */
#ifndef CHKDSK_H
#define CHKDSK_H

#include "types.h"
#include "rt.h"
#include "sys.h"
#include "msg.h"
#include "fat.h"

/* directory entry layout */
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
/* reserved bits ignored */
#define IS_LFN( ent ) (((ent)[DE_ATTR] & 0x3F) == A_LFN)

#define DEL_MARK  0xE5

/* cluster map flags (vol.map, one byte per cluster) */
#define MF_USED    0x01          /* claimed by a file or directory          */
#define MF_SEEN    0x02          /* lost chain scan: visited                */
#define MF_BAD     0x04          /* marked bad in the FAT                   */
#define MF_LOST    0x08          /* allocated but claimed by nothing        */
#define MF_CROSS   0x10          /* claimed more than once                  */
#define MF_CUR     0x40          /* claimed by the chain being walked now   */
#define MF_HEAD    0x80          /* lost chain scan: first cluster of chain */

typedef struct {
    int   drive;
    int   dofix;                /* /F */
    int   noisy;                /* /V */
    char *filespec;             /* [path]filename to check, or NULL */
} OPTIONS;

typedef struct {
    u32 hid_cnt, hid_clus;
    u32 dir_cnt, dir_clus;
    u32 fil_cnt, fil_clus;
    u32 root_clus;              /* FAT32 root directory clusters    */
    u32 bad_clus;
    u32 orph_cnt, orph_clus;    /* recovered (or would-be) files     */
    u32 lost_kept;              /* lost clusters left allocated      */
    u32 cross_cnt;
    int ftrunc;                 /* part of the tree was not processed */
} STATS;

extern OPTIONS opt;
extern STATS   st;

/* MAIN.C */
void chkdsk_main( void );       /* called by each SYS_*.C entry point */
void chkdsk_abort( void );      /* unlock the drive and exit 255      */

/* SCAN.C */
void scan_pass1( void );
void scan_pass2( void );
void nofix_notice( void );
void report_error( const char *path, const char *text );
int  prompt_yn( const char *text );
u32  entry_first( const u8 *ent );
void entry_set_first( u8 *ent, u32 clus );
void entry_stamp( u8 *ent );
int  dir_fix( DIRBUF *dir, u32 idx );

/* LOST.C */
void lost_chains( void );

/* FRAG.C */
void frag_check( void );

/* LFN.C */
#define LFN_MAX 255
typedef struct {
    int  active;                /* collecting a run of LFN entries    */
    int  ok;                    /* run is consistent so far           */
    u32  start;                 /* index of the first entry of run    */
    int  expect;                /* next sequence number expected      */
    u8   sum;                   /* checksum every entry must carry    */
    u16  name[LFN_MAX + 14];
    int  len;                   /* characters (from first entry)      */
} LFNRUN;

u8   lfn_checksum( const u8 *sfn );
void lfn_begin( LFNRUN *run );
int  lfn_add( LFNRUN *run, const u8 *ent, u32 idx ); /* 0 = previous run broken */
int  lfn_match( LFNRUN *run, const u8 *sfn );        /* complete & valid for sfn */
void lfn_oem( const LFNRUN *run, char *out );        /* LFN_MAX+1 bytes */
void sfn_name( const u8 *ent, char *out );           /* "NAME.EXT", 13 bytes */
int  sfn_is_dot( const u8 *ent );                    /* 1 = ".", 2 = ".." */
int  wild_match( const char *pat, const char *name );
int  fcb_match( const char *pat, const u8 *sfn );

#endif
