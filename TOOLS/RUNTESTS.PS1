param(
    [string[]]$Run = @(),                        # tests to run; the list below if none
    [int]$MemMB = 32,                            # the machine's RAM
    [string]$Cpu = '486',                        # DOSBox-X cputype: 386, 486, pentium...
    [string]$Name = '',                          # the baseline's name; <cpu>-<MB> if not given
    [string[]]$Cfg = @( 'DOS=HIGH,UMB', 'DEVICE=C:\TESTS\IOCDEV.SYS', 'DEVICE=C:\TESTS\TESTBLK.SYS' ),
                                                 # CONFIG.SYS - IOCDEV.SYS is
                                                 # what DRVTEST talks to, and
                                                 # TESTBLK.SYS's RAM disk (D:)
                                                 # is what BLKTEST writes on
    [int]$Seconds = 240,                         # DOSBox-X's -time-limit
    [string]$Exe = '',                           # DOSBox-X, if DBXRUN.PS1 would pick wrong
    [switch]$Update,                             # keep what this run said as the baseline
    [switch]$All,                                # every row in $rows, one after another
    [switch]$Sound,                              # a Sound Blaster in the machine (DMATEST);
                                                 # the baselines are without one
    [string]$SbType = 'sb16'                     # ...and which: sb16vibra for ISAPNP
)
# RUNTESTS.PS1 - run the unattended tests on one DOSBox-X machine and
# say which of them now answer differently.
#
#   powershell -File TOOLS\RUNTESTS.PS1 -All [-Update]
#   powershell -File TOOLS\RUNTESTS.PS1 [-MemMB n] [-Cpu type] [-Name row]
#              [-Run TEST,TEST...] [-Cfg line;line...] [-Update]
#
# -All is the one to run after a change: every machine in $rows, and
# exit code 0 only if all of them matched.  With no -All it is one
# machine, a 486 with 32MB unless told otherwise.
#
# A 32MB disk is built the way MKHD.CMD builds one - the system, every
# program in BIN\ and every test in BIN\TESTS\ - but with an AUTOEXEC.BAT
# of its own, which runs each test into C:\<test>.OUT and ends with
# C:\DONE.TXT.  DBXRUN.PS1 boots it with no window, FATGET.PL reads the
# answers back, and each one is compared with BASELINE\<row>\<test>.OUT.
#
# WHY IT EXISTS.  Every test here prints what it found, and until now a
# person read it.  That is fine for one change and hopeless for a change
# to the memory model, which every test touches and which has to be
# checked on several machines after every step: a 4MB 386, a 486 with no
# 4MB pages, and Pentiums either side of 256MB and of 2GB.  The baseline
# is plain text in the repository, so a change that MEANS to move an
# answer shows the move in its own diff, and one that did not mean to is
# caught.
#
# THE TESTS RUN IN C:\WORK and their answers go to the root.  A test
# that deletes through a wildcard deletes in the current directory, and
# FCBTEST's "FCB????????" took its own FCBTEST.OUT with it while it was
# being written.  FATGET.PL reads the root, and nothing runs there.
#
# A TEST THAT NEVER FINISHED is named, not guessed: "START <test>" goes
# to the printer before each one, and DBXRUN.PS1 captures the printer
# into LPT.TXT as it happens, so when DONE.TXT is missing the last START
# says which test the time ran out in.  It is the printer and not a file
# on C: because the lazy write cache commits when a PROGRAM ends
# (CACHE.INC, "WRITE CACHING, AND WHEN IT IS OVER"), not when a batch
# line does: a marker written to the disk is still in the cache when
# the test after it hangs, and goes down with it - as does C:\WORK and
# everything else the batch wrote since the last program ended.
# IDLETEST without /T showed that, and took the whole run with it.  The
# same rule is why HELLO runs after DONE.TXT: it is the program whose
# ending puts DONE.TXT on the disk.
#
# An entry in -Run may carry arguments ("IDLETEST /T"); the first word
# names the test and its .OUT file.
#
# -Sound puts a Sound Blaster 16 in the machine (DBXRUN.PS1), which
# DMATEST drives through the DMA controller; -SbType sb16vibra makes it
# a Plug and Play one, which ISAPNP lists.  The baselines are recorded
# without one, so give such a run a -Name of its own.
#
# -Update writes what this run said as the new baseline, after masking.
# Run it twice on an unchanged build before trusting a row: anything
# that differs between two runs of the same kernel belongs in $masks.
#
# Exit code 0 when every test ran and every answer matched, 1 otherwise.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$perl = 'C:\Program Files\Git\usr\bin\perl.exe'

# The machines.  The 386 is the floor the README promises; the 486 has
# no 4MB pages; 252/256/260MB sit either side of the 64 page tables
# PMDOS.INC maps, and 1GB/3GB either side of 2GB.  Each has its own
# BASELINE\<cpu>-<MB>.
$rows = @( @( '386', 4 ), @( '486', 32 ), @( 'pentium', 252 ), @( 'pentium', 256 ),
           @( 'pentium', 260 ), @( 'pentium', 1024 ), @( 'pentium', 3072 ) )

if ( $All ) {
    $bad = 0
    foreach ( $row in $rows ) {
        $args1 = @( '-NoProfile', '-File', $PSCommandPath, '-Cpu', $row[0], '-MemMB', "$( $row[1] )", '-Seconds', "$Seconds" )
        if ( $Update ) { $args1 += '-Update' }
        $said = & powershell @args1 2>&1
        if ( $LASTEXITCODE -ne 0 ) { $bad++ }
        $notes = @( $said | Where-Object { "$_" -match 'DIFFERENT|HUNG|not reached|no output|unknown|no baseline|never written' } )
        $same = @( $said | Where-Object { "$_" -match '\s(same|recorded)$' } ).Count
        "{0,-13} {1,-4} {2,2} matched{3}" -f "$( $row[0] )-$( $row[1] )", $( if ( $LASTEXITCODE -eq 0 ) { 'ok' } else { 'FAIL' } ), $same,
            $( if ( $notes.Count -gt 0 ) { ':  ' + ( ( $notes | ForEach-Object { ( "$_" -replace '\s+', ' ' ).Trim() } ) -join '; ' ) } else { '' } )
    }
    if ( $bad -gt 0 ) { exit 1 }
    exit 0
}

# The tests that run to the end with nobody at the keyboard on a
# DOSBox-X machine, in a fixed order - TSRTEST leaves a program
# resident, so what follows it sees one.  NOT here:
#   IDETEST              a real IDE controller; IDE86.PS1 runs it on 86Box
#   CRASH                faults on purpose and restarts the shell
#   KBTEST, INT9TEST     wait for keys
#   PARKTEST, PARKWIN, PARKEXEC, WINDRV
#                        the handover, which wants its own batch files
#   TESTDEV, TESTBLK     drivers, loaded by a CONFIG.SYS that names them
# IOCDEV.SYS and TESTBLK.SYS ARE loaded by this one's CONFIG.SYS, for
# DRVTEST and BLKTEST, which park the kernel themselves and so ask both
# DOSes about them.  The RAM disk is D: on every row.
$default = @(
    'MEM', 'A20TEST', 'ALLOCTST', 'APITEST', 'ARGS', 'BIGMEM', 'BUFTEST', 'CACHETST', 'CDATEST', 'CDXTEST',
    'CFGTEST', 'CTRYDMP', 'CTRYTEST', 'CURTEST', 'DEVCRIT', 'DEVTEST', 'DMATEST', 'DPMIFP32', 'DPMIFPU',
    'DPMII32', 'DPMIIRQ', 'DPMITEST', 'DPMIV32', 'DPMIVEC', 'DRIVES', 'EMSTEST', 'EXECLEAK', 'EXECTST',
    'F32TEST', 'FCBTEST', 'FNDTEST', 'FPU32', 'FPUTEST', 'HELLO', 'HELLOX', 'HNDTEST', 'HOOKTEST',
    'HWPROBE', 'I13TEST', 'I24TEST', 'IDLETEST /T', 'IRQTEST', 'ISAPNP', 'LFNAPI', 'LOLTEST', 'LPTTEST',
    'MCBTEST', 'MEMPROBE', 'MS32TEST', 'MSTEST', 'NICIRQ', 'PCITEST', 'PEKID', 'PMD /S', 'PROBE13', 'R3TEST',
    'RMFIND', 'RMFSTEST', 'RMIVT', 'RMSPEED', 'RMTEST', 'SFTSCAN',
    # these four park the kernel too, as SFTSCAN does, so they follow it:
    # every test before them still runs on a machine that has never
    # been parked
    'BLKTEST', 'CDSTEST', 'DRVTEST', 'NETTEST', 'SEEKWR',
    'SHARETST', 'SLOTTEST', 'TIMERTST', 'TSRTEST', 'UDTEST',
    'UMBTEST', 'VBETEST', 'VDSTEST', 'VFTEST', 'VFYTEST', 'VPICTEST', 'WINTEST', 'XMSREPT', 'XMSTEST',
    # last, because it leaves a program resident: a handler on the timer
    # and the clock that IRQXWAIT, a native program, waits on
    'IRQXTEST /I', 'IRQXWAIT', 'IRQXTEST /R'
)

# Run-to-run noise: a pattern and what it becomes.  Each one was seen to
# differ between two runs of one unchanged build, and each masks only
# the NUMBER - the test's own ok/OK beside it is still compared, so an
# interrupt that stops arriving still fails the run.
$masks = @(
    # interrupts counted over one second (VPICTEST, DPMIIRQ, DPMII32, DPMIV32, DPMIVEC)
    , @( '(, 1s[ .]+)[0-9A-F]{4} [0-9A-F]{4}', '$1#### ####' )
    , @( '(, 1s[ .]+)[0-9A-F]{4}', '$1####' )
    , @( '(and after (the EOI|unmasking) \.+ )[0-9A-F]{4}', '$1####' )
    , @( '(interrupts off/on\s+)[0-9A-F]{4}/[0-9A-F]{4}', '$1####/####' )
    , @( '(leaves the flags alone\s+)[0-9A-F]{4}', '$1####' )
    , @( '(a thousand (times, in ticks|calls to the BIOS)\s+)[0-9A-F]{4}', '$1####' )
    # PCITEST: how many of a line's interrupts went to the box
    , @( 'taken \d+, to a box \d+', 'taken #, to a box #' )
    # PROBE13: the BIOS tick count either side of a call
    , @( 'tick [0-9A-F]{4}/[0-9A-F]{4}', 'tick ####/####' )
    # RMSPEED: it is a stopwatch
    , @( '\s+\d+ ticks,\s+\d+ ms each', ' # ticks, # ms each' )
    # APITEST: 5Ah makes the name from the clock
    , @( '(unique name: \\)[0-9A-F]{8}', '$1########' )
    # TIMERTST: ticks counted over two seconds, and against the RTC
    , @( '(, 2s[ .]+)[0-9A-F]{4}', '$1####' )
    , @( '(and INT 1Ch( behind it|, passed on) \.+ )[0-9A-F]{4}', '$1####' )
    , @( '(clock ticks in 2 RTC s \.+ )[0-9A-F]{4}', '$1####' )
    # RMIVT: the same count, with the kernel calling the BIOS meanwhile
    # (its IRQ 8 count is ", 1s" above)
    , @( '((2Ch|timer x4): clock in 2 RTC s \.+ )[0-9A-F]{4}', '$1####' )
    # IRQXTEST and IRQXWAIT: what happened while the clock ran
    , @( '(ticks since /I \.+ )\d+', '$1#' )
    , @( '(INT 1Ch calls, 9/10 \.+ )\d+', '$1#' )
    , @( '(IRQ 8 interrupts, 3/tick \. )\d+', '$1#' )
    , @( '(Zs typed by INT 1Ch \.+ )\d+', '$1#' )
    , @( '(and calls the clock did not ask for \.+ )\d+', '$1#' )
    , @( '(Zs while polling, 5s \.+ )\d+', '$1#' )
)

# "powershell -File" hands a list over as ONE string, so -Run is split
# on commas here; -Cfg on semicolons, as IDE86.PS1 does, because a
# CONFIG.SYS line can have a comma in it (DOS=HIGH,UMB).
$Run = @( $Run | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' } )
$Cfg = @( $Cfg | ForEach-Object { $_ -split ';' } | Where-Object { $_ -ne '' } )
if ( $Run.Count -eq 0 ) { $Run = $default }
# A test named twice answers into a second file: MEM.OUT, then MEM2.OUT.
$tests = @()
$seen = @{}
foreach ( $entry in $Run ) {
    $test = ( $entry.Trim() -split '\s+' )[0].ToUpper()
    if ( $seen.ContainsKey( $test ) ) {
        $seen[ $test ]++
        $n = "$( $seen[ $test ] )"
        $stem = if ( $test.Length + $n.Length -gt 8 ) { $test.Substring( 0, 8 - $n.Length ) } else { $test }
        $tests += "$stem$n"
    } else {
        $seen[ $test ] = 1
        $tests += $test
    }
}
if ( $Name -eq '' ) { $Name = "$Cpu-$MemMB" }
$endmark = 'RUNTESTS: END OF AUTOEXEC'
$work = Join-Path $root "IMAGES\RUNTESTS\$Name"
$stage = Join-Path $work 'stage'
$outdir = Join-Path $work 'out'
$image = Join-Path $work 'T.IMG'
$base = Join-Path $root "BASELINE\$Name"

foreach ( $dir in $work, $stage, $outdir ) { New-Item -ItemType Directory -Force $dir | Out-Null }
Get-ChildItem $stage, $outdir | Remove-Item -Force
if ( Test-Path $image ) { Remove-Item $image -Force }

function Get-Masked( [string]$text ) {
    foreach ( $mask in $masks ) { $text = $text -replace $mask[0], $mask[1] }
    return $text
}

function New-Disk {
    $crlf = "`r`n"
    [IO.File]::WriteAllText( "$stage\CONFIG.SYS", ( ( $Cfg -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    $auto = @( '@ECHO OFF', 'PATH C:\TESTS;C:\', 'MD C:\WORK', 'CD \WORK' )
    for ( $i = 0; $i -lt $Run.Count; $i++ ) {
        $auto += "ECHO START $( $tests[ $i ] ) > PRN", "$( $Run[ $i ].Trim() ) > C:\$( $tests[ $i ] ).OUT"
    }
    $auto += 'ECHO DONE > C:\DONE.TXT', 'HELLO > NUL', "ECHO $endmark > PRN"
    [IO.File]::WriteAllText( "$stage\AUTOEXEC.BAT", ( ( $auto -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )

    # The system files first and in MKHD.CMD's order; then every other
    # program, which is what a test that runs one expects to find.
    $first = @( 'PMIO.SYS', 'PMDOS.SYS', 'CMD.EXE', 'COMMAND.COM' )
    $files = @( $first | ForEach-Object { "BIN\$_" } )
    $files += Get-ChildItem "$root\BIN" -File |
        Where-Object { $_.Extension -match '^\.(EXE|COM)$' -and $first -notcontains $_.Name -and $_.Name -ne 'SYS16.COM' } |
        Sort-Object Name | ForEach-Object { "BIN\$( $_.Name )" }
    $files += 'BIN\RMDOS.SYS', "$stage\CONFIG.SYS", "$stage\AUTOEXEC.BAT", '-dir', 'TESTS'
    $files += Get-ChildItem "$root\BIN\TESTS" -File | Sort-Object Name | ForEach-Object { "BIN\TESTS\$( $_.Name )" }

    Push-Location $root
    try {
        & TOOLS\MKDISK.EXE -hd16 32 $image -mbr BIN\MBRBOOT.BIN -vbr BIN\BOOT16.BIN -s2 BIN\BOOT2.BIN @files | Out-Null
        if ( $LASTEXITCODE -ne 0 ) { throw "MKDISK failed: $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}

# FATGET.PL prints each file as "--- NAME (n bytes)" and then the file,
# or "--- NAME: not on the volume".
function Read-Results {
    $names = @( 'DONE.TXT' ) + @( $tests | ForEach-Object { "$_.OUT" } )
    # Windows PowerShell turns a native program's stderr into an error,
    # and "Stop" would make the first one fatal.
    $ErrorActionPreference = 'Continue'
    $said = & $perl "$root\TOOLS\FATGET.PL" $image @names 2>&1
    $ErrorActionPreference = 'Stop'
    $found = @{}
    $current = ''
    $body = New-Object System.Collections.Generic.List[string]
    foreach ( $line in $said ) {
        $line = "$line"
        if ( $line -match '^--- (\S+) \(\d+ bytes\)$' ) {
            if ( $current -ne '' ) { $found[ $current ] = $body -join "`n" }
            $current = $Matches[1].ToUpper()
            $body = New-Object System.Collections.Generic.List[string]
        } elseif ( $line -match '^--- (\S+): not on the volume$' ) {
            if ( $current -ne '' ) { $found[ $current ] = $body -join "`n" }
            $current = ''
        } elseif ( $current -ne '' ) {
            $body.Add( $line.TrimEnd( "`r" ) )
        }
    }
    if ( $current -ne '' ) { $found[ $current ] = $body -join "`n" }
    return $found
}

if ( -not ( Test-Path "$root\TOOLS\MKDISK.EXE" ) ) { throw 'Run BUILD.CMD first.' }
New-Disk
$dbx = & "$PSScriptRoot\DBXRUN.PS1" -Hd $image -Seconds $Seconds -MemMB $MemMB -Cpu $Cpu -Exe $Exe `
    -Lpt ( Join-Path $work 'LPT.TXT' ) -Until $endmark -Sound:$Sound -SbType $SbType
$found = Read-Results
$done = $found.ContainsKey( 'DONE.TXT' )
$stuck = -1
$lptfile = Join-Path $work 'LPT.TXT'
if ( -not $done -and ( Test-Path $lptfile ) ) {
    $starts = @( Get-Content $lptfile | Where-Object { $_ -match '^START (\S+)' } | ForEach-Object { $Matches[1].ToUpper() } )
    if ( $starts.Count -gt 0 ) {
        $at = $starts[ $starts.Count - 1 ]
        for ( $i = 0; $i -lt $tests.Count; $i++ ) { if ( $tests[ $i ] -eq $at ) { $stuck = $i } }
    }
}

$failed = $false
$report = @()
for ( $i = 0; $i -lt $tests.Count; $i++ ) {
    $test = $tests[ $i ]
    $file = "$test.OUT"
    if ( $i -eq $stuck ) {
        $status = 'HUNG'
    } elseif ( $stuck -ge 0 -and $i -gt $stuck ) {
        $status = 'not reached'
    } elseif ( -not $done -and $stuck -lt 0 ) {
        $status = 'unknown'
    } elseif ( $found.ContainsKey( $file ) ) {
        $status = 'ran'
    } else {
        $status = 'no output'
    }
    $verdict = ''
    if ( $status -eq 'ran' ) {
        # CRLF, as git hands every text file in this repository back on
        # Windows, and compared with the line ends taken out, so that a
        # baseline that has been through a checkout still matches.
        $text = ( Get-Masked $found[ $file ] ) -replace "`n", "`r`n"
        [IO.File]::WriteAllText( "$outdir\$file", $text, [Text.Encoding]::ASCII )
        $was = Join-Path $base $file
        if ( $Update ) {
            New-Item -ItemType Directory -Force $base | Out-Null
            [IO.File]::WriteAllText( $was, $text, [Text.Encoding]::ASCII )
            $verdict = 'recorded'
        } elseif ( -not ( Test-Path $was ) ) {
            $verdict = 'no baseline'
            $failed = $true
        } elseif ( ( [IO.File]::ReadAllText( $was ) -replace "`r", '' ) -ceq ( $text -replace "`r", '' ) ) {
            $verdict = 'same'
        } else {
            $verdict = 'DIFFERENT'
            $failed = $true
        }
    } else {
        $failed = $true
    }
    $report += '{0,-9} {1,-12} {2}' -f $test, $status, $verdict
}

"[$Name]  $MemMB MB, cputype=$Cpu  $( ( $dbx | Select-Object -Last 1 ) )"
$report
if ( -not $done ) { "DONE.TXT was never written: the run did not reach the end of AUTOEXEC.BAT" }
foreach ( $line in $report ) {
    if ( $line -match '^(\S+)\s+ran\s+DIFFERENT' ) {
        "----- $( $Matches[1] ): fc /n $base\$( $Matches[1] ).OUT $outdir\$( $Matches[1] ).OUT"
    }
}
if ( $failed ) { exit 1 }
exit 0
