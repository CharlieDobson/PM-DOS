/*
 * FAT.H - volume geometry, FAT access and directory buffers.
 */
#ifndef FAT_H
#define FAT_H

#include "types.h"
#include "rt.h"

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
    int  mirror;                /* FAT32: all FATs kept in sync            */
    u32  active_fat;            /* FAT32: FAT used when mirroring is off   */
    u32  fsinfo_sec;            /* FAT32: FSInfo sector, 0 = none          */
    int  has_serial;
    u32  serial;
    u8   media;
    int  writes;                /* nonzero: fixes are written (/F)         */
    u8  *fat;                   /* the FAT, whole (FAT12/16; NULL on FAT32) */
    u32  fatbytes;
    int  fat_dirty;
    u8  *map;                   /* one flag byte per cluster (chkdsk.h)    */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)
#define IS_EOC( val )   ((val) >= vol.eoc_min)

int  vol_open( int drive );
void bpb_from_dpb( const u8 *dpb, u8 *sector ); /* for SYS_*.C */
int  fat_load( void );
u32  fat_get( u32 clus );
void fat_set( u32 clus, u32 val );
int  fat_write( void );
void fsinfo_update( u32 free_clusters );
u32  clus_lba( u32 clus );
int  read_secs( u32 lba, u32 count, void *buf );
int  write_secs( u32 lba, u32 count, const void *buf, int kind );

/*
 * A directory read completely into memory.  Fixed FAT12/16 root
 * directories have fixed_root set; everything else is a cluster list.
 */
typedef struct {
    int  fixed_root;
    U32LIST clus;
    u32  nent;                  /* 32-byte entries in data */
    u8  *data;
} DIRBUF;

int  dir_load_root( DIRBUF *dir );                      /* uses FAT for FAT32 */
int  dir_load_list( DIRBUF *dir, const U32LIST *clus ); /* explicit clusters */
int  dir_load_chain( DIRBUF *dir, u32 first, u32 limit );
int  dir_extend( DIRBUF *dir );                         /* FAT32: add cluster */
void dir_free( DIRBUF *dir );
int  dir_write_entry( DIRBUF *dir, u32 idx );           /* honours vol.writes */

#endif
