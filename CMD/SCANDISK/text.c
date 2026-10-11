/*
 * TEXT.C - what SCANDISK says that is not about one particular file:
 * the help, what each check is and what each kind of problem means
 * ("More Info"), and the summary of a drive.
 *
 * None of this is MS-DOS's wording.  The switches, the settings and
 * what they do are ScanDisk's; the sentences are written for this one.
 */
#include "scandisk.h"

COUNTRY country;

const char help_text[] =
    "Checks a disk for errors, repairs them, and can scan its surface.\r\n"
    "\r\n"
    "SCANDISK [drive: [drive: ...] | /ALL] [/CHECKONLY | /AUTOFIX [/NOSAVE] | /CUSTOM]\r\n"
    "         [/SURFACE] [/G] [/NOSUMMARY]\r\n"
    "SCANDISK /FRAGMENT [drive:][path]filename\r\n"
    "SCANDISK /UNDO [undo-drive:] [/G]\r\n"
    "\r\n"
    "  drive:       The drive to check.  The current drive if none is given.\r\n"
    "  /ALL         Checks every hard disk drive.\r\n"
    "  /CHECKONLY   Looks for errors and repairs nothing.\r\n"
    "  /AUTOFIX     Repairs errors without asking.  Lost clusters are saved as\r\n"
    "               files in the root directory.\r\n"
    "  /NOSAVE      With /AUTOFIX, frees lost clusters and saves nothing.\r\n"
    "  /CUSTOM      Runs as the [CUSTOM] section of SCANDISK.INI says.\r\n"
    "  /SURFACE     Scans the surface of the disk after the other checks,\r\n"
    "               without asking first.\r\n"
    "  /G           Uses a graphics screen (EGA or VGA).\r\n"
    "  /NOSUMMARY   Shows no summary after each drive, and does not ask for\r\n"
    "               an Undo disk.\r\n"
    "  /FRAGMENT    Reports which of the named files are fragmented.\r\n"
    "  /UNDO        Undoes the repairs ScanDisk last made, from the Undo disk\r\n"
    "               in undo-drive (A: if none is given).\r\n";

/* ------------------------------------------------------------------ */
/* the checks                                                          */
/* ------------------------------------------------------------------ */

const char *phase_name( int phase )
{
    static const char *const names[PH_COUNT] = {
        "Media descriptor", "File allocation tables", "Directory structure", "File system",
        "Surface scan"
    };

    return names[phase];
}

const char *phase_info( int phase )
{
    switch ( phase ) {
    case PH_MEDIA:
        return "The media descriptor is one byte that says what kind of disk this is.  The "
               "boot sector has it, and so does the first entry of each file allocation "
               "table.  ScanDisk checks that the byte is one DOS knows and that the copies "
               "agree.";
    case PH_FAT:
        return "The file allocation table (FAT) records which clusters of the disk each file "
               "is in, and which are free.  A disk keeps two copies.  ScanDisk reads both, "
               "checks that they are the same, and counts the entries that point outside "
               "the disk.";
    case PH_DIRS:
        return "ScanDisk reads every directory on the drive and follows every file through "
               "the file allocation table.  It checks names and attributes, the \".\" and "
               "\"..\" entries of each directory, long file names, and that each file's size "
               "matches the clusters the file is in.";
    case PH_FILESYS:
        return "ScanDisk looks for lost clusters, which the file allocation table shows as "
               "in use although no file has them, and for cross-linked files, which are two "
               "files recorded as being in the same clusters.";
    }
    return "The surface scan reads every sector of the drive.  A cluster that cannot be read "
           "is marked as bad so that DOS never uses it again; if a file was in the cluster, "
           "ScanDisk first moves what can still be read to a good one.  The scan reads only, "
           "and writes nothing to a cluster that is sound.  Exit ends the scan; the clusters "
           "already found bad stay marked.";
}

/* ------------------------------------------------------------------ */
/* the kinds of problem                                                */
/* ------------------------------------------------------------------ */

const char *pb_info( int kind )
{
    switch ( kind ) {
    case PB_MEDIA:
        return "DOS looks at the media descriptor to tell what kind of disk it has been given, "
               "and may refuse a disk whose descriptor is wrong.  The repair writes the right "
               "byte to the boot sector and to each file allocation table.  No file is "
               "touched.";
    case PB_FATREAD:
        return "A disk keeps two copies of its file allocation table so that one can stand "
               "in for the other.  One copy has sectors that cannot be read.  The repair "
               "writes those sectors again from the copy that can; writing a sector often "
               "makes a drive replace it with a spare.  If the sectors still cannot be "
               "written, copy your files off the disk and stop using it.";
    case PB_FATMISMATCH:
        return "The two copies of the file allocation table should be the same.  When they "
               "differ, a write to the disk was interrupted or a sector has gone bad.  "
               "ScanDisk counts the entries of each copy that point outside the disk and "
               "takes the copy with fewer as the true one; the repair makes the other the "
               "same.  If you leave this, ScanDisk goes on with the copy DOS uses.";
    case PB_NAME:
        return "A file name may not contain small letters, spaces at its start, or any of "
               "\" * + , . / : ; < = > ? [ \\ ] |.  DOS cannot open, rename or delete a file "
               "whose name has them.  The repair makes small letters capitals and replaces "
               "the other characters with underscores; a long file name the file has is "
               "kept.";
    case PB_ATTR:
        return "Two of the eight attribute bits are not used, and an entry cannot be both a "
               "volume label and a directory.  The repair clears the bits that mean nothing "
               "and leaves the rest.";
    case PB_SIZE:
        return "A directory entry records how long its file is, and the file allocation "
               "table records which clusters the file is in.  The two disagree: the size is "
               "larger than the clusters can hold, or leaves whole clusters unused.  The "
               "repair sets the size to what the clusters hold.  The end of the file may "
               "then have data that does not belong to it; look at the file.";
    case PB_DOTS:
        return "Every directory except the root begins with two entries: \".\", which points "
               "at the directory itself, and \"..\", which points at the directory above.  "
               "The repair writes both again.  The files in the directory are not touched.";
    case PB_LFN:
        return "A long file name is kept in extra directory entries in front of the entry "
               "for the file.  These entries have no file after them, or do not fit the "
               "file that follows.  This happens when a program that knows nothing of long "
               "names deletes or renames a file.  The repair removes the entries; the files "
               "keep their short names.";
    case PB_LABEL:
        return "A volume label may not contain small letters or any of "
               "\" * + , . / : ; < = > ? [ \\ ] |.  The repair makes small letters capitals "
               "and replaces the other characters with underscores.";
    case PB_CHAIN:
        return "The file allocation table links the clusters of a file into a chain.  This "
               "chain has a link to a cluster that is free, is marked bad, is not on the "
               "disk at all, or is an earlier cluster of the same chain.  The repair ends "
               "the chain at the last good link, so the file is shorter by whatever came "
               "after.  Clusters that came after may turn up as lost clusters, which can be "
               "saved as files.";
    case PB_FIRST:
        return "A directory entry names the first cluster of its file.  This one names a "
               "cluster that is free, is marked bad, or is not on the disk.  Nothing of the "
               "file can be reached.  The repair makes the file an empty one; delete it "
               "afterwards.";
    case PB_BADDIR:
        return "This directory entry does not lead to a directory: the cluster it points at "
               "is not one a directory can be in, belongs to something else, or does not "
               "begin with the \".\" and \"..\" entries every directory has.  The repair "
               "deletes the entry.  If clusters belonged to it, they are found as lost "
               "clusters by the next check and can be saved as a file.";
    case PB_CROSS:
        return "Two files are recorded as using the same clusters.  Only one of them can "
               "have its own data there.  The repair gives the second file clusters of "
               "its own, with a copy of as much of the shared part as its size says it "
               "has, so that both files can be opened and deleted safely.  Afterwards "
               "look at both files: one of them is damaged from the shared cluster on.";
    case PB_LOST:
        return "Lost clusters are marked as in use, but no file has them.  They are left "
               "behind when a program is stopped while writing a file.  Save makes each "
               "chain of lost clusters a file named FILE0000.CHK, FILE0001.CHK and so on in "
               "the root directory, where you can look for anything worth keeping.  Delete "
               "frees the clusters; whatever was in them is gone.";
    case PB_BADCLUS:
        return "A sector of this cluster cannot be read.  The repair marks the cluster as "
               "bad in the file allocation table, so DOS never puts anything there again.  "
               "If a file was in the cluster, ScanDisk first copies every sector that can "
               "still be read to a free cluster and puts that cluster in the file instead; "
               "the sectors that cannot be read are filled with zeroes.  A disk that "
               "develops bad clusters is likely to develop more.";
    case PB_SYSAREA:
        return "The system area is the boot sector, the file allocation tables and the root "
               "directory.  They are at fixed places on the disk, so a bad sector there "
               "cannot be moved out of the way.  Copy your files to another disk as soon as "
               "you can and stop using this one.";
    case PB_DIRREAD:
        return "ScanDisk could not read this directory, so the files in it were not checked.  "
               "Clusters that belong to those files look like lost clusters, and ScanDisk "
               "leaves lost clusters alone on this run because of that.  A surface scan can "
               "move the readable part of the directory to a good cluster.";
    }
    return "";
}

/* ------------------------------------------------------------------ */
/* dates and times, as the country writes them (CHKDSK's)              */
/* ------------------------------------------------------------------ */

static char *two( u32 val, char *cur )
{
    cur[0] = (char)('0' + (val / 10) % 10);
    cur[1] = (char)('0' + val % 10);
    return cur + 2;
}

char *fmt_date( u16 dos_date, char *buf )
{
    u32 day = dos_date & 31, mon = (dos_date >> 5) & 15, year = 1980 + (dos_date >> 9);
    char *cur = buf;

    switch ( country.date_fmt ) {
    case 1:                                     /* D M Y */
        cur = two( day, cur );  *cur++ = country.date_sep;
        cur = two( mon, cur );  *cur++ = country.date_sep;
        sfmt( cur, 6, "%u", year );
        break;
    case 2:                                     /* Y M D */
        cur += sfmt( cur, 6, "%u", year );
        *cur++ = country.date_sep;  cur = two( mon, cur );
        *cur++ = country.date_sep;  cur = two( day, cur );
        *cur = 0;
        break;
    default:                                    /* M D Y */
        cur = two( mon, cur );  *cur++ = country.date_sep;
        cur = two( day, cur );  *cur++ = country.date_sep;
        sfmt( cur, 6, "%u", year );
        break;
    }
    return buf;
}

char *fmt_time( u16 dos_time, char *buf )
{
    u32 hour = dos_time >> 11, min = (dos_time >> 5) & 63;
    char *cur = buf;
    char ampm = 'a';

    if ( !country.time_24 ) {
        if ( hour >= 12 ) {
            ampm = 'p';
        }
        hour %= 12;
        if ( hour == 0 ) {
            hour = 12;
        }
    }
    if ( hour >= 10 ) {
        *cur++ = (char)('0' + hour / 10);
    }
    *cur++ = (char)('0' + hour % 10);
    *cur++ = country.time_sep;
    cur = two( min, cur );
    if ( !country.time_24 ) {
        *cur++ = ampm;
    }
    *cur = 0;
    return buf;
}

/* ------------------------------------------------------------------ */
/* the summary of a drive                                              */
/* ------------------------------------------------------------------ */

const char *drive_verdict( void )
{
    static char line[200];
    char drive = (char)('A' + vol.drive);
    u32 left = st.found - st.fixed;

    if ( !st.logical_done ) {
        sfmt( line, sizeof( line ), "ScanDisk did not finish checking drive %c:.", drive );
    } else if ( st.found == 0 ) {
        sfmt( line, sizeof( line ), "ScanDisk found no problems on drive %c:.", drive );
    } else if ( left == 0 ) {
        sfmt( line, sizeof( line ), "ScanDisk found %,u problem%s on drive %c: and repaired %s.",
              st.found, st.found == 1 ? "" : "s", drive,
              st.found == 1 ? "the problem" : "all of them" );
    } else {
        sfmt( line, sizeof( line ),
              "ScanDisk found %,u problem%s on drive %c:.  %,u %s not repaired.",
              st.found, st.found == 1 ? "" : "s", drive, left, left == 1 ? "was" : "were" );
    }
    return line;
}

static void row( TEXT *text, u32 clusters, const char *what, u32 count, const char *one,
                 const char *many )
{
    char num[40], line[120];

    fmt_bytes( mul32( clusters, vol.bpc ), num );
    if ( one ) {
        sfmt( line, sizeof( line ), "%18s bytes in %,u %s", num, count, count == 1 ? one : many );
    } else {
        sfmt( line, sizeof( line ), "%18s bytes %s", num, what );
    }
    tx_add( text, line );
}

void summary_text( TEXT *text )
{
    u32 avail;
    char num[40], line[200];

    tx_wrap( text, drive_verdict(), LOG_WIDTH );
    if ( st.surface == 1 ) {
        sfmt( line, sizeof( line ),
              "The surface scan read %,u cluster%s and found %,u that cannot be read.",
              st.surf_examined, st.surf_examined == 1 ? "" : "s", st.surf_bad );
        tx_wrap( text, line, LOG_WIDTH );
    } else if ( st.surface == 2 ) {
        sfmt( line, sizeof( line ),
              "The surface scan was stopped after %,u of %,u clusters; %,u cannot be read.",
              st.surf_examined, vol.nclus, st.surf_bad );
        tx_wrap( text, line, LOG_WIDTH );
    }
    if ( !st.logical_done || vol.map == NULL ) {
        return;
    }
    avail = fat_count_free();
    tx_add( text, "" );
    row( text, vol.nclus, "on the drive in all", 0, NULL, NULL );
    if ( st.hid_cnt ) {
        row( text, st.hid_clus, NULL, st.hid_cnt, "hidden file", "hidden files" );
    }
    if ( st.dir_cnt ) {
        row( text, st.dir_clus, NULL, st.dir_cnt, "directory", "directories" );
    }
    if ( st.root_clus ) {
        row( text, st.root_clus, "in the root directory", 0, NULL, NULL );
    }
    row( text, st.fil_clus, NULL, st.fil_cnt, "file", "files" );
    if ( st.orph_cnt ) {
        row( text, st.orph_clus, NULL, st.orph_cnt, "file saved from lost clusters",
             "files saved from lost clusters" );
    }
    if ( st.lost_kept ) {
        row( text, st.lost_kept, "in lost clusters", 0, NULL, NULL );
    }
    if ( st.bad_clus ) {
        row( text, st.bad_clus, "in clusters marked as bad", 0, NULL, NULL );
    }
    row( text, avail, "free", 0, NULL, NULL );
    tx_add( text, "" );
    fmt_bytes( (u64)vol.bpc, num );
    tx_addf( text, "%18s bytes in each cluster", num );
    tx_addf( text, "%,18u clusters on the drive", vol.nclus );
    tx_addf( text, "%,18u clusters free", avail );
}
