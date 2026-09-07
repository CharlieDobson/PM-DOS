/*=====================================================================
 * FSIZE.C - assert the size of a built binary, for BUILD.CMD
 *
 *     FSIZE -eq  <bytes> <file> [note]     exact size
 *     FSIZE -max <bytes> <file> [note]     size ceiling
 *
 * Exits 0 when the file satisfies the assertion, 1 when it does not
 * (printing what it actually was, plus the optional note), and 2 when
 * the file cannot be opened at all.  BUILD.CMD calls it as
 *
 *     TOOLS\FSIZE.EXE -eq 512 BIN\BOOT.BIN || goto :fail
 *
 * Exists because these assertions used to be written with cmd.exe's
 * %%~zF, and %%~zF is a command EXTENSION: Windows NT 3.51's cmd.exe
 * expands it to the literal text "%~zF", so every size check silently
 * became the comparison "%~zF"=="512" and failed the build on the one
 * machine the checks were least able to be debugged on.  A four-line
 * host tool works on every cmd.exe there has ever been.
 *
 * The sizes it guards are real ceilings, not tidiness: a boot sector
 * that is not exactly 512 bytes will not boot, IORM.BIN is blitted as
 * a fixed 4K, and the stub copies exactly IO_PM_MAX bytes of IOPM.BIN
 * up past 1MB - anything past that is left behind silently.
 *
 * Built with:  wcl386 -q -bt=nt -fe=fsize.exe fsize.c
 *=====================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *f;
    long  size;
    int   exact;

    if (argc < 4) {
        fprintf(stderr, "usage: FSIZE -eq|-max <bytes> <file> [note]\n");
        return 2;
    }

    if      (strcmp(argv[1], "-eq")  == 0) exact = 1;
    else if (strcmp(argv[1], "-max") == 0) exact = 0;
    else {
        fprintf(stderr, "FSIZE: unknown option %s\n", argv[1]);
        return 2;
    }

    f = fopen(argv[3], "rb");
    if (f == NULL) {
        printf("%s could not be opened - it was not built\n", argv[3]);
        return 2;
    }
    fseek(f, 0L, SEEK_END);
    size = ftell(f);
    fclose(f);

    if (exact ? (size == atol(argv[2])) : (size <= atol(argv[2])))
        return 0;

    printf("%s is %ld bytes, %s %s", argv[3], size,
           exact ? "not" : "over the limit of", argv[2]);
    if (argc > 4) printf(" - %s", argv[4]);
    printf("\n");
    return 1;
}
