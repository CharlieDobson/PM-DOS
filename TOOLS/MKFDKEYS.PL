#!/usr/bin/perl
# mkfdkeys.pl OUTDIR - the key file DISK\FDMTEST.BAT types into FDISK.
#
# FDISK's screen is tested the way EDIT's is (MKEDKEYS.PL): with FDTEST
# naming a log and standard input taken from a file, it reads its keys
# from the file - a byte a key, 0 and a scan code for a grey one - and
# writes its screen into the log wherever the script says {DUMP}.  The
# same {NAME} and {NAME*n} escapes as MKEDKEYS.PL; everything else is
# typed as it stands.  The script goes to OUTDIR\FDM1.KEY.
#
# WHAT THE SCRIPT DOES, on MULTI.IMG as the second disk (disk 1):
#
#   Tab, picture         disk 1 as it is: four slots and the chain
#   F1, picture, Esc     the help, in the viewer
#   Down x4, picture     the cursor on the first logical drive
#   P, picture, Esc      its plan, in the viewer
#   M, picture           move mode: the second bar under the first
#   Right x2, picture    two cylinders up, the dry run's verdict on it
#   Esc                  and nothing moved
#   Up x2, S, picture    resize mode on slot 3, the Linux partition
#   Esc
#   X, picture           not a key: the status line says so
#   B, picture, N        the boot-code question, answered no
#   D, picture, N        the question, answered no
#   W, picture           writes armed, in the title bar
#   D, Y, Enter, picture partition 3 deleted for real, and the table
#                        read back - which MKHD puts back on the next
#                        build, as FDMTEST always has
#   C, picture           the emptied slot and C's first question...
#   Esc, picture         ...abandoned
#   C, Enter, Enter,     ...and asked again with both answers blank: the
#   picture, Y, Enter,   dry run's proposal on the second bar, then made,
#   picture              and the table read back with a new slot 3
#   Q                    leave
use strict; use warnings;

my $out = shift or die "usage: mkfdkeys.pl OUTDIR\n";

my %key = (
    ENTER => "\r", ESC => "\x1B", BS => "\x08", TAB => "\x09",
    UP => "\0\x48", DOWN => "\0\x50", LEFT => "\0\x4B", RIGHT => "\0\x4D",
    HOME => "\0\x47", END => "\0\x4F", PGUP => "\0\x49", PGDN => "\0\x51",
    F1 => "\0\x3B", F2 => "\0\x3C",
    DUMP => "\xFF\xFF",
);

sub render {
    my ($script) = @_;
    my $bytes = '';
    while (length $script) {
        if ($script =~ s/^\{([A-Z0-9]+)(?:\*(\d+))?\}//) {
            die "unknown key {$1}\n" unless exists $key{$1};
            $bytes .= $key{$1} x ($2 // 1);
        } else {
            $script =~ s/^(.)//s;
            $bytes .= $1;
        }
    }
    return $bytes;
}

my %script = (
    FDM1 => '{TAB}{DUMP}{F1}{DUMP}{ESC}{DOWN*4}{DUMP}P{DUMP}{ESC}M{DUMP}{RIGHT*2}{DUMP}{ESC}' .
            '{UP*2}S{DUMP}{ESC}X{DUMP}B{DUMP}ND{DUMP}NW{DUMP}DY{ENTER}{DUMP}' .
            'C{DUMP}{ESC}{DUMP}C{ENTER}{ENTER}{DUMP}Y{ENTER}{DUMP}Q',
);

for my $name (sort keys %script) {
    my $path = "$out/$name.KEY";
    open(my $fh, '>', $path) or die "$path: $!";
    binmode $fh;
    print $fh render($script{$name});
    close $fh;
    printf "%s: %d bytes\n", $path, length render($script{$name});
}
