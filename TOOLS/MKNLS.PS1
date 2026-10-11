param(
    [string]$Dir = '',                 # where the .DEF files are and the .ASM files go
    [switch]$Report                    # say what each code page could not show
)
# MKNLS.PS1 - NLS\COUNTRY.DEF and NLS\KEYBOARD.DEF into NLS\COUNTRY.ASM
# and NLS\KEYBOARD.ASM, which BUILD.CMD assembles into COUNTRY.SYS and
# KEYBOARD.SYS.
#
# NOT PART OF THE BUILD.  BUILD.CMD needs Open Watcom and nothing else,
# so the two .ASM files are kept in the tree as this wrote them; run
# this again after editing a .DEF file.  It is here rather than in C
# because what it needs is what the system already knows: which
# character each byte of a code page is, what a letter's capital is,
# and what an accent and a letter make together.
#
# COUNTRY.SYS is in MS-DOS's format, so the kernel reads either file:
#
#   00h   FFh,'COUNTRY', eight zeros, dw 1, db 1, dd 17h
#   17h   dw entries, then for each: dw 12, dw country, dw code page,
#         dw 0, dw 0, dd where its table of tables is
#   a table of tables: dw how many, then for each: dw 6, dw which,
#         dd where the data is
#   data: FFh, seven letters, dw length, the bytes
#
#   which = 1 the country block (country, code page, 38h's 34 bytes),
#   2 upper case for 80h-FFh, 4 the same for file names, 5 what ends a
#   file name, 6 the collating weights, 7 the double-byte ranges, and
#   35 the letters for yes and no.
#
# THE TABLES ARE MADE BY RULE, and are not MS-DOS's byte for byte:
#
#   upper case   a small letter becomes its capital when the code page
#                has the capital, and the unaccented capital when it
#                does not; everything else is itself.
#   collating    a letter weighs what its unaccented capital does, a
#                currency sign what '$' does, and the letters a
#                country puts after Z ('sort' in COUNTRY.DEF) come
#                after Z.
#
# KEYBOARD.SYS is PM-DOS's own format:
#
#   00h   'PMKEYB',1Ah,1
#   08h   dw layouts, then for each: two letters, dw keyboard ID,
#         dw code page, dw length, dd where the record is
#   a record is what KEYB hands the kernel as it stands (KBL_ in
#   PMDOS.INC): a header, six bytes a key, and the dead keys.
$ErrorActionPreference = 'Stop'
if ( $Dir -eq '' ) { $Dir = Join-Path ( Split-Path $PSScriptRoot -Parent ) 'NLS' }
try { [System.Text.Encoding]::RegisterProvider( [System.Text.CodePagesEncodingProvider]::Instance ) } catch { }

#---------------------------------------------------------------------
# code pages: byte -> character and back
#---------------------------------------------------------------------
$cpcache = @{}
function Get-CodePage( [int]$id ) {
    if ( $cpcache.ContainsKey( $id ) ) { return $cpcache[$id] }
    $enc = [System.Text.Encoding]::GetEncoding( $id )
    $all = New-Object byte[] 256
    for ( $i = 0; $i -lt 256; $i++ ) { $all[$i] = $i }
    $str = $enc.GetString( $all )
    $uni = New-Object int[] 256
    $rev = @{}
    for ( $i = 0; $i -lt 256; $i++ ) {
        $uni[$i] = [int][char]$str[$i]
        if ( $i -ge 0x20 -and $i -ne 0x7F -and -not $rev.ContainsKey( $uni[$i] ) ) { $rev[$uni[$i]] = $i }
    }
    # the section and paragraph signs are the glyphs every PC font has
    # at 15h and 14h, in a page that has them nowhere else
    if ( -not $rev.ContainsKey( 0xA7 ) ) { $rev[0xA7] = 0x15 }
    if ( -not $rev.ContainsKey( 0xB6 ) ) { $rev[0xB6] = 0x14 }
    $cp = @{ id = $id; uni = $uni; rev = $rev
             greek = ( $id -eq 737 -or $id -eq 869 ); cyr = ( $id -eq 855 -or $id -eq 866 ) }
    $cpcache[$id] = $cp
    return $cp
}

# a letter without its accent, as an ASCII capital, or 0
$plainbase = @{ 0xE6 = 'A'; 0xC6 = 'A'; 0xF8 = 'O'; 0xD8 = 'O'; 0xDF = 'S'; 0xF0 = 'D'; 0xD0 = 'D'
                0x111 = 'D'; 0x110 = 'D'; 0x142 = 'L'; 0x141 = 'L'; 0x131 = 'I'; 0x153 = 'O'; 0x152 = 'O' }
function Get-Base( [int]$u ) {
    if ( $plainbase.ContainsKey( $u ) ) { return [int][char]$plainbase[$u] }
    $s = ( [string][char]$u ).Normalize( [System.Text.NormalizationForm]::FormD )
    $c = [int][char]$s[0]
    if ( $s.Length -gt 1 -and ( ( $c -ge 0x41 -and $c -le 0x5A ) -or ( $c -ge 0x61 -and $c -le 0x7A ) ) ) {
        return ( $c -band 0xDF )
    }
    return 0
}
function Test-Script( $cp, [int]$u ) {  # is this letter one the page is an alphabet for
    if ( $u -ge 0x370 -and $u -le 0x3FF ) { return $cp.greek }
    if ( $u -ge 0x400 -and $u -le 0x4FF ) { return $cp.cyr }
    return $true
}
function Get-Upper( $cp, [int]$b ) {    # the byte's capital in this page
    $u = $cp.uni[$b]
    if ( $b -lt 0x80 ) { if ( $b -ge 0x61 -and $b -le 0x7A ) { return ( $b - 0x20 ) } else { return $b } }
    $ch = [char]$u
    if ( -not [char]::IsLetter( $ch ) -or -not ( Test-Script $cp $u ) ) { return $b }
    $up = [int][char]::ToUpperInvariant( $ch )
    if ( $up -eq $u ) { return $b }
    if ( $cp.rev.ContainsKey( $up ) ) { return $cp.rev[$up] }
    $base = Get-Base $u
    if ( $base -ne 0 ) { return $base }
    return $b
}

$sorts = @{
    '-'  = @{}
    'dk' = @{ 0xC6 = 0x5B; 0xE6 = 0x5B; 0xD8 = 0x5C; 0xF8 = 0x5C; 0xC5 = 0x5D; 0xE5 = 0x5D; 0xDC = 0x59; 0xFC = 0x59 }
    'sv' = @{ 0xC5 = 0x5B; 0xE5 = 0x5B; 0xC4 = 0x5C; 0xE4 = 0x5C; 0xD6 = 0x5D; 0xF6 = 0x5D; 0xDC = 0x59; 0xFC = 0x59 }
}
function Get-Weight( $cp, [int]$b, $sort ) {
    if ( $b -lt 0x80 ) { if ( $b -ge 0x61 -and $b -le 0x7A ) { return ( $b - 0x20 ) } else { return $b } }
    $u = $cp.uni[$b]
    if ( $sort.ContainsKey( $u ) ) { return $sort[$u] }
    $ch = [char]$u
    if ( [char]::IsLetter( $ch ) ) {
        $base = Get-Base $u
        if ( $base -ne 0 ) { return $base }
        if ( Test-Script $cp $u ) {
            $s = ( [string]$ch ).Normalize( [System.Text.NormalizationForm]::FormD )
            $up = [int][char]::ToUpperInvariant( $s[0] )
            if ( $cp.rev.ContainsKey( $up ) ) { return $cp.rev[$up] }
        }
        if ( $u -eq 0x192 ) { return 0x24 }             # the florin sign
        return $b
    }
    if ( [char]::GetUnicodeCategory( $ch ) -eq [System.Globalization.UnicodeCategory]::CurrencySymbol ) { return 0x24 }
    if ( $u -eq 0xBF ) { return 0x3F }
    if ( $u -eq 0xA1 ) { return 0x21 }
    if ( $u -eq 0xAB -or $u -eq 0xBB ) { return 0x22 }
    return $b
}
function ConvertTo-Bytes( $cp, [string]$text ) {       # $null if the page cannot spell it
    $out = @()
    foreach ( $ch in $text.ToCharArray() ) {
        $u = [int]$ch
        if ( $u -lt 0x80 ) { $out += $u }
        elseif ( $cp.rev.ContainsKey( $u ) ) { $out += $cp.rev[$u] }
        else { return $null }
    }
    return , $out
}

#---------------------------------------------------------------------
# a growing image, and the .ASM it is written out as
#---------------------------------------------------------------------
function New-Image { return , ( New-Object System.Collections.Generic.List[byte] ) }
function Add-B( $img, [int[]]$v ) { foreach ( $x in $v ) { $img.Add( [byte]( $x -band 0xFF ) ) } }
function Add-W( $img, [int]$v ) { $img.Add( [byte]( $v -band 0xFF ) ); $img.Add( [byte]( ( $v -shr 8 ) -band 0xFF ) ) }
function Add-D( $img, [long]$v ) { Add-W $img ( $v -band 0xFFFF ); Add-W $img ( ( $v -shr 16 ) -band 0xFFFF ) }
function Set-W( $img, [int]$at, [int]$v ) { $img[$at] = [byte]( $v -band 0xFF ); $img[$at + 1] = [byte]( ( $v -shr 8 ) -band 0xFF ) }
function Set-D( $img, [int]$at, [long]$v ) { Set-W $img $at ( $v -band 0xFFFF ); Set-W $img ( $at + 2 ) ( ( $v -shr 16 ) -band 0xFFFF ) }
function Write-Asm( $img, [string]$path, [string]$what, [string]$from ) {
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine( ';=====================================================================' )
    [void]$sb.AppendLine( "; $what" )
    [void]$sb.AppendLine( ';' )
    [void]$sb.AppendLine( "; WRITTEN BY TOOLS\MKNLS.PS1 FROM NLS\$from - change that file and" )
    [void]$sb.AppendLine( '; run the script again; an edit made here is lost the next time.' )
    [void]$sb.AppendLine( ';' )
    [void]$sb.AppendLine( '; Data only.  Linked with:  wlink form raw bin option offset=0x0' )
    [void]$sb.AppendLine( ';=====================================================================' )
    [void]$sb.AppendLine( '_NLS    SEGMENT USE16 PUBLIC ''DATA''' )
    for ( $i = 0; $i -lt $img.Count; $i += 20 ) {
        $n = [Math]::Min( 20, $img.Count - $i )
        $part = for ( $k = 0; $k -lt $n; $k++ ) { [string]$img[$i + $k] }
        [void]$sb.AppendLine( '        db      ' + ( $part -join ',' ) )
    }
    [void]$sb.AppendLine( '_NLS    ENDS' )
    [void]$sb.AppendLine( '        END' )
    [System.IO.File]::WriteAllText( $path, $sb.ToString(), ( New-Object System.Text.ASCIIEncoding ) )
}
function Read-Def( [string]$path ) {
    return [System.IO.File]::ReadAllLines( $path, [System.Text.Encoding]::UTF8 )
}

#=====================================================================
# COUNTRY.SYS
#=====================================================================
$countries = @()
foreach ( $line in ( Read-Def ( Join-Path $Dir 'COUNTRY.DEF' ) ) ) {
    $t = $line.Trim()
    if ( $t -eq '' -or $t.StartsWith( ';' ) ) { continue }
    $f = $t -split '\s+'
    if ( $f.Count -ne 16 ) { throw "COUNTRY.DEF: sixteen fields wanted: $t" }
    $sp = { param( $x ) if ( $x -eq 'sp' ) { ' ' } else { $x } }
    $order = @{ 'mdy' = 0; 'dmy' = 1; 'ymd' = 2 }[$f[3]]
    if ( $null -eq $order ) { throw "COUNTRY.DEF: date order: $t" }
    if ( -not $sorts.ContainsKey( $f[15] ) ) { throw "COUNTRY.DEF: sort: $t" }
    $countries += @{
        code = [int]$f[0]; name = $f[1]; cps = @( $f[2] -split ',' | ForEach-Object { [int]$_ } )
        date = $order; dsep = ( & $sp $f[4] ); t24 = [int]( $f[5] -eq '24' ); tsep = ( & $sp $f[6] )
        thou = ( & $sp $f[7] ); dec = ( & $sp $f[8] ); list = ( & $sp $f[9] )
        money = @( $f[10] -split '\|' ); mfmt = [int]$f[11]; mdig = [int]$f[12]
        yes = $f[13]; no = $f[14]; sort = $f[15]
    }
}

$img = New-Image
Add-B $img ( @( 0xFF ) + [int[]][char[]]'COUNTRY' + @( 0, 0, 0, 0, 0, 0, 0, 0 ) )
Add-W $img 1
Add-B $img 1
Add-D $img 0x17
$nent = 0
foreach ( $c in $countries ) { $nent += $c.cps.Count }
Add-W $img $nent
$enttab = $img.Count
for ( $i = 0; $i -lt $nent; $i++ ) { Add-B $img ( New-Object int[] 14 ) }

$blocks = @{}                           # a table already written -> where
function Add-Block( [string]$tag, [int[]]$data ) {
    $key = $tag + ':' + ( $data -join ',' )
    if ( $blocks.ContainsKey( $key ) ) { return $blocks[$key] }
    $at = $img.Count
    Add-B $img ( @( 0xFF ) + [int[]][char[]]( $tag.PadRight( 7 ) ) )
    Add-W $img $data.Count
    Add-B $img $data
    $blocks[$key] = $at
    return $at
}
$fchar = @( 1, 0, 0xFF, 0, 0, 0x20, 2, 14 ) + [int[]][char[]]'."/\[]:|<>+=;,'
$e = 0
foreach ( $c in $countries ) {
    foreach ( $id in $c.cps ) {
        $cp = Get-CodePage $id
        $info = New-Image
        Add-W $info $c.code
        Add-W $info $id
        Add-W $info $c.date
        $cur = $null
        foreach ( $m in $c.money ) { $cur = ConvertTo-Bytes $cp $m; if ( $null -ne $cur ) { break } }
        if ( $null -eq $cur -or $cur.Count -gt 4 ) { throw "country $($c.code), page ${id}: no currency symbol fits" }
        Add-B $info ( $cur + ( New-Object int[] ( 5 - $cur.Count ) ) )
        foreach ( $s in @( $c.thou, $c.dec, $c.dsep, $c.tsep ) ) { Add-B $info @( [int][char]$s, 0 ) }
        Add-B $info @( $c.mfmt, $c.mdig, $c.t24 )
        Add-D $info 0                                   # the case map call: the kernel's
        Add-B $info @( [int][char]$c.list, 0 )
        Add-B $info ( New-Object int[] 10 )
        if ( $info.Count -ne 38 ) { throw 'the country block is not 38 bytes' }

        $up = for ( $b = 0x80; $b -lt 0x100; $b++ ) { Get-Upper $cp $b }
        $wt = for ( $b = 0; $b -lt 0x100; $b++ ) { Get-Weight $cp $b $sorts[$c.sort] }
        $yn = @()
        foreach ( $l in @( $c.yes, $c.no ) ) {
            $x = ConvertTo-Bytes $cp $l
            if ( $null -eq $x -or $x.Count -ne 1 ) { $x = @( [int][char]$( if ( $l -eq $c.yes ) { 'Y' } else { 'N' } ) ) }
            $yn += @( $x[0], 0 )
        }
        $subs = @(
            @( 1, ( Add-Block 'CTYINFO' ( [int[]]$info.ToArray() ) ) ),
            @( 2, ( Add-Block 'UCASE' $up ) ),
            @( 4, ( Add-Block 'FUCASE' $up ) ),
            @( 5, ( Add-Block 'FCHAR' $fchar ) ),
            @( 6, ( Add-Block 'COLLATE' $wt ) ),
            @( 7, ( Add-Block 'DBCS' @() ) ),
            @( 35, ( Add-Block 'YESNO' $yn ) )
        )
        $subtab = $img.Count
        Add-W $img $subs.Count
        foreach ( $s in $subs ) { Add-W $img 6; Add-W $img $s[0]; Add-D $img $s[1] }
        $at = $enttab + 14 * $e
        Set-W $img $at 12
        Set-W $img ( $at + 2 ) $c.code
        Set-W $img ( $at + 4 ) $id
        Set-D $img ( $at + 10 ) $subtab
        $e++
    }
}
Add-W $img 0                            # so that the empty DBCS table is followed by its end
Write-Asm $img ( Join-Path $Dir 'COUNTRY.ASM' ) 'COUNTRY.ASM - COUNTRY.SYS: what COUNTRY= in CONFIG.SYS reads' 'COUNTRY.DEF'
"COUNTRY.SYS   {0,6} bytes, {1} countries, {2} entries" -f $img.Count, $countries.Count, $nent

#=====================================================================
# KEYBOARD.SYS
#=====================================================================
$rowscan = @{
    'E' = @( 0x29, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 )
    'D' = @( 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B )
    'C' = @( 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2B )
    'B' = @( 0x56, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35 )
}
$rowfirst = @{ 'E' = 0; 'D' = 1; 'C' = 1; 'B' = 0 }     # E00 and B00 are keys; D and C start at 01
$levelno = @{ '' = 0; '+' = 1; '@' = 2; '@+' = 3 }
# a dead key's own character -> what it puts on a letter, and what to
# print for it alone in a page that has no such character
$accents = @{
    0x5E  = @( @( 0x302 ), '^' );  0xB4  = @( @( 0x301 ), "'" ); 0x60  = @( @( 0x300 ), '`' )
    0xA8  = @( @( 0x308 ), '"' );  0x7E  = @( @( 0x303 ), '~' ); 0xB8  = @( @( 0x327 ), ',' )
    0x2C7 = @( @( 0x30C ), '' );   0x2D8 = @( @( 0x306 ), '' );  0xB0  = @( @( 0x30A ), '' )
    0x2DB = @( @( 0x328 ), '' );   0x2D9 = @( @( 0x307 ), '' );  0x2DD = @( @( 0x30B ), '"' )
    0x384 = @( @( 0x301 ), "'" );  0x385 = @( @( 0x308, 0x301 ), '' )
    0xB7  = @( @( 0x307 ), '' );   0xAF  = @( @( 0x304 ), '' );  0x201A = @( @( 0x327 ), ',' )
}
function Get-Pos( [string]$p ) {        # 'E12+' -> scan code, level
    if ( $p -notmatch '^([EDCB])(\d\d)(@?\+?)$' ) { throw "KEYBOARD.DEF: position $p" }
    $i = [int]$Matches[2] - $rowfirst[$Matches[1]]
    $r = $rowscan[$Matches[1]]
    if ( $i -lt 0 -or $i -ge $r.Count ) { throw "KEYBOARD.DEF: position $p" }
    return @( $r[$i], $levelno[$Matches[3]] )
}

$layouts = @()
$cur = $null
foreach ( $line in ( Read-Def ( Join-Path $Dir 'KEYBOARD.DEF' ) ) ) {
    $t = $line.Trim()
    if ( $t -eq '' -or $t.StartsWith( ';' ) ) { continue }
    if ( $t -match '^\[(\w\w) (\d+)\]\s*(.*)$' ) {
        $cur = @{ code = $Matches[1].ToUpper(); id = [int]$Matches[2]; name = $Matches[3]; cps = @()
                  keys = @{}; dead = @{}; caps = @{}; dec = '.' }
        $layouts += $cur
        continue
    }
    if ( $null -eq $cur ) { throw "KEYBOARD.DEF: a line before any layout: $t" }
    $f = $t -split '\s+'
    switch -Regex ( $f[0] ) {
        '^cp$'   { $cur.cps = @( $f[1..( $f.Count - 1 )] | ForEach-Object { [int]$_ } ); break }
        '^dec$'  { $cur.dec = $f[1]; break }
        '^caps$' { foreach ( $p in $f[1..( $f.Count - 1 )] ) { if ( $p ) { $cur.caps[( Get-Pos $p )[0]] = 1 } }; break }
        '^dead$' { foreach ( $p in $f[1..( $f.Count - 1 )] ) { $q = Get-Pos $p; $cur.dead["$($q[0]):$($q[1])"] = 1 }; break }
        '^([EDCB])(@?\+?)$' {
            $r = $rowscan[$Matches[1]]
            $lv = $levelno[$Matches[2]]
            if ( $f[1].Length -ne $r.Count ) { throw "KEYBOARD.DEF: [$($cur.code)] row $($f[0]) wants $($r.Count) keys: $t" }
            for ( $i = 0; $i -lt $r.Count; $i++ ) {
                if ( -not $cur.keys.ContainsKey( $r[$i] ) ) { $cur.keys[$r[$i]] = @( 0, 0, 0, 0 ) }
                $u = [int][char]$f[1][$i]
                if ( $u -ne 0x2205 ) { $cur.keys[$r[$i]][$lv] = $u }
            }
            break
        }
        default  { throw "KEYBOARD.DEF: $t" }
    }
}

$kimg = New-Image
Add-B $kimg ( [int[]][char[]]'PMKEYB' + @( 0x1A, 1 ) )
$nrec = 0
foreach ( $l in $layouts ) { $nrec += $l.cps.Count }
Add-W $kimg $nrec
$ktab = $kimg.Count
for ( $i = 0; $i -lt $nrec; $i++ ) { Add-B $kimg ( New-Object int[] 12 ) }
$r = 0
$biggest = 0
foreach ( $l in $layouts ) {
    foreach ( $id in $l.cps ) {
        $cp = Get-CodePage $id
        $missing = @{}
        $deads = @()                    # the accents this layout has, in the order met
        $entries = @()
        $typed = @{}                    # every character a key gives, for the dead keys' pairs
        $altgr = 0
        foreach ( $sc in ( $l.keys.Keys | Sort-Object ) ) {
            $k = $l.keys[$sc]
            $flags = 0
            if ( $l.caps.ContainsKey( $sc ) ) { $flags = 1 }
            $vals = @( 0, 0, 0, 0 )
            for ( $lv = 0; $lv -lt 4; $lv++ ) {
                $u = $k[$lv]
                if ( $u -eq 0 ) { continue }
                if ( $l.dead.ContainsKey( "${sc}:$lv" ) -and -not $accents.ContainsKey( $u ) ) {
                    Write-Warning "[$($l.code) $($l.id)] no accent known for the dead key U+$( '{0:X4}' -f $u ): an ordinary key"
                }
                if ( $l.dead.ContainsKey( "${sc}:$lv" ) -and $accents.ContainsKey( $u ) ) {
                    $n = [Array]::IndexOf( $deads, $u )
                    if ( $n -lt 0 ) { $deads += $u; $n = $deads.Count - 1 }
                    $vals[$lv] = $n
                    $flags = $flags -bor ( 0x10 -shl $lv )
                    if ( $lv -ge 2 ) { $altgr = 1 }
                    continue
                }
                $b = 0
                if ( $u -lt 0x80 ) { $b = $u } elseif ( $cp.rev.ContainsKey( $u ) ) { $b = $cp.rev[$u] } else { $missing[$u] = 1 }
                $vals[$lv] = $b
                if ( $b -ne 0 ) { $typed[$u] = $b; if ( $lv -ge 2 ) { $altgr = 1 } }
            }
            $entries += , ( @( $sc, $flags ) + $vals )
        }
        $deadblk = @()
        foreach ( $d in $deads ) {
            $alone = 0
            if ( $d -lt 0x80 ) { $alone = $d } elseif ( $cp.rev.ContainsKey( $d ) ) { $alone = $cp.rev[$d] }
            elseif ( $accents[$d][1] -ne '' ) { $alone = [int][char]$accents[$d][1] }
            $pairs = @()
            foreach ( $u in ( $typed.Keys | Sort-Object ) ) {
                if ( -not [char]::IsLetter( [char]$u ) ) { continue }
                $s = [string][char]$u
                foreach ( $m in $accents[$d][0] ) { $s += [char]$m }
                $s = $s.Normalize( [System.Text.NormalizationForm]::FormC )
                if ( $s.Length -eq 1 -and $cp.rev.ContainsKey( [int][char]$s[0] ) -and [int][char]$s[0] -ne $u ) {
                    $pairs += @( $typed[$u], $cp.rev[[int][char]$s[0]] )
                }
            }
            $deadblk += @( $alone, ( $pairs.Count / 2 ) ) + $pairs
        }
        $dec = ConvertTo-Bytes $cp $l.dec
        $rec = New-Image
        Add-W $rec 0x4C4B                               # 'KL'
        Add-B $rec ( [int[]][char[]]$l.code )
        Add-W $rec $l.id
        Add-W $rec $id
        Add-B $rec @( $entries.Count, $deads.Count, $dec[0], $altgr )
        foreach ( $en in $entries ) { Add-B $rec $en }
        Add-B $rec $deadblk
        $at = $ktab + 12 * $r
        Add-B $kimg @()
        $kimg[$at] = [byte][char]$l.code[0]
        $kimg[$at + 1] = [byte][char]$l.code[1]
        Set-W $kimg ( $at + 2 ) $l.id
        Set-W $kimg ( $at + 4 ) $id
        Set-W $kimg ( $at + 6 ) $rec.Count
        Set-D $kimg ( $at + 8 ) $kimg.Count
        $kimg.AddRange( $rec )
        if ( $rec.Count -gt $biggest ) { $biggest = $rec.Count }
        if ( $Report ) {
            $m = ( $missing.Keys | Sort-Object | ForEach-Object { [char]$_ } ) -join ''
            "  {0} {1,3} page {2,3}: {3,3} bytes, {4} dead, cannot show: {5}" -f $l.code, $l.id, $id, $rec.Count, $deads.Count, $m
        }
        $r++
    }
}
Write-Asm $kimg ( Join-Path $Dir 'KEYBOARD.ASM' ) 'KEYBOARD.ASM - KEYBOARD.SYS: the layouts KEYB loads' 'KEYBOARD.DEF'
"KEYBOARD.SYS  {0,6} bytes, {1} layouts, {2} records, the longest {3} bytes" -f $kimg.Count, $layouts.Count, $nrec, $biggest
