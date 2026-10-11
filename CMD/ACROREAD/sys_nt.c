/*
 * SYS_NT.C - ACROREAD's system calls on Windows, for ACROHOST.EXE: the
 * same PDF code built as a console program that reads a file and
 * writes what it made of it, so that a page can be checked without
 * booting anything.  Files and memory only - there is no screen here,
 * and the calls for one do nothing.
 *
 * Not built by BUILD.CMD; MKHOST.CMD builds it.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rt.h"
#include "sys.h"

static FILE *files[8];

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

void sys_break_install( void )
{
}

void sys_stdout( const char *buf, u32 len )
{
    fwrite( buf, 1, len, stdout );
}

int sys_stdout_is_console( void )
{
    return 0;
}

int sys_stdin_is_console( void )
{
    return 0;
}

int sys_stdin_byte( void )
{
    return getchar();
}

void sys_screen_size( int *rows, int *cols )
{
    *rows = 25;
    *cols = 80;
}

void sys_put_cells( u32 first, const u16 *cells, u32 count )
{
    (void)first;
    (void)cells;
    (void)count;
}

void sys_get_cells( u32 first, u16 *cells, u32 count )
{
    (void)first;
    memset( cells, 0, count * 2 );
}

void sys_cursor( int row, int col )
{
    (void)row;
    (void)col;
}

void sys_get_cursor( int *row, int *col )
{
    *row = *col = 0;
}

void sys_cursor_shape( int kind )
{
    (void)kind;
}

int sys_key_ready( void )
{
    return 0;
}

int sys_key_read( int *shift_now )
{
    *shift_now = 0;
    return 0x011B;
}

u32 sys_hundredths( void )
{
    return 0;
}

int sys_open_read( const char *path, u32 *handle )
{
    int slot;

    for ( slot = 1; slot < 8; slot++ ) {
        if ( files[slot] == NULL ) {
            files[slot] = fopen( path, "rb" );
            if ( files[slot] == NULL ) {
                return ERR_NOFILE;
            }
            *handle = (u32)slot;
            return 0;
        }
    }
    return ERR_NOHANDLES;
}

int sys_read( u32 handle, void *buf, u32 len, u32 *done )
{
    *done = (u32)fread( buf, 1, len, files[handle] );
    return 0;
}

int sys_seek( u32 handle, u32 pos )
{
    return fseek( files[handle], (long)pos, SEEK_SET ) ? ERR_ACCESS : 0;
}

int sys_close( u32 handle )
{
    fclose( files[handle] );
    files[handle] = NULL;
    return 0;
}

int sys_file_size( u32 handle, u32 *size )
{
    fseek( files[handle], 0, SEEK_END );
    *size = (u32)ftell( files[handle] );
    fseek( files[handle], 0, SEEK_SET );
    return 0;
}

int sys_vbe_info( u8 *block, int *version, int *modes )
{
    (void)block;
    *version = *modes = 0;
    return 0;
}

int sys_vbe_mode( int mode, u8 *block )
{
    (void)mode;
    (void)block;
    return 0;
}

int sys_gfx_set( int mode, u32 *bytes, u32 *pitch )
{
    (void)mode;
    *bytes = *pitch = 0;
    return 1;
}

void sys_gfx_restore( void )
{
}

void sys_gfx_palette( int first, int count, const u8 *bgr0 )
{
    (void)first;
    (void)count;
    (void)bgr0;
}

void __cdecl gfx_put( u32 offset, const void *src, u32 bytes )
{
    (void)offset;
    (void)src;
    (void)bytes;
}

void __cdecl gfx_fill( u32 offset, u32 value, u32 bytes )
{
    (void)offset;
    (void)value;
    (void)bytes;
}

unsigned __cdecl port_in( unsigned port )
{
    (void)port;
    return 0;
}

void __cdecl port_out( unsigned port, unsigned value )
{
    (void)port;
    (void)value;
}
