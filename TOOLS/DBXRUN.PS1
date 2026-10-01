param(
    [string]$Hd = '',                            # hard disk image (C:), and the
                                                 # boot disk unless -Floppy is given
    [string]$Floppy = '',                        # boot from this floppy image instead
    [string]$Multi = '',                         # optional second disk (D:...)
    [int]$Seconds = 240,                         # DOSBox-X's -time-limit
    [string]$Cap = '',                           # record the screen here (emptied first)
    [string]$Lpt = '',                           # LPT1 captured to this file
    [string[]]$Log = @(),                        # DOSBox-X log types to switch on
    [switch]$E9                                  # port E9h text into the log
)
# DBXRUN.PS1 - boot PM-DOS from a hard-disk image in DOSBox-X with NO
# window, and let the time limit end the run.
#
#   powershell -File TOOLS\DBXRUN.PS1 -Hd X.IMG [-Multi M.IMG] [-Seconds n]
#              [-Cap DIR] [-Lpt FILE] [-E9] [-Log type,type...]
#   powershell -File TOOLS\DBXRUN.PS1 -Floppy A.IMG [-Hd X.IMG] ...
#
# -Floppy boots from A: with the hard disk, if there is one, as the
# machine's C: - which is how a real machine meets PM-DOS for the first
# time and was the one boot nothing here had ever made.  What the guest
# writes to A: is read back with FLOPGET.PL.
#
# WHY IT EXISTS.  86Box and DOSBox 0.74 both put a window on the screen
# of the person at the machine; DOSBox-X with the SDL dummy driver puts
# nothing anywhere, and its -time-limit ends the run CLEANLY, so every
# write to the image is on the disk afterwards (a killed emulator loses
# them).  What the guest leaves is then read back: files with
# FATGET.PL, the printer with -Lpt, the screen with -Cap and
# ZMBVSHOT.PL, and RMDOS's INT 21h trace (BUILD rme9) with -E9.
#
# -Cap records from BEFORE the boot ("config -avistart" in [autoexec]),
# which is the only way to see a graphical screen - Windows on its
# splash screen looks exactly like Windows still loading.
#
# [dos] int33=false, always: DOSBox-X's own mouse driver lives in its
# DOS, which "boot" throws away, and its vector stays behind pointing
# into memory the booted system reuses.  Windows' MOUSE.DRV calls it
# and runs into FF FF.
#
# A guest that stops with interrupts off (CLI; HLT), or floods the log,
# can make DOSBox-X miss its own time limit.  The run is then killed
# and whatever it had not yet written - log lines included - is lost.
$ErrorActionPreference = 'Stop'
$exe = 'C:\DOSBox-X\bin\x64\Release\dosbox-x.exe'

# The images are 63 sectors a track and 16 heads, as MKDISK builds
# them; the cylinder count is whatever the file's size says.
function Get-Cylinders( [string]$image ) {
    return [int][math]::Floor( (Get-Item $image).Length / ( 512 * 63 * 16 ) )
}

if ( $Hd -eq '' -and $Floppy -eq '' ) { throw 'Give -Hd, -Floppy or both.' }
$boot = if ( $Floppy -ne '' ) { $Floppy } else { $Hd }
$dir = if ( $Cap -ne '' ) { $Cap } else { Split-Path -Parent ( Resolve-Path $boot ) }
if ( $Cap -ne '' ) {
    New-Item -ItemType Directory -Force $Cap | Out-Null
    Get-ChildItem $Cap | Remove-Item -Force
}
if ( $Lpt -ne '' -and ( Test-Path $Lpt ) ) { Remove-Item $Lpt -Force }
$conf = Join-Path $dir 'dbxrun.conf'
$lines = @( '[sdl]', 'output=surface', '[dosbox]', 'memsize=32' )
if ( $Cap -ne '' ) { $lines += "captures=$Cap" }
if ( $E9 ) { $lines += 'bochs debug port e9=true' }
$lines += '[cpu]', 'core=normal', 'cycles=max', '[mixer]', 'nosound=true'
$lines += '[dos]', 'int33=false'
if ( $Log.Count -gt 0 -or $E9 ) {
    $lines += '[log]', "logfile=$dir\dosbox.log"
    foreach ( $type in $Log ) { $lines += "$type=true" }
}
if ( $Lpt -ne '' ) { $lines += '[parallel]', "parallel1=file append:$Lpt" }
$lines += '[autoexec]'
if ( $Cap -ne '' ) { $lines += 'config -avistart' }
if ( $Floppy -ne '' ) { $lines += "imgmount a: `"$Floppy`" -t floppy" }
if ( $Hd -ne '' ) { $lines += "imgmount 2 `"$Hd`" -size 512,63,16,$( Get-Cylinders $Hd ) -fs none" }
if ( $Multi -ne '' ) { $lines += "imgmount 3 `"$Multi`" -size 512,63,16,$( Get-Cylinders $Multi ) -fs none" }
$lines += if ( $Floppy -ne '' ) { 'boot -l a' } else { 'boot -l c' }
Set-Content -Path $conf -Value $lines -Encoding ascii

$env:SDL_VIDEODRIVER = 'dummy'
$proc = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru `
    -ArgumentList @( '-silent', '-nopromptfolder', '-time-limit', "$Seconds", '-conf', "`"$conf`"" )
if ( -not $proc.WaitForExit( ( $Seconds + 90 ) * 1000 ) ) {
    $proc.Kill()
    Write-Output 'DOSBox-X did not exit - killed'
}
Write-Output "DOSBox-X exit code $( $proc.ExitCode )"
if ( $Cap -ne '' ) {
    Get-ChildItem $Cap -Filter *.avi | ForEach-Object { "{0}  {1:N0}" -f $_.Name, $_.Length }
}
