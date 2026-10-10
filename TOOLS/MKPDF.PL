#!/usr/bin/perl
# mkpdf.pl OUTDIR [JPEG] [G4 W H]  - test documents for ACROREAD, one for
# each way a PDF file can be put together.  Written by hand so that
# what each one tests is known:
#
#   V10.PDF      PDF 1.0: a table, nothing compressed, a standard font
#                with no widths, an inline image
#   A85.PDF      1.2: ASCII85 over Flate on the content, a Type 3 font
#   INCR.PDF     V10 saved again: a second table behind /Prev
#   XSTM.PDF     1.5: a cross-reference stream and an object stream
#   XSTMZ.PDF    ...both deflated, the table with a PNG predictor
#   BADXREF.PDF  V10 with its startxref pointing nowhere
#   BADXSTM.PDF  XSTMZ with its startxref pointing nowhere
#   RC4_40.PDF   encrypted, revision 2      RC4_128.PDF  revision 3
#   AES128.PDF   revision 4, AESV2          AES256.PDF   revision 6
#   AES256R5.PDF revision 5                 PWD.PDF      RC4-128, user password "secret"
#   HYBRID.PDF   1.5: a table whose trailer names a stream in /XRefStm
#   LZW.PDF      the content through LZW      RL.PDF  through RunLength
#   T1FONT.PDF   a Type 1 font program, made here and embedded
#   JPEG.PDF     a DCT image (if a JPEG file is given)
#   G4.PDF       a CCITT Group 4 image (if its data, width and height are)
use strict; use warnings;
use Digest::MD5 qw(md5);
use Digest::SHA qw(sha256 sha384 sha512);
use Compress::Zlib;

my ($outdir, $jpeg, $g4, $g4w, $g4h) = @ARGV;
die "usage: mkpdf.pl OUTDIR [JPEG] [G4DATA W H]\n" unless $outdir;

sub slurp { open my $fh, '<', $_[0] or die "$_[0]: $!"; binmode $fh; local $/; my $d = <$fh>; close $fh; $d }
sub spit  { open my $fh, '>', "$outdir/$_[0]" or die "$_[0]: $!"; binmode $fh; print $fh $_[1]; close $fh; printf "%-13s %7d\n", $_[0], length $_[1] }

#--- RC4 and AES --------------------------------------------------------
sub rc4 {
    my ($key, $data) = @_;
    my @k = unpack('C*', $key); my @s = (0..255); my $j = 0;
    for my $i (0..255) { $j = ($j + $s[$i] + $k[$i % @k]) & 255; @s[$i,$j] = @s[$j,$i] }
    my ($a, $b) = (0, 0); my $out = '';
    for my $c (unpack('C*', $data)) {
        $a = ($a + 1) & 255; $b = ($b + $s[$a]) & 255; @s[$a,$b] = @s[$b,$a];
        $out .= chr($c ^ $s[($s[$a] + $s[$b]) & 255]);
    }
    return $out;
}
my $tmp = "$outdir/_aes.tmp";
sub aes_cbc {                       # by openssl: this perl has no AES of its own
    my ($key, $iv, $data, $pad, $dec) = @_;
    open my $fh, '>', $tmp or die; binmode $fh; print $fh $data; close $fh;
    my $bits = length($key) * 8;
    my $cmd = sprintf('openssl enc -aes-%d-cbc %s %s -K %s -iv %s -in "%s"', $bits, $dec ? '-d' : '-e',
                      $pad ? '' : '-nopad', unpack('H*', $key), unpack('H*', $iv), $tmp);
    my $out = `$cmd`;
    die "openssl failed: $cmd\n" if $?;
    return $out;
}

#--- a document: objects, then a table of one kind or another -------------
my $PADDING = pack('H*', '28BF4E5E4E758A4164004E56FFFA01082E2E00B6D0683E802F0CA9FE6453697A');
my $ID = md5('acroread test id');

sub stream_obj { my ($dict, $data) = @_; return { dict => $dict, data => $data } }

sub content {
    my ($title) = @_;
    return "BT /F1 24 Tf 72 720 Td ($title) Tj ET\n" .
           "BT /F1 12 Tf 72 690 Td 14 TL (The quick brown fox jumps over the lazy dog.) Tj T* " .
           "[(Kerned) -200 (text) 300 (in) -100 (an array)] TJ T* (Parentheses \\(nested (ones)\\) and \\101\\102\\103 octal.) Tj ET\n" .
           "0.2 0.4 0.8 rg 72 600 150 50 re f\n" .
           "1 0 0 RG 3 w 250 600 m 300 660 350 590 400 650 c S\n" .
           "0 0.6 0 rg 0 0 0 RG 1 w 430 600 60 50 re B\n" .
           "q 0.5 g 72 540 m 122 540 l 97 580 l h f Q\n" .
           "[4 2] 0 d 72 520 m 500 520 l S [] 0 d\n";
}

# the objects every one of them has: 1 catalogue, 2 pages, 3 page, 4 font, 5 content
sub base_objects {
    my ($title, %opt) = @_;
    my $text = content($title) . ($opt{more} || '');
    my $font = $opt{font} || '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>';
    my $res = "<< /Font << /F1 4 0 R >> " . ($opt{res} || '') . ">>";
    my @objs = (undef,
        '<< /Type /Catalog /Pages 2 0 R >>',
        '<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 612 792] >>',
        "<< /Type /Page /Parent 2 0 R /Resources $res /Contents 5 0 R >>",
        $font,
        stream_obj($opt{cdict} || '', $opt{cdata} || $text));
    push @objs, @{$opt{extra}} if $opt{extra};
    return @objs;
}

sub obj_text {
    my ($num, $obj, $enc) = @_;
    if (ref $obj) {
        my $data = $obj->{data};
        $data = $enc->{stream}->($num, $data) if $enc && !$obj->{plain};
        return sprintf("%d 0 obj\n<< %s /Length %d >>\nstream\n", $num, $obj->{dict}, length $data) . $data . "\nendstream\nendobj\n";
    }
    $obj =~ s/\(([^()]*)\)/'<' . unpack('H*', $enc->{string}->($num, $1)) . '>'/ge if $enc && $obj !~ m{/Type /Font|/Encrypt};
    return "$num 0 obj\n$obj\nendobj\n";
}

# a file with a cross-reference table
sub with_table {
    my ($version, $objs, %opt) = @_;
    my $out = "%PDF-$version\n%\xE2\xE3\xCF\xD3\n";
    my @off;
    for my $num (1 .. $#$objs) {
        next unless defined $objs->[$num];
        $off[$num] = length $out;
        $out .= obj_text($num, $objs->[$num], $opt{enc} && $num != ($opt{encobj} || 0) ? $opt{enc} : undef);
    }
    my $xref = length $out;
    $out .= "xref\n0 " . scalar(@$objs) . "\n0000000000 65535 f \n";
    $out .= defined $off[$_] ? sprintf("%010d 00000 n \n", $off[$_]) : "0000000000 00000 f \n" for 1 .. $#$objs;
    $out .= "trailer\n<< /Size " . scalar(@$objs) . " /Root 1 0 R /ID [<" . unpack('H*', $ID) . "><" . unpack('H*', $ID) . ">]" .
            ($opt{encobj} ? " /Encrypt $opt{encobj} 0 R" : '') . ($opt{trailer} || '') . " >>\nstartxref\n" .
            (defined $opt{badstart} ? $opt{badstart} : $xref) . "\n%%EOF\n";
    return ($out, $xref, \@off);
}

# a file with a cross-reference stream, and objects 1 to 4 in an object stream
sub with_stream {
    my ($objs, %opt) = @_;
    my $out = "%PDF-1.5\n%\xE2\xE3\xCF\xD3\n";
    my (@type, @f2, @f3);
    my $ostm_num = scalar(@$objs);
    my $xref_num = $ostm_num + 1;
    my @packed = grep { !ref $objs->[$_] } 1 .. $#$objs;
    my ($head, $body) = ('', '');
    for my $i (0 .. $#packed) {
        $head .= "$packed[$i] " . length($body) . " ";
        $body .= $objs->[$packed[$i]] . "\n";
        ($type[$packed[$i]], $f2[$packed[$i]], $f3[$packed[$i]]) = (2, $ostm_num, $i);
    }
    my $odata = $head . $body;
    my $odict = "/Type /ObjStm /N " . scalar(@packed) . " /First " . length($head);
    if ($opt{deflate}) { $odata = compress($odata); $odict .= " /Filter /FlateDecode" }
    for my $num (grep { ref $objs->[$_] } 1 .. $#$objs) {
        ($type[$num], $f2[$num], $f3[$num]) = (1, length $out, 0);
        $out .= obj_text($num, $objs->[$num]);
    }
    ($type[$ostm_num], $f2[$ostm_num], $f3[$ostm_num]) = (1, length $out, 0);
    $out .= obj_text($ostm_num, stream_obj($odict, $odata));
    my $xref = length $out;
    ($type[$xref_num], $f2[$xref_num], $f3[$xref_num]) = (1, $xref, 0);
    my $rows = pack('CnC', 0, 0, 255);
    $rows .= pack('CnC', $type[$_], $f2[$_], $f3[$_]) for 1 .. $xref_num;
    my $xdict = "/Type /XRef /Size " . ($xref_num + 1) . " /W [1 2 1] /Root 1 0 R";
    if ($opt{deflate}) {
        # the PNG "up" predictor: each row less the one above
        my ($prev, $pred) = ("\0" x 4, '');
        for (my $at = 0; $at < length $rows; $at += 4) {
            my $row = substr($rows, $at, 4);
            $pred .= "\x02" . join('', map { chr((ord(substr($row, $_, 1)) - ord(substr($prev, $_, 1))) & 255) } 0 .. 3);
            $prev = $row;
        }
        $rows = compress($pred);
        $xdict .= " /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 4 >>";
    }
    $out .= obj_text($xref_num, stream_obj($xdict, $rows));
    $out .= "startxref\n" . (defined $opt{badstart} ? $opt{badstart} : $xref) . "\n%%EOF\n";
    return $out;
}

#--- the standard security handler ---------------------------------------
sub encryption {
    my ($rev, $bits, $aes, $user) = @_;
    my $n = $bits / 8;
    my $perm = -44;
    my $pbytes = pack('V', $perm & 0xFFFFFFFF);
    my (%enc, $dict, $key);
    if ($rev <= 4) {
        my $pad = substr($user . $PADDING, 0, 32);
        my $opad = substr('owner' . $PADDING, 0, 32);
        my $ok = md5($opad);
        if ($rev >= 3) { $ok = md5(substr($ok, 0, $n)) for 1 .. 50 }
        $ok = substr($ok, 0, $n);
        my $o = rc4($ok, $pad);
        if ($rev >= 3) { for my $i (1 .. 19) { $o = rc4(join('', map { chr(ord($_) ^ $i) } split //, $ok), $o) } }
        $key = md5($pad . $o . $pbytes . $ID);
        if ($rev >= 3) { $key = md5(substr($key, 0, $n)) for 1 .. 50 }
        $key = substr($key, 0, $n);
        my $u;
        if ($rev == 2) { $u = rc4($key, $PADDING) }
        else {
            $u = rc4($key, md5($PADDING . $ID));
            for my $i (1 .. 19) { $u = rc4(join('', map { chr(ord($_) ^ $i) } split //, $key), $u) }
            $u .= "\0" x 16;
        }
        my $v = $rev == 2 ? 1 : ($rev == 3 ? 2 : 4);
        $dict = "<< /Filter /Standard /V $v /R $rev /Length $bits /P $perm /O <" . unpack('H*', $o) . "> /U <" . unpack('H*', $u) . ">";
        $dict .= $aes ? " /CF << /StdCF << /CFM /AESV2 /AuthEvent /DocOpen /Length 16 >> >> /StmF /StdCF /StrF /StdCF"
                      : " /CF << /StdCF << /CFM /V2 /AuthEvent /DocOpen /Length 16 >> >> /StmF /StdCF /StrF /StdCF" if $rev == 4;
        $dict .= " >>";
        my $objkey = sub {
            my ($num) = @_;
            my $k = md5($key . substr(pack('V', $num), 0, 3) . "\0\0" . ($aes ? 'sAlT' : ''));
            return substr($k, 0, $n + 5 > 16 ? 16 : $n + 5);
        };
        my $crypt = sub {
            my ($num, $data) = @_;
            return rc4($objkey->($num), $data) unless $aes;
            my $iv = md5("iv $num " . length $data);
            return $iv . aes_cbc($objkey->($num), $iv, $data, 1, 0);
        };
        $enc{stream} = $enc{string} = $crypt;
    } else {
        # revisions 5 and 6: a random file key, wrapped by each password's hash
        $key = sha256('the file key of the test');
        my $hash = sub {
            my ($pw, $salt, $udata) = @_;
            my $k = sha256($pw . $salt . $udata);
            return $k if $rev == 5;
            for (my $round = 0; ; $round++) {
                my $k1 = ($pw . $k . $udata) x 64;
                my $e = aes_cbc(substr($k, 0, 16), substr($k, 16, 16), $k1, 0, 0);
                my $sum = 0; $sum += $_ for unpack('C16', $e);
                $k = $sum % 3 == 0 ? sha256($e) : ($sum % 3 == 1 ? sha384($e) : sha512($e));
                last if $round >= 63 && ord(substr($e, -1)) <= $round - 31;
            }
            return substr($k, 0, 32);
        };
        my ($uvs, $uks, $ovs, $oks) = ('uvalsalt', 'ukeysalt', 'ovalsalt', 'okeysalt');
        my $u = $hash->($user, $uvs, '') . $uvs . $uks;
        my $ue = aes_cbc($hash->($user, $uks, ''), "\0" x 16, $key, 0, 0);
        my $o = $hash->('owner', $ovs, $u) . $ovs . $oks;
        my $oe = aes_cbc($hash->('owner', $oks, $u), "\0" x 16, $key, 0, 0);
        $dict = "<< /Filter /Standard /V 5 /R $rev /Length 256 /P $perm /O <" . unpack('H*', $o) . "> /U <" . unpack('H*', $u) .
                "> /OE <" . unpack('H*', $oe) . "> /UE <" . unpack('H*', $ue) . "> /Perms <" . ('00' x 16) .
                "> /CF << /StdCF << /CFM /AESV3 /AuthEvent /DocOpen /Length 32 >> >> /StmF /StdCF /StrF /StdCF >>";
        my $crypt = sub {
            my ($num, $data) = @_;
            my $iv = md5("iv $num " . length $data);
            return $iv . aes_cbc($key, $iv, $data, 1, 0);
        };
        $enc{stream} = $enc{string} = $crypt;
    }
    return (\%enc, $dict);
}

sub encrypted {
    my ($name, $rev, $bits, $aes, $user) = @_;
    my ($enc, $dict) = encryption($rev, $bits, $aes, $user);
    my @objs = base_objects("$name: encrypted, revision $rev",
        font => '<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>',
        extra => ['<< /Title (A string that is encrypted too) >>', $dict]);
    my ($pdf) = with_table($rev >= 5 ? '1.7' : '1.4', \@objs, enc => $enc, encobj => 7);
    spit($name, $pdf);
}

#--- the files -------------------------------------------------------------
my $inline = "q 64 0 0 64 440 690 cm BI /W 8 /H 8 /BPC 8 /CS /G ID " .
             join('', map { my $r = $_; map { chr((($r + $_) & 1) ? 255 - $r * 20 : $_ * 30) } 0 .. 7 } 0 .. 7) . "\nEI Q\n";
my @v10 = base_objects('V10: PDF 1.0, a table', more => $inline);
my ($v10, $v10xref) = with_table('1.0', \@v10);
spit('V10.PDF', $v10);

# the same saved again: a new content stream, and a second table
{
    my $more = $v10;
    my $off = length $more;
    $more .= obj_text(5, stream_obj('', content('INCR: saved twice; this is the second')));
    my $xref = length $more;
    $more .= "xref\n5 1\n" . sprintf("%010d 00000 n \n", $off) . "trailer\n<< /Size 6 /Root 1 0 R /Prev $v10xref >>\nstartxref\n$xref\n%%EOF\n";
    spit('INCR.PDF', $more);
}
spit('BADXREF.PDF', (with_table('1.3', [base_objects('BADXREF: the table is not where it says')], badstart => 7))[0]);

# LZW would need an encoder; ASCII85 and ASCIIHex over deflate, and a Type 3 font
{
    my $text = content('A85: ASCII85 over Flate') .
               "BT /F2 30 Tf 72 450 Td (ABAB) Tj ET\n";
    my $z = compress($text);
    my $a85 = '';
    for (my $at = 0; $at < length $z; $at += 4) {
        my $chunk = substr($z, $at, 4); my $len = length $chunk;
        my $word = unpack('N', $chunk . ("\0" x (4 - $len)));
        my @d; for (1 .. 5) { unshift @d, $word % 85; $word = int($word / 85) }
        $a85 .= join('', map { chr($_ + 33) } @d[0 .. $len]);
    }
    $a85 .= '~>';
    my @objs = base_objects('', cdict => '/Filter [/ASCII85Decode /FlateDecode]', cdata => $a85,
        res => '/Font << /F1 4 0 R /F2 6 0 R >> ',
        extra => ['<< /Type /Font /Subtype /Type3 /FontBBox [0 0 1000 1000] /FontMatrix [0.001 0 0 0.001 0 0] ' .
                  '/CharProcs << /sq 7 0 R /tri 8 0 R >> /Encoding << /Type /Encoding /Differences [65 /sq /tri] >> ' .
                  '/FirstChar 65 /LastChar 66 /Widths [1000 1000] >>',
                  stream_obj('', "1000 0 0 0 800 800 d1 100 0 700 700 re f"),
                  stream_obj('/Filter /ASCIIHexDecode', unpack('H*', "1000 0 0 0 800 800 d1 100 0 m 800 0 l 450 700 l f") . '>')]);
    $objs[3] = '<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R /F2 6 0 R >> >> /Contents 5 0 R >>';
    spit('A85.PDF', (with_table('1.2', \@objs))[0]);
}

my @xs = base_objects('XSTM: a cross-reference stream');
spit('XSTM.PDF', with_stream(\@xs));
@xs = base_objects('XSTMZ: deflated, with a predictor');
spit('XSTMZ.PDF', with_stream(\@xs, deflate => 1));
@xs = base_objects('BADXSTM: found by reading the file');
spit('BADXSTM.PDF', with_stream(\@xs, deflate => 1, badstart => 99999));

encrypted('RC4_40.PDF', 2, 40, 0, '');
encrypted('RC4_128.PDF', 3, 128, 0, '');
encrypted('AES128.PDF', 4, 128, 1, '');
encrypted('AES256R5.PDF', 5, 256, 1, '');
encrypted('AES256.PDF', 6, 256, 1, '');
encrypted('PWD.PDF', 3, 128, 0, 'secret');

if ($jpeg && -f $jpeg) {
    my $data = slurp($jpeg);
    my ($w, $h, $n) = (0, 0, 3);
    for (my $at = 2; $at + 9 < length $data; ) {
        my ($ff, $mark, $len) = unpack('CCn', substr($data, $at, 4));
        last if $ff != 0xFF;
        if ($mark >= 0xC0 && $mark <= 0xC2) { ($h, $w, $n) = unpack('x5 n n C', substr($data, $at, 10)); last }
        $at += 2 + $len;
    }
    my @objs = base_objects("JPEG: a DCT image, $w by $h", more => "q 300 0 0 " . int(300 * $h / $w) . " 72 200 cm /Im1 Do Q\n",
        res => '/XObject << /Im1 6 0 R >> ',
        extra => [stream_obj("/Type /XObject /Subtype /Image /Width $w /Height $h /BitsPerComponent 8 /ColorSpace " .
                             ($n == 1 ? '/DeviceGray' : '/DeviceRGB') . " /Filter /DCTDecode", $data)]);
    spit('JPEG.PDF', (with_table('1.3', \@objs))[0]);
}
if ($g4 && -f $g4) {
    my $data = slurp($g4);
    my @objs = base_objects("G4: a CCITT Group 4 image, $g4w by $g4h",
        more => "q 300 0 0 " . int(300 * $g4h / $g4w) . " 72 150 cm /Im1 Do Q\n0.8 0 0 rg q 100 0 0 " . int(100 * $g4h / $g4w) . " 420 400 cm /Im2 Do Q\n",
        res => '/XObject << /Im1 6 0 R /Im2 7 0 R >> ',
        extra => [stream_obj("/Type /XObject /Subtype /Image /Width $g4w /Height $g4h /BitsPerComponent 1 /ColorSpace /DeviceGray " .
                             "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns $g4w /Rows $g4h >>", $data),
                  stream_obj("/Type /XObject /Subtype /Image /Width $g4w /Height $g4h /BitsPerComponent 1 /ImageMask true " .
                             "/Filter /CCITTFaxDecode /DecodeParms << /K -1 /Columns $g4w /Rows $g4h >>", $data)]);
    spit('G4.PDF', (with_table('1.3', \@objs))[0]);
}
#--- four more: a hybrid file, LZW, RunLength, an embedded Type 1 font ------

# HYBRID.PDF: a table for old readers, which says objects 1 to 4 are
# free, and a stream named by /XRefStm that says where they really are
{
    my @objs = base_objects('HYBRID: a table and a stream');
    my $out = "%PDF-1.5\n%\xE2\xE3\xCF\xD3\n";
    my ($head, $body) = ('', '');
    for my $num (1 .. 4) { $head .= "$num " . length($body) . " "; $body .= $objs[$num] . "\n" }
    my %off;
    $off{5} = length $out; $out .= obj_text(5, $objs[5]);
    $off{6} = length $out; $out .= obj_text(6, stream_obj("/Type /ObjStm /N 4 /First " . length($head), $head . $body));
    $off{7} = length $out;
    my $rows = pack('CnC', 0, 0, 255) . join('', map { pack('CnC', 2, 6, $_) } 0 .. 3) .
               join('', map { pack('CnC', 1, $off{$_}, 0) } 5 .. 7);
    $out .= obj_text(7, stream_obj("/Type /XRef /Size 8 /W [1 2 1] /Root 1 0 R", $rows));
    my $xref = length $out;
    $out .= "xref\n0 1\n0000000000 65535 f \n5 3\n" . join('', map { sprintf("%010d 00000 n \n", $off{$_}) } 5 .. 7);
    $out .= "trailer\n<< /Size 8 /Root 1 0 R /XRefStm $off{7} >>\nstartxref\n$xref\n%%EOF\n";
    spit('HYBRID.PDF', $out);
}

# LZW.PDF: the content through LZW as PDF writes it (the code grows a
# bit early), and RL.PDF: through RunLength
sub lzw {
    my ($data) = @_;
    my %table;
    my ($bits, $nbits, $out) = (0, 0, '');
    my ($width, $next) = (9, 258);
    my $emit = sub { $bits = ($bits << $width) | $_[0]; $nbits += $width;
                     while ($nbits >= 8) { $out .= chr(($bits >> ($nbits - 8)) & 255); $nbits -= 8 } $bits &= (1 << $nbits) - 1 };
    $emit->(256);
    my $cur = '';
    for my $ch (split //, $data) {
        if ($cur eq '' || exists $table{$cur . $ch}) { $cur .= $ch; next }
        $emit->(length($cur) == 1 ? ord($cur) : $table{$cur});
        $table{$cur . $ch} = $next++;
        $width++ if $next + 1 > (1 << $width) && $width < 12;
        if ($next >= 4094) { $emit->(256); %table = (); $width = 9; $next = 258 }
        $cur = $ch;
    }
    $emit->(length($cur) == 1 ? ord($cur) : $table{$cur}) if $cur ne '';
    $emit->(257);
    $out .= chr(($bits << (8 - $nbits)) & 255) if $nbits;
    return $out;
}
sub runlength {
    my ($data) = @_; my $out = '';
    while (length $data) {
        if ($data =~ /^((.)\2{2,127})/s) { $out .= chr(257 - length $1) . $2; substr($data, 0, length $1) = ''; next }
        my $take = 1;
        $take++ while $take < 128 && $take < length($data) && substr($data, $take, 3) !~ /^(.)\1\1/s;
        $out .= chr($take - 1) . substr($data, 0, $take); substr($data, 0, $take) = '';
    }
    return $out . chr(128);
}
{
    my $long = content('LZW: the content is LZW') . join('', map { "BT /F1 9 Tf 72 " . (480 - $_ * 10) . " Td (Line $_ of forty, to give the table something to learn from.) Tj ET\n" } 1 .. 40);
    spit('LZW.PDF', (with_table('1.2', [base_objects('', cdict => '/Filter /LZWDecode', cdata => lzw($long))]))[0]);
    my $runs = content('RL: the content is RunLength') . "0.9 g 72 300 468 100 re f" . (' ' x 300) . "\n0 g BT /F1 12 Tf 80 340 Td (after three hundred spaces) Tj ET\n";
    spit('RL.PDF', (with_table('1.2', [base_objects('', cdict => '/Filter /RunLengthDecode', cdata => runlength($runs))]))[0]);
}

# T1FONT.PDF: a Type 1 font program made here - two glyphs, a
# subroutine, a division - enciphered as the format says and embedded
sub t1_crypt {
    my ($plain, $key) = @_; my $out = '';
    for my $p (unpack('C*', $plain)) { my $c = $p ^ ($key >> 8); $out .= chr($c); $key = (($c + $key) * 52845 + 22719) & 0xFFFF }
    return $out;
}
sub t1_num { my ($v) = @_; return chr($v + 139) if $v >= -107 && $v <= 107;
             return chr((($v - 108) >> 8) + 247) . chr(($v - 108) & 255) if $v >= 108 && $v <= 1131;
             return chr(((-$v - 108) >> 8) + 251) . chr((-$v - 108) & 255) if $v <= -108 && $v >= -1131;
             return chr(255) . pack('N', $v & 0xFFFFFFFF) }
sub t1_cs { my $out = "\0\0\0\0"; for my $tok (@_) { $out .= $tok =~ /^-?\d+$/ ? t1_num($tok) : { hsbw => "\x0D", rmoveto => "\x15", rlineto => "\x05",
            closepath => "\x09", endchar => "\x0E", callsubr => "\x0A", return => "\x0B", div => "\x0C\x0C", rrcurveto => "\x08", hlineto => "\x06", vlineto => "\x07" }->{$tok} }
            return t1_crypt($out, 4330) }
{
    my $subr = t1_cs(qw(0 0 rmoveto 1000 2 div 0 rlineto 0 500 rlineto -500 0 rlineto closepath return));
    my $notdef = t1_cs(qw(0 500 hsbw endchar));
    my $glyph_a = t1_cs(qw(100 700 hsbw 0 0 rmoveto 500 0 rlineto -250 700 rlineto closepath endchar));
    my $glyph_b = t1_cs(qw(50 600 hsbw 0 callsubr 150 150 rmoveto 200 hlineto 200 vlineto -200 hlineto closepath
                           100000 1000 div 600 rmoveto 100 0 0 100 -100 0 rrcurveto closepath endchar));
    my $clear = "%!PS-AdobeFont-1.0: AcroTest 001.000\n11 dict begin\n/FontInfo 1 dict dup begin /FullName (AcroTest) readonly def end readonly def\n" .
                "/FontName /AcroTest def\n/Encoding 256 array\n0 1 255 {1 index exch /.notdef put} for\ndup 65 /A put\ndup 66 /B put\nreadonly def\n" .
                "/PaintType 0 def\n/FontType 1 def\n/FontMatrix [0.001 0 0 0.001 0 0] readonly def\n/FontBBox {0 0 1000 1000} readonly def\n" .
                "currentdict end\ncurrentfile eexec\n";
    my $private = "dup /Private 8 dict dup begin\n/RD {string currentfile exch readstring pop} executeonly def\n/ND {noaccess def} executeonly def\n" .
                  "/NP {noaccess put} executeonly def\n/BlueValues [] def\n/lenIV 4 def\n/Subrs 1 array\ndup 0 " . length($subr) . " RD $subr NP\nND\n" .
                  "2 index /CharStrings 3 dict dup begin\n/.notdef " . length($notdef) . " RD $notdef ND\n/A " . length($glyph_a) . " RD $glyph_a ND\n" .
                  "/B " . length($glyph_b) . " RD $glyph_b ND\nend\nend\nreadonly put\nnoaccess put\ndup /FontName get exch definefont pop\nmark currentfile closefile\n";
    my $cipher = t1_crypt("AcRo" . $private, 55665);
    my $tail = (("0" x 64 . "\n") x 8) . "cleartomark\n";
    my @objs = base_objects('T1FONT: an embedded Type 1 font', more => "0 0.3 0.7 rg BT /F2 60 Tf 72 420 Td (ABBA) Tj ET\n",
        extra => ['<< /Type /Font /Subtype /Type1 /BaseFont /AcroTest /FirstChar 65 /LastChar 66 /Widths [700 600] /FontDescriptor 7 0 R >>',
                  '<< /Type /FontDescriptor /FontName /AcroTest /Flags 4 /FontBBox [0 0 1000 1000] /ItalicAngle 0 /Ascent 800 /Descent 0 ' .
                  '/CapHeight 700 /StemV 80 /FontFile 8 0 R >>',
                  stream_obj("/Length1 " . length($clear) . " /Length2 " . length($cipher) . " /Length3 " . length($tail), $clear . $cipher . $tail)]);
    $objs[3] = '<< /Type /Page /Parent 2 0 R /Resources << /Font << /F1 4 0 R /F2 6 0 R >> >> /Contents 5 0 R >>';
    spit('T1FONT.PDF', (with_table('1.3', \@objs))[0]);
}

unlink $tmp;
