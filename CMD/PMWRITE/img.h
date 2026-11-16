/*
 * IMG.H - the pictures in a document: what kind a file is, how big,
 * and its pixels.
 *
 * A document keeps a picture as the file it came as (WDOC.H).  To show
 * it, or to put it in a PDF, it is decoded here into rows of pixels,
 * three bytes each - blue, green, red - the top row first.  What can
 * be decoded: PNG (every kind, interlaced too; a picture that is
 * partly clear is set on white), BMP and the headerless form of it
 * that Windows calls a DIB, and JPEG through the PDF code's decoder
 * (PDFVIEW.H), which does the baseline kind.  GIF, TIFF, WMF and the
 * rest are sized where that is easy, and shown as an empty frame.
 */
#ifndef IMG_H
#define IMG_H

#include "wdoc.h"

/* the kind a picture file is, from its first bytes: IMG_* (WDOC.H),
   IMG_UNKNOWN when it is none this knows */
int   img_kind( const u8 *data, u32 len );

/* its size in pixels, and how many of them it says go to an inch (0
   when it does not say).  1 if the size could be read. */
int   img_size( const u8 *data, u32 len, u32 *width, u32 *height, u32 *dpi_x, u32 *dpi_y );

/* its pixels: a heap block of width * height * 3 bytes that the caller
   xfrees, or NULL - a kind that is not decoded, a damaged file, a
   picture of more than IMG_MAX_SIDE pixels a side, or no memory */
#define IMG_MAX_SIDE    4096
u8   *img_decode( const u8 *data, u32 len, u32 *width, u32 *height );

/* the size a picture is shown at when its file is all there is to go
   by: its pixels at the resolution it names, or at 96 to the inch,
   and never wider than six inches.  In twips. */
void  img_natural( const u8 *data, u32 len, s32 *width, s32 *height );

#endif
