#!/usr/bin/perl
# fltref.pl - what LIBTEST's "F" lines should say, from the host's printf
#
#   perl fltref.pl > FLT.REF
#   grep "^F " W.TXT | diff FLT.REF -
#
# The values and formats are LIBTEST.C's, in the same order; keep the
# two lists the same.  Perl hands the formats to the C library it was
# built with, which rounds exactly (Git for Windows' perl: Cygwin's).
use strict;
use warnings;

my @vals = ( 0.0, 0.1, 0.5, 1.5, 2.5, 3.5, 1.0 / 3.0, 2.0 / 3.0, 123456789.125, 1e22, 1e23,
    1e300, 1e-300, 4.9406564584124654e-324, 1.7976931348623157e308, 9.5, 99.95, 0.000123456,
    123456.0, 1e-5, 1e15, 1e16, 1e17, -2.75 );
my @fmts = ( "%f", "%.0f", "%.3f", "%.10f", "%e", "%.0e", "%.10e", "%g", "%.3g", "%.17g", "%#g",
    "%+010.2f", "%-12.3e|", "%G", "%E", "%.20f" );

for my $v ( 0 .. $#vals ) {
    for my $f ( @fmts ) {
        next if $vals[$v] > 1e100 && ( $f eq "%.10f" || $f eq "%.20f" );
        printf "F %d %s %s\n", $v, $f, sprintf( $f, $vals[$v] );
    }
}
printf "F big %s\n", sprintf( "%.0f", 1.7976931348623157e308 );
printf "F tiny %s\n", sprintf( "%.330f", 4.9406564584124654e-324 );
