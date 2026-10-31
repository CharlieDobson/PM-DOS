/*
 * STREAM.H - what a FILE is, for the stdio modules
 *
 * Programs see FILE as an incomplete type (INCLUDE\STDIO.H), so this
 * layout can change without breaking a program built against an older
 * library.
 */
#ifndef STREAM_H
#define STREAM_H

#include <stdio.h>

#define SF_READ     0x0001              /* opened for reading */
#define SF_WRITE    0x0002              /* opened for writing */
#define SF_TEXT     0x0004              /* CR LF <-> \n, Ctrl+Z ends it */
#define SF_EOF      0x0008
#define SF_ERR      0x0010
#define SF_OWNBUF   0x0020              /* the buffer is malloc()ed here */
#define SF_LBF      0x0040              /* line-buffered */
#define SF_NBF      0x0080              /* unbuffered */
#define SF_READING  0x0100              /* the buffer holds input */
#define SF_WRITING  0x0200              /* the buffer holds output */
#define SF_SETUP    0x0400              /* buffering decided */
#define SF_INUSE    0x0800              /* the slot is a stream */
#define SF_DEVICE   0x1000              /* the handle is a device */
#define SF_SETBUF   0x2000              /* setvbuf() chose the buffering */

struct __pm_file {
    int             handle;
    int             flags;
    unsigned char  *buf;
    size_t          size;
    unsigned char  *ptr;                /* the next byte */
    size_t          cnt;                /* reading: bytes after ptr */
    size_t          wlen;               /* writing: bytes in buf */
    int             ungot;              /* ungetc()'s character, or EOF */
    char           *tmpname;            /* tmpfile(): removed at fclose() */
    unsigned char   one;                /* the buffer of an unbuffered stream */
};

/* buffering decided, the exit flush hooked: before any I/O */
void __pm_stream_setup( FILE *fp );

/* write n bytes, translating \n in a text stream; 0 or EOF */
int __pm_stream_write( FILE *fp, const char *s, size_t n );

/* empty the output buffer; 0 or EOF */
int __pm_stream_flush( FILE *fp );

/* every stream, for fopen() to find a free one */
extern struct __pm_file __pm_files[FOPEN_MAX];

#endif
