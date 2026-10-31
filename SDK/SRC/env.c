/*
 * ENV.C - getenv() and putenv()
 *
 * Names are matched without regard to case, as COMMAND.COM and CMD.EXE
 * store them in capitals anyway.  putenv() changes the program's own
 * list (environ); a program it starts later is given that list.
 */
#include "pmlib.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* the length of the name in entry: up to '=' */
static size_t name_len( const char *entry )
{
    const char *eq = strchr( entry, '=' );

    return eq ? (size_t)(eq - entry) : strlen( entry );
}

static int find( const char *name, size_t len )
{
    int i;

    if ( environ == NULL ) {
        return -1;
    }
    for ( i = 0; environ[i] != NULL; i++ ) {
        if ( strnicmp( environ[i], name, len ) == 0 && environ[i][len] == '=' ) {
            return i;
        }
    }
    return -1;
}

char *getenv( const char *name )
{
    size_t len = strlen( name );
    int i = find( name, len );

    return i < 0 ? NULL : environ[i] + len + 1;
}

/* "NAME=value" sets NAME, "NAME=" removes it.  The string itself goes
   into the list, so it must not change or go away afterwards. */
int putenv( const char *entry )
{
    static char **ours;                 /* a list malloc()ed here */
    size_t len = name_len( entry );
    int i = find( entry, len );
    int count = 0;
    char **list;

    if ( entry[len] != '=' || len == 0 ) {
        errno = EINVAL;
        return -1;
    }
    if ( i >= 0 && entry[len + 1] != '\0' ) {
        environ[i] = (char *)entry;
        return 0;
    }
    if ( i >= 0 ) {
        do {
            environ[i] = environ[i + 1];
        } while ( environ[i++] != NULL );
        return 0;
    }
    if ( entry[len + 1] == '\0' ) {
        return 0;                       /* removing what is not there */
    }
    while ( environ != NULL && environ[count] != NULL ) {
        count++;
    }
    list = (char **)malloc( (count + 2) * sizeof( char * ) );
    if ( list == NULL ) {
        errno = ENOMEM;
        return -1;
    }
    if ( count ) {
        memcpy( list, environ, count * sizeof( char * ) );
    }
    list[count] = (char *)entry;
    list[count + 1] = NULL;
    if ( ours != NULL ) {
        free( ours );
    }
    ours = list;
    environ = list;
    return 0;
}
