/* ==========================================================================
 * SMBCTEST.C - known-answer tests for SMBCRYPT.C.  Host build:
 *   wcl386 -bt=nt -fe=smbctest.exe smbctest.c smbcrypt.c
 * Vectors: RFC 1320/1321 (MD4/MD5), RFC 2202 (HMAC-MD5), the classic RC4
 * strings, the NT hash of "password", and the MS-NLMP NTOWFv2 example.
 * ========================================================================== */
#include "smbcrypt.h"
#include "smbsign.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;

static void hexof( const unsigned char *data, int len, char *out )
{
    static const char *hex = "0123456789abcdef";
    int i;
    for ( i = 0; i < len; i++ ) {
        out[i * 2]     = hex[( data[i] >> 4 ) & 0xF];
        out[i * 2 + 1] = hex[data[i] & 0xF];
    }
    out[len * 2] = 0;
}

static void check( const char *name, const unsigned char *got, int len, const char *want )
{
    char hex[132];
    hexof( got, len, hex );
    if ( strcmp( hex, want ) == 0 ) {
        printf( "  ok    %-14s %s\n", name, hex );
    } else {
        printf( "  FAIL  %-14s got %s want %s\n", name, hex, want );
        fails++;
    }
}

int main( void )
{
    unsigned char dig[16], out[64];
    RC4_CTX       rc4;

    printf( "SMBCRYPT known-answer tests\n" );

    md4( (const unsigned char *)"", 0, dig );
    check( "md4-empty", dig, 16, "31d6cfe0d16ae931b73c59d7e0c089c0" );
    md4( (const unsigned char *)"abc", 3, dig );
    check( "md4-abc", dig, 16, "a448017aaf21d8525fc10ae87aa6729d" );

    md5( (const unsigned char *)"", 0, dig );
    check( "md5-empty", dig, 16, "d41d8cd98f00b204e9800998ecf8427e" );
    md5( (const unsigned char *)"abc", 3, dig );
    check( "md5-abc", dig, 16, "900150983cd24fb0d6963f7d28e17f72" );
    md5( (const unsigned char *)"The quick brown fox jumps over the lazy dog", 43, dig );
    check( "md5-fox", dig, 16, "9e107d9d372bb6826bd81d3542a419d6" );

    {
        unsigned char key1[16];
        memset( key1, 0x0b, 16 );
        hmac_md5( key1, 16, (const unsigned char *)"Hi There", 8, dig );
        check( "hmacmd5-1", dig, 16, "9294727a3638bb1c13f48ef8158bfc9d" );
    }
    hmac_md5( (const unsigned char *)"Jefe", 4,
              (const unsigned char *)"what do ya want for nothing?", 28, dig );
    check( "hmacmd5-2", dig, 16, "750c783e6ab0b503eaa86e310a5db738" );

    rc4_init( &rc4, (const unsigned char *)"Key", 3 );
    rc4_crypt( &rc4, (const unsigned char *)"Plaintext", out, 9 );
    check( "rc4-key", out, 9, "bbf316e8d940af0ad3" );
    rc4_init( &rc4, (const unsigned char *)"Wiki", 4 );
    rc4_crypt( &rc4, (const unsigned char *)"pedia", out, 5 );
    check( "rc4-wiki", out, 5, "1021bf0420" );

    nt_hash( "password", dig );
    check( "nthash-pw", dig, 16, "8846f7eaee8fb117ad06bdd830b7586c" );

    ntowf_v2( "Password", "User", "Domain", dig );
    check( "ntowfv2", dig, 16, "0c868a403bfd7a93a3001ef22ef02e3f" );

    /* DES-ECB: the classic FIPS worked example */
    {
        static const unsigned char dkey[8] = { 0x13,0x34,0x57,0x79,0x9B,0xBC,0xDF,0xF1 };
        static const unsigned char dpt[8]  = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };
        des_encrypt_block( dkey, dpt, out );
        check( "des-block", out, 8, "85e813540f0ab405" );
    }

    /* LM hash (LMOWFv1) */
    lm_hash( "", dig );
    check( "lm-empty", dig, 16, "aad3b435b51404eeaad3b435b51404ee" );
    lm_hash( "password", dig );
    check( "lm-password", dig, 16, "e52cac67419a9a224a3b108f3fa6cb6d" );

    /* NTLMv1 response: desl( NTOWFv1("Password"), ServerChallenge ),
       MS-NLMP 4.2.2's worked example (challenge 0123456789abcdef) */
    {
        static const unsigned char chal[8] = { 0x01,0x23,0x45,0x67,0x89,0xAB,0xCD,0xEF };
        unsigned char nth[16];
        nt_hash( "Password", nth );
        desl( nth, chal, out );
        check( "ntlmv1-resp", out, 24, "67c43011f30298a2ad35ece64f16331c44bdbed927841f94" );
    }

    /* AES-CMAC, RFC 4493 vectors (K = 2b7e1516...) */
    {
        static const unsigned char cmac_key[16] = {
            0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };
        static const unsigned char msg[64] = {
            0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a,
            0xae,0x2d,0x8a,0x57,0x1e,0x03,0xac,0x9c,0x9e,0xb7,0x6f,0xac,0x45,0xaf,0x8e,0x51,
            0x30,0xc8,0x1c,0x46,0xa3,0x5c,0xe4,0x11,0xe5,0xfb,0xc1,0x19,0x1a,0x0a,0x52,0xef,
            0xf6,0x9f,0x24,0x45,0xdf,0x4f,0x9b,0x17,0xad,0x2b,0x41,0x7b,0xe6,0x6c,0x37,0x10 };
        aes_cmac( cmac_key, msg, 0,  dig ); check( "cmac-0",  dig, 16, "bb1d6929e95937287fa37d129b756746" );
        aes_cmac( cmac_key, msg, 16, dig ); check( "cmac-16", dig, 16, "070a16b46b4d4144f79bdd9dd04a287c" );
        aes_cmac( cmac_key, msg, 40, dig ); check( "cmac-40", dig, 16, "dfa66747de9ae63030ca32611497c827" );
        aes_cmac( cmac_key, msg, 64, dig ); check( "cmac-64", dig, 16, "51f0bebf7e3b9d92fc49741779363cfe" );
    }

    printf( fails ? "\n%d FAILED\n" : "\nall passed\n", fails );
    return fails ? 1 : 0;
}
