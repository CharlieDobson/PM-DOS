#!/usr/bin/perl
# fatget.pl IMAGE [NAME...]   - read files out of the FAT16 volume that
# starts at the first partition of a PM-DOS hard disk image.
#
# WHY THIS EXISTS.  Every test on the far side of the handover has been
# read off a SCREENSHOT, one boot at a time, and a screen holds 25 lines
# of a run that prints eighty.  A test that redirects its output to a
# file can be read here, in full, without a camera - and four tests can
# share one boot.
use strict; use warnings;

my $img = shift or die "usage: fatget.pl IMAGE [NAME...]\n";
open(my $F, '<', $img) or die "$img: $!";
binmode $F;

# The partition table, for where the volume starts.
seek($F, 0x1BE, 0); read($F, my $pt, 64);
my $start = 63;
for my $i (0..3) {
    my ($type, $lba) = unpack('x4 C x3 V', substr($pt, $i*16, 16));
    next unless $type;
    $start = $lba, last if $type == 0x04 || $type == 0x06 || $type == 0x0E;
}

seek($F, $start*512, 0); read($F, my $bs, 512);
my ($bps,$spc,$rsvd,$nfat,$nroot,$tot16,$med,$spf) = unpack('x11 vCvCvvCv', $bs);
my $fatsec  = $start + $rsvd;
my $rootsec = $fatsec + $nfat*$spf;
my $datasec = $rootsec + int(($nroot*32 + $bps-1)/$bps);

sub sect { my ($n,$c) = @_; seek($F, $n*512, 0); read($F, my $d, 512*($c||1)); return $d; }

my $fat = sect($fatsec, $spf);

sub chain {
    my ($c) = @_; my @l;
    while ($c >= 2 && $c < 0xFFF8) { push @l, $c; $c = unpack('v', substr($fat, $c*2, 2)); }
    return @l;
}

my $root = sect($rootsec, int(($nroot*32+511)/512));
my %dir;
for my $i (0 .. $nroot-1) {
    my $e = substr($root, $i*32, 32);
    my $f = ord(substr($e,0,1));
    last if $f == 0;
    next if $f == 0xE5;
    next if (ord(substr($e,11,1)) & 0x0F) == 0x0F;   # a long-name slot
    my $n = substr($e,0,8); my $x = substr($e,8,3);
    $n =~ s/ +$//; $x =~ s/ +$//;
    my $name = $x eq '' ? $n : "$n.$x";
    my ($clus,$size) = unpack('x26 vV', $e);
    $dir{uc $name} = [$clus, $size];
}

my @want = @ARGV ? (map { uc } @ARGV) : (sort keys %dir);
for my $name (@want) {
    unless ($dir{$name}) { print "--- $name: not on the volume\n"; next; }
    my ($clus,$size) = @{$dir{$name}};
    my $data = '';
    $data .= sect($datasec + ($_-2)*$spc, $spc) for chain($clus);
    $data = substr($data, 0, $size);
    if (@ARGV) { $data =~ s/\r\n/\n/g; print "--- $name ($size bytes)\n$data\n"; }
    else       { printf("%-14s %7d bytes  clus %d\n", $name, $size, $clus); }
}
