#!/usr/bin/perl
# mkfdkeys.pl OUTDIR - the key file DISK\FDMTEST.BAT types into FDISK.
#
# FDISK's screen is tested the way EDIT's is (MKEDKEYS.PL): with FDTEST
# naming a log and standard input taken from a file, it reads its keys
# and its mouse from the file and writes its screen into the log wherever
# the script says {DUMP}.  The same escapes as MKEDKEYS.PL:
#
#   {NAME} a key, {NAME*n} that key n times, {ALT+X} an Alt key
#   {PRESS r,c} {RELEASE r,c}   the left button down or up on a cell
#   {DRAG r,c} {MOVE r,c}       the pointer moved, the button held or not
#   {CLICK r,c} {DCLICK r,c}    a press and a release, once or twice
#
# Rows and columns are the screen's, from 0: the title is row 0, the
# menu bar row 1, the bar rows 4-6 starting at column 3.  Everything else
# is typed as it stands.  The script goes to OUTDIR\FDM1.KEY.
#
# WHAT THE SCRIPT DOES, on MULTI.IMG as the second disk (disk 1), whose
# 165312 sectors put 2234 of them in each of the bar's 74 cells:
#
#   Tab, picture            disk 1: slots 1-3, the container with 5-7
#   click 5,34, picture     partition 2 (columns 33-39) selected
#   press 5,35, drag 3      partition 3 is right behind it, so the block
#   right, picture, let go  cannot go anywhere; let go, it stays
#   click list row 14, D    partition 3 selected in the list and the
#   picture, Y, picture,    delete asked about and answered yes,
#   Enter, picture          done, and the free space it left
#   press 5,35, drag 5,39   partition 2 dragged four cells right into
#   picture, let go,        that space: it stops at the container and
#   picture, Y, picture,    ends on a cylinder (start 75600); the move
#   Enter                   asked about and made
#   click 5,34, C, picture  the free space it left selected; C, the size
#   Enter, picture, Y,      offered taken as it is, the question, made:
#   picture, Enter          a new slot 3 at 68544 + 7056
#   click list row 16, S,   partition 5 (16 MB FAT16) resized: 8 typed,
#   picture, 8, picture,    the bar showing it, the question, made -
#   Enter, picture, Y,      ending on the cylinder nearest 8 MB
#   picture, Enter          (91854 + 16002)
#   press 5,45, drag 5,50   partition 5 dragged five cells right inside
#   picture, let go,        its own link, which now has room: 102942
#   picture, Y, picture,
#   Enter
#   click 1,15, picture,    the Partition menu, from the mouse
#   Esc
#   Alt+H, picture, Esc     the Help menu, from the keyboard
#   F1, picture, Esc        the help, in the viewer
#   M, 46, picture, Enter,  partition 5 moved by typing: 46 MB, ending
#   picture, Y, picture,    on the nearest cylinder - 93870
#   Enter
#   Enter, picture, Esc     partition 5's details (/P) in the viewer
#   X, picture              not a key: the status line says so
#   rows 19, 18, 16: D, Y,  logical drives 7, 6 and 5 deleted, which
#   Enter each              empties the extended partition...
#   row 15: D, Y, Enter,    ...so it can be deleted too
#   picture
#   row 15: C, space,       the free space it left, the box's check box
#   picture, Enter,         ticked: an extended partition, in slot 4, at
#   picture, Y, picture,    91728 + 73584
#   Enter
#   row 16: C, Enter,       the free space inside it: a logical drive in
#   picture, Y, picture,    an empty container, its record where the
#   Enter, picture          container starts (91728) and the volume a
#                           track on: 91791 + 73521
#   Q                       leave
use strict; use warnings;

my $out = shift or die "usage: mkfdkeys.pl OUTDIR\n";

my %key = (
    ENTER => "\r", ESC => "\x1B", BS => "\x08", TAB => "\x09", SHTAB => "\0\x0F",
    UP => "\0\x48", DOWN => "\0\x50", LEFT => "\0\x4B", RIGHT => "\0\x4D",
    HOME => "\0\x47", END => "\0\x4F", PGUP => "\0\x49", PGDN => "\0\x51",
    DEL => "\0\x53",
    F1 => "\0\x3B", F2 => "\0\x3C", F5 => "\0\x3F", F10 => "\0\x44",
    DUMP => "\xFF\xFF",
);
my %altscan = (
    D=>0x20, F=>0x21, H=>0x23, P=>0x19, X=>0x2D,
);

sub mouse {
    my ($buttons, $row, $col) = @_;
    return "\xFE" . chr($buttons) . chr($row) . chr($col);
}

sub render {
    my ($script) = @_;
    my $bytes = '';
    while (length $script) {
        if ($script =~ s/^\{(PRESS|RELEASE|DRAG|MOVE|CLICK|DCLICK) (\d+),(\d+)\}//) {
            my ($what, $row, $col) = ($1, $2, $3);
            if    ($what eq 'PRESS' || $what eq 'DRAG') { $bytes .= mouse(1, $row, $col) }
            elsif ($what eq 'RELEASE' || $what eq 'MOVE') { $bytes .= mouse(0, $row, $col) }
            else {
                my $times = $what eq 'DCLICK' ? 2 : 1;
                $bytes .= (mouse(1, $row, $col) . mouse(0, $row, $col)) x $times;
            }
        } elsif ($script =~ s/^\{ALT\+(.)\}//) {
            my $ch = uc $1;
            die "no Alt scan code for $ch\n" unless exists $altscan{$ch};
            $bytes .= "\0" . chr($altscan{$ch});
        } elsif ($script =~ s/^\{([A-Z0-9]+)(?:\*(\d+))?\}//) {
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
    FDM1 => '{TAB}{DUMP}'
          . '{CLICK 5,34}{DUMP}'
          . '{PRESS 5,35}{DRAG 5,38}{DUMP}{RELEASE 5,38}{DUMP}'
          . '{CLICK 14,10}D{DUMP}Y{DUMP}{ENTER}{DUMP}'
          . '{PRESS 5,35}{DRAG 5,39}{DUMP}{RELEASE 5,39}{DUMP}Y{DUMP}{ENTER}'
          . '{CLICK 5,34}C{DUMP}{ENTER}{DUMP}Y{DUMP}{ENTER}'
          . '{CLICK 16,10}S{DUMP}8{DUMP}{ENTER}{DUMP}Y{DUMP}{ENTER}'
          . '{PRESS 5,45}{DRAG 5,50}{DUMP}{RELEASE 5,50}{DUMP}Y{DUMP}{ENTER}'
          . '{CLICK 1,15}{DUMP}{ESC}'
          . '{ALT+H}{DUMP}{ESC}'
          . '{F1}{DUMP}{ESC}'
          . 'M46{DUMP}{ENTER}{DUMP}Y{DUMP}{ENTER}'
          . '{ENTER}{DUMP}{ESC}'
          . 'X{DUMP}'
          . '{CLICK 19,10}DY{ENTER}{CLICK 18,10}DY{ENTER}{CLICK 16,10}DY{ENTER}'
          . '{CLICK 15,10}DY{ENTER}{DUMP}'
          . '{CLICK 15,10}C {DUMP}{ENTER}{DUMP}Y{DUMP}{ENTER}'
          . '{CLICK 16,10}C{ENTER}{DUMP}Y{DUMP}{ENTER}{DUMP}Q',
);

for my $name (sort keys %script) {
    my $path = "$out/$name.KEY";
    open(my $fh, '>', $path) or die "$path: $!";
    binmode $fh;
    print $fh render($script{$name});
    close $fh;
    printf "%s: %d bytes\n", $path, length render($script{$name});
}
