/* ==========================================================================
 * AES.C - AES-128/256 (FIPS-197) + CBC and counter mode (SP800-38A).
 * Portable C89.
 * ==========================================================================
 * The schedule (rk) is bytes in FIPS-197 order, so callers can keep it in a
 * struct of bytes.  The rounds work on 32-bit column words instead: a
 * column's row 0 is the word's low byte.  One 1K table each way does
 * SubBytes and MixColumns (or their inverses) together, and the other rows
 * are that table's entry rotated.  The schedule becomes words once per call,
 * so the CBC and counter-mode calls, one per record, pay that only once.
 *
 * Decryption uses FIPS-197's equivalent inverse cipher (5.3.5): the middle
 * round keys go through InvMixColumns first, also once per call.
 * ========================================================================== */
#include "aes.h"
#include <string.h>

static const unsigned char sbox[256] = {
0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const unsigned char inv_sbox[256] = {
0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static const unsigned char rcon[11] = {
0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36
};

typedef unsigned int u32;           /* 32-bit on all Win32 targets */

#define AES_MAX_WORDS   60          /* 15 round keys of 4 words (AES-256) */

#define ROT8( x )   ( ( (x) << 8 ) | ( (x) >> 24 ) )
#define ROT16( x )  ( ( (x) << 16 ) | ( (x) >> 16 ) )
#define ROT24( x )  ( ( (x) << 24 ) | ( (x) >> 8 ) )
#define ROW0( x )   ( (x) & 0xff )
#define ROW1( x )   ( ( (x) >> 8 ) & 0xff )
#define ROW2( x )   ( ( (x) >> 16 ) & 0xff )
#define ROW3( x )   ( (x) >> 24 )

/* te[x]: SubBytes then MixColumns of x in row 0 - rows 0..3 get 2s, s, s, 3s */
static const u32 te[256] = {
0xa56363c6UL,0x847c7cf8UL,0x997777eeUL,0x8d7b7bf6UL,
0x0df2f2ffUL,0xbd6b6bd6UL,0xb16f6fdeUL,0x54c5c591UL,
0x50303060UL,0x03010102UL,0xa96767ceUL,0x7d2b2b56UL,
0x19fefee7UL,0x62d7d7b5UL,0xe6abab4dUL,0x9a7676ecUL,
0x45caca8fUL,0x9d82821fUL,0x40c9c989UL,0x877d7dfaUL,
0x15fafaefUL,0xeb5959b2UL,0xc947478eUL,0x0bf0f0fbUL,
0xecadad41UL,0x67d4d4b3UL,0xfda2a25fUL,0xeaafaf45UL,
0xbf9c9c23UL,0xf7a4a453UL,0x967272e4UL,0x5bc0c09bUL,
0xc2b7b775UL,0x1cfdfde1UL,0xae93933dUL,0x6a26264cUL,
0x5a36366cUL,0x413f3f7eUL,0x02f7f7f5UL,0x4fcccc83UL,
0x5c343468UL,0xf4a5a551UL,0x34e5e5d1UL,0x08f1f1f9UL,
0x937171e2UL,0x73d8d8abUL,0x53313162UL,0x3f15152aUL,
0x0c040408UL,0x52c7c795UL,0x65232346UL,0x5ec3c39dUL,
0x28181830UL,0xa1969637UL,0x0f05050aUL,0xb59a9a2fUL,
0x0907070eUL,0x36121224UL,0x9b80801bUL,0x3de2e2dfUL,
0x26ebebcdUL,0x6927274eUL,0xcdb2b27fUL,0x9f7575eaUL,
0x1b090912UL,0x9e83831dUL,0x742c2c58UL,0x2e1a1a34UL,
0x2d1b1b36UL,0xb26e6edcUL,0xee5a5ab4UL,0xfba0a05bUL,
0xf65252a4UL,0x4d3b3b76UL,0x61d6d6b7UL,0xceb3b37dUL,
0x7b292952UL,0x3ee3e3ddUL,0x712f2f5eUL,0x97848413UL,
0xf55353a6UL,0x68d1d1b9UL,0x00000000UL,0x2cededc1UL,
0x60202040UL,0x1ffcfce3UL,0xc8b1b179UL,0xed5b5bb6UL,
0xbe6a6ad4UL,0x46cbcb8dUL,0xd9bebe67UL,0x4b393972UL,
0xde4a4a94UL,0xd44c4c98UL,0xe85858b0UL,0x4acfcf85UL,
0x6bd0d0bbUL,0x2aefefc5UL,0xe5aaaa4fUL,0x16fbfbedUL,
0xc5434386UL,0xd74d4d9aUL,0x55333366UL,0x94858511UL,
0xcf45458aUL,0x10f9f9e9UL,0x06020204UL,0x817f7ffeUL,
0xf05050a0UL,0x443c3c78UL,0xba9f9f25UL,0xe3a8a84bUL,
0xf35151a2UL,0xfea3a35dUL,0xc0404080UL,0x8a8f8f05UL,
0xad92923fUL,0xbc9d9d21UL,0x48383870UL,0x04f5f5f1UL,
0xdfbcbc63UL,0xc1b6b677UL,0x75dadaafUL,0x63212142UL,
0x30101020UL,0x1affffe5UL,0x0ef3f3fdUL,0x6dd2d2bfUL,
0x4ccdcd81UL,0x140c0c18UL,0x35131326UL,0x2fececc3UL,
0xe15f5fbeUL,0xa2979735UL,0xcc444488UL,0x3917172eUL,
0x57c4c493UL,0xf2a7a755UL,0x827e7efcUL,0x473d3d7aUL,
0xac6464c8UL,0xe75d5dbaUL,0x2b191932UL,0x957373e6UL,
0xa06060c0UL,0x98818119UL,0xd14f4f9eUL,0x7fdcdca3UL,
0x66222244UL,0x7e2a2a54UL,0xab90903bUL,0x8388880bUL,
0xca46468cUL,0x29eeeec7UL,0xd3b8b86bUL,0x3c141428UL,
0x79dedea7UL,0xe25e5ebcUL,0x1d0b0b16UL,0x76dbdbadUL,
0x3be0e0dbUL,0x56323264UL,0x4e3a3a74UL,0x1e0a0a14UL,
0xdb494992UL,0x0a06060cUL,0x6c242448UL,0xe45c5cb8UL,
0x5dc2c29fUL,0x6ed3d3bdUL,0xefacac43UL,0xa66262c4UL,
0xa8919139UL,0xa4959531UL,0x37e4e4d3UL,0x8b7979f2UL,
0x32e7e7d5UL,0x43c8c88bUL,0x5937376eUL,0xb76d6ddaUL,
0x8c8d8d01UL,0x64d5d5b1UL,0xd24e4e9cUL,0xe0a9a949UL,
0xb46c6cd8UL,0xfa5656acUL,0x07f4f4f3UL,0x25eaeacfUL,
0xaf6565caUL,0x8e7a7af4UL,0xe9aeae47UL,0x18080810UL,
0xd5baba6fUL,0x887878f0UL,0x6f25254aUL,0x722e2e5cUL,
0x241c1c38UL,0xf1a6a657UL,0xc7b4b473UL,0x51c6c697UL,
0x23e8e8cbUL,0x7cdddda1UL,0x9c7474e8UL,0x211f1f3eUL,
0xdd4b4b96UL,0xdcbdbd61UL,0x868b8b0dUL,0x858a8a0fUL,
0x907070e0UL,0x423e3e7cUL,0xc4b5b571UL,0xaa6666ccUL,
0xd8484890UL,0x05030306UL,0x01f6f6f7UL,0x120e0e1cUL,
0xa36161c2UL,0x5f35356aUL,0xf95757aeUL,0xd0b9b969UL,
0x91868617UL,0x58c1c199UL,0x271d1d3aUL,0xb99e9e27UL,
0x38e1e1d9UL,0x13f8f8ebUL,0xb398982bUL,0x33111122UL,
0xbb6969d2UL,0x70d9d9a9UL,0x898e8e07UL,0xa7949433UL,
0xb69b9b2dUL,0x221e1e3cUL,0x92878715UL,0x20e9e9c9UL,
0x49cece87UL,0xff5555aaUL,0x78282850UL,0x7adfdfa5UL,
0x8f8c8c03UL,0xf8a1a159UL,0x80898909UL,0x170d0d1aUL,
0xdabfbf65UL,0x31e6e6d7UL,0xc6424284UL,0xb86868d0UL,
0xc3414182UL,0xb0999929UL,0x772d2d5aUL,0x110f0f1eUL,
0xcbb0b07bUL,0xfc5454a8UL,0xd6bbbb6dUL,0x3a16162cUL
};

/* td[x]: InvSubBytes then InvMixColumns of x in row 0 - rows 0..3 get 14s, 9s, 13s, 11s */
static const u32 td[256] = {
0x50a7f451UL,0x5365417eUL,0xc3a4171aUL,0x965e273aUL,
0xcb6bab3bUL,0xf1459d1fUL,0xab58faacUL,0x9303e34bUL,
0x55fa3020UL,0xf66d76adUL,0x9176cc88UL,0x254c02f5UL,
0xfcd7e54fUL,0xd7cb2ac5UL,0x80443526UL,0x8fa362b5UL,
0x495ab1deUL,0x671bba25UL,0x980eea45UL,0xe1c0fe5dUL,
0x02752fc3UL,0x12f04c81UL,0xa397468dUL,0xc6f9d36bUL,
0xe75f8f03UL,0x959c9215UL,0xeb7a6dbfUL,0xda595295UL,
0x2d83bed4UL,0xd3217458UL,0x2969e049UL,0x44c8c98eUL,
0x6a89c275UL,0x78798ef4UL,0x6b3e5899UL,0xdd71b927UL,
0xb64fe1beUL,0x17ad88f0UL,0x66ac20c9UL,0xb43ace7dUL,
0x184adf63UL,0x82311ae5UL,0x60335197UL,0x457f5362UL,
0xe07764b1UL,0x84ae6bbbUL,0x1ca081feUL,0x942b08f9UL,
0x58684870UL,0x19fd458fUL,0x876cde94UL,0xb7f87b52UL,
0x23d373abUL,0xe2024b72UL,0x578f1fe3UL,0x2aab5566UL,
0x0728ebb2UL,0x03c2b52fUL,0x9a7bc586UL,0xa50837d3UL,
0xf2872830UL,0xb2a5bf23UL,0xba6a0302UL,0x5c8216edUL,
0x2b1ccf8aUL,0x92b479a7UL,0xf0f207f3UL,0xa1e2694eUL,
0xcdf4da65UL,0xd5be0506UL,0x1f6234d1UL,0x8afea6c4UL,
0x9d532e34UL,0xa055f3a2UL,0x32e18a05UL,0x75ebf6a4UL,
0x39ec830bUL,0xaaef6040UL,0x069f715eUL,0x51106ebdUL,
0xf98a213eUL,0x3d06dd96UL,0xae053eddUL,0x46bde64dUL,
0xb58d5491UL,0x055dc471UL,0x6fd40604UL,0xff155060UL,
0x24fb9819UL,0x97e9bdd6UL,0xcc434089UL,0x779ed967UL,
0xbd42e8b0UL,0x888b8907UL,0x385b19e7UL,0xdbeec879UL,
0x470a7ca1UL,0xe90f427cUL,0xc91e84f8UL,0x00000000UL,
0x83868009UL,0x48ed2b32UL,0xac70111eUL,0x4e725a6cUL,
0xfbff0efdUL,0x5638850fUL,0x1ed5ae3dUL,0x27392d36UL,
0x64d90f0aUL,0x21a65c68UL,0xd1545b9bUL,0x3a2e3624UL,
0xb1670a0cUL,0x0fe75793UL,0xd296eeb4UL,0x9e919b1bUL,
0x4fc5c080UL,0xa220dc61UL,0x694b775aUL,0x161a121cUL,
0x0aba93e2UL,0xe52aa0c0UL,0x43e0223cUL,0x1d171b12UL,
0x0b0d090eUL,0xadc78bf2UL,0xb9a8b62dUL,0xc8a91e14UL,
0x8519f157UL,0x4c0775afUL,0xbbdd99eeUL,0xfd607fa3UL,
0x9f2601f7UL,0xbcf5725cUL,0xc53b6644UL,0x347efb5bUL,
0x7629438bUL,0xdcc623cbUL,0x68fcedb6UL,0x63f1e4b8UL,
0xcadc31d7UL,0x10856342UL,0x40229713UL,0x2011c684UL,
0x7d244a85UL,0xf83dbbd2UL,0x1132f9aeUL,0x6da129c7UL,
0x4b2f9e1dUL,0xf330b2dcUL,0xec52860dUL,0xd0e3c177UL,
0x6c16b32bUL,0x99b970a9UL,0xfa489411UL,0x2264e947UL,
0xc48cfca8UL,0x1a3ff0a0UL,0xd82c7d56UL,0xef903322UL,
0xc74e4987UL,0xc1d138d9UL,0xfea2ca8cUL,0x360bd498UL,
0xcf81f5a6UL,0x28de7aa5UL,0x268eb7daUL,0xa4bfad3fUL,
0xe49d3a2cUL,0x0d927850UL,0x9bcc5f6aUL,0x62467e54UL,
0xc2138df6UL,0xe8b8d890UL,0x5ef7392eUL,0xf5afc382UL,
0xbe805d9fUL,0x7c93d069UL,0xa92dd56fUL,0xb31225cfUL,
0x3b99acc8UL,0xa77d1810UL,0x6e639ce8UL,0x7bbb3bdbUL,
0x097826cdUL,0xf418596eUL,0x01b79aecUL,0xa89a4f83UL,
0x656e95e6UL,0x7ee6ffaaUL,0x08cfbc21UL,0xe6e815efUL,
0xd99be7baUL,0xce366f4aUL,0xd4099feaUL,0xd67cb029UL,
0xafb2a431UL,0x31233f2aUL,0x3094a5c6UL,0xc066a235UL,
0x37bc4e74UL,0xa6ca82fcUL,0xb0d090e0UL,0x15d8a733UL,
0x4a9804f1UL,0xf7daec41UL,0x0e50cd7fUL,0x2ff69117UL,
0x8dd64d76UL,0x4db0ef43UL,0x544daaccUL,0xdf0496e4UL,
0xe3b5d19eUL,0x1b886a4cUL,0xb81f2cc1UL,0x7f516546UL,
0x04ea5e9dUL,0x5d358c01UL,0x737487faUL,0x2e410bfbUL,
0x5a1d67b3UL,0x52d2db92UL,0x335610e9UL,0x1347d66dUL,
0x8c61d79aUL,0x7a0ca137UL,0x8e14f859UL,0x893c13ebUL,
0xee27a9ceUL,0x35c961b7UL,0xede51ce1UL,0x3cb1477aUL,
0x59dfd29cUL,0x3f73f255UL,0x79ce1418UL,0xbf37c773UL,
0xeacdf753UL,0x5baafd5fUL,0x146f3ddfUL,0x86db4478UL,
0x81f3afcaUL,0x3ec468b9UL,0x2c342438UL,0x5f40a3c2UL,
0x72c31d16UL,0x0c25e2bcUL,0x8b493c28UL,0x41950dffUL,
0x7101a839UL,0xdeb30c08UL,0x9ce4b4d8UL,0x90c15664UL,
0x6184cb7bUL,0x70b632d5UL,0x745c6c48UL,0x4257b8d0UL
};

void aes128_expand_key(const unsigned char key[16], unsigned char rk[176])
{
    int i;
    unsigned char t0, t1, t2, t3, tmp;

    memcpy(rk, key, 16);
    for (i = 4; i < 44; i++) {
        t0 = rk[(i-1)*4 + 0];
        t1 = rk[(i-1)*4 + 1];
        t2 = rk[(i-1)*4 + 2];
        t3 = rk[(i-1)*4 + 3];
        if ((i & 3) == 0) {                     /* RotWord + SubWord + Rcon */
            tmp = t0;
            t0 = (unsigned char)(sbox[t1] ^ rcon[i/4]);
            t1 = sbox[t2];
            t2 = sbox[t3];
            t3 = sbox[tmp];
        }
        rk[i*4 + 0] = (unsigned char)(rk[(i-4)*4 + 0] ^ t0);
        rk[i*4 + 1] = (unsigned char)(rk[(i-4)*4 + 1] ^ t1);
        rk[i*4 + 2] = (unsigned char)(rk[(i-4)*4 + 2] ^ t2);
        rk[i*4 + 3] = (unsigned char)(rk[(i-4)*4 + 3] ^ t3);
    }
}

void aes256_expand_key(const unsigned char key[32], unsigned char rk[240])
{
    int i;
    unsigned char t0, t1, t2, t3, tmp;

    memcpy(rk, key, 32);                         /* Nk = 8 words */
    for (i = 8; i < 60; i++) {                   /* 60 words = 15 round keys */
        t0 = rk[(i-1)*4 + 0];
        t1 = rk[(i-1)*4 + 1];
        t2 = rk[(i-1)*4 + 2];
        t3 = rk[(i-1)*4 + 3];
        if ((i & 7) == 0) {                     /* RotWord + SubWord + Rcon */
            tmp = t0;
            t0 = (unsigned char)(sbox[t1] ^ rcon[i/8]);
            t1 = sbox[t2];
            t2 = sbox[t3];
            t3 = sbox[tmp];
        } else if ((i & 7) == 4) {              /* SubWord only (AES-256 extra) */
            t0 = sbox[t0]; t1 = sbox[t1]; t2 = sbox[t2]; t3 = sbox[t3];
        }
        rk[i*4 + 0] = (unsigned char)(rk[(i-8)*4 + 0] ^ t0);
        rk[i*4 + 1] = (unsigned char)(rk[(i-8)*4 + 1] ^ t1);
        rk[i*4 + 2] = (unsigned char)(rk[(i-8)*4 + 2] ^ t2);
        rk[i*4 + 3] = (unsigned char)(rk[(i-8)*4 + 3] ^ t3);
    }
}

static u32 load_col( const unsigned char *p )
{
    return (u32)p[0] | ( (u32)p[1] << 8 ) | ( (u32)p[2] << 16 ) | ( (u32)p[3] << 24 );
}

static void store_col( unsigned char *p, u32 col )
{
    p[0] = (unsigned char)col;
    p[1] = (unsigned char)( col >> 8 );
    p[2] = (unsigned char)( col >> 16 );
    p[3] = (unsigned char)( col >> 24 );
}

/* The byte schedule as words, round 0 first: 4 * (nr + 1) of them. */
static void enc_words( const unsigned char *rk, int nr, u32 *kw )
{
    int i;
    for ( i = 0; i < 4 * ( nr + 1 ); i++ ) {
        kw[i] = load_col( rk + 4 * i );
    }
}

/* The decryption schedule, in the order dec_block uses it: round nr, then
   rounds nr-1 down to 1 through InvMixColumns, then round 0.  td[sbox[b]]
   is InvMixColumns of b alone in row 0, since the two S-boxes cancel. */
static void dec_words( const unsigned char *rk, int nr, u32 *kw )
{
    int round, col;
    u32 word;
    for ( col = 0; col < 4; col++ ) {
        kw[col] = load_col( rk + 16 * nr + 4 * col );
        kw[4 * nr + col] = load_col( rk + 4 * col );
    }
    for ( round = 1; round < nr; round++ ) {
        for ( col = 0; col < 4; col++ ) {
            word = load_col( rk + 16 * ( nr - round ) + 4 * col );
            kw[4 * round + col] = td[sbox[ROW0( word )]] ^ ROT8( td[sbox[ROW1( word )]] ) ^
                                  ROT16( td[sbox[ROW2( word )]] ) ^ ROT24( td[sbox[ROW3( word )]] );
        }
    }
}

/* ShiftRows takes row r of column c from column c + r. */
static void enc_block( const u32 *kw, int nr, const unsigned char in[16], unsigned char out[16] )
{
    u32 t0, t1, t2, t3, u0, u1, u2, u3;
    int round;

    t0 = load_col( in ) ^ kw[0];
    t1 = load_col( in + 4 ) ^ kw[1];
    t2 = load_col( in + 8 ) ^ kw[2];
    t3 = load_col( in + 12 ) ^ kw[3];
    for ( round = 1; round < nr; round++ ) {
        kw += 4;
        u0 = te[ROW0( t0 )] ^ ROT8( te[ROW1( t1 )] ) ^ ROT16( te[ROW2( t2 )] ) ^ ROT24( te[ROW3( t3 )] ) ^ kw[0];
        u1 = te[ROW0( t1 )] ^ ROT8( te[ROW1( t2 )] ) ^ ROT16( te[ROW2( t3 )] ) ^ ROT24( te[ROW3( t0 )] ) ^ kw[1];
        u2 = te[ROW0( t2 )] ^ ROT8( te[ROW1( t3 )] ) ^ ROT16( te[ROW2( t0 )] ) ^ ROT24( te[ROW3( t1 )] ) ^ kw[2];
        u3 = te[ROW0( t3 )] ^ ROT8( te[ROW1( t0 )] ) ^ ROT16( te[ROW2( t1 )] ) ^ ROT24( te[ROW3( t2 )] ) ^ kw[3];
        t0 = u0; t1 = u1; t2 = u2; t3 = u3;
    }
    kw += 4;                                    /* last round: no MixColumns */
    u0 = (u32)sbox[ROW0( t0 )] | ( (u32)sbox[ROW1( t1 )] << 8 ) | ( (u32)sbox[ROW2( t2 )] << 16 ) | ( (u32)sbox[ROW3( t3 )] << 24 );
    u1 = (u32)sbox[ROW0( t1 )] | ( (u32)sbox[ROW1( t2 )] << 8 ) | ( (u32)sbox[ROW2( t3 )] << 16 ) | ( (u32)sbox[ROW3( t0 )] << 24 );
    u2 = (u32)sbox[ROW0( t2 )] | ( (u32)sbox[ROW1( t3 )] << 8 ) | ( (u32)sbox[ROW2( t0 )] << 16 ) | ( (u32)sbox[ROW3( t1 )] << 24 );
    u3 = (u32)sbox[ROW0( t3 )] | ( (u32)sbox[ROW1( t0 )] << 8 ) | ( (u32)sbox[ROW2( t1 )] << 16 ) | ( (u32)sbox[ROW3( t2 )] << 24 );
    store_col( out, u0 ^ kw[0] );
    store_col( out + 4, u1 ^ kw[1] );
    store_col( out + 8, u2 ^ kw[2] );
    store_col( out + 12, u3 ^ kw[3] );
}

/* InvShiftRows takes row r of column c from column c - r. */
static void dec_block( const u32 *kw, int nr, const unsigned char in[16], unsigned char out[16] )
{
    u32 t0, t1, t2, t3, u0, u1, u2, u3;
    int round;

    t0 = load_col( in ) ^ kw[0];
    t1 = load_col( in + 4 ) ^ kw[1];
    t2 = load_col( in + 8 ) ^ kw[2];
    t3 = load_col( in + 12 ) ^ kw[3];
    for ( round = 1; round < nr; round++ ) {
        kw += 4;
        u0 = td[ROW0( t0 )] ^ ROT8( td[ROW1( t3 )] ) ^ ROT16( td[ROW2( t2 )] ) ^ ROT24( td[ROW3( t1 )] ) ^ kw[0];
        u1 = td[ROW0( t1 )] ^ ROT8( td[ROW1( t0 )] ) ^ ROT16( td[ROW2( t3 )] ) ^ ROT24( td[ROW3( t2 )] ) ^ kw[1];
        u2 = td[ROW0( t2 )] ^ ROT8( td[ROW1( t1 )] ) ^ ROT16( td[ROW2( t0 )] ) ^ ROT24( td[ROW3( t3 )] ) ^ kw[2];
        u3 = td[ROW0( t3 )] ^ ROT8( td[ROW1( t2 )] ) ^ ROT16( td[ROW2( t1 )] ) ^ ROT24( td[ROW3( t0 )] ) ^ kw[3];
        t0 = u0; t1 = u1; t2 = u2; t3 = u3;
    }
    kw += 4;                                    /* last round: no InvMixColumns */
    u0 = (u32)inv_sbox[ROW0( t0 )] | ( (u32)inv_sbox[ROW1( t3 )] << 8 ) | ( (u32)inv_sbox[ROW2( t2 )] << 16 ) | ( (u32)inv_sbox[ROW3( t1 )] << 24 );
    u1 = (u32)inv_sbox[ROW0( t1 )] | ( (u32)inv_sbox[ROW1( t0 )] << 8 ) | ( (u32)inv_sbox[ROW2( t3 )] << 16 ) | ( (u32)inv_sbox[ROW3( t2 )] << 24 );
    u2 = (u32)inv_sbox[ROW0( t2 )] | ( (u32)inv_sbox[ROW1( t1 )] << 8 ) | ( (u32)inv_sbox[ROW2( t0 )] << 16 ) | ( (u32)inv_sbox[ROW3( t3 )] << 24 );
    u3 = (u32)inv_sbox[ROW0( t3 )] | ( (u32)inv_sbox[ROW1( t2 )] << 8 ) | ( (u32)inv_sbox[ROW2( t1 )] << 16 ) | ( (u32)inv_sbox[ROW3( t0 )] << 24 );
    store_col( out, u0 ^ kw[0] );
    store_col( out + 4, u1 ^ kw[1] );
    store_col( out + 8, u2 ^ kw[2] );
    store_col( out + 12, u3 ^ kw[3] );
}

/* Key-size-generic single block: nr rounds over the matching schedule rk.
   The word copies of the schedule are key material: wiped before return. */
void aes_encrypt( const unsigned char *rk, int nr,
                  const unsigned char in[16], unsigned char out[16] )
{
    u32 kw[AES_MAX_WORDS];
    enc_words( rk, nr, kw );
    enc_block( kw, nr, in, out );
    memset( kw, 0, sizeof( kw ) );
}

void aes_decrypt( const unsigned char *rk, int nr,
                  const unsigned char in[16], unsigned char out[16] )
{
    u32 kw[AES_MAX_WORDS];
    dec_words( rk, nr, kw );
    dec_block( kw, nr, in, out );
    memset( kw, 0, sizeof( kw ) );
}

void aes128_encrypt( const unsigned char rk[176],
                     const unsigned char in[16], unsigned char out[16] )
{ aes_encrypt( rk, 10, in, out ); }
void aes128_decrypt( const unsigned char rk[176],
                     const unsigned char in[16], unsigned char out[16] )
{ aes_decrypt( rk, 10, in, out ); }
void aes256_encrypt( const unsigned char rk[240],
                     const unsigned char in[16], unsigned char out[16] )
{ aes_encrypt( rk, 14, in, out ); }
void aes256_decrypt( const unsigned char rk[240],
                     const unsigned char in[16], unsigned char out[16] )
{ aes_decrypt( rk, 14, in, out ); }

int aes_cbc_encrypt( const unsigned char *rk, int nr, const unsigned char iv[16],
                     const unsigned char *in, unsigned char *out, int len )
{
    u32 kw[AES_MAX_WORDS];
    unsigned char prev[16], blk[16];
    int i, j;
    if ( len <= 0 || ( len & 15 ) ) return -1;
    enc_words( rk, nr, kw );
    memcpy( prev, iv, 16 );
    for ( i = 0; i < len; i += 16 ) {
        for ( j = 0; j < 16; j++ ) blk[j] = (unsigned char)( in[i + j] ^ prev[j] );
        enc_block( kw, nr, blk, out + i );
        memcpy( prev, out + i, 16 );
    }
    memset( kw, 0, sizeof( kw ) );
    return 0;
}

int aes_cbc_decrypt( const unsigned char *rk, int nr, const unsigned char iv[16],
                     const unsigned char *in, unsigned char *out, int len )
{
    u32 kw[AES_MAX_WORDS];
    unsigned char prev[16], ct[16];
    int i, j;
    if ( len <= 0 || ( len & 15 ) ) return -1;
    dec_words( rk, nr, kw );
    memcpy( prev, iv, 16 );
    for ( i = 0; i < len; i += 16 ) {
        memcpy( ct, in + i, 16 );               /* save (in case in == out) */
        dec_block( kw, nr, in + i, out + i );
        for ( j = 0; j < 16; j++ ) out[i + j] ^= prev[j];
        memcpy( prev, ct, 16 );
    }
    memset( kw, 0, sizeof( kw ) );
    return 0;
}

int aes128_cbc_encrypt( const unsigned char rk[176], const unsigned char iv[16],
                        const unsigned char *in, unsigned char *out, int len )
{ return aes_cbc_encrypt( rk, 10, iv, in, out, len ); }
int aes128_cbc_decrypt( const unsigned char rk[176], const unsigned char iv[16],
                        const unsigned char *in, unsigned char *out, int len )
{ return aes_cbc_decrypt( rk, 10, iv, in, out, len ); }

void aes_ctr32( const unsigned char *rk, int nr, const unsigned char icb[16],
                const unsigned char *in, int len, unsigned char *out )
{
    u32 kw[AES_MAX_WORDS];
    unsigned char cb[16], ks[16];
    int i, j, n;
    enc_words( rk, nr, kw );
    memcpy( cb, icb, 16 );
    for ( i = 0; i < len; i += 16 ) {
        enc_block( kw, nr, cb, ks );
        n = ( len - i >= 16 ) ? 16 : ( len - i );
        for ( j = 0; j < n; j++ ) out[i + j] = (unsigned char)( in[i + j] ^ ks[j] );
        for ( j = 15; j >= 12; j-- ) {          /* inc32 */
            if ( ++cb[j] != 0 ) break;
        }
    }
    memset( kw, 0, sizeof( kw ) );
    memset( ks, 0, sizeof( ks ) );
}
