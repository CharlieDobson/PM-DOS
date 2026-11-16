#!/usr/bin/perl
# mkpwkeys.pl SCRIPTS OUTDIR - key files for PMWRITE's test mode.
#
# PMWRITE (and PWDEMO) is tested the way EDIT and DOSSHELL are (see
# TOOLS\MKEDKEYS.PL and MKDSKEYS.PL): with PWTEST naming a log and
# standard input taken from a file, the keys come from the file, and
# the program writes its screen into the log wherever the script says
# {DUMP} and its picture as a .BMP beside the log wherever it says
# {SHOT}.  DISP.H lists the bytes; this writes them from a script.
#
# SCRIPTS is a text file.  A line "=NAME" starts a script, which goes to
# OUTDIR\NAME.KEY; the lines after it are the script, joined together
# (line ends and the blanks that begin a line count for nothing); a
# line that starts with # is a comment.  In a script:
#
#   {NAME}  {NAME*n}      a key, or that key n times: ENTER ESC BS TAB
#                         SHTAB UP DOWN LEFT RIGHT HOME END PGUP PGDN
#                         INS DEL SPACE F1 to F12
#   {SHIFT+NAME} {CTRL+NAME} {ALT+NAME}
#                         that key held with Shift, Ctrl or Alt - the
#                         function keys, the arrows and their
#                         neighbours, TAB, ENTER, BS, SPACE
#   {CTRL+X}              Ctrl and a letter: {CTRL+I} is not {TAB}
#   {CTRL+SHIFT+X}        Ctrl, Shift and a letter
#   {ALT+X}               Alt and a letter or a digit
#   {ALT}                 a tap of Alt
#   {SHIFT} ... {NOSHIFT} Shift held down for what is between
#   {KEY aa,ss}           a key given whole: character and scan code,
#                         in hexadecimal
#   {DUMP}  {SHOT}        the cells into the log; the picture to a file
#   {LBRACE}              a { typed
#   anything else         typed as it stands
#
# THE MOUSE, by cell (row,col from 0) or - with a P in front - by pixel
# (x,y) on a graphics screen:
#
#   {PRESS r,c}   {PPRESS x,y}      the left button goes down there
#   {DRAG r,c}    {PDRAG x,y}       the pointer moves with it held
#   {RELEASE r,c} {PRELEASE x,y}    it comes up there
#   {MOVE r,c}    {PMOVE x,y}       the pointer moves, nothing held
#   {CLICK r,c}   {PCLICK x,y}      a press and a release
#   {DCLICK r,c}  {PDCLICK x,y}     two of them: a double click
#   {RCLICK r,c}  {PRCLICK x,y}     the right button's press and release
#   {WHEEL n}                       the wheel, n notches (negative: away)
#
# A script ends when its keys run out, which leaves the program without
# saving anything.
use strict; use warnings;

my ($in, $out) = @ARGV;
die "usage: mkpwkeys.pl SCRIPTS OUTDIR\n" unless defined $out;

my %key = (
    ENTER => "\r", ESC => "\x1B", BS => "\x08", TAB => "\x09", SHTAB => "\0\x0F",
    UP => "\0\x48", DOWN => "\0\x50", LEFT => "\0\x4B", RIGHT => "\0\x4D",
    HOME => "\0\x47", END => "\0\x4F", PGUP => "\0\x49", PGDN => "\0\x51",
    INS => "\0\x52", DEL => "\0\x53", SPACE => " ", LBRACE => "{",
    DUMP => "\xFF\xFF", SHOT => "\xFF\xFE",
);
$key{"F$_"} = "\0" . chr(0x3A + $_) for 1 .. 10;
$key{F11} = "\0\x85"; $key{F12} = "\0\x86";

# the scan codes of the keys that have other ones when a shift key is held
my %shifted = ( SHIFT => {}, CTRL => {}, ALT => {} );
for my $n (1 .. 10) {
    $shifted{SHIFT}{"F$n"} = "\0" . chr(0x53 + $n);
    $shifted{CTRL}{"F$n"}  = "\0" . chr(0x5D + $n);
    $shifted{ALT}{"F$n"}   = "\0" . chr(0x67 + $n);
}
@{$shifted{SHIFT}}{qw(F11 F12)} = ("\0\x87", "\0\x88");
@{$shifted{CTRL}}{qw(F11 F12)}  = ("\0\x89", "\0\x8A");
@{$shifted{ALT}}{qw(F11 F12)}   = ("\0\x8B", "\0\x8C");
my %ctrl_nav = ( LEFT => 0x73, RIGHT => 0x74, END => 0x75, PGDN => 0x76, HOME => 0x77,
                 PGUP => 0x84, UP => 0x8D, DOWN => 0x91, INS => 0x92, DEL => 0x93, TAB => 0x94 );
my %alt_nav  = ( HOME => 0x97, UP => 0x98, PGUP => 0x99, LEFT => 0x9B, RIGHT => 0x9D,
                 END => 0x9F, DOWN => 0xA0, PGDN => 0xA1, INS => 0xA2, DEL => 0xA3, TAB => 0xA5,
                 BS => 0x0E, ENTER => 0x1C );
$shifted{CTRL}{$_} = "\0" . chr($ctrl_nav{$_}) for keys %ctrl_nav;
$shifted{ALT}{$_}  = "\0" . chr($alt_nav{$_})  for keys %alt_nav;
$shifted{CTRL}{ENTER} = "\xFF\xFC\x0A\x1C";       # a line feed, from the Enter key
$shifted{CTRL}{BS}    = "\xFF\xFC\x7F\x0E";
$shifted{SHIFT}{TAB}  = "\0\x0F";

my %altscan = (
    Q=>0x10, W=>0x11, E=>0x12, R=>0x13, T=>0x14, Y=>0x15, U=>0x16, I=>0x17, O=>0x18,
    P=>0x19, A=>0x1E, S=>0x1F, D=>0x20, F=>0x21, G=>0x22, H=>0x23, J=>0x24, K=>0x25,
    L=>0x26, Z=>0x2C, X=>0x2D, C=>0x2E, V=>0x2F, B=>0x30, N=>0x31, M=>0x32,
    1=>0x78, 2=>0x79, 3=>0x7A, 4=>0x7B, 5=>0x7C, 6=>0x7D, 7=>0x7E, 8=>0x7F, 9=>0x80, 0=>0x81,
);
my %bit = ( SHIFT => 2, CTRL => 4, ALT => 8 );

my $held = 0;           # the shift state the file has set

sub state { return "\xFF" . chr($_[0]) }

# a key with shift keys held for it alone
sub with {
    my ($bits, $bytes) = @_;
    return state($held | $bits) . $bytes . state($held);
}

sub cell_mouse  { my ($b, $r, $c) = @_; return "\xFE" . chr($b) . chr($r) . chr($c) }
sub pixel_mouse { my ($b, $x, $y) = @_; return "\xFF\xFD" . chr($b) . pack('v', $x) . pack('v', $y) }

sub mouse {
    my ($what, $one, $two) = @_;
    my $pixel = ($what =~ s/^P(?=PRESS|DRAG|RELEASE|MOVE|CLICK|DCLICK|RCLICK)//);
    my $at = $pixel ? sub { pixel_mouse($_[0], $one, $two) } : sub { cell_mouse($_[0], $one, $two) };
    return $at->(1) if $what eq 'PRESS' || $what eq 'DRAG';
    return $at->(0) if $what eq 'RELEASE' || $what eq 'MOVE';
    return $at->(1) . $at->(0) if $what eq 'CLICK';
    return ($at->(1) . $at->(0)) x 2 if $what eq 'DCLICK';
    return $at->(2) . $at->(0) if $what eq 'RCLICK';
    die "unknown mouse word $what\n";
}

sub combo {
    my ($mods, $name) = @_;
    my @mods = split /\+/, $mods;
    my $bits = 0;
    $bits |= ($bit{$_} // die "unknown shift key $_\n") for @mods;
    if (length($name) == 1) {
        my $ch = uc $name;
        if ($bits & 4) {                        # Ctrl and a letter, with Shift or not
            die "no Ctrl key for $name\n" unless $ch =~ /^[A-Z]$/;
            return with($bits, chr(ord($ch) & 0x1F));
        }
        if ($bits == 8) {
            die "no Alt scan code for $name\n" unless exists $altscan{$ch};
            return "\0" . chr($altscan{$ch});
        }
        die "{$mods+$name}: only Ctrl and Alt go with a character\n";
    }
    die "unknown key {$name}\n" unless exists $key{$name};
    return with($bits, ' ') if $name eq 'SPACE';
    # the key's own scan code for the first shift key the BIOS has one
    # for, and the others held round it
    for my $mod (qw(CTRL ALT SHIFT)) {
        next unless ($bits & $bit{$mod}) && exists $shifted{$mod}{$name};
        my $rest = $bits & ~$bit{$mod};
        return $rest ? with($rest, $shifted{$mod}{$name}) : $shifted{$mod}{$name};
    }
    return with($bits, $key{$name});
}

sub render {
    my ($script) = @_;
    my $bytes = '';
    $held = 0;
    while (length $script) {
        if ($script =~ s/^\{(P?(?:PRESS|RELEASE|DRAG|MOVE|CLICK|DCLICK|RCLICK)) (\d+),(\d+)\}//) {
            $bytes .= mouse($1, $2, $3);
        } elsif ($script =~ s/^\{WHEEL (-?\d+)\}//) {
            $bytes .= "\xFF\xFB" . chr($1 & 0xFF);
        } elsif ($script =~ s/^\{KEY ([0-9A-Fa-f]+),([0-9A-Fa-f]+)\}//) {
            $bytes .= "\xFF\xFC" . chr(hex $1) . chr(hex $2);
        } elsif ($script =~ s/^\{SHIFT\}//) {
            $held |= 2; $bytes .= state($held);
        } elsif ($script =~ s/^\{NOSHIFT\}//) {
            $held &= ~2; $bytes .= state($held);
        } elsif ($script =~ s/^\{ALT\}//) {
            $bytes .= state(8) . state(0); $held = 0;
        } elsif ($script =~ s/^\{((?:(?:SHIFT|CTRL|ALT)\+)+)([^}]+)\}//) {
            my ($mods, $name) = ($1, $2);
            $mods =~ s/\+$//;
            $bytes .= combo($mods, $name);
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

open(my $fh, '<', $in) or die "$in: $!";
my (%scripts, $name);
while (my $line = <$fh>) {
    $line =~ s/[\r\n]+$//;
    next if $line =~ /^#/;
    if ($line =~ /^=(\w+)\s*$/) { $name = uc $1; $scripts{$name} = ''; next; }
    next unless defined $name;
    $line =~ s/^\s+//;
    $scripts{$name} .= $line;
}
close $fh;
for my $script (sort keys %scripts) {
    open(my $kf, '>:raw', "$out/$script.KEY") or die "$out/$script.KEY: $!";
    print $kf render($scripts{$script});
    close $kf;
}
print scalar(keys %scripts), " key files in $out\n";
