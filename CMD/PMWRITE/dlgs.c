/*
 * DLGS.C - the dialog boxes.
 *
 * Each is a table of controls for UI.C's dialog engine, a call of
 * dlg_run, and then the answer put to the document through EDIT.C -
 * so each is one step for Undo.
 *
 * A DIALOG CHANGES ONLY WHAT WAS CHANGED IN IT.  The Font and
 * Paragraph boxes open showing the look at the caret; with a
 * selection of mixed looks, pressing OK must not make all of it the
 * same in every respect, so what each field held when the box opened
 * is remembered and only the fields that differ afterwards are
 * applied.
 *
 * LENGTHS are typed as the country writes them: inches, or
 * centimeters when PMWRITE.INI says so, and a unit typed after the
 * number (in, ", cm, mm, pt) is believed whichever is set.
 */
#include "pw.h"

/* ------------------------------------------------------------------ */
/* lengths and text                                                    */
/* ------------------------------------------------------------------ */

void len_text( int twips, char *buf )
{
    unsigned int value, whole, part;
    int used = 0;

    if ( twips < 0 ) {
        buf[used++] = '-';
        twips = -twips;
    }
    if ( set.inches ) {
        value = ((unsigned int)twips * 100 + 720) / 1440;   /* hundredths of an inch */
    } else {
        value = ((unsigned int)twips * 254 + 720) / 1440;   /* hundredths of a centimeter */
    }
    whole = value / 100;
    part = value % 100;
    sfmt( buf + used, 14, set.inches ? "%u.%02u\"" : "%u.%02u cm", whole, part );
}

/* a number with a decimal point (or comma) as thousandths: 0 if there
   is no number there.  *text moves past it. */
static int number_parse( const char **text, unsigned int *thousandths )
{
    const char *cur = *text;
    unsigned int whole = 0, part = 0, scale = 100;
    int digits = 0;

    while ( *cur >= '0' && *cur <= '9' ) {
        if ( whole < 100000UL ) {
            whole = whole * 10 + (unsigned int)(*cur - '0');
        }
        cur++;
        digits++;
    }
    if ( *cur == '.' || *cur == ',' ) {
        cur++;
        while ( *cur >= '0' && *cur <= '9' ) {
            part += (unsigned int)(*cur - '0') * scale;
            scale /= 10;
            cur++;
            digits++;
        }
    }
    if ( digits == 0 || whole > 2000 ) {
        return 0;
    }
    *thousandths = whole * 1000 + part;
    *text = cur;
    return 1;
}

int len_parse( const char *text, int *twips )
{
    unsigned int value;
    int negative = 0;

    while ( *text == ' ' ) {
        text++;
    }
    if ( *text == '-' ) {
        negative = 1;
        text++;
    }
    if ( !number_parse( &text, &value ) ) {
        return 0;
    }
    while ( *text == ' ' ) {
        text++;
    }
    if ( *text == '"' || str_nicmp( text, "in", 2 ) == 0 ) {
        value = value * 144 / 100;
    } else if ( str_nicmp( text, "cm", 2 ) == 0 ) {
        value = value * 144 / 254;
    } else if ( str_nicmp( text, "mm", 2 ) == 0 ) {
        value = value * 144 / 2540;
    } else if ( str_nicmp( text, "pt", 2 ) == 0 ) {
        value = value / 50;
    } else if ( *text == 0 ) {
        value = set.inches ? value * 144 / 100 : value * 144 / 254;
    } else {
        return 0;
    }
    if ( value > 31680 ) {              /* twenty-two inches: no paper is wider */
        return 0;
    }
    *twips = negative ? -(int)value : (int)value;
    return 1;
}

/* points, typed with or without "pt", as twips */
static int pt_parse( const char *text, int *twips )
{
    unsigned int value;

    while ( *text == ' ' ) {
        text++;
    }
    if ( !number_parse( &text, &value ) || value > 1584000UL ) {
        return 0;
    }
    *twips = (int)(value / 50);
    return 1;
}

static void pt_text( int twips, char *buf )
{
    sfmt( buf, 12, twips % 20 ? "%u.%u pt" : "%u pt", (uint32_t)twips / 20, ((uint32_t)twips % 20) / 2 );
}

/* what was typed, which is in the screen's code page, as the
   document's kind of text -> how many units */
static unsigned int to_wide( const char *text, uint16_t *out, unsigned int max )
{
    unsigned int len = 0;

    while ( *text && len < max ) {
        out[len++] = cp_to_uni( country.codepage ? country.codepage : CP_437, (uint8_t)*text++ );
    }
    return len;
}

/* UTF-8, as the document keeps its title and the like, for a field */
static void field_from_utf8( char *field, unsigned int max, const char *utf8 )
{
    const uint8_t *cur = (const uint8_t *)utf8, *end = cur + str_len( utf8 );
    char near_buf[8];
    unsigned int used = 0;
    int count;

    while ( cur < end ) {
        count = cp_put( country.codepage ? country.codepage : CP_437, utf8_get( &cur, end ), near_buf );
        if ( used + (unsigned int)count >= max ) {
            break;
        }
        mem_cpy( field + used, near_buf, (unsigned int)count );
        used += (unsigned int)count;
    }
    field[used] = 0;
}

static void field_to_utf8( char *utf8, unsigned int max, const char *field )
{
    uint8_t one[4];
    unsigned int used = 0;
    int count;

    for ( ; *field; field++ ) {
        count = utf8_put( one, cp_to_uni( country.codepage ? country.codepage : CP_437, (uint8_t)*field ) );
        if ( used + (unsigned int)count >= max ) {
            break;
        }
        mem_cpy( utf8 + used, one, (unsigned int)count );
        used += (unsigned int)count;
    }
    utf8[used] = 0;
}

/* a button answers with its own id, and Enter in a list is OK */
static int plain_activate( DLG *dlg, int index )
{
    return dlg->ctl[index].type == CT_BUTTON ? dlg->ctl[index].id : ID_OK;
}

/* OK and Cancel along the bottom of a box */
static int ok_cancel( CTL *ctl, int row, int cols )
{
    ctl_button( &ctl[0], row, cols - 26, "OK", ID_OK, 1 );
    ctl_button( &ctl[1], row, cols - 14, "Cancel", ID_CANCEL, 0 );
    return 2;
}

/* ------------------------------------------------------------------ */
/* one thing chosen from a list                                        */
/* ------------------------------------------------------------------ */

int dlg_pick( const char *title, const char *const *names, int count, int start )
{
    CTL ctl[3];
    DLG dlg;
    int height = count < 12 ? count + 2 : 14;

    if ( count <= 0 ) {
        return -1;
    }
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = title;
    dlg.rows = height + 5;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_list( &ctl[0], 1, 2, 36, height, NULL, names, count );
    ctl[0].sel = start >= 0 && start < count ? start : 0;
    dlg.nctl = 1 + ok_cancel( &ctl[1], height + 2, dlg.cols );
    if ( dlg_run( &dlg ) != ID_OK ) {
        return -1;
    }
    return ctl[0].sel;
}

/* ------------------------------------------------------------------ */
/* Format > Font                                                       */
/* ------------------------------------------------------------------ */

static const char *const color_names[] = {
    "Auto", "Black", "Blue", "Cyan", "Green", "Magenta", "Red", "Yellow", "White", "Dark Blue",
    "Dark Cyan", "Dark Green", "Dark Magenta", "Dark Red", "Dark Yellow", "Dark Gray", "Light Gray"
};

static const uint32_t color_values[] = {
    COLOR_AUTO, 0x000000UL, 0x0000FFUL, 0x00FFFFUL, 0x00FF00UL, 0xFF00FFUL, 0xFF0000UL, 0xFFFF00UL,
    0xFFFFFFUL, 0x000080UL, 0x008080UL, 0x008000UL, 0x800080UL, 0x800000UL, 0x808000UL, 0x808080UL,
    0xC0C0C0UL
};

#define NCOLORS         17

void dlg_font( void )
{
    static const char *const always[] = { "Times New Roman", "Arial", "Courier New", "Symbol" };
    static const uint16_t flag_of[] = { CF_BOLD, CF_ITALIC, CF_UNDER, CF_DUNDER, CF_STRIKE, CF_HIDDEN,
                                   CF_SMALLCAPS, CF_ALLCAPS };
    static const char *const flag_names[] = { "&Bold", "&Italic", "&Underline", "&Double Underline",
                                              "Stri&kethrough", "&Hidden", "S&mall Caps", "&All Caps" };
    EDITOR *ed = ED;
    const char *names[64];
    CTL ctl[24];
    DLG dlg;
    CHP was, now;
    char size[12];
    int points;
    uint32_t mask = 0;
    unsigned int index;
    uint16_t flag_mask = 0;
    int flags[8], place, place_was, color_was = 0, count, used = 0;

    for ( index = 0; index < 4; index++ ) {
        wd_font( ed->doc, always[index], FAM_ANY, -1 );
    }
    /* and the families of the TrueType files that were found (TF.H),
       while the list has room */
    for ( index = 0; ed->doc->nfont < 64 && tf_family( index ) != NULL; index++ ) {
        wd_font( ed->doc, tf_family( index ), FAM_ANY, -1 );
    }
    count = ed->doc->nfont > 64 ? 64 : (int)ed->doc->nfont;
    for ( index = 0; index < (unsigned int)count; index++ ) {
        names[index] = ed->doc->font[index].name;
    }
    ed_cur_chp( ed, &was );
    now = was;
    for ( index = 0; index < NCOLORS; index++ ) {
        if ( color_values[index] == was.color ) {
            color_was = (int)index;
        }
    }
    for ( index = 0; index < 8; index++ ) {
        flags[index] = (was.flags & flag_of[index]) != 0;
    }
    place = place_was = (was.flags & CF_SUPER) ? 1 : (was.flags & CF_SUB) ? 2 : 0;
    sfmt( size, sizeof( size ), (was.size & 1) ? "%u.5" : "%u", (uint32_t)was.size / 2 );

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Font";
    dlg.rows = 17;
    dlg.cols = 62;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "&Font:" );
    ctl_drop( &ctl[used], 2, 2, 28, names, count );
    ctl[used++].sel = was.font < (unsigned int)count ? was.font : 0;
    ctl_label( &ctl[used++], 1, 33, "&Size:" );
    ctl_edit( &ctl[used++], 2, 33, 7, size, sizeof( size ) );
    ctl_label( &ctl[used++], 1, 43, "&Color:" );
    ctl_drop( &ctl[used], 2, 43, 16, color_names, NCOLORS );
    ctl[used++].sel = color_was;
    ctl_frame( &ctl[used++], 4, 2, 36, 10, "Style" );
    for ( index = 0; index < 8; index++ ) {
        ctl_check( &ctl[used++], 5 + (int)index, 4, flag_names[index], &flags[index] );
    }
    ctl_frame( &ctl[used++], 4, 40, 20, 5, "Position" );
    ctl_radio( &ctl[used++], 5, 42, "&Normal", &place, 0 );
    ctl_radio( &ctl[used++], 6, 42, "Su&perscript", &place, 1 );
    ctl_radio( &ctl[used++], 7, 42, "Subsc&ript", &place, 2 );
    dlg.sep = 14;
    used += ok_cancel( &ctl[used], 15, dlg.cols );
    dlg.nctl = used;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        if ( pt_parse( size, &points ) && points >= 20 && points <= 10000 ) {
            break;
        }
        msg_box( "The size is a number of points from 1 to 500.", MB_OK );
    }
    /* only what was changed */
    now.font = (uint16_t)ctl[1].sel;
    if ( now.font != was.font ) {
        mask |= CM_FONT;
    }
    now.size = (uint16_t)(points / 10);
    if ( now.size != was.size ) {
        mask |= CM_SIZE;
    }
    now.color = color_values[ctl[5].sel < NCOLORS ? ctl[5].sel : 0];
    if ( ctl[5].sel != color_was ) {
        mask |= CM_COLOR;
    }
    now.flags = 0;
    for ( index = 0; index < 8; index++ ) {
        if ( flags[index] ) {
            now.flags |= flag_of[index];
        }
        if ( flags[index] != ((was.flags & flag_of[index]) != 0) ) {
            flag_mask |= flag_of[index];
        }
    }
    if ( place != place_was ) {
        flag_mask |= CF_SUPER | CF_SUB;
        now.flags |= (uint16_t)(place == 1 ? CF_SUPER : place == 2 ? CF_SUB : 0);
    }
    if ( flag_mask ) {
        mask |= CM_FLAGS;
    }
    if ( mask ) {
        ed_set_chp( ed, mask, &now, flag_mask );
    }
}

/* ------------------------------------------------------------------ */
/* Format > Paragraph                                                  */
/* ------------------------------------------------------------------ */

void dlg_paragraph( void )
{
    static const char *const aligns[] = { "Left", "Centered", "Right", "Justified" };
    static const char *const spacings[] = { "Single", "1.5 Lines", "Double", "At Least", "Exactly" };
    EDITOR *ed = ED;
    const PAP *was = ed_cur_pap( ed );
    PAP now;
    CTL ctl[28];
    DLG dlg;
    char left[16], right[16], first[16], before[12], after[12], at[12];
    char left_was[16], right_was[16], first_was[16], before_was[12], after_was[12], at_was[12];
    int value;
    uint32_t mask = 0;
    uint16_t flag_mask = 0;
    int page_before, keep_next, keep, spacing, spacing_was, used = 0, align_at, spacing_at;

    len_text( was->left, left );
    len_text( was->right, right );
    len_text( was->first, first );
    pt_text( was->before, before );
    pt_text( was->after, after );
    spacing = was->line_rule == LS_EXACT ? 4 : was->line_rule == LS_ATLEAST ? 3 :
              was->line >= 480 ? 2 : was->line >= 360 ? 1 : 0;
    spacing_was = spacing;
    at[0] = 0;
    if ( spacing >= 3 ) {
        pt_text( was->line, at );
    }
    str_cpy( left_was, left );
    str_cpy( right_was, right );
    str_cpy( first_was, first );
    str_cpy( before_was, before );
    str_cpy( after_was, after );
    str_cpy( at_was, at );
    page_before = (was->flags & PF_PAGEBEFORE) != 0;
    keep_next = (was->flags & PF_KEEPNEXT) != 0;
    keep = (was->flags & PF_KEEP) != 0;

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Paragraph";
    dlg.rows = 18;
    dlg.cols = 62;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "Ali&gnment:" );
    align_at = used;
    ctl_drop( &ctl[used], 1, 14, 14, aligns, 4 );
    ctl[used++].sel = was->align <= AL_JUSTIFY ? was->align : 0;
    ctl_frame( &ctl[used++], 3, 2, 28, 5, "Indentation" );
    ctl_label( &ctl[used++], 4, 4, "&Left:" );
    ctl_edit( &ctl[used++], 4, 17, 10, left, sizeof( left ) );
    ctl_label( &ctl[used++], 5, 4, "&Right:" );
    ctl_edit( &ctl[used++], 5, 17, 10, right, sizeof( right ) );
    ctl_label( &ctl[used++], 6, 4, "&First line:" );
    ctl_edit( &ctl[used++], 6, 17, 10, first, sizeof( first ) );
    ctl_frame( &ctl[used++], 3, 31, 29, 6, "Spacing" );
    ctl_label( &ctl[used++], 4, 33, "&Before:" );
    ctl_edit( &ctl[used++], 4, 47, 10, before, sizeof( before ) );
    ctl_label( &ctl[used++], 5, 33, "Aft&er:" );
    ctl_edit( &ctl[used++], 5, 47, 10, after, sizeof( after ) );
    ctl_label( &ctl[used++], 6, 33, "Li&ne spacing:" );
    spacing_at = used;
    ctl_drop( &ctl[used], 6, 47, 12, spacings, 5 );
    ctl[used++].sel = spacing;
    ctl_label( &ctl[used++], 7, 33, "&At:" );
    ctl_edit( &ctl[used++], 7, 47, 10, at, sizeof( at ) );
    ctl_check( &ctl[used++], 10, 4, "&Page break before", &page_before );
    ctl_check( &ctl[used++], 11, 4, "Keep with ne&xt", &keep_next );
    ctl_check( &ctl[used++], 12, 4, "&Keep lines together", &keep );
    ctl_text( &ctl[used++], 13, 4, 0, "A first line of less than nothing hangs to the left." );
    dlg.sep = 15;
    used += ok_cancel( &ctl[used], 16, dlg.cols );
    dlg.nctl = used;

    pap_init( &now );
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        mask = 0;
        flag_mask = 0;
        if ( ctl[align_at].sel != was->align ) {
            now.align = (uint8_t)ctl[align_at].sel;
            mask |= PM_ALIGN;
        }
        if ( str_cmp( left, left_was ) != 0 ) {
            if ( !len_parse( left, &value ) || value < 0 ) {
                msg_box( "The left indent is not a length.", MB_OK );
                continue;
            }
            now.left = value;
            mask |= PM_LEFT;
        }
        if ( str_cmp( right, right_was ) != 0 ) {
            if ( !len_parse( right, &value ) || value < 0 ) {
                msg_box( "The right indent is not a length.", MB_OK );
                continue;
            }
            now.right = value;
            mask |= PM_RIGHT;
        }
        if ( str_cmp( first, first_was ) != 0 ) {
            if ( !len_parse( first, &value ) ) {
                msg_box( "The first line's indent is not a length.", MB_OK );
                continue;
            }
            now.first = value;
            mask |= PM_FIRST;
        }
        if ( str_cmp( before, before_was ) != 0 ) {
            if ( !pt_parse( before, &value ) || value > 31680 ) {
                msg_box( "The space before is a number of points.", MB_OK );
                continue;
            }
            now.before = (uint16_t)value;
            mask |= PM_BEFORE;
        }
        if ( str_cmp( after, after_was ) != 0 ) {
            if ( !pt_parse( after, &value ) || value > 31680 ) {
                msg_box( "The space after is a number of points.", MB_OK );
                continue;
            }
            now.after = (uint16_t)value;
            mask |= PM_AFTER;
        }
        spacing = ctl[spacing_at].sel;
        if ( spacing != spacing_was || str_cmp( at, at_was ) != 0 ) {
            if ( spacing >= 3 ) {
                if ( !pt_parse( at, &value ) || value < 20 || value > 31680 ) {
                    msg_box( "Say in \"At\" how many points apart the lines are.", MB_OK );
                    continue;
                }
                now.line = (uint16_t)value;
                now.line_rule = (uint8_t)(spacing == 3 ? LS_ATLEAST : LS_EXACT);
            } else {
                now.line = (uint16_t)(spacing == 2 ? 480 : spacing == 1 ? 360 : 240);
                now.line_rule = LS_MULTIPLE;
            }
            mask |= PM_LINE;
        }
        break;
    }
    if ( page_before != ((was->flags & PF_PAGEBEFORE) != 0) ) {
        flag_mask |= PF_PAGEBEFORE;
    }
    if ( keep_next != ((was->flags & PF_KEEPNEXT) != 0) ) {
        flag_mask |= PF_KEEPNEXT;
    }
    if ( keep != ((was->flags & PF_KEEP) != 0) ) {
        flag_mask |= PF_KEEP;
    }
    now.flags = (uint16_t)((page_before ? PF_PAGEBEFORE : 0) | (keep_next ? PF_KEEPNEXT : 0) | (keep ? PF_KEEP : 0));
    if ( flag_mask ) {
        mask |= PM_FLAGS;
    }
    if ( mask ) {
        ed_set_pap( ed, mask, &now, flag_mask );
    }
}

/* ------------------------------------------------------------------ */
/* Format > Tabs                                                       */
/* ------------------------------------------------------------------ */

/*
 * The paragraph's tab stops as one line of text, and back.  A stop is
 * its place and, after it, a letter for its kind - L (or none), C, R,
 * D for decimal - and a mark for what leads up to it: . - or _
 */
static void tabs_text( const PAP *pap, char *buf, unsigned int max )
{
    char one[24];
    unsigned int value;
    int index;

    buf[0] = 0;
    for ( index = 0; index < pap->ntabs && pap->tabs != NULL; index++ ) {
        value = set.inches ? ((unsigned int)pap->tabs[index].pos * 100 + 720) / 1440 :
                             ((unsigned int)pap->tabs[index].pos * 254 + 720) / 1440;
        sfmt( one, sizeof( one ), "%s%u.%02u%c%s", index ? " " : "", value / 100, value % 100,
              "LCRD"[pap->tabs[index].kind & 3], pap->tabs[index].lead == LEAD_DOTS ? "." :
              pap->tabs[index].lead == LEAD_DASHES ? "-" : pap->tabs[index].lead == LEAD_LINE ? "_" : "" );
        if ( !str_catn( buf, one, max ) ) {
            break;
        }
    }
}

static int tabs_parse( const char *text, WTAB *tabs, int *count )
{
    unsigned int value;

    *count = 0;
    for ( ;; ) {
        while ( *text == ' ' || *text == ',' || *text == ';' ) {
            text++;
        }
        if ( *text == 0 ) {
            return 1;
        }
        if ( *count >= MAX_TABS || !number_parse( &text, &value ) ) {
            return 0;
        }
        value = set.inches ? value * 144 / 100 : value * 144 / 254;
        if ( value == 0 || value > 31680 ) {
            return 0;
        }
        mem_set( &tabs[*count], 0, sizeof( WTAB ) );
        tabs[*count].pos = (int)value;
        switch ( ch_upper( *text ) ) {
        case 'L':
            text++;
            break;
        case 'C':
            tabs[*count].kind = TAB_CENTER;
            text++;
            break;
        case 'R':
            tabs[*count].kind = TAB_RIGHT;
            text++;
            break;
        case 'D':
            tabs[*count].kind = TAB_DECIMAL;
            text++;
            break;
        }
        if ( *text == '.' ) {
            tabs[*count].lead = LEAD_DOTS;
            text++;
        } else if ( *text == '-' ) {
            tabs[*count].lead = LEAD_DASHES;
            text++;
        } else if ( *text == '_' ) {
            tabs[*count].lead = LEAD_LINE;
            text++;
        }
        (*count)++;
    }
}

void dlg_tabs( void )
{
    EDITOR *ed = ED;
    CTL ctl[10];
    DLG dlg;
    PAP pap;
    WTAB tabs[MAX_TABS];
    /* room for every stop a paragraph can have, written out in full */
    char text[MAX_TABS * 14], was[MAX_TABS * 14], deftab[16], deftab_was[16];
    int value;
    int count, used = 0;

    tabs_text( ed_cur_pap( ed ), text, sizeof( text ) );
    str_cpy( was, text );
    len_text( ed->doc->deftab, deftab );
    str_cpy( deftab_was, deftab );
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Tabs";
    dlg.rows = 13;
    dlg.cols = 66;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "&Tab stops, from the left margin:" );
    ctl_edit( &ctl[used++], 2, 2, 60, text, sizeof( text ) );
    ctl_text( &ctl[used++], 4, 2, 0, set.inches ?
              "A place for each, in inches, with a space between: 1.5 3 4.25\n"
              "After a place, C centers on it, R sets text to its left, D lines\n"
              "up decimal points; then . - or _ fills the gap: 6R." :
              "A place for each, in centimeters, with a space between: 2.5 5 9\n"
              "After a place, C centers on it, R sets text to its left, D lines\n"
              "up decimal points; then . - or _ fills the gap: 15R." );
    ctl_label( &ctl[used++], 8, 2, "&Default tab stops every:" );
    ctl_edit( &ctl[used++], 8, 28, 10, deftab, sizeof( deftab ) );
    dlg.sep = 10;
    used += ok_cancel( &ctl[used], 11, dlg.cols );
    dlg.nctl = used;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        if ( !tabs_parse( text, tabs, &count ) ) {
            msg_box( "The tab stops are not written as the box says.", MB_OK );
            continue;
        }
        if ( !len_parse( deftab, &value ) || value < 144 || value > 8640 ) {
            msg_box( "The default tab stops are not a length.", MB_OK );
            continue;
        }
        break;
    }
    if ( str_cmp( text, was ) != 0 ) {
        pap_init( &pap );
        pap.tabs = tabs;
        pap.ntabs = (uint8_t)count;
        ed_set_pap( ed, PM_TABS, &pap, 0 );
    }
    if ( str_cmp( deftab, deftab_was ) != 0 ) {
        ed->doc->deftab = value;
        ed->doc->serial++;
        lay_stale( ed->lay );
        view_reset();
    }
}

/* ------------------------------------------------------------------ */
/* File > Page Setup                                                   */
/* ------------------------------------------------------------------ */

static const char *const paper_names[] = {
    "Letter 8.5 x 11 in", "Legal 8.5 x 14 in", "Executive 7.25 x 10.5 in", "A4 210 x 297 mm",
    "A5 148 x 210 mm", "B5 182 x 257 mm", "Custom"
};

static const int paper_sizes[][2] = {
    { 12240, 15840 }, { 12240, 20160 }, { 10440, 15120 }, { 11906, 16838 }, { 8391, 11906 },
    { 10318, 14570 }
};

#define NPAPERS         6

typedef struct {
    char *width, *height;
    int  *landscape;
    int   paper_ctl;
} PAGEDLG;

/* another paper chosen: its size goes into the two fields */
static void page_changed( DLG *dlg, int index )
{
    PAGEDLG *pd = (PAGEDLG *)dlg->data;
    int paper;

    if ( index != pd->paper_ctl ) {
        return;
    }
    paper = dlg->ctl[index].sel;
    if ( paper >= 0 && paper < NPAPERS ) {
        len_text( paper_sizes[paper][*pd->landscape ? 1 : 0], pd->width );
        len_text( paper_sizes[paper][*pd->landscape ? 0 : 1], pd->height );
    }
}

void dlg_page_setup( void )
{
    EDITOR *ed = ED;
    PAGESETUP *page = &ed->doc->page, now;
    PAGEDLG pd;
    CTL ctl[30];
    DLG dlg;
    char width[16], height[16], top[16], bottom[16], left[16], right[16], header[16], footer[16];
    int short_side, long_side, hold;
    int landscape, landscape_was, paper = NPAPERS, index, used = 0;

    landscape = landscape_was = page->width > page->height;
    short_side = landscape ? page->height : page->width;
    long_side = landscape ? page->width : page->height;
    for ( index = 0; index < NPAPERS; index++ ) {
        if ( short_side > paper_sizes[index][0] - 30 && short_side < paper_sizes[index][0] + 30 &&
             long_side > paper_sizes[index][1] - 30 && long_side < paper_sizes[index][1] + 30 ) {
            paper = index;
        }
    }
    len_text( page->width, width );
    len_text( page->height, height );
    len_text( page->top, top );
    len_text( page->bottom, bottom );
    len_text( page->left, left );
    len_text( page->right, right );
    len_text( page->header, header );
    len_text( page->footer, footer );
    pd.width = width;
    pd.height = height;
    pd.landscape = &landscape;

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Page Setup";
    dlg.rows = 17;
    dlg.cols = 64;
    dlg.ctl = ctl;
    dlg.data = &pd;
    dlg.changed = page_changed;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "&Paper:" );
    pd.paper_ctl = used;
    ctl_drop( &ctl[used], 1, 12, 28, paper_names, NPAPERS + 1 );
    ctl[used++].sel = paper;
    ctl_label( &ctl[used++], 3, 2, "&Width:" );
    ctl_edit( &ctl[used++], 3, 12, 10, width, sizeof( width ) );
    ctl_label( &ctl[used++], 4, 2, "H&eight:" );
    ctl_edit( &ctl[used++], 4, 12, 10, height, sizeof( height ) );
    ctl_frame( &ctl[used++], 2, 42, 20, 4, "Orientation" );
    ctl_radio( &ctl[used++], 3, 44, "P&ortrait", &landscape, 0 );
    ctl_radio( &ctl[used++], 4, 44, "L&andscape", &landscape, 1 );
    ctl_frame( &ctl[used++], 6, 2, 60, 5, "Margins" );
    ctl_label( &ctl[used++], 7, 4, "&Top:" );
    ctl_edit( &ctl[used++], 7, 14, 10, top, sizeof( top ) );
    ctl_label( &ctl[used++], 8, 4, "&Bottom:" );
    ctl_edit( &ctl[used++], 8, 14, 10, bottom, sizeof( bottom ) );
    ctl_label( &ctl[used++], 7, 32, "&Left:" );
    ctl_edit( &ctl[used++], 7, 42, 10, left, sizeof( left ) );
    ctl_label( &ctl[used++], 8, 32, "&Right:" );
    ctl_edit( &ctl[used++], 8, 42, 10, right, sizeof( right ) );
    ctl_label( &ctl[used++], 11, 2, "From the edge to the &header:" );
    ctl_edit( &ctl[used++], 11, 32, 10, header, sizeof( header ) );
    ctl_label( &ctl[used++], 12, 2, "From the edge to the &footer:" );
    ctl_edit( &ctl[used++], 12, 32, 10, footer, sizeof( footer ) );
    dlg.sep = 14;
    used += ok_cancel( &ctl[used], 15, dlg.cols );
    dlg.nctl = used;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        now = *page;
        if ( !len_parse( width, &now.width ) || !len_parse( height, &now.height ) || now.width < 1440 ||
             now.height < 1440 ) {
            msg_box( "The paper's width and height are lengths of an inch or more.", MB_OK );
            continue;
        }
        if ( !len_parse( top, &now.top ) || !len_parse( bottom, &now.bottom ) || !len_parse( left, &now.left ) ||
             !len_parse( right, &now.right ) || !len_parse( header, &now.header ) ||
             !len_parse( footer, &now.footer ) || now.top < 0 || now.bottom < 0 || now.left < 0 ||
             now.right < 0 || now.header < 0 || now.footer < 0 ) {
            msg_box( "One of the margins is not a length.", MB_OK );
            continue;
        }
        /* turned on its side, when the orientation changed and the two
           fields were left as they were */
        if ( (landscape != 0) != (now.width > now.height) && landscape != landscape_was ) {
            hold = now.width;
            now.width = now.height;
            now.height = hold;
        }
        if ( now.left + now.right > now.width - 1440 || now.top + now.bottom > now.height - 1440 ) {
            msg_box( "The margins leave less than an inch of the paper for text.", MB_OK );
            continue;
        }
        break;
    }
    now.landscape = (uint8_t)(now.width > now.height);
    *page = now;
    ed->doc->serial++;
    lay_stale( ed->lay );
    view_reset();
}

/* ------------------------------------------------------------------ */
/* Edit > Find, Replace, Go To                                         */
/* ------------------------------------------------------------------ */

static char find_text[64], replace_text[64];

static int find_flags( void )
{
    return (set.find_case ? WF_CASE : 0) | (set.find_word ? WF_WORD : 0);
}

static void not_found( void )
{
    msg_box( "The text was not found.", MB_OK );
}

void find_again( void )
{
    uint16_t what[64];
    unsigned int len;

    if ( find_text[0] == 0 ) {
        dlg_find( 0 );
        return;
    }
    len = to_wide( find_text, what, 64 );
    if ( !ed_find( ED, what, len, find_flags() ) ) {
        not_found();
    }
}

void dlg_find( int replace )
{
    EDITOR *ed = ED;
    CTL ctl[12];
    DLG dlg;
    uint16_t what[64], with[64];
    char said[40];
    unsigned int len, with_len, count;
    int id, used = 0, col;

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = replace ? "Replace" : "Find";
    dlg.rows = replace ? 11 : 9;
    dlg.cols = 64;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    /* low on the screen, so that what is found shows above it */
    dlg.placed = 1;
    dlg.top = scr_rows - dlg.rows - 2;
    dlg.left = (scr_cols - dlg.cols) / 2;
    ctl_label( &ctl[used++], 1, 2, "Fi&nd what:" );
    ctl_edit( &ctl[used++], 1, 18, 42, find_text, sizeof( find_text ) );
    if ( replace ) {
        ctl_label( &ctl[used++], 3, 2, "Re&place with:" );
        ctl_edit( &ctl[used++], 3, 18, 42, replace_text, sizeof( replace_text ) );
    }
    ctl_check( &ctl[used++], replace ? 5 : 3, 2, "Match &case", &set.find_case );
    ctl_check( &ctl[used++], replace ? 6 : 4, 2, "Find &whole words only", &set.find_word );
    dlg.sep = dlg.rows - 3;
    col = 2;
    ctl_button( &ctl[used++], dlg.rows - 2, col, "&Find Next", ID_USER, 1 );
    col += ctl_button_width( "&Find Next" ) + 2;
    if ( replace ) {
        ctl_button( &ctl[used++], dlg.rows - 2, col, "&Replace", ID_USER + 1, 0 );
        col += ctl_button_width( "&Replace" ) + 2;
        ctl_button( &ctl[used++], dlg.rows - 2, col, "Replace &All", ID_USER + 2, 0 );
        col += ctl_button_width( "Replace &All" ) + 2;
    }
    ctl_button( &ctl[used++], dlg.rows - 2, col, "Cancel", ID_CANCEL, 0 );
    dlg.nctl = used;
    for ( ;; ) {
        id = dlg_run( &dlg );
        if ( id == ID_CANCEL || find_text[0] == 0 ) {
            return;
        }
        len = to_wide( find_text, what, 64 );
        with_len = to_wide( replace_text, with, 64 );
        if ( id == ID_USER + 2 ) {
            count = ed_replace_all( ed, what, len, with, with_len, find_flags() );
            if ( count == 0 ) {
                not_found();
            } else {
                sfmt( said, sizeof( said ), count == 1 ? "%u replacement was made." :
                      "%u replacements were made.", count );
                msg_box( said, MB_OK );
            }
            return;
        }
        if ( id == ID_USER + 1 ) {
            ed_replace( ed, what, len, with, with_len, find_flags() );
        }
        if ( !ed_find( ed, what, len, find_flags() ) ) {
            not_found();
            if ( !replace ) {
                return;
            }
            continue;
        }
        if ( !replace ) {
            return;                     /* found: Shift+F4 looks again */
        }
        ed_update( ed );
    }
}

void dlg_goto( void )
{
    EDITOR *ed = ED;
    LINFO info;
    const LPAGE *start;
    WPOS pos;
    char text[8];
    unsigned int page, pages;
    int ok;

    text[0] = 0;
    if ( !input_box( "Go To", "&Page:", text, sizeof( text ), 6 ) ) {
        return;
    }
    page = dec_parse( text, &ok );
    if ( !ok || page == 0 ) {
        return;
    }
    if ( ed == NULL ) {
        if ( PDFV->pages < 1 ) {
            return;
        }
        PDFV->page = (int)(page > (unsigned int)PDFV->pages ? (unsigned int)PDFV->pages : page) - 1;
        PDFV->scroll_y = 0;
        PDFV->top = 0;
        return;
    }
    lay_update( ed->lay );
    pages = lay_pages( ed->lay );
    page--;                             /* sheets are counted from 1, as the status bar counts */
    if ( page >= pages ) {
        page = pages - 1;
    }
    start = lay_page( ed->lay, page );
    pos.para = start->para;
    pos.off = 0;
    if ( lay_line( ed->lay, start->para, start->line, &info ) ) {
        pos.off = info.start;
    }
    ed_goto( ed, pos, 0 );
    ed_scroll_to( ed, start->y );
}

/* ------------------------------------------------------------------ */
/* View > Zoom, Display; Insert > Break                                */
/* ------------------------------------------------------------------ */

void dlg_zoom( void )
{
    static const char *const names[] = { "50%", "75%", "100%", "125%", "150%", "200%", "Page Width" };
    static const int values[] = { 50, 75, 100, 125, 150, 200, 0 };
    int now = PDFV != NULL ? PDFV->zoom : set.zoom, start = 2, index, chosen;

    for ( index = 0; index < 7; index++ ) {
        if ( values[index] == now ) {
            start = index;
        }
    }
    /* "Page Width" is a PDF's: a document's lines are as wide as its page */
    chosen = dlg_pick( "Zoom", names, PDFV != NULL ? 7 : 6, start );
    if ( chosen < 0 ) {
        return;
    }
    if ( PDFV != NULL ) {
        PDFV->zoom = values[chosen];
    } else {
        set.zoom = values[chosen];
    }
    view_reset();
}

void dlg_display( void )
{
    const char *names[24];
    int count = disp_mode_count(), index, chosen;

    if ( count > 24 ) {
        count = 24;
    }
    for ( index = 0; index < count; index++ ) {
        names[index] = disp_mode_info( index )->name;
    }
    chosen = dlg_pick( "Display", names, count, disp_mode_now() );
    if ( chosen >= 0 && chosen != disp_mode_now() ) {
        screen_apply( chosen );
    }
}

void dlg_break( void )
{
    static const char *const names[] = { "Page Break", "Line Break" };
    int chosen = dlg_pick( "Break", names, 2, 0 );

    if ( chosen >= 0 ) {
        ed_break( ED, chosen == 0 ? WCH_PAGE : WCH_LINE );
    }
}

void dlg_table( void )
{
    static char cols[6] = "2", rows[6] = "2";
    CTL ctl[8];
    DLG dlg;
    unsigned int ncols, nrows;
    int used = 0, ok_cols, ok_rows;

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Insert Table";
    dlg.rows = 9;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "Number of &columns:" );
    ctl_edit( &ctl[used++], 1, 24, 6, cols, sizeof( cols ) );
    ctl_label( &ctl[used++], 3, 2, "Number of &rows:" );
    ctl_edit( &ctl[used++], 3, 24, 6, rows, sizeof( rows ) );
    dlg.sep = 6;
    used += ok_cancel( &ctl[used], 7, dlg.cols );
    dlg.nctl = used;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        ncols = dec_parse( cols, &ok_cols );
        nrows = dec_parse( rows, &ok_rows );
        if ( ok_cols && ok_rows && ncols >= 1 && ncols <= 16 && nrows >= 1 && nrows <= 200 ) {
            break;
        }
        msg_box( "A table has 1 to 16 columns and 1 to 200 rows.", MB_OK );
    }
    ed_insert_table( ED, (int)nrows, (int)ncols );
}

/* Table > Column Width: the least is the narrowest cell the layout
   will set a line in (LAY.C, MIN_WIDTH and the room either side) */
#define COL_LEAST       864

void dlg_col_width( void )
{
    char wide[16];
    CTL ctl[6];
    DLG dlg;
    int twips = ed_col_width( ED );
    int used = 0;

    if ( twips <= 0 ) {
        return;
    }
    len_text( twips, wide );
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Column Width";
    dlg.rows = 7;
    dlg.cols = 40;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "&Width of the column:" );
    ctl_edit( &ctl[used++], 1, 24, 12, wide, sizeof( wide ) );
    dlg.sep = 4;
    used += ok_cancel( &ctl[used], 5, dlg.cols );
    dlg.nctl = used;
    for ( ;; ) {
        if ( dlg_run( &dlg ) != ID_OK ) {
            return;
        }
        if ( len_parse( wide, &twips ) && twips >= COL_LEAST && twips <= 31680 ) {
            break;
        }
        msg_box( "A column is from 0.6 inch (1.5 cm) to 22 inches (55.8 cm) wide.", MB_OK );
    }
    ed_set_col_width( ED, twips );
}

/* ------------------------------------------------------------------ */
/* File > Summary Info; Tools > Word Count                             */
/* ------------------------------------------------------------------ */

void dlg_info( void )
{
    WDOC *doc = ED->doc;
    CTL ctl[12];
    DLG dlg;
    char title[INFO_LEN], author[INFO_LEN], subject[INFO_LEN], keywords[INFO_LEN];
    char title_was[INFO_LEN], author_was[INFO_LEN], subject_was[INFO_LEN], keywords_was[INFO_LEN];
    int used = 0, changed = 0;

    field_from_utf8( title, sizeof( title ), doc->title );
    field_from_utf8( author, sizeof( author ), doc->author );
    field_from_utf8( subject, sizeof( subject ), doc->subject );
    field_from_utf8( keywords, sizeof( keywords ), doc->keywords );
    str_cpy( title_was, title );
    str_cpy( author_was, author );
    str_cpy( subject_was, subject );
    str_cpy( keywords_was, keywords );
    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Summary Info";
    dlg.rows = 13;
    dlg.cols = 64;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_label( &ctl[used++], 1, 2, "&Title:" );
    ctl_edit( &ctl[used++], 1, 14, 46, title, sizeof( title ) );
    ctl_label( &ctl[used++], 3, 2, "&Author:" );
    ctl_edit( &ctl[used++], 3, 14, 46, author, sizeof( author ) );
    ctl_label( &ctl[used++], 5, 2, "&Subject:" );
    ctl_edit( &ctl[used++], 5, 14, 46, subject, sizeof( subject ) );
    ctl_label( &ctl[used++], 7, 2, "&Keywords:" );
    ctl_edit( &ctl[used++], 7, 14, 46, keywords, sizeof( keywords ) );
    dlg.sep = 10;
    used += ok_cancel( &ctl[used], 11, dlg.cols );
    dlg.nctl = used;
    if ( dlg_run( &dlg ) != ID_OK ) {
        return;
    }
    /* only what was changed: a field shows what the screen's code page
       has, and one written back untouched would lose what it has not */
    if ( str_cmp( title, title_was ) != 0 ) {
        field_to_utf8( doc->title, INFO_LEN, title );
        changed = 1;
    }
    if ( str_cmp( author, author_was ) != 0 ) {
        field_to_utf8( doc->author, INFO_LEN, author );
        changed = 1;
    }
    if ( str_cmp( subject, subject_was ) != 0 ) {
        field_to_utf8( doc->subject, INFO_LEN, subject );
        changed = 1;
    }
    if ( str_cmp( keywords, keywords_was ) != 0 ) {
        field_to_utf8( doc->keywords, INFO_LEN, keywords );
        changed = 1;
    }
    if ( changed ) {
        doc->serial++;
    }
}

void dlg_count( void )
{
    EDITOR *ed = ED;
    const WPARA *para;
    char text[200];
    unsigned int pi, index, words = 0, chars = 0, lines = 0;
    int in_word;

    lay_update( ed->lay );
    for ( pi = 0; pi < ed->story->count; pi++ ) {
        para = ed->story->para[pi];
        lines += lay_lines( ed->lay, pi );
        in_word = 0;
        for ( index = 0; index < para->len; index++ ) {
            if ( para->text[index] == ' ' || para->text[index] == WCH_TAB || para->text[index] == WCH_LINE ||
                 para->text[index] == WCH_PAGE || para->text[index] == WCH_NBSP ) {
                in_word = 0;
                continue;
            }
            if ( para->text[index] >= 0xDC00 && para->text[index] <= 0xDFFF ) {
                continue;               /* the second half of one character */
            }
            chars++;
            if ( !in_word ) {
                words++;
                in_word = 1;
            }
        }
    }
    sfmt( text, sizeof( text ), "Pages:        %,u\nWords:        %,u\nCharacters:   %,u\nParagraphs:   %,u\n"
          "Lines:        %,u", lay_pages( ed->lay ), words, chars, ed->story->count, lines );
    msg_text( "Word Count", text, MB_OK );
}

/* ------------------------------------------------------------------ */
/* Help                                                                */
/* ------------------------------------------------------------------ */

void dlg_keys( void )
{
    static const char *const lines[] = {
        "F10, or Alt       the menus",
        "Ctrl+N            a new document",
        "Ctrl+O            open a document",
        "Ctrl+S            save",
        "F12               save under a name or in a format",
        "Ctrl+W            close the document",
        "Ctrl+P            print",
        "Alt+F4            leave PMWRITE",
        "Ctrl+F6           the next open document",
        "",
        "Arrows            move the caret",
        "Ctrl+Left, Right  a word at a time",
        "Ctrl+Up, Down     a paragraph at a time",
        "Home, End         the start and end of the line",
        "Ctrl+Home, End    the start and end of the document",
        "PgUp, PgDn        a windowful",
        "Shift and a move  select",
        "Ctrl+A            select everything",
        "",
        "Enter             a new paragraph",
        "Shift+Enter       a new line in the same paragraph",
        "Ctrl+Enter        a new page",
        "Ins               type over, or insert",
        "Ctrl+Backspace    delete the word before",
        "Ctrl+Del          delete the word after",
        "Ctrl+Z, Ctrl+Y    undo, redo",
        "Ctrl+X, C, V      cut, copy, paste",
        "Ctrl+F, Ctrl+H    find, replace",
        "Shift+F4          find again",
        "Ctrl+G            go to a page",
        "",
        "Ctrl+B, I, U      bold, italic, underlined",
        "Ctrl+=            subscript",
        "Ctrl+Shift+=      superscript",
        "Ctrl+L, E, R, J   left, centered, right, justified",
        "Ctrl+1, 5, 2      single, one and a half, double spacing",
        "Ctrl+M            indent; with Shift, back again",
        "Ctrl+Shift+S      choose a style",
        "Ctrl+Shift+F      choose a font",
        "Ctrl+Shift+P      choose a size",
        "",
        "With a PDF showing:",
        "PgUp, PgDn        through the pages",
        "Arrows            move the page in the window",
        "+ and -           larger and smaller",
        "File menu         Edit PDF as Text makes a document of it"
    };
    CTL ctl[2];
    DLG dlg;
    int height = scr_rows - 8 > 20 ? 20 : scr_rows - 8;

    mem_set( &dlg, 0, sizeof( dlg ) );
    dlg.title = "Keys";
    dlg.rows = height + 5;
    dlg.cols = 64;
    dlg.ctl = ctl;
    dlg.activate = plain_activate;
    ctl_list( &ctl[0], 1, 2, 60, height, NULL, lines, sizeof( lines ) / sizeof( lines[0] ) );
    ctl_button( &ctl[1], height + 2, 27, "Close", ID_CANCEL, 1 );
    dlg.nctl = 2;
    dlg_run( &dlg );
}

void dlg_about( void )
{
    msg_text( "About PMWRITE", "PMWRITE\nVersion " PW_VERSION "\n\nA word processor for PM-DOS.", MB_OK );
}
