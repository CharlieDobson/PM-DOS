#!/usr/bin/perl
# mathref.pl - check LIBTEST2's "D" and "M" lines against references
# worked out here with 40 significant digits
#
#   perl mathref.pl W2.TXT [G2.TXT ...]
#
# "D input value": value must be what the host's strtod makes of input
# (Git for Windows' perl: Cygwin's, which rounds correctly).
# "M name args result": result is compared with the true value, and the
# error is given in units in the last place of a double.  A result more
# than its function's allowance out is a FAIL.
#
# Math::BigFloat's bdiv() divides to integers when it is called in list
# context - an argument list is one - so every division here goes
# through dv(), which calls it alone.
use strict;
use warnings;
use POSIX ();
use Math::BigFloat;

$| = 1;
my $DIG = 40;
my $P = $DIG + 15;                      # working precision
sub bf { return Math::BigFloat->new( $_[0] ) }
sub dv { my $r = $_[0]->copy; $r->bdiv( $_[1], $P ); return $r }
sub ml { my $r = $_[0]->copy; $r->bmul( $_[1] ); return $r }
sub ad { my $r = $_[0]->copy; $r->badd( $_[1] ); return $r }
sub sb { my $r = $_[0]->copy; $r->bsub( $_[1] ); return $r }
# e^x past 1000 either way is out of a double's range, and Math::BigFloat
# would take hours over e^100000
sub bexp {
    return bf( "1e500" ) if $_[0]->bcmp( 1000 ) > 0;
    return bf( 0 ) if $_[0]->bcmp( -1000 ) < 0;
    my $r = $_[0]->copy; $r->bexp( $P ); return $r;
}
sub blog { my $r = $_[0]->copy; $r->blog( undef, $P ); return $r }
sub bsqrt { my $r = $_[0]->copy; $r->bsqrt( $P ); return $r }
sub batan { my $r = $_[0]->copy; $r->batan( $P ); return $r }

my $PI = Math::BigFloat->bpi( $P + 30 );
my $LN2 = blog( bf( 2 ) );
my $ONE = bf( 1 );

# x mod 2 pi.  The largest double is 309 digits long, so pi and the
# quotient take 420 to leave x its fraction
my $RDIG = 420;
my $TWO_PI_R = Math::BigFloat->bpi( $RDIG )->bmul( 2 );
sub reduce {
    my ( $x ) = @_;
    my $n = $x->copy;
    $n->bdiv( $TWO_PI_R, $RDIG );
    $n->bfloor();
    my $r = $x->copy;
    $r->bsub( $n->bmul( $TWO_PI_R ) );
    return $r->bround( $P );
}
sub bsin { my $r = reduce( $_[0] ); $r->bsin( $P ); return $r }
sub bcos { my $r = reduce( $_[0] ); $r->bcos( $P ); return $r }

sub erf_series {
    my ( $x ) = @_;
    my $x2 = ml( $x, $x );
    my $term = bf( 1 );
    my $sum = bf( 1 );
    my $tiny = bf( "1e-60" );
    for my $n ( 0 .. 5000 ) {
        $term = dv( ml( $term, ml( $x2, 2 ) ), 2 * $n + 3 );
        $sum = ad( $sum, $term );
        last if dv( $term, $sum )->bcmp( $tiny ) < 0;
    }
    return ml( ml( dv( ml( $x, 2 ), bsqrt( $PI ) ), bexp( ml( $x2, -1 ) ) ), $sum );
}

sub erfc_fraction {
    my ( $x ) = @_;
    my $t = $x->copy;
    for ( my $k = 600; $k >= 1; $k-- ) {
        $t = ad( $x, dv( bf( $k / 2 ), $t ) );
    }
    return dv( bexp( ml( ml( $x, $x ), -1 ) ), ml( bsqrt( $PI ), $t ) );
}

# past 27 erfc is below the smallest double, and e^-x^2 would take
# Math::BigFloat hours
my $ERF_FAR = bf( 27 );

sub berf {
    my ( $x ) = @_;
    return bf( $x->is_neg ? -1 : 1 ) if $x->copy->babs->bcmp( $ERF_FAR ) > 0;
    return erf_series( $x ) if $x->copy->babs->bcmp( 4 ) < 0;
    return $x->is_neg ? sb( erfc_fraction( ml( $x, -1 ) ), 1 ) : sb( $ONE, erfc_fraction( $x ) );
}
sub berfc {
    my ( $x ) = @_;
    return bf( 0 ) if $x->bcmp( $ERF_FAR ) > 0;
    return bf( 2 ) if $x->copy->bneg->bcmp( $ERF_FAR ) > 0;
    return $x->bcmp( 4 ) < 0 ? sb( $ONE, erf_series( $x ) ) : erfc_fraction( $x );
}

# ln Gamma(x), x > 0: shift past 40, then Stirling with 12 terms
my @B = ( [ 1, 12 ], [ -1, 360 ], [ 1, 1260 ], [ -1, 1680 ], [ 1, 1188 ], [ -691, 360360 ],
          [ 1, 156 ], [ -3617, 122400 ], [ 43867, 244188 ], [ -174611, 125400 ],
          [ 77683, 5796 ], [ -236364091, 1506960 ] );
sub blgamma_pos {
    my ( $x ) = @_;
    my $prod = bf( 1 );
    my $z = $x->copy;
    while ( $z->bcmp( 40 ) < 0 ) {
        $prod = ml( $prod, $z );
        $z = ad( $z, 1 );
    }
    my $r = dv( $ONE, $z );
    my $r2 = ml( $r, $r );
    my $series = bf( 0 );
    my $pow = $r->copy;
    for my $c ( @B ) {
        $series = ad( $series, ml( dv( bf( $c->[0] ), $c->[1] ), $pow ) );
        $pow = ml( $pow, $r2 );
    }
    my $lsq = dv( blog( ml( $PI, 2 ) ), 2 );
    my $s = ad( ad( sb( ml( sb( $z, bf( "0.5" ) ), blog( $z ) ), $z ), $lsq ), $series );
    return sb( $s, blog( $prod ) );
}
sub btgamma {
    my ( $x ) = @_;
    return bexp( blgamma_pos( $x ) ) if !$x->is_neg;
    my $s = bsin( ml( $PI, $x ) );
    return dv( $PI, ml( $s, bexp( blgamma_pos( sb( $ONE, $x ) ) ) ) );
}
sub blgamma {
    my ( $x ) = @_;
    return blgamma_pos( $x ) if !$x->is_neg;
    my $s = bsin( ml( $PI, $x ) )->babs;
    return sb( blog( dv( $PI, $s ) ), blgamma_pos( sb( $ONE, $x ) ) );
}

sub basin { batan( dv( $_[0], bsqrt( sb( $ONE, ml( $_[0], $_[0] ) ) ) ) ) }

my %ref = (
    sin    => sub { bsin( $_[0] ) },
    cos    => sub { bcos( $_[0] ) },
    tan    => sub { dv( bsin( $_[0] ), bcos( $_[0] ) ) },
    atan   => sub { batan( $_[0] ) },
    asin   => sub { basin( $_[0] ) },
    acos   => sub { sb( dv( $PI, 2 ), basin( $_[0] ) ) },
    exp    => sub { bexp( $_[0] ) },
    expm1  => sub { sb( bexp( $_[0] ), 1 ) },
    log    => sub { blog( $_[0] ) },
    log2   => sub { dv( blog( $_[0] ), $LN2 ) },
    log10  => sub { dv( blog( $_[0] ), blog( bf( 10 ) ) ) },
    log1p  => sub { blog( ad( $_[0], 1 ) ) },
    sqrt   => sub { bsqrt( $_[0] ) },
    cbrt   => sub { my $a = $_[0]->copy->babs; $a->broot( 3, $P ); $_[0]->is_neg ? ml( $a, -1 ) : $a },
    sinh   => sub { dv( sb( bexp( $_[0] ), bexp( ml( $_[0], -1 ) ) ), 2 ) },
    cosh   => sub { dv( ad( bexp( $_[0] ), bexp( ml( $_[0], -1 ) ) ), 2 ) },
    tanh   => sub { my $e = bexp( ml( $_[0], 2 ) ); dv( sb( $e, 1 ), ad( $e, 1 ) ) },
    asinh  => sub { blog( ad( $_[0], bsqrt( ad( ml( $_[0], $_[0] ), 1 ) ) ) ) },
    acosh  => sub { blog( ad( $_[0], bsqrt( sb( ml( $_[0], $_[0] ), 1 ) ) ) ) },
    atanh  => sub { dv( blog( dv( ad( $ONE, $_[0] ), sb( $ONE, $_[0] ) ) ), 2 ) },
    erf    => sub { berf( $_[0] ) },
    erfc   => sub { berfc( $_[0] ) },
    tgamma => sub { btgamma( $_[0] ) },
    lgamma => sub { blgamma( $_[0] ) },
    pow    => sub { bexp( ml( $_[1], blog( $_[0] ) ) ) },
    atan2  => sub { my $r = $_[0]->copy; $r->batan2( $_[1], $P ); $r },
    hypot  => sub { bsqrt( ad( ml( $_[0], $_[0] ), ml( $_[1], $_[1] ) ) ) },
);
my %allow = ( pow => 2, sinh => 2, tgamma => 4, lgamma => 4, erf => 2, erfc => 2 );

# a double's last place, near the value v (a Perl number)
sub ulp {
    my ( $v ) = @_;
    return 2 ** -1074 if $v == 0;
    my ( undef, $e ) = POSIX::frexp( abs( $v ) );
    my $u = 2 ** ( $e - 53 );
    return $u < 2 ** -1074 ? 2 ** -1074 : $u;
}

my $fails = 0;
for my $file ( @ARGV ) {
    open my $fh, '<', $file or die "$file: $!";
    my %worst;
    my %where;
    while ( my $line = <$fh> ) {
        $line =~ s/\r?\n$//;
        if ( $line =~ /^D (\S+) (\S+)$/ ) {
            my ( $in, $got ) = ( $1, $2 );
            my $want = sprintf( "%.17g", scalar POSIX::strtod( $in ) );
            if ( $want ne $got ) {
                print "$file: FAIL strtod($in) = $got, should be $want\n";
                $fails++;
            }
            next;
        }
        next unless $line =~ /^M (\w+) (\S+) (\S+)$/;
        my ( $name, $args, $got ) = ( $1, $2, $3 );
        # a double past 2^53 is a whole number, and %.0f writes all of it
        my @a = map { bf( abs( $_ ) >= 2 ** 53 ? sprintf( "%.0f", $_ ) : sprintf( "%.40e", $_ ) ) }
                split /,/, $args;
        my $r = $ref{$name}->( @a );
        my $rs = $r->copy->bround( 25 )->bsstr();
        my $rd = 0 + sprintf( "%.20e", $rs );
        if ( $got =~ /inf|nan/i || abs( $rd ) > 1.7976931348623157e308 ) {
            if ( lc( $got ) ne lc( sprintf( "%.17g", $rd ) ) ) {
                print "$file: FAIL $name($args) = $got, should be $rd\n";
                $fails++;
            }
            next;
        }
        if ( abs( $rd ) < 1e-30 && abs( $got ) < 1e-30 ) {
            next;                       # a true zero (lgamma(1)): the series' noise
        }
        my $err = dv( sb( bf( $got ), $r )->babs, bf( sprintf( "%.30e", ulp( $rd ) ) ) );
        my $e = 0 + $err->bround( 10 )->bsstr();
        my $limit = $allow{$name} // 1;
        if ( $e > $limit ) {
            printf "%s: FAIL %s(%s) = %s, %.2f units out (true %s)\n", $file, $name, $args, $got, $e, $rs;
            $fails++;
        }
        if ( !defined $worst{$name} || $e > $worst{$name} ) {
            $worst{$name} = $e;
            $where{$name} = $args;
        }
    }
    close $fh;
    print "$file: worst ", join( ', ', map { sprintf "%s %.2f (%s)", $_, $worst{$_}, $where{$_} } sort keys %worst ), "\n";
}
print $fails ? "$fails FAILED\n" : "all within their allowances\n";
exit( $fails ? 1 : 0 );
