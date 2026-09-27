/*
 * LABEL.H - shared definitions for LABEL/32.
 */
#ifndef LABEL_H
#define LABEL_H

#include "types.h"
#include "rt.h"
#include "sys.h"
#include "msg.h"
#include "fat.h"

/* directory entry layout */
#define DE_ATTR     11
#define DE_TIME     22
#define DE_DATE     24
#define DE_LEN      32

#define A_VOLID   0x08
#define A_DIR     0x10
#define A_LFN     0x0F          /* RDONLY|HIDDEN|SYSTEM|VOLID */
/* reserved bits ignored */
#define IS_LFN( ent ) (((ent)[DE_ATTR] & 0x3F) == A_LFN)

#define DEL_MARK  0xE5          /* deleted entry                          */
#define E5_ALIAS  0x05          /* stored for a name that starts with E5h */

#define LABEL_LEN 11

/* MAIN.C */
void label_main( void );        /* called by each SYS_*.C entry point  */
void disk_error( int code, int writing );   /* report, unlock, exit 1 */

#endif
