/*=====================================================================
 * MKDISK.C - build PM-DOS disk images
 *
 *   mkdisk out.img boot.bin [files...]          1.44M FAT12 boot floppy
 *   mkdisk -hd16 <MB> out.img [opts] [files...] MBR + FAT16 hard disk
 *   mkdisk -hd32 <MB> out.img [opts] [files...] MBR + FAT32 hard disk
 *
 *   hard-disk options:
 *     -mbr <file>   install this 512-byte MBR bootstrap (its first
 *                   1BEh bytes; the partition table and signature are
 *                   ours) and mark the partition active
 *     -vbr <file>   install this 512-byte volume boot record (its
 *                   jump and its code from the end of the BPB up; the
 *                   BPB in between is ours) - stage 1 of the boot
 *     -s2 <file>    stage 2, written into the partition's reserved
 *                   sectors.  Required with -vbr: stage 1 does
 *                   nothing but load it.
 *     -frag         deliberately scatter every file across alternate
 *                   clusters, to prove the loader really walks the
 *                   FAT rather than reading straight through
 *
 * Floppy images take their boot sector from boot.bin.  On floppies,
 * and on bootable hard disks, the first two files are expected to be
 * PMIO.SYS and PMDOS.SYS and get System+Hidden+ReadOnly.  Disks use
 * 63 sectors/track, 16 heads (the matching DOSBox imgmount line is
 * printed on completion).
 *
 * Compiles with Open Watcom (wcl386 -bt=nt), MSVC, or anything sane.
 *===================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SECSIZE     512
#define SPT         63
#define HEADS       16
#define SECPERCYL   (SPT * HEADS)

/* Stage 2 placement - must match S2_* in INC\PMDOS.INC */
#define S2_SECTORS  8
#define S2_START16  1                   /* FAT16: right after the VBR   */
#define S2_START32  9                   /* FAT32: past FSInfo + backup  */

static unsigned char *image;
static long img_secs;

/* -spc: sectors per cluster, forced.  Zero means "pick it from the
   volume size", which is what FORMAT does and what every image built
   before v0.32 got by having no choice. */
static unsigned long opt_spc = 0;

/* ------------------------------------------------------------------ */
static unsigned char *sec(long lba) { return image + lba * SECSIZE; }

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)(v >> 8);
}
static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ------------------------------------------------------------------ */
static void name83(const char *path, unsigned char *out)
{
    const char *base = path, *p;
    int i;

    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/' || *p == ':')
            base = p + 1;

    memset(out, ' ', 11);
    for (i = 0; *base && *base != '.' && i < 8; base++, i++)
        out[i] = (unsigned char)toupper(*base);
    while (*base && *base != '.')
        base++;
    if (*base == '.') {
        base++;
        for (i = 8; *base && i < 11; base++, i++)
            out[i] = (unsigned char)toupper(*base);
    }
}

static void stamp(unsigned char *de)
{
    de[22] = 0x00; de[23] = 0x60;                   /* 12:00:00       */
    de[24] = (unsigned char)(6 | ((8 & 7) << 5));   /* 2026-08-06     */
    de[25] = (unsigned char)(((2026 - 1980) << 1) | (8 >> 3));
}

/*=====================================================================
 * VFAT long-file-name test entries (the v0.11 read side needs disks
 * that already CARRY long names before the kernel can write any).
 * This C code is also the reference for the Phase-2 assembly: the
 * checksum, the fragment layout, the FFFF padding, all of it.
 *===================================================================*/
static unsigned char lfn_cksum(const unsigned char *n)
{
    unsigned char s = 0;
    int i;
    for (i = 0; i < 11; i++)
        s = (unsigned char)(((s & 1) << 7) + (s >> 1) + n[i]);
    return s;
}

/* write the LFN chain for lname in front of the 8.3 entry to come.
   de = where the chain starts; returns the number of entries used.
   Fragments go last-part-first: sequence N|40h down to 1. */
static int lfn_chain(unsigned char *de, const char *lname,
                     const unsigned char *short11)
{
    static const int ofs[13] = {1,3,5,7,9,14,16,18,20,22,24,28,30};
    int len = (int)strlen(lname), nfrag = (len + 12) / 13, f, i;
    unsigned char ck = lfn_cksum(short11);

    for (f = 0; f < nfrag; f++) {
        int seq = nfrag - f;
        unsigned char *e = de + f * 32;
        memset(e, 0, 32);
        for (i = 0; i < 13; i++) {          /* padding first: FFFF */
            e[ofs[i]] = 0xFF; e[ofs[i] + 1] = 0xFF;
        }
        e[0]  = (unsigned char)(seq | (f == 0 ? 0x40 : 0));
        e[11] = 0x0F;
        e[13] = ck;
        for (i = 0; i < 13; i++) {
            int pos = (seq - 1) * 13 + i;
            if (pos > len) break;           /* the rest stays FFFF */
            e[ofs[i]]     = (unsigned char)(pos == len ? 0 : lname[pos]);
            e[ofs[i] + 1] = 0;              /* 0000 terminates */
        }
    }
    return nfrag;
}

/* FAT12 entry write (the floppy planting needs two of its own) */
static void fat12_set(unsigned char *fat, int c, int v)
{
    int off = c + c / 2;
    if (c & 1) {
        fat[off]     = (unsigned char)((fat[off] & 0x0F) | ((v << 4) & 0xF0));
        fat[off + 1] = (unsigned char)((v >> 4) & 0xFF);
    } else {
        fat[off]     = (unsigned char)(v & 0xFF);
        fat[off + 1] = (unsigned char)((fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F));
    }
}

/* load a signed 512-byte boot sector file */
static int load_bootsec(const char *path, unsigned char *buf)
{
    FILE *f = fopen(path, "rb");
    long  n;
    if (!f) { perror(path); return 1; }
    n = fread(buf, 1, SECSIZE, f);
    fclose(f);
    if (n != SECSIZE || buf[510] != 0x55 || buf[511] != 0xAA) {
        fprintf(stderr, "%s: not a signed 512-byte boot sector\n", path);
        return 1;
    }
    return 0;
}

/*=====================================================================
 * The classic 1.44M floppy
 *===================================================================*/
static int build_floppy(int argc, char **argv)
{
    static unsigned char fat[9 * SECSIZE];
    FILE *f;
    long  n;
    int   arg, clus = 2, dirent;
    const long ROOTLBA = 19, DATALBA = 33, NCLUS = (2880 - 33);

    img_secs = 2880;
    image = (unsigned char *)calloc(img_secs, SECSIZE);
    if (!image) { fprintf(stderr, "out of memory\n"); return 1; }

    memset(fat, 0, sizeof fat);
    fat[0] = 0xF0; fat[1] = 0xFF; fat[2] = 0xFF;

    /* volume label first */
    {
        unsigned char *de = sec(ROOTLBA);
        memcpy(de, "PM-DOS     ", 11);
        de[11] = 0x08;
        stamp(de);
    }
    dirent = 1;

    f = fopen(argv[2], "rb");
    if (!f) { perror(argv[2]); return 1; }
    n = fread(image, 1, SECSIZE, f);
    fclose(f);
    if (n != SECSIZE || image[510] != 0x55 || image[511] != 0xAA) {
        fprintf(stderr, "%s: not a signed 512-byte boot sector\n", argv[2]);
        return 1;
    }

    for (arg = 3; arg < argc; arg++) {
        unsigned char *de;
        long size, left;
        int  first = 0, prev = 0;

        f = fopen(argv[arg], "rb");
        if (!f) { perror(argv[arg]); return 1; }
        fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);

        left = size;
        while (left > 0) {
            if (clus >= NCLUS + 2) { fprintf(stderr, "disk full\n"); fclose(f); return 1; }
            n = fread(sec(DATALBA + (long)(clus - 2)), 1, SECSIZE, f);
            if (n <= 0) break;
            if (prev) {                     /* FAT12 link prev -> clus */
                int off = prev + prev / 2;
                if (prev & 1) {
                    fat[off]   = (unsigned char)((fat[off] & 0x0F) | ((clus << 4) & 0xF0));
                    fat[off+1] = (unsigned char)((clus >> 4) & 0xFF);
                } else {
                    fat[off]   = (unsigned char)(clus & 0xFF);
                    fat[off+1] = (unsigned char)((fat[off+1] & 0xF0) | ((clus >> 8) & 0x0F));
                }
            } else
                first = clus;
            prev = clus++;
            left -= n;
        }
        fclose(f);
        if (prev) {                         /* terminate the chain */
            int off = prev + prev / 2;
            if (prev & 1) {
                fat[off]   = (unsigned char)((fat[off] & 0x0F) | 0xF0);
                fat[off+1] = 0xFF;
            } else {
                fat[off]   = 0xFF;
                fat[off+1] = (unsigned char)(fat[off+1] | 0x0F);
            }
        }

        de = sec(ROOTLBA) + dirent * 32;
        name83(argv[arg], de);
        de[11] = (unsigned char)(arg - 3 < 2 ? 0x07 : 0x20);
        stamp(de);
        put16(de + 26, (unsigned)first);
        put32(de + 28, (unsigned long)size);
        dirent++;
        printf("  %-14s %7ld bytes, cluster %d..%d\n", argv[arg], size, first, clus - 1);
    }

    /* ---- long-file-name test set (v0.11 read side) ----------------
       Two long-named files sharing one content cluster, and a long-
       named directory holding NESTED.TXT (same content again - a
       read-only test disk can crosslink without a care).  This is
       what LFNTEST.BAT chews on. */
    {
        static const char content[] = "This file wears a long name.\r\n";
        int csize = (int)sizeof content - 1;
        int cclus = clus++;
        int dclus = clus++;
        unsigned char *de, *dd;

        memcpy(sec(DATALBA + (long)(cclus - 2)), content, csize);
        fat12_set(fat, cclus, 0xFFF);
        fat12_set(fat, dclus, 0xFFF);

        de = sec(ROOTLBA) + dirent * 32;
        dirent += lfn_chain(de, "Long File Name.txt",
                            (const unsigned char *)"LONGFI~1TXT");
        de = sec(ROOTLBA) + dirent * 32;
        memcpy(de, "LONGFI~1TXT", 11);
        de[11] = 0x20; stamp(de);
        put16(de + 26, (unsigned)cclus);
        put32(de + 28, (unsigned long)csize);
        dirent++;

        de = sec(ROOTLBA) + dirent * 32;
        dirent += lfn_chain(de, "MixedCaseName.txt",
                            (const unsigned char *)"MIXEDC~1TXT");
        de = sec(ROOTLBA) + dirent * 32;
        memcpy(de, "MIXEDC~1TXT", 11);
        de[11] = 0x20; stamp(de);
        put16(de + 26, (unsigned)cclus);
        put32(de + 28, (unsigned long)csize);
        dirent++;

        de = sec(ROOTLBA) + dirent * 32;
        dirent += lfn_chain(de, "Long Directory",
                            (const unsigned char *)"LONGDI~1   ");
        de = sec(ROOTLBA) + dirent * 32;
        memcpy(de, "LONGDI~1   ", 11);
        de[11] = 0x10; stamp(de);
        put16(de + 26, (unsigned)dclus);
        dirent++;

        dd = sec(DATALBA + (long)(dclus - 2));
        memset(dd, 0, SECSIZE);
        memcpy(dd, ".          ", 11);
        dd[11] = 0x10; stamp(dd);
        put16(dd + 26, (unsigned)dclus);
        memcpy(dd + 32, "..         ", 11);
        dd[32 + 11] = 0x10; stamp(dd + 32);
        memcpy(dd + 64, "NESTED  TXT", 11);
        dd[64 + 11] = 0x20; stamp(dd + 64);
        put16(dd + 64 + 26, (unsigned)cclus);
        put32(dd + 64 + 28, (unsigned long)csize);

        printf("  + LFN test set: 2 long-named files, 1 long-named dir\n");
    }

    memcpy(sec(1),  fat, sizeof fat);
    memcpy(sec(10), fat, sizeof fat);
    return 0;
}

/*=====================================================================
 * FAT16 / FAT32 hard disks
 *===================================================================*/
static int build_hd(int fat32, long mb, int argc, char **argv, int firstfile,
                    const char *mbrfile, const char *vbrfile,
                    const char *s2file, int frag, int active)
{
    unsigned long cyls, total, plba, psec;
    unsigned long spc, rsvd, rootent, rootsecs, spf, nclus, datalba, rootlba;
    unsigned long clus, dirent = 0, bpbsize;
    unsigned long lastfirst = 0;
    long lastsize = 0;
    /* FAT32: clusters of root dir.  8, not 4: at one sector per cluster
       four clusters is 64 entries, and the test image list went past it
       the day MCBTEST and MEM622 were added.  A FAT32 root is a cluster
       chain and could grow on demand; this is the smaller change, and
       the failure it prevents was at least loud - "root full" and a
       non-zero exit, rather than an image quietly missing files. */
    unsigned long rootclus = 8;
    unsigned char *vbr, *fatp;
    unsigned char code[SECSIZE];
    FILE *f;
    long  n;
    int   arg, sysfiles = 0;

    if (vbrfile && !s2file) {
        fprintf(stderr, "-vbr needs -s2: stage 1 only loads stage 2\n");
        return 1;
    }
    bpbsize = fat32 ? 0x5A : 0x3E;

    cyls  = (unsigned long)mb * 2048 / SECPERCYL;
    total = cyls * SECPERCYL;
    plba  = SPT;                        /* partition starts on track 1 */
    psec  = total - plba;

    img_secs = (long)total;
    image = (unsigned char *)calloc(img_secs, SECSIZE);
    if (!image) { fprintf(stderr, "out of memory\n"); return 1; }

    if (fat32) {
        spc = 1; rsvd = 32; rootent = 0; rootsecs = 0;
        /* iterate spf until stable */
        spf = 1;
        for (n = 0; n < 8; n++) {
            nclus = (psec - rsvd - 2 * spf) / spc;
            spf   = ((nclus + 2) * 4 + SECSIZE - 1) / SECSIZE;
        }
        nclus = (psec - rsvd - 2 * spf) / spc;
        if (nclus < 65525) {
            fprintf(stderr, "image too small for FAT32 (%lu clusters)\n", nclus);
            return 1;
        }
    } else {
        /* stage 2 lives in the reserved area, so make room for it -
           and a -active (SYS-target) disk must have that room even
           though nothing is written there yet, or SYS would have to
           refuse it */
        rsvd = (s2file || active) ? 1 + S2_SECTORS : 1;
        rootent = 512; rootsecs = rootent * 32 / SECSIZE;

        /* CLUSTER SIZE BY VOLUME SIZE, the way FORMAT does it: the
           smallest power of two that brings the count inside FAT16's
           65524.  This was a flat spc = 2, which capped the tool at
           about 67MB - so every image it has ever built has 1K
           clusters, and the kernel's own ceiling on cluster size (8K
           until v0.32) could not be reached by any test disk here.
           The 850MB drive that found it needs 16K clusters, which is
           four times what the kernel would mount.  A limit no test can
           reach is not covered by having tests. */
        for (spc = opt_spc ? opt_spc : 2; ; spc *= 2) {
            spf = 1;
            for (n = 0; n < 8; n++) {
                nclus = (psec - rsvd - 2 * spf - rootsecs) / spc;
                spf   = ((nclus + 2) * 2 + SECSIZE - 1) / SECSIZE;
            }
            nclus = (psec - rsvd - 2 * spf - rootsecs) / spc;
            if (opt_spc || nclus <= 65524 || spc >= 64) break;
        }
        if (nclus < 4085 || nclus > 65524) {
            fprintf(stderr, "cluster count %lu out of FAT16 range\n", nclus);
            return 1;
        }
    }
    rootlba = plba + rsvd + 2 * spf;                /* FAT16 root area  */
    datalba = rootlba + rootsecs;                   /* first data LBA   */

    /*--- MBR ---------------------------------------------------------*/
    {
        unsigned char *m = sec(0), *pe = m + 0x1BE;
        unsigned long endcyl = cyls - 1;

        if (mbrfile) {                              /* real bootstrap  */
            if (load_bootsec(mbrfile, code)) return 1;
            memcpy(m, code, 0x1BE);                 /* code only       */
        }
        /* -active marks the partition bootable even without boot code:
           the SYS test target - a disk the MBR will chain into once
           SYS has put a real VBR there */
        pe[0] = (unsigned char)((vbrfile || active) ? 0x80 : 0x00);
        pe[1] = 1; pe[2] = 1; pe[3] = 0;            /* CHS start 0/1/1 */
        pe[4] = (unsigned char)(fat32 ? 0x0B : 0x06);
        pe[5] = HEADS - 1;                          /* CHS end: head   */
        pe[6] = (unsigned char)(SPT | ((endcyl >> 2) & 0xC0));
        pe[7] = (unsigned char)(endcyl & 0xFF);
        put32(pe + 8,  plba);
        put32(pe + 12, psec);
        m[510] = 0x55; m[511] = 0xAA;
    }

    /*--- VBR ---------------------------------------------------------*/
    vbr = sec(plba);
    if (vbrfile) {                                  /* real boot code  */
        if (load_bootsec(vbrfile, code)) return 1;
        memcpy(vbr, code, 3);                       /* the jump ...    */
        memcpy(vbr + bpbsize, code + bpbsize,       /* ... and the     */
               SECSIZE - bpbsize);                  /* code past the BPB */
    } else {
        vbr[0] = 0xEB; vbr[1] = 0xFE; vbr[2] = 0x90;  /* jmp $         */
    }
    memcpy(vbr + 3, "PM-DOS  ", 8);
    put16(vbr + 11, SECSIZE);
    vbr[13] = (unsigned char)spc;
    put16(vbr + 14, (unsigned)rsvd);
    vbr[16] = 2;                                    /* FAT copies      */
    put16(vbr + 17, (unsigned)rootent);
    put16(vbr + 19, 0);                             /* 16-bit total: no */
    vbr[21] = 0xF8;                                 /* media           */
    put16(vbr + 22, fat32 ? 0 : (unsigned)spf);
    put16(vbr + 24, SPT);
    put16(vbr + 26, HEADS);
    put32(vbr + 28, plba);                          /* hidden sectors  */
    put32(vbr + 32, psec);                          /* 32-bit total    */
    if (fat32) {
        put32(vbr + 36, spf);                       /* sectors per FAT */
        put32(vbr + 44, 2);                         /* root cluster    */
        put16(vbr + 48, 1);                         /* FSInfo sector   */
        put16(vbr + 50, 6);                         /* backup boot     */
        vbr[64] = 0x80; vbr[66] = 0x29;
        memcpy(vbr + 71, "PM-DOS     ", 11);
        memcpy(vbr + 82, "FAT32   ", 8);
    } else {
        vbr[36] = 0x80; vbr[38] = 0x29;
        memcpy(vbr + 43, "PM-DOS     ", 11);
        memcpy(vbr + 54, "FAT16   ", 8);
    }
    vbr[510] = 0x55; vbr[511] = 0xAA;

    if (fat32) {
        unsigned char *fs = sec(plba + 1);          /* FSInfo          */
        put32(fs + 0,   0x41615252UL);
        put32(fs + 484, 0x61417272UL);
        put32(fs + 488, 0xFFFFFFFFUL);              /* free: unknown   */
        put32(fs + 492, 0xFFFFFFFFUL);
        fs[510] = 0x55; fs[511] = 0xAA;
        memcpy(sec(plba + 6), vbr, SECSIZE);        /* backup boot     */
    }

    /*--- stage 2, into the reserved sectors --------------------------*/
    if (s2file) {
        long s2len;
        f = fopen(s2file, "rb");
        if (!f) { perror(s2file); return 1; }
        fseek(f, 0, SEEK_END); s2len = ftell(f); fseek(f, 0, SEEK_SET);
        if (s2len > S2_SECTORS * SECSIZE) {
            fprintf(stderr, "%s: %ld bytes, over the %d reserved sectors\n",
                    s2file, s2len, S2_SECTORS);
            fclose(f); return 1;
        }
        if (fread(sec(plba + (fat32 ? S2_START32 : S2_START16)), 1,
                  (size_t)s2len, f) != (size_t)s2len) {
            perror(s2file); fclose(f); return 1;
        }
        fclose(f);
        printf("  stage 2         %7ld bytes at reserved sector %d\n",
               s2len, fat32 ? S2_START32 : S2_START16);
    }

    /*--- FAT[0..1] and, on FAT32, the root directory chain ------------*/
    fatp = sec(plba + rsvd);
    if (fat32) {
        unsigned long i;
        put32(fatp + 0, 0x0FFFFFF8UL);
        put32(fatp + 4, 0x0FFFFFFFUL);
        /* A FAT32 root is a chain like any other directory, not a
           fixed area - one cluster of it holds only spc*16 entries,
           which a boot disk outgrows immediately.  Allocate it
           contiguously from cluster 2 so a dir entry's byte offset
           still maps straight onto datalba. */
        for (i = 0; i < rootclus - 1; i++)
            put32(fatp + (2 + i) * 4, 2 + i + 1);
        put32(fatp + (2 + rootclus - 1) * 4, 0x0FFFFFFFUL);
    } else {
        put16(fatp + 0, 0xFFF8);
        put16(fatp + 2, 0xFFFF);
    }

    /*--- volume label, then the files, into the root -----------------*/
    {
        unsigned char *de = fat32 ? sec(datalba) : sec(rootlba);
        memcpy(de, "PM-DOS     ", 11);
        de[11] = 0x08;
        stamp(de);
        dirent = 1;
    }

    clus = fat32 ? 2 + rootclus : 2;                /* past the root   */
    lastfirst = 0; lastsize = 0;
    for (arg = firstfile; arg < argc; arg++) {
        unsigned char *de;
        unsigned long first = 0, prev = 0, maxent;
        long size, left;

        maxent = fat32 ? rootclus * spc * 16 : rootent;
        if (dirent >= maxent) { fprintf(stderr, "root full\n"); return 1; }

        f = fopen(argv[arg], "rb");
        if (!f) { perror(argv[arg]); return 1; }
        fseek(f, 0, SEEK_END); size = ftell(f); fseek(f, 0, SEEK_SET);

        left = size;
        while (left > 0) {
            unsigned long s;
            if (clus >= nclus + 2) { fprintf(stderr, "disk full\n"); fclose(f); return 1; }
            for (s = 0; s < spc && left > 0; s++) {
                n = fread(sec(datalba + (clus - 2) * spc + s), 1, SECSIZE, f);
                if (n <= 0) break;
                left -= n;
            }
            if (prev) {
                if (fat32) put32(fatp + prev * 4, clus);
                else       put16(fatp + prev * 2, (unsigned)clus);
            } else
                first = clus;
            prev = clus;
            /* -frag leaves every other cluster free, so a loader that
               reads straight through instead of following the chain
               gets garbage and says so */
            clus += frag ? 2 : 1;
        }
        fclose(f);
        if (prev) {
            if (fat32) put32(fatp + prev * 4, 0x0FFFFFFFUL);
            else       put16(fatp + prev * 2, 0xFFFF);
        }

        de = (fat32 ? sec(datalba) : sec(rootlba)) + dirent * 32;
        name83(argv[arg], de);
        /* on a bootable image PMIO.SYS and PMDOS.SYS are system files */
        de[11] = (unsigned char)(vbrfile && sysfiles < 2 ? 0x07 : 0x20);
        sysfiles++;
        stamp(de);
        put16(de + 26, (unsigned)(first & 0xFFFF));
        put16(de + 20, (unsigned)(first >> 16));
        put32(de + 28, (unsigned long)size);
        dirent++;
        printf("  %-14s %7ld bytes, cluster %lu..%lu\n", argv[arg], size, first, clus - 1);
        lastfirst = first; lastsize = size;
    }

    /* ---- long-file-name test files (v0.11 read side) --------------
       Same two names as the floppy, crosslinked onto the LAST file's
       chain: no new clusters, just directory entries to read. */
    if (lastfirst) {
        unsigned long maxent = fat32 ? rootclus * spc * 16 : rootent;
        if (dirent + 6 <= maxent) {
            unsigned char *rb = fat32 ? sec(datalba) : sec(rootlba);
            unsigned char *de;

            de = rb + dirent * 32;
            dirent += lfn_chain(de, "Long File Name.txt",
                                (const unsigned char *)"LONGFI~1TXT");
            de = rb + dirent * 32;
            memcpy(de, "LONGFI~1TXT", 11);
            de[11] = 0x20; stamp(de);
            put16(de + 26, (unsigned)(lastfirst & 0xFFFF));
            put16(de + 20, (unsigned)(lastfirst >> 16));
            put32(de + 28, (unsigned long)lastsize);
            dirent++;

            de = rb + dirent * 32;
            dirent += lfn_chain(de, "MixedCaseName.txt",
                                (const unsigned char *)"MIXEDC~1TXT");
            de = rb + dirent * 32;
            memcpy(de, "MIXEDC~1TXT", 11);
            de[11] = 0x20; stamp(de);
            put16(de + 26, (unsigned)(lastfirst & 0xFFFF));
            put16(de + 20, (unsigned)(lastfirst >> 16));
            put32(de + 28, (unsigned long)lastsize);
            dirent++;

            printf("  + LFN test set: 2 long-named files\n");
        }
    }

    /* second FAT copy */
    memcpy(sec(plba + rsvd + spf), fatp, spf * SECSIZE);

    printf("FAT%d: %lu sectors, %lu clusters, %lu sectors/FAT, %lu reserved%s\n",
           fat32 ? 32 : 16, total, nclus, spf, rsvd,
           frag ? ", FRAGMENTED" : "");
    printf("DOSBox: imgmount 2 <img> -size 512,%d,%d,%lu -t hdd -fs none%s\n",
           SPT, HEADS, cyls, vbrfile ? " (then: boot -l c)" : "");
    if (vbrfile) printf("  bootable: MBR + FAT%d VBR + stage 2\n", fat32 ? 32 : 16);
    return 0;
}

/* ------------------------------------------------------------------ */
int main(int argc, char **argv)
{
    const char *out;
    FILE *f;
    int r;

    if (argc >= 4 && (!strcmp(argv[1], "-hd16") || !strcmp(argv[1], "-hd32"))) {
        long mb = atol(argv[2]);
        const char *mbrfile = NULL, *vbrfile = NULL, *s2file = NULL;
        int first = 4, frag = 0, active = 0;

        /* 2048MB is FAT16's own ceiling with 32K clusters.  The old cap
           was 500, which is below the 512MB line where FORMAT stops
           being able to use 8K clusters - so the tool could not build
           a disk that reaches the kernel's cluster-size limit even
           after build_hd learned to scale spc.  The whole image is
           held in memory, so a big one wants the RAM for it. */
        if (mb < 4 || mb > 2048) { fprintf(stderr, "size out of range\n"); return 1; }
        out = argv[3];
        while (first < argc && argv[first][0] == '-') {
            if (!strcmp(argv[first], "-frag"))   { frag = 1; first++; continue; }
            if (!strcmp(argv[first], "-active")) { active = 1; first++; continue; }
            if (first + 1 >= argc) break;
            if (!strcmp(argv[first], "-mbr"))      mbrfile = argv[first + 1];
            else if (!strcmp(argv[first], "-vbr")) vbrfile = argv[first + 1];
            else if (!strcmp(argv[first], "-s2"))  s2file  = argv[first + 1];
            else if (!strcmp(argv[first], "-spc")) opt_spc = atol(argv[first + 1]);
            else break;
            first += 2;
        }
        r = build_hd(argv[1][3] == '3', mb, argc, argv, first,
                     mbrfile, vbrfile, s2file, frag, active);
    } else if (argc >= 3) {
        out = argv[1];
        r = build_floppy(argc, argv);
    } else {
        fprintf(stderr,
            "usage: mkdisk out.img boot.bin [files...]\n"
            "       mkdisk -hd16 <MB> out.img [opts] [files...]\n"
            "       mkdisk -hd32 <MB> out.img [opts] [files...]\n"
            "opts:  -mbr <file> -vbr <file> -s2 <file> -frag -spc <n>\n"
            "\n"
            "-spc forces sectors per cluster instead of letting the size\n"
            "pick it.  It exists so the kernel's cluster-size ceiling can\n"
            "be tested WITHOUT an 800MB image: FAT16 needs only 4085\n"
            "clusters, so -hd16 80 -spc 32 gives 16K clusters in 80MB.\n");
        return 1;
    }
    if (r) return r;

    f = fopen(out, "wb");
    if (!f) { perror(out); return 1; }
    if (fwrite(image, SECSIZE, img_secs, f) != (size_t)img_secs) {
        perror(out); fclose(f); return 1;
    }
    fclose(f);
    printf("%s: %ld sectors\n", out, img_secs);
    return 0;
}
