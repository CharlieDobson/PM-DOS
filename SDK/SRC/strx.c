/*
 * STRX.C - strerror() and the DOS compilers' string extras
 */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <errno.h>

static const struct {
    int         num;
    const char *text;
} messages[] = {
    { 0,            "No error" },
    { EPERM,        "Not allowed" },
    { ENOENT,       "No such file or directory" },
    { EINTR,        "Interrupted" },
    { EIO,          "Input/output error" },
    { E2BIG,        "Argument list too long" },
    { ENOEXEC,      "Not a program" },
    { EBADF,        "Bad file handle" },
    { ENOMEM,       "Not enough memory" },
    { EACCES,       "Access denied" },
    { EBUSY,        "In use" },
    { EEXIST,       "File exists" },
    { EXDEV,        "Not the same drive" },
    { ENODEV,       "No such drive" },
    { ENOTDIR,      "Not a directory" },
    { EISDIR,       "Is a directory" },
    { EINVAL,       "Invalid argument" },
    { EMFILE,       "Too many open files" },
    { ENOTTY,       "Not a device" },
    { EFBIG,        "File too large" },
    { ENOSPC,       "Disk full" },
    { ESPIPE,       "Cannot seek on a device" },
    { EROFS,        "Write-protected" },
    { EPIPE,        "Broken pipe" },
    { EDOM,         "Argument out of range" },
    { ERANGE,       "Result out of range" },
    { ENAMETOOLONG, "Name too long" },
    { ENOSYS,       "Not supported" },
    { ENOTEMPTY,    "Directory not empty" },
    { EILSEQ,       "Invalid character" }
};

char *strerror( int errnum )
{
    size_t i;

    for ( i = 0; i < sizeof( messages ) / sizeof( messages[0] ); i++ ) {
        if ( messages[i].num == errnum ) {
            return (char *)messages[i].text;
        }
    }
    return (char *)"Unknown error";
}

char *strdup( const char *s )
{
    size_t len = strlen( s ) + 1;
    char *copy = (char *)malloc( len );

    if ( copy != NULL ) {
        memcpy( copy, s, len );
    }
    return copy;
}

int stricmp( const char *a, const char *b )
{
    int ca, cb;

    do {
        ca = tolower( (unsigned char)*a++ );
        cb = tolower( (unsigned char)*b++ );
    } while ( ca == cb && ca != '\0' );
    return ca - cb;
}

int strnicmp( const char *a, const char *b, size_t n )
{
    int ca, cb;

    for ( ; n; n-- ) {
        ca = tolower( (unsigned char)*a++ );
        cb = tolower( (unsigned char)*b++ );
        if ( ca != cb || ca == '\0' ) {
            return ca - cb;
        }
    }
    return 0;
}

char *strlwr( char *s )
{
    char *p;

    for ( p = s; *p; p++ ) {
        *p = (char)tolower( (unsigned char)*p );
    }
    return s;
}

char *strupr( char *s )
{
    char *p;

    for ( p = s; *p; p++ ) {
        *p = (char)toupper( (unsigned char)*p );
    }
    return s;
}

char *strrev( char *s )
{
    char *head = s;
    char *tail = s + strlen( s );
    char c;

    while ( tail - head > 1 ) {
        c = *head;
        *head++ = *--tail;
        *tail = c;
    }
    return s;
}
