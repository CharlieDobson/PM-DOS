/*
 * MDOPL.C - MIDI voices as register writes for a Yamaha OPL2 or OPL3
 * (MIDI.H).
 *
 * AN OPL VOICE IS A CHANNEL OF TWO OPERATORS.  The OPL2 has nine, the
 * OPL3 eighteen in two sets of registers (the second set is DECEV bank
 * 1).  Each channel's modulator and carrier sit at fixed places in the
 * operator registers (20h to F5h); its frequency, key and feedback are
 * A0h, B0h and C0h plus the channel's number.  All of them are 2-op
 * voices: the OPL3's 4-op mode stays off.
 *
 * ONLY WHAT CHANGED IS WRITTEN.  A note on a voice that already holds
 * the patch writes its levels, its frequency and its key; the rest of a
 * patch is written only when the voice last played another one.  On an
 * OPL2 the driver waits some 23 microseconds after every write.
 *
 * THE KEY MUST GO UP BEFORE IT GOES DOWN AGAIN.  An envelope starts
 * when the key bit goes from 0 to 1, so a voice still keyed is keyed
 * off first, and its patch and levels are written between that write
 * and the new key-on, which gives the chip the time to see the change.
 *
 * LOUDNESS IS THE CARRIER'S TOTAL LEVEL.  Velocity, volume and
 * expression are added to the patch's own level in the chip's 0.75 dB
 * steps.  With the two operators in series the modulator's level is
 * the brightness, and a quarter of the velocity's loss goes there too,
 * so a soft note is a duller one as well; with the two added together
 * both are heard, and both take the whole loss.
 *
 * PAN ON AN OPL3 IS LEFT, BOTH OR RIGHT (C0h bits 4 and 5, which a
 * Sound Blaster wires to its left and right).  An OPL2 has one output.
 * Controller 1 turns on the chip's own vibrato (BDh makes it the deeper
 * one, 14 cents).
 */
#include "rt.h"
#include "fix.h"
#include "dec.h"
#include "midi.h"

#define OPL_BD          0x40    /* deep vibrato, light tremolo, no rhythm mode */
#define VIB_FROM        32      /* controller 1 from this up turns the vibrato on */

/* the modulator's operator for each channel of a register set; the
   carrier's is 3 further on */
static const uint8_t opl_slot[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };

static void opl_reg( MDSTATE *md, int bank, int reg, int val )
{
    md_stage( md, EV_OPL, (uint8_t)bank, (uint8_t)reg, (uint8_t)val );
}

/* -> the register set; *chan the channel in it */
static int opl_where( int voice, int *chan )
{
    *chan = voice % 9;
    return voice / 9;
}

static uint8_t opl_misc( const MDSTATE *md, const MDVOICE *vc, int op )
{
    uint8_t misc = vc->patch->op[op].misc;

    if ( !vc->drum && md->chan[vc->chan].modulation >= VIB_FROM ) {
        misc |= OP_VIB;
    }
    return misc;
}

static uint8_t opl_c0( const MDSTATE *md, const MDVOICE *vc )
{
    uint8_t val = (uint8_t)(vc->patch->fbc & 0x0F), pan = md->chan[vc->chan].pan;

    if ( md->synth == SYN_OPL3 ) {
        val |= pan < 32 ? 0x10 : pan >= 96 ? 0x20 : 0x30;
    }
    return val;
}

void opl_silence( MDSTATE *md )
{
    MDVOICE *vc;
    int v, chan, bank, slot;

    for ( v = 0; v < (int)md->voices; v++ ) {
        vc = &md->voice[v];
        bank = opl_where( v, &chan );
        slot = opl_slot[chan];
        opl_reg( md, bank, 0x40 + slot, 0x3F );
        opl_reg( md, bank, 0x43 + slot, 0x3F );
        opl_reg( md, bank, 0x80 + slot, 0x0F );
        opl_reg( md, bank, 0x83 + slot, 0x0F );
        opl_reg( md, bank, 0xB0 + chan, 0x00 );
        vc->opl_a0 = 0;
        vc->opl_b0 = 0;
        vc->opl_c0 = 0xFF;
        vc->opl_40[0] = vc->opl_40[1] = 0x3F;
        vc->opl_vib = 0xFF;
        vc->loaded = NULL;              /* 80h was changed: the patch goes in again */
    }
}

void opl_setup( MDSTATE *md )
{
    if ( md->synth == SYN_OPL3 ) {
        opl_reg( md, 1, 0x05, 0x01 );   /* OPL3 mode */
        opl_reg( md, 1, 0x04, 0x00 );   /* no 4-op voices */
    }
    opl_reg( md, 0, 0x01, 0x20 );       /* the waveforms enabled */
    opl_reg( md, 0, 0x08, 0x00 );
    opl_reg( md, 0, 0xBD, OPL_BD );
    opl_silence( md );
}

void opl_level( MDSTATE *md, int voice )
{
    MDVOICE *vc = &md->voice[voice];
    const MDPATCH *patch = vc->patch;
    uint32_t vol, bright, tl[2];
    int chan, bank = opl_where( voice, &chan ), op;
    uint8_t val;

    vol = (md_voice_att( md, vc ) + 16) >> 5;
    bright = ((md_vel_att[vc->vel & 0x7F] + 16) >> 5) >> 2;
    tl[0] = (uint32_t)(patch->op[0].level & 0x3F) + ((patch->fbc & 1) ? vol : bright);
    tl[1] = (uint32_t)(patch->op[1].level & 0x3F) + vol;
    for ( op = 0; op < 2; op++ ) {
        val = (uint8_t)((patch->op[op].level & 0xC0) | (tl[op] > 63 ? 63 : tl[op]));
        if ( val != vc->opl_40[op] ) {
            opl_reg( md, bank, 0x40 + opl_slot[chan] + op * 3, val );
            vc->opl_40[op] = val;
        }
    }
}

void opl_pan( MDSTATE *md, int voice )
{
    MDVOICE *vc = &md->voice[voice];
    int chan, bank = opl_where( voice, &chan );
    uint8_t val = opl_c0( md, vc );

    if ( val != vc->opl_c0 ) {
        opl_reg( md, bank, 0xC0 + chan, val );
        vc->opl_c0 = val;
    }
}

void opl_vibrato( MDSTATE *md, int voice )
{
    MDVOICE *vc = &md->voice[voice];
    int chan, bank = opl_where( voice, &chan ), op;
    uint8_t vib;

    if ( vc->loaded == NULL ) {
        return;
    }
    vib = (uint8_t)(opl_misc( md, vc, 1 ) & OP_VIB);
    if ( vib == vc->opl_vib ) {
        return;
    }
    for ( op = 0; op < 2; op++ ) {
        opl_reg( md, bank, 0x20 + opl_slot[chan] + op * 3, opl_misc( md, vc, op ) );
    }
    vc->opl_vib = vib;
}

/* A0h and B0h for the voice's pitch now, with the key as "key" says */
static void opl_freq( MDSTATE *md, int voice, int key )
{
    MDVOICE *vc = &md->voice[voice];
    int chan, bank = opl_where( voice, &chan );
    uint32_t fnum, block;
    uint8_t a0, b0;

    md_fnum( md_pitch_hz( md_voice_pitch( md, vc ) ), &fnum, &block );
    a0 = (uint8_t)(fnum & 0xFF);
    b0 = (uint8_t)((key ? 0x20 : 0) | (block << 2) | (fnum >> 8));
    if ( a0 != vc->opl_a0 ) {
        opl_reg( md, bank, 0xA0 + chan, a0 );
        vc->opl_a0 = a0;
    }
    if ( b0 != vc->opl_b0 ) {
        opl_reg( md, bank, 0xB0 + chan, b0 );
        vc->opl_b0 = b0;
    }
}

void opl_start( MDSTATE *md, int voice )
{
    MDVOICE *vc = &md->voice[voice];
    const MDPATCH *patch = vc->patch;
    int chan, bank = opl_where( voice, &chan ), op, slot;

    if ( vc->opl_b0 & 0x20 ) {
        vc->opl_b0 &= (uint8_t)~0x20;
        opl_reg( md, bank, 0xB0 + chan, vc->opl_b0 );
    }
    if ( vc->loaded != patch ) {
        for ( op = 0; op < 2; op++ ) {
            slot = opl_slot[chan] + op * 3;
            opl_reg( md, bank, 0x20 + slot, opl_misc( md, vc, op ) );
            opl_reg( md, bank, 0x60 + slot, patch->op[op].ad );
            opl_reg( md, bank, 0x80 + slot, patch->op[op].sr );
            opl_reg( md, bank, 0xE0 + slot, patch->op[op].wave & 3 );
        }
        vc->loaded = patch;
        vc->opl_vib = (uint8_t)(opl_misc( md, vc, 1 ) & OP_VIB);
        vc->opl_c0 = 0xFF;
    } else {
        opl_vibrato( md, voice );
    }
    opl_pan( md, voice );
    opl_level( md, voice );
    opl_freq( md, voice, 1 );
}

void opl_stop( MDSTATE *md, int voice )
{
    MDVOICE *vc = &md->voice[voice];
    int chan, bank = opl_where( voice, &chan );

    if ( vc->opl_b0 & 0x20 ) {
        vc->opl_b0 &= (uint8_t)~0x20;
        opl_reg( md, bank, 0xB0 + chan, vc->opl_b0 );
    }
}

void opl_kill( MDSTATE *md, int voice )
{
    MDVOICE *vc = &md->voice[voice];
    int chan, bank = opl_where( voice, &chan ), op;

    opl_stop( md, voice );
    for ( op = 0; op < 2; op++ ) {
        if ( vc->opl_40[op] != 0x3F && (op == 1 || (vc->patch != NULL && (vc->patch->fbc & 1))) ) {
            opl_reg( md, bank, 0x40 + opl_slot[chan] + op * 3, 0x3F );
            vc->opl_40[op] = 0x3F;
        }
    }
}

void opl_pitch( MDSTATE *md, int voice )
{
    opl_freq( md, voice, (md->voice[voice].opl_b0 & 0x20) != 0 );
}
