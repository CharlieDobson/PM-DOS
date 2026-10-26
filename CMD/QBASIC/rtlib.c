/*
 * RTLIB.C - the runtime routines that are not strings, the screen,
 * graphics, sound or files: numbers, RND, TIMER, DATA, arrays, errors,
 * events, the date and the time.
 *
 * RND IS QBASIC'S OWN GENERATOR: a 24-bit linear congruence (multiply
 * by 16,598,013, add 12,820,163) started at 327,680 for every run, and
 * RANDOMIZE mixes the top half of the seed's DOUBLE into its upper two
 * bytes.  A program that never calls RANDOMIZE gets the numbers it got
 * under MS-DOS, which some of them depend on.
 */
#include "qb.h"

/* ------------------------------------------------------------------ */
/* numbers                                                             */
/* ------------------------------------------------------------------ */

static void check_d( double v )
{
    if ( DBL_BAD( v ) ) {
        vm_error( E_OVERFLOW );
    }
}

static float to_single( double v )
{
    volatile float f = (float)v;

    if ( (*(volatile u32 *)&f & 0x7F800000UL) == 0x7F800000UL ) {
        vm_error( E_OVERFLOW );
    }
    return f;
}

void rtf_ABSL( void )
{
    s32 v = POP_L();

    if ( v == (s32)0x80000000UL ) {
        vm_error( E_OVERFLOW );
    }
    PUSH_L( v < 0 ? -v : v );
}

void rtf_ABSS( void )
{
    double v = POP_S();

    PUSH_S( v < 0 ? -v : v );
}

void rtf_ABSD( void )
{
    double v = POP_D();

    PUSH_D( v < 0 ? -v : v );
}

void rtf_SGNL( void )
{
    s32 v = POP_L();

    PUSH_L( v < 0 ? -1 : v > 0 ? 1 : 0 );
}

void rtf_SGNS( void )
{
    double v = POP_S();

    PUSH_L( v < 0 ? -1 : v > 0 ? 1 : 0 );
}

void rtf_SGND( void )
{
    double v = POP_D();

    PUSH_L( v < 0 ? -1 : v > 0 ? 1 : 0 );
}

void rtf_INTS( void )
{
    PUSH_S( fp_floor( POP_S() ) );
}

void rtf_INTD( void )
{
    PUSH_D( fp_floor( POP_D() ) );
}

void rtf_FIXS( void )
{
    PUSH_S( fp_trunc( POP_S() ) );
}

void rtf_FIXD( void )
{
    PUSH_D( fp_trunc( POP_D() ) );
}

static double do_sqr( double v )
{
    if ( v < 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    return fp_sqrt( v );
}

static double do_log( double v )
{
    if ( v <= 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    return fp_log( v );
}

static double do_exp( double v )
{
    double r;

    if ( v > 709.78 ) {
        vm_error( E_OVERFLOW );
    }
    r = fp_exp( v );
    check_d( r );
    return r;
}

/* SIN, COS and TAN refuse an argument of 2^32 or more */
static double trig_arg( double v )
{
    if ( v >= 4294967296.0 || v <= -4294967296.0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    return v;
}

static double do_tan( double v )
{
    double r = fp_tan( trig_arg( v ) );

    check_d( r );
    return r;
}

/* a SINGLE's answer is pushed as it came: it is rounded when stored */
void rtf_SQRS( void ) { PUSH_S( do_sqr( POP_S() ) ); }
void rtf_SQRD( void ) { PUSH_D( do_sqr( POP_D() ) ); }
void rtf_SINS( void ) { PUSH_S( fp_sin( trig_arg( POP_S() ) ) ); }
void rtf_SIND( void ) { PUSH_D( fp_sin( trig_arg( POP_D() ) ) ); }
void rtf_COSS( void ) { PUSH_S( fp_cos( trig_arg( POP_S() ) ) ); }
void rtf_COSD( void ) { PUSH_D( fp_cos( trig_arg( POP_D() ) ) ); }
void rtf_ATNS( void ) { PUSH_S( fp_atn( POP_S() ) ); }
void rtf_ATND( void ) { PUSH_D( fp_atn( POP_D() ) ); }
void rtf_LOGS( void ) { PUSH_S( do_log( POP_S() ) ); }
void rtf_LOGD( void ) { PUSH_D( do_log( POP_D() ) ); }
void rtf_TANS( void ) { PUSH_S( do_tan( POP_S() ) ); }
void rtf_TAND( void ) { PUSH_D( do_tan( POP_D() ) ); }

void rtf_EXPS( void )
{
    PUSH_S( do_exp( POP_S() ) );
}

void rtf_EXPD( void )
{
    PUSH_D( do_exp( POP_D() ) );
}

/* ------------------------------------------------------------------ */
/* RND, RANDOMIZE, TIMER                                               */
/* ------------------------------------------------------------------ */

static u32 rnd_seed;

void rnd_reset( void )
{
    rnd_seed = 327680UL;
}

static float rnd_next( void )
{
    rnd_seed = (rnd_seed * 16598013UL + 12820163UL) & 0xFFFFFFUL;
    return (float)((double)rnd_seed / 16777216.0);
}

void rtf_RND0( void )
{
    PUSH_S( rnd_next() );
}

void rtf_RND1( void )
{
    float n = POP_S();
    u32 bits;

    if ( n == 0 ) {
        PUSH_S( (float)((double)rnd_seed / 16777216.0) );
        return;
    }
    if ( n < 0 ) {
        bits = *(u32 *)&n;
        rnd_seed = (bits & 0xFFFFFFUL) + ((bits >> 24) & 0xFFUL);
    }
    PUSH_S( rnd_next() );
}

void randomize( double seed )
{
    u32 high = ((u32 *)&seed)[1];

    high ^= high >> 16;
    high &= 0xFFFFUL;
    rnd_seed = (rnd_seed & 0xFFUL) | (high << 8);
}

void rtf_RANDOMIZE( void )
{
    randomize( POP_D() );
}

void rtf_TIMER( void )
{
    PUSH_S( num_sng( (double)sys_hundredths() / 100.0 ) );
}

/* ------------------------------------------------------------------ */
/* DATA                                                                */
/* ------------------------------------------------------------------ */

/* the next DATA item; READ moves past it only once it has been taken
   without an error, so a READ that overflows reads the same item again */
static DATAITEM *next_item( void )
{
    PROGRAM *prog = vm.prog;

    if ( vm.data_next >= prog->ndata ) {
        vm_error( E_OUT_OF_DATA );
    }
    return &prog->data[vm.data_next];
}

/* an item as a number: blank is 0; anything else must be one */
static double item_number( DATAITEM *item, int *type )
{
    double val;
    u32 i;

    for ( i = 0; i < item->len && (item->text[i] == ' ' || item->text[i] == '\t'); i++ ) {
    }
    if ( i == item->len && !item->quoted ) {
        *type = TY_INT;
        return 0;
    }
    if ( item->quoted || !num_scan( item->text, item->len, &val, type ) ) {
        vm_error_data( E_SYNTAX, item );   /* the READ's error, shown at the DATA */
    }
    return val;
}

static s32 item_whole( int to_int )
{
    int type;
    double v = fp_round( item_number( next_item(), &type ) );

    if ( to_int ? (v < -32768.0 || v > 32767.0) : (v < -2147483648.0 || v > 2147483647.0) ) {
        vm_error( E_OVERFLOW );
    }
    return (s32)v;
}

void rtf_READI( void )
{
    s16 *var = (s16 *)POP_A();

    *var = (s16)item_whole( 1 );
    vm.data_next++;
}

void rtf_READL( void )
{
    s32 *var = (s32 *)POP_A();

    *var = item_whole( 0 );
    vm.data_next++;
}

void rtf_READS( void )
{
    float *var = (float *)POP_A();
    int type;

    *var = to_single( item_number( next_item(), &type ) );
    vm.data_next++;
}

void rtf_READD( void )
{
    double *var = (double *)POP_A();
    int type;

    *var = item_number( next_item(), &type );
    vm.data_next++;
}

void rtf_READSTR( void )
{
    STR **var = (STR **)POP_A();
    DATAITEM *item = next_item();
    STR *old = *var;

    *var = qs_make( item->text, item->len );
    qs_release( old );
    vm.data_next++;
}

void rtf_READFS( void )
{
    u32 width = (u32)POP_L();
    char *field = (char *)POP_A();
    DATAITEM *item = next_item();
    u32 n = item->len < width ? item->len : width;

    mem_cpy( field, item->text, n );
    mem_set( field + n, ' ', width - n );
    vm.data_next++;
}

void rtf_RESTORE( void )
{
    vm.data_next = (u32)POP_L();
}

/* ------------------------------------------------------------------ */
/* arrays                                                              */
/* ------------------------------------------------------------------ */

u32 type_bytes( QTYPE type )
{
    switch ( TKIND( type ) ) {
    case TY_INT:    return 2;
    case TY_LONG:   return 4;
    case TY_SINGLE: return 4;
    case TY_DOUBLE: return 8;
    case TY_FSTR:   return TARG( type );
    case TY_REC:    return vm.prog->recs[TARG( type )].size;
    }
    return 4;
}

/* An array with these bounds (lo, hi pairs): NULL if the bounds are bad
   or there is no memory - never an error, because vm_begin makes arrays
   before there is anywhere for an error to go. */
ARR *arr_make( QTYPE type, u32 esize, int ndims, const s32 *bounds, int flags )
{
    ARR *arr;
    double total = 1;
    u32 count = 1, bytes;
    int d;

    if ( esize == 0 ) {
        esize = type_bytes( type );
    }
    for ( d = 0; d < ndims; d++ ) {
        if ( bounds[d * 2 + 1] < bounds[d * 2] ) {
            return NULL;
        }
        total *= (double)bounds[d * 2 + 1] - bounds[d * 2] + 1;
    }
    if ( total * esize > 256.0 * 1024 * 1024 ) {
        return NULL;
    }
    count = (u32)total;
    bytes = count * esize;
    arr = (ARR *)try_alloc( sizeof( ARR ) + (u32)(2 * ndims) * sizeof( s32 ) );
    if ( arr == NULL ) {
        return NULL;
    }
    arr->data = (u8 *)try_alloc( bytes ? bytes : 4 );
    if ( arr->data == NULL ) {
        xfree( arr );
        return NULL;
    }
    mem_set( arr->data, 0, bytes );
    arr->type = type;
    arr->esize = esize;
    arr->count = count;
    arr->ndims = (u8)ndims;
    arr->flags = (u8)flags;
    arr->vseg = 0;
    for ( d = 0; d < ndims; d++ ) {
        arr->lb[d] = bounds[d * 2];
        arr->lb[ndims + d] = bounds[d * 2 + 1] - bounds[d * 2] + 1;
    }
    return arr;
}

static void arr_clear( ARR *arr )
{
    u32 i;

    if ( TKIND( arr->type ) == TY_STR ) {
        for ( i = 0; i < arr->count; i++ ) {
            qs_release( ((STR **)arr->data)[i] );
        }
    }
    mem_set( arr->data, 0, arr->count * arr->esize );
}

void arr_free( ARR *arr )
{
    if ( arr == NULL ) {
        return;
    }
    if ( TKIND( arr->type ) == TY_STR ) {
        file_forget_vars( arr->data, arr->data + arr->count * arr->esize );
    }
    mem_forget( arr->data, arr->data + arr->count * arr->esize );
    arr_clear( arr );
    xfree( arr->data );
    xfree( arr );
}

/* Arrays REDIM or ERASE let go of while an element's address was still
   waiting on the stack (a(i) = f(x), where f does the REDIM): kept, empty,
   until the run ends, so that the store lands in memory that is still
   theirs.  QBasic stores into the new array; the value is lost here. */
static ARR **parked;
static u32   nparked, parked_cap;

static int arr_in_use( ARR *arr )
{
    u8 *lo = arr->data, *hi = arr->data + arr->count * arr->esize;
    CELL *cell;
    u32 k;
    int i;

    for ( cell = vm.stack; cell < vm.sp; cell++ ) {
        if ( (u8 *)cell->ptr >= lo && (u8 *)cell->ptr < hi ) {
            return 1;
        }
    }
    for ( k = 0; k < vm.nframes; k++ ) {
        void **params = (void **)vm.frames[k].mine;
        for ( i = 0; i < vm.frames[k].proc->nparams; i++ ) {
            if ( (u8 *)params[i] >= lo && (u8 *)params[i] < hi ) {
                return 1;
            }
        }
    }
    return 0;
}

static void arr_let_go( ARR *arr )
{
    if ( !arr_in_use( arr ) ) {
        arr_free( arr );
        return;
    }
    if ( nparked == parked_cap ) {
        u32 cap = parked_cap ? parked_cap * 2 : 8;
        ARR **grown = (ARR **)try_realloc( parked, cap * sizeof( ARR * ) );
        if ( grown == NULL ) {
            vm_error( E_OUT_OF_MEMORY );
        }
        parked = grown;
        parked_cap = cap;
    }
    arr_clear( arr );
    parked[nparked++] = arr;
}

/* the run is over, or CLEAR left nothing pending: no address is waiting */
void arr_unpark( void )
{
    while ( nparked > 0 ) {
        arr_free( parked[--nparked] );
    }
}

/* DIM: slot, lo/hi pairs, type, width, flags (1 static, 2 REDIM), count */
void rtf_DIM( void )
{
    int ndims = (int)POP_L(), flags = (int)POP_L(), d;
    u32 esize = (u32)POP_L();
    QTYPE type = (QTYPE)POP_L();
    s32 bounds[MAX_DIMS * 2];
    ARR **slot;

    if ( ndims < 1 || ndims > MAX_DIMS ) {
        vm_error( E_ILLEGAL_CALL );             /* the compiler allows no more */
    }
    vm.sp -= ndims * 2;
    for ( d = 0; d < ndims * 2; d++ ) {
        bounds[d] = vm.sp[d].l;
    }
    slot = (ARR **)POP_A();
    for ( d = 0; d < ndims; d++ ) {
        if ( bounds[d * 2 + 1] < bounds[d * 2] ) {
            vm_error( E_SUBSCRIPT );
        }
    }
    if ( *slot != NULL ) {
        if ( flags & 2 ) {
            arr_let_go( *slot );
            *slot = NULL;
        } else if ( flags & 1 ) {
            return;                             /* a static: made once */
        } else {
            vm_error( E_DUPLICATE );            /* "Array already dimensioned" */
        }
    }
    *slot = arr_make( type, esize, ndims, bounds, (flags & 1) ? AF_STATIC : 0 );
    if ( *slot == NULL ) {
        vm_error( E_OUT_OF_MEMORY );
    }
}

void rtf_ERASE( void )
{
    ARR **slot = (ARR **)POP_A();

    if ( *slot == NULL ) {
        return;
    }
    if ( (*slot)->flags & AF_STATIC ) {
        arr_clear( *slot );
        return;
    }
    arr_let_go( *slot );
    *slot = NULL;
}

static ARR *bound_args( int *dim )
{
    ARR *arr;

    *dim = (int)POP_L();
    arr = *(ARR **)POP_A();
    if ( arr == NULL || *dim < 0 || *dim > arr->ndims ) {
        vm_error( E_SUBSCRIPT );
    }
    return arr;
}

/* dimension 0 is not an error in QBasic (it reads what lies before the
   bounds); here it is 0 */

void rtf_LBOUND( void )
{
    int dim;
    ARR *arr = bound_args( &dim );

    PUSH_L( dim == 0 ? 0 : arr->lb[dim - 1] );
}

void rtf_UBOUND( void )
{
    int dim;
    ARR *arr = bound_args( &dim );

    PUSH_L( dim == 0 ? 0 : ARR_UB( arr, dim - 1 ) );
}

/* ------------------------------------------------------------------ */
/* errors and the rest of the machine's state                          */
/* ------------------------------------------------------------------ */

void rtf_ERROR( void )
{
    s32 code = POP_L();

    if ( code < 1 || code > 255 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    vm_error( (int)code );
}

void rtf_ERR( void )
{
    PUSH_L( vm.err );
}

void rtf_ERL( void )
{
    PUSH_L( vm.erl );
}

/* TRON, TROFF: the IDE lights each statement as it runs, as Debug >
   Trace On does; a full-speed run stops for it to take over */
void rtf_TRON( void )
{
    int on = (int)POP_L();

    if ( ide_trace && on && !vm.tron && vm.go == GO_RUN && !vm.immediate ) {
        vm.stopped = RUN_STEP;
    }
    vm.tron = on;
}

/* FRE(-1): the most memory there is; FRE(-2): stack to spare; anything
   else: string space, which QBasic counts in a 64K segment */
/* The string space as QBasic has it: 64K at most, less the text the
   program holds.  Strings here can go past that; FRE only reports. */
static s32 string_space( void )
{
    u32 avail = mem_avail(), used = qs_bytes();

    if ( avail > 65535UL ) {
        avail = 65535UL;
    }
    return used >= avail ? 0 : (s32)(avail - used);
}

void rtf_FRE( void )
{
    s32 which = POP_L();
    u32 avail;

    if ( which == -2 ) {
        PUSH_L( (s32)((vm.stack_end - vm.sp) * sizeof( CELL )) );
        return;
    }
    if ( which == -1 ) {
        avail = mem_avail();
        PUSH_L( avail > 0x7FFFFFFFUL ? 0x7FFFFFFFL : (s32)avail );
        return;
    }
    PUSH_L( string_space() );
}

void rtf_FRESTR( void )
{
    qs_release( POP_STR() );
    PUSH_L( string_space() );
}

/* ------------------------------------------------------------------ */
/* events                                                              */
/* ------------------------------------------------------------------ */

#define EV_TIMER    1
#define EV_KEY      2
#define EV_PLAY     3
#define EV_COM      4
#define EV_PEN      5
#define EV_STRIG    6
#define NKEYS       32

#define ES_OFF      0
#define ES_ON       1
#define ES_STOP     2

typedef struct {
    u8 *handler;
    u8  state;
    u8  pending;                /* happened while STOPped */
    u8  busy;                   /* its handler is running */
} EVENT;

static EVENT ev_timer, ev_play, ev_keys[NKEYS];

/* KEY(n) is on (or stopped): its key is the trap's, not INKEY$'s */
int event_key_armed( int n )
{
    return n > 0 && n < NKEYS && ev_keys[n].handler != NULL && ev_keys[n].state != ES_OFF;
}
static u32 timer_every, timer_last;   /* hundredths; the clock goes round at midnight */
static int play_below;

int  snd_queued( void );
int  con_take_trapped_key( int *which, const u8 *armed );

void events_reset( void )
{
    mem_set( &ev_timer, 0, sizeof( ev_timer ) );
    mem_set( &ev_play, 0, sizeof( ev_play ) );
    mem_set( ev_keys, 0, sizeof( ev_keys ) );
    timer_every = 0;
    play_below = 0;
}

static EVENT *event_of( int kind, int which )
{
    switch ( kind ) {
    case EV_TIMER:  return &ev_timer;
    case EV_PLAY:   return &ev_play;
    case EV_KEY:
        if ( which < 1 || which >= NKEYS ) {
            vm_error( E_ILLEGAL_CALL );
        }
        return &ev_keys[which];
    }
    return NULL;                                /* COM, PEN, STRIG: never happen */
}

/* ON event GOSUB: which, kind, handler */
void rtf_EVSET( void )
{
    u32 handler = (u32)POP_L();
    int kind = (int)POP_L();
    s32 which = POP_L();
    EVENT *ev;

    if ( kind == EV_TIMER ) {
        if ( which < 1 || which > 86400L ) {
            vm_error( E_ILLEGAL_CALL );
        }
        timer_every = (u32)which * 100UL;
        timer_last = sys_hundredths();
    } else if ( kind == EV_PLAY ) {
        if ( which < 1 || which > 32 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        play_below = (int)which;
    }
    ev = event_of( kind, (int)which );
    if ( ev != NULL ) {
        ev->handler = handler ? vm.code + handler : NULL;
    }
}

/* KEY(n) ON and the like: which, kind, state (1 ON, 0 OFF, 2 STOP) */
void rtf_EVCTL( void )
{
    int state = (int)POP_L(), kind = (int)POP_L(), i;
    s32 which = POP_L();
    EVENT *ev;

    if ( kind == EV_KEY && which == 0 ) {
        for ( i = 1; i < NKEYS; i++ ) {
            ev_keys[i].state = (u8)state;
            if ( state == ES_OFF ) {
                ev_keys[i].pending = 0;
            }
        }
        return;
    }
    ev = event_of( kind, (int)which );
    if ( ev == NULL ) {
        return;
    }
    ev->state = (u8)state;
    if ( state == ES_OFF ) {
        ev->pending = 0;
    }
    if ( kind == EV_TIMER && state == ES_ON ) {
        timer_last = sys_hundredths();
    }
    vm_poll_now();
}

/* one event that is due, ON and not busy: its handler; the caller GOSUBs */
static int fire( EVENT *ev, u8 **handler )
{
    if ( ev->handler == NULL || ev->state == ES_OFF ) {
        ev->pending = 0;
        return 0;
    }
    if ( ev->state == ES_STOP || ev->busy ) {
        ev->pending = 1;
        return 0;
    }
    ev->pending = 0;
    ev->busy = 1;
    *handler = ev->handler;
    return 1;
}

int events_check( u8 **handler, int *kind, int *which )
{
    u32 now;
    u8 armed[NKEYS];
    int i, key;

    if ( vm.in_handler ) {
        return 0;                               /* not inside an error handler */
    }
    if ( timer_every && ev_timer.handler ) {
        /* the time since the last one, across midnight too (a whole day,
           86400 seconds, comes a hundredth early) */
        u32 since;
        now = sys_hundredths();
        since = (now + 8640000UL - timer_last) % 8640000UL;
        if ( since >= timer_every || since == 8639999UL || ev_timer.pending ) {
            if ( since >= timer_every || since == 8639999UL ) {
                timer_last = now;
                if ( ev_timer.state != ES_OFF ) {
                    ev_timer.pending = 1;
                }
            }
            if ( ev_timer.pending && fire( &ev_timer, handler ) ) {
                *kind = EV_TIMER;
                *which = 0;
                return 1;
            }
        }
    }
    if ( play_below && ev_play.handler && ev_play.state != ES_OFF ) {
        if ( snd_queued() < play_below || ev_play.pending ) {
            ev_play.pending = 1;
            if ( fire( &ev_play, handler ) ) {
                *kind = EV_PLAY;
                *which = 0;
                return 1;
            }
        }
    }
    for ( i = 0; i < NKEYS; i++ ) {
        armed[i] = (u8)(ev_keys[i].handler != NULL && ev_keys[i].state != ES_OFF);
        if ( armed[i] && ev_keys[i].pending && fire( &ev_keys[i], handler ) ) {
            *kind = EV_KEY;
            *which = i;
            return 1;
        }
    }
    if ( con_take_trapped_key( &key, armed ) ) {
        ev_keys[key].pending = 1;
        if ( fire( &ev_keys[key], handler ) ) {
            *kind = EV_KEY;
            *which = key;
            return 1;
        }
    }
    return 0;
}

/* RETURN from an event's handler: it may happen again */
void events_done( int kind, int which )
{
    EVENT *ev = NULL;

    switch ( kind ) {
    case EV_TIMER:  ev = &ev_timer; break;
    case EV_PLAY:   ev = &ev_play; break;
    case EV_KEY:    ev = which > 0 && which < NKEYS ? &ev_keys[which] : NULL; break;
    }
    if ( ev != NULL ) {
        ev->busy = 0;
        if ( ev->pending ) {
            vm_poll_now();
        }
    }
}

/* ------------------------------------------------------------------ */
/* the date and the time                                               */
/* ------------------------------------------------------------------ */

static void two( char *at, int v )
{
    at[0] = (char)('0' + v / 10 % 10);
    at[1] = (char)('0' + v % 10);
}

void rtf_DATE( void )
{
    char buf[10];
    int year, month, day;

    sys_date( &year, &month, &day );
    two( buf, month );
    buf[2] = '-';
    two( buf + 3, day );
    buf[5] = '-';
    two( buf + 6, year / 100 );
    two( buf + 8, year % 100 );
    PUSH_STR( qs_make( buf, 10 ) );
}

void rtf_TIME( void )
{
    char buf[8];
    int hour, minute, second, hund;

    sys_time( &hour, &minute, &second, &hund );
    two( buf, hour );
    buf[2] = ':';
    two( buf + 3, minute );
    buf[5] = ':';
    two( buf + 6, second );
    PUSH_STR( qs_make( buf, 8 ) );
}

/* up to "max" numbers separated by any of "seps"; -> how many */
static int parse_fields( STR *s, int *out, int max, const char *seps )
{
    u32 pos = 0, len = QS_LEN( s );
    const char *text = QS_TEXT( s );
    int count = 0, val, digits;

    while ( pos < len && text[pos] == ' ' ) {
        pos++;
    }
    while ( count < max ) {
        val = 0;
        digits = 0;
        while ( pos < len && text[pos] >= '0' && text[pos] <= '9' ) {
            val = val * 10 + (text[pos++] - '0');
            digits++;
            if ( digits > 4 ) {
                return -1;
            }
        }
        if ( digits == 0 ) {
            return -1;
        }
        out[count++] = val;
        if ( pos >= len ) {
            break;
        }
        if ( str_chr( seps, text[pos] ) == NULL ) {
            return -1;
        }
        pos++;
    }
    while ( pos < len && text[pos] == ' ' ) {
        pos++;
    }
    return pos == len ? count : -1;
}

void rtf_DATESET( void )
{
    STR *s = POP_STR();
    int f[3], n = parse_fields( s, f, 3, "-/" );

    qs_release( s );
    if ( n != 3 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( f[2] < 100 ) {
        f[2] += f[2] < 80 ? 2000 : 1900;
    }
    if ( f[0] < 1 || f[0] > 12 || f[1] < 1 || f[1] > 31 || f[2] < 1980 || f[2] > 2099
      || sys_set_date( f[2], f[0], f[1] ) != 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
}

void rtf_TIMESET( void )
{
    STR *s = POP_STR();
    int f[3] = { 0, 0, 0 }, n = parse_fields( s, f, 3, ":" );

    qs_release( s );
    if ( n < 1 || f[0] > 23 || f[1] > 59 || f[2] > 59 || sys_set_time( f[0], f[1], f[2] ) != 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
}

/* ------------------------------------------------------------------ */
/* SLEEP and the joystick                                              */
/* ------------------------------------------------------------------ */

/* SLEEP [seconds]: until the time is up or a key is pressed */
/* an enabled ON TIMER or ON PLAY event is due: SLEEP ends for it */
static int event_due( void )
{
    if ( timer_every && ev_timer.handler && ev_timer.state == ES_ON ) {
        u32 since = (sys_hundredths() + 8640000UL - timer_last) % 8640000UL;
        if ( since >= timer_every || ev_timer.pending ) {
            return 1;
        }
    }
    return play_below && ev_play.handler && ev_play.state == ES_ON && snd_queued() < play_below;
}

void rtf_SLEEP( void )
{
    s32 seconds = POP_L();
    int given = (int)POP_L();
    u32 start, now, until = 0;
    int keys_before;

    con_flush();
    gfx_flush();
    start = sys_hundredths();
    if ( given && seconds > 0 ) {
        until = (u32)seconds * 100UL;
    }
    con_poll_keys();
    keys_before = con_key_waiting();
    for ( ;; ) {
        con_poll_keys();
        if ( con_key_waiting() > keys_before ) {
            return;
        }
        snd_service();
        if ( event_due() ) {
            vm_poll_now();                      /* its handler at the next statement */
            return;
        }
        if ( until ) {
            now = sys_hundredths();
            if ( now < start ) {
                now += 8640000UL;               /* midnight */
            }
            if ( now - start >= until ) {
                return;
            }
        }
    }
}

/* the game port: four timers and four buttons at 201h */
static int stick_val[4];

static void read_sticks( void )
{
    u32 count, bits, done = 0;
    int i;

    for ( i = 0; i < 4; i++ ) {
        stick_val[i] = 0;
    }
    port_out( 0x201, 0 );
    for ( count = 0; count < 2000 && done != 0x0F; count++ ) {
        bits = port_in( 0x201 ) & 0x0F;
        for ( i = 0; i < 4; i++ ) {
            if ( !(done & (1u << i)) && !(bits & (1u << i)) ) {
                stick_val[i] = (int)(count / 8);
                done |= 1u << i;
            }
        }
    }
}

void rtf_STICK( void )
{
    s32 which = POP_L();

    if ( which < 0 || which > 3 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    if ( which == 0 ) {
        read_sticks();
    }
    PUSH_L( stick_val[which] );
}

void rtf_STRIG( void )
{
    s32 which = POP_L();
    u32 bits;
    /* QBasic's order: A's first button, B's first, A's second, B's second
       (port 201h: 10h A1, 20h A2, 40h B1, 80h B2) */
    static const u8 button[8] = { 0x10, 0x10, 0x40, 0x40, 0x20, 0x20, 0x80, 0x80 };

    if ( which < 0 || which > 7 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    bits = port_in( 0x201 );
    PUSH_L( (bits & button[which]) ? 0 : -1 );
}

void rtf_PEN( void )
{
    s32 which = POP_L();

    if ( which < 0 || which > 9 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    PUSH_L( 0 );                                /* no light pen */
}
