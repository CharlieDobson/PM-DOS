/*
 * CFB.H - the OLE2 compound file: a file that holds a small tree of
 * named streams, which is what a Word 6.0 to 2003 .DOC is, and what a
 * .docx with a password on it is wrapped in.
 *
 * READ.  The whole file is in memory (FMT.H); a stream is found by its
 * name and handed over as a heap block.  Nothing in the file is
 * trusted: a damaged one gives a NULL, never a read outside the bytes.
 *
 * WRITE.  A root with a few streams in it, each given as a block of
 * memory, written to an OUT in one go.
 */
#ifndef CFB_H
#define CFB_H

#include "fmt.h"

typedef struct CFB CFB;
typedef struct CFBW CFBW;

/* 1 if the bytes start with a compound file's signature */
int   cfb_is( const u8 *data, u32 len );

/* the file opened: NULL if it is not one, is damaged past use, or
   there is no memory.  "data" must stay until cfb_close. */
CFB  *cfb_open( const u8 *data, u32 len );
void  cfb_close( CFB *cfb );

/* a stream by its name, in ASCII, letters in either case: its number,
   or -1.  "Storage/Stream" finds a stream one storage down. */
int   cfb_find( CFB *cfb, const char *name );

/* how many bytes a stream says it has (0 for what is not a stream) */
u32   cfb_size( CFB *cfb, int entry );

/* a stream's bytes in a heap block the caller xfrees, and *len how
   many; NULL on damage or no memory */
u8   *cfb_read( CFB *cfb, int entry, u32 *len );

/* a file to be written: NULL if there is no memory */
CFBW *cfbw_new( void );
/* a stream for it.  The bytes are NOT copied: they must stay until
   cfbw_write is done.  0 if the name is too long (31 characters) or
   there are too many streams (16). */
int   cfbw_add( CFBW *w, const char *name, const u8 *data, u32 len );
/* the class the root says the file is of: 16 bytes */
void  cfbw_clsid( CFBW *w, const u8 *clsid );
/* the file: 1, or 0 if it is too big to be one or the OUT failed */
int   cfbw_write( CFBW *w, OUT *out );
void  cfbw_free( CFBW *w );

#endif
