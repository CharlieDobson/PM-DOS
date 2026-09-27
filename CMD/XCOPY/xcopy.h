/*
 * XCOPY.H - what the parts of XCOPY share.
 *
 *   MAIN.C   the command line, the source and the destination, the end
 *   COPY.C   the walk through the source tree and the copying itself
 *   PATH.C   names: full paths, wildcards, the destination template
 */
#ifndef XCOPY_H
#define XCOPY_H

#include "types.h"
#include "rt.h"
#include "sys.h"
#include "msg.h"

/* the errorlevels DOS's XCOPY documents */
#define EXIT_OK     0           /* files were copied                    */
#define EXIT_NONE   1           /* nothing was found to copy            */
#define EXIT_BREAK  2           /* ended by ^C                          */
#define EXIT_INIT   4           /* before any copying, or out of space  */
#define EXIT_WRITE  5           /* a file could not be read or written  */

#define DATE_NONE   0
#define DATE_SINCE  1           /* /D:date - on or after                 */
#define DATE_NEWER  2           /* /D - newer than the copy already there */

typedef struct {
    int archive_only;           /* /A */
    int archive_reset;          /* /M */
    int date_mode;              /* /D, /D:date */
    u16 date;
    int prompt;                 /* /P */
    int subdirs;                /* /S */
    int empty;                  /* /E */
    int verify;                 /* /V */
    int wait;                   /* /W */
    int cont;                   /* /C */
    int assume_dir;             /* /I */
    int quiet;                  /* /Q */
    int full;                   /* /F */
    int list;                   /* /L */
    int hidden;                 /* /H */
    int readonly;               /* /R */
    int tree;                   /* /T */
    int update;                 /* /U */
    int keep_attr;              /* /K */
    int short_names;            /* /N */
    int overwrite;              /* /Y (1) or /-Y (0) */
} OPTIONS;

typedef struct {
    char src_full[PATH_MAX];    /* the source directory, full, ending in '\' */
    char src_disp[PATH_MAX];    /* the same as it was typed, for the listing */
    char pattern[PATH_MAX];     /* which files in it */
    char dst_full[PATH_MAX];    /* the destination directory, full, '\' */
    char tmpl[PATH_MAX];        /* a name or wildcard template; "" = keep */
    int  same_dir;              /* source and destination directory are one */
} JOB;

extern OPTIONS opt;
extern JOB     job;

/* MAIN.C */
void xcopy_main( void );
void out_of_memory( void );
void quit( int code );
int  ask( const char *tmpl, const char *arg, const char *keys );

/* COPY.C */
int  copy_run( void );

/* PATH.C */
int   is_sep( int ch );
int   has_wild( const char *name );
const char *name_part( const char *path );
void  end_slash( char *path );
int   make_full( const char *in, char *out );
void  map_name( const char *src, const char *tmpl, char *out );

#endif
