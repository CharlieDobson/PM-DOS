/*
 * DECTAB.C - every codec MPLAY has, and the choice of one for a file
 * (DEC.H).
 *
 * A CODEC IS IN THE TABLE WHEN ITS HAVE_ IS DEFINED, so a test program
 * built for one format (MKHOST.CMD with MPHAVE set) needs only that
 * format's sources.  The shipped program defines all of them but
 * HAVE_WMA.
 *
 * THE FILE'S BYTES DECIDE, NOT ITS NAME.  Every codec is asked how
 * sure it is; the surest is opened first, and if it turns the file
 * down after all the next surest is tried.  The extension is only a
 * tie-breaker each codec may use: an MP3 renamed .WAV still plays.
 *
 * WITHOUT HAVE_WMA a Windows Media file is still known, by the GUID its
 * header object starts with, and is turned down by name ("Windows
 * Media Audio is not supported") rather than as an unknown or damaged
 * file.  The WMA decoder needs WMATAB.C, which is not shipped (NOTES.TXT
 * says why).
 */
#include "rt.h"
#include "sys.h"
#include "dec.h"

extern const CODEC codec_wav;
#ifdef HAVE_VOC
extern const CODEC codec_voc;
#endif
#ifdef HAVE_MP3
extern const CODEC codec_mp3;
#endif
#ifdef HAVE_OGG
extern const CODEC codec_ogg;
#endif
#ifdef HAVE_WMA
extern const CODEC codec_wma;
#else
static const u8 asf_guid[16] = {
    0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C
};

static int nowma_detect( SRC *src, const u8 *head, u32 len, const char *ext )
{
    (void)src;
    (void)ext;
    if ( len >= 16 && mem_cmp( head, asf_guid, 16 ) == 0 ) {
        return 100;
    }
    return 0;
}

static int nowma_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    (void)src;
    (void)opt;
    *state = NULL;
    str_cpyt( info->format, "Windows Media Audio is not supported", sizeof( info->format ) );
    return DE_UNSUPPORTED;
}

/* never reached: open never succeeds */
static int nowma_decode( void *state, DECBUF *buf )
{
    (void)state;
    buf->frames = 0;
    buf->ev_count = 0;
    return DE_FORMAT;
}

static int nowma_seek( void *state, u32 ms, u32 *at_ms )
{
    (void)state;
    (void)ms;
    *at_ms = 0;
    return DE_FORMAT;
}

static void nowma_close( void *state )
{
    (void)state;
}

static const CODEC codec_nowma = {
    "Windows Media Audio",
    "",
    nowma_detect,
    nowma_open,
    nowma_decode,
    nowma_seek,
    nowma_close
};
#endif
#ifdef HAVE_MIDI
extern const CODEC codec_midi;
#endif

const CODEC *const codecs[] = {
    &codec_wav,
#ifdef HAVE_VOC
    &codec_voc,
#endif
#ifdef HAVE_MP3
    &codec_mp3,
#endif
#ifdef HAVE_OGG
    &codec_ogg,
#endif
#ifdef HAVE_WMA
    &codec_wma,
#else
    &codec_nowma,
#endif
#ifdef HAVE_MIDI
    &codec_midi,
#endif
    NULL
};

const int codec_count = sizeof( codecs ) / sizeof( codecs[0] ) - 1;

/* the extension of a path, in capitals, without the dot */
static void path_ext( const char *path, char *ext )
{
    const char *dot = NULL, *at;
    int len = 0;

    for ( at = path; *at; at++ ) {
        if ( *at == '.' ) {
            dot = at;
        } else if ( *at == '\\' || *at == '/' || *at == ':' ) {
            dot = NULL;
        }
    }
    if ( dot != NULL ) {
        for ( at = dot + 1; *at && len < 7; at++ ) {
            ext[len++] = (char)ch_upper( *at );
        }
    }
    ext[len] = 0;
}

int dec_open( SRC *src, const char *path, const DECOPT *opt,
              const CODEC **codec, void **state, DECINFO *info )
{
    int score[16], tried[16];
    char ext[8], unsup[sizeof( info->format )];
    u8 *head;
    const u8 *peek;
    u32 len;
    int i, best, rc, result = DE_FORMAT, count = codec_count;

    *codec = NULL;
    *state = NULL;
    if ( count > 16 ) {
        count = 16;
    }
    rc = src_open( src, path );
    if ( rc != 0 ) {
        return rc == ERR_NOMEM ? DE_NOMEM : DE_READ;
    }
    head = (u8 *)try_alloc( SRC_PEEK );
    if ( head == NULL ) {
        src_close( src );
        return DE_NOMEM;
    }
    len = src->size < SRC_PEEK ? src->size : SRC_PEEK;
    peek = src_peek( src, len );
    if ( peek == NULL ) {
        xfree( head );
        src_close( src );
        return src->error ? DE_READ : DE_FORMAT;
    }
    mem_cpy( head, peek, len );
    path_ext( path, ext );
    for ( i = 0; i < count; i++ ) {
        score[i] = codecs[i]->detect( src, head, len, ext );
        tried[i] = 0;
        src_seek( src, 0 );
    }
    xfree( head );
    for ( ;; ) {
        best = -1;
        for ( i = 0; i < count; i++ ) {
            if ( !tried[i] && score[i] > 0 && (best < 0 || score[i] > score[best]) ) {
                best = i;
            }
        }
        if ( best < 0 ) {
            break;
        }
        tried[best] = 1;
        mem_set( info, 0, sizeof( DECINFO ) );
        src_seek( src, 0 );
        rc = codecs[best]->open( src, opt, info, state );
        if ( rc == DE_OK ) {
            *codec = codecs[best];
            return DE_OK;
        }
        *state = NULL;
        /* a file the codec knows but cannot play says so; keep that
           answer over a later "not mine" */
        if ( rc != DE_FORMAT || result == DE_FORMAT ) {
            result = rc;
        }
        if ( rc == DE_UNSUPPORTED ) {
            str_cpyt( unsup, info->format, sizeof( unsup ) );
        }
        if ( rc == DE_NOMEM || rc == DE_READ ) {
            break;
        }
    }
    src_close( src );
    if ( result == DE_UNSUPPORTED ) {
        mem_set( info, 0, sizeof( DECINFO ) );
        str_cpyt( info->format, unsup, sizeof( info->format ) );
    }
    return result;
}
