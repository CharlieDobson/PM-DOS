use strict; use warnings;
use Compress::Raw::Zlib;
# ZMBVSHOT.PL - screenshots out of a DOSBox(-X) ZMBV capture.
#
#     perl TOOLS\ZMBVSHOT.PL <capture.avi> <outprefix> [every-N-seconds]
#
# Writes <outprefix>_last.bmp (the final frame) and, with a third
# argument, <outprefix>_NNNN.bmp every N seconds of video as well.
#
# WHY IT EXISTS: headless DOSBox-X (the SDL dummy driver) has no window
# to take a screenshot of, and nothing a guest prints to a file shows
# a graphical screen - Windows 3.1 stopped on its splash screen, or in
# a dialog, looks exactly like Windows still loading.  DOSBox-X will
# record from the moment `config -avistart` runs in [autoexec], `boot`
# and all, so the screen is on the disk after the run; this reads it
# back.  No ffmpeg on this machine, so the codec is decoded here: ZMBV
# is zlib over either a whole frame (key) or per-block motion vectors
# plus an XOR (delta), and that is all of it.  DOSBox-X records 32 bits
# a pixel; 8, 15/16 and 24 are decoded as well.

my ($avi, $prefix, $every) = @ARGV;
die "usage: zmbvshot.pl <capture.avi> <outprefix> [every-N-seconds]\n" unless defined $prefix;
open(my $fh, '<', $avi) or die "$avi: $!";
binmode $fh;
my $d = do { local $/; <$fh> };
close $fh;

#--- the headers: frame size and rate ----------------------------------
my ($w, $h, $usec);
{
    my $i = index($d, 'avih');
    die "no avih\n" if $i < 0;
    $usec = unpack('V', substr($d, $i + 8, 4));
    my $s = index($d, 'strf');
    die "no strf\n" if $s < 0;
    ($w, $h) = unpack('VV', substr($d, $s + 12, 8));
}
my $fps = $usec ? 1e6 / $usec : 70;
printf "%s: %dx%d, %.2f frames a second\n", $avi, $w, $h, $fps;

#--- the frames, in file order ---------------------------------------
my @frames;
{
    my $m = index($d, 'movi');
    die "no movi\n" if $m < 0;
    my $p = $m + 4;
    while ($p + 8 <= length $d) {
        my ($id, $len) = unpack('A4V', substr($d, $p, 8));
        last if $id eq 'idx1';
        if ($id eq 'LIST') { $p += 12; next }
        push @frames, [$p + 8, $len] if $id =~ /^\d\ddc$/;
        $p += 8 + $len + ($len & 1);
    }
}
printf "%d video frames (%.1f s)\n", scalar @frames, @frames / $fps;

#--- the decoder ---------------------------------------------------------
my %BPP = (4 => 1, 5 => 2, 6 => 2, 7 => 3, 8 => 4);
my ($bw, $bh, $fmt, $bpp, $inf);
my @pal = (0) x 768;
my $cur = '';           # the current frame, $bpp bytes a pixel, top row first

sub inflate_all {
    my $in = shift;
    my $out = '';
    my $st = $inf->inflate($in, $out);
    die "zlib: $st\n" if $st != Z_OK && $st != Z_STREAM_END && $st != Z_BUF_ERROR;
    return $out;
}

sub decode {
    my ($off, $len) = @_;
    return unless $len;
    my $flags = ord(substr($d, $off, 1));
    my $pos = $off + 1;
    if ($flags & 1) {
        my ($hi, $lo, $comp, $f, $bwid, $bhei) = unpack('C6', substr($d, $pos, 6));
        $pos += 6;
        die "not zlib (compression $comp)\n" unless $comp == 1;
        die "format $f is not decoded here\n" unless $BPP{$f};
        ($fmt, $bw, $bh, $bpp) = ($f, $bwid, $bhei, $BPP{$f});
        ($inf) = Compress::Raw::Zlib::Inflate->new(-WindowBits => MAX_WBITS, -ConsumeInput => 1, -LimitOutput => 0);
        my $raw = inflate_all(substr($d, $pos, $len - 7));
        my $rp = 0;
        if ($bpp == 1) { @pal = unpack('C768', substr($raw, 0, 768)); $rp = 768 }
        $cur = substr($raw, $rp, $w * $h * $bpp);
        return;
    }
    return unless defined $inf;
    my $raw = inflate_all(substr($d, $pos, $len - 1));
    my $rp = 0;
    if ($bpp == 1 && ($flags & 2)) {
        my @x = unpack('C768', substr($raw, 0, 768));
        $pal[$_] ^= $x[$_] for 0 .. 767;
        $rp = 768;
    }
    my $bx = int(($w + $bw - 1) / $bw);
    my $by = int(($h + $bh - 1) / $bh);
    my $nb = $bx * $by;
    my @vec = unpack('c' . ($nb * 2), substr($raw, $rp, $nb * 2));
    $rp += ($nb * 2 + 3) & ~3;
    my $old = $cur;
    my $rowb = $w * $bpp;
    for my $b (0 .. $nb - 1) {
        my $xorf = $vec[$b * 2] & 1;
        my $dx = $vec[$b * 2] >> 1;
        my $dy = $vec[$b * 2 + 1] >> 1;
        next unless $xorf || $dx || $dy;        # the block did not change
        my $x0 = ($b % $bx) * $bw;
        my $y0 = int($b / $bx) * $bh;
        my $cw = $x0 + $bw > $w ? $w - $x0 : $bw;
        my $ch = $y0 + $bh > $h ? $h - $y0 : $bh;
        my $lb = $cw * $bpp;
        for my $row (0 .. $ch - 1) {
            my $sy = $y0 + $row + $dy;
            my $sx = $x0 + $dx;
            my $line;
            if ($sy < 0 || $sy >= $h) {
                $line = "\0" x $lb;
            } elsif ($sx >= 0 && $sx + $cw <= $w) {
                $line = substr($old, $sy * $rowb + $sx * $bpp, $lb);
            } else {
                $line = '';
                for my $col (0 .. $cw - 1) {
                    my $px = $sx + $col;
                    $line .= ($px >= 0 && $px < $w) ? substr($old, $sy * $rowb + $px * $bpp, $bpp) : "\0" x $bpp;
                }
            }
            if ($xorf) {
                $line ^= substr($raw, $rp, $lb);
                $rp += $lb;
            }
            substr($cur, ($y0 + $row) * $rowb + $x0 * $bpp, $lb) = $line;
        }
    }
}

sub rgb_of {                # one pixel -> (r, g, b)
    my $px = shift;
    if ($bpp == 4) { my ($b, $g, $r) = unpack('C3', $px); return ($r, $g, $b) }
    if ($bpp == 3) { my ($b, $g, $r) = unpack('C3', $px); return ($r, $g, $b) }
    if ($bpp == 1) { my $i = ord($px); return @pal[$i * 3 .. $i * 3 + 2] }
    my $v = unpack('v', $px);
    if ($fmt == 5) { return ((($v >> 10) & 31) << 3, (($v >> 5) & 31) << 3, ($v & 31) << 3) }
    return ((($v >> 11) & 31) << 3, (($v >> 5) & 63) << 2, ($v & 31) << 3);
}

sub save_bmp {                  # always 24-bit, which everything reads
    my $name = shift;
    my $stride = ($w * 3 + 3) & ~3;
    my $pix = '';
    for (my $y = $h - 1; $y >= 0; $y--) {
        my $line = '';
        if ($bpp == 4) {
            my $src = substr($cur, $y * $w * 4, $w * 4);
            $line = join('', map { substr($src, $_ * 4, 3) } 0 .. $w - 1);
        } else {
            for my $x (0 .. $w - 1) {
                my ($r, $g, $b) = rgb_of(substr($cur, ($y * $w + $x) * $bpp, $bpp));
                $line .= pack('CCC', $b, $g, $r);
            }
        }
        $pix .= $line . ("\0" x ($stride - $w * 3));
    }
    my $hdr = pack('A2VvvV', 'BM', 54 + length $pix, 0, 0, 54)
            . pack('VVVvvVVVVVV', 40, $w, $h, 1, 24, 0, length $pix, 2835, 2835, 0, 0);
    open(my $o, '>', $name) or die "$name: $!";
    binmode $o;
    print $o $hdr, $pix;
    close $o;
    print "  $name\n";
}

my $step = defined($every) ? int($every * $fps + 0.5) : 0;
for my $i (0 .. $#frames) {
    decode(@{$frames[$i]});
    if ($step && $i && $i % $step == 0 && length $cur) {
        save_bmp(sprintf('%s_%04d.bmp', $prefix, int($i / $fps + 0.5)));
    }
}
save_bmp("${prefix}_last.bmp") if length $cur;
