use strict; use warnings;
# FATREPL.PL - put new copies of files onto a FAT16 image, in place.
#
#     perl TOOLS\FATREPL.PL <image> [-dir NAME] <hostfile>... [-dir NAME <hostfile>...]
#
# Each host file goes into the root (or into the directory the last -dir
# named, which must already exist there) under its own 8.3 name,
# upper-cased.  A file of that name already there has its clusters
# freed and its entry rewritten; a new one takes a free entry.
#
# WHY IT EXISTS: MKHD.CMD builds a hard-disk image from nothing, and an
# image can carry things MKHD does not know about - a Windows install
# grafted in with FATPUT.PL, most of all.  Rebuilding such an image to
# refresh PMDOS.SYS would take the graft with it.  This changes only the
# files it is given.
#
# FAT16 only, with or without an MBR in front, 8.3 names only; it does
# not touch long-name entries, and refuses a name that has one in front
# of it rather than orphan the chain.  A cluster another entry also
# points into is not freed (see claimed() below).

my $img = shift or die "usage: fatrepl.pl <image> [-dir NAME] <hostfile>...\n";
open(my $fh, '+<', $img) or die "$img: $!";
binmode $fh;
my $d = do { local $/; <$fh> };

#--- the volume -------------------------------------------------------
my $base = 0;
if (unpack('v', substr($d, 11, 2)) != 512) {
    my $pt = substr($d, 0x1BE, 16);
    die "no FAT volume and no partition table\n" unless ord(substr($pt, 4, 1));
    $base = unpack('V', substr($pt, 8, 4)) * 512;
}
my $bs    = substr($d, $base, 512);
my $spc   = ord(substr($bs, 13, 1));
my $res   = unpack('v', substr($bs, 14, 2));
my $nfat  = ord(substr($bs, 16, 1));
my $nroot = unpack('v', substr($bs, 17, 2));
my $spf   = unpack('v', substr($bs, 22, 2));
my $tot   = unpack('v', substr($bs, 19, 2)) || unpack('V', substr($bs, 32, 4));
die "no fixed root: this looks like FAT32\n" unless $nroot;
my $rootoff = $base + ($res + $nfat * $spf) * 512;
my $dataoff = $rootoff + int(($nroot * 32 + 511) / 512) * 512;
my $nclus   = int(($tot - ($dataoff - $base) / 512) / $spc) + 2;
die "a FAT12 volume: this tool only does FAT16\n" if $nclus < 4085;
my $csize   = $spc * 512;
my $fatoff  = $base + $res * 512;

sub fat_get { my $c = shift; unpack('v', substr($d, $fatoff + $c * 2, 2)) }
sub fat_set {
    my ($c, $v) = @_;
    for my $f (0 .. $nfat - 1) {
        substr($d, $fatoff + $f * $spf * 512 + $c * 2, 2) = pack('v', $v);
    }
}
sub clus_off { $dataoff + ($_[0] - 2) * $csize }

#--- a directory as a list of entry offsets in the image --------------
sub dir_slots {
    my $clus = shift;
    my @s;
    if (!$clus) { push @s, $rootoff + $_ * 32 for 0 .. $nroot - 1; return @s }
    while ($clus >= 2 && $clus < 0xFFF8) {
        push @s, clus_off($clus) + $_ * 32 for 0 .. $csize / 32 - 1;
        $clus = fat_get($clus);
    }
    return @s;
}

sub name83 {
    my $n = uc shift;
    $n =~ s{.*[\\/]}{};
    my ($b, $e) = $n =~ /^([^.]*)(?:\.(.*))?$/;
    $e = '' unless defined $e;
    die "$n: not an 8.3 name\n" if length($b) < 1 || length($b) > 8 || length($e) > 3;
    return sprintf('%-8s%-3s', $b, $e);
}

sub find_entry {
    my ($dirclus, $n83) = @_;
    my ($hit, $free, $prev);
    for my $o (dir_slots($dirclus)) {
        my $c = ord(substr($d, $o, 1));
        if ($c == 0) { $free //= $o; last }
        if ($c == 0xE5) { $free //= $o; $prev = undef; next }
        my $a = ord(substr($d, $o + 11, 1));
        if (($a & 0x3F) == 0x0F) { $prev = $o; next }
        if (substr($d, $o, 11) eq $n83) {
            die "$n83 carries a long name: this tool does not rewrite those\n" if defined $prev;
            $hit = $o; last;
        }
        $prev = undef;
    }
    return ($hit, $free);
}

#--- clusters some OTHER entry still owns --------------------------------
# MKDISK cross-links on purpose: the long-named LFNTEST fixtures in the
# root all point at one cluster of the last file it wrote (LFNTEST only
# reads them).  Freeing that file's chain blindly would leave those
# entries pointing at free space the next file then takes, so a cluster
# any other entry reaches is left allocated, to the entries that remain.
sub claimed {
    my $skip = shift;                       # the entry being replaced
    my %own;
    my @todo = (0);
    my %seen;
    while (@todo) {
        my $dc = shift @todo;
        next if $seen{$dc}++;
        for my $o (dir_slots($dc)) {
            my $c0 = ord(substr($d, $o, 1));
            last if $c0 == 0;
            next if $c0 == 0xE5 || $o == $skip;
            my $a = ord(substr($d, $o + 11, 1));
            next if ($a & 0x3F) == 0x0F || ($a & 0x08);
            my $nm = substr($d, $o, 11);
            next if $nm eq '.          ' || $nm eq '..         ';
            my $c = unpack('v', substr($d, $o + 26, 2));
            if ($a & 0x10) { push @todo, $c if $c; }
            while ($c >= 2 && $c < 0xFFF8 && !$own{$c}) { $own{$c} = 1; $c = fat_get($c) }
        }
    }
    return \%own;
}

sub free_chain {
    my ($c, $own) = @_;
    while ($c >= 2 && $c < 0xFFF8) {
        my $n = fat_get($c);
        last if $own->{$c};                 # the rest belongs to someone else
        fat_set($c, 0);
        $c = $n;
    }
}

sub alloc_chain {
    my $need = shift;
    return 0 unless $need;
    my @got;
    for (my $c = 2; $c < $nclus && @got < $need; $c++) { push @got, $c if fat_get($c) == 0 }
    die "the volume is full\n" if @got < $need;
    for my $i (0 .. $#got) { fat_set($got[$i], $i == $#got ? 0xFFFF : $got[$i + 1]) }
    return @got;
}

sub dostime {
    my @t = localtime(shift);
    my $date = (($t[5] - 80) << 9) | (($t[4] + 1) << 5) | $t[3];
    my $time = ($t[2] << 11) | ($t[1] << 5) | int($t[0] / 2);
    return ($time, $date);
}

#--- the files ----------------------------------------------------------
my $dirclus = 0;
my $dirname = '\\';
while (@ARGV) {
    my $arg = shift @ARGV;
    if ($arg eq '-dir') {
        my $n83 = name83(shift @ARGV);
        my ($hit) = find_entry(0, $n83);
        die "no directory $n83 in the root\n" unless defined $hit;
        die "$n83 is not a directory\n" unless ord(substr($d, $hit + 11, 1)) & 0x10;
        $dirclus = unpack('v', substr($d, $hit + 26, 2));
        ($dirname = "\\$n83\\") =~ s/ +(?=\\)//; $dirname =~ s/ +/./;
        next;
    }
    open(my $in, '<', $arg) or die "$arg: $!";
    binmode $in;
    my $data = do { local $/; <$in> };
    close $in;
    my $n83 = name83($arg);
    my ($hit, $free) = find_entry($dirclus, $n83);
    my $o = defined($hit) ? $hit : $free;
    die "no room for $n83 in $dirname\n" unless defined $o;
    if (defined $hit) { free_chain(unpack('v', substr($d, $hit + 26, 2)), claimed($hit)) }
    my @ch = alloc_chain(int((length($data) + $csize - 1) / $csize));
    for my $i (0 .. $#ch) {
        my $piece = substr($data, $i * $csize, $csize);
        $piece .= "\0" x ($csize - length $piece);
        substr($d, clus_off($ch[$i]), $csize) = $piece;
    }
    my ($t, $dt) = dostime((stat $arg)[9]);
    my $attr = defined($hit) ? ord(substr($d, $hit + 11, 1)) : 0x20;   # a system
    # file stays one: MKDISK marks PMIO.SYS and PMDOS.SYS as it installs them
    my $e = $n83 . pack('C', $attr) . ("\0" x 10) . pack('vvvV', $t, $dt, $ch[0] // 0, length $data);
    substr($d, $o, 32) = $e;
    printf "  %-12s %8d bytes  %s  %s\n", $n83, length $data,
        defined($hit) ? 'replaced' : 'added   ', $dirname;
}

seek($fh, 0, 0);
print $fh $d;
close $fh;
