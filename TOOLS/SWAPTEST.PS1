# swaptest.ps1 - the one test this harness could not do before: change
# the disc UNDERNEATH a running guest and see whether PM-DOS notices.
#
# 86Box has no external media control, so the Media menu is driven with
# the mouse: Media -> CD-ROM 1 -> Image..., then the path is typed into
# the file dialog.  The guest is waiting at a PAUSE; a scancode-level
# keybd_event releases it (SendKeys does not reach the guest - 86Box
# reads raw input and wants the window clicked into first).
param([string]$NewIso = 'S:\Software\offnt42.iso',
      [string]$Shot   = 'S:\CPP\PM-DOS\TOOLS\swap.png')
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public class SW {
 [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")] public static extern void mouse_event(uint f,uint x,uint y,uint d,IntPtr e);
 [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, IntPtr extra);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; } }
'@
function Click([int]$x,[int]$y) {
  [SW]::SetCursorPos($x,$y) | Out-Null; Start-Sleep -Milliseconds 400
  [SW]::mouse_event(0x0002,0,0,0,[IntPtr]::Zero)
  [SW]::mouse_event(0x0004,0,0,0,[IntPtr]::Zero)
  Start-Sleep -Milliseconds 800
}
function Shot([string]$path,[int]$w,[int]$h) {
  $b = New-Object System.Drawing.Bitmap $w,$h
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.CopyFromScreen(0,0,0,0,$b.Size); $g.Dispose(); $b.Save($path)
}

Get-Process 86Box -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep 2
Start-Process -FilePath "C:\86Box\86Box.exe" -ArgumentList '-P','"C:\86Box\VMs\PM-DOS"' -WorkingDirectory "C:\86Box"
Start-Sleep -Seconds 48

$p = Get-Process 86Box | Select-Object -First 1
$h = $p.MainWindowHandle
[SW]::ShowWindow($h,9) | Out-Null
[SW]::SetWindowPos($h,[IntPtr](-1),0,0,0,0,0x0041) | Out-Null
[SW]::SetForegroundWindow($h) | Out-Null
Start-Sleep 1

Click 134 37          # Media
Click 220 83          # CD-ROM 1 (ATAPI): ...
Click 437 113         # Image...
Start-Sleep 2
Click 590 678         # the File name box
[System.Windows.Forms.SendKeys]::SendWait($NewIso)
Start-Sleep 1
[System.Windows.Forms.SendKeys]::SendWait("{ENTER}")
Start-Sleep 3

Click 350 350         # into the guest, so the keyboard reaches it
[SW]::keybd_event(0x20,0x39,0,[IntPtr]::Zero); Start-Sleep -Milliseconds 80
[SW]::keybd_event(0x20,0x39,2,[IntPtr]::Zero)
Start-Sleep 8
Shot $Shot 730 500
"swapped to $NewIso, shot in $Shot"
