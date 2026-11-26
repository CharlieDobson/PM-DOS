/*
 * FILES.C - documents opened and saved, in whatever format.
 *
 * OPENING.  The whole file is read into memory and every format is
 * asked whose it is (FMT.H); the surest reads it.  A PDF is the
 * exception: it is not read into a document but looked at, a page at
 * a time, by the PDF code - and File > Edit PDF as Text is the way to
 * a document made of its words.
 *
 * SAVING is to a file beside the one being replaced, which takes its
 * name only when all of it has been written; so a disk that fills, or
 * a format that fails half way, leaves the old file as it was.
 *
 * A document saved as a PDF is a copy: the document goes on being the
 * file it was, in the format it had, because a PDF is not something
 * PMWRITE can open again as the same document.
 */
#include "pw.h"
#include "img.h"
#include "pdfview.h"

#define FILE_MOST       (64UL * 1024 * 1024)

static const FBTYPE open_types[] = {
    { "All documents", "*.DOC;*.DOCX;*.RTF;*.WRI;*.ODT;*.WPD;*.WP;*.WP5;*.WS;*.SAM;*.LWP;*.PDF;*.TXT" },
    { "Word (*.DOC;*.DOCX;*.RTF)", "*.DOC;*.DOCX;*.DOT;*.RTF" },
    { "Windows Write (*.WRI)", "*.WRI" },
    { "OpenDocument Text (*.ODT)", "*.ODT;*.OTT" },
    { "WordPerfect (*.WPD;*.WP)", "*.WPD;*.WP;*.WP5;*.WP6" },
    { "WordStar (*.WS)", "*.WS;*.WSD" },
    { "Ami Pro (*.SAM)", "*.SAM" },
    { "Lotus Word Pro (*.LWP)", "*.LWP" },
    { "PDF (*.PDF)", "*.PDF" },
    { "Text (*.TXT)", "*.TXT;*.ASC" },
    { "All files (*.*)", "*.*" }
};

#define NOPEN_TYPES     (sizeof( open_types ) / sizeof( open_types[0] ))

uint8_t *file_read( const char *path, uint32_t *len, int quiet )
{
    uint8_t *data = NULL;
    uint32_t handle, size = 0, done = 0;
    int rc;

    *len = 0;
    rc = sys_open_read( path, &handle );
    if ( rc == 0 ) {
        rc = sys_file_size( handle, &size );
        if ( rc == 0 && size > FILE_MOST ) {
            rc = ERR_NOMEM;
        }
        if ( rc == 0 ) {
            data = (uint8_t *)try_alloc( size + 1 );
            if ( data == NULL ) {
                rc = ERR_NOMEM;
            } else {
                rc = sys_read( handle, data, size, &done );
                if ( rc == 0 && done != size ) {
                    rc = ERR_ACCESS;
                }
            }
        }
        sys_close( handle );
    }
    if ( rc != 0 ) {
        xfree( data );
        if ( !quiet ) {
            msg_error( path, rc );
        }
        return NULL;
    }
    data[size] = 0;
    *len = size;
    return data;
}

/* why a format could not read a file, said */
static void load_failed( const char *path, int id, int rc )
{
    const char *why;

    switch ( rc ) {
    case WE_NOMEM:
        why = "There is not enough memory to open it.";
        break;
    case WE_PASSWORD:
        why = "It is protected with a password.";
        break;
    case WE_UNSUPPORTED:
        why = id == FMT_NONE ? "PMWRITE does not know this kind of file." :
                               "It is of a version, or has a feature, that PMWRITE does not read.";
        break;
    default:
        why = "The file is damaged, or is not what its name says.";
        break;
    }
    msg_box2( path, why, MB_OK );
}

/* what a file had that the document could not keep, said once */
static void say_lost( uint32_t lost )
{
    char text[480];

    if ( lost == 0 ) {
        return;
    }
    str_cpy( text, "Not everything in this file could be kept:" );
    if ( lost & LOST_IMAGES ) {
        str_catn( text, "\n  some pictures", sizeof( text ) );
    }
    if ( lost & LOST_TABLES ) {
        str_catn( text, "\n  the layout of a table (its text is here)", sizeof( text ) );
    }
    if ( lost & LOST_OBJECTS ) {
        str_catn( text, "\n  drawings, equations or embedded objects", sizeof( text ) );
    }
    if ( lost & LOST_LAYOUT ) {
        str_catn( text, "\n  columns, frames or text boxes", sizeof( text ) );
    }
    if ( lost & LOST_NOTES ) {
        str_catn( text, "\n  footnotes or comments", sizeof( text ) );
    }
    if ( lost & LOST_REVISIONS ) {
        str_catn( text, "\n  tracked changes (shown as accepted)", sizeof( text ) );
    }
    if ( lost & LOST_TEXT ) {
        str_catn( text, "\n  some characters", sizeof( text ) );
    }
    if ( lost & LOST_STYLES ) {
        str_catn( text, "\n  a style sheet kept in another file", sizeof( text ) );
    }
    if ( lost & LOST_FIELDS ) {
        str_catn( text, "\n  fields (their last results are shown)", sizeof( text ) );
    }
    msg_text( ui_name, text, MB_OK );
}

/* a PDF, to be looked at: its password asked for if it has one */
static int open_pdf( const char *path )
{
    PDFDOCVIEW *pdf;
    char password[48];
    int rc, index;

    pdf = (PDFDOCVIEW *)try_alloc( sizeof( PDFDOCVIEW ) );
    if ( pdf == NULL ) {
        no_memory();
        return 0;
    }
    mem_set( pdf, 0, sizeof( PDFDOCVIEW ) );
    pdf->pix_page = pdf->lines_page = -1;
    rc = pdfview_open( path );
    while ( rc == PV_PASSWORD ) {
        password[0] = 0;
        if ( !input_box( "Password", "This PDF's &password:", password, sizeof( password ), 24 ) ) {
            pdfview_close();
            xfree( pdf );
            return 0;
        }
        rc = pdfview_password( password, str_len( password ) );
        if ( rc == PV_OK ) {
            str_cpy( pdf->password, password );
        } else if ( rc == PV_PASSWORD ) {
            msg_box( "That is not the password.", MB_OK );
        }
    }
    if ( rc != PV_OK ) {
        pdfview_close();
        xfree( pdf );
        msg_box2( path, rc == PV_NOMEM ? "There is not enough memory to open it." :
                  rc == PV_CRYPT ? "It is protected in a way PMWRITE does not read." :
                  rc == PV_IO ? "The file could not be read." : "The file is damaged, or is not a PDF.", MB_OK );
        return 0;
    }
    pdf->pages = pdfview_pages();
    index = doc_add( NULL, pdf, path );
    if ( index < 0 ) {
        view_free_pdf( pdf );
        return 0;
    }
    doc_show( index );
    return 1;
}

/* a file's bytes as a document: NULL, and why said, if they are not one */
static WDOC *load_doc( const char *path, const uint8_t *data, uint32_t len, const char *password )
{
    WDOC *doc;
    int id, rc;

    id = fmt_detect( data, len, path );
    if ( id == FMT_NONE ) {
        load_failed( path, id, WE_UNSUPPORTED );
        return NULL;
    }
    doc = wd_new();
    if ( doc == NULL ) {
        no_memory();
        return NULL;
    }
    busy_show( "Reading", path );
    mouse_shape( MP_WAIT );
    fmt_pdf_password( password );
    rc = formats[id]->load( data, len, doc );
    fmt_pdf_password( NULL );
    mouse_shape( MP_ARROW );
    if ( rc == WE_OK && wd_failed( doc ) ) {
        rc = WE_NOMEM;
    }
    if ( rc != WE_OK ) {
        wd_free( doc );
        load_failed( path, id, rc );
        return NULL;
    }
    doc->format = id;
    doc->failed = 0;
    wd_finish( doc );
    return doc;
}

int file_open( const char *path, int as_text )
{
    WDOC *doc;
    EDITOR *ed;
    uint8_t *data;
    char full[PATH_LEN];
    uint32_t len;
    int index;

    if ( sys_truename( path, full, 2 ) != 0 ) {
        str_cpyn( full, path, PATH_LEN );
    }
    index = doc_find( full );
    if ( index >= 0 && !as_text ) {
        doc_show( index );
        return 1;
    }
    if ( ndocs >= MAX_DOCS ) {
        msg_box( "No more documents can be open at once. Close one first.", MB_OK );
        return 0;
    }
    data = file_read( full, &len, 0 );
    if ( data == NULL ) {
        return 0;
    }
    if ( !as_text && fmt_detect( data, len, full ) == FMT_PDF ) {
        xfree( data );
        return open_pdf( full );
    }
    doc = load_doc( full, data, len, as_text && cur_doc >= 0 && PDFV != NULL ? PDFV->password : NULL );
    xfree( data );
    if ( doc == NULL ) {
        return 0;
    }
    ed = ed_new( doc, !gfx_on );
    if ( ed == NULL ) {
        wd_free( doc );
        no_memory();
        return 0;
    }
    /* a PDF's words are a new document: it has no file of its own yet */
    index = doc_add( ed, NULL, as_text ? "" : full );
    if ( index < 0 ) {
        ed_free( ed );
        return 0;
    }
    if ( as_text ) {
        ed->format = FMT_NONE;
    }
    doc_show( index );
    say_lost( doc->lost );
    return 1;
}

void file_open_ask( void )
{
    char path[PATH_LEN];
    int type = 0;

    path[0] = 0;
    if ( fbox_open( "Open", path, open_types, (int)NOPEN_TYPES, &type ) ) {
        file_open( path, 0 );
    }
}

void file_to_text( void )
{
    if ( PDFV != NULL ) {
        file_open( docs[cur_doc].path, 1 );
    }
}

void file_new( void )
{
    static int number;
    WDOC *doc;
    EDITOR *ed;
    int index;

    if ( ndocs >= MAX_DOCS ) {
        msg_box( "No more documents can be open at once. Close one first.", MB_OK );
        return;
    }
    doc = wd_new();
    if ( doc != NULL ) {
        wd_finish( doc );
    }
    ed = doc != NULL && !wd_failed( doc ) ? ed_new( doc, !gfx_on ) : NULL;
    if ( ed == NULL ) {
        wd_free( doc );
        no_memory();
        return;
    }
    ed->untitled = ++number;
    index = doc_add( ed, NULL, "" );
    if ( index < 0 ) {
        ed_free( ed );
        return;
    }
    doc_show( index );
}

/* ------------------------------------------------------------------ */
/* saving                                                              */
/* ------------------------------------------------------------------ */

/* the formats that can be written, as the Save As box lists them */
static int save_types( FBTYPE *types, int *ids, char patterns[][24] )
{
    const FORMAT *fmt;
    const char *ext;
    int id, count = 0, len;

    for ( id = FMT_NONE + 1; id < FMT_COUNT; id++ ) {
        fmt = formats[id];
        if ( fmt == NULL || fmt->save == NULL ) {
            continue;
        }
        /* "*." and the format's first extension */
        ext = fmt->exts;
        patterns[count][0] = '*';
        patterns[count][1] = '.';
        for ( len = 0; ext[len] && ext[len] != ';' && len < 8; len++ ) {
            patterns[count][2 + len] = ext[len];
        }
        patterns[count][2 + len] = 0;
        types[count].label = fmt->name;
        types[count].patterns = patterns[count];
        ids[count] = id;
        count++;
    }
    return count;
}

/* the document written to "path" as a format: 1 if it is there, whole */
static int write_doc( EDITOR *ed, const char *path, int id, int variant )
{
    OUT *out;
    char temp[PATH_LEN];
    char *slash;
    int rc, error;

    /* beside the file it will be, under a name of its own */
    if ( !str_cpyn( temp, path, PATH_LEN ) ) {
        return 0;
    }
    slash = str_rchr( temp, '\\' );
    if ( slash == NULL ) {
        slash = str_rchr( temp, ':' );
    }
    slash = slash == NULL ? temp : slash + 1;
    *slash = 0;
    if ( !str_catn( temp, "PMWRITE.$$$", PATH_LEN ) ) {
        return 0;
    }
    if ( str_icmp( temp, path ) == 0 ) {
        msg_box2( path, "PMWRITE uses that name while it saves. Choose another.", MB_OK );
        return 0;
    }
    busy_show( "Saving", path );
    mouse_shape( MP_WAIT );
    out = out_file( temp );
    if ( out == NULL ) {
        mouse_shape( MP_ARROW );
        msg_box2( path, "The file could not be made there.", MB_OK );
        return 0;
    }
    ed->doc->failed = 0;
    rc = formats[id]->save( ed->doc, out, variant );
    error = out_close( out );
    ed->doc->failed = 0;
    mouse_shape( MP_ARROW );
    if ( rc != WE_OK || error != 0 ) {
        sys_delete( temp );
        if ( rc == WE_NOMEM || error == WE_NOMEM ) {
            msg_box2( path, "There is not enough memory to save it. The file on the disk is as it was.", MB_OK );
        } else if ( error > 0 ) {
            msg_error( path, error );
        } else {
            msg_box2( path, "It could not be saved in that format. The file on the disk is as it was.", MB_OK );
        }
        return 0;
    }
    sys_delete( path );
    error = sys_rename( temp, path );
    if ( error != 0 ) {
        msg_error( path, error );
        msg_box2( temp, "The document was saved under this name instead.", MB_OK );
        return 0;
    }
    return 1;
}

int file_save( int index, int ask )
{
    FBTYPE types[FMT_COUNT];
    char patterns[FMT_COUNT][24];
    const char *names[16];
    EDITOR *ed;
    const FORMAT *fmt;
    char path[PATH_LEN];
    int ids[FMT_COUNT], count, type = 0, id, variant, scan, nvariants;

    if ( index < 0 || index >= ndocs || docs[index].ed == NULL ) {
        return 0;
    }
    ed = docs[index].ed;
    id = ed->format;
    variant = ed->variant;
    str_cpy( path, docs[index].path );
    fmt = fmt_get( id );
    /* a file with no name yet, or of a format that is only read, is asked about */
    if ( path[0] == 0 || fmt == NULL || fmt->save == NULL || id == FMT_PDF ) {
        ask = 1;
    }
    if ( ask ) {
        count = save_types( types, ids, patterns );
        if ( count == 0 ) {
            return 0;
        }
        for ( scan = 0; scan < count; scan++ ) {
            if ( ids[scan] == id ) {
                type = scan;
            }
        }
        if ( fmt == NULL || fmt->save == NULL ) {
            /* nothing to go by: Word's own format, if it is there, else the first */
            for ( scan = 0; scan < count; scan++ ) {
                if ( ids[scan] == FMT_DOCX ) {
                    type = scan;
                }
            }
        }
        if ( !fbox_save( "Save As", path, types, count, &type ) ) {
            return 0;
        }
        if ( type < 0 || type >= count ) {
            type = 0;
        }
        if ( ids[type] != id ) {
            variant = -1;
        }
        id = ids[type];
        fmt = formats[id];
        /* which kind of that format, when it has more than one */
        nvariants = 0;
        while ( fmt->variants != NULL && fmt->variants[nvariants] != NULL && nvariants < 16 ) {
            names[nvariants] = fmt->variants[nvariants];
            nvariants++;
        }
        if ( variant < 0 || variant >= nvariants ) {
            variant = fmt->def_variant;
        }
        if ( nvariants > 1 ) {
            variant = dlg_pick( fmt->name, names, nvariants, variant );
            if ( variant < 0 ) {
                return 0;
            }
        }
    }
    if ( !write_doc( ed, path, id, variant ) ) {
        return 0;
    }
    if ( id == FMT_PDF ) {
        app_message( "A PDF copy was saved. The document is still the file it was." );
        return 1;
    }
    str_cpy( docs[index].path, path );
    str_cpy( ed->path, path );
    ed->format = id;
    ed->variant = variant;
    ed_mark_saved( ed );
    doc_name( index );
    cmds_init();
    return 1;
}

int file_close_ask( int index )
{
    char text[80];
    int answer;

    if ( index < 0 || index >= ndocs || docs[index].ed == NULL || !ed_modified( docs[index].ed ) ) {
        return 1;
    }
    doc_show( index );
    sfmt( text, sizeof( text ), "%s has changed.", docs[index].name );
    answer = msg_box2( text, "Save the changes?", MB_YESNOCANCEL );
    if ( answer == ID_YES ) {
        return file_save( index, 0 );
    }
    return answer == ID_NO;
}

/* ------------------------------------------------------------------ */
/* Insert > File, Picture                                              */
/* ------------------------------------------------------------------ */

void file_insert( void )
{
    WDOC *other;
    uint8_t *data;
    char path[PATH_LEN];
    uint32_t len;
    int type = 0;

    path[0] = 0;
    if ( !fbox_open( "Insert File", path, open_types, (int)NOPEN_TYPES, &type ) ) {
        return;
    }
    data = file_read( path, &len, 0 );
    if ( data == NULL ) {
        return;
    }
    other = load_doc( path, data, len, NULL );
    xfree( data );
    if ( other == NULL ) {
        return;
    }
    ed_insert_doc( ED, other );
    wd_free( other );
}

void file_picture( void )
{
    static const FBTYPE types[] = {
        { "Pictures (*.BMP;*.PNG;*.JPG)", "*.BMP;*.PNG;*.JPG;*.JPEG;*.GIF" },
        { "All files (*.*)", "*.*" }
    };
    uint8_t *data;
    char path[PATH_LEN];
    int width, height;
    uint32_t len;
    int type = 0, kind;

    path[0] = 0;
    if ( !fbox_open( "Insert Picture", path, types, 2, &type ) ) {
        return;
    }
    data = file_read( path, &len, 0 );
    if ( data == NULL ) {
        return;
    }
    kind = img_kind( data, len );
    if ( kind == IMG_UNKNOWN ) {
        msg_box2( path, "PMWRITE does not know this kind of picture.", MB_OK );
    } else {
        img_natural( data, len, &width, &height );
        ed_insert_image( ED, data, len, kind, width, height );
    }
    xfree( data );
}

/* ------------------------------------------------------------------ */
/* File > Print                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char *row;
    int   cols;
} PRINTCTX;

static void print_seg( void *ctx, const LSEG *seg )
{
    PRINTCTX *pc = (PRINTCTX *)ctx;
    char near_buf[8];
    unsigned int index;
    int col = (int)(seg->x / GRID_W), code;

    if ( seg->kind != SEG_TEXT && seg->kind != SEG_MADE ) {
        return;
    }
    for ( index = 0; index < seg->count && col + (int)index < pc->cols; index++ ) {
        if ( col + (int)index < 0 ) {
            continue;                   /* left of the paper: an indent below nothing */
        }
        code = seg->text[index];
        if ( seg->upper && code >= 'a' && code <= 'z' ) {
            code -= 0x20;
        }
        cp_put( country.codepage ? country.codepage : CP_437, (uint32_t)code, near_buf );
        pc->row[col + (int)index] = near_buf[0];
    }
}

/*
 * The document as a printer of characters prints it: its lines as the
 * text screen sets them, a form feed between pages.  PM-DOS has no
 * printer drivers, so this is what every printer can take; the way to
 * paper with the document's own type is a PDF.
 */
void file_print( void )
{
    static char device[64] = "PRN";
    EDITOR *ed = ED;
    PRINTCTX pc;
    LAY *lay;
    LINFO info;
    char row[256];
    uint32_t handle, done;
    unsigned int pi, li, lines, page = 0, pages, indent;
    int rc, last, left;

    if ( !input_box( "Print", "&Print to (PRN, LPT1, or a file's name):", device, sizeof( device ), 24 ) ||
         device[0] == 0 ) {
        return;
    }
    lay = lay_new( ed->doc, &ed->doc->body, 1 );
    if ( lay == NULL || !lay_update( lay ) ) {
        lay_free( lay );
        lay_stale( ed->lay );
        view_reset();
        no_memory();
        return;
    }
    rc = sys_open_write( device, &handle );
    if ( rc != 0 ) {
        rc = sys_create( device, &handle );
    }
    if ( rc != 0 ) {
        lay_free( lay );
        lay_stale( ed->lay );
        view_reset();
        msg_error( device, rc );
        return;
    }
    busy_show( "Printing", device );
    pages = lay_pages( lay );
    left = (int)(ed->doc->page.left / GRID_W);
    indent = (unsigned int)(left > 20 ? 20 : left);
    pc.row = row + indent;
    pc.cols = (int)(sizeof( row ) - indent - 3);
    for ( pi = 0; pi < ed->doc->body.count && rc == 0; pi++ ) {
        lines = lay_lines( lay, pi );
        for ( li = 0; li < lines && rc == 0; li++ ) {
            if ( !lay_line( lay, pi, li, &info ) ) {
                continue;
            }
            if ( page + 1 < pages && lay_page( lay, page + 1 )->para == pi && lay_page( lay, page + 1 )->line == li ) {
                page++;
                rc = sys_write( handle, "\f", 1, &done );
            }
            mem_set( row, ' ', sizeof( row ) );
            lay_segs( lay, pi, li, print_seg, &pc );
            for ( last = (int)sizeof( row ) - 3; last > 0 && row[last - 1] == ' '; last-- ) {
            }
            row[last] = '\r';
            row[last + 1] = '\n';
            if ( rc == 0 ) {
                rc = sys_write( handle, row, (uint32_t)last + 2, &done );
            }
            /* a line of double spacing is a line and an empty one */
            for ( last = GRID_H; last < info.height && rc == 0; last += GRID_H ) {
                rc = sys_write( handle, "\r\n", 2, &done );
            }
        }
    }
    if ( rc == 0 ) {
        rc = sys_write( handle, "\f", 1, &done );
    }
    sys_close( handle );
    lay_free( lay );
    /* the screen's own lines were taken over by the printing's: made again */
    lay_stale( ed->lay );
    view_reset();
    if ( rc != 0 ) {
        msg_error( device, rc );
    }
}
