/*
 * SND.C - SOUND, PLAY and BEEP: the PC speaker.
 *
 * NOTES GO INTO A QUEUE and are played by the clock: snd_service() is
 * called between statements (VM.C's look round), while a program waits
 * for a key, and continuously while a statement waits for the music to
 * finish.  MF, the default, makes PLAY and SOUND wait until the queue is
 * empty; MB lets the program go on with up to 32 notes waiting, which
 * is what ON PLAY counts.
 *
 * The speaker is channel 2 of the 8254 and bits 0 and 1 of port 61h;
 * a native program's port instructions are emulated by the kernel, and
 * a note costs five of them.
 *
 * PLAY'S LANGUAGE: A-G (# or + sharp, - flat, then a length and dots),
 * O octave (0-6, 4 to start with; O3 starts at middle C), < >, L length
 * (1-64), T tempo (32-255 quarter notes a minute), P pause, N note
 * (0-84, 0 a rest), ML MN MS (the share of a note that sounds: all, 7/8,
 * 3/4), MF MB, and X with a VARPTR$ for a string to play from.
 */
#include "qb.h"

#define QMAX        32
#define TICK_MS     54.9254

typedef struct {
    u16 freq;                   /* 0: silence */
    u32 on_ms, off_ms;
} NOTE;

static NOTE queue[QMAX + 1];
static int  q_head, q_count;
static int  sounding;           /* a note (or its gap) is under way */
static int  in_gap;
static u32  phase_end;
static u32  gap_left;
static int  background;         /* MB */
static int  octave = 4, length = 4, tempo = 120, style = 7;     /* style in eighths */

static u32 now_ms( void )
{
    return sys_hundredths() * 10UL;
}

static void speaker( int freq )
{
    u32 divisor;
    int bits;

    if ( freq <= 0 ) {
        port_out( 0x61, port_in( 0x61 ) & 0xFC );
        return;
    }
    divisor = 1193182UL / (u32)freq;
    if ( divisor > 0xFFFF ) {
        divisor = 0xFFFF;
    }
    port_out( 0x43, 0xB6 );
    port_out( 0x42, divisor & 0xFF );
    port_out( 0x42, divisor >> 8 );
    bits = (int)port_in( 0x61 );
    port_out( 0x61, (u32)(bits | 3) );
}

int snd_queued( void )
{
    return q_count + (sounding && !in_gap ? 1 : 0);
}

/* the music, moved on to where the clock says it should be */
void snd_service( void )
{
    u32 now;
    NOTE *note;

    if ( !sounding && q_count == 0 ) {
        return;
    }
    now = now_ms();
    if ( sounding && now < phase_end && phase_end - now > 3600000UL ) {
        phase_end = now;                        /* midnight */
    }
    for ( ;; ) {
        if ( sounding ) {
            if ( now < phase_end ) {
                return;
            }
            if ( !in_gap && gap_left ) {
                speaker( 0 );
                in_gap = 1;
                phase_end += gap_left;
                gap_left = 0;
                continue;
            }
            sounding = 0;
        }
        if ( q_count == 0 ) {
            speaker( 0 );
            return;
        }
        note = &queue[q_head];
        q_head = (q_head + 1) % (QMAX + 1);
        q_count--;
        speaker( note->freq );
        sounding = 1;
        in_gap = 0;
        gap_left = note->off_ms;
        if ( phase_end < now || phase_end - now > 1000 ) {
            phase_end = now;
        }
        phase_end += note->on_ms;
    }
}

void snd_stop( void )
{
    q_count = 0;
    sounding = 0;
    speaker( 0 );
    background = 0;
    octave = 4;
    length = 4;
    tempo = 120;
    style = 7;
}

/* wait for room in the queue, or (want_empty) for it to be done */
static void snd_wait( int want_empty )
{
    con_flush();
    for ( ;; ) {
        snd_service();
        if ( want_empty ? (!sounding && q_count == 0) : q_count < QMAX ) {
            return;
        }
        con_poll_keys();
    }
}

static void enqueue( int freq, u32 on_ms, u32 off_ms )
{
    NOTE *note;

    if ( q_count >= QMAX ) {
        snd_wait( 0 );
    }
    note = &queue[(q_head + q_count) % (QMAX + 1)];
    note->freq = (u16)freq;
    note->on_ms = on_ms;
    note->off_ms = off_ms;
    q_count++;
    snd_service();
}

void snd_beep( void )
{
    enqueue( 800, 250, 0 );
    snd_wait( 1 );
}

void rtf_BEEP( void )
{
    snd_beep();
}

/* SOUND frequency, duration in clock ticks */
void rtf_SOUND( void )
{
    double duration = POP_D(), freq = POP_D();

    if ( duration < 0 || duration > 65535.0 || freq < 37 || freq > 32767 ) {
        if ( !(duration == 0 && freq >= 0 && freq <= 32767) ) {
            vm_error( E_ILLEGAL_CALL );
        }
    }
    if ( duration == 0 ) {
        /* a duration of 0 ends what is sounding */
        q_count = 0;
        sounding = 0;
        speaker( 0 );
        return;
    }
    enqueue( (int)freq, (u32)(duration * TICK_MS), 0 );
    if ( !background ) {
        snd_wait( 1 );
    }
}

/* the frequency of note n, 1 (C of octave 0) to 84 */
static int note_freq( int n )
{
    return (int)fp_floor( 440.0 * fp_pow( 2.0, (n - 46) / 12.0 ) + 0.5 );
}

/* a note's or a pause's length in ms: 1 = a whole note */
static u32 length_ms( int len, int dots )
{
    double ms = 240000.0 / tempo / len, extra = ms;

    while ( dots-- > 0 ) {
        extra /= 2;
        ms += extra;
    }
    return (u32)ms;
}

static int read_num( MACRO *cur, int *out )
{
    double v;

    if ( !macro_number( cur, &v, 0 ) ) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int read_dots( MACRO *cur )
{
    int dots = 0;

    while ( cur->pos < cur->len && cur->text[cur->pos] == '.' ) {
        dots++;
        cur->pos++;
    }
    return dots;
}

static void play_note( int n, int len, int dots )
{
    u32 total = length_ms( len, dots ), on;

    if ( n == 0 ) {
        enqueue( 0, total, 0 );
        return;
    }
    on = total * (u32)style / 8;
    enqueue( note_freq( n ), on, total - on );
}

static void play_string( const char *text, u32 len, int depth )
{
    static const int semis[7] = { 9, 11, 0, 2, 4, 5, 7 };      /* A B C D E F G */
    MACRO cur;
    int ch, n, v, dots;

    if ( depth > 8 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    cur.text = text;
    cur.len = len;
    cur.pos = 0;
    while ( cur.pos < cur.len ) {
        ch = ch_upper( cur.text[cur.pos++] );
        if ( ch == ' ' || ch == ';' || ch == '\t' ) {
            continue;
        }
        if ( ch >= 'A' && ch <= 'G' ) {
            n = octave * 12 + semis[ch - 'A'] + 1;
            if ( cur.pos < cur.len && (cur.text[cur.pos] == '#' || cur.text[cur.pos] == '+') ) {
                n++;
                cur.pos++;
            } else if ( cur.pos < cur.len && cur.text[cur.pos] == '-' ) {
                n--;
                cur.pos++;
            }
            v = length;
            if ( read_num( &cur, &v ) && (v < 1 || v > 64) ) {
                vm_error( E_ILLEGAL_CALL );
            }
            dots = read_dots( &cur );
            if ( n < 1 || n > 84 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            play_note( n, v, dots );
            continue;
        }
        switch ( ch ) {
        case 'O':
            if ( !read_num( &cur, &v ) || v < 0 || v > 6 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            octave = v;
            break;
        case '<':
            if ( octave > 0 ) {
                octave--;
            }
            break;
        case '>':
            if ( octave < 6 ) {
                octave++;
            }
            break;
        case 'L':
            if ( !read_num( &cur, &v ) || v < 1 || v > 64 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            length = v;
            break;
        case 'T':
            if ( !read_num( &cur, &v ) || v < 32 || v > 255 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            tempo = v;
            break;
        case 'P':
            if ( !read_num( &cur, &v ) || v < 1 || v > 64 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            dots = read_dots( &cur );
            enqueue( 0, length_ms( v, dots ), 0 );
            break;
        case 'N':
            if ( !read_num( &cur, &v ) || v < 0 || v > 84 ) {
                vm_error( E_ILLEGAL_CALL );
            }
            dots = read_dots( &cur );
            play_note( v, length, dots );
            break;
        case 'M':
            if ( cur.pos >= cur.len ) {
                vm_error( E_ILLEGAL_CALL );
            }
            switch ( ch_upper( cur.text[cur.pos++] ) ) {
            case 'L':   style = 8; break;
            case 'N':   style = 7; break;
            case 'S':   style = 6; break;
            case 'F':   background = 0; break;
            case 'B':   background = 1; break;
            default:    vm_error( E_ILLEGAL_CALL );
            }
            break;
        case 'X': {
            QTYPE type;
            void *addr;
            STR **slot, *sub;
            if ( cur.pos + 3 > cur.len ) {
                vm_error( E_ILLEGAL_CALL );
            }
            slot = mem_varptr_string( (const u8 *)cur.text + cur.pos, &type, &addr );
            cur.pos += 3;
            if ( slot == NULL ) {
                vm_error( E_ILLEGAL_CALL );
            }
            sub = qs_retain( *slot );
            PUSH_STR( sub );                    /* held on the stack: an error in it frees it */
            play_string( QS_TEXT( sub ), QS_LEN( sub ), depth + 1 );
            qs_release( POP_STR() );
            break;
        }
        default:
            vm_error( E_ILLEGAL_CALL );
        }
    }
}

void rtf_PLAY( void )
{
    STR *s = vm.sp[-1].str;              /* left on the stack: an error frees it */

    play_string( QS_TEXT( s ), QS_LEN( s ), 0 );
    qs_release( POP_STR() );
    if ( !background ) {
        snd_wait( 1 );
    }
}

void rtf_PLAYF( void )
{
    (void)POP_L();
    PUSH_L( background ? snd_queued() : 0 );
}
