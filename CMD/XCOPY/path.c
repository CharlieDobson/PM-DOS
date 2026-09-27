/*
 * PATH.C - names.
 *
 * make_full() is how XCOPY knows where a path really is.  Two of its
 * decisions depend on that - "is the destination inside the source?"
 * (a cyclic copy) and "is it the same directory?" (a file copied onto
 * itself) - and neither can be answered from the text as typed, which
 * may be relative, may run through "..", and may name a directory by
 * its short name on one side and its long name on the other.  So the
 * path is made absolute, "." and ".." are taken out, and the longest
 * part of it that exists is given to 7160h for its long form; whatever
 * does not exist yet (a destination about to be made) is added back
 * as typed.
 */
#include "xcopy.h"

int is_sep( int ch )
{
    return ch == '\\' || ch == '/';
}

int has_wild( const char *name )
{
    for ( ; *name; name++ ) {
        if ( *name == '*' || *name == '?' ) {
            return 1;
        }
    }
    return 0;
}

/* the last element: after the last separator, or after "X:" */
const char *name_part( const char *path )
{
    const char *last = path;

    if ( path[0] && path[1] == ':' ) {
        last = path + 2;
    }
    for ( ; *path; path++ ) {
        if ( is_sep( *path ) ) {
            last = path + 1;
        }
    }
    return last;
}

/* a directory's full path as a prefix: "C:\" stays, "C:\A" gets one */
void end_slash( char *path )
{
    u32 len = str_len( path );

    if ( len && path[len - 1] != '\\' && len + 1 < PATH_MAX ) {
        path[len] = '\\';
        path[len + 1] = 0;
    }
}

#define MAX_COMPS   64

int make_full( const char *in, char *out )
{
    char abs[PATH_MAX * 2];
    char canon[PATH_MAX];
    char *comp[MAX_COMPS];
    u32 len;
    int drv, count = 0, keep, rc, i;
    const char *rest;
    char *cur;

    if ( in[0] && in[1] == ':' ) {
        drv = ch_upper( in[0] ) - 'A';
        rest = in + 2;
    } else {
        drv = sys_get_drive();
        rest = in;
    }
    if ( drv < 0 || drv > 25 || !sys_drive_valid( drv ) ) {
        return ERR_BADDRIVE;
    }

    /* absolute, as text */
    abs[0] = (char)('A' + drv);
    abs[1] = ':';
    abs[2] = '\\';
    abs[3] = 0;
    if ( !is_sep( rest[0] ) ) {
        rc = sys_get_cwd( drv, abs + 3 );
        if ( rc != 0 ) {
            return rc;
        }
        if ( abs[3] ) {
            str_catn( abs, "\\", sizeof( abs ) );
        }
    }
    if ( !str_catn( abs, rest, sizeof( abs ) ) ) {
        return ERR_NOPATH;
    }

    /* the components, with "." and ".." taken out */
    cur = abs + 3;
    while ( *cur ) {
        while ( is_sep( *cur ) ) {
            *cur++ = 0;
        }
        if ( *cur == 0 ) {
            break;
        }
        if ( count == MAX_COMPS ) {
            return ERR_NOPATH;
        }
        comp[count] = cur;
        while ( *cur && !is_sep( *cur ) ) {
            cur++;
        }
        if ( *cur ) {
            *cur++ = 0;
        }
        if ( str_icmp( comp[count], "." ) == 0 ) {
            continue;
        }
        if ( str_icmp( comp[count], ".." ) == 0 ) {
            if ( count > 0 ) {
                count--;
            }
            continue;
        }
        count++;
    }

    /* the longest prefix that exists, in its long form */
    for ( keep = count; keep >= 0; keep-- ) {
        out[0] = (char)('A' + drv);
        out[1] = ':';
        out[2] = '\\';
        out[3] = 0;
        for ( i = 0; i < keep; i++ ) {
            if ( i ) {
                str_catn( out, "\\", PATH_MAX );
            }
            if ( !str_catn( out, comp[i], PATH_MAX ) ) {
                return ERR_NOPATH;
            }
        }
        rc = sys_truename( out, canon, 2 );
        if ( rc == 0 && canon[0] && canon[1] == ':' ) {
            break;
        }
        if ( keep == 0 ) {
            str_cpy( canon, out );          /* the root: take it as it is */
            break;
        }
    }
    if ( !str_cpyn( out, canon, PATH_MAX ) ) {
        return ERR_NOPATH;
    }
    for ( i = keep; i < count; i++ ) {
        len = str_len( out );
        if ( len && out[len - 1] != '\\' ) {
            if ( !str_catn( out, "\\", PATH_MAX ) ) {
                return ERR_NOPATH;
            }
        }
        if ( !str_catn( out, comp[i], PATH_MAX ) ) {
            return ERR_NOPATH;
        }
    }
    return 0;
}

/*
 * map_name - a source name through a destination template, the way
 * COPY and REN rename: "*.BAK" makes READ.ME into READ.BAK.  The name
 * and its extension (after the LAST dot) are mapped separately, each by
 * its half of the template: '?' takes the character in that place, '*'
 * the rest of that half, anything else is itself.  A template with no
 * dot maps against the whole name, so "*" is every name unchanged.  A
 * template with no wildcards is simply the new name.
 */
static void map_half( const char *src, u32 slen, const char *tmpl, u32 tlen,
                      char *out, u32 *pos )
{
    u32 tpos, spos = 0;

    for ( tpos = 0; tpos < tlen; tpos++ ) {
        if ( tmpl[tpos] == '*' ) {
            while ( spos < slen && *pos < PATH_MAX - 1 ) {
                out[(*pos)++] = src[spos++];
            }
            return;
        }
        if ( *pos >= PATH_MAX - 1 ) {
            return;
        }
        if ( tmpl[tpos] == '?' ) {
            if ( spos < slen ) {
                out[(*pos)++] = src[spos];
            }
        } else {
            out[(*pos)++] = tmpl[tpos];
        }
        spos++;
    }
}

void map_name( const char *src, const char *tmpl, char *out )
{
    const char *sdot = str_rchr( src, '.' );
    const char *tdot = str_rchr( tmpl, '.' );
    u32 slen = str_len( src ), tlen = str_len( tmpl );
    u32 sbase, pos = 0;

    if ( !has_wild( tmpl ) ) {
        str_cpyn( out, tmpl, PATH_MAX );
        return;
    }
    if ( tdot == NULL ) {
        map_half( src, slen, tmpl, tlen, out, &pos );
        out[pos] = 0;
        return;
    }
    sbase = sdot ? (u32)(sdot - src) : slen;
    map_half( src, sbase, tmpl, (u32)(tdot - tmpl), out, &pos );
    if ( pos < PATH_MAX - 1 ) {
        out[pos++] = '.';
    }
    if ( sdot ) {
        map_half( sdot + 1, slen - sbase - 1, tdot + 1,
                  tlen - (u32)(tdot - tmpl) - 1, out, &pos );
    } else {
        map_half( "", 0, tdot + 1, tlen - (u32)(tdot - tmpl) - 1, out, &pos );
    }
    while ( pos && out[pos - 1] == '.' ) {  /* "README" through "*.*" */
        pos--;
    }
    out[pos] = 0;
}
