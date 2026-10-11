#!/usr/bin/perl
# mkdskeys.pl OUTDIR - the key files DISK\DSTEST.BAT types into DOSSHELL.
#
# DOSSHELL is tested the way EDIT is (see MKEDKEYS.PL): with DSTEST
# naming a log and standard input taken from a file, the shell reads its
# keys from the file - the bytes 21h/07h hands back, a zero and a scan
# code for an extended key - and writes its screen into the log wherever
# the script says {DUMP}.  FFh and a byte set the shift state, FFh FFh
# is the dump, and FFh FEh ({SHOT}) writes a graphics screen as a .BMP
# file beside the log, named after it: DS7.SCR's first is DS701.BMP.
#
# {NAME} is a key and {NAME*n} that key n times; {CTRL+X} a control
# key, {ALT+X} an Alt key, {SHIFT} ... {NOSHIFT} Shift held down, {ALT}
# a tap of Alt; everything else is typed as it stands.  Each script
# goes to OUTDIR\<name>.KEY.
#
# THE MOUSE is FEh, the buttons, a row and a column, as EDIT's is:
# {PRESS r,c} and {RELEASE r,c} are the left button, {DRAG r,c} moves
# with it held, {MOVE r,c} moves without, {CLICK r,c} is a press and a
# release and {DCLICK r,c} two of them.  Rows and columns count from 0;
# the title is row 0 and the menu bar row 1.
#
# A TEST RUN HAS NO DOSSHELL.INI unless DSINI names one, so every script
# starts from the same screen - 25 lines of text with /T, the tree and
# the files above the program list, the keys in the tree - and the
# settings of whoever runs the tests are left alone.  A script ends when
# its keys run out, which leaves the shell without saving anything; DS8
# and DS9 are the pair that leave through the menu, to a file of their
# own.
#
# WHAT THE SCRIPTS CAN COUNT ON is what DSTEST.BAT makes: \DS\ONE with
# A.TXT, B.TXT and C.DAT in it and a directory DEEP holding D.TXT, an
# empty \DS\TWO, and \DS\ONE the current directory.  The files are then
# on rows 5, 6 and 7.  Where a directory is in the tree depends on what
# else the disk has, so no script points the mouse at one.
use strict; use warnings;

my $out = shift or die "usage: mkdskeys.pl OUTDIR\n";

my %key = (
    ENTER => "\r", ESC => "\x1B", BS => "\x08", TAB => "\x09", SHTAB => "\0\x0F",
    UP => "\0\x48", DOWN => "\0\x50", LEFT => "\0\x4B", RIGHT => "\0\x4D",
    HOME => "\0\x47", END => "\0\x4F", PGUP => "\0\x49", PGDN => "\0\x51",
    INS => "\0\x52", DEL => "\0\x53", SPACE => " ",
    F1 => "\0\x3B", F3 => "\0\x3D", F5 => "\0\x3F", F7 => "\0\x41", F8 => "\0\x42",
    F9 => "\0\x43", F10 => "\0\x44", SHF5 => "\0\x58", SHF8 => "\0\x5B", SHF9 => "\0\x5C",
    ALTF4 => "\0\x6B", CSLASH => "\0\x95", CSTAR => "\0\x96", CBSLASH => "\x1C",
    SHIFT => "\xFF\x02", NOSHIFT => "\xFF\x00", ALT => "\xFF\x08\xFF\x00",
    DUMP => "\xFF\xFF", SHOT => "\xFF\xFE",
);
my %altscan = (
    Q=>0x10, W=>0x11, E=>0x12, R=>0x13, T=>0x14, Y=>0x15, U=>0x16, I=>0x17, O=>0x18,
    P=>0x19, A=>0x1E, S=>0x1F, D=>0x20, F=>0x21, G=>0x22, H=>0x23, J=>0x24, K=>0x25,
    L=>0x26, Z=>0x2C, X=>0x2D, C=>0x2E, V=>0x2F, B=>0x30, N=>0x31, M=>0x32,
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
        } elsif ($script =~ s/^\{([A-Z0-9]+)\+(.)\}//) {
            my ($mod, $ch) = ($1, uc $2);
            if ($mod eq 'CTRL') { $bytes .= chr(ord($ch) & 0x1F) }
            elsif ($mod eq 'ALT') {
                die "no Alt scan code for $ch\n" unless exists $altscan{$ch};
                $bytes .= "\0" . chr($altscan{$ch});
            } else { die "unknown modifier $mod\n" }
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

# The scripts.  Each says in its comment what DSTEST.BAT shows after it.
my %scripts = (
    # 1: the screen as it starts; the menu bar (F10), each menu in turn;
    #    help - its list of subjects, one of them read, and back; About.
    DS1 => '{DUMP}{F10}{DUMP}{ENTER}{DUMP}{RIGHT}{DUMP}{RIGHT}{DUMP}{RIGHT}{DUMP}{RIGHT}{DUMP}{ESC}'
         . '{F1}{DUMP}{TAB}{ENTER}{DUMP}{BS}{ESC}{ALT+H}A{DUMP}{ENTER}',

    # 2: files.  B.TXT copied to \DS\TWO (F8); B.TXT and C.DAT selected
    #    with Shift+Down and moved there (F7) - B.TXT is there already,
    #    so the shell asks, and is told Yes; A.TXT renamed FIRST.TXT and
    #    made read-only; Del on it, Yes - and DOS will not; a directory
    #    NEWDIR made in \DS\ONE from the tree.
    #    -> \DS\ONE: FIRST.TXT (read-only), DEEP, NEWDIR
    #       \DS\TWO: B.TXT, C.DAT
    DS2 => '{TAB}{DOWN}{DUMP}{F8}{DUMP}\\DS\\TWO{ENTER}'
         . '{SHIFT}{DOWN}{NOSHIFT}{DUMP}{F7}\\DS\\TWO{ENTER}{DUMP}Y{DUMP}'
         . '{ALT+F}N{DUMP}FIRST.TXT{ENTER}{DUMP}'
         . '{ALT+F}G{DUMP}{DOWN*3}{SPACE}{ENTER}'
         . '{DEL}{DUMP}Y{DUMP}{ENTER}'
         . '{SHTAB}{ALT+F}E{DUMP}NEWDIR{ENTER}{DUMP}',

    # 3: the tree and the views.  + opens \DS\ONE, * all of it, - shuts
    #    it; then each view: one list, two, all files, programs, and
    #    files with programs again - where Shift+Tab takes the keys back
    #    from the programs to the files.  A search of the whole disk for
    #    D.TXT, and Esc out of its answer; Show Information; File
    #    Display Options with the order turned round; Confirmation.
    DS3 => '+{DUMP}*{DUMP}-{DUMP}{ALT+V}S{DUMP}{ALT+V}D{DUMP}{ALT+V}A{DUMP}{ALT+V}P{DUMP}{ALT+V}F{DUMP}'
         . '{SHTAB}{ALT+F}H{DUMP}D.TXT{ENTER}{DUMP}{ESC}{DUMP}'
         . '{ALT+O}I{DUMP}{ENTER}{ALT+O}F{DUMP}{TAB}{TAB}{SPACE}{ENTER}{DUMP}'
         . '{ALT+O}C{DUMP}{ESC}',

    # 4: the program list.  F1 on Command Prompt, which has help text of
    #    its own; into Disk Utilities and out again; a new entry,
    #    "Hello", whose command has a %1 and a box to ask for it; the
    #    entry opened - the box, and VER run with what it holds; its
    #    properties; Reorder, up one; Delete.
    DS4 => '{TAB}{TAB}{DUMP}{F1}{DUMP}{ENTER}{DOWN}{DOWN}{ENTER}{DUMP}{ESC}{DUMP}'
         . '{ALT+F}N{ENTER}Hello{TAB}VER %1{ENTER}'
         . 'Say hello{TAB}What VER is to be given{TAB}Switch:{TAB}/R{ENTER}{DUMP}'
         . '{ENTER}{DUMP}{ENTER}{DUMP}{ALT+F}P{DUMP}{ESC}'
         . '{ALT+F}R{UP}{ENTER}{DUMP}{DEL}{DUMP}Y{DUMP}',

    # 5: the viewer on A.TXT - text, hexadecimal and text again - and
    #    File, Run with VER for a command.
    DS5 => '{TAB}{F9}{DUMP}{F9}{DUMP}{F9}{ESC}{ALT+F}RVER{ENTER}{DUMP}',

    # 6: the mouse.  A click on A.TXT and a Shift+click on C.DAT select
    #    all three; Options and Show Information from the menus; a
    #    double click on Disk Utilities, and one on the line that leads
    #    back; View, Single File List, and a click on the files' scroll
    #    arrow.
    DS6 => '{CLICK 5,40}{SHIFT}{CLICK 7,40}{NOSHIFT}{DUMP}'
         . '{CLICK 1,10}{DUMP}{CLICK 6,12}{DUMP}{ENTER}'
         . '{DCLICK 16,10}{DUMP}{DCLICK 14,10}{DUMP}'
         . '{CLICK 1,20}{CLICK 3,22}{CLICK 23,79}{DUMP}',

    # 7: /G:M1 - the same shell drawn in 640x480; the log says [vga30]
    #    under each screen when the machine could show it and [text25]
    #    when it could not.  The Display dialog lists what it can show.
    DS7 => '{DUMP}{SHOT}{ALT+O}D{DUMP}{ESC}{ALT+F}{DUMP}{ESC}',

    # 8: DSINI names a file.  The Program List view and the second
    #    color scheme chosen, and out through File, Exit - which writes
    #    the file, typed after the run.
    DS8 => '{ALT+V}P{ALT+O}O{DOWN}{ENTER}{DUMP}{ALT+F}X',

    # 9: ...and started again with the same file: the program list
    #    alone is what comes up.
    DS9 => '{DUMP}',
);

for my $name (sort keys %scripts) {
    open(my $fh, '>:raw', "$out/$name.KEY") or die "$out/$name.KEY: $!";
    print $fh render($scripts{$name});
    close $fh;
}
print scalar(keys %scripts), " key files in $out\n";
