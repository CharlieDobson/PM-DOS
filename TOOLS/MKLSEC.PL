#!/usr/bin/perl
# mklsec.pl OUT.IMG BPS SECTORS [FILE...]  - a FAT12 volume whose BPB
# says its sectors are BPS bytes (512, 1024, 2048 or 4096), SECTORS of
# them, with each FILE from the host copied into the root, contiguous.
#
# THE FIXTURE FOR LOGICAL SECTOR SIZES.  Until v0.60 PM-DOS refused any
# BPB whose sector was not 512 bytes, and nothing on the host would
# make one it could be tested against: MKDISK.EXE writes 512-byte
# floppies, Windows' FORMAT will not put a 2048-byte sector on a
# 512-byte medium, and DOSBox-X's IMGMAKE writes 512 too.  So this
# writes the whole volume itself - boot record, both FATs, root, data -
# and FLOPGET.PL reads it back.  The image is then carried onto a
# PM-DOS disk as an ordinary file and MOUNTed: see LSECTEST.BAT.
#
# One sector per cluster, one sector of root directory (BPS/32 entries),
# two FATs.  Small on purpose - it has to fit on a floppy beside PM-DOS.
use strict; use warnings;

my ($out, $bps, $nsec, @files) = @ARGV;
die "usage: mklsec.pl OUT.IMG BPS SECTORS [FILE...]\n" unless $out && $bps && $nsec;
die "BPS must be 512, 1024, 2048 or 4096\n" unless $bps =~ /^(512|1024|2048|4096)$/;

my $rsvd   = 1;
my $nfat   = 2;
my $nroot  = $bps / 32;                          # one sector of root
my $rootsecs = 1;
# FAT12 sectors per FAT: enough for every cluster the rest can hold
my $spf = 1;
while (1) {
    my $clus = $nsec - $rsvd - $nfat*$spf - $rootsecs;
    last if int((($clus + 2) * 3 + 1) / 2) <= $spf * $bps;
    $spf++;
}
my $datasec = $rsvd + $nfat*$spf + $rootsecs;
my $nclus   = $nsec - $datasec;
die "too many clusters for FAT12\n" if $nclus >= 4085;

my $img = "\0" x ($nsec * $bps);

# --- the boot record: a jump, and a DOS 4 BPB ---------------------------
my $label = sprintf('%-11.11s', "LSEC$bps");
my $boot = pack('C3 A8 v C v C v v C v v v V V C C C V A11 A8',
    0xEB, 0x3C, 0x90, 'PMDOSTST',
    $bps, 1, $rsvd, $nfat, $nroot, ($nsec < 65536 ? $nsec : 0), 0xF0, $spf,
    18, 2, 0, ($nsec < 65536 ? 0 : $nsec),
    0x00, 0x00, 0x29, 0x1D2E3F40, $label, 'FAT12   ');
substr($img, 0, length $boot) = $boot;
substr($img, 510, 2) = "\x55\xAA";               # where DOS looks
substr($img, $bps-2, 2) = "\x55\xAA" if $bps > 512;   # and the sector's end

# --- the FAT: media byte, end-of-chain, then each file's run ------------
my @fat = (0xFF0, 0xFFF);
my $root = '';
my $next = 2;
my $vol = pack('A11 C x10 v v v V', $label, 0x08, 0, 0x5000, 0, 0);
$root .= $vol;
for my $path (@files) {
    open(my $H, '<', $path) or die "$path: $!";
    binmode $H;
    local $/; my $data = <$H>; close $H;
    $data = '' unless defined $data;
    (my $base = $path) =~ s{.*[/\\]}{};
    my ($n, $x) = split /\./, uc $base, 2;
    $x = '' unless defined $x;
    my $need = int((length($data) + $bps - 1) / $bps);
    my $first = $need ? $next : 0;
    for my $i (0 .. $need-1) {
        die "volume full at $path\n" if $next - 2 >= $nclus;
        $fat[$next] = ($i == $need-1) ? 0xFFF : $next + 1;
        substr($img, ($datasec + $next - 2) * $bps, $bps) =
            substr($data . ("\0" x $bps), $i * $bps, $bps);
        $next++;
    }
    # 12:00:00 on 25 September 2026, stamped the way DOS stamps them
    my $date = ((2026-1980) << 9) | (9 << 5) | 25;
    my $time = 12 << 11;
    $root .= pack('A8 A3 C x10 v v v V', $n, $x, 0x20, $time, $date, $first, length $data);
}
die "root directory full\n" if length($root) > $bps;
substr($img, ($rsvd + $nfat*$spf) * $bps, length $root) = $root;

my $fatbytes = "\0" x ($spf * $bps);
for my $c (0 .. $#fat) {
    my $v = $fat[$c] // 0;
    my $o = int($c * 3 / 2);
    my $w = unpack('v', substr($fatbytes, $o, 2));
    if ($c & 1) { $w = ($w & 0x000F) | ($v << 4); }
    else        { $w = ($w & 0xF000) | $v; }
    substr($fatbytes, $o, 2) = pack('v', $w & 0xFFFF);
}
for my $k (0 .. $nfat-1) {
    substr($img, ($rsvd + $k*$spf) * $bps, length $fatbytes) = $fatbytes;
}

open(my $O, '>', $out) or die "$out: $!";
binmode $O;
print $O $img;
close $O;
printf "%s: %d sectors of %d bytes, %d clusters, %d file(s)\n",
    $out, $nsec, $bps, $nclus, scalar @files;
