/*
 * F_STUBS.C - a place held for each format whose code is not built
 * in, so that FMTTAB.C's table is whole and the program links.
 *
 * Only PWHOST.EXE is built with this file, and only when it is built
 * for the work on one format (MKHOST.CMD: HAVE_x is defined for each
 * format whose own code is in the build).  PMWRITE.EXE has them all
 * and does not link it.
 */
#include "fmt.h"

static int stub_detect( const u8 *data, u32 len, const char *ext )
{
    (void)data;
    (void)len;
    (void)ext;
    return 0;
}

static int stub_load( const u8 *data, u32 len, WDOC *doc )
{
    (void)data;
    (void)len;
    (void)doc;
    return WE_UNSUPPORTED;
}

/* with every HAVE_x defined nothing below uses these two */
void *const stub_unused[2] = { (void *)stub_detect, (void *)stub_load };

#ifndef HAVE_RTF
const FORMAT fmt_rtf = {
    FMT_RTF, "Rich Text Format", "RTF", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_DOCX
const FORMAT fmt_docx = {
    FMT_DOCX, "Word Document", "DOCX", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_ODT
const FORMAT fmt_odt = {
    FMT_ODT, "OpenDocument Text", "ODT", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_DOC
const FORMAT fmt_doc = {
    FMT_DOC, "Word 97-2003 Document", "DOC", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_WINWORD
const FORMAT fmt_winword = {
    FMT_WINWORD, "Word for Windows 2.0", "DOC", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_WORDDOS
const FORMAT fmt_worddos = {
    FMT_WORDDOS, "Word for MS-DOS", "DOC", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_WRITE
const FORMAT fmt_write = {
    FMT_WRITE, "Windows Write", "WRI", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_WORDSTAR
const FORMAT fmt_wordstar = {
    FMT_WORDSTAR, "WordStar", "WS;WSD", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_WORDPERFECT
const FORMAT fmt_wordperfect = {
    FMT_WORDPERFECT, "WordPerfect", "WPD;WP;WP5;WP6", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_AMIPRO
const FORMAT fmt_amipro = {
    FMT_AMIPRO, "Ami Pro", "SAM", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_WORDPRO
const FORMAT fmt_wordpro = {
    FMT_WORDPRO, "Lotus Word Pro", "LWP", stub_detect, stub_load, NULL, NULL, 0
};
#endif

#ifndef HAVE_PDF
const FORMAT fmt_pdf = {
    FMT_PDF, "PDF", "PDF", stub_detect, stub_load, NULL, NULL, 0
};
#endif
