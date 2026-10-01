[CmdletBinding(PositionalBinding=$false)]
param(
    [Parameter(Mandatory=$true)][string]$Machine,    # 86Box machine name: p3bf, cuv4xls, vs440fx...
    [string]$Cpu = 'pentium2_deschutes',
    [string]$Speed = '450000000',
    [string]$Multi = '4.5',
    [string]$Video = 'cl_gd5436_pci',
    [int]$SoftFloat = 1,                             # 86Box's exact coprocessor: the one that
                                                     # reports an unmasked error
    [int]$Dynarec = 1,
    [string]$Kernel = '',                            # where PMIO.SYS and PMDOS.SYS are, if not BIN\
    [string[]]$Cfg = @( 'DOS=HIGH,UMB' ),            # CONFIG.SYS
    [string[]]$Run = @( 'FPUTEST', 'FPU32', 'DPMIFPU', 'DPMIFP32', 'FPU32 /F', 'FPUTEST /S', 'FPUTEST' ),
    [string]$Work = "$env:TEMP\fpu86",               # where the scratch VMs go
    [int]$Seconds = 150,                             # give up on the boot after this long
    [string]$Shot = ''                               # a .png of the screen as it was at the end
)
# FPU86.PS1 - run the coprocessor tests on a machine whose coprocessor
# reports its errors, and print what they said.
#
#   powershell -File TOOLS\FPU86.PS1 -Machine p3bf
#   powershell -File TOOLS\FPU86.PS1 -Machine p3bf -Kernel C:\other\kernel
#
# WHY IT EXISTS.  DOSBox-X's coprocessor never raises an error: a
# program that unmasks divide-by-zero and divides is simply handed
# infinity, under its own DOS as much as under PM-DOS.  So everything in
# IO\FPU.INC that is about the error arriving - exception 16, the
# interrupt a DOS box is given for it, a DPMI client's handler, a native
# program being ended - runs there without ever being asked to do
# anything.  86Box with fpu_softfloat = 1 raises it.
#
# What DOSBox-X has and this has not is SSE: 86Box's newest processor is
# a Pentium II, which has FXSAVE and no XMM registers.  The two between
# them cover the file; neither does alone.
#
# The last two in the default list are the ones that end a program on
# purpose, and the FPUTEST after them is there to show the machine came
# through it: a DOS box and a prompt that still work.
#
# BUILD fpuirq is the kernel that takes the error from interrupt line
# 13, as a 386 has to.  Run this against it as well: it is the only way
# that path is ever executed.
#
# The scratch VM is BUS86.PS1's without the network card.  It puts a
# window on the screen for half a minute, and will not start if an 86Box
# is already running.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$perl = 'C:\Program Files\Git\usr\bin\perl.exe'
$vm = Join-Path $Work $Machine
$image = Join-Path $vm 'T.IMG'
$stage = Join-Path $vm 'stage'
if ( $Kernel -eq '' ) { $Kernel = Join-Path $root 'BIN' }

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class Fpu86 {
 [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags); }
"@

# The screen, for a guest that has stopped: what it printed before it
# did is on the screen and nowhere else, because a redirected program's
# output reaches the disk when the program ends.
function Save-Screen( $proc ) {
    $proc.Refresh()
    $rect = New-Object Fpu86+RECT
    if ( -not [Fpu86]::GetWindowRect( $proc.MainWindowHandle, [ref]$rect ) ) { return }
    $wide = $rect.R - $rect.L
    $high = $rect.B - $rect.T
    if ( $wide -le 0 -or $high -le 0 ) { return }
    $bitmap = New-Object System.Drawing.Bitmap $wide, $high
    $graphics = [System.Drawing.Graphics]::FromImage( $bitmap )
    $dc = $graphics.GetHdc()
    [Fpu86]::PrintWindow( $proc.MainWindowHandle, $dc, 2 ) | Out-Null
    $graphics.ReleaseHdc( $dc )
    $bitmap.Save( $Shot, [System.Drawing.Imaging.ImageFormat]::Png )
    $graphics.Dispose()
    $bitmap.Dispose()
}

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
        'fpu_type = internal', "fpu_softfloat = $SoftFloat", "cpu_use_dynarec = $Dynarec", 'time_sync = local',
        'mem_size = 65536', '',
        '[Video]', "gfxcard = $Video", '',
        '[Input devices]', 'mouse_type = ps2', '',
        '[Sound]', 'sndcard = none', '',
        '[Ports (COM & LPT)]', 'serial1_enabled = 1', 'lpt1_enabled = 0', '',
        '[Storage controllers]', 'hdc = internal', '',
        '[Hard disks]', 'hdd_01_parameters = 63, 16, 65, 0, ide', 'hdd_01_fn = T.IMG',
        'hdd_01_speed = 2000_7200rpm', 'hdd_01_ide_channel = 0:0', '',
        '[Floppy and CD-ROM drives]', 'fdd_01_type = 35_2hd', 'fdd_02_type = none'
    )
    Set-Content -Path "$vm\86box.cfg" -Value $lines -Encoding ascii
}

function New-Disk {
    $crlf = "`r`n"
    [IO.File]::WriteAllText( "$stage\CONFIG.SYS", ( ( $Cfg -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    $auto = @( '@ECHO OFF', "PATH C:\TESTS;C:\", "ECHO === the coprocessor tests === > C:\FPU.TXT" )
    foreach ( $test in $Run ) {
        $auto += "ECHO === $test === >> C:\FPU.TXT", "$test >> C:\FPU.TXT"
    }
    $auto += "ECHO DONE > C:\DONE.TXT"
    [IO.File]::WriteAllText( "$stage\AUTOEXEC.BAT", ( ( $auto -join $crlf ) + $crlf ), [Text.Encoding]::ASCII )
    $files = @( "$Kernel\PMIO.SYS", "$Kernel\PMDOS.SYS", 'BIN\CMD.EXE', 'BIN\COMMAND.COM', 'BIN\MEM.EXE',
                "$stage\CONFIG.SYS", "$stage\AUTOEXEC.BAT", '-dir', 'TESTS', 'BIN\TESTS\FPUTEST.COM',
                'BIN\TESTS\FPU32.EXE', 'BIN\TESTS\DPMIFPU.COM', 'BIN\TESTS\DPMIFP32.COM',
                'BIN\TESTS\HELLO.COM' )
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
        if ( $Shot -ne '' ) { Save-Screen $proc }
        $proc.Refresh()
        [Fpu86]::PostMessage( $proc.MainWindowHandle, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero ) | Out-Null
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
$said = & $perl "$root\TOOLS\FATGET.PL" $image FPU.TXT
Set-Content -Path "$vm\fpu.txt" -Value $said
$said
