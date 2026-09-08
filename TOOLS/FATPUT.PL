use strict; use warnings;
# fatput.pl <image> <hostdir> <DOSNAME>  - graft a host directory tree
# into the root of a FAT16 volume, with or without an MBR in front.
#
# 8.3 names only, which is all Windows 3.x has.  Written because there
# is no mtools on this machine and MKDISK.EXE takes a flat file list.

my ($img, $host, $dosname) = @ARGV;
die "usage: fatput.pl <image> <hostdir> <DOSNAME>\n" unless defined $dosname;

open(my $fh, '+<', $img) or die "$img: $!";
binmode $fh;
my $imgdata = do { local $/; <$fh> };
my $imglen  = length($imgdata);

#--- find the volume -------------------------------------------------
my $base = 0;
my $bps0 = unpack('v', substr($imgdata, 11, 2));
if ($bps0 != 512) {
    my $pt = substr($imgdata, 0x1BE, 16);
    die "no FAT volume and no partition table\n" unless ord(substr($pt,4,1));
    $base = unpack('V', substr($pt, 8, 4)) * 512;
}

my $bs    = substr($imgdata, $base, 512);
my $bps   = unpack('v', substr($bs,11,2));
my $spc   = ord(substr($bs,13,1));
my $res   = unpack('v', substr($bs,14,2));
my $nfat  = ord(substr($bs,16,1));
my $nroot = unpack('v', substr($bs,17,2));
my $spf   = unpack('v', substr($bs,22,2));
my $tot16 = unpack('v', substr($bs,19,2));
my $tot32 = unpack('V', substr($bs,32,4));
my $tot   = $tot16 ? $tot16 : $tot32;
die "bytes per sector $bps, expected 512\n" unless $bps == 512;
die "no root directory - this looks like FAT32\n" unless $nroot;

my $rootsec = $res + $nfat * $spf;
my $rootlen = int(($nroot * 32 + $bps - 1) / $bps);
my $datasec = $rootsec + $rootlen;
my $nclus   = int(($tot - $datasec) / $spc);
die "FAT12 volume - this tool only does FAT16\n" if $nclus < 4085;
my $csize   = $spc * $bps;

printf STDERR "volume at sector %d: %d clusters of %d bytes, root %d entries\n",
       $base/512, $nclus, $csize, $nroot;

#--- byte offsets ----------------------------------------------------
sub off_sec  { my $s = shift; return $base + $s * $bps; }
sub off_clus { my $c = shift; return off_sec($datasec + ($c - 2) * $spc); }

sub fat_get {
    my $c = shift;
    return unpack('v', substr($imgdata, off_sec($res) + $c*2, 2));
}
sub fat_set {
    my ($c, $v) = @_;
    for my $n (0 .. $nfat-1) {
        substr($imgdata, off_sec($res + $n*$spf) + $c*2, 2) = pack('v', $v);
    }
}

my $next_free = 2;
sub alloc_clus {
    while ($next_free < $nclus + 2) {
        if (fat_get($next_free) == 0) {
            my $c = $next_free++;
            fat_set($c, 0xFFFF);
            substr($imgdata, off_clus($c), $csize) = "\x00" x $csize;
            return $c;
        }
        $next_free++;
    }
    die "the volume is full\n";
}

#--- 8.3 names -------------------------------------------------------
sub name83 {
    my $n = uc shift;
    $n =~ s/[^A-Z0-9_\^\$\~\!\#\%\&\-\{\}\(\)\@\'\`\.]/_/g;
    my ($b, $e) = ($n, '');
    if ($n =~ /^(.*)\.([^.]*)$/) { ($b, $e) = ($1, $2); }
    $b =~ s/\.//g;
    $b = substr($b, 0, 8);
    $e = substr($e, 0, 3);
    return sprintf("%-8s%-3s", $b, $e);
}

#--- directories -----------------------------------------------------
# A directory is either the root (a fixed run of sectors) or a cluster
# chain.  dir_slots returns the byte offset of every entry position it
# has, growing a chained directory by one cluster when it runs out.
sub dir_slot {
    my $dir = shift;
    if ($dir->{root}) {
        for my $i (0 .. $nroot - 1) {
            my $o = off_sec($rootsec) + $i * 32;
            my $f = substr($imgdata, $o, 1);
            return $o if $f eq "\x00" or $f eq "\xE5";
        }
        die "the root directory is full\n";
    }
    my $c = $dir->{clus};
    my $last = $c;
    while (1) {
        for my $i (0 .. int($csize/32) - 1) {
            my $o = off_clus($c) + $i * 32;
            my $f = substr($imgdata, $o, 1);
            return $o if $f eq "\x00" or $f eq "\xE5";
        }
        $last = $c;
        my $nx = fat_get($c);
        last if $nx < 2 || $nx >= 0xFFF8;
        $c = $nx;
    }
    my $new = alloc_clus();
    fat_set($last, $new);
    return off_clus($new);
}

# 1 Jan 1994, 00:00 - a fixed stamp, so two runs of this tool produce
# identical images and a diff means something.
my $DDATE = ((1994 - 1980) << 9) | (1 << 5) | 1;
my $DTIME = 0;

sub put_entry {
    my ($off, $n83, $attr, $clus, $size) = @_;
    my $e = $n83
          . chr($attr)
          . ("\x00" x 10)
          . pack('v', $DTIME)
          . pack('v', $DDATE)
          . pack('v', $clus)
          . pack('V', $size);
    die "entry is " . length($e) . " bytes\n" unless length($e) == 32;
    substr($imgdata, $off, 32) = $e;
}

sub add_file {
    my ($dir, $name, $data) = @_;
    my $size = length($data);
    my $first = 0;
    if ($size) {
        my ($prev, $p) = (0, 0);
        while ($p < $size) {
            my $c = alloc_clus();
            $first = $c unless $first;
            fat_set($prev, $c) if $prev;
            my $chunk = substr($data, $p, $csize);
            substr($imgdata, off_clus($c), length($chunk)) = $chunk;
            $prev = $c;
            $p += $csize;
        }
    }
    put_entry(dir_slot($dir), name83($name), 0x20, $first, $size);
    return $size;
}

sub add_dir {
    my ($dir, $name) = @_;
    my $c = alloc_clus();
    my $parent = $dir->{root} ? 0 : $dir->{clus};
    put_entry(off_clus($c),      sprintf("%-11s", "."),  0x10, $c,       0);
    put_entry(off_clus($c) + 32, sprintf("%-11s", ".."), 0x10, $parent,  0);
    put_entry(dir_slot($dir), name83($name), 0x10, $c, 0);
    return { clus => $c };
}

#--- walk ------------------------------------------------------------
my $files = 0;
my $bytes = 0;
sub graft {
    my ($hostpath, $dir) = @_;
    opendir(my $dh, $hostpath) or die "$hostpath: $!";
    my @ents = sort grep { $_ ne '.' && $_ ne '..' } readdir($dh);
    closedir $dh;
    for my $e (@ents) {
        my $p = "$hostpath/$e";
        if (-d $p) {
            graft($p, add_dir($dir, $e));
        } elsif (-f $p) {
            open(my $f, '<', $p) or die "$p: $!";
            binmode $f;
            my $d = do { local $/; <$f> };
            close $f;
            $bytes += add_file($dir, $e, $d);
            $files++;
        }
    }
}

my $top = add_dir({ root => 1 }, $dosname);
graft($host, $top);

die "image grew from $imglen to " . length($imgdata) . "\n"
    unless length($imgdata) == $imglen;

seek($fh, 0, 0);
print $fh $imgdata;
close $fh;

printf STDERR "grafted %d files, %d bytes into \\%s (next free cluster %d of %d)\n",
       $files, $bytes, $dosname, $next_free, $nclus + 1;
