/*
 * FILE.C - L and W, and the load the command line does at the start.
 *
 * L and W with no address move the file named by N between the disk and
 * the work area.  A .COM or data file loads at the load address (CS:100
 * in the work area, or a native program's image at its slot); an .EXE
 * is loaded and relocated as DOS would, and its header sets the start-
 * ing registers; a native D32 program is handed to the kernel to hold,
 * and DEBUG switches to MODE_NAT.  CX (16-bit) holds the length loaded.
 *
 * L with an address and a drive/sector/count reads raw sectors with
 * INT 25h; W with the same writes them with INT 26h.
 *
 * W cannot write a .EXE or .HEX, as 6.22 could not: their lengths come
 * from the header or the records, which DEBUG does not keep.
 */
#include "debug.h"

/* the tail MAIN.C built for a native load */
const u8 *native_tail( void );

static u16 load_seg( void )
{
    return work_seg;
}

/* read a whole file into BUF (at most MAX); the count, or -1 on an
   error already reported */
static long read_whole( const char *name, u8 *buf, u32 max )
{
    int handle, err;
    u32 size, got, done = 0;

    handle = sys_open( name, &err );
    if ( handle < 0 ) {
        out_err( msg_nofile );
        return -1;
    }
    sys_size( handle, &size );
    if ( size > max ) {
        size = max;
    }
    while ( done < size ) {
        if ( sys_read( handle, buf + done, size - done, &got ) || got == 0 ) {
            break;
        }
        done += got;
    }
    sys_close( handle );
    return (long)done;
}

/* An .EXE, loaded and relocated as DOS's EXEC would: the load module -
   the pages the header counts, less the header - at the load segment
   plus 10h, and the registers EXEC sets from the header.  HEAD is the
   file's start (LEN bytes, at least the header's fields); the module is
   read from the file itself, so a program bigger than filebuf loads
   whole.  0, or 1 after an error already said. */
static int load_exe( const u8 *head, u32 len )
{
    u32 hdr = (u32)*(const u16 *)( head + 8 ) * 16;
    u32 pages = *(const u16 *)( head + 4 );
    u32 last = *(const u16 *)( head + 2 ) & 0x1FF;
    u16 relocs = *(const u16 *)( head + 6 );
    u16 reltab = *(const u16 *)( head + 24 );
    u16 start_cs = *(const u16 *)( head + 22 );
    u16 start_ip = *(const u16 *)( head + 20 );
    u16 start_ss = *(const u16 *)( head + 14 );
    u16 start_sp = *(const u16 *)( head + 16 );
    u16 base = (u16)( load_seg() + 0x10 );      /* past the PSP */
    u32 size, end, image, done, got, i;
    int handle, err;

    handle = sys_open( file_name, &err );
    if ( handle < 0 ) {
        out_err( msg_nofile );
        return 1;
    }
    sys_size( handle, &size );
    end = pages ? ( pages - 1 ) * 512 + ( last ? last : 512 ) : 0;
    if ( end == 0 || end > size ) {
        end = size;
    }
    image = end > hdr ? end - hdr : 0;
    if ( image > work_len - 0x100 ) {   /* base:0 is the work area's 100h */
        sys_close( handle );
        out_err( msg_toobig );
        return 1;
    }
    done = 0;
    if ( sys_seek( handle, hdr ) == 0 ) {
        while ( done < image ) {
            if ( sys_read( handle, work_buf + 0x100 + done, image - done, &got ) || got == 0 ) {
                break;
            }
            done += got;
        }
    }
    sys_close( handle );

    /* only the entries inside what was read: a table past it is cut off */
    for ( i = 0; i < relocs && (u32)reltab + i * 4 + 4 <= len; i++ ) {
        u16 roff = *(const u16 *)( head + reltab + i * 4 );
        u16 rseg = *(const u16 *)( head + reltab + i * 4 + 2 );
        ADDR r;
        u8  wd[2];
        u16 v;
        r.seg = (u16)( base + rseg );
        r.off = roff;
        mem_read( r, wd, 2 );
        v = (u16)( wd[0] | ( wd[1] << 8 ) );
        v = (u16)( v + base );
        wd[0] = (u8)v;
        wd[1] = (u8)( v >> 8 );
        mem_write( r, wd, 2 );
    }
    cpu.cs = (u16)( base + start_cs );
    cpu.eip = start_ip;
    cpu.ss = (u16)( base + start_ss );
    cpu.esp = start_sp;
    cpu.ds = cpu.es = (u16)load_seg();
    cpu.eax = 0;
    /* BX:CX is the file's length less 512, as 6.22 has it whatever the
       header's own length is; DX, BP, SI and DI stay as they were */
    size = size > 512 ? size - 512 : 0;
    cpu.ebx = size >> 16;
    cpu.ecx = size & 0xFFFF;
    return 0;
}

/* The D32 signature 'CD' in a bare image, or behind an MZ stub that
   points at one.  1 if this file is a native program. */
static int is_d32( const u8 *file, u32 len )
{
    u32 off = 0;

    if ( len >= 2 && file[0] == 'M' && file[1] == 'Z' ) {
        if ( len >= 0x40 && *(const u16 *)( file + 0x18 ) >= 0x40 ) {
            off = *(const u32 *)( file + 0x3C );
        }
    }
    return off < len && len - off >= 2 && file[off] == 'C' && file[off + 1] == 'D';
}

/* an MZ header with the fields EXEC reads; anything else named .EXE is
   loaded as a .COM, as EXEC would */
static int is_mz( const u8 *file, u32 len )
{
    return len >= 0x1C && ( ( file[0] == 'M' && file[1] == 'Z' ) || ( file[0] == 'Z' && file[1] == 'M' ) );
}

#define FILEBUF_MAX 0x20000             /* enough to relocate an .EXE */
static u8 *filebuf;

/* The N file into memory.  WHERE is L's address, or NULL: a .COM or
   data file goes there, as 6.22's did; an .EXE and a native program go
   where they always go. */
void load_file( const ADDR *where )
{
    long got = 0;
    DRX  regs;

    if ( filebuf == NULL ) {
        filebuf = (u8 *)try_alloc( FILEBUF_MAX );
        if ( filebuf == NULL ) {
            out_err( msg_nomem );
            return;
        }
    }

    if ( file_name[0] == 0 ) {
        return;                         /* no name: 6.22's L said nothing */
    }

    /* A D32 program: let the kernel hold it, and read its start-up
       registers back.  DEBUG becomes a native debugger. */
    if ( file_kind == KIND_EXE || file_kind == KIND_DATA || file_kind == KIND_COM ) {
        got = read_whole( file_name, filebuf, FILEBUF_MAX );
        if ( got < 0 ) {
            return;
        }
        if ( is_d32( filebuf, (u32)got ) ) {
            int rc;

            /* the kernel holds one program at a time: the one held now
               goes first, as 6.22's L replaced what it had loaded */
            if ( mode == MODE_NAT ) {
                prog_release();
                new_work();
            }
            rc = sys_dbg_load( file_name, (const char *)native_tail(), &regs );
            if ( rc == ERR_EXEFMT ) {
                out_str( msg_norun );
                out_nl();
                return;
            }
            if ( rc ) {
                out_err( rc == ERR_NOFILE ? msg_nofile : msg_toobig );
                return;
            }
            mode = MODE_NAT;
            prog_held = 1;
            mem_set( &cpu, 0, sizeof( cpu ) );
            cpu.eax = regs.eax; cpu.ebx = regs.ebx; cpu.ecx = regs.ecx;
            cpu.edx = regs.edx; cpu.esi = regs.esi; cpu.edi = regs.edi;
            cpu.ebp = regs.ebp; cpu.esp = regs.esp; cpu.eip = regs.eip;
            cpu.efl = regs.efl;
            cpu.cs = regs.cs; cpu.ds = regs.ds; cpu.es = regs.es;
            cpu.ss = regs.ss; cpu.fs = regs.fs; cpu.gs = regs.gs;
            cpu.limit = regs.limit;
            positions_at_ip();
            return;
        }
    }

    /* the work area.  An address given while a native program was held
       was one in its slot, which has just gone: the file goes to CS:100. */
    if ( mode == MODE_NAT ) {
        prog_release();
        new_work();
        where = NULL;
    }
    /* filebuf holds the file's start, from the look for a D32 program */
    if ( file_kind == KIND_EXE && is_mz( filebuf, (u32)got ) ) {
        if ( load_exe( filebuf, (u32)got ) == 0 ) {
            positions_at_ip();
        }
        return;
    }
    {
        /* A .COM - or a file named .EXE that is not one - at 100h with
           the registers EXEC starts a .COM with.  A data file at CS:100
           or L's address, with only BX:CX changed. */
        int is_com = file_kind == KIND_COM || file_kind == KIND_EXE;
        u32 start = (u32)( is_com ? load_seg() : cpu.cs ) * 16 + 0x100;
        u32 at;

        if ( where && !is_com ) {
            start = (u32)where->seg * 16 + where->off;
        }
        if ( start < (u32)work_seg * 16 || start >= (u32)work_seg * 16 + work_len ) {
            coded_error( "df" );        /* outside the work area: read only */
        }
        at = start - (u32)work_seg * 16;
        got = read_whole( file_name, work_buf + at, work_len - at );
        if ( got < 0 ) {
            return;
        }
        if ( is_com ) {
            cpu.cs = cpu.ds = cpu.es = cpu.ss = load_seg();
            cpu.eip = 0x100;
            cpu.esp = 0xFFFE;
            cpu.eax = 0;
            positions_at_ip();
        }
        cpu.ecx = (u32)got & 0xFFFF;
        cpu.ebx = ( (u32)got >> 16 ) & 0xFFFF;
    }
}

/* INT 25h and 26h move a whole logical sector, which can be up to 4K;
   L and W take 512 bytes of each, and W reads the sector first so the
   rest of it is written back as it was */
#define SECTOR_MAX 4096
static u8 sector_buf[SECTOR_MAX];

/* L [address [drive sector count]] */
void cmd_load( void )
{
    ADDR addr;
    u32  drive, sector, count;
    u8   *buf = sector_buf;
    u32  i;

    if ( at_end() ) {
        load_file( NULL );
        return;
    }
    addr.seg = cpu.cs;
    get_addr( &addr, cpu.cs );
    if ( at_end() ) {
        /* L address alone loads the N file there.  A .COM or an .EXE
           goes where EXEC puts it, and any address but CS:100 is an
           error under its last digit, as 6.22's; a native program is
           still handed to the kernel. */
        if ( ( file_kind == KIND_COM || file_kind == KIND_EXE )
          && ( addr.seg != cpu.cs || addr.off != 0x100 ) ) {
            lp--;
            syntax_error();
        }
        load_file( &addr );
        return;
    }
    drive = get_hex( 4 );
    sector = get_hex( 8 );
    count = get_hex( 4 );
    need_end();
    for ( i = 0; i < count; i++ ) {
        u32 j;

        if ( sys_abs_read( (int)drive, sector + i, 1, buf ) ) {
            out_str( msg_rderr );
            out_ch( (char)( 'A' + drive ) );
            out_nl();
            return;
        }
        for ( j = 0; j < 512; j++ ) {
            ADDR at = addr;
            at.off = addr.off + i * 512 + j;
            if ( at.off >= mem_seg_end( at ) ) {
                return;                 /* the rest is past the segment */
            }
            if ( mem_write( at, buf + j, 1 ) ) {
                coded_error( "df" );
            }
        }
    }
}

/* W [address [drive sector count]] */
void cmd_write( void )
{
    ADDR addr;
    u32  drive, sector, count;
    u32  len, done;
    u8   buf[512];
    int  handle, err;

    if ( !at_end() ) {
        addr.seg = cpu.cs;
        get_addr( &addr, cpu.cs );
        if ( !at_end() ) {
            drive = get_hex( 4 );
            sector = get_hex( 8 );
            count = get_hex( 4 );
            need_end();
            for ( ; count; count--, sector++ ) {
                u32 j;
                /* the sector's own bytes past the first 512 stay */
                if ( sys_abs_read( (int)drive, sector, 1, sector_buf ) ) {
                    out_str( msg_wrerr );
                    out_ch( (char)( 'A' + drive ) );
                    out_nl();
                    return;
                }
                for ( j = 0; j < 512; j++ ) {
                    ADDR at = addr;
                    u8 one[1], ok[1];
                    at.off = addr.off + j;
                    mem_get( at, one, 1, ok );
                    sector_buf[j] = one[0];
                }
                if ( sys_abs_write( (int)drive, sector, 1, sector_buf ) ) {
                    out_str( msg_wrerr );
                    out_ch( (char)( 'A' + drive ) );
                    out_nl();
                    return;
                }
                addr.off += 512;
            }
            return;
        }
    } else {
        addr.seg = cpu.cs;
        addr.off = mode == MODE_NAT ? SLOT_IMAGE : 0x100;
    }

    if ( file_kind == KIND_EXE || file_kind == KIND_HEX ) {
        out_str( msg_noexe );
        out_nl();
        return;
    }
    if ( file_name[0] == 0 ) {
        out_str( msg_noname );
        out_nl();
        return;
    }
    len = ( cpu.ebx & 0xFFFF ) * 0x10000UL + ( cpu.ecx & 0xFFFF );
    handle = sys_create( file_name, &err );
    if ( handle < 0 ) {
        out_str( err == ERR_ACCESS ? msg_access : msg_nocreate );
        out_nl();
        return;
    }
    out_str( msg_writing );
    out_hex( len, len > 0xFFFF ? 8 : 5 );
    out_str( msg_bytes );
    out_nl();
    done = 0;
    while ( done < len ) {
        u32 n = len - done;
        u32 wrote;
        ADDR at = addr;
        if ( n > sizeof( buf ) ) {
            n = sizeof( buf );
        }
        if ( mem_is_low( addr ) ) {
            /* on through memory past 64K, as 6.22 stepped its segment;
               past the megabyte there is nothing to read */
            u32 lin = (u32)addr.seg * 16 + addr.off + done;
            if ( lin >= 0x100000UL ) {
                mem_set( buf, 0, n );
            } else {
                at.seg = (u16)( lin >> 4 );
                at.off = lin & 0x0F;
                mem_read( at, buf, n );
            }
        } else {
            at.off = addr.off + done;
            mem_read( at, buf, n );
        }
        if ( sys_write_file( handle, buf, n, &wrote ) || wrote != n ) {
            break;
        }
        done += n;
    }
    sys_close( handle );
    if ( done < len ) {
        sys_delete( file_name );        /* no half-written file left */
        out_str( msg_diskfull );
        out_nl();
    }
}
