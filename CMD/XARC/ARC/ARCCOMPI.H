/*===========================================================================
 * ARCCOMPI.H  -  Inside ARCCOMP: what the scanner hands the two writers
 * Target: MSVC 2.2  Win32s
 *
 * Private to ARCCOMP.C, ZIPWRITE.C and SZWRITE.C.  A front end includes
 * ARCCOMP.H and never this.
 *
 * ONE ENTRY PER FILE OR FOLDER, and an entry does not carry its source path:
 * it carries the path RELATIVE to the folder its item was named in, plus the
 * number of that folder.  A flat archive stores only the last component of
 * the relative path, a pathed one stores all of it, and the file on disk is
 * the base folder joined to it - so one string serves all three, and ten
 * thousand files cost ten thousand short strings rather than three long ones
 * each.  That is the difference between fitting and not on a 4 MB machine.
 *===========================================================================*/

#ifndef ARCCOMPI_H
#define ARCCOMPI_H

#include <stdio.h>
#include "arccomp.h"

/* How a file is to be packed, decided from its first few kilobytes. */
#define CK_DATA     0       /* compress it                                 */
#define CK_EXE      1       /* x86 code: the branch filter, then LZMA (7z)  */
#define CK_PACKED   2       /* already compressed: store it                 */
#define CK_DELTA    3       /* samples in fixed frames (PCM sound, true-
                             * colour pixels): the Delta filter, then LZMA
                             * (7z); deflated as data in a zip             */

/* What became of an entry while the archive was written. */
#define CS_PENDING  0
#define CS_DONE     1
#define CS_SKIPPED  2       /* could not be opened when its turn came       */
#define CS_DROPPED  3       /* left out at scan time (a duplicate name)     */

typedef struct {
    char    *rel;           /* path from the base folder, '\' separated     */
    uint16_t base;          /* index into job->bases                        */
    uint8_t  isDir;
    uint8_t  kind;          /* CK_*                                         */
    uint8_t  text;          /* looks like text (zip's internal attribute)   */
    uint8_t  state;         /* CS_*                                         */
    uint8_t  method;        /* zip: 0 stored, 8 deflated                    */
    uint8_t  folder;        /* 7z: which coded folder                       */
    uint8_t  delta;         /* CK_DELTA: the frame size, 1..255             */
    uint32_t attr;          /* DOS attribute bits                           */
    uint32_t size;          /* as found by the scan                         */
    uint16_t dosDate;       /* last write, local, packed                    */
    uint16_t dosTime;
    uint32_t mtLo, mtHi;    /* last write, UTC FILETIME                     */

    /* Filled in as the entry is written. */
    uint32_t crc;
    uint32_t realSize;      /* bytes actually read                          */
    uint32_t packed;        /* zip: bytes of compressed data                */
    uint32_t offset;        /* zip: where its local header starts           */
} CompEntry;

struct ArcCompJob {
    int        fmt;
    int        keepPaths;
    int        recurse;
    int        clearArc;    /* take the A bit off what went in              */

    char     **items;      /* as given to ArcCompAdd                       */
    int        nItems;

    char     **bases;       /* full folder paths, each ending in '\'        */
    int        nBases;

    char     **excl;        /* exclusion patterns                           */
    int        nExcl;

    CompEntry *ents;
    uint32_t   nEnts;
    uint32_t   capEnts;
    int        scanned;

    uint32_t  *order;       /* the order the writer goes through ents in    */
    uint32_t   nOrder;

    char       outFull[SZ_MAX_NAME];   /* the archive's own full path       */
    uint32_t   totalBytes;             /* of everything to be read          */

    /* Results. */
    int        nFiles;
    uint32_t   inBytes;
    uint32_t   outBytes;
    int        nDup, nUnread, nLong, nBig, nMissing;
    char       problem[SZ_MAX_NAME];
    uint32_t   dictSize;

    /* Progress, while a run is going. */
    ArcCompProgress prog;
    void      *user;
    uint32_t   doneBytes;
    int        cancelled;
};

/* The name an entry is stored under: its whole relative path, or just the
 * last part of it in a flat archive.  Points into the entry. */
const char *CompStoredName( ArcCompJob *j, const CompEntry *e );

/* The entry's file on disk, built into 'dst'. */
void   CompSourcePath( ArcCompJob *j, const CompEntry *e, char *dst, int size );

/* Open an entry's file for reading; NULL (and the entry marked CS_SKIPPED,
 * and counted, and named in ArcCompProblem if it is the first) when it is
 * not there any more or cannot be opened. */
FILE  *CompOpenSource( ArcCompJob *j, CompEntry *e );

/* Report progress; returns 0 when the user has asked to stop (and remembers
 * that, so every later call says 0 too). */
int    CompProgress( ArcCompJob *j, int index, int count, const char *name );

/* Note the first problem file. */
void   CompSetProblem( ArcCompJob *j, const char *name );

/* Decide CK_* (and the text flag) from the first bytes of a file, whose
 * whole length is 'fileSize'.  For CK_DELTA, *delta receives the frame size
 * (1..255); either pointer may be NULL. */
int    CompSniff( const uint8_t *buf, uint32_t len, uint32_t fileSize,
                  int *isText, unsigned *delta );

/* Writers.  Each writes the whole archive to 'f', which is open for binary
 * writing and positioned at 0, and fills j->outBytes.  SZ_OK or SZ_ERR_*. */
int    ZipWriteArchive( ArcCompJob *j, FILE *f );
int    SzWriteArchive( ArcCompJob *j, FILE *f );

/*---- Lent by SZWRITE.C to SZARC.C ----------------------------------------- *
 * Deleting from a 7z writes a header again and can compress a folder again,
 * and the pieces for both are here already.  SZARC.C is the only other user.
 *
 * HBuf    a byte buffer that grows; 'failed' is set when memory runs out and
 *         every later put is then ignored, so a caller checks once at the end.
 * HNum    7z's variable-length number.  HBits: a bit vector, MSB first.
 * BcjEnc  the x86 branch filter, encoding: BcjEncChunk converts in place and
 *         returns how many bytes are finished; the rest wait for the bytes
 *         after them, and at the end of the stream go out as they are.  Start
 *         it zeroed.
 * SzwDictFor  the LZMA dictionary for 'bytes' of input, within what this
 *         machine can spare for the encoder. */
typedef struct {
    uint8_t *p;
    uint32_t len, cap;
    int      failed;
} HBuf;

void   HPut( HBuf *h, const void *data, uint32_t n );
void   HByte( HBuf *h, uint8_t b );
void   HUInt32( HBuf *h, uint32_t v );
void   HNum( HBuf *h, uint32_t v );
void   HBits( HBuf *h, const uint8_t *bits, uint32_t n );

typedef struct {
    uint32_t base;
    uint32_t mask;
} BcjEnc;

uint32_t BcjEncChunk( BcjEnc *b, uint8_t *data, uint32_t size );
uint32_t SzwDictFor( uint32_t bytes );

/* Read buffer size for file data. */
#define COMP_IOBUF  32768U

#endif /* ARCCOMPI_H */
