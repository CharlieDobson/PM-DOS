/*
 * D32COFF.LD - the linker script for a PM-DOS program built with
 * DJGPP's GCC
 *
 *   i586-pc-msdosdjgpp-gcc -nostdlib -T d32coff.ld -o prog.cof prog.o
 *       -L<lib> -lpmc
 *   i586-pc-msdosdjgpp-objcopy -O binary prog.cof PROG.EXE
 *
 * D32.LD's layout in DJGPP's COFF, which has .text, .data and .bss
 * and nothing else, and an underscore in front of every C name.
 *
 * --defsym=___d32_stack_size=<bytes> sets the stack, 64K by default.
 */
OUTPUT_FORMAT( "coff-go32" )
ENTRY( __d32_start )
EXTERN( __d32_start )

SECTIONS
{
    . = 0x2000;
    .text : {
        KEEP( *(.d32head) )
        *(.text .text.*)
        *(.rdata .rdata.* .rodata .rodata.*)
    }
    .data : {
        ___d32_init_start = .;
        KEEP( *(SORT( .ctors.* )) )
        KEEP( *(.ctors) )
        ___d32_init_end = .;
        ___d32_fini_start = .;
        KEEP( *(SORT( .dtors.* )) )
        KEEP( *(.dtors) )
        ___d32_fini_end = .;
        *(.data .data.*)
        LONG( 0 )
    }
    ___d32_image_end = .;
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }
    . = ALIGN( 16 );
    ___d32_bss_end = .;
    ___d32_bss_size = ___d32_bss_end - ___d32_image_end;
    PROVIDE( ___d32_stack_size = 0x10000 );
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
        *(.gnu.*)
    }
}
