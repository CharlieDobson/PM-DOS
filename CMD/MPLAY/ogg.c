/*
 * OGG.C - Ogg Vorbis files: the Ogg pages and packets around a Vorbis
 * stream, the codec MPLAY sees, its timing and its seeking (DEC.H).
 *
 * AN OGG FILE IS A RUN OF PAGES.  Each page starts "OggS", says which
 * logical stream it belongs to (a serial number) and where that
 * stream's time stands at its end (the granule position), and carries a
 * table of segment lengths: a packet is the segments up to one shorter
 * than 255 bytes, and it may go on into the next page.  A CRC covers the
 * whole page, so a damaged page is found and skipped, and the next
 * "OggS" with a good CRC is where decoding picks up again.
 *
 * ONE FILE CAN HOLD MANY STREAMS.  Streams side by side (a film's
 * picture and its sound) start together, each with a page marked as a
 * beginning; the first Vorbis one is played and the pages of the rest
 * are passed over.  Streams one after another (a chain: a radio
 * recording, an album in one file) each bring their own headers, which
 * are read when the stream playing gives way to the next.  A link with
 * a different sample rate ends playback there, because changing the
 * rate is a resampler's work; a different number of channels is mixed
 * to the channels of the first link.
 *
 * TIME COMES FROM THE GRANULE POSITIONS.  Every packet finishes a known
 * number of samples (a quarter of its block and a quarter of the one
 * before), so the packets completed on a page can be placed exactly by
 * counting back from the page's granule position.  That gives the start
 * of the stream, which may lie before zero (those samples are dropped),
 * the end of the last page (where the last packet is cut short), and,
 * after a seek, where the first samples decoded belong.
 *
 * A SEEK HALVES THE FILE until it finds the last page that ends at least
 * two long blocks before the target, decodes from there, and drops the
 * samples up to the exact one asked for.  The length is the last page's
 * granule position; a chained file is measured link by link, also by
 * halving, so that nothing has to be read from end to end at open.
 *
 * DAMAGE COSTS BOUNDED WORK.  A decode call gives up after wading
 * through two megabytes of pages that fail their CRC and returns a
 * little silence instead, so even a file of nothing but false page
 * headers keeps the player answering.
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"
#include "vb.h"

#define PAGE_MAX        (27UL + 255 + 255UL * 255)
#define BUF_START       (32UL * 1024)
#define PKT_MAX         (1UL << 21)     /* a longer packet is dropped */
#define MAX_LINKS       64
#define MAX_GROUP       16              /* streams that start together, tracked */
#define WORK_DECODE     (1UL << 21)     /* bytes of damage one decode call wades through */
#define WORK_SEEK       (1UL << 23)
#define TAIL_MAX        (1UL << 20)     /* how far back from the end the last page is sought */
#define STRETCH         65536UL         /* a seek's halving stops at this, then goes page by page */
#define PREROLL         8192L           /* samples decoded before a seek's target, at least */
#define SILENCE         64              /* frames given for a call that found only damage */
#define QMAX            1024            /* packets held while waiting for a granule position */
#define F_CONT          1
#define F_BOS           2
#define F_EOS           4
#define NO_END          0x7FFFFFFFL
#define ANY_SERIAL      0
#define ONE_SERIAL      1

typedef struct {
    uint32_t offset;            /* the link's first page */
    uint32_t end;               /* the next link's first page, or the end of the file */
    uint32_t serial;            /* its Vorbis stream */
    uint32_t group[MAX_GROUP];  /* every stream that starts with it */
    int      ngroup;
    uint32_t rate;
    uint32_t base_lo, base_hi;  /* the granule position of its first sample */
    uint32_t start;             /* that sample, counted from the start of the file */
    uint32_t frames;            /* its length, 0 when not known */
} LINK;

typedef struct {
    SRC           *src;
    uint32_t       size;
    uint32_t       crc[256];
    /* the page in hand */
    uint8_t       *page;
    uint32_t       ppos, plen, pnseg, pserial, pseq, glo, ghi;
    uint8_t        pflags;
    uint32_t       work, work_max;
    /* packets: the one being put together follows those queued from the page */
    uint8_t       *buf;
    unsigned int   buf_size, buf_used, part_at;
    int            dropping;
    unsigned int   nq, qi;          /* packets queued, the next to decode */
    unsigned int   ntimed, nplaced; /* how many have their length, their place in time */
    unsigned int   q_at[QMAX], q_len[QMAX], q_dur[QMAX];
    int32_t        q_pos[QMAX];
    uint8_t        q_fresh[QMAX];   /* after lost pages: the decoder starts afresh here */
    int            gap;
    int32_t        q_end;
    int            have_seq;
    uint32_t       next_seq;
    /* the stream playing */
    VB            *vb;
    VBID           id;
    VBOPT          opt;
    DECOPT         ask;
    uint32_t       rate;            /* the first link's: every link must have it */
    int            link;
    unsigned int   last_bs;
    int            pos_known, at_start, base_set;
    int32_t        pos, keep_from;
    int            ended, eos_seen;
    const int16_t *out;
    unsigned int   out_left;
    LINK           links[MAX_LINKS];
    int            nlinks;
} OGG;

/* ------------------------------------------------------------------ */
/* pages                                                               */
/* ------------------------------------------------------------------ */
static void crc_init( OGG *og )
{
    uint32_t i, r;
    int k;

    for ( i = 0; i < 256; i++ ) {
        r = i << 24;
        for ( k = 0; k < 8; k++ ) {
            r = (r & 0x80000000UL) ? (r << 1) ^ 0x04C11DB7UL : r << 1;
        }
        og->crc[i] = r;
    }
}

/* the CRC of a page with its own CRC field taken as zero */
static uint32_t page_crc( const OGG *og, const uint8_t *page, unsigned int len )
{
    uint32_t crc = 0;
    unsigned int i;

    for ( i = 0; i < 22; i++ ) {
        crc = (crc << 8) ^ og->crc[((crc >> 24) ^ page[i]) & 255];
    }
    for ( ; i < 26; i++ ) {
        crc = (crc << 8) ^ og->crc[(crc >> 24) & 255];
    }
    for ( ; i < len; i++ ) {
        crc = (crc << 8) ^ og->crc[((crc >> 24) ^ page[i]) & 255];
    }
    return crc;
}

/* The next good page that starts before "limit", into og->page.
   -> 1, 0 (none), -1 (the file could not be read), -2 (too much
   damage for one call: og->work passed og->work_max) */
static int page_next( OGG *og, uint32_t limit )
{
    SRC *src = og->src;
    const uint8_t *head;
    uint32_t pos, left;
    unsigned int want, hlen, body, i;
    int at;

    for ( ;; ) {
        if ( og->work > og->work_max ) {
            return -2;
        }
        pos = src_tell( src );
        left = src_left( src );
        if ( pos >= limit || left < 27 ) {
            return 0;
        }
        head = src_peek( src, 27 );
        if ( head == NULL ) {
            return src->error ? -1 : 0;
        }
        if ( mem_cmp( head, "OggS", 4 ) != 0 || head[4] != 0 ) {
            /* not a page: look for the next capture pattern */
            want = left < SRC_PEEK ? left : SRC_PEEK;
            head = src_peek( src, want );
            if ( head == NULL ) {
                return src->error ? -1 : 0;
            }
            at = mem_find( head + 1, want - 1, "OggS" );
            src_skip( src, at >= 0 ? (uint32_t)at + 1 : want - 3 );
            og->work += want;
            continue;
        }
        hlen = 27 + (unsigned int)head[26];
        head = left >= hlen ? src_peek( src, hlen ) : NULL;
        body = 0;
        if ( head != NULL ) {
            for ( i = 27; i < hlen; i++ ) {
                body += head[i];
            }
        }
        if ( head == NULL || hlen + body > left ) {
            if ( src->error ) {
                return -1;
            }
            src_skip( src, 1 );         /* cut off by the end, or a false start */
            og->work += hlen;
            continue;
        }
        mem_cpy( og->page, head, hlen );
        src_skip( src, hlen );
        if ( src_read( src, og->page + hlen, body ) != body ) {
            return src->error ? -1 : 0;
        }
        if ( page_crc( og, og->page, hlen + body ) != LE32( og->page + 22 ) ) {
            og->work += hlen + body;
            src_seek( src, pos + 1 );
            continue;
        }
        og->ppos = pos;
        og->plen = hlen + body;
        og->pnseg = hlen - 27;
        og->pflags = og->page[5];
        og->glo = LE32( og->page + 6 );
        og->ghi = LE32( og->page + 10 );
        og->pserial = LE32( og->page + 14 );
        og->pseq = LE32( og->page + 18 );
        return 1;
    }
}

/* a * b / c, or the largest uint32_t when that does not fit: the divide
   would fault on a quotient over 32 bits, and a, b and c can all come
   from the file or the player */
static uint32_t safe_muldiv( uint32_t a, uint32_t b, uint32_t c )
{
    if ( c == 0 || vb_umulhi( a, b ) >= c ) {
        return 0xFFFFFFFFUL;
    }
    return fx_umuldiv( a, b, c );
}

static int gran_none( uint32_t lo, uint32_t hi )
{
    return lo == 0xFFFFFFFFUL && hi == 0xFFFFFFFFUL;
}

/* a granule position less the link's base, as a signed count that
   saturates */
static int32_t gran_rel( const LINK *ln, uint32_t lo, uint32_t hi )
{
    uint32_t dlo = lo - ln->base_lo, dhi = hi - ln->base_hi - (lo < ln->base_lo ? 1 : 0);

    if ( dhi == 0 ) {
        return dlo > 0x7FFFFFFFUL ? NO_END : (int32_t)dlo;
    }
    if ( dhi == 0xFFFFFFFFUL ) {
        return dlo < 0x80000000UL ? -NO_END : (int32_t)dlo;
    }
    return (dhi & 0x80000000UL) ? -NO_END : NO_END;
}

static int in_group( const LINK *ln, uint32_t serial )
{
    int i;

    for ( i = 0; i < ln->ngroup; i++ ) {
        if ( ln->group[i] == serial ) {
            return 1;
        }
    }
    return 0;
}

/* the page in hand begins a Vorbis stream */
static int vorbis_bos( const OGG *og )
{
    const uint8_t *body = og->page + 27 + og->pnseg;

    return (og->pflags & F_BOS) && !(og->pflags & F_CONT) && og->pnseg > 0 && og->page[27] >= 30
           && body[0] == 1 && mem_cmp( body + 1, "vorbis", 6 ) == 0;
}

/* ------------------------------------------------------------------ */
/* packets                                                             */
/* ------------------------------------------------------------------ */
static int buf_room( OGG *og, unsigned int need )
{
    unsigned int size = og->buf_size, most = PKT_MAX + PAGE_MAX;
    uint8_t *grown;

    if ( og->buf_used + need <= og->buf_size ) {
        return 1;
    }
    while ( size < og->buf_used + need && size < most ) {
        size *= 2;
    }
    if ( size > most ) {
        size = most;
    }
    if ( size < og->buf_used + need ) {
        return 0;
    }
    grown = (uint8_t *)try_realloc( og->buf, size );
    if ( grown == NULL ) {
        return 0;
    }
    og->buf = grown;
    og->buf_size = size;
    return 1;
}

static void packets_reset( OGG *og )
{
    og->buf_used = og->part_at = 0;
    og->dropping = 0;
    og->nq = og->qi = 0;
    og->ntimed = og->nplaced = 0;
    og->gap = 0;
    og->q_end = NO_END;
    og->eos_seen = 0;
    og->have_seq = 0;
    og->out_left = 0;
}

/* The complete packets of the page in hand (of the stream playing)
   queued after those still waiting; the one it leaves unfinished kept
   for the next page */
static void page_take( OGG *og )
{
    const uint8_t *seg = og->page + 27, *body = og->page + 27 + og->pnseg;
    unsigned int i, k, lace, keep, done = og->qi;
    int skip = 0;

    /* the packets decoded go, and the rest move down */
    keep = done < og->nq ? og->q_at[done] : og->part_at;
    if ( keep > 0 ) {
        mem_cpy( og->buf, og->buf + keep, og->buf_used - keep );
        og->buf_used -= keep;
        og->part_at -= keep;
    }
    for ( k = done; k < og->nq; k++ ) {
        og->q_at[k - done] = og->q_at[k] - keep;
        og->q_len[k - done] = og->q_len[k];
        og->q_dur[k - done] = og->q_dur[k];
        og->q_pos[k - done] = og->q_pos[k];
        og->q_fresh[k - done] = og->q_fresh[k];
    }
    og->nq -= done;
    og->ntimed = og->ntimed > done ? og->ntimed - done : 0;
    og->nplaced = og->nplaced > done ? og->nplaced - done : 0;
    og->qi = 0;
    if ( og->have_seq && og->pseq != og->next_seq ) {
        og->buf_used = og->part_at;     /* pages were lost: so is the packet they held, */
        og->dropping = 0;
        og->gap = 1;                    /* and the next one does not follow the last */
    }
    og->have_seq = 1;
    og->next_seq = og->pseq + 1;
    if ( og->pflags & F_CONT ) {
        if ( og->buf_used == og->part_at && !og->dropping ) {
            skip = 1;                   /* the packet's start is not here */
        }
    } else {
        og->buf_used = og->part_at;     /* a packet that never finished */
        og->dropping = 0;
    }
    for ( i = 0; i < og->pnseg; i++ ) {
        lace = seg[i];
        if ( skip ) {
            if ( lace < 255 ) {
                skip = 0;
            }
            body += lace;
            continue;
        }
        if ( !og->dropping ) {
            if ( og->buf_used - og->part_at + lace > PKT_MAX || !buf_room( og, lace ) ) {
                og->dropping = 1;
                og->buf_used = og->part_at;
            } else {
                mem_cpy( og->buf + og->buf_used, body, lace );
                og->buf_used += lace;
            }
        }
        body += lace;
        if ( lace < 255 ) {
            if ( !og->dropping && og->buf_used > og->part_at && og->nq < QMAX ) {
                og->q_at[og->nq] = og->part_at;
                og->q_len[og->nq] = og->buf_used - og->part_at;
                og->q_fresh[og->nq] = (uint8_t)og->gap;
                og->gap = 0;
                og->nq++;
            }
            og->dropping = 0;
            og->part_at = og->buf_used;
        }
    }
}

/* Where each queued packet's samples belong, from the page's granule
   position: counted back from it, or on from the last page when that
   agrees.  The last page of the stream cuts its packets off there.
   Packets on a page with no granule position wait for one that has it
   (an early encoder wrote none on pages that do end packets), unless
   the time is already known or "final" says no such page will come. */
static void queue_times( OGG *og, int final )
{
    LINK *ln = &og->links[og->link];
    unsigned int k, bs, total = 0;
    int32_t g, start;
    int has_g = !final && !gran_none( og->glo, og->ghi );

    for ( k = og->ntimed; k < og->nq; k++ ) {
        bs = vb_block( og->vb, og->buf + og->q_at[k], og->q_len[k] );
        if ( og->q_fresh[k] ) {
            og->last_bs = 0;
        }
        og->q_dur[k] = og->last_bs != 0 && bs != 0 ? (og->last_bs + bs) / 4 : 0;
        if ( bs != 0 ) {
            og->last_bs = bs;
        }
    }
    og->ntimed = og->nq;
    if ( og->nplaced >= og->nq ) {
        return;
    }
    for ( k = og->nplaced; k < og->nq; k++ ) {
        total += og->q_dur[k];
    }
    if ( !has_g && !og->pos_known && !final && og->nq < QMAX ) {
        return;
    }
    if ( has_g && !og->base_set ) {
        /* the link's time starts where its first page's packets do,
           unless that is before zero */
        ln->base_lo = ln->base_hi = 0;
        if ( og->ghi != 0 || og->glo >= total ) {
            ln->base_lo = og->glo - total;
            ln->base_hi = og->ghi - (og->glo < total ? 1 : 0);
        }
        og->base_set = 1;
    }
    if ( has_g ) {
        g = gran_rel( ln, og->glo, og->ghi );
        if ( og->pos_known ) {
            start = og->pos;
            if ( !(og->pflags & F_EOS) && start + (int32_t)total != g ) {
                start = g - (int32_t)total;     /* a gap: the granule position is right */
            }
        } else if ( og->at_start && (og->pflags & F_EOS) ) {
            start = 0;                  /* the only page: it is the end that is cut */
        } else {
            start = g - (int32_t)total;
        }
        if ( og->pflags & F_EOS ) {
            og->q_end = g;
        }
    } else {
        start = og->pos_known ? og->pos : 0;
    }
    og->pos_known = 1;
    og->at_start = 0;
    for ( k = og->nplaced; k < og->nq; k++ ) {
        og->q_pos[k] = start;
        start += (int32_t)og->q_dur[k];
    }
    og->nplaced = og->nq;
    og->pos = start;
}

/* ------------------------------------------------------------------ */
/* links                                                               */
/* ------------------------------------------------------------------ */
/* The headers of a link, from the page in hand ("held") or the file
   position on: the streams that start together, the first Vorbis one
   among them and its three headers.  The stream is then ready to play
   from its first audio page. */
static int link_open( OGG *og, int held, DECINFO *info )
{
    LINK *ln = &og->links[og->link];
    VB *vb = NULL;
    int got = 0, rc, chosen = 0, first = og->rate == 0, plain = 0;
    const uint8_t *pkt;
    unsigned int len, down;

    vb_close( og->vb );
    og->vb = NULL;
    packets_reset( og );
    ln->ngroup = 0;
    for ( ;; ) {
        if ( !held ) {
            rc = page_next( og, 0xFFFFFFFFUL );
            if ( rc <= 0 ) {
                vb_close( vb );
                return rc == -1 ? DE_READ : DE_FORMAT;
            }
        }
        held = 0;
        if ( og->pflags & F_BOS ) {
            if ( plain ) {
                vb_close( vb );
                return DE_FORMAT;       /* another link began inside the headers */
            }
            if ( ln->ngroup == 0 ) {
                ln->offset = og->ppos;
            }
            if ( ln->ngroup < MAX_GROUP && !in_group( ln, og->pserial ) ) {
                ln->group[ln->ngroup++] = og->pserial;
            }
            if ( !chosen && vorbis_bos( og ) ) {
                chosen = 1;
                ln->serial = og->pserial;
            }
        } else if ( !chosen ) {
            if ( info != NULL ) {
                sfmt( info->format, sizeof( info->format ), "Ogg, with no Vorbis sound" );
            }
            return ln->ngroup > 0 ? DE_UNSUPPORTED : DE_FORMAT;
        } else {
            plain = 1;                  /* the streams' first pages are all seen */
        }
        if ( !chosen || og->pserial != ln->serial ) {
            continue;
        }
        page_take( og );
        while ( og->qi < og->nq && got < 3 ) {
            pkt = og->buf + og->q_at[og->qi];
            len = og->q_len[og->qi];
            og->qi++;
            if ( got == 0 ) {
                rc = vb_ident( &og->id, pkt, len );
                if ( rc == DE_OK && !first && og->id.rate != og->rate ) {
                    rc = DE_UNSUPPORTED;        /* a new rate: playback ends here */
                }
                if ( rc != DE_OK ) {
                    if ( info != NULL && rc == DE_UNSUPPORTED ) {
                        sfmt( info->format, sizeof( info->format ), "Ogg Vorbis, %u channels",
                              og->id.channels );
                    }
                    return rc;
                }
                if ( first ) {
                    down = og->ask.down > 2 ? 2 : og->ask.down;
                    while ( down > 0 && (og->id.rate >> down) < 8000 ) {
                        down--;
                    }
                    og->opt.down = (int)down;
                    og->opt.out_ch = og->id.channels == 1 || og->ask.mono ? 1 : 2;
                    og->opt.fast = og->ask.fast;
                    og->opt.eq = og->ask.eq;
                }
            } else if ( got == 1 ) {
                if ( len < 7 || pkt[0] != 3 || mem_cmp( pkt + 1, "vorbis", 6 ) != 0 ) {
                    return DE_FORMAT;
                }
                if ( info != NULL ) {
                    vb_comment( pkt, len, info );
                }
            } else {
                rc = vb_open( &vb, &og->id, pkt, len, &og->opt );
                if ( rc != DE_OK ) {
                    if ( info != NULL && rc == DE_UNSUPPORTED ) {
                        sfmt( info->format, sizeof( info->format ), "Ogg Vorbis, setup too big" );
                    }
                    return rc;
                }
            }
            got++;
        }
        if ( got == 3 ) {
            break;
        }
    }
    ln->rate = og->id.rate;
    if ( first ) {
        og->rate = og->id.rate;
    }
    og->vb = vb;
    og->last_bs = 0;
    og->pos_known = 0;
    og->at_start = 1;
    og->base_set = 0;
    og->keep_from = 0;
    og->pos = 0;
    og->ntimed = og->nplaced = og->qi;
    queue_times( og, 0 );       /* audio on the setup's own page, which should not be */
    return DE_OK;
}

/* the next page of the stream playing, its packets queued: 1, or 0 at
   the end, -2 for too much damage */
static int page_fetch( OGG *og )
{
    int rc;

    for ( ;; ) {
        rc = page_next( og, 0xFFFFFFFFUL );
        if ( rc <= 0 ) {
            return rc == -2 ? -2 : 0;
        }
        if ( (og->pflags & F_BOS) && !in_group( &og->links[og->link], og->pserial ) ) {
            /* the next link of a chain */
            if ( og->link + 1 < og->nlinks && og->links[og->link + 1].offset == og->ppos ) {
                og->link++;
            } else if ( og->link + 1 < MAX_LINKS && og->link + 1 >= og->nlinks ) {
                og->link++;
                mem_set( &og->links[og->link], 0, sizeof( LINK ) );
                og->nlinks = og->link + 1;
            }
            if ( link_open( og, 1, NULL ) != DE_OK ) {
                return 0;
            }
            if ( og->links[og->link].base_lo != 0 || og->links[og->link].base_hi != 0 ) {
                og->base_set = 1;
            }
            return 1;
        }
        if ( og->pserial != og->links[og->link].serial ) {
            if ( og->eos_seen ) {
                og->work += og->plen;   /* only other streams are left: that is work too */
            }
            continue;
        }
        if ( og->pflags & F_EOS ) {
            og->eos_seen = 1;
        }
        page_take( og );
        queue_times( og, 0 );
        return 1;
    }
}

/* the last page, of one stream or any, that starts between "low" and
   "end": found by reading back from the end a stretch at a time */
static int last_page( OGG *og, uint32_t low, uint32_t end, int which, uint32_t serial, uint32_t *glo, uint32_t *ghi,
                      uint32_t *spos )
{
    uint32_t hi = end, from, floor = end > TAIL_MAX ? end - TAIL_MAX : 0;
    int found;

    if ( floor < low ) {
        floor = low;
    }
    while ( hi > floor ) {
        from = hi - floor > STRETCH ? hi - STRETCH : floor;
        src_seek( og->src, from );
        found = 0;
        while ( page_next( og, hi ) == 1 ) {
            if ( (which == ANY_SERIAL || og->pserial == serial)
                 && !gran_none( og->glo, og->ghi ) ) {
                *glo = og->glo;
                *ghi = og->ghi;
                *spos = og->pserial;
                found = 1;
            }
        }
        if ( found ) {
            return 1;
        }
        hi = from;
    }
    return 0;
}

/* a page of stream "serial" starts within a few stretches after "at" */
static int serial_after( OGG *og, uint32_t at, uint32_t top, uint32_t serial )
{
    uint32_t end = top - at > 4 * STRETCH ? at + 4 * STRETCH : top;

    src_seek( og->src, at );
    while ( page_next( og, end ) == 1 ) {
        if ( og->pserial == serial && !gran_none( og->glo, og->ghi ) ) {
            return 1;
        }
    }
    return 0;
}

/* The last page of one stream between "lo" and "hi" when it ends well
   before hi because another stream of its link goes on: by halving,
   taking the stream to have ended where none of its pages follows
   within a few stretches. */
static int serial_last( OGG *og, uint32_t lo, uint32_t hi, uint32_t serial, uint32_t *glo, uint32_t *ghi )
{
    uint32_t mid, top = hi;
    int i, found = 0;

    for ( i = 0; i < 40 && hi - lo > STRETCH; i++ ) {
        mid = lo + (hi - lo) / 2;
        if ( serial_after( og, mid, top, serial ) ) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    src_seek( og->src, lo );
    while ( page_next( og, top - hi > 4 * STRETCH ? hi + 4 * STRETCH : top ) == 1 ) {
        if ( og->pserial == serial && !gran_none( og->glo, og->ghi ) ) {
            *glo = og->glo;
            *ghi = og->ghi;
            found = 1;
        }
    }
    return found;
}

/* the BOS pages at "at": a link's streams, its Vorbis serial and rate */
static int link_peek( OGG *og, uint32_t at, LINK *ln )
{
    const uint8_t *body;
    VBID id;

    mem_set( ln, 0, sizeof( LINK ) );
    ln->offset = at;
    src_seek( og->src, at );
    while ( page_next( og, 0xFFFFFFFFUL ) == 1 && (og->pflags & F_BOS) ) {
        if ( ln->ngroup < MAX_GROUP ) {
            ln->group[ln->ngroup++] = og->pserial;
        }
        if ( ln->rate == 0 && vorbis_bos( og ) ) {
            body = og->page + 27 + og->pnseg;
            if ( vb_ident( &id, body, og->page[27] ) == DE_OK ) {
                ln->serial = og->pserial;
                ln->rate = id.rate;
            }
        }
    }
    return ln->rate != 0;
}

/* where link "ln" ends: the first page after it that is not one of its
   streams', by halving and then page by page */
static uint32_t link_end( OGG *og, const LINK *ln )
{
    uint32_t lo = ln->offset, hi = og->size, mid;
    int i;

    for ( i = 0; i < 40 && hi - lo > STRETCH; i++ ) {
        mid = lo + (hi - lo) / 2;
        src_seek( og->src, mid );
        if ( page_next( og, hi ) != 1 ) {
            hi = mid;
        } else if ( in_group( ln, og->pserial ) ) {
            lo = og->ppos + og->plen;
        } else {
            hi = og->ppos;
        }
    }
    src_seek( og->src, lo );
    while ( page_next( og, hi ) == 1 ) {
        if ( !in_group( ln, og->pserial ) ) {
            return og->ppos;
        }
    }
    return hi;
}

/* the length of every link */
static void measure( OGG *og )
{
    LINK *ln;
    uint32_t glo, ghi, serial;
    int32_t rel;
    int k;

    og->nlinks = 1;
    ln = &og->links[0];
    ln->end = og->size;
    if ( !last_page( og, ln->offset, og->size, ANY_SERIAL, 0, &glo, &ghi, &serial ) ) {
        return;
    }
    if ( in_group( ln, serial ) ) {
        if ( serial != ln->serial
             && !last_page( og, ln->offset, og->size, ONE_SERIAL, ln->serial, &glo, &ghi,
                            &serial )
             && !serial_last( og, ln->offset, og->size, ln->serial, &glo, &ghi ) ) {
            return;
        }
        rel = gran_rel( ln, glo, ghi );
        ln->frames = rel > 0 && rel != NO_END ? (uint32_t)rel : 0;
        return;
    }
    for ( k = 0; k < MAX_LINKS; k++ ) {
        ln = &og->links[k];
        ln->end = link_end( og, ln );
        if ( last_page( og, ln->offset, ln->end, ONE_SERIAL, ln->serial, &glo, &ghi, &serial )
             || serial_last( og, ln->offset, ln->end, ln->serial, &glo, &ghi ) ) {
            rel = gran_rel( ln, glo, ghi );
            ln->frames = rel > 0 && rel != NO_END ? (uint32_t)rel : 0;
        }
        og->nlinks = k + 1;
        if ( ln->end >= og->size || k + 1 >= MAX_LINKS
             || !link_peek( og, ln->end, &og->links[k + 1] )
             || og->links[k + 1].rate != og->rate ) {
            break;
        }
        og->links[k + 1].start = ln->start + ln->frames;
    }
}

static uint32_t total_frames( const OGG *og )
{
    const LINK *ln = &og->links[og->nlinks - 1];

    return ln->start + ln->frames;
}

/* ------------------------------------------------------------------ */
/* the codec                                                           */
/* ------------------------------------------------------------------ */
static int ogg_detect( SRC *src, const uint8_t *head, unsigned int len, const char *ext )
{
    int at;

    (void)src;
    if ( len >= 4 && mem_cmp( head, "OggS", 4 ) == 0 ) {
        return mem_find( head, len, "vorbis" ) >= 0 ? 100 : 40;
    }
    at = mem_find( head, len, "OggS" );
    if ( at >= 0 && mem_find( head + at, len - (unsigned int)at, "vorbis" ) >= 0 ) {
        return 60;                      /* something stuck on before the first page */
    }
    return str_icmp( ext, "OGG" ) == 0 || str_icmp( ext, "OGA" ) == 0 ? 5 : 0;
}

static void ogg_close( void *state )
{
    OGG *og = (OGG *)state;

    if ( og != NULL ) {
        vb_close( og->vb );
        xfree( og->page );
        xfree( og->buf );
        xfree( og );
    }
}

static int ogg_open( SRC *src, const DECOPT *opt, DECINFO *info, void **state )
{
    OGG *og;
    int rc;
    uint32_t resume, frames, ms;
    const char *chans;

    *state = NULL;
    og = (OGG *)try_alloc( sizeof( OGG ) );
    if ( og == NULL ) {
        return DE_NOMEM;
    }
    mem_set( og, 0, sizeof( OGG ) );
    og->src = src;
    og->size = src->size;
    og->ask = *opt;
    crc_init( og );
    og->page = (uint8_t *)try_alloc( PAGE_MAX );
    og->buf = (uint8_t *)try_alloc( BUF_START );
    og->buf_size = BUF_START;
    if ( og->page == NULL || og->buf == NULL ) {
        ogg_close( og );
        return DE_NOMEM;
    }
    og->work_max = WORK_SEEK;
    src_seek( src, 0 );
    og->nlinks = 1;
    rc = link_open( og, 0, info );
    if ( rc != DE_OK ) {
        ogg_close( og );
        return rc;
    }
    /* the first audio page fixes where the link's time starts */
    while ( !og->base_set ) {
        rc = page_fetch( og );
        if ( rc <= 0 || og->link != 0 ) {
            break;
        }
    }
    resume = src_tell( src );
    measure( og );
    src_seek( src, resume );
    og->work = 0;

    frames = total_frames( og );
    ms = frames ? safe_muldiv( frames, 1000, og->rate ) : 0;
    info->file_rate = og->rate;
    info->rate = og->rate >> og->opt.down;
    info->file_channels = (uint8_t)og->id.channels;
    info->channels = (uint8_t)og->opt.out_ch;
    info->bits = 0;
    info->length_ms = ms;
    info->flags = DIF_EQ | (ms ? DIF_SEEK : 0);
    if ( !(og->id.br_min == og->id.br_max && og->id.br_nom == og->id.br_max
           && og->id.br_nom > 0) ) {
        info->flags |= DIF_VBR;
    }
    if ( ms > 0 ) {
        info->kbps = safe_muldiv( og->size, 8, ms );
    } else {
        info->kbps = og->id.br_nom > 0 ? (uint32_t)og->id.br_nom / 1000 : 0;
    }
    chans = og->id.channels == 1 ? "mono" : og->id.channels == 2 ? "stereo" : NULL;
    if ( chans != NULL ) {
        sfmt( info->format, sizeof( info->format ), "Ogg Vorbis, %s%s", chans,
              og->nlinks > 1 ? ", chained" : "" );
    } else {
        sfmt( info->format, sizeof( info->format ), "Ogg Vorbis, %u channels%s",
              og->id.channels, og->nlinks > 1 ? ", chained" : "" );
    }
    *state = og;
    return DE_OK;
}

/* The next packet decoded and its samples made ready, as many as fall
   between the start (or a seek's target) and the stream's end.
   -> 1, 0 at the end, -2 for too much damage */
static int packet_next( OGG *og )
{
    unsigned int k, frames, skip, stop;
    int32_t from, to, start;
    const int16_t *pcm;
    int rc, down = og->opt.down;

    if ( og->qi >= og->nplaced ) {
        rc = page_fetch( og );
        if ( rc == 0 && og->nplaced < og->nq ) {
            queue_times( og, 1 );       /* the end came before a granule position */
            return 1;
        }
        return rc;
    }
    k = og->qi++;
    if ( og->q_fresh[k] ) {
        vb_restart( og->vb );       /* a clean start rather than a block lapped onto a stranger */
    }
    frames = vb_decode( og->vb, og->buf + og->q_at[k], og->q_len[k], &pcm );
    start = og->q_pos[k];
    from = start > og->keep_from ? start : og->keep_from;
    to = start + (int32_t)og->q_dur[k];
    if ( to > og->q_end ) {
        to = og->q_end;
    }
    if ( to <= from || frames == 0 ) {
        return 1;
    }
    skip = (unsigned int)(from - start) >> down;
    stop = (unsigned int)(to - start) >> down;
    if ( stop > frames ) {
        stop = frames;
    }
    if ( skip < stop ) {
        og->out = pcm + skip * (unsigned int)og->opt.out_ch;
        og->out_left = stop - skip;
    }
    return 1;
}

static int ogg_decode( void *state, DECBUF *buf )
{
    OGG *og = (OGG *)state;
    unsigned int take, ch = (unsigned int)og->opt.out_ch;
    int16_t *dst = buf->pcm;
    int rc;

    buf->frames = 0;
    buf->ev_count = 0;
    og->work = 0;
    og->work_max = WORK_DECODE;
    while ( buf->frames < buf->max ) {
        if ( og->out_left > 0 ) {
            take = buf->max - buf->frames;
            if ( take > og->out_left ) {
                take = og->out_left;
            }
            mem_cpy( dst, og->out, take * ch * sizeof( int16_t ) );
            dst += take * ch;
            og->out += take * ch;
            og->out_left -= take;
            buf->frames += take;
            continue;
        }
        if ( og->ended || og->vb == NULL ) {
            break;
        }
        rc = packet_next( og );
        if ( rc == 0 ) {
            og->ended = 1;
            break;
        }
        if ( rc == -2 && og->eos_seen ) {
            og->ended = 1;              /* what is left belongs to other streams */
            break;
        }
        if ( rc == -2 ) {
            if ( buf->frames == 0 ) {
                mem_set( dst, 0, SILENCE * ch * sizeof( int16_t ) );
                buf->frames = SILENCE;
            }
            break;
        }
    }
    if ( og->src->error && buf->frames == 0 ) {
        return DE_READ;
    }
    return DE_OK;
}

/* the page to start reading from for a target "t" samples into link
   "ln": the last page of its stream whose granule position is at or
   before t */
static uint32_t find_page( OGG *og, const LINK *ln, int32_t t )
{
    uint32_t lo = ln->offset, hi = ln->end, mid, best = ln->offset;
    int32_t g;
    int i, rc;

    if ( t <= 0 ) {
        return best;
    }
    for ( i = 0; i < 40 && hi - lo > STRETCH; i++ ) {
        mid = lo + (hi - lo) / 2;
        src_seek( og->src, mid );
        while ( (rc = page_next( og, hi )) == 1
                && (og->pserial != ln->serial || gran_none( og->glo, og->ghi )) ) {
        }
        if ( rc != 1 ) {
            hi = mid;
            continue;
        }
        g = gran_rel( ln, og->glo, og->ghi );
        if ( g <= t ) {
            best = og->ppos;
            lo = og->ppos + og->plen;
        } else {
            hi = mid;
        }
    }
    src_seek( og->src, lo );
    while ( page_next( og, hi ) == 1 ) {
        if ( og->pserial != ln->serial || gran_none( og->glo, og->ghi ) ) {
            continue;
        }
        if ( gran_rel( ln, og->glo, og->ghi ) > t ) {
            break;
        }
        best = og->ppos;
    }
    return best;
}

static int ogg_seek( void *state, uint32_t ms, uint32_t *at_ms )
{
    OGG *og = (OGG *)state;
    uint32_t target, total = total_frames( og ), page;
    int k;
    LINK *ln;

    *at_ms = 0;
    if ( total == 0 || og->rate == 0 ) {
        return DE_FORMAT;
    }
    target = safe_muldiv( ms, og->rate, 1000 );
    if ( target > total ) {
        target = total;
    }
    for ( k = og->nlinks - 1; k > 0 && og->links[k].start > target; k-- ) {
    }
    og->work = 0;
    og->work_max = WORK_SEEK;
    if ( k != og->link || og->vb == NULL ) {
        og->link = k;
        src_seek( og->src, og->links[k].offset );
        if ( link_open( og, 0, NULL ) != DE_OK ) {
            og->ended = 1;
            return DE_FORMAT;
        }
    }
    ln = &og->links[k];
    page = find_page( og, ln, (int32_t)(target - ln->start) - PREROLL );
    src_seek( og->src, page );
    packets_reset( og );
    vb_restart( og->vb );
    og->last_bs = 0;
    og->pos_known = 0;
    og->at_start = page == ln->offset;
    og->base_set = 1;
    og->keep_from = (int32_t)(target - ln->start);
    og->ended = 0;
    og->work = 0;
    *at_ms = safe_muldiv( target, 1000, og->rate );
    return DE_OK;
}

const CODEC codec_ogg = {
    "Ogg Vorbis",
    "OGG OGA",
    ogg_detect,
    ogg_open,
    ogg_decode,
    ogg_seek,
    ogg_close
};
