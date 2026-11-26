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

void *sys_mem_alloc( unsigned int size )
{
    return malloc( size );
}

unsigned int sys_mem_largest( void )
{
    return 64UL * 1024 * 1024;
}

void sys_stdout( const char *buf, unsigned int len )
{
    fwrite( buf, 1, len, stdout );
}

static int slot_open( const char *path, const char *mode, uint32_t *handle )
{
    int slot;

    for ( slot = 1; slot < NFILES; slot++ ) {
        if ( files[slot] == NULL ) {
            files[slot] = fopen( path, mode );
            if ( files[slot] == NULL ) {
                return ERR_NOFILE;
            }
            *handle = (uint32_t)slot;
            return 0;
        }
    }
    return ERR_NOHANDLES;
}

int sys_open_read( const char *path, uint32_t *handle )
{
    return slot_open( path, "rb", handle );
}

int sys_create( const char *path, uint32_t *handle )
{
    return slot_open( path, "wb+", handle );
}

int sys_read( uint32_t handle, void *buf, uint32_t len, uint32_t *done )
{
    *done = (uint32_t)fread( buf, 1, len, files[handle] );
    return 0;
}

int sys_write( uint32_t handle, const void *buf, uint32_t len, uint32_t *done )
{
    *done = (uint32_t)fwrite( buf, 1, len, files[handle] );
    return *done == len ? 0 : ERR_DISKFULL;
}

int sys_seek( uint32_t handle, uint32_t pos )
{
    return fseek( files[handle], (long)pos, SEEK_SET ) ? ERR_ACCESS : 0;
}

int sys_close( uint32_t handle )
{
    int rc = fclose( files[handle] );

    files[handle] = NULL;
    return rc ? ERR_ACCESS : 0;
}

int sys_file_size( uint32_t handle, uint32_t *size )
{
    fseek( files[handle], 0, SEEK_END );
    *size = (uint32_t)ftell( files[handle] );
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
    rec->attr = (uint8_t)found->attrib;
    rec->size = (uint32_t)found->size;
    strncpy( rec->name, found->name, PATH_LEN - 1 );
}

int sys_find_first( const char *pattern, uint16_t attrs, FINDREC *rec, uint32_t *handle )
{
    struct _finddata_t found;
    long find = _findfirst( pattern, &found );

    (void)attrs;
    if ( find == -1 ) {
        return ERR_NOFILE;
    }
    *handle = (uint32_t)find;
    find_rec( &found, rec );
    return 0;
}

int sys_find_next( uint32_t handle, FINDREC *rec )
{
    struct _finddata_t found;

    if ( _findnext( (long)handle, &found ) != 0 ) {
        return ERR_NOMORE;
    }
    find_rec( &found, rec );
    return 0;
}

void sys_find_close( uint32_t handle )
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
