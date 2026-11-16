/*
 * FMTTAB.C - the formats there are, and which one a file is.
 *
 * Each format is one F_*.C file that defines one FORMAT; this is the
 * list of them, in the order of FMT.H's numbers, which is also the
 * order the Open and Save As dialogs show.
 */
#include "fmt.h"

extern const FORMAT fmt_txt, fmt_rtf, fmt_docx, fmt_odt, fmt_doc, fmt_winword, fmt_worddos,
                    fmt_write, fmt_wordstar, fmt_wordperfect, fmt_amipro, fmt_wordpro, fmt_pdf;

const FORMAT *const formats[FMT_COUNT] = {
    NULL,
    &fmt_txt, &fmt_rtf, &fmt_docx, &fmt_odt, &fmt_doc, &fmt_winword, &fmt_worddos,
    &fmt_write, &fmt_wordstar, &fmt_wordperfect, &fmt_amipro, &fmt_wordpro, &fmt_pdf
};

/* what each is called on a command line, by FMT_* */
static const char *const tags[FMT_COUNT] = {
    "", "TXT", "RTF", "DOCX", "ODT", "DOC", "WINWORD", "WORDDOS", "WRITE", "WORDSTAR",
    "WORDPERFECT", "AMIPRO", "WORDPRO", "PDF"
};

const FORMAT *fmt_get( int id )
{
    return id > FMT_NONE && id < FMT_COUNT ? formats[id] : NULL;
}

const char *fmt_tag( int id )
{
    return id > FMT_NONE && id < FMT_COUNT ? tags[id] : "";
}

int fmt_by_tag( const char *tag )
{
    int id;

    for ( id = FMT_NONE + 1; id < FMT_COUNT; id++ ) {
        if ( str_icmp( tags[id], tag ) == 0 ) {
            return id;
        }
    }
    return -1;
}

int fmt_variant( const FORMAT *fmt, const char *text )
{
    int index;
    u32 len = str_len( text );

    if ( fmt->variants == NULL || len == 0 ) {
        return -1;
    }
    for ( index = 0; fmt->variants[index] != NULL; index++ ) {
        if ( str_nicmp( fmt->variants[index], text, len ) == 0 ) {
            return index;
        }
    }
    /* or anywhere in the name: "5.1" finds "WordPerfect 5.1" */
    for ( index = 0; fmt->variants[index] != NULL; index++ ) {
        if ( mem_ifind( (const u8 *)fmt->variants[index], str_len( fmt->variants[index] ), text ) >= 0 ) {
            return index;
        }
    }
    return -1;
}

void fmt_ext_of( const char *name, char *ext )
{
    const char *dot = NULL;
    int len = 0;

    for ( ; *name; name++ ) {
        if ( *name == '.' ) {
            dot = name;
        } else if ( *name == '\\' || *name == '/' || *name == ':' ) {
            dot = NULL;
        }
    }
    if ( dot != NULL ) {
        for ( dot++; *dot && len < 7; dot++ ) {
            ext[len++] = (char)ch_upper( *dot );
        }
    }
    ext[len] = 0;
}

int fmt_detect( const u8 *data, u32 len, const char *name )
{
    char ext[8];
    int id, sure, best = FMT_NONE, best_sure = 0;

    fmt_ext_of( name, ext );
    for ( id = FMT_NONE + 1; id < FMT_COUNT; id++ ) {
        if ( formats[id]->detect == NULL ) {
            continue;
        }
        sure = formats[id]->detect( data, len, ext );
        if ( sure > best_sure ) {
            best_sure = sure;
            best = id;
        }
    }
    return best;
}
