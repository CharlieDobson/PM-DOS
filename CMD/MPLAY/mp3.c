/*
 * MP3.C - MPEG audio files: finding the frames, the tags around them,
 * the VBR headers, seeking, and the codec table (DEC.H, MP3.H).
 *
 * AN MPEG AUDIO FILE IS FRAMES, EACH WITH A 4-BYTE HEADER that begins
 * with eleven 1 bits.  There is no file header: what is before the
 * first frame (an ID3v2 tag, or anything at all) and after the last
 * (ID3v1, APE, Lyrics3) is not sound.  Eleven 1 bits turn up in any
 * data, so a header is believed only when the frame it describes is
 * followed by another of the same version, layer and rate - the test
 * every decoder uses.  After that each frame is taken from where the
 * last one ended, and only a header that does not fit sends the
 * decoder looking again.
 *
 * FREE FORMAT FRAMES DO NOT SAY HOW LONG THEY ARE.  The first two
 * headers in a row give the length; every frame after that is the
 * same, plus its padding byte.
 *
 * THE LENGTH OF A VBR FILE is in its first frame, which is not sound:
 * the Xing (or LAME's "Info") header gives the frames, the bytes and a
 * table of where each hundredth of the time starts; Fraunhofer's VBRI
 * gives the same with a table of its own.  A file without one is taken
 * to be the bit rate of its first frame throughout.  The encoder delay
 * and padding LAME records are not trimmed: the sound is given out
 * exactly as the frames decode.
 *
 * A SEEK LANDS EARLY AND DECODES ITS WAY IN.  Layer 3 needs up to 511
 * bytes of earlier frames and the overlap of the granule before, and
 * every layer needs 16 slots of synthesis history, so the decoder
 * starts a few frames before the place asked for and throws those
 * frames' output away.
 *
 * THE EQUALIZER IS DONE HERE FOR EVERY LAYER: in layer 3 on the 576
 * spectral lines, in layers 1 and 2 as one gain for each subband,
 * folded into the scale factors so it costs nothing a sample.
 */
#include "rt.h"
#include "mp3.h"

#define FIRST_SCAN      (4UL << 20)     /* the first frame is looked for this far in */
#define SCAN_WINDOW     4096UL
#define PRIME_MAX       10              /* frames decoded and dropped after a seek */
#define MIN_FRAME       21              /* a header and the shortest side information */
#define TEXT_MAX        (4 * TAG_LEN + 8)
#define MAX_FRAMES      16000000UL      /* more than any file can hold: a damaged VBR header */
#define FREE_TRIES      32              /* free format frames measured while looking for the first */

static const uint16_t rate_tab[3][3] = {
    { 44100, 48000, 32000 },
    { 22050, 24000, 16000 },
    { 11025, 12000,  8000 }
};

static const uint16_t kbps_tab[2][3][15] = {
    {   /* MPEG-1 */
        { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448 },
        { 0, 32, 48, 56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320, 384 },
        { 0, 32, 40, 48,  56,  64,  80,  96, 112, 128, 160, 192, 224, 256, 320 }
    }, {    /* MPEG-2 and 2.5 */
        { 0, 32, 48, 56,  64,  80,  96, 112, 128, 144, 160, 176, 192, 224, 256 },
        { 0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 },
        { 0,  8, 16, 24,  32,  40,  48,  56,  64,  80,  96, 112, 128, 144, 160 }
    }
};

static const char *const ver_name[3] = { "MPEG-1", "MPEG-2", "MPEG-2.5" };
static const char *const mode_name[4] = { "stereo", "joint stereo", "dual channel", "mono" };

typedef struct {
    SRC      *src;
    MP3DEC    dec;
    MP3SYN    syn[2];
    MP3HDR    lock;                 /* the first frame's header: every frame must agree */
    uint32_t  audio_start;          /* after the tags at the start */
    uint32_t  audio_end;            /* before the tags at the end */
    uint32_t  first;                /* the first frame played */
    uint32_t  vbr_pos;              /* the frame with the VBR header, where its table counts from */
    uint32_t  free_len;             /* free format: a frame's bytes without padding */
    uint32_t  frames;               /* from a VBR header, 0 when not known */
    uint32_t  bytes;                /* from a VBR header, 0 when not known */
    uint32_t  length_ms;
    uint32_t  kbps;                 /* the average */
    uint8_t   toc[100];             /* Xing: where each hundredth starts, in 256ths of bytes */
    uint8_t   has_toc;
    uint8_t   taps;                 /* window taps of the synthesis: 16, or 14 fast */
    uint32_t *vbri;                 /* VBRI: bytes before each table entry, vbri_count + 1 */
    uint32_t  vbri_count;
    uint32_t  vbri_frames;          /* frames an entry covers */
    int16_t  *pcm;                  /* a frame's output */
    uint32_t  pcm_have, pcm_used;   /* frames of it */
    uint32_t  skip;                 /* frames to decode and not give out */
    const EQSET *eq;
    uint16_t  eq_serial;
    uint8_t   eq_known;             /* the tables are made for eq_serial */
} MP3STATE;

/* ------------------------------------------------------------------ */
/* bits                                                                */
/* ------------------------------------------------------------------ */
void mp3_bits_init( MP3BITS *bits, const uint8_t *data, unsigned int len, unsigned int bitpos )
{
    bits->data = data;
    bits->len = len;
    bits->pos = bitpos >> 3;
    bits->cache = 0;
    bits->count = 0;
    if ( bitpos & 7 ) {
        mp3_bits_get( bits, (int)(bitpos & 7) );
    }
}

uint32_t mp3_bits_get( MP3BITS *bits, int count )
{
    uint32_t val;

    if ( count <= 0 ) {
        return 0;
    }
    while ( bits->count <= 24 ) {
        bits->cache |= (uint32_t)(bits->pos < bits->len ? bits->data[bits->pos] : 0) << (24 - bits->count);
        bits->pos++;
        bits->count += 8;
    }
    val = bits->cache >> (32 - count);
    bits->cache <<= count;
    bits->count -= count;
    return val;
}

/* ------------------------------------------------------------------ */
/* frame headers                                                       */
/* ------------------------------------------------------------------ */
static int parse_header( uint32_t word, MP3HDR *hdr )
{
    uint32_t vbits = (word >> 19) & 3, lbits = (word >> 17) & 3;
    uint32_t br = (word >> 12) & 15, sr = (word >> 10) & 3;

    if ( (word & 0xFFE00000UL) != 0xFFE00000UL || vbits == 1 || lbits == 0 || br == 15
         || sr == 3 || (word & 3) == 2 ) {
        return 0;
    }
    hdr->version = (uint8_t)(vbits == 3 ? MV_1 : (vbits == 2 ? MV_2 : MV_25));
    hdr->layer = (uint8_t)(4 - lbits);
    hdr->crc = (uint8_t)(((word >> 16) & 1) == 0);
    hdr->br_index = (uint8_t)br;
    hdr->sr_index = (uint8_t)sr;
    hdr->padding = (uint8_t)((word >> 9) & 1);
    hdr->mode = (uint8_t)((word >> 6) & 3);
    hdr->mode_ext = (uint8_t)((word >> 4) & 3);
    hdr->channels = (uint8_t)(hdr->mode == MODE_MONO ? 1 : 2);
    hdr->sfreq = (uint8_t)(hdr->version * 3 + sr);
    hdr->rate = rate_tab[hdr->version][sr];
    hdr->kbps = kbps_tab[hdr->version != MV_1][hdr->layer - 1][br];
    if ( hdr->layer == 1 ) {
        hdr->samples = 384;
    } else if ( hdr->layer == 3 && hdr->version != MV_1 ) {
        hdr->samples = 576;
    } else {
        hdr->samples = 1152;
    }
    if ( br == 0 ) {
        hdr->length = 0;
    } else if ( hdr->layer == 1 ) {
        hdr->length = (12000UL * hdr->kbps / hdr->rate + hdr->padding) * 4;
    } else {
        hdr->length = (hdr->samples == 576 ? 72000UL : 144000UL) * hdr->kbps / hdr->rate
                      + hdr->padding;
    }
    return 1;
}

/* the same stream: version, layer, rate, and free format or not */
static int same_kind( const MP3HDR *lhs, const MP3HDR *rhs )
{
    return lhs->version == rhs->version && lhs->layer == rhs->layer
           && lhs->sr_index == rhs->sr_index && (lhs->br_index == 0) == (rhs->br_index == 0);
}

static uint32_t frame_len( const MP3STATE *st, const MP3HDR *hdr )
{
    if ( hdr->br_index != 0 ) {
        return hdr->length;
    }
    if ( st->free_len == 0 ) {
        return 0;
    }
    return st->free_len + hdr->padding * (hdr->layer == 1 ? 4UL : 1UL);
}

/* the four bytes at "pos", if they are before the end of the frames */
static int word_at( MP3STATE *st, uint32_t pos, uint32_t *word )
{
    const uint8_t *at;

    if ( pos > st->audio_end || st->audio_end - pos < 4 ) {
        return 0;
    }
    src_seek( st->src, pos );
    at = src_peek( st->src, 4 );
    if ( at == NULL ) {
        return 0;
    }
    *word = BE32( at );
    return 1;
}

/* A frame at "pos" that agrees with "lock" (NULL: with itself) and is
   followed by another that does, or by the end.  -> 1 and its header */
static int good_pair( MP3STATE *st, uint32_t pos, const MP3HDR *lock, MP3HDR *hdr )
{
    MP3HDR next;
    uint32_t word, len, after;

    if ( !word_at( st, pos, &word ) || !parse_header( word, hdr ) ) {
        return 0;
    }
    if ( lock == NULL ) {
        lock = hdr;
    }
    if ( !same_kind( lock, hdr ) ) {
        return 0;
    }
    len = frame_len( st, hdr );
    if ( len < MIN_FRAME || len > MP3_MAXFRAME || len > st->audio_end - pos ) {
        return 0;
    }
    after = pos + len;
    if ( st->audio_end - after < 4 ) {
        return 1;                           /* the last frame */
    }
    return word_at( st, after, &word ) && parse_header( word, &next ) && same_kind( lock, &next );
}

/* Free format: the length from the next header of the same kind,
   checked by the one after it.  -> 1 with st->free_len set */
static int find_free_len( MP3STATE *st, uint32_t pos, const MP3HDR *hdr )
{
    MP3HDR next, third;
    const uint8_t *buf;
    unsigned int have, off;
    uint32_t word, base, after, unit = hdr->layer == 1 ? 4 : 1;

    have = st->audio_end - pos;
    if ( have > MP3_MAXFRAME + 8 ) {
        have = MP3_MAXFRAME + 8;
    }
    src_seek( st->src, pos );
    buf = src_peek( st->src, have );
    for ( off = MIN_FRAME; buf != NULL && off + 4 <= have; off++ ) {
        if ( buf[off] != 0xFF || (buf[off + 1] & 0xE0) != 0xE0 ) {
            continue;
        }
        if ( !parse_header( BE32( buf + off ), &next ) || !same_kind( hdr, &next ) ) {
            continue;
        }
        if ( off < hdr->padding * unit + MIN_FRAME ) {
            continue;
        }
        base = off - hdr->padding * unit;
        after = pos + off + base + next.padding * unit;
        if ( after > st->audio_end || st->audio_end - after < 4 ) {
            st->free_len = base;            /* two frames and the end */
            return 1;
        }
        if ( word_at( st, after, &word ) && parse_header( word, &third ) && same_kind( hdr, &third ) ) {
            st->free_len = base;
            return 1;
        }
        src_seek( st->src, pos );           /* word_at may have moved the file's buffer */
        buf = src_peek( st->src, have );
    }
    return 0;
}

/* The first good pair from "pos" on, before "limit".  lock NULL: the
   first frame of the file, of any kind (a free format one is measured
   on the way).  -> its position, or st->audio_end when there is none */
static uint32_t find_sync( MP3STATE *st, uint32_t pos, uint32_t limit, const MP3HDR *lock, MP3HDR *hdr )
{
    MP3HDR cand;
    const uint8_t *buf;
    unsigned int have, i;
    uint32_t word;
    unsigned int free_tries = 0;

    if ( limit > st->audio_end ) {
        limit = st->audio_end;
    }
    while ( pos < limit && limit - pos >= 4 ) {
        have = limit - pos;
        if ( have > SCAN_WINDOW ) {
            have = SCAN_WINDOW;
        }
        src_seek( st->src, pos );
        buf = src_peek( st->src, have );
        if ( buf == NULL ) {
            return st->audio_end;
        }
        for ( i = 0; i + 1 < have; i++ ) {
            if ( buf[i] != 0xFF || (buf[i + 1] & 0xE0) != 0xE0 ) {
                continue;
            }
            if ( lock == NULL ) {
                /* measuring a free format frame reads up to 4K: a file of
                   nothing but such headers gets a limited number of tries */
                st->free_len = 0;
                if ( word_at( st, pos + i, &word ) && parse_header( word, &cand )
                     && cand.br_index == 0 && free_tries < FREE_TRIES ) {
                    free_tries++;
                    find_free_len( st, pos + i, &cand );
                }
            }
            if ( good_pair( st, pos + i, lock, hdr ) ) {
                return pos + i;
            }
            /* the checks moved the file's buffer: look at the window again */
            src_seek( st->src, pos );
            buf = src_peek( st->src, have );
            if ( buf == NULL ) {
                return st->audio_end;
            }
        }
        pos += have - 1;
    }
    return st->audio_end;
}

/* ------------------------------------------------------------------ */
/* tags                                                                */
/* ------------------------------------------------------------------ */
static uint32_t syncsafe( const uint8_t *at )
{
    return ((uint32_t)(at[0] & 0x7F) << 21) | ((uint32_t)(at[1] & 0x7F) << 14)
           | ((uint32_t)(at[2] & 0x7F) << 7) | (uint32_t)(at[3] & 0x7F);
}

/* the whole size of an ID3v2 tag whose first 10 bytes are "head", or 0 */
static uint32_t id3v2_size( const uint8_t *head )
{
    if ( head[0] != 'I' || head[1] != 'D' || head[2] != '3' || head[3] < 2 || head[3] > 4
         || head[4] == 0xFF || ((head[6] | head[7] | head[8] | head[9]) & 0x80) ) {
        return 0;
    }
    return 10 + syncsafe( head + 6 ) + (head[3] == 4 && (head[5] & 0x10) ? 10 : 0);
}

/* one text field: the encoding byte, then the text */
static void id3_text( char *dst, const uint8_t *data, unsigned int len )
{
    int big = 0;

    if ( len < 1 ) {
        return;
    }
    switch ( data[0] ) {
    case 0:
        tag_latin1( dst, TAG_LEN, data + 1, len - 1 );
        break;
    case 1:
    case 2:
        data++;
        len--;
        big = data[-1] == 2;
        if ( len >= 2 && data[0] == 0xFE && data[1] == 0xFF ) {
            big = 1;
            data += 2;
            len -= 2;
        } else if ( len >= 2 && data[0] == 0xFF && data[1] == 0xFE ) {
            big = 0;
            data += 2;
            len -= 2;
        }
        tag_utf16( dst, TAG_LEN, data, len, big );
        break;
    case 3:
        tag_utf8( dst, TAG_LEN, data + 1, len - 1 );
        break;
    }
}

/* the bytes of an ID3v2 tag, with ID3v2.2 and 2.3's unsynchronization
   (a 0 after every FF) taken out as they are read */
typedef struct {
    SRC     *src;
    uint32_t end;
    uint8_t  unsync;
    uint8_t  last_ff;
} ID3RD;

static int id3_byte( ID3RD *rd )
{
    int byte;

    if ( src_tell( rd->src ) >= rd->end ) {
        return -1;
    }
    byte = src_byte( rd->src );
    if ( byte == 0 && rd->unsync && rd->last_ff ) {
        if ( src_tell( rd->src ) >= rd->end ) {
            return -1;
        }
        byte = src_byte( rd->src );
    }
    rd->last_ff = (uint8_t)(byte == 0xFF);
    return byte;
}

static unsigned int id3_read( ID3RD *rd, uint8_t *dst, unsigned int len )
{
    unsigned int got;
    int byte;

    for ( got = 0; got < len; got++ ) {
        byte = id3_byte( rd );
        if ( byte < 0 ) {
            break;
        }
        dst[got] = (uint8_t)byte;
    }
    return got;
}

/* 0, or -1 when the tag ends first */
static int id3_skip( ID3RD *rd, uint32_t len )
{
    uint32_t left = src_tell( rd->src ) < rd->end ? rd->end - src_tell( rd->src ) : 0;

    if ( len > left ) {
        return -1;
    }
    if ( !rd->unsync ) {
        return src_skip( rd->src, len );
    }
    while ( len-- > 0 ) {
        if ( id3_byte( rd ) < 0 ) {
            return -1;
        }
    }
    return 0;
}

/* the frames of the ID3v2 tag at "pos", "total" bytes: the title,
   artist and album where info has none yet */
static void read_id3v2( SRC *src, uint32_t pos, uint32_t total, DECINFO *info )
{
    uint8_t head[10], fh[10], text[TEXT_MAX];
    const uint8_t *data;
    ID3RD rd;
    char *dst;
    uint32_t size;
    unsigned int take;
    uint32_t flags;
    unsigned int dlen, i, out;
    int ver, frames, hlen, skip;

    src_seek( src, pos );
    if ( src_read( src, head, 10 ) != 10 ) {
        return;
    }
    ver = head[3];
    if ( ver == 2 && (head[5] & 0x40) ) {
        return;                             /* ID3v2.2 compressed the whole tag */
    }
    rd.src = src;
    rd.end = pos + total - (ver == 4 && (head[5] & 0x10) ? 10 : 0);
    rd.unsync = (uint8_t)(ver < 4 && (head[5] & 0x80));
    rd.last_ff = 0;
    if ( ver >= 3 && (head[5] & 0x40) ) {
        if ( id3_read( &rd, fh, 4 ) != 4 ) {
            return;
        }
        size = ver == 3 ? BE32( fh ) : syncsafe( fh );
        if ( ver == 4 ) {
            size = size >= 4 ? size - 4 : 0;
        }
        if ( id3_skip( &rd, size ) != 0 ) {
            return;
        }
    }
    hlen = ver == 2 ? 6 : 10;
    for ( frames = 0; frames < 1000; frames++ ) {
        if ( id3_read( &rd, fh, (unsigned int)hlen ) != (unsigned int)hlen ) {
            return;
        }
        if ( !((fh[0] >= 'A' && fh[0] <= 'Z') || (fh[0] >= '0' && fh[0] <= '9')) ) {
            return;                         /* padding */
        }
        flags = 0;
        if ( ver == 2 ) {
            size = ((uint32_t)fh[3] << 16) | ((uint32_t)fh[4] << 8) | fh[5];
        } else {
            size = BE32( fh + 4 );
            if ( ver == 4 && !((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80) ) {
                size = syncsafe( fh + 4 );  /* some writers put a plain number here */
            }
            flags = BE16( fh + 8 );
        }
        dst = NULL;
        if ( ver == 2 ) {
            if ( mem_cmp( fh, "TT2", 3 ) == 0 ) {
                dst = info->title;
            } else if ( mem_cmp( fh, "TP1", 3 ) == 0 ) {
                dst = info->artist;
            } else if ( mem_cmp( fh, "TAL", 3 ) == 0 ) {
                dst = info->album;
            }
        } else {
            if ( mem_cmp( fh, "TIT2", 4 ) == 0 ) {
                dst = info->title;
            } else if ( mem_cmp( fh, "TPE1", 4 ) == 0 ) {
                dst = info->artist;
            } else if ( mem_cmp( fh, "TALB", 4 ) == 0 ) {
                dst = info->album;
            }
        }
        /* compressed or encrypted frames are not read */
        skip = ver == 3 ? (flags & 0x00C0) != 0 : (ver == 4 ? (flags & 0x000C) != 0 : 0);
        if ( dst == NULL || dst[0] != 0 || skip ) {
            if ( id3_skip( &rd, size ) != 0 ) {
                return;
            }
            continue;
        }
        take = size < TEXT_MAX ? size : TEXT_MAX;
        if ( id3_read( &rd, text, take ) != take || id3_skip( &rd, size - take ) != 0 ) {
            return;
        }
        data = text;
        dlen = take;
        if ( ver == 3 && (flags & 0x0020) && dlen > 0 ) {
            data++;                         /* a group byte */
            dlen--;
        }
        if ( ver == 4 ) {
            if ( (flags & 0x0040) && dlen > 0 ) {
                data++;
                dlen--;
            }
            if ( (flags & 0x0001) && dlen >= 4 ) {
                data += 4;                  /* the length before unsynchronization */
                dlen -= 4;
            }
            if ( (flags & 0x0002) || (head[5] & 0x80) ) {
                for ( i = 0, out = 0; i < dlen; i++ ) {
                    if ( !(i > 0 && data[i] == 0 && data[i - 1] == 0xFF) ) {
                        text[out++] = data[i];
                    }
                }
                data = text;
                dlen = out;
            }
        }
        id3_text( dst, data, dlen );
    }
}

/* the items of an APE tag: "len" bytes at "pos" */
static void read_ape( SRC *src, uint32_t pos, uint32_t len, uint32_t items, DECINFO *info )
{
    uint8_t head[8], text[TEXT_MAX];
    char key[16];
    char *dst;
    uint32_t end = pos + len, size;
    unsigned int take, keylen;
    int byte;

    src_seek( src, pos );
    if ( items > 256 ) {
        items = 256;
    }
    while ( items-- > 0 && src_tell( src ) + 9 <= end ) {
        if ( src_read( src, head, 8 ) != 8 ) {
            return;
        }
        size = LE32( head );
        keylen = 0;
        for ( ;; ) {
            if ( src_tell( src ) >= end ) {
                return;
            }
            byte = src_byte( src );
            if ( byte <= 0 ) {
                break;
            }
            if ( keylen + 1 < sizeof( key ) ) {
                key[keylen++] = (char)byte;
            }
        }
        key[keylen] = 0;
        if ( byte < 0 || size > end - src_tell( src ) ) {
            return;
        }
        dst = NULL;
        if ( str_icmp( key, "Title" ) == 0 ) {
            dst = info->title;
        } else if ( str_icmp( key, "Artist" ) == 0 ) {
            dst = info->artist;
        } else if ( str_icmp( key, "Album" ) == 0 ) {
            dst = info->album;
        }
        if ( dst != NULL && dst[0] == 0 && (LE32( head + 4 ) & 6) == 0 ) {
            take = size < TEXT_MAX ? size : TEXT_MAX;
            if ( src_read( src, text, take ) != take ) {
                return;
            }
            tag_utf8( dst, TAG_LEN, text, take );
            size -= take;
        }
        if ( src_skip( src, size ) != 0 ) {
            return;
        }
    }
}

/* The tags at the end - ID3v1 (and its TAG+), APE, Lyrics3 - in any
   order the writers use.  -> where the frames end */
static uint32_t end_tags( SRC *src, uint32_t start, DECINFO *info )
{
    uint8_t v1[128], foot[32];
    const uint8_t *at;
    uint32_t end = src->size, size, items, total, look, digit;
    int round, have_v1 = 0, k, where;

    if ( end - start >= 128 && src_seek( src, end - 128 ) == 0 && src_read( src, v1, 128 ) == 128
         && mem_cmp( v1, "TAG", 3 ) == 0 ) {
        have_v1 = 1;
        end -= 128;
        if ( end - start >= 227 && src_seek( src, end - 227 ) == 0
             && src_read( src, foot, 4 ) == 4 && mem_cmp( foot, "TAG+", 4 ) == 0 ) {
            end -= 227;
        }
    }
    for ( round = 0; round < 4; round++ ) {
        if ( end - start >= 32 && src_seek( src, end - 32 ) == 0 && src_read( src, foot, 32 ) == 32
             && mem_cmp( foot, "APETAGEX", 8 ) == 0 ) {
            size = LE32( foot + 12 );
            items = LE32( foot + 16 );
            if ( size < 32 || size > end - start ) {
                break;
            }
            total = size + ((LE32( foot + 20 ) & 0x80000000UL) ? 32 : 0);
            if ( total > end - start ) {
                total = size;
            }
            read_ape( src, end - size, size - 32, items, info );
            end -= total;
            continue;
        }
        if ( end - start >= 15 && src_seek( src, end - 15 ) == 0 && src_read( src, foot, 15 ) == 15
             && mem_cmp( foot + 6, "LYRICS200", 9 ) == 0 ) {
            size = 0;
            for ( k = 0; k < 6; k++ ) {
                digit = (uint32_t)foot[k] - '0';
                if ( digit > 9 ) {
                    break;
                }
                size = size * 10 + digit;
            }
            if ( k == 6 && size >= 11 && size <= end - start - 15 && src_seek( src, end - 15 - size ) == 0
                 && src_read( src, foot, 11 ) == 11 && mem_cmp( foot, "LYRICSBEGIN", 11 ) == 0 ) {
                end -= size + 15;
                continue;
            }
        }
        if ( end - start >= 20 && src_seek( src, end - 9 ) == 0 && src_read( src, foot, 9 ) == 9
             && mem_cmp( foot, "LYRICSEND", 9 ) == 0 ) {
            look = end - start < 5100 + 9 ? end - start : 5100 + 9;
            src_seek( src, end - look );
            at = src_peek( src, look );
            where = at != NULL ? mem_find( at, look, "LYRICSBEGIN" ) : -1;
            if ( where >= 0 ) {
                end -= look - (uint32_t)where;
                continue;
            }
        }
        break;
    }
    if ( have_v1 ) {
        if ( info->title[0] == 0 ) {
            tag_latin1( info->title, TAG_LEN, v1 + 3, 30 );
        }
        if ( info->artist[0] == 0 ) {
            tag_latin1( info->artist, TAG_LEN, v1 + 33, 30 );
        }
        if ( info->album[0] == 0 ) {
            tag_latin1( info->album, TAG_LEN, v1 + 63, 30 );
        }
    }
    return end;
}

/* ------------------------------------------------------------------ */
/* the VBR header in the first frame                                   */
/* ------------------------------------------------------------------ */
/* -> 0 none, 1 a VBR header, 2 LAME's "Info" (a constant rate) */
static int read_vbr( MP3STATE *st, const MP3HDR *hdr, uint32_t pos, uint32_t len )
{
    const uint8_t *frame, *at;
    uint32_t off, flags, entries, scale, esize, i, k, sum, val, left;
    int pass;

    if ( hdr->layer != 3 ) {
        return 0;
    }
    src_seek( st->src, pos );
    frame = src_peek( st->src, len );
    if ( frame == NULL ) {
        return 0;
    }
    if ( hdr->version == MV_1 ) {
        off = hdr->channels == 1 ? 21 : 36;
    } else {
        off = hdr->channels == 1 ? 13 : 21;
    }
    for ( pass = 0; pass < 2; pass++, off += 2 ) {
        if ( off + 8 > len ) {
            break;
        }
        at = frame + off;
        if ( mem_cmp( at, "Xing", 4 ) != 0 && mem_cmp( at, "Info", 4 ) != 0 ) {
            continue;
        }
        flags = BE32( at + 4 );
        at += 8;
        left = len - off - 8;
        if ( (flags & 1) && left >= 4 ) {
            st->frames = BE32( at );
            at += 4;
            left -= 4;
        }
        if ( (flags & 2) && left >= 4 ) {
            st->bytes = BE32( at );
            at += 4;
            left -= 4;
        }
        if ( (flags & 4) && left >= 100 ) {
            st->has_toc = 1;
            for ( i = 0; i < 100; i++ ) {
                st->toc[i] = at[i];
                if ( i > 0 && at[i] < at[i - 1] ) {
                    st->has_toc = 0;        /* not a table of positions */
                }
            }
        }
        st->vbr_pos = pos;
        return frame[off] == 'X' ? 1 : 2;
    }
    if ( len >= 36 + 26 && mem_cmp( frame + 36, "VBRI", 4 ) == 0 ) {
        at = frame + 36;
        st->bytes = BE32( at + 10 );
        st->frames = BE32( at + 14 );
        entries = BE16( at + 18 );
        scale = BE16( at + 20 );
        esize = BE16( at + 22 );
        st->vbri_frames = BE16( at + 24 );
        st->vbr_pos = pos;
        if ( entries > 0 && esize >= 1 && esize <= 4 && st->vbri_frames > 0
             && entries * esize <= len - 36 - 26 ) {
            st->vbri = (uint32_t *)try_alloc( (entries + 1) * sizeof( uint32_t ) );
            if ( st->vbri != NULL ) {
                at += 26;
                sum = 0;
                st->vbri[0] = 0;
                for ( i = 0; i < entries; i++ ) {
                    val = 0;
                    for ( k = 0; k < esize; k++ ) {
                        val = (val << 8) | *at++;
                    }
                    /* held to the file's size so that the sum cannot wrap */
                    if ( val > st->src->size || scale > st->src->size / (val ? val : 1) ) {
                        val = 0;
                    } else {
                        val *= scale;
                    }
                    sum = val > st->src->size - sum ? st->src->size : sum + val;
                    st->vbri[i + 1] = sum;
                }
                st->vbri_count = entries;
            }
        }
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* detection                                                           */
/* ------------------------------------------------------------------ */
/* a frame header at buf[i] followed by another of its kind inside buf
   (or reaching past buf's end).  Free format frames are measured only
   while free_left lasts */
static int pair_in( const uint8_t *buf, unsigned int len, unsigned int i, int *free_left )
{
    MP3HDR one, two;
    unsigned int next, k;

    if ( i + 4 > len || !parse_header( BE32( buf + i ), &one ) ) {
        return 0;
    }
    if ( one.br_index != 0 ) {
        next = i + one.length;
        if ( next + 4 > len ) {
            return one.length >= MIN_FRAME && next >= len;
        }
        return parse_header( BE32( buf + next ), &two ) && same_kind( &one, &two );
    }
    if ( *free_left <= 0 ) {
        return 0;                           /* enough free format tries */
    }
    (*free_left)--;
    for ( k = i + MIN_FRAME; k + 4 <= len && k - i <= MP3_MAXFRAME; k++ ) {
        if ( buf[k] == 0xFF && parse_header( BE32( buf + k ), &two ) && same_kind( &one, &two ) ) {
            return 1;
        }
    }
    return 0;
}

static int mp3_detect( SRC *src, const uint8_t *head, unsigned int len, const char *ext )
{
    const uint8_t *buf = head;
    uint32_t start = 0;
    unsigned int have = len, i;
    int named, id3 = 0, free_left = 16;

    named = str_icmp( ext, "MP3" ) == 0 || str_icmp( ext, "MP2" ) == 0
            || str_icmp( ext, "MP1" ) == 0 || str_icmp( ext, "MPA" ) == 0;
    if ( len >= 10 ) {
        start = id3v2_size( head );
        id3 = start != 0;
    }
    if ( start > 0 && start + 4 > len ) {
        /* a tag longer than what was handed over: look past it */
        if ( start >= src->size ) {
            return named ? 10 : 0;
        }
        have = src->size - start;
        if ( have > SRC_PEEK ) {
            have = SRC_PEEK;
        }
        src_seek( src, start );
        buf = src_peek( src, have );
        if ( buf == NULL ) {
            src_seek( src, 0 );
            return named ? 10 : 0;
        }
        start = 0;
    }
    for ( i = start; i + 4 <= have; i++ ) {
        if ( buf[i] == 0xFF && (buf[i + 1] & 0xE0) == 0xE0 && pair_in( buf, have, i, &free_left ) ) {
            src_seek( src, 0 );
            if ( i == start ) {
                return id3 ? 95 : 90;
            }
            return named ? 80 : 60;
        }
    }
    src_seek( src, 0 );
    return named ? 10 : 0;
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */
/* the equalizer's tables, made again only when it changes */
static void eq_check( MP3STATE *st )
{
    const EQSET *eq = st->eq;
    int i, flat;

    if ( eq == NULL || !eq->on ) {
        st->dec.eq_on = 0;
        st->eq_known = 0;
        return;
    }
    if ( st->eq_known && eq->serial == st->eq_serial ) {
        return;
    }
    st->eq_serial = eq->serial;
    st->eq_known = 1;
    flat = eq->preamp == 0;
    for ( i = 0; i < EQ_BANDS; i++ ) {
        if ( eq->band[i] != 0 ) {
            flat = 0;
        }
    }
    st->dec.eq_on = (uint8_t)!flat;
    if ( !flat ) {
        eq_curve( eq, st->lock.rate, 32, st->dec.eq32 );
        if ( st->lock.layer == 3 ) {
            mp3_l3_eq( &st->dec, eq, st->lock.rate );
        }
    }
}

/* one frame into st->pcm */
static void decode_frame( MP3STATE *st, const MP3HDR *hdr, const uint8_t *frame, unsigned int len )
{
    MP3DEC *dec = &st->dec;
    unsigned int slot, step, nsb = dec->nsb, out_ch = dec->out_ch, ch;
    int16_t *pcm = st->pcm;

    dec->hdr = *hdr;
    if ( hdr->layer == 1 ) {
        mp3_layer1( dec, frame, len );
    } else if ( hdr->layer == 2 ) {
        mp3_layer2( dec, frame, len );
    } else {
        mp3_layer3( dec, frame, len );
    }
    if ( out_ch == 2 && hdr->channels == 1 ) {
        mem_cpy( dec->sb[1], dec->sb[0], dec->slots * MP3_SB * sizeof( int32_t ) );
    }
    step = nsb * out_ch;
    for ( slot = 0; slot < dec->slots; slot++ ) {
        for ( ch = 0; ch < out_ch; ch++ ) {
            mp3_synth( &st->syn[ch], dec->sb[ch] + slot * MP3_SB, (int)nsb, pcm + ch, (int)out_ch,
                       st->taps );
        }
        pcm += step;
    }
    st->pcm_have = dec->slots * nsb;
    st->pcm_used = 0;
}

/* the next frame decoded into st->pcm.  0 at the end */
static int next_frame( MP3STATE *st )
{
    MP3HDR hdr;
    const uint8_t *frame;
    uint32_t pos, word, len;

    for ( ;; ) {
        pos = src_tell( st->src );
        if ( !word_at( st, pos, &word ) ) {
            return 0;
        }
        len = 0;
        if ( parse_header( word, &hdr ) && same_kind( &st->lock, &hdr ) ) {
            len = frame_len( st, &hdr );
        }
        if ( len < MIN_FRAME || len > MP3_MAXFRAME || len > st->audio_end - pos ) {
            /* lost: the next good pair of frames */
            pos = find_sync( st, pos + 1, st->audio_end, &st->lock, &hdr );
            if ( pos >= st->audio_end ) {
                src_seek( st->src, st->audio_end );
                return 0;
            }
            src_seek( st->src, pos );
            continue;
        }
        src_seek( st->src, pos );
        frame = src_peek( st->src, len );
        if ( frame == NULL ) {
            return 0;
        }
        eq_check( st );
        decode_frame( st, &hdr, frame, len );
        src_seek( st->src, pos + len );
        if ( st->skip > 0 ) {
            st->skip--;
            st->pcm_have = 0;
            continue;
        }
        return 1;
    }
}

static int mp3_decode( void *state, DECBUF *buf )
{
    MP3STATE *st = (MP3STATE *)state;
    unsigned int take, nch = st->dec.out_ch;

    buf->frames = 0;
    buf->ev_count = 0;
    while ( buf->frames < buf->max ) {
        if ( st->pcm_used >= st->pcm_have ) {
            if ( !next_frame( st ) ) {
                break;
            }
            continue;
        }
        take = st->pcm_have - st->pcm_used;
        if ( take > buf->max - buf->frames ) {
            take = buf->max - buf->frames;
        }
        mem_cpy( buf->pcm + buf->frames * nch, st->pcm + st->pcm_used * nch,
                 take * nch * sizeof( int16_t ) );
        st->pcm_used += take;
        buf->frames += take;
    }
    return buf->frames == 0 && st->src->error ? DE_READ : DE_OK;
}

/* ------------------------------------------------------------------ */
/* opening, seeking, closing                                           */
/* ------------------------------------------------------------------ */
static void mp3_close( void *state )
{
    MP3STATE *st = (MP3STATE *)state;
    int ch;

    if ( st == NULL ) {
        return;
    }
    mp3_l3_close( &st->dec );
    for ( ch = 0; ch < 2; ch++ ) {
        mp3_syn_close( &st->syn[ch] );
        if ( st->dec.sb[ch] != NULL ) {
            xfree( st->dec.sb[ch] );
        }
    }
    if ( st->pcm != NULL ) {
        xfree( st->pcm );
    }
    if ( st->vbri != NULL ) {
        xfree( st->vbri );
    }
    xfree( st );
}

/* a * b / c for numbers that come from the file: the processor's divide
   faults when the answer does not fit, so a result that would not is
   held at the largest there is, and so is a divide by 0 */
static uint32_t muldiv_sat( uint32_t a, uint32_t b, uint32_t c )
{
    if ( c == 0 || (b != 0 && a / c >= 0xFFFFFFFFUL / b) ) {
        return 0xFFFFFFFFUL;
    }
    return fx_umuldiv( a, b, c );
}

/* base + off, or "limit" when that is past it (or would wrap) */
static uint32_t pos_add( uint32_t base, uint32_t off, uint32_t limit )
{
    return base > limit || off > limit - base ? limit : base + off;
}

/* the bytes of a frame, on average */
static uint32_t avg_frame( const MP3STATE *st )
{
    uint32_t bytes = st->audio_end - st->first, count = st->frames, avg;

    if ( count == 0 ) {
        avg = fx_umuldiv( st->kbps, 125UL * st->lock.samples, st->lock.rate );
    } else {
        if ( st->bytes != 0 && st->bytes <= bytes + MP3_MAXFRAME ) {
            bytes = st->bytes;
        }
        avg = bytes / count;
    }
    return avg < MIN_FRAME ? MIN_FRAME : avg;
}

static int mp3_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    MP3STATE *st;
    MP3HDR hdr;
    uint8_t head[10];
    uint32_t pos = 0, size, len, bytes, spf, rate;
    int tags, vbr, ch, down;

    *state = NULL;
    st = (MP3STATE *)try_alloc( sizeof( MP3STATE ) );
    if ( st == NULL ) {
        return DE_NOMEM;
    }
    mem_set( st, 0, sizeof( MP3STATE ) );
    st->src = src;

    /* the tags at the start, one after another */
    for ( tags = 0; tags < 4; tags++ ) {
        src_seek( src, pos );
        if ( src_read( src, head, 10 ) != 10 ) {
            break;
        }
        size = id3v2_size( head );
        if ( size == 0 || size > src->size - pos ) {
            break;
        }
        read_id3v2( src, pos, size, info );
        pos += size;
    }
    st->audio_start = pos;
    st->audio_end = src->size;
    st->audio_end = end_tags( src, st->audio_start, info );

    /* the first frame */
    size = st->audio_end - st->audio_start > FIRST_SCAN ? st->audio_start + FIRST_SCAN : st->audio_end;
    pos = find_sync( st, st->audio_start, size, NULL, &hdr );
    if ( pos >= st->audio_end ) {
        mp3_close( st );
        return src->error ? DE_READ : DE_FORMAT;
    }
    st->lock = hdr;
    st->first = pos;
    len = frame_len( st, &hdr );
    vbr = read_vbr( st, &hdr, pos, len );
    if ( vbr != 0 ) {
        st->first = pos + len;              /* the header frame is not sound */
    }
    if ( st->frames > MAX_FRAMES ) {
        st->frames = 0;
        st->has_toc = 0;
    }
    if ( st->bytes > src->size ) {
        st->bytes = 0;
    }
    if ( vbr == 2 ) {
        st->has_toc = 0;                    /* a constant rate: a position is exact, the table is not */
    }

    /* what to make */
    spf = hdr.samples;
    rate = hdr.rate;
    down = opt->down > 2 ? 2 : opt->down;
    while ( down > 0 && (rate >> down) < 8000 ) {
        down--;
    }
    st->dec.nsb = (uint8_t)(MP3_SB >> down);
    st->dec.out_ch = (uint8_t)(hdr.channels == 1 || opt->mono ? 1 : 2);
    st->dec.fast = opt->fast;
    st->taps = (uint8_t)(opt->fast ? 14 : 16);
    st->eq = opt->eq;
    for ( ch = 0; ch < st->dec.out_ch; ch++ ) {
        st->dec.sb[ch] = (int32_t *)try_alloc( MP3_SLOTS * MP3_SB * sizeof( int32_t ) );
        if ( st->dec.sb[ch] == NULL || mp3_syn_open( &st->syn[ch] ) != DE_OK ) {
            mp3_close( st );
            return DE_NOMEM;
        }
        mem_set( st->dec.sb[ch], 0, MP3_SLOTS * MP3_SB * sizeof( int32_t ) );
    }
    st->pcm = (int16_t *)try_alloc( MP3_SLOTS * MP3_SB * 2 * sizeof( int16_t ) );
    if ( st->pcm == NULL || (hdr.layer == 3 && mp3_l3_open( &st->dec, hdr.sfreq ) != DE_OK) ) {
        mp3_close( st );
        return DE_NOMEM;
    }

    /* the length and the rate */
    if ( hdr.br_index == 0 ) {
        st->kbps = fx_umuldiv( frame_len( st, &hdr ) * 8, rate, spf * 1000UL );
    } else {
        st->kbps = hdr.kbps;
    }
    if ( st->kbps == 0 ) {
        st->kbps = 1;
    }
    bytes = st->audio_end - st->first;
    if ( st->frames > bytes / MIN_FRAME + 1 ) {
        st->frames = 0;                     /* more frames than the file could hold */
        st->has_toc = 0;
    }
    if ( st->frames != 0 ) {
        st->length_ms = fx_umuldiv( st->frames, spf * 1000UL, rate );
        if ( st->bytes != 0 && st->bytes <= bytes + len ) {
            bytes = st->bytes;
        }
        if ( vbr != 2 && st->length_ms > 0 && bytes / 512 < st->length_ms ) {
            st->kbps = fx_umuldiv( bytes, 8, st->length_ms );
            if ( st->kbps == 0 ) {
                st->kbps = 1;
            }
        }
    } else {
        st->length_ms = muldiv_sat( bytes, 8, st->kbps );
    }

    info->file_rate = rate;
    info->rate = rate >> down;
    info->file_channels = hdr.channels;
    info->channels = st->dec.out_ch;
    info->bits = 0;
    info->kbps = st->kbps;
    info->length_ms = st->length_ms;
    info->flags = DIF_SEEK | DIF_EQ | (vbr == 1 ? DIF_VBR : 0);
    sfmt( info->format, sizeof( info->format ), "%s layer %u%s, %s", ver_name[hdr.version],
          hdr.layer, hdr.br_index == 0 ? ", free format" : "", mode_name[hdr.mode] );
    src_seek( src, st->first );
    *state = st;
    return DE_OK;
}

static int mp3_seek( void *state, uint32_t ms, uint32_t *at_ms )
{
    MP3STATE *st = (MP3STATE *)state;
    MP3HDR hdr;
    uint32_t pos, avg, prime, back, index, pct, frac, low, high, bytes, frame;
    int ch;

    for ( ch = 0; ch < st->dec.out_ch; ch++ ) {
        mp3_syn_reset( &st->syn[ch] );
    }
    mp3_l3_reset( &st->dec );
    st->pcm_have = st->pcm_used = 0;
    st->skip = 0;
    if ( ms > st->length_ms ) {
        ms = st->length_ms;
    }
    if ( ms == 0 ) {
        src_seek( st->src, st->first );
        *at_ms = 0;
        return DE_OK;
    }
    avg = avg_frame( st );
    if ( st->lock.layer == 3 ) {
        prime = 511 / avg + 2;
        if ( prime > PRIME_MAX ) {
            prime = PRIME_MAX;
        }
    } else {
        prime = st->lock.layer == 1 ? 2 : 1;
    }

    /* where the time is in the file */
    bytes = st->bytes != 0 ? st->bytes : st->audio_end - st->vbr_pos;
    if ( st->has_toc && st->length_ms > 0 ) {
        pct = fx_umuldiv( ms, 100, st->length_ms );
        if ( pct > 99 ) {
            pct = 99;
        }
        frac = fx_umuldiv( ms, 25600, st->length_ms ) - pct * 256;
        if ( frac > 255 ) {
            frac = 255;
        }
        low = st->toc[pct];
        high = pct < 99 ? st->toc[pct + 1] : 256;
        pos = pos_add( st->vbr_pos, fx_umuldiv( low * 256 + (high - low) * frac, bytes, 65536UL ),
                       st->audio_end );
    } else if ( st->vbri != NULL && st->frames != 0 ) {
        frame = fx_umuldiv( ms, st->lock.rate, st->lock.samples * 1000UL );
        index = frame / st->vbri_frames;
        if ( index >= st->vbri_count ) {
            pos = pos_add( st->first, st->vbri[st->vbri_count], st->audio_end );
        } else {
            /* the entry's start, and as far into it as the frame is */
            pos = pos_add( st->first, st->vbri[index], st->audio_end );
            pos = pos_add( pos, fx_umuldiv( st->vbri[index + 1] - st->vbri[index],
                                            frame - index * st->vbri_frames, st->vbri_frames ),
                           st->audio_end );
        }
    } else {
        pos = pos_add( st->first, muldiv_sat( ms, st->kbps, 8 ), st->audio_end );
    }
    if ( pos < st->first || pos > st->audio_end ) {
        pos = st->audio_end;
    }

    /* a few frames earlier, to fill the reservoir and the overlap */
    back = prime * avg;
    if ( pos - st->first < back ) {
        prime = (pos - st->first) / avg;
        back = pos - st->first;
    }
    pos -= back;
    pos = find_sync( st, pos, st->audio_end, &st->lock, &hdr );
    if ( pos >= st->audio_end ) {
        src_seek( st->src, st->audio_end );
        *at_ms = st->length_ms;
        return DE_OK;
    }
    src_seek( st->src, pos );
    st->skip = prime;
    if ( st->has_toc || st->vbri != NULL ) {
        *at_ms = ms;
    } else {
        *at_ms = muldiv_sat( pos - st->first, 8, st->kbps );
        if ( *at_ms > st->length_ms ) {
            *at_ms = st->length_ms;
        }
        frame = fx_umuldiv( prime * st->lock.samples, 1000, st->lock.rate );
        *at_ms = frame > st->length_ms - *at_ms ? st->length_ms : *at_ms + frame;
    }
    return DE_OK;
}

const CODEC codec_mp3 = {
    "MPEG audio",
    "MP3 MP2 MP1 MPA",
    mp3_detect,
    mp3_open,
    mp3_decode,
    mp3_seek,
    mp3_close
};
