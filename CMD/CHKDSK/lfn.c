/*
 * LFN.C - VFAT long filename entries and name matching.
 *
 * A long name is stored as a run of attribute-0Fh entries directly in
 * front of its short (8.3) entry, last part first.  The first entry of a
 * run carries 40h plus the entry count, the others count down to 1, and
 * every entry holds the checksum of the 8.3 name it belongs to.
 */
#include "chkdsk.h"

/* byte offsets of the 13 UCS-2 characters inside an LFN entry */
static const u8 lfn_off[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

#define LFN_ENTRIES_MAX 20      /* 20 * 13 = 260 >= 255 characters */

u8 lfn_checksum( const u8 *sfn )
{
    u8 sum = 0;
    int i;

    for ( i = 0; i < 11; i++ ) {
        sum = (u8)(((sum & 1) << 7) + (sum >> 1) + sfn[i]);
    }
    return sum;
}

void lfn_begin( LFNRUN *run )
{
    run->active = 0;
    run->ok = 0;
    run->len = 0;
    run->expect = 0;
}

static void lfn_store( LFNRUN *run, const u8 *ent, int seq )
{
    int i;
    u16 *dst = run->name + (seq - 1) * 13;

    for ( i = 0; i < 13; i++ ) {
        dst[i] = RD16( ent + lfn_off[i] );
    }
}

/*
 * Feed one LFN entry.  Returns 0 when this entry starts a new run while
 * an earlier run was still open (the caller reports the earlier run
 * first, then calls again), 1 otherwise.
 */
int lfn_add( LFNRUN *run, const u8 *ent, u32 idx )
{
    int seq = ent[0] & 0x1F;

    if ( ent[0] & 0x40 ) {
        if ( run->active ) {
            return 0;
        }
        run->active = 1;
        run->start = idx;
        run->sum = ent[13];
        run->ok = (seq >= 1 && seq <= LFN_ENTRIES_MAX);
        if ( run->ok ) {
            run->len = seq * 13;
            lfn_store( run, ent, seq );
            run->expect = seq - 1;
        }
        return 1;
    }
    if ( !run->active ) {               /* orphan: start a broken run */
        run->active = 1;
        run->start = idx;
        run->ok = 0;
        return 1;
    }
    if ( run->ok && seq == run->expect && seq >= 1 && ent[13] == run->sum ) {
        lfn_store( run, ent, seq );
        run->expect--;
    } else {
        run->ok = 0;
    }
    return 1;
}

/* the open run is a complete, valid long name for this 8.3 entry */
int lfn_match( LFNRUN *run, const u8 *sfn )
{
    int i;

    if ( !run->active || !run->ok || run->expect != 0 ||
         run->sum != lfn_checksum( sfn ) ) {
        return 0;
    }
    for ( i = 0; i < run->len; i++ ) {
        if ( run->name[i] == 0 ) {
            break;
        }
    }
    run->len = i > LFN_MAX ? LFN_MAX : i;
    return run->len > 0;
}

/* Unicode U+00A0..U+00FF to code page 437; 0 = no equivalent */
static const u8 latin1_437[96] = {
    0xFF, 0xAD, 0x9B, 0x9C, 0x00, 0x9D, 0x00, 0x00,   /* A0 */
    0x00, 0x00, 0xA6, 0xAE, 0xAA, 0x00, 0x00, 0x00,   /* A8 */
    0xF8, 0xF1, 0xFD, 0x00, 0x00, 0xE6, 0x00, 0xFA,   /* B0 */
    0x00, 0x00, 0xA7, 0xAF, 0xAC, 0xAB, 0x00, 0xA8,   /* B8 */
    0x00, 0x00, 0x00, 0x00, 0x8E, 0x8F, 0x92, 0x80,   /* C0 */
    0x00, 0x90, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* C8 */
    0x00, 0xA5, 0x00, 0x00, 0x00, 0x00, 0x99, 0x00,   /* D0 */
    0x00, 0x00, 0x00, 0x00, 0x9A, 0x00, 0x00, 0xE1,   /* D8 */
    0x85, 0xA0, 0x83, 0x00, 0x84, 0x86, 0x91, 0x87,   /* E0 */
    0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,   /* E8 */
    0x00, 0xA4, 0x95, 0xA2, 0x93, 0x00, 0x94, 0xF6,   /* F0 */
    0x00, 0x97, 0xA3, 0x96, 0x81, 0x00, 0x00, 0x98    /* F8 */
};

void lfn_oem( const LFNRUN *run, char *out )
{
    int i;
    u16 ch;

    for ( i = 0; i < run->len; i++ ) {
        ch = run->name[i];
        if ( ch >= 0x20 && ch < 0x80 ) {
            out[i] = (char)ch;
        } else if ( ch >= 0xA0 && ch <= 0xFF && latin1_437[ch - 0xA0] ) {
            out[i] = (char)latin1_437[ch - 0xA0];
        } else {
            out[i] = '_';
        }
    }
    out[i] = 0;
}

/* 8.3 entry name as DOS displays it: "NAME.EXT", trailing blanks gone */
void sfn_name( const u8 *ent, char *out )
{
    int i;
    char *dst = out;

    for ( i = 0; i < 8; i++ ) {
        *dst++ = (char)ent[i];
    }
    if ( out[0] == 0x05 ) {
        out[0] = (char)DEL_MARK;
    }
    while ( dst > out && dst[-1] == ' ' ) {
        dst--;
    }
    if ( ent[8] != ' ' || ent[9] != ' ' || ent[10] != ' ' ) {
        *dst++ = '.';
        for ( i = 8; i < 11; i++ ) {
            *dst++ = (char)ent[i];
        }
        while ( dst[-1] == ' ' ) {
            dst--;
        }
    }
    *dst = 0;
}

int sfn_is_dot( const u8 *ent )
{
    static const char dot[] = ".          ";
    static const char dotdot[] = "..         ";

    if ( mem_cmp( ent, dot, 11 ) == 0 ) {
        return 1;
    }
    if ( mem_cmp( ent, dotdot, 11 ) == 0 ) {
        return 2;
    }
    return 0;
}

/* long filename wildcard match ("*" any run, "?" any one character) */
int wild_match( const char *pat, const char *name )
{
    if ( str_icmp( pat, "*.*" ) == 0 ) {
        return 1;
    }
    for ( ;; ) {
        if ( *pat == 0 ) {
            return *name == 0;
        }
        if ( *pat == '*' ) {
            while ( *pat == '*' ) {
                pat++;
            }
            if ( *pat == 0 ) {
                return 1;
            }
            for ( ; *name; name++ ) {
                if ( wild_match( pat, name ) ) {
                    return 1;
                }
            }
            return 0;
        }
        if ( *name == 0 ) {
            return 0;
        }
        if ( *pat != '?' && ch_upper( *pat ) != ch_upper( *name ) ) {
            return 0;
        }
        pat++;
        name++;
    }
}

/*
 * 8.3 match with FCB semantics (DOS function 29h parsing): "*" fills the
 * rest of its field with "?", a missing extension means a blank one.
 * Patterns that are not valid 8.3 names never match here.
 */
int fcb_match( const char *pat, const u8 *sfn )
{
    char fcb[11];
    int pos = 0, field = 0, max = 8, star = 0;
    const char *cur = pat;
    u8 ch;

    mem_set( fcb, ' ', 11 );
    for ( ; *cur; cur++ ) {
        ch = (u8)*cur;
        if ( ch == '.' ) {
            if ( field ) {
                return 0;
            }
            field = 1;
            pos = 8;
            max = 11;
            star = 0;
            continue;
        }
        if ( ch <= ' ' || ch == '"' || ch == '/' || ch == '\\' || ch == '[' ||
             ch == ']' || ch == ':' || ch == '|' || ch == '<' || ch == '>' ||
             ch == '+' || ch == '=' || ch == ';' || ch == ',' ) {
            return 0;
        }
        if ( star ) {                     /* text after "*" is ignored */
            continue;
        }
        if ( ch == '*' ) {
            while ( pos < max ) {
                fcb[pos++] = '?';
            }
            star = 1;
            continue;
        }
        if ( pos >= max ) {
            return 0;
        }
        fcb[pos++] = (char)ch_upper( ch );
    }
    for ( pos = 0; pos < 11; pos++ ) {
        if ( fcb[pos] != '?' &&
             (u8)fcb[pos] != (pos == 0 && sfn[0] == 0x05 ?
                              DEL_MARK : sfn[pos]) ) {
            return 0;
        }
    }
    return 1;
}
