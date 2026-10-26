/*
 * TREE.H - what the parts of TREE share.
 *
 *   MAIN.C   the command line, the heading and where the tree starts
 *   WALK.C   the walk through the directories, and the drawing
 */
#ifndef TREE_H
#define TREE_H

#include "types.h"
#include "rt.h"
#include "sys.h"
#include "msg.h"

/* the errorlevels 6.22's TREE leaves */
#define EXIT_OK     0           /* the tree was shown                   */
#define EXIT_ERROR  1           /* a bad command line, drive or path,
                                   or a directory that could not be read */
#define EXIT_BREAK  3           /* ended by ^C                          */

/* a directory more than this far down cannot have a path that fits:
   every level is a name and a backslash */
#define LEVEL_MAX   (PATH_MAX / 2)

typedef struct {
    int  files;                 /* /F */
    int  ascii;                 /* /A */
} OPTIONS;

extern OPTIONS opt;

/* MAIN.C */
void tree_main( void );
void quit( int code );

/* WALK.C - the tree below "dir" (a full path, no '\' on the end unless
   it is the root); EXIT_OK, or EXIT_ERROR if part of it could not be
   read or was too deep to name */
int  walk_tree( const char *dir );

#endif
