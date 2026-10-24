use strict; use warnings;
# MKEXFAT.PL - build an exFAT volume from a directory on this machine,
# to give PM-DOS's exFAT code volumes it did not make itself.
#
#   perl TOOLS\MKEXFAT.PL out.vol -mb N [options] [-tree DIR]
#
#   -mb N          the volume is N megabytes
#   -clus N[K]     bytes a cluster (4K by default); 512 up to 32M
#   -bps N         bytes a sector: 512 (the default), 1024, 2048, 4096
#   -label TEXT    the volume label (none by default)
#   -tree DIR      everything in DIR goes into the root, subdirectories
#                  and all.  A name may spell a character this machine's
#                  file names cannot hold as %XXXX (four hex digits):
#                  "caf%00E9.TXT" is put in as cafe with an acute accent
#   -chain         every file and directory gets a FAT chain, as if no
#                  allocation had ever been contiguous.  Without it a
#                  contiguous one is marked NoFatChain, as Windows marks
#                  one, and its FAT entries are left as STALE NONSENSE -
#                  pointers into other files - so that anything that
#                  follows the FAT for it reads the wrong clusters
#   -frag          allocate the files a cluster at a time, round robin,
#                  so every file of more than one cluster is fragmented
#                  and has to be read through its chain
#   -vdl NAME=N    the file NAME (in the root) gets ValidDataLength N,
#                  less than its size; the bytes past N on the disk are
#                  0xEE, which a reader must not hand back
#   -dirent N      pad the root with N extra files called PADnnnn.TXT,
#                  one cluster's worth of entries and more, so the root
#                  directory runs over several clusters
#   -upcase KIND   "latin1" (the default): the up-case table maps a-z and
#                  the Latin-1 letters; "ascii": a-z only.  Either is a
#                  legal table - a volume carries its own, and that is
#                  what its name hashes are made with
#   -time YYYYMMDDHHMMSS   every timestamp (20261023123456 by default)
#   -dirty         leave VolumeDirty set, as a volume nobody unmounted is
#   -offset N      PartitionOffset: the sector the volume starts at on its
#                  disk (MKGPT.PL patches this when it places a volume)
#
# The layout is the one Windows writes: a boot region of twelve sectors
# and its copy, the FAT at 128 sectors (24 on a volume too small for
# that), the cluster heap on a cluster boundary, and in it the bitmap,
# the up-case table and the root, in that order, before any file.

my $out = shift or die "usage: mkexfat.pl out.vol -mb N [options]\n";
my %o = (mb => 0, clus => 4096, bps => 512, label => '', tree => undef, chain => 0, frag => 0,
         dirent => 0, upcase => 'latin1', time => '20261023123456', dirty => 0, offset => 0);
my %vdl;
while (@ARGV) {
    my $a = shift;
    if    ($a eq '-mb')     { $o{mb} = shift }
    elsif ($a eq '-clus')   { $o{clus} = size(shift) }
    elsif ($a eq '-bps')    { $o{bps} = shift }
    elsif ($a eq '-label')  { $o{label} = shift }
    elsif ($a eq '-tree')   { $o{tree} = shift }
    elsif ($a eq '-chain')  { $o{chain} = 1 }
    elsif ($a eq '-frag')   { $o{frag} = 1 }
    elsif ($a eq '-dirent') { $o{dirent} = shift }
    elsif ($a eq '-upcase') { $o{upcase} = shift }
    elsif ($a eq '-time')   { $o{time} = shift }
    elsif ($a eq '-dirty')  { $o{dirty} = 1 }
    elsif ($a eq '-offset') { $o{offset} = shift }
    elsif ($a eq '-vdl')    { my ($n, $v) = split /=/, shift; $vdl{uc $n} = $v }
    else { die "mkexfat.pl: what is $a?\n" }
}
die "mkexfat.pl: -mb is wanted\n" unless $o{mb} > 0;
my $bps = $o{bps};
my $bshift = log2($bps);
die "mkexfat.pl: -bps must be 512, 1024, 2048 or 4096\n" unless $bshift >= 9 && $bshift <= 12;
my $spc = $o{clus} / $bps;
my $cshift = log2($spc);
die "mkexfat.pl: -clus must be a power of two, at least a sector, at most 32M\n"
    unless $cshift >= 0 && $o{clus} <= 32 * 1024 * 1024;
my $csize = $o{clus};

#--- the geometry ----------------------------------------------------------
my $vollen = $o{mb} * 1024 * 1024 / $bps;
my $fatoff = $vollen >= 4096 ? 128 : 24;
my ($fatlen, $heap, $nclus) = (1, 0, 0);
for (1 .. 8) {
    $heap = align($fatoff + $fatlen, $spc);
    $nclus = int(($vollen - $heap) / $spc);
    my $need = int((($nclus + 2) * 4 + $bps - 1) / $bps);
    last if $need <= $fatlen;
    $fatlen = $need;
}
die "mkexfat.pl: too small for any clusters\n" if $nclus < 1;

my $img = "\0" x ($vollen * $bps);
my @fat = (0xFFFFFFF8, 0xFFFFFFFF);
$fat[$_ + 2] = 0 for 0 .. $nclus - 1;
my @used = (0) x ($nclus + 2);
my $nextfree = 2;

#--- the up-case table --------------------------------------------------
my @up = (0 .. 0xFFFF);
$up[$_] = $_ - 0x20 for ord('a') .. ord('z');
if ($o{upcase} eq 'latin1') {
    for (0xE0 .. 0xFE) { $up[$_] = $_ - 0x20 unless $_ == 0xF7 }
    $up[0xFF] = 0x178;
    $up[0xB5] = 0x39C;
}
my @ucw;                        # compressed: FFFF n = n characters map to themselves
my $i = 0;
while ($i <= 0xFFFF) {
    if ($up[$i] == $i) {
        my $j = $i;
        $j++ while $j <= 0xFFFF && $up[$j] == $j && $j - $i < 0xFFFF;
        if ($j - $i > 2) { push @ucw, 0xFFFF, $j - $i; $i = $j; next }
    }
    push @ucw, $up[$i];
    $i++;
}
my $upcase = pack('v*', @ucw);
my $upsum = cksum32($upcase, -1);

#--- the tree to write ----------------------------------------------------
my ($tdate, $ttime) = stamp($o{time});
my $root = { name => '', dir => 1, kids => [] };
read_tree($root, $o{tree}) if defined $o{tree};
for my $n (1 .. $o{dirent}) {
    push @{$root->{kids}}, { name => sprintf('PAD%04d.TXT', $n), data => "pad $n\r\n" };
}

#--- the heap: bitmap, up-case, root, then everything else ---------------
my $bmlen = int(($nclus + 7) / 8);
my $bmfirst = alloc_run(int(($bmlen + $csize - 1) / $csize), 1);
my $upfirst = alloc_run(int((length($upcase) + $csize - 1) / $csize), 1);
plan_dirs($root);                       # how big each directory has to be
$root->{first} = alloc_run($root->{nclus}, 1);
alloc_tree($root);
write_tree($root);

# the bitmap, from @used
my $bm = "\0" x $bmlen;
for my $c (2 .. $nclus + 1) { vec($bm, $c - 2, 1) = 1 if $used[$c] }
put_clusters($bmfirst, $bm);
put_clusters($upfirst, $upcase);

# the root's own three entries in front of the files'
my $rent = '';
if ($o{label} ne '') {
    my @l = map { ord } split //, substr($o{label}, 0, 11);
    $rent .= pack('C C v11 x8', 0x83, scalar @l, @l, (0) x (11 - @l));
}
$rent .= pack('C C x18 V Q<', 0x81, 0, $bmfirst, $bmlen);
$rent .= pack('C x3 V x12 V Q<', 0x82, $upsum, $upfirst, length $upcase);
$root->{raw} = $rent . $root->{raw};
put_dir($root);

#--- the FAT ---------------------------------------------------------------
my $fatbytes = pack('V*', @fat[0 .. $nclus + 1]);
substr($img, $fatoff * $bps, length $fatbytes) = $fatbytes;

#--- the boot region, twice -------------------------------------------------
my $inuse = 0;
$inuse += $used[$_] for 2 .. $nclus + 1;
my $bs = "\xEB\x76\x90EXFAT   " . ("\0" x 53)
       . pack('Q< Q< V V V V V V v v C C C C C', $o{offset}, $vollen, $fatoff, $fatlen, $heap, $nclus,
              $root->{first}, 0x1D150A3F, 0x0100, $o{dirty} ? 2 : 0, $bshift, $cshift, 1, 0x80,
              int($inuse * 100 / $nclus));
$bs .= "\0" x (510 - length $bs) . "\x55\xAA";
$bs .= "\0" x ($bps - 512);
my $region = $bs;
for (1 .. 8) { $region .= ("\0" x ($bps - 4)) . "\0\0\x55\xAA" }
$region .= "\0" x (2 * $bps);           # OEM parameters, reserved
my $sum = 0;
my $k = 0;
for my $b (unpack('C*', $region)) {
    $sum = ((($sum & 1) ? 0x80000000 : 0) + ($sum >> 1) + $b) & 0xFFFFFFFF
        unless $k == 106 || $k == 107 || $k == 112;
    $k++;
}
$region .= pack('V', $sum) x ($bps / 4);
substr($img, 0, length $region) = $region;
substr($img, 12 * $bps, length $region) = $region;

open(my $fh, '>', $out) or die "$out: $!";
binmode $fh;
print $fh $img;
close $fh;
printf "%s: %d sectors of %d, cluster %d, %d clusters, FAT at %d (%d), heap at %d, root at cluster %d\n",
    $out, $vollen, $bps, $csize, $nclus, $fatoff, $fatlen, $heap, $root->{first};
exit 0;

#=========================================================================
sub size { my $v = shift; $v = $1 * 1024 if $v =~ /^(\d+)K$/i; $v = $1 * 1048576 if $v =~ /^(\d+)M$/i; return $v }
sub log2 { my $v = shift; my $n = 0; while ($v > 1) { return -1 if $v & 1; $v >>= 1; $n++ } return $n }
sub align { my ($v, $a) = @_; return int(($v + $a - 1) / $a) * $a }

sub stamp {
    my ($y, $mo, $d, $h, $mi, $s) = shift =~ /^(\d{4})(\d\d)(\d\d)(\d\d)(\d\d)(\d\d)$/
        or die "mkexfat.pl: -time YYYYMMDDHHMMSS\n";
    return ((($y - 1980) << 9) | ($mo << 5) | $d, ($h << 11) | ($mi << 5) | int($s / 2));
}

# the rotate-and-add both checksums are, 32 bits
sub cksum32 {
    my ($data, $skip) = @_;
    my $s = 0;
    for my $b (unpack('C*', $data)) { $s = ((($s & 1) ? 0x80000000 : 0) + ($s >> 1) + $b) & 0xFFFFFFFF }
    return $s;
}

sub read_tree {
    my ($node, $dir) = @_;
    opendir(my $dh, $dir) or die "$dir: $!";
    for my $e (sort grep { $_ ne '.' && $_ ne '..' } readdir $dh) {
        my $p = "$dir/$e";
        my $name = $e;
        $name =~ s/%([0-9A-Fa-f]{4})/chr(hex $1)/ge;
        if (-d $p) {
            my $kid = { name => $name, dir => 1, kids => [] };
            read_tree($kid, $p);
            push @{$node->{kids}}, $kid;
        } else {
            open(my $f, '<', $p) or die "$p: $!";
            binmode $f;
            local $/; my $data = <$f>;
            close $f;
            push @{$node->{kids}}, { name => $name, data => $data // '' };
        }
    }
    closedir $dh;
}

sub nslots { my $n = shift; return 2 + int((length($n->{name}) + 14) / 15) }

sub plan_dirs {
    my $d = shift;
    my $slots = $d == $root ? 3 : 0;            # label, bitmap, up-case
    for my $k (@{$d->{kids}}) {
        $slots += nslots($k);
        plan_dirs($k) if $k->{dir};
    }
    $slots++;                                    # room for the end marker
    $d->{nclus} = int(($slots * 32 + $csize - 1) / $csize) || 1;
}

# take N clusters in a row (contiguous), or one at a time when fragmenting
sub alloc_run {
    my ($n, $contig) = @_;
    my $first = $nextfree;
    die "mkexfat.pl: the volume is full\n" if $nextfree + $n - 1 > $nclus + 1;
    for my $c ($first .. $first + $n - 1) { $used[$c] = 1; $fat[$c] = $c + 1 }
    $fat[$first + $n - 1] = 0xFFFFFFFF;
    $nextfree += $n;
    return $first;
}

sub alloc_tree {
    my $d = shift;
    my @files = grep { !$_->{dir} && length $_->{data} } @{$d->{kids}};
    for my $k (grep { $_->{dir} } @{$d->{kids}}) { $k->{first} = alloc_run($k->{nclus}, 1); $k->{clus} = [$k->{first} .. $k->{first} + $k->{nclus} - 1] }
    if ($o{frag} && @files > 1) {
        # one cluster each, round robin, until every file has enough
        my %need = map { $_ => int((length($_->{data}) + $csize - 1) / $csize) } @files;
        my $more = 1;
        while ($more) {
            $more = 0;
            for my $f (@files) {
                next unless $need{$f};
                my $c = $nextfree++;
                die "mkexfat.pl: the volume is full\n" if $c > $nclus + 1;
                $used[$c] = 1;
                push @{$f->{clus}}, $c;
                $need{$f}--;
                $more = 1;
            }
        }
        for my $f (@files) {
            my @c = @{$f->{clus}};
            $fat[$c[$_]] = $c[$_ + 1] for 0 .. $#c - 1;
            $fat[$c[-1]] = 0xFFFFFFFF;
            $f->{first} = $c[0];
        }
    } else {
        for my $f (@files) {
            my $n = int((length($f->{data}) + $csize - 1) / $csize);
            $f->{first} = alloc_run($n, 1);
            $f->{clus} = [$f->{first} .. $f->{first} + $n - 1];
        }
    }
    alloc_tree($_) for grep { $_->{dir} } @{$d->{kids}};
}

sub is_contig { my $c = shift; return 0 unless $c && @$c; for (1 .. $#$c) { return 0 if $c->[$_] != $c->[$_ - 1] + 1 } return 1 }

sub write_tree {
    my $d = shift;
    my $raw = '';
    for my $k (@{$d->{kids}}) {
        my ($len, $vdl, $first) = (0, 0, 0);
        if ($k->{dir}) {
            write_tree($k);
            $len = $vdl = $k->{nclus} * $csize;
            $first = $k->{first};
        } elsif (length $k->{data}) {
            $len = length $k->{data};
            $vdl = $vdl{uc $k->{name}} // $len;
            $first = $k->{first};
            my $data = $k->{data};
            substr($data, $vdl) = "\xEE" x ($len - $vdl) if $vdl < $len && $d == $root;
            put_chain($k->{clus}, $data);
        }
        my $flags = 1;                           # AllocationPossible
        if ($first && !$o{chain} && is_contig($k->{clus})) {
            $flags |= 2;                         # NoFatChain - and the FAT
            for my $c (@{$k->{clus}}) {          # under it made wrong on purpose
                $fat[$c] = 2 + (($c * 7919) % $nclus);
            }
        }
        my @name = map { ord } split //, $k->{name};
        my $hash = 0;
        for my $ch (@name) {
            my $u = $up[$ch];
            for my $b ($u & 0xFF, $u >> 8) { $hash = ((($hash & 1) ? 0x8000 : 0) + ($hash >> 1) + $b) & 0xFFFF }
        }
        my $nname = int((@name + 14) / 15);
        my $attr = $k->{dir} ? 0x10 : 0x20;
        my $ts = ($tdate << 16) | $ttime;
        my $set = pack('C C v v v V V V C C C C C x7', 0x85, 1 + $nname, 0, $attr, 0, $ts, $ts, $ts, 0, 0, 0, 0, 0);
        $set .= pack('C C x C v x2 Q< x4 V Q<', 0xC0, $flags, scalar @name, $hash, $vdl, $first, $len);
        for my $j (0 .. $nname - 1) {
            my @part = @name[$j * 15 .. ($j * 15 + 14 > $#name ? $#name : $j * 15 + 14)];
            push @part, (0) x (15 - @part);
            $set .= pack('C C v15', 0xC1, 0, @part);
        }
        my $sum = 0;
        my $n = 0;
        for my $b (unpack('C*', $set)) {
            $sum = ((($sum & 1) ? 0x8000 : 0) + ($sum >> 1) + $b) & 0xFFFF unless $n == 2 || $n == 3;
            $n++;
        }
        substr($set, 2, 2) = pack('v', $sum);
        $raw .= $set;
    }
    $d->{raw} = $raw;
    put_dir($d) unless $d == $root;
}

sub put_dir {
    my $d = shift;
    my $raw = $d->{raw};
    die "mkexfat.pl: directory overflow\n" if length($raw) > $d->{nclus} * $csize;
    $raw .= "\0" x ($d->{nclus} * $csize - length $raw);
    my @c = ($d->{first} .. $d->{first} + $d->{nclus} - 1);
    put_chain(\@c, $raw);
    if ($o{chain} || $d == $root) {
        $fat[$c[$_]] = $c[$_ + 1] for 0 .. $#c - 1;   # the root's chain is always
        $fat[$c[-1]] = 0xFFFFFFFF;                      # in the FAT
    }
}

sub put_chain {
    my ($c, $data) = @_;
    for my $j (0 .. $#$c) {
        my $piece = substr($data, $j * $csize, $csize);
        last unless length $piece;
        substr($img, ($heap + ($c->[$j] - 2) * $spc) * $bps, length $piece) = $piece;
    }
}

sub put_clusters { my ($first, $data) = @_; my $n = int((length($data) + $csize - 1) / $csize); put_chain([$first .. $first + $n - 1], $data) }
