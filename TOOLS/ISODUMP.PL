# isodump.pl - an independent reading of an ISO9660 disc, so that what
# the kernel prints can be checked against something that is not the
# kernel.  Lists the primary tree and the Joliet tree side by side.
use strict; use warnings;
my $f = shift or die "usage: isodump.pl <image> [dir]\n";
my $want = shift;
open my $fh, '<:raw', $f or die "$f: $!\n";
sub blk { my ($n)=@_; seek($fh, $n*2048, 0) or return undef; read($fh,my $b,2048)==2048 or return undef; return $b }
sub le32 { return unpack('V', substr($_[0], $_[1], 4)) }

my (%pri, %jol);
for my $n (16..40) {
  my $b = blk($n) or last;
  my $type = ord(substr($b,0,1));
  last if $type == 255;
  next unless substr($b,1,5) eq 'CD001';
  my $ext = le32($b, 156+2);
  my $len = le32($b, 156+10);
  if ($type == 1) { %pri = (ext=>$ext, len=>$len, name=>substr($b,40,32)) }
  elsif ($type == 2) {
    my $esc = substr($b,88,3);
    if ($esc eq '%/@' or $esc eq '%/C' or $esc eq '%/E') {
      %jol = (ext=>$ext, len=>$len, esc=>$esc);
    }
  }
}
sub walk {
  my ($ext,$len,$joliet) = @_;
  my @out; my $off = 0;
  while ($len > 0) {
    my $b = blk($ext + int($off/2048));
    my $p = $off % 2048;
    my $rl = ord(substr($b,$p,1));
    if ($rl == 0) { $off = (int($off/2048)+1)*2048; $len -= 2048 - $p; next }
    my $idlen = ord(substr($b,$p+32,1));
    my $id    = substr($b,$p+33,$idlen);
    my $flags = ord(substr($b,$p+25,1));
    my $size  = le32($b,$p+10);
    my $exta  = le32($b,$p+2);
    if ($idlen == 1 and ord($id) < 2) { $id = ord($id) ? '..' : '.' }
    elsif ($joliet) { $id = pack('C*', map { $_ > 255 ? 0x5f : $_ } unpack('n*', $id)) }
    push @out, sprintf("  %-40s %s ext=%-7d %d bytes", $id, ($flags & 2) ? '<DIR>' : '     ', $exta, $size);
    $off += $rl; $len -= $rl;
  }
  return @out;
}
print "PRIMARY  root ext=$pri{ext} len=$pri{len}\n";
print "$_\n" for walk($pri{ext}, $pri{len}, 0);
if (%jol) {
  print "\nJOLIET (escape $jol{esc})  root ext=$jol{ext} len=$jol{len}\n";
  print "$_\n" for walk($jol{ext}, $jol{len}, 1);
}
