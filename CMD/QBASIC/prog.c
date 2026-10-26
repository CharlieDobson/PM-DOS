/*
 * PROG.C - a program's text: the module and its procedures, loaded from
 * a file and saved back to one.
 *
 * QBASIC SHOWS EACH PROCEDURE ON ITS OWN, so a file is cut up as it is
 * read: SUB or FUNCTION to its END starts a procedure's text, and the
 * remarks, blank lines and DEFtype statements just above a SUB go with
 * it, because that is where QBasic puts them when it saves.  A
 * procedure in a file whose module has DEFINT A-Z (or any other DEFtype)
 * before it is given those statements at its top, as QBasic does, so
 * that each text says by itself what its unsuffixed names are.
 *
 * SAVING puts the module first and then the procedures in the order of
 * their names, and adds a DECLARE at the top of the module for every
 * procedure that has none.
 */
#include "qb.h"

PROGTEXT ptext;

static void free_texts( void )
{
    int i;

    for ( i = 0; i < ptext.ntexts; i++ ) {
        doc_free( ptext.texts[i] );
        ptext.texts[i] = NULL;
    }
    ptext.ntexts = 0;
}

void prog_clear( void )
{
    /* the empty text first: without one the old program stays */
    DOC *fresh = doc_new();

    if ( fresh == NULL ) {
        out_of_memory();
        return;
    }
    free_texts();
    ptext.texts[0] = fresh;
    ptext.ntexts = 1;
    ptext.path[0] = 0;
    str_cpy( ptext.name, "Untitled" );
    ptext.modified = 0;
}

/* ------------------------------------------------------------------ */
/* reading a line's shape                                              */
/* ------------------------------------------------------------------ */

static TOKEN line_toks[256];

/* the first statement's tokens, after a label or line number */
static TOKEN *first_tokens( const char *text, u32 len, int *count )
{
    int col;
    const char *msg;
    int n = lex_line( text, len, line_toks, 256, &col, &msg );

    if ( n < 1 ) {
        *count = 0;
        return NULL;
    }
    *count = n;
    n = 0;
    if ( line_toks[0].kind == TK_LINENUM ) {
        n++;
    }
    if ( line_toks[n].kind == TK_LABEL ) {
        n++;
    }
    *count -= n;
    return line_toks + n;
}

/* SUB or FUNCTION starting a procedure: its name and kind */
int prog_line_proc( const char *text, u32 len, char *name, int *kind )
{
    int count, i;
    TOKEN *tok = first_tokens( text, len, &count );

    if ( tok == NULL || count < 2 || tok[0].kind != TK_KW
      || (tok[0].kw != KW_SUB && tok[0].kw != KW_FUNCTION) || tok[1].kind != TK_ID ) {
        return 0;
    }
    *kind = tok[0].kw == KW_SUB ? PK_SUB : PK_FUNC;
    for ( i = 0; i < tok[1].len && i < NAME_MAX + 1; i++ ) {
        name[i] = text[tok[1].col + i];
    }
    name[i] = 0;
    return 1;
}

/* END SUB or END FUNCTION */
static int is_proc_end( const char *text, u32 len )
{
    int count;
    TOKEN *tok = first_tokens( text, len, &count );

    return tok != NULL && count >= 2 && tok[0].kind == TK_KW && tok[0].kw == KW_END
        && tok[1].kind == TK_KW && (tok[1].kw == KW_SUB || tok[1].kw == KW_FUNCTION);
}

/* a remark, a blank line or a DEFtype statement: what goes with the
   procedure below it.  -> 2 for a DEFtype statement, 1 for the others */
static int is_lead_line( const char *text, u32 len, u8 *deftype )
{
    int count, i, from, to, type;
    TOKEN *tok = first_tokens( text, len, &count );

    if ( tok == NULL || tok[0].kind == TK_EOL || tok[0].kind == TK_REM ) {
        return tok != NULL && (line_toks[0].kind != TK_LINENUM && line_toks[0].kind != TK_LABEL);
    }
    if ( tok[0].kind != TK_KW ) {
        return 0;
    }
    switch ( tok[0].kw ) {
    case KW_DEFINT: type = TY_INT; break;
    case KW_DEFLNG: type = TY_LONG; break;
    case KW_DEFSNG: type = TY_SINGLE; break;
    case KW_DEFDBL: type = TY_DOUBLE; break;
    case KW_DEFSTR: type = TY_STR; break;
    default:
        return 0;
    }
    /* follow it, to know what the procedures after it inherit */
    for ( i = 1; i < count && tok[i].kind == TK_ID; ) {
        from = ch_upper( text[tok[i].col] ) - 'A';
        to = from;
        i++;
        if ( i + 1 < count && tok[i].kind == TK_MINUS && tok[i + 1].kind == TK_ID ) {
            to = ch_upper( text[tok[i + 1].col] ) - 'A';
            i += 2;
        }
        if ( from >= 0 && to <= 25 && from <= to ) {
            for ( ; from <= to; from++ ) {
                deftype[from] = (u8)type;
            }
        }
        if ( i < count && tok[i].kind == TK_COMMA ) {
            i++;
        } else {
            break;
        }
    }
    return 2;
}

/* DEFINT A-Z and the like: the statements that make "deftype" from all
   SINGLE, a type at a time */
static int add_deflines( DOC *doc, u32 at, const u8 *deftype )
{
    static const char *const kws[] = { "", "DEFINT", "DEFLNG", "", "DEFDBL", "DEFSTR" };
    char line[128];
    int type, letter, start, added = 0, len;

    for ( type = TY_INT; type <= TY_STR; type++ ) {
        if ( type == TY_SINGLE ) {
            continue;
        }
        len = 0;
        for ( letter = 0; letter < 26; ) {
            if ( deftype[letter] != type ) {
                letter++;
                continue;
            }
            start = letter;
            while ( letter < 26 && deftype[letter] == type ) {
                letter++;
            }
            if ( len == 0 ) {
                str_cpy( line, kws[type] );
                len = (int)str_len( line );
                line[len++] = ' ';
            } else {
                line[len++] = ',';
                line[len++] = ' ';
            }
            line[len++] = (char)('A' + start);
            if ( letter - 1 > start ) {
                line[len++] = '-';
                line[len++] = (char)('A' + letter - 1);
            }
        }
        if ( len ) {
            if ( !doc_insert_line( doc, at + (u32)added, line, (u32)len ) ) {
                return -1;
            }
            added++;
        }
    }
    return added;
}

static int defs_differ( const u8 *deftype )
{
    int i;

    for ( i = 0; i < 26; i++ ) {
        if ( deftype[i] != TY_SINGLE ) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    char **text;
    u32   *len;
    u32    count, cap;
} PENDING;

static int pending_add( PENDING *p, const char *text, u32 len )
{
    if ( p->count == p->cap ) {
        u32 cap = p->cap ? p->cap * 2 : 16;
        char **gt = (char **)try_realloc( p->text, cap * sizeof( char * ) );
        u32 *gl;
        if ( gt == NULL ) {
            return 0;
        }
        p->text = gt;
        gl = (u32 *)try_realloc( p->len, cap * sizeof( u32 ) );
        if ( gl == NULL ) {
            return 0;
        }
        p->len = gl;
        p->cap = cap;
    }
    p->text[p->count] = (char *)try_alloc( len + 1 );
    if ( p->text[p->count] == NULL ) {
        return 0;
    }
    mem_cpy( p->text[p->count], text, len );
    p->len[p->count] = len;
    p->count++;
    return 1;
}

static int pending_flush( PENDING *p, DOC *doc )
{
    u32 i;
    int ok = 1;

    for ( i = 0; i < p->count; i++ ) {
        if ( ok && !doc_append( doc, p->text[i], p->len[i] ) ) {
            ok = 0;
        }
        xfree( p->text[i] );
    }
    p->count = 0;
    return ok;
}

/* a document's first, empty line goes once something is added */
static void drop_first_empty( DOC *doc )
{
    if ( doc->count > 1 && doc->lines[0].len == 0 && doc->lines[0].text == NULL ) {
        doc_delete_lines( doc, 0, 1 );
    }
}

static DOC *new_proc_text( const char *name, int kind )
{
    DOC *doc;

    if ( ptext.ntexts > MAX_PROCS ) {
        return NULL;
    }
    doc = doc_new();
    if ( doc == NULL ) {
        return NULL;
    }
    str_cpyn( doc->pname, name, sizeof( doc->pname ) );
    doc->pkind = (u8)kind;
    ptext.texts[ptext.ntexts++] = doc;
    return doc;
}

int prog_load( const char *path )
{
    u32 handle, got, pos, len = 0;
    u8 *buf;
    char *line;
    int rc, last_cr = 0, ok = 1, state = 0, kind, at_eof = 0;
    char name[NAME_MAX + 2];
    u8 deftype[26];
    DOC *cur = NULL;
    PENDING pend;
    const char *slash;

    rc = sys_open_read( path, &handle );
    if ( rc != 0 ) {
        return rc;
    }
    buf = (u8 *)try_alloc( 8192 );
    line = (char *)try_alloc( MAX_LINE + 1 );
    if ( buf == NULL || line == NULL ) {
        xfree( buf );
        xfree( line );
        sys_close( handle );
        return -1;
    }
    prog_clear();
    mem_set( deftype, TY_SINGLE, sizeof( deftype ) );
    mem_set( &pend, 0, sizeof( pend ) );
    /* state: 0 the module, 1 inside a procedure, 2 after one */
    for ( ;; ) {
        rc = sys_read( handle, buf, 8192, &got );
        if ( rc != 0 || got == 0 ) {
            break;
        }
        for ( pos = 0; pos <= got && ok; pos++ ) {
            int ch = pos < got ? buf[pos] : -1, end_line = 0;
            if ( ch == 26 ) {
                at_eof = 1;
                break;
            }
            if ( ch < 0 ) {
                break;
            }
            if ( ch == '\n' ) {
                if ( last_cr ) {
                    last_cr = 0;
                    continue;
                }
                end_line = 1;
            } else if ( ch == '\r' ) {
                end_line = 1;
                last_cr = 1;
            } else {
                last_cr = 0;
                if ( len < MAX_LINE ) {
                    line[len++] = (char)ch;
                }
                continue;
            }
            if ( !end_line ) {
                continue;
            }
            /* a whole line: where does it go? */
            if ( state == 1 ) {
                ok = doc_append( cur, line, len );
                if ( is_proc_end( line, len ) ) {
                    state = 2;
                }
            } else if ( prog_line_proc( line, len, name, &kind ) ) {
                cur = new_proc_text( name, kind );
                if ( cur == NULL ) {
                    ok = 0;
                    break;
                }
                {
                    u32 i;
                    int has_def = 0;
                    /* the file's own DEFtype line above the procedure: no other added */
                    for ( i = 0; i < pend.count; i++ ) {
                        u8 tmp[26];
                        mem_cpy( tmp, deftype, 26 );
                        if ( is_lead_line( pend.text[i], pend.len[i], tmp ) == 2 ) {
                            has_def = 1;
                        }
                    }
                    if ( !has_def && defs_differ( deftype ) && add_deflines( cur, 1, deftype ) < 0 ) {
                        ok = 0;
                    }
                }
                ok = ok && pending_flush( &pend, cur ) && doc_append( cur, line, len );
                drop_first_empty( cur );
                state = 1;
            } else if ( is_lead_line( line, len, deftype ) ) {
                ok = pending_add( &pend, line, len );
            } else {
                ok = pending_flush( &pend, ptext.texts[0] ) && doc_append( ptext.texts[0], line, len );
                state = state == 2 ? 2 : 0;
            }
            len = 0;
        }
        if ( at_eof || !ok ) {
            break;
        }
    }
    if ( ok && len > 0 ) {
        /* the last line, with no line end after it */
        if ( state == 1 ) {
            ok = doc_append( cur, line, len );
        } else {
            ok = pending_add( &pend, line, len );
        }
    }
    if ( ok ) {
        ok = pending_flush( &pend, ptext.texts[0] );
    } else {
        pending_flush( &pend, ptext.texts[0] );
    }
    xfree( pend.text );
    xfree( pend.len );
    xfree( buf );
    xfree( line );
    sys_close( handle );
    drop_first_empty( ptext.texts[0] );
    if ( !ok || rc != 0 ) {
        prog_clear();
        return ok ? rc : -1;
    }
    for ( kind = 0; kind < ptext.ntexts; kind++ ) {
        ptext.texts[kind]->modified = 0;
    }
    str_cpyn( ptext.path, path, sizeof( ptext.path ) );
    slash = str_rchr( path, '\\' );
    if ( slash == NULL ) {
        slash = str_rchr( path, ':' );
    }
    str_cpyn( ptext.name, slash ? slash + 1 : path, sizeof( ptext.name ) );
    ptext.modified = 0;
    return 0;
}

/* ------------------------------------------------------------------ */
/* procedures                                                          */
/* ------------------------------------------------------------------ */

int prog_find( const char *name )
{
    int i;
    u32 len = str_len( name );

    for ( i = 1; i < ptext.ntexts; i++ ) {
        const char *have = ptext.texts[i]->pname;
        u32 hlen = str_len( have );
        /* the name may carry its suffix on either side */
        if ( hlen > 0 && lex_suffix_type( have[hlen - 1] ) != TY_NONE ) {
            hlen--;
        }
        if ( len > 0 && lex_suffix_type( name[len - 1] ) != TY_NONE && str_nicmp( have, name, hlen ) == 0
          && hlen == len - 1 ) {
            return i;
        }
        if ( hlen == len && str_nicmp( have, name, len ) == 0 ) {
            return i;
        }
    }
    return -1;
}

/* the module's DEFtype letters at its end, which a new procedure gets */
static void module_deftypes( u8 *deftype )
{
    DOC *doc = ptext.texts[0];
    u32 i;

    mem_set( deftype, TY_SINGLE, 26 );
    for ( i = 0; i < doc->count; i++ ) {
        is_lead_line( doc->lines[i].text ? doc->lines[i].text : "", doc->lines[i].len, deftype );
    }
}

/* a new procedure, "SUB name" and "END SUB" -> its text, -1 */
int prog_add_proc( const char *name, int kind )
{
    DOC *doc;
    char line[NAME_MAX + 16];
    u8 deftype[26];
    int added;

    if ( prog_find( name ) >= 0 ) {
        return -1;
    }
    doc = new_proc_text( name, kind );
    if ( doc == NULL ) {
        return -1;
    }
    module_deftypes( deftype );
    added = defs_differ( deftype ) ? add_deflines( doc, 0, deftype ) : 0;
    str_cpy( line, kind == PK_SUB ? "SUB " : "FUNCTION " );
    str_catn( line, name, sizeof( line ) );
    if ( added < 0 || !doc_set_line( doc, (u32)added, line, str_len( line ) )
      || !doc_append( doc, kind == PK_SUB ? "END SUB" : "END FUNCTION", kind == PK_SUB ? 7 : 12 ) ) {
        prog_delete_proc( ptext.ntexts - 1 );
        return -1;
    }
    if ( added > 0 && doc->count > (u32)added + 2 ) {
        doc_delete_lines( doc, (u32)added + 1, 1 );
    }
    ptext.modified = 1;
    return ptext.ntexts - 1;
}

void prog_delete_proc( int text )
{
    int i;

    if ( text < 1 || text >= ptext.ntexts ) {
        return;
    }
    doc_free( ptext.texts[text] );
    for ( i = text; i < ptext.ntexts - 1; i++ ) {
        ptext.texts[i] = ptext.texts[i + 1];
    }
    ptext.ntexts--;
    ptext.texts[ptext.ntexts] = NULL;
    ptext.modified = 1;
}

/* ------------------------------------------------------------------ */
/* saving                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    u32 handle;
    u8  buf[4096];
    u32 used;
    int rc;
} WRITER;

static void w_bytes( WRITER *w, const char *text, u32 len )
{
    u32 done;

    while ( len > 0 && w->rc == 0 ) {
        u32 n = sizeof( w->buf ) - w->used;
        if ( n > len ) {
            n = len;
        }
        mem_cpy( w->buf + w->used, text, n );
        w->used += n;
        text += n;
        len -= n;
        if ( w->used == sizeof( w->buf ) ) {
            w->rc = sys_write( w->handle, w->buf, w->used, &done );
            w->used = 0;
        }
    }
}

static void w_line( WRITER *w, const char *text, u32 len )
{
    w_bytes( w, text, len );
    w_bytes( w, "\r\n", 2 );
}

/* does the module already DECLARE this procedure? */
static int has_declare( const char *name, int kind )
{
    DOC *doc = ptext.texts[0];
    u32 i;
    int count;
    TOKEN *tok;
    char upper[NAME_MAX + 2], have[NAME_MAX + 2];
    u32 n;

    str_cpyn( upper, name, sizeof( upper ) );
    for ( n = 0; upper[n]; n++ ) {
        upper[n] = (char)ch_upper( upper[n] );
    }
    for ( i = 0; i < doc->count; i++ ) {
        LINE *ln = &doc->lines[i];
        tok = first_tokens( ln->text ? ln->text : "", ln->len, &count );
        if ( tok == NULL || count < 3 || tok[0].kind != TK_KW || tok[0].kw != KW_DECLARE
          || tok[1].kind != TK_KW || tok[1].kw != (kind == PK_SUB ? KW_SUB : KW_FUNCTION)
          || tok[2].kind != TK_ID ) {
            continue;
        }
        for ( n = 0; n < tok[2].len && n < NAME_MAX + 1; n++ ) {
            have[n] = (char)ch_upper( ln->text[tok[2].col + n] );
        }
        have[n] = 0;
        if ( str_cmp( have, upper ) == 0 ) {
            return 1;
        }
    }
    return 0;
}

/* DECLARE SUB name (the parameters of its heading) */
static int declare_for( DOC *doc, char *out, u32 max )
{
    u32 i, at;
    LINE *ln;
    const char *paren;
    int count;
    TOKEN *tok;

    for ( i = 0; i < doc->count; i++ ) {
        ln = &doc->lines[i];
        tok = first_tokens( ln->text ? ln->text : "", ln->len, &count );
        if ( tok == NULL || count < 2 || tok[0].kind != TK_KW || (tok[0].kw != KW_SUB && tok[0].kw != KW_FUNCTION) ) {
            continue;
        }
        str_cpy( out, "DECLARE " );
        str_catn( out, tok[0].kw == KW_SUB ? "SUB " : "FUNCTION ", max );
        at = str_len( out );
        if ( at + tok[1].len + 1 < max ) {
            mem_cpy( out + at, ln->text + tok[1].col, tok[1].len );
            out[at + tok[1].len] = 0;
        }
        str_catn( out, " (", max );
        paren = NULL;
        {
            u32 k;
            for ( k = tok[1].col + tok[1].len; k < ln->len; k++ ) {
                if ( ln->text[k] == '(' ) {
                    paren = ln->text + k + 1;
                    break;
                }
                if ( ln->text[k] != ' ' ) {
                    break;
                }
            }
        }
        if ( paren != NULL ) {
            /* to the ) that closes the list: a( ) in it is an array */
            const char *close = paren;
            int depth = 1;
            u32 plen;
            while ( close < ln->text + ln->len ) {
                if ( *close == '(' ) {
                    depth++;
                } else if ( *close == ')' && --depth == 0 ) {
                    break;
                }
                close++;
            }
            plen = (u32)(close - paren);
            at = str_len( out );
            if ( at + plen + 2 > max ) {
                return 0;                       /* too long for a line: no DECLARE */
            }
            mem_cpy( out + at, paren, plen );
            out[at + plen] = 0;
        }
        str_catn( out, ")", max );
        return 1;
    }
    return 0;
}

int prog_save( const char *path )
{
    WRITER *w;
    DOC *doc;
    u32 i, done;
    int t, order[MAX_PROCS + 1], n, j, k, added = 0;
    static char decl[MAX_LINE + 1];
    const char *slash;

    /* the DECLAREs the module lacks go at its top first, as QBasic's do */
    for ( t = 1; t < ptext.ntexts; t++ ) {
        doc = ptext.texts[t];
        if ( !has_declare( doc->pname, doc->pkind ) && declare_for( doc, decl, sizeof( decl ) ) ) {
            if ( !doc_insert_line( ptext.texts[0], (u32)added, decl, str_len( decl ) ) ) {
                return ERR_NOMEM;
            }
            added++;
        }
    }
    w = (WRITER *)try_alloc( sizeof( WRITER ) );
    if ( w == NULL ) {
        return ERR_NOMEM;
    }
    w->used = 0;
    w->rc = sys_create( path, &w->handle );
    if ( w->rc != 0 ) {
        int rc = w->rc;
        xfree( w );
        return rc;
    }
    doc = ptext.texts[0];
    for ( i = 0; i < doc->count; i++ ) {
        /* a module that is one empty line is nothing at all */
        if ( doc->count == 1 && doc->lines[0].len == 0 ) {
            break;
        }
        w_line( w, doc->lines[i].text ? doc->lines[i].text : "", doc->lines[i].len );
    }
    /* the procedures, by name */
    n = 0;
    for ( t = 1; t < ptext.ntexts; t++ ) {
        order[n++] = t;
    }
    for ( j = 1; j < n; j++ ) {
        k = order[j];
        for ( t = j - 1; t >= 0 && str_icmp( ptext.texts[order[t]]->pname, ptext.texts[k]->pname ) > 0; t-- ) {
            order[t + 1] = order[t];
        }
        order[t + 1] = k;
    }
    for ( j = 0; j < n; j++ ) {
        doc = ptext.texts[order[j]];
        if ( doc->lines[0].len != 0 ) {
            w_line( w, "", 0 );                 /* a blank line before each */
        }
        for ( i = 0; i < doc->count; i++ ) {
            w_line( w, doc->lines[i].text ? doc->lines[i].text : "", doc->lines[i].len );
        }
    }
    if ( w->used && w->rc == 0 ) {
        w->rc = sys_write( w->handle, w->buf, w->used, &done );
    }
    if ( sys_close( w->handle ) != 0 && w->rc == 0 ) {
        w->rc = ERR_ACCESS;
    }
    t = w->rc;
    xfree( w );
    if ( t == 0 ) {
        str_cpyn( ptext.path, path, sizeof( ptext.path ) );
        slash = str_rchr( path, '\\' );
        if ( slash == NULL ) {
            slash = str_rchr( path, ':' );
        }
        str_cpyn( ptext.name, slash ? slash + 1 : path, sizeof( ptext.name ) );
        ptext.modified = 0;
        for ( j = 0; j < ptext.ntexts; j++ ) {
            ptext.texts[j]->modified = 0;
        }
    }
    return t;
}
