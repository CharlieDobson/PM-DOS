/*
 * XML.C - XML read a piece at a time, and escaped as it is written
 * (XML.H).
 *
 * THE DOCUMENT IS READ WHERE IT LIES.  Names and plain text are handed
 * back as pieces of the caller's own bytes; only what has to be
 * changed - text with an entity or a carriage return in it, and every
 * attribute's value, which a caller wants to end with a 0 - is copied,
 * into one scratch block that is used again for each piece.  So there
 * is no limit on how long a name, a value or a run of text is, beyond
 * memory.
 *
 * NAMESPACES.  Each xmlns declaration is kept, with the depth of the
 * element that made it, until that element ends; a prefix is looked up
 * from the newest declaration back, as the rule is.  An attribute with
 * no prefix is in no namespace, whatever its element's default.
 *
 * WHAT IS CHECKED is what a reader needs in order not to be led
 * astray: every end tag must match the start that is open, a tag or a
 * comment must close before the document ends, and elements may be
 * XML_MAX_DEPTH deep and no deeper (the open elements are kept in a
 * table of that size, so depth costs nothing and cannot run away).
 * The characters a name is made of, and the other niceties of the
 * grammar, are not checked: a reader of files gains nothing by
 * refusing one that a looser reader would have understood.
 *
 * UTF-16, which the standard allows and nothing here is expected to
 * meet, is turned into UTF-8 before the reading starts.
 */
#include "fmt.h"
#include "xml.h"

typedef struct {
    const char *uri;
    int   id;
} XNS;

typedef struct {
    const uint8_t *prefix;
    unsigned int len;
    int          ns;
    unsigned int level;         /* the depth of the element that declared it */
} XBIND;

typedef struct {
    const uint8_t *name;        /* as it is written, prefix and all */
    unsigned int len;
} XOPEN;

typedef struct {
    XML          pub;
    const uint8_t *data;
    unsigned int len, pos;
    uint8_t     *own;           /* a UTF-16 document, as UTF-8 */
    int          failed, ended;
    int          pending;       /* <a/> was given as a start: its end is owed */
    unsigned int level;         /* how many elements are open */
    XNS         *known;
    unsigned int nknown, known_cap;
    XBIND       *bind;
    unsigned int nbind, bind_cap;
    XATTR       *attr;
    unsigned int attr_cap;
    uint8_t     *scratch;
    unsigned int used, cap;
    int          xml_id;        /* the number XML_NS_URI was given */
    XOPEN        open[XML_MAX_DEPTH];
} XMLP;

#define IS_SPACE( ch )  ((ch) == ' ' || (ch) == '\t' || (ch) == '\r' || (ch) == '\n')

/* ------------------------------------------------------------------ */
/* the scratch block                                                   */
/* ------------------------------------------------------------------ */

static int xml_put( XMLP *xp, const uint8_t *bytes, unsigned int len )
{
    uint8_t *fresh;
    unsigned int want;

    if ( len > xp->cap - xp->used ) {
        if ( len > 0x7FFFFFFFUL - xp->used ) {
            return 0;
        }
        want = xp->cap ? xp->cap : 256;
        while ( want < xp->used + len ) {
            want *= 2;
        }
        fresh = (uint8_t *)try_realloc( xp->scratch, want );
        if ( fresh == NULL ) {
            return 0;
        }
        xp->scratch = fresh;
        xp->cap = want;
    }
    mem_cpy( xp->scratch + xp->used, bytes, len );
    xp->used += len;
    return 1;
}

/* an entity at raw[0] == '&': its character, and *took bytes of the
   text; 0 if it is not one this reader knows */
static uint32_t xml_entity( const uint8_t *raw, unsigned int len, unsigned int *took )
{
    static const struct {
        const char *name;
        uint8_t len, code;
    } named[5] = {
        { "amp;", 4, '&' }, { "lt;", 3, '<' }, { "gt;", 3, '>' }, { "quot;", 5, '"' },
        { "apos;", 5, '\'' }
    };
    uint32_t code = 0;
    unsigned int index, digit;
    int hex, digits = 0;

    if ( len >= 4 && raw[1] == '#' ) {
        hex = raw[2] == 'x' || raw[2] == 'X';
        for ( index = hex ? 3 : 2; index < len && digits < 9; index++, digits++ ) {
            digit = raw[index];
            if ( digit >= '0' && digit <= '9' ) {
                digit -= '0';
            } else if ( hex && digit >= 'a' && digit <= 'f' ) {
                digit -= 'a' - 10;
            } else if ( hex && digit >= 'A' && digit <= 'F' ) {
                digit -= 'A' - 10;
            } else {
                break;
            }
            code = hex ? code * 16 + digit : code * 10 + digit;
        }
        if ( digits == 0 || digits > 8 || index >= len || raw[index] != ';' ) {
            return 0;
        }
        *took = index + 1;
        /* what is no character at all is the mark for one */
        if ( code == 0 || code > 0x10FFFFUL || (code >= 0xD800 && code <= 0xDFFF) ) {
            code = 0xFFFD;
        }
        return code;
    }
    for ( index = 0; index < 5; index++ ) {
        if ( len > named[index].len && mem_cmp( raw + 1, named[index].name, named[index].len ) == 0 ) {
            *took = (unsigned int)named[index].len + 1;
            return named[index].code;
        }
    }
    return 0;
}

/*
 * Text as a caller wants it, added to the scratch block: entities as
 * their characters, and line ends as XML says a reader sees them - one
 * line feed in text, one space in an attribute's value.
 */
static int xml_decode( XMLP *xp, const uint8_t *raw, unsigned int len, int quoted )
{
    uint8_t utf[4];
    uint32_t code;
    unsigned int pos = 0, from = 0, took;

    while ( pos < len ) {
        if ( raw[pos] == '&' ) {
            code = xml_entity( raw + pos, len - pos, &took );
            if ( code == 0 ) {
                pos++;                  /* not one: it stands as it is */
                continue;
            }
            if ( !xml_put( xp, raw + from, pos - from ) ||
                 !xml_put( xp, utf, (unsigned int)utf8_put( utf, code ) ) ) {
                return 0;
            }
            pos += took;
            from = pos;
        } else if ( raw[pos] == '\r' || (quoted && (raw[pos] == '\n' || raw[pos] == '\t')) ) {
            if ( !xml_put( xp, raw + from, pos - from ) ||
                 !xml_put( xp, (const uint8_t *)(quoted ? " " : "\n"), 1 ) ) {
                return 0;
            }
            if ( raw[pos] == '\r' && pos + 1 < len && raw[pos + 1] == '\n' ) {
                pos++;
            }
            pos++;
            from = pos;
        } else {
            pos++;
        }
    }
    return xml_put( xp, raw + from, pos - from );
}

/* ------------------------------------------------------------------ */
/* opening and closing                                                 */
/* ------------------------------------------------------------------ */

/* UTF-16 as UTF-8, in a block of its own */
static uint8_t *xml_from_utf16( const uint8_t *data, uint32_t len, int big, uint32_t *out_len )
{
    uint8_t *out;
    uint32_t pos, used = 0, code, low;

    if ( len / 2 > 0x7FFFFFFFUL / 3 ) {
        return NULL;
    }
    out = (uint8_t *)try_alloc( len / 2 * 3 + 4 );
    if ( out == NULL ) {
        return NULL;
    }
    for ( pos = 0; pos + 1 < len; pos += 2 ) {
        code = big ? BE16( data + pos ) : LE16( data + pos );
        if ( code >= 0xD800 && code <= 0xDBFF && pos + 3 < len ) {
            low = big ? BE16( data + pos + 2 ) : LE16( data + pos + 2 );
            if ( low >= 0xDC00 && low <= 0xDFFF ) {
                code = 0x10000UL + ((code - 0xD800) << 10) + (low - 0xDC00);
                pos += 2;
            }
        }
        if ( code >= 0xD800 && code <= 0xDFFF ) {
            code = 0xFFFD;
        }
        used += (uint32_t)utf8_put( out + used, code );
    }
    *out_len = used;
    return out;
}

XML *xml_open( const uint8_t *data, uint32_t len )
{
    XMLP *xp = (XMLP *)try_alloc( sizeof( XMLP ) );
    int wide = 0, big = 0;

    if ( xp == NULL ) {
        return NULL;
    }
    mem_set( xp, 0, sizeof( XMLP ) );
    if ( len >= 2 && data[0] == 0xFF && data[1] == 0xFE ) {
        wide = 1;
        data += 2;
        len -= 2;
    } else if ( len >= 2 && data[0] == 0xFE && data[1] == 0xFF ) {
        wide = big = 1;
        data += 2;
        len -= 2;
    } else if ( len >= 4 && data[0] == '<' && data[1] == 0 && data[3] == 0 ) {
        wide = 1;
    } else if ( len >= 4 && data[0] == 0 && data[1] == '<' && data[2] == 0 ) {
        wide = big = 1;
    } else if ( len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF ) {
        data += 3;
        len -= 3;
    }
    if ( wide ) {
        xp->own = xml_from_utf16( data, len, big, &len );
        if ( xp->own == NULL ) {
            xfree( xp );
            return NULL;
        }
        data = xp->own;
    }
    xp->data = data;
    xp->len = len;
    return &xp->pub;
}

void xml_close( XML *xml )
{
    XMLP *xp = (XMLP *)xml;

    if ( xp != NULL ) {
        xfree( xp->own );
        xfree( xp->known );
        xfree( xp->bind );
        xfree( xp->attr );
        xfree( xp->scratch );
        xfree( xp );
    }
}

int xml_ns( XML *xml, const char *uri, int id )
{
    XMLP *xp = (XMLP *)xml;
    XNS *fresh;
    unsigned int want;

    if ( xp->nknown == xp->known_cap ) {
        want = xp->known_cap ? xp->known_cap * 2 : 32;
        fresh = (XNS *)try_realloc( xp->known, want * sizeof( XNS ) );
        if ( fresh == NULL ) {
            xp->failed = 1;
            return 0;
        }
        xp->known = fresh;
        xp->known_cap = want;
    }
    xp->known[xp->nknown].uri = uri;
    xp->known[xp->nknown].id = id;
    xp->nknown++;
    if ( str_cmp( uri, XML_NS_URI ) == 0 ) {
        xp->xml_id = id;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* namespaces                                                          */
/* ------------------------------------------------------------------ */

static int xml_known( const XMLP *xp, const char *uri )
{
    unsigned int index;

    for ( index = 0; index < xp->nknown; index++ ) {
        if ( str_cmp( xp->known[index].uri, uri ) == 0 ) {
            return xp->known[index].id;
        }
    }
    return 0;
}

static int xml_declare( XMLP *xp, const uint8_t *prefix, unsigned int len, const char *uri )
{
    XBIND *fresh;
    unsigned int want;

    if ( xp->nbind == xp->bind_cap ) {
        want = xp->bind_cap ? xp->bind_cap * 2 : 32;
        fresh = (XBIND *)try_realloc( xp->bind, want * sizeof( XBIND ) );
        if ( fresh == NULL ) {
            return 0;
        }
        xp->bind = fresh;
        xp->bind_cap = want;
    }
    xp->bind[xp->nbind].prefix = prefix;
    xp->bind[xp->nbind].len = len;
    xp->bind[xp->nbind].ns = xml_known( xp, uri );
    xp->bind[xp->nbind].level = xp->level;
    xp->nbind++;
    return 1;
}

/* the namespace a prefix stands for now; "len" 0 asks for the default */
static int xml_resolve( const XMLP *xp, const uint8_t *prefix, unsigned int len )
{
    unsigned int index;

    if ( len == 3 && mem_cmp( prefix, "xml", 3 ) == 0 ) {
        return xp->xml_id;
    }
    for ( index = xp->nbind; index > 0; index-- ) {
        if ( xp->bind[index - 1].len == len &&
             (len == 0 || mem_cmp( xp->bind[index - 1].prefix, prefix, len ) == 0) ) {
            return xp->bind[index - 1].ns;
        }
    }
    return 0;
}

/* how long the prefix of a name is, with its colon: 0 for none */
static unsigned int xml_prefix( const uint8_t *name, unsigned int len )
{
    unsigned int index;

    for ( index = 0; index < len; index++ ) {
        if ( name[index] == ':' ) {
            return index + 1 < len ? index + 1 : 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the pieces                                                          */
/* ------------------------------------------------------------------ */

static int xml_fail( XMLP *xp )
{
    xp->failed = 1;
    return XML_ERROR;
}

/* the element on top of the table ends */
static int xml_give_end( XMLP *xp )
{
    const XOPEN *top = &xp->open[xp->level - 1];
    unsigned int cut = xml_prefix( top->name, top->len );

    xp->pub.ns = xml_resolve( xp, top->name, cut ? cut - 1 : 0 );
    xp->pub.name = (const char *)top->name + cut;
    xp->pub.name_len = top->len - cut;
    xp->pub.depth = (int)xp->level;
    xp->pub.nattr = 0;
    /* what it declared goes with it */
    while ( xp->nbind && xp->bind[xp->nbind - 1].level == xp->level ) {
        xp->nbind--;
    }
    xp->level--;
    return XML_END;
}

static int xml_end_tag( XMLP *xp )
{
    const uint8_t *data = xp->data;
    unsigned int len = xp->len, pos = xp->pos + 2, name_at = pos, name_len;

    while ( pos < len && !IS_SPACE( data[pos] ) && data[pos] != '>' ) {
        pos++;
    }
    name_len = pos - name_at;
    while ( pos < len && IS_SPACE( data[pos] ) ) {
        pos++;
    }
    if ( pos >= len || data[pos] != '>' || xp->level == 0 ||
         xp->open[xp->level - 1].len != name_len ||
         mem_cmp( xp->open[xp->level - 1].name, data + name_at, name_len ) != 0 ) {
        return xml_fail( xp );
    }
    xp->pos = pos + 1;
    return xml_give_end( xp );
}

static int xml_start_tag( XMLP *xp )
{
    const uint8_t *data = xp->data;
    XATTR *fresh, *attr;
    unsigned int len = xp->len, pos = xp->pos + 1, name_at = pos, name_len, at, val_at, cut, want;
    unsigned int nattr = 0, index;
    int quote, empty;

    while ( pos < len && !IS_SPACE( data[pos] ) && data[pos] != '/' && data[pos] != '>' ) {
        pos++;
    }
    name_len = pos - name_at;
    if ( name_len == 0 || xp->level >= XML_MAX_DEPTH ) {
        return xml_fail( xp );
    }
    xp->used = 0;
    for ( ;; ) {
        while ( pos < len && IS_SPACE( data[pos] ) ) {
            pos++;
        }
        if ( pos >= len ) {
            return xml_fail( xp );
        }
        if ( data[pos] == '>' ) {
            pos++;
            empty = 0;
            break;
        }
        if ( data[pos] == '/' ) {
            if ( pos + 1 >= len || data[pos + 1] != '>' ) {
                return xml_fail( xp );
            }
            pos += 2;
            empty = 1;
            break;
        }
        at = pos;
        while ( pos < len && !IS_SPACE( data[pos] ) && data[pos] != '=' && data[pos] != '>' &&
                data[pos] != '/' ) {
            pos++;
        }
        if ( pos == at ) {
            return xml_fail( xp );
        }
        if ( nattr == xp->attr_cap ) {
            want = xp->attr_cap ? xp->attr_cap * 2 : 16;
            fresh = (XATTR *)try_realloc( xp->attr, want * sizeof( XATTR ) );
            if ( fresh == NULL ) {
                return xml_fail( xp );
            }
            xp->attr = fresh;
            xp->attr_cap = want;
        }
        attr = &xp->attr[nattr];
        attr->name = (const char *)data + at;
        attr->name_len = pos - at;
        while ( pos < len && IS_SPACE( data[pos] ) ) {
            pos++;
        }
        if ( pos >= len || data[pos] != '=' ) {
            return xml_fail( xp );
        }
        pos++;
        while ( pos < len && IS_SPACE( data[pos] ) ) {
            pos++;
        }
        if ( pos >= len || (data[pos] != '"' && data[pos] != '\'') ) {
            return xml_fail( xp );
        }
        quote = data[pos++];
        val_at = pos;
        while ( pos < len && data[pos] != quote ) {
            pos++;
        }
        if ( pos >= len ) {
            return xml_fail( xp );
        }
        attr->at = xp->used;
        if ( !xml_decode( xp, data + val_at, pos - val_at, 1 ) ||
             !xml_put( xp, (const uint8_t *)"", 1 ) ) {
            return xml_fail( xp );
        }
        attr->value_len = xp->used - 1 - attr->at;
        pos++;
        nattr++;
    }
    xp->pos = pos;
    xp->open[xp->level].name = data + name_at;
    xp->open[xp->level].len = name_len;
    xp->level++;
    /* the scratch block has stopped moving: the values are where they
       will stay.  Declarations first, for they hold for this very tag. */
    for ( index = 0; index < nattr; index++ ) {
        attr = &xp->attr[index];
        attr->value = (const char *)xp->scratch + attr->at;
        attr->ns = 0;
        if ( attr->name_len == 5 && mem_cmp( attr->name, "xmlns", 5 ) == 0 ) {
            attr->ns = -1;
            if ( !xml_declare( xp, NULL, 0, attr->value ) ) {
                return xml_fail( xp );
            }
        } else if ( attr->name_len > 6 && mem_cmp( attr->name, "xmlns:", 6 ) == 0 ) {
            attr->ns = -1;
            if ( !xml_declare( xp, (const uint8_t *)attr->name + 6, attr->name_len - 6, attr->value ) ) {
                return xml_fail( xp );
            }
        }
    }
    for ( index = 0; index < nattr; index++ ) {
        attr = &xp->attr[index];
        if ( attr->ns == 0 ) {
            cut = xml_prefix( (const uint8_t *)attr->name, attr->name_len );
            if ( cut ) {
                attr->ns = xml_resolve( xp, (const uint8_t *)attr->name, cut - 1 );
                attr->name += cut;
                attr->name_len -= cut;
            }
        }
    }
    cut = xml_prefix( data + name_at, name_len );
    xp->pub.ns = xml_resolve( xp, data + name_at, cut ? cut - 1 : 0 );
    xp->pub.name = (const char *)data + name_at + cut;
    xp->pub.name_len = name_len - cut;
    xp->pub.depth = (int)xp->level;
    xp->pub.attr = xp->attr;
    xp->pub.nattr = nattr;
    xp->pending = empty;
    return XML_START;
}

/* where "needle" is at or after "from", or the document's length */
static unsigned int xml_find( const XMLP *xp, unsigned int from, const char *needle )
{
    int at = mem_find( xp->data + from, xp->len - from, needle );

    return at < 0 ? xp->len : from + (unsigned int)at;
}

/* a declaration: <!DOCTYPE ...>, with what it may hold in brackets */
static int xml_skip_decl( XMLP *xp )
{
    const uint8_t *data = xp->data;
    unsigned int pos = xp->pos + 2, square = 0;
    int quote = 0;

    for ( ; pos < xp->len; pos++ ) {
        if ( quote ) {
            if ( data[pos] == quote ) {
                quote = 0;
            }
        } else if ( data[pos] == '"' || data[pos] == '\'' ) {
            quote = data[pos];
        } else if ( data[pos] == '[' ) {
            square++;
        } else if ( data[pos] == ']' ) {
            if ( square ) {
                square--;
            }
        } else if ( data[pos] == '>' && square == 0 ) {
            xp->pos = pos + 1;
            return 1;
        }
    }
    return 0;
}

static int xml_piece( XMLP *xp )
{
    const uint8_t *data;
    unsigned int len, start, end, index;
    int plain;

    if ( xp == NULL || xp->failed ) {
        return XML_ERROR;
    }
    if ( xp->ended ) {
        return XML_EOF;
    }
    if ( xp->pending ) {
        xp->pending = 0;
        return xml_give_end( xp );
    }
    data = xp->data;
    len = xp->len;
    for ( ;; ) {
        if ( xp->pos >= len ) {
            if ( xp->level ) {
                return xml_fail( xp );  /* it stopped inside an element */
            }
            xp->ended = 1;
            return XML_EOF;
        }
        start = xp->pos;
        if ( data[start] != '<' ) {
            plain = 1;
            for ( end = start; end < len && data[end] != '<'; end++ ) {
                if ( data[end] == '&' || data[end] == '\r' ) {
                    plain = 0;
                }
            }
            xp->pos = end;
            if ( xp->level == 0 ) {
                continue;               /* between the pieces of the prolog */
            }
            if ( plain ) {
                xp->pub.text = (const char *)data + start;
                xp->pub.text_len = end - start;
            } else {
                xp->used = 0;
                if ( !xml_decode( xp, data + start, end - start, 0 ) ) {
                    return xml_fail( xp );
                }
                xp->pub.text = (const char *)xp->scratch;
                xp->pub.text_len = xp->used;
            }
            return XML_TEXT;
        }
        if ( len - start >= 4 && mem_cmp( data + start, "<!--", 4 ) == 0 ) {
            end = xml_find( xp, start + 4, "-->" );
            if ( end >= len ) {
                return xml_fail( xp );
            }
            xp->pos = end + 3;
            continue;
        }
        if ( len - start >= 9 && mem_cmp( data + start, "<![CDATA[", 9 ) == 0 ) {
            end = xml_find( xp, start + 9, "]]>" );
            if ( end >= len ) {
                return xml_fail( xp );
            }
            xp->pos = end + 3;
            if ( xp->level == 0 || end == start + 9 ) {
                continue;
            }
            /* as it is: nothing in it is an entity.  Only its line
               ends are a reader's to tidy, and one with a return in
               it goes through the scratch block for that. */
            xp->pub.text = (const char *)data + start + 9;
            xp->pub.text_len = end - start - 9;
            for ( index = start + 9; index < end; index++ ) {
                if ( data[index] == '\r' ) {
                    break;
                }
            }
            if ( index < end ) {
                xp->used = 0;
                for ( index = start + 9; index < end; index++ ) {
                    if ( data[index] == '\r' ) {
                        if ( index + 1 < end && data[index + 1] == '\n' ) {
                            continue;
                        }
                        if ( !xml_put( xp, (const uint8_t *)"\n", 1 ) ) {
                            return xml_fail( xp );
                        }
                    } else if ( !xml_put( xp, data + index, 1 ) ) {
                        return xml_fail( xp );
                    }
                }
                xp->pub.text = (const char *)xp->scratch;
                xp->pub.text_len = xp->used;
            }
            return XML_TEXT;
        }
        if ( len - start < 2 ) {
            return xml_fail( xp );
        }
        if ( data[start + 1] == '?' ) {
            end = xml_find( xp, start + 2, "?>" );
            if ( end >= len ) {
                return xml_fail( xp );
            }
            xp->pos = end + 2;
            continue;
        }
        if ( data[start + 1] == '!' ) {
            if ( !xml_skip_decl( xp ) ) {
                return xml_fail( xp );
            }
            continue;
        }
        if ( data[start + 1] == '/' ) {
            return xml_end_tag( xp );
        }
        return xml_start_tag( xp );
    }
}

int xml_next( XML *xml )
{
    XMLP *xp = (XMLP *)xml;
    int token = xml_piece( xp );

    if ( xp != NULL ) {
        xp->pub.token = token;
    }
    return token;
}

/* ------------------------------------------------------------------ */
/* what a reader asks                                                  */
/* ------------------------------------------------------------------ */

int xml_is( const XML *xml, int ns, const char *name )
{
    unsigned int len;

    if ( xml->ns != ns ) {
        return 0;
    }
    len = str_len( name );
    return xml->name_len == len && mem_cmp( xml->name, name, len ) == 0;
}

const char *xml_attr( const XML *xml, int ns, const char *name )
{
    unsigned int index, len = str_len( name );

    for ( index = 0; index < xml->nattr; index++ ) {
        if ( xml->attr[index].ns == ns && xml->attr[index].name_len == len &&
             mem_cmp( xml->attr[index].name, name, len ) == 0 ) {
            return xml->attr[index].value;
        }
    }
    return NULL;
}

int xml_skip( XML *xml )
{
    int depth = xml->depth, token;

    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            return 0;
        }
        if ( token == XML_END && xml->depth == depth ) {
            return 1;
        }
    }
}

int xml_child( XML *xml, int depth )
{
    int token;

    /* whatever of the last child was not read goes by here: it is all
       deeper than a child is */
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            return 0;
        }
        if ( token == XML_START && xml->depth == depth + 1 ) {
            return 1;
        }
        if ( token == XML_END && xml->depth <= depth ) {
            return 0;
        }
    }
}

int xml_gather( XML *xml, char *buf, unsigned int max )
{
    int depth = xml->depth, token;
    unsigned int used = 0, part;

    if ( max ) {
        buf[0] = 0;
    }
    for ( ;; ) {
        token = xml_next( xml );
        if ( token == XML_EOF || token == XML_ERROR ) {
            return 0;
        }
        if ( token == XML_END && xml->depth == depth ) {
            return 1;
        }
        if ( token == XML_TEXT && max && used < max - 1 ) {
            part = xml->text_len < max - 1 - used ? xml->text_len : max - 1 - used;
            /* cut between characters, not inside one */
            if ( part < xml->text_len ) {
                while ( part && ((uint8_t)xml->text[part] & 0xC0) == 0x80 ) {
                    part--;
                }
            }
            mem_cpy( buf + used, xml->text, part );
            used += part;
            buf[used] = 0;
            if ( part < xml->text_len ) {
                used = max - 1;         /* full: nothing after the cut */
            }
        }
    }
}

void xml_mark( const XML *xml, XMARK *mark )
{
    const XMLP *xp = (const XMLP *)xml;

    mark->pos = xp->pos;
    mark->level = xp->level;
    mark->nbind = xp->nbind;
    mark->pending = xp->pending;
}

void xml_rewind( XML *xml, const XMARK *mark )
{
    XMLP *xp = (XMLP *)xml;

    if ( mark->level <= XML_MAX_DEPTH && mark->nbind <= xp->bind_cap && mark->pos <= xp->len ) {
        xp->pos = mark->pos;
        xp->level = mark->level;
        xp->nbind = mark->nbind;
        xp->pending = mark->pending;
        xp->ended = 0;
    }
}

/* ------------------------------------------------------------------ */
/* writing                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    OUT         *out;
    unsigned int used;
    uint8_t      buf[160];
} XESC;

static void esc_code( XESC *esc, uint32_t code, int quote )
{
    const char *as = NULL;

    if ( esc->used > sizeof( esc->buf ) - 8 ) {
        out_put( esc->out, esc->buf, esc->used );
        esc->used = 0;
    }
    if ( code < 0x20 ) {
        if ( code == '\t' ) {
            as = quote ? "&#9;" : NULL;
        } else if ( code == '\n' ) {
            as = quote ? "&#10;" : NULL;
        } else if ( code == '\r' ) {
            as = "&#13;";
        } else {
            return;                     /* XML has no way to say it */
        }
    } else if ( code == '&' ) {
        as = "&amp;";
    } else if ( code == '<' ) {
        as = "&lt;";
    } else if ( code == '>' ) {
        as = "&gt;";
    } else if ( code == '"' && quote ) {
        as = "&quot;";
    } else if ( code == '\'' && quote ) {
        as = "&apos;";
    } else if ( code == 0xFFFE || code == 0xFFFF ) {
        return;
    }
    if ( as != NULL ) {
        while ( *as ) {
            esc->buf[esc->used++] = (uint8_t)*as++;
        }
    } else {
        esc->used += (unsigned int)utf8_put( esc->buf + esc->used, code );
    }
}

void xml_esc_n( OUT *out, const uint8_t *text, unsigned int len, int quote )
{
    XESC esc;
    const uint8_t *end = text + len;

    esc.out = out;
    esc.used = 0;
    while ( text < end ) {
        esc_code( &esc, utf8_get( &text, end ), quote );
    }
    out_put( out, esc.buf, esc.used );
}

void xml_esc( OUT *out, const char *text, int quote )
{
    xml_esc_n( out, (const uint8_t *)text, str_len( text ), quote );
}

void xml_esc16( OUT *out, const uint16_t *text, unsigned int len, int quote )
{
    XESC esc;
    unsigned int pos = 0;

    esc.out = out;
    esc.used = 0;
    while ( pos < len ) {
        esc_code( &esc, utf16_get( text, len, &pos ), quote );
    }
    out_put( out, esc.buf, esc.used );
}
