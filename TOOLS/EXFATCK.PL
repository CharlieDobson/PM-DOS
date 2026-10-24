use strict; use warnings;
# EXFATCK.PL - check an exFAT volume against the specification, list
# it, and take files out of it.  The other half of MKEXFAT.PL: written
# from the format, not from PM-DOS's code, so that what PM-DOS writes
# is judged by something that did not make the same mistakes.
#
#   perl TOOLS\EXFATCK.PL image [-base N] [-list] [-x DIR] [-q]
#
# The volume is found by itself: sector 0 if that is an exFAT boot
# sector, else the first Basic Data partition of a GPT, else the first
# 07h entry of an MBR, whose volume says EXFAT.  -base N says where it
# is instead.  -list prints every file with its size and an MD5;
# -x DIR copies the tree out to DIR.  -q prints only the problems.
#
# What it checks: the boot sector's fields, both boot regions and their
# checksums; FAT[0] and FAT[1]; the root's bitmap and up-case entries,
# the up-case table's checksum; every directory entry set - its order,
# its SetChecksum, the name's hash under THIS volume's up-case table,
# the characters a name may not hold, the same name twice in one
# directory; every file's and directory's clusters - a chain that ends
# where its length says, a NoFatChain run inside the heap - and then
# the bitmap against the lot: a cluster two files claim, a cluster in
# use the bitmap calls free, a cluster the bitmap holds that nothing
# uses.  An entry after a directory's end marker that is not one too.
#
# Exit code 0 if it is clean, 1 if anything was found.

use Digest::MD5 qw(md5_hex);

my $file = shift or die "usage: exfatck.pl image [-base N] [-list] [-x DIR] [-q]\n";
my ($base, $list, $xdir, $quiet) = (undef, 0, undef, 0);
while (@ARGV) {
    my $a = shift;
    if    ($a eq '-base') { $base = shift }
    elsif ($a eq '-list') { $list = 1 }
    elsif ($a eq '-x')    { $xdir = shift }
    elsif ($a eq '-q')    { $quiet = 1 }
    else { die "exfatck.pl: what is $a?\n" }
}
open(my $fh, '<', $file) or die "$file: $!";
binmode $fh;
my $errors = 0;
sub bad { $errors++; print "ERROR: ", @_, "\n" }
sub warnf { print "warning: ", @_, "\n" }
sub rd { my ($off, $len) = @_; seek($fh, $off, 0); my $b = ''; read($fh, $b, $len); return $b }

#--- where is it ------------------------------------------------------
unless (defined $base) {
    my $s0 = rd(0, 512);
    if (substr($s0, 3, 8) eq 'EXFAT   ') { $base = 0 }
    else {
        my @pe = map { [unpack('x4 C x3 V V', substr($s0, 0x1BE + $_ * 16, 16))] } 0 .. 3;
        if (grep { $_->[0] == 0xEE } @pe) {
            my $h = rd(512, 512);
            my ($alba, $n, $esz) = unpack('x72 Q< V V', $h);
            for my $i (0 .. $n - 1) {
                my $e = rd($alba * 512 + $i * $esz, 128);
                next unless unpack('H8', $e) eq 'a2a0d0eb';
                my $st = unpack('x32 Q<', $e);
                if (substr(rd($st * 512, 16), 3, 8) eq 'EXFAT   ') { $base = $st; last }
            }
        } else {
            for my $p (@pe) {
                next unless $p->[0] == 0x07;
                if (substr(rd($p->[1] * 512, 16), 3, 8) eq 'EXFAT   ') { $base = $p->[1]; last }
            }
        }
    }
    die "exfatck.pl: no exFAT volume found in $file\n" unless defined $base;
}

#--- the boot sector ----------------------------------------------------
my $bs = rd($base * 512, 512);
die "exfatck.pl: no EXFAT signature at sector $base\n" unless substr($bs, 3, 8) eq 'EXFAT   ';
my ($poff, $vlen, $fatoff, $fatlen, $heap, $nclus, $rootc, $serial, $rev, $vflags, $bsh, $csh, $nfat, $drv, $pct)
    = unpack('Q< Q< V V V V V V v v C C C C C', substr($bs, 64, 49));
bad("the jump at the front is not EB 76 90") unless substr($bs, 0, 3) eq "\xEB\x76\x90";
bad("MustBeZero (bytes 11-63) is not zero") unless substr($bs, 11, 53) eq "\0" x 53;
bad("no boot signature AA55") unless substr($bs, 510, 2) eq "\x55\xAA";
bad("BytesPerSectorShift $bsh is outside 9-12") if $bsh < 9 || $bsh > 12;
bad("SectorsPerClusterShift $csh is too big") if $bsh + $csh > 25;
bad("NumberOfFats $nfat is not 1 or 2") unless $nfat == 1 || $nfat == 2;
bad(sprintf "FileSystemRevision %04X is not 1.00", $rev) unless $rev == 0x0100;
bad("PartitionOffset $poff, but the volume is at sector $base") if $poff != $base && $poff != 0;
warnf("PartitionOffset is zero") if $poff == 0 && $base != 0;
my $bps = 1 << $bsh;
my $spc = 1 << $csh;
my $csize = $bps * $spc;
my $vbase = $base * 512;
bad("FatOffset $fatoff is in the boot regions") if $fatoff < 24;
bad("FatLength $fatlen cannot hold ", $nclus + 2, " entries") if $fatlen * $bps < ($nclus + 2) * 4;
bad("ClusterHeapOffset $heap overlaps the FAT") if $heap < $fatoff + $fatlen * $nfat;
bad("ClusterCount $nclus does not fit the volume") if $heap + $nclus * $spc > $vlen;
bad("the root's first cluster $rootc is outside the heap") if $rootc < 2 || $rootc > $nclus + 1;
my @size = (-s $file);
bad("the volume runs past the end of the image") if $vbase + $vlen * $bps > $size[0];

# both boot regions and their checksums
my @region;
for my $r (0, 1) {
    my $reg = rd($vbase + $r * 12 * $bps, 12 * $bps);
    my $sum = 0;
    my $k = 0;
    for my $b (unpack('C*', substr($reg, 0, 11 * $bps))) {
        $sum = ((($sum & 1) ? 0x80000000 : 0) + ($sum >> 1) + $b) & 0xFFFFFFFF
            unless $k == 106 || $k == 107 || $k == 112;
        $k++;
    }
    my $name = $r ? 'backup' : 'main';
    bad("the $name boot checksum sector does not hold the checksum")
        unless substr($reg, 11 * $bps, $bps) eq pack('V', $sum) x ($bps / 4);
    for my $s (1 .. 8) {
        bad("the $name region's extended boot sector $s has no AA550000 signature")
            unless substr($reg, ($s + 1) * $bps - 4, 4) eq "\0\0\x55\xAA";
    }
    push @region, $reg;
}
{
    my ($m, $b) = @region;
    substr($_, 106, 2) = "\0\0" for $m, $b;
    substr($_, 112, 1) = "\0" for $m, $b;
    bad("the backup boot region is not a copy of the main one") unless $m eq $b;
}
printf "volume at sector %d: %d sectors of %d, cluster %d, %d clusters, FAT %d+%d, heap %d, root %d, flags %04X, %d%% used\n",
    $base, $vlen, $bps, $csize, $nclus, $fatoff, $fatlen, $heap, $rootc, $vflags, $pct unless $quiet;
warnf("VolumeDirty is set") if $vflags & 2;

#--- the FAT and the heap -------------------------------------------------
my $fatbin = rd($vbase + $fatoff * $bps, ($nclus + 2) * 4);
my @fat = unpack('V*', $fatbin);
bad(sprintf "FAT[0] is %08X, not FFFFFFF8", $fat[0]) unless $fat[0] == 0xFFFFFFF8;
bad(sprintf "FAT[1] is %08X, not FFFFFFFF", $fat[1]) unless $fat[1] == 0xFFFFFFFF;
my @owner;                      # who claims each cluster
sub cread { my $c = shift; return rd($vbase + ($heap + ($c - 2) * $spc) * $bps, $csize) }

# the clusters of a chain: [first, count, nofatchain] -> list, claiming
sub clusters {
    my ($first, $len, $nofat, $who, $isdir) = @_;
    my $need = defined $len ? int(($len + $csize - 1) / $csize) : 0;
    my @c;
    return () unless $first || $len;
    if (!$first) { bad("$who: DataLength $len but no first cluster"); return () }
    if ($first < 2 || $first > $nclus + 1) { bad("$who: first cluster $first is outside the heap"); return () }
    if ($nofat) {
        bad("$who: a NoFatChain run of $need runs off the heap") if $first + $need - 1 > $nclus + 1;
        @c = ($first .. $first + $need - 1);
    } else {
        my $c = $first;
        my %seen;
        while (1) {
            if ($c < 2 || $c > $nclus + 1) { bad(sprintf "%s: the chain reaches %08X", $who, $c); last }
            if ($seen{$c}++) { bad("$who: the chain loops at $c"); last }
            push @c, $c;
            my $n = $fat[$c];
            last if $n == 0xFFFFFFFF;
            if ($n == 0) { bad("$who: cluster ${c}'s FAT entry is 0 in the middle of a chain"); last }
            if ($n == 0xFFFFFFF7) { bad("$who: cluster ${c}'s chain runs into a bad-cluster mark"); last }
            $c = $n;
        }
        if (defined $len) {
            if ($isdir) { bad("$who: the chain has ", scalar @c, " clusters, DataLength says $need") if @c != $need }
            elsif (@c < $need) { bad("$who: the chain has ", scalar @c, " clusters, DataLength needs $need") }
            elsif (@c > $need) { bad("$who: the chain has ", scalar @c, " clusters, DataLength needs only $need") }
        }
    }
    for my $c (@c) {
        if (defined $owner[$c]) { bad("cluster $c belongs to both $owner[$c] and $who") }
        else { $owner[$c] = $who }
    }
    return @c;
}

sub readall { my @c = @_; return join '', map { cread($_) } @c }

#--- the root, and its system entries -------------------------------------
my @rootc = clusters($rootc, undef, 0, '\\', 1);
my $rootdata = readall(@rootc);
my (@bm, $bmdata, @up);
my ($bmseen, $upseen, $lblseen) = (0, 0, 0);
for (my $o = 0; $o < length $rootdata; $o += 32) {
    my $t = ord substr($rootdata, $o, 1);
    last if $t == 0;
    if ($t == 0x81) {
        $bmseen++;
        my ($flags, $first, $len) = unpack('x C x18 V Q<', substr($rootdata, $o, 32));
        bad("the bitmap is ", $len, " bytes, ", int(($nclus + 7) / 8), " are wanted") if $len != int(($nclus + 7) / 8);
        my @c = clusters($first, $len, 0, '(bitmap)');
        $bmdata = substr(readall(@c), 0, $len) if ($flags & 1) == 0;
    } elsif ($t == 0x82) {
        $upseen++;
        my ($sum, $first, $len) = unpack('x4 V x12 V Q<', substr($rootdata, $o, 32));
        my @c = clusters($first, $len, 0, '(up-case table)');
        my $tab = substr(readall(@c), 0, $len);
        my $s = 0;
        for my $b (unpack('C*', $tab)) { $s = ((($s & 1) ? 0x80000000 : 0) + ($s >> 1) + $b) & 0xFFFFFFFF }
        bad(sprintf "the up-case table's checksum is %08X, the entry says %08X", $s, $sum) if $s != $sum;
        my @w = unpack('v*', $tab);
        my $ch = 0;
        for (my $i = 0; $i < @w; $i++) {
            if ($w[$i] == 0xFFFF && $i + 1 < @w) { $up[$ch] = $ch, $ch++ for 1 .. $w[++$i]; next }
            $up[$ch++] = $w[$i];
        }
    } elsif ($t == 0x83) {
        $lblseen++;
        my ($n, @l) = unpack('x C v11', substr($rootdata, $o, 32));
        bad("the volume label is $n characters long") if $n > 11;
        printf "label: %s\n", join('', map { chr } @l[0 .. $n - 1]) if $n && !$quiet;
    }
}
bad("the root has $bmseen bitmap entries") unless $bmseen == $nfat;
bad("the root has $upseen up-case table entries") unless $upseen == 1;
bad("the root has $lblseen volume label entries") if $lblseen > 1;
for my $c (0 .. 0xFFFF) { $up[$c] //= $c }

#--- every directory --------------------------------------------------------
my @dirq = (['\\', $rootdata, 1]);
my $nfiles = 0;
while (my $d = shift @dirq) {
    my ($path, $data, $isroot) = @$d;
    my %names;
    my $ended = 0;
    my $nent = length($data) / 32;
    for (my $i = 0; $i < $nent; $i++) {
        my $e = substr($data, $i * 32, 32);
        my $t = ord $e;
        if ($ended) { bad("$path: entry $i (type ", sprintf('%02X', $t), ") follows the end marker") if $t; next }
        if ($t == 0) { $ended = 1; next }
        next unless $t & 0x80;                       # deleted
        next if $isroot && ($t == 0x81 || $t == 0x82 || $t == 0x83);
        if ($t == 0xA0) { next }                     # volume GUID
        if ($t != 0x85) {
            bad("$path: entry $i is a secondary ", sprintf('%02X', $t), " with no file entry in front") if $t & 0x40;
            bad("$path: entry $i has the unknown critical primary type ", sprintf('%02X', $t)) unless $t & 0x20;
            next;
        }
        my ($sc, $sum, $attr) = unpack('x C v v', $e);
        if ($sc < 2 || $sc > 18) { bad("$path: entry ${i}'s SecondaryCount is $sc"); next }
        if ($i + $sc >= $nent) { bad("$path: entry ${i}'s set runs off the directory"); next }
        my $set = substr($data, $i * 32, ($sc + 1) * 32);
        my $s = 0;
        my $k = 0;
        for my $b (unpack('C*', $set)) { $s = ((($s & 1) ? 0x8000 : 0) + ($s >> 1) + $b) & 0xFFFF unless $k == 2 || $k == 3; $k++ }
        my $st = substr($set, 32, 32);
        if (ord($st) != 0xC0) { bad("$path: entry ${i}'s first secondary is not a stream extension"); next }
        my ($flags, $nlen, $hash, $vdl, $first, $len) = unpack('x C x C v x2 Q< x4 V Q<', $st);
        my $nn = int(($nlen + 14) / 15);
        my @name;
        for my $j (1 .. $nn) {
            my $ne = substr($set, (1 + $j) * 32, 32);
            if (ord($ne) != 0xC1) { bad("$path: entry ${i}'s name entry $j is type ", sprintf('%02X', ord $ne)); last }
            push @name, unpack('x2 v15', $ne);
        }
        splice(@name, $nlen) if @name > $nlen;
        my $name = join '', map { $_ < 128 ? chr : sprintf('%%%04X', $_) } @name;
        my $full = $path eq '\\' ? "\\$name" : "$path\\$name";
        bad("$full: SetChecksum is ", sprintf('%04X', $sum), ", the set sums to ", sprintf('%04X', $s)) if $s != $sum;
        bad("$full: SecondaryCount $sc, but a name of $nlen wants at least ", $nn + 1) if $sc < $nn + 1;
        bad("$full: NameLength is 0") if $nlen == 0;
        my $h = 0;
        for my $c (@name) { my $u = $up[$c]; for my $b ($u & 0xFF, $u >> 8) { $h = ((($h & 1) ? 0x8000 : 0) + ($h >> 1) + $b) & 0xFFFF } }
        bad("$full: NameHash is ", sprintf('%04X', $hash), ", the name hashes to ", sprintf('%04X', $h)) if $h != $hash;
        bad("$full: a character the name may not hold") if grep { $_ < 0x20 || index('"*/:<>?\\|', chr($_ < 128 ? $_ : 1)) >= 0 } @name;
        my $key = join ',', map { $up[$_] } @name;
        bad("$full: the same name twice in $path") if $names{$key}++;
        bad("$full: ValidDataLength $vdl is past DataLength $len") if $vdl > $len;
        bad("$full: DataLength $len but AllocationPossible is clear") if $len && !($flags & 1);
        my $isdir = $attr & 0x10;
        if ($isdir) {
            bad("$full: a directory's DataLength $len is not whole clusters") if $len % $csize;
            bad("$full: a directory's ValidDataLength $vdl is not its DataLength") if $vdl != $len;
            bad("$full: an empty directory") unless $len;
        }
        my @c = clusters($first, $len, $flags & 2, $full, $isdir);
        $nfiles++;
        if ($isdir) {
            push @dirq, [$full, substr(readall(@c), 0, $len), 0];
            printf "%-50s <DIR>%s\n", $full, ($flags & 2) ? ' run' : '' if $list;
        } else {
            my $content = substr(readall(@c), 0, $len);
            substr($content, $vdl) = "\0" x ($len - $vdl) if $vdl < $len;
            printf "%-50s %10d %s%s%s\n", $full, $len, md5_hex($content), ($flags & 2) ? ' run' : '',
                $vdl < $len ? " vdl=$vdl" : '' if $list;
            if (defined $xdir) {
                (my $hp = "$xdir$full") =~ s/\\/\//g;
                (my $hd = $hp) =~ s/\/[^\/]*$//;
                mkpath($hd);
                open(my $o, '>', $hp) or die "$hp: $!";
                binmode $o;
                print $o $content;
                close $o;
            }
        }
        $i += $sc;
    }
}

#--- the bitmap against what is used ----------------------------------------
if (defined $bmdata) {
    my ($lost, $unmarked, $inuse) = (0, 0, 0);
    for my $c (2 .. $nclus + 1) {
        my $bit = vec($bmdata, $c - 2, 1);
        $inuse++ if $bit;
        if ($bit && !defined $owner[$c]) { $lost++; bad("cluster $c is marked in use and nothing uses it") if $lost <= 10 }
        if (!$bit && defined $owner[$c]) { $unmarked++; bad("cluster $c ($owner[$c]) is in use and the bitmap calls it free") if $unmarked <= 10 }
    }
    bad("$lost lost clusters in all") if $lost > 10;
    bad("$unmarked unmarked clusters in all") if $unmarked > 10;
    if ($pct != 0xFF) {
        my $real = int($inuse * 100 / $nclus);
        warnf("PercentInUse says $pct, the bitmap says $real") if abs($real - $pct) > 1;
    }
    printf "%d files and directories, %d of %d clusters in use\n", $nfiles, $inuse, $nclus unless $quiet;
}

print $errors ? "$errors problem(s)\n" : "clean\n";
exit($errors ? 1 : 0);

sub mkpath { my $p = shift; my $acc = ''; for my $part (split /\//, $p) { $acc .= $part . '/'; mkdir $acc unless -d $acc } }
