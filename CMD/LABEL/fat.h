/*
 * FAT.H - volume geometry, sector I/O, the root directory and the boot
 * sector label field.
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
    u32  nclus;                 /* number of data clusters                 */
    u32  maxclus;               /* highest valid cluster number (nclus+1)  */
    int  mirror;                /* FAT32: all FATs kept in sync            */
    u32  active_fat;            /* FAT32: FAT used when mirroring is off   */
    u32  fsinfo_sec;            /* FAT32: FSInfo sector, 0 = none          */
    int  has_serial;
    u32  serial;
    int  boot_ok;               /* sector 0 holds the BPB in use           */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)

int  vol_open( int drive );                     /* 0, or a DE_* code */
void bpb_from_dpb( const u8 *dpb, u8 *sector ); /* for SYS_*.C */
u32  clus_lba( u32 clus );

/* sector transfers; an error is reported and ends the program */
void read_secs( u32 lba, u32 count, void *buf );
void write_secs( u32 lba, u32 count, const void *buf, int kind );

/*
 * The root directory, read completely into memory.  On FAT32 it is the
 * cluster chain from the BPB; on FAT12/16 the fixed root region.
 */
typedef struct {
    U32LIST clus;               /* FAT32: the chain, in order               */
    u32  nent;                  /* 32-byte entries                          */
    u32  nsec;                  /* sectors in data                          */
    u8  *data;
    u8  *dirty;                 /* one flag per sector of data              */
    int  can_grow;              /* FAT32: chain ends in a proper end mark   */
} ROOTDIR;

void root_load( ROOTDIR *root );
void root_free( ROOTDIR *root );
int  root_next_label( ROOTDIR *root, u32 from );   /* entry index, -1 = none */
int  root_free_slot( ROOTDIR *root );              /* entry index, -1 = full */
int  root_grow( ROOTDIR *root );                   /* FAT32; -1 = disk full  */
void root_touch( ROOTDIR *root, u32 idx );         /* entry idx was changed  */
void root_write( ROOTDIR *root );                  /* changed sectors        */

void boot_set_label( const u8 *label );         /* 11 bytes */

#endif
