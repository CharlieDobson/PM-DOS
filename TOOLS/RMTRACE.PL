use strict; use warnings;
my $img = shift // 'C:/86Box/VMs/PM-DOS/MULTI.IMG';
open my $f,'<',$img or die "$img: $!"; binmode $f;
read $f, my $d, 72*512; close $f;
my $at = index($d,"RMDBG");
die "no trace on the image\n" if $at < 0;
my ($LEN,$HDR,$N2F,$STRN,$STRL,$ENT) = (32*512,224,32,8,128,32);
my ($XN,$XL) = (24,10);   # must match RDBG_XN/RDBG_XL in DOS/RMDBG.INC
my $SOFF = $HDR + $N2F*2;
my $XOFF = $SOFF + $STRN*$STRL;
my $LSTN = 24;   # must match RDBG_LSTN
my $LOFF = $XOFF + $XN*$XL;
my $EOFF = $LOFF + $LSTN*$STRL;
my $blk = substr($d,$at,$LEN);
my @mn = qw(entry vecs volinit sysinit sftinit xms RESUME run goback);
my ($cnt,$head,$m,$n2f) = unpack('vvvv', substr($blk,8,8));
printf "INT 21h served %d   INT 2Fh %d   marks: %s\n", $cnt,$n2f,
   join(",", map { $mn[$_] } grep { $m & (1<<$_) } 0..$#mn);
my ($pseg,$plow,$n11,$n15) = unpack("vvvv", substr($blk,0x70,8));
printf "RMDOS parked at %04X:0000 (linear %06X)   bounce seg %04X
", $pseg, $pseg*16, $plow;
my ($i11at,$cpsp,$ctop) = unpack("vvv", substr($blk,0x78,6));
printf "BIOS reflected down: INT 11h %d (first after INT 21h call %d)   INT 15h %d
", $n11, $i11at, $n15;
printf "last child PSP %04X, told its memory ends at %04X - %d paragraphs, %dK
",
   $cpsp, $ctop, $ctop-$cpsp, (($ctop-$cpsp)*16)/1024 if $cpsp;
my ($n67,$h67a,$h67b,$lpsp,$jcnt,$jseg,$joff,$jused) = unpack("v8", substr($blk,0x80,16));
printf "handles: PSP %04X has %d entries at %04X:%04X, %d of them in use%s\n",
   $lpsp, $jcnt, $jseg, $joff, $jused,
   ($jused >= $jcnt ? "   <- FULL" : "");
printf "         67h asked for a bigger table %d times%s\n", $n67,
   ($n67 ? sprintf(" (first %d, last %d)", $h67a, $h67b) : " - NOBODY ASKED");
my ($tpsp,$tpar,$tip,$tcs) = unpack("v4", substr($blk,0x98,8));
printf "first terminate: PSP %04X, parent %04X%s, terminate address %04X:%04X\n",
   $tpsp, $tpar, ($tpar == $tpsp ? " (ITSELF - DOS returns instead of freeing)" : ""),
   $tcs, $tip if $tpsp;
my ($tsp,$tss,$tcsp,$tcss,$tnclo,$tnfre,$tfseg,$tflen) = unpack("v8", substr($blk,0xA0,16));
if ($tpsp) {
  printf "         resumed on the PARENT's stack %04X:%04X; the 4Ch's own frame was %04X:%04X%s\n",
     $tss,$tsp,$tcss,$tcsp,
     (($tss==$tcss && $tsp==$tcsp) ? "   <- the SAME frame" : "   <- A DIFFERENT FRAME");
  my @f = unpack("v13", substr($blk,0xB0,26));
  printf "         frame there before patching: es=%04X ds=%04X di=%04X si=%04X bp=%04X sp=%04X bx=%04X dx=%04X cx=%04X ax=%04X  %04X:%04X fl=%04X\n",
     @f[0..9], $f[11], $f[10], $f[12];
  printf "         clean-up: %d handle%s closed, %d arena block%s freed%s\n",
     $tnclo, ($tnclo==1?"":"s"), $tnfre, ($tnfre==1?"":"s"),
     ($tfseg ? sprintf(" (first %04X, %d paragraphs)", $tfseg, $tflen) : "");
}

my ($nfail,$fpsp,$fcnt,$fused) = unpack("v4", substr($blk,0x90,8));
printf "         %d opens refused for want of a handle%s\n", $nfail,
   ($nfail ? sprintf("; the first was PSP %04X, %d of %d entries in use", $fpsp, $fused, $fcnt) : "");
if ($n2f) { my $k=$n2f<$N2F?$n2f:$N2F;
  printf "INT 2Fh AX: %s\n", join(" ", map {sprintf "%04X",$_} unpack("v$k",substr($blk,$HDR,$k*2))); }
my %fn=(0x09=>"print",0x3D=>"open",0x4B=>"exec");
print "\nstrings (ring, oldest first in slot order):\n";
for my $i (0..$STRN-1) {
  my $x = substr($blk,$SOFF+$i*$STRL,$STRL);
  my $ah = ord(substr($x,0,1)); my $s = substr($x,1);
  if ($ah == 0xFC) {
    # the environment tail: the interesting half is all after a NUL, so
    # the NULs are shown rather than used as a terminator.
    (my $e = $s) =~ s|[^ -~]|.|g;
    printf "  %-5s %s
", "env", $e;
    next;
  }
  $s =~ s/\x00.*//s; $s =~ s/\x24.*//s; $s =~ s/[^\x20-\x7e]/./g;
  next unless $s =~ /\S/;
  printf "  %-5s %s\n", ($fn{$ah}//sprintf("%02Xh",$ah)), $s;
}
print_xms();
print_last();
print_flushes();
my $max = int(($LEN-$EOFF)/$ENT);
my $n = $cnt < $max ? $cnt : $max;
print "\nINT 21h calls - what was asked, and what came back:\n";
for my $i (0..$n-1) {
  my $slot = ($head-$n+$i) % $max;
  my @w = unpack('v13', substr($blk,$EOFF+$slot*$ENT,26));
  printf "%5d  AX=%04X BX=%04X CX=%04X DX=%04X  %04X:%04X ds=%04X  ->  AX=%04X BX=%04X CX=%04X DX=%04X CF=%d\n",
     $w[7],$w[0],$w[1],$w[2],$w[3],$w[4],$w[5],$w[6],$w[8],$w[9],$w[10],$w[11],($w[12]&1);
}

#---------------------------------------------------------------------
# The XMS log.  Windows takes its extended memory through a far call
# into the driver, so none of it appears above and none of it ever has.
# Names come from the XMS specification; the ones that matter here are
# 08h/88h - how much is there - and 09h/89h - give me this much.
#---------------------------------------------------------------------
sub print_xms {
  my $nx = unpack("v", substr($blk,0x7E,2));
  return unless $nx;
  my %xf = (0x00=>'version',  0x01=>'req HMA',  0x02=>'rel HMA',
            0x03=>'gl A20',   0x04=>'gl -A20',  0x05=>'lo A20',
            0x06=>'lo -A20',  0x07=>'A20?',     0x08=>'free?',
            0x09=>'alloc',    0x0A=>'free',     0x0B=>'move',
            0x0C=>'lock',     0x0D=>'unlock',   0x0E=>'handle?',
            0x0F=>'realloc',  0x10=>'alloc UMB',0x11=>'free UMB',
            0x12=>'realloc UMB',
            0x88=>'free? 32', 0x89=>'alloc 32', 0x8E=>'handle? 32',
            0x8F=>'realloc 32');
  my $k = $nx < $XN ? $nx : $XN;
  printf "\nXMS calls (%d made, first %d kept):\n", $nx, $k;
  for my $i (0..$k-1) {
    my ($ah,$bl,$dxin,$ax,$dx,$bx) =
        unpack("CCvvvv", substr($blk,$XOFF+$i*$XL,$XL));
    if ($ah == 0xFF) {
      printf "       POOL base %04X%04X  %d KB
", $dxin, $ax, $dx + $bx*65536;
      next;
    }
    if ($ah == 0xFD) {
      # rx_pooltest: every page in the pool stamped with its own linear
      # address and read back.  A mismatch is the answer to the question
      # the standard-mode fault left behind - the difference between the
      # address asked and the address that answered names the address
      # line that is not getting through.
      my $bad = $dxin*65536 + $ax;
      my $got = $dx + $bx*65536;
      my $gate = ($bl & 1) ? "A20 open" : "A20 SHUT";
      if ($bl & 2) { print "       POOL SELF-TEST not run - protected mode was not ours to leave\n"; next; }
      if ($bad == 0xFFFFFFFF) {
        printf "       POOL SELF-TEST ok: %d dwords each read back their own address (%s)\n", $got, $gate;
      } else {
        printf "       POOL SELF-TEST FAILED at %08X: read %08X (off by %08X)  (%s)\n",
           $bad, $got, ($bad - $got) & 0xFFFFFFFF, $gate;
      }
      next;
    }
    if ($ah == 0xFE) {
      printf "       biggest arena block: owner %08X, %d KB   (park running as %04X)
",
         $dxin*65536 + $ax, $dx, $bx;
      next;
    }
    printf "%5d  AH=%02X %-11s DX=%04X  ->  AX=%04X BX=%04X DX=%04X%s\n",
       $i, $ah, ($xf{$ah}//'?'), $dxin, $ax, $bx, $dx,
       ($ax ? "" : sprintf("   FAILED BL=%02X", $bl));
  }
}

#---------------------------------------------------------------------
# The wrapping ring.  The ring of firsts says how a start-up got where
# it did; this one says what a program that will not stop is doing.
#---------------------------------------------------------------------
sub print_last {
  my @rows;
  for my $i (0..$LSTN-1) {
    my $x = substr($blk,$LOFF+$i*$STRL,$STRL);
    my $ah = ord(substr($x,0,1));
    my $at = unpack("v", substr($x,1,2));
    my $s = substr($x,3);
    $s =~ s/\x00.*//s; $s =~ s/\x24.*//s; $s =~ s/[^\x20-\x7e]/./g;
    next unless $s =~ /\S/;
    push @rows, [$at, sprintf("  %5d  %-5s %s", $at,
       ($ah==0x3D?"open":$ah==0x4B?"exec":$ah==0x09?"print":sprintf("%02Xh",$ah)), $s)];
  }
  return unless @rows;
  print "\nthe LAST names, oldest first (the number is the DOS call it happened on):\n";
  print "$_->[1]\n" for sort { $a->[0] <=> $b->[0] } @rows;
}

#---------------------------------------------------------------------
# The flush footprints, which are the only thing that says whether the
# CHANNEL was still alive when the machine stopped talking.  Sixteen
# sectors further up the gap, one per flush, wrapping - so these are
# the LAST sixteen, and the DOS-call count in each one against the
# count in the header says how far the run got past its last flush.
#---------------------------------------------------------------------
sub print_flushes {
  my @f;
  for my $i (0..15) {
    my $s = substr($d, (39+$i)*512, 16);
    next unless substr($s,0,6) eq "FLUSH ";
    my ($n,$ax,$fl,$marks,$cnt) = unpack("v5", substr($s,6,10));
    push @f, [$n,$ax,$fl,$marks,$cnt];
  }
  return unless @f;
  print "\nflushes (the last 16 to land, by number):\n";
  for my $r (sort { $a->[0] <=> $b->[0] } @f) {
    printf "  flush %5d  at DOS call %5d  marks %04X  previous INT 13h AX=%04X CF=%d%s\n",
       $r->[0], $r->[4], $r->[3], $r->[1], $r->[2] & 1,
       ($r->[2] & 1) ? "   <- THE WRITE BEFORE THIS ONE FAILED" : "";
  }
}
