/*
 * FLATE.H - DEFLATE (RFC 1951) both ways, memory to memory, with and
 * without the zlib wrapper (RFC 1950).
 *
 * WHO USES IT.  ZIP.C, for the parts of a .docx or .odt; the PDF
 * writer, whose FlateDecode streams are zlib streams; a PNG reader,
 * whose IDAT chunks are one too.
 *
 * Everything works on whole blocks in memory, because that is how the
 * formats hand their files over (FMT.H).  Nothing here ends the
 * program: no memory, or a stream that is damaged, is a 0 or a NULL.
 */
#ifndef FLATE_H
#define FLATE_H

#include "types.h"

/*
 * A raw DEFLATE stream decoded into "dst_cap" bytes at "dst".  1 when
 * the whole stream was decoded and it fitted; *dst_len is then how
 * many bytes it made.  0 for a damaged stream, one that is cut short,
 * or one that makes more than "dst_cap" bytes.  Bytes after the end of
 * the stream are not looked at.
 */
int   inflate_mem( const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len );

/*
 * The same when nobody knows how big the result is: a heap block that
 * grows as it fills, never past "max_len".  NULL for damage, for more
 * than "max_len" bytes, or for no memory.  The caller xfrees it.
 */
uint8_t *inflate_alloc( const uint8_t *src, uint32_t src_len, uint32_t *out_len, uint32_t max_len );

/* ...and both for a zlib stream: the two bytes in front are checked,
   and the Adler-32 behind */
int      zlib_inflate_mem( const uint8_t *src, uint32_t src_len, uint8_t *dst, uint32_t dst_cap, uint32_t *dst_len );
uint8_t *zlib_inflate_alloc( const uint8_t *src, uint32_t src_len, uint32_t *out_len, uint32_t max_len );

/*
 * "len" bytes compressed into a heap block, which the caller xfrees:
 * a raw DEFLATE stream, or with "zlib" not 0 a zlib stream (header
 * and Adler-32).  NULL when there is no memory - what a caller that
 * can store the bytes as they are then does.  The result can be a few
 * bytes longer than what went in, when that cannot be made smaller.
 */
uint8_t *deflate_alloc( const uint8_t *src, uint32_t len, uint32_t *out_len, int zlib );

/*
 * A raw stream made a piece at a time, for a writer that has not got
 * the whole of it in memory: each call packs one piece, and the blocks
 * written one after another are one DEFLATE stream.  "last" is not 0
 * for the piece that ends it (which may be of no bytes).  A match never
 * reaches back into an earlier piece, so small pieces pack worse.
 */
uint8_t *deflate_piece( const uint8_t *src, uint32_t len, uint32_t *out_len, int last );

#endif
