/*
 * LIBTEST3.C - the SDK's C library, third part: directories, stat,
 * DOS.H, running programs
 *
 *   LIBTEST3 <its own name>
 *
 * It needs its own file name to run itself as a child (argv[0] is ""
 * on kernels up to 0.69, which put no path behind the environment),
 * and checks that argv[0] is the full path on newer ones.  Run it in a
 * directory it may write in; it makes and removes LT3*.
 *
 * "FAIL" lines are checks that went wrong; the last line is
 * "END <failures>".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <direct.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dos.h>
#include <process.h>
#include <conio.h>
#include <time.h>
#include <io.h>
#include <pmdos.h>

static int failures;

#define CHECK( cond ) \
    do { if ( !(cond) ) { printf( "FAIL line %d: %s\n", __LINE__, #cond ); failures++; } } while ( 0 )

static void touch( const char *name, const char *text )
{
    FILE *fp = fopen( name, "w" );

    if ( fp != NULL ) {
        fputs( text, fp );
        fclose( fp );
    }
}

static void dirs( void )
{
    char cwd[264], back[264];
    DIR *d;
    struct dirent *e;
    struct stat st;
    int seen_a = 0, seen_long = 0, seen_sub = 0, count = 0;
    int fd;

    CHECK( getcwd( cwd, sizeof( cwd ) ) != NULL && cwd[1] == ':' && cwd[2] == '\\' );
    CHECK( _getdrive() == cwd[0] - 'A' + 1 );
    rmdir( "LT3DIR\\SUB" );
    rmdir( "LT3DIR" );
    CHECK( mkdir( "LT3DIR" ) == 0 );
    CHECK( mkdir( "LT3DIR" ) < 0 && errno == EACCES );
    CHECK( mkdir( "LT3DIR\\SUB", 0755 ) == 0 );
    touch( "LT3DIR\\A.TXT", "hello" );
    touch( "LT3DIR\\A long name.txt", "long" );
    CHECK( chdir( "LT3DIR" ) == 0 );
    CHECK( getcwd( back, sizeof( back ) ) != NULL );
    CHECK( strlen( back ) > strlen( cwd ) && strstr( back, "LT3DIR" ) != NULL );
    CHECK( chdir( ".." ) == 0 );
    CHECK( getcwd( back, sizeof( back ) ) != NULL && strcmp( back, cwd ) == 0 );

    d = opendir( "LT3DIR" );
    CHECK( d != NULL );
    while ( d != NULL && (e = readdir( d )) != NULL ) {
        count++;
        if ( strcmp( e->d_name, "A.TXT" ) == 0 && e->d_size == 5 ) {
            seen_a = 1;
        }
        if ( strcmp( e->d_name, "A long name.txt" ) == 0 ) {
            seen_long = 1;              /* only with long names on */
        }
        if ( strcmp( e->d_name, "SUB" ) == 0 && (e->d_attr & _A_SUBDIR) ) {
            seen_sub = 1;
        }
    }
    if ( d != NULL ) {
        rewinddir( d );
        CHECK( readdir( d ) != NULL );
        CHECK( closedir( d ) == 0 );
    }
    CHECK( seen_a && seen_sub && count >= 3 );
    printf( "R long names %s\n", seen_long ? "yes" : "no" );
    CHECK( opendir( "LT3NONE" ) == NULL && errno == ENOENT );

    CHECK( stat( "LT3DIR\\A.TXT", &st ) == 0 && S_ISREG( st.st_mode ) && st.st_size == 5 );
    CHECK( st.st_mtime > 1700000000L && (st.st_mode & S_IWRITE) );
    CHECK( stat( "LT3DIR", &st ) == 0 && S_ISDIR( st.st_mode ) );
    CHECK( stat( "\\", &st ) == 0 && S_ISDIR( st.st_mode ) );
    CHECK( stat( "LT3NONE.TXT", &st ) < 0 && errno == ENOENT );
    CHECK( chmod( "LT3DIR\\A.TXT", S_IREAD ) == 0 );
    CHECK( stat( "LT3DIR\\A.TXT", &st ) == 0 && !(st.st_mode & S_IWRITE) );
    CHECK( access( "LT3DIR\\A.TXT", W_OK ) < 0 && errno == EACCES );
    CHECK( remove( "LT3DIR\\A.TXT" ) < 0 );     /* read-only */
    CHECK( chmod( "LT3DIR\\A.TXT", S_IREAD | S_IWRITE ) == 0 );
    fd = open( "LT3DIR\\A.TXT", O_RDONLY );
    CHECK( fd >= 0 && fstat( fd, &st ) == 0 && S_ISREG( st.st_mode ) && st.st_size == 5 );
    close( fd );
    CHECK( fstat( 1, &st ) == 0 && S_ISREG( st.st_mode ) );     /* stdout is a file here */

    CHECK( remove( "LT3DIR\\A.TXT" ) == 0 );
    remove( "LT3DIR\\A long name.txt" );
    remove( "LT3DIR\\ALONGN~1.TXT" );
    CHECK( rmdir( "LT3DIR" ) < 0 );             /* SUB is still in it */
    CHECK( rmdir( "LT3DIR\\SUB" ) == 0 );
    CHECK( rmdir( "LT3DIR" ) == 0 );
}

static void dos_calls( void )
{
    struct find_t f;
    struct dosdate_t date;
    struct dostime_t tim;
    struct diskfree_t df;
    unsigned attr, fdate, ftime;
    unsigned rc;
    int fd, n = 0;

    touch( "LT3F1.TXT", "1" );
    touch( "LT3F2.TXT", "22" );
    rc = _dos_findfirst( "LT3F?.TXT", _A_NORMAL, &f );
    while ( rc == 0 ) {
        n++;
        rc = _dos_findnext( &f );
    }
    printf( "R findfirst found %d, last rc %u\n", n, rc );
    CHECK( n == 2 && rc == 0x12 );
    CHECK( _dos_findfirst( "LT3NONE.*", _A_NORMAL, &f ) != 0 );
    CHECK( _dos_getfileattr( "LT3F1.TXT", &attr ) == 0 && !(attr & _A_RDONLY) );
    CHECK( _dos_setfileattr( "LT3F1.TXT", _A_RDONLY ) == 0 );
    CHECK( _dos_getfileattr( "LT3F1.TXT", &attr ) == 0 && (attr & _A_RDONLY) );
    CHECK( _dos_setfileattr( "LT3F1.TXT", _A_NORMAL ) == 0 );
    fd = open( "LT3F2.TXT", O_RDWR );
    CHECK( fd >= 0 );
    CHECK( _dos_setftime( fd, (unsigned)((2001 - 1980) << 9 | 2 << 5 | 3), (unsigned)(4 << 11 | 5 << 5 | 3) ) == 0 );
    close( fd );
    fd = open( "LT3F2.TXT", O_RDONLY );
    CHECK( _dos_getftime( fd, &fdate, &ftime ) == 0 && fdate == ((2001 - 1980) << 9 | 2 << 5 | 3) && ftime == (4 << 11 | 5 << 5 | 3) );
    close( fd );
    remove( "LT3F1.TXT" );
    remove( "LT3F2.TXT" );

    _dos_getdate( &date );
    _dos_gettime( &tim );
    CHECK( date.year >= 2024 && date.month >= 1 && date.month <= 12 && date.day >= 1 );
    CHECK( date.dayofweek <= 6 && tim.hour < 24 && tim.minute < 60 );
    memset( &df, 0, sizeof( df ) );
    rc = _dos_getdiskfree( 0, &df );
    printf( "R getdiskfree rc %u: %lu of %lu clusters free, %lu sectors of %lu bytes\n", rc,
            df.avail_clusters, df.total_clusters, df.sectors_per_cluster, df.bytes_per_sector );
    CHECK( rc == 0 && df.bytes_per_sector == 512 && df.total_clusters > 0 );
    CHECK( df.avail_clusters <= df.total_clusters );
    CHECK( _dos_getdiskfree( 26, &df ) != 0 );  /* Z: is not there */
    CHECK( screenwidth() >= 40 && screenheight() >= 25 );
    gotoxy( 5, 3 );
    CHECK( wherex() == 5 && wherey() == 3 );
}

/* the kernel's own answers, not the library's: 4Eh with nothing to
   find says 12h, and 7303h's record starts with its size */
static void kernel_calls( void )
{
    struct find_t f;
    unsigned char rec[48];
    char root[4];
    pm_regs regs;

    memset( &regs, 0, sizeof( regs ) );
    regs.eax = 0x1A00;
    regs.edx = (uint32_t)&f;
    pm_int21( &regs );
    memset( &regs, 0, sizeof( regs ) );
    regs.eax = 0x4E00;
    regs.edx = (uint32_t)"LT3NONE.*";
    CHECK( pm_int21( &regs ) == 1 && PM_AX( regs ) == 0x12 );
    printf( "R 4Eh on nothing: AX %04X\n", PM_AX( regs ) );

    root[0] = (char)('A' + _getdrive() - 1);
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    memset( rec, 0, sizeof( rec ) );
    memset( &regs, 0, sizeof( regs ) );
    regs.eax = 0x7303;
    regs.edx = (uint32_t)root;
    regs.edi = (uint32_t)rec;
    regs.ecx = sizeof( rec );
    CHECK( pm_int21( &regs ) == 0 && rec[0] == 0x2C && rec[1] == 0 );
    CHECK( *(uint32_t *)(rec + 8) == 512 );     /* bytes a sector, at 08h */
    printf( "R 7303h size word %02X%02X, %lu sectors a cluster of %lu bytes\n", rec[1], rec[0],
            (unsigned long)*(uint32_t *)(rec + 4), (unsigned long)*(uint32_t *)(rec + 8) );
}

static void programs( const char *self )
{
    static char big[201];
    static char toobig[4001];
    FILE *fp;
    char line[64];
    int rc;

    if ( self == NULL ) {
        printf( "FAIL: run as LIBTEST3 <its own name>\n" );
        failures++;
        return;
    }
    rc = spawnl( P_WAIT, self, self, "child", "7", NULL );
    CHECK( rc == 7 );
    rc = spawnlp( P_WAIT, self, self, "child", "with space", NULL );
    CHECK( rc == 10 );                          /* "with space" is 10 long */
    /* past 126 characters: a tail of 7Fh and CMDLINE, then too long
       for any environment */
    memset( big, 'x', 200 );
    rc = spawnl( P_WAIT, self, self, "child", big, NULL );
    printf( "R a 200-character argument came back as %d\n", rc );
    CHECK( rc == 200 );
    memset( toobig, 'y', 4000 );
    CHECK( spawnl( P_WAIT, self, self, "child", toobig, NULL ) < 0 && errno == E2BIG );
    rc = spawnl( P_WAIT, self, self, "argv0", self, NULL );
    CHECK( rc == 0 );
    CHECK( spawnl( P_WAIT, "LT3NONE.EXE", "x", NULL ) < 0 && errno == ENOENT );
    CHECK( spawnl( P_NOWAIT, self, self, NULL ) < 0 && errno == EINVAL );
    CHECK( system( NULL ) != 0 );
    printf( "R COMSPEC=%s\n", getenv( "COMSPEC" ) ? getenv( "COMSPEC" ) : "(none)" );
    rc = system( "ECHO from the shell> LT3SYS.TMP" );
    fp = fopen( "LT3SYS.TMP", "r" );
    printf( "R LT3SYS.TMP %s\n", fp ? "made" : "not made" );
    CHECK( fp != NULL && fgets( line, sizeof( line ), fp ) != NULL
           && strncmp( line, "from the shell", 14 ) == 0 );
    if ( fp != NULL ) {
        fclose( fp );
    }
    remove( "LT3SYS.TMP" );
    printf( "R system returned %d\n", rc );
}

/* "X:\...\NAME.EXT", the file name being name and an extension */
static int is_path_to( const char *path, const char *name )
{
    const char *base = strrchr( path, '\\' );
    size_t len = strlen( name );

    if ( path[0] == '\0' || path[1] != ':' || path[2] != '\\' || base == NULL ) {
        return 0;
    }
    base++;
    return len > 0 && strnicmp( base, name, len ) == 0 && base[len] == '.';
}

int main( int argc, char **argv )
{
    if ( argc >= 2 && strcmp( argv[1], "child" ) == 0 ) {
        /* as a child: exit with the number, or the length of the text */
        return argc >= 3 ? (atoi( argv[2] ) ? atoi( argv[2] ) : (int)strlen( argv[2] )) : 99;
    }
    if ( argc >= 3 && strcmp( argv[1], "argv0" ) == 0 ) {
        /* as a child: 0 if argv[0] is a full path to the program named */
        return is_path_to( argv[0], argv[2] ) ? 0 : 1;
    }
    printf( "R argv[0] %s\n", argv[0] );
    CHECK( is_path_to( argv[0], argc >= 2 ? argv[1] : "" ) );
    dirs();
    dos_calls();
    kernel_calls();
    programs( argc >= 2 ? argv[1] : NULL );
    printf( "END %d\n", failures );
    return failures ? 1 : 0;
}
