/*
 * LOCALE.C - setlocale() and localeconv(), for the one locale there is
 */
#include <locale.h>
#include <limits.h>
#include <string.h>

char *setlocale( int category, const char *locale )
{
    if ( category < LC_ALL || category > LC_TIME ) {
        return NULL;
    }
    if ( locale == NULL || *locale == '\0' || strcmp( locale, "C" ) == 0
         || strcmp( locale, "POSIX" ) == 0 ) {
        return (char *)"C";
    }
    return NULL;
}

struct lconv *localeconv( void )
{
    static struct lconv c = {
        ".", "", "", "", "", "", "", "", "",
        CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX,
        "", CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX, CHAR_MAX
    };

    return &c;
}
