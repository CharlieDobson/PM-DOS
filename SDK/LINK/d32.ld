/*
 * D32.LD - the GNU ld (and LLVM lld) script for a PM-DOS program,
 * GCC or Clang
 *
 *   i686-elf-gcc -nostdlib -T d32.ld -o prog.elf prog.o -L<lib> -lpmc
 *   i686-elf-objcopy -O binary prog.elf PROG.EXE
 *
 * The image is linked for 2000h, where the kernel loads it, with the
 * D32 header first.  The BSS is not in the file: the header tells the
 * kernel how many bytes past the end of the file to zero, so the size
 * is measured from the end of the data, not from where .bss starts.
 *
 * --defsym=__d32_stack_size=<bytes> sets the stack, 64K by default.
 */
OUTPUT_FORMAT( "elf32-i386" )
ENTRY( _d32_start )
EXTERN( _d32_start )

SECTIONS
{
    . = 0x2000;
    .text : {
        KEEP( *(.d32head) )
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
    }
    .data : {
        __d32_init_start = .;
        KEEP( *(SORT( .init_array.* )) )
        KEEP( *(.init_array .ctors) )
        __d32_init_end = .;
        __d32_fini_start = .;
        KEEP( *(SORT( .fini_array.* )) )
        KEEP( *(.fini_array .dtors) )
        __d32_fini_end = .;
        *(.data .data.*)
        LONG( 0 )
    }
    __d32_image_end = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN( 16 );
    __d32_bss_end = .;
    __d32_bss_size = __d32_bss_end - __d32_image_end;
    PROVIDE( __d32_stack_size = 0x10000 );
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu.*)
    }
}
