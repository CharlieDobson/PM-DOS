# MKISO.PS1 - build S:\Software\lfntest.iso, the disc the ISO9660 long-
# name path was written against.
#
# WHY A DISC HAD TO BE MADE.  The three discs already here - offnt42,
# win16ddk, windiag - are all pre-Windows-95 and every name on them
# already fits 8.3, so they prove Joliet is being FOUND and prove
# nothing at all about what happens to a name that does not fit.  This
# one is built to contain exactly the cases the code has branches for:
#
#   README.TXT             an exact 8.3 name, which must get no long
#                          name at all
#   Read Me First.txt      spaces, which are DROPPED and not
#   Read Me Second.txt     substituted - and two of them, so the ~N
#                          aliases have to differ from each other
#   note.txt.backup.txt    two dots: the LAST is the extension
#   lower.txt              case only, which must NOT get a ~N alias
#   Setup Files\           a long directory name, walked into by both
#                          its alias and its long name
#   WIN31\                 a directory whose name is already 8.3
#
# IMAPI2FS is Windows' own image builder and is on every Windows since
# Vista; FileSystemsToCreate = 3 is ISO9660 + Joliet, which is what
# makes the disc carry BOTH trees - the point of the exercise, since
# the reader has to choose one.
#
# TOOLS\isodump.pl (or any host-side reader) is the oracle: it lists
# both trees, and what PM-DOS shows has to be the Joliet one.
param([string]$Out = 'S:\Software\lfntest.iso',
      [string]$Work = "$env:TEMP\pmdos-isosrc")

Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
public class IsoWriter {
  public static void Create(string path, object stream, int blockSize, int totalBlocks) {
    IStream i = (IStream)stream;
    byte[] buf = new byte[blockSize];
    IntPtr pcb = Marshal.AllocHGlobal(4);
    using (FileStream o = File.OpenWrite(path)) {
      while (totalBlocks-- > 0) {
        i.Read(buf, blockSize, pcb);
        o.Write(buf, 0, Marshal.ReadInt32(pcb));
      }
      o.Flush();
    }
    Marshal.FreeHGlobal(pcb);
  }
}
'@

Remove-Item -Recurse -Force $Work -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Work | Out-Null
Set-Content -NoNewline -Path "$Work\README.TXT" -Value 'A plain 8.3 name, which needs no long name at all.'
Set-Content -NoNewline -Path "$Work\Read Me First.txt" -Value 'Spaces in the base: the alias drops them rather than substituting.'
Set-Content -NoNewline -Path "$Work\Read Me Second.txt" -Value 'The second one, whose alias must not collide with the first.'
Set-Content -NoNewline -Path "$Work\note.txt.backup.txt" -Value 'Two dots: the LAST one is the extension.'
Set-Content -NoNewline -Path "$Work\lower.txt" -Value 'Lower case only - the alias is the same name, so no tilde.'
New-Item -ItemType Directory -Force "$Work\Setup Files" | Out-Null
Set-Content -NoNewline -Path "$Work\Setup Files\config.ini" -Value '[test]'
Set-Content -NoNewline -Path "$Work\Setup Files\A Rather Long Name Indeed.doc" -Value 'deep'
New-Item -ItemType Directory -Force "$Work\WIN31" | Out-Null
Set-Content -NoNewline -Path "$Work\WIN31\SETUP.EXE" -Value 'not really'

Remove-Item $Out -ErrorAction SilentlyContinue
$fsi = New-Object -ComObject IMAPI2FS.MsftFileSystemImage
$fsi.FileSystemsToCreate = 3            # ISO9660 + Joliet
$fsi.VolumeName = 'LFNTEST'
$fsi.Root.AddTree($Work, $false)
$r = $fsi.CreateResultImage()
[IsoWriter]::Create($Out, $r.ImageStream, $r.BlockSize, $r.TotalBlocks)
Get-Item $Out | Select-Object FullName, Length
