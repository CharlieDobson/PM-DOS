param(
    [string[]]$Rows = @(),                       # e.g. nls-gr; every row if none
    [int]$Seconds = 150,
    [switch]$Update                              # keep what these runs said as the baselines
)
# NLSTESTS.PS1 - the country and the keyboard layout, one boot for each.
#
#   powershell -File TOOLS\NLSTESTS.PS1 [-Rows nls-gr,nls-fr] [-Update]
#
# RUNTESTS.PS1 boots one CONFIG.SYS and so tests one country, the one
# the kernel has built in.  COUNTRY= is a CONFIG.SYS line and KEYB's
# layout stays until the machine restarts, so each country here is a
# machine of its own: RUNTESTS.PS1 with a -Name, a COUNTRY= line added
# to its CONFIG.SYS and a list of things to run.  The answers are kept
# in BASELINE\<row> like any other row's.
#
# What a row runs:
#   NLSTEST     38h, 65h and 66h, printed (TOOLS\NLSTEST.ASM)
#   KEYB xx     the layout, and KEYB alone to say which
#   KBLTEST     forty keys pressed by scan code, with INT 09h alone and
#               with a handler passing each one on (TOOLS\KBLTEST.ASM)
#   DIR, CHCP   what the interpreter makes of the country
#   SFTSCAN     parks the kernel under the real-mode DOS and comes back,
#               so that KBLTEST after it is the layout after a park
#   CDSTEST     parks too, and lists the drives the real-mode DOS has
#
# The dates DIR prints are the files' and change with every build, so
# DIR's answer is looked at and not compared: it is not in a row here.
#
# Exit code 0 when every row matched its baseline.
$ErrorActionPreference = 'Stop'
$rt = Join-Path $PSScriptRoot 'RUNTESTS.PS1'
$base = 'DOS=HIGH,UMB;DEVICE=C:\TESTS\IOCDEV.SYS;DEVICE=C:\TESTS\TESTBLK.SYS'
$all = [ordered]@{
    # the kernel's own country: every way KEYB can be asked and refused,
    # and a second Italian keyboard by its ID
    'nls-us' = @( '', 'KEYB,KEYB XX,KEYB GR;999,KEYB GR;;C:\NOSUCH.SYS,KEYB /?,KEYB GR IT,KEYB IT;;/ID:142,KEYB,KBLTEST,KEYB US,KEYB,CHCP,CHCP 850,CHCP 999,CHCP' )
    'nls-gr' = @( 'COUNTRY=049', 'NLSTEST,KEYB GR,KEYB,KBLTEST,SFTSCAN,KBLTEST,KEYB US,KEYB,CHCP 850,CHCP,NLSTEST' )
    'nls-fr' = @( 'COUNTRY=033,850', 'NLSTEST,KEYB FR,KEYB,KBLTEST' )
    'nls-uk' = @( 'COUNTRY=044', 'NLSTEST,KEYB UK,KBLTEST' )
    'nls-dk' = @( 'COUNTRY=045', 'NLSTEST,KEYB DK,KEYB,KBLTEST' )
    'nls-sp' = @( 'COUNTRY=034,,C:\COUNTRY.SYS', 'NLSTEST,KEYB SP,KBLTEST' )
    'nls-pl' = @( 'COUNTRY=048', 'NLSTEST,KEYB PL,KEYB,KBLTEST' )
    'nls-ru' = @( 'COUNTRY=007', 'NLSTEST,KEYB RU,KBLTEST' )
    # an installed system: everything but the kernel in C:\DOS, and
    # SHELL=C:\DOS\CMD.EXE /P below the COUNTRY= line (RUNTESTS.PS1,
    # -SysDir).  No file is named to COUNTRY= or to KEYB, CHCP goes
    # back to COUNTRY.SYS for another code page, and SFTSCAN and
    # CDSTEST park the kernel, which has to find RMDOS.SYS
    'nls-dir' = @( 'COUNTRY=049', 'NLSTEST,KEYB GR,KEYB,KBLTEST,SFTSCAN,KBLTEST,CHCP 850,CHCP,NLSTEST,CDSTEST', 'DOS' )
}
$Rows = @( $Rows | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' } )
if ( $Rows.Count -eq 0 ) { $Rows = @( $all.Keys ) }
$bad = 0
foreach ( $row in $Rows ) {
    if ( -not $all.Contains( $row ) ) { "${row}: no such row"; $bad++; continue }
    $cfg = $base
    if ( $all[$row][0] -ne '' ) { $cfg = "$base;$( $all[$row][0] )" }
    # a comma separates the things to run, so one inside KEYB's own
    # argument is written as a semicolon above
    $run = ( $all[$row][1] -split ',' | ForEach-Object { $_ -replace ';', ',' } )
    $a = @( '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $rt, '-Name', $row, '-Cfg', $cfg,
            '-Seconds', "$Seconds", '-Nls' )
    if ( $all[$row].Count -gt 2 ) { $a += '-SysDir', $all[$row][2] }
    $a += '-Run'
    $a += ( $run -join '|' )
    if ( $Update ) { $a += '-Update' }
    $said = & powershell @a 2>&1
    if ( $LASTEXITCODE -ne 0 ) { $bad++ }
    $notes = @( $said | Where-Object { "$_" -match 'DIFFERENT|HUNG|not reached|no output|unknown|no baseline|never written' } )
    $same = @( $said | Where-Object { "$_" -match '\s(same|recorded)$' } ).Count
    "{0,-10} {1,-4} {2,2} matched{3}" -f $row, $( if ( $LASTEXITCODE -eq 0 ) { 'ok' } else { 'FAIL' } ), $same,
        $( if ( $notes.Count -gt 0 ) { ':  ' + ( ( $notes | ForEach-Object { ( "$_" -replace '\s+', ' ' ).Trim() } ) -join '; ' ) } else { '' } )
}
if ( $bad -gt 0 ) { exit 1 }
exit 0
