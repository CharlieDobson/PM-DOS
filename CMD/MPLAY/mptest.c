/*
 * MPTEST.C - the engine with no screen: MPTEST FILE [/B] [/H] [/Q] [/M]
 * plays a file through the sound driver and prints where it is, for
 * testing the engine and the drivers on PM-DOS.  Not shipped.
 *
 *   /B   best quality      /H  half rate      /Q  quarter rate
 *   /M   one channel       /S:n  seek to n seconds first
 *   /T:n stop after n seconds
 *   /EQ  a bass and treble boost
 *   /X   no sound: decode as fast as it goes and say how fast (/F the
 *        codec's fast path; /H /Q /M as above; /T:n the first n seconds)
 *
 * Esc stops it.  At the end it prints the card, the format, the load
 * and the underruns.
 */
#include "rt.h"
#include "sys.h"
#include "dec.h"
#include "player.h"
#include "fix.h"

int mem_reclaim( void )
{
    return 0;
}

void out_of_memory( void )
{
    sys_stdout( "MPTEST: out of memory\r\n", 23 );
    sys_exit( 3 );
}

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

void mp_main( void )
{
    char line[200], path[PATH_LEN];
    const char *cmd;
    const PLCARD *card;
    DECINFO info;
    PLSTAT st;
    PLSET set;
    EQSET eq;
    u32 last = 0xFFFFFFFFUL, seek_s = 0, stop_s = 0;
    int speed = 0, fast = 0;
    int rc, shift, use_eq = 0, len = 0;

    sys_init();
    cmd = sys_cmdline();
    set = *pl_get_settings();
    path[0] = 0;
    while ( *cmd ) {
        while ( *cmd == ' ' ) {
            cmd++;
        }
        if ( *cmd == '/' ) {
            cmd++;
            switch ( ch_upper( *cmd ) ) {
            case 'B': set.quality = PQ_BEST; break;
            case 'H': set.quality = PQ_HALF; break;
            case 'Q': set.quality = PQ_QUARTER; break;
            case 'M': set.mono = 1; break;
            case 'S': seek_s = dec_parse( cmd + 2, &rc ); break;
            case 'T': stop_s = dec_parse( cmd + 2, &rc ); break;
            case 'E': use_eq = 1; break;
            case 'F': fast = 1; break;
            case 'X': speed = 1; break;
            }
            while ( *cmd && *cmd != ' ' ) {
                cmd++;
            }
        } else if ( *cmd ) {
            len = 0;
            while ( *cmd && *cmd != ' ' && len < PATH_LEN - 1 ) {
                path[len++] = *cmd++;
            }
            path[len] = 0;
        }
    }
    if ( str_icmp( path, "PCI" ) == 0 ) {
        u32 regs[6], index, cls;

        mem_set( regs, 0, sizeof( regs ) );
        rc = sys_bus( 0, regs );
        sfmt( line, sizeof( line ), "BUS_PCIHERE: ok %d, EAX %X, last bus %u, functions %u\r\n", rc, regs[0],
              regs[1], regs[2] );
        say( line );
        for ( index = 0; index < 64; index++ ) {
            mem_set( regs, 0, sizeof( regs ) );
            regs[4] = index;
            if ( !sys_bus( 1, regs ) ) {
                break;
            }
            cls = regs[2];
            sfmt( line, sizeof( line ), "%08X  %04X:%04X  class %06X rev %02X\r\n", regs[1],
                  regs[0] & 0xFFFF, regs[0] >> 16, cls >> 8, cls & 0xFF );
            say( line );
            {
                u32 bar[6];

                mem_set( bar, 0, sizeof( bar ) );
                bar[1] = regs[1];
                if ( sys_bus( 6, bar ) ) {
                    sfmt( line, sizeof( line ), "          BAR0 %08X flags %X size %X\r\n", bar[0], bar[3], bar[2] );
                } else {
                    sfmt( line, sizeof( line ), "          BAR0 refused\r\n" );
                }
                say( line );
                mem_set( bar, 0, sizeof( bar ) );
                bar[2] = 0x1274;
                bar[3] = 0x1371;
                if ( index == 0 && sys_bus( 3, bar ) ) {
                    sfmt( line, sizeof( line ), "1274:1371 found by BUS_PCIID at %08X\r\n", bar[1] );
                    say( line );
                }
            }
            mem_set( regs + 1, 0, 5 * sizeof( u32 ) );
        }
        sys_exit( 0 );
    }
    if ( path[0] == 0 ) {
        say( "MPTEST FILE [/B] [/H] [/Q] [/M] [/S:sec] [/T:sec] [/EQ]\r\n" );
        sys_exit( 2 );
    }
    if ( !pl_init() ) {
        say( "MPTEST: no memory\r\n" );
        sys_exit( 3 );
    }
    pl_settings( &set );
    card = pl_card();
    sfmt( line, sizeof( line ), "card: %s %s; processor: %s, class %d (%u a ms)\r\n", card->name, card->line,
          pl_cpu_name(), pl_cpu_class(), pl_cpu_speed() );
    say( line );
    if ( use_eq ) {
        mem_set( &eq, 0, sizeof( eq ) );
        eq.on = 1;
        eq.band[0] = 16;
        eq.band[1] = 12;
        eq.band[8] = 12;
        eq.band[9] = 16;
        eq.preamp = -8;
        pl_set_eq( &eq );
    }
    pl_set_volume( 100, 0 );
    if ( speed ) {
        /* /X: decode the whole file as fast as it goes, nothing played */
        SRC src;
        const CODEC *codec;
        void *state;
        DECOPT opt;
        DECBUF buf;
        s16 *pcm = (s16 *)try_alloc( 4608UL * 2 * sizeof( s16 ) );
        u32 frames = 0, t0, t1, cs, audio_cs;

        mem_set( &opt, 0, sizeof( opt ) );
        opt.down = (u8)(set.quality == PQ_HALF ? 1 : set.quality == PQ_QUARTER ? 2 : 0);
        opt.mono = (u8)set.mono;
        opt.fast = (u8)fast;
        opt.rate = 22050;
        opt.synth = SYN_SOFT;
        if ( use_eq ) {
            mem_set( &eq, 0, sizeof( eq ) );
            eq.on = 1;
            eq.band[0] = 16;
            eq.band[9] = 16;
            eq.serial = 1;
            opt.eq = &eq;
        }
        if ( pcm == NULL || dec_open( &src, path, &opt, &codec, &state, &info ) != DE_OK ) {
            say( "MPTEST: cannot open it\r\n" );
            sys_exit( 1 );
        }
        sfmt( line, sizeof( line ), "%s: %u Hz %u ch out (file %u Hz %u ch), down %u mono %u fast %u eq %u\r\n",
              info.format, info.rate, info.channels, info.file_rate, info.file_channels, (u32)opt.down,
              (u32)opt.mono, (u32)opt.fast, (u32)use_eq );
        say( line );
        t0 = sys_hundredths();
        for ( ;; ) {
            mem_set( &buf, 0, sizeof( buf ) );
            buf.pcm = pcm;
            buf.max = 4608;
            if ( codec->decode( state, &buf ) != DE_OK || buf.frames == 0 ) {
                break;
            }
            frames += buf.frames;
            if ( stop_s && frames >= stop_s * info.rate ) {
                break;
            }
        }
        t1 = sys_hundredths();
        cs = t1 >= t0 ? t1 - t0 : t1 + 8640000UL - t0;
        audio_cs = info.rate ? fx_umuldiv( frames, 100, info.rate ) : 0;
        sfmt( line, sizeof( line ), "decoded %u frames = %u.%02u s of sound in %u.%02u s: %u.%02u times real time\r\n",
              frames, audio_cs / 100, audio_cs % 100, cs / 100, cs % 100,
              cs ? audio_cs / cs : 0, cs ? (audio_cs % cs) * 100 / cs : 0 );
        say( line );
        codec->close( state );
        src_close( &src );
        xfree( pcm );
        sfmt( line, sizeof( line ), "heap held at exit: %u\r\n", heap_in_use() );
        say( line );
        sys_exit( 0 );
    }
    rc = pl_open( path, &info );
    if ( rc != DE_OK ) {
        sfmt( line, sizeof( line ), "MPTEST: cannot play %s (%d) %s\r\n", path, rc, info.format );
        say( line );
        pl_done();
        sys_exit( 1 );
    }
    sfmt( line, sizeof( line ), "file: %s, %u Hz, %u ch, %u ms, %u kbps\r\n  \"%s\" - \"%s\"\r\n",
          info.format, info.rate, info.channels, info.length_ms, info.kbps, info.title, info.artist );
    say( line );
    if ( seek_s ) {
        pl_seek( seek_s * 1000 );
    }
    pl_play();
    for ( ;; ) {
        rc = pl_pump( 40 );
        pl_status( &st );
        if ( st.pos_ms / 1000 != last ) {
            last = st.pos_ms / 1000;
            sfmt( line, sizeof( line ), "%u:%02u  %s  load %d%%  underruns %u\r\n", last / 60, last % 60,
                  st.quality, st.load, st.underruns );
            say( line );
        }
        if ( rc == PS_ENDED || rc == PS_EMPTY ) {
            break;
        }
        if ( stop_s && st.pos_ms >= stop_s * 1000 ) {
            break;
        }
        if ( sys_key_wait( 0, &shift, KW_NOWAIT ) == 1 && (sys_key_read( &shift ) & 0xFF) == 27 ) {
            break;
        }
    }
    pl_status( &st );
    sfmt( line, sizeof( line ), "end at %u ms: %s, load %d%%, underruns %u, interrupts %u\r\n", st.pos_ms,
          st.quality, st.load, st.underruns, st.irqs );
    say( line );
    pl_done();
    sfmt( line, sizeof( line ), "heap held at exit: %u\r\n", heap_in_use() );
    say( line );
    sys_exit( 0 );
}
