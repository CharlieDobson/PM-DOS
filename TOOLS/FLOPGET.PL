#!/usr/bin/perl
# flopget.pl IMAGE [NAME...]  - read files out of the ROOT directory of
# an UNPARTITIONED FAT12 or FAT16 image: a boot floppy, or a volume
# image made to be MOUNTed.  With no names it lists the root.
#
# FATGET.PL does the same for the first partition of a hard disk image
# and cannot read a floppy - no partition table, and a FAT12 FAT - so a
# headless run that booted from A: and logged to A:\T.TXT had nothing
# on the host to read the log back with.  This is that.
#
# It honours the BPB's sector size, so it also reads the 1024-, 2048-
# and 4096-byte-sector fixtures MKLSEC.PL makes - which is the second
# thing it is for: checking what PM-DOS WROTE to one of those, with a
# reader that owes nothing to the kernel it is checking.
use strict; use warnings;

my $img = shift or die "usage: flopget.pl IMAGE [NAME...]\n";
open(my $F, '<', $img) or die "$img: $!";
binmode $F;
read($F, my $bs, 512) == 512 or die "$img: too short\n";

my ($bps,$spc,$rsvd,$nfat,$nroot,$tot16,$med,$spf,$spt,$heads,$hid,$tot32)
    = unpack('x11 v C v C v v C v v v V V', $bs);
die "$img: no BPB (bytes/sector $bps)\n" unless $bps >= 512 && !($bps & ($bps-1));
my $tot      = $tot16 || $tot32;
my $rootsec  = $rsvd + $nfat*$spf;
my $rootsecs = int(($nroot*32 + $bps-1)/$bps);
my $datasec  = $rootsec + $rootsecs;
my $nclus    = int(($tot - $datasec)/$spc);
my $fat12    = $nclus < 4085;
my $eoc      = $fat12 ? 0xFF8 : 0xFFF8;

sub sect { my ($n,$c) = @_; seek($F, $n*$bps, 0); read($F, my $d, $bps*($c||1)); return $d; }

my $fat = sect($rsvd, $spf);
sub nextc {
    my ($c) = @_;
    if ($fat12) {
        my $v = unpack('v', substr($fat, int($c*3/2), 2));
        return $c & 1 ? $v >> 4 : $v & 0xFFF;
    }
    return unpack('v', substr($fat, $c*2, 2));
}

my $root = sect($rootsec, $rootsecs);
my (%dir, @order);
for my $i (0 .. $nroot-1) {
    my $e = substr($root, $i*32, 32);
    my $f = ord(substr($e,0,1));
    last if $f == 0;
    next if $f == 0xE5;
    my $attr = ord(substr($e,11,1));
    next if ($attr & 0x0F) == 0x0F;             # a long-name slot
    next if $attr & 0x08;                       # the volume label
    my $n = substr($e,0,8); my $x = substr($e,8,3);
    $n =~ s/ +$//; $x =~ s/ +$//;
    my $name = $x eq '' ? $n : "$n.$x";
    my ($clus,$size) = unpack('x26 vV', $e);
    $dir{uc $name} = [$clus, $size, $attr];
    push @order, uc $name;
}

unless (@ARGV) {
    printf "%s: %d-byte sectors, FAT%d, %d clusters of %d sectors\n",
        $img, $bps, $fat12 ? 12 : 16, $nclus, $spc;
    for my $name (@order) {
        my ($clus,$size,$attr) = @{$dir{$name}};
        printf "  %-12s %s %9d  cluster %d\n", $name,
            ($attr & 0x10) ? '<DIR>' : '     ', $size, $clus;
    }
    exit 0;
}

binmode STDOUT;
for my $name (map { uc } @ARGV) {
    unless ($dir{$name}) { print "--- $name: not on the volume\n"; next; }
    my ($clus,$size) = @{$dir{$name}};
    my $data = '';
    my $guard = 0;
    while ($clus >= 2 && $clus < $eoc && length($data) < $size) {
        $data .= sect($datasec + ($clus-2)*$spc, $spc);
        $clus = nextc($clus);
        die "$name: chain loops\n" if ++$guard > $nclus;
    }
    print substr($data, 0, $size);
}
