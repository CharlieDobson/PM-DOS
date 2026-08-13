/*=====================================================================
 * MKMULTI.C - build a multi-partition test disk.
 *
 *   mkmulti out.img [-mbr <file>]
 *
 * The partition scan added in 0.26 has code paths that a single-volume
 * image cannot reach: a second primary, an extended container, a chain
 * of logical drives inside it, a non-FAT partition that has to be
 * skipped, and a hidden one that has to be ignored.  Every one of them
 * is a chance to walk off the end of a disk, and none of them was ever
 * executed by the images MKDISK builds.
 *
 * So this makes a disk with all of it:
 *
 *   partition 1   06h  FAT16B, 33MB          -> the first primary
 *   partition 2   01h  FAT12,   8MB          -> a second primary
 *   partition 3   83h  not FAT               -> must be skipped
 *   partition 4   05h  extended container
 *                   +-- 04h  FAT16, 16MB     -> first logical
 *                   +-- 06h  FAT16B, 16MB    -> second logical
 *                   +-- 16h  hidden FAT16B   -> must NOT get a letter
 *
 * Every volume gets a distinctive label and one file named after
 * itself, so that a wrong answer is visible rather than merely
 * plausible: if the letters come out shuffled, DIR says so.
 *
 * DOS assigned letters as: the first primary of each disk, then all
 * logicals, then the remaining primaries.  On this disk that is
 *   C: partition 1,  D: first logical,  E: second logical,
 *   F: partition 2
 * which is deliberately NOT the order they appear in the table.
 *
 * Built by BUILD.CMD with wcl386 -bt=nt.
 *===================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SECSIZE   512
#define SPT       63
#define HEADS     16
#define SECPERCYL (SPT * HEADS)

static unsigned char *image;
static long img_secs;

static unsigned char *sec(long lba) { return image + (long)lba * SECSIZE; }

static void put16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void put32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xFF);
    p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF);
    p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* A CHS triple for the partition table.  Frozen at the maximum once
   the address stops fitting, which is what every tool has done since
   disks passed 8GB - the LBA fields are the real answer and these are
   there so that something reading only CHS sees a legal value rather
   than a wrapped one. */
static void putchs(unsigned char *p, unsigned long lba)
{
    unsigned long c = lba / SECPERCYL;
    unsigned long h = (lba / SPT) % HEADS;
    unsigned long s = (lba % SPT) + 1;
    if (c > 1023) { c = 1023; h = HEADS - 1; s = SPT; }
    p[0] = (unsigned char)h;
    p[1] = (unsigned char)(s | ((c >> 2) & 0xC0));
    p[2] = (unsigned char)(c & 0xFF);
}

static void fat12_set(unsigned char *fat, int c, int v)
{
    int off = c + (c / 2);
    if (c & 1) {
        fat[off]     = (unsigned char)((fat[off] & 0x0F) | ((v << 4) & 0xF0));
        fat[off + 1] = (unsigned char)((v >> 4) & 0xFF);
    } else {
        fat[off]     = (unsigned char)(v & 0xFF);
        fat[off + 1] = (unsigned char)((fat[off + 1] & 0xF0) | ((v >> 8) & 0x0F));
    }
}

static void stamp(unsigned char *de)
{
    put16(de + 22, (12 << 11) | (0 << 5) | 0);      /* 12:00:00 */
    put16(de + 24, ((2026 - 1980) << 9) | (8 << 5) | 9);
}

static void name83(const char *s, unsigned char *out)
{
    int i = 0, j = 0;
    memset(out, ' ', 11);
    while (s[i] && s[i] != '.' && j < 8) out[j++] = (unsigned char)s[i++];
    while (s[i] && s[i] != '.') i++;
    if (s[i] == '.') {
        i++; j = 8;
        while (s[i] && j < 11) out[j++] = (unsigned char)s[i++];
    }
}

/*---------------------------------------------------------------------
 * format_volume - lay a FAT12 or FAT16 volume down at start..start+secs.
 *
 * fat12 chooses the flavour, but ONLY as a request: the cluster count
 * decides what the volume really is, so the sectors-per-cluster is
 * picked to land the count inside the range asked for and the caller
 * is told if it could not be done.  That is the same rule the kernel
 * mounts by, and building a test disk that disagrees with it would
 * test nothing.
 *-------------------------------------------------------------------*/
static int format_volume(unsigned long start, unsigned long secs,
                         int fat12, const char *label, const char *fname,
                         const char *text)
{
    unsigned long spc, rsvd = 1, nfats = 2, rootent = 512, rootsecs;
    unsigned long spf, nclus, rootlba, datalba, i;
    unsigned char *vbr, *fatp, *de;

    rootsecs = rootent * 32 / SECSIZE;

    /* Try each legal cluster size until the count lands in range. */
    for (spc = 1; spc <= 64; spc <<= 1) {
        spf = 1;
        for (i = 0; i < 12; i++) {
            nclus = (secs - rsvd - nfats * spf - rootsecs) / spc;
            if (fat12) spf = ((nclus + 2) * 3 / 2 + SECSIZE - 1) / SECSIZE;
            else       spf = ((nclus + 2) * 2 + SECSIZE - 1) / SECSIZE;
        }
        nclus = (secs - rsvd - nfats * spf - rootsecs) / spc;
        if (fat12) { if (nclus < 4085) break; }
        else       { if (nclus >= 4085 && nclus < 65525) break; }
    }
    if (spc > 64) {
        fprintf(stderr, "cannot make a %s volume of %lu sectors\n",
                fat12 ? "FAT12" : "FAT16", secs);
        return 1;
    }

    rootlba = start + rsvd + nfats * spf;
    datalba = rootlba + rootsecs;

    /*--- boot sector -------------------------------------------------*/
    vbr = sec(start);
    vbr[0] = 0xEB; vbr[1] = 0xFE; vbr[2] = 0x90;    /* jmp $ */
    memcpy(vbr + 3, "PM-DOS  ", 8);
    put16(vbr + 0x0B, SECSIZE);
    vbr[0x0D] = (unsigned char)spc;
    put16(vbr + 0x0E, (unsigned)rsvd);
    vbr[0x10] = (unsigned char)nfats;
    put16(vbr + 0x11, (unsigned)rootent);
    /* TOTSEC16 only if it fits, exactly as DOS 3.31 specified: a
       volume over 65535 sectors MUST leave the 16-bit field zero and
       use the 32-bit one, and a reader that checks them the other way
       round sees a 33MB volume as a 512-byte one. */
    if (secs < 0x10000) put16(vbr + 0x13, (unsigned)secs);
    else                put16(vbr + 0x13, 0);
    vbr[0x15] = 0xF8;                               /* fixed disk */
    put16(vbr + 0x16, (unsigned)spf);
    put16(vbr + 0x18, SPT);
    put16(vbr + 0x1A, HEADS);
    put32(vbr + 0x1C, start);                       /* hidden sectors */
    put32(vbr + 0x20, secs < 0x10000 ? 0 : secs);
    vbr[0x24] = 0x80;                               /* BIOS drive */
    vbr[0x26] = 0x29;                               /* extended sig */
    put32(vbr + 0x27, 0x12345678L);
    memset(vbr + 0x2B, ' ', 11);
    memcpy(vbr + 0x2B, label, strlen(label) > 11 ? 11 : strlen(label));
    memcpy(vbr + 0x36, fat12 ? "FAT12   " : "FAT16   ", 8);
    vbr[510] = 0x55; vbr[511] = 0xAA;

    /*--- FATs --------------------------------------------------------*/
    for (i = 0; i < nfats; i++) {
        fatp = sec(start + rsvd + i * spf);
        if (fat12) {
            fat12_set(fatp, 0, 0xFF8);
            fat12_set(fatp, 1, 0xFFF);
            fat12_set(fatp, 2, 0xFFF);      /* the one file, one cluster */
        } else {
            put16(fatp + 0, 0xFFF8);
            put16(fatp + 2, 0xFFFF);
            put16(fatp + 4, 0xFFFF);
        }
    }

    /*--- root directory: a label and one file ------------------------*/
    /* A volume label is ELEVEN CONTIGUOUS BYTES and not an 8.3 name:
       there is no implied dot between byte 7 and byte 8, so running it
       through the filename converter drops everything past the eighth
       character - "LOGICAL-ONE" becomes "LOGICAL-" and the test stops
       being able to tell its own volumes apart. */
    de = sec(rootlba);
    memset(de, ' ', 11);
    memcpy(de, label, strlen(label) > 11 ? 11 : strlen(label));
    de[11] = 0x08;                                  /* volume label */
    stamp(de);
    de += 32;

    name83(fname, de);
    de[11] = 0x20;                                  /* archive */
    stamp(de);
    put16(de + 26, 2);                              /* first cluster */
    put32(de + 28, (unsigned long)strlen(text));
    memcpy(sec(datalba), text, strlen(text));

    printf("  %-11s %8lu sectors at %8lu  spc=%-2lu spf=%-4lu %lu clusters"
           "  %s\n", label, secs, start, spc, spf, nclus,
           fat12 ? "FAT12" : "FAT16");
    return 0;
}

/*--- one partition table entry ------------------------------------*/
static void putpart(unsigned char *pe, int type, unsigned long start,
                    unsigned long secs, int active)
{
    pe[0] = (unsigned char)(active ? 0x80 : 0x00);
    putchs(pe + 1, start);
    pe[4] = (unsigned char)type;
    putchs(pe + 5, start + secs - 1);
    put32(pe + 8, start);
    put32(pe + 12, secs);
}

int main(int argc, char **argv)
{
    const char *out = NULL, *mbrfile = NULL;
    unsigned long total, p1s, p2s, p3s, extbase, extsecs;
    unsigned long l1base, l1secs, l2base, l2secs, l3base, l3secs;
    unsigned char *m, *pe;
    FILE *f;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-mbr") && i + 1 < argc) mbrfile = argv[++i];
        else if (!out) out = argv[i];
    }
    if (!out) {
        fprintf(stderr, "usage: mkmulti out.img [-mbr file]\n");
        return 1;
    }

    /* Everything is a whole number of cylinders and every partition
       starts on a track boundary, which is what every DOS-era tool
       did and what the CHS fields have to agree with. */
    p1s = 33UL * 2048;  p1s -= p1s % SECPERCYL;
    p2s =  8UL * 2048;  p2s -= p2s % SECPERCYL;
    p3s =  4UL * 2048;  p3s -= p3s % SECPERCYL;
    l1secs = 16UL * 2048; l1secs -= l1secs % SECPERCYL;
    l2secs = 16UL * 2048; l2secs -= l2secs % SECPERCYL;
    l3secs =  4UL * 2048; l3secs -= l3secs % SECPERCYL;

    /* Each logical drive costs a track for its own boot record. */
    extsecs = (l1secs + SPT) + (l2secs + SPT) + (l3secs + SPT);
    extbase = SPT + p1s + p2s + p3s;
    total   = extbase + extsecs;
    total  += SECPERCYL - (total % SECPERCYL);

    img_secs = (long)total;
    image = (unsigned char *)calloc(img_secs, SECSIZE);
    if (!image) { fprintf(stderr, "out of memory\n"); return 1; }

    printf("mkmulti: %s, %lu sectors (%lu MB)\n", out, total, total / 2048);

    /*--- the master boot record --------------------------------------*/
    m = sec(0);
    if (mbrfile) {
        FILE *mf = fopen(mbrfile, "rb");
        unsigned char code[SECSIZE];
        if (!mf) { fprintf(stderr, "cannot open %s\n", mbrfile); return 1; }
        if (fread(code, 1, SECSIZE, mf) != SECSIZE) {
            fprintf(stderr, "%s is not 512 bytes\n", mbrfile); return 1;
        }
        fclose(mf);
        memcpy(m, code, 0x1BE);
    }
    pe = m + 0x1BE;
    putpart(pe +  0, 0x06, SPT,                 p1s, 1);
    putpart(pe + 16, 0x01, SPT + p1s,           p2s, 0);
    putpart(pe + 32, 0x83, SPT + p1s + p2s,     p3s, 0);   /* not FAT */
    putpart(pe + 48, 0x05, extbase,             extsecs, 0);
    m[510] = 0x55; m[511] = 0xAA;

    /*--- the extended chain ------------------------------------------
     * Entry 0 of each record is relative to THAT record; entry 1 is
     * relative to the FIRST container.  Getting those two bases the
     * same way round is the whole difficulty of extended partitions,
     * and a builder that gets it wrong produces an image that only
     * a reader with the same bug can read. */
    l1base = extbase;
    l2base = extbase + SPT + l1secs;
    l3base = l2base  + SPT + l2secs;

    m = sec(l1base); pe = m + 0x1BE;
    putpart(pe +  0, 0x04, SPT, l1secs, 0);
    putpart(pe + 16, 0x05, l2base - extbase, SPT + l2secs, 0);
    m[510] = 0x55; m[511] = 0xAA;

    m = sec(l2base); pe = m + 0x1BE;
    putpart(pe +  0, 0x06, SPT, l2secs, 0);
    putpart(pe + 16, 0x05, l3base - extbase, SPT + l3secs, 0);
    m[510] = 0x55; m[511] = 0xAA;

    m = sec(l3base); pe = m + 0x1BE;
    putpart(pe +  0, 0x16, SPT, l3secs, 0);     /* HIDDEN: no letter */
    m[510] = 0x55; m[511] = 0xAA;

    /*--- and the volumes themselves ----------------------------------*/
    printf("  volume                                                 \n");
    if (format_volume(SPT,           p1s,    0, "PRIMARY-ONE", "PRIM1.TXT",
                      "This is the first primary partition, type 06h.\r\n"))
        return 1;
    if (format_volume(SPT + p1s,     p2s,    1, "PRIMARY-TWO", "PRIM2.TXT",
                      "This is the second primary partition, type 01h.\r\n"))
        return 1;
    if (format_volume(l1base + SPT,  l1secs, 0, "LOGICAL-ONE", "LOG1.TXT",
                      "This is the first logical drive, type 04h.\r\n"))
        return 1;
    if (format_volume(l2base + SPT,  l2secs, 0, "LOGICAL-TWO", "LOG2.TXT",
                      "This is the second logical drive, type 06h.\r\n"))
        return 1;
    if (format_volume(l3base + SPT,  l3secs, 0, "HIDDEN-VOL", "HIDDEN.TXT",
                      "This volume is hidden and must not get a letter.\r\n"))
        return 1;

    f = fopen(out, "wb");
    if (!f) { fprintf(stderr, "cannot create %s\n", out); return 1; }
    if (fwrite(image, SECSIZE, img_secs, f) != (size_t)img_secs) {
        fprintf(stderr, "short write\n"); return 1;
    }
    fclose(f);
    printf("  expected letters: C: PRIMARY-ONE  D: LOGICAL-ONE  "
           "E: LOGICAL-TWO  F: PRIMARY-TWO\n");
    return 0;
}
