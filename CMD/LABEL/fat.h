/*
 * FAT.H - volume geometry, sector I/O, the root directory and the boot
 * sector label field.
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
    uint32_t nclus;             /* number of data clusters                 */
    uint32_t maxclus;           /* highest valid cluster number (nclus+1)  */
    int      mirror;            /* FAT32: all FATs kept in sync            */
    uint32_t active_fat;        /* FAT32: FAT used when mirroring is off   */
    uint32_t fsinfo_sec;        /* FAT32: FSInfo sector, 0 = none          */
    int      has_serial;
    uint32_t serial;
    int      boot_ok;           /* sector 0 holds the BPB in use           */
    int      exfat;             /* exFAT: fattype is 0, rsvd is the FAT's  */
                                /* offset and root_clus the root's cluster */
} VOLUME;

extern VOLUME vol;

#define CLUS_OK( clus ) ((clus) >= 2 && (clus) <= vol.maxclus)

int      vol_open( int drive );                               /* 0, or a DE_* code */
void     bpb_from_dpb( const uint8_t *dpb, uint8_t *sector ); /* for SYS_*.C */
uint32_t clus_lba( uint32_t clus );

/* sector transfers; an error is reported and ends the program */
void read_secs( uint32_t lba, uint32_t count, void *buf );
void write_secs( uint32_t lba, uint32_t count, const void *buf, int kind );

/*
 * The root directory, read completely into memory.  On FAT32 it is the
 * cluster chain from the BPB; on FAT12/16 the fixed root region.
 */
typedef struct {
    U32LIST      clus;          /* FAT32: the chain, in order               */
    unsigned int nent;          /* 32-byte entries                          */
    uint32_t     nsec;          /* sectors in data                          */
    uint8_t     *data;
    uint8_t     *dirty;         /* one flag per sector of data              */
    int          can_grow;      /* FAT32: chain ends in a proper end mark   */
} ROOTDIR;

void root_load( ROOTDIR *root );
void root_free( ROOTDIR *root );
int  root_next_label( ROOTDIR *root, unsigned int from );  /* entry index, -1 = none */
int  root_free_slot( ROOTDIR *root );                      /* entry index, -1 = full */
int  root_grow( ROOTDIR *root );                           /* FAT32; -1 = disk full */
void root_touch( ROOTDIR *root, unsigned int idx );        /* entry idx was changed */
void root_write( ROOTDIR *root );                          /* changed sectors    */

void boot_set_label( const uint8_t *label );    /* 11 bytes */

/* exFAT: the label entry in the root, the only place exFAT keeps one */
int  xf_get_label( char *name );            /* 1 = a label, in name[12]; 0 = none */
int  xf_set_label( const uint8_t *label );  /* 11 bytes, NULL = none; -1 = full */

#endif
