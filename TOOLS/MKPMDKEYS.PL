#!/usr/bin/perl
# mkpmdkeys.pl OUTDIR - the key files DISK\PMDTEST.BAT types into PMD.
#
# PMD is tested the way EDIT is (see MKEDKEYS.PL, whose notation this
# is): with PMDTEST naming a log and standard input taken from a file,
# PMD reads its keys from the file - the bytes 21h/07h hands back, a
# zero and a scan code for an extended key - and writes its screen into
# the log wherever the script says {DUMP}.  FFh and a byte set the shift
# state (letting go of Alt on its own opens the menu bar), FFh FFh is
# the dump, and FEh, the buttons, a row and a column is the mouse.
#
# {NAME} is a key and {NAME*n} that key n times; {ALT+X} an Alt key,
# {ALT} a tap of Alt; {CLICK r,c} a press and a release of the left
# button on that cell, {PRESS r,c} and {RELEASE r,c} the halves of one;
# everything else is typed as it stands.  Each script goes to
# OUTDIR\<name>.KEY.
use strict; use warnings;

my $out = shift or die "usage: mkpmdkeys.pl OUTDIR\n";

my %key = (
    ENTER => "\r", ESC => "\x1B", BS => "\x08", TAB => "\x09", SHTAB => "\0\x0F",
    UP => "\0\x48", DOWN => "\0\x50", LEFT => "\0\x4B", RIGHT => "\0\x4D",
    HOME => "\0\x47", END => "\0\x4F", PGUP => "\0\x49", PGDN => "\0\x51",
    DEL => "\0\x53", F3 => "\0\x3D", F5 => "\0\x3F", F10 => "\0\x44",
    ALT => "\xFF\x08\xFF\x00", DUMP => "\xFF\xFF",
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
        if ($script =~ s/^\{(PRESS|RELEASE|CLICK) (\d+),(\d+)\}//) {
            my ($what, $row, $col) = ($1, $2, $3);
            if    ($what eq 'PRESS')   { $bytes .= mouse(1, $row, $col) }
            elsif ($what eq 'RELEASE') { $bytes .= mouse(0, $row, $col) }
            else                       { $bytes .= mouse(1, $row, $col) . mouse(0, $row, $col) }
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

# The scripts.  Each says in its comment what PMDTEST.BAT shows after it.
my %scripts = (
    # 1: the main screen, then every page by its letter - the Memory
    #    page scrolled to its end as well - and out with F3.
    PMD1 => '{DUMP}P{DUMP}{ENTER}M{DUMP}{PGDN}{DUMP}{ENTER}V{DUMP}{ENTER}N{DUMP}{ENTER}'
          . 'O{DUMP}{ENTER}U{DUMP}{ENTER}A{DUMP}{ENTER}D{DUMP}{ENTER}L{DUMP}{ENTER}'
          . 'C{DUMP}{ENTER}Q{DUMP}{ENTER}T{DUMP}{ENTER}R{DUMP}{ENTER}{F3}',

    # 2: the menus.  A tap of Alt and Enter opens File; Alt+U and Alt+H
    #    the other two; About; the first file in the File menu shown and
    #    closed; a button clicked with the mouse, and its page's OK
    #    button clicked; File, Exit.
    PMD2 => '{ALT}{DUMP}{ENTER}{DUMP}{ESC}{ALT+U}{DUMP}{ESC}{ALT+H}{DUMP}A{DUMP}{ENTER}'
          . '{ALT+F}1{DUMP}{ENTER}{CLICK 2,6}{DUMP}{ESC}'
          . '{ALT+F}X',

    # 3: Utilities.  Memory Block Display, down the list; Memory
    #    Browser, the ROM BIOS searched for "bios"; Insert Command and
    #    Test Printer looked at and cancelled.
    PMD3 => '{ALT+U}M{DUMP}{DOWN*11}{DUMP}{END}{DUMP}{ESC}'
          . '{ALT+U}B{DUMP}{TAB}bios{ENTER}{DUMP}{ENTER}{ESC}'
          . '{ALT+U}I{DUMP}{ESC}{ALT+U}T{DUMP}{ESC}{F3}',

    # 4: File.  Find File for *.SYS from C:\ down: the list, the first
    #    file's details, and the file itself.  Then Print Report: only
    #    the summary and the Computer page, to the file C:\PMDT\RPT.TXT.
    #    -> RPT.TXT, which PMDTEST.BAT types
    PMD4 => '{ALT+F}F{DUMP}CONFIG.SYS{TAB}C:\\{ENTER}{DUMP}{ALT+I}{DUMP}{ENTER}{ALT+D}{DUMP}{ENTER}{ESC}'
          . '{ALT+F}P{DUMP}A{DUMP}A{DOWN*2} {DOWN} {DUMP}F{TAB}C:\\PMDT\\RPT.TXT{ENTER}{F3}',

    # 5: Insert Command.  PMDT\INS.TXT is "BUFFERS=10" and "FILES=30".
    #    FILES=40 replaces the line that sets FILES (Yes); then
    #    LASTDRIVE=Z, which the file has not got, goes on the end.
    #    -> "BUFFERS=10" CR LF "FILES=40" CR LF "LASTDRIVE=Z" CR LF
    PMD5 => '{ALT+U}I{ENTER}{DUMP}{TAB}C:\\PMDT\\INS.TXT{ENTER}{DUMP}Y{DUMP}{ENTER}'
          . '{ALT+U}I{DOWN*2}{ENTER}{TAB}C:\\PMDT\\INS.TXT{ENTER}{DUMP}{ENTER}{F3}',
);

for my $name (sort keys %scripts) {
    open my $fh, '>', "$out/$name.KEY" or die "$out/$name.KEY: $!\n";
    binmode $fh;
    print $fh render($scripts{$name});
    close $fh;
}
