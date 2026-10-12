#!/usr/bin/perl
# mkhelp.pl SRC... OUT - DISK\HELP.HLP from its source parts.
#
# HELP.HLP is read by HELP.EXE as it stands (HELPMAKE's source format,
# see CMD\HELP\HLP.C), so it could be written by hand.  This script
# takes the repetition out of writing a hundred command topics that all
# look alike, and keeps the code page 437 characters out of the editor:
#
#   #topic NAME|Alias|...   a command or statement: the names it answers
#                           to (the second, if there is one, is how the
#                           contents lists it), then the three frozen
#                           lines every command topic has - the Notes
#                           and Examples links, the rule - and the title
#   #title TEXT             the title shown, when it is not NAME
#   #notes                  the topic's Notes, as NAME--Notes
#   #examples               ...and its Examples, as NAME--Examples
#   #group NAME|Alias|...   a topic with a centred title and no frozen
#                           lines (Batch Commands)
#   #plain NAME|Alias|...   a topic written out in full below it
#   #contents               where the list of every #topic and #group
#                           goes, three columns, alphabetical
#   #nolist                 ...leave this topic out of that list
#
# In the text: [[NAME]] is a link to NAME, [[words|NAME]] a link that
# says something else; {b} {i} {u} {p} are HELPMAKE's \b \i \u \p (bold,
# the link colour, underline, plain); {<} {>} are the arrow brackets,
# {-} a rule character, {=} a double one, {o} a bullet, {*} the
# end-of-topic mark, {-x40} forty rule characters.  A backslash is a
# backslash: the parts are written as the text reads, and this script
# doubles each one for HELPMAKE's format, where \\ is the literal.
# A line of '#' on its own, or starting '#!', is a comment.
use strict;
my $out = pop @ARGV or die "usage: mkhelp.pl SRC... OUT\n";
die "usage: mkhelp.pl SRC... OUT\n" unless @ARGV;

my %map = ( '<' => chr(0x11), '>' => chr(0x10), '-' => chr(0xC4), '=' => chr(0xCD),
            'o' => chr(0xFE), '*' => chr(0x04), 'bl' => chr(0xB3), 'ul' => chr(0xDA),
            'ur' => chr(0xBF), 'll' => chr(0xC0), 'lr' => chr(0xD9), 'sh' => chr(0xB1) );
my $WIDTH = 78;                 # columns the window shows
my $RULE  = 76;                 # the rule under the frozen links

sub expand {
    my ( $l ) = @_;
    $l =~ s/\{b\}/\\b/g;
    $l =~ s/\{i\}/\\i/g;
    $l =~ s/\{u\}/\\u/g;
    $l =~ s/\{p\}/\\p/g;
    $l =~ s/\[\[([^\]|]+)\|([^\]]+)\]\]/<\\a$1\\v$2\\v>/g;
    $l =~ s/\[\[([^\]]+)\]\]/<\\a$1\\v$1\\v>/g;
    $l =~ s/\{-x(\d+)\}/chr(0xC4) x $1/ge;
    $l =~ s/\{=x(\d+)\}/chr(0xCD) x $1/ge;
    $l =~ s/\{(<|>|-|=|o|\*|bl|ul|ur|ll|lr|sh)\}/$map{$1}/g;
    return $l;
}

sub centred {
    my ( $text ) = @_;
    my $pad = int( ( $WIDTH - length( $text ) ) / 2 );
    $pad = 0 if $pad < 0;
    return ( ' ' x $pad ) . $text;
}

# ---- read everything into topics
my @topics;         # { kind, names[], title, body[], notes[], examples[], list }
my $cur;            # the topic being filled
my $part;           # 'body', 'notes' or 'examples'
my $has_contents = 0;
for my $src ( @ARGV ) {
    open( my $i, '<', $src ) or die "$src: $!";
    while ( my $l = <$i> ) {
        $l =~ s/\r?\n$//;
        next if $l =~ /^#!/ || $l eq '#';
        if ( $l =~ /^#(topic|group|plain)\s+(.+?)\s*$/ ) {
            my @names = split /\|/, $2;
            $cur = { kind => $1, names => \@names, title => $names[0], body => [], notes => [], examples => [], list => 1 };
            push @topics, $cur;
            $part = 'body';
            next;
        }
        die "$src line $.: text before the first #topic\n" unless $cur;
        if ( $l =~ /^#title\s+(.+?)\s*$/ )  { $cur->{title} = $1; next; }
        if ( $l =~ /^#nolist\s*$/ )         { $cur->{list} = 0; next; }
        if ( $l =~ /^#notes\s*$/ )          { $part = 'notes'; next; }
        if ( $l =~ /^#examples\s*$/ )       { $part = 'examples'; next; }
        if ( $l =~ /^#contents\s*$/ )       { push @{ $cur->{$part} }, "\0contents"; $has_contents = 1; next; }
        die "$src line $.: unknown directive $l\n" if $l =~ /^#\w/;
        $l =~ s/\\/\\\\/g;             # a backslash in the text is a backslash
        push @{ $cur->{$part} }, $l;
    }
    close $i;
}

# ---- the contents list: every listed topic, three columns
sub listname {
    my ( $t ) = @_;
    return @{ $t->{names} } > 1 ? $t->{names}[1] : $t->{names}[0];
}
my @listed = sort { lc( listname( $a ) ) cmp lc( listname( $b ) ) }
             grep { $_->{list} && $_->{kind} ne 'plain' } @topics;
my @contents;
{
    my $rows = int( ( @listed + 2 ) / 3 );
    my @cols = ( 0, 29, 57 );
    for my $r ( 0 .. $rows - 1 ) {
        my $line = '';
        for my $c ( 0 .. 2 ) {
            my $idx = $c * $rows + $r;
            next if $idx >= @listed;
            my $t = $listed[$idx];
            my $plain = '<' . listname( $t ) . '>';
            $line .= ' ' x ( $cols[$c] - length( $line ) ) if length( $line ) < $cols[$c];
            $line .= '<\\a' . listname( $t ) . '\\v' . $t->{names}[0] . '\\v>';
            # keep column positions by the shown width: the escapes take no room
            my $shown = $line;
            $shown =~ s/\\a//g; $shown =~ s/\\v[^\\]*\\v//g;
            $line .= ' ' x ( $cols[$c] + length( $plain ) - length( $shown ) ) if 0;
        }
        push @contents, $line;
    }
}

# the escapes have no width: pad by what is shown, not what is written
sub shown_len {
    my ( $l ) = @_;
    $l =~ s/\\v[^\\]*\\v//g;
    $l =~ s/\\\\/x/g;
    $l =~ s/\\[abiup]//g;
    return length( $l );
}
# redo the columns properly
@contents = ();
{
    my $rows = int( ( @listed + 2 ) / 3 );
    my @cols = ( 0, 29, 57 );
    for my $r ( 0 .. $rows - 1 ) {
        my $line = '';
        for my $c ( 0 .. 2 ) {
            my $idx = $c * $rows + $r;
            next if $idx >= @listed;
            my $t = $listed[$idx];
            my $want = $cols[$c];
            my $have = shown_len( $line );
            $line .= ' ' x ( $want - $have ) if $have < $want;
            $line .= '<\\a' . listname( $t ) . '\\v' . $t->{names}[0] . '\\v>';
        }
        push @contents, $line;
    }
}

# ---- write
open( my $o, '>:raw', $out ) or die "$out: $!";
my $lines = 0;
sub emit { for my $l ( @_ ) { print $o expand( $l ), "\r\n"; $lines++; } }

sub emit_body {
    my ( $body ) = @_;
    for my $l ( @$body ) {
        if ( $l eq "\0contents" ) { emit( @contents ); }
        else { emit( $l ); }
    }
}

sub frozen {
    my ( @links ) = @_;
    my $l = '';
    if ( @links ) {
        $l = '{i}' . join( '  ', map { '{<}\\a' . $_->[0] . '\\v' . $_->[1] . '\\v{>}' } @links ) . '{p}';
    }
    return ( $l, '{b}{i}{-x' . $RULE . '}{p}', ' ' );
}

my $n = 0;
for my $t ( @topics ) {
    my $name = $t->{names}[0];
    my $has_notes = @{ $t->{notes} } > 0;
    my $has_ex = @{ $t->{examples} } > 0;
    $n++;
    if ( $t->{kind} eq 'plain' ) {
        emit( map { ".context $_" } @{ $t->{names} } );
        emit( ".topic $t->{title}" );
        emit_body( $t->{body} );
        next;
    }
    if ( $t->{kind} eq 'group' ) {
        emit( map { ".context $_" } @{ $t->{names} } );
        emit( ".topic $t->{title}" );
        emit( centred( $t->{title} ), ' ' );
        emit_body( $t->{body} );
        emit( ' ', centred( '{*}' ) );
        next;
    }
    # a command topic
    emit( map { ".context $_" } @{ $t->{names} } );
    emit( ".topic $t->{title}", '.freeze 3' );
    my @links;
    push @links, [ 'Notes', "$name--Notes" ] if $has_notes;
    push @links, [ 'Examples', "$name--Examples" ] if $has_ex;
    emit( frozen( @links ) );
    emit( centred( $t->{title} ), ' ' );
    emit_body( $t->{body} );
    emit( ' ', centred( '{*}' ) );
    if ( $has_notes ) {
        emit( ".context $name--Notes", ".topic $t->{title}--Notes", '.freeze 3' );
        @links = ();
        push @links, [ 'Examples', "$name--Examples" ] if $has_ex;
        push @links, [ 'Syntax', $name ];
        emit( frozen( @links ) );
        emit( centred( $t->{title} . "{-}{-}Notes" ), ' ' );
        emit_body( $t->{notes} );
        emit( ' ', centred( '{*}' ) );
        $n++;
    }
    if ( $has_ex ) {
        emit( ".context $name--Examples", ".topic $t->{title}--Examples", '.freeze 3' );
        @links = ( [ 'Syntax', $name ] );
        push @links, [ 'Notes', "$name--Notes" ] if $has_notes;
        emit( frozen( @links ) );
        emit( centred( $t->{title} . "{-}{-}Examples" ), ' ' );
        emit_body( $t->{examples} );
        emit( ' ', centred( '{*}' ) );
        $n++;
    }
}
close $o;
printf "%s: %d topics, %d listed in the contents, %d lines, %d bytes\n", $out, $n, scalar( @listed ), $lines, -s $out;

# ---- every link must lead somewhere
my %ctx;
for my $t ( @topics ) {
    $ctx{ lc $_ } = 1 for @{ $t->{names} };
    if ( $t->{kind} eq 'topic' ) {
        $ctx{ lc "$t->{names}[0]--Notes" } = 1 if @{ $t->{notes} };
        $ctx{ lc "$t->{names}[0]--Examples" } = 1 if @{ $t->{examples} };
    }
}
open( my $chk, '<:raw', $out ) or die;
my $bad = 0;
while ( my $l = <$chk> ) {
    $l =~ s/\r?\n$//;
    if ( $l !~ /^\./ && shown_len( $l ) > $WIDTH ) {
        print "line $. is ", shown_len( $l ), " wide: $l\n";
        $bad++;
    }
    while ( $l =~ /\\a([^\\]*)\\v([^\\]*)\\v/g ) {
        my $target = $2;
        next if $ctx{ lc $target };
        print "no such topic: \"$target\" (link \"$1\")\n";
        $bad++;
    }
}
close $chk;
for my $want ( 'Contents', 'Help on Help', 'File Menu', 'Search Menu', 'Help Menu', 'Print Command',
               'Exit Command', 'Find Command', 'Repeat Last Find Command', 'Help on Help Command',
               'About Command', 'Find Dialog', 'Print Dialog', 'Printer Setup Dialog' ) {
    next if $ctx{ lc $want };
    print "HELP.EXE looks for a topic named \"$want\" and there is none\n";
    $bad++;
}
exit( $bad ? 1 : 0 );
