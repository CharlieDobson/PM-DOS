/*
 * SYS_NT.C - PMWRITE's system calls on Windows, for PWHOST.EXE: the
 * document, the formats and the PDF code built as a console program
 * that reads a file and writes what it made of it, so that a format
 * can be checked without booting anything.  Files and memory only -
 * there is no screen here.
 *
 * Not built by BUILD.CMD; MKHOST.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "rt.h"
#include "sys.h"

#define NFILES  16

static FILE *files[NFILES];

int sys_init( void )
{
    return 0;
}

const char *sys_cmdline( void )
{
    return "";
}

const char *sys_getenv( const char *name )
{
    return getenv( name );
}

void sys_exit( int code )
{
    exit( code );
}

void *sys_mem_alloc( u32 size )
{
    return malloc( size );
}

u32 sys_mem_largest( void )
{
    return 64UL * 1024 * 1024;
}

void sys_stdout( const char *buf, u32 len )
{
    fwrite( buf, 1, len, stdout );
}

static int slot_open( const char *path, const char *mode, u32 *handle )
{
    int slot;

    for ( slot = 1; slot < NFILES; slot++ ) {
        if ( files[slot] == NULL ) {
            files[slot] = fopen( path, mode );
            if ( files[slot] == NULL ) {
                return ERR_NOFILE;
            }
            *handle = (u32)slot;
            return 0;
        }
    }
    return ERR_NOHANDLES;
}

int sys_open_read( const char *path, u32 *handle )
{
    return slot_open( path, "rb", handle );
}

int sys_create( const char *path, u32 *handle )
{
    return slot_open( path, "wb+", handle );
}

int sys_read( u32 handle, void *buf, u32 len, u32 *done )
{
    *done = (u32)fread( buf, 1, len, files[handle] );
    return 0;
}

int sys_write( u32 handle, const void *buf, u32 len, u32 *done )
{
    *done = (u32)fwrite( buf, 1, len, files[handle] );
    return *done == len ? 0 : ERR_DISKFULL;
}

int sys_seek( u32 handle, u32 pos )
{
    return fseek( files[handle], (long)pos, SEEK_SET ) ? ERR_ACCESS : 0;
}

int sys_close( u32 handle )
{
    int rc = fclose( files[handle] );

    files[handle] = NULL;
    return rc ? ERR_ACCESS : 0;
}

int sys_file_size( u32 handle, u32 *size )
{
    fseek( files[handle], 0, SEEK_END );
    *size = (u32)ftell( files[handle] );
    fseek( files[handle], 0, SEEK_SET );
    return 0;
}

int sys_delete( const char *path )
{
    return remove( path ) ? ERR_NOFILE : 0;
}

int sys_rename( const char *from, const char *to )
{
    return rename( from, to ) ? ERR_ACCESS : 0;
}

/* a directory's files, for TF.C's search for fonts: the handle is
   _findfirst's */
static void find_rec( const struct _finddata_t *found, FINDREC *rec )
{
    memset( rec, 0, sizeof( *rec ) );
    rec->attr = (u8)found->attrib;
    rec->size = (u32)found->size;
    strncpy( rec->name, found->name, PATH_LEN - 1 );
}

int sys_find_first( const char *pattern, u16 attrs, FINDREC *rec, u32 *handle )
{
    struct _finddata_t found;
    long find = _findfirst( pattern, &found );

    (void)attrs;
    if ( find == -1 ) {
        return ERR_NOFILE;
    }
    *handle = (u32)find;
    find_rec( &found, rec );
    return 0;
}

int sys_find_next( u32 handle, FINDREC *rec )
{
    struct _finddata_t found;

    if ( _findnext( (long)handle, &found ) != 0 ) {
        return ERR_NOMORE;
    }
    find_rec( &found, rec );
    return 0;
}

void sys_find_close( u32 handle )
{
    _findclose( (long)handle );
}

/* nowhere: a test program looks for fonts only where it is told to */
const char *sys_program_dir( void )
{
    return "";
}

int sys_get_drive( void )
{
    return 2;
}

int sys_drive_valid( int drv )
{
    (void)drv;
    return 0;
}

int sys_crit_seen( void )
{
    return 0;
}

void sys_now( DATETIME *now )
{
    /* a fixed day, so that a file written twice is the same file */
    now->year = 2026;
    now->month = 1;
    now->day = 1;
    now->hour = 12;
    now->minute = 0;
    now->second = 0;
}

int sys_bus( u32 fn, u32 *regs )
{
    (void)fn;
    (void)regs;
    return 0;
}

/* no 32-bit drivers on Windows */
int sys_driver_find( u32 flag )
{
    (void)flag;
    return -1;
}

int sys_driver_call( int row, u32 fn, void *buf, u32 len, u32 *answer )
{
    (void)row;
    (void)fn;
    (void)buf;
    (void)len;
    if ( answer != NULL ) {
        *answer = 0;
    }
    return 1;
}
