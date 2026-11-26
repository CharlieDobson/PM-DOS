/*
 * TFSTUB.C - a stand-in for TF.C in the layout tests on Windows, from
 * before TF.C was written and for tests that want widths that never
 * change: every character of a proportional font is half an em wide,
 * a space a quarter, and a fixed-pitch one six tenths.  Nothing here
 * ships, and nothing is drawn.
 */
#include "tf.h"

struct TFACE {
    int base;
    int flags;
};

static struct TFACE faces[4][4];

void tf_init( void )
{
}

void tf_done( void )
{
}

TFACE *tf_face( const WFONT *font, int flags )
{
    int base = font->fixed ? TFB_COURIER : font->family == FAM_SWISS ? TFB_HELVETICA :
               font->family == FAM_SYMBOL ? TFB_SYMBOL : TFB_TIMES;
    int style = ((flags & CF_BOLD) ? 1 : 0) | ((flags & CF_ITALIC) ? 2 : 0);

    faces[base][style].base = base;
    faces[base][style].flags = flags & (CF_BOLD | CF_ITALIC);
    return &faces[base][style];
}

int tf_base( const TFACE *face )
{
    return face->base;
}

int tf_is_builtin( const TFACE *face )
{
    (void)face;
    return 1;
}

int tf_flags( const TFACE *face )
{
    return face->flags;
}

unsigned int tf_advance( TFACE *face, uint32_t code )
{
    if ( face->base == TFB_COURIER ) {
        return 600;
    }
    return code == ' ' ? 250 : 500;
}

void tf_metrics( TFACE *face, int *ascent, int *descent, int *gap )
{
    (void)face;
    *ascent = 800;
    *descent = 200;
    *gap = 0;
}

int32_t tf_draw( TFACE *face, int32_t em, int32_t pen_x, int32_t base_y, const uint16_t *text, unsigned int len, COVER *cover )
{
    (void)face;
    (void)em;
    (void)base_y;
    (void)text;
    (void)len;
    (void)cover;
    return pen_x;
}

int tf_reclaim( void )
{
    return 0;
}
