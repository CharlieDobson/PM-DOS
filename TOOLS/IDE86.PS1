[CmdletBinding(PositionalBinding=$false)]
param(
    [Parameter(Mandatory=$true)][string]$Machine,    # 86Box machine name: cuv4xls, p3bf, vs440fx, p5a...
    [Parameter(Mandatory=$true, Position=0, ValueFromRemainingArguments=$true)]
    [string[]]$Cases,                                # one boot each: CONFIG.SYS lines joined by ';'
    [string]$Cpu = 'pentium2_deschutes',
    [string]$Speed = '450000000',
    [string]$Multi = '4.5',
    [string]$Cd = '',                                # an ISO for the secondary master
    [string]$Work = "$env:TEMP\ide86",               # where the scratch VMs go
    [int]$Seconds = 120                              # give up on a boot after this long
)
# IDE86.PS1 - run IDETEST on a real IDE controller, once for each
# CONFIG.SYS given, and print what it said.
#
#   powershell -File TOOLS\IDE86.PS1 -Machine cuv4xls -Cpu celeron_mendocino
#              -Speed 433333333 -Multi 6.5 "IDEDMA=ON" "IDECABLE=40;IDEDMA=ON"
#
# The cases are whatever is left on the line, one argument each.
#
# WHY IT EXISTS.  How a disk's data moves is decided from the host
# adapter, the BIOS and the cable, and DOSBox-X has none of the three:
# its disk is PIO and nothing else.  86Box has real chipsets, and a
# different board is one word in a config file.  Each case is a
# scratch VM in $Work with a 32MB disk built for it, whose
# AUTOEXEC.BAT runs IDETEST into a file; the image is read while the
# machine is still up, and the window is CLOSED, not killed - a killed
# 86Box loses what it was writing.
#
# The boards the transfer modes were tested on:
#   cuv4xls   VIA 686B, Ultra DMA 5     -Cpu celeron_mendocino -Speed 433333333 -Multi 6.5
#   p3bf      Intel PIIX4E, Ultra DMA 2 (the defaults)
#   vs440fx   Intel PIIX3, no Ultra DMA -Cpu pentiumpro -Speed 166666666 -Multi 2.5
#   p5a       ALi, which PM-DOS does not time
#                                       -Cpu pentium_p55c -Speed 233333333 -Multi 3.5
#
# THE CHECKSUM IDETEST PRINTS IS OF THIS DISK, and every case builds
# the same one but for CONFIG.SYS, so the number is the same for every
# case on every board - 98461D56 when this was written.  It covers the
# FAT, so it is a fresh disk's number: a second boot of the same image
# has IDE.TXT on it and prints another.  It moves too when a file on
# the disk changes size across a cluster.
#
# It puts a window on the screen for ten or twenty seconds a case, and
# will not start if an 86Box is already running.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$perl = 'C:\Program Files\Git\usr\bin\perl.exe'
$vm = Join-Path $Work $Machine
$image = Join-Path $vm 'T.IMG'
$stage = Join-Path $vm 'stage'

Add-Type @"
using System; using System.Runtime.InteropServices;
public class Ide86 {
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l); }
"@

function New-Vm {
    New-Item -ItemType Directory -Force ( Join-Path $vm 'nvr' ) | Out-Null
    New-Item -ItemType Directory -Force $stage | Out-Null
    # a machine the PM-DOS VM has been before has a CMOS worth having
    foreach ( $ext in 'nvr', 'bin' ) {
        $saved = "C:\86Box\VMs\PM-DOS\nvr\$Machine.$ext"
        if ( ( Test-Path $saved ) -and -not ( Test-Path "$vm\nvr\$Machine.$ext" ) ) { Copy-Item $saved "$vm\nvr\" }
    }
    $cfg = @(
        '[General]', 'confirm_exit = 0', 'confirm_reset = 0', 'confirm_save = 0', 'vid_renderer = qt_software', '',
        '[Machine]', "machine = $Machine", "cpu_family = $Cpu", "cpu_speed = $Speed", "cpu_multi = $Multi",
        'fpu_type = internal', 'cpu_use_dynarec = 1', 'time_sync = local', 'mem_size = 65536', '',
        '[Video]', 'gfxcard = cl_gd5436_pci', '',
        '[Input devices]', 'mouse_type = ps2', '',
        '[Sound]', 'sndcard = none', '',
        '[Storage controllers]', 'hdc = internal', '',
        '[Hard disks]', 'hdd_01_parameters = 63, 16, 65, 0, ide', 'hdd_01_fn = T.IMG',
        'hdd_01_speed = 2000_7200rpm', 'hdd_01_ide_channel = 0:0', '',
        '[Floppy and CD-ROM drives]', 'fdd_01_type = 35_2hd', 'fdd_02_type = none'
    )
    if ( $Cd -ne '' ) {
        $cfg += 'cdrom_01_speed = 32', 'cdrom_01_parameters = 1, atapi', 'cdrom_01_ide_channel = 1:0',
                "cdrom_01_image_path = $Cd", 'cdrom_01_type = 86BOX_CD-ROM_1.00'
    }
    Set-Content -Path "$vm\86box.cfg" -Value $cfg -Encoding ascii
}

function New-Disk( [string[]]$lines ) {
    $crlf = "`r`n"
    [IO.File]::WriteAllText( "$stage\CONFIG.SYS", ( ( $lines -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    $auto = '@ECHO OFF', 'C:\TESTS\IDETEST.EXE > C:\IDE.TXT', 'ECHO DONE > C:\DONE.TXT'
    [IO.File]::WriteAllText( "$stage\AUTOEXEC.BAT", ( ( $auto -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    Push-Location $root
    try {
        & TOOLS\MKDISK.EXE -hd16 32 $image -mbr BIN\MBRBOOT.BIN -vbr BIN\BOOT16.BIN -s2 `
            BIN\BOOT2.BIN BIN\PMIO.SYS BIN\PMDOS.SYS BIN\CMD.EXE BIN\COMMAND.COM BIN\MEM.EXE `
            "$stage\CONFIG.SYS" "$stage\AUTOEXEC.BAT" -dir TESTS BIN\TESTS\IDETEST.EXE `
            BIN\TESTS\APITEST.COM | Out-Null
        if ( $LASTEXITCODE -ne 0 ) { throw "MKDISK failed: $LASTEXITCODE" }
    } finally {
        Pop-Location
    }
}

function Start-Guest {
    $proc = Start-Process -FilePath 'C:\86Box\86Box.exe' -ArgumentList '-P', "`"$vm`"" -WorkingDirectory 'C:\86Box' -PassThru
    $start = Get-Date
    $done = $false
    while ( ( ( Get-Date ) - $start ).TotalSeconds -lt $Seconds ) {
        Start-Sleep -Seconds 3
        if ( $proc.HasExited ) { break }
        $found = & $perl "$root\TOOLS\FATGET.PL" $image DONE.TXT 2>$null
        if ( $found -match 'DONE\.TXT \(' ) { $done = $true; break }
    }
    if ( -not $proc.HasExited ) {
        Start-Sleep -Seconds 2
        $proc.Refresh()
        [Ide86]::PostMessage( $proc.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero ) | Out-Null
        if ( -not $proc.WaitForExit( 15000 ) ) { $proc.Kill(); '86Box did not close - killed' }
    }
    if ( -not $done ) { "the guest never finished ($Seconds s)" }
}

if ( Get-Process 86Box -ErrorAction SilentlyContinue ) {
    'an 86Box is already running'
    exit 2
}
New-Vm
$number = 0
foreach ( $case in $Cases ) {
    $number++
    "=== $case"
    New-Disk ( $case -split ';' )
    Start-Guest
    $said = & $perl "$root\TOOLS\FATGET.PL" $image IDE.TXT
    Set-Content -Path "$vm\case$number.txt" -Value $said
    $said | Where-Object { $_ -match 'controller:|host timing|policy|disk \d|packet \d|offers|FAILED|Checksum|Result|IDETEST:|not on the volume' }
}
