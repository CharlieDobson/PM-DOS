#!/usr/bin/perl
# mkkeys.pl OUT.KEY  - the keystrokes DISK\KEYTEST.BAT types into the
# command-line editor, as the bytes 21h/07h hands back for them.
#
# WHY A FILE.  The editor only exists at a prompt with a keyboard behind
# it, and nothing on this machine can type at one unattended: DOSBox-X's
# AUTOTYPE crashes DOSBox-X itself (0xC0000409 on exit, with plain keys
# typed into its own shell and PM-DOS nowhere in sight), and the 86Box
# and DOSBox 0.74 rigs need the host's keyboard and screen.  But 07h
# reads a redirected standard input a byte at a time, and an extended
# key reaches a program as a zero and then its scan code - so a file
# holding "\0\x48" IS the Up arrow, as far as the editor can tell.  With
# KYTEST set the editor runs on such a file (see ky_readline), and
# "COMMAND < KEYTEST.KEY" is a session typed without a keyboard.
#
# The script below is what gets typed.  {NAME} is a key, {NAME*n} is
# that key n times, and everything else is typed as it stands.  Each
# test writes a file of its own, so that KEYTEST.BAT can read back what
# the editor actually produced - the comment says what it must hold.
use strict; use warnings;

my $out = shift or die "usage: mkkeys.pl OUT.KEY\n";

my %key = (
    ENTER => "\r",  ESC => "\x1B",  BS => "\x08",  CT => "\x14",
    UP    => "\0\x48", DOWN  => "\0\x50", LEFT  => "\0\x4B", RIGHT => "\0\x4D",
    HOME  => "\0\x47", END   => "\0\x4F", PGUP  => "\0\x49", PGDN  => "\0\x51",
    INS   => "\0\x52", DEL   => "\0\x53",
    F1 => "\0\x3B", F2 => "\0\x3C", F3 => "\0\x3D", F4 => "\0\x3E",
    F5 => "\0\x3F", F7 => "\0\x41", F8 => "\0\x42", F9 => "\0\x43",
    CLEFT => "\0\x73", CRIGHT => "\0\x74", CEND => "\0\x75", CHOME => "\0\x77",
    AF7 => "\0\x6E", AF10 => "\0\x71",
);

my @script = (
    # typing, and Up with Backspace at the end of the line  -> K1 K2 = one
    'ECHO one>K1.TXT{ENTER}',
    '{UP}{BS*5}2.TXT{ENTER}',
    # F3, Home, Right and overtyping                          -> K3 = two
    '{F3}{HOME}{RIGHT*5}two{END}{BS*5}3.TXT{ENTER}',
    # Ins: insert for one line                                -> K4 = new two
    '{UP}{HOME}{RIGHT*5}{INS}new {END}{BS*5}4.TXT{ENTER}',
    # Del                                                     -> K5 = two
    '{UP}{HOME}{RIGHT*5}{DEL*4}{END}{BS*5}5.TXT{ENTER}',
    # Esc throws the line away                                -> K6 = six
    'garbage{ESC}ECHO six>K6.TXT{ENTER}',
    # F2 copies the template up to a character, F3 the rest   -> K7 = six
    '{F2}KK7{F3}{ENTER}',
    # F4 deletes up to a character                            -> K8 = eight
    '{F1*5}{F4}>{INS}eight{F3}{END}{BS*5}8.TXT{ENTER}',
    # F5 keeps the line as the template, F3 brings it back    -> K9 = ninety
    # (longer than the line before, or F3 would bring back that line's
    # tail too: F5 keeps the template's whole length, as DOSKEY does)
    'ECHO ninety>K9.TXT{F5}{F3}{ENTER}',
    # PgUp: the oldest line                                   -> KA = one
    '{PGUP}{END}{BS*5}A.TXT{ENTER}',
    # F8: the newest line that starts with what is typed      -> KB = new two
    'ECHO new{F8}{END}{BS*5}B.TXT{ENTER}',
    # F9: a line by number, counted from the oldest           -> KC = two
    '{F9}3{ENTER}{END}{BS*5}C.TXT{ENTER}',
    # Up, Up, Down: back to the newer one                     -> KD = two
    '{UP}{UP}{DOWN}{END}{BS*5}D.TXT{ENTER}',
    # Ctrl+Right to a word, Del across it                     -> KE = fifteen
    'ECHO junk fifteen>KE.TXT{HOME}{CRIGHT}{DEL*5}{ENTER}',
    # Ctrl+Left to a word, Ctrl+End deletes to the end        -> KF = sixteen
    'ECHO sixteen>KF.TXT junk{CLEFT}{CEND}{ENTER}',
    # Ctrl+Home deletes to the start                          -> KG = seventeen
    'junk ECHO seventeen>KG.TXT{HOME}{RIGHT*5}{CHOME}{ENTER}',
    # Backspace in the middle of the line                     -> KH = eighteen
    'ECHO eighteenX>KH.TXT{LEFT*7}{BS}{ENTER}',
    # macros: $1 $2, $G                                       -> KI = alpha-beta
    'DOSKEY W=ECHO $1-$2$GKI.TXT{ENTER}',
    'W alpha beta{ENTER}',
    # $T: two commands from one macro                         -> KJ = first, KK = second
    'DOSKEY TWO=ECHO first$GKJ.TXT$TECHO second$GKK.TXT{ENTER}',
    'TWO{ENTER}',
    # $*: the whole of the rest                               -> KL = [a b  c]
    'DOSKEY ALL=ECHO [$*]$GKL.TXT{ENTER}',
    'ALL a b  c{ENTER}',
    # a leading blank is never a macro: KI keeps alpha-beta
    ' W x y{ENTER}',
    # Ctrl+T typed between two commands                       -> KM = t1, KN = t2
    'ECHO t1>KM.TXT{CT}ECHO t2>KN.TXT{ENTER}',
    # what DOSKEY remembers
    'DOSKEY /H>HIST.TXT{ENTER}',
    'DOSKEY /M>MACS.TXT{ENTER}',
    # F7 lists the history on the screen; the next line still works -> KO
    '{F7}ECHO after f7>KO.TXT{ENTER}',
    # Alt+F10 forgets the macros: KI keeps alpha-beta, MACS2 is empty
    '{AF10}W p q{ENTER}',
    'DOSKEY /M>MACS2.TXT{ENTER}',
    # Alt+F7 forgets the history: Up then finds nothing       -> KQ = q
    '{AF7}{UP}ECHO q>KQ.TXT{ENTER}',
    'DOSKEY /H>HIST2.TXT{ENTER}',
    'EXIT{ENTER}',
);

my $bytes = '';
for my $line (@script) {
    my $s = $line;
    while (length $s) {
        if ($s =~ s/^\{([A-Z0-9]+)(?:\*(\d+))?\}//) {
            my ($k, $n) = ($1, $2 // 1);
            die "no key called $k\n" unless exists $key{$k};
            $bytes .= $key{$k} x $n;
        } else {
            $s =~ s/^(.)//s;
            $bytes .= $1;
        }
    }
}
open(my $O, '>', $out) or die "$out: $!";
binmode $O;
print $O $bytes;
close $O;
printf "%s: %d bytes of keystrokes\n", $out, length $bytes;
