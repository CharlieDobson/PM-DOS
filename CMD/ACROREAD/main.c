/*
 * MAIN.C - ACROREAD.EXE, the PM-DOS PDF reader: the command line, and
 * the file opened and handed to the viewer.
 *
 *   ACROREAD file            the first page, as a picture
 *   ACROREAD file /T         ...as text, on the text screen
 *   ACROREAD file /G         a picture in sixteen greys, on the VGA
 *                            screen every machine has, without asking
 *                            the video BIOS for anything better
 *   ACROREAD file /P:n       starting at page n
 *   ACROREAD file /X         the document's text to standard output,
 *                            a form feed between pages:
 *                            ACROREAD MANUAL /X > PRN prints it
 *
 * WHAT IT READS.  A PDF file of any version, 1.0 to 2.0: with a
 * cross-reference table or a cross-reference stream, with or without
 * object streams, saved once or many times over, damaged in its table
 * or not, and encrypted by the standard handler in any of its
 * revisions (XREF.C and CRYPT.C say how).  What it draws of a page
 * GFX.H says; what it does not is there too.
 */
#include "gfx.h"
#include "view.h"

static const char usage[] =
    "Shows a PDF document.\r\n"
    "\r\n"
    "ACROREAD [drive:][path]filename [/T | /G] [/P:n] [/X]\r\n"
    "\r\n"
    "  filename  The document.  .PDF is added to a name with no extension.\r\n"
    "  /T        Starts with the text of the page, on the text screen.\r\n"
    "  /G        Draws pages in 16 grays on the standard VGA screen.\r\n"
    "  /P:n      Starts at page n.\r\n"
    "  /X        Writes the text of the document to standard output.\r\n"
    "\r\n"
    "Pages are drawn 640 by 480: in color when the video BIOS has VBE 2.0\r\n"
    "or later, in 16 grays when it has not.  F1 lists the keys.\r\n";

static void say( const char *text )
{
    sys_stdout( text, str_len( text ) );
}

void out_of_memory( void )
{
    if ( pdf_trap ) {
        pdf_fail( PE_NOMEM );
    }
    video_text();
    say( "Not enough memory\r\n" );
    sys_exit( 8 );
}

/* a password, typed unseen: 0 if nothing was typed */
static int password_ask( char *buf, int max )
{
    int len = 0, key, shift;

    say( "Password: " );
    for ( ;; ) {
        if ( sys_stdin_is_console() ) {
            key = sys_key_read( &shift ) & 0xFF;
        } else {
            key = sys_stdin_byte();
            if ( key == '\n' ) {
                continue;                   /* the other half of the line's end */
            }
            if ( key < 0 ) {
                key = '\r';
            }
        }
        if ( key == '\r' ) {
            break;
        }
        if ( key == 8 ) {
            if ( len ) {
                len--;
                say( "\b \b" );
            }
        } else if ( key == 27 ) {
            len = 0;
            break;
        } else if ( key >= 32 && len < max - 1 ) {
            buf[len++] = (char)key;
            say( "*" );
        }
    }
    buf[len] = 0;
    say( "\r\n" );
    return len;
}

/* the document's text to standard output */
static void write_text( void )
{
    static JMPBUF trap;
    TEXTPAGE *tp;
    int index, line, why;

    for ( index = 0; index < doc.page_count; index++ ) {
        pdf_trap = &trap;
        why = rt_setjmp( &trap );
        if ( why == 0 ) {
            pdf_page_begin();
            tp = text_extract( index );
            for ( line = 0; tp && line < tp->count; line++ ) {
                say( tp->lines[line] );
                say( "\r\n" );
            }
        }
        pdf_page_end();
        pdf_trap = NULL;
        if ( index + 1 < doc.page_count ) {
            say( "\f\r\n" );
        }
    }
}

void acro_main( void )
{
    static char path[PATH_LEN + 8], password[128];
    const char *cur, *shown, *dot;
    u32 len = 0;
    int want = WANT_BEST, first_page = 0, dump = 0, quoted = 0, why, tries, ok;

    sys_init();
    sys_break_install();

    for ( cur = sys_cmdline(); *cur; ) {
        if ( *cur == ' ' || *cur == '\t' ) {
            cur++;
        } else if ( *cur == '/' ) {
            cur++;
            switch ( ch_upper( *cur ) ) {
            case 'T':
                want = WANT_TEXT;
                break;
            case 'G':
                want = WANT_VGA;
                break;
            case 'X':
                dump = 1;
                break;
            case 'P':
                cur++;
                if ( *cur == ':' || *cur == '=' ) {
                    cur++;
                }
                first_page = 0;
                while ( *cur >= '0' && *cur <= '9' ) {
                    first_page = first_page * 10 + (*cur++ - '0');
                }
                first_page--;
                continue;
            default:
                say( usage );
                sys_exit( *cur == '?' ? 0 : 1 );
            }
            if ( *cur ) {
                cur++;
            }
        } else {
            /* the file's name: to a space or a switch, or between quotes */
            len = 0;
            for ( ; *cur && (quoted || (*cur != ' ' && *cur != '\t' && *cur != '/')); cur++ ) {
                if ( *cur == '"' ) {
                    quoted = !quoted;
                } else if ( len < PATH_LEN ) {
                    path[len++] = *cur;
                }
            }
            path[len] = 0;
        }
    }
    if ( path[0] == 0 ) {
        say( usage );
        sys_exit( 1 );
    }
    /* the name without its path, for the screen; and .PDF if it says
       no extension */
    shown = path;
    for ( cur = path; *cur; cur++ ) {
        if ( *cur == '\\' || *cur == '/' || *cur == ':' ) {
            shown = cur + 1;
        }
    }
    dot = str_rchr( shown, '.' );
    if ( dot == NULL ) {
        str_catn( path, ".PDF", sizeof( path ) );
    }

    why = pdf_open( path );
    for ( tries = 0; why == PE_PASSWORD && tries < 3; tries++ ) {
        ok = password_ask( password, sizeof( password ) ) && pdf_password( password, str_len( password ) );
        if ( ok ) {
            why = pdf_open_finish();
        } else if ( password[0] == 0 ) {
            break;
        } else {
            say( "That is not the password\r\n" );
        }
    }
    if ( why != PE_NONE || doc.page_count < 1 ) {
        say( path );
        say( ": " );
        say( why == PE_IO ? "File not found" : (why != PE_NONE ? why_text( why ) : "The document has no pages") );
        say( "\r\n" );
        sys_exit( 1 );
    }
    if ( dump ) {
        write_text();
    } else {
        view_run( shown, first_page, want );
    }
    pdf_close();
    sys_exit( 0 );
}
