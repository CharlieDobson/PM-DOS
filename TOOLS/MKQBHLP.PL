#!/usr/bin/perl
# mkqbhlp.pl [-nolinks] SRC... OUT - QBASIC.HLP from the parts in
# DISK\QBHELP (-nolinks: one part checked alone, its links not followed).
#
# QBASIC.HLP is read by QBASIC.EXE (CMD\QBASIC\IDE_HELP.C) in HELPMAKE's
# source format, the same as HELP.HLP: ".context NAME" lines, ".topic"
# for the title, \b bold, \a link text \v context \v.  This script
# builds the Index from the topics and keeps the code page 437
# characters out of the editor:
#
#   #topic TITLE        a topic, and the title its window shows
#   #key NAME Kind      a name F1 finds the topic by (a keyword, case does
#                       not matter), and the Index entry "NAME Kind"
#   #key NAME           ...a name with no Index entry
#   #entry TEXT         an Index entry for the topic under other words
#                       ("Boolean Operators"), filed under its first letter
#   #context NAME       a name the program asks for (m.open, d.find, an
#                       error's message - the rest of the line, blanks
#                       and all), never in the Index
#   #index              where the Index's lettered lists go
#   #ascii              where the table of the 256 characters goes
#
# In the text: [[NAME]] is a link to NAME, [[words|NAME]] a link that
# says something else, {{NAME}} and {{words|NAME}} the same without the
# arrow brackets round it; {b} {i} {u} {p} are \b \i \u \p; {<} {>} the
# arrow brackets, {-} {=} rule characters, {o} a bullet, {*} the end
# mark, {-x40} forty rule characters; the box corners {ul} {ur} {ll}
# {lr}, {bl} a box side, {tl} {tr} its tees.  A backslash is a
# backslash.  A line starting '#!' is a comment.
use strict;
use warnings;
my $nolinks = @ARGV && $ARGV[0] eq '-nolinks' ? shift( @ARGV ) : 0;
my $out = pop @ARGV or die "usage: mkqbhlp.pl [-nolinks] SRC... OUT\n";
die "usage: mkqbhlp.pl [-nolinks] SRC... OUT\n" unless @ARGV;

my %map = ( '<' => chr(0x11), '>' => chr(0x10), '-' => chr(0xC4), '=' => chr(0xCD),
            'o' => chr(0xFE), '*' => chr(0x04), 'bl' => chr(0xB3), 'ul' => chr(0xDA),
            'ur' => chr(0xBF), 'll' => chr(0xC0), 'lr' => chr(0xD9), 'tl' => chr(0xC3),
            'tr' => chr(0xB4) );
my $WIDTH = 78;

sub expand {
    my ( $l ) = @_;
    $l =~ s/\{b\}/\\b/g;
    $l =~ s/\{i\}/\\i/g;
    $l =~ s/\{u\}/\\u/g;
    $l =~ s/\{p\}/\\p/g;
    $l =~ s/\[\[([^\]|]+)\|([^\]]+)\]\]/$map{'<'}\\a$1\\v$2\\v$map{'>'}/g;
    $l =~ s/\[\[([^\]]+)\]\]/$map{'<'}\\a$1\\v$1\\v$map{'>'}/g;
    $l =~ s/\{\{([^}|]+)\|([^}]+)\}\}/\\a$1\\v$2\\v/g;
    $l =~ s/\{\{([^}]+)\}\}/\\a$1\\v$1\\v/g;
    $l =~ s/\{-x(\d+)\}/chr(0xC4) x $1/ge;
    $l =~ s/\{=x(\d+)\}/chr(0xCD) x $1/ge;
    $l =~ s/\{(<|>|-|=|o|\*|bl|ul|ur|ll|lr|tl|tr)\}/$map{$1}/g;
    return $l;
}

# what a line shows, for the width check: links show their words only
sub shown_len {
    my ( $l ) = @_;
    $l =~ s/\[\[([^\]|]+)\|[^\]]+\]\]/<$1>/g;
    $l =~ s/\[\[([^\]]+)\]\]/<$1>/g;
    $l =~ s/\{\{([^}|]+)\|[^}]+\}\}/$1/g;
    $l =~ s/\{\{([^}]+)\}\}/$1/g;
    $l =~ s/\{-x(\d+)\}/'-' x $1/ge;
    $l =~ s/\{=x(\d+)\}/'=' x $1/ge;
    $l =~ s/\{[bipu]\}//g;
    $l =~ s/\{(<|>|-|=|o|\*|bl|ul|ur|ll|lr|tl|tr)\}/x/g;
    return length( $l );
}

# ---- read the parts
my @topics;         # { title, contexts[], body[] }
my @entries;        # [ shown text, context, sort key ]
my %seen;
my $cur;
for my $src ( @ARGV ) {
    open( my $i, '<', $src ) or die "$src: $!";
    while ( my $l = <$i> ) {
        $l =~ s/\r?\n$//;
        next if $l =~ /^#!/;
        if ( $l =~ /^#topic\s+(.+?)\s*$/ ) {
            $cur = { title => $1, contexts => [], body => [] };
            push @topics, $cur;
            next;
        }
        die "$src line $.: text before the first #topic\n" unless $cur;
        if ( $l =~ /^#key\s+(\S+)(?:\s+(.+?))?\s*$/ ) {
            my ( $name, $kind ) = ( $1, $2 );
            die "$src line $.: $name is a context twice\n" if $seen{ lc $name }++;
            push @{ $cur->{contexts} }, $name;
            push @entries, [ "$name $kind", $name, uc $name ] if defined $kind;
            next;
        }
        if ( $l =~ /^#context\s+(.+?)\s*$/ ) {
            die "$src line $.: $1 is a context twice\n" if $seen{ lc $1 }++;
            push @{ $cur->{contexts} }, $1;
            next;
        }
        if ( $l =~ /^#entry\s+(.+?)\s*$/ ) {
            die "$src line $.: #entry before the topic has a #key or #context\n" unless @{ $cur->{contexts} };
            push @entries, [ $1, $cur->{contexts}[0], uc $1 ];
            next;
        }
        if ( $l =~ /^#index\s*$/ ) { push @{ $cur->{body} }, "\0index"; next; }
        if ( $l =~ /^#ascii\s*$/ ) { push @{ $cur->{body} }, "\0ascii"; next; }
        die "$src line $.: unknown directive $l\n" if $l =~ /^#\w/;
        die "$src line $.: wider than $WIDTH columns\n" if shown_len( $l ) > $WIDTH;
        $l =~ s/\\/\\\\/g;
        push @{ $cur->{body} }, $l;
    }
    close $i;
}
for my $t ( @topics ) {
    die "topic \"$t->{title}\" has no #key or #context\n" unless @{ $t->{contexts} };
}

# ---- every link must lead somewhere
my %ctx = map { lc( $_ ) => 1 } map { @{ $_->{contexts} } } @topics;
for my $t ( @topics ) {
    last if $nolinks;
    for my $l ( @{ $t->{body} } ) {
        while ( $l =~ /\[\[(?:[^\]|]+\|)?([^\]]+)\]\]|\{\{(?:[^}|]+\|)?([^}]+)\}\}/g ) {
            my $to = defined $1 ? $1 : $2;
            die "topic \"$t->{title}\": a link to $to, which is no topic\n" unless $ctx{ lc $to };
        }
    }
}

# ---- the Index: a lettered box for each letter, two columns under it
my @index;
{
    my %by;
    for my $e ( @entries ) {
        my $letter = substr( $e->[2], 0, 1 );
        $letter = '#' if $letter !~ /[A-Z]/;
        push @{ $by{$letter} }, $e;
    }
    for my $letter ( sort keys %by ) {
        my @list = sort { $a->[2] cmp $b->[2] } @{ $by{$letter} };
        push @index, ' ' x 66 . chr(0xC9) . chr(0xCD) x 3 . chr(0xBB);
        push @index, chr(0xCD) x 66 . chr(0xB9) . " $letter " . chr(0xBA);
        push @index, ' ' x 66 . chr(0xC8) . chr(0xCD) x 3 . chr(0xBC);
        # down the first column, then the second; each entry a link
        # without the arrow brackets
        my $rows = int( ( @list + 1 ) / 2 );
        for my $r ( 0 .. $rows - 1 ) {
            my $left = $list[$r];
            my $right = $list[ $rows + $r ];
            my $line = '  ' . "\\a$left->[0]\\v$left->[1]\\v";
            if ( $right ) {
                $line .= ' ' x ( 35 - length( $left->[0] ) ) if length( $left->[0] ) < 35;
                $line .= "\\a$right->[0]\\v$right->[1]\\v";
            }
            push @index, $line;
        }
    }
}

# ---- the characters 0 to 255: eight columns of 32, each code and its
# character (0, 9, 10 and 13 cannot stand in a line, and show blank)
sub ascii_table {
    my @lines;
    for my $r ( 0 .. 31 ) {
        my $line = '';
        for my $c ( 0 .. 7 ) {
            my $code = $c * 32 + $r;
            my $ch = ( $code == 0 || $code == 9 || $code == 10 || $code == 13 ) ? ' ' : chr( $code );
            $ch = '\\\\' if $code == 92;           # HELPMAKE's way to write one
            $line .= sprintf( '%5d ', $code ) . $ch . '   ';
        }
        $line =~ s/\s+$//;
        push @lines, $line;
    }
    return @lines;
}

# ---- write
open( my $o, '>:raw', $out ) or die "$out: $!";
for my $t ( @topics ) {
    print $o ".context $_\r\n" for @{ $t->{contexts} };
    print $o ".topic $t->{title}\r\n";
    for my $l ( @{ $t->{body} } ) {
        if ( $l eq "\0index" ) {
            print $o "$_\r\n" for @index;
        } elsif ( $l eq "\0ascii" ) {
            print $o "$_\r\n" for ascii_table();
        } else {
            print $o expand( $l ), "\r\n";
        }
    }
}
close $o;
printf "%d topics, %d Index entries\n", scalar @topics, scalar @entries;
