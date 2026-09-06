use strict; use warnings;
#---------------------------------------------------------------------
# UNDIS.PL - disassemble a window of a raw 16-bit binary.
#
# Written to read DOSX.EXE, because "Standard Mode: Fault in MS-DOS
# Extender  EC=0000 CS=0053 IP=2B9A" names an instruction inside a
# binary nothing here could disassemble.  There is no ndisasm and no
# working python on this machine; there IS wdis, which reads object
# files only.  So the bytes are re-emitted as a wasm source, assembled,
# and handed to wdis.
#
#   perl UNDIS.PL <file> <start-hex> <len-hex> [outbase]
#   wasm -q <outbase>.asm && wdis <outbase>.obj
#
# TWO TRAPS, both of which cost a round of wrong answers:
#
#   - NO ORG.  The obvious way to make wdis print true addresses is to
#     ORG the segment at the window's start.  wasm pads an ORG with
#     zeros, wdis disassembles those two bytes at a time, and an ODD
#     origin like 2B87h therefore comes out one byte off all the way
#     down.  Every candidate base then scores identically and the test
#     looks broken rather than wrong.  Addresses here are window-
#     relative; the caller adds the window start.
#
#   - wdis DECIDES code-or-data for itself and hex-dumps what it takes
#     for data, which is most windows opened on a stripped binary.  A
#     NOP lead-in does not reliably talk it round either.  When it
#     dumps hex rather than disassembling, read the bytes - for finding
#     ONE instruction that is usually enough, and the byte dump is at
#     least honest about where it is.
#
# WHAT THIS FOUND, so it need not be found twice:
#
#   DOSX.EXE (Windows 3.1, 32682 bytes, MZ with ZERO relocations - it
#   relocates itself) keeps its INT 31h dispatch table at file 6542h as
#   [function][handler] pairs, 4 bytes each, functions 0000h upwards.
#   The handlers are segment offsets, and the PROTECTED-MODE CODE
#   SEGMENT BEGINS AT FILE OFFSET 3AC2h.  So
#
#       file offset = 3AC2h + code offset
#
#   That base was not guessed: every other candidate was eliminated by
#   requiring that all 26 handler entry points in the table be preceded
#   by a terminator (C3/CB/EB/E9/C2/CA).  3AC2h scores 25 of 26; the
#   next best scores 8.  It is confirmed independently by the handler
#   for DPMI 0604h - Get Page Size - at code 2B9D:
#
#       2B9D  C7 46 0C 00 00    mov word ptr [bp+0Ch],0
#       2BA2  C7 46 10 00 10    mov word ptr [bp+10h],1000h
#
#   which returns 4096 in CX:DX, exactly as the DPMI specification
#   requires.  Nothing but the right base decodes to that.
#
#   KRNL386.EXE is easier - it is an NE, so the segment table gives the
#   answer outright.  Segment 1 is CODE at file offset 1620h and 50545
#   bytes long, so
#
#       file offset = 1620h + segment-1 offset
#
#   and segment 1 holds 74 INT 31h sites, one of them the BEF9 that
#   makes the Set Segment Limit call standard mode dies on.
#---------------------------------------------------------------------
my ($file,$start,$len,$base) = @ARGV;
die "usage: UNDIS.PL file start-hex len-hex [outbase]\n" unless defined $len;
$start = hex($start); $len = hex($len);
$base ||= "undis";

open my $f,'<',$file or die "$file: $!"; binmode $f;
seek $f,$start,0; read($f,my $d,$len); close $f;
die "short read: wanted $len bytes at $start\n" if length($d) < $len;

open my $o,'>',"$base.asm" or die "$base.asm: $!";
print $o "        .386p\n";
print $o "_T SEGMENT BYTE PUBLIC USE16 'CODE'\n";
for (my $i=0; $i<length($d); $i+=16) {
  my @b = unpack("C*", substr($d,$i,16));
  print $o "        db ", join(",", map { sprintf("0%02Xh",$_) } @b), "\n";
}
print $o "_T ENDS\n        END\n";
close $o;
printf "wrote %s.asm  file %05X..%05X\n", $base, $start, $start+$len-1;
printf "  addresses wdis prints are window-relative: add %05X\n", $start;
printf "  DOSX.EXE code offset = file offset - 3AC2h\n" if $file =~ /DOSX/i;
