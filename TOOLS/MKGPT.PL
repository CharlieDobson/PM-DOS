use strict; use warnings;
# MKGPT.PL - build a GPT disk image, for testing what reads one.
#
#   perl TOOLS\MKGPT.PL out.img -mb <size> [options] -part <spec> ...
#
#   -mb N          the disk is N megabytes (a whole number of 16x63
#                  cylinders is added on top if that is not one already,
#                  so DBXRUN's -size line comes out even)
#   -mbr FILE      boot code for the protective MBR: the first 440 bytes
#                  of FILE.  Without it the code area is zeros
#   -entries N     slots in the entry array (128, the usual)
#   -part SPEC     one partition, in table order.  SPEC is a comma list:
#                    type=bdp|efi|msr|linux|bios|<GUID>   (bdp)
#                    start=<sector>  (the next 1MB line after the last)
#                    size=<n>[K|M]   (sectors; 0 or absent = the rest)
#                    name=<text>     (the partition's own name, ASCII)
#                    attr=<bits>     (decimal bit numbers joined by +,
#                                     e.g. 2 for Legacy BIOS bootable,
#                                     62 for hidden, 63 for no letter)
#                    file=<image>    (its first sectors come from this
#                                     file: a raw volume)
#                    from=<image>:<n> (...or from partition n, 1-4, of
#                                     an MBR disk image such as MKDISK's)
#                    slot=<n>        (which array slot, 0 up; the next
#                                     free one by default)
#   -break WHAT    damage the result on purpose: "primary" (the primary
#                  header's CRC), "array" (one byte of the primary array),
#                  "backup" (the backup header's CRC), "both" (both
#                  headers' CRCs), "nomark" (no 0EEh entry in the MBR)
#   -hybrid T      an MBR entry of type T (hex) for the first partition
#                  as well as the 0EEh one, as some tools write
#   -msdos         no GPT at all: an ordinary MBR partition table, four
#                  entries at most, each -part's type= its type in hex
#                  (07 for exFAT, 0C for FAT32...), placed as above
#
# A volume put in from file= or from= has its own idea of where it
# starts patched to the new place: a FAT boot sector's hidden-sector
# count at 1Ch, and an exFAT boot region's PartitionOffset at 64 with
# both boot checksums made again.  Nothing else in it is touched.
#
# The layout is the one every partitioning tool writes: header at 1,
# the array at 2, the first usable sector after the array; the backup
# array and header at the end, the header in the last sector.  CRC-32
# is the IEEE one GPT specifies.

my $out = shift or die "usage: mkgpt.pl out.img -mb N [options] -part SPEC ...\n";
my ($mb, $mbrfile, $nent, $break, $hybrid, $msdos) = (0, undef, 128, '', undef, 0);
my @parts;
while (@ARGV) {
    my $a = shift;
    if    ($a eq '-mb')      { $mb = shift }
    elsif ($a eq '-msdos')   { $msdos = 1 }
    elsif ($a eq '-mbr')     { $mbrfile = shift }
    elsif ($a eq '-entries') { $nent = shift }
    elsif ($a eq '-break')   { $break = shift }
    elsif ($a eq '-hybrid')  { $hybrid = hex(shift) }
    elsif ($a eq '-part')    { push @parts, parse_part(shift) }
    else { die "mkgpt.pl: what is $a?\n" }
}
die "mkgpt.pl: -mb is wanted\n" unless $mb > 0;
die "mkgpt.pl: -entries must be a multiple of 4\n" if $nent % 4;

my $cyl = 16 * 63;
my $total = $mb * 2048;
$total += $cyl - ($total % $cyl) if $total % $cyl;
my $asecs = $nent * 128 / 512;              # sectors in the array
my $first = 2 + $asecs;
my $last  = $total - 1 - $asecs - 1;        # the last usable sector
if ($msdos) {                               # an MBR disk: up to four
    die "mkgpt.pl: -msdos takes four partitions at most\n" if @parts > 4;
    ($first, $last) = (1, $total - 1);
}

my %types = (
    bdp   => 'EBD0A0A2-B9E5-4433-87C0-68B6B72699C7',
    efi   => 'C12A7328-F81F-11D2-BA4B-00A0C93EC93B',
    msr   => 'E3C9E316-0B5C-4DB8-817D-F92DF00215AE',
    linux => '0FC63DAF-8483-4772-8E79-3D69D8477DE4',
    bios  => '21686148-6449-6E6F-744E-656564454649',
);

#--- place the partitions ---------------------------------------------
# a volume put in from a file is read first: with no size= its partition
# is exactly as long as it is
for my $p (@parts) {
    $p->{vol} = loadvol($p);
    $p->{size} = length($p->{vol}) / 512 if defined $p->{vol} && !$p->{size};
}
my $next = 2048;
my $slot = 0;
my %used;
for my $p (@parts) {
    $p->{start} = align($next) unless defined $p->{start};
    die "mkgpt.pl: a partition starts at $p->{start}, before $first\n" if $p->{start} < $first;
    $p->{size} = $last + 1 - $p->{start} unless $p->{size};
    $p->{end} = $p->{start} + $p->{size} - 1;
    die "mkgpt.pl: a partition runs past the last usable sector $last\n" if $p->{end} > $last;
    $next = $p->{end} + 1;
    $p->{slot} = $slot unless defined $p->{slot};
    die "mkgpt.pl: slot $p->{slot} twice\n" if $used{$p->{slot}}++;
    die "mkgpt.pl: slot $p->{slot} is past the array\n" if $p->{slot} >= $nent;
    $slot = $p->{slot} + 1;
}

#--- the entry array ----------------------------------------------------
my $array = "\0" x ($nent * 128);
my $n = 0;
for my $p ($msdos ? () : @parts) {
    my $t = $types{$p->{type}} // $p->{type};
    my $e = guid($t) . guid(sprintf('%08X-0000-4000-8000-%012X', 0x504D4400 + $n, 0x1D150000 + $n))
          . q64($p->{start}) . q64($p->{end}) . q64($p->{attr})
          . pack('v36', map { ord } split //, substr($p->{name}, 0, 36));
    substr($array, $p->{slot} * 128, 128) = $e;
    $n++;
}

#--- the disk -----------------------------------------------------------
open(my $fh, '+>', $out) or die "$out: $!";
binmode $fh;
truncate($fh, $total * 512) or die "$out: $!";

# the protective MBR: one entry, type EEh, from sector 1 to the end or
# to the 32-bit limit, and the boot code in front if there is some
my $mbr = "\0" x 512;
if ($mbrfile) {
    open(my $m, '<', $mbrfile) or die "$mbrfile: $!";
    binmode $m;
    read($m, my $code, 440);
    close $m;
    substr($mbr, 0, length $code) = $code;
}
my $pe = 0x1BE;
if ($msdos) {
    # an ordinary partition table: type= is the MBR type in hex
    for my $p (@parts) {
        substr($mbr, $pe, 16) = pack('C4 C4 V2', 0, 0xFE, 0xFF, 0xFF, hex($p->{type}), 0xFE, 0xFF, 0xFF,
                                     $p->{start}, $p->{size});
        $pe += 16;
    }
    $break = 'nomark';
} elsif (defined $hybrid && @parts) {
    my $p = $parts[0];
    substr($mbr, $pe, 16) = pack('C4 C4 V2', 0, 0xFE, 0xFF, 0xFF, $hybrid, 0xFE, 0xFF, 0xFF,
                                 $p->{start}, $p->{size});
    $pe += 16;
}
unless ($break eq 'nomark') {
    my $len = $total - 1;
    $len = 0xFFFFFFFF if $len > 0xFFFFFFFF;
    substr($mbr, $pe, 16) = pack('C4 C4 V2', 0, 0, 2, 0, 0xEE, 0xFE, 0xFF, 0xFF, 1, $len);
}
substr($mbr, 510, 2) = "\x55\xAA";
put(0, $mbr);

unless ($msdos) {
    my $acrc = crc32($array);
    put(2, $array);
    put($total - 1 - $asecs, $array);
    put(1, header(1, $total - 1, 2, $acrc));
    put($total - 1, header($total - 1, 1, $total - 1 - $asecs, $acrc));
}

#--- the volumes ----------------------------------------------------------
for my $p (@parts) {
    my $vol = $p->{vol};
    next unless defined $vol;
    die "mkgpt.pl: the volume is bigger than its partition\n" if length($vol) > $p->{size} * 512;
    fixvol(\$vol, $p->{start});
    seek($fh, $p->{start} * 512, 0);
    print $fh $vol;
}

#--- and the damage, if asked for --------------------------------------
if ($break eq 'primary' || $break eq 'both') { poke(1 * 512 + 16, 0x5A) }
if ($break eq 'backup'  || $break eq 'both') { poke(($total - 1) * 512 + 16, 0x5A) }
if ($break eq 'array')                       { poke(2 * 512 + 40, 0x01) }
close $fh;

printf "%s: %d sectors, %d partitions, usable %d-%d\n", $out, $total, scalar @parts, $first, $last;
printf "  %d: %-6s %10d +%-10d %s\n", $_->{slot} + 1, $_->{type}, $_->{start}, $_->{size}, $_->{name} for @parts;
printf "  imgmount 2 %s -size 512,63,16,%d -fs none\n", $out, $total / $cyl;
exit 0;

#=========================================================================
sub parse_part {
    my %p = (type => 'bdp', attr => 0, name => '');
    for my $kv (split /,/, shift) {
        my ($k, $v) = split /=/, $kv, 2;
        die "mkgpt.pl: '$kv' in a -part is not key=value\n" unless defined $v;
        if ($k eq 'size' || $k eq 'start') {
            $v = $1 * 2048 if $v =~ /^(\d+)M$/i;
            $v = $1 * 2 if $v =~ /^(\d+)K$/i;
        }
        if ($k eq 'attr') {
            my $bits = 0;
            $bits += 2 ** $_ for split /\+/, $v;
            $v = $bits;
        }
        $p{$k} = $v;
    }
    return \%p;
}

sub align { my $s = shift; return int(($s + 2047) / 2048) * 2048 }

# the sectors a -part puts in its partition, or undef for none
sub loadvol {
    my $p = shift;
    if ($p->{file}) {
        open(my $v, '<', $p->{file}) or die "$p->{file}: $!";
        binmode $v;
        local $/; my $vol = <$v>;
        close $v;
        $vol .= "\0" x (512 - length($vol) % 512) if length($vol) % 512;
        return $vol;
    }
    if ($p->{from}) {
        my ($img, $which) = $p->{from} =~ /^(.*):(\d)$/ or die "mkgpt.pl: from=image:n\n";
        open(my $v, '<', $img) or die "$img: $!";
        binmode $v;
        local $/; my $d = <$v>;
        close $v;
        my ($st, $len) = unpack('V2', substr($d, 0x1BE + ($which - 1) * 16 + 8, 8));
        die "mkgpt.pl: $img has no partition $which\n" unless $len;
        return substr($d, $st * 512, $len * 512);
    }
    return undef;
}

sub header {
    my ($my, $alt, $alba, $acrc) = @_;
    my $h = 'EFI PART' . pack('V V V V', 0x00010000, 92, 0, 0)
          . q64($my) . q64($alt) . q64($first) . q64($last)
          . guid('504D2D44-4F53-4750-5400-' . sprintf('%012X', $total))
          . q64($alba) . pack('V V V', $nent, 128, $acrc);
    substr($h, 16, 4) = pack('V', crc32($h));
    return $h . ("\0" x (512 - length $h));
}

# GUID text -> its 16 bytes: the first three fields are little-endian
sub guid {
    my @f = split /-/, shift;
    return pack('V v v', hex $f[0], hex $f[1], hex $f[2]) . pack('H4 H12', $f[3], $f[4]);
}

sub q64 { my $v = shift; return pack('V V', $v % 4294967296, int($v / 4294967296)) }

sub put {
    my ($lba, $data) = @_;
    seek($fh, $lba * 512, 0);
    print $fh $data;
}

sub poke {
    my ($off, $x) = @_;
    seek($fh, $off, 0);
    read($fh, my $b, 1);
    seek($fh, $off, 0);
    print $fh chr(ord($b) ^ $x);
}

my @crctab;
sub crc32 {
    unless (@crctab) {
        for my $i (0 .. 255) {
            my $c = $i;
            $c = ($c & 1) ? (0xEDB88320 ^ ($c >> 1)) : ($c >> 1) for 1 .. 8;
            push @crctab, $c;
        }
    }
    my $c = 0xFFFFFFFF;
    $c = $crctab[($c ^ $_) & 0xFF] ^ ($c >> 8) for unpack('C*', shift);
    return $c ^ 0xFFFFFFFF;
}

# where the volume thinks it is
sub fixvol {
    my ($v, $start) = @_;
    if (substr($$v, 3, 8) eq 'EXFAT   ') {
        my $bps = 1 << ord(substr($$v, 108, 1));
        for my $base (0, 12 * $bps) {
            substr($$v, $base + 64, 8) = q64($start);
            my $sum = 0;
            my $region = substr($$v, $base, 11 * $bps);
            my $i = 0;
            for my $b (unpack('C*', $region)) {
                $sum = ((($sum & 1) ? 0x80000000 : 0) + ($sum >> 1) + $b) & 0xFFFFFFFF
                    unless $i == 106 || $i == 107 || $i == 112;
                $i++;
            }
            substr($$v, $base + 11 * $bps, $bps) = pack('V', $sum) x ($bps / 4);
        }
    } elsif (unpack('v', substr($$v, 11, 2)) == 512 && ord(substr($$v, 13, 1))) {
        substr($$v, 0x1C, 4) = pack('V', $start);
    }
}
