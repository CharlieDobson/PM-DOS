[CmdletBinding(PositionalBinding=$false)]
param(
    [Parameter(Mandatory=$true)][string]$Machine,    # 86Box machine name: p3bf, cuv4xls, vs440fx, p5a...
    [string]$Cpu = 'pentium2_deschutes',
    [string]$Speed = '450000000',
    [string]$Multi = '4.5',
    [string]$Net = 'ne2kpci',                        # the network card, or 'none'
    [string]$Video = 'cl_gd5436_pci',
    [string[]]$Cfg = @( 'DOS=HIGH,UMB' ),            # CONFIG.SYS
    [string[]]$Run = @( 'PCITEST', 'NICIRQ', 'VPICTEST', 'VDSTEST', 'DPMIIRQ', 'DPMII32', 'DPMIVEC', 'DPMIV32' ),
    [string]$Work = "$env:TEMP\bus86",               # where the scratch VMs go
    [int]$Seconds = 150                              # give up on the boot after this long
)
# BUS86.PS1 - run the bus tests on a machine that has buses, and print
# what they said.
#
#   powershell -File TOOLS\BUS86.PS1 -Machine p3bf
#   powershell -File TOOLS\BUS86.PS1 -Machine cuv4xls -Cpu celeron_mendocino
#              -Speed 433333333 -Multi 6.5
#
# WHY IT EXISTS.  DOSBox-X has a PCI bus with two cards on it, neither
# of which interrupts, and no bridge; its interrupt lines are all
# edge-triggered.  What IO\IRQ.INC, VPIC.INC and PCI.INC are FOR is a
# card on a level-triggered line behind a bridge, and 86Box has those:
# an AGP bridge and a second bus on the 440BX boards, an 8029 network
# card on whatever line the BIOS gave it, and an edge/level register
# that says so.  NICIRQ is the test that needs all of it.
#
# The scratch VM is IDE86.PS1's with a network card in it.  It puts a
# window on the screen for half a minute, and will not start if an
# 86Box is already running.
#
# The boards IDE86.PS1 lists all work here, with the same -Cpu, -Speed
# and -Multi.
#
# NOT THE 486 BOARDS.  Their BIOS does not look for a hard disk: it is
# told the drive's type in its setup screen, a scratch machine's CMOS
# is blank, and the boot stops at "DISK BOOT FAILURE" before a line of
# PM-DOS has run.  A floppy would need no telling, and 86Box's floppy
# reads as an empty drive (NOTES.TXT, "THE 86BOX FLOPPY IS NOT OUR
# BUG").  A processor without 4MB pages is DOSBox-X with cputype=486.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$perl = 'C:\Program Files\Git\usr\bin\perl.exe'
$vm = Join-Path $Work $Machine
$image = Join-Path $vm 'T.IMG'
$stage = Join-Path $vm 'stage'

Add-Type @"
using System; using System.Runtime.InteropServices;
public class Bus86 {
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
    $lines = @(
        '[General]', 'confirm_exit = 0', 'confirm_reset = 0', 'confirm_save = 0', 'vid_renderer = qt_software', '',
        '[Machine]', "machine = $Machine", "cpu_family = $Cpu", "cpu_speed = $Speed", "cpu_multi = $Multi",
        'fpu_type = internal', 'cpu_use_dynarec = 1', 'time_sync = local', 'mem_size = 65536', '',
        '[Video]', "gfxcard = $Video", '',
        '[Input devices]', 'mouse_type = ps2', '',
        '[Sound]', 'sndcard = none', ''
    )
    if ( $Net -ne 'none' ) {
        $lines += '[Network]', 'net_01_net_type = slirp', "net_01_card = $Net", 'net_01_link = 0', ''
    }
    $lines += '[Ports (COM & LPT)]', 'serial1_enabled = 1', 'lpt1_enabled = 0', '',
              '[Storage controllers]', 'hdc = internal', ''
    $lines += '[Hard disks]', 'hdd_01_parameters = 63, 16, 65, 0, ide', 'hdd_01_fn = T.IMG',
              'hdd_01_speed = 2000_7200rpm', 'hdd_01_ide_channel = 0:0', '',
              '[Floppy and CD-ROM drives]', 'fdd_01_type = 35_2hd', 'fdd_02_type = none'
    Set-Content -Path "$vm\86box.cfg" -Value $lines -Encoding ascii
}

function New-Disk {
    $crlf = "`r`n"
    [IO.File]::WriteAllText( "$stage\CONFIG.SYS", ( ( $Cfg -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    $auto = @( '@ECHO OFF', "PATH C:\TESTS;C:\", "ECHO === the bus tests === > C:\BUS.TXT" )
    foreach ( $test in $Run ) {
        $auto += "ECHO === $test === >> C:\BUS.TXT", "$test >> C:\BUS.TXT"
    }
    $auto += "ECHO DONE > C:\DONE.TXT"
    [IO.File]::WriteAllText( "$stage\AUTOEXEC.BAT", ( ( $auto -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    $files = @( 'BIN\PMIO.SYS', 'BIN\PMDOS.SYS', 'BIN\CMD.EXE', 'BIN\COMMAND.COM', 'BIN\MEM.EXE',
                "$stage\CONFIG.SYS", "$stage\AUTOEXEC.BAT", '-dir', 'TESTS', 'BIN\TESTS\PCITEST.EXE',
                'BIN\TESTS\NICIRQ.COM', 'BIN\TESTS\VPICTEST.COM', 'BIN\TESTS\VDSTEST.COM',
                'BIN\TESTS\DPMIIRQ.COM', 'BIN\TESTS\DPMII32.COM', 'BIN\TESTS\DPMIVEC.COM',
                'BIN\TESTS\DPMIV32.COM', 'BIN\TESTS\IRQTEST.EXE', 'BIN\TESTS\APITEST.COM' )
    Push-Location $root
    try {
        & TOOLS\MKDISK.EXE -hd16 32 $image -mbr BIN\MBRBOOT.BIN -vbr BIN\BOOT16.BIN -s2 `
            BIN\BOOT2.BIN @files | Out-Null
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
        [Bus86]::PostMessage( $proc.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero ) | Out-Null
        if ( -not $proc.WaitForExit( 15000 ) ) { $proc.Kill(); '86Box did not close - killed' }
    }
    if ( -not $done ) { "the guest never finished ($Seconds s)" }
}

if ( Get-Process 86Box -ErrorAction SilentlyContinue ) {
    'an 86Box is already running'
    exit 2
}
New-Vm
New-Disk
Start-Guest
$said = & $perl "$root\TOOLS\FATGET.PL" $image BUS.TXT
Set-Content -Path "$vm\bus.txt" -Value $said
$said
