/*
 * PACK.C - a compiled program as a block of bytes, and back: what Make
 * EXE File puts behind QBRUN.EXE, and what QBRUN.EXE runs.
 *
 * THE CODE NEEDS NOTHING DONE TO IT.  Every operand the compiler writes
 * is a number - an offset into the code or the data, a constant's
 * number, a routine's number - so the bytes go out as they are, and
 * the tables beside them (the procedures, the statements, DATA, the
 * arrays made at the start, the COMMON) go out field by field.  The
 * names the Immediate window looks things up by are left behind.
 *
 * THE BLOCK:
 *
 *   +0   'Q','B','X', PACK_VER
 *   +4   the instruction set's signature: a hash of the names and
 *        shapes in OP.H and RTFN.H, so that a runtime never runs code
 *        made for another instruction set
 *   +8   the block's length, this header and the trailer included
 *   +12  PK_* flags
 *   +16  0
 *   +20  the program, little-endian 32-bit words and counted strings
 *   end  the length again, then 'Q','B','X','E': how the block is found
 *        from the end of a file (CHAIN to another program)
 *
 * The reading side checks every count against what is left of the
 * block before it allocates, so a damaged file is refused rather than
 * read past.  It does not check the code: the instructions are as
 * trusted as any other program's.
 */
#include "qb.h"

#define PACK_VER        1
#define HEAD_SIZE       20
#define TAIL_SIZE       8

/* the instruction set, written out: OP.H's and RTFN.H's names and shapes */
static const char iset[] =
#define OP( name, shape )   #name shape ";"
#include "op.h"
#undef OP
#define RT( name )          #name ";"
#include "rtfn.h"
#undef RT
    "";

static uint32_t iset_sig( void )
{
    uint32_t hash = 0x811C9DC5UL;       /* FNV-1a */
    const char *scan;

    for ( scan = iset; *scan; scan++ ) {
        hash = (hash ^ (uint8_t)*scan) * 0x01000193UL;
    }
    return hash;
}

static uint32_t get32( const uint8_t *at )
{
    return (uint32_t)at[0] | ((uint32_t)at[1] << 8) | ((uint32_t)at[2] << 16) | ((uint32_t)at[3] << 24);
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t     *buf;
    unsigned int len, cap;
    int          failed;
} OUTBUF;

static void put_bytes( OUTBUF *out, const void *src, unsigned int len )
{
    uint8_t *grown;
    unsigned int cap;

    if ( out->failed || len == 0 ) {
        return;
    }
    if ( out->len + len < out->len || out->len + len > 0x40000000UL ) {
        out->failed = 1;                        /* never a program's size */
        return;
    }
    if ( out->len + len > out->cap ) {
        cap = out->cap ? out->cap : 4096;
        while ( cap < out->len + len ) {
            cap *= 2;
        }
        grown = (uint8_t *)try_realloc( out->buf, cap );
        if ( grown == NULL ) {
            out->failed = 1;
            return;
        }
        out->buf = grown;
        out->cap = cap;
    }
    mem_cpy( out->buf + out->len, src, len );
    out->len += len;
}

static void put32( OUTBUF *out, uint32_t val )
{
    uint8_t bytes[4];

    bytes[0] = (uint8_t)val;
    bytes[1] = (uint8_t)(val >> 8);
    bytes[2] = (uint8_t)(val >> 16);
    bytes[3] = (uint8_t)(val >> 24);
    put_bytes( out, bytes, 4 );
}

static void put_text( OUTBUF *out, const char *text, unsigned int len )
{
    put32( out, len );
    put_bytes( out, text, len );
}

static void put_list( OUTBUF *out, const unsigned int *list, unsigned int count )
{
    unsigned int i;

    put32( out, count );
    for ( i = 0; i < count; i++ ) {
        put32( out, list[i] );
    }
}

static void put_proc( OUTBUF *out, const PROC *proc )
{
    int i;
    unsigned int n;

    put_text( out, proc->name, str_len( proc->name ) );
    put32( out, proc->kind );
    put32( out, proc->is_static );
    put32( out, proc->defined );
    put32( out, proc->declared );
    put32( out, proc->rettype );
    put32( out, (uint32_t)proc->nparams );
    for ( i = 0; i < proc->nparams; i++ ) {
        put32( out, proc->params[i].type );
        put32( out, proc->params[i].array );
        put32( out, proc->params[i].ndims );
    }
    put32( out, proc->entry );
    put32( out, proc->frame_size );
    put32( out, proc->static_size );
    put32( out, proc->ret_off );
    put_list( out, proc->rel_str, proc->nrel_str );
    put_list( out, proc->rel_arr, proc->nrel_arr );
    put_list( out, proc->srel_str, proc->nsrel_str );
    put_list( out, proc->srel_arr, proc->nsrel_arr );
    put32( out, proc->nimp_arr );
    for ( n = 0; n < proc->nimp_arr; n++ ) {
        put32( out, proc->imp_arr[n] );
        put32( out, proc->imp_type[n] );
        put32( out, proc->imp_dims[n] );
    }
    put32( out, (uint32_t)proc->text );
    put32( out, proc->line );
    put32( out, proc->first_stmt );
}

/* the program as a block -> xfree() it; NULL when memory ran out */
uint8_t *pack_program( PROGRAM *prog, uint32_t flags, const char *module, unsigned int *len )
{
    OUTBUF out;
    unsigned int i, n;
    int k;
    uint8_t ver = PACK_VER;

    mem_set( &out, 0, sizeof( out ) );
    put_bytes( &out, "QBX", 3 );
    put_bytes( &out, &ver, 1 );
    put32( &out, iset_sig() );
    put32( &out, 0 );                           /* the length, once known */
    put32( &out, flags );
    put32( &out, 0 );
    put_text( &out, module, str_len( module ) );

    put32( &out, prog->code_len );
    put_bytes( &out, prog->code, prog->code_len );
    put32( &out, prog->nconsts );
    for ( i = 0; i < prog->nconsts; i++ ) {
        put_text( &out, QS_TEXT( prog->consts[i] ), QS_LEN( prog->consts[i] ) );
    }
    put32( &out, prog->nlocs );
    for ( i = 0; i < prog->nlocs; i++ ) {
        const STMTLOC *loc = &prog->locs[i];
        put32( &out, loc->pc );
        put32( &out, loc->end_pc );
        put32( &out, loc->line );
        put32( &out, loc->text );
        put32( &out, loc->col );
        put32( &out, loc->len );
        put32( &out, (uint32_t)loc->lineno );
        put32( &out, loc->proc );
    }
    put32( &out, (uint32_t)prog->nprocs );
    for ( k = 0; k < prog->nprocs; k++ ) {
        put_proc( &out, &prog->procs[k] );
    }
    put32( &out, (uint32_t)prog->nrecs );
    for ( k = 0; k < prog->nrecs; k++ ) {
        const RECDEF *rec = &prog->recs[k];
        put_text( &out, rec->name, str_len( rec->name ) );
        put32( &out, rec->size );
        put32( &out, (uint32_t)rec->nfields );
        for ( n = 0; n < (unsigned int)rec->nfields; n++ ) {
            put_text( &out, rec->fields[n].name, str_len( rec->fields[n].name ) );
            put32( &out, rec->fields[n].type );
            put32( &out, rec->fields[n].offset );
        }
    }
    put32( &out, prog->gsize );
    put_list( &out, prog->grel_str, prog->ngrel_str );
    put_list( &out, prog->grel_arr, prog->ngrel_arr );
    put32( &out, prog->narrinit );
    for ( i = 0; i < prog->narrinit; i++ ) {
        const ARRINIT *init = &prog->arrinit[i];
        put32( &out, init->scope );
        put32( &out, init->proc );
        put32( &out, init->offset );
        put32( &out, init->type );
        put32( &out, init->esize );
        put32( &out, init->ndims );
        put32( &out, init->flags );
        for ( n = 0; n < (unsigned int)init->ndims * 2; n++ ) {
            put32( &out, (uint32_t)init->bounds[n] );
        }
    }
    put32( &out, prog->ncommon );
    for ( i = 0; i < prog->ncommon; i++ ) {
        put32( &out, prog->common[i].offset );
        put32( &out, prog->common[i].type );
        put32( &out, prog->common[i].size );
        put32( &out, prog->common[i].is_array );
    }
    put32( &out, prog->ndata );
    for ( i = 0; i < prog->ndata; i++ ) {
        const DATAITEM *item = &prog->data[i];
        put_text( &out, item->text, item->len );
        put32( &out, item->quoted );
        put32( &out, item->in_text );
        put32( &out, item->col );
        put32( &out, item->line );
    }
    put32( &out, prog->main_entry );
    put32( &out, prog->uses_events );
    put32( &out, (uint32_t)prog->option_base );

    put32( &out, out.len + TAIL_SIZE );
    put_bytes( &out, "QBXE", 4 );
    if ( out.failed ) {
        xfree( out.buf );
        return NULL;
    }
    n = out.len;
    out.buf[8] = (uint8_t)n;
    out.buf[9] = (uint8_t)(n >> 8);
    out.buf[10] = (uint8_t)(n >> 16);
    out.buf[11] = (uint8_t)(n >> 24);
    *len = n;
    return out.buf;
}

/* Is this file (whole, in memory) a QBRUN.EXE that runs what this QBASIC
   makes?  Its image holds the same instruction set, written out, as
   this program does, and nothing has been put behind it yet. */
int pack_runtime_ok( const uint8_t *file, unsigned int len )
{
    unsigned int size = sizeof( iset ) - 1, at;

    if ( len < size + TAIL_SIZE || pack_tail_length( file + len - TAIL_SIZE ) != 0 ) {
        return 0;
    }
    for ( at = 0; at + size <= len; at++ ) {
        if ( file[at] == (uint8_t)iset[0] && mem_cmp( file + at, iset, size ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

/* The first bytes of a block: its whole length, or 0 when they are not
   the start of one this runtime can run */
uint32_t pack_length( const uint8_t *head, unsigned int avail )
{
    uint32_t len;

    if ( avail < HEAD_SIZE || head[0] != 'Q' || head[1] != 'B' || head[2] != 'X' || head[3] != PACK_VER
      || get32( head + 4 ) != iset_sig() ) {
        return 0;
    }
    len = get32( head + 8 );
    return len >= HEAD_SIZE + TAIL_SIZE ? len : 0;
}

/* The last eight bytes of a file: the length of the block they end, or
   0 when the file does not end with one */
uint32_t pack_tail_length( const uint8_t *tail )
{
    if ( tail[4] != 'Q' || tail[5] != 'B' || tail[6] != 'X' || tail[7] != 'E' ) {
        return 0;
    }
    return get32( tail );
}

typedef struct {
    const uint8_t *at;
    unsigned int left;
    int bad;
} INBUF;

static uint32_t get( INBUF *in )
{
    uint32_t val;

    if ( in->bad || in->left < 4 ) {
        in->bad = 1;
        return 0;
    }
    val = get32( in->at );
    in->at += 4;
    in->left -= 4;
    return val;
}

/* a count of things at least "least" bytes each, there to be read */
static unsigned int get_count( INBUF *in, unsigned int least )
{
    unsigned int count = get( in );

    if ( least && count > in->left / least ) {
        in->bad = 1;
        return 0;
    }
    return count;
}

/* room for "count" things of "size" bytes, zeroed; NULL (and *nomem)
   when there is none.  An empty table is NULL and no failure. */
static void *get_room( INBUF *in, unsigned int count, unsigned int size, int *nomem )
{
    void *mem;

    if ( in->bad || count == 0 ) {
        return NULL;
    }
    if ( count > 0xFFFFFFFFUL / size ) {
        in->bad = 1;
        return NULL;
    }
    mem = try_alloc( count * size );
    if ( mem == NULL ) {
        *nomem = 1;
        in->bad = 1;
        return NULL;
    }
    mem_set( mem, 0, count * size );
    return mem;
}

/* a counted string into a buffer of "max" bytes, ended */
static void get_name( INBUF *in, char *buf, unsigned int max )
{
    unsigned int len = get_count( in, 1 );

    if ( in->bad || len >= max ) {
        in->bad = 1;
        buf[0] = 0;
        return;
    }
    mem_cpy( buf, in->at, len );
    buf[len] = 0;
    in->at += len;
    in->left -= len;
}

static unsigned int *get_list( INBUF *in, unsigned int *count, int *nomem )
{
    unsigned int *list, i;

    *count = get_count( in, 4 );
    list = (unsigned int *)get_room( in, *count, 4, nomem );
    for ( i = 0; list != NULL && i < *count; i++ ) {
        list[i] = get( in );
    }
    return list;
}

static void get_proc( INBUF *in, PROC *proc, unsigned int code_len, int *nomem )
{
    unsigned int n;
    int i;

    get_name( in, proc->name, sizeof( proc->name ) );
    proc->kind = (uint8_t)get( in );
    proc->is_static = (uint8_t)get( in );
    proc->defined = (uint8_t)get( in );
    proc->declared = (uint8_t)get( in );
    proc->rettype = get( in );
    proc->nparams = (int)get_count( in, 12 );
    proc->params = (PARAM *)get_room( in, (unsigned int)proc->nparams, sizeof( PARAM ), nomem );
    for ( i = 0; proc->params != NULL && i < proc->nparams; i++ ) {
        proc->params[i].type = get( in );
        proc->params[i].array = (uint8_t)get( in );
        proc->params[i].ndims = (uint8_t)get( in );
    }
    proc->entry = get( in );
    proc->frame_size = get( in );
    proc->static_size = get( in );
    proc->ret_off = get( in );
    proc->rel_str = get_list( in, &proc->nrel_str, nomem );
    proc->rel_arr = get_list( in, &proc->nrel_arr, nomem );
    proc->srel_str = get_list( in, &proc->nsrel_str, nomem );
    proc->srel_arr = get_list( in, &proc->nsrel_arr, nomem );
    proc->nimp_arr = get_count( in, 12 );
    proc->imp_arr = (unsigned int *)get_room( in, proc->nimp_arr, 4, nomem );
    proc->imp_type = (QTYPE *)get_room( in, proc->nimp_arr, sizeof( QTYPE ), nomem );
    proc->imp_dims = (uint8_t *)get_room( in, proc->nimp_arr, 1, nomem );
    for ( n = 0; proc->imp_dims != NULL && n < proc->nimp_arr; n++ ) {
        proc->imp_arr[n] = get( in );
        proc->imp_type[n] = get( in );
        proc->imp_dims[n] = (uint8_t)get( in );
    }
    proc->text = (int)get( in );
    proc->line = get( in );
    proc->first_stmt = get( in );
    if ( proc->entry >= code_len ) {
        in->bad = 1;
    }
}

void pack_free( PROGRAM *prog )
{
    unsigned int i;
    int k;

    if ( prog == NULL ) {
        return;
    }
    /* a constant still held (the COMMON CHAIN carries, for one) is left
       to its holder, as COMP.C leaves it: its count never reaches 0 */
    for ( i = 0; prog->consts != NULL && i < prog->nconsts; i++ ) {
        if ( prog->consts[i] != NULL && prog->consts[i]->ref == QS_PERM ) {
            xfree( prog->consts[i] );
        }
    }
    xfree( prog->consts );
    for ( k = 0; prog->procs != NULL && k < prog->nprocs; k++ ) {
        PROC *proc = &prog->procs[k];
        xfree( proc->params );
        xfree( proc->rel_str );
        xfree( proc->rel_arr );
        xfree( proc->srel_str );
        xfree( proc->srel_arr );
        xfree( proc->imp_arr );
        xfree( proc->imp_type );
        xfree( proc->imp_dims );
    }
    xfree( prog->procs );
    for ( k = 0; prog->recs != NULL && k < prog->nrecs; k++ ) {
        xfree( prog->recs[k].fields );
    }
    xfree( prog->recs );
    for ( i = 0; prog->arrinit != NULL && i < prog->narrinit; i++ ) {
        xfree( prog->arrinit[i].bounds );
    }
    xfree( prog->arrinit );
    for ( i = 0; prog->data != NULL && i < prog->ndata; i++ ) {
        xfree( prog->data[i].text );
    }
    xfree( prog->data );
    xfree( prog->code );
    xfree( prog->locs );
    xfree( prog->grel_str );
    xfree( prog->grel_arr );
    xfree( prog->common );
    xfree( prog );
}

/* A block back into a program -> it (pack_free() it), with its PK_*
   flags and the name of its module; NULL and *rc = PK_NOTONE for a block
   that is not one or is damaged, PK_NOMEM for no memory */
PROGRAM *pack_read( const uint8_t *blob, unsigned int len, uint32_t *flags, char *module, unsigned int module_max, int *rc )
{
    INBUF in;
    PROGRAM *prog;
    uint32_t total;
    unsigned int i, n, area = 0;
    int k, nomem = 0;

    total = pack_length( blob, len );
    if ( total == 0 || total > len ) {
        *rc = PK_NOTONE;
        return NULL;
    }
    in.at = blob + HEAD_SIZE;
    in.left = total - HEAD_SIZE - TAIL_SIZE;
    in.bad = 0;
    *flags = get32( blob + 12 );
    prog = (PROGRAM *)try_alloc( sizeof( PROGRAM ) );
    if ( prog == NULL ) {
        *rc = PK_NOMEM;
        return NULL;
    }
    mem_set( prog, 0, sizeof( *prog ) );
    get_name( &in, module, module_max );

    prog->code_len = get_count( &in, 1 );
    prog->code = (uint8_t *)get_room( &in, prog->code_len, 1, &nomem );
    if ( prog->code != NULL ) {
        mem_cpy( prog->code, in.at, prog->code_len );
        in.at += prog->code_len;
        in.left -= prog->code_len;
    }
    prog->nconsts = get_count( &in, 4 );
    prog->consts = (STR **)get_room( &in, prog->nconsts, sizeof( STR * ), &nomem );
    for ( i = 0; prog->consts != NULL && i < prog->nconsts && !in.bad; i++ ) {
        n = get_count( &in, 1 );
        if ( n > STR_MAX ) {
            in.bad = 1;
        } else if ( n > 0 && !in.bad ) {
            prog->consts[i] = qs_perm( (const char *)in.at, n );
            if ( prog->consts[i] == NULL ) {
                nomem = 1;
                in.bad = 1;
            }
            in.at += n;
            in.left -= n;
        }
    }
    prog->nlocs = get_count( &in, 32 );
    prog->locs = (STMTLOC *)get_room( &in, prog->nlocs, sizeof( STMTLOC ), &nomem );
    for ( i = 0; prog->locs != NULL && i < prog->nlocs; i++ ) {
        STMTLOC *loc = &prog->locs[i];
        loc->pc = get( &in );
        loc->end_pc = get( &in );
        loc->line = get( &in );
        loc->text = (uint16_t)get( &in );
        loc->col = (uint16_t)get( &in );
        loc->len = (uint16_t)get( &in );
        loc->lineno = (int32_t)get( &in );
        loc->proc = (uint16_t)get( &in );
        if ( loc->pc > prog->code_len || loc->end_pc > prog->code_len ) {
            in.bad = 1;
        }
    }
    prog->nprocs = (int)get_count( &in, 64 );
    prog->procs = (PROC *)get_room( &in, (unsigned int)prog->nprocs, sizeof( PROC ), &nomem );
    for ( k = 0; prog->procs != NULL && k < prog->nprocs && !in.bad; k++ ) {
        get_proc( &in, &prog->procs[k], prog->code_len, &nomem );
    }
    prog->nrecs = (int)get_count( &in, 12 );
    prog->recs = (RECDEF *)get_room( &in, (unsigned int)prog->nrecs, sizeof( RECDEF ), &nomem );
    for ( k = 0; prog->recs != NULL && k < prog->nrecs && !in.bad; k++ ) {
        RECDEF *rec = &prog->recs[k];
        get_name( &in, rec->name, sizeof( rec->name ) );
        rec->size = get( &in );
        rec->nfields = (int)get_count( &in, 12 );
        rec->fields = (struct RFIELD *)get_room( &in, (unsigned int)rec->nfields, sizeof( struct RFIELD ), &nomem );
        for ( n = 0; rec->fields != NULL && n < (unsigned int)rec->nfields; n++ ) {
            get_name( &in, rec->fields[n].name, sizeof( rec->fields[n].name ) );
            rec->fields[n].type = get( &in );
            rec->fields[n].offset = get( &in );
        }
    }
    prog->gsize = get( &in );
    prog->grel_str = get_list( &in, &prog->ngrel_str, &nomem );
    prog->grel_arr = get_list( &in, &prog->ngrel_arr, &nomem );
    prog->narrinit = get_count( &in, 28 );
    prog->arrinit = (ARRINIT *)get_room( &in, prog->narrinit, sizeof( ARRINIT ), &nomem );
    for ( i = 0; prog->arrinit != NULL && i < prog->narrinit && !in.bad; i++ ) {
        ARRINIT *init = &prog->arrinit[i];
        init->scope = (uint8_t)get( &in );
        init->proc = (uint16_t)get( &in );
        init->offset = get( &in );
        init->type = get( &in );
        init->esize = get( &in );
        init->ndims = (uint8_t)get( &in );
        init->flags = (uint8_t)get( &in );
        /* the run writes the array's pointer at "offset": it has to be
           in the data it names */
        if ( init->ndims == 0 || init->ndims > MAX_DIMS ) {
            in.bad = 1;
        } else if ( init->scope == SC_GLOBAL ) {
            area = prog->gsize;
        } else if ( init->scope == SC_STATIC && (int)init->proc < prog->nprocs && prog->procs != NULL ) {
            area = prog->procs[init->proc].static_size;
        } else {
            in.bad = 1;
        }
        if ( in.bad || area < 4 || init->offset > area - 4 ) {
            in.bad = 1;
            break;
        }
        init->bounds = (int32_t *)get_room( &in, (unsigned int)init->ndims * 2, 4, &nomem );
        for ( n = 0; init->bounds != NULL && n < (unsigned int)init->ndims * 2; n++ ) {
            init->bounds[n] = (int32_t)get( &in );
        }
    }
    prog->ncommon = get_count( &in, 16 );
    prog->common = (COMMONVAR *)get_room( &in, prog->ncommon, sizeof( COMMONVAR ), &nomem );
    for ( i = 0; prog->common != NULL && i < prog->ncommon; i++ ) {
        prog->common[i].offset = get( &in );
        prog->common[i].type = get( &in );
        prog->common[i].size = get( &in );
        prog->common[i].is_array = (uint8_t)get( &in );
    }
    prog->ndata = get_count( &in, 20 );
    prog->data = (DATAITEM *)get_room( &in, prog->ndata, sizeof( DATAITEM ), &nomem );
    for ( i = 0; prog->data != NULL && i < prog->ndata && !in.bad; i++ ) {
        DATAITEM *item = &prog->data[i];
        n = get_count( &in, 1 );
        if ( n > 0xFFFF ) {
            in.bad = 1;
            break;
        }
        item->text = (char *)get_room( &in, n + 1, 1, &nomem );
        if ( item->text != NULL ) {
            mem_cpy( item->text, in.at, n );
            in.at += n;
            in.left -= n;
        }
        item->len = (uint16_t)n;
        item->quoted = (uint8_t)get( &in );
        item->in_text = (uint16_t)get( &in );
        item->col = (uint16_t)get( &in );
        item->line = get( &in );
    }
    prog->main_entry = get( &in );
    prog->uses_events = (uint8_t)get( &in );
    prog->option_base = (int)get( &in );
    if ( in.bad || in.left != 0 || prog->code_len == 0 || prog->main_entry >= prog->code_len ) {
        pack_free( prog );
        *rc = nomem ? PK_NOMEM : PK_NOTONE;
        return NULL;
    }
    *rc = 0;
    return prog;
}
