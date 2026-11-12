/*
 * PWL.C - the password list behind NET LOGON, inside SMBFS.SYS
 *
 * A user's saved passwords and remembered connections are a file, NAME.PWL
 * beside NET.EXE, encrypted with a key made from a password of the list's
 * own.  NET.EXE reads the file and asks for the passwords; this does the
 * rest, and keeps the unlocked list and its key here until NET LOGOFF or a
 * restart.  A NET USE with no password (NUF_SAVED) is answered from it.
 *
 * WHAT IT PROTECTS AND WHAT IT DOES NOT.  The file on its own is safe to
 * lose: without the list's password it is noise, and the key is slow to
 * guess at.  Once unlocked, the passwords are in this driver's memory, and
 * PM-DOS is a single-user system whose programs can READ the kernel's
 * memory (its pages are write-protected, not hidden) - so the unlocked
 * list is as safe as the machine left logged on, which is WfW's position
 * too.  What this does make sure of is that no call hands a saved password
 * or the key back to a program: every change comes back as the whole file,
 * encrypted, for NET.EXE to write, and PWL_LOCK wipes the memory.
 *
 * THE FILE.  A 64-byte header, which is also GCM's associated data - a
 * changed byte in it fails the tag:
 *      0  "PMDOSPWL"
 *      8  the version, 1 (a word)
 *     10  the header's length, 64 (a word)
 *     12  PBKDF2 rounds (a dword)
 *     16  the salt, 16 bytes
 *     32  how many times the file has been written (a dword)
 *     36  GCM's nonce, 12 bytes: 8 random and that count, big-endian
 *     48  how many bytes are encrypted (a dword)
 *     52  zeros, to 64
 * then the encrypted entries, then GCM's 16-byte tag.  Encrypted:
 *     "PWL1", the owner (32 bytes), how many passwords and connections
 *     (a word each), each password (domain or server, user, password -
 *     64 bytes each) and each connection (the drive and the flags, a dword
 *     each; server 64, share 80, user 64, domain 64).  Numbers are
 *     little-endian.
 *
 * THE KEY is PBKDF2-HMAC-SHA256 of the list's password and the salt, the
 * rounds being the file's own.  AES-256 in GCM.  The nonce carries the
 * write count, so it never repeats under one key even if the random half
 * does; a new password brings a new salt and so a new key.
 */
#define ND_DRIVER
#include "ndrv.h"
#include "redir.h"
#include "pwl.h"
#define NST_U32_DEFINED                /* NDRV.H's u32 - the same 32 bits */
#include "sha.h"
#include "aes.h"
#include "gcm.h"

void *memcpy( void *to, const void *from, unsigned len );
void *memset( void *to, int value, unsigned len );

#define PWL_VERSION     1
#define PWL_HEAD        64
#define PWL_TAG         16
#define PWL_ROUNDS      8192UL          /* a new list's */
#define PWL_ROUNDS_MIN  1000UL          /* a file outside these is not ours: */
#define PWL_ROUNDS_MAX  1000000UL       /* the top keeps a bad one from hanging */
#define PWL_FIELD       64
#define PWL_SHARE       80
#define PWL_OWNER       32
#define CRED_LEN        ( 3 * PWL_FIELD )                       /* 192 */
#define MAP_LEN         ( 8 + 3 * PWL_FIELD + PWL_SHARE )       /* 280 */
#define PLAIN_HEAD      ( 4 + PWL_OWNER + 4 )                   /* 40 */
#define PLAIN_MAX       ( PLAIN_HEAD + PWL_CREDS * CRED_LEN + PWL_MAPS * MAP_LEN )

/* the largest file has to fit the block NET.EXE hands over */
typedef char pwl_fits[( PWL_HEAD + PLAIN_MAX + PWL_TAG <= PWL_FILE_MAX ) ? 1 : -1];
/* ...and the block has to be the size REDIR.INC's PWLR_SIZE says */
typedef char pwl_size[( sizeof( pwl_req ) == 12944 ) ? 1 : -1];

typedef struct {
    char target[PWL_FIELD];             /* a domain, or a server */
    char user[PWL_FIELD];
    char pass[PWL_FIELD];
} pwl_cred;

typedef struct {
    u32  drive, flags;
    char server[PWL_FIELD];
    char share[PWL_SHARE];
    char user[PWL_FIELD];
    char domain[PWL_FIELD];
} pwl_map;

typedef struct {
    int      open;                      /* a list is unlocked */
    char     owner[PWL_OWNER];
    u8       salt[16];
    u32      rounds;
    u32      saves;
    u8       key[32];
    int      ncred, nmap;
    pwl_cred cred[PWL_CREDS];
    pwl_map  map[PWL_MAPS];
    u8       plain[PLAIN_MAX];          /* the entries, before and after GCM */
} pwl_list;

/* bought the first time it is needed and kept: wiped, never freed */
static pwl_list *g_list;

/* =================================================================== */
/* small helpers                                                       */
/* =================================================================== */

static int text_len( const char *text, int max )
{
    int len = 0;
    while ( len < max && text[len] ) len++;
    return len;
}

static void text_copy( char *to, const char *from, int max )
{
    int i;
    for ( i = 0; i < max - 1 && from[i]; i++ ) to[i] = from[i];
    to[i] = 0;
}

static char fold( char ch )
{
    return ( ch >= 'a' && ch <= 'z' ) ? (char)( ch - 32 ) : ch;
}

static int same_text( const char *a, const char *b )
{
    while ( *a && fold( *a ) == fold( *b ) ) { a++; b++; }
    return *a == 0 && *b == 0;
}

static int same_bytes( const u8 *a, const u8 *b, int len )
{
    int i;
    for ( i = 0; i < len; i++ ) if ( a[i] != b[i] ) return 0;
    return 1;
}

static void wipe( void *what, unsigned len )
{
    volatile u8 *p = (volatile u8 *)what;
    while ( len-- ) *p++ = 0;
}

static void put16( u8 *at, u32 value )
{
    at[0] = (u8)value;
    at[1] = (u8)( value >> 8 );
}

static void put32( u8 *at, u32 value )
{
    at[0] = (u8)value;
    at[1] = (u8)( value >> 8 );
    at[2] = (u8)( value >> 16 );
    at[3] = (u8)( value >> 24 );
}

static u32 get16( const u8 *at )
{
    return (u32)at[0] | ( (u32)at[1] << 8 );
}

static u32 get32( const u8 *at )
{
    return (u32)at[0] | ( (u32)at[1] << 8 ) | ( (u32)at[2] << 16 ) | ( (u32)at[3] << 24 );
}

/* a field out of the file into a string, NUL-terminated whatever it held */
static void take( char *to, const u8 *from, int len )
{
    memcpy( to, from, (unsigned)len );
    to[len - 1] = 0;
}

/* a string into a field of the file, zero-filled */
static void give( u8 *to, const char *from, int len )
{
    memset( to, 0, (unsigned)len );
    text_copy( (char *)to, from, len );
}

/* =================================================================== */
/* random numbers                                                      */
/*                                                                     */
/* For the salt and the nonce.  A 486 has no time-stamp counter and     */
/* the machine no entropy source, so the pool is fed what NET.EXE       */
/* gathers - a free-running count read at each key of the password -    */
/* with the tick count, every time it is called, and SHA-256 does the   */
/* rest.  The nonce does not rely on it: its write count cannot repeat. */
/* =================================================================== */

static u8  g_pool[32];
static u32 g_draws;

static void rnd_stir( const u8 *data, int len )
{
    SHA256_CTX ctx;
    u8         more[8];

    put32( more, nds_ticks() );
    put32( more + 4, g_draws );
    sha256_init( &ctx );
    sha256_update( &ctx, g_pool, sizeof( g_pool ) );
    sha256_update( &ctx, data, (unsigned)len );
    sha256_update( &ctx, more, sizeof( more ) );
    sha256_final( &ctx, g_pool );
    wipe( &ctx, sizeof( ctx ) );
}

static void rnd_bytes( u8 *out, int len )
{
    u8  in[40], block[32];
    int n;

    while ( len > 0 ) {
        memcpy( in, g_pool, 32 );
        put32( in + 32, g_draws++ );
        memcpy( in + 36, "OUT!", 4 );
        sha256( in, sizeof( in ), block );
        n = ( len < 32 ) ? len : 32;
        memcpy( out, block, (unsigned)n );
        out += n;
        len -= n;
    }
    /* a new pool, so what was given cannot be worked back from it */
    memcpy( in, g_pool, 32 );
    put32( in + 32, g_draws++ );
    memcpy( in + 36, "KEY!", 4 );
    sha256( in, sizeof( in ), g_pool );
    wipe( in, sizeof( in ) );
    wipe( block, sizeof( block ) );
}

/* =================================================================== */
/* the key: PBKDF2-HMAC-SHA256, one 32-byte block                      */
/*                                                                     */
/* The two HMAC pads are hashed once and their states copied for every */
/* round, which halves the work of calling hmac_sha256 each time - and  */
/* the round count is what decides how long NET LOGON takes on a 486.   */
/* =================================================================== */

static void pbkdf2( const char *pass, const u8 *salt, int saltlen, u32 rounds, u8 out[32] )
{
    SHA256_CTX inner, outer, ctx;
    u8         pad[64], block[32], count[4];
    int        passlen = text_len( pass, PWL_FIELD ), i;
    u32        round;

    /* a password longer than the block would be hashed first; ours stop
       at 63, so the key is the password itself, zero-filled */
    memset( pad, 0x36, sizeof( pad ) );
    for ( i = 0; i < passlen; i++ ) pad[i] ^= (u8)pass[i];
    sha256_init( &inner );
    sha256_update( &inner, pad, sizeof( pad ) );
    memset( pad, 0x5C, sizeof( pad ) );
    for ( i = 0; i < passlen; i++ ) pad[i] ^= (u8)pass[i];
    sha256_init( &outer );
    sha256_update( &outer, pad, sizeof( pad ) );

    /* U1 = HMAC( password, salt || 00000001 ) */
    count[0] = 0; count[1] = 0; count[2] = 0; count[3] = 1;
    ctx = inner;
    sha256_update( &ctx, salt, (unsigned)saltlen );
    sha256_update( &ctx, count, 4 );
    sha256_final( &ctx, block );
    ctx = outer;
    sha256_update( &ctx, block, 32 );
    sha256_final( &ctx, block );
    memcpy( out, block, 32 );

    /* U2 ... Un, each the HMAC of the last, all of them XORed together */
    for ( round = 1; round < rounds; round++ ) {
        ctx = inner;
        sha256_update( &ctx, block, 32 );
        sha256_final( &ctx, block );
        ctx = outer;
        sha256_update( &ctx, block, 32 );
        sha256_final( &ctx, block );
        for ( i = 0; i < 32; i++ ) out[i] ^= block[i];
    }

    wipe( pad, sizeof( pad ) );
    wipe( block, sizeof( block ) );
    wipe( &inner, sizeof( inner ) );
    wipe( &outer, sizeof( outer ) );
    wipe( &ctx, sizeof( ctx ) );
}

/* =================================================================== */
/* the list, in memory                                                 */
/* =================================================================== */

static pwl_list *list_get( void )
{
    if ( g_list == (pwl_list *)0 ) {
        g_list = (pwl_list *)nds_alloc( sizeof( pwl_list ) );
        if ( g_list != (pwl_list *)0 ) memset( g_list, 0, sizeof( pwl_list ) );
    }
    return g_list;
}

/* forget it: the passwords, the key and every scrap of the entries */
static void list_wipe( pwl_list *list )
{
    wipe( list, sizeof( pwl_list ) );
}

/* the same, but not the plain buffer - which holds the entries file_open
   has just decrypted and is about to read */
static void list_clear( pwl_list *list )
{
    wipe( list, (unsigned)( (u8 *)list->plain - (u8 *)list ) );
}

/* =================================================================== */
/* the file                                                            */
/* =================================================================== */

/* the list, encrypted, into q->data; q->len its length */
static void file_write( pwl_list *list, pwl_req *q )
{
    u8  *file = q->data, *plain = list->plain;
    u8   rk[240], hkey[16];
    u32  plen;
    int  i;

    memcpy( plain, "PWL1", 4 );
    give( plain + 4, list->owner, PWL_OWNER );
    put16( plain + 36, (u32)list->ncred );
    put16( plain + 38, (u32)list->nmap );
    plen = PLAIN_HEAD;
    for ( i = 0; i < list->ncred; i++ ) {
        give( plain + plen, list->cred[i].target, PWL_FIELD );
        give( plain + plen + PWL_FIELD, list->cred[i].user, PWL_FIELD );
        give( plain + plen + 2 * PWL_FIELD, list->cred[i].pass, PWL_FIELD );
        plen += CRED_LEN;
    }
    for ( i = 0; i < list->nmap; i++ ) {
        put32( plain + plen, list->map[i].drive );
        put32( plain + plen + 4, list->map[i].flags );
        give( plain + plen + 8, list->map[i].server, PWL_FIELD );
        give( plain + plen + 8 + PWL_FIELD, list->map[i].share, PWL_SHARE );
        give( plain + plen + 8 + PWL_FIELD + PWL_SHARE, list->map[i].user, PWL_FIELD );
        give( plain + plen + 8 + 2 * PWL_FIELD + PWL_SHARE, list->map[i].domain, PWL_FIELD );
        plen += MAP_LEN;
    }

    memset( file, 0, PWL_HEAD );
    memcpy( file, "PMDOSPWL", 8 );
    put16( file + 8, PWL_VERSION );
    put16( file + 10, PWL_HEAD );
    put32( file + 12, list->rounds );
    memcpy( file + 16, list->salt, 16 );
    list->saves++;
    put32( file + 32, list->saves );
    rnd_bytes( file + 36, 8 );
    file[44] = (u8)( list->saves >> 24 );
    file[45] = (u8)( list->saves >> 16 );
    file[46] = (u8)( list->saves >> 8 );
    file[47] = (u8)list->saves;
    put32( file + 48, plen );

    aes256_expand_key( list->key, rk );
    gcm_calc_h( rk, 14, hkey );
    gcm_encrypt( rk, 14, hkey, file + 36, file, PWL_HEAD, plain, (int)plen,
                 file + PWL_HEAD, file + PWL_HEAD + plen );
    q->len = PWL_HEAD + plen + PWL_TAG;

    wipe( rk, sizeof( rk ) );
    wipe( hkey, sizeof( hkey ) );
    wipe( plain, plen );
}

/* q->data opened with pass, and the list made of it - but only once every
   check has passed: until then the list in memory, unlocked or not, is as
   it was.  PWLE_* */
static u32 file_open( pwl_list *list, const pwl_req *q, const char *pass )
{
    const u8 *file = q->data;
    u8       *plain = list->plain;
    u8        key[32], rk[240], hkey[16];
    u32       len = q->len, plen, rounds, ncred, nmap, pos;
    int       i, bad;

    if ( len < PWL_HEAD + PLAIN_HEAD + PWL_TAG || len > PWL_FILE_MAX ) return PWLE_BADFILE;
    if ( !same_bytes( file, (const u8 *)"PMDOSPWL", 8 ) || get16( file + 8 ) != PWL_VERSION ||
         get16( file + 10 ) != PWL_HEAD ) return PWLE_BADFILE;
    rounds = get32( file + 12 );
    if ( rounds < PWL_ROUNDS_MIN || rounds > PWL_ROUNDS_MAX ) return PWLE_BADFILE;
    plen = get32( file + 48 );
    if ( plen > PLAIN_MAX || PWL_HEAD + plen + PWL_TAG != len ) return PWLE_BADFILE;

    pbkdf2( pass, file + 16, 16, rounds, key );
    aes256_expand_key( key, rk );
    gcm_calc_h( rk, 14, hkey );
    bad = gcm_decrypt( rk, 14, hkey, file + 36, file, PWL_HEAD, file + PWL_HEAD, (int)plen,
                       file + PWL_HEAD + plen, plain );
    wipe( rk, sizeof( rk ) );
    wipe( hkey, sizeof( hkey ) );
    if ( bad ) {
        wipe( key, sizeof( key ) );
        wipe( plain, plen );
        return PWLE_BADPASS;
    }

    /* authentic - so anything wrong now is a file some other version made */
    ncred = ( plen >= PLAIN_HEAD ) ? get16( plain + 36 ) : 0;
    nmap  = ( plen >= PLAIN_HEAD ) ? get16( plain + 38 ) : 0;
    if ( plen < PLAIN_HEAD || !same_bytes( plain, (const u8 *)"PWL1", 4 ) ||
         ncred > PWL_CREDS || nmap > PWL_MAPS ||
         PLAIN_HEAD + ncred * CRED_LEN + nmap * MAP_LEN != plen ) {
        wipe( key, sizeof( key ) );
        wipe( plain, plen );
        return PWLE_BADFILE;
    }
    {
        char owner[PWL_OWNER];
        take( owner, plain + 4, PWL_OWNER );
        if ( !same_text( owner, q->owner ) ) {
            wipe( key, sizeof( key ) );
            wipe( plain, plen );
            return PWLE_OWNER;
        }
    }

    /* it is good: it replaces whatever was unlocked */
    list_clear( list );
    take( list->owner, plain + 4, PWL_OWNER );
    memcpy( list->salt, file + 16, 16 );
    list->rounds = rounds;
    list->saves = get32( file + 32 );
    memcpy( list->key, key, 32 );
    list->ncred = (int)ncred;
    list->nmap = (int)nmap;
    pos = PLAIN_HEAD;
    for ( i = 0; i < (int)ncred; i++ ) {
        take( list->cred[i].target, plain + pos, PWL_FIELD );
        take( list->cred[i].user, plain + pos + PWL_FIELD, PWL_FIELD );
        take( list->cred[i].pass, plain + pos + 2 * PWL_FIELD, PWL_FIELD );
        pos += CRED_LEN;
    }
    for ( i = 0; i < (int)nmap; i++ ) {
        list->map[i].drive = get32( plain + pos );
        list->map[i].flags = get32( plain + pos + 4 );
        take( list->map[i].server, plain + pos + 8, PWL_FIELD );
        take( list->map[i].share, plain + pos + 8 + PWL_FIELD, PWL_SHARE );
        take( list->map[i].user, plain + pos + 8 + PWL_FIELD + PWL_SHARE, PWL_FIELD );
        take( list->map[i].domain, plain + pos + 8 + 2 * PWL_FIELD + PWL_SHARE, PWL_FIELD );
        pos += MAP_LEN;
    }
    list->open = 1;
    wipe( key, sizeof( key ) );
    wipe( plain, plen );
    return PWLE_OK;
}

/* a new key from a new password and a new salt */
static void list_rekey( pwl_list *list, const char *pass )
{
    rnd_bytes( list->salt, sizeof( list->salt ) );
    list->rounds = PWL_ROUNDS;
    pbkdf2( pass, list->salt, sizeof( list->salt ), list->rounds, list->key );
}

/* =================================================================== */
/* the calls                                                           */
/* =================================================================== */

/* every string field ends inside itself, whatever the program sent */
static void req_terminate( pwl_req *q )
{
    q->owner[sizeof( q->owner ) - 1] = 0;
    q->pass[sizeof( q->pass ) - 1] = 0;
    q->newpass[sizeof( q->newpass ) - 1] = 0;
    q->target[sizeof( q->target ) - 1] = 0;
    q->user[sizeof( q->user ) - 1] = 0;
    q->secret[sizeof( q->secret ) - 1] = 0;
    q->server[sizeof( q->server ) - 1] = 0;
    q->share[sizeof( q->share ) - 1] = 0;
    q->domain[sizeof( q->domain ) - 1] = 0;
}

static u32 do_addcred( pwl_list *list, pwl_req *q )
{
    int i;
    if ( q->target[0] == 0 ) return PWLE_ARGS;
    for ( i = 0; i < list->ncred; i++ )
        if ( same_text( list->cred[i].target, q->target ) && same_text( list->cred[i].user, q->user ) )
            break;
    if ( i == list->ncred ) {
        if ( list->ncred >= PWL_CREDS ) return PWLE_FULL;
        list->ncred++;
    }
    text_copy( list->cred[i].target, q->target, PWL_FIELD );
    text_copy( list->cred[i].user, q->user, PWL_FIELD );
    text_copy( list->cred[i].pass, q->secret, PWL_FIELD );
    file_write( list, q );
    return PWLE_OK;
}

/* every password saved for the target - only the user's, when one is named */
static u32 do_delcred( pwl_list *list, pwl_req *q )
{
    int i, kept = 0, gone = 0;
    if ( q->target[0] == 0 ) return PWLE_ARGS;
    for ( i = 0; i < list->ncred; i++ ) {
        if ( same_text( list->cred[i].target, q->target ) &&
             ( q->user[0] == 0 || same_text( list->cred[i].user, q->user ) ) ) {
            gone++;
            continue;
        }
        if ( kept != i ) list->cred[kept] = list->cred[i];
        kept++;
    }
    if ( gone == 0 ) return PWLE_NOTFOUND;
    for ( i = kept; i < list->ncred; i++ ) wipe( &list->cred[i], sizeof( pwl_cred ) );
    list->ncred = kept;
    file_write( list, q );
    return PWLE_OK;
}

static u32 do_addmap( pwl_list *list, pwl_req *q )
{
    int i;
    if ( q->drive >= 26 || q->server[0] == 0 || q->share[0] == 0 ) return PWLE_ARGS;
    for ( i = 0; i < list->nmap; i++ ) if ( list->map[i].drive == q->drive ) break;
    if ( i == list->nmap ) {
        if ( list->nmap >= PWL_MAPS ) return PWLE_FULL;
        list->nmap++;
    }
    list->map[i].drive = q->drive;
    list->map[i].flags = q->flags & NUF_SMB1;
    text_copy( list->map[i].server, q->server, PWL_FIELD );
    text_copy( list->map[i].share, q->share, PWL_SHARE );
    text_copy( list->map[i].user, q->user, PWL_FIELD );
    text_copy( list->map[i].domain, q->domain, PWL_FIELD );
    file_write( list, q );
    return PWLE_OK;
}

static u32 do_delmap( pwl_list *list, pwl_req *q )
{
    int i;
    for ( i = 0; i < list->nmap; i++ ) if ( list->map[i].drive == q->drive ) break;
    if ( i == list->nmap ) return PWLE_NOTFOUND;
    for ( ; i + 1 < list->nmap; i++ ) list->map[i] = list->map[i + 1];
    list->nmap--;
    wipe( &list->map[list->nmap], sizeof( pwl_map ) );
    file_write( list, q );
    return PWLE_OK;
}

void pwl_call( pwl_req *q )
{
    pwl_list *list;
    u32       rc = PWLE_OK;

    req_terminate( q );
    rnd_stir( q->seed, (int)sizeof( q->seed ) );
    list = list_get();
    if ( list == (pwl_list *)0 ) {
        q->status = PWLE_MEMORY;
        goto out;
    }

    switch ( q->op ) {
    case PWL_STATUS:
        q->flags = (u32)list->open;
        q->index = list->open ? (u32)list->ncred : 0;
        q->drive = list->open ? (u32)list->nmap : 0;
        text_copy( q->owner, list->open ? list->owner : "", (int)sizeof( q->owner ) );
        break;

    case PWL_CREATE:
        if ( q->owner[0] == 0 ) { rc = PWLE_ARGS; break; }
        list_wipe( list );
        text_copy( list->owner, q->owner, PWL_OWNER );
        list_rekey( list, q->pass );
        list->open = 1;
        file_write( list, q );
        break;

    case PWL_UNLOCK:
        rc = file_open( list, q, q->pass );
        break;

    case PWL_LOCK:
        list_wipe( list );
        break;

    case PWL_CHANGE:
        rc = file_open( list, q, q->pass );
        if ( rc != PWLE_OK ) break;
        list_rekey( list, q->newpass );
        file_write( list, q );
        break;

    case PWL_ADDCRED:
    case PWL_DELCRED:
    case PWL_ADDMAP:
    case PWL_DELMAP:
    case PWL_GETCRED:
    case PWL_GETMAP:
        if ( !list->open ) { rc = PWLE_LOCKED; break; }
        if ( q->op == PWL_ADDCRED ) rc = do_addcred( list, q );
        else if ( q->op == PWL_DELCRED ) rc = do_delcred( list, q );
        else if ( q->op == PWL_ADDMAP ) rc = do_addmap( list, q );
        else if ( q->op == PWL_DELMAP ) rc = do_delmap( list, q );
        else if ( q->op == PWL_GETCRED ) {
            if ( q->index >= (u32)list->ncred ) { rc = PWLE_NOMORE; break; }
            text_copy( q->target, list->cred[q->index].target, (int)sizeof( q->target ) );
            text_copy( q->user, list->cred[q->index].user, (int)sizeof( q->user ) );
        } else {
            pwl_map *map;
            if ( q->index >= (u32)list->nmap ) { rc = PWLE_NOMORE; break; }
            map = &list->map[q->index];
            q->drive = map->drive;
            q->flags = map->flags;
            text_copy( q->server, map->server, (int)sizeof( q->server ) );
            text_copy( q->share, map->share, (int)sizeof( q->share ) );
            text_copy( q->user, map->user, (int)sizeof( q->user ) );
            text_copy( q->domain, map->domain, (int)sizeof( q->domain ) );
        }
        break;

    default:
        rc = PWLE_ARGS;
        break;
    }
    q->status = rc;

out:
    /* the program's copies of the passwords it sent: NET.EXE clears them
       too, but nothing should rely on the program to */
    wipe( q->pass, sizeof( q->pass ) );
    wipe( q->newpass, sizeof( q->newpass ) );
    wipe( q->secret, sizeof( q->secret ) );
}

int pwl_fill( const char *server, const char *domain, char *user, int usermax,
              char *pass, int passmax )
{
    const char *target = domain[0] ? domain : server;
    int         i;

    if ( g_list == (pwl_list *)0 || !g_list->open ) return 0;
    for ( i = 0; i < g_list->ncred; i++ ) {
        pwl_cred *cred = &g_list->cred[i];
        if ( !same_text( cred->target, target ) ) continue;
        if ( user[0] && !same_text( cred->user, user ) ) continue;
        if ( user[0] == 0 ) text_copy( user, cred->user, usermax );
        text_copy( pass, cred->pass, passmax );
        return 1;
    }
    return 0;
}
