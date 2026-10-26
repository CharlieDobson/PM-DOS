#!/usr/bin/perl
# ANSICMP.PL - compare two ANSITEST.COM result files record by record.
#
#   perl ANSICMP.PL answer.txt other.txt          what differs
#   perl ANSICMP.PL -show N file.txt              case N's screen, its
#                                                 rows that are not blank,
#                                                 and its attributes
#
# The first file is the answer (ANSITEST under MS-DOS with Microsoft's
# ANSI.SYS); every case of the second that differs from it is listed:
# the BIOS's mode, size and cursor, the input read back, and each screen
# cell that is not the same, as row,column char/attribute.  Exit code 0
# when nothing differs.
use strict;
use warnings;

my $REC = 2 + 5 + 1 + 64 + 4000;

sub slurp {
    my ( $f ) = @_;
    open my $h, '<:raw', $f or die "$f: $!\n";
    local $/;
    my $d = <$h>;
    close $h;
    return $d;
}

sub records {
    my ( $d ) = @_;
    my %r;
    $r{info} = substr( $d, 0, 23 );
    my $p = 23;
    while ( $p + $REC <= length $d ) {
        my $rec = substr( $d, $p, $REC );
        # past the count, the input area is whatever the last record
        # left in ANSITEST's buffer: not part of the answer
        my $nin = ord substr( $rec, 7, 1 );
        $nin = 64 if $nin > 64;
        substr( $rec, 8 + $nin, 64 - $nin ) = "\0" x ( 64 - $nin );
        $r{ ord substr( $rec, 1, 1 ) } = $rec;
        $p += $REC;
    }
    return \%r;
}

sub hdr {
    my ( $rec ) = @_;
    my ( $mode, $cols, $rows, $col, $row, $nin ) = unpack 'x2 C C C C C C', $rec;
    my $in = substr( $rec, 8, $nin > 64 ? 64 : $nin );
    return ( $mode, $cols, $rows, $col, $row, $in );
}

sub vis {
    my ( $s ) = @_;
    $s =~ s/([^\x21-\x7E])/sprintf '<%02X>', ord $1/ge;
    return $s;
}

if ( @ARGV == 3 && $ARGV[0] eq '-show' ) {
    my $r = records( slurp( $ARGV[2] ) );
    my $rec = $r->{ $ARGV[1] } or die "no case $ARGV[1]\n";
    my ( $mode, $cols, $rows, $col, $row, $in ) = hdr( $rec );
    print "case $ARGV[1]: mode $mode, $cols columns, rows-1 $rows, cursor $row,$col, input '", vis( $in ), "'\n";
    my $scr = substr( $rec, 72, 4000 );
    for my $y ( 0 .. 24 ) {
        my $line = '';
        my $at = '';
        for my $x ( 0 .. 79 ) {
            my ( $c, $a ) = unpack 'C C', substr( $scr, ( $y * 80 + $x ) * 2, 2 );
            $line .= ( $c >= 32 && $c < 127 ) ? chr $c : ( $c == 0 ? ' ' : '.' );
            $at .= sprintf '%02X', $a;
        }
        next if $line =~ /^ *$/ && $at =~ /^(07)*$/;
        printf "%2d |%s|\n", $y, $line;
        my $runs = '';
        my $last = '';
        for my $x ( 0 .. 79 ) {
            my $a = substr( $at, $x * 2, 2 );
            $runs .= " $x:$a" if $a ne $last;
            $last = $a;
        }
        print "    attr$runs\n";
    }
    exit 0;
}

die "perl ANSICMP.PL answer.txt other.txt | -show N file.txt\n" if @ARGV != 2;
my $A = records( slurp( $ARGV[0] ) );
my $B = records( slurp( $ARGV[1] ) );
my $bad = 0;

if ( $A->{info} ne $B->{info} ) {
    my @a = unpack 'a C C v C18', $A->{info};
    my @b = unpack 'a C C v C18', $B->{info};
    print "info: 1A00h AL $a[1] / $b[1], 440Ch CF $a[2] / $b[2], AX $a[3] / $b[3]\n";
    print "      block @a[4..21]\n      block @b[4..21]\n";
    $bad++;
}

for my $n ( sort { $a <=> $b } grep { /^\d+$/ } keys %$A ) {
    my $ra = $A->{$n};
    my $rb = $B->{$n};
    if ( !defined $rb ) { print "case $n: missing\n"; $bad++; next }
    next if $ra eq $rb;
    $bad++;
    my @ha = hdr( $ra );
    my @hb = hdr( $rb );
    my @say;
    push @say, "mode $ha[0]/$hb[0]" if $ha[0] != $hb[0];
    push @say, "cols $ha[1]/$hb[1]" if $ha[1] != $hb[1];
    push @say, "rows $ha[2]/$hb[2]" if $ha[2] != $hb[2];
    push @say, "cursor $ha[4],$ha[3]/$hb[4],$hb[3]" if $ha[3] != $hb[3] || $ha[4] != $hb[4];
    push @say, "input '" . vis( $ha[5] ) . "' / '" . vis( $hb[5] ) . "'" if $ha[5] ne $hb[5];
    my $sa = substr( $ra, 72, 4000 );
    my $sb = substr( $rb, 72, 4000 );
    my @cells;
    for my $i ( 0 .. 1999 ) {
        my $ca = substr( $sa, $i * 2, 2 );
        my $cb = substr( $sb, $i * 2, 2 );
        next if $ca eq $cb;
        my ( $c1, $a1 ) = unpack 'C C', $ca;
        my ( $c2, $a2 ) = unpack 'C C', $cb;
        push @cells, sprintf '%d,%d %02X/%02X %02X/%02X', int( $i / 80 ), $i % 80, $c1, $a1, $c2, $a2;
    }
    if ( @cells ) {
        my $more = @cells > 12 ? ' ... (' . scalar( @cells ) . ' cells)' : '';
        push @say, 'cells ' . join( '; ', @cells[ 0 .. ( @cells > 12 ? 11 : $#cells ) ] ) . $more;
    }
    print "case $n: ", join( ', ', @say ), "\n";
}
print $bad ? "$bad differ\n" : "all the same\n";
exit( $bad ? 1 : 0 );
