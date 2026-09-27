#!/usr/bin/perl
# mkedkeys.pl OUTDIR - the key files DISK\EDTEST.BAT types into EDIT.
#
# EDIT is tested the way CMD.EXE's line editor is (see MKKEYS.PL): with
# EDTEST naming a log and standard input taken from a file, EDIT reads
# its keys from the file - the bytes 21h/07h hands back, a zero and a
# scan code for an extended key - and writes its screen into the log
# wherever the script says {DUMP}.  Two more escapes than MKKEYS.PL's:
# FFh and a byte set the shift state (Shift+arrows select; letting go of
# Alt on its own opens the menu bar), and FFh FFh is the dump.
#
# {NAME} is a key and {NAME*n} that key n times; {CTRL+X} a control
# key, {ALT+X} an Alt key, {SHIFT} ... {NOSHIFT} Shift held down, {ALT}
# a tap of Alt; everything else is typed as it stands.  Each script
# goes to OUTDIR\<name>.KEY.
#
# THE MOUSE is FEh, the buttons, a row and a column: EDIT puts the
# pointer on that screen cell with INT 33h 04h and the buttons down or
# up there with 21h/F0h AL=0Bh, and reads the result back through INT
# 33h as it would the mouse's.  {PRESS r,c} and {RELEASE r,c} are the
# left button, {DRAG r,c} moves with it held, {MOVE r,c} moves without,
# {CLICK r,c} is a press and a release and {DCLICK r,c} two of them.
# Rows and columns count from 0, the menu bar being row 0.
use strict; use warnings;

my $out = shift or die "usage: mkedkeys.pl OUTDIR\n";

my %key = (
    ENTER => "\r", ESC => "\x1B", BS => "\x08", TAB => "\x09", SHTAB => "\0\x0F",
    UP => "\0\x48", DOWN => "\0\x50", LEFT => "\0\x4B", RIGHT => "\0\x4D",
    HOME => "\0\x47", END => "\0\x4F", PGUP => "\0\x49", PGDN => "\0\x51",
    INS => "\0\x52", DEL => "\0\x53",
    F1 => "\0\x3B", F2 => "\0\x3C", F3 => "\0\x3D", F4 => "\0\x3E", F5 => "\0\x3F",
    F6 => "\0\x40", F7 => "\0\x41", F8 => "\0\x42", F9 => "\0\x43", F10 => "\0\x44",
    CLEFT => "\0\x73", CRIGHT => "\0\x74", CEND => "\0\x75", CPGDN => "\0\x76",
    CHOME => "\0\x77", CPGUP => "\0\x84", CUP => "\0\x8D", CDOWN => "\0\x91",
    CINS => "\0\x92", CDEL => "\0\x93", CTRLF4 => "\0\x61", CTRLF6 => "\0\x63",
    CTRLF8 => "\0\x65",
    SHIFT => "\xFF\x02", NOSHIFT => "\xFF\x00", ALT => "\xFF\x08\xFF\x00",
    DUMP => "\xFF\xFF",
);
my %altscan = (
    Q=>0x10, W=>0x11, E=>0x12, R=>0x13, T=>0x14, Y=>0x15, U=>0x16, I=>0x17, O=>0x18,
    P=>0x19, A=>0x1E, S=>0x1F, D=>0x20, F=>0x21, G=>0x22, H=>0x23, J=>0x24, K=>0x25,
    L=>0x26, Z=>0x2C, X=>0x2D, C=>0x2E, V=>0x2F, B=>0x30, N=>0x31, M=>0x32,
    1=>0x78, 2=>0x79, 3=>0x7A, 4=>0x7B, 5=>0x7C, 6=>0x7D, 7=>0x7E, 8=>0x7F, 9=>0x80,
    0=>0x81,
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

# The scripts.  Each says in its comment what EDTEST.BAT checks after it.
my %scripts = (
    # 1: ED1.TXT is "Hello, world." and "Second line".  Look at it, type
    #    " (edited)" at the end of line 1 and "third line" on the empty
    #    line the file ends with, save through the File menu (a tap of
    #    Alt, F, S) and leave (Alt, F, X).
    #    -> "Hello, world. (edited)" CR LF "Second line" CR LF "third line"
    ED1 => '{DUMP}{END} (edited){CEND}third line{DUMP}{ALT}FS{DUMP}{ALT}FX',

    # 2: a new file.  Auto-indent, Tab at the start of a line, Enter in
    #    the middle of one, Shift+End and Shift+Del to cut, Shift+Ins to
    #    paste in virtual space, then Save As with a long name.
    #    -> "Long Name.txt":  (empty)
    #                         "  indented"
    #                         "        tab"
    #                         "          kept"
    #                         "          firstover"
    ED2 => 'first{ENTER}  indented{ENTER}kept{HOME}{TAB}tab{ENTER}'
         . '{CHOME}{SHIFT}{END}{NOSHIFT}{SHIFT}{DEL}{NOSHIFT}{CEND}{ENTER}'
         . '{SHIFT}{INS}{NOSHIFT}over{DUMP}'
         . '{ALT}FA{DUMP}\\ED\\Long Name.txt{ENTER}{DUMP}{ALT}FX',

    # 3: ED3.TXT is "one cat", "two cat", "three cats".  Find "cat"
    #    (line 1), F3 (line 2); Replace cat with dog, Change All (all
    #    three); Replace dog with pig, Find and Verify: Skip "dogs",
    #    Change line 1's, Cancel at line 2's.
    #    -> "one pig" CR LF "two dog" CR LF "three dogs" CR LF
    ED3 => '{ALT}SF{DUMP}cat{ENTER}{DUMP}{F3}{DUMP}'
         . '{ALT}SR{DUMP}cat{TAB}dog{ALT+A}{DUMP}{ENTER}'
         . '{ALT}SRdog{TAB}pig{ENTER}{DUMP}S{ENTER}{ESC}{DUMP}{ENTER}'
         . '{ALT}FS{ALT}FX',

    # 4: two windows, a second file opened into one of them through the
    #    Open dialog, F6 between them, the View menu's list of files,
    #    help, and leaving with a change not saved (No).
    ED4 => '{CTRLF6}{DUMP}{ALT}FO{DUMP}\\ED\\ED1.TXT{ENTER}{DUMP}{F6}zz{F6}{DUMP}'
         . '{ALT}V{DUMP}{ESC}{F1}{DUMP}{TAB}{ENTER}{DUMP}{ESC}{ALT}FX{DUMP}N',

    # 5: /R: typing changes nothing, and Save asks for a new name
    #    (Esc) - then out, with nothing to ask
    ED5 => 'xyz{DEL}{DUMP}{ALT}FS{DUMP}{ESC}{ALT}FX',

    # 6: /16: a binary file shown sixteen bytes to a line, one byte
    #    typed over, saved - the file must be the same but for that byte
    ED6 => '{DUMP}{INS}Z{ALT}FS{ALT}FX',

    # 7: the WordStar keys and bookmarks: ^Y cuts a line, ^K1 marks,
    #    ^Q1 comes back, ^QY cuts the rest of a line, ^N breaks one.
    #    -> ED7.TXT
    ED7 => '{CTRL+Y}{CTRL+K}1{CTRL+X}{CTRL+X}{CTRL+Q}1{CTRL+D}{CTRL+D}{CTRL+Q}Y'
         . '{CTRL+X}{CTRL+N}{DUMP}{ALT}FS{ALT}FX',

    # 8: /H - fifty lines - then F4, the screen from before, and back
    ED8 => '{DUMP}{F4}{DUMP}{ENTER}{ALT}FX',

    # 9: EDTABS.TXT has tabs.  Settings: tab stops of 4 (the screen
    #    shows them); Colors, looked at and cancelled; Print the whole
    #    file to PRN (the harness captures the printer); Ctrl+F6 splits,
    #    Ctrl+F8 and Up twice move the line between, Ctrl+F4 closes one.
    ED9 => '{DUMP}{ALT}OS{DUMP}4{ENTER}{DUMP}{ALT}OC{DUMP}{ESC}'
         . '{ALT}FP{DUMP}{ENTER}{CTRLF6}{CTRLF8}{UP}{UP}{ENTER}{DUMP}{CTRLF4}{DUMP}{ALT}FX',

    # 10: EDIT W*.TXT opens W1.TXT ("alpha") and W2.TXT ("beta").  Copy
    #     "alpha" (Shift+End, Ctrl+Ins), View 2, paste at the end of W2,
    #     then File Close: the question, Yes.
    #     -> W2.TXT: "beta" CR LF "alpha"
    ED10 => '{SHIFT}{END}{NOSHIFT}{CINS}{ALT}V2{CEND}{SHIFT}{INS}{NOSHIFT}{DUMP}'
          . '{ALT}FC{DUMP}Y{DUMP}{ALT}FX',

    # 11: ED11.TXT is a, b, c, d.  Shift+Down three times selects three
    #     lines; Tab, Tab indents them twice, Shift+Tab once back.
    #     -> eight spaces before a, b and c; d as it was
    ED11 => '{SHIFT}{DOWN*3}{NOSHIFT}{TAB}{TAB}{DUMP}{SHTAB}{DUMP}{ALT}FS{ALT}FX',

    # 12: EDLF.TXT has LF line ends and a line of 5000 x's: the warning
    #     that it was split, then saved - CR LF throughout, the long line
    #     in two (4096 and 904)
    ED12 => '{DUMP}{ENTER}{DUMP}{ALT}FS{ALT}FX',

    # 13: /S: the title is the short name
    ED13 => '{DUMP}{ALT}FX',

    # 14: the mouse in the text.  ED14.TXT is "alpha beta gamma", "second
    #     line here", "third".  A click before "beta" and an X typed; a
    #     double click on "second" selects it and "first" replaces it; a
    #     drag across "thi" and Del; a click at the start of line 1 and
    #     a Shift+click after "alpha", and Z replaces the selection.  Then
    #     File, Save and File, Exit, all clicked.
    #     -> "Z Xbeta gamma" CR LF "first line here" CR LF "rd"
    ED14 => '{DUMP}{CLICK 2,7}X{DCLICK 3,3}first{DUMP}'
          . '{PRESS 4,1}{DRAG 4,4}{DUMP}{RELEASE 4,4}{DEL}'
          . '{CLICK 2,1}{SHIFT}{CLICK 2,6}{NOSHIFT}Z{DUMP}'
          . '{CLICK 0,3}{DUMP}{CLICK 4,3}{CLICK 0,3}{CLICK 10,3}',

    # 15: the scroll bars, a dialog and help, by mouse.  ED15.TXT is
    #     "line 1" to "line 40".  The down arrow (a line), the track below
    #     the box (a page), the box dragged near the top; the right arrow
    #     and the left one.  Search, Find: "line 25" typed, Match
    #     Upper/Lowercase clicked, OK clicked - the cursor goes to it.
    #     Then help from the status bar, and out.
    ED15 => '{DUMP}{CLICK 22,79}{DUMP}{CLICK 15,79}{DUMP}'
          . '{PRESS 12,79}{DRAG 4,79}{RELEASE 4,79}{DUMP}{CLICK 23,78}{DUMP}{CLICK 23,1}'
          . '{CLICK 0,15}{CLICK 2,16}line 25{CLICK 12,14}{DUMP}{CLICK 14,25}{DUMP}'
          . '{CLICK 24,18}{DUMP}{ESC}{CLICK 0,3}{CLICK 10,3}',

    # 16: two windows by mouse.  Ctrl+F6, a click in the top window and
    #     "top" typed there; the lower window's title bar dragged up; View,
    #     Size Window chosen by dragging down the menu, and the line put
    #     where a click says.  Then File, Exit, and the question about the
    #     change answered No with a click.
    ED16 => '{CTRLF6}{CLICK 3,5}top{DUMP}{PRESS 12,40}{DRAG 8,40}{DUMP}{RELEASE 8,40}'
          . '{PRESS 0,23}{DRAG 3,24}{DUMP}{RELEASE 3,24}{CLICK 16,40}{DUMP}'
          . '{CLICK 0,3}{CLICK 10,3}{DUMP}{CLICK 14,37}',

    # 17: the Open dialog by mouse, in a directory holding A.TXT and
    #     B.TXT and nothing else.  A click on A.TXT in the list of files
    #     puts its name in the box; a double click on B.TXT opens it, as
    #     Enter would.
    ED17 => '{CLICK 0,3}{CLICK 3,3}{CLICK 9,10}{DUMP}{DCLICK 10,10}{DUMP}{CLICK 0,3}{CLICK 10,3}',
);

for my $name (sort keys %scripts) {
    open(my $fh, '>:raw', "$out/$name.KEY") or die "$out/$name.KEY: $!";
    print $fh render($scripts{$name});
    close $fh;
}
print scalar(keys %scripts), " key files in $out\n";
