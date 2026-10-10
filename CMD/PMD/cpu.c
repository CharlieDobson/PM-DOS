/*
 * CPU.C - which processor this is.
 *
 * MSD's test stops at the 486: it tells an 8088 from a 286 from a 386
 * by what the flags register will hold, and a 486 from a 386 by the AC
 * flag, and anything later is a 486 to it - or "Unknown".  This asks
 * the processor itself where the processor can be asked:
 *
 *   THE ID FLAG HOLDS    there is a CPUID instruction.  Leaf 0 is the
 *                        maker's twelve letters; leaf 1 the family,
 *                        model and stepping and what it can do; leaves
 *                        80000002h-4h, where they exist, the name the
 *                        maker gave it, which is believed over any
 *                        table here.
 *   ONLY THE AC FLAG     a 486 from before CPUID - or a Cyrix, which
 *                        a divide gives away (it leaves the flags
 *                        alone) and whose own registers at ports 22h
 *                        and 23h say which.
 *   NEITHER              a 386.  PM-DOS runs on nothing older, so
 *                        MSD's tests for the 8088, the V20 and the 286
 *                        have nothing to find here.
 *
 * The coprocessor is the kernel's to report (21h/F1h AL=13h): it set
 * the machine up for one or for none, and an x87 instruction with none
 * would be a fault.
 */
#include "pmd.h"

typedef struct {
    u8          family, model;      /* model FFh: any of that family */
    const char *name;
} MODEL;

static const MODEL intel_models[] = {
    { 4, 0, "486DX" },              { 4, 1, "486DX" },
    { 4, 2, "486SX" },              { 4, 3, "486DX2" },
    { 4, 4, "486SL" },              { 4, 5, "486SX2" },
    { 4, 7, "486DX2 (write-back)" },{ 4, 8, "486DX4" },
    { 4, 9, "486DX4 (write-back)" },{ 4, 0xFF, "486" },
    { 5, 0, "Intel Pentium (A-step)" },
    { 5, 1, "Intel Pentium" },      { 5, 2, "Intel Pentium" },
    { 5, 3, "Intel Pentium OverDrive" },
    { 5, 4, "Intel Pentium MMX" },  { 5, 7, "Intel Mobile Pentium" },
    { 5, 8, "Intel Mobile Pentium MMX" },
    { 5, 0xFF, "Intel Pentium" },
    { 6, 0, "Intel Pentium Pro (A-step)" },
    { 6, 1, "Intel Pentium Pro" },  { 6, 3, "Intel Pentium II" },
    { 6, 5, "Intel Pentium II" },   { 6, 6, "Intel Celeron" },
    { 6, 7, "Intel Pentium III" },  { 6, 8, "Intel Pentium III" },
    { 6, 9, "Intel Pentium M" },    { 6, 10, "Intel Pentium III Xeon" },
    { 6, 11, "Intel Pentium III" }, { 6, 13, "Intel Pentium M" },
    { 6, 14, "Intel Core" },        { 6, 15, "Intel Core 2" },
    { 6, 0xFF, "Intel P6 family" },
    { 15, 0xFF, "Intel Pentium 4" },
    { 0, 0, NULL }
};

static const MODEL amd_models[] = {
    { 4, 3, "Am486DX2" },           { 4, 7, "Am486DX2 (write-back)" },
    { 4, 8, "Am486DX4" },           { 4, 9, "Am486DX4 (write-back)" },
    { 4, 14, "Am5x86" },            { 4, 15, "Am5x86 (write-back)" },
    { 4, 0xFF, "Am486" },
    { 5, 0, "AMD K5" },             { 5, 1, "AMD K5" },
    { 5, 2, "AMD K5" },             { 5, 3, "AMD K5" },
    { 5, 6, "AMD K6" },             { 5, 7, "AMD K6" },
    { 5, 8, "AMD K6-2" },           { 5, 9, "AMD K6-III" },
    { 5, 13, "AMD K6-2+ or K6-III+" },
    { 5, 0xFF, "AMD K5 or K6 family" },
    { 6, 1, "AMD Athlon" },         { 6, 2, "AMD Athlon" },
    { 6, 3, "AMD Duron" },          { 6, 4, "AMD Athlon" },
    { 6, 6, "AMD Athlon XP" },      { 6, 7, "AMD Duron" },
    { 6, 8, "AMD Athlon XP" },      { 6, 10, "AMD Athlon XP" },
    { 6, 0xFF, "AMD Athlon family" },
    { 15, 0xFF, "AMD Athlon 64" },
    { 0, 0, NULL }
};

static const MODEL cyrix_models[] = {
    { 4, 4, "Cyrix MediaGX" },      { 4, 9, "Cyrix 5x86" },
    { 4, 0xFF, "Cyrix 486" },
    { 5, 2, "Cyrix 6x86" },         { 5, 4, "Cyrix MediaGXm" },
    { 5, 0xFF, "Cyrix 6x86" },
    { 6, 0, "Cyrix 6x86MX or M II" },
    { 6, 5, "VIA Cyrix III" },      { 6, 0xFF, "Cyrix M II" },
    { 0, 0, NULL }
};

static const MODEL centaur_models[] = {
    { 5, 4, "IDT WinChip C6" },     { 5, 8, "IDT WinChip 2" },
    { 5, 9, "IDT WinChip 3" },      { 5, 0xFF, "IDT WinChip" },
    { 6, 6, "VIA C3 (Samuel)" },    { 6, 7, "VIA C3" },
    { 6, 8, "VIA C3" },             { 6, 9, "VIA C3 (Nehemiah)" },
    { 6, 0xFF, "VIA C7" },
    { 0, 0, NULL }
};

static const MODEL umc_models[] = {
    { 4, 1, "UMC U5D" },            { 4, 2, "UMC U5S" },
    { 4, 0xFF, "UMC 486" },
    { 0, 0, NULL }
};

static const MODEL nexgen_models[] = {
    { 5, 0xFF, "NexGen Nx586" },
    { 0, 0, NULL }
};

static const MODEL rise_models[] = {
    { 5, 0xFF, "Rise mP6" },
    { 0, 0, NULL }
};

static const MODEL transmeta_models[] = {
    { 5, 0xFF, "Transmeta Crusoe" },
    { 15, 0xFF, "Transmeta Efficeon" },
    { 0, 0, NULL }
};

static const MODEL sis_models[] = {
    { 5, 0xFF, "SiS 55x" },
    { 0, 0, NULL }
};

static const MODEL nsc_models[] = {
    { 5, 0xFF, "National Semiconductor Geode" },
    { 0, 0, NULL }
};

typedef struct {
    const char  *id;                /* CPUID leaf 0's twelve letters */
    const char  *maker;             /* for a model no table has */
    const MODEL *models;
} VENDOR;

static const VENDOR vendors[] = {
    { "GenuineIntel", "Intel",     intel_models },
    { "AuthenticAMD", "AMD",       amd_models },
    { "AMDisbetter!", "AMD",       amd_models },
    { "CyrixInstead", "Cyrix",     cyrix_models },
    { "CentaurHauls", "Centaur",   centaur_models },
    { "UMC UMC UMC ", "UMC",       umc_models },
    { "NexGenDriven", "NexGen",    nexgen_models },
    { "RiseRiseRise", "Rise",      rise_models },
    { "GenuineTMx86", "Transmeta", transmeta_models },
    { "SiS SiS SiS ", "SiS",       sis_models },
    { "Geode by NSC", "National Semiconductor", nsc_models },
    { NULL, NULL, NULL }
};

static const char *model_name( const MODEL *models, int family, int model )
{
    const char *any = NULL;

    for ( ; models->name; models++ ) {
        if ( models->family != family ) {
            continue;
        }
        if ( models->model == model ) {
            return models->name;
        }
        if ( models->model == 0xFF ) {
            any = models->name;
        }
    }
    return any;
}

/* what leaf 1's EDX (and the extended leaf's, for 3DNow!) says the
   processor can do: the ones a DOS program might care about */
static void feature_text( char *out, u32 max, u32 edx, u32 ext_edx )
{
    static const struct {
        u8          bit;
        const char *name;
    } bits[] = {
        { 0, "FPU" }, { 4, "TSC" }, { 5, "MSR" }, { 8, "CX8" }, { 15, "CMOV" },
        { 23, "MMX" }, { 24, "FXSR" }, { 25, "SSE" }, { 26, "SSE2" }
    };
    int index;

    out[0] = 0;
    for ( index = 0; index < (int)(sizeof( bits ) / sizeof( bits[0] )); index++ ) {
        if ( edx & (1UL << bits[index].bit) ) {
            if ( out[0] ) {
                str_catn( out, " ", max );
            }
            str_catn( out, bits[index].name, max );
        }
    }
    if ( ext_edx & 0x80000000UL ) {
        if ( out[0] ) {
            str_catn( out, " ", max );
        }
        str_catn( out, "3DNow!", max );
    }
}

/* A Cyrix chip from before its CPUID says which it is in DIR0, register
   FEh behind ports 22h and 23h. */
static const char *cyrix_by_dir0( void )
{
    unsigned dir0;

    port_out( 0x22, 0xFE );
    dir0 = port_in( 0x23 );
    if ( dir0 <= 0x07 ) {
        return "Cyrix Cx486SLC/DLC";
    }
    if ( dir0 >= 0x10 && dir0 <= 0x13 ) {
        return "Cyrix Cx486S";
    }
    if ( dir0 >= 0x1A && dir0 <= 0x1F ) {
        return "Cyrix Cx486DX/DX2";
    }
    if ( dir0 >= 0x28 && dir0 <= 0x2F ) {
        return "Cyrix 5x86";
    }
    if ( dir0 >= 0x30 && dir0 <= 0x3F ) {
        return "Cyrix 6x86";
    }
    if ( dir0 >= 0x40 && dir0 <= 0x4F ) {
        return "Cyrix MediaGX";
    }
    if ( dir0 >= 0x50 && dir0 <= 0x5F ) {
        return "Cyrix 6x86MX";
    }
    return "Cyrix 486";
}

void cpu_identify( CPUINFO *cpu )
{
    u32 regs[4], top, ext_edx = 0, sig, feat;
    const VENDOR *vendor;
    const char *name;
    char *brand;
    int index, x87 = (sys_fpu_flags() & FPUF_X87) != 0;

    mem_set( cpu, 0, sizeof( *cpu ) );
    if ( !cpu_flag_holds( EFL_ID ) ) {
        /* no CPUID: the flags and a divide are all there is to go on */
        if ( !cpu_flag_holds( EFL_AC ) ) {
            cpu->level = 3;
            str_cpy( cpu->name, "80386" );
            return;
        }
        cpu->level = 4;
        if ( cpu_div_keeps_flags() ) {
            str_cpy( cpu->name, cyrix_by_dir0() );
            cpu->fpu_internal = x87;
            return;
        }
        str_cpy( cpu->name, x87 ? "486DX" : "486SX" );
        cpu->fpu_internal = x87;
        return;
    }

    cpu->cpuid = 1;
    cpu_id( 0, regs );
    top = regs[0];
    mem_cpy( cpu->vendor, &regs[1], 4 );            /* EBX, EDX, ECX */
    mem_cpy( cpu->vendor + 4, &regs[3], 4 );
    mem_cpy( cpu->vendor + 8, &regs[2], 4 );
    cpu->vendor[12] = 0;
    sig = feat = 0;
    if ( top >= 1 ) {
        cpu_id( 1, regs );
        sig = regs[0];
        feat = regs[3];
    }
    cpu->stepping = (int)(sig & 0x0F);
    cpu->model = (int)((sig >> 4) & 0x0F);
    cpu->family = (int)((sig >> 8) & 0x0F);
    if ( cpu->family == 15 ) {
        cpu->family += (int)((sig >> 20) & 0xFF);
    }
    if ( cpu->family == 6 || cpu->family >= 15 ) {
        cpu->model += (int)((sig >> 12) & 0xF0);
    }
    if ( cpu->family == 0 ) {
        cpu->family = 4;                            /* a CPUID that says nothing */
    }
    cpu->level = cpu->family > 15 ? 15 : cpu->family;
    cpu->fpu_internal = (feat & 1) != 0;

    /* the extended leaves: 3DNow!, and the processor's own name */
    cpu_id( 0x80000000UL, regs );
    top = regs[0];
    if ( top >= 0x80000001UL && top <= 0x800000FFUL ) {
        cpu_id( 0x80000001UL, regs );
        ext_edx = regs[3];
    }
    if ( top >= 0x80000004UL && top <= 0x800000FFUL ) {
        for ( index = 0; index < 3; index++ ) {
            cpu_id( 0x80000002UL + (u32)index, regs );
            mem_cpy( cpu->brand + index * 16, regs, 16 );
        }
        cpu->brand[48] = 0;
        for ( brand = cpu->brand; *brand == ' '; brand++ ) {
        }
        mem_cpy( cpu->brand, brand, str_len( brand ) + 1 );
        for ( index = (int)str_len( cpu->brand ); index > 0 && cpu->brand[index - 1] == ' '; index-- ) {
            cpu->brand[index - 1] = 0;
        }
    }
    feature_text( cpu->features, sizeof( cpu->features ), feat, ext_edx );

    name = NULL;
    for ( vendor = vendors; vendor->id; vendor++ ) {
        if ( str_cmp( cpu->vendor, vendor->id ) == 0 ) {
            name = model_name( vendor->models, cpu->family > 15 ? 15 : cpu->family, cpu->model );
            break;
        }
    }
    if ( cpu->brand[0] ) {
        str_cpyn( cpu->name, cpu->brand, sizeof( cpu->name ) );
        if ( cpu->name[0] == 0 ) {                  /* too long: what fits */
            mem_cpy( cpu->name, cpu->brand, sizeof( cpu->name ) - 1 );
        }
    } else if ( name ) {
        str_cpy( cpu->name, name );
    } else {
        sfmt( cpu->name, sizeof( cpu->name ), "%s family %u model %u",
              vendor->id ? vendor->maker : "Unknown", cpu->family, cpu->model );
    }
}
