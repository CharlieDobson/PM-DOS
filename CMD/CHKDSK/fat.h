/*
 * FAT.H - volume geometry, FAT access and directory buffers.
 */
#ifndef FAT_H
#define FAT_H

#include "types.h"
#include "rt.h"

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
    int      mirror;            /* FAT32: all FATs kept in sync            */
    uint32_t active_fat;        /* FAT32: FAT used when mirroring is off   */
    uint32_t fsinfo_sec;        /* FAT32: FSInfo sector, 0 = none          */
    int      has_serial;
    uint32_t serial;
    uint8_t  media;
    int      writes;            /* nonzero: fixes are written (/F)         */
    uint8_t *fat;               /* the FAT, whole (FAT12/16; NULL on FAT32) */
    uint32_t fatbytes;
    int      fat_dirty;
    uint8_t *map;               /* one flag byte per cluster (chkdsk.h)    */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)
#define IS_EOC( val )   ((val) >= vol.eoc_min)

int      vol_open( int drive );
void     bpb_from_dpb( const uint8_t *dpb, uint8_t *sector ); /* for SYS_*.C */
int      fat_load( void );
uint32_t fat_get( uint32_t clus );
void     fat_set( uint32_t clus, uint32_t val );
int      fat_write( void );
void     fsinfo_update( uint32_t free_clusters );
uint32_t clus_lba( uint32_t clus );
int      read_secs( uint32_t lba, uint32_t count, void *buf );
int      write_secs( uint32_t lba, uint32_t count, const void *buf, int kind );

/*
 * A directory read completely into memory.  Fixed FAT12/16 root
 * directories have fixed_root set; everything else is a cluster list.
 */
typedef struct {
    int          fixed_root;
    U32LIST      clus;
    unsigned int nent;          /* 32-byte entries in data */
    uint8_t     *data;
} DIRBUF;

int  dir_load_root( DIRBUF *dir );                      /* uses FAT for FAT32 */
int  dir_load_list( DIRBUF *dir, const U32LIST *clus ); /* explicit clusters */
int  dir_load_chain( DIRBUF *dir, uint32_t first, uint32_t limit );
int  dir_extend( DIRBUF *dir );                         /* FAT32: add cluster */
void dir_free( DIRBUF *dir );
int  dir_write_entry( DIRBUF *dir, unsigned int idx );  /* honours vol.writes */

#endif
