use strict; use warnings;
#---------------------------------------------------------------------
# dosxpoke.pl - patch DOSX.EXE *inside* HD16.IMG, never the master.
#
#   perl dosxpoke.pl <image> [code-offset-hex:hexbytes ...]
#
# With no patch arguments it only reports where DOSX sits and whether
# it is pristine.  DOSX MOVES inside the image every time stage.sh
# rebuilds it, so the file offset is re-derived every time from a
# 48-byte signature (the 0007 + 0008 DPMI handlers) rather than
# remembered - a stale hard-coded offset read garbage once already.
#
#   image offset = imgbase + 3AC2h + code offset
#---------------------------------------------------------------------
my $img = shift or die "usage: dosxpoke.pl <image> [codehex:bytes ...]\n";
my $MASTER = "C:/Users/Charlie/AppData/Local/Temp/claude/c--Users-Charlie-source-repos/7e86d6c9-5651-4540-9204-f4444d6aa1f4/scratchpad/DOSX.EXE.pristine";
my $CODE = 0x3AC2;             # DOSX protected-mode code segment base
# The signature must sit somewhere that is NEVER patched, or the tool
# stops being able to find the file it has just edited.  Anchoring it on
# the 0007/0008 handlers did exactly that.  The INT 31h DISPATCHER at
# 2B58 - the table walk and the shared error/success stubs after it - is
# the one part of DOSX every experiment here leaves alone.
my $SIGC = 0x2B58;
my $SIGN = 48;

open my $m,'<',$MASTER or die "$MASTER: $!"; binmode $m;
seek $m,$CODE+$SIGC,0; read($m,my $sig,$SIGN); close $m;

open my $h,'+<',$img or die "$img: $!"; binmode $h;
local $/; my $d = <$h>;

my @hits; my $p = 0;
while (($p = index($d,$sig,$p)) >= 0) { push @hits,$p; $p++ }
if (!@hits) {
  print "signature NOT found - image DOSX is already patched, or absent\n";
  exit 1;
}
printf "signature found %d time(s)\n", scalar @hits;
my $base = $hits[0] - ($CODE+$SIGC);
printf "DOSX at image offset %08X   (code offset = image - %08X)\n",
       $base, $base+$CODE;

# sanity: the dispatch table must still name the stock handlers
seek $h,$base+0x6542,0; read($h,my $tab,0x60);
my %t; for (my $i=0;$i<0x60;$i+=4) { my ($f,$x)=unpack("vv",substr($tab,$i,4)); $t{$f}=$x }
printf "  table[0007]=%04X (expect 2C31)  table[0008]=%04X (expect 2C3B)\n",
       $t{0x0007}//0, $t{0x0008}//0;

# and say plainly whether the handlers themselves still match the master
open my $m2,"<",$MASTER or die; binmode $m2;
seek $m2,$CODE+0x2C31,0; read($m2,my $hm,0x38); close $m2;
seek $h,$base+$CODE+0x2C31,0; read($h,my $hi,0x38);
printf "  0007+0008 handlers: %s\n",
   ($hm eq $hi) ? "PRISTINE (match master byte-for-byte)" : "PATCHED";

for my $a (@ARGV) {
  my ($co,$hex) = split /:/,$a,2;
  $co = hex($co); (my $hx = $hex) =~ s/\s+//g;
  my $bytes = pack("H*",$hx);
  my $off = $base + $CODE + $co;
  seek $h,$off,0; read($h,my $old,length $bytes);
  seek $h,$off,0; print $h $bytes;
  printf "  code %04X (image %08X) %d bytes\n", $co,$off,length $bytes;
  printf "      was %s\n", join(" ",map{sprintf "%02X",$_}unpack("C*",$old));
  printf "      now %s\n", join(" ",map{sprintf "%02X",$_}unpack("C*",$bytes));
}
close $h;
