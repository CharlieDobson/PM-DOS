/*
 * F_WP.C - WordPerfect: 4.2, 5.0, 5.1 / 5.2 and 6.x and later are read,
 * 5.1 is written.
 *
 * THREE FILES UNDER ONE NAME.  A WordPerfect 5 or 6 file starts with
 * FFh "WPC", a pointer to the text, the product (1 is WordPerfect) and
 * the kind of file (0Ah is a document), and a version: major 0 is 5.x,
 * major 2 is 6.0 and everything after it.  A 4.2 file has no start at
 * all - it is text from its first byte - so it is known only by how
 * its codes look, and never surely.
 *
 * THE TEXT is a stream of bytes.  Below 80h a byte is a character (in
 * 6.x the bytes 01h to 20h are accented letters and a space is the
 * code 80h).  Above, it is a function: one byte, or a fixed number of
 * bytes, or a length that is written at both ends so that the text
 * can be read backwards.  Both ends are checked here, and a function
 * that is not whole is stepped over a byte at a time; that is what
 * keeps a damaged file from being followed out of its own bytes.
 *
 * CHARACTERS outside ASCII are a set and a number in that set
 * (WordPerfect's own sets: multinational, box drawing, typographic,
 * iconic, math, Greek, Hebrew, Cyrillic, kana).  The tables below say
 * what each is in Unicode; 5.x and 6.x number some of the sets
 * differently, so there are two tables for those.
 *
 * LENGTHS in 5.x and 6.x are WordPerfect units, 1200 to the inch.
 * 4.2 counts columns and lines, taken here as tenths and sixths of an
 * inch, which is what its usual 10-pitch printer made of them.
 *
 * WHAT IS LEFT OUT.  Margins set in the middle of a document become
 * the paragraphs' indents, the first ones the page's.  Of two headers
 * or two footers the first is kept.  Text columns, boxes and pictures,
 * styles, outlines and paragraph numbers are not read (their text,
 * where it is in the text stream, is kept).  A file with a password
 * is not opened.
 *
 * WRITING makes a 5.1 file, the form every later WordPerfect and
 * every converter reads: the prefix with one empty index block, then
 * margins, tab stops, spacing and justification where they change,
 * attributes for the looks, the header, the footer and the notes.
 */
#include "fmt.h"

static const char *const wp_variants[] = {
    "WordPerfect 5.1",
    NULL
};

static const uint16_t wp_set1[242] = {
    0x0300, 0x00B7, 0x0303, 0x0302, 0x0335, 0x0338, 0x0301, 0x0308, 0x0304, 0x0313,
    0x0315, 0x02BC, 0x0326, 0x0315, 0x00B0, 0x0307, 0x030B, 0x0327, 0x0328, 0x030C,
    0x0337, 0x0305, 0x0306, 0x00DF, 0x0138, 0x006A, 0x00C1, 0x00E1, 0x00C2, 0x00E2,
    0x00C4, 0x00E4, 0x00C0, 0x00E0, 0x00C5, 0x00E5, 0x00C6, 0x00E6, 0x00C7, 0x00E7,
    0x00C9, 0x00E9, 0x00CA, 0x00EA, 0x00CB, 0x00EB, 0x00C8, 0x00E8, 0x00CD, 0x00ED,
    0x00CE, 0x00EE, 0x00CF, 0x00EF, 0x00CC, 0x00EC, 0x00D1, 0x00F1, 0x00D3, 0x00F3,
    0x00D4, 0x00F4, 0x00D6, 0x00F6, 0x00D2, 0x00F2, 0x00DA, 0x00FA, 0x00DB, 0x00FB,
    0x00DC, 0x00FC, 0x00D9, 0x00F9, 0x0178, 0x00FF, 0x00C3, 0x00E3, 0x0110, 0x0111,
    0x00D8, 0x00F8, 0x00D5, 0x00F5, 0x00DD, 0x00FD, 0x00D0, 0x00F0, 0x00DE, 0x00FE,
    0x0102, 0x0103, 0x0100, 0x0101, 0x0104, 0x0105, 0x0106, 0x0107, 0x010C, 0x010D,
    0x0108, 0x0109, 0x010A, 0x010B, 0x010E, 0x010F, 0x011A, 0x011B, 0x0116, 0x0117,
    0x0112, 0x0113, 0x0118, 0x0119, 0x01F4, 0x01F5, 0x011E, 0x011F, 0x01E6, 0x01E7,
    0x0122, 0x0123, 0x011C, 0x011D, 0x0120, 0x0121, 0x0124, 0x0125, 0x0126, 0x0127,
    0x0130, 0x0069, 0x012A, 0x012B, 0x012E, 0x012F, 0x0128, 0x0129, 0x0132, 0x0133,
    0x0134, 0x0135, 0x0136, 0x0137, 0x0139, 0x013A, 0x013D, 0x013E, 0x013B, 0x013C,
    0x013F, 0x0140, 0x0141, 0x0142, 0x0143, 0x0144, 0x0000, 0x0149, 0x0147, 0x0148,
    0x0145, 0x0146, 0x0150, 0x0151, 0x014C, 0x014D, 0x0152, 0x0153, 0x0154, 0x0155,
    0x0158, 0x0159, 0x0156, 0x0157, 0x015A, 0x015B, 0x0160, 0x0161, 0x015E, 0x015F,
    0x015C, 0x015D, 0x0164, 0x0165, 0x0162, 0x0163, 0x0166, 0x0167, 0x016C, 0x016D,
    0x0170, 0x0171, 0x016A, 0x016B, 0x0172, 0x0173, 0x016E, 0x016F, 0x0168, 0x0169,
    0x0174, 0x0175, 0x0176, 0x0177, 0x0179, 0x017A, 0x017D, 0x017E, 0x017B, 0x017C,
    0x014A, 0x014B, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1EF2, 0x1EF3, 0x010E, 0x010F,
    0x01A0, 0x01A1, 0x01AF, 0x01B0, 0x0114, 0x0115, 0x012C, 0x012D, 0x0049, 0x0131,
    0x014E, 0x014F
};

static const uint16_t wp5_set2[28] = {
    0x0323, 0x0324, 0x02DA, 0x0325, 0x02BC, 0x032D, 0x2017, 0x005F, 0x0138, 0x032E,
    0x033E, 0x2018, 0x0020, 0x02BD, 0x02DB, 0x0327, 0x0321, 0x0322, 0x030D, 0x2019,
    0x0329, 0x0020, 0x0621, 0x02BE, 0x0306, 0x0310, 0x2032, 0x2034
};

static const uint16_t wp6_set2[145] = {
    0x02B9, 0x02BA, 0x02BB, 0x0020, 0x02BD, 0x02BC, 0x0020, 0x02BE, 0x02BF, 0x0310,
    0x02D0, 0x02D1, 0x0306, 0x032E, 0x0329, 0x02C8, 0x02CC, 0x02C9, 0x02CA, 0x02CB,
    0x02CD, 0x02CE, 0x02CF, 0x02C6, 0x02C7, 0x02DC, 0x0325, 0x02DA, 0x032D, 0x032C,
    0x0323, 0x0308, 0x0324, 0x031C, 0x031D, 0x031E, 0x031F, 0x0320, 0x0321, 0x0322,
    0x032A, 0x032B, 0x02D2, 0x02D3, 0x0361, 0x0356, 0x005F, 0x2017, 0x033E, 0x02DB,
    0x0327, 0x0233, 0x030D, 0x02B0, 0x02B6, 0x0250, 0x0251, 0x0252, 0x0253, 0x0299,
    0x0254, 0x0255, 0x0297, 0x0256, 0x0257, 0x0258, 0x0259, 0x025A, 0x025B, 0x025C,
    0x025D, 0x029A, 0x025E, 0x025F, 0x0278, 0x0261, 0x0260, 0x0262, 0x029B, 0x0263,
    0x0264, 0x0265, 0x0266, 0x0267, 0x029C, 0x0268, 0x026A, 0x0269, 0x029D, 0x029E,
    0x026B, 0x026C, 0x026D, 0x029F, 0x026E, 0x028E, 0x026F, 0x0270, 0x0271, 0x0272,
    0x0273, 0x0274, 0x0276, 0x0277, 0x02A0, 0x0279, 0x027A, 0x027B, 0x027C, 0x027D,
    0x027E, 0x027F, 0x0280, 0x0281, 0x0282, 0x0283, 0x0284, 0x0285, 0x0286, 0x0287,
    0x0288, 0x0275, 0x0289, 0x028A, 0x028C, 0x028B, 0x028D, 0x03C7, 0x028F, 0x0290,
    0x0291, 0x0292, 0x0293, 0x0294, 0x0295, 0x0296, 0x02A1, 0x02A2, 0x0298, 0x02A3,
    0x02A4, 0x02A5, 0x02A6, 0x02A7, 0x02A8
};

static const uint16_t wp_set3[88] = {
    0x2591, 0x2592, 0x2593, 0x2588, 0x258C, 0x2580, 0x2590, 0x2584, 0x2500, 0x2502,
    0x250C, 0x2510, 0x2518, 0x2514, 0x251C, 0x252C, 0x2524, 0x2534, 0x253C, 0x2550,
    0x2551, 0x2554, 0x2557, 0x255D, 0x255A, 0x2560, 0x2566, 0x2563, 0x2569, 0x256C,
    0x2552, 0x2555, 0x255B, 0x2558, 0x2553, 0x2556, 0x255C, 0x2559, 0x255E, 0x2565,
    0x2561, 0x2568, 0x255F, 0x2564, 0x2562, 0x2567, 0x256B, 0x256A, 0x2574, 0x2575,
    0x2576, 0x2577, 0x2578, 0x2579, 0x257A, 0x257B, 0x257C, 0x257E, 0x257D, 0x257F,
    0x251F, 0x2522, 0x251E, 0x2521, 0x252E, 0x2532, 0x252D, 0x2531, 0x2527, 0x2526,
    0x252A, 0x2529, 0x2536, 0x253A, 0x2535, 0x2539, 0x2541, 0x2546, 0x253E, 0x2540,
    0x2544, 0x254A, 0x253D, 0x2545, 0x2548, 0x2543, 0x2549, 0x2547
};

static const uint16_t wp_set4[102] = {
    0x25CF, 0x25CB, 0x25A0, 0x2022, 0x002A, 0x00B6, 0x00A7, 0x00A1, 0x00BF, 0x00AB,
    0x00BB, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00AA, 0x00BA, 0x00BD, 0x00BC, 0x00A2,
    0x00B2, 0x207F, 0x00AE, 0x00A9, 0x00A4, 0x00BE, 0x00B3, 0x201B, 0x2019, 0x2018,
    0x201F, 0x201D, 0x201C, 0x2013, 0x2014, 0x2039, 0x203A, 0x25CB, 0x25A1, 0x2020,
    0x2021, 0x2122, 0x2120, 0x211E, 0x25CF, 0x25E6, 0x25A0, 0x25AA, 0x25A1, 0x25AB,
    0x2012, 0xFB00, 0xFB03, 0xFB04, 0xFB01, 0xFB02, 0x2026, 0x0024, 0x20A3, 0x20A2,
    0x20A0, 0x20A4, 0x201A, 0x201E, 0x2153, 0x2154, 0x215B, 0x215C, 0x215D, 0x215E,
    0x24C2, 0x24C5, 0x20AC, 0x2105, 0x2106, 0x2030, 0x2116, 0x2014, 0x00B9, 0x2409,
    0x240C, 0x240D, 0x240A, 0x2424, 0x240B, 0x267C, 0x20A9, 0x20A6, 0x20A8, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0000, 0x0000
};

static const uint16_t wp5_set5[35] = {
    0x2665, 0x2666, 0x2663, 0x2660, 0x2642, 0x2640, 0x263C, 0x263A, 0x263B, 0x266A,
    0x266C, 0x25AC, 0x2302, 0x203C, 0x221A, 0x21A8, 0x2310, 0x2319, 0x25D8, 0x25D9,
    0x21B5, 0x261E, 0x261C, 0x2713, 0x2610, 0x2612, 0x2639, 0x266F, 0x266D, 0x266E,
    0x260E, 0x231A, 0x231B, 0x2104, 0x23B5
};

static const uint16_t wp6_set5[255] = {
    0x2661, 0x2662, 0x2667, 0x2664, 0x2642, 0x2640, 0x263C, 0x263A, 0x263B, 0x266A,
    0x266C, 0x25AC, 0x2302, 0x203C, 0x221A, 0x21A8, 0x2310, 0x2319, 0x25D8, 0x25D9,
    0x21B5, 0x2104, 0x261C, 0x23B5, 0x2610, 0x2612, 0x2639, 0x266F, 0x266D, 0x266E,
    0x260E, 0x231A, 0x231B, 0x2701, 0x2702, 0x2703, 0x2704, 0x260E, 0x2706, 0x2707,
    0x2708, 0x2709, 0x261B, 0x261E, 0x270C, 0x270D, 0x270E, 0x270F, 0x2710, 0x2711,
    0x2712, 0x2713, 0x2714, 0x2715, 0x2716, 0x2717, 0x2718, 0x2719, 0x271A, 0x271B,
    0x271C, 0x271D, 0x271E, 0x271F, 0x2720, 0x2721, 0x2722, 0x2723, 0x2724, 0x2725,
    0x2726, 0x2727, 0x2605, 0x2606, 0x272A, 0x272B, 0x272C, 0x272D, 0x272E, 0x272F,
    0x2730, 0x2731, 0x2732, 0x2733, 0x2734, 0x2735, 0x2736, 0x2737, 0x2738, 0x2739,
    0x273A, 0x273B, 0x273C, 0x273D, 0x273E, 0x273F, 0x2740, 0x2741, 0x2742, 0x2743,
    0x2744, 0x2745, 0x2746, 0x2747, 0x2748, 0x2749, 0x274A, 0x274B, 0x25CF, 0x274D,
    0x25A0, 0x274F, 0x2750, 0x2751, 0x2752, 0x25B2, 0x25BC, 0x25C6, 0x2756, 0x25D7,
    0x2758, 0x2759, 0x275A, 0x275B, 0x275C, 0x275D, 0x275E, 0x2036, 0x2033, 0x0020,
    0x0020, 0x0020, 0x0020, 0x2329, 0x232A, 0x005B, 0x005D, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x2190, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x21E8, 0x21E6, 0x2794, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x25D6, 0x0020,
    0x0020, 0x2761, 0x2762, 0x2763, 0x2764, 0x2765, 0x2766, 0x2767, 0x2663, 0x2666,
    0x2665, 0x2660, 0x2780, 0x2781, 0x2782, 0x2783, 0x2784, 0x2785, 0x2786, 0x2787,
    0x2788, 0x2789, 0x2776, 0x2777, 0x2778, 0x2779, 0x277A, 0x277B, 0x277C, 0x277D,
    0x277E, 0x277F, 0x2780, 0x2781, 0x2782, 0x2783, 0x2784, 0x2785, 0x2786, 0x2787,
    0x2788, 0x2789, 0x278A, 0x278B, 0x278C, 0x278D, 0x278E, 0x278F, 0x2790, 0x2791,
    0x2792, 0x2793, 0x2794, 0x2192, 0x2194, 0x2195, 0x2798, 0x2799, 0x279A, 0x279B,
    0x279C, 0x279D, 0x279E, 0x279F, 0x27A0, 0x27A1, 0x27A2, 0x27A3, 0x27A4, 0x27A5,
    0x27A6, 0x27A7, 0x27A8, 0x27A9, 0x27AA, 0x27AB, 0x27AC, 0x27AD, 0x27AE, 0x27AF,
    0x0020, 0x27B1, 0x27B2, 0x27B3, 0x27B4, 0x27B5, 0x27B6, 0x27B7, 0x27B8, 0x27B9,
    0x27BA, 0x27BB, 0x27BC, 0x27BD, 0x27BE
};

static const uint16_t wp_set6[238] = {
    0x2212, 0x00B1, 0x2264, 0x2265, 0x221D, 0x002F, 0x2215, 0x2216, 0x00F7, 0x2223,
    0x27E8, 0x27E9, 0x223C, 0x2248, 0x2261, 0x2208, 0x2229, 0x2225, 0x2211, 0x221E,
    0x00AC, 0x2192, 0x2190, 0x2191, 0x2193, 0x2194, 0x2195, 0x25B8, 0x25C2, 0x25B4,
    0x25BE, 0x22C5, 0x00B7, 0x2218, 0x2219, 0x212B, 0x00B0, 0x00B5, 0x203E, 0x00D7,
    0x222B, 0x220F, 0x2213, 0x2207, 0x2202, 0x2032, 0x2033, 0x2192, 0x212F, 0x2113,
    0x210F, 0x2111, 0x211C, 0x2118, 0x21C4, 0x21C6, 0x21D2, 0x21D0, 0x21D1, 0x21D3,
    0x21D4, 0x21D5, 0x2197, 0x2198, 0x2196, 0x2199, 0x222A, 0x2282, 0x2283, 0x2286,
    0x2287, 0x220D, 0x2205, 0x2308, 0x2309, 0x230A, 0x230B, 0x226A, 0x226B, 0x2220,
    0x2297, 0x2295, 0x2296, 0x2A38, 0x2299, 0x2227, 0x2228, 0x22BB, 0x22A4, 0x22A5,
    0x2312, 0x22A2, 0x22A3, 0x25A1, 0x25A0, 0x25CA, 0x25C6, 0x27E6, 0x27E7, 0x2260,
    0x2262, 0x2235, 0x2234, 0x2237, 0x222E, 0x2112, 0x212D, 0x2128, 0x2118, 0x20DD,
    0x29CB, 0x25C7, 0x22C6, 0x2034, 0x2210, 0x2243, 0x2245, 0x227A, 0x227C, 0x227B,
    0x227D, 0x2203, 0x2200, 0x22D8, 0x22D9, 0x228E, 0x228A, 0x228B, 0x2293, 0x2294,
    0x228F, 0x2291, 0x22E4, 0x2290, 0x2292, 0x22E5, 0x25B3, 0x25BD, 0x25C3, 0x25B9,
    0x22C8, 0x2323, 0x2322, 0x25EF, 0x219D, 0x21A9, 0x21AA, 0x21A3, 0x21BC, 0x21BD,
    0x21C0, 0x21C1, 0x21CC, 0x21CB, 0x21BF, 0x21BE, 0x21C3, 0x21C2, 0x21C9, 0x21C7,
    0x22D3, 0x22D2, 0x22D0, 0x22D1, 0x229A, 0x229B, 0x229D, 0x2127, 0x2221, 0x2222,
    0x25C3, 0x25B9, 0x25B5, 0x25BF, 0x2214, 0x2250, 0x2252, 0x2253, 0x224E, 0x224D,
    0x22A8, 0x2258, 0x226C, 0x0285, 0x2605, 0x226E, 0x2270, 0x226F, 0x2271, 0x2241,
    0x2244, 0x2247, 0x2249, 0x2280, 0x22E0, 0x2281, 0x22E1, 0x2284, 0x2285, 0x2288,
    0x2289, 0x0020, 0x0020, 0x22E2, 0x22E3, 0x2226, 0x2224, 0x226D, 0x2204, 0x2209,
    0x2247, 0x2130, 0x2131, 0x2102, 0x0020, 0x2115, 0x211D, 0x225F, 0x22BE, 0x220B,
    0x22EF, 0x2026, 0x22EE, 0x22F1, 0x0020, 0x20E1, 0x002B, 0x002D, 0x003D, 0x002A,
    0x2032, 0x2033, 0x2034, 0x210B, 0x2118, 0x2272, 0x2273, 0x0020
};

static const uint16_t wp_set7[229] = {
    0x2320, 0x2321, 0x23A5, 0x23BD, 0x221A, 0x0020, 0x2211, 0x220F, 0x2210, 0x222B,
    0x222E, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x23A7, 0x23A8, 0x23A9, 0x23AA, 0x0020,
    0x0020, 0x0020, 0x0020, 0x23AB, 0x23AC, 0x23AD, 0x23AA, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x222A, 0x222B, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x239B, 0x239D, 0x239C, 0x0020, 0x0020, 0x0020,
    0x0020, 0x239E, 0x23A8, 0x239F, 0x0020, 0x0020, 0x0020, 0x0020, 0x23A1, 0x23A3,
    0x23A2, 0x0020, 0x20AA, 0x0020, 0x0020, 0x23A4, 0x23A6, 0x23A5, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x22C3, 0x22C2, 0x228E, 0x2A04, 0x2294, 0x2A06,
    0x2227, 0x22C0, 0x2228, 0x22C1, 0x2297, 0x2A02, 0x2295, 0x2A01, 0x2299, 0x2A00,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x229D, 0x0020, 0x2238, 0x0020, 0x27E6, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x0020, 0x27E7, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020,
    0x21BC, 0x21BD, 0x0020, 0x296C, 0x296D, 0x296A, 0x296B, 0x0020, 0x21C9, 0x21C7,
    0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020, 0x21BE, 0x21BF, 0x21C3, 0x21C2,
    0x0020, 0x2293, 0x2A05, 0x23A1, 0x0020, 0x0020, 0x0020, 0x0020, 0x0020
};

static const uint16_t wp5_set8[210] = {
    0x0391, 0x03B1, 0x0392, 0x03B2, 0x0392, 0x03D0, 0x0393, 0x03B3, 0x0394, 0x03B4,
    0x0395, 0x03B5, 0x0396, 0x03B6, 0x0397, 0x03B7, 0x0398, 0x03B8, 0x0399, 0x03B9,
    0x039A, 0x03BA, 0x039B, 0x03BB, 0x039C, 0x03BC, 0x039D, 0x03BD, 0x039E, 0x03BE,
    0x039F, 0x03BF, 0x03A0, 0x03C0, 0x03A1, 0x03C1, 0x03A3, 0x03C3, 0x03F9, 0x03DB,
    0x03A4, 0x03C4, 0x03A5, 0x03C5, 0x03A6, 0x03D5, 0x03A7, 0x03C7, 0x03A8, 0x03C8,
    0x03A9, 0x03C9, 0x03AC, 0x03AD, 0x03AE, 0x03AF, 0x03CA, 0x03CC, 0x03CD, 0x03CB,
    0x03CE, 0x03B5, 0x03D1, 0x03F0, 0x03D6, 0x1FE5, 0x03D2, 0x03C6, 0x03C9, 0x037E,
    0x0387, 0x0384, 0x00A8, 0x0385, 0x1FED, 0x1FEF, 0x1FC0, 0x1FBD, 0x1FBF, 0x1FBE,
    0x1FCE, 0x1FDE, 0x1FCD, 0x1FDD, 0x1FCF, 0x1FDF, 0x0384, 0x1FEF, 0x1FC0, 0x1FBD,
    0x1FBF, 0x1FCE, 0x1FDE, 0x1FCD, 0x1FDD, 0x1FCF, 0x1FDF, 0x1F70, 0x1FB6, 0x1FB3,
    0x1FB4, 0x1FB7, 0x1F00, 0x1F04, 0x1F02, 0x1F06, 0x1F80, 0x1F84, 0x1F86, 0x1F01,
    0x1F05, 0x1F03, 0x1F07, 0x1F81, 0x1F85, 0x1F87, 0x1F72, 0x1F10, 0x1F14, 0x1F13,
    0x1F11, 0x1F15, 0x1F13, 0x1F74, 0x1FC6, 0x1FC3, 0x1FC4, 0x1FC2, 0x1FC7, 0x1F20,
    0x1F24, 0x1F22, 0x1F26, 0x1F90, 0x1F94, 0x1F96, 0x1F21, 0x1F25, 0x1F23, 0x1F27,
    0x1F91, 0x1F95, 0x1F97, 0x1F76, 0x1FD6, 0x0390, 0x1FD2, 0x1F30, 0x1F34, 0x1F32,
    0x1F36, 0x1F31, 0x1F35, 0x1F33, 0x1F37, 0x1F78, 0x1F40, 0x1F44, 0x1F42, 0x1F41,
    0x1F45, 0x1F43, 0x1F7A, 0x1FE6, 0x03B0, 0x1FE3, 0x1F50, 0x1F54, 0x1F52, 0x1F56,
    0x1F51, 0x1F55, 0x1F53, 0x1F57, 0x1F7C, 0x1FF6, 0x1FF3, 0x1FF4, 0x1FF2, 0x1FF7,
    0x1F60, 0x1F64, 0x1F62, 0x1F66, 0x1FA0, 0x1FA4, 0x1FA6, 0x1F61, 0x1F65, 0x1F63,
    0x1F67, 0x1FA1, 0x1FA5, 0x1FA7, 0x0374, 0x0375, 0x03DB, 0x03DD, 0x03D9, 0x03E1,
    0x0386, 0x0388, 0x0389, 0x038A, 0x038C, 0x038E, 0x038F, 0x03AA, 0x03AB, 0x1FE5
};

static const uint16_t wp6_set8[219] = {
    0x0391, 0x03B1, 0x0392, 0x03B2, 0x0392, 0x03D0, 0x0393, 0x03B3, 0x0394, 0x03B4,
    0x0395, 0x03B5, 0x0396, 0x03B6, 0x0397, 0x03B7, 0x0398, 0x03B8, 0x0399, 0x03B9,
    0x039A, 0x03BA, 0x039B, 0x03BB, 0x039C, 0x03BC, 0x039D, 0x03BD, 0x039E, 0x03BE,
    0x039F, 0x03BF, 0x03A0, 0x03C0, 0x03A1, 0x03C1, 0x03A3, 0x03C3, 0x03A3, 0x03C2,
    0x03A4, 0x03C4, 0x03A5, 0x03C5, 0x03A6, 0x03C6, 0x03A7, 0x03C7, 0x03A8, 0x03C8,
    0x03A9, 0x03C9, 0x0386, 0x03AC, 0x0388, 0x03AD, 0x0389, 0x03AE, 0x038A, 0x03AF,
    0x03AA, 0x03CA, 0x038C, 0x03CC, 0x038E, 0x03CD, 0x03AB, 0x03CB, 0x038F, 0x03CE,
    0x03B5, 0x03D1, 0x03F0, 0x03D6, 0x03F1, 0x03C2, 0x03D2, 0x03D5, 0x03C9, 0x037E,
    0x0387, 0x0374, 0x0375, 0x0384, 0x00A8, 0x0385, 0x1FED, 0x1FEF, 0x1FC0, 0x1FBD,
    0x1FFE, 0x037A, 0x1FCE, 0x1FDE, 0x1FCD, 0x1FDD, 0x1FCF, 0x1FDF, 0x0384, 0x1FEF,
    0x1FC0, 0x1FBD, 0x1FFE, 0x1FCE, 0x1FDE, 0x1FCD, 0x1FDD, 0x1FCF, 0x1FDF, 0x1F70,
    0x1FB6, 0x1FB3, 0x1FB4, 0x1FB2, 0x1FB7, 0x1F00, 0x1F04, 0x1F02, 0x1F06, 0x1F80,
    0x1F84, 0x1F82, 0x1F86, 0x1F01, 0x1F05, 0x1F03, 0x1F07, 0x1F81, 0x1F85, 0x1F83,
    0x1F87, 0x1F72, 0x1F10, 0x1F14, 0x1F12, 0x1F11, 0x1F15, 0x1F13, 0x1F74, 0x1FC6,
    0x1FC3, 0x1FC4, 0x1FC2, 0x1FC7, 0x1F20, 0x1F24, 0x1F22, 0x1F26, 0x1F90, 0x1F94,
    0x1F92, 0x1F96, 0x1F21, 0x1F25, 0x1F23, 0x1F27, 0x1F91, 0x1F95, 0x1F93, 0x1F97,
    0x1F76, 0x1FD6, 0x0390, 0x1FD2, 0x1F30, 0x1F34, 0x1F32, 0x1F36, 0x1F31, 0x1F35,
    0x1F33, 0x1F37, 0x1F78, 0x1F40, 0x1F44, 0x1F42, 0x1F41, 0x1F45, 0x1F43, 0x1FE5,
    0x1FE4, 0x1F7A, 0x1FE6, 0x03B0, 0x1FE2, 0x1F50, 0x1F54, 0x1F52, 0x1F56, 0x1F51,
    0x1F55, 0x1F53, 0x1F57, 0x1F7C, 0x1FF6, 0x1FF3, 0x1FF4, 0x1FF2, 0x1FF7, 0x1F60,
    0x1F64, 0x1F62, 0x1F66, 0x1FA0, 0x1FA4, 0x1FA2, 0x1FA6, 0x1F61, 0x1F65, 0x1F63,
    0x1F67, 0x1FA1, 0x1FA5, 0x1FA3, 0x1FA7, 0x03DA, 0x03DC, 0x03DE, 0x03E0
};

static const uint16_t wp5_set9[119] = {
    0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7, 0x05D8, 0x05D9,
    0x05DA, 0x05DB, 0x05DC, 0x05DD, 0x05DE, 0x05DF, 0x05E0, 0x05E1, 0x05E2, 0x05E3,
    0x05E4, 0x05E5, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, 0x05BE, 0x05C0, 0x05C3,
    0x05F3, 0x05F4, 0x05B0, 0x05B1, 0x05B2, 0x05B3, 0x05B4, 0x05B5, 0x05B6, 0x05B7,
    0x05B8, 0x05B9, 0x05BA, 0x05BB, 0x05BC, 0x05BD, 0x05BF, 0x05B7, 0xFBE1, 0x05F0,
    0x05F1, 0x05F2, 0x0591, 0x0596, 0x05AD, 0x05A4, 0x059A, 0x059B, 0x05A3, 0x05A5,
    0x05A6, 0x05A7, 0x09AA, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597, 0x0598, 0x0599,
    0x05A8, 0x059C, 0x059D, 0x059E, 0x05A1, 0x05A9, 0x05A0, 0x059F, 0x05AB, 0x05AC,
    0x05AF, 0x05C4, 0x0544, 0x05D0, 0xFB31, 0xFB32, 0xFB33, 0xFB34, 0xFB35, 0xFB4B,
    0xFB36, 0x05D7, 0xFB38, 0xFB39, 0xFB3B, 0xFB3A, 0x05DA, 0x05DA, 0x05DA, 0x05DA,
    0x05DA, 0x05DA, 0xFB3C, 0xFB3E, 0xFB40, 0x05DF, 0xFB41, 0xFB44, 0xFB46, 0xFB47,
    0xFB2B, 0xFB2D, 0xFB2A, 0xFB2C, 0xFB4A, 0xFB4C, 0xFB4E, 0xFB1F, 0xFB1D
};

static const uint16_t wp6_set9[123] = {
    0x05D0, 0x05D1, 0x05D2, 0x05D3, 0x05D4, 0x05D5, 0x05D6, 0x05D7, 0x05D8, 0x05D9,
    0x05DA, 0x05DB, 0x05DC, 0x05DD, 0x05DE, 0x05DF, 0x05E0, 0x05E1, 0x05E2, 0x05E3,
    0x05E4, 0x05E5, 0x05E6, 0x05E7, 0x05E8, 0x05E9, 0x05EA, 0x05BE, 0x05C0, 0x05C3,
    0x05F3, 0x05F4, 0x05B0, 0x05B1, 0x05B2, 0x05B3, 0x05B4, 0x05B5, 0x05B6, 0x05B7,
    0x05B8, 0x05B9, 0x05B9, 0x05BB, 0x05BC, 0x05BD, 0x05BF, 0x05B7, 0xFB1E, 0x05F0,
    0x05F1, 0x05F2, 0xFB1F, 0x0591, 0x0596, 0x0020, 0x05A4, 0x059A, 0x059B, 0x05A3,
    0x05A5, 0x05A6, 0x05A7, 0x05A2, 0x0592, 0x0593, 0x0594, 0x0595, 0x0597, 0x0598,
    0x0599, 0x05A8, 0x05F3, 0x05F3, 0x05F4, 0x0020, 0x05A9, 0x05A0, 0x059F, 0x05AB,
    0x05AC, 0x05AF, 0x05C4, 0x05AA, 0xFB30, 0xFB31, 0x05D1, 0xFB32, 0xFB33, 0xFB34,
    0xFB35, 0xFB4B, 0xFB36, 0x05D7, 0xFB38, 0xFB39, 0xFB1D, 0xFB3B, 0xFB3A, 0x05DA,
    0x05DA, 0x05DA, 0x05DA, 0x05DA, 0x05DA, 0xFB3C, 0xFB3E, 0xFB40, 0x05D5, 0xFB41,
    0xFB44, 0x05E4, 0xFB46, 0xFB47, 0xFB2B, 0xFB2D, 0xFB2A, 0xFB2C, 0xFB4A, 0x05DC,
    0xFB3C, 0xFB49, 0x20AA
};

static const uint16_t wp5_set10[150] = {
    0x0410, 0x0430, 0x0411, 0x0431, 0x0412, 0x0432, 0x0413, 0x0433, 0x0414, 0x0434,
    0x0415, 0x0435, 0x0401, 0x0451, 0x0416, 0x0436, 0x0417, 0x0437, 0x0418, 0x0438,
    0x0419, 0x0439, 0x041A, 0x043A, 0x041B, 0x043B, 0x041C, 0x043C, 0x041D, 0x043D,
    0x041E, 0x043E, 0x041F, 0x043F, 0x0420, 0x0440, 0x0421, 0x0441, 0x0422, 0x0442,
    0x0423, 0x0443, 0x0424, 0x0444, 0x0425, 0x0445, 0x0426, 0x0446, 0x0427, 0x0447,
    0x0428, 0x0448, 0x0429, 0x0449, 0x042A, 0x044A, 0x042B, 0x044B, 0x042C, 0x044C,
    0x042D, 0x044D, 0x042E, 0x044E, 0x042F, 0x044F, 0x0490, 0x0491, 0x0402, 0x0452,
    0x0403, 0x0453, 0x0404, 0x0454, 0x0405, 0x0455, 0x0406, 0x0456, 0x0407, 0x0457,
    0x0408, 0x0458, 0x0409, 0x0459, 0x040A, 0x045A, 0x040B, 0x045B, 0x040C, 0x045C,
    0x040E, 0x045E, 0x040F, 0x045F, 0x0462, 0x0463, 0x0472, 0x0473, 0x0474, 0x0475,
    0x046A, 0x046B, 0xA640, 0xA641, 0x0429, 0x0449, 0x04C0, 0x04CF, 0x0466, 0x0467,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0400, 0x0450, 0x0000, 0x0000, 0x040D, 0x045D, 0x0000, 0x0000, 0x0000, 0x0000,
    0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0301, 0x0300
};

static const uint16_t wp6_set10[250] = {
    0x0410, 0x0430, 0x0411, 0x0431, 0x0412, 0x0432, 0x0413, 0x0433, 0x0414, 0x0434,
    0x0415, 0x0435, 0x0401, 0x0451, 0x0416, 0x0436, 0x0417, 0x0437, 0x0418, 0x0438,
    0x0419, 0x0439, 0x041A, 0x043A, 0x041B, 0x043B, 0x041C, 0x043C, 0x041D, 0x043D,
    0x041E, 0x043E, 0x041F, 0x043F, 0x0420, 0x0440, 0x0421, 0x0441, 0x0422, 0x0442,
    0x0423, 0x0443, 0x0424, 0x0444, 0x0425, 0x0445, 0x0426, 0x0446, 0x0427, 0x0447,
    0x0428, 0x0448, 0x0429, 0x0449, 0x042A, 0x044A, 0x042B, 0x044B, 0x042C, 0x044C,
    0x042D, 0x044D, 0x042E, 0x044E, 0x042F, 0x044F, 0x04D8, 0x04D9, 0x0403, 0x0453,
    0x0490, 0x0491, 0x0492, 0x0493, 0x0402, 0x0452, 0x0404, 0x0454, 0x0404, 0x0454,
    0x0496, 0x0497, 0x0405, 0x0455, 0x0020, 0x0020, 0x0418, 0x0438, 0x0406, 0x0456,
    0x0407, 0x0457, 0x0020, 0x0020, 0x0408, 0x0458, 0x040C, 0x045C, 0x049A, 0x049B,
    0x04C3, 0x04C4, 0x049C, 0x049D, 0x0409, 0x0459, 0x04A2, 0x04A3, 0x040A, 0x045A,
    0x047A, 0x047B, 0x0460, 0x0461, 0x040B, 0x045B, 0x040E, 0x045E, 0x04EE, 0x04EF,
    0x04AE, 0x04AF, 0x04B0, 0x04B1, 0x0194, 0x0263, 0x04B2, 0x04B3, 0x0425, 0x0445,
    0x04BA, 0x04BB, 0x047E, 0x047F, 0x040F, 0x045F, 0x04B6, 0x04B7, 0x04B8, 0x04B9,
    0x0428, 0x0448, 0x0462, 0x0463, 0x0466, 0x0467, 0x046A, 0x046B, 0x046E, 0x046F,
    0x0470, 0x0471, 0x0472, 0x0473, 0x0474, 0x0475, 0x0410, 0x0430, 0x0415, 0x0435,
    0x0404, 0x0454, 0x0418, 0x0438, 0x0406, 0x0456, 0x0407, 0x0457, 0x041E, 0x043E,
    0x0423, 0x0443, 0x042B, 0x044B, 0x042D, 0x044D, 0x042E, 0x044E, 0x042F, 0x044F,
    0x0410, 0x0430, 0x0400, 0x0450, 0x0401, 0x0451, 0x040D, 0x045D, 0x041E, 0x043E,
    0x0423, 0x0443, 0x042B, 0x044B, 0x042D, 0x044D, 0x042E, 0x044E, 0x042F, 0x044F,
    0x0301, 0x0300, 0x0308, 0x0306, 0x0326, 0x0328, 0x0304, 0x0020, 0x201E, 0x201C,
    0x10D0, 0x10D1, 0x10D2, 0x10D3, 0x10D4, 0x10D5, 0x10D6, 0x10F1, 0x10D7, 0x10D8,
    0x10D9, 0x10DA, 0x10DB, 0x10DC, 0x10F2, 0x10DD, 0x10DE, 0x10DF, 0x10E0, 0x10E1,
    0x10E2, 0x10E3, 0x10F3, 0x10E4, 0x10E5, 0x10E6, 0x10E7, 0x10E8, 0x10E9, 0x10EA,
    0x10EB, 0x10EC, 0x10ED, 0x10EE, 0x10F4, 0x10EF, 0x10F0, 0x10F5, 0x10F6, 0x10E3
};

static const uint16_t wp5_set11[185] = {
    0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3053, 0x3083, 0x3085, 0x3087, 0x3094,
    0x3095, 0x3096, 0x3042, 0x3044, 0x3046, 0x3048, 0x304A, 0x304B, 0x304D, 0x3047,
    0x3051, 0x3053, 0x304C, 0x304E, 0x3050, 0x3052, 0x3054, 0x3055, 0x3057, 0x3059,
    0x305B, 0x305D, 0x3056, 0x3058, 0x305A, 0x305C, 0x305E, 0x305F, 0x3051, 0x3064,
    0x3066, 0x3068, 0x3060, 0x3062, 0x3065, 0x3067, 0x3069, 0x306A, 0x306B, 0x306C,
    0x306D, 0x306E, 0x306F, 0x3072, 0x3075, 0x3078, 0x307B, 0x3070, 0x3073, 0x3076,
    0x3079, 0x307C, 0x3071, 0x3074, 0x3077, 0x307A, 0x307D, 0x307E, 0x307F, 0x3080,
    0x3081, 0x3082, 0x3084, 0x3086, 0x3088, 0x3089, 0x308A, 0x308B, 0x308C, 0x308D,
    0x308E, 0x3092, 0x3093, 0x3014, 0x3015, 0xFF3B, 0xFF3D, 0x300C, 0x300D, 0x300C,
    0x300D, 0x302A, 0x3002, 0x3001, 0x309D, 0x309E, 0x3003, 0x30FC, 0x309B, 0x309C,
    0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x3057, 0x30F4,
    0x30F5, 0x30F6, 0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA, 0x30AB, 0x30AD, 0x30AF,
    0x30B1, 0x30B3, 0x30AC, 0x30AE, 0x30B0, 0x30B2, 0x30B4, 0x30B5, 0x30C4, 0x30B9,
    0x30BB, 0x30BD, 0x30B6, 0x30B8, 0x30BA, 0x30BC, 0x30BE, 0x30BF, 0x30C1, 0x30C4,
    0x30C6, 0x30C8, 0x30C0, 0x30C2, 0x30C5, 0x30C7, 0x30C9, 0x30CA, 0x30CB, 0x30CC,
    0x30CD, 0x30CE, 0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x03D0, 0x30DB, 0x30D3, 0x30D6,
    0x30D9, 0x30DC, 0x30D1, 0x30D4, 0x30D7, 0x30DA, 0x30DD, 0x30DE, 0x30DF, 0x30E0,
    0x30E1, 0x30E2, 0x30E4, 0x30E6, 0x30E8, 0x30E9, 0x30EA, 0x30AB, 0x30EC, 0x30ED,
    0x30EF, 0x30F2, 0x30F3, 0x30FD, 0x30FE
};

static const uint16_t wp6_set11[63] = {
    0xFF61, 0xFF62, 0xFF63, 0xFF64, 0xFF65, 0xFF66, 0xFF67, 0xFF68, 0xFF69, 0xFF6A,
    0xFF6B, 0xFF6C, 0xFF6D, 0xFF6E, 0xFF6F, 0xFF70, 0xFF71, 0xFF72, 0xFF73, 0xFF74,
    0xFF75, 0xFF76, 0xFF77, 0xFF78, 0xFF79, 0xFF7A, 0xFF7B, 0xFF7C, 0xFF7D, 0xFF7E,
    0xFF7F, 0xFF80, 0xFF81, 0xFF82, 0xFF83, 0xFF84, 0xFF85, 0xFF86, 0xFF87, 0xFF88,
    0xFF89, 0xFF8A, 0xFF8B, 0xFF8C, 0xFF8D, 0xFF8E, 0xFF8F, 0xFF90, 0xFF91, 0xFF92,
    0xFF93, 0xFF94, 0xFF95, 0xFF96, 0xFF97, 0xFF98, 0xFF99, 0xFF9A, 0xFF9B, 0xFF9C,
    0xFF9D, 0xFF9E, 0xFF9F
};

static const uint16_t wp6_low[32] = {
    0x00E5, 0x00C5, 0x00E6, 0x00C6, 0x00E4, 0x00C4, 0x00E1, 0x00E0, 0x00E2, 0x00E3,
    0x00C3, 0x00E7, 0x00C7, 0x00EB, 0x00E9, 0x00C9, 0x00E8, 0x00EA, 0x00ED, 0x00F1,
    0x00D1, 0x00F8, 0x00D8, 0x00F5, 0x00D5, 0x00F6, 0x00D6, 0x00FC, 0x00DC, 0x00FA,
    0x00F9, 0x00DF
};

/* characters that are two of Unicode's: the set, the number, a letter and its mark */
static const uint16_t wp_pairs[49][4] = {
    { 1, 156, 0x02BC, 0x004E },
    { 1, 212, 0x0044, 0x0304 },
    { 1, 213, 0x0064, 0x0304 },
    { 1, 214, 0x004C, 0x0304 },
    { 1, 215, 0x006C, 0x0304 },
    { 1, 216, 0x004E, 0x0304 },
    { 1, 217, 0x006E, 0x0304 },
    { 1, 218, 0x0052, 0x0300 },
    { 1, 219, 0x0072, 0x0300 },
    { 1, 220, 0x0053, 0x0304 },
    { 1, 221, 0x0073, 0x0304 },
    { 1, 222, 0x0054, 0x0304 },
    { 1, 223, 0x0074, 0x0304 },
    { 1, 224, 0x0059, 0x0306 },
    { 1, 225, 0x0079, 0x0306 },
    { 10, 110, 0x0410, 0x0301 },
    { 10, 111, 0x0430, 0x0301 },
    { 10, 112, 0x0415, 0x0301 },
    { 10, 113, 0x0435, 0x0301 },
    { 10, 114, 0x0418, 0x0301 },
    { 10, 115, 0x0458, 0x0301 },
    { 10, 116, 0x041E, 0x0301 },
    { 10, 117, 0x043E, 0x0301 },
    { 10, 118, 0x0423, 0x0301 },
    { 10, 119, 0x0443, 0x0301 },
    { 10, 120, 0x042B, 0x0301 },
    { 10, 121, 0x044B, 0x0301 },
    { 10, 122, 0x042D, 0x0301 },
    { 10, 123, 0x044D, 0x0301 },
    { 10, 124, 0x042E, 0x0301 },
    { 10, 125, 0x044E, 0x0301 },
    { 10, 126, 0x042F, 0x0301 },
    { 10, 127, 0x044F, 0x0301 },
    { 10, 128, 0x0410, 0x0300 },
    { 10, 129, 0x0430, 0x0300 },
    { 10, 132, 0x0401, 0x0300 },
    { 10, 133, 0x0451, 0x0300 },
    { 10, 136, 0x041E, 0x0300 },
    { 10, 137, 0x043E, 0x0300 },
    { 10, 138, 0x0423, 0x0300 },
    { 10, 139, 0x0443, 0x0300 },
    { 10, 140, 0x042B, 0x0300 },
    { 10, 141, 0x044B, 0x0300 },
    { 10, 142, 0x042D, 0x0300 },
    { 10, 143, 0x044D, 0x0300 },
    { 10, 144, 0x042E, 0x0300 },
    { 10, 145, 0x044E, 0x0300 },
    { 10, 146, 0x042F, 0x0300 },
    { 10, 147, 0x044F, 0x0300 }
};

/* ------------------------------------------------------------------ */
/* the character sets                                                  */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint16_t *tab;
    unsigned int count;
} WPSET;

#define WP_NSETS    12

static const WPSET wp5_sets[WP_NSETS] = {
    { NULL, 0 }, { wp_set1, 242 }, { wp5_set2, 28 }, { wp_set3, 88 },
    { wp_set4, 102 }, { wp5_set5, 35 }, { wp_set6, 238 }, { wp_set7, 229 },
    { wp5_set8, 210 }, { wp5_set9, 119 }, { wp5_set10, 150 }, { wp5_set11, 185 }
};

static const WPSET wp6_sets[WP_NSETS] = {
    { NULL, 0 }, { wp_set1, 242 }, { wp6_set2, 145 }, { wp_set3, 88 },
    { wp_set4, 102 }, { wp6_set5, 255 }, { wp_set6, 238 }, { wp_set7, 229 },
    { wp6_set8, 219 }, { wp6_set9, 123 }, { wp6_set10, 250 }, { wp6_set11, 63 }
};

#define WP_NPAIRS   (sizeof( wp_pairs ) / sizeof( wp_pairs[0] ))

/* Unicode's combining accents, and the same accents standing alone */
static const uint16_t wp_spacing[][2] = {
    { 0x0300, 0x0060 }, { 0x0301, 0x00B4 }, { 0x0302, 0x02C6 }, { 0x0303, 0x02DC },
    { 0x0304, 0x00AF }, { 0x0305, 0x00AF }, { 0x0306, 0x02D8 }, { 0x0307, 0x02D9 },
    { 0x0308, 0x00A8 }, { 0x030B, 0x02DD }, { 0x030C, 0x02C7 }, { 0x0313, 0x2019 },
    { 0x0315, 0x2019 }, { 0x0326, 0x002C }, { 0x0327, 0x00B8 }, { 0x0328, 0x02DB },
    { 0x0335, 0x002D }, { 0x0337, 0x002F }, { 0x0338, 0x002F }
};

#define WP_NSPACING (sizeof( wp_spacing ) / sizeof( wp_spacing[0] ))

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */
#define WPU_TWIPS( wpu )    ((int)(wpu) * 6 / 5)
#define WP_DEPTH            3       /* a note in a header in ... */

/* the attributes, by WordPerfect's numbers */
#define WPA_XLARGE      0
#define WPA_VLARGE      1
#define WPA_LARGE       2
#define WPA_SMALL       3
#define WPA_FINE        4
#define WPA_SUPER       5
#define WPA_SUB         6
#define WPA_OUTLINE     7
#define WPA_ITALIC      8
#define WPA_SHADOW      9
#define WPA_REDLINE     10
#define WPA_DUNDER      11
#define WPA_BOLD        12
#define WPA_STRIKE      13
#define WPA_UNDER       14
#define WPA_SMALLCAPS   15

static const uint16_t wp_attr_flag[16] = {
    0, 0, 0, 0, 0, CF_SUPER, CF_SUB, CF_OUTLINE, CF_ITALIC, CF_SHADOW, 0,
    CF_DUNDER, CF_BOLD, CF_STRIKE, CF_UNDER, CF_SMALLCAPS
};

typedef struct {
    const uint8_t *data;
    uint32_t     len;
    WDOC        *doc;
    WBUILD       wb;
    int          version;       /* 42, 50, 51 or 60 */
    int          depth;         /* how far into headers and notes */
    int          mute;          /* text that is a number WordPerfect made */
    uint32_t     attrs;         /* the attributes that are on, a bit each */
    uint16_t     base_size;     /* half points, before Large and Small */
    uint8_t      justify;       /* AL_* until the next change */
    uint8_t      line_align;    /* AL_CENTER or AL_RIGHT for this paragraph, or 0 */
    uint8_t      page_next;     /* the next paragraph starts a page */
    uint8_t      page_set;      /* 5.x: the page's margins have been read */
    uint8_t      in_table, in_cell;
    int          page_left, page_right;     /* the page's margins, twips */
    int          mar_left, mar_right;       /* the margins in force */
    int          adj_left, adj_right, adj_first;  /* 6.x: paragraph adjustments */
    int          ind_left, ind_right, ind_first;  /* until the paragraph ends */
    /* 5.x: the fonts of the prefix */
    const uint8_t *fonts;
    uint32_t     nfonts;
    int          fonts50;
    const uint8_t *names;
    uint32_t     names_len;
    /* 6.x: the prefix's index */
    uint32_t     index_off, index_count;
    /* the table being read */
    unsigned int ncols, next_col;
    int          col_edge[MAX_CELLS];
    int          tab_left;
    TROW         row;
    /* 4.2 */
    uint8_t      one_shot;      /* Superscript or Subscript, for one character */
    uint8_t      dunder;        /* underlining is double */
    uint8_t      dead[64];      /* functions that have no closing gate left */
} WP;

static void wp5_body( WP *wp, uint32_t pos, uint32_t end );
static void wp6_body( WP *wp, uint32_t pos, uint32_t end );
static void wp42_body( WP *wp, uint32_t pos, uint32_t end );

static int wp_clamp( int value, int low, int high )
{
    return value < low ? low : value > high ? high : value;
}

static int wp_empty_line( const WP *wp )
{
    return wp->wb.para == NULL || wp->wb.para->len == 0;
}

static int wp_body_empty( const WP *wp )
{
    return wp->depth == 0 && wp->doc->body.count == 0;
}

static void wp_char( WP *wp, uint32_t code )
{
    if ( !wp->mute ) {
        wb_char( &wp->wb, code );
    }
}

/* a character of one of WordPerfect's sets */
static void wp_extchar( WP *wp, uint32_t set, uint32_t num )
{
    const WPSET *sets = wp->version >= 60 ? wp6_sets : wp5_sets;
    unsigned int index;
    uint32_t code = 0;

    if ( set == 0 ) {
        if ( num >= 0x20 && num < 0x7F ) {
            wp_char( wp, num );
        }
        return;
    }
    if ( set == 1 || (set == 10 && wp->version < 60) ) {
        for ( index = 0; index < WP_NPAIRS; index++ ) {
            if ( wp_pairs[index][0] == set && wp_pairs[index][1] == num ) {
                wp_char( wp, wp_pairs[index][2] );
                wp_char( wp, wp_pairs[index][3] );
                return;
            }
        }
    }
    if ( set < WP_NSETS && num < sets[set].count ) {
        code = sets[set].tab[num];
    }
    if ( code == 0 ) {
        wp->doc->lost |= LOST_TEXT;
        code = '?';
    }
    /* an accent on its own is the accent that takes a place */
    for ( index = 0; code >= 0x300 && code < 0x370 && index < WP_NSPACING; index++ ) {
        if ( wp_spacing[index][0] == code ) {
            code = wp_spacing[index][1];
            break;
        }
    }
    wp_char( wp, code );
}

/* the looks the attributes and the font size come to */
static void wp_looks( WP *wp )
{
    unsigned int index, size = wp->base_size;
    uint16_t flags = 0;

    for ( index = 0; index < 16; index++ ) {
        if ( wp->attrs & (1UL << index) ) {
            flags |= wp_attr_flag[index];
        }
    }
    if ( wp->attrs & (1UL << WPA_XLARGE) ) {
        size = size * 2;
    } else if ( wp->attrs & (1UL << WPA_VLARGE) ) {
        size = size * 3 / 2;
    } else if ( wp->attrs & (1UL << WPA_LARGE) ) {
        size = size * 6 / 5;
    } else if ( wp->attrs & (1UL << WPA_SMALL) ) {
        size = size * 4 / 5;
    } else if ( wp->attrs & (1UL << WPA_FINE) ) {
        size = size * 3 / 5;
    }
    wp->wb.chp.flags = flags;
    wp->wb.chp.size = (uint16_t)wp_clamp( (int)size, 2, 1638 );
}

static void wp_attr( WP *wp, uint32_t attr, int on )
{
    if ( attr < 16 ) {
        if ( on ) {
            wp->attrs |= 1UL << attr;
        } else {
            wp->attrs &= ~(1UL << attr);
        }
        wp_looks( wp );
    }
}

/* a size in 3600ths of an inch (a fiftieth of a point) */
static void wp_size( WP *wp, uint32_t size )
{
    if ( size >= 50 && size <= 40000UL ) {
        wp->base_size = (uint16_t)(size / 25);
        wp_looks( wp );
    }
}

/* a font by the name a file gave it: "Arial  (TT)", "Courier 10cpi",
   "Times New Roman Regular" are the faces Arial, Courier, Times New
   Roman */
static void wp_font( WP *wp, char *name )
{
    unsigned int used = str_len( name );
    char *cut;

    cut = str_chr( name, '(' );
    if ( cut != NULL && cut != name ) {
        *cut = 0;
        used = str_len( name );
    }
    while ( used && name[used - 1] == ' ' ) {
        name[--used] = 0;
    }
    if ( used > 8 && str_icmp( name + used - 8, " Regular" ) == 0 ) {
        name[used -= 8] = 0;
    }
    if ( used > 3 && str_icmp( name + used - 3, "cpi" ) == 0 ) {
        cut = str_rchr( name, ' ' );
        if ( cut != NULL && cut != name ) {
            *cut = 0;
        }
    }
    if ( name[0] ) {
        wp->wb.chp.font = (uint16_t)wd_font( wp->doc, name, FAM_ANY, -1 );
    }
}

/* the paragraph ends: its indents and alignment are made of what is in
   force, and what was for this paragraph only is forgotten */
static void wp_par( WP *wp, uint16_t flags )
{
    PAP *pap = &wp->wb.pap;

    pap->left = wp_clamp( wp->mar_left - wp->page_left + wp->adj_left + wp->ind_left, -14400, 31680 );
    pap->right = wp_clamp( wp->mar_right - wp->page_right + wp->adj_right + wp->ind_right, -14400, 31680 );
    pap->first = wp_clamp( wp->adj_first + wp->ind_first, -31680, 31680 );
    pap->align = wp->line_align ? wp->line_align : wp->justify;
    if ( wp->in_cell ) {
        flags |= PF_INTABLE;
    } else if ( wp->page_next && wp->depth == 0 ) {
        flags |= PF_PAGEBEFORE;
        wp->page_next = 0;
    }
    pap->flags = flags;
    wb_par( &wp->wb );
    wp->ind_left = wp->ind_right = wp->ind_first = 0;
    wp->line_align = 0;
}

/* a line was broken here by WordPerfect: the words on each side are
   still two words */
static void wp_soft_space( WP *wp )
{
    WPARA *para = wp->wb.para;
    uint16_t last;

    if ( para == NULL || para->len == 0 ) {
        return;
    }
    last = para->text[para->len - 1];
    if ( last != ' ' && last != '-' && last != WCH_SHY && last != WCH_TAB ) {
        wp_char( wp, ' ' );
    }
}

static void wp_page( WP *wp )
{
    if ( wp->depth || wp->in_cell ) {
        return;
    }
    if ( wp->wb.para != NULL ) {
        wp_par( wp, 0 );
    }
    wp->page_next = 1;
}

/* a tab that centers or sets right what follows: at the start of a
   line it is the paragraph's alignment, anywhere else a tab */
static void wp_align_tab( WP *wp, int align )
{
    if ( wp_empty_line( wp ) ) {
        wp->line_align = (uint8_t)align;
    } else {
        wp_char( wp, WCH_TAB );
    }
}

/* Indent: to "pos" from the paper's edge if the file says where, else
   a half inch more than before */
static void wp_indent( WP *wp, int pos, int both )
{
    int amount = pos - wp->mar_left - wp->adj_left;

    if ( pos <= 0 || amount <= wp->ind_left || amount > 20000 ) {
        amount = wp->ind_left + 720;
    }
    wp->ind_left = amount;
    if ( both ) {
        wp->ind_right = amount;
    }
}

/* Margin Release: the first line starts to the left of the rest, at
   "pos" from the paper's edge if the file says where, else a tab stop
   back */
static void wp_back_tab( WP *wp, int pos )
{
    int first = pos - (wp->mar_left + wp->adj_left + wp->ind_left);

    if ( !wp_empty_line( wp ) ) {
        return;
    }
    if ( pos > 0 && first < 0 && first > -14400 ) {
        wp->ind_first = first;
    } else if ( wp->ind_first > -14400 ) {
        wp->ind_first -= 720;
    }
}

/* --- tables --------------------------------------------------------- */

static void wp_cell_end( WP *wp, int row_end )
{
    uint16_t flags = PF_INTABLE | PF_CELLEND;
    unsigned int index;

    if ( !wp->in_cell ) {
        return;
    }
    if ( row_end ) {
        for ( index = 1; index < wp->row.ncells; index++ ) {
            if ( wp->row.edge[index] <= wp->row.edge[index - 1] ) {
                wp->row.edge[index] = wp->row.edge[index - 1] + 144;
            }
        }
        wp->row.left = wp->tab_left;
        wp->row.borders = 1;
        if ( !pap_set_row( &wp->wb.pap, &wp->row ) ) {
            wp->doc->failed = 1;
        }
        flags |= PF_ROWEND;
    }
    wp_par( wp, flags );
    if ( row_end ) {
        pap_set_row( &wp->wb.pap, NULL );
    }
    wp->in_cell = 0;
}

static void wp_tbl_start( WP *wp, int left )
{
    if ( wp->in_cell ) {
        wp_cell_end( wp, 1 );
    } else if ( wp->wb.para != NULL ) {
        wp_par( wp, 0 );
    }
    wp->in_table = 1;
    wp->ncols = 0;
    wp->next_col = 0;
    wp->tab_left = wp_clamp( left, 0, 14400 );
    wp->row.ncells = 0;
}

static void wp_tbl_column( WP *wp, int width )
{
    int from = wp->ncols ? wp->col_edge[wp->ncols - 1] : wp->tab_left;

    if ( wp->ncols < MAX_CELLS ) {
        wp->col_edge[wp->ncols++] = from + wp_clamp( width, 144, 31680 );
    }
}

static void wp_tbl_row( WP *wp )
{
    if ( !wp->in_table ) {
        wp_tbl_start( wp, 0 );
    }
    wp_cell_end( wp, 1 );
    wp->row.ncells = 0;
    wp->next_col = 0;
}

static void wp_tbl_cell( WP *wp, unsigned int span )
{
    int edge;

    if ( !wp->in_table ) {
        wp_tbl_start( wp, 0 );
    }
    if ( wp->row.ncells >= MAX_CELLS ) {
        wp_soft_space( wp );            /* more cells than a row can have */
        return;
    }
    if ( wp->in_cell ) {
        wp_cell_end( wp, 0 );
    } else if ( wp->wb.para != NULL ) {
        wp_par( wp, 0 );
    }
    if ( span < 1 || span > MAX_CELLS ) {
        span = 1;
    }
    wp->next_col += span;
    if ( wp->next_col <= wp->ncols ) {
        edge = wp->col_edge[wp->next_col - 1];
    } else {
        edge = (wp->ncols ? wp->col_edge[wp->ncols - 1] : wp->tab_left) +
               (int)(wp->next_col - wp->ncols) * 1440;
    }
    wp->row.edge[wp->row.ncells++] = edge;
    wp->in_cell = 1;
}

static void wp_tbl_off( WP *wp )
{
    wp_cell_end( wp, 1 );
    wp->in_table = 0;
}

/* --- text that is somewhere else: a header, a note ------------------ */

static void wp_sub( WP *wp, STORY *story, uint32_t pos, uint32_t end )
{
    WBUILD   keep_wb;
    uint32_t keep_attrs = wp->attrs;
    uint16_t keep_size = wp->base_size;
    int      keep_mute = wp->mute;
    uint8_t  keep_align = wp->line_align, keep_table = wp->in_table, keep_cell = wp->in_cell;
    uint8_t  keep_justify = wp->justify;
    int      keep_left = wp->ind_left, keep_right = wp->ind_right, keep_first = wp->ind_first;
    int      keep_mar_left = wp->mar_left, keep_mar_right = wp->mar_right;

    if ( story == NULL || wp->depth >= WP_DEPTH || pos > end || end > wp->len ) {
        return;
    }
    keep_wb = wp->wb;
    wb_init( &wp->wb, wp->doc, story );
    wp->wb.chp = keep_wb.chp;
    wp->wb.chp.flags = 0;
    wp->attrs = 0;
    wp->mute = 0;
    wp->line_align = 0;
    wp->in_table = wp->in_cell = 0;
    wp->ind_left = wp->ind_right = wp->ind_first = 0;
    wp_looks( wp );
    wp->depth++;
    if ( wp->version >= 60 ) {
        wp6_body( wp, pos, end );
    } else if ( wp->version >= 50 ) {
        wp5_body( wp, pos, end );
    } else {
        wp42_body( wp, pos, end );
    }
    if ( wp->in_cell ) {
        wp_cell_end( wp, 1 );
    }
    if ( wp->wb.para != NULL ) {
        wp_par( wp, 0 );
    }
    wb_done( &wp->wb );
    wp->depth--;
    wp->wb = keep_wb;
    wp->attrs = keep_attrs;
    wp->base_size = keep_size;
    wp->mute = keep_mute;
    wp->line_align = keep_align;
    wp->in_table = keep_table;
    wp->in_cell = keep_cell;
    wp->justify = keep_justify;
    wp->ind_left = keep_left;
    wp->ind_right = keep_right;
    wp->ind_first = keep_first;
    wp->mar_left = keep_mar_left;
    wp->mar_right = keep_mar_right;
}

/* a header (0, 1) or footer (2, 3): the first of each is the document's */
static void wp_hdrftr( WP *wp, unsigned int which, uint32_t pos, uint32_t end )
{
    STORY *story = which < 2 ? &wp->doc->header : &wp->doc->footer;

    if ( which > 3 || wp->depth != 0 || story->count != 0 ) {
        return;
    }
    wp_sub( wp, story, pos, end );
}

static void wp_note( WP *wp, int kind, uint32_t pos, uint32_t end )
{
    int obj;

    if ( wp->depth != 0 ) {
        return;                         /* no notes in a header or a note */
    }
    obj = wd_note_new( wp->doc, kind );
    if ( obj < 0 ) {
        return;
    }
    wb_object( &wp->wb, obj );
    wp_sub( wp, wd_note_story( wp->doc, obj ), pos, end );
}

/* ------------------------------------------------------------------ */
/* WordPerfect 5.0 and 5.1                                             */
/* ------------------------------------------------------------------ */
static const uint8_t wp5_fixed[16] = { 4, 9, 11, 3, 3, 5, 6, 7, 4, 5, 6, 6, 8, 10, 10, 12 };

/* a font of the prefix's list, and the size it is listed with */
static void wp5_font( WP *wp, uint32_t number, uint32_t size )
{
    char name[FONT_NAME_LEN];
    const uint8_t *entry;
    uint32_t off;
    unsigned int used = 0;

    if ( number < wp->nfonts ) {
        entry = wp->fonts + number * 86;
        off = LE16( entry + 18 );
        if ( size == 0 ) {
            size = LE16( entry + (wp->fonts50 ? 22 : 47) );
        }
        while ( off < wp->names_len && wp->names[off] != 0 && used < FONT_NAME_LEN - 1 ) {
            name[used++] = (char)(wp->names[off] < 0x7F ? wp->names[off] : '?');
            off++;
        }
        name[used] = 0;
        wp_font( wp, name );
    }
    wp_size( wp, size );
}

/* the tab stops of a Tab Set: 40 places, then a type for each in half
   a byte; 5.1 adds whether they are counted from the margin */
static void wp5_tabs( WP *wp, const uint8_t *body, uint32_t len )
{
    WTAB tabs[MAX_TABS];
    unsigned int index;
    uint32_t pos, kind;
    int base = wp->mar_left, where;
    int count = 0;

    if ( len < 200 ) {
        return;
    }
    if ( len >= 204 && LE16( body + 202 ) != 0xFFFF ) {
        base = WPU_TWIPS( LE16( body + 202 ) );     /* counted from the margin */
    }
    mem_set( tabs, 0, sizeof( tabs ) );
    for ( index = 0; index < 40; index++ ) {
        pos = LE16( body + 100 + index * 2 );
        if ( pos == 0xFFFF ) {
            break;
        }
        kind = body[180 + index / 2];
        kind = (index & 1) ? kind & 0x0F : kind >> 4;
        where = WPU_TWIPS( pos ) - base;
        if ( where <= 0 || where > 31680 ) {
            continue;
        }
        tabs[count].pos = where;
        tabs[count].kind = (uint8_t)((kind & 3) == 1 ? TAB_CENTER : (kind & 3) == 2 ? TAB_RIGHT :
                                     (kind & 3) == 3 ? TAB_DECIMAL : TAB_LEFT);
        tabs[count].lead = (uint8_t)((kind & 4) ? LEAD_DOTS : LEAD_NONE);
        count++;
    }
    if ( !pap_set_tabs( &wp->wb.pap, tabs, count ) ) {
        wp->doc->failed = 1;
    }
}

/* Define Table: how many columns, and how wide each is */
static void wp5_table( WP *wp, const uint8_t *body, uint32_t len )
{
    uint32_t old, count, at;
    unsigned int index;

    if ( len < 4 ) {
        return;
    }
    old = LE16( body + 2 );
    if ( old > 255 ) {
        return;
    }
    at = 4 + 20 + 5 * old;
    if ( !IN_RANGE( at, 24, len ) ) {
        return;
    }
    count = LE16( body + at + 2 );
    if ( count > MAX_CELLS || !IN_RANGE( at + 24, count * 2, len ) ) {
        return;
    }
    wp_tbl_start( wp, 0 );
    for ( index = 0; index < count; index++ ) {
        wp_tbl_column( wp, WPU_TWIPS( LE16( body + at + 24 + index * 2 ) ) );
    }
}

static void wp5_page_format( WP *wp, uint32_t sub, const uint8_t *body, uint32_t len )
{
    PAGESETUP *page = &wp->doc->page;
    int one, two;
    uint32_t value;

    switch ( sub ) {
    case 1:                             /* left and right margins */
        if ( len >= 8 ) {
            one = WPU_TWIPS( LE16( body + 4 ) );
            two = WPU_TWIPS( LE16( body + 6 ) );
            /* the first ones, before any text, are the page's */
            if ( wp_body_empty( wp ) && wp_empty_line( wp ) && !wp->page_set ) {
                page->left = wp->page_left = one;
                page->right = wp->page_right = two;
                wp->page_set = 1;
            }
            wp->mar_left = one;
            wp->mar_right = two;
        }
        break;
    case 2:                             /* line spacing, in 256ths */
        if ( len >= 4 ) {
            value = LE16( body + 2 ) * 240 / 256;
            if ( value >= 60 && value <= 2400 ) {
                wp->wb.pap.line = (uint16_t)value;
                wp->wb.pap.line_rule = LS_MULTIPLE;
            }
        }
        break;
    case 4:
        wp5_tabs( wp, body, len );
        break;
    case 5:                             /* top and bottom margins */
        if ( len >= 8 && wp_body_empty( wp ) ) {
            page->top = WPU_TWIPS( LE16( body + 4 ) );
            page->bottom = WPU_TWIPS( LE16( body + 6 ) );
        }
        break;
    case 6:
        if ( len >= 2 ) {
            wp->justify = (uint8_t)(body[1] == 0 ? AL_LEFT : body[1] == 2 ? AL_CENTER :
                                    body[1] == 3 ? AL_RIGHT : AL_JUSTIFY);
        }
        break;
    case 11:                            /* the paper */
        if ( len >= 190 && wp_body_empty( wp ) ) {
            one = WPU_TWIPS( LE16( body + 97 ) );
            two = WPU_TWIPS( LE16( body + 95 ) );
            if ( one >= 1440 && two >= 1440 ) {
                page->width = one;
                page->height = two;
                page->landscape = (uint8_t)(one > two);
            }
        }
        break;
    default:
        break;
    }
}

static void wp5_var( WP *wp, uint32_t code, uint32_t sub, uint32_t pos, uint32_t len )
{
    const uint8_t *body = wp->data + pos;
    uint32_t skip;

    switch ( code ) {
    case 0xD0:
        wp5_page_format( wp, sub, body, len );
        break;
    case 0xD1:
        if ( sub == 0 && len >= 6 ) {
            wp->wb.chp.color = (body[3] | body[4] | body[5]) ? RGB( body[3], body[4], body[5] ) : COLOR_AUTO;
        } else if ( sub == 1 && len >= 26 ) {
            wp5_font( wp, body[25], len >= 30 ? LE16( body + 28 ) : 0 );
        }
        break;
    case 0xD2:
        if ( sub == 0x0B && wp->depth == 0 ) {
            wp5_table( wp, body, len );
        }
        break;
    case 0xD5:
        if ( len > 18 && body[7] != 0 ) {
            wp_hdrftr( wp, sub, pos + 18, pos + len );
        }
        break;
    case 0xD6:
        if ( sub == 0 && len >= 4 ) {
            skip = 4 + 2 * ((uint32_t)body[3] + 1) + 9;
        } else {
            skip = 7;
        }
        if ( sub <= 1 && skip <= len ) {
            wp_note( wp, sub == 0 ? NOTE_FOOT : NOTE_END, pos + skip, pos + len );
        }
        break;
    case 0xD8:
        if ( sub == 0 ) {
            wp_char( wp, WCH_DATE );
        }
        break;
    case 0xDA:
        wp->doc->lost |= sub == 0 ? LOST_IMAGES : sub >= 5 ? 0 : LOST_LAYOUT;
        break;
    case 0xDC:
    case 0xDD:
        if ( wp->depth != 0 ) {
            break;
        }
        if ( sub == 0 ) {
            wp_tbl_cell( wp, code == 0xDC && len >= 3 ? (uint32_t)(body[2] & 0x7F) : 1 );
        } else if ( sub == 1 || sub == 3 ) {
            wp_tbl_row( wp );
        } else if ( sub == 2 ) {
            wp_tbl_off( wp );
        }
        break;
    default:
        break;
    }
}

static void wp5_fixed_fn( WP *wp, const uint8_t *body )
{
    uint32_t kind;

    switch ( body[0] ) {
    case 0xC0:
        wp_extchar( wp, body[2], body[1] );
        break;
    case 0xC1:
        kind = body[1] >> 3;
        if ( kind == 0x18 || kind == 0x1C ) {
            wp_align_tab( wp, AL_CENTER );
        } else if ( kind == 0x0C ) {
            wp_align_tab( wp, AL_RIGHT );
        } else if ( kind == 0x10 ) {
            wp_back_tab( wp, WPU_TWIPS( LE16( body + 4 ) ) );
        } else {
            wp_char( wp, WCH_TAB );
        }
        break;
    case 0xC2:
        wp_indent( wp, WPU_TWIPS( LE16( body + 6 ) ), body[1] & 1 );
        break;
    case 0xC3:
        wp_attr( wp, body[1], 1 );
        break;
    case 0xC4:
        wp_attr( wp, body[1], 0 );
        break;
    default:
        break;
    }
}

static void wp5_single( WP *wp, uint32_t code )
{
    switch ( code ) {
    case 0x0A:
    case 0x8C:
    case 0x99:
        wp_par( wp, 0 );
        break;
    case 0x0B:
    case 0x0D:
    case 0x90:
    case 0x91:
    case 0x92:
        wp_soft_space( wp );
        break;
    case 0x0C:
        wp_page( wp );
        break;
    case 0x02:
        wp_char( wp, WCH_PAGENUM );
        break;
    case 0x09:
        wp_char( wp, WCH_TAB );
        break;
    case 0x81:
        wp->justify = AL_JUSTIFY;
        break;
    case 0x82:
        wp->justify = AL_LEFT;
        break;
    case 0xA0:
        wp_char( wp, WCH_NBSP );
        break;
    case 0xA9:
    case 0xAA:
    case 0xAB:
        wp_char( wp, '-' );
        break;
    case 0xAC:
    case 0xAD:
    case 0xAE:
        wp_char( wp, WCH_SHY );
        break;
    default:
        if ( code >= 0x20 && code < 0x7F ) {
            wp_char( wp, code );
        }
        break;
    }
}

static void wp5_body( WP *wp, uint32_t pos, uint32_t end )
{
    const uint8_t *data = wp->data;
    uint32_t code, size, sub;

    while ( pos < end ) {
        code = data[pos];
        if ( code < 0xC0 ) {
            pos++;
            wp5_single( wp, code );
        } else if ( code < 0xD0 ) {
            size = wp5_fixed[code - 0xC0];
            if ( !IN_RANGE( pos, size, end ) || data[pos + size - 1] != code ) {
                pos++;                  /* not a whole function */
                continue;
            }
            wp5_fixed_fn( wp, data + pos );
            pos += size;
        } else {
            /* function, kind, length ... length, kind, function: the
               length counts what follows it */
            if ( !IN_RANGE( pos, 8, end ) ) {
                break;
            }
            sub = data[pos + 1];
            size = LE16( data + pos + 2 );
            if ( size < 4 || !IN_RANGE( pos + 4, size, end ) || LE16( data + pos + size ) != size ||
                 data[pos + size + 2] != sub || data[pos + size + 3] != code ) {
                pos++;
                continue;
            }
            wp5_var( wp, code, sub, pos + 4, size - 4 );
            pos += size + 4;
        }
    }
}

/* the prefix: blocks of ten-byte entries, each a kind, a length and a
   place; the list of fonts and their names are what is wanted */
static void wp5_prefix( WP *wp )
{
    const uint8_t *data = wp->data;
    uint32_t pos = 16;
    unsigned int blocks;
    uint32_t count, index, next, kind, size, off;

    for ( blocks = 0; blocks < 64 && IN_RANGE( pos, 10, wp->len ); blocks++ ) {
        if ( LE16( data + pos ) != 0xFFFB ) {
            break;
        }
        count = LE16( data + pos + 2 );
        next = LE32( data + pos + 6 );
        if ( count > 256 ) {
            count = 256;
        }
        for ( index = 1; index < count; index++ ) {
            if ( !IN_RANGE( pos + index * 10, 10, wp->len ) ) {
                break;
            }
            kind = LE16( data + pos + index * 10 );
            size = LE32( data + pos + index * 10 + 2 );
            off = LE32( data + pos + index * 10 + 6 );
            if ( size == 0 || !IN_RANGE( off, size, wp->len ) ) {
                continue;
            }
            if ( (kind == 15 || kind == 2) && wp->fonts == NULL ) {
                wp->fonts = data + off;
                wp->nfonts = size / 86;
                wp->fonts50 = kind == 2;
            } else if ( kind == 7 && wp->names == NULL ) {
                wp->names = data + off;
                wp->names_len = size;
            }
        }
        if ( next <= pos ) {
            break;
        }
        pos = next;
    }
}

/* ------------------------------------------------------------------ */
/* WordPerfect 6.0 and later                                           */
/* ------------------------------------------------------------------ */
static const uint8_t wp6_fixed[16] = { 4, 5, 3, 3, 3, 3, 4, 4, 4, 5, 5, 6, 6, 8, 8, 0 };

/* packet "id" of the prefix: where, how long, what kind; 0 for none */
static int wp6_packet( const WP *wp, uint32_t id, uint32_t *off, uint32_t *size, uint32_t *kind )
{
    const uint8_t *entry;

    if ( id < 1 || id >= wp->index_count ) {
        return 0;
    }
    entry = wp->data + wp->index_off + 14 * id;
    *kind = entry[1];
    *size = LE32( entry + 6 );
    *off = LE32( entry + 10 );
    return *size != 0 && IN_RANGE( *off, *size, wp->len );
}

/* the font a packet describes: its name is sixteen-bit characters */
static void wp6_font( WP *wp, uint32_t id )
{
    char name[FONT_NAME_LEN];
    const uint8_t *body;
    uint32_t off, size, kind, count;
    unsigned int index, used = 0;

    if ( !wp6_packet( wp, id, &off, &size, &kind ) || kind != 0x55 || size < 24 ) {
        return;
    }
    body = wp->data + off;
    count = LE16( body + 22 ) / 2;
    if ( count > (size - 24) / 2 ) {
        count = (size - 24) / 2;
    }
    for ( index = 0; index < count && used < FONT_NAME_LEN - 1; index++ ) {
        if ( body[24 + index * 2 + 1] != 0 ) {
            continue;
        }
        if ( body[24 + index * 2] == 0 ) {
            break;
        }
        name[used++] = (char)(body[24 + index * 2] < 0x7F ? body[24 + index * 2] : '?');
    }
    name[used] = 0;
    wp_font( wp, name );
}

/* the text a function keeps in the prefix: the first of its packets
   that is text */
static int wp6_text_packet( const WP *wp, const uint8_t *ids, uint32_t nids, uint32_t *off, uint32_t *size )
{
    unsigned int index;
    uint32_t kind, blocks;

    for ( index = 0; index < nids; index++ ) {
        if ( wp6_packet( wp, LE16( ids + index * 2 ), off, size, &kind ) && kind == 0x08 ) {
            /* how many blocks, a pointer, the length of each block;
               then the blocks, one after another */
            if ( *size < 6 ) {
                return 0;
            }
            blocks = LE16( wp->data + *off );
            if ( blocks > (*size - 6) / 4 ) {
                return 0;
            }
            *off += 6 + blocks * 4;
            *size -= 6 + blocks * 4;
            return 1;
        }
    }
    return 0;
}

static void wp6_tabs( WP *wp, const uint8_t *body, uint32_t len )
{
    WTAB         tabs[MAX_TABS];
    unsigned int index;
    uint32_t     at = 4, kind, pos, repeat;
    int          base, where = 0;
    int          count = 0;
    uint8_t      tab_kind = TAB_LEFT, lead = LEAD_NONE;

    if ( len < 4 ) {
        return;
    }
    base = (body[0] & 1) ? WPU_TWIPS( LE16( body + 1 ) ) : wp->mar_left;
    mem_set( tabs, 0, sizeof( tabs ) );
    for ( index = 0; index < body[3] && IN_RANGE( at, 3, len ); index++ ) {
        kind = body[at];
        pos = LE16( body + at + 1 );
        at += 3;
        if ( kind & 0x80 ) {
            repeat = kind & 0x7F;       /* the one before, again and again */
        } else {
            repeat = 0;
            tab_kind = (uint8_t)((kind & 0x0F) == 1 ? TAB_CENTER : (kind & 0x0F) == 2 ? TAB_RIGHT :
                                 (kind & 0x0F) == 3 ? TAB_DECIMAL : TAB_LEFT);
            lead = (uint8_t)((kind & 0x10) ? LEAD_DOTS : LEAD_NONE);
            where = WPU_TWIPS( pos ) - base;
        }
        do {
            if ( repeat ) {
                where += WPU_TWIPS( pos );
            }
            if ( where > 0 && where <= 31680 && count < MAX_TABS ) {
                tabs[count].pos = where;
                tabs[count].kind = tab_kind;
                tabs[count].lead = lead;
                count++;
            }
        } while ( repeat-- > 1 );
    }
    if ( !pap_set_tabs( &wp->wb.pap, tabs, count ) ) {
        wp->doc->failed = 1;
    }
}

/* the end of a line, a cell, a row, a page: by the numbers of the End
   of Line group, which the one-byte codes are turned into too */
static void wp6_eol( WP *wp, uint32_t sub )
{
    if ( (sub >= 0x01 && sub <= 0x03) || (sub >= 0x14 && sub <= 0x16) ) {
        wp_soft_space( wp );
    } else if ( (sub >= 0x04 && sub <= 0x08) || (sub >= 0x17 && sub <= 0x1B) ) {
        /* no number WordPerfect made runs past the end of a line: one
           whose end is missing does not take the rest of the text */
        wp->mute = 0;
        wp_par( wp, 0 );
    } else if ( sub == 0x09 || sub == 0x1C ) {
        wp_page( wp );
    } else if ( wp->depth != 0 ) {
        return;
    } else if ( sub == 0x0A ) {
        wp_tbl_cell( wp, 1 );
    } else if ( sub >= 0x0B && sub <= 0x10 ) {
        wp_tbl_row( wp );
        wp_tbl_cell( wp, 1 );
    } else if ( sub >= 0x11 && sub <= 0x13 ) {
        wp_tbl_off( wp );
    }
}

static void wp6_var( WP *wp, uint32_t code, uint32_t sub, uint32_t pos, uint32_t size )
{
    PAGESETUP *page = &wp->doc->page;
    const uint8_t *data = wp->data, *ids = NULL, *body;
    uint32_t cur = pos + 5, limit = pos + size - 3, nids = 0, len, off, psize, value;
    uint32_t flags = data[pos + 4];
    int one, two;

    /* the packets it uses, then the part that is kept when it is deleted */
    if ( flags & 0x80 ) {
        nids = data[cur++];
        if ( !IN_RANGE( cur, nids * 2, limit ) ) {
            return;
        }
        ids = data + cur;
        cur += nids * 2;
    }
    if ( !IN_RANGE( cur, 2, limit ) ) {
        return;
    }
    len = LE16( data + cur );
    cur += 2;
    if ( !IN_RANGE( cur, len, limit ) ) {
        len = limit - cur;
    }
    body = data + cur;

    switch ( code ) {
    case 0xD0:
        wp6_eol( wp, sub );
        break;
    case 0xD1:
        if ( (sub == 0 || sub == 1) && len >= 2 && wp_body_empty( wp ) ) {
            if ( sub == 0 ) {
                page->top = WPU_TWIPS( LE16( body ) );
            } else {
                page->bottom = WPU_TWIPS( LE16( body ) );
            }
        } else if ( sub == 0x11 && len >= 9 && wp_body_empty( wp ) ) {
            one = WPU_TWIPS( LE16( body + 5 ) );
            two = WPU_TWIPS( LE16( body + 3 ) );
            if ( one >= 1440 && two >= 1440 ) {
                page->width = one;
                page->height = two;
                page->landscape = (uint8_t)(one > two);
            }
        }
        break;
    case 0xD2:
        if ( (sub == 0 || sub == 1) && len >= 2 ) {
            one = WPU_TWIPS( LE16( body ) );
            if ( wp_body_empty( wp ) && wp_empty_line( wp ) ) {
                if ( sub == 0 ) {
                    page->left = wp->page_left = one;
                } else {
                    page->right = wp->page_right = one;
                }
            }
            if ( sub == 0 ) {
                wp->mar_left = one;
            } else {
                wp->mar_right = one;
            }
        } else if ( sub == 2 ) {
            wp->doc->lost |= LOST_LAYOUT;   /* text columns */
        }
        break;
    case 0xD3:
        if ( sub == 0x01 && len >= 4 ) {
            value = (LE32( body ) >> 8) * 240 / 256;
            if ( value >= 60 && value <= 2400 ) {
                wp->wb.pap.line = (uint16_t)value;
                wp->wb.pap.line_rule = LS_MULTIPLE;
            }
        } else if ( sub == 0x04 ) {
            wp6_tabs( wp, body, len );
        } else if ( sub == 0x05 && len >= 1 ) {
            wp->justify = (uint8_t)(body[0] == 0 ? AL_LEFT : body[0] == 2 ? AL_CENTER :
                                    body[0] == 3 ? AL_RIGHT : AL_JUSTIFY);
        } else if ( sub == 0x0A && len >= 4 ) {
            value = (LE32( body ) >> 8) * 240 / 256;
            wp->wb.pap.after = (uint16_t)(value > 240 && value <= 2400 ? value - 240 : 0);
        } else if ( sub >= 0x0B && sub <= 0x0D && len >= 2 ) {
            value = LE16( body );
            one = WPU_TWIPS( value >= 0x8000UL ? (int)value - 0x10000L : (int)value );
            if ( sub == 0x0B ) {
                wp->adj_first = one;
            } else if ( sub == 0x0C ) {
                wp->adj_left = one;
            } else {
                wp->adj_right = one;
            }
        }
        break;
    case 0xD4:
        if ( sub == 0x18 && len >= 3 ) {
            wp->wb.chp.color = (body[0] | body[1] | body[2]) ? RGB( body[0], body[1], body[2] ) : COLOR_AUTO;
        } else if ( sub == 0x1A ) {
            if ( nids >= 1 ) {
                wp6_font( wp, LE16( ids ) );
            }
            if ( len >= 8 ) {
                wp_size( wp, LE16( body + 6 ) );
            }
        } else if ( sub == 0x1B && len >= 2 ) {
            wp_size( wp, LE16( body ) );
        } else if ( sub == 0x2A && wp->depth == 0 ) {
            wp_tbl_start( wp, 0 );
        } else if ( sub == 0x2C && len >= 3 && wp->depth == 0 && wp->in_table ) {
            wp_tbl_column( wp, WPU_TWIPS( LE16( body + 1 ) ) );
        }
        break;
    case 0xD6:
        if ( sub <= 3 && len >= 1 && body[0] != 0 && wp6_text_packet( wp, ids, nids, &off, &psize ) ) {
            wp_hdrftr( wp, sub, off, off + psize );
        } else if ( sub == 4 || sub == 5 ) {
            wp->doc->lost |= LOST_LAYOUT;   /* a watermark */
        }
        break;
    case 0xD7:
        /* between On and Off is the number as it was shown; the note's
           text is a packet of the prefix */
        if ( sub == 0 || sub == 2 ) {
            if ( wp->depth == 0 && wp6_text_packet( wp, ids, nids, &off, &psize ) ) {
                wp_note( wp, sub == 0 ? NOTE_FOOT : NOTE_END, off, off + psize );
            }
            wp->mute++;
        } else if ( (sub == 1 || sub == 3) && wp->mute ) {
            wp->mute--;
        }
        break;
    case 0xDA:
        /* a number WordPerfect made: the page's becomes a field, and
           the digits that follow, up to Off, are dropped */
        if ( !(sub & 1) ) {
            if ( sub == 0x04 || sub == 0x06 ) {
                wp_char( wp, WCH_PAGENUM );
            } else if ( sub == 0x14 ) {
                wp_char( wp, WCH_NUMPAGES );
            }
            if ( sub == 0x04 || sub == 0x06 || sub == 0x14 || sub == 0x0E || sub == 0x10 ) {
                wp->mute++;
            }
        } else if ( (sub == 0x05 || sub == 0x07 || sub == 0x15 || sub == 0x0F || sub == 0x11) && wp->mute ) {
            wp->mute--;
        }
        break;
    case 0xDF:
        wp->doc->lost |= sub == 3 ? 0 : LOST_OBJECTS;
        break;
    case 0xE0:
        if ( flags & 0x40 ) {
            break;                      /* one WordPerfect itself passes over */
        }
        sub >>= 3;                      /* the low bits say how it is shown */
        if ( sub == 0x08 || sub == 0x09 ) {
            wp_align_tab( wp, AL_CENTER );
        } else if ( sub == 0x10 ) {
            wp_align_tab( wp, AL_RIGHT );
        } else if ( sub == 0x06 || sub == 0x07 ) {
            wp_indent( wp, 0, sub == 0x07 );
        } else if ( sub == 0x00 ) {
            wp_back_tab( wp, 0 );
        } else {
            wp_char( wp, WCH_TAB );
        }
        break;
    default:
        break;
    }
}

static void wp6_single( WP *wp, uint32_t code )
{
    /* the one-byte ends of line, as the End of Line group numbers them */
    static const uint8_t eol_of[0xD0 - 0xB4] = {
        0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14,   /* B4-BC */
        0x13, 0x12, 0x11,                                       /* BD-BF */
        0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,               /* C0-C6 */
        0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01    /* C7-CF */
    };

    if ( code >= 0xB4 ) {
        wp6_eol( wp, eol_of[code - 0xB4] );
        return;
    }
    switch ( code ) {
    case 0x80:
        wp_char( wp, ' ' );
        break;
    case 0x81:
        wp_char( wp, WCH_NBSP );
        break;
    case 0x82:
    case 0x83:
        wp_char( wp, WCH_SHY );
        break;
    case 0x84:
        wp_char( wp, '-' );
        break;
    case 0x87:
        wp6_eol( wp, 0x04 );
        break;
    default:
        break;
    }
}

static void wp6_body( WP *wp, uint32_t pos, uint32_t end )
{
    const uint8_t *data = wp->data;
    uint32_t code, size;

    while ( pos < end ) {
        code = data[pos];
        if ( code < 0x80 ) {
            pos++;
            if ( code > 0x20 ) {
                wp_char( wp, code == 0x7F ? 0 : code );
            } else if ( code != 0 ) {
                wp_char( wp, wp6_low[code - 1] );
            }
        } else if ( code < 0xD0 ) {
            pos++;
            wp6_single( wp, code );
        } else if ( code < 0xF0 ) {
            /* group, kind, size, flags ... size, group: the size is of
               all of it */
            if ( !IN_RANGE( pos, 10, end ) ) {
                break;
            }
            size = LE16( data + pos + 2 );
            if ( size < 10 || !IN_RANGE( pos, size, end ) || LE16( data + pos + size - 3 ) != size ||
                 data[pos + size - 1] != code ) {
                pos++;
                continue;
            }
            wp6_var( wp, code, data[pos + 1], pos, size );
            pos += size;
        } else {
            size = wp6_fixed[code - 0xF0];
            if ( size == 0 || !IN_RANGE( pos, size, end ) || data[pos + size - 1] != code ) {
                pos++;
                continue;
            }
            if ( code == 0xF0 ) {
                wp_extchar( wp, data[pos + 2], data[pos + 1] );
            } else if ( code == 0xF2 ) {
                wp_attr( wp, data[pos + 1], 1 );
            } else if ( code == 0xF3 ) {
                wp_attr( wp, data[pos + 1], 0 );
            }
            pos += size;
        }
    }
}

/* the prefix's index, and the font the document starts in */
static void wp6_prefix( WP *wp )
{
    const uint8_t *data = wp->data;
    uint32_t at = LE16( data + 14 ), count;
    unsigned int index;
    uint32_t off, size, kind;

    if ( at < 16 || !IN_RANGE( at, 14, wp->len ) ) {
        return;
    }
    count = LE16( data + at + 2 );
    if ( count > (wp->len - at) / 14 ) {
        count = (wp->len - at) / 14;
    }
    wp->index_off = at;
    wp->index_count = count;
    for ( index = 1; index < count; index++ ) {
        if ( wp6_packet( wp, index, &off, &size, &kind ) && kind == 0x25 && size >= 6 ) {
            wp6_font( wp, LE16( data + off + 2 ) );
            wp_size( wp, LE16( data + off + 4 ) );
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* WordPerfect 4.2                                                     */
/* ------------------------------------------------------------------ */
#define WP42_COL    144             /* a column: a tenth of an inch */
#define WP42_HALF   120             /* half a line: a twelfth */

/* how long each function C0h to FFh is, with both its gates; 0 for one
   that runs to its closing gate */
static const uint8_t wp42_size[64] = {
    6, 4, 3, 5, 5, 6, 4, 6, 8, 42, 3, 6, 4, 3, 4, 3,
    6, 0, 0, 4, 4, 4, 6, 0, 4, 4, 4, 4, 0, 24, 4, 0,
    4, 3, 0, 150, 6, 23, 11, 3, 3, 0, 0, 32, 4, 0, 44, 18,
    6, 106, 0, 100, 4, 0, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/* the function at "pos", whole: its length, or 0.  "dead" remembers the
   functions whose closing gate was looked for to the end and not found,
   so that a file full of them is not searched again for each */
static uint32_t wp42_extent( const uint8_t *data, uint32_t pos, uint32_t end, uint8_t *dead )
{
    uint32_t code = data[pos], size = wp42_size[code - 0xC0], at;

    if ( size ) {
        return IN_RANGE( pos, size, end ) && data[pos + size - 1] == code ? size : 0;
    }
    if ( dead[code - 0xC0] ) {
        return 0;
    }
    for ( at = pos + 1; at < end; at++ ) {
        if ( data[at] != code ) {
            continue;
        }
        /* a header ends FFh, lines, kind, gate */
        if ( code == 0xD1 && (at < pos + 10 || data[at - 3] != 0xFF) ) {
            continue;
        }
        return at - pos + 1;
    }
    dead[code - 0xC0] = 1;
    return 0;
}

static void wp42_char( WP *wp, uint32_t code )
{
    wp_char( wp, code );
    if ( wp->one_shot ) {
        /* Superscript and Subscript are for one character */
        wp->attrs &= ~((1UL << WPA_SUPER) | (1UL << WPA_SUB));
        wp->one_shot = 0;
        wp_looks( wp );
    }
}

/* Tab Reset: a bit for each column that has a stop */
static void wp42_tabs( WP *wp, const uint8_t *table )
{
    WTAB tabs[MAX_TABS];
    unsigned int column;
    int where;
    int count = 0;

    mem_set( tabs, 0, sizeof( tabs ) );
    for ( column = 0; column < 256 && count < MAX_TABS; column++ ) {
        if ( table[column / 8] & (0x80 >> (column & 7)) ) {
            where = (int)column * WP42_COL - wp->mar_left;
            if ( where > 0 ) {
                tabs[count].pos = where;
                tabs[count].kind = TAB_LEFT;
                count++;
            }
        }
    }
    if ( !pap_set_tabs( &wp->wb.pap, tabs, count ) ) {
        wp->doc->failed = 1;
    }
}

static void wp42_fn( WP *wp, uint32_t pos, uint32_t size )
{
    PAGESETUP *page = &wp->doc->page;
    const uint8_t *body = wp->data + pos;
    uint32_t at;
    int one, two;

    switch ( body[0] ) {
    case 0xC0:                          /* margins, as columns */
        one = (int)body[3] * WP42_COL;
        two = page->width - (int)body[4] * WP42_COL;
        if ( two < 0 ) {
            two = 0;
        }
        if ( wp_body_empty( wp ) && wp_empty_line( wp ) ) {
            page->left = wp->page_left = one;
            page->right = wp->page_right = two;
        }
        wp->mar_left = one;
        wp->mar_right = two;
        break;
    case 0xC1:                          /* spacing, in half lines */
        if ( body[2] >= 1 && body[2] <= 20 ) {
            wp->wb.pap.line = (uint16_t)(body[2] * 120);
            wp->wb.pap.line_rule = LS_MULTIPLE;
        }
        break;
    case 0xC2:
        if ( wp_empty_line( wp ) ) {
            wp->ind_first -= (int)body[1] * WP42_COL;
        }
        break;
    case 0xC3:
        if ( body[1] == 0 ) {
            wp_align_tab( wp, AL_CENTER );
        } else {
            wp_char( wp, WCH_TAB );
        }
        break;
    case 0xC4:
        if ( (body[1] & 0x7F) == 0x0A ) {
            wp_align_tab( wp, AL_RIGHT );
        } else {
            wp_char( wp, WCH_TAB );
        }
        break;
    case 0xCB:                          /* pitch: characters to the inch */
        if ( body[3] >= 5 && body[3] <= 24 ) {
            wp->base_size = (uint16_t)(240 / body[3]);
            wp_looks( wp );
        }
        break;
    case 0xCC:
        one = (int)body[2] * WP42_COL - wp->mar_left;
        if ( one > 0 ) {
            wp->ind_left = one;
        }
        break;
    case 0xCE:
        if ( wp_body_empty( wp ) ) {
            page->top = (int)body[2] * WP42_HALF;
        }
        break;
    case 0xD0:                          /* the paper: lines, and lines of text */
        if ( wp_body_empty( wp ) && body[3] >= 6 ) {
            page->height = (int)body[3] * 240;
            one = page->height - page->top - (int)body[4] * WP42_HALF;
            page->bottom = one > 0 ? one : 0;
        }
        break;
    case 0xD1:
        if ( size >= 11 && (body[size - 2] >> 2) != 0 ) {
            wp_hdrftr( wp, body[size - 2] & 3, pos + 7, pos + size - 4 );
        }
        break;
    case 0xD2:
        if ( size >= 7 ) {
            wp_note( wp, NOTE_FOOT, pos + 6, pos + size - 1 );
        }
        break;
    case 0xDA:
        wp->dunder = (uint8_t)(body[2] & 1);
        break;
    case 0xE0:
        if ( body[1] == 0 ) {
            wp->ind_left += (int)body[2] * WP42_COL;
            wp->ind_right = wp->ind_left;
        }
        break;
    case 0xE1:
        wp42_char( wp, cp_to_uni( CP_437, body[1] ) );
        break;
    case 0xE2:                          /* a note: its text is after FFh and two margins */
        for ( at = 6; at + 3 < size && at < 6 + 300; at++ ) {
            if ( body[at] == 0xFF ) {
                wp_note( wp, (body[1] & 2) ? NOTE_END : NOTE_FOOT, pos + at + 3, pos + size - 1 );
                break;
            }
        }
        break;
    case 0xEB:
        wp_char( wp, WCH_DATE );
        break;
    case 0xF1:
        wp42_tabs( wp, body + 53 );
        break;
    default:
        break;
    }
}

static void wp42_single( WP *wp, uint32_t code )
{
    switch ( code ) {
    case 0x02:
        wp_char( wp, WCH_PAGENUM );
        break;
    case 0x09:
        wp_char( wp, WCH_TAB );
        break;
    case 0x0A:
    case 0x8C:
        wp_par( wp, 0 );
        break;
    case 0x0B:
    case 0x0D:
        wp_soft_space( wp );
        break;
    case 0x0C:
        wp_page( wp );
        break;
    case 0x81:
        wp->justify = AL_JUSTIFY;
        break;
    case 0x82:
        wp->justify = AL_LEFT;
        break;
    case 0x90:
    case 0x91:
        wp_attr( wp, WPA_REDLINE, code == 0x90 );
        break;
    case 0x92:
    case 0x93:
        wp_attr( wp, WPA_STRIKE, code == 0x92 );
        break;
    case 0x94:
        wp_attr( wp, wp->dunder ? WPA_DUNDER : WPA_UNDER, 1 );
        break;
    case 0x95:
        wp_attr( wp, WPA_DUNDER, 0 );
        wp_attr( wp, WPA_UNDER, 0 );
        break;
    case 0x9C:
    case 0x9D:
        wp_attr( wp, WPA_BOLD, code == 0x9D );
        break;
    case 0xA0:
        wp42_char( wp, WCH_NBSP );
        break;
    case 0xA9:
    case 0xAA:
    case 0xAB:
        wp42_char( wp, '-' );
        break;
    case 0xAC:
    case 0xAD:
    case 0xAE:
        wp_char( wp, WCH_SHY );
        break;
    case 0xB2:
    case 0xB3:
        wp_attr( wp, WPA_ITALIC, code == 0xB2 );
        break;
    case 0xB4:
    case 0xB5:
        wp_attr( wp, WPA_SHADOW, code == 0xB4 );
        break;
    case 0xB6:
    case 0xB7:
        wp_attr( wp, WPA_OUTLINE, code == 0xB6 );
        break;
    case 0xBC:
    case 0xBD:
        wp_attr( wp, code == 0xBC ? WPA_SUPER : WPA_SUB, 1 );
        wp->one_shot = 1;
        break;
    default:
        if ( code >= 0x20 && code < 0x7F ) {
            wp42_char( wp, code );
        }
        break;
    }
}

static void wp42_body( WP *wp, uint32_t pos, uint32_t end )
{
    const uint8_t *data = wp->data;
    uint32_t size;

    while ( pos < end ) {
        if ( data[pos] < 0xC0 ) {
            wp42_single( wp, data[pos++] );
            continue;
        }
        size = wp42_extent( data, pos, end, wp->dead );
        if ( size == 0 ) {
            pos++;                      /* not a whole function */
            continue;
        }
        wp42_fn( wp, pos, size );
        pos += size;
    }
}

/* Is it 4.2?  There is no signature: its functions, each closed by its
   own code at the right distance, and nothing in between that text
   never has, are all there is to go by. */
static int wp42_look( const uint8_t *data, uint32_t len, const char *ext )
{
    uint8_t      dead[64];
    uint32_t     look = len < 8192 ? len : 8192, pos = 0;
    unsigned int good = 0;
    uint32_t     size, code;
    int          sure;

    mem_set( dead, 0, sizeof( dead ) );
    while ( pos < look ) {
        code = data[pos];
        if ( code >= 0xC0 ) {
            size = wp42_extent( data, pos, look, dead );
            if ( size == 0 ) {
                if ( look < len && look - pos < 512 ) {
                    break;              /* cut by the end of the look, not the file */
                }
                return 0;
            }
            good++;
            pos += size;
        } else if ( code < 0x20 && (code < 0x09 || code > 0x0D) && code != 0x02 && code != 0x1A ) {
            return 0;
        } else {
            pos++;
        }
    }
    if ( good == 0 ) {
        return 0;
    }
    sure = good >= 3 ? 40 : 25;
    if ( str_cmp( ext, "WP" ) == 0 || str_cmp( ext, "WP4" ) == 0 || str_cmp( ext, "WPD" ) == 0 ||
         str_cmp( ext, "DOC" ) == 0 ) {
        sure += 10;
    }
    return sure;
}

/* ------------------------------------------------------------------ */
/* writing WordPerfect 5.1                                             */
/* ------------------------------------------------------------------ */
#define WPS_MAX_FONTS   255         /* a font's number is a byte */
#define WPS_MAX_SUB     60000UL     /* a header or note, in one function */
#define WPS_UNKNOWN     0x7FFFFFFFL

typedef struct {
    WDOC        *doc;
    OUT         *out;
    int          depth;
    uint32_t     attrs;         /* the attributes that are on in the file */
    int          font;          /* the font and size in force there, by the */
    uint32_t     size;          /* list's numbers; -1 for "not known" */
    uint32_t     color;
    int          left, right;   /* the indents the margins in force make */
    int          align;
    uint32_t     line;
    const WTAB  *tabs;          /* the tab stops last written */
    unsigned int ntabs;
    uint32_t     notes[2];
    unsigned int nfonts;
    uint16_t     fonts[WPS_MAX_FONTS];  /* the document's font for each in the list */
    uint16_t     sizes[WPS_MAX_FONTS];
} WPS;

static void wps_story( WPS *ws, STORY *story );

/* twips as WordPerfect units: rounded up, which gives back the unit a
   length read from a file was made from */
static uint32_t wps_wpu( int twips )
{
    if ( twips <= 0 ) {
        return 0;
    }
    if ( twips > 65000 ) {
        twips = 65000;
    }
    return ((uint32_t)twips * 5 + 5) / 6;
}

/* function, kind, length ... length, kind, function */
static uint32_t wps_open( OUT *out, int code, int sub )
{
    uint32_t at = out_tell( out );

    out_byte( out, code );
    out_byte( out, sub );
    out_le16( out, 0 );
    return at;
}

static void wps_close( OUT *out, uint32_t at, int code, int sub )
{
    uint32_t end = out_tell( out );

    out_seek( out, at + 2 );
    out_le16( out, end - at );
    out_seek( out, end );
    out_le16( out, end - at );
    out_byte( out, sub );
    out_byte( out, code );
}

static void wps_margins( WPS *ws, int left, int right )
{
    PAGESETUP *page = &ws->doc->page;
    uint32_t at = wps_open( ws->out, 0xD0, 1 );

    out_le16( ws->out, ws->left == WPS_UNKNOWN ? 0 : wps_wpu( page->left + ws->left ) );
    out_le16( ws->out, ws->right == WPS_UNKNOWN ? 0 : wps_wpu( page->right + ws->right ) );
    out_le16( ws->out, wps_wpu( page->left + left ) );
    out_le16( ws->out, wps_wpu( page->right + right ) );
    wps_close( ws->out, at, 0xD0, 1 );
    ws->left = left;
    ws->right = right;
}

static void wps_align( WPS *ws, int align )
{
    static const uint8_t code_of[4] = { 0, 2, 3, 1 }; /* AL_* as WordPerfect's */
    uint32_t at = wps_open( ws->out, 0xD0, 6 );

    out_byte( ws->out, ws->align >= 0 && ws->align < 4 ? code_of[ws->align] : 0 );
    out_byte( ws->out, code_of[align & 3] );
    wps_close( ws->out, at, 0xD0, 6 );
    ws->align = align;
}

static void wps_spacing( WPS *ws, uint32_t line )
{
    uint32_t at = wps_open( ws->out, 0xD0, 2 );

    out_le16( ws->out, ws->line * 256 / 240 );
    out_le16( ws->out, line * 256 / 240 );
    wps_close( ws->out, at, 0xD0, 2 );
    ws->line = line;
}

/* Tab Set: the old stops (left empty), forty places, their kinds two
   to a byte, and the left margin they are counted from */
static void wps_tabs( WPS *ws, const WTAB *tabs, unsigned int count )
{
    uint8_t      kinds[20];
    uint32_t     at = wps_open( ws->out, 0xD0, 4 );
    unsigned int index;
    uint32_t     kind;
    uint32_t     margin = wps_wpu( ws->doc->page.left + (ws->left == WPS_UNKNOWN ? 0 : ws->left) );

    if ( count > 40 ) {
        count = 40;
    }
    mem_set( kinds, 0, sizeof( kinds ) );
    out_fill( ws->out, 0xFF, 80 );
    out_fill( ws->out, 0, 20 );
    for ( index = 0; index < 40; index++ ) {
        if ( index < count ) {
            out_le16( ws->out, margin + wps_wpu( tabs[index].pos ) );
            kind = tabs[index].kind == TAB_CENTER ? 1 : tabs[index].kind == TAB_RIGHT ? 2 :
                   tabs[index].kind == TAB_DECIMAL ? 3 : 0;
            if ( tabs[index].lead != LEAD_NONE ) {
                kind |= 4;
            }
            kinds[index / 2] |= (uint8_t)((index & 1) ? kind : kind << 4);
        } else {
            out_le16( ws->out, 0xFFFF );
        }
    }
    out_put( ws->out, kinds, 20 );
    out_le16( ws->out, 0xFFFF );
    out_le16( ws->out, margin );
    wps_close( ws->out, at, 0xD0, 4 );
    ws->tabs = tabs;
    ws->ntabs = count;
}

/* the font and size of a run: Font Change names a font of the list */
static void wps_font( WPS *ws, const CHP *chp )
{
    uint32_t at;
    unsigned int index;
    uint32_t number = 0;

    for ( index = 0; index < ws->nfonts; index++ ) {
        if ( ws->fonts[index] == chp->font ) {
            number = index;
            break;
        }
    }
    if ( ws->font == (int)number && ws->size == chp->size ) {
        return;
    }
    at = wps_open( ws->out, 0xD1, 1 );
    out_byte( ws->out, 0 );
    out_le16( ws->out, (uint32_t)chp->size * 25 );
    out_fill( ws->out, 0, 22 );
    out_byte( ws->out, (int)number );
    out_le16( ws->out, 0 );
    out_le16( ws->out, (uint32_t)chp->size * 25 );
    out_byte( ws->out, 0 );
    wps_close( ws->out, at, 0xD1, 1 );
    ws->font = (int)number;
    ws->size = chp->size;
}

static void wps_color( WPS *ws, uint32_t color )
{
    uint32_t at;

    if ( color == COLOR_AUTO ) {
        color = 0;
    }
    if ( color == ws->color ) {
        return;
    }
    at = wps_open( ws->out, 0xD1, 0 );
    out_byte( ws->out, (int)((ws->color >> 16) & 0xFF) );
    out_byte( ws->out, (int)((ws->color >> 8) & 0xFF) );
    out_byte( ws->out, (int)(ws->color & 0xFF) );
    out_byte( ws->out, (int)((color >> 16) & 0xFF) );
    out_byte( ws->out, (int)((color >> 8) & 0xFF) );
    out_byte( ws->out, (int)(color & 0xFF) );
    wps_close( ws->out, at, 0xD1, 0 );
    ws->color = color;
}

/* the attributes: off the ones that end, on the ones that start */
static void wps_attrs( WPS *ws, uint32_t want )
{
    unsigned int index;

    for ( index = 0; index < 16; index++ ) {
        if ( (ws->attrs & (1UL << index)) && !(want & (1UL << index)) ) {
            out_byte( ws->out, 0xC4 );
            out_byte( ws->out, (int)index );
            out_byte( ws->out, 0xC4 );
        }
    }
    for ( index = 0; index < 16; index++ ) {
        if ( !(ws->attrs & (1UL << index)) && (want & (1UL << index)) ) {
            out_byte( ws->out, 0xC3 );
            out_byte( ws->out, (int)index );
            out_byte( ws->out, 0xC3 );
        }
    }
    ws->attrs = want;
}

static uint32_t wps_attrs_of( uint16_t flags )
{
    unsigned int index;
    uint32_t attrs = 0;

    for ( index = 0; index < 16; index++ ) {
        if ( wp_attr_flag[index] && (flags & wp_attr_flag[index]) ) {
            attrs |= 1UL << index;
        }
    }
    if ( flags & CF_WUNDER ) {
        attrs |= 1UL << WPA_UNDER;
    }
    return attrs;
}

/* Tab, and Margin Release to "pos" WordPerfect units from the paper's
   edge: function, kind, where it was, where it goes, the column */
static void wps_tab( OUT *out, int kind, uint32_t pos )
{
    out_byte( out, 0xC1 );
    out_byte( out, kind );
    out_le16( out, 0 );
    out_le16( out, pos );
    out_le16( out, 0 );
    out_byte( out, 0xC1 );
}

/* a character: ASCII as it is, the rest by its set and number */
static void wps_char( WPS *ws, uint32_t code )
{
    /* the typographic set before the others: its quotes and dashes
       are the ones every reader knows */
    static const uint8_t order[WP_NSETS - 1] = { 4, 1, 6, 8, 10, 3, 5, 7, 9, 11, 2 };
    char plain[8];
    uint32_t set, num;
    unsigned int index;
    int count;

    if ( code >= 0x20 && code < 0x7F ) {
        out_byte( ws->out, (int)code );
        return;
    }
    for ( index = 0; index < WP_NSETS - 1 && code <= 0xFFFF; index++ ) {
        set = order[index];
        for ( num = 0; num < wp5_sets[set].count; num++ ) {
            if ( wp5_sets[set].tab[num] == code && code != 0x20 ) {
                out_byte( ws->out, 0xC0 );
                out_byte( ws->out, (int)num );
                out_byte( ws->out, (int)set );
                out_byte( ws->out, 0xC0 );
                return;
            }
        }
    }
    count = uni_fallback( code, plain );
    if ( count <= 0 ) {
        plain[0] = '?';
        count = 1;
    }
    out_put( ws->out, plain, (uint32_t)count );
}

/* a header, a footer or a note: its text is made apart, because the
   function's length is sixteen bits and a story may not fit */
static void wps_sub( WPS *ws, STORY *story, int code, int sub, const uint8_t *head, uint32_t head_len )
{
    OUT *main_out = ws->out, *part;
    const WTAB *keep_tabs = ws->tabs;
    uint8_t *bytes;
    uint32_t len = 0, at, keep_attrs = ws->attrs, keep_size = ws->size;
    unsigned int keep_ntabs = ws->ntabs;
    uint32_t keep_line = ws->line;
    uint32_t keep_color = ws->color;
    int keep_left = ws->left, keep_right = ws->right;
    int keep_font = ws->font, keep_align = ws->align;

    if ( story == NULL || ws->depth != 0 ) {
        return;
    }
    part = out_mem();
    if ( part == NULL ) {
        out_fail( main_out, WE_NOMEM );
        return;
    }
    ws->out = part;
    ws->depth = 1;
    ws->attrs = 0;
    ws->font = -1;
    ws->left = ws->right = WPS_UNKNOWN;
    ws->align = -1;
    ws->line = 240;
    ws->tabs = NULL;
    ws->ntabs = 0;
    wps_story( ws, story );
    wps_attrs( ws, 0 );
    /* what was in force outside is in force again after it */
    ws->out = main_out;
    ws->depth = 0;
    ws->attrs = keep_attrs;
    ws->font = keep_font;
    ws->size = keep_size;
    ws->color = keep_color;
    ws->left = keep_left;
    ws->right = keep_right;
    ws->align = keep_align;
    ws->line = keep_line;
    ws->tabs = keep_tabs;
    ws->ntabs = keep_ntabs;
    if ( out_error( part ) ) {
        out_fail( main_out, out_error( part ) );
    }
    bytes = out_data( part, &len );
    if ( bytes != NULL && len <= WPS_MAX_SUB ) {
        at = wps_open( main_out, code, sub );
        out_put( main_out, head, head_len );
        out_put( main_out, bytes, len );
        wps_close( main_out, at, code, sub );
    }
    out_close( part );
}

static void wps_note( WPS *ws, const WOBJ *obj )
{
    uint8_t  head[16];
    int      end = obj->fmt == NOTE_END;
    uint32_t number = ++ws->notes[end], used = end ? 7 : 15;

    /* flags, the number; a footnote has a count of pages and their
       lines after it, an endnote four bytes.  The text starts with
       the code that shows the note's number, as WordPerfect's own
       notes do: a reader may take the first byte for it unseen. */
    mem_set( head, 0, sizeof( head ) );
    head[1] = (uint8_t)(number & 0xFF);
    head[2] = (uint8_t)((number >> 8) & 0xFF);
    head[used] = 0x8D;
    wps_sub( ws, obj->story, 0xD6, end, head, used + 1 );
}

static void wps_para( WPS *ws, WPARA *para, int last )
{
    WDOC *doc = ws->doc;
    const PAP *pap = &para->pap;
    const CHP *chp;
    unsigned int pos = 0, run, stop;
    uint32_t code, line;
    int obj, in_table = (pap->flags & PF_INTABLE) != 0;
    int left = pap->left, right = pap->right;

    if ( (pap->flags & PF_PAGEBEFORE) && ws->depth == 0 ) {
        out_byte( ws->out, 0x0C );
    }
    if ( !in_table ) {
        if ( left != ws->left || right != ws->right ) {
            wps_margins( ws, left, right );
        }
        if ( pap->align != ws->align ) {
            wps_align( ws, pap->align );
        }
        line = pap->line_rule == LS_MULTIPLE && pap->line >= 60 ? pap->line : 240;
        if ( line != ws->line ) {
            wps_spacing( ws, line );
        }
        if ( pap->ntabs != ws->ntabs ||
             (pap->ntabs && mem_cmp( pap->tabs, ws->tabs, pap->ntabs * sizeof( WTAB ) ) != 0) ) {
            if ( pap->ntabs ) {
                wps_tabs( ws, pap->tabs, pap->ntabs );
            }
        }
        if ( pap->first < 0 ) {
            wps_tab( ws->out, 0x80, wps_wpu( doc->page.left + left + pap->first ) );
        } else if ( pap->first > 0 && para->len ) {
            wps_tab( ws->out, 0, wps_wpu( doc->page.left + left + pap->first ) );
        }
    }
    while ( pos < para->len ) {
        run = wd_run( para, pos );
        if ( run == 0 ) {
            break;
        }
        stop = pos + run;
        chp = wd_chp_get( doc, para->attr[pos] );
        if ( chp->flags & CF_HIDDEN ) {
            pos = stop;
            continue;
        }
        wps_font( ws, chp );
        wps_color( ws, chp->color );
        wps_attrs( ws, wps_attrs_of( chp->flags ) );
        while ( pos < stop ) {
            code = utf16_get( para->text, stop, &pos );
            switch ( code ) {
            case WCH_TAB:
                wps_tab( ws->out, 0, 0 );
                break;
            case WCH_LINE:
                out_byte( ws->out, 0x0A );
                break;
            case WCH_PAGE:
                out_byte( ws->out, 0x0C );
                break;
            case WCH_NBSP:
                out_byte( ws->out, 0xA0 );
                break;
            case WCH_SHY:
                out_byte( ws->out, 0xAC );
                break;
            case WCH_NBHYPHEN:
                out_byte( ws->out, '-' );
                break;
            case '-':
                out_byte( ws->out, 0xA9 );   /* the hyphen a line may break at */
                break;
            case WCH_PAGENUM:
                out_byte( ws->out, 0x02 );
                break;
            case WCH_OBJ:
                obj = wd_obj_at( para, pos - 1 );
                if ( obj >= 0 && (unsigned int)obj < doc->nobj && doc->obj[obj].kind == OBJ_NOTE ) {
                    wps_note( ws, &doc->obj[obj] );
                }
                break;
            default:
                if ( !WCH_IS_FIELD( code ) ) {
                    wps_char( ws, code );
                }
                break;
            }
        }
    }
    /* a table is written as its text: a tab between cells, a line for
       each row */
    if ( in_table && !(pap->flags & PF_ROWEND) ) {
        if ( pap->flags & PF_CELLEND ) {
            wps_tab( ws->out, 0, 0 );
        } else {
            out_byte( ws->out, ' ' );
        }
    } else if ( !last ) {
        out_byte( ws->out, 0x0A );
    }
}

static void wps_story( WPS *ws, STORY *story )
{
    unsigned int index;

    for ( index = 0; index < story->count; index++ ) {
        /* the last paragraph of a header or a note has no Hard Return
           after it: there it would be an empty line */
        wps_para( ws, story->para[index], ws->depth != 0 && index + 1 == story->count );
    }
}

static int wp_save( WDOC *doc, OUT *out, int variant )
{
    WPS         *ws;
    uint8_t      head[18];
    unsigned int index, slot;
    uint32_t     at, pool = 0, name_off;

    (void)variant;
    ws = (WPS *)try_alloc( sizeof( WPS ) );
    if ( ws == NULL ) {
        return WE_NOMEM;
    }
    mem_set( ws, 0, sizeof( WPS ) );
    ws->doc = doc;
    ws->out = out;

    /* the fonts: the first is the one the document starts in */
    for ( index = 0; index < doc->nchp; index++ ) {
        for ( slot = 0; slot < ws->nfonts; slot++ ) {
            if ( ws->fonts[slot] == doc->chp[index].font ) {
                break;
            }
        }
        if ( slot == ws->nfonts && ws->nfonts < WPS_MAX_FONTS && doc->chp[index].font < doc->nfont ) {
            ws->fonts[ws->nfonts] = doc->chp[index].font;
            ws->sizes[ws->nfonts] = doc->chp[index].size;
            ws->nfonts++;
        }
    }
    for ( index = 0; index < ws->nfonts; index++ ) {
        pool += str_len( doc->font[ws->fonts[index]].name ) + 1;
    }

    /* the prefix: one index block that names the two packets */
    out_put( out, "\xFFWPC", 4 );
    out_le32( out, 66 + pool + ws->nfonts * 86 );
    out_byte( out, 1 );
    out_byte( out, 0x0A );
    out_byte( out, 0 );
    out_byte( out, 1 );
    out_le32( out, 0 );
    out_le16( out, 0xFFFB );
    out_le16( out, 5 );
    out_le16( out, 50 );
    out_le32( out, 0 );
    out_le16( out, ws->nfonts ? 7 : 0 );
    out_le32( out, ws->nfonts ? pool : 0 );
    out_le32( out, ws->nfonts ? 66 : 0 );
    out_le16( out, ws->nfonts ? 15 : 0 );
    out_le32( out, ws->nfonts * 86 );
    out_le32( out, ws->nfonts ? 66 + pool : 0 );
    out_fill( out, 0, 20 );
    for ( index = 0; index < ws->nfonts; index++ ) {
        out_put( out, doc->font[ws->fonts[index]].name, str_len( doc->font[ws->fonts[index]].name ) + 1 );
    }
    name_off = 0;
    for ( index = 0; index < ws->nfonts; index++ ) {
        out_fill( out, 0, 18 );
        out_le16( out, name_off );
        out_fill( out, 0, 3 );
        out_le16( out, (uint32_t)ws->sizes[index] * 25 );
        out_fill( out, 0, 22 );
        out_le16( out, (uint32_t)ws->sizes[index] * 25 );
        out_fill( out, 0, 37 );
        name_off += str_len( doc->font[ws->fonts[index]].name ) + 1;
    }

    /* the text: the page's margins first, then the header and footer */
    ws->font = 0;
    ws->size = ws->nfonts ? ws->sizes[0] : 24;
    ws->left = ws->right = WPS_UNKNOWN;
    ws->align = -1;
    ws->line = 240;
    wps_margins( ws, 0, 0 );
    at = wps_open( out, 0xD0, 5 );
    out_le32( out, 0 );
    out_le16( out, wps_wpu( doc->page.top ) );
    out_le16( out, wps_wpu( doc->page.bottom ) );
    wps_close( out, at, 0xD0, 5 );
    mem_set( head, 0, sizeof( head ) );
    head[7] = 1;                        /* on every page */
    if ( doc->header.count ) {
        wps_sub( ws, &doc->header, 0xD5, 0, head, 18 );
    }
    if ( doc->footer.count ) {
        wps_sub( ws, &doc->footer, 0xD5, 2, head, 18 );
    }
    /* the font is said before the first line, so that empty lines at
       the top are in it too */
    ws->font = -1;
    wps_font( ws, &doc->style[0].chp );
    wps_story( ws, &doc->body );
    wps_attrs( ws, 0 );
    xfree( ws );
    return out_error( out ) == WE_NOMEM ? WE_NOMEM : out_error( out ) ? WE_IO : WE_OK;
}

/* ------------------------------------------------------------------ */
/* the format                                                          */
/* ------------------------------------------------------------------ */
static int wp_signed( const uint8_t *data, uint32_t len )
{
    return len >= 16 && data[0] == 0xFF && data[1] == 'W' && data[2] == 'P' && data[3] == 'C';
}

static int wp_detect( const uint8_t *data, uint32_t len, const char *ext )
{
    if ( wp_signed( data, len ) ) {
        return (data[8] == 1 && data[9] == 0x0A) ? 100 : 0;
    }
    return wp42_look( data, len, ext );
}

static int wp_load( const uint8_t *data, uint32_t len, WDOC *doc )
{
    WP wp;
    uint32_t start = 0;

    mem_set( &wp, 0, sizeof( wp ) );
    wp.data = data;
    wp.len = len;
    wp.doc = doc;
    wp.version = 42;
    if ( wp_signed( data, len ) ) {
        if ( data[8] != 1 || data[9] != 0x0A ) {
            return WE_FORMAT;
        }
        if ( LE16( data + 12 ) != 0 ) {
            return WE_PASSWORD;
        }
        start = LE32( data + 4 );
        if ( start < 16 || start > len ) {
            return WE_FORMAT;
        }
        if ( data[10] == 0 ) {
            wp.version = data[11] == 0 ? 50 : 51;
        } else if ( data[10] == 2 ) {
            wp.version = 60;
        } else {
            return WE_UNSUPPORTED;
        }
    } else if ( wp42_look( data, len, "WP" ) == 0 ) {
        return WE_FORMAT;
    }
    doc->variant = 0;

    /* WordPerfect's own page: an inch of margin all round */
    doc->page.left = doc->page.right = doc->page.top = doc->page.bottom = 1440;
    wp.page_left = wp.page_right = wp.mar_left = wp.mar_right = 1440;
    wp.justify = AL_LEFT;

    wb_init( &wp.wb, doc, &doc->body );
    wp.base_size = wp.wb.chp.size ? wp.wb.chp.size : 24;
    if ( wp.version >= 60 ) {
        wp6_prefix( &wp );
    } else if ( wp.version >= 50 ) {
        wp.justify = AL_JUSTIFY;        /* 5.x starts with full justification */
        wp5_prefix( &wp );
        if ( wp.nfonts ) {
            wp5_font( &wp, 0, 0 );
        }
    } else {
        /* 4.2: columns 10 to 74 of a 10-pitch printer, justified */
        wp.wb.chp.font = (uint16_t)wd_font( doc, "Courier New", FAM_MODERN, 1 );
        wp.justify = AL_JUSTIFY;
        doc->page.right = wp.page_right = wp.mar_right = doc->page.width - 74 * WP42_COL;
    }
    wp_looks( &wp );
    doc->style[0].chp = wp.wb.chp;

    if ( wp.version >= 60 ) {
        wp6_body( &wp, start, len );
    } else if ( wp.version >= 50 ) {
        wp5_body( &wp, start, len );
    } else {
        wp42_body( &wp, start, len );
    }
    if ( wp.in_cell ) {
        wp_cell_end( &wp, 1 );
    }
    if ( wp.wb.para != NULL ) {
        wp_par( &wp, 0 );
    }
    wb_done( &wp.wb );
    return wd_failed( doc ) ? WE_NOMEM : WE_OK;
}

const FORMAT fmt_wordperfect = {
    FMT_WORDPERFECT, "WordPerfect", "WPD;WP;WP5;WP6;DOC", wp_detect, wp_load, wp_save, wp_variants, 0
};
