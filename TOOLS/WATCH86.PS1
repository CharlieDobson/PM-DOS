# Poll the 86Box screen instead of guessing a delay.
#
# 86Box run times on this host drift by a factor of two or more with
# load - POST alone has taken anywhere from 50s to 200s - so a single
# timed screenshot catches the DOSX fault message only by luck, and the
# machine resets a second or two later.  Shoot every few seconds and
# fingerprint each frame so only the interesting ones need looking at.
#
# Fingerprint: lit-pixel counts in three horizontal bands plus a
# saturated-colour count.  The Award POST screen is the one with
# colour; its summary page is colour-free but empty in the lower band.
param([int]$Start = 120, [int]$Stop = 400, [int]$Every = 3, [string]$Prefix = "w")
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; } }
"@
Get-Process 86Box | Stop-Process -Force; Start-Sleep 2
Start-Process -FilePath "C:\86Box\86Box.exe" -ArgumentList '-P','"C:\86Box\VMs\PM-DOS"' -WorkingDirectory "C:\86Box"
Start-Sleep -Seconds 5
$h=(Get-Process 86Box).MainWindowHandle
[W]::ShowWindow($h,9)|Out-Null
[W]::SetWindowPos($h,[IntPtr](-1),0,0,0,0,0x0041)|Out-Null
Start-Sleep -Seconds ($Start - 5)
$t = $Start
while ($t -le $Stop) {
  # Re-assert the window position EVERY frame.  86Box recreates or
  # moves its window across an emulated reset, and CopyFromScreen reads
  # the desktop, so one topmost call at the start is not enough - a whole
  # 210-second poll came back as 53 black frames.
  $h=(Get-Process 86Box | Select-Object -First 1).MainWindowHandle
  if (-not $h -or $h -eq [IntPtr]::Zero) { "{0,4}s (no window)" -f $t; Start-Sleep -Seconds $Every; $t += $Every; continue }
  [W]::ShowWindow($h,9)|Out-Null
  [W]::SetWindowPos($h,[IntPtr](-1),0,0,0,0,0x0041)|Out-Null
  Start-Sleep -Milliseconds 300
  $r=New-Object W+R; [W]::GetWindowRect($h,[ref]$r)|Out-Null
  if (($r.Rt-$r.L) -le 0 -or ($r.B-$r.T) -le 0) { "{0,4}s (bad rect)" -f $t; Start-Sleep -Seconds $Every; $t += $Every; continue }
  $bmp=New-Object System.Drawing.Bitmap(($r.Rt-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L,$r.T,0,0,$bmp.Size); $g.Dispose()
  $H=$bmp.Height; $Wd=$bmp.Width
  $b1=0;$b2=0;$b3=0;$col=0
  for ($y=60; $y -lt $H-30; $y+=8) {
    for ($x=4; $x -lt $Wd-4; $x+=8) {
      $p=$bmp.GetPixel($x,$y)
      $mx=[Math]::Max($p.R,[Math]::Max($p.G,$p.B)); $mn=[Math]::Min($p.R,[Math]::Min($p.G,$p.B))
      if (($mx-$mn) -gt 60) { $col++ }
      if ($mx -gt 90) { if ($y -lt $H/3) {$b1++} elseif ($y -lt 2*$H/3) {$b2++} else {$b3++} }
    }
  }
  $name = "{0}{1:d3}.png" -f $Prefix,$t
  $bmp.Save("$PSScriptRoot\$name",[System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  "{0,4}s {1}  colour={2,4} top={3,4} mid={4,4} bot={5,4}" -f $t,$name,$col,$b1,$b2,$b3
  Start-Sleep -Seconds $Every; $t += $Every
}
$p = Get-Process 86Box | Select-Object -First 1
if ($p) { [W]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
  $n=0; while ((Get-Process 86Box) -and $n -lt 20){Start-Sleep -Milliseconds 500;$n++}
  Get-Process 86Box | Stop-Process -Force }
Write-Output "closed"
