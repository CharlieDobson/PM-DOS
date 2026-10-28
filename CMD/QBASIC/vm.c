/*
 * VM.C - carrying out the compiled program.
 *
 * ONE LOOP, ONE SWITCH.  The stack pointer and the code pointer are
 * the loop's own while it runs; before anything that may look at them
 * (a runtime routine, an error) they are written back into "vm".
 *
 * AN ERROR LEAVES FROM WHEREVER IT IS FOUND: vm_error() jumps back to
 * vm_go(), which lets go of the strings the failed statement had on
 * the stack and then either goes to the ON ERROR handler or stops the
 * run with RUN_ERROR.  The handler runs as module code with any
 * procedure's frame still under it, so that RESUME and RESUME NEXT can
 * go back into the procedure; RESUME to a label leaves every procedure.
 *
 * EVERY STATEMENT STARTS WITH OP_STMT, and every POLL_EVERY of them
 * (or every one, when stepping or when a breakpoint is set on it) the
 * loop looks round: Ctrl+Break, the keys for INKEY$ and ON KEY, the
 * music playing in the background, ON TIMER, a step or a breakpoint.
 */
#include "qb.h"

VM vm;

#define STACK_CELLS     16384
#define POLL_EVERY      256
#define FRAME_ARENA     (256UL * 1024UL)
#define ERR_BREAK       (-1)            /* not an error: Ctrl+Break */

extern void (*const rtf_table[RT_COUNT])( void );

static JMPBUF vm_jmp;
static int    vm_err_code;
static u32    vm_err_stmt_override;
static const DATAITEM *vm_err_data;

void events_reset( void );
int  events_check( u8 **handler, int *kind, int *which );
void events_done( int kind, int which );
void mem_reset_segs( void );
void rnd_reset( void );
void arr_free( ARR *arr );
void arr_unpark( void );
ARR *arr_make( QTYPE type, u32 esize, int ndims, const s32 *bounds, int flags );

void push_l( s32 val )
{
    vm.stag[vm.sp - vm.stack] = 0;
    (vm.sp++)->l = val;
}

void push_d( double val )
{
    vm.stag[vm.sp - vm.stack] = 0;
    (vm.sp++)->d = val;
}

void push_str( STR *val )
{
    vm.stag[vm.sp - vm.stack] = 1;
    (vm.sp++)->str = val;
}

/* A SINGLE about to be shown by PRINT, WRITE, PRINT USING or STR$.  QBasic
   keeps the wider value until it is stored or shown: PRINT EXP(89) is
   Overflow, d# = EXP(89) is not. */
double pop_single( void )
{
    double v = POP_S();

    if ( v > SNG_MAX || v < -SNG_MAX ) {
        vm_error( E_OVERFLOW );
    }
    return v;
}

void vm_error( int code )
{
    vm_err_code = code;
    vm_err_stmt_override = 0xFFFFFFFFUL;
    vm_err_data = NULL;
    rt_longjmp( &vm_jmp, 1 );
}

/* an error that belongs to another statement: READ's bad item is the
   DATA statement's */
void vm_error_at( int code, u32 stmt )
{
    vm_err_code = code;
    vm_err_stmt_override = stmt;
    vm_err_data = NULL;
    rt_longjmp( &vm_jmp, 1 );
}

/* READ's bad item: the error is the READ statement's (RESUME NEXT goes
   on after the READ), but it is shown where the item is written */
void vm_error_data( int code, const DATAITEM *item )
{
    vm_err_code = code;
    vm_err_stmt_override = 0xFFFFFFFFUL;
    vm_err_data = item;
    rt_longjmp( &vm_jmp, 1 );
}

u32 vm_current_stmt( void )
{
    return vm.stmt;
}

/* ------------------------------------------------------------------ */
/* frames                                                              */
/* ------------------------------------------------------------------ */

static void release_slots( u8 *base, const u32 *strs, u32 nstrs, const u32 *arrs, u32 narrs )
{
    u32 i;

    for ( i = 0; i < nstrs; i++ ) {
        STR **slot = (STR **)(base + strs[i]);
        qs_release( *slot );
        *slot = NULL;
    }
    for ( i = 0; i < narrs; i++ ) {
        ARR **slot = (ARR **)(base + arrs[i]);
        arr_free( *slot );
        *slot = NULL;
    }
}

/* the frame on top let go of: its strings and arrays, its memory */
static void frame_pop( void )
{
    FRAME *frame = &vm.frames[--vm.nframes];
    PROC *proc = frame->proc;

    file_forget_vars( frame->mine, frame->mine + frame->mine_size );
    mem_forget( frame->mine, frame->mine + frame->mine_size );
    release_slots( frame->mine, proc->rel_str, proc->nrel_str, proc->rel_arr, proc->nrel_arr );
    vm.ftop -= frame->mine_size;
    vm.fdata = frame->fdata;
    vm.sdata = frame->sdata;
    if ( vm.ngosub > frame->gosub_base ) {
        vm.ngosub = frame->gosub_base;
    }
    vm.stmt = frame->stmt;
    vm.stmt_sp = frame->stmt_sp;
    con_print_pop( &frame->print );
}

/* every frame above "depth" let go of */
static void frames_unwind( u32 depth )
{
    while ( vm.nframes > depth ) {
        frame_pop();
    }
}

static void call_proc( int index, u8 *ret_pc )
{
    PROGRAM *prog = vm.prog;
    PROC *proc = &prog->procs[index];
    FRAME *frame;
    u32 size = (proc->frame_size + 7u) & ~7u, i;
    u8 *mine;

    /* each call leaves the caller's pending cells on the stack; keep room
       for the most one statement can push (a 4,095-byte line) */
    if ( vm.stack_end - vm.sp < 2048 ) {
        vm_error( E_OUT_OF_STACK );
    }
    if ( vm.nframes == vm.frames_cap ) {
        u32 cap = vm.frames_cap ? vm.frames_cap * 2 : 64;
        FRAME *grown = (FRAME *)try_realloc( vm.frames, cap * sizeof( FRAME ) );
        if ( grown == NULL ) {
            vm_error( E_OUT_OF_STACK );
        }
        vm.frames = grown;
        vm.frames_cap = cap;
    }
    if ( size < 8 ) {
        size = 8;
    }
    if ( vm.ftop + size > vm.fcap ) {
        vm_error( E_OUT_OF_STACK );
    }
    mine = vm.farena + vm.ftop;
    mem_set( mine, 0, size );
    vm.ftop += size;
    /* the arguments' addresses, into the parameters' slots */
    vm.sp -= proc->nparams;
    for ( i = 0; i < (u32)proc->nparams; i++ ) {
        ((void **)mine)[i] = vm.sp[i].ptr;
    }
    frame = &vm.frames[vm.nframes++];
    frame->proc = proc;
    frame->ret_pc = ret_pc;
    frame->fdata = vm.fdata;
    frame->sdata = vm.sdata;
    frame->gosub_base = vm.ngosub;
    frame->mine = mine;
    frame->mine_size = size;
    frame->stmt = vm.stmt;
    frame->stmt_sp = vm.stmt_sp;
    con_print_push( &frame->print );
    vm.fdata = mine;
    vm.sdata = proc->static_data;
    vm.stmt_sp = vm.sp;
    /* arrays it uses without a DIM: made now, 0 to 10 */
    for ( i = 0; i < proc->nimp_arr; i++ ) {
        s32 bounds[MAX_DIMS * 2];
        int d;
        for ( d = 0; d < proc->imp_dims[i]; d++ ) {
            bounds[d * 2] = prog->option_base;
            bounds[d * 2 + 1] = 10;
        }
        /* static, as the module's are: ERASE zeroes them */
        *(ARR **)(mine + proc->imp_arr[i]) = arr_make( proc->imp_type[i], type_bytes( proc->imp_type[i] ),
                                                       proc->imp_dims[i], bounds, AF_STATIC | AF_IMPLICIT );
        if ( *(ARR **)(mine + proc->imp_arr[i]) == NULL ) {
            vm_error( E_OUT_OF_MEMORY );
        }
    }
    vm.pc = vm.code + proc->entry;
}

/* ------------------------------------------------------------------ */
/* the run                                                             */
/* ------------------------------------------------------------------ */

static void *zalloc( u32 size )
{
    void *mem = try_alloc( size ? size : 4 );

    if ( mem != NULL ) {
        mem_set( mem, 0, size ? size : 4 );
    }
    return mem;
}

static int make_arrays( u32 first )
{
    PROGRAM *prog = vm.prog;
    u32 i;

    for ( i = first; i < prog->narrinit; i++ ) {
        ARRINIT *init = &prog->arrinit[i];
        u8 *base = init->scope == SC_GLOBAL ? vm.gdata : prog->procs[init->proc].static_data;
        ARR *arr = arr_make( init->type, init->esize, init->ndims, init->bounds, init->flags );
        if ( arr == NULL ) {
            return 0;
        }
        *(ARR **)(base + init->offset) = arr;
    }
    return 1;
}

static void free_data( void )
{
    PROGRAM *prog = vm.prog;
    int i;

    if ( vm.gdata != NULL ) {
        file_forget_vars( vm.gdata, vm.gdata + vm.gcap );
        mem_forget( vm.gdata, vm.gdata + vm.gcap );
        release_slots( vm.gdata, prog->grel_str, prog->ngrel_str, prog->grel_arr, prog->ngrel_arr );
        xfree( vm.gdata );
        vm.gdata = NULL;
    }
    for ( i = 0; i < prog->nprocs; i++ ) {
        PROC *proc = &prog->procs[i];
        if ( proc->static_data != NULL ) {
            file_forget_vars( proc->static_data, proc->static_data + proc->static_size );
            mem_forget( proc->static_data, proc->static_data + proc->static_size );
            release_slots( proc->static_data, proc->srel_str, proc->nsrel_str, proc->srel_arr, proc->nsrel_arr );
            xfree( proc->static_data );
            proc->static_data = NULL;
        }
    }
}

/* the program's variables as a new run has them (CLEAR, RUN) */
static int fresh_data( void )
{
    PROGRAM *prog = vm.prog;
    int i;

    free_data();
    /* with room for the names the Immediate window makes */
    vm.gcap = prog->gsize + IMM_DATA_SPARE;
    vm.gdata = (u8 *)zalloc( vm.gcap );
    if ( vm.gdata == NULL ) {
        return 0;
    }
    for ( i = 0; i < prog->nprocs; i++ ) {
        PROC *proc = &prog->procs[i];
        proc->static_data = (u8 *)zalloc( proc->static_size );
        if ( proc->static_data == NULL ) {
            return 0;
        }
    }
    return make_arrays( 0 );
}

/* ------------------------------------------------------------------ */
/* CHAIN: the blank COMMON's values, and the open files, go on to the  */
/* next program                                                        */
/* ------------------------------------------------------------------ */

static COMMONVAR *carry_vars;           /* the old program's COMMON, its values in carry_data */
static u8        *carry_data;
static u32        carry_n;
static int        keep_files;           /* the next vm_end and vm_begin leave the files open */

/* a string or an array changes owner: the old program's slot is emptied,
   so its end does not free what the next program is given */
void vm_common_take( void )
{
    PROGRAM *prog = vm.prog;
    u32 i, total = 0, at = 0;

    vm_common_drop();
    keep_files = 1;
    if ( prog == NULL || prog->ncommon == 0 || vm.gdata == NULL ) {
        return;
    }
    for ( i = 0; i < prog->ncommon; i++ ) {
        total += prog->common[i].size;
    }
    carry_vars = (COMMONVAR *)try_alloc( prog->ncommon * sizeof( COMMONVAR ) );
    carry_data = (u8 *)try_alloc( total ? total : 1 );
    if ( carry_vars == NULL || carry_data == NULL ) {
        xfree( carry_vars );
        xfree( carry_data );
        carry_vars = NULL;
        carry_data = NULL;
        return;                                 /* no memory: the COMMON is lost */
    }
    mem_cpy( carry_vars, prog->common, prog->ncommon * sizeof( COMMONVAR ) );
    carry_n = prog->ncommon;
    for ( i = 0; i < carry_n; i++ ) {
        u8 *slot = vm.gdata + carry_vars[i].offset;
        mem_cpy( carry_data + at, slot, carry_vars[i].size );
        if ( carry_vars[i].is_array || TKIND( carry_vars[i].type ) == TY_STR ) {
            *(void **)slot = NULL;
        }
        at += carry_vars[i].size;
    }
}

static void carry_release( COMMONVAR *cv, u8 *data )
{
    if ( cv->is_array ) {
        arr_free( *(ARR **)data );
    } else if ( TKIND( cv->type ) == TY_STR ) {
        qs_release( *(STR **)data );
    }
}

/* into the new program's COMMON, by position, where the kind and size
   agree (the rest is let go) */
void vm_common_give( void )
{
    PROGRAM *prog = vm.prog;
    u32 i, at = 0;

    for ( i = 0; i < carry_n; i++ ) {
        COMMONVAR *cv = &carry_vars[i], *to = prog != NULL && i < prog->ncommon ? &prog->common[i] : NULL;
        u8 *data = carry_data + at;
        at += cv->size;
        if ( to == NULL || vm.gdata == NULL || to->is_array != cv->is_array || to->size != cv->size
          || TKIND( to->type ) != TKIND( cv->type ) ) {
            carry_release( cv, data );
            continue;
        }
        if ( cv->is_array && *(ARR **)data != NULL && (*(ARR **)data)->esize != type_bytes( to->type ) ) {
            carry_release( cv, data );          /* STRING * n or a TYPE of another size */
            continue;
        }
        carry_release( to, vm.gdata + to->offset );    /* what the new program made there */
        mem_cpy( vm.gdata + to->offset, data, cv->size );
    }
    xfree( carry_vars );
    xfree( carry_data );
    carry_vars = NULL;
    carry_data = NULL;
    carry_n = 0;
}

void vm_common_drop( void )
{
    u32 i, at = 0;

    for ( i = 0; i < carry_n; i++ ) {
        carry_release( &carry_vars[i], carry_data + at );
        at += carry_vars[i].size;
    }
    xfree( carry_vars );
    xfree( carry_data );
    carry_vars = NULL;
    carry_data = NULL;
    carry_n = 0;
    if ( keep_files ) {
        keep_files = 0;
        file_close_all();                       /* the next program never came */
    }
}

int vm_begin( PROGRAM *prog )
{
    vm_end();
    mem_set( &vm, 0, sizeof( vm ) );
    vm.prog = prog;
    vm.code = prog->code;
    vm.stack = (CELL *)try_alloc( STACK_CELLS * sizeof( CELL ) );
    vm.stag = (u8 *)try_alloc( STACK_CELLS );
    vm.farena = (u8 *)try_alloc( FRAME_ARENA );
    if ( vm.stack == NULL || vm.stag == NULL || vm.farena == NULL || !fresh_data() ) {
        vm_end();
        return 0;
    }
    vm.fcap = FRAME_ARENA;
    vm.stack_end = vm.stack + STACK_CELLS - 16;
    vm.sp = vm.stack;
    vm.stmt_sp = vm.stack;
    vm.pc = vm.code + prog->main_entry;
    vm.sdata = vm.gdata;
    vm.running = 1;
    vm.poll = 1;
    if ( keep_files ) {
        keep_files = 0;                         /* CHAIN: the files stay open */
    } else {
        file_reset_state();
    }
    events_reset();
    mem_reset_segs();
    rnd_reset();
    snd_stop();
    return 1;
}

void vm_end( void )
{
    CELL *cell;

    if ( vm.prog == NULL ) {
        return;
    }
    if ( vm.stack != NULL ) {
        for ( cell = vm.stack; cell < vm.sp; cell++ ) {
            if ( vm.stag[cell - vm.stack] ) {
                qs_release( cell->str );
            }
        }
    }
    frames_unwind( 0 );
    arr_unpark();
    free_data();
    if ( !keep_files ) {
        file_close_all();
    }
    snd_stop();
    xfree( vm.stack );
    xfree( vm.stag );
    xfree( vm.farena );
    xfree( vm.frames );
    xfree( vm.gosubs );
    vm.stack = NULL;
    vm.stag = NULL;
    vm.farena = NULL;
    vm.frames = NULL;
    vm.gosubs = NULL;
    vm.running = 0;
    vm.prog = NULL;
}

/* CLEAR: every variable back to nothing, files closed */
void rtf_CLEAR( void )
{
    CELL *cell;

    /* QBasic: Illegal function call in a SUB or FUNCTION (the frames
       below would be left running on freed variables) */
    if ( vm.nframes > 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    for ( cell = vm.stack; cell < vm.sp; cell++ ) {
        if ( vm.stag[cell - vm.stack] ) {
            qs_release( cell->str );
            vm.stag[cell - vm.stack] = 0;
        }
    }
    vm.sp = vm.stack;
    vm.stmt_sp = vm.stack;
    vm.ngosub = 0;
    vm.on_error = NULL;
    vm.clears++;
    arr_unpark();
    if ( !fresh_data() ) {
        vm.dead = 1;
        vm_error( E_OUT_OF_MEMORY );
    }
    events_reset();
    vm.on_error = NULL;
    vm.sdata = vm.gdata;
    vm.fdata = NULL;
    file_raise( file_close_all() );             /* last: the rest is done whatever it says */
}

/* Set Next Statement: only within the procedure that is running */
void vm_set_next( u32 stmt )
{
    vm.pc = vm.code + vm.prog->locs[stmt].pc;
    vm.stmt = stmt;
}

static void gosub_push( u8 *ret, int event, int which )
{
    GOSUBREC *rec;

    if ( vm.ngosub == vm.gosub_cap ) {
        u32 cap = vm.gosub_cap ? vm.gosub_cap * 2 : 64;
        GOSUBREC *grown = (GOSUBREC *)try_realloc( vm.gosubs, cap * sizeof( GOSUBREC ) );
        if ( grown == NULL ) {
            vm_error( E_OUT_OF_STACK );
        }
        vm.gosubs = grown;
        vm.gosub_cap = cap;
    }
    rec = &vm.gosubs[vm.ngosub++];
    rec->pc = ret;
    rec->frame = vm.nframes;
    rec->event = (u8)event;
    rec->which = (u8)which;
}

static u8 *gosub_pop( void )
{
    GOSUBREC *rec;

    if ( vm.ngosub == 0 || (vm.nframes > 0 && vm.ngosub <= vm.frames[vm.nframes - 1].gosub_base) ) {
        vm_error( E_RETURN_NO_GOSUB );
    }
    rec = &vm.gosubs[--vm.ngosub];
    if ( rec->event ) {
        events_done( rec->event, rec->which );
    }
    return rec->pc;
}

/* Look round, between statements: -> RUN_* to stop with, or -1 */
static int look_round( u32 stmt )
{
    PROGRAM *prog = vm.prog;
    u8 *handler;
    int kind, which;

    vm.poll = POLL_EVERY;
    con_poll_keys();
    snd_service();
    if ( vm.stopped >= 0 ) {
        int why = vm.stopped;
        vm.stopped = -1;
        return why;
    }
    if ( prog->uses_events && events_check( &handler, &kind, &which ) ) {
        gosub_push( vm.pc - 5, kind, which );   /* back to this statement */
        vm.pc = handler;
        return -2;                              /* go on, at the handler */
    }
    if ( vm.skip_stop ) {
        vm.skip_stop = 0;
        if ( vm.go != GO_RUN ) {
            vm.poll = 0;
        }
        return -1;
    }
    if ( (prog->locs[stmt].flags & (SL_BREAK | SL_HERE)) && !vm.immediate ) {
        return RUN_BREAKPOINT;
    }
    if ( vm.go == GO_STEP ) {
        return RUN_STEP;
    }
    if ( vm.go == GO_PSTEP ) {
        if ( vm.nframes <= vm.step_frames ) {
            return RUN_STEP;
        }
        vm.poll = 0;
    }
    return -1;
}

#define FETCH8()    (*pc++)
#define FETCH16()   (pc += 2, *(u16 *)(pc - 2))
#define FETCH32()   (pc += 4, *(u32 *)(pc - 4))
#define SYNC()      (vm.pc = pc, vm.sp = sp)
#define RESYNC()    (pc = vm.pc, sp = vm.sp)
#define TAG( c, t ) (vm.stag[(c) - vm.stack] = (u8)(t))
#define FAIL( e )   do { SYNC(); vm_error( e ); } while ( 0 )
#define SNG_BAD( f ) ((*(volatile u32 *)&(f) & 0x7F800000UL) == 0x7F800000UL)
#define INT_RANGE( v ) ((v) >= -32768L && (v) <= 32767L)

static void *ref_ptr( u8 scope, u32 off )
{
    switch ( scope ) {
    case SC_GLOBAL: return vm.gdata + off;
    case SC_FRAME:  return vm.fdata + off;
    case SC_STATIC: return vm.sdata + off;
    }
    return *(void **)(vm.fdata + off);
}

static s32 round_whole( double v, int to_int )
{
    double r = fp_round( v );

    if ( to_int ? (r < -32768.0 || r > 32767.0) : (r < -2147483648.0 || r > 2147483647.0) ) {
        vm_error( E_OVERFLOW );
    }
    return (s32)r;
}

/* x ^ y for the two floating types */
static double power( double base, double expo )
{
    s32 n;

    if ( expo == fp_trunc( expo ) && expo >= -1024.0 && expo <= 1024.0 ) {
        /* a whole power: multiplied out in the coprocessor */
        n = (s32)expo;
        if ( base == 0 && n < 0 ) {
            vm_error( E_ILLEGAL_CALL );         /* 0 ^ -1 */
        }
        return fp_powi( base, (int)n );
    }
    if ( base == 0 ) {
        if ( expo < 0 ) {
            vm_error( E_ILLEGAL_CALL );
        }
        return 0;
    }
    if ( base < 0 ) {
        vm_error( E_ILLEGAL_CALL );
    }
    return fp_pow( base, expo );
}

static int run_loop( void )
{
    u8 *pc = vm.pc;
    CELL *sp = vm.sp;
    PROGRAM *prog = vm.prog;
    u8 *code = vm.code;
    u32 off, a32;
    s32 lv, rv, res;
    double dv;
    volatile float fv;                  /* through memory: a cast alone may not round */
    int op, why;

    for ( ;; ) {
        op = FETCH8();
        switch ( op ) {
        case OP_CI:
            TAG( sp, 0 );
            (sp++)->l = (s32)FETCH32();
            break;
        case OP_CS:
            TAG( sp, 0 );
            (sp++)->d = *(float *)pc;
            pc += 4;
            break;
        case OP_CD:
            TAG( sp, 0 );
            (sp++)->d = *(double *)pc;
            pc += 8;
            break;
        case OP_CSTR:
            TAG( sp, 1 );
            (sp++)->str = qs_retain( prog->consts[FETCH32()] );
            break;
        case OP_POPN:
            sp--;
            break;
        case OP_POPS:
            qs_release( (--sp)->str );
            break;
        case OP_DUP:
            *sp = sp[-1];
            TAG( sp, vm.stag[sp - 1 - vm.stack] );
            if ( vm.stag[sp - vm.stack] ) {
                qs_retain( sp->str );
            }
            sp++;
            break;

        /* addresses */
        case OP_AG:
            TAG( sp, 0 );
            (sp++)->ptr = vm.gdata + FETCH32();
            break;
        case OP_AF:
            TAG( sp, 0 );
            (sp++)->ptr = vm.fdata + FETCH32();
            break;
        case OP_AS:
            TAG( sp, 0 );
            (sp++)->ptr = vm.sdata + FETCH32();
            break;
        case OP_AR:
            TAG( sp, 0 );
            (sp++)->ptr = *(void **)(vm.fdata + FETCH32());
            break;
        case OP_AOFS:
            sp[-1].ptr = (u8 *)sp[-1].ptr + FETCH32();
            break;
        case OP_AIDX1: {
            ARR *arr = *(ARR **)sp[-2].ptr;
            u32 idx;
            if ( arr == NULL ) {
                FAIL( E_SUBSCRIPT );
            }
            idx = (u32)(sp[-1].l - arr->lb[0]);
            if ( idx >= (u32)arr->lb[1] || arr->ndims != 1 ) {
                FAIL( E_SUBSCRIPT );
            }
            sp--;
            sp[-1].ptr = arr->data + idx * arr->esize;
            break;
        }
        case OP_AIDX: {
            int n = FETCH8(), d;
            ARR *arr = *(ARR **)sp[-n - 1].ptr;
            u32 lin = 0, mult = 1, idx;
            if ( arr == NULL || arr->ndims != n ) {
                FAIL( E_SUBSCRIPT );
            }
            for ( d = 0; d < n; d++ ) {
                idx = (u32)(sp[-n + d].l - arr->lb[d]);
                if ( idx >= (u32)arr->lb[n + d] ) {
                    FAIL( E_SUBSCRIPT );
                }
                lin += idx * mult;
                mult *= (u32)arr->lb[n + d];
            }
            sp -= n;
            sp[-1].ptr = arr->data + lin * arr->esize;
            break;
        }
        case OP_AIDXR1: {
            ARR *arr = *(ARR **)sp[-2].ptr;
            u32 idx;
            if ( arr == NULL ) {
                FAIL( E_SUBSCRIPT );
            }
            idx = (u32)(sp[-1].l - arr->lb[0]);
            if ( idx >= (u32)arr->lb[1] || arr->ndims != 1 ) {
                FAIL( E_SUBSCRIPT );
            }
            sp[-1].u = idx * arr->esize;        /* the slot stays under it */
            break;
        }
        case OP_AIDXR: {
            int n = FETCH8(), d;
            ARR *arr = *(ARR **)sp[-n - 1].ptr;
            u32 lin = 0, mult = 1, idx;
            if ( arr == NULL || arr->ndims != n ) {
                FAIL( E_SUBSCRIPT );
            }
            for ( d = 0; d < n; d++ ) {
                idx = (u32)(sp[-n + d].l - arr->lb[d]);
                if ( idx >= (u32)arr->lb[n + d] ) {
                    FAIL( E_SUBSCRIPT );
                }
                lin += idx * mult;
                mult *= (u32)arr->lb[n + d];
            }
            sp -= n - 1;
            sp[-1].u = lin * arr->esize;
            break;
        }
        case OP_EREF: {
            /* slot, offset, the copy's address, the note's: the element
               into the copy, and the copy's address is the argument */
            u32 size = FETCH32();
            int is_str = FETCH8();
            ARR **slot = (ARR **)sp[-4].ptr;
            u32 at = sp[-3].u;
            u8 *copy = (u8 *)sp[-2].ptr;
            u8 *elem = (*slot)->data + at;
            ((void **)sp[-1].ptr)[0] = slot;
            ((u32 *)sp[-1].ptr)[1] = at;
            if ( is_str ) {
                STR *old = *(STR **)copy;
                *(STR **)copy = qs_retain( *(STR **)elem );
                qs_release( old );
            } else {
                mem_cpy( copy, elem, size );
            }
            sp -= 3;
            sp[-1].ptr = copy;
            TAG( sp - 1, 0 );
            break;
        }
        case OP_EBACK: {
            /* the copy's address, the note's: back into the element, if
               the array is still there and still that big */
            u32 size = FETCH32();
            int is_str = FETCH8();
            u8 *copy = (u8 *)sp[-2].ptr;
            ARR *arr = *(ARR **)((void **)sp[-1].ptr)[0];
            u32 at = ((u32 *)sp[-1].ptr)[1];
            if ( arr != NULL && at <= arr->count * arr->esize && size <= arr->count * arr->esize - at ) {
                u8 *elem = arr->data + at;
                if ( is_str ) {
                    qs_release( *(STR **)elem );
                    *(STR **)elem = *(STR **)copy;
                } else {
                    mem_cpy( elem, copy, size );
                }
            } else if ( is_str ) {
                qs_release( *(STR **)copy );
            }
            if ( is_str ) {
                *(STR **)copy = NULL;
            }
            sp -= 2;
            break;
        }
        case OP_EXCH: {
            CELL cell = sp[-1];
            u8 tag = vm.stag[sp - 1 - vm.stack];
            sp[-1] = sp[-2];
            TAG( sp - 1, vm.stag[sp - 2 - vm.stack] );
            sp[-2] = cell;
            TAG( sp - 2, tag );
            break;
        }

        /* through an address */
        case OP_LDI:
            sp[-1].l = *(s16 *)sp[-1].ptr;
            break;
        case OP_LDL:
            sp[-1].l = *(s32 *)sp[-1].ptr;
            break;
        case OP_LDS:
            sp[-1].d = *(float *)sp[-1].ptr;
            break;
        case OP_LDD:
            sp[-1].d = *(double *)sp[-1].ptr;
            break;
        case OP_LDSTR:
            sp[-1].str = qs_retain( *(STR **)sp[-1].ptr );
            TAG( sp - 1, 1 );
            break;
        case OP_LDFS: {
            u32 len = FETCH16();
            SYNC();
            sp[-1].str = qs_make( (const char *)sp[-1].ptr, len );
            TAG( sp - 1, 1 );
            break;
        }
        case OP_STI:
            *(s16 *)sp[-2].ptr = (s16)sp[-1].l;
            sp -= 2;
            break;
        case OP_STL:
            *(s32 *)sp[-2].ptr = sp[-1].l;
            sp -= 2;
            break;
        case OP_STS:
            fv = (float)sp[-1].d;
            if ( SNG_BAD( fv ) ) {
                FAIL( E_OVERFLOW );
            }
            *(float *)sp[-2].ptr = fv;
            sp -= 2;
            break;
        case OP_STD:
            *(double *)sp[-2].ptr = sp[-1].d;
            sp -= 2;
            break;
        case OP_STSTR: {
            STR **slot = (STR **)sp[-2].ptr;
            STR *old = *slot;
            *slot = sp[-1].str;
            qs_release( old );
            sp -= 2;
            break;
        }
        case OP_STFS: {
            u32 len = FETCH16(), have;
            STR *s = sp[-1].str;
            char *field = (char *)sp[-2].ptr;
            have = QS_LEN( s );
            if ( have > len ) {
                have = len;
            }
            mem_cpy( field, QS_TEXT( s ), have );
            mem_set( field + have, ' ', len - have );
            qs_release( s );
            sp -= 2;
            break;
        }
        case OP_STREC:
            off = FETCH32();
            mem_cpy( sp[-2].ptr, sp[-1].ptr, off );
            sp -= 2;
            break;

        /* the module's data and the frame */
        case OP_GLDI:
            TAG( sp, 0 );
            (sp++)->l = *(s16 *)(vm.gdata + FETCH32());
            break;
        case OP_GLDL:
            TAG( sp, 0 );
            (sp++)->l = *(s32 *)(vm.gdata + FETCH32());
            break;
        case OP_GLDS:
            TAG( sp, 0 );
            (sp++)->d = *(float *)(vm.gdata + FETCH32());
            break;
        case OP_GLDD:
            TAG( sp, 0 );
            (sp++)->d = *(double *)(vm.gdata + FETCH32());
            break;
        case OP_GLDSTR:
            TAG( sp, 1 );
            (sp++)->str = qs_retain( *(STR **)(vm.gdata + FETCH32()) );
            break;
        case OP_GSTI:
            *(s16 *)(vm.gdata + FETCH32()) = (s16)(--sp)->l;
            break;
        case OP_GSTL:
            *(s32 *)(vm.gdata + FETCH32()) = (--sp)->l;
            break;
        case OP_GSTS:
            fv = (float)sp[-1].d;
            if ( SNG_BAD( fv ) ) {
                FAIL( E_OVERFLOW );
            }
            *(float *)(vm.gdata + FETCH32()) = fv;
            sp--;
            break;
        case OP_GSTD:
            *(double *)(vm.gdata + FETCH32()) = (--sp)->d;
            break;
        case OP_GSTSTR: {
            STR **slot = (STR **)(vm.gdata + FETCH32());
            STR *old = *slot;
            *slot = (--sp)->str;
            qs_release( old );
            break;
        }
        case OP_FLDI:
            TAG( sp, 0 );
            (sp++)->l = *(s16 *)(vm.fdata + FETCH32());
            break;
        case OP_FLDL:
            TAG( sp, 0 );
            (sp++)->l = *(s32 *)(vm.fdata + FETCH32());
            break;
        case OP_FLDS:
            TAG( sp, 0 );
            (sp++)->d = *(float *)(vm.fdata + FETCH32());
            break;
        case OP_FLDD:
            TAG( sp, 0 );
            (sp++)->d = *(double *)(vm.fdata + FETCH32());
            break;
        case OP_FLDSTR:
            TAG( sp, 1 );
            (sp++)->str = qs_retain( *(STR **)(vm.fdata + FETCH32()) );
            break;
        case OP_FSTI:
            *(s16 *)(vm.fdata + FETCH32()) = (s16)(--sp)->l;
            break;
        case OP_FSTL:
            *(s32 *)(vm.fdata + FETCH32()) = (--sp)->l;
            break;
        case OP_FSTS:
            fv = (float)sp[-1].d;
            if ( SNG_BAD( fv ) ) {
                FAIL( E_OVERFLOW );
            }
            *(float *)(vm.fdata + FETCH32()) = fv;
            sp--;
            break;
        case OP_FSTD:
            *(double *)(vm.fdata + FETCH32()) = (--sp)->d;
            break;
        case OP_FSTSTR: {
            STR **slot = (STR **)(vm.fdata + FETCH32());
            STR *old = *slot;
            *slot = (--sp)->str;
            qs_release( old );
            break;
        }

        /* conversions */
        case OP_L2I:
            if ( !INT_RANGE( sp[-1].l ) ) {
                FAIL( E_OVERFLOW );
            }
            break;
        case OP_L2S:
            sp[-1].d = (double)sp[-1].l;
            break;
        case OP_L2D:
            sp[-1].d = (double)sp[-1].l;
            break;
        case OP_S2D:
            break;                              /* a SINGLE is a double on the stack */
        case OP_D2S:
            fv = (float)sp[-1].d;                /* CSNG: rounded to a single */
            if ( SNG_BAD( fv ) ) {
                FAIL( E_OVERFLOW );
            }
            sp[-1].d = fv;
            break;
        case OP_S2I:
            SYNC();
            sp[-1].l = round_whole( sp[-1].d, 1 );
            break;
        case OP_S2L:
            SYNC();
            sp[-1].l = round_whole( sp[-1].d, 0 );
            break;
        case OP_D2I:
            SYNC();
            sp[-1].l = round_whole( sp[-1].d, 1 );
            break;
        case OP_D2L:
            SYNC();
            sp[-1].l = round_whole( sp[-1].d, 0 );
            break;
        case OP_UNDER_S2L:
            SYNC();
            sp[-2].l = round_whole( sp[-2].d, 0 );
            break;
        case OP_UNDER_D2L:
            SYNC();
            sp[-2].l = round_whole( sp[-2].d, 0 );
            break;
        case OP_UNDER_L2S:
            sp[-2].d = (double)sp[-2].l;
            break;
        case OP_UNDER_L2D:
            sp[-2].d = (double)sp[-2].l;
            break;
        case OP_UNDER_S2D:
            break;

        /* INTEGER */
        case OP_ADDI:
            res = sp[-2].l + sp[-1].l;
            if ( !INT_RANGE( res ) ) {
                FAIL( E_OVERFLOW );
            }
            (--sp)[-1].l = res;
            break;
        case OP_SUBI:
            res = sp[-2].l - sp[-1].l;
            if ( !INT_RANGE( res ) ) {
                FAIL( E_OVERFLOW );
            }
            (--sp)[-1].l = res;
            break;
        case OP_MULI:
            res = sp[-2].l * sp[-1].l;
            if ( !INT_RANGE( res ) ) {
                FAIL( E_OVERFLOW );
            }
            (--sp)[-1].l = res;
            break;
        case OP_NEGI:
            if ( sp[-1].l == -32768L ) {
                FAIL( E_OVERFLOW );
            }
            sp[-1].l = -sp[-1].l;
            break;
        case OP_IDIVI:
        case OP_IDIVL:
            rv = sp[-1].l;
            lv = sp[-2].l;
            if ( rv == 0 ) {
                FAIL( E_DIV_ZERO );
            }
            if ( rv == -1 ) {
                /* QBasic's own: an INTEGER -32768 \ -1 is a divide fault that
                   ends the program, which is Overflow here; the LONG one
                   wraps to itself with no error */
                if ( op == OP_IDIVI && lv == -32768L ) {
                    FAIL( E_OVERFLOW );
                }
                res = (s32)(0U - (u32)lv);
            } else {
                res = lv / rv;
            }
            (--sp)[-1].l = res;
            break;
        case OP_MODI:
        case OP_MODL:
            rv = sp[-1].l;
            if ( rv == 0 ) {
                FAIL( E_DIV_ZERO );
            }
            res = rv == -1 ? 0 : sp[-2].l % rv;
            (--sp)[-1].l = res;
            break;

        /* LONG */
        case OP_ADDL:
            lv = sp[-2].l;
            rv = sp[-1].l;
            res = (s32)((u32)lv + (u32)rv);
            if ( ((lv ^ res) & (rv ^ res)) < 0 ) {
                FAIL( E_OVERFLOW );
            }
            (--sp)[-1].l = res;
            break;
        case OP_SUBL:
            lv = sp[-2].l;
            rv = sp[-1].l;
            res = (s32)((u32)lv - (u32)rv);
            if ( ((lv ^ rv) & (lv ^ res)) < 0 ) {
                FAIL( E_OVERFLOW );
            }
            (--sp)[-1].l = res;
            break;
        case OP_MULL:
            dv = (double)sp[-2].l * (double)sp[-1].l;
            if ( dv < -2147483647.0 || dv > 2147483647.0 ) {       /* -2^31 too, as QBasic */
                FAIL( E_OVERFLOW );
            }
            res = (s32)((u32)sp[-2].l * (u32)sp[-1].l);
            (--sp)[-1].l = res;
            break;
        case OP_NEGL:
            if ( sp[-1].l == (s32)0x80000000UL ) {
                FAIL( E_OVERFLOW );
            }
            sp[-1].l = -sp[-1].l;
            break;

        /* SINGLE and DOUBLE: a SINGLE's value is a double on the stack and
           is only rounded when it is stored (or CSNG'd), as QBasic's is on
           the coprocessor - CDBL(1 / 3) is .3333333333333333 */
        case OP_ADDS:
        case OP_ADDD:
            dv = sp[-2].d + sp[-1].d;
            goto dbl_result;
        case OP_SUBS:
        case OP_SUBD:
            dv = sp[-2].d - sp[-1].d;
            goto dbl_result;
        case OP_MULS:
        case OP_MULD:
            dv = sp[-2].d * sp[-1].d;
            goto dbl_result;
        case OP_DIVS:
        case OP_DIVD:
            if ( sp[-1].d == 0 ) {
                FAIL( sp[-2].d == 0 ? E_OVERFLOW : E_DIV_ZERO );    /* 0 / 0 is Overflow */
            }
            dv = sp[-2].d / sp[-1].d;
            goto dbl_result;
        case OP_POWS:
        case OP_POWD:
            SYNC();
            dv = power( sp[-2].d, sp[-1].d );
        dbl_result:
            if ( DBL_BAD( dv ) ) {
                FAIL( E_OVERFLOW );
            }
            (--sp)[-1].d = dv;
            break;
        case OP_NEGS:
        case OP_NEGD:
            sp[-1].d = sp[-1].d == 0 ? 0 : -sp[-1].d;         /* QBasic has no -0 */
            break;
        case OP_CAT:
            /* qs_cat owns both strings, and releases them itself when it
               fails: the unwinder must not see their cells */
            SYNC();
            vm.sp = sp - 2;
            sp[-2].str = qs_cat( sp[-2].str, sp[-1].str );
            sp--;
            break;

        /* comparisons */
        case OP_CMPL:
            lv = sp[-2].l;
            rv = sp[-1].l;
            res = lv < rv ? -1 : lv > rv ? 1 : 0;
            goto compared;
        case OP_CMPS:
        case OP_CMPD:
            res = sp[-2].d < sp[-1].d ? -1 : sp[-2].d > sp[-1].d ? 1 : 0;
            goto compared;
        case OP_CMPSTR:
            res = qs_cmp( sp[-2].str, sp[-1].str );
            qs_release( sp[-2].str );
            qs_release( sp[-1].str );
        compared:
            switch ( FETCH8() ) {
            case CC_EQ: res = res == 0; break;
            case CC_NE: res = res != 0; break;
            case CC_LT: res = res < 0; break;
            case CC_GT: res = res > 0; break;
            case CC_LE: res = res <= 0; break;
            default:    res = res >= 0; break;
            }
            sp--;
            sp[-1].l = res ? -1 : 0;
            TAG( sp - 1, 0 );
            break;

        /* logic */
        case OP_NOT:
            sp[-1].l = ~sp[-1].l;
            break;
        case OP_AND:
            sp[-2].l &= sp[-1].l;
            sp--;
            break;
        case OP_OR:
            sp[-2].l |= sp[-1].l;
            sp--;
            break;
        case OP_XOR:
            sp[-2].l ^= sp[-1].l;
            sp--;
            break;
        case OP_EQV:
            sp[-2].l = ~(sp[-2].l ^ sp[-1].l);
            sp--;
            break;
        case OP_IMP:
            sp[-2].l = ~sp[-2].l | sp[-1].l;
            sp--;
            break;

        /* flow */
        case OP_STMT:
            a32 = FETCH32();
            vm.stmt = a32;
            vm.stmt_sp = sp;
            if ( --vm.poll <= 0 || prog->locs[a32].flags ) {
                SYNC();
                why = look_round( a32 );
                if ( why >= 0 ) {
                    vm.pc = pc - 5;             /* the statement is still to come */
                    vm.skip_stop = 1;
                    return why;
                }
                RESYNC();
            }
            break;
        case OP_JMP:
            pc = code + FETCH32();
            break;
        case OP_JZ:
            a32 = FETCH32();
            if ( (--sp)->l == 0 ) {
                pc = code + a32;
            }
            break;
        case OP_JNZ:
            a32 = FETCH32();
            if ( (--sp)->l != 0 ) {
                pc = code + a32;
            }
            break;
        case OP_GOSUB:
            a32 = FETCH32();
            SYNC();
            gosub_push( pc, 0, 0 );
            pc = code + a32;
            break;
        case OP_RETURN:
            SYNC();
            pc = gosub_pop();
            break;
        case OP_RETURNTO:
            a32 = FETCH32();
            SYNC();
            gosub_pop();
            pc = code + a32;
            break;
        case OP_ONGO: {
            u32 count = FETCH16();
            int gosub = FETCH8();
            s32 n = (--sp)->l;
            if ( n < 0 || n > 255 ) {
                FAIL( E_ILLEGAL_CALL );
            }
            if ( n >= 1 && (u32)n <= count ) {
                a32 = *(u32 *)(pc + (n - 1) * 4);
                pc += count * 4;
                if ( gosub ) {
                    SYNC();
                    gosub_push( pc, 0, 0 );
                }
                pc = code + a32;
            } else {
                pc += count * 4;
            }
            break;
        }
        case OP_CALL:
            off = FETCH16();
            SYNC();
            call_proc( (int)off, pc );
            RESYNC();
            break;
        case OP_RET:
            SYNC();
            if ( vm.nframes == 0 ) {
                return RUN_END;
            }
            pc = vm.frames[vm.nframes - 1].ret_pc;
            frame_pop();
            sp = vm.sp;
            if ( vm.go == GO_PSTEP && vm.nframes < vm.step_frames ) {
                vm.step_frames = vm.nframes;
            }
            break;
        case OP_FRET: {
            FRAME *frame;
            PROC *proc;
            CELL value;
            int is_str = 0;
            SYNC();
            frame = &vm.frames[vm.nframes - 1];
            proc = frame->proc;
            switch ( TKIND( proc->rettype ) ) {
            case TY_INT:    value.l = *(s16 *)(frame->mine + proc->ret_off); break;
            case TY_LONG:   value.l = *(s32 *)(frame->mine + proc->ret_off); break;
            case TY_SINGLE: value.d = *(float *)(frame->mine + proc->ret_off); break;
            case TY_DOUBLE: value.d = *(double *)(frame->mine + proc->ret_off); break;
            default:
                value.str = *(STR **)(frame->mine + proc->ret_off);
                *(STR **)(frame->mine + proc->ret_off) = NULL;
                is_str = 1;
                break;
            }
            pc = frame->ret_pc;
            frame_pop();
            sp = vm.sp;
            TAG( sp, is_str );
            *sp++ = value;
            if ( vm.go == GO_PSTEP && vm.nframes < vm.step_frames ) {
                vm.step_frames = vm.nframes;
            }
            break;
        }

        /* FOR and NEXT */
        case OP_FORI:
        case OP_FORL:
        case OP_FORS:
        case OP_FORD:
        case OP_NEXTI:
        case OP_NEXTL:
        case OP_NEXTS:
        case OP_NEXTD: {
            u8 vs = pc[0], ls = pc[5], ss = pc[10];
            void *var = ref_ptr( vs, *(u32 *)(pc + 1) );
            void *lim = ref_ptr( ls, *(u32 *)(pc + 6) );
            void *stp = ref_ptr( ss, *(u32 *)(pc + 11) );
            int go_on;
            a32 = *(u32 *)(pc + 15);
            pc += 19;
            switch ( op ) {
            case OP_FORI:
                lv = *(s16 *)var;
                go_on = *(s16 *)stp >= 0 ? lv <= *(s16 *)lim : lv >= *(s16 *)lim;
                if ( !go_on ) {
                    pc = code + a32;
                }
                break;
            case OP_FORL:
                lv = *(s32 *)var;
                go_on = *(s32 *)stp >= 0 ? lv <= *(s32 *)lim : lv >= *(s32 *)lim;
                if ( !go_on ) {
                    pc = code + a32;
                }
                break;
            case OP_FORS:
                fv = *(float *)var;
                go_on = *(float *)stp >= 0 ? fv <= *(float *)lim : fv >= *(float *)lim;
                if ( !go_on ) {
                    pc = code + a32;
                }
                break;
            case OP_FORD:
                dv = *(double *)var;
                go_on = *(double *)stp >= 0 ? dv <= *(double *)lim : dv >= *(double *)lim;
                if ( !go_on ) {
                    pc = code + a32;
                }
                break;
            case OP_NEXTI:
                res = *(s16 *)var + *(s16 *)stp;
                if ( !INT_RANGE( res ) ) {
                    /* going down, QBasic has already stored the wrapped
                       value when it finds the overflow; going up it has not */
                    if ( *(s16 *)stp < 0 ) {
                        *(s16 *)var = (s16)res;
                    }
                    FAIL( E_OVERFLOW );
                }
                *(s16 *)var = (s16)res;
                go_on = *(s16 *)stp >= 0 ? res <= *(s16 *)lim : res >= *(s16 *)lim;
                if ( go_on ) {
                    pc = code + a32;
                }
                break;
            case OP_NEXTL:
                lv = *(s32 *)var;
                rv = *(s32 *)stp;
                res = (s32)((u32)lv + (u32)rv);
                if ( ((lv ^ res) & (rv ^ res)) < 0 ) {
                    FAIL( E_OVERFLOW );
                }
                *(s32 *)var = res;
                go_on = rv >= 0 ? res <= *(s32 *)lim : res >= *(s32 *)lim;
                if ( go_on ) {
                    pc = code + a32;
                }
                break;
            case OP_NEXTS:
                /* the sum is compared before it is rounded into the
                   variable: FOR x! = 16777214 TO 16777216 ends */
                dv = (double)*(float *)var + *(float *)stp;
                fv = (float)dv;
                if ( SNG_BAD( fv ) ) {
                    FAIL( E_OVERFLOW );
                }
                *(float *)var = fv;
                go_on = *(float *)stp >= 0 ? dv <= *(float *)lim : dv >= *(float *)lim;
                if ( go_on ) {
                    pc = code + a32;
                }
                break;
            default:
                dv = *(double *)var + *(double *)stp;
                if ( DBL_BAD( dv ) ) {
                    FAIL( E_OVERFLOW );
                }
                *(double *)var = dv;
                go_on = *(double *)stp >= 0 ? dv <= *(double *)lim : dv >= *(double *)lim;
                if ( go_on ) {
                    pc = code + a32;
                }
                break;
            }
            break;
        }
        case OP_SWAP: {
            u8 *lhs = (u8 *)sp[-2].ptr, *rhs = (u8 *)sp[-1].ptr, tmp;
            off = FETCH32();
            while ( off-- ) {
                tmp = *lhs;
                *lhs++ = *rhs;
                *rhs++ = tmp;
            }
            sp -= 2;
            break;
        }
        case OP_RT:
            off = FETCH16();
            SYNC();
            rtf_table[off]();
            RESYNC();
            if ( vm.stopped >= 0 ) {
                why = vm.stopped;
                vm.stopped = -1;
                return why;
            }
            break;
        case OP_END:
            SYNC();
            return RUN_END;
        case OP_MODEND:
            /* an error handler that ran off the module's end never said
               RESUME (an END in it is fine) */
            if ( vm.in_handler ) {
                FAIL( E_NO_RESUME );
            }
            SYNC();
            return RUN_END;
        case OP_STOP:
            SYNC();
            return RUN_STOP;
        case OP_SYSTEM:
            SYNC();
            return RUN_SYSTEM;
        case OP_ONERR:
            a32 = FETCH32();
            if ( a32 == 0 ) {
                vm.on_error = NULL;
                if ( vm.in_handler ) {
                    /* ON ERROR GOTO 0 in the handler: the error stands */
                    vm.fatal_stmt = vm.err_stmt;
                    SYNC();
                    vm_error_at( vm.err, vm.err_stmt );
                }
            } else {
                vm.on_error = code + a32;
            }
            if ( vm.immediate ) {
                vm.imm_onerr = 1;
            }
            break;
        case OP_RESUME:
        case OP_RESUMENEXT:
            SYNC();
            if ( !vm.in_handler ) {
                vm_error( E_RESUME_NO_ERR );
            }
            frames_unwind( vm.err_nframes );
            pc = code + (op == OP_RESUME ? prog->locs[vm.err_stmt].pc : prog->locs[vm.err_stmt].end_pc);
            vm.in_handler = 0;
            vm.err = 0;
            sp = vm.sp = vm.stmt_sp;
            break;
        case OP_RESUMETO:
            a32 = FETCH32();
            SYNC();
            if ( !vm.in_handler ) {
                vm_error( E_RESUME_NO_ERR );
            }
            frames_unwind( 0 );
            vm.ngosub = 0;
            pc = code + a32;
            vm.in_handler = 0;
            vm.err = 0;
            /* the strings the statements left pending go too */
            for ( sp = vm.stack; sp < vm.sp; sp++ ) {
                if ( vm.stag[sp - vm.stack] ) {
                    qs_release( sp->str );
                    vm.stag[sp - vm.stack] = 0;
                }
            }
            sp = vm.sp = vm.stack;
            break;
        case OP_NOP:
            break;
        case OP_IMMRET:
            SYNC();
            return RUN_IMMEDIATE;
        case OP_IMMGOTO:
            a32 = FETCH32();
            vm.pc = code + a32;
            vm.sp = sp;
            return RUN_IMMGOTO;
        case OP_IMMRETURN:
            SYNC();
            vm.pc = gosub_pop();
            return RUN_IMMGOTO;
        default:
            FAIL( E_INTERNAL );
        }
    }
}

/* the strings the failed statement had on the stack, let go of */
static void drop_statement_cells( void )
{
    CELL *cell, *from = vm.stmt_sp;

    if ( from < vm.stack || from > vm.sp ) {
        from = vm.stack;
    }
    for ( cell = from; cell < vm.sp; cell++ ) {
        if ( vm.stag[cell - vm.stack] ) {
            qs_release( cell->str );
            vm.stag[cell - vm.stack] = 0;
        }
    }
    vm.sp = from;
}

int vm_go( int how )
{
    int why;

    if ( vm.dead ) {
        vm.fatal = E_OUT_OF_MEMORY;
        vm.fatal_stmt = vm.stmt;
        vm.fatal_data = NULL;
        return RUN_ERROR;
    }
    vm.go = how;
    vm.step_frames = vm.nframes;
    vm.stopped = -1;
    vm.poll = how == GO_RUN ? 1 : 0;
    for ( ;; ) {
        if ( rt_setjmp( &vm_jmp ) == 0 ) {
            why = run_loop();
            con_flush();
            return why;
        }
        /* vm_error() came back here */
        drop_statement_cells();
        if ( vm_err_code == ERR_BREAK ) {
            vm.pc = vm.code + vm.prog->locs[vm.stmt].pc;
            vm.skip_stop = 1;
            con_flush();
            return RUN_BREAK;
        }
        if ( vm_err_stmt_override != 0xFFFFFFFFUL ) {
            vm.stmt = vm_err_stmt_override;
        }
        /* "Out of stack space" is not trapped, as in QBasic: the handler
           would want stack too */
        if ( vm.on_error != NULL && !vm.in_handler && vm_err_code != E_OUT_OF_STACK ) {
            vm.err = vm_err_code;
            vm.erl = vm.prog->locs[vm.stmt].lineno;
            vm.err_stmt = vm.stmt;
            vm.err_nframes = vm.nframes;
            vm.in_handler = 1;
            vm.pc = vm.on_error;
            continue;
        }
        vm.fatal = vm_err_code;
        vm.fatal_stmt = vm.stmt;
        vm.fatal_data = vm_err_data;
        vm.err = vm_err_code;
        vm.pc = vm.code + vm.prog->locs[vm.stmt].pc;
        vm.skip_stop = 1;
        con_flush();
        return RUN_ERROR;
    }
}

/* An Immediate line, compiled into the program at "entry", run where the
   program stopped (or in the program made ready, or ended); arrays from
   "new_arrays" on are the line's own.  -> RUN_IMMEDIATE when the line is
   done, RUN_IMMGOTO when it said GOTO or RETURN (vm.pc is where the
   program goes on), -1 for no memory, or how the run ended.  The
   program is left as it was, but for what the line changed - and an ON
   ERROR the line set stands. */
int vm_immediate( u32 entry, u32 new_arrays )
{
    u8 *pc = vm.pc, *on_error = vm.on_error, *fdata = vm.fdata, *sdata = vm.sdata, *goes_on;
    CELL *sp = vm.sp, *stmt_sp = vm.stmt_sp, *cell;
    u32 stmt = vm.stmt, nframes = vm.nframes, ngosub = vm.ngosub;
    int skip_stop = vm.skip_stop, in_handler = vm.in_handler, err = vm.err, in_event = vm.in_event;
    s32 erl = vm.erl;
    u32 clears = vm.clears;
    int why;

    if ( !make_arrays( new_arrays ) ) {
        return -1;
    }
    vm.pc = vm.code + entry;
    vm.on_error = NULL;                         /* its errors are its own */
    vm.in_handler = 0;
    vm.skip_stop = 0;
    vm.immediate = 1;
    vm.imm_onerr = 0;
    why = vm_go( GO_RUN );
    vm.immediate = 0;
    if ( why == RUN_END || why == RUN_SYSTEM || why == RUN_CHAIN ) {
        return why;
    }
    if ( vm.clears != clears ) {
        /* CLEAR took away the stack and variables the stopped program
           had: nothing to put back (the caller ends the program) */
        return why;
    }
    goes_on = vm.pc;
    frames_unwind( nframes );
    for ( cell = sp; cell < vm.sp; cell++ ) {
        if ( vm.stag[cell - vm.stack] ) {
            qs_release( cell->str );
            vm.stag[cell - vm.stack] = 0;
        }
    }
    vm.sp = sp;
    vm.stmt_sp = stmt_sp;
    vm.fdata = fdata;
    vm.sdata = sdata;
    if ( !vm.imm_onerr ) {
        vm.on_error = on_error;
    }
    vm.in_handler = in_handler;
    vm.err = err;
    vm.erl = erl;
    vm.in_event = in_event;
    vm.stmt = stmt;                             /* good until the next statement */
    if ( why == RUN_IMMGOTO ) {
        /* the program goes on from the label, or from its GOSUB (RETURN
           took the GOSUB off) */
        vm.pc = goes_on;
        vm.skip_stop = 1;
        return why;
    }
    vm.pc = pc;
    if ( vm.ngosub > ngosub ) {
        vm.ngosub = ngosub;
    }
    vm.skip_stop = skip_stop;
    return why;
}

/* END: the files closed, the procedures left, the variables kept - the
   Immediate window can still read them */
void vm_finish( void )
{
    CELL *cell;

    for ( cell = vm.stack; cell < vm.sp; cell++ ) {
        if ( vm.stag[cell - vm.stack] ) {
            qs_release( cell->str );
            vm.stag[cell - vm.stack] = 0;
        }
    }
    vm.sp = vm.stack;
    vm.stmt_sp = vm.stack;
    frames_unwind( 0 );
    arr_unpark();
    vm.ngosub = 0;
    file_close_all();
    events_reset();
    snd_stop();
    vm.on_error = NULL;
    vm.in_handler = 0;
    vm.sdata = vm.gdata;
    vm.fdata = NULL;
    vm.skip_stop = 0;
}

/* Ctrl+Break, found by whoever reads the keys */
void vm_break( void )
{
    vm_err_code = ERR_BREAK;
    vm_err_stmt_override = 0xFFFFFFFFUL;
    vm_err_data = NULL;
    rt_longjmp( &vm_jmp, 1 );
}

void vm_poll_now( void )
{
    vm.poll = 0;
}

/* ------------------------------------------------------------------ */
/* the routines' table                                                 */
/* ------------------------------------------------------------------ */

void (*const rtf_table[RT_COUNT])( void ) = {
#define RT( name ) rtf_##name,
#include "rtfn.h"
#undef RT
};

/* ------------------------------------------------------------------ */
/* errors as sentences                                                 */
/* ------------------------------------------------------------------ */

static const struct {
    u8 code;
    const char *text;
} err_texts[] = {
    {  1, "NEXT without FOR" },         {  2, "Syntax error" },
    {  3, "RETURN without GOSUB" },     {  4, "Out of DATA" },
    {  5, "Illegal function call" },    {  6, "Overflow" },
    {  7, "Out of memory" },            {  8, "Label not defined" },
    {  9, "Subscript out of range" },   { 10, "Duplicate definition" },
    { 11, "Division by zero" },         { 12, "Illegal in direct mode" },
    { 13, "Type mismatch" },            { 14, "Out of string space" },
    { 16, "String formula too complex" }, { 17, "Cannot continue" },
    { 18, "Function not defined" },     { 19, "No RESUME" },
    { 20, "RESUME without error" },     { 24, "Device timeout" },
    { 25, "Device fault" },             { 26, "FOR without NEXT" },
    { 27, "Out of paper" },             { 28, "Out of stack space" },
    { 29, "WHILE without WEND" },       { 30, "WEND without WHILE" },
    { 33, "Duplicate label" },          { 35, "Subprogram not defined" },
    { 37, "Argument-count mismatch" },  { 38, "Array not defined" },
    { 40, "Variable required" },        { 50, "FIELD overflow" },
    { 51, "Internal error" },           { 52, "Bad file name or number" },
    { 53, "File not found" },           { 54, "Bad file mode" },
    { 55, "File already open" },        { 56, "FIELD statement active" },
    { 57, "Device I/O error" },         { 58, "File already exists" },
    { 59, "Bad record length" },        { 61, "Disk full" },
    { 62, "Input past end of file" },   { 63, "Bad record number" },
    { 64, "Bad file name" },            { 67, "Too many files" },
    { 68, "Device unavailable" },       { 69, "Communication-buffer overflow" },
    { 70, "Permission denied" },        { 71, "Disk not ready" },
    { 72, "Disk-media error" },         { 73, "Advanced feature unavailable" },
    { 74, "Rename across disks" },      { 75, "Path/File access error" },
    { 76, "Path not found" }
};

const char *err_message( int code )
{
    int i;

    for ( i = 0; i < (int)(sizeof( err_texts ) / sizeof( err_texts[0] )); i++ ) {
        if ( err_texts[i].code == code ) {
            return err_texts[i].text;
        }
    }
    return "Unprintable error";
}
