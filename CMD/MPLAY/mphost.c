/*
 * MPHOST.C - MPLAY's decoders as a Windows console program, for
 * testing a codec without booting anything.
 *
 *   MPHOST info FILE [options]          what the codec found
 *   MPHOST wav FILE OUT.WAV [options]   decode to a 16-bit wave file
 *   MPHOST time FILE [options]          decode, write nothing, and say
 *                                       how long it took
 *   MPHOST ev FILE OUT.TXT [options]    MIDI: the events, one a line
 *   MPHOST cmp A.WAV B.WAV [/LAG:n] [/SKIP:n] [/LEN:n]
 *                                       how close two wave files are
 *
 * Options for a decode:
 *   /D1 /D2        the rate halved, quartered     /M   one channel
 *   /F             the codec's faster path        /SEEK:ms  start there
 *   /MS:ms         stop after this much           /RATE:n   synth rate
 *   /SYN:SOFT|OPL2|OPL3|MPU                       MIDI's synthesizer
 *   /EQ:a,b,...    ten band values and an optional preamp, in half
 *                  decibels (-24..24): /EQ:12,12,6,0,0,0,0,0,6,12
 *   /EQ:BASS /EQ:TREBLE /EQ:V      a boost of the lows, of the highs,
 *                  or a V (both)
 *   /PIECE:n       ask for n frames a call (default 4096)
 *
 * CMP lines the two files up first: B is slid up to /LAG frames
 * (default 2000) either way against A, and the offset with the
 * smallest difference over the first seconds is used.  /SKIP leaves
 * out frames at the start of A (a decoder's delay), /LEN compares only
 * that many.  It prints the offset, the largest difference, and the
 * signal-to-noise ratio in decibels - A is the signal, A - B the
 * noise.  Over 60 dB a 16-bit decoder is as good as exact; under 20
 * something is wrong.
 *
 * This file is the only one in MPLAY that uses the C library and
 * floating point: nothing here ships.  MKHOST.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include "rt.h"
#include "sys.h"
#include "dec.h"

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    fprintf( stderr, "out of memory\n" );
    exit( 3 );
}

typedef struct {
    DECOPT opt;
    EQSET  eq;
    int    use_eq;
    u32    seek_ms;
    u32    max_ms;
    u32    piece;
    u32    lag, skip, len;
} OPTS;

static int parse_eq( const char *val, EQSET *eq )
{
    int i, n = 0, neg, num;
    const char *at = val;

    mem_set( eq, 0, sizeof( EQSET ) );
    eq->on = 1;
    eq->serial = 1;
    if ( stricmp( val, "BASS" ) == 0 ) {
        eq->band[0] = 20; eq->band[1] = 18; eq->band[2] = 12; eq->band[3] = 6;
        return 1;
    }
    if ( stricmp( val, "TREBLE" ) == 0 ) {
        eq->band[6] = 6; eq->band[7] = 12; eq->band[8] = 18; eq->band[9] = 20;
        return 1;
    }
    if ( stricmp( val, "V" ) == 0 ) {
        eq->band[0] = 18; eq->band[1] = 14; eq->band[2] = 6; eq->band[3] = -4; eq->band[4] = -10;
        eq->band[5] = -10; eq->band[6] = -4; eq->band[7] = 6; eq->band[8] = 14; eq->band[9] = 18;
        eq->preamp = -8;
        return 1;
    }
    while ( *at && n < EQ_BANDS + 1 ) {
        neg = 0;
        if ( *at == '-' ) {
            neg = 1;
            at++;
        }
        num = 0;
        while ( *at >= '0' && *at <= '9' ) {
            num = num * 10 + (*at - '0');
            at++;
        }
        if ( num > 24 ) {
            num = 24;
        }
        if ( n < EQ_BANDS ) {
            eq->band[n] = (s8)(neg ? -num : num);
        } else {
            eq->preamp = (s8)(neg ? -num : num);
        }
        n++;
        if ( *at == ',' ) {
            at++;
        } else {
            break;
        }
    }
    for ( i = n; i < EQ_BANDS; i++ ) {
        eq->band[i] = 0;
    }
    return n > 0;
}

static int parse_opts( int argc, char **argv, int first, OPTS *opts )
{
    int i;
    const char *arg;

    memset( opts, 0, sizeof( OPTS ) );
    opts->piece = 4096;
    opts->lag = 2000;
    opts->opt.rate = 44100;
    opts->opt.synth = SYN_SOFT;
    for ( i = first; i < argc; i++ ) {
        arg = argv[i];
        if ( arg[0] != '/' && arg[0] != '-' ) {
            fprintf( stderr, "what is %s?\n", arg );
            return 0;
        }
        arg++;
        if ( stricmp( arg, "D1" ) == 0 ) {
            opts->opt.down = 1;
        } else if ( stricmp( arg, "D2" ) == 0 ) {
            opts->opt.down = 2;
        } else if ( stricmp( arg, "M" ) == 0 ) {
            opts->opt.mono = 1;
        } else if ( stricmp( arg, "F" ) == 0 ) {
            opts->opt.fast = 1;
        } else if ( strnicmp( arg, "SEEK:", 5 ) == 0 ) {
            opts->seek_ms = (u32)strtoul( arg + 5, NULL, 10 );
        } else if ( strnicmp( arg, "MS:", 3 ) == 0 ) {
            opts->max_ms = (u32)strtoul( arg + 3, NULL, 10 );
        } else if ( strnicmp( arg, "RATE:", 5 ) == 0 ) {
            opts->opt.rate = (u32)strtoul( arg + 5, NULL, 10 );
        } else if ( strnicmp( arg, "PIECE:", 6 ) == 0 ) {
            opts->piece = (u32)strtoul( arg + 6, NULL, 10 );
            if ( opts->piece < 1152 ) {
                opts->piece = 1152;
            }
        } else if ( strnicmp( arg, "LAG:", 4 ) == 0 ) {
            opts->lag = (u32)strtoul( arg + 4, NULL, 10 );
        } else if ( strnicmp( arg, "SKIP:", 5 ) == 0 ) {
            opts->skip = (u32)strtoul( arg + 5, NULL, 10 );
        } else if ( strnicmp( arg, "LEN:", 4 ) == 0 ) {
            opts->len = (u32)strtoul( arg + 4, NULL, 10 );
        } else if ( strnicmp( arg, "SYN:", 4 ) == 0 ) {
            if ( stricmp( arg + 4, "OPL2" ) == 0 ) {
                opts->opt.synth = SYN_OPL2;
            } else if ( stricmp( arg + 4, "OPL3" ) == 0 ) {
                opts->opt.synth = SYN_OPL3;
            } else if ( stricmp( arg + 4, "MPU" ) == 0 ) {
                opts->opt.synth = SYN_MPU;
            } else {
                opts->opt.synth = SYN_SOFT;
            }
        } else if ( strnicmp( arg, "EQ:", 3 ) == 0 ) {
            if ( !parse_eq( arg + 3, &opts->eq ) ) {
                fprintf( stderr, "bad /EQ\n" );
                return 0;
            }
            opts->use_eq = 1;
        } else {
            fprintf( stderr, "unknown option /%s\n", arg );
            return 0;
        }
    }
    opts->opt.eq = opts->use_eq ? &opts->eq : NULL;
    return 1;
}

static const char *de_name( int rc )
{
    switch ( rc ) {
    case DE_OK:          return "OK";
    case DE_FORMAT:      return "not a file any codec knows, or damaged";
    case DE_UNSUPPORTED: return "a kind of file the codec does not play";
    case DE_NOMEM:       return "out of memory";
    case DE_READ:        return "the file could not be read";
    }
    return "?";
}

static void print_info( const CODEC *codec, const DECINFO *info )
{
    printf( "codec:    %s\n", codec->name );
    printf( "format:   %s\n", info->format );
    printf( "rate:     %lu (file %lu)\n", (unsigned long)info->rate, (unsigned long)info->file_rate );
    printf( "channels: %u (file %u)\n", info->channels, info->file_channels );
    printf( "bits:     %u\n", info->bits );
    printf( "kbps:     %lu\n", (unsigned long)info->kbps );
    printf( "length:   %lu ms\n", (unsigned long)info->length_ms );
    printf( "flags:    %s%s%s%s%s\n", info->flags & DIF_SEEK ? "SEEK " : "",
            info->flags & DIF_VBR ? "VBR " : "", info->flags & DIF_EQ ? "EQ " : "",
            info->flags & DIF_EVENTS ? "EVENTS " : "", info->flags & DIF_LEVELS ? "LEVELS " : "" );
    printf( "title:    %s\n", info->title );
    printf( "artist:   %s\n", info->artist );
    printf( "album:    %s\n", info->album );
}

static void put32( FILE *out, u32 val )
{
    fputc( (int)(val & 0xFF), out );
    fputc( (int)((val >> 8) & 0xFF), out );
    fputc( (int)((val >> 16) & 0xFF), out );
    fputc( (int)((val >> 24) & 0xFF), out );
}

static void put16( FILE *out, u32 val )
{
    fputc( (int)(val & 0xFF), out );
    fputc( (int)((val >> 8) & 0xFF), out );
}

static void wav_header( FILE *out, u32 rate, u32 channels, u32 frames )
{
    u32 bytes = frames * channels * 2;

    fwrite( "RIFF", 1, 4, out );
    put32( out, 36 + bytes );
    fwrite( "WAVEfmt ", 1, 8, out );
    put32( out, 16 );
    put16( out, 1 );
    put16( out, channels );
    put32( out, rate );
    put32( out, rate * channels * 2 );
    put16( out, channels * 2 );
    put16( out, 16 );
    fwrite( "data", 1, 4, out );
    put32( out, bytes );
}

/* info, wav, time, ev */
static int run_decode( const char *mode, const char *path, const char *out_path, OPTS *opts )
{
    SRC src;
    const CODEC *codec;
    void *state;
    DECINFO info;
    DECBUF buf;
    FILE *out = NULL;
    u32 total = 0, events = 0, limit = 0, at_ms, held_before;
    int rc, pieces = 0;
    clock_t start, stop;
    s16 *pcm;
    DECEV *ev;
    u32 i, ev_room = 4096;

    held_before = heap_in_use();
    rc = dec_open( &src, path, &opts->opt, &codec, &state, &info );
    if ( rc != DE_OK ) {
        printf( "open: %s", de_name( rc ) );
        if ( rc == DE_UNSUPPORTED ) {
            printf( " (%s)", info.format );
        }
        printf( "\n" );
        return 2;
    }
    if ( strcmp( mode, "info" ) == 0 ) {
        print_info( codec, &info );
        codec->close( state );
        src_close( &src );
        return 0;
    }
    if ( opts->seek_ms ) {
        rc = codec->seek( state, opts->seek_ms, &at_ms );
        printf( "seek to %lu: %s, at %lu ms\n", (unsigned long)opts->seek_ms, de_name( rc ),
                (unsigned long)at_ms );
    }
    if ( opts->max_ms ) {
        limit = (u32)((double)opts->max_ms * info.rate / 1000.0);
    }
    pcm = (s16 *)malloc( opts->piece * 2 * sizeof( s16 ) );
    ev = (DECEV *)malloc( ev_room * sizeof( DECEV ) );
    if ( pcm == NULL || ev == NULL ) {
        printf( "no memory\n" );
        return 3;
    }
    if ( out_path != NULL ) {
        out = fopen( out_path, strcmp( mode, "ev" ) == 0 ? "w" : "wb" );
        if ( out == NULL ) {
            printf( "cannot write %s\n", out_path );
            return 2;
        }
        if ( strcmp( mode, "wav" ) == 0 ) {
            wav_header( out, info.rate, info.channels, 0 );
        }
    }
    start = clock();
    for ( ;; ) {
        memset( &buf, 0, sizeof( buf ) );
        buf.pcm = pcm;
        buf.max = opts->piece;
        buf.ev = ev;
        buf.ev_max = ev_room;
        rc = codec->decode( state, &buf );
        if ( rc != DE_OK ) {
            printf( "decode: %s after %lu frames\n", de_name( rc ), (unsigned long)total );
            break;
        }
        if ( buf.frames > buf.max ) {
            printf( "DEFECT: the codec gave %lu frames for a piece of %lu\n",
                    (unsigned long)buf.frames, (unsigned long)buf.max );
            break;
        }
        if ( buf.ev_count > buf.ev_max ) {
            printf( "DEFECT: %lu events for room for %lu\n", (unsigned long)buf.ev_count,
                    (unsigned long)buf.ev_max );
            break;
        }
        if ( buf.frames == 0 && buf.ev_count == 0 ) {
            break;
        }
        if ( out != NULL && strcmp( mode, "wav" ) == 0 ) {
            fwrite( pcm, sizeof( s16 ) * info.channels, buf.frames, out );
        }
        if ( out != NULL && strcmp( mode, "ev" ) == 0 ) {
            for ( i = 0; i < buf.ev_count; i++ ) {
                fprintf( out, "%10lu %s %u %02X %02X\n", (unsigned long)(total + ev[i].frame),
                         ev[i].kind == EV_OPL ? "OPL" : ev[i].kind == EV_MPU ? "MPU" : "???",
                         ev[i].bank, ev[i].reg, ev[i].val );
            }
        }
        events += buf.ev_count;
        total += buf.frames;
        pieces++;
        if ( limit && total >= limit ) {
            break;
        }
        if ( buf.frames == 0 && ++pieces > 1000000 ) {
            printf( "DEFECT: events and no frames, for ever\n" );
            break;
        }
    }
    stop = clock();
    if ( out != NULL ) {
        if ( strcmp( mode, "wav" ) == 0 ) {
            fseek( out, 0, SEEK_SET );
            wav_header( out, info.rate, info.channels, total );
        }
        fclose( out );
    }
    codec->close( state );
    src_close( &src );
    free( pcm );
    free( ev );
    {
        double audio = info.rate ? (double)total / info.rate : 0.0;
        double cpu = (double)(stop - start) / CLOCKS_PER_SEC;
        printf( "%lu frames = %.3f s of sound at %lu Hz, %u channel(s); %lu events\n",
                (unsigned long)total, audio, (unsigned long)info.rate, info.channels,
                (unsigned long)events );
        printf( "decoded in %.3f s: %.1f times real time on this PC\n", cpu,
                cpu > 0.0 ? audio / cpu : 0.0 );
    }
    if ( heap_in_use() != held_before ) {
        printf( "LEAK: %lu bytes still held after close\n",
                (unsigned long)(heap_in_use() - held_before) );
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* comparing two wave files                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    u32  rate, channels, frames;
    s16 *data;
} WAVE;

static int load_wave( const char *path, WAVE *wave )
{
    FILE *in = fopen( path, "rb" );
    unsigned char hdr[8], fmt[16];
    long size;
    int got_fmt = 0;

    memset( wave, 0, sizeof( WAVE ) );
    if ( in == NULL ) {
        printf( "cannot read %s\n", path );
        return 0;
    }
    if ( fread( hdr, 1, 4, in ) != 4 || memcmp( hdr, "RIFF", 4 ) != 0 ) {
        printf( "%s: not RIFF\n", path );
        fclose( in );
        return 0;
    }
    fseek( in, 12, SEEK_SET );
    while ( fread( hdr, 1, 8, in ) == 8 ) {
        size = (long)LE32( hdr + 4 );
        if ( memcmp( hdr, "fmt ", 4 ) == 0 && size >= 16 ) {
            fread( fmt, 1, 16, in );
            fseek( in, size - 16 + (size & 1), SEEK_CUR );
            if ( LE16( fmt ) != 1 || LE16( fmt + 14 ) != 16 ) {
                printf( "%s: only 16-bit PCM is compared\n", path );
                fclose( in );
                return 0;
            }
            wave->channels = LE16( fmt + 2 );
            wave->rate = LE32( fmt + 4 );
            got_fmt = 1;
        } else if ( memcmp( hdr, "data", 4 ) == 0 && got_fmt ) {
            wave->frames = (u32)size / (2 * wave->channels);
            wave->data = (s16 *)malloc( wave->frames * wave->channels * 2 + 2 );
            if ( wave->data == NULL ) {
                fclose( in );
                return 0;
            }
            wave->frames = (u32)fread( wave->data, 2 * wave->channels, wave->frames, in );
            fclose( in );
            return 1;
        } else {
            fseek( in, size + (size & 1), SEEK_CUR );
        }
    }
    printf( "%s: no data\n", path );
    fclose( in );
    return 0;
}

static int run_cmp( const char *path_a, const char *path_b, OPTS *opts )
{
    WAVE wa, wb;
    long lag, best_lag = 0, ia, ib;
    double err, best = -1.0, sig = 0.0, noise = 0.0, diff;
    u32 ch, frames, n, window;
    long maxdiff = 0;

    if ( !load_wave( path_a, &wa ) || !load_wave( path_b, &wb ) ) {
        return 2;
    }
    if ( wa.channels != wb.channels ) {
        printf( "channels differ: %lu and %lu\n", (unsigned long)wa.channels,
                (unsigned long)wb.channels );
        return 1;
    }
    if ( wa.rate != wb.rate ) {
        printf( "rates differ: %lu and %lu\n", (unsigned long)wa.rate, (unsigned long)wb.rate );
    }
    /* the offset: B[i + lag] against A[i], over a window of up to ten
       seconds from the skip point */
    window = wa.rate * 10;
    for ( lag = -(long)opts->lag; lag <= (long)opts->lag; lag++ ) {
        err = 0.0;
        for ( n = 0; n < window; n += 7 ) {
            ia = (long)(opts->skip + n);
            ib = ia + lag;
            if ( ia >= (long)wa.frames || ib < 0 || ib >= (long)wb.frames ) {
                continue;
            }
            for ( ch = 0; ch < wa.channels; ch++ ) {
                diff = (double)wa.data[ia * wa.channels + ch] - wb.data[ib * wa.channels + ch];
                err += diff * diff;
            }
        }
        if ( best < 0.0 || err < best ) {
            best = err;
            best_lag = lag;
        }
    }
    frames = 0;
    for ( n = opts->skip; n < wa.frames; n++ ) {
        ib = (long)n + best_lag;
        if ( ib < 0 ) {
            continue;
        }
        if ( ib >= (long)wb.frames || (opts->len && n - opts->skip >= opts->len) ) {
            break;
        }
        for ( ch = 0; ch < wa.channels; ch++ ) {
            double va = wa.data[n * wa.channels + ch];
            diff = va - wb.data[ib * wa.channels + ch];
            sig += va * va;
            noise += diff * diff;
            if ( labs( (long)diff ) > maxdiff ) {
                maxdiff = labs( (long)diff );
            }
        }
        frames++;
    }
    printf( "A: %lu frames  B: %lu frames  compared %lu from A[%lu] with B offset %ld\n",
            (unsigned long)wa.frames, (unsigned long)wb.frames, (unsigned long)frames,
            (unsigned long)opts->skip, best_lag );
    if ( frames == 0 ) {
        printf( "nothing to compare\n" );
        return 1;
    }
    printf( "largest difference %ld, RMS difference %.2f, ", maxdiff,
            sqrt( noise / ((double)frames * wa.channels) ) );
    if ( noise == 0.0 ) {
        printf( "identical\n" );
    } else {
        printf( "SNR %.1f dB\n", 10.0 * log10( (sig > 0.0 ? sig : 1.0) / noise ) );
    }
    /* the same with B scaled by the gain that fits best: a level
       difference (a mixer's volume) is then told from a real one */
    {
        double ab = 0.0, bb = 0.0, gain, fit = 0.0, va, vb;

        for ( n = opts->skip; n < wa.frames; n++ ) {
            ib = (long)n + best_lag;
            if ( ib < 0 ) {
                continue;
            }
            if ( ib >= (long)wb.frames || (opts->len && n - opts->skip >= opts->len) ) {
                break;
            }
            for ( ch = 0; ch < wa.channels; ch++ ) {
                va = wa.data[n * wa.channels + ch];
                vb = wb.data[ib * wa.channels + ch];
                ab += va * vb;
                bb += vb * vb;
            }
        }
        gain = bb > 0.0 ? ab / bb : 1.0;
        for ( n = opts->skip; n < wa.frames; n++ ) {
            ib = (long)n + best_lag;
            if ( ib < 0 ) {
                continue;
            }
            if ( ib >= (long)wb.frames || (opts->len && n - opts->skip >= opts->len) ) {
                break;
            }
            for ( ch = 0; ch < wa.channels; ch++ ) {
                diff = wa.data[n * wa.channels + ch] - gain * wb.data[ib * wa.channels + ch];
                fit += diff * diff;
            }
        }
        printf( "B times %.4f (%.2f dB) fits best: SNR %.1f dB\n", gain, 20.0 * log10( gain > 0.0 ? gain : 1e-9 ),
                fit > 0.0 ? 10.0 * log10( (sig > 0.0 ? sig : 1.0) / fit ) : 999.0 );
    }
    free( wa.data );
    free( wb.data );
    return 0;
}

int main( int argc, char **argv )
{
    OPTS opts;

    if ( argc >= 3 && (strcmp( argv[1], "info" ) == 0 || strcmp( argv[1], "time" ) == 0) ) {
        if ( !parse_opts( argc, argv, 3, &opts ) ) {
            return 2;
        }
        return run_decode( argv[1], argv[2], NULL, &opts );
    }
    if ( argc >= 4 && (strcmp( argv[1], "wav" ) == 0 || strcmp( argv[1], "ev" ) == 0) ) {
        if ( !parse_opts( argc, argv, 4, &opts ) ) {
            return 2;
        }
        return run_decode( argv[1], argv[2], argv[3], &opts );
    }
    if ( argc >= 4 && strcmp( argv[1], "cmp" ) == 0 ) {
        if ( !parse_opts( argc, argv, 4, &opts ) ) {
            return 2;
        }
        return run_cmp( argv[2], argv[3], &opts );
    }
    printf( "MPHOST info|time FILE [options]\n"
            "MPHOST wav FILE OUT.WAV [options]\n"
            "MPHOST ev FILE OUT.TXT [options]\n"
            "MPHOST cmp A.WAV B.WAV [/LAG:n] [/SKIP:n] [/LEN:n]\n"
            "options: /D1 /D2 /M /F /SEEK:ms /MS:ms /RATE:n /PIECE:n /SYN:SOFT|OPL2|OPL3|MPU\n"
            "         /EQ:b1,...,b10[,pre] (half dB) /EQ:BASS /EQ:TREBLE /EQ:V\n" );
    return 2;
}
