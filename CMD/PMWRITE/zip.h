/*
 * ZIP.H - a ZIP archive read from memory and written to an OUT: the
 * package a .docx and an .odt are.
 *
 * READING.  The whole file is in memory (FMT.H), so an archive is
 * opened on those bytes and a member is handed back as a heap block,
 * inflated, with its CRC-32 checked.  What cannot be read is refused
 * member by member, not archive by archive: an encrypted member, a
 * zip64 one, or one packed by anything other than "stored" and
 * "deflated" gives NULL from zip_read and the rest are still there.
 *
 * WRITING.  Members are added one after another and the directory is
 * written at the end.  Each has its sizes and CRC in its own header -
 * no data descriptor, no extra field - which is the form the strictest
 * readers ask for and the form OpenDocument demands of "mimetype".
 */
#ifndef ZIP_H
#define ZIP_H

#include "fmt.h"

/* why a call gave NULL or 0 */
#define ZIP_OK          0
#define ZIP_NOMEM       1       /* no memory */
#define ZIP_DAMAGED     2       /* not an archive, or one that does not add up */
#define ZIP_UNSUPPORTED 3       /* encrypted, zip64, or a method that is not read */

/* the most a member may say it unpacks to */
#define ZIP_MAX_MEMBER  0x10000000UL

typedef struct ZIP ZIP;

/* an archive in "len" bytes at "data", which must stay where they are
   until zip_close.  NULL if it is not one, or there is no memory: "why"
   (which may be NULL) is told which. */
ZIP  *zip_open_why( const u8 *data, u32 len, int *why );
#define zip_open( data, len )   zip_open_why( (data), (len), NULL )
void  zip_close( ZIP *zip );

u32   zip_count( const ZIP *zip );
/* a member's name, with '/' between directories; "" past the end */
const char *zip_name( const ZIP *zip, u32 index );
/* the member called exactly "name", or the same in any case of
   letters: its index, or -1 */
int   zip_find( const ZIP *zip, const char *name );
int   zip_ifind( const ZIP *zip, const char *name );
/* how many bytes a member unpacks to */
u32   zip_size( const ZIP *zip, u32 index );
/* a member's bytes: a heap block for the caller to xfree (one byte
   longer than *len, and that byte is 0).  NULL, and zip_error says why. */
u8   *zip_read( ZIP *zip, u32 index, u32 *len );
int   zip_error( const ZIP *zip );      /* ZIP_*: the last zip_read's */
/* the start of a member, for telling what it is without unpacking a
   big one: as much of it as fits in "cap" bytes at "buf", and zeroes
   after.  No CRC is checked.  1 if there was anything to give. */
int   zip_peek( ZIP *zip, u32 index, u8 *buf, u32 cap );

typedef struct ZIPW ZIPW;

/* an archive begun where "out" is now.  NULL if there is no memory. */
ZIPW *zipw_new( OUT *out );
/*
 * A member, whole.  It is deflated unless "store" is not 0 or deflating
 * does not make it smaller.  0 when it could not be added (no memory,
 * or more members than an archive holds): the OUT is then failed too.
 */
int   zipw_add( ZIPW *zipw, const char *name, const u8 *data, u32 len, int store );
/*
 * A member a piece at a time, deflated: begin, any number of writes,
 * finish.  Each piece is packed by itself, so pieces of 32K and more
 * pack best.  The sizes are filled in by going back (out_seek).
 */
int   zipw_begin( ZIPW *zipw, const char *name );
int   zipw_write( ZIPW *zipw, const u8 *data, u32 len );
int   zipw_finish( ZIPW *zipw );
/* the directory, and the ZIPW is freed.  1 if all of it was written. */
int   zipw_end( ZIPW *zipw );

#endif
