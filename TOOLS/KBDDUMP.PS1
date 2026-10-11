param(
    [Parameter(Mandatory=$true)][string]$Dll,      # e.g. C:\Windows\System32\kbdgr.dll
    [string]$Code = 'XX',                          # the two letters KEYB will take
    [int]$Id = 0,                                  # the keyboard ID (KEYB's /ID)
    [string]$Name = ''
)
# KBDDUMP.PS1 - one Windows keyboard layout DLL, printed in the form
# NLS\KEYBOARD.DEF is written in: the four rows of typing keys, one
# line per shift level, and the positions that are dead keys.
#
# A DRAFTING AID, not part of the build.  A national layout is the same
# keys under Windows as it was under DOS, so the layout Windows ships is
# where a new section of KEYBOARD.DEF starts; what Windows added since
# (the euro, mostly) is in no DOS code page and TOOLS\MKNLS.PS1 leaves
# out whatever a code page cannot show.  Nothing is loaded or switched:
# the file is read as bytes and the layout's tables are followed in it.
#
# 64-bit layout DLLs only (System32 on a 64-bit Windows).
$ErrorActionPreference = 'Stop'
$b = [System.IO.File]::ReadAllBytes( $Dll )

function U16( [int]$o ) { return [System.BitConverter]::ToUInt16( $b, $o ) }
function U32( [int]$o ) { return [System.BitConverter]::ToUInt32( $b, $o ) }
function U64( [int]$o ) { return [System.BitConverter]::ToUInt64( $b, $o ) }

$pe = [int]( U32 0x3C )
if ( ( U32 $pe ) -ne 0x4550 ) { throw "$Dll is not a PE file" }
$nsec = U16 ( $pe + 6 )
$optsz = U16 ( $pe + 20 )
$opt = $pe + 24
if ( ( U16 $opt ) -ne 0x20B ) { throw "$Dll is not a 64-bit DLL" }
$base = U64 ( $opt + 24 )
$exprva = U32 ( $opt + 112 )
$sec = $opt + $optsz
$sections = @()
for ( $i = 0; $i -lt $nsec; $i++ ) {
    $s = $sec + 40 * $i
    $sections += , @( ( U32 ( $s + 12 ) ), ( U32 ( $s + 8 ) ), ( U32 ( $s + 20 ) ), ( U32 ( $s + 16 ) ) )
}
function Off( [uint64]$rva ) {
    foreach ( $s in $sections ) {
        $len = [Math]::Max( $s[1], $s[3] )
        if ( $rva -ge $s[0] -and $rva -lt ( $s[0] + $len ) ) { return [int]( $rva - $s[0] + $s[2] ) }
    }
    throw ( 'RVA {0:X} is in no section' -f $rva )
}
function Ptr( [int]$o ) {              # a pointer stored in the file -> file offset, 0 for NULL
    $v = U64 $o
    if ( $v -eq 0 ) { return 0 }
    return ( Off ( $v - $base ) )
}

# KbdLayerDescriptor: lea rax,[rip+rel32] / ret
$e = Off $exprva
$nnames = U32 ( $e + 24 )
$funcs = Off ( U32 ( $e + 28 ) )
$names = Off ( U32 ( $e + 32 ) )
$ords = Off ( U32 ( $e + 36 ) )
$fn = 0
for ( $i = 0; $i -lt $nnames; $i++ ) {
    $no = Off ( U32 ( $names + 4 * $i ) )
    $nm = ''
    while ( $b[$no] -ne 0 ) { $nm += [char]$b[$no]; $no++ }
    if ( $nm -eq 'KbdLayerDescriptor' ) { $fn = U32 ( $funcs + 4 * ( U16 ( $ords + 2 * $i ) ) ) }
}
if ( $fn -eq 0 ) { throw 'no KbdLayerDescriptor export' }
$c = Off $fn
if ( $b[$c] -ne 0x48 -or $b[$c + 1] -ne 0x8D -or $b[$c + 2] -ne 0x05 ) { throw 'KbdLayerDescriptor is not the usual LEA' }
$tab = Off ( $fn + 7 + [System.BitConverter]::ToInt32( $b, $c + 3 ) )

$mods = Ptr $tab
$maxmod = U16 ( $mods + 8 )
$col = @{}                              # modifier bits -> column
for ( $m = 0; $m -le $maxmod; $m++ ) { $col[$m] = $b[$mods + 10 + $m] }
$vkwc = Ptr ( $tab + 8 )
$vsc = Ptr ( $tab + 0x30 )
$maxvsc = $b[$tab + 0x38]

# every virtual key's row: attributes, and a character or a dead key per column
$rows = @{}
for ( $t = $vkwc; ( U64 $t ) -ne 0; $t += 16 ) {
    $p = Ptr $t
    $nmod = $b[$t + 8]
    $size = $b[$t + 9]
    while ( $b[$p] -ne 0 ) {
        $vk = $b[$p]
        $r = @{ attr = $b[$p + 1]; ch = @(); dead = @() }
        for ( $k = 0; $k -lt $nmod; $k++ ) {
            $w = U16 ( $p + 2 + 2 * $k )
            $d = $false
            if ( $w -eq 0xF001 ) { $w = U16 ( $p + $size + 2 + 2 * $k ); $d = $true }
            elseif ( $w -ge 0xF000 ) { $w = 0 }
            $r.ch += $w
            $r.dead += $d
        }
        if ( $vk -ne 0xFF -and -not $rows.ContainsKey( [int]$vk ) ) { $rows[[int]$vk] = $r }
        $p += $size
    }
}

$keys = [ordered]@{
    E = @( 0x29, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13 )
    D = @( 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B )
    C = @( 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2B )
    B = @( 0x56, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35 )
}
$levels = [ordered]@{ '' = 0; '+' = 1; '@' = 6; '@+' = 7 }
$none = [char]0x2205
$deadpos = @()
$caps = @()
$out = @( "[$Code $Id] $Name" )
foreach ( $rk in $keys.Keys ) {
    foreach ( $lv in $levels.Keys ) {
        $bits = $levels[$lv]
        if ( -not $col.ContainsKey( $bits ) -or $col[$bits] -eq 0x0F ) { continue }
        $cn = $col[$bits]
        $line = ''
        $any = $false
        $idx = 0
        foreach ( $sc in $keys[$rk] ) {
            $ch = 0
            if ( $sc -le $maxvsc ) {
                $vk = ( U16 ( $vsc + 2 * $sc ) ) -band 0xFF
                if ( $rows.ContainsKey( [int]$vk ) ) {
                    $r = $rows[[int]$vk]
                    if ( $cn -lt $r.ch.Count ) {
                        $ch = $r.ch[$cn]
                        if ( $r.dead[$cn] ) { $deadpos += ( '{0}{1:D2}{2}' -f $rk, ( $idx + $( if ( $rk -eq 'E' -or $rk -eq 'B' ) { 0 } else { 1 } ) ), $lv ) }
                    }
                    if ( $lv -eq '' -and ( $r.attr -band 1 ) ) { $caps += ( '{0}{1:D2}' -f $rk, ( $idx + $( if ( $rk -eq 'E' -or $rk -eq 'B' ) { 0 } else { 1 } ) ) ) }
                }
            }
            if ( $ch -lt 0x20 -or $ch -eq 0x7F ) { $line += $none } else { $line += [char]$ch; $any = $true }
            $idx++
        }
        if ( $any ) { $out += ( '{0,-4}{1}' -f ( $rk + $lv ), $line ) }
    }
}
if ( $deadpos.Count ) { $out += 'dead ' + ( $deadpos -join ' ' ) }
$out += 'caps ' + ( $caps -join ' ' )
# the keypad's decimal key
$vk = 0x6E
if ( $rows.ContainsKey( $vk ) ) { $out += 'dec  ' + [char]$rows[$vk].ch[0] }
$out
