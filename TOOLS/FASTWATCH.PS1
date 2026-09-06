# Catch a frame that is only on the screen for a fraction of a second.
#
# watch86.ps1 polls once a second and spends most of that second in
# GetPixel: 5400 calls per frame.  The DOSX fault message survives about
# half a second before PM-DOS's unpark resets the machine, so a
# once-a-second poll caught it once in twelve boot loops.
#
# So: sample 24 pixels apart instead of 8 (a tenth of the calls), and
# SAVE ONLY WHAT CHANGED.  A boot loop is the same six or seven frames
# over and over; deduplicating on the fingerprint turns a three-minute
# poll into a handful of files, and the one that matters is the one
# whose fingerprint nothing else shares.
param([int]$Start = 40, [int]$Seconds = 200, [string]$Prefix = "f")
$ErrorActionPreference = 'SilentlyContinue'
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W2 { [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out R r);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a,int x,int y,int cx,int cy,uint f);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c);
 [StructLayout(LayoutKind.Sequential)] public struct R { public int L,T,Rt,B; } }
"@
Get-Process 86Box | Stop-Process -Force; Start-Sleep 2
Start-Process -FilePath "C:\86Box\86Box.exe" -ArgumentList '-P','"C:\86Box\VMs\PM-DOS"' -WorkingDirectory "C:\86Box"
Start-Sleep -Seconds $Start
$seen = @{}
$n = 0
$deadline = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $deadline) {
  $h=(Get-Process 86Box | Select-Object -First 1).MainWindowHandle
  if (-not $h -or $h -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200; continue }
  [W2]::ShowWindow($h,9)|Out-Null
  [W2]::SetWindowPos($h,[IntPtr](-1),0,0,0,0,0x0041)|Out-Null
  $r=New-Object W2+R; [W2]::GetWindowRect($h,[ref]$r)|Out-Null
  if (($r.Rt-$r.L) -le 0 -or ($r.B-$r.T) -le 0) { Start-Sleep -Milliseconds 200; continue }
  $bmp=New-Object System.Drawing.Bitmap(($r.Rt-$r.L),($r.B-$r.T))
  $g=[System.Drawing.Graphics]::FromImage($bmp); $g.CopyFromScreen($r.L,$r.T,0,0,$bmp.Size); $g.Dispose()
  $H=$bmp.Height; $Wd=$bmp.Width
  $col=0; $lit=0; $band=0
  for ($y=60; $y -lt $H-30; $y+=24) {
    for ($x=4; $x -lt $Wd-4; $x+=24) {
      $p=$bmp.GetPixel($x,$y)
      $mx=[Math]::Max($p.R,[Math]::Max($p.G,$p.B)); $mn=[Math]::Min($p.R,[Math]::Min($p.G,$p.B))
      if (($mx-$mn) -gt 60) { $col++ }
      if ($mx -gt 90) { $lit++; $band += $y }
    }
  }
  $key = "{0}/{1}/{2}" -f $col,$lit,[int]($band/16)
  if (-not $seen.ContainsKey($key)) {
    $seen[$key] = 1
    $name = "{0}{1:d3}.png" -f $Prefix,$n; $n++
    $bmp.Save("$PSScriptRoot\$name",[System.Drawing.Imaging.ImageFormat]::Png)
    "{0}  {1}  colour={2,3} lit={3,3}" -f (Get-Date -Format "HH:mm:ss"),$name,$col,$lit
  } else { $seen[$key]++ }
  $bmp.Dispose()
}
$p = Get-Process 86Box | Select-Object -First 1
if ($p) { [W2]::PostMessage($p.MainWindowHandle,0x0010,[IntPtr]::Zero,[IntPtr]::Zero)|Out-Null
  $m=0; while ((Get-Process 86Box) -and $m -lt 20){Start-Sleep -Milliseconds 500;$m++}
  Get-Process 86Box | Stop-Process -Force }
"closed - {0} distinct frames of {1} fingerprints" -f $n,$seen.Count
