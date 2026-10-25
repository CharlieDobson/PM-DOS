/*=====================================================================
 * MKSETUP.C - make PM-DOS's Setup disks
 *
 *   mksetup <list> <outdir>
 *   mksetup -iso <out.iso> <boot.img> <dir> [label]
 *
 * THE FIRST FORM reads <list> (DISK\SETUP.LST), INC\PMVER.INC for the
 * version and NLS\COUNTRY.DEF and NLS\KEYBOARD.DEF for the countries
 * and keyboard layouts, run from the top of the tree.  It writes:
 *
 *   <outdir>\STAGE\    every file the Setup disks carry, under the name
 *                      it has there: packed files end in '_'; SETUP.INF;
 *                      Setup disk 1's own CONFIG.SYS and AUTOEXEC.BAT;
 *                      and the tag files DISK1, DISK2... that tell the
 *                      disks apart.  A CD, a USB stick or a network
 *                      directory is this directory as it is.
 *   <outdir>\MKIMG.CMD MKDISK lines for the diskettes (DISK1.IMG ...),
 *                      the USB image (SETUPUSB.IMG, 16MB, 16 heads of
 *                      63 sectors) and the Zip image (SETUPZIP.IMG, the
 *                      same at 64 heads of 32), then this program's
 *                      -iso line for SETUPCD.ISO.  BUILD.CMD calls it.
 *
 * PACKING is SZDD, the format MS-DOS 5's EXPAND reads (and SETUP's own
 * COPY.C): "SZDD" 88 F0 27 33, 'A', the last character of the name the
 * '_' took the place of, the length; then LZSS with a 4K window that
 * starts full of spaces and is written from 4096-16, a flag byte for
 * each eight items read from its low bit up, 1 = a byte as it is, 0 = a
 * position (12 bits) and a length less 3 (4 bits).  A file is packed
 * when that makes it smaller; every packed file is unpacked again here
 * and compared before it is used.
 *
 * THE DISKS are 1.44MB FAT12 diskettes as MKDISK makes them: 2847
 * clusters of 512 bytes and 224 root entries, one of them the label.
 * Disk 1 starts with what the Setup disk is started from and Setup
 * needs before it can ask for another disk - the files the list marks
 * 1, in order, as they are - and then whatever fits; the rest go on in
 * list order to disk 2 and on.
 *
 * THE SECOND FORM writes an ISO9660 CD-ROM image of <dir>'s files (one
 * directory, 8.3 names) that starts the computer the El Torito way: the
 * BIOS makes <boot.img>, a 1.44MB diskette image, drive A:.  The image
 * is a file on the disc as well.
 *
 * Built with:  wcl386 -q -bt=nt -fe=mksetup.exe mksetup.c
 *===================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <sys/stat.h>
#include <direct.h>
#include <io.h>

#define MAXFILES    200
#define MAXLINES    400
#define DISK_CLUS   2847UL          /* a 1.44MB diskette's data clusters */
#define DISK_ROOT   223             /* root entries, less the label */
#define CLUS        512UL
#define SPARE       8UL             /* clusters held back on each disk */
#define INF_ROOM    24              /* clusters held for SETUP.INF */

typedef struct {
    char  src[260];                 /* the file in the tree */
    char  name[13];                 /* its 8.3 name */
    char  packed[13];               /* the name on the Setup disk */
    char  where[8];                 /* ROOT DOS SETUP */
    char  flags[8];                 /* S F U from the list, P when packed */
    int   first;                    /* on disk 1, as it is */
    int   nopack;
    unsigned char *data;            /* what goes on the disk */
    unsigned long size;             /* its size once expanded */
    unsigned long disksize;         /* its size on the disk */
    int   disk;
} ENTRY;

static ENTRY ent[MAXFILES];
static int nent;

static char *section[16][MAXLINES];
static int nsection[16];
static const char *section_names[] = {
    "setup", "files", "config", "autoexec", "config.add", "config.remove",
    "autoexec.remove", "floppy.config", "setup.config", "setup.autoexec"
};
enum { SEC_SETUP, SEC_FILES, SEC_CONFIG, SEC_AUTOEXEC, SEC_CFGADD,
       SEC_CFGDROP, SEC_AUTODROP, SEC_FLCFG, SEC_SUCFG, SEC_SUAUTO, SEC_N };

static char product[32] = "PM-DOS", dosdir[64] = "C:\\DOS", label[16] = "PM-DOS";
static char version[16];
static int ndisks;

/*---------------------------------------------------------------------
 * small things
 *-------------------------------------------------------------------*/
static void die(const char *what, const char *name)
{
    fprintf(stderr, "mksetup: %s%s%s\n", what, name ? " - " : "", name ? name : "");
    exit(1);
}

static char *trim(char *s)
{
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

static unsigned char *load(const char *path, unsigned long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 1);
    if (!buf || (n && fread(buf, 1, n, f) != (size_t)n)) die("cannot read", path);
    fclose(f);
    buf[n] = 0;
    *len = (unsigned long)n;
    return buf;
}

static void save(const char *path, const unsigned char *data, unsigned long len)
{
    FILE *f = fopen(path, "wb");
    if (!f || (len && fwrite(data, 1, len, f) != len) || fclose(f)) die("cannot write", path);
}

static void base_name(char *dst, const char *path)
{
    const char *p = strrchr(path, '\\');
    const char *q = strrchr(path, '/');
    if (q > p) p = q;
    strncpy(dst, p ? p + 1 : path, 12);
    dst[12] = 0;
    for (p = dst; *p; p++) *(char *)p = (char)toupper((unsigned char)*p);
}

/*---------------------------------------------------------------------
 * SZDD
 *-------------------------------------------------------------------*/
#define WSIZE   4096
#define WMASK   (WSIZE - 1)
#define MAXLEN  18
#define HBITS   13
#define CHAIN   512

static unsigned char *szdd_pack(const unsigned char *in, unsigned long n,
                                char lastch, unsigned long *outlen)
{
    unsigned char *out = malloc(n + n / 8 + 64);
    long *head = malloc(sizeof(long) << HBITS);
    long *prev = malloc(sizeof(long) * (n + 1));
    unsigned long i, o = 14, flagat;
    int bit = 8, k;

    if (!out || !head || !prev) die("out of memory", NULL);
    memcpy(out, "SZDD\x88\xF0\x27\x33", 8);
    out[8] = 'A';
    out[9] = (unsigned char)lastch;
    out[10] = (unsigned char)n; out[11] = (unsigned char)(n >> 8);
    out[12] = (unsigned char)(n >> 16); out[13] = (unsigned char)(n >> 24);
    for (k = 0; k < (1 << HBITS); k++) head[k] = -1;
    flagat = 0;
    for (i = 0; i < n; ) {
        unsigned long best = 0, bestpos = 0;
        if (bit == 8) { flagat = o++; out[flagat] = 0; bit = 0; }
        if (i + 2 < n) {
            unsigned h = ((in[i] << 8) ^ (in[i + 1] << 4) ^ in[i + 2]) & ((1 << HBITS) - 1);
            long cand = head[h];
            int chain = 0;
            while (cand >= 0 && chain++ < CHAIN && i - (unsigned long)cand <= WSIZE - MAXLEN) {
                unsigned long len = 0, max = n - i < MAXLEN ? n - i : MAXLEN;
                while (len < max && in[cand + len] == in[i + len]) len++;
                if (len > best) { best = len; bestpos = (unsigned long)cand; if (len == MAXLEN) break; }
                cand = prev[cand];
            }
        }
        if (best >= 3) {
            unsigned pos = (unsigned)((WSIZE - 16 + bestpos) & WMASK);
            out[o++] = (unsigned char)pos;
            out[o++] = (unsigned char)(((pos >> 4) & 0xF0) | (best - 3));
        } else {
            best = 1;
            out[flagat] |= (unsigned char)(1 << bit);
            out[o++] = in[i];
        }
        bit++;
        /* every position passed goes into the chains */
        while (best--) {
            if (i + 2 < n) {
                unsigned h = ((in[i] << 8) ^ (in[i + 1] << 4) ^ in[i + 2]) & ((1 << HBITS) - 1);
                prev[i] = head[h];
                head[h] = (long)i;
            }
            i++;
        }
    }
    free(head);
    free(prev);
    *outlen = o;
    return out;
}

/* the same decoder as SETUP's COPY.C: 1 = it does not give "in" back */
static int szdd_check(const unsigned char *packed, unsigned long plen,
                      const unsigned char *in, unsigned long n)
{
    unsigned char win[WSIZE];
    unsigned long at = 14, total = 0;
    int pos = WSIZE - 16, flags = 0, bits = 0, i;

    memset(win, ' ', sizeof(win));
    while (at < plen) {
        if (bits == 0) { flags = packed[at++]; bits = 8; continue; }
        if (flags & 1) {
            if (total >= n || packed[at] != in[total]) return 1;
            win[pos] = packed[at++]; pos = (pos + 1) & WMASK; total++;
        } else {
            int from, len;
            if (at + 1 >= plen) return 1;
            from = packed[at] | ((packed[at + 1] & 0xF0) << 4);
            len = (packed[at + 1] & 0x0F) + 3;
            at += 2;
            for (i = 0; i < len; i++) {
                unsigned char v = win[(from + i) & WMASK];
                if (total >= n || v != in[total]) return 1;
                win[pos] = v; pos = (pos + 1) & WMASK; total++;
            }
        }
        flags >>= 1;
        bits--;
    }
    return total != n;
}

/* NAME.EXT -> NAME.EX_ ; NAME.E -> NAME.E_ ; NAME -> NAME._ */
static char packed_name(char *dst, const char *name)
{
    const char *dot = strchr(name, '.');
    size_t len = strlen(name);
    char last;

    strcpy(dst, name);
    if (!dot) { strcat(dst, "._"); return 0; }
    if (strlen(dot) < 4) { strcat(dst, "_"); return 0; }
    last = dst[len - 1];
    dst[len - 1] = '_';
    return last;
}

/*---------------------------------------------------------------------
 * the list
 *-------------------------------------------------------------------*/
static void read_list(const char *path)
{
    unsigned long len;
    char *text = (char *)load(path, &len), *line, *next;
    int sec = -1, i;

    if (!text) die("cannot read", path);
    for (line = text; *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        if (sec != SEC_CONFIG && sec != SEC_AUTOEXEC && sec != SEC_SUCFG &&
            sec != SEC_SUAUTO && sec != SEC_FLCFG && sec != SEC_CFGADD) {
            line = trim(line);
        } else {
            char *t = trim(line);
            if (*t == '[' || *t == ';' || !*t) line = t;
            else { size_t l = strlen(line); while (l && (line[l-1] == '\r')) line[--l] = 0; }
        }
        if (!*line || *line == ';') continue;
        if (*line == '[') {
            char *e = strchr(line, ']');
            if (e) *e = 0;
            sec = -1;
            for (i = 0; i < SEC_N; i++) if (!strcmp(line + 1, section_names[i])) sec = i;
            if (sec < 0) die("unknown section", line);
            continue;
        }
        if (sec < 0) die("a line before any section", line);
        if (nsection[sec] >= MAXLINES) die("too many lines in", section_names[sec]);
        section[sec][nsection[sec]++] = line;
    }
    for (i = 0; i < nsection[SEC_SETUP]; i++) {
        char *eq = strchr(section[SEC_SETUP][i], '=');
        if (!eq) continue;
        *eq++ = 0;
        if (!strcmp(trim(section[SEC_SETUP][i]), "product")) strncpy(product, trim(eq), 31);
        else if (!strcmp(trim(section[SEC_SETUP][i]), "dosdir")) strncpy(dosdir, trim(eq), 63);
        else if (!strcmp(trim(section[SEC_SETUP][i]), "label")) strncpy(label, trim(eq), 11);
    }
    for (i = 0; i < nsection[SEC_FILES]; i++) {
        char src[260], where[16], flags[16];
        ENTRY *e;
        int n;
        flags[0] = 0;
        n = sscanf(section[SEC_FILES][i], "%259s %15s %15s", src, where, flags);
        if (n < 2) die("a [files] line wants a file and where it goes", section[SEC_FILES][i]);
        if (nent >= MAXFILES) die("too many files", NULL);
        e = &ent[nent++];
        memset(e, 0, sizeof(*e));
        strcpy(e->src, src);
        base_name(e->name, src);
        strncpy(e->where, where, 7);
        for (n = 0; flags[n]; n++) {
            char c = (char)toupper((unsigned char)flags[n]);
            if (c == '1') e->first = 1;
            else if (c == 'N') e->nopack = 1;
            else if (c == 'S' || c == 'F' || c == 'U') {
                size_t l = strlen(e->flags);
                if (l < 6) { e->flags[l] = c; e->flags[l + 1] = 0; }
            } else die("an unknown flag in", section[SEC_FILES][i]);
        }
        if (strcmp(e->where, "ROOT") && strcmp(e->where, "DOS") && strcmp(e->where, "SETUP"))
            die("where must be ROOT, DOS or SETUP", section[SEC_FILES][i]);
    }
}

static void read_version(void)
{
    unsigned long len;
    char *text = (char *)load("INC\\PMVER.INC", &len), *p;
    int major = -1, minor = -1;

    if (!text) die("cannot read", "INC\\PMVER.INC");
    p = strstr(text, "PMDOS_MAJOR");
    if (p) { p = strstr(p, "EQU"); if (p) major = atoi(p + 3); }
    p = strstr(text, "PMDOS_MINOR");
    if (p) { p = strstr(p, "EQU"); if (p) minor = atoi(p + 3); }
    if (major < 0 || minor < 0) die("no version in", "INC\\PMVER.INC");
    sprintf(version, "%d.%02d", major, minor);
}

/*---------------------------------------------------------------------
 * the countries and layouts, for SETUP's lists
 *-------------------------------------------------------------------*/
typedef struct { char line[160]; char name[40]; } ROW;
static ROW crow[96], krow[96];
static int ncrow, nkrow;
static int allpages[16], nallpages;

static int byname(const void *a, const void *b)
{
    return strcmp(((const ROW *)a)->name, ((const ROW *)b)->name);
}

static void note_page(int page)
{
    int i;
    for (i = 0; i < nallpages; i++) if (allpages[i] == page) return;
    if (nallpages < 16) allpages[nallpages++] = page;
}

static void read_countries(void)
{
    unsigned long len;
    char *text = (char *)load("NLS\\COUNTRY.DEF", &len), *line, *next;

    if (!text) die("cannot read", "NLS\\COUNTRY.DEF");
    for (line = text; *line; line = next) {
        char code[8], name[40], pages[40], date[8], dsep[8], clock[8], tsep[8];
        char *p, list[40];
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = line + strlen(line);
        line = trim(line);
        if (!*line || *line == ';') continue;
        if (sscanf(line, "%7s %39s %39s %7s %7s %7s %7s", code, name, pages, date, dsep, clock, tsep) != 7)
            die("a COUNTRY.DEF line SETUP cannot read", line);
        for (p = name; *p; p++) if (*p == '-') *p = ' ';
        list[0] = 0;
        for (p = strtok(pages, ","); p; p = strtok(NULL, ",")) {
            if (list[0]) strcat(list, " ");
            strcat(list, p);
            note_page(atoi(p));
        }
        if (ncrow >= 96) die("too many countries", NULL);
        strcpy(crow[ncrow].name, name);
        sprintf(crow[ncrow].line, "%s,%s,%d,%c,%c,%s,%s", code, name,
                !strcmp(date, "dmy") ? 1 : !strcmp(date, "ymd") ? 2 : 0,
                dsep[0], tsep[0], clock, list);
        ncrow++;
    }
    qsort(crow, ncrow, sizeof(ROW), byname);
}

static void read_keyboards(void)
{
    unsigned long len;
    char *text = (char *)load("NLS\\KEYBOARD.DEF", &len), *line, *next;
    char code[8] = "", name[40] = "", list[40] = "";
    int id = 0, i;

    if (!text) die("cannot read", "NLS\\KEYBOARD.DEF");
    for (line = text; ; line = next) {
        int end = !*line;
        if (!end) {
            next = strchr(line, '\n');
            if (next) *next++ = 0; else next = line + strlen(line);
            line = trim(line);
        }
        if (end || *line == '[') {
            if (code[0]) {
                if (nkrow >= 95) die("too many layouts", NULL);
                if (!list[0]) die("a layout with no cp line", code);
                strcpy(krow[nkrow].name, name);
                sprintf(krow[nkrow].line, "%s,%d,%s,%s", code, id, name, list);
                nkrow++;
            }
            if (end) break;
            code[0] = 0; list[0] = 0;
            if (sscanf(line, "[%7s %d]", code, &id) != 2) die("a KEYBOARD.DEF section SETUP cannot read", line);
            if (!strchr(line, ']')) die("a KEYBOARD.DEF section with no ]", line);
            strncpy(name, trim(strchr(line, ']') + 1), 36);
            name[36] = 0;
            /* "Switzerland, French" becomes "Switzerland (French)", as 6.22
               named them - and keeps its comma out of the INF */
            {
                char *comma = strchr(name, ',');
                if (comma) {
                    char rest[40];
                    strcpy(rest, trim(comma + 1));
                    sprintf(comma, " (%s)", rest);
                }
            }
            continue;
        }
        if (!strncmp(line, "cp", 2) && (line[2] == ' ' || line[2] == '\t')) {
            strncpy(list, trim(line + 2), 39);
            for (i = 0; list[i]; i++) if (list[i] == '\t') list[i] = ' ';
        }
    }
    /* the layout the BIOS has, which needs no KEYB and no code page */
    strcpy(krow[nkrow].name, "United States");
    list[0] = 0;
    for (i = 0; i < nallpages; i++) sprintf(list + strlen(list), "%s%d", i ? " " : "", allpages[i]);
    sprintf(krow[nkrow].line, "US,0,United States,%s", list);
    nkrow++;
    qsort(krow, nkrow, sizeof(ROW), byname);
}

/*---------------------------------------------------------------------
 * the disks
 *-------------------------------------------------------------------*/
static unsigned long clusters(unsigned long bytes)
{
    return (bytes + CLUS - 1) / CLUS;
}

static void add_text(char *buf, size_t max, const char *line)
{
    if (strlen(buf) + strlen(line) + 3 >= max) die("SETUP.INF is too big", NULL);
    strcat(buf, line);
    strcat(buf, "\r\n");
}

static unsigned char *join_section(int sec, unsigned long *len)
{
    static char buf[8192];
    int i;
    buf[0] = 0;
    for (i = 0; i < nsection[sec]; i++) add_text(buf, sizeof(buf), section[sec][i]);
    *len = strlen(buf);
    return (unsigned char *)buf;
}

static void make_set(const char *outdir)
{
    char stage[300], path[400], line[400], tag[16];
    static char inf[60000];
    unsigned long len, used[16], i_used[16];
    int i, d, s;
    FILE *cmd;

    sprintf(stage, "%s\\STAGE", outdir);
    mkdir(stage);                       /* there already is no matter */

    /* read and pack */
    for (i = 0; i < nent; i++) {
        ENTRY *e = &ent[i];
        unsigned char *raw = load(e->src, &len);
        if (!raw) die("missing", e->src);
        e->size = len;
        strcpy(e->packed, e->name);
        e->data = raw;
        e->disksize = len;
        if (!e->first && !e->nopack && len > 0) {
            char last = packed_name(e->packed, e->name);
            unsigned long plen;
            unsigned char *p = szdd_pack(raw, len, last, &plen);
            if (szdd_check(p, plen, raw, len)) die("packing does not give the file back", e->src);
            if (plen < len) {
                e->data = p;
                e->disksize = plen;
                strcat(e->flags, "P");
            } else {
                strcpy(e->packed, e->name);
                free(p);
            }
        }
    }
    /* two files must not share a name on the disk */
    for (i = 0; i < nent; i++) for (d = i + 1; d < nent; d++)
        if (!strcmp(ent[i].packed, ent[d].packed)) die("two files would share the name", ent[i].packed);

    /* disk 1: what it starts from, Setup's own two files, the tag */
    memset(used, 0, sizeof(used));
    memset(i_used, 0, sizeof(i_used));
    ndisks = 1;
    used[1] = SPARE + INF_ROOM + 3;     /* SETUP.INF, CONFIG.SYS, AUTOEXEC.BAT, DISK1 */
    i_used[1] = 4;
    for (i = 0; i < nent; i++) if (ent[i].first) {
        ent[i].disk = 1;
        used[1] += clusters(ent[i].disksize);
        i_used[1]++;
    }
    if (used[1] > DISK_CLUS) die("disk 1's start-up files do not fit on it", NULL);
    for (i = 0; i < nent; i++) if (!ent[i].first) {
        unsigned long c = clusters(ent[i].disksize);
        for (d = 1; d <= ndisks; d++)
            if (used[d] + c <= DISK_CLUS && i_used[d] < DISK_ROOT) break;
        if (d > ndisks) {
            if (++ndisks > 9) die("more than nine disks", NULL);
            d = ndisks;
            used[d] = SPARE + 1;        /* the tag */
            i_used[d] = 1;
        }
        ent[i].disk = d;
        used[d] += c;
        i_used[d]++;
    }

    /* SETUP.INF */
    inf[0] = 0;
    add_text(inf, sizeof(inf), "; SETUP.INF - what PM-DOS Setup installs.  Written by TOOLS\\MKSETUP.EXE");
    add_text(inf, sizeof(inf), "; from DISK\\SETUP.LST; change that and build again rather than this.");
    add_text(inf, sizeof(inf), "[setup]");
    sprintf(line, "product=%s", product); add_text(inf, sizeof(inf), line);
    sprintf(line, "version=%s", version); add_text(inf, sizeof(inf), line);
    sprintf(line, "dosdir=%s", dosdir); add_text(inf, sizeof(inf), line);
    sprintf(line, "label=%s", label); add_text(inf, sizeof(inf), line);
    add_text(inf, sizeof(inf), "[disks]");
    for (d = 1; d <= ndisks; d++) {
        sprintf(line, "%d=DISK%d,%s %s Setup Disk #%d", d, d, product, version, d);
        add_text(inf, sizeof(inf), line);
    }
    add_text(inf, sizeof(inf), "[files]");
    for (d = 1; d <= ndisks; d++) for (i = 0; i < nent; i++) if (ent[i].disk == d) {
        sprintf(line, "%d,%s,%s,%s,%s,%lu", d, ent[i].packed, ent[i].name, ent[i].where,
                ent[i].flags, ent[i].size);
        add_text(inf, sizeof(inf), line);
    }
    add_text(inf, sizeof(inf), "[countries]");
    for (i = 0; i < ncrow; i++) add_text(inf, sizeof(inf), crow[i].line);
    add_text(inf, sizeof(inf), "[keyboards]");
    for (i = 0; i < nkrow; i++) add_text(inf, sizeof(inf), krow[i].line);
    for (s = SEC_CONFIG; s <= SEC_FLCFG; s++) {
        sprintf(line, "[%s]", section_names[s]);
        add_text(inf, sizeof(inf), line);
        for (i = 0; i < nsection[s]; i++) add_text(inf, sizeof(inf), section[s][i]);
    }
    if (clusters(strlen(inf)) > INF_ROOM) die("SETUP.INF outgrew the room held for it", NULL);

    /* the stage */
    for (i = 0; i < nent; i++) {
        sprintf(path, "%s\\%s", stage, ent[i].packed);
        save(path, ent[i].data, ent[i].disksize);
    }
    sprintf(path, "%s\\SETUP.INF", stage);
    save(path, (unsigned char *)inf, strlen(inf));
    sprintf(path, "%s\\CONFIG.SYS", stage);
    { unsigned char *t = join_section(SEC_SUCFG, &len); save(path, t, len); }
    sprintf(path, "%s\\AUTOEXEC.BAT", stage);
    { unsigned char *t = join_section(SEC_SUAUTO, &len); save(path, t, len); }
    for (d = 1; d <= ndisks; d++) {
        sprintf(tag, "DISK%d", d);
        sprintf(path, "%s\\%s", stage, tag);
        sprintf(line, "%s %s Setup Disk #%d\r\n", product, version, d);
        save(path, (unsigned char *)line, strlen(line));
    }

    /* the commands that make the images */
    sprintf(path, "%s\\MKIMG.CMD", outdir);
    cmd = fopen(path, "wb");
    if (!cmd) die("cannot write", path);
    fprintf(cmd, "@rem MKIMG.CMD - written by TOOLS\\MKSETUP.EXE: the Setup images\r\n");
    for (d = 1; d <= ndisks; d++) {
        fprintf(cmd, "TOOLS\\MKDISK.EXE %s\\DISK%d.IMG BIN\\BOOT.BIN -nolfn -label \"DISK      %d\"", outdir, d, d);
        if (d == 1) {
            for (i = 0; i < nent; i++) if (ent[i].disk == 1 && ent[i].first)
                fprintf(cmd, " %s\\%s", stage, ent[i].packed);
            fprintf(cmd, " %s\\SETUP.INF %s\\CONFIG.SYS %s\\AUTOEXEC.BAT", stage, stage, stage);
        }
        fprintf(cmd, " %s\\DISK%d", stage, d);
        for (i = 0; i < nent; i++) if (ent[i].disk == d && !ent[i].first)
            fprintf(cmd, " %s\\%s", stage, ent[i].packed);
        fprintf(cmd, "\r\n");
    }
    for (s = 0; s < 2; s++) {
        fprintf(cmd, "TOOLS\\MKDISK.EXE -hd16 16 %s\\%s.IMG%s -mbr BIN\\MBRBOOT.BIN -vbr BIN\\BOOT16.BIN"
                     " -s2 BIN\\BOOT2.BIN -label \"PMDOS SETUP\"",
                outdir, s ? "SETUPZIP" : "SETUPUSB", s ? " -geom 64 32" : "");
        for (i = 0; i < nent; i++) if (ent[i].first)
            fprintf(cmd, " %s\\%s", stage, ent[i].packed);
        fprintf(cmd, " %s\\SETUP.INF %s\\CONFIG.SYS %s\\AUTOEXEC.BAT", stage, stage, stage);
        for (d = 1; d <= ndisks; d++) fprintf(cmd, " %s\\DISK%d", stage, d);
        for (i = 0; i < nent; i++) if (!ent[i].first)
            fprintf(cmd, " %s\\%s", stage, ent[i].packed);
        fprintf(cmd, "\r\n");
    }
    fprintf(cmd, "TOOLS\\MKSETUP.EXE -iso %s\\SETUPCD.ISO %s\\DISK1.IMG %s \"PMDOS_%s\"\r\n",
            outdir, outdir, stage, version);
    fclose(cmd);

    printf("%d disks, %d files (%s %s)\n", ndisks, nent, product, version);
    for (d = 1; d <= ndisks; d++)
        printf("  disk %d: %lu of %lu clusters, %lu root entries\n", d, used[d], DISK_CLUS, i_used[d]);
}

/*---------------------------------------------------------------------
 * the CD
 *-------------------------------------------------------------------*/
#define SEC2K   2048UL

static void both16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void both32(unsigned char *p, unsigned long v)
{
    int i;
    for (i = 0; i < 4; i++) {
        p[i] = (unsigned char)(v >> (8 * i));
        p[7 - i] = (unsigned char)(v >> (8 * i));
    }
}

static void put32le(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

static void put32be(unsigned char *p, unsigned long v)
{
    p[3] = (unsigned char)v; p[2] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v >> 16); p[0] = (unsigned char)(v >> 24);
}

static void pad(unsigned char *p, const char *text, int len)
{
    int i, n = (int)strlen(text);
    for (i = 0; i < len; i++) p[i] = (unsigned char)(i < n ? text[i] : ' ');
}

typedef struct {
    char name[16];                  /* NAME.EXT;1 */
    char path[300];
    unsigned long size, lba;
} CDFILE;

static CDFILE cdf[MAXFILES + 16];
static int ncdf;

static int cdcmp(const void *a, const void *b)
{
    return strcmp(((const CDFILE *)a)->name, ((const CDFILE *)b)->name);
}

static unsigned char stamp7[7];

static void dirrec(unsigned char *p, unsigned long lba, unsigned long size, int dir,
                   const char *name, int nlen)
{
    int len = 33 + nlen + ((nlen & 1) ? 0 : 1);
    memset(p, 0, len);
    p[0] = (unsigned char)len;
    both32(p + 2, lba);
    both32(p + 10, size);
    memcpy(p + 18, stamp7, 7);
    p[25] = (unsigned char)(dir ? 2 : 0);
    both16(p + 28, 1);
    p[32] = (unsigned char)nlen;
    memcpy(p + 33, name, nlen);
}

static void make_iso(const char *out, const char *bootimg, const char *dir, const char *vol)
{
    unsigned char *iso, *p;
    unsigned long total, rootlba, rootsecs, at, cat, i, bootlba = 0, sum;
    unsigned long rootbytes;
    char path[400], spec[400], digits[20];
    int k, n;
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    FILE *f;
    struct _finddata_t fd;
    long h;

    /* the files: the directory's, then the boot image */
    if (strlen(dir) > 250) die("the directory's name is too long", dir);
    sprintf(spec, "%s\\*.*", dir);
    h = (long)_findfirst(spec, &fd);
    if (h == -1) die("nothing in", dir);
    do {
        if (fd.attrib & _A_SUBDIR) continue;
        if (ncdf >= MAXFILES + 15) die("too many files for the CD", NULL);
        /* one directory of 8.3 names, which is all ISO9660 level 1 has */
        {
            const char *dot = strchr(fd.name, '.');
            size_t len = strlen(fd.name);
            if (len > 12 || (dot && (dot - fd.name > 8 || strlen(dot) > 4)) || (!dot && len > 8))
                die("not an 8.3 name, which the CD cannot hold", fd.name);
            if (strlen(dir) + len + 2 > sizeof(cdf[0].path)) die("the path is too long", dir);
        }
        base_name(cdf[ncdf].name, fd.name);
        if (!strchr(cdf[ncdf].name, '.')) strcat(cdf[ncdf].name, ".");
        strcat(cdf[ncdf].name, ";1");
        sprintf(cdf[ncdf].path, "%s\\%s", dir, fd.name);
        cdf[ncdf].size = fd.size;
        ncdf++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    {
        struct stat st;
        if (stat(bootimg, &st) || st.st_size != 1474560L) die("the boot image is not a 1.44MB diskette", bootimg);
        if (strlen(bootimg) + 1 > sizeof(cdf[0].path)) die("the path is too long", bootimg);
        base_name(cdf[ncdf].name, bootimg);
        strcat(cdf[ncdf].name, ";1");
        strcpy(cdf[ncdf].path, bootimg);
        cdf[ncdf].size = (unsigned long)st.st_size;
        ncdf++;
    }
    qsort(cdf, ncdf, sizeof(CDFILE), cdcmp);

    stamp7[0] = (unsigned char)tm->tm_year; stamp7[1] = (unsigned char)(tm->tm_mon + 1);
    stamp7[2] = (unsigned char)tm->tm_mday; stamp7[3] = (unsigned char)tm->tm_hour;
    stamp7[4] = (unsigned char)tm->tm_min; stamp7[5] = (unsigned char)tm->tm_sec;

    /* the root's size: records do not cross a sector */
    rootbytes = 0;
    {
        unsigned long insec = 68;           /* "." and ".." */
        rootsecs = 1;
        for (k = 0; k < ncdf; k++) {
            int nlen = (int)strlen(cdf[k].name);
            int len = 33 + nlen + ((nlen & 1) ? 0 : 1);
            if (insec + len > SEC2K) { rootsecs++; insec = 0; }
            insec += len;
        }
    }
    /* the layout: 16 empty, PVD 16, boot record 17, end 18, catalog 19,
       path tables 20 and 21, root 22, then the files */
    cat = 19;
    rootlba = 22;
    at = rootlba + rootsecs;
    for (k = 0; k < ncdf; k++) {
        cdf[k].lba = at;
        at += (cdf[k].size + SEC2K - 1) / SEC2K;
        if (!strcmp(cdf[k].path, bootimg)) bootlba = cdf[k].lba;
    }
    total = at;
    iso = calloc(total, SEC2K);
    if (!iso) die("out of memory", NULL);

    /* the files */
    for (k = 0; k < ncdf; k++) {
        unsigned long len;
        unsigned char *data = load(cdf[k].path, &len);
        if (!data) die("cannot read", cdf[k].path);
        memcpy(iso + cdf[k].lba * SEC2K, data, len);
        free(data);
    }

    /* the root */
    p = iso + rootlba * SEC2K;
    dirrec(p, rootlba, rootsecs * SEC2K, 1, "\0", 1);
    dirrec(p + 34, rootlba, rootsecs * SEC2K, 1, "\1", 1);
    {
        unsigned long insec = 68, secno = 0;
        for (k = 0; k < ncdf; k++) {
            int nlen = (int)strlen(cdf[k].name);
            int len = 33 + nlen + ((nlen & 1) ? 0 : 1);
            if (insec + len > SEC2K) { secno++; insec = 0; }
            dirrec(p + secno * SEC2K + insec, cdf[k].lba, cdf[k].size, 0, cdf[k].name, nlen);
            insec += len;
        }
    }

    /* the path tables: the root alone */
    p = iso + 20 * SEC2K;
    p[0] = 1; put32le(p + 2, rootlba); p[6] = 1;
    p = iso + 21 * SEC2K;
    p[0] = 1; put32be(p + 2, rootlba); p[7] = 1;

    /* the primary volume descriptor */
    p = iso + 16 * SEC2K;
    p[0] = 1; memcpy(p + 1, "CD001", 5); p[6] = 1;
    pad(p + 8, "", 32);
    pad(p + 40, vol, 32);
    both32(p + 80, total);
    both16(p + 120, 1);
    both16(p + 124, 1);
    both16(p + 128, (unsigned)SEC2K);
    both32(p + 132, 10);
    put32le(p + 140, 20);
    put32be(p + 148, 21);
    dirrec(p + 156, rootlba, rootsecs * SEC2K, 1, "\0", 1);
    pad(p + 190, "", 128);
    pad(p + 318, "PM-DOS", 128);
    pad(p + 446, "", 128);
    pad(p + 574, "PM-DOS SETUP", 128);
    pad(p + 702, "", 37); pad(p + 739, "", 37); pad(p + 776, "", 37);
    sprintf(digits, "%04d%02d%02d%02d%02d%02d00", tm->tm_year + 1900, tm->tm_mon + 1,
            tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    memcpy(p + 813, digits, 16); p[829] = 0;
    memcpy(p + 830, digits, 16); p[846] = 0;
    memset(p + 847, '0', 16); p[863] = 0;
    memset(p + 864, '0', 16); p[880] = 0;
    p[881] = 1;

    /* El Torito: the boot record, and the catalog it points at */
    p = iso + 17 * SEC2K;
    p[0] = 0; memcpy(p + 1, "CD001", 5); p[6] = 1;
    memcpy(p + 7, "EL TORITO SPECIFICATION", 23);
    put32le(p + 0x47, cat);
    p = iso + 18 * SEC2K;
    p[0] = 255; memcpy(p + 1, "CD001", 5); p[6] = 1;

    p = iso + cat * SEC2K;
    p[0] = 1;                       /* validation entry, x86 */
    memcpy(p + 4, "PM-DOS SETUP", 12);
    p[30] = 0x55; p[31] = 0xAA;
    sum = 0;
    for (i = 0; i < 32; i += 2) sum += p[i] | (p[i + 1] << 8);
    sum = (0x10000UL - (sum & 0xFFFF)) & 0xFFFF;
    p[28] = (unsigned char)sum; p[29] = (unsigned char)(sum >> 8);
    p += 32;                        /* the initial entry */
    p[0] = 0x88;                    /* bootable */
    p[1] = 2;                       /* a 1.44MB diskette */
    p[6] = 1;                       /* one 512-byte sector: the BIOS reads */
    put32le(p + 8, bootlba);        /* the rest itself as drive A: */

    f = fopen(out, "wb");
    if (!f || fwrite(iso, SEC2K, total, f) != total || fclose(f)) die("cannot write", out);
    n = ncdf;
    printf("%s: %lu sectors, %d files, boots %s\n", out, total, n, bootimg);
}

/*---------------------------------------------------------------------*/
int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "-iso")) {
        make_iso(argv[2], argv[3], argv[4], argc > 5 ? argv[5] : "PMDOS_SETUP");
        return 0;
    }
    if (argc != 3) {
        fprintf(stderr,
            "usage: mksetup <list> <outdir>        the Setup disks (from the tree's top)\n"
            "       mksetup -iso <out.iso> <boot.img> <dir> [label]\n");
        return 1;
    }
    read_version();
    read_list(argv[1]);
    read_countries();
    read_keyboards();
    /* every path made below is <outdir>\STAGE\ and an 8.3 name, in
       buffers of 300 and 400 */
    if (strlen(argv[2]) > 200) die("the output directory's name is too long", argv[2]);
    make_set(argv[2]);
    return 0;
}
