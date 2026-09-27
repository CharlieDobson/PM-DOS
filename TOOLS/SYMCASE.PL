#!/usr/bin/perl
# SYMCASE.PL - find names wasm will quietly treat as ONE symbol.
#
# wasm folds case.  A table called for_saves and an equate called
# FOR_SAVES are the same name to it, and it does not always say so: the
# 16-bit COMMAND.COM assembled [for_saves+di] as [di+10h] - the equate's
# value, 16 - and wrote every FOR it parked into its own PSP.  Nothing
# failed to build.  The saved INT 23h vector came back wrong when the
# shell ended, and that is how it was found (NOTES.TXT, v0.60).
#
# For each .ASM named, this follows its INCLUDEs and reports every name
# defined both as an EQU and as a label or data item, ignoring case.
#
#     perl TOOLS\SYMCASE.PL CMD\COMMAND\COMMAND.ASM DOS\DOSMAIN.ASM ...
#
# No output means no clash.
use strict;
my %seen;
sub unit {
    my ($file, $dir, $eq, $lab) = @_;
    open my $f, '<', $file or return;
    while (<$f>) {
        s/;.*//;
        if (/^\s*INCLUDE\s+(\S+)/i) {
            my $inc = $1;
            for my $d ($dir, "INC", "../INC", "../../INC", "DOS", "IO", "CMD") {
                my $p = "$d/$inc";
                if (-f $p) { unit($p, $d, $eq, $lab); last; }
            }
            next;
        }
        if (/^\s*([A-Za-z_@\$\?][\w@\$\?]*)\s+EQU\b/i) { push @{$eq->{lc $1}}, "$file:$."; next; }
        if (/^\s*([A-Za-z_@\$\?][\w@\$\?]*)\s*:/) { push @{$lab->{lc $1}}, "$file:$."; next; }
        if (/^\s*([A-Za-z_@\$\?][\w@\$\?]*)\s+(DB|DW|DD|DQ|DF|DT|LABEL)\b/i) { push @{$lab->{lc $1}}, "$file:$."; next; }
    }
}
for my $top (@ARGV) {
    my (%eq, %lab);
    my $dir = $top; $dir =~ s{/[^/]*$}{}; $dir = "." if $dir eq $top;
    unit($top, $dir, \%eq, \%lab);
    for my $k (sort keys %eq) {
        next unless $lab{$k};
        print "$top: $k  EQU @{$eq{$k}}  LABEL @{$lab{$k}}\n";
    }
}
